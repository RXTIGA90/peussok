// SPDX-License-Identifier: GPL-3.0-only
#include "capture.h"
#include "mjpeg.h"
#include "video.h"
#include <dvdmedia.h>
#include <algorithm>
#include <atomic>

static const GUID sub_nv12 = {0x3231564E, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID sub_p010 = {0x30313050, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID sub_yuy2 = {0x32595559, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID sub_mjpg = {0x47504A4D, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID sub_bgr24 = {0xe436eb7d, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};
// the kinds other makers' cards send (Elgato, Magewell, Blackmagic, simple USB cards)
static GUID fourcc(DWORD f) { return {f, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}}; }
static const GUID sub_uyvy = fourcc(0x59565955), sub_hdyc = fourcc(0x43594448), sub_i420 = fourcc(0x30323449), sub_iyuv = fourcc(0x56555949), sub_yv12 = fourcc(0x32315659);
static const GUID sub_rgb32 = {0xe436eb7e, 0x524f, 0x11ce, {0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70}};
static const GUID sub_argb32 = {0x773c9ac0, 0x3274, 0x11d0, {0xb7, 0x24, 0x00, 0xaa, 0x00, 0x6c, 0x1a, 0x01}};

const char *pixel_name(Pixel p)
{
    switch (p)
    {
    case Pixel::nv12: return "NV12";
    case Pixel::p010: return "P010";
    case Pixel::yuy2: return "YUY2";
    case Pixel::bgr24: return "RGB24";
    case Pixel::mjpg: return "MJPG";
    case Pixel::uyvy: return "UYVY";
    case Pixel::bgra: return "RGB32";
    case Pixel::i420: return "I420";
    case Pixel::yv12: return "YV12";
    default: return "?";
    }
}
static Pixel pixel_of(const GUID &g)
{
    if (g == sub_nv12) return Pixel::nv12;
    if (g == sub_p010) return Pixel::p010;
    if (g == sub_yuy2) return Pixel::yuy2;
    if (g == sub_bgr24) return Pixel::bgr24;
    if (g == sub_mjpg) return Pixel::mjpg;
    if (g == sub_uyvy || g == sub_hdyc) return Pixel::uyvy;
    if (g == sub_rgb32 || g == sub_argb32) return Pixel::bgra;
    if (g == sub_i420 || g == sub_iyuv) return Pixel::i420;
    if (g == sub_yv12) return Pixel::yv12;
    return Pixel::none;
}
// I420 / YV12 (three planes) to NV12 (two): the brightness as it is, the two colour planes woven together.
bool planar_to_nv12(const uint8_t *d, size_t size, int w, int h, bool v_first, std::vector<uint8_t> &out)
{
    size_t y = (size_t)w * h, q = (size_t)(w / 2) * (h / 2);
    if (w < 2 || h < 2 || size < y + 2 * q) return false;
    out.resize(y + 2 * q);
    memcpy(out.data(), d, y);
    const uint8_t *u = d + y + (v_first ? q : 0), *v = d + y + (v_first ? 0 : q);
    uint8_t *o = out.data() + y;
    for (size_t i = 0; i < q; i++) { o[2 * i] = u[i]; o[2 * i + 1] = v[i]; }
    return true;
}
static bool parse(const AM_MEDIA_TYPE &mt, VideoFormat &f)
{
    f = VideoFormat();
    f.pixel = pixel_of(mt.subtype);
    if (f.pixel == Pixel::none || !mt.pbFormat) return false;
    const BITMAPINFOHEADER *b = nullptr;
    REFERENCE_TIME t = 0;
    if (mt.formattype == FORMAT_VideoInfo && mt.cbFormat >= sizeof(VIDEOINFOHEADER))
    {
        auto *v = reinterpret_cast<VIDEOINFOHEADER *>(mt.pbFormat);
        b = &v->bmiHeader;
        t = v->AvgTimePerFrame;
    }
    else if (mt.formattype == FORMAT_VideoInfo2 && mt.cbFormat >= sizeof(VIDEOINFOHEADER2))
    {
        auto *v = reinterpret_cast<VIDEOINFOHEADER2 *>(mt.pbFormat);
        b = &v->bmiHeader;
        t = v->AvgTimePerFrame;
    }
    if (!b) return false;
    f.w = b->biWidth;
    f.h = abs(b->biHeight);
    f.fps = t > 0 ? 1e7 / (double)t : 0;
    f.bottom_up = (f.pixel == Pixel::bgr24 || f.pixel == Pixel::bgra) && b->biHeight > 0;
    f.full = f.pixel == Pixel::bgr24 || f.pixel == Pixel::bgra;
    return f.w > 0 && f.h > 0;
}
static void free_type(AM_MEDIA_TYPE *mt)
{
    if (!mt) return;
    if (mt->cbFormat) CoTaskMemFree(mt->pbFormat);
    if (mt->pUnk) mt->pUnk->Release();
    CoTaskMemFree(mt);
}

// ---- sink filter with one input pin ----
class SinkFilter final : public IBaseFilter, public IPin, public IMemInputPin
{
public:
    SinkFilter(bool video, CaptureSink *s) : is_video(video), out(s) {}

    // IUnknown (filter and pin are one object)
    STDMETHODIMP QueryInterface(REFIID id, void **p) override
    {
        if (!p) return E_POINTER;
        *p = nullptr;
        if (id == IID_IUnknown || id == IID_IBaseFilter || id == IID_IMediaFilter || id == IID_IPersist) *p = static_cast<IBaseFilter *>(this);
        else if (id == IID_IPin) *p = static_cast<IPin *>(this);
        else if (id == IID_IMemInputPin) *p = static_cast<IMemInputPin *>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG n = --refs;
        if (!n) delete this;
        return n;
    }

    // IBaseFilter
    STDMETHODIMP GetClassID(CLSID *id) override { *id = GUID{0x6c2d7d4a, 0x9a1f, 0x4c55, {0x8e, 0x0b, 0x51, 0x3e, 0x2a, 0x77, 0x10, 0x0c}}; return S_OK; }
    STDMETHODIMP Stop() override
    {
        AcquireSRWLockExclusive(&gate);
        state = State_Stopped;
        if (is_video && out) out->on_stop();
        ReleaseSRWLockExclusive(&gate);
        return S_OK;
    }
    STDMETHODIMP Pause() override { state = State_Paused; return S_OK; }
    STDMETHODIMP Run(REFERENCE_TIME start) override { run_start = start; state = State_Running; return S_OK; }
    STDMETHODIMP GetState(DWORD, FILTER_STATE *s) override { *s = state; return S_OK; }
    STDMETHODIMP SetSyncSource(IReferenceClock *c) override
    {
        if (c) c->AddRef();
        if (clock) clock->Release();
        clock = c;
        return S_OK;
    }
    STDMETHODIMP GetSyncSource(IReferenceClock **c) override
    {
        *c = clock;
        if (clock) clock->AddRef();
        return S_OK;
    }
    STDMETHODIMP EnumPins(IEnumPins **e) override;
    STDMETHODIMP FindPin(LPCWSTR, IPin **p) override { *p = static_cast<IPin *>(this); AddRef(); return S_OK; }
    STDMETHODIMP QueryFilterInfo(FILTER_INFO *i) override
    {
        wcscpy(i->achName, L"Peussok sink");
        i->pGraph = graph;
        if (graph) graph->AddRef();
        return S_OK;
    }
    STDMETHODIMP JoinFilterGraph(IFilterGraph *g, LPCWSTR) override { graph = g; return S_OK; } // not counted, as the rules say
    STDMETHODIMP QueryVendorInfo(LPWSTR *) override { return E_NOTIMPL; }

    // IPin
    STDMETHODIMP Connect(IPin *, const AM_MEDIA_TYPE *) override { return E_UNEXPECTED; }
    STDMETHODIMP ReceiveConnection(IPin *from, const AM_MEDIA_TYPE *mt) override
    {
        if (peer) return VFW_E_ALREADY_CONNECTED;
        if (QueryAccept(mt) != S_OK) return VFW_E_TYPE_NOT_ACCEPTED;
        take(mt);
        peer = from;
        peer->AddRef();
        return S_OK;
    }
    STDMETHODIMP Disconnect() override { if (!peer) return S_FALSE; release(peer); return S_OK; }
    STDMETHODIMP ConnectedTo(IPin **p) override
    {
        if (!peer) { *p = nullptr; return VFW_E_NOT_CONNECTED; }
        *p = peer;
        peer->AddRef();
        return S_OK;
    }
    STDMETHODIMP ConnectionMediaType(AM_MEDIA_TYPE *mt) override
    {
        if (!peer) return VFW_E_NOT_CONNECTED;
        *mt = type;
        if (type.cbFormat)
        {
            mt->pbFormat = (BYTE *)CoTaskMemAlloc(type.cbFormat);
            memcpy(mt->pbFormat, type.pbFormat, type.cbFormat);
        }
        return S_OK;
    }
    STDMETHODIMP QueryPinInfo(PIN_INFO *i) override { i->pFilter = this; AddRef(); i->dir = PINDIR_INPUT; wcscpy(i->achName, L"In"); return S_OK; }
    STDMETHODIMP QueryDirection(PIN_DIRECTION *d) override { *d = PINDIR_INPUT; return S_OK; }
    STDMETHODIMP QueryId(LPWSTR *id) override
    {
        *id = (LPWSTR)CoTaskMemAlloc(8);
        if (!*id) return E_OUTOFMEMORY;
        wcscpy(*id, L"In");
        return S_OK;
    }
    STDMETHODIMP QueryAccept(const AM_MEDIA_TYPE *mt) override
    {
        if (is_video)
        {
            VideoFormat f;
            return mt->majortype == MEDIATYPE_Video && parse(*mt, f) ? S_OK : S_FALSE;
        }
        return mt->majortype == MEDIATYPE_Audio && mt->subtype == MEDIASUBTYPE_PCM && mt->formattype == FORMAT_WaveFormatEx ? S_OK : S_FALSE;
    }
    STDMETHODIMP EnumMediaTypes(IEnumMediaTypes **) override { return E_NOTIMPL; }
    STDMETHODIMP QueryInternalConnections(IPin **, ULONG *) override { return E_NOTIMPL; }
    STDMETHODIMP EndOfStream() override { return S_OK; }
    STDMETHODIMP BeginFlush() override { return S_OK; }
    STDMETHODIMP EndFlush() override { return S_OK; }
    STDMETHODIMP NewSegment(REFERENCE_TIME, REFERENCE_TIME, double) override { return S_OK; }

    // IMemInputPin
    STDMETHODIMP GetAllocator(IMemAllocator **) override { return VFW_E_NO_ALLOCATOR; }
    STDMETHODIMP NotifyAllocator(IMemAllocator *, BOOL) override { return S_OK; }
    STDMETHODIMP GetAllocatorRequirements(ALLOCATOR_PROPERTIES *) override { return E_NOTIMPL; }
    STDMETHODIMP Receive(IMediaSample *s) override
    {
        LONGLONG arrival = qpc();
        CaptureSink *o = out; // may be taken away meanwhile; the object stays
        if (state == State_Stopped || !o) return S_FALSE;
        AM_MEDIA_TYPE *changed = nullptr;
        if (s->GetMediaType(&changed) == S_OK && changed) { take(changed); free_type(changed); }
        BYTE *data = nullptr;
        long size = s->GetActualDataLength();
        if (FAILED(s->GetPointer(&data)) || size <= 0) return S_OK;
        if (is_video)
        {
            double device = -1;
            REFERENCE_TIME a = 0, b = 0, now = 0;
            if (clock && SUCCEEDED(s->GetTime(&a, &b)) && SUCCEEDED(clock->GetTime(&now))) device = (double)(now - run_start - a) / 10000.0;
            if (vf.pixel == Pixel::mjpg)
            {
                VideoFormat f = vf;
                f.pixel = Pixel::nv12;
                f.full = true;
                const uint8_t *picture = nullptr;
                size_t n = 0;
                if (mjpeg.decode(data, (size_t)size, vf.w, vf.h, video_adapter(), picture, n)) o->on_video(picture, n, f, arrival, device);
                return S_OK; // the driver's buffer has nothing to be read early
            }
            if (vf.pixel == Pixel::i420 || vf.pixel == Pixel::yv12)
            {
                VideoFormat f = vf;
                f.pixel = Pixel::nv12;
                if (planar_to_nv12(data, (size_t)size, vf.w, vf.h, vf.pixel == Pixel::yv12, woven)) o->on_video(woven.data(), woven.size(), f, arrival, device);
                return S_OK;
            }
            o->on_video(data, (size_t)size, vf, arrival, device);
            AcquireSRWLockExclusive(&gate);
            if (state != State_Stopped && out) out->on_buffer(data, (size_t)size, vf);
            ReleaseSRWLockExclusive(&gate);
        }
        else o->on_audio(data, (size_t)size, af);
        return S_OK;
    }
    STDMETHODIMP ReceiveMultiple(IMediaSample **s, long n, long *done) override
    {
        for (*done = 0; *done < n; ++*done) Receive(s[*done]);
        return S_OK;
    }
    STDMETHODIMP ReceiveCanBlock() override { return S_FALSE; }

    void detach()
    {
        AcquireSRWLockExclusive(&gate);
        if (is_video && out) out->on_stop();
        out = nullptr;
        ReleaseSRWLockExclusive(&gate);
    }
    VideoFormat vf;
    AudioFormat af;
    Mjpeg mjpeg;
    std::vector<uint8_t> woven; // I420 / YV12 made into NV12

private:
    ~SinkFilter()
    {
        release(peer);
        if (clock) clock->Release();
        if (type.cbFormat) CoTaskMemFree(type.pbFormat);
    }
    void take(const AM_MEDIA_TYPE *mt)
    {
        if (type.cbFormat) CoTaskMemFree(type.pbFormat);
        type = *mt;
        type.pUnk = nullptr;
        if (mt->cbFormat)
        {
            type.pbFormat = (BYTE *)CoTaskMemAlloc(mt->cbFormat);
            memcpy(type.pbFormat, mt->pbFormat, mt->cbFormat);
        }
        if (is_video) parse(type, vf);
        else if (type.cbFormat >= sizeof(WAVEFORMATEX) - 2)
        {
            auto *w = reinterpret_cast<WAVEFORMATEX *>(type.pbFormat);
            af = {(int)w->nSamplesPerSec, (int)w->nChannels, (int)w->wBitsPerSample};
        }
    }
    std::atomic<ULONG> refs{1};
    SRWLOCK gate = SRWLOCK_INIT;
    bool is_video;
    CaptureSink *volatile out;
    volatile FILTER_STATE state = State_Stopped;
    REFERENCE_TIME run_start = 0;
    IReferenceClock *clock = nullptr;
    IFilterGraph *graph = nullptr;
    IPin *peer = nullptr;
    AM_MEDIA_TYPE type{};
};
class OnePin final : public IEnumPins
{
public:
    OnePin(IPin *p, int at) : pin(p), pos(at) { pin->AddRef(); }
    STDMETHODIMP QueryInterface(REFIID id, void **p) override
    {
        if (id != IID_IUnknown && id != IID_IEnumPins) { *p = nullptr; return E_NOINTERFACE; }
        *p = this;
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++refs; }
    STDMETHODIMP_(ULONG) Release() override { ULONG n = --refs; if (!n) delete this; return n; }
    STDMETHODIMP Next(ULONG n, IPin **p, ULONG *got) override
    {
        ULONG k = 0;
        if (n && pos == 0) { *p = pin; pin->AddRef(); pos = 1; k = 1; }
        if (got) *got = k;
        return k == n ? S_OK : S_FALSE;
    }
    STDMETHODIMP Skip(ULONG n) override { pos += (int)n; return pos <= 1 ? S_OK : S_FALSE; }
    STDMETHODIMP Reset() override { pos = 0; return S_OK; }
    STDMETHODIMP Clone(IEnumPins **e) override { *e = new OnePin(pin, pos); return S_OK; }

private:
    ~OnePin() { pin->Release(); }
    std::atomic<ULONG> refs{1};
    IPin *pin;
    int pos;
};
STDMETHODIMP SinkFilter::EnumPins(IEnumPins **e) { *e = new OnePin(static_cast<IPin *>(this), 0); return S_OK; }

// ---- devices ----
static std::wstring prop(IPropertyBag *bag, const wchar_t *name)
{
    VARIANT v;
    VariantInit(&v);
    std::wstring s;
    if (SUCCEEDED(bag->Read(name, &v, nullptr)) && v.vt == VT_BSTR) s = v.bstrVal;
    VariantClear(&v);
    return s;
}
template <class F> static void each_device(F f, const GUID &category = CLSID_VideoInputDeviceCategory)
{
    ICreateDevEnum *de = nullptr;
    IEnumMoniker *em = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&de)))) return;
    if (de->CreateClassEnumerator(category, &em, 0) == S_OK)
    {
        IMoniker *m = nullptr;
        while (em->Next(1, &m, nullptr) == S_OK)
        {
            IPropertyBag *bag = nullptr;
            if (SUCCEEDED(m->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag))))
            {
                Device d{prop(bag, L"FriendlyName"), prop(bag, L"DevicePath")};
                if (d.path.empty()) d.path = d.name;
                bool stop = f(d, m);
                bag->Release();
                if (stop) { m->Release(); break; }
            }
            m->Release();
        }
        em->Release();
    }
    de->Release();
}
std::vector<Device> capture_devices()
{
    std::vector<Device> list;
    each_device([&](const Device &d, IMoniker *) { list.push_back(d); return false; });
    return list;
}

