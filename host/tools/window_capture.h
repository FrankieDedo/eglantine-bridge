// Captures the game area of a window (Windows only), without touching the
// program that owns it. Uses DXGI Desktop Duplication, like OBS "Display
// Capture": the window must be visible on screen (not minimized or covered).
//
// The game area is the biggest child window (Eglantine/yuzu draw with Vulkan or
// OpenGL into a native child widget), cropped to the centred 16:9 rectangle,
// which is where the emulator draws the picture; the rest is black bars.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class WindowCapture {
public:
    WindowCapture();
    ~WindowCapture();

    // Looks for a visible top-level window whose title contains `title`
    // (case-insensitive); only Qt windows unless qt_only is false.
    // Returns false if none is open right now.
    bool FindWindowByTitle(const std::string& title, bool qt_only = true);
    bool HasWindow() const;
    std::string WindowTitle() const;

    // Waits up to timeout_ms for a new screen update and copies the game area
    // into `bgra` (tightly packed, w*4 bytes per row). Returns false on timeout
    // or if the window is gone / minimized; *error explains real failures.
    bool Grab(std::vector<uint8_t>& bgra, int& w, int& h, int timeout_ms, std::string* error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
