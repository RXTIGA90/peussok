// SPDX-License-Identifier: GPL-3.0-only
// NVIDIA Video Frame Generation (NVIDIA Video Effects SDK) as a SeqEngine.
// No NVIDIA binary is shipped: the runtime is the one the user installed (nvidia-vfx from
// pypi.nvidia.com, folder ...\site-packages\nvvfx\libs) or a folder named by PK_VFG_DIR.
// API declarations follow the MIT-licensed NVIDIA Broadcast VFX SDK headers and the MIT
// NVIDIA-Maxine/VFX-SDK-Samples VideoFrameGenerationEffectApp (see engine/licenses/nvidia_vfx_*.txt).
// Host BGRA/BGR in and out; each source is uploaded once into a two-slot GPU cache (tagged ids).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <softpub.h>
#include <wintrust.h>
#include "seq_engines.h"
#include <string>
#include <vector>
#include <memory>
#include <chrono>
#include <cstring>
#include <cmath>
#include <mutex>
#include <cstdio>

static void vfgLog(const std::string& m);
namespace {
typedef int NvCV_Status;
struct CUstream_st;
typedef CUstream_st* CUstream;
struct NvVFX_Object;
typedef NvVFX_Object* NvVFX_Handle;
// NvCVImage descriptor (layout as in the MIT nvCVImage.h).
struct NvCVImage {
 unsigned int width, height; signed int pitch; int pixelFormat, componentType;
 unsigned char pixelBytes, componentBytes, numComponents, planar, gpuMem, colorspace, reserved[2];
 void* pixels; void* deletePtr; void (*deleteProc)(void*); unsigned long long bufferBytes;
};
enum { NVCV_BGR = 5, NVCV_RGBA = 6, NVCV_BGRA = 7, NVCV_U8 = 1, NVCV_CHUNKY = 0, NVCV_CPU = 0, NVCV_GPU = 1 };

struct Api {
 HMODULE core = nullptr;
 NvCV_Status (*create)(const char*, NvVFX_Handle*) = nullptr;
 void (*destroy)(NvVFX_Handle) = nullptr;
 NvCV_Status (*setU32)(NvVFX_Handle, const char*, unsigned) = nullptr;
 NvCV_Status (*setF32)(NvVFX_Handle, const char*, float) = nullptr;
 NvCV_Status (*setImage)(NvVFX_Handle, const char*, NvCVImage*) = nullptr;
 NvCV_Status (*setStream)(NvVFX_Handle, const char*, CUstream) = nullptr;
 NvCV_Status (*load)(NvVFX_Handle) = nullptr;
 NvCV_Status (*run)(NvVFX_Handle, int) = nullptr;
 NvCV_Status (*streamCreate)(CUstream*) = nullptr;
 NvCV_Status (*streamDestroy)(CUstream) = nullptr;
 NvCV_Status (*streamSync)(CUstream) = nullptr;
 NvCV_Status (*imgAlloc)(NvCVImage*, unsigned, unsigned, int, int, unsigned, unsigned, unsigned) = nullptr;
 void (*imgDealloc)(NvCVImage*) = nullptr;
 NvCV_Status (*imgInit)(NvCVImage*, unsigned, unsigned, int, void*, int, int, unsigned, unsigned) = nullptr;
 NvCV_Status (*imgTransfer)(const NvCVImage*, NvCVImage*, float, CUstream, NvCVImage*) = nullptr;
};

bool signedFile(const std::wstring& path) {
 WINTRUST_FILE_INFO file{sizeof(file)}; file.pcwszFilePath = path.c_str(); GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
 WINTRUST_DATA data{sizeof(data)}; data.dwUIChoice = WTD_UI_NONE; data.fdwRevocationChecks = WTD_REVOKE_NONE; data.dwUnionChoice = WTD_CHOICE_FILE;
 data.pFile = &file; data.dwStateAction = WTD_STATEACTION_VERIFY;
 LONG r = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &data);
 data.dwStateAction = WTD_STATEACTION_CLOSE; WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &action, &data); return r == 0;
}
bool exists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }

