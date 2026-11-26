// SPDX-License-Identifier: GPL-3.0-only
#include "rec.h"
#include "app.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <d3dcompiler.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlobj.h>

// ---- Media Foundation, loaded when it is first needed (Windows N editions have none: then recording is not offered) ----
static GUID fcc(DWORD f) { return {f, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}}; }
static const GUID sub_nv12 = fcc(0x3231564E), sub_h264 = fcc(0x34363248), sub_hevc = fcc(0x43564548), sub_pcm = fcc(1), sub_aac = fcc(0x1610);
static const GUID k_hw = {0xa634a91c, 0x822b, 0x41b9, {0xa4, 0x94, 0x4d, 0xe4, 0x64, 0x36, 0x12, 0xb0}};        // MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS
static const GUID k_nothrottle = {0x08b845d8, 0x2b74, 0x4afe, {0x9d, 0x53, 0xbe, 0x16, 0xd2, 0xd5, 0xae, 0x4f}}; // MF_SINK_WRITER_DISABLE_THROTTLING
static const GUID k_container = {0x150ff23f, 0x4abc, 0x478b, {0xac, 0x4f, 0xe1, 0x91, 0x6f, 0xba, 0x1c, 0xca}}; // MF_TRANSCODE_CONTAINERTYPE
static const GUID k_mp4 = {0xdc6cd05d, 0xb9d0, 0x40ef, {0xbd, 0x35, 0xfa, 0x62, 0x2c, 0x1a, 0xb2, 0x8a}};       // MFTranscodeContainerType_MPEG4
static struct
{
    bool tried = false, ok = false;
    HRESULT(WINAPI *startup)(ULONG, DWORD) = nullptr;
    HRESULT(WINAPI *media_type)(IMFMediaType **) = nullptr;
    HRESULT(WINAPI *attributes)(IMFAttributes **, UINT32) = nullptr;
    HRESULT(WINAPI *memory)(DWORD, IMFMediaBuffer **) = nullptr;
    HRESULT(WINAPI *sample)(IMFSample **) = nullptr;
    HRESULT(WINAPI *writer)(LPCWSTR, IMFByteStream *, IMFAttributes *, IMFSinkWriter **) = nullptr;
} mf;
static bool mf_load()
{
    if (mf.tried) return mf.ok;
    mf.tried = true;
    HMODULE p = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32), r = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto get = [](HMODULE m, auto &f, const char *n) { f = m ? reinterpret_cast<std::decay_t<decltype(f)>>(reinterpret_cast<void *>(GetProcAddress(m, n))) : nullptr; return f != nullptr; };
    mf.ok = get(p, mf.startup, "MFStartup") && get(p, mf.media_type, "MFCreateMediaType") && get(p, mf.attributes, "MFCreateAttributes") && get(p, mf.memory, "MFCreateMemoryBuffer") &&
            get(p, mf.sample, "MFCreateSample") && get(r, mf.writer, "MFCreateSinkWriterFromURL") && SUCCEEDED(mf.startup(MF_VERSION, MFSTARTUP_LITE));
    if (!mf.ok) logf("rec: Media Foundation is not there");
    return mf.ok;
}

// ---- state ----
struct Piece { std::vector<uint8_t> data; LONGLONG when; bool video; };
static std::mutex m;
static std::condition_variable wake;
static std::deque<Piece> queue;
static std::atomic<bool> want{false}, running{false};
static std::atomic<long long> pictures{0}, dropped{0};
static std::wstring file, error, folder_now;
static const wchar_t *codec = L"";
static LONGLONG began = 0;
static std::atomic<bool> paused{false};
static std::atomic<LONGLONG> paused_at{0}, paused_total{0}; // qpc ticks
static std::thread worker;
static int vw, vh;    // size the writer was made for
static double vfps;   // rate in its header
static AudioFormat af; // the sound it was made for (rate 0: none)
volatile LONG rec_pq = 0; // the picture is HDR10: recorded as an ordinary video
volatile LONG rec_codec_pref = 0, rec_quality = 1, rec_sound = 1, rec_made = 1;
static std::wstring folder_set;
void rec_set_folder(const std::wstring &f) { std::lock_guard<std::mutex> g(m); folder_set = f; }

