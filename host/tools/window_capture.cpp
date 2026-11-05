#include "window_capture.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>

namespace {

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

long Area(const RECT& r) { return std::max(0L, r.right - r.left) * std::max(0L, r.bottom - r.top); }

struct FindCtx {
    std::wstring needle;
    bool qt_only = true;
    HWND best = nullptr;
    long best_area = 0;
};

BOOL CALLBACK FindTop(HWND hwnd, LPARAM lp) {
    auto* c = reinterpret_cast<FindCtx*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t cls[64] = {0};
    GetClassNameW(hwnd, cls, 64);
    // Eglantine is a Qt app: skips e.g. an Explorer window on the "Eglantine bridge" folder.
    if (c->qt_only && std::wcsncmp(cls, L"Qt", 2) != 0) return TRUE;
    // Never our own console (its title is the path of eglantine_bridge.exe).
    if (!std::wcscmp(cls, L"ConsoleWindowClass") || !std::wcscmp(cls, L"CASCADIA_HOSTING_WINDOW_CLASS"))
        return TRUE;
    wchar_t title[512] = {0};
    GetWindowTextW(hwnd, title, 512);
    if (Lower(title).find(c->needle) == std::wstring::npos) return TRUE;
    RECT r;
    GetWindowRect(hwnd, &r);
    if (Area(r) > c->best_area) {
        c->best = hwnd;
        c->best_area = Area(r);
    }
    return TRUE;
}

struct ChildCtx {
    HWND best = nullptr;
    long best_area = 0;
};

BOOL CALLBACK FindChild(HWND hwnd, LPARAM lp) {
    auto* c = reinterpret_cast<ChildCtx*>(lp);
    if (!IsWindowVisible(hwnd)) return TRUE;
    RECT r;
    GetWindowRect(hwnd, &r);
    if (Area(r) > c->best_area) {
        c->best = hwnd;
        c->best_area = Area(r);
    }
    return TRUE;
}

}  // namespace

struct WindowCapture::Impl {
    HWND hwnd = nullptr;
    HMONITOR monitor = nullptr;
    RECT desktop{};   // the duplicated output, in virtual-desktop coordinates
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGIOutputDuplication* dup = nullptr;
    ID3D11Texture2D* staging = nullptr;
    int staging_w = 0, staging_h = 0;

    ~Impl() { ReleaseAll(); }

    void ReleaseAll() {
        SafeRelease(staging);
        SafeRelease(dup);
        SafeRelease(ctx);
        SafeRelease(dev);
        staging_w = staging_h = 0;
        monitor = nullptr;
    }

    // The area the emulator draws into, in screen coordinates.
    RECT GameRect() const {
        RECT client;
        GetClientRect(hwnd, &client);
        POINT tl{client.left, client.top}, br{client.right, client.bottom};
        ClientToScreen(hwnd, &tl);
        ClientToScreen(hwnd, &br);
        RECT r{tl.x, tl.y, br.x, br.y};
        ChildCtx cc;
        EnumChildWindows(hwnd, FindChild, reinterpret_cast<LPARAM>(&cc));
        // Use the render child only if it is a big part of the window (not a toolbar).
        if (cc.best && cc.best_area * 10 >= Area(r) * 3) GetWindowRect(cc.best, &r);
        // Centred 16:9 rectangle: the emulator letterboxes the picture inside it.
        const long w = r.right - r.left, h = r.bottom - r.top;
        long cw = w, ch = w * 9 / 16;
        if (ch > h) {
            ch = h;
            cw = h * 16 / 9;
        }
        const long x = r.left + (w - cw) / 2, y = r.top + (h - ch) / 2;
        return RECT{x, y, x + cw, y + ch};
    }

    bool OpenDuplication(HMONITOR mon, std::string* error) {
        ReleaseAll();
        IDXGIFactory1* factory = nullptr;
        if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
            if (error) *error = "DXGI not available";
            return false;
        }
        bool ok = false;
        IDXGIAdapter1* adapter = nullptr;
        for (UINT a = 0; !ok && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; a++) {
            IDXGIOutput* output = nullptr;
            for (UINT o = 0; !ok && adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; o++) {
                DXGI_OUTPUT_DESC desc;
                output->GetDesc(&desc);
                if (desc.Monitor == mon) {
                    // The device must live on the GPU that drives this monitor.
                    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
                    HRESULT hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, 2,
                                                   D3D11_SDK_VERSION, &dev, nullptr, &ctx);
                    IDXGIOutput1* output1 = nullptr;
                    if (SUCCEEDED(hr))
                        hr = output->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&output1));
                    if (SUCCEEDED(hr)) hr = output1->DuplicateOutput(dev, &dup);
                    SafeRelease(output1);
                    if (SUCCEEDED(hr)) {
                        desktop = desc.DesktopCoordinates;
                        monitor = mon;
                        ok = true;
                    } else if (error) {
                        char msg[96];
                        std::snprintf(msg, sizeof msg, "screen capture failed (0x%08lx)",
                                      static_cast<unsigned long>(hr));
                        *error = msg;
                    }
                }
                SafeRelease(output);
            }
            SafeRelease(adapter);
        }
        SafeRelease(factory);
        if (!ok) ReleaseAll();
        return ok;
    }
};