// PK_VFG_DIR, else %APPDATA%\Python\Python3xx\site-packages\nvvfx\libs (pip --user install).
std::wstring findRuntime() {
 wchar_t v[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"PK_VFG_DIR", v, MAX_PATH);
 if (n && n < MAX_PATH && exists(std::wstring(v) + L"\\NVVideoEffects.dll")) return v;
 // One-click download target (engines\nvvfg\Get-NvidiaVfg.ps1).
 n = GetEnvironmentVariableW(L"LOCALAPPDATA", v, MAX_PATH);
 if (n && n < MAX_PATH) { std::wstring c = std::wstring(v) + L"\\Peussok\\nvidia-vfg"; if (exists(c + L"\\NVVideoEffects.dll")) return c; }
 n = GetEnvironmentVariableW(L"APPDATA", v, MAX_PATH); if (!n || n >= MAX_PATH) return L"";
 std::wstring base = std::wstring(v) + L"\\Python\\";
 WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((base + L"Python3*").c_str(), &fd); std::wstring found;
 if (h != INVALID_HANDLE_VALUE) {
  do { std::wstring c = base + fd.cFileName + L"\\site-packages\\nvvfx\\libs"; if (exists(c + L"\\NVVideoEffects.dll")) found = c; } while (FindNextFileW(h, &fd));
  FindClose(h);
 }
 return found;
}

