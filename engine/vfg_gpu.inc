// SPDX-License-Identifier: GPL-3.0-only
// (included at the end of vfg_engine.cpp): VFG on the zero-copy GPU path. The application's shared
// D3D11 textures (NT handles) are imported into CUDA (driver API from nvcuda.dll, loaded at run time), copied into
// pitched BGRA buffers, converted to the SDK's RGBA slots, interpolated, and written back into the output texture.
// Same slot numbering as pk_gpu_ctx_* (0-2 inputs, 3-4 outputs).
namespace {
typedef int CUresult; typedef int CUdevice; struct CUctx_st; typedef CUctx_st* CUcontext; typedef unsigned long long CUdeviceptr;
struct CUextMemory_st; typedef CUextMemory_st* CUexternalMemory; struct CUmipmappedArray_st; typedef CUmipmappedArray_st* CUmipmappedArray;
struct CUarray_st; typedef CUarray_st* CUarray;
struct CuExtMemDesc { int type; union { int fd; struct { void* handle; const void* name; } win32; const void* sci; } handle; unsigned long long size; unsigned int flags; unsigned int reserved[16]; };
struct CuArray3DDesc { size_t Width, Height, Depth; int Format; unsigned int NumChannels; unsigned int Flags; };
struct CuExtMipDesc { unsigned long long offset; CuArray3DDesc arrayDesc; unsigned int numLevels; unsigned int reserved[16]; };
struct CuMemcpy2D { size_t srcXInBytes, srcY; int srcMemoryType; const void* srcHost; CUdeviceptr srcDevice; CUarray srcArray; size_t srcPitch;
                    size_t dstXInBytes, dstY; int dstMemoryType; void* dstHost; CUdeviceptr dstDevice; CUarray dstArray; size_t dstPitch; size_t WidthInBytes, Height; };
struct Cu {
 HMODULE m = nullptr;
 CUresult (*init)(unsigned) = nullptr; CUresult (*deviceGet)(CUdevice*, int) = nullptr;
 CUresult (*getLuid)(char*, unsigned*, CUdevice) = nullptr; CUresult (*primaryRetain)(CUcontext*, CUdevice) = nullptr;
 CUresult (*push)(CUcontext) = nullptr; CUresult (*pop)(CUcontext*) = nullptr;
 CUresult (*importMem)(CUexternalMemory*, const CuExtMemDesc*) = nullptr; CUresult (*mapMip)(CUmipmappedArray*, CUexternalMemory, const CuExtMipDesc*) = nullptr;
 CUresult (*mipLevel)(CUarray*, CUmipmappedArray, unsigned) = nullptr; CUresult (*destroyMem)(CUexternalMemory) = nullptr; CUresult (*mipDestroy)(CUmipmappedArray) = nullptr;
 CUresult (*copy2D)(const CuMemcpy2D*, CUstream) = nullptr; CUresult (*sync)(CUstream) = nullptr;
 template <class F> bool get(F& f, const char* n) { f = reinterpret_cast<F>(GetProcAddress(m, n)); return f != nullptr; }
 bool load() {
  m = LoadLibraryW(L"nvcuda.dll"); if (!m) return false;
  return get(init, "cuInit") && get(deviceGet, "cuDeviceGet") && get(getLuid, "cuDeviceGetLuid") && get(primaryRetain, "cuDevicePrimaryCtxRetain") &&
         get(push, "cuCtxPushCurrent_v2") && get(pop, "cuCtxPopCurrent_v2") && get(importMem, "cuImportExternalMemory") && get(mapMip, "cuExternalMemoryGetMappedMipmappedArray") &&
         get(mipLevel, "cuMipmappedArrayGetLevel") && get(destroyMem, "cuDestroyExternalMemory") && get(mipDestroy, "cuMipmappedArrayDestroy") && get(copy2D, "cuMemcpy2DAsync_v2") && get(sync, "cuStreamSynchronize");
 }
};
std::string g_vfgGpuError;
struct VfgGpu {
 Vfg* v = nullptr; Cu cu; CUcontext ctx = nullptr; int w = 0, h = 0, n = 0;
 CUexternalMemory mem[5]{}; CUmipmappedArray mip[5]{}; CUarray arr[5]{};
 NvCVImage lin[5]{};                         // pitched BGRA staging per slot
 int slotOf[2]{-1, -1}; uint64_t idOf[2]{};  // which input slot + id sits in v->src[k]
 ~VfgGpu() {
  if (ctx) cu.push(ctx);
  for (int i = 0; i < 5; i++) { if (lin[i].pixels && v) v->api.imgDealloc(&lin[i]); if (mip[i]) cu.mipDestroy(mip[i]); if (mem[i]) cu.destroyMem(mem[i]); }
  delete v;
  if (ctx) { CUcontext c; cu.pop(&c); }
 }
 bool copyArr(CUarray a, NvCVImage& l, bool toArray) {
  CuMemcpy2D c{}; c.WidthInBytes = (size_t)w * 4; c.Height = h;
  if (toArray) { c.srcMemoryType = 2; c.srcDevice = (CUdeviceptr)l.pixels; c.srcPitch = l.pitch; c.dstMemoryType = 3; c.dstArray = a; }
  else { c.srcMemoryType = 3; c.srcArray = a; c.dstMemoryType = 2; c.dstDevice = (CUdeviceptr)l.pixels; c.dstPitch = l.pitch; }
  return cu.copy2D(&c, v->stream) == 0;
 }
};
} // namespace

