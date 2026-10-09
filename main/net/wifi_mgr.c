#include "wifi_mgr.h"

#include <stdlib.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

static const char *TAG = "wifi";

#define MAX_LISTENERS        6
#define MAX_SCAN_RESULTS     24
#define ATTEMPT_RETRIES      4      // before reporting FAILED on a fresh attempt
#define RECONNECT_DELAY_MS   5000   // after an established link drops

typedef struct {
    wifi_mgr_state_cb_t cb;
    void *ctx;
} listener_t;

static listener_t s_listeners[MAX_LISTENERS];
static wifi_mgr_state_t s_state = WIFI_MGR_IDLE;
static char s_ssid[33];
static bool s_want_connected;   // keep trying after a drop
static bool s_was_connected;    // this attempt has succeeded at least once
static bool s_persistent;       // keep retrying after the initial attempt fails
static int s_retries;
static bool s_sntp_started;
static TimerHandle_t s_reconnect_timer;
static void (*s_time_sync_cb)(void);

static wifi_mgr_scan_cb_t s_scan_cb;
static void *s_scan_ctx;

static void set_state(wifi_mgr_state_t st, const char *reason)
{
    s_state = st;
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (s_listeners[i].cb) {
            s_listeners[i].cb(st, reason, s_listeners[i].ctx);
        }
    }
}

static const char *reason_text(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return "Wrong password";
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "Network not found";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
        return "Access point rejected the connection";
    default:
        return "Connection failed";
    }
}

static void reconnect_timer_cb(TimerHandle_t t)
{
    if (s_want_connected && s_state != WIFI_MGR_CONNECTED) {
        esp_wifi_connect();
    }
}

static void on_time_sync(struct timeval *tv)
{
    ESP_LOGI(TAG, "time synchronised");
    if (s_time_sync_cb) {
        s_time_sync_cb();
    }
}

void wifi_mgr_set_time_sync_cb(void (*cb)(void))
{
    s_time_sync_cb = cb;
}

static void start_sntp_once(void)
{
    if (s_sntp_started) {
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST("pool.ntp.org", "time.google.com"));
    cfg.sync_cb = on_time_sync;
    if (esp_netif_sntp_init(&cfg) == ESP_OK) {
        s_sntp_started = true;
    }
}

static int ap_cmp(const void *a, const void *b)
{
    return ((const wifi_mgr_ap_t *)b)->rssi - ((const wifi_mgr_ap_t *)a)->rssi;
}

static void deliver_scan(void)
{
    uint16_t n = MAX_SCAN_RESULTS;
    wifi_ap_record_t *recs = calloc(n, sizeof(*recs));
    wifi_mgr_ap_t *aps = calloc(n, sizeof(*aps));
    int count = 0;
    if (recs && aps && esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
        for (int i = 0; i < n; i++) {
            const char *ssid = (const char *)recs[i].ssid;
            if (ssid[0] == '\0') {
                continue;  // hidden network
            }
            bool dup = false;
            for (int j = 0; j < count; j++) {
                if (strcmp(aps[j].ssid, ssid) == 0) {
                    dup = true;
                    if (recs[i].rssi > aps[j].rssi) {
                        aps[j].rssi = recs[i].rssi;
                    }
                    break;
                }
            }
            if (!dup) {
                strlcpy(aps[count].ssid, ssid, sizeof(aps[count].ssid));
                aps[count].rssi = recs[i].rssi;
                aps[count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                count++;
            }
        }
        qsort(aps, count, sizeof(*aps), ap_cmp);
    } else {
        esp_wifi_clear_ap_list();
    }

    wifi_mgr_scan_cb_t cb = s_scan_cb;
    s_scan_cb = NULL;
    if (cb) {
        cb(aps, count, s_scan_ctx);
    }
    free(recs);
    free(aps);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *ev = data;
        ESP_LOGW(TAG, "disconnected from '%s', reason %d", s_ssid, ev->reason);
        if (ev->reason == WIFI_REASON_ASSOC_LEAVE) {
            // Our own esp_wifi_disconnect(); a new attempt may already be underway.
            if (!s_want_connected) {
                set_state(WIFI_MGR_IDLE, NULL);
            }
        } else if (!s_want_connected) {
            set_state(WIFI_MGR_IDLE, NULL);
        } else if (s_was_connected) {
            // Established link dropped (e.g. driving away): keep trying quietly.
            set_state(WIFI_MGR_CONNECTING, "Reconnecting...");
            xTimerStart(s_reconnect_timer, 0);
        } else if (++s_retries <= ATTEMPT_RETRIES) {
            esp_wifi_connect();
        } else if (s_persistent) {
            if (s_retries == ATTEMPT_RETRIES + 1) {
                set_state(WIFI_MGR_FAILED, reason_text(ev->reason));
            }
            if (s_persistent && s_want_connected) {  // a listener may have given up
                set_state(WIFI_MGR_CONNECTING, "Retrying...");
                xTimerStart(s_reconnect_timer, 0);
            }
        } else {
            s_want_connected = false;
            set_state(WIFI_MGR_FAILED, reason_text(ev->reason));
        }
        break;
    }
    case WIFI_EVENT_SCAN_DONE:
        deliver_scan();
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "connected to '%s', ip " IPSTR, s_ssid, IP2STR(&ev->ip_info.ip));
        s_was_connected = true;
        s_retries = 0;
        start_sntp_once();
        set_state(WIFI_MGR_CONNECTED, NULL);
    }
}

