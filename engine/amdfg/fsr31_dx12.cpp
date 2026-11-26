// SPDX-License-Identifier: GPL-3.0-only
// Offscreen AMD FSR frame generation (FidelityFX SDK 2.x "ffx-api", D3D12) without a swapchain.
// Official AMD release v2.4.0 headers (include\) and signed DLLs (rt\): amd_fidelityfx_loader_dx12.dll
//   (ffx-api entry points) + amd_fidelityfx_framegeneration_dx12.dll (providers FSR FG 4.0.2 ML / FSR FG 3.1.x).
// The loader is loaded with LoadLibrary/GetProcAddress (plain C API, works with zig/mingw).
// No swapchain context, no frame generation callback: ffxConfigure -> PrepareV2 -> manual FG dispatch into our UAV.
//
// C API (ctypes) - same shape as dlssg_ngx (dg_*):
//   void* fg_create(const wchar_t* rtDir, const wchar_t* logPath, int w, int h, int mvW, int mvH, int fmt)
//   int   fg_interpolate(void* e, const uint8_t* a, const uint8_t* b, const void* mvHalf2, uint64_t idA, uint64_t idB, uint8_t* out, double* msOut)
//   int   fg_set_f(void* e, const char* key, float v)   // mvscale_x/y, depth, near, far, fov, frame_ms, transfer
//   void  fg_stats(void* e, double* o4)
//   void  fg_destroy(void* e)
// Env: FG_ADAPTER=<dxgi index> picks the adapter (default: high-performance GPU), FG_D3D_DEBUG=1 debug layer,
//                  FG_VERSION=<index into GetVersions list> overrides the provider, FG_DEBUG_CHECK=1 runtime API checks.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <mutex>

#include "ffx_api.h"
#include "ffx_api_types.h"
#include "dx12/ffx_api_dx12.h"
#include "ffx_framegeneration.h"

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

static FILE* g_log = nullptr;
static std::mutex g_logMu;
#define LOG(...) do { std::lock_guard<std::mutex> _lk(g_logMu); if (g_log) { fprintf(g_log, __VA_ARGS__); fputc('\n', g_log); fflush(g_log); } } while (0)

// ---------------------------------------------------------------- loader (process-wide, never unloaded)
static struct {
    HMODULE loader = nullptr, fgdll = nullptr;
    PfnFfxCreateContext create = nullptr; PfnFfxDestroyContext destroy = nullptr; PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr; PfnFfxDispatch dispatch = nullptr;
} ffx;
static std::mutex g_ffxMu; // the ffx contexts are not thread safe; also serialises loader init

