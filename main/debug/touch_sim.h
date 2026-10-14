#pragma once

#include <stdint.h>

// Debug-only virtual finger: an extra LVGL pointer device driven by a script, so taps and
// swipes can be tested without touching the panel. Call from a normal task (not the LVGL task);
// both functions block until the gesture is done.
void dbg_touch_tap(int32_t x, int32_t y);
void dbg_touch_swipe(int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint32_t duration_ms);
