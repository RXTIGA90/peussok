// SPDX-License-Identifier: MIT
// PEUSSOK extension: reuse immutable source images identified by the owning live scheduler.
// This RIFE instance belongs to one inference worker. No cross-thread cache.
#include <cstring>
#include <chrono>
namespace ncnn{extern thread_local std::chrono::steady_clock::time_point pk_fence_done;}
struct RifeTaggedCache {
    ncnn::VulkanDevice* device;
    ncnn::VkAllocator *blob, *staging;
    ncnn::VkMat padded[2],phase;
    int phase_w=0,phase_h=0;float phase_t=-1;
    uint64_t ids[2]={0,0};
    int width[2]={0,0}, height[2]={0,0};
    explicit RifeTaggedCache(ncnn::VulkanDevice* d):device(d),blob(d->acquire_blob_allocator()),staging(d->acquire_staging_allocator()){}
    ~RifeTaggedCache(){
        padded[0].release();padded[1].release();phase.release();
        device->reclaim_blob_allocator(blob);device->reclaim_staging_allocator(staging);
    }
    int find(const ncnn::Mat& input,uint64_t id)const{

        for(int i=0;i<2;i++)if(width[i]==input.w&&height[i]==input.h&&ids[i]==id)return i;
        return -1;
    }
};
int RIFE::process_v4_tagged(const ncnn::Mat& a,const ncnn::Mat& b,float t,ncnn::Mat& output,uint64_t aid,uint64_t bid)const{
    const auto stage0=std::chrono::steady_clock::now();
    if(!tagged_cache)tagged_cache=new RifeTaggedCache(vkdev);
    RifeTaggedCache& cache=*tagged_cache;
    struct SubmissionGuard {
        RifeTaggedCache& cache;bool completed=false;
        ~SubmissionGuard(){if(!completed){cache.width[0]=cache.width[1]=0;cache.phase_w=0;}}
    } submission{cache};
    ncnn::Option opt=flownet.opt;
    opt.blob_vkallocator=cache.blob;opt.workspace_vkallocator=cache.blob;opt.staging_vkallocator=cache.staging;
    const int w=a.w,h=a.h,pw=(w+padding-1)/padding*padding,ph=(h+padding-1)/padding*padding;
    const size_t element=opt.use_fp16_storage?2u:4u;
    // 3 = bgr24, 4 = BGRA (GPU byte path only; the CPU pixel fallback handles bgr24).
    const int px=(int)a.elemsize;const bool bytes=opt.use_fp16_storage&&opt.use_int8_storage;
    if((px!=3&&px!=4)||(int)b.elemsize!=px||(int)output.elemsize!=px||(px==4&&(!bytes||!rife_preproc4||!rife_postproc4)))return -1;
    ncnn::VkCompute cmd(vkdev);
    int ai=cache.find(a,aid),bi=cache.find(b,bid);
    auto prepare=[&](const ncnn::Mat& input,int slot,uint64_t id){
        // Clone before updating CPU identity; failed submissions invalidate cache.
        ncnn::Mat raw;
        if(bytes)raw=ncnn::Mat(w,h,(unsigned char*)input.data,(size_t)px,1);
        else raw=ncnn::Mat::from_pixels((const unsigned char*)input.data,ncnn::Mat::PIXEL_BGR2RGB,w,h);
        ncnn::VkMat uploaded;cmd.record_clone(raw,uploaded,opt);if(uploaded.empty())return false;
        cache.padded[slot].create(pw,ph,3,element,1,cache.blob);if(cache.padded[slot].empty())return false;
        std::vector<ncnn::VkMat> bindings={uploaded,cache.padded[slot]};
        std::vector<ncnn::vk_constant_type> constants(6);
        constants[0].i=uploaded.w;constants[1].i=uploaded.h;constants[2].i=uploaded.cstep;
        constants[3].i=pw;constants[4].i=ph;constants[5].i=cache.padded[slot].cstep;
        cmd.record_pipeline(px==4?rife_preproc4:rife_preproc,bindings,constants,cache.padded[slot]);
        cache.ids[slot]=id;
        cache.width[slot]=w;cache.height[slot]=h;return true;
    };
    if(ai<0){ai=bi==0?1:0;if(!prepare(a,ai,aid))return -1;}
    if(bi<0){
        if(aid==bid)bi=ai;
        else {bi=1-ai;if(!prepare(b,bi,bid))return -1;}
    }
    // flow_half: the flow network sees half-size images, so its timestep map is half size.
    const int qw=flow_half?(int)(pw*flow_scale+0.5f):pw,qh=flow_half?(int)(ph*flow_scale+0.5f):ph;
    ncnn::VkMat& phase=cache.phase;
    if(cache.phase_w!=qw||cache.phase_h!=qh||cache.phase_t!=t){
        phase.create(qw,qh,1,element,1,cache.blob);if(phase.empty())return -1;
        cache.phase_w=qw;cache.phase_h=qh;cache.phase_t=t;
        std::vector<ncnn::VkMat> bindings={phase};std::vector<ncnn::vk_constant_type> constants(4);
        constants[0].i=qw;constants[1].i=qh;constants[2].i=phase.cstep;constants[3].f=t;
        cmd.record_pipeline(rife_v4_timestep,bindings,constants,phase);
    }
    ncnn::VkMat result;
    {
        ncnn::Extractor ex=flownet.create_extractor();
        ex.set_blob_vkallocator(cache.blob);ex.set_workspace_vkallocator(cache.blob);ex.set_staging_vkallocator(cache.staging);
        // flow_half: the full-size images enter before the head network's own half-size resize.
        ex.input(flow_half?"pk_full_in0":"in0",cache.padded[ai]);ex.input(flow_half?"pk_full_in1":"in1",cache.padded[bi]);ex.input("in2",phase);
        if(!flow_half){if(ex.extract("out0",result,cmd)){cache.width[0]=cache.width[1]=0;return -1;}}
        else {
            ncnn::VkMat flowmask;
            if(ex.extract("pk_flowmask",flowmask,cmd)){cache.width[0]=cache.width[1]=0;return -1;}
            ncnn::Extractor tail=tailnet.create_extractor();
            tail.set_blob_vkallocator(cache.blob);tail.set_workspace_vkallocator(cache.blob);tail.set_staging_vkallocator(cache.staging);
            tail.input("img0",cache.padded[ai]);tail.input("img1",cache.padded[bi]);tail.input("fm",flowmask);
            if(tail.extract("out0",result,cmd)){cache.width[0]=cache.width[1]=0;return -1;}
        }
    }
    if(result.empty())return -1;
    ncnn::VkMat packed;
    if(bytes)packed.create(w,h,(size_t)px,1,cache.blob);
    else packed.create(w,h,3,(size_t)4u,1,cache.blob);
    if(packed.empty())return -1;
    {
        std::vector<ncnn::VkMat> bindings={result,packed};std::vector<ncnn::vk_constant_type> constants(6);
        constants[0].i=result.w;constants[1].i=result.h;constants[2].i=result.cstep;
        constants[3].i=w;constants[4].i=h;constants[5].i=packed.cstep;
        cmd.record_pipeline(px==4?rife_postproc4:rife_postproc,bindings,constants,packed);
    }
    ncnn::Mat out;
    if(bytes)out=ncnn::Mat(w,h,(unsigned char*)output.data,(size_t)px,1);
    cmd.record_clone(packed,out,opt);
    const auto stage1=std::chrono::steady_clock::now();
    int code=cmd.submit_and_wait();
    if(code){cache.width[0]=cache.width[1]=0;return code;}
    if(!(opt.use_fp16_storage&&opt.use_int8_storage))out.to_pixels((unsigned char*)output.data,ncnn::Mat::PIXEL_RGB2BGR);
    // 1.7.8 stage timing: in = CPU upload copies + command recording, gpu = submit until the fence,
    // out = staging download copy (ncnn copies it after the fence).
    {using ms=std::chrono::duration<double,std::milli>;const auto stage2=std::chrono::steady_clock::now();
     auto fence=ncnn::pk_fence_done<stage1?stage2:ncnn::pk_fence_done;
     stage_ms[0]=ms(stage1-stage0).count();stage_ms[1]=ms(fence-stage1).count();stage_ms[2]=ms(stage2-fence).count();}
    submission.completed=true;
    return 0;
}

