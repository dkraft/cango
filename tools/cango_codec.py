"""CANGO batch and CANGO1 log codec. Layout matches common/include."""

import struct

FRAME_BYTES = 20
BATCH_HDR = 6
HEADER_BYTES = 64

PROFILE_NAME = {0: "dealer", 1: "startup", 2: "both", 3: "manual", 4: "window"}
MARKER_NAME = {
    1: "startup_begin",
    2: "startup_end",
    3: "dealer_begin",
    4: "dealer_end",
    5: "pretrigger_empty",
    6: "context_trunc",
}


def pack_frame(t_us, can_id, flags, data):
    raw = bytes(data[:8])
    return struct.pack("<IIBB8sH", t_us & 0xFFFFFFFF, can_id & 0xFFFFFFFF, flags & 0xFF, len(raw), raw.ljust(8, b"\x00"), 0)


def unpack_frame(blob):
    t_us, can_id, flags, length, data, _reserved = struct.unpack("<IIBB8sH", blob[:FRAME_BYTES])
    return {"t_us": t_us, "id": can_id, "flags": flags, "len": length, "data": data[:length]}


def encode_batch(seq, frames):
    body = b"".join(frames)
    count = len(frames)
    if len(body) != count * FRAME_BYTES:
        raise ValueError("frame size")
    return bytes((ord("C"), ord("G"), 1, count)) + struct.pack("<H", seq & 0xFFFF) + body


def decode_batch(blob):
    if len(blob) < BATCH_HDR or blob[0:2] != b"CG" or blob[2] != 1:
        raise ValueError("batch header")
    count = blob[3]
    if len(blob) < BATCH_HDR + count * FRAME_BYTES:
        raise ValueError("batch short")
    seq = struct.unpack_from("<H", blob, 4)[0]
    frames = []
    off = BATCH_HDR
    for _ in range(count):
        frames.append(unpack_frame(blob[off : off + FRAME_BYTES]))
        off += FRAME_BYTES
    return seq, frames


def encode_log(info, records):
    project = info.get("project_id", "").encode("ascii", "replace")[:16]
    header = bytearray(HEADER_BYTES)
    header[0:4] = b"CG01"
    struct.pack_into("<H", header, 4, 1)
    struct.pack_into("<H", header, 6, HEADER_BYTES)
    struct.pack_into("<I", header, 8, info["bitrate"])
    header[12] = 1 if info.get("listen_only", 1) else 0
    header[13] = info["profile"]
    header[16 : 16 + len(project)] = project
    struct.pack_into("<Q", header, 32, info.get("unix_s", 0))
    struct.pack_into("<Q", header, 40, info.get("start_us", 0))
    out = bytes(header)
    for rec in records:
        kind = rec["type"]
        if kind == "frame":
            payload = rec["raw"]
            rtype = 1
        elif kind == "gap":
            payload = struct.pack("<I", rec["missed"])
            rtype = 2
        elif kind == "marker":
            payload = bytes((rec["code"],)) + struct.pack("<I", rec["t_us"])
            rtype = 3
        elif kind == "user":
            payload = struct.pack("<I", rec["t_us"])
            rtype = 4
        elif kind == "error":
            payload = struct.pack("<IBBB", rec["t_us"], rec["tec"], rec["rec"], rec["state"])
            rtype = 5
        else:
            raise ValueError(kind)
        out += struct.pack("<BBH", rtype, 0, len(payload)) + payload
    return out


def parse_log(blob):
    if len(blob) < HEADER_BYTES or blob[0:4] != b"CG01":
        raise ValueError("log header")
    version, header_len = struct.unpack_from("<HH", blob, 4)
    if version != 1 or header_len != HEADER_BYTES:
        raise ValueError("log version")
    bitrate = struct.unpack_from("<I", blob, 8)[0]
    project = blob[16:32].split(b"\x00", 1)[0].decode("ascii", "replace")
    info = {
        "bitrate": bitrate,
        "listen_only": blob[12],
        "profile": blob[13],
        "project_id": project,
        "unix_s": struct.unpack_from("<Q", blob, 32)[0],
        "start_us": struct.unpack_from("<Q", blob, 40)[0],
    }
    records = []
    off = HEADER_BYTES
    while off + 4 <= len(blob):
        rtype, _pad, length = struct.unpack_from("<BBH", blob, off)
        off += 4
        if off + length > len(blob):
            raise ValueError("record short")
        payload = blob[off : off + length]
        off += length
        if rtype == 1 and length == FRAME_BYTES:
            frame = unpack_frame(payload)
            frame["type"] = "frame"
            records.append(frame)
        elif rtype == 2 and length == 4:
            records.append({"type": "gap", "missed": struct.unpack("<I", payload)[0]})
        elif rtype == 3 and length == 5:
            records.append({"type": "marker", "code": payload[0], "t_us": struct.unpack_from("<I", payload, 1)[0]})
        elif rtype == 4 and length == 4:
            records.append({"type": "user", "t_us": struct.unpack("<I", payload)[0]})
        elif rtype == 5 and length == 7:
            t_us, tec, rec, state = struct.unpack("<IBBB", payload)
            records.append({"type": "error", "t_us": t_us, "tec": tec, "rec": rec, "state": state})
        else:
            records.append({"type": "unknown", "rtype": rtype, "raw": payload})
    if off != len(blob):
        raise ValueError("trailing")
    return info, records


def format_log(blob):
    info, records = parse_log(blob)
    profile = PROFILE_NAME.get(info["profile"], str(info["profile"]))
    lines = [
        "CANGO1 bitrate=%d listen_only=%d profile=%s project=%s unix_s=%d start_us=%d"
        % (info["bitrate"], info["listen_only"], profile, info["project_id"], info["unix_s"], info["start_us"])
    ]
    for rec in records:
        kind = rec["type"]
        if kind == "frame":
            flags = []
            if rec["flags"] & 0x01:
                flags.append("EXT")
            if rec["flags"] & 0x02:
                flags.append("RTR")
            if rec["flags"] & 0x04:
                flags.append("TX")
            lines.append(
                "FRAME t_us=%d id=%X flags=%s len=%d data=%s"
                % (rec["t_us"], rec["id"], ",".join(flags), rec["len"], rec["data"].hex())
            )
        elif kind == "gap":
            lines.append("GAP missed=%d" % rec["missed"])
        elif kind == "marker":
            lines.append("MARKER code=%d name=%s t_us=%d" % (rec["code"], MARKER_NAME.get(rec["code"], "?"), rec["t_us"]))
        elif kind == "user":
            lines.append("USER t_us=%d" % rec["t_us"])
        elif kind == "error":
            lines.append("ERROR t_us=%d tec=%d rec=%d state=%d" % (rec["t_us"], rec["tec"], rec["rec"], rec["state"]))
        else:
            lines.append("UNKNOWN rtype=%d" % rec.get("rtype", 0))
    return "\n".join(lines) + "\n"
