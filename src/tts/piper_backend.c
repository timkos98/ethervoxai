/**
 * @file piper_backend.c
 * @brief Piper TTS backend using ONNX Runtime
 * 
 * Neural text-to-speech synthesis using Piper models.
 * Converts text → phonemes → ONNX inference → raw PCM audio.
 */

#include "ethervox/tts.h"
#include "ethervox/text_normalizer.h"
#include "ethervox/logging.h"
#include "phonemizer/phonemizer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <onnxruntime/onnxruntime_c_api.h>
#include <speex/speex_resampler.h>

#define PIPER_MAX_PHONEMES 512
#define PIPER_SAMPLE_RATE 22050
#define TARGET_SAMPLE_RATE 16000
#define MAX_PHONEME_MAP_SIZE 256

// Phoneme ID map entry
typedef struct {
    char phoneme[8];  // UTF-8 phoneme (up to 4 bytes + null)
    int id;
} phoneme_map_entry_t;

typedef struct {
    ethervox_tts_config_t config;
    const OrtApi* ort_api;
    OrtEnv* env;
    OrtSession* session;
    OrtMemoryInfo* memory_info;
    SpeexResamplerState* resampler;
    phoneme_map_entry_t phoneme_map[MAX_PHONEME_MAP_SIZE];
    int phoneme_map_size;
    char piper_voice[32];  // e.g., "en-us", "es-419", "zh", "de" (from model config)
    char language_code[16];  // e.g., "en_US", "es_MX", "zh_CN", "de_DE"
    bool has_speaker_id_input;  // True if model expects 'sid' input
    bool initialized;
    phonemizer_t* phonemizer;  // Custom phonemizer context
} piper_context_t;

// ONNX Runtime error check
#define ORT_CHECK(expr) \
    do { \
        OrtStatus* status = (expr); \
        if (status != NULL) { \
            const char* msg = g_ort_api->GetErrorMessage(status); \
            ETHERVOX_LOG_ERROR("[Piper] ONNX Error: %s", msg); \
            g_ort_api->ReleaseStatus(status); \
            return -1; \
        } \
    } while(0)

static const OrtApi* g_ort_api = NULL;

/**
 * Load phoneme_id_map from model's .onnx.json config file
 * Maps UTF-8 phoneme characters to integer IDs
 */
