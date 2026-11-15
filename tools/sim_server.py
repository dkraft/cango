#!/usr/bin/env python3
"""Host stand-in for the CANGO WiFi session.

Serves web/www, a CANGO1 sample log, and a WebSocket that speaks the
session JSON plus live batches. View filters are not applied here: the
page keeps one filter per tab.
"""

import base64
import hashlib
import json
import select
import socketserver
import struct
import sys
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cango_codec import encode_batch, encode_log, pack_frame

ROOT = Path(__file__).resolve().parents[1]
WWW = ROOT / "web" / "www"
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

GENERIC = {
    "id": "generic",
    "name": "Generic",
    "bitrate": 500000,
    "listen_only": True,
    "saved_filters": [],
    "logging": {
        "capture_profile": "dealer",
        "startup_max_s": 60,
        "startup_quiet_s": 3,
        "dealer_quiet_s": 5,
        "dealer_context_s": 5,
        "complete_on_user_events": 0,
        "min_free_mb": 64,
    },
    "tx_allow": [],
    "modules": [],
    "actions": [],
}

ZERO_XB = {
    "id": "zero-xb",
    "name": "Zero XB",
    "bitrate": 500000,
    "listen_only": True,
    "saved_filters": [],
    "logging": {
        "capture_profile": "dealer",
        "startup_max_s": 60,
        "startup_quiet_s": 3,
        "dealer_quiet_s": 5,
        "dealer_context_s": 5,
        "complete_on_user_events": 0,
        "min_free_mb": 64,
    },
    "tx_allow": [],
    "modules": [],
    "actions": [],
}

SAMPLE_FRAME = pack_frame(1000, 0x7E0, 0, bytes((0x02, 0x10, 0x01)))
SAMPLE_LOG = encode_log(
    {"bitrate": 500000, "listen_only": 1, "profile": 0, "project_id": "generic", "unix_s": 1700000000, "start_us": 1000},
    [
        {"type": "frame", "raw": SAMPLE_FRAME},
        {"type": "marker", "code": 3, "t_us": 1000},
        {"type": "user", "t_us": 1500},
        {"type": "gap", "missed": 2},
    ],
)


class State:
    def __init__(self, selftest_path):
        self.lock = threading.Lock()
        self.profile = "dealer"
        self.numbers = {
            "startup_max_s": 60,
            "startup_quiet_s": 3,
            "dealer_quiet_s": 5,
            "dealer_context_s": 5,
            "complete_on_user_events": 0,
        }
        self.armed = False
        self.project = "generic"
        self.wifi = [
            {"slot": 0, "ssid": "BPSSI", "password": "KoalaBear"},
            {"slot": 1, "ssid": "mifi", "password": "KoalaBear"},
        ]
        self.rx = 0
        self.selftest_path = selftest_path
        self.selftest = None


STATE = None


def status_obj():
    with STATE.lock:
        return {
            "op": "bus_status",
            "state": "running",
            "tec": 0,
            "rec": 0,
            "rx": STATE.rx,
            "missed": 0,
            "dropped": 0,
            "sd": True,
            "capture": "idle",
            "mode": "wifi",
            "bitrate": 500000,
            "listen_only": not STATE.armed,
            "armed": STATE.armed,
            "profile": "30s",
            "project": STATE.project,
            "ap": "CANGO-SIM",
            "sta": False,
        }


