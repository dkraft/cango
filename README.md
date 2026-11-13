# CANGO

Universal classical CAN interface for the LilyGO T-CAN485. One firmware session. WiFi (a web page) and BLE (an Android app) are transports. Vehicle behavior lives in projects. A new tab opens the built-in Generic project.

The BOOT button, after the red startup flash, is the only mode switch. Modes are mutually exclusive so a capture gets the radios off and the RAM:

| LED | meaning |
| --- | --- |
| red flash | startup, button ignored |
| purple | capture running |
| white | capture complete |
| blue flash | BLE |
| green flash | WiFi access point, station not associated |
| green solid | station has an address |

Short press (release before 600 ms) writes a user event into the open log and flushes it. From a completed capture it opens `/logs/manual/<stamp>.cango`, writes one event, and closes it. Long press (800 ms, once per hold) cycles capture, BLE, WiFi. The gap between 600 and 800 ms is ignored. The last mode is restored from NVS. A first boot with no key starts a new capture.

The default capture profile is `dealer`. The page or the app can change it to `startup` or `both`. They do not turn the radios on. Dealer capture stays purple until a tester session is seen and then goes quiet, and it keeps a few seconds of prior traffic instead of the whole wait. A bus with no tester stays purple on purpose. The file is raw ISO-TP plus surrounding frames, for a person to study later.

## What this build does not do

Injection is the only transmit path: arm, confirm, an id on the project allow list, then one frame. After 60 seconds the controller returns to listen-only. An empty allow list rejects every id.

There is no second CAN channel, no gateway, and no man-in-the-middle. `discover` and `run_action` return `not_implemented`. The Modules and Actions panels are visible and disabled. The log is not a seed/key, immobilizer, odometer, emissions, or chassis-control procedure, and the firmware does not invent one.

## Layout

| path | role |
| --- | --- |
| `common/` | frames, log, filter, dealer detector, capture, projects. Host-tested C |
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
