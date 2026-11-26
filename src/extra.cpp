// SPDX-License-Identifier: GPL-3.0-only
#include "extra.h"
#include "base.h"
#include <algorithm>
#include <d3dcompiler.h>

enum { LEVELS = 5 };
struct Tex { ID3D11Texture2D *t; ID3D11ShaderResourceView *s; ID3D11UnorderedAccessView *u; };
static Tex still[3], before, calm, trust, state;
static bool before_ok;
static ID3D11ComputeShader *cs_still, *cs_wide;
static Tex pyr[2][LEVELS], vec[LEVELS], tmp[LEVELS], past[LEVELS], dense, vec2[LEVELS], past2[LEVELS], dense2, fix_motion, fix_raw, fix_weight;
static int rw, rh;
static int lw[LEVELS], lh[LEVELS], gw[LEVELS], gh[LEVELS], W, H, turn, have;
static bool flow_ok, flow2_ok;
static ID3D11ComputeShader *cs_down0, *cs_down, *cs_coarse, *cs_match, *cs_median, *cs_dense, *cs_solve, *cs_soft, *cs_calm, *cs_trust, *cs_judge, *cs_coarse_q, *cs_match_q;
static ID3D11VertexShader *vs;
static ID3D11PixelShader *ps;
static ID3D11Buffer *cb, *cb_draw;
static ID3D11SamplerState *smp;

static const char src[] =
    "cbuffer C:register(b0){uint LW;uint LH;uint GW;uint GH;uint PW;uint PH;float lambda;uint hasPast;};SamplerState lin:register(s0);"
    "Texture2D<float4> Color:register(t0);Texture2D<float> Up:register(t0);RWTexture2D<float> LOut:register(u0);"
    /* brightness at the base size: four taps, so every source pixel counts at 4K */
    "[numthreads(8,8,1)]void down0(uint3 id:SV_DispatchThreadID){if(id.x>=LW||id.y>=LH)return;float2 s=1.0/float2(LW,LH),c=(float2(id.xy)+0.5)*s;float v=0;"
    " [unroll]for(int k=0;k<4;k++){float3 x=Color.SampleLevel(lin,c+(float2(k&1,k>>1)-0.5)*0.5*s,0).rgb;v+=dot(x,float3(0.299,0.587,0.114));}LOut[id.xy]=v*0.25;}"
    "[numthreads(8,8,1)]void down(uint3 id:SV_DispatchThreadID){if(id.x>=LW||id.y>=LH)return;LOut[id.xy]=Up.SampleLevel(lin,(float2(id.xy)+0.5)/float2(LW,LH),0);}\n"
    "Texture2D<float> New:register(t0);Texture2D<float> Old:register(t1);Texture2D<float2> Coarse:register(t2);Texture2D<float2> Past:register(t3);RWTexture2D<float2> VOut:register(u0);"
    /* average difference of a window; samples outside the older picture do not count */
    "float cost(int2 o,float2 v){float s=0,n=0;float2 k=1.0/float2(LW,LH);[loop]for(int y=-2;y<6;y+=1)[loop]for(int x=-2;x<6;x+=1){int2 p=o+int2(x,y);if(p.x<0||p.y<0||p.x>=(int)LW||p.y>=(int)LH)continue;"
    "  float2 q=float2(p)+v;if(q.x<0||q.y<0||q.x>(float)LW-1||q.y>(float)LH-1)continue;s+=abs(New[p]-Old.SampleLevel(lin,(q+0.5)*k,0));n+=1;}"
    " return n<8?0.12:s/n;}"
    "[numthreads(4,4,1)]void coarse(uint3 id:SV_DispatchThreadID){if(id.x>=GW||id.y>=GH)return;int2 o=int2(id.xy)*4;float best=1e9;float2 bv=0;"
    " [loop]for(int y=-5;y<=5;y++)[loop]for(int x=-9;x<=9;x++){float2 v=float2(x,y);float c=cost(o,v)+lambda*0.5*length(v);if(c<best){best=c;bv=v;}}VOut[id.xy]=bv;}"
    "void trial(int2 o,float2 v,float2 pred,inout float best,inout float2 bv){float c=cost(o,v)+lambda*length(v-pred);if(c<best){best=c;bv=v;}}"
    "[numthreads(4,4,1)]void match(uint3 id:SV_DispatchThreadID){if(id.x>=GW||id.y>=GH)return;int2 o=int2(id.xy)*4;int2 pc=int2(id.xy)/2,side=int2((id.x&1)?1:-1,(id.y&1)?1:-1),mx=int2(PW,PH)-1;"
    " float2 pred=Coarse[min(pc,mx)]*2,bv=pred;float best=cost(o,pred);"
    " trial(o,Coarse[clamp(pc+int2(side.x,0),int2(0,0),mx)]*2,pred,best,bv);trial(o,Coarse[clamp(pc+int2(0,side.y),int2(0,0),mx)]*2,pred,best,bv);trial(o,Coarse[clamp(pc+side,int2(0,0),mx)]*2,pred,best,bv);"
    " trial(o,float2(0,0),pred,best,bv);if(hasPast)trial(o,Past[id.xy],pred,best,bv);"
    " float st[4]={1.0,1.0,0.5,0.25};[loop]for(int k=0;k<4;k++){float2 c0=bv;float s=st[k];trial(o,c0+float2(s,0),pred,best,bv);trial(o,c0-float2(s,0),pred,best,bv);trial(o,c0+float2(0,s),pred,best,bv);trial(o,c0-float2(0,s),pred,best,bv);"
    "  if(k==0){trial(o,c0+float2(s,s),pred,best,bv);trial(o,c0-float2(s,s),pred,best,bv);trial(o,c0+float2(s,-s),pred,best,bv);trial(o,c0-float2(s,-s),pred,best,bv);}}"
    " VOut[id.xy]=bv;}\n"
    "Texture2D<float2> VIn:register(t2);"
    "[numthreads(8,8,1)]void median(uint3 id:SV_DispatchThreadID){if(id.x>=GW||id.y>=GH)return;float2 v[9];int n=0;[unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++)v[n++]=VIn[clamp(int2(id.xy)+int2(x,y),int2(0,0),int2(GW,GH)-1)];"
    " float best=1e9;int bi=4;[loop]for(int i=0;i<9;i++){float s=0;[unroll]for(int j=0;j<9;j++)s+=abs(v[i].x-v[j].x)+abs(v[i].y-v[j].y);if(s<best||(s==best&&i==4)){best=s;bi=i;}}VOut[id.xy]=v[bi];}\n"
    /* every pixel of the base size: the neighbouring vector that explains its own surroundings best */
    "[numthreads(8,8,1)]void fine(uint3 id:SV_DispatchThreadID){if(id.x>=LW||id.y>=LH)return;int2 p=id.xy;float2 k=1.0/float2(LW,LH);int2 b=(p-2)/4;float best=1e9;float2 bv=0;"
    " [loop]for(int y=-1;y<=2;y++)[loop]for(int x=-1;x<=2;x++){float2 v=VIn[clamp(b+int2(x,y),int2(0,0),int2(GW,GH)-1)];float s=0,n=0;"
    "  [unroll]for(int j=-1;j<=1;j++)[unroll]for(int i=-1;i<=1;i++){int2 q=clamp(p+int2(i,j),int2(0,0),int2(LW,LH)-1);float2 r=float2(q)+v;if(r.x<0||r.y<0||r.x>(float)LW-1||r.y>(float)LH-1)continue;s+=abs(New[q]-Old.SampleLevel(lin,(r+0.5)*k,0));n+=1;}"
    "  float2 dc=float2(p)-(float2(b+int2(x,y))*4+2);float c=(n<1?0.05:s/n)+0.0006*length(dc);if(c<best){best=c;bv=v;}}VOut[id.xy]=bv;}\n";
