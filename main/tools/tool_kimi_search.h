#pragma once

#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

/**
 * Initialize Kimi web search tool.
 */
esp_err_t tool_kimi_search_init(void);

/**
 * Execute a web search using Kimi AI.
 *
 * @param input_json   JSON string with "query" field
 * @param output       Output buffer for formatted search results
 * @param output_size  Size of output buffer
 * @return ESP_OK on success
 */
esp_err_t tool_kimi_search_execute(const char *input_json, char *output, size_t output_size);

/**
 * Save Kimi API key to NVS.
 */
esp_err_t tool_kimi_search_set_key(const char *api_key);

/**
 * Check if Kimi search is available (has API key configured).
 */
bool tool_kimi_search_is_available(void);
