# CANGO session

WiFi and BLE carry the same JSON control messages and the same live batches. The BOOT button selects the transport. Neither client can change that mode.

JSON integers are decimal. The page and the APK display CAN ids in hex and convert before sending. Text replies end with a newline. BLE control writes are reassembled until that newline.

## Live batch

Binary, little-endian:

| offset | field |
| --- | --- |
| 0 | `'C' 'G'` |
| 2 | version `1` |
| 3 | frame count |
| 4 | sequence `u16` |
| 6 | `count` packed 20-byte frames |

A frame is `t_us u32`, `id u32`, `flags u8`, `len u8`, `data[8]`, `reserved u16`. Flags: `EXT 0x01`, `RTR 0x02`, `TX 0x04`.

## Link

WebSocket path `/ws`. Text frames are JSON. Binary frames are batches. `GET /logs/<kind>/<file>` downloads a log. Kinds are `startup`, `dealer`, and `manual`. The web files live on the card at `/www`.

The page keeps one view filter per tab and does not send `set_filter`. The firmware has a single WiFi filter, so a device-wide filter would starve every tab. BLE does send `set_filter`, because that link cannot carry a busy bus. Capture files ignore both filters.

## Operations

`hello` returns version, mode, and AP name.

`status` and `capture_status` return `bus_status`: state, tec, rec, rx, missed, dropped, sd, capture (`idle`, `running`, `complete`), mode, bitrate, listen_only, armed, profile, project, ap, sta. The device also pushes this about once a second.

`get_projects`, `put_project`, `delete_project`, `apply_project`. `put_project` carries the project fields beside `op`. `id` `generic` is rejected. Apply loads Generic or `zero-xb` from firmware when the card has no file for that id. A named file on the card overrides the built-in. An unknown id with no file returns `missing`. It does not reopen a capture that is already running. `zero-xb` is listen-only, dealer capture, 500 kbit/s as the first listen, and an empty allow list.

`set_capture` takes `profile` (`dealer`, `startup`, `both`) and optional seconds: `startup_max_s`, `startup_quiet_s`, `dealer_quiet_s`, `dealer_context_s`, `complete_on_user_events`. The value is stored on the device. A named project file is updated. Generic is not rewritten.

`set_filter` takes `active`, decimal `id` and `mask`, `extended` (`any`, `std`, `ext`), `change_only`, and `min_interval_ms`.

`arm_tx` requires `"confirm": true`. The transceiver leaves listen-only for 60 seconds. `disarm` returns to listen-only. `send_frame` needs an armed bus and an id on the project allow list. `data` is a hex string or a JSON array of bytes. An empty allow list rejects every id.

`get_wifi` and `set_wifi` (`slot`, `ssid`, `password`) edit the remembered station list. The access point stays up. Passwords are returned because the page is the owner on the local link.

`set_time` takes decimal `unix_s` and stamps later log headers.

`discover` and `run_action` return `{ok:false, error:not_implemented}`.

`list_logs` returns `{name, kind, bytes}` for files under the three log directories.
