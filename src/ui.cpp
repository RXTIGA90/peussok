// SPDX-License-Identifier: GPL-3.0-only
// The program window around the picture: icon rail, settings column, data cards, and the benchmark page. In full
// screen the same settings open as a popup at the left edge.
// One function per part runs for painting and again for each mouse event: a widget draws itself when painting and reports a click
// otherwise, so its place, look and action are written once. Coordinates: 96-dpi units.
// All of this runs on the window thread without the graphics card; the picture path has its own threads and its own device.
#include "app.h"
#include "bench.h"
#include "draw.h"
#include "sys.h"
#include "vrr.h"
#include "theme.h"
#include "guard.h"
#include "mjpeg.h"
#include "nr.h"
#include "hdr.h"
#include "rec.h"
#include <commdlg.h>
#include <algorithm>
#include <atomic>
#include <thread>
#include <shobjidl.h>
#include <cmath>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

HWND ui_window;
enum Mode { PAINT, DOWN, MOVE, UP };
struct Area { float l, t, r, b; };
static struct { Mode mode; float mx, my; int drag; bool changed, released; Area clip; } gui = {PAINT, -1, -1, -1, false, false, {0, 0, 1e6f, 1e6f}};
static int dpi = 96, page = [] { wchar_t v[8]; return GetEnvironmentVariableW(L"PKAPP_PAGE", v, 8) ? _wtoi(v) : 0; }(), bench_fps = 60; // PKAPP_PAGE: tests open a page
static float scroll[3], scroll_max[3];
static bool glass; // Windows draws the blurred desktop behind the window

// ---- colours ----
#define SKY SECOND
static const Ink CARD = ink(255, 255, 255, 14), CARD_HI = ink(255, 255, 255, 26), FIELD = ink(255, 255, 255, 20), FIELD_HI = ink(255, 255, 255, 38), EDGE = ink(255, 255, 255, 18), AMBER = ink(255, 190, 84), VIOLET = ink(184, 150, 255), CORAL = ink(255, 126, 110);
enum { TOP = 44, RAIL = 84, PANEL_W = 404, DATA_H = 304, CARD_H = 136, GAP = 8, EDGE_PAD = 12 };
enum { CARD_FPS, CARD_LATENCY, CARD_PACE, CARD_GEN, CARD_QUALITY, CARD_SOURCE, CARD_GPU, CARD_CPU, CARD_RAM, CARD_SCREEN, CARD_SPEC, CARDS };
enum { P_HOME, P_GENERATE, P_PICTURE, P_SCREEN, P_INPUT, P_SOUND, P_RECORD, P_DATA, P_THEME, P_BENCH, P_SETTINGS, PAGES };

// ---- history: one value a second, two minutes ----
enum { H_SHOWN, H_SOURCE, H_LATENCY, H_GPU, H_CPU, HISTORY_KINDS, HISTORY = 120 };
static float history[HISTORY_KINDS][HISTORY];
static int history_n;
static double peak_ms;

static Ink theme_shade(float k) // the window's colour, brighter or darker
{
    auto f = [&](int s) { return std::min(255, (int)((float)(BASE >> s & 255) * k + (k > 1 ? 2 : 0))); };
    return ink(f(16), f(8), f(0));
}
static Ink theme_preview(int what) { for (auto &t : themes()) if (t.id == L"mine") return what ? t.base : t.accent; return 0; }

// ---- widgets ----
static void text(Font f, Ink c, float x, float y, float w, float h, Align a, const std::wstring &s) { if (gui.mode == PAINT) d_text(f, c, x, y, w, h, a, s); }
static void box(float x, float y, float w, float h, float r, Ink fill, Ink line = 0, float lw = 1) { if (gui.mode == PAINT) d_box(x, y, w, h, r, fill, line, lw); }
static void fill(float x, float y, float w, float h, Ink c) { if (gui.mode == PAINT) d_fill(x, y, w, h, c); }
static bool in(float x, float y, float w, float h)
{
    return gui.mx >= x && gui.mx < x + w && gui.my >= y && gui.my < y + h && gui.mx >= gui.clip.l && gui.mx < gui.clip.r && gui.my >= gui.clip.t && gui.my < gui.clip.b;
}
static bool guide_block; // the user guide is open: what is under it does not take the mouse
static bool hot(float x, float y, float w, float h) { return !guide_block && gui.drag < 0 && in(x, y, w, h); }
static bool pressed(float x, float y, float w, float h, bool enabled = true) { return !guide_block && gui.mode == DOWN && enabled && in(x, y, w, h); }
static std::wstring fmt(const wchar_t *f, ...)
{
    wchar_t t[300];
    va_list a;
    va_start(a, f);
    vswprintf(t, 300, f, a);
    va_end(a);
    return t;
}
static std::wstring num1(float v, const wchar_t *f) { return v < 0 ? std::wstring(L"—") : fmt(f, (double)v); }
enum { MK_POWER, MK_PRESETS, MK_MODEL, MK_MULT, MK_DEVICE, MK_FULL, MK_RECORD, MK_RAIL_SCREEN, MK_RAIL_SOUND, MK_RAIL_BENCH, MK_RAIL_GUIDE, MK_COUNT };
static void mark(int i, float x, float y, float w, float h);
static void set(volatile LONG &v, int x) { InterlockedExchange(&v, x); tuning_save(); }
static void clip_to(float x, float y, float w, float h)
{
    gui.clip = {x, y, x + w, y + h};
    if (gui.mode == PAINT) d_clip(x, y, w, h);
}
static void clip_end()
{
    gui.clip = {0, 0, 1e6f, 1e6f};
    if (gui.mode == PAINT) d_unclip();
}
static bool button(float x, float y, float w, float h, const std::wstring &s, bool on = false, bool enabled = true, const wchar_t *icon = nullptr)
{
    bool hov = enabled && hot(x, y, w, h);
    box(x, y, w, h, h / 2 < 9 ? h / 2 : 9, on ? fade(ACCENT, hov ? 70 : 48) : hov ? FIELD_HI : FIELD, on ? fade(ACCENT, 210) : EDGE);
    Ink c = !enabled ? DIM : on ? ACCENT_HI : TEXT;
    if (icon)
    {
        float tw = gui.mode == PAINT ? d_width(F_BODY, s) : 0, at = x + (w - tw - 24) / 2;
        text(F_ICON, c, at, y, 20, h, A_LEFT, icon);
        text(F_BODY, c, at + 24, y, w, h, A_LEFT, s);
    }
    else text(on ? F_BOLD : F_BODY, c, x + 3, y, w - 6, h, A_CENTER, s);
    return pressed(x, y, w, h, enabled);
}
static bool main_button(float x, float y, float w, float h, const std::wstring &s, const wchar_t *icon = nullptr)
{
    bool hov = hot(x, y, w, h);
    box(x, y, w, h, 10, hov ? ACCENT_HI : ACCENT);
    float tw = icon && gui.mode == PAINT ? d_width(F_BOLD, s) : 0;
    if (icon) { text(F_ICON, ON_ACCENT, x + (w - tw - 26) / 2, y, 22, h, A_LEFT, icon); text(F_BOLD, ON_ACCENT, x + (w - tw - 26) / 2 + 26, y, w, h, A_LEFT, s); }
    else text(F_BOLD, ON_ACCENT, x, y, w, h, A_CENTER, s);
    return pressed(x, y, w, h);
}
static bool toggle(float x, float y, bool on)
{
    bool hov = hot(x, y, 44, 24);
    box(x, y + 1, 44, 22, 11, on ? (hov ? ACCENT_HI : ACCENT) : hov ? FIELD_HI : FIELD, on ? 0 : ink(255, 255, 255, 70));
    if (gui.mode == PAINT) d_circle(x + (on ? 32 : 12), y + 12, on ? 7.5f : 6.5f, on ? ON_ACCENT : ink(255, 255, 255, 200));
    return pressed(x, y, 44, 24);
}
static int slider(int id, float x, float y, float w, int lo, int hi, int def, int v)
{
    float span = w - 18, at = hi > lo ? span * (float)(v - lo) / (float)(hi - lo) : 0;
    gui.changed = gui.released = false;
    if (gui.mode == PAINT)
    {
        box(x + 7, y + 9, span + 4, 4, 2, ink(255, 255, 255, 46));
        box(x + 7, y + 9, at + 2, 4, 2, ACCENT);
        if (def >= lo) fill(x + 8.5f + span * (float)(def - lo) / (float)(hi - lo), y + 17, 1.5f, 4, SOFT);
        bool h = gui.drag == id || hot(x, y, w, 22);
        d_circle(x + 9 + at, y + 11, 9, theme_shade(3), ink(255, 255, 255, 40));
        d_circle(x + 9 + at, y + 11, h ? 6.5f : 5.2f, ACCENT);
        return v;
    }
    if (gui.mode == DOWN && in(x, y - 2, w, 28)) gui.drag = id;
    if (gui.drag == id && span > 0)
    {
        float p = std::min(std::max(gui.mx - x - 9, 0.f), span);
        int n = lo + (int)(p * (float)(hi - lo) / span + 0.5f);
        gui.changed = n != v;
        gui.released = gui.mode == UP;
        v = n;
    }
    return v;
}
static int segments(float x, float y, float w, float h, const std::vector<std::wstring> &names, int now, int enabled = 1 << 30)
{
    box(x, y, w, h, 9, ink(0, 0, 0, 70), EDGE);
    float sw = w / (float)names.size();
    int got = -1;
    for (int i = 0; i < (int)names.size(); i++)
    {
        float sx = x + sw * (float)i;
        bool on = i == now, can = enabled == 1 << 30 || (enabled >> i & 1), hov = can && hot(sx, y, sw, h);
        if (on) box(sx + 3, y + 3, sw - 6, h - 6, 7, ACCENT);
        else if (hov) box(sx + 3, y + 3, sw - 6, h - 6, 7, FIELD_HI);
        text(on ? F_BOLD : F_BODY, on ? ON_ACCENT : can ? TEXT : DIM, sx + 2, y, sw - 4, h, A_CENTER, names[(size_t)i]);
        if (pressed(sx, y, sw, h, can)) got = i;
    }
    return got;
}
static void dots(float x, float y, int n, Ink c)
{
    if (gui.mode == PAINT)
        for (int i = 0; i < 5; i++) d_circle(x + 4 + (float)i * 11, y + 4, 3.6f, i < n ? c : ink(255, 255, 255, 34));
}

// ---- a column of setting cards ----
struct Col { float x, y, w; };
static void heading(Col &c, const std::wstring &s, const std::wstring &note = L"")
{
    text(F_BOLD, SOFT, c.x + 4, c.y, c.w, 28, A_LEFT, s);
    if (!note.empty()) text(F_SMALL, SOFT, c.x + c.w / 3, c.y, c.w * 2 / 3 - 4, 28, A_RIGHT, note);
    c.y += 31;
}
static void note(Col &c, const std::wstring &s, int lines = 1, Ink col = SOFT)
{
    text(F_SMALL, col, c.x + 4, c.y, c.w - 8, 18.f * (float)lines + 3, lines > 1 ? A_WRAP : A_LEFT, s);
    c.y += 18.f * (float)lines + 7;
}
// A card with icon, title and description; the caller puts its control at the right.
static float card_top, card_more; // the top of the last card_row (its returned y moves down when the description wraps)
static float card_row(Col &c, const wchar_t *icon, const std::wstring &t, const std::wstring &d, float h = 60)
{
    float y = c.y, more = 0;
    if (!d.empty()) more = std::max(0.f, std::ceil(d_height(F_SMALL, d, c.w - 64)) - 21.f);
    card_top = y;
    card_more = more;
    box(c.x, y, c.w, h + more, 10, CARD, EDGE);
    text(F_ICON, ACCENT, c.x + 14, y, 24, 60, A_CENTER, icon);
    float th = d.empty() ? 60 : 24;
    text(F_BOLD, TEXT, c.x + 50, d.empty() ? y : y + 9, c.w - 64, th, A_LEFT, t);
    if (!d.empty()) text(F_SMALL, SOFT, c.x + 50, y + 31, c.w - 64, 20 + more, A_WRAP, d);
    c.y += h + more + 6;
    return y + more;
}
static void switch_row(Col &c, const wchar_t *icon, const std::wstring &t, const std::wstring &d, volatile LONG &v)
{
    float y = card_row(c, icon, t, d);
    if (toggle(c.x + c.w - 58, y + 18 - card_more / 2, v != 0)) set(v, !v);
}
static void slider_row(Col &c, const wchar_t *icon, int id, const std::wstring &t, const std::wstring &value, const std::wstring &d, volatile LONG &v, int lo, int hi, int def)
{
    float y = card_row(c, icon, t, d, d.empty() ? 80 : 88);
    text(F_BOLD, ACCENT_HI, c.x + c.w - 190, card_top + 9, 176, 24, A_RIGHT, value);
    int n = slider(id, c.x + 44, y + (d.empty() ? 46.f : 55.f), c.w - 58, lo, hi, def, v);
    if (gui.changed) InterlockedExchange(&v, n);
    if (gui.released) tuning_save();
}
static int segment_row(Col &c, const wchar_t *icon, const std::wstring &t, const std::wstring &d, const std::vector<std::wstring> &names, int now, int enabled = 1 << 30)
{
    float y = card_row(c, icon, t, d, 102);
    return segments(c.x + 14, y + 58, c.w - 28, 34, names, now, enabled);
}

// ---- themes ----
static void repaint_all()
{
    InvalidateRect(app.window, nullptr, FALSE);
    if (ui_window) InvalidateRect(ui_window, nullptr, FALSE);
}
static void mix_row(Col &c, int id, const std::wstring &t, volatile LONG &v, int hi, Ink swatch)
{
    float y = card_row(c, L"\xE790", t, L"", 80);
    if (gui.mode == PAINT) d_circle(c.x + c.w - 30, y + 22, 9, swatch, ink(255, 255, 255, 60));
    int n = slider(id, c.x + 44, y + 46, c.w - 58, 0, hi, -1, v);
    if (gui.changed) { InterlockedExchange(&v, n); theme_mix(); repaint_all(); }
    if (gui.released) tuning_save();
}
static void page_theme(Col &c)
{
    heading(c, tr(L"테마", L"Theme"));
    const auto &l = themes();
    float w = (c.w - 8) / 2;
    for (size_t i = 0; i < l.size(); i++)
    {
        const Theme &t = l[i];
        float x = c.x + (float)(i % 2) * (w + 8), y = c.y + (float)(i / 2) * 74;
        bool on = theme_now() == (int)i, h = hot(x, y, w, 66);
        box(x, y, w, 66, 12, fade(t.base, 235), on ? t.accent : h ? ink(255, 255, 255, 80) : EDGE, on ? 2.f : 1.f);
        if (gui.mode == PAINT)
        {
            clip_to(x + 1, y + 1, w - 2, 64);
            d_glow(x + w, y + 66, 60, fade(t.second, 90));
            d_glow(x, y, 60, fade(t.accent, 90));
            clip_end();
            d_circle(x + 24, y + 33, 10, t.accent);
            d_circle(x + 40, y + 33, 7, t.second);
        }
        text(on ? F_BOLD : F_BODY, ink(246, 247, 248), x + 56, y, w - 64, 66, A_LEFT, tr(t.ko.c_str(), t.en.c_str()));
        if (pressed(x, y, w, 66)) { theme_use((int)i); repaint_all(); return; }
    }
    c.y += (float)((l.size() + 1) / 2) * 74 + 6;
    heading(c, tr(L"내 테마 만들기", L"Make your own"));
    mix_row(c, 40, tr(L"강조 색", L"Accent color"), mine_hue, 359, theme_preview(0));
    mix_row(c, 41, tr(L"강조 색 진하기", L"Accent strength"), mine_sat, 100, theme_preview(0));
    mix_row(c, 42, tr(L"배경 색", L"Background color"), mine_base, 359, theme_preview(1));
    mix_row(c, 43, tr(L"배경 밝기", L"Background brightness"), mine_dark, 40, theme_preview(1));
    mix_row(c, 44, tr(L"빛 번짐", L"Glow"), mine_glow, 120, theme_preview(0));
    if (button(c.x, c.y, w, 36, tr(L"배경 그림 고르기", L"Choose a picture"), false, true, L"\xEB9F"))
    {
        gui.mode = MOVE;
        wchar_t file[MAX_PATH] = L"";
        OPENFILENAMEW o{};
        o.lStructSize = sizeof o;
        o.hwndOwner = ui_open() ? ui_window : app.window;
        o.lpstrFilter = L"*.jpg;*.jpeg;*.png;*.bmp\0*.jpg;*.jpeg;*.png;*.bmp\0";
        o.lpstrFile = file;
        o.nMaxFile = MAX_PATH;
        o.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameW(&o)) { theme_image(file); repaint_all(); }
        return;
    }
    if (button(c.x + w + 8, c.y, w, 36, tr(L"그림 빼기", L"Remove picture"), false, !ini_text(L"theme_image", L"").empty())) { theme_image(L""); repaint_all(); return; }
    c.y += 44;
    if (button(c.x, c.y, w, 36, tr(L"테마 폴더 열기", L"Open theme folder"), false, true, L"\xE838")) ShellExecuteW(nullptr, L"open", theme_folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (button(c.x + w + 8, c.y, w, 36, tr(L"다시 읽기", L"Reload"), false, true, L"\xE72C")) { themes_load(); repaint_all(); return; }
    c.y += 44;
    note(c, tr(L"테마 폴더에 이름.ini 파일을 넣으면 목록에 나와요. 폴더 안의 example.txt를 보고 만드세요. 색은 #RRGGBB로 적어요.", L"Put a .ini file in the theme folder to add a theme. See example.txt in that folder. Colors are written as #RRGGBB."), 3);
}

