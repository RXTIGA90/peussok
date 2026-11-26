// SPDX-License-Identifier: GPL-3.0-only
// Colours of the program window: built-in themes, one the person mixes, and themes from files.
#pragma once
#include "draw.h"
struct Theme
{
    std::wstring id, ko, en;  // id: name in the settings; files: the file's name
    Ink accent, second, base; // marks and buttons, the other light and the second chart colour, the window itself
    int glow;                 // strength of the lights in the corners, 0-120
    std::wstring image;       // picture behind everything
};
extern volatile LONG mine_hue, mine_sat, mine_base, mine_dark, mine_glow; // the person's own mix
void themes_load();
const std::vector<Theme> &themes();
int theme_now();
void theme_use(int i);                      // remembers it
void theme_mix();                           // the sliders moved
void theme_image(const std::wstring &path); // picture of the person's own theme, empty: none
std::wstring theme_folder();                // made with an example when it is not there
// the colours in use
extern Ink ACCENT, ACCENT_HI, ON_ACCENT, TEXT, SOFT, DIM, SECOND, BASE;
extern int THEME_GLOW;
extern std::wstring THEME_IMAGE;
