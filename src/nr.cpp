// SPDX-License-Identifier: GPL-3.0-only
#include "nr.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>

// ---- the runtime of a game ----
static std::wstring found;
static bool looked;
static std::mutex look; // the signature of 250 MB takes half a second
static std::atomic<int> known{-1};
static bool nvidia_signed(const std::wstring &file)
{
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof fi;
    fi.pcwszFilePath = file.c_str();
    WINTRUST_DATA d{};
    d.cbStruct = sizeof d;
    d.dwUIChoice = WTD_UI_NONE;
    d.fdwRevocationChecks = WTD_REVOKE_NONE;
    d.dwUnionChoice = WTD_CHOICE_FILE;
    d.pFile = &fi;
    d.dwStateAction = WTD_STATEACTION_VERIFY;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG r = WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &policy, &d);
    bool ok = false;
    if (!r)
        if (CRYPT_PROVIDER_DATA *pd = WTHelperProvDataFromStateData(d.hWVTStateData))
            if (CRYPT_PROVIDER_SGNR *sg = WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0))
                if (CRYPT_PROVIDER_CERT *c = WTHelperGetProvCertFromChain(sg, 0))
                {
                    wchar_t name[256] = L"";
                    CertGetNameStringW(c->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, 256);
                    ok = !_wcsnicmp(name, L"NVIDIA Corporation", 18);
                }
    d.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust((HWND)INVALID_HANDLE_VALUE, &policy, &d);
    return ok;
}
static bool there(const std::wstring &f) { return GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES; }
// Looks for nvngx_dlssnr.dll in a folder and the folders under it (up to `depth` levels).
static void dig(const std::wstring &dir, int depth, std::vector<std::wstring> &out)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (depth > 0 && wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) dig(dir + L"\\" + fd.cFileName, depth - 1, out);
        }
        else if (!_wcsicmp(fd.cFileName, L"nvngx_dlssnr.dll")) out.push_back(dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
// The games on this PC that bring the file (Steam libraries, Epic, Xbox app), newest file first. Only files signed by NVIDIA are used.
static std::wstring in_games()
{
    std::vector<std::wstring> roots;
    wchar_t p[MAX_PATH] = L"";
    DWORD n = sizeof p;
    if (!RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, p, &n))
    {
        std::vector<std::wstring> libs = {p};
        if (FILE *f = _wfopen((std::wstring(p) + L"\\steamapps\\libraryfolders.vdf").c_str(), L"rb"))
        {
            char line[1024];
            while (fgets(line, sizeof line, f))
            {
                char *a = strstr(line, "\"path\""), *b = a ? strchr(a + 6, '"') : nullptr, *e = b ? strchr(b + 1, '"') : nullptr;
                if (!e) continue;
                *e = 0;
                wchar_t w[512];
                if (!MultiByteToWideChar(CP_UTF8, 0, b + 1, -1, w, 512)) continue;
                std::wstring s;
                for (const wchar_t *c = w; *c; c++)
                {
                    s += *c;
                    if (*c == L'\\' && c[1] == L'\\') c++;
                }
                libs.push_back(s);
            }
            fclose(f);
        }
        for (auto &l : libs) roots.push_back(l + L"\\steamapps\\common");
    }
    wchar_t pf[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) roots.push_back(std::wstring(pf) + L"\\Epic Games");
    for (wchar_t d = L'C'; d <= L'Z'; d++)
    {
        wchar_t r[4] = {d, L':', L'\\', 0};
        if (GetDriveTypeW(r) != DRIVE_FIXED) continue;
        roots.push_back(std::wstring(1, d) + L":\\XboxGames");
        roots.push_back(std::wstring(1, d) + L":\\Epic Games");
    }
    std::vector<std::wstring> all;
    // first where NVIDIA itself puts its files: the display driver and the NVIDIA app's NGX folder (no game needed when it is there)
    wchar_t win[MAX_PATH] = L"", pd[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"SystemRoot", win, MAX_PATH)) dig(std::wstring(win) + L"\\System32\\DriverStore\\FileRepository", 3, all);
    if (GetEnvironmentVariableW(L"ProgramData", pd, MAX_PATH)) dig(std::wstring(pd) + L"\\NVIDIA\\NGX", 4, all);
    for (auto &r : roots) dig(r, 5, all); // root, game, ..., streamline, nvngx_dlssnr.dll
    auto when = [](const std::wstring &f) {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        GetFileAttributesExW(f.c_str(), GetFileExInfoStandard, &a);
        return ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    };
    std::sort(all.begin(), all.end(), [&](const std::wstring &a, const std::wstring &b) { return when(a) > when(b); });
    for (auto &f : all)
        if (nvidia_signed(f)) { logf("nr: found in a game: %ls", f.c_str()); return f; }
    if (!all.empty()) logf("nr: %d files in games, none signed by NVIDIA", (int)all.size());
    return L"";
}
int nr_there()
{
    static std::once_flag once;
    std::call_once(once, [] { std::thread([] { nr_file(); }).detach(); });
    return known;
}
bool nr_built() { return true; }
std::wstring nr_file()
{
    std::lock_guard<std::mutex> g(look);
    if (looked) return found;
    looked = true;
    found = ini_text(L"nr_file", L"");
    if (found.empty() || !there(found)) found = exe_dir() + L"\\nvngx_dlssnr.dll"; // put there by the person who uses the program
    if (!there(found)) found = data_dir() + L"\\dlss5\\nvngx_dlssnr.dll";
    if (there(found) && !nvidia_signed(found)) { logf("nr: the file is not signed by NVIDIA"); found.clear(); }
    if (found.empty() || !there(found)) found = in_games(); // checks the signature itself
    known = !found.empty();
    return found;
}
static volatile LONG choice;
int nr_choice() { return (int)choice; }
static std::wstring folder; // of the runtime NGX was started with
static void *params;
std::wstring nr_choose(const std::wstring &file)
{
    size_t k = file.find_last_of(L"\\/");
    if (k == file.npos || _wcsicmp(file.c_str() + k + 1, L"nvngx_dlssnr.dll")) return tr(L"nvngx_dlssnr.dll 파일을 골라 주세요", L"Please choose nvngx_dlssnr.dll");
    if (!nvidia_signed(file)) return tr(L"NVIDIA가 서명한 파일이 아니에요. 고친 파일은 쓰지 않아요", L"This file is not signed by NVIDIA. Modified files are not used");
    ini_set(L"nr_file", file);
    if (params && _wcsicmp(file.substr(0, k).c_str(), folder.c_str())) return tr(L"다시 켜면 이 파일을 써요", L"This file is used after a restart");
    {
        std::lock_guard<std::mutex> g(look);
        looked = false;
    }
    nr_file();
    InterlockedIncrement(&choice);
    return L"";
}
// ---- the driver's interface (NGX), as its loader finds it ----
struct Log { void (*call)(const char *, int, int); int level; bool only; };
struct Info { const wchar_t *const *path; unsigned paths; void *internal; Log log; };
typedef unsigned Result;
static bool failed(Result r) { return (r & 0xFFF00000u) == 0xBAD00000u; }
static struct
{
    HMODULE m;
    Result (*init)(const char *, int, const char *, const wchar_t *, ID3D12Device *, int, const Info *);
    Result (*alloc)(void **);
    Result (*caps)(void **);
    Result (*create)(ID3D12GraphicsCommandList *, int, void *, void **);
    Result (*eval)(ID3D12GraphicsCommandList *, const void *, const void *, void *);
    Result (*drop)(void *);
} ngx;
enum { SET_D3D12 = 1, SET_UI = 4, SET_F = 6, GET_I = 11 }; // places in the parameter object's table
template <class T> static void put(void *p, int slot, const char *n, T v) { (*(void (***)(void *, const char *, T))p)[slot](p, n, v); }
static std::wstring core()
{
    for (const wchar_t *k : {L"System\\CurrentControlSet\\Services\\nvlddmkm\\Parameters\\NGXCore", L"System\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore"})
    {
        wchar_t p[MAX_PATH] = L"";
        DWORD n = sizeof p;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, k, L"NGXPath", RRF_RT_REG_SZ, nullptr, p, &n)) continue;
        std::wstring f = std::wstring(p) + L"\\_nvngx.dll";
        if (there(f)) return f;
    }
    WIN32_FIND_DATAW fd;
    std::wstring base = L"C:\\Windows\\System32\\DriverStore\\FileRepository\\", best;
    HANDLE h = FindFirstFileW((base + L"nv_dispi.inf_amd64_*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return best;
    do
        if (there(base + fd.cFileName + L"\\_nvngx.dll")) best = base + fd.cFileName + L"\\_nvngx.dll";
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

// ---- shared pictures and the second device ----
static SRWLOCK lock = SRWLOCK_INIT;
struct Shared { ID3D11Texture2D *t; ID3D11ShaderResourceView *s; ID3D11RenderTargetView *r; ID3D12Resource *d; };
enum { LAYERS = 5 };
static Shared in, mv, outs[LAYERS];
enum { KEYS = 5 };
static ID3D11Texture2D *acc_t[KEYS + 1]; // what the network changed, steadied over time
static ID3D11ShaderResourceView *acc_s[KEYS + 1];
static ID3D11RenderTargetView *acc_r[KEYS + 1]; // the last one: between two pictures
static ID3D11Texture2D *mvk_t[KEYS];
static ID3D11ShaderResourceView *mvk_s[KEYS];
static ID3D11Texture2D *net_t[2]; // the same of the network itself, the newest and the one before
static ID3D11ShaderResourceView *net_s[2];
static ID3D11RenderTargetView *net_r[2];
static int net_turn;
static bool net_ok, think_due, pending; // think_due: the network has not seen the newest picture yet; pending: it is at work on it
static UINT64 pending_value;
static bool think_reset;
static float think_skin, think_steady, think_blend;
static bool acc_ok[KEYS];
static ID3D11Texture2D *low_t;
static ID3D11ShaderResourceView *low_s;
static ID3D11RenderTargetView *low_r;
static int layers_now;
static ID3D12Device *dev12;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc[3];
static ID3D12GraphicsCommandList *list;
static ID3D12Fence *fence12;
static ID3D11Fence *fence11;
static ID3D12Resource *depth, *query_back;
static ID3D12QueryHeap *queries;
static UINT64 value, value_of[3], freq;
static int turn, W, H, nw, nh;
static void *feature[LAYERS];
static bool ready;
static Smooth took;
static ID3D11VertexShader *vs, *vs_hold;
static ID3D11Texture2D *hold_t[2];
static ID3D11ShaderResourceView *hold_s[2];
static ID3D11RenderTargetView *hold_r[2];
static int hold_turn;
static ID3D11PixelShader *ps_down, *ps_motion, *ps_mix, *ps_hold, *ps_low, *ps_acc, *ps_btw, *ps_move;
static ID3D11SamplerState *smp;
static ID3D11Buffer *cb;
static ID3D11Device *made_on;

static const char src[] =
    "cbuffer C:register(b0){float4 size;float power;float hud;float sx;float sy;float4 look;float smooth;float3 pad;};Texture2D t0:register(t0);Texture2D t1:register(t1);Texture2D t2:register(t2);Texture2D<uint> map:register(t3);Texture2D<float2> flow:register(t4);Texture2D t5:register(t5);Texture2D t6:register(t6);Texture2D t7:register(t7);SamplerState s:register(s0);"
    "struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};V vs(uint i:SV_VertexID){V o;o.uv=float2((i<<1)&2,i&2);o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);return o;}"
    // the smaller picture: every pixel of the full one counts (four bilinear taps reach 4x4)
    "float4 down(V v):SV_Target{float2 k=0.25/size.zw;float3 c=0;[unroll]for(int i=0;i<4;i++)c+=t0.SampleLevel(s,v.uv+float2(i&1?k.x:-k.x,i&2?k.y:-k.y),0).rgb;return float4(c*0.25,1);}"
    "float3 net(float2 u){return lerp(t7.SampleLevel(s,u,0).rgb,t2.SampleLevel(s,u,0).rgb,look.w);}"
    // skin: 1 where the (slightly blurred) original has its colour
    "float skin(float2 u){float2 k=3/size.zw;float3 c=t1.SampleLevel(s,u,0).rgb*2;[unroll]for(int i=0;i<4;i++)c+=t1.SampleLevel(s,u+float2(i<2?(i?1:-1):0,i<2?0:(i>2?1:-1))*k,0).rgb;c/=6;"
    " float y=dot(c,float3(0.299,0.587,0.114)),b=0.564*(c.b-y)/(y+0.05),r=0.713*(c.r-y)/(y+0.05);return smoothstep(0.045,0.09,r)*(1-smoothstep(0.26,0.36,r))*smoothstep(-0.36,-0.27,b)*(1-smoothstep(-0.05,0.0,b))*smoothstep(0.04,0.1,y);}"
    // What the network changed, steadied: its fine texture is a little different in every picture, which looks like noise. The pictures before
    // are followed along the motion; where the picture itself is another one there, or the change would leave what is around it now, they do not count.
    "float4 acc(V v):SV_Target{float2 k=1/size.zw;float3 o=t1.SampleLevel(s,v.uv,0).rgb,d=net(v.uv)-o,lo=d,hi=d;[unroll]for(int i=0;i<8;i++){float2 u=v.uv+float2(i<3?i-1:i<5?(i==3?-1:1):i-6,i<3?-1:i<5?0:1)*k;float3 n=net(u)-t1.SampleLevel(s,u,0).rgb;lo=min(lo,n);hi=max(hi,n);}"
    " float y=dot(o,float3(0.299,0.587,0.114));float2 p=v.uv+flow.SampleLevel(s,v.uv,0)*k;float4 h=t0.SampleLevel(s,p,0);float ok=pad.x*(1-smoothstep(0.02,0.07,abs(h.a-y)))*(all(p>0)&&all(p<1)?1:0);"
    " return float4(lerp(d,clamp(h.rgb,lo-0.006,hi+0.006),ok),y);}"
    // the change of the picture before, where it is now; where the picture is another one there, less of it
    "float4 move(V v):SV_Target{float2 p=v.uv+flow.SampleLevel(s,v.uv,0)/size.zw;float4 h=t0.SampleLevel(s,p,0);float y=dot(t1.SampleLevel(s,v.uv,0).rgb,float3(0.299,0.587,0.114));return float4(h.rgb*(1-smoothstep(0.06,0.2,abs(h.a-y))),y);}"
    // between two pictures: the change of both, each followed along the motion to where it is then
    "float4 btw(V v):SV_Target{float2 m=flow.SampleLevel(s,v.uv,0)/size.zw;float f=look.w;return float4(lerp(t0.SampleLevel(s,v.uv+f*m,0).rgb,t2.SampleLevel(s,v.uv-(1-f)*m,0).rgb,f),0);}"
    "float4 low(V v):SV_Target{float2 k=1/size.zw;float3 c=0;float m=0;[unroll]for(int y=0;y<4;y++)[unroll]for(int x=0;x<4;x++){float2 u=v.uv+(float2(x,y)-1.5)*2*k;c+=t2.SampleLevel(s,u,0).rgb;if(smooth>0&&((x|y)&1)==0)m+=skin(u);}return float4(c/16,m/4);}"
    "float2 motion(V v):SV_Target{return flow.SampleLevel(s,v.uv,0)*float2(sx,sy);}"
    // Where the HUD is, remembered: what stands still while most of the picture moves. While everything stands still nothing is learned.
    "struct VH{float4 p:SV_Position;float2 uv:TEXCOORD0;nointerpolation float cover:TEXCOORD1;};VH vs_hold(uint i:SV_VertexID){VH o;o.uv=float2((i<<1)&2,i&2);o.p=float4(o.uv*float2(2,-2)+float2(-1,1),0,1);uint w,h;map.GetDimensions(w,h);float n=0;"
    " [loop]for(uint y=0;y<18;y++)[loop]for(uint x=0;x<32;x++)n+=map[int2((x+0.5)*w/32,(y+0.5)*h/18)]?1:0;o.cover=n/576;return o;}"
    "float4 hold(VH v):SV_Target{uint w,h;map.GetDimensions(w,h);int2 p=v.uv*float2(w,h);float m=0;[unroll]for(int y=-2;y<=2;y++)[unroll]for(int x=-2;x<=2;x++)m+=map[clamp(p+int2(x,y)*7,int2(0,0),int2(w,h)-1)]?1:0;"
    " float t=smoothstep(0.08,0.3,m/25),o=t0.SampleLevel(s,v.uv,0).r;float r=v.cover>0.3?0:0.012;return o+(t-o)*r;}"
    // what the network changed, over the full picture; the HUD keeps its colours, with a soft edge
    "float4 mix(V v):SV_Target{float3 o=t0.SampleLevel(s,v.uv,0).rgb,d=t2.SampleLevel(s,v.uv,0).rgb;float k=power;"
    " float4 l4=0;float2 e=8/size.zw;[unroll]for(int j=-1;j<=1;j++)[unroll]for(int i=-1;i<=1;i++)l4+=t6.SampleLevel(s,v.uv+float2(i,j)*e,0)*(2-abs(i))*(2-abs(j));l4/=16;float3 l=l4.rgb;"
    " float m0=smooth>0?max(skin(v.uv),smoothstep(0.35,0.65,l4.a))*smooth:0;" // what lies within skin (a bright spot, a shadow) belongs to it
    " float3 f=(d-l)*look.x*(1-0.9*m0);d=l*look.y;float3 Y=float3(0.2126,0.7152,0.0722);float dl=dot(d,Y);d*=dl<0?1-0.55*m0:1;d+=f;"
    // the skin's own fine texture, taken down a little; edges (eyes, lips, outlines) stay
    " if(m0>0.02){float3 a=0;float w=0;[unroll]for(int q=0;q<8;q++){float2 z;sincos(q*0.785398,z.y,z.x);[unroll]for(int g=1;g<=2;g++){float3 n=t0.SampleLevel(s,v.uv+z*g*3/size.xy,0).rgb;float x=exp(-dot(n-o,n-o)*260);a+=n*x;w+=x;}}o=lerp(o,(a+o)/(w+1),0.7*m0);}"
    " d=lerp(d,dot(d,Y)*(o+0.02)/(dot(o,Y)+0.02),max(max(look.z,m0),0.85*smoothstep(0.4,0.7,dot(o,Y))));" // the same colour, lighter or darker
    " if(hud>0){float m=0;[unroll]for(int y=-1;y<=1;y++)[unroll]for(int x=-1;x<=1;x++)m+=t5.SampleLevel(s,v.uv+float2(x,y)*8/size.xy,0).r;float b=0;[unroll]for(int q=-2;q<=2;q++)[unroll]for(int p=-2;p<=2;p++)b+=t5.SampleLevel(s,v.uv+float2(p,q)*36/size.xy,0).r;"
    " float2 z=abs(v.uv-float2(0.5,0.54))/float2(0.22,0.42);float mid=1-smoothstep(0.85,1.1,max(z.x,z.y));" // the player in the middle is no HUD, however still it stands
    " k*=1-max(smoothstep(0.5,0.75,m/9),smoothstep(0.26,0.38,b/25))*(1-mid);}"
    " return float4(saturate(o+d*k),1);}";
static bool shaders(ID3D11Device *d)
{
    if (made_on == d) return ps_mix != nullptr;
    made_on = d;
    auto make = [&](const char *entry, const char *target) {
        ID3DBlob *b = nullptr, *e = nullptr;
        if (FAILED(D3DCompile(src, sizeof src - 1, nullptr, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &e))) logf("nr: %s", e ? (const char *)e->GetBufferPointer() : "shader");
        release(e);
        return b;
    };
    auto pixel = [&](const char *entry) {
        ID3D11PixelShader *p = nullptr;
        if (ID3DBlob *b = make(entry, "ps_5_0")) { d->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &p); b->Release(); }
        return p;
    };
    if (ID3DBlob *b = make("vs", "vs_5_0")) { d->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs); b->Release(); }
    if (ID3DBlob *b = make("vs_hold", "vs_5_0")) { d->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &vs_hold); b->Release(); }
    ps_hold = pixel("hold");
    ps_low = pixel("low");
    ps_acc = pixel("acc");
    ps_btw = pixel("btw");
    ps_move = pixel("move");
    ps_down = pixel("down");
    ps_motion = pixel("motion");
    ps_mix = pixel("mix");
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    d->CreateSamplerState(&sd, &smp);
    D3D11_BUFFER_DESC b{64, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    d->CreateBuffer(&b, nullptr, &cb);
    return vs && vs_hold && ps_hold && ps_low && ps_acc && ps_btw && ps_move && ps_down && ps_motion && ps_mix && smp && cb;
}
static bool shared(ID3D11Device *d, Shared &x, int w, int h, DXGI_FORMAT f)
{
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w;
    td.Height = (UINT)h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = f;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
    IDXGIResource1 *r = nullptr;
    HANDLE n = nullptr;
    bool ok = SUCCEEDED(d->CreateTexture2D(&td, nullptr, &x.t)) && SUCCEEDED(d->CreateShaderResourceView(x.t, nullptr, &x.s)) && SUCCEEDED(d->CreateRenderTargetView(x.t, nullptr, &x.r)) &&
              SUCCEEDED(x.t->QueryInterface(IID_PPV_ARGS(&r))) && SUCCEEDED(r->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &n)) && SUCCEEDED(dev12->OpenSharedHandle(n, IID_PPV_ARGS(&x.d)));
    if (n) CloseHandle(n);
    release(r);
    return ok;
}
static void drop(Shared &x)
{
    release(x.d);
    release(x.r);
    release(x.s);
    release(x.t);
}
static void turn_over(ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    list->ResourceBarrier(1, &x);
}
static bool idle() // everything that was sent is done
{
    if (!queue || !fence12) return true;
    queue->Signal(fence12, ++value);
    for (ULONGLONG t = GetTickCount64(); fence12->GetCompletedValue() < value; Sleep(1))
        if (GetTickCount64() - t > 3000) return false;
    return true;
}
static void log_line(const char *m, int, int)
{
    if (strstr(m, "rror") || strstr(m, "dlssnr")) logf("nr: ngx %.*s", (int)std::min<size_t>(strcspn(m, "\r\n"), 220), m);
}
static bool ngx_up;
static std::string start_once(ID3D11Device *d, IDXGIAdapter1 *adapter, const std::wstring &data)
{
    CreateDirectoryW(data.c_str(), nullptr);
    if (FAILED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev12)))) return "Direct3D 12";
    D3D12_COMMAND_QUEUE_DESC qd{};
    if (FAILED(dev12->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return "queue";
    for (auto &a : alloc) if (FAILED(dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) return "allocator";
    if (FAILED(dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], nullptr, IID_PPV_ARGS(&list)))) return "list";
    list->Close();
    ID3D11Device5 *d5 = nullptr;
    HANDLE h = nullptr;
    bool ok = SUCCEEDED(d->QueryInterface(IID_PPV_ARGS(&d5))) && SUCCEEDED(d5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11))) && SUCCEEDED(fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h)) && SUCCEEDED(dev12->OpenSharedHandle(h, IID_PPV_ARGS(&fence12)));
    if (h) CloseHandle(h);
    release(d5);
    if (!ok) return "shared fence";
    D3D12_QUERY_HEAP_DESC qh{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 6, 0};
    dev12->CreateQueryHeap(&qh, IID_PPV_ARGS(&queries));
    queue->GetTimestampFrequency(&freq);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 48;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&query_back));
    ngx.m = LoadLibraryW(core().c_str());
    auto get = [&](auto &f, const char *n) { f = ngx.m ? reinterpret_cast<std::decay_t<decltype(f)>>(reinterpret_cast<void *>(GetProcAddress(ngx.m, n))) : nullptr; return f != nullptr; };
    if (!get(ngx.init, "NVSDK_NGX_D3D12_Init_ProjectID") || !get(ngx.alloc, "NVSDK_NGX_D3D12_AllocateParameters") || !get(ngx.caps, "NVSDK_NGX_D3D12_GetCapabilityParameters") || !get(ngx.create, "NVSDK_NGX_D3D12_CreateFeature") || !get(ngx.eval, "NVSDK_NGX_D3D12_EvaluateFeature") ||
        !get(ngx.drop, "NVSDK_NGX_D3D12_ReleaseFeature"))
        return "the driver has no NGX";
    static const wchar_t *paths[1];
    paths[0] = folder.c_str();
    static Info info;
    info = {paths, 1, nullptr, {log_line, 1, false}};
    ngx_up = true; // it keeps what it was given
    Result r = ngx.init("5d0c7a3e-93f4-4b1e-a6f5-2f8f1f0e7b31", 0, "1.0", data.c_str(), dev12, 0x15, &info);
    if (failed(r)) { char t[40]; snprintf(t, 40, "NGX %08x", r); return t; }
    void *cap = nullptr;
    int can = 0;
    if (!failed(ngx.caps(&cap)) && cap) (*(Result(***)(void *, const char *, int *))cap)[GET_I](cap, "DLSSNR.Available", &can);
    if (!can) return "needs RTX 50 and a newer driver";
    if (failed(ngx.alloc(&params)) || !params) return "NGX parameters";
    return "";
}
static std::string start(ID3D11Device *d, IDXGIAdapter1 *adapter)
{
    if (params) return "";
    static int tried = -1;
    static std::string failed_as = "could not be opened";
    if (tried == (int)choice || (ngx_up && !params)) return failed_as;
    tried = (int)choice;
    folder = nr_file().substr(0, nr_file().find_last_of(L"\\/"));
    static std::wstring data = data_dir() + L"\\ngx";
    failed_as = start_once(d, adapter, data);
    if (!failed_as.empty() && !ngx_up) // what was made is not kept: another file may be tried
    {
        params = nullptr;
        release(query_back);
        release(queries);
        release(fence12);
        release(fence11);
        release(list);
        for (auto &a : alloc) release(a);
        release(queue);
        release(dev12);
    }
    return failed_as;
}
static bool send()
{
    list->Close();
    ID3D12CommandList *l[1] = {list};
    queue->ExecuteCommandLists(1, l);
    return true;
}
void nr_close()
{
    AcquireSRWLockExclusive(&lock);
    ready = false;
    if (!idle()) // still at work after 3 s: what it uses is left alone
    {
        logf("nr: the graphics card does not answer");
        for (auto &f : feature) f = nullptr;
        W = H = 0;
        ReleaseSRWLockExclusive(&lock);
        return;
    }
    for (auto &f : feature) if (f) { ngx.drop(f); f = nullptr; }
    drop(in);
    drop(mv);
    for (auto &o : outs) drop(o);
    for (int i = 0; i <= KEYS; i++) { release(acc_r[i]); release(acc_s[i]); release(acc_t[i]); }
    for (int i = 0; i < KEYS; i++) { release(mvk_s[i]); release(mvk_t[i]); acc_ok[i] = false; }
    for (int i = 0; i < 2; i++) { release(net_r[i]); release(net_s[i]); release(net_t[i]); }
    net_ok = think_due = pending = false;
    release(low_r);
    release(low_s);
    release(low_t);
    for (int i = 0; i < 2; i++) { release(hold_r[i]); release(hold_s[i]); release(hold_t[i]); }
    release(depth);
    W = H = 0;
    took.clear();
    ReleaseSRWLockExclusive(&lock);
}
bool nr_ready(int w, int h) { return ready && w == W && h == H; }
double nr_ms() { return ready ? took.get() : 0; }
std::string nr_open(ID3D11Device *d, IDXGIAdapter1 *adapter, int w, int h, int lines, int style, int layers)
{
    nr_close();
    if (nr_file().empty()) return "no runtime";
    DXGI_ADAPTER_DESC1 ad{};
    adapter->GetDesc1(&ad);
    if (ad.VendorId != 0x10de) return "NVIDIA only";
    AcquireSRWLockExclusive(&lock);
    std::string why = start(d, adapter);
    if (why.empty() && !shaders(d)) why = "shaders";
    nh = std::min(h, lines) & ~1;
    nw = (int)((int64_t)w * nh / h + 1) & ~1;
    if (why.empty() && (!shared(d, in, nw, nh, DXGI_FORMAT_R16G16B16A16_FLOAT) || !shared(d, mv, nw, nh, DXGI_FORMAT_R16G16_FLOAT))) why = "shared pictures";
    layers = std::min<int>(LAYERS, std::max(1, layers));
    for (int i = 0; why.empty() && i < layers; i++)
        if (!shared(d, outs[i], nw, nh, DXGI_FORMAT_R16G16B16A16_FLOAT)) why = "shared pictures";
    layers_now = layers;
    for (int i = 0; why.empty() && i < 2; i++)
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)w / 4;
        td.Height = (UINT)h / 4;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16_UNORM;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        std::vector<BYTE> zero((size_t)td.Width * td.Height * 2);
        D3D11_SUBRESOURCE_DATA sd{zero.data(), td.Width * 2, 0};
        if (FAILED(d->CreateTexture2D(&td, &sd, &hold_t[i])) || FAILED(d->CreateShaderResourceView(hold_t[i], nullptr, &hold_s[i])) || FAILED(d->CreateRenderTargetView(hold_t[i], nullptr, &hold_r[i]))) why = "mask";
    }
    if (why.empty())
    {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)w;
        td.Height = (UINT)h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UINT;
        td.SampleDesc.Count = 1;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        D3D11_TEXTURE2D_DESC ld = td;
        ld.Width = (UINT)std::max(1, nw / 8);
        ld.Height = (UINT)std::max(1, nh / 8);
        ld.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        D3D11_TEXTURE2D_DESC ad = ld;
        ad.Width = (UINT)nw;
        ad.Height = (UINT)nh;
        for (int i = 0; i <= KEYS; i++) if (FAILED(d->CreateTexture2D(&ad, nullptr, &acc_t[i])) || FAILED(d->CreateShaderResourceView(acc_t[i], nullptr, &acc_s[i])) || FAILED(d->CreateRenderTargetView(acc_t[i], nullptr, &acc_r[i]))) why = "mask";
        for (int i = 0; i < 2; i++) if (FAILED(d->CreateTexture2D(&ad, nullptr, &net_t[i])) || FAILED(d->CreateShaderResourceView(net_t[i], nullptr, &net_s[i])) || FAILED(d->CreateRenderTargetView(net_t[i], nullptr, &net_r[i]))) why = "mask";
        ad.Format = DXGI_FORMAT_R16G16_FLOAT;
        for (int i = 0; i < KEYS; i++) if (FAILED(d->CreateTexture2D(&ad, nullptr, &mvk_t[i])) || FAILED(d->CreateShaderResourceView(mvk_t[i], nullptr, &mvk_s[i]))) why = "mask";
        if (FAILED(d->CreateTexture2D(&ld, nullptr, &low_t)) || FAILED(d->CreateShaderResourceView(low_t, nullptr, &low_s)) || FAILED(d->CreateRenderTargetView(low_t, nullptr, &low_r))) why = "mask";
    }
    if (why.empty())
    {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = (UINT)nw;
        rd.Height = (UINT)nh;
        rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R32_FLOAT;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE cv{};
        cv.Format = DXGI_FORMAT_R32_FLOAT;
        cv.Color[0] = 0.5f; // a picture from a cable has no depth: the same everywhere
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        ID3D12DescriptorHeap *heap = nullptr;
        if (FAILED(dev12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&depth))) || FAILED(dev12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap)))) why = "depth";
        else
        {
            D3D12_CPU_DESCRIPTOR_HANDLE at = heap->GetCPUDescriptorHandleForHeapStart();
            dev12->CreateRenderTargetView(depth, nullptr, at);
            alloc[0]->Reset();
            list->Reset(alloc[0], nullptr);
            list->ClearRenderTargetView(at, cv.Color, 0, nullptr);
            turn_over(depth, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            auto ui = [](const char *n, unsigned v) { put(params, SET_UI, n, v); };
            auto fl = [](const char *n, float v) { put(params, SET_F, n, v); };
            for (const char *n : {"Width", "OutWidth", "DLSSNR.Width"}) ui(n, (unsigned)nw);
            for (const char *n : {"Height", "OutHeight", "DLSSNR.Height"}) ui(n, (unsigned)nh);
            ui("PerfQualityValue", 2);
            ui("CreationNodeMask", 1);
            ui("VisibilityNodeMask", 1);
            ui("DLSSNR.Enabled", 1);
            ui("DLSSNR.Hint.Render.Preset", 0);
            ui("DLSSNR.Style", (unsigned)style);
            ui("DLSSNR.UseAutoMask", 1); // people are found by the network, so that their skin can be held back
            fl("DLSSNR.SkinStructureStrength", -1);
            ui("DLSSNR.UICorrection", 0);
            fl("DLSSNR.Intensity", 1);
            fl("DLSSNR.LocalStructureStrength", 1);
            fl("DLSSNR.LocalToneStrength", 1);
            Result r = 1;
            for (int i = 0; i < layers && !failed(r); i++) r = ngx.create(list, 18, params, &feature[i]);
            send();
            if (!idle()) why = "the graphics card does not answer";
            else if (failed(r) || !feature[0]) { char t[40]; snprintf(t, 40, "NGX create %08x", r); why = t; }
        }
        release(heap);
    }
    logf("nr: %dx%d for %dx%d style %d layers %d %s", nw, nh, w, h, style, layers, why.empty() ? "ready" : why.c_str());
    if (why.empty())
    {
        W = w;
        H = h;
        for (auto &v : value_of) v = 0;
        ready = true;
    }
    ReleaseSRWLockExclusive(&lock);
    return why;
}
// One full-screen pass.
static void draw_pass(ID3D11DeviceContext *c, ID3D11PixelShader *p, ID3D11RenderTargetView *to, int w, int h, std::initializer_list<ID3D11ShaderResourceView *> s)
{
    ID3D11ShaderResourceView *none[8] = {};
    ID3D11RenderTargetView *nort = nullptr;
    D3D11_VIEWPORT vp = {0, 0, (float)w, (float)h, 0, 1};
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(p == ps_hold ? vs_hold : vs, nullptr, 0);
    c->PSSetShader(p, nullptr, 0);
    c->PSSetConstantBuffers(0, 1, &cb);
    c->PSSetSamplers(0, 1, &smp);
    c->OMSetRenderTargets(1, &to, nullptr);
    c->PSSetShaderResources(0, (UINT)s.size(), s.begin());
    c->RSSetState(nullptr);
    c->RSSetViewports(1, &vp);
    c->Draw(3, 0);
    c->OMSetRenderTargets(1, &nort, nullptr);
    c->PSSetShaderResources(0, 8, none);
}
bool nr_between(ID3D11DeviceContext *c, ID3D11ShaderResourceView *made, ID3D11RenderTargetView *target, int a, int b, float f, bool hud, const NrLook &look)
{
    if (!TryAcquireSRWLockExclusive(&lock)) return false;
    bool ok = ready && acc_ok[a] && acc_ok[b];
    if (ok)
    {
        struct { float w, h, pw, ph, power, hud, sx, sy, detail, tone, keep, f, smooth, pad[3]; } k = {(float)W, (float)H, (float)nw, (float)nh, look.power, hud ? 1.f : 0.f, 0, 0, look.detail, look.tone, look.keep, f, look.smooth, {}};
        c->UpdateSubresource(cb, 0, nullptr, &k, 0, 0);
        draw_pass(c, ps_btw, acc_r[KEYS], nw, nh, {acc_s[a], nullptr, acc_s[b], nullptr, mvk_s[b]});
        draw_pass(c, ps_low, low_r, std::max(1, nw / 8), std::max(1, nh / 8), {nullptr, made, acc_s[KEYS]});
        draw_pass(c, ps_mix, target, W, H, {made, made, acc_s[KEYS], nullptr, nullptr, hold_s[hold_turn], low_s});
    }
    ReleaseSRWLockExclusive(&lock);
    return ok;
}
struct Cb { float w, h, pw, ph, power, hud, sx, sy, detail, tone, keep, blend, smooth, pad[3]; };
// The network looks at `in` (with the motion in mv). The D3D12 queue waits for the passes before it.
static Result think(ID3D11DeviceContext4 *c)
{
    think_due = false;
    c->Signal(fence11, ++value);
    c->Flush();
    queue->Wait(fence12, value);
    alloc[turn]->Reset();
    list->Reset(alloc[turn], nullptr);
    const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, common = D3D12_RESOURCE_STATE_COMMON, write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    turn_over(in.d, common, read);
    turn_over(mv.d, common, read);
    for (int i = 0; i < layers_now; i++) turn_over(outs[i].d, common, write);
    put(params, SET_D3D12, "DLSSNR.Depth", (ID3D12Resource *)depth);
    put(params, SET_D3D12, "DLSSNR.MVec", (ID3D12Resource *)mv.d);
    auto ui = [](const char *n, unsigned v) { put(params, SET_UI, n, v); };
    ui("DLSSNR.Enabled", 1);
    ui("DLSSNR.Reset", think_reset);
    ui("DLSSNR.DepthInverted", 0);
    for (const char *n : {"DLSSNR.ColorSubrect", "DLSSNR.OutputSubrect", "DLSSNR.DepthSubrect", "DLSSNR.MVecSubrect"})
    {
        std::string t = n;
        ui((t + "BaseX").c_str(), 0);
        ui((t + "BaseY").c_str(), 0);
        ui((t + "Width").c_str(), (unsigned)nw);
        ui((t + "Height").c_str(), (unsigned)nh);
    }
    ui("DLSSNR.UseAutoMask", 1);
    put(params, SET_F, "DLSSNR.SkinStructureStrength", think_skin);
    put(params, SET_F, "DLSSNR.MVecScaleX", 1.f);
    put(params, SET_F, "DLSSNR.MVecScaleY", 1.f);
    if (queries) list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)turn * 2);
    Result r = 1;
    int read_now = 0; // the first ones are read already
    for (int i = 0; i < layers_now && !failed(r); i++)
    {
        if (i) { turn_over(outs[i - 1].d, write, read); read_now = i; }
        put(params, SET_D3D12, "DLSSNR.Color", (ID3D12Resource *)(i ? outs[i - 1].d : in.d));
        put(params, SET_D3D12, "DLSSNR.Output", (ID3D12Resource *)outs[i].d);
        r = ngx.eval(list, feature[i], params, nullptr);
    }
    if (queries)
    {
        list->EndQuery(queries, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)turn * 2 + 1);
        list->ResolveQueryData(queries, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)turn * 2, 2, query_back, (UINT64)turn * 16);
    }
    turn_over(in.d, read, common);
    turn_over(mv.d, read, common);
    for (int i = 0; i < layers_now; i++) turn_over(outs[i].d, i < read_now ? read : write, common);
    send();
    queue->Signal(fence12, ++value);
    value_of[turn] = pending_value = value;
    turn = (turn + 1) % 3;
    if (value_of[turn] && freq && query_back) // the oldest one is done
    {
        UINT64 *t = nullptr;
        D3D12_RANGE a{(SIZE_T)turn * 16, (SIZE_T)turn * 16 + 16}, n{};
        if (fence12->GetCompletedValue() >= value_of[turn] && SUCCEEDED(query_back->Map(0, &a, (void **)&t)))
        {
            double ms = 1000.0 * (double)(t[turn * 2 + 1] - t[turn * 2]) / (double)freq;
            if (ms > 0 && ms < 200) took.add(ms);
            query_back->Unmap(0, &n);
        }
    }
    if (failed(r))
    {
        static int said;
        if (said++ < 5) logf("nr: evaluate %08x", r);
        ready = false;
    }
    else pending = true;
    return r;
}
// What it said: the change, steadied with the one before.
static void collect(ID3D11DeviceContext4 *c)
{
    pending = false;
    c->Wait(fence11, pending_value);
    Cb k = {(float)W, (float)H, (float)nw, (float)nh, 1, 0, 0, 0, 1, 1, 0, layers_now > 1 ? think_blend : 1.f, 0, {think_reset || !net_ok ? 0.f : think_steady, 0, 0}};
    c->UpdateSubresource(cb, 0, nullptr, &k, 0, 0);
    ID3D11ShaderResourceView *last = outs[layers_now - 1].s, *first = outs[std::max(0, layers_now - 2)].s;
    int to = net_turn ^ 1;
    draw_pass(c, ps_acc, net_r[to], nw, nh, {net_ok ? net_s[net_turn] : nullptr, in.s, last, nullptr, mv.s, nullptr, nullptr, first});
    net_turn = to;
    net_ok = true;
}
void nr_think(ID3D11DeviceContext4 *c)
{
    if (!TryAcquireSRWLockExclusive(&lock)) return;
    if (ready && think_due && fence12->GetCompletedValue() >= value_of[turn]) think(c);
    ReleaseSRWLockExclusive(&lock);
}
bool nr_run(ID3D11DeviceContext4 *c, ID3D11ShaderResourceView *full, ID3D11ShaderResourceView *motion, int mw, int mh, ID3D11ShaderResourceView *hud, ID3D11RenderTargetView *target, bool reset, const NrLook &look, int key)
{
    if (!TryAcquireSRWLockExclusive(&lock)) return false; // being set up
    acc_ok[key] = false;
    bool ok = ready && fence12->GetCompletedValue() >= value_of[turn]; // three under way: the card cannot keep up
    if (ok && think_due) think(c); // the picture before was never looked at: now, or the motion would skip one
    if (ok && pending) collect(c);
    ok = ok && ready;
    if (ok)
    {
        Cb k = {(float)W, (float)H, (float)nw, (float)nh, look.power, hud ? 1.f : 0.f, mw > 0 ? (float)nw / (float)mw : 0.f, mh > 0 ? (float)nh / (float)mh : 0.f, look.detail, look.tone, look.keep, 1, look.smooth, {}};
        c->UpdateSubresource(cb, 0, nullptr, &k, 0, 0);
        draw_pass(c, ps_down, in.r, nw, nh, {full});
        if (motion) draw_pass(c, ps_motion, mv.r, nw, nh, {nullptr, nullptr, nullptr, nullptr, motion});
        else { const float zero[4] = {}; c->ClearRenderTargetView(mv.r, zero); }
        c->CopyResource(mvk_t[key], mv.t);
        think_due = true;
        think_reset = reset || !motion;
        think_skin = look.skin;
        think_steady = look.steady;
        think_blend = look.blend;
        if (look.fast && net_ok && !think_reset) draw_pass(c, ps_move, acc_r[key], nw, nh, {net_s[net_turn], in.s, nullptr, nullptr, mv.s});
        else // waited for
        {
            if (!failed(think(c)) && pending)
            {
                collect(c);
                c->CopyResource(acc_t[key], net_t[net_turn]);
                c->UpdateSubresource(cb, 0, nullptr, &k, 0, 0);
            }
            else ok = false;
        }
        if (ok)
        {
            if (hud)
            {
                hold_turn ^= 1;
                ID3D11ShaderResourceView *none = nullptr;
                c->VSSetShaderResources(3, 1, &hud);
                draw_pass(c, ps_hold, hold_r[hold_turn], W / 4, H / 4, {hold_s[hold_turn ^ 1], nullptr, nullptr, hud});
                c->VSSetShaderResources(3, 1, &none);
            }
            acc_ok[key] = true;
            draw_pass(c, ps_low, low_r, std::max(1, nw / 8), std::max(1, nh / 8), {nullptr, in.s, acc_s[key]});
            draw_pass(c, ps_mix, target, W, H, {full, in.s, acc_s[key], nullptr, nullptr, hold_s[hold_turn], low_s});
        }
    }
    ReleaseSRWLockExclusive(&lock);
    return ok;
}