struct Vfg final : SeqEngine {
 Api api; NvVFX_Handle fx = nullptr; CUstream stream = nullptr; int w = 0, h = 0; unsigned mode = 1;  // MEDIUM (was HIGH = 2, ~4x the GPU time; 4070-class 3.6% live). The benchmark used MEDIUM.
 NvCVImage src[2]{}, dst{}, tmp{}; uint64_t srcId[2]{}; int next = 0;
 std::vector<uint8_t> host;
 ~Vfg() { release(); if (stream && api.streamDestroy) api.streamDestroy(stream); }
 void release() {
  if (fx) { api.destroy(fx); fx = nullptr; }
  for (auto& s : src) if (s.pixels) api.imgDealloc(&s);
  if (dst.pixels) api.imgDealloc(&dst); if (tmp.pixels) api.imgDealloc(&tmp);
  memset(src, 0, sizeof src); memset(&dst, 0, sizeof dst); memset(&tmp, 0, sizeof tmp); srcId[0] = srcId[1] = 0; w = h = 0;
 }
 bool setup(int fw, int fh) {
  release(); w = fw; h = fh;
  for (auto& s : src) if (api.imgAlloc(&s, w, h, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 32)) return false;
  if (api.imgAlloc(&dst, w, h, NVCV_RGBA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 32)) return false;
  if (api.create("VideoFrameGeneration", &fx)) { fx = nullptr; return false; }
  if (api.setStream(fx, "CudaStream", stream) || api.setU32(fx, "InputWidth", w) || api.setU32(fx, "InputHeight", h) ||
      api.setU32(fx, "Mode", mode) || api.setU32(fx, "AutomaticShotChangeDetectionEnabled", 0) || api.load(fx) ||
      api.setImage(fx, "DstImage0", &dst) || api.setU32(fx, "FrameMultiplier", 0)) return false;
  return true;
 }
 // Host frame (ch 3 BGR / 4 BGRA) -> GPU RGBA slot; the SDK converts during the transfer.
 // never write into the slot the other side of this pair uses (3x/4x reuse an older source:
 // with A cached, the round-robin slot could be A's own slot, and B overwrote it).
 int upload(const uint8_t* f, int ch, uint64_t id, int avoidSlot, uint64_t avoidId) {
  if (id) for (int k = 0; k < 2; k++) if (srcId[k] == id) return k;
  int k = next;
  if (avoidSlot >= 0) k = 1 - avoidSlot; else if (avoidId && srcId[k] == avoidId) k ^= 1;
  next = k ^ 1; NvCVImage view{};
  if (api.imgInit(&view, w, h, w * ch, const_cast<uint8_t*>(f), ch == 4 ? NVCV_BGRA : NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_CPU)) return -1;
  if (api.imgTransfer(&view, &src[k], 1.f, stream, &tmp)) return -1;
  srcId[k] = id; return k;
 }
 int notes = 0;
 int ka_dbg() const { return -1; }
 void note(const char* why, uint64_t aid, uint64_t bid, float t, int x, int y) {
  if (notes++ < 40) vfgLog(std::string("vfg_fail ") + why + " aid=" + std::to_string(aid) + " bid=" + std::to_string(bid) + " t=" + std::to_string(t) + " x=" + std::to_string(x) + " y=" + std::to_string(y) + " src=" + std::to_string(srcId[0]) + "," + std::to_string(srcId[1]));
 }
 std::mutex busy; // one SDK effect + 2 slots: keep calls serialized (cheap safety; the 3x failures were the slot bug above)
 int process(const uint8_t* a, const uint8_t* b, int fw, int fh, int ch, float t, uint8_t* o, uint64_t aid, uint64_t bid) override {
  std::lock_guard<std::mutex> hold(busy);
  if (t <= 0.f || t >= 1.f) { memcpy(o, t <= 0.f ? a : b, (size_t)fw * fh * ch); wait = 0; return 0; }
  if ((fw != w || fh != h) && !setup(fw, fh)) { release(); return -2; }
  auto t0 = std::chrono::steady_clock::now();
  // 3x check: same source on both sides (repeated capture frame) -> nothing to interpolate.
  if (a == b || (aid && aid == bid)) { memcpy(o, a, (size_t)fw * fh * ch); wait = 0; note("same", aid, bid, t, ka_dbg(), -1); return 0; }
  int ka = upload(a, ch, aid, -1, bid); if (ka < 0) { note("upA", aid, bid, t, -1, -1); return -2; }
  if (!aid) srcId[ka] = 0;
  int kb = upload(b, ch, bid, ka, 0); if (kb < 0 || kb == ka) { note("upB", aid, bid, t, ka, kb); return -2; }
  if (!bid) srcId[kb] = 0;
  auto t1 = std::chrono::steady_clock::now();
  if (api.setImage(fx, "SrcImage0", &src[ka]) || api.setImage(fx, "SrcImage1", &src[kb]) || api.setU32(fx, "ShotChange", 0) ||
      api.setF32(fx, "Timestep", t)) { note("set", aid, bid, t, ka, kb); return -2; }
  if (int rc = api.run(fx, 0)) { note("run", aid, bid, t, ka, rc); return -2; }
  auto t2 = std::chrono::steady_clock::now();
  NvCVImage view{};
  if (api.imgInit(&view, w, h, w * ch, o, ch == 4 ? NVCV_BGRA : NVCV_BGR, NVCV_U8, NVCV_CHUNKY, NVCV_CPU) || api.imgTransfer(&dst, &view, 1.f, stream, &tmp)) { note("down", aid, bid, t, ka, kb); return -2; }
  if (api.streamSync) api.streamSync(stream);
  if (ch == 4) for (size_t p = 0, n = (size_t)w * h; p < n; p++) o[p * 4 + 3] = 255;
  auto t3 = std::chrono::steady_clock::now();
  auto ms = [](auto x, auto y) { return std::chrono::duration<double, std::milli>(y - x).count(); };
  stage[0] = ms(t0, t1); stage[1] = ms(t1, t2); stage[2] = ms(t2, t3); wait = stage[1];
  return 0;
 }
};
} // namespace