static int load_phoneme_map(const char* model_path, piper_context_t* ctx) {
    // Construct config path: model.onnx → model.onnx.json
    char config_path[512];
    snprintf(config_path, sizeof(config_path), "%s.json", model_path);
    
    FILE* f = fopen(config_path, "r");
    if (!f) {
        ETHERVOX_LOG_WARN("[Piper] Could not open config file: %s", config_path);
        return -1;
    }
    
    // Read entire config file
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char* config_json = malloc(fsize + 1);
    if (!config_json) {
        fclose(f);
        return -1;
    }
    
    fread(config_json, 1, fsize, f);
    config_json[fsize] = '\0';
    fclose(f);
    
    // Find "phoneme_id_map" section
    const char* map_start = strstr(config_json, "\"phoneme_id_map\"");
    if (!map_start) {
        ETHERVOX_LOG_DEBUG("[Piper] No phoneme_id_map found in config");
        free(config_json);
        return -1;
    }
    
    // Skip to opening brace
    map_start = strchr(map_start, '{');
    if (!map_start) {
        free(config_json);
        return -1;
    }
    
    // Parse phoneme entries: "phoneme": [id]
    ctx->phoneme_map_size = 0;
    const char* p = map_start + 1;
    
    while (*p && ctx->phoneme_map_size < MAX_PHONEME_MAP_SIZE) {
        // Skip whitespace
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
        
        if (*p == '}') break;  // End of phoneme_id_map
        if (*p == ',') { p++; continue; }
        
        // Expect: "phoneme": [id]
        if (*p != '"') break;
        p++;  // Skip opening quote
        
        // Extract phoneme (UTF-8 character)
        const char* phoneme_start = p;
        while (*p && *p != '"' && (p - phoneme_start) < 7) p++;
        
        if (*p != '"') break;
        
        size_t phoneme_len = p - phoneme_start;
        if (phoneme_len > 0 && phoneme_len < 8) {
            strncpy(ctx->phoneme_map[ctx->phoneme_map_size].phoneme, phoneme_start, phoneme_len);
            ctx->phoneme_map[ctx->phoneme_map_size].phoneme[phoneme_len] = '\0';
            
            // Skip to ID value
            p = strchr(p, '[');
            if (!p) break;
            p++;
            
            int id = atoi(p);
            ctx->phoneme_map[ctx->phoneme_map_size].id = id;
            ctx->phoneme_map_size++;
            
            // Skip to closing bracket
            p = strchr(p, ']');
            if (!p) break;
            p++;
        } else {
            break;
        }
    }
    
    // Parse language code: "language": { "code": "es_MX", "family": "...", "region": "..." }
    // This is the most reliable identifier for determining phonemization rules
    const char* lang_section = strstr(config_json, "\"language\"");
    if (lang_section) {
        const char* code_start = strstr(lang_section, "\"code\"");
        if (code_start) {
            // Skip past "code" key and find value
            code_start = strchr(code_start + 6, ':');
            if (code_start) {
                // Find opening quote of value
                code_start = strchr(code_start, '"');
                if (code_start) {
                    code_start++;
                    const char* code_end = strchr(code_start, '"');
                    if (code_end && (code_end - code_start) < sizeof(ctx->language_code)) {
                        strncpy(ctx->language_code, code_start, code_end - code_start);
                        ctx->language_code[code_end - code_start] = '\0';
                        ETHERVOX_LOG_DEBUG("[Piper] language code: %s\n", ctx->language_code);
                        
                        // Convert language code to phonemizer format
                        // en_US -> en-us, de_DE -> de-de, zh_CN -> zh-cn, es_MX -> es-mx
                        char phoneme_lang[32] = {0};
                        strncpy(phoneme_lang, ctx->language_code, sizeof(phoneme_lang) - 1);
                        
                        // Convert to lowercase and replace underscore with hyphen
                        for (char* c = phoneme_lang; *c; c++) {
                            *c = tolower(*c);
                            if (*c == '_') *c = '-';
                        }
                        
                        strncpy(ctx->piper_voice, phoneme_lang, sizeof(ctx->piper_voice) - 1);
                        ETHERVOX_LOG_DEBUG("[Piper] Phoneme language (from code): %s", ctx->piper_voice);
                    }
                }
            }
        }
    }
    
    // Fallback: Try "phonemes": { "language": "en-us" } (older format)
    if (ctx->piper_voice[0] == '\0') {
        const char* phonemes_section = strstr(config_json, "\"phonemes\"");
        const char* voice_section = phonemes_section ? phonemes_section : strstr(config_json, "\"espeak\"");
        
        if (voice_section) {
            // Try "language" field first (new format), then "voice" (legacy)
            const char* voice_start = strstr(voice_section, "\"language\"");
            if (!voice_start) {
                voice_start = strstr(voice_section, "\"voice\"");
            }
            
            if (voice_start) {
                // Find the colon after the key
                voice_start = strchr(voice_start, ':');
                if (voice_start) {
                    // Find the opening quote of the value
                    voice_start = strchr(voice_start, '"');
                    if (voice_start) {
                        voice_start++;
                        const char* voice_end = strchr(voice_start, '"');
                        if (voice_end && (voice_end - voice_start) < sizeof(ctx->piper_voice)) {
                            strncpy(ctx->piper_voice, voice_start, voice_end - voice_start);
                            ctx->piper_voice[voice_end - voice_start] = '\0';
                            ETHERVOX_LOG_DEBUG("[Piper] Phoneme language (from phonemes): %s", ctx->piper_voice);
                        }
                    }
                }
            }
        }
    }
    
    // Final fallback if phoneme language not found in config
    if (ctx->piper_voice[0] == '\0') {
        strcpy(ctx->piper_voice, "en-us");
        ETHERVOX_LOG_DEBUG("[Piper] Warning: No phoneme language in config, using default 'en-us'");
    }
    
    free(config_json);
    ETHERVOX_LOG_DEBUG("[Piper] Loaded %d phoneme mappings from config", ctx->phoneme_map_size);
    return 0;
}

/**
 * Look up phoneme ID from loaded map
 */
static int phoneme_to_id(piper_context_t* ctx, const char* phoneme) {
    // Special tokens
    if (strcmp(phoneme, "^") == 0) {
        // Look up in map first, fallback to 1
        for (int i = 0; i < ctx->phoneme_map_size; i++) {
            if (strcmp(ctx->phoneme_map[i].phoneme, "^") == 0) {
                return ctx->phoneme_map[i].id;
            }
        }
        return 1;  // Fallback
    }
    
    if (strcmp(phoneme, "$") == 0) {
        for (int i = 0; i < ctx->phoneme_map_size; i++) {
            if (strcmp(ctx->phoneme_map[i].phoneme, "$") == 0) {
                return ctx->phoneme_map[i].id;
            }
        }
        return 2;  // Fallback
    }
    
    // Normal phoneme lookup
    for (int i = 0; i < ctx->phoneme_map_size; i++) {
        if (strcmp(ctx->phoneme_map[i].phoneme, phoneme) == 0) {
            return ctx->phoneme_map[i].id;
        }
    }
    
    // Unknown phoneme → use pad token (usually "_")
    for (int i = 0; i < ctx->phoneme_map_size; i++) {
        if (strcmp(ctx->phoneme_map[i].phoneme, "_") == 0) {
            return ctx->phoneme_map[i].id;
        }
    }
    
    return 0;  // Last resort fallback
}

