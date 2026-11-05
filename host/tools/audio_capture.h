// Captures what the PC is playing (WASAPI loopback of the default output
// device), converted to 48 kHz stereo int16. Windows only.
// It records every program's sound, not just Eglantine's.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

class LoopbackCapture {
public:
    // Called on the capture thread with 48 kHz stereo interleaved samples.
    using Callback = std::function<void(const int16_t* pcm, size_t frames)>;
    using LogFn = std::function<void(const std::string&)>;

    LoopbackCapture();
    ~LoopbackCapture();

    void Start(Callback cb, LogFn log);
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
