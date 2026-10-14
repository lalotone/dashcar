#pragma once

#include "esp_lcd_types.h"

// tools/soak.sh only (CONFIG_DASHCAR_SOAK): ~35 min realistic workload, see soak.c.
void soak_start(esp_lcd_panel_handle_t panel);
