// Scripted touch stress test (CONFIG_DASHCAR_SELFTEST, see tools/selftest.sh).
// Drives the real UI through the buffered touch path with a virtual finger: navigation via
// cards, swipe-back, the big back button and the status bar; radar cancelled mid-load; double
// taps; fuel-chip storms; city search. Logs heap per cycle (leak check) and ends with
// "selftest: RESULT: N failure(s)".
#include "selftest.h"

#include <stdio.h>
#include "debug/touch_sim.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui/ui.h"

#define AT(...) ESP_LOGW("selftest", __VA_ARGS__)
// Home card centres (ui_home.c 3x2 grid: x 496..1000, cards 160x250, gap 12)
#define CARD_WEATHER 576, 189
#define CARD_RADAR   748, 189
#define CARD_TRAFFIC 920, 189
#define CARD_FUEL    576, 451
#define CARD_SETTING 748, 451
#define CARD_PARKING 920, 451
static int s_fail;
static void expect_home(bool want, const char *what)
{
    if (ui_is_home() != want) {
        s_fail++;
        AT("FAIL: %s (home=%d, expected %d)", what, ui_is_home(), want);
    }
}
static void back_swipe(void)
{
    dbg_touch_swipe(150, 300, 750, 310, 300);
    vTaskDelay(pdMS_TO_TICKS(900));
}
static void heap(const char *tag)
{
    AT("%s: internal %u, psram %u", tag, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
       (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
static void selftest_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(12000));  // boot, Wi-Fi, first forecast
    expect_home(true, "boot ends on home");
    heap("start");
    for (int cycle = 1; cycle <= 10; cycle++) {
        dbg_touch_tap(CARD_WEATHER);  vTaskDelay(pdMS_TO_TICKS(1200));
        expect_home(false, "weather card opens weather");
        back_swipe();                 expect_home(true, "swipe back from weather");
        dbg_touch_tap(CARD_TRAFFIC);  vTaskDelay(pdMS_TO_TICKS(1500));
        expect_home(false, "traffic card opens traffic");
        back_swipe();                 expect_home(true, "swipe back from traffic");
        dbg_touch_tap(CARD_FUEL);     vTaskDelay(pdMS_TO_TICKS(1500));
        expect_home(false, "fuel card opens fuel");
        back_swipe();                 expect_home(true, "swipe back from fuel");
        dbg_touch_tap(CARD_PARKING);  vTaskDelay(pdMS_TO_TICKS(1000));
        expect_home(false, "parked card opens where-I-parked");
        back_swipe();                 expect_home(true, "swipe back from where-I-parked");
        dbg_touch_tap(CARD_SETTING);  vTaskDelay(pdMS_TO_TICKS(1000));
        expect_home(false, "settings card opens settings");
        dbg_touch_tap(900, 32);       vTaskDelay(pdMS_TO_TICKS(900));   // status bar = home
        expect_home(true, "status bar goes home from settings");
        dbg_touch_tap(CARD_RADAR);    vTaskDelay(pdMS_TO_TICKS(1500 + cycle * 300));  // leave mid-load
        expect_home(false, "radar card opens radar");
        back_swipe();                 // radar -> weather
        expect_home(false, "radar back goes to weather");
        dbg_touch_tap(200, 32);       vTaskDelay(pdMS_TO_TICKS(900));   // big back button: weather -> home
        expect_home(true, "big back button from weather");
        char tag[24];
        snprintf(tag, sizeof(tag), "cycle %d", cycle);
        heap(tag);
    }
    AT("double taps on every card");
    const int cards[5][2] = { { CARD_WEATHER }, { CARD_FUEL }, { CARD_SETTING }, { CARD_RADAR }, { CARD_TRAFFIC } };
    static const char *const names[] = { "weather", "fuel", "settings", "radar", "traffic" };
    for (int i = 0; i < 5; i++) {
        char what[64];
        dbg_touch_tap(cards[i][0], cards[i][1]);
        dbg_touch_tap(cards[i][0], cards[i][1]);
        vTaskDelay(pdMS_TO_TICKS(1500));
        snprintf(what, sizeof(what), "double tap opens %s", names[i]);
        expect_home(false, what);
        dbg_touch_tap(900, 32);
        vTaskDelay(pdMS_TO_TICKS(1000));
        snprintf(what, sizeof(what), "status bar -> home after double tap on %s", names[i]);
        expect_home(true, what);
    }
    AT("fuel: hammer the fuel-type chips while fetches are in flight");
    dbg_touch_tap(CARD_FUEL);
    vTaskDelay(pdMS_TO_TICKS(800));
    const int chips[4][2] = { { 55, 94 }, { 187, 94 }, { 338, 94 }, { 470, 94 } };
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 4; i++) {
            dbg_touch_tap(chips[i][0], chips[i][1]);
        }
    }
    vTaskDelay(pdMS_TO_TICKS(6000));
    back_swipe();
    expect_home(true, "back from fuel after chip storm");
    AT("traffic: filter and radius chips, then open an incident");
    dbg_touch_tap(CARD_TRAFFIC);
    vTaskDelay(pdMS_TO_TICKS(1500));
    dbg_touch_tap(160, 94);    // "All (incl. roadworks)"
    vTaskDelay(pdMS_TO_TICKS(700));
    dbg_touch_tap(960, 94);    // 100 km
    vTaskDelay(pdMS_TO_TICKS(900));
    dbg_touch_tap(700, 200);   // first incident row -> detail popup
    vTaskDelay(pdMS_TO_TICKS(1200));
    dbg_touch_tap(512, 40);    // tap to close the popup
    vTaskDelay(pdMS_TO_TICKS(700));
    dbg_touch_tap(50, 94);     // back to "Major"
    vTaskDelay(pdMS_TO_TICKS(700));
    back_swipe();
    expect_home(true, "back from traffic after chips + popup");
    AT("weather -> location search -> back -> back");
    dbg_touch_tap(CARD_WEATHER);
    vTaskDelay(pdMS_TO_TICKS(1200));
    if (ui_lock()) {
        ui_city_search_open();
        ui_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    dbg_touch_swipe(150, 200, 750, 210, 300);   // above the keyboard
    vTaskDelay(pdMS_TO_TICKS(900));
    expect_home(false, "city search back goes to weather");
    back_swipe();
    expect_home(true, "weather back goes home");
    heap("end");
    AT("RESULT: %d failure(s)", s_fail);
    vTaskDelay(portMAX_DELAY);  // PSRAM-stack task: park instead of self-deleting
}

void selftest_start(void)
{
    // PSRAM stack: the test needs 12 KB and must not compete for internal RAM.
    xTaskCreatePinnedToCoreWithCaps(selftest_task, "selftest", 12288, NULL, 2, NULL, tskNO_AFFINITY, MALLOC_CAP_SPIRAM);
}