static const char calm_src[] =
    "cbuffer C:register(b0){uint LW;uint LH;uint GW;uint GH;uint PW;uint PH;float lambda;uint hasPast;};Texture2D<float> New:register(t0);Texture2D<float2> VIn:register(t2);RWTexture2D<float2> VOut:register(u0);"
    /* every place takes the motion most of its surroundings of the same brightness agree on */
    "[numthreads(8,8,1)]void calm(uint3 id:SV_DispatchThreadID){if(id.x>=LW||id.y>=LH)return;int2 p=id.xy,hi=int2(LW,LH)-1;float me=New[p];float2 v[25];float w[25];int n=0;"
    " [unroll]for(int y=-2;y<=2;y++)[unroll]for(int x=-2;x<=2;x++){int2 q=clamp(p+int2(x,y)*(int)GW,int2(0,0),hi);v[n]=VIn[q];w[n]=1.0/(1.0+25.0*abs(New[q]-me));n++;}"
    " float best=1e9;int bi=12;[loop]for(int i=0;i<25;i+=(i==11?1:(i==12?1:2))){float s=0;[unroll]for(int j=0;j<25;j++)s+=w[j]*min(3.0,length(v[i]-v[j]));s-=i==12?lambda:0;if(s<best){best=s;bi=i;}}VOut[id.xy]=v[bi];}";
