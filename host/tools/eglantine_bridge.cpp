// Eglantine Bridge - external host: plays Eglantine on the Switch WITHOUT
// modifying the emulator.
//   video    : captures the Eglantine window from the screen (DXGI duplication)
//   controls : DSU server on 127.0.0.1:26760, read by Eglantine's built-in
//              "UDP Controller" support (buttons, sticks, gyro, touch)
//   audio    : captures what the PC plays (WASAPI loopback), sent as Opus
//   rumble   : not available (DSU has no rumble)
//
// Usage: eglantine_bridge [--window "Eglantine"] [--any-window] [--no-audio]
//                         [--port 52141] [--dsu-port 26760]
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "audio_capture.h"
#include "ebridge/dsu_server.h"
#include "ebridge/host.h"
#include "window_capture.h"

static std::atomic<bool> g_quit{false};

static void Log(const char* fmt, const std::string& a = {}) {
    std::printf(fmt, a.c_str());
    std::printf("\n");
    std::fflush(stdout);
}

static void EnableDpiAwareness() {
    // Window coordinates must be physical pixels, like the captured screen.
    using Fn = BOOL(WINAPI*)(HANDLE);
    if (auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"user32.dll"),
                                                      "SetProcessDpiAwarenessContext")))
        fn(reinterpret_cast<HANDLE>(-4));   // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    else
        SetProcessDPIAware();
}

int main(int argc, char** argv) {
    std::string window_title = "Eglantine";
    ebridge::HostConfig cfg;
    cfg.name = "Eglantine";
    uint16_t dsu_port = 26760;
    bool qt_only = true, audio = true;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--no-audio")) {
            audio = false;
            continue;
        }
        if (!std::strcmp(argv[i], "--any-window")) {   // also non-Qt windows (other builds, tests)
            qt_only = false;
            continue;
        }
        if (i + 1 >= argc) break;
        if (!std::strcmp(argv[i], "--window")) window_title = argv[++i];
        else if (!std::strcmp(argv[i], "--port")) cfg.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--dsu-port")) dsu_port = static_cast<uint16_t>(std::atoi(argv[++i]));
    }
    SetConsoleOutputCP(CP_UTF8);
    EnableDpiAwareness();

    ebridge::Host host(cfg);
    ebridge::DsuServer dsu;

    host.SetEventCallback([&](ebridge::Event e, const std::string& d) {
        switch (e) {
        case ebridge::Event::PairingPin: Log(">>> PAIRING PIN: %s <<<", d); break;
        case ebridge::Event::Connected: Log("Switch connected: %s", d); break;
        case ebridge::Event::Disconnected:
            dsu.Neutral();   // release every button in the emulator
            Log("Switch disconnected: %s", d);
            break;
        case ebridge::Event::Log: Log("%s", d); break;
        }
    });
    host.SetInputCallback([&](const ebridge::InputState& s) { dsu.Update(s); });

    std::string err;
    if (!dsu.Start(dsu_port, "127.0.0.1", &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    if (!host.Start(&err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf(
        "Eglantine Bridge (external) - listening for the Switch on TCP+UDP %u\n"
        "\n"
        "Eglantine setup (only once):\n"
        "  1. Emulation > Configure > Controls > Advanced: turn on \"Enable UDP\n"
        "     controllers\". The server 127.0.0.1:%u is already the default one\n"
        "     (Motion/Touch > Configure).\n"
        "  2. Connect the Switch, then in the Player 1 tab pick the input device\n"
        "     \"UDP Controller 0\": the buttons are mapped automatically.\n"
        "The video is taken from the window \"%s\": keep it visible (full screen\n"
        "is fine), not minimised. The audio is the whole PC's audio\n"
        "(--no-audio to turn it off). Ctrl+C to quit.\n\n",
        cfg.port, dsu_port, window_title.c_str());
    std::fflush(stdout);
    std::signal(SIGINT, [](int) { g_quit = true; });

    LoopbackCapture loopback;
    if (audio)
        loopback.Start([&](const int16_t* pcm, size_t frames) { host.SubmitAudio(pcm, frames); },
                       [](const std::string& m) { Log("%s", m); });

    WindowCapture cap;
    std::vector<uint8_t> frame, last;
    int last_w = 0, last_h = 0;
    bool was_streaming = false, warned_no_window = false;
    int last_dsu_clients = -1;
    auto last_sent = std::chrono::steady_clock::now();
    auto last_find = last_sent - std::chrono::seconds(10);
    auto streaming_since = last_sent;

    while (!g_quit) {
        const auto now = std::chrono::steady_clock::now();
        const bool streaming = host.IsStreaming();
        if (streaming != was_streaming) {
            was_streaming = streaming;
            warned_no_window = false;
            last_w = 0;   // new session: start from a fresh picture
            streaming_since = now;
        }
        if (!streaming) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        // Tell the user once whether Eglantine is reading the controls.
        const int clients = dsu.ClientCount();
        if (clients != last_dsu_clients && now - streaming_since > std::chrono::seconds(4)) {
            last_dsu_clients = clients;
            Log(clients ? "Eglantine is reading the controls (DSU)%s"
                        : "Eglantine is not reading the controls yet: enable the UDP controller (see above)%s");
        }

        if (!cap.HasWindow() && now - last_find > std::chrono::seconds(1)) {
            last_find = now;
            if (cap.FindWindowByTitle(window_title, qt_only)) {
                Log("capturing the window: %s", cap.WindowTitle());
                warned_no_window = false;
            } else if (!warned_no_window) {
                Log("window \"%s\" not found: open Eglantine (checking every second)", window_title);
                warned_no_window = true;
            }
        }

        int w = 0, h = 0;
        std::string cap_err;
        if (cap.HasWindow() && cap.Grab(frame, w, h, 20, &cap_err)) {
            host.SubmitVideoFrame(frame.data(), static_cast<uint32_t>(w), static_cast<uint32_t>(h),
                                  static_cast<uint32_t>(w) * 4, 0);
            last.swap(frame);
            last_w = w;
            last_h = h;
            last_sent = std::chrono::steady_clock::now();
            continue;
        }
        if (!cap_err.empty()) Log("%s", cap_err);
        if (!cap.HasWindow()) std::this_thread::sleep_for(std::chrono::milliseconds(20));

        // Screen not changing (paused game, menu) or no window: repeat the last
        // picture a few times per second, so a console that just connected or
        // lost a packet still gets a keyframe.
        if (std::chrono::steady_clock::now() - last_sent > std::chrono::milliseconds(250)) {
            if (last_w == 0) {
                last_w = 1280;
                last_h = 720;
                last.assign(static_cast<size_t>(last_w) * last_h * 4, 24);   // dark grey: "no picture yet"
            }
            host.SubmitVideoFrame(last.data(), static_cast<uint32_t>(last_w), static_cast<uint32_t>(last_h),
                                  static_cast<uint32_t>(last_w) * 4, 0);
            last_sent = std::chrono::steady_clock::now();
        }
    }
    loopback.Stop();
    dsu.Stop();
    host.Stop();
    return 0;
}
