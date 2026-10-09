#include "touch_sim.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui/touch_input.h"

// Samples go through the same queue as the real panel (ui/touch_input.c), at ~50 Hz.

static void step(int32_t x, int32_t y, bool pressed, uint32_t ms)
{
    touch_input_inject(x, y, pressed);
    vTaskDelay(pdMS_TO_TICKS(ms));
}

void dbg_touch_tap(int32_t x, int32_t y)
{
    step(x, y, true, 40);
    step(x, y, true, 40);
    step(x, y, true, 40);
    step(x, y, false, 200);
}

void dbg_touch_swipe(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t duration_ms)
{
    const int steps = duration_ms / 20 > 2 ? duration_ms / 20 : 2;
    for (int i = 0; i <= steps; i++) {
        step(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps, true, 20);
    }
    step(x1, y1, false, 300);
}
