// SPDX-License-Identifier: GPL-3.0-only
// Peussok: shows a capture card's picture with frame generation, without any other program.
#include "app.h"
#include "theme.h"
#include "bench.h"
#include "guard.h"
#include "sys.h"
#include "rec.h"
#include "screen.h"
#include <algorithm>
#include <thread>
#include <shellapi.h>
#include <timeapi.h>
#include <windowsx.h>

AppState app;
static Capture capture;
static ScreenCapture screen; // a monitor or a window instead of a card
static struct Sink : CaptureSink
{
    void on_video(const uint8_t *d, size_t n, const VideoFormat &f, LONGLONG arrival, double) override { video_on_picture(d, n, f, arrival); }
    void on_buffer(uint8_t *d, size_t n, const VideoFormat &f) override { video_buffer(d, n, f); }
    void on_stop() override { video_buffers_gone(); }
    void on_audio(const uint8_t *d, size_t n, const AudioFormat &f) override { if (app.sound && !screen.running()) audio_push(d, n, f); rec_audio(d, n, f, qpc()); }
} sink;

bool replay_start(CaptureSink *s); // tests
void replay_stop();
// ---- settings ----
struct Entry { const wchar_t *key; volatile LONG *v; int min, max; };
static const Entry entries[] = {
    {L"model", &tune.model, 0, 127}, {L"multiplier", &tune.mult, 2, 4}, {L"share", &tune.share, 20, 50}, {L"margin", &tune.margin_x10, 0, 30}, {L"rife_flow", &tune.flow, 0, 3},
    {L"sharpen", &tune.sharpen, 0, 100}, {L"sharpen_made", &tune.sharpen_made, 0, 1}, {L"cut", &tune.cut, 0, 96}, {L"hud", &tune.hud, 0, 1}, {L"text", &tune.text, 0, 1}, {L"repair", &tune.repair, 0, 1}, {L"early", &tune.early, 0, 2}, {L"sync", &tune.sync, 0, 2}, {L"queue", &tune.queue, 1, 3}, {L"hdr", &tune.hdr, 0, 1}, {L"hdr_in", &tune.hdr_in, 0, 1}, {L"hdr_white", &tune.hdr_white, 80, 400}, {L"hdr_boost", &tune.hdr_boost, 0, 100}, {L"nr", &tune.nr, 0, 1}, {L"nr_style", &tune.nr_style, 0, 2}, {L"nr_power", &tune.nr_power, 0, 200}, {L"nr_size", &tune.nr_size, 0, 2}, {L"nr_layers", &tune.nr_layers, 1, 5}, {L"nr_detail", &tune.nr_detail, 0, 200}, {L"nr_tone", &tune.nr_tone, 0, 200}, {L"nr_keep", &tune.nr_keep, 0, 100}, {L"nr_skin", &tune.nr_skin, 0, 100}, {L"nr_steady", &tune.nr_steady, 0, 95}, {L"nr_fast", &tune.nr_fast, 0, 1}, {L"nr_order", &tune.nr_order, 0, 1},
    {L"range", &tune.range, 0, 2}, {L"fit", &tune.fit, 0, 1}, {L"osd", &tune.osd, 0, 3}, {L"corner", &tune.corner, 0, 3}, {L"volume", &audio_volume, 0, 100}, {L"mute", &audio_mute, 0, 1}, {L"audio_delay", &audio_delay, 0, 300}, {L"rec_codec", &rec_codec_pref, 0, 2}, {L"rec_quality", &rec_quality, 0, 2}, {L"rec_sound", &rec_sound, 0, 1}, {L"rec_made", &rec_made, 0, 1}, {L"on", &tune.on, 0, 1}, {L"cards", &app.cards, 0, 0x7ff},
    {L"mine_hue", &mine_hue, 0, 359}, {L"mine_sat", &mine_sat, 0, 100}, {L"mine_base", &mine_base, 0, 359}, {L"mine_dark", &mine_dark, 0, 40}, {L"mine_glow", &mine_glow, 0, 120}};
