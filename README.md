# ADSB Radar

ADSB Radar is ESP-IDF firmware for the Waveshare
ESP32-P4-WIFI6-Touch-LCD-XC with the 4-inch 720x720 round touch display. It is a
standalone aircraft radar display: the ESP32 joins WiFi, fetches live ADS-B
traffic, plots aircraft around a configurable centre point, and draws a
radar-style screen with range rings, heading information, altitude colours,
airport markers, country outlines, weather icons, and optional runway geometry.

Once the device has been configured it does not need a Raspberry Pi or a
browser left open. WiFi details, radar location, data source, display styling,
notifications, ranges, API keys, and hardware-control settings are saved on the
ESP32. The radar is operated from the touch screen, the optional physical
controls, or the built-in browser admin page.

AI assistance has been used during development of this project, mainly for code
generation, refactoring, documentation, and debugging. The firmware has still
been built and tested on the target ESP32-P4 hardware as the project has
evolved.

## Screenshots

| Live airport view | Wide-area traffic view |
| --- | --- |
| ![ADSB Radar centred on London Heathrow with nearby aircraft, airport labels, range rings, and the live sweep line](assets/Radar%20Images/egll.png) | ![ADSB Radar at 250 miles showing dense regional aircraft traffic, country outlines, altitude colours, and radar labels](assets/Radar%20Images/250miles.png) |
| Centred on London Heathrow, showing close-range aircraft, airport context, range rings, heading arrows, and the sweep line. | A 250-mile regional view with country outlines, altitude-coloured aircraft, dense traffic, and range labels. |

| Busy terminal area | Aircraft detail popup |
| --- | --- |
| ![ADSB Radar centred around Los Angeles with clustered aircraft, altitude colour bands, airport markers, and coastline outline](assets/Radar%20Images/lax.png) | ![ADSB Radar aircraft detail popup showing callsign, PlaneSpotters thumbnail, aircraft type, altitude, speed, heading, range, and vertical rate](assets/Radar%20Images/lax-photo.png) |
| Los Angeles terminal traffic with coastline overlay, airport markers, altitude colouring, and aircraft labels. | Touching an aircraft opens a detail popup with aircraft metadata and a PlaneSpotters thumbnail when one is available. |

