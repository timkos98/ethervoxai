/**
 * @file bug_reporter.c
 * @brief Anonymous bug and feature request reporting implementation
 *
 * Collects system information and submits issues to GitHub anonymously.
 *
 * Copyright (c) 2024-2025 EthervoxAI Team
 * SPDX-License-Identifier: CC-BY-NC-SA-4.0
 */

#include "ethervox/bug_reporter.h"
#include "ethervox/logging.h"
#include "ethervox/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <sys/utsname.h>
#include <unistd.h>

// GitHub API configuration
#define GITHUB_API_URL "https://api.github.com/repos/ethervox-ai/ethervoxai-android/issues"
// Token is read from environment variable ETHERVOX_GITHUB_TOKEN at runtime
// Set it with: export ETHERVOX_GITHUB_TOKEN="your_token_here"
// Fine-grained token should have ONLY public_repo (issues:write) scope

// Response buffer for HTTP responses
typedef struct {
    char* data;
    size_t size;
} http_response_t;

static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    http_response_t* mem = (http_response_t*)userp;
    
    char* ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) {
        ETHERVOX_LOG_ERROR("Out of memory in write_callback");
        return 0;
    }
    
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;
    
    return realsize;
}

int ethervox_report_get_system_info(char* buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        return -1;
    }
    
    struct utsname sys_info;
    char hostname[256] = "Unknown";
    int written = 0;
    
    // Get system information
    if (uname(&sys_info) == 0) {
        gethostname(hostname, sizeof(hostname));
        
        written = snprintf(buffer, buffer_size,
            "### System Information\n\n"
            "- **OS:** %s %s\n"
            "- **Architecture:** %s\n"
            "- **Machine:** %s\n"
            "- **Hostname:** %s\n"
            "- **App Version:** %s\n"
            "- **Git Branch:** %s\n"
            "- **Git Commit:** %s\n",
            sys_info.sysname,
            sys_info.release,
            sys_info.machine,
            sys_info.nodename,
            hostname,
            ETHERVOX_BACKEND_VERSION,
            ETHERVOX_GIT_BRANCH,
            ETHERVOX_GIT_COMMIT
        );
    } else {
        written = snprintf(buffer, buffer_size,
            "### System Information\n\n"
            "- **OS:** Unknown\n"
            "- **App Version:** %s\n",
            ETHERVOX_BACKEND_VERSION
        );
    }
    
    return written;
}

static char* json_escape(const char* str) {
    if (!str) return NULL;
    
    size_t len = strlen(str);
    // Worst case: every char needs escaping (e.g., all quotes)
    char* escaped = malloc(len * 2 + 1);
    if (!escaped) return NULL;
    
    char* out = escaped;
    for (const char* in = str; *in; in++) {
        switch (*in) {
            case '"':
            case '\\':
                *out++ = '\\';
                *out++ = *in;
                break;
            case '\n':
                *out++ = '\\';
                *out++ = 'n';
                break;
            case '\r':
                *out++ = '\\';
                *out++ = 'r';
                break;
            case '\t':
                *out++ = '\\';
                *out++ = 't';
                break;
            default:
                *out++ = *in;
                break;
        }
    }
    *out = '\0';
    
    return escaped;
}

