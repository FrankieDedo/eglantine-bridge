#include "audio_capture.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT (MinGW declares them but has no definition to link).
const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

enum class SampleType { Float32, Int16, Int32, Unsupported };

SampleType Classify(const WAVEFORMATEX* f) {
    WORD tag = f->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* x = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
        if (IsEqualGUID(x->SubFormat, kSubtypeFloat)) tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (IsEqualGUID(x->SubFormat, kSubtypePcm)) tag = WAVE_FORMAT_PCM;
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && f->wBitsPerSample == 32) return SampleType::Float32;
    if (tag == WAVE_FORMAT_PCM && f->wBitsPerSample == 16) return SampleType::Int16;
    if (tag == WAVE_FORMAT_PCM && f->wBitsPerSample == 32) return SampleType::Int32;
    return SampleType::Unsupported;
}

// Linear resampler to 48 kHz stereo; keeps its state between buffers.
struct Resampler {
    double step = 1.0;   // input rate / 48000
    double t = 0.0;
    float prev_l = 0, prev_r = 0;
    std::vector<int16_t> out;

    static int16_t ToS16(float v) {
        return static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
    }
    void Push(float l, float r) {
        while (t < 1.0) {
            const float k = static_cast<float>(t);
            out.push_back(ToS16(prev_l + (l - prev_l) * k));
            out.push_back(ToS16(prev_r + (r - prev_r) * k));
            t += step;
        }
        t -= 1.0;
        prev_l = l;
        prev_r = r;
    }
};

}  // namespace

struct LoopbackCapture::Impl {
    std::thread thread;
    std::atomic<bool> running{false};
    Callback cb;
    LogFn log;

    void Log(const std::string& s) {
        if (log) log(s);
    }

    // One capture session; returns when the device goes away or on Stop().
    void RunOnce() {
        IMMDeviceEnumerator* en = nullptr;
        IMMDevice* dev = nullptr;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* cap = nullptr;
        WAVEFORMATEX* fmt = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&en));
        if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (SUCCEEDED(hr))
            hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
        if (SUCCEEDED(hr)) hr = client->GetMixFormat(&fmt);
        SampleType type = SampleType::Unsupported;
        if (SUCCEEDED(hr)) {
            type = Classify(fmt);
            if (type == SampleType::Unsupported) hr = E_FAIL;
        }
        if (SUCCEEDED(hr))
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200000 /* 20 ms */, 0,
                                    fmt, nullptr);
        if (SUCCEEDED(hr))
            hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&cap));
        if (SUCCEEDED(hr)) hr = client->Start();

        if (SUCCEEDED(hr)) {
            char msg[128];
            std::snprintf(msg, sizeof msg, "audio: capturing the PC audio output (%lu Hz, %u channels)",
                          static_cast<unsigned long>(fmt->nSamplesPerSec), static_cast<unsigned>(fmt->nChannels));
            Log(msg);
            Resampler rs;
            rs.step = fmt->nSamplesPerSec / 48000.0;
            const int ch = fmt->nChannels;
            while (running) {
                Sleep(5);
                UINT32 packet = 0;
                while (running && SUCCEEDED(hr = cap->GetNextPacketSize(&packet)) && packet > 0) {
                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;
                    hr = cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                    if (FAILED(hr)) break;
                    const bool silent = flags & AUDCLNT_BUFFERFLAGS_SILENT;
                    for (UINT32 i = 0; i < frames; i++) {
                        float l = 0, r = 0;
                        if (!silent) {
                            const int c1 = ch > 1 ? 1 : 0;   // mono: same on both sides
                            if (type == SampleType::Float32) {
                                const float* p = reinterpret_cast<const float*>(data) + size_t(i) * ch;
                                l = p[0];
                                r = p[c1];
                            } else if (type == SampleType::Int16) {
                                const int16_t* p = reinterpret_cast<const int16_t*>(data) + size_t(i) * ch;
                                l = p[0] / 32768.0f;
                                r = p[c1] / 32768.0f;
                            } else {
                                const int32_t* p = reinterpret_cast<const int32_t*>(data) + size_t(i) * ch;
                                l = static_cast<float>(p[0] / 2147483648.0);
                                r = static_cast<float>(p[c1] / 2147483648.0);
                            }
                        }
                        rs.Push(l, r);
                    }
                    cap->ReleaseBuffer(frames);
                    if (!rs.out.empty() && cb) cb(rs.out.data(), rs.out.size() / 2);
                    rs.out.clear();
                }
                if (FAILED(hr)) break;   // AUDCLNT_E_DEVICE_INVALIDATED: output device changed
            }
            client->Stop();
            if (running) Log("audio: device changed, reopening the capture");
        } else {
            char msg[128];
            std::snprintf(msg, sizeof msg, "audio: capture not available (0x%08lx), retrying",
                          static_cast<unsigned long>(hr));
            Log(msg);
        }
        if (fmt) CoTaskMemFree(fmt);
        SafeRelease(cap);
        SafeRelease(client);
        SafeRelease(dev);
        SafeRelease(en);
    }

    void Loop() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (running) {
            RunOnce();
            for (int i = 0; i < 20 && running; i++) Sleep(100);   // retry after 2 s
        }
        CoUninitialize();
    }
};

LoopbackCapture::LoopbackCapture() : impl(std::make_unique<Impl>()) {}
LoopbackCapture::~LoopbackCapture() { Stop(); }

void LoopbackCapture::Start(Callback cb, LogFn log) {
    if (impl->running.exchange(true)) return;
    impl->cb = std::move(cb);
    impl->log = std::move(log);
    impl->thread = std::thread([this] { impl->Loop(); });
}

void LoopbackCapture::Stop() {
    if (!impl->running.exchange(false)) return;
    if (impl->thread.joinable()) impl->thread.join();
}