// 11-18 internal (from the 11-16 GPU-path prototype): BGRA frames already on the GPU, same shaders and network.
int RIFE::process_v4_gpu(const ncnn::VkMat& a,const ncnn::VkMat& b,float t,ncnn::VkMat& packed,uint64_t aid,uint64_t bid)const{
    const auto stage0=std::chrono::steady_clock::now();
    if(!tagged_cache)tagged_cache=new RifeTaggedCache(vkdev);
    RifeTaggedCache& cache=*tagged_cache;
    ncnn::Option opt=flownet.opt;
    opt.blob_vkallocator=cache.blob;opt.workspace_vkallocator=cache.blob;opt.staging_vkallocator=cache.staging;
    const int w=a.w,h=a.h,pw=(w+padding-1)/padding*padding,ph=(h+padding-1)/padding*padding;
    const size_t element=opt.use_fp16_storage?2u:4u;
    if(a.elemsize!=4||b.elemsize!=4||b.w!=w||b.h!=h||!rife_preproc4||!rife_postproc4||!(opt.use_fp16_storage&&opt.use_int8_storage))return -1;
    ncnn::VkCompute cmd(vkdev);
    // Cache identity is by (size, id) as in the CPU path; a CPU Mat header is enough for find().
    ncnn::Mat ha(w,h,(void*)0,(size_t)4,1),hb(w,h,(void*)0,(size_t)4,1);
    int ai=cache.find(ha,aid),bi=cache.find(hb,bid);
    // 2.4.0: merged full-size passes (rife_fused.h). padded[] then holds the flow network's small input instead.
    const bool fused=flow_half&&rife_down&&rife_tail;
    struct SubmissionGuard { RifeTaggedCache& cache;bool completed=false; ~SubmissionGuard(){if(!completed){cache.width[0]=cache.width[1]=0;cache.phase_w=0;}} } submission{cache};
    const int fw=(int)(pw*flow_scale+0.5f),fh=(int)(ph*flow_scale+0.5f);
    auto prepare=[&](const ncnn::VkMat& input,int slot,uint64_t id){
        if(fused){
            ncnn::VkMat& low=cache.padded[slot];low.create(fw,fh,3,element,1,cache.blob);if(low.empty())return false;
            std::vector<ncnn::VkMat> bindings={input,low};std::vector<ncnn::vk_constant_type> c(7);
            c[0].i=w;c[1].i=h;c[2].i=pw;c[3].i=ph;c[4].i=fw;c[5].i=fh;c[6].i=(int)low.cstep;
            ncnn::VkMat size;size.w=fw;size.h=fh;size.c=1;
            cmd.record_pipeline(rife_down,bindings,c,size);
            cache.ids[slot]=id;cache.width[slot]=w;cache.height[slot]=h;return true;
        }
        cache.padded[slot].create(pw,ph,3,element,1,cache.blob);if(cache.padded[slot].empty())return false;
        std::vector<ncnn::VkMat> bindings={input,cache.padded[slot]};
        std::vector<ncnn::vk_constant_type> constants(6);
        constants[0].i=input.w;constants[1].i=input.h;constants[2].i=input.cstep;
        constants[3].i=pw;constants[4].i=ph;constants[5].i=cache.padded[slot].cstep;
        cmd.record_pipeline(rife_preproc4,bindings,constants,cache.padded[slot]);
        cache.ids[slot]=id;cache.width[slot]=w;cache.height[slot]=h;return true;
    };
    if(ai<0){ai=bi==0?1:0;if(!prepare(a,ai,aid))return -1;}
    if(bi<0){if(aid==bid)bi=ai;else{bi=1-ai;if(!prepare(b,bi,bid))return -1;}}
    const int qw=flow_half?(int)(pw*flow_scale+0.5f):pw,qh=flow_half?(int)(ph*flow_scale+0.5f):ph;
    ncnn::VkMat& phase=cache.phase;
    if(cache.phase_w!=qw||cache.phase_h!=qh||cache.phase_t!=t){
        phase.create(qw,qh,1,element,1,cache.blob);if(phase.empty())return -1;
        cache.phase_w=qw;cache.phase_h=qh;cache.phase_t=t;
        std::vector<ncnn::VkMat> bindings={phase};std::vector<ncnn::vk_constant_type> constants(4);
        constants[0].i=qw;constants[1].i=qh;constants[2].i=phase.cstep;constants[3].f=t;
        cmd.record_pipeline(rife_v4_timestep,bindings,constants,phase);
    }
    ncnn::VkMat result;
    {
        ncnn::Extractor ex=flownet.create_extractor();
        ex.set_blob_vkallocator(cache.blob);ex.set_workspace_vkallocator(cache.blob);ex.set_staging_vkallocator(cache.staging);
        ex.input(flow_half&&!fused?"pk_full_in0":"in0",cache.padded[ai]);ex.input(flow_half&&!fused?"pk_full_in1":"in1",cache.padded[bi]);ex.input("in2",phase);
        if(fused){
            if(packed.w!=w||packed.h!=h||packed.elemsize!=4)packed.create(w,h,(size_t)4,1,cache.blob);
            ncnn::VkMat fm;
            if(packed.empty()||ex.extract("pk_flowmask",fm,cmd)){cache.width[0]=cache.width[1]=0;return -1;}
            if(fm.elempack!=1){ncnn::VkMat one;vkdev->convert_packing(fm,one,1,cmd,opt);fm=one;}
            std::vector<ncnn::VkMat> bindings={a,b,fm,packed};std::vector<ncnn::vk_constant_type> c(8);
            c[0].i=w;c[1].i=h;c[2].i=pw;c[3].i=ph;c[4].i=fm.w;c[5].i=fm.h;c[6].i=(int)fm.cstep;c[7].f=1.f/flow_scale;
            ncnn::VkMat size;size.w=w;size.h=h;size.c=1;
            cmd.record_pipeline(rife_tail,bindings,c,size);
        }
        else if(!flow_half){if(ex.extract("out0",result,cmd)){cache.width[0]=cache.width[1]=0;return -1;}}
        else{
            ncnn::VkMat flowmask;
            if(ex.extract("pk_flowmask",flowmask,cmd)){cache.width[0]=cache.width[1]=0;return -1;}
            ncnn::Extractor tail=tailnet.create_extractor();
            tail.set_blob_vkallocator(cache.blob);tail.set_workspace_vkallocator(cache.blob);tail.set_staging_vkallocator(cache.staging);
            tail.input("img0",cache.padded[ai]);tail.input("img1",cache.padded[bi]);tail.input("fm",flowmask);
            if(tail.extract("out0",result,cmd)){cache.width[0]=cache.width[1]=0;return -1;}
        }
    }
    if(!fused){
        if(result.empty())return -1;
        if(packed.w!=w||packed.h!=h||packed.elemsize!=4)packed.create(w,h,(size_t)4,1,cache.blob);
        if(packed.empty())return -1;
        std::vector<ncnn::VkMat> bindings={result,packed};std::vector<ncnn::vk_constant_type> constants(6);
        constants[0].i=result.w;constants[1].i=result.h;constants[2].i=result.cstep;
        constants[3].i=w;constants[4].i=h;constants[5].i=packed.cstep;
        cmd.record_pipeline(rife_postproc4,bindings,constants,packed);
    }
    const auto stage1=std::chrono::steady_clock::now();
    int code=cmd.submit_and_wait();
    if(code){cache.width[0]=cache.width[1]=0;return code;}
    using ms=std::chrono::duration<double,std::milli>;const auto stage2=std::chrono::steady_clock::now();
    stage_ms[0]=ms(stage1-stage0).count();stage_ms[1]=ms(stage2-stage1).count();stage_ms[2]=0;
    submission.completed=true;
    return 0;
}
