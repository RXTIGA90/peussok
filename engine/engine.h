// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#ifdef PK_ENGINE_BUILD
#define PK_API __declspec(dllexport)
#else
#define PK_API __declspec(dllimport)
#endif
// Packed BGR8; dimensions are unchanged. Handles must not be shared concurrently.
PK_API void* pk_create(const wchar_t* model_directory, int gpu_id);
PK_API int pk_interpolate(void* handle, const uint8_t* a, const uint8_t* b,
                         int width, int height, float t, uint8_t* output);
// Immutable source IDs must be unique for the lifetime of this engine.
PK_API int pk_interpolate_tagged(void*,const uint8_t*,const uint8_t*,int,int,float,uint8_t*,uint64_t,uint64_t);
// Same as pk_interpolate_tagged for packed BGRA (4 bytes per pixel, alpha output 255).
PK_API int pk_interpolate_bgra(void*,const uint8_t*,const uint8_t*,int,int,float,uint8_t*,uint64_t,uint64_t);
// same as pk_interpolate_bgra on a second RIFE network with a fixed flow scale (loaded on first use; RIFE only, else -4).
PK_API int pk_interpolate_bgra_aux(void*,const uint8_t*,const uint8_t*,int,int,float,uint8_t*,uint64_t,uint64_t,float);
PK_API const char* pk_last_error(void);
PK_API const char* pk_gpu_name(void* handle);
PK_API const char* pk_model_name(void* handle);
PK_API double pk_last_ms(void* handle);
// Diagnostic only: GPU fence wait inside the last interpolation, -1 if not measured.
PK_API double pk_last_gpu_wait_ms(void* handle);
// RIFE flow size policy. 1 (default): flow at about FHD (FHD full, QHD 0.75, UHD 0.5);
// 0 (low latency): half-size flow from FHD up.
PK_API void pk_set_flow_floor(void* handle,int fhd_floor);
// diagnostics. Stage times of the last interpolation in ms: [0] CPU input copy/convert and command
// recording, [1] submit until the GPU fence, [2] CPU output copy/convert. Returns 1 (values -1) if not measured.
PK_API int pk_last_stages(void* handle,double* out3);
// One line: vendor/device ids, Vulkan API, driver name and version, compute queue family, fp16 support.
PK_API const char* pk_gpu_info(void* handle);
// settings switches for engines created afterwards: bit 0 keeps the GPU clock up while generating
// (small low-priority compute load; aimed at AMD/Intel), bit 1 RIFE fp16 arithmetic where supported.
PK_API void pk_set_gpu_options(int flags);
PK_API void pk_destroy(void* handle);
#ifdef __cplusplus
}
#endif
