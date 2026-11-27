// SPDX-License-Identifier: GPL-3.0-only
// A monitor or one window as the input. Desktop Duplication hands over the monitor's picture whenever something on it changed;
// for a window only its part is copied. Our own windows are excluded from every capture, so the program can show its picture on
// the same monitor, over the window it takes the picture from. The sound is what the PC plays (loopback), for recordings only:
// the PC already plays it itself.
#include "screen.h"
#include "video.h"
#include <d3d11.h>
#include <dxgi1_6.h>
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <algorithm>
#include <string>

// tests (PKAPP_SCRTIME): how long each step of a screen picture takes
struct StepTimes
{
    double sum[5] = {}; int n = 0; const char *name;
    explicit StepTimes(const char *s) : name(s) {}
    void add(LONGLONG a, LONGLONG b, LONGLONG c, LONGLONG d, LONGLONG e, LONGLONG when)
    {
        static int on = GetEnvironmentVariableW(L"PKAPP_SCRTIME", nullptr, 0) ? 1 : 0;
        if (!on) return;
        sum[0] += ms_of(b - a); sum[1] += ms_of(c - b); sum[2] += ms_of(d - c); sum[3] += ms_of(e - d); sum[4] += ms_of(a - when);
        if (++n == 120) { logf("%s: copy+map %.2f  pack %.2f  hand over %.2f  total %.2f  shown->got %.2f ms", name, sum[0] / n, sum[1] / n, sum[2] / n, (sum[0] + sum[1] + sum[2]) / n, sum[4] / n); for (auto &x : sum) x = 0; n = 0; }
        (void)e;
    }
};
bool screen_path(const std::wstring &p) { return p.rfind(L"screen:", 0) == 0 || p.rfind(L"window:", 0) == 0; }

static std::wstring exe_of(HWND w);
std::wstring screen_exe(HWND w) { return exe_of(w); }
static std::wstring exe_of(HWND w)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t s[MAX_PATH];
    DWORD n = MAX_PATH;
    std::wstring r;
    if (QueryFullProcessImageNameW(h, 0, s, &n))
    {
        r = s;
        size_t k = r.find_last_of(L"\\/");
        if (k != std::wstring::npos) r = r.substr(k + 1);
        for (auto &c : r) c = (wchar_t)towlower(c);
    }
    CloseHandle(h);
    return r;
}
// A window a person would take the picture of: shown, not hidden by Windows, with a title and some size, not ours.
static bool usable(HWND w, bool listing)
{
    if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER) || (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return false;
    if (listing && IsIconic(w)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) return false;
    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked) return false;
    if (GetWindowTextLengthW(w) == 0) return false;
    RECT r;
    GetClientRect(w, &r);
    return !listing || (r.right >= 480 && r.bottom >= 270);
}
// key: "program.exe|title". The window with that title, otherwise the largest one of that program (titles change).
static HWND find_window(const std::wstring &key)
{
    size_t bar = key.find(L'|');
    struct S { std::wstring exe, title; HWND best; LONG area; } s = {key.substr(0, bar), bar == std::wstring::npos ? L"" : key.substr(bar + 1), nullptr, -1};
    EnumWindows([](HWND w, LPARAM p) -> BOOL {
        S &s = *(S *)p;
        if (!usable(w, false) || exe_of(w) != s.exe) return TRUE;
        RECT r;
        GetClientRect(w, &r);
        wchar_t t[256];
        GetWindowTextW(w, t, 256);
        LONG a = (IsIconic(w) ? 0 : r.right * r.bottom + 1) + (!s.title.empty() && s.title == t ? 1 << 30 : 0);
        if (a > s.area) { s.area = a; s.best = w; }
        return TRUE;
    }, (LPARAM)&s);
    return s.best;
}

