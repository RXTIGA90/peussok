// SPDX-License-Identifier: GPL-3.0-only
#include "engine.h"

const ModelInfo models[] = {
    {9, FAMILY_VFG, L"NVIDIA VFG", L"", 4, 5, true, true},
    {12, FAMILY_DLSSG, L"NVIDIA DLSS FG", L"engines\\dlssg", 5, 4, true, false},
    {18, FAMILY_AMDFG, L"AMD FSR FG", L"engines\\amdfg", 5, 1, false, false},
    {5, FAMILY_FSR3, L"FSR 3", L"engines\\fsr3", 5, 3, false, false},
    {0, FAMILY_RIFE, L"RIFE 4.6", L"model", 4, 4, false, true},
    {11, FAMILY_RIFE, L"RIFE 4.15 lite", L"rife415lite", 2, 4, false, true},
    {100, FAMILY_EXTRA, tr(L"저지연 외삽 (시험)", L"Extrapolation (trial)"), L"", 5, 2, false, true},
};
const int model_count = sizeof models / sizeof models[0] - 1; // the last one is for tests only (its motion search fills the lines that are not there yet)
const ModelInfo *model_by_id(int id)
{
    for (auto &m : models)
        if (m.id == id) return &m;
    return &models[4];
}

static HMODULE dll;
std::wstring Engine::folder()
{
    wchar_t v[1024];
    DWORD n = GetEnvironmentVariableW(L"PKAPP_ENGINE", v, 1024);
    return n && n < 1024 ? std::wstring(v) : exe_dir() + L"\\engine";
}
bool Engine::load_dll()
{
    if (dll) return true;
    std::wstring f = folder();
    // RIFE and FSR 3 import vulkan-1.dll: the graphics driver's copy first, then the copy in the engine folder.
    if (!LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32))
        LoadLibraryExW((f + L"\\vulkan\\vulkan-1.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    dll = LoadLibraryExW((f + L"\\peussok_engine.dll").c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    logf("engine: dll %s (%lu)", dll ? "loaded" : "failed", dll ? 0 : GetLastError());
    return dll != nullptr;
}
template <class F> static bool get(F &f, const std::string &name)
{
    f = reinterpret_cast<F>(reinterpret_cast<void *>(GetProcAddress(dll, name.c_str())));
    return f != nullptr;
}

std::string Engine::open(const ModelInfo &m, int w, int h, int flow)
{
    close();
    if (m.family == FAMILY_EXTRA) return "";
    if (!load_dll()) return "peussok_engine.dll";
    const std::string prefix = m.family == FAMILY_VFG ? "pk_vfg_gpu" : m.family == FAMILY_FSR3 ? "pk_fsr3_gpu" : m.family == FAMILY_RIFE ? "pk_gpu" : "pk_seq_gpu";
    void *(*create)(const wchar_t *, int) = nullptr;
    void (*floor)(void *, int) = nullptr;
    const char *(*last)() = nullptr;
    bool ok = get(create, "pk_create") && get(fn_destroy, "pk_destroy") && get(floor, "pk_set_flow_floor") && get(last, "pk_last_error") &&
              get(fn_import, prefix + "_ctx_import") && get(fn_generate, prefix + "_ctx_generate") && get(fn_ctx_destroy, prefix + "_ctx_destroy") &&
              get(fn_error, m.family == FAMILY_RIFE ? "pk_gpu_bench_error" : prefix + "_error");
    fn_flow = nullptr;
    if (m.family == FAMILY_RIFE) get(fn_flow, prefix + "_ctx_set_flow");
    if (!ok) return "engine exports missing";
    std::wstring dir = folder() + L"\\" + m.dir;
    if (m.family == FAMILY_FSR3)
    {
        // 4K: a half-size flow saves 1.2 ms; the full one (flow: 1) leaves less of a ghost beside heads and arms.
        if (!GetEnvironmentVariableW(L"PKAPP_FSR_OF", nullptr, 0)) SetEnvironmentVariableW(L"PK_TEST_FSR_OF_HALF", h >= 2000 && !flow ? L"1" : nullptr);
    }
    LONGLONG t0 = qpc();
    std::string reason;
    if (m.family == FAMILY_RIFE || m.family == FAMILY_FSR3)
    {
        wchar_t gpu[8]; // tests: another graphics card
        engine = create(dir.c_str(), GetEnvironmentVariableW(L"PKAPP_GPU", gpu, 8) ? _wtoi(gpu) : -1);
        if (!engine) return std::string("pk_create: ") + last();
        if (m.family == FAMILY_RIFE) floor(engine, flow <= 1 ? 0 : flow);
        void *(*c)(void *, int, int, uint8_t *) = nullptr;
        if (get(c, prefix + "_ctx_create")) ctx = c(engine, w, h, id);
    }
    else if (m.family == FAMILY_VFG)
    {
        void *(*c)(int, int, uint8_t *) = nullptr;
        if (get(c, prefix + "_ctx_create")) ctx = c(w, h, id);
    }
    else
    {
        void *(*c)(int, const wchar_t *, int, int, uint8_t *) = nullptr;
        if (get(c, prefix + "_ctx_create")) ctx = c(m.family, dir.c_str(), w, h, id);
    }
    if (!ctx)
    {
        reason = error();
        close();
        return reason.empty() ? "context" : reason;
    }
    logf("engine: %ls %dx%d ready in %.0f ms", m.name, w, h, ms_of(qpc() - t0));
    return "";
}
void Engine::close()
{
    if (ctx && fn_ctx_destroy) fn_ctx_destroy(ctx);
    ctx = nullptr;
    if (engine && fn_destroy) fn_destroy(engine);
    engine = nullptr;
}
bool Engine::import(HANDLE nt, int expect) { return fn_import(ctx, nt) == expect; }

bool vfg_installed()
{
    wchar_t v[600];
    auto has = [&](const std::wstring &f) { // the last one the download puts there too
        return GetFileAttributesW((f + L"\\NVVideoEffects.dll").c_str()) != INVALID_FILE_ATTRIBUTES && GetFileAttributesW((f + L"\\nvVFXVideoFrameGeneration.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
    };
    DWORD n = GetEnvironmentVariableW(L"PK_VFG_DIR", v, 600);
    if (n && n < 600 && has(v)) return true;
    n = GetEnvironmentVariableW(L"LOCALAPPDATA", v, 600);
    if (n && n < 600 && has(std::wstring(v) + L"\\Peussok\\nvidia-vfg")) return true;
    n = GetEnvironmentVariableW(L"APPDATA", v, 600);
    if (!n || n >= 600) return false;
    std::wstring base = std::wstring(v) + L"\\Python\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((base + L"Python3*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    do ok = ok || has(base + fd.cFileName + L"\\site-packages\\nvvfx\\libs"); while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}
// The best picture that runs without more being installed: VFG on RTX 40 and 50 when it is there, RIFE on other RTX cards, the light FSR 3 everywhere else.
int default_model() { return nvidia_rtx() >= 40 && vfg_installed() ? 9 : nvidia_rtx() ? 0 : 5; }
int nvidia_rtx()
{
    static int cached = [] {
        DISPLAY_DEVICEW d;
        d.cb = sizeof d;
        int best = 0;
        for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &d, 0); i++, d.cb = sizeof d)
        {
            const wchar_t *p = wcsstr(d.DeviceString, L"RTX ");
            if (p && wcsstr(d.DeviceString, L"NVIDIA"))
            {
                int n = _wtoi(p + 4);
                best = wcsstr(d.DeviceString, L"Quadro") ? 20 : n >= 2000 && n < 10000 ? n / 1000 * 10 : 1;
            }
        }
        return best;
    }();
    return cached;
}
