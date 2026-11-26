// SPDX-License-Identifier: GPL-3.0-only
// Offscreen NVIDIA DLSS Frame Generation (nvngx_dlssg.dll) through the public NGX DLSSG API.
// Public DLSS SDK v310.9.16 headers only (sdk\include).
// NGX core (_nvngx.dll) is taken from the display driver (DriverStore) and its exports are called
//   directly (the public nvsdk_ngx_loader.h does the same LoadLibrary lookup). The MSVC static lib
//   nvsdk_ngx_d.lib cannot be linked with zig/mingw, so the few lib-side helpers
//   (NVSDK_NGX_Parameter_Set*, *_EvaluateFeature_C) are implemented below on top of the core exports.
// nvngx_dlssg.dll is never linked; NGX core loads it from the feature path list (rt\).
//
// C API (ctypes):
//   void* dg_create(const wchar_t* rtDir, const wchar_t* logPath, int w, int h, int mvW, int mvH, int fmt)
//   int   dg_interpolate(void* e, const uint8_t* a, const uint8_t* b, const void* mvHalf2, uint64_t idA, uint64_t idB, uint8_t* out, double* msOut)
//   int   dg_set_f(void* e, const char* key, float v)   // tuning knobs: mvscale_x/y, depth, near, far, fov, inv_depth, cam_motion
//   void  dg_destroy(void* e)
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
#include <vector>

#include "nvsdk_ngx.h"
#include "nvsdk_ngx_defs_dlssg.h"
#include "nvsdk_ngx_params_dlssg.h"
#include "nvsdk_ngx_helpers_dlssg_d3d.h"

static FILE* g_log = nullptr;
#define LOG(...) do { if (g_log) { fprintf(g_log, __VA_ARGS__); fputc('\n', g_log); fflush(g_log); } } while (0)

// ---------------------------------------------------------------- NGX core (driver) entry points
// Core (driver) export order is (..., device, version, featureInfo): the SDK version comes BEFORE the feature info,
// unlike the app-facing C++ overload in nvsdk_ngx.h. Found by a crash reading address 0x15 (= NVSDK_NGX_Version_API).
typedef NVSDK_NGX_Result (NVSDK_CONV* PFN_Init)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
typedef NVSDK_NGX_Result (NVSDK_CONV* PFN_InitPID)(const char*, NVSDK_NGX_EngineType, const char*, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
typedef NVSDK_NGX_Result (NVSDK_CONV* PFN_Shutdown1)(ID3D12Device*);
typedef NVSDK_NGX_Result (NVSDK_CONV* PFN_Params)(NVSDK_NGX_Parameter**);
typedef NVSDK_NGX_Result (NVSDK_CONV* PFN_DestroyParams)(NVSDK_NGX_Parameter*);
typedef NVSDK_NGX_Result (NVSDK_CONV* PFN_Create)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
typedef NVSDK_NGX_Result (NVSDK_CONV* PFN_Eval)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, void*);
typedef NVSDK_NGX_Result (NVSDK_CONV* PFN_Release)(NVSDK_NGX_Handle*);
static struct {
    HMODULE mod; PFN_Init init; PFN_InitPID initPid; PFN_Shutdown1 shutdown1; PFN_Params capParams, allocParams; PFN_DestroyParams destroyParams;
    PFN_Create create; PFN_Eval eval; PFN_Release release;
} ngx;

// NVSDK_NGX_Parameter is an MSVC-compiled C++ object. MSVC groups overloaded virtuals and lays them out in
// reverse declaration order, so the vtable is: Set(void*), Set(D3D12), Set(D3D11), Set(int), Set(uint),
// Set(double), Set(float), Set(ull), Get(void**) ... Get(ull*), Reset. Verified at runtime (selfTestParams).
enum { VS_PTR = 0, VS_D3D12 = 1, VS_D3D11 = 2, VS_I = 3, VS_UI = 4, VS_D = 5, VS_F = 6, VS_ULL = 7,
       VG_PTR = 8, VG_D3D12 = 9, VG_D3D11 = 10, VG_I = 11, VG_UI = 12, VG_D = 13, VG_F = 14, VG_ULL = 15, V_RESET = 16 };
