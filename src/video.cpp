// SPDX-License-Identifier: GPL-3.0-only
#include "video.h"
#include "guard.h"
#include "extra.h"
#include "vrr.h"
#include "bench.h"
#include "nr.h"
#include "hdr.h"
#include "rec.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <avrt.h>

Tuning tune;
Numbers num;

// tex: 0-2 originals, 3-4 engine outputs, 5-6 outputs with HUD protection, 7-8 originals while the engine is too slow, 9-14 pictures
// of 3x/4x (two sets of three)
enum { INPUTS = 3, OUT0 = 3, HUD0 = 5, SPARE0 = 7, MULTI0 = 9, NRO0 = 15, NRM0 = 20, TEXTURES = 26 }; // NRO0, NRM0: originals and made pictures with DLSS 5

// ---- shaders ----
static const char vs_src[] = "struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};V main(uint i:SV_VertexID){V o;o.uv=float2((i<<1)&2,i&2);o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}";
// Y and UV planes (or packed YUY2 / BGR) to BGRA. BT.709 for pictures of 720 lines and more, BT.601 below.
static const char ps_convert_src[] =
    "Texture2D<float> Y:register(t0);Texture2D<float2> UV:register(t1);Texture2D<float4> P:register(t2);SamplerState s:register(s0);"
    "cbuffer C:register(b0){uint W;uint H;uint kind;uint full;uint sd;uint flip;uint pq;uint pad;};"
    "float3 rgb(float y,float2 c){if(!full){y=(y*255.0-16.0)/219.0;c=(c*255.0-128.0)/224.0;}else{c=c-128.0/255.0;}"
    " float kr=pq?0.2627:sd?0.299:0.2126,kb=pq?0.0593:sd?0.114:0.0722,kg=1.0-kr-kb;"
    " return saturate(float3(y+2*(1-kr)*c.y,y-2*kb*(1-kb)/kg*c.x-2*kr*(1-kr)/kg*c.y,y+2*(1-kb)*c.x));}"
    "float4 main(float4 p:SV_Position):SV_Target{int2 q=int2(p.xy);if(flip)q.y=(int)H-1-q.y;"
    " if(kind==2){float4 v=P.Load(int3(q.x/2,q.y,0));float2 c=v.yw;"                      /* YUY2: Y0 U Y1 V, chroma sited on the left pixel */
    "  if((q.x&1)){float4 n=P.Load(int3(min(q.x/2+1,(int)W/2-1),q.y,0));c=(c+n.yw)*0.5;}"
    "  return float4(rgb((q.x&1)?v.z:v.x,c),1);}"
    " if(kind==4){float4 v=P.Load(int3(q.x/2,q.y,0));float2 c=v.xz;"                      /* UYVY / HDYC: U Y0 V Y1 */
    "  if((q.x&1)){float4 n=P.Load(int3(min(q.x/2+1,(int)W/2-1),q.y,0));c=(c+n.xz)*0.5;}"
    "  return float4(rgb((q.x&1)?v.w:v.y,c),1);}"
    " if(kind==5){return float4(P.Load(int3(q,0)).rgb,1);}"                                   /* RGB32 (BGRA bytes, read as BGRA) */
    " if(kind==3){uint b=(uint)q.x*3;float3 v=float3(Y.Load(int3(b+2,q.y,0)),Y.Load(int3(b+1,q.y,0)),Y.Load(int3(b,q.y,0)));return float4(v,1);}" /* BGR24 bytes */
    " float y=Y.Load(int3(q,0));float2 c=UV.SampleLevel(s,float2((q.x+1.0)/W,(q.y+0.5)/H),0);"
    " if(kind==1){y=y*65535.0/65280.0;c=c*65535.0/65280.0;}"                               /* P010: 10 bits in the upper part */
    " float3 o=rgb(y,c);if(pq){uint h=(uint)q.x*1973u+(uint)q.y*9277u;h=(h<<13)^h;h=h*(h*h*15731u+789221u)+1376312589u;"
    "  o=saturate(o+((float)(h&1023u)/1023.0-0.5)/255.0);}"                               /* HDR10: dither to 8 bits */
    " return float4(o,1);}";
// What goes to the screen: as it is, or (HDR) as HDR10. Ordinary brightness stays at the chosen white; the brightest parts go on towards
// the monitor's peak. Colours keep their tone.
static const char out_src[] =
    "cbuffer Out:register(b1){float hdr;float white;float peak;float pqin;};"
    "float3 pq_nits(float3 e){float3 p=pow(saturate(e),1.0/78.84375);return 10000.0*pow(max(p-0.8359375,0)/(18.8515625-18.6875*p),1.0/0.1593017578125);}"
    "float3 sdr_of_pq(float3 e){float3 l=mul(float3x3(1.6605,-0.5876,-0.0728,-0.1246,1.1329,-0.0083,-0.0182,-0.1006,1.1187),pq_nits(e))/203.0;"
    " l=max(l,0);float m=max(l.r,max(l.g,l.b));float t=m<=0.8?m:0.8+0.2*(1-exp(-(m-0.8)/0.2));"
    " l*=m>0?t/m:0;return pow(saturate(l),1/2.2);}"
    "float3 sent(float3 c){if(hdr<0.5)return c;"
    " float3 l=pow(saturate(c),2.2);float y=max(l.r,max(l.g,l.b));float up=max(0,y-0.5)*2;float gain=1+(peak/white-1)*up*up;l*=gain*white;"
    " l=mul(float3x3(0.6274,0.3293,0.0433,0.0691,0.9195,0.0114,0.0164,0.0880,0.8956),l)/10000.0;"
    " float3 p=pow(saturate(l),0.1593017578125);return pow((0.8359375+18.8515625*p)/(1+18.6875*p),78.84375);}"
    "float3 picture(float3 c){float3 o=sent(c);if(pqin>0.5)o=hdr>0.5?c:sdr_of_pq(c);return o;}";
static const char ps_show_src[] = "Texture2D t:register(t0);SamplerState s:register(s0);float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return float4(picture(t.Sample(s,uv).rgb),1);}";
// Bicubic scale (5 taps) and contrast adaptive sharpen; k0.x = strength.
static const char ps_sharp_src[] =
    "Texture2D t:register(t0);SamplerState s:register(s0);cbuffer C:register(b0){float4 k0;};"
    "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{uint W,H;t.GetDimensions(W,H);float2 sz=float2(W,H),d=1.0/sz;"
    "float2 q=uv*sz-0.5,f=frac(q),i=floor(q);"
    "float2 w0=f*(-0.5+f*(1.0-0.5*f)),w1=1.0+f*f*(-2.5+1.5*f),w2=f*(0.5+f*(2.0-1.5*f)),w3=f*f*(-0.5+0.5*f),w12=w1+w2;"
    "float2 a=(i-0.5)*d,c=(i+2.5)*d,b=(i+0.5+w2/w12)*d;float wc=w12.x*w12.y,wl=w0.x*w12.y,wr=w3.x*w12.y,wt=w12.x*w0.y,wb=w12.x*w3.y;"
    "float3 cc=t.SampleLevel(s,b,0).rgb,l=t.SampleLevel(s,float2(a.x,b.y),0).rgb,r=t.SampleLevel(s,float2(c.x,b.y),0).rgb,u=t.SampleLevel(s,float2(b.x,a.y),0).rgb,dn=t.SampleLevel(s,float2(b.x,c.y),0).rgb;"
    "float3 col=(cc*wc+l*wl+r*wr+u*wt+dn*wb)/(wc+wl+wr+wt+wb);float3 mn=min(col,min(min(l,r),min(u,dn))),mx=max(col,max(max(l,r),max(u,dn)));"
    "float3 amp=sqrt(saturate(min(mn,1.0-mx)/max(mx,1e-4))),k=-k0.x*amp;return float4(picture(saturate((col+k*(l+r+u+dn))/(1.0+4.0*k))),1);}";
static const char ps_label_src[] = "Texture2D t:register(t0);SamplerState s:register(s0);float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{float2 v=t.Sample(s,uv).rg;return float4(sent(v.rrr),max(v.r,v.g*0.8));}";
// HUD protection: per pixel, how many originals in a row kept their colour; pixels that stayed long enough (and their
// neighbours) come from the original. Same rule as the plugin.
static const char hud_src[] =
    "Texture2D<float4> Src:register(t0);RWTexture2D<uint> Ref:register(u0);RWTexture2D<uint> Cnt:register(u1);cbuffer P:register(b0){uint W;uint H;uint TOL;uint RUN;};"
    "[numthreads(16,16,1)]void cs_count(uint3 id:SV_DispatchThreadID){if(id.x>=W||id.y>=H)return;uint3 v=(uint3)(Src[id.xy].rgb*255.0+0.5);uint r=Ref[id.xy];int3 d=abs((int3)v-int3(r&255,(r>>8)&255,(r>>16)&255));"
    " if(max(d.x,max(d.y,d.z))>(int)TOL){Ref[id.xy]=v.x|(v.y<<8)|(v.z<<16);Cnt[id.xy]=0;}else Cnt[id.xy]=min(Cnt[id.xy]+1,255);}\n"
    "Texture2D<uint> CntR:register(t1);RWTexture2D<uint> Map:register(u2);"
    "[numthreads(16,16,1)]void cs_map(uint3 id:SV_DispatchThreadID){if(id.x>=W||id.y>=H)return;uint m=0;"
    " [unroll]for(int dy=-2;dy<=2;dy++)[unroll]for(int dx=-2;dx<=2;dx++){int2 p=clamp(int2(id.xy)+int2(dx,dy),int2(0,0),int2(W-1,H-1));m|=CntR[p]>=RUN?1u:0u;}Map[id.xy]=m;}\n"
    "Texture2D<float4> Gen:register(t2);Texture2D<float4> Org:register(t3);Texture2D<uint> MapR:register(t4);"
    "float4 vs_full(uint i:SV_VertexID):SV_Position{float2 t=float2((i<<1)&2,i&2);return float4(t*float2(2,-2)+float2(-1,1),0,1);}"
    // Text protection. Strokes of a text stay exactly where they are while the picture behind them moves. From two originals (and
    // the pair before): pixels on a strong edge that did not change (strict), enough of them close together, with movement around
    // them, make a text block, unless many strong edges around move (icons of a map, not a text); a block stays awake for 15 originals. Inside a block a made picture may not leave the colours of
    // the two originals (smeared strokes disappear), and the strokes themselves come from the originals.
    "Texture2D<float> LA:register(t0);Texture2D<float> LB:register(t1);Texture2D<float4> TA:register(t2);Texture2D<float4> TB:register(t3);Texture2D<uint> BitsOld:register(t4);Texture2D<uint> Bits:register(t5);Texture2D<uint4> CellR:register(t6);Texture2D<uint> ActR:register(t7);"
    "SamplerState lin:register(s0);RWTexture2D<float> LOut:register(u3);RWTexture2D<uint> BitsOut:register(u4);RWTexture2D<uint4> CellOut:register(u5);RWTexture2D<uint> Mem:register(u6);RWTexture2D<uint> ActOut:register(u7);"
    "cbuffer T:register(b1){uint DW;uint DH;uint CW;uint CH;uint STEP;uint3 tpad;};"
    "float3 at(Texture2D<float4> t,int2 p){return t.SampleLevel(lin,(float2(p)+0.5)/float2(DW,DH),0).rgb;}"
    "[numthreads(8,8,1)]void cs_luma(uint3 id:SV_DispatchThreadID){if(id.x>=DW||id.y>=DH)return;float3 c=at(TB,id.xy);LOut[id.xy]=max(c.r,max(c.g,c.b));}"
    "[numthreads(8,8,1)]void cs_bits(uint3 id:SV_DispatchThreadID){if(id.x>=DW||id.y>=DH)return;float3 v=abs(at(TA,id.xy)-at(TB,id.xy))*255.0;float d=max(v.r,max(v.g,v.b));uint o=d>3.0?4u:0u;"
    " {float2 a3=float2(1,0),b3=a3,a5=a3,b5=a3;[unroll]for(int y=-2;y<=2;y++)[unroll]for(int x=-2;x<=2;x++){int2 p=clamp(int2(id.xy)+int2(x,y),int2(0,0),int2(DW-1,DH-1));float a=LA[p],b=LB[p];"
    "  a5=float2(min(a5.x,a),max(a5.y,a));b5=float2(min(b5.x,b),max(b5.y,b));if(abs(x)<2&&abs(y)<2){a3=float2(min(a3.x,a),max(a3.y,a));b3=float2(min(b3.x,b),max(b3.y,b));}}"
    "  float r3=min(a3.y-a3.x,b3.y-b3.x)*255.0,r5=min(a5.y-a5.x,b5.y-b5.x)*255.0;bool strict=d<=6.0&&r3>=70.0;"
    "  if(strict){o|=8u;if(BitsOld[id.xy]&8u)o|=1u;}if(r5>=50.0&&d<=min(36.0,max(6.0,0.3*r5)))o|=2u;"
    "  if(d>max(6.0,0.3*r5)&&max(a3.y-a3.x,b3.y-b3.x)*255.0>=70.0)o|=16u;}"
    " BitsOut[id.xy]=o;}"
    "[numthreads(8,8,1)]void cs_cell(uint3 id:SV_DispatchThreadID){if(id.x>=CW||id.y>=CH)return;uint4 n=0;[unroll]for(uint y=0;y<4;y++)[unroll]for(uint x=0;x<4;x++){uint b=Bits[id.xy*4+uint2(x,y)];n.x+=b&1u;n.y+=((b&4u)&&!(b&1u))?1u:0u;n.z+=(b>>1)&1u;n.w+=(b>>4)&1u;}CellOut[id.xy]=n;}"
    "[numthreads(8,8,1)]void cs_act(uint3 id:SV_DispatchThreadID){if(id.x>=CW||id.y>=CH)return;float s=0,m=0,dens=0,rd=0,me=0;int n=0;"
    " [unroll]for(int y=-4;y<=4;y++)[unroll]for(int x=-4;x<=4;x++){int2 p=int2(id.xy)+int2(x,y);if(p.x<0||p.y<0||p.x>=(int)CW||p.y>=(int)CH)continue;uint4 c=CellR[p];s+=c.x;m+=c.y;n+=16;if(abs(x)<3&&abs(y)<3){dens+=c.x;rd+=c.z;me+=c.w;}}"
    " bool quiet=rd>=2.0*me;bool core=CellR[id.xy].x>0&&dens>=8&&m>=0.3*max(1.0,n-s)&&quiet;uint mem=Mem[id.xy];mem=core?15u:(mem?mem-1u:0u);Mem[id.xy]=mem;ActOut[id.xy]=(core||(mem>0&&rd>=24&&quiet))?1u:0u;}"
    "RWTexture2D<float> BlockOut:register(u0);"
    "[numthreads(8,8,1)]void cs_block(uint3 id:SV_DispatchThreadID){if(id.x>=CW||id.y>=CH)return;float o=0;[unroll]for(int y=-5;y<=5;y++)[unroll]for(int x=-5;x<=5;x++){int2 p=clamp(int2(id.xy)+int2(x,y),int2(0,0),int2(CW-1,CH-1));if(ActR[p])o=max(o,saturate((6.0-length(float2(x,y)))/3.0));}BlockOut[id.xy]=o;}"
    "cbuffer F:register(b2){uint useHud;uint useText;uint useFix;uint fpad;float2 full;float2 fpad2;};Texture2D<float2> FixM:register(t10);Texture2D<float2> FixW:register(t11);Texture2D<float4> Prev:register(t8);Texture2D<float> BlockR:register(t9);"
    "float4 ps_compose(float4 p:SV_Position):SV_Target{int2 q=int2(p.xy);if(useHud&&MapR[q])return float4(Org[q].rgb,1);float3 g=Gen[q].rgb;"
    " if(useFix){float2 u=p.xy/full;float2 gs;FixW.GetDimensions(gs.x,gs.y);int2 n=int2(u*gs);float w=FixW.SampleLevel(lin,u,0).x;if(w>0.01){float2 f=FixM[n];float k=FixW[n].y;float3 a=Prev.SampleLevel(lin,(p.xy-0.5*f)/full,0).rgb,b=Org.SampleLevel(lin,(p.xy+0.5*f)/full,0).rgb;g=lerp(g,k<0.25?(a+b)*0.5:k<0.75?a:b,w);}}"
    " if(useText){int2 e=q/(int)STEP;float wt=BlockR.SampleLevel(lin,p.xy/(float2(CW,CH)*4.0*STEP),0);if(wt>0.0){float3 a=Prev[q].rgb,b=Org[q].rgb,v=abs(a-b);bool near=false;"
    "  if(max(v.r,max(v.g,v.b))<=36.0/255.0){[unroll]for(int y=-2;y<=2;y++)[unroll]for(int x=-2;x<=2;x++)near=near||(Bits[clamp(e+int2(x,y),int2(0,0),int2(DW-1,DH-1))]&2u);}"
    "  g=lerp(g,near?(a+b)*0.5:clamp(g,min(a,b)-8.0/255.0,max(a,b)+8.0/255.0),wt);}}"
    " return float4(g,1);}";

