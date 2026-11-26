// SPDX-License-Identifier: GPL-3.0-only
// HDR of the monitor the window is on: the switch of Windows.
#pragma once
#include "base.h"
int hdr_state(HWND window);            // 0 not possible, 1 off, 2 on
void hdr_set(HWND window, bool on);    // in the background; the picture may be gone for a moment
bool hdr_busy();
float hdr_peak(HWND window);           // brightest the monitor says it can show, in nits
