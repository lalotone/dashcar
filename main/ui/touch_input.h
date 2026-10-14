#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_lcd_touch.h"
#include "lvgl.h"

// Buffered touch input. A dedicated task polls the GT911 every 10 ms and queues samples;
// LVGL replays all of them (continue_reading) when it gets to read input. Without this,
// touches made while LVGL renders a heavy frame (300-800 ms for a new screen) are lost and
// swipes arrive as a single point, so they don't register as gestures.
//
// `tp` may be NULL (no touch controller): the LVGL device is still created so
// touch_input_inject() (debug finger) works.
lv_indev_t *touch_input_init(esp_lcd_touch_handle_t tp, lv_display_t *disp);

// Ignore touches that *start* in the next `ms` (until the finger lifts). Called on every screen
// change so the second tap of a hurried double tap doesn't hit whatever is under it on the
// new screen.
void touch_input_guard(uint32_t ms);

// Debug: feed a synthetic sample through the same queue as the real panel.
void touch_input_inject(int32_t x, int32_t y, bool pressed);