// ---- open ----
static IPin *find_pin(IBaseFilter *f, const GUID &major, int skip)
{
    IEnumPins *e = nullptr;
    IPin *p = nullptr, *found = nullptr;
    if (FAILED(f->EnumPins(&e))) return nullptr;
    while (!found && e->Next(1, &p, nullptr) == S_OK)
    {
        PIN_DIRECTION dir;
        IEnumMediaTypes *t = nullptr;
        if (SUCCEEDED(p->QueryDirection(&dir)) && dir == PINDIR_OUTPUT && SUCCEEDED(p->EnumMediaTypes(&t)))
        {
            AM_MEDIA_TYPE *mt = nullptr;
            if (t->Next(1, &mt, nullptr) == S_OK)
            {
                if (mt->majortype == major && skip-- == 0) { found = p; found->AddRef(); }
                free_type(mt);
            }
            t->Release();
        }
        p->Release();
    }
    e->Release();
    return found;
}
static void set_rate(AM_MEDIA_TYPE *mt, double fps)
{
    if (fps <= 0) return;
    if (mt->formattype == FORMAT_VideoInfo) reinterpret_cast<VIDEOINFOHEADER *>(mt->pbFormat)->AvgTimePerFrame = (REFERENCE_TIME)(1e7 / fps);
    else if (mt->formattype == FORMAT_VideoInfo2) reinterpret_cast<VIDEOINFOHEADER2 *>(mt->pbFormat)->AvgTimePerFrame = (REFERENCE_TIME)(1e7 / fps);
}

