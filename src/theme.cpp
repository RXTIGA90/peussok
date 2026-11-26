// SPDX-License-Identifier: GPL-3.0-only
#include "theme.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

Ink ACCENT, ACCENT_HI, ON_ACCENT, TEXT, SOFT, DIM, SECOND, BASE;
int THEME_GLOW;
std::wstring THEME_IMAGE;
volatile LONG mine_hue = 190, mine_sat = 80, mine_base = 220, mine_dark = 14, mine_glow = 40;
static std::vector<Theme> list;
static int now;

static Ink hsv(double h, double s, double v)
{
    double c = v * s, x = c * (1 - fabs(fmod(h / 60, 2) - 1)), m = v - c, r = 0, g = 0, b = 0;
    if (h < 60) { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else { r = c; b = x; }
    return ink((int)((r + m) * 255 + .5), (int)((g + m) * 255 + .5), (int)((b + m) * 255 + .5));
}
static int part(Ink c, int shift) { return (int)(c >> shift & 255); }
static Ink mix(Ink a, Ink b, double t)
{
    auto f = [&](int s) { return (int)(part(a, s) * (1 - t) + part(b, s) * t + .5); };
    return ink(f(16), f(8), f(0));
}
static double light(Ink c) { return (0.299 * part(c, 16) + 0.587 * part(c, 8) + 0.114 * part(c, 0)) / 255; }
static void apply(const Theme &t)
{
    ACCENT = t.accent;
    ACCENT_HI = mix(t.accent, ink(255, 255, 255), 0.22);
    ON_ACCENT = light(t.accent) > 0.45 ? mix(t.accent, ink(0, 0, 0), 0.9) : ink(255, 255, 255);
    TEXT = mix(ink(246, 247, 248), t.accent, 0.04);
    SOFT = fade(TEXT, 150);
    DIM = fade(TEXT, 84);
    SECOND = t.second;
    BASE = t.base;
    THEME_GLOW = t.glow;
    THEME_IMAGE = t.image;
}
static Theme mine()
{
    double h = mine_hue, s = mine_sat / 100.0;
    return {L"mine", L"내 테마", L"My theme", hsv(h, s, 1), hsv(fmod(h + 60, 360), s * 0.8, 1), hsv(mine_base, 0.6, mine_dark / 100.0), (int)mine_glow, ini_text(L"theme_image", L"")};
}
// A theme file: lines "key = value", UTF-8. name; accent, second, background as #RRGGBB; glow 0-120; image (a file next to it).
static bool read_file(const std::wstring &path, const std::wstring &folder, const std::wstring &file, Theme &t)
{
    FILE *f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    char raw[8192];
    size_t n = fread(raw, 1, sizeof raw - 1, f);
    fclose(f);
    raw[n] = 0;
    wchar_t w[8192];
    const char *from = n >= 3 && !memcmp(raw, "\xEF\xBB\xBF", 3) ? raw + 3 : raw;
    if (!MultiByteToWideChar(CP_UTF8, 0, from, -1, w, 8192)) return false;
    t = {file, file, file, ink(134, 206, 44), ink(60, 170, 220), ink(14, 16, 18), 30, L""};
    bool any = false;
    std::wstring all = w;
    for (size_t at = 0; at < all.size();)
    {
        size_t end = all.find_first_of(L"\r\n", at);
        std::wstring line = all.substr(at, end == all.npos ? all.npos : end - at);
        at = end == all.npos ? all.size() : end + 1;
        size_t eq = line.find(L'=');
        if (eq == line.npos || line[0] == L';') continue;
        auto trim = [](const std::wstring &s) {
            size_t a = s.find_first_not_of(L" \t"), b = s.find_last_not_of(L" \t");
            return a == s.npos ? std::wstring() : s.substr(a, b - a + 1);
        };
        std::wstring key = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        unsigned rgb = 0;
        bool is_color = v.size() == 7 && v[0] == L'#' && swscanf(v.c_str() + 1, L"%x", &rgb) == 1;
        Ink c = 0xFF000000u | (rgb & 0xFFFFFF);
        if (key == L"name" && !v.empty()) { t.ko = t.en = v.substr(0, 24); any = true; }
        else if (key == L"accent" && is_color) { t.accent = c; any = true; }
        else if (key == L"second" && is_color) t.second = c;
        else if (key == L"background" && is_color) t.base = c;
        else if (key == L"glow") t.glow = std::min(120, std::max(0, _wtoi(v.c_str())));
        else if (key == L"image" && !v.empty() && v.find_first_of(L"\\/:") == v.npos) t.image = folder + L"\\" + v;
    }
    return any;
}
std::wstring theme_folder()
{
    std::wstring d = data_dir() + L"\\themes";
    CreateDirectoryW(d.c_str(), nullptr);
    std::wstring example = d + L"\\example.txt";
    if (GetFileAttributesW(example.c_str()) == INVALID_FILE_ATTRIBUTES)
        if (FILE *f = _wfopen(example.c_str(), L"wb"))
        {
            fputs("\xEF\xBB\xBF"
                  "; 이 파일을 복사해 이름.ini 로 저장하면 테마 목록에 나와요. / Copy this file as name.ini to add a theme.\r\n"
                  "name = 바다\r\naccent = #2FC6E0\r\nsecond = #7A6CFF\r\nbackground = #06141E\r\nglow = 50\r\n"
                  "; image = picture.jpg  (같은 폴더의 그림 / a picture in this folder)\r\n", f);
            fclose(f);
        }
    return d;
}
void themes_load()
{
    list = {
        {L"green", L"그린", L"Green", ink(134, 206, 44), ink(60, 170, 220), ink(12, 18, 10), 26, L""},
        {L"blue", L"블루", L"Blue", ink(64, 152, 255), ink(126, 96, 255), ink(6, 13, 34), 44, L""},
        {L"red", L"레드", L"Red", ink(242, 58, 64), ink(255, 150, 90), ink(30, 8, 10), 44, L""},
        {L"sunset", L"선셋", L"Sunset", ink(255, 96, 160), ink(255, 156, 62), ink(32, 10, 42), 76, L""},
        {L"neon", L"네온", L"Neon", ink(178, 120, 255), ink(0, 222, 214), ink(14, 9, 32), 56, L""},
        {L"gold", L"골드", L"Gold", ink(255, 192, 72), ink(255, 112, 72), ink(26, 18, 6), 40, L""},
        {L"mono", L"모노", L"Mono", ink(226, 229, 234), ink(140, 150, 164), ink(15, 16, 18), 14, L""},
        mine()};
    std::wstring folder = data_dir() + L"\\themes";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((folder + L"\\*.ini").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            Theme t;
            if (list.size() < 32 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && read_file(folder + L"\\" + fd.cFileName, folder, fd.cFileName, t)) list.push_back(t);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::wstring id = ini_text(L"theme", L"green");
    now = 0;
    for (size_t i = 0; i < list.size(); i++)
        if (list[i].id == id) now = (int)i;
    apply(list[(size_t)now]);
}
const std::vector<Theme> &themes()
{
    if (list.empty()) themes_load();
    return list;
}
int theme_now() { return now; }
void theme_use(int i)
{
    if (i < 0 || i >= (int)list.size()) return;
    now = i;
    apply(list[(size_t)i]);
    ini_set(L"theme", list[(size_t)i].id);
}
void theme_mix()
{
    for (size_t i = 0; i < list.size(); i++)
        if (list[i].id == L"mine")
        {
            list[i] = mine();
            theme_use((int)i);
        }
}
void theme_image(const std::wstring &path)
{
    ini_set(L"theme_image", path);
    theme_mix();
}
