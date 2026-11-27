// SPDX-License-Identifier: GPL-3.0-only
// One window, by itself: Windows Graphics Capture hands over what the window draws, also when other windows (or this program,
// full screen) cover it. The toolchain has no headers for it, so the few interfaces used are declared here (Windows SDK layout).
#include "screen.h"
#include "video.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>
#include <inspectable.h>
#include <winstring.h>
#include <algorithm>

namespace wgc
{
struct SizeInt32 { int32_t Width, Height; };
struct Token { int64_t value; };
static const GUID IID_ItemInterop = {0x3628E81B, 0x3CAC, 0x4C60, {0xB7, 0xF4, 0x23, 0xCE, 0x0E, 0x0C, 0x33, 0x56}};
static const GUID IID_Item = {0x79C3F95B, 0x31F7, 0x4EC2, {0xA4, 0x64, 0x63, 0x2E, 0xF5, 0xD3, 0x07, 0x60}};
static const GUID IID_PoolStatics2 = {0x589B103F, 0x6BBC, 0x5DF5, {0xA9, 0x91, 0x02, 0xE2, 0x8B, 0x3B, 0x66, 0xD5}};
static const GUID IID_Session2 = {0x2C39AE40, 0x7D2E, 0x5044, {0x80, 0x4E, 0x8B, 0x67, 0x99, 0xD4, 0xCF, 0x9E}};
static const GUID IID_Session3 = {0xF2CDD966, 0x22AE, 0x5EA1, {0x95, 0x96, 0x3A, 0x28, 0x93, 0x44, 0xC3, 0xBE}};
static const GUID IID_Access = {0xA9B3D012, 0x3DF2, 0x4EE3, {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1}};
static const GUID IID_Closable = {0x30D5A829, 0x7FA4, 0x4026, {0x83, 0xBB, 0xD7, 0x5B, 0xAE, 0x4E, 0xA9, 0x9E}};
static const GUID IID_Handler = {0x51A947F7, 0x79CF, 0x5A3E, {0xA3, 0xA5, 0x12, 0x89, 0xCF, 0xA6, 0xDF, 0xE8}}; // TypedEventHandler<Direct3D11CaptureFramePool, Object>
static const GUID IID_Agile = {0x94EA2B94, 0xE9CC, 0x49E0, {0xC0, 0xFF, 0xEE, 0x64, 0xCA, 0x8F, 0x5B, 0x90}};

struct ItemInterop : IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE CreateForWindow(HWND w, REFIID iid, void **out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateForMonitor(HMONITOR m, REFIID iid, void **out) = 0;
};
struct Item : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_DisplayName(HSTRING *v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(SizeInt32 *v) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_Closed(void *h, Token *t) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_Closed(Token t) = 0;
};
struct Frame : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Surface(IInspectable **v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SystemRelativeTime(int64_t *v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ContentSize(SizeInt32 *v) = 0;
};
struct Session : IInspectable { virtual HRESULT STDMETHODCALLTYPE StartCapture() = 0; };
struct Session2 : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_IsCursorCaptureEnabled(boolean *v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsCursorCaptureEnabled(boolean v) = 0;
};
struct Session3 : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_IsBorderRequired(boolean *v) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsBorderRequired(boolean v) = 0;
};
struct Pool : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE Recreate(IInspectable *device, int32_t format, int32_t buffers, SizeInt32 size) = 0;
    virtual HRESULT STDMETHODCALLTYPE TryGetNextFrame(Frame **v) = 0;
    virtual HRESULT STDMETHODCALLTYPE add_FrameArrived(IUnknown *h, Token *t) = 0;
    virtual HRESULT STDMETHODCALLTYPE remove_FrameArrived(Token t) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateCaptureSession(Item *item, Session **v) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_DispatcherQueue(IInspectable **v) = 0;
};
struct PoolStatics2 : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE CreateFreeThreaded(IInspectable *device, int32_t format, int32_t buffers, SizeInt32 size, Pool **v) = 0;
};
struct Access : IUnknown { virtual HRESULT STDMETHODCALLTYPE GetInterface(REFIID iid, void **out) = 0; };
struct Closable : IInspectable { virtual HRESULT STDMETHODCALLTYPE Close() = 0; };

// FrameArrived: only wakes the capture thread
struct Wake final : IUnknown
{
    HANDLE ev;
    volatile LONG refs = 1;
    explicit Wake(HANDLE e) : ev(e) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override
    {
        if (iid == IID_Handler || iid == __uuidof(IUnknown) || iid == IID_Agile) { *out = this; AddRef(); return S_OK; }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&refs); }
    ULONG STDMETHODCALLTYPE Release() override { LONG n = InterlockedDecrement(&refs); if (!n) delete this; return (ULONG)n; }
    virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable *, IInspectable *) { SetEvent(ev); return S_OK; }
};

