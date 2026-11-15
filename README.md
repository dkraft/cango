# CANGO

Universal classical CAN interface for the LilyGO T-CAN485. One firmware session. WiFi (a web page) and BLE (an Android app) are transports. Vehicle behavior lives in projects. A new tab opens the built-in Generic project.

The BOOT button, after the red startup flash, is the only mode switch. Modes are mutually exclusive so a capture gets the radios off and the RAM:

| LED | meaning |
| --- | --- |
| red flash | startup, button ignored |
| purple flash | capture writing, 30 seconds |
| purple solid | capture mode, file closed |
| blue flash | BLE, waiting for a phone |
| blue solid | BLE phone linked. A lost link goes back to flashing |
| green flash | WiFi access point, station not associated |
| green solid | station has an address |

Long press (800 ms, once per hold) cycles capture, BLE, WiFi. The gap between 600 and 800 ms is ignored. The last mode is restored from NVS. A first boot with no key starts in capture.

A mounted SD card is the request to log. Entering capture mode opens one folder and writes every frame for 30 seconds. A short press (release before 600 ms) while the LED is purple closes that file and opens another folder for another 30 seconds. No card means the LED stays solid and nothing is written. The folder is `/logs/<stamp>/` with `log.cango`, `log.json`, and `log.csv`. In WiFi mode the board also speaks GVRET on TCP port 23, so SavvyCAN can attach on that port.

## What this build does not do

Injection is the only transmit path: arm, confirm, an id on the project allow list, then one frame. After 60 seconds the controller returns to listen-only. An empty allow list rejects every id.

There is no second CAN channel, no gateway, and no man-in-the-middle. `discover` and `run_action` return `not_implemented`. Find modules stays visible and disabled. The + control starts a vehicle. Zero XB is a built-in listen-only project with an empty allow list, so the first job is to record the bus. A card file named `zero-xb` overrides that built-in. The log is not a seed/key, immobilizer, odometer, emissions, or chassis-control procedure, and the firmware does not invent one.

## Layout

| path | role |
| --- | --- |
| `common/` | frames, log, filter, projects. Host-tested C |
| `firmware/` | ESP-IDF app: TWAI, SD, NVS, HTTP, NimBLE |
| `web/www/` | static page, copied to the card as `/www` |
| `tools/` | codec, log printer, WiFi simulator, host test |
| `android/` | BLE client. The installable APK is the GitHub release asset |
| `protocol/` | session JSON and CANGO1 bytes |

Factory station networks, seeded once into NVS, are `BPSSI` and `mifi`, both with password `KoalaBear`. Later edits are kept. The access point is `CANGO-XXXX` from the chip id and stays up while station association is tried in that order. Solid green means a station address exists. Logs download over HTTP only.

## Checks on a machine without ESP-IDF

```sh
make -C common test
python3 tools/test_host.py
```

The second command builds the C fixture, compares the Python codec to those bytes, talks to the simulator, and loads the page in headless Firefox. Flashing a board, phone BLE, and an APK build are outside that run. See `docs/build.md` and `docs/pins.md`.
