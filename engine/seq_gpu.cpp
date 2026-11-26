// SPDX-License-Identifier: GPL-3.0-only
// GPU-path slot API for the D3D12 offscreen frame generators
// (family 5 = NVIDIA DLSS Frame Generation through pk_dlssg.dll, family 6 = AMD FidelityFX FG through pk_amdfg.dll).
// Same calls as pk_gpu_ctx_* (RIFE) and pk_vfg_gpu_ctx_* (VFG): the application shares D3D11 textures by NT handle,
// the DLL opens them in D3D12 and copies on the GPU (no CPU upload/readback). Midpoint only (2x).
#include <windows.h>
#include <string>
#include <cstring>
#include <cstdint>
#include <mutex>
namespace {
typedef void* (*Create)(const wchar_t*, const wchar_t*, int, int, int, int, int);
typedef void (*Destroy)(void*);
typedef int (*Luid)(void*, uint8_t*);
typedef int (*Import)(void*, HANDLE);
typedef int (*Gen)(void*, int, uint64_t, int, uint64_t, int, double*);
struct SeqGpu { void* e = nullptr; Destroy destroy = nullptr; Import import = nullptr; Gen gen = nullptr; };
thread_local std::string err;
bool exists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
std::wstring dlssgRuntime(const std::wstring& bundled) {
 wchar_t v[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"PK_DLSSG_DIR", v, MAX_PATH);
 if (n && n < MAX_PATH && exists(std::wstring(v) + L"\\nvngx_dlssg.dll")) return v;
 if (!bundled.empty() && exists(bundled + L"\\nvngx_dlssg.dll")) return bundled; // bundled runtime
 n = GetEnvironmentVariableW(L"LOCALAPPDATA", v, MAX_PATH);
 if (n && n < MAX_PATH) { std::wstring c = std::wstring(v) + L"\\Peussok\\nvidia-dlssg"; if (exists(c + L"\\nvngx_dlssg.dll")) return c; }
 return L"";
}
}
extern "C" __declspec(dllexport) const char* pk_seq_gpu_error() { return err.c_str(); }
extern "C" __declspec(dllexport) void* pk_seq_gpu_ctx_create(int family, const wchar_t* modelDir, int w, int h, uint8_t* luid8) {
 static std::mutex createMu; std::lock_guard<std::mutex> lk(createMu); // two pipelines created DLSS-G at once -> 0xBAD0000B
 err.clear(); if (family != 5 && family != 6) { err = "family"; return nullptr; }
 const wchar_t* dll = family == 5 ? L"\\pk_dlssg.dll" : L"\\pk_amdfg.dll"; const char* px = family == 5 ? "dg_" : "fg_";
 HMODULE m = LoadLibraryExW((std::wstring(modelDir) + dll).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
 if (!m) { err = "dll load failed"; return nullptr; }
 auto f = [&](const char* n) { return GetProcAddress(m, (std::string(px) + n).c_str()); };
 auto create = reinterpret_cast<Create>(f("create")); auto luid = reinterpret_cast<Luid>(f("gpu_luid"));
 auto* s = new SeqGpu; s->destroy = reinterpret_cast<Destroy>(f("destroy")); s->import = reinterpret_cast<Import>(f("gpu_import")); s->gen = reinterpret_cast<Gen>(f("gpu_generate"));
 if (!create || !luid || !s->destroy || !s->import || !s->gen) { err = "GPU-path exports missing (old DLL)"; delete s; return nullptr; }
 std::wstring rt = family == 5 ? dlssgRuntime(modelDir) : std::wstring(modelDir);
 if (rt.empty()) { err = "NVIDIA DLSS Frame Generation runtime not found"; delete s; return nullptr; }
 wchar_t lp[MAX_PATH]; DWORD ln = GetEnvironmentVariableW(family == 5 ? L"PK_DLSSG_LOG" : L"PK_AMDFG_LOG", lp, MAX_PATH);
 s->e = create(rt.c_str(), ln && ln < MAX_PATH ? lp : nullptr, w, h, w, h, 1); // 1 = B8G8R8A8 (host frames are BGRA)
 if (!s->e) { err = "create failed"; delete s; return nullptr; }
 if (luid(s->e, luid8)) { err = "luid"; s->destroy(s->e); delete s; return nullptr; }
 return s;
}
extern "C" __declspec(dllexport) int pk_seq_gpu_ctx_import(void* p, HANDLE h) { auto* s = static_cast<SeqGpu*>(p); if (!s) return -1; int r = s->import(s->e, h); if (r < 0) err = "import"; return r; }
extern "C" __declspec(dllexport) int pk_seq_gpu_ctx_generate(void* p, int a, uint64_t aid, int b, uint64_t bid, int out, float t, double* ms3) {
 auto* s = static_cast<SeqGpu*>(p); if (!s) return -1; if (t < 0.499f || t > 0.501f) { err = "midpoint only"; return -4; }
 double ms = 0; int r = s->gen(s->e, a, aid, b, bid, out, &ms); if (r) { err = "generate " + std::to_string(r); return r; }
 if (ms3) { ms3[0] = 0; ms3[1] = ms; ms3[2] = 0; } return 0;
}
extern "C" __declspec(dllexport) void pk_seq_gpu_ctx_destroy(void* p) { auto* s = static_cast<SeqGpu*>(p); if (!s) return; if (s->e) s->destroy(s->e); delete s; }
