# DashCar

In-car dashboard firmware for the **Waveshare ESP32-S3-Touch-LCD-5B**: a 5" 1024×600 touch screen
running weather, rain radar, traffic, Spanish fuel-price, GPS, trip computer, speed camera and parking apps over
a phone hotspot.

- **Hardware:** ESP32-S3 (16 MB flash, 8 MB PSRAM), RGB panel, GT911 touch, CH422G IO expander
  → full reference in [`ESP32-info.md`](ESP32-info.md)
- **GPS:** GT-U7 (u-blox 7 class, NMEA 9600 baud), wired to the **CAN terminals** with no extra parts:
  GPS VCC → VOUT, jumper VOUT → CAN_H, GPS TX → CAN_L, GND → GND, GPS RX unconnected, 120 Ω switch OFF
  (why this works: `ESP32-info.md` §3). Don't use the CAN bus for anything else while it's wired like that.
- **Software:** ESP-IDF 5.5.5, LVGL 9.6 (via `espressif/esp_lvgl_adapter`), plain C
- **Agents / contributors:** read [`CLAUDE.md`](CLAUDE.md) first: workflow, rules and gotchas

---

## Features

| Screen | What it does |
|---|---|
| **Boot splash** | Mountain landscape at dawn with a compass whose needle swings and settles on north, then "DashCar" in a military stencil face sprayed on left to right with a light bar, ≥ 2.6 s while Wi-Fi joins in the background. |
| **Wi-Fi setup** | Scan list + SSID/password form on the on-screen keyboard. Saved in NVS after a successful connection. |
| **Home** | Clock; **Alerts** panel next to it (AEMET warnings first, then major traffic nearby; tap: popup with both, traffic rows open the incident + Maps QR, "Open Traffic"); **live rain map** of ~50 km around the car (latest radar frame, your position, incident dots, "No rain expected in the next 2 hours"; tap for the full Radar); app cards with live subtitles (Weather, Radar, Traffic / Fuel, GPS, Trip; scroll the cards up for Explore, Cameras, Parked, then Settings; dots show the position). The **Fuel card shows a QR to the nearest station** (Google Maps directions) with brand, price and distance from where the car is now; the Parked card shows a "P" sign icon, how long ago you parked and, with a GPS fix, how far away the car is. |
| **GPS** | Live speed (huge), heading, latitude/longitude, altitude, satellites and fix accuracy (HDOP). The status bar shows the satellite count next to Wi-Fi (green = fix, amber = searching). |
| **Trip** | Trip computer from the GPS: distance, driving time, average and top speed, stopped time; today, this month, previous trip and a GPS odometer. A trip starts when the car moves and ends after 2 h without moving (or "New trip"). Speeds under 5 km/h count as stopped (GPS noise), and 3 consecutive moving fixes are needed before distance counts. Saved to flash every 2 min and at every stop. |
| **Speed cameras** | DGT list of 690 fixed cameras and 47 average-speed sections (not Catalonia / Basque Country), downloaded weekly and kept in flash, so it works offline. On any screen: a **red banner** ~30 s before a fixed camera ahead (bearing within 30° of your course), an **amber banner** inside an average-speed section with your running average and the distance to the end, then your final average. Screen: alerts on/off and the nearest 20 cameras. |
| **Speed limit** | Limit of the road you're on from OpenStreetMap `maxspeed`: a round sign in the status bar on every screen (in place of the satellite count), big on the GPS screen, "Limit 90" on the GPS card. It turns **red when you're over** (by more than 2 km/h). Matched once a second to the nearest road within 25 m that runs in your direction (per-direction limits respected; a change must be seen twice). Roads around you are downloaded when the GPS has moved > 1 km, at most every 2 min, and kept in flash. Untagged motorways count as 120. Map limits can be outdated: the real signs rule. |
| **Explore** | Overlanding places within 25 km from OpenStreetMap: drinking water/springs, campsites, huts and shelters (not bus stops), viewpoints, picnic sites, toilets. Filter chips, the nearest 20 by distance with compass direction; tap for the coordinates and a Google Maps QR. **Runs only while open** (≈25 KB PSRAM, freed on close, nothing in the background); each area is saved in flash (16 at most) and used offline or when the server is busy. |
| **Following the car** | With a GPS fix every position-based app follows the car on a **2-minute cadence**: distances, sorting and map markers are recomputed locally every 2 min; data is downloaded again only after a real move (weather 5 km, fuel 15 km, rain map 10 km, warnings 25 km, traffic 50 km) and never more than once per 2 min. Without a fix: the chosen city, else the IP location. |
| **Where I parked** | Big QR with Google Maps walking directions back to the car, spot details and age. The GPS saves the spot after a 90 s stop that follows driving; until the first one it shows a hardcoded demo spot. |
| **Traffic (Spain)** | DGT live incidents (accidents, closures, queues, broken-down vehicles, roadworks...) within 25/50/100 km, Major/All filter, severity colours, detail popup with a Maps QR; dots on the radar maps. |
| **Weather** | Current conditions, 15-min "Next 2 hours" rain chart, 24 h strip, 7-day forecast. °C/°F. Location by IP or city search. |
| **Radar** | Dark map with the last 2 h of real radar plus 12 h of forecast rain and clouds on one timeline (play/scrub, Rain/Clouds toggles). |
| **Settings** | Wi-Fi, night mode (Auto/On/Off), units, wireless updates (version, slot, on/off), clock/RTC status, data sources. |
| **Fuel (Spain)** | Nearby stations and prices for Diésel / 95 / 98 / Diésel+ / GLP within 5/10/25 km; cheapest/nearest/average; list by price or distance; station popup with a **QR code** that opens Google Maps navigation on your phone. |

