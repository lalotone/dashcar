# CLAUDE.md: working on DashCar

ESP-IDF 5.5 + LVGL 9.6 dashboard for a **Waveshare ESP32-S3-Touch-LCD-5B** (1024×600), mounted in the user's Dacia.
It connects through the user's phone hotspot in Spain.

Read before changing anything:
- [`README.md`](README.md): features, architecture, source map, how to add an app, troubleshooting
- [`ESP32-info.md`](ESP32-info.md): pins, **memory budget (§4)**, config, API quirks (§8)

## Environment

- The board is plugged in over USB as `/dev/ttyACM0`. Flashing it is the normal dev loop and is expected.
  The original firmware is backed up in `backup/`.
- **The user is often watching or touching the physical screen while you test.** A screen you're testing can
  be closed under you, which looks like "the radar context was freed". Tell them when a test needs about a
  minute hands-off.
- The repo is **not a git repository**, so there's no history to fall back on. Make careful edits.
- Toolchain: always run `. ./env.sh` first (ESP-IDF 5.5.5 with the Python 3.12 venv; the system Python 3.14
  breaks IDF). Use `command grep` in zsh one-liners, because `grep` may be aliased.

```sh
. ./env.sh && idf.py build                      # check: no warnings from main/ (keep it that way)
tools/ota.sh                                    # install over Wi-Fi (this PC is usually on the hotspot too)
idf.py -p /dev/ttyACM0 flash                    # USB: needed for crash logs, bootloader/partition changes
rm sdkconfig && idf.py build                    # after ANY edit to sdkconfig.defaults
uvx --with pyserial --with pillow python tools/devcap.py --dumps 0 --seconds 20   # reset + read boot log
```

## Verifying UI changes without seeing the screen

1. Add a temporary hook in `main/main.c`, wrapped in `// TEMP-AUTOTEST-BEGIN` / `// TEMP-AUTOTEST-END`, and start
   it after `log_heap("ui")` with an `xTaskCreate(..., 12288, ...)` line marked `// TEMP-AUTOTEST`. Smaller stacks
   overflow once the task opens popups. The task should:
   - wait for its preconditions (e.g. `weather_get()` returns true);
   - open the screen under the LVGL lock (`esp_lv_adapter_lock(-1)` … `unlock`);
   - wait for data to load, then call
     `dbg_screenshot("tag", NULL, NULL, 2)` from `debug/screenshot.h`.

   Use `lv_layer_top()` as the object for popups and the status bar. Pass an `lv_area_t` plus step 1 for a
   small full-resolution crop, e.g. a QR code. To close a popup, delete only it
   (`lv_obj_get_child(lv_layer_top(), -1)`); **never `lv_obj_clean(lv_layer_top())`**, which deletes the status
   bar and crashes its timer. The night overlay lives on `lv_layer_sys()` and isn't in snapshots.
2. Build, flash, then:
   `uvx --with pyserial --with pillow python tools/devcap.py --dumps N --out <scratchpad>/devcap`.
   Add `--with opencv-python-headless --with numpy … --qr` to decode QR codes.
3. Look at the PNGs with the Read tool. A half-resolution full screen takes about 10 s to transfer. A few rows
   may be lost to interleaved logs; harmless watchdog warnings may appear.
4. **Remove the hook:** `command grep -rn AUTOTEST main` must print nothing. Then rebuild and flash the clean
   firmware (`tools/ota.sh`).

**Debug builds and rollback:** a debug build pushed over OTA that crashes within 20 s is rolled back to the
previous image. That looks like "my change didn't take" (check `Compile time` in the boot log). Flash debug
builds over USB to see the crash.

Also check `dashcar:` heap lines and new `E (`/`W (` lines in the log after every change.

