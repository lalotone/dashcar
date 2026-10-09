#include <assert.h>
#include "board/board.h"
#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "net/wifi_mgr.h"
#include "services/alerts.h"
#include "services/net_worker.h"
#include "services/ota.h"
#include "services/rain_now.h"
#include "services/traffic.h"
#include "services/ext_rtc.h"
#include "services/weather.h"
#include "settings.h"
#include "debug/selftest.h"
#include "ui/touch_input.h"
#include "ui/ui.h"

static const char *TAG = "dashcar";

static void *psram_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(size);
}

static void log_heap(const char *stage)
{
    ESP_LOGI(TAG, "%-8s internal free %u (largest %u, min ever %u), psram free %u", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void app_main(void)
{
    const esp_lv_adapter_rotation_t rotation = ESP_LV_ADAPTER_ROTATE_0;
    // Two frame buffers in LVGL direct mode: LVGL draws into the back buffer, the panel swaps at
    // vsync, then LVGL copies only the areas it changed into the other buffer. No tearing while a
    // screen is being drawn (single-buffer mode showed half-drawn screens), and a 1-s clock tick
    // copies a few KB. Don't use the double/triple "partial" modes: they copy the whole ~1.2 MB
    // frame PSRAM->PSRAM after EVERY refresh, which starved scan-out (drift/glitches).
    const esp_lv_adapter_tear_avoid_mode_t tear_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT;

    settings_init();
    log_heap("boot");

    // JSON trees (forecast grids) can be large and short-lived: keep them in PSRAM.
    cJSON_Hooks hooks = { .malloc_fn = psram_malloc, .free_fn = heap_caps_free };
    cJSON_InitHooks(&hooks);

    // Wi-Fi first: its RX buffers need internal RAM, which the display stack also wants.
    wifi_mgr_init();
    log_heap("wifi");

    board_handles_t board = { 0 };
    ESP_ERROR_CHECK(board_init(tear_mode, rotation, &board));

    // Clock from the RTC right away; network time is written back on every SNTP sync.
    ext_rtc_init(board_i2c_bus());
    wifi_mgr_set_time_sync_cb(ext_rtc_save_now);

    esp_lv_adapter_config_t adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
    adapter_cfg.task_stack_size = 16 * 1024;
    adapter_cfg.stack_in_psram = true;
    ESP_ERROR_CHECK(esp_lv_adapter_init(&adapter_cfg));

    esp_lv_adapter_display_config_t disp_cfg = ESP_LV_ADAPTER_DISPLAY_RGB_DEFAULT_CONFIG(
        board.panel, NULL, BOARD_LCD_H_RES, BOARD_LCD_V_RES, rotation);
    disp_cfg.tear_avoid_mode = tear_mode;
    disp_cfg.profile.use_psram = true;
    disp_cfg.profile.buffer_height = 12;  // partial draw buffer lives in internal RAM (24KB)
    lv_display_t *disp = esp_lv_adapter_register_display(&disp_cfg);
    assert(disp);

    ESP_ERROR_CHECK(esp_lv_adapter_start());
    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        touch_input_init(board.touch, disp);  // buffered: no lost touches while rendering
        esp_lv_adapter_unlock();
    }
    log_heap("display");

    net_worker_init();  // before the services: they register steps on it
    weather_init();
    alerts_init();
    traffic_init();
    rain_now_init();
    ui_fuel_init();     // background fuel prices for the home Fuel card
    ota_init(ui_ota_event);

    if (esp_lv_adapter_lock(-1) == ESP_OK) {
        ui_init(disp);
        ui_splash_open();  // joins the saved network, or opens Wi-Fi setup
        esp_lv_adapter_unlock();
    }
    ESP_ERROR_CHECK(board_backlight_set(true));
    log_heap("ui");
#if CONFIG_DASHCAR_SELFTEST
    selftest_start();  // tools/selftest.sh: scripted touch stress test, never in normal builds
#endif
    ota_mark_valid_later(20);  // confirm this image once it has run for a while
}