static ID3DBlob *compile(const char *src, const char *entry, const char *target)
{
    ID3DBlob *code = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(src, strlen(src), nullptr, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err)))
        logf("shader %s: %s", entry, err ? (const char *)err->GetBufferPointer() : "?");
    release(err);
    return code;
}
static ID3D11VertexShader *make_vs(ID3D11Device *d, const char *src, const char *entry = "main")
{
    ID3D11VertexShader *s = nullptr;
    ID3DBlob *b = compile(src, entry, "vs_5_0");
    if (b) d->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s);
    release(b);
    return s;
}
static ID3D11PixelShader *make_ps(ID3D11Device *d, const char *src, const char *entry = "main")
{
    ID3D11PixelShader *s = nullptr;
    ID3DBlob *b = compile(src, entry, "ps_5_0");
    if (b) d->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s);
    release(b);
    return s;
}
static ID3D11ComputeShader *make_cs(ID3D11Device *d, const char *src, const char *entry)
{
    ID3D11ComputeShader *s = nullptr;
    ID3DBlob *b = compile(src, entry, "cs_5_0");
    if (b) d->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &s);
    release(b);
    return s;
}

// ---- parallel copy ----
// One picture is copied in pieces by this thread and the helpers; the caller is told when the pieces are done, in order, so the
// graphics card can fetch the first ones while the last are still being written.
struct CopyPool
{
    enum { THREADS = 3, MAX_PIECES = 32 };
    struct Piece { uint8_t *d; const uint8_t *s; size_t dp, sp, bytes; int rows; };
    HANDLE wake[THREADS]{}, thread[THREADS]{};
    Piece piece[MAX_PIECES]{};
    int pieces = 0;
    std::atomic<int> next{0}, inside{0};
    std::atomic<bool> done[MAX_PIECES]{};
    volatile LONG stop = 0;
    struct Arg { CopyPool *p; int i; } args[THREADS];
    static void run_piece(const Piece &k)
    {
        if (k.dp == k.sp && k.dp == k.bytes) { memcpy(k.d, k.s, k.bytes * (size_t)k.rows); return; }
        for (int y = 0; y < k.rows; y++) memcpy(k.d + k.dp * y, k.s + k.sp * y, k.bytes);
    }
    bool one()
    {
        int i = next.fetch_add(1);
        if (i >= pieces) return false;
        run_piece(piece[i]);
        done[i].store(true, std::memory_order_release);
        return true;
    }
    static DWORD WINAPI main(void *v)
    {
        auto *a = (Arg *)v;
        CopyPool *p = a->p;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        for (;;)
        {
            WaitForSingleObject(p->wake[a->i], INFINITE);
            if (p->stop) return 0;
            p->inside++;
            while (p->one()) {}
            p->inside--;
        }
    }
    void start()
    {
        for (int i = 0; i < THREADS; i++)
        {
            args[i] = {this, i};
            wake[i] = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            thread[i] = CreateThread(nullptr, 0, main, &args[i], 0, nullptr);
        }
    }
    void finish()
    {
        stop = 1;
        for (int i = 0; i < THREADS; i++)
            if (thread[i]) { SetEvent(wake[i]); WaitForSingleObject(thread[i], 2000); CloseHandle(thread[i]); CloseHandle(wake[i]); thread[i] = nullptr; }
    }
    // piece[0..n) must be filled; submit(i) is called in order on this thread
    template <class F> void run(int n, F submit)
    {
        while (inside) YieldProcessor(); // a helper that woke late for the previous picture
        for (int i = 0; i < n; i++) done[i].store(false, std::memory_order_relaxed);
        pieces = n;
        next.store(0, std::memory_order_release);
        for (int i = 0; i < THREADS; i++) if (thread[i]) SetEvent(wake[i]);
        int sent = 0;
        bool more = true;
        while (sent < n)
        {
            if (done[sent].load(std::memory_order_acquire)) { submit(sent++); continue; }
            if (more) more = one(); else YieldProcessor();
        }
        next.store(MAX_PIECES, std::memory_order_release);
    }
};

// ---- state ----
struct Lock { SRWLOCK l = SRWLOCK_INIT; };
static HWND window;
static ID3D11Device *dev;             // frame device
static ID3D11DeviceContext *ctx;      // used inside gl
static Lock gl;
static IDXGIAdapter1 *adapter;
static CopyPool pool;
static int W, H;
static VideoFormat fmt;
static ID3D11Texture2D *planeY, *planeUV, *packed, *tex[TEXTURES];
// One piece of a picture on its way: a band of rows of one plane. Three sets, so a band is never written while it is still read.
struct Band { ID3D11Texture2D *plane, *stage[3]; size_t offset, pitch, bytes; int y, rows; };
static Band bands[CopyPool::MAX_PIECES];
static int band_count, band_set, band_steps; // band_steps: pieces of the luma (or packed) plane; the chroma pieces follow
static ID3D11RasterizerState *scissor;
static ID3D11ShaderResourceView *srvY, *srvUV, *srvPacked, *srvTex[TEXTURES];
static ID3D11RenderTargetView *rtv[TEXTURES];
static HANDLE nt[TEXTURES];
static ID3D11Query *query[INPUTS], *hud_query, *spare_query;
static ID3D11Fence *fence;              // converted originals: can be waited for without the context
static ID3D11DeviceContext4 *ctx4;
static UINT64 fence_next, fence_of[INPUTS];
static ID3D11VertexShader *vs_full, *vs_hud;
static ID3D11PixelShader *ps_convert, *ps_compose;
static ID3D11ComputeShader *cs_count, *cs_map, *cs_luma, *cs_bits, *cs_cell, *cs_act, *cs_block;
static ID3D11Buffer *cb_convert, *cb_hud, *cb_text, *cb_flags;
// text protection: luma of the last two originals, what each pixel is (two, alternating), counts per cell, memory, blocks
static struct Map { ID3D11Texture2D *t; ID3D11ShaderResourceView *s; ID3D11UnorderedAccessView *u; } t_luma[2], t_bits[2], t_cell, t_mem, t_act, t_block;
static int text_turn, text_pairs, text_dw, text_dh;
static ID3D11SamplerState *linear;
static ID3D11Texture2D *hud_ref, *hud_count, *hud_map;
static ID3D11UnorderedAccessView *u_ref, *u_count, *u_map;
static ID3D11ShaderResourceView *s_count, *s_map;
static unsigned long long hud_sources;
static volatile LONG tex_generation; // the presenter reopens the shared textures when this changes
static Lock tex_lock;                // nt[] while they are replaced
static bool have_multi, have_nr;

// capture side
static int cur = -1, prev = -1, next_out, next_set;
static uint64_t ids[INPUTS], next_id;
static bool cut_next;
static int behind;                    // pictures in a row that found the engine still busy
static int extra_lines = 360;           // motion search of the extrapolation
static volatile LONG slow_count;      // made pictures in a row that took longer than the game's own interval
static std::vector<uint8_t> sample_now, sample_last;
static std::vector<uint8_t> sample_of[INPUTS];
static double src_ms = 1000.0 / 60;
static LONGLONG last_source;

// worker
static struct Job { int a, b, out, set, m; uint64_t ida, idb; LONGLONG arrival; bool gen, late, fresh; double rate, past; bool early; UINT64 fence; uint64_t seen; } job; // rate, past: the motion found belongs to a longer or shorter time than usual
static volatile LONG busy, stop_all, gen_fail, engine_broken, made_with_engine; // the first pictures of an engine may take seconds (shaders are built)
static volatile LONG64 busy_since;
static HANDLE shown_event; // an original was presented
static HANDLE job_wake, worker_handle, present_handle, setup_handle, setup_wake;
static double lead_ms = 4;
static LONGLONG last_due;

// engine
static Engine engine;
static Lock engine_lock; // exclusive: setup or worker
static std::string engine_reason;
static Lock reason_lock;
static int engine_w, engine_h, engine_model = -1;
static bool engine_multi, engine_fine; // fine: FSR 3 at 4K with the full flow
static int flow_level, flow_user, flow_n;
static double flow_ms;
static ULONGLONG flow_since;
static bool flow_raised;

// RIFE 4.6 looks nearly the same with a quarter motion size at 1440p and more, and is several ms faster there.
// a 4K 30 fps game: 4.15 lite with the full motion size was both slower and worse on fast camera turns (15->30 test,
// same pairs: full 20.87 / half 25.26 dB turning, 38.19 / 38.33 calm; 58 / 25 ms), so it now uses half at 30 fps too.
static int flow_base(int model) { (void)model; return (int64_t)W * H >= 2560LL * 1440 ? 2 : 1; }
static int engine_family() { return engine_model >= 0 ? model_by_id(engine_model)->family : -1; }
// Motion check of made pictures: asked for, or where the picture matters most with a model that leaves ghosts beside moving figures.
static bool nr_wanted();
static ID3D11ShaderResourceView *original_view(int slot);
static bool repair_on() { return tune.repair || (!bench_running && !tune.early && tune.share >= 50 && tune.mult == 2 && (engine_family() == FAMILY_FSR3 || engine_family() == FAMILY_AMDFG)); }
static void fill_mult(int &m, const ModelInfo *info) { m = tune.mult; if (m < 2 || m > 4 || (m > 2 && !info->multi)) m = 2; }

// streaming of a picture while it comes in (below)
static Lock pic_lock; // the capture thread's picture, or the streaming thread
struct Hold { Lock &l; Hold(Lock &x) : l(x) { AcquireSRWLockExclusive(&l.l); } ~Hold() { ReleaseSRWLockExclusive(&l.l); } };
struct Known { uint8_t *p; bool armed; uint64_t saved[CopyPool::MAX_PIECES]; };
static Known known[16];
static int known_count;
static uint64_t mark;
static bool no_stream;
static struct { int buffer = -1, set, slot = -1, rows; unsigned done, seen; uint64_t hash[CopyPool::MAX_PIECES], last[CopyPool::MAX_PIECES]; bool differs; } stream;
static HANDLE stream_wake, stream_handle;

// Starting early: the engine starts when most of the picture is there; the lines still missing are guessed from the motion before
// (they are in the picture only while the engine reads it, the original itself is always shown complete).
enum { EARLY_NONE, EARLY_WAITING, EARLY_COMPLETE, EARLY_ORIGINAL, EARLY_NOTHING };
static struct { volatile LONG state; int slot; HANDLE full; LONGLONG arrival; uint64_t id; bool fresh; } ahead;
static ID3D11Texture2D *pred;
static ID3D11RenderTargetView *pred_rtv;
static uint64_t pred_of, fed_of; // the original the guess continues, the last one the motion search saw
static double engine_ms = 4;     // how long an engine takes
static double usual_change = 8;

// ---- present queue ----
struct Item { int tex, made; LONGLONG due, arrival; uint64_t order; }; // order: never goes back on the screen
static Item items[12];
static int item_count;
static Lock item_lock;
static HANDLE item_wake;
static volatile LONG redraw;
static int shown_of(int slot);
static void post(int t, int made, LONGLONG due, LONGLONG arrival, uint64_t order)
{
    if (!made) t = shown_of(t);
    AcquireSRWLockExclusive(&item_lock.l);
    if (item_count == 12) { memmove(items, items + 1, sizeof(Item) * 11); item_count = 11; num.dropped = num.dropped + 1; }
    int i = item_count++;
    while (i > 0 && items[i - 1].due > due) { items[i] = items[i - 1]; --i; }
    items[i] = {t, made, due, arrival, order};
    ReleaseSRWLockExclusive(&item_lock.l);
    SetEvent(item_wake);
}

// ---- GPU waits ----
static bool wait_query(ID3D11Query *q)
{
    ULONGLONG start = GetTickCount64();
    for (int spin = 0;; spin++)
    {
        BOOL done = FALSE;
        AcquireSRWLockExclusive(&gl.l);
        HRESULT hr = ctx->GetData(q, &done, sizeof done, 0);
        ReleaseSRWLockExclusive(&gl.l);
        if (hr == S_OK && done) return true;
        if (FAILED(hr) || GetTickCount64() - start > 2000) return false;
        if (spin < 200) YieldProcessor(); else SwitchToThread();
    }
}

static bool wait_converted(int slot, UINT64 value = 0)
{
    if (!fence) return wait_query(query[slot]);
    if (!value) value = fence_of[slot];
    ULONGLONG start = GetTickCount64();
    for (int spin = 0;; spin++)
    {
        UINT64 v = fence->GetCompletedValue();
        if (v == UINT64_MAX) return false; // the device is gone
        if (v >= value) return true;
        if (GetTickCount64() - start > 2000) return false;
        if (spin < 2000) YieldProcessor(); else SwitchToThread();
    }
}