static void vfgLog(const std::string& m) { // only with PK_VFG_LOG set
 if (!GetEnvironmentVariableW(L"PK_VFG_LOG", nullptr, 0)) return;
 wchar_t t[MAX_PATH]; DWORD n = GetTempPathW(MAX_PATH, t); if (!n || n >= MAX_PATH) return;
 FILE* f = _wfopen((std::wstring(t) + L"pk_vfg_test.log").c_str(), L"a"); if (!f) return; fputs(m.c_str(), f); fputc(10, f); fclose(f);
}
static SeqEngine* pk_create_vfg_impl(std::string& error);
SeqEngine* pk_create_vfg(std::string& error) {
 SeqEngine* e = pk_create_vfg_impl(error);
 vfgLog(std::string("pk_create_vfg pid=") + std::to_string(GetCurrentProcessId()) + " ok=" + (e ? "1" : "0") + " error=" + error);
 return e;
}
static SeqEngine* pk_create_vfg_impl(std::string& error) {
 std::wstring dir = findRuntime();
 if (dir.empty()) { error = "NVIDIA Video Frame Generation runtime not found (install nvidia-vfx or set PK_VFG_DIR)"; return nullptr; }
 const wchar_t* order[] = { L"cudart64_12.dll", L"NVCVImage.dll", L"nvngxruntime.dll", L"NVVideoEffects.dll", L"nvVFXVideoFrameGeneration.dll" };
 for (auto n : order) { std::wstring p = dir + L"\\" + n; if (!exists(p) || !signedFile(p)) { error = "NVIDIA VFX runtime file missing or not NVIDIA-signed"; return nullptr; } }
 SetDllDirectoryW(dir.c_str());
 HMODULE core = nullptr;
 for (auto n : order) { HMODULE m = LoadLibraryExW((dir + L"\\" + n).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH); if (!m) { SetDllDirectoryW(nullptr); error = "NVIDIA VFX runtime load failed"; return nullptr; } if (!wcscmp(n, L"NVVideoEffects.dll")) core = m; }
 SetDllDirectoryW(nullptr);
 std::unique_ptr<Vfg> v(new Vfg); Api& a = v->api; a.core = core;
 // NvVFX_* come from NVVideoEffects.dll, NvCVImage_* from NVCVImage.dll.
 HMODULE img = GetModuleHandleW(L"NVCVImage.dll");
 std::string missing;
 auto get = [&](HMODULE mod, auto& fn, const char* name) { fn = mod ? reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(mod, name)) : nullptr; if (!fn) missing += std::string(" ") + name; };
 get(core, a.create, "NvVFX_CreateEffect"); get(core, a.destroy, "NvVFX_DestroyEffect"); get(core, a.setU32, "NvVFX_SetU32"); get(core, a.setF32, "NvVFX_SetF32");
 get(core, a.setImage, "NvVFX_SetImage"); get(core, a.setStream, "NvVFX_SetCudaStream"); get(core, a.load, "NvVFX_Load"); get(core, a.run, "NvVFX_Run");
 get(core, a.streamCreate, "NvVFX_CudaStreamCreate"); get(core, a.streamDestroy, "NvVFX_CudaStreamDestroy");
 get(img, a.imgAlloc, "NvCVImage_Alloc"); get(img, a.imgDealloc, "NvCVImage_Dealloc"); get(img, a.imgTransfer, "NvCVImage_Transfer"); get(img, a.imgInit, "NvCVImage_Init");
 a.streamSync = reinterpret_cast<decltype(a.streamSync)>(GetProcAddress(core, "NvVFX_CudaStreamSynchronize"));
 if (!missing.empty()) { error = "NVIDIA VFX runtime exports missing:" + missing; return nullptr; }
 wchar_t m[8]; DWORD n = GetEnvironmentVariableW(L"PK_VFG_MODE", m, 8); if (n == 1 && m[0] >= L'0' && m[0] <= L'2') v->mode = m[0] - L'0';
 if (a.streamCreate(&v->stream)) { error = "CUDA stream unavailable (Video Frame Generation needs an NVIDIA RTX graphics card)"; return nullptr; }
 return v.release();
}

// Zero-copy GPU path (D3D11 shared textures -> CUDA), used by the application when NVIDIA VFG is chosen.
#include "vfg_gpu.inc"
