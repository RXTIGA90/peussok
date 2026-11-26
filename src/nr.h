// SPDX-License-Identifier: GPL-3.0-only
// NVIDIA's neural rendering (DLSS 5) on the originals; by default laid over the made pictures too, after they are made (nr_order).
// Nothing of NVIDIA's is part of this program: the runtime (nvngx_dlssnr.dll) is the one a game on this computer brought along. It is
// opened the ordinary way, through the display driver, and only when its signature is NVIDIA's; nothing is patched, hooked or renamed.
// The network looks at a smaller picture, and only what it changes is laid over the full one: sharpness stays, the cost does not grow
// with 4K. What stands still (HUD) keeps its own colours.
#pragma once
#include "base.h"
#include <d3d11_4.h>
struct IDXGIAdapter1;
bool nr_built();                                 // false: left out of this build
std::wstring nr_file();                          // the runtime, empty: none found
int nr_there();                                  // the same without waiting: 1 found, 0 none, -1 being looked for
int nr_choice();                                 // changes when another file is chosen
std::wstring nr_choose(const std::wstring &file); // the person's own choice; empty: accepted
// Setup thread; takes a second the first time. Empty: ready.
std::string nr_open(ID3D11Device *dev, IDXGIAdapter1 *adapter, int w, int h, int lines, int style, int layers); // layers 1-5: the network looks at its own picture again
void nr_close();
bool nr_ready(int w, int h);
// Capture thread, on the frame device: `full` (a copy of the original) becomes the picture in `target`. motion: back to the picture before, in
// its own pixels (may be null). False: nothing was drawn.
// How much of the change is used: everything, its fine part, its broad part (light), and how much of the original colour stays. 1: as it comes.
// blend: how much of the last layer over the one before (1: all of it). skin: how much structure the network gives people (0-1, below 0 its own
// choice). smooth: skin keeps its own texture and loses a little of it, so that faces do not look older.
struct NrLook { float power, detail, tone, keep, blend, skin, smooth, steady; bool fast; }; // steady: how much of the pictures before (0-1)
// key: 0-4, where this picture's change is kept for nr_between.
bool nr_run(ID3D11DeviceContext4 *c, ID3D11ShaderResourceView *full, ID3D11ShaderResourceView *motion, int mw, int mh, ID3D11ShaderResourceView *hud, ID3D11RenderTargetView *target, bool reset, const NrLook &look, int key);
// fast: what the network said about the picture before is used, moved along the motion, and nothing waits for it; it works on this one in
// nr_think(), called when the pictures are on their way (a picture that begins a scene is waited for).
void nr_think(ID3D11DeviceContext4 *c);
// The same for a picture made between the originals a and b (f: 0 a, 1 b).
bool nr_between(ID3D11DeviceContext *c, ID3D11ShaderResourceView *made, ID3D11RenderTargetView *target, int a, int b, float f, bool hud, const NrLook &look);
double nr_ms();                                  // graphics card time of one picture, 0: none yet