/**
 * Tokenize IPA string into individual UTF-8 character tokens
 * Piper models expect each IPA character as a separate token (including stress/length markers)
 * Special handling: Keep diphthongs together (ɔɪ, aɪ, aʊ, eɪ, oʊ) with their stress markers
 */
static int tokenize_ipa(const char* ipa, char tokens[][8], size_t max_tokens) {
    size_t token_count = 0;
    const char* p = ipa;
    
    while (*p && token_count < max_tokens) {
        char* token = tokens[token_count];
        size_t token_len = 0;
        
        // Handle space as a phoneme token (word boundary for prosody)
        if (*p == ' ') {
            token[0] = ' '; token[1] = '\0';
            p++;
            token_count++;
            continue;
        }
        
        // Skip periods (sentence boundaries handled by punctuation)
        if (*p == '.') {
            p++;
            continue;
        }
        
        // Check for stress marker followed by diphthong (keep together as one token)
        // Common English diphthongs: ɔɪ (choice), aɪ (price), aʊ (mouth), eɪ (face), oʊ (goat)
        if (((unsigned char)p[0] == 0xCB && (unsigned char)p[1] == 0x88) ||  // ˈ (primary stress, 3-byte UTF-8)
            ((unsigned char)p[0] == 0xCB && (unsigned char)p[1] == 0x8C)) {  // ˌ (secondary stress, 3-byte UTF-8)
            // Stress marker detected, check if followed by diphthong
            const char* after_stress = p + 3;  // Skip stress marker (3 bytes)
            
            // Check for diphthong patterns (2-byte + 2-byte UTF-8)
            bool is_diphthong = false;
            int diphthong_len = 0;
            
            // ɔɪ (0xC994 + 0xC9AA)
            if ((unsigned char)after_stress[0] == 0xC9 && (unsigned char)after_stress[1] == 0x94 &&
                (unsigned char)after_stress[2] == 0xC9 && (unsigned char)after_stress[3] == 0xAA) {
                is_diphthong = true; diphthong_len = 4;
            }
            // aɪ (0x61 + 0xC9AA)
            else if (after_stress[0] == 'a' && 
                     (unsigned char)after_stress[1] == 0xC9 && (unsigned char)after_stress[2] == 0xAA) {
                is_diphthong = true; diphthong_len = 3;
            }
            // aʊ (0x61 + 0xCA8A)
            else if (after_stress[0] == 'a' &&
                     (unsigned char)after_stress[1] == 0xCA && (unsigned char)after_stress[2] == 0x8A) {
                is_diphthong = true; diphthong_len = 3;
            }
            // eɪ (0x65 + 0xC9AA)
            else if (after_stress[0] == 'e' &&
                     (unsigned char)after_stress[1] == 0xC9 && (unsigned char)after_stress[2] == 0xAA) {
                is_diphthong = true; diphthong_len = 3;
            }
            // oʊ (0x6F + 0xCA8A)
            else if (after_stress[0] == 'o' &&
                     (unsigned char)after_stress[1] == 0xCA && (unsigned char)after_stress[2] == 0x8A) {
                is_diphthong = true; diphthong_len = 3;
            }
            
            if (is_diphthong) {
                // Copy stress + diphthong as one token
                memcpy(token, p, 3 + diphthong_len);  // stress (3) + diphthong
                token[3 + diphthong_len] = '\0';
                p += 3 + diphthong_len;
                token_count++;
                continue;
            }
        }
        
        // Parse one UTF-8 character (single phoneme)
        if ((*p & 0x80) == 0) {
            // Single-byte ASCII (a-z, punctuation, etc.)
            token[0] = *p; token[1] = '\0'; token_len = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            // 2-byte UTF-8 (most IPA vowels/consonants)
            token[0] = p[0]; token[1] = p[1]; token[2] = '\0'; token_len = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            // 3-byte UTF-8 (stress markers ˈˌ, length ː, etc.)
            token[0] = p[0]; token[1] = p[1]; token[2] = p[2]; token[3] = '\0'; token_len = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            // 4-byte UTF-8 (rare IPA symbols)
            token[0] = p[0]; token[1] = p[1]; token[2] = p[2]; token[3] = p[3]; token[4] = '\0'; token_len = 4;
        } else {
            // Invalid UTF-8, skip
            p++;
            continue;
        }
        
        p += token_len;
        token_count++;
    }
    
    return token_count;
}

/**
 * Convert IPA phonemes directly to phoneme IDs (bypass phonemizer)
 * Used for pronunciation training where IPA is already known
 */
