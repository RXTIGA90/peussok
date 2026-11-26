// SPDX-License-Identifier: GPL-3.0-only
#include "bench.h"
#include <memory>
#include "sys.h"
#include "video.h"
#include <algorithm>
#include <avrt.h>
#include <cmath>

const int bench_w[BENCH_SIZES] = {1920, 2560, 3840}, bench_h[BENCH_SIZES] = {1080, 1440, 2160};
BenchResult bench_result;
volatile LONG bench_running, bench_done, bench_steps, bench_model_now = -1, bench_size_now;
static volatile LONG cancel;
static HANDLE thread;
static bool full_run;
static struct { LONG model, mult, flow, on; } kept;

const wchar_t *bench_size_name(int i) { return i == 0 ? L"1080p" : i == 1 ? L"1440p" : L"4K"; }

// ---- the test picture ----
// A textured background that pans, discs that move against it, and a fixed HUD. 16 pictures that repeat without a seam.
enum { LOOP = 16 };
static std::vector<uint8_t> picture[LOOP];
static int loop_n = LOOP; // pictures there are
static bool from_game;
// Pictures of a game instead, when the person has put them there: bench\game.nv12 beside the program or in the data folder, one 3840x2160
// NV12 picture after the other. They are run forwards and back, so that there is no jump.
static FILE *game_file()
{
    for (const std::wstring &d : {exe_dir(), data_dir()})
        if (FILE *f = _wfopen((d + L"\\bench\\game.nv12").c_str(), L"rb")) return f;
    return nullptr;
}
static bool game_pictures(int w, int h)
{
    const int gw = 3840, gh = 2160;
    FILE *f = game_file();
    if (!f) return false;
    std::vector<uint8_t> big((size_t)gw * gh * 3 / 2);
    int n = 0;
    for (; n < LOOP && fread(big.data(), 1, big.size(), f) == big.size(); n++)
    {
        auto &p = picture[n];
        p.resize((size_t)w * h * 3 / 2);
        if (w == gw && h == gh) { p = big; continue; }
        auto shrink = [&](const uint8_t *from, int fw, int fh, uint8_t *to, int tw, int th, int step) { // mean of what each pixel covers
            for (int y = 0; y < th; y++)
            {
                int y0 = y * fh / th, y1 = std::max(y0 + 1, (y + 1) * fh / th);
                for (int x = 0; x < tw; x++)
                {
                    int x0 = x * fw / tw, x1 = std::max(x0 + 1, (x + 1) * fw / tw);
                    for (int k = 0; k < step; k++)
                    {
                        int sum = 0;
                        for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) sum += from[((size_t)j * fw + i) * step + k];
                        to[((size_t)y * tw + x) * step + k] = (uint8_t)(sum / ((y1 - y0) * (x1 - x0)));
                    }
                }
            }
        };
        shrink(big.data(), gw, gh, p.data(), w, h, 1);
        shrink(&big[(size_t)gw * gh], gw / 2, gh / 2, &p[(size_t)w * h], w / 2, h / 2, 2);
    }
    fclose(f);
    if (n < 4) return false;
    loop_n = n;
    return true;
}
static void draw_pictures(int w, int h)
{
    from_game = game_pictures(w, h);
    if (from_game) return;
    loop_n = LOOP;
    const int pan = w / 160 * 2, period = pan * LOOP; // texture period in x
    std::vector<uint8_t> tile((size_t)period * h);
    unsigned seed = 12345;
    std::vector<float> grid;
    auto noise = [&](int cell, float gain) { // smooth noise, periodic in x
        int gw = std::max(1, period / cell), gh = h / cell + 2;
        grid.resize((size_t)gw * gh);
        for (auto &g : grid) { seed = seed * 1664525u + 1013904223u; g = (float)(seed >> 24) / 255.f - 0.5f; }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < period; x++)
            {
                float fx = (float)x * (float)gw / (float)period, fy = (float)y / (float)cell;
                int ix = (int)fx, iy = (int)fy;
                float ax = fx - (float)ix, ay = fy - (float)iy;
                float a = grid[(size_t)iy * gw + ix % gw], b = grid[(size_t)iy * gw + (ix + 1) % gw], c = grid[(size_t)(iy + 1) * gw + ix % gw], d = grid[(size_t)(iy + 1) * gw + (ix + 1) % gw];
                float v = (a + (b - a) * ax) * (1 - ay) + (c + (d - c) * ax) * ay;
                uint8_t &t = tile[(size_t)y * period + x];
                t = (uint8_t)std::min(225.f, std::max(20.f, (float)t + v * gain));
            }
    };
    std::fill(tile.begin(), tile.end(), (uint8_t)105);
    noise(h / 9, 120);
    noise(h / 36, 60);
    noise(h / 180 + 1, 26);
    for (int i = 0; i < LOOP; i++)
    {
        auto &p = picture[i];
        p.resize((size_t)w * h * 3 / 2);
        int shift = pan * i;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w;)
            {
                int tx = (x + shift) % period, n = std::min(period - tx, w - x);
                memcpy(&p[(size_t)y * w + x], &tile[(size_t)y * period + tx], (size_t)n);
                x += n;
            }
        uint8_t *uv = &p[(size_t)w * h];
        for (int y = 0; y < h / 2; y++)
            for (int x = 0; x < w / 2; x++)
            {
                uv[(size_t)y * w + x * 2] = (uint8_t)(128 + 18 * y / (h / 2));
                uv[(size_t)y * w + x * 2 + 1] = (uint8_t)(128 - 14 * x / (w / 2));
            }
        // discs on a circle path, one turn per loop
        for (int k = 0; k < 5; k++)
        {
            double a = 6.2831853 * ((double)i / LOOP + k / 5.0), rr = h * 0.06;
            int cx = (int)(w * (0.15 + 0.175 * k) + cos(a) * h * 0.03), cy = (int)(h * 0.52 + sin(a) * h * 0.04);
            for (int y = std::max(0, (int)(cy - rr)); y < std::min(h, (int)(cy + rr)); y++)
            {
                double dy = y - cy, s = rr * rr - dy * dy;
                if (s <= 0) continue;
                int half = (int)sqrt(s), x0 = std::max(0, cx - half), x1 = std::min(w, cx + half);
                memset(&p[(size_t)y * w + x0], 60 + 35 * k, (size_t)(x1 - x0));
            }
        }
        // a HUD that never moves
        for (int y = h / 24; y < h / 24 + h / 40; y++) memset(&p[(size_t)y * w + w / 24], 210, (size_t)(w / 6));
        for (int y = h - h / 10; y < h - h / 14; y++) memset(&p[(size_t)y * w + w - w / 5], 40, (size_t)(w / 6));
    }
}

