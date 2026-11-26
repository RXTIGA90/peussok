// SPDX-License-Identifier: GPL-3.0-only
// AMD FidelityFX SDK 2.4.0 frame generation DLL (ffx-api, D3D12), offscreen.
// engines\amdfg\pk_amdfg.dll is our code (engine/amdfg/fsr31_dx12.cpp); amd_fidelityfx_framegeneration_dx12.dll and
// amd_fidelityfx_loader_dx12.dll are AMD's signed binaries (AMD binary redistribution terms, LICENSE.md beside them).
// (RTX 5080): provider FSR FG 3.1.6, zero motion vectors 36.21 dB (same as our Vulkan FSR 3 with zero vectors);
// on RX 9000 the DLL may pick its ML provider (FSR 4 FG), not measured. Midpoint only (2x).
#include <windows.h>
#include "seq_engines.h"
#include <string>
#include <vector>
#include <chrono>
#include <cmath>
#include <memory>
namespace {
typedef void* (*FgCreate)(const wchar_t*, const wchar_t*, int, int, int, int, int);
typedef int (*FgInterp)(void*, const uint8_t*, const uint8_t*, const void*, uint64_t, uint64_t, uint8_t*, double*);
typedef void (*FgDestroy)(void*);
struct AmdFg final : SeqEngine {
 FgCreate create = nullptr; FgInterp interp = nullptr; FgDestroy destroy = nullptr;
 void* e = nullptr; int w = 0, h = 0; std::wstring dir; std::vector<uint8_t> ta, tb, to;
 ~AmdFg() override { if (e && destroy) destroy(e); }
 int process(const uint8_t* a, const uint8_t* b, int fw, int fh, int ch, float t, uint8_t* o, uint64_t aid, uint64_t bid) override {
  if (std::fabs(t - 0.5f) > 1e-3f) return -4;
  auto t0 = std::chrono::steady_clock::now();
  if (!e || fw != w || fh != h) {
   if (e) { destroy(e); e = nullptr; }
   wchar_t lp[MAX_PATH]; DWORD ln = GetEnvironmentVariableW(L"PK_AMDFG_LOG", lp, MAX_PATH);
   e = create(dir.c_str(), ln && ln < MAX_PATH ? lp : nullptr, fw, fh, fw, fh, 1);
   if (!e) return -1; w = fw; h = fh;
  }
  const uint8_t *pa = a, *pb = b; uint8_t* po = o; size_t px = (size_t)fw * fh;
  if (ch != 4) { ta.resize(px * 4); tb.resize(px * 4); to.resize(px * 4); pk_to_4(a, ch, ta.data(), px, false); pk_to_4(b, ch, tb.data(), px, false); pa = ta.data(); pb = tb.data(); po = to.data(); }
  double ms = 0; if (interp(e, pa, pb, nullptr, aid, bid, po, &ms)) return -1;
  if (ch != 4) pk_from_4(to.data(), o, ch, px, false);
  wait = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  stage[0] = 0; stage[1] = ms; stage[2] = 0;
  return 0;
 }
};
} // namespace
SeqEngine* pk_create_amdfg(const std::wstring& modelDir, std::string& error) {
 HMODULE m = LoadLibraryExW((modelDir + L"\\pk_amdfg.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
 if (!m) { error = "pk_amdfg.dll load failed"; return nullptr; }
 std::unique_ptr<AmdFg> d(new AmdFg); d->dir = modelDir;
 d->create = reinterpret_cast<FgCreate>(GetProcAddress(m, "fg_create")); d->interp = reinterpret_cast<FgInterp>(GetProcAddress(m, "fg_interpolate"));
 d->destroy = reinterpret_cast<FgDestroy>(GetProcAddress(m, "fg_destroy"));
 if (!d->create || !d->interp || !d->destroy) { error = "pk_amdfg.dll exports missing"; return nullptr; }
 return d.release();
}
