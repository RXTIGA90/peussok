// SPDX-License-Identifier: GPL-3.0-only
// GPU-path prototype. Measures a frame path with no CPU copies:
//   D3D11 texture (stands in for the application's frame) --CopyResource--> D3D11 shared texture
//   --imported as VkImage--> vkCmdCopyImageToBuffer --> ncnn VkMat --> RIFE (same shaders/network)
//   --> packed VkMat --> vkCmdCopyBufferToImage --> D3D11 shared output texture --CopyResource--> D3D11 texture.
// Synchronisation is CPU-side (D3D11 event query / Vulkan fence) to keep the prototype simple; the
// numbers therefore include those waits. Output bytes are read back once at the end for comparison.
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <vulkan/vulkan.h>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <string>
#include <functional>
#include "gpu.h"
#include "mat.h"
#include "rife.h"

RIFE* pk_engine_rife(void* p);
static std::string g_err;
extern "C" __declspec(dllexport) const char* pk_gpu_bench_error(){return g_err.c_str();}

namespace {
template<class T> void rel(T*& p){if(p){p->Release();p=nullptr;}}
struct Shared { ID3D11Texture2D* tex=nullptr; HANDLE h=nullptr; VkImage img=VK_NULL_HANDLE; VkDeviceMemory mem=VK_NULL_HANDLE; };
using clk=std::chrono::steady_clock;
double ms(clk::time_point a,clk::time_point b){return std::chrono::duration<double,std::milli>(b-a).count();}
}

