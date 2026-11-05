#!/usr/bin/env python3
"""
Eglantine Bridge - reference simulator (protocol v1).

  python tools/ebridge_sim.py host   [--port 52141] [--pin 1234]
      Mock PC host: answers discovery, handles pairing, prints the input it
      receives and sends a test rumble pulse when A is pressed.
      Use it to test the real Switch app before Eglantine is patched.

  python tools/ebridge_sim.py client [--host IP] [--port 52141] [--pin 1234] [--seconds 3]
      Mock Switch client: discovers (or uses --host), pairs, streams fake
      input. Use it to test the host side.

  python tools/ebridge_sim.py selftest
      Runs host and client against each other on 127.0.0.1.

Struct layouts mirror common/ebridge_protocol.h exactly.
"""
import argparse
import json
import os
import random
import select
import socket
import struct
import sys
import threading
import time

PROTO_VERSION = 1
DEFAULT_PORT = 52141
DISCOVERY_PORT = 52140
MAGIC = 0x31474245
UDP_MAGIC = 0x4245

# --- struct formats (must match the C header) -------------------------------
F_DISC_QUERY = struct.Struct("<IBBH")                 # 8
F_DISC_RESP = struct.Struct("<IBBH16s32sI")           # 60
F_CTL_HDR = struct.Struct("<HH")                      # 4
F_HELLO = struct.Struct("<IHH16s16s32sHHBBH")         # 80
F_PIN = struct.Struct("<I")
F_PAIR_OK = struct.Struct("<16s16s")                  # 32
F_WELCOME = struct.Struct("<IHHHBBI")                 # 16
F_REJECT = struct.Struct("<HH")
F_PING = struct.Struct("<Q")
F_UDP_HDR = struct.Struct("<HBBIII")                  # 16
F_INPUT_HEAD = struct.Struct("<BBBBIhhhh3f3f")        # 40
F_TOUCH = struct.Struct("<HHB3x")                     # 8
F_RUMBLE = struct.Struct("<B3x4f")                    # 20
F_VIDEO = struct.Struct("<IHHBBHI")                   # 16

assert (F_DISC_QUERY.size, F_DISC_RESP.size, F_HELLO.size, F_PAIR_OK.size,
        F_WELCOME.size, F_UDP_HDR.size, F_INPUT_HEAD.size + 4 * F_TOUCH.size,
        F_RUMBLE.size, F_VIDEO.size) == (8, 60, 80, 32, 16, 16, 72, 20, 16)

DISC_QUERY, DISC_RESPONSE = 1, 2
(CTL_HELLO, CTL_PAIR_REQUIRED, CTL_PAIR_PIN, CTL_PAIR_OK, CTL_WELCOME,
 CTL_REJECT, CTL_PING, CTL_PONG, CTL_BYE, CTL_REQUEST_IDR,
 CTL_GAMES_REQUEST, CTL_GAME_ENTRY, CTL_GAMES_END, CTL_LAUNCH, CTL_GAME_ICON, CTL_STATUS) = range(1, 17)
F_STATUS = struct.Struct("<B3xII96s64s")                 # 172
STATUS_NAMES = {0: "idle", 1: "booting", 2: "loading", 3: "running", 4: "error"}
F_GAME = struct.Struct("<HHIQ96s16s16s16s")             # 160
F_ICON = struct.Struct("<HHIII")                         # 16
F_GAMES_END = struct.Struct("<HH")
F_LAUNCH = struct.Struct("<HH")
assert F_GAME.size == 160 and F_ICON.size == 16
REJECT_NAMES = {1: "version mismatch", 2: "wrong PIN", 3: "host busy", 4: "protocol error"}
UDP_HELLO, UDP_INPUT, UDP_VIDEO, UDP_AUDIO, UDP_RUMBLE, UDP_HEARTBEAT = range(1, 7)
BUTTON_NAMES = ["A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "+", "-",
                "Left", "Up", "Right", "Down", "SL", "SR", "Home", "Capture"]
