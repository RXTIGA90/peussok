// SPDX-License-Identifier: GPL-3.0-only
// What the settings window needs from the program (main.cpp).
#pragma once
#include "audio.h"
#include "capture.h"
#include "video.h"

struct AppState
{
    HWND window = nullptr, view = nullptr; // the program window and the picture inside it
    std::vector<Device> devices;
    std::wstring device;         // path of the chosen device
    VideoFormat want;            // w == 0: automatic
    bool want_rate = false;      // the frame rate was chosen by the user (not the largest up to 60)
    VideoFormat current;         // what the device delivers
    std::vector<VideoFormat> formats;
    AudioFormat audio;
    std::wstring problem;        // why there is no picture, empty when fine
    bool fullscreen = false, signal = false, sound = true;
    int hot_mods = MOD_CONTROL | MOD_ALT, hot_vk = 'G';
    bool hot_ok = false, hot_capture = false;
    // what the window shows
    bool show_panel = true, show_data = true, bench_page = false;
    volatile LONG cards = 0x7ff; // bit per data card
    bool testing = false;
    bool recording = false;      // numbers to a CSV file
    int start_on = 0, start_full = 0; // when the program starts: 0 as it was last, 1 on / full screen, 2 off / window
    bool awake = true, topmost = false, see_through = true; // keep the monitor on; the window above others; the desktop shines through
};
extern AppState app;
void app_open();                     // (re)open the chosen device with the wanted format
void app_refresh_devices();
void app_fullscreen(bool on);
void app_generation(bool on);
void app_hotkey();                   // register again
void app_video_record(bool on);      // recording the picture on or off (Ctrl+Alt+R)
std::wstring app_hotkey_name();
std::wstring app_status_line();
double app_shown_fps();
void tuning_load();
void tuning_save();                  // everything that is set in the window
void tuning_reset_keys(std::initializer_list<const wchar_t *> keys); // the settings of one tab, as on the first start
void tuning_reset();                 // as on the first start
void app_topmost();
bool app_autostart();                // Windows starts the program
void app_autostart(bool on);

double app_source_fps();
double app_made_fps();
double app_repeat_percent();          // pictures of the card that repeat the one before
void app_record(bool on);
void app_bench(bool full);
void app_screen_changed();            // lay the window out again
std::wstring app_monitor_name();

// ui.cpp: the program window (picture, settings panel, data cards, benchmark page) and the settings popup over full screen
void ui_init();
void ui_layout();                     // places the picture
void ui_toggle();                     // full screen: the settings popup; window: the settings panel
bool ui_open();
bool ui_guide_escape();               // Esc closes the user guide first (true: it was open)                       // popup open
void ui_animate();                    // window thread, 15 times a second: the ring of the home page
void ui_tick();                       // window thread, 4 times a second
bool ui_key(const MSG &m);            // hot key recording
bool ui_message(HWND w, UINT m, WPARAM a, LPARAM b, LRESULT &r); // mouse, paint and wheel of the program window
bool ui_needs_numbers();              // something on the screen shows computer numbers
extern HWND ui_window;

void video_set_label(const std::wstring &text, int px);
