// SPDX-License-Identifier: GPL-3.0-only
// A monitor or one window as the input, for remote play and PC games (Desktop Duplication; our own windows are left out of it).
#pragma once
#include "capture.h"

bool screen_path(const std::wstring &path);   // "screen:..." or "window:..."
std::vector<Device> screen_devices();         // the monitors (a window still works when its path is given)
std::wstring screen_exe(HWND w);               // lower case file name of the program of a window
void screen_exclude_own(bool on);             // our windows are not in any capture (or are again)

class ScreenCapture
{
public:
    ~ScreenCapture() { close(); }
    std::wstring open(const std::wstring &path, bool sound, CaptureSink *sink); // empty or the reason
    void close();
    bool running() const { return thread != nullptr; }
    VideoFormat video() const;
    const AudioFormat &audio() const { return af; }
    RECT area() const; // the part of the desktop that is taken (empty before the first picture)

private:
    static DWORD WINAPI run(void *self);
    static DWORD WINAPI run_sound(void *self);
    void loop();
    void sound_loop();
    std::wstring path;
    CaptureSink *sink = nullptr;
    HANDLE thread = nullptr, sound = nullptr;
    volatile LONG stop = 0;
    mutable SRWLOCK fl = SRWLOCK_INIT;
    VideoFormat vf;
    RECT taken{};
    AudioFormat af;
};