Driving-friendly navigation, on every screen:
- **Back:** the arrow and title in the header are one big back button.
- **Swipe:** swipe right anywhere to go back.
- **Home:** tap the status bar (⌂, top right) to go home.

Also:
- **Night mode:** dims everything about 55% from 20 min after sunset to 20 min before sunrise (from the
  forecast); can also be forced on or off.
- **Clock:** comes from the onboard RTC at boot, before Wi-Fi, and is corrected by network time.
- **Wireless updates:** see Quick start.

Background updates (one at a time, on the network worker): traffic and home rain map every 5 min,
weather and warnings every 10 min, fuel prices every 30 min (for the home Fuel card; the Fuel app
refreshes on demand too). Roughly 5 MB/hour of mobile data
while running.

Wi-Fi behaviour:
- **Wrong password at boot:** back to the setup form.
- **Hotspot not up yet, or out of range:** dashboard, with retries in the background.
- **Link drops while driving:** silent reconnect every 5 s. The status bar (top right) shows the state.

## Quick start

```sh
tools/setup.sh                           # once per machine: ESP-IDF v5.5.5 + toolchains (~/esp/esp-idf)
. ./env.sh                               # ESP-IDF 5.5 env (Python 3.9-3.13 venv)
idf.py build
idf.py -p /dev/ttyACM0 flash monitor     # Ctrl+] to exit the monitor
```

- **Wireless update (normal workflow):** `idf.py build && tools/ota.sh`. The computer must be on the board's
  network (the phone hotspot), and Settings → Wireless updates must be on.
  - It uploads `build/dashcar.bin` to `http://dashcar.local/update`. The board refuses anything that isn't a
    DashCar image.
  - A new image that doesn't run for 20 s healthy is rolled back by the bootloader.
  - USB is still needed for bootloader, partition-table or `sdkconfig` changes that affect the bootloader.
- First build downloads components into `managed_components/` (needs internet).
- After editing `sdkconfig.defaults`: `rm sdkconfig && idf.py build`.
- Restore the original Waveshare demo (keep your own dump; `backup/` is not in git):
  `python -m esptool -p /dev/ttyACM0 read_flash 0 0x1000000 backup/factory_demo_full_16MB.bin` before the first
  flash, and `write_flash 0 backup/factory_demo_full_16MB.bin` to put it back.
- First-time toolchain setup on a new machine: `tools/setup.sh` (details in `ESP32-info.md` §2).

## Architecture