// How much of the picture the motion does not explain: after a jump of the camera nothing fits, and nothing may be moved. The same when
// most of the picture turned round twice in a row (a shaking camera).
static const char trust_src[] =
    "cbuffer C:register(b0){uint LW;uint LH;uint GW;uint GH;uint PW;uint PH;float lambda;uint hasPast;};SamplerState lin:register(s0);Texture2D<float> New:register(t0);Texture2D<float> Old:register(t1);Texture2D<float2> V:register(t2);Texture2D<float2> V0:register(t3);RWTexture2D<uint> Sum:register(u0);"
    "[numthreads(8,8,1)]void trust(uint3 id:SV_DispatchThreadID){int2 p=int2(id.xy)*2;if(p.x>=(int)LW||p.y>=(int)LH)return;float2 q=float2(p)+V[p];uint o;if(q.x<0||q.y<0||q.x>(float)LW-1||q.y>(float)LH-1)return;"
    " InterlockedAdd(Sum[uint2(1,0)],1,o);if(abs(New[p]-Old.SampleLevel(lin,(q+0.5)/float2(LW,LH),0))>14.0/255.0)InterlockedAdd(Sum[uint2(0,0)],1,o);if(hasPast){float2 f=V[p],f0=V0.SampleLevel(lin,(q+0.5)/float2(LW,LH),0);float g=length(f),g0=length(f0);if(min(g,g0)>1.2&&g0>0.3*g&&dot(f,f0)<-0.4*g*g0)InterlockedAdd(Sum[uint2(2,0)],1,o);}}\n"
    "Texture2D<uint> SumR:register(t0);RWTexture2D<float> State:register(u0);"
    /* compared with the pictures before: a game with much detail always has some that does not fit */
    "[numthreads(1,1,1)]void judge(uint3 id:SV_DispatchThreadID){float r=SumR[uint2(0,0)]/max(1.0,(float)SumR[uint2(1,0)]),a=State[uint2(0,0)];if(a<=0)a=r;float t=r>0.012?1-smoothstep(2.6,3.8,r/max(a,0.004)):1;if(r>0.3)t=0;float b=SumR[uint2(2,0)]/max(1.0,(float)SumR[uint2(1,0)]);t*=1-smoothstep(0.45,0.7,min(b,State[uint2(2,0)]));State[uint2(2,0)]=b;State[uint2(1,0)]=t;State[uint2(0,0)]=lerp(a,min(r,2*a+0.01),0.1);}";
// Repair: for a place y halfway, the motion f (older to newer) is found from each direction by a fixed point; the older original is read
// at y-f/2, the newer at y+f/2. A side counts when the motion there is the same one (nothing in front or behind).
static const char fix_src[] =
    "cbuffer D:register(b0){float2 size;float2 base;float2 grid;float2 pad;};SamplerState lin:register(s0);"
    "Texture2D<float4> A:register(t0);Texture2D<float4> B:register(t1);Texture2D<float4> G:register(t2);Texture2D<float2> FB:register(t3);Texture2D<float2> FA:register(t4);RWTexture2D<float2> MOut:register(u0);RWTexture2D<float2> WOut:register(u1);"
    "float2 fb(float2 p){return FB.SampleLevel(lin,p/size,0)*(size/base);}float2 fa(float2 p){return FA.SampleLevel(lin,p/size,0)*(size/base);}"
    "float3 rd(Texture2D<float4> t,float2 p){return t.SampleLevel(lin,p/size,0).rgb;}float mx(float3 v){return max(abs(v.x),max(abs(v.y),abs(v.z)));}bool inside(float2 p){return p.x>=0&&p.y>=0&&p.x<size.x&&p.y<size.y;}"
    "static const float2 tap[9]={float2(0,0),float2(-1,-1),float2(1,-1),float2(-1,1),float2(1,1),float2(-2,0),float2(2,0),float2(0,-2),float2(0,2)};"
    /* kind: 2 both, 1 older only, 0.. none -> score; D how far the made picture is away */
    "void judge(float2 y,float2 f,inout float best,inout float2 bf,inout float bk,inout float bd){float k=size.y/1080.0,tau=4.0*k;float2 xa=y-0.5*f,xb=y+0.5*f;"
    " float2 ea=abs(fa(xa)-f),eb=abs(-fb(xb)-f);bool va=inside(xa)&&ea.x+ea.y<tau,vb=inside(xb)&&eb.x+eb.y<tau;if(!va&&!vb)return;"
    " float r=0,da=0,db=0,dm=0;[unroll]for(int i=0;i<9;i++){float2 o=tap[i]*k;float3 a=rd(A,xa+o),b=rd(B,xb+o),g=rd(G,y+o);r+=mx(a-b);da+=mx(g-a);db+=mx(g-b);dm+=mx(g-(a+b)*0.5);}r*=255.0/9;"
    " float s,kind,d;if(va&&vb&&r<14.0){s=3.0-r/40.0;kind=0;d=dm;}else if(va&&!vb){s=1.0;kind=0.5;d=da;}else if(vb&&!va){s=1.0;kind=1.0;d=db;}else return;"
    " if(s>best+0.05){best=s;bf=f;bk=kind;bd=d*255.0/9;}}"
    "[numthreads(8,8,1)]void solve(uint3 id:SV_DispatchThreadID){if(id.x>=(uint)grid.x||id.y>=(uint)grid.y)return;float2 y=(float2(id.xy)+0.5)*size/grid;float best=0,bk=0,bd=0;float2 bf=0;"
    " float2 f=-fb(y);[unroll]for(int i=0;i<4;i++)f=-fb(y+0.5*f);judge(y,f,best,bf,bk,bd);"
    " f=fa(y);[unroll]for(int j=0;j<4;j++)f=fa(y-0.5*f);judge(y,f,best,bf,bk,bd);"
    " float w=0;if(best>1.5)w=saturate((bd-28.0)/10.0);else if(best>0)w=saturate((bd-80.0)/20.0);MOut[id.xy]=bf;WOut[id.xy]=float2(w,bk);}\n"
    "Texture2D<float2> Raw:register(t0);"
    "[numthreads(8,8,1)]void soften(uint3 id:SV_DispatchThreadID){if(id.x>=(uint)grid.x||id.y>=(uint)grid.y)return;float s=0,n=0;float2 me=Raw[id.xy];[unroll]for(int y=-2;y<=2;y++)[unroll]for(int x=-2;x<=2;x++){float2 o=Raw[clamp(int2(id.xy)+int2(x,y),int2(0,0),int2(grid)-1)];bool near=abs(x)<2&&abs(y)<2;if(near)s+=o.x;n+=o.x*(o.y==me.y?1:0);}"
    " float w=s/9;n/=25;if(me.y>0.25)w*=saturate((n-0.55)*4.0);else w*=saturate(n*4.0);WOut[id.xy]=float2(w,me.y);}";