// ---- presets and models ----
// How early the pictures are made and how long they are shown; the model is a choice of its own.
static const int preset_share[4] = {20, 20, 25, 50}, preset_early[4] = {2, 1, 0, 0};
static int best_model() // for this computer
{
    const BenchResult &r = bench_result;
    if (r.valid && r.best_model >= 0 && r.gpu == sys_info().gpu) return r.model_id[r.best_model]; // measured on this card
    return nvidia_rtx() >= 40 && !vfg_installed() ? 0 : default_model();
}
// The lists: first the models that look right, then those that may break pictures (and stay for slow computers).
static const int model_order[] = {9, 0, 11, 12, 5, 18};
enum { GOOD_MODELS = 4 };
static bool rough(const ModelInfo &m) { return m.quality <= 3; }
static int preset_now()
{
    for (int i = 0; i < 4; i++)
        if (tune.share == preset_share[i] && tune.early == preset_early[i]) return i;
    return -1;
}
static void use_preset(int i)
{
    tune.share = preset_share[i];
    tune.early = preset_early[i];
    tune.margin_x10 = i == 3 ? 3 : 1;
    tuning_save();
}
static void use_model(const ModelInfo &m)
{
    tune.model = m.id;
    if (!m.multi) tune.mult = 2;
    tuning_save();
}
static std::wstring engine_text()
{
    if (num.engine_state == 2 && num.engine_model == tune.model) return tr(L"사용 중", L"In use");
    if (num.engine_state == -1) return tr(L"안 됨", L"Failed");
    return num.engine_state == 1 ? tr(L"준비 중", L"Loading") : L"";
}
static std::atomic<bool> vfg_getting; // the NVIDIA VFG download runs (hidden)
static void offer_vfg()
{
    if (vfg_getting) return;
    HWND owner = ui_open() ? ui_window : app.window;
    int r = MessageBoxW(owner, tr(L"NVIDIA VFG 런타임이 이 PC에 없어요.\n\nNVIDIA 공식 서버(pypi.nvidia.com)에서 받아 필요한 파일 5개만 설치할까요?\n받는 양은 약 416 MB예요. 받으면 NVIDIA 사용 조건에 동의하는 것이 돼요.",
                                  L"NVIDIA's VFG runtime is not on this PC.\n\nDownload it from NVIDIA (pypi.nvidia.com) and install the 5 files that are needed?\nThe download is about 416 MB. By downloading you accept NVIDIA's terms."), app_name(), MB_YESNO | MB_ICONINFORMATION);
    if (r != IDYES || vfg_getting) return;
    // No console window and no key to press: the script runs hidden, its output goes to a log, and the result comes back as a message.
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + Engine::folder() + L"\\engines\\nvvfg\\Get-NvidiaVfg.ps1\"";
    std::wstring log = data_dir() + L"\\vfg-download.log";
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    STARTUPINFOW si{sizeof si};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = si.hStdError = out;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    SetEnvironmentVariableW(L"PK_VFG_NO_PAUSE", L"1");
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    SetEnvironmentVariableW(L"PK_VFG_NO_PAUSE", nullptr);
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    if (!ok)
    {
        MessageBoxW(owner, tr(L"받기를 시작하지 못했어요.", L"Could not start the download."), app_name(), MB_OK | MB_ICONWARNING);
        return;
    }
    CloseHandle(pi.hThread);
    vfg_getting = true;
    logf("vfg: download started");
    std::thread([p = pi.hProcess, log] {
        WaitForSingleObject(p, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(p, &code);
        CloseHandle(p);
        std::string why; // the script's last line on failure
        if (FILE *f = _wfopen(log.c_str(), L"rb"))
        {
            char line[512];
            while (fgets(line, sizeof line, f))
                if (strlen(line) > 2) why = line;
            fclose(f);
        }
        logf("vfg: download ended %lu %s", code, why.c_str());
        vfg_getting = false;
        if (code == 0 && vfg_installed())
            MessageBoxW(nullptr, tr(L"NVIDIA VFG를 받았어요. 이제 모델에서 NVIDIA VFG를 고를 수 있어요.", L"NVIDIA VFG is installed. You can now choose NVIDIA VFG as the model."), app_name(), MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
        else
        {
            int n = MultiByteToWideChar(CP_OEMCP, 0, why.c_str(), -1, nullptr, 0);
            std::wstring w(n > 0 ? n : 1, L'\0');
            if (n > 0) MultiByteToWideChar(CP_OEMCP, 0, why.c_str(), -1, w.data(), n);
            MessageBoxW(nullptr, fmt(tr(L"NVIDIA VFG를 받지 못했어요.\n\n%ls", L"NVIDIA VFG could not be installed.\n\n%ls"), w.c_str()).c_str(), app_name(), MB_OK | MB_ICONWARNING | MB_TOPMOST | MB_SETFOREGROUND);
        }
    }).detach();
}
// The display settings, each with its own switch. numbers: also the numbers over the game (the home page).
static void render_card(Col &c, bool numbers)
{
    static ULONGLONG looked;
    static int monitor = -1, allowed = -1, was = ALLOW_IDLE;
    static bool changed;
    static std::wstring bad;
    if (GetTickCount64() - looked > 3000) { looked = GetTickCount64(); monitor = vrr_monitor(app.window); allowed = vrr_allowed(app.window); }
    int choice = ini_int(L"vrr_profile", -1); // -1: never touched here, so as the driver has it
    bool active = num.vrr == 2, want = choice < 0 ? active : choice != 0, nvidia = num.vrr >= 0;
    std::wstring why;
    int step = vrr_allow_now(why);
    if (step != was) { if (was == ALLOW_BUSY) looked = 0; was = step; }
    bool ask = want && !active && allowed == 0 && monitor != 0, after = false;
    bool alone = choice == 0 && allowed == 1 && monitor != 0 && nvidia && step != ALLOW_BUSY; // off here, on in everything else
    std::wstring hint;
    if (!bad.empty()) hint = fmt(tr(L"드라이버 설정을 바꾸지 못했어요 (%ls)", L"The driver setting could not be changed (%ls)"), bad.c_str());
    else if (want && !active && monitor == 0) hint = tr(L"모니터 메뉴에서도 Adaptive-Sync(FreeSync·VRR)를 켜 주세요. MSI는 '게이밍' 안에 있어요", L"Also turn on Adaptive-Sync (FreeSync/VRR) in the monitor menu");
    else if (step == ALLOW_BUSY) hint = tr(L"이 모니터의 G-SYNC를 바꾸고 있어요. 화면이 잠깐 꺼질 수 있어요", L"Changing G-SYNC for this monitor. The screen may go black briefly");
    else if (ask && step == ALLOW_FAILED) hint = fmt(tr(L"이 모니터에서 켜지 못했어요 (%ls). NVIDIA 제어판의 G-SYNC가 켜져 있는지 봐 주세요", L"Could not turn it on (%ls). Is G-SYNC on in the NVIDIA Control Panel?"), why.c_str());
    else if (ask) hint = tr(L"드라이버가 이 모니터에서는 꺼 두었어요. 켤 때 화면이 1~2초 꺼질 수 있어요", L"Off in the driver for this monitor. The screen may go black for 1-2 s");
    else if (alone) hint = tr(L"다른 프로그램은 켜져 있어서 전체 화면이나 알림 때 화면이 까매질 수 있어요. 켜 보세요", L"On in other programs: the screen may go black in full screen or on notifications");
    else if (want && !active && !changed) hint = allowed == 1 ? app.fullscreen ? L"" : tr(L"준비됐어요. 전체 화면에서 작동해요", L"Ready. It works in full screen") : tr(L"아직 작동하지 않아요. NVIDIA 제어판 > G-SYNC 설정도 확인해 주세요", L"Not active yet. Check NVIDIA Control Panel > Set up G-SYNC");
    int rows = numbers ? 4 : 3;
    float h = 46 + (float)rows * 44 + (hint.empty() ? 0.f : 44.f) + (changed ? 44.f : 0.f) + ((ask && step != ALLOW_BUSY) || alone ? 44.f : 0.f), y = c.y;
    box(c.x, y, c.w, h, 10, CARD, EDGE);
    text(F_ICON, ACCENT, c.x + 14, y + 4, 24, 40, A_CENTER, L"\xE7F4");
    text(F_BOLD, TEXT, c.x + 50, y + 4, c.w - 64, 40, A_LEFT, tr(L"렌더링", L"Rendering"));
    y += 46;
    float lw = c.w * 0.36f, cx = c.x + 16 + lw, cw = c.w - lw - 30;
    auto name = [&](const wchar_t *t) { text(F_BODY, TEXT, c.x + 16, y, c.w * 0.5f, 36, A_LEFT, t); };
    name(tr(L"화면 동기화", L"Display sync"));
    int s = segments(cx, y + 2, cw, 32, {tr(L"수직", L"V-Sync"), tr(L"가변", L"VRR"), tr(L"끔", L"Off")}, tune.sync);
    if (s >= 0) set(tune.sync, s);
    y += 44;
    name(tr(L"최대 프레임 대기", L"Frame queue"));
    float qx = c.x + c.w - 14 - 118;
    box(qx, y + 2, 118, 32, 9, ink(0, 0, 0, 70), EDGE);
    text(F_BOLD, tune.sync == 0 ? TEXT : SOFT, qx + 34, y + 2, 50, 32, A_CENTER, fmt(L"%ld", tune.queue));
    for (int i = 0; i < 2; i++)
    {
        float bx = qx + (i ? 84.f : 0.f);
        bool can = i ? tune.queue < 3 : tune.queue > 1, hov = can && hot(bx, y + 2, 34, 32);
        if (hov) box(bx + 3, y + 5, 28, 26, 7, FIELD_HI);
        text(F_BOLD, can ? TEXT : DIM, bx, y + 1, 34, 32, A_CENTER, i ? L"+" : L"−");
        if (pressed(bx, y + 2, 34, 32, can)) set(tune.queue, tune.queue + (i ? 1 : -1));
    }
    y += 44;
    name(tr(L"G-SYNC 호환", L"G-SYNC compatible"));
    text(F_BODY, active && want ? ACCENT_HI : SOFT, c.x + c.w - 14 - 44 - 100, y, 92, 36, A_RIGHT, active && want ? tr(L"작동 중", L"Active") : want ? tr(L"켬", L"On") : tr(L"끔", L"Off"));
    if (toggle(c.x + c.w - 58, y + 6, want) && nvidia && step != ALLOW_BUSY)
    {
        bool on = !want;
        bad = on ? vrr_driver_on() : vrr_driver_off(true); // off for this program only; the monitor and other programs stay as they are
        if (bad.empty())
        {
            ini_set(L"vrr_profile", on);
            if (on) tune.sync = 1;
            changed = true;
            tuning_save();
            if (on && vrr_allowed(app.window) == 0 && monitor != 0) vrr_allow(app.window, true); // the monitor too
        }
        gui.mode = MOVE; // this press is used up
        repaint_all();
    }
    y += 44;
    if (numbers)
    {
        name(tr(L"프레임 표시", L"Overlay"));
        int o = segments(cx, y + 2, cw, 32, {tr(L"끔", L"Off"), L"fps", tr(L"지연", L"Latency")}, tune.osd);
        if (o >= 0) set(tune.osd, o);
        y += 44;
    }
    if (!hint.empty()) { text(F_SMALL, AMBER, c.x + 16, y - 2, c.w - 32, 40, A_WRAP, hint); y += 44; }
    if (ask && step != ALLOW_BUSY)
    {
        after = button(c.x + 14, y - 2, c.w - 28, 34, tr(L"이 모니터에서 G-SYNC 켜기", L"Turn on G-SYNC for this monitor"), true, true, L"\xE7F4");
        y += 44;
    }
    if (alone)
    {
        after = button(c.x + 14, y - 2, c.w - 28, 34, tr(L"이 모니터에서 모두 끄기", L"Turn off for this monitor"), false, true, L"\xE7F4");
        y += 44;
    }
    if (after)
    {
        vrr_allow(app.window, !alone);
        gui.mode = MOVE;
        repaint_all();
    }
    if (changed && button(c.x + 14, y - 2, c.w - 28, 34, tr(L"다시 켜서 적용", L"Restart to apply"), true, true, L"\xE72C"))
    {
        tuning_save();
        guard_again();
    }
    c.y += h + 6;
}
// Picture quality of the models against real in-between pictures: game recording, 1080p, 60 to 120 fps, 600 pictures, PSNR (higher is better).
static const wchar_t *model_psnr(int id)
{
    switch (id)
    {
    case 9: return L"33.3 dB";
    case 11: return L"33.1 dB";
    case 0: return L"32.9 dB";
    case 12: return L"32.6 dB";
    case 5: return L"32.6 dB";
    case 18: return L"31.0 dB";
    }
    return L"—";
}
static int model_slot(int id)
{
    for (int m = 0; m < bench_result.models; m++)
        if (bench_result.model_id[m] == id) return m;
    return -1;
}
static void open_bench()
{
    if (app.fullscreen) app_fullscreen(false);
    app.bench_page = true;
    app_screen_changed();
}

// ---- pages ----
static float gauge, gauge_target; // 0: the original's frame rate reaches the screen, 1: the wanted one does
static Area ring_area;
static ULONGLONG ring_seen;
// The round switch: shows what is going on and turns generation on and off.
static void power(Col &c, float r)
{
    float cx = c.x + c.w / 2, cy = c.y + r + 18;
    bool trouble = !app.problem.empty() || !app.signal, on = tune.on != 0, hov = hot(cx - r, cy - r, 2 * r, 2 * r);
    Ink ring = trouble ? AMBER : ACCENT;
    ring_area = {cx - r - 40, cy - r - 40, cx + r + 40, cy + r + 40};
    mark(MK_POWER, cx - r - 8, cy - r - 8, 2 * r + 16, 2 * r + 16);
    if (gui.mode == PAINT)
    {
        // The ring: grey at the original's frame rate, all green at the wanted one.
        if (gauge > 0.01f || hov) d_glow(cx, cy, r + 34, fade(ring, (int)(28 + 50 * gauge) + (hov ? 20 : 0)));
        d_circle(cx, cy, r, fade(theme_shade(0.75f), 190), ink(255, 255, 255, 52), 8);
        d_arc(cx, cy, r, trouble ? 1.f : gauge, fade(ring, 70), 14);
        d_arc(cx, cy, r, trouble ? 1.f : gauge, ring, 8);
    }
    if (trouble) text(F_HEAD, TEXT, cx - r, cy - 16, 2 * r, 34, A_CENTER, app.problem.empty() ? tr(L"신호 대기", L"No signal") : tr(L"확인 필요", L"Check needed"));
    else if (on)
    {
        text(F_BIG, TEXT, cx - r, cy - 44, 2 * r, 42, A_CENTER, num.engine_state == 1 ? std::wstring(tr(L"준비 중", L"Loading")) : fmt(L"%.0f fps", app_source_fps() * (1 + gauge * ((model_by_id(tune.model)->multi ? (double)tune.mult : 2.0) - 1))));
        text(F_SMALL, SOFT, cx - r, cy - 2, 2 * r, 22, A_CENTER, num.engine_state == -1 ? std::wstring(tr(L"이 모델은 안 돼요", L"Model not available")) : fmt(tr(L"원본 %.0f fps", L"Source %.0f fps"), app_source_fps()));
    }
    else
    {
        text(F_TITLE, TEXT, cx - r, cy - 40, 2 * r, 36, A_CENTER, tr(L"꺼짐", L"Off"));
        text(F_SMALL, SOFT, cx - r, cy - 2, 2 * r, 22, A_CENTER, fmt(tr(L"원본 %.0f fps", L"%.0f fps"), app_source_fps()));
    }
    if (gui.mode == PAINT) ring_seen = GetTickCount64();
    text(F_SMALL, on ? ACCENT_HI : SOFT, cx - r, cy + 28, 2 * r, 22, A_CENTER, on ? tr(L"눌러서 끄기", L"Click to turn off") : tr(L"눌러서 켜기", L"Click to turn on"));
    if (pressed(cx - r, cy - r, 2 * r, 2 * r)) app_generation(!on);
    c.y += 2 * r + 34;
    std::wstring s = app.signal ? fmt(tr(L"지연 %.1f ms   ·   %dp %.0f fps 입력", L"Latency %.1f ms   ·   %dp %.0f fps input"), num.latency.get(), app.current.h, app.current.fps) : app_status_line();
    text(F_BODY, SOFT, c.x, c.y, c.w, 24, A_CENTER, s);
    c.y += 32;
}
static void multiplier(Col &c)
{
    const ModelInfo *info = model_by_id(tune.model);
    int target = (int)(1000.0 / std::max(5.0, num.interval.get()) + 0.5) * (int)tune.mult;
    bool over = info->multi && tune.mult > 2 && num.hz_mhz && target > num.hz_mhz / 1000 + 5;
    int m = segment_row(c, L"\xE916", tr(L"배수", L"Multiplier"), !info->multi ? std::wstring(tr(L"이 모델은 2배만 돼요", L"This model supports 2x only")) : over ? fmt(tr(L"%d fps는 모니터(%ld Hz)보다 많아요", L"%d fps is above the monitor (%ld Hz)"), target, num.hz_mhz / 1000) : std::wstring(tr(L"원본 프레임을 몇 배로 늘릴지", L"Frames shown per source frame")),
                        {tr(L"2배", L"2x"), tr(L"3배", L"3x"), tr(L"4배", L"4x")}, info->multi ? (int)tune.mult - 2 : 0, info->multi ? 1 << 30 : 1);
    if (m >= 0) set(tune.mult, m + 2);
}
// Recording what is on the screen (made pictures too) with the sound, to Videos\Peussok.
static void record_row(Col &c)
{
    bool on = rec_on();
    std::wstring say, e = rec_error();
    if (on)
    {
        int s = (int)rec_seconds();
        say = fmt(tr(L"\x25CF 녹화 중 %d:%02d · %ls · %lld장", L"\x25CF Recording %d:%02d · %ls · %lld frames"), s / 60, s % 60, rec_codec(), rec_pictures());
        if (rec_dropped() > 0) say += fmt(tr(L" · 빠짐 %lld", L" · left out %lld"), rec_dropped());
    }
    else if (!e.empty()) say = e;
    else say = tr(L"보이는 화면 그대로 (생성 프레임·소리 포함) · Ctrl+Alt+R", L"What you see, made frames and sound included · Ctrl+Alt+R");
    float y = card_row(c, on ? L"\xE7C8" : L"\xE714", tr(L"녹화", L"Record"), L"", 88);
    text(F_SMALL, on ? ACCENT_HI : !e.empty() ? AMBER : SOFT, c.x + 50, y + 58, c.w - 64, 20, A_LEFT, say); // a line of its own under the buttons
    if (button(c.x + c.w - 196, y + 13, 90, 34, on ? tr(L"멈추기", L"Stop") : tr(L"시작", L"Start"), on)) app_video_record(!on);
    if (button(c.x + c.w - 100, y + 13, 86, 34, tr(L"폴더", L"Folder"))) ShellExecuteW(nullptr, L"open", rec_folder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
static void volume_row(Col &c)
{
    float y = card_row(c, audio_mute ? L"\xE74F" : L"\xE767", tr(L"소리", L"Sound"), L"");
    int v = slider(1, c.x + 110, y + 19, c.w - 236, 0, 100, -1, audio_volume);
    if (gui.changed) InterlockedExchange(&audio_volume, v);
    if (gui.released) tuning_save();
    text(F_BODY, TEXT, c.x + c.w - 124, y, 34, 60, A_RIGHT, fmt(L"%ld", audio_volume));
    if (button(c.x + c.w - 82, y + 14, 68, 32, tr(L"음소거", L"Mute"), audio_mute != 0)) set(audio_mute, !audio_mute);
}
// The switch of DLSS 5 and, when it is on, how strong; true: something was changed.
static bool nr_row(Col &c)
{
    static int held = 0; // where the slider is while it is held
    int there = nr_there();
    std::wstring say;
    std::string e = video_nr_error();
    bool on = there > 0 && tune.nr;
    int now = held ? held : (int)tune.nr_layers;
    double one = nr_ms(), fps = app_source_fps();
    bool heavy = one > 0 && fps > 10 && one > 400.0 / fps; // the model needs its time too
    if (there < 0) say = tr(L"파일을 찾는 중", L"Looking for the file");
    else if (!there) say = tr(L"게임에 들어 있는 NVIDIA 파일이 필요해요", L"Needs an NVIDIA file from a game");
    else if (!on) say = tr(L"게임 화면을 실사 느낌으로 (RTX 50)", L"Neural rendering (RTX 50)");
    else if (!e.empty()) say = fmt(tr(L"쓸 수 없어요: %hs", L"Not available: %hs"), e.c_str());
    else say = one > 0 ? fmt(tr(L"작동 중 · %.1f ms", L"Active · %.1f ms"), one) : std::wstring(tr(L"준비 중", L"Loading"));
    float y = card_row(c, L"\xE793", tr(L"DLSS 5", L"DLSS 5"), say, on ? 176.f : 60.f);
    if (on)
    {
        fill(c.x + 14, y + 62, c.w - 28, 1, EDGE);
        text(F_BODY, TEXT, c.x + 50, y + 66, 150, 26, A_LEFT, tr(L"강도", L"Strength"));
        text(F_BOLD, heavy ? AMBER : ACCENT_HI, c.x + c.w - 190, y + 66, 176, 26, A_RIGHT, heavy ? fmt(tr(L"%d / 5 · 끊길 수 있어요", L"%d / 5 · may stutter"), now) : fmt(L"%d / 5", now));
        int n = slider(20, c.x + 44, y + 92, c.w - 58, 1, 5, 1, now);
        if (gui.changed) held = n;
        if (gui.released)
        {
            held = 0;
            if (n != tune.nr_layers)
            {
                set(tune.nr_layers, n);
                gui.mode = MOVE;
                repaint_all();
                return true;
            }
        }
        text(F_BODY, TEXT, c.x + 50, y + 118, 170, 26, A_LEFT, tr(L"피부 부드럽게", L"Softer skin"));
        text(F_BOLD, ACCENT_HI, c.x + c.w - 190, y + 118, 176, 26, A_RIGHT, tune.nr_skin ? fmt(L"%ld%%", tune.nr_skin) : std::wstring(tr(L"끔", L"Off")));
        n = slider(21, c.x + 44, y + 144, c.w - 58, 0, 100, 50, tune.nr_skin);
        if (gui.changed) InterlockedExchange(&tune.nr_skin, n);
        if (gui.released) tuning_save();
    }
    if (there <= 0 || !toggle(c.x + c.w - 58, y + 18 - card_more / 2, tune.nr != 0)) return false;
    set(tune.nr, !tune.nr);
    gui.mode = MOVE;
    repaint_all();
    return true;
}
// The switch of HDR; true: it was pressed.
static bool hdr_row(Col &c)
{
    bool on = num.hdr == 2;
    float y = card_row(c, L"\xE706", L"HDR", hdr_busy() ? tr(L"바꾸고 있어요. 화면이 잠깐 꺼져요", L"Changing. The screen may go black briefly") : on && num.hdr_out ? fmt(tr(L"작동 중 · 최대 %ld nit", L"Active · peak %ld nits"), num.hdr_peak) : on ? std::wstring(tr(L"Windows HDR만 켜져 있어요", L"Only Windows HDR is on")) : std::wstring(tr(L"켤 때 화면이 1~2초 꺼질 수 있어요", L"The screen may go black for 1-2 s")));
    if (!toggle(c.x + c.w - 58, y + 18 - card_more / 2, on && tune.hdr) || hdr_busy()) return false;
    bool want = !(on && tune.hdr);
    set(tune.hdr, want);
    if (want && !on) { ini_set(L"hdr_ours", 1); hdr_set(app.window, true); }
    else if (!want && on && ini_int(L"hdr_ours", 0)) { ini_set(L"hdr_ours", 0); hdr_set(app.window, false); } // Windows HDR the person had on stays on
    gui.mode = MOVE;
    repaint_all();
    return true;
}
// The model in use; pressed, it opens the list.
static void model_card(Col &c)
{
    static bool open;
    const ModelInfo *now = model_by_id(tune.model);
    int best = best_model(), n = open ? model_count : 0;
    bool warn = rough(*now) && !open;
    float h = 52 + (float)n * 42 + (open ? 72.f : warn ? 26.f : 0.f), y = c.y;
    bool hov = hot(c.x, y, c.w, 52);
    box(c.x, y, c.w, h, 10, hov && !open ? CARD_HI : CARD, open ? fade(ACCENT, 120) : EDGE);
    text(F_ICON, ACCENT, c.x + 14, y, 24, 52, A_CENTER, L"\xE950");
    text(F_BOLD, TEXT, c.x + 50, y, 80, 52, A_LEFT, tr(L"모델", L"Model"));
    text(F_BOLD, ACCENT_HI, c.x + 100, y, c.w - 100 - 40, 52, A_RIGHT, now->name);
    text(F_ICON, SOFT, c.x + c.w - 34, y, 24, 52, A_CENTER, open ? L"\xE70E" : L"\xE70D");
    bool flip = pressed(c.x, y, c.w, 52);
    if (warn) text(F_SMALL, AMBER, c.x + 50, y + 44, c.w - 64, 26, A_LEFT, best != now->id && !rough(*model_by_id(best)) ? fmt(tr(L"깨져 보일 수 있어요. 추천: %ls", L"May show artifacts. Better: %ls"), model_by_id(best)->name) : std::wstring(tr(L"깨져 보일 수 있는 모델이에요", L"May show artifacts")));
    if (open)
    {
        text(F_SMALL, ACCENT_HI, c.x + 16, y + 50, c.w - 32, 26, A_LEFT, tr(L"추천 모델", L"Recommended"));
        float ly = y + 52 + 26 + GOOD_MODELS * 42;
        fill(c.x + 14, ly + 4, c.w - 28, 1, EDGE);
        text(F_SMALL, AMBER, c.x + 16, ly + 8, c.w - 32, 26, A_LEFT, tr(L"비추천 · 화면이 깨져 보일 수 있어요", L"Not recommended (may show artifacts)"));
    }
    for (int i = 0; i < n; i++)
    {
        const ModelInfo &m = *model_by_id(model_order[i]);
        float ry = y + 52 + 26 + (float)i * 42 + (i >= GOOD_MODELS ? 36.f : 0.f);
        bool on = m.id == tune.model, can = !m.nvidia || nvidia_rtx(), over = can && hot(c.x + 6, ry, c.w - 12, 38);
        if (on || over) box(c.x + 6, ry, c.w - 12, 38, 8, on ? fade(ACCENT, 40) : CARD_HI);
        if (gui.mode == PAINT) d_circle(c.x + 24, ry + 19, 6, 0, can ? (on ? ACCENT : ink(255, 255, 255, 110)) : DIM, on ? 5 : 1.5f);
        std::wstring tail = !can ? tr(L"RTX 필요", L"Needs RTX") : m.id == 9 && !vfg_installed() ? (vfg_getting ? tr(L"받는 중", L"Downloading") : tr(L"받기", L"Get")) : on && num.engine_state == -1 ? tr(L"안 됨", L"Failed") : on && num.engine_state == 1 ? tr(L"준비 중", L"Loading") : m.id == best && !rough(m) ? tr(L"추천", L"Best here") : L"";
        text(on ? F_BOLD : F_BODY, can ? rough(m) && !on ? SOFT : TEXT : DIM, c.x + 42, ry, c.w - 42 - 116, 38, A_LEFT, m.name);
        if (!tail.empty()) text(F_SMALL, on && num.engine_state == -1 ? AMBER : m.id == best ? ACCENT_HI : SOFT, c.x + c.w - 136, ry, 56, 38, A_RIGHT, tail);
        dots(c.x + c.w - 72, ry + 15, m.quality, can ? SKY : DIM);
        if (pressed(c.x + 6, ry, c.w - 12, 38, can))
        {
            open = false;
            gui.mode = MOVE; // this press is used up: nothing below it may take it
            if (m.id == 9 && !vfg_installed()) offer_vfg();
            else if (!on) use_model(m);
            repaint_all();
            break;
        }
    }
    if (flip) { open = !open; gui.mode = MOVE; repaint_all(); }
    c.y += h + 6;
}
static void page_home(Col &c)
{
    power(c, 84);
    if (!app.problem.empty())
    {
        note(c, app.problem, 2, AMBER);
        if (button(c.x, c.y, c.w, 36, tr(L"캡처보드 다시 찾기", L"Search for the capture card again"))) { app_refresh_devices(); app_open(); }
        c.y += 44;
    }
    const wchar_t *n[4] = {tr(L"초저지연", L"Instant"), tr(L"저지연", L"Fast"), tr(L"균형", L"Balanced"), tr(L"화질 우선", L"Quality")}, *ic[4] = {L"\xE945", L"\xEC4A", L"\xE9F5", L"\xE7B3"};
    const wchar_t *a[4] = {tr(L"입력 지연이 가장 짧아요 · 화면 아래쪽은 덜 정확해요", L"Fastest response, bottom less exact"), tr(L"지연이 짧고 화면 깨짐이 적어요", L"Fast response, fewer artifacts"), tr(L"지연과 화질의 균형 (권장)", L"Good response and quality (default)"), tr(L"움직임이 가장 부드러워요 · 지연은 늘어요", L"Smoothest motion, slower response")};
    int now = preset_now();
    float w = (c.w - 24) / 4;
    mark(MK_PRESETS, c.x, c.y, c.w, 84);
    for (int i = 0; i < 4; i++)
    {
        float x = c.x + (float)i * (w + 8);
        bool sel = now == i, h = hot(x, c.y, w, 84);
        box(x, c.y, w, 84, 12, sel ? fade(ACCENT, 46) : h ? CARD_HI : CARD, sel ? ACCENT : EDGE, sel ? 1.6f : 1);
        text(F_ICON_BIG, sel ? ACCENT_HI : SOFT, x, c.y + 12, w, 32, A_CENTER, ic[i]);
        text(sel ? F_BOLD : F_BODY, sel ? ACCENT_HI : TEXT, x, c.y + 48, w, 26, A_CENTER, n[i]);
        if (pressed(x, c.y, w, 84)) use_preset(i);
    }
    c.y += 90;
    bool waits = tune.model == 100, early = preset_early[now >= 0 ? now : 2] && (tune.mult > 2 || waits); // these do not start early
    text(F_SMALL, SOFT, c.x, c.y, c.w, 22, A_CENTER, waits ? std::wstring(tr(L"이 모델은 원본을 바로 보여 줘요", L"This model shows source frames at once")) : early ? std::wstring(tr(L"2배에서만 더 빨라져요", L"Faster with 2x only")) : now >= 0 ? std::wstring(a[now]) : std::wstring(tr(L"사용자 설정", L"Custom settings")));
    c.y += 30;
    float my = c.y;
    model_card(c);
    mark(MK_MODEL, c.x, my, c.w, c.y - my - 6);
    my = c.y;
    multiplier(c);
    mark(MK_MULT, c.x, my, c.w, c.y - my - 6);
    if (nr_there() > 0 && nr_row(c)) return; // only where there is something to switch on
    c.y += 4;
    if (main_button(c.x, c.y, c.w, 46, app.fullscreen ? tr(L"창으로 보기", L"Back to window") : tr(L"전체 화면으로 시작", L"Start full screen"), L"\xE740")) { app_fullscreen(!app.fullscreen); return; }
    c.y += 54;
    if (button(c.x, c.y, c.w, 40, tr(L"내 컴퓨터에 맞는 설정 찾기", L"Find the best settings for this PC"), false, true, L"\xEC4A")) { open_bench(); return; }
    c.y += 46;
    note(c, fmt(tr(L"F11 전체 화면 · F1 설정 · %ls 켜고 끄기", L"F11 full screen · F1 panel · %ls on/off"), app_hotkey_name().c_str()));
    note(c, tr(L"자세한 설정: 생성 · 화질 · 화면 · 입력 · 소리 · 녹화 탭", L"More: the Generation, Quality, Display, Input, Sound and Record tabs"));
}
static void model_rows(Col &c)
{
    heading(c, tr(L"모델", L"Model"));
    text(F_SMALL, SOFT, c.x + c.w - 146, c.y - 31, 60, 28, A_LEFT, tr(L"속도", L"Speed"));
    text(F_SMALL, SOFT, c.x + c.w - 76, c.y - 31, 60, 28, A_LEFT, tr(L"화질", L"Quality"));
    for (int i = 0; i < model_count; i++)
    {
        const ModelInfo &m = *model_by_id(model_order[i]);
        if (i == GOOD_MODELS) { text(F_BODY, AMBER, c.x + 4, c.y, c.w, 28, A_LEFT, tr(L"비추천 · 화면이 깨져 보일 수 있어요", L"Not recommended (may show artifacts)")); c.y += 31; }
        bool on = tune.model == m.id, can = !m.nvidia || nvidia_rtx(), h = can && hot(c.x, c.y, c.w, 50);
        box(c.x, c.y, c.w, 50, 10, on ? fade(ACCENT, 40) : h ? CARD_HI : CARD, on ? ACCENT : EDGE, on ? 1.5f : 1);
        if (gui.mode == PAINT) d_circle(c.x + 22, c.y + 25, 7, 0, can ? (on ? ACCENT : ink(255, 255, 255, 110)) : DIM, on ? 5 : 1.5f);
        text(F_BOLD, can ? TEXT : DIM, c.x + 42, c.y + 4, c.w - 196, 24, A_LEFT, m.name);
        std::wstring sub = !can ? tr(L"NVIDIA RTX 필요", L"Needs NVIDIA RTX") : m.id == 9 && !vfg_installed() ? (vfg_getting ? tr(L"NVIDIA 파일 받는 중 (약 416 MB)", L"Downloading NVIDIA files (about 416 MB)") : tr(L"눌러서 NVIDIA 파일 받기", L"Click to get NVIDIA files")) : on ? engine_text() : fmt(tr(L"화질 %ls", L"Quality %ls"), model_psnr(m.id));
        int slot = model_slot(m.id), size = num.h >= 2100 ? 2 : num.h >= 1400 ? 1 : 0;
        if (can && bench_result.valid && slot >= 0 && bench_result.cell[slot][size].state == 1) sub += fmt(tr(L" · %ls %d배까지", L" · %ls up to %dx"), bench_size_name(size), bench_result.cell[slot][size].mult60);
        text(F_SMALL, on && num.engine_state == -1 ? AMBER : SOFT, c.x + 42, c.y + 26, c.w - 196, 20, A_LEFT, sub);
        dots(c.x + c.w - 146, c.y + 21, m.speed, can ? ACCENT : DIM);
        dots(c.x + c.w - 76, c.y + 21, m.quality, can ? SKY : DIM);
        if (pressed(c.x, c.y, c.w, 50, can))
        {
            if (m.id == 9 && !vfg_installed()) { gui.mode = MOVE; offer_vfg(); return; }
            if (!on) use_model(m);
        }
        c.y += 55;
    }
    if (num.engine_state == -1 && tune.on)
    {
        std::string why = video_engine_error();
        note(c, fmt(tr(L"사용 불가 이유: %hs", L"Reason: %hs"), why.c_str()), 2, AMBER);
    }
    c.y += 4;
}
static void page_generate(Col &c)
{
    float y = card_row(c, L"\xE945", tr(L"프레임 생성", L"Frame generation"), app_status_line());
    if (toggle(c.x + c.w - 58, y + 18 - card_more / 2, tune.on != 0)) app_generation(!tune.on);
    model_rows(c);
    multiplier(c);
    heading(c, tr(L"지연", L"Latency"));
    double refresh = num.hz_mhz ? 1000000.0 / num.hz_mhz : 0, share = num.share.get();
    slider_row(c, L"\xE916", 10, tr(L"생성 프레임 표시 시간", L"Generated frame time"), share > 0 ? fmt(L"%ld%% · %.1f ms%ls", tune.share, share, tune.sync && refresh > share ? tr(L" (<1주기)", L" (<1 refresh)") : L"") : fmt(L"%ld%%", tune.share), tr(L"짧으면 지연이 줄고 길면 움직임이 고르게 보여요", L"Shorter: less latency. Longer: smoother"), tune.share, 20, 50, 25);
    slider_row(c, L"\xE81C", 11, tr(L"여유 시간", L"Safety margin"), fmt(L"%.1f ms", tune.margin_x10 / 10.0), tr(L"늘리면 끊김이 줄고, 줄이면 지연이 짧아져요", L"Higher: steadier. Lower: less latency"), tune.margin_x10, 0, 30, 1);
    int e = segment_row(c, L"\xE945", tr(L"선행 생성", L"Start early"), num.ahead.get() > 50 ? tr(L"작동 중 · 화면 아래쪽 일부는 예측해서 채워요", L"Active. The bottom part is estimated") : tr(L"프레임이 다 들어오기 전에 생성을 시작해 지연을 줄여요", L"Starts before the full frame arrives"), {tr(L"끔", L"Off"), tr(L"약", L"Low"), tr(L"강", L"High")}, tune.early);
    if (e >= 0) set(tune.early, e);
    heading(c, tr(L"생성 프레임 보호", L"Protecting made frames"));
    switch_row(c, L"\xE7B5", tr(L"HUD 보호", L"HUD protection"), tr(L"고정된 HUD가 일그러지지 않게 해요. 대화창이 깨지면 끄세요", L"Keeps fixed HUD and text steady"), tune.hud);
    switch_row(c, L"\xE8D2", tr(L"글자 보호", L"Text protection"), tr(L"자막·안내 문구가 뜨자마자 보호해요", L"Protects subtitles as soon as they appear"), tune.text);
    switch_row(c, L"\xE77B", tr(L"얼굴·움직임 보정 (실험)", L"Face and motion fix (trial)"), tr(L"생성 프레임에서 움직임이 어긋난 부분을 바로잡아요", L"Fixes areas that do not match the motion"), tune.repair);
    slider_row(c, L"\xE8B2", 12, tr(L"장면 전환 감지", L"Scene cut detection"), tune.cut ? fmt(L"%ld", tune.cut) : tr(L"끔", L"Off"), tr(L"장면이 바뀔 때 겹쳐 보이면 값을 낮추세요", L"Lower it if scene cuts look blended"), tune.cut, 0, 96, 40);
    heading(c, L"RIFE");
    int f = segment_row(c, L"\xE7C2", tr(L"RIFE 모션 계산 해상도", L"RIFE motion size"), tr(L"낮을수록 빨라요. 자동은 PC 성능에 맞춰 정해요", L"Smaller is faster. Auto adapts to this PC"), {tr(L"자동", L"Auto"), L"1/2", L"1/4", L"1/8"}, tune.flow);
    if (f >= 0) set(tune.flow, f);
}
static void page_picture(Col &c)
{
    heading(c, tr(L"선명도", L"Sharpness"));
    slider_row(c, L"\xE7B3", 13, tr(L"선명도", L"Sharpness"), tune.sharpen ? fmt(L"%ld%%", tune.sharpen) : tr(L"끔", L"Off"), L"", tune.sharpen, 0, 100, -1);
    switch_row(c, L"\xE7A9", tr(L"생성 프레임만 샤프닝", L"Sharpen generated frames only"), tr(L"원본 프레임은 그대로 두고 생성 프레임만 보정해요", L"Source frames stay untouched"), tune.sharpen_made);
    heading(c, tr(L"색", L"Color"));
    int f = segment_row(c, L"\xE790", tr(L"색 범위", L"Color range"), tr(L"검은색이 뜨거나 뭉개지면 바꿔 보세요", L"Change it if black looks gray or crushed"), {tr(L"자동", L"Automatic"), tr(L"제한", L"Limited"), tr(L"전체", L"Full")}, tune.range == 0 ? 0 : tune.range == 2 ? 1 : 2);
    if (f >= 0) set(tune.range, f == 0 ? 0 : f == 1 ? 2 : 1);
    if (nr_built())
    {
        heading(c, tr(L"DLSS 5", L"DLSS 5"));
        if (nr_row(c)) return;
        if (tune.nr)
        {
            // ready-made looks, and everything on its own
            static const int looks[4][6] = {{1, 100, 100, 100, 0, 0}, {2, 100, 110, 90, 20, 0}, {4, 100, 120, 90, 30, 0}, {3, 100, 120, 100, 20, 1}}; // step, strength, detail, light, colour kept, style
            int look = -1;
            for (int i = 0; i < 4; i++)
                if (tune.nr_layers == looks[i][0] && tune.nr_power == looks[i][1] && tune.nr_detail == looks[i][2] && tune.nr_tone == looks[i][3] && tune.nr_keep == looks[i][4] && tune.nr_style == looks[i][5]) look = i;
            int k = segment_row(c, L"\xE735", tr(L"프리셋", L"Preset"), look < 0 ? tr(L"사용자 설정", L"Custom settings") : tr(L"아래에서 세부 조정할 수 있어요", L"Fine-tune it below"), {tr(L"약하게", L"Light"), tr(L"실사", L"Real"), tr(L"아주 실사", L"Strong"), tr(L"선명", L"Vivid")}, look);
            if (k >= 0)
            {
                tune.nr_layers = looks[k][0];
                tune.nr_power = looks[k][1];
                tune.nr_detail = looks[k][2];
                tune.nr_tone = looks[k][3];
                tune.nr_keep = looks[k][4];
                tune.nr_style = looks[k][5];
                tuning_save();
            }
            slider_row(c, L"\xE9E9", 14, tr(L"세기", L"Amount"), fmt(L"%ld%%", tune.nr_power), L"", tune.nr_power, 0, 200, 100);
            slider_row(c, L"\xE7B3", 15, tr(L"디테일", L"Detail"), fmt(L"%ld%%", tune.nr_detail), tr(L"옷감·피부·바닥의 질감", L"Texture of cloth, skin and ground"), tune.nr_detail, 0, 200, 100);
            slider_row(c, L"\xE706", 16, tr(L"명암", L"Light"), fmt(L"%ld%%", tune.nr_tone), tr(L"어두워 보이면 줄이세요", L"Lower it if it looks too dark"), tune.nr_tone, 0, 200, 100);
            slider_row(c, L"\xE790", 17, tr(L"원래 색 유지", L"Keep original color"), fmt(L"%ld%%", tune.nr_keep), tr(L"색이 이상하면 늘리세요", L"Raise it if colors look wrong"), tune.nr_keep, 0, 100, 0);
            k = segment_row(c, L"\xE790", tr(L"스타일", L"Style"), L"", {tr(L"기본", L"Default"), tr(L"자연", L"Natural"), tr(L"영화", L"Cinematic")}, tune.nr_style);
            if (k >= 0) set(tune.nr_style, k);
            k = segment_row(c, L"\xE7C2", tr(L"처리 해상도", L"Processing size"), tr(L"낮을수록 빨라요. 디테일은 원본 해상도로 살려요", L"Smaller is faster. Sharpness is kept"), {tr(L"빠름", L"Fast"), tr(L"보통", L"Normal"), tr(L"원본", L"Full")}, tune.nr_size);
            if (k >= 0) set(tune.nr_size, k);
            k = segment_row(c, L"\xE8CB", tr(L"적용 순서", L"Order"), tune.nr_order ? tr(L"원본 프레임에 먼저 적용한 뒤 생성해요. 움직임이 크면 깜빡일 수 있어요", L"Applied to the originals, then frames are made from them. May flicker in fast motion")
                                                                                     : tr(L"프레임을 먼저 생성한 뒤 적용해요. 깜빡임이 가장 적어요", L"Frames are made first and the change is laid over them. Least flicker"),
                            {tr(L"생성 뒤 (기본)", L"After (default)"), tr(L"생성 전", L"Before")}, tune.nr_order);
            if (k >= 0) set(tune.nr_order, k);
            slider_row(c, L"\xE9F5", 22, tr(L"깜빡임 억제", L"Flicker reduction"), fmt(L"%ld%%", tune.nr_steady), tr(L"높을수록 질감 떨림이 줄어요. 너무 높으면 움직일 때 잔상이 생겨요", L"Higher: steadier texture. Too high: trails in motion"), tune.nr_steady, 0, 95, 90);
        }
        static std::wstring bad;
        if (!bad.empty()) note(c, bad, 2, AMBER);
        if (button(c.x, c.y, c.w, 36, tr(L"파일 고르기 (nvngx_dlssnr.dll)", L"Choose the file (nvngx_dlssnr.dll)"), false, true, L"\xE8E5"))
        {
            gui.mode = MOVE;
            wchar_t f[MAX_PATH] = L"";
            OPENFILENAMEW o{};
            o.lStructSize = sizeof o;
            o.hwndOwner = ui_open() ? ui_window : app.window;
            o.lpstrFilter = L"nvngx_dlssnr.dll\0nvngx_dlssnr.dll\0";
            o.lpstrFile = f;
            o.nMaxFile = MAX_PATH;
            o.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&o)) bad = nr_choose(f);
            repaint_all();
            return;
        }
        c.y += 46;
        note(c, tr(L"이 파일은 프쏙에 들어 있지 않아요. 켜면 선행 생성은 꺼져요. 고정된 HUD는 원래 색을 유지해요", L"The file is not included. Start early does not work with it. A fixed HUD keeps its colors"), 2);
    }
}
// A row with a field that shows the chosen value; a click opens the list (the system's own menu). The index chosen, or -1.
// A choice like the model list: the row opens in place and lists the choices in the program's own look (not a Windows menu).
static std::wstring choice_open; // the title of the open one
static int choice_row(Col &c, const wchar_t *icon, const std::wstring &t, const std::vector<std::wstring> &names, int now, const std::wstring &d = L"")
{
    bool open = choice_open == t && !names.empty();
    float list = open ? 8.f + (float)names.size() * 40.f : 0.f;
    float y = card_row(c, icon, t, d, 60 + list);
    float bw = std::min(c.w * 0.56f, 340.f), bx = c.x + c.w - bw - 12, by = y + 12;
    bool h = hot(bx, by, bw, 36);
    box(bx, by, bw, 36, 8, h || open ? FIELD_HI : FIELD, h || open ? ACCENT : EDGE);
    text(F_BODY, TEXT, bx + 12, by, bw - 44, 36, A_LEFT, now >= 0 && now < (int)names.size() ? names[(size_t)now] : std::wstring(L"—"));
    text(F_ICON, SOFT, bx + bw - 32, by, 24, 36, A_CENTER, open ? L"\xE70E" : L"\xE70D");
    if (pressed(bx, by, bw, 36) && !names.empty())
    {
        choice_open = open ? L"" : t;
        gui.mode = MOVE;
        repaint_all();
        return -1;
    }
    if (!open) return -1;
    fill(c.x + 14, y + 60, c.w - 28, 1, EDGE);
    for (size_t i = 0; i < names.size(); i++)
    {
        float ry = y + 64 + (float)i * 40;
        bool on = (int)i == now, hv = hot(c.x + 8, ry, c.w - 16, 38);
        if (on || hv) box(c.x + 8, ry, c.w - 16, 38, 8, on ? fade(ACCENT, 40) : CARD_HI, on ? fade(ACCENT, 120) : CARD_HI);
        if (on) text(F_ICON, ACCENT, c.x + 16, ry, 24, 38, A_CENTER, L"\xE73E");
        text(F_BODY, on ? ACCENT_HI : TEXT, c.x + 50, ry, c.w - 64, 38, A_LEFT, names[i]);
        if (pressed(c.x + 8, ry, c.w - 16, 38))
        {
            choice_open.clear();
            gui.mode = MOVE;
            repaint_all();
            return (int)i;
        }
    }
    return -1;
}
// The order kinds are offered in, and taken when the user leaves the kind to the program: no decoding first, compressed last.
static int kind_rank(Pixel p)
{
    static const Pixel order[] = {Pixel::nv12, Pixel::p010, Pixel::yuy2, Pixel::uyvy, Pixel::i420, Pixel::yv12, Pixel::bgra, Pixel::bgr24, Pixel::mjpg};
    for (int i = 0; i < (int)(sizeof order / sizeof order[0]); i++) if (order[i] == p) return i;
    return 99;
}
static std::wstring kind_name(Pixel p)
{
    switch (p)
    {
    case Pixel::nv12: return tr(L"NV12 (가장 빨라요)", L"NV12 (fastest)");
    case Pixel::p010: return L"P010 (10bit)";
    case Pixel::mjpg: return tr(L"MJPG (압축, USB 캡처보드)", L"MJPG (compressed, USB cards)");
    default: return std::wstring(pixel_name(p), pixel_name(p) + strlen(pixel_name(p)));
    }
}
static std::wstring size_name(int w, int h)
{
    std::wstring s = fmt(L"%d×%d", w, h);
    if (w == 3840 && h == 2160) s += L" (4K)";
    else if (w == 2560 && h == 1440) s += L" (1440p)";
    else if (w == 1920 && h == 1080) s += L" (1080p)";
    else if (w == 1280 && h == 720) s += L" (720p)";
    return s;
}
static void page_input(Col &c)
{
    bool reopen = false;
    // ---- the card ----
    heading(c, tr(L"캡처보드", L"Capture card"));
    {
        std::vector<std::wstring> names;
        int now = -1;
        for (size_t i = 0; i < app.devices.size(); i++)
        {
            names.push_back(app.devices[i].name);
            if (app.devices[i].path == app.device) now = (int)i;
        }
        mark(MK_DEVICE, c.x, c.y, c.w, 60);
        int r = choice_row(c, L"\xE7F4", tr(L"장치", L"Device"), names, now);
        if (r >= 0 && r != now)
        {
            app.device = app.devices[(size_t)r].path;
            app.want = VideoFormat();
            app.want_rate = false;
            tuning_save();
            app_open();
            return;
        }
    }
    if (button(c.x, c.y, c.w, 36, tr(L"캡처보드 다시 찾기", L"Search for cards again"), false, true, L"\xE72C")) { app_refresh_devices(); app_open(); return; }
    c.y += 44;
    if (!app.problem.empty()) note(c, app.problem, 2, AMBER);

    // ---- what the card offers ----
    std::vector<VideoFormat> all;
    for (auto &x : app.formats)
        if (x.w >= 640 && x.w <= 3840 && x.h <= 2160 && x.pixel != Pixel::none) all.push_back(x);
    auto rate_of = [](const VideoFormat &x) { return std::min(x.fps, 60.0); };
    bool autom = app.want.w == 0;
    heading(c, tr(L"화면 형식", L"Video format"), autom ? tr(L"자동: 4K 60fps까지 가장 높은 해상도", L"Automatic: up to 4K 60") : std::wstring());

    // kind (decoding)
    std::vector<Pixel> kinds;
    for (auto &x : all) if (std::find(kinds.begin(), kinds.end(), x.pixel) == kinds.end()) kinds.push_back(x.pixel);
    std::sort(kinds.begin(), kinds.end(), [](Pixel a, Pixel b) { return kind_rank(a) < kind_rank(b); });
    {
        std::vector<std::wstring> names = {tr(L"자동", L"Automatic")};
        for (Pixel p : kinds) names.push_back(kind_name(p));
        int now = 0;
        for (size_t i = 0; i < kinds.size(); i++) if (!autom && app.want.pixel == kinds[i]) now = (int)i + 1;
        int r = choice_row(c, L"\xE790", tr(L"디코드 형식", L"Decode format"), names, now);
        if (r == 0 && now != 0) { app.want = VideoFormat(); app.want_rate = false; reopen = true; }
        else if (r > 0 && r != now)
        {
            // keep the size if this kind has it, otherwise its largest one that runs at 30 or more
            Pixel p = kinds[(size_t)r - 1];
            const VideoFormat *pick = nullptr;
            for (auto &x : all)
                if (x.pixel == p && x.w == (autom ? app.current.w : app.want.w) && x.h == (autom ? app.current.h : app.want.h)) pick = &x;
            if (!pick)
                for (auto &x : all)
                    if (x.pixel == p && (!pick || (rate_of(x) >= 29 && (rate_of(*pick) < 29 || x.w * x.h > pick->w * pick->h)))) pick = &x;
            if (pick) { app.want = *pick; app.want.fps = rate_of(*pick); app.want_rate = false; reopen = true; }
        }
    }
    // size
    {
        std::vector<std::pair<int, int>> sizes;
        for (auto &x : all)
            if ((autom || x.pixel == app.want.pixel) && std::find(sizes.begin(), sizes.end(), std::make_pair(x.w, x.h)) == sizes.end()) sizes.push_back({x.w, x.h});
        std::sort(sizes.begin(), sizes.end(), [](auto a, auto b) { return a.first * a.second > b.first * b.second; });
        std::vector<std::wstring> names = {tr(L"자동", L"Automatic")};
        int now = 0;
        for (size_t i = 0; i < sizes.size(); i++)
        {
            double top = 0;
            for (auto &x : all) if (x.w == sizes[i].first && x.h == sizes[i].second && (autom || x.pixel == app.want.pixel)) top = std::max(top, x.fps);
            names.push_back(size_name(sizes[i].first, sizes[i].second) + fmt(tr(L"  · 최대 %.0f", L"  · up to %.0f"), top));
            if (!autom && app.want.w == sizes[i].first && app.want.h == sizes[i].second) now = (int)i + 1;
        }
        int r = choice_row(c, L"\xE740", tr(L"해상도", L"Resolution"), names, now);
        if (r == 0 && now != 0) { app.want = VideoFormat(); app.want_rate = false; reopen = true; }
        else if (r > 0 && r != now)
        {
            auto s = sizes[(size_t)r - 1];
            const VideoFormat *pick = nullptr; // the kind chosen, or (automatic) the fastest kind at this size
            for (auto &x : all)
                if (x.w == s.first && x.h == s.second && (autom ? (!pick || rate_of(x) > rate_of(*pick) + 0.5 || (fabs(rate_of(x) - rate_of(*pick)) <= 0.5 && kind_rank(x.pixel) < kind_rank(pick->pixel))) : x.pixel == app.want.pixel)) pick = &x;
            if (pick) { app.want = *pick; app.want.fps = rate_of(*pick); app.want_rate = false; reopen = true; }
        }
    }
    // frames per second
    {
        double top = 60;
        const VideoFormat &f = autom ? app.current : app.want;
        for (auto &x : all) if (x.w == f.w && x.h == f.h && x.pixel == f.pixel) top = x.fps;
        std::vector<std::wstring> names = {tr(L"자동", L"Automatic")};
        std::vector<double> rates = {0};
        for (double r : {60.0, 59.94, 50.0, 30.0, 29.97, 25.0, 24.0})
            if (r <= top + 0.5) { names.push_back(r == (int)r ? fmt(L"%.0f", r) : fmt(L"%.2f", r)); rates.push_back(r); }
        int now = 0;
        for (size_t k = 1; k < rates.size(); k++) if (app.want_rate && fabs(app.want.fps - rates[k]) < 0.01) now = (int)k;
        int r = choice_row(c, L"\xE916", tr(L"프레임 레이트 (FPS)", L"Frame rate (FPS)"), names, now);
        if (r >= 0 && r != now && f.w)
        {
            app.want = f;
            app.want_rate = r > 0;
            app.want.fps = r > 0 ? rates[(size_t)r] : std::min(top, 60.0);
            reopen = true;
        }
    }

    // ---- what arrives ----
    heading(c, tr(L"입력 신호", L"Input signal"));
    static ULONGLONG seen_at;
    static long long seen_n;
    static double rate;
    ULONGLONG t = GetTickCount64();
    if (t - seen_at > 4000) { seen_at = t; seen_n = num.captured; rate = 0; }
    else if (t - seen_at > 2000) { rate = (double)(num.captured - seen_n) * 1000.0 / (double)(t - seen_at); seen_at = t; seen_n = num.captured; }
    if (!app.current.w || (rate == 0 && num.captured == 0)) note(c, tr(L"신호가 없어요. 콘솔 전원, HDMI 케이블, 캡처보드 입력을 확인해 주세요", L"No signal. Check the console, the cable and the card's input"), 2, AMBER);
    else
    {
        note(c, fmt(tr(L"입력 중 %d×%d · %.2f fps · %hs · 소리 %ls", L"Receiving %d×%d · %.2f fps · %hs · sound %ls"), app.current.w, app.current.h, app.current.fps, pixel_name(app.current.pixel),
                    app.audio.rate ? fmt(L"%d Hz", app.audio.rate).c_str() : tr(L"없음", L"none")));
        if (rate > 1) note(c, fmt(tr(L"실제 입력: %.1f fps", L"Pictures the card really sends: %.1f per second"), rate));
    }
    if (app.current.pixel == Pixel::mjpg) note(c, fmt(tr(L"MJPEG를 푸는 데 %.1f ms (%ls). 선행 생성은 쓸 수 없어요", L"MJPEG decoding: %.1f ms (%ls). Start early is not available"), Mjpeg::last_ms(), Mjpeg::with()), 2, Mjpeg::last_ms() > 0.8 * 1000 / std::max(10.0, app.current.fps) ? AMBER : SOFT);
    if (rate > 10 && app.current.fps > 10 && fabs(rate / app.current.fps - 1) > 0.04)
    {
        // a USB 2.0 card (or port): it announces 60 but sends what the bus allows; generation still doubles it
        bool usb2 = app.current.pixel == Pixel::mjpg && rate < app.current.fps * 0.6;
        note(c, usb2 ? fmt(tr(L"%.0f fps로 열었지만 실제로는 %.1f fps만 들어와요. USB 2.0 캡처보드이거나 USB 2.0 포트에 꽂혀 있어요. 파란색 USB 3.0 포트에 꽂아 보세요. 지금 상태로도 프레임 생성으로 2배가 돼요",
                                     L"Opened at %.0f fps, but the card sends %.1f per second: a USB 2.0 card or port. Try a blue USB 3.0 port. Generation still doubles it"), app.current.fps, rate)
                     : fmt(tr(L"%.0f fps로 열렸지만 실제 입력은 %.1f fps예요. 입력 신호에 맞는 FPS(보통 60)를 고르면 지연 없이 받아요", L"Opened at %.0f fps, but the card sends %.1f per second. Choose the rate of the signal (usually 60)"), app.current.fps, rate),
             3, AMBER);
    }
    note(c, tr(L"다른 프로그램이 캡처보드를 다른 해상도로 먼저 열어 두면 8~33 ms 늦게 들어와요", L"If another program opened the card at another size first, frames arrive 8-33 ms later"), 2);
    if (reopen) { tuning_save(); app_open(); }
}
static void flag_row(Col &c, const wchar_t *icon, const std::wstring &t, const std::wstring &d, bool &v, void (*then)() = nullptr);
static void page_screen(Col &c)
{
    heading(c, tr(L"화면", L"Display"));
    int s;
    s = segment_row(c, L"\xE740", tr(L"보기", L"View"), L"", {tr(L"전체 화면", L"Full screen"), tr(L"창", L"Window")}, app.fullscreen ? 0 : 1);
    if (s >= 0) { app_fullscreen(s == 0); return; }
    s = segment_row(c, L"\xE9A6", tr(L"화면 채우기", L"Scaling"), L"", {tr(L"비율 유지", L"Keep aspect"), tr(L"꽉 채우기", L"Stretch")}, tune.fit);
    if (s >= 0) set(tune.fit, s);
    heading(c, tr(L"동기화", L"Sync"));
    render_card(c, false);
    note(c, tr(L"수직 동기화: 화면 찢어짐 없음, 지연 약 3 ms 증가. 가변·끔: 지연이 짧아요. 최대 프레임 대기는 수직 동기화에서만 적용돼요", L"V-Sync: no tearing, about 3 ms slower. VRR and Off: low latency. The frame queue is used with V-Sync only"), 2);
    if (num.hdr > 0)
    {
        heading(c, L"HDR");
        if (hdr_row(c)) return;
        slider_row(c, L"\xE706", 18, tr(L"기본 밝기", L"Base brightness"), fmt(L"%ld nit", tune.hdr_white), tr(L"기본 흰색 밝기 (SDR 화이트)", L"Brightness of ordinary white"), tune.hdr_white, 80, 400, 200);
        slider_row(c, L"\xE793", 19, tr(L"하이라이트", L"Highlights"), fmt(L"%ld%%", tune.hdr_boost), tr(L"햇빛·조명 같은 밝은 부분의 밝기", L"How bright sunlight and lamps get"), tune.hdr_boost, 0, 100, 50);
        note(c, tr(L"SDR 화면을 HDR 모니터에 맞게 밝게 보여 줘요", L"Expands a normal (SDR) picture to HDR. A console's HDR picture keeps its own colours"), 2);
    }
    // the console's HDR: the card is opened as P010 (10 bits) and the picture is kept as HDR10 (BT.2020, PQ)
    static bool console_hdr;
    console_hdr = app.want.pixel == Pixel::p010;
    flag_row(c, L"\xE7F4", tr(L"콘솔 HDR 입력", L"Console HDR input"),
             tr(L"콘솔에서 HDR을 켰다면 켜세요. 캡처보드가 SDR로 변환한 영상을 10비트로 받아 색 띠(밴딩)가 줄어요", L"For a console with HDR on: the card's SDR picture in 10 bits, smoother gradients"),
             console_hdr, [] { app.want.pixel = console_hdr ? Pixel::p010 : Pixel::none; app_open(); tuning_save(); });
    heading(c, tr(L"화면 오버레이 (OSD)", L"On-screen overlay"));
    s = segment_row(c, L"\xE9D9", tr(L"표시", L"Show"), L"", {tr(L"끔", L"Off"), L"fps", tr(L"지연", L"Latency"), tr(L"전문가", L"Expert")}, tune.osd);
    if (s >= 0) set(tune.osd, s);
    s = segment_row(c, L"\xE744", tr(L"위치", L"Position"), L"", {L"↖", L"↗", L"↙", L"↘"}, tune.corner);
    if (s >= 0) set(tune.corner, s);
    heading(c, tr(L"모니터", L"Monitor"));
    flag_row(c, L"\xE706", tr(L"모니터 절전 방지", L"Keep the monitor on"), tr(L"영상이 들어오는 동안만", L"While there is a game signal"), app.awake);
}
static const wchar_t *card_name(int i)
{
    static const wchar_t *ko[CARDS] = {L"FPS", L"지연", L"프레임 간격", L"프레임 생성", L"화질", L"입력 소스", L"그래픽카드", L"CPU", L"메모리", L"화면 출력", L"사양"}, *en[CARDS] = {L"FPS", L"Latency", L"Frame pacing", L"Frame generation", L"Quality", L"Source", L"Graphics card", L"CPU", L"Memory", L"Display", L"Specs"};
    return tr(ko[i], en[i]);
}
static void draw_card(int id, float x, float y, float w);
// The cards take as much room as they need: the picture and the window are laid out again.
static void show_cards(int which)
{
    set(app.cards, which);
    scroll[1] = 0;
    app_screen_changed();
    repaint_all();
}
static void page_data(Col &c, bool popup)
{
    heading(c, tr(L"보이는 데이터", L"Visible data"), tr(L"눌러서 숨기기·보이기", L"Click to show or hide"));
    float w = (c.w - 6) / 2;
    for (int i = 0; i < CARDS; i++)
        if (button(c.x + (float)(i % 2) * (w + 6), c.y + (float)(i / 2) * 38, w, 32, card_name(i), app.cards >> i & 1)) show_cards(app.cards ^ 1 << i);
    c.y += (float)((CARDS + 1) / 2) * 38 + 4;
    if (button(c.x, c.y, w, 32, tr(L"모두 보기", L"Show all"))) show_cards((1 << CARDS) - 1);
    if (button(c.x + w + 6, c.y, w, 32, tr(L"모두 숨기기", L"Hide all"))) show_cards(0);
    c.y += 42;
    if (popup)
        for (int i = 0; i < CARDS; i++)
            if (app.cards >> i & 1) { draw_card(i, c.x, c.y, c.w); c.y += CARD_H + GAP; }
    heading(c, tr(L"기록", L"Log"));
    float y = card_row(c, L"\xE7C8", tr(L"데이터 기록", L"Record data"), tr(L"1초에 한 줄씩 CSV로 (엑셀)", L"One line per second (CSV)"));
    if (button(c.x + c.w - 104, y + 13, 90, 34, app.recording ? tr(L"멈추기", L"Stop") : tr(L"시작", L"Start"), app.recording)) app_record(!app.recording);
    if (button(c.x, c.y, c.w, 36, tr(L"기록 폴더", L"Open folder"), false, true, L"\xE838")) ShellExecuteW(nullptr, L"open", data_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    c.y += 46;
    note(c, tr(L"PC 정보는 화면에 보일 때만 1초에 한 번, 가장 낮은 우선순위로 읽어요. 영상 처리와 분리되어 있어 지연과 화질에 영향이 없어요", L"System data is read once per second at the lowest priority, and only while it is shown"), 3);
}
static void toggle_data()
{
    app.show_data = !app.show_data;
    if (app.show_data && !app.cards) set(app.cards, (1 << CARDS) - 1);
    tuning_save();
    app_screen_changed();
}
static void flag_row(Col &c, const wchar_t *icon, const std::wstring &t, const std::wstring &d, bool &v, void (*then)())
{
    float y = card_row(c, icon, t, d);
    if (!toggle(c.x + c.w - 58, y + 18 - card_more / 2, v)) return;
    v = !v;
    tuning_save();
    gui.mode = MOVE; // used up
    if (then) then();
    repaint_all();
}
static void names_again() // after a change of the language
{
    SetWindowTextW(app.window, app_name());
    if (ui_window) SetWindowTextW(ui_window, tr(L"프쏙 설정", L"PEUSSOK settings"));
    if (!app.problem.empty()) app_open(); // says it again
    repaint_all();
}
static void hotkey_row(Col &c)
{
    float y = card_row(c, L"\xE765", tr(L"프레임 생성 단축키", L"On/off shortcut"), app.hot_ok || app.hot_capture ? tr(L"다른 창을 보고 있어도 작동해요", L"Works in other windows too") : tr(L"다른 프로그램이 쓰고 있어요", L"In use by another program"), 102);
    if (button(c.x + 14, y + 58, c.w - 28, 34, app.hot_capture ? tr(L"누르세요…", L"Press keys…") : app_hotkey_name(), app.hot_capture)) { app.hot_capture = true; app_hotkey(); }
}
static void page_settings(Col &c)
{
    heading(c, tr(L"언어와 모양", L"Language and appearance"));
    int s = segment_row(c, L"\xF2B7", tr(L"언어", L"Language"), tr(L"자동: Windows에서 쓰는 언어", L"Automatic: Windows language"), {tr(L"자동", L"Automatic"), L"한국어", L"English"}, (int)app_language);
    if (s >= 0) { set(app_language, s); gui.mode = MOVE; names_again(); return; }
    flag_row(c, L"\xE7C4", tr(L"창 반투명 효과", L"Transparent window"), tr(L"바탕화면이 흐리게 비쳐 보여요", L"Shows the blurred desktop behind"), app.see_through);
    flag_row(c, L"\xE840", tr(L"창을 항상 위에", L"Always on top"), tr(L"다른 창을 써도 가려지지 않아요", L"Stays above other windows"), app.topmost, app_topmost);
    heading(c, tr(L"시작 설정", L"On startup"));
    float y = card_row(c, L"\xE7E8", tr(L"Windows와 함께 켜기", L"Start with Windows"), tr(L"로그인하면 자동으로 켜져요", L"After you sign in"));
    static int autostart = -1;
    if (autostart < 0) autostart = app_autostart();
    if (toggle(c.x + c.w - 58, y + 18 - card_more / 2, autostart > 0)) { app_autostart(autostart <= 0); autostart = app_autostart(); gui.mode = MOVE; repaint_all(); return; }
    s = segment_row(c, L"\xE945", tr(L"프레임 생성", L"Frame generation"), L"", {tr(L"마지막 상태로", L"Last used"), tr(L"켬", L"On"), tr(L"끔", L"Off")}, app.start_on);
    if (s >= 0) { app.start_on = s; tuning_save(); }
    s = segment_row(c, L"\xE740", tr(L"화면", L"Display"), L"", {tr(L"마지막 상태로", L"Last used"), tr(L"전체 화면", L"Full screen"), tr(L"창", L"Window")}, app.start_full);
    if (s >= 0) { app.start_full = s; tuning_save(); }
    heading(c, tr(L"단축키", L"Shortcuts"));
    hotkey_row(c);
    note(c, tr(L"녹화: Ctrl+Alt+R · F11 전체 화면 · F1 설정 · M 음소거 · ↑↓ 소리", L"Record: Ctrl+Alt+R · F11 full screen · F1 panel · M mute · Up/Down volume"), 2);
    heading(c, tr(L"파일과 초기화", L"Files and reset"));
    float w = (c.w - 6) / 2;
    if (button(c.x, c.y, w, 36, tr(L"설정·기록 폴더", L"Open data folder"), false, true, L"\xE838")) { gui.mode = MOVE; ShellExecuteW(nullptr, L"open", data_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    if (button(c.x + w + 6, c.y, w, 36, tr(L"전체 초기화", L"Reset settings"), false, !bench_running && vrr_allow_now(*std::make_unique<std::wstring>()) != ALLOW_BUSY, L"\xE72C"))
    {
        gui.mode = MOVE;
        if (MessageBoxW(ui_open() ? ui_window : app.window, tr(L"모든 설정을 처음 상태로 되돌릴까요?\n\n테마 파일과 기록, 벤치마크 결과는 지우지 않아요.", L"Reset all settings?\n\nTheme files, logs and benchmark results are kept."), app_name(), MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
        {
            if (ini_int(L"vrr_profile", -1) >= 0) // what this program changed in the driver
            {
                std::wstring model = vrr_model(app.window);
                vrr_driver_off(false);
                ini_set(L"vrr_profile", -1);
                if (!model.empty() && ini_text(L"vrr_monitor_model", L"") == model) vrr_allow(app.window, false);
            }
            tuning_reset();
            autostart = -1;
            names_again();
            app_screen_changed();
        }
        return;
    }
    c.y += 46;
    note(c, fmt(L"%ls %ls", app_name(), APP_VERSION));
}
// ---- Sound tab ----
static void page_sound(Col &c)
{
    heading(c, tr(L"소리", L"Sound"));
    volume_row(c);
    flag_row(c, L"\xE767", tr(L"캡처보드 소리 켜기", L"Capture card sound"), tr(L"콘솔에 헤드셋을 연결했다면 끄세요", L"Turn off with a console headset"), app.sound, app_open);
    slider_row(c, L"\xE916", 30, tr(L"오디오 싱크 (지연)", L"Sound delay"), fmt(L"%ld ms", audio_delay), tr(L"소리가 화면보다 먼저 들리면 늘리세요", L"Raise it if the sound comes before the picture"), audio_delay, 0, 300, 0);
    double lat = app.signal ? num.latency.get() : 0;
    if (lat > 1 && button(c.x, c.y, c.w, 36, fmt(tr(L"화면 지연에 맞추기 (%.0f ms)", L"Match the picture's delay (%.0f ms)"), lat), false, true, L"\xE916"))
        set(audio_delay, (int)std::lround(std::min(300.0, lat)));
    if (lat > 1) c.y += 44;
    heading(c, tr(L"오디오 입력", L"Sound coming in"));
    if (!app.sound) note(c, tr(L"캡처보드 소리가 꺼져 있어요", L"The card's sound is off"), 1, AMBER);
    else if (!app.audio.rate) note(c, tr(L"이 캡처보드에서 소리를 찾지 못했어요. 캡처보드의 오디오 입력을 확인해 주세요", L"No sound found on this card. Check the card's audio input"), 2, AMBER);
    else
    {
        note(c, fmt(tr(L"%d Hz · %d채널 · %d비트", L"%d Hz · %d channels · %d bit"), app.audio.rate, app.audio.channels, app.audio.bits));
        note(c, fmt(tr(L"오디오 버퍼 %.0f ms · 끊김 %lld번", L"Sound waiting %.0f ms · gaps %lld"), audio_buffer_ms(), (long long)audio_gaps));
    }
    note(c, tr(L"M 음소거 · ↑↓ 소리 크기", L"M mute · Up/Down volume"));
}
// ---- Record tab ----
static std::wstring pick_folder(const std::wstring &start) // the system's folder picker, on a thread of its own (COM apartment)
{
    std::wstring out;
    std::thread([&] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return;
        IFileOpenDialog *d = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d))))
        {
            DWORD o = 0;
            d->GetOptions(&o);
            d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
            IShellItem *at = nullptr;
            if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&at)))) { d->SetFolder(at); at->Release(); }
            if (SUCCEEDED(d->Show(ui_open() ? ui_window : app.window)))
            {
                IShellItem *r = nullptr;
                PWSTR p = nullptr;
                if (SUCCEEDED(d->GetResult(&r)) && SUCCEEDED(r->GetDisplayName(SIGDN_FILESYSPATH, &p))) { out = p; CoTaskMemFree(p); }
                if (r) r->Release();
            }
            d->Release();
        }
        CoUninitialize();
    }).join();
    return out;
}
static void page_record(Col &c)
{
    heading(c, tr(L"녹화", L"Recording"));
    record_row(c);
    bool on = rec_on();
    if (on) note(c, tr(L"아래 설정은 다음 녹화부터 적용돼요", L"Changes below apply to the next recording"), 1, AMBER);
    int k = segment_row(c, L"\xE7F4", tr(L"녹화 대상", L"What to record"), tr(L"보이는 그대로를 고르면 생성 프레임까지 녹화돼요", L"As shown includes the made frames"), {tr(L"보이는 그대로", L"As shown"), tr(L"원본만", L"Originals only")}, rec_made ? 0 : 1);
    if (k >= 0) set(rec_made, k == 0);
    k = segment_row(c, L"\xE790", tr(L"코덱", L"Codec"), tr(L"HEVC가 같은 크기에 더 깨끗해요. 안 열리는 곳이 있으면 H.264", L"HEVC is cleaner for its size; H.264 plays everywhere"), {tr(L"자동", L"Automatic"), L"HEVC", L"H.264"}, (int)rec_codec_pref);
    if (k >= 0) set(rec_codec_pref, k);
    int mb4k[3] = {48, 80, 128};
    k = segment_row(c, L"\xE7B3", tr(L"화질", L"Quality"), fmt(tr(L"4K 60fps HEVC 기준 약 %d Mbps", L"About %d Mbps for 4K 60 HEVC"), mb4k[std::min(2L, std::max(0L, (LONG)rec_quality))]), {tr(L"작게", L"Smaller"), tr(L"보통", L"Normal"), tr(L"높게", L"High")}, (int)rec_quality);
    if (k >= 0) set(rec_quality, k);
    switch_row(c, L"\xE767", tr(L"소리 넣기", L"Include sound"), tr(L"캡처보드 소리를 AAC로 함께 저장", L"The card's sound as AAC"), rec_sound);
    // where the files go
    std::wstring dir = rec_folder();
    float y = card_row(c, L"\xE838", tr(L"폴더", L"Folder"), L"", 92);
    text(F_SMALL, SOFT, c.x + 50, y + 58, c.w - 64, 20, A_LEFT, dir);
    if (button(c.x + c.w - 286, y + 13, 86, 34, tr(L"바꾸기", L"Change")))
    {
        gui.mode = MOVE;
        std::wstring f = pick_folder(dir);
        if (!f.empty()) { rec_set_folder(f); ini_set(L"rec_folder", f); }
        repaint_all();
        return;
    }
    if (button(c.x + c.w - 194, y + 13, 86, 34, tr(L"기본", L"Default"))) { rec_set_folder(L""); ini_set(L"rec_folder", L""); }
    if (button(c.x + c.w - 102, y + 13, 88, 34, tr(L"열기", L"Open"))) ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    std::wstring last = rec_file();
    if (!last.empty()) note(c, fmt(tr(L"마지막 파일: %ls", L"Last file: %ls"), last.c_str()), 2);
    note(c, tr(L"위쪽 녹화 버튼(전체 화면 버튼 옆)이나 Ctrl+Alt+R로도 시작·정지할 수 있어요. 그래픽카드 인코더를 써서 지연에 영향이 없어요", L"The record button at the top (beside full screen) or Ctrl+Alt+R start and stop it too. The graphics card's encoder is used, so latency does not change"), 2);
}
static float panel_width();
// ---- user guide: the window goes dark, one place at a time is lit and explained (Next / Back / Skip) ----
enum { M_NONE = -1, M_POWER = MK_POWER, M_PRESETS = MK_PRESETS, M_MODEL = MK_MODEL, M_MULT = MK_MULT, M_DEVICE = MK_DEVICE, M_FULL = MK_FULL, M_RECORD = MK_RECORD, M_RAIL_SCREEN = MK_RAIL_SCREEN, M_RAIL_SOUND = MK_RAIL_SOUND, M_RAIL_BENCH = MK_RAIL_BENCH, M_RAIL_GUIDE = MK_RAIL_GUIDE, MARKS = MK_COUNT };
static float marks[MARKS][4];
static bool marked[MARKS];
static void mark(int i, float x, float y, float w, float h) { marks[i][0] = x; marks[i][1] = y; marks[i][2] = w; marks[i][3] = h; marked[i] = true; }
static int guide = -1;   // the step shown, -1: none
struct GuideStep { int page, mark; const wchar_t *t_ko, *t_en, *b_ko, *b_en; };
static const GuideStep guide_steps[] = {
    {P_HOME, M_NONE, L"프쏙에 오신 걸 환영해요", L"Welcome to PEUSSOK",
     L"캡처보드로 받은 게임 화면의 프레임을 늘려서 더 부드럽게 보여 줘요. 30fps 게임은 60fps 이상으로 보여요.\n\n중요한 곳만 짧게 둘러볼게요.",
     L"PEUSSOK shows your capture card's game picture with more frames: a 30 fps game looks like 60 fps or more.\n\nA short tour of the important places."},
    {P_INPUT, M_DEVICE, L"1. 캡처보드 고르기", L"1. Choose the capture card",
     L"콘솔이 연결된 캡처보드를 고르세요. 형식·해상도·초당 장수는 '자동'이 가장 좋아요.", L"Choose the card your console is connected to. Leave format, resolution and frame rate on Automatic."},
    {P_HOME, M_POWER, L"2. 켜고 끄기", L"2. On and off",
     L"가운데 원을 누르면 프레임 생성이 켜지고 꺼져요. 원 안에 보이는 fps와 원본 fps가 나와요.", L"Click the circle to switch frame generation on or off. It shows the frames you see and the source frames."},
    {P_HOME, M_PRESETS, L"3. 반응과 화질 고르기", L"3. Response or quality",
     L"처음에는 '균형'을 쓰세요. 반응이 중요한 게임은 '초저지연', 부드러움이 중요하면 '화질 우선'이에요.", L"Start with Balanced. Instant for fast games, Quality for the smoothest motion."},
    {P_HOME, M_MODEL, L"4. 모델", L"4. Model",
     L"프레임을 만드는 방법이에요. '추천'이 붙은 모델을 쓰세요. NVIDIA VFG는 처음 고를 때 NVIDIA 파일을 받아요.", L"How the frames are made. Use the one marked Best here. NVIDIA VFG downloads NVIDIA's files the first time."},
    {P_HOME, M_MULT, L"5. 배수", L"5. Multiplier",
     L"원본 한 장을 몇 장으로 늘릴지예요. 30fps 게임은 2배면 60, 4배면 120fps가 돼요. 모니터 주사율을 넘지 않게 고르세요.", L"How many frames per source frame: 30 fps becomes 60 with 2x, 120 with 4x. Stay within your monitor's refresh rate."},
    {P_HOME, M_FULL, L"6. 전체 화면", L"6. Full screen",
     L"게임은 전체 화면에서 하세요. 지연이 가장 짧아요. F11로도 바꿀 수 있고, Esc로 나와요.", L"Play in full screen: it has the lowest latency. F11 switches, Esc leaves."},
    {P_HOME, M_RECORD, L"7. 녹화", L"7. Recording",
     L"보이는 화면 그대로 소리와 함께 MP4로 저장해요. 다시 누르면 멈춰요. Ctrl+Alt+R로도 돼요.", L"Saves what you see, with sound, as MP4. Click again to stop. Ctrl+Alt+R works too."},
    {P_SCREEN, M_RAIL_SCREEN, L"8. 화면 설정", L"8. Display settings",
     L"G-SYNC·FreeSync, HDR, 게임 화면 위 fps 표시는 여기 있어요. 소리·녹화 설정은 그 아래 탭에 있어요.", L"G-SYNC/FreeSync, HDR and the on-screen fps are here. Sound and recording have their own tabs below."},
    {P_HOME, M_RAIL_BENCH, L"9. 내 컴퓨터에 맞추기", L"9. Fit it to this PC",
     L"벤치마크가 이 컴퓨터에서 잘 도는 모델과 배수를 찾아 줘요. 처음 한 번 해 보세요.", L"The benchmark finds the model and multiplier that run well on this PC. Try it once."},
    {P_HOME, M_RAIL_GUIDE, L"다 됐어요", L"All set",
     L"이 안내는 왼쪽 아래 '사용 가이드'에서 언제든 다시 볼 수 있어요. 자세한 내용은 설명서.txt에 있어요.", L"Open this tour again with Guide at the bottom left. More in the manual (설명서.txt / README.txt)."},
};
static const int GUIDE_STEPS = (int)(sizeof guide_steps / sizeof guide_steps[0]);
static void guide_go(int step)
{
    if (step < 0 || step >= GUIDE_STEPS)
    {
        guide = -1;
        ini_set(L"guide_seen", 1);
        repaint_all();
        return;
    }
    if (app.fullscreen) app_fullscreen(false);
    guide = step;
    app.bench_page = false;
    bool was = app.show_panel;
    app.show_panel = true;
    page = guide_steps[step].page;
    scroll[0] = 0;
    if (!was) app_screen_changed();
    repaint_all();
}
bool ui_guide_escape() // Esc: the guide closes first
{
    if (guide < 0) return false;
    guide_go(-1);
    return true;
}
static void guide_draw(float W, float H)
{
    if (guide < 0) return;
    const GuideStep &g = guide_steps[guide];
    bool hole = g.mark >= 0 && marked[g.mark];
    float hx = 0, hy = 0, hw = 0, hh = 0;
    if (hole) { hx = marks[g.mark][0] - 6; hy = marks[g.mark][1] - 6; hw = marks[g.mark][2] + 12; hh = marks[g.mark][3] + 12; }
    Ink dark = ink(0, 0, 0, 175);
    if (hole)
    {
        fill(0, 0, W, std::max(0.f, hy), dark);
        fill(0, hy + hh, W, std::max(0.f, H - hy - hh), dark);
        fill(0, hy, std::max(0.f, hx), hh, dark);
        fill(hx + hw, hy, std::max(0.f, W - hx - hw), hh, dark);
        box(hx, hy, hw, hh, 12, ink(0, 0, 0, 0), ACCENT, 2.5f);
    }
    else fill(0, 0, W, H, dark);
    // the card stays over the settings column (the game picture is a window of its own, nothing is drawn over it):
    // under the lit place where there is room, else over it
    float pw = panel_width(), ax = pw > 0 ? RAIL : 0, aw = pw > 0 ? pw : W;
    float cw = std::min(380.f, aw - 24), chh = 226, cx = ax + (aw - cw) / 2, cy;
    if (!hole) cy = (H - chh) / 2;
    else if (hy + hh + 14 + chh < H - 8) cy = hy + hh + 14;
    else cy = hy - 14 - chh;
    cy = std::min(std::max(TOP + 4.f, cy), H - chh - 12);
    box(cx, cy, cw, chh, 14, theme_shade(1.2f), ACCENT, 1.5f);
    text(F_SMALL, SOFT, cx + 20, cy + 14, cw - 40, 20, A_LEFT, fmt(L"%d / %d", guide + 1, GUIDE_STEPS));
    text(F_BOLD, TEXT, cx + 20, cy + 36, cw - 40, 26, A_LEFT, tr(g.t_ko, g.t_en));
    text(F_BODY, TEXT, cx + 20, cy + 68, cw - 40, 104, A_WRAP, tr(g.b_ko, g.b_en));
    float by = cy + chh - 50;
    bool last = guide == GUIDE_STEPS - 1;
    if (!last && button(cx + 16, by, 96, 36, tr(L"건너뛰기", L"Skip"))) { gui.mode = MOVE; guide_go(-1); return; }
    if (guide > 0 && button(cx + cw - 16 - 120 - 8 - 90, by, 90, 36, tr(L"이전", L"Back"))) { gui.mode = MOVE; guide_go(guide - 1); return; }
    if (button(cx + cw - 16 - 120, by, 120, 36, last ? tr(L"시작하기", L"Done") : tr(L"다음", L"Next"), true)) { gui.mode = MOVE; guide_go(last ? -1 : guide + 1); return; }
}
// Top of a settings tab: puts this tab's settings back as on the first start. The first click asks, the second one does it.
static void reset_bar(Col &c, int p)
{
    static int asked = -1;
    static ULONGLONG when;
    if (asked == p && GetTickCount64() - when > 4000) asked = -1;
    bool ask = asked == p;
    float bw = ask ? 230.f : 150.f, bx = c.x + c.w - bw, by = c.y;
    bool h = hot(bx, by, bw, 30);
    box(bx, by, bw, 30, 8, ask ? fade(AMBER, 40) : h ? FIELD_HI : FIELD, ask ? fade(AMBER, 160) : h ? ACCENT : EDGE);
    text(F_ICON, ask ? AMBER : SOFT, bx + 8, by, 22, 30, A_CENTER, L"\xE72C");
    text(F_SMALL, ask ? AMBER : TEXT, bx + 32, by, bw - 40, 30, A_LEFT, ask ? tr(L"한 번 더 누르면 되돌려요", L"Click again to reset") : tr(L"기본값으로", L"Reset tab"));
    c.y += 38;
    if (!pressed(bx, by, bw, 30)) return;
    gui.mode = MOVE;
    if (!ask) { asked = p; when = GetTickCount64(); repaint_all(); return; }
    asked = -1;
    switch (p)
    {
    case P_GENERATE: tuning_reset_keys({L"model", L"multiplier", L"share", L"margin", L"rife_flow", L"early", L"cut", L"hud", L"text", L"repair"}); break;
    case P_PICTURE: tuning_reset_keys({L"sharpen", L"sharpen_made", L"range", L"nr", L"nr_style", L"nr_power", L"nr_size", L"nr_layers", L"nr_detail", L"nr_tone", L"nr_keep", L"nr_skin", L"nr_steady", L"nr_fast", L"nr_order"}); break;
    case P_SCREEN: tuning_reset_keys({L"fit", L"sync", L"queue", L"hdr", L"hdr_in", L"hdr_white", L"hdr_boost", L"osd", L"corner"}); break;
    case P_INPUT: { bool hdr_input = app.want.pixel == Pixel::p010; app.want = VideoFormat(); if (hdr_input) app.want.pixel = Pixel::p010; tuning_save(); app_open(); break; } // the console's HDR stays as chosen
    case P_SOUND: app.sound = true; tuning_reset_keys({L"volume", L"mute", L"audio_delay"}); break;
    case P_RECORD: rec_set_folder(L""); ini_set(L"rec_folder", L""); tuning_reset_keys({L"rec_codec", L"rec_quality", L"rec_sound", L"rec_made"}); break;
    case P_DATA: tuning_reset_keys({L"cards"}); break;
    }
    app_screen_changed();
    repaint_all();
}
static void draw_page(int p, Col &c, bool popup)
{
    if (p != P_HOME && p != P_THEME && p != P_SETTINGS && p != P_BENCH) reset_bar(c, p);
    switch (p)
    {
    case P_GENERATE: page_generate(c); break;
    case P_PICTURE: page_picture(c); break;
    case P_INPUT: page_input(c); break;
    case P_SCREEN: page_screen(c); break;
    case P_SOUND: page_sound(c); break;
    case P_RECORD: page_record(c); break;
    case P_DATA: page_data(c, popup); break;
    case P_THEME: page_theme(c); break;
    case P_SETTINGS: page_settings(c); break;
    default: page_home(c); break;
    }
}
// The settings column: scrolls inside the given rectangle.
static void draw_column(float x, float y, float w, float h, bool popup)
{
    scroll[0] = std::min(scroll[0], scroll_max[0]);
    clip_to(x, y, w, h);
    Col c = {x + 14, y + 6 - scroll[0], w - 30};
    float y0 = c.y;
    draw_page(page, c, popup);
    float total = c.y - y0 + 18;
    scroll_max[0] = std::max(0.f, total - h);
    if (scroll_max[0] > 0)
    {
        float bar = std::max(30.f, h * h / std::max(1.f, total));
        box(x + w - 8, y + (h - bar) * scroll[0] / scroll_max[0], 4, bar, 2, ink(255, 255, 255, 60));
    }
    clip_end();
}