// ---- textures ----
static void free_textures(int from, int to)
{
    AcquireSRWLockExclusive(&tex_lock.l);
    for (int i = from; i < to; i++)
    {
        if (nt[i]) { CloseHandle(nt[i]); nt[i] = nullptr; }
        release(rtv[i]);
        release(srvTex[i]);
        release(tex[i]);
    }
    InterlockedIncrement(&tex_generation);
    ReleaseSRWLockExclusive(&tex_lock.l);
}
static bool make_textures(int from, int to)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)W;
    d.Height = (UINT)H;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    AcquireSRWLockExclusive(&tex_lock.l);
    bool ok = true;
    for (int i = from; ok && i < to; i++)
    {
        IDXGIResource1 *r = nullptr;
        ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &tex[i])) && SUCCEEDED(tex[i]->QueryInterface(IID_PPV_ARGS(&r))) &&
             SUCCEEDED(r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &nt[i])) &&
             SUCCEEDED(dev->CreateRenderTargetView(tex[i], nullptr, &rtv[i])) && SUCCEEDED(dev->CreateShaderResourceView(tex[i], nullptr, &srvTex[i]));
        release(r);
    }
    if (from == 0)
    {
        release(pred_rtv);
        release(pred);
        pred_of = 0;
        d.MiscFlags = 0;
        if (ok && (FAILED(dev->CreateTexture2D(&d, nullptr, &pred)) || FAILED(dev->CreateRenderTargetView(pred, nullptr, &pred_rtv)))) release(pred);
    }
    InterlockedIncrement(&tex_generation);
    ReleaseSRWLockExclusive(&tex_lock.l);
    if (!ok) logf("video: textures failed");
    return ok;
}
static void free_input()
{
    for (int i = 0; i < band_count; i++)
        for (auto &t : bands[i].stage) release(t);
    band_count = 0;
    release(srvY);
    release(srvUV);
    release(srvPacked);
    release(planeY);
    release(planeUV);
    release(packed);
    release(u_ref);
    release(u_count);
    release(u_map);
    release(s_count);
    release(s_map);
    release(hud_ref);
    release(hud_count);
    release(hud_map);
    stream.buffer = -1;
    known_count = 0;
    for (Map *x : {&t_luma[0], &t_luma[1], &t_bits[0], &t_bits[1], &t_cell, &t_mem, &t_act, &t_block}) { release(x->s); release(x->u); release(x->t); }
    hud_sources = 0;
    text_pairs = 0;
}
// A plane on the graphics card and the bands it is filled with.
static bool make_plane(int w, int h, DXGI_FORMAT f, size_t offset, size_t pitch, size_t bytes, int count, ID3D11Texture2D **t, ID3D11ShaderResourceView **v)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, t)) || FAILED(dev->CreateShaderResourceView(*t, nullptr, v))) return false;
    d.BindFlags = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    for (int i = 0; i < count; i++)
    {
        int y0 = h * i / count, y1 = h * (i + 1) / count;
        Band &b = bands[band_count];
        b = {*t, {}, offset + pitch * (size_t)y0, pitch, bytes, y0, y1 - y0};
        d.Height = (UINT)(y1 - y0);
        band_count++;
        for (auto &s : b.stage)
            if (FAILED(dev->CreateTexture2D(&d, nullptr, &s))) return false;
    }
    return true;
}
static bool make_map(Map &x, int w, int h, DXGI_FORMAT f)
{
    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &x.t)) || FAILED(dev->CreateShaderResourceView(x.t, nullptr, &x.s)) || FAILED(dev->CreateUnorderedAccessView(x.t, nullptr, &x.u))) return false;
    const UINT zero[4] = {};
    const float zerof[4] = {};
    if (f == DXGI_FORMAT_R8_UNORM) ctx->ClearUnorderedAccessViewFloat(x.u, zerof);
    else ctx->ClearUnorderedAccessViewUint(x.u, zero);
    return true;
}
static LONG range_used;
static volatile bool pq_input; // the picture is HDR10 (PQ, BT.2020) from a game console
static LONG range_of(const VideoFormat &f) { return tune.range == 1 || (tune.range == 0 && f.full); }
static void convert_constants(const VideoFormat &f) // inside gl
{
    range_used = range_of(f);
    pq_input = f.pixel == Pixel::p010 && tune.hdr_in != 0;
    struct { UINT w, h, kind, full, sd, flip, pq, pad; } c = {(UINT)f.w, (UINT)f.h, f.pixel == Pixel::nv12 ? 0u : f.pixel == Pixel::p010 ? 1u : f.pixel == Pixel::yuy2 ? 2u : f.pixel == Pixel::uyvy ? 4u : f.pixel == Pixel::bgra ? 5u : 3u, (UINT)range_used, f.h < 720 ? 1u : 0u, f.bottom_up ? 1u : 0u, (UINT)pq_input, 0};
    ctx->UpdateSubresource(cb_convert, 0, nullptr, &c, 0, 0);
}
static bool make_input(const VideoFormat &f)
{
    free_input();
    bool ok = false;
    size_t line = (size_t)f.w * (f.pixel == Pixel::p010 ? 2 : 1), y_size = line * (size_t)f.h, row3 = (size_t)((f.w * 3 + 3) & ~3);
    int n = f.h > 1200 ? 16 : 8, m = n;
    band_steps = n;
    switch (f.pixel)
    {
    case Pixel::nv12: ok = make_plane(f.w, f.h, DXGI_FORMAT_R8_UNORM, 0, line, line, n, &planeY, &srvY) && make_plane(f.w / 2, f.h / 2, DXGI_FORMAT_R8G8_UNORM, y_size, line, line, m, &planeUV, &srvUV); break;
    case Pixel::p010: ok = make_plane(f.w, f.h, DXGI_FORMAT_R16_UNORM, 0, line, line, n, &planeY, &srvY) && make_plane(f.w / 2, f.h / 2, DXGI_FORMAT_R16G16_UNORM, y_size, line, line, m, &planeUV, &srvUV); break;
    case Pixel::yuy2: ok = make_plane(f.w / 2, f.h, DXGI_FORMAT_R8G8B8A8_UNORM, 0, (size_t)f.w * 2, (size_t)f.w * 2, n, &packed, &srvPacked); break;
    case Pixel::uyvy: ok = make_plane(f.w / 2, f.h, DXGI_FORMAT_R8G8B8A8_UNORM, 0, (size_t)f.w * 2, (size_t)f.w * 2, n, &packed, &srvPacked); break;
    case Pixel::bgra: ok = make_plane(f.w, f.h, DXGI_FORMAT_B8G8R8A8_UNORM, 0, (size_t)f.w * 4, (size_t)f.w * 4, n, &packed, &srvPacked); break;
    case Pixel::bgr24: ok = make_plane(f.w * 3, f.h, DXGI_FORMAT_R8_UNORM, 0, row3, (size_t)f.w * 3, n, &planeY, &srvY); break;
    default: break;
    }
    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)f.w;
    d.Height = (UINT)f.h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R32_UINT;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &hud_ref)) && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &hud_count)); // read and written: only R32 can do both everywhere
    d.Format = DXGI_FORMAT_R8_UINT;
    ok = ok && SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &hud_map));
    ok = ok && SUCCEEDED(dev->CreateUnorderedAccessView(hud_ref, nullptr, &u_ref)) && SUCCEEDED(dev->CreateUnorderedAccessView(hud_count, nullptr, &u_count)) &&
         SUCCEEDED(dev->CreateUnorderedAccessView(hud_map, nullptr, &u_map)) && SUCCEEDED(dev->CreateShaderResourceView(hud_count, nullptr, &s_count)) &&
         SUCCEEDED(dev->CreateShaderResourceView(hud_map, nullptr, &s_map));
    UINT step = f.h >= 2000 ? 2 : 1, dw = (UINT)f.w / step, dh = (UINT)f.h / step, cw = dw / 4, ch = dh / 4;
    for (auto &x : t_luma) ok = ok && make_map(x, (int)dw, (int)dh, DXGI_FORMAT_R8_UNORM);
    for (auto &x : t_bits) ok = ok && make_map(x, (int)dw, (int)dh, DXGI_FORMAT_R8_UINT);
    ok = ok && make_map(t_cell, (int)cw, (int)ch, DXGI_FORMAT_R8G8B8A8_UINT) && make_map(t_mem, (int)cw, (int)ch, DXGI_FORMAT_R32_UINT) && make_map(t_act, (int)cw, (int)ch, DXGI_FORMAT_R8_UINT) && make_map(t_block, (int)cw, (int)ch, DXGI_FORMAT_R8_UNORM);
    UINT tx[8] = {dw, dh, cw, ch, step, 0, 0, 0};
    ctx->UpdateSubresource(cb_text, 0, nullptr, tx, 0, 0);
    text_dw = (int)dw;
    text_dh = (int)dh;
    convert_constants(f);
    UINT h[4] = {(UINT)f.w, (UINT)f.h, 8, 30};
    ctx->UpdateSubresource(cb_hud, 0, nullptr, h, 0, 0);
    if (!ok) logf("video: input textures failed %s %dx%d", pixel_name(f.pixel), f.w, f.h);
    return ok;
}

// ---- input: one picture from the capture card ----
static size_t frame_bytes(const VideoFormat &f)
{
    size_t n = (size_t)f.w * f.h;
    return f.pixel == Pixel::nv12 ? n * 3 / 2 : f.pixel == Pixel::p010 ? n * 3 : f.pixel == Pixel::yuy2 || f.pixel == Pixel::uyvy ? n * 2 : f.pixel == Pixel::bgra ? n * 4 : (size_t)((f.w * 3 + 3) & ~3) * f.h;
}
// Every eighth line of the picture (luma for NV12): enough to tell a repeated picture and a scene cut.
static void take_samples(const uint8_t *data, const VideoFormat &f)
{
    size_t line = f.pixel == Pixel::nv12 ? f.w : f.pixel == Pixel::bgr24 ? (size_t)((f.w * 3 + 3) & ~3) : f.pixel == Pixel::bgra ? (size_t)f.w * 4 : (size_t)f.w * 2;
    int n = f.h / 8;
    sample_now.resize(line * n);
    for (int i = 0; i < n; i++) memcpy(&sample_now[line * i], data + line * (i * 8 + 3), line);
}
// The same picture again, also when the console put a notice over the game: then it draws every picture anew, with a little noise.
static bool same_picture()
{
    const std::vector<uint8_t> &a = sample_now, &b = sample_last;
    if (a.size() != b.size()) return false;
    if (a == b) return true;
    size_t n = a.size(), sum = 0;
    bool wide = fmt.pixel == Pixel::p010; // upper bytes only
    for (size_t at = 0; at < n; at += 4096)
    {
        size_t end = std::min(n, at + 4096);
        unsigned apart = 0, part = 0;
        for (size_t i = at; i < end; i++)
        {
            unsigned d = (unsigned)(a[i] > b[i] ? a[i] - b[i] : b[i] - a[i]);
            if (wide && !(i & 1)) d = 0;
            apart += d > 2;
            part += d;
        }
        if (apart) return false;
        sum += part;
    }
    return sum * 20 <= n; // on average at most 1/20 of a level
}
static double sample_difference(const std::vector<uint8_t> &a, const std::vector<uint8_t> &b)
{
    if (a.size() != b.size() || a.empty()) return 255;
    unsigned long long sum = 0, n = 0;
    for (size_t i = fmt.pixel == Pixel::p010 ? 1 : 0; i < a.size(); i += 32, n++) sum += (unsigned)abs((int)a[i] - (int)b[i]);
    return (double)sum / (double)n;
}
static void wait_worker(unsigned ms = 3000) { ULONGLONG t = GetTickCount64(); while (busy && GetTickCount64() - t < ms) SwitchToThread(); }

