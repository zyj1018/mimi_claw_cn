#include "feishu_bot.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_websocket_client.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "feishu";

static char s_app_id[64] = MIMI_SECRET_FEISHU_APP_ID;
static char s_app_secret[64] = MIMI_SECRET_FEISHU_APP_SECRET;
static char s_tenant_token[256] = {0};
static int64_t s_token_expire_ts = 0;

static esp_websocket_client_handle_t s_ws_client = NULL;
static bool s_is_running = false;

/* HTTP Response Accumulator */
typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt) {
    http_resp_t *resp = (http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (resp && resp->len + evt->data_len < resp->cap) {
            memcpy(resp->buf + resp->len, evt->data, evt->data_len);
            resp->len += evt->data_len;
            resp->buf[resp->len] = 0;
        }
    }
    return ESP_OK;
}

static esp_err_t feishu_get_tenant_access_token(void) {
    if (strlen(s_app_id) == 0 || strlen(s_app_secret) == 0) {
        ESP_LOGE(TAG, "App ID or Secret not set");
        return ESP_ERR_INVALID_STATE;
    }

    char *resp_buf = heap_caps_calloc(1, 1024, MALLOC_CAP_SPIRAM);
    if (!resp_buf) return ESP_ERR_NO_MEM;

    http_resp_t resp = {.buf = resp_buf, .len = 0, .cap = 1024};

    esp_http_client_config_t config = {
        .url = "https://open.feishu.cn/open-apis/auth/v3/tenant_access_token/internal",
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp_buf);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "app_id", s_app_id);
    cJSON_AddStringToObject(root, "app_secret", s_app_secret);
    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            cJSON *json = cJSON_Parse(resp.buf);
            if (json) {
                cJSON *code = cJSON_GetObjectItem(json, "code");
                if (code && code->valueint == 0) {
                    cJSON *token = cJSON_GetObjectItem(json, "tenant_access_token");
                    cJSON *expire = cJSON_GetObjectItem(json, "expire");
                    if (token && token->valuestring && expire) {
                        strncpy(s_tenant_token, token->valuestring, sizeof(s_tenant_token) - 1);
                        s_token_expire_ts = (esp_timer_get_time() / 1000000) + expire->valueint - 60; // Buffer 60s
                        ESP_LOGI(TAG, "Got tenant token, expires in %d s", expire->valueint);
                    }
                } else {
                    ESP_LOGE(TAG, "Feishu Auth Error: %s", resp.buf);
                    err = ESP_FAIL;
                }
                cJSON_Delete(json);
            } else {
                ESP_LOGE(TAG, "Failed to parse auth response");
                err = ESP_FAIL;
            }
        } else {
            ESP_LOGE(TAG, "Auth HTTP Status: %d", status);
            err = ESP_FAIL;
        }
    } else {
        ESP_LOGE(TAG, "Auth Request Failed: %s", esp_err_to_name(err));
    }

    free(post_data);
    esp_http_client_cleanup(client);
    free(resp_buf);
    return err;
}

static esp_err_t feishu_ensure_token(void) {
    int64_t now = esp_timer_get_time() / 1000000;
    if (strlen(s_tenant_token) == 0 || now >= s_token_expire_ts) {
        return feishu_get_tenant_access_token();
    }
    return ESP_OK;
}

