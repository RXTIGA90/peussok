// SPDX-License-Identifier: GPL-3.0-only
// Sound of the capture card to the default playback device (WASAPI shared mode, event driven, smallest period the device offers).
#pragma once
#include "base.h"
#include "capture.h"

extern volatile LONG audio_volume; // 0-100
extern volatile LONG audio_mute;
extern volatile LONG audio_delay; // ms the sound waits longer (0-300), to meet the picture
void audio_start();
void audio_stop();
void audio_push(const uint8_t *data, size_t size, const AudioFormat &f); // capture thread
double audio_buffer_ms(); // sound waiting here
extern volatile LONG64 audio_gaps; // times the sound ran out or was cut