void wifi_mgr_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));  // credentials live in settings.c
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_NONE);

    s_reconnect_timer = xTimerCreate("wifi_rc", pdMS_TO_TICKS(RECONNECT_DELAY_MS), pdFALSE, NULL, reconnect_timer_cb);
}

void wifi_mgr_add_listener(wifi_mgr_state_cb_t cb, void *ctx)
{
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].ctx == ctx) {
            return;  // already registered (screen reopened before the old one was deleted)
        }
    }
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (!s_listeners[i].cb) {
            s_listeners[i] = (listener_t){ cb, ctx };
            return;
        }
    }
    ESP_LOGE(TAG, "too many listeners");
}

void wifi_mgr_remove_listener(wifi_mgr_state_cb_t cb, void *ctx)
{
    for (int i = 0; i < MAX_LISTENERS; i++) {
        if (s_listeners[i].cb == cb && s_listeners[i].ctx == ctx) {
            s_listeners[i] = (listener_t){ 0 };
        }
    }
}

void wifi_mgr_connect(const char *ssid, const char *pass, bool persistent)
{
    wifi_config_t cfg = { 0 };
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    cfg.sta.pmf_cfg.capable = true;

    xTimerStop(s_reconnect_timer, 0);
    s_want_connected = false;  // the disconnect below must not trigger a retry
    esp_wifi_disconnect();

    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    s_want_connected = true;
    s_was_connected = false;
    s_persistent = persistent;
    s_retries = 0;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    set_state(WIFI_MGR_CONNECTING, NULL);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect: %s", esp_err_to_name(err));
        s_want_connected = false;
        set_state(WIFI_MGR_FAILED, "Wi-Fi driver busy, try again");
    }
}

void wifi_mgr_disconnect(void)
{
    xTimerStop(s_reconnect_timer, 0);
    s_want_connected = false;
    esp_wifi_disconnect();
    set_state(WIFI_MGR_IDLE, NULL);
}

bool wifi_mgr_scan(wifi_mgr_scan_cb_t cb, void *ctx)
{
    if (s_state == WIFI_MGR_CONNECTING) {
        xTimerStop(s_reconnect_timer, 0);
        s_want_connected = false;
        esp_wifi_disconnect();
        set_state(WIFI_MGR_IDLE, NULL);
    }
    s_scan_cb = cb;
    s_scan_ctx = ctx;
    const wifi_scan_config_t scan_cfg = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan start: %s", esp_err_to_name(err));
        s_scan_cb = NULL;
        return false;
    }
    return true;
}

wifi_mgr_state_t wifi_mgr_state(void)
{
    return s_state;
}

const char *wifi_mgr_ssid(void)
{
    return s_ssid;
}

int8_t wifi_mgr_rssi(void)
{
    wifi_ap_record_t ap;
    if (s_state == WIFI_MGR_CONNECTED && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return -127;
}
