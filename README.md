# CYD GPS

GPS speedometer, odometer and GPX track logger for the Cheap Yellow Display
(ESP32-2432S028, 2-USB variant) with a NEO-8M-compatible GPS module.
Arduino-ESP32 2.0.17 + LVGL 9.2, PlatformIO.

Po polsku (jak wrócić do projektu, ekrany, komendy, dziennik restartów): [powrot_do_projektu.md](powrot_do_projektu.md).

## Hardware

| GPS module | CYD connector CN1 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| TX  | IO27 |
| RX  | IO22 |

Baud rate is detected automatically (115200 tried first). A micro-SD card (FAT32) in the CYD
slot receives the GPX tracks: a point every 5 m while moving (receiver speed >= 3 km/h), nothing
while standing, position jumps (implied speed > 2 x receiver speed + 15 km/h) skipped.

## Screens (swipe left/right)

1. Speedometer (large seven-segment digits) with the direction of travel from the GPS course (arrow + N/NE/E..., from 5 km/h; no compass in the module)
2. Acceleration timer (POMIAR): 0-50/100/200, 100-200 km/h, 100 m, 1/4 mile, road grade
3. Odometer: trip, total, max, average, moving time; long-press "Zeruj trip" resets the trip and starts a new GPX file
4. GPS: position, altitude, satellites, HDOP, UTC, signal strength
5. Wi-Fi: home network / hotspot, web page address, device password, updates, screen rotation (⟳ turns by 90°; also on the web page, USB command `R`)

## Acceleration timer ("POMIAR" screen)

0-50, 0-100, 0-200, 100-200 km/h, 100 m and 1/4 mile from the GPS Doppler speed, with the road
grade of the run (straight line fitted to GPS altitude vs distance over the whole run, anchored by the
altitude averaged while standing; shown from 50 m, about +-1-2 %; records keep the grade they were set
on, since downhill runs are faster). The GPS module is switched
to 10 Hz at every start (`$PCAS02,100` for AT6558 clones, UBX CFG-RATE for u-blox; never saved in the
module), and the measured rate is shown. Stand still for 1 s, then drive off: the clock starts where
the speed left 0 (interpolated between samples) and the run ends when the speed drops by 5 km/h.
Crossing times are interpolated between samples (about +-0.05 s at 10 Hz). Best times are kept in NVS
(not from runs at < 5 Hz or interrupted ones); long-press the "POMIAR" title to clear them. Logic in
`lib/accel_core` (host tests). Use it on a closed track only.

## Wi-Fi and web page

On first boot the CYD opens a hotspot `CYD-GPS-xxxx`; its password is generated on the device
and shown on the Wi-Fi screen. Open `http://192.168.4.1` (user `cyd`, same password) for live
data, GPX download/delete, home Wi-Fi settings and firmware upload. No credentials are stored
in this repository.

Rides ("Przejazdy" on the web page): every GPX file as date/time, distance, moving time, average and
max speed and climb; tap one for the track on an OpenStreetMap map (loaded by the phone; without
internet, e.g. on the CYD hotspot, the track alone with a scale bar) and a speed chart. The phone does
the maths (`src/web/rides.js`, tested by `test/host/test_rides.mjs`). Track points carry the GPS
(Doppler) speed as Garmin's `gpxtpx:speed`, which Strava and Garmin Connect read as well.

QR login: long-press "QR" on the Wi-Fi screen. It shows a one-time link (valid 2 minutes, one use)
and the device password; scanning it opens the page logged in for 30 days (cookie, up to 4
browsers, kept across reboots). "Wyloguj tel." (long press) logs all browsers out. The password
is not shown on the Wi-Fi screen otherwise.

## Updates from GitHub

In the home network the CYD checks `releases/latest/download/version.txt` after start-up and
every 6 hours ("Sprawdz aktualizacje" on the Wi-Fi screen checks at once) and offers newer versions (install by long-pressing the button on the Wi-Fi
screen, or on the web page). A new image must run 15 s with the network up, otherwise the
device rolls back to the previous one.

Publishing a version:

```sh
sh tools/release.sh 1.2.3   # bumps include/fw_version.h, tests, builds, tags, pushes
```

GitHub Actions (`.github/workflows/release.yml`) builds the tag and creates the release.

## Development

```sh
pio run -e cyd2usb -t upload     # flash over USB
pio run -e cyd2usb_sim -t upload # synthetic GPS data (no NVS writes)
sh test/host/run.sh              # host tests for the odometer and GPX logic
```

USB serial commands (115200): `n` toggles the raw NMEA echo, `T` zeroes the whole odometer,
`L` lists the GPX files, `D 20261123-114602.gpx` deletes one (not the file being written), `R` rotates
the screen by 90°, `B` prints the restart log (reset reason of the last 8 starts, also on the web
page), `?` help.
