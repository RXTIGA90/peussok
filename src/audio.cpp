// SPDX-License-Identifier: GPL-3.0-only
#include "audio.h"
#include <algorithm>
#include <audioclient.h>
#include <avrt.h>
#include <cmath>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>

volatile LONG audio_volume = 100, audio_mute = 0, audio_delay = 0; // audio_delay: ms added, to meet the picture (Sound tab)

// Ring of stereo float samples. The capture card and the sound card have their own clocks, so the renderer reads slightly faster
// or slower (at most 0.5%) to keep the waiting sound near the target.
enum { RING = 1 << 16 };
static float ring[RING][2];
static volatile LONG64 wpos, rpos_int;
static volatile LONG in_rate = 48000;
static volatile LONG stop_flag, format_seen;
static HANDLE thread;
static const double base_ms = 18;
volatile LONG64 audio_gaps;

void audio_push(const uint8_t *d, size_t size, const AudioFormat &f)
{
    if (f.channels < 1 || (f.bits != 16 && f.bits != 24 && f.bits != 32)) return;
    if (!format_seen) { logf("audio: %d Hz %d ch %d bit", f.rate, f.channels, f.bits); format_seen = 1; }
    in_rate = f.rate;
    int bytes = f.bits / 8, frame = bytes * f.channels;
    size_t n = size / (size_t)frame;
    LONG64 w = wpos;
    for (size_t i = 0; i < n; i++)
    {
        float v[2];
        for (int c = 0; c < 2; c++)
        {
            const uint8_t *p = d + i * frame + (size_t)std::min(c, f.channels - 1) * bytes;
            if (f.bits == 16) v[c] = (float)*(const int16_t *)p / 32768.f;
            else if (f.bits == 24) v[c] = (float)((int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) / 2147483648.0);
            else v[c] = (float)(*(const int32_t *)p / 2147483648.0);
        }
        ring[(w + i) & (RING - 1)][0] = v[0];
        ring[(w + i) & (RING - 1)][1] = v[1];
    }
    MemoryBarrier();
    InterlockedExchange64(&wpos, w + (LONG64)n);
}
double audio_buffer_ms() { return 1000.0 * (double)(wpos - rpos_int) / (double)in_rate; }

