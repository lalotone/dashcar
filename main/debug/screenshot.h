#pragma once

#include "lvgl.h"

// Debug-only: dumps a screenshot over the console so it can be checked without seeing the
// panel. Capture and convert with `tools/devcap.py`. Format:
//   DUMP <tag> <w> <h> <step>\n  <h lines of w RGB565 pixels as 4-hex-digit words>  ENDDUMP\n
//
// obj:  what to snapshot (NULL = active screen; lv_layer_top() for popups/status bar)
// area: region in obj coordinates (NULL = whole object)
// step: 1 = full resolution, 2 = half (4x less data; ~10 s for a full screen)
//
// Takes the LVGL lock for the snapshot only; call from a normal task, not from LVGL callbacks.
void dbg_screenshot(const char *tag, lv_obj_t *obj, const lv_area_t *area, int step);
