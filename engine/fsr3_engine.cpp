// SPDX-License-Identifier: GPL-3.0-only
// AMD FidelityFX SDK 1.1.4 (MIT) FSR 3 frame interpolation on captured video. Video has no depth
// or game motion vectors, so depth is a constant far plane and motion comes from FidelityFX optical flow.
// that optical flow (8x8 blocks) is also fed as the game motion vectors; before, they were zero. FSR 3 produces the
// midpoint only (2x).
// Runs on its own Vulkan device, separate from the ncnn instance used by RIFE.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vulkan/vulkan.h>
#include <FidelityFX/host/ffx_opticalflow.h>
#include <FidelityFX/host/ffx_frameinterpolation.h>
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include "seq_engines.h"
#include <vector>
#include <algorithm>
#include <memory>
#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>
#include <thread>
#include <mutex>
#include <functional>
#include "thread_pool.h"
int pk_gpu_options_value(); // engine.cpp: settings switches (bit 2: FSR 3 acceleration)

// Test only: per-pass timestamps from the FidelityFX backend (ffx_vk.cpp calls this around every compute dispatch).
static VkQueryPool csPassPool=VK_NULL_HANDLE;static uint32_t csPassCount=0,csPassId[64],csPassX[64],csPassY[64];
void pk_ffx_mark(VkCommandBuffer cb,uint32_t pass,uint32_t x,uint32_t y,int after){
 if(!csPassPool||csPassCount>=128)return;if(!after){csPassId[csPassCount/2]=pass;csPassX[csPassCount/2]=x;csPassY[csPassCount/2]=y;}
 vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,csPassPool,csPassCount++);}
namespace {
struct Image{VkImage img=VK_NULL_HANDLE;VkDeviceMemory mem=VK_NULL_HANDLE;VkImageCreateInfo info{};FfxResource res{};};
struct Buffer{VkBuffer buf=VK_NULL_HANDLE;VkDeviceMemory mem=VK_NULL_HANDLE;void* map=nullptr;};

void barrier(VkCommandBuffer cb,VkImage img,VkImageLayout from,VkImageLayout to){
 VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_MEMORY_READ_BIT;b.dstAccessMask=b.srcAccessMask;b.oldLayout=from;b.newLayout=to;
 b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=img;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
 vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);}