CODEC_H264 = 1


def now_us():
    return int(time.monotonic() * 1_000_000) & 0xFFFFFFFF


def cstr(b):
    return b.split(b"\0", 1)[0].decode("utf-8", "replace")


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed")
        buf += chunk
    return buf


def ctl_send(sock, msg_type, payload=b""):
    sock.sendall(F_CTL_HDR.pack(msg_type, len(payload)) + payload)


def ctl_recv(sock):
    msg_type, length = F_CTL_HDR.unpack(recv_exact(sock, F_CTL_HDR.size))
    return msg_type, recv_exact(sock, length) if length else b""


def udp_pack(msg_type, token, seq, payload=b""):
    return F_UDP_HDR.pack(UDP_MAGIC, msg_type, 0, token, seq, now_us()) + payload


def decode_input(payload):
    head = F_INPUT_HEAD.unpack_from(payload, 0)
    player, ctrl, touch_count, _r, buttons, lx, ly, rx, ry = head[:9]
    gyro, accel = head[9:12], head[12:15]
    touches = [F_TOUCH.unpack_from(payload, F_INPUT_HEAD.size + i * F_TOUCH.size)
               for i in range(min(touch_count, 4))]
    return dict(player=player, ctrl=ctrl, buttons=buttons, lx=lx, ly=ly, rx=rx, ry=ry,
                gyro=gyro, accel=accel, touches=touches)


def encode_input(buttons, lx=0, ly=0, rx=0, ry=0, gyro=(0, 0, 0), accel=(0, 0, -1), touches=()):
    out = F_INPUT_HEAD.pack(0, 0, len(touches), 0, buttons, lx, ly, rx, ry, *gyro, *accel)
    for i in range(4):
        x, y, fid = touches[i] if i < len(touches) else (0, 0, 0)
        out += F_TOUCH.pack(x, y, fid)
    return out


