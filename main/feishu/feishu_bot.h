#pragma once

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Feishu Bot module
 * 
 * Loads App ID and App Secret from NVS or build-time secrets.
 */
esp_err_t feishu_bot_init(void);

/**
 * @brief Start Feishu Bot tasks
 * 
 * Starts the WebSocket client for receiving events.
 * Should be called after WiFi is connected.
 */
esp_err_t feishu_bot_start(void);

/**
 * @brief Send a message to a Feishu user or chat
 * 
 * @param chat_id The open_id of the user or chat
 * @param text The message content (text)
 * @return esp_err_t ESP_OK on success
 */
esp_err_t feishu_send_message(const char *chat_id, const char *text);

/**
 * @brief Set Feishu App configuration at runtime
 * 
 * @param app_id Feishu App ID
 * @param app_secret Feishu App Secret
 * @return esp_err_t ESP_OK on success
 */
esp_err_t feishu_set_config(const char *app_id, const char *app_secret);

#ifdef __cplusplus
}
#endif