int ethervox_report_submit(
    ethervox_report_type_t type,
    const char* title,
    const char* description,
    bool include_system_info,
    ethervox_report_result_t* result
) {
    if (!title || !description || !result) {
        ETHERVOX_LOG_ERROR("Invalid arguments to bug reporter");
        return -1;
    }
    
    // Initialize result
    memset(result, 0, sizeof(ethervox_report_result_t));
    result->success = false;
    
    // Get GitHub token from compile-time definition
    // Token is compiled into the binary from environment variable at build time
    // Priority: ETHERVOX_GITHUB_TOKEN_DESKTOP → ETHERVOX_GITHUB_TOKEN → empty string
    const char* github_token = ETHERVOX_GITHUB_TOKEN;
    
    if (!github_token || github_token[0] == '\0') {
        snprintf(result->error_message, sizeof(result->error_message),
                 "Bug reporting not configured. Set ETHERVOX_GITHUB_TOKEN at build time.");
        ETHERVOX_LOG_WARN("Bug reporting disabled: ETHERVOX_GITHUB_TOKEN was not set during build");
        return -1;
    }
    
    // Initialize curl
    CURL* curl = curl_easy_init();
    if (!curl) {
        snprintf(result->error_message, sizeof(result->error_message),
                 "Failed to initialize HTTP client");
        return -1;
    }
    
    // Build issue body
    const char* issue_type = (type == ETHERVOX_REPORT_FEATURE) ? "Feature Request" : "Bug Report";
    const char* emoji = (type == ETHERVOX_REPORT_FEATURE) ? "💡" : "🐞";
    
    char body[4096];
    int body_len = snprintf(body, sizeof(body),
        "## %s\\n\\n"
        "> **Reported via in-app reporter**\\n"
        "> *This is a user-submitted %s. Please add appropriate labels.*\\n\\n"
        "### Description\\n\\n"
        "%s\\n\\n",
        issue_type,
        issue_type,
        description
    );
    
    // Add system info if requested
    if (include_system_info) {
        char sys_info[1024];
        ethervox_report_get_system_info(sys_info, sizeof(sys_info));
        
        // Escape newlines for JSON
        for (char* p = sys_info; *p; p++) {
            if (*p == '\n') *p = ' ';
        }
        
        body_len += snprintf(body + body_len, sizeof(body) - body_len,
                            "%s", sys_info);
    }
    
    // Escape strings for JSON
    char* escaped_title = json_escape(title);
    char* escaped_body = json_escape(body);
    
    if (!escaped_title || !escaped_body) {
        snprintf(result->error_message, sizeof(result->error_message),
                 "Failed to allocate memory for request");
        free(escaped_title);
        free(escaped_body);
        curl_easy_cleanup(curl);
        return -1;
    }
    
    // Build JSON payload
    const char* prefix = (type == ETHERVOX_REPORT_FEATURE) ? "[Feature]" : "[Bug]";
    char json_payload[8192];
    snprintf(json_payload, sizeof(json_payload),
        "{"
        "\"title\":\"%s %s\","
        "\"body\":\"%s\""
        "}",
        prefix, escaped_title, escaped_body
    );
    
    free(escaped_title);
    free(escaped_body);
    
    // Prepare HTTP headers
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
    
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", github_token);
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    // Setup response buffer
    http_response_t response = {0};
    
    // Configure curl
    curl_easy_setopt(curl, CURLOPT_URL, GITHUB_API_URL);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_payload);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void*)&response);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "EthervoxAI-BugReporter/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    
    // Perform request
    CURLcode res = curl_easy_perform(curl);
    
    if (res != CURLE_OK) {
        snprintf(result->error_message, sizeof(result->error_message),
                 "Network error: %s", curl_easy_strerror(res));
        ETHERVOX_LOG_ERROR("Failed to submit bug report: %s", curl_easy_strerror(res));
    } else {
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        result->http_status = (int)http_code;
        
        if (http_code == 201) {
            // Success - extract issue URL
            result->success = true;
            
            // Parse JSON response for html_url
            if (response.data) {
                char* url_start = strstr(response.data, "\"html_url\":\"");
                if (url_start) {
                    url_start += 12; // Skip past "html_url":"
                    char* url_end = strchr(url_start, '"');
                    if (url_end) {
                        size_t url_len = url_end - url_start;
                        if (url_len < sizeof(result->issue_url)) {
                            strncpy(result->issue_url, url_start, url_len);
                            result->issue_url[url_len] = '\0';
                        }
                    }
                }
            }
            
            ETHERVOX_LOG_INFO("Bug report created successfully: %s", result->issue_url);
        } else {
            snprintf(result->error_message, sizeof(result->error_message),
                     "HTTP %d", (int)http_code);
            
            if (response.data) {
                ETHERVOX_LOG_ERROR("Failed to create bug report: HTTP %ld - %s",
                                  http_code, response.data);
            } else {
                ETHERVOX_LOG_ERROR("Failed to create bug report: HTTP %ld", http_code);
            }
        }
    }
    
    // Cleanup
    free(response.data);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    
    return result->success ? 0 : -1;
}