# --- mock host ---------------------------------------------------------------
class MockHost:
    def __init__(self, port, name, fixed_pin=None, state_file=None, quiet=False):
        self.port, self.name, self.fixed_pin, self.quiet = port, name, fixed_pin, quiet
        self.host_id = os.urandom(16)
        self.state_file = state_file
        self.paired = {}  # client_id hex -> secret hex
        if state_file and os.path.exists(state_file):
            with open(state_file) as f:
                self.paired = json.load(f)
        self.session = None  # dict(token, client_id, udp_addr, last_seen)
        self.stop = threading.Event()
        self.inputs_received = 0
        self.last_input = None

    def log(self, *a):
        if not self.quiet:
            print("[host]", *a, flush=True)

    def save(self):
        if self.state_file:
            with open(self.state_file, "w") as f:
                json.dump(self.paired, f)

    def start(self):
        self.tcp = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.tcp.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.tcp.bind(("0.0.0.0", self.port))
        self.tcp.listen(1)
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind(("0.0.0.0", self.port))
        self.disc = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.disc.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            self.disc.bind(("0.0.0.0", DISCOVERY_PORT))
        except OSError as e:
            self.log("discovery not available:", e)
            self.disc = None
        for target in (self.tcp_loop, self.udp_loop, self.disc_loop):
            threading.Thread(target=target, daemon=True).start()
        self.log(f"listening on TCP+UDP {self.port}, discovery on UDP {DISCOVERY_PORT}")

    def disc_loop(self):
        while self.disc and not self.stop.is_set():
            r, _, _ = select.select([self.disc], [], [], 0.2)
            if not r:
                continue
            data, addr = self.disc.recvfrom(64)
            if len(data) < F_DISC_QUERY.size:
                continue
            magic, typ, ver, _ = F_DISC_QUERY.unpack_from(data)
            if magic != MAGIC or typ != DISC_QUERY:
                continue
            flags = 2 if self.session else 0
            resp = F_DISC_RESP.pack(MAGIC, DISC_RESPONSE, PROTO_VERSION, self.port,
                                    self.host_id, self.name.encode()[:31], flags)
            self.disc.sendto(resp, addr)
            self.log("discovery reply to", addr[0])

    def tcp_loop(self):
        while not self.stop.is_set():
            r, _, _ = select.select([self.tcp], [], [], 0.2)
            if not r:
                continue
            conn, addr = self.tcp.accept()
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            try:
                self.handle_client(conn, addr)
            except (ConnectionError, OSError, struct.error) as e:
                self.log("client disconnected:", e)
            finally:
                self.session = None
                conn.close()

    def handle_client(self, conn, addr):
        msg_type, payload = ctl_recv(conn)
        if msg_type != CTL_HELLO or len(payload) != F_HELLO.size:
            ctl_send(conn, CTL_REJECT, F_REJECT.pack(4, 0))
            return
        magic, ver, _, client_id, secret, name, mw, mh, codecs, fps, _ = F_HELLO.unpack(payload)
        if magic != MAGIC or ver != PROTO_VERSION:
            ctl_send(conn, CTL_REJECT, F_REJECT.pack(1, 0))
            return
        if self.session:
            ctl_send(conn, CTL_REJECT, F_REJECT.pack(3, 0))
            return
        cid = client_id.hex()
        self.log(f"HELLO from '{cstr(name)}' ({addr[0]}), decoder max {mw}x{mh}@{fps}")
        if self.paired.get(cid) != secret.hex():
            pin = self.fixed_pin if self.fixed_pin is not None else random.randint(0, 9999)
            self.log(f">>> PAIRING PIN: {pin:04d} <<<")
            ctl_send(conn, CTL_PAIR_REQUIRED)
            for _attempt in range(3):
                msg_type, payload = ctl_recv(conn)
                if msg_type == CTL_PAIR_PIN and F_PIN.unpack(payload)[0] == pin:
                    new_secret = os.urandom(16)
                    self.paired[cid] = new_secret.hex()
                    self.save()
                    ctl_send(conn, CTL_PAIR_OK, F_PAIR_OK.pack(new_secret, self.host_id))
                    self.log("paired")
                    break
                ctl_send(conn, CTL_PAIR_REQUIRED)
            else:
                ctl_send(conn, CTL_REJECT, F_REJECT.pack(2, 0))
                return
        token = random.getrandbits(32) or 1
        w, h = min(mw, 1280), min(mh, 720)
        self.session = dict(token=token, client_id=cid, udp_addr=None, last_seen=time.monotonic(),
                            rumble_seq=0)
        ctl_send(conn, CTL_WELCOME, F_WELCOME.pack(token, self.port, w, h, 60, CODEC_H264, 15000))
        self.log(f"session started (token {token:08x})")
        while not self.stop.is_set():
            r, _, _ = select.select([conn], [], [], 0.2)
            if self.session and time.monotonic() - self.session["last_seen"] > 5:
                self.log("no UDP data for 5 s, closing the session")
                return
            if not r:
                continue
            msg_type, payload = ctl_recv(conn)
            if msg_type == CTL_PING:
                ctl_send(conn, CTL_PONG, payload)
            elif msg_type == CTL_BYE:
                self.log("the client closed the session")
                return

    def udp_loop(self):
        prev_buttons = 0
        while not self.stop.is_set():
            r, _, _ = select.select([self.udp], [], [], 0.2)
            if not r:
                continue
            data, addr = self.udp.recvfrom(2048)
            s = self.session
            if len(data) < F_UDP_HDR.size or not s:
                continue
            magic, typ, _f, token, seq, _ts = F_UDP_HDR.unpack_from(data)
            if magic != UDP_MAGIC or token != s["token"]:
                continue
            s["udp_addr"], s["last_seen"] = addr, time.monotonic()
            if typ == UDP_INPUT:
                st = decode_input(data[F_UDP_HDR.size:])
                self.inputs_received += 1
                self.last_input = st
                if st["buttons"] != prev_buttons:
                    names = [n for i, n in enumerate(BUTTON_NAMES) if st["buttons"] >> i & 1]
                    self.log(f"buttons: {' '.join(names) or '-'}  L=({st['lx']},{st['ly']}) "
                             f"R=({st['rx']},{st['ry']}) touch={len(st['touches'])}")
                    if st["buttons"] & 1 and not prev_buttons & 1:
                        self.send_rumble(0.6, 160.0, 0.6, 320.0)
                    elif not st["buttons"] & 1 and prev_buttons & 1:
                        self.send_rumble(0, 160.0, 0, 320.0)
                    prev_buttons = st["buttons"]

    def send_rumble(self, al, fl, ah, fh):
        s = self.session
        if s and s["udp_addr"]:
            s["rumble_seq"] += 1
            self.udp.sendto(udp_pack(UDP_RUMBLE, s["token"], s["rumble_seq"],
                                     F_RUMBLE.pack(0, al, fl, ah, fh)), s["udp_addr"])


