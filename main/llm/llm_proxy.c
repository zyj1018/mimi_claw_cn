#include "llm_proxy.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_tls.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "cJSON.h"
#include <time.h>
#include <netdb.h>
#include <sys/socket.h>

#include "mbedtls/platform.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/esp_debug.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"

static const char *TAG = "llm";

#define LLM_API_KEY_MAX_LEN 320
#define LLM_MODEL_MAX_LEN   64
#define LLM_API_URL_MAX_LEN 128
#define LLM_DUMP_MAX_BYTES   (16 * 1024)
#define LLM_DUMP_CHUNK_BYTES 320

static char s_api_key[LLM_API_KEY_MAX_LEN] = {0};
static char s_model[LLM_MODEL_MAX_LEN] = MIMI_LLM_DEFAULT_MODEL;
static char s_provider[16] = MIMI_LLM_PROVIDER_DEFAULT;
static char s_api_url[LLM_API_URL_MAX_LEN] = {0};

static void llm_log_payload(const char *label, const char *payload)
{
    if (!payload) {
        ESP_LOGI(TAG, "%s: <null>", label);
        return;
    }

    size_t total = strlen(payload);
#if MIMI_LLM_LOG_VERBOSE_PAYLOAD
    size_t shown = total > LLM_DUMP_MAX_BYTES ? LLM_DUMP_MAX_BYTES : total;
    ESP_LOGI(TAG, "%s (%u bytes)%s",
             label,
             (unsigned)total,
             (shown < total) ? " [truncated]" : "");

    char chunk[LLM_DUMP_CHUNK_BYTES + 1];
    for (size_t off = 0; off < shown; off += LLM_DUMP_CHUNK_BYTES) {
        size_t n = shown - off;
        if (n > LLM_DUMP_CHUNK_BYTES) {
            n = LLM_DUMP_CHUNK_BYTES;
        }
        memcpy(chunk, payload + off, n);
        chunk[n] = '\0';
        ESP_LOGI(TAG, "%s[%u]: %s", label, (unsigned)off, chunk);
    }
#else
    if (MIMI_LLM_LOG_PREVIEW_BYTES > 0) {
        size_t shown = total > MIMI_LLM_LOG_PREVIEW_BYTES ? MIMI_LLM_LOG_PREVIEW_BYTES : total;
        char preview[MIMI_LLM_LOG_PREVIEW_BYTES + 1];
        memcpy(preview, payload, shown);
        preview[shown] = '\0';
        for (size_t i = 0; i < shown; i++) {
            if (preview[i] == '\n' || preview[i] == '\r' || preview[i] == '\t') {
                preview[i] = ' ';
            }
        }
        ESP_LOGI(TAG, "%s (%u bytes): %s%s",
                 label,
                 (unsigned)total,
                 preview,
                 (shown < total) ? " ..." : "");
    } else {
        ESP_LOGI(TAG, "%s (%u bytes)", label, (unsigned)total);
    }
#endif
}

static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strnlen(src, dst_size - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* ── Response buffer ──────────────────────────────────────────── */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} resp_buf_t;

static esp_err_t resp_buf_init(resp_buf_t *rb, size_t initial_cap)
{
    rb->data = heap_caps_calloc(1, initial_cap, MALLOC_CAP_SPIRAM);
    if (!rb->data) return ESP_ERR_NO_MEM;
    rb->len = 0;
    rb->cap = initial_cap;
    return ESP_OK;
}

static esp_err_t resp_buf_append(resp_buf_t *rb, const char *data, size_t len)
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

static void resp_buf_free(resp_buf_t *rb)
{
    free(rb->data);
    rb->data = NULL;
    rb->len = 0;
    rb->cap = 0;
}

/* ── Shared HTTP dispatch ─────────────────────────────────────── */

static bool provider_is_openai_compatible(void)
{
    /* If explicitly anthropic, then no. Otherwise default to OpenAI compatible. */
    if (strcmp(s_provider, "anthropic") == 0) {
        return false;
    }
    return true;
}

static const char *llm_api_url(void)
{
    if (s_api_url[0] != '\0') {
        return s_api_url;
    }
    return provider_is_openai_compatible() ? MIMI_OPENAI_API_URL : MIMI_LLM_API_URL;
}

