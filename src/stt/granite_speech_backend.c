/**
 * @file granite_speech_backend.c
 * @brief Granite Speech (4.1-2b / 4.1-2b-plus) ASR backend via llama.cpp mtmd
 *
 * Replaces the former whisper.cpp and Vosk backends entirely (no
 * backward-compat path). Loads a Granite Speech GGUF (LLM decoder + Conformer
 * audio encoder weights) together with its companion mmproj GGUF (QFormer
 * audio projector) via llama.cpp's mtmd (multimodal) library, and transcribes
 * accumulated utterance audio to text using one of two fixed prompts,
 * selected by which variant is configured:
 *
 *   - ETHERVOX_STT_BACKEND_GRANITE_SPEECH       (BASE) - punctuated ASR/AST,
 *     used for Mode 1 (voice conversation) and Mode 4 (voice-to-text).
 *   - ETHERVOX_STT_BACKEND_GRANITE_SPEECH_PLUS  (PLUS) - Speaker-Attributed
 *     ASR (SAA), natively emitting "[Speaker N]: ..." tags. Used exclusively
 *     for Mode 2 (transcription) - replaces Whisper AND any separate
 *     diarization heuristic in one step.
 *
 * IBM's model card is explicit that an unfamiliar or tool-calling prompt is
 * simply ignored, falling back to plain transcription - so this backend only
 * ever sends the fixed ASR/SAA prompt strings below. The Governor (a
 * separate llama_model instance - see src/governor/governor.c) is the only
 * component that performs reasoning/tool-calling, always on text produced
 * here, never directly on audio.
 *
 * Each finalize() call is one-shot: the KV cache is reset in start() and the
 * whole accumulated utterance/chunk is tokenized (audio + prompt) and decoded
 * in a single pass. Mode 2's cross-chunk continuity comes from
 * config.prefix_text (Granite Speech's own incremental-decoding mechanism),
 * not from carrying the KV cache across calls.
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * Licensed under CC BY-NC-SA 4.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ethervox/stt.h"
#include "ethervox/error.h"
#include "ethervox/logging.h"
#include "ethervox/chat_template.h"

#if defined(LLAMA_CPP_AVAILABLE) && LLAMA_CPP_AVAILABLE && defined(MTMD_AVAILABLE) && MTMD_AVAILABLE

#include "llama.h"
#include "ggml.h"
#include "mtmd.h"
#include "mtmd-helper.h"

#define LOG_ERROR(...) ethervox_log(ETHERVOX_LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_WARN(...)  ethervox_log(ETHERVOX_LOG_LEVEL_WARN, __FILE__, __LINE__, __func__, __VA_ARGS__)
#define LOG_INFO(...)  ethervox_log(ETHERVOX_LOG_LEVEL_INFO, __FILE__, __LINE__, __func__, __VA_ARGS__)

// ----------------------------------------------------------------------------
// Fixed prompts, verbatim from IBM's granite-speech-4.1-2b-plus model card.
// Mode switching is entirely prompt-driven - there is no separate API flag,
// and no per-variant code path beyond "which of these two strings is used".
// ----------------------------------------------------------------------------
#define GRANITE_SPEECH_ASR_PROMPT \
  "<|audio|> can you transcribe the speech into a written format?"
#define GRANITE_SPEECH_SAA_PROMPT \
  "<|audio|> Speaker attribution: Transcribe and denote who is speaking by adding " \
  "[Speaker 1]: and [Speaker 2]: tags before speaker turns."
#define GRANITE_SPEECH_SYSTEM_PROMPT \
  "You are Granite, developed by IBM. You are a helpful AI assistant"

#define GRANITE_SPEECH_ASR_MAX_TOKENS 256
#define GRANITE_SPEECH_SAA_MAX_TOKENS 1024
#define GRANITE_SPEECH_PROMPT_BUF_SIZE 2048

static bool g_granite_llama_backend_initialized = false;

typedef struct {
  struct llama_model* model;
  struct llama_context* ctx;
  mtmd_context* mctx;
  const chat_template_t* tmpl;  // CHAT_TEMPLATE_GRANITE - shared with the Governor's format
  int audio_sample_rate;        // From mtmd_get_audio_sample_rate(), expected 16000
} granite_speech_context_t;

static void granite_speech_log_callback(enum ggml_log_level level, const char* text, void* user_data) {
  (void)user_data;
  if (!text) return;
  // Only surface warnings/errors - llama.cpp's INFO/DEBUG stream is very
  // verbose during GGUF loading and would otherwise flood the app log.
  if (level == GGML_LOG_LEVEL_ERROR) {
    LOG_ERROR("[llama.cpp] %s", text);
  } else if (level == GGML_LOG_LEVEL_WARN) {
    LOG_WARN("[llama.cpp] %s", text);
  }
}

ethervox_result_t ethervox_stt_granite_speech_init(ethervox_stt_runtime_t* runtime) {
  if (!runtime->config.model_path || runtime->config.model_path[0] == '\0') {
    LOG_ERROR("Granite Speech: model_path is required");
    return ETHERVOX_ERROR_INVALID_ARGUMENT;
  }
  if (!runtime->config.mmproj_path || runtime->config.mmproj_path[0] == '\0') {
    LOG_ERROR("Granite Speech: mmproj_path is required (audio projector GGUF - "
              "Granite Speech always ships as a (model, mmproj) pair, never one file)");
    return ETHERVOX_ERROR_INVALID_ARGUMENT;
  }

  granite_speech_context_t* gs = (granite_speech_context_t*)calloc(1, sizeof(granite_speech_context_t));
  if (!gs) {
    return ETHERVOX_ERROR_OUT_OF_MEMORY;
  }

  if (!g_granite_llama_backend_initialized) {
    ggml_log_set(granite_speech_log_callback, NULL);
    llama_log_set(granite_speech_log_callback, NULL);
    llama_backend_init();
    ggml_backend_load_all();
    if (ggml_backend_reg_count() == 0) {
      LOG_ERROR("Granite Speech: no ggml backends loaded, cannot load model");
      free(gs);
      return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    g_granite_llama_backend_initialized = true;
  }

  struct llama_model_params model_params = llama_model_default_params();
  model_params.n_gpu_layers = runtime->config.n_gpu_layers;
  model_params.use_mmap = true;
  model_params.use_mlock = false;

  gs->model = llama_model_load_from_file(runtime->config.model_path, model_params);
  if (!gs->model) {
    LOG_ERROR("Granite Speech: failed to load model from %s", runtime->config.model_path);
    free(gs);
    return ETHERVOX_ERROR_INVALID_ARGUMENT;
  }

  struct llama_context_params ctx_params = llama_context_default_params();
  // Granite Speech's own model card documents evaluation on segments up to
  // ~9 minutes. 4096 tokens comfortably covers one utterance's audio tokens
  // + prompt + generated transcript for a single one-shot call.
  ctx_params.n_ctx = 4096;
  ctx_params.n_batch = 512;
  ctx_params.n_ubatch = 512;
  ctx_params.no_perf = true;

  gs->ctx = llama_init_from_model(gs->model, ctx_params);
  if (!gs->ctx) {
    LOG_ERROR("Granite Speech: failed to create llama context");
    llama_model_free(gs->model);
    free(gs);
    return ETHERVOX_ERROR_INVALID_ARGUMENT;
  }

  struct mtmd_context_params mtmd_params = mtmd_context_params_default();
  mtmd_params.use_gpu = (runtime->config.n_gpu_layers != 0);
  mtmd_params.print_timings = false;
  mtmd_params.n_threads = ctx_params.n_threads;
  // media_marker left NULL: the mmproj GGUF's own metadata carries the
  // "<|audio|>" marker used verbatim in the prompt strings below, so mtmd
  // resolves it from the file without an override.

  gs->mctx = mtmd_init_from_file(runtime->config.mmproj_path, gs->model, mtmd_params);
  if (!gs->mctx) {
    LOG_ERROR("Granite Speech: failed to load mmproj from %s", runtime->config.mmproj_path);
    llama_free(gs->ctx);
    llama_model_free(gs->model);
    free(gs);
    return ETHERVOX_ERROR_INVALID_ARGUMENT;
  }

  if (!mtmd_support_audio(gs->mctx)) {
    LOG_ERROR("Granite Speech: loaded mmproj does not report audio support - wrong file?");
    mtmd_free(gs->mctx);
    llama_free(gs->ctx);
    llama_model_free(gs->model);
    free(gs);
    return ETHERVOX_ERROR_INVALID_ARGUMENT;
  }

  gs->audio_sample_rate = mtmd_get_audio_sample_rate(gs->mctx);
  if (gs->audio_sample_rate > 0 && (uint32_t)gs->audio_sample_rate != runtime->config.sample_rate) {
    LOG_WARN("Granite Speech: model expects %d Hz audio, config requests %u Hz - "
             "audio capture must resample to match", gs->audio_sample_rate, runtime->config.sample_rate);
  }

  // Granite Speech shares the same Granite 4.1 chat template family as the
  // Governor (see src/governor/chat_template.c) - reuse it directly instead
  // of re-declaring the "<|start_of_role|>...<|end_of_role|>" wrapper here.
  gs->tmpl = chat_template_get(CHAT_TEMPLATE_GRANITE, NULL);

  runtime->backend_context = gs;
  LOG_INFO("Granite Speech backend initialized (%s variant)",
           runtime->config.backend == ETHERVOX_STT_BACKEND_GRANITE_SPEECH_PLUS ? "PLUS" : "BASE");
  return ETHERVOX_SUCCESS;
}

ethervox_result_t ethervox_stt_granite_speech_start(ethervox_stt_runtime_t* runtime) {
  granite_speech_context_t* gs = (granite_speech_context_t*)runtime->backend_context;
  if (!gs) return ETHERVOX_ERROR_NOT_INITIALIZED;

  // Reset the KV cache so each utterance/chunk starts from a clean context -
  // Granite Speech is invoked one-shot per finalize() call, not as a
  // persistent multi-turn chat session (see file header).
  llama_memory_t mem = llama_get_memory(gs->ctx);
  if (mem) {
    llama_memory_clear(mem, true);
  }
  return ETHERVOX_SUCCESS;
}

// Granite Speech has no incremental/partial-result path (unlike Whisper's
// streaming design) - audio is simply accumulated here and transcribed in
// one pass when ethervox_stt_granite_speech_finalize() is called.
ethervox_result_t ethervox_stt_granite_speech_process(ethervox_stt_runtime_t* runtime,
                                                       const ethervox_audio_buffer_t* audio_buffer,
                                                       ethervox_stt_result_t* result) {
  if (!runtime->backend_context) return ETHERVOX_ERROR_NOT_INITIALIZED;

  uint32_t space_left = runtime->accumulator_size - runtime->accumulator_write_pos;
  uint32_t to_copy = audio_buffer->size < space_left ? audio_buffer->size : space_left;
  if (to_copy > 0) {
    memcpy(runtime->audio_accumulator + runtime->accumulator_write_pos,
           audio_buffer->data, to_copy * sizeof(float));
    runtime->accumulator_write_pos += to_copy;
  }
  if (to_copy < audio_buffer->size) {
    LOG_WARN("Granite Speech: audio accumulator full (%u/%u samples), dropping %u samples - "
             "caller should finalize() more often for this mode",
             runtime->accumulator_write_pos, runtime->accumulator_size,
             audio_buffer->size - to_copy);
  }

  // No partial result to report - Mode 1/4 UI live captions come from the
  // Governor's own streaming, not from STT partials.
  result->is_partial = false;
  result->is_final = false;
  return ETHERVOX_SUCCESS;
}

ethervox_result_t ethervox_stt_granite_speech_finalize(ethervox_stt_runtime_t* runtime,
                                                        ethervox_stt_result_t* result) {
  granite_speech_context_t* gs = (granite_speech_context_t*)runtime->backend_context;
  if (!gs) return ETHERVOX_ERROR_NOT_INITIALIZED;

  if (runtime->accumulator_write_pos == 0) {
    LOG_WARN("Granite Speech: finalize() called with no accumulated audio");
    return ETHERVOX_ERROR_INVALID_ARGUMENT;
  }

  bool is_plus = (runtime->config.backend == ETHERVOX_STT_BACKEND_GRANITE_SPEECH_PLUS);
  const char* task_prompt = is_plus ? GRANITE_SPEECH_SAA_PROMPT : GRANITE_SPEECH_ASR_PROMPT;
  uint32_t max_tokens = runtime->config.max_transcript_tokens != 0
                            ? runtime->config.max_transcript_tokens
                            : (is_plus ? GRANITE_SPEECH_SAA_MAX_TOKENS : GRANITE_SPEECH_ASR_MAX_TOKENS);

  // Build the full prompt: system + user (containing the "<|audio|>" marker)
  // + assistant generation-prompt tag (+ prefix_text, if carrying forward a
  // Mode 2 incremental-decoding session - matches the model's own Jinja
  // template, which appends prefix_text immediately after the assistant tag).
  char prompt[GRANITE_SPEECH_PROMPT_BUF_SIZE];
  int written = snprintf(prompt, sizeof(prompt), "%s%s%s%s%s%s%s%s",
                          gs->tmpl->system_start, GRANITE_SPEECH_SYSTEM_PROMPT, gs->tmpl->system_end,
                          gs->tmpl->user_start, task_prompt, gs->tmpl->user_end,
                          gs->tmpl->assistant_start,
                          runtime->config.prefix_text ? runtime->config.prefix_text : "");
  if (written < 0 || (size_t)written >= sizeof(prompt)) {
    LOG_ERROR("Granite Speech: prompt buffer too small (needed %d bytes)", written);
    return ETHERVOX_ERROR_INVALID_ARGUMENT;
  }

  mtmd_bitmap* bitmap = mtmd_bitmap_init_from_audio(runtime->accumulator_write_pos, runtime->audio_accumulator);
  if (!bitmap) {
    LOG_ERROR("Granite Speech: failed to construct audio bitmap from %u samples",
              runtime->accumulator_write_pos);
    return ETHERVOX_ERROR_INVALID_ARGUMENT;
  }

  mtmd_input_text input_text = {.text = prompt, .add_special = true, .parse_special = true};
  mtmd_input_chunks* chunks = mtmd_input_chunks_init();
  const mtmd_bitmap* bitmaps[1] = {bitmap};
  int32_t tok_rc = mtmd_tokenize(gs->mctx, chunks, &input_text, bitmaps, 1);
  if (tok_rc != 0) {
    LOG_ERROR("Granite Speech: mtmd_tokenize failed (rc=%d)", tok_rc);
    mtmd_input_chunks_free(chunks);
    mtmd_bitmap_free(bitmap);
    return ETHERVOX_ERROR_INVALID_ARGUMENT;
  }

  llama_pos new_n_past = 0;
  int32_t eval_rc = mtmd_helper_eval_chunks(gs->mctx, gs->ctx, chunks, /*n_past=*/0, /*seq_id=*/0,
                                            /*n_batch=*/512, /*logits_last=*/true, &new_n_past);
  mtmd_input_chunks_free(chunks);
  mtmd_bitmap_free(bitmap);
  if (eval_rc != 0) {
    LOG_ERROR("Granite Speech: mtmd_helper_eval_chunks failed (rc=%d)", eval_rc);
    return ETHERVOX_ERROR_INVALID_ARGUMENT;
  }

  // Autoregressive greedy decoding of the transcript, one token at a time -
  // mirrors the pattern used for the Governor's own generation loop in
  // src/governor/governor.c. Greedy (temp=0) matches the decoding IBM/
  // llama.cpp validated their token-for-token reference match against.
  const struct llama_vocab* vocab = llama_model_get_vocab(gs->model);
  struct llama_sampler* sampler = llama_sampler_chain_init(llama_sampler_chain_default_params());
  llama_sampler_chain_add(sampler, llama_sampler_init_temp(0.0f));
  llama_sampler_chain_add(sampler, llama_sampler_init_dist(0));

  char* transcript = (char*)calloc(1, 8192);
  size_t transcript_len = 0;
  size_t transcript_cap = 8192;
  llama_pos pos = new_n_past;

  for (uint32_t i = 0; i < max_tokens; i++) {
    llama_token next_token = llama_sampler_sample(sampler, gs->ctx, -1);
    if (llama_vocab_is_eog(vocab, next_token)) {
      break;
    }

    char piece[128];
    int n_chars = llama_token_to_piece(vocab, next_token, piece, sizeof(piece), 0, false);
    if (n_chars > 0) {
      if (transcript_len + (size_t)n_chars + 1 > transcript_cap) {
        transcript_cap *= 2;
        char* grown = (char*)realloc(transcript, transcript_cap);
        if (!grown) {
          LOG_ERROR("Granite Speech: out of memory growing transcript buffer");
          break;
        }
        transcript = grown;
      }
      memcpy(transcript + transcript_len, piece, (size_t)n_chars);
      transcript_len += (size_t)n_chars;
      transcript[transcript_len] = '\0';

      if (chat_template_has_stop_sequence(gs->tmpl, transcript)) {
        break;
      }
    }

    llama_batch batch = llama_batch_init(1, 0, 1);
    batch.n_tokens = 1;
    batch.token[0] = next_token;
    batch.pos[0] = pos;
    batch.n_seq_id[0] = 1;
    batch.seq_id[0][0] = 0;
    batch.logits[0] = true;
    int decode_rc = llama_decode(gs->ctx, batch);
    llama_batch_free(batch);
    if (decode_rc != 0) {
      LOG_ERROR("Granite Speech: llama_decode failed during generation (rc=%d)", decode_rc);
      break;
    }
    pos++;
  }

  llama_sampler_free(sampler);

  // Strip any trailing stop-sequence text that leaked into the buffer before
  // the check above caught it (e.g. "<|end_of_text|>" itself is 1-3 tokens).
  for (int i = 0; i < gs->tmpl->stop_sequence_count; i++) {
    const char* stop = gs->tmpl->stop_sequences[i];
    if (!stop) continue;
    char* found = strstr(transcript, stop);
    if (found) {
      *found = '\0';
      transcript_len = strlen(transcript);
    }
  }

  result->text = transcript;  // ownership transferred to caller (ethervox_stt_result_free)
  result->confidence = 1.0f;   // Granite Speech does not expose a per-utterance confidence score
  result->is_partial = false;
  result->is_final = true;
  result->language = runtime->config.language;

  runtime->accumulator_write_pos = 0;  // Ready for the next utterance/chunk
  return ETHERVOX_SUCCESS;
}

