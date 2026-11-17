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

WebSocket path `/ws`. Text frames are JSON. Binary frames are batches. `GET /logs/<folder>/<file>` downloads a log. `GET /sd/<path>` downloads any file under the card root. Each capture is a folder under `/logs` containing `log.cango`, `log.json`, and `log.csv`. The web files live on the card at `/www`.

The page keeps one view filter per tab and does not send `set_filter`. The firmware has a single WiFi filter, so a device-wide filter would starve every tab. BLE does send `set_filter`, because that link cannot carry a busy bus. Checked ids are notified at most every 200 ms, latest frame per id, for up to 64 ids. Capture files ignore both filters. WiFi mode also accepts one GVRET client on TCP port 23. That socket carries the full bus, not the page filter. A new connection replaces the old one. USB serial stays the log console.

## Operations

`hello` returns version, mode, and AP name.

`status` and `capture_status` return `bus_status`: state, tec, rec, rx, missed, dropped, sd, capture (`idle`, `running`, `complete`), mode, bitrate, listen_only, armed, profile (`30s`), project, ap, sta. `running` is the open 30 second file. `complete` is capture mode after that file has closed. The device also pushes this about once a second.

`get_projects`, `put_project`, `delete_project`, `apply_project`. `put_project` carries the project fields beside `op`. `id` `generic` is rejected. Apply loads Generic or `zero-xb` from firmware when the card has no file for that id. A named file on the card overrides the built-in. An unknown id with no file returns `missing`. It does not reopen a capture that is already running. `zero-xb` is listen-only, 500 kbit/s as the first listen, and an empty allow list. Recording it is the same 30 second window as any other project.

`set_capture` returns ok and does not change the window. Capture is 30 seconds of every frame when a card is mounted. A short BOOT press in capture mode starts a new folder.

`set_filter` takes `active`, `extended` (`any`, `std`, `ext`), `change_only`, and `min_interval_ms`. With an `ids` array it also takes `mode` (`all` hides those ids, `only` passes them) and an inclusive range when both `id_from` and `id_to` are present. At most 32 ids. A longer list returns `error` `full` and leaves the previous list. `active` false clears the filter. With no `ids` field, decimal `id` and `mask` still work. The default mask is `0x7ff` when `mask` is omitted.

WiFi mode answers GVRET on port 23. Two `0xE7` bytes enter binary mode. Commands start with `0xF1`. The device reports one bus at the project bitrate. `0x11` means enabled and listen-only. `0x01` means enabled and armed. Command `0x05` is consumed and does not change the bitrate. A frame from SavvyCAN (`0x00`) uses the same arm and allow-list path as `send_frame`. Unarmed, the wrong bus, or an id outside the allow list is discarded. There is no GVRET error reply. `log.csv` uses SavvyCAN's header `Time Stamp,ID,Extended,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8`. Timestamps are microseconds, the id is eight hex digits, Extended is `true` or `false`, and Bus is `0`.

`arm_tx` requires `"confirm": true`. The transceiver leaves listen-only for 60 seconds. `disarm` returns to listen-only. `send_frame` needs an armed bus and an id on the project allow list. `data` is a hex string or a JSON array of bytes. An empty allow list rejects every id.

`get_wifi` and `set_wifi` (`slot`, `ssid`, `password`) edit the remembered station list. The access point stays up. Passwords are returned because the page is the owner on the local link.

`set_time` takes decimal `unix_s` and stamps later log headers.

`discover` and `run_action` return `{ok:false, error:not_implemented}`.

`list_logs` returns `{name, kind, bytes}`. `kind` is the capture folder name.

`list_dir` takes `path` relative to the card root and `skip`. An empty path is the root. A slash in a JSON string may be sent as `\/`. The reply is `{path, skip, entries, more}`. Each entry is `{name, dir, bytes}`. Directories are listed first. A page is eight names, and the directory keeps at most 96. `..`, a leading slash, and a name that cannot be stored in JSON are `error` `bad`. No card is `error` `sd`. A missing folder is `error` `missing`. A blank card root is given `www`, `projects`, and `logs`, and `projects/generic.json` is written when that file is missing.

`delete_path` removes that file or folder. The card root and `projects/generic.json` return `error` `rejected`. A path inside the capture folder that is still open returns `error` `busy`. A missing path returns `error` `missing`.

`read_file` takes `path` and `offset`. The reply is one JSON line with `n`, `size`, `more`, and `data` as base64 of at most 360 bytes. A directory is `error` `dir`. A missing file is `error` `missing`. No card is `error` `sd`. A path the card cannot store is `error` `bad`.

`format_card` requires `"confirm": true`. Without it the reply is `error` `confirm`. A capture that is still open, or a format already running, is `error` `busy`. No card is `error` `sd`. The first reply is `{ok:true, state:started}`. When the card is empty again the board sends `{ok:true, state:done}` and restores `www`, `projects`, `logs`, and `projects/generic.json`. A failed format is `{ok:false, error:fail, state:done}`.
