# Builds peussok_engine.dll, then stages the engine folder that is shipped into <repo>\build\stage\engine.
[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$cmake=Join-Path $root 'toolchain\cmake-4.4.5-windows-x86_64\bin\cmake.exe'
$ninja=Join-Path $root 'toolchain\ninja\ninja.exe'
if(!(Test-Path -LiteralPath $cmake)){throw 'Run tools\Get-Toolchain.ps1 first.'}
$saved=$env:ZIG_GLOBAL_CACHE_DIR
try {
 $env:ZIG_GLOBAL_CACHE_DIR=Join-Path $root 'toolchain\cache'
 $build=Join-Path $root 'build\engine'
 $internal=if($env:PK_INTERNAL -eq '1'){'-DCS_INTERNAL_VFG=ON'}else{'-DCS_INTERNAL_VFG=OFF'}  # 2.2.8: NVIDIA VFG slot in internal builds only
 & $cmake -S $PSScriptRoot -B $build -G Ninja "-DCMAKE_TOOLCHAIN_FILE=$PSScriptRoot/zig-toolchain.cmake" "-DCMAKE_MAKE_PROGRAM=$ninja" '-DCMAKE_BUILD_TYPE=Release' '-DCMAKE_POLICY_VERSION_MINIMUM=3.5' $internal
 if($LASTEXITCODE){throw 'CMake configure failed'}
 & $cmake --build $build --parallel 6
 if($LASTEXITCODE){throw 'Build failed'}
}finally{$env:ZIG_GLOBAL_CACHE_DIR=$saved}
# Stage: engine DLL, RIFE 4.6 + 4.15 lite models, FSR 3 notice, licenses, file manifest.
$dest=Join-Path $root 'build\stage\engine'
if(Test-Path -LiteralPath $dest){Remove-Item -LiteralPath $dest -Recurse -Force}
New-Item -ItemType Directory -Path (Join-Path $dest 'model'),(Join-Path $dest 'rife415lite'),(Join-Path $dest 'engines\fsr3') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'peussok_engine.dll') -Destination $dest
foreach($file in @('flownet.param','flownet.bin')){
 Copy-Item -LiteralPath (Join-Path $root "third_party\rife-ncnn-vulkan\models\rife-v4.6\$file") -Destination (Join-Path $dest 'model')
 Copy-Item -LiteralPath (Join-Path $root "third_party\rife-ncnn-vulkan\models\rife-v4.15-lite\$file") -Destination (Join-Path $dest 'rife415lite')
}
Copy-Item -LiteralPath (Join-Path $root 'third_party\fidelityfx-sdk-1.1.4\LICENSE.txt') -Destination (Join-Path $dest 'engines\fsr3\LICENSE-FidelityFX.txt')
# The two bridge DLLs need the NVIDIA / AMD SDK files, which are not in this repository (PK_DLSSG_SRC / PK_AMDFG_SRC); without them
# those two models are skipped and the rest builds normally.
$dsrc0=if($env:PK_DLSSG_SRC){$env:PK_DLSSG_SRC}else{(Join-Path $root 'sdk\dlssg')}
if(-not (Test-Path (Join-Path $dsrc0 'sdk\include'))){Write-Warning "DLSS SDK not found ($dsrc0): DLSS FG is left out"}else{ & { # DLSS FG slot. pk_dlssg.dll = our NGX DLSSG wrapper (public DLSS SDK headers stay outside this tree).
 $dsrc=if($env:PK_DLSSG_SRC){$env:PK_DLSSG_SRC}else{(Join-Path $root 'sdk\dlssg')};$zig=Join-Path $root 'toolchain\zig-x86_64-windows-0.14.1\zig.exe'
 New-Item -ItemType Directory -Path (Join-Path $dest 'engines\dlssg') -Force|Out-Null
 $env:ZIG_GLOBAL_CACHE_DIR=Join-Path $root 'toolchain\cache'
 & $zig c++ -target x86_64-windows-gnu -O2 -std=c++17 -shared -Wno-nonportable-include-path -I (Join-Path $dsrc 'sdk\include') (Join-Path $PSScriptRoot 'dlssg\dlssg_ngx.cpp') -o (Join-Path $dest 'engines\dlssg\pk_dlssg.dll') -ld3d12 -ldxgi -ladvapi32 -lpsapi
 $env:ZIG_GLOBAL_CACHE_DIR=$saved
 if($LASTEXITCODE){throw 'pk_dlssg build failed'}
 Get-ChildItem (Join-Path $dest 'engines\dlssg') -Filter '*.pdb'|Remove-Item;Get-ChildItem (Join-Path $dest 'engines\dlssg') -Filter '*.lib'|Remove-Item
 # nvngx_dlssg.dll (NVIDIA-signed, public Streamline/DLSS SDK 310.9.16) is bundled in the installer under the NVIDIA RTX SDK
 # license (copied beside it). It is never part of the source repository (build/ and dist/ are ignored).
 $nsrc=if($env:PK_DLSSG_RUNTIME_SRC){$env:PK_DLSSG_RUNTIME_SRC}else{(Join-Path $root 'sdk\streamline-bin')}
 Copy-Item -LiteralPath (Join-Path $nsrc 'nvngx_dlssg.dll') -Destination (Join-Path $dest 'engines\dlssg')
 Copy-Item -LiteralPath (Join-Path $nsrc 'nvngx_dlss.license.txt') -Destination (Join-Path $dest 'engines\dlssg\LICENSE-NVIDIA-RTX-SDK.txt')
 foreach($f in @('NOTICE-NVIDIA-DLSSG.txt')){Copy-Item -LiteralPath (Join-Path $PSScriptRoot "dlssg\$f") -Destination (Join-Path $dest 'engines\dlssg')}
} }
# the extra RIFE models (4.17-4.26) are no longer shipped (only the measured models are kept).
# AMD FSR FG and the NVIDIA VFG notice are staged in public builds too.
$zig=Join-Path $root 'toolchain\zig-x86_64-windows-0.14.1\zig.exe';$saved=$env:ZIG_GLOBAL_CACHE_DIR
$asrc0=if($env:PK_AMDFG_SRC){$env:PK_AMDFG_SRC}else{(Join-Path $root 'sdk\fidelityfx')}
if(-not (Test-Path (Join-Path $asrc0 'sdk\Kits'))){Write-Warning "FidelityFX SDK 2.4 not found ($asrc0): AMD FSR FG is left out"}
& {
 if(-not (Test-Path (Join-Path $asrc0 'sdk\Kits'))){return}
 # AMD FSR FG slot (our pk_amdfg.dll + AMD's signed DLLs and license, unmodified)
 $asrc=if($env:PK_AMDFG_SRC){$env:PK_AMDFG_SRC}else{(Join-Path $root 'sdk\fidelityfx')}
 New-Item -ItemType Directory -Path (Join-Path $dest 'engines\amdfg') -Force|Out-Null
 $env:ZIG_GLOBAL_CACHE_DIR=Join-Path $root 'toolchain\cache'
 & $zig c++ -target x86_64-windows-gnu -O2 -std=c++17 -shared -Wno-nonportable-include-path -Wno-deprecated-declarations -I (Join-Path $asrc 'sdk\Kits\FidelityFX\api\include') -I (Join-Path $asrc 'sdk\Kits\FidelityFX\framegeneration\include') (Join-Path $PSScriptRoot 'amdfg\fsr31_dx12.cpp') -o (Join-Path $dest 'engines\amdfg\pk_amdfg.dll') -ld3d12 -ldxgi -lpsapi
 $env:ZIG_GLOBAL_CACHE_DIR=$saved
 if($LASTEXITCODE){throw 'pk_amdfg build failed'}
 Get-ChildItem (Join-Path $dest 'engines\amdfg') -Include '*.pdb','*.lib' -Recurse|Remove-Item
 foreach($f in @('amd_fidelityfx_framegeneration_dx12.dll','amd_fidelityfx_loader_dx12.dll','LICENSE.md','3rdpartynotice.md')){Copy-Item -LiteralPath (Join-Path $asrc "rt\$f") -Destination (Join-Path $dest 'engines\amdfg')}
 Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'amdfg\NOTICE-AMD-FG.txt') -Destination (Join-Path $dest 'engines\amdfg')
}
# NVIDIA VFG notice and download script: in every build, also without the AMD SDK.
New-Item -ItemType Directory -Path (Join-Path $dest 'engines\nvvfg') -Force|Out-Null;foreach($f in @('NOTICE-NVIDIA-VFG.txt','Get-NvidiaVfg.ps1')){Copy-Item -LiteralPath (Join-Path $PSScriptRoot "nvvfg\$f") -Destination (Join-Path $dest 'engines\nvvfg')}
# official Vulkan loader (LunarG runtime components zip) for PCs whose graphics driver left no vulkan-1.dll. Not part of
# the source repository; the hash is the one the application checks (engine/vulkan_pin.h).
$vsrc=if($env:PK_VULKANRT_SRC){$env:PK_VULKANRT_SRC}else{(Join-Path $root 'sdk\vulkan-runtime')}
if(-not (Test-Path (Join-Path $vsrc 'x64\vulkan-1.dll'))){Write-Warning "Vulkan runtime components not found ($vsrc): engine\vulkan is left out"}else{
 $pin=[regex]::Match([IO.File]::ReadAllText((Join-Path $root 'engine\vulkan_pin.h')),'PK_VULKAN_SHA256 "([0-9A-F]{64})"').Groups[1].Value
 if((Get-FileHash -LiteralPath (Join-Path $vsrc 'x64\vulkan-1.dll') -Algorithm SHA256).Hash -ne $pin){throw 'vulkan-1.dll does not match engine\vulkan_pin.h'}
 New-Item -ItemType Directory -Path (Join-Path $dest 'vulkan') -Force|Out-Null
 Copy-Item -LiteralPath (Join-Path $vsrc 'x64\vulkan-1.dll'),(Join-Path $vsrc 'VulkanRT-License.txt') -Destination (Join-Path $dest 'vulkan')
}
$files=@(Get-ChildItem $dest -Recurse -File | Where-Object {$_.Extension -in @('.dll','.bin','.param')})
@($files|ForEach-Object{[ordered]@{file=$_.FullName.Substring($dest.Length+1);sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}})|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $dest 'manifest.json') -Encoding UTF8
Copy-Item -LiteralPath (Join-Path $root 'LICENSE'),(Join-Path $root 'LICENSE-EXCEPTION.txt') -Destination $dest
foreach($notice in @('THIRD_PARTY_NOTICES.txt','source-manifest.json')){Copy-Item -LiteralPath (Join-Path $PSScriptRoot $notice) -Destination $dest}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'licenses') -Destination $dest -Recurse
Get-FileHash -LiteralPath (Join-Path $dest 'peussok_engine.dll') -Algorithm SHA256