static void ffxMsg(uint32_t type, const wchar_t* msg) {
    std::lock_guard<std::mutex> lk(g_logMu);
    if (g_log) { fprintf(g_log, "[ffx %s] %ls\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "ERR" : "WARN", msg); fflush(g_log); }
}

static bool loadFfx(const wchar_t* rtDir) {
    if (ffx.create) return true;
    std::wstring rt = rtDir;
    // Load the effect DLL explicitly first: the loader reuses a module of that name already in the process (lookup step 1),
    // so this pins OUR copy instead of whatever sits next to the host exe (step 2).
    std::wstring fgp = rt + L"\\amd_fidelityfx_framegeneration_dx12.dll", ldp = rt + L"\\amd_fidelityfx_loader_dx12.dll";
    ffx.fgdll = LoadLibraryExW(fgp.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!ffx.fgdll) LOG("LoadLibrary %ls fail %lu", fgp.c_str(), GetLastError());
    ffx.loader = LoadLibraryExW(ldp.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!ffx.loader) { LOG("LoadLibrary %ls fail %lu", ldp.c_str(), GetLastError()); return false; }
    ffx.create = (PfnFfxCreateContext)GetProcAddress(ffx.loader, "ffxCreateContext");
    ffx.destroy = (PfnFfxDestroyContext)GetProcAddress(ffx.loader, "ffxDestroyContext");
    ffx.configure = (PfnFfxConfigure)GetProcAddress(ffx.loader, "ffxConfigure");
    ffx.query = (PfnFfxQuery)GetProcAddress(ffx.loader, "ffxQuery");
    ffx.dispatch = (PfnFfxDispatch)GetProcAddress(ffx.loader, "ffxDispatch");
    if (!ffx.create || !ffx.destroy || !ffx.configure || !ffx.query || !ffx.dispatch) { LOG("missing loader export"); ffx.create = nullptr; return false; }
    wchar_t nm[MAX_PATH]; GetModuleFileNameW(ffx.loader, nm, MAX_PATH); LOG("loader: %ls", nm);
    if (ffx.fgdll) { GetModuleFileNameW(ffx.fgdll, nm, MAX_PATH); LOG("fg dll: %ls", nm); }
    return true;
}

// ---------------------------------------------------------------- D3D12 helpers (same as dlssg_ngx)
static ID3D12Resource* makeTex(ID3D12Device* d, UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_STATES st, bool uav) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.Format = f; rd.SampleDesc.Count = 1;
    if (uav) rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* r = nullptr; HRESULT hr = d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r));
    if (FAILED(hr)) LOG("makeTex fail hr=0x%08lx", (unsigned long)hr);
    return r;
}
static ID3D12Resource* makeBuf(ID3D12Device* d, UINT64 size, D3D12_HEAP_TYPE t) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = t;
    D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, t == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r));
    return r;
}
static void barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER br{}; br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; br.Transition.pResource = r; br.Transition.StateBefore = a; br.Transition.StateAfter = b; br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &br);
}
static UINT pitchOf(UINT rowBytes) { return (rowBytes + 255) & ~255u; }
static void bufToTex(ID3D12GraphicsCommandList* cl, ID3D12Resource* buf, UINT64 off, ID3D12Resource* tex, UINT w, UINT h, DXGI_FORMAT f, UINT bpp) {
    D3D12_TEXTURE_COPY_LOCATION s{}, t{};
    s.pResource = buf; s.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; s.PlacedFootprint.Offset = off; s.PlacedFootprint.Footprint = { f, w, h, 1, pitchOf(w * bpp) };
    t.pResource = tex; t.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cl->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
}
static void texToBuf(ID3D12GraphicsCommandList* cl, ID3D12Resource* tex, ID3D12Resource* buf, UINT w, UINT h, DXGI_FORMAT f, UINT bpp) {
    D3D12_TEXTURE_COPY_LOCATION s{}, t{};
    t.pResource = buf; t.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; t.PlacedFootprint.Footprint = { f, w, h, 1, pitchOf(w * bpp) };
    s.pResource = tex; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cl->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
}
static void copyRows(uint8_t* dst, UINT dp, const uint8_t* src, UINT sp, UINT rowBytes, UINT h) { for (UINT y = 0; y < h; y++) memcpy(dst + (size_t)y * dp, src + (size_t)y * sp, rowBytes); }
template <class T> static void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------- engine
static const D3D12_RESOURCE_STATES SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
struct Engine {
    UINT W, H, MW, MH; DXGI_FORMAT fmt;
    ID3D12Device* dev = nullptr; ID3D12CommandQueue* q = nullptr; ID3D12CommandAllocator* ca = nullptr; ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12Fence* fence = nullptr; UINT64 fv = 0; HANDLE ev = nullptr;
    ID3D12Resource* color[2] = {}; ID3D12Resource* mv = nullptr; ID3D12Resource* depth = nullptr; ID3D12Resource* outI = nullptr;
    ID3D12Resource* up = nullptr; ID3D12Resource* rb = nullptr; uint8_t* upPtr = nullptr; uint8_t* rbPtr = nullptr;
    UINT64 offColor = 0, offMv = 0, offDepth = 0;
    ffxContext fg = nullptr;
    int cur = 0; uint64_t lastId = ~0ull; uint64_t frameId = 1; bool zeroMvUploaded = false; bool depthDirty = true;
    float mvsx = 1.f, mvsy = 1.f, depthVal = 1.0f, nearZ = 0.1f, farZ = 1000.f, fov = 1.0f, frameMs = 16.7f; int transfer = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB;
    LARGE_INTEGER qf;
    ID3D12QueryHeap* qh = nullptr; ID3D12Resource* qrb = nullptr; UINT64 tsFreq = 1;
    double stUpload = 0, stGpuEval = 0, stWait = 0, stRead = 0;
    ID3D12Resource* gsrc = nullptr; ID3D12Resource* gout = nullptr; ID3D12Resource* gslot[8] = {}; int gslots = 0; // GPU path (shared D3D11 textures)
};