static bool change_format(const VideoFormat &f)
{
    logf("video: picture %s %dx%d %.2f", pixel_name(f.pixel), f.w, f.h, f.fps);
    wait_worker();
    AcquireSRWLockExclusive(&engine_lock.l); // no engine call while its textures go away
    engine.close();
    engine_model = -1;
    num.engine_state = 0;
    ReleaseSRWLockExclusive(&engine_lock.l);
    AcquireSRWLockExclusive(&item_lock.l);
    item_count = 0;
    ReleaseSRWLockExclusive(&item_lock.l);
    AcquireSRWLockExclusive(&gl.l);
    free_textures(0, TEXTURES);
    have_multi = have_nr = false;
    W = f.w;
    H = f.h;
    bool ok = make_input(f) && make_textures(0, MULTI0);
    if (!ok) { logf("video: no memory for this picture"); free_textures(0, TEXTURES); }
    ReleaseSRWLockExclusive(&gl.l);
    fmt = f;
    cur = prev = -1;
    sample_last.clear();
    last_source = 0;
    src_ms = f.fps > 10 ? 1000.0 / f.fps : 1000.0 / 60;
    num.w = W;
    num.h = H;
    SetEvent(setup_wake);
    return ok;
}
// Text protection for the pair (older, newer); on the frame device, inside gl.
static void text_pass(int older, int newer)
{
    ID3D11ShaderResourceView *none[8] = {};
    ID3D11UnorderedAccessView *nou[5] = {};
    ID3D11Buffer *cb[2] = {cb_hud, cb_text};
    int now = text_turn ^ 1, cw = text_dw / 4, ch = text_dh / 4;
    text_turn = now;
    ctx->CSSetConstantBuffers(0, 2, cb);
    ctx->CSSetSamplers(0, 1, &linear);
    auto run = [&](ID3D11ComputeShader *s, int first, std::initializer_list<ID3D11ShaderResourceView *> in, int slot, std::initializer_list<ID3D11UnorderedAccessView *> out, int w, int h) {
        ctx->CSSetShader(s, nullptr, 0);
        ctx->CSSetShaderResources((UINT)first, (UINT)in.size(), in.begin());
        ctx->CSSetUnorderedAccessViews((UINT)slot, (UINT)out.size(), out.begin(), nullptr);
        ctx->Dispatch((UINT)(w + 7) / 8, (UINT)(h + 7) / 8, 1);
        ctx->CSSetUnorderedAccessViews((UINT)slot, (UINT)out.size(), nou, nullptr);
        ctx->CSSetShaderResources(0, 8, none);
    };
    run(cs_luma, 3, {original_view(newer)}, 3, {t_luma[now].u}, text_dw, text_dh);
    if (older < 0) { text_pairs = 0; return; }
    run(cs_bits, 0, {t_luma[now ^ 1].s, t_luma[now].s, original_view(older), original_view(newer), t_bits[now ^ 1].s}, 4, {t_bits[now].u}, text_dw, text_dh);
    run(cs_cell, 5, {t_bits[now].s}, 5, {t_cell.u}, cw, ch);
    run(cs_act, 6, {t_cell.s}, 6, {t_mem.u, t_act.u}, cw, ch);
    run(cs_block, 7, {t_act.s}, 0, {t_block.u}, cw, ch);
    text_pairs++;
}
// ---- streaming ----
// A capture card writes a picture into the driver's buffer line by line while it comes in over HDMI (16 ms at 60 Hz), and hands it
// over when the last line is there. The pieces that are complete go to the graphics card right away, so only the last one is left
// when the picture is handed over. A piece is complete when the mark at its end (written while the buffer was ours) is gone; what
// was read early is checked against the finished picture, and read again if it differs. Drivers that fill a buffer at once, or
// not in order, simply get the ordinary way.
static uint64_t *mark_at(uint8_t *p, int i)
{
    Band &b = bands[i];
    return (uint64_t *)(p + b.offset + b.pitch * (size_t)(b.rows - 1) + b.bytes - 8);
}
static void stream_disarm(int i)
{
    if (!known[i].armed) return;
    known[i].armed = false;
    for (int k = 0; k < band_count; k++)
    {
        uint64_t *m = mark_at(known[i].p, k);
        if (*m == mark) *m = known[i].saved[k]; // never overwritten: put back what was there
    }
}
static uint64_t piece_hash(const uint8_t *p, int i)
{
    Band &b = bands[i];
    uint64_t h = 1469598103934665603ull;
    for (int y = b.rows - 1; y >= 0; y -= 16)
    {
        const uint64_t *r = (const uint64_t *)(p + b.offset + b.pitch * (size_t)y);
        size_t n = b.bytes / 8;
        for (size_t x = (size_t)(y >> 4 & 15); x < n; x += 16) h = (h ^ r[x]) * 1099511628211ull;
        h = (h ^ r[n - 1]) * 1099511628211ull;
    }
    return h;
}
static void stream_check() // the picture is finished: is what was read early still what is there?
{
    uint8_t *p = known[stream.buffer].p;
    for (int i = 0; i < band_count; i++)
        if (stream.done & (1u << i) && piece_hash(p, i) != stream.hash[i])
        {
            stream.done &= ~(1u << i);
            stream.seen &= ~(1u << i);
            stream.rows = 0;
            static int said;
            if (said++ < 5) logf("video: a piece changed after it was read");
        }
}
// Colour conversion of the rows [from, to) into a slot; on the frame device, inside gl.
static void convert_rows(int slot, int from, int to)
{
    if (to <= from) return;
    D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
    D3D11_RECT r = {0, from, W, to};
    ID3D11ShaderResourceView *in[3] = {srvY, srvUV, srvPacked}, *none[3] = {};
    ID3D11RenderTargetView *nort = nullptr;
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_full, nullptr, 0);
    ctx->PSSetShader(ps_convert, nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, &cb_convert);
    ctx->PSSetSamplers(0, 1, &linear);
    ctx->PSSetShaderResources(0, 3, in);
    ctx->RSSetState(scissor);
    ctx->RSSetScissorRects(1, &r);
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &rtv[slot], nullptr);
    ctx->Draw(3, 0);
    ctx->OMSetRenderTargets(1, &nort, nullptr);
    ctx->RSSetState(nullptr);
    ctx->PSSetShaderResources(0, 3, none); // the planes are written again right after this
}
static void stream_upload(int i)
{
    Band &b = bands[i];
    D3D11_MAPPED_SUBRESOURCE m{};
    AcquireSRWLockExclusive(&gl.l);
    HRESULT hr = ctx->Map(b.stage[stream.set], 0, D3D11_MAP_WRITE, 0, &m);
    ReleaseSRWLockExclusive(&gl.l);
    if (FAILED(hr)) return;
    CopyPool::run_piece({(uint8_t *)m.pData, known[stream.buffer].p + b.offset, m.RowPitch, b.pitch, b.bytes, b.rows});
    AcquireSRWLockExclusive(&gl.l);
    ctx->Unmap(b.stage[stream.set], 0);
    ctx->CopySubresourceRegion(b.plane, 0, 0, (UINT)b.y, 0, b.stage[stream.set], 0, nullptr);
    ReleaseSRWLockExclusive(&gl.l);
    stream.done |= 1u << i;
}
static double share_ms();
// Enough of the picture is there: the engine starts. rows: how many lines are converted.
static bool early_wanted()
{
    const ModelInfo *info = model_by_id(engine_model);
    int m = 2;
    fill_mult(m, info);
    return tune.early && tune.on && !nr_wanted() && !repair_on() && m == 2 && fence && pred && num.engine_state == 2 && info->family != FAMILY_EXTRA && engine_w == W && engine_h == H && !num.too_slow;
}
static void early_start(int slot, int rows, const uint8_t *data)
{
    double scan = 960.0 / (fmt.fps > 10 ? fmt.fps : 60), part = tune.early > 1 ? std::min((engine_ms + share_ms() + 0.3) / scan, 0.34) : std::min((engine_ms + 0.3) / scan, 0.2);
    if (rows < H * (1 - part) - H / 40 || rows >= H || busy || ahead.state || cur < 0 || slot == cur || slot == prev || cut_next || pred_of != ids[cur]) return;
    // the picture before once more, a cut, or the camera jumping somewhere else
    if (sample_last.empty()) return;
    {
        size_t line = fmt.pixel == Pixel::nv12 ? (size_t)fmt.w : fmt.pixel == Pixel::bgr24 ? (size_t)((fmt.w * 3 + 3) & ~3) : fmt.pixel == Pixel::bgra ? (size_t)fmt.w * 4 : (size_t)fmt.w * 2;
        unsigned long long sum = 0, n = 0, apart = 0;
        for (int i = 0; i * 8 + 3 < rows; i += 2)
            for (size_t x = fmt.pixel == Pixel::p010 ? 1 : 0; x < line; x += 8, n++)
            {
                unsigned a = (unsigned)abs((int)data[line * (size_t)(i * 8 + 3) + x] - (int)sample_last[line * (size_t)i + x]);
                sum += a;
                apart += a > 2;
            }
        double d = n ? (double)sum / (double)n : 0;
        if (apart < 6) return; // the picture before once more, as far as it is here
        if (tune.cut && (d > tune.cut || d > 2.0 * std::max(usual_change, 8.0))) return;
    }
    D3D11_BOX rest = {0, (UINT)rows, 0, (UINT)W, (UINT)H, 1};
    AcquireSRWLockExclusive(&gl.l);
    ctx->CopySubresourceRegion(tex[slot], 0, 0, (UINT)rows, 0, pred, 0, &rest);
    UINT64 value = ++fence_next;
    ctx4->Signal(fence, value);
    ctx->Flush();
    ReleaseSRWLockExclusive(&gl.l);
    ahead.slot = slot;
    ResetEvent(ahead.full);
    InterlockedExchange(&ahead.state, EARLY_WAITING);
    job = {cur, slot, next_out, next_set, 2, ids[cur], ++next_id, qpc() + ticks_of(scan * (H - rows) / H), true, false, false, 1.0, 1.0, true, value};
    next_out ^= 1;
    next_set ^= 1;
    InterlockedExchange64(&busy_since, qpc());
    InterlockedExchange(&busy, 1);
    SetEvent(job_wake);
}
static bool early_end(LONG how) // the picture is complete (or is not the one that was expected)
{
    if (InterlockedCompareExchange(&ahead.state, how, EARLY_WAITING) != EARLY_WAITING) return false;
    SetEvent(ahead.full);
    return true;
}
static void stream_step() // one look
{
    if (!tex[0] || !known_count) return;
    bool planar = planeUV != nullptr;
    if (stream.buffer < 0)
    {
        for (int i = 0; i < known_count; i++)
            if (known[i].armed && *(volatile uint64_t *)mark_at(known[i].p, 0) != mark) { stream.buffer = i; break; }
        if (stream.buffer < 0) return;
        stream.set = band_set;
        band_set = (band_set + 1) % 3;
        stream.done = stream.seen = 0;
        stream.rows = 0;
        stream.differs = false;
        int slot = 0;
        while (slot == cur || slot == prev) ++slot;
        stream.slot = busy && (job.a == slot || job.b == slot) ? -1 : slot;
    }
    uint8_t *p = known[stream.buffer].p;
    bool any = false, more = true;
    for (int k = 0; k < band_steps && more; k++)
    {
        int at[2] = {k, planar ? band_steps + k : -1};
        for (int i : at)
        {
            if (i < 0 || stream.seen & (1u << i)) continue;
            if (*(volatile uint64_t *)mark_at(p, i) == mark) { more = false; break; }
            std::atomic_thread_fence(std::memory_order_acquire);
            stream.seen |= 1u << i;
            stream.hash[i] = piece_hash(p, i);
            if (stream.hash[i] != stream.last[i]) stream.differs = true;
            any = true;
        }
    }
    if (!any || !stream.differs) return; // so far the picture before once more: nothing to do
    for (int k = 0; k < band_steps; k++)
        for (int i : {k, planar ? band_steps + k : k})
            if (stream.seen & (1u << i) && !(stream.done & (1u << i))) stream_upload(i);
    int rows = 0;
    for (int k = 0; k < band_steps && stream.done & (1u << k) && (!planar || stream.done & (1u << (band_steps + k))); k++) rows = bands[k].y + bands[k].rows;
    if (planar && rows < H) rows = std::max(0, rows - 4) & ~1; // the chroma row below is needed too
    AcquireSRWLockExclusive(&gl.l);
    if (stream.slot >= 0 && !fmt.bottom_up && rows > stream.rows) { convert_rows(stream.slot, stream.rows, rows); stream.rows = rows; }
    ctx->Flush();
    ReleaseSRWLockExclusive(&gl.l);
    if (stream.slot >= 0 && stream.rows && early_wanted()) early_start(stream.slot, stream.rows, p);
}
static DWORD WINAPI stream_main(void *)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* high resolution */, TIMER_ALL_ACCESS);
    while (!stop_all)
    {
        bool waiting;
        {
            Hold hold(pic_lock);
            stream_step();
            waiting = known_count > 0;
        }
        LARGE_INTEGER due;
        double left = (fmt.fps > 10 ? 1000.0 / fmt.fps : 16.7) - ms_of(qpc() - num.last_arrival); // until the picture is complete
        due.QuadPart = left > (tune.early ? 8 : 4) ? -20000 : -4000; // asleep, not spinning: 2 ms, near the end 0.4 ms
        if (waiting && timer && ms_of(qpc() - num.last_arrival) < 200 && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 50);
        else WaitForSingleObject(stream_wake, 100);
    }
    if (timer) CloseHandle(timer);
    return 0;
}
void video_buffer(uint8_t *data, size_t size, const VideoFormat &f)
{
    if (stop_all) return;
    Hold hold(pic_lock);
    if (f.pixel != fmt.pixel || f.w != W || f.h != H || size < frame_bytes(f) || !tex[0] || no_stream) return;
    int i = 0;
    while (i < known_count && known[i].p != data) ++i;
    if (i == known_count)
    {
        MEMORY_BASIC_INFORMATION mi;
        if (i == 16 || !VirtualQuery(data, &mi, sizeof mi) || !(mi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) || (uint8_t *)mi.BaseAddress + mi.RegionSize < data + frame_bytes(f)) return;
        known[known_count++] = {data, false, {}};
        logf("video: driver buffer %d", known_count);
    }
    if (!mark) mark = 0x5AC3A1D7914E6B2Full ^ ((uint64_t)qpc() << 17);
    if (stream.buffer == i) stream.buffer = -1;
    for (int k = 0; k < band_count; k++)
    {
        uint64_t *m = mark_at(data, k);
        known[i].saved[k] = *m;
        *m = mark;
    }
    std::atomic_thread_fence(std::memory_order_release);
    known[i].armed = true;
    SetEvent(stream_wake);
}
void video_buffers_gone()
{
    Hold hold(pic_lock);
    known_count = 0;
    stream.buffer = -1;
}
// ---- trial: DLSS 5 on the originals ----
static bool raw_used[INPUTS + 2];
static std::string nr_reason;
static volatile LONG nr_bad, nr_good; // it cannot be used as things are; pictures done since it was opened
static bool nr_fed; // the motion of this picture has been looked for
static bool nr_wanted() { return tune.nr != 0 && !bench_running && !nr_bad && !pq_input; }
std::string video_nr_error()
{
    AcquireSRWLockShared(&reason_lock.l);
    std::string s = nr_reason;
    ReleaseSRWLockShared(&reason_lock.l);
    return s;
}
static int raw_of(int slot) { return slot < INPUTS ? slot : INPUTS + (slot - SPARE0); }
// The picture of an original that goes to the screen.
static int shown_of(int slot)
{
    if (slot >= INPUTS && (slot < SPARE0 || slot >= MULTI0)) return slot;
    return have_nr && raw_used[raw_of(slot)] ? NRO0 + raw_of(slot) : slot;
}
static ID3D11ShaderResourceView *original_view(int slot) { return srvTex[slot]; }
// 1 to 5: 0.75, 1, 1.25, 1.5 and 2 layers. Above 1 the second layer is mixed in.
struct NrStep { int layers; float power, blend; };
static NrStep nr_step_of()
{
    static const NrStep all[5] = {{1, 0.75f, 1}, {1, 1, 1}, {2, 1, 0.25f}, {2, 1, 0.5f}, {2, 1, 1}};
    return all[std::min(5L, std::max(1L, (long)tune.nr_layers)) - 1];
}
// Skin, 0 to 100: how young. 0: all the structure the network gives people. More: less of it, and the skin is smoothed as well.
static float nr_skin_of() { return std::max(0.f, 1.f - (float)tune.nr_skin / 50.f * 0.7f); }
static NrLook nr_look() { return NrLook{(float)tune.nr_power / 100.f * nr_step_of().power, (float)tune.nr_detail / 100.f, (float)tune.nr_tone / 100.f, (float)tune.nr_keep / 100.f, nr_step_of().blend, nr_skin_of(), (float)tune.nr_skin / 100.f, (float)tune.nr_steady / 100.f, tune.nr_fast != 0}; }
// A made picture between the originals a and b, with what the network changed in both: the model never sees that, so made pictures and
// originals differ no more than without it. Worker.
static int nr_finish(int t, int a, int b, float f)
{
    if (!have_nr || !raw_used[raw_of(a)] || !raw_used[raw_of(b)]) return t;
    static int turn;
    turn = (turn + 1) % (TEXTURES - NRM0);
    AcquireSRWLockExclusive(&gl.l);
    bool ok = have_nr && rtv[NRM0 + turn] && nr_between(ctx, srvTex[t], rtv[NRM0 + turn], raw_of(a), raw_of(b), f, tune.hud && hud_sources >= 31, nr_look());
    if (ok) { ctx->End(hud_query); ctx->Flush(); }
    ReleaseSRWLockExclusive(&gl.l);
    return ok && wait_query(hud_query) ? NRM0 + turn : t;
}
static void nr_apply(int slot, bool reset) // on the frame device, inside gl
{
    int k = raw_of(slot);
    raw_used[k] = nr_fed = false;
    if (!nr_wanted() || !have_nr || !nr_ready(W, H) || !ctx4 || !fence) return;
    int mw = 0, mh = 0;
    ID3D11ShaderResourceView *motion = nullptr;
    bool alone = !repair_on() && engine_family() != FAMILY_EXTRA; // nobody else needs the motion: a smaller search is enough (2.1 -> 0.6 ms at 4K)
    if (extra_init(dev) && extra_size(dev, ctx, W, H, alone ? 180 : extra_lines))
    {
        if (reset) extra_forget();
        nr_fed = true;
        extra_feed(ctx, srvTex[slot], !alone && engine_family() != FAMILY_EXTRA, alone);
        if (extra_ready()) motion = extra_motion(&mw, &mh);
    }
    raw_used[k] = nr_run(ctx4, srvTex[slot], motion, mw, mh, tune.hud && hud_sources >= 31 ? s_map : nullptr, rtv[NRO0 + k], reset || !motion, nr_look(), k);
    if (raw_used[k]) InterlockedIncrement(&nr_good);
    if (raw_used[k] && tune.nr_order == 1)
    {
        // before generation: the original itself becomes the changed picture, so the model makes its pictures from
        // those. The network keeps its own small copy of the original, so nothing reads the changed one back into it.
        ctx->CopyResource(tex[slot], tex[NRO0 + k]);
        raw_used[k] = false; // shown as it is now; nothing to lay over the made pictures
    }
}
// HUD, text and motion measuring of a converted original; on the frame device, inside gl.
static void measure(int older, int slot, bool fresh)
{
    ID3D11ShaderResourceView *none[5] = {};
    if (tune.hud)
    {
        ID3D11UnorderedAccessView *u[3] = {u_ref, u_count, nullptr}, *nou[3] = {};
        ctx->CSSetConstantBuffers(0, 1, &cb_hud);
        ctx->CSSetShader(cs_count, nullptr, 0);
        ID3D11ShaderResourceView *seen = original_view(slot);
        ctx->CSSetShaderResources(0, 1, &seen);
        ctx->CSSetUnorderedAccessViews(0, 2, u, nullptr);
        ctx->Dispatch((W + 15) / 16, (H + 15) / 16, 1);
        ctx->CSSetUnorderedAccessViews(0, 3, nou, nullptr);
        ctx->CSSetShader(cs_map, nullptr, 0);
        ctx->CSSetShaderResources(1, 1, &s_count);
        u[0] = u[1] = nullptr;
        u[2] = u_map;
        ctx->CSSetUnorderedAccessViews(0, 3, u, nullptr);
        ctx->Dispatch((W + 15) / 16, (H + 15) / 16, 1);
        ctx->CSSetUnorderedAccessViews(0, 3, nou, nullptr);
        ctx->CSSetShaderResources(0, 2, none);
        ctx->Flush();
        hud_sources++;
    }
    else hud_sources = 0;
    if (tune.text) { text_pass(older, slot); ctx->Flush(); }
    else text_pairs = 0;
    static int tries;
    bool repair = repair_on() && engine_family() != FAMILY_EXTRA;
    if (nr_fed) nr_fed = false;
    else if (tune.on && (engine_family() == FAMILY_EXTRA || repair) && (!num.too_slow || ++tries % 120 < 2) && extra_init(dev) && extra_size(dev, ctx, W, H, extra_lines))
    {
        if (older < 0 || fresh) extra_forget();
        extra_feed(ctx, srvTex[slot], repair);
        ctx->Flush();
    }
    else if (!tune.early || repair) extra_forget();
}
static volatile LONG forget_last;
void video_no_signal() { InterlockedExchange(&forget_last, 1); }

