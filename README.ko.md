# 프쏙 (PEUSSOK)

[English](README.md)

캡처카드 화면을 직접 받아서 프레임을 늘려 보여 주는 윈도우 프로그램이에요. 30 → 60, 60 → 120, 일부 모델은 3배·4배도 돼요.
다른 프로그램을 켤 필요가 없어요.

## 할 수 있는 것

- 캡처카드를 직접 열어서(DirectShow) 전체 화면으로 빠르게 보여 줘요. 다른 회사 카드도 돼요: NV12, P010, YUY2, UYVY(HDYC), RGB32, RGB24, I420, YV12, MJPEG. 카드, 크기·형식, 초당 장수를 직접 고를 수 있어요.
- 모델 여섯 가지: FSR 3, AMD FSR FG, RIFE 4.6, RIFE 4.15 lite, NVIDIA DLSS FG, NVIDIA VFG
- **미리 시작**: 화면을 줄 단위로 넘겨주는 카드에서는 화면이 다 오기 전에 만들기 시작해요.
- HUD·글자 보호, 장면 전환 감지, 선명도, HDR 모니터 출력, G-SYNC·FreeSync
- **DLSS 5**(NVIDIA 뉴럴 렌더링): 게임 화면을 실사 느낌으로 바꿔요. 강도 1~5, 피부 부드럽게, 적용 순서(생성 뒤·생성 전), 깜빡임 억제.
- **녹화**: 보이는 화면 그대로(만든 프레임 포함) 캡처카드 소리와 함께 MP4로 저장해요(그래픽카드 인코더, HEVC 또는 H.264, Ctrl+Alt+R).
- 벤치마크가 내 컴퓨터에 맞는 모델을 찾아 줘요. 모델이 멈추면 다른 모델로 바꿔서 계속해요.
- 처음 켜면 화면을 짚어 주는 사용 가이드(왼쪽 아래 '사용 가이드'로 다시 보기), 화면·입력·소리·녹화 탭
- 한국어와 영어

## 지연

RTX 5080, 4K, 2배에서 쟀어요. 원본이 다 도착한 때부터 화면으로 내보낼 때(Present)까지이고, 60fps / 30fps 게임이에요.
게임기, 캡처카드, 모니터의 지연은 들어 있지 않아요.

| 모델 | 미리 시작 끔 | 조금 | 많이 |
|---|---|---|---|
| FSR 3 | 5.4 / 8.4 ms | 4.6 / 7.3 ms | 1.0 / 4.1 ms |
| NVIDIA DLSS FG | 6.1 / 9.1 ms | 4.5 / 7.3 ms | 2.0 / 5.0 ms |
| NVIDIA VFG | 7.1 / 9.9 ms | 4.7 / 7.4 ms | 2.2 / 5.2 ms |
| RIFE 4.6 | 7.1 / 10.1 ms | 4.8 / 7.9 ms | 2.3 / 5.8 ms |

"많이"에서는 만든 장면의 아래쪽을 짐작해서 채워요. 원본은 언제나 온전해요.

## 필요한 것

- 윈도우 10/11 64비트, DirectX 11과 Vulkan 1.2(FSR 3은 1.3) 이상이 되는 그래픽카드, 캡처카드
- NVIDIA VFG와 DLSS FG는 드라이버가 허용하는 RTX 카드(공식은 RTX 40·50)가 필요해요.
- DLSS 5는 RTX 50과 2026년 9월 이후 드라이버, 그리고 이 PC의 게임(Steam·Epic·Xbox 앱)에 들어 있는 NVIDIA 서명 파일(nvngx_dlssnr.dll)이 필요해요. 프로그램이 자동으로 찾고, 없으면 직접 고를 수 있어요. 이 파일은 NVIDIA 것이라 배포 파일에 넣지 않아요.
- 확인한 구성: RTX 5080과 AVerMedia GC573, USB 2.0 카드(MS2109, MJPEG로 1080p 30fps). 다른 카드는 확인이 부족해서 결과를 알려 주시면 큰 도움이 돼요.

## 아직 안 되는 것

- 방송(스트리밍)은 없어요. 게임기 HDR은 받지 못해요(게임기에서 꺼 주세요). HDCP 화면은 나오지 않아요.
- 창보다 전체 화면이 빨라요(창은 7~11 ms 늘어요).
- 배포 파일에 서명이 없어서 윈도우가 경고를 띄워요.

## 개인 정보

프로그램이 보내는 것은 없어요. 내려받는 것은 NVIDIA VFG 런타임(약 416 MB, pypi.nvidia.com)뿐이고, 동의했을 때만 받아요.

## 직접 만들기

윈도우, Git Bash, Python 3이 필요해요.

```
powershell -ExecutionPolicy Bypass -File tools/Get-Toolchain.ps1
powershell -ExecutionPolicy Bypass -File engine/Build-Engine.ps1
bash package.sh
bash installer/build.sh
```

DLSS FG와 AMD FSR FG는 NVIDIA·AMD의 SDK가 있어야 만들어져요(`PK_DLSSG_SRC`, `PK_AMDFG_SRC`). 없으면 두 모델만 빠져요.

## 라이선스

[GPL-3.0](LICENSE)이고 NVIDIA·AMD 라이브러리에 대한 [예외](LICENSE-EXCEPTION.txt)가 있어요. 다른 사람이 만든 부분은
[engine/THIRD_PARTY_NOTICES.txt](engine/THIRD_PARTY_NOTICES.txt)에 있어요. 이름과 아이콘은 포함되지 않아요.
NVIDIA, GeForce, DLSS, G-SYNC는 NVIDIA의 상표이고 AMD, FidelityFX는 AMD의 상표예요.
개인이 만든 프로그램이고 NVIDIA, AMD, 캡처카드 제조사와 관계가 없어요.

지금은 다른 사람의 코드를 받지 않아요. 버그 신고와 시험 결과는 Issues에 남겨 주세요.