def handle_op(msg):
    op = msg.get("op")
    if op == "hello":
        return {"op": "hello", "ok": True, "version": "0.1.0", "mode": "wifi", "ap": "CANGO-SIM"}
    if op == "get_projects":
        return {"op": "get_projects", "ok": True, "projects": [GENERIC, ZERO_XB]}
    if op == "list_logs":
        return {
            "op": "list_logs",
            "ok": True,
            "logs": [{"name": "sample.cango", "kind": "dealer", "bytes": len(SAMPLE_LOG)}],
        }
    if op in ("capture_status", "status"):
        return status_obj()
    if op in ("discover", "run_action"):
        return {"op": op, "ok": False, "error": "not_implemented"}
    if op == "set_filter":
        return {"op": "set_filter", "ok": True}
    if op == "set_capture":
        profile = msg.get("profile", STATE.profile)
        if profile not in ("dealer", "startup", "both"):
            return {"op": "set_capture", "ok": False, "error": "bad_profile"}
        with STATE.lock:
            STATE.profile = profile
            for key in STATE.numbers:
                if key in msg:
                    STATE.numbers[key] = int(msg[key])
        return {"op": "set_capture", "ok": True}
    if op == "apply_project":
        ident = msg.get("id", "")
        if ident not in ("generic", "zero-xb"):
            return {"op": "apply_project", "ok": False, "error": "missing"}
        with STATE.lock:
            STATE.project = ident
            STATE.profile = "dealer"
        return {"op": "apply_project", "ok": True}
    if op == "put_project":
        if msg.get("id") in (None, "", "generic"):
            return {"op": "put_project", "ok": False, "error": "rejected"}
        return {"op": "put_project", "ok": True}
    if op == "delete_project":
        if msg.get("id") in (None, "", "generic"):
            return {"op": "delete_project", "ok": False}
        return {"op": "delete_project", "ok": True}
    if op == "arm_tx":
        if msg.get("confirm") is not True:
            return {"op": "arm_tx", "ok": False, "error": "confirm"}
        with STATE.lock:
            STATE.armed = True
        return {"op": "arm_tx", "ok": True}
    if op == "disarm":
        with STATE.lock:
            STATE.armed = False
        return {"op": "disarm", "ok": True}
    if op == "send_frame":
        with STATE.lock:
            armed = STATE.armed
            allow = GENERIC["tx_allow"]
        if not armed:
            return {"op": "send_frame", "ok": False, "error": "disarmed"}
        if msg.get("id") not in allow:
            return {"op": "send_frame", "ok": False, "error": "not_allowed"}
        return {"op": "send_frame", "ok": True}
    if op == "set_time":
        if "unix_s" not in msg:
            return {"op": "set_time", "ok": False}
        return {"op": "set_time", "ok": True}
    if op == "set_wifi":
        try:
            slot = int(msg.get("slot", 0))
        except (TypeError, ValueError):
            return {"op": "set_wifi", "ok": False, "error": "slot"}
        if slot < 0 or slot > 3 or "ssid" not in msg or "password" not in msg:
            return {"op": "set_wifi", "ok": False, "error": "slot"}
        with STATE.lock:
            while len(STATE.wifi) <= slot:
                STATE.wifi.append({"slot": len(STATE.wifi), "ssid": "", "password": ""})
            STATE.wifi[slot] = {"slot": slot, "ssid": msg["ssid"], "password": msg["password"]}
        return {"op": "set_wifi", "ok": True}
    if op == "get_wifi":
        with STATE.lock:
            entries = list(STATE.wifi)
        return {"op": "get_wifi", "ok": True, "entries": entries}
    return {"op": op or "", "ok": False, "error": "unknown"}


def read_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise EOFError
        buf += chunk
    return buf


def read_http(conn):
    data = b""
    while b"\r\n\r\n" not in data:
        chunk = conn.recv(4096)
        if not chunk:
            raise EOFError
        data += chunk
        if len(data) > 1024 * 1024:
            raise ValueError("header")
    head, rest = data.split(b"\r\n\r\n", 1)
    lines = head.decode("iso-8859-1").split("\r\n")
    method, path, _proto = lines[0].split(" ", 2)
    headers = {}
    for line in lines[1:]:
        if ":" in line:
            key, value = line.split(":", 1)
            headers[key.strip().lower()] = value.strip()
    length = int(headers.get("content-length", "0") or 0)
    while len(rest) < length:
        rest += read_exact(conn, length - len(rest))
    return method, path, headers, rest[:length]


def send_http(conn, code, body, content_type, extra=""):
    reason = {200: "OK", 204: "No Content", 400: "Bad Request", 404: "Not Found", 405: "Method Not Allowed"}[code]
    if isinstance(body, str):
        body = body.encode()
    hdr = (
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n%s\r\n"
        % (code, reason, content_type, len(body), extra)
    )
    conn.sendall(hdr.encode("ascii") + body)


def content_type(path):
    if path.endswith(".html"):
        return "text/html; charset=utf-8"
    if path.endswith(".js"):
        return "text/javascript; charset=utf-8"
    if path.endswith(".css"):
        return "text/css; charset=utf-8"
    if path.endswith(".json"):
        return "application/json"
    if path.endswith(".cango"):
        return "application/octet-stream"
    return "application/octet-stream"