```
app_main (main.c)
 ├─ settings_init()            NVS
 ├─ wifi_mgr_init()            Wi-Fi BEFORE the display (internal RAM, see ESP32-info §4)
 ├─ board_init()               CH422G, RGB panel (2 frame buffers), GT911
 ├─ ext_rtc_init()             clock from the PCF85063A RTC; SNTP writes back on every sync
 ├─ esp_lv_adapter_*           LVGL 9 task, display + touch registration
 ├─ net_worker_init()          ONE task for all network jobs (periodic steps + on-demand jobs)
 ├─ weather_init()             step: chosen city / GPS / IP location → Open-Meteo every 10 min (MET Norway if it fails)
 ├─ alerts_init()              step: MeteoAlarm warnings every 10 min
 ├─ traffic_init()             step: DGT incidents every 5 min (gzip, ~175 KB)
 ├─ rain_now_init()            step: home rain map (base + latest radar) every 5 min
 ├─ ui_fuel_init()             step: fuel prices for the home card (30 min; re-sorted every 2 min on the move)
 ├─ trip_init()                trip computer state from NVS
 ├─ gps_init()                 UART1 on GPIO 16 + 200 ms esp_timer poll: NMEA → fix, clock, parking stops, trip
 ├─ cameras_init()             step: speed cameras from flash; download weekly
 ├─ speedlimit_init()          step: OSM roads around the GPS (moved > 1 km, ≥ 2 min apart); matched by the status bar
 ├─ ota_init()                 HTTP update server + mDNS while on Wi-Fi (if enabled)
 ├─ ui_init() + ui_splash_open()   (ui_init also creates the status bar and night overlay)
 └─ ota_mark_valid_later(20)  confirm this image after 20 s, or the bootloader rolls back
```

Tasks:

| Task | Does |
|---|---|
| LVGL task (adapter) | All UI work: screens, timers, rendering |
| `net_worker` | **All** network work, sequentially: periodic steps (weather, warnings, traffic, rain map) and posted jobs (fuel search, city search, radar loader, OTA server start). Internal 10 KB stack. |
| `touch` | Polls the GT911 every 10 ms into a queue (buffered input) |
| `esp_timer` (system) | Also runs the GPS poll every 200 ms (reads the UART buffer, parses NMEA; no own task) |
| `httpd` (only while the update server runs) | `GET /`, `POST /update` |
| Wi-Fi event loop (`sys_evt`, 4 KB stack) | `wifi_mgr` state changes and scan results, notifies listeners. Keep work light; start heavy services from a helper task (see `ota.c`). |
| No tasks are created at runtime | screens post jobs with `net_worker_post()`; nothing can fail with "out of memory" because internal RAM is fragmented |

Rules:
- Only touch LVGL from the LVGL task, or while holding `ui_lock()`/`ui_unlock()`. The lock is recursive.
- Every screen keeps `static lv_obj_t *s_scr`, cleared in its `LV_EVENT_DELETE` handler. A callback from another
  task takes the lock and then checks `s_scr` (or a generation counter) before touching widgets.
- Navigation: `ui_screen_load(scr)` deletes the previous screen. Its delete handler removes listeners and timers.

### Source map