// stats[0..6]: d3d copy-in, vk copy-in, rife, vk copy-out, d3d copy-out, total (ms, mean after warm-up), iterations measured.
extern "C" __declspec(dllexport) int pk_gpu_bench(void* engine,const uint8_t* a,const uint8_t* b,int w,int h,int iters,uint8_t* out,double* stats){
    g_err.clear();
    RIFE* r=pk_engine_rife(engine);if(!r){g_err="no RIFE (call pk_interpolate_bgra once first)";return -1;}
    ncnn::VulkanDevice* vd=r->device();VkDevice dev=vd->vkdevice();VkPhysicalDevice pd=vd->info.physical_device();
    // Same GPU on both APIs: match the adapter LUID.
    VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};p2.pNext=&idp;vkGetPhysicalDeviceProperties2(pd,&p2);
    if(!idp.deviceLUIDValid){g_err="Vulkan device has no LUID";return -2;}
    IDXGIFactory1* fac=nullptr;if(FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),(void**)&fac))){g_err="DXGI factory";return -3;}
    IDXGIAdapter1* ad=nullptr;
    for(UINT i=0;fac->EnumAdapters1(i,&ad)!=DXGI_ERROR_NOT_FOUND;i++){DXGI_ADAPTER_DESC1 d;ad->GetDesc1(&d);
        if(!memcmp(&d.AdapterLuid,idp.deviceLUID,8))break;rel(ad);}
    rel(fac);if(!ad){g_err="no DXGI adapter with the Vulkan LUID";return -4;}
    ID3D11Device* d3d=nullptr;ID3D11DeviceContext* ctx=nullptr;D3D_FEATURE_LEVEL fl;
    HRESULT hr=D3D11CreateDevice(ad,D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d3d,&fl,&ctx);rel(ad);
    if(FAILED(hr)){g_err="D3D11CreateDevice";return -5;}
    auto PFN_props=(PFN_vkGetMemoryWin32HandlePropertiesKHR)vkGetDeviceProcAddr(dev,"vkGetMemoryWin32HandlePropertiesKHR");
    if(!PFN_props){g_err="VK_KHR_external_memory_win32 not enabled on the ncnn device";rel(ctx);rel(d3d);return -6;}

    D3D11_TEXTURE2D_DESC td{};td.Width=w;td.Height=h;td.MipLevels=1;td.ArraySize=1;td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    // Source frames (what the host would hand us) and the host-side output texture.
    ID3D11Texture2D *src[2]={},*dst=nullptr,*stage=nullptr;
    D3D11_SUBRESOURCE_DATA sd[2]={{a,(UINT)w*4,0},{b,(UINT)w*4,0}};
    for(int i=0;i<2;i++)d3d->CreateTexture2D(&td,&sd[i],&src[i]);d3d->CreateTexture2D(&td,nullptr,&dst);
    D3D11_TEXTURE2D_DESC sdsc=td;sdsc.Usage=D3D11_USAGE_STAGING;sdsc.BindFlags=0;sdsc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;d3d->CreateTexture2D(&sdsc,nullptr,&stage);
    // Shared textures: two inputs, one output.
    D3D11_TEXTURE2D_DESC shd=td;shd.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
    Shared sh[3];int rc=0;
    VkCommandPool pool=VK_NULL_HANDLE;VkCommandBuffer cb=VK_NULL_HANDLE;VkFence fence=VK_NULL_HANDLE;VkQueue q=VK_NULL_HANDLE;
    ID3D11Query* evq=nullptr;D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT,0};d3d->CreateQuery(&qd,&evq);
    ncnn::VkAllocator* alloc=vd->acquire_blob_allocator();
    ncnn::VkMat raw[2],packed;
    auto d3dwait=[&]{ctx->End(evq);ctx->Flush();BOOL done=FALSE;while(ctx->GetData(evq,&done,sizeof done,0)!=S_OK||!done){}};
    uint32_t qf=vd->info.compute_queue_family_index();
    auto submit=[&](auto record)->bool{
        vkResetCommandBuffer(cb,0);VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cb,&bi);record();vkEndCommandBuffer(cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cb;
        vkResetFences(dev,1,&fence);if(vkQueueSubmit(q,1,&si,fence))return false;return vkWaitForFences(dev,1,&fence,VK_TRUE,~0ull)==VK_SUCCESS;};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={(uint32_t)w,(uint32_t)h,1};
    auto bufbar=[&](const ncnn::VkMat& m,VkAccessFlags sa,VkAccessFlags da,VkPipelineStageFlags ss,VkPipelineStageFlags ds){
        VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};bb.srcAccessMask=sa;bb.dstAccessMask=da;bb.srcQueueFamilyIndex=bb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        bb.buffer=m.buffer();bb.offset=m.buffer_offset();bb.size=(VkDeviceSize)w*h*4;vkCmdPipelineBarrier(cb,ss,ds,0,0,nullptr,1,&bb,0,nullptr);};
    double sum[6]={0};int counted=0;
    for(int i=0;i<3&&!rc;i++){
        if(FAILED(d3d->CreateTexture2D(&shd,nullptr,&sh[i].tex))){g_err="shared texture";rc=-7;break;}
        IDXGIResource1* res=nullptr;sh[i].tex->QueryInterface(__uuidof(IDXGIResource1),(void**)&res);
        if(!res||FAILED(res->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&sh[i].h))){rel(res);g_err="CreateSharedHandle";rc=-8;break;}rel(res);
        VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};ext.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ic.pNext=&ext;ic.imageType=VK_IMAGE_TYPE_2D;ic.format=VK_FORMAT_B8G8R8A8_UNORM;
        ic.extent={(uint32_t)w,(uint32_t)h,1};ic.mipLevels=1;ic.arrayLayers=1;ic.samples=VK_SAMPLE_COUNT_1_BIT;ic.tiling=VK_IMAGE_TILING_OPTIMAL;
        ic.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;ic.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        if(vkCreateImage(dev,&ic,nullptr,&sh[i].img)){g_err="vkCreateImage (external)";rc=-9;break;}
        VkMemoryRequirements mr;vkGetImageMemoryRequirements(dev,sh[i].img,&mr);
        VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
        if(PFN_props(dev,VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,sh[i].h,&hp)){g_err="vkGetMemoryWin32HandlePropertiesKHR";rc=-10;break;}
        uint32_t bits=mr.memoryTypeBits&hp.memoryTypeBits,type=0;if(!bits){g_err="no common memory type";rc=-11;break;}while(!(bits&(1u<<type)))type++;
        VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};ded.image=sh[i].img;
        VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};imp.pNext=&ded;imp.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;imp.handle=sh[i].h;
        VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.pNext=&imp;ma.allocationSize=mr.size;ma.memoryTypeIndex=type;
        if(vkAllocateMemory(dev,&ma,nullptr,&sh[i].mem)){g_err="vkAllocateMemory (import)";rc=-12;break;}
        if(vkBindImageMemory(dev,sh[i].img,sh[i].mem,0)){g_err="vkBindImageMemory";rc=-13;break;}
    }
    if(!rc){
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pc.queueFamilyIndex=qf;
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if(vkCreateCommandPool(dev,&pc,nullptr,&pool)){g_err="command pool";rc=-14;}
        else{cai.commandPool=pool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;vkAllocateCommandBuffers(dev,&cai,&cb);vkCreateFence(dev,&fc,nullptr,&fence);}
        for(int i=0;i<2&&!rc;i++){raw[i].create(w,h,(size_t)4,1,alloc);if(raw[i].empty()){g_err="VkMat";rc=-15;}}
    }
    if(!rc){
        q=vd->acquire_queue(qf);
        // Images stay in GENERAL so D3D11 writes are never discarded by a layout transition.
        submit([&]{VkImageMemoryBarrier ib[3];for(int i=0;i<3;i++){ib[i]={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};ib[i].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;ib[i].newLayout=VK_IMAGE_LAYOUT_GENERAL;
            ib[i].srcQueueFamilyIndex=ib[i].dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;ib[i].image=sh[i].img;ib[i].subresourceRange=range;ib[i].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;}
            vkCmdPipelineBarrier(cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,3,ib);});
        // Both sources start on the GPU (first pair), then each iteration brings one new source like a live 2x stream.
        for(int i=0;i<2;i++)ctx->CopyResource(sh[i].tex,src[i]);d3dwait();
        submit([&]{for(int i=0;i<2;i++){region.bufferOffset=raw[i].buffer_offset();vkCmdCopyImageToBuffer(cb,sh[i].img,VK_IMAGE_LAYOUT_GENERAL,raw[i].buffer(),1,&region);}});
        uint64_t id[2]={1,2};
        for(int k=0;k<iters&&!rc;k++){
            int n=k&1; // slot receiving the new source this iteration
            auto t0=clk::now();
            if(k){ctx->CopyResource(sh[n].tex,src[n]);d3dwait();}
            auto t1=clk::now();
            if(k){if(!submit([&]{region.bufferOffset=raw[n].buffer_offset();vkCmdCopyImageToBuffer(cb,sh[n].img,VK_IMAGE_LAYOUT_GENERAL,raw[n].buffer(),1,&region);
                bufbar(raw[n],VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);})){g_err="copy-in submit";rc=-16;break;}
                id[n]=id[n^1]+1;}
            auto t2=clk::now();
            // Older source first so the pair is (previous, new) as in the live path.
            int older=id[0]<id[1]?0:1;
            if(r->process_v4_gpu(raw[older],raw[older^1],0.5f,packed,id[older],id[older^1])){g_err="process_v4_gpu";rc=-17;break;}
            auto t3=clk::now();
            if(!submit([&]{bufbar(packed,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
                region.bufferOffset=packed.buffer_offset();vkCmdCopyBufferToImage(cb,packed.buffer(),sh[2].img,VK_IMAGE_LAYOUT_GENERAL,1,&region);})){g_err="copy-out submit";rc=-18;break;}
            auto t4=clk::now();
            ctx->CopyResource(dst,sh[2].tex);d3dwait();
            auto t5=clk::now();
            if(k>=5){double v[6]={ms(t0,t1),ms(t1,t2),ms(t2,t3),ms(t3,t4),ms(t4,t5),ms(t0,t5)};for(int j=0;j<6;j++)sum[j]+=v[j];counted++;}
        }
        vd->reclaim_queue(qf,q);
    }
    if(!rc&&out){ctx->CopyResource(stage,sh[2].tex);D3D11_MAPPED_SUBRESOURCE m;
        if(SUCCEEDED(ctx->Map(stage,0,D3D11_MAP_READ,0,&m))){for(int y=0;y<h;y++)memcpy(out+(size_t)y*w*4,(uint8_t*)m.pData+(size_t)y*m.RowPitch,(size_t)w*4);ctx->Unmap(stage,0);}}
    if(stats){for(int j=0;j<6;j++)stats[j]=counted?sum[j]/counted:-1;stats[6]=counted;}
    raw[0].release();raw[1].release();packed.release();vd->reclaim_blob_allocator(alloc);
    if(fence)vkDestroyFence(dev,fence,nullptr);if(pool)vkDestroyCommandPool(dev,pool,nullptr);
    for(auto& s:sh){if(s.img)vkDestroyImage(dev,s.img,nullptr);if(s.mem)vkFreeMemory(dev,s.mem,nullptr);if(s.h)CloseHandle(s.h);rel(s.tex);}
    rel(evq);rel(stage);rel(dst);rel(src[0]);rel(src[1]);rel(ctx);rel(d3d);
    return rc;
}

