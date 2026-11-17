#!/usr/bin/env python3
"""Host checks: C codec parity, session simulator, and the browser page."""

import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from cango_codec import decode_batch, encode_batch, encode_log, format_gvret_csv, format_log, pack_frame

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def fail(msg):
    print("FAIL", msg)
    return False


def check_core():
    subprocess.run(["make", "-C", str(ROOT / "common"), "test"], check=True)
    subprocess.run(["make", "-C", str(ROOT / "common"), "tests/dump_fixture"], check=True)
    out = subprocess.check_output([str(ROOT / "common" / "tests" / "dump_fixture")], text=True)
    lines = dict(line.split(" ", 1) for line in out.splitlines())
    batch = bytes.fromhex(lines["BATCH"])
    log = bytes.fromhex(lines["LOG"])
    frame = pack_frame(1000, 0x7E0, 0, bytes((0x02, 0x10, 0x01)))
    if encode_batch(7, [frame]) != batch:
        return fail("python batch does not match C")
    seq, frames = decode_batch(batch)
    if seq != 7 or frames[0]["id"] != 0x7E0 or frames[0]["data"] != bytes((0x02, 0x10, 0x01)):
        return fail("batch decode %s" % frames)
    encoded = encode_log(
        {"bitrate": 500000, "listen_only": 1, "profile": 0, "project_id": "generic", "unix_s": 1700000000, "start_us": 1000},
        [
            {"type": "frame", "raw": frame},
            {"type": "marker", "code": 3, "t_us": 1000},
            {"type": "user", "t_us": 1500},
            {"type": "gap", "missed": 2},
        ],
    )
    if encoded != log:
        return fail("python log does not match C")
    text = format_log(log)
    for needle in ("profile=dealer", "project=generic", "id=7E0", "data=021001", "name=dealer_begin", "USER t_us=1500", "GAP missed=2"):
        if needle not in text:
            return fail("missing " + needle)
    csv_path = Path("/tmp/cango-sample.csv")
    cango_path = Path("/tmp/cango-sample.cango")
    cango_path.write_bytes(log)
    subprocess.run([sys.executable, str(ROOT / "tools" / "cango_convert.py"), str(cango_path), "--csv", str(csv_path)], check=True)
    csv_lines = csv_path.read_text().splitlines()
    if csv_lines[0] != "Time Stamp,ID,Extended,Bus,LEN,D1,D2,D3,D4,D5,D6,D7,D8":
        return fail("csv header %s" % csv_lines[0])
    if not csv_lines[1].startswith("1000,000007E0,false,0,3,02,10,01,00,00,00,00,00,"):
        return fail("csv row %s" % csv_lines[1])
    if format_gvret_csv(log) != csv_path.read_text():
        return fail("csv converter drift")
    ble = (ROOT / "firmware" / "main" / "ble_mode.c").read_text()
    kotlin = (ROOT / "android" / "app" / "src" / "main" / "java" / "app" / "cango" / "Codec.kt").read_text()
    uuids = {
        "8a3c0001-1f2e-4b5a-9c0d-11c0a1000001": "01, 0x00, 0x00, 0xa1, 0xc0, 0x11, 0x0d, 0x9c, 0x5a, 0x4b, 0x2e, 0x1f, 0x01, 0x00, 0x3c, 0x8a",
        "8a3c0002-1f2e-4b5a-9c0d-11c0a1000001": "01, 0x00, 0x00, 0xa1, 0xc0, 0x11, 0x0d, 0x9c, 0x5a, 0x4b, 0x2e, 0x1f, 0x02, 0x00, 0x3c, 0x8a",
        "8a3c0003-1f2e-4b5a-9c0d-11c0a1000001": "01, 0x00, 0x00, 0xa1, 0xc0, 0x11, 0x0d, 0x9c, 0x5a, 0x4b, 0x2e, 0x1f, 0x03, 0x00, 0x3c, 0x8a",
        "8a3c0004-1f2e-4b5a-9c0d-11c0a1000001": "01, 0x00, 0x00, 0xa1, 0xc0, 0x11, 0x0d, 0x9c, 0x5a, 0x4b, 0x2e, 0x1f, 0x04, 0x00, 0x3c, 0x8a",
    }
    for text_uuid, le in uuids.items():
        if text_uuid not in kotlin or le not in ble:
            return fail("uuid drift " + text_uuid)
    print("codec parity ok")
    return True


