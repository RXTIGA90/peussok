// SPDX-License-Identifier: GPL-3.0-only
// Benchmark: feeds a moving test picture at 60 pictures a second through the real path (copy, convert, generate, HUD protection,
// show) for every model and size, and turns the measured times into what this computer can do.
#pragma once
#include "engine.h"

enum { BENCH_SIZES = 3, BENCH_MODELS = 8 };
struct BenchCell
{
    int state = 0;       // 0 not measured, 1 measured, 2 not possible (reason)
    float ready = 0;     // ms, arrival to converted
    float gen = 0, gen95 = 0;   // ms, one made picture
    float made = 0, made95 = 0; // ms, arrival to finished made picture
    int flow = 0;        // RIFE motion size used: 1 half, 2 quarter, 3 eighth
    int missed = 0, frames = 0;
    // derived
    int mult60 = 0, mult30 = 0, mult120 = 0; // largest multiplier for 60, 30 and 120 fps games; 1 = cannot
    float max_fps = 0;   // made pictures a second
    float latency = 0;   // ms, arrival to screen of the original at 60 fps 2x and 25%
};
struct BenchResult
{
    bool valid = false, full = false, game = false; // game: with pictures of a game
    std::wstring when, gpu, driver, cpu, ram;
    int model_id[BENCH_MODELS]{}, models = 0, monitor_hz = 0, monitor_h = 0;
    std::wstring reason[BENCH_MODELS]; // why a model cannot be used at all
    BenchCell cell[BENCH_MODELS][BENCH_SIZES];
    int score = 0, best_model = -1, best_size = -1, best_mult = 0, fast_model = -1;
};
extern const int bench_w[BENCH_SIZES], bench_h[BENCH_SIZES];
extern BenchResult bench_result;         // the last one (also after a restart)
extern volatile LONG bench_running, bench_done, bench_steps, bench_model_now, bench_size_now; // progress
void bench_start(bool full);             // the capture must be closed
void bench_cancel();
void bench_finish();                     // window thread, when bench_running fell back to 0: joins the thread, restores the settings
void bench_load();
void bench_fake(int w, int h);          // tests: endless test pictures instead of a capture card
std::wstring bench_text(const BenchResult &r);               // the result as text
const wchar_t *bench_size_name(int i);
int bench_recommended_size(const BenchResult &r, int model); // largest size with 2x at 60 fps, -1 none
