#include "screenshot.h"

#include <stdio.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui/ui.h"

void dbg_screenshot(const char *tag, lv_obj_t *obj, const lv_area_t *area, int step)
{
    if (!ui_lock()) {
        return;
    }
    // No lv_refr_now() here: called from another task while a full redraw is pending it can
    // deadlock with the display adapter. lv_snapshot_take renders into its own buffer anyway.
    lv_draw_buf_t *snap = lv_snapshot_take(obj ? obj : lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    ui_unlock();
    if (!snap) {
        printf("DUMPFAIL %s\n", tag);
        return;
    }
    step = step < 1 ? 1 : step;
    int x0 = 0, y0 = 0, x1 = snap->header.w - 1, y1 = snap->header.h - 1;
    if (area) {
        x0 = LV_MAX(area->x1, 0);
        y0 = LV_MAX(area->y1, 0);
        x1 = LV_MIN(area->x2, x1);
        y1 = LV_MIN(area->y2, y1);
    }
    const int w = (x1 - x0) / step + 1, h = (y1 - y0) / step + 1;
    char *line = heap_caps_malloc(w * 4 + 2, MALLOC_CAP_SPIRAM);
    if (line) {
        printf("\nDUMP %s %d %d %d\n", tag, w, h, step);
        for (int y = y0; y <= y1; y += step) {
            const uint16_t *row = (const uint16_t *)(snap->data + (size_t)y * snap->header.stride);
            int o = 0;
            for (int x = x0; x <= x1; x += step) {
                o += sprintf(line + o, "%04x", row[x]);
            }
            puts(line);
            vTaskDelay(1);  // let the idle task feed the watchdog
        }
        printf("ENDDUMP\n");
        heap_caps_free(line);
    }
    lv_draw_buf_destroy(snap);
}
