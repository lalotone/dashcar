# ESP32 device & platform reference: Waveshare ESP32-S3-Touch-LCD-5B

Hardware and platform facts for this project. Everything here was read from the device itself (esptool, boot
logs, on-device tests) or from Waveshare's official repo, unless marked otherwise.
Project overview: [`README.md`](README.md). Agent workflow: [`CLAUDE.md`](CLAUDE.md).

Contents:
1. Identity
2. Toolchain & flashing
3. Pin map
4. Memory budget (read before adding features)
5. Project configuration
6. Drivers, fonts, graphics
7. Networking
8. External APIs: details & quirks
9. Factory firmware
10. Hardware ideas / unused peripherals

---

## 1. Identity (read from the device)

| Item | Value |
|---|---|
| Board | Waveshare **ESP32-S3-Touch-LCD-5B**, the **1024×600** variant (the non-B is 800×480 with different timing) |
| SoC | ESP32-S3 (QFN56), revision **v0.2**, efuse block rev v1.4 |
| CPU | Dual-core Xtensa LX7 @ 240 MHz (+ LP core), Wi-Fi 2.4 GHz + BLE 5 |
| Flash | **16 MB**, quad (QIO), 3.3 V, manufacturer ID 0x46, device 0x4018 |
| PSRAM | **8 MB octal** (AP Memory, gen 3, 3 V), in-package (`ESP32-S3R8`) |
| Crystal | 40 MHz |
| MAC | unique per board: `uvx esptool --port /dev/ttyACM0 read-mac` |
| USB | Native USB-Serial/JTAG, VID:PID **303a:1001** ("Espressif USB JTAG/serial debug unit") |
| Serial port (Linux) | `/dev/ttyACM0` = `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00` |
| Permissions | your user must be in `dialout` (or `uucp` on some distros); no sudo needed then |

Quick identification: `uvx esptool --port /dev/ttyACM0 flash-id` (resets the board, writes nothing).

---

## 2. Toolchain & flashing

- **ESP-IDF v5.5.5** in `~/esp/esp-idf`, tools in `~/.espressif` (esp32s3 target only).
- The system Python is **3.14, too new for IDF 5.5**. The IDF venv is `~/.espressif/python_env/idf5.5_py3.12_env`.
- **Always run `. ./env.sh`** from the repo root (it sets `IDF_PYTHON_ENV_PATH`). Sourcing `export.sh` directly
  fails with "Python virtual environment ... py3.14 ... not found".
- Fresh machine:
  `git clone -b v5.5.5 --depth 1 --recursive --shallow-submodules https://github.com/espressif/esp-idf ~/esp/esp-idf`,
  then run `install.sh esp32s3` with a Python 3.12 first in `PATH`.
- esptool: inside the env use `python -m esptool ...`; outside it, `uvx esptool ...`.
- Flashing over USB-Serial/JTAG needs no buttons, and auto-reset works. If it doesn't: hold BOOT, tap RESET, release BOOT.
- App flash takes ~10 s. A full 16 MB read takes ~4 min (~580 kbit/s).

```sh
. ./env.sh
idf.py build                                  # ~1 min incremental; first build several minutes
idf.py -p /dev/ttyACM0 flash monitor          # Ctrl+] exits the monitor
rm sdkconfig && idf.py build                  # REQUIRED after editing sdkconfig.defaults
idf.py size                                   # memory summary (app ≈ 2.0 MB of the 6 MB partition)
python -m esptool erase_region 0x9000 0x6000  # wipe settings (NVS) only
```

Non-interactive log capture (agents): `uvx --with pyserial --with pillow python tools/devcap.py --dumps 0 --seconds 20`
resets the board and prints the console. See `CLAUDE.md` for screenshots.

**Backup / restore.** `backup/factory_demo_full_16MB.bin` is the full original flash
(sha256 `795555525710d9669be42464c97f6604f6a7d6a18e76e25bd7f45572b0f7802c`, gitignored).
Restore with `python -m esptool -p /dev/ttyACM0 write_flash 0 backup/factory_demo_full_16MB.bin`.
Waveshare also ships prebuilt test bins in its repo's `firmware/` folder (e.g. `09_lvgl_Porting_1024x600.bin`).

---

## 3. Pin map