struct Fsr3 final:SeqEngine{
 VkInstance inst=VK_NULL_HANDLE;VkPhysicalDevice phys=VK_NULL_HANDLE;VkDevice dev=VK_NULL_HANDLE;VkQueue queue=VK_NULL_HANDLE;uint32_t family=0;
 VkCommandPool pool=VK_NULL_HANDLE;VkCommandBuffer cb=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;VkPhysicalDeviceMemoryProperties memProps{};
 VkDeviceContext dc{};std::vector<uint8_t> scratch,sharedScratch;FfxInterface iface{},shared{};FfxUInt32 sharedId=0;bool backend=false;
 // Per-resolution state, rebuilt when the frame size changes.
 int w=0,h=0,ch=3;bool contexts=false; // ch: bytes per pixel of the caller's frames
FfxOpticalflowContext of{};FfxFrameInterpolationContext fi{};FfxOpticalflowSharedResourceDescriptions ofr{};FfxResourceInternal ofVec{},ofScd{},dilD{},dilMV{},recD{};
 Image color,out,depth,mv,mvSmall;Buffer up,down;uint64_t frameId=0,last=0;
 // BGRA frames (the live path) are swapped to RGBA on the GPU: copied into a B8G8R8A8 image and blitted into the
 // R8G8B8A8 image FSR 3 reads (and back for the output). The CPU only copies, on persistent threads. Before, the CPU
 // swapped every pixel both ways (0.9 + 0.7 ms per FHD frame on a 9800X3D). PK_TEST_FSR_CPU_SWAP=1 keeps the old path.
 Image inBgra,outBgra;bool gpuSwap=false;PkPool copier;
 // Test only: PK_TEST_FSR_NO_GAME_MV=1 skips Prepare (and the game vector passes in FFX) after a
 // reset; PK_TEST_FSR_OF_HALF=1 runs optical flow on a half-size copy (flow vectors then cover 16 px blocks).
 // when FSR 3 returns the newer frame unchanged (its scene-change path: after a cut, or a large motion it takes for
 // one, for a few frames), the output is replaced by a 50/50 blend of the pair. full 60.mp4 (280 pairs): 8 of the 9
 // pairs more than 3 dB below a blend were exact copies of the newer frame; the whole video scored 30.98 dB vs 34.09 for a
 // blend. PK_TEST_FSR_NO_BLEND_FALLBACK=1 keeps the copy (A/B). sceneFallbacks counts replaced frames.
 bool blendFallback=true;uint64_t sceneFallbacks=0;
 // (FSR 3 looks steadier, wanted at 4x): FSR 3 only makes the midpoint, so 4x halves twice. The midpoint M of
 // (A,B) comes from this context (the normal A,B,C stream) and is kept for the .5 job; a second FSR 3 context is fed the
 // half-step stream A,M,B,M2,C... so its midpoints are the .25 (A,M) and .75 (M,B) pictures without a reset in between.
 // 3x has no midpoint form and stays refused. PK_TEST_FSR_NO_QUARTER=1 keeps 2x only.
 uint32_t vid=0,did=0;std::unique_ptr<SeqEngine> quarter;std::vector<uint8_t> mid;uint64_t midA=0,midB=0;bool quarterOff=false;
 bool noGame=false,ofHalf=false,renderHalf=false;Image colorHalf; // renderHalf (PK_TEST_FSR_RENDER_HALF=1): depth/game-vector passes at half size
 // zero motion vectors scored 36.21 dB (moving areas 25.72); the optical flow
 // fed as motion vectors 38.03 (27.83). A dense CPU flow reached 39.12, so the
 // gain is the motion itself, not its resolution (flow cut to 8x8 blocks: 39.01). The flow runs first, its block vectors
 // (ow x oh, integer pixels) are read back, converted to half floats and uploaded, and the game-vector passes run at that
 // block size (renderSize = ow x oh), so nothing is expanded per pixel. Vectors point from the current frame to the previous
 // one (prev = cur + mv); the opposite sign scored 35.0. PK_TEST_FSR_ZERO_MV=1 restores the zero vectors (A/B).
 bool flowMv=true;Buffer ofDown,mvUp;uint32_t ow=0,oh=0,ru=0,rh=0;int mvUpK=1;bool mvMed=false;std::vector<float> mvTmp;
 // block vectors are expanded 4x bilinearly by default; 37.91 -> 38.05 dB,
 // visible 8x8 block edges on fast arms/shields gone, +2.8 ms on RTX 5080. Original note: PK_TEST_FSR_MV_UP=2|4 expands the 8x8 block vectors bilinearly (ru x rh = ow*k x oh*k) so block edges do not
 // show as steps; PK_TEST_FSR_MV_MED=1 first applies a 3x3 median on the block grid.
 static uint16_t toHalf(float f){uint32_t x;memcpy(&x,&f,4);const uint32_t sg=(x>>16)&0x8000u;const int e=(int)((x>>23)&0xff)-112;const uint32_t m=x&0x7fffffu;
  if(e<=0)return (uint16_t)sg;if(e>=31)return (uint16_t)(sg|0x7c00u);return (uint16_t)(sg|((uint32_t)e<<10)|(m>>13));}
 static bool flag(const wchar_t* n){wchar_t v[4];return GetEnvironmentVariableW(n,v,4)==1&&v[0]==L'1';}
 // Test only (PK_TEST_FSR_PROFILE=<file>): GPU timestamps per fed frame, appended as
 // up,prepare,optical_flow,interpolation,readback (ms). speed check.
 VkQueryPool qp=VK_NULL_HANDLE;double tsPeriod=0;FILE* profile=nullptr;
 void mark(uint32_t i){if(qp)vkCmdWriteTimestamp(cb,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,qp,i);}
 void profileStart(){if(!profile){wchar_t f[260];if(GetEnvironmentVariableW(L"PK_TEST_FSR_PROFILE",f,260)-1u<259u)profile=_wfopen(f,L"a");}
  if(profile&&!qp){VkPhysicalDeviceProperties pp;vkGetPhysicalDeviceProperties(phys,&pp);tsPeriod=pp.limits.timestampPeriod;
   VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qi.queryType=VK_QUERY_TYPE_TIMESTAMP;qi.queryCount=6;if(vkCreateQueryPool(dev,&qi,nullptr,&qp))qp=VK_NULL_HANDLE;
   qi.queryCount=128;if(qp&&vkCreateQueryPool(dev,&qi,nullptr,&csPassPool))csPassPool=VK_NULL_HANDLE;}
  if(qp)vkCmdResetQueryPool(cb,qp,0,6);if(csPassPool)vkCmdResetQueryPool(cb,csPassPool,0,128);csPassCount=0;}
 void profileEnd(bool output){if(!qp||!profile)return;uint64_t t[6];if(vkGetQueryPoolResults(dev,qp,0,6,sizeof t,t,8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT))return;
  if(csPassPool&&csPassCount>=2&&output){uint64_t q[128];uint32_t n=csPassCount&~1u;if(!vkGetQueryPoolResults(dev,csPassPool,0,n,sizeof q,q,8,VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT))
   for(uint32_t i=0;i<n;i+=2)fprintf(profile,"pass,%u,%u,%u,%u,%.3f\n",i/2,csPassId[i/2],csPassX[i/2],csPassY[i/2],(q[i+1]-q[i])*tsPeriod/1e6);}
  auto d=[&](int a,int b){return (t[b]-t[a])*tsPeriod/1e6;};fprintf(profile,"%d,%.3f,%.3f,%.3f,%.3f,%.3f\n",output?1:0,d(0,1),d(1,2),d(2,3),d(3,4),d(4,5));fflush(profile);}