**Interaction tests:** `debug/touch_sim.h` (`dbg_touch_tap`, `dbg_touch_swipe`) injects a virtual finger into
the same buffered input queue as the panel, so taps and gestures can be tested from a hook. For anything
touching navigation, screens or timers, run the full stress test:
- `tools/selftest.sh`: builds into `build-selftest/` (its sdkconfig is regenerated every run so it matches the
  product), flashes over USB, runs ~4 min, prints `SELFTEST PASSED` or `FAILED` with heap per cycle.
- A half-resolution full-screen `dbg_screenshot` takes **~55 s** over serial: give devcap `--seconds 200`.
  Crop with an `lv_area_t` when you can.
- Afterwards, re-flash normal firmware (`idf.py -p /dev/ttyACM0 flash`).
- Extend `main/debug/selftest.c` when adding screens.
- Decode crash backtraces: `xtensa-esp32s3-elf-addr2line -pfiaC -e build/dashcar.elf <addrs>`.

## Rules that are easy to break

- **Threading:** LVGL only on the LVGL task or under `ui_lock()`. Callbacks from other tasks must re-check that
  their screen still exists (`s_scr`, generation counters). Do blocking HTTP in worker tasks, never in event
  callbacks. Pattern: `fetch_task` in `ui/app_fuel.c`.
- **Memory** (`ESP32-info.md` §4):
  - Internal RAM is the scarce resource.
  - Put new big buffers in PSRAM (`heap_caps_malloc(…, MALLOC_CAP_SPIRAM)`).
  - Don't lower `SPIRAM_MALLOC_ALWAYSINTERNAL` globally; it breaks the RGB driver.
  - Keep `wifi_mgr_init()` before `board_init()`.
  - Worker task stacks are internal (8 KB each).
- **Fonts:** built-in `lv_font_montserrat_*` are ASCII-only. Any `° € · © á ñ` text must use `ui_font_*`.
  New glyphs need a font re-subset (`ESP32-info.md` §6).
- **LVGL 9.6:** use `lv_obj_set_hidden/clickable/scrollable/event_bubble`, not add/remove_flag. Use the
  `ui_list_*` helpers, not `lv_list`. `lv_line` keeps the caller's points pointer.
- **HTTP:** URLs over ~2 KB need a bigger `buffer_size_tx`. Many tiles to one host: use `http_session_get()`.
  When evaluating a tile or image provider, **check the pixels**: CARTO returns 200 with an "API KEY REQUIRED"
  image, and RainViewer returns a "Zoom Level Not Supported" image above z7.
- **Generated files:** `ui/dacia_logo.c` (`tools/gen_logo.py`) and `services/warn_zones.c` (`tools/gen_zones.py`).
  Don't hand-edit them.
- **Navigation:** every non-home screen uses `ui_header_create(scr, title, on_back)`, which gives the big back
  button plus swipe-right-to-go-back. The status bar is the home button. Keep touch targets ≥ 48 px.
  - Never load another screen from inside an `LV_EVENT_GESTURE` handler: use `lv_async_call`. Doing it
    synchronously double-frees the screen's event list.
  - Every screen swallows the click that would follow a swipe (`ui_screen_create`).
- **Screen lifetime:** a screen can be opened again before the old instance is deleted (double tap).
  - Create timers with `ui_screen_own_timer(scr, t)`, never a static you delete in the handler.
  - Start delete handlers with `if (lv_event_get_target_obj(e) != s_scr) return;`.
  - Listener registration is de-duplicated.
  - Per-instance resources (e.g. radar ctx) go in the delete callback's user data.
- **No runtime task creation.** All network/IO work goes through `services/net_worker`:
  - services register a *step* (called ~1/s, returns fast unless a fetch is due);
  - screens `net_worker_post()` jobs.
  `xTaskCreate` at runtime failed with "out of memory" whenever internal RAM was fragmented.
  Only debug code may create tasks, and then with a PSRAM stack (`xTaskCreatePinnedToCoreWithCaps`).
- **Location:** always use `location_get()` (services/location), never the weather data directly. The GPS
  will plug in there.
- **Big static buffers** (arrays of structs in UI code) need `EXT_RAM_BSS_ATTR`, or they silently take
  internal RAM (a 34 KB `static traffic_item_t[200]` once cost 50 KB at boot).
