#include "ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "crashlog.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "mdns.h"
#include "net/wifi_mgr.h"
#include "services/gps.h"
#include "services/net_worker.h"
#include "settings.h"
#include "ui/ui.h"

static const char *TAG = "ota";

#define CHUNK 4096

static httpd_handle_t s_server;
static ota_cb_t s_cb;
static bool s_mdns_ready;

static void report(ota_event_t ev, int pct, const char *msg)
{
    if (s_cb) {
        s_cb(ev, pct, msg);
    }
}

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (supply voltage dropped)";
    case ESP_RST_PANIC:     return "crash (panic)";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_SW:        return "software (update / restart)";
    case ESP_RST_USB:       return "USB (flashing / serial port)";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    default:                return "other";
    }
}

static esp_err_t on_get_root(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char crash[300];
    crashlog_text(crash, sizeof(crash));
    char gps_text[64];
    gps_status_text(gps_text, sizeof(gps_text));
    gps_stats_t gs;
    gps_get_stats(&gs);
    char buf[1000];
    snprintf(buf, sizeof(buf),
             "%s %s (built %s %s), running from %s\nPOST the app image to /update\n"
             "heap: internal free %u (largest block %u, min ever %u), psram free %u (largest block %u)\n"
             "uptime %lld s, tasks %u, last reset: %s, brownouts since flashing: %lu\n"
             "gps: %s, %u bytes, %u sentences, %u bad\n"
             "last crash: %s\n",
             app->project_name, app->version, app->date, app->time, ota_running_partition(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (long long)(esp_log_timestamp() / 1000), (unsigned)uxTaskGetNumberOfTasks(),
             reset_name(esp_reset_reason()), (unsigned long)settings_get_brownouts(),
             gps_text, (unsigned)gs.bytes, (unsigned)gs.sentences, (unsigned)gs.bad, crash);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, buf);
}

// GET /diag: what the board is busy with right now, measured over 3 s without disturbing it
// (display glitches = something hogging PSRAM: shows as redraw volume or a task's CPU use).
static int cmp_task(const void *a, const void *b)
{
    const uintptr_t x = (uintptr_t)((const TaskStatus_t *)a)->xHandle, y = (uintptr_t)((const TaskStatus_t *)b)->xHandle;
    return (x > y) - (x < y);
}

static esp_err_t on_get_diag(httpd_req_t *req)
{
    enum { MAX_T = 32, WINDOW_MS = 3000 };
    TaskStatus_t *a = heap_caps_calloc(2 * MAX_T, sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM);
    char *buf = heap_caps_malloc(3072, MALLOC_CAP_SPIRAM);
    if (!a || !buf) {
        heap_caps_free(a);
        heap_caps_free(buf);
        return httpd_resp_send_500(req);
    }
    TaskStatus_t *b = a + MAX_T;
    uint32_t t0, t1, refr, px;
    ui_diag_take(&refr, &px);
    const UBaseType_t na = uxTaskGetSystemState(a, MAX_T, &t0);
    vTaskDelay(pdMS_TO_TICKS(WINDOW_MS));
    const UBaseType_t nb = uxTaskGetSystemState(b, MAX_T, &t1);
    ui_diag_take(&refr, &px);
    qsort(a, na, sizeof(*a), cmp_task);
    const uint32_t span = t1 - t0;  // run-time clock units (both cores share it: 200 % = both busy)
    int len = snprintf(buf, 3072,
                       "window %d ms: %lu refreshes (%.1f/s), %lu kpixels redrawn (%.0f kpx/s; full screen = 614)\n"
                       "home %d, psram free %u (largest %u), internal free %u (min ever %u)\n\nCPU (%% of one core):\n",
                       WINDOW_MS, (unsigned long)refr, refr * 1000.0f / WINDOW_MS, (unsigned long)(px / 1000),
                       px / 1000 * 1000.0f / WINDOW_MS, ui_is_home(),
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                       (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    for (UBaseType_t i = 0; i < nb && len < 2900; i++) {
        TaskStatus_t key = { .xHandle = b[i].xHandle };
        const TaskStatus_t *p = bsearch(&key, a, na, sizeof(*a), cmp_task);
        const uint32_t used = b[i].ulRunTimeCounter - (p ? p->ulRunTimeCounter : 0);
        const float pct = span ? used * 100.0f / span : 0;
        if (pct >= 0.5f) {
            len += snprintf(buf + len, 3072 - len, "  %-16s %5.1f %%  (stack free %lu)\n", b[i].pcTaskName, pct,
                            (unsigned long)b[i].usStackHighWaterMark);
        }
    }
    httpd_resp_set_type(req, "text/plain");
    esp_err_t err = httpd_resp_sendstr(req, buf);
    heap_caps_free(a);
    heap_caps_free(buf);
    return err;
}

static esp_err_t fail(httpd_req_t *req, esp_ota_handle_t h, const char *status, const char *msg)
{
    if (h) {
        esp_ota_abort(h);
    }
    ESP_LOGW(TAG, "update failed: %s", msg);
    report(OTA_EVT_FAILED, 0, msg);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, msg);
    return ESP_OK;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static esp_err_t on_post_update(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return fail(req, 0, "500 Internal Server Error", "no OTA partition (flash once over USB with the OTA layout)\n");
    }
    if (req->content_len <= 0 || req->content_len > part->size) {
        return fail(req, 0, "400 Bad Request", "image size missing or larger than the OTA partition\n");
    }
    char *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        return fail(req, 0, "500 Internal Server Error", "out of memory\n");
    }
    ESP_LOGI(TAG, "receiving %d bytes into %s", req->content_len, part->label);
    report(OTA_EVT_START, 0, NULL);

    esp_ota_handle_t h = 0;
    int received = 0, last_pct = -1;
    bool checked = false;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, buf, MIN(CHUNK, req->content_len - received));
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            free(buf);
            return fail(req, h, "400 Bad Request", "connection lost during upload\n");
        }
        if (!checked) {
            // The first chunk holds the image header + app description: refuse anything
            // that isn't a DashCar build before touching flash.
            const size_t desc_off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
            const esp_app_desc_t *desc = (const esp_app_desc_t *)(buf + desc_off);
            if (n < (int)(desc_off + sizeof(esp_app_desc_t)) || desc->magic_word != ESP_APP_DESC_MAGIC_WORD ||
                    strcmp(desc->project_name, esp_app_get_description()->project_name) != 0) {
                free(buf);
                return fail(req, 0, "400 Bad Request", "not a DashCar app image (use build/dashcar.bin)\n");
            }
            ESP_LOGI(TAG, "incoming version %s", desc->version);
            if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
                free(buf);
                return fail(req, 0, "500 Internal Server Error", "esp_ota_begin failed\n");
            }
            checked = true;
        }
        if (esp_ota_write(h, buf, n) != ESP_OK) {
            free(buf);
            return fail(req, h, "500 Internal Server Error", "flash write failed\n");
        }
        received += n;
        int pct = (int)((int64_t)received * 100 / req->content_len);
        if (pct != last_pct) {
            last_pct = pct;
            report(OTA_EVT_PROGRESS, pct, NULL);
        }
    }
    free(buf);
    esp_err_t err = esp_ota_end(h);  // verifies the image (checksum / SHA-256)
    if (err != ESP_OK) {
        return fail(req, 0, "400 Bad Request",
                    err == ESP_ERR_OTA_VALIDATE_FAILED ? "image is corrupt\n" : "esp_ota_end failed\n");
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        return fail(req, 0, "500 Internal Server Error", "could not select the new image\n");
    }
    ESP_LOGI(TAG, "update written to %s, restarting", part->label);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK: update installed, restarting\n");
    report(OTA_EVT_DONE, 100, NULL);
    // 4 KB: esp_restart() runs the shutdown handlers (Wi-Fi stop...) on this stack; 2 KB overflowed and the
    // board "crashed" into the new image (reset reason panic). Internal stack: the cache may be disabled.
    xTaskCreate(restart_task, "ota_restart", 4096, NULL, 5, NULL);
    return ESP_OK;
}