/* Simple URL parser for proxy path */
static void get_host_path_from_url(const char *url, char *host, size_t host_len, char *path, size_t path_len)
{
    /* Default fallback */
    safe_copy(host, host_len, provider_is_openai_compatible() ? "api.openai.com" : "api.anthropic.com");
    safe_copy(path, path_len, provider_is_openai_compatible() ? "/v1/chat/completions" : "/v1/messages");

    if (!url || url[0] == '\0') return;

    /* Skip scheme */
    const char *p = strstr(url, "://");
    if (p) {
        p += 3;
    } else {
        p = url;
    }

    /* Find path start */
    const char *slash = strchr(p, '/');
    if (slash) {
        size_t hlen = slash - p;
        if (hlen >= host_len) hlen = host_len - 1;
        memcpy(host, p, hlen);
        host[hlen] = '\0';

        safe_copy(path, path_len, slash);
    } else {
        safe_copy(host, host_len, p);
        safe_copy(path, path_len, "/");
    }
}

/* ── Init ─────────────────────────────────────────────────────── */

esp_err_t llm_proxy_init(void)
{
    /* Start with build-time defaults */
    if (MIMI_SECRET_API_KEY[0] != '\0') {
        safe_copy(s_api_key, sizeof(s_api_key), MIMI_SECRET_API_KEY);
    }
    if (MIMI_SECRET_MODEL[0] != '\0') {
        safe_copy(s_model, sizeof(s_model), MIMI_SECRET_MODEL);
    }
    if (MIMI_SECRET_MODEL_PROVIDER[0] != '\0') {
        safe_copy(s_provider, sizeof(s_provider), MIMI_SECRET_MODEL_PROVIDER);
    }
    /* No build-time secret for URL yet, but could be added. Defaults to empty. */

    /* NVS overrides take highest priority (set via CLI) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_LLM, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[LLM_API_KEY_MAX_LEN] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_API_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            safe_copy(s_api_key, sizeof(s_api_key), tmp);
        }
        char model_tmp[LLM_MODEL_MAX_LEN] = {0};
        len = sizeof(model_tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_MODEL, model_tmp, &len) == ESP_OK && model_tmp[0]) {
            safe_copy(s_model, sizeof(s_model), model_tmp);
        }
        char provider_tmp[16] = {0};
        len = sizeof(provider_tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_PROVIDER, provider_tmp, &len) == ESP_OK && provider_tmp[0]) {
            safe_copy(s_provider, sizeof(s_provider), provider_tmp);
        }
        char url_tmp[LLM_API_URL_MAX_LEN] = {0};
        len = sizeof(url_tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_API_URL, url_tmp, &len) == ESP_OK && url_tmp[0]) {
            safe_copy(s_api_url, sizeof(s_api_url), url_tmp);
        }
        nvs_close(nvs);
    }

    if (s_api_key[0]) {
        ESP_LOGI(TAG, "LLM proxy initialized (provider: %s, model: %s)", s_provider, s_model);
        if (s_api_url[0]) {
            ESP_LOGI(TAG, "Custom API URL: %s", s_api_url);
        }
    } else {
        ESP_LOGW(TAG, "No API key. Use CLI: set_api_key <KEY>");
    }
    return ESP_OK;
}

/* ── Raw MbedTLS (Insecure) ─────────────────────────────────── */

static esp_err_t llm_http_raw_mbedtls(const char *post_data, resp_buf_t *rb, int *out_status)
{
    char host[128];
    char path[128];
    get_host_path_from_url(llm_api_url(), host, sizeof(host), path, sizeof(path));
    
    char port_str[6] = "443";
    
    ESP_LOGI(TAG, "Connecting to %s:443 (Raw MbedTLS, Insecure)", host);

    mbedtls_net_context server_fd;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    int ret;
    esp_err_t esp_ret = ESP_FAIL;

    mbedtls_net_init(&server_fd);
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    if ((ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0)) != 0) {
        ESP_LOGE(TAG, "mbedtls_ctr_drbg_seed failed: -0x%x", -ret);
        goto exit;
    }

    if ((ret = mbedtls_net_connect(&server_fd, host, port_str, MBEDTLS_NET_PROTO_TCP)) != 0) {
        ESP_LOGE(TAG, "mbedtls_net_connect failed: -0x%x", -ret);
        goto exit;
    }

    if ((ret = mbedtls_ssl_config_defaults(&conf,
                    MBEDTLS_SSL_IS_CLIENT,
                    MBEDTLS_SSL_TRANSPORT_STREAM,
                    MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        ESP_LOGE(TAG, "mbedtls_ssl_config_defaults failed: -0x%x", -ret);
        goto exit;
    }

    /* CRITICAL: Disable verification */
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

    if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0) {
        ESP_LOGE(TAG, "mbedtls_ssl_setup failed: -0x%x", -ret);
        goto exit;
    }

    if ((ret = mbedtls_ssl_set_hostname(&ssl, host)) != 0) {
        ESP_LOGE(TAG, "mbedtls_ssl_set_hostname failed: -0x%x", -ret);
        goto exit;
    }

    /* MbedTLS 3.x requires 5 arguments (timeout callback). Pass NULL. */
    mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            ESP_LOGE(TAG, "mbedtls_ssl_handshake failed: -0x%x", -ret);
            goto exit;
        }
    }

    ESP_LOGI(TAG, "SSL handshake success");

    /* Send HTTP Request */
    char header[1024];
    int body_len = strlen(post_data);
    int hlen;
    
    if (provider_is_openai_compatible()) {
        hlen = snprintf(header, sizeof(header),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "Authorization: Bearer %s\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n",
            path, host, s_api_key, body_len);
    } else {
        hlen = snprintf(header, sizeof(header),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "x-api-key: %s\r\n"
            "anthropic-version: %s\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n",
            path, host, s_api_key, MIMI_LLM_API_VERSION, body_len);
    }

    /* Write Header */
    int written = 0;
    while (written < hlen) {
        ret = mbedtls_ssl_write(&ssl, (const unsigned char *)header + written, hlen - written);
        if (ret > 0) written += ret;
        else if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            ESP_LOGE(TAG, "mbedtls_ssl_write header failed: -0x%x", -ret);
            goto exit;
        }
    }

    /* Write Body */
    written = 0;
    while (written < body_len) {
        ret = mbedtls_ssl_write(&ssl, (const unsigned char *)post_data + written, body_len - written);
        if (ret > 0) written += ret;
        else if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            ESP_LOGE(TAG, "mbedtls_ssl_write body failed: -0x%x", -ret);
            goto exit;
        }
    }

    /* Read Response */
    unsigned char tmp[1024];
    while (1) {
        ret = mbedtls_ssl_read(&ssl, tmp, sizeof(tmp));
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) break;
        if (ret < 0) {
            ESP_LOGE(TAG, "mbedtls_ssl_read failed: -0x%x", -ret);
            goto exit;
        }
        if (resp_buf_append(rb, (char *)tmp, ret) != ESP_OK) {
            esp_ret = ESP_ERR_NO_MEM;
            goto exit;
        }
    }

    esp_ret = ESP_OK;

    /* Parse status line */
    *out_status = 0;
    if (rb->len > 5 && strncmp(rb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(rb->data, ' ');
        if (sp) *out_status = atoi(sp + 1);
    }

    /* Strip HTTP headers */
    char *body = strstr(rb->data, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t blen = rb->len - (body - rb->data);
        memmove(rb->data, body, blen);
        rb->len = blen;
        rb->data[rb->len] = '\0';
    }

exit:
    mbedtls_net_free(&server_fd);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return esp_ret;
}