std::wstring rec_folder()
{
    {
        std::lock_guard<std::mutex> g(m);
        if (!folder_set.empty() && (CreateDirectoryW(folder_set.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)) return folder_set;
    }
    wchar_t *p = nullptr;
    std::wstring f;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &p)) && p) f = std::wstring(p) + L"\\Peussok";
    CoTaskMemFree(p);
    if (f.empty()) f = data_dir() + L"\\videos";
    CreateDirectoryW(f.c_str(), nullptr);
    return f;
}
bool rec_on() { return want || running; }
std::wstring rec_file() { std::lock_guard<std::mutex> g(m); return file; }
std::wstring rec_error() { std::lock_guard<std::mutex> g(m); return error; }
const wchar_t *rec_codec() { return codec; }
long long rec_pictures() { return pictures; }
long long rec_dropped() { return dropped; }
double rec_seconds()
{
    if (!running || !began) return 0;
    LONGLONG now = qpc(), off = paused_total + (paused ? now - paused_at : 0);
    return std::max(0.0, ms_of(now - began - off) / 1000.0);
}
bool rec_paused() { return rec_on() && paused; }
void rec_pause(bool on)
{
    if (!rec_on() || on == paused) return;
    if (on) { paused_at = qpc(); paused = true; logf("rec: paused"); }
    else { paused_total += qpc() - paused_at; paused = false; logf("rec: going on"); }
}