// ---- data cards ----
static void facts(float x, float y, float w, const std::vector<std::pair<std::wstring, std::wstring>> &f)
{
    float k = 0;
    for (auto &p : f)
    {
        float lw = gui.mode == PAINT ? std::min(w * 0.5f, d_width(F_SMALL, p.first) + 8) : 0;
        text(F_SMALL, SOFT, x, y + k, lw, 20, A_LEFT, p.first);
        text(F_SMALL, TEXT, x + lw, y + k, w - lw, 20, A_RIGHT, p.second);
        k += 20;
    }
}
static void spark(int kind, float x, float y, float w, float h, Ink col, float top, bool under = true)
{
    if (gui.mode != PAINT || history_n < 2 || !d_visible(x, y, w, h)) return;
    int n = std::min(history_n, (int)HISTORY);
    std::vector<std::pair<float, float>> p;
    for (int i = 0; i < n; i++)
    {
        float v = std::max(0.f, history[kind][(history_n - n + i) % HISTORY]);
        p.push_back({x + w * (float)(HISTORY - n + i) / (HISTORY - 1), y + h - h * std::min(1.f, v / std::max(1.f, top))});
    }
    d_lines(p, col, 1.6f, under ? fade(col, 40) : 0, y + h);
}
static float history_top(int kind, float low)
{
    float t = low;
    for (int i = 0; i < std::min(history_n, (int)HISTORY); i++) t = std::max(t, history[kind][i]);
    return t;
}
// How evenly pictures come: a made picture and its original are shown for different times on purpose, so they count together.
static void pace(double &avg, double &p99)
{
    std::vector<int> v;
    LONG next = num.ring_next;
    int group = tune.on && num.engine_state == 2 ? std::max(2, model_by_id(tune.model)->multi ? (int)tune.mult : 2) : 1;
    for (int i = 0; i + group <= std::min<int>(240, next); i += group)
    {
        int sum = 0;
        for (int k = 0; k < group; k++) sum += (int)(num.ring[(unsigned)(next - 1 - i - k) % 256] & (RING_MADE - 1));
        v.push_back(sum / group);
    }
    avg = p99 = 0;
    if (v.empty()) return;
    double s = 0;
    for (int x : v) s += x;
    std::sort(v.begin(), v.end());
    avg = s / (double)v.size() / 1000.0;
    p99 = v[v.size() * 99 / 100] / 1000.0;
}
static Ink step_color_of(int i) { const Ink c[5] = {SKY, ACCENT, VIOLET, ink(255, 255, 255, 90), AMBER}; return c[i]; }
static void draw_card(int id, float x, float y, float w)
{
    static const wchar_t *icons[CARDS] = {L"\xEC4A", L"\xE916", L"\xE9D2", L"\xE945", L"\xE7B3", L"\xE7F4", L"\xE7F1", L"\xE950", L"\xE964", L"\xE7F8", L"\xE946"};
    bool h = hot(x, y, w, CARD_H);
    box(x, y, w, CARD_H, 12, h ? CARD_HI : CARD, EDGE);
    text(F_ICON, ACCENT, x + 12, y + 6, 20, 24, A_LEFT, icons[id]);
    text(F_SMALL, SOFT, x + 36, y + 6, w - 70, 24, A_LEFT, card_name(id));
    if (h)
    {
        text(F_ICON, hot(x + w - 32, y + 4, 28, 26) ? TEXT : SOFT, x + w - 32, y + 4, 28, 26, A_CENTER, L"\xE711");
        if (pressed(x + w - 32, y + 4, 28, 26)) { show_cards(app.cards & ~(1 << id)); return; }
    }
    if (gui.mode != PAINT || !d_visible(x, y, w, CARD_H)) return;
    bool live = app.signal || bench_running, gen = live && tune.on && num.engine_state == 2;
    float ix = x + 14, iw = w - 28, hx = ix + iw / 2, hw = iw / 2;
    auto big = [&](const std::wstring &v, const wchar_t *u) {
        text(F_BIG, TEXT, ix, y + 28, iw, 42, A_LEFT, v);
        text(F_SMALL, SOFT, ix + d_width(F_BIG, v) + 5, y + 42, 60, 24, A_LEFT, u);
    };
    SysLive s = sys_live();
    const SysInfo &si = sys_info();
    switch (id)
    {
    case CARD_FPS:
    {
        double avg, p99;
        pace(avg, p99);
        big(live ? fmt(L"%.0f", app_shown_fps()) : L"—", L"fps");
        float top = history_top(H_SHOWN, 30) * 1.05f;
        spark(H_SHOWN, hx, y + 32, hw, 34, ACCENT, top);
        spark(H_SOURCE, hx, y + 32, hw, 34, SKY, top, false);
        facts(ix, y + 72, iw, {{tr(L"원본", L"Source"), live ? fmt(L"%.1f fps", app_source_fps()) : L"—"}, {tr(L"생성 프레임", L"Generated"), gen ? fmt(tr(L"%.1f fps", L"%.1f / s"), app_made_fps() * (double)(model_by_id(tune.model)->multi ? tune.mult - 1 : 1)) : L"—"}, {tr(L"하위 1%", L"1% low"), live && p99 > 0 ? fmt(L"%.0f fps", 1000.0 / p99) : L"—"}});
        break;
    }
    case CARD_LATENCY:
    {
        big(live ? fmt(L"%.1f", num.latency.get()) : L"—", L"ms");
        spark(H_LATENCY, hx, y + 32, hw, 34, AMBER, history_top(H_LATENCY, 5) * 1.1f);
        double v[5] = {num.ready.get(), num.generate.get(), num.hud.get(), tune.margin_x10 / 10.0, num.share.get()}, total = 0, at = 0;
        for (double z : v) total += z;
        box(ix, y + 76, iw, 9, 4.5f, ink(255, 255, 255, 26));
        if (gen && total > 0.1)
            for (int i = 0; i < 5; i++)
            {
                float a = ix + iw * (float)(at / total);
                at += v[i];
                fill(a, y + 76, std::max(0.f, ix + iw * (float)(at / total) - a - 1), 9, step_color_of(i));
            }
        text(F_SMALL, SOFT, ix, y + 90, iw, 20, A_LEFT, gen ? fmt(tr(L"변환 %.1f 생성 %.1f HUD %.1f 표시 %.1f", L"In %.1f · gen %.1f · HUD %.1f · show %.1f"), v[0], v[1], v[2], v[4]) : L"");
        text(F_SMALL, SOFT, ix, y + 110, iw, 20, A_LEFT, live ? fmt(tr(L"최대 %.1f ms (입력→출력)", L"Max %.1f ms (arrival→present)"), peak_ms) : L"");
        break;
    }
    case CARD_PACE:
    {
        double avg, p99;
        pace(avg, p99);
        LONG next = num.ring_next;
        int bars = std::min<int>({(int)(iw / 4), 200, (int)next}), peak = 8000;
        for (int i = 0; i < bars; i++) peak = std::max(peak, (int)(num.ring[(unsigned)(next - bars + i) % 256] & (RING_MADE - 1)));
        peak = std::min(peak, 50000);
        if (live)
            for (int i = 0; i < bars; i++)
            {
                LONG r = num.ring[(unsigned)(next - bars + i) % 256];
                float bh = std::min(70.f, std::max(1.f, (float)(r & (RING_MADE - 1)) * 70.f / (float)peak));
                fill(ix + (float)i * 4, y + 106 - bh, 3, bh, r & RING_MADE ? ACCENT : fade(SKY, 170));
            }
        text(F_SMALL, SOFT, ix, y + 110, iw, 20, A_LEFT, live ? fmt(tr(L"평균 %.2f · 최장 1%% %.2f ms", L"Avg %.2f · worst 1%% %.2f ms"), avg, p99) : L"");
        text(F_SMALL, ACCENT, x + w - 150, y + 6, 70, 24, A_RIGHT, h ? L"" : tr(L"생성", L"Gen"));
        text(F_SMALL, SKY, x + w - 76, y + 6, 62, 24, A_RIGHT, h ? L"" : tr(L"원본", L"Source"));
        break;
    }
    case CARD_GEN:
    {
        double made = num.sources > 0 ? 100.0 * (double)(num.sources - num.skipped) / (double)num.sources : 0;
        big(gen ? fmt(L"%.1f", made) : L"—", tr(L"% 정시", L"% on time"));
        std::wstring model = model_by_id(tune.model)->name;
        if (num.flow) model += fmt(L" 1/%d", num.flow == 1 ? 2 : num.flow == 2 ? 4 : 8);
        facts(ix, y + 72, iw, {{tr(L"모델", L"Model"), model + fmt(tr(L" · %ld배", L" · %ldx"), tune.mult)}, {tr(L"생성 시간", L"Per frame"), gen ? fmt(L"%.2f ms", num.generate.get()) : L"—"}, {tr(L"건너뜀·늦음·버림", L"Skip·late·drop"), fmt(L"%lld · %lld · %lld", (long long)num.skipped, (long long)num.late, (long long)num.dropped)}});
        break;
    }
    case CARD_QUALITY:
    {
        big(tune.on ? model_psnr(tune.model) : tr(L"원본", L"Source"), L"");
        dots(x + w - 72, y + 46, tune.on ? model_by_id(tune.model)->quality : 5, SKY);
        const wchar_t *ch = app.current.pixel == Pixel::nv12 ? L"4:2:0 8bit" : app.current.pixel == Pixel::p010 ? L"4:2:0 10bit" : app.current.pixel == Pixel::yuy2 || app.current.pixel == Pixel::uyvy ? L"4:2:2 8bit" : app.current.pixel == Pixel::i420 || app.current.pixel == Pixel::yv12 ? L"4:2:0 8bit" : app.current.pixel == Pixel::mjpg ? L"8bit" : L"RGB 8bit";
        facts(ix, y + 72, iw, {{tr(L"색", L"Color"), fmt(L"%hs %ls · %ls", pixel_name(app.current.pixel), ch, tune.range == 1 || (tune.range == 0 && app.current.pixel == Pixel::mjpg) ? tr(L"전체", L"full") : tr(L"제한", L"limited"))}, {tr(L"크기", L"Size"), fmt(L"%ldp → %ld×%ld", num.h, num.view_w, num.view_h)}, {tr(L"보정", L"Filters"), fmt(L"HUD %ls · %ls · %ls %lld", tune.hud ? tr(L"켬", L"On") : tr(L"끔", L"Off"), tune.sharpen ? fmt(tr(L"선명 %ld%%", L"sharp %ld%%"), tune.sharpen).c_str() : tr(L"선명 끔", L"sharp off"), tr(L"컷", L"cuts"), (long long)num.cuts)}});
        break;
    }
    case CARD_SOURCE:
    {
        std::wstring name;
        for (auto &d : app.devices)
            if (d.path == app.device) name = d.name;
        text(F_BOLD, TEXT, ix, y + 28, iw, 26, A_LEFT, name.empty() ? L"—" : name);
        facts(ix, y + 54, iw, {{tr(L"형식", L"Format"), app.current.w ? fmt(L"%dp %.2f %hs", app.current.h, app.current.fps, pixel_name(app.current.pixel)) : L"—"}, {tr(L"입력 간격", L"Interval"), live ? fmt(L"%.2f ms", num.interval.get()) : L"—"}, {tr(L"중복 프레임", L"Repeats"), live ? fmt(L"%.0f%%", app_repeat_percent()) : L"—"}, {tr(L"소리", L"Sound"), fmt(tr(L"%d kHz · %.0f ms · 끊김 %lld", L"%d kHz · %.0f ms · gaps %lld"), app.audio.rate / 1000, audio_buffer_ms(), (long long)audio_gaps)}});
        break;
    }
    case CARD_GPU:
        big(num1(s.gpu, L"%.0f"), L"%");
        spark(H_GPU, hx, y + 32, hw, 34, ACCENT, 100);
        facts(ix, y + 72, iw, {{tr(L"클럭·메모리", L"Clock·memory"), s.gpu_mhz < 0 ? L"—" : fmt(L"%.0f · %.0f MHz", (double)s.gpu_mhz, (double)s.mem_mhz)}, {tr(L"전력·온도", L"Power·temp"), s.gpu_w >= 0 ? fmt(L"%.0f/%.0f W · %.0f°C", (double)s.gpu_w, (double)s.gpu_limit_w, (double)s.gpu_temp) : s.gpu_temp >= 0 ? fmt(L"%.0f%% · %.0f°C", (double)s.gpu_power_pct, (double)s.gpu_temp) : L"—"}, {tr(L"비디오 메모리", L"VRAM"), s.vram_used >= 0 ? fmt(tr(L"%.1f/%.1f GB (앱 %.1f)", L"%.1f/%.1f GB (app %.1f)"), (double)s.vram_used, (double)s.vram_total, (double)s.vram_app) : fmt(tr(L"앱 %.1f/%.1f GB", L"App %.1f/%.1f GB"), (double)s.vram_app, (double)s.vram_total)}});
        break;
    case CARD_CPU:
        big(num1(s.cpu, L"%.0f"), L"%");
        spark(H_CPU, hx, y + 32, hw, 34, SKY, 100);
        facts(ix, y + 72, iw, {{tr(L"이 프로그램", L"This app"), num1(s.cpu_app, L"%.1f%%")}, {tr(L"클럭", L"Clock"), num1(s.cpu_mhz, L"%.0f MHz")}, {tr(L"전력", L"Power"), num1(s.cpu_w, L"%.0f W")}});
        break;
    case CARD_RAM:
        big(num1(s.ram_used, L"%.1f"), L"GB");
        box(hx, y + 46, hw, 8, 4, ink(255, 255, 255, 26));
        if (s.ram_total > 0) box(hx, y + 46, hw * s.ram_used / s.ram_total, 8, 4, VIOLET);
        facts(ix, y + 72, iw, {{tr(L"전체", L"Total"), num1(s.ram_total, L"%.0f GB")}, {tr(L"이 프로그램", L"This app"), num1(s.ram_app, L"%.2f GB")}, {L"PCIe", s.pcie_gen > 0 ? fmt(L"%.0f.0 x%.0f", (double)s.pcie_gen, (double)s.pcie_width) : L"—"}});
        break;
    case CARD_SCREEN:
        big(num.hz_mhz ? fmt(L"%ld", num.hz_mhz / 1000) : L"—", L"Hz");
        facts(ix, y + 72, iw, {{tr(L"모니터", L"Monitor"), app_monitor_name()}, {tr(L"동기화", L"Sync"), tune.sync == 0 ? tr(L"수직 동기화", L"V-Sync") : tune.sync == 1 ? (num.vrr == 0 ? tr(L"가변 (모니터 꺼짐)", L"VRR (monitor off)") : num.vrr == 2 ? tr(L"가변 (작동 중)", L"VRR (active)") : tr(L"가변 (VRR)", L"VRR")) : tr(L"끔", L"Off")}, {tr(L"출력 방식", L"Output"), num.composed == 1 ? tr(L"윈도우가 합성 (+지연)", L"Composed (+latency)") : num.composed == 0 ? tr(L"바로 표시", L"Direct") : L"—"}});
        break;
    case CARD_SPEC:
        facts(ix, y + 30, iw, {{L"GPU", si.gpu}, {tr(L"드라이버", L"Driver"), si.driver}, {L"CPU", si.cpu}, {L"RAM", si.ram}, {L"OS", si.os}});
        break;
    }
}
static bool data_visible() { return !app.fullscreen && app.show_data && !app.bench_page && app.cards; }
static void draw_data(float x, float y, float w, float h)
{
    int cols = std::max(1, (int)((w + GAP) / (232 + GAP))), n = 0, i = 0;
    for (int k = 0; k < CARDS; k++) n += app.cards >> k & 1;
    if (n < cols) cols = std::max(1, n);
    float cw = (w - GAP * (float)(cols - 1)) / (float)cols;
    int rows = (n + cols - 1) / cols;
    scroll_max[1] = std::max(0.f, (float)rows * (CARD_H + GAP) + 6 - h);
    scroll[1] = std::min(scroll[1], scroll_max[1]);
    clip_to(x, y, w, h);
    for (int k = 0; k < CARDS; k++)
    {
        if (!(app.cards >> k & 1)) continue;
        draw_card(k, x + (float)(i % cols) * (cw + GAP), y + GAP - scroll[1] + (float)(i / cols) * (CARD_H + GAP), cw);
        i++;
    }
    clip_end();
}

