// SPDX-License-Identifier: GPL-3.0-only
// Automatic recovery. The program runs as two processes: a tiny guard that only waits, and the program itself. When the program
// crashes, or ends itself because a graphics driver or an engine got stuck, the guard starts it again with the same settings. A
// model that fails again and again is replaced by the next one.
//
// Order: 1 the engine returns errors       -> the engine is closed and opened again (about half a second, the picture keeps running)
//        2 it fails again within a minute  -> the program starts again (about two seconds)
//        3 the same model fails twice more -> the next model
//        4 six starts in ten minutes       -> generation stays off
#pragma once
#include "base.h"

enum { GUARD_EXIT = 77, GUARD_AGAIN = 78 }; // exit codes: start me again (after trouble; because the person asked)
int guard_main(const wchar_t *cmd);      // the guard process; returns when the program has ended for good
void guard_child_begin(const wchar_t *cmd); // in the program, at the start
void guard_clean_exit();                 // in the program: the user is closing it
void guard_restart(const char *why);     // in the program: does not return
void guard_again();                      // the same, asked for by the person
int guard_next_model(int model);         // the model to use instead, or -1
bool guard_recovered();                  // this run was started by the guard after a problem
std::wstring guard_notice();             // what to tell the user for a few seconds, or empty
void guard_say(const std::wstring &s);
// for tests: PKAPP_FAULT=err|hang|crash|lost @ picture [: model]
void guard_fault(int model, bool &fail);
