// SPDX-License-Identifier: GPL-3.0-only
// What the computer is and how busy it is. Everything is read without administrator rights, once a second, on a lowest-priority
// thread that sleeps while nothing on the screen shows these numbers; it never touches the picture path.
#pragma once
#include "base.h"

struct SysInfo // read once
{
    std::wstring cpu, gpu, driver, os, ram;
    int cores = 0, threads = 0, vram_mb = 0, cpu_base_mhz = 0;
};
struct SysLive // -1 = not available
{
    float cpu = -1, cpu_app = -1, cpu_mhz = -1, cpu_w = -1;
    float ram_used = -1, ram_total = -1, ram_app = -1; // GB
    float gpu = -1, gpu_mhz = -1, gpu_max_mhz = -1, mem_mhz = -1, gpu_w = -1, gpu_limit_w = -1, gpu_temp = -1, gpu_fan = -1, gpu_power_pct = -1;
    float vram_used = -1, vram_total = -1, vram_app = -1, pcie_gen = -1, pcie_width = -1, pstate = -1; // GB
    const wchar_t *limit = L"";        // why the graphics card does not run faster
};
void sys_start();
void sys_stop();
void sys_want(bool on);
const SysInfo &sys_info();
SysLive sys_live();
