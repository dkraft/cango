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

A blank card is given `/www`, `/projects`, and `/logs` when it mounts, and again if Files opens an empty root. Copy `web/www` onto the card as `/www` (`index.html`, `app.js`, `style.css`). Projects go in `/projects`. Each capture is `/logs/<stamp>/log.cango`, `log.json`, and `log.csv`. The Files list browses the card, and `GET /sd/<path>` downloads a file. If `index.html` is missing, the device serves a short fallback page.

## Page without the board

```sh
python3 tools/sim_server.py 8765
```

Open `http://127.0.0.1:8765/`. The simulator emits ids `0x100`, `0x200`, and `0x123`. `?selftest=1` runs the page checks and posts the result.

## Android

`android/` is Kotlin and Compose, min SDK 26, version 0.1.13. The version number is centered on the top bar. The Files tab is the card browser. The BLE mark is solid blue while the phone is linked and grey otherwise. From that directory, `./gradlew assembleRelease` writes `app/build/outputs/apk/release/app-release.apk`. If `keystore.properties` is absent, the release build is signed with the debug key. The codec bytes match `common/` and `tools/test_host.py` checks the firmware UUID constants. The installable APK is attached to the GitHub release. Open the menu at the top left and tap Scan while the LED flashes blue. The system Bluetooth page does not open the link. WiFi names and passwords are on that same page.

SavvyCAN connects while the board is in WiFi mode. In SavvyCAN choose Connection, Add New Device Connection, Network Connection, and the board's address. The port is 23. A finished capture folder also contains `log.csv`, which SavvyCAN opens directly. `python3 tools/cango_convert.py file.cango --csv out.csv` writes that same file for an older log.

## Log text

```sh
python3 tools/cango_convert.py path/to/file.cango
```