Source: [waveshareteam/ESP32-S3-Touch-LCD-5](https://github.com/waveshareteam/ESP32-S3-Touch-LCD-5)
(`examples/ESP-IDF/09_lvgl_v9_demo`, `EXAMPLE_USE_1024_600_LCD`), cross-checked with the Arduino config.

### RGB LCD (ST7262, 16-bit RGB565, no SPI init sequence)
| Signal | GPIO | | Signal | GPIO |
|---|---|---|---|---|
| HSYNC | 46 | | VSYNC | 3 |
| DE | 5 | | PCLK | 7 |
| DISP / backlight | CH422G EXIO2 | | | |

Data lines, in `esp_lcd` `data_gpio_nums[0..15]` order:

| D0–D4 (B3..B7) | D5–D10 (G2..G7) | D11–D15 (R3..R7) |
|---|---|---|
| 14, 38, 18, 17, 10 | 39, 0, 45, 48, 47, 21 | 1, 2, 42, 41, 40 |

**5B timing:** pclk 21 MHz, `pclk_active_neg = 1`; hsync pulse/back/front = 30/145/170; vsync 2/23/12;
`bounce_buffer_size_px = 1024 * 10` (prevents drift while PSRAM is busy).
The 800×480 board uses different values (16 MHz, 4/8/8, 4/8/8). **Don't use those here.**

### I2C bus (shared): SDA GPIO 8, SCL GPIO 9, 400 kHz
| Address | Device |
|---|---|
| 0x24 | CH422G mode register (write 0x01 = IO pins are outputs) |
| 0x38 | CH422G output register (EXIO0..7) |
| 0x20–0x27, 0x30–0x3F | reserved by the CH422G (don't put other devices here) |
| 0x5D | GT911 touch (0x14 if INT is high during reset) |
| 0x51 | PCF85063A RTC (not used yet) |

### CH422G EXIO bits (register 0x38), write-only: always write the full byte (shadowed in `board.c`)
| Bit | Name | Use |
|---|---|---|
| 0 | DI0 | isolated digital input 0 |
| 1 | TP_RST | touch reset (1 = run) |
| 2 | DISP | **backlight enable**: on/off only, no PWM dimming |
| 3 | LCD_RST | panel reset (1 = run) |
| 4 | SD_CS | microSD chip select, **active low** |
| 5 | DI1 | isolated digital input 1 |

OC0/OC1 are isolated digital outputs DO0/DO1 (see Waveshare's `05_IO_Test`).
Known-good sequence: `0x24←0x01`; `0x38←0x2C`; GPIO4 low; `0x38←0x2E` (touch out of reset); backlight on = `0x38←0x1E`.

### Touch (GT911)
I2C as above. INT = **GPIO 4**, RST = EXIO1. Driving GPIO4 low while releasing reset latches address 0x5D.
GPIO4 is left as an output and touch is polled (no IRQ).
This unit reports `TouchPad_ID 0x39,0x31,0x31` ("911"), config version 82, native 1024×600 with no swap or mirror.

### Other peripherals
| Peripheral | Pins | Notes |
|---|---|---|
| CAN (TWAI) | TX 15, RX 16 | onboard **TJA1051T** transceiver, CANH/CANL on the terminal block, 120 Ω termination resistor |
| RS485 | RX 43, TX 44 | through an **SP3485 transceiver**: not usable as a TTL UART (e.g. for GPS) |
| microSD (SPI) | MOSI 11, MISO 13, CLK 12, CS = EXIO4 | `SDSPI_HOST_DEFAULT`, CS `-1` in the IDF config |
| RTC | PCF85063A @ 0x51 | could keep time without network |
| USB | GPIO 19/20 | native USB |
| Isolated DI0/DI1 | via CH422G (IO0, IO5) | PC817 optocouplers with a common terminal: can sense 12 V signals (ignition, headlights) |
| Isolated DO0/DO1 | via CH422G (OC0, OC1) | PC814 optocoupler outputs |
| External I2C header | shared SDA 8 / SCL 9 | `I2C_VCC` selectable; e.g. sensors or an I2C GPS (u-blox DDC at 0x42) |
| Power | DC input **6–36 V** (SY8293 buck) or USB-C | can run directly from car 12 V; Li-ion battery connector with a CS8501 charger and a battery switch (**turn it OFF when debugging on a PC**, per the schematic) |

**Unavailable GPIOs:** 26–32 (flash), 33–37 (octal PSRAM), 19/20 (USB). GPIO 6 appears in no Waveshare
example; check the schematic (`hardware/schematics/` in the Waveshare repo) before using it.

---

## 4. Memory budget: the main constraint

Internal SRAM, not PSRAM, runs out first. Boot log lines `dashcar: <stage> internal free … psram free …`:

| Stage | Internal free (largest block) | PSRAM free |
|---|---|---|
| boot | 258 KB (164 KB) | 6.46 MB |
| after Wi-Fi init | 194 KB (119 KB) | 6.40 MB |
| after display + LVGL | 108 KB (63 KB) | 3.92 MB |
| after UI built (home), v1.3 | ~98 KB | 3.63 MB |
| steady state, v1.3 (all services running) | ~79–91 KB | ~2.65 MB (rain map holds ~1 MB) |
| after UI built, v1.3.2 (single frame buffer) | ~121 KB | 4.83 MB |
| self-test, v1.3.2 (worst case across all cycles) | ≥ 102 KB | ≥ 3.7 MB |
| first ~10 s after Wi-Fi (Wi-Fi, SNTP, mDNS, first TLS) | dips to ~14 KB | — |
| radar open (rain map released) | ~78 KB | ~0.6–1.3 MB |
| after UI built, v1.4.0 (`DOUBLE_DIRECT`, 2 frame buffers) | ~121 KB | 3.63 MB |
| self-test, v1.4.0 (worst case across all cycles) | ≥ 102 KB | ≥ 2.1 MB |

**Fixed costs**

| Item | Size | Lives in |
|---|---|---|
| 1 frame buffer (1024×600×2) | 1.2 MB | PSRAM |
| RGB bounce buffers | 2 × 20 KB | internal |
| LVGL partial draw buffer (`buffer_height` 12 lines) | 24 KB | internal |
| Wi-Fi static RX buffers + driver | — | internal |
| Task stacks: net_worker 10 KB, touch 3 KB, httpd 6 KB, mDNS (16 tasks total) | — | internal |
| System event task (`sys_evt`) | 4 KB (raised from 2.3 KB) | internal |
| LVGL task stack | 16 KB | PSRAM |

**Radar while open:** base map + composite (1.1 MB each) + 7 radar frames (~0.95 MB), all in PSRAM.

**Rules learned the hard way** (all applied):
1. **Init Wi-Fi before the display.** Otherwise `esp_wifi_init → ESP_ERR_NO_MEM` ("Expected to init 16 rx buffer, actual is 14").
2. **Tear mode `DOUBLE_DIRECT`** since v1.4.0 (`NONE` in v1.3.2). LVGL direct mode renders into the back
   frame buffer; the panel swaps at vsync and LVGL then copies only the *invalidated areas* into the other
   buffer. No tearing on screen changes (with `NONE`, half-drawn screens were visible), and a clock tick
   still copies only a few KB. Costs a second 1.2 MB frame buffer in PSRAM; internal RAM is unchanged.
   History: the `DOUBLE_PARTIAL`/`TRIPLE_PARTIAL` modes The `DOUBLE_PARTIAL`/`TRIPLE_PARTIAL` modes
   copy the *whole unchanged rest of the frame* (~1.2 MB, CPU memcpy PSRAM→PSRAM, no DMA2D on the S3)
   after **every** refresh (`copy_unrendered_area_from_front_to_back` in the adapter's v9 bridge), even
   for a 1-second clock tick. That starved the RGB panel's scan-out (drift, glitches, a "blinking" status
   bar). Never use the partial modes.
   - Set the mode in **both** `board_init()` (panel `num_fbs`) and `disp_cfg.tear_avoid_mode`. A mismatch
     crashes at boot with `panel frame buffer request failed (required=N)`.
3. **`buffer_height` 12.** The default of 50 (100 KB) fails to allocate, and the adapter silently falls back
   (`alloc partial draw buffer ... failed`, `tear mode 4 setup failed`).
4. `CONFIG_ESP_WIFI_IRAM_OPT=n`, `CONFIG_ESP_WIFI_RX_IRAM_OPT=n` and `STATIC_RX_BUFFER_NUM=8` free about 60 KB.
5. **`CONFIG_MBEDTLS_HARDWARE_AES=n`.** HW AES needs internal DMA bounce buffers; with it on, HTTPS fails with
   `esp-aes: Failed to allocate memory`.
6. **TLS and lwIP to PSRAM:** `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` and `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`.
7. **LVGL heap in PSRAM:** `CONFIG_LV_USE_CUSTOM_MALLOC` + `main/lv_mem_psram.c`. `main` is linked `WHOLE_ARCHIVE`,
   or the linker drops those symbols with "undefined reference to lv_malloc_core".
8. **cJSON in PSRAM:** `cJSON_InitHooks` in `main.c`. The radar forecast grid is a 52 KB JSON document of thousands of nodes.
9. `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536` keeps internal RAM for explicit internal/DMA allocations.
10. **Do not** lower `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` globally (tried 256). The LVGL adapter then mallocs its
    RGB callback context in PSRAM, which fails with `register_event_callbacks: user context not in internal RAM`.
11. **Allocate explicitly:** big or long-lived app buffers use `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`. Plain
    `malloc` under 16 KB goes to internal RAM first.
12. **Signs of internal-RAM starvation:** `wifi:mem fail`, `wifi:m f null`, `esp-tls: Failed to create socket`.
    Opening a worker task fails with "Out of memory" in the UI.
13. **Next things to try if internal RAM runs short:** smaller task stacks (6 KB); fewer bounce-buffer lines
    (8 or 6; must divide 600 into an even count); `buffer_height` 8.

---

## 5. Project configuration

- **Partitions** (`partitions.csv`, OTA layout since v1.2):

  | Partition | Offset | Size | Contents |
  |---|---|---|---|
  | nvs | 0x9000 | 24 KB | settings; kept at the old offset so settings survived the layout change |
  | otadata | 0xF000 | 8 KB | which app slot boots |
  | phy_init | 0x11000 | 4 KB | |
  | **ota_0** | 0x20000 | 4 MB | app slot (~2.1 MB used) |
  | **ota_1** | 0x420000 | 4 MB | app slot |
  | storage | 0x820000 | 4 MB | SPIFFS at `/tiles` (map tile cache) |

  About 4 MB of flash is unpartitioned.
  - **Rollback:** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`. `idf.py flash` writes ota_0 and resets otadata.
  - **Boot log:** it reports `Loaded app from partition at offset 0x20000` (ota_0) or `0x420000` (ota_1).
- Flash QIO 80 MHz. PSRAM octal **80 MHz**; the factory demo ran 120 MHz, which needs experimental config.
- `CONFIG_LCD_RGB_ISR_IRAM_SAFE=y`, `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y` (recover from display drift),
  `CONFIG_FREERTOS_HZ=1000`, `CONFIG_COMPILER_OPTIMIZATION_PERF=y`.
- LVGL extras on: Tiny TTF, snapshot (debug screenshots), QR code (fuel navigation), canvas.
- **NVS** namespace `dashcar` (`settings.c`):

  | Key | Type | Meaning |
  |---|---|---|
  | `ssid`, `pass` | str | Wi-Fi credentials |
  | `loc_lat`, `loc_lon` | blob (float) | chosen location |
  | `loc_name` | str | chosen location name (absent = locate by IP) |
  | `imperial` | u8 | units |
  | `fuel_prod` | u8 | fuel product ID (default 4) |
  | `fuel_km` | u8 | fuel search radius (default 10) |
  | `night` | u8 | night mode 0 auto / 1 on / 2 off |
  | `ota` | u8 | wireless updates on (default 1) |
  | `utc_off` | i32 | last forecast UTC offset (local time at boot, before the first forecast) |
  | `park_lat`, `park_lon` | blob (double) | parking spot (absent = demo spot) |
  | `park_t` | i64 | when it was saved (UTC) |
  | `wx_cache` | blob (`weather_t`) | last good forecast, shown at boot if < 12 h old (ignored if the struct size changed) |

  The Wi-Fi driver's own storage is RAM-only (`WIFI_STORAGE_RAM`).

**Components** (`main/idf_component.yml`, resolved in `dependencies.lock`):

| Component | Resolved | Notes |
|---|---|---|
| lvgl/lvgl `^9` | **9.6.0** | see §6 for API changes |
| espressif/esp_lvgl_adapter `^0.5.2` | 0.5.3 | LVGL task; display/touch registration; **recursive** `esp_lv_adapter_lock()`. Pulls in freetype, button, knob… (unused) |
| espressif/esp_lcd_touch_gt911 `^1` | 1.x | works with the new `i2c_master` bus (set `scl_speed_hz`) |
| espressif/libpng `^1.6` | 1.6.58 | simplified read API (`png_image_*`) for radar and label tiles |
| espressif/esp_new_jpeg `^1` | 1.0.x | JPEG to RGB565_LE for basemap tiles; output buffer must be 16-byte aligned |
| espressif/mdns `^1.14` | 1.14 | `dashcar.local` for the update server |
| json (cJSON) | IDF built-in | moves out of IDF in v6 (`espressif/cjson`) |

---

## 6. Drivers, fonts, graphics

- **I2C:** use the new driver only (`driver/i2c_master.h`). Waveshare's examples use the legacy `driver/i2c.h`;
  mixing the two aborts at runtime.
- **LVGL 9.6 API changes:**
  - `lv_obj_add_flag`/`lv_obj_remove_flag` are deprecated → use `lv_obj_set_hidden/clickable/scrollable/event_bubble`.
  - `lv_list` is deprecated → use the `ui_list_*` helpers in `ui.c`.
  - Public headers are under `managed_components/lvgl__lvgl/include/lvgl/`.
  - `lv_line_set_points` keeps the caller's pointer, so give it a copy (see `wx_icon.c`).
  - LVGL 9.6 bug: deleting any canvas (incl. `lv_qrcode`) logs `lv_image_src_get_type: image src … invalid
    magic` twice, because `lv_canvas_destructor` passes `&canvas->draw_buf` instead of `canvas->draw_buf` to
    `lv_image_cache_drop` (backtrace-verified). Harmless; don't patch `managed_components/` (it gets re-fetched).
- **Fonts:** LVGL's built-in Montserrat is **ASCII only** (no `°`, `€`, `·`, `©`, accents). Any text with those
  characters must use the `ui_font_*` Tiny TTF fonts. The built-in fonts serve as `fallback` for `LV_SYMBOL_*` icons.
  The subsets in `main/fonts/` (~21 KB each) cover Latin-1, Latin Extended-A, quotes, dashes, bullet, ellipsis, €,
  arrows and minus. To add glyphs, re-run with the original TTFs from
  github.com/JulietaUla/Montserrat (`fonts/ttf/`):
  ```sh
  uvx --from fonttools pyftsubset Montserrat-Medium.ttf --layout-features='' --no-hinting \
    --unicodes="U+0020-007E,U+00A0-017F,U+2013-2014,U+2018-201E,U+2022,U+2026,U+20AC,U+2190-2193,U+2212" \
    --output-file=main/fonts/Montserrat-Medium.ttf      # same for SemiBold
  ```
  Sizes: sm 18, md 22, lg 28 (semibold), xl 44 (semibold), display 64 (semibold), huge 112.
- **Images:**
  - **Logo:** `ui/dacia_logo.c` is an **A8** (alpha-only) bitmap, 440×123 = 54 KB, generated by `tools/gen_logo.py`
    from the Wikimedia "Dacia 2021 symbol" path. A8 images are tinted with `image_recolor`. To draw at another
    size, use `lv_image_set_inner_align(…, LV_IMAGE_ALIGN_STRETCH)` with a set object size, or `lv_image_set_scale`.
  - **Icons:** weather, fuel and radar icons are drawn from LVGL primitives (`wx_icon.c`, `ui_home.c`), with no assets.
- **Rendering performance** (measured with LV_EVENT_REFR_READY):

  | What | Time |
  |---|---|
  | Full-screen redraw of a simple screen | ~200–250 ms (12-line internal strips, two cores) |
  | New screen to first frame | 150–700 ms (weather ~700, fuel ~450) |
  | Fuel list, 60 rows | build 1.0 s + redraw 1.8 s, which froze the UI |
  | Fuel list, 20 rows (now) | 170 ms + 650 ms |

  Swapping the TTF fonts for bitmap fonts only cut the redraw by ~35% and the build not at all: the cost is
  widget count and layout. LVGL's heap is in PSRAM (§4), which makes object-heavy screens slower.
- **Touch input is buffered** (`ui/touch_input.c`): a task polls the GT911 every 10 ms into a queue, and the
  LVGL read callback replays every sample (`continue_reading`). Before this, LVGL only sampled touch between
  renders, so taps during a 300–800 ms render were lost and swipes became single points.
- **Task model:** one `net_worker` task runs every network job in sequence (see README). Concurrent TLS
  sessions at boot were the main cause of internal-RAM dips (to 8 KB), and runtime `xTaskCreate` then failed.
- **Display drift/flicker:** the RGB panel scans out of PSRAM through bounce buffers. Long PSRAM-heavy loops
  (full 1024×536 recomposes, big memcpy) can starve it; the picture shifts until the next frame
  (`LCD_RGB_RESTART_IN_VSYNC` resyncs it). The radar now recomposes every 4 base tiles instead of every tile
  and yields between tiles.
- **Radar compositor** (`app_radar.c`): plain RGB565 loops on a 1024×536 `lv_canvas` buffer. Radar frames are
  stored as 1-byte palette indices (~100 colours, shared palette). Forecast fields are bilinear-interpolated
  from a 12×7 grid.

---

## 7. Networking

- **Wi-Fi:** STA only, power save off.
  - **Boot:** `wifi_mgr_connect(..., persistent=true)` keeps retrying in the background.
  - **Setup form:** connects with `persistent=false` and reports FAILED.
  - **Dropped link:** retries every 5 s.
  - **Scanning** aborts an attempt that hasn't connected yet. Leaving the setup screen resumes the saved network.
  - Reason codes map to user text in `wifi_mgr.c` (AUTH_FAIL/handshake → "Wrong password", NO_AP_FOUND → "Network not found").
- **Time:** SNTP (`pool.ntp.org`, `time.google.com`), started on first IP. There's no timezone database:
  local time = UTC + Open-Meteo's `utc_offset_seconds` for the forecast location.
- **HTTPS:** `esp_http_client` with the IDF CA bundle.
  - `http_get()`: one-shot, body capped at 128 KB in PSRAM.
  - `http_session_get()`: keeps the TLS connection alive between requests to the same host; this is what made
    loading ~60 radar tiles practical. Per-call body cap; retries once on a fresh connection.
- **Wireless updates** (`services/ota.c`):
  - **Server:** `esp_http_server` on port 80 with mDNS `dashcar.local`. It's started from a helper task when Wi-Fi
    connects, not from the event task (that overflowed `sys_evt`).
  - **Image check:** `POST /update` checks the first chunk's `esp_app_desc_t` (magic and `project_name ==
    "dashcar"`) before `esp_ota_begin`. It writes with `OTA_WITH_SEQUENTIAL_WRITES`, then `esp_ota_end` verifies
    the image.
  - **Timing:** a 2.1 MB image installs in ~18 s over the phone hotspot.
  - **Rollback:** new images boot `PENDING_VERIFY` and `ota_mark_valid_later(20)` confirms them, so a crash in
    the first 20 s rolls back. Verified on the device: a crashing debug build was rolled back automatically.
- **RTC** (`services/ext_rtc.c`): PCF85063A at 0x51.
  - **Format:** time registers 0x04–0x0A in BCD, holding UTC. Bit 7 of seconds (OS) set means the time is invalid.
  - **Sanity check:** years outside 2025–2040 are also treated as invalid; the factory demo left 2054 in it.
  - **Updates:** written after every SNTP sync (`wifi_mgr_set_time_sync_cb`).
  - **Power-off:** keeping time while unpowered needs a backup cell on the board's RTC connector (untested;
    see the Settings → Clock status).
  - **Naming:** functions are `ext_rtc_*` because IDF already defines `rtc_init`.
- **TX buffer:** `buffer_size_tx` is 2048 (one-shot) / 3072 (sessions). The 512 B default breaks long URLs with
  `HTTP_HEADER: Buffer length is small`.
- **Response headers:** `esp_http_client` sends no `Accept-Encoding`, so servers reply uncompressed.

---

## 8. External APIs: details & quirks

**MET Norway** (backup forecast, `services/weather.c` `parse_met`; used when Open-Meteo fails, then for 1 h):
- `https://api.met.no/weatherapi/locationforecast/2.0/complete?lat=..&lon=..` (~62 KB JSON). Max 4 decimals
  in the coordinates (else 403). Terms ask for an identifying User-Agent (ideally with contact info); our
  generic `dashcar/1.0 (ESP32-S3)` is accepted today. Limits are per app, not per IP.
- Hourly steps for ~2.5 days, then 6-hourly (`next_6_hours` has `air_temperature_max/min`) to ~9 days.
  Fields: `air_temperature`, `apparent_air_temperature`, `relative_humidity`, `wind_speed` (m/s),
  `wind_from_direction`, `ultraviolet_index_clear_sky` (hourly part only), `precipitation_amount`, `symbol_code`
  (e.g. `lightrainshowers_day`, mapped to WMO codes; `_day/_night` gives is_day).
- **Missing vs Open-Meteo:** precipitation probability (not in Spain: `precip_prob = -1`, the UI shows the
  amount), 15-min nowcast (`rain15_count = 0`), time zone (uses the last Open-Meteo offset from NVS; can be 1 h
  off across a DST change if Open-Meteo stays blocked), sunrise/sunset (computed on the device, ±2 min).
- It starts at the current hour, so "today" only covers the remaining hours; today's high/low/UV are merged
  with the previous forecast for the same day and place.

**Open-Meteo: HTTP 429 "Daily API request limit exceeded"** (10,000/day **per public IP**). Mobile carriers
share IPs (CGNAT), so the hotspot can hit it with our own usage in the hundreds. Hence the MET backup, the
retry back-off and the NVS cache (`wx_cache`).

**Open-Meteo** (no key):
- **Forecast** (`services/weather.c`):
  `api.open-meteo.com/v1/forecast?latitude&longitude&current=…&hourly=…&daily=…&minutely_15=precipitation&forecast_minutely_15=8&timezone=auto&timeformat=unixtime`.
  - Daily `time` is the UTC epoch of **local** midnight. Add `utc_offset_seconds` before formatting the weekday.
  - `minutely_15[0]` is the current 15-minute slot.
- **Multi-point:** comma-separated lat/lon lists return a JSON array. 84 points × 13 h ≈ 52 KB, URL ≈ 1.3 KB.
- **Geocoding:** `geocoding-api.open-meteo.com/v1/search?name=…&count=8&language=en&format=json`.

**IP geolocation:** `https://ipinfo.io/json` (`loc` = "lat,lon"), with fallback `http://ip-api.com/json/`
(the free tier is HTTP-only).

**Basemap** (radar): **Esri Dark Gray Canvas**, no key.
- Base layer: JPEG from `server.arcgisonline.com/ArcGIS/rest/services/Canvas/World_Dark_Gray_Base/MapServer/tile/{z}/{y}/{x}`.
  Note **y before x**.
- Labels layer: PNG RGBA from `.../World_Dark_Gray_Reference/MapServer/tile/{z}/{y}/{x}`.
- Both are cached forever in SPIFFS as `b8_x_y` and `r8_x_y`.
- **CARTO basemaps now require an API key.** They return HTTP 200 with an "API KEY REQUIRED" image, so always
  inspect tile pixels when evaluating a provider.

**RainViewer** (radar; free-tier terms since 2026-01):
- `api.rainviewer.com/public/weather-maps.json` lists `radar.past[]` (13 frames, 10 min apart, `path` is a hash)
  and an empty `nowcast`.
- Tiles: `{host}{path}/256/{z}/{x}/{y}/2/1_1.png`.
- **Max zoom 7.** z8 and above return a "Zoom Level Not Supported" image.
- The colour-scheme parameter is ignored (always "Universal Blue"). Limit is 100 requests/IP/minute.
- We use 7 frames 20 min apart, newest first, at about 6 tiles per frame.

**Spanish fuel prices** (Ministerio, no key):
- Base URL: `https://energia.serviciosmin.gob.es/ServiciosRestCarburantes/PreciosCarburantes/`. Returns JSON by
  default; the TLS chain ends at DigiCert Global Root G2 (in the bundle).
- **Endpoints:**

  | Endpoint | Size | Use |
  |---|---|---|
  | `EstacionesTerrestres/` (all of Spain) | 12 MB / 16 s | too big |
  | `FiltroProducto/{id}` | 4.3 MB | too big |
  | **`FiltroProvinciaProducto/{prov}/{prod}`** | ~90 KB, 0.6 s | used; `2` and `02` both work |
  | `FiltroProvincia/`, `FiltroMunicipio/`, `Listados/Provincias/`, `Listados/ProductosPetroliferos/` | — | others |

- **Product IDs:** 4 Gasóleo A, 5 Gasóleo Premium, 1 Gasolina 95 E5, 23 G95 E10, 3 Gasolina 98 E5,
  17 GLP, 18 GNC, 22 H2, 26 AdBlue.
- **Parsing quirks:** numbers are strings with decimal commas (`"1,459"`). Keys contain UTF-8 accents
  (`"Rótulo"`, `"Dirección"`, `"Longitud (WGS84)"`). Text is uppercase (we title-case it). `"Tipo Venta": "R"`
  means members/fleets only and is skipped. `"Horario"` contains "24H" for 24-hour stations.
- **Which provinces:** there's no GPS, so the app queries the nearest province by centre, plus any whose centre is
  within +45 km of that distance, up to 3. The centres in `fuel.c` are station centroids per province, generated
  from the full dataset.

**MeteoAlarm** (warnings, no key):
- **Feeds:**

  | Feed | Use |
  |---|---|
  | `feeds.meteoalarm.org/api/v1/warnings/feeds-spain` (JSON) | 3.3 MB, too big |
  | **`feeds.meteoalarm.org/feeds/meteoalarm-legacy-atom-spain`** (Atom) | ~330 KB, used |
  | `api.meteoalarm.org/edr/v1/...` (point query) | needs a token |

- **Atom content:** English only, one `<entry>` per alert × area, including expired ones (filter on
  `cap:expires`). The area is `cap:geocode` EMMA_ID (e.g. `ES107`). Fields used: `cap:areaDesc`,
  `cap:event`, `cap:severity` (Moderate/Severe/Extreme = yellow/orange/red), `cap:onset`, `cap:expires`, and
  `title` ("Yellow Wind Warning issued for Spain - …").
- **Zone lookup:** polygons come from MeteoAlarm's geocodes file
  (`gitlab.com/meteoalarm-pm-group/documents`, e.g. `MeteoAlarm_Geocodes_2026_07_31.json`, 33 MB, all of Europe).
  `tools/gen_zones.py` keeps the 233 `ES` zones, simplified at 0.005° → 7,588 points (~59 KB), and asserts
  that Zaragoza falls in ES107.
- **Verified live:** "Yellow Wind Warning, Ribera del Ebro de Zaragoza" on 2026-10-08.

**DGT traffic** (Spain, no key):
- **Feed:** `https://nap.dgt.es/datex2/v3/dgt/SituationPublication/datex2_v37.xml`, DATEX II v3.7. The v36 URL
  redirects to it.
  - Size: **5.4 MB** plain, **~175 KB with `Accept-Encoding: gzip`** (`http_get_stream_gzip` inflates on the fly
    with zlib, buffers in PSRAM).
  - Updated every minute (`max-age=60`, no 304 support).
- **Structure:** ~900 `<sit:situation>` (one incident; up to ~52 KB each, the parser buffers 64 KB),
  ~1,250 `<sit:situationRecord xsi:type=...>`.
- **Record types:**

  | Type | Count |
  |---|---|
  | `RoadOrCarriagewayOrLaneManagement` (lane/road closures, alternating traffic) | 748 |
  | `GenericSituationRecord` (cause in `sit:causeType`) | 373 |
  | `AbnormalTraffic` (`slowTraffic`) | 66 |
  | `GeneralObstruction` (`objectOnTheRoad`...) | 24 |
  | `SpeedManagement` | 19 |
  | Others | few |

  `vehicleObstructionType=vehicleStuck` is common (broken-down vehicle).
- **Fields used:**
  - `loc:latitude/longitude` (first point)
  - `loc:roadName`
  - `lse:kilometerPoint`
  - `lse:municipality`
  - `loc:tpegDirection` (`eastBound`... / `unknown`)
  - `com:overallStartTime` / `overallEndTime` (ISO with `.000+01:00`)
  - `sit:severity` (rarely present)
- **Severity:** our own mapping in `traffic.c`.
  - High: accident, road/carriageway closed, fire, flooding, hazardous load.
  - Medium: slow traffic, broken-down vehicle, objects, rockfall.
  - Low: roadworks, lane restrictions, speed limits.

**Home rain map** (`services/rain_now.c`):
- Basemap: Esri z9 tiles (cache `b9_*`/`r9_*`), about 100×80 km.
- Radar: RainViewer z7 shown 4×.
- Memory: 440×340, double-buffered (~1 MB PSRAM). Rebuilt when the location moves > 3 km.

**Google Maps URL** (fuel QR): `https://www.google.com/maps/dir/?api=1&destination=LAT,LON&travelmode=driving`.
It opens navigation in the Maps app on Android or iOS, or in the browser. Verified by decoding a device
screenshot with OpenCV.

---

## 9. Factory firmware (before we flashed)

ESP-IDF v5.4-dev, built Aug 2025. Partitions: nvs, phy_init, factory 4 MB, storage 1 MB. Flash QIO 80 MHz, PSRAM 120 MHz.
It initialised the GT911, RGB LCD + LVGL, CAN (a test frame timed out with nothing on the bus), RS485 and Wi-Fi STA.

---

## 10. Hardware ideas / unused peripherals

- **No GPS:** location is by IP or a chosen city. Options:
  - an **I2C GPS** (u-blox, DDC protocol at 0x42) on the external I2C header. The RS485 pins can't take a TTL
    UART GPS because of the SP3485 transceiver;
  - Wi-Fi-based geolocation (send scanned BSSIDs to a free service such as beaconDB; coverage untested);
  - a phone companion app.
- **Power and ignition:** feed the 6–36 V input from a fused switched 12 V, or use an isolated DI to sense ignition
  (screen off / sleep) or headlights (night mode).
- **CAN (GPIO 15/16):** could read OBD-II (500 kbit/s on most cars) with proper wiring and termination.
- **RTC (PCF85063A):** could show the time before Wi-Fi/SNTP is up.
- **Backlight is on/off only.** Night dimming would need a hardware mod, or darker UI colours after sunset
  (sunset time is already in the forecast).
- **microSD:** would hold a larger tile cache or logs (SPI pins above, CS through EXIO4).
