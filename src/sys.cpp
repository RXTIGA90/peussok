// SPDX-License-Identifier: GPL-3.0-only
#include "sys.h"
#include "video.h"
#include <algorithm>
#include <dxgi1_6.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>

static SysInfo info;
static SysLive live;
static SRWLOCK lock = SRWLOCK_INIT;
static HANDLE thread, wake;
static volatile LONG want, stopping;

template <class F> static void entry(F &f, HMODULE m, const char *n) { f = reinterpret_cast<F>(reinterpret_cast<void *>(GetProcAddress(m, n))); }

// ---- NVIDIA: NVML comes with the driver; only the few entry points used here are declared ----
struct Nvml
{
    HMODULE dll = nullptr;
    void *dev = nullptr;
    struct Util { unsigned gpu, mem; };
    struct Mem { unsigned long long total, free, used; };
    int (*init)() = nullptr;
    int (*count)(unsigned *) = nullptr;
    int (*handle)(unsigned, void **) = nullptr;
    int (*name)(void *, char *, unsigned) = nullptr;
    int (*util)(void *, Util *) = nullptr;
    int (*clock)(void *, int, unsigned *) = nullptr;
    int (*max_clock)(void *, int, unsigned *) = nullptr;
    int (*power)(void *, unsigned *) = nullptr;
    int (*limit)(void *, unsigned *) = nullptr;
    int (*temp)(void *, int, unsigned *) = nullptr;
    int (*fan)(void *, unsigned *) = nullptr;
    int (*mem)(void *, Mem *) = nullptr;
    int (*gen)(void *, unsigned *) = nullptr;
    int (*width)(void *, unsigned *) = nullptr;
    int (*pstate)(void *, int *) = nullptr;
    int (*reasons)(void *, unsigned long long *) = nullptr;
    void open(const std::wstring &gpu)
    {
        dll = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll) return;
        entry(init, dll, "nvmlInit_v2");
        entry(count, dll, "nvmlDeviceGetCount_v2");
        entry(handle, dll, "nvmlDeviceGetHandleByIndex_v2");
        entry(name, dll, "nvmlDeviceGetName");
        entry(util, dll, "nvmlDeviceGetUtilizationRates");
        entry(clock, dll, "nvmlDeviceGetClockInfo");
        entry(max_clock, dll, "nvmlDeviceGetMaxClockInfo");
        entry(power, dll, "nvmlDeviceGetPowerUsage");
        entry(limit, dll, "nvmlDeviceGetEnforcedPowerLimit");
        entry(temp, dll, "nvmlDeviceGetTemperature");
        entry(fan, dll, "nvmlDeviceGetFanSpeed");
        entry(mem, dll, "nvmlDeviceGetMemoryInfo");
        entry(gen, dll, "nvmlDeviceGetCurrPcieLinkGeneration");
        entry(width, dll, "nvmlDeviceGetCurrPcieLinkWidth");
        entry(pstate, dll, "nvmlDeviceGetPerformanceState");
        entry(reasons, dll, "nvmlDeviceGetCurrentClocksThrottleReasons");
        unsigned n = 0;
        if (!init || !count || !handle || !name || init() || count(&n)) return;
        for (unsigned i = 0; i < n; i++)
        {
            void *d = nullptr;
            char t[96] = "";
            if (handle(i, &d) || name(d, t, sizeof t)) continue;
            std::wstring w(t, t + strlen(t));
            if (gpu.find(w) != std::wstring::npos || w.find(gpu) != std::wstring::npos) dev = d;
        }
    }
    void read(SysLive &s)
    {
        unsigned v = 0;
        int p = 0;
        Util u{};
        Mem m{};
        unsigned long long r = 0;
        if (util && !util(dev, &u)) s.gpu = (float)u.gpu;
        if (clock && !clock(dev, 0, &v)) s.gpu_mhz = (float)v;
        if (clock && !clock(dev, 2, &v)) s.mem_mhz = (float)v;
        if (max_clock && !max_clock(dev, 0, &v)) s.gpu_max_mhz = (float)v;
        if (power && !power(dev, &v)) s.gpu_w = (float)v / 1000.f;
        if (limit && !limit(dev, &v)) s.gpu_limit_w = (float)v / 1000.f;
        if (temp && !temp(dev, 0, &v)) s.gpu_temp = (float)v;
        if (fan && !fan(dev, &v)) s.gpu_fan = (float)v;
        if (mem && !mem(dev, &m)) { s.vram_used = (float)((double)m.used / 1073741824.0); s.vram_total = (float)((double)m.total / 1073741824.0); }
        if (gen && !gen(dev, &v)) s.pcie_gen = (float)v;
        if (width && !width(dev, &v)) s.pcie_width = (float)v;
        if (pstate && !pstate(dev, &p)) s.pstate = (float)p;
        if (reasons && !reasons(dev, &r))
            s.limit = r & 0x60 ? tr(L"온도 제한", L"Temperature limit") : r & 0x84 ? tr(L"전력 제한", L"Power limit") : r & 1 ? tr(L"쉬는 중", L"Idle") : L"";
    }
};
static Nvml nvml;

