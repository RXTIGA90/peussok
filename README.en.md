# PEUSSOK (프쏙)

[한국어](README.md)

Shows a capture card's picture on Windows with frame generation: 30 → 60, 60 → 120, and 3x/4x with some models.
No other program is needed. Without a card it also works on a remote play window (PXPlay, PS Remote Play) or a PC game.

## What it does

- Opens the capture card itself (DirectShow) and shows it full screen with as little delay as possible. Cards of other makers work too: NV12, P010, YUY2, UYVY (HDYC), RGB32, RGB24, I420, YV12, MJPEG; the card, size, kind and frame rate can be chosen.
- **Window and monitor capture**: like OBS, choose Window or Monitor as the input type. A window is taken by itself, also when other windows cover it.
- Six models to choose from: FSR 3, AMD FSR FG, RIFE 4.6, RIFE 4.15 lite, NVIDIA DLSS FG and NVIDIA VFG.
- *Start early*: with a card that hands its picture over line by line, the model starts before the last lines are there.
- Protects HUD and text, detects scene cuts, sharpens, shows HDR on an HDR monitor, works with G-SYNC / FreeSync.
- **DLSS 5** (NVIDIA neural rendering): a more lifelike picture. Strength 1-5, smoother skin, order (after or before generation), flicker reduction.
- **Recording**: what you see, made frames included, with the card's sound, to MP4 (the graphics card's encoder, HEVC or H.264, Ctrl+Alt+R).
- A benchmark finds the model for your computer. If a model stops, the program goes on with another one.
- A step-by-step guide (Guide, bottom left), tabs for display, input, sound and recording, and a Reset button on each tab.
- Korean and English.

## Delay

Measured on an RTX 5080 at 4K, 2x: from the moment the whole original has arrived to the moment it is handed to the screen
(Present), in a game with 60 / 30 pictures a second. It does not include the console, the card or the monitor.

| Model | Start early off | Low | High |
|---|---|---|---|
| FSR 3 | 5.4 / 8.4 ms | 4.6 / 7.3 ms | 1.0 / 4.1 ms |
| NVIDIA DLSS FG | 6.1 / 9.1 ms | 4.5 / 7.3 ms | 2.0 / 5.0 ms |
| NVIDIA VFG | 7.1 / 9.9 ms | 4.7 / 7.4 ms | 2.2 / 5.2 ms |
| RIFE 4.6 | 7.1 / 10.1 ms | 4.8 / 7.9 ms | 2.3 / 5.8 ms |

With *a lot*, the lowest part of the made pictures is guessed. Originals are always complete.

## Needs

- Windows 10/11 64 bit, a graphics card with DirectX 11 and Vulkan 1.2 (FSR 3: 1.3), a capture card (not needed for window or monitor capture).
- NVIDIA VFG and DLSS FG need an RTX card that the driver allows (officially RTX 40/50).
- DLSS 5 needs an RTX 50, a driver from September 2026 or later, and the NVIDIA-signed nvngx_dlssnr.dll that a game on this PC brings (Steam, Epic, Xbox app). The program finds it, or you choose it. It is NVIDIA's file, so it is not part of the download.
- Tested: RTX 5080 with AVerMedia GC573, and a USB 2.0 card (MS2109, 1080p at 30 fps over MJPEG). Other cards are little tested; reports are welcome.

## Known limits

- No streaming. The console's HDR brightness is not taken in as it is (the GC573 hands over an SDR picture; Console HDR input takes it in 10 bits). HDCP pictures cannot be shown.
- Full screen is faster than a window (a window adds 7-11 ms).
- During monitor capture, PEUSSOK full screen on that same monitor is hidden from other capture programs (OBS, remote desktop), so it does not capture itself.
- Remote play pictures are compressed and arrive unevenly, so results are a little below a capture card.
- The release files are not signed, so Windows shows a warning.

## Privacy

The program sends nothing. The only download is NVIDIA's VFG runtime (about 416 MB, from pypi.nvidia.com), and only after you agree.

## Build

Windows, Git Bash, Python 3:

```
powershell -ExecutionPolicy Bypass -File tools/Get-Toolchain.ps1
powershell -ExecutionPolicy Bypass -File engine/Build-Engine.ps1
bash package.sh
bash installer/build.sh
```

The toolchain script downloads Zig, CMake and Ninja and checks their SHA-256. The bridges for DLSS FG and AMD FSR FG need the SDKs
of NVIDIA and AMD (`PK_DLSSG_SRC`, `PK_AMDFG_SRC`); without them these two models are left out.

## License

[GPL-3.0](LICENSE) with an [exception](LICENSE-EXCEPTION.txt) for the libraries of NVIDIA and AMD. Parts made by others:
[engine/THIRD_PARTY_NOTICES.txt](engine/THIRD_PARTY_NOTICES.txt). The name and the icon are not part of it.
NVIDIA, GeForce, DLSS and G-SYNC are trademarks of NVIDIA Corporation; AMD and FidelityFX are trademarks of AMD.
A personal project, not related to or endorsed by NVIDIA, AMD or any capture card maker.

Code from others is not taken in for now; bug reports and test results are very welcome (Issues).