def ws_send(conn, opcode, payload):
    if isinstance(payload, str):
        payload = payload.encode()
    n = len(payload)
    hdr = bytes((0x80 | opcode,))
    if n < 126:
        hdr += bytes((n,))
    elif n < 65536:
        hdr += bytes((126,)) + struct.pack(">H", n)
    else:
        hdr += bytes((127,)) + struct.pack(">Q", n)
    conn.sendall(hdr + payload)


def ws_recv(conn):
    first = read_exact(conn, 2)
    opcode = first[0] & 0x0F
    masked = first[1] & 0x80
    length = first[1] & 0x7F
    if length == 126:
        length = struct.unpack(">H", read_exact(conn, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", read_exact(conn, 8))[0]
    mask = read_exact(conn, 4) if masked else b""
    data = read_exact(conn, length) if length else b""
    if mask:
        data = bytes(b ^ mask[i % 4] for i, b in enumerate(data))
    return opcode, data


def ws_session(conn):
    seq = 0
    tick = 0
    t_us = 0
    conn.settimeout(None)
    while True:
        ready, _, _ = select.select([conn], [], [], 0.05)
        if ready:
            opcode, data = ws_recv(conn)
            if opcode == 8:
                return
            if opcode == 9:
                ws_send(conn, 10, data)
                continue
            if opcode == 1:
                try:
                    msg = json.loads(data.decode())
                except (UnicodeError, json.JSONDecodeError):
                    ws_send(conn, 1, "{\"ok\":false,\"error\":\"json\"}\n")
                    continue
                reply = handle_op(msg)
                ws_send(conn, 1, json.dumps(reply, separators=(",", ":")) + "\n")
        tick += 1
        frames = [
            pack_frame(t_us, 0x100, 0, bytes((tick & 0xFF, 0, 0, 0))),
            pack_frame(t_us + 100, 0x200, 0, bytes((tick & 0xFF, 1))),
            pack_frame(t_us + 200, 0x123, 0, bytes((0x10, 0, 0, 0, 0, 0, 0, 0))),
        ]
        t_us = (t_us + 1000) & 0xFFFFFFFF
        with STATE.lock:
            STATE.rx += 3
        ws_send(conn, 2, encode_batch(seq, frames))
        seq = (seq + 1) & 0xFFFF
        if tick % 20 == 0:
            ws_send(conn, 1, json.dumps(status_obj(), separators=(",", ":")) + "\n")


def serve_static(conn, path):
    rel = path.split("?", 1)[0]
    if rel == "/":
        rel = "/index.html"
    if ".." in rel or not rel.startswith("/"):
        send_http(conn, 400, "path", "text/plain")
        return
    file_path = (WWW / rel.lstrip("/")).resolve()
    if WWW.resolve() not in file_path.parents and file_path != WWW.resolve():
        send_http(conn, 400, "path", "text/plain")
        return
    if not file_path.is_file():
        send_http(conn, 404, "missing", "text/plain")
        return
    send_http(conn, 200, file_path.read_bytes(), content_type(file_path.name), "Cache-Control: no-store\r\n")


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        conn = self.request
        try:
            method, path, headers, body = read_http(conn)
        except (EOFError, ValueError, ConnectionError):
            return
        target = path.split("?", 1)[0]
        try:
            if headers.get("upgrade", "").lower() == "websocket" and target == "/ws":
                key = headers.get("sec-websocket-key", "")
                accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
                raw = (
                    "HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: %s\r\n\r\n" % accept
                )
                conn.sendall(raw.encode("ascii"))
                ws_session(conn)
                return
            if method == "POST" and target == "/selftest":
                STATE.selftest = body
                Path(STATE.selftest_path).write_bytes(body)
                print("selftest " + body.decode("utf-8", "replace"), flush=True)
                send_http(conn, 200, "{\"ok\":true}\n", "application/json")
                return
            if method == "GET" and target == "/logs/dealer/sample.cango":
                send_http(conn, 200, SAMPLE_LOG, "application/octet-stream")
                return
            if method == "GET" and target.startswith("/logs/"):
                send_http(conn, 404, "missing", "text/plain")
                return
            if method == "GET":
                serve_static(conn, path)
                return
            send_http(conn, 405, "method", "text/plain")
        except (EOFError, BrokenPipeError, ConnectionError):
            return


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    global STATE
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    out = sys.argv[2] if len(sys.argv) > 2 else "/tmp/cango-selftest-%d.json" % port
    STATE = State(out)
    server = Server(("127.0.0.1", port), Handler)
    print("cango sim http://127.0.0.1:%d" % port, flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