std::wstring Capture::open(const std::wstring &path, const VideoFormat &wanted, bool audio, CaptureSink *sink)
{
    VideoFormat want = wanted;
    close();
    formats.clear();
    vf = VideoFormat();
    af = AudioFormat();
    IPin *pin = nullptr, *apin = nullptr;
    std::wstring vname;
    IAMStreamConfig *cfg = nullptr;
    auto fail = [&](const wchar_t *ko, const wchar_t *en, HRESULT hr) {
        logf("capture: failed %08lx (%ls)", (unsigned long)hr, en);
        release(cfg);
        release(pin);
        release(apin);
        close();
        wchar_t t[160];
        swprintf(t, 160, L"%ls (%08lX)", tr(ko, en), (unsigned long)hr);
        return std::wstring(t);
    };
    each_device([&](const Device &d, IMoniker *m) {
        if (d.path != path) return false;
        m->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&source));
        vname = d.name;
        logf("capture: device %ls", d.name.c_str());
        return true;
    });
    if (!source) return fail(L"캡처 장치를 찾지 못했어요", L"Capture device not found", E_FAIL);
    HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph));
    if (FAILED(hr)) return fail(L"DirectShow를 쓸 수 없어요", L"DirectShow is not available", hr);
    if (FAILED(hr = graph->AddFilter(source, L"capture"))) return fail(L"캡처 장치를 열 수 없어요", L"The capture device could not be opened", hr);
    pin = find_pin(source, MEDIATYPE_Video, 0);
    if (!pin) return fail(L"영상 출력이 없는 장치예요", L"This device has no video output", E_FAIL);

    // formats of the device
    AM_MEDIA_TYPE *best = nullptr, *current = nullptr;
    VideoFormat bf, cf;
    if (SUCCEEDED(pin->QueryInterface(IID_PPV_ARGS(&cfg))))
    {
        cfg->GetFormat(&current);
        if (current) { parse(*current, cf); logf("capture: current format %s %dx%d %.3f", pixel_name(cf.pixel), cf.w, cf.h, cf.fps); }
        int n = 0, size = 0;
        cfg->GetNumberOfCapabilities(&n, &size);
        std::vector<BYTE> caps((size_t)std::max(size, (int)sizeof(VIDEO_STREAM_CONFIG_CAPS)));
        Pixel asked = want.pixel;
        if (!want.w)
        {
            // automatic: the card scales to the requested size, so the largest 60 Hz size it offers, up to 3840x2160
            for (int i = 0; i < n; i++)
            {
                AM_MEDIA_TYPE *mt = nullptr;
                if (cfg->GetStreamCaps(i, &mt, caps.data()) != S_OK) continue;
                VideoFormat f;
                auto *c = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS *>(caps.data());
                if (parse(*mt, f) && f.pixel != Pixel::bgr24 && f.pixel != Pixel::p010 && f.w <= 3840 && f.h <= 2160 && f.w * 9 == f.h * 16)
                {
                    f.fps = c->MinFrameInterval > 0 ? 1e7 / (double)c->MinFrameInterval : f.fps;
                    if (f.fps >= 59 && (f.w > want.w || (f.w == want.w && (f.pixel == Pixel::nv12 || want.pixel == Pixel::mjpg) && f.pixel != Pixel::mjpg))) want = f; // MJPEG only where the size is not to be had without
                }
                free_type(mt);
            }
            if (want.fps > 60) want.fps = 60;
            if (asked != Pixel::none) want.pixel = asked; // the size is automatic, the kind of pixel was chosen (P010 for a console's HDR)
        }
        int bestScore = -1;
        for (int i = 0, pass = 0; i <= n; i++)
        {
            if (i == n)
            {
                if (best || pass++ || cf.pixel != Pixel::none || formats.empty()) break;
                // no such size, and the driver's own choice is of a kind this program cannot read: the largest one it can
                want = *std::max_element(formats.begin(), formats.end(), [](const VideoFormat &a, const VideoFormat &b) { return std::min(a.fps, 60.0) * a.w * a.h < std::min(b.fps, 60.0) * b.w * b.h; });
                i = -1;
                continue;
            }
            AM_MEDIA_TYPE *mt = nullptr;
            if (cfg->GetStreamCaps(i, &mt, caps.data()) != S_OK) continue;
            VideoFormat f;
            auto *c = reinterpret_cast<VIDEO_STREAM_CONFIG_CAPS *>(caps.data());
            bool keep = false;
            if (parse(*mt, f))
            {
                double top = c->MinFrameInterval > 0 ? 1e7 / (double)c->MinFrameInterval : f.fps, low = c->MaxFrameInterval > 0 ? 1e7 / (double)c->MaxFrameInterval : f.fps;
                f.fps = top;
                bool known = false;
                for (auto &k : formats)
                    if (k.pixel == f.pixel && k.w == f.w && k.h == f.h) { k.fps = std::max(k.fps, f.fps); known = true; }
                if (!known) formats.push_back(f);
                // score: the wanted size (or the driver's current one), the wanted pixel type (or NV12), a rate that fits
                int w = want.w ? want.w : cf.w, h = want.h ? want.h : cf.h;
                double fps = want.fps > 0 ? want.fps : cf.fps;
                Pixel px = want.pixel != Pixel::none ? want.pixel : Pixel::nv12;
                // after the wanted kind: NV12, then the packed 4:2:2 kinds, the planar 4:2:0 ones (woven on the CPU), RGB, MJPEG last
                int score = (f.w == w && f.h == h ? 1000 : 0) + (f.pixel == px ? 200 : f.pixel == Pixel::nv12 ? 60 : f.pixel == Pixel::yuy2 ? 40 : f.pixel == Pixel::uyvy ? 38 : f.pixel == Pixel::i420 || f.pixel == Pixel::yv12 ? 30 : f.pixel == Pixel::bgra ? 24 : f.pixel == Pixel::bgr24 ? 20 : f.pixel == Pixel::mjpg ? 10 : 0);
                if (fps <= top + 0.5 && fps >= low - 0.5) score += 100;
                if (score > bestScore && f.w == w && f.h == h)
                {
                    bestScore = score;
                    free_type(best);
                    best = mt;
                    keep = true;
                    bf = f;
                    double use = fps > 0 ? (fps > top ? top : fps < low ? low : fps) : top;
                    set_rate(best, use);
                    bf.fps = use;
                }
            }
            if (!keep) free_type(mt);
        }
    }
    if (!best) { best = current; current = nullptr; bf = cf; }
    free_type(current);
    if (!best) return fail(L"쓸 수 있는 화면 형식이 없어요", L"No usable video format", E_FAIL);
    if (cfg)
    {
        hr = cfg->SetFormat(best);
        logf("capture: set %s %dx%d %.3f -> %08lx", pixel_name(bf.pixel), bf.w, bf.h, bf.fps, (unsigned long)hr);
    }
    IAMBufferNegotiation *neg = nullptr;
    if (SUCCEEDED(pin->QueryInterface(IID_PPV_ARGS(&neg))))
    {
        ALLOCATOR_PROPERTIES ap = {4, -1, -1, -1};
        neg->SuggestAllocatorProperties(&ap);
        neg->Release();
    }
    vs = new SinkFilter(true, sink);
    graph->AddFilter(vs, L"video sink");
    hr = graph->ConnectDirect(pin, static_cast<IPin *>(vs), best);
    if (FAILED(hr)) hr = graph->ConnectDirect(pin, static_cast<IPin *>(vs), nullptr);
    free_type(best);
    if (FAILED(hr)) return fail(L"이 화면 형식으로 연결하지 못했어요", L"Could not connect with this format", hr);
    vf = vs->vf;
    logf("capture: connected %s %dx%d %.3f", pixel_name(vf.pixel), vf.w, vf.h, vf.fps);
    if (audio && !(apin = find_pin(source, MEDIATYPE_Audio, 0)) && vname.size() >= 4)
    {
        // USB cards bring their sound as an audio device of its own, named after the card ("Digital Audio Interface (USB3.0 Capture)")
        std::wstring low = vname;
        for (auto &ch : low) ch = (wchar_t)towlower(ch);
        each_device([&](const Device &d, IMoniker *m) {
            std::wstring n = d.name;
            for (auto &ch : n) ch = (wchar_t)towlower(ch);
            if (n.find(low) == std::wstring::npos) return false;
            if (FAILED(m->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&asource))) || !asource) return false;
            if (SUCCEEDED(graph->AddFilter(asource, L"audio capture")) && (apin = find_pin(asource, MEDIATYPE_Audio, 0)))
            {
                logf("capture: sound from %ls", d.name.c_str());
                return true;
            }
            graph->RemoveFilter(asource);
            release(asource);
            return false;
        }, CLSID_AudioInputDeviceCategory);
    }
    if (audio && apin)
    {
        IAMBufferNegotiation *an = nullptr;
        if (SUCCEEDED(apin->QueryInterface(IID_PPV_ARGS(&an))))
        {
            ALLOCATOR_PROPERTIES ap = {8, 48000 * 4 / 100, -1, -1}; // 10 ms pieces instead of the usual half second
            an->SuggestAllocatorProperties(&ap);
            an->Release();
        }
        as = new SinkFilter(false, sink);
        graph->AddFilter(as, L"audio sink");
        HRESULT ha = graph->ConnectDirect(apin, static_cast<IPin *>(as), nullptr);
        if (SUCCEEDED(ha)) af = as->af;
        logf("capture: audio %08lx %d Hz %d ch %d bit", (unsigned long)ha, af.rate, af.channels, af.bits);
    }
    if (FAILED(hr = graph->QueryInterface(IID_PPV_ARGS(&control)))) return fail(L"시작하지 못했어요", L"Could not start", hr);
    hr = control->Run();
    if (FAILED(hr)) return fail(L"캡처를 시작하지 못했어요. 다른 프로그램이 장치를 쓰고 있는지 확인하세요", L"Could not start. Is another program using the device?", hr);
    logf("capture: running");
    release(cfg);
    release(pin);
    release(apin);
    return L"";
}
void Capture::close()
{
    if (vs) vs->detach();
    if (as) as->detach();
    if (control) { control->Stop(); release(control); }
    if (graph)
    {
        if (vs) graph->RemoveFilter(vs);
        if (as) graph->RemoveFilter(as);
        if (source) graph->RemoveFilter(source);
        if (asource) graph->RemoveFilter(asource);
    }
    if (vs) { vs->Release(); vs = nullptr; }
    if (as) { as->Release(); as = nullptr; }
    release(source);
    release(asource);
    release(graph);
}