// ---- measuring ----
static float pick(std::vector<float> v, double q)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, (size_t)((double)v.size() * q))];
}
// Feeds pictures for a while; returns false when cancelled.
static int feed_at, feed_step = 1;
static LONGLONG feed_next;
static void feed_one(const VideoFormat &f)
{
    LONGLONG step = ticks_of(1000.0 / 60), now = qpc();
    if (feed_next < now - 3 * step || feed_next > now + 3 * step) feed_next = now;
    wait_until(feed_next);
    feed_next += step;
    if (feed_at >= loop_n) feed_at = 0;
    auto &p = picture[feed_at];
    if (from_game) // forwards and back
    {
        if (feed_at + feed_step < 0 || feed_at + feed_step >= loop_n) feed_step = -feed_step;
        feed_at += feed_step;
    }
    else feed_at = (feed_at + 1) % LOOP;
    video_on_picture(p.data(), p.size(), f, qpc());
}
static bool measure(const VideoFormat &f, int frames, BenchCell &c)
{
    for (int i = 0; i < 24 && !cancel; i++) feed_one(f); // clocks and averages settle
    video_bench(true);
    LONG64 made = num.made_count, sources = num.sources;
    for (int i = 0; i < frames && !cancel; i++) feed_one(f);
    Sleep(20);
    std::vector<float> ready, gen, total;
    video_bench_read(ready, gen, total);
    video_bench(false);
    c.frames = (int)(num.sources - sources);
    c.missed = std::max(0, c.frames - (int)(num.made_count - made));
    c.ready = pick(ready, 0.5);
    c.gen = pick(gen, 0.5);
    c.gen95 = pick(gen, 0.95);
    c.made = pick(total, 0.5);
    c.made95 = pick(total, 0.95);
    c.state = gen.size() > 5 ? 1 : 2;
    return !cancel;
}
static void derive(const ModelInfo &m, BenchCell &c)
{
    if (c.state != 1) return;
    auto mult = [&](double source) { // all made pictures of one original must be ready before the next original arrives
        double room = 0.85 * 1000.0 / source - (c.made95 - c.gen95);
        int n = c.gen95 > 0 ? (int)(room / c.gen95) : 0;
        return std::min(1 + std::max(0, n), m.multi ? 4 : 2);
    };
    c.mult60 = mult(60);
    c.mult30 = mult(30);
    c.mult120 = mult(120);
    c.max_fps = c.gen > 0 ? 1000.f / c.gen : 0;
    c.latency = m.family == FAMILY_EXTRA ? c.ready + 0.3f : c.made + 0.1f + 0.25f * 16.667f;
}
int bench_recommended_size(const BenchResult &r, int model)
{
    for (int i = BENCH_SIZES - 1; i >= 0; i--)
        if (r.cell[model][i].state == 1 && r.cell[model][i].mult60 >= 2) return i;
    return -1;
}
static void conclude(BenchResult &r)
{
    // Score: how many pictures a second FSR 3 and RIFE 4.6 (they run on every maker's card) can make, over the sizes measured.
    double log_sum = 0;
    int n = 0;
    for (int m = 0; m < r.models; m++)
        if (r.model_id[m] == 5 || r.model_id[m] == 0)
            for (int s = 0; s < BENCH_SIZES; s++)
                if (r.cell[m][s].state == 1 && r.cell[m][s].max_fps > 0)
                {
                    float f = r.cell[m][s].max_fps / (r.cell[m][s].flow == 2 ? 2.f : r.cell[m][s].flow == 3 ? 3.f : 1.f); // a smaller motion size is easier
                    log_sum += log(f * (s == 2 ? 4.0 : s == 1 ? 1.78 : 1.0));
                    n++;
                }
    r.score = n ? (int)(exp(log_sum / n) * 2 + 0.5) : 0;
    // Recommended: the best picture that makes 2x at the largest size, with at most 15 ms; fastest: the shortest latency there.
    int top = r.monitor_h >= 2100 ? 2 : r.monitor_h >= 1400 ? 1 : 0;
    r.best_model = r.fast_model = -1;
    for (int s = top; s >= 0 && r.best_model < 0; s--)
        for (int m = 0; m < r.models; m++)
        {
            const BenchCell &c = r.cell[m][s];
            if (c.state != 1 || c.mult60 < 2 || c.missed * 10 > c.frames) continue;
            const ModelInfo *i = model_by_id(r.model_id[m]);
            if (c.latency <= 15 && (r.best_model < 0 || i->quality > model_by_id(r.model_id[r.best_model])->quality)) { r.best_model = m; r.best_size = s; }
            if (i->family != FAMILY_EXTRA && (r.fast_model < 0 || c.latency < r.cell[r.fast_model][s].latency)) r.fast_model = m;
        }
    if (r.best_model >= 0)
    {
        int cap = r.monitor_hz > 0 ? std::max(2, r.monitor_hz / 60) : 4;
        r.best_mult = std::min(r.cell[r.best_model][r.best_size].mult60, cap);
    }
}