void ethervox_stt_granite_speech_stop(ethervox_stt_runtime_t* runtime) {
  (void)runtime;
  // No per-session teardown needed - the model/context stay resident across
  // start()/stop() cycles (reloading a ~1-2GB GGUF per utterance would be far
  // too slow); only ethervox_stt_granite_speech_cleanup() releases them.
}

void ethervox_stt_granite_speech_cleanup(ethervox_stt_runtime_t* runtime) {
  granite_speech_context_t* gs = (granite_speech_context_t*)runtime->backend_context;
  if (!gs) return;

  if (gs->mctx) mtmd_free(gs->mctx);
  if (gs->ctx) llama_free(gs->ctx);
  if (gs->model) llama_model_free(gs->model);
  free(gs);
  runtime->backend_context = NULL;
}

#else  // !(LLAMA_CPP_AVAILABLE && MTMD_AVAILABLE)

// Stub implementation for builds without llama.cpp/mtmd (e.g. mtmd not yet
// vendored). Fails loudly rather than silently no-opping, so a missing build
// dependency is caught immediately instead of masquerading as "no speech
// detected".
ethervox_result_t ethervox_stt_granite_speech_init(ethervox_stt_runtime_t* runtime) {
  (void)runtime;
  ETHERVOX_LOG_ERROR("Granite Speech backend requires LLAMA_CPP_AVAILABLE and MTMD_AVAILABLE - "
                     "rebuild with llama.cpp mtmd support (see cmake/FetchDependencies.cmake)");
  return ETHERVOX_ERROR_NOT_SUPPORTED;
}
ethervox_result_t ethervox_stt_granite_speech_start(ethervox_stt_runtime_t* runtime) {
  (void)runtime;
  return ETHERVOX_ERROR_NOT_SUPPORTED;
}
ethervox_result_t ethervox_stt_granite_speech_process(ethervox_stt_runtime_t* runtime,
                                                       const ethervox_audio_buffer_t* audio_buffer,
                                                       ethervox_stt_result_t* result) {
  (void)runtime; (void)audio_buffer; (void)result;
  return ETHERVOX_ERROR_NOT_SUPPORTED;
}
ethervox_result_t ethervox_stt_granite_speech_finalize(ethervox_stt_runtime_t* runtime,
                                                        ethervox_stt_result_t* result) {
  (void)runtime; (void)result;
  return ETHERVOX_ERROR_NOT_SUPPORTED;
}
void ethervox_stt_granite_speech_stop(ethervox_stt_runtime_t* runtime) { (void)runtime; }
void ethervox_stt_granite_speech_cleanup(ethervox_stt_runtime_t* runtime) { (void)runtime; }

#endif  // LLAMA_CPP_AVAILABLE && MTMD_AVAILABLE
