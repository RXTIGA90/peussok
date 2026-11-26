// SPDX-License-Identifier: GPL-3.0-only
#include "mjpeg.h"
#include <algorithm>
#include <atomic>
#include <d3d11_4.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>

// Media Foundation is loaded when it is needed: some editions of Windows come without it.
static struct Mf
{
    HRESULT(WINAPI *startup)(ULONG, DWORD) = nullptr;
    HRESULT(WINAPI *enumerate)(GUID, UINT32, const MFT_REGISTER_TYPE_INFO *, const MFT_REGISTER_TYPE_INFO *, IMFActivate ***, UINT32 *) = nullptr;
    HRESULT(WINAPI *sample)(IMFSample **) = nullptr;
    HRESULT(WINAPI *buffer)(DWORD, IMFMediaBuffer **) = nullptr;
    HRESULT(WINAPI *type)(IMFMediaType **) = nullptr;
    HRESULT(WINAPI *manager)(UINT *, IMFDXGIDeviceManager **) = nullptr;
    bool ready = false, tried = false;
    bool load()
    {
        if (tried) return ready;
        tried = true;
        HMODULE m = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto get = [&](auto &f, const char *n) { f = m ? reinterpret_cast<std::decay_t<decltype(f)>>(reinterpret_cast<void *>(GetProcAddress(m, n))) : nullptr; return f != nullptr; };
        ready = get(startup, "MFStartup") && get(enumerate, "MFTEnumEx") && get(sample, "MFCreateSample") && get(buffer, "MFCreateMemoryBuffer") && get(type, "MFCreateMediaType") && get(manager, "MFCreateDXGIDeviceManager") &&
                SUCCEEDED(startup(MF_VERSION, MFSTARTUP_LITE));
        return ready;
    }
} mf;
static Smooth took;
static const wchar_t *volatile who = L"";
double Mjpeg::last_ms() { return took.get(); }
const wchar_t *Mjpeg::with() { return who; }

