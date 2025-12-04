/**
 * @file platform_macos.c
 * @brief macOS-specific audio platform implementation for EthervoxAI
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

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <AudioToolbox/AudioToolbox.h>

#include "ethervox/audio.h"

#ifdef ETHERVOX_PLATFORM_MACOS

#define BUFFER_SIZE 4096
#define NUM_BUFFERS 3

typedef struct {
  AudioQueueRef queue;
  AudioQueueBufferRef buffers[NUM_BUFFERS];
  bool is_recording;
  bool is_playing;
  
  // Ring buffer for captured audio
  int16_t* ring_buffer;
  size_t ring_buffer_size;
  size_t write_pos;
  size_t read_pos;
  pthread_mutex_t lock;
  
  uint32_t sample_rate;
  uint8_t channels;
} macos_audio_state_t;

// Audio queue callback for input
static void input_callback(void* user_data, AudioQueueRef queue,
                          AudioQueueBufferRef buffer,
                          const AudioTimeStamp* start_time,
                          UInt32 num_packets,
                          const AudioStreamPacketDescription* packet_desc) {
  (void)queue;
  (void)start_time;
  (void)packet_desc;
  
  macos_audio_state_t* state = (macos_audio_state_t*)user_data;
  if (!state || !state->is_recording) {
    return;
  }
  
  // Copy audio data to ring buffer
  int16_t* samples = (int16_t*)buffer->mAudioData;
  size_t sample_count = buffer->mAudioDataByteSize / sizeof(int16_t);
  
  pthread_mutex_lock(&state->lock);
  
  for (size_t i = 0; i < sample_count; i++) {
    state->ring_buffer[state->write_pos] = samples[i];
    state->write_pos = (state->write_pos + 1) % state->ring_buffer_size;
    
    // Overwrite old data if buffer full
    if (state->write_pos == state->read_pos) {
      state->read_pos = (state->read_pos + 1) % state->ring_buffer_size;
    }
  }
  
  pthread_mutex_unlock(&state->lock);
  
  // Re-enqueue buffer for more recording
  AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
}

static int macos_audio_init(ethervox_audio_runtime_t* runtime,
                            const ethervox_audio_config_t* config) {
  macos_audio_state_t* state = (macos_audio_state_t*)calloc(1, sizeof(macos_audio_state_t));
  if (!state) {
    return -1;
  }
  
  state->sample_rate = config->sample_rate ? config->sample_rate : 16000;
  state->channels = config->channels ? config->channels : 1;
  
  // Allocate ring buffer (10 seconds of audio)
  state->ring_buffer_size = state->sample_rate * 10;
  state->ring_buffer = (int16_t*)calloc(state->ring_buffer_size, sizeof(int16_t));
  if (!state->ring_buffer) {
    free(state);
    return -1;
  }
  
  pthread_mutex_init(&state->lock, NULL);

  runtime->platform_data = state;
  printf("macOS CoreAudio driver initialized (%u Hz, %u ch)\n", 
         state->sample_rate, state->channels);
  return 0;
}

static int macos_audio_start_capture(ethervox_audio_runtime_t* runtime) {
  macos_audio_state_t* state = (macos_audio_state_t*)runtime->platform_data;
  if (!state) {
    return -1;
  }
  
  if (state->is_recording) {
    return 0; // Already recording
  }
  
  // Configure audio format
  AudioStreamBasicDescription format = {0};
  format.mSampleRate = state->sample_rate;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
  format.mBitsPerChannel = 16;
  format.mChannelsPerFrame = state->channels;
  format.mBytesPerFrame = state->channels * sizeof(int16_t);
  format.mFramesPerPacket = 1;
  format.mBytesPerPacket = format.mBytesPerFrame;
  
  // Create audio queue
  OSStatus status = AudioQueueNewInput(&format, input_callback, state,
                                       NULL, kCFRunLoopCommonModes, 0,
                                       &state->queue);
  if (status != noErr) {
    fprintf(stderr, "Failed to create audio input queue: %d\n", status);
    return -1;
  }
  
  // Allocate and enqueue buffers
  for (int i = 0; i < NUM_BUFFERS; i++) {
    status = AudioQueueAllocateBuffer(state->queue, BUFFER_SIZE, &state->buffers[i]);
    if (status != noErr) {
      fprintf(stderr, "Failed to allocate audio buffer %d: %d\n", i, status);
      AudioQueueDispose(state->queue, true);
      state->queue = NULL;
      return -1;
    }
    AudioQueueEnqueueBuffer(state->queue, state->buffers[i], 0, NULL);
  }
  
  // Start recording
  status = AudioQueueStart(state->queue, NULL);
  if (status != noErr) {
    fprintf(stderr, "Failed to start audio queue: %d\n", status);
    AudioQueueDispose(state->queue, true);
    state->queue = NULL;
    return -1;
  }
  
  state->is_recording = true;
  printf("🎤 Started microphone capture\n");
  return 0;
}

static int macos_audio_stop_capture(ethervox_audio_runtime_t* runtime) {
  macos_audio_state_t* state = (macos_audio_state_t*)runtime->platform_data;
  if (!state || !state->is_recording) {
    return -1;
  }

  // Stop and dispose queue
  if (state->queue) {
    AudioQueueStop(state->queue, true);
    AudioQueueDispose(state->queue, true);
    state->queue = NULL;
  }
  
  state->is_recording = false;
  printf("⏹️  Stopped microphone capture\n");
  return 0;
}

static int macos_audio_start_playback(ethervox_audio_runtime_t* runtime) {
  macos_audio_state_t* state = (macos_audio_state_t*)runtime->platform_data;
  if (!state) {
    return -1;
  }

  state->is_playing = true;
  printf("macOS audio playback requested (not implemented)\n");
  return -1;
}

static int macos_audio_stop_playback(ethervox_audio_runtime_t* runtime) {
  macos_audio_state_t* state = (macos_audio_state_t*)runtime->platform_data;
  if (!state) {
    return -1;
  }

  state->is_playing = false;
  return 0;
}

static int macos_audio_read(ethervox_audio_runtime_t* runtime, ethervox_audio_buffer_t* buffer) {
  macos_audio_state_t* state = (macos_audio_state_t*)runtime->platform_data;
  if (!state || !buffer) {
    return -1;
  }
  
  pthread_mutex_lock(&state->lock);
  
  // Calculate available samples
  size_t available;
  if (state->write_pos >= state->read_pos) {
    available = state->write_pos - state->read_pos;
  } else {
    available = state->ring_buffer_size - state->read_pos + state->write_pos;
  }
  
  if (available == 0) {
    pthread_mutex_unlock(&state->lock);
    return 0; // No data available
  }
  
  // Read up to buffer capacity
  size_t to_read = (available < buffer->size) ? available : buffer->size;
  
  for (size_t i = 0; i < to_read; i++) {
    // Convert int16 to float32 normalized to [-1.0, 1.0]
    buffer->data[i] = (float)state->ring_buffer[state->read_pos] / 32768.0f;
    state->read_pos = (state->read_pos + 1) % state->ring_buffer_size;
  }
  
  // NOTE: Do NOT overwrite buffer->size - it's the capacity, not the actual count
  // The return value indicates how many samples were actually read
  
  pthread_mutex_unlock(&state->lock);
  
  return to_read;
}

static void macos_audio_cleanup(ethervox_audio_runtime_t* runtime) {
  if (!runtime || !runtime->platform_data) {
    return;
  }
  
  macos_audio_state_t* state = (macos_audio_state_t*)runtime->platform_data;
  
  // Stop capture if active
  if (state->is_recording && state->queue) {
    AudioQueueStop(state->queue, true);
    AudioQueueDispose(state->queue, true);
  }
  
  pthread_mutex_destroy(&state->lock);
  free(state->ring_buffer);
  free(state);
  runtime->platform_data = NULL;
  printf("macOS CoreAudio driver cleaned up\n");
}

int ethervox_audio_register_platform_driver(ethervox_audio_runtime_t* runtime) {
  if (!runtime) {
    return -1;
  }

  runtime->driver.init = macos_audio_init;
  runtime->driver.start_capture = macos_audio_start_capture;
  runtime->driver.stop_capture = macos_audio_stop_capture;
  runtime->driver.start_playback = macos_audio_start_playback;
  runtime->driver.stop_playback = macos_audio_stop_playback;
  runtime->driver.read_audio = macos_audio_read;
  runtime->driver.cleanup = macos_audio_cleanup;

  return 0;
}

#endif  // ETHERVOX_PLATFORM_MACOS
