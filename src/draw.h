// SPDX-License-Identifier: GPL-3.0-only
// Drawing for the windows of the program: Direct2D and DirectWrite in software, into a bitmap with an alpha channel, so the window
// can be see-through. Nothing here uses the graphics card.
#pragma once
#include "base.h"

typedef uint32_t Ink; // 0xAARRGGBB, not premultiplied
constexpr Ink ink(int r, int g, int b, int a = 255) { return (Ink)a << 24 | (Ink)r << 16 | (Ink)g << 8 | (Ink)b; }
constexpr Ink fade(Ink c, int a) { return (c & 0xFFFFFF) | (Ink)a << 24; }

enum Font { F_SMALL, F_BODY, F_BOLD, F_HEAD, F_TITLE, F_BIG, F_HUGE, F_ICON, F_ICON_BIG, FONTS };
enum Align { A_LEFT, A_CENTER, A_RIGHT, A_WRAP };

bool d_init();
bool d_begin(HDC mem, const RECT &paint, int dpi); // paint: pixels of the window that are drawn into (0,0) of the bitmap
void d_end();
void d_text(Font f, Ink c, float x, float y, float w, float h, Align a, const std::wstring &s);
float d_width(Font f, const std::wstring &s);
void d_box(float x, float y, float w, float h, float radius, Ink fill, Ink line = 0, float width = 1);
void d_fill(float x, float y, float w, float h, Ink c);
void d_circle(float cx, float cy, float r, Ink fill, Ink line = 0, float width = 1);
void d_arc(float cx, float cy, float r, float part, Ink c, float width); // from the top, clockwise, part 0-1
void d_glow(float cx, float cy, float r, Ink c);                       // soft light, strongest in the middle
void d_lines(const std::vector<std::pair<float, float>> &p, Ink c, float width, Ink under, float base); // under: filled down to base
void d_clip(float x, float y, float w, float h);
void d_unclip();
bool d_visible(float x, float y, float w, float h);
bool d_image(const std::wstring &path, float w, float h, float alpha); // covers the rectangle from 0,0
void d_poly(const std::vector<std::pair<float, float>> &p, Ink c); // filled
