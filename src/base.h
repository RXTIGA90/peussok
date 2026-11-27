// SPDX-License-Identifier: GPL-3.0-only
// Shared small helpers: time, log, settings, COM release.
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define APP_VERSION L"1.0.2"

template <class T> inline void release(T *&p) { if (p) { p->Release(); p = nullptr; } }

// ---- time ----
LONGLONG qpc();
extern LONGLONG qpc_freq;
inline double ms_of(LONGLONG ticks) { return 1000.0 * (double)ticks / (double)qpc_freq; }
inline LONGLONG ticks_of(double ms) { return (LONGLONG)(ms * (double)qpc_freq / 1000.0); }
void wait_until(LONGLONG target); // high resolution timer, then a short spin

// ---- log: %LOCALAPPDATA%\Peussok\app.log (started new each run) ----
void log_open();
void logf(const char *format, ...);
std::wstring data_dir();
std::wstring exe_dir();
bool portable_setup(); // portable.txt beside the program: settings, logs and NVIDIA files in its data folder (call first)

// ---- settings: %LOCALAPPDATA%\Peussok\settings.ini ----
int ini_int(const wchar_t *key, int def);
std::wstring ini_text(const wchar_t *key, const wchar_t *def);
void ini_set(const wchar_t *key, int value);
void ini_set(const wchar_t *key, const std::wstring &value);

// ---- language ----
bool korean();
extern volatile LONG app_language; // 0 that of Windows, 1 Korean, 2 English
inline const wchar_t *tr(const wchar_t *ko, const wchar_t *en) { return korean() ? ko : en; }

// Value smoothed over about 8 samples, read by another thread.
struct Smooth
{
    volatile LONG v = 0; // x100
    void add(double x)
    {
        LONG now = (LONG)(x * 100 + 0.5), old = v;
        if (old) now = (LONG)(old + (now - old) / 8.0 + (now > old ? 0.5 : -0.5));
        InterlockedExchange(&v, now);
    }
    double get() const { return v / 100.0; }
    void clear() { InterlockedExchange(&v, 0); }
};
const wchar_t *app_name(); // the name people see
