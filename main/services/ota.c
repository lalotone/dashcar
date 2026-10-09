#include "ota.h"

#include <string.h>
#include <sys/param.h>
#include "esp_heap_caps.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "mdns.h"
#include "net/wifi_mgr.h"
#include "services/net_worker.h"
#include "settings.h"

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

static esp_err_t on_get_root(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    char buf[512];
    snprintf(buf, sizeof(buf),
             "%s %s (built %s %s), running from %s\nPOST the app image to /update\n"
             "heap: internal free %u (largest block %u, min ever %u), psram free %u\n"
             "uptime %lld s, tasks %u\n",
             app->project_name, app->version, app->date, app->time, ota_running_partition(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (long long)(esp_log_timestamp() / 1000), (unsigned)uxTaskGetNumberOfTasks());
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, buf);
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
    xTaskCreate(restart_task, "ota_restart", 2048, NULL, 5, NULL);
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

static void mark_valid(TimerHandle_t t)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed after a healthy start (rollback cancelled)");
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
