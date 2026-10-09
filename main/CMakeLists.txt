set(srcs_debug)
if(CONFIG_DASHCAR_SELFTEST)
    list(APPEND srcs_debug "debug/selftest.c")  # tools/selftest.sh builds only
endif()

idf_component_register(
    SRCS
        ${srcs_debug}
        "main.c"
        "settings.c"
        "lv_mem_psram.c"
        "debug/screenshot.c"
        "debug/touch_sim.c"
        "board/board.c"
        "net/wifi_mgr.c"
        "net/http_util.c"
        "services/weather.c"
        "services/map_tiles.c"
        "services/fuel.c"
        "services/ext_rtc.c"
        "services/alerts.c"
        "services/warn_zones.c"
        "services/ota.c"
        "services/timeutil.c"
        "services/location.c"
        "services/traffic.c"
        "services/rain_now.c"
        "services/net_worker.c"
        "services/parking.c"
        "ui/ui.c"
        "ui/ui_wifi.c"
        "ui/ui_home.c"
        "ui/app_weather.c"
        "ui/wx_icon.c"
        "ui/app_radar.c"
        "ui/app_fuel.c"
        "ui/ui_settings.c"
        "ui/touch_input.c"
        "ui/app_traffic.c"
        "ui/app_parking.c"
        "ui/dacia_logo.c"
    INCLUDE_DIRS "."
    WHOLE_ARCHIVE  # lv_mem_psram.c provides symbols only LVGL references
    PRIV_REQUIRES
        esp_driver_gpio esp_driver_i2c esp_lcd esp_wifi esp_netif esp_event
        esp_http_client esp-tls mbedtls nvs_flash json spiffs espressif__libpng espressif__esp_new_jpeg espressif__mdns espressif__zlib esp_http_server app_update esp_app_format bootloader_support
    EMBED_FILES
        "fonts/Montserrat-Medium.ttf"
        "fonts/Montserrat-SemiBold.ttf")