bool video_even; // tests: pictures come as fast as they are read
static double gap_before;
// ---- a picture already on the graphics card (screen capture) ----
// Its samples (for repeats and cuts) come from a small copy: every 8th pixel of every 8th line, read back to the CPU.
static ID3D11Texture2D *gsrc, *small_rt, *small_read;
static const D3D11_BOX *gbox;
static ID3D11RenderTargetView *small_rtv;
static ID3D11PixelShader *ps_small;
static const char ps_small_src[] = "Texture2D t:register(t0);float4 main(float4 p:SV_Position):SV_Target{return t.Load(int3(int2(p.xy)*8+3,0));}";
static void gpu_samples() // inside gl
{
    int w = W / 8, h = H / 8;
    D3D11_TEXTURE2D_DESC d{};
    if (small_rt) small_rt->GetDesc(&d);
    if (!small_rt || (int)d.Width != w || (int)d.Height != h)
    {
        release(small_rtv); release(small_rt); release(small_read);
        d = {};
        d.Width = (UINT)w; d.Height = (UINT)h; d.MipLevels = d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        dev->CreateTexture2D(&d, nullptr, &small_rt);
        if (small_rt) dev->CreateRenderTargetView(small_rt, nullptr, &small_rtv);
        d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        dev->CreateTexture2D(&d, nullptr, &small_read);
        if (!ps_small) ps_small = make_ps(dev, ps_small_src);
    }
    if (!small_rtv || !small_read || !ps_small) { sample_now.assign(16, 0); return; }
    D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
    ID3D11ShaderResourceView *none = nullptr;
    ID3D11RenderTargetView *nort = nullptr;
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_full, nullptr, 0);
    ctx->PSSetShader(ps_small, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srvPacked);
    ctx->RSSetState(nullptr);
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &small_rtv, nullptr);
    ctx->Draw(3, 0);
    ctx->OMSetRenderTargets(1, &nort, nullptr);
    ctx->PSSetShaderResources(0, 1, &none);
    ctx->CopyResource(small_read, small_rt);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(small_read, 0, D3D11_MAP_READ, 0, &m))) { sample_now.assign(16, 0); return; }
    sample_now.resize((size_t)w * h * 4);
    for (int y = 0; y < h; y++) memcpy(&sample_now[(size_t)y * w * 4], (const uint8_t *)m.pData + (size_t)m.RowPitch * y, (size_t)w * 4);
    ctx->Unmap(small_read, 0);
}
static void on_picture(const uint8_t *data, size_t size, const VideoFormat &f, LONGLONG arrival);
void video_on_texture(ID3D11Texture2D *src, const D3D11_BOX &box, const VideoFormat &f, LONGLONG arrival)
{
    if (f.pixel != Pixel::bgra) return;
    gsrc = src;
    gbox = &box;
    on_picture(nullptr, 0, f, arrival);
    gsrc = nullptr;
}
void video_alive(LONGLONG arrival)
{
    num.captured = num.captured + 1;
    num.last_arrival = arrival;
}
ID3D11Device *video_device() { return dev; }
void video_on_picture(const uint8_t *data, size_t size, const VideoFormat &f, LONGLONG arrival) { on_picture(data, size, f, arrival); }
static void on_picture(const uint8_t *data, size_t size, const VideoFormat &f, LONGLONG arrival)
{
    if (stop_all || f.pixel == Pixel::none || (!gsrc && size < frame_bytes(f))) return;
    Hold hold(pic_lock);
    for (int i = 0; i < known_count; i++) if (known[i].p == data) stream_disarm(i);
    num.captured = num.captured + 1;
    num.last_arrival = arrival;
    if (f.pixel != fmt.pixel || f.w != W || f.h != H || f.bottom_up != fmt.bottom_up)
    {
        early_end(EARLY_NOTHING);
        if (!change_format(f)) return;
    }
    if (!tex[0]) return;
    fmt.fps = f.fps;
    fmt.full = f.full;
    if (InterlockedExchange(&forget_last, 0)) sample_last.clear();
    if (gsrc)
    {
        // into the input texture first (the samples are read from it); the conversion below takes it as if it had been uploaded
        AcquireSRWLockExclusive(&gl.l);
        LONGLONG c0 = qpc();
        ctx->CopySubresourceRegion(packed, 0, 0, 0, 0, gsrc, 0, gbox);
        gpu_samples();
        num.copy.add(ms_of(qpc() - c0));
        ReleaseSRWLockExclusive(&gl.l);
    }
    else take_samples(data, f);
    if (same_picture()) { early_end(EARLY_NOTHING); stream.buffer = -1; return; } // the game has not drawn a new picture
    std::swap(sample_now, sample_last);
    num.sources = num.sources + 1;
    double gap = 0; // since the picture before
    if (last_source)
    {
        double d = gap = video_even ? src_ms : ms_of(arrival - last_source);
        if (d > 3 && d < 60) { src_ms = 0.9 * src_ms + 0.1 * d; num.interval.add(d); }
    }
    last_source = arrival;

    int slot = 0;
    while (slot == cur || slot == prev) ++slot;
    // The engine is still using this slot (too slow for this computer, or stuck): show the original from a texture of its own, so
    // the game keeps its own frame rate.
    static int spare;
    bool mine = ahead.state == EARLY_WAITING && ahead.slot == slot && stream.buffer >= 0 && known[stream.buffer].p == data; // the engine is at work on this picture
    if (!mine) early_end(EARLY_NOTHING);
    bool aside = !mine && busy && (job.a == slot || job.b == slot);
    if (aside) { spare ^= 1; slot = SPARE0 + spare; }
    LONGLONG t0 = qpc();
    bool streamed = stream.buffer >= 0 && known[stream.buffer].p == data;
    if (streamed) stream_check();
    else { stream.done = stream.seen = 0; stream.set = band_set; band_set = (band_set + 1) % 3; }
    if (stream.slot != slot || !streamed) stream.rows = 0;
    stream.buffer = -1;
    AcquireSRWLockExclusive(&gl.l);
    if (range_used != range_of(fmt)) { convert_constants(fmt); stream.rows = 0; } // what came in so far was converted the old way
    if (!gsrc)
    {
        bool mapped = true;
        int n = 0, which[CopyPool::MAX_PIECES];
        for (int i = 0; i < band_count; i++)
        {
            if (stream.done & (1u << i)) continue;
            D3D11_MAPPED_SUBRESOURCE m{};
            Band &b = bands[i];
            if (FAILED(ctx->Map(b.stage[stream.set], 0, D3D11_MAP_WRITE, 0, &m))) { mapped = false; m.pData = nullptr; }
            which[n] = i;
            pool.piece[n++] = {(uint8_t *)m.pData, data + b.offset, m.RowPitch, b.pitch, b.bytes, m.pData ? b.rows : 0};
        }
        pool.run(n, [&](int k) {
            Band &b = bands[which[k]];
            if (!pool.piece[k].d) return;
            ctx->Unmap(b.stage[stream.set], 0);
            ctx->CopySubresourceRegion(b.plane, 0, 0, (UINT)b.y, 0, b.stage[stream.set], 0, nullptr);
            if ((k & 7) == 7) ctx->Flush();
        });
        if (!mapped) { static int said; if (said++ < 5) logf("video: upload failed"); }
    }
    if (!gsrc) num.copy.add(ms_of(qpc() - t0));
    for (int i = 0; i < band_count; i++) stream.last[i] = gsrc ? 0 : stream.seen & (1u << i) ? stream.hash[i] : piece_hash(data, i);
    num.early.add(100.0 * stream.rows / H);
    convert_rows(slot, stream.rows, H);
    static int check = GetEnvironmentVariableW(L"PKAPP_CHECK", nullptr, 0) ? 1 : 0; // tests: is the streamed picture the same as the ordinary one?
    if (check && streamed && num.sources % 7 == 0)
    {
        static ID3D11Texture2D *st2[2];
        D3D11_TEXTURE2D_DESC td;
        tex[slot]->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING; td.BindFlags = td.MiscFlags = 0; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (auto &t : st2) if (!t) dev->CreateTexture2D(&td, nullptr, &t);
        ctx->CopyResource(st2[0], tex[slot]);
        for (int i = 0; i < band_count; i++)
        {
            Band &b = bands[i];
            D3D11_MAPPED_SUBRESOURCE m{};
            ctx->Map(b.stage[(stream.set + 1) % 3], 0, D3D11_MAP_WRITE, 0, &m);
            CopyPool::run_piece({(uint8_t *)m.pData, data + b.offset, m.RowPitch, b.pitch, b.bytes, b.rows});
            ctx->Unmap(b.stage[(stream.set + 1) % 3], 0);
            ctx->CopySubresourceRegion(b.plane, 0, 0, (UINT)b.y, 0, b.stage[(stream.set + 1) % 3], 0, nullptr);
        }
        convert_rows(SPARE0, 0, H);
        ctx->CopyResource(st2[1], tex[SPARE0]);
        D3D11_MAPPED_SUBRESOURCE x{}, y{};
        ctx->Map(st2[0], 0, D3D11_MAP_READ, 0, &x);
        ctx->Map(st2[1], 0, D3D11_MAP_READ, 0, &y);
        int bad = 0, first = -1;
        for (int k = 0; k < H; k++) if (memcmp((char *)x.pData + x.RowPitch * k, (char *)y.pData + y.RowPitch * k, (size_t)W * 4)) { if (first < 0) first = k; bad++; }
        ctx->Unmap(st2[0], 0);
        ctx->Unmap(st2[1], 0);
        logf("check rows different %d first %d (early rows %d)", bad, first, stream.rows);
    }
    static bool aside_cut; // cut_next only because a picture was shown aside: the scene is the same
    nr_apply(slot, cur < 0 || (cut_next && !aside_cut) || (tune.cut && sample_difference(sample_last, sample_of[cur]) > tune.cut));
    ctx->End(aside ? spare_query : query[slot]);
    if (fence && !aside) ctx4->Signal(fence, fence_of[slot] = ++fence_next);
    ctx->Flush();
    if (aside)
    {
        ReleaseSRWLockExclusive(&gl.l);
        num.skipped = num.skipped + 1;
        behind++;
        LONGLONG due = behind > 2 ? 0 : arrival + ticks_of(lead_ms + src_ms * 0.5);
        if (wait_query(spare_query)) post(slot, 0, std::max(due, qpc()), arrival, ++next_id * 2);
        cut_next = aside_cut = true; // the pair is broken: the next one starts fresh
        return;
    }
    // The engine starts now; HUD counting and the like are only needed when the made picture is finished (gl is held until they are sent).
    int older = cur;
    bool fresh = cut_next;
    int cut = 0, level = tune.cut;
    sample_of[slot] = sample_last;
    if (cur >= 0 && level)
    {
        // a cut, or the camera jumping somewhere else: much more change than in the pictures before
        double &usual = usual_change, d = sample_difference(sample_of[slot], sample_of[cur]);
        cut = d > level || d > 2.2 * std::max(usual, 8.0);
        usual = 0.9 * usual + 0.1 * std::min(d, 2 * usual + 1);
    }
    if (cut) num.cuts = num.cuts + 1;
    if (cut_next) { cut = 1; cut_next = aside_cut = false; }
    prev = cur;
    cur = slot;
    ids[slot] = ++next_id;
    bool gen = tune.on && prev >= 0 && !cut && num.engine_state == 2 && engine_w == W && engine_h == H;
    if (mine)
    {
        behind = 0;
        measure(older, slot, fresh);
        ahead.arrival = arrival;
        ahead.id = ids[slot];
        ahead.fresh = fresh;
        gap_before = gap;
        bool taken = early_end(cut ? EARLY_ORIGINAL : EARLY_COMPLETE);
        uint64_t order = ids[slot] * 2;
        ReleaseSRWLockExclusive(&gl.l);
        if (!taken && wait_converted(slot)) post(slot, 0, qpc(), arrival, order); // the engine had given up waiting
        return;
    }
    if (busy)
    {
        num.skipped = num.skipped + 1;
        behind++;
        LONGLONG due = std::max(qpc(), behind > 2 ? 0 : arrival + ticks_of(tune.on ? lead_ms + src_ms * 0.5 : 0));
        measure(older, slot, fresh);
        uint64_t order = ids[slot] * 2;
        ReleaseSRWLockExclusive(&gl.l);
        if (wait_converted(slot)) post(slot, 0, std::max(due, qpc()), arrival, order);
        return;
    }
    behind = 0;
    const ModelInfo *info = model_by_id(engine_model);
    int m = 2;
    fill_mult(m, info);
    if (m > 2 && !engine_multi) m = 2;
    static int probe;
    if (num.too_slow && engine_family() != FAMILY_EXTRA) { if (++probe % 120) gen = false; }   // the originals at their own rate; a try every two seconds
    bool late = !gen || engine_family() == FAMILY_EXTRA; // the original goes straight to the screen: nothing may be in its way
    job = {prev, cur, next_out, next_set, m, prev >= 0 ? ids[prev] : 0, ids[cur], arrival, gen, late, fresh, gap > 3 * src_ms ? 0.0 /* something moved after a rest (a menu): no guess */ : gap > 0 ? std::min(1.5, src_ms / gap) : 1.0, gap > 0 && gap_before > 0 ? std::min(3.0, std::max(0.33, gap / gap_before)) : 1.0, false, 0};
    gap_before = gap;
    next_out ^= 1;
    next_set ^= 1;
    InterlockedExchange64(&busy_since, qpc());
    InterlockedExchange(&busy, 1);
    SetEvent(job_wake);
    if (!late) measure(older, slot, fresh);
    ReleaseSRWLockExclusive(&gl.l);
}

void (*video_hook)(int, int, int);
int video_tune;
// ---- benchmark samples ----
enum { BENCH_MAX = 1024 };
static struct { float ready, gen, made; } bench_sample[BENCH_MAX];
static volatile LONG bench_on, bench_n;
void video_bench(bool on) { if (on) InterlockedExchange(&bench_n, 0); InterlockedExchange(&bench_on, on); }
void video_bench_read(std::vector<float> &ready, std::vector<float> &gen, std::vector<float> &made)
{
    for (LONG i = 0; i < bench_n && i < BENCH_MAX; i++)
    {
        ready.push_back(bench_sample[i].ready);
        gen.push_back(bench_sample[i].gen);
        made.push_back(bench_sample[i].made);
    }
}
IDXGIAdapter1 *video_adapter() { return adapter; }
static volatile LONG present_lost;
bool video_lost() { return present_lost || (dev && dev->GetDeviceRemovedReason() != S_OK); }
void video_test_lost() { InterlockedExchange(&present_lost, 1); }
double video_stuck_seconds() { return busy && made_with_engine > 5 ? ms_of(qpc() - busy_since) / 1000.0 : 0; }
void video_engine_restart()
{
    InterlockedExchange(&engine_broken, 1);
    SetEvent(setup_wake);
}