static int ipa_to_phoneme_ids(piper_context_t* ctx, const char* ipa_text, int64_t* phoneme_ids, size_t* phoneme_count) {
    ETHERVOX_LOG_DEBUG("[Piper] Direct IPA: '%s'\n", ipa_text);
    
    // Tokenize IPA into individual UTF-8 characters
    char phoneme_tokens[PIPER_MAX_PHONEMES][8];
    size_t total_tokens = tokenize_ipa(ipa_text, phoneme_tokens, PIPER_MAX_PHONEMES);
    
    ETHERVOX_LOG_DEBUG("[Piper] Tokenized %zu IPA phonemes\n", total_tokens);
    
    // Convert tokens to IDs using phoneme_id_map
    size_t id_count = 0;
    
    // Add BOS token (^)
    phoneme_ids[id_count++] = phoneme_to_id(ctx, "^");
    
    for (size_t i = 0; i < total_tokens && id_count < PIPER_MAX_PHONEMES - 4; i++) {
        int id = phoneme_to_id(ctx, phoneme_tokens[i]);
        phoneme_ids[id_count++] = id;
        // Add PAD token after each phoneme (Piper requirement for proper duration)
        phoneme_ids[id_count++] = 0;  // PAD = "_"
    }
    
    // Add EOS token ($)
    phoneme_ids[id_count++] = phoneme_to_id(ctx, "$");
    
    *phoneme_count = id_count;
    
    ETHERVOX_LOG_DEBUG("[Piper] Final phoneme sequence (%zu tokens): [", id_count);
    for (size_t i = 0; i < (id_count < 20 ? id_count : 20); i++) {
        fprintf(stderr, "%lld%s", (long long)phoneme_ids[i], i < id_count-1 ? "," : "");
    }
    if (id_count > 20) fprintf(stderr, "...");
    fprintf(stderr, "]\n");
    
    return 0;
}

/**
 * Convert text to phonemes using custom phonemizer
 */
static int text_to_phonemes(piper_context_t* ctx, const char* text, int64_t* phoneme_ids, size_t* phoneme_count) {
    if (!ctx->phonemizer) {
        ETHERVOX_LOG_DEBUG("[Piper] ERROR: Phonemizer not initialized");
        return -1;
    }
    
    ETHERVOX_LOG_DEBUG("[Piper] Text: '%s'\n", text);
    
    // Normalize text (numbers → words, times → spoken form)
    char normalized_text[4096];
    if (ethervox_tts_normalize_text(text, normalized_text, sizeof(normalized_text)) != 0) {
        ETHERVOX_LOG_DEBUG("[Piper] WARNING: Text normalization failed, using original");
        strncpy(normalized_text, text, sizeof(normalized_text) - 1);
        normalized_text[sizeof(normalized_text) - 1] = '\0';
    } else if (strcmp(text, normalized_text) != 0) {
        ETHERVOX_LOG_DEBUG("[Piper] Normalized: '%s'\n", normalized_text);
    }
    
    // Convert normalized text to IPA using phonemizer
    char ipa_output[4096];
    if (phonemizer_text_to_ipa(ctx->phonemizer, normalized_text, ipa_output, sizeof(ipa_output)) != 0) {
        ETHERVOX_LOG_DEBUG("[Piper] ERROR: Phonemizer failed");
        return -1;
    }
    
    ETHERVOX_LOG_DEBUG("[Piper] IPA: '%s'\n", ipa_output);
    
    // Tokenize IPA into individual UTF-8 characters
    char phoneme_tokens[PIPER_MAX_PHONEMES][8];
    size_t total_tokens = tokenize_ipa(ipa_output, phoneme_tokens, PIPER_MAX_PHONEMES);
    
    ETHERVOX_LOG_DEBUG("[Piper] Tokenized %zu IPA phonemes\n", total_tokens);
    
    // Convert tokens to IDs using phoneme_id_map
    size_t id_count = 0;
    
    // Add BOS token (^)
    phoneme_ids[id_count++] = phoneme_to_id(ctx, "^");
    
    for (size_t i = 0; i < total_tokens && id_count < PIPER_MAX_PHONEMES - 4; i++) {
        int id = phoneme_to_id(ctx, phoneme_tokens[i]);
        if (id < 0) {
            ETHERVOX_LOG_DEBUG("[Piper] WARNING: Unknown phoneme '%s', skipping\n", phoneme_tokens[i]);
            continue;
        }
        phoneme_ids[id_count++] = id;
        // Add PAD token after each phoneme (Piper requirement for proper duration)
        phoneme_ids[id_count++] = 0;  // PAD = "_"
    }
    
    // Add EOS token ($)
    phoneme_ids[id_count++] = phoneme_to_id(ctx, "$");
    
    *phoneme_count = id_count;
    
    ETHERVOX_LOG_DEBUG("[Piper] Final phoneme sequence (%zu tokens): [", id_count);
    for (size_t i = 0; i < (id_count < 20 ? id_count : 20); i++) {
        fprintf(stderr, "%lld%s", (long long)phoneme_ids[i], i < id_count-1 ? "," : "");
    }
    if (id_count > 20) fprintf(stderr, "...");
    fprintf(stderr, "]\n");
    
    return 0;
}

/**
 * Run ONNX inference
 */