/* ── Direct path: esp_http_client ───────────────────────────── */

static esp_err_t llm_http_direct(const char *post_data, resp_buf_t *rb, int *out_status)
{
    // Redirect to Raw MbedTLS implementation to bypass verification issues
    return llm_http_raw_mbedtls(post_data, rb, out_status);
}

/* ── Proxy path: manual HTTP over CONNECT tunnel ────────────── */

static esp_err_t llm_http_via_proxy(const char *post_data, resp_buf_t *rb, int *out_status)
{
    char host[128];
    char path[128];
    get_host_path_from_url(llm_api_url(), host, sizeof(host), path, sizeof(path));

    /* Note: proxy_conn_open needs a port. We assume 443 for HTTPS. 
       If custom URL uses custom port, this simple parser won't catch it. 
       Users should use direct mode or standard ports. */
    proxy_conn_t *conn = proxy_conn_open(host, 443, 30000);
    if (!conn) return ESP_ERR_HTTP_CONNECT;

    int body_len = strlen(post_data);
    char header[1024];
    int hlen = 0;
    if (provider_is_openai_compatible()) {
        hlen = snprintf(header, sizeof(header),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "Authorization: Bearer %s\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n",
            path, host, s_api_key, body_len);
    } else {
        hlen = snprintf(header, sizeof(header),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Content-Type: application/json\r\n"
            "x-api-key: %s\r\n"
            "anthropic-version: %s\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n",
            path, host, s_api_key, MIMI_LLM_API_VERSION, body_len);
    }

    if (proxy_conn_write(conn, header, hlen) < 0 ||
        proxy_conn_write(conn, post_data, body_len) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    /* Read full response into buffer */
    char tmp[4096];
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 120000);
        if (n <= 0) break;
        if (resp_buf_append(rb, tmp, n) != ESP_OK) break;
    }
    proxy_conn_close(conn);

    /* Parse status line */
    *out_status = 0;
    if (rb->len > 5 && strncmp(rb->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(rb->data, ' ');
        if (sp) *out_status = atoi(sp + 1);
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

    return ESP_OK;
}



/* ── Shared HTTP dispatch ─────────────────────────────────────── */

static esp_err_t llm_http_call(const char *post_data, resp_buf_t *rb, int *out_status)
{
    if (http_proxy_is_enabled()) {
        return llm_http_via_proxy(post_data, rb, out_status);
    } else {
        return llm_http_direct(post_data, rb, out_status);
    }
}

static cJSON *convert_tools_openai(const char *tools_json)
{
    if (!tools_json) return NULL;
    cJSON *arr = cJSON_Parse(tools_json);
    if (!arr || !cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return NULL;
    }
    cJSON *out = cJSON_CreateArray();
    cJSON *tool;
    cJSON_ArrayForEach(tool, arr) {
        cJSON *name = cJSON_GetObjectItem(tool, "name");
        cJSON *desc = cJSON_GetObjectItem(tool, "description");
        cJSON *schema = cJSON_GetObjectItem(tool, "input_schema");
        if (!name || !cJSON_IsString(name)) continue;

        cJSON *func = cJSON_CreateObject();
        cJSON_AddStringToObject(func, "name", name->valuestring);
        if (desc && cJSON_IsString(desc)) {
            cJSON_AddStringToObject(func, "description", desc->valuestring);
        }
        if (schema) {
            cJSON_AddItemToObject(func, "parameters", cJSON_Duplicate(schema, 1));
        }

        cJSON *wrap = cJSON_CreateObject();
        cJSON_AddStringToObject(wrap, "type", "function");
        cJSON_AddItemToObject(wrap, "function", func);
        cJSON_AddItemToArray(out, wrap);
    }
    cJSON_Delete(arr);
    return out;
}

static cJSON *convert_messages_openai(const char *system_prompt, cJSON *messages)
{
    cJSON *out = cJSON_CreateArray();
    if (system_prompt && system_prompt[0]) {
        cJSON *sys = cJSON_CreateObject();
        cJSON_AddStringToObject(sys, "role", "system");
        cJSON_AddStringToObject(sys, "content", system_prompt);
        cJSON_AddItemToArray(out, sys);
    }

    if (!messages || !cJSON_IsArray(messages)) return out;

    cJSON *msg;
    cJSON_ArrayForEach(msg, messages) {
        cJSON *role = cJSON_GetObjectItem(msg, "role");
        cJSON *content = cJSON_GetObjectItem(msg, "content");
        if (!role || !cJSON_IsString(role)) continue;

        if (content && cJSON_IsString(content)) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", role->valuestring);
            cJSON_AddStringToObject(m, "content", content->valuestring);
            cJSON_AddItemToArray(out, m);
            continue;
        }

        if (!content || !cJSON_IsArray(content)) continue;

        if (strcmp(role->valuestring, "assistant") == 0) {
            cJSON *m = cJSON_CreateObject();
            cJSON_AddStringToObject(m, "role", "assistant");

            /* collect text */
            char *text_buf = NULL;
            size_t off = 0;
            cJSON *block;
            cJSON *tool_calls = NULL;
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (text && cJSON_IsString(text)) {
                        size_t tlen = strlen(text->valuestring);
                        char *tmp = realloc(text_buf, off + tlen + 1);
                        if (tmp) {
                            text_buf = tmp;
                            memcpy(text_buf + off, text->valuestring, tlen);
                            off += tlen;
                            text_buf[off] = '\0';
                        }
                    }
                } else if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "tool_use") == 0) {
                    if (!tool_calls) tool_calls = cJSON_CreateArray();
                    cJSON *id = cJSON_GetObjectItem(block, "id");
                    cJSON *name = cJSON_GetObjectItem(block, "name");
                    cJSON *input = cJSON_GetObjectItem(block, "input");
                    if (!name || !cJSON_IsString(name)) continue;

                    cJSON *tc = cJSON_CreateObject();
                    if (id && cJSON_IsString(id)) {
                        cJSON_AddStringToObject(tc, "id", id->valuestring);
                    }
                    cJSON_AddStringToObject(tc, "type", "function");
                    cJSON *func = cJSON_CreateObject();
                    cJSON_AddStringToObject(func, "name", name->valuestring);
                    if (input) {
                        char *args = cJSON_PrintUnformatted(input);
                        if (args) {
                            cJSON_AddStringToObject(func, "arguments", args);
                            free(args);
                        }
                    }
                    cJSON_AddItemToObject(tc, "function", func);
                    cJSON_AddItemToArray(tool_calls, tc);
                }
            }
            if (text_buf) {
                cJSON_AddStringToObject(m, "content", text_buf);
            } else {
                cJSON_AddStringToObject(m, "content", "");
            }
            if (tool_calls) {
                cJSON_AddItemToObject(m, "tool_calls", tool_calls);
            }
            cJSON_AddItemToArray(out, m);
            free(text_buf);
        } else if (strcmp(role->valuestring, "user") == 0) {
            /* tool_result blocks become role=tool */
            cJSON *block;
            bool has_user_text = false;
            char *text_buf = NULL;
            size_t off = 0;
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "tool_result") == 0) {
                    cJSON *tool_id = cJSON_GetObjectItem(block, "tool_use_id");
                    cJSON *tcontent = cJSON_GetObjectItem(block, "content");
                    if (!tool_id || !cJSON_IsString(tool_id)) continue;
                    cJSON *tm = cJSON_CreateObject();
                    cJSON_AddStringToObject(tm, "role", "tool");
                    cJSON_AddStringToObject(tm, "tool_call_id", tool_id->valuestring);
                    if (tcontent && cJSON_IsString(tcontent)) {
                        cJSON_AddStringToObject(tm, "content", tcontent->valuestring);
                    } else {
                        cJSON_AddStringToObject(tm, "content", "");
                    }
                    cJSON_AddItemToArray(out, tm);
                } else if (btype && cJSON_IsString(btype) && strcmp(btype->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (text && cJSON_IsString(text)) {
                        size_t tlen = strlen(text->valuestring);
                        char *tmp = realloc(text_buf, off + tlen + 1);
                        if (tmp) {
                            text_buf = tmp;
                            memcpy(text_buf + off, text->valuestring, tlen);
                            off += tlen;
                            text_buf[off] = '\0';
                        }
                        has_user_text = true;
                    }
                }
            }
            if (has_user_text) {
                cJSON *um = cJSON_CreateObject();
                cJSON_AddStringToObject(um, "role", "user");
                cJSON_AddStringToObject(um, "content", text_buf);
                cJSON_AddItemToArray(out, um);
            }
            free(text_buf);
        }
    }

    return out;
}

