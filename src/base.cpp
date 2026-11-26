// SPDX-License-Identifier: GPL-3.0-only
#include "base.h"
#include <algorithm>
#include <shlobj.h>

LONGLONG qpc_freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
LONGLONG qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

void wait_until(LONGLONG target)
{
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* high resolution */, TIMER_ALL_ACCESS);
    for (;;)
    {
        double left = ms_of(target - qpc());
        if (left <= 0) return;
        if (left > 0.7 && timer)
        {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((left - 0.5) * 10000.0);
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) { WaitForSingleObject(timer, INFINITE); continue; }
        }
        YieldProcessor();
    }
}

std::wstring exe_dir()
{
    wchar_t p[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, p, MAX_PATH * 4);
    std::wstring s(p, n);
    size_t k = s.find_last_of(L'\\');
    return k == std::wstring::npos ? L"." : s.substr(0, k);
}
// The portable version keeps everything in its own folder: with portable.txt beside the program, LOCALAPPDATA of this process (and
// of what it starts: the guard, the VFG download) points at <program>\data, so data_dir(), the NVIDIA VFG and DLSS FG runtimes and
// the download all go there. The installed version has no portable.txt.
bool portable_setup()
{
    std::wstring d = exe_dir();
    if (GetFileAttributesW((d + L"\\portable.txt").c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    d += L"\\data";
    CreateDirectoryW(d.c_str(), nullptr);
    SetEnvironmentVariableW(L"LOCALAPPDATA", d.c_str());
    return true;
}
std::wstring data_dir()
{
    static std::wstring dir = [] {
        portable_setup(); // first: names in tables are made (and settings read) before wWinMain runs
        wchar_t p[MAX_PATH] = L"";
        GetEnvironmentVariableW(L"LOCALAPPDATA", p, MAX_PATH);
        std::wstring d = std::wstring(p) + L"\\Peussok";
        CreateDirectoryW(d.c_str(), nullptr);
        return d;
    }();
    return dir;
}

static FILE *log_file;
static SRWLOCK log_lock = SRWLOCK_INIT;
static LONGLONG log_start;
void log_open()
{
    log_start = qpc();
    std::wstring p = data_dir() + L"\\app.log", old = data_dir() + L"\\app-previous.log";
    MoveFileExW(p.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    log_file = _wfopen(p.c_str(), L"wb");
}
void logf(const char *format, ...)
{
    char line[1024];
    va_list a;
    va_start(a, format);
    vsnprintf(line, sizeof line, format, a);
    va_end(a);
    AcquireSRWLockExclusive(&log_lock);
    if (log_file) { fprintf(log_file, "%10.3f %s\r\n", ms_of(qpc() - log_start) / 1000.0, line); fflush(log_file); }
    ReleaseSRWLockExclusive(&log_lock);
}

// Windows keeps a settings file in UTF-16 only when it begins with a mark; without it names in other scripts are lost.
static void ini_unicode(const std::wstring &p)
{
    std::vector<char> raw;
    if (FILE *f = _wfopen(p.c_str(), L"rb"))
    {
        char b[4096];
        for (size_t n; (n = fread(b, 1, sizeof b, f)) > 0 && raw.size() < 1 << 20;) raw.insert(raw.end(), b, b + n);
        fclose(f);
        if (raw.size() >= 2 && raw[0] == '\xFF' && raw[1] == '\xFE') return;
    }
    std::wstring all;
    if (!raw.empty())
    {
        int n = MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), nullptr, 0);
        all.resize((size_t)std::max(0, n));
        if (n > 0) MultiByteToWideChar(CP_ACP, 0, raw.data(), (int)raw.size(), &all[0], n);
        if (n <= 0) return;
    }
    std::wstring t = p + L".new";
    FILE *f = _wfopen(t.c_str(), L"wb");
    if (!f) return;
    bool ok = fwrite("\xFF\xFE", 1, 2, f) == 2 && (all.empty() || fwrite(all.data(), 2, all.size(), f) == all.size());
    ok = !fclose(f) && ok;
    if (ok) MoveFileExW(t.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
    else DeleteFileW(t.c_str());
}
static const std::wstring &ini_path()
{
    static std::wstring p = [] { std::wstring s = data_dir() + L"\\settings.ini"; ini_unicode(s); return s; }();
    return p;
}
int ini_int(const wchar_t *key, int def) { return (int)GetPrivateProfileIntW(L"app", key, def, ini_path().c_str()); }
std::wstring ini_text(const wchar_t *key, const wchar_t *def)
{
    wchar_t v[1024];
    GetPrivateProfileStringW(L"app", key, def, v, 1024, ini_path().c_str());
    return v;
}
void ini_set(const wchar_t *key, const std::wstring &value) { WritePrivateProfileStringW(L"app", key, value.c_str(), ini_path().c_str()); }
void ini_set(const wchar_t *key, int value) { ini_set(key, std::to_wstring(value)); }

volatile LONG app_language;
bool korean()
{
    static bool ko = [] {
        wchar_t v[8];
        if (GetEnvironmentVariableW(L"PKAPP_LANG", v, 8)) { app_language = !_wcsnicmp(v, L"ko", 2) ? 1 : 2; }
        else app_language = ini_int(L"language", 0);
        return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_KOREAN;
    }();
    return app_language == 1 || (app_language != 2 && ko);
}
const wchar_t *app_name() { return tr(L"프쏙", L"PEUSSOK"); }