static bool render_once()
{
    IMMDeviceEnumerator *en = nullptr;
    IMMDevice *dev = nullptr;
    IAudioClient3 *c3 = nullptr;
    IAudioRenderClient *rc = nullptr;
    WAVEFORMATEX *mix = nullptr;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::wstring id;
    bool again = true;
    do
    {
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) break;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) break;
        LPWSTR s = nullptr;
        if (SUCCEEDED(dev->GetId(&s))) { id = s; CoTaskMemFree(s); }
        if (FAILED(dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, (void **)&c3))) break;
        if (FAILED(c3->GetMixFormat(&mix))) break;
        UINT32 def = 0, unit = 0, low = 0, high = 0, size = 0;
        HRESULT hr = c3->GetSharedModeEnginePeriod(mix, &def, &unit, &low, &high);
        if (SUCCEEDED(hr)) hr = c3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, low, mix, nullptr);
        if (FAILED(hr)) { static int said; if (said++ < 5) logf("audio: open failed %08lx", (unsigned long)hr); break; }
        c3->SetEventHandle(ev);
        c3->GetBufferSize(&size);
        if (FAILED(c3->GetService(IID_PPV_ARGS(&rc)))) break;
        bool flt = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT || (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && reinterpret_cast<WAVEFORMATEXTENSIBLE *>(mix)->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT);
        logf("audio: playback %lu Hz %u ch period %u (%.1f ms) buffer %u %s", mix->nSamplesPerSec, mix->nChannels, low, 1000.0 * low / mix->nSamplesPerSec, size, flt ? "float" : "other");
        if (!flt) { again = false; break; }
        BYTE *p = nullptr;
        if (SUCCEEDED(rc->GetBuffer(size, &p))) rc->ReleaseBuffer(size, AUDCLNT_BUFFERFLAGS_SILENT);
        c3->Start();
        double pos = 0, fill = base_ms;
        bool running = false;
        ULONGLONG checked = GetTickCount64();
        while (!stop_flag)
        {
            if (WaitForSingleObject(ev, 500) != WAIT_OBJECT_0 && GetTickCount64() - checked < 3000) continue;
            if (GetTickCount64() - checked > 2000)
            {
                checked = GetTickCount64(); // follow a change of the default playback device
                IMMDevice *now = nullptr;
                LPWSTR n = nullptr;
                bool same = SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &now)) && SUCCEEDED(now->GetId(&n)) && id == n;
                if (n) CoTaskMemFree(n);
                release(now);
                if (!same) break;
            }
            UINT32 pad = 0;
            if (FAILED(c3->GetCurrentPadding(&pad))) break;
            UINT32 want = pad < 2 * low ? std::min(2 * low, size) - pad : 0; // keep two periods in the device
            if (!want || FAILED(rc->GetBuffer(want, &p))) continue;
            float *out = reinterpret_cast<float *>(p);
            LONG64 w = wpos;
            static LONG delay_now = -1;
            double target_ms = base_ms + (double)std::min(300L, std::max(0L, (LONG)audio_delay));
            if (delay_now != audio_delay) { delay_now = audio_delay; running = false; } // a new delay: start again from it, not by drifting there
            double have = 1000.0 * ((double)w - pos) / in_rate;
            if (!running && have >= target_ms) { running = true; pos = (double)w - target_ms * in_rate / 1000.0; fill = target_ms; }
            if (running && (have < 1 || have > target_ms + 200)) audio_gaps = audio_gaps + 1;
            if (running && (have < 1 || have > target_ms + 200)) running = have > target_ms + 200 ? (pos = (double)w - target_ms * in_rate / 1000.0, true) : false;
            fill = 0.98 * fill + 0.02 * have;
            double adj = std::min(0.005, std::max(-0.005, (fill - target_ms) * 0.0004)), step = (double)in_rate / mix->nSamplesPerSec * (1.0 + adj);
            float gain = audio_mute ? 0.f : (float)audio_volume / 100.f;
            for (UINT32 i = 0; i < want; i++)
            {
                float l = 0, r = 0;
                if (running && pos + 1 < (double)w)
                {
                    LONG64 k = (LONG64)pos;
                    float t = (float)(pos - (double)k), *a = ring[k & (RING - 1)], *b = ring[(k + 1) & (RING - 1)];
                    l = (a[0] + (b[0] - a[0]) * t) * gain;
                    r = (a[1] + (b[1] - a[1]) * t) * gain;
                    pos += step;
                }
                for (int ch = 0; ch < mix->nChannels; ch++) *out++ = ch == 0 ? l : ch == 1 ? r : 0.f;
                if (mix->nChannels == 1) out[-1] = (l + r) * 0.5f;
            }
            rc->ReleaseBuffer(want, 0);
            InterlockedExchange64(&rpos_int, (LONG64)pos);
        }
        c3->Stop();
    } while (false);
    if (mix) CoTaskMemFree(mix);
    release(rc);
    release(c3);
    release(dev);
    release(en);
    CloseHandle(ev);
    return again;
}
static DWORD WINAPI audio_main(void *)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD k = 0;
    AvSetMmThreadCharacteristicsW(L"Pro Audio", &k);
    while (!stop_flag)
    {
        if (!render_once()) break;
        if (!stop_flag) Sleep(300);
    }
    return 0;
}
void audio_start() { if (!thread) { stop_flag = 0; thread = CreateThread(nullptr, 0, audio_main, nullptr, 0, nullptr); } }
void audio_stop()
{
    stop_flag = 1;
    if (thread) { WaitForSingleObject(thread, 3000); CloseHandle(thread); thread = nullptr; }
}