std::vector<Device> screen_devices()
{
    std::vector<Device> out;
    IDXGIFactory1 *f = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f)))
    {
        int n = 0;
        IDXGIAdapter1 *a = nullptr;
        for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++)
        {
            IDXGIOutput *o = nullptr;
            for (UINT k = 0; a->EnumOutputs(k, &o) != DXGI_ERROR_NOT_FOUND; k++)
            {
                DXGI_OUTPUT_DESC d;
                if (SUCCEEDED(o->GetDesc(&d)) && d.AttachedToDesktop)
                {
                    MONITORINFO mi = {sizeof mi};
                    GetMonitorInfoW(d.Monitor, &mi);
                    int w = d.DesktopCoordinates.right - d.DesktopCoordinates.left, h = d.DesktopCoordinates.bottom - d.DesktopCoordinates.top;
                    wchar_t s[160];
                    ++n;
                    if (mi.dwFlags & MONITORINFOF_PRIMARY) swprintf(s, 160, tr(L"주 모니터 · %d×%d", L"Main monitor · %d×%d"), w, h);
                    else swprintf(s, 160, tr(L"모니터 %d · %d×%d", L"Monitor %d · %d×%d"), n, w, h);
                    out.push_back({s, std::wstring(L"screen:") + d.DeviceName});
                }
                o->Release();
            }
            a->Release();
        }
        f->Release();
    }
    // windows, as OBS lists them: [program.exe] title
    std::vector<Device> wins;
    EnumWindows([](HWND w, LPARAM p) -> BOOL {
        auto &wins = *(std::vector<Device> *)p;
        if (!usable(w, true)) return TRUE;
        std::wstring exe = exe_of(w);
        if (exe.empty() || exe == L"explorer.exe" || exe == L"textinputhost.exe" || exe == L"searchhost.exe" || exe == L"shellexperiencehost.exe" || exe == L"startmenuexperiencehost.exe") return TRUE;
        wchar_t t[256];
        GetWindowTextW(w, t, 256);
        std::wstring title = t, shown = title.size() > 40 ? title.substr(0, 39) + L"…" : title;
        wins.push_back({L"[" + exe + L"] " + shown, L"window:" + exe + L"|" + title});
        return TRUE;
    }, (LPARAM)&wins);
    out.insert(out.end(), wins.begin(), wins.end());
    return out;
}

void screen_exclude_own(bool on)
{
    static int had = -1;
    if (GetEnvironmentVariableW(L"PKAPP_NO_EXCLUDE", nullptr, 0)) on = false; // tests: screenshots of the window
    struct S { DWORD how; } s = {on ? 0x11u /* WDA_EXCLUDEFROMCAPTURE */ : 0u};
    EnumWindows([](HWND w, LPARAM p) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == GetCurrentProcessId())
        {
            DWORD now = 0;
            GetWindowDisplayAffinity(w, &now);
            if (now != ((S *)p)->how) SetWindowDisplayAffinity(w, ((S *)p)->how);
        }
        return TRUE;
    }, (LPARAM)&s);
    if (had != (int)on) { had = on; logf("screen: own windows %s capture", on ? "left out of" : "back in"); }
}