// ---- every other maker: the numbers Task Manager shows (D3DKMT) ----
struct KmAdapter { LUID luid; UINT handle; };
struct KmQuery { UINT handle; int type; void *data; UINT size; };
struct KmPerf { ULONG index; ULONGLONG mem, mem_max, mem_oc, bandwidth, pcie; ULONG fan, power, temp; UCHAR state; };
struct KmNode { ULONG node, index; ULONGLONG freq, max, oc; ULONG volt, volt_max, volt_oc; ULONGLONG latency; };
static UINT km_handle;
static LONG(WINAPI *km_query)(KmQuery *);
static void km_open(LUID l)
{
    HMODULE g = GetModuleHandleW(L"gdi32.dll");
    LONG(WINAPI *open)(KmAdapter *) = nullptr;
    entry(open, g, "D3DKMTOpenAdapterFromLuid");
    entry(km_query, g, "D3DKMTQueryAdapterInfo");
    KmAdapter a = {l, 0};
    if (open && km_query && open(&a) >= 0) km_handle = a.handle;
}
static void km_read(SysLive &s)
{
    if (!km_handle) return;
    KmPerf p{};
    KmQuery q = {km_handle, 62, &p, sizeof p};
    if (km_query(&q) >= 0)
    {
        if (p.temp) s.gpu_temp = (float)p.temp / 10.f;
        if (p.power) s.gpu_power_pct = (float)p.power / 10.f;
        if (p.mem) s.mem_mhz = (float)((double)p.mem / 1e6);
    }
    KmNode n{};
    q = {km_handle, 61, &n, sizeof n};
    if (km_query(&q) >= 0 && n.freq) { s.gpu_mhz = (float)((double)n.freq / 1e6); s.gpu_max_mhz = (float)((double)n.max / 1e6); }
}