// ---- worker ----
static bool compose(int target, int made, int older, int original)
{
    bool hud = tune.hud && hud_sources >= 31, text = tune.text && text_pairs >= 1;
    bool fix = repair_on() && extra_both_ready();
    if (!hud && !text && !fix) return false;
    AcquireSRWLockExclusive(&gl.l);
    D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
    ID3D11ShaderResourceView *fm = nullptr, *fw = nullptr;
    if (fix) { extra_repair(ctx, srvTex[older], srvTex[original], srvTex[made]); extra_repair_views(&fm, &fw); }
    ID3D11ShaderResourceView *s[8] = {srvTex[made], srvTex[original], s_map, t_bits[text_turn].s, nullptr, nullptr, srvTex[older], t_block.s}, *none[10] = {}, *s2[2] = {fm, fw};
    struct { UINT hud, text, fix, pad; float w, h, pad2[2]; } flags = {hud, text, fix, 0, (float)W, (float)H, {0, 0}};
    ID3D11Buffer *cb[2] = {cb_text, cb_flags};
    ctx->UpdateSubresource(cb_flags, 0, nullptr, &flags, 0, 0);
    ctx->PSSetConstantBuffers(1, 2, cb);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_hud, nullptr, 0);
    ctx->PSSetShader(ps_compose, nullptr, 0);
    ctx->PSSetShaderResources(2, 8, s);
    ctx->PSSetShaderResources(10, 2, s2);
    ctx->PSSetSamplers(0, 1, &linear);
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetRenderTargets(1, &rtv[target], nullptr);
    ctx->Draw(3, 0);
    ID3D11RenderTargetView *nort = nullptr;
    ctx->OMSetRenderTargets(1, &nort, nullptr);
    ctx->PSSetShaderResources(2, 10, none);
    ctx->End(hud_query);
    ctx->Flush();
    ReleaseSRWLockExclusive(&gl.l);
    return wait_query(hud_query);
}
static bool copy_texture(int target, int from)
{
    AcquireSRWLockExclusive(&gl.l);
    ctx->CopyResource(tex[target], tex[from]);
    ctx->End(hud_query);
    ctx->Flush();
    ReleaseSRWLockExclusive(&gl.l);
    return wait_query(hud_query);
}
static void lead(double made)
{
    lead_ms = 0.9 * lead_ms + 0.1 * (made + tune.margin_x10 / 10.0);
    lead_ms = std::min(std::max(lead_ms, tune.early > 1 ? -6.0 : tune.early ? 0.3 : 1.0), std::max(10.0, 0.8 * src_ms));
}
// How long a made picture stays before the original follows. With variable refresh or sync off, 20% is enough in 30 fps games
// as long as that is at least one refresh.
static double share_ms()
{
    double base = src_ms * 0.01 * tune.share, hz_ms = num.hz_mhz > 0 ? 1000000.0 / num.hz_mhz : 0;
    // V-Sync: the two pictures wait in line, the original takes the refresh right after the made one
    if ((!tune.sync || !num.tearing) && hz_ms > 0 && src_ms > 2.2 * hz_ms) return tune.queue > 1 ? 0.2 : 1.1 * hz_ms; // a queue of one: each waits for its own refresh
    // the monitor follows the pictures: two pictures closer than a refresh would leave part of the first one on the screen
    if (num.vrr == 2 && hz_ms > 0 && base < hz_ms + 0.1 && src_ms > 1.9 * hz_ms) return std::min(hz_ms + 0.1, src_ms * 0.5);
    if (tune.share <= 25 && src_ms > 25 && tune.sync && num.tearing && num.hz_mhz > 0)
    {
        double refresh = 1000000.0 / num.hz_mhz, low = std::max(src_ms * 0.19, refresh + 0.1);
        if (low < base) base = low;
    }
    return base;
}
// A model that cannot keep up only costs frame rate: after 30 late pictures (and no smaller RIFE motion size left) the originals are
// shown alone until a try is fast enough.
static void speed_check(double took)
{
    bool smaller = engine.can_flow() && !tune.flow && flow_level && flow_level < ((int64_t)W * H >= 2560LL * 1440 ? 3 : 2);
    bool extra = engine_family() == FAMILY_EXTRA; // due before half the interval is over
    double limit = extra ? 0.55 * src_ms : 1.1 * src_ms;
    if (took > limit) slow_count = slow_count + 1;
    else slow_count = 0;
    if (extra && extra_lines > 200 && slow_count >= 20) // a smaller search first
    {
        extra_lines = extra_lines == 360 ? 270 : 180;
        slow_count = 0;
        logf("extra: motion at %d lines", extra_lines);
    }
    if (num.too_slow ? took < (extra ? 0.5 : 0.95) * src_ms : slow_count >= (smaller ? 900 : 30))
    {
        InterlockedExchange(&num.too_slow, !num.too_slow);
        slow_count = 0;
        logf("engine: %s (%.1f ms for %.1f)", num.too_slow ? "too slow, originals only" : "fast enough again", took, src_ms);
    }
}
static void flow_check(double took)
{
    speed_check(took);
    if (!engine.can_flow() || !flow_level) return;
    bool large = (int64_t)W * H >= 2560LL * 1440;
    int user = tune.flow;
    auto set = [&](int level) {
        if (level == 3 && !large) level = 2;
        if (!engine.set_flow(level)) { logf("engine: flow size failed %s", engine.error()); num.engine_state = -1; return; }
        flow_level = level;
        flow_n = 0;
        num.flow = level;
        logf("engine: RIFE motion size 1/%d", level == 1 ? 2 : level == 2 ? 4 : 8);
    };
    wchar_t key[40];
    swprintf(key, 40, L"rife_auto_%d_%d", engine_model, H);
    if (user != flow_user) { flow_user = user; set(user ? user : std::max(flow_base(engine_model), ini_int(key, 1))); return; }
    if (user || ++flow_n <= 5) return;
    if (flow_n == 6) { flow_ms = took; flow_since = GetTickCount64(); }
    else flow_ms = 0.95 * flow_ms + 0.05 * took;
    ULONGLONG held = GetTickCount64() - flow_since;
    if (flow_ms > 0.85 * src_ms && held > (flow_ms > 1.4 * src_ms ? 5000u : 10000u) && flow_level < (large ? 3 : 2)) { set(flow_level + 1); ini_set(key, flow_level); }
    else if (flow_level > flow_base(engine_model) && !flow_raised && held > 15000 && 3.2 * flow_ms < 0.6 * src_ms) { flow_raised = true; set(flow_level - 1); ini_set(key, flow_level); }
}
static int engine_call(int a, uint64_t ida, int b, uint64_t idb, int out, float t)
{
    bool fail = false;
    guard_fault(engine_model, fail);
    return fail ? -1 : engine.generate(a, ida, b, idb, out, t);
}
static void measure_late(Job &j) // after the original was presented
{
    if (!j.late) return;
    j.late = false;
    WaitForSingleObject(shown_event, 3);
    wait_until(qpc() + ticks_of(0.8)); // and is on its way to the screen
    AcquireSRWLockExclusive(&gl.l);
    measure(j.a, j.b, j.fresh);
    ReleaseSRWLockExclusive(&gl.l);
}
// The engine was faster than the cable: waits for the last lines. What became of the picture.
static LONG early_wait(Job &j)
{
    if (!j.early) return EARLY_COMPLETE;
    if (ahead.state == EARLY_WAITING) WaitForSingleObject(ahead.full, 12);
    LONG s = InterlockedCompareExchange(&ahead.state, EARLY_NOTHING, EARLY_WAITING); // nothing came in time
    if (s == EARLY_WAITING) s = EARLY_NOTHING;
    if (s == EARLY_COMPLETE || s == EARLY_ORIGINAL)
    {
        j.arrival = ahead.arrival;
        j.seen = j.idb; // the engine knows it, with guessed lines, by this number
        j.idb = ahead.id;
        j.fresh = ahead.fresh;
        j.early = false;
        if (!wait_converted(j.b)) s = EARLY_NOTHING;
        j.early = true;
    }
    else { static int said; if (said++ < 20) logf("video: started early for nothing"); }
    InterlockedExchange(&ahead.state, EARLY_NONE);
    return s;
}
// What the next original will probably look like (only its lowest lines are ever used, see early_start).
static void predict(Job &j)
{
    if (!tune.early || nr_wanted() || repair_on() || !pred || bench_on || engine_family() == FAMILY_EXTRA || !extra_prepared() || num.generate.get() > 0.5 * src_ms) { pred_of = 0; return; }
    wait_until(qpc() + ticks_of(0.4)); // the made picture is on its way to the screen
    // These models continue from the picture they saw last: they get the finished one, or they would start afresh with every picture (and
    // only mix the two originals then).
    if (j.seen && (engine_family() == FAMILY_FSR3 || engine_family() == FAMILY_AMDFG)) engine.generate(j.b, j.seen, j.b, j.idb, OUT0 + (j.out ^ 1), 0.5f);
    AcquireSRWLockExclusive(&gl.l);
    if (extra_size(dev, ctx, W, H, extra_lines))
    {
        if (fed_of != j.ida || j.fresh) extra_forget();
        extra_feed(ctx, srvTex[j.b], false);
        fed_of = j.idb;
        if (extra_ready())
        {
            ExtraProtect p = {s_map, t_bits[text_turn].s, t_block.s, tune.hud && hud_sources >= 31, tune.text && text_pairs >= 1, text_dh && H >= 2000 ? 2u : 1u, (unsigned)text_dw / 4, (unsigned)text_dh / 4, (unsigned)text_dw, (unsigned)text_dh};
            extra_draw(ctx, pred_rtv, srvTex[j.a], srvTex[j.b], 1.f, 1.f, p);
            pred_of = j.idb;
        }
        ctx->Flush();
    }
    ReleaseSRWLockExclusive(&gl.l);
}
static DWORD WINAPI worker_main(void *)
{
    DWORD k = 0;
    AvSetMmThreadCharacteristicsW(L"Games", &k);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    while (WaitForSingleObject(job_wake, INFINITE) == WAIT_OBJECT_0 && !stop_all)
    {
        if (!busy) continue;
        Job j = job;
        bool ready = (!j.gen || wait_converted(j.a)) && wait_converted(j.b, j.fence);
        double ready_ms = ms_of(qpc() - j.arrival);
        if (!j.early) num.ready.add(ready_ms);
        num.ahead.add(j.early ? 100 : 0);
        bool made = false, tried = false;
        if (ready && j.gen && engine_family() == FAMILY_EXTRA)
        {
            // extrapolation: the original goes out now, the guessed pictures follow it in even steps
            LONGLONG t0 = qpc();
            ResetEvent(shown_event);
            post(j.b, 0, t0, j.arrival, j.idb * 2);
            last_due = t0;
            measure_late(j);
            for (int i = 1; i < j.m && extra_ready(); i++)
            {
                int target = j.m > 2 ? MULTI0 + 3 * j.set + i - 1 : OUT0 + j.out;
                AcquireSRWLockExclusive(&gl.l);
                ExtraProtect p = {s_map, t_bits[text_turn].s, t_block.s, tune.hud && hud_sources >= 31, tune.text && text_pairs >= 1, text_dh && H >= 2000 ? 2u : 1u, (unsigned)text_dw / 4, (unsigned)text_dh / 4, (unsigned)text_dw, (unsigned)text_dh};
                extra_draw(ctx, rtv[target], srvTex[j.a], srvTex[j.b], (float)(j.rate * i / j.m), (float)j.past, p);
                ctx->End(hud_query);
                ctx->Flush();
                ReleaseSRWLockExclusive(&gl.l);
                if (!wait_query(hud_query)) break;
                if (video_hook) video_hook(target, j.a, j.b);
                if (i == 1) { num.generate.add(ms_of(qpc() - t0)); num.made.add(ms_of(qpc() - j.arrival)); }
                if (i == 1 && bench_on && bench_n < BENCH_MAX) { bench_sample[bench_n] = {(float)ready_ms, (float)ms_of(qpc() - t0), (float)ms_of(qpc() - j.arrival)}; InterlockedIncrement(&bench_n); }
                post(nr_finish(target, j.b, j.b, 1), 1, t0 + ticks_of(src_ms * i / j.m), j.arrival, j.idb * 2 + 1);
                made = true;
            }
            num.share.clear();
            num.hud.clear();
            if (made) speed_check(ms_of(qpc() - j.arrival));
            num.made_count = num.made_count + (made ? 1 : 0);
            made_with_engine = made_with_engine + 1;
            InterlockedExchange(&busy, 0);
            continue;
        }
        if (ready && j.gen && TryAcquireSRWLockExclusive(&engine_lock.l))
        {
            if (num.engine_state == 2)
            {
                tried = true;
                LONGLONG t0 = qpc(), first = 0, step = 0;
                if (j.m > 2)
                {
                    int base = MULTI0 + 3 * j.set, n = 0;
                    bool skip = false;
                    for (int i = 1; i < j.m; i++)
                    {
                        if (engine_call(j.a, j.ida, j.b, j.idb, OUT0 + j.out, (float)i / (float)j.m)) break;
                        if (!compose(base + i - 1, OUT0 + j.out, j.a, j.b) && !copy_texture(base + i - 1, OUT0 + j.out)) break;
                        n = i;
                        int show = nr_finish(base + i - 1, j.a, j.b, (float)i / (float)j.m);
                        LONGLONG now = qpc();
                        if (i == 1)
                        {
                            lead(ms_of(now - j.arrival));
                            LONGLONG want = j.arrival + ticks_of(lead_ms);
                            first = std::max(want, now);
                            if (last_due && first < last_due + ticks_of(src_ms * 0.2)) first = last_due + ticks_of(src_ms * 0.2);
                            double each = src_ms / j.m * tune.share / 50.0, hz_ms = num.hz_mhz > 0 ? 1000000.0 / num.hz_mhz : 0;
                            if (num.vrr == 2 && hz_ms > 0 && each < hz_ms + 0.1 && j.m * (hz_ms + 0.1) < src_ms) each = hz_ms + 0.1; // the monitor follows: whole pictures only
                            step = ticks_of(each);
                            skip = first - want > ticks_of(src_ms * 0.5);
                        }
                        if (!skip) post(show, 1, first + step * (i - 1), j.arrival, j.idb * 2 - 1);
                    }
                    num.generate.add(ms_of(qpc() - t0));
                    if (n == j.m - 1)
                    {
                        made = true;
                        LONGLONG orig = first + step * (j.m - 1);
                        post(j.b, 0, orig, j.arrival, j.idb * 2);
                        last_due = orig;
                        flow_check(ms_of(qpc() - j.arrival));
                    }
                }
                else if (!engine_call(j.a, j.ida, j.b, j.idb, OUT0 + j.out, video_tune > 100 ? video_tune / 100.f : 0.5f))
                {
                    made = true;
                    LONGLONG t1 = qpc();
                    num.generate.add(ms_of(t1 - t0));
                    engine_ms = 0.9 * engine_ms + 0.1 * std::min(50.0, ms_of(t1 - t0));
                    int show = OUT0 + j.out;
                    if (j.early && tune.early > 1) // the made picture does not even wait for the last lines
                    {
                        if (compose(HUD0 + j.out, show, j.a, j.b)) show = HUD0 + j.out;
                        show = nr_finish(show, j.a, j.b, 0.5f);
                        LONGLONG now = qpc();
                        num.hud.add(ms_of(now - t1));
                        num.made.add(ms_of(now - j.arrival));
                        lead(ms_of(now - j.arrival));
                        LONGLONG mid = std::max(j.arrival + ticks_of(lead_ms), now);
                        if (last_due && mid < last_due + ticks_of(src_ms * 0.2)) mid = last_due + ticks_of(src_ms * 0.2);
                        double share = share_ms();
                        num.share.add(share);
                        post(show, 1, mid, j.arrival, j.idb * 2 - 1);
                        LONG how = early_wait(j);
                        if (how == EARLY_COMPLETE || how == EARLY_ORIGINAL)
                        {
                            LONGLONG orig = std::max(qpc(), mid + ticks_of(share));
                            post(j.b, 0, orig, j.arrival, j.idb * 2);
                            last_due = orig;
                            if (how == EARLY_COMPLETE) predict(j);
                        }
                        flow_check(ms_of(t1 - t0) + 0.5);
                        j.early = false; // done
                    }
                    else
                    {
                    LONG how = early_wait(j);
                    if (how == EARLY_COMPLETE) {
                    if (compose(HUD0 + j.out, show, j.a, j.b)) show = HUD0 + j.out;
                    if (video_hook) video_hook(show, j.a, j.b);
                    show = nr_finish(show, j.a, j.b, 0.5f);
                    LONGLONG now = qpc();
                    num.hud.add(ms_of(now - t1));
                    num.made.add(ms_of(now - j.arrival));
                    if (bench_on && bench_n < BENCH_MAX) { bench_sample[bench_n] = {(float)ready_ms, (float)ms_of(t1 - t0), (float)ms_of(now - j.arrival)}; InterlockedIncrement(&bench_n); }
                    lead(ms_of(now - j.arrival));
                    LONGLONG want = j.arrival + ticks_of(lead_ms), mid = std::max(want, now), half = ticks_of(src_ms * 0.5);
                    if (last_due && mid < last_due + ticks_of(src_ms * 0.2)) mid = last_due + ticks_of(src_ms * 0.2);
                    double share = share_ms();
                    num.share.add(share);
                    bool skip = mid - want > half;
                    LONGLONG orig = skip ? std::max(mid, want + half) : mid + ticks_of(share);
                    if (!skip) post(show, 1, mid, j.arrival, j.idb * 2 - 1);
                    post(j.b, 0, orig, j.arrival, j.idb * 2);
                    last_due = orig;
                    flow_check(j.early ? ms_of(qpc() - t0) : ms_of(qpc() - j.arrival));
                    predict(j);
                    }
                    else if (how == EARLY_ORIGINAL) { LONGLONG now = qpc(); post(j.b, 0, now, j.arrival, j.idb * 2); last_due = now; }
                    }
                }
                else
                {
                    static int said;
                    if (said++ < 5) logf("engine: generate failed %s", engine.error());
                }
            }
            ReleaseSRWLockExclusive(&engine_lock.l);
        }
        if (j.early && !made) // the engine failed: the original as soon as it is there
        {
            LONG how = early_wait(j);
            ready = how == EARLY_COMPLETE || how == EARLY_ORIGINAL;
        }
        if (made) { num.made_count = num.made_count + 1; gen_fail = 0; made_with_engine = made_with_engine + 1; }
        else if (tried && ++gen_fail == 3) video_engine_restart(); // not a single slip: open the engine again
        else if (ready)
        {
            LONGLONG now = qpc();
            ResetEvent(shown_event);
            post(j.b, 0, now, j.arrival, j.idb * 2); // generation off, loading, a cut or a failure: the original as it is
            last_due = now;
        }
        if (j.late) measure_late(j);
        if (nr_wanted() && have_nr) // the pictures are on their way: now the network may look at the newest one
        {
            AcquireSRWLockExclusive(&gl.l);
            nr_think(ctx4);
            ctx->Flush();
            ReleaseSRWLockExclusive(&gl.l);
        }
        InterlockedExchange(&busy, 0);
    }
    return 0;
}