esp_err_t feishu_send_message(const char *chat_id, const char *text) {
    if (feishu_ensure_token() != ESP_OK) return ESP_FAIL;

    char *resp_buf = heap_caps_calloc(1, 1024, MALLOC_CAP_SPIRAM);
    if (!resp_buf) return ESP_ERR_NO_MEM;

    http_resp_t resp = {.buf = resp_buf, .len = 0, .cap = 1024};

    // Use "messages" endpoint. Detect if chat_id starts with "oc_" (group) or "ou_" (user)
    // Feishu OpenID starts with "ou_", ChatID starts with "oc_"
    const char *receive_id_type = "open_id";
    if (strncmp(chat_id, "oc_", 3) == 0) {
        receive_id_type = "chat_id";
    }
    
    char url[300]; // Increased buffer size for URL with long parameters
    snprintf(url, sizeof(url), "https://open.feishu.cn/open-apis/im/v1/messages?receive_id_type=%s", receive_id_type);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(resp_buf);
        return ESP_FAIL;
    }

    // Build JSON: {"receive_id": chat_id, "msg_type": "text", "content": "{\"text\":\"...\"}"}
    // Note: Content is a JSON string!
    cJSON *content_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(content_obj, "text", text);
    char *content_str = cJSON_PrintUnformatted(content_obj);
    cJSON_Delete(content_obj);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "receive_id", chat_id);
    cJSON_AddStringToObject(root, "msg_type", "text");
    cJSON_AddStringToObject(root, "content", content_str);
    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(content_str);

    char auth_header[512]; // Increased buffer for auth header
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_tenant_token);

    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            ESP_LOGE(TAG, "Send Msg Error %d: %s", status, resp.buf);
            err = ESP_FAIL;
        } else {
            ESP_LOGI(TAG, "Message sent to %s", chat_id);
        }
    } else {
        ESP_LOGE(TAG, "Send Request Failed: %s", esp_err_to_name(err));
    }

    free(post_data);
    esp_http_client_cleanup(client);
    free(resp_buf);
    return err;
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WS Error: type=%d status=%d sock_errno=%d tls=%s",
                (int)data->error_handle.error_type,
                data->error_handle.esp_ws_handshake_status_code,
                data->error_handle.esp_transport_sock_errno,
                esp_err_to_name(data->error_handle.esp_tls_last_esp_err));
            break;
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WS Connected");
            break;
        case WEBSOCKET_EVENT_DATA:
            if (data->data_len > 0) {
                // 打印完整的 WebSocket 数据包，方便调试
                // ESP_LOGI(TAG, "WS Data (%d bytes)", data->data_len);

                // 飞书 WS v2 使用 protobuf 协议，但核心业务数据是作为 JSON 字符串嵌入在其中的。
                // 我们不引入繁重的 protobuf 库，而是直接搜索 JSON 的特征符 '{' ... '}'
                // 这种方法虽然 hacky，但在嵌入式场景下非常有效。
                
                char *payload = (char *)data->data_ptr;
                int len = data->data_len;
                
                // 简单的 JSON 提取器：寻找最外层的 {}
                int json_start = -1;
                int json_end = -1;
                int brace_count = 0;
                
                for (int i = 0; i < len; i++) {
                    if (payload[i] == '{') {
                        if (brace_count == 0) json_start = i;
                        brace_count++;
                    } else if (payload[i] == '}') {
                        brace_count--;
                        if (brace_count == 0 && json_start != -1) {
                            json_end = i;
                            // 找到第一个完整的 JSON 对象就停止，通常这就是我们要的 header+event
                            // 注意：飞书 protobuf 可能包含多个字段，但通常 JSON 在最后或者最明显的位置
                            // 如果提取出来的 JSON 包含 "schema": "2.0"，那就是它了
                            if (json_end > json_start + 10) { // 简单的长度过滤
                                break;
                            } else {
                                // 可能是个小的干扰项，重置继续找
                                json_start = -1; 
                            }
                        }
                    }
                }
                
                if (json_start != -1 && json_end != -1) {
                    int json_len = json_end - json_start + 1;
                    char *json_str = calloc(1, json_len + 1);
                    if (json_str) {
                        memcpy(json_str, payload + json_start, json_len);
                        json_str[json_len] = 0;
                        
                        ESP_LOGI(TAG, "Extracted JSON: %s", json_str);
                        
                        cJSON *json = cJSON_Parse(json_str);
                        if (json) {
                             // ... 这里的逻辑和之前一样 ...
                             // 2. 检查 "header" -> "event_type"
                             cJSON *header = cJSON_GetObjectItem(json, "header");
                             cJSON *event_type = cJSON_GetObjectItem(header, "event_type");
                             if (event_type) {
                                 ESP_LOGI(TAG, "Event Type: %s", event_type->valuestring);
                                 
                                 if (strcmp(event_type->valuestring, "im.message.receive_v1") == 0) {
                                     cJSON *event = cJSON_GetObjectItem(json, "event");
                                     cJSON *message = cJSON_GetObjectItem(event, "message");
                                     cJSON *content = cJSON_GetObjectItem(message, "content");
                                     // ESP_LOGI(TAG, "Msg Content: %s", content ? content->valuestring : "NULL");
                                     
                                     cJSON *sender = cJSON_GetObjectItem(event, "sender");
                                     cJSON *sender_id_obj = cJSON_GetObjectItem(sender, "sender_id");
                                     cJSON *open_id = cJSON_GetObjectItem(sender_id_obj, "open_id");
                                     
                                     cJSON *chat_id = cJSON_GetObjectItem(message, "chat_id");
                                     cJSON *chat_type = cJSON_GetObjectItem(message, "chat_type");
         
                                     // Determine reply target
                                     const char *target_id = NULL;
                                     if (chat_type && strcmp(chat_type->valuestring, "group") == 0) {
                                         if (chat_id) target_id = chat_id->valuestring;
                                     } else {
                                         if (open_id) target_id = open_id->valuestring;
                                     }
                                     
                                     if (content && target_id) {
                                         // Content is JSON string: "{\"text\":\"hello\"}"
                                         cJSON *content_json = cJSON_Parse(content->valuestring);
                                         if (content_json) {
                                             cJSON *text = cJSON_GetObjectItem(content_json, "text");
                                             if (text && text->valuestring) {
                                                 // Push to Bus
                                                 mimi_msg_t msg;
                                                 strncpy(msg.channel, MIMI_CHAN_FEISHU, sizeof(msg.channel)-1);
                                                 strncpy(msg.chat_id, target_id, sizeof(msg.chat_id)-1);
                                                 msg.content = strdup(text->valuestring);
                                                 
                                                 ESP_LOGI(TAG, "Pushing to bus: [%s] %s", target_id, text->valuestring);
                                                 
                                                 if (message_bus_push_inbound(&msg) != ESP_OK) {
                                                     ESP_LOGE(TAG, "Failed to push to bus");
                                                     free(msg.content);
                                                 }
                                             }
                                             cJSON_Delete(content_json);
                                         }
                                     }
                                 }
                             }
                            cJSON_Delete(json);
                        }
                        free(json_str);
                    }
                } else {
                    ESP_LOGD(TAG, "No JSON found in protobuf packet");
                }
            }
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WS Disconnected");
            break;
        default:
            break;
    }
}

