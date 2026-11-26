# Builds dist/Peussok-<version> and a zip (Git Bash, Python 3). First: tools/Get-Toolchain.ps1 and engine/Build-Engine.ps1
cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"; V=$(grep -oE 'APP_VERSION L"[^"]+' src/base.h | cut -d'"' -f2); N=Peussok-$V
[ -f build/stage/engine/peussok_engine.dll ] || { echo "run engine/Build-Engine.ps1 first"; exit 1; }
bash build.sh || exit 1
rm -rf "dist/$N" "dist/$N.zip"; mkdir -p "dist/$N"
cp build/Peussok.exe LICENSE LICENSE-EXCEPTION.txt README.txt 설명서.txt "dist/$N/"
cp -r build/stage/engine "dist/$N/engine"; find "dist/$N" -name "*.log" -delete
(cd dist && python -c "import shutil,sys;shutil.make_archive(sys.argv[1],'zip','.',sys.argv[1])" "$N")
ls -la dist; sha256sum "dist/$N.zip"