struct Api
{
    HRESULT(WINAPI *factory)(HSTRING, REFIID, void **) = nullptr;
    HRESULT(WINAPI *make)(PCWSTR, UINT32, HSTRING *) = nullptr;
    HRESULT(WINAPI *drop)(HSTRING) = nullptr;
    HRESULT(WINAPI *init)(int) = nullptr;
    HRESULT(WINAPI *device)(IDXGIDevice *, IInspectable **) = nullptr;
    bool ok = false;
    Api()
    {
        HMODULE c = LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32), d = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!c || !d) return;
        factory = reinterpret_cast<decltype(factory)>(reinterpret_cast<void *>(GetProcAddress(c, "RoGetActivationFactory")));
        make = reinterpret_cast<decltype(make)>(reinterpret_cast<void *>(GetProcAddress(c, "WindowsCreateString")));
        drop = reinterpret_cast<decltype(drop)>(reinterpret_cast<void *>(GetProcAddress(c, "WindowsDeleteString")));
        init = reinterpret_cast<decltype(init)>(reinterpret_cast<void *>(GetProcAddress(c, "RoInitialize")));
        device = reinterpret_cast<decltype(device)>(reinterpret_cast<void *>(GetProcAddress(d, "CreateDirect3D11DeviceFromDXGIDevice")));
        ok = factory && make && drop && init && device;
    }
    template <class T> HRESULT get(const wchar_t *cls, REFIID iid, T **out)
    {
        HSTRING s = nullptr;
        HRESULT hr = make(cls, (UINT32)wcslen(cls), &s);
        if (SUCCEEDED(hr)) hr = factory(s, iid, (void **)out);
        drop(s);
        return hr;
    }
};
} // namespace wgc

template <class T> static void out_(T *&p) { if (p) { p->Release(); p = nullptr; } }
template <class T> static void close_(T *&p)
{
    if (!p) return;
    wgc::Closable *c = nullptr;
    if (SUCCEEDED(p->QueryInterface(wgc::IID_Closable, (void **)&c))) { c->Close(); c->Release(); }
    out_(p);
}

