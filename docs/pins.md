# T-CAN485 pins

Classical CAN only. The ESP32 has one TWAI controller. Firmware does not drive a second channel, the RS485 port, or a gateway.

| function | GPIO | note |
| --- | --- | --- |
| CAN TX | 27 | to SN65HVD231 |
| CAN RX | 26 | from SN65HVD231 |
| CAN speed / RS | 23 | driven low: high-speed slope |
| CAN power | 16 | driven high: ME2107 boost on, matching the official CAN example |
| SD MISO | 2 | SPI, 20 MHz bring-up |
| SD MOSI | 15 | |
| SD SCLK | 14 | |
| SD CS | 13 | |
| WS2812 | 4 | one LED |
| BOOT | 0 | active low, pull-up. Strapping pin only while reset is held |
| RS485 TX | 22 | unused in v1 |
| RS485 RX | 21 | unused in v1 |

The README and the schematic disagree on the MAX13487 enable pins. v1 leaves that port alone. Confirm the enable wiring on the schematic before using it.

Free header pins: 25, 32, 33, 5, 12, 34, 35, 18. GPIO 34 and 35 are input only.

Onboard 120 Ω termination (RY2) may be fitted. A node in the middle of a bus must not be terminated. Treat a stuffed RY2 as something to remove or switch off when the board is not at the end of the harness.

Listen-only does not ACK. Switching to normal mode, for an armed injection, reinstalls the TWAI driver. The hardware acceptance filter is open. View filters are software.
