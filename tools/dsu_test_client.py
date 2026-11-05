"""DSU test client: behaves like Eglantine's built-in "UDP Controller" driver.

  python tools/dsu_test_client.py [--port 26760] [--seconds 10]

Subscribes to the DSU server of eglantine_bridge.exe, checks every packet the
way Eglantine does (magic, version, length, CRC32) and prints what Eglantine
would see, using the same conversions as src/input_common/drivers/udp_client.cpp.
"""
import argparse
import random
import socket
import struct
import time
import zlib

CLIENT_MAGIC = 0x43555344
SERVER_MAGIC = 0x53555344
VERSION = 1001
T_VERSION, T_PORTINFO, T_PADDATA = 0x00100000, 0x00100001, 0x00100002
HDR = struct.Struct("<IHHIII")          # magic, version, payload_len, crc, id, type
PORTINFO = struct.Struct("<BBBB6sBB")
PADDATA = struct.Struct("<12sIHBBBBBB12s6s6sQffffff")
assert HDR.size == 20 and PORTINFO.size == 12 and PADDATA.size == 80

# Eglantine's digital_button order and its auto-mapping to Switch buttons.
DSU_BITS = ["Share", "L3", "R3", "Options", "Up", "Right", "Down", "Left",
            "L2", "R2", "L1", "R1", "Triangle", "Circle", "Cross", "Square"]
TO_SWITCH = {"Circle": "A", "Cross": "B", "Triangle": "X", "Square": "Y", "Options": "+",
             "Share": "-", "L1": "L", "R1": "R", "L2": "ZL", "R2": "ZR", "L3": "LS", "R3": "RS",
             "Up": "Su", "Down": "Giu", "Left": "Sin", "Right": "Des"}


def request(payload, rtype, cid):
    hdr = HDR.pack(CLIENT_MAGIC, VERSION, len(payload) + 4, 0, cid, rtype)
    crc = zlib.crc32(hdr + payload) & 0xFFFFFFFF
    return HDR.pack(CLIENT_MAGIC, VERSION, len(payload) + 4, crc, cid, rtype) + payload


def validate(data):
    """Same checks as Eglantine's Response::Validate. Returns (type, body) or raises."""
    magic, ver, plen, crc, _sid, rtype = HDR.unpack_from(data)
    if magic != SERVER_MAGIC:
        raise ValueError("bad magic")
    if ver != VERSION:
        raise ValueError("bad version")
    size = {T_VERSION: 2, T_PORTINFO: 12, T_PADDATA: 80}.get(rtype)
    if size is None:
        raise ValueError("unknown type")
    if plen != size + 4 or len(data) < size + 20:
        raise ValueError(f"bad length: {len(data)} / {plen}")
    zeroed = data[:8] + b"\0\0\0\0" + data[12:20 + size]
    if zlib.crc32(zeroed) & 0xFFFFFFFF != crc:
        raise ValueError("bad CRC")
    return rtype, data[20:20 + size]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=26760)
    ap.add_argument("--seconds", type=float, default=10)
    a = ap.parse_args()
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("0.0.0.0", 0))
    s.settimeout(0.2)
    dest = (a.host, a.port)
    cid = random.getrandbits(32)

    s.sendto(request(b"", T_VERSION, cid), dest)
    end = time.time() + a.seconds
    next_sub = 0
    counts = {T_VERSION: 0, T_PORTINFO: 0, T_PADDATA: 0}
    bad = 0
    last_counter = None
    last_line = ""
    while time.time() < end:
        if time.time() >= next_sub:   # Eglantine re-subscribes every 3 s
            s.sendto(request(struct.pack("<I4B", 4, 0, 1, 2, 3), T_PORTINFO, cid), dest)
            s.sendto(request(struct.pack("<BB6s", 0, 0, b"\0" * 6), T_PADDATA, cid), dest)
            next_sub = time.time() + 3
        try:
            data, _ = s.recvfrom(256)
        except socket.timeout:
            continue
        try:
            rtype, body = validate(data)
        except ValueError as e:
            bad += 1
            print("[dsu] packet dropped:", e)
            continue
        counts[rtype] += 1
        if rtype == T_VERSION:
            print("[dsu] server version:", struct.unpack("<H", body)[0])
        elif rtype == T_PORTINFO:
            pid, state, model, *_ = PORTINFO.unpack(body)
            print(f"[dsu] port {pid}: state={state} model={model}")
        else:
            (_info, counter, buttons, home, hard, lx, ly, rx, ry, _analog, t1, _t2, _ts,
             ax, ay, az, pitch, yaw, roll) = PADDATA.unpack(body)
            if counter == last_counter:
                print("[dsu] repeated counter: Eglantine would drop the packet")
            last_counter = counter
            pressed = [TO_SWITCH[n] for i, n in enumerate(DSU_BITS) if buttons & (1 << i)]
            if home:
                pressed.append("HOME")
            t_active, _tid, tx, ty = struct.unpack("<BBHH", t1)
            # Eglantine's conversions
            stick = lambda v: (v - 127.0) / 127.0
            gyro = (pitch / 312, roll / 312, -yaw / 312)
            accel = (ax, -az, ay)
            touch = f" touch=({(tx - 100) / 1700:.2f},{(ty - 50) / 800:.2f})" if t_active else ""
            line = (f"buttons={'+'.join(pressed) or '-'} L=({stick(lx):.2f},{stick(ly):.2f}) "
                    f"R=({stick(rx):.2f},{stick(ry):.2f}) gyro=({gyro[0]:.2f},{gyro[1]:.2f},{gyro[2]:.2f}) "
                    f"accel=({accel[0]:.2f},{accel[1]:.2f},{accel[2]:.2f}){touch}")
            if line != last_line:
                print("[dsu]", line)
                last_line = line
    print(f"[dsu] received: version={counts[T_VERSION]} ports={counts[T_PORTINFO]} "
          f"state={counts[T_PADDATA]}, dropped={bad}")


if __name__ == "__main__":
    main()