static bool waitGpu(Engine* e) {
    e->q->Signal(e->fence, ++e->fv);
    if (e->fence->GetCompletedValue() < e->fv) { e->fence->SetEventOnCompletion(e->fv, e->ev); WaitForSingleObject(e->ev, 5000); }
    HRESULT r = e->dev->GetDeviceRemovedReason(); if (FAILED(r)) { LOG("device removed 0x%08lx", (unsigned long)r); return false; }
    return true;
}
static void beginCl(Engine* e) { e->ca->Reset(); e->cl->Reset(e->ca, nullptr); }
static void dumpDebug(Engine* e) { // FG_D3D_DEBUG only: copy debug-layer messages into our log
    static int total = 0; if (!getenv("FG_D3D_DEBUG") || total > 200) return;
    ID3D12InfoQueue* iq = nullptr; if (FAILED(e->dev->QueryInterface(IID_PPV_ARGS(&iq)))) { LOG("no info queue"); total = 999; return; }
    UINT64 n = iq->GetNumStoredMessages();
    for (UINT64 i = 0; i < n && total <= 200; i++, total++) {
        SIZE_T len = 0; iq->GetMessage(i, nullptr, &len); D3D12_MESSAGE* m = (D3D12_MESSAGE*)malloc(len);
        if (m && SUCCEEDED(iq->GetMessage(i, m, &len))) LOG("[d3d12 sev%d id%d] %s", (int)m->Severity, (int)m->ID, m->pDescription);
        free(m);
    }
    iq->ClearStoredMessages(); iq->Release();
}
static bool submit(Engine* e) {
    HRESULT hr = e->cl->Close(); if (FAILED(hr)) { LOG("cl close 0x%08lx", (unsigned long)hr); dumpDebug(e); return false; }
    ID3D12CommandList* l[] = { e->cl }; e->q->ExecuteCommandLists(1, l); bool ok = waitGpu(e); dumpDebug(e); return ok;
}

static void freeEngine(Engine* e) {
    if (!e) return;
    if (e->rb && e->rbPtr) { D3D12_RANGE wr{ 0, 0 }; e->rb->Unmap(0, &wr); e->rbPtr = nullptr; }
    if (e->up && e->upPtr) { e->up->Unmap(0, nullptr); e->upPtr = nullptr; }
    rel(e->color[0]); rel(e->color[1]); rel(e->mv); rel(e->depth); rel(e->outI); rel(e->up); rel(e->rb); rel(e->qh); rel(e->qrb);
    rel(e->cl); rel(e->ca); rel(e->fence); rel(e->q); rel(e->dev);
    if (e->ev) { CloseHandle(e->ev); e->ev = nullptr; }
    delete e;
}

extern "C" __declspec(dllexport) int fg_set_f(Engine* e, const char* k, float v) {
    if (!e) return -1;
    if (!strcmp(k, "mvscale_x")) e->mvsx = v; else if (!strcmp(k, "mvscale_y")) e->mvsy = v;
    else if (!strcmp(k, "depth")) { e->depthVal = v; e->depthDirty = true; }
    else if (!strcmp(k, "near")) e->nearZ = v; else if (!strcmp(k, "far")) e->farZ = v; else if (!strcmp(k, "fov")) e->fov = v;
    else if (!strcmp(k, "frame_ms")) e->frameMs = v; else if (!strcmp(k, "transfer")) e->transfer = (int)v;
    else return -1;
    LOG("set %s=%g", k, v); return 0;
}

