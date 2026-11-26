// SPDX-License-Identifier: GPL-3.0-only
// The frame generation engine (peussok_engine.dll).
// A context works on five shared BGRA8 textures (0-2 originals, 3-4 outputs) that this program owns.
#pragma once
#include "base.h"

struct ModelInfo
{
    int id, family;      // ids as in the plugin's settings
    const wchar_t *name, *dir;
    int speed, quality;  // 1-5, for the settings window
    bool nvidia, multi;  // needs NVIDIA RTX; can make 3x and 4x
};
extern const ModelInfo models[];
extern const int model_count;
const ModelInfo *model_by_id(int id);
enum { FAMILY_RIFE = 0, FAMILY_FSR3 = 3, FAMILY_VFG = 4, FAMILY_DLSSG = 5, FAMILY_AMDFG = 6, FAMILY_EXTRA = 100 }; // EXTRA: no engine, see extra.h

class Engine
{
public:
    ~Engine() { close(); }
    // flow: RIFE motion size 1 half, 2 quarter, 3 eighth. Empty string or the reason.
    std::string open(const ModelInfo &m, int w, int h, int flow);
    void close();
    bool import(HANDLE nt, int expect);
    int generate(int a, uint64_t ida, int b, uint64_t idb, int out, float t) { return fn_generate(ctx, a, ida, b, idb, out, t, ms); }
    bool set_flow(int level) { return fn_flow && !fn_flow(ctx, level == 1 ? 0 : level); }
    bool can_flow() const { return fn_flow != nullptr; }
    const char *error() const { return fn_error ? fn_error() : "?"; }
    const uint8_t *luid() const { return id; }
    static bool load_dll();
    static std::wstring folder();

private:
    void *engine = nullptr, *ctx = nullptr;
    uint8_t id[8]{};
    double ms[3]{};
    void (*fn_destroy)(void *) = nullptr;
    int (*fn_import)(void *, HANDLE) = nullptr;
    int (*fn_generate)(void *, int, uint64_t, int, uint64_t, int, float, double *) = nullptr;
    int (*fn_flow)(void *, int) = nullptr;
    void (*fn_ctx_destroy)(void *) = nullptr;
    const char *(*fn_error)() = nullptr;
};
bool vfg_installed(); // NVIDIA's VFG runtime is on this PC
int default_model();
int nvidia_rtx(); // 0 no, 1 unknown generation, 20, 30, 40, 50
