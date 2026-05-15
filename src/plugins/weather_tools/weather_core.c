/**
 * @file weather_core.c
 * @brief Weather forecast core implementation using Open-Meteo API
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * Licensed under CC BY-NC-SA 4.0
 * For full license terms, see: https://creativecommons.org/licenses/by-nc-sa/4.0/
 * 
 * Weather data provided by Open-Meteo (MIT License)
 * API: https://open-meteo.com
 */

#include "ethervox/weather_tools.h"
#include "ethervox/logging.h"
#include "ethervox/error.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#if HAVE_LIBCURL
#include <curl/curl.h>
#else
#error "Weather tools require libcurl support. Install libcurl development packages."
#endif

#include "cJSON.h"

// ============================================================================
// Constants
// ============================================================================

#define OPENMETEO_FORECAST_URL "https://api.open-meteo.com/v1/forecast"
#define OPENMETEO_GEOCODING_URL "https://geocoding-api.open-meteo.com/v1/search"
#define DEFAULT_USER_AGENT "EthervoxAI/1.0 (Weather Tool)"

// ============================================================================
// Module State
// ============================================================================

static ethervox_weather_config_t g_config = {
    .enable_cache = true,
    .cache_ttl_sec = ETHERVOX_WEATHER_CACHE_TTL_SEC,
    .max_cache_entries = 100,
    .request_timeout_sec = ETHERVOX_WEATHER_API_TIMEOUT_SEC,
    .max_retries = ETHERVOX_WEATHER_MAX_RETRIES,
    .user_agent = DEFAULT_USER_AGENT
};

static bool g_initialized = false;

// ============================================================================
// Forward Declarations
// ============================================================================

// Cache functions (implemented in weather_cache.c)
extern ethervox_weather_response_t* weather_cache_get(const ethervox_weather_request_t* request);
extern void weather_cache_put(const ethervox_weather_request_t* request, const ethervox_weather_response_t* response);
extern void weather_cache_clear(void);
extern void weather_cache_get_stats(uint64_t* hit_count, uint64_t* miss_count, size_t* size);

// ============================================================================
// WMO Weather Code Conversion
// ============================================================================

/**
 * @brief Convert WMO weather code to human-readable condition
 * 
 * WMO Code Table 4677
 * See: https://www.nodc.noaa.gov/archive/arc0021/0002199/1.1/data/0-data/HTML/WMO-CODE/WMO4677.HTM
 */
static const char* wmo_code_to_condition(int code) {
    switch (code) {
        case 0: return "Clear sky";
        case 1: return "Mainly clear";
        case 2: return "Partly cloudy";
        case 3: return "Overcast";
        case 45: case 48: return "Foggy";
        case 51: case 53: case 55: return "Drizzle";
        case 56: case 57: return "Freezing drizzle";
        case 61: case 63: case 65: return "Rain";
        case 66: case 67: return "Freezing rain";
        case 71: case 73: case 75: return "Snow";
        case 77: return "Snow grains";
        case 80: case 81: case 82: return "Rain showers";
        case 85: case 86: return "Snow showers";
        case 95: return "Thunderstorm";
        case 96: case 99: return "Thunderstorm with hail";
        default: return "Unknown";
    }
}

// ============================================================================
// HTTP Request Handling
// ============================================================================

/**
 * @brief Callback for curl to write response data
 */
struct curl_response {
    char* data;
    size_t size;
};

static size_t weather_curl_write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    struct curl_response* response = (struct curl_response*)userp;
    
    char* ptr = realloc(response->data, response->size + realsize + 1);
    if (!ptr) {
        ethervox_log(ETHERVOX_LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__,
                    "Failed to allocate memory for HTTP response");
        return 0;
    }
    
    response->data = ptr;
    memcpy(&(response->data[response->size]), contents, realsize);
    response->size += realsize;
    response->data[response->size] = 0;
    
    return realsize;
}

/**
 * @brief Make HTTP GET request with retry logic
 */