// Events of a decoder that works by itself: how many pictures it wants and how many are ready.
class Events final : public IMFAsyncCallback
{
public:
    Events(IMFMediaEventGenerator *g) : from(g), event(CreateEventW(nullptr, FALSE, FALSE, nullptr)) { from->AddRef(); }
    STDMETHODIMP QueryInterface(REFIID id, void **p) override
    {
        if (id != IID_IUnknown && id != IID_IMFAsyncCallback) { *p = nullptr; return E_NOINTERFACE; }
        *p = static_cast<IMFAsyncCallback *>(this);
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
    STDMETHODIMP GetParameters(DWORD *, DWORD *) override { return E_NOTIMPL; }
    STDMETHODIMP Invoke(IMFAsyncResult *r) override
    {
        IMFMediaEvent *e = nullptr;
        if (FAILED(from->EndGetEvent(r, &e))) return S_OK;
        MediaEventType t = MEUnknown;
        e->GetType(&t);
        e->Release();
        if (t == METransformNeedInput) ++wants;
        if (t == METransformHaveOutput) ++has;
        SetEvent(event);
        if (!stop) from->BeginGetEvent(this, nullptr);
        return S_OK;
    }
    bool wait(std::atomic<int> &n, DWORD ms)
    {
        ULONGLONG end = GetTickCount64() + ms;
        while (n <= 0)
        {
            ULONGLONG now = GetTickCount64();
            if (now >= end) return false;
            WaitForSingleObject(event, (DWORD)(end - now));
        }
        return true;
    }
    std::atomic<int> wants{0}, has{0};
    std::atomic<bool> stop{false};

private:
    ~Events() { from->Release(); CloseHandle(event); }
    std::atomic<ULONG> refs{1};
    IMFMediaEventGenerator *from;
    HANDLE event;
};
struct Mjpeg::State
{
    IMFTransform *t = nullptr;
    IMFActivate *act = nullptr;
    Events *events = nullptr;
    IMFSample *out = nullptr; // software: the same one again and again
    IMFMediaBuffer *out_buffer = nullptr;
    bool locked = false, mapped = false, provides = false, started = false;
    DWORD size = 0;
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    ID3D11Texture2D *stage = nullptr;
    IMFDXGIDeviceManager *man = nullptr;
    LONGLONG n = 0;
};
void Mjpeg::close()
{
    if (!s) return;
    if (s->mapped) s->ctx->Unmap(s->stage, 0);
    if (s->locked) s->out_buffer->Unlock();
    if (s->events) s->events->stop = true;
    if (s->t && s->started) { s->t->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0); s->t->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0); }
    if (s->act) s->act->ShutdownObject();
    release(s->out_buffer);
    release(s->out);
    release(s->t);
    release(s->act);
    release(s->events);
    release(s->man);
    release(s->stage);
    release(s->ctx);
    release(s->dev);
    delete s;
    s = nullptr;
    result = nullptr;
    before.clear();
    took.clear();
    who = L"";
}
bool Mjpeg::start(IMFTransform *t, bool hard)
{
    IMFAttributes *a = nullptr;
    UINT32 async = 0;
    if (SUCCEEDED(t->GetAttributes(&a)) && a)
    {
        a->GetUINT32(MF_TRANSFORM_ASYNC, &async);
        if (async) a->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        a->SetUINT32(MF_LOW_LATENCY, TRUE);
        a->Release();
    }
    if (hard && (FAILED(t->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR)s->man)))) return false;
    IMFMediaType *in = nullptr;
    if (FAILED(mf.type(&in))) return false;
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_MJPG);
    in->SetUINT64(MF_MT_FRAME_SIZE, (UINT64)w << 32 | (UINT32)h);
    in->SetUINT64(MF_MT_FRAME_RATE, (UINT64)60 << 32 | 1);
    in->SetUINT64(MF_MT_PIXEL_ASPECT_RATIO, (UINT64)1 << 32 | 1);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    HRESULT hr = t->SetInputType(0, in, 0);
    in->Release();
    if (FAILED(hr)) return false;
    bool any = false;
    for (DWORD i = 0; !any; i++)
    {
        IMFMediaType *o = nullptr;
        if (FAILED(t->GetOutputAvailableType(0, i, &o))) break;
        GUID g{};
        o->GetGUID(MF_MT_SUBTYPE, &g);
        any = g == MFVideoFormat_NV12 && SUCCEEDED(t->SetOutputType(0, o, 0));
        o->Release();
    }
    MFT_OUTPUT_STREAM_INFO info{};
    if (!any || FAILED(t->GetOutputStreamInfo(0, &info))) return false;
    s->provides = info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES);
    s->size = info.cbSize;
    if (!s->provides && (FAILED(mf.sample(&s->out)) || FAILED(mf.buffer(std::max<DWORD>(s->size, (DWORD)w * (DWORD)h * 3 / 2), &s->out_buffer)) || FAILED(s->out->AddBuffer(s->out_buffer)))) return false;
    if (async)
    {
        IMFMediaEventGenerator *g = nullptr;
        if (FAILED(t->QueryInterface(IID_PPV_ARGS(&g)))) return false;
        s->events = new Events(g);
        g->BeginGetEvent(s->events, nullptr);
        g->Release();
    }
    t->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    t->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    s->started = true;
    return true;
}
bool Mjpeg::open(int width, int height, IDXGIAdapter1 *adapter)
{
    close();
    w = width;
    h = height;
    fails = 0;
    if (!mf.load()) { logf("mjpeg: Media Foundation is not there"); return false; }
    for (int hard = hard_failed || !adapter ? 0 : 1; hard >= 0; hard--)
    {
        MFT_REGISTER_TYPE_INFO in = {MFMediaType_Video, MFVideoFormat_MJPG};
        IMFActivate **act = nullptr;
        UINT32 n = 0;
        if (FAILED(mf.enumerate(MFT_CATEGORY_VIDEO_DECODER, hard ? MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER : MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, &in, nullptr, &act, &n))) continue;
        DXGI_ADAPTER_DESC1 d{};
        if (adapter) adapter->GetDesc1(&d);
        const wchar_t *vendor = d.VendorId == 0x10de ? L"NVIDIA" : d.VendorId == 0x1002 ? L"AMD" : d.VendorId == 0x8086 ? L"Intel" : L"?";
        bool ok = false;
        for (UINT32 i = 0; i < n && !ok; i++)
        {
            wchar_t *name = nullptr;
            UINT32 len = 0;
            act[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &len);
            std::wstring called = name ? name : L"";
            if (name) CoTaskMemFree(name);
            if (hard && called.find(vendor) == std::wstring::npos) continue; // the card that shows the pictures
            s = new State;
            s->act = act[i];
            s->act->AddRef();
            if (hard)
            {
                UINT token = 0;
                ID3D10Multithread *mt = nullptr;
                if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &s->dev, nullptr, &s->ctx)) || FAILED(s->dev->QueryInterface(IID_PPV_ARGS(&mt)))) { close(); continue; }
                mt->SetMultithreadProtected(TRUE);
                mt->Release();
                D3D11_TEXTURE2D_DESC td{};
                td.Width = (UINT)w;
                td.Height = (UINT)h;
                td.MipLevels = td.ArraySize = 1;
                td.Format = DXGI_FORMAT_NV12;
                td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_STAGING;
                td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                if (FAILED(s->dev->CreateTexture2D(&td, nullptr, &s->stage)) || FAILED(mf.manager(&token, &s->man)) || FAILED(s->man->ResetDevice(s->dev, token))) { close(); continue; }
            }
            HRESULT hr = act[i]->ActivateObject(IID_PPV_ARGS(&s->t));
            ok = SUCCEEDED(hr) && start(s->t, hard);
            logf("mjpeg: %ls %dx%d %s", called.c_str(), w, h, ok ? "ready" : "not usable");
            if (!ok) close();
            else who = hard ? vendor : L"CPU";
        }
        for (UINT32 i = 0; i < n; i++) act[i]->Release();
        CoTaskMemFree(act);
        if (ok) return true;
    }
    return false;
}
bool Mjpeg::run(const uint8_t *data, size_t size)
{
    if (s->mapped) { s->ctx->Unmap(s->stage, 0); s->mapped = false; }
    if (s->locked) { s->out_buffer->Unlock(); s->locked = false; }
    result = nullptr;
    if (s->events && !s->events->wait(s->events->wants, 300)) return false;
    IMFSample *in = nullptr;
    IMFMediaBuffer *b = nullptr;
    BYTE *to = nullptr;
    HRESULT hr = mf.sample(&in);
    if (SUCCEEDED(hr)) hr = mf.buffer((DWORD)size, &b);
    if (SUCCEEDED(hr)) hr = b->Lock(&to, nullptr, nullptr);
    if (SUCCEEDED(hr))
    {
        memcpy(to, data, size);
        b->Unlock();
        b->SetCurrentLength((DWORD)size);
        in->AddBuffer(b);
        in->SetSampleTime(s->n++ * 166667);
        in->SetSampleDuration(166667);
        hr = s->t->ProcessInput(0, in, 0);
        if (SUCCEEDED(hr) && s->events) --s->events->wants;
    }
    release(b);
    release(in);
    if (FAILED(hr)) return false;
    for (int again = 0; again < 2; again++)
    {
        if (s->events)
        {
            if (!s->events->wait(s->events->has, 300)) return false;
            --s->events->has;
        }
        MFT_OUTPUT_DATA_BUFFER o{};
        DWORD st = 0;
        o.pSample = s->out;
        hr = s->t->ProcessOutput(0, 1, &o, &st);
        if (o.pEvents) o.pEvents->Release();
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) // it has seen the real size
        {
            bool any = false;
            for (DWORD i = 0; !any; i++)
            {
                IMFMediaType *t = nullptr;
                if (FAILED(s->t->GetOutputAvailableType(0, i, &t))) break;
                GUID g{};
                t->GetGUID(MF_MT_SUBTYPE, &g);
                any = g == MFVideoFormat_NV12 && SUCCEEDED(s->t->SetOutputType(0, t, 0));
                t->Release();
            }
            if (!any) return false;
            continue;
        }
        if (FAILED(hr)) return false;
        if (!s->provides)
        {
            BYTE *p = nullptr;
            DWORD n = 0;
            if (FAILED(s->out_buffer->Lock(&p, nullptr, &n))) return false;
            s->locked = true;
            if (n < (DWORD)w * (DWORD)h * 3 / 2) return false;
            result = p;
            return true;
        }
        IMFMediaBuffer *ob = nullptr;
        IMFDXGIBuffer *x = nullptr;
        ID3D11Texture2D *tex = nullptr;
        UINT sub = 0;
        if (o.pSample) o.pSample->GetBufferByIndex(0, &ob);
        if (ob && SUCCEEDED(ob->QueryInterface(IID_PPV_ARGS(&x)))) { x->GetResource(IID_PPV_ARGS(&tex)); x->GetSubresourceIndex(&sub); }
        D3D11_MAPPED_SUBRESOURCE m{};
        if (tex)
        {
            D3D11_TEXTURE2D_DESC td;
            tex->GetDesc(&td);
            D3D11_BOX box = {0, 0, 0, (UINT)w, (UINT)h, 1};
            if (td.Format == DXGI_FORMAT_NV12 && td.Width >= (UINT)w && td.Height >= (UINT)h)
            {
                s->ctx->CopySubresourceRegion(s->stage, 0, 0, 0, 0, tex, sub, &box);
                s->mapped = SUCCEEDED(s->ctx->Map(s->stage, 0, D3D11_MAP_READ, 0, &m));
            }
        }
        release(tex);
        release(x);
        release(ob);
        if (o.pSample) o.pSample->Release();
        if (!s->mapped) return false;
        const uint8_t *p = (const uint8_t *)m.pData;
        if (m.RowPitch != (UINT)w)
        {
            packed.resize((size_t)w * h * 3 / 2);
            for (int y = 0; y < h * 3 / 2; y++) memcpy(&packed[(size_t)w * y], p + (size_t)m.RowPitch * y, (size_t)w);
            p = packed.data();
        }
        result = p;
        return true;
    }
    return false;
}
bool Mjpeg::decode(const uint8_t *data, size_t size, int width, int height, IDXGIAdapter1 *adapter, const uint8_t *&out, size_t &n)
{
    while (size > 4 && !(data[size - 2] == 0xFF && data[size - 1] == 0xD9)) --size; // some drivers hand over more than the picture
    if (dead || size <= 4 || data[0] != 0xFF || data[1] != 0xD8) return false;
    n = (size_t)width * height * 3 / 2;
    if (s && width == w && height == h && result && size == before.size() && !memcmp(before.data(), data, size)) { out = result; return true; } // the same picture again
    for (;;)
    {
        if ((!s || width != w || height != h) && !open(width, height, adapter)) { dead = true; return false; }
        LONGLONG t = qpc();
        if (run(data, size))
        {
            fails = 0;
            if (s->n > 3) took.add(ms_of(qpc() - t)); // the first ones make the decoder ready
            before.assign(data, data + size);
            out = result;
            return true;
        }
        static int said;
        if (said++ < 10) logf("mjpeg: a picture was not decoded (%ls)", who);
        before.clear();
        if (++fails < 3) return false;
        bool hard = s && s->dev;
        close();
        if (!hard) return false; // opened again with the next one
        hard_failed = true;      // the one of Windows
    }
}
