#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_lv_adapter.h"
#include "driver/i2c_master.h"

// Waveshare ESP32-S3-Touch-LCD-5B (1024x600, ST7262 RGB panel, GT911 touch, CH422G IO expander)
#define BOARD_LCD_H_RES 1024
#define BOARD_LCD_V_RES 600

typedef struct {
    esp_lcd_panel_handle_t panel;
    esp_lcd_touch_handle_t touch;
} board_handles_t;

esp_err_t board_init(esp_lv_adapter_tear_avoid_mode_t tear_mode,
                     esp_lv_adapter_rotation_t rotation,
                     board_handles_t *out);

esp_err_t board_backlight_set(bool on);

// Shared I2C bus (CH422G, GT911, PCF85063A RTC). Valid after board_init().
i2c_master_bus_handle_t board_i2c_bus(void);
