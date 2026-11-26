// SPDX-License-Identifier: GPL-3.0-only
// Extrapolation: the original is shown the moment it arrives, and the pictures after it are guessed from the motion between the last
// two originals. Nothing has to wait for the next original, so generation adds no latency; what cannot be known (things that come into
// view) is where it looks worse than interpolation.
//
// Motion: hierarchical block matching on the brightness at about 360 lines (5 levels, 4x4 blocks, 8x8 windows, quarter-pixel), vector
// median per level, then the best of the neighbouring vectors for every pixel, calmed twice. All compute shaders, any Direct3D 11 graphics card.
#pragma once
#include <d3d11_4.h>

bool extra_init(ID3D11Device *d);
bool extra_size(ID3D11Device *d, ID3D11DeviceContext *c, int w, int h, int lines = 540); // lines: size the motion is searched at
void extra_forget();                                             // the next picture has no past (cut, new input)
void extra_feed(ID3D11DeviceContext *c, ID3D11ShaderResourceView *newest, bool both = false, bool quick = false); // both: also from the picture before to this one; quick: only the motion, nothing that judges it
// Repair of an interpolated picture: where the motion of both originals agrees on what must be there and the model shows something
// else (a head in front of a turning background), the originals moved along that motion take its place.
void extra_repair(ID3D11DeviceContext *c, ID3D11ShaderResourceView *older, ID3D11ShaderResourceView *newest, ID3D11ShaderResourceView *made);
void extra_repair_views(ID3D11ShaderResourceView **motion, ID3D11ShaderResourceView **weight); // motion in full-size pixels; weight and which original
bool extra_both_ready();
// motion from the picture before to this one
bool extra_prepared(); // extra_init() is done (it takes 0.7 s)
bool extra_ready();
struct ExtraProtect { ID3D11ShaderResourceView *hud, *bits, *block; unsigned hud_on, text_on, step, cw, ch, dw, dh; };
// a picture `t` intervals after the newest (0 < t < 1)
void extra_draw(ID3D11DeviceContext *c, ID3D11RenderTargetView *target, ID3D11ShaderResourceView *older, ID3D11ShaderResourceView *newest, float t, float past, const ExtraProtect &p);
ID3D11ShaderResourceView *extra_motion(int *w, int *h); // motion of every place of the newest picture back to the one before, in its own pixels
