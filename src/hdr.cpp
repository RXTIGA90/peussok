// SPDX-License-Identifier: GPL-3.0-only
#include "hdr.h"
#include <algorithm>
#include <atomic>
#include <dxgi1_6.h>
#include <thread>

// Windows 11 24H2 and later tell HDR and wide colour apart; earlier ones know one switch for both.
struct Info2 { DISPLAYCONFIG_DEVICE_INFO_HEADER h; UINT32 bits, encoding, depth, mode; };
struct Info1 { DISPLAYCONFIG_DEVICE_INFO_HEADER h; UINT32 bits, encoding, depth; };
struct Set1 { DISPLAYCONFIG_DEVICE_INFO_HEADER h; UINT32 on; };
static bool path_of(HWND window, DISPLAYCONFIG_PATH_TARGET_INFO &t)
{
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    UINT32 np = 0, nm = 0;
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi) || GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm)) return false;
    std::vector<DISPLAYCONFIG_PATH_INFO> p(np);
    std::vector<DISPLAYCONFIG_MODE_INFO> m(nm);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, p.data(), &nm, m.data(), nullptr)) return false;
    for (UINT32 i = 0; i < np; i++)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME s{};
        s.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof s, p[i].sourceInfo.adapterId, p[i].sourceInfo.id};
        if (!DisplayConfigGetDeviceInfo(&s.header) && !wcscmp(s.viewGdiDeviceName, mi.szDevice)) { t = p[i].targetInfo; return true; }
    }
    return false;
}
int hdr_state(HWND window)
{
    DISPLAYCONFIG_PATH_TARGET_INFO t{};
    if (!path_of(window, t)) return 0;
    Info2 b{};
    b.h = {(DISPLAYCONFIG_DEVICE_INFO_TYPE)15, sizeof b, t.adapterId, t.id};
    if (!DisplayConfigGetDeviceInfo(&b.h)) return b.mode == 2 ? 2 : b.bits & 16 ? 1 : 0; // the mode in use is HDR; HDR is supported
    Info1 a{};
    a.h = {(DISPLAYCONFIG_DEVICE_INFO_TYPE)9, sizeof a, t.adapterId, t.id};
    if (DisplayConfigGetDeviceInfo(&a.h)) return 0;
    return a.bits & 2 ? 2 : a.bits & 1 ? 1 : 0;
}
static std::atomic<bool> busy;
bool hdr_busy() { return busy; }
void hdr_set(HWND window, bool on)
{
    if (busy.exchange(true)) return;
    std::thread([window, on] {
        DISPLAYCONFIG_PATH_TARGET_INFO t{};
        LONG r = -1;
        if (path_of(window, t))
            for (int type : {16, 10}) // SET_HDR_STATE, else SET_ADVANCED_COLOR_STATE
            {
                Set1 s{};
                s.h = {(DISPLAYCONFIG_DEVICE_INFO_TYPE)type, sizeof s, t.adapterId, t.id};
                s.on = on;
                r = DisplayConfigSetDeviceInfo(&s.h);
                if (!r) break;
            }
        logf("hdr: %s -> %ld", on ? "on" : "off", r);
        for (int i = 0; i < 40 && !r && (hdr_state(window) == 2) != on; i++) Sleep(100);
        busy = false;
    }).detach();
}
float hdr_peak(HWND window)
{
    IDXGIFactory1 *f = nullptr;
    float peak = 0;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return 0;
    HMONITOR m = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    IDXGIAdapter1 *a = nullptr;
    for (UINT i = 0; f->EnumAdapters1(i, &a) == S_OK; i++)
    {
        IDXGIOutput *o = nullptr;
        for (UINT k = 0; a->EnumOutputs(k, &o) == S_OK; k++)
        {
            IDXGIOutput6 *o6 = nullptr;
            DXGI_OUTPUT_DESC1 d;
            if (SUCCEEDED(o->QueryInterface(IID_PPV_ARGS(&o6))) && SUCCEEDED(o6->GetDesc1(&d)) && d.Monitor == m) peak = d.MaxLuminance;
            release(o6);
            release(o);
        }
        release(a);
    }
    release(f);
    return peak;
}
