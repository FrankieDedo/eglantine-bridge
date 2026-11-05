// Test host: ebridge_host_cli [port]
// Streams a 60 fps test pattern: a square that follows the left stick
// (green while A is held), moving colour bars, and a binary frame counter.
// Audio: a 440 Hz tone while A is held (silence otherwise), to check audio and its delay.
// Good for checking picture, smoothness and input latency without Eglantine.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "ebridge/host.h"
#include "ebridge_protocol.h"

static std::atomic<bool> g_quit{false};

struct SharedInput {
    std::atomic<float> lx{0}, ly{0};
    std::atomic<bool> a{false};
};

static void DrawPattern(std::vector<uint8_t>& px, int w, int h, uint32_t frame, const SharedInput& in) {
    // Scrolling colour bars.
    for (int y = 0; y < h; y++) {
        uint8_t* row = px.data() + size_t(y) * w * 4;
        for (int x = 0; x < w; x++) {
            const int bar = ((x + int(frame) * 4) / 160) % 7;
            static const uint8_t colours[7][3] = {{192, 192, 192}, {0, 192, 192}, {192, 192, 0},
                                                  {0, 192, 0},     {192, 0, 192}, {0, 0, 192},
                                                  {192, 0, 0}};   // BGR
            const uint8_t shade = uint8_t(40 + y * 120 / h);
            row[x * 4 + 0] = uint8_t(colours[bar][0] * shade / 160);
            row[x * 4 + 1] = uint8_t(colours[bar][1] * shade / 160);
            row[x * 4 + 2] = uint8_t(colours[bar][2] * shade / 160);
            row[x * 4 + 3] = 255;
        }
    }
    auto rect = [&](int x0, int y0, int rw, int rh, uint8_t b, uint8_t g, uint8_t r) {
        for (int y = std::max(0, y0); y < std::min(h, y0 + rh); y++)
            for (int x = std::max(0, x0); x < std::min(w, x0 + rw); x++) {
                uint8_t* p = px.data() + (size_t(y) * w + x) * 4;
                p[0] = b;
                p[1] = g;
                p[2] = r;
            }
    };
    // Frame counter: 16 blocks, white = 1.
    for (int bit = 0; bit < 16; bit++) {
        const bool on = (frame >> (15 - bit)) & 1;
        rect(40 + bit * 75, 20, 65, 40, on ? 255 : 30, on ? 255 : 30, on ? 255 : 30);
    }
    // Square that follows the left stick.
    const int size = 120;
    const int cx = int(w / 2 + in.lx.load() * (w / 2 - size));
    const int cy = int(h / 2 - in.ly.load() * (h / 2 - size));
    rect(cx - size / 2 - 4, cy - size / 2 - 4, size + 8, size + 8, 0, 0, 0);
    if (in.a) rect(cx - size / 2, cy - size / 2, size, size, 0, 230, 0);
    else rect(cx - size / 2, cy - size / 2, size, size, 255, 255, 255);
}