static void writer_loop()
{
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMFSinkWriter *w = nullptr;
    DWORD vs = 0, as = 0;
    bool has_audio = false;
    LONGLONG t0 = 0, last_v = -1;
    long long audio_samples = 0;
    LONGLONG audio_start = -1;
    auto fail = [&](const wchar_t *why, HRESULT hr) {
        std::lock_guard<std::mutex> g(m);
        wchar_t t[160];
        swprintf(t, 160, L"%ls (%08lX)", why, (unsigned long)hr);
        error = t;
        logf("rec: %ls", t);
    };
    // the writer, for HEVC first and H.264 when the card has no HEVC encoder
    auto open = [&](bool hevc) -> HRESULT {
        IMFAttributes *a = nullptr;
        HRESULT hr = mf.attributes(&a, 3);
        if (SUCCEEDED(hr)) { a->SetUINT32(k_hw, TRUE); a->SetUINT32(k_nothrottle, TRUE); a->SetGUID(k_container, k_mp4); }
        if (SUCCEEDED(hr)) hr = mf.writer(file.c_str(), nullptr, a, &w);
        if (a) a->Release();
        IMFMediaType *out = nullptr, *in = nullptr;
        UINT32 rate = (UINT32)std::max(1.0, vfps * 1000 + 0.5);
        UINT32 mbps = vw * vh >= 3840 * 2000 ? (hevc ? 80 : 120) : vw * vh >= 2560 * 1400 ? (hevc ? 45 : 70) : (hevc ? 25 : 40);
        if (vfps > 75) mbps = mbps * 3 / 2;
        mbps = rec_quality == 0 ? mbps * 6 / 10 : rec_quality == 2 ? mbps * 16 / 10 : mbps;
        if (SUCCEEDED(hr)) hr = mf.media_type(&out);
        if (SUCCEEDED(hr))
        {
            out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            out->SetGUID(MF_MT_SUBTYPE, hevc ? sub_hevc : sub_h264);
            out->SetUINT32(MF_MT_AVG_BITRATE, mbps * 1000000);
            out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(out, MF_MT_FRAME_SIZE, (UINT32)vw, (UINT32)vh);
            MFSetAttributeRatio(out, MF_MT_FRAME_RATE, rate, 1000);
            MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            if (!hevc) out->SetUINT32(MF_MT_MPEG2_PROFILE, 100); // High
            hr = w->AddStream(out, &vs);
        }
        if (SUCCEEDED(hr)) hr = mf.media_type(&in);
        if (SUCCEEDED(hr))
        {
            in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            in->SetGUID(MF_MT_SUBTYPE, sub_nv12);
            in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
            MFSetAttributeSize(in, MF_MT_FRAME_SIZE, (UINT32)vw, (UINT32)vh);
            MFSetAttributeRatio(in, MF_MT_FRAME_RATE, rate, 1000);
            MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
            in->SetUINT32(MF_MT_DEFAULT_STRIDE, (UINT32)vw);
            hr = w->SetInputMediaType(vs, in, nullptr);
        }
        if (out) out->Release();
        if (in) in->Release();
        // the sound: AAC 192 kbit/s from the card's 16-bit PCM
        has_audio = false;
        if (SUCCEEDED(hr) && rec_sound && af.rate && af.bits == 16 && (af.channels == 1 || af.channels == 2) && (af.rate == 44100 || af.rate == 48000))
        {
            IMFMediaType *ao = nullptr, *ai = nullptr;
            HRESULT ha = mf.media_type(&ao);
            if (SUCCEEDED(ha))
            {
                ao->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                ao->SetGUID(MF_MT_SUBTYPE, sub_aac);
                ao->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                ao->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)af.rate);
                ao->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)af.channels);
                ao->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
                ha = w->AddStream(ao, &as);
            }
            if (SUCCEEDED(ha)) ha = mf.media_type(&ai);
            if (SUCCEEDED(ha))
            {
                ai->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                ai->SetGUID(MF_MT_SUBTYPE, sub_pcm);
                ai->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                ai->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)af.rate);
                ai->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)af.channels);
                ai->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32)af.channels * 2);
                ai->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(af.rate * af.channels * 2));
                ha = w->SetInputMediaType(as, ai, nullptr);
            }
            if (ao) ao->Release();
            if (ai) ai->Release();
            has_audio = SUCCEEDED(ha);
            if (!has_audio) logf("rec: no sound in the file (%08lx)", (unsigned long)ha);
        }
        if (SUCCEEDED(hr)) hr = w->BeginWriting();
        if (FAILED(hr) && w) { w->Release(); w = nullptr; DeleteFileW(file.c_str()); }
        return hr;
    };
    auto put = [&](DWORD stream, const std::vector<uint8_t> &d, LONGLONG time, LONGLONG length) {
        IMFMediaBuffer *b = nullptr;
        IMFSample *s = nullptr;
        BYTE *p = nullptr;
        HRESULT hr = mf.memory((DWORD)d.size(), &b);
        if (SUCCEEDED(hr)) hr = b->Lock(&p, nullptr, nullptr);
        if (SUCCEEDED(hr)) { memcpy(p, d.data(), d.size()); b->Unlock(); b->SetCurrentLength((DWORD)d.size()); hr = mf.sample(&s); }
        if (SUCCEEDED(hr)) { s->AddBuffer(b); s->SetSampleTime(time); s->SetSampleDuration(length); hr = w->WriteSample(stream, s); }
        if (s) s->Release();
        if (b) b->Release();
        return hr;
    };
    auto hns = [&](LONGLONG q) { return (LONGLONG)((double)(q - t0) * 1e7 / (double)qpc_freq); };
    for (;;)
    {
        Piece p;
        {
            std::unique_lock<std::mutex> g(m);
            wake.wait(g, [] { return !queue.empty() || !want; });
            if (queue.empty() && !want) break;
            p = std::move(queue.front());
            queue.pop_front();
        }
        if (p.video)
        {
            if (!w)
            {
                HRESULT hr = E_FAIL;
                if (rec_codec_pref != 2) { hr = open(true); codec = L"HEVC"; }
                if (FAILED(hr) && rec_codec_pref != 1) { hr = open(false); codec = L"H.264"; }
                if (FAILED(hr)) { codec = L""; fail(tr(L"녹화를 시작하지 못했어요", L"Recording could not start"), hr); want = false; continue; }
                t0 = p.when;
                began = qpc();
                running = true;
                logf("rec: %ls %dx%d %.2f fps%ls -> %ls", codec, vw, vh, vfps, has_audio ? L" + sound" : L"", file.c_str());
            }
            LONGLONG t = hns(p.when);
            if (t <= last_v) t = last_v + 1;
            last_v = t;
            if (FAILED(put(vs, p.data, t, (LONGLONG)(1e7 / std::max(1.0, vfps))))) dropped++;
            else pictures++;
        }
        else if (w && has_audio)
        {
            // continuous: the first piece fixes the start, the count of samples the rest (no gaps from arrival jitter)
            int frame = af.channels * 2;
            LONGLONG n = (LONGLONG)(p.data.size() / (size_t)frame);
            if (audio_start < 0)
            {
                LONGLONG at = hns(p.when) - (LONGLONG)(1e7 * (double)n / af.rate);
                if (at < 0) continue; // from before the first picture
                audio_start = at;
            }
            LONGLONG at = audio_start + (LONGLONG)(1e7 * (double)audio_samples / af.rate);
            put(as, p.data, at, (LONGLONG)(1e7 * (double)n / af.rate));
            audio_samples += n;
        }
    }
    if (w)
    {
        HRESULT hr = w->Finalize();
        if (FAILED(hr)) fail(tr(L"파일을 마무리하지 못했어요", L"The file could not be finished"), hr);
        w->Release();
        logf("rec: done %lld pictures, %lld left out, %.1f s", (long long)pictures, (long long)dropped, began ? ms_of(qpc() - began) / 1000.0 : 0.0);
    }
    running = false;
    began = 0;
    if (SUCCEEDED(co)) CoUninitialize();
}

