# Build

This tree was written for ESP-IDF 5.x. It builds with ESP-IDF 5.4.4 for ESP32 (`idf.py -C firmware set-target esp32` once, then `idf.py -C firmware build`). Host checks are `make -C common test` and `python3 tools/test_host.py`.

## Firmware

```sh
. "$IDF_PATH/export.sh"
idf.py -C firmware set-target esp32
idf.py -C firmware build
idf.py -C firmware -p /dev/ttyUSB0 flash monitor
```

`firmware/sdkconfig.defaults` selects ESP32, 4 MB QIO flash, the custom partition table, NimBLE peripheral, and the HTTP WebSocket server. `menuconfig` may rename a symbol if the IDF minor version differs. There is no OTA partition: nvs, phy_init, and a 3 MB factory app.

Copy `web/www` onto the card as `/www` (`index.html`, `app.js`, `style.css`). Projects go in `/projects`. Logs appear under `/logs/startup`, `/logs/dealer`, and `/logs/manual`. If `index.html` is missing, the device serves a short fallback page.

## Page without the board

```sh
python3 tools/sim_server.py 8765
```

Open `http://127.0.0.1:8765/`. The simulator emits ids `0x100`, `0x200`, and `0x123`. `?selftest=1` runs the page checks and posts the result.

## Android

`android/` is Kotlin and Compose, min SDK 26, version 0.1.0. From that directory, `./gradlew assembleRelease` writes `app/build/outputs/apk/release/app-release.apk`. If `keystore.properties` is absent, the release build is signed with the debug key. The codec bytes match `common/` and `tools/test_host.py` checks the firmware UUID constants. The installable APK is attached to the GitHub release.

## Log text

```sh
python3 tools/cango_convert.py path/to/file.cango
```
