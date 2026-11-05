# Eglantine Bridge protocol (v1)

The exact packet layout is in [`common/ebridge_protocol.h`](../common/ebridge_protocol.h): little-endian,
packed structs with static size checks. [`tools/ebridge_sim.py`](../tools/ebridge_sim.py) is a reference
implementation of both ends.

## Ports

| Use | Protocol | Port |
|---|---|---|
| LAN discovery | UDP broadcast | 52140 |
| Control channel (pairing, game list, status, pings) | TCP | 52141 |
| Stream (input, video, audio, rumble) | UDP | 52141 |

To play away from home, forward **TCP 52141 + UDP 52141** to the PC and connect to
`public-address:52141`. Host names (DDNS) are accepted.

## Session

```
Switch                                   PC
  |-- UDP broadcast  DISC_QUERY  -------->|           (optional)
  |<------------------ DISC_RESPONSE -----|  name, port, busy?
  |
  |== TCP connect ========================|
  |-- HELLO (console id, secret) -------->|
  |<----------------- PAIR_REQUIRED ------|  only if not paired: the PC shows a PIN
  |-- PAIR_PIN (4 digits) --------------->|  3 attempts at most
  |<---------------------- PAIR_OK -------|  new secret, saved by the console
  |<---------------------- WELCOME -------|  session token, resolution, fps, bitrate
  |
  |-- GAMES_REQUEST --------------------->|  (optional) game menu
  |<-------------- GAME_ENTRY, GAME_ICON -|  one entry per game, icon in chunks
  |<-------------------------- GAMES_END -|
  |-- LAUNCH (index) -------------------->|  start a game, or skip to keep the running one
  |
  |-- UDP HELLO (token) ----------------->|  the PC learns the UDP address (works behind NAT)
  |-- UDP INPUT x120/s ------------------>|
  |<------------- UDP VIDEO, AUDIO, RUMBLE|
  |<------------------------- STATUS -----|  booting / loading shaders n/m / running / error
  |-- TCP PING every 1 s ---------------->|
  |<-------------------------- PONG ------|  latency shown on the Switch
```

A session ends with `BYE`, when the TCP connection closes, or after 5 seconds without data.
A host that doesn't know `GAMES_REQUEST` ignores it: the console waits about a second for
`GAMES_END` and goes straight to the stream.

## Game list

- `GAME_ENTRY` (`eb_ctl_game_t`, 160 bytes): index, title ID, running flag, name, file type, size and
  play time as display strings. The first 112 bytes are the original layout, so older consoles still read it.
- `GAME_ICON`: a 64x64 RGB888 icon (12288 bytes) sent in chunks of up to 1024 bytes after the entry.
  Only when the console asked for icons in `GAMES_REQUEST.flags`.
- `LAUNCH` starts the game with that index on the PC.

## Status

`STATUS` (`eb_ctl_status_t`, 172 bytes) tells the console what the emulator is doing, for its loading
screen: a state (`IDLE`, `BOOTING`, `LOADING`, `RUNNING`, `ERROR`), a progress `value`/`total`
(e.g. shaders built), a short text and the game name. The host sends it when it changes and once
at the start of each session. Older consoles ignore it.

## Design choices

- **Input over UDP without retransmission:** each packet holds the full controller state, so a lost
  packet is replaced by the next one about 8 ms later.
- **Buttons in the order of Eglantine's `VirtualGamepad::VirtualButton`:** bit N is button N, no
  conversion table.
- **Gyroscope in revolutions per second, accelerometer in G:** the libnx units, which are also the
  ones Eglantine uses internally.
- **Touch in 1280x720 pixels:** the PC normalises it to 0..1.

## Video

- **Format:** H.264 Annex-B, 720p60, CBR, no B-frames, 4 slices per frame (the Switch decodes the slices
  in parallel on its 4 cores), BT.709 limited range. A safety keyframe every 5 s.
- **Fragments:** each frame is split into `EB_UDP_VIDEO` packets of `eb_video_frag_t` + up to
  `EB_VIDEO_FRAG_DATA` (1368) bytes. Fragment *i* goes at offset `i * 1368`.
- **Loss:** when a frame doesn't complete, the Switch drops it, stops decoding and sends `REQUEST_IDR`
  every 250 ms until a keyframe arrives. The host sends at most one keyframe every 300 ms. There is no
  error correction yet: in tests, 2% packet loss still showed about 97% of the frames.
- **Encoders on the PC**, in order:
  1. FFmpeg's `h264_nvenc`, `h264_qsv`, `libx264` (`ultrafast`, `zerolatency`, 4 sliced threads), when the
     FFmpeg the host is built with has them;
  2. on Windows, Media Foundation: the GPU encoder (NVIDIA, AMD or Intel), kept only if a test frame comes
     out in at least 2 slices, then Microsoft's software encoder. Both are part of Windows, so they also work
     with a decode-only FFmpeg;
  3. FFmpeg's `h264_amf`, which always writes a single slice per frame: the Switch then decodes on one core only.

  `EBRIDGE_ENCODER=<name>` keeps only one of them (`h264_nvenc`, `h264_qsv`, `libx264`, `mf-gpu`,
  `mf-software`, `h264_amf`). Every 5 s the host logs fps, bitrate and the average encode time.

## Audio

- **Format:** Opus 48 kHz stereo, one 10 ms frame (480 samples per channel) per `EB_UDP_AUDIO`
  packet = `eb_audio_t` + one Opus packet. The encoder runs in `RESTRICTED_LOWDELAY` mode at 128 kbit/s.
- **Loss:** `seq` increases by one per frame. For each missing frame (up to 3) the Switch uses Opus
  packet loss concealment instead of skipping.
- **Delay:** 3 audout buffers of 10 ms plus a buffer of 30 to 120 ms. Above 120 ms (PC and Switch
  clocks drift slightly, packets arrive in bursts) old audio is dropped.

## Controls in external mode

`eglantine_bridge.exe` passes the controls to an unmodified Eglantine through the **DSU/cemuhook**
protocol the emulator already supports (`src/input_common/drivers/udp_client.cpp`): a UDP server on
`127.0.0.1:26760` with one controller with buttons, sticks, gyroscope, accelerometer and touch. The
conversions are the inverse of Eglantine's: gyroscope × 312, accelerometer axes reordered, touch in the
default calibration (x 100..1800, y 50..850). DSU has no rumble.

## Security

Pairing with a PIN stops another console on the same network from connecting by accident. The traffic
is **not encrypted**, though: the secret travels in the clear inside `HELLO`. That is fine on a LAN;
before exposing the port to the Internet the protocol needs encryption (a key exchange at pairing time
plus AES-GCM, as Moonlight does).