WindowCapture::WindowCapture() : impl(std::make_unique<Impl>()) {}
WindowCapture::~WindowCapture() = default;

bool WindowCapture::FindWindowByTitle(const std::string& title, bool qt_only) {
    FindCtx c;
    c.needle = Lower(ToWide(title));
    c.qt_only = qt_only;
    EnumWindows(FindTop, reinterpret_cast<LPARAM>(&c));
    impl->hwnd = c.best;
    return impl->hwnd != nullptr;
}

bool WindowCapture::HasWindow() const { return impl->hwnd && IsWindow(impl->hwnd); }

std::string WindowCapture::WindowTitle() const {
    if (!HasWindow()) return {};
    wchar_t title[512] = {0};
    GetWindowTextW(impl->hwnd, title, 512);
    return ToUtf8(title);
}

bool WindowCapture::Grab(std::vector<uint8_t>& bgra, int& w, int& h, int timeout_ms, std::string* error) {
    if (!HasWindow()) {
        impl->hwnd = nullptr;
        return false;
    }
    if (IsIconic(impl->hwnd)) {   // minimized: nothing is drawn on screen
        Sleep(static_cast<DWORD>(timeout_ms));
        return false;
    }
    const HMONITOR mon = MonitorFromWindow(impl->hwnd, MONITOR_DEFAULTTONEAREST);
    if (!impl->dup || mon != impl->monitor) {
        if (!impl->OpenDuplication(mon, error)) {
            Sleep(static_cast<DWORD>(timeout_ms));
            return false;
        }
    }

    DXGI_OUTDUPL_FRAME_INFO info;
    IDXGIResource* res = nullptr;
    HRESULT hr = impl->dup->AcquireNextFrame(static_cast<UINT>(timeout_ms), &info, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (FAILED(hr)) {   // ACCESS_LOST: mode change, UAC prompt, fullscreen switch... reopen next time
        impl->ReleaseAll();
        return false;
    }
    bool ok = false;
    ID3D11Texture2D* tex = nullptr;
    if (info.LastPresentTime.QuadPart != 0 &&   // 0 = only the mouse moved
        SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex)))) {
        RECT g = impl->GameRect();
        const RECT& d = impl->desktop;
        // Into output coordinates, clipped to this monitor.
        long x0 = std::max(g.left, d.left) - d.left, y0 = std::max(g.top, d.top) - d.top;
        long x1 = std::min(g.right, d.right) - d.left, y1 = std::min(g.bottom, d.bottom) - d.top;
        const int cw = static_cast<int>((x1 - x0) & ~1L), ch = static_cast<int>((y1 - y0) & ~1L);
        if (cw >= 64 && ch >= 36) {
            if (!impl->staging || impl->staging_w != cw || impl->staging_h != ch) {
                SafeRelease(impl->staging);
                D3D11_TEXTURE2D_DESC td{};
                td.Width = static_cast<UINT>(cw);
                td.Height = static_cast<UINT>(ch);
                td.MipLevels = 1;
                td.ArraySize = 1;
                td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_STAGING;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (SUCCEEDED(impl->dev->CreateTexture2D(&td, nullptr, &impl->staging))) {
                    impl->staging_w = cw;
                    impl->staging_h = ch;
                }
            }
            if (impl->staging) {
                const D3D11_BOX box{static_cast<UINT>(x0), static_cast<UINT>(y0), 0,
                                    static_cast<UINT>(x0 + cw), static_cast<UINT>(y0 + ch), 1};
                impl->ctx->CopySubresourceRegion(impl->staging, 0, 0, 0, 0, tex, 0, &box);
                ok = true;
            }
        }
    }
    SafeRelease(tex);
    SafeRelease(res);
    impl->dup->ReleaseFrame();
    if (!ok) return false;

    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(impl->ctx->Map(impl->staging, 0, D3D11_MAP_READ, 0, &m))) return false;
    w = impl->staging_w;
    h = impl->staging_h;
    bgra.resize(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; y++)
        std::memcpy(bgra.data() + static_cast<size_t>(y) * w * 4,
                    static_cast<const uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                    static_cast<size_t>(w) * 4);
    impl->ctx->Unmap(impl->staging, 0);
    return true;
}
