// SPDX-License-Identifier: GPL-3.0-only
#include "vrr.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// NVIDIA's published driver interface: is variable refresh at work on the window's monitor? Reads only.
typedef void *(*Query)(unsigned);
struct Info { unsigned version, flags, rest[4]; };
static std::mutex one; // NVIDIA does not say these calls may run side by side
int vrr_state(HWND window)
{
    static int last = -1;
    std::unique_lock<std::mutex> g(one, std::try_to_lock);
    if (!g) return last; // busy: the picture does not wait
    static Query q;
    static bool tried;
    static int (*by_name)(const char *, unsigned *);
    static int (*get)(unsigned, Info *);
    if (!tried)
    {
        tried = true;
        HMODULE m = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        q = m ? (Query)(void *)GetProcAddress(m, "nvapi_QueryInterface") : nullptr;
        auto init = q ? (int (*)())q(0x0150e828) : nullptr;
        if (init && !init())
        {
            by_name = (int (*)(const char *, unsigned *))q(0xAE457190);
            get = (int (*)(unsigned, Info *))q(0xDF8FDA57);
        }
    }
    if (!by_name || !get) return -1;
    MONITORINFOEXA mi{};
    mi.cbSize = sizeof mi;
    unsigned id = 0;
    Info s{};
    s.version = sizeof s | (1 << 16);
    if (!GetMonitorInfoA(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi) || by_name(mi.szDevice, &id) || get(id, &s)) return -1;
    return last = (s.flags & 17) == 17 ? 2 : (s.flags & 3) ? 1 : 0; // 16 alone: the monitor can, but follows the desktop, not this program
}

// ---- the monitor's own description (EDID, as Windows keeps it) ----
static std::vector<BYTE> edid_of(HWND window)
{
    std::vector<BYTE> e;
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    DISPLAY_DEVICEW d{};
    d.cb = sizeof d;
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi) || !EnumDisplayDevicesW(mi.szDevice, 0, &d, EDD_GET_DEVICE_INTERFACE_NAME)) return e;
    std::wstring id = d.DeviceID; // \?\DISPLAY#MODEL#INSTANCE#{...}
    size_t a = id.find(L'#'), b = a == id.npos ? a : id.find(L'#', a + 1), c = b == id.npos ? b : id.find(L'#', b + 1);
    if (c == id.npos) return e;
    std::wstring key = L"SYSTEM\\CurrentControlSet\\Enum\\DISPLAY\\" + id.substr(a + 1, b - a - 1) + L"\\" + id.substr(b + 1, c - b - 1) + L"\\Device Parameters";
    DWORD n = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"EDID", RRF_RT_REG_BINARY, nullptr, nullptr, &n) || n < 128 || n > 4096) return e;
    e.resize(n);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"EDID", RRF_RT_REG_BINARY, nullptr, e.data(), &n)) e.clear();
    return e;
}
static bool scan_edid(const std::vector<BYTE> &e, bool &hdmi, bool &hdmi_vrr, bool &freesync, bool &dp_vrr)
{
    hdmi = hdmi_vrr = freesync = dp_vrr = false;
    if (e.size() < 128) return false;
    if (e[20] & 0x80) hdmi = (e[20] & 15) == 2;
    for (int k = 0; k < 4; k++) // range limits without a timing formula: a DisplayPort monitor with Adaptive-Sync on
    {
        const BYTE *p = &e[(size_t)(54 + 18 * k)];
        if (!p[0] && !p[1] && p[3] == 0xfd && (e[24] & 1) && p[10] == 1 && p[6] > p[5]) dp_vrr = true;
    }
    for (size_t x = 128; x + 128 <= e.size(); x += 128)
    {
        const BYTE *p = &e[x];
        if (p[0] == 0x02) // CTA-861
            for (int i = 4; i < p[2] && i < 127;)
            {
                int tag = p[i] >> 5, n = p[i] & 31;
                if (i + n >= 128) break;
                if (tag == 3 && n >= 3)
                {
                    unsigned oui = p[i + 1] | p[i + 2] << 8 | p[i + 3] << 16;
                    if (oui == 0x000C03 || oui == 0xC45DD8) hdmi = true;
                    if (oui == 0xC45DD8 && n >= 10 && ((p[i + 9] & 0xc0) << 2 | p[i + 10])) hdmi_vrr = true;
                    if (oui == 0x00001A) freesync = true;
                }
                i += n + 1;
            }
        if (p[0] == 0x70) // DisplayID: an Adaptive-Sync block
            for (int i = 5; i + 3 < 127 && p[i + 2];)
            {
                if (p[i] == 0x2b) dp_vrr = true;
                i += 3 + p[i + 2];
            }
    }
    return true;
}
int vrr_monitor(HWND window)
{
    bool hdmi, hv, fs, dp;
    if (!scan_edid(edid_of(window), hdmi, hv, fs, dp)) return -1;
    return hv || fs || dp ? 1 : hdmi ? 0 : -1;
}