static ethervox_result_t http_get_request(
    const char* url,
    char** response_out,
    char** error_message_out
) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        if (error_message_out) {
            *error_message_out = strdup("Failed to initialize HTTP client");
        }
        return ETHERVOX_ERROR_NETWORK;
    }
    
    struct curl_response response = {0};
    ethervox_result_t result = ETHERVOX_SUCCESS;
    CURLcode res;
    long http_code = 0;
    int retry_count = 0;
    
    ethervox_log(ETHERVOX_LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__,
                "HTTP GET: %s", url);
    
    // Configure curl
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, weather_curl_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, g_config.user_agent);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)g_config.request_timeout_sec);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "gzip");
    
    // Retry loop
    while (retry_count <= g_config.max_retries) {
        res = curl_easy_perform(curl);
        
        if (res == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
            
            if (http_code == 200) {
                // Success
                *response_out = response.data;
                result = ETHERVOX_SUCCESS;
                break;
            } else if (http_code == 429) {
                // Rate limit - don't retry
                result = ETHERVOX_ERROR_API_RATE_LIMIT;
                if (error_message_out) {
                    *error_message_out = strdup("API rate limit exceeded");
                }
                break;
            } else if (http_code >= 500) {
                // Server error - retry
                ethervox_log(ETHERVOX_LOG_LEVEL_WARN, __FILE__, __LINE__, __func__,
                            "HTTP %ld - retrying (%d/%d)", http_code, retry_count + 1, g_config.max_retries);
                retry_count++;
                if (retry_count <= g_config.max_retries) {
                    // Exponential backoff: 1s, 2s, 4s
                    struct timespec ts = {.tv_sec = 1 << (retry_count - 1), .tv_nsec = 0};
                    nanosleep(&ts, NULL);
                    continue;
                }
                result = ETHERVOX_ERROR_API_RESPONSE;
                if (error_message_out) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "API server error (HTTP %ld)", http_code);
                    *error_message_out = strdup(msg);
                }
                break;
            } else {
                // Other HTTP error
                result = ETHERVOX_ERROR_API_RESPONSE;
                if (error_message_out) {
                    char msg[256];
                    snprintf(msg, sizeof(msg), "API request failed (HTTP %ld)", http_code);
                    *error_message_out = strdup(msg);
                }
                break;
            }
        } else {
            // Network error
            if (res == CURLE_OPERATION_TIMEDOUT) {
                result = ETHERVOX_ERROR_DOWNLOAD_TIMEOUT;
                if (error_message_out) {
                    *error_message_out = strdup("Request timeout - weather service not responding");
                }
            } else {
                result = ETHERVOX_ERROR_NETWORK;
                if (error_message_out) {
                    char msg[512];
                    snprintf(msg, sizeof(msg), "Network error: %s", curl_easy_strerror(res));
                    *error_message_out = strdup(msg);
                }
            }
            break;
        }
    }
    
    if (result != ETHERVOX_SUCCESS && response.data) {
        free(response.data);
    }
    
    curl_easy_cleanup(curl);
    return result;
}

/**
 * @brief URL-encode a string
 */
static char* url_encode(const char* str) {
    CURL* curl = curl_easy_init();
    if (!curl) return NULL;
    
    char* encoded = curl_easy_escape(curl, str, 0);
    char* result = encoded ? strdup(encoded) : NULL;
    
    if (encoded) curl_free(encoded);
    curl_easy_cleanup(curl);
    
    return result;
}

// ============================================================================
// Geocoding
// ============================================================================