// ---- step 2c: live GPU context used by the application's test mode (PK_TEST_GPU_GEN=1) ----
// The application owns D3D11 shared textures on the host's device and passes their NT handles here; this side imports them
// as Vulkan images. generate(): copy the two source images into RIFE's input buffers (only when their id changed),
// run RIFE, copy the packed result into the output image. Blocking; called from the application's worker thread.
int pk_engine_prepare(void* p,int w,int h);
namespace {
struct GpuSlot { VkImage img=VK_NULL_HANDLE; VkDeviceMemory mem=VK_NULL_HANDLE; ncnn::VkMat raw; uint64_t loaded=0; };
struct GpuCtx {
    void* engine=nullptr; RIFE* r=nullptr; ncnn::VulkanDevice* vd=nullptr; VkDevice dev=VK_NULL_HANDLE; int w=0,h=0;
    ncnn::VkAllocator* alloc=nullptr; VkCommandPool pool=VK_NULL_HANDLE; VkCommandBuffer cb=VK_NULL_HANDLE; VkFence fence=VK_NULL_HANDLE;
    PFN_vkGetMemoryWin32HandlePropertiesKHR props=nullptr; GpuSlot slot[8]; int slots=0; ncnn::VkMat packed; double ms[3]={0,0,0};
};
}
extern "C" __declspec(dllexport) void* pk_gpu_ctx_create(void* engine,int w,int h,uint8_t* luid8){
    g_err.clear();RIFE* r=pk_engine_rife(engine);if(!r){g_err="no RIFE";return nullptr;}
    if(pk_engine_prepare(engine,w,h)){g_err="model load for size";return nullptr;}
    r=pk_engine_rife(engine); // reloaded for the flow scale
    auto* c=new GpuCtx;c->engine=engine;c->r=r;c->vd=r->device();c->dev=c->vd->vkdevice();c->w=w;c->h=h;
    VkPhysicalDeviceIDProperties idp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};p2.pNext=&idp;vkGetPhysicalDeviceProperties2(c->vd->info.physical_device(),&p2);
    if(luid8)memcpy(luid8,idp.deviceLUID,8);
    c->props=(PFN_vkGetMemoryWin32HandlePropertiesKHR)vkGetDeviceProcAddr(c->dev,"vkGetMemoryWin32HandlePropertiesKHR");
    if(!c->props||!idp.deviceLUIDValid){g_err="external memory win32 unavailable";delete c;return nullptr;}
    c->alloc=c->vd->acquire_blob_allocator();
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pc.queueFamilyIndex=c->vd->info.compute_queue_family_index();
    vkCreateCommandPool(c->dev,&pc,nullptr,&c->pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};cai.commandPool=c->pool;cai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;cai.commandBufferCount=1;vkAllocateCommandBuffers(c->dev,&cai,&c->cb);
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};vkCreateFence(c->dev,&fc,nullptr,&c->fence);
    return c;
}
static bool ctx_submit(GpuCtx* c,const std::function<void()>& record){
    vkResetCommandBuffer(c->cb,0);VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(c->cb,&bi);record();vkEndCommandBuffer(c->cb);
    uint32_t qf=c->vd->info.compute_queue_family_index();VkQueue q=c->vd->acquire_queue(qf);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&c->cb;
    vkResetFences(c->dev,1,&c->fence);VkResult rq=vkQueueSubmit(q,1,&si,c->fence);c->vd->reclaim_queue(qf,q);
    return rq==VK_SUCCESS&&vkWaitForFences(c->dev,1,&c->fence,VK_TRUE,~0ull)==VK_SUCCESS;
}
extern "C" __declspec(dllexport) int pk_gpu_ctx_import(void* p,HANDLE handle){
    auto* c=static_cast<GpuCtx*>(p);if(!c||c->slots>=8)return -1;GpuSlot& s=c->slot[c->slots];
    VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};ext.handleTypes=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ic.pNext=&ext;ic.imageType=VK_IMAGE_TYPE_2D;ic.format=VK_FORMAT_B8G8R8A8_UNORM;
    ic.extent={(uint32_t)c->w,(uint32_t)c->h,1};ic.mipLevels=1;ic.arrayLayers=1;ic.samples=VK_SAMPLE_COUNT_1_BIT;ic.tiling=VK_IMAGE_TILING_OPTIMAL;
    ic.usage=VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;ic.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    if(vkCreateImage(c->dev,&ic,nullptr,&s.img)){g_err="vkCreateImage";return -2;}
    VkMemoryRequirements mr;vkGetImageMemoryRequirements(c->dev,s.img,&mr);
    VkMemoryWin32HandlePropertiesKHR hp{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
    if(c->props(c->dev,VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT,handle,&hp)){g_err="handle properties";return -3;}
    uint32_t bits=mr.memoryTypeBits&hp.memoryTypeBits,type=0;if(!bits){g_err="memory type";return -4;}while(!(bits&(1u<<type)))type++;
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};ded.image=s.img;
    VkImportMemoryWin32HandleInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};imp.pNext=&ded;imp.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;imp.handle=handle;
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.pNext=&imp;ma.allocationSize=mr.size;ma.memoryTypeIndex=type;
    if(vkAllocateMemory(c->dev,&ma,nullptr,&s.mem)||vkBindImageMemory(c->dev,s.img,s.mem,0)){g_err="import memory";return -5;}
    s.raw.create(c->w,c->h,(size_t)4,1,c->alloc);if(s.raw.empty()){g_err="VkMat";return -6;}
    VkImage img=s.img;
    if(!ctx_submit(c,[&]{VkImageMemoryBarrier ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};ib.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;ib.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        ib.srcQueueFamilyIndex=ib.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;ib.image=img;ib.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};ib.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT|VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(c->cb,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&ib);})){g_err="layout";return -7;}
    return c->slots++;
}
// change RIFE's flow size while running (worker thread only, no generate in flight). floor as in pk_set_flow_floor.
// Returns 0, or <0 when the network could not be reloaded (the context must then not be used any more).
extern "C" void pk_set_flow_floor(void* p,int fhd_floor);
extern "C" __declspec(dllexport) int pk_gpu_ctx_set_flow(void* p,int floor){
    auto* c=static_cast<GpuCtx*>(p);if(!c||!c->engine)return -1;
    vkDeviceWaitIdle(c->dev);c->packed.release();
    pk_set_flow_floor(c->engine,floor);
    if(pk_engine_prepare(c->engine,c->w,c->h)){g_err="reload for flow size";return -2;}
    c->r=pk_engine_rife(c->engine);return c->r?0:-3;
}
// ms3: copy-in, rife, copy-out (ms). Returns 0 on success.
extern "C" __declspec(dllexport) int pk_gpu_ctx_generate(void* p,int a,uint64_t aid,int b,uint64_t bid,int out,float t,double* ms3){
    auto* c=static_cast<GpuCtx*>(p);if(!c||a<0||b<0||out<0||a>=c->slots||b>=c->slots||out>=c->slots)return -1;
    auto t0=clk::now();
    VkBufferImageCopy region{};region.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};region.imageExtent={(uint32_t)c->w,(uint32_t)c->h,1};
    auto bufbar=[&](const ncnn::VkMat& m,VkAccessFlags sa,VkAccessFlags da,VkPipelineStageFlags ss,VkPipelineStageFlags ds){
        VkBufferMemoryBarrier bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};bb.srcAccessMask=sa;bb.dstAccessMask=da;bb.srcQueueFamilyIndex=bb.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        bb.buffer=m.buffer();bb.offset=m.buffer_offset();bb.size=(VkDeviceSize)c->w*c->h*4;vkCmdPipelineBarrier(c->cb,ss,ds,0,0,nullptr,1,&bb,0,nullptr);};
    int need[2]={c->slot[a].loaded!=aid?a:-1,c->slot[b].loaded!=bid?b:-1};
    if(need[0]>=0||need[1]>=0){
        if(!ctx_submit(c,[&]{for(int k:need)if(k>=0){region.bufferOffset=c->slot[k].raw.buffer_offset();
            vkCmdCopyImageToBuffer(c->cb,c->slot[k].img,VK_IMAGE_LAYOUT_GENERAL,c->slot[k].raw.buffer(),1,&region);
            bufbar(c->slot[k].raw,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);}})){g_err="copy-in";return -2;}
        if(need[0]>=0)c->slot[a].loaded=aid;if(need[1]>=0)c->slot[b].loaded=bid;
    }
    auto t1=clk::now();
    if(c->r->process_v4_gpu(c->slot[a].raw,c->slot[b].raw,t,c->packed,aid,bid)){g_err="rife";return -3;}
    auto t2=clk::now();
    if(!ctx_submit(c,[&]{bufbar(c->packed,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT);
        region.bufferOffset=c->packed.buffer_offset();vkCmdCopyBufferToImage(c->cb,c->packed.buffer(),c->slot[out].img,VK_IMAGE_LAYOUT_GENERAL,1,&region);})){g_err="copy-out";return -4;}
    auto t3=clk::now();
    if(ms3){ms3[0]=ms(t0,t1);ms3[1]=ms(t1,t2);ms3[2]=ms(t2,t3);}
    return 0;
}
extern "C" __declspec(dllexport) void pk_gpu_ctx_destroy(void* p){
    auto* c=static_cast<GpuCtx*>(p);if(!c)return;vkDeviceWaitIdle(c->dev);
    for(int i=0;i<c->slots;i++){c->slot[i].raw.release();if(c->slot[i].img)vkDestroyImage(c->dev,c->slot[i].img,nullptr);if(c->slot[i].mem)vkFreeMemory(c->dev,c->slot[i].mem,nullptr);}
    c->packed.release();if(c->alloc)c->vd->reclaim_blob_allocator(c->alloc);
    if(c->fence)vkDestroyFence(c->dev,c->fence,nullptr);if(c->pool)vkDestroyCommandPool(c->dev,c->pool,nullptr);delete c;
}