static esp_err_t feishu_bot_start_ws(void) {
    if (s_is_running) return ESP_OK;
    
    // Feishu WS requires a specific endpoint: wss://open.feishu.cn/ws/v2/
    // Headers: Authorization: Bearer <token>
    
    if (feishu_ensure_token() != ESP_OK) {
        ESP_LOGE(TAG, "Cannot start WS without token");
        return ESP_FAIL;
    }

    // 将大缓冲区改为动态分配，避免占用栈空间
    char *resp_buf = heap_caps_calloc(1, 2048, MALLOC_CAP_SPIRAM);
    if (!resp_buf) {
        // 如果 SPIRAM 失败，尝试内部 RAM
        resp_buf = calloc(1, 2048);
        if (!resp_buf) return ESP_ERR_NO_MEM;
    }

    http_resp_t resp = {.buf = resp_buf, .len = 0, .cap = 2048};

    esp_http_client_config_t http_config = {
        // 使用抓包得到的地址，它在 SDK 日志里出现过
        .url = "https://open.feishu.cn/callback/ws/endpoint",
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 2048, // 减小 HTTP Client 内部缓冲区，避免栈溢出
        .buffer_size_tx = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    if (!client) {
        free(resp_buf);
        return ESP_FAIL;
    }

    // 构造请求体
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "AppID", s_app_id);     // 之前是 "app_id"，SDK 源码显示为 "AppID"
    cJSON_AddStringToObject(root, "AppSecret", s_app_secret); // 之前是 "app_secret"，SDK 源码显示为 "AppSecret"
    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_tenant_token);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    ESP_LOGI(TAG, "Fetching WS endpoint...");
    esp_err_t err = esp_http_client_perform(client);
    
    char ws_url[512] = {0};
    
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            ESP_LOGI(TAG, "Endpoint Resp: %s", resp.buf);
            cJSON *json = cJSON_Parse(resp.buf);
            if (json) {
                // 尝试解析 SDK 返回的结构：{"code":0,"data":{"URL":"wss://..."}}
                // 注意：字段名是 "URL" 而不是 "url"
                cJSON *data = cJSON_GetObjectItem(json, "data");
                if (data) {
                    cJSON *url = cJSON_GetObjectItem(data, "URL"); // 改为大写 URL
                    if (url && url->valuestring) {
                        strncpy(ws_url, url->valuestring, sizeof(ws_url) - 1);
                    }
                }
                cJSON_Delete(json);
            }
        } else {
            ESP_LOGE(TAG, "Endpoint Error Status: %d", status);
        }
    } else {
        ESP_LOGE(TAG, "Endpoint Request Failed: %s", esp_err_to_name(err));
    }

    free(post_data);
    esp_http_client_cleanup(client);
    free(resp_buf);

    if (strlen(ws_url) == 0) {
        ESP_LOGE(TAG, "Failed to get dynamic WS URL, falling back to static");
        // 如果获取失败，还是回退到静态地址试一下，或者直接报错
        strncpy(ws_url, "wss://open.feishu.cn/ws/v2/", sizeof(ws_url) - 1);
    } else {
        ESP_LOGI(TAG, "Got Dynamic WS URL: %s", ws_url);
    }

    esp_websocket_client_config_t config = {
        .uri = ws_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .reconnect_timeout_ms = 10000,
        .network_timeout_ms = 30000,
        .buffer_size = 1024 * 16, // 增加到 16KB，防止握手包过大导致栈溢出
        .task_stack = 8192, // 显式指定 WebSocket 任务栈大小
        .keep_alive_enable = true,
        .ping_interval_sec = 20,
        .user_agent = "ESP32-MimiClaw/1.0",
    };

    s_ws_client = esp_websocket_client_init(&config);
    if (!s_ws_client) return ESP_FAIL;

    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);
    
    // 复用上面的 auth_header，不需要重新声明
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_tenant_token);
    esp_websocket_client_append_header(s_ws_client, "Authorization", auth_header);
    
    // 恢复 App-Id Header，并移除自定义 SDK Version Header，避免干扰
    esp_websocket_client_append_header(s_ws_client, "App-Id", s_app_id);

    ESP_LOGI(TAG, "Connecting to Feishu WS with token prefix: %.10s...", s_tenant_token);
    // 复用 err 变量，不需要重新声明类型
    err = esp_websocket_client_start(s_ws_client);
    if (err == ESP_OK) {
        s_is_running = true;
        ESP_LOGI(TAG, "Feishu Bot Started");
    }
    return err;
}