ethervox_result_t ethervox_weather_geocode(
    const char* location_name,
    double* latitude_out,
    double* longitude_out,
    char** error_message_out
) {
    if (!g_initialized) {
        if (error_message_out) {
            *error_message_out = strdup("Weather tools not initialized");
        }
        return ETHERVOX_ERROR_NOT_INITIALIZED;
    }
    
    if (!location_name || !latitude_out || !longitude_out) {
        if (error_message_out) {
            *error_message_out = strdup("Invalid parameters");
        }
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // URL-encode location name
    char* encoded_name = url_encode(location_name);
    if (!encoded_name) {
        if (error_message_out) {
            *error_message_out = strdup("Failed to encode location name");
        }
        return ETHERVOX_ERROR_OUT_OF_MEMORY;
    }
    
    // Build geocoding URL
    char url[1024];
    snprintf(url, sizeof(url), "%s?name=%s&count=1&language=en&format=json",
             OPENMETEO_GEOCODING_URL, encoded_name);
    free(encoded_name);
    
    // Make request
    char* response_json = NULL;
    char* error_msg = NULL;
    ethervox_result_t result = http_get_request(url, &response_json, &error_msg);
    
    if (result != ETHERVOX_SUCCESS) {
        if (error_message_out) {
            *error_message_out = error_msg;
        } else {
            free(error_msg);
        }
        return result;
    }
    
    // Parse JSON response
    cJSON* root = cJSON_Parse(response_json);
    free(response_json);
    
    if (!root) {
        if (error_message_out) {
            *error_message_out = strdup("Failed to parse geocoding response");
        }
        return ETHERVOX_ERROR_API_RESPONSE;
    }
    
    cJSON* results = cJSON_GetObjectItem(root, "results");
    if (!results || !cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        cJSON_Delete(root);
        if (error_message_out) {
            char msg[512];
            snprintf(msg, sizeof(msg), "Location '%s' not found", location_name);
            *error_message_out = strdup(msg);
        }
        return ETHERVOX_ERROR_NOT_FOUND;
    }
    
    cJSON* first_result = cJSON_GetArrayItem(results, 0);
    cJSON* lat = cJSON_GetObjectItem(first_result, "latitude");
    cJSON* lon = cJSON_GetObjectItem(first_result, "longitude");
    
    if (!lat || !lon || !cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) {
        cJSON_Delete(root);
        if (error_message_out) {
            *error_message_out = strdup("Invalid geocoding response format");
        }
        return ETHERVOX_ERROR_API_RESPONSE;
    }
    
    *latitude_out = lat->valuedouble;
    *longitude_out = lon->valuedouble;
    
    ethervox_log(ETHERVOX_LOG_LEVEL_INFO, __FILE__, __LINE__, __func__,
                "Geocoded '%s' to (%.4f, %.4f)", location_name, *latitude_out, *longitude_out);
    
    cJSON_Delete(root);
    return ETHERVOX_SUCCESS;
}

// ============================================================================
// Weather Forecast Parsing
// ============================================================================

/**
 * @brief Parse ISO 8601 timestamp to Unix time
 */
static time_t parse_iso8601(const char* timestamp) {
    struct tm tm = {0};
    sscanf(timestamp, "%d-%d-%dT%d:%d",
           &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
           &tm.tm_hour, &tm.tm_min);
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    return mktime(&tm);
}

/**
 * @brief Parse current weather from JSON
 */
static ethervox_result_t parse_current_weather(
    cJSON* root,
    ethervox_weather_response_t* response
) {
    cJSON* current = cJSON_GetObjectItem(root, "current_weather");
    if (!current) {
        return ETHERVOX_ERROR_API_RESPONSE;
    }
    
    response->forecast_count = 1;
    response->forecasts = calloc(1, sizeof(ethervox_weather_forecast_t));
    if (!response->forecasts) {
        return ETHERVOX_ERROR_OUT_OF_MEMORY;
    }
    
    ethervox_weather_forecast_t* fc = &response->forecasts[0];
    
    cJSON* temp = cJSON_GetObjectItem(current, "temperature");
    cJSON* windspeed = cJSON_GetObjectItem(current, "windspeed");
    cJSON* winddirection = cJSON_GetObjectItem(current, "winddirection");
    cJSON* weathercode = cJSON_GetObjectItem(current, "weathercode");
    cJSON* time_item = cJSON_GetObjectItem(current, "time");
    
    if (temp && cJSON_IsNumber(temp)) {
        fc->temperature = (float)temp->valuedouble;
        fc->apparent_temperature = fc->temperature; // No feels-like in current weather
    }
    
    if (windspeed && cJSON_IsNumber(windspeed)) {
        fc->wind_speed = (float)windspeed->valuedouble;
    }
    
    if (winddirection && cJSON_IsNumber(winddirection)) {
        fc->wind_direction = winddirection->valueint;
    }
    
    if (weathercode && cJSON_IsNumber(weathercode)) {
        fc->weather_code = weathercode->valueint;
        strncpy(fc->condition, wmo_code_to_condition(fc->weather_code), sizeof(fc->condition) - 1);
    }
    
    if (time_item && cJSON_IsString(time_item)) {
        fc->timestamp = parse_iso8601(time_item->valuestring);
    } else {
        fc->timestamp = time(NULL);
    }
    
    return ETHERVOX_SUCCESS;
}

/**
 * @brief Parse hourly forecast from JSON
 */
static ethervox_result_t parse_hourly_forecast(
    cJSON* root,
    ethervox_weather_response_t* response,
    int max_hours
) {
    cJSON* hourly = cJSON_GetObjectItem(root, "hourly");
    if (!hourly) {
        return ETHERVOX_ERROR_API_RESPONSE;
    }
    
    cJSON* time_array = cJSON_GetObjectItem(hourly, "time");
    cJSON* temp_array = cJSON_GetObjectItem(hourly, "temperature_2m");
    cJSON* apparent_temp_array = cJSON_GetObjectItem(hourly, "apparent_temperature");
    cJSON* humidity_array = cJSON_GetObjectItem(hourly, "relative_humidity_2m");
    cJSON* precipitation_array = cJSON_GetObjectItem(hourly, "precipitation");
    cJSON* precipitation_prob_array = cJSON_GetObjectItem(hourly, "precipitation_probability");
    cJSON* weathercode_array = cJSON_GetObjectItem(hourly, "weathercode");
    cJSON* windspeed_array = cJSON_GetObjectItem(hourly, "wind_speed_10m");
    cJSON* winddirection_array = cJSON_GetObjectItem(hourly, "wind_direction_10m");
    
    if (!time_array || !cJSON_IsArray(time_array)) {
        return ETHERVOX_ERROR_API_RESPONSE;
    }
    
    int count = cJSON_GetArraySize(time_array);
    if (count > max_hours) count = max_hours;
    
    response->forecast_count = count;
    response->forecasts = calloc(count, sizeof(ethervox_weather_forecast_t));
    if (!response->forecasts) {
        return ETHERVOX_ERROR_OUT_OF_MEMORY;
    }
    
    for (int i = 0; i < count; i++) {
        ethervox_weather_forecast_t* fc = &response->forecasts[i];
        
        cJSON* time_item = cJSON_GetArrayItem(time_array, i);
        if (time_item && cJSON_IsString(time_item)) {
            fc->timestamp = parse_iso8601(time_item->valuestring);
        }
        
        cJSON* temp = cJSON_GetArrayItem(temp_array, i);
        if (temp && cJSON_IsNumber(temp)) {
            fc->temperature = (float)temp->valuedouble;
        }
        
        cJSON* apparent = cJSON_GetArrayItem(apparent_temp_array, i);
        if (apparent && cJSON_IsNumber(apparent)) {
            fc->apparent_temperature = (float)apparent->valuedouble;
        }
        
        cJSON* humidity = cJSON_GetArrayItem(humidity_array, i);
        if (humidity && cJSON_IsNumber(humidity)) {
            fc->humidity = humidity->valueint;
        }
        
        cJSON* precip = cJSON_GetArrayItem(precipitation_array, i);
        if (precip && cJSON_IsNumber(precip)) {
            fc->precipitation = (float)precip->valuedouble;
        }
        
        cJSON* precip_prob = cJSON_GetArrayItem(precipitation_prob_array, i);
        if (precip_prob && cJSON_IsNumber(precip_prob)) {
            fc->precipitation_probability = precip_prob->valueint;
        }
        
        cJSON* weathercode = cJSON_GetArrayItem(weathercode_array, i);
        if (weathercode && cJSON_IsNumber(weathercode)) {
            fc->weather_code = weathercode->valueint;
            strncpy(fc->condition, wmo_code_to_condition(fc->weather_code), sizeof(fc->condition) - 1);
        }
        
        cJSON* windspeed = cJSON_GetArrayItem(windspeed_array, i);
        if (windspeed && cJSON_IsNumber(windspeed)) {
            fc->wind_speed = (float)windspeed->valuedouble;
        }
        
        cJSON* winddirection = cJSON_GetArrayItem(winddirection_array, i);
        if (winddirection && cJSON_IsNumber(winddirection)) {
            fc->wind_direction = winddirection->valueint;
        }
    }
    
    return ETHERVOX_SUCCESS;
}

/**
 * @brief Parse daily forecast from JSON
 */
static ethervox_result_t parse_daily_forecast(
    cJSON* root,
    ethervox_weather_response_t* response,
    int max_days
) {
    cJSON* daily = cJSON_GetObjectItem(root, "daily");
    if (!daily) {
        return ETHERVOX_ERROR_API_RESPONSE;
    }
    
    cJSON* time_array = cJSON_GetObjectItem(daily, "time");
    cJSON* temp_max_array = cJSON_GetObjectItem(daily, "temperature_2m_max");
    cJSON* temp_min_array = cJSON_GetObjectItem(daily, "temperature_2m_min");
    cJSON* precipitation_array = cJSON_GetObjectItem(daily, "precipitation_sum");
    cJSON* precipitation_prob_array = cJSON_GetObjectItem(daily, "precipitation_probability_max");
    cJSON* weathercode_array = cJSON_GetObjectItem(daily, "weathercode");
    cJSON* windspeed_array = cJSON_GetObjectItem(daily, "wind_speed_10m_max");
    
    if (!time_array || !cJSON_IsArray(time_array)) {
        return ETHERVOX_ERROR_API_RESPONSE;
    }
    
    int count = cJSON_GetArraySize(time_array);
    if (count > max_days) count = max_days;
    
    response->forecast_count = count;
    response->forecasts = calloc(count, sizeof(ethervox_weather_forecast_t));
    if (!response->forecasts) {
        return ETHERVOX_ERROR_OUT_OF_MEMORY;
    }
    
    for (int i = 0; i < count; i++) {
        ethervox_weather_forecast_t* fc = &response->forecasts[i];
        
        cJSON* time_item = cJSON_GetArrayItem(time_array, i);
        if (time_item && cJSON_IsString(time_item)) {
            fc->timestamp = parse_iso8601(time_item->valuestring);
        }
        
        cJSON* temp_max = cJSON_GetArrayItem(temp_max_array, i);
        cJSON* temp_min = cJSON_GetArrayItem(temp_min_array, i);
        if (temp_max && temp_min && cJSON_IsNumber(temp_max) && cJSON_IsNumber(temp_min)) {
            // Average of max and min for simplicity
            fc->temperature = (float)((temp_max->valuedouble + temp_min->valuedouble) / 2.0);
            fc->apparent_temperature = fc->temperature;
        }
        
        cJSON* precip = cJSON_GetArrayItem(precipitation_array, i);
        if (precip && cJSON_IsNumber(precip)) {
            fc->precipitation = (float)precip->valuedouble;
        }
        
        cJSON* precip_prob = cJSON_GetArrayItem(precipitation_prob_array, i);
        if (precip_prob && cJSON_IsNumber(precip_prob)) {
            fc->precipitation_probability = precip_prob->valueint;
        }
        
        cJSON* weathercode = cJSON_GetArrayItem(weathercode_array, i);
        if (weathercode && cJSON_IsNumber(weathercode)) {
            fc->weather_code = weathercode->valueint;
            strncpy(fc->condition, wmo_code_to_condition(fc->weather_code), sizeof(fc->condition) - 1);
        }
        
        cJSON* windspeed = cJSON_GetArrayItem(windspeed_array, i);
        if (windspeed && cJSON_IsNumber(windspeed)) {
            fc->wind_speed = (float)windspeed->valuedouble;
        }
    }
    
    return ETHERVOX_SUCCESS;
}

// ============================================================================
// Main Weather Forecast API
// ============================================================================

ethervox_result_t ethervox_weather_get_forecast(
    const ethervox_weather_request_t* request,
    ethervox_weather_response_t** response_out,
    char** error_message_out
) {
    if (!g_initialized) {
        if (error_message_out) {
            *error_message_out = strdup("Weather tools not initialized");
        }
        return ETHERVOX_ERROR_NOT_INITIALIZED;
    }
    
    if (!request || !response_out) {
        if (error_message_out) {
            *error_message_out = strdup("Invalid parameters");
        }
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Validate coordinates
    if (request->latitude < -90.0 || request->latitude > 90.0 ||
        request->longitude < -180.0 || request->longitude > 180.0) {
        if (error_message_out) {
            *error_message_out = strdup("Invalid coordinates (latitude must be -90 to 90, longitude -180 to 180)");
        }
        return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Check cache
    if (request->use_cache && g_config.enable_cache) {
        ethervox_weather_response_t* cached = weather_cache_get(request);
        if (cached) {
            *response_out = cached;
            ethervox_log(ETHERVOX_LOG_LEVEL_DEBUG, __FILE__, __LINE__, __func__,
                        "Cache hit for (%.4f, %.4f)", request->latitude, request->longitude);
            return ETHERVOX_SUCCESS;
        }
    }
    
    // Build forecast URL based on type
    char url[2048];
    const char* base = OPENMETEO_FORECAST_URL;
    
    switch (request->forecast_type) {
        case ETHERVOX_WEATHER_CURRENT:
            snprintf(url, sizeof(url),
                     "%s?latitude=%.4f&longitude=%.4f&current_weather=true",
                     base, request->latitude, request->longitude);
            break;
            
        case ETHERVOX_WEATHER_HOURLY:
            snprintf(url, sizeof(url),
                     "%s?latitude=%.4f&longitude=%.4f"
                     "&hourly=temperature_2m,apparent_temperature,relative_humidity_2m,"
                     "precipitation,precipitation_probability,weathercode,"
                     "wind_speed_10m,wind_direction_10m"
                     "&forecast_days=1",
                     base, request->latitude, request->longitude);
            break;
            
        case ETHERVOX_WEATHER_DAILY:
            snprintf(url, sizeof(url),
                     "%s?latitude=%.4f&longitude=%.4f"
                     "&daily=temperature_2m_max,temperature_2m_min,precipitation_sum,"
                     "precipitation_probability_max,weathercode,wind_speed_10m_max"
                     "&forecast_days=1",
                     base, request->latitude, request->longitude);
            break;
            
        case ETHERVOX_WEATHER_7DAY:
            snprintf(url, sizeof(url),
                     "%s?latitude=%.4f&longitude=%.4f"
                     "&daily=temperature_2m_max,temperature_2m_min,precipitation_sum,"
                     "precipitation_probability_max,weathercode,wind_speed_10m_max"
                     "&forecast_days=7",
                     base, request->latitude, request->longitude);
            break;
            
        default:
            if (error_message_out) {
                *error_message_out = strdup("Invalid forecast type");
            }
            return ETHERVOX_ERROR_INVALID_ARGUMENT;
    }
    
    // Make HTTP request
    char* response_json = NULL;
    char* error_msg = NULL;
    ethervox_result_t result = http_get_request(url, &response_json, &error_msg);
    
    if (result != ETHERVOX_SUCCESS) {
        if (error_message_out) {
            *error_message_out = error_msg;
        } else {
            free(error_msg);
        }
        return result;
    }
    
    // Parse JSON
    cJSON* root = cJSON_Parse(response_json);
    free(response_json);
    
    if (!root) {
        if (error_message_out) {
            *error_message_out = strdup("Failed to parse weather response");
        }
        return ETHERVOX_ERROR_API_RESPONSE;
    }
    
    // Create response structure
    ethervox_weather_response_t* response = calloc(1, sizeof(ethervox_weather_response_t));
    if (!response) {
        cJSON_Delete(root);
        if (error_message_out) {
            *error_message_out = strdup("Memory allocation failed");
        }
        return ETHERVOX_ERROR_OUT_OF_MEMORY;
    }
    
    // Extract metadata
    cJSON* lat = cJSON_GetObjectItem(root, "latitude");
    cJSON* lon = cJSON_GetObjectItem(root, "longitude");
    cJSON* tz = cJSON_GetObjectItem(root, "timezone");
    
    if (lat && cJSON_IsNumber(lat)) response->latitude = lat->valuedouble;
    if (lon && cJSON_IsNumber(lon)) response->longitude = lon->valuedouble;
    if (tz && cJSON_IsString(tz)) {
        strncpy(response->timezone, tz->valuestring, sizeof(response->timezone) - 1);
    }
    
    strncpy(response->location_name, request->location_name, sizeof(response->location_name) - 1);
    response->generated_at = time(NULL);
    response->from_cache = false;
    
    // Parse forecast data based on type
    switch (request->forecast_type) {
        case ETHERVOX_WEATHER_CURRENT:
            result = parse_current_weather(root, response);
            break;
        case ETHERVOX_WEATHER_HOURLY:
            result = parse_hourly_forecast(root, response, 24);
            break;
        case ETHERVOX_WEATHER_DAILY:
            result = parse_daily_forecast(root, response, 1);
            break;
        case ETHERVOX_WEATHER_7DAY:
            result = parse_daily_forecast(root, response, 7);
            break;
    }
    
    cJSON_Delete(root);
    
    if (result != ETHERVOX_SUCCESS) {
        ethervox_weather_free_response(response);
        if (error_message_out) {
            *error_message_out = strdup("Failed to parse weather data");
        }
        return result;
    }
    
    // Cache the response
    if (request->use_cache && g_config.enable_cache) {
        weather_cache_put(request, response);
    }
    
    *response_out = response;
    
    ethervox_log(ETHERVOX_LOG_LEVEL_INFO, __FILE__, __LINE__, __func__,
                "Fetched weather for (%.4f, %.4f): %d forecasts",
                request->latitude, request->longitude, (int)response->forecast_count);
    
    return ETHERVOX_SUCCESS;
}

// ============================================================================
// Module Lifecycle
// ============================================================================

ethervox_result_t ethervox_weather_init(const ethervox_weather_config_t* config) {
    if (g_initialized) {
        return ETHERVOX_SUCCESS;
    }
    
    // Use provided config or defaults
    if (config) {
        g_config = *config;
    }
    
    // Initialize curl globally
    curl_global_init(CURL_GLOBAL_DEFAULT);
    
    g_initialized = true;
    
    ethervox_log(ETHERVOX_LOG_LEVEL_INFO, __FILE__, __LINE__, __func__,
                "Weather tools initialized (cache: %s, TTL: %ds)",
                g_config.enable_cache ? "enabled" : "disabled",
                g_config.cache_ttl_sec);
    
    return ETHERVOX_SUCCESS;
}

void ethervox_weather_cleanup(void) {
    if (!g_initialized) {
        return;
    }
    
    weather_cache_clear();
    curl_global_cleanup();
    
    g_initialized = false;
    
    ethervox_log(ETHERVOX_LOG_LEVEL_INFO, __FILE__, __LINE__, __func__,
                "Weather tools cleaned up");
}

void ethervox_weather_free_response(ethervox_weather_response_t* response) {
    if (!response) return;
    
    if (response->forecasts) {
        free(response->forecasts);
    }
    free(response);
}

ethervox_result_t ethervox_weather_clear_cache(void) {
    if (!g_initialized) {
        return ETHERVOX_ERROR_NOT_INITIALIZED;
    }
    
    weather_cache_clear();
    return ETHERVOX_SUCCESS;
}

void ethervox_weather_get_cache_stats(
    uint64_t* hit_count_out,
    uint64_t* miss_count_out,
    size_t* size_out
) {
    if (g_initialized) {
        weather_cache_get_stats(hit_count_out, miss_count_out, size_out);
    } else {
        if (hit_count_out) *hit_count_out = 0;
        if (miss_count_out) *miss_count_out = 0;
        if (size_out) *size_out = 0;
    }
}
