// SPDX-License-Identifier: MIT
// PEUSSOK extension (2.4.0): the full-size work of the GPU path in two compute shaders.
//
// Before, every full-size step was its own pass over planar fp16 images: BGRA -> planar RGB, resize for the flow
// network, resize of flow and mask, crop, multiply, sigmoid, two warps, two multiplies, add, planar RGB -> BGRA. At
// 3840x2160 those passes cost more than the flow network itself. The arithmetic is unchanged; the passes are merged:
//   down: BGRA bytes -> planar RGB at the flow network's size (same bilinear rule as ncnn Interp, align_corners = 0)
//   tail: BGRA bytes of both frames + flow/mask -> BGRA bytes (resize flow and mask, warp twice, blend, round)
// One difference on purpose: the tail clamps its samples to the real image. The old path clamped to the padded image,
// so motion out of the bottom/right edge pulled in the black padding.
static const char rife_fused_down_comp[] = R"(
#version 450
#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#endif
layout (binding = 0) readonly buffer image_blob { uint image_data[]; };
layout (binding = 1) writeonly buffer top_blob { sfp top_data[]; };
layout (push_constant) uniform parameter { int w; int h; int pw; int ph; int outw; int outh; int cstep; } p;
vec3 texel(int x, int y)
{
    if (x >= p.w || y >= p.h) return vec3(0.0); // padding is black, as in the planar path
    return unpackUnorm4x8(image_data[y * p.w + x]).zyx;
}
void main()
{
    int gx = int(gl_GlobalInvocationID.x), gy = int(gl_GlobalInvocationID.y);
    if (gx >= p.outw || gy >= p.outh) return;
    float sx = clamp((float(gx) + 0.5) * float(p.pw) / float(p.outw) - 0.5, 0.0, float(p.pw - 1));
    float sy = clamp((float(gy) + 0.5) * float(p.ph) / float(p.outh) - 0.5, 0.0, float(p.ph - 1));
    int x0 = int(sx), y0 = int(sy), x1 = min(x0 + 1, p.pw - 1), y1 = min(y0 + 1, p.ph - 1);
    float a = sx - float(x0), b = sy - float(y0);
    vec3 v = mix(mix(texel(x0, y0), texel(x1, y0), a), mix(texel(x0, y1), texel(x1, y1), a), b);
    int o = gy * p.outw + gx;
    top_data[o] = sfp(v.x);
    top_data[p.cstep + o] = sfp(v.y);
    top_data[2 * p.cstep + o] = sfp(v.z);
}
)";
static const char rife_fused_tail_comp[] = R"(
#version 450
#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#endif
layout (binding = 0) readonly buffer image0_blob { uint image0[]; };
layout (binding = 1) readonly buffer image1_blob { uint image1[]; };
layout (binding = 2) readonly buffer flow_blob { sfp flow_data[]; }; // 4 flow channels + mask before the sigmoid
layout (binding = 3) writeonly buffer top_blob { uint top_data[]; };
layout (push_constant) uniform parameter { int w; int h; int pw; int ph; int fw; int fh; int cstep; float up; } p;
void main()
{
    int gx = int(gl_GlobalInvocationID.x), gy = int(gl_GlobalInvocationID.y);
    if (gx >= p.w || gy >= p.h) return;
    float lx = clamp((float(gx) + 0.5) * float(p.fw) / float(p.pw) - 0.5, 0.0, float(p.fw - 1));
    float ly = clamp((float(gy) + 0.5) * float(p.fh) / float(p.ph) - 0.5, 0.0, float(p.fh - 1));
    int x0 = int(lx), y0 = int(ly), x1 = min(x0 + 1, p.fw - 1), y1 = min(y0 + 1, p.fh - 1);
    float a = lx - float(x0), b = ly - float(y0);
    float f[5];
    for (int c = 0; c < 5; c++)
    {
        int o = c * p.cstep;
        f[c] = mix(mix(float(flow_data[o + y0 * p.fw + x0]), float(flow_data[o + y0 * p.fw + x1]), a),
                   mix(float(flow_data[o + y1 * p.fw + x0]), float(flow_data[o + y1 * p.fw + x1]), a), b);
    }
    float m = 1.0 / (1.0 + exp(-f[4]));
    vec2 last = vec2(p.w - 1, p.h - 1);
    ivec2 edge = ivec2(p.w - 1, p.h - 1);
    vec2 s0 = clamp(vec2(gx, gy) + vec2(f[0], f[1]) * p.up, vec2(0.0), last);
    vec2 s1 = clamp(vec2(gx, gy) + vec2(f[2], f[3]) * p.up, vec2(0.0), last);
    ivec2 a0 = ivec2(s0), a1 = ivec2(s1), b0 = min(a0 + 1, edge), b1 = min(a1 + 1, edge);
    vec2 t0 = s0 - vec2(a0), t1 = s1 - vec2(a1);
    vec4 u = mix(mix(unpackUnorm4x8(image0[a0.y * p.w + a0.x]), unpackUnorm4x8(image0[a0.y * p.w + b0.x]), t0.x),
                 mix(unpackUnorm4x8(image0[b0.y * p.w + a0.x]), unpackUnorm4x8(image0[b0.y * p.w + b0.x]), t0.x), t0.y);
    vec4 v = mix(mix(unpackUnorm4x8(image1[a1.y * p.w + a1.x]), unpackUnorm4x8(image1[a1.y * p.w + b1.x]), t1.x),
                 mix(unpackUnorm4x8(image1[b1.y * p.w + a1.x]), unpackUnorm4x8(image1[b1.y * p.w + b1.x]), t1.x), t1.y);
    vec4 r = u * m + v * (1.0 - m);
    r.w = 1.0;
    top_data[gy * p.w + gx] = packUnorm4x8(r);
}
)";
// PK_RIFE_FUSED=0 (tests only) keeps the planar path for comparison.
static bool rife_fused_wanted()
{
    wchar_t v[4];
    return !(GetEnvironmentVariableW(L"PK_RIFE_FUSED",v,4)==1&&v[0]==L'0');
}