int main(int argc, char** argv) {
    ebridge::HostConfig cfg;
    cfg.name = "Eglantine (test)";
    if (argc > 1) cfg.port = static_cast<uint16_t>(std::atoi(argv[1]));

    ebridge::Host host(cfg);
    SharedInput shared;
    uint32_t prev_buttons = 0;

    host.SetEventCallback([](ebridge::Event e, const std::string& d) {
        switch (e) {
        case ebridge::Event::PairingPin: std::printf(">>> PAIRING PIN: %s <<<\n", d.c_str()); break;
        case ebridge::Event::Connected: std::printf("connected: %s\n", d.c_str()); break;
        case ebridge::Event::Disconnected: std::printf("disconnected: %s\n", d.c_str()); break;
        case ebridge::Event::Log: std::printf("%s\n", d.c_str()); break;
        }
        std::fflush(stdout);
    });
    host.SetInputCallback([&](const ebridge::InputState& s) {
        shared.lx = s.lx;
        shared.ly = s.ly;
        shared.a = s.buttons & 1;
        if (s.buttons == prev_buttons) return;
        std::printf("P%d buttons=%05x L=(%.2f,%.2f) R=(%.2f,%.2f) touch=%d gyro=(%.2f,%.2f,%.2f)\n",
                    s.player + 1, s.buttons, s.lx, s.ly, s.rx, s.ry, s.touch_count, s.gyro[0],
                    s.gyro[1], s.gyro[2]);
        std::fflush(stdout);
        const bool a = s.buttons & 1, was_a = prev_buttons & 1;
        if (a != was_a) host.SendRumble(s.player, a ? 0.6f : 0, 160, a ? 0.6f : 0, 320);
        prev_buttons = s.buttons;
    });

    // Fake game list, to try the console's game menu without Eglantine.
    host.SetGameCallbacks(
        [] {
            std::vector<ebridge::GameInfo> games{
                {"Test game", 0x0100000000010000ull, true, "test1", "NSP", "1.2 GiB", "02:48:50", {}},
                {"Example Town Adventure", 0x0100000000020000ull, false, "test2", "XCI", "15.8 GiB", "00:12:03", {}},
                {"A very long title to see how it gets cut on the console screen",
                 0, false, "test3", "Directory", "Not scanned", "", {}},
            };
            // Coloured gradient icons, so the console's list can be checked without Eglantine.
            const uint8_t tints[3][3] = {{226, 58, 98}, {60, 140, 230}, {80, 200, 120}};
            for (size_t g = 0; g < games.size(); g++) {
                auto& px = games[g].icon_rgb;
                px.resize(64 * 64 * 3);
                for (int y = 0; y < 64; y++)
                    for (int x = 0; x < 64; x++)
                        for (int c = 0; c < 3; c++)
                            px[(y * 64 + x) * 3 + c] = static_cast<uint8_t>(tints[g][c] * (64 + x + y) / 192);
            }
            return games;
        },
        [&host](const ebridge::GameInfo& g) {
            std::printf("launch requested: %s (%s)\n", g.name.c_str(), g.id.c_str());
            std::fflush(stdout);
            // Fake boot: a few seconds of "loading shaders", then running.
            std::thread([&host, name = g.name] {
                host.SetStatus(EB_STATUS_BOOTING, 0, 0, "Starting the game", name);
                std::this_thread::sleep_for(std::chrono::seconds(1));
                for (uint32_t i = 0; i <= 400; i += 20) {
                    host.SetStatus(EB_STATUS_LOADING, i, 400, "Loading shaders", name);
                    std::this_thread::sleep_for(std::chrono::milliseconds(150));
                }
                host.SetStatus(EB_STATUS_RUNNING, 0, 0, "", name);
            }).detach();
        });
    host.SetStatus(EB_STATUS_RUNNING, 0, 0, "", "Test game");

    std::string err;
    if (!host.Start(&err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return 1;
    }
    std::printf("listening on TCP+UDP %u (Ctrl+C to quit)\n", cfg.port);
    std::signal(SIGINT, [](int) { g_quit = true; });

    // Test pattern at 60 fps, only while a console is connected.
    const int w = cfg.width, h = cfg.height;
    std::vector<uint8_t> px(size_t(w) * h * 4);
    uint32_t frame = 0;
    std::vector<int16_t> tone(800 * 2);   // 1/60 s at 48 kHz, stereo
    double phase = 0;
    auto next = std::chrono::steady_clock::now();
    while (!g_quit) {
        next += std::chrono::microseconds(1000000 / 60);
        if (host.IsStreaming()) {
            DrawPattern(px, w, h, frame++, shared);
            host.SubmitVideoFrame(px.data(), w, h, w * 4, 0);
            for (size_t i = 0; i < 800; i++) {
                const int16_t v = shared.a ? static_cast<int16_t>(std::sin(phase) * 8000) : 0;
                phase += 2 * 3.14159265358979 * 440 / 48000;
                tone[i * 2] = tone[i * 2 + 1] = v;
            }
            host.SubmitAudio(tone.data(), 800);
        }
        std::this_thread::sleep_until(next);
    }
    host.Stop();
    return 0;
}