// ---- picture ----
RECT ScreenCapture::area() const
{
    AcquireSRWLockShared(&fl);
    RECT r = taken;
    ReleaseSRWLockShared(&fl);
    return r;
}
VideoFormat ScreenCapture::video() const
{
    AcquireSRWLockShared(&fl);
    VideoFormat f = vf;
    ReleaseSRWLockShared(&fl);
    return f;
}
std::wstring ScreenCapture::open(const std::wstring &p, bool, CaptureSink *s)
{
    close();
    path = p;
    sink = s;
    stop = 0;
    vf = VideoFormat();
    af = AudioFormat();
    taken = RECT{};
    if (p.rfind(L"window:", 0) == 0 && !find_window(p.substr(7)))
        return tr(L"고른 창이 열려 있지 않아요. 그 프로그램을 켜면 자동으로 받아요", L"The chosen window is not open. It is taken as soon as the program runs");
    // the sound the PC plays: its rate decides the recording's sound
    IMMDeviceEnumerator *e = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&e)))
    {
        IMMDevice *d = nullptr;
        IAudioClient *c = nullptr;
        WAVEFORMATEX *mix = nullptr;
        if (SUCCEEDED(e->GetDefaultAudioEndpoint(eRender, eConsole, &d)) && SUCCEEDED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&c)) && SUCCEEDED(c->GetMixFormat(&mix)))
            af = {(int)mix->nSamplesPerSec, 2, 16};
        if (mix) CoTaskMemFree(mix);
        if (c) c->Release();
        if (d) d->Release();
        e->Release();
    }
    thread = CreateThread(nullptr, 0, run, this, 0, nullptr);
    if (af.rate) sound = CreateThread(nullptr, 0, run_sound, this, 0, nullptr);
    logf("screen: open %ls", p.c_str());
    return L"";
}
void ScreenCapture::close()
{
    if (!thread) return;
    InterlockedExchange(&stop, 1);
    WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    thread = nullptr;
    if (sound) { WaitForSingleObject(sound, 3000); CloseHandle(sound); sound = nullptr; }
    if (sink) sink->on_stop();
    logf("screen: closed");
}
DWORD WINAPI ScreenCapture::run(void *self) { ((ScreenCapture *)self)->loop(); return 0; }
DWORD WINAPI ScreenCapture::run_sound(void *self) { CoInitializeEx(nullptr, COINIT_MULTITHREADED); ((ScreenCapture *)self)->sound_loop(); CoUninitialize(); return 0; }

template <class T> static void drop(T *&p) { if (p) { p->Release(); p = nullptr; } }

