# installer/build.sh [suffix]: dist/Peussok-<version> -> dist/Peussok-Setup-<version><suffix>.exe (run package.sh first)
cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")"
V=$(grep -oE 'APP_VERSION L"[^"]+' ../src/base.h | cut -d'"' -f2); S=../dist/Peussok-$V; O=../dist/Peussok-Setup-$V$1.exe
W=../build/setup; CSC=/c/Windows/Microsoft.NET/Framework64/v4.0.30319/csc.exe
[ -f "$S/Peussok.exe" ] || { echo "no $S"; exit 1; }
rm -rf $W; mkdir -p $W
printf 'static class Info { public const string Version = "%s"; }\n' "$V" > $W/Info.cs
python - "$S" $W <<'P'
import hashlib, os, sys, zipfile
s, w = sys.argv[1:]
with zipfile.ZipFile(w + '/payload.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z, open(w + '/payload.sha256', 'w', encoding='utf-8', newline='\n') as m:
    for d, _, fs in sorted(os.walk(s)):
        for f in sorted(fs):
            if f.endswith('.log'): continue
            p = os.path.join(d, f); n = os.path.relpath(p, s)
            z.write(p, n.replace('\\', '/'))
            m.write(hashlib.sha256(open(p, 'rb').read()).hexdigest() + '  ' + n + '\n')
P
cat > $W/app.manifest <<'M'
<?xml version="1.0" encoding="utf-8"?>
<assembly manifestVersion="1.0" xmlns="urn:schemas-microsoft-com:asm.v1">
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3"><security><requestedPrivileges><requestedExecutionLevel level="asInvoker" uiAccess="false"/></requestedPrivileges></security></trustInfo>
  <compatibility xmlns="urn:schemas-microsoft-com:compatibility.v1"><application><supportedOS Id="{8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}"/></application></compatibility>
</assembly>
M
R="/r:System.Windows.Forms.dll /r:System.Drawing.dll /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll"
export MSYS2_ARG_CONV_EXCL='*'; w() { cygpath -w "$1"; }
$CSC /nologo /target:winexe /platform:x64 /optimize+ /codepage:65001 /define:UNINSTALLER "/out:$(w $W/uninstall.exe)" "/win32manifest:$(w $W/app.manifest)" "/win32icon:$(w ../res/app.ico)" $R "$(w $W/Info.cs)" "$(w Setup.cs)" || exit 1
$CSC /nologo /target:winexe /platform:x64 /optimize+ /codepage:65001 "/out:$(w "$O")" "/win32manifest:$(w $W/app.manifest)" "/win32icon:$(w ../res/app.ico)" "/resource:$(w $W/payload.zip),payload.zip" "/resource:$(w $W/payload.sha256),payload.sha256" "/resource:$(w $W/uninstall.exe),uninstall.exe" $R "$(w $W/Info.cs)" "$(w Setup.cs)" || exit 1
ls -la "$O"; sha256sum "$O"