static int piper_infer(piper_context_t* ctx, 
                       const int64_t* phoneme_ids,
                       size_t phoneme_count,
                       float** output_audio,
                       size_t* output_sample_count) {
    
    if (!ctx->session) {
        ETHERVOX_LOG_DEBUG("[Piper] Session not initialized");
        return -1;
    }
    
    // Create input tensor (phoneme IDs)
    int64_t input_shape[] = {1, (int64_t)phoneme_count};
    size_t input_tensor_size = phoneme_count;
    
    ETHERVOX_LOG_DEBUG("[Piper] ONNX input: phoneme_count=%zu, shape=[%lld, %lld]\n", 
           phoneme_count, input_shape[0], input_shape[1]);
    printf("[Piper] First 10 phoneme IDs: [");
    for (size_t i = 0; i < (phoneme_count < 10 ? phoneme_count : 10); i++) {
        printf("%lld%s", phoneme_ids[i], i < 9 && i < phoneme_count - 1 ? "," : "");
    }
    printf("]\n");
    
    OrtValue* input_tensor = NULL;
    ORT_CHECK(g_ort_api->CreateTensorWithDataAsOrtValue(
        ctx->memory_info,
        (void*)phoneme_ids,
        input_tensor_size * sizeof(int64_t),
        input_shape,
        2,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
        &input_tensor
    ));
    
    // Create input_lengths tensor (required by Piper models)
    int64_t lengths_data[] = {(int64_t)phoneme_count};
    int64_t lengths_shape[] = {1};
    
    OrtValue* lengths_tensor = NULL;
    ORT_CHECK(g_ort_api->CreateTensorWithDataAsOrtValue(
        ctx->memory_info,
        (void*)lengths_data,
        sizeof(int64_t),
        lengths_shape,
        1,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
        &lengths_tensor
    ));
    
    // Create scales tensor (noise_scale, length_scale, noise_w)
    // noise_scale: phoneme duration variance (0.0=monotone, 1.0=very expressive)
    // length_scale: speed control (higher=slower, lower=faster)
    // noise_w: pitch/prosody variance (0.0=flat, 1.0=very expressive)
    float length_scale = (ctx->config.speaking_rate > 0.0f) ? (1.0f / ctx->config.speaking_rate) : 1.0f;
    float noise_scale = (ctx->config.phoneme_variance >= 0.0f) ? ctx->config.phoneme_variance : 0.667f;
    float noise_w = (ctx->config.prosody_variance >= 0.0f) ? ctx->config.prosody_variance : 0.8f;
    
    ETHERVOX_LOG_DEBUG("[Piper] Synthesis scales: noise_scale=%.3f, length_scale=%.3f, noise_w=%.3f\n",
           noise_scale, length_scale, noise_w);
    
    float scales_data[] = {noise_scale, length_scale, noise_w};
    int64_t scales_shape[] = {3};
    
    OrtValue* scales_tensor = NULL;
    ORT_CHECK(g_ort_api->CreateTensorWithDataAsOrtValue(
        ctx->memory_info,
        (void*)scales_data,
        3 * sizeof(float),
        scales_shape,
        1,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
        &scales_tensor
    ));
    
    // Prepare input arrays
    const char* input_names[4];
    OrtValue* input_tensors[4];
    size_t num_inputs = 3;  // Default: input, input_lengths, scales
    
    input_names[0] = "input";
    input_names[1] = "input_lengths";
    input_names[2] = "scales";
    input_tensors[0] = input_tensor;
    input_tensors[1] = lengths_tensor;
    input_tensors[2] = scales_tensor;
    
    // Create speaker_id tensor only for multi-speaker models
    OrtValue* speaker_id_tensor = NULL;
    int64_t speaker_id_data[] = {(int64_t)ctx->config.speaker_id};
    int64_t speaker_id_shape[] = {1};
    
    if (ctx->has_speaker_id_input) {
        ORT_CHECK(g_ort_api->CreateTensorWithDataAsOrtValue(
            ctx->memory_info,
            (void*)speaker_id_data,
            sizeof(int64_t),
            speaker_id_shape,
            1,
            ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64,
            &speaker_id_tensor
        ));
        
        input_names[3] = "sid";
        input_tensors[3] = speaker_id_tensor;
        num_inputs = 4;
    }
    
    // Run inference
    const char* output_names[] = {"output"};
    OrtValue* output_tensor = NULL;
    
    OrtStatus* status = g_ort_api->Run(
        ctx->session,
        NULL,  // run options
        input_names,
        (const OrtValue* const*)input_tensors,
        num_inputs,
        output_names,
        1,
        &output_tensor
    );
    
    // Release input tensors
    g_ort_api->ReleaseValue(input_tensor);
    g_ort_api->ReleaseValue(lengths_tensor);
    g_ort_api->ReleaseValue(scales_tensor);
    if (speaker_id_tensor) {
        g_ort_api->ReleaseValue(speaker_id_tensor);
    }
    
    if (status != NULL) {
        const char* msg = g_ort_api->GetErrorMessage(status);
        ETHERVOX_LOG_DEBUG("[Piper] ONNX Inference error: %s\n", msg);
        fprintf(stderr, "[Piper] Input details: phoneme_count=%zu, input_names=[%s, %s, %s, %s]\n",
                phoneme_count, input_names[0], input_names[1], input_names[2], input_names[3]);
        g_ort_api->ReleaseStatus(status);
        return -1;
    }
    
    // Get output tensor data
    float* output_data = NULL;
    ORT_CHECK(g_ort_api->GetTensorMutableData(output_tensor, (void**)&output_data));
    
    // Get output shape
    OrtTensorTypeAndShapeInfo* shape_info = NULL;
    ORT_CHECK(g_ort_api->GetTensorTypeAndShape(output_tensor, &shape_info));
    
    size_t num_dims = 0;
    ORT_CHECK(g_ort_api->GetDimensionsCount(shape_info, &num_dims));
    
    int64_t* dims = (int64_t*)malloc(sizeof(int64_t) * num_dims);
    ORT_CHECK(g_ort_api->GetDimensions(shape_info, dims, num_dims));
    
    size_t sample_count = 1;
    for (size_t i = 0; i < num_dims; i++) {
        sample_count *= dims[i];
    }
    
    ETHERVOX_LOG_DEBUG("[Piper] ONNX output: dims=%zu, shape=[", num_dims);
    for (size_t i = 0; i < num_dims; i++) {
        printf("%lld%s", dims[i], i < num_dims - 1 ? "," : "");
    }
    printf("], sample_count=%zu\n", sample_count);
    
    // Check first few samples for sanity
    printf("[Piper] First 10 audio samples: [");
    for (size_t i = 0; i < (sample_count < 10 ? sample_count : 10); i++) {
        printf("%.4f%s", output_data[i], i < 9 && i < sample_count - 1 ? "," : "");
    }
    printf("]\n");
    
    // Calculate audio statistics
    float min_val = output_data[0], max_val = output_data[0];
    double sum = 0.0, sum_sq = 0.0;
    for (size_t i = 0; i < sample_count; i++) {
        float val = output_data[i];
        if (val < min_val) min_val = val;
        if (val > max_val) max_val = val;
        sum += val;
        sum_sq += val * val;
    }
    float mean = sum / sample_count;
    float rms = sqrt(sum_sq / sample_count);
    ETHERVOX_LOG_DEBUG("[Piper] Audio stats: min=%.4f, max=%.4f, mean=%.6f, RMS=%.4f\n", 
           min_val, max_val, mean, rms);
    
    // Copy output
    *output_audio = (float*)malloc(sample_count * sizeof(float));
    memcpy(*output_audio, output_data, sample_count * sizeof(float));
    *output_sample_count = sample_count;
    
    // Debug: Print last 10 samples to verify ordering
    printf("[Piper] Last 10 audio samples: [");
    size_t start_idx = sample_count >= 10 ? sample_count - 10 : 0;
    for (size_t i = start_idx; i < sample_count; i++) {
        printf("%.4f%s", (*output_audio)[i], i < sample_count - 1 ? "," : "");
    }
    printf("]\n");
    
    free(dims);
    g_ort_api->ReleaseTensorTypeAndShapeInfo(shape_info);
    g_ort_api->ReleaseValue(output_tensor);
    
    return 0;
}

