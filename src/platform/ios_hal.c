/**
 * @file ios_hal.c
 * @brief iOS Hardware Abstraction Layer implementation for EthervoxAI
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

#if defined(__APPLE__) && (TARGET_OS_IOS || TARGET_OS_MACCATALYST)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/time.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <sys/sysctl.h>

#include "ethervox/platform.h"
#include "ethervox/config.h"
#include "ethervox/error.h"

#define LOGD(...) ETHERVOX_LOGD(__VA_ARGS__)
#define LOGE(...) ETHERVOX_LOGE(__VA_ARGS__)
#define LOGI(...) ETHERVOX_LOGI(__VA_ARGS__)

// iOS platform-specific data
typedef struct {
    uint64_t boot_time_us;
    char device_model[64];
    char ios_version[32];
    uint32_t ram_size_mb;
} ios_platform_data_t;

static ethervox_result_t ios_init(ethervox_platform_info_t* info) {
    LOGI("Initializing iOS platform HAL");

    ios_platform_data_t* platform_data = 
        (ios_platform_data_t*)calloc(1, sizeof(ios_platform_data_t));
    if (!platform_data) {
        ETHERVOX_RETURN_ERROR(ETHERVOX_ERROR_OUT_OF_MEMORY, "Failed to allocate platform data");
    }

    info->platform_specific_data = platform_data;

    // Get boot time
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    platform_data->boot_time_us = (uint64_t)ts.tv_sec * 1000000ULL + 
                                   (uint64_t)ts.tv_nsec / 1000ULL;

    // Set platform name
#if TARGET_OS_MACCATALYST
    snprintf(info->platform_name, ETHERVOX_PLATFORM_NAME_LEN, "Mac Catalyst");
#else
    snprintf(info->platform_name, ETHERVOX_PLATFORM_NAME_LEN, "iOS");
#endif

    // Get iOS version (will be populated by Swift bridge if needed)
    snprintf(platform_data->ios_version, sizeof(platform_data->ios_version), "iOS");

    // Get CPU info
    size_t len;
    
    // CPU model
    char cpu_brand[128] = {0};
    len = sizeof(cpu_brand);
    if (sysctlbyname("machdep.cpu.brand_string", cpu_brand, &len, NULL, 0) == 0) {
        snprintf(info->cpu_model, ETHERVOX_PLATFORM_CPU_MODEL_LEN, "%s", cpu_brand);
    } else {
        snprintf(info->cpu_model, ETHERVOX_PLATFORM_CPU_MODEL_LEN, "Apple ARM64");
    }

    // CPU frequency
    uint64_t cpu_freq = 0;
    len = sizeof(cpu_freq);
    if (sysctlbyname("hw.cpufrequency", &cpu_freq, &len, NULL, 0) == 0) {
        info->cpu_frequency_mhz = (uint32_t)(cpu_freq / 1000000);
    } else {
        info->cpu_frequency_mhz = 0;
    }

    // Core count
    int core_count = 0;
    len = sizeof(core_count);
    if (sysctlbyname("hw.ncpu", &core_count, &len, NULL, 0) == 0) {
        info->core_count = (uint32_t)core_count;
    } else {
        info->core_count = 1;
    }

    // Get physical memory
    uint64_t ram_size = 0;
    len = sizeof(ram_size);
    if (sysctlbyname("hw.memsize", &ram_size, &len, NULL, 0) == 0) {
        platform_data->ram_size_mb = (uint32_t)(ram_size / (1024 * 1024));
        info->capabilities.ram_size_mb = platform_data->ram_size_mb;
    } else {
        platform_data->ram_size_mb = 0;
        info->capabilities.ram_size_mb = 0;
    }

    // Set capabilities
    info->capabilities.has_audio_input = true;
    info->capabilities.has_audio_output = true;
    info->capabilities.has_microphone_array = false;  // Typically single mic on iOS
    info->capabilities.has_gpio = false;
    info->capabilities.has_spi = false;
    info->capabilities.has_i2c = false;
    info->capabilities.has_uart = false;
    info->capabilities.has_wifi = true;
    info->capabilities.has_bluetooth = true;
    info->capabilities.has_ethernet = false;
    info->capabilities.has_display = true;
    info->capabilities.has_camera = true;
    info->capabilities.max_audio_channels = 2;  // Stereo
    info->capabilities.max_sample_rate = 48000;
    info->capabilities.gpio_pin_count = 0;
    info->capabilities.flash_size_mb = 0;  // Would need Swift bridge to get storage info

    LOGI("iOS HAL initialized: %s, %u cores, %u MB RAM",
         info->cpu_model, info->core_count, info->capabilities.ram_size_mb);

    return ETHERVOX_SUCCESS;
}

static void ios_cleanup(ethervox_platform_info_t* info) {
    LOGI("Cleaning up iOS platform HAL");
    
    if (info && info->platform_specific_data) {
        free(info->platform_specific_data);
        info->platform_specific_data = NULL;
    }
}

// GPIO functions not available on iOS - return error
static ethervox_result_t ios_gpio_configure(uint32_t pin, ethervox_gpio_mode_t mode) {
    (void)pin;
    (void)mode;
    ETHERVOX_RETURN_ERROR(ETHERVOX_ERROR_NOT_SUPPORTED, "GPIO not supported on iOS");
}

static ethervox_result_t ios_gpio_write(uint32_t pin, bool state) {
    (void)pin;
    (void)state;
    ETHERVOX_RETURN_ERROR(ETHERVOX_ERROR_NOT_SUPPORTED, "GPIO not supported on iOS");
}

static bool ios_gpio_read(uint32_t pin) {
    (void)pin;
    return false;
}

// I2C functions not available on iOS - return error
static ethervox_result_t ios_i2c_write(uint32_t bus, uint8_t device_addr, const uint8_t* data, uint32_t len) {
    (void)bus;
    (void)device_addr;
    (void)data;
    (void)len;
    ETHERVOX_RETURN_ERROR(ETHERVOX_ERROR_NOT_SUPPORTED, "I2C not supported on iOS");
}

static ethervox_result_t ios_i2c_read(uint32_t bus, uint8_t device_addr, uint8_t* data, uint32_t len) {
    (void)bus;
    (void)device_addr;
    (void)data;
    (void)len;
    ETHERVOX_RETURN_ERROR(ETHERVOX_ERROR_NOT_SUPPORTED, "I2C not supported on iOS");
}

// SPI functions not available on iOS - return error
static ethervox_result_t ios_spi_transfer(uint32_t bus, const uint8_t* tx_data, uint8_t* rx_data, uint32_t len) {
    (void)bus;
    (void)tx_data;
    (void)rx_data;
    (void)len;
    ETHERVOX_RETURN_ERROR(ETHERVOX_ERROR_NOT_SUPPORTED, "SPI not supported on iOS");
}

// Timing functions
static uint32_t ios_delay_ms(uint32_t ms) {
    usleep(ms * 1000);
    return ms;
}

static uint32_t ios_delay_us(uint32_t us) {
    usleep(us);
    return us;
}

static uint64_t ios_get_timestamp_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

// System control functions
static void ios_reset(void) {
    LOGI("System reset not supported on iOS (sandboxed environment)");
    // iOS doesn't allow app-initiated system reset
}

static void ios_enter_sleep_mode(ethervox_sleep_mode_t mode) {
    // iOS manages power automatically, apps can't directly control sleep
    switch (mode) {
        case ETHERVOX_SLEEP_LIGHT:
            usleep(100000);  // 100ms delay
            break;
        case ETHERVOX_SLEEP_DEEP:
            usleep(1000000);  // 1 second delay
            break;
        default:
            break;
    }
}

static uint32_t ios_get_free_heap_size(void) {
    mach_port_t host_port = mach_host_self();
    mach_msg_type_number_t host_size = sizeof(vm_statistics_data_t) / sizeof(integer_t);
    vm_size_t page_size;
    vm_statistics_data_t vm_stat;

    host_page_size(host_port, &page_size);

    if (host_statistics(host_port, HOST_VM_INFO, (host_info_t)&vm_stat, &host_size) == KERN_SUCCESS) {
        uint64_t free_mem = (uint64_t)vm_stat.free_count * page_size;
        return (uint32_t)(free_mem / 1024);  // Return KB
    }

    return 0;
}

static uint32_t ios_get_battery_voltage_mv(void) {
    // Battery voltage monitoring requires Swift/Objective-C bridge
    // Return 0 to indicate unavailable
    return 0;
}

// Platform HAL interface
static const ethervox_platform_hal_t ios_hal = {
    .init = ios_init,
    .cleanup = ios_cleanup,
    .gpio_configure = ios_gpio_configure,
    .gpio_write = ios_gpio_write,
    .gpio_read = ios_gpio_read,
    .i2c_write = ios_i2c_write,
    .i2c_read = ios_i2c_read,
    .spi_transfer = ios_spi_transfer,
    .delay_ms = ios_delay_ms,
    .delay_us = ios_delay_us,
    .get_timestamp_us = ios_get_timestamp_us,
    .reset = ios_reset,
    .enter_sleep_mode = ios_enter_sleep_mode,
    .get_free_heap_size = ios_get_free_heap_size,
    .get_battery_voltage_mv = ios_get_battery_voltage_mv
};

const ethervox_platform_hal_t* ethervox_platform_get_ios_hal(void) {
    return &ios_hal;
}

#endif // __APPLE__ && (TARGET_OS_IOS || TARGET_OS_MACCATALYST)
