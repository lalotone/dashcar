#pragma once

#include <stdbool.h>
#include "lvgl.h"

// Vector-ish weather icon for a WMO weather code, built from LVGL primitives
// so it scales to any size without image assets.
lv_obj_t *wx_icon_create(lv_obj_t *parent, int32_t size, int code, bool is_day);
