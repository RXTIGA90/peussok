// SPDX-License-Identifier: GPL-3.0-only
// Capture device input through DirectShow, without any filter in between: the capture pin connects straight to a sink of our
// own, whose Receive() runs on the driver's streaming thread. Nothing is queued, nothing is copied before the callback.
#pragma once
#include "base.h"
#include <dshow.h>

// mjpg, i420, yv12: made into NV12 here, before anyone sees it. uyvy also stands for HDYC; bgra for RGB32 and ARGB32.
enum class Pixel { none, nv12, p010, yuy2, bgr24, mjpg, uyvy, bgra, i420, yv12 };
const char *pixel_name(Pixel p);
bool planar_to_nv12(const uint8_t *d, size_t size, int w, int h, bool v_first, std::vector<uint8_t> &out); // I420 / YV12

struct VideoFormat
{
    Pixel pixel = Pixel::none;
    int w = 0, h = 0;
    double fps = 0;
    bool bottom_up = false;
    bool full = false; // levels 0-255, as JPEG has them
};
struct AudioFormat { int rate = 0, channels = 0, bits = 0; };
struct Device { std::wstring name, path; };

struct CaptureSink
{
    virtual ~CaptureSink() {}
    // arrival: QPC when the driver handed the picture over. device_ms: how long ago the device says the picture began, or < 0.
    virtual void on_video(const uint8_t *data, size_t size, const VideoFormat &f, LONGLONG arrival, double device_ms) = 0;
    // The picture was handled; the driver fills its buffer again after this. Never called after on_stop().
    virtual void on_buffer(uint8_t *data, size_t size, const VideoFormat &f) {}
    virtual void on_stop() {} // the buffers go away now
    virtual void on_audio(const uint8_t *data, size_t size, const AudioFormat &f) = 0;
    // Screen capture: the part box of src (a texture of video_device(), B8G8R8A8) is the picture; valid only during the call.
    virtual void on_texture(struct ID3D11Texture2D *src, const struct D3D11_BOX &box, const VideoFormat &f, LONGLONG arrival) {}
    virtual void on_alive(LONGLONG arrival) {} // nothing new, the input is still there
};

std::vector<Device> capture_devices();

class Capture
{
public:
    ~Capture() { close(); }
    // want.w == 0: let the driver choose (its current format), then prefer NV12. Returns an empty string or the reason.
    std::wstring open(const std::wstring &path, const VideoFormat &want, bool audio, CaptureSink *sink);
    void close();
    bool running() const { return control != nullptr; }
    const VideoFormat &video() const { return vf; }
    const AudioFormat &audio() const { return af; }
    std::vector<VideoFormat> formats; // what the device offers (largest rate of each size and pixel type)

private:
    IGraphBuilder *graph = nullptr;
    IMediaControl *control = nullptr;
    IBaseFilter *source = nullptr, *asource = nullptr; // asource: the sound of a USB card, a device of its own
    VideoFormat vf;
    AudioFormat af;
    class SinkFilter *vs = nullptr, *as = nullptr;
};