// ---- benchmark page ----
static Ink mult_color(int m) { return m >= 3 ? ACCENT_HI : m == 2 ? SKY : CORAL; }
static int cell_mult(const BenchCell &c) { return bench_fps == 30 ? c.mult30 : bench_fps == 120 ? c.mult120 : c.mult60; }
static void bench_view(float W, float &x, float &y) // where the test picture is shown while it runs
{
    float w = W - RAIL - 32;
    x = RAIL + 12 + w - 252;
    y = TOP + 84;
}
static void draw_bench(float W, float H)
{
    const BenchResult &r = bench_result;
    const SysInfo &si = sys_info();
    bool run = bench_running != 0;
    float x = RAIL + 12, w = W - RAIL - 32, y = TOP + 4, bx = x + w;
    text(F_TITLE, TEXT, x + 4, y, 400, 34, A_LEFT, tr(L"컴퓨터 성능 확인", L"Benchmark"));
    text(F_SMALL, SOFT, x + 4, y + 36, w - 330, 22, A_LEFT, tr(L"60fps 테스트 영상으로 모델별 생성 시간을 측정해요. 측정하는 동안 캡처는 잠시 멈춰요.", L"Runs a 60 fps test image through the real pipeline. Capture pauses meanwhile."));
    if (run) { if (button(bx - 130, y + 10, 130, 42, tr(L"멈추기", L"Stop"), false, true, L"\xE71A")) bench_cancel(); }
    else
    {
        if (main_button(bx - 186, y + 10, 186, 42, tr(L"전체 (약 1분 30초)", L"Full (90 s)"), L"\xE768")) { app_bench(true); app_screen_changed(); return; }
        if (button(bx - 346, y + 10, 152, 42, tr(L"빠르게 (약 30초)", L"Quick (30 s)"))) { app_bench(false); app_screen_changed(); return; }
    }
    y += 70;
    float cw = (w - 12) / 2, ch = 156;
    box(x, y, cw, ch, 12, CARD, EDGE);
    text(F_BOLD, TEXT, x + 18, y + 8, 200, 28, A_LEFT, tr(L"이 컴퓨터", L"This computer"));
    facts(x + 18, y + 42, cw - 36, {{tr(L"그래픽카드", L"Graphics card"), si.gpu + fmt(L" · %.0f GB", si.vram_mb / 1024.0)}, {tr(L"드라이버", L"Driver"), si.driver}, {L"CPU", si.cpu + fmt(tr(L" · %d코어", L" · %d cores"), si.cores)}, {L"RAM", si.ram}, {tr(L"모니터", L"Monitor"), fmt(L"%ldp · %ld Hz", num.monitor_h, num.hz_mhz / 1000)}});
    float sx = x + cw + 12;
    box(sx, y, cw, ch, 12, CARD, EDGE);
    if (run)
    {
        int steps = std::max(1, (int)bench_steps), done = (int)bench_done;
        float pw = cw - 296;
        text(F_BOLD, ACCENT_HI, sx + 18, y + 10, pw, 26, A_LEFT, tr(L"측정 중", L"Measuring"));
        text(F_HEAD, TEXT, sx + 18, y + 38, pw, 28, A_LEFT, bench_model_now >= 0 ? fmt(L"%ls · %ls", models[bench_model_now].name, bench_size_name(bench_size_now)) : tr(L"테스트 영상 준비 중", L"Preparing the test image"));
        text(F_SMALL, SOFT, sx + 18, y + 66, pw, 22, A_LEFT, fmt(L"%d / %d", done, steps));
        box(sx + 18, y + 98, pw, 8, 4, ink(255, 255, 255, 30));
        box(sx + 18, y + 98, pw * (float)done / (float)steps, 8, 4, ACCENT);
        text(F_SMALL, SOFT, sx + 18, y + 118, pw, 22, A_LEFT, tr(L"다른 프로그램을 끄면 더 정확해요", L"Close other programs for accurate results"));
    }
    else if (r.valid)
    {
        text(F_SMALL, SOFT, sx + 18, y + 10, 120, 22, A_LEFT, tr(L"점수", L"Score"));
        text(F_HUGE, ACCENT_HI, sx + 18, y + 28, 190, 60, A_LEFT, fmt(L"%d", r.score));
        text(F_SMALL, SOFT, sx + 18, y + 92, 180, 20, A_LEFT, r.when + (r.full ? L"" : tr(L" · 빠르게", L" · quick")) + (r.game ? tr(L" · 게임 화면", L" · game") : L""));
        text(F_SMALL, SOFT, sx + 18, y + 112, 190, 40, A_WRAP, tr(L"FSR 3·RIFE 4.6의 초당\n생성 프레임 수로 계산해요", L"Based on frames per second\nof FSR 3 and RIFE 4.6"));
        float rx = sx + 214, rw = cw - 230;
        text(F_SMALL, SOFT, rx, y + 10, rw, 22, A_LEFT, tr(L"추천 설정", L"Recommended"));
        if (r.best_model >= 0)
        {
            const BenchCell &b = r.cell[r.best_model][r.best_size];
            text(F_HEAD, TEXT, rx, y + 32, rw, 28, A_LEFT, fmt(tr(L"%ls · %ls · 2배", L"%ls · %ls · 2x"), model_by_id(r.model_id[r.best_model])->name, bench_size_name(r.best_size)));
            text(F_SMALL, SOFT, rx, y + 60, rw, 22, A_LEFT, fmt(tr(L"예상 지연 %.1f ms · 60fps 게임은 %d배까지", L"About %.1f ms · up to %dx at 60 fps"), (double)b.latency, b.mult60));
            if (r.fast_model >= 0) text(F_SMALL, SOFT, rx, y + 80, rw, 22, A_LEFT, fmt(tr(L"지연 최소: %ls %.1f ms", L"Lowest latency: %ls %.1f ms"), model_by_id(r.model_id[r.fast_model])->name, (double)r.cell[r.fast_model][r.best_size].latency));
            if (main_button(rx, y + 110, 124, 34, tr(L"이 설정 쓰기", L"Use these settings"))) { use_model(*model_by_id(best_model())); use_preset(2); }
        }
        else text(F_BODY, AMBER, rx, y + 34, rw, 70, A_WRAP, tr(L"60fps를 2배로 만들 수 있는 모델이 없어요. 해상도를 낮추거나 30fps 게임에서 써 보세요.", L"No model can do 2x at 60 fps on this PC. Try a lower resolution or 30 fps games."));
        if (button(rx + 132, y + 110, 124, 34, tr(L"결과 파일", L"Open results"), false, true, L"\xE8A5")) ShellExecuteW(nullptr, L"open", (data_dir() + L"\\benchmark.txt").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    else text(F_BODY, SOFT, sx + 18, y + 20, cw - 36, 110, A_WRAP, tr(L"아직 측정 기록이 없어요. 위의 \"빠르게\"나 \"전체\"를 누르면 모델별 사용 가능 여부, 최대 배수, 알맞은 해상도를 알려 줘요.", L"Not measured yet. Press Quick or Full to see which models work, up to which multiplier, and at which resolution."));
    y += ch + 12;
    int rows = std::max(1, r.models);
    float head = 66, rh = std::min(62.f, std::max(40.f, (H - y - 18 - head - 32) / (float)rows));
    box(x, y, w, head + rh * (float)rows + 34, 12, CARD, EDGE);
    text(F_BOLD, TEXT, x + 18, y + 6, 160, 34, A_LEFT, tr(L"게임 프레임", L"Game frame rate"));
    int s = segments(x + 146, y + 8, 252, 32, {L"30 fps", L"60 fps", L"120 fps"}, bench_fps == 30 ? 0 : bench_fps == 60 ? 1 : 2);
    if (s >= 0) bench_fps = s == 0 ? 30 : s == 1 ? 60 : 120;
    text(F_SMALL, SOFT, x + 410, y + 8, w - 428, 32, A_RIGHT, tr(L"최대 배수 · 프레임당 생성 시간 · 60fps 2배 예상 지연", L"Max multiplier · time per frame · latency at 60 fps 2x"));
    float nx = x + 18, nw = w * 0.22f, lw = w * 0.17f, colw = (w - nw - 36 - lw) / 3;
    const wchar_t *hs[3] = {L"1080p", L"1440p", L"4K"};
    for (int i = 0; i < 3; i++) text(F_SMALL, SOFT, nx + nw + (float)i * colw, y + 42, colw, 22, A_LEFT, hs[i]);
    text(F_SMALL, SOFT, nx + nw + 3 * colw, y + 42, lw, 22, A_LEFT, tr(L"권장 해상도", L"Best resolution"));
    for (int m = 0; m < r.models; m++)
    {
        float ry = y + head + (float)m * rh;
        fill(x + 12, ry, w - 24, 1, EDGE);
        bool now = run && bench_model_now == m;
        if (now) fill(x + 1, ry + 1, 3, rh - 2, ACCENT);
        text(F_BOLD, now ? ACCENT_HI : TEXT, nx, ry + rh / 2 - 22, nw, 24, A_LEFT, model_by_id(r.model_id[m])->name);
        text(F_SMALL, SOFT, nx, ry + rh / 2, nw, 20, A_LEFT, r.reason[m].empty() ? fmt(tr(L"화질 %ls", L"Quality %ls"), model_psnr(r.model_id[m])) : r.reason[m]);
        for (int i = 0; i < 3; i++)
        {
            const BenchCell &c = r.cell[m][i];
            float cx = nx + nw + (float)i * colw;
            if (c.state == 1)
            {
                int k = cell_mult(c);
                text(F_BOLD, mult_color(k), cx, ry + rh / 2 - 22, colw - 8, 24, A_LEFT, k >= 2 ? fmt(tr(L"%d배 · %d fps", L"%dx · %d fps"), k, k * bench_fps) : std::wstring(tr(L"불가", L"Too slow")));
                text(F_SMALL, SOFT, cx, ry + rh / 2, colw - 8, 20, A_LEFT, fmt(L"%.2f ms%ls · %.1f ms", (double)c.gen, c.flow == 2 ? L" (1/4)" : c.flow == 3 ? L" (1/8)" : L"", (double)c.latency));
            }
            else text(F_SMALL, DIM, cx, ry, colw - 8, rh, A_LEFT, c.state == 2 ? tr(L"안 됨", L"Not possible") : now && bench_size_now == i ? tr(L"측정 중…", L"Measuring…") : L"—");
        }
        int best = r.valid ? bench_recommended_size(r, m) : -1;
        text(F_BOLD, best >= 0 ? TEXT : DIM, nx + nw + 3 * colw, ry, lw, rh, A_LEFT, best >= 0 ? bench_size_name(best) : r.valid ? tr(L"없음", L"None") : L"—");
    }
    text(F_SMALL, SOFT, x + 18, y + head + (float)rows * rh + 4, w - 36, 26, A_LEFT, tr(L"1/4·1/8은 RIFE 모션 계산 해상도를 낮춘 결과예요. 배수는 상위 95% 생성 시간 기준이고, 모니터 주사율을 넘는 배수는 표시하지 않아요.", L"1/4 and 1/8: RIFE with a smaller motion size. Multipliers use the 95% time. Rates above the monitor refresh are not visible."));
}

// ---- program window ----
static float panel_width() { return !app.show_panel || app.bench_page ? 0 : (float)PANEL_W; }
static void sizes(HWND w, float &W, float &H)
{
    RECT r;
    GetClientRect(w, &r);
    W = (float)r.right * 96 / (float)dpi;
    H = (float)r.bottom * 96 / (float)dpi;
}
static int data_columns(float w, int n) { return std::min(std::max(1, n), std::max(1, (int)((w + GAP) / (232 + GAP)))); }
static float data_height(float h)
{
    if (!data_visible()) return 0;
    float W, H;
    sizes(app.window, W, H);
    int n = 0;
    for (int k = 0; k < CARDS; k++) n += app.cards >> k & 1;
    int cols = data_columns(W - RAIL - panel_width() - EDGE_PAD, n), rows = (n + cols - 1) / cols;
    return std::min(std::min((float)DATA_H, h / 3 + 40), (float)rows * (CARD_H + GAP) + 8);
}
static int px(float v) { return (int)(v * (float)dpi / 96 + 0.5f); }
static void backdrop(HWND w, bool on)
{
    MARGINS m = {on ? -1 : 0, on ? -1 : 0, on ? -1 : 0, on ? -1 : 0};
    DwmExtendFrameIntoClientArea(w, &m);
    int type = on ? 3 : 1; // the blurred desktop, or none
    HRESULT hr = DwmSetWindowAttribute(w, 38, &type, sizeof type);
    glass = on && SUCCEEDED(hr);
}
void ui_layout()
{
    if (!app.view) return;
    static int had = -1;
    if (had != (int)app.fullscreen) { had = app.fullscreen; backdrop(app.window, !app.fullscreen); }
    if (app.fullscreen)
    {
        RECT r;
        GetClientRect(app.window, &r);
        MoveWindow(app.view, 0, 0, r.right, r.bottom, TRUE);
        ShowWindow(app.view, SW_SHOWNA);
        return;
    }
    int d = (int)GetDpiForWindow(app.window);
    if (d != dpi && !ui_open()) dpi = d;
    float w, h;
    sizes(app.window, w, h);
    if (app.bench_page)
    {
        float x, y;
        bench_view(w, x, y);
        MoveWindow(app.view, px(x), px(y), px(240), px(135), TRUE);
        ShowWindow(app.view, bench_running ? SW_SHOWNA : SW_HIDE);
        return;
    }
    float x = RAIL + panel_width(), dh = data_height(h);
    MoveWindow(app.view, px(x), px(TOP), px(w - x - EDGE_PAD), px(h - TOP - dh - EDGE_PAD), TRUE);
    ShowWindow(app.view, SW_SHOWNA);
}
static bool rail_item(float y, float h, const wchar_t *icon, const std::wstring &name, bool on)
{
    bool hov = hot(8, y, RAIL - 16, h);
    if (on || hov) box(8, y, RAIL - 16, h, 10, on ? CARD_HI : CARD);
    if (on) box(8, y + h / 2 - 13, 3.5f, 26, 2, ACCENT);
    float top = y + std::max(0.f, (h - 48) / 2);
    text(F_ICON_BIG, on ? ACCENT_HI : SOFT, 0, top + 1, RAIL, 28, A_CENTER, icon);
    text(F_SMALL, on ? TEXT : SOFT, 2, top + 28, RAIL - 4, 20, A_CENTER, name);
    return pressed(8, y, RAIL - 16, h);
}
static void draw_rail(float H, bool popup)
{
    static const wchar_t *ko[PAGES] = {L"홈", L"생성", L"화질", L"화면", L"입력", L"소리", L"녹화", L"데이터", L"테마", L"벤치마크", L"설정"}, *en[PAGES] = {L"Home", L"Generation", L"Quality", L"Display", L"Input", L"Sound", L"Record", L"Data", L"Theme", L"Benchmark", L"Settings"}, *ic[PAGES] = {L"\xE80F", L"\xE945", L"\xE7B3", L"\xE770", L"\xE7F4", L"\xE767", L"\xE714", L"\xE9D9", L"\xE790", L"\xEC4A", L"\xE713"};
    bool data = !popup && !app.bench_page;
    int n = (int)PAGES + data + (popup ? 0 : 1);
    float step = std::min(68.f, (H - TOP - 14) / (float)n), y = TOP + 6; // everything has its place in a low window too
    int go = -1;
    for (int i = 0; i < PAGES; i++)
    {
        bool on = i == P_BENCH ? app.bench_page && !popup : !app.bench_page && (popup || app.show_panel) && page == i;
        float at = i == P_SETTINGS ? H - 8 - step : y; // the last one, at the bottom
        if (!popup && i == P_SCREEN) mark(MK_RAIL_SCREEN, 8, at, RAIL - 16, step - 6);
        if (!popup && i == P_BENCH) mark(MK_RAIL_BENCH, 8, at, RAIL - 16, step - 6);
        if (rail_item(at, step - 6, ic[i], tr(ko[i], en[i]), on) && !bench_running) { go = i; if (on && !popup) go = PAGES; }
        if (i != P_SETTINGS) y += step;
    }
    float by = H - 8 - 2 * step;
    if (!popup)
    {
        mark(MK_RAIL_GUIDE, 8, by, RAIL - 16, step - 6);
        if (!bench_running && rail_item(by, step - 6, L"\xE897", tr(L"사용 가이드", L"Guide"), guide >= 0)) { guide_go(0); return; }
        by -= step;
    }
    if (data && rail_item(by, step - 6, L"\xE8A1", app.show_data ? tr(L"데이터 숨김", L"Hide data") : tr(L"데이터 보기", L"Show data"), false)) { toggle_data(); return; }
    if (go < 0) return;
    if (go == P_BENCH) { open_bench(); return; }
    bool was = app.bench_page || !app.show_panel;
    app.show_panel = go != PAGES || app.bench_page; // the open page again: hide the column
    if (go != PAGES) page = go;
    app.bench_page = false;
    scroll[0] = 0;
    if (was || !app.show_panel) { tuning_save(); app_screen_changed(); }
}
// The sign of the program: the syllable of its name, written with a brush. The upper and the middle stroke are two originals, the light
// between them the picture made.
static void logo(float x, float y, float size)
{
    static const short p0[] = {236, 149, 245, 157, 256, 167, 271, 178, 292, 185, 320, 188, 354, 188, 390, 187, 426, 184, 463, 179, 502, 173, 539, 166, 572, 159, 599, 153, 623, 147, 643, 140, 662, 134, 677, 126, 690, 117, 701, 108, 712, 102, 724, 99, 735, 97, 746, 98, 757, 102, 767, 109, 778, 120, 787, 132, 793, 145, 796, 157, 795, 171, 793, 184, 788, 195, 781, 203, 773, 210, 761, 215, 746, 219, 724, 221, 699, 221, 670, 220, 639, 222, 604, 225, 565, 230, 527, 237, 494, 245, 467, 255, 444, 267, 423, 280, 404, 290, 385, 298, 368, 303, 354, 308, 342, 311, 330, 300, 314, 286, 296, 268, 281, 249, 268, 225, 255, 196, 244, 169};
    static const short p1[] = {339, 312, 349, 304, 365, 299, 384, 297, 402, 300, 419, 308, 438, 321, 456, 335, 470, 349, 480, 362, 486, 375, 490, 389, 492, 403, 491, 418, 488, 434, 483, 450, 476, 465, 468, 479, 457, 493, 447, 505, 439, 514, 431, 502, 421, 487, 410, 470, 398, 452, 387, 436, 375, 418, 364, 400, 355, 382, 347, 364, 340, 344, 337, 326};
    static const short p2[] = {585, 273, 593, 265, 605, 260, 620, 257, 635, 259, 652, 266, 671, 278, 690, 292, 704, 305, 712, 316, 717, 326, 720, 336, 719, 347, 715, 359, 707, 371, 699, 384, 691, 398, 684, 413, 678, 428, 673, 444, 669, 458, 669, 473, 671, 487, 674, 500, 675, 509, 662, 509, 644, 509, 626, 509, 613, 509, 615, 495, 618, 475, 621, 454, 622, 435, 622, 418, 621, 401, 619, 385, 616, 370, 611, 356, 604, 343, 597, 331, 591, 319, 587, 306, 583, 294, 582, 282};
    static const short p3[] = {210, 516, 220, 524, 232, 535, 249, 546, 271, 553, 302, 556, 339, 557, 378, 555, 418, 552, 458, 547, 501, 541, 541, 533, 577, 526, 607, 519, 633, 513, 655, 506, 675, 499, 692, 491, 706, 481, 718, 473, 730, 467, 743, 463, 756, 461, 768, 462, 779, 466, 791, 473, 802, 485, 812, 498, 819, 511, 822, 524, 821, 538, 818, 552, 813, 563, 806, 572, 797, 579, 784, 585, 767, 589, 744, 591, 716, 590, 684, 590, 651, 591, 612, 595, 570, 601, 528, 607, 492, 616, 462, 626, 437, 640, 414, 653, 394, 663, 373, 671, 355, 677, 339, 682, 326, 686, 313, 674, 295, 659, 276, 641, 259, 620, 245, 595, 231, 565, 219, 537};
    static const short p4[] = {60, 742, 75, 750, 93, 762, 118, 774, 151, 781, 196, 784, 251, 784, 311, 781, 369, 777, 429, 771, 492, 762, 553, 753, 606, 745, 650, 737, 688, 729, 722, 721, 752, 713, 777, 703, 797, 692, 815, 683, 834, 676, 853, 671, 871, 669, 889, 670, 906, 673, 924, 682, 941, 694, 956, 708, 965, 722, 969, 737, 969, 753, 964, 768, 956, 781, 946, 791, 932, 799, 914, 805, 888, 810, 854, 812, 812, 812, 765, 812, 715, 815, 658, 820, 595, 827, 533, 835, 479, 845, 435, 858, 397, 873, 364, 888, 333, 900, 303, 909, 275, 916, 251, 922, 233, 926, 213, 914, 186, 897, 158, 878, 133, 856, 111, 828, 91, 796, 73, 765};
    auto stroke = [&](const short *p, int n, Ink c) {
        std::vector<std::pair<float, float>> l;
        for (int i = 0; i + 1 < n; i += 2) l.push_back({x + (float)p[i] * size / 1024, y + (float)p[i + 1] * size / 1024});
        d_poly(l, c);
    };
    const Ink light = ink(255, 238, 170);
    stroke(p0, (int)(sizeof p0 / sizeof p0[0]), ACCENT);
    stroke(p3, (int)(sizeof p3 / sizeof p3[0]), SECOND);
    stroke(p4, (int)(sizeof p4 / sizeof p4[0]), ACCENT);
    stroke(p1, (int)(sizeof p1 / sizeof p1[0]), light);
    stroke(p2, (int)(sizeof p2 / sizeof p2[0]), light);
}
// Caption: name and the three window buttons.
static void caption(float W)
{
    if (gui.mode == PAINT) logo(18, TOP / 2 - 17, 34);
    text(F_BOLD, TEXT, 60, 0, 120, TOP, A_LEFT, app_name());
    std::wstring s = !app.problem.empty() ? app.problem : app.current.w && app.signal ? fmt(L"%d×%d · %.0f fps · %hs", app.current.w, app.current.h, app.current.fps, pixel_name(app.current.pixel)) : app_status_line();
    bool rec = rec_on();
    if (rec) s = fmt(tr(L"\x25CF 녹화 중 %d:%02d · ", L"\x25CF Recording %d:%02d · "), (int)rec_seconds() / 60, (int)rec_seconds() % 60) + s;
    text(F_SMALL, rec ? ink(255, 96, 86) : app.problem.empty() ? SOFT : AMBER, 138, 0, W - 138 - 308, TOP, A_LEFT, s);
    const wchar_t *g[5] = {rec ? L"\xE71A" : L"\xE7C8", L"\xE740", L"\xE921", IsZoomed(app.window) ? L"\xE923" : L"\xE922", L"\xE8BB"};
    for (int i = 0; i < 5; i++)
    {
        float x = W - 48.f * (float)(5 - i);
        if (i == 0) mark(MK_RECORD, x, 2, 48, TOP - 12);
        if (i == 1) mark(MK_FULL, x, 2, 48, TOP - 12);
        bool hov = hot(x, 0, 48, TOP - 8);
        if (hov) fill(x, 0, 48, TOP - 8, i == 4 ? ink(220, 60, 50) : CARD_HI);
        text(F_ICON, i == 0 ? (rec ? ink(255, 96, 86) : TEXT) : i == 1 ? ACCENT_HI : TEXT, x, 0, 48, TOP - 8, A_CENTER, g[i]);
        if (pressed(x, 0, 48, TOP - 8))
        {
            if (i == 0) app_video_record(!rec);
            else if (i == 1) app_fullscreen(true);
            else if (i == 2) ShowWindow(app.window, SW_MINIMIZE);
            else if (i == 3) ShowWindow(app.window, IsZoomed(app.window) ? SW_RESTORE : SW_MAXIMIZE);
            else PostMessageW(app.window, WM_CLOSE, 0, 0);
            return;
        }
    }
}
static void background(float W, float H, bool see_through)
{
    if (gui.mode != PAINT) return;
    if (!THEME_IMAGE.empty() && d_image(THEME_IMAGE, W, H, 1)) d_fill(0, 0, W, H, fade(BASE, 204));
    else d_fill(0, 0, W, H, see_through ? fade(BASE, 150) : theme_shade(1.5f));
    d_glow(0, H, std::min(W, H) * 0.9f, fade(ACCENT, THEME_GLOW)); // a little light from the corners
    d_glow(W, 0, std::min(W, H) * 0.7f, fade(SECOND, THEME_GLOW * 7 / 10));
    d_fill(0, 0, RAIL, H, ink(0, 0, 0, 46));
}
static void draw_main()
{
    float w, h;
    sizes(app.window, w, h);
    static bool first = true;
    if (first && gui.mode == PAINT)
    {
        wchar_t v[8];
        if (GetEnvironmentVariableW(L"PKAPP_GUIDE", v, 8)) guide_go(_wtoi(v)); // tests: a step of the guide
    }
    first = false;
    for (bool &m : marked) m = false;
    guide_block = guide >= 0;
    background(w, h, glass && app.see_through);
    caption(w);
    draw_rail(h, false);
    if (app.bench_page) { guide_block = false; draw_bench(w, h); return; }
    float pw = panel_width(), dh = data_height(h);
    if (pw > 0) draw_column(RAIL, TOP, pw, h - TOP - 4, false);
    if (dh > 0) draw_data(RAIL + pw, h - dh - EDGE_PAD + 4, w - RAIL - pw - EDGE_PAD, dh);
    guide_block = false;
    guide_draw(w, h);
}
// ---- popup over full screen ----
static void draw_popup()
{
    float w, h;
    sizes(ui_window, w, h);
    background(w, h, false);
    text(F_HEAD, TEXT, RAIL + 16, 0, 200, TOP, A_LEFT, tr(L"설정", L"Settings"));
    bool hov = hot(w - 48, 0, 48, TOP);
    if (hov) fill(w - 48, 0, 48, TOP, CARD_HI);
    text(F_ICON, TEXT, w - 48, 0, 48, TOP, A_CENTER, L"\xE8BB");
    if (pressed(w - 48, 0, 48, TOP)) { ui_toggle(); return; }
    draw_rail(h, true);
    draw_column(RAIL, TOP, w - RAIL, h - TOP - 4, true);
}

// ---- painting and events, shared by both windows ----
static void draw(HWND w) { if (w == ui_window) draw_popup(); else draw_main(); }
static void paint(HWND w)
{
    static HDC mem;
    static HBITMAP bm;
    static HGDIOBJ first;
    static SIZE have;
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    int pw = ps.rcPaint.right - ps.rcPaint.left, ph = ps.rcPaint.bottom - ps.rcPaint.top;
    if (pw > 0 && ph > 0)
    {
        if (!mem) mem = CreateCompatibleDC(dc);
        if (pw > have.cx || ph > have.cy)
        {
            have = {std::max<LONG>(have.cx, pw), std::max<LONG>(have.cy, ph)};
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = have.cx;
            bi.bmiHeader.biHeight = -have.cy;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            void *bits = nullptr;
            HBITMAP n = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            HGDIOBJ old = SelectObject(mem, n);
            if (!first) first = old;
            if (bm) DeleteObject(bm);
            bm = n;
        }
        auto kept = gui; // a question on the screen paints in the middle of a press
        gui.mode = PAINT;
        gui.clip = {0, 0, 1e6f, 1e6f};
        if (d_begin(mem, ps.rcPaint, dpi))
        {
            draw(w);
            d_end();
            BitBlt(dc, ps.rcPaint.left, ps.rcPaint.top, pw, ph, mem, 0, 0, SRCCOPY);
        }
        gui.mode = kept.mode;
        gui.clip = kept.clip;
    }
    EndPaint(w, &ps);
}
static void refresh(HWND w)
{
    if (w == ui_window || app.bench_page) { InvalidateRect(w, nullptr, FALSE); return; }
    float W, H;
    sizes(w, W, H);
    RECT all, r;
    GetClientRect(w, &all);
    r = {0, 0, all.right, px(TOP)};
    InvalidateRect(w, &r, FALSE);
    float pw = panel_width(), dh = data_height(H);
    r = {0, px(TOP), px(RAIL + pw), all.bottom};
    InvalidateRect(w, &r, FALSE);
    if (dh > 0) { r = {px(RAIL + pw), px(H - dh - EDGE_PAD), all.right, all.bottom}; InvalidateRect(w, &r, FALSE); }
}
static void event(HWND w, Mode m, LPARAM lp)
{
    gui.mode = m;
    gui.mx = (float)(short)LOWORD(lp) * 96 / (float)dpi;
    gui.my = (float)(short)HIWORD(lp) * 96 / (float)dpi;
    draw(w);
    if (m == DOWN && gui.drag >= 0) SetCapture(w);
    if (m == UP) { gui.drag = -1; if (GetCapture() == w) ReleaseCapture(); }
    refresh(w);
}
static void wheel(HWND w, WPARAM a, LPARAM b)
{
    POINT p = {GET_X_LPARAM(b), GET_Y_LPARAM(b)};
    ScreenToClient(w, &p);
    float x = (float)p.x * 96 / (float)dpi, y = (float)p.y * 96 / (float)dpi, W, H;
    int k = 0;
    if (w != ui_window)
    {
        sizes(w, W, H);
        if (x > RAIL + panel_width()) k = 1;
        if (k == 1 && y < H - data_height(H) - EDGE_PAD) return;
    }
    scroll[k] = std::min(scroll_max[k], std::max(0.f, scroll[k] - (float)GET_WHEEL_DELTA_WPARAM(a) * 70 / WHEEL_DELTA));
    refresh(w);
}
static bool common(HWND w, UINT m, WPARAM a, LPARAM b, LRESULT &r)
{
    r = 0;
    switch (m)
    {
    case WM_PAINT: paint(w); return true;
    case WM_LBUTTONDOWN:
        SetFocus(w);
        gui.drag = -1;
        if (app.hot_capture) { app.hot_capture = false; app_hotkey(); } // another press ends the waiting for a key
        event(w, DOWN, b);
        return true;
    case WM_MOUSEMOVE:
    {
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, w, 0};
        TrackMouseEvent(&t);
        event(w, MOVE, b);
        return true;
    }
    case WM_LBUTTONUP: event(w, UP, b); return true;
    case WM_MOUSELEAVE: gui.mx = gui.my = -1; refresh(w); return true;
    case WM_CAPTURECHANGED: if ((HWND)b != w) gui.drag = -1; return true;
    case WM_MOUSEWHEEL: wheel(w, a, b); return true;
    }
    return false;
}
bool ui_message(HWND w, UINT m, WPARAM a, LPARAM b, LRESULT &r)
{
    if (app.fullscreen)
    {
        if (m != WM_PAINT) return false;
        ValidateRect(w, nullptr);
        r = 0;
        return true;
    }
    switch (m)
    {
    case WM_NCCALCSIZE: // no system caption: it is drawn here
    {
        if (!a) return false;
        auto *p = (NCCALCSIZE_PARAMS *)b;
        LONG top = p->rgrc[0].top;
        r = DefWindowProcW(w, m, a, b);
        p->rgrc[0].top = IsZoomed(w) ? top + GetSystemMetricsForDpi(SM_CYSIZEFRAME, GetDpiForWindow(w)) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, GetDpiForWindow(w)) : top;
        r = 0;
        return true;
    }
    case WM_NCACTIVATE: r = DefWindowProcW(w, m, TRUE, -1); return true; // stays see-through while another window is used
    case WM_NCHITTEST:
    {
        r = DefWindowProcW(w, m, a, b);
        if (r != HTCLIENT) return true;
        POINT p = {GET_X_LPARAM(b), GET_Y_LPARAM(b)};
        ScreenToClient(w, &p);
        float W, H;
        sizes(w, W, H);
        float x = (float)p.x * 96 / (float)dpi, y = (float)p.y * 96 / (float)dpi;
        if (!IsZoomed(w) && y < 6) r = x < 8 ? HTTOPLEFT : x > W - 8 ? HTTOPRIGHT : HTTOP;
        else if (y < TOP - 6 && x < W - 48 * 5) r = HTCAPTION;
        return true;
    }
    }
    return common(w, m, a, b, r);
}
static LRESULT CALLBACK popup_proc(HWND w, UINT m, WPARAM a, LPARAM b)
{
    LRESULT r;
    if (common(w, m, a, b, r)) return r;
    switch (m)
    {
    case WM_ERASEBKGND: return 1;
    case WM_KEYDOWN: if (a == VK_ESCAPE || a == VK_F1) ui_toggle(); else if (a == VK_F11) app_fullscreen(!app.fullscreen); return 0;
    case WM_CLOSE: ui_toggle(); return 0;
    }
    return DefWindowProcW(w, m, a, b);
}
void ui_init()
{
    dpi = (int)GetDpiForWindow(app.window);
    if (!d_init()) logf("ui: drawing not available");
    WNDCLASSW c{};
    c.lpfnWndProc = popup_proc;
    c.hInstance = GetModuleHandleW(nullptr);
    c.lpszClassName = L"PeussokAppSettings";
    c.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    RegisterClassW(&c);
    SetWindowPos(app.window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}
bool ui_open() { return ui_window && IsWindowVisible(ui_window); }
void ui_toggle()
{
    if (!app.fullscreen && !ui_open()) // a window: show or hide the column
    {
        if (bench_running) return;
        if (app.bench_page) app.bench_page = false;
        else app.show_panel = !app.show_panel;
        tuning_save();
        app_screen_changed();
        return;
    }
    if (!ui_window)
    {
        ui_window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, L"PeussokAppSettings", tr(L"프쏙 설정", L"PEUSSOK settings"), WS_POPUP, 0, 0, 100, 100, app.window, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetLayeredWindowAttributes(ui_window, 0, 232, LWA_ALPHA); // the game shows through a little
    }
    if (ui_open())
    {
        ShowWindow(ui_window, SW_HIDE);
        if (app.hot_capture) { app.hot_capture = false; app_hotkey(); }
        SetForegroundWindow(app.window);
        return;
    }
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(app.window, MONITOR_DEFAULTTONEAREST), &mi);
    dpi = (int)GetDpiForWindow(app.window);
    int w = px(RAIL + PANEL_W + 8), h = mi.rcMonitor.bottom - mi.rcMonitor.top;
    SetWindowPos(ui_window, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, w, h, SWP_SHOWWINDOW);
    SetForegroundWindow(ui_window);
    SetFocus(ui_window);
}
bool ui_needs_numbers()
{
    if (IsIconic(app.window)) return false;
    if (app.fullscreen) return ui_open() && page == P_DATA;
    return data_visible();
}
void ui_animate()
{
    // how many pictures reached the screen in the last half second, against what the multiplier asks for
    static LONGLONG t;
    static LONG64 shown, sources;
    LONGLONG now = qpc();
    double ms = t ? ms_of(now - t) : 0;
    if (!t || ms >= 500)
    {
        if (t)
        {
            double s = (double)(num.sources - sources), n = (double)(num.shown - shown), m = model_by_id(tune.model)->multi ? (double)tune.mult : 2;
            bool live = (app.signal || bench_running) && tune.on && s > 2;
            gauge_target = live ? (float)std::min(1.0, std::max(0.0, (n / s - 1) / (m - 1))) : 0;
            if (gauge_target > 0.97f) gauge_target = 1; // a picture more or less at the edge of the half second
        }
        t = now;
        shown = num.shown;
        sources = num.sources;
    }
    float before = gauge;
    gauge += (gauge_target - gauge) * 0.22f;
    if (std::fabs(gauge_target - gauge) < 0.004f) gauge = gauge_target;
    if (gauge == before || GetTickCount64() - ring_seen > 600) return; // not on the screen, or standing still
    HWND w = ui_open() ? ui_window : app.window;
    RECT r = {px(ring_area.l), px(ring_area.t), px(ring_area.r), px(ring_area.b)};
    InvalidateRect(w, &r, FALSE);
}
void ui_tick()
{
    static ULONGLONG second;
    if (GetTickCount64() - second >= 1000)
    {
        second = GetTickCount64();
        SysLive s = sys_live();
        int at = history_n % HISTORY;
        bool live = app.signal || bench_running;
        history[H_SHOWN][at] = live ? (float)app_shown_fps() : 0;
        history[H_SOURCE][at] = live ? (float)app_source_fps() : 0;
        history[H_LATENCY][at] = live ? (float)num.latency.get() : 0;
        history[H_GPU][at] = s.gpu;
        history[H_CPU][at] = s.cpu;
        history_n++;
        peak_ms = num.lat_peak / 1000.0;
        InterlockedExchange(&num.lat_peak, 0);
    }
    if (ui_open()) refresh(ui_window);
    else if (!app.fullscreen && !IsIconic(app.window)) refresh(app.window);
}
bool ui_key(const MSG &m)
{
    if (!app.hot_capture || (m.message != WM_KEYDOWN && m.message != WM_SYSKEYDOWN)) return false;
    int vk = (int)m.wParam;
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_PROCESSKEY || vk == VK_HANGUL) return true;
    int mods = (GetKeyState(VK_CONTROL) < 0 ? MOD_CONTROL : 0) | (GetKeyState(VK_MENU) < 0 ? MOD_ALT : 0) | (GetKeyState(VK_SHIFT) < 0 ? MOD_SHIFT : 0);
    if (vk != VK_ESCAPE)
    {
        if (!(mods & (MOD_CONTROL | MOD_ALT))) return true;
        app.hot_mods = mods;
        app.hot_vk = vk;
    }
    app.hot_capture = false;
    app_hotkey();
    tuning_save();
    return true;
}
