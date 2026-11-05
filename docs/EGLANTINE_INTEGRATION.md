# Building the bridge into Eglantine

The working integration is in [PokeDinner](https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork),
a fork of [Eglantine](https://github.com/Djihads80/Eglantine). Both derive from Eden, so the same
changes apply to Eglantine with small adjustments. This page lists what they touch.

## Overview

```
 renderer_vulkan ──► Common::EBridge::SubmitVideo ─┐
 sink_stream     ──► Common::EBridge::SubmitAudio ─┼──► EBridge::BridgeService ──► ebridge::Host ──► Switch
 emulated_controller ► SubmitRumble ───────────────┘          │                     (this repository)
                                                              ▼
                              InputCommon::VirtualGamepad (player 1 and handheld)
```

- `common/ebridge_hooks.{h,cpp}`: the sinks. The frontend installs them; the renderer, audio and HID code
  feed them. `WantsVideo()` / `WantsAudio()` are a single atomic load, so nothing is copied unless a
  console is connected.
- `yuzu/ebridge_service.{h,cpp}`: owns the `ebridge::Host`, turns the console's input into the virtual
  gamepad, forwards rumble, keeps the game list and status, and sends grey frames when no game is running.
- `externals/ebridge/`: the host library from this repository (`host/` and `common/`).

## Changes in the emulator

| File | Change |
|---|---|
| `src/common/CMakeLists.txt` | adds `ebridge_hooks.cpp/.h` |
| `CMakeLists.txt` | the `ENABLE_EBRIDGE` option |
| `src/yuzu/CMakeLists.txt` | with `ENABLE_EBRIDGE`: adds `ebridge_service.cpp/.h`, `add_subdirectory(externals/ebridge)`, links `ebridge`, defines `YUZU_EBRIDGE` |
| `src/video_core/renderer_vulkan/renderer_vulkan.{h,cpp}` | after each frame is composited, copies it to a 1280x720 BGRA image and reads it back through 3 buffers checked with `scheduler.IsFree`, so the renderer never waits |
| `src/audio_core/sink/sink_stream.cpp` | sends the mixed output to the bridge, then zeroes it: the PC stays quiet while the console plays |
| `src/hid_core/frontend/emulated_controller.cpp` | forwards the game's vibration for each player |
| `src/yuzu/main_window.{h,cpp}` | starts the bridge, shows the pairing PIN, feeds the game list every 2 s from the game list model (name, path, program ID, columns, icon), launches games through `OnGameListLoadFile`, and reports the status (booting, shader progress from `LoadProgress`, running on the first frame, errors, idle when emulation stops) |

## Controls

Eglantine's virtual gamepad is always connected to the players
(`EmulatedController::LoadVirtualGamepadParams()`): port 0 is player 1, port 8 is handheld mode.
The bridge writes both, so the game sees the console whatever the controller setup. Bit N of the
protocol's button mask is `VirtualGamepad::VirtualButton` N. Touch points use IDs offset by 100 so they
don't clash with the mouse.

## Dependencies

The host library needs FFmpeg **with H.264 encoders** (`libx264`, or `h264_nvenc` / `h264_qsv` /
`h264_amf`) and Opus, found with pkg-config. PokeDinner builds with MSYS2 UCRT64, whose FFmpeg has them.
The FFmpeg bundled with Eden-based builds may only have the decoders NVDEC needs: check before using it.

## Still to do before proposing it upstream

- A settings entry to turn the bridge on and off at run time (at build time there is `ENABLE_EBRIDGE`,
  on by default with the Qt frontend).
- Fetch the library through CPM from a tagged release of this repository instead of copying it.
- Build checks with MSVC and on Linux (so far only built with MSYS2 GCC on Windows).
- The OpenGL renderer has no video tap yet: only Vulkan streams.

## License

Eglantine and Eden are GPLv3, and the yuzu files they use are GPL-2.0-or-later. This repository is
GPL-3.0-or-later, so the library can be built into them.
