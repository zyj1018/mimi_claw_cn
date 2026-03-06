#include "tool_kimi_search.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "kimi_search";

static char s_kimi_api_key[128] = {0};

#define KIMI_SEARCH_BUF_SIZE     (32 * 1024)
#define KIMI_API_URL             "https://api.moonshot.cn/v1/chat/completions"
#define KIMI_API_HOST            "api.moonshot.cn"

/* ── Response buffer ──────────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} search_buf_t;

static esp_err_t resp_buf_init(search_buf_t *rb, size_t initial_cap)
{
    rb->data = heap_caps_calloc(1, initial_cap, MALLOC_CAP_SPIRAM);
    if (!rb->data) return ESP_ERR_NO_MEM;
    rb->len = 0;
    rb->cap = initial_cap;
    return ESP_OK;
}

static esp_err_t resp_buf_append(search_buf_t *rb, const char *data, size_t len)
{
    while (rb->len + len >= rb->cap) {
        size_t new_cap = rb->cap * 2;
        char *tmp = heap_caps_realloc(rb->data, new_cap, MALLOC_CAP_SPIRAM);
        if (!tmp) return ESP_ERR_NO_MEM;
        rb->data = tmp;
        rb->cap = new_cap;
    }
    memcpy(rb->data + rb->len, data, len);
    rb->len += len;
    rb->data[rb->len] = '\0';
    return ESP_OK;
}

static void resp_buf_free(search_buf_t *rb)
{
    free(rb->data);
    rb->data = NULL;
    rb->len = 0;
    rb->cap = 0;
}

/* ── HTTP event handler ───────────────────────────────────────── */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    search_buf_t *rb = (search_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        resp_buf_append(rb, (const char *)evt->data, evt->data_len);
    }
    return ESP_OK;
}

/* ── Init ─────────────────────────────────────────────────────── */

esp_err_t tool_kimi_search_init(void)
{
    /* Start with build-time default */
    if (MIMI_SECRET_API_KEY[0] != '\0') {
        strncpy(s_kimi_api_key, MIMI_SECRET_API_KEY, sizeof(s_kimi_api_key) - 1);
    }

    /* NVS overrides take highest priority (set via CLI) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_LLM, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[128] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_kimi_api_key, tmp, sizeof(s_kimi_api_key) - 1);
        }
        nvs_close(nvs);
    }

    if (s_kimi_api_key[0]) {
        ESP_LOGI(TAG, "Kimi search initialized (API key configured)");
    } else {
        ESP_LOGW(TAG, "No Kimi API key. Use CLI: set_api_key <KEY>");
    }
    return ESP_OK;
}

bool tool_kimi_search_is_available(void)
{
    return s_kimi_api_key[0] != '\0';
}

/* ── Build tool response request ──────────────────────────────── */

static char *build_tool_response_request(const char *original_query, 
                                          const char *tool_call_id,
                                          const char *tool_result)
{
    cJSON *body = cJSON_CreateObject();
    if (!body) return NULL;

    cJSON_AddStringToObject(body, "model", "moonshot-v1-8k");
    cJSON_AddNumberToObject(body, "max_tokens", 2048);

    /* Build messages array with tool response */
    cJSON *messages = cJSON_CreateArray();
    if (messages) {
        /* Original user query */
        cJSON *user_msg = cJSON_CreateObject();
        if (user_msg) {
            cJSON_AddStringToObject(user_msg, "role", "user");
            cJSON_AddStringToObject(user_msg, "content", original_query);
            cJSON_AddItemToArray(messages, user_msg);
        }

        /* Assistant's tool call request */
        cJSON *assistant_msg = cJSON_CreateObject();
        if (assistant_msg) {
            cJSON_AddStringToObject(assistant_msg, "role", "assistant");
            cJSON_AddStringToObject(assistant_msg, "content", "");
            
            cJSON *tool_calls = cJSON_CreateArray();
            if (tool_calls) {
                cJSON *tc = cJSON_CreateObject();
                if (tc) {
                    cJSON_AddStringToObject(tc, "id", tool_call_id);
                    cJSON_AddStringToObject(tc, "type", "builtin_function");
                    cJSON_AddStringToObject(tc, "function", "$web_search");
                    cJSON_AddItemToArray(tool_calls, tc);
                }
                cJSON_AddItemToObject(assistant_msg, "tool_calls", tool_calls);
            }
            cJSON_AddItemToArray(messages, assistant_msg);
        }

        /* Tool response with role=tool */
        cJSON *tool_msg = cJSON_CreateObject();
        if (tool_msg) {
            cJSON_AddStringToObject(tool_msg, "role", "tool");
            cJSON_AddStringToObject(tool_msg, "tool_call_id", tool_call_id);
            cJSON_AddStringToObject(tool_msg, "content", tool_result);
            cJSON_AddItemToArray(messages, tool_msg);
        }

        cJSON_AddItemToObject(body, "messages", messages);
    }

    char *result = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    return result;
}

/* ── Parse search results from response ───────────────────────── */

static void parse_search_results(cJSON *root, char *output, size_t output_size)
{
    output[0] = '\0';

    /* Get the main content from the response */
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (!choices || !cJSON_IsArray(choices)) {
        snprintf(output, output_size, "Error: No choices in response");
        return;
    }

    cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
    if (!choice0) {
        snprintf(output, output_size, "Error: No choice in response");
        return;
    }

    cJSON *message = cJSON_GetObjectItem(choice0, "message");
    if (!message) {
        snprintf(output, output_size, "Error: No message in response");
        return;
    }

    /* Get the main content */
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content && cJSON_IsString(content) && content->valuestring[0]) {
        strncpy(output, content->valuestring, output_size - 1);
        output[output_size - 1] = '\0';
        return;
    }

    snprintf(output, output_size, "No search results found.");
}