/**
 * Resample from 22050Hz to 16000Hz
 */
static int resample_audio(SpeexResamplerState* resampler,
                         const float* input,
                         size_t input_count,
                         float** output,
                         size_t* output_count) {
    
    // Calculate output size
    size_t estimated_output = (input_count * TARGET_SAMPLE_RATE) / PIPER_SAMPLE_RATE + 64;
    *output = (float*)malloc(estimated_output * sizeof(float));
    
    spx_uint32_t in_len = input_count;
    spx_uint32_t out_len = estimated_output;
    
    int err = speex_resampler_process_float(
        resampler,
        0,  // channel
        input,
        &in_len,
        *output,
        &out_len
    );
    
    if (err != RESAMPLER_ERR_SUCCESS) {
        ETHERVOX_LOG_DEBUG("[Piper] Resampling error: %d\n", err);
        free(*output);
        *output = NULL;
        return -1;
    }
    
    *output_count = out_len;
    
    // Debug: Check first and last samples after resampling
    printf("[Piper] After resampling: first sample=%.4f, last sample=%.4f, count=%u\n",
           (*output)[0], (*output)[out_len-1], out_len);
    
    // Debug: Check samples at 25%, 50%, 75% to verify no reversal
    printf("[Piper] Sample check - 25%%:%.4f, 50%%:%.4f, 75%%:%.4f\n",
           (*output)[out_len/4], (*output)[out_len/2], (*output)[out_len*3/4]);
    
    return 0;
}

/**
 * Synthesize speech from IPA phonemes directly (Piper-specific)
 */