def free_port():
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def ws_client(port):
    sock = socket.create_connection(("127.0.0.1", port), timeout=5)
    key = "dGhlIHNhbXBsZSBub25jZQ=="
    req = (
        "GET /ws HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n" % key
    )
    sock.sendall(req.encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        buf += sock.recv(4096)
    if b"101 " not in buf.split(b"\r\n", 1)[0]:
        raise RuntimeError(buf[:80])

    def send_text(text):
        payload = text.encode()
        mask = b"\x11\x22\x33\x44"
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        hdr = bytes((0x81, 0x80 | len(payload))) + mask + masked
        sock.sendall(hdr)

    def recv():
        def exact(n):
            data = b""
            while len(data) < n:
                chunk = sock.recv(n - len(data))
                if not chunk:
                    raise EOFError
                data += chunk
            return data

        first = exact(2)
        opcode = first[0] & 0x0F
        length = first[1] & 0x7F
        if length == 126:
            length = struct.unpack(">H", exact(2))[0]
        elif length == 127:
            length = struct.unpack(">Q", exact(8))[0]
        return opcode, exact(length)

    send_text(json.dumps({"op": "hello"}))
    send_text(json.dumps({"op": "set_capture", "profile": "both", "dealer_quiet_s": 9}))
    send_text(json.dumps({"op": "arm_tx", "confirm": False}))
    send_text(json.dumps({"op": "discover"}))
    seen = {}
    frames = []
    deadline = time.time() + 5
    while time.time() < deadline and (len(seen) < 4 or not frames):
        opcode, payload = recv()
        if opcode == 1:
            msg = json.loads(payload.decode().strip())
            seen[msg.get("op")] = msg
        elif opcode == 2:
            _seq, got = decode_batch(payload)
            frames.extend(got)
    sock.close()
    if not seen.get("hello", {}).get("ok"):
        return fail("hello")
    if seen.get("set_capture", {}).get("ok") is not True:
        return fail("set_capture")
    if seen.get("arm_tx", {}).get("error") != "confirm":
        return fail("arm confirm")
    if seen.get("discover", {}).get("error") != "not_implemented":
        return fail("discover")
    ids = {frame["id"] for frame in frames}
    if ids != {0x100, 0x200, 0x123}:
        return fail("live ids %s" % ids)
    print("session protocol ok")
    return True


def browser(port, out_path):
    firefox = shutil.which("firefox")
    if not firefox:
        print("firefox missing; page selftest not run")
        return False
    profile = Path("/tmp/cango-ff-%d" % port)
    shutil.rmtree(profile, ignore_errors=True)
    profile.mkdir()
    url = "http://127.0.0.1:%d/?selftest=1" % port
    proc = subprocess.Popen(
        [firefox, "--headless", "--no-remote", "--profile", str(profile), url],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
    )
    report = None
    deadline = time.time() + 45
    while time.time() < deadline:
        if out_path.exists() and out_path.stat().st_size > 0:
            report = json.loads(out_path.read_text())
            break
        if proc.poll() is not None:
            break
        time.sleep(0.2)
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
    err = b""
    if proc.stderr:
        err = proc.stderr.read()[-500:]
    if report is None:
        print("firefox selftest did not report", err.decode("utf-8", "replace"))
        return False
    if not report.get("ok"):
        print("firefox selftest failed", json.dumps(report))
        return False
    print("firefox selftest ok", json.dumps(report["checks"]))
    return True


def main():
    if not check_core():
        return 1
    port = free_port()
    out_path = Path("/tmp/cango-selftest-%d.json" % port)
    out_path.unlink(missing_ok=True)
    sim = subprocess.Popen(
        [sys.executable, str(ROOT / "tools" / "sim_server.py"), str(port), str(out_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        deadline = time.time() + 5
        up = False
        while time.time() < deadline:
            try:
                urllib.request.urlopen("http://127.0.0.1:%d/" % port, timeout=0.5).read()
                up = True
                break
            except OSError:
                if sim.poll() is not None:
                    print(sim.stdout.read())
                    return 1
                time.sleep(0.05)
        if not up:
            print("sim did not start")
            return 1
        log = urllib.request.urlopen("http://127.0.0.1:%d/logs/dealer/sample.cango" % port).read()
        if not log.startswith(b"CG01"):
            print("FAIL log download")
            return 1
        page = urllib.request.urlopen("http://127.0.0.1:%d/" % port).read()
        if b"id=\"profile\"" in page or b"id=\"save-capture\"" in page:
            print("FAIL capture config still on page")
            return 1
        if not ws_client(port):
            return 1
        if not browser(port, out_path):
            return 1
    finally:
        sim.terminate()
        try:
            sim.wait(timeout=3)
        except subprocess.TimeoutExpired:
            sim.kill()
    print("host checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