static std::unique_ptr<BenchResult> finished;
static DWORD WINAPI bench_main(void *)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD k = 0;
    HANDLE av = AvSetMmThreadCharacteristicsW(L"Games", &k);
    BenchResult r;
    r.full = full_run;
    const SysInfo &si = sys_info();
    r.gpu = si.gpu;
    r.driver = si.driver;
    r.cpu = si.cpu;
    r.ram = si.ram;
    r.monitor_hz = (int)num.hz_mhz / 1000;
    r.monitor_h = (int)num.monitor_h;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t w[64];
    swprintf(w, 64, L"%04d-%02d-%02d %02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    r.when = w;
    r.models = model_count;
    for (int m = 0; m < model_count; m++)
    {
        r.model_id[m] = models[m].id;
        if (models[m].nvidia && !nvidia_rtx()) r.reason[m] = tr(L"NVIDIA RTX 전용", L"NVIDIA RTX only");
        else if (models[m].id == 9 && !vfg_installed()) r.reason[m] = tr(L"NVIDIA 파일 필요", L"Needs NVIDIA files");
    }
    tune.on = 1;
    tune.mult = 2;
    int frames = full_run ? 180 : 75;
    for (int s = 0; s < BENCH_SIZES && !cancel; s++)
    {
        if (!full_run && s == 1) { InterlockedAdd(&bench_done, model_count); continue; } // quick: 1080p and 4K
        InterlockedExchange(&bench_size_now, s);
        InterlockedExchange(&bench_model_now, -1);
        draw_pictures(bench_w[s], bench_h[s]);
        r.game = from_game;
        VideoFormat f;
        f.pixel = Pixel::nv12;
        f.w = bench_w[s];
        f.h = bench_h[s];
        f.fps = 60;
        for (int m = 0; m < model_count && !cancel; m++)
        {
            InterlockedExchange(&bench_model_now, m);
            BenchCell &c = r.cell[m][s];
            if (!r.reason[m].empty()) { c.state = 2; InterlockedIncrement(&bench_done); continue; }
            bool rife = models[m].family == FAMILY_RIFE;
            int first = models[m].id == 0 && f.h > 1300 ? 2 : 1; // what the program uses by itself
            tune.flow = rife ? first : 0;
            tune.model = models[m].id;
            ULONGLONG begin = GetTickCount64();
            bool ok = false;
            while (!cancel && GetTickCount64() - begin < 25000)
            {
                feed_one(f);
                if (GetTickCount64() - begin < 150) continue;
                if (num.engine_state == 2 && num.engine_model == models[m].id && num.engine_w == f.w && num.engine_h == f.h) { ok = true; break; }
                if (num.engine_state == -1 && GetTickCount64() - begin > 1500) break;
            }
            if (!ok)
            {
                c.state = 2;
                std::string why = video_engine_error();
                logf("bench: %ls %dx%d not possible: %s", models[m].name, f.w, f.h, why.c_str());
            }
            else
                for (int level = first;; level++)
                {
                    if (rife) { tune.flow = level; c.flow = level; for (int i = 0; i < 180 && !cancel && (i < 12 || num.flow != level); i++) feed_one(f); }
                    if (!measure(f, frames, c)) break;
                    if (!rife || level >= (f.h > 1300 ? 3 : 2) || (c.made95 < 0.85 * 1000.0 / 60 && c.missed * 10 < c.frames)) break;
                }
            derive(models[m], c);
            logf("bench: %ls %dx%d state=%d ready=%.2f gen=%.2f/%.2f made=%.2f/%.2f missed=%d/%d flow=%d x60=%d x30=%d x120=%d", models[m].name, f.w, f.h, c.state, c.ready, c.gen, c.gen95, c.made, c.made95, c.missed, c.frames, c.flow, c.mult60, c.mult30, c.mult120);
            bench_result.cell[m][s] = c; // shown while it runs
            InterlockedIncrement(&bench_done);
        }
    }
    for (auto &p : picture) std::vector<uint8_t>().swap(p);
    if (!cancel)
    {
        conclude(r);
        r.valid = true;
        finished = std::make_unique<BenchResult>(r); // the window takes it over when this thread has ended
        FILE *out = _wfopen((data_dir() + L"\\benchmark.txt").c_str(), L"wb");
        if (out)
        {
            std::wstring text = bench_text(r);
            int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
            std::string u((size_t)std::max(1, n), 0);
            WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &u[0], n, nullptr, nullptr);
            fputs("\xEF\xBB\xBF", out);
            fputs(u.c_str(), out);
            fclose(out);
        }
        out = _wfopen((data_dir() + L"\\benchmark.bin").c_str(), L"wb");
        if (out)
        {
            int head[8] = {2, r.models, (int)r.full | (int)r.game << 1, r.score, r.best_model, r.best_size, r.best_mult, r.fast_model};
            fwrite(head, sizeof head, 1, out);
            fwrite(r.model_id, sizeof r.model_id, 1, out);
            int mon[2] = {r.monitor_hz, r.monitor_h};
            fwrite(mon, sizeof mon, 1, out);
            fwrite(r.cell, sizeof r.cell, 1, out);
            auto put = [&](const std::wstring &s) { int n = (int)s.size(); fwrite(&n, sizeof n, 1, out); fwrite(s.data(), sizeof(wchar_t), s.size(), out); };
            put(r.when);
            put(r.gpu);
            put(r.driver);
            put(r.cpu);
            put(r.ram);
            for (auto &x : r.reason) put(x);
            fclose(out);
        }
    }
    if (av) AvRevertMmThreadCharacteristics(av);
    tune.model = kept.model;
    tune.mult = kept.mult;
    tune.flow = kept.flow;
    tune.on = kept.on;
    InterlockedExchange(&bench_running, 0);
    return 0;
}
void bench_load()
{
    FILE *in = _wfopen((data_dir() + L"\\benchmark.bin").c_str(), L"rb");
    if (!in) return;
    BenchResult r;
    int head[8] = {}, mon[2] = {};
    bool ok = fread(head, sizeof head, 1, in) == 1 && head[0] == 2 && head[1] == model_count && fread(r.model_id, sizeof r.model_id, 1, in) == 1 && fread(mon, sizeof mon, 1, in) == 1 && fread(r.cell, sizeof r.cell, 1, in) == 1;
    auto take = [&](std::wstring &s) {
        int n = 0;
        ok = ok && fread(&n, sizeof n, 1, in) == 1 && n >= 0 && n < 500;
        if (!ok) return;
        s.resize((size_t)n);
        if (n) ok = fread(&s[0], sizeof(wchar_t), (size_t)n, in) == (size_t)n;
    };
    take(r.when);
    take(r.gpu);
    take(r.driver);
    take(r.cpu);
    take(r.ram);
    for (auto &x : r.reason) take(x);
    fclose(in);
    if (!ok) return;
    r.models = head[1];
    r.full = head[2] & 1;
    r.game = head[2] >> 1 & 1;
    r.score = head[3];
    r.best_model = head[4];
    r.best_size = head[5];
    r.best_mult = head[6];
    r.fast_model = head[7];
    r.monitor_hz = mon[0];
    r.monitor_h = mon[1];
    bool sane = r.best_model < r.models && r.fast_model < r.models && r.best_size < BENCH_SIZES;
    for (int m = 0; m < model_count; m++) sane = sane && r.model_id[m] == models[m].id;
    if (!sane) return;
    r.valid = true;
    bench_result = r;
}
static int fake_w, fake_h;
static DWORD WINAPI fake_main(void *)
{
    draw_pictures(fake_w, fake_h);
    VideoFormat f;
    f.pixel = Pixel::nv12;
    f.w = fake_w;
    f.h = fake_h;
    f.fps = 60;
    for (;;) feed_one(f);
}
void bench_fake(int w, int h)
{
    fake_w = w;
    fake_h = h;
    CreateThread(nullptr, 1 << 20, fake_main, nullptr, 0, nullptr);
}
void bench_start(bool full)
{
    if (bench_running) return;
    kept = {tune.model, tune.mult, tune.flow, tune.on};
    full_run = full;
    cancel = 0;
    bench_result = BenchResult();
    bench_result.models = model_count;
    for (int m = 0; m < model_count; m++) bench_result.model_id[m] = models[m].id;
    bench_done = 0;
    bench_steps = model_count * BENCH_SIZES;
    bench_model_now = -1;
    bench_size_now = 0;
    InterlockedExchange(&bench_running, 1);
    thread = CreateThread(nullptr, 1 << 20, bench_main, nullptr, 0, nullptr);
    if (!thread) bench_running = 0;
}
void bench_cancel() { InterlockedExchange(&cancel, 1); }
void bench_finish()
{
    if (!thread) return;
    WaitForSingleObject(thread, 30000);
    CloseHandle(thread);
    thread = nullptr;
    if (finished) { bench_result = *finished; finished.reset(); }
    if (!bench_result.valid) bench_load(); // cancelled: the earlier result again
}
std::wstring bench_text(const BenchResult &r)
{
    std::wstring o;
    wchar_t t[256];
    auto add = [&](const wchar_t *f, auto... a) { swprintf(t, 256, f, a...); o += t; };
    add(L"%ls %ls  %ls  %ls\r\n", app_name(), APP_VERSION, tr(L"벤치마크", L"benchmark"), r.when.c_str());
    add(L"%ls\r\n", r.game ? tr(L"테스트 영상  게임 녹화", L"Test picture  game recording") : tr(L"테스트 영상  합성 이미지", L"Test picture  drawn"));
    add(L"GPU  %ls (%ls)\r\nCPU  %ls\r\nRAM  %ls\r\n%ls  %d\r\n\r\n", r.gpu.c_str(), r.driver.c_str(), r.cpu.c_str(), r.ram.c_str(), tr(L"점수", L"Score"), r.score);
    for (int s = 0; s < BENCH_SIZES; s++)
    {
        bool any = false;
        for (int m = 0; m < r.models; m++) any = any || r.cell[m][s].state;
        if (!any) continue;
        add(L"[%ls]\r\n", bench_size_name(s));
        for (int m = 0; m < r.models; m++)
        {
            const BenchCell &c = r.cell[m][s];
            if (c.state == 1)
                add(tr(L"%-16ls 생성 시간 %.2f ms (95%% %.2f) · %.0f fps · 60fps %d배 · 30fps %d배 · 120fps %d배 · 예상 지연 %.1f ms · 실패 %d/%d%ls\r\n",
                       L"%-16ls made in %.2f ms (95%% %.2f) · %.0f a second · 60 fps %dx · 30 fps %dx · 120 fps %dx · latency %.1f ms · missed %d/%d%ls\r\n"),
                    model_by_id(r.model_id[m])->name, (double)c.gen, (double)c.gen95, (double)c.max_fps, c.mult60, c.mult30, c.mult120, (double)c.latency, c.missed, c.frames, c.flow == 2 ? L" · 1/4" : c.flow == 3 ? L" · 1/8" : L"");
            else
                add(L"%-16ls %ls\r\n", model_by_id(r.model_id[m])->name, r.reason[m].empty() ? tr(L"안 됨", L"Not possible") : r.reason[m].c_str());
        }
        o += L"\r\n";
    }
    if (r.best_model >= 0)
        add(tr(L"추천  %ls · %ls · %d배\r\n", L"Recommended  %ls · %ls · %dx\r\n"), model_by_id(r.model_id[r.best_model])->name, bench_size_name(r.best_size), r.best_mult);
    if (r.fast_model >= 0) add(tr(L"지연 최소  %ls\r\n", L"Lowest latency  %ls\r\n"), model_by_id(r.model_id[r.fast_model])->name);
    return o;
}
