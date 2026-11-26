// SPDX-License-Identifier: GPL-3.0-only
// Tests: recorded NV12 pictures arrive the way a capture card delivers them (line by line, 60 Hz).
// PKAPP_REPLAY = file, PKAPP_REPLAY_MODE = "3840x2160 30 120" (size, pictures per second, how many to load)
#include "app.h"
#include "mjpeg.h"
#include <vector>
#include <algorithm>
#include <cmath>

static std::vector<std::vector<uint8_t>> pictures;
static CaptureSink *sink;
static VideoFormat form;
static double fps = 30;
static volatile LONG stop;
static Pixel kind = Pixel::nv12; // PKAPP_REPLAY_PIXEL: the recording made into another kind first (yuy2 uyvy rgb32 i420 yv12), as other cards send
static std::vector<uint8_t> made_as(const std::vector<uint8_t> &p, int w, int h)
{
    const uint8_t *Y = p.data(), *C = p.data() + (size_t)w * h;
    std::vector<uint8_t> o;
    if (kind == Pixel::yuy2 || kind == Pixel::uyvy)
    {
        o.resize((size_t)w * h * 2);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x += 2)
            {
                uint8_t *d = &o[((size_t)y * w + x) * 2];
                const uint8_t *c = C + (size_t)(y / 2) * w + x;
                if (kind == Pixel::yuy2) { d[0] = Y[(size_t)y * w + x]; d[1] = c[0]; d[2] = Y[(size_t)y * w + x + 1]; d[3] = c[1]; }
                else { d[0] = c[0]; d[1] = Y[(size_t)y * w + x]; d[2] = c[1]; d[3] = Y[(size_t)y * w + x + 1]; }
            }
    }
    else if (kind == Pixel::bgra)
    {
        o.resize((size_t)w * h * 4);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                float l = (Y[(size_t)y * w + x] - 16) / 219.f, u = (C[(size_t)(y / 2) * w + (x & ~1)] - 128) / 224.f, v = (C[(size_t)(y / 2) * w + (x & ~1) + 1] - 128) / 224.f;
                float r = l + 1.5748f * v, g = l - 0.1873f * u - 0.4681f * v, b = l + 1.8556f * u;
                auto q = [](float a) { return (uint8_t)std::min(255.f, std::max(0.f, a * 255.f + 0.5f)); };
                uint8_t *d = &o[((size_t)y * w + x) * 4];
                d[0] = q(b); d[1] = q(g); d[2] = q(r); d[3] = 255;
            }
    }
    else if (kind == Pixel::i420 || kind == Pixel::yv12)
    {
        size_t n = (size_t)w * h, q = n / 4;
        o.resize(n + 2 * q);
        memcpy(o.data(), Y, n);
        for (size_t i = 0; i < q; i++) { o[n + (kind == Pixel::yv12 ? q : 0) + i] = C[2 * i]; o[n + (kind == Pixel::yv12 ? 0 : q) + i] = C[2 * i + 1]; }
    }
    return o;
}
static DWORD WINAPI feed(void *)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    size_t line = (size_t)form.w, rows = (size_t)form.h * 3 / 2, at = 0;
    std::vector<uint8_t> buffer[4];
    for (auto &b : buffer) b.resize(line * rows);
    LONGLONG step = ticks_of(1001.0 / 60), begin = qpc();
    int repeat = std::max(1, (int)lround(60.0 / std::max(1.0, fps))), forward = 1; // 60: every scan, 30: twice, 15: four times
    for (LONGLONG n = 0; !stop; n++)
    {
        auto &to = buffer[n & 3];
        const auto &from = pictures[at];
        LONGLONG start = begin + n * step;
        const int parts = 64;
        for (int k = 0; k < parts; k++)
        {
            wait_until(start + ticks_of(16.0 * (k + 1) / parts));
            if (form.pixel == Pixel::mjpg || kind != Pixel::nv12) continue;
            int y0 = form.h * k / parts, y1 = form.h * (k + 1) / parts;
            memcpy(&to[line * (size_t)y0], &from[line * (size_t)y0], line * (size_t)(y1 - y0));
            size_t c0 = (size_t)(form.h + y0 / 2), c1 = (size_t)(form.h + y1 / 2);
            memcpy(&to[line * c0], &from[line * c0], line * (c1 - c0));
        }
        if (kind != Pixel::nv12)
        {
            static std::vector<uint8_t> woven;
            VideoFormat f = form;
            f.pixel = kind;
            f.full = kind == Pixel::bgra;
            if (kind == Pixel::i420 || kind == Pixel::yv12)
            {
                f.pixel = Pixel::nv12;
                if (planar_to_nv12(from.data(), from.size(), form.w, form.h, kind == Pixel::yv12, woven)) sink->on_video(woven.data(), woven.size(), f, qpc(), 0);
            }
            else
            {
                sink->on_video(from.data(), from.size(), f, qpc(), 0);
                sink->on_buffer(const_cast<uint8_t *>(from.data()), from.size(), f);
            }
        }
        else if (form.pixel == Pixel::mjpg)
        {
            static Mjpeg m;
            VideoFormat f = form;
            f.pixel = Pixel::nv12;
            f.full = true;
            const uint8_t *d = nullptr;
            size_t size = 0;
            LONGLONG at_all = qpc();
            if (m.decode(from.data(), from.size(), form.w, form.h, video_adapter(), d, size)) sink->on_video(d, size, f, at_all, 0);
        }
        else
        {
            sink->on_video(to.data(), to.size(), form, qpc(), 0);
            sink->on_buffer(to.data(), to.size(), form);
        }
        if (n % repeat == repeat - 1) // forwards, then backwards: no jump
        {
            if (pictures.size() > 1 && ((forward > 0 && at + 1 == pictures.size()) || (forward < 0 && at == 0))) forward = -forward;
            at = (size_t)((long long)at + (pictures.size() > 1 ? forward : 0));
        }
        if (qpc() > begin + (n + 3) * step) begin = qpc() - (n + 1) * step;
    }
    sink->on_stop(); // the buffers go
    return 0;
}
bool replay_start(CaptureSink *s)
{
    wchar_t file[MAX_PATH], mode[64] = L"3840x2160 30 120";
    if (!GetEnvironmentVariableW(L"PKAPP_REPLAY", file, MAX_PATH)) return false;
    GetEnvironmentVariableW(L"PKAPP_REPLAY_MODE", mode, 64);
    int count = 120, first = 0;
    form.pixel = Pixel::nv12;
    swscanf(mode, L"%dx%d %lf %d %d", &form.w, &form.h, &fps, &count, &first);
    form.fps = 60;
    FILE *f = _wfopen(file, L"rb");
    if (!f) return false;
    size_t size = (size_t)form.w * form.h * 3 / 2;
    _fseeki64(f, (long long)size * first, SEEK_SET);
    if (wcsstr(file, L".mjpeg"))
    {
        form.pixel = Pixel::mjpg;
        std::vector<uint8_t> p;
        for (int c, was = 0; (int)pictures.size() < count && (c = fgetc(f)) != EOF; was = c)
        {
            p.push_back((uint8_t)c);
            if (was == 0xFF && c == 0xD9) { pictures.push_back(std::move(p)); p.clear(); }
        }
        count = 0;
    }
    for (int i = 0; i < count; i++)
    {
        std::vector<uint8_t> p(size);
        if (fread(p.data(), 1, size, f) != size) break;
        pictures.push_back(std::move(p));
    }
    fclose(f);
    wchar_t px[16] = L"";
    if (GetEnvironmentVariableW(L"PKAPP_REPLAY_PIXEL", px, 16) && form.pixel == Pixel::nv12)
    {
        kind = !_wcsicmp(px, L"yuy2") ? Pixel::yuy2 : !_wcsicmp(px, L"uyvy") ? Pixel::uyvy : !_wcsicmp(px, L"rgb32") ? Pixel::bgra : !_wcsicmp(px, L"i420") ? Pixel::i420 : !_wcsicmp(px, L"yv12") ? Pixel::yv12 : Pixel::nv12;
        for (auto &p : pictures) p = made_as(p, form.w, form.h);
        logf("replay: made into %hs", pixel_name(kind));
    }
    logf("replay: %d pictures", (int)pictures.size());
    if (pictures.empty()) return false;
    app.current = form;
    sink = s;
    CreateThread(nullptr, 0, feed, nullptr, 0, nullptr);
    return true;
}
void replay_stop() { InterlockedExchange(&stop, 1); Sleep(60); }
