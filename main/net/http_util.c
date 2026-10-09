#include "http_util.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "zlib.h"

static const char *TAG = "http";

#define HTTP_MAX_BODY  (128 * 1024)

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    size_t max;
    bool   overflow;
} body_t;

static esp_err_t on_event(esp_http_client_event_t *ev)
{
    body_t *body = ev->user_data;
    if (ev->event_id != HTTP_EVENT_ON_DATA || body->overflow) {
        return ESP_OK;
    }
    if (body->len + ev->data_len + 1 > body->cap) {
        size_t cap = body->cap ? body->cap * 2 : 8192;
        while (cap < body->len + ev->data_len + 1) {
            cap *= 2;
        }
        if (cap > body->max) {
            body->overflow = true;
            return ESP_OK;
        }
        char *nb = heap_caps_realloc(body->buf, cap, MALLOC_CAP_SPIRAM);
        if (!nb) {
            body->overflow = true;
            return ESP_OK;
        }
        body->buf = nb;
        body->cap = cap;
    }
    memcpy(body->buf + body->len, ev->data, ev->data_len);
    body->len += ev->data_len;
    body->buf[body->len] = '\0';
    return ESP_OK;
}

esp_err_t http_get(const char *url, char **out, size_t *out_len)
{
    body_t body = { .max = HTTP_MAX_BODY };
    const esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = on_event,
        .user_data = &body,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,  // long query strings (Open-Meteo) exceed the 512B default
        .user_agent = "dashcar/1.0 (ESP32-S3)",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && (status < 200 || status >= 300)) {
        ESP_LOGW(TAG, "%s -> HTTP %d", url, status);
        err = ESP_FAIL;
    }
    if (err == ESP_OK && (body.overflow || !body.buf)) {
        ESP_LOGW(TAG, "%s -> body %s", url, body.overflow ? "too large" : "empty");
        err = body.overflow ? ESP_ERR_NO_MEM : ESP_FAIL;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "GET %s failed: %s (internal free %u)", url, esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        free(body.buf);
        return err;
    }
    *out = body.buf;
    if (out_len) {
        *out_len = body.len;
    }
    return ESP_OK;
}

// ---- gzip ----------------------------------------------------------------

typedef struct {
    http_stream_cb_t on_data;
    void *ctx;
    z_stream zs;
    bool started, gzip, zinit;
    char *out;
} gz_ctx_t;

static voidpf z_alloc(voidpf opaque, uInt items, uInt size)
{
    return heap_caps_calloc(items, size, MALLOC_CAP_SPIRAM);
}

static void z_free(voidpf opaque, voidpf p)
{
    heap_caps_free(p);
}

static bool gz_on_data(const char *data, size_t len, void *arg)
{
    gz_ctx_t *g = arg;
    if (!g->started) {
        g->started = true;
        g->gzip = len >= 2 && (uint8_t)data[0] == 0x1f && (uint8_t)data[1] == 0x8b;
        if (g->gzip) {
            g->zs.zalloc = z_alloc;
            g->zs.zfree = z_free;
            if (inflateInit2(&g->zs, 16 + MAX_WBITS) != Z_OK) {
                return false;
            }
            g->zinit = true;
        }
    }
    if (!g->gzip) {
        return g->on_data(data, len, g->ctx);
    }
    g->zs.next_in = (Bytef *)data;
    g->zs.avail_in = len;
    while (g->zs.avail_in > 0) {
        g->zs.next_out = (Bytef *)g->out;
        g->zs.avail_out = 8192;
        int r = inflate(&g->zs, Z_NO_FLUSH);
        if (r != Z_OK && r != Z_STREAM_END) {
            ESP_LOGW(TAG, "inflate error %d", r);
            return false;
        }
        size_t n = 8192 - g->zs.avail_out;
        if (n && !g->on_data(g->out, n, g->ctx)) {
            return false;
        }
        if (r == Z_STREAM_END) {
            break;
        }
    }
    return true;
}

static esp_err_t stream_impl(const char *url, bool gzip, http_stream_cb_t on_data, void *ctx);

esp_err_t http_get_stream(const char *url, http_stream_cb_t on_data, void *ctx)
{
    return stream_impl(url, false, on_data, ctx);
}