std::wstring rec_start()
{
    if (rec_on()) return L"";
    if (!mf_load())
    {
        std::lock_guard<std::mutex> l(m);
        error = tr(L"이 Windows에는 녹화에 필요한 미디어 기능이 없어요 (N 버전이면 '미디어 기능 팩'을 설치하세요)", L"This Windows has no Media Foundation (on an N edition, install the Media Feature Pack)");
        return error;
    }
    if (worker.joinable()) worker.join();
    SYSTEMTIME s;
    GetLocalTime(&s);
    wchar_t name[64];
    swprintf(name, 64, L"\\Peussok-%04d%02d%02d-%02d%02d%02d.mp4", s.wYear, s.wMonth, s.wDay, s.wHour, s.wMinute, s.wSecond);
    std::wstring where = rec_folder(); // takes the lock itself
    {
        std::lock_guard<std::mutex> g(m);
        file = where + name;
        error.clear();
        queue.clear();
    }
    vw = vh = 0;
    pictures = dropped = 0;
    paused = false;
    paused_total = 0;
    af = app.audio;
    want = true;
    worker = std::thread(writer_loop);
    logf("rec: start");
    return L"";
}
void rec_stop()
{
    if (!want && !running) return;
    logf("rec: stop");
    want = false;
    wake.notify_all();
    if (worker.joinable()) worker.join();
}

// ---- the presenter's side: BGRA -> NV12 on the card, read back a few pictures later ----
static const char vs_src[] = "float4 main(uint i:SV_VertexID):SV_Position{return float4(i==2?3:-1,i==1?3:-1,0,1);}";
static const char pq_src[] =
    "float3 pq_nits(float3 e){float3 p=pow(saturate(e),1.0/78.84375);return 10000.0*pow(max(p-0.8359375,0)/(18.8515625-18.6875*p),1.0/0.1593017578125);}"
    "float3 sd(float3 e){\n#ifdef PQ\nfloat3 l=max(mul(float3x3(1.6605,-0.5876,-0.0728,-0.1246,1.1329,-0.0083,-0.0182,-0.1006,1.1187),pq_nits(e))/203.0,0);"
    "float m=max(l.r,max(l.g,l.b));float t=m<=0.8?m:0.8+0.2*(1-exp(-(m-0.8)/0.2));l*=m>0?t/m:0;return pow(saturate(l),1/2.2);\n#else\nreturn e;\n#endif\n}\n";
static const char ps_y_src[] = "Texture2D t:register(t0);float main(float4 p:SV_Position):SV_Target{float3 c=sd(t.Load(int3(p.xy,0)).rgb);"
                               "return (16.0+219.0*dot(c,float3(0.2126,0.7152,0.0722)))/255.0;}";
