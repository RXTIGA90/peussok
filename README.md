# 프쏙 (PEUSSOK)

[English](README.en.md)

캡처보드로 받은 콘솔 화면에 프레임 생성을 걸어 주는 윈도우 프로그램입니다.
30fps 게임은 60fps로, 60fps 게임은 120fps로 늘려 주고, 일부 모델은 3배·4배까지 됩니다.
OBS 같은 방송 프로그램 없이 이것 하나만 켜면 됩니다.
캡처보드가 없어도 PXPlay, PS Remote Play 같은 리모트 플레이 창이나 PC 게임 창에 걸 수 있습니다.

## 다운로드

[Releases](https://github.com/RXTIGA90/peussok/releases/latest)에서 받으세요.

- **Peussok-Setup-x.x.x.exe** : 설치 버전
- **Peussok-x.x.x-Portable.zip** : 포터블 버전 (압축 풀고 Peussok.exe 실행)

코드 서명이 없어서 처음 실행할 때 윈도우 경고가 뜰 수 있습니다. "추가 정보 → 실행"을 누르면 됩니다.
사용법은 프로그램 왼쪽 아래 "사용 가이드"와 설명서.txt에 있습니다.

## 주요 기능

- **프레임 생성 모델 6종**: NVIDIA VFG, NVIDIA DLSS FG, AMD FSR FG, FSR 3, RIFE 4.6, RIFE 4.15 lite
- **DLSS 5**: 게임 화면을 실사 느낌으로 바꿔 줍니다 (강도 1~5, 피부 보정, 깜빡임 억제)
- **녹화**: 보이는 화면 그대로(생성 프레임 포함) 캡처보드 소리와 함께 MP4로 저장 (HEVC/H.264, Ctrl+Alt+R)
- **선행 생성**: 프레임이 다 들어오기 전에 생성을 시작해서 지연을 줄입니다
- **HUD·자막 보호**, 장면 전환 감지, 샤프닝, HDR 모니터 출력, G-SYNC·FreeSync 지원
- **콘솔 HDR**: 콘솔에서 HDR을 켰다면 "게임기 HDR 받기"로 10비트(P010)로 받습니다. 색감은 원본 그대로, 하늘 같은 곳의 색 띠(밴딩)는 줄어듭니다
- **창·모니터 캡처**: OBS처럼 입력 종류에서 "창"이나 "모니터"를 고르면 리모트 플레이·PC 게임 화면에 프레임 생성을 겁니다. 창 캡처는 다른 창이 위를 덮어도 그 창만 받습니다
- **다양한 캡처보드 지원**: NV12, P010, YUY2, UYVY(HDYC), RGB32, RGB24, I420, YV12, MJPEG. 저가형 USB 캡처보드도 됩니다
- 벤치마크로 내 PC에 맞는 모델·배수 추천, 모델이 멈추면 자동으로 다른 모델로 전환
- 탭마다 "기본값으로" 버튼, 한국어·영어 지원

## 지연 시간

RTX 5080, 4K, 2배 기준입니다. 캡처 프레임이 다 들어온 시점부터 화면에 출력(Present)하기까지의 시간이며, 60fps / 30fps 게임 기준입니다.
콘솔, 캡처보드, 모니터 자체의 지연은 포함되지 않았습니다.

| 모델 | 선행 생성 끔 | 선행 생성 약 | 선행 생성 강 |
|---|---|---|---|
| FSR 3 | 5.4 / 8.4 ms | 4.6 / 7.3 ms | 1.0 / 4.1 ms |
| NVIDIA DLSS FG | 6.1 / 9.1 ms | 4.5 / 7.3 ms | 2.0 / 5.0 ms |
| NVIDIA VFG | 7.1 / 9.9 ms | 4.7 / 7.4 ms | 2.2 / 5.2 ms |
| RIFE 4.6 | 7.1 / 10.1 ms | 4.8 / 7.9 ms | 2.3 / 5.8 ms |

선행 생성을 "강"으로 두면 생성 프레임의 화면 아래쪽 일부를 예측해서 채웁니다. 원본 프레임은 항상 그대로입니다.

## 요구 사양

- 윈도우 10/11 64비트, 캡처보드 (창·모니터 캡처는 캡처보드 없이 됩니다)
- DirectX 11과 Vulkan 1.2 이상을 지원하는 그래픽카드 (FSR 3은 Vulkan 1.3)
- NVIDIA VFG·DLSS FG: RTX 그래픽카드 (공식 지원은 RTX 40·50 시리즈)
- DLSS 5: RTX 50 시리즈, 2026년 9월 이후 드라이버, 그리고 PC에 DLSS 5를 지원하는 게임(Steam·Epic·Xbox 앱)이 설치되어 있어야 합니다.
  DLSS 5 파일(nvngx_dlssnr.dll)은 NVIDIA 소유라 배포 파일에 넣지 않고, 설치된 게임에서 자동으로 찾아 씁니다. 못 찾으면 직접 지정할 수 있습니다.
- 테스트한 환경: RTX 5080 + AVerMedia GC573, USB 2.0 캡처보드(MS2109, MJPEG 1080p 30fps).
  다른 캡처보드·그래픽카드에서 써 보시고 결과를 알려 주시면 큰 도움이 됩니다.

## DLSS 5 파일 찾는 법

DLSS 5 파일(nvngx_dlssnr.dll)은 NVIDIA가 아직 따로 배포하지 않아서, 저작권 때문에 프쏙에 넣을 수 없습니다. 아래 중 하나만 하면 됩니다.

1. DLSS 5를 지원하는 게임을 PC에 설치하세요. 프쏙이 Steam·Epic·Xbox 앱 게임 폴더에서 자동으로 찾습니다.
2. 이미 파일이 있다면 Peussok.exe가 있는 폴더에 넣으세요. 다음에 켤 때 바로 씁니다.
3. 화질 탭의 "파일 고르기"로 직접 골라도 됩니다.

NVIDIA가 서명한 정식 파일만 사용합니다. 인터넷에 올라온 수정본이나 출처를 모르는 파일은 쓰지 않습니다.
NVIDIA가 공식 배포를 허용하면 프쏙에 바로 넣겠습니다.

## 알려진 제한

- 방송(스트리밍) 기능은 없습니다.
- 모니터 캡처 중 프쏙을 같은 모니터에 전체 화면으로 띄우면, 프쏙 화면은 다른 캡처 프로그램(OBS, 원격 데스크톱)에서 보이지 않습니다. 자기 화면을 다시 찍지 않기 위해서입니다.
- 리모트 플레이 영상은 압축돼 있고 프레임 간격이 고르지 않아서, 캡처보드보다는 결과가 조금 떨어집니다.
- 콘솔 HDR을 HDR 밝기 그대로 받을 수는 없습니다. GC573은 HDR 신호를 SDR로 변환해서 넘겨줍니다.
- HDCP가 걸린 화면은 나오지 않습니다. 콘솔에서 HDCP를 꺼 주세요.
- 창 모드보다 전체 화면이 빠릅니다 (창 모드는 7~11 ms 늘어납니다).

## 개인 정보

프로그램이 외부로 보내는 데이터는 없습니다. 인터넷에서 받는 것은 NVIDIA VFG 런타임(약 416 MB, pypi.nvidia.com)뿐이고, 동의했을 때만 받습니다.

## 직접 빌드하기

윈도우, Git Bash, Python 3이 필요합니다.

```
powershell -ExecutionPolicy Bypass -File tools/Get-Toolchain.ps1
powershell -ExecutionPolicy Bypass -File engine/Build-Engine.ps1
bash package.sh
bash installer/build.sh
```

DLSS FG와 AMD FSR FG는 NVIDIA·AMD SDK가 있어야 빌드됩니다(`PK_DLSSG_SRC`, `PK_AMDFG_SRC`). 없으면 이 두 모델만 빠집니다.

## 라이선스

[GPL-3.0](LICENSE)이며, NVIDIA·AMD 라이브러리에 대한 [예외 조항](LICENSE-EXCEPTION.txt)이 있습니다.
다른 사람이 만든 부분은 [engine/THIRD_PARTY_NOTICES.txt](engine/THIRD_PARTY_NOTICES.txt)에 있습니다. 프로그램 이름과 아이콘은 라이선스에 포함되지 않습니다.
NVIDIA, GeForce, DLSS, G-SYNC는 NVIDIA의 상표이고, AMD, FidelityFX는 AMD의 상표입니다.
개인이 만든 프로그램이며 NVIDIA, AMD, 캡처보드 제조사와 관계가 없습니다.

지금은 외부 코드 기여(PR)는 받지 않습니다. 버그 제보와 테스트 결과는 Issues에 남겨 주세요.