void screen_window_loop(HWND (*find)(const std::wstring &), const std::wstring &key, volatile LONG *stop, CaptureSink *sink, void (*set_format)(void *, const VideoFormat &), void *self); // wgc.cpp
void ScreenCapture::loop()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    bool win = path.rfind(L"window:", 0) == 0;
    std::wstring key = path.substr(7);
    if (win) // the window by itself (Windows Graphics Capture), not the part of the monitor it lies on
    {
        screen_window_loop(find_window, key, &stop, sink, [](void *me, const VideoFormat &f) {
            ScreenCapture *c = (ScreenCapture *)me;
            AcquireSRWLockExclusive(&c->fl);
            c->vf = f;
            ReleaseSRWLockExclusive(&c->fl);
        }, this);
        return;
    }
    std::vector<uint8_t> packed, meta;
    double gaps[32] = {};
    int gap_n = 0;
    LONGLONG last_present = 0;
    int said = 0;
    while (!stop)
    {
        HWND hw = nullptr;
        HMONITOR mon = nullptr;
        if (win)
        {
            hw = find_window(key);
            if (!hw || IsIconic(hw)) { Sleep(300); continue; }
            mon = MonitorFromWindow(hw, MONITOR_DEFAULTTONEAREST);
        }
        // the output (and its graphics card) that shows it
        IDXGIFactory1 *f = nullptr;
        IDXGIAdapter1 *ad = nullptr;
        IDXGIOutput1 *out = nullptr;
        DXGI_OUTPUT_DESC od{};
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f)))
        {
            IDXGIAdapter1 *a = nullptr;
            for (UINT i = 0; !out && f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++)
            {
                IDXGIOutput *o = nullptr;
                for (UINT k = 0; !out && a->EnumOutputs(k, &o) != DXGI_ERROR_NOT_FOUND; k++)
                {
                    DXGI_OUTPUT_DESC d;
                    if (SUCCEEDED(o->GetDesc(&d)) && d.AttachedToDesktop && (win ? d.Monitor == mon : key == d.DeviceName))
                    {
                        o->QueryInterface(__uuidof(IDXGIOutput1), (void **)&out);
                        od = d;
                        if (out) { ad = a; a->AddRef(); }
                    }
                    o->Release();
                }
                a->Release();
            }
            f->Release();
        }
        ID3D11Device *dev = nullptr;
        ID3D11DeviceContext *ctx = nullptr;
        IDXGIOutputDuplication *dup = nullptr;
        // The monitor of the frame device's graphics card, not turned: its pictures stay on the card (direct). Otherwise a device of
        // its own, and the pictures go through the CPU.
        bool direct = false;
        if (out && video_device() && video_adapter())
        {
            DXGI_ADAPTER_DESC1 a1, a2;
            ad->GetDesc1(&a1);
            video_adapter()->GetDesc1(&a2);
            direct = a1.AdapterLuid.LowPart == a2.AdapterLuid.LowPart && a1.AdapterLuid.HighPart == a2.AdapterLuid.HighPart && od.Rotation <= DXGI_MODE_ROTATION_IDENTITY;
        }
        if (out)
        {
            D3D_FEATURE_LEVEL fl11 = D3D_FEATURE_LEVEL_11_0;
            if (direct) { dev = video_device(); dev->AddRef(); }
            if (direct || SUCCEEDED(D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl11, 1, D3D11_SDK_VERSION, &dev, nullptr, &ctx)))
            {
                IDXGIOutput5 *o5 = nullptr;
                HRESULT hr = E_FAIL;
                if (SUCCEEDED(out->QueryInterface(__uuidof(IDXGIOutput5), (void **)&o5)))
                {
                    DXGI_FORMAT fm = DXGI_FORMAT_B8G8R8A8_UNORM;
                    hr = o5->DuplicateOutput1(dev, 0, 1, &fm, &dup);
                    o5->Release();
                }
                if (FAILED(hr)) hr = out->DuplicateOutput(dev, &dup);
                if (FAILED(hr) && said++ < 5) logf("screen: duplication failed 0x%08lx", (unsigned long)hr);
            }
        }
        drop(out);
        drop(ad);
        if (!dup) { drop(ctx); drop(dev); Sleep(500); continue; }
        DXGI_OUTDUPL_DESC dd;
        dup->GetDesc(&dd);
        // a monitor turned on its side (or upside down): its picture comes as the panel has it, and is turned back here
        int turn = dd.Rotation == DXGI_MODE_ROTATION_ROTATE90 ? 90 : dd.Rotation == DXGI_MODE_ROTATION_ROTATE180 ? 180 : dd.Rotation == DXGI_MODE_ROTATION_ROTATE270 ? 270 : 0;
        LONG dw = od.DesktopCoordinates.right - od.DesktopCoordinates.left, dh = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
        if (turn) direct = false;
        logf("screen: %ls %ux%u rotation %d%s", od.DeviceName, dd.ModeDesc.Width, dd.ModeDesc.Height, (int)dd.Rotation, direct ? " (on the card)" : "");
        ID3D11Texture2D *stage = nullptr;
        int sw = 0, sh = 0, tw = 0, th = 0; // tw, th: the staging copy of a turned monitor (all of it)
        bool have = false;
        LONGLONG sent = 0;
        while (!stop)
        {
            // the part to take: the whole monitor, or the window's inside
            RECT want = {0, 0, dw, dh}; // as the desktop has it
            if (win)
            {
                if (!IsWindow(hw) || MonitorFromWindow(hw, MONITOR_DEFAULTTONEAREST) != mon) break; // gone or moved to another monitor
                RECT c;
                GetClientRect(hw, &c);
                POINT p = {0, 0};
                ClientToScreen(hw, &p);
                RECT r = {p.x - od.DesktopCoordinates.left, p.y - od.DesktopCoordinates.top, 0, 0};
                r.right = r.left + c.right;
                r.bottom = r.top + c.bottom;
                IntersectRect(&want, &want, &r);
            }
            int w = (int)(want.right - want.left) & ~7, h = (int)(want.bottom - want.top) & ~1;
            DXGI_OUTDUPL_FRAME_INFO fi{};
            IDXGIResource *res = nullptr;
            // On the frame device a wait inside AcquireNextFrame would hold the device (and the generation with it): it only looks, and
            // the thread sleeps a millisecond between looks.
            HRESULT hr = dup->AcquireNextFrame(direct ? 0 : 100, &fi, &res);
            if (direct && hr == DXGI_ERROR_WAIT_TIMEOUT) Sleep(1);
            if (hr == DXGI_ERROR_WAIT_TIMEOUT || (SUCCEEDED(hr) && fi.LastPresentTime.QuadPart == 0))
            {
                if (SUCCEEDED(hr)) { res->Release(); dup->ReleaseFrame(); }
                // nothing new: the last picture again now and then, so the input counts as alive
                if (have && direct && ms_of(qpc() - sent) > 250) { sink->on_alive(qpc()); sent = qpc(); }
                else if (have && turn && ms_of(qpc() - sent) > 250)
                {
                    VideoFormat vv = video();
                    sink->on_video(packed.data(), (size_t)sw * sh * 4, vv, qpc(), -1);
                    sent = qpc();
                }
                else if (have && ms_of(qpc() - sent) > 250)
                {
                    D3D11_MAPPED_SUBRESOURCE m{};
                    if (SUCCEEDED(ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m)))
                    {
                        const uint8_t *data = (const uint8_t *)m.pData;
                        if (m.RowPitch != (UINT)sw * 4)
                        {
                            packed.resize((size_t)sw * sh * 4);
                            for (int y = 0; y < sh; y++) memcpy(&packed[(size_t)y * sw * 4], data + (size_t)m.RowPitch * y, (size_t)sw * 4);
                            data = packed.data();
                        }
                        VideoFormat vv = video();
                        sink->on_video(data, (size_t)sw * sh * 4, vv, qpc(), -1);
                        ctx->Unmap(stage, 0);
                    }
                    sent = qpc();
                }
                continue;
            }
            if (FAILED(hr)) { logf("screen: frame 0x%08lx", (unsigned long)hr); break; } // mode change, secure desktop, ...
            // only when the part we take has changed (our own picture is left out, but still counts as an update)
            bool changed = true;
            if (fi.TotalMetadataBufferSize && !turn) // the rectangles are those of the panel: only checked when it is not turned
            {
                meta.resize(fi.TotalMetadataBufferSize);
                UINT used = 0;
                if (SUCCEEDED(dup->GetFrameDirtyRects((UINT)meta.size(), (RECT *)meta.data(), &used)))
                {
                    changed = false;
                    RECT *rs = (RECT *)meta.data(), x;
                    for (UINT i = 0; i < used / sizeof(RECT) && !changed; i++) changed = IntersectRect(&x, &rs[i], &want) != 0;
                }
                if (!changed)
                {
                    DXGI_OUTDUPL_MOVE_RECT *mv = (DXGI_OUTDUPL_MOVE_RECT *)meta.data();
                    if (SUCCEEDED(dup->GetFrameMoveRects((UINT)meta.size(), mv, &used)))
                        for (UINT i = 0; i < used / sizeof *mv && !changed; i++) { RECT x; changed = IntersectRect(&x, &mv[i].DestinationRect, &want) != 0; }
                }
            }
            if (!changed || w < 64 || h < 64) { res->Release(); dup->ReleaseFrame(); continue; }
            ID3D11Texture2D *t = nullptr;
            res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t);
            res->Release();
            if (!t) { dup->ReleaseFrame(); continue; }
            if (direct) // straight into the frame device's input
            {
                LONGLONG when = fi.LastPresentTime.QuadPart;
                if (last_present && when > last_present) { double g = ms_of(when - last_present); if (g > 2 && g < 200) gaps[gap_n++ & 31] = g; }
                last_present = when;
                if (w != sw || h != sh) { sw = w; sh = h; gap_n = 0; }
                double fps = 60;
                if (gap_n >= 8)
                {
                    int n = std::min(gap_n, 32);
                    double q[32];
                    std::copy(gaps, gaps + n, q);
                    std::nth_element(q, q + n / 2, q + n);
                    fps = 1000.0 / q[n / 2];
                    for (double r : {24.0, 30.0, 50.0, 60.0, 72.0, 90.0, 120.0, 144.0, 165.0, 240.0})
                        if (fabs(fps / r - 1) < 0.06) { fps = r; break; }
                }
                VideoFormat nf;
                nf.pixel = Pixel::bgra; nf.w = w; nf.h = h; nf.fps = fps; nf.full = true;
                AcquireSRWLockExclusive(&fl);
                vf = nf;
                taken = {od.DesktopCoordinates.left + want.left, od.DesktopCoordinates.top + want.top, od.DesktopCoordinates.left + want.left + w, od.DesktopCoordinates.top + want.top + h};
                ReleaseSRWLockExclusive(&fl);
                D3D11_BOX box = {(UINT)want.left, (UINT)want.top, 0, (UINT)(want.left + w), (UINT)(want.top + h), 1};
                LONGLONG t0 = qpc();
                sink->on_texture(t, box, nf, std::min(when, qpc()));
                t->Release();
                dup->ReleaseFrame();
                static StepTimes st("screen times");
                st.add(t0, t0, t0, qpc(), qpc(), when);
                have = true;
                sent = qpc();
                continue;
            }
            D3D11_TEXTURE2D_DESC td;
            t->GetDesc(&td);
            int cw = turn ? (int)td.Width : w, ch = turn ? (int)td.Height : h; // what is copied: all of a turned monitor
            if (w != sw || h != sh || cw != tw || ch != th)
            {
                drop(stage);
                D3D11_TEXTURE2D_DESC d{};
                d.Width = (UINT)cw;
                d.Height = (UINT)ch;
                d.MipLevels = d.ArraySize = 1;
                d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                d.SampleDesc.Count = 1;
                d.Usage = D3D11_USAGE_STAGING;
                d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(dev->CreateTexture2D(&d, nullptr, &stage))) { t->Release(); dup->ReleaseFrame(); break; }
                sw = w;
                sh = h;
                tw = cw;
                th = ch;
                have = false;
                gap_n = 0;
            }
            LONGLONG t0 = qpc();
            if (turn) ctx->CopyResource(stage, t);
            else
            {
                D3D11_BOX box = {(UINT)want.left, (UINT)want.top, 0, (UINT)(want.left + w), (UINT)(want.top + h), 1};
                ctx->CopySubresourceRegion(stage, 0, 0, 0, 0, t, 0, &box);
            }
            t->Release();
            D3D11_MAPPED_SUBRESOURCE m{};
            hr = ctx->Map(stage, 0, D3D11_MAP_READ, 0, &m);
            dup->ReleaseFrame();
            if (FAILED(hr)) break;
            LONGLONG when = fi.LastPresentTime.QuadPart;
            if (last_present && when > last_present)
            {
                double g = ms_of(when - last_present);
                if (g > 2 && g < 200) gaps[gap_n++ & 31] = g;
            }
            last_present = when;
            // the rate the window really shows: the middle of the last gaps, near a usual rate taken as that
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
            VideoFormat nf;
            nf.pixel = Pixel::bgra;
            nf.w = w;
            nf.h = h;
            nf.fps = fps;
            nf.full = true;
            AcquireSRWLockExclusive(&fl);
            vf = nf;
            taken = {od.DesktopCoordinates.left + want.left, od.DesktopCoordinates.top + want.top, od.DesktopCoordinates.left + want.left + w, od.DesktopCoordinates.top + want.top + h};
            ReleaseSRWLockExclusive(&fl);
            LONGLONG t1 = qpc();
            const uint8_t *data = (const uint8_t *)m.pData;
            if (turn)
            {
                // desktop point (x, y) -> panel point (u, v)
                packed.resize((size_t)w * h * 4);
                uint32_t *o = (uint32_t *)packed.data();
                for (int y = 0; y < h; y++)
                {
                    int yd = (int)want.top + y;
                    for (int x = 0; x < w; x++)
                    {
                        int xd = (int)want.left + x, u, v;
                        if (turn == 90) { u = yd; v = (int)dw - 1 - xd; }
                        else if (turn == 270) { u = (int)dh - 1 - yd; v = xd; }
                        else { u = (int)dw - 1 - xd; v = (int)dh - 1 - yd; }
                        u = std::min(std::max(u, 0), tw - 1);
                        v = std::min(std::max(v, 0), th - 1);
                        *o++ = *(const uint32_t *)(data + (size_t)m.RowPitch * v + (size_t)u * 4);
                    }
                }
                data = packed.data();
            }
            else if (m.RowPitch != (UINT)w * 4)
            {
                packed.resize((size_t)w * h * 4);
                for (int y = 0; y < h; y++) memcpy(&packed[(size_t)y * w * 4], data + (size_t)m.RowPitch * y, (size_t)w * 4);
                data = packed.data();
            }

            LONGLONG t2 = qpc();
            sink->on_video(data, (size_t)w * h * 4, nf, std::min(when, qpc()), -1);
            static StepTimes st("screen times");
            st.add(t0, t1, t2, qpc(), qpc(), when);
            ctx->Unmap(stage, 0);
            have = true;
            sent = qpc();
        }
        drop(stage);
        drop(dup);
        drop(ctx);
        drop(dev);
        if (!stop) Sleep(100);
    }
}

