# CLAUDE.md: working on DashCar

ESP-IDF 5.5 + LVGL 9.6 dashboard for a **Waveshare ESP32-S3-Touch-LCD-5B** (1024×600), mounted in the user's car.
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
- The repo is a git repository with little history (one initial commit). Make careful edits.
- Toolchain: always run `. ./env.sh` first (ESP-IDF 5.5.5 with the Python 3.12 venv; the system Python 3.14
  breaks IDF). Use `command grep` in zsh one-liners, because `grep` may be aliased. The venv's `bin/python3`
  links to Homebrew's `python@3.12` (it once pointed into `/tmp` and broke when that was cleared).

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

**Crashes are recorded on the board** (`main/crashlog.c`, a `--wrap=esp_panic_handler`): faulting PCs, task,
uptime and time, kept in RTC memory and saved to NVS at the next boot. `http://dashcar.local/` shows "last
crash"; decode with `xtensa-esp32s3-elf-addr2line -pfiaC -e build/dashcar.elf <PCs>` (same build!).

**Status page `http://dashcar.local/`** shows the last reset reason (power-on / software / crash / watchdog /
BROWNOUT) and a brownout counter (NVS): check it whenever the board "restarted by itself".

**Live diagnostics without resetting: `curl http://dashcar.local/diag`** (3 s window: refreshes/s, kpixels
redrawn/s, CPU % per task, heap). Opening the USB serial port resets the board: use this when something is
happening *now* (e.g. display glitches: if the board is idle while it glitches, it's power, not software).

**Soak test (realistic daily workload, ~35 min): `tools/soak.sh`** (`main/debug/soak.c`, build-soak/). Needs the
hotspot and hands off the screen. A simulated GPS drives real roads (`debug/soak_route.h`: Zaragoza, A-2 past a
fixed camera, stop, Z-40 average-speed section) while a ghost user taps through every app and a monitor
logs heap (per 30 s window), stack headroom, slowest refresh, errors, and checks that the frame on screen matches
a fresh render (persistent "glitch lines"). Prints `soak: ok/FAIL` per check and `RESULT`. Trip and parking use
separate NVS keys in that build; night mode is restored. Run it after anything touching memory, networking or
screens. The serial capture holds `/dev/ttyACM0` for 45 min: stop it before flashing. Don't `pkill -f devcap`
from a shell whose own command line contains that string (it kills itself).

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
- **Generated files:** `ui/app_logo.c` + `ui/splash_bg.jpg` (`tools/gen_logo.py`) and `services/warn_zones.c` (`tools/gen_zones.py`).
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
- **Location:** always use `location_get()` (services/location), never the weather data directly. It prefers
  a GPS fix (`services/gps`, up to 30 min old), then the chosen city, then IP.
- **Following the GPS: `LOCATION_FOLLOW_MS` (2 min) is the cadence** for position-dependent work. Local maths
  (distances, re-sorting, map markers) runs at that pace; a *refetch* needs a real move AND at least that long
  since the last one: weather 5 km, fuel 15 km (`FUEL_REFETCH_KM`, re-sorted locally meanwhile), rain map recentre
  10 km, alerts 25 km, traffic 50 km. The user asked for this explicitly ("avoid being so fast, 2 mins is OK").
- **GPS speed below 5 km/h (`GPS_MIN_KMH`) or with HDOP > 5 is reported as 0**, in `gps.c`, for every user.
  The user saw "1 km/h" and phantom trip kilometres while parked; don't reintroduce raw speeds.
- **`net_worker` has `MAX_STEPS` (10).** A service registered past it is silently dropped except for an
  `E net_worker: too many steps` line at boot (speed limits were, once). Check the boot log after adding one.
- **GPS wiring uses the CAN transceiver** (GPS TX on CAN_L, CAN_H tied to 3.3 V): never start TWAI/CAN
  while it's wired like that (ESP32-info §3).
- **Big static buffers** (arrays of structs in UI code) need `EXT_RAM_BSS_ATTR`, or they silently take
  internal RAM (a 34 KB `static traffic_item_t[200]` once cost 50 KB at boot).
- **The firmware itself lives in PSRAM** (`SPIRAM_XIP_FROM_PSRAM`, needed so flash writes don't stall the
  display): every KB the app grows costs a KB of PSRAM (app ~2.25 MB). 8 MB − app − 2.4 MB frame buffers leaves
  ~3.7 MB for everything; the Radar needs ~2.7 MB of it while open. Watch `psram free (largest …)` in the boot log.
- **PSRAM 80 MHz stays** (not 120): ESP-IDF says octal PSRAM at 120 MHz can crash after a ~20 °C temperature
  change, which a car guarantees.
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
  Keep lists to about 20 rows (fuel: `MAX_ROWS`). Build list rows with `ui_list_row` (big, driving-friendly).
- **Apps that aren't needed in the background run only while open** (Explore): allocate on open, free in the
  delete handler, one net_worker job with a generation counter; no step, no live home-card data.
- **Full-screen redraws from other tasks:** don't call `lv_refr_now()` for those; it can deadlock with the
  display adapter.
- **Touch input:** goes through `ui/touch_input.c` (buffered, timestamped). Don't register touch via
  `esp_lv_adapter_register_touch`. `ui_screen_load()` arms a 350 ms tap guard: presses *captured* in that
  window are ignored until release, so a hurried double tap doesn't hit the next screen.
- **Timer callbacks:** FreeRTOS timers run in `Tmr Svc` (2 KB stack): only set flags or `net_worker_post()`
  there; no flash writes, no logging, no big locals (it crashed the board after every OTA).
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

- Writes in English (sometimes brief). Uses the dashboard in Spain over a phone hotspot.
- Wants things that actually work on the device. Verify on hardware and say what was and wasn't verified.
- Likes polish (animated splash, modern home), as long as it doesn't cost much storage or performance.
- Doesn't like how QR codes look on the home cards: use drawn icons there; keep QRs inside the app screens.
  (The Fuel card QR to the nearest station was an explicit earlier request.)
- Sends messages mid-task; answer them as you go.

## Status (2026-10-14) and ideas

Done (2026-10-14): crash after every Wi-Fi update, found with the new crash recorder: the 20 s "mark new image
valid" ran in FreeRTOS's timer task (2 KB stack) and overflowed it (stack overflow hook → abort) on the first
boot of each OTA image. Now the timer posts the work to net_worker. **Never do flash writes or logging in a
FreeRTOS timer callback** (`Tmr Svc` has a 2 KB stack). Verified: clean restart, no crash past 40 s.
Done (2026-10-14): Wi-Fi updates restarted through a 2 KB task: esp_restart()'s shutdown handlers overflowed it,
so every OTA ended in a panic/watchdog reset (the image still booted). Now 4 KB; verified: reset reason
"software". Reset reason + brownout count on the status page.
Done (2026-10-14): Explore (overlanding POIs, `services/poi`, `ui/app_explore.c`): runs only while open (user
requirement: no live data, no background RAM). List rows everywhere are now big for driving (`ui_list_row`,
`UI_LIST_ROW_H` 124: ~3 visible, scroll) in Explore, Fuel, Traffic, Cameras; `ui_list_add_button` 72 px.
Verified: 388 places around the board on the device, list screenshot; PSRAM back to baseline (±2 KB) after the
open/close cycles. Not yet seen on the device: the bigger rows (installed over Wi-Fi, USB was unplugged) and the
detail popup. Network-worker URL/parse buffers moved to PSRAM statics (EXT_RAM_BSS_ATTR) for stack margin.
Done (2026-10-14): display glitches that "start at some point" were measured with /diag while happening: board
idle (1 refresh/s, CPUs ~96 % idle), so electrical: on laptop USB the screen (~550 mA) + Wi-Fi peaks + GPS sag
the 3.3 V rail. Wi-Fi TX power capped at 13 dBm + modem sleep (wifi_mgr.c). The glitching cleared by itself;
not yet confirmed which of GPS load / USB supply. Advised: 5 V 2 A charger at the desk, 12 V DC input in the car,
470 µF at the GPS. Status line 'Display glitches' in README troubleshooting.
Done (2026-10-14): reliability pass after "glitch lines" and "Not enough memory for the map" reports. Radar base
map stored as 8-bit grey (−550 KB), speed-limit and camera buffers allocated once at boot (no per-download
alloc/shrink → fragmentation), speed-limit roads released while the Radar is open (like the rain map),
Overpass: bbox query + 45 s timeout (servers often 504 / 20-30 s), net_worker MAX_STEPS 10. Soak run 2 (35 min):
no crash/reboot, 362 UI steps 0 failures, Radar 0 memory failures, 95 frame checks 0 stuck rows, trip 43.6/43.8 km,
parking, camera + section OK (route since moved to the A-2). Open: speed limit known only 24 % (Overpass overloaded that day; fix not yet soaked),
internal RAM low point 8.4 KB (JPEG decoder's fixed 10 KB during Radar tile loads + Wi-Fi; under investigation with
per-window minimums), visible transient display drift under heavy PSRAM load (recovers; the Radar's animation is
the heaviest load). The user's original "lines that stay" was not reproduced.
Done (2026-10-14): OpenStreetMap speed limits (`services/speedlimit`): sign in the status bar / GPS screen / GPS
card, red when over. Verified: parser + matcher on a real Overpass response (host test: 1632 roads, motorway 120,
per-direction match), sign rendering on the device (forced value). Not yet verified: the download on the device
(hotspot off) and matching while driving.
Done (2026-10-14): Trip computer (`services/trip`, `ui/app_trip.c`) and Speed cameras (`services/cameras`,
`ui/app_cameras.c`, banner `ui/cam_alert.c`; DGT DATEX II list, 737 entries, SPIFFS copy). Home grid is now 3 full
rows. Verified: parser on the real file (host test, 690 fixed + 47 sections), screenshots of both screens, both
banners and the grid; self-test 10 cycles (only the 2 known no-Wi-Fi failures). Not yet verified: the download
on the device (hotspot off), a real alert/section while driving, trip totals on a drive. `tools/selftest.sh`
now captures 480 s (the test got longer).
Done (2026-10-14): GPS-following cadence (see the rule above): fuel no longer refetches every 1 km (re-sorts
locally, home card = nearest station from the current position), rain-map "you" dot placed from the current
position, Parked card shows "N km away" (GPS only), Parking and Traffic screens re-sort every 2 min. Verified:
build + boot with a GPS fix; the network-side triggers were not exercised (hotspot off during the test).
Done (2026-10-14): splash = mountain landscape (procedural, JPEG) + compass settling on north + stencil
"DashCar" (Black Ops One bitmap) + light bar; the user rejected a piston animation in favour of this "nature"
look. Compass and word mark verified by on-device snapshots; the full composed frame only as a PC mockup.
The home header still uses the small gauge emblem.
Done (v1.4.0, also): MET Norway backup forecast when Open-Meteo fails (then MET for 1 h; see ESP32-info §8).
Done (v1.4.0, also): crescent moon icon (was an arc that looked like a spinner); tear mode `DOUBLE_DIRECT`;
offline/rate-limit resilience: last forecast cached in NVS (`wx_cache`), weather retry back-off 1→15 min,
`location_get()` falls back to chosen city / IP location so Radar, rain map, Fuel, Traffic, Alerts work without
Open-Meteo. **Open-Meteo's free daily limit is per public IP, and the phone hotspot's mobile-operator IP is shared
(CGNAT): HTTP 429 "Daily API request limit exceeded" happens even with our own low usage.**
Done (2026-10-14): GT-U7 GPS fitted, read through the CAN transceiver (no free TTL UART on the board;
ESP32-info §3). `services/gps.c` (UART1 on GPIO 16, esp_timer poll, NMEA RMC/GGA/GSV) feeds `location_get()`, sets
the clock when there's no network, and saves the parking spot after a 90 s stop that follows driving. GPS
status is on `http://dashcar.local/` and in the log every 60 s. Also: GPS app screen (`ui/app_gps.c`), satellite
count in the status bar (`UI_STATUS_W` now 360), home card grid scrolls (third row: Parked; snap points only on
rows 1–2, page dots), weather follows the GPS when no city is chosen (refetch after moving > 5 km, at most every
2 min; town name from Nominatim reverse geocoding). Verified on hardware: clean NMEA (0 checksum errors), a fix
indoors (clock set from GPS, Radar centred on it), screenshots of home/scrolled grid/GPS screen/status bar,
self-test (GPS card + scrolled Parked card pass). Not yet verified: weather following the car (no hotspot
during the test), parking save on a real drive, Nominatim response on the device.
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
- Weather: a chosen city overrides the GPS (Weather > Location > "My location" returns to automatic). The
  `location_get()` name is the forecast place. Without Wi-Fi the self-test fails its first checks (splash waits
  for Wi-Fi; Radar needs a location): run it with the hotspot on.
- Internal RAM dips to ~14 KB for a moment ~10 s after boot (Wi-Fi + SNTP + mDNS + first TLS together).
  It's harmless now, because nothing allocates task stacks at runtime.

Ideas the user may ask for:
- fuel stations as dots on the radar map
- OBD-II over the CAN pins (the GPS occupies them now: move it to RS485 first, ESP32-info §3)
- a camera module (e.g. Grove Vision AI V2 over I2C) reading speed signs to confirm/correct the OSM limit
- with the GPS now fitted: rain ahead, "Park here" button, GPS track/breadcrumb, "My car" reminders using the
  GPS odometer
- Explore v2: region packs on a microSD card (PC script from a Geofabrik extract) for full offline coverage;
  breadcrumb/GPX
- "My car" reminders (ITV, insurance, oil), parking timer, air quality/pollen
- ignition sensing on an isolated DI (screen off when parked)
- Wi-Fi geolocation (beaconDB) for a moving location without GPS
