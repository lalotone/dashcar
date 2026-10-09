#include "touch_input.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "touch";

#define POLL_MS     10
#define QUEUE_LEN   128   // > 1 s of movement at 100 Hz

typedef struct {
    int16_t x, y;
    bool pressed;
    uint32_t t_ms;   // when it was captured (rendering can delay reading by ~0.5 s)
} sample_t;

static QueueHandle_t s_queue;
static esp_lcd_touch_handle_t s_tp;
static sample_t s_last;          // what LVGL saw last (LVGL task only)
static volatile bool s_injecting; // debug finger active: pause the real panel
static volatile uint32_t s_guard_until;  // ms: presses captured before this are ignored
static bool s_swallowing;         // inside an ignored press (until release)

static uint32_t now_ms(void)
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void push(const sample_t *in)
{
    sample_t sv = *in;
    sv.t_ms = now_ms();
    const sample_t *s = &sv;
    if (xQueueSend(s_queue, s, 0) != pdTRUE) {
        // Full (LVGL stalled > 1 s): drop the oldest so the latest state wins.
        sample_t drop;
        xQueueReceive(s_queue, &drop, 0);
        xQueueSend(s_queue, s, 0);
    }
}

static void poll_task(void *arg)
{
    bool was_pressed = false;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        if (s_injecting) {
            continue;
        }
        esp_lcd_touch_point_data_t p[1];
        uint8_t n = 0;
        if (esp_lcd_touch_read_data(s_tp) != ESP_OK ||
                esp_lcd_touch_get_data(s_tp, p, &n, 1) != ESP_OK) {
            continue;
        }
        if (n > 0) {
            sample_t s = { .x = p[0].x, .y = p[0].y, .pressed = true };
            push(&s);
            was_pressed = true;
        } else if (was_pressed) {
            sample_t s = { .x = s_last.x, .y = s_last.y, .pressed = false };
            push(&s);  // one release event; nothing queued while idle
            was_pressed = false;
        }
    }
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    sample_t s;
    while (xQueueReceive(s_queue, &s, 0) == pdTRUE) {
        // Tap guard: drop a press that begins inside the guard window, through its release.
        const bool new_press = s.pressed && !s_last.pressed && !s_swallowing;
        if (new_press && (int32_t)(s_guard_until - s.t_ms) > 0) {
            s_swallowing = true;
        }
        if (s_swallowing) {
            if (!s.pressed) {
                s_swallowing = false;
            }
            continue;
        }
        if (!s.pressed) {
            s.x = s_last.x;  // release where the finger last was
            s.y = s_last.y;
        }
        s_last = s;
        data->continue_reading = uxQueueMessagesWaiting(s_queue) > 0;
        break;
    }
    data->point.x = s_last.x;
    data->point.y = s_last.y;
    data->state = s_last.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

lv_indev_t *touch_input_init(esp_lcd_touch_handle_t tp, lv_display_t *disp)
{
    s_tp = tp;
    s_queue = xQueueCreate(QUEUE_LEN, sizeof(sample_t));
    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, read_cb);
    lv_indev_set_display(indev, disp);
    if (tp) {
        xTaskCreatePinnedToCore(poll_task, "touch", 3072, NULL, 5, NULL, 1);
    } else {
        ESP_LOGW(TAG, "no touch controller");
    }
    return indev;
}

void touch_input_inject(int32_t x, int32_t y, bool pressed)
{
    s_injecting = true;
    sample_t s = { .x = (int16_t)x, .y = (int16_t)y, .pressed = pressed };
    push(&s);
    if (!pressed) {
        s_injecting = false;
    }
}

void touch_input_guard(uint32_t ms)
{
    s_guard_until = now_ms() + ms;
}