 // GPU path: host D3D11 textures imported as Vulkan images (GENERAL layout).
 struct GSlot{VkImage img=VK_NULL_HANDLE;VkDeviceMemory mem=VK_NULL_HANDLE;};GSlot gslot[8];int gslots=0;VkImage gIn=VK_NULL_HANDLE,gOut=VK_NULL_HANDLE;
 void gblit(VkImage from,VkImageLayout fl,VkImage to,VkImageLayout tl){VkImageBlit b{};b.srcSubresource=b.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
  b.srcOffsets[1]=b.dstOffsets[1]={w,h,1};vkCmdBlitImage(cb,from,fl,to,tl,1,&b,VK_FILTER_NEAREST);}
 void gfree(){for(int i=0;i<gslots;i++){if(gslot[i].img)vkDestroyImage(dev,gslot[i].img,nullptr);if(gslot[i].mem)vkFreeMemory(dev,gslot[i].mem,nullptr);gslot[i]=GSlot{};}gslots=0;}
 bool blitFormat(VkFormat f){VkFormatProperties p;vkGetPhysicalDeviceFormatProperties(phys,f,&p);
  const VkFormatFeatureFlags need=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  return (p.optimalTilingFeatures&need)==need;}
 bool plainImage(Image& im,VkFormat fmt){
  im.info={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};im.info.imageType=VK_IMAGE_TYPE_2D;im.info.format=fmt;im.info.extent={(uint32_t)w,(uint32_t)h,1};im.info.mipLevels=1;im.info.arrayLayers=1;im.info.samples=VK_SAMPLE_COUNT_1_BIT;
  im.info.tiling=VK_IMAGE_TILING_OPTIMAL;im.info.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if(vkCreateImage(dev,&im.info,nullptr,&im.img))return false;VkMemoryRequirements req;vkGetImageMemoryRequirements(dev,im.img,&req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=memType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  return ai.memoryTypeIndex!=UINT32_MAX&&!vkAllocateMemory(dev,&ai,nullptr,&im.mem)&&!vkBindImageMemory(dev,im.img,im.mem,0);}
 void blit(VkImage from,VkImage to){VkImageBlit b{};b.srcSubresource=b.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
  b.srcOffsets[1]=b.dstOffsets[1]={w,h,1};vkCmdBlitImage(cb,from,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,to,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&b,VK_FILTER_NEAREST);}
 void copy(uint8_t* d,const uint8_t* s,size_t n){copier.run(n,4096,[=](size_t a,size_t b){memcpy(d+a,s+a,b-a);});}
 // FSR 3 leaves its own alpha in the output; the old CPU swap wrote 255, so the readback copy sets it (every pixel differed).
 void copyOpaque(uint8_t* d,const uint8_t* s,size_t n){copier.run(n/4,1024,[=](size_t a,size_t b){const uint32_t* x=(const uint32_t*)s;uint32_t* y=(uint32_t*)d;for(size_t i=a;i<b;i++)y[i]=x[i]|0xFF000000u;});}

 ~Fsr3(){if(dev){vkDeviceWaitIdle(dev);gfree();release();if(backend)shared.fpDestroyBackendContext(&shared,sharedId);if(fence)vkDestroyFence(dev,fence,nullptr);if(pool)vkDestroyCommandPool(dev,pool,nullptr);vkDestroyDevice(dev,nullptr);}if(inst)vkDestroyInstance(inst,nullptr);}
 uint32_t memType(uint32_t bits,VkMemoryPropertyFlags want){for(uint32_t i=0;i<memProps.memoryTypeCount;i++)if((bits&(1u<<i))&&(memProps.memoryTypes[i].propertyFlags&want)==want)return i;return UINT32_MAX;}
 bool makeImage(Image& im,VkFormat fmt,const wchar_t* name,FfxResourceUsage usage,int iw=0,int ih=0){
  im.info={VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};im.info.imageType=VK_IMAGE_TYPE_2D;im.info.format=fmt;im.info.extent={(uint32_t)(iw?iw:w),(uint32_t)(ih?ih:h),1};im.info.mipLevels=1;im.info.arrayLayers=1;im.info.samples=VK_SAMPLE_COUNT_1_BIT;
  im.info.tiling=VK_IMAGE_TILING_OPTIMAL;im.info.usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  if(vkCreateImage(dev,&im.info,nullptr,&im.img))return false;VkMemoryRequirements req;vkGetImageMemoryRequirements(dev,im.img,&req);
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=memType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if(ai.memoryTypeIndex==UINT32_MAX||vkAllocateMemory(dev,&ai,nullptr,&im.mem)||vkBindImageMemory(dev,im.img,im.mem,0))return false;
  im.res=ffxGetResourceVK((void*)im.img,ffxGetImageResourceDescriptionVK(im.img,im.info,usage),name,FFX_RESOURCE_STATE_COMPUTE_READ);return true;}
 // Readback must use host-cached memory: reading write-combined memory on the CPU took ~150 ms per FHD frame.
 bool makeBuffer(Buffer& b,size_t size,bool readback=false){VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=size;bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if(vkCreateBuffer(dev,&bi,nullptr,&b.buf))return false;VkMemoryRequirements req;vkGetBufferMemoryRequirements(dev,b.buf,&req);VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;
  const VkMemoryPropertyFlags visible=VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  ai.memoryTypeIndex=readback?memType(req.memoryTypeBits,visible|VK_MEMORY_PROPERTY_HOST_CACHED_BIT):UINT32_MAX;
  if(ai.memoryTypeIndex==UINT32_MAX)ai.memoryTypeIndex=memType(req.memoryTypeBits,visible);
  return ai.memoryTypeIndex!=UINT32_MAX&&!vkAllocateMemory(dev,&ai,nullptr,&b.mem)&&!vkBindBufferMemory(dev,b.buf,b.mem,0)&&!vkMapMemory(dev,b.mem,0,size,0,&b.map);}
 void freeImage(Image& im){if(im.img)vkDestroyImage(dev,im.img,nullptr);if(im.mem)vkFreeMemory(dev,im.mem,nullptr);im=Image{};}
 void freeBuffer(Buffer& b){if(b.buf)vkDestroyBuffer(dev,b.buf,nullptr);if(b.mem)vkFreeMemory(dev,b.mem,nullptr);b=Buffer{};}
 void release(){
  if(qp){vkDestroyQueryPool(dev,qp,nullptr);qp=VK_NULL_HANDLE;}if(csPassPool){vkDestroyQueryPool(dev,csPassPool,nullptr);csPassPool=VK_NULL_HANDLE;}
  if(contexts){ffxFrameInterpolationContextDestroy(&fi);ffxOpticalflowContextDestroy(&of);for(auto r:{ofVec,ofScd,dilD,dilMV,recD})shared.fpDestroyResource(&shared,r,sharedId);contexts=false;}
  for(Image* im:{&color,&out,&depth,&mv,&inBgra,&outBgra,&colorHalf,&mvSmall})freeImage(*im);gpuSwap=false;freeBuffer(up);freeBuffer(down);freeBuffer(ofDown);freeBuffer(mvUp);ow=oh=0;w=h=0;last=0;}
 bool submit(){if(vkEndCommandBuffer(cb))return false;VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cb;
  return !vkQueueSubmit(queue,1,&si,fence)&&!vkWaitForFences(dev,1,&fence,VK_TRUE,UINT64_MAX)&&!vkResetFences(dev,1,&fence);}
 bool begin(){VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;return !vkBeginCommandBuffer(cb,&bi);}
 bool resize(int fw,int fh){
  // "FSR 3 가속" (settings, default on): renderHalf + noGame (no Prepare after a reset). Video has constant far depth
  // and zero game vectors, so these passes give the same output at half size and need no repeat (same hash).
  // The PK_TEST_FSR_* variables still force each part on for A/B tests; OF_HALF stays test only (output changes).
  {bool fast=(pk_gpu_options_value()&4)!=0;release();w=fw;h=fh;noGame=fast||flag(L"PK_TEST_FSR_NO_PREPARE");ofHalf=flag(L"PK_TEST_FSR_OF_HALF");renderHalf=fast||flag(L"PK_TEST_FSR_RENDER_HALF");blendFallback=!flag(L"PK_TEST_FSR_NO_BLEND_FALLBACK");
   flowMv=!flag(L"PK_TEST_FSR_ZERO_MV");if(flowMv){noGame=false;renderHalf=false;}}
  FfxOpticalflowContextDescription od{};od.backendInterface=iface;od.resolution={(uint32_t)(ofHalf?w/2:w),(uint32_t)(ofHalf?h/2:h)};if(ffxOpticalflowContextCreate(&of,&od))return false;
  FfxFrameInterpolationContextDescription fd{};fd.backendInterface=iface;fd.maxRenderSize={(uint32_t)w,(uint32_t)h};fd.displaySize={(uint32_t)w,(uint32_t)h};
  fd.backBufferFormat=FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;fd.previousInterpolationSourceFormat=FFX_SURFACE_FORMAT_R8G8B8A8_UNORM;if(ffxFrameInterpolationContextCreate(&fi,&fd)){ffxOpticalflowContextDestroy(&of);return false;}
  contexts=true;FfxFrameInterpolationSharedResourceDescriptions fir{};
  if(ffxOpticalflowGetSharedResourceDescriptions(&of,&ofr)||ffxFrameInterpolationGetSharedResourceDescriptions(&fi,&fir))return false;
  if(shared.fpCreateResource(&shared,&ofr.opticalFlowVector,sharedId,&ofVec)||shared.fpCreateResource(&shared,&ofr.opticalFlowSCD,sharedId,&ofScd)||
     shared.fpCreateResource(&shared,&fir.dilatedDepth,sharedId,&dilD)||shared.fpCreateResource(&shared,&fir.dilatedMotionVectors,sharedId,&dilMV)||
     shared.fpCreateResource(&shared,&fir.reconstructedPrevNearestDepth,sharedId,&recD))return false;
  size_t bytes=(size_t)w*h*4;
  if(!makeImage(color,VK_FORMAT_R8G8B8A8_UNORM,L"pk_color",FFX_RESOURCE_USAGE_READ_ONLY)||!makeImage(out,VK_FORMAT_R8G8B8A8_UNORM,L"pk_output",FFX_RESOURCE_USAGE_UAV)||
     !makeImage(depth,VK_FORMAT_R32_SFLOAT,L"pk_depth",FFX_RESOURCE_USAGE_READ_ONLY)||!makeImage(mv,VK_FORMAT_R16G16_SFLOAT,L"pk_motion",FFX_RESOURCE_USAGE_READ_ONLY)||
     !makeBuffer(up,bytes)||!makeBuffer(down,bytes,true))return false;
  if(flowMv){ow=ofr.opticalFlowVector.resourceDescription.width;oh=ofr.opticalFlowVector.resourceDescription.height;
   {wchar_t v[4];mvUpK=GetEnvironmentVariableW(L"PK_TEST_FSR_MV_UP",v,4)==1&&v[0]>=L'1'&&v[0]<=L'8'?v[0]-L'0':4;}mvMed=flag(L"PK_TEST_FSR_MV_MED"); // expanded 4x again, on the GPU (a linear blit): had left it out, the numbers were the same but block edges showed as square pieces beside arms and heads
   ru=std::min<uint32_t>(ow*mvUpK,w);rh=std::min<uint32_t>(oh*mvUpK,h);
   if(mvUpK>1&&!makeImage(mvSmall,VK_FORMAT_R16G16_SFLOAT,L"pk_motion_blocks",FFX_RESOURCE_USAGE_READ_ONLY,(int)ow,(int)oh))return false;
   if(!ow||!oh||ow>(uint32_t)w||oh>(uint32_t)h||!makeBuffer(ofDown,(size_t)ow*oh*4,true)||!makeBuffer(mvUp,(size_t)ru*rh*4))return false;}
  if(ofHalf&&!makeImage(colorHalf,VK_FORMAT_R8G8B8A8_UNORM,L"pk_color_half",FFX_RESOURCE_USAGE_READ_ONLY,w/2,h/2))return false;
  {wchar_t v[4];bool cpu=GetEnvironmentVariableW(L"PK_TEST_FSR_CPU_SWAP",v,4)==1&&v[0]==L'1';
   gpuSwap=!cpu&&blitFormat(VK_FORMAT_B8G8R8A8_UNORM)&&blitFormat(VK_FORMAT_R8G8B8A8_UNORM)&&plainImage(inBgra,VK_FORMAT_B8G8R8A8_UNORM)&&plainImage(outBgra,VK_FORMAT_B8G8R8A8_UNORM);
   if(!gpuSwap){freeImage(inBgra);freeImage(outBgra);}}
  if(!begin())return false;
  VkClearColorValue farDepth{};farDepth.float32[0]=1;VkClearColorValue zero{};VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
  for(Image* im:{&color,&out})barrier(cb,im->img,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  if(ofHalf)barrier(cb,colorHalf.img,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  for(auto p:{std::make_pair(&depth,farDepth),std::make_pair(&mv,zero)}){barrier(cb,p.first->img,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
   vkCmdClearColorImage(cb,p.first->img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&p.second,1,&r);barrier(cb,p.first->img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);}
  return submit();}
 // Feed one frame; when wantOutput, the midpoint between the previous and this frame is read back.
 bool feed(const uint8_t* bgr,bool reset,uint8_t* bgrOut){
  const bool gpu=gpuSwap&&ch==4;
  auto t0=std::chrono::steady_clock::now();if(!gIn){if(gpu)copy((uint8_t*)up.map,bgr,(size_t)w*h*4);else pk_to_4(bgr,ch,(uint8_t*)up.map,(size_t)w*h,true);}if(!begin())return false;
  profileStart();mark(0);
  VkBufferImageCopy c{};c.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};c.imageExtent={(uint32_t)w,(uint32_t)h,1};
  if(gIn){barrier(cb,color.img,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);gblit(gIn,VK_IMAGE_LAYOUT_GENERAL,color.img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);}
  else if(gpu){barrier(cb,inBgra.img,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);vkCmdCopyBufferToImage(cb,up.buf,inBgra.img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&c);
   barrier(cb,inBgra.img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
   barrier(cb,color.img,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);blit(inBgra.img,color.img);}
  else{barrier(cb,color.img,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);vkCmdCopyBufferToImage(cb,up.buf,color.img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&c);}
  if(ofHalf){barrier(cb,color.img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);barrier(cb,colorHalf.img,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
   VkImageBlit hb{};hb.srcSubresource=hb.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};hb.srcOffsets[1]={w,h,1};hb.dstOffsets[1]={w/2,h/2,1};
   vkCmdBlitImage(cb,color.img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,colorHalf.img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&hb,VK_FILTER_LINEAR);
   barrier(cb,colorHalf.img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);barrier(cb,color.img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);}
  else barrier(cb,color.img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);mark(1);
  bool useMv=false,ofDone=false;FfxOpticalflowDispatchDescription o{};
  if(flowMv&&!reset){
   o.commandList=ffxGetCommandListVK(cb);o.color=ofHalf?colorHalf.res:color.res;o.opticalFlowVector=shared.fpGetResource(&shared,ofVec);o.opticalFlowSCD=shared.fpGetResource(&shared,ofScd);
   o.reset=false;o.backbufferTransferFunction=FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;o.minMaxLuminance={0,1};if(ffxOpticalflowContextDispatch(&of,&o))return false;ofDone=true;
   VkImage flow=(VkImage)o.opticalFlowVector.resource;VkBufferImageCopy fc{};fc.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};fc.imageExtent={ow,oh,1};
   barrier(cb,flow,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);vkCmdCopyImageToBuffer(cb,flow,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,ofDown.buf,1,&fc);
   barrier(cb,flow,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL);
   if(!submit()||!begin())return false;
   const int16_t* v=(const int16_t*)ofDown.map;uint16_t* m=(uint16_t*)mvUp.map;const float k=ofHalf?2.f:1.f; // flow vectors are in flow-image pixels
   if(!mvMed)for(size_t i=0,n=(size_t)ow*oh*2;i<n;i++)m[i]=toHalf(v[i]*k);
   else{mvTmp.resize((size_t)ow*oh*2);for(size_t i=0;i<mvTmp.size();i++)mvTmp[i]=v[i]*k;
    for(int y=0;y<(int)oh;y++)for(int x=0;x<(int)ow;x++)for(int c=0;c<2;c++){float t[9];int n=0;
      for(int dy=-1;dy<=1;dy++)for(int dx=-1;dx<=1;dx++){int yy=std::min(std::max(y+dy,0),(int)oh-1),xx=std::min(std::max(x+dx,0),(int)ow-1);t[n++]=mvTmp[((size_t)yy*ow+xx)*2+c];}
      std::nth_element(t,t+4,t+9);m[((size_t)y*ow+x)*2+c]=toHalf(t[4]);}}
   fc.imageExtent={ow,oh,1};
   if(mvUpK>1){ // the GPU expands the block vectors bilinearly
    barrier(cb,mvSmall.img,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);vkCmdCopyBufferToImage(cb,mvUp.buf,mvSmall.img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&fc);
    barrier(cb,mvSmall.img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);barrier(cb,mv.img,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit mb{};mb.srcSubresource=mb.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};mb.srcOffsets[1]={(int)ow,(int)oh,1};mb.dstOffsets[1]={(int)ru,(int)rh,1};
    vkCmdBlitImage(cb,mvSmall.img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,mv.img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&mb,VK_FILTER_LINEAR);}
   else{barrier(cb,mv.img,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);vkCmdCopyBufferToImage(cb,mvUp.buf,mv.img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&fc);}
   barrier(cb,mv.img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);useMv=true;}
  FfxCommandList cl=ffxGetCommandListVK(cb);++frameId;
  FfxFrameInterpolationPrepareDescription pd{};pd.commandList=cl;pd.renderSize=flowMv?FfxDimensions2D{ru,rh}:FfxDimensions2D{(uint32_t)(renderHalf?w/2:w),(uint32_t)(renderHalf?h/2:h)};
  pd.motionVectorScale=useMv?FfxFloatCoords2D{(float)ru/w,(float)rh/h}:FfxFloatCoords2D{0,0}; // scale/renderSize = 1/displaySize
  if(!flag(L"PK_TEST_FSR_OLD_PREPARE")){pd.frameTimeDelta=1000.f/60;pd.cameraNear=0.1f;pd.cameraFar=1000.f;} // these three were swallowed by the comment above (zeros)
  pd.viewSpaceToMetersFactor=1;pd.cameraFovAngleVertical=1.0f;pd.depth=depth.res;pd.motionVectors=mv.res;pd.frameID=frameId;
  pd.dilatedDepth=shared.fpGetResource(&shared,dilD);pd.dilatedMotionVectors=shared.fpGetResource(&shared,dilMV);pd.reconstructedPrevDepth=shared.fpGetResource(&shared,recD);
  if((!noGame||reset)&&ffxFrameInterpolationPrepare(&fi,&pd))return false;mark(2);
  if(!ofDone){o.commandList=cl;o.color=ofHalf?colorHalf.res:color.res;o.opticalFlowVector=shared.fpGetResource(&shared,ofVec);o.opticalFlowSCD=shared.fpGetResource(&shared,ofScd);
  o.reset=reset;o.backbufferTransferFunction=FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;o.minMaxLuminance={0,1};if(ffxOpticalflowContextDispatch(&of,&o))return false;}mark(3);
  FfxFrameInterpolationDispatchDescription d{};d.commandList=cl;d.displaySize={(uint32_t)w,(uint32_t)h};d.renderSize=pd.renderSize;d.currentBackBuffer=color.res;d.output=out.res;d.interpolationRect={0,0,w,h};
  d.opticalFlowVector=o.opticalFlowVector;d.opticalFlowSceneChangeDetection=o.opticalFlowSCD;d.opticalFlowBlockSize=8;d.opticalFlowScale=ofHalf?FfxFloatCoords2D{2.f/w,2.f/h}:FfxFloatCoords2D{1.f/w,1.f/h};
  d.opticalFlowBufferSize={ofr.opticalFlowVector.resourceDescription.width,ofr.opticalFlowVector.resourceDescription.height};d.cameraNear=0.1f;d.cameraFar=1000.f;d.cameraFovAngleVertical=1.0f;
  d.viewSpaceToMetersFactor=1;d.frameTimeDelta=1000.f/60;d.reset=reset;d.backBufferTransferFunction=FFX_BACKBUFFER_TRANSFER_FUNCTION_SRGB;d.minMaxLuminance[0]=0;d.minMaxLuminance[1]=1;d.frameID=frameId;
  d.dilatedDepth=pd.dilatedDepth;d.dilatedMotionVectors=pd.dilatedMotionVectors;d.reconstructedPrevDepth=pd.reconstructedPrevDepth;if(ffxFrameInterpolationDispatch(&fi,&d))return false;mark(4);
  if(bgrOut&&gOut){barrier(cb,out.img,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);gblit(out.img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,gOut,VK_IMAGE_LAYOUT_GENERAL);
   barrier(cb,out.img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);}
  else if(bgrOut&&gpu){barrier(cb,out.img,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);barrier(cb,outBgra.img,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
   blit(out.img,outBgra.img);barrier(cb,out.img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
   barrier(cb,outBgra.img,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);vkCmdCopyImageToBuffer(cb,outBgra.img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,down.buf,1,&c);}
  else if(bgrOut){barrier(cb,out.img,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);vkCmdCopyImageToBuffer(cb,out.img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,down.buf,1,&c);
   barrier(cb,out.img,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);}
  mark(5);auto t1=std::chrono::steady_clock::now();if(!submit())return false;auto t2=std::chrono::steady_clock::now();profileEnd(bgrOut!=nullptr);
  if(bgrOut&&!gOut){if(gpu)copyOpaque(bgrOut,(const uint8_t*)down.map,(size_t)w*h*4);else pk_from_4((const uint8_t*)down.map,bgrOut,ch,(size_t)w*h,true);}auto t3=std::chrono::steady_clock::now();
  using ms=std::chrono::duration<double,std::milli>;stage[0]+=ms(t1-t0).count();stage[1]+=ms(t2-t1).count();stage[2]+=ms(t3-t2).count();return true;}
 int process(const uint8_t* a,const uint8_t* b,int fw,int fh,int pixelBytes,float t,uint8_t* o,uint64_t aid,uint64_t bid)override{
  ch=pixelBytes;const size_t bytes=(size_t)fw*fh*pixelBytes;
  if(!quarterOff&&aid&&bid&&(std::fabs(t-.25f)<.06f||std::fabs(t-.75f)<.06f)){
   auto q0=std::chrono::steady_clock::now();
   if(midA!=aid||midB!=bid){mid.resize(bytes);int c=midpoint(a,b,fw,fh,mid.data(),aid,bid);if(c)return c;midA=aid;midB=bid;}
   if(!quarter){std::string err;quarter.reset(pk_create_fsr3(vid,did,err));if(!quarter){quarterOff=true;return -4;}}
   const uint64_t mId=((aid*0x9E3779B97F4A7C15ull)^(bid<<1))|1ull;
   int c=t<.5f?quarter->process(a,mid.data(),fw,fh,pixelBytes,.5f,o,aid,mId):quarter->process(mid.data(),b,fw,fh,pixelBytes,.5f,o,mId,bid);
   for(int i=0;i<3;i++)stage[i]=quarter->stage[i];wait=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-q0).count();return c;}
  if(aid&&bid&&midA==aid&&midB==bid&&std::fabs(t-.5f)<=.1f&&mid.size()==bytes){memcpy(o,mid.data(),bytes);wait=0;stage[0]=-1;return 0;}
  int c=midpoint(a,b,fw,fh,o,aid,bid,t);if(!c&&aid&&bid&&std::fabs(t-.5f)<=.1f){mid.assign(o,o+bytes);midA=aid;midB=bid;}return c;}
 int midpoint(const uint8_t* a,const uint8_t* b,int fw,int fh,uint8_t* o,uint64_t aid,uint64_t bid,float t=.5f){
  // FSR 3 only has the midpoint; drifted phases near an end return that source frame.
  if(t<.25f||t>.75f){memcpy(o,t<.25f?a:b,(size_t)fw*fh*ch);wait=0;stage[0]=-1;return 0;}
  if(std::fabs(t-.5f)>.1f)return -4; // 1/3, 2/3 etc. are not midpoints
  if(fw!=w||fh!=h){if(!resize(fw,fh)){release();return -2;}}
  auto start=std::chrono::steady_clock::now();stage[0]=stage[1]=stage[2]=0;
  // Optical flow reports a scene change for its first frames and after a reset; the output is
  // then a plain blend until the history settles.
  if(!aid||last!=aid){if(!feed(a,true,nullptr))return -2;}
  if(!feed(b,false,o))return -2;last=aid?bid:0;
  if(blendFallback){
   // Sampled check (4093 odd-strided pixels, colour only): equal to b everywhere sampled means the copy path; a and b must
   // differ there too, or a still picture would be re-blended for nothing.
   const size_t px=(size_t)fw*fh,step=px/4093?px/4093:1;bool same=true,moving=false;
   for(size_t i=0;i<px&&same;i+=step){const uint8_t *x=o+i*ch,*y=b+i*ch,*z=a+i*ch;same=x[0]==y[0]&&x[1]==y[1]&&x[2]==y[2];moving|=z[0]!=y[0]||z[1]!=y[1]||z[2]!=y[2];}
   if(same&&moving){++sceneFallbacks;const size_t n=px*ch;
    copier.run(n,4096,[=](size_t s0,size_t s1){for(size_t i=s0;i<s1;i++)o[i]=(uint8_t)((a[i]+b[i]+1)>>1);});
    if(ch==4)copier.run(px,1024,[=](size_t s0,size_t s1){for(size_t i=s0;i<s1;i++)o[i*4+3]=255;});}
  }
  wait=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();return 0;}
};
}

SeqEngine* pk_create_fsr3(uint32_t vendorId,uint32_t deviceId,std::string& error){
 std::unique_ptr<Fsr3> f(new Fsr3);f->vid=vendorId;f->did=deviceId;f->quarterOff=Fsr3::flag(L"PK_TEST_FSR_NO_QUARTER");
 VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_3;VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ii.pApplicationInfo=&app;
 if(vkCreateInstance(&ii,nullptr,&f->inst)){f->inst=VK_NULL_HANDLE;error="Vulkan 1.3 instance unavailable";return nullptr;}
 uint32_t n=0;vkEnumeratePhysicalDevices(f->inst,&n,nullptr);std::vector<VkPhysicalDevice> pd(n);vkEnumeratePhysicalDevices(f->inst,&n,pd.data());
 for(auto p:pd){VkPhysicalDeviceProperties pp;vkGetPhysicalDeviceProperties(p,&pp);if(pp.vendorID==vendorId&&pp.deviceID==deviceId){f->phys=p;break;}}
 if(!f->phys){error="No Vulkan GPU";return nullptr;}
 VkPhysicalDeviceProperties props;vkGetPhysicalDeviceProperties(f->phys,&props);if(props.apiVersion<VK_API_VERSION_1_3){error="FSR 3 needs a Vulkan 1.3 GPU driver";return nullptr;}
 vkGetPhysicalDeviceMemoryProperties(f->phys,&f->memProps);
 uint32_t qn=0;vkGetPhysicalDeviceQueueFamilyProperties(f->phys,&qn,nullptr);std::vector<VkQueueFamilyProperties> qf(qn);vkGetPhysicalDeviceQueueFamilyProperties(f->phys,&qn,qf.data());
 f->family=UINT32_MAX;for(uint32_t i=0;i<qn;i++)if((qf[i].queueFlags&(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT))==(VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT)){f->family=i;break;}
 if(f->family==UINT32_MAX){error="No compute queue";return nullptr;}
 // The FidelityFX backend picks shader permutations from supported features, so enable them all.
 VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};f12.pNext=&f13;
 VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};f11.pNext=&f12;VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};f2.pNext=&f11;
 vkGetPhysicalDeviceFeatures2(f->phys,&f2);f2.features.robustBufferAccess=VK_FALSE;
 uint32_t en=0;vkEnumerateDeviceExtensionProperties(f->phys,nullptr,&en,nullptr);std::vector<VkExtensionProperties> ep(en);vkEnumerateDeviceExtensionProperties(f->phys,nullptr,&en,ep.data());
 std::vector<const char*> ext;for(const char* want:{VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME,VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME,VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
  VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME
#ifdef PK_INTERNAL_VFG
  ,"VK_KHR_external_memory_win32" /* GPU path */
#endif
  })for(auto& e:ep)if(!strcmp(e.extensionName,want))ext.push_back(want);
 float prio=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=f->family;qi.queueCount=1;qi.pQueuePriorities=&prio;
 VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.pNext=&f2;di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;di.enabledExtensionCount=(uint32_t)ext.size();di.ppEnabledExtensionNames=ext.data();
 if(vkCreateDevice(f->phys,&di,nullptr,&f->dev)){f->dev=VK_NULL_HANDLE;error="Vulkan device creation failed";return nullptr;}
 vkGetDeviceQueue(f->dev,f->family,0,&f->queue);
 VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};ci.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;ci.queueFamilyIndex=f->family;
 VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
 if(vkCreateCommandPool(f->dev,&ci,nullptr,&f->pool)){error="Vulkan command pool failed";return nullptr;}ca.commandPool=f->pool;
 if(vkAllocateCommandBuffers(f->dev,&ca,&f->cb)||vkCreateFence(f->dev,&fci,nullptr,&f->fence)){error="Vulkan command buffer failed";return nullptr;}
 f->dc={f->dev,f->phys,vkGetDeviceProcAddr};FfxDevice dev=ffxGetDeviceVK(&f->dc);
 // +64: the vendored backend aligns its scratch arrays to 16 bytes (see THIRD_PARTY_NOTICES).
 size_t size=ffxGetScratchMemorySizeVK(f->phys,4)+64,sharedSize=ffxGetScratchMemorySizeVK(f->phys,1)+64;f->scratch.resize(size);f->sharedScratch.resize(sharedSize);
 if(ffxGetInterfaceVK(&f->iface,dev,f->scratch.data(),size,4)||ffxGetInterfaceVK(&f->shared,dev,f->sharedScratch.data(),sharedSize,1)||
    f->shared.fpCreateBackendContext(&f->shared,FFX_EFFECT_SHAREDRESOURCES,nullptr,&f->sharedId)){error="FidelityFX backend initialization failed";return nullptr;}
 f->backend=true;return f.release();
}