// Where something really stands still: edges that did not move although the motion there says they should have, and their surroundings.
// Dark or flat ground that merely keeps its colour is not held.
static const char still_src[] =
    "cbuffer D:register(b0){float2 size;float2 base;float2 grid;float2 dir;};SamplerState lin:register(s0);Texture2D<float4> A:register(t0);Texture2D<float4> B:register(t1);Texture2D<float2> F:register(t2);Texture2D<uint> In:register(t0);RWTexture2D<uint> Out:register(u0);"
    "float mx(float3 v){v=abs(v);return max(v.x,max(v.y,v.z));}float3 rd(Texture2D<float4> t,float2 p){return t.SampleLevel(lin,p/size,0).rgb;}"
    "[numthreads(8,8,1)]void still(uint3 id:SV_DispatchThreadID){if(id.x>=480||id.y>=270)return;float2 cell=size/float2(480,270);float k=size.y/1080.0;uint o=0;"
    " [loop]for(int y=0;y<4;y++)[loop]for(int x=0;x<4;x++){float2 p=(float2(id.xy)+(float2(x,y)+0.5)/4)*cell;float3 b=rd(B,p);if(mx(b-rd(A,p))>8.0/255)continue;"
    "  float e=max(max(mx(b-rd(B,p+float2(k,0))),mx(b-rd(B,p-float2(k,0)))),max(mx(b-rd(B,p+float2(0,k))),mx(b-rd(B,p-float2(0,k)))));if(e<40.0/255)continue;"
    "  float2 f=F.SampleLevel(lin,p/size,0)*(size/base);float m=0;[unroll]for(int i=0;i<5;i++){float2 q=p+(i==0?float2(0,0):float2(i&1?(i==1?k:-k):0,i&1?0:(i==2?k:-k)));m=max(m,mx(rd(B,q)-rd(A,q+f)));}"
    "  if(m>28.0/255)o=1;}Out[id.xy]=o;}"
    "[numthreads(8,8,1)]void wide(uint3 id:SV_DispatchThreadID){if(id.x>=480||id.y>=270)return;uint o=0;[loop]for(int i=-12;i<=12;i++)o|=In[clamp(int2(id.xy)+int2(dir)*i,int2(0,0),int2(479,269))];Out[id.xy]=o;}";
static const char draw_src[] =
    "cbuffer D:register(b0){float2 size;float2 base;float t;uint hudOn;uint textOn;uint STEP;uint CW;uint CH;uint DW;uint DH;float accel;float pastRate;float2 dpad;};SamplerState lin:register(s0);"
    "Texture2D<float4> A:register(t0);Texture2D<float4> B:register(t1);Texture2D<float2> F:register(t2);Texture2D<uint> Hud:register(t3);Texture2D<uint> Bits:register(t4);Texture2D<float> Block:register(t5);Texture2D<uint> Still:register(t6);Texture2D<float2> F0:register(t7);Texture2D<float> State:register(t8);static float T;"
    "float4 vsm(uint i:SV_VertexID):SV_Position{float2 q=float2((i<<1)&2,i&2);return float4(q*float2(2,-2)+float2(-1,1),0,1);}"
    "float2 flow(float2 p){float2 f=F.SampleLevel(lin,p/size,0)*(size/base);if(accel>0){float2 f0=F0.SampleLevel(lin,(p+f)/size,0)*(size/base)*pastRate,d=f-f0;float l=length(d),lim=0.5*length(f)+2;if(l>lim)d*=lim/l*saturate(3-2*l/lim);f+=d*accel*(t+1)*0.5;}return f*T;}"
    /* does this place belong to a HUD or a text? */
    "bool held(float2 p){int2 q=int2(p);if(q.x<0||q.y<0||q.x>=(int)size.x||q.y>=(int)size.y)return false;if(!Still[int2(p/size*float2(480,270))])return false;if(hudOn&&Hud[q])return true;if(!textOn)return false;if(Block.SampleLevel(lin,p/(float2(CW,CH)*4.0*STEP),0)<=0.3)return false;"
    " int2 e=q/(int)STEP;[unroll]for(int y=-2;y<=2;y++)[unroll]for(int x=-2;x<=2;x++)if(Bits[clamp(e+int2(x,y),int2(0,0),int2(DW,DH)-1)]&2u)return true;return false;}"
    /* the motion towards a border is squeezed: there is nothing beyond it to bring in */
    "float soft(float p,float n,float e){if(p>n-e)return n-e+e*(1-exp(-(p-(n-e))/e));if(p<e)return e-e*(1-exp(-(e-p)/e));return p;}"
    "float4 psm(float4 sp:SV_Position):SV_Target{float2 p=sp.xy;T=State[uint2(1,0)];if(T<=0)return float4(B[int2(p)].rgb,1);if(held(p))return float4(B[int2(p)].rgb,1);float2 f=flow(p);[unroll]for(int i=0;i<3;i++)f=flow(p+f*t);"
    " float2 m=p+f*t;float e=size.y/12.0;float2 o=m;if((o.x>size.x-e&&f.x>0)||(o.x<e&&f.x<0))m.x=lerp(o.x,soft(o.x,size.x,e),saturate(abs(f.x)*t/(size.y/18.0)));if((o.y>size.y-e&&f.y>0)||(o.y<e&&f.y<0))m.y=lerp(o.y,soft(o.y,size.y,e),saturate(abs(f.y)*t/(size.y/18.0)));"
    " if(held(m)){float2 a=m+flow(m);if(a.x>=0&&a.y>=0&&a.x<size.x&&a.y<size.y&&!held(a))return float4(A.SampleLevel(lin,a/size,0).rgb,1);}"
    " return float4(B.SampleLevel(lin,m/size,0).rgb,1);}";