- **PSRAM budget:** frame buffers 2.4 MB; home rain map ~1 MB (released while the Radar is open, see
  `rain_now_pause/release/resume`); Radar ~2.3 MB + frames. Check `psram free` in the boot log and on
  `http://dashcar.local/` (which also shows internal free/largest/min-ever).
- **Display mode:** tear mode `DOUBLE_DIRECT` (2 frame buffers, only dirty areas copied; no tearing). Never the
  double/triple *partial* modes: they copy
  ~1.2 MB per refresh and caused visible glitches.
- **Only touch widgets when something changed:** `ui_label_set_text_if_changed`, `ui_set_hidden_if_changed`.
  Every redraw is PSRAM traffic, and 1-second timers re-setting identical text kept the screen redrawing.
- **PSRAM bandwidth:** the RGB panel scans out of PSRAM. Long tight loops over big buffers (memcpy of a
  full map, per-pixel compose) must work in ~64-row chunks with `vTaskDelay(1)` in between, or the display
  drifts/glitches.
- **Background loaders** (radar): free big buffers in the delete handler, check `cancel` under the lock before
  writing them, and don't recompose full-screen buffers per tile (PSRAM contention → display drift and a
  starved UI).
- **Rendering cost:** a new screen takes 300–800 ms to its first frame; each list row ~40 ms to build + draw.
  Keep lists to about 20 rows (fuel: `MAX_ROWS`).
- **Full-screen redraws from other tasks:** don't call `lv_refr_now()` for those; it can deadlock with the
  display adapter.
- **Touch input:** goes through `ui/touch_input.c` (buffered, timestamped). Don't register touch via
  `esp_lv_adapter_register_touch`. `ui_screen_load()` arms a 350 ms tap guard: presses *captured* in that
  window are ignored until release, so a hurried double tap doesn't hit the next screen.
- **Event task:** Wi-Fi listeners run in `sys_evt` (4 KB). Don't start servers or do heavy work there; spawn a task.
- **Symbol clashes:** IDF owns names like `rtc_init`, so prefix our modules (`ext_rtc_*`).
- **Bootloader/partitions:** changing `partitions.csv` or bootloader options requires USB `idf.py flash`;
  OTA only replaces the app.

## Code conventions

- C99, 4-space indent, braces on their own line for functions, K&R otherwise. File-static state uses an `s_` prefix.
  Comments explain *why*, sparingly. Match the surrounding code.
- **UI:**
  - **Text and layout:** English UI text. Spanish number style only for fuel prices (`1,679 €`). Layouts use
    absolute positions plus flex, designed for exactly 1024×600.
  - **Palette and fonts:** use the `UI_COLOR_*` palette and `ui_font_*` sizes from `ui/ui.h`.
  - **Shared helpers:** `ui_header_create`, `ui_card_create`, `ui_label_create`, `ui_button_create`, `ui_toast`, `ui_list_*`.
  - **Style:** dark, flat cards (surface `#111827`, 1 px edge, radius 16–24), tinted accent badges, large touch
    targets (≥ 48 px). The user asked for a modern look (not "2007s").
- Every non-default Kconfig goes in `sdkconfig.defaults` with a comment saying why.
- Before finishing: zero warnings from `main/`, no `TEMP-AUTOTEST` left, the clean build flashed, and the docs
  (README source map / features, `ESP32-info.md`) updated with anything new.

## User preferences

- Writes in English (sometimes brief). Lives in Spain. Drives a Dacia. Uses a phone hotspot.
- Wants things that actually work on the device. Verify on hardware and say what was and wasn't verified.
- Likes polish (Dacia splash, modern home), as long as it doesn't cost much storage or performance.
- Doesn't like how QR codes look on the home cards: use drawn icons there; keep QRs inside the app screens.
  (The Fuel card QR to the nearest station was an explicit earlier request.)
- Sends messages mid-task; answer them as you go.

