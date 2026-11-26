# Builds build/Peussok.exe (Git Bash). First: tools/Get-Toolchain.ps1
cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"; Z=$PWD/toolchain/zig-x86_64-windows-0.14.1/zig.exe
[ -f "$Z" ] || { echo "run tools/Get-Toolchain.ps1 first"; exit 1; }
export ZIG_GLOBAL_CACHE_DIR="$(cygpath -w "$PWD/toolchain/cache")"; mkdir -p build toolchain/cache
(cd res && "$Z" rc app.rc ../build/app.res) || exit 1
"$Z" c++ -s -target x86_64-windows-gnu -std=c++17 -O2 -municode -Wl,--subsystem,windows -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers src/*.cpp build/app.res -o build/Peussok.exe -ld3d11 -ldxgi -ld3dcompiler_47 -ldxguid -lstrmiids -lole32 -loleaut32 -luuid -luser32 -lgdi32 -lshell32 -ladvapi32 -lavrt -lwinmm -lksuser -lpdh -lpsapi -ld2d1 -ldwrite -ldwmapi -lmfuuid -ld3d12 -lwintrust -lcrypt32 -lversion -lcomdlg32 -lwindowscodecs || exit 1
ls -la build/Peussok.exe
