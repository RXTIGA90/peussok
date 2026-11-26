// rife implemented with ncnn library

#ifndef RIFE_H
#define RIFE_H

#include <string>
#include <stdint.h>
struct RifeTaggedCache;

// ncnn
#include "net.h"

class RIFE
{
public:
    RIFE(int gpuid, bool tta_mode = false, bool tta_temporal_mode = false, bool uhd_mode = false, int num_threads = 1, bool rife_v2 = false, bool rife_v4 = false, int padding = 32);
    ~RIFE();

#if _WIN32
    int load(const std::wstring& modeldir);
#else
    int load(const std::string& modeldir);
#endif

    int process(const ncnn::Mat& in0image, const ncnn::Mat& in1image, float timestep, ncnn::Mat& outimage) const;

    int process_cpu(const ncnn::Mat& in0image, const ncnn::Mat& in1image, float timestep, ncnn::Mat& outimage) const;

    int process_v4(const ncnn::Mat& in0image, const ncnn::Mat& in1image, float timestep, ncnn::Mat& outimage) const;

    int process_v4_cpu(const ncnn::Mat& in0image, const ncnn::Mat& in1image, float timestep, ncnn::Mat& outimage) const;

    int process_v4_tagged(const ncnn::Mat&,const ncnn::Mat&,float,ncnn::Mat&,uint64_t,uint64_t) const;
    // PEUSSOK: set before load(). RIFE v4 flow at half size (inference scale 0.5);
    // the output image stays full size. Requires padding 64.
    // 11-18 internal GPU path: BGRA frames already on the GPU (D3D11 shared texture imported in Vulkan).
    int process_v4_gpu(const ncnn::VkMat& a,const ncnn::VkMat& b,float t,ncnn::VkMat& packed,uint64_t aid,uint64_t bid) const;
    ncnn::VulkanDevice* device() const { return vkdev; }
    bool flow_half = false;
    // 1.6.5: flow size relative to the image when flow_half is set (0.5 UHD, 0.75 QHD; padding 64 / 128).
    float flow_scale = 0.5f;
    // 1.7.8: last process_v4_tagged stage times in ms (in, gpu, out).
    mutable double stage_ms[3] = {-1, -1, -1};
    // 1.7.8 settings switch: fp16 arithmetic where the GPU supports it (set before load()).
    bool fp16_arith = false;
private:
    mutable RifeTaggedCache* tagged_cache = nullptr;
    ncnn::VulkanDevice* vkdev;
    ncnn::Net flownet;
    ncnn::Net tailnet; // flow_half: full-size warp and blend
    ncnn::Net contextnet;
    ncnn::Net fusionnet;
    ncnn::Pipeline* rife_preproc;
    ncnn::Pipeline* rife_postproc;
    ncnn::Pipeline* rife_preproc4 = nullptr;  // BGRA input (tagged path)
    ncnn::Pipeline* rife_postproc4 = nullptr; // BGRA output (tagged path)
    ncnn::Pipeline* rife_down = nullptr; // 2.4.0 GPU path: bytes -> flow network input (rife_fused.h)
    ncnn::Pipeline* rife_tail = nullptr; // 2.4.0 GPU path: warp and blend to bytes (rife_fused.h)
    ncnn::Pipeline* rife_flow_tta_avg;
    ncnn::Pipeline* rife_flow_tta_temporal_avg;
    ncnn::Pipeline* rife_out_tta_temporal_avg;
    ncnn::Pipeline* rife_v4_timestep;
    ncnn::Layer* rife_uhd_downscale_image;
    ncnn::Layer* rife_uhd_upscale_flow;
    ncnn::Layer* rife_uhd_double_flow;
    ncnn::Layer* rife_v2_slice_flow;
    bool tta_mode;
    bool tta_temporal_mode;
    bool uhd_mode;
    int num_threads;
    bool rife_v2;
    bool rife_v4;
    int padding;
};

#endif // RIFE_H