// ---- sound: what the PC plays, as 16-bit stereo; silence where nothing plays, so the recording's sound keeps its time ----
void ScreenCapture::sound_loop()
{
    IMMDeviceEnumerator *e = nullptr;
    IMMDevice *d = nullptr;
    IAudioClient *c = nullptr;
    IAudioCaptureClient *cc = nullptr;
    WAVEFORMATEX *mix = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&e)) || FAILED(e->GetDefaultAudioEndpoint(eRender, eConsole, &d)) ||
        FAILED(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&c)) || FAILED(c->GetMixFormat(&mix)) ||
        FAILED(c->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, mix, nullptr)) || FAILED(c->GetService(__uuidof(IAudioCaptureClient), (void **)&cc)) || FAILED(c->Start()))
    {
        logf("screen: no sound of the PC");
        if (mix) CoTaskMemFree(mix);
        drop(cc); drop(c); drop(d); drop(e);
        return;
    }
    bool is_float = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((WAVEFORMATEXTENSIBLE *)mix)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    int ch = mix->nChannels, bits = mix->wBitsPerSample, rate = (int)mix->nSamplesPerSec;
    AudioFormat f = {rate, 2, 16};
    std::vector<int16_t> buf;
    LONGLONG start = qpc();
    long long given = 0;
    while (!stop)
    {
        Sleep(10);
        UINT32 n = 0;
        while (SUCCEEDED(cc->GetNextPacketSize(&n)) && n)
        {
            BYTE *p = nullptr;
            DWORD flags = 0;
            UINT32 frames = 0;
            if (FAILED(cc->GetBuffer(&p, &frames, &flags, nullptr, nullptr))) break;
            buf.resize((size_t)frames * 2);
            for (UINT32 i = 0; i < frames; i++)
                for (int k = 0; k < 2; k++)
                {
                    int s = std::min(k, ch - 1);
                    float v = 0;
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                    {
                        if (is_float && bits == 32) v = ((const float *)p)[(size_t)i * ch + s];
                        else if (bits == 16) v = ((const int16_t *)p)[(size_t)i * ch + s] / 32768.f;
                        else if (bits == 32) v = (float)(((const int32_t *)p)[(size_t)i * ch + s] / 2147483648.0);
                    }
                    buf[(size_t)i * 2 + k] = (int16_t)std::max(-32768.f, std::min(32767.f, v * 32767.f));
                }
            cc->ReleaseBuffer(frames);
            if (frames) sink->on_audio((const uint8_t *)buf.data(), buf.size() * 2, f);
            given += frames;
        }
        long long due = (long long)(ms_of(qpc() - start) * rate / 1000.0), gap = due - given - rate / 20; // more than 50 ms behind: nothing plays
        if (gap > 0)
        {
            buf.assign((size_t)gap * 2, 0);
            sink->on_audio((const uint8_t *)buf.data(), buf.size() * 2, f);
            given += gap;
        }
    }
    c->Stop();
    CoTaskMemFree(mix);
    drop(cc); drop(c); drop(d); drop(e);
}