// ---- performance counters ----
static PDH_HQUERY pdh;
static PDH_HCOUNTER c_freq, c_power, c_gpu;
static void pdh_open(bool gpu, LUID l)
{
    if (PdhOpenQueryW(nullptr, 0, &pdh) != ERROR_SUCCESS) { pdh = nullptr; return; }
    if (PdhAddEnglishCounterW(pdh, L"\\Processor Information(_Total)\\Actual Frequency", 0, &c_freq) != ERROR_SUCCESS) c_freq = nullptr;
    if (PdhAddEnglishCounterW(pdh, L"\\Energy Meter(*)\\Power", 0, &c_power) != ERROR_SUCCESS) c_power = nullptr;
    wchar_t t[160];
    swprintf(t, 160, L"\\GPU Engine(*luid_0x%08lX_0x%08lX*)\\Utilization Percentage", (unsigned long)l.HighPart, (unsigned long)l.LowPart);
    if (!gpu || PdhAddEnglishCounterW(pdh, t, 0, &c_gpu) != ERROR_SUCCESS) c_gpu = nullptr;
    PdhCollectQueryData(pdh);
}
template <class F> static void pdh_each(PDH_HCOUNTER c, F f)
{
    static std::vector<BYTE> buf;
    DWORD size = 0, n = 0;
    if (!c || PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &n, nullptr) != (PDH_STATUS)PDH_MORE_DATA) return;
    buf.resize(size);
    auto *a = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W *>(buf.data());
    if (PdhGetFormattedCounterArrayW(c, PDH_FMT_DOUBLE, &size, &n, a) != ERROR_SUCCESS) return;
    for (DWORD i = 0; i < n; i++)
        if (a[i].FmtValue.CStatus == ERROR_SUCCESS) f(a[i].szName, a[i].FmtValue.doubleValue);
}
static void pdh_read(SysLive &s)
{
    if (!pdh || PdhCollectQueryData(pdh) != ERROR_SUCCESS) return;
    PDH_FMT_COUNTERVALUE v;
    if (c_freq && PdhGetFormattedCounterValue(c_freq, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS) s.cpu_mhz = (float)v.doubleValue;
    double pkg = 0;
    pdh_each(c_power, [&](const wchar_t *n, double w) {
        std::wstring t = n;
        for (auto &ch : t) ch = (wchar_t)towlower(ch);
        if (t.find(L"_pkg") != std::wstring::npos) pkg += w;
    });
    if (pkg > 0) s.cpu_w = (float)(pkg / 1000.0);
    if (!c_gpu) return;
    double kinds[4] = {}; // 3D, compute, copy, other: the busier of the first two is what counts
    pdh_each(c_gpu, [&](const wchar_t *n, double p) { kinds[wcsstr(n, L"engtype_3D") ? 0 : wcsstr(n, L"ompute") ? 1 : wcsstr(n, L"Copy") ? 2 : 3] += p; });
    s.gpu = (float)std::min(100.0, std::max(kinds[0], kinds[1]));
}

// ---- fixed facts ----
static std::wstring reg_text(const wchar_t *key, const wchar_t *value)
{
    wchar_t t[256] = L"";
    DWORD n = sizeof t;
    RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ, nullptr, t, &n);
    std::wstring s = t;
    while (!s.empty() && s.back() == L' ') s.pop_back();
    return s;
}
// Memory speed and number of modules from the firmware table (SMBIOS type 17).
static void memory_modules(int &speed, int &modules)
{
    DWORD fw = GetSystemFirmwareTable(0x52534D42, 0, nullptr, 0); // "RSMB"
    std::vector<BYTE> t(fw);
    if (fw <= 8 || GetSystemFirmwareTable(0x52534D42, 0, t.data(), fw) != fw) return;
    for (size_t p = 8; p + 4 < t.size();)
    {
        BYTE type = t[p], len = t[p + 1];
        if (len < 4 || type == 127) break;
        if (type == 17 && len >= 0x22 && p + 0x22 < t.size())
        {
            int size = t[p + 0x0C] | t[p + 0x0D] << 8, conf = t[p + 0x20] | t[p + 0x21] << 8, rated = t[p + 0x15] | t[p + 0x16] << 8;
            if (size) { modules++; speed = std::max(speed, conf ? conf : rated); }
        }
        p += len;
        while (p + 1 < t.size() && (t[p] || t[p + 1])) p++;
        p += 2;
    }
}
static void read_facts(IDXGIAdapter1 *a)
{
    const wchar_t *cpu = L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", *nt = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    info.cpu = reg_text(cpu, L"ProcessorNameString");
    DWORD mhz = 0, n = sizeof mhz;
    RegGetValueW(HKEY_LOCAL_MACHINE, cpu, L"~MHz", RRF_RT_REG_DWORD, nullptr, &mhz, &n);
    info.cpu_base_mhz = (int)mhz;
    info.threads = (int)GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    DWORD size = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &size);
    std::vector<BYTE> b(size);
    auto *cores = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(b.data());
    if (size && GetLogicalProcessorInformationEx(RelationProcessorCore, cores, &size))
        for (DWORD o = 0; o < size; o += reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(&b[o])->Size) info.cores++;
    int speed = 0, modules = 0;
    memory_modules(speed, modules);
    ULONGLONG kb = 0;
    GetPhysicallyInstalledSystemMemory(&kb);
    wchar_t r[96];
    if (speed) swprintf(r, 96, L"%.0f GB · %d MT/s · %d%ls", (double)kb / 1048576.0, speed, modules, tr(L"개", L" modules"));
    else swprintf(r, 96, L"%.0f GB", (double)kb / 1048576.0);
    info.ram = r;
    std::wstring build = reg_text(nt, L"CurrentBuild"), rel = reg_text(nt, L"DisplayVersion");
    info.os = (_wtoi(build.c_str()) >= 22000 ? L"Windows 11 " : L"Windows 10 ") + rel + L" (" + build + L")";
    if (!a) return;
    DXGI_ADAPTER_DESC1 d;
    a->GetDesc1(&d);
    info.gpu = d.Description;
    info.vram_mb = (int)(d.DedicatedVideoMemory >> 20);
    LARGE_INTEGER v{};
    if (FAILED(a->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v))) return;
    wchar_t s[64];
    int c = HIWORD(v.LowPart), e = LOWORD(v.LowPart);
    if (d.VendorId == 0x10DE) { int k = (c % 10) * 10000 + e; swprintf(s, 64, L"%d.%02d", k / 100, k % 100); }
    else swprintf(s, 64, L"%d.%d.%d.%d", HIWORD(v.HighPart), LOWORD(v.HighPart), c, e);
    info.driver = s;
}