template <typename T> static void pset(NVSDK_NGX_Parameter* p, int slot, const char* n, T v) {
    typedef void (*Fn)(void*, const char*, T); (*(Fn**)p)[slot]((void*)p, n, v);
}
template <typename T> static NVSDK_NGX_Result pget(const NVSDK_NGX_Parameter* p, int slot, const char* n, T* v) {
    typedef NVSDK_NGX_Result (*Fn)(const void*, const char*, T*); return (*(Fn**)p)[slot]((const void*)p, n, v);
}

// Lib-side helpers that the public helper headers call (normally inside nvsdk_ngx_d.lib).
extern "C" {
void NVSDK_CONV NVSDK_NGX_Parameter_SetULL(NVSDK_NGX_Parameter* p, const char* n, unsigned long long v) { pset(p, VS_ULL, n, v); }
void NVSDK_CONV NVSDK_NGX_Parameter_SetF(NVSDK_NGX_Parameter* p, const char* n, float v) { pset(p, VS_F, n, v); }
void NVSDK_CONV NVSDK_NGX_Parameter_SetD(NVSDK_NGX_Parameter* p, const char* n, double v) { pset(p, VS_D, n, v); }
void NVSDK_CONV NVSDK_NGX_Parameter_SetUI(NVSDK_NGX_Parameter* p, const char* n, unsigned int v) { pset(p, VS_UI, n, v); }
void NVSDK_CONV NVSDK_NGX_Parameter_SetI(NVSDK_NGX_Parameter* p, const char* n, int v) { pset(p, VS_I, n, v); }
void NVSDK_CONV NVSDK_NGX_Parameter_SetD3d11Resource(NVSDK_NGX_Parameter* p, const char* n, ID3D11Resource* v) { pset(p, VS_D3D11, n, v); }
void NVSDK_CONV NVSDK_NGX_Parameter_SetD3d12Resource(NVSDK_NGX_Parameter* p, const char* n, ID3D12Resource* v) { pset(p, VS_D3D12, n, v); }
void NVSDK_CONV NVSDK_NGX_Parameter_SetVoidPointer(NVSDK_NGX_Parameter* p, const char* n, void* v) { pset(p, VS_PTR, n, v); }
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_Parameter_GetUI(NVSDK_NGX_Parameter* p, const char* n, unsigned int* v) { return pget(p, VG_UI, n, v); }
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_Parameter_GetI(NVSDK_NGX_Parameter* p, const char* n, int* v) { return pget(p, VG_I, n, v); }
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_Parameter_GetVoidPointer(NVSDK_NGX_Parameter* p, const char* n, void** v) { return pget(p, VG_PTR, n, v); }
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList* cl, NVSDK_NGX_Feature f, NVSDK_NGX_Parameter* p, NVSDK_NGX_Handle** h) { return ngx.create(cl, f, p, h); }
NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature_C(ID3D12GraphicsCommandList* cl, const NVSDK_NGX_Handle* h, const NVSDK_NGX_Parameter* p, PFN_NVSDK_NGX_ProgressCallback_C) { return ngx.eval(cl, h, p, nullptr); }
}

static void NVSDK_CONV ngxLog(const char* msg, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature f) {
    if (g_log) { fprintf(g_log, "[ngx f%d] %s", (int)f, msg); size_t n = strlen(msg); if (!n || msg[n - 1] != '\n') fputc('\n', g_log); fflush(g_log); }
}

static std::wstring findCore() {
    // Same order as the public nvsdk_ngx_loader.h: registry NGXCore path -> _nvngx.dll
    const wchar_t* keys[][2] = { { L"System\\CurrentControlSet\\Services\\nvlddmkm\\Parameters\\NGXCore", L"NGXPath" },
                                 { L"System\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore", L"NGXPath" },
                                 { L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath" } };
    for (auto& k : keys) {
        HKEY h; if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, k[0], 0, KEY_READ, &h) != ERROR_SUCCESS) continue;
        wchar_t buf[MAX_PATH] = {}; DWORD sz = sizeof(buf);
        LONG r = RegQueryValueExW(h, k[1], nullptr, nullptr, (LPBYTE)buf, &sz); RegCloseKey(h);
        if (r == ERROR_SUCCESS && buf[0]) { std::wstring p = std::wstring(buf) + L"\\_nvngx.dll"; if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p; }
    }
    // Fallback: driver store folder of the loaded nvlddmkm (search DriverStore)
    WIN32_FIND_DATAW fd; HANDLE fh = FindFirstFileW(L"C:\\Windows\\System32\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_*", &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do { std::wstring p = L"C:\\Windows\\System32\\DriverStore\\FileRepository\\" + std::wstring(fd.cFileName) + L"\\_nvngx.dll";
             if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) { FindClose(fh); return p; } } while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    return L"";
}