#ifdef PK_INTERNAL_VFG
#include <vulkan/vulkan_win32.h>
int pk_engine_family(void* p);
// GPU path: same slot API as pk_gpu_ctx_* (RIFE). The application's engine handle owns the FSR 3 context.
SeqEngine* pk_engine_seq(void* p);
static thread_local std::string fsr_gpu_err;
extern "C" __declspec(dllexport) const char* pk_fsr3_gpu_error(){return fsr_gpu_err.c_str();}
extern "C" __declspec(dllexport) void* pk_fsr3_gpu_ctx_create(void* engine,int w,int h,uint8_t* luid8){
 fsr_gpu_err.clear();auto* f=pk_engine_family(engine)==3?static_cast<Fsr3*>(pk_engine_seq(engine)):nullptr;if(!f){fsr_gpu_err="no FSR 3 engine";return nullptr;}
 f->ch=4;if(f->w!=w||f->h!=h){if(!f->resize(w,h)){f->release();fsr_gpu_err="resize";return nullptr;}}
 if(!f->gpuSwap){fsr_gpu_err="BGRA blit unsupported";return nullptr;}
 VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};p2.pNext=&idp;
 vkGetPhysicalDeviceProperties2(f->phys,&p2);if(!idp.deviceLUIDValid){fsr_gpu_err="no LUID";return nullptr;}if(luid8)memcpy(luid8,idp.deviceLUID,8);
 if(!vkGetDeviceProcAddr(f->dev,"vkGetMemoryWin32HandlePropertiesKHR")){fsr_gpu_err="external memory win32 not enabled";return nullptr;}
 return f;
}
extern "C" __declspec(dllexport) int pk_fsr3_gpu_ctx_import(void* p,HANDLE handle){
 auto* f=static_cast<Fsr3*>(p);if(!f||f->gslots>=8)return -1;auto& s=f->gslot[f->gslots];
 auto props=(PFN_vkGetMemoryWin32HandlePropertiesKHR)vkGetDeviceProcAddr(f->dev,"vkGetMemoryWin32HandlePropertiesKHR");
 VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};ext.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
 VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ic.pNext=&ext;ic.imageType=VK_IMAGE_TYPE_2D;ic.format=VK_FORMAT_B8G8R8A8_UNORM;ic.extent={(uint32_t)f->w,(uint32_t)f->h,1};
 ic.mipLevels=1;ic.arrayLayers=1;ic.samples=VK_SAMPLE_COUNT_1_BIT;ic.tiling=VK_IMAGE_TILING_OPTIMAL;ic.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;ic.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
 if(vkCreateImage(f->dev,&ic,nullptr,&s.img)){fsr_gpu_err="vkCreateImage";return -2;}
 VkMemoryRequirements mr;vkGetImageMemoryRequirements(f->dev,s.img,&mr);VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
 if(!props||props(f->dev,VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,handle,&hp)){fsr_gpu_err="handle properties";return -3;}
 uint32_t bits=mr.memoryTypeBits&hp.memoryTypeBits,type=0;if(!bits){fsr_gpu_err="memory type";return -4;}while(!(bits&(1u<<type)))type++;
 VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};ded.image=s.img;
 VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};imp.pNext=&ded;imp.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;imp.handle=handle;
 VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.pNext=&imp;ma.allocationSize=mr.size;ma.memoryTypeIndex=type;
 if(vkAllocateMemory(f->dev,&ma,nullptr,&s.mem)||vkBindImageMemory(f->dev,s.img,s.mem,0)){fsr_gpu_err="import memory";return -5;}
 if(!f->begin())return -6;barrier(f->cb,s.img,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);if(!f->submit()){fsr_gpu_err="layout";return -7;}
 return f->gslots++;
}
extern "C" __declspec(dllexport) int pk_fsr3_gpu_ctx_generate(void* p,int a,uint64_t aid,int b,uint64_t bid,int out,float t,double* ms3){
 auto* f=static_cast<Fsr3*>(p);if(!f||a<0||b<0||out<0||a>=f->gslots||b>=f->gslots||out>=f->gslots)return -1;if(t<.499f||t>.501f){fsr_gpu_err="midpoint only";return -4;}
 auto t0=std::chrono::steady_clock::now();uint8_t dummy=0;bool ok=true;f->stage[0]=f->stage[1]=f->stage[2]=0;
 if(!aid||f->last!=aid){f->gIn=f->gslot[a].img;f->gOut=VK_NULL_HANDLE;ok=f->feed(nullptr,true,nullptr);}
 if(ok){f->gIn=f->gslot[b].img;f->gOut=f->gslot[out].img;ok=f->feed(nullptr,false,&dummy);}
 f->gIn=f->gOut=VK_NULL_HANDLE;if(!ok){f->last=0;fsr_gpu_err="feed";return -2;}f->last=bid;
 if(ms3){ms3[0]=0;ms3[1]=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();ms3[2]=0;}return 0;
}
extern "C" __declspec(dllexport) void pk_fsr3_gpu_ctx_destroy(void* p){auto* f=static_cast<Fsr3*>(p);if(!f)return;vkDeviceWaitIdle(f->dev);f->gfree();}
#endif

