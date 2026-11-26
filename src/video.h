// SPDX-License-Identifier: GPL-3.0-only
// Pictures: upload and colour conversion, frame generation, timing, and the window's presenter.
//
//   capture thread   on_picture(): skip repeats, copy into the GPU, convert into a shared input texture, hand a job to the worker
//   worker thread    wait for the conversion, generate, HUD protection, post "made picture" and "original" with their due times
//   present thread   own D3D11 device and swap chain; shows each picture when it is due
//   setup thread     makes and frees engines (loading a model takes 0.1-2 s)
#pragma once
#include "base.h"
#include "capture.h"
#include "engine.h"
#include <dxgi1_6.h>

// Everything a user can change while the picture runs.
struct Tuning
{
    volatile LONG on = 0;          // generation on
    volatile LONG model = 9;       // ModelInfo::id
    volatile LONG mult = 2;        // 2-4
    volatile LONG share = 25;      // % of an original interval a made picture stays on screen (20-50)
    volatile LONG margin_x10 = 1;  // ms x10 before a made picture is shown
    volatile LONG flow = 0;        // RIFE motion size: 0 automatic, 1 half, 2 quarter, 3 eighth
    volatile LONG sharpen = 0;     // 0 off, 1-100
    volatile LONG sharpen_made = 0;
    volatile LONG cut = 40;        // scene cut threshold on the luma difference, 0 off
    volatile LONG hud = 1;         // HUD protection
    volatile LONG repair = 0;      // trial: motion check of made pictures (faces)
    volatile LONG early = 0;       // start before the last lines are there: 0 no, 1 up to 20%, 2 up to 32% of the picture is guessed
    volatile LONG nr = 0, nr_style = 0, nr_power = 100, nr_size = 1, nr_layers = 2, nr_detail = 100, nr_tone = 100, nr_keep = 0, nr_skin = 50, nr_steady = 90, nr_fast = 1, nr_order = 0; // trial: DLSS 5; style 0 its own, 1 natural, 2 cinematic; strength %; size 0 720, 1 1080 lines, 2 as the picture
    volatile LONG hdr_in = 0;        // P010 read as HDR10 (PQ, BT.2020) (1). 0: as an ordinary picture: the GC573 already turns a console's HDR into SDR
    volatile LONG hdr = 0, hdr_white = 200, hdr_boost = 50; // HDR output; brightness of ordinary white in nits, how far highlights go (% of the way to the monitor's peak)
    volatile LONG text = 1;        // text protection: works from the third picture a text is on screen
    volatile LONG queue = 2;       // pictures that may wait for the monitor (V-Sync), 1-3
    volatile LONG sync = 1;        // 0 V-Sync, 1 variable refresh, 2 off
    volatile LONG range = 0;       // 0 automatic (full for MJPEG, else limited), 1 full, 2 limited
    volatile LONG fit = 0;         // 0 keep the aspect ratio, 1 stretch
    volatile LONG osd = 1;         // 0 off, 1 simple, 2 detailed
    volatile LONG corner = 0;      // 0 top left, 1 top right, 2 bottom left, 3 bottom right
};
extern Tuning tune;

struct Numbers // for the window; all read without locks
{
    Smooth ahead, early, copy, ready, generate, hud, made, share, latency, interval; // ms
    volatile LONG flow = 0, w = 0, h = 0, engine_model = -1, engine_state = 0, engine_w = 0, engine_h = 0, too_slow = 0, monitor_h = 0, view_w = 0, view_h = 0, lat_peak = 0; // lat_peak: us, largest since the reader set it to 0
    // state 0 none, 1 loading, 2 ready, -1 failed
    volatile LONG64 captured = 0, sources = 0, made_count = 0, skipped = 0, dropped = 0, late = 0, shown = 0, cuts = 0, last_arrival = 0;
    volatile LONG hz_mhz = 0, tearing = 0, composed = -1, vrr = -1, hdr = 0, hdr_peak = 0, hdr_out = 0; // composed: 1 Windows composes the window (about 10 ms more), 0 straight to the screen
    volatile LONG ring[256]{}, ring_next = 0; // how long each picture stayed on screen in us, bit 30 = made picture
};
extern Numbers num;
enum { RING_MADE = 1 << 30 };

bool video_start(HWND window);
void video_stop();
void video_on_picture(const uint8_t *data, size_t size, const VideoFormat &f, LONGLONG arrival); // capture thread
void video_buffer(uint8_t *data, size_t size, const VideoFormat &f); // capture thread: the driver fills this buffer again
void video_buffers_gone();
void video_resized();          // window thread
void video_no_signal();        // forget the last pictures (input changed)
std::string video_engine_error();
std::string video_nr_error(); // empty: ready or off
IDXGIAdapter1 *video_adapter();
extern void (*video_hook)(int made, int older, int newer); // tests: called after every made picture
extern bool video_even;                                    // tests: even times
extern int video_tune;                                     // tests: trial switches
bool video_lost();             // the graphics device is gone
void video_test_lost();        // tests: make it so
double video_stuck_seconds();  // how long the worker has been inside one picture
void video_engine_restart();
void video_bench(bool on);     // collect the times of each made picture
void video_bench_read(std::vector<float> &ready, std::vector<float> &gen, std::vector<float> &made);
