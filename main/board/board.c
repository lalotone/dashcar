#include "board.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "board";

// Timing and pins from waveshareteam/ESP32-S3-Touch-LCD-5 (EXAMPLE_USE_1024_600_LCD)
#define LCD_PCLK_HZ        (21 * 1000 * 1000)
#define LCD_BOUNCE_LINES   10

#define PIN_I2C_SDA        GPIO_NUM_8
#define PIN_I2C_SCL        GPIO_NUM_9
#define PIN_TOUCH_INT      GPIO_NUM_4

#define PIN_LCD_VSYNC      GPIO_NUM_3
#define PIN_LCD_HSYNC      GPIO_NUM_46
#define PIN_LCD_DE         GPIO_NUM_5
#define PIN_LCD_PCLK       GPIO_NUM_7

// CH422G uses fixed I2C addresses per register instead of a device address + register.
#define CH422G_ADDR_MODE   0x24
#define CH422G_ADDR_OUTPUT 0x38
#define CH422G_MODE_IO_OE  0x01

#define EXIO_TP_RST        (1 << 1)
#define EXIO_DISP          (1 << 2)  // backlight enable
#define EXIO_LCD_RST       (1 << 3)
#define EXIO_SD_CS         (1 << 4)  // active low
#define EXIO_DI1           (1 << 5)  // isolated digital input DI1 on this board

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_ch422g_mode;
static i2c_master_dev_handle_t s_ch422g_out;
static uint8_t s_exio_state;

static esp_err_t i2c_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "i2c bus");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CH422G_ADDR_MODE,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_ch422g_mode), TAG, "ch422g mode");
    dev_cfg.device_address = CH422G_ADDR_OUTPUT;
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_ch422g_out), TAG, "ch422g out");

    const uint8_t mode = CH422G_MODE_IO_OE;
    return i2c_master_transmit(s_ch422g_mode, &mode, 1, 100);
}

static esp_err_t exio_write(uint8_t state)
{
    s_exio_state = state;
    return i2c_master_transmit(s_ch422g_out, &s_exio_state, 1, 100);
}

// GT911 latches its I2C address (0x5D) from INT being low while RST is released.
static esp_err_t touch_reset(void)
{
    const gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << PIN_TOUCH_INT,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_conf), TAG, "touch int gpio");

    ESP_RETURN_ON_ERROR(exio_write(EXIO_DISP | EXIO_LCD_RST | EXIO_DI1), TAG, "exio");
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(PIN_TOUCH_INT, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(exio_write(EXIO_TP_RST | EXIO_DISP | EXIO_LCD_RST | EXIO_DI1), TAG, "exio");
    vTaskDelay(pdMS_TO_TICKS(200));
    return ESP_OK;
}

esp_err_t board_backlight_set(bool on)
{
    uint8_t state = EXIO_TP_RST | EXIO_LCD_RST | EXIO_SD_CS;
    if (on) {
        state |= EXIO_DISP;
    }
    return exio_write(state);
}

static esp_err_t panel_init(uint8_t num_fbs, esp_lcd_panel_handle_t *panel)
{
    const esp_lcd_rgb_panel_config_t cfg = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = BOARD_LCD_H_RES,
            .v_res = BOARD_LCD_V_RES,
            .hsync_pulse_width = 30,
            .hsync_back_porch = 145,
            .hsync_front_porch = 170,
            .vsync_pulse_width = 2,
            .vsync_back_porch = 23,
            .vsync_front_porch = 12,
            .flags.pclk_active_neg = 1,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = num_fbs,
        .bounce_buffer_size_px = BOARD_LCD_H_RES * LCD_BOUNCE_LINES,
        .dma_burst_size = 64,
        .hsync_gpio_num = PIN_LCD_HSYNC,
        .vsync_gpio_num = PIN_LCD_VSYNC,
        .de_gpio_num = PIN_LCD_DE,
        .pclk_gpio_num = PIN_LCD_PCLK,
        .disp_gpio_num = -1,
        .data_gpio_nums = {
            GPIO_NUM_14, GPIO_NUM_38, GPIO_NUM_18, GPIO_NUM_17, GPIO_NUM_10,             // B3..B7
            GPIO_NUM_39, GPIO_NUM_0,  GPIO_NUM_45, GPIO_NUM_48, GPIO_NUM_47, GPIO_NUM_21, // G2..G7
            GPIO_NUM_1,  GPIO_NUM_2,  GPIO_NUM_42, GPIO_NUM_41, GPIO_NUM_40,             // R3..R7
        },
        .flags.fb_in_psram = 1,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&cfg, panel), TAG, "rgb panel");
    return esp_lcd_panel_init(*panel);
}

static esp_err_t touch_init(esp_lcd_touch_handle_t *touch)
{
    ESP_RETURN_ON_ERROR(touch_reset(), TAG, "touch reset");

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    io_cfg.scl_speed_hz = 400000;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_cfg, &io), TAG, "touch io");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = -1,
        .int_gpio_num = -1,
    };
    return esp_lcd_touch_new_i2c_gt911(io, &tp_cfg, touch);
}

esp_err_t board_init(esp_lv_adapter_tear_avoid_mode_t tear_mode,
                     esp_lv_adapter_rotation_t rotation,
                     board_handles_t *out)
{
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "i2c / CH422G");

    const uint8_t num_fbs = esp_lv_adapter_get_required_frame_buffer_count(tear_mode, rotation);
    ESP_LOGI(TAG, "RGB panel %dx%d, %u frame buffers", BOARD_LCD_H_RES, BOARD_LCD_V_RES, num_fbs);
    ESP_RETURN_ON_ERROR(panel_init(num_fbs, &out->panel), TAG, "panel");

    if (touch_init(&out->touch) != ESP_OK) {
        ESP_LOGE(TAG, "GT911 init failed, continuing without touch");
        out->touch = NULL;
    }
    return ESP_OK;
}

i2c_master_bus_handle_t board_i2c_bus(void)
{
    return s_i2c_bus;
}