// ---------------------------------------------------------------- D3D12 helpers
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

// ---------------------------------------------------------------- engine
static const D3D12_RESOURCE_STATES SRV = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
struct Engine {
    UINT W, H, MW, MH; DXGI_FORMAT fmt;
    ID3D12Device* dev = nullptr; ID3D12CommandQueue* q = nullptr; ID3D12CommandAllocator* ca = nullptr; ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12Fence* fence = nullptr; UINT64 fv = 0; HANDLE ev = nullptr;
    ID3D12Resource* color[2] = {}; ID3D12Resource* mv = nullptr; ID3D12Resource* depth = nullptr; ID3D12Resource* outI = nullptr;
    ID3D12Resource* up = nullptr; ID3D12Resource* rb = nullptr; uint8_t* upPtr = nullptr; uint8_t* rbPtr = nullptr;
    UINT64 offColor = 0, offMv = 0, offDepth = 0;
    NVSDK_NGX_Parameter* params = nullptr; NVSDK_NGX_Handle* fg = nullptr;
    unsigned mfMax = 1; uint64_t lastA = 0; bool havePair = false; // multi-frame
    int cur = 0; uint64_t lastId = ~0ull; uint64_t frameId = 1; bool zeroMvUploaded = false; bool depthDirty = true;
    float mvsx = 1.f, mvsy = 1.f, depthVal = 0.5f, nearZ = 0.1f, farZ = 1000.f, fov = 1.0f; int invDepth = 0, camMotion = 1, neverFlags = 1, menuDet = 0, setFrameId = 1;
    LARGE_INTEGER qf;
    ID3D12QueryHeap* qh = nullptr; ID3D12Resource* qrb = nullptr; UINT64 tsFreq = 1;
    double stUpload = 0, stGpuEval = 0, stWait = 0, stRead = 0;
    ID3D12Resource* gsrc = nullptr; ID3D12Resource* gout = nullptr; ID3D12Resource* gslot[8] = {}; int gslots = 0; // GPU path (shared D3D11 textures) // last call breakdown (ms)
};

static bool waitGpu(Engine* e) {
    e->q->Signal(e->fence, ++e->fv);
    if (e->fence->GetCompletedValue() < e->fv) { e->fence->SetEventOnCompletion(e->fv, e->ev); WaitForSingleObject(e->ev, 5000); }
    HRESULT r = e->dev->GetDeviceRemovedReason(); if (FAILED(r)) { LOG("device removed 0x%08lx", (unsigned long)r); return false; }
    return true;
}
static void beginCl(Engine* e) { e->ca->Reset(); e->cl->Reset(e->ca, nullptr); }
static bool submit(Engine* e) {
    HRESULT hr = e->cl->Close(); if (FAILED(hr)) { LOG("cl close 0x%08lx", (unsigned long)hr); return false; }
    ID3D12CommandList* l[] = { e->cl }; e->q->ExecuteCommandLists(1, l); return waitGpu(e);
}

static bool selfTestParams(NVSDK_NGX_Parameter* p) {
    pset(p, VS_UI, "cs.test.ui", 42u); pset(p, VS_F, "cs.test.f", 0.25f); pset(p, VS_ULL, "cs.test.ull", 1ull << 40); pset(p, VS_PTR, "cs.test.p", (void*)p);
    unsigned int ui = 0; float f = 0; unsigned long long ull = 0; void* vp = nullptr;
    NVSDK_NGX_Result r1 = pget(p, VG_UI, "cs.test.ui", &ui), r2 = pget(p, VG_F, "cs.test.f", &f), r3 = pget(p, VG_ULL, "cs.test.ull", &ull), r4 = pget(p, VG_PTR, "cs.test.p", &vp);
    LOG("param selftest ui=%u(%x) f=%g(%x) ull=%llu(%x) ptr ok=%d(%x)", ui, r1, f, r2, ull, r3, vp == (void*)p, r4);
    return ui == 42 && f == 0.25f && ull == (1ull << 40) && vp == (void*)p;
}