template <class T> static void drop(T *&p) { if (p) { p->Release(); p = nullptr; } }
static ID3DBlob *compile(const char *s, const char *entry, const char *target)
{
    ID3DBlob *code = nullptr, *err = nullptr;
    if (FAILED(D3DCompile(s, strlen(s), nullptr, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err))) logf("extra shader %s: %s", entry, err ? (const char *)err->GetBufferPointer() : "?");
    drop(err);
    return code;
}
static bool make(ID3D11Device *d, const char *entry, ID3D11ComputeShader **out, const char *from = src)
{
    ID3DBlob *b = compile(from, entry, "cs_5_0");
    bool ok = b && SUCCEEDED(d->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, out));
    drop(b);
    return ok;
}
static SRWLOCK init_lock = SRWLOCK_INIT;
static volatile LONG prepared;
bool extra_prepared() { return prepared != 0; }
// The quick search: a quarter of the places of each window and fewer steps. Its chains of tries, not its size, are what takes time on the card.
static std::string quick_source()
{
    std::string s = src;
    auto put = [&](const char *a, const char *b) { size_t p = s.find(a); if (p != s.npos) s.replace(p, strlen(a), b); };
    put("[loop]for(int y=-2;y<6;y+=1)[loop]for(int x=-2;x<6;x+=1)", "[loop]for(int y=-1;y<6;y+=2)[loop]for(int x=-1;x<6;x+=2)");
    put("return n<8?0.12:s/n;", "return n<2?0.12:s/n;");
    put("[loop]for(int k=0;k<4;k++){float2 c0", "[loop]for(int k=0;k<4;k+=2){float2 c0");
    return s;
}
static bool init(ID3D11Device *d)
{
    if (ps) return true;
    static const std::string quick = quick_source();
    if (!make(d, "coarse", &cs_coarse_q, quick.c_str()) || !make(d, "match", &cs_match_q, quick.c_str())) return false;
    bool ok = make(d, "down0", &cs_down0) && make(d, "down", &cs_down) && make(d, "coarse", &cs_coarse) && make(d, "match", &cs_match) && make(d, "median", &cs_median) && make(d, "fine", &cs_dense) && make(d, "solve", &cs_solve, fix_src) && make(d, "soften", &cs_soft, fix_src) && make(d, "calm", &cs_calm, calm_src) && make(d, "trust", &cs_trust, trust_src) && make(d, "judge", &cs_judge, trust_src) && make(d, "still", &cs_still, still_src) && make(d, "wide", &cs_wide, still_src);
    ID3DBlob *v = compile(draw_src, "vsm", "vs_5_0"), *p = compile(draw_src, "psm", "ps_5_0");
    ok = ok && v && p && SUCCEEDED(d->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &vs)) && SUCCEEDED(d->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &ps));
    drop(v);
    drop(p);
    D3D11_BUFFER_DESC b{32, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    ok = ok && SUCCEEDED(d->CreateBuffer(&b, nullptr, &cb));
    b.ByteWidth = 64;
    ok = ok && SUCCEEDED(d->CreateBuffer(&b, nullptr, &cb_draw));
    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    return ok && SUCCEEDED(d->CreateSamplerState(&s, &smp));
}
bool extra_init(ID3D11Device *d)
{
    if (prepared) return true;
    AcquireSRWLockExclusive(&init_lock);
    bool ok = init(d);
    if (ok) InterlockedExchange(&prepared, 1);
    ReleaseSRWLockExclusive(&init_lock);
    return ok;
}
static void free_tex(Tex &x) { drop(x.s); drop(x.u); drop(x.t); }
static void free_all()
{
    for (auto &a : pyr) for (auto &x : a) free_tex(x);
    for (auto &x : vec) free_tex(x);
    for (auto &x : tmp) free_tex(x);
    for (auto &x : past) free_tex(x);
    for (auto &x : vec2) free_tex(x);
    for (auto &x : past2) free_tex(x);
    for (auto &x : still) free_tex(x);
    free_tex(before);
    free_tex(calm);
    free_tex(trust);
    free_tex(state);
    free_tex(dense);
    free_tex(dense2);
    free_tex(fix_motion);
    free_tex(fix_raw);
    free_tex(fix_weight);
}
static bool make_tex(ID3D11Device *d, Tex &x, int w, int h, DXGI_FORMAT f)
{
    D3D11_TEXTURE2D_DESC t{};
    t.Width = (UINT)w;
    t.Height = (UINT)h;
    t.MipLevels = t.ArraySize = 1;
    t.Format = f;
    t.SampleDesc.Count = 1;
    t.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(d->CreateTexture2D(&t, nullptr, &x.t)) && SUCCEEDED(d->CreateShaderResourceView(x.t, nullptr, &x.s)) && SUCCEEDED(d->CreateUnorderedAccessView(x.t, nullptr, &x.u));
}
bool extra_size(ID3D11Device *d, ID3D11DeviceContext *, int w, int h, int lines)
{
    static int lines_now;
    if (w == W && h == H && lines == lines_now && dense.t) return true;
    lines_now = lines;
    free_all();
    W = w;
    H = h;
    bool ok = true;
    lh[0] = std::min(h, lines);
    lw[0] = (int)((int64_t)w * lh[0] / h);
    for (int l = 0; l < LEVELS && ok; l++)
    {
        if (l) { lw[l] = (lw[l - 1] + 1) / 2; lh[l] = (lh[l - 1] + 1) / 2; }
        gw[l] = (lw[l] + 3) / 4;
        gh[l] = (lh[l] + 3) / 4;
        ok = make_tex(d, pyr[0][l], lw[l], lh[l], DXGI_FORMAT_R16_UNORM) && make_tex(d, pyr[1][l], lw[l], lh[l], DXGI_FORMAT_R16_UNORM) && make_tex(d, vec[l], gw[l], gh[l], DXGI_FORMAT_R16G16_FLOAT) &&
             make_tex(d, tmp[l], gw[l], gh[l], DXGI_FORMAT_R16G16_FLOAT) && make_tex(d, past[l], gw[l], gh[l], DXGI_FORMAT_R16G16_FLOAT) && make_tex(d, vec2[l], gw[l], gh[l], DXGI_FORMAT_R16G16_FLOAT) && make_tex(d, past2[l], gw[l], gh[l], DXGI_FORMAT_R16G16_FLOAT);
    }
    rh = std::min(h, 540);
    rw = (int)((int64_t)w * rh / h);
    ok = ok && make_tex(d, trust, 3, 1, DXGI_FORMAT_R32_UINT) && make_tex(d, state, 3, 1, DXGI_FORMAT_R32_FLOAT) && make_tex(d, calm, lw[0], lh[0], DXGI_FORMAT_R16G16_FLOAT) && make_tex(d, before, lw[0], lh[0], DXGI_FORMAT_R16G16_FLOAT) && make_tex(d, dense, lw[0], lh[0], DXGI_FORMAT_R16G16_FLOAT) && make_tex(d, dense2, lw[0], lh[0], DXGI_FORMAT_R16G16_FLOAT) && make_tex(d, fix_motion, rw, rh, DXGI_FORMAT_R16G16_FLOAT) &&
         make_tex(d, fix_raw, rw, rh, DXGI_FORMAT_R8G8_UNORM) && make_tex(d, fix_weight, rw, rh, DXGI_FORMAT_R8G8_UNORM);
    for (auto &x : still) ok = ok && make_tex(d, x, 480, 270, DXGI_FORMAT_R8_UINT);
    extra_forget();
    if (!ok) { logf("extra: textures failed"); free_all(); W = H = 0; }
    return ok;
}
void extra_forget() { have = 0; flow_ok = flow2_ok = false; }
bool extra_both_ready() { return flow_ok && flow2_ok; }
bool extra_ready() { return flow_ok; }
void extra_feed(ID3D11DeviceContext *c, ID3D11ShaderResourceView *newest, bool both, bool quick)
{
    if (!dense.t) return;
    int now = turn ^= 1, old = now ^ 1;
    ID3D11ShaderResourceView *none[4] = {};
    ID3D11UnorderedAccessView *nou = nullptr;
    c->CSSetSamplers(0, 1, &smp);
    c->CSSetConstantBuffers(0, 1, &cb);
    auto run = [&](ID3D11ComputeShader *s, int l, int w, int h, int group, ID3D11ShaderResourceView *a, ID3D11ShaderResourceView *b, ID3D11ShaderResourceView *e, ID3D11ShaderResourceView *f, ID3D11UnorderedAccessView *u, float lambda) {
        struct { UINT lw, lh, gw, gh, pw, ph; float lambda; UINT past; } k = {(UINT)lw[l], (UINT)lh[l], (UINT)gw[l], (UINT)gh[l], l + 1 < LEVELS ? (UINT)gw[l + 1] : 1, l + 1 < LEVELS ? (UINT)gh[l + 1] : 1, lambda, have > 1};
        c->UpdateSubresource(cb, 0, nullptr, &k, 0, 0);
        ID3D11ShaderResourceView *in[4] = {a, b, e, f};
        c->CSSetShader(s, nullptr, 0);
        c->CSSetShaderResources(0, 4, in);
        c->CSSetUnorderedAccessViews(0, 1, &u, nullptr);
        c->Dispatch((UINT)(w + group - 1) / group, (UINT)(h + group - 1) / group, 1);
        c->CSSetUnorderedAccessViews(0, 1, &nou, nullptr);
        c->CSSetShaderResources(0, 4, none);
    };
    run(cs_down0, 0, lw[0], lh[0], 8, newest, nullptr, nullptr, nullptr, pyr[now][0].u, 0);
    for (int l = 1; l < LEVELS; l++) run(cs_down, l, lw[l], lh[l], 8, pyr[now][l - 1].s, nullptr, nullptr, nullptr, pyr[now][l].u, 0);
    if (have)
    {
        for (int l = LEVELS - 1; l >= 0; l--)
        {
            const float lambda = 0.0012f;
            if (l == LEVELS - 1) run(quick ? cs_coarse_q : cs_coarse, l, gw[l], gh[l], 4, pyr[now][l].s, pyr[old][l].s, nullptr, nullptr, tmp[l].u, lambda);
            else run(quick ? cs_match_q : cs_match, l, gw[l], gh[l], 4, pyr[now][l].s, pyr[old][l].s, vec[l + 1].s, past[l].s, tmp[l].u, lambda);
            run(cs_median, l, gw[l], gh[l], 8, nullptr, nullptr, tmp[l].s, nullptr, vec[l].u, 0);
        }
        before_ok = flow_ok;
        if (flow_ok) c->CopyResource(before.t, dense.t);
        run(cs_dense, 0, lw[0], lh[0], 8, pyr[now][0].s, pyr[old][0].s, vec[0].s, nullptr, dense.u, 0);
        const int calm_n = lh[0] >= 360 ? 2 : 0; // fast graphics cards only
        for (int k = 0; k < calm_n; k++)
        {
            struct { UINT lw, lh, gw, gh, pw, ph; float lambda; UINT past; } q = {(UINT)lw[0], (UINT)lh[0], k & 1 ? 1u : 2u, 0, 0, 0, 0.5f, 0};
            c->UpdateSubresource(cb, 0, nullptr, &q, 0, 0);
            ID3D11ShaderResourceView *in[4] = {pyr[now][0].s, nullptr, k & 1 ? calm.s : dense.s, nullptr};
            c->CSSetShader(cs_calm, nullptr, 0);
            c->CSSetShaderResources(0, 4, in);
            c->CSSetUnorderedAccessViews(0, 1, k & 1 ? &dense.u : &calm.u, nullptr);
            c->Dispatch((UINT)(lw[0] + 7) / 8, (UINT)(lh[0] + 7) / 8, 1);
            c->CSSetUnorderedAccessViews(0, 1, &nou, nullptr);
            c->CSSetShaderResources(0, 4, none);
        }
        if (!quick)
        {
            const UINT zero[4] = {};
            c->ClearUnorderedAccessViewUint(trust.u, zero);
            run(cs_trust, 0, lw[0] / 2, lh[0] / 2, 8, pyr[now][0].s, pyr[old][0].s, dense.s, before_ok ? before.s : nullptr, trust.u, 0);
            run(cs_judge, 0, 1, 1, 1, trust.s, nullptr, nullptr, nullptr, state.u, 0);
        }
        for (int l = 0; l < LEVELS; l++) c->CopyResource(past[l].t, vec[l].t);
        flow_ok = true;
        flow2_ok = both;
        if (both)
        {
            for (int l = LEVELS - 1; l >= 0; l--)
            {
                const float lambda = 0.0012f;
                if (l == LEVELS - 1) run(cs_coarse, l, gw[l], gh[l], 4, pyr[old][l].s, pyr[now][l].s, nullptr, nullptr, tmp[l].u, lambda);
                else run(cs_match, l, gw[l], gh[l], 4, pyr[old][l].s, pyr[now][l].s, vec2[l + 1].s, past2[l].s, tmp[l].u, lambda);
                run(cs_median, l, gw[l], gh[l], 8, nullptr, nullptr, tmp[l].s, nullptr, vec2[l].u, 0);
            }
            run(cs_dense, 0, lw[0], lh[0], 8, pyr[old][0].s, pyr[now][0].s, vec2[0].s, nullptr, dense2.u, 0);
            for (int l = 0; l < LEVELS; l++) c->CopyResource(past2[l].t, vec2[l].t);
        }
    }
    have = std::min(have + 1, 2);
}
void extra_draw(ID3D11DeviceContext *c, ID3D11RenderTargetView *target, ID3D11ShaderResourceView *older, ID3D11ShaderResourceView *newest, float t, float past_rate, const ExtraProtect &p)
{
    ID3D11ShaderResourceView *in3[3] = {older, newest, dense.s}, *no3[3] = {};
    ID3D11UnorderedAccessView *nou = nullptr;
    c->CSSetSamplers(0, 1, &smp);
    c->CSSetConstantBuffers(0, 1, &cb);
    for (int i = 0; i < 3; i++)
    {
        float f[8] = {(float)W, (float)H, (float)lw[0], (float)lh[0], 480, 270, i == 1 ? 1.f : 0.f, i == 2 ? 1.f : 0.f};
        c->UpdateSubresource(cb, 0, nullptr, f, 0, 0);
        c->CSSetShader(i ? cs_wide : cs_still, nullptr, 0);
        c->CSSetShaderResources(0, i ? 1 : 3, i ? &still[i - 1].s : in3);
        c->CSSetUnorderedAccessViews(0, 1, &still[i].u, nullptr);
        c->Dispatch(60, 34, 1);
        c->CSSetUnorderedAccessViews(0, 1, &nou, nullptr);
        c->CSSetShaderResources(0, 3, no3);
    }
    const float accel = 0.5f; // how much of the change of the motion goes on
    struct { float w, h, bw, bh, t; UINT hud, text, step, cw, ch, dw, dh; float accel, past, pad[2]; } k = {(float)W, (float)H, (float)lw[0], (float)lh[0], t, p.hud_on, p.text_on, std::max(1u, p.step), p.cw, p.ch, p.dw, p.dh, before_ok ? accel : 0, past_rate, {0, 0}};
    c->UpdateSubresource(cb_draw, 0, nullptr, &k, 0, 0);
    D3D11_VIEWPORT vp = {0, 0, (float)W, (float)H, 0, 1};
    ID3D11ShaderResourceView *s[9] = {older, newest, dense.s, p.hud, p.bits, p.block, still[2].s, before.s, state.s}, *none[9] = {};
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vs, nullptr, 0);
    c->PSSetShader(ps, nullptr, 0);
    c->PSSetConstantBuffers(0, 1, &cb_draw);
    c->PSSetSamplers(0, 1, &smp);
    c->PSSetShaderResources(0, 9, s);
    c->RSSetViewports(1, &vp);
    c->OMSetRenderTargets(1, &target, nullptr);
    c->Draw(3, 0);
    ID3D11RenderTargetView *nort = nullptr;
    c->OMSetRenderTargets(1, &nort, nullptr);
    c->PSSetShaderResources(0, 9, none);
}
ID3D11ShaderResourceView *extra_motion(int *w, int *h)
{
    *w = lw[0];
    *h = lh[0];
    return dense.s;
}
void extra_repair(ID3D11DeviceContext *c, ID3D11ShaderResourceView *older, ID3D11ShaderResourceView *newest, ID3D11ShaderResourceView *made)
{
    float k[8] = {(float)W, (float)H, (float)lw[0], (float)lh[0], (float)rw, (float)rh, 0, 0};
    c->UpdateSubresource(cb, 0, nullptr, k, 0, 0);
    ID3D11ShaderResourceView *in[5] = {older, newest, made, dense.s, dense2.s}, *none[5] = {};
    ID3D11UnorderedAccessView *u[2] = {fix_motion.u, fix_raw.u}, *nou[2] = {};
    c->CSSetSamplers(0, 1, &smp);
    c->CSSetConstantBuffers(0, 1, &cb);
    c->CSSetShader(cs_solve, nullptr, 0);
    c->CSSetShaderResources(0, 5, in);
    c->CSSetUnorderedAccessViews(0, 2, u, nullptr);
    c->Dispatch((UINT)(rw + 7) / 8, (UINT)(rh + 7) / 8, 1);
    c->CSSetUnorderedAccessViews(0, 2, nou, nullptr);
    c->CSSetShaderResources(0, 5, none);
    c->CSSetShader(cs_soft, nullptr, 0);
    c->CSSetShaderResources(0, 1, &fix_raw.s);
    c->CSSetUnorderedAccessViews(1, 1, &fix_weight.u, nullptr);
    c->Dispatch((UINT)(rw + 7) / 8, (UINT)(rh + 7) / 8, 1);
    c->CSSetUnorderedAccessViews(1, 1, nou, nullptr);
    c->CSSetShaderResources(0, 1, none);
}
void extra_repair_views(ID3D11ShaderResourceView **motion, ID3D11ShaderResourceView **weight)
{
    *motion = fix_motion.s;
    *weight = fix_weight.s;
}