// Safe for several engines in one process: waits for this engine's queue to go idle, destroys only its own context.
// The loader/effect DLLs stay loaded (process-wide; unloading while another engine runs would crash).
extern "C" __declspec(dllexport) void fg_destroy(Engine* e) {
    if (!e) return;
    if (e->q && e->fence && e->ev) { e->q->Signal(e->fence, ++e->fv); if (e->fence->GetCompletedValue() < e->fv) { e->fence->SetEventOnCompletion(e->fv, e->ev); WaitForSingleObject(e->ev, 5000); } }
    LOG("destroy %p: gpu idle", (void*)e);
    if (e->fg) { std::lock_guard<std::mutex> lk(g_ffxMu); ffxReturnCode_t r = ffx.destroy(&e->fg, nullptr); LOG("destroy %p: ffxDestroyContext=%u", (void*)e, r); e->fg = nullptr; }
    freeEngine(e);
    LOG("destroyed");
}

static void logVersions(ID3D12Device* dev) {
    uint64_t count = 0;
    ffxQueryDescGetVersions qv{}; qv.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS; qv.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION; qv.device = dev; qv.outputCount = &count;
    ffxReturnCode_t r = ffx.query(nullptr, &qv.header);
    LOG("GetVersions count=%llu rc=%u", (unsigned long long)count, r);
    if (r || !count || count > 16) return;
    uint64_t ids[16] = {}; const char* names[16] = {};
    qv.versionIds = ids; qv.versionNames = names; ffx.query(nullptr, &qv.header);
    for (uint64_t i = 0; i < count; i++) LOG("  version[%llu] id=0x%llx name=%s", (unsigned long long)i, (unsigned long long)ids[i], names[i] ? names[i] : "?");
}