static const char ps_uv_src[] = "Texture2D t:register(t0);float2 main(float4 p:SV_Position):SV_Target{int2 q=int2(p.xy)*2;"
                                "float3 c=(sd(t.Load(int3(q,0)).rgb)+sd(t.Load(int3(q+int2(1,0),0)).rgb)+sd(t.Load(int3(q+int2(0,1),0)).rgb)+sd(t.Load(int3(q+int2(1,1),0)).rgb))*0.25;"
                                "return float2(128.0+224.0*dot(c,float3(-0.1146,-0.3854,0.5)),128.0+224.0*dot(c,float3(0.5,-0.4542,-0.0458)))/255.0;}";
enum { RING = 4 };
static struct
{
    ID3D11Device *dev = nullptr;
    int w = 0, h = 0;
    ID3D11VertexShader *vs = nullptr;
    ID3D11PixelShader *py = nullptr, *puv = nullptr, *py_pq = nullptr, *puv_pq = nullptr;
    ID3D11Texture2D *ty = nullptr, *tuv = nullptr, *sy[RING]{}, *suv[RING]{};
    ID3D11RenderTargetView *ry = nullptr, *ruv = nullptr;
    LONGLONG when[RING]{};
    bool busy[RING]{};
    int next = 0, oldest = 0;
} g;
template <class T> static void drop(T *&p) { if (p) { p->Release(); p = nullptr; } }
void rec_release()
{
    drop(g.vs); drop(g.py); drop(g.puv); drop(g.py_pq); drop(g.puv_pq); drop(g.ty); drop(g.tuv); drop(g.ry); drop(g.ruv);
    for (int i = 0; i < RING; i++) { drop(g.sy[i]); drop(g.suv[i]); g.busy[i] = false; }
    g.dev = nullptr;
    g.w = g.h = 0;
    g.next = g.oldest = 0;
}
static bool make(ID3D11Device *d, int w, int h)
{
    rec_release();
    g.dev = d;
    g.w = w;
    g.h = h;
    auto shader = [&](const char *src, const char *target, ID3DBlob **b) { ID3DBlob *e = nullptr; HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, b, &e); if (e) e->Release(); return SUCCEEDED(hr); };
    ID3DBlob *b = nullptr;
    bool ok = shader(vs_src, "vs_5_0", &b) && SUCCEEDED(d->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &g.vs));
    drop(b);
    for (int k = 0; k < 4; k++)
    {
        std::string src = std::string(k >= 2 ? "#define PQ 1\n" : "") + pq_src + (k & 1 ? ps_uv_src : ps_y_src);
        ID3D11PixelShader **to[4] = {&g.py, &g.puv, &g.py_pq, &g.puv_pq};
        ok = ok && shader(src.c_str(), "ps_5_0", &b) && SUCCEEDED(d->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, to[k]));
        drop(b);
    }
    D3D11_TEXTURE2D_DESC t{};
    t.MipLevels = t.ArraySize = 1;
    t.SampleDesc.Count = 1;
    auto plane = [&](int pw, int ph, DXGI_FORMAT f, ID3D11Texture2D **rt, ID3D11RenderTargetView **rv, ID3D11Texture2D **st) {
        t.Width = (UINT)pw; t.Height = (UINT)ph; t.Format = f;
        t.Usage = D3D11_USAGE_DEFAULT; t.BindFlags = D3D11_BIND_RENDER_TARGET; t.CPUAccessFlags = 0;
        bool r = SUCCEEDED(d->CreateTexture2D(&t, nullptr, rt)) && SUCCEEDED(d->CreateRenderTargetView(*rt, nullptr, rv));
        t.Usage = D3D11_USAGE_STAGING; t.BindFlags = 0; t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (int i = 0; i < RING && r; i++) r = SUCCEEDED(d->CreateTexture2D(&t, nullptr, &st[i]));
        return r;
    };
    ok = ok && plane(w, h, DXGI_FORMAT_R8_UNORM, &g.ty, &g.ry, g.sy) && plane(w / 2, h / 2, DXGI_FORMAT_R8G8_UNORM, &g.tuv, &g.ruv, g.suv);
    if (!ok) { logf("rec: textures failed %dx%d", w, h); rec_release(); }
    return ok;
}
// Pictures that are ready on the card go to the writer; with wait, all of them.
static void collect(ID3D11DeviceContext *c, bool wait)
{
    while (g.busy[g.oldest])
    {
        int i = g.oldest;
        D3D11_MAPPED_SUBRESOURCE a{}, b{};
        UINT flag = wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT;
        if (FAILED(c->Map(g.sy[i], 0, D3D11_MAP_READ, flag, &a))) return;
        if (FAILED(c->Map(g.suv[i], 0, D3D11_MAP_READ, flag, &b))) { c->Unmap(g.sy[i], 0); return; }
        Piece p;
        p.video = true;
        p.when = g.when[i];
        size_t W = (size_t)g.w, H = (size_t)g.h;
        p.data.resize(W * H * 3 / 2);
        for (size_t y = 0; y < H; y++) memcpy(&p.data[y * W], (uint8_t *)a.pData + y * a.RowPitch, W);
        for (size_t y = 0; y < H / 2; y++) memcpy(&p.data[W * H + y * W], (uint8_t *)b.pData + y * b.RowPitch, W);
        c->Unmap(g.sy[i], 0);
        c->Unmap(g.suv[i], 0);
        g.busy[i] = false;
        g.oldest = (i + 1) % RING;
        {
            std::lock_guard<std::mutex> l(m);
            if (queue.size() > 12) dropped++; // the encoder is behind: leave the picture out
            else queue.push_back(std::move(p));
        }
        wake.notify_one();
    }
}
void rec_frame(ID3D11Device *d, ID3D11DeviceContext *c, ID3D11ShaderResourceView *picture, int w, int h, double fps, LONGLONG when, bool made)
{
    if (!want)
    {
        if (g.dev) rec_release();
        return;
    }
    if (made && !rec_made) return; // the originals only
    if (paused) return;
    w &= ~1;
    h &= ~1;
    if (w < 64 || h < 64 || !picture) return;
    if (!vw) { vw = w; vh = h; vfps = fps > 1 ? fps : 60; } // the writer's size: fixed for the file
    if (w != vw || h != vh)
    {
        {
            std::lock_guard<std::mutex> l(m);
            error = tr(L"화면 크기가 바뀌어서 녹화를 멈췄어요", L"Recording stopped: the picture size changed");
        }
        want = false;
        wake.notify_all();
        return;
    }
    if (g.dev != d || g.w != w || g.h != h)
        if (!make(d, w, h)) { logf("rec: no copies on the card"); want = false; wake.notify_all(); return; }
    collect(c, false);
    int i = g.next;
    if (g.busy[i]) { dropped++; return; } // the card has not finished the older ones
    D3D11_VIEWPORT v1 = {0, 0, (float)w, (float)h, 0, 1}, v2 = {0, 0, (float)(w / 2), (float)(h / 2), 0, 1};
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(g.vs, nullptr, 0);
    c->PSSetShaderResources(0, 1, &picture);
    c->OMSetRenderTargets(1, &g.ry, nullptr);
    c->RSSetViewports(1, &v1);
    c->PSSetShader(rec_pq ? g.py_pq : g.py, nullptr, 0);
    c->Draw(3, 0);
    c->OMSetRenderTargets(1, &g.ruv, nullptr);
    c->RSSetViewports(1, &v2);
    c->PSSetShader(rec_pq ? g.puv_pq : g.puv, nullptr, 0);
    c->Draw(3, 0);
    ID3D11RenderTargetView *none = nullptr;
    ID3D11ShaderResourceView *nos = nullptr;
    c->OMSetRenderTargets(1, &none, nullptr);
    c->PSSetShaderResources(0, 1, &nos);
    c->CopyResource(g.sy[i], g.ty);
    c->CopyResource(g.suv[i], g.tuv);
    g.when[i] = when - paused_total;
    g.busy[i] = true;
    g.next = (i + 1) % RING;
}
void rec_audio(const uint8_t *data, size_t size, const AudioFormat &f, LONGLONG when)
{
    if (!running || paused || !size || f.rate != af.rate || f.channels != af.channels || f.bits != af.bits) return;
    Piece p;
    p.video = false;
    p.when = when - paused_total;
    p.data.assign(data, data + size);
    {
        std::lock_guard<std::mutex> l(m);
        if (queue.size() > 400) return;
        queue.push_back(std::move(p));
    }
    wake.notify_one();
}