// ---- setup thread ----
std::string video_engine_error()
{
    AcquireSRWLockShared(&reason_lock.l);
    std::string s = engine_reason;
    ReleaseSRWLockShared(&reason_lock.l);
    return s;
}
static int monitor_mhz();
static DWORD WINAPI setup_main(void *)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (dev) extra_init(dev); // not while a picture waits
    int failed_model = -1, failed_h = 0;
    while (!stop_all)
    {
        WaitForSingleObject(setup_wake, 300);
        if (stop_all) break;
        static ULONGLONG vrr_checked;
        if (GetTickCount64() - vrr_checked > 3000) { vrr_checked = GetTickCount64(); num.vrr = vrr_state(window); if (num.hz_mhz) num.hz_mhz = monitor_mhz(); LONG was = num.hdr; static int pretend = GetEnvironmentVariableW(L"PKAPP_HDR", nullptr, 0) ? 2 : 0; /* tests: only this window's own output */ num.hdr = pretend ? pretend : hdr_state(window); if (num.hdr == 2 && (was != 2 || !num.hdr_peak)) num.hdr_peak = (LONG)std::min(4000.f, std::max(0.f, hdr_peak(window))); }
        int want = tune.model, w = W, h = H;
        if (w <= 0 || !tex[0]) continue;
        { // the trial of DLSS 5
            static int nw, nh, ns = -1, nl = -1, nk = -1, nc = -1, again;
            static bool was, trying;
            bool asked = tune.nr && !bench_running;
            int lines = tune.nr_size == 0 ? 720 : tune.nr_size == 1 ? 1080 : 4320;
            bool other = nw != w || nh != h || ns != tune.nr_style || nl != lines || nk != nr_step_of().layers || nc != nr_choice();
            if (!asked || other) { InterlockedExchange(&nr_bad, 0); again = 0; }
            if (trying && (!asked || nr_good > 60)) { trying = false; ini_set(L"nr_trying", 0); } // it works
            bool on = nr_wanted();
            if (!on && was)
            {
                nr_close();
                was = false;
                nw = 0;
                AcquireSRWLockExclusive(&gl.l);
                have_nr = false;
                free_textures(NRO0, TEXTURES);
                ReleaseSRWLockExclusive(&gl.l);
            }
            if (on && !have_nr)
            {
                AcquireSRWLockExclusive(&gl.l);
                if (w == W && h == H && tex[0]) { have_nr = make_textures(NRO0, TEXTURES); if (!have_nr) free_textures(NRO0, TEXTURES); }
                ReleaseSRWLockExclusive(&gl.l);
            }
            if (on && have_nr && (!was || other || !nr_ready(w, h)))
            {
                if (was && !other && nr_good < 300) again++; // stopped soon after it was opened
                else again = 0;
                was = true;
                nw = w;
                nh = h;
                ns = tune.nr_style;
                nl = lines;
                nk = nr_step_of().layers;
                nc = nr_choice();
                InterlockedExchange(&nr_good, 0);
                trying = true;
                ini_set(L"nr_trying", 1); // if the program does not survive this, it is off at the next start
                std::string why = fence ? nr_open(dev, adapter, w, h, lines, ns, nk) : "Direct3D 11.4";
                if (why.empty() && again > 2) { why = "it stops again and again"; nr_close(); }
                if (!why.empty())
                {
                    trying = false;
                    ini_set(L"nr_trying", 0);
                    InterlockedExchange(&nr_bad, 1);
                }
                AcquireSRWLockExclusive(&reason_lock.l);
                nr_reason = why;
                ReleaseSRWLockExclusive(&reason_lock.l);
            }
        }
        const ModelInfo *info = model_by_id(want);
        int m = 2;
        fill_mult(m, info);
        bool multi = m > 2;
        static ULONGLONG restarted;
        if (InterlockedExchange(&engine_broken, 0) && num.engine_state == 2 && engine_model == want)
        {
            if (restarted && GetTickCount64() - restarted < 60000) guard_restart("the engine failed again");
            restarted = GetTickCount64();
            logf("engine: %ls stopped (%s), opening it again", info->name, engine.error());
            guard_say(std::wstring(info->name) + tr(L": 생성이 멈춰서 다시 켰어요", L": generation stopped and was restarted"));
            InterlockedExchange(&num.engine_state, 1);
            gen_fail = 0;
            engine_model = -1;
        }
        bool fine = info->family == FAMILY_FSR3 && h >= 2000 && tune.early < 2; // not where every millisecond counts
        if (engine_model == want && engine_w == w && engine_h == h && (!multi || engine_multi) && engine_fine == fine && num.engine_state == 2) continue;
        if (num.engine_state == -1 && failed_model == want && failed_h == h) continue;
        InterlockedExchange(&num.engine_state, 1);
        AcquireSRWLockExclusive(&engine_lock.l);
        engine.close();
        if (multi && !have_multi)
        {
            AcquireSRWLockExclusive(&gl.l);
            have_multi = make_textures(MULTI0, NRO0);
            if (!have_multi) { free_textures(MULTI0, NRO0); InterlockedExchange(&tune.mult, 2); } // no memory for them
            ReleaseSRWLockExclusive(&gl.l);
        }
        wchar_t key[40];
        swprintf(key, 40, L"rife_auto_%d_%d", want, h);
        flow_user = tune.flow;
        flow_level = info->family == FAMILY_RIFE && (int64_t)w * h >= 1920LL * 1080 ? (flow_user ? flow_user : std::max(flow_base(want), ini_int(key, 1))) : 0;
        flow_n = 0;
        flow_raised = false;
        std::string why = engine.open(*info, w, h, info->family == FAMILY_FSR3 ? fine : flow_level);
        engine_fine = fine;
        if (why.empty() && info->family != FAMILY_EXTRA)
        {
            DXGI_ADAPTER_DESC1 d;
            adapter->GetDesc1(&d);
            if (memcmp(&d.AdapterLuid, engine.luid(), 8)) why = "engine uses another graphics card";
            for (int i = 0; why.empty() && i < 5; i++)
                if (!engine.import(nt[i], i)) why = std::string("import: ") + engine.error();
            if (!why.empty()) engine.close();
        }
        if (!why.empty() && info->family == FAMILY_AMDFG)
        {
            HMODULE rt = LoadLibraryExW(L"vcruntime140_1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!rt) why = "needs the Microsoft Visual C++ 2015-2022 runtime (x64)";
            else FreeLibrary(rt);
        }
        bool ok = why.empty();
        wchar_t worked[24];
        swprintf(worked, 24, L"worked_%d_%d", want, h);
        if (ok) ini_set(worked, 1);
        static int tries;
        if (ok) tries = 0;
        else if (bench_running) {} // the benchmark notes it and goes on
        else if (ini_int(worked, 0) && ++tries <= 2) // it has worked on this computer before: try again, then start again
        {
            logf("engine: %ls failed: %s, trying again", info->name, why.c_str());
            InterlockedExchange(&num.engine_state, 1);
            engine_model = -1;
            ReleaseSRWLockExclusive(&engine_lock.l);
            Sleep(1500);
            continue;
        }
        else if (guard_recovered() && guard_next_model(want) >= 0) // the replacement does not run here: the next one
        {
            int n = guard_next_model(want);
            logf("engine: %ls failed: %s, using %ls", info->name, why.c_str(), model_by_id(n)->name);
            guard_say(std::wstring(model_by_id(n)->name) + tr(L"(으)로 바꿨어요", L" is used now"));
            tune.model = n;
            ini_set(L"model", n);
            InterlockedExchange(&num.engine_state, 1);
            engine_model = -1;
            ReleaseSRWLockExclusive(&engine_lock.l);
            continue;
        }
        else if (ini_int(worked, 0)) guard_restart("the engine cannot be opened any more");
        engine_model = ok ? want : -1;
        engine_w = w;
        engine_h = h;
        engine_multi = have_multi;
        failed_model = ok ? -1 : want;
        failed_h = h;
        num.flow = ok ? flow_level : 0;
        num.engine_model = engine_model;
        num.engine_w = w;
        num.engine_h = h;
        num.generate.clear();
        num.made.clear();
        lead_ms = 4;
        extra_lines = 360;
        AcquireSRWLockExclusive(&reason_lock.l);
        engine_reason = why;
        ReleaseSRWLockExclusive(&reason_lock.l);
        InterlockedExchange(&made_with_engine, 0);
        InterlockedExchange(&num.too_slow, 0);
        slow_count = 0;
        InterlockedExchange(&num.engine_state, ok ? 2 : -1);
        ReleaseSRWLockExclusive(&engine_lock.l);
        if (!ok) logf("engine: %ls failed: %s", info->name, why.c_str());
    }
    AcquireSRWLockExclusive(&engine_lock.l);
    engine.close();
    ReleaseSRWLockExclusive(&engine_lock.l);
    return 0;
}

// ---- on-screen label (built by the window thread, drawn by the presenter) ----
static Lock label_lock;
static ID3D11Device *present_dev;      // the presenter's device; textures may be made on any thread
static ID3D11ShaderResourceView *label_srv; // made by the window thread, so the presenter never waits for it
static int label_w, label_h;
void video_set_label(const std::wstring &text, int px)
{
    static std::wstring last;
    static int last_px, last_corner = -1;
    if (text == last && px == last_px && last_corner == tune.corner) return;
    last = text;
    last_px = px;
    last_corner = tune.corner;
    if (text.empty())
    {
        AcquireSRWLockExclusive(&label_lock.l);
        label_w = label_h = 0;
        release(label_srv);
        ReleaseSRWLockExclusive(&label_lock.l);
        return;
    }
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    HFONT font = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Malgun Gothic");
    HGDIOBJ of = SelectObject(mem, font);
    RECT r = {0, 0, 4000, 0};
    DrawTextW(mem, text.c_str(), -1, &r, DT_CALCRECT | DT_NOPREFIX);
    int edge = std::max(1, px / 14), pad = edge + 1, w = r.right + 2 * pad, h = r.bottom + 2 * pad;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    uint32_t *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&bits, nullptr, 0);
    if (bmp && bits)
    {
        HGDIOBJ ob = SelectObject(mem, bmp);
        memset(bits, 0, (size_t)w * h * 4);
        SetBkMode(mem, TRANSPARENT);
        SetTextColor(mem, RGB(255, 255, 255));
        RECT t = {pad, pad, w - pad, h};
        DrawTextW(mem, text.c_str(), -1, &t, ((tune.corner & 1) ? DT_RIGHT : DT_LEFT) | DT_NOPREFIX);
        GdiFlush();
        std::vector<uint8_t> texels((size_t)w * h * 2); // R the letters, G a thin dark line around them: readable on any picture, nothing behind
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                unsigned most = 0;
                for (int b = std::max(0, y - edge); b <= std::min(h - 1, y + edge) && most < 255; b++)
                    for (int a = std::max(0, x - edge); a <= std::min(w - 1, x + edge); a++)
                        if ((a - x) * (a - x) + (b - y) * (b - y) <= edge * edge + 1) most = std::max(most, (bits[(size_t)b * w + a] >> 8) & 255);
                size_t i = (size_t)y * w + x;
                texels[i * 2] = (uint8_t)((bits[i] >> 8) & 255);
                texels[i * 2 + 1] = (uint8_t)most;
            }
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)w;
        td.Height = (UINT)h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sdt = {texels.data(), (UINT)w * 2, 0};
        ID3D11Texture2D *lt = nullptr;
        ID3D11ShaderResourceView *made = nullptr, *old = nullptr;
        if (present_dev && SUCCEEDED(present_dev->CreateTexture2D(&td, &sdt, &lt))) present_dev->CreateShaderResourceView(lt, nullptr, &made);
        release(lt);
        if (made)
        {
            AcquireSRWLockExclusive(&label_lock.l);
            old = label_srv;
            label_srv = made;
            label_w = w;
            label_h = h;
            ReleaseSRWLockExclusive(&label_lock.l);
            release(old);
        }
        else last.clear();
        SelectObject(mem, ob);
    }
    if (bmp) DeleteObject(bmp);
    SelectObject(mem, of);
    DeleteObject(font);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    InterlockedExchange(&redraw, 1);
    SetEvent(item_wake);
}