int ethervox_tts_piper_synthesize_ipa(ethervox_tts_context_t* ctx_generic,
                                      const char* ipa_phonemes,
                                      ethervox_tts_audio_t* output) {
    piper_context_t* ctx = (piper_context_t*)ctx_generic;
    
    if (!ctx || !ctx->initialized) {
        return -1;
    }
    
    // Convert IPA directly to phoneme IDs (bypass phonemizer)
    int64_t phoneme_ids[PIPER_MAX_PHONEMES];
    size_t phoneme_count = 0;
    
    if (ipa_to_phoneme_ids(ctx, ipa_phonemes, phoneme_ids, &phoneme_count) != 0) {
        return -1;
    }
    
    // Run inference
    float* audio_samples = NULL;
    size_t sample_count = 0;
    
    if (piper_infer(ctx, phoneme_ids, phoneme_count, &audio_samples, &sample_count) != 0) {
        return -1;
    }
    
    output->samples = audio_samples;
    output->sample_count = sample_count;
    output->sample_rate = TARGET_SAMPLE_RATE;
    output->channels = 1;
    
    return 0;
}

ethervox_tts_context_t* ethervox_tts_piper_create(const ethervox_tts_config_t* config) {
    piper_context_t* ctx = (piper_context_t*)calloc(1, sizeof(piper_context_t));
    if (!ctx) return NULL;
    
    ctx->config = *config;
    g_ort_api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    ctx->ort_api = g_ort_api;
    
    // Create ONNX environment
    OrtStatus* status = g_ort_api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "piper", &ctx->env);
    if (status != NULL) {
        ETHERVOX_LOG_DEBUG("[Piper] Failed to create ONNX environment");
        g_ort_api->ReleaseStatus(status);
        free(ctx);
        return NULL;
    }
    
    // Create session options
    OrtSessionOptions* session_options = NULL;
    status = g_ort_api->CreateSessionOptions(&session_options);
    if (status) {
        ETHERVOX_LOG_DEBUG("[Piper] Failed to create session options");
        g_ort_api->ReleaseStatus(status);
        g_ort_api->ReleaseEnv(ctx->env);
        free(ctx);
        return NULL;
    }
    
    status = g_ort_api->SetIntraOpNumThreads(session_options, 1);
    if (status) g_ort_api->ReleaseStatus(status);
    
    status = g_ort_api->SetSessionGraphOptimizationLevel(session_options, ORT_ENABLE_BASIC);
    if (status) g_ort_api->ReleaseStatus(status);
    
    // Load model
    if (!config->model_path) {
        ETHERVOX_LOG_DEBUG("[Piper] Model path not specified");
        g_ort_api->ReleaseSessionOptions(session_options);
        g_ort_api->ReleaseEnv(ctx->env);
        free(ctx);
        return NULL;
    }
    
    status = g_ort_api->CreateSession(ctx->env, config->model_path, session_options, &ctx->session);
    g_ort_api->ReleaseSessionOptions(session_options);
    
    if (status != NULL) {
        const char* msg = g_ort_api->GetErrorMessage(status);
        ETHERVOX_LOG_DEBUG("[Piper] Failed to load model: %s\n", msg);
        g_ort_api->ReleaseStatus(status);
        g_ort_api->ReleaseEnv(ctx->env);
        free(ctx);
        return NULL;
    }
    
    // Create memory info
    status = g_ort_api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &ctx->memory_info);
    if (status) {
        ETHERVOX_LOG_DEBUG("[Piper] Failed to create memory info");
        g_ort_api->ReleaseStatus(status);
        g_ort_api->ReleaseSessionOptions(session_options);
        g_ort_api->ReleaseSession(ctx->session);
        g_ort_api->ReleaseEnv(ctx->env);
        free(ctx);
        return NULL;
    }
    
    // Check if model has speaker_id input (multi-speaker models only)
    ctx->has_speaker_id_input = false;
    size_t num_inputs = 0;
    status = g_ort_api->SessionGetInputCount(ctx->session, &num_inputs);
    if (status != NULL) {
        const char* msg = g_ort_api->GetErrorMessage(status);
        ETHERVOX_LOG_ERROR("[Piper] Failed to get input count: %s", msg);
        g_ort_api->ReleaseStatus(status);
        g_ort_api->ReleaseSession(ctx->session);
        g_ort_api->ReleaseEnv(ctx->env);
        free(ctx);
        return NULL;
    }
    ETHERVOX_LOG_DEBUG("[Piper] Model has %zu input(s)\\n", num_inputs);
    
    // Check if any input is named "sid"
    for (size_t i = 0; i < num_inputs; i++) {
        char* input_name = NULL;
        OrtAllocator* allocator = NULL;
        status = g_ort_api->GetAllocatorWithDefaultOptions(&allocator);
        if (status != NULL) {
            g_ort_api->ReleaseStatus(status);
            continue;
        }
        status = g_ort_api->SessionGetInputName(ctx->session, i, allocator, &input_name);
        if (status != NULL) {
            g_ort_api->ReleaseStatus(status);
            continue;
        }
        
        if (input_name) {
            ETHERVOX_LOG_DEBUG("[Piper]   Input %zu: %s\n", i, input_name);
            if (strcmp(input_name, "sid") == 0) {
                ctx->has_speaker_id_input = true;
                ETHERVOX_LOG_DEBUG("[Piper] Model supports speaker_id (multi-speaker)");
            }
            allocator->Free(allocator, input_name);
        }
    }
    
    if (!ctx->has_speaker_id_input) {
        ETHERVOX_LOG_DEBUG("[Piper] Model is single-speaker (no sid input)");
    }
    
    // Create resampler (22050Hz → 16000Hz)
    int err = 0;
    ctx->resampler = speex_resampler_init(1, PIPER_SAMPLE_RATE, TARGET_SAMPLE_RATE, 5, &err);
    if (err != RESAMPLER_ERR_SUCCESS) {
        ETHERVOX_LOG_DEBUG("[Piper] Failed to create resampler: %d\n", err);
        g_ort_api->ReleaseMemoryInfo(ctx->memory_info);
        g_ort_api->ReleaseSession(ctx->session);
        g_ort_api->ReleaseEnv(ctx->env);
        free(ctx);
        return NULL;
    }
    
    // Load phoneme_id_map from model config
    if (load_phoneme_map(config->model_path, ctx) < 0) {
        ETHERVOX_LOG_DEBUG("[Piper] Warning: Failed to load phoneme map, using default mapping");
    }
    
    // Initialize phonemizer for the detected language
    ctx->phonemizer = phonemizer_create(ctx->piper_voice);
    if (!ctx->phonemizer) {
        ETHERVOX_LOG_ERROR("[Piper] Failed to initialize phonemizer for language: %s\n", ctx->piper_voice);
        speex_resampler_destroy(ctx->resampler);
        g_ort_api->ReleaseMemoryInfo(ctx->memory_info);
        g_ort_api->ReleaseSession(ctx->session);
        g_ort_api->ReleaseEnv(ctx->env);
        free(ctx);
        return NULL;
    }
    
    ctx->initialized = true;
    ETHERVOX_LOG_DEBUG("[Piper] Initialized (model: %s, language: %s)\n", config->model_path, ctx->piper_voice);
    
    return (ethervox_tts_context_t*)ctx;
}

