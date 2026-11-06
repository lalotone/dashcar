set(srcs_debug)
if(CONFIG_DASHCAR_SELFTEST)
    list(APPEND srcs_debug "debug/selftest.c")  # tools/selftest.sh builds only
endif()
if(CONFIG_DASHCAR_SOAK)
    list(APPEND srcs_debug "debug/soak.c")      # tools/soak.sh builds only
endif()

# Splash emblem: the owner's private one (tools/gen_logo.py --dacia, git-ignored) when present,
# else the DashCar "DC". After adding or removing the private file, run `idf.py reconfigure`.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/ui/emblem_private.c")
    set(src_emblem "ui/emblem_private.c")
else()
    set(src_emblem "ui/emblem.c")
endif()
message(STATUS "DashCar splash emblem: ${src_emblem}")

idf_component_register(
    SRCS
        ${srcs_debug}
        ${src_emblem}
        "main.c"
        "crashlog.c"
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
        "services/gps.c"
        "services/trip.c"
        "services/cameras.c"
        "services/speedlimit.c"
        "services/poi.c"
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
        "ui/app_gps.c"
        "ui/app_gps_signal.c"
        "ui/app_trip.c"
        "ui/app_cameras.c"
        "ui/app_explore.c"
        "ui/cam_alert.c"
        "ui/app_logo.c"
    INCLUDE_DIRS "."
    WHOLE_ARCHIVE  # lv_mem_psram.c provides symbols only LVGL references
    PRIV_REQUIRES
        esp_driver_gpio esp_driver_i2c esp_driver_uart esp_timer esp_lcd esp_wifi esp_netif esp_event
        esp_http_client esp-tls mbedtls nvs_flash json spiffs espressif__libpng espressif__esp_new_jpeg espressif__mdns espressif__zlib esp_http_server app_update esp_app_format bootloader_support
    EMBED_FILES
        "fonts/Montserrat-Medium.ttf"
        "fonts/Montserrat-SemiBold.ttf")

if(src_emblem STREQUAL "ui/emblem_private.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DASHCAR_EMBLEM_PRIVATE=1)  # splash composition
endif()

# crashlog.c records each crash (PCs, task) in RTC memory before the normal panic output.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")