```
main/
  main.c                boot sequence, heap logging, cJSON → PSRAM hooks
  settings.[ch]         NVS: Wi-Fi creds, location, units, fuel, night mode, OTA on/off, last UTC offset
  lv_mem_psram.c        LVGL heap in PSRAM (CONFIG_LV_USE_CUSTOM_MALLOC)
  board/board.[ch]      pins, panel timing, CH422G, GT911, backlight
  net/wifi_mgr.[ch]     STA connect/retry/scan, listeners, SNTP
  net/http_util.[ch]    http_get() one-shot; http_session_get() keep-alive (tiles); URL encode
  services/weather.[ch] Open-Meteo forecast + 15-min rain + geocoding; MET Norway backup; IP geolocation;
                        last forecast cached in NVS
  services/map_tiles.[ch] Web Mercator math, SPIFFS tile cache (/tiles), PNG (libpng) + JPEG (esp_new_jpeg) decode
  services/net_worker.[ch] the single network task: periodic steps + job queue
  services/location.[ch] "where is the car" (GPS fix, else chosen city, else IP) used by every app
  services/gps.[ch]     GT-U7 NMEA via the CAN transceiver (UART1, GPIO 16): fix, clock, parking stops
  services/trip.[ch]    trip computer (fed by every fix; NVS "trip2")
  services/cameras.[ch] DGT speed cameras: gzip stream parse, SPIFFS copy (/tiles/cameras.bin), weekly refresh
  services/speedlimit.[ch] OpenStreetMap speed limits: Overpass download (line-based JSON parse), road matching
  services/poi.[ch]     Explore data: Overpass download, nearest 400 kept, saved areas in /tiles/poi_*
  services/traffic.[ch] DGT DATEX II incidents: gzip stream, per-situation parsing, labels, severity
  services/rain_now.[ch] home rain map: z9 basemap + latest RainViewer frame, double-buffered
  services/timeutil.[ch] ISO 8601 → UTC, days_from_civil (no timegm in newlib)
  services/parking.[ch] parking spot (NVS) + Maps walking URL; demo spot until the GPS saves one
  services/fuel.[ch]    Ministerio fuel API, province picking, parsing, distances
  services/alerts.[ch]  MeteoAlarm warnings: zone lookup (point in polygon), streamed Atom parsing
  services/warn_zones.[ch] GENERATED zone polygons (tools/gen_zones.py); lookup code is in alerts.c
  services/ext_rtc.[ch] PCF85063A RTC (named ext_rtc_*: IDF already has rtc_init)
  services/ota.[ch]     HTTP update server, image check, mDNS, rollback confirmation
  ui/ui.[ch]            fonts (Tiny TTF), theme, status bar (home button), header (big back + swipe),
                        night overlay, OTA progress overlay, card/list/toast helpers
  ui/ui_home.c          home screen (+ warning pill/popup) and boot splash
  ui/ui_settings.c      settings screen
  ui/ui_wifi.c          Wi-Fi setup screen
  ui/app_weather.c      weather screen + city search
  ui/app_radar.c        radar map: loader task, compositor, timeline
  ui/app_fuel.c         fuel screen, station popup with QR
  ui/app_traffic.c      traffic screen, incident popup with QR
  ui/app_parking.c      "Where I parked" screen (walking-directions QR)
  ui/app_gps.c          GPS screen (speed, heading, position, altitude, satellites)
  ui/app_trip.c         Trip computer screen
  ui/app_cameras.c      Speed cameras screen (alerts toggle, nearest cameras)
  ui/app_explore.c      Explore: overlanding places, list + Maps QR (runs only while open)
  ui/cam_alert.c        speed camera / average-speed section banner on the top layer (1 s check)
  ui/touch_input.[ch]   buffered touch: 10 ms polling task + LVGL replay (no lost taps/swipes while rendering)
  ui/wx_icon.[ch]       weather icons drawn from LVGL primitives (any size)
  ui/app_logo.c         GENERATED by tools/gen_logo.py (A8 bitmaps: home icon, stencil word mark, compass needle)
  ui/splash_bg.jpg      GENERATED by tools/gen_logo.py (splash landscape, ~33 KB, embedded)
  debug/screenshot.[ch] dbg_screenshot(): dump the screen over serial (see tools/devcap.py)
  debug/touch_sim.[ch]  virtual finger: dbg_touch_tap() / dbg_touch_swipe() through the real input queue
  debug/selftest.[ch]   scripted touch stress test (only built by tools/selftest.sh)
  fonts/                Montserrat Medium/SemiBold, Latin subset (OFL)
tools/
  gen_logo.py           regenerate ui/app_logo.c + ui/splash_bg.jpg (original artwork; Black Ops One font in tools/fonts)
  fonts/                Black Ops One (SIL OFL), used only by gen_logo.py
  devcap.py             reset board, stream logs, save DUMP screenshots as PNG (+ QR decode)
  ota.sh                upload build/dashcar.bin over Wi-Fi (default host dashcar.local)
  soak.sh               ~35 min realistic workload test (simulated drive + ghost user + monitor), see CLAUDE.md
  setup.sh              install ESP-IDF v5.5.5 + toolchains (once per machine)
  selftest.sh           build (build-selftest/), flash and run the touch stress test; PASS/FAIL
  gen_zones.py          regenerate services/warn_zones.c from the MeteoAlarm geocodes file
partitions.csv          nvs 24K · otadata · phy · ota_0 4M · ota_1 4M · storage (SPIFFS tile cache) 4M
sdkconfig.defaults      every non-default setting, with comments explaining why
env.sh                  sources ESP-IDF with the right Python venv
dependencies.lock       exact component versions (LVGL 9.6, esp_lvgl_adapter 0.5.3, ...): keep it committed
backup/                 full 16 MB factory flash dump (gitignored)
```

### Adding an app

