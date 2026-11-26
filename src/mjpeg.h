// SPDX-License-Identifier: GPL-3.0-only
// Motion JPEG pictures of a capture card: decoded by the graphics card where it can, else by the decoder that is part of Windows.
#pragma once
#include "base.h"
struct IDXGIAdapter1;
class Mjpeg
{
public:
    ~Mjpeg() { close(); }
    // One picture to NV12 (w x h, no gaps). The result stays until the next call. False: nothing came of it.
    bool decode(const uint8_t *data, size_t size, int w, int h, IDXGIAdapter1 *adapter, const uint8_t *&out, size_t &n);
    void close();
    static double last_ms();          // smoothed time of one picture, 0: none
    static const wchar_t *with();     // who decodes

private:
    bool open(int w, int h, IDXGIAdapter1 *adapter);
    bool start(struct IMFTransform *t, bool hard);
    bool run(const uint8_t *data, size_t size);
    struct State;
    State *s = nullptr;
    int w = 0, h = 0, fails = 0;
    bool hard_failed = false, dead = false;
    std::vector<uint8_t> before, packed;
    const uint8_t *result = nullptr;
};
