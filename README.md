# CYD GPS

GPS speedometer, odometer and GPX track logger for the Cheap Yellow Display
(ESP32-2432S028, 2-USB variant) with a NEO-8M-compatible GPS module.
Arduino-ESP32 2.0.17 + LVGL 9.2, PlatformIO.

## Hardware

| GPS module | CYD connector CN1 |
|---|---|
| VCC | 3V3 |
| GND | GND |
| TX  | IO27 |
| RX  | IO22 |

Baud rate is detected automatically (115200 tried first). A micro-SD card (FAT32) in the CYD
slot receives the GPX tracks.

## Screens (swipe left/right)

1. Speedometer (large seven-segment digits)
2. Odometer: trip, total, max, average, moving time; long-press "Zeruj trip" resets the trip and starts a new GPX file
3. GPS: position, altitude, satellites, HDOP, UTC, signal strength
4. Wi-Fi: home network / hotspot, web page address, device password, updates

## Wi-Fi and web page

On first boot the CYD opens a hotspot `CYD-GPS-xxxx`; its password is generated on the device
and shown on the Wi-Fi screen. Open `http://192.168.4.1` (user `cyd`, same password) for live
data, GPX download/delete, home Wi-Fi settings and firmware upload. No credentials are stored
in this repository.

## Updates from GitHub

In the home network the CYD checks `releases/latest/download/version.txt` after start-up and
every 6 hours and offers newer versions (install by long-pressing the button on the Wi-Fi
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
`L` lists the GPX files, `D 20261123-114602.gpx` deletes one (not the file being written), `?` help.