1. **Screen:** create `main/ui/app_<name>.c` with `void ui_<name>_open(void)` and declare it in `ui/ui.h`. Follow
   `app_fuel.c` as the template: header via `ui_header_create(scr, title, on_back)`, which gives the big back
   button and swipe-back for free; `s_scr` plus a delete handler; network calls as jobs with `net_worker_post()`
   (no runtime tasks); and `render()` under the lock.
2. **Home card:** add an entry to `APPS[]` in `ui/ui_home.c` (title, accent colour, open function). Then add an
   icon case in `fill_badge()` and a subtitle in `update_subtitles()`. The grid shows two rows of three and
   scrolls for more; only the first card of rows 1–2 is snappable. Update the card coordinates in
   `debug/selftest.c` if the order changes.
   - **Position:** use `location_get()`. If the app depends on where the car is, follow it on the
     `LOCATION_FOLLOW_MS` (2 min) cadence: recompute locally, refetch only after a real move.
3. **Build:** add the `.c` file to `main/CMakeLists.txt` (`SRCS`). Add any new IDF component to `PRIV_REQUIRES`
   and any registry component to `main/idf_component.yml`.
4. **Memory:** allocate big buffers in PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`). Watch the
   `dashcar:` heap lines at boot and the internal-RAM rules in `ESP32-info.md` §4.
5. **Check:** verify the layout on the device with `dbg_screenshot()` and `tools/devcap.py` (see `CLAUDE.md`).

## External services (all keyless)

| Service | Used for | Notes |
|---|---|---|
| [Open-Meteo](https://open-meteo.com) | forecast, 15-min rain, geocoding, radar forecast grid | Free, non-commercial |
| [MET Norway](https://api.met.no) | backup forecast when Open-Meteo fails (e.g. HTTP 429) | Free, no key, CC BY 4.0 |
| ipinfo.io (fallback ip-api.com) | approximate location by IP | ip-api is HTTP-only |
| [RainViewer](https://www.rainviewer.com/api.html) | past radar tiles | free tier: zoom ≤ 7, no nowcast, 100 req/min |
| Esri Dark Gray Canvas | radar basemap | attribution shown on the map |
| Ministerio "Precios Carburantes" | Spanish fuel prices | updated every 30 min |
| [MeteoAlarm](https://meteoalarm.org) Atom feed (Spain) | official AEMET warnings | ~330 KB, streamed; zones from MeteoAlarm geocodes (CC BY 4.0) |
| pool.ntp.org / time.google.com | time (SNTP) | written back to the RTC |
| DGT NAP (DATEX II v3.7) | live traffic incidents (Spain) | 5.4 MB XML, 175 KB gzipped; requested with `Accept-Encoding: gzip` |
| DGT NAP "radares" (DATEX II 1.0) | fixed speed cameras + average-speed sections | 2 MB XML, 62 KB gzipped; CC BY; weekly |
| [Overpass API](https://overpass-api.de) (OpenStreetMap), mirror maps.mail.ru | road speed limits (`maxspeed`) | ~160 KB gzipped per area; often busy (504): next mirror on the next try |
| [Nominatim](https://nominatim.org) (OpenStreetMap) | town name for the weather when it follows the GPS | ≤ 1 req/s, identified User-Agent; called at most once per forecast |
| Google Maps URLs | navigation link in the fuel/traffic QRs | `maps/dir/?api=1&destination=lat,lon` |

Details and quirks of each API: `ESP32-info.md` §8.

## Troubleshooting

| Symptom (serial log / screen) | Likely cause → fix |
|---|---|
| Display glitches (lines, shifts) while `http://dashcar.local/diag` shows the board idle (~1 refresh/s, CPUs ~95 % idle) | Power, not software: screen ~550 mA + ESP32/Wi-Fi peaks + GPS on a laptop USB port (500/900 mA) sags the 3.3 V rail. Use a 5 V 2 A charger (battery switch OFF on USB) or, in the car, 12 V on the DC input; a 470 µF capacitor at the GPS VCC/GND helps. If it persists, unplug GPS VCC to confirm. |
| No GPS arrow in the status bar / `gps: No data from the GPS` | GPS not powered or TX not on CAN_L. Check VOUT → GPS VCC and the VOUT → CAN_H jumper. |
| `GPS signal garbled (check wiring)` or a rising `bad` count on `http://dashcar.local/` | CAN_H not tied to VOUT, the 120 Ω switch ON, or a loose GND. |
| Trip adds kilometres while parked | Shouldn't happen any more: speeds < 5 km/h and fixes with HDOP > 5 report 0, and 3 moving fixes in a row are needed. Tap "New trip" to clear a bad trip. |
| Cameras card says "no data yet" | The list downloads on the first Wi-Fi connection (then weekly) and is kept in flash. |
| Amber GPS arrow for minutes | Searching: the antenna needs a view of the sky (windscreen). First fix after a cold start can take a few minutes. |
| `esp_wifi_init ... ESP_ERR_NO_MEM` at boot | Something now allocates internal RAM before Wi-Fi. Keep `wifi_mgr_init()` before `board_init()`. |
| `wifi:mem fail`, `esp-tls: Failed to create socket` | Internal RAM starvation. Move the new buffers to PSRAM; see `ESP32-info.md` §4. |
| `HTTP_HEADER: Buffer length is small` | URL longer than the HTTP TX buffer. Raise `buffer_size_tx` in `http_util.c`. |
| `panel frame buffer request failed (required=3)` | Tear mode mismatch: set it in both `board_init()` and `disp_cfg.tear_avoid_mode`. |
| A box instead of a character | Glyph missing from the font subset (`main/fonts`), or text drawn with built-in `lv_font_montserrat_*` (ASCII only). |
| Map shows "API KEY REQUIRED" or "Zoom Level Not Supported" | The tile provider changed terms. Check the tile pixels, not just HTTP 200. |
| "Out of memory" / "Busy, please try again" opening an app | Internal RAM: never create tasks at runtime (use `net_worker_post`), keep big statics out of internal RAM (`EXT_RAM_BSS_ATTR`). Check `http://dashcar.local/` for live heap numbers. |
| Radar can't allocate its map | PSRAM: the home rain map must be released first (`rain_now_pause/release/resume`, done by the radar loader job). |
| Crash on swipe-back (`tlsf_free ... block already marked as free`) | Navigation ran *inside* the gesture event and deleted the screen LVGL was dispatching to. Navigate via `lv_async_call` (see `on_back_gesture`). |
| Crash in a screen's timer after reopening it (`LoadProhibited` in `refresh`) | Screen opened twice and a shared static timer got deleted by the old instance. Use `ui_screen_own_timer()` and the `s_scr` guard in delete handlers. |
| Taps/swipes ignored right after a screen opens | LVGL doesn't read input while rendering (300–800 ms for a new screen). Input is buffered by `ui/touch_input.c`; keep it that way. |
| Display shifts/flickers briefly | PSRAM bandwidth contention (the RGB panel scans out of PSRAM). Keep tear mode `DOUBLE_DIRECT` (never the partial modes), avoid repeated full-screen recomposes and needless redraws; see `ESP32-info.md` §4/§6. |
| `A stack overflow in task sys_evt` | Heavy work in a Wi-Fi listener. Move it to a helper task. |
| Clock wrong at boot / TLS fails until SNTP | RTC holds garbage. Years outside 2025–2040 are rejected; the next SNTP sync fixes the RTC. |
| `tools/ota.sh`: can't reach dashcar.local | Not on the same network, wireless updates are off, or no mDNS on this PC. Pass the IP from Settings instead. |
| New firmware "didn't stick" after OTA | It crashed in its first 20 s and the bootloader rolled back. Flash over USB to see the crash. |
| `idf.py` complains about a Python 3.14 venv | You sourced `export.sh` directly. Use `. ./env.sh`. |

## Credits and licences

- Montserrat font: SIL Open Font License (`main/fonts/OFL.txt`). Black Ops One (splash word mark, rendered to a
  bitmap): SIL Open Font License (`tools/fonts/OFL-BlackOpsOne.txt`).
- Map data: Esri, HERE, Garmin, © OpenStreetMap contributors (speed limits: OpenStreetMap, ODbL). Radar: RainViewer. Weather: Open-Meteo (CC BY 4.0), MET Norway (CC BY 4.0).
  Fuel prices: Ministerio para la Transición Ecológica (Spain). Warnings: AEMET via MeteoAlarm (CC BY 4.0).
  Speed cameras: DGT (NAP, CC BY). Places (Explore) and speed limits: © OpenStreetMap contributors (ODbL).
- The DashCar emblem, splash landscape and compass are original artwork (`tools/gen_logo.py`).