## Status (2026-10-08) and ideas

Done (v1.4.0, also): MET Norway backup forecast when Open-Meteo fails (then MET for 1 h; see ESP32-info §8).
Done (v1.4.0, also): crescent moon icon (was an arc that looked like a spinner); tear mode `DOUBLE_DIRECT`;
offline/rate-limit resilience: last forecast cached in NVS (`wx_cache`), weather retry back-off 1→15 min,
`location_get()` falls back to chosen city / IP location so Radar, rain map, Fuel, Traffic, Alerts work without
Open-Meteo. **Open-Meteo's free daily limit is per public IP, and the phone hotspot's Telefónica IP is shared
(CGNAT): HTTP 429 "Daily API request limit exceeded" happens even with our own low usage.**
Done (v1.4.0): "Where I parked" (home card with a "P" sign icon + screen with the QR; demo spot + `TODO(GPS)` in services/parking.c:
the GPS code must call `parking_save(lat, lon)` when the car stops / ignition off).
Done (v1.3.2): display glitch fix (single frame buffer, change-only redraws, Wi-Fi colour hysteresis).
Done (v1.3.1): combined Alerts popup (AEMET + DGT), Fuel card QR to the nearest station (background
fuel refresh every 30 min; short `google.com/maps/dir//lat,lon` URL so a 118 px QR still scans).
Done (v1.3.0):
- Traffic (DGT), home redesign (alerts panel + live rain map, 3×2 grid), single network worker,
  location service, tap guard
Earlier (v1.2.0):
- Wi-Fi setup and reconnect logic, boot splash, modern home
- weather (+ 2 h rain), radar (past radar + forecast rain/clouds), fuel (Spain) with QR navigation
- night mode, wireless updates with rollback, RTC clock, AEMET/MeteoAlarm warnings
- Settings screen, driving-friendly navigation (big back, swipe back, status bar = home)
- tooling: devcap, screenshots, virtual finger + `tools/selftest.sh`, ota.sh, logo and zone generators

Known limitations:
- The Radar app loads once per open (~30 s) and doesn't auto-refresh. The home rain map does refresh every
  5 min (verified on device: updates 305 s apart; RainViewer itself publishes every 10 min).
- `lv_image_src_get_type: image src … invalid magic` in the log whenever a screen with a canvas/QR is deleted
  is an **upstream LVGL 9.6 bug** (`lv_canvas_destructor` passes `&canvas->draw_buf` to
  `lv_image_cache_drop`). Harmless log noise, not a use-after-free; don't chase it.
- The status-bar time uses the forecast location's UTC offset.
- Swipe/tap reliability is verified by the scripted self-test (virtual finger). Real-finger feel still needs
  the user's feedback.
- Warnings are Spain-only (feed and zone table); outside Spain no zone matches.
- No GPS yet: Traffic/Fuel/rain map distances are from the IP or chosen-city location (shown on the
  Traffic screen). When the GPS arrives, implement it behind `location_get()`.
- Internal RAM dips to ~14 KB for a moment ~10 s after boot (Wi-Fi + SNTP + mDNS + first TLS together).
  It's harmless now, because nothing allocates task stacks at runtime.
- No GPS, so location is by IP or a chosen city.

Ideas the user may ask for:
- fuel stations as dots on the radar map
- a GPS module (I2C header; not the RS485 pins)
- OBD-II over the CAN pins
- GPS module (user plans one; I2C u-blox recommended): location_get(), "where did I park", speed,
  speed cameras, trip computer, rain ahead
- overlanding "Explore" app: OpenStreetMap Overpass POIs (water, campsites, huts, viewpoints...),
  offline cache (SPIFFS/microSD), breadcrumb/GPX
- "My car" reminders (ITV, insurance, oil), parking timer, air quality/pollen
- ignition sensing on an isolated DI (screen off when parked)
- Wi-Fi geolocation (beaconDB) for a moving location without GPS
- a Dacia car silhouette on the splash (offered, not done)