// Runs until stop: finds the window (again and again while it is not there), takes its inside, hands it to the sink.
void screen_window_loop(HWND (*find)(const std::wstring &), const std::wstring &key, volatile LONG *stop, CaptureSink *sink, void (*set_format)(void *, const VideoFormat &), void *self)
{
    static wgc::Api api;
    if (!api.ok) { logf("wgc: not on this Windows"); return; }
    api.init(1); // multithreaded
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::vector<uint8_t> packed;
    double gaps[32] = {};
    int gap_n = 0, said = 0;
    LONGLONG last = 0;
    while (!*stop)
    {
        HWND hw = find(key);
        if (!hw || IsIconic(hw)) { Sleep(300); continue; }
        ID3D11Device *dev = nullptr;
        ID3D11DeviceContext *ctx = nullptr;
        IDXGIDevice *dx = nullptr;
        IInspectable *rdev = nullptr;
        wgc::ItemInterop *interop = nullptr;
        wgc::Item *item = nullptr;
        wgc::PoolStatics2 *statics = nullptr;
        wgc::Pool *pool = nullptr;
        wgc::Session *session = nullptr;
        wgc::Wake *wake = nullptr;
        wgc::Token tok{};
        bool added = false;
        HRESULT hr = D3D11CreateDevice(video_adapter(), video_adapter() ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
        if (SUCCEEDED(hr)) hr = dev->QueryInterface(__uuidof(IDXGIDevice), (void **)&dx);
        if (SUCCEEDED(hr)) hr = api.device(dx, &rdev);
        if (SUCCEEDED(hr)) hr = api.get(L"Windows.Graphics.Capture.GraphicsCaptureItem", wgc::IID_ItemInterop, &interop);
        if (SUCCEEDED(hr)) hr = interop->CreateForWindow(hw, wgc::IID_Item, (void **)&item);
        wgc::SizeInt32 size{};
        if (SUCCEEDED(hr)) hr = item->get_Size(&size);
        if (SUCCEEDED(hr)) hr = api.get(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", wgc::IID_PoolStatics2, &statics);
        if (SUCCEEDED(hr)) hr = statics->CreateFreeThreaded(rdev, 87 /* B8G8R8A8 */, 3, size, &pool);
        if (SUCCEEDED(hr)) hr = pool->CreateCaptureSession(item, &session);
        if (SUCCEEDED(hr))
        {
            wake = new wgc::Wake(ev);
            added = SUCCEEDED(pool->add_FrameArrived(wake, &tok));
            wgc::Session2 *s2 = nullptr;
            if (SUCCEEDED(session->QueryInterface(wgc::IID_Session2, (void **)&s2))) { s2->put_IsCursorCaptureEnabled(false); s2->Release(); }
            wgc::Session3 *s3 = nullptr;
            if (SUCCEEDED(session->QueryInterface(wgc::IID_Session3, (void **)&s3))) { s3->put_IsBorderRequired(false); s3->Release(); } // no yellow frame
            hr = session->StartCapture();
        }
        if (FAILED(hr))
        {
            if (said++ < 5) logf("wgc: start failed 0x%08lx", (unsigned long)hr);
        }
        else
        {
            logf("wgc: window %dx%d%s", size.Width, size.Height, added ? "" : " (polled)");
            ID3D11Texture2D *stage = nullptr;
            int sw = 0, sh = 0;
            bool have = false;
            LONGLONG sent = 0;
            while (!*stop)
            {
                if (!IsWindow(hw)) break;
                WaitForSingleObject(ev, added ? 100 : 4);
                wgc::Frame *f = nullptr;
                for (wgc::Frame *g = nullptr; SUCCEEDED(pool->TryGetNextFrame(&g)) && g;) { out_(f); f = g; g = nullptr; } // all that waits: the newest
                if (!f)
                {
                    if (have && ms_of(qpc() - sent) > 250) // nothing new (minimized, or a still picture): the last one again, so the input stays alive
                    {
                        VideoFormat v;
                        v.pixel = Pixel::bgra; v.w = sw; v.h = sh; v.fps = 60; v.full = true;
                        sink->on_video(packed.data(), packed.size(), v, qpc(), -1);
                        sent = qpc();
                    }
                    continue;
                }
                wgc::SizeInt32 cs{};
                int64_t t100 = 0;
                f->get_ContentSize(&cs);
                f->get_SystemRelativeTime(&t100);
                IInspectable *surf = nullptr;
                wgc::Access *acc = nullptr;
                ID3D11Texture2D *tex = nullptr;
                if (SUCCEEDED(f->get_Surface(&surf)) && SUCCEEDED(surf->QueryInterface(wgc::IID_Access, (void **)&acc))) acc->GetInterface(__uuidof(ID3D11Texture2D), (void **)&tex);
                out_(acc);
                out_(surf);
                if (cs.Width != size.Width || cs.Height != size.Height) // the window changed its size: the pool follows
                {
                    size = cs;
                    pool->Recreate(rdev, 87, 3, size);
                }
                // the inside of the window: the picture starts at the window's frame as DWM draws it
                RECT frame{}, client{};
                POINT o = {0, 0};
                DwmGetWindowAttribute(hw, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof frame);
                GetClientRect(hw, &client);
                ClientToScreen(hw, &o);
                int x0 = std::max(0L, o.x - frame.left), y0 = std::max(0L, o.y - frame.top);
                int w = std::min((int)client.right, cs.Width - x0) & ~7, h = std::min((int)client.bottom, cs.Height - y0) & ~1;
                if (!tex || w < 64 || h < 64) { out_(tex); out_(f); continue; }
                if (w != sw || h != sh)
                {
                    out_(stage);
                    D3D11_TEXTURE2D_DESC d{};
                    d.Width = (UINT)w;
                    d.Height = (UINT)h;
                    d.MipLevels = d.ArraySize = 1;
                    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                    d.SampleDesc.Count = 1;
                    d.Usage = D3D11_USAGE_STAGING;
                    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    if (FAILED(dev->CreateTexture2D(&d, nullptr, &stage))) { out_(tex); out_(f); break; }
                    sw = w;
                    sh = h;
                    gap_n = 0;
                }
                D3D11_BOX box = {(UINT)x0, (UINT)y0, 0, (UINT)(x0 + w), (UINT)(y0 + h), 1};
                ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, tex, 0, &box);
                out_(tex);
                D3D11_MAPPED_SUBRESOURCE m{};
                hr = ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m);
                out_(f);
                if (FAILED(hr)) break;
                LONGLONG when = (LONGLONG)((double)t100 * (double)qpc_freq / 1e7);
                if (last && when > last) { double g = ms_of(when - last); if (g > 2 && g < 200) gaps[gap_n++ & 31] = g; }
                last = when;
                double fps = 60;
                if (gap_n >= 8)
                {
                    int n = std::min(gap_n, 32);
                    double s[32];
                    std::copy(gaps, gaps + n, s);
                    std::nth_element(s, s + n / 2, s + n);
                    fps = 1000.0 / s[n / 2];
                    for (double r : {24.0, 30.0, 50.0, 60.0, 72.0, 90.0, 120.0, 144.0, 165.0, 240.0})
                        if (fabs(fps / r - 1) < 0.06) { fps = r; break; }
                }
                VideoFormat v;
                v.pixel = Pixel::bgra; v.w = w; v.h = h; v.fps = fps; v.full = true;
                set_format(self, v);
                packed.resize((size_t)w * h * 4);
                for (int y = 0; y < h; y++) memcpy(&packed[(size_t)y * w * 4], (const uint8_t *)m.pData + (size_t)m.RowPitch * y, (size_t)w * 4);
                ctx->Unmap(stage, 0);
                sink->on_video(packed.data(), packed.size(), v, when > 0 && when <= qpc() ? when : qpc(), -1);
                have = true;
                sent = qpc();
            }
            out_(stage);
        }
        if (added) pool->remove_FrameArrived(tok);
        out_(wake);
        close_(session);
        close_(pool);
        out_(statics);
        out_(item);
        out_(interop);
        out_(rdev);
        out_(dx);
        out_(ctx);
        out_(dev);
        if (!*stop) Sleep(300);
    }
    CloseHandle(ev);
}