// ---- presenter ----
static volatile LONG resized = 1;
void video_resized()
{
    InterlockedExchange(&resized, 1);
    SetEvent(item_wake);
}
static void note_presented(const Item &it, LONGLONG now)
{
    static LONGLONG last;
    static int last_made;
    if (last)
    {
        double us = ms_of(now - last) * 1000.0;
        if (us < 1e6)
        {
            LONG at = InterlockedIncrement(&num.ring_next) - 1;
            InterlockedExchange(&num.ring[(unsigned)at % 256], (LONG)us | (last_made ? RING_MADE : 0));
        }
    }
    last = now;
    last_made = it.made;
    num.shown = num.shown + 1;
    if (it.made) return;
    double ms = ms_of(now - it.arrival);
    if ((!tune.sync || !num.tearing) && num.hz_mhz > 0) ms += (num.share.get() > 0 && num.share.get() < 1 ? 1.5 : 0.5) * 1000000.0 / num.hz_mhz; // V-Sync: waits for its refresh
    num.latency.add(ms);
    if ((LONG)(ms * 1000) > num.lat_peak) num.lat_peak = (LONG)(ms * 1000);
}
static int monitor_mhz()
{
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof mi;
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi) || !EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;
    num.monitor_h = (LONG)std::min(dm.dmPelsHeight, dm.dmPelsWidth);
    int hz = (int)dm.dmDisplayFrequency;
    if ((hz + 1) % 60 == 0) hz++;
    return hz * 1000;
}
static DWORD WINAPI present_main(void *)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD k = 0;
    AvSetMmThreadCharacteristicsW(L"Games", &k);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    ID3D11Device *d = nullptr;
    ID3D11Device1 *d1 = nullptr;
    ID3D11DeviceContext *c = nullptr;
    IDXGIFactory2 *factory = nullptr;
    IDXGISwapChain1 *chain = nullptr;
    IDXGISwapChainMedia *media = nullptr;
    ULONGLONG media_checked = 0;
    ID3D11RenderTargetView *back = nullptr;
    ID3D11ShaderResourceView *srv[TEXTURES]{};
    ID3D11BlendState *blend = nullptr;
    ID3D11SamplerState *smp = nullptr;
    ID3D11Buffer *cb = nullptr;
    LONG generation = -1;
    int bw = 0, bh = 0, last_tex = -1, last_made = 0, sharp_level = -1;
    BOOL tearing = FALSE;
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, nullptr, &c))) { logf("present: device failed"); return 1; }
    d->QueryInterface(IID_PPV_ARGS(&d1));
    present_dev = d;
    IDXGIDevice1 *dd = nullptr;
    LONG queue_now = 0;
    if (SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&dd)))) dd->SetGPUThreadPriority(7);
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    IDXGIFactory5 *f5 = nullptr;
    if (factory && SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&f5)))) { if (FAILED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof tearing))) tearing = FALSE; release(f5); }
    ID3D11VertexShader *vs = make_vs(d, vs_src);
    ID3D11PixelShader *show = make_ps(d, (std::string(out_src) + ps_show_src).c_str()), *sharp = make_ps(d, (std::string(out_src) + ps_sharp_src).c_str()), *label = make_ps(d, (std::string(out_src) + ps_label_src).c_str());
    ID3D11Buffer *cb_out = nullptr;
    bool hdr_now = false;
    float out_now[4] = {-1};
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    d->CreateSamplerState(&sd, &smp);
    D3D11_BUFFER_DESC ob{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    d->CreateBuffer(&ob, nullptr, &cb_out);
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO; // must be a valid value: without it there is no blending at all
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(d->CreateBlendState(&bd, &blend))) logf("present: no blending");
    D3D11_BUFFER_DESC cbd{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    d->CreateBuffer(&cbd, nullptr, &cb);
    num.tearing = tearing;
    auto draw = [&](int t, int made) {
        float black[4] = {0, 0, 0, 1};
        c->ClearRenderTargetView(back, black);
        c->OMSetRenderTargets(1, &back, nullptr);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(vs, nullptr, 0);
        c->PSSetSamplers(0, 1, &smp);
        float peak = num.hdr_peak > 0 ? (float)num.hdr_peak : 600.f, white = (float)tune.hdr_white, want_out[4] = {hdr_now ? 1.f : 0.f, white, white + std::max(0.f, peak - white) * (float)tune.hdr_boost / 100.f, pq_input ? 1.f : 0.f};
        if (memcmp(want_out, out_now, sizeof out_now)) { memcpy(out_now, want_out, sizeof out_now); c->UpdateSubresource(cb_out, 0, nullptr, out_now, 0, 0); }
        c->PSSetConstantBuffers(1, 1, &cb_out);
        if (t >= 0 && srv[t] && W > 0 && H > 0)
        {
            float x = 0, y = 0, w = (float)bw, h = (float)bh;
            if (!tune.fit)
            {
                if ((int64_t)bw * H > (int64_t)bh * W) { w = (float)bh * W / H; x = std::floor(((float)bw - w) / 2); }
                else { h = (float)bw * H / W; y = std::floor(((float)bh - h) / 2); }
            }
            D3D11_VIEWPORT v = {x, y, w, h, 0, 1};
            c->RSSetViewports(1, &v);
            int level = made && tune.sharpen_made && !tune.sharpen ? 75 : (int)tune.sharpen;
            if (level && sharp)
            {
                if (level != sharp_level) { float kk[4] = {0.2f * (float)level / 100.f, 0, 0, 0}; c->UpdateSubresource(cb, 0, nullptr, kk, 0, 0); sharp_level = level; }
                c->PSSetConstantBuffers(0, 1, &cb);
                c->PSSetShader(sharp, nullptr, 0);
            }
            else c->PSSetShader(show, nullptr, 0);
            c->PSSetShaderResources(0, 1, &srv[t]);
            c->Draw(3, 0);
        }
        AcquireSRWLockShared(&label_lock.l);
        int lw = label_w, lh = label_h;
        if (label_srv)
        {
            float mx = (float)bh / 90, x = (tune.corner & 1) ? (float)bw - lw - mx : mx, y = (tune.corner & 2) ? (float)bh - lh - mx : mx, bf[4] = {};
            D3D11_VIEWPORT v = {x, y, (float)lw, (float)lh, 0, 1};
            c->RSSetViewports(1, &v);
            c->OMSetBlendState(blend, bf, 0xffffffff);
            c->PSSetShader(label, nullptr, 0);
            c->PSSetShaderResources(0, 1, &label_srv);
            c->Draw(3, 0);
            c->OMSetBlendState(nullptr, bf, 0xffffffff);
        }
        ReleaseSRWLockShared(&label_lock.l);
        ID3D11ShaderResourceView *none = nullptr;
        c->PSSetShaderResources(0, 1, &none);
    };
    auto present = [&] {
        LONG queue = tune.sync && tearing ? 2 : tune.queue; // without V-Sync nothing waits
        if (queue_now != queue && dd) { queue_now = queue; dd->SetMaximumFrameLatency((UINT)std::min<LONG>(3, std::max<LONG>(1, queue_now))); }
        HRESULT hr = tune.sync && tearing ? chain->Present(0, DXGI_PRESENT_ALLOW_TEARING) : chain->Present(1, DXGI_PRESENT_DO_NOT_WAIT);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG) InterlockedExchange(&present_lost, 1);
        if (FAILED(hr) && hr != DXGI_ERROR_WAS_STILL_DRAWING) { static int said; if (said++ < 5) logf("present: failed %08lx", (unsigned long)hr); }
        return SUCCEEDED(hr);
    };
    uint64_t drawn = 0; // order of the picture that is already in the back buffer
    LONGLONG last_present = 0, made_at = 0, again[4] = {}; // again: when the same picture is presented once more, see below
    int again_n = 0;
    auto repeat = [&](LONGLONG due) { // shows the last picture again when it is time; true: look at the queue again
        while (again_n && qpc() - again[0] > ticks_of(2)) { memmove(again, again + 1, sizeof(LONGLONG) * 3); --again_n; } // missed
        if (!again_n || (due && due < again[0] + ticks_of(6.6))) return false; // a scan takes a refresh: nothing may be in the way of the next picture
        LONGLONG left = again[0] - qpc();
        if (left > ticks_of(1.2) && !due) { WaitForSingleObject(item_wake, (DWORD)std::max(1.0, ms_of(left) - 1.0)); return true; }
        wait_until(again[0]);
        memmove(again, again + 1, sizeof(LONGLONG) * 3);
        --again_n;
        if (last_tex >= 0 && srv[last_tex]) { drawn = 0; draw(last_tex, last_made); present(); }
        return true;
    };
    LONG look[4] = {}; // what changes the look of a picture that stands still
    while (!stop_all)
    {
        bool hdr_want = (tune.hdr || pq_input) && num.hdr == 2;
        if (InterlockedExchange(&resized, 0) || (chain && hdr_want != hdr_now))
        {
            RECT r;
            GetClientRect(window, &r);
            if (r.right > 0 && r.bottom > 0 && (!chain || r.right != bw || r.bottom != bh || hdr_want != hdr_now))
            {
                drawn = 0;
                release(back);
                c->ClearState();
                c->Flush();
                bw = r.right;
                bh = r.bottom;
                num.view_w = bw;
                num.view_h = bh;
                if (!chain)
                {
                    DXGI_SWAP_CHAIN_DESC1 s{};
                    s.Width = (UINT)bw;
                    s.Height = (UINT)bh;
                    s.Format = hdr_want ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
                    s.SampleDesc.Count = 1;
                    s.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                    s.BufferCount = 3;
                    s.Scaling = DXGI_SCALING_NONE;
                    s.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                    s.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
                    s.Flags = tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
                    HRESULT hr = factory->CreateSwapChainForHwnd(d, window, &s, nullptr, nullptr, &chain);
                    if (FAILED(hr)) { logf("present: swap chain %08lx", (unsigned long)hr); InterlockedExchange(&present_lost, 1); break; }
                    chain->QueryInterface(IID_PPV_ARGS(&media));
                    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
                }
                else chain->ResizeBuffers(0, (UINT)bw, (UINT)bh, hdr_want ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM, tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
                hdr_now = hdr_want;
                IDXGISwapChain3 *c3 = nullptr;
                if (SUCCEEDED(chain->QueryInterface(IID_PPV_ARGS(&c3))))
                {
                    HRESULT hr = c3->SetColorSpace1(hdr_now ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
                    if (FAILED(hr)) logf("present: colour space %08lx", (unsigned long)hr);
                    c3->Release();
                }
                num.hdr_out = hdr_now;
                ID3D11Texture2D *bt = nullptr;
                chain->GetBuffer(0, IID_PPV_ARGS(&bt));
                if (bt) d->CreateRenderTargetView(bt, nullptr, &back);
                release(bt);
                sharp_level = -1;
                num.hz_mhz = monitor_mhz();
                logf("present: %dx%d tearing=%d refresh=%d", bw, bh, (int)tearing, (int)num.hz_mhz / 1000);
                InterlockedExchange(&redraw, 1);
            }
        }
        if (!back) { WaitForSingleObject(item_wake, 50); continue; }
        if (tex_generation != generation)
        {
            drawn = 0;
            for (auto &s : srv) release(s);
            AcquireSRWLockShared(&tex_lock.l);
            generation = tex_generation;
            for (int i = 0; i < TEXTURES; i++)
            {
                ID3D11Texture2D *t = nullptr;
                if (nt[i] && SUCCEEDED(d1->OpenSharedResource1(nt[i], IID_PPV_ARGS(&t)))) d->CreateShaderResourceView(t, nullptr, &srv[i]);
                release(t);
            }
            ReleaseSRWLockShared(&tex_lock.l);
            if (last_tex >= 0 && !srv[last_tex]) last_tex = -1;
        }
        Item it{};
        AcquireSRWLockExclusive(&item_lock.l);
        bool have = item_count > 0;
        if (have) it = items[0];
        ReleaseSRWLockExclusive(&item_lock.l);
        if (!have)
        {
            // A change of the label or the window is drawn with the next picture; without pictures, now.
            const LONG now_look[4] = {tune.fit, tune.sharpen + 1000 * tune.hdr_white, tune.sharpen_made + 10 * tune.hdr_boost, tune.corner};
            if (memcmp(look, now_look, sizeof look)) { memcpy(look, now_look, sizeof look); InterlockedExchange(&redraw, 1); }
            if (redraw && ms_of(qpc() - last_present) > 100)
            {
                InterlockedExchange(&redraw, 0);
                drawn = 0;
                draw(num.sources && ms_of(qpc() - num.last_arrival) < 3000 ? last_tex : -1, last_made);
                if (!present()) InterlockedExchange(&redraw, 1);
            }
            if (repeat(0)) continue;
            WaitForSingleObject(item_wake, 50);
            continue;
        }
        if (it.due > qpc())
        {
            if (repeat(it.due)) continue;
            if (it.order != drawn && srv[it.tex] && ms_of(it.due - qpc()) > 0.4) // drawn now, so that only the flip is left
            {
                draw(it.tex, it.made);
                c->Flush();
                drawn = it.order;
            }
            wait_until(it.due); // a newer picture may have come
            continue;
        }
        AcquireSRWLockExclusive(&item_lock.l);
        LONGLONG now = qpc();
        int take = 0;
        while (take + 1 < item_count && items[take + 1].due <= now) ++take;
        if (item_count) it = items[take];
        num.dropped = num.dropped + take;
        memmove(items, items + take + 1, sizeof(Item) * (size_t)std::max(0, item_count - take - 1));
        item_count = std::max(0, item_count - take - 1);
        ReleaseSRWLockExclusive(&item_lock.l);
        if (!srv[it.tex]) continue; // its texture is gone (DLSS 5 was switched off)
        static uint64_t shown_order;
        if (it.order + 1000 < shown_order) shown_order = 0; // a new picture size starts over
        if (it.order < shown_order) { num.dropped = num.dropped + 1; continue; } // finished too late: never step back
        shown_order = it.order;
        if (now - it.due > qpc_freq / 1000) num.late = num.late + 1;
        if (it.order != drawn) draw(it.tex, it.made);
        drawn = 0;
        last_present = qpc();
        if (present())
        {
            note_presented(it, qpc());
            rec_pq = pq_input;
            if (rec_on()) rec_frame(d, c, srv[it.tex], W, H, 1000.0 / std::max(4.0, src_ms) * (rec_made && tune.on && engine_family() >= 0 ? (double)tune.mult : 1.0), last_present, it.made != 0);
        }
        else InterlockedExchange(&redraw, 1); // if nothing follows, it is shown after all
        if (!it.made) SetEvent(shown_event);
        bool follows = last_made && it.made; // 3x, 4x
        last_tex = it.tex;
        last_made = it.made;
        // Variable refresh: below some rate the driver shows every picture twice by itself, and the original then has to wait behind the
        // second showing of the made picture. Shown again here, in the long gap after the original, the rate stays above that.
        again_n = 0;
        if (it.made) { if (!follows) made_at = last_present; }
        else if (num.vrr == 2 && num.composed != 1 && tune.sync == 1 && src_ms > 24 && src_ms < 60 && made_at && !video_tune)
        {
            LONGLONG next = made_at + ticks_of(src_ms);
            double span = ms_of(next - last_present);
            int n = std::min(3, (int)(span / 10.0));
            for (int i = 1; i <= n; i++) again[again_n++] = last_present + ticks_of(span * i / (n + 1));
        }
        if (media && GetTickCount64() - media_checked > 1000)
        {
            media_checked = GetTickCount64();
            DXGI_FRAME_STATISTICS_MEDIA fs{};
            HRESULT hr = media->GetFrameStatisticsMedia(&fs);
            static int said = -9;
            int now_mode = FAILED(hr) ? -1 : (int)fs.CompositionMode;
            if (now_mode != said) { said = now_mode; logf("present: composition mode %d (%08lx)", now_mode, (unsigned long)hr); }
            InterlockedExchange(&num.composed, FAILED(hr) ? -1 : fs.CompositionMode == DXGI_FRAME_PRESENTATION_MODE_COMPOSED);
        }
    }
    for (auto &s : srv) release(s);
    AcquireSRWLockExclusive(&label_lock.l);
    present_dev = nullptr;
    release(label_srv);
    ReleaseSRWLockExclusive(&label_lock.l);
    c->ClearState();
    release(back);
    release(media);
    release(chain);
    c->Flush();
    release(vs);
    release(show);
    release(sharp);
    release(label);
    release(smp);
    release(blend);
    release(cb);
    release(cb_out);
    release(factory);
    release(d1);
    release(c);
    release(d);
    return 0;
}

// ---- start and stop ----
bool video_start(HWND w)
{
    window = w;
    IDXGIFactory6 *f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return false;
    wchar_t which[8]; // tests: another graphics card
    f->EnumAdapterByGpuPreference(GetEnvironmentVariableW(L"PKAPP_GPU", which, 8) ? (UINT)_wtoi(which) : 0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    release(f);
    if (!adapter) return false;
    DXGI_ADAPTER_DESC1 ad;
    adapter->GetDesc1(&ad);
    logf("video: %ls", ad.Description);
    if (FAILED(D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx))) return false;
    // screen capture (Desktop Duplication, Windows Graphics Capture) works on this device from its own thread
    ID3D11Multithread *mt = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&mt)))) { mt->SetMultithreadProtected(TRUE); release(mt); }
    IDXGIDevice *xd = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&xd)))) { xd->SetGPUThreadPriority(5); release(xd); }
    vs_full = make_vs(dev, vs_src);
    ps_convert = make_ps(dev, ps_convert_src);
    vs_hud = make_vs(dev, hud_src, "vs_full");
    ps_compose = make_ps(dev, hud_src, "ps_compose");
    cs_count = make_cs(dev, hud_src, "cs_count");
    cs_map = make_cs(dev, hud_src, "cs_map");
    cs_luma = make_cs(dev, hud_src, "cs_luma");
    cs_bits = make_cs(dev, hud_src, "cs_bits");
    cs_cell = make_cs(dev, hud_src, "cs_cell");
    cs_act = make_cs(dev, hud_src, "cs_act");
    cs_block = make_cs(dev, hud_src, "cs_block");
    D3D11_BUFFER_DESC b{32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    dev->CreateBuffer(&b, nullptr, &cb_convert);
    dev->CreateBuffer(&b, nullptr, &cb_text);
    b.ByteWidth = 16;
    dev->CreateBuffer(&b, nullptr, &cb_hud);
    b.ByteWidth = 32;
    dev->CreateBuffer(&b, nullptr, &cb_flags);
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &linear);
    D3D11_QUERY_DESC q{D3D11_QUERY_EVENT, 0};
    for (auto &x : query) dev->CreateQuery(&q, &x);
    dev->CreateQuery(&q, &hud_query);
    ID3D11Device5 *d5 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d5))) && SUCCEEDED(ctx->QueryInterface(IID_PPV_ARGS(&ctx4))) && FAILED(d5->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) fence = nullptr;
    release(d5);
    dev->CreateQuery(&q, &spare_query);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &scissor);
    no_stream = GetEnvironmentVariableW(L"PKAPP_NO_STREAM", nullptr, 0) != 0; // tests
    if (!scissor || !vs_full || !ps_convert || !vs_hud || !ps_compose || !cs_count || !cs_map || !cs_luma || !cs_bits || !cs_cell || !cs_act || !cs_block || !cb_text || !cb_flags || !cb_convert || !cb_hud || !hud_query) return false;
    pool.start();
    job_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    shown_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    item_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    setup_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ahead.full = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    stream_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stream_handle = CreateThread(nullptr, 0, stream_main, nullptr, 0, nullptr);
    worker_handle = CreateThread(nullptr, 0, worker_main, nullptr, 0, nullptr);
    present_handle = CreateThread(nullptr, 0, present_main, nullptr, 0, nullptr);
    setup_handle = CreateThread(nullptr, 1 << 20, setup_main, nullptr, 0, nullptr);
    return worker_handle && present_handle && setup_handle;
}
void video_stop() // the capture must be closed first
{
    InterlockedExchange(&stop_all, 1);
    SetEvent(job_wake);
    SetEvent(item_wake);
    SetEvent(setup_wake);
    SetEvent(stream_wake);
    HANDLE h[4] = {worker_handle, present_handle, setup_handle, stream_handle};
    bool ended = WaitForMultipleObjects(4, h, TRUE, 8000) == WAIT_OBJECT_0;
    pool.finish();
    if (!ended) { logf("video: a thread did not stop"); return; } // the process ends next; never free what a thread still uses
    AcquireSRWLockExclusive(&gl.l);
    free_textures(0, TEXTURES);
    free_input();
    for (auto &x : query) release(x);
    release(hud_query);
    release(fence);
    release(ctx4);
    release(spare_query);
    ctx->ClearState();
    ctx->Flush();
    ReleaseSRWLockExclusive(&gl.l);
}