static void server_start(void)
{
    if (s_server) {
        return;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.max_open_sockets = 3;
    cfg.recv_wait_timeout = 10;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "HTTP server failed to start");
        s_server = NULL;
        return;
    }
    const httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = on_get_root };
    const httpd_uri_t update = { .uri = "/update", .method = HTTP_POST, .handler = on_post_update };
    const httpd_uri_t diag = { .uri = "/diag", .method = HTTP_GET, .handler = on_get_diag };
    httpd_register_uri_handler(s_server, &diag);
    httpd_register_uri_handler(s_server, &root);
    httpd_register_uri_handler(s_server, &update);

    if (!s_mdns_ready && mdns_init() == ESP_OK) {
        mdns_hostname_set("dashcar");
        mdns_instance_name_set("DashCar");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        s_mdns_ready = true;
    }
    ESP_LOGI(TAG, "update server on http://dashcar.local/ (port 80)");
}

static void server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
        ESP_LOGI(TAG, "update server stopped");
    }
}

void ota_apply_setting(void)
{
    if (settings_get_ota_enabled() && wifi_mgr_state() == WIFI_MGR_CONNECTED) {
        server_start();
    } else {
        server_stop();
    }
}

static void apply_job(void *arg)
{
    ota_apply_setting();
}

static void on_wifi(wifi_mgr_state_t st, const char *reason, void *ctx)
{
    // Runs in the system event task, whose stack is too small for httpd/mDNS start-up.
    if (st == WIFI_MGR_CONNECTED) {
        net_worker_post(apply_job, NULL);
    }
}

void ota_init(ota_cb_t cb)
{
    s_cb = cb;
    wifi_mgr_add_listener(on_wifi, NULL);
    ota_apply_setting();
}

bool ota_server_running(void)
{
    return s_server != NULL;
}

const char *ota_running_partition(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "?";
}

static void mark_valid_job(void *arg)
{
    (void)arg;
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed after a healthy start (rollback cancelled)");
    }
}

// Runs in FreeRTOS's timer task, whose 2 KB stack overflowed doing the flash write + log itself (crash
// 20 s after every Wi-Fi update): hand the work to the network worker.
static void mark_valid(TimerHandle_t t)
{
    if (!net_worker_post(mark_valid_job, NULL)) {
        xTimerChangePeriod(t, pdMS_TO_TICKS(5000), 0);  // queue full: try again shortly
        return;
    }
    xTimerDelete(t, 0);
}

void ota_mark_valid_later(int seconds)
{
    TimerHandle_t t = xTimerCreate("ota_ok", pdMS_TO_TICKS(seconds * 1000), pdFALSE, NULL, mark_valid);
    if (t) {
        xTimerStart(t, 0);
    }
}
