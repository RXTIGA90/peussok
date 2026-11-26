// SPDX-License-Identifier: GPL-3.0-only
#define PK_ENGINE_BUILD
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include "engine.h"
#include "rife.h"
#include <vector>
#include "gpu.h"
#include <mutex>
#include <memory>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <string>
#include "model_catalog.h"
#include "seq_engines.h"
#include <vulkan/vulkan.h>
#include <atomic>
#include "gpu_keeper.h"
static std::mutex lifecycle;
// settings switches, set by the application before it creates a stream: bit 0 GPU clock keep, bit 1 fp16 arithmetic,
// bit 2 FSR 3 acceleration.
static std::atomic<int> gpu_options{0};
void pk_set_gpu_options(int flags){gpu_options=flags&7;}
int pk_gpu_options_value(){return gpu_options.load();}
static unsigned users;
static thread_local std::string error;
struct Engine { std::unique_ptr<RIFE> rife,aux; float auxScale=0; std::unique_ptr<SeqEngine> seq;
 int gpu,model; double ms=0,wait=0,stage[3]={-1,-1,-1}; std::string info; std::wstring path; float scale=1.f; int floor=1; uint64_t untagged=0; };
// RIFE estimates motion at a reduced size and warps the full-size image (reference inference scale).
// FHD floor (stable, default): FHD and below full size, QHD 0.75 and UHD 0.5 (both 1920x1080 flow);
// real footage FHD 28.88 vs 28.31 dB at half, QHD 29.63 vs 29.36 dB. Low latency: half size from FHD
// up. Test override: PK_RIFE_FLOW=full|half|0.75.
static float want_scale(int w,int h,int floor){
 wchar_t v[8];DWORD n=GetEnvironmentVariableW(L"PK_RIFE_FLOW",v,8);
 if(n==4&&!wcscmp(v,L"full"))return 1.f;if(n==4&&!wcscmp(v,L"half"))return .5f;if(n==4&&!wcscmp(v,L"0.75"))return .75f;
 if(n==5&&!wcscmp(v,L"0.125"))return .125f;if(n==4&&!wcscmp(v,L"0.25"))return .25f;if(n==5&&!wcscmp(v,L"0.375"))return .375f; // slow GPUs (RX 6600)
 int64_t px=(int64_t)w*h;
 // slow GPU (live.cpp picks it when half-size flow does not fit the 2x budget): RX 6600, Mario Kart real
 // frames, flow half 17.0 ms 24.01 dB, quarter 10.2 ms 23.10 dB (FSR 3 20.25 dB, repeat 17.95 dB).
 if(floor==3)return px>=2560LL*1440?.125f:px>=1920LL*1080?.25f:.5f; // one step lower, QHD and up (GPU path)
 if(floor==2)return px>=1920LL*1080?.25f:.5f;
 if(!floor)return px>=1920LL*1080?.5f:1.f;
 return px<=1920LL*1200?1.f:px<=2560LL*1600?.75f:.5f;
}
static int load_rife(Engine* e,float scale){
 // Padding keeps the scaled flow input a multiple of 32: 64 for 0.5, 128 for 0.75.
 // PK_RIFE_TTA=t (temporal), s (spatial flips) for quality checks only.
 wchar_t tv[4]={0};GetEnvironmentVariableW(L"PK_RIFE_TTA",tv,4);const bool ttaS=tv[0]==L's',ttaT=tv[0]==L't';
 std::unique_ptr<RIFE> r(new RIFE(e->gpu,ttaS,ttaT,false,1,false,true,scale<.2f?256:scale<.3f?128:scale<.4f?256:scale<.6f?64:scale<1.f?128:pk_models[e->model].padding));
 r->flow_half=scale<1.f;r->flow_scale=scale;r->fp16_arith=(gpu_options.load()&2)!=0;int rc=r->load(e->path);if(rc)return rc;e->rife=std::move(r);e->scale=scale;return 0;
}
// diagnostics: driver name/version and the queue ncnn computes on (AMD exposes separate
// compute-only families). A short-lived Vulkan 1.2 instance reads the driver properties.
static std::string describe_gpu(int gpu){
 const ncnn::GpuInfo& g=ncnn::get_gpu_info(gpu);char line[512];
 std::string driver="unknown";
 VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_2;VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ii.pApplicationInfo=&app;VkInstance inst=VK_NULL_HANDLE;
 if(!vkCreateInstance(&ii,nullptr,&inst)){
  uint32_t n=0;vkEnumeratePhysicalDevices(inst,&n,nullptr);std::vector<VkPhysicalDevice> pd(n);vkEnumeratePhysicalDevices(inst,&n,pd.data());
  for(auto p:pd){VkPhysicalDeviceProperties pp;vkGetPhysicalDeviceProperties(p,&pp);if(pp.vendorID!=g.vendor_id()||pp.deviceID!=g.device_id()||pp.apiVersion<VK_API_VERSION_1_2)continue;
   VkPhysicalDeviceDriverProperties dp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};p2.pNext=&dp;
   vkGetPhysicalDeviceProperties2(p,&p2);driver=std::string(dp.driverName)+" "+dp.driverInfo;break;}
  vkDestroyInstance(inst,nullptr);
 }
 for(char& c:driver)if(c=='\r'||c=='\n'||c=='"')c=' ';
 uint32_t api=g.api_version();
 snprintf(line,sizeof line,"vendor=0x%04x device=0x%04x type=%d api=%u.%u.%u driver_raw=0x%08x driver=\"%s\" compute_family=%u graphics_family=%u compute_queues=%u fp16_storage=%d fp16_arith=%d subgroup=%u",
  g.vendor_id(),g.device_id(),g.type(),VK_VERSION_MAJOR(api),VK_VERSION_MINOR(api),VK_VERSION_PATCH(api),g.driver_version(),driver.c_str(),g.compute_queue_family_index(),g.graphics_queue_family_index(),g.compute_queue_count(),
  (int)g.support_fp16_storage(),(int)g.support_fp16_arithmetic(),g.subgroup_size());
 return line;
}
static bool trusted(const std::wstring& path,const char* expected){
 HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
 if(file==INVALID_HANDLE_VALUE)return false;
 BCRYPT_ALG_HANDLE alg=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;bool ok=false;
 if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0&&
    BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0)>=0){
  uint8_t block[65536],digest[32];DWORD count=0;BOOL read;
  bool valid=true;
  while((read=ReadFile(file,block,sizeof(block),&count,nullptr))&&count)
   if(BCryptHashData(hash,block,count,0)<0){valid=false;break;}
  if(read&&valid&&BCryptFinishHash(hash,digest,32,0)>=0){
   char hex[65];for(int i=0;i<32;i++)snprintf(hex+i*2,3,"%02X",digest[i]);ok=strcmp(hex,expected)==0;
  }
 }
 if(hash)BCryptDestroyHash(hash);if(alg)BCryptCloseAlgorithmProvider(alg,0);CloseHandle(file);return ok;
}
// Every file an engine loads from the application folder is pinned by SHA-256 in the catalog.
static bool approved(const std::wstring& path,int i){
 const PkModelSpec& m=pk_models[i];
 if(m.family==3||m.family==4||m.family==5||m.family==6)return trusted(path+L"/"+m.basename,m.param_sha256);
 return trusted(path+L"/"+m.basename+L".param",m.param_sha256)&&trusted(path+L"/"+m.basename+L".bin",m.bin_sha256);
}
static bool layers_off,layers_tried;
static void* create_locked(const std::wstring& path,int model,int gpu){
 if(!users&&ncnn::create_gpu_instance()!=0){error="Vulkan instance could not be created";ncnn::destroy_gpu_instance();return nullptr;}
 int count=ncnn::get_gpu_count();
 if(gpu<0)gpu=ncnn::get_default_gpu_index();
 if(gpu<0||gpu>=count){error="No requested Vulkan GPU";if(!users)ncnn::destroy_gpu_instance();return nullptr;}
 try {
  std::unique_ptr<Engine> e(new Engine);e->gpu=gpu;e->model=model;e->path=path;try{e->info=describe_gpu(gpu);}catch(...){}
  int rc=-1;
  if(pk_models[model].family==3){{const ncnn::GpuInfo& info=ncnn::get_gpu_info(gpu);e->seq.reset(pk_create_fsr3(info.vendor_id(),info.device_id(),error));}rc=e->seq?0:-1;}
#ifdef PK_INTERNAL_VFG
  else if(pk_models[model].family==4){e->seq.reset(pk_create_vfg(error));rc=e->seq?0:-1;}
#ifdef PK_INTERNAL_DLSSG
  else if(pk_models[model].family==5){e->seq.reset(pk_create_dlssg(path,error));rc=e->seq?0:-1;}
#endif
  else if(pk_models[model].family==6){e->seq.reset(pk_create_amdfg(path,error));rc=e->seq?0:-1;}
#endif
  else rc=load_rife(e.get(),1.f);
  {int o=gpu_options.load();char extra[96];snprintf(extra,sizeof extra," options=%d fp16_arith_used=%d",o,(pk_models[model].family!=3&&(o&2)&&ncnn::get_gpu_info(gpu).support_fp16_arithmetic())?1:0);e->info+=extra;}
  if(rc!=0){if(error.empty())error="Model load failed";e.reset();if(!users)ncnn::destroy_gpu_instance();return nullptr;}
  ++users;return e.release();
 }catch(...){error="Model initialization exception";if(!users)ncnn::destroy_gpu_instance();return nullptr;}
}
void* pk_create(const wchar_t* dir,int gpu){
 error.clear();if(!dir){error="Missing model directory";return nullptr;}
 std::wstring path(dir);
 int model=-1;
 if(path.size()<=230)for(int i=0;i<PK_MODEL_COUNT;i++)
  if(pk_model_valid(i)&&approved(path,i)){model=i;break;}
 if(!pk_model_valid(model)){error="Approved model missing, changed, or path too long";return nullptr;}
 std::lock_guard<std::mutex> guard(lifecycle);
 // Vulkan overlay layers (RivaTuner, Overwolf, Steam, Discord, ReShade ...) load into every Vulkan program and can break
 // the start. For the Vulkan models (RIFE, FSR 3), when the first start fails, try once per process with the loader's
 // implicit layers off. The variable belongs to the whole host process: it stays only when the retry worked (later
 // engines need it too) and is put back otherwise. A PC that starts normally never reaches the retry.
 const int family=pk_models[model].family;
 void* e=create_locked(path,model,gpu);
 if(!e&&!users&&!layers_tried&&(family==0||family==3)){
  std::string first=error;layers_tried=true;wchar_t old[512];DWORD had=GetEnvironmentVariableW(L"VK_LOADER_LAYERS_DISABLE",old,512);
  if(had<512){
   SetEnvironmentVariableW(L"VK_LOADER_LAYERS_DISABLE",L"~implicit~");error.clear();e=create_locked(path,model,gpu);
   if(e)layers_off=true;else{error=first+" | again without Vulkan overlay layers: "+error;SetEnvironmentVariableW(L"VK_LOADER_LAYERS_DISABLE",had?old:nullptr);}
  }
 }
 if(e&&layers_off&&(family==0||family==3))static_cast<Engine*>(e)->info+=" implicit_layers=off";
 return e;
}
static int interpolate_impl(void* handle,const uint8_t* a,const uint8_t* b,int w,int h,int ch,float t,uint8_t* output,uint64_t aid,uint64_t bid){
 error.clear();auto* e=static_cast<Engine*>(handle);
 if(!e||!a||!b||!output||w<32||h<32||w>4096||h>2160||(ch!=3&&ch!=4)||!std::isfinite(t)||t<0||t>1){error="Invalid frame or interpolation phase";return -1;}
 e->wait=-1;e->stage[0]=e->stage[1]=e->stage[2]=-1;
 if(t==0||t==1){memcpy(output,t==0?a:b,(size_t)w*h*ch);e->ms=0;return 0;}
 if(e->rife&&want_scale(w,h,e->floor)!=e->scale){
  try{std::lock_guard<std::mutex> guard(lifecycle);if(load_rife(e,want_scale(w,h,e->floor))){error="Model load failed";return -2;}}catch(...){error="Model initialization exception";return -3;}
 }
 LARGE_INTEGER start,end,freq;QueryPerformanceFrequency(&freq);QueryPerformanceCounter(&start);
 try {
  ncnn::Mat in0(w,h,const_cast<uint8_t*>(a),(size_t)ch,ch);
  ncnn::Mat in1(w,h,const_cast<uint8_t*>(b),(size_t)ch,ch);
  ncnn::Mat out(w,h,output,(size_t)ch,ch);
  // Untagged calls (calibration, tools) get private ids so RIFE never reuses a cached source.
  if(e->rife&&(!aid||!bid)){aid=(1ULL<<63)|++e->untagged;bid=(1ULL<<63)|++e->untagged;}
  int code=e->seq?e->seq->process(a,b,w,h,ch,t,output,aid,bid):e->rife->process_v4_tagged(in0,in1,t,out,aid,bid);
  QueryPerformanceCounter(&end);e->ms=1000.0*(end.QuadPart-start.QuadPart)/freq.QuadPart;
  if(e->seq)e->wait=e->seq->wait;
  {const double* st=e->seq?e->seq->stage:e->rife->stage_ms;for(int i=0;i<3;i++)e->stage[i]=st[i];}
  if(code){error=code==-4?"Phase not supported by this engine":"Inference failed";return code==-4?-4:-2;}
  return 0;
 }catch(...){error="Inference exception";return -3;}
}
int pk_interpolate(void* p,const uint8_t* a,const uint8_t* b,int w,int h,float t,uint8_t* out){return interpolate_impl(p,a,b,w,h,3,t,out,0,0);}
int pk_interpolate_tagged(void* p,const uint8_t* a,const uint8_t* b,int w,int h,float t,uint8_t* out,uint64_t aid,uint64_t bid){return interpolate_impl(p,a,b,w,h,3,t,out,aid,bid);}
int pk_interpolate_bgra(void* p,const uint8_t* a,const uint8_t* b,int w,int h,float t,uint8_t* out,uint64_t aid,uint64_t bid){return interpolate_impl(p,a,b,w,h,4,t,out,aid,bid);}
// a second RIFE network for a different flow scale, so a 30 fps frame can mix half-size flow (large
// motion) with full-size flow (thin objects). Ground truth a test game: half fwd+rev 24.01 dB, plus one full pass at 1/5 weight 24.22.
int pk_interpolate_bgra_aux(void* p,const uint8_t* a,const uint8_t* b,int w,int h,float t,uint8_t* output,uint64_t aid,uint64_t bid,float scale){
 error.clear();auto* e=static_cast<Engine*>(p);
 if(!e||!e->rife||!a||!b||!output||w<32||h<32||w>4096||h>2160||!std::isfinite(t)||t<=0||t>=1||!(scale>0&&scale<=1)){error="Invalid aux request";return -4;}
 if(!e->aux||e->auxScale!=scale){
  try{std::lock_guard<std::mutex> guard(lifecycle);
   std::unique_ptr<RIFE> r(new RIFE(e->gpu,false,false,false,1,false,true,scale<.3f?128:scale<.4f?256:scale<.6f?64:scale<1.f?128:pk_models[e->model].padding));
   r->flow_half=scale<1.f;r->flow_scale=scale;r->fp16_arith=(gpu_options.load()&2)!=0;if(r->load(e->path)){error="Aux model load failed";return -2;}
   e->aux=std::move(r);e->auxScale=scale;}catch(...){error="Aux model initialization exception";return -3;}
 }
 LARGE_INTEGER start,end,freq;QueryPerformanceFrequency(&freq);QueryPerformanceCounter(&start);
 try{ncnn::Mat in0(w,h,const_cast<uint8_t*>(a),(size_t)4,4);ncnn::Mat in1(w,h,const_cast<uint8_t*>(b),(size_t)4,4);ncnn::Mat out(w,h,output,(size_t)4,4);
  if(!aid||!bid){aid=(1ULL<<63)|++e->untagged;bid=(1ULL<<63)|++e->untagged;}
  int code=e->aux->process_v4_tagged(in0,in1,t,out,aid,bid);QueryPerformanceCounter(&end);e->ms=1000.0*(end.QuadPart-start.QuadPart)/freq.QuadPart;
  if(code){error="Aux inference failed";return -2;}return 0;}catch(...){error="Aux inference exception";return -3;}
}
const char* pk_last_error(){return error.c_str();}
const char* pk_gpu_name(void* p){return p?ncnn::get_gpu_info(static_cast<Engine*>(p)->gpu).device_name():"none";}
const char* pk_model_name(void* p){return p?pk_models[static_cast<Engine*>(p)->model].name:"none";}
double pk_last_ms(void* p){return p?static_cast<Engine*>(p)->ms:-1;}
// Diagnostic: GPU fence wait inside the last pk_interpolate; RIFE does not measure it (-1).
double pk_last_gpu_wait_ms(void* p){return p?static_cast<Engine*>(p)->wait:-1;}
// Takes effect on the next interpolation (the RIFE network is reloaded when the scale changes).
// diagnostics: stage times of the last interpolation in ms (in, gpu, out); -1 when not measured.
int pk_last_stages(void* p,double* out3){if(!p||!out3)return -1;auto* e=static_cast<Engine*>(p);for(int i=0;i<3;i++)out3[i]=e->stage[i];return e->stage[0]<0?1:0;}
const char* pk_gpu_info(void* p){return p?static_cast<Engine*>(p)->info.c_str():"";}
int pk_engine_ids(void* p,uint32_t* vendor,uint32_t* device){if(!p)return -1;const ncnn::GpuInfo& g=ncnn::get_gpu_info(static_cast<Engine*>(p)->gpu);*vendor=g.vendor_id();*device=g.device_id();return 0;}
// 2 = slow GPU, low latency: quarter-size flow from FHD up (see want_scale).
void pk_set_flow_floor(void* p,int fhd_floor){if(p)static_cast<Engine*>(p)->floor=fhd_floor==2||fhd_floor==3?fhd_floor:fhd_floor?1:0;}
void pk_destroy(void* p){
 if(!p)return;std::lock_guard<std::mutex> guard(lifecycle);delete static_cast<Engine*>(p);
 if(users&&!--users)ncnn::destroy_gpu_instance();
}
#ifdef PK_INTERNAL_VFG
// GPU-path prototype: the bench in gpu_interop.cpp drives this engine's RIFE directly.
RIFE* pk_engine_rife(void* p){return p?static_cast<Engine*>(p)->rife.get():nullptr;}
SeqEngine* pk_engine_seq(void* p){return p?static_cast<Engine*>(p)->seq.get():nullptr;} // FSR 3 GPU path
int pk_engine_family(void* p){return p?pk_models[static_cast<Engine*>(p)->model].family:-1;}
// GPU-path step 2c: load the RIFE flow scale for a frame size without a CPU interpolation call.
int pk_engine_prepare(void* p,int w,int h){auto* e=static_cast<Engine*>(p);if(!e||!e->rife)return -1;
 if(want_scale(w,h,e->floor)!=e->scale){try{std::lock_guard<std::mutex> guard(lifecycle);if(load_rife(e,want_scale(w,h,e->floor)))return -2;}catch(...){return -3;}}return 0;}
#endif