static void logModules() {
    HMODULE mods[512]; DWORD n = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &n)) {
        for (DWORD i = 0; i < n / sizeof(HMODULE); i++) {
            wchar_t nm[MAX_PATH]; GetModuleFileNameW(mods[i], nm, MAX_PATH);
            if (wcsstr(nm, L"ngx") || wcsstr(nm, L"NGX") || wcsstr(nm, L".bin")) LOG("module: %ls", nm);
        }
    }
}

extern "C" __declspec(dllexport) int dg_set_f(Engine* e, const char* k, float v) {
    if (!strcmp(k, "mvscale_x")) e->mvsx = v; else if (!strcmp(k, "mvscale_y")) e->mvsy = v;
    else if (!strcmp(k, "depth")) { e->depthVal = v; e->depthDirty = true; }
    else if (!strcmp(k, "near")) e->nearZ = v; else if (!strcmp(k, "far")) e->farZ = v; else if (!strcmp(k, "fov")) e->fov = v;
    else if (!strcmp(k, "inv_depth")) e->invDepth = (int)v; else if (!strcmp(k, "cam_motion")) e->camMotion = (int)v;
    else if (!strcmp(k, "menu_detect")) e->menuDet = (int)v; else if (!strcmp(k, "set_frame_id")) e->setFrameId = (int)v;
    else return -1;
    LOG("set %s=%g", k, v); return 0;
}

extern "C" __declspec(dllexport) void dg_destroy(Engine* e) {
    if (!e) return;
    if (e->q && e->fence) { e->q->Signal(e->fence, ++e->fv); if (e->fence->GetCompletedValue() < e->fv) { e->fence->SetEventOnCompletion(e->fv, e->ev); WaitForSingleObject(e->ev, 5000); } }
    LOG("destroy: gpu idle");
    if (e->fg) { ngx.release(e->fg); e->fg = nullptr; } LOG("destroy: feature released");
    if (getenv("DG_DESTROY_PARAMS") && e->params && ngx.destroyParams) ngx.destroyParams(e->params); LOG("destroy: params");
    // NGX is process-wide: Shutdown1 from a second engine (two may exist) crashed on close. The
    // feature is released above; NGX itself is left to process exit unless DG_SHUTDOWN is set.
    if (getenv("DG_SHUTDOWN") && ngx.shutdown1 && e->dev) ngx.shutdown1(e->dev); LOG("destroy: done");
    // D3D objects are leaked on purpose (test process exits right after)
    LOG("destroyed");
}

