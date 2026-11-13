# CANGO1 log

Little-endian. One classical CAN bus, so records have no channel field.

## Header, 64 bytes

| offset | field |
| --- | --- |
| 0 | magic `CG01` |
| 4 | version `u16` = 1 |
| 6 | header length `u16` = 64 |
| 8 | bitrate `u32` |
| 12 | listen-only `u8` |
| 13 | profile `u8`: 0 dealer, 1 startup, 2 both, 3 manual |
| 16 | project id, 16 bytes, zero padded |
| 32 | unix time `u64`, 0 if the clock was never set |
| 40 | start timestamp `u64` |
| 48 | reserved |

## Records

`type u8`, `pad u8`, `len u16`, then `len` payload bytes.

| type | name | payload |
| --- | --- | --- |
| 1 | frame | 20-byte frame |
| 2 | gap | `missed u32` |
| 3 | marker | `code u8`, `t_us u32` |
| 4 | user | `t_us u32` |
| 5 | error | `t_us u32`, `tec u8`, `rec u8`, `state u8` |

Marker codes: startup begin 1, startup end 2, dealer begin 3, dealer end 4, pre-trigger empty 5, context truncated 6.

Dealer capture stores the raw ISO-TP exchange and the surrounding traffic, including a context ring flushed when the session opens. It does not decode services or build a replay. `tools/cango_convert.py` prints the records as text.
