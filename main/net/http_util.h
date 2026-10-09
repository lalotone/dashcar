#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

// GET `url` (http or https, using the IDF CA bundle) into a freshly allocated,
// NUL-terminated buffer. Caller frees *out. Fails on non-2xx status.
esp_err_t http_get(const char *url, char **out, size_t *out_len);

// Reuses one connection (TLS session) for consecutive requests to the same host,
// e.g. downloading dozens of map tiles. Not thread-safe; one per task.
typedef struct {
    void *client;      // esp_http_client_handle_t
    char  host[64];
} http_session_t;

// Like http_get(), but binary-safe and keeps the connection open.
// `max_len` caps the body size. *status gets the HTTP status (may be NULL).
esp_err_t http_session_get(http_session_t *s, const char *url, size_t max_len,
                           char **out, size_t *out_len, int *status);
void http_session_close(http_session_t *s);

// Streams a GET body through `on_data` in chunks (for large documents parsed on the fly).
// Returning false from on_data aborts. Fails on non-2xx status.
typedef bool (*http_stream_cb_t)(const char *data, size_t len, void *ctx);
esp_err_t http_get_stream(const char *url, http_stream_cb_t on_data, void *ctx);
// Same, but asks for gzip and inflates on the fly (falls back to plain if the server
// doesn't compress). XML/JSON feeds shrink ~95% (DGT traffic: 5.4 MB -> 174 KB).
esp_err_t http_get_stream_gzip(const char *url, http_stream_cb_t on_data, void *ctx);

// Percent-encodes `in` for use in a query string. Returns bytes written (excl. NUL).
size_t http_url_encode(const char *in, char *out, size_t out_size);