# --- mock client -------------------------------------------------------------
def discover(timeout=1.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    q = F_DISC_QUERY.pack(MAGIC, DISC_QUERY, PROTO_VERSION, 0)
    for target in ("255.255.255.255", "127.0.0.1"):
        try:
            s.sendto(q, (target, DISCOVERY_PORT))
        except OSError:
            pass
    found, end = {}, time.monotonic() + timeout
    while time.monotonic() < end:
        r, _, _ = select.select([s], [], [], 0.1)
        if r:
            data, addr = s.recvfrom(128)
            if len(data) >= F_DISC_RESP.size:
                magic, typ, ver, port, hid, name, flags = F_DISC_RESP.unpack_from(data)
                if magic == MAGIC and typ == DISC_RESPONSE:
                    found[hid] = (addr[0], port, cstr(name), flags)
    s.close()
    return list(found.values())


VIDEO_FRAG_DATA = 1400 - 16 - 16


class FrameAssembler:
    """Rebuilds video frames from fragments; drops incomplete ones."""

    def __init__(self):
        self.frames_ok = self.frames_lost = self.keyframes = 0
        self.cur_id, self.parts, self.need_key = None, {}, True
        self.output = []  # complete frames, in order

    def add(self, payload):
        frame_id, idx, count, flags, codec, _r, size = F_VIDEO.unpack_from(payload)
        data = payload[F_VIDEO.size:]
        lost = False
        if frame_id != self.cur_id:
            if self.cur_id is not None and self.parts:
                self.frames_lost += 1   # previous frame never completed
                self.need_key = lost = True
            self.cur_id, self.parts = frame_id, {}
        self.parts[idx] = data
        if len(self.parts) == count:
            frame = b"".join(self.parts[i] for i in range(count))
            self.parts = {}
            if len(frame) != size:
                self.frames_lost += 1
                self.need_key = lost = True
            elif flags & 1 or not self.need_key:
                self.need_key = False
                self.keyframes += flags & 1
                self.frames_ok += 1
                self.output.append(frame)
        return lost


def ogg_opus(packets):
    """Wraps 10 ms Opus packets (48 kHz stereo) in an Ogg file any player can open."""
    def crc(data):
        c = 0
        for b in data:
            c ^= b << 24
            for _ in range(8):
                c = ((c << 1) ^ 0x04C11DB7) & 0xFFFFFFFF if c & 0x80000000 else (c << 1) & 0xFFFFFFFF
        return c

    def page(payload, granule, seqno, flags):
        lacing = bytes([255] * (len(payload) // 255) + [len(payload) % 255])
        hdr = struct.pack("<4sBBqIII", b"OggS", 0, flags, granule, 0x45425247, seqno, 0)
        hdr += bytes([len(lacing)]) + lacing
        c = crc(hdr + payload)
        return hdr[:22] + struct.pack("<I", c) + hdr[26:] + payload

    head = b"OpusHead" + struct.pack("<BBHIhB", 1, 2, 0, 48000, 0, 0)
    tags = b"OpusTags" + struct.pack("<I", 7) + b"ebridge" + struct.pack("<I", 0)
    out = [page(head, 0, 0, 2), page(tags, 0, 1, 0)]
    for i, pkt in enumerate(packets):
        out.append(page(pkt, (i + 1) * 480, i + 2, 4 if i == len(packets) - 1 else 0))
    return b"".join(out)


def list_games(tcp, udp, token, dest):
    """Asks for the game list like the console's menu does. None = host doesn't support it."""
    ctl_send(tcp, CTL_GAMES_REQUEST, struct.pack("<I", 1))   # 1 = with icons
    games, icons, deadline = [], {}, time.monotonic() + 3
    while time.monotonic() < deadline:
        udp.sendto(udp_pack(UDP_HEARTBEAT, token, 0), dest)
        r, _, _ = select.select([tcp], [], [], 0.2)
        if not r:
            continue
        msg_type, payload = ctl_recv(tcp)
        txt = lambda b: b.split(b"\0")[0].decode("utf-8", "replace")
        if msg_type == CTL_GAME_ENTRY:
            idx, flags, _, title_id, name, ftype, size, ptime = F_GAME.unpack(payload[:F_GAME.size])
            games.append((idx, bool(flags & 1), title_id,
                          f"{txt(name)}  [{txt(ftype)} | {txt(size)} | {txt(ptime)}]"))
        elif msg_type == CTL_GAME_ICON:
            idx, _sz, off, total, _ = F_ICON.unpack_from(payload)
            icons[idx] = icons.get(idx, 0) + len(payload) - F_ICON.size
        elif msg_type == CTL_GAMES_END:
            full = sum(1 for v in icons.values() if v == 64 * 64 * 3)
            print(f"[client] complete icons: {full}")
            return games
    return None


def run_client(host, port, pin, seconds, state_file, quiet=False, dump=None, drop=0.0,
               dump_audio=None, games=False, launch=None):
    def log(*a):
        if not quiet:
            print("[client]", *a, flush=True)

    state = {}
    if state_file and os.path.exists(state_file):
        with open(state_file) as f:
            state = json.load(f)
    if "client_id" not in state:
        state["client_id"] = os.urandom(16).hex()
    secret = bytes.fromhex(state.get("secrets", {}).get(f"{host}:{port}", "00" * 16))

    tcp = socket.create_connection((host, port), timeout=5)
    tcp.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    ctl_send(tcp, CTL_HELLO, F_HELLO.pack(MAGIC, PROTO_VERSION, 0, bytes.fromhex(state["client_id"]),
                                          secret, b"ebridge_sim", 1920, 1080, CODEC_H264, 60, 0))
    paired_now = False
    while True:
        msg_type, payload = ctl_recv(tcp)
        if msg_type == CTL_PAIR_REQUIRED:
            if pin is None:
                pin = int(input("PIN mostrato dall'host: "))
            log("sending PIN")
            ctl_send(tcp, CTL_PAIR_PIN, F_PIN.pack(pin))
        elif msg_type == CTL_PAIR_OK:
            new_secret, _hid = F_PAIR_OK.unpack(payload)
            state.setdefault("secrets", {})[f"{host}:{port}"] = new_secret.hex()
            if state_file:
                with open(state_file, "w") as f:
                    json.dump(state, f)
            paired_now = True
            log("paired")
        elif msg_type == CTL_REJECT:
            reason = F_REJECT.unpack(payload)[0]
            raise RuntimeError(f"rejected: {REJECT_NAMES.get(reason, reason)}")
        elif msg_type == CTL_WELCOME:
            token, udp_port, w, h, fps, codec, kbps = F_WELCOME.unpack(payload)
            log(f"session: video {w}x{h}@{fps}, {kbps} kbps")
            break

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    video = FrameAssembler()
    dest = (host, udp_port)
    udp.sendto(udp_pack(UDP_HELLO, token, 0), dest)
    if games or launch is not None:
        found = list_games(tcp, udp, token, dest)
        if found is None:
            log("the host does not support the game list")
        else:
            log(f"{len(found)} games:")
            for idx, running, title_id, name in found:
                log(f"  [{idx}] {name}  ({title_id:016x}){'  <- running' if running else ''}")
            if launch is not None:
                ctl_send(tcp, CTL_LAUNCH, F_LAUNCH.pack(launch, 0))
                log(f"asked to launch game {launch}")
    ctl_send(tcp, CTL_PING, F_PING.pack(time.monotonic_ns() // 1000))
    seq, rumbles, rtt_ms, last_idr = 0, 0, None, 0.0
    audio_pkts, audio_last, audio_gaps = [], None, 0
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        t = time.monotonic()
        buttons = 1 if int(t * 2) % 2 else 0          # press A twice a second
        lx = int(32767 * 0.7)
        seq += 1
        udp.sendto(udp_pack(UDP_INPUT, token, seq,
                            encode_input(buttons, lx=lx, touches=[(640, 360, 0)] if buttons else [])), dest)
        r, _, _ = select.select([udp, tcp], [], [], 1 / 120)
        tcp_ready = tcp in r
        while udp in r:
            data, _ = udp.recvfrom(2048)
            typ = F_UDP_HDR.unpack_from(data)[1]
            if typ == UDP_RUMBLE:
                rumbles += 1
            elif typ == UDP_AUDIO:
                a_seq = struct.unpack_from("<I", data, F_UDP_HDR.size)[0]
                if audio_last is not None and a_seq != audio_last + 1:
                    audio_gaps += 1
                audio_last = a_seq
                audio_pkts.append(data[F_UDP_HDR.size + 8:])
            elif typ == UDP_VIDEO and random.random() < drop:
                pass  # simulated packet loss
            elif typ == UDP_VIDEO and video.add(data[F_UDP_HDR.size:]):
                ctl_send(tcp, CTL_REQUEST_IDR)
            r, _, _ = select.select([udp], [], [], 0)
        # Like the Switch app: keep asking for a keyframe (every 250 ms) until one arrives.
        if video.need_key and video.cur_id is not None and time.monotonic() - last_idr > 0.25:
            ctl_send(tcp, CTL_REQUEST_IDR)
            last_idr = time.monotonic()
        if tcp_ready:
            msg_type, payload = ctl_recv(tcp)
            if msg_type == CTL_STATUS:
                st, val, tot, text, game = F_STATUS.unpack(payload[:F_STATUS.size])
                txt = lambda b: b.split(b"\0")[0].decode("utf-8", "replace")
                prog = f" {val}/{tot}" if tot else ""
                log(f"status: {STATUS_NAMES.get(st, st)} '{txt(text)}'{prog} [{txt(game)}]")
            if msg_type == CTL_PONG:
                rtt_ms = (time.monotonic_ns() // 1000 - F_PING.unpack(payload)[0]) / 1000
    ctl_send(tcp, CTL_BYE)
    tcp.close()
    log(f"sent {seq} input packets, received {rumbles} rumble, RTT {rtt_ms:.2f} ms"
        if rtt_ms is not None else f"sent {seq} input packets, received {rumbles} rumble")
    if video.frames_ok or video.frames_lost:
        log(f"video: {video.frames_ok} complete frames ({video.keyframes} keyframes), "
            f"{video.frames_lost} lost")
    if audio_pkts:
        log(f"audio: {len(audio_pkts)} Opus packets ({len(audio_pkts) / 100:.1f} s), {audio_gaps} gaps")
    if dump_audio and audio_pkts:
        with open(dump_audio, "wb") as f:
            f.write(ogg_opus(audio_pkts))
        log(f"audio saved to {dump_audio}")
    if dump and video.output:
        with open(dump, "wb") as f:
            f.write(b"".join(video.output))
        log(f"H.264 stream saved to {dump}")
    return dict(sent=seq, rumbles=rumbles, paired_now=paired_now, rtt_ms=rtt_ms,
                frames=video.frames_ok, lost=video.frames_lost)


def selftest():
    import tempfile
    tmp = tempfile.mkdtemp()
    port = 52199
    host = MockHost(port, "selftest-host", fixed_pin=4321,
                    state_file=os.path.join(tmp, "host.json"), quiet=True)
    host.start()
    time.sleep(0.2)
    cstate = os.path.join(tmp, "client.json")

    # 1) wrong PIN must be rejected
    try:
        run_client("127.0.0.1", port, 1111, 0.2, cstate, quiet=True)
        print("FAIL: wrong PIN accepted"); return 1
    except RuntimeError as e:
        print("ok  wrong PIN rejected ->", e)
    time.sleep(0.3)

    # 2) first connection pairs
    r1 = run_client("127.0.0.1", port, 4321, 1.0, cstate, quiet=True)
    assert r1["paired_now"], "should have paired"
    print(f"ok  pairing + {r1['sent']} input, {r1['rumbles']} rumble")
    time.sleep(0.3)

    # 3) reconnect must reuse the secret (no PIN)
    r2 = run_client("127.0.0.1", port, None, 1.0, cstate, quiet=True)
    assert not r2["paired_now"], "should not have asked for the PIN"
    print(f"ok  reconnect without PIN, {r2['sent']} input")

    li = host.last_input
    assert host.inputs_received >= r1["sent"] * 0.9, "troppi input persi in locale"
    assert li["lx"] == int(32767 * 0.7) and li["accel"][2] == -1.0
    print(f"ok  host received {host.inputs_received} inputs, last left stick = {li['lx']}")
    assert r1["rumbles"] > 0, "no rumble received"
    print("ok  rumble host -> client")

    found = discover(0.5)
    print("ok  discovery:" if found else "--  discovery: no reply "
          "(normal if the firewall blocks broadcasts)", found)
    host.stop.set()
    print("SELFTEST PASSED")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["host", "client", "selftest"])
    ap.add_argument("--host")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--pin", type=int)
    ap.add_argument("--seconds", type=float, default=5)
    ap.add_argument("--name", default=socket.gethostname())
    ap.add_argument("--state", help="JSON file to store pairings in")
    ap.add_argument("--dump", help="client: save the received video to a .h264 file")
    ap.add_argument("--dump-audio", help="client: save the received audio to a .opus file")
    ap.add_argument("--games", action="store_true", help="client: ask for the game list")
    ap.add_argument("--launch", type=int, help="client: launch the game with this number")
    ap.add_argument("--drop", type=float, default=0.0, help="client: fraction of video packets to drop (test)")
    a = ap.parse_args()
    if a.mode == "selftest":
        sys.exit(selftest())
    if a.mode == "host":
        h = MockHost(a.port, a.name, a.pin, a.state or "ebridge_host_state.json")
        h.start()
        try:
            while True:
                time.sleep(1)
        except KeyboardInterrupt:
            pass
    else:
        target = a.host
        port = a.port
        if not target:
            found = discover()
            if not found:
                sys.exit("no host found: use --host IP --port PORT")
            target, port, name, _ = found[0]
            print(f"found '{name}' at {target}:{port}")
        run_client(target, port, a.pin, a.seconds, a.state or "ebridge_client_state.json",
                   dump=a.dump, drop=a.drop, dump_audio=a.dump_audio,
                   games=a.games, launch=a.launch)


if __name__ == "__main__":
    main()
