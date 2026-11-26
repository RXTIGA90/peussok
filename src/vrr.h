// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "base.h"
int vrr_state(HWND window); // 2 the monitor follows the pictures, 1 possible, 0 off, -1 not known
int vrr_monitor(HWND window); // what the monitor itself says: 1 it offers variable refresh, 0 it does not (switched off in its menu, or it cannot), -1 not known
// Switches variable refresh on for this program in the NVIDIA driver (a profile of its own, windowed and full screen). Empty: done.
std::wstring vrr_driver_on();
std::wstring vrr_driver_off(bool fixed); // removes the profile again; fixed: the driver has it on for everything, so this program asks for a fixed rate
// The driver's own switch of this monitor ("enable settings for the selected display model"): 1 on, 0 off, -1 not known or no such monitor.
int vrr_allowed(HWND window);
std::wstring vrr_model(HWND window); // the monitor's model; settings "vrr_monitor_model": where this program switched it on
void vrr_allow(HWND window, bool on); // in the background; the picture may be gone for a moment
enum { ALLOW_IDLE, ALLOW_BUSY, ALLOW_DONE, ALLOW_FAILED };
int vrr_allow_now(std::wstring &why);
