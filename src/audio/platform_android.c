/**
 * @file platform_android.c
 * @brief Android-specific audio platform implementation for EthervoxAI
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 *
 * This file is part of EthervoxAI, licensed under CC BY-NC-SA 4.0.
 * You are free to share and adapt this work under the following terms:
 * - Attribution: Credit the original authors
 * - NonCommercial: Not for commercial use
 * - ShareAlike: Distribute under same license
 *
 * For full license terms, see: https://creativecommons.org/licenses/by-nc-sa/4.0/
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ethervox/audio.h"

#if defined(__ANDROID__)

#include <android/log.h>
#include <stdbool.h>

// Check API level for AAudio vs OpenSL ES
#if __ANDROID_API__ >= 26
#define USE_AAUDIO 1
#include <aaudio/AAudio.h>
#else
#define USE_OPENSL_ES 1
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#endif

#define LOG_TAG "EthervoxAudio"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// ===========================================================================
// AAudio Implementation (API 26+)
// ===========================================================================
#ifdef USE_AAUDIO

typedef struct {
  AAudioStream* input_stream;
  AAudioStream* output_stream;
  bool is_recording;
  bool is_playing;
  float* capture_buffer;
  size_t capture_buffer_size;
} aaudio_data_t;

static aaudio_result_t aaudio_data_callback(AAudioStream* stream, void* user_data,
                                            void* audio_data, int32_t num_frames) {
  ethervox_audio_runtime_t* runtime = (ethervox_audio_runtime_t*)user_data;
  (void)stream;
  
  if (!runtime || !audio_data) {
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }

  // Process audio data if callback is set
  if (runtime->on_audio_data) {
    ethervox_audio_buffer_t buffer;
    buffer.data = (float*)audio_data;
    buffer.size = (uint32_t)num_frames;
    buffer.channels = runtime->config.channels;
    
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    buffer.timestamp_us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
    
    runtime->on_audio_data(&buffer, runtime->user_data);
  }

  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void aaudio_error_callback(AAudioStream* stream, void* user_data, aaudio_result_t error) {
  (void)stream;
  (void)user_data;
  LOGE("AAudio error callback: %s", AAudio_convertResultToText(error));
}

static int aaudio_init(ethervox_audio_runtime_t* runtime,
                       const ethervox_audio_config_t* config) {
  aaudio_data_t* audio_data = (aaudio_data_t*)calloc(1, sizeof(aaudio_data_t));
  if (!audio_data) {
    LOGE("Failed to allocate AAudio data");
    return -1;
  }
  
  runtime->platform_data = audio_data;
  audio_data->capture_buffer_size = config->buffer_size;
  audio_data->capture_buffer = (float*)malloc(config->buffer_size * sizeof(float) * config->channels);
  
  if (!audio_data->capture_buffer) {
    LOGE("Failed to allocate capture buffer");
    free(audio_data);
    return -1;
  }
  
  LOGI("AAudio driver initialized (API 26+)");
  return 0;
}

static int aaudio_start_capture(ethervox_audio_runtime_t* runtime) {
  aaudio_data_t* audio_data = (aaudio_data_t*)runtime->platform_data;
  if (!audio_data) {
    LOGE("AAudio data not initialized");
    return -1;
  }

  AAudioStreamBuilder* builder;
  aaudio_result_t result = AAudio_createStreamBuilder(&builder);
  if (result != AAUDIO_OK) {
    LOGE("Failed to create AAudio stream builder: %s", AAudio_convertResultToText(result));
    return -1;
  }

  AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_INPUT);
  AAudioStreamBuilder_setSampleRate(builder, (int32_t)runtime->config.sample_rate);
  AAudioStreamBuilder_setChannelCount(builder, (int32_t)runtime->config.channels);
  AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
  AAudioStreamBuilder_setBufferCapacityInFrames(builder, (int32_t)runtime->config.buffer_size);
  AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
  AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
  AAudioStreamBuilder_setDataCallback(builder, aaudio_data_callback, runtime);
  AAudioStreamBuilder_setErrorCallback(builder, aaudio_error_callback, runtime);

  result = AAudioStreamBuilder_openStream(builder, &audio_data->input_stream);
  AAudioStreamBuilder_delete(builder);

  if (result != AAUDIO_OK) {
    LOGE("Failed to open AAudio input stream: %s", AAudio_convertResultToText(result));
    return -1;
  }

  result = AAudioStream_requestStart(audio_data->input_stream);
  if (result != AAUDIO_OK) {
    LOGE("Failed to start AAudio input stream: %s", AAudio_convertResultToText(result));
    AAudioStream_close(audio_data->input_stream);
    audio_data->input_stream = NULL;
    return -1;
  }

  audio_data->is_recording = true;
  LOGI("AAudio capture started");
  return 0;
}

static int aaudio_stop_capture(ethervox_audio_runtime_t* runtime) {
  aaudio_data_t* audio_data = (aaudio_data_t*)runtime->platform_data;
  if (!audio_data || !audio_data->input_stream) {
    return 0;
  }

  AAudioStream_requestStop(audio_data->input_stream);
  AAudioStream_close(audio_data->input_stream);
  audio_data->input_stream = NULL;
  audio_data->is_recording = false;
  
  LOGI("AAudio capture stopped");
  return 0;
}

static int aaudio_start_playback(ethervox_audio_runtime_t* runtime) {
  aaudio_data_t* audio_data = (aaudio_data_t*)runtime->platform_data;
  if (!audio_data) {
    LOGE("AAudio data not initialized");
    return -1;
  }

  AAudioStreamBuilder* builder;
  aaudio_result_t result = AAudio_createStreamBuilder(&builder);
  if (result != AAUDIO_OK) {
    LOGE("Failed to create AAudio stream builder: %s", AAudio_convertResultToText(result));
    return -1;
  }

  AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
  AAudioStreamBuilder_setSampleRate(builder, (int32_t)runtime->config.sample_rate);
  AAudioStreamBuilder_setChannelCount(builder, (int32_t)runtime->config.channels);
  AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
  AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);

  result = AAudioStreamBuilder_openStream(builder, &audio_data->output_stream);
  AAudioStreamBuilder_delete(builder);

  if (result != AAUDIO_OK) {
    LOGE("Failed to open AAudio output stream: %s", AAudio_convertResultToText(result));
    return -1;
  }

  result = AAudioStream_requestStart(audio_data->output_stream);
  if (result != AAUDIO_OK) {
    LOGE("Failed to start AAudio output stream: %s", AAudio_convertResultToText(result));
    AAudioStream_close(audio_data->output_stream);
    audio_data->output_stream = NULL;
    return -1;
  }

  audio_data->is_playing = true;
  LOGI("AAudio playback started");
  return 0;
}

static int aaudio_stop_playback(ethervox_audio_runtime_t* runtime) {
  aaudio_data_t* audio_data = (aaudio_data_t*)runtime->platform_data;
  if (!audio_data || !audio_data->output_stream) {
    return 0;
  }

  AAudioStream_requestStop(audio_data->output_stream);
  AAudioStream_close(audio_data->output_stream);
  audio_data->output_stream = NULL;
  audio_data->is_playing = false;
  
  LOGI("AAudio playback stopped");
  return 0;
}

static int aaudio_read_audio(ethervox_audio_runtime_t* runtime, ethervox_audio_buffer_t* buffer) {
  aaudio_data_t* audio_data = (aaudio_data_t*)runtime->platform_data;
  if (!audio_data || !audio_data->input_stream || !buffer) {
    return -1;
  }

  int32_t frames_to_read = (int32_t)(buffer->size / runtime->config.channels);
  int32_t frames_read = AAudioStream_read(audio_data->input_stream, buffer->data, 
                                          frames_to_read, 0);
  
  if (frames_read < 0) {
    LOGE("AAudio read error: %s", AAudio_convertResultToText((aaudio_result_t)frames_read));
    return -1;
  }

  buffer->size = (uint32_t)frames_read * runtime->config.channels;
  
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  buffer->timestamp_us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
  
  return frames_read;
}

static int aaudio_write_audio(ethervox_audio_runtime_t* runtime, 
                              const ethervox_audio_buffer_t* buffer) {
  aaudio_data_t* audio_data = (aaudio_data_t*)runtime->platform_data;
  if (!audio_data || !audio_data->output_stream || !buffer) {
    return -1;
  }

  int32_t frames_to_write = (int32_t)(buffer->size / runtime->config.channels);
  int32_t frames_written = AAudioStream_write(audio_data->output_stream, buffer->data,
                                              frames_to_write, 0);
  
  if (frames_written < 0) {
    LOGE("AAudio write error: %s", AAudio_convertResultToText((aaudio_result_t)frames_written));
    return -1;
  }

  return frames_written;
}

static void aaudio_cleanup(ethervox_audio_runtime_t* runtime) {
  aaudio_data_t* audio_data = (aaudio_data_t*)runtime->platform_data;
  if (!audio_data) {
    return;
  }

  aaudio_stop_capture(runtime);
  aaudio_stop_playback(runtime);
  
  if (audio_data->capture_buffer) {
    free(audio_data->capture_buffer);
  }
  
  free(audio_data);
  runtime->platform_data = NULL;
  
  LOGI("AAudio cleanup completed");
}

#endif  // USE_AAUDIO

// ===========================================================================
// OpenSL ES Implementation (API 24-25)
// ===========================================================================
#ifdef USE_OPENSL_ES

typedef struct {
  SLObjectItf engine_object;
  SLEngineItf engine_engine;
  SLObjectItf recorder_object;
  SLRecordItf recorder_record;
  SLAndroidSimpleBufferQueueItf recorder_buffer_queue;
  SLObjectItf player_object;
  SLPlayItf player_play;
  SLAndroidSimpleBufferQueueItf player_buffer_queue;
  bool is_recording;
  bool is_playing;
  float* capture_buffer;
  float* playback_buffer;
  size_t buffer_size;
  ethervox_audio_runtime_t* runtime;
} opensl_data_t;

static void opensl_recorder_callback(SLAndroidSimpleBufferQueueItf bq, void* context) {
  opensl_data_t* data = (opensl_data_t*)context;
  if (!data || !data->runtime) {
    return;
  }

  if (data->runtime->on_audio_data) {
    ethervox_audio_buffer_t buffer;
    buffer.data = data->capture_buffer;
    buffer.size = (uint32_t)data->buffer_size;
    buffer.channels = data->runtime->config.channels;
    
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    buffer.timestamp_us = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
    
    data->runtime->on_audio_data(&buffer, data->runtime->user_data);
  }

  // Re-enqueue the buffer
  (*bq)->Enqueue(bq, data->capture_buffer, 
                 data->buffer_size * sizeof(float) * data->runtime->config.channels);
}

static int opensl_init(ethervox_audio_runtime_t* runtime,
                       const ethervox_audio_config_t* config) {
  opensl_data_t* audio_data = (opensl_data_t*)calloc(1, sizeof(opensl_data_t));
  if (!audio_data) {
    LOGE("Failed to allocate OpenSL ES data");
    return -1;
  }

  runtime->platform_data = audio_data;
  audio_data->runtime = runtime;
  audio_data->buffer_size = config->buffer_size;
  
  audio_data->capture_buffer = (float*)malloc(config->buffer_size * sizeof(float) * config->channels);
  audio_data->playback_buffer = (float*)malloc(config->buffer_size * sizeof(float) * config->channels);
  
  if (!audio_data->capture_buffer || !audio_data->playback_buffer) {
    LOGE("Failed to allocate audio buffers");
    free(audio_data->capture_buffer);
    free(audio_data->playback_buffer);
    free(audio_data);
    return -1;
  }

  // Create OpenSL ES engine
  SLresult result = slCreateEngine(&audio_data->engine_object, 0, NULL, 0, NULL, NULL);
  if (result != SL_RESULT_SUCCESS) {
    LOGE("Failed to create OpenSL ES engine: %d", result);
    free(audio_data->capture_buffer);
    free(audio_data->playback_buffer);
    free(audio_data);
    return -1;
  }

  result = (*audio_data->engine_object)->Realize(audio_data->engine_object, SL_BOOLEAN_FALSE);
  if (result != SL_RESULT_SUCCESS) {
    LOGE("Failed to realize OpenSL ES engine: %d", result);
    (*audio_data->engine_object)->Destroy(audio_data->engine_object);
    free(audio_data->capture_buffer);
    free(audio_data->playback_buffer);
    free(audio_data);
    return -1;
  }

  result = (*audio_data->engine_object)->GetInterface(audio_data->engine_object, 
                                                       SL_IID_ENGINE,
                                                       &audio_data->engine_engine);
  if (result != SL_RESULT_SUCCESS) {
    LOGE("Failed to get OpenSL ES engine interface: %d", result);
    (*audio_data->engine_object)->Destroy(audio_data->engine_object);
    free(audio_data->capture_buffer);
    free(audio_data->playback_buffer);
    free(audio_data);
    return -1;
  }

  LOGI("OpenSL ES driver initialized (API 24-25)");
  return 0;
}

static int opensl_start_capture(ethervox_audio_runtime_t* runtime) {
  opensl_data_t* audio_data = (opensl_data_t*)runtime->platform_data;
  if (!audio_data) {
    LOGE("OpenSL ES data not initialized");
    return -1;
  }

  SLDataLocator_IODevice loc_dev = {
    SL_DATALOCATOR_IODEVICE,
    SL_IODEVICE_AUDIOINPUT,
    SL_DEFAULTDEVICEID_AUDIOINPUT,
    NULL
  };
  
  SLDataSource audio_src = {&loc_dev, NULL};

  SLDataLocator_AndroidSimpleBufferQueue loc_bq = {
    SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
    2
  };

  SLDataFormat_PCM format_pcm = {
    SL_DATAFORMAT_PCM,
    runtime->config.channels,
    runtime->config.sample_rate * 1000,  // milliHz
    SL_PCMSAMPLEFORMAT_FIXED_16,
    SL_PCMSAMPLEFORMAT_FIXED_16,
    runtime->config.channels == 1 ? SL_SPEAKER_FRONT_CENTER : 
                                    (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT),
    SL_BYTEORDER_LITTLEENDIAN
  };

  SLDataSink audio_snk = {&loc_bq, &format_pcm};

  const SLInterfaceID ids[1] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
  const SLboolean req[1] = {SL_BOOLEAN_TRUE};

  SLresult result = (*audio_data->engine_engine)->CreateAudioRecorder(
      audio_data->engine_engine,
      &audio_data->recorder_object,
      &audio_src,
      &audio_snk,
      1,
      ids,
      req);

  if (result != SL_RESULT_SUCCESS) {
    LOGE("Failed to create audio recorder: %d", result);
    return -1;
  }

  result = (*audio_data->recorder_object)->Realize(audio_data->recorder_object, SL_BOOLEAN_FALSE);
  if (result != SL_RESULT_SUCCESS) {
    LOGE("Failed to realize audio recorder: %d", result);
    (*audio_data->recorder_object)->Destroy(audio_data->recorder_object);
    audio_data->recorder_object = NULL;
    return -1;
  }

  result = (*audio_data->recorder_object)->GetInterface(audio_data->recorder_object,
                                                         SL_IID_RECORD,
                                                         &audio_data->recorder_record);
  if (result != SL_RESULT_SUCCESS) {
    LOGE("Failed to get recorder interface: %d", result);
    (*audio_data->recorder_object)->Destroy(audio_data->recorder_object);
    audio_data->recorder_object = NULL;
    return -1;
  }

  result = (*audio_data->recorder_object)->GetInterface(audio_data->recorder_object,
                                                         SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                                         &audio_data->recorder_buffer_queue);
  if (result != SL_RESULT_SUCCESS) {
    LOGE("Failed to get recorder buffer queue: %d", result);
    (*audio_data->recorder_object)->Destroy(audio_data->recorder_object);
    audio_data->recorder_object = NULL;
    return -1;
  }

  result = (*audio_data->recorder_buffer_queue)->RegisterCallback(
      audio_data->recorder_buffer_queue,
      opensl_recorder_callback,
      audio_data);
  if (result != SL_RESULT_SUCCESS) {
    LOGE("Failed to register recorder callback: %d", result);
    (*audio_data->recorder_object)->Destroy(audio_data->recorder_object);
    audio_data->recorder_object = NULL;
    return -1;
  }

  // Enqueue initial buffers
  result = (*audio_data->recorder_buffer_queue)->Enqueue(
      audio_data->recorder_buffer_queue,
      audio_data->capture_buffer,
      audio_data->buffer_size * sizeof(float) * runtime->config.channels);
  if (result != SL_RESULT_SUCCESS) {
    LOGE("Failed to enqueue initial buffer: %d", result);
    (*audio_data->recorder_object)->Destroy(audio_data->recorder_object);
    audio_data->recorder_object = NULL;
    return -1;
  }

  result = (*audio_data->recorder_record)->SetRecordState(audio_data->recorder_record,
                                                           SL_RECORDSTATE_RECORDING);
  if (result != SL_RESULT_SUCCESS) {
    LOGE("Failed to start recording: %d", result);
    (*audio_data->recorder_object)->Destroy(audio_data->recorder_object);
    audio_data->recorder_object = NULL;
    return -1;
  }

  audio_data->is_recording = true;
  LOGI("OpenSL ES capture started");
  return 0;
}

static int opensl_stop_capture(ethervox_audio_runtime_t* runtime) {
  opensl_data_t* audio_data = (opensl_data_t*)runtime->platform_data;
  if (!audio_data || !audio_data->recorder_object) {
    return 0;
  }

  if (audio_data->recorder_record) {
    (*audio_data->recorder_record)->SetRecordState(audio_data->recorder_record,
                                                    SL_RECORDSTATE_STOPPED);
  }

  (*audio_data->recorder_object)->Destroy(audio_data->recorder_object);
  audio_data->recorder_object = NULL;
  audio_data->recorder_record = NULL;
  audio_data->recorder_buffer_queue = NULL;
  audio_data->is_recording = false;

  LOGI("OpenSL ES capture stopped");
  return 0;
}

static int opensl_start_playback(ethervox_audio_runtime_t* runtime) {
  opensl_data_t* audio_data = (opensl_data_t*)runtime->platform_data;
  if (!audio_data) {
    LOGE("OpenSL ES data not initialized");
    return -1;
  }

  // Create output mix
  SLObjectItf output_mix_object;
  SLresult result = (*audio_data->engine_engine)->CreateOutputMix(
      audio_data->engine_engine,
      &output_mix_object,
      0,
      NULL,
      NULL);

  if (result != SL_RESULT_SUCCESS) {
    LOGE("Failed to create output mix: %d", result);
    return -1;
  }

  result = (*output_mix_object)->Realize(output_mix_object, SL_BOOLEAN_FALSE);
  if (result != SL_RESULT_SUCCESS) {
    LOGE("Failed to realize output mix: %d", result);
    (*output_mix_object)->Destroy(output_mix_object);
    return -1;
  }

  SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {
    SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
    2
  };

  SLDataFormat_PCM format_pcm = {
    SL_DATAFORMAT_PCM,
    runtime->config.channels,
    runtime->config.sample_rate * 1000,
    SL_PCMSAMPLEFORMAT_FIXED_16,
    SL_PCMSAMPLEFORMAT_FIXED_16,
    runtime->config.channels == 1 ? SL_SPEAKER_FRONT_CENTER :
                                    (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT),
    SL_BYTEORDER_LITTLEENDIAN
  };

  SLDataSource audio_src = {&loc_bufq, &format_pcm};

  SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX, output_mix_object};
  SLDataSink audio_snk = {&loc_outmix, NULL};

  const SLInterfaceID ids[1] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
  const SLboolean req[1] = {SL_BOOLEAN_TRUE};

  result = (*audio_data->engine_engine)->CreateAudioPlayer(
      audio_data->engine_engine,
      &audio_data->player_object,
      &audio_src,
      &audio_snk,
      1,
      ids,
      req);

  if (result != SL_RESULT_SUCCESS) {
    LOGE("Failed to create audio player: %d", result);
    (*output_mix_object)->Destroy(output_mix_object);
    return -1;
  }

  result = (*audio_data->player_object)->Realize(audio_data->player_object, SL_BOOLEAN_FALSE);
  if (result != SL_RESULT_SUCCESS) {
    LOGE("Failed to realize audio player: %d", result);
    (*audio_data->player_object)->Destroy(audio_data->player_object);
    (*output_mix_object)->Destroy(output_mix_object);
    audio_data->player_object = NULL;
    return -1;
  }

  result = (*audio_data->player_object)->GetInterface(audio_data->player_object,
                                                       SL_IID_PLAY,
                                                       &audio_data->player_play);
  if (result != SL_RESULT_SUCCESS) {
    LOGE("Failed to get player interface: %d", result);
    (*audio_data->player_object)->Destroy(audio_data->player_object);
    (*output_mix_object)->Destroy(output_mix_object);
    audio_data->player_object = NULL;
    return -1;
  }

  result = (*audio_data->player_object)->GetInterface(audio_data->player_object,
                                                       SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                                       &audio_data->player_buffer_queue);
  if (result != SL_RESULT_SUCCESS) {
    LOGE("Failed to get player buffer queue: %d", result);
    (*audio_data->player_object)->Destroy(audio_data->player_object);
    (*output_mix_object)->Destroy(output_mix_object);
    audio_data->player_object = NULL;
    return -1;
  }

  result = (*audio_data->player_play)->SetPlayState(audio_data->player_play, SL_PLAYSTATE_PLAYING);
  if (result != SL_RESULT_SUCCESS) {
    LOGE("Failed to start playback: %d", result);
    (*audio_data->player_object)->Destroy(audio_data->player_object);
    (*output_mix_object)->Destroy(output_mix_object);
    audio_data->player_object = NULL;
    return -1;
  }

  audio_data->is_playing = true;
  LOGI("OpenSL ES playback started");
  return 0;
}

static int opensl_stop_playback(ethervox_audio_runtime_t* runtime) {
  opensl_data_t* audio_data = (opensl_data_t*)runtime->platform_data;
  if (!audio_data || !audio_data->player_object) {
    return 0;
  }

  if (audio_data->player_play) {
    (*audio_data->player_play)->SetPlayState(audio_data->player_play, SL_PLAYSTATE_STOPPED);
  }

  (*audio_data->player_object)->Destroy(audio_data->player_object);
  audio_data->player_object = NULL;
  audio_data->player_play = NULL;
  audio_data->player_buffer_queue = NULL;
  audio_data->is_playing = false;

  LOGI("OpenSL ES playback stopped");
  return 0;
}

static int opensl_read_audio(ethervox_audio_runtime_t* runtime, ethervox_audio_buffer_t* buffer) {
  // OpenSL ES uses callback-based audio, so direct reading is not typical
  // This would need to be implemented with a ring buffer if synchronous reads are needed
  (void)runtime;
  (void)buffer;
  LOGD("Direct read not implemented for OpenSL ES (use callbacks)");
  return -1;
}

static int opensl_write_audio(ethervox_audio_runtime_t* runtime,
                              const ethervox_audio_buffer_t* buffer) {
  opensl_data_t* audio_data = (opensl_data_t*)runtime->platform_data;
  if (!audio_data || !audio_data->player_buffer_queue || !buffer) {
    return -1;
  }

  SLresult result = (*audio_data->player_buffer_queue)->Enqueue(
      audio_data->player_buffer_queue,
      buffer->data,
      buffer->size * sizeof(float));

  if (result != SL_RESULT_SUCCESS) {
    LOGE("Failed to enqueue playback buffer: %d", result);
    return -1;
  }

  return (int)buffer->size;
}

static void opensl_cleanup(ethervox_audio_runtime_t* runtime) {
  opensl_data_t* audio_data = (opensl_data_t*)runtime->platform_data;
  if (!audio_data) {
    return;
  }

  opensl_stop_capture(runtime);
  opensl_stop_playback(runtime);

  if (audio_data->engine_object) {
    (*audio_data->engine_object)->Destroy(audio_data->engine_object);
  }

  if (audio_data->capture_buffer) {
    free(audio_data->capture_buffer);
  }
  if (audio_data->playback_buffer) {
    free(audio_data->playback_buffer);
  }

  free(audio_data);
  runtime->platform_data = NULL;

  LOGI("OpenSL ES cleanup completed");
}

#endif  // USE_OPENSL_ES

// ===========================================================================
// Platform Driver Registration
// ===========================================================================

int ethervox_audio_register_platform_driver(ethervox_audio_runtime_t* runtime) {
  if (!runtime) {
    return -1;
  }

#ifdef USE_AAUDIO
  runtime->driver.init = aaudio_init;
  runtime->driver.start_capture = aaudio_start_capture;
  runtime->driver.stop_capture = aaudio_stop_capture;
  runtime->driver.start_playback = aaudio_start_playback;
  runtime->driver.stop_playback = aaudio_stop_playback;
  runtime->driver.read_audio = aaudio_read_audio;
  runtime->driver.write_audio = aaudio_write_audio;
  runtime->driver.cleanup = aaudio_cleanup;
  LOGI("Registered AAudio driver (API 26+)");
#elif defined(USE_OPENSL_ES)
  runtime->driver.init = opensl_init;
  runtime->driver.start_capture = opensl_start_capture;
  runtime->driver.stop_capture = opensl_stop_capture;
  runtime->driver.start_playback = opensl_start_playback;
  runtime->driver.stop_playback = opensl_stop_playback;
  runtime->driver.read_audio = opensl_read_audio;
  runtime->driver.write_audio = opensl_write_audio;
  runtime->driver.cleanup = opensl_cleanup;
  LOGI("Registered OpenSL ES driver (API 24-25)");
#else
#error "No audio driver available for this Android API level"
#endif

  return 0;
}

#endif  // __ANDROID__