// Pixel conversions.
// conversions run on one persistent pool (three threads were created per call). Never freed: no thread
// joins during DLL unload.
static void pk_par(size_t n,const std::function<void(size_t,size_t)>& f){static std::mutex m;static PkPool* p=new PkPool;std::lock_guard<std::mutex> l(m);p->run(n,64,f);}
void pk_bgr_to_4(const uint8_t* s,uint8_t* d,size_t n,bool swap){
 auto job=[=](size_t a,size_t b){for(size_t p=a;p<b;p++){d[p*4]=s[p*3+(swap?2:0)];d[p*4+1]=s[p*3+1];d[p*4+2]=s[p*3+(swap?0:2)];d[p*4+3]=255;}};
 pk_par(n,job);}
void pk_4_to_bgr(const uint8_t* s,uint8_t* d,size_t n,bool swap){
 auto job=[=](size_t a,size_t b){for(size_t p=a;p<b;p++){d[p*3]=s[p*4+(swap?2:0)];d[p*3+1]=s[p*4+1];d[p*3+2]=s[p*4+(swap?0:2)];}};
 pk_par(n,job);}
static void pk_swap_4(const uint8_t* s,uint8_t* d,size_t n){
 // Whole 32-bit pixels (little endian B,G,R,A): swap bytes 0 and 2, alpha 255. The compiler vectorizes
 // this; the byte-wise loop made FSR 3 BGRA uploads slower than the bgr24 conversion at UHD.
 auto job=[=](size_t a,size_t b){for(size_t p=a;p<b;p++){uint32_t v;memcpy(&v,s+p*4,4);v=(v&0x0000FF00u)|((v>>16)&0xFFu)|((v&0xFFu)<<16)|0xFF000000u;memcpy(d+p*4,&v,4);}};
 pk_par(n,job);}
void pk_to_4(const uint8_t* s,int ch,uint8_t* d,size_t n,bool swap){if(ch==3)pk_bgr_to_4(s,d,n,swap);else if(swap)pk_swap_4(s,d,n);else memcpy(d,s,n*4);}
void pk_from_4(const uint8_t* s,uint8_t* d,int ch,size_t n,bool swap){if(ch==3)pk_4_to_bgr(s,d,n,swap);else if(swap)pk_swap_4(s,d,n);else memcpy(d,s,n*4);}