extern "C" __declspec(dllexport) Engine* fg_create(const wchar_t* rtDir, const wchar_t* logPath, int w, int h, int mvW, int mvH, int fmtCode) {
    if (!g_log && logPath) g_log = _wfopen(logPath, L"w");
    Engine* e = new Engine(); e->W = w; e->H = h; e->MW = mvW; e->MH = mvH;
    e->fmt = fmtCode == 1 ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    QueryPerformanceFrequency(&e->qf);

    if (getenv("FG_D3D_DEBUG")) { ID3D12Debug* dbg = nullptr; if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); dbg->Release(); LOG("d3d12 debug layer on"); } }

    // 1) D3D12 device: high-performance adapter, or FG_ADAPTER=<EnumAdapters1 index>
    IDXGIFactory6* fac = nullptr; if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)))) { LOG("dxgi factory fail"); freeEngine(e); return nullptr; }
    IDXGIAdapter1* ad = nullptr;
    if (getenv("FG_ADAPTER")) fac->EnumAdapters1((UINT)atoi(getenv("FG_ADAPTER")), &ad);
    else fac->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad));
    fac->Release();
    if (!ad) { LOG("adapter not found"); freeEngine(e); return nullptr; }
    DXGI_ADAPTER_DESC1 add{}; ad->GetDesc1(&add); LOG("adapter: %ls (vendor 0x%04x)", add.Description, add.VendorId);
    HRESULT hr = D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&e->dev)); ad->Release();
    if (FAILED(hr)) { LOG("device fail 0x%08lx", (unsigned long)hr); freeEngine(e); return nullptr; }
    { D3D12_FEATURE_DATA_SHADER_MODEL sm{ D3D_SHADER_MODEL_6_6 }; e->dev->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm); LOG("highest shader model 0x%x", (unsigned)sm.HighestShaderModel); }
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    e->dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&e->q));
    e->dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&e->ca));
    e->dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, e->ca, nullptr, IID_PPV_ARGS(&e->cl)); e->cl->Close();
    e->dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&e->fence)); e->ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // 2) resources
    for (int i = 0; i < 2; i++) e->color[i] = makeTex(e->dev, w, h, e->fmt, D3D12_RESOURCE_STATE_COPY_DEST, false);
    e->mv = makeTex(e->dev, mvW, mvH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, false);
    e->depth = makeTex(e->dev, mvW, mvH, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, false);
    e->outI = makeTex(e->dev, w, h, e->fmt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    UINT64 cs = (UINT64)pitchOf(w * 4) * h, ms = (UINT64)pitchOf(mvW * 4) * mvH;
    e->offColor = 0; e->offMv = (cs + 511) & ~511ull; e->offDepth = (e->offMv + ms + 511) & ~511ull;
    e->up = makeBuf(e->dev, e->offDepth + ms, D3D12_HEAP_TYPE_UPLOAD); if (e->up) e->up->Map(0, nullptr, (void**)&e->upPtr);
    e->rb = makeBuf(e->dev, cs, D3D12_HEAP_TYPE_READBACK); if (e->rb) { D3D12_RANGE rr{ 0, (SIZE_T)cs }; e->rb->Map(0, &rr, (void**)&e->rbPtr); }
    { D3D12_QUERY_HEAP_DESC qhd{}; qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qhd.Count = 2; e->dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&e->qh));
      e->qrb = makeBuf(e->dev, 16, D3D12_HEAP_TYPE_READBACK); e->q->GetTimestampFrequency(&e->tsFreq); }
    if (!e->color[0] || !e->color[1] || !e->mv || !e->depth || !e->outI || !e->upPtr || !e->rbPtr) { LOG("resource alloc fail"); freeEngine(e); return nullptr; }
    beginCl(e);
    memset(e->upPtr + e->offMv, 0, ms);
    bufToTex(e->cl, e->up, e->offMv, e->mv, mvW, mvH, DXGI_FORMAT_R16G16_FLOAT, 4);
    barrier(e->cl, e->mv, D3D12_RESOURCE_STATE_COPY_DEST, SRV);
    { float* dp = (float*)(e->upPtr + e->offDepth); UINT pp = pitchOf(mvW * 4) / 4; for (UINT y = 0; y < (UINT)mvH; y++) for (UINT x = 0; x < (UINT)mvW; x++) dp[y * pp + x] = e->depthVal; e->depthDirty = false; }
    bufToTex(e->cl, e->up, e->offDepth, e->depth, mvW, mvH, DXGI_FORMAT_R32_FLOAT, 4);
    barrier(e->cl, e->depth, D3D12_RESOURCE_STATE_COPY_DEST, SRV);
    for (int i = 0; i < 2; i++) barrier(e->cl, e->color[i], D3D12_RESOURCE_STATE_COPY_DEST, SRV);
    if (!submit(e)) { freeEngine(e); return nullptr; }

    // 3) ffx-api frame generation context (no swapchain context)
    std::lock_guard<std::mutex> lk(g_ffxMu);
    if (!loadFfx(rtDir)) { freeEngine(e); return nullptr; }
    { static bool dbgSet = false; if (!dbgSet) { dbgSet = true;
        ffxConfigureDescGlobalDebug gd{}; gd.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG; gd.effectId = FFX_API_EFFECT_ID_FRAMEGENERATION; gd.fpMessage = ffxMsg; gd.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_VERBOSE;
        LOG("global debug configure rc=%u", ffx.configure(nullptr, &gd.header)); } }
    logVersions(e->dev);

    ffxCreateBackendDX12Desc be{}; be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12; be.device = e->dev;
    ffxCreateContextDescFrameGenerationVersion ver{}; ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION; ver.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateContextDescFrameGeneration cd{}; cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    cd.flags = 0;
    if ((UINT)mvW == (UINT)w && (UINT)mvH == (UINT)h) cd.flags |= FFX_FRAMEGENERATION_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS;
    if (getenv("FG_DEBUG_CHECK")) cd.flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
    cd.displaySize = { (uint32_t)w, (uint32_t)h }; cd.maxRenderSize = { (uint32_t)mvW, (uint32_t)mvH };
    cd.backBufferFormat = ffxApiGetSurfaceFormatDX12(e->fmt);
    ffxOverrideVersion ov{}; ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    cd.header.pNext = &be.header; be.header.pNext = &ver.header;
    if (getenv("FG_VERSION")) {
        uint64_t count = 0, ids[16] = {}; const char* names[16] = {};
        ffxQueryDescGetVersions qv{}; qv.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS; qv.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION; qv.device = e->dev; qv.outputCount = &count;
        ffx.query(nullptr, &qv.header); if (count > 16) count = 16; qv.versionIds = ids; qv.versionNames = names; ffx.query(nullptr, &qv.header);
        int idx = atoi(getenv("FG_VERSION")); if (idx >= 0 && (uint64_t)idx < count) { ov.versionId = ids[idx]; ver.header.pNext = &ov.header; LOG("override version %s", names[idx]); }
    }
    ffxReturnCode_t r = ffx.create(&e->fg, &cd.header, nullptr);
    LOG("ffxCreateContext(FG %dx%d mv %dx%d fmt %u flags 0x%x) = %u ctx=%p", w, h, mvW, mvH, cd.backBufferFormat, cd.flags, r, e->fg);
    if (r != FFX_API_RETURN_OK || !e->fg) { e->fg = nullptr; freeEngine(e); return nullptr; }
    { ffxQueryGetProviderVersion pv{}; pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION; ffxReturnCode_t qr = ffx.query(&e->fg, &pv.header);
      LOG("provider version rc=%u id=0x%llx name=%s", qr, (unsigned long long)pv.versionId, pv.versionName ? pv.versionName : "?"); }
    { FfxApiEffectMemoryUsage mu{}; ffxQueryDescFrameGenerationGetGPUMemoryUsage q{}; q.header.type = FFX_API_QUERY_DESC_TYPE_FRAMEGENERATION_GPU_MEMORY_USAGE; q.gpuMemoryUsageFrameGeneration = &mu;
      ffxReturnCode_t qr = ffx.query(&e->fg, &q.header); LOG("gpu memory rc=%u total=%.1f MB aliasable=%.1f MB", qr, mu.totalUsageInBytes / 1048576.0, mu.aliasableUsageInBytes / 1048576.0); }
    return e;
}