static void feishu_ws_task(void *arg) {
    ESP_LOGI(TAG, "Feishu WS init task started");
    feishu_bot_start_ws();
    vTaskDelete(NULL);
}

esp_err_t feishu_bot_start(void) {
    // Only start if configured
    if (strlen(s_app_id) == 0 || strlen(s_app_secret) == 0) {
        ESP_LOGW(TAG, "Feishu not configured, skipping start");
        return ESP_OK;
    }
    
    // Offload WS startup to a separate task to save stack space in the calling context
    // Increase stack to 12KB to handle MbedTLS handshake overhead
    xTaskCreate(feishu_ws_task, "feishu_ws_init", 12288, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t feishu_bot_init(void) {
    // Load from NVS
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_FEISHU, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_app_id);
        nvs_get_str(nvs, MIMI_NVS_KEY_FEISHU_APP_ID, s_app_id, &len);
        len = sizeof(s_app_secret);
        nvs_get_str(nvs, MIMI_NVS_KEY_FEISHU_SECRET, s_app_secret, &len);
        nvs_close(nvs);
    }
    
    if (strlen(s_app_id) == 0) {
        ESP_LOGW(TAG, "Feishu App ID not set");
    }
    return ESP_OK;
}

esp_err_t feishu_set_config(const char *app_id, const char *app_secret) {
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_FEISHU, NVS_READWRITE, &nvs) != ESP_OK) return ESP_FAIL;
    
    nvs_set_str(nvs, MIMI_NVS_KEY_FEISHU_APP_ID, app_id);
    nvs_set_str(nvs, MIMI_NVS_KEY_FEISHU_SECRET, app_secret);
    nvs_commit(nvs);
    nvs_close(nvs);
    
    strncpy(s_app_id, app_id, sizeof(s_app_id)-1);
    strncpy(s_app_secret, app_secret, sizeof(s_app_secret)-1);
    
    // Force token refresh
    s_token_expire_ts = 0;
    
    // Restart WS if running
    if (s_is_running) {
        esp_websocket_client_stop(s_ws_client);
        esp_websocket_client_destroy(s_ws_client);
        s_is_running = false;
        feishu_bot_start_ws();
    }
    
    return ESP_OK;
}