extern "C" __declspec(dllexport) Engine* dg_create(const wchar_t* rtDir, const wchar_t* logPath, int w, int h, int mvW, int mvH, int fmtCode) {
    if (!g_log && logPath) g_log = _wfopen(logPath, L"w");
    Engine* e = new Engine(); e->W = w; e->H = h; e->MW = mvW; e->MH = mvH;
    e->fmt = fmtCode == 1 ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    QueryPerformanceFrequency(&e->qf);

    // 1) D3D12 device on the high-performance adapter
    IDXGIFactory6* fac = nullptr; if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)))) { LOG("dxgi factory fail"); return nullptr; }
    IDXGIAdapter1* ad = nullptr; fac->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad));
    DXGI_ADAPTER_DESC1 add{}; ad->GetDesc1(&add); LOG("adapter: %ls", add.Description);
    if (FAILED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&e->dev)))) { LOG("device fail"); return nullptr; }
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    e->dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&e->q));
    e->dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&e->ca));
    e->dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, e->ca, nullptr, IID_PPV_ARGS(&e->cl)); e->cl->Close();
    e->dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&e->fence)); e->ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // 2) NGX core from the driver
    std::wstring core = _wgetenv(L"DG_CORE") ? _wgetenv(L"DG_CORE") : findCore();
    LOG("core: %ls", core.c_str());
    ngx.mod = LoadLibraryW(core.c_str()); if (!ngx.mod) { LOG("LoadLibrary core fail %lu", GetLastError()); return nullptr; }
    ngx.init = (PFN_Init)GetProcAddress(ngx.mod, "NVSDK_NGX_D3D12_Init");
    ngx.initPid = (PFN_InitPID)GetProcAddress(ngx.mod, "NVSDK_NGX_D3D12_Init_ProjectID");
    ngx.shutdown1 = (PFN_Shutdown1)GetProcAddress(ngx.mod, "NVSDK_NGX_D3D12_Shutdown1");
    ngx.capParams = (PFN_Params)GetProcAddress(ngx.mod, "NVSDK_NGX_D3D12_GetCapabilityParameters");
    ngx.allocParams = (PFN_Params)GetProcAddress(ngx.mod, "NVSDK_NGX_D3D12_AllocateParameters");
    ngx.destroyParams = (PFN_DestroyParams)GetProcAddress(ngx.mod, "NVSDK_NGX_D3D12_DestroyParameters");
    ngx.create = (PFN_Create)GetProcAddress(ngx.mod, "NVSDK_NGX_D3D12_CreateFeature");
    ngx.eval = (PFN_Eval)GetProcAddress(ngx.mod, "NVSDK_NGX_D3D12_EvaluateFeature");
    ngx.release = (PFN_Release)GetProcAddress(ngx.mod, "NVSDK_NGX_D3D12_ReleaseFeature");
    if (!ngx.init || !ngx.initPid || !ngx.capParams || !ngx.create || !ngx.eval) { LOG("missing core export"); return nullptr; }

    static std::wstring rt; rt = rtDir; static const wchar_t* paths[1]; paths[0] = rt.c_str();
    static NVSDK_NGX_FeatureCommonInfo fci; memset(&fci, 0, sizeof fci);
    fci.PathListInfo.Path = paths; fci.PathListInfo.Length = 1;
    fci.LoggingInfo.LoggingCallback = ngxLog; fci.LoggingInfo.MinimumLoggingLevel = _wgetenv(L"DG_VERBOSE") ? NVSDK_NGX_LOGGING_LEVEL_VERBOSE : NVSDK_NGX_LOGGING_LEVEL_ON;
    fci.LoggingInfo.DisableOtherLoggingSinks = false;
    std::wstring dataPath = rt; // logs/temporary data next to the runtime copy
    NVSDK_NGX_Result r;
    const char* mode = getenv("DG_INIT") ? getenv("DG_INIT") : "project";
    if (!strcmp(mode, "appid")) {
        unsigned long long appId = getenv("DG_APPID") ? _strtoui64(getenv("DG_APPID"), nullptr, 0) : 0x24480451ull; // generic test id
        r = ngx.init(appId, dataPath.c_str(), e->dev, NVSDK_NGX_Version_API, &fci);
        LOG("NVSDK_NGX_D3D12_Init(appId=0x%llx) = 0x%08x", appId, (unsigned)r);
    } else {
        const char* pid = getenv("DG_PROJECT") ? getenv("DG_PROJECT") : "6b3f2d1e-5a4c-4e8b-9f21-0c7d1e2a3b4c";
        r = ngx.initPid(pid, NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0", dataPath.c_str(), e->dev, NVSDK_NGX_Version_API, &fci);
        LOG("NVSDK_NGX_D3D12_Init_ProjectID(%s) = 0x%08x", pid, (unsigned)r);
    }
    if (NVSDK_NGX_FAILED(r)) return nullptr;

    r = ngx.capParams(&e->params); LOG("GetCapabilityParameters = 0x%08x", (unsigned)r);
    if (NVSDK_NGX_FAILED(r) || !e->params) return nullptr;
    if (!selfTestParams(e->params)) { LOG("parameter vtable layout mismatch"); return nullptr; }
    { int av = -1, need = -1, ir = -1; unsigned mj = 0, mn = 0, mfc = 0;
      pget(e->params, VG_I, NVSDK_NGX_Parameter_FrameGeneration_Available, &av);
      pget(e->params, VG_I, NVSDK_NGX_Parameter_FrameGeneration_NeedsUpdatedDriver, &need);
      pget(e->params, VG_I, NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &ir);
      pget(e->params, VG_UI, NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMajor, &mj);
      pget(e->params, VG_UI, NVSDK_NGX_Parameter_FrameGeneration_MinDriverVersionMinor, &mn);
      pget(e->params, VG_UI, NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &mfc);
      LOG("FG available=%d needsDriver=%d initResult=0x%x minDrv=%u.%u MultiFrameCountMax=%u", av, need, ir, mj, mn, mfc); e->mfMax = mfc ? mfc : 1; }

    // 3) resources
    for (int i = 0; i < 2; i++) e->color[i] = makeTex(e->dev, w, h, e->fmt, D3D12_RESOURCE_STATE_COPY_DEST, false);
    e->mv = makeTex(e->dev, mvW, mvH, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, false);
    e->depth = makeTex(e->dev, mvW, mvH, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_STATE_COPY_DEST, false);
    e->outI = makeTex(e->dev, w, h, e->fmt, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    UINT64 cs = (UINT64)pitchOf(w * 4) * h, ms = (UINT64)pitchOf(mvW * 4) * mvH;
    e->offColor = 0; e->offMv = (cs + 511) & ~511ull; e->offDepth = (e->offMv + ms + 511) & ~511ull;
    e->up = makeBuf(e->dev, e->offDepth + ms, D3D12_HEAP_TYPE_UPLOAD); e->up->Map(0, nullptr, (void**)&e->upPtr);
    e->rb = makeBuf(e->dev, cs, D3D12_HEAP_TYPE_READBACK); D3D12_RANGE rr{ 0, (SIZE_T)cs }; e->rb->Map(0, &rr, (void**)&e->rbPtr);
    { D3D12_QUERY_HEAP_DESC qhd{}; qhd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qhd.Count = 2; e->dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&e->qh));
      e->qrb = makeBuf(e->dev, 16, D3D12_HEAP_TYPE_READBACK); e->q->GetTimestampFrequency(&e->tsFreq); }
    if (!e->color[0] || !e->color[1] || !e->mv || !e->depth || !e->outI || !e->upPtr || !e->rbPtr) { LOG("resource alloc fail"); return nullptr; }
    // initial transitions: everything readable except output
    beginCl(e);
    memset(e->upPtr + e->offMv, 0, ms);
    bufToTex(e->cl, e->up, e->offMv, e->mv, mvW, mvH, DXGI_FORMAT_R16G16_FLOAT, 4);
    barrier(e->cl, e->mv, D3D12_RESOURCE_STATE_COPY_DEST, SRV);
    { float* dp = (float*)(e->upPtr + e->offDepth); UINT pp = pitchOf(mvW * 4) / 4; for (UINT y = 0; y < (UINT)mvH; y++) for (UINT x = 0; x < (UINT)mvW; x++) dp[y * pp + x] = e->depthVal; e->depthDirty = false; }
    bufToTex(e->cl, e->up, e->offDepth, e->depth, mvW, mvH, DXGI_FORMAT_R32_FLOAT, 4);
    barrier(e->cl, e->depth, D3D12_RESOURCE_STATE_COPY_DEST, SRV);
    for (int i = 0; i < 2; i++) barrier(e->cl, e->color[i], D3D12_RESOURCE_STATE_COPY_DEST, SRV);
    if (!submit(e)) return nullptr;

    // 4) create the DLSS-G feature
    unsigned never = NVSDK_NGX_DLSSG_ResourceFlags_HUDLess | NVSDK_NGX_DLSSG_ResourceFlags_UI | NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha | NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField;
    unsigned always = NVSDK_NGX_DLSSG_ResourceFlags_Backbuffer | NVSDK_NGX_DLSSG_ResourceFlags_MVecs | NVSDK_NGX_DLSSG_ResourceFlags_Depth | NVSDK_NGX_DLSSG_ResourceFlags_OutputInterpolated;
    if (!getenv("DG_NO_PROVIDED_FLAGS")) {
        pset(e->params, VS_UI, NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags, never);
        pset(e->params, VS_UI, NVSDK_NGX_DLSSG_Parameter_ResourceAlwaysProvided_Flags, always);
    }
    NVSDK_NGX_DLSSG_Create_Params cp{}; cp.Width = w; cp.Height = h; cp.NativeBackbufferFormat = e->fmt; cp.RenderWidth = mvW; cp.RenderHeight = mvH; cp.DynamicResolutionScaling = false;
    beginCl(e);
    r = NGX_D3D12_CREATE_DLSSG(e->cl, 1, 1, &e->fg, e->params, &cp);
    LOG("NGX_D3D12_CREATE_DLSSG %dx%d mv %dx%d fmt %d = 0x%08x handle=%p", w, h, mvW, mvH, (int)e->fmt, (unsigned)r, (void*)e->fg);
    if (!submit(e)) return nullptr;
    logModules();
    if (NVSDK_NGX_FAILED(r) || !e->fg) return nullptr;
    return e;
}

