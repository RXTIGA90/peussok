// SPDX-License-Identifier: GPL-3.0-only
// GPU clock keep (see gpu_keeper.cpp). Internal to the engine DLL.
#pragma once
#include <cstdint>
void* pk_keeper_start(uint32_t vendor,uint32_t device);
void pk_keeper_active(void* keeper,bool on);
double pk_keeper_dispatch_ms(void* keeper);
uint64_t pk_keeper_dispatches(void* keeper);
void pk_keeper_stop(void* keeper);
// GPU options set by the application before a stream is created (bit 0 clock keep, bit 1 fp16 arithmetic).
int pk_gpu_options_value();
// PCI vendor/device ids of an engine handle (internal).
int pk_engine_ids(void* engine,uint32_t* vendor,uint32_t* device);
