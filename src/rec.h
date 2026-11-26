// SPDX-License-Identifier: GPL-3.0-only
// Recording what goes to the screen (the made pictures too) with the card's sound, to an MP4 file. The graphics card's own encoder
// (HEVC, or H.264 where HEVC is not there) through Windows Media Foundation; nothing else is installed.
#pragma once
#include "capture.h"
#include <d3d11.h>

// settings of the Record tab (kept in settings.ini by the app)
extern volatile LONG rec_codec_pref; // 0 automatic (HEVC, else H.264), 1 HEVC, 2 H.264
extern volatile LONG rec_quality;    // 0 smaller files, 1 normal, 2 high
extern volatile LONG rec_sound;      // 1: the card's sound goes into the file
extern volatile LONG rec_made;       // 1: what is shown, made pictures too; 0: the originals only
void rec_set_folder(const std::wstring &f); // empty: Videos\Peussok
bool rec_on();                 // recording (or about to start with the next picture)
std::wstring rec_start();      // empty, or why not
void rec_stop();
void rec_pause(bool on);       // pause (nothing is written) or go on in the same file
bool rec_paused();
std::wstring rec_file();       // the file being written (or the last one)
double rec_seconds();          // how long it runs
long long rec_pictures();      // written so far
long long rec_dropped();       // left out because the encoder was behind
std::wstring rec_folder();     // Videos\Peussok
const wchar_t *rec_codec();    // L"HEVC" / L"H.264" / L""
std::wstring rec_error();      // the last trouble, if any
// Presenter thread: a picture that was just presented (source size, SDR BGRA). fps: the rate the pictures come at, for the file header.
extern volatile LONG rec_pq;        // the picture is a console's HDR10 (PQ): recorded as an ordinary video
void rec_frame(ID3D11Device *d, ID3D11DeviceContext *c, ID3D11ShaderResourceView *picture, int w, int h, double fps, LONGLONG when, bool made = false);
void rec_release();            // presenter thread: its device goes away
// Capture thread: the card's sound as it arrives.
void rec_audio(const uint8_t *data, size_t size, const AudioFormat &f, LONGLONG when);
