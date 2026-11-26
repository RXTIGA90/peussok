// SPDX-License-Identifier: GPL-3.0-only
// NVIDIA DLSS Frame Generation through the public NGX DLSSG API, offscreen.
// The NGX/D3D12 part lives in pk_dlssg.dll (engine/dlssg/dlssg_ngx.cpp, public DLSS SDK headers).
// nvngx_dlssg.dll is bundled in the installer (NVIDIA RTX SDKs license); a newer one can be copied from the NVIDIA app's DLSS cache
// (engines\dlssg\Get-NvidiaDlssg.ps1) into %LOCALAPPDATA%\Peussok\nvidia-dlssg, or found via PK_DLSSG_DIR.
// 2x; 3x/4x (multi-frame, RTX 50). Motion vectors are zero: DLSS-G's own optical flow handles the video
// 38.72 dB on the 30->60 game test, ~0.85 ms GPU at 1080p).
#include <windows.h>
#include "seq_engines.h"
#include <string>
#include <vector>
#include <chrono>
#include <cmath>
#include <memory>
namespace {
typedef void* (*DgCreate)(const wchar_t*, const wchar_t*, int, int, int, int, int);
typedef int (*DgInterp)(void*, const uint8_t*, const uint8_t*, const void*, uint64_t, uint64_t, uint8_t*, double*);
typedef void (*DgDestroy)(void*);
typedef int (*DgInterpMf)(void*, const uint8_t*, const uint8_t*, const void*, uint64_t, uint64_t, unsigned, unsigned, uint8_t*, double*);
bool exists(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
std::wstring runtimeDir(const std::wstring& bundled) {
 wchar_t v[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"PK_DLSSG_DIR", v, MAX_PATH);
 if (n && n < MAX_PATH && exists(std::wstring(v) + L"\\nvngx_dlssg.dll")) return v;
 if (!bundled.empty() && exists(bundled + L"\\nvngx_dlssg.dll")) return bundled; // bundled runtime
 n = GetEnvironmentVariableW(L"LOCALAPPDATA", v, MAX_PATH);
 if (n && n < MAX_PATH) { std::wstring c = std::wstring(v) + L"\\Peussok\\nvidia-dlssg"; if (exists(c + L"\\nvngx_dlssg.dll")) return c; }
 return L"";
}
struct Dg final : SeqEngine {
 HMODULE mod = nullptr; DgCreate create = nullptr; DgInterp interp = nullptr; DgDestroy destroy = nullptr; DgInterpMf interpMf = nullptr; int n = 2;
 void* e = nullptr; int w = 0, h = 0; std::wstring rt; std::vector<uint8_t> ta, tb, to;
 ~Dg() override { if (e && destroy) destroy(e); }
 int process(const uint8_t* a, const uint8_t* b, int fw, int fh, int ch, float t, uint8_t* o, uint64_t aid, uint64_t bid) override {
  // multi-frame: t = index/n. n stays from the previous pair while t fits it (4x keeps t=0.5 as index 2 of 4).
  auto fits = [&](int k) { float x = t * k; return std::fabs(x - std::round(x)) < 1e-3f && std::round(x) >= 1 && std::round(x) < k; };
  if (!fits(n)) { int k = 2; while (k <= 6 && !fits(k)) k++; if (k > 6) return -4; n = k; }
  if (n > 2 && !interpMf) return -4;
  unsigned index = (unsigned)std::lround(t * n);
  auto t0 = std::chrono::steady_clock::now();
  if (!e || fw != w || fh != h) {
   if (e) { destroy(e); e = nullptr; }
   wchar_t lp[MAX_PATH]; DWORD ln = GetEnvironmentVariableW(L"PK_DLSSG_LOG", lp, MAX_PATH);   // test: NGX/D3D12 log file
   e = create(rt.c_str(), ln && ln < MAX_PATH ? lp : nullptr, fw, fh, fw, fh, 1);               // 1 = B8G8R8A8 (live frames are BGRA)
   if (!e) return -1; w = fw; h = fh;
  }
  const uint8_t *pa = a, *pb = b; uint8_t* po = o; size_t px = (size_t)fw * fh;
  if (ch != 4) { ta.resize(px * 4); tb.resize(px * 4); to.resize(px * 4); pk_to_4(a, ch, ta.data(), px, false); pk_to_4(b, ch, tb.data(), px, false); pa = ta.data(); pb = tb.data(); po = to.data(); }
  double ms = 0; int rc = interpMf ? interpMf(e, pa, pb, nullptr, aid, bid, (unsigned)(n - 1), index, po, &ms) : interp(e, pa, pb, nullptr, aid, bid, po, &ms);
  if (rc == -4) return -4;
  if (rc) return -1;
  if (ch != 4) pk_from_4(to.data(), o, ch, px, false);
  wait = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  stage[0] = 0; stage[1] = ms; stage[2] = 0;
  return 0;
 }
};
} // namespace
SeqEngine* pk_create_dlssg(const std::wstring& modelDir, std::string& error) {
 std::wstring rt = runtimeDir(modelDir);
 if (rt.empty()) { error = "NVIDIA DLSS Frame Generation runtime not found (engines\\dlssg\\Get-NvidiaDlssg.ps1 or PK_DLSSG_DIR)"; return nullptr; }
 std::wstring dll = modelDir + L"\\pk_dlssg.dll";
 HMODULE m = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
 if (!m) { error = "pk_dlssg.dll load failed"; return nullptr; }
 std::unique_ptr<Dg> d(new Dg); d->mod = m; d->rt = rt;
 d->create = reinterpret_cast<DgCreate>(GetProcAddress(m, "dg_create")); d->interp = reinterpret_cast<DgInterp>(GetProcAddress(m, "dg_interpolate"));
 d->destroy = reinterpret_cast<DgDestroy>(GetProcAddress(m, "dg_destroy"));
 d->interpMf = reinterpret_cast<DgInterpMf>(GetProcAddress(m, "dg_interpolate_mf"));
 if (!d->create || !d->interp || !d->destroy) { error = "pk_dlssg.dll exports missing"; return nullptr; }
 return d.release();
}