/* ── Public: chat with tools (non-streaming) ──────────────────── */

void llm_response_free(llm_response_t *resp)
{
    free(resp->text);
    resp->text = NULL;
    resp->text_len = 0;
    for (int i = 0; i < resp->call_count; i++) {
        free(resp->calls[i].input);
        resp->calls[i].input = NULL;
    }
    resp->call_count = 0;
    resp->tool_use = false;
}

esp_err_t llm_chat_tools(const char *system_prompt,
                         cJSON *messages,
                         const char *tools_json,
                         llm_response_t *resp)
{
    memset(resp, 0, sizeof(*resp));

    if (s_api_key[0] == '\0') return ESP_ERR_INVALID_STATE;

    /* Build request body (non-streaming) */
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", s_model);
    if (provider_is_openai_compatible()) {
        /* Some OpenAI compatible APIs might prefer max_tokens, but max_completion_tokens is newer standard for O1/etc. 
           We can stick to max_tokens for broader compatibility if needed, but OpenAI recommends max_completion_tokens now.
           However, most clones (Moonshot/Doubao) use max_tokens. Let's switch to max_tokens for compatibility! */
        cJSON_AddNumberToObject(body, "max_tokens", MIMI_LLM_MAX_TOKENS);
    } else {
        cJSON_AddNumberToObject(body, "max_tokens", MIMI_LLM_MAX_TOKENS);
    }

    if (provider_is_openai_compatible()) {
        cJSON *openai_msgs = convert_messages_openai(system_prompt, messages);
        cJSON_AddItemToObject(body, "messages", openai_msgs);

        if (tools_json) {
            cJSON *tools = convert_tools_openai(tools_json);
            if (tools) {
                cJSON_AddItemToObject(body, "tools", tools);
                cJSON_AddStringToObject(body, "tool_choice", "auto");
            }
        }
    } else {
        cJSON_AddStringToObject(body, "system", system_prompt);

        /* Deep-copy messages so caller keeps ownership */
        cJSON *msgs_copy = cJSON_Duplicate(messages, 1);
        cJSON_AddItemToObject(body, "messages", msgs_copy);

        /* Add tools array if provided */
        if (tools_json) {
            cJSON *tools = cJSON_Parse(tools_json);
            if (tools) {
                cJSON_AddItemToObject(body, "tools", tools);
            }
        }
    }

    char *post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "Calling LLM API with tools (provider: %s, model: %s, body: %d bytes)",
             s_provider, s_model, (int)strlen(post_data));
    llm_log_payload("LLM tools request", post_data);

    /* HTTP call */
    resp_buf_t rb;
    if (resp_buf_init(&rb, MIMI_LLM_STREAM_BUF_SIZE) != ESP_OK) {
        free(post_data);
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    esp_err_t err = llm_http_call(post_data, &rb, &status);
    free(post_data);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        llm_log_payload("LLM tools partial response", rb.data);
        resp_buf_free(&rb);
        return err;
    }

    llm_log_payload("LLM tools raw response", rb.data);

    if (status != 200) {
        ESP_LOGE(TAG, "API error %d: %.500s", status, rb.data ? rb.data : "");
        resp_buf_free(&rb);
        return ESP_FAIL;
    }

    /* Parse full JSON response */
    cJSON *root = cJSON_Parse(rb.data);
    resp_buf_free(&rb);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse API response JSON");
        return ESP_FAIL;
    }

    if (provider_is_openai_compatible()) {
        cJSON *choices = cJSON_GetObjectItem(root, "choices");
        cJSON *choice0 = choices && cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : NULL;
        if (choice0) {
            cJSON *finish = cJSON_GetObjectItem(choice0, "finish_reason");
            if (finish && cJSON_IsString(finish)) {
                resp->tool_use = (strcmp(finish->valuestring, "tool_calls") == 0);
            }

            cJSON *message = cJSON_GetObjectItem(choice0, "message");
            if (message) {
                cJSON *content = cJSON_GetObjectItem(message, "content");
                if (content && cJSON_IsString(content)) {
                    size_t tlen = strlen(content->valuestring);
                    resp->text = calloc(1, tlen + 1);
                    if (resp->text) {
                        memcpy(resp->text, content->valuestring, tlen);
                        resp->text_len = tlen;
                    }
                }

                cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
                if (tool_calls && cJSON_IsArray(tool_calls)) {
                    cJSON *tc;
                    cJSON_ArrayForEach(tc, tool_calls) {
                        if (resp->call_count >= MIMI_MAX_TOOL_CALLS) break;
                        llm_tool_call_t *call = &resp->calls[resp->call_count];
                        cJSON *id = cJSON_GetObjectItem(tc, "id");
                        cJSON *func = cJSON_GetObjectItem(tc, "function");
                        if (id && cJSON_IsString(id)) {
                            strncpy(call->id, id->valuestring, sizeof(call->id) - 1);
                        }
                        if (func) {
                            cJSON *name = cJSON_GetObjectItem(func, "name");
                            cJSON *args = cJSON_GetObjectItem(func, "arguments");
                            if (name && cJSON_IsString(name)) {
                                strncpy(call->name, name->valuestring, sizeof(call->name) - 1);
                            }
                            if (args && cJSON_IsString(args)) {
                                call->input = strdup(args->valuestring);
                                if (call->input) {
                                    call->input_len = strlen(call->input);
                                }
                            }
                        }
                        resp->call_count++;
                    }
                    if (resp->call_count > 0) {
                        resp->tool_use = true;
                    }
                }
            }
        }
    } else {
        /* stop_reason */
        cJSON *stop_reason = cJSON_GetObjectItem(root, "stop_reason");
        if (stop_reason && cJSON_IsString(stop_reason)) {
            resp->tool_use = (strcmp(stop_reason->valuestring, "tool_use") == 0);
        }

        /* Iterate content blocks */
        cJSON *content = cJSON_GetObjectItem(root, "content");
        if (content && cJSON_IsArray(content)) {
            /* Accumulate total text length first */
            size_t total_text = 0;
            cJSON *block;
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (btype && strcmp(btype->valuestring, "text") == 0) {
                    cJSON *text = cJSON_GetObjectItem(block, "text");
                    if (text && cJSON_IsString(text)) {
                        total_text += strlen(text->valuestring);
                    }
                }
            }

            /* Allocate and copy text */
            if (total_text > 0) {
                resp->text = calloc(1, total_text + 1);
                if (resp->text) {
                    cJSON_ArrayForEach(block, content) {
                        cJSON *btype = cJSON_GetObjectItem(block, "type");
                        if (!btype || strcmp(btype->valuestring, "text") != 0) continue;
                        cJSON *text = cJSON_GetObjectItem(block, "text");
                        if (!text || !cJSON_IsString(text)) continue;
                        size_t tlen = strlen(text->valuestring);
                        memcpy(resp->text + resp->text_len, text->valuestring, tlen);
                        resp->text_len += tlen;
                    }
                    resp->text[resp->text_len] = '\0';
                }
            }

            /* Extract tool_use blocks */
            cJSON_ArrayForEach(block, content) {
                cJSON *btype = cJSON_GetObjectItem(block, "type");
                if (!btype || strcmp(btype->valuestring, "tool_use") != 0) continue;
                if (resp->call_count >= MIMI_MAX_TOOL_CALLS) break;

                llm_tool_call_t *call = &resp->calls[resp->call_count];

                cJSON *id = cJSON_GetObjectItem(block, "id");
                if (id && cJSON_IsString(id)) {
                    strncpy(call->id, id->valuestring, sizeof(call->id) - 1);
                }

                cJSON *name = cJSON_GetObjectItem(block, "name");
                if (name && cJSON_IsString(name)) {
                    strncpy(call->name, name->valuestring, sizeof(call->name) - 1);
                }

                cJSON *input = cJSON_GetObjectItem(block, "input");
                if (input) {
                    char *input_str = cJSON_PrintUnformatted(input);
                    if (input_str) {
                        call->input = input_str;
                        call->input_len = strlen(input_str);
                    }
                }

                resp->call_count++;
            }
        }
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Response: %d bytes text, %d tool calls, stop=%s",
             (int)resp->text_len, resp->call_count,
             resp->tool_use ? "tool_use" : "end_turn");

    return ESP_OK;
}

/* ── NVS helpers ──────────────────────────────────────────────── */

esp_err_t llm_set_api_key(const char *api_key)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_API_KEY, api_key));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    safe_copy(s_api_key, sizeof(s_api_key), api_key);
    ESP_LOGI(TAG, "API key saved");
    return ESP_OK;
}

esp_err_t llm_set_model(const char *model)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_MODEL, model));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    safe_copy(s_model, sizeof(s_model), model);
    ESP_LOGI(TAG, "Model set to: %s", s_model);
    return ESP_OK;
}

esp_err_t llm_set_provider(const char *provider)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_PROVIDER, provider));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    safe_copy(s_provider, sizeof(s_provider), provider);
    ESP_LOGI(TAG, "Provider set to: %s", s_provider);
    return ESP_OK;
}

esp_err_t llm_set_api_url(const char *api_url)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_LLM, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_API_URL, api_url));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    safe_copy(s_api_url, sizeof(s_api_url), api_url);
    ESP_LOGI(TAG, "API URL set to: %s", s_api_url);
    return ESP_OK;
}