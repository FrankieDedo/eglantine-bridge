# Eglantine Bridge

Play the games running in **Eglantine** (a Nintendo Switch 2 emulator based on Eden) on your PC
from a **Nintendo Switch 1**. The PC streams video and audio to a homebrew app on the console,
and the console sends its controls back, rumble included.

```
 Switch 1 (homebrew app)                      PC (Eglantine with the bridge built in)
 ┌───────────────────────────┐   UDP input   ┌────────────────────────────────────┐
 │ Joy-Con / Pro Controller, │ ────────────► │ virtual gamepad: player 1 / handheld│
 │ touch screen, gyroscope   │               │                                    │
 │                           │   UDP video   │ Vulkan renderer → H.264 encoder    │
 │ H.264 decoder             │ ◄──────────── │ (x264, NVENC, Quick Sync)          │
 │ audout                    │ ◄──────────── │ audio output → Opus                │
 │ vibration                 │ ◄──────────── │ game rumble                        │
 └───────────────────────────┘   UDP audio   └────────────────────────────────────┘
        ▲   TCP: discovery, PIN pairing, game list, loading status, pings   ▲
```

## Features

- Finds the PC on the LAN, or connects to a typed IP:port. Pairing with a 4-digit PIN, once per console.
- **Game list on the console**, with icons, file type, size and play time: start any game in the PC library from the Switch.
- **Loading screen** that shows what the emulator is doing (starting the game, building shaders with progress, errors).
- 720p60 H.264 video, 4 slices per frame so the Switch decodes on all 4 cores. Recovers from packet loss with keyframes.
- Opus audio at 48 kHz, 10 ms packets. While the console is connected, the PC speakers are muted.
- Full controls: buttons, sticks, gyroscope and accelerometer, touch screen, rumble.
- The connection stays up when you press HOME on the Switch (NSP install).

## Requirements

- A Switch 1 with custom firmware (and sigpatches for the NSP).
- Eglantine with the bridge built in. Today that is [PokeDinner](https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork),
  a fork of Eglantine. See [docs/EGLANTINE_INTEGRATION.md](docs/EGLANTINE_INTEGRATION.md).
  For an unmodified Eglantine there is an external mode, with some limits (see below).
- The PC and the console on the same network (or port forwarding, see *Playing away from home*).

## Playing

1. Install the app on the Switch: the **NSP** (recommended: it has its own HOME menu icon, the
   keyboard and all the memory) or copy `eglantine-bridge.nro` to `sdmc:/switch/`.
2. Start Eglantine on the PC. The bridge listens on TCP and UDP port 52141. On the first run
   Windows may ask for network access: allow private networks.
3. Open Eglantine Bridge on the Switch and pick the PC. The first time, type the PIN shown on the PC.
4. Pick a game. The first row continues the game already running on the PC.

On the Switch, while playing:

- **Hold + and − for 1 second**: back to the game list.
- **− + D-pad down**: the emulated console's HOME button (the real one belongs to the system).

## Building

On Windows, with:

- [MSYS2](https://www.msys2.org/) in `C:\msys64`, UCRT64 packages: `gcc cmake ninja pkgconf ffmpeg opus`;
- [devkitPro](https://devkitpro.org/) with `switch-dev switch-sdl2 switch-sdl2_ttf switch-ffmpeg switch-libopus`.

Double-click **`build.bat`**, or run `./build.sh` from Git Bash. It builds the PC tools in
`build/host/` and the Switch app in `switch-client/eglantine-bridge.nro`. devkitPro's `make` does not
accept spaces in paths, so the script builds the Switch app in a temporary copy.

### NSP

```bash
./make_nsp.sh path/to/prod.keys
```

Makes `switch-client/eglantine-bridge.nsp` (title ID `05EB0B1D00000000`), the full app, not a
forwarder: the `.nro` is not needed on the SD card. The keys are only used to encrypt the NSP and are
not stored in it. The first run downloads and builds [hacBrewPack](https://github.com/rlaphoenix/hacBrewPack)
(GPLv2) in `tools/hacbrewpack/`.

## External mode (unmodified Eglantine)

`build/host/eglantine_bridge.exe` works next to a stock Eglantine: it captures the emulator window
(DXGI) and the PC audio (WASAPI), and passes the controls through the DSU/cemuhook protocol that
Eglantine already supports. Once, in Eglantine: *Emulation → Configure → Controls*:

- *Advanced* tab: turn on **Enable UDP controllers** (the server `127.0.0.1:26760` is the default);
- *Player 1* tab: pick **UDP Controller 0** as the input device (it appears once the Switch is connected).

Limits: no rumble (DSU has none), no game list or loading screen, the audio is the whole PC's, and the
Eglantine window must stay visible (full screen is fine, minimised is not). Options:
`--window "title"` if the emulator window has another name, `--no-audio`.

## Testing without Eglantine

- `build/host/ebridge_host_cli.exe`: a test host. It sends colour bars with a square that follows the
  left stick (green while A is held), a 440 Hz tone while A is held, and three fake games.
- `tools/ebridge_sim.py`: a Python implementation of both ends of the protocol.
  ```bash
  python tools/ebridge_sim.py selftest
  python tools/ebridge_sim.py client --host 127.0.0.1 --seconds 5 --dump video.h264 --dump-audio audio.opus --drop 0.02
  python tools/ebridge_sim.py client --host 127.0.0.1 --games --launch 1
  ```
- `tools/dsu_test_client.py`: pretends to be Eglantine and prints the controls that
  `eglantine_bridge.exe` would send it.
- `tools/ui_preview/`: draws the Switch app's screens on a PC, to check the layout without a console.

## Troubleshooting

Until the first picture arrives, the Switch shows a thin bar at the bottom: **red** = no video
packets, **yellow** = packets arrive but no frame decodes, **green** = frames decoded. While streaming,
the app writes `sdmc:/config/eglantine-bridge/log.txt` every second (video, audio, network).

## Playing away from home

Forward TCP **52141** and UDP **52141** on your router to the PC and type `your-public-ip:52141` on the
Switch (a DDNS name works too). The traffic is **not encrypted** yet
(see [the protocol](docs/PROTOCOL.md#security)), so the LAN is still the recommended setup.

## Repository layout

| Folder | Contents |
|---|---|
| `common/ebridge_protocol.h` | Packet format, shared by the Switch app and the PC |
| `switch-client/` | Switch app (C, libnx, SDL2): menus, connection, controls, video, audio |
| `host/` | C++ library for the PC (sessions, video and audio encoding, DSU server), used by the emulator |
| `host/tools/` | External mode (`eglantine_bridge.exe`) and the test host |
| `tools/` | Simulators, DSU test client, UI preview |
| `docs/` | [Protocol](docs/PROTOCOL.md) and [emulator integration](docs/EGLANTINE_INTEGRATION.md) |

## License

GPL-3.0-or-later, see [LICENSE](LICENSE). The app icon is Eglantine's flower.