static LONG first[sizeof entries / sizeof entries[0]];
void tuning_load()
{
    for (size_t i = 0; i < sizeof first / sizeof first[0]; i++) first[i] = *entries[i].v;
    tune.model = default_model();
    for (auto &e : entries)
    {
        int v = ini_int(e.key, *e.v);
        if (v >= e.min && v <= e.max) InterlockedExchange(e.v, v);
    }
    tune.model = model_by_id(tune.model)->id;
    if (tune.model == 100) tune.model = default_model(); // no longer offered
    themes_load();
    if (ini_int(L"nr_steps", 0) != 2) // before 0.7.4 the strength meant whole layers
    {
        ini_set(L"nr_steps", 2);
        tune.nr_layers = 2;
        tune.nr_power = tune.nr_detail = tune.nr_tone = 100;
        tune.nr_keep = 0;
    }
    app.device = ini_text(L"device", L"");
    app.want.w = ini_int(L"width", 0);
    app.want.h = ini_int(L"height", 0);
    app.want.fps = ini_int(L"fps_x1000", 0) / 1000.0;
    app.want.pixel = (Pixel)std::min(std::max(ini_int(L"pixel", 0), 0), (int)Pixel::yv12);
    app.want_rate = ini_int(L"fps_chosen", 0) != 0;
    app.sound = ini_int(L"sound", 1) != 0;
    app.hot_mods = ini_int(L"hotkey_mods", MOD_CONTROL | MOD_ALT);
    app.hot_vk = ini_int(L"hotkey_vk", 'G');
    rec_set_folder(ini_text(L"rec_folder", L""));
    app.show_panel = ini_int(L"panel", 1) != 0;
    app.show_data = ini_int(L"data", 1) != 0;
    app.start_on = ini_int(L"start_on", 0);
    app.start_full = ini_int(L"start_full", 2); // 0 as last time, 1 full screen, 2 window
    if (ini_int(L"start_window_v101", 0) == 0) { ini_set(L"start_window_v101", 1); if (app.start_full == 0) app.start_full = 2; } // once, for settings from before
    app.awake = ini_int(L"awake", 1) != 0;
    app.topmost = ini_int(L"topmost", 0) != 0;
    app.see_through = ini_int(L"see_through", 1) != 0;
}
void tuning_reset_keys(std::initializer_list<const wchar_t *> keys)
{
    for (auto k : keys)
        for (size_t i = 0; i < sizeof first / sizeof first[0]; i++)
            if (!wcscmp(entries[i].key, k)) InterlockedExchange(entries[i].v, first[i]);
    for (auto k : keys) if (!wcscmp(k, L"model")) tune.model = default_model();
    tuning_save();
}
void tuning_reset()
{
    LONG on = tune.on;
    for (size_t i = 0; i < sizeof first / sizeof first[0]; i++) InterlockedExchange(entries[i].v, first[i]);
    tune.on = on;
    tune.model = default_model();
    app.want = VideoFormat();
    app.sound = app.awake = app.see_through = app.show_panel = app.show_data = true;
    app.topmost = false;
    app.hot_mods = MOD_CONTROL | MOD_ALT;
    app.hot_vk = 'G';
    app.start_on = 0;
    app.start_full = 2; // a window
    app_language = 0;
    theme_image(L"");
    theme_use(0);
    app_autostart(false);
    app_hotkey();
    app_topmost();
    tuning_save();
    app_open();
}
static const wchar_t *run_key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
bool app_autostart()
{
    wchar_t v[MAX_PATH * 4];
    DWORD n = sizeof v;
    return !RegGetValueW(HKEY_CURRENT_USER, run_key, L"Peussok", RRF_RT_REG_SZ, nullptr, v, &n) && wcsstr(v, (exe_dir() + L"\\").c_str());
}
void app_autostart(bool on)
{
    wchar_t p[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH * 4);
    std::wstring v = L"\"" + std::wstring(p, n) + L"\"";
    if (on) RegSetKeyValueW(HKEY_CURRENT_USER, run_key, L"Peussok", REG_SZ, v.c_str(), (DWORD)((v.size() + 1) * 2));
    else RegDeleteKeyValueW(HKEY_CURRENT_USER, run_key, L"Peussok");
}
void app_topmost()
{
    SetWindowPos(app.window, app.topmost && !app.fullscreen ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}
void tuning_save()
{
    if (bench_running) return; // the benchmark borrows the settings
    for (auto &e : entries) ini_set(e.key, (int)*e.v);
    ini_set(L"device", app.device);
    ini_set(L"width", app.want.w);
    ini_set(L"height", app.want.h);
    ini_set(L"fps_x1000", (int)(app.want.fps * 1000 + 0.5));
    ini_set(L"pixel", (int)app.want.pixel);
    ini_set(L"fps_chosen", app.want_rate ? 1 : 0);
    ini_set(L"sound", app.sound);
    ini_set(L"hotkey_mods", app.hot_mods);
    ini_set(L"hotkey_vk", app.hot_vk);
    ini_set(L"panel", app.show_panel);
    ini_set(L"data", app.show_data);
    ini_set(L"start_on", app.start_on);
    ini_set(L"start_full", app.start_full);
    ini_set(L"awake", app.awake);
    ini_set(L"topmost", app.topmost);
    ini_set(L"see_through", app.see_through);
    if (!GetEnvironmentVariableW(L"PKAPP_LANG", nullptr, 0)) ini_set(L"language", (int)app_language);
}

// ---- input ----
void app_refresh_devices()
{
    app.devices = capture_devices();
    for (auto &d : screen_devices()) app.devices.push_back(d);
    bool known = false;
    std::wstring name = ini_text(L"device_name", L"");
    for (auto &d : app.devices)
        if (d.path == app.device)
        {
            known = true;
            if (name != d.name) ini_set(L"device_name", d.name);
        }
    if (known || app.devices.empty() || app.device.rfind(L"window:", 0) == 0) return; // a taken window is not in the list
    if (!app.device.empty() && !name.empty()) // the chosen card is unplugged: only the same one at another socket takes its place
    {
        for (auto &d : app.devices)
            if (d.name == name) { app.device = d.path; return; }
        return;
    }
    // first choice: a capture card, then anything that is not a virtual camera or a webcam
    auto has = [](const std::wstring &s, const wchar_t *t) { return wcsstr(s.c_str(), t) != nullptr; };
    const Device *best = nullptr;
    int score = -1;
    for (auto &d : app.devices)
    {
        if (screen_path(d.path)) continue; // a monitor or a window only when chosen
        int s = has(d.name, L"Virtual") || has(d.name, L"OBS") || has(d.name, L"VTube") ? 0 : has(d.name, L"AVerMedia") || has(d.name, L"Elgato") || has(d.name, L"Capture") || has(d.name, L"Game") || has(d.name, L"Cam Link") ? 3 : has(d.name, L"cam") || has(d.name, L"CAM") || has(d.name, L"Cam") ? 1 : 2;
        if (s > score) { score = s; best = &d; }
    }
    if (best && score >= (app.device.empty() ? 2 : 3)) app.device = best->path; // a card that is unplugged stays the chosen one; a webcam is never taken unasked
}
void app_open()
{
    if (bench_running) return;
    if (app.devices.empty()) app_refresh_devices();
    capture.close();
    screen.close();
    video_no_signal();
    screen_exclude_own(false); // until the program lies over what is taken
    if (screen_path(app.device))
    {
        app.problem = screen.open(app.device, app.sound, &sink);
        app.formats.clear();
        app.current = screen.video();
        app.audio = screen.audio();
        if (!app.problem.empty()) logf("app: %ls", app.problem.c_str());
        return;
    }
    if (app.device.empty()) { app.problem = tr(L"캡처보드를 찾지 못했어요", L"No capture card found"); return; }
    VideoFormat want = app.want;
    app.problem = capture.open(app.device, want, app.sound, &sink);
    app.formats = capture.formats;
    app.current = capture.video();
    app.audio = capture.audio();
    if (!app.problem.empty()) logf("app: %ls", app.problem.c_str());
}

// ---- window ----
static WINDOWPLACEMENT before_full = {sizeof before_full};
static void fill_monitor()
{
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(app.window, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowPos(app.window, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
}
void app_fullscreen(bool on)
{
    if (on == app.fullscreen) return;
    if (on && app.bench_page) { if (bench_running) return; app.bench_page = false; }
    app.fullscreen = on;
    LONG style = GetWindowLongW(app.window, GWL_STYLE);
    if (on)
    {
        GetWindowPlacement(app.window, &before_full);
        SetWindowLongW(app.window, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        fill_monitor();
    }
    else
    {
        SetWindowLongW(app.window, GWL_STYLE, (style & ~WS_POPUP) | (WS_OVERLAPPEDWINDOW & ~WS_SYSMENU));
        SetWindowPlacement(app.window, &before_full);
        SetWindowPos(app.window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    if (!on && ui_open()) ui_toggle();
    if (app.topmost) app_topmost();
    app_screen_changed();
    ini_set(L"fullscreen", on);
}
void app_generation(bool on)
{
    if (bench_running) return; // it needs it as it is
    InterlockedExchange(&tune.on, on);
    ini_set(L"on", on);
    logf("app: generation %s", on ? "on" : "off");
}
void app_hotkey()
{
    UnregisterHotKey(app.window, 1);
    app.hot_ok = app.hot_vk && !app.hot_capture && RegisterHotKey(app.window, 1, (UINT)app.hot_mods | MOD_NOREPEAT, (UINT)app.hot_vk);
    UnregisterHotKey(app.window, 2);
    RegisterHotKey(app.window, 2, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'R'); // recording
    UnregisterHotKey(app.window, 3);
    RegisterHotKey(app.window, 3, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'P'); // recording: pause / go on
}
// Recording the picture on or off; why it could not start is shown on the recording card.
void app_video_record(bool on)
{
    if (on)
    {
        std::wstring e = rec_start();
        if (!e.empty()) logf("rec: %ls", e.c_str());
    }
    else rec_stop();
    if (app.window) InvalidateRect(app.window, nullptr, FALSE);
}
std::wstring app_hotkey_name()
{
    if (!app.hot_vk) return tr(L"없음", L"None");
    std::wstring s;
    if (app.hot_mods & MOD_CONTROL) s += L"Ctrl + ";
    if (app.hot_mods & MOD_ALT) s += L"Alt + ";
    if (app.hot_mods & MOD_SHIFT) s += L"Shift + ";
    wchar_t k[32];
    if ((app.hot_vk >= 'A' && app.hot_vk <= 'Z') || (app.hot_vk >= '0' && app.hot_vk <= '9')) swprintf(k, 32, L"%c", app.hot_vk);
    else if (app.hot_vk >= VK_F1 && app.hot_vk <= VK_F24) swprintf(k, 32, L"F%d", app.hot_vk - VK_F1 + 1);
    else if (!GetKeyNameTextW((LONG)(MapVirtualKeyW((UINT)app.hot_vk, MAPVK_VK_TO_VSC) << 16), k, 32)) swprintf(k, 32, L"0x%02X", app.hot_vk);
    return s + k;
}

// ---- numbers ----
static double shown_fps, source_fps;
static double made_fps, repeats;
double app_shown_fps() { return shown_fps; }
double app_source_fps() { return source_fps; }
double app_made_fps() { return made_fps; }
double app_repeat_percent() { return repeats; }
void app_screen_changed()
{
    ui_layout();
    InvalidateRect(app.window, nullptr, FALSE);
}
std::wstring app_monitor_name()
{
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(app.window, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi);
    DISPLAY_DEVICEW d{};
    d.cb = sizeof d;
    if (!EnumDisplayDevicesW(mi.szDevice, 0, &d, 0)) return mi.szDevice;
    return d.DeviceString;
}
// ---- numbers to a file, one line a second ----
static FILE *record;
void app_record(bool on)
{
    if (record) { fclose(record); record = nullptr; }
    app.recording = false;
    if (!on) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t name[64];
    swprintf(name, 64, L"\\numbers-%04d%02d%02d-%02d%02d%02d.csv", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    record = _wfopen((data_dir() + name).c_str(), L"w");
    if (!record) return;
    fputs("time,model,multiplier,on,source_fps,made_fps,shown_fps,latency_ms,latency_max_ms,ready_ms,generate_ms,hud_ms,share_ms,skipped,late,dropped,cuts,cpu_pct,cpu_app_pct,cpu_mhz,cpu_w,ram_gb,ram_app_gb,gpu_pct,gpu_mhz,mem_mhz,gpu_w,gpu_c,vram_gb,vram_app_gb\n", record);
    app.recording = true;
}
static void record_line()
{
    if (!record) return;
    SysLive s = sys_live();
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(record, "%02d:%02d:%02d,%ld,%ld,%ld,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%lld,%lld,%lld,%lld,%.1f,%.1f,%.0f,%.1f,%.2f,%.2f,%.0f,%.0f,%.0f,%.1f,%.0f,%.2f,%.2f\n", t.wHour, t.wMinute, t.wSecond, tune.model, tune.mult, tune.on, source_fps, made_fps, shown_fps,
            num.latency.get(), num.lat_peak / 1000.0, num.ready.get(), num.generate.get(), num.hud.get(), num.share.get(), (long long)num.skipped, (long long)num.late, (long long)num.dropped, (long long)num.cuts, (double)s.cpu, (double)s.cpu_app,
            (double)s.cpu_mhz, (double)s.cpu_w, (double)s.ram_used, (double)s.ram_app, (double)s.gpu, (double)s.gpu_mhz, (double)s.mem_mhz, (double)s.gpu_w, (double)s.gpu_temp, (double)s.vram_used, (double)s.vram_app);
    fflush(record);
}
// ---- benchmark ----
void app_bench(bool full)
{
    if (bench_running) return;
    capture.close();
    screen.close();
    bench_start(full);
}
static void bench_check()
{
    static bool was;
    bool now = bench_running != 0;
    if (was && !now)
    {
        bench_finish();
        video_no_signal();
        app_open();
        app_screen_changed();
    }
    was = now;
}
static void count_rates()
{
    static LONGLONG t;
    static LONG64 shown, sources, made, captured;
    LONGLONG now = qpc();
    if (t && ms_of(now - t) >= 1900)
    {
        shown_fps = 1000.0 * (double)(num.shown - shown) / ms_of(now - t);
        source_fps = 1000.0 * (double)(num.sources - sources) / ms_of(now - t);
        made_fps = 1000.0 * (double)(num.made_count - made) / ms_of(now - t);
        repeats = num.captured > captured ? 100.0 * (1.0 - (double)(num.sources - sources) / (double)(num.captured - captured)) : 0;
    }
    else if (t) return;
    t = now;
    shown = num.shown;
    sources = num.sources;
    made = num.made_count;
    captured = num.captured;
}
std::wstring app_status_line()
{
    wchar_t t[256];
    if (!app.problem.empty()) return app.problem;
    if (!app.signal) return tr(L"입력 신호 대기 중", L"Waiting for a signal");
    if (!tune.on) swprintf(t, 256, tr(L"꺼짐 · 원본 %.0f fps", L"Off · source %.0f fps"), source_fps);
    else if (num.engine_state == 1) swprintf(t, 256, tr(L"%ls 준비 중", L"Loading %ls"), model_by_id(tune.model)->name);
    else if (num.too_slow) swprintf(t, 256, tr(L"%ls은(는) 이 컴퓨터에서 느려요", L"%ls is too slow here"), model_by_id(tune.model)->name);
    else if (num.engine_state == -1) swprintf(t, 256, tr(L"%ls을(를) 쓸 수 없어요: %hs", L"%ls is not available: %hs"), model_by_id(tune.model)->name, video_engine_error().c_str());
    else swprintf(t, 256, tr(L"켜짐 · %.0f → %.0f fps", L"On · %.0f → %.0f fps"), source_fps, shown_fps);
    return t;
}
static void update_label()
{
    std::wstring text;
    wchar_t line[200];
    std::wstring notice = guard_notice();
    if (bench_running) text = std::wstring(tr(L"벤치마크 ", L"Benchmark ")) + (bench_model_now >= 0 ? models[bench_model_now].name : L"") + L" · " + bench_size_name(bench_size_now);
    else if (!app.problem.empty() || !app.signal) text = app_status_line() + (app.signal ? L"" : tr(L"\nF1: 설정", L"\nF1: settings"));
    else if (tune.osd)
    {
        swprintf(line, 200, L"%.0f / %.0f fps", source_fps, shown_fps);
        text = line;
        if (tune.osd >= 2)
        {
            swprintf(line, 200, tr(L"\n지연 %.1f ms (입력→출력)", L"\nlatency %.1f ms (arrival→screen)"), num.latency.get());
            text += line;
            if (tune.on && num.engine_state == 2)
            {
                swprintf(line, 200, tr(L"\n생성 %.1f · 준비 %.1f · HUD %.1f · 표시 %.1f ms", L"\ngen %.1f · ready %.1f · HUD %.1f · shown %.1f ms"), num.generate.get(), num.ready.get(), num.hud.get(), num.share.get());
                text += line;
            }
            if (tune.osd == 3)
            {
                SysLive s = sys_live();
                swprintf(line, 200, L"\nGPU %.0f%% · %.0f MHz · %.0f W · %.0f°C · %.1f GB\nCPU %.0f%% · %.0f MHz · %.0f W   RAM %.1f GB", (double)s.gpu, (double)s.gpu_mhz, (double)s.gpu_w, (double)s.gpu_temp, (double)s.vram_used, (double)s.cpu, (double)s.cpu_mhz, (double)s.cpu_w, (double)s.ram_used);
                text += line;
            }
            swprintf(line, 200, L"\n%ls", tune.on ? model_by_id(tune.model)->name : L"-");
            text += line;
        }
    }
    if (!notice.empty()) text = notice + (text.empty() ? L"" : L"\n") + text;
    RECT r;
    GetClientRect(app.view, &r);
    static ULONGLONG said;
    if (tune.osd == 3 && notice.empty() && GetTickCount64() - said < 950) return; // many lines: once a second
    said = GetTickCount64();
    bool words = bench_running || !app.problem.empty() || !app.signal || !notice.empty(); // something to read, not only numbers
    video_set_label(text, std::max(12, (int)r.bottom * (words ? 19 : 13) / 1080));
}
// Is everything still alive? Called four times a second.
static void watch()
{
    static ULONGLONG loading, dead;
    static LONG64 made;
    ULONGLONG now = GetTickCount64();
    if (video_lost()) guard_restart("the graphics device is gone");
    if (video_stuck_seconds() > std::max(1.5, num.generate.get() * 0.008)) guard_restart("a picture is stuck in the engine");
    if (num.engine_state != 1) loading = 0;
    else if (!loading) loading = now;
    else if (now - loading > 60000) guard_restart("the engine does not finish loading");
    // pictures come, generation is on and ready, but none is made
    bool should = tune.on && num.engine_state == 2 && !num.too_slow && app.signal && !bench_running && source_fps > 10;
    if (!should || num.made_count != made) dead = now;
    made = num.made_count;
    if (should && now - dead > 4000) { dead = now; video_engine_restart(); }
}
static void tick()
{
    count_rates();
    watch();
    bool signal = ms_of(qpc() - num.last_arrival) < 1500 && num.captured > 0;
    if (signal != app.signal) { app.signal = signal; logf("app: signal %d", signal); if (!signal) video_no_signal(); }
    static ULONGLONG retry;
    static ULONGLONG silent;
    bench_check();
    bool open = capture.running() || screen.running();
    if (!screen.running()) screen_exclude_own(false);
    else
    {
        VideoFormat v = screen.video();
        if (v.w) app.current = v;
        // Left out of the capture only while the program covers what is taken in full screen (it would take itself); a window
        // always stays visible to remote desktop programs, OBS and screenshots.
        RECT a = screen.area(), w, x;
        GetWindowRect(app.window, &w);
        bool over = app.fullscreen && IntersectRect(&x, &a, &w); // only full screen: a window stays reachable (it then shows itself, as an OBS preview does)
        screen_exclude_own(over);
    }
    if (signal || !open) silent = GetTickCount64();
    if (!bench_running && !app.testing && GetTickCount64() - retry > 3000 && (!open || GetTickCount64() - silent > 8000)) // unplugged, or in use by another program
    {
        retry = silent = GetTickCount64();
        app_refresh_devices();
        app_open();
    }
    SetThreadExecutionState(signal && app.awake ? ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
    update_label();
    sys_want(ui_needs_numbers() || tune.osd == 3 || app.recording || bench_running);
    static ULONGLONG second;
    if (GetTickCount64() - second >= 1000) { second = GetTickCount64(); record_line(); }
    ui_tick();
    static int reopen = GetEnvironmentVariableW(L"PKAPP_REOPEN", nullptr, 0) ? 1 : 0; // tests: the card again and again, in other sizes
    static ULONGLONG reopened;
    if (reopen && GetTickCount64() - reopened > 2500)
    {
        static const int sizes[4][3] = {{3840, 2160, 1}, {1920, 1080, 3}, {2560, 1440, 1}, {3840, 2160, 2}};
        reopened = GetTickCount64();
        const int *z = sizes[reopen++ & 3];
        app.want.w = z[0];
        app.want.h = z[1];
        app.want.pixel = (Pixel)z[2];
        app_open();
    }
    static int stats = -1;
    if (stats < 0) { wchar_t v[4]; stats = GetEnvironmentVariableW(L"PKAPP_STATS", v, 4) ? 1 : 0; }
    static ULONGLONG said;
    if (stats && GetTickCount64() - said > 2000)
    {
        said = GetTickCount64();
        logf("stats src=%.1f shown=%.1f lat=%.2f copy=%.2f ready=%.2f gen=%.2f hud=%.2f made=%.2f share=%.2f early=%.0f%% ahead=%.0f%% every=%.1f | sources=%lld made=%lld skipped=%lld dropped=%lld late=%lld cuts=%lld flow=%ld audio=%.0fms gaps=%lld gpu=%.0f%%",
             source_fps, shown_fps, num.latency.get(), num.copy.get(), num.ready.get(), num.generate.get(), num.hud.get(), num.made.get(), num.share.get(), num.early.get(), num.ahead.get(), num.interval.get(), (long long)num.sources, (long long)num.made_count,
             (long long)num.skipped, (long long)num.dropped, (long long)num.late, (long long)num.cuts, num.flow, audio_buffer_ms(), (long long)audio_gaps, (double)sys_live().gpu);
    }
}

static ULONGLONG moved;
static POINT last_mouse;
static void mouse_moved(POINT p)
{
    if (p.x != last_mouse.x || p.y != last_mouse.y) { last_mouse = p; moved = GetTickCount64(); }
}
// The picture: a child window that only shows; keys stay with the program window.
static LRESULT CALLBACK view_proc(HWND w, UINT m, WPARAM a, LPARAM b)
{
    switch (m)
    {
    case WM_SIZE: video_resized(); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: ValidateRect(w, nullptr); video_resized(); return 0;
    case WM_LBUTTONDOWN: SetFocus(app.window); return 0;
    case WM_LBUTTONDBLCLK: app_fullscreen(!app.fullscreen); return 0;
    case WM_RBUTTONUP: ui_toggle(); return 0;
    case WM_MOUSEMOVE:
    {
        POINT p = {GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        mouse_moved(p);
        break;
    }
    case WM_SETCURSOR:
        if (app.fullscreen && !ui_open() && GetTickCount64() - moved > 2000) { SetCursor(nullptr); return TRUE; }
        break;
    }
    return DefWindowProcW(w, m, a, b);
}
static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM a, LPARAM b)
{
    LRESULT r = 0;
    if (ui_message(w, m, a, b, r)) return r;
    switch (m)
    {
    case WM_SIZE: if (a != SIZE_MINIMIZED) { ui_layout(); InvalidateRect(w, nullptr, FALSE); } return 0;
    case WM_ERASEBKGND: return 1;
    case WM_GETMINMAXINFO:
    {
        UINT dpi = GetDpiForWindow(w);
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST), &mi);
        ((MINMAXINFO *)b)->ptMinTrackSize = {std::min<LONG>(MulDiv(980, (int)dpi, 96), mi.rcWork.right - mi.rcWork.left), std::min<LONG>(MulDiv(660, (int)dpi, 96), mi.rcWork.bottom - mi.rcWork.top)}; // a small or strongly scaled screen
        return 0;
    }
    case WM_HOTKEY:
        if (a == 2) app_video_record(!rec_on());
        else if (a == 3) { rec_pause(!rec_paused()); InvalidateRect(app.window, nullptr, FALSE); }
        else app_generation(!tune.on);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (a == VK_F1) ui_toggle();
        else if (a == VK_F11 || (a == VK_RETURN && m == WM_SYSKEYDOWN)) app_fullscreen(!app.fullscreen);
        else if (a == VK_ESCAPE) { if (ui_guide_escape()) {} else if (ui_open()) ui_toggle(); else if (app.fullscreen) app_fullscreen(false); else if (app.bench_page && !bench_running) { app.bench_page = false; app_screen_changed(); } }
        else if (a == 'G' && m == WM_KEYDOWN) app_generation(!tune.on);
        else if (a == 'M') InterlockedExchange(&audio_mute, !audio_mute);
        else if (a == VK_UP || a == VK_DOWN) InterlockedExchange(&audio_volume, std::min(100L, std::max(0L, audio_volume + (a == VK_UP ? 5L : -5L))));
        else break;
        return 0;
    case WM_SYSCHAR: if (a == VK_RETURN) return 0; break;
    case WM_TIMER:
        if (a == 2) { ui_animate(); return 0; }
        tick();
        if (app.fullscreen && !ui_open() && GetTickCount64() - moved > 2000 && GetForegroundWindow() == w) SetCursor(nullptr);
        return 0;
    case WM_DISPLAYCHANGE:
        if (app.fullscreen) fill_monitor();
        else app_screen_changed();
        return 0;
    case WM_DPICHANGED:
    {
        if (app.fullscreen) { fill_monitor(); return 0; }
        RECT *r = (RECT *)b;
        SetWindowPos(w, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_CLOSE:
        guard_clean_exit();
        if (bench_running) { bench_cancel(); bench_finish(); }
        DestroyWindow(w);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(w, m, a, b);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmd, int)
{
    portable_setup();
    if (!wcsstr(cmd, L"--child") && !GetEnvironmentVariableW(L"PKAPP_NO_GUARD", nullptr, 0)) return guard_main(cmd);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HANDLE one = CreateMutexW(nullptr, FALSE, L"PeussokAppOne");
    if (GetLastError() == ERROR_ALREADY_EXISTS && !GetEnvironmentVariableW(L"PKAPP_FAKE", nullptr, 0) && !GetEnvironmentVariableW(L"PKAPP_SECOND", nullptr, 0)) // tests: a second one
    {
        HWND h = FindWindowW(L"PeussokApp", nullptr);
        if (h) { if (IsIconic(h)) ShowWindow(h, SW_RESTORE); SetForegroundWindow(h); }
        return 0;
    }
    log_open();
    logf("Peussok %ls", APP_VERSION);
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS *e) -> LONG {
        logf("crash 0x%08lx at +0x%llx", (unsigned long)e->ExceptionRecord->ExceptionCode, (unsigned long long)((char *)e->ExceptionRecord->ExceptionAddress - (char *)GetModuleHandleW(nullptr)));
        return EXCEPTION_CONTINUE_SEARCH;
    });
    guard_child_begin(cmd);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1);
    tuning_load();
    WNDCLASSW c{};
    c.lpfnWndProc = proc;
    c.hInstance = inst;
    c.lpszClassName = L"PeussokApp";
    c.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    c.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    c.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    RegisterClassW(&c);
    WNDCLASSW v{};
    v.style = CS_DBLCLKS;
    v.lpfnWndProc = view_proc;
    v.hInstance = inst;
    v.lpszClassName = L"PeussokAppView";
    v.hCursor = c.hCursor;
    RegisterClassW(&v);
    UINT dpi = GetDpiForSystem();
    POINT zero = {0, 0};
    MONITORINFO pm = {sizeof pm};
    GetMonitorInfoW(MonitorFromPoint(zero, MONITOR_DEFAULTTOPRIMARY), &pm);
    RECT r = {0, 0, MulDiv(1480, (int)dpi, 96), MulDiv(900, (int)dpi, 96)};
    r.right = std::min(r.right, (pm.rcWork.right - pm.rcWork.left) * 92 / 100);
    r.bottom = std::min(r.bottom, (pm.rcWork.bottom - pm.rcWork.top) * 90 / 100);
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    app.window = CreateWindowExW(0, c.lpszClassName, app_name(), (WS_OVERLAPPEDWINDOW & ~WS_SYSMENU) | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!app.window) return 1;
    BOOL dark = TRUE; // dark title bar
    if (HMODULE dwm = LoadLibraryExW(L"dwmapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
    {
        auto attr = reinterpret_cast<HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD)>(reinterpret_cast<void *>(GetProcAddress(dwm, "DwmSetWindowAttribute")));
        if (attr) attr(app.window, 20, &dark, sizeof dark);
    }
    app.view = CreateWindowExW(0, v.lpszClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, 640, 360, app.window, nullptr, inst, nullptr);
    ui_init();
    ui_layout();
    if (!app.view || !video_start(app.view))
    {
        MessageBoxW(nullptr, tr(L"그래픽 장치를 열 수 없어요.", L"The graphics device could not be opened."), app_name(), MB_OK | MB_ICONERROR);
        return 1;
    }
    wchar_t fake[32]; // tests: no capture card, no sound, no visible window
    bool testing = GetEnvironmentVariableW(L"PKAPP_FAKE", fake, 32) != 0;
    if (!testing) audio_start();
    sys_start();
    bench_load();
    if (ini_int(L"nr_trying", 0)) { tune.nr = 0; ini_set(L"nr_trying", 0); guard_say(tr(L"DLSS 5를 열다가 멈춰서 껐어요", L"DLSS 5 was turned off because the program stopped while loading it")); }
    if (app.start_on && !guard_recovered()) tune.on = app.start_on == 1;
    if (wcsstr(cmd, L"--on")) tune.on = 1;
    if (wcsstr(cmd, L"--off")) tune.on = 0;
    if (const wchar_t *m = wcsstr(cmd, L"--model ")) tune.model = model_by_id(_wtoi(m + 8))->id;
    tuning_save(); // the guard reads the model from there
    int seconds = 0;
    if (const wchar_t *s = wcsstr(cmd, L"--seconds ")) seconds = _wtoi(s + 10);
    if (!testing)
    {
        ShowWindow(app.window, SW_SHOW);
        SetForegroundWindow(app.window);
    }
    if (app.topmost) app_topmost();
    if ((app.start_full && !guard_recovered() ? app.start_full == 1 : ini_int(L"fullscreen", 0) != 0) || wcsstr(cmd, L"--full")) app_fullscreen(true);
    if (testing && !replay_start(&sink))
    {
        int fw = 1280, fh = 720;
        swscanf(fake, L"%dx%d", &fw, &fh);
        bench_fake(fw, fh);
    }
    else if (!testing)
    {
        app_hotkey();
        app_refresh_devices();
        if (replay_start(&sink)) testing = true;
        else app_open();
    }
    app.testing = testing;
    SetTimer(app.window, 1, 250, nullptr);
    SetTimer(app.window, 2, 66, nullptr);
    if (wcsstr(cmd, L"--bench")) { app.bench_page = true; app_bench(wcsstr(cmd, L"--bench-full") != nullptr); app_screen_changed(); }
    { // tests: PKAPP_REC = "start seconds", records from then on for that long
        wchar_t v[32];
        if (GetEnvironmentVariableW(L"PKAPP_REC", v, 32))
        {
            int a = 3, n = 5;
            swscanf(v, L"%d %d", &a, &n);
            std::thread([a, n] { Sleep((DWORD)a * 1000); app_video_record(true); Sleep((DWORD)n * 1000); app_video_record(false); }).detach();
        }
    }
    ULONGLONG begin = GetTickCount64();
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        if (seconds && GetTickCount64() - begin > (ULONGLONG)seconds * 1000) { guard_clean_exit(); DestroyWindow(app.window); seconds = 0; continue; }
        if (ui_key(m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (bench_running) { bench_cancel(); bench_finish(); }
    app_record(false);
    replay_stop();
    capture.close();
    screen.close();
    audio_stop();
    sys_stop();
    rec_stop();
    video_stop();
    tuning_save();
    timeEndPeriod(1);
    logf("closed");
    CloseHandle(one);
    ExitProcess(0); // engine DLLs of other vendors are not unloaded one by one
}