static DWORD WINAPI sys_main(void *)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
    IDXGIAdapter1 *a = video_adapter();
    IDXGIAdapter3 *a3 = nullptr;
    LUID luid{};
    if (a)
    {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        luid = d.AdapterLuid;
        a->QueryInterface(IID_PPV_ARGS(&a3));
    }
    bool opened = false;
    ULONGLONG idle0 = 0, total0 = 0, app0 = 0;
    auto big = [](const FILETIME &f) { return (ULONGLONG)f.dwHighDateTime << 32 | f.dwLowDateTime; };
    while (!stopping)
    {
        WaitForSingleObject(wake, want ? 1000 : INFINITE);
        if (stopping) break;
        if (!want) continue;
        if (!opened)
        {
            opened = true;
            nvml.open(info.gpu);
            if (!nvml.dev) km_open(luid);
            pdh_open(!nvml.dev, luid);
            logf("sys: %s", nvml.dev ? "NVML" : km_handle ? "D3DKMT" : "counters only");
        }
        SysLive s;
        FILETIME i, k, u, c, e, pk, pu;
        if (GetSystemTimes(&i, &k, &u) && GetProcessTimes(GetCurrentProcess(), &c, &e, &pk, &pu))
        {
            ULONGLONG idle = big(i), total = big(k) + big(u), app = big(pk) + big(pu);
            if (total0 && total > total0)
            {
                s.cpu = 100.f * (1.f - (float)(idle - idle0) / (float)(total - total0));
                s.cpu_app = 100.f * (float)(app - app0) / (float)(total - total0);
            }
            idle0 = idle;
            total0 = total;
            app0 = app;
        }
        MEMORYSTATUSEX m = {sizeof m, 0, 0, 0, 0, 0, 0, 0, 0};
        if (GlobalMemoryStatusEx(&m)) { s.ram_total = (float)((double)m.ullTotalPhys / 1073741824.0); s.ram_used = (float)((double)(m.ullTotalPhys - m.ullAvailPhys) / 1073741824.0); }
        PROCESS_MEMORY_COUNTERS pm{};
        pm.cb = sizeof pm;
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof pm)) s.ram_app = (float)((double)pm.WorkingSetSize / 1073741824.0);
        pdh_read(s);
        if (nvml.dev) nvml.read(s);
        else km_read(s);
        DXGI_QUERY_VIDEO_MEMORY_INFO q{};
        if (a3 && SUCCEEDED(a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &q)))
        {
            s.vram_app = (float)((double)q.CurrentUsage / 1073741824.0);
            if (s.vram_total < 0) s.vram_total = (float)(info.vram_mb / 1024.0);
        }
        AcquireSRWLockExclusive(&lock);
        live = s;
        ReleaseSRWLockExclusive(&lock);
    }
    release(a3);
    return 0;
}
void sys_start()
{
    read_facts(video_adapter());
    wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread = CreateThread(nullptr, 0, sys_main, nullptr, 0, nullptr);
}
void sys_stop()
{
    stopping = 1;
    if (wake) SetEvent(wake);
    if (thread) WaitForSingleObject(thread, 2000);
}
void sys_want(bool on)
{
    if (InterlockedExchange(&want, on) != (LONG)on && wake) SetEvent(wake);
}
const SysInfo &sys_info() { return info; }
SysLive sys_live()
{
    AcquireSRWLockShared(&lock);
    SysLive s = live;
    ReleaseSRWLockShared(&lock);
    return s;
}
