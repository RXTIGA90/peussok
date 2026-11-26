# Downloads the pinned build tools into <repo>\toolchain and checks their SHA-256.
# zig 0.14.1 (C/C++ compiler, linker, dlltool), CMake 4.4.5, Ninja 1.13.2. The C# installer uses csc.exe from .NET Framework 4 (part of Windows).
[CmdletBinding()]
# -ZipDir <folder>: use zip files already downloaded there (zig.zip, cmake.zip, ninja.zip) instead of downloading.
param([string]$ZipDir)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$dest=Join-Path $root 'toolchain'
New-Item -ItemType Directory -Force $dest | Out-Null
$tools=@(
 @{name='zig';url='https://ziglang.org/download/0.14.1/zig-x86_64-windows-0.14.1.zip';sha256='554f5378228923ffd558eac35e21af020c73789d87afeabf4bfd16f2e6feed2c';check='zig-x86_64-windows-0.14.1\zig.exe'},
 @{name='cmake';url='https://github.com/Kitware/CMake/releases/download/v4.4.5/cmake-4.4.5-windows-x86_64.zip';sha256='326c826486d46a23cf9d5a1c9ee3a567d427bdd02c4a34071937a2a675cff004';check='cmake-4.4.5-windows-x86_64\bin\cmake.exe'},
 @{name='ninja';url='https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip';sha256='07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65';check='ninja\ninja.exe';sub='ninja'}
)
foreach($t in $tools){
 if(Test-Path -LiteralPath (Join-Path $dest $t.check)){"$($t.name): already present";continue}
 $zip=Join-Path $dest ($t.name+'.zip')
 if($ZipDir){Copy-Item -LiteralPath (Join-Path $ZipDir ($t.name+'.zip')) -Destination $zip}
 else{"$($t.name): downloading $($t.url)";Invoke-WebRequest -Uri $t.url -OutFile $zip -UseBasicParsing}
 $hash=(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
 if($hash -ne $t.sha256){Remove-Item -LiteralPath $zip;throw "$($t.name): SHA-256 mismatch ($hash)"}
 $out=if($t.sub){Join-Path $dest $t.sub}else{$dest}
 Expand-Archive -LiteralPath $zip -DestinationPath $out -Force
 Remove-Item -LiteralPath $zip
 if(!(Test-Path -LiteralPath (Join-Path $dest $t.check))){throw "$($t.name): $($t.check) not found after extracting"}
 "$($t.name): ok"
}