int ethervox_tts_piper_synthesize(ethervox_tts_context_t* ctx,
                                   const char* text,
                                   ethervox_tts_audio_t* output) {
    piper_context_t* piper = (piper_context_t*)ctx;
    
    if (!piper || !piper->initialized) {
        return -1;
    }
    
    // Convert text to phonemes
    int64_t phoneme_ids[PIPER_MAX_PHONEMES];
    size_t phoneme_count = 0;
    
    if (text_to_phonemes(piper, text, phoneme_ids, &phoneme_count) < 0) {
        return -1;
    }
    
    if (phoneme_count == 0) {
        ETHERVOX_LOG_DEBUG("[Piper] No phonemes generated");
        return -1;
    }
    
    // Run ONNX inference
    float* piper_audio = NULL;
    size_t piper_sample_count = 0;
    
    if (piper_infer(piper, phoneme_ids, phoneme_count, &piper_audio, &piper_sample_count) < 0) {
        return -1;
    }
    
    // Resample to 16kHz
    float* resampled_audio = NULL;
    size_t resampled_count = 0;
    
    if (resample_audio(piper->resampler, piper_audio, piper_sample_count, 
                      &resampled_audio, &resampled_count) < 0) {
        free(piper_audio);
        return -1;
    }
    
    free(piper_audio);
    
    // Fill output
    output->samples = resampled_audio;
    output->sample_count = resampled_count;
    output->sample_rate = TARGET_SAMPLE_RATE;
    output->channels = 1;
    
    return 0;
}

void* ethervox_tts_piper_get_phonemizer(void* piper_impl) {
    piper_context_t* piper = (piper_context_t*)piper_impl;
    if (!piper) {
        ETHERVOX_LOG_DEBUG("[Piper] get_phonemizer: piper_impl is NULL");
        return NULL;
    }
    if (!piper->phonemizer) {
        ETHERVOX_LOG_DEBUG("[Piper] get_phonemizer: phonemizer field is NULL");
    }
    return piper->phonemizer;
}

void ethervox_tts_piper_destroy(ethervox_tts_context_t* ctx) {
    piper_context_t* piper = (piper_context_t*)ctx;
    if (!piper) return;
    
    if (piper->phonemizer) {
        phonemizer_destroy(piper->phonemizer);
    }
    
    if (piper->resampler) {
        speex_resampler_destroy(piper->resampler);
    }
    
    if (piper->memory_info) {
        g_ort_api->ReleaseMemoryInfo(piper->memory_info);
    }
    
    if (piper->session) {
        g_ort_api->ReleaseSession(piper->session);
    }
    
    if (piper->env) {
        g_ort_api->ReleaseEnv(piper->env);
    }
    
    free(piper);
}