static void fillOpt(Engine* e, NVSDK_NGX_DLSSG_Opt_Eval_Params& o, bool reset, unsigned mfCount = 1, unsigned mfIndex = 1) {
    memset(&o, 0, sizeof o);
    o.multiFrameCount = mfCount; o.multiFrameIndex = mfIndex;
    float I[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
    memcpy(o.cameraViewToClip, I, sizeof I); memcpy(o.clipToCameraView, I, sizeof I); memcpy(o.clipToLensClip, I, sizeof I);
    memcpy(o.clipToPrevClip, I, sizeof I); memcpy(o.prevClipToClip, I, sizeof I);
    o.mvecScale[0] = e->mvsx; o.mvecScale[1] = e->mvsy;
    o.cameraUp[1] = 1; o.cameraRight[0] = 1; o.cameraFwd[2] = 1;
    o.cameraNear = e->nearZ; o.cameraFar = e->farZ; o.cameraFOV = e->fov; o.cameraAspectRatio = (float)e->W / e->H;
    o.depthInverted = e->invDepth != 0; o.cameraMotionIncluded = e->camMotion != 0; o.reset = reset;
    o.menuDetectionEnabled = e->menuDet != 0;
    o.minRelativeLinearDepthObjectSeparation = 40.0f;
    o.mvecsSubrectSize = { e->MW, e->MH }; o.depthSubrectSize = { e->MW, e->MH };
    o.backbufferSubrectSize = { e->W, e->H }; o.outputInterpSubrectSize = { e->W, e->H };
}

// Feed one rendered frame (color + mv); DLSS-G writes the frame between the previous feed and this one into outI.
// multi-frame: upload=false re-evaluates the same rendered frame for another multiFrameIndex (inputs unchanged).
static int feed(Engine* e, const uint8_t* bgra, const void* mvHalf2, bool reset, bool readback, unsigned mfCount = 1, unsigned mfIndex = 1, bool upload = true) {
    LARGE_INTEGER t0, t1, t2; QueryPerformanceCounter(&t0);
    beginCl(e);
    int c = upload ? e->cur ^ 1 : e->cur;
    if (upload) {
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
    } // upload
    NVSDK_NGX_D3D12_DLSSG_Eval_Params ep{}; ep.pBackbuffer = e->color[c]; ep.pDepth = e->depth; ep.pMVecs = e->mv; ep.pOutputInterpFrame = e->outI;
    NVSDK_NGX_DLSSG_Opt_Eval_Params o; fillOpt(e, o, reset, mfCount, mfIndex);
    if (e->setFrameId) pset(e->params, VS_ULL, NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, (unsigned long long)(upload ? e->frameId : e->frameId - 1));
    if (upload) e->frameId++;
    QueryPerformanceCounter(&t1); e->stUpload = double(t1.QuadPart - t0.QuadPart) * 1000.0 / e->qf.QuadPart;
    e->cl->EndQuery(e->qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
    NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSSG(e->cl, e->fg, e->params, &ep, &o);
    e->cl->EndQuery(e->qh, D3D12_QUERY_TYPE_TIMESTAMP, 1);
    e->cl->ResolveQueryData(e->qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, e->qrb, 0);
    if (NVSDK_NGX_FAILED(r)) { LOG("EVALUATE_DLSSG = 0x%08x", (unsigned)r); e->cl->Close(); return -2; }
    if (readback) {
        barrier(e->cl, e->outI, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (e->gout) { barrier(e->cl, e->gout, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST); e->cl->CopyResource(e->gout, e->outI); barrier(e->cl, e->gout, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON); }
        else texToBuf(e->cl, e->outI, e->rb, e->W, e->H, e->fmt, 4);
        barrier(e->cl, e->outI, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }
    if (!submit(e)) return -3;
    QueryPerformanceCounter(&t2); e->stWait = double(t2.QuadPart - t1.QuadPart) * 1000.0 / e->qf.QuadPart;
    { UINT64* ts = nullptr; D3D12_RANGE rr{ 0, 16 }; if (SUCCEEDED(e->qrb->Map(0, &rr, (void**)&ts))) { e->stGpuEval = double(ts[1] - ts[0]) * 1000.0 / e->tsFreq; D3D12_RANGE wr{ 0, 0 }; e->qrb->Unmap(0, &wr); } }
    e->cur = c;
    return 0;
}

extern "C" __declspec(dllexport) int dg_interpolate(Engine* e, const uint8_t* a, const uint8_t* b, const void* mvHalf2, uint64_t idA, uint64_t idB, uint8_t* out, double* msOut) {
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

// multi-frame generation. count = generated frames per pair (1 = 2x, 2 = 3x, 3 = 4x; RTX 50 up to MultiFrameCountMax),
// index = 1..count (frame at t = index/(count+1)). The first call for a new pair uploads b; later indices reuse it.
extern "C" __declspec(dllexport) int dg_interpolate_mf(Engine* e, const uint8_t* a, const uint8_t* b, const void* mvHalf2, uint64_t idA, uint64_t idB,
                                                       unsigned count, unsigned index, uint8_t* out, double* msOut) {
    LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0);
    if (!count || index < 1 || index > count || count > e->mfMax) return -4;
    bool samePair = e->lastId == idB && e->havePair && e->lastA == idA;
    if (!samePair) {
        if (e->lastId != idA) { int rc = feed(e, a, nullptr, true, false, count, 1); if (rc) return rc; }
        int rc = feed(e, b, mvHalf2, false, true, count, index); if (rc) return rc;
        e->lastId = idB; e->lastA = idA; e->havePair = true;
    } else { int rc = feed(e, b, mvHalf2, false, true, count, index, false); if (rc) return rc; }
    UINT cp = pitchOf(e->W * 4);
    copyRows(out, e->W * 4, e->rbPtr, cp, e->W * 4, e->H);
    QueryPerformanceCounter(&t1); if (msOut) *msOut = double(t1.QuadPart - t0.QuadPart) * 1000.0 / e->qf.QuadPart;
    return 0;
}
extern "C" __declspec(dllexport) unsigned dg_mf_max(Engine* e) { return e ? e->mfMax : 0; }

// last call breakdown: [0]=upload/record ms (CPU), [1]=DLSS-G evaluate GPU ms (timestamps), [2]=submit+wait ms, [3]=readback copy ms
extern "C" __declspec(dllexport) void dg_stats(Engine* e, double* o) { o[0] = e->stUpload; o[1] = e->stGpuEval; o[2] = e->stWait; o[3] = e->stRead; }

// ---------------------------------------------------------------- GPU path
// The application shares its D3D11 textures (NT handles); they are opened here as D3D12 resources and copied on the GPU,
// so no CPU upload or readback. Same evaluation as dg_interpolate (midpoint, zero motion vectors).
extern "C" __declspec(dllexport) int dg_gpu_luid(Engine* e, uint8_t* luid8) { if (!e || !e->dev) return -1; LUID l = e->dev->GetAdapterLuid(); memcpy(luid8, &l, 8); return 0; }
extern "C" __declspec(dllexport) int dg_gpu_import(Engine* e, HANDLE h) {
    if (!e || e->gslots >= 8) return -1; ID3D12Resource* r = nullptr; HRESULT hr = e->dev->OpenSharedHandle(h, IID_PPV_ARGS(&r));
    if (FAILED(hr) || !r) { LOG("gpu import 0x%08lx", (unsigned long)hr); return -2; }
    e->gslot[e->gslots] = r; return e->gslots++;
}
extern "C" __declspec(dllexport) int dg_gpu_generate(Engine* e, int a, uint64_t idA, int b, uint64_t idB, int out, double* msOut) {
    if (!e || a < 0 || b < 0 || out < 0 || a >= e->gslots || b >= e->gslots || out >= e->gslots) return -1;
    LARGE_INTEGER t0, t1; QueryPerformanceCounter(&t0); int rc = 0;
    if (e->lastId != idA) { e->gsrc = e->gslot[a]; e->gout = nullptr; rc = feed(e, nullptr, nullptr, true, false); }
    if (!rc) { e->gsrc = e->gslot[b]; e->gout = e->gslot[out]; rc = feed(e, nullptr, nullptr, false, true); }
    e->gsrc = e->gout = nullptr; if (rc) { e->lastId = ~0ull; return rc; }
    e->lastId = idB; QueryPerformanceCounter(&t1); if (msOut) *msOut = double(t1.QuadPart - t0.QuadPart) * 1000.0 / e->qf.QuadPart;
    return 0;
}