esp_err_t http_get_stream_gzip(const char *url, http_stream_cb_t on_data, void *ctx)
{
    gz_ctx_t *g = heap_caps_calloc(1, sizeof(*g), MALLOC_CAP_SPIRAM);
    if (!g) {
        return ESP_ERR_NO_MEM;
    }
    g->on_data = on_data;
    g->ctx = ctx;
    g->out = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    esp_err_t err = g->out ? stream_impl(url, true, gz_on_data, g) : ESP_ERR_NO_MEM;
    if (g->zinit) {
        inflateEnd(&g->zs);
    }
    free(g->out);
    free(g);
    return err;
}

static esp_err_t stream_impl(const char *url, bool gzip, http_stream_cb_t on_data, void *ctx)
{
    const esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .user_agent = "dashcar/1.0 (ESP32-S3)",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }
    if (gzip) {
        esp_http_client_set_header(client, "Accept-Encoding", "gzip");
    }
    esp_err_t err = esp_http_client_open(client, 0);
    char *chunk = NULL;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            ESP_LOGW(TAG, "%s -> HTTP %d", url, status);
            err = ESP_FAIL;
        }
    }
    if (err == ESP_OK) {
        chunk = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
        err = chunk ? ESP_OK : ESP_ERR_NO_MEM;
    }
    while (err == ESP_OK) {
        int n = esp_http_client_read(client, chunk, 4096);
        if (n < 0) {
            err = ESP_FAIL;
        } else if (n == 0) {
            break;  // end of body
        } else if (!on_data(chunk, n, ctx)) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stream %s failed: %s", url, esp_err_to_name(err));
    }
    free(chunk);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static void host_of(const char *url, char *out, size_t len)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t n = strcspn(p, "/:?");
    if (n >= len) {
        n = len - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
}

void http_session_close(http_session_t *s)
{
    if (s->client) {
        esp_http_client_cleanup(s->client);
        s->client = NULL;
    }
    s->host[0] = '\0';
}

esp_err_t http_session_get(http_session_t *s, const char *url, size_t max_len,
                           char **out, size_t *out_len, int *status)
{
    char host[sizeof(s->host)];
    host_of(url, host, sizeof(host));

    for (int attempt = 0; attempt < 2; attempt++) {
        body_t body = { .max = max_len };
        if (s->client && strcmp(host, s->host) != 0) {
            http_session_close(s);
        }
        if (!s->client) {
            const esp_http_client_config_t cfg = {
                .url = url,
                .event_handler = on_event,
                .crt_bundle_attach = esp_crt_bundle_attach,
                .timeout_ms = 15000,
                .buffer_size = 4096,
                .buffer_size_tx = 3072,  // forecast-grid URLs are ~1.5 KB
                .user_agent = "dashcar/1.0 (ESP32-S3)",
            };
            s->client = esp_http_client_init(&cfg);
            if (!s->client) {
                return ESP_ERR_NO_MEM;
            }
            strlcpy(s->host, host, sizeof(s->host));
        } else {
            esp_http_client_set_url(s->client, url);
        }
        esp_http_client_set_user_data(s->client, &body);

        esp_err_t err = esp_http_client_perform(s->client);
        int code = esp_http_client_get_status_code(s->client);
        if (status) {
            *status = code;
        }
        if (err != ESP_OK) {
            // Server may have closed the kept-alive connection: retry once on a fresh one.
            free(body.buf);
            http_session_close(s);
            if (attempt == 0) {
                continue;
            }
            ESP_LOGW(TAG, "GET %s failed: %s", url, esp_err_to_name(err));
            return err;
        }
        if (code < 200 || code >= 300 || body.overflow || !body.buf) {
            ESP_LOGW(TAG, "GET %s -> HTTP %d%s", url, code, body.overflow ? " (too large)" : "");
            free(body.buf);
            return ESP_FAIL;
        }
        *out = body.buf;
        if (out_len) {
            *out_len = body.len;
        }
        return ESP_OK;
    }
    return ESP_FAIL;
}

size_t http_url_encode(const char *in, char *out, size_t out_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 <= out_size; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[o++] = *p;
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 0xF];
        }
    }
    out[o] = '\0';
    return o;
}
