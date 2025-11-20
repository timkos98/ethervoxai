/**
 * @file android_hal.c
 * @brief Android Hardware Abstraction Layer implementation for EthervoxAI
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

#if defined(__ANDROID__)

#include <android/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/sysinfo.h>

#include "ethervox/platform.h"
#include "ethervox/config.h"

#define LOGD(...) ETHERVOX_LOGD(__VA_ARGS__)
#define LOGE(...) ETHERVOX_LOGE(__VA_ARGS__)
#define LOGI(...) ETHERVOX_LOGI(__VA_ARGS__)

#define LOG_TAG "EthervoxHAL"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

// Android platform-specific data
typedef struct {
  uint64_t boot_time_us;
  char device_model[64];
  char android_version[32];
  int api_level;
} android_platform_data_t;

static int android_init(ethervox_platform_info_t* info) {
  LOGI("Initializing Android platform HAL");

  android_platform_data_t* platform_data = 
      (android_platform_data_t*)calloc(1, sizeof(android_platform_data_t));
  if (!platform_data) {
    LOGE("Failed to allocate platform data");
    return -1;
  }

  info->platform_specific_data = platform_data;

  // Get boot time
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  platform_data->boot_time_us = (uint64_t)ts.tv_sec * 1000000ULL + 
                                 (uint64_t)ts.tv_nsec / 1000ULL;

  // Set platform name
  snprintf(info->platform_name, ETHERVOX_PLATFORM_NAME_LEN, "Android");
  
  // Get Android API level
  platform_data->api_level = __ANDROID_API__;
  snprintf(platform_data->android_version, sizeof(platform_data->android_version),
           "API %d", platform_data->api_level);

  // Set CPU info (basic info, more detailed info would need JNI callbacks)
  snprintf(info->cpu_model, ETHERVOX_PLATFORM_CPU_MODEL_LEN, "ARM/x86");
  info->cpu_frequency_mhz = 0;  // Would need JNI to get actual frequency
  info->core_count = (uint32_t)sysconf(_SC_NPROCESSORS_CONF);

  // Set capabilities
  info->capabilities.has_audio_input = true;
  info->capabilities.has_audio_output = true;
  info->capabilities.has_microphone_array = false;  // Typically single mic
  info->capabilities.has_gpio = false;
  info->capabilities.has_spi = false;
  info->capabilities.has_i2c = false;
  info->capabilities.has_uart = false;
  info->capabilities.has_wifi = true;  // Most Android devices have WiFi
  info->capabilities.has_bluetooth = true;
  info->capabilities.has_ethernet = false;
  info->capabilities.has_display = true;
  info->capabilities.has_camera = true;
  info->capabilities.max_audio_channels = 2;  // Stereo
  info->capabilities.max_sample_rate = 48000;
  info->capabilities.gpio_pin_count = 0;
  
  // Get memory info
  struct sysinfo si;
  if (sysinfo(&si) == 0) {
    info->capabilities.ram_size_mb = (uint32_t)(si.totalram / (1024 * 1024));
  } else {
    info->capabilities.ram_size_mb = 0;
  }
  info->capabilities.flash_size_mb = 0;  // Would need JNI to get storage info

  LOGI("Android HAL initialized: API %d, %u cores, %u MB RAM",
       platform_data->api_level, info->core_count, info->capabilities.ram_size_mb);
  
  return 0;
}

static void android_cleanup(ethervox_platform_info_t* info) {
  LOGI("Cleaning up Android platform HAL");
  
  if (info->platform_specific_data) {
    free(info->platform_specific_data);
    info->platform_specific_data = NULL;
  }
}

// GPIO functions - not available on Android
static int android_gpio_configure(uint32_t pin, ethervox_gpio_mode_t mode) {
  (void)pin;
  (void)mode;
  LOGD("GPIO not supported on Android");
  return -1;
}

static int android_gpio_write(uint32_t pin, bool state) {
  (void)pin;
  (void)state;
  return -1;
}

static bool android_gpio_read(uint32_t pin) {
  (void)pin;
  return false;
}

static int android_gpio_set_pwm(uint32_t pin, uint32_t duty_cycle) {
  (void)pin;
  (void)duty_cycle;
  return -1;
}

// I2C functions - not available on standard Android
static int android_i2c_init(uint32_t bus, uint32_t sda_pin, uint32_t scl_pin) {
  (void)bus;
  (void)sda_pin;
  (void)scl_pin;
  return -1;
}

static int android_i2c_write(uint32_t bus, uint8_t device_addr, 
                             const uint8_t* data, uint32_t len) {
  (void)bus;
  (void)device_addr;
  (void)data;
  (void)len;
  return -1;
}

static int android_i2c_read(uint32_t bus, uint8_t device_addr, 
                           uint8_t* data, uint32_t len) {
  (void)bus;
  (void)device_addr;
  (void)data;
  (void)len;
  return -1;
}

static void android_i2c_cleanup(uint32_t bus) {
  (void)bus;
}

// SPI functions - not available on standard Android
static int android_spi_init(uint32_t bus, uint32_t mosi_pin, uint32_t miso_pin,
                           uint32_t clk_pin, uint32_t cs_pin) {
  (void)bus;
  (void)mosi_pin;
  (void)miso_pin;
  (void)clk_pin;
  (void)cs_pin;
  return -1;
}

static int android_spi_transfer(uint32_t bus, const uint8_t* tx_data,
                               uint8_t* rx_data, uint32_t len) {
  (void)bus;
  (void)tx_data;
  (void)rx_data;
  (void)len;
  return -1;
}

static void android_spi_cleanup(uint32_t bus) {
  (void)bus;
}

// System operations
static void android_system_reset(void) {
  LOGD("System reset not supported on Android (requires privileged access)");
  // Would need JNI callback to request app restart
}

static void android_system_sleep(ethervox_sleep_mode_t mode) {
  (void)mode;
  LOGD("System sleep controlled by Android power manager");
  // Android handles power management automatically
}

static uint64_t android_get_timestamp_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static uint32_t android_get_free_memory(void) {
  struct sysinfo si;
  if (sysinfo(&si) == 0) {
    return (uint32_t)(si.freeram / (1024 * 1024));  // MB
  }
  return 0;
}

static float android_get_cpu_temperature(void) {
  // Reading CPU temperature on Android requires root or special permissions
  // Return default value
  LOGD("CPU temperature reading not available without privileged access");
  return ETHERVOX_PLATFORM_DEFAULT_TEMP_C;
}

static uint32_t android_get_free_heap_size(void) {
  struct sysinfo si;
  if (sysinfo(&si) == 0) {
    return (uint32_t)si.freeram;
  }
  return 0;
}

// Power management
static int android_set_cpu_frequency(uint32_t frequency_mhz) {
  (void)frequency_mhz;
  LOGD("CPU frequency control managed by Android");
  return -1;  // Not accessible from NDK
}

static int android_enable_power_saving(bool enable) {
  (void)enable;
  LOGD("Power saving mode managed by Android");
  return -1;  // Would need JNI callback
}

static float android_get_battery_voltage(void) {
  LOGD("Battery voltage requires JNI callback to BatteryManager");
  return 0.0f;  // Would need JNI callback
}

static uint32_t android_delay_us(uint32_t us) {
  usleep(us);
  return us;
}

static uint32_t android_delay_ms(uint32_t ms) {
  usleep(ms * 1000);
  return ms;
}

// Register Android HAL implementation
int ethervox_platform_hal_register_android(ethervox_platform_t* platform) {
  if (!platform) {
    LOGE("Invalid platform pointer");
    return -1;
  }

  // Initialize platform info
  if (android_init(&platform->info) != 0) {
    LOGE("Failed to initialize Android platform info");
    return -1;
  }

  // Register HAL function pointers
  platform->hal.init = android_init;
  platform->hal.cleanup = android_cleanup;
  
  // GPIO operations (not supported)
  platform->hal.gpio_configure = android_gpio_configure;
  platform->hal.gpio_write = android_gpio_write;
  platform->hal.gpio_read = android_gpio_read;
  platform->hal.gpio_set_pwm = android_gpio_set_pwm;
  
  // I2C operations (not supported)
  platform->hal.i2c_init = android_i2c_init;
  platform->hal.i2c_write = android_i2c_write;
  platform->hal.i2c_read = android_i2c_read;
  platform->hal.i2c_cleanup = android_i2c_cleanup;
  
  // SPI operations (not supported)
  platform->hal.spi_init = android_spi_init;
  platform->hal.spi_transfer = android_spi_transfer;
  platform->hal.spi_cleanup = android_spi_cleanup;
  
  // System operations
  platform->hal.system_reset = android_system_reset;
  platform->hal.system_sleep = android_system_sleep;
  platform->hal.get_timestamp_us = android_get_timestamp_us;
  platform->hal.get_free_memory = android_get_free_memory;
  platform->hal.get_cpu_temperature = android_get_cpu_temperature;
  platform->hal.get_free_heap_size = android_get_free_heap_size;
  
  // Power management
  platform->hal.set_cpu_frequency = android_set_cpu_frequency;
  platform->hal.enable_power_saving = android_enable_power_saving;
  platform->hal.get_battery_voltage = android_get_battery_voltage;
  platform->hal.delay_us = android_delay_us;
  platform->hal.delay_ms = android_delay_ms;

  platform->is_initialized = true;
  platform->boot_time = android_get_timestamp_us();

  LOGI("Android HAL registered successfully");
  return 0;
}

#endif  // __ANDROID__