static FfxApiResource res(ID3D12Resource* r, uint32_t state) { return ffxApiGetResourceDX12(r, state); }

// Feed one frame (color + mv). FSR FG writes the frame between the previous feed and this one into outI.
static int feed(Engine* e, const uint8_t* bgra, const void* mvHalf2, bool reset, bool readback) {
    LARGE_INTEGER t0, t1, t2; QueryPerformanceCounter(&t0);
    beginCl(e);
    int c = e->cur ^ 1;
    UINT cp = pitchOf(e->W * 4);
    if (!e->gsrc) copyRows(e->upPtr + e->offColor, cp, bgra, e->W * 4, e->W * 4, e->H);
    barrier(e->cl, e->color[c], SRV, D3D12_RESOURCE_STATE_COPY_DEST);
    if (e->gsrc) { barrier(e->cl, e->gsrc, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE); e->cl->CopyResource(e->color[c], e->gsrc); barrier(e->cl, e->gsrc, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON); }
    else bufToTex(e->cl, e->up, e->offColor, e->color[c], e->W, e->H, e->fmt, 4);
    barrier(e->cl, e->color[c], D3D12_RESOURCE_STATE_COPY_DEST, SRV);
    if (mvHalf2 || !e->zeroMvUploaded) {
        UINT mp = pitchOf(e->MW * 4);
        if (mvHalf2) copyRows(e->upPtr + e->offMv, mp, (const uint8_t*)mvHalf2, e->MW * 4, e->MW * 4, e->MH);
        else memset(e->upPtr + e->offMv, 0, (size_t)mp * e->MH);
        barrier(e->cl, e->mv, SRV, D3D12_RESOURCE_STATE_COPY_DEST);
        bufToTex(e->cl, e->up, e->offMv, e->mv, e->MW, e->MH, DXGI_FORMAT_R16G16_FLOAT, 4);
        barrier(e->cl, e->mv, D3D12_RESOURCE_STATE_COPY_DEST, SRV);
        e->zeroMvUploaded = !mvHalf2;
    }
    if (e->depthDirty) {
        float* dp = (float*)(e->upPtr + e->offDepth); UINT pp = pitchOf(e->MW * 4) / 4;
        for (UINT y = 0; y < e->MH; y++) for (UINT x = 0; x < e->MW; x++) dp[y * pp + x] = e->depthVal;
        barrier(e->cl, e->depth, SRV, D3D12_RESOURCE_STATE_COPY_DEST);
        bufToTex(e->cl, e->up, e->offDepth, e->depth, e->MW, e->MH, DXGI_FORMAT_R32_FLOAT, 4);
        barrier(e->cl, e->depth, D3D12_RESOURCE_STATE_COPY_DEST, SRV);
        e->depthDirty = false;
    }
    uint64_t fid = e->frameId++;
    QueryPerformanceCounter(&t1); e->stUpload = double(t1.QuadPart - t0.QuadPart) * 1000.0 / e->qf.QuadPart;
    e->cl->EndQuery(e->qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    ffxReturnCode_t r;
    {
        std::lock_guard<std::mutex> lk(g_ffxMu);
        // (1) configure: FG on, no swapchain, no callbacks, sync (no async compute)
        ffxConfigureDescFrameGeneration cfg{}; cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        cfg.swapChain = nullptr; cfg.presentCallback = nullptr; cfg.frameGenerationCallback = nullptr;
        cfg.frameGenerationEnabled = true; cfg.allowAsyncWorkloads = false; cfg.HUDLessColor = FfxApiResource{};
        cfg.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY; cfg.onlyPresentGenerated = false;
        cfg.generationRect = { 0, 0, (int32_t)e->W, (int32_t)e->H }; cfg.frameID = fid;
        r = ffx.configure(&e->fg, &cfg.header);
        if (r) { LOG("ffxConfigure(FG) = %u", r); e->cl->Close(); return -2; }
        // (2) prepare V2: depth (constant far plane), MVs (pixels, current -> previous), static camera
        ffxDispatchDescFrameGenerationPrepareV2 pp{}; pp.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        pp.frameID = fid; pp.flags = 0; pp.commandList = e->cl;
        pp.renderSize = { e->MW, e->MH }; pp.jitterOffset = { 0, 0 }; pp.motionVectorScale = { e->mvsx, e->mvsy };
        pp.frameTimeDelta = e->frameMs; pp.reset = reset;
        pp.cameraNear = e->nearZ; pp.cameraFar = e->farZ; pp.cameraFovAngleVertical = e->fov; pp.viewSpaceToMetersFactor = 1.0f;
        pp.depth = res(e->depth, FFX_API_RESOURCE_STATE_COMPUTE_READ); pp.motionVectors = res(e->mv, FFX_API_RESOURCE_STATE_COMPUTE_READ);
        pp.cameraPosition[0] = pp.cameraPosition[1] = pp.cameraPosition[2] = 0;
        pp.cameraUp[1] = 1; pp.cameraRight[0] = 1; pp.cameraForward[2] = 1;
        r = ffx.dispatch(&e->fg, &pp.header);
        if (r) { LOG("ffxDispatch(PrepareV2) = %u", r); e->cl->Close(); return -3; }
        // (3) frame generation, manual dispatch into our UAV
        ffxDispatchDescFrameGeneration fd{}; fd.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
        fd.commandList = e->cl; fd.presentColor = res(e->color[c], FFX_API_RESOURCE_STATE_COMPUTE_READ);
        fd.outputs[0] = res(e->outI, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS); fd.numGeneratedFrames = 1;
        fd.reset = reset; fd.backbufferTransferFunction = (uint32_t)e->transfer; fd.minMaxLuminance[0] = 0; fd.minMaxLuminance[1] = 1000;
        fd.generationRect = { 0, 0, (int32_t)e->W, (int32_t)e->H }; fd.frameID = fid;
        r = ffx.dispatch(&e->fg, &fd.header);
        if (r) { LOG("ffxDispatch(FrameGeneration) = %u", r); e->cl->Close(); return -4; }
    }
    e->cl->EndQuery(e->qh, D3D12_QUERY_TYPE_TIMESTAMP, 1);
    e->cl->ResolveQueryData(e->qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, e->qrb, 0);
    if (readback) {
        barrier(e->cl, e->outI, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (e->gout) { barrier(e->cl, e->gout, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST); e->cl->CopyResource(e->gout, e->outI); barrier(e->cl, e->gout, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON); }
        else texToBuf(e->cl, e->outI, e->rb, e->W, e->H, e->fmt, 4);
        barrier(e->cl, e->outI, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (!submit(e)) return -5;
    QueryPerformanceCounter(&t2); e->stWait = double(t2.QuadPart - t1.QuadPart) * 1000.0 / e->qf.QuadPart;
    { UINT64* ts = nullptr; D3D12_RANGE rr{ 0, 16 }; if (SUCCEEDED(e->qrb->Map(0, &rr, (void**)&ts))) { e->stGpuEval = double(ts[1] - ts[0]) * 1000.0 / e->tsFreq; D3D12_RANGE wr{ 0, 0 }; e->qrb->Unmap(0, &wr); } }
    e->cur = c;
    return 0;
}

extern "C" __declspec(dllexport) int fg_interpolate(Engine* e, const uint8_t* a, const uint8_t* b, const void* mvHalf2, uint64_t idA, uint64_t idB, uint8_t* out, double* msOut) {
    if (!e || !e->fg) return -1;
    LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
    if (e->lastId != idA) {                       // history break: feed A with reset, output ignored
        int rc = feed(e, a, nullptr, true, false); if (rc) return rc;
    }
    int rc = feed(e, b, mvHalf2, false, true); if (rc) return rc;
    e->lastId = idB;
    UINT cp = pitchOf(e->W * 4);
    LARGE_INTEGER tr; QueryPerformanceCounter(&tr);
    copyRows(out, e->W * 4, e->rbPtr, cp, e->W * 4, e->H);
    QueryPerformanceCounter(&t1); e->stRead = double(t1.QuadPart - tr.QuadPart) * 1000.0 / e->qf.QuadPart;
    if (msOut) *msOut = double(t1.QuadPart - t0.QuadPart) * 1000.0 / e->qf.QuadPart;
    return 0;
}

// last call breakdown: [0]=upload/record ms (CPU), [1]=prepare+FG GPU ms (timestamps), [2]=submit+wait ms, [3]=readback copy ms
extern "C" __declspec(dllexport) void fg_stats(Engine* e, double* o) { o[0] = e->stUpload; o[1] = e->stGpuEval; o[2] = e->stWait; o[3] = e->stRead; }

// ---------------------------------------------------------------- GPU path
// The application shares its D3D11 textures (NT handles); they are opened here as D3D12 resources and copied on the GPU,
// so no CPU upload or readback. Same evaluation as fg_interpolate (midpoint, zero motion vectors).
extern "C" __declspec(dllexport) int fg_gpu_luid(Engine* e, uint8_t* luid8) { if (!e || !e->dev) return -1; LUID l = e->dev->GetAdapterLuid(); memcpy(luid8, &l, 8); return 0; }
extern "C" __declspec(dllexport) int fg_gpu_import(Engine* e, HANDLE h) {
    if (!e || e->gslots >= 8) return -1; ID3D12Resource* r = nullptr; HRESULT hr = e->dev->OpenSharedHandle(h, IID_PPV_ARGS(&r));
    if (FAILED(hr) || !r) { LOG("gpu import 0x%08lx", (unsigned long)hr); return -2; }
    e->gslot[e->gslots] = r; return e->gslots++;
}
extern "C" __declspec(dllexport) int fg_gpu_generate(Engine* e, int a, uint64_t idA, int b, uint64_t idB, int out, double* msOut) {
    if (!e || a < 0 || b < 0 || out < 0 || a >= e->gslots || b >= e->gslots || out >= e->gslots) return -1;
    LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0); int rc = 0;
    if (e->lastId != idA) { e->gsrc = e->gslot[a]; e->gout = nullptr; rc = feed(e, nullptr, nullptr, true, false); }
    if (!rc) { e->gsrc = e->gslot[b]; e->gout = e->gslot[out]; rc = feed(e, nullptr, nullptr, false, true); }
    e->gsrc = e->gout = nullptr; if (rc) { e->lastId = ~0ull; return rc; }
    e->lastId = idB; QueryPerformanceCounter(&t1); if (msOut) *msOut = double(t1.QuadPart - t0.QuadPart) * 1000.0 / e->qf.QuadPart;
    return 0;
}