| Weather overlay | 2026 King's flypast |
| --- | --- |
| ![ADSB Radar showing airport weather icons next to airport labels](assets/Radar%20Images/weather.png) | ![ADSB Radar showing aircraft from the 2026 King's flypast heading towards London](assets/Radar%20Images/king1.png) |
| Airport weather can be drawn beside airport labels using Open-Meteo data. | A wide-area view of the 2026 King's flypast traffic heading towards London. |

| Country boundary overlay | Military aircraft example |
| --- | --- |
| ![ADSB Radar over south-east England with country boundary outlines, airport labels, altitude-coloured aircraft, and range rings](assets/Radar%20Images/eufi.png) | ![ADSB Radar showing a highlighted C-17 aircraft example](assets/Radar%20Images/c17.png) |
| Country outlines can be drawn subtly behind the radar data, giving extra context without hiding aircraft. | Notification and aircraft-colour settings can be used to make aircraft of interest stand out. |

## Browser UI

The browser admin is part of the firmware and is served directly by the ESP32.
These screenshots are from `assets/UI Images`.

| Dashboard | Location |
| --- | --- |
| ![Dashboard page showing memory, GPS, caches, data source and aircraft status](assets/UI%20Images/Dashboard.png) | ![Location page with centre-source cards and OpenStreetMap location picker](assets/UI%20Images/Location.png) |
| The dashboard brings together device state, aircraft status, screenshots, and settings import/export. | The location page supports manual position, GPS, airport search, and named-place search with a map picker. |

| Data Sources | Display |
| --- | --- |
| ![Data Sources page with aircraft source cards and service keys](assets/UI%20Images/Data%20Sources.png) | ![Display settings page showing grouped visual controls](assets/UI%20Images/Display.png) |
| Aircraft feeds, local receiver URLs, and service keys live in one place. | Display settings are grouped by purpose, including sweep, maps, aircraft, labels, colours, line widths, and visibility. |

| Notifications | Ranges |
| --- | --- |
| ![Notifications page with single-line aircraft type rules](assets/UI%20Images/Notifications.png) | ![Ranges page with compact range preset rows](assets/UI%20Images/Ranges.png) |
| Notification rules can colour, label, bold, or focus aircraft matching an exact aircraft type. | Up to ten range presets can be configured with refresh intervals, label limits, and optional history trails. |

| Hardware Control | WiFi |
| --- | --- |
| ![Hardware Control page with SSD1306 module settings and a placeholder ST7789T3 tab](assets/UI%20Images/Hardware.png) | ![WiFi page showing current connection and scanned networks](assets/UI%20Images/Wifi.png) |
| Hardware Control covers the SSD1306 status display, physical buttons, rotary encoder pins, and action mapping. | WiFi setup can scan networks, save credentials, and report the current connection state. |

| Colour groups | Radar configuration |
| --- | --- |
| ![Display colour settings grouped into tabs](assets/UI%20Images/Colours.png) | ![Radar configuration controls in the browser UI](assets/UI%20Images/Radar%20Config.png) |
| Colour, width, visibility, and label controls are grouped so busy pages stay manageable. | The radar configuration controls expose the low-level display options without needing a rebuild. |

The project is configured for the 4-inch 720x720 display variant:

- `CONFIG_BSP_LCD_TYPE_720_720_4_INCH=y`
- ESP32-P4 target
- Waveshare ESP32-P4 WiFi 6 Touch LCD XC BSP
- LVGL 9 user interface

Waveshare hardware documentation:

- [ESP32-P4-WIFI6-Touch-LCD-XC product docs](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-XC)
- [Waveshare ESP-IDF setup guide](https://docs.waveshare.com/ESP32-P4-WIFI6-Touch-LCD-XC/Development-Environment-Setup-IDF)

## Features

- Full-screen radar drawn for the round 4-inch 720x720 LCD.
- Live aircraft data from Airplanes.live, ADSB.lol, ADSB.fi, or a local
  `aircraft.json` feed such as readsb or dump1090.
- Touch-screen range control with up to ten configurable ranges, refresh
  intervals, label limits, and optional per-range aircraft history trails.
- Aircraft symbols coloured by altitude, with optional emergency squawk
  highlighting.
- Callsign and flight-level/speed labels, including configurable aircraft label
  font and size.
- ATC-style aircraft target symbols with configurable heading indicators:
  none, line, or arrow.
- Optional climb/descent triangles beside the flight level.
- Aircraft filtering from the radar DATA menu: all aircraft, military aircraft,
  or interesting aircraft.
- DATA menu toggles for aircraft headings, airports, country boundaries,
  runways, and aircraft on the ground.
- Aircraft detail popup from touch selection, with optional PlaneSpotters photo
  thumbnails and optional route information where available.
- Notification rules for aircraft type matches, including per-rule colour,
  enabled state, bold labels, optional "dim other aircraft" focus mode, and
  radar banner text.
- Airport markers generated from `data/airports.csv`, with airport search for
  setting the radar centre.
- Optional airport weather icons from Open-Meteo, including configurable icon
  size, colour, and refresh interval.
- Optional country boundary overlay generated from `data/world.geojson`.
- Optional runway overlay when the radar is centred on an airport, using
  AirportDB with cached, rate-limited API calls.
- Captive portal for first-time WiFi setup and a radar WiFi menu for IP address,
  changing WiFi, clearing NVS, and rebooting the device.
- Browser-based Bootstrap admin page with Location, Data Sources, Display,
  Hardware Control, Notifications, Ranges, WiFi, and Dashboard sections.
- Browser admin supports light and dark themes, stored per browser.
- Display controls for colours, line widths, visibility, label fonts and sizes,
  tick lengths, radial spacing, sweep step, sweep draw interval, and sweep trail
  behaviour.
- Dashboard pages for memory, heap low-water marks, caches, WiFi, GPS, data
  source, the full aircraft list, current LVGL screenshot download, and settings
  import/export.
- Aircraft table in the browser uses DataTables search and grouping, and marks
  aircraft currently shown with labels on the radar.
- Optional USB GPS support for the radar centre, including GPS status and a
  button to copy the current GPS fix into the manual position fields.
- Optional SSD1306 128x64 I2C display for local status such as WiFi/IP address
  and live background activity such as aircraft fetches, weather refreshes,
  photo downloads, route lookups, and runway fetches.
- Optional physical confirm/back buttons and rotary encoder for range changes
  and a small device-side menu. GPIO pins and the SSD1306 I2C address are
  configurable from the browser.
- Hardware Control page includes a placeholder tab for future ST7789T3 module
  options.

## Required Hardware

Required:

- Waveshare ESP32-P4-WIFI6-Touch-LCD-XC, 4-inch 720x720 version.
- USB cable for flashing, power, and serial monitor.
- A WiFi network with internet access.
- A development machine running Linux, macOS, or Windows.

Optional:

- USB GPS receiver connected to the board USB-A port.
- SSD1306 128x64 I2C display at address `0x3c` for status output.
- Physical controls: confirm button on GPIO 30, back button on GPIO 46, rotary
  encoder CLK on GPIO 47, DATA on GPIO 52, and encoder push button on GPIO 48.
- AirportDB API token if you want runway drawing. AirportDB requires
  registration at [airportdb.io](https://airportdb.io) and the free API limit is
  5000 calls per month.
- OpenWeather API key if you want to search for the radar centre by place name.
- A local ADS-B receiver if you prefer to use a local `aircraft.json` feed
  instead of an internet aircraft API.

This firmware is not configured for the 3.4-inch 800x800 variant. That display
would need a different BSP LCD Kconfig selection and UI layout review.

## Software Required

Install:

- ESP-IDF. This project has been built with ESP-IDF `v5.5.2`.
- Python 3.
- Git.
- CMake and Ninja. These are normally installed by the ESP-IDF tools installer.
- VS Code with the Espressif IDF extension, optional but useful.

The ESP-IDF extension is enough if it is configured to use a working ESP-IDF
installation and can run `idf.py` tasks. The command line examples below are the
same operations the extension performs.

## Project Layout

```text
.
|-- CMakeLists.txt
|-- data/
|   |-- airports.csv
|   `-- world.geojson
|-- assets/
|   |-- Radar Images/
|   `-- UI Images/
|-- main/
|   |-- app/
|   |-- aircraft/
|   |-- airports/
|   |-- core/
|   |-- gps/
|   |-- net/
|   |-- settings/
|   |-- ui/
|   `-- wifi/
|-- tools/
|   |-- generate_airports.py
|   `-- generate_boundaries.py
|-- components/
|-- main/idf_component.yml
|-- dependencies.lock
|-- partitions.csv
|-- sdkconfig
`-- sdkconfig.defaults
```

`data/airports.csv` and `data/world.geojson` are build inputs. The generated C++
files are written into the build directory and should not be committed.

`assets/Radar Images/` and `assets/UI Images/` are documentation images used by
this README. They are not compiled into the firmware.

`managed_components/` is downloaded by ESP-IDF Component Manager and is ignored
by Git.

## Clone And Open

```bash
git clone <your-repo-url> radar
cd radar
```

If you are using VS Code, open this folder and select the ESP-IDF extension
environment for ESP-IDF `v5.5.2` or another compatible ESP-IDF 5.x install.

## Configure ESP-IDF

From a shell:

```bash
. ~/esp/esp-idf-v5.5.2/export.sh
```

Adjust the path to match your ESP-IDF installation. If ESP-IDF is installed
somewhere else, use that full path instead.

Set the target:

```bash
idf.py set-target esp32p4
```

The project already includes `sdkconfig` and `sdkconfig.defaults`. If you need to
regenerate local config from defaults:

```bash
idf.py fullclean
idf.py set-target esp32p4
idf.py reconfigure
```

## Build

```bash
. ~/esp/esp-idf-v5.5.2/export.sh
idf.py build
```

During the build:

- ESP-IDF downloads managed dependencies into `managed_components/`.
- `tools/generate_airports.py` converts `data/airports.csv` into generated C++.
- `tools/generate_boundaries.py` converts `data/world.geojson` into generated C++.
- The final firmware is written to `build/radar_console.bin`.

The build output should end with something like:

```text
Project build complete.
```

## Flash

Connect the board to the development machine using the USB programming port.

Find the serial port:

```bash
ls /dev/ttyACM* /dev/ttyUSB*
```

On this board the USB serial device may appear as `QinHeng Electronics USB
Single Serial`, usually mapped to something like `/dev/ttyACM0` or `/dev/ttyUSB0`.

Flash and monitor:

```bash
. ~/esp/esp-idf-v5.5.2/export.sh
idf.py -p /dev/ttyACM0 flash monitor
```

Replace `/dev/ttyACM0` with the port on your machine.

If flashing cannot connect, put the ESP32-P4 into download mode:

1. Hold `BOOT`.
2. Press and release `RESET`.
3. Release `BOOT` after the flash command starts connecting.

Exit the serial monitor with `Ctrl+]`.

## First Boot And WiFi Setup

On first boot, or when no saved WiFi credentials exist, the device starts a
captive portal.

1. On a phone or computer, connect to the WiFi network shown on the radar screen.
   It is normally named `RadarSetup-XXXX`.
2. Open `http://192.168.4.1`.
3. Select your WiFi network and enter its password.
4. Save. The ESP32 stores the credentials and reconnects automatically.

After the device has joined your WiFi network, the radar screen shows the current
status on the WiFi menu. Use the WiFi button on the radar to view the IP address,
restart captive portal mode, or reboot the ESP32.

## Browser Admin

When the radar is connected to WiFi, open the device IP address in a browser.
The admin page is where most of the setup work happens:

- Dashboard shows the health of the ESP32: memory, heap low-water marks, cache
  use, WiFi, GPS, data source state, aircraft fetch state, a searchable aircraft
  table, a downloadable LVGL screenshot, and settings import/export.
- Location sets the radar centre. You can type coordinates, use a USB GPS fix,
  search the airport database, or search for a place name and pick the position
  from the map.
- Data Sources chooses the aircraft feed. The app can use Airplanes.live,
  ADSB.lol, ADSB.fi, or a local readsb/dump1090-style `aircraft.json` URL. This
  page also stores the AirportDB and OpenWeather keys.
- Display controls the look of the radar: layers, country outlines, runways,
  airport weather, heading indicators, ground aircraft, sweep timing, fading
  trail, colours, line widths, label fonts, tick spacing, and altitude bands.
- Hardware Control currently has an SSD1306 Module tab for the status OLED,
  physical button GPIOs, rotary encoder GPIOs, and action mapping. A blank
  ST7789T3 tab is present ready for the next hardware-control implementation.
- Notifications configures up to ten exact aircraft-type rules. A match can
  colour the aircraft, keep its label visible, use bold text, show radar banner
  text, and optionally dim every other aircraft.
- Ranges configures up to ten selectable range presets. Each preset has its own
  distance, aircraft refresh interval, label limit, and optional ten-point
  history trail.
- WiFi shows the current connection, scans nearby networks with a normal signal
  strength indicator, and stores WiFi credentials.

Settings are stored in NVS on the ESP32 and are applied to the radar after they
are saved.

## Data Sources

The built-in internet aircraft feeds use the configured radar centre and range.
Ranges are entered in statute miles on the device, then converted where the
upstream API expects nautical miles.

Airplanes.live:

```text
https://api.airplanes.live/v2/point/{lat}/{lon}/{range_nm}
```

ADSB.lol:

```text
https://api.adsb.lol/v2/point/{lat}/{lon}/{range_nm}
```

ADSB.fi:

```text
https://opendata.adsb.fi/api/v3/lat/{lat}/lon/{lon}/dist/{range_nm}
```

ADSB.fi open data is documented at <https://github.com/adsbfi/opendata>.
Their public endpoint is for personal, non-commercial use and is rate limited,
so keep the range refresh intervals sensible.

Local receiver mode expects a readsb/dump1090-style JSON document, for example:

```text
http://192.168.1.28:8080/data/aircraft.json
```

Aircraft photos:

```text
https://api.planespotters.net/pub/photos/hex/{icao_hex}
```

The app adds browser-like headers and serialises HTTPS operations so aircraft
data and photo downloads do not overlap at the TLS layer.

Aircraft routes:

```text
https://api.adsbdb.com/v0/callsign/{flight}
```

Route lookups are optional and can be disabled in the Display settings. The
popup can show either a short route or a longer route description.

Airport runways:

```text
https://airportdb.io/api/v1/airport/{ICAO}?apiToken={apiToken}
```

Runway drawing is optional. It is only enabled when the radar is centred on an
airport and an AirportDB API token has been saved in settings.
[AirportDB](https://airportdb.io) requires registration and the free API limit is
5000 calls per month. The firmware caches and rate-limits AirportDB requests to
avoid wasting that quota.

Airport weather:

```text
https://api.open-meteo.com/v1/forecast
```

The app batches visible airport coordinates into one Open-Meteo request where
possible. Weather refresh defaults to 15 minutes and is configurable in the
Display settings. This does not require an API key.

Location search:

```text
http://api.openweathermap.org/geo/1.0/direct
```

OpenWeather geocoding is used only for named-place search on the Location page
and requires an OpenWeather API key.

## GPS

Optional USB GPS support is built in.

Connect a USB GPS receiver to the board USB-A port. In settings, choose GPS as
the radar centre source. The settings page reports GPS status, including whether
the receiver is connected, whether NMEA data is being received, and whether a fix
is available.

You can also copy the current GPS position into the manual radar centre fields.

## Device Controls

The round display has touch controls around the radar edge:

- Range button: tap the left side to step down, the right side to step up, or
  the middle to open the range list.
- DATA button: choose the aircraft filter and toggle map/aircraft layers.
- WiFi button: show the IP address, start WiFi setup, clear NVS, or reboot.

If the optional physical controls are fitted, the rotary encoder can change the
range or drive the small device menu. The confirm, back, and encoder push
buttons can be mapped to common actions in the Hardware Control page. The GPIO
pins are configurable, so the defaults do not have to match your final wiring.

The optional SSD1306 display shows compact status information: WiFi state, IP
address, current background activity, aircraft data status, range, aircraft
count, and GPS state. It is deliberately simple, but useful when the round
display is showing the radar and you still want to know what the ESP32 is doing.

## Useful Commands

Clean build artifacts:

```bash
idf.py fullclean
```

Reconfigure after changing Kconfig or component files:

```bash
idf.py reconfigure
```

Open menuconfig:

```bash
idf.py menuconfig
```

Build, flash, and monitor in one command:

```bash
idf.py -p /dev/ttyACM0 build flash monitor
```

## Git Notes

Commit these files:

- Source under `main/`
- Data under `data/`
- Generators under `tools/`
- `components/bsp_extra/`
- `components/waveshare__esp32_p4_wifi6_touch_lcd_xc/`
- `main/idf_component.yml`
- `dependencies.lock`
- `sdkconfig`
- `sdkconfig.defaults`
- `partitions.csv`

Do not commit:

- `build/`
- `managed_components/`
- editor swap files
- local SDK config backups

The `.gitignore` file is set up for this.

## Troubleshooting

No serial port:

- Check the USB cable supports data.
- Try another USB port.
- On Linux, check `dmesg` after plugging in the board.
- Add your user to the serial group if required, for example `dialout` on Debian
  or Raspberry Pi OS.

Build fails after moving files or changing data:

```bash
idf.py fullclean
idf.py build
```

WiFi setup portal does not appear:

- Reboot the ESP32.
- Use the radar WiFi menu to start captive portal mode.
- Connect directly to the displayed `RadarSetup-XXXX` network and open
  `http://192.168.4.1`.

Aircraft data is stale:

- Check that the ESP32 has joined WiFi.
- Check the configured radar centre and range.
- Confirm the WiFi network has internet access and allows HTTPS.
- If you use a local receiver source, confirm the configured
  `aircraft.json` URL is reachable from the ESP32 network.
- Check the Dashboard aircraft table to see whether data is being received but
  filtered or outside the active range.

Runways are not displayed:

- Confirm the radar centre source is an airport.
- Confirm an AirportDB API token is saved in settings. Register at
  [airportdb.io](https://airportdb.io) if you do not have one.
- Confirm runway drawing is enabled in settings.

Sweep animation is slow:

- Disable optional aircraft history trails for the active range.
- Reduce the number of visible labels for busy ranges.
- Increase the sweep draw interval or reduce the sweep trail count in Display
  settings.
- Check the Dashboard memory panel for low internal/DMA heap.