// ---- the NVIDIA driver's settings of this program ----
struct Profile { unsigned version; wchar_t name[2048]; unsigned gpu, predefined, apps, settings; };
struct Application { unsigned version, predefined; wchar_t app[2048], friendly[2048], launcher[2048]; };
struct Setting { unsigned version; wchar_t name[2048]; unsigned id, type, location, is_predefined, predefined_valid; union { unsigned u; BYTE room[4100]; } predefined, current; };
enum { PLAIN, WANTED, STEADY };
static std::wstring driver(int what)
{
    std::lock_guard<std::mutex> g(one);
    bool on = what != PLAIN;
    HMODULE m = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto q = m ? (Query)(void *)GetProcAddress(m, "nvapi_QueryInterface") : nullptr;
    if (!q) return L"NVIDIA driver not found";
    auto init = (int (*)())q(0x0150e828);
    auto create = (int (*)(void **))q(0x0694D52E);
    auto load = (int (*)(void *))q(0x375DBD6B);
    auto save = (int (*)(void *))q(0xFCBC7E14);
    auto destroy = (int (*)(void *))q(0xDAD9CFF8);
    auto find = (int (*)(void *, const wchar_t *, void **))q(0x7E4A9A0B);
    auto make = (int (*)(void *, Profile *, void **))q(0xCC176068);
    auto drop_profile = (int (*)(void *, void *))q(0x17093206);
    auto add = (int (*)(void *, void *, Application *))q(0x4347A9DE);
    auto set = (int (*)(void *, void *, Setting *))q(0x577DD202);
    if (!init || !create || !load || !save || !destroy || !find || !make || !drop_profile || !add || !set || init()) return L"NVIDIA driver interface";
    void *s = nullptr, *p = nullptr;
    int r = create(&s);
    if (!r) r = load(s);
    static const wchar_t title[2048] = L"Peussok";
    bool there = !r && !find(s, title, &p) && p;
    if (!r && !on) { if (there) { r = drop_profile(s, p); if (!r) r = save(s); } }
    else if (!r)
    {
        if (!there)
        {
            auto np = std::make_unique<Profile>();
            memset(np.get(), 0, sizeof *np);
            np->version = sizeof(Profile) | 1 << 16;
            wcscpy(np->name, title);
            np->gpu = 1; // GeForce
            r = make(s, np.get(), &p);
            auto a = std::make_unique<Application>();
            memset(a.get(), 0, sizeof *a);
            a->version = sizeof(Application) | 1 << 16;
            wcscpy(a->app, L"peussok.exe");
            wcscpy(a->friendly, title);
            if (!r) r = add(s, p, a.get());
        }
        const unsigned values[][2] = {{0x1094F1F7, what == WANTED ? 2u : 0u}, {0x10A879CF, what == WANTED ? 0u : 4u}}; // requested: full screen and windows, not overridden; or a fixed rate
        for (auto &v : values)
        {
            auto x = std::make_unique<Setting>();
            memset(x.get(), 0, sizeof *x);
            x->version = sizeof(Setting) | 1 << 16;
            x->id = v[0];
            x->current.u = v[1];
            if (!r) r = set(s, p, x.get());
        }
        if (!r) r = save(s);
    }
    if (s) destroy(s);
    logf("vrr: driver profile %d -> %d", what, r);
    if (!r) return L"";
    wchar_t t[64];
    swprintf(t, 64, L"NVIDIA %d", r);
    return t;
}
// ---- the driver's switch of one monitor: the two calls NVIDIA publishes for it, and the place where its control panel keeps the choice ----
struct SyncGet { unsigned version, longest_us, bits, count; unsigned long long stamp; unsigned token, rest[3]; };
struct SyncSet { unsigned version, longest_us, bits, rest1; unsigned long long longest_ns; unsigned rest[4]; };
struct Monitor
{
    Query q = nullptr;
    unsigned id = 0;
    bool vrr = false;
    SyncGet now{};
    int (*set)(unsigned, SyncSet *) = nullptr;
    int (*get)(unsigned, SyncGet *) = nullptr;
    bool read()
    {
        memset(&now, 0, sizeof now);
        now.version = sizeof now | 1 << 16;
        return get && !get(id, &now);
    }
};
static bool monitor_of(HWND window, Monitor &m)
{
    HMODULE l = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    m.q = l ? (Query)(void *)GetProcAddress(l, "nvapi_QueryInterface") : nullptr;
    if (!m.q) return false;
    auto init = (int (*)())m.q(0x0150e828);
    auto by_name = (int (*)(const char *, unsigned *))m.q(0xAE457190);
    auto caps = (int (*)(unsigned, void *))m.q(0x3b05c7e1);
    m.get = (int (*)(unsigned, SyncGet *))m.q(0xb73d1ee9);
    m.set = (int (*)(unsigned, SyncSet *))m.q(0x3eebba1d);
    MONITORINFOEXA mi{};
    mi.cbSize = sizeof mi;
    if (!init || !by_name || !caps || !m.get || !m.set || init() || !GetMonitorInfoA(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi) || by_name(mi.szDevice, &m.id)) return false;
    alignas(8) BYTE c[68] = {}; // version, size, kind of answer, connector, valid, answer
    unsigned version = sizeof c | 1 << 16, kind = 0x1002;
    WORD size = sizeof c;
    memcpy(c, &version, 4);
    memcpy(c + 4, &size, 2);
    memcpy(c + 8, &kind, 4);
    m.vrr = !caps(m.id, c) && (c[16] & 1) && (c[17] & 1);
    return m.read();
}
int vrr_allowed(HWND window)
{
    static int last = -1;
    std::unique_lock<std::mutex> g(one, std::try_to_lock);
    if (!g) return last;
    Monitor m;
    return last = monitor_of(window, m) && m.vrr ? !(m.now.bits & 1) : -1;
}
static int keep_choice(Query q, const std::wstring &model, bool on)
{
    auto create = (int (*)(void **))q(0x0694D52E);
    auto load = (int (*)(void *))q(0x375DBD6B);
    auto save = (int (*)(void *))q(0xFCBC7E14);
    auto destroy = (int (*)(void *))q(0xDAD9CFF8);
    auto find = (int (*)(void *, const wchar_t *, void **))q(0x7E4A9A0B);
    auto make = (int (*)(void *, Profile *, void **))q(0xCC176068);
    auto add = (int (*)(void *, void *, Application *))q(0x4347A9DE);
    auto set = (int (*)(void *, void *, Setting *))q(0x577DD202);
    if (!create || !load || !save || !destroy || !find || !make || !add || !set) return -1;
    auto np = std::make_unique<Profile>();
    memset(np.get(), 0, sizeof *np);
    np->version = sizeof(Profile) | 1 << 16;
    wcscpy(np->name, model.c_str());
    void *s = nullptr, *p = nullptr;
    int r = create(&s);
    if (!r) r = load(s);
    if (!r && (find(s, np->name, &p) || !p))
    {
        r = make(s, np.get(), &p);
        auto a = std::make_unique<Application>();
        memset(a.get(), 0, sizeof *a);
        a->version = sizeof(Application) | 1 << 16;
        for (size_t i = 0; i < model.size(); i++) a->app[i] = (wchar_t)towlower(model[i]);
        wcscpy(a->friendly, model.c_str());
        if (!r) r = add(s, p, a.get());
    }
    const unsigned values[][2] = {{0x106D5CFF, 1}, {0x1194F158, on ? 1u : 0u}}; // not in the list of programs; this model
    for (auto &v : values)
    {
        auto x = std::make_unique<Setting>();
        memset(x.get(), 0, sizeof *x);
        x->version = sizeof(Setting) | 1 << 16;
        x->id = v[0];
        x->current.u = v[1];
        if (!r) r = set(s, p, x.get());
    }
    if (!r) r = save(s);
    if (s) destroy(s);
    return r;
}
static std::atomic<int> allow_state{ALLOW_IDLE};
static std::mutex allow_lock;
static std::wstring allow_why;
static void allow_end(int r, const wchar_t *where)
{
    {
        std::lock_guard<std::mutex> g(allow_lock);
        allow_why.clear();
        if (r) { wchar_t t[64]; swprintf(t, 64, L"%ls %d", where, r); allow_why = t; }
    }
    logf("vrr: monitor switch %ls -> %d", where, r);
    allow_state = r ? ALLOW_FAILED : ALLOW_DONE;
}
static std::wstring model_of(const std::vector<BYTE> &e)
{
    if (e.size() < 128) return L"";
    wchar_t model[32];
    swprintf(model, 32, L"0x%04X:0x%04X", e[8] | e[9] << 8, e[10] | e[11] << 8);
    return model;
}
std::wstring vrr_model(HWND window) { return model_of(edid_of(window)); }
void vrr_allow(HWND window, bool on)
{
    if (allow_state.exchange(ALLOW_BUSY) == ALLOW_BUSY) return;
    std::vector<BYTE> e = edid_of(window);
    std::thread([window, on, e] {
        std::lock_guard<std::mutex> g(one);
        Monitor m;
        if (e.size() < 128 || !monitor_of(window, m) || !m.vrr) { allow_end(-1, L"monitor"); return; }
        std::wstring model = model_of(e);
        int r = keep_choice(m.q, model, on);
        if (r) { allow_end(r, L"store"); return; }
        ini_set(L"vrr_monitor_model", on ? model : std::wstring());
        if ((m.now.bits & 1) == (on ? 0u : 1u)) { allow_end(0, L"already"); return; }
        SyncSet x{};
        x.version = sizeof x | 2 << 16;
        x.longest_us = m.now.longest_us; // the call replaces everything: what was there goes back
        x.longest_ns = (unsigned long long)m.now.longest_us * 1000;
        x.bits = (m.now.bits & 2) | (on ? 0u : 1u);
        r = m.set(m.id, &x);
        if (r) { keep_choice(m.q, model, !on); ini_set(L"vrr_monitor_model", on ? std::wstring() : model); allow_end(r, L"display"); return; }
        for (int i = 0; i < 80; i++) // the monitor may connect again
        {
            Sleep(100);
            if (m.read() && (m.now.bits & 1) == (on ? 0u : 1u)) { allow_end(0, L"display"); return; }
        }
        allow_end(-2, L"display");
    }).detach();
}
int vrr_allow_now(std::wstring &why)
{
    std::lock_guard<std::mutex> g(allow_lock);
    why = allow_why;
    return allow_state;
}
std::wstring vrr_driver_on() { return driver(WANTED); }
std::wstring vrr_driver_off(bool fixed) { return driver(fixed ? STEADY : PLAIN); }