/* ── Direct HTTPS request ─────────────────────────────────────── */

static esp_err_t kimi_api_request(const char *post_data, search_buf_t *rb)
{
    esp_http_client_config_t config = {
        .url = KIMI_API_URL,
        .event_handler = http_event_handler,
        .user_data = rb,
        .timeout_ms = 60000,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    /* Kimi uses Bearer token authentication */
    char auth[160];
    snprintf(auth, sizeof(auth), "Bearer %s", s_kimi_api_key);
    esp_http_client_set_header(client, "Authorization", auth);

    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) return err;
    if (status != 200) {
        ESP_LOGE(TAG, "Kimi API returned %d, response: %s", status, rb->data ? rb->data : "(empty)");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Proxy HTTPS request ──────────────────────────────────────── */

static esp_err_t kimi_api_request_via_proxy(const char *post_data, search_buf_t *rb)
{
    proxy_conn_t *conn = proxy_conn_open(KIMI_API_HOST, 443, 60000);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    int body_len = strlen(post_data);
    char header[1024];
    int hlen = snprintf(header, sizeof(header),
        "POST /v1/chat/completions HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: Bearer %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        KIMI_API_HOST, s_kimi_api_key, body_len);

    if (proxy_conn_write(conn, header, hlen) < 0 ||
        proxy_conn_write(conn, post_data, body_len) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    /* Read full response */
    char tmp[4096];
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 60000);
        if (n <= 0) break;
        if (resp_buf_append(rb, tmp, n) != ESP_OK) break;
    }
    proxy_conn_close(conn);

    /* Parse status line */
    int status = 0;
    if (rb->len > 5 && strncmp(rb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(rb->data, ' ');
        if (sp) status = atoi(sp + 1);
    }

    /* Strip HTTP headers, keep body only */
    char *body = strstr(rb->data, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t blen = rb->len - (body - rb->data);
        memmove(rb->data, body, blen);
        rb->len = blen;
        rb->data[rb->len] = '\0';
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Kimi API returned %d via proxy, response: %s", status, rb->data ? rb->data : "(empty)");
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* ── Execute ──────────────────────────────────────────────────── */

esp_err_t tool_kimi_search_execute(const char *input_json, char *output, size_t output_size)
{
    if (s_kimi_api_key[0] == '\0') {
        snprintf(output, output_size, "Error: No Kimi API key configured.");
        return ESP_ERR_INVALID_STATE;
    }

    /* Parse input to get query and tool call info */
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *query = cJSON_GetObjectItem(input, "query");
    if (!query || !cJSON_IsString(query) || query->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Missing 'query' field");
        return ESP_ERR_INVALID_ARG;
    }

    /* Get tool call ID if provided (from LLM tool call) */
    cJSON *tool_call_id = cJSON_GetObjectItem(input, "tool_call_id");
    const char *tc_id = (tool_call_id && cJSON_IsString(tool_call_id)) ? 
                        tool_call_id->valuestring : "web_search:0";

    const char *search_query = query->valuestring;
    ESP_LOGI(TAG, "Kimi searching: %s (tool_call_id: %s)", search_query, tc_id);

    /* Build tool response - for Kimi builtin_function, we just echo back the query */
    /* Kimi will perform the actual search on its server */
    char tool_result[256];
    snprintf(tool_result, sizeof(tool_result), "{\"query\": \"%s\"}", search_query);

    char *post_data = build_tool_response_request(search_query, tc_id, tool_result);
    cJSON_Delete(input);

    if (!post_data) {
        snprintf(output, output_size, "Error: Failed to build tool response");
        return ESP_ERR_NO_MEM;
    }

    search_buf_t rb;
    if (resp_buf_init(&rb, KIMI_SEARCH_BUF_SIZE) != ESP_OK) {
        free(post_data);
        snprintf(output, output_size, "Error: Out of memory");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err;
    if (http_proxy_is_enabled()) {
        err = kimi_api_request_via_proxy(post_data, &rb);
    } else {
        err = kimi_api_request(post_data, &rb);
    }

    free(post_data);

    if (err != ESP_OK) {
        resp_buf_free(&rb);
        snprintf(output, output_size, "Error: Kimi API request failed");
        return err;
    }

    ESP_LOGI(TAG, "Kimi API response: %s", rb.data ? rb.data : "(empty)");

    /* Parse response */
    cJSON *root = cJSON_Parse(rb.data);
    resp_buf_free(&rb);

    if (!root) {
        snprintf(output, output_size, "Error: Failed to parse response");
        return ESP_FAIL;
    }

    /* Check for API errors */
    cJSON *error = cJSON_GetObjectItem(root, "error");
    if (error) {
        cJSON *msg = cJSON_GetObjectItem(error, "message");
        snprintf(output, output_size, "Error: %s", 
                 (msg && cJSON_IsString(msg)) ? msg->valuestring : "Kimi API error");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* Parse final results */
    parse_search_results(root, output, output_size);
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Kimi search complete, %d bytes result", (int)strlen(output));
    return ESP_OK;
}

/* ── Set API key ──────────────────────────────────────────────── */

esp_err_t tool_kimi_search_set_key(const char *api_key)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_API_KEY, api_key));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_kimi_api_key, api_key, sizeof(s_kimi_api_key) - 1);
    ESP_LOGI(TAG, "Kimi API key saved");
    return ESP_OK;
}