extern "C" __declspec(dllexport) const char* pk_vfg_gpu_error() { return g_vfgGpuError.c_str(); }
extern "C" __declspec(dllexport) void* pk_vfg_gpu_ctx_create(int w, int h, uint8_t* luidOut) {
 std::string err; SeqEngine* e = pk_create_vfg_impl(err); if (!e) { g_vfgGpuError = err; return nullptr; }
 std::unique_ptr<VfgGpu> g(new VfgGpu); g->v = static_cast<Vfg*>(e); g->w = w; g->h = h;
 if (!g->cu.load() || g->cu.init(0)) { g_vfgGpuError = "nvcuda.dll / cuInit failed"; return nullptr; }
 CUdevice dev = 0; unsigned mask = 0; char luid[8]{};
 if (g->cu.deviceGet(&dev, 0) || g->cu.getLuid(luid, &mask, dev) || g->cu.primaryRetain(&g->ctx, dev)) { g_vfgGpuError = "CUDA device 0 unavailable"; return nullptr; }
 if (luidOut) memcpy(luidOut, luid, 8);
 g->cu.push(g->ctx);
 bool ok = g->v->setup(w, h);
 for (int i = 0; ok && i < 5; i++) ok = g->v->api.imgAlloc(&g->lin[i], w, h, NVCV_BGRA, NVCV_U8, NVCV_CHUNKY, NVCV_GPU, 256) == 0;
 CUcontext c; g->cu.pop(&c);
 if (!ok) { g_vfgGpuError = "VFG setup / staging alloc failed"; return nullptr; }
 vfgLog("pk_vfg_gpu_ctx_create " + std::to_string(w) + "x" + std::to_string(h) + " mode=" + std::to_string(g->v->mode));
 return g.release();
}
extern "C" __declspec(dllexport) int pk_vfg_gpu_ctx_import(void* p, HANDLE nt) {
 VfgGpu* g = static_cast<VfgGpu*>(p); if (!g || g->n >= 5) return -1; int i = g->n;
 g->cu.push(g->ctx); int rc = -1;
 unsigned long long base = ((unsigned long long)g->w * g->h * 4 + 0xFFFF) & ~0xFFFFull;
 for (int k = 0; k < 4 && rc; k++) {  // the driver wants the allocation size; tiled textures can be larger than w*h*4
  CuExtMemDesc d{}; d.type = 6 /* D3D11_RESOURCE (NT handle) */; d.handle.win32.handle = nt; d.size = base + base * k / 2; d.flags = 1 /* DEDICATED */;
  if (g->mip[i]) { g->cu.mipDestroy(g->mip[i]); g->mip[i] = nullptr; }
  if (g->mem[i]) { g->cu.destroyMem(g->mem[i]); g->mem[i] = nullptr; }
  CUresult r1 = g->cu.importMem(&g->mem[i], &d); if (r1) { g_vfgGpuError = "cuImportExternalMemory " + std::to_string(r1); continue; }
  CuExtMipDesc md{}; md.arrayDesc.Width = g->w; md.arrayDesc.Height = g->h; md.arrayDesc.Depth = 0; md.arrayDesc.Format = 0x01; md.arrayDesc.NumChannels = 4; md.arrayDesc.Flags = 0x20 /* COLOR_ATTACHMENT */; md.numLevels = 1;
  CUresult r2 = g->cu.mapMip(&g->mip[i], g->mem[i], &md); if (r2) { g_vfgGpuError = "cuExternalMemoryGetMappedMipmappedArray " + std::to_string(r2); continue; }
  if (g->cu.mipLevel(&g->arr[i], g->mip[i], 0)) { g_vfgGpuError = "cuMipmappedArrayGetLevel"; continue; }
  rc = 0; vfgLog("pk_vfg_gpu_ctx_import slot " + std::to_string(i) + " size=" + std::to_string(d.size));
 }
 CUcontext c; g->cu.pop(&c);
 if (rc) { vfgLog("pk_vfg_gpu_ctx_import failed: " + g_vfgGpuError); return -1; }
 return g->n++;
}
// a/b input slots (0-2) with ids, out slot (3-4), t in (0,1). ms[0] copy-in, ms[1] VFG, ms[2] copy-out.
extern "C" __declspec(dllexport) int pk_vfg_gpu_ctx_generate(void* p, int a, uint64_t aid, int b, uint64_t bid, int out, float t, double* ms) {
 VfgGpu* g = static_cast<VfgGpu*>(p); if (!g || a < 0 || b < 0 || out < 3 || a >= g->n || b >= g->n || out >= g->n) return -1;
 Vfg* v = g->v; auto now = [] { return std::chrono::steady_clock::now(); };
 auto d = [](std::chrono::steady_clock::time_point x, std::chrono::steady_clock::time_point y) { return std::chrono::duration<double, std::milli>(y - x).count(); };
 g->cu.push(g->ctx); int rc = 0; auto t0 = now();
 auto slotFor = [&](int s, uint64_t id, int avoid) -> int {
  for (int k = 0; k < 2; k++) if (g->idOf[k] == id && g->slotOf[k] == s) return k;
  int k = avoid >= 0 ? 1 - avoid : (g->idOf[0] == bid ? 1 : 0);
  if (!g->copyArr(g->arr[s], g->lin[s], false)) return -1;
  if (v->api.imgTransfer(&g->lin[s], &v->src[k], 1.f, v->stream, &v->tmp)) return -1;
  g->idOf[k] = id; g->slotOf[k] = s; return k;
 };
 int ka = slotFor(a, aid, -1), kb = ka < 0 ? -1 : slotFor(b, bid, ka);
 auto t1 = now();
 if (ka < 0 || kb < 0 || ka == kb) rc = -2;
 else if (v->api.setImage(v->fx, "SrcImage0", &v->src[ka]) || v->api.setImage(v->fx, "SrcImage1", &v->src[kb]) || v->api.setU32(v->fx, "ShotChange", 0) ||
          v->api.setF32(v->fx, "Timestep", t) || v->api.run(v->fx, 0)) rc = -3;
 auto t2 = now();
 if (!rc && (v->api.imgTransfer(&v->dst, &g->lin[out], 1.f, v->stream, &v->tmp) || !g->copyArr(g->arr[out], g->lin[out], true))) rc = -4;
 if (g->cu.sync(v->stream) && !rc) rc = -5;
 auto t3 = now();
 CUcontext c; g->cu.pop(&c);
 if (ms) { ms[0] = d(t0, t1); ms[1] = d(t1, t2); ms[2] = d(t2, t3); }
 if (rc) g_vfgGpuError = "vfg gpu generate rc=" + std::to_string(rc);
 return rc;
}
extern "C" __declspec(dllexport) void pk_vfg_gpu_ctx_destroy(void* p) { delete static_cast<VfgGpu*>(p); }
