# PEUSSOK: one-click NVIDIA VFG runtime download (started from the model menu after the user agrees).
# Source: the official NVIDIA package index https://pypi.nvidia.com/nvidia-vfx/ (no login). Nothing is downloaded from anywhere else.
# Steps: download the wheel (zip) -> check SHA-256 -> unpack only the 5 runtime DLLs -> check each is NVIDIA-signed -> delete the wheel.
param([string]$Dest = (Join-Path $env:LOCALAPPDATA 'Peussok\nvidia-vfg'))
$ErrorActionPreference = 'Stop'
$wheel = 'nvidia_vfx-0.2.0.0-cp312-abi3-win_amd64.whl'
$url = "https://pypi.nvidia.com/nvidia-vfx/$wheel"
$sha = 'CEBAA12411F7E22B39293205C5C4D07B8073D14CA1E0E7727EE4375207A36445'
$need = @('cudart64_12.dll', 'NVCVImage.dll', 'nvngxruntime.dll', 'NVVideoEffects.dll', 'nvVFXVideoFrameGeneration.dll')
$Host.UI.RawUI.WindowTitle = 'PEUSSOK - NVIDIA VFG 받기'
Write-Host 'NVIDIA VFG 런타임을 NVIDIA 공식 서버에서 받아요 (약 416 MB).'
Write-Host "주소: $url"
Write-Host "설치 위치: $Dest"
Write-Host ''
try {
    New-Item -ItemType Directory -Force $Dest | Out-Null
    $tmp = Join-Path $env:TEMP ("cs-vfg-" + [guid]::NewGuid().ToString('N') + '.whl')
    Add-Type -AssemblyName System.Net.Http
    $client = New-Object System.Net.Http.HttpClient
    $client.Timeout = [TimeSpan]::FromMinutes(30)
    $resp = $client.GetAsync($url, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).Result
    if (-not $resp.IsSuccessStatusCode) { throw "다운로드 실패: HTTP $([int]$resp.StatusCode)" }
    $total = $resp.Content.Headers.ContentLength
    $in = $resp.Content.ReadAsStreamAsync().Result; $out = [System.IO.File]::Create($tmp)
    $buf = New-Object byte[] (1MB); $done = 0L; $last = -1
    while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) {
        $out.Write($buf, 0, $n); $done += $n
        $pct = if ($total) { [int](100 * $done / $total) } else { 0 }
        if ($pct -ne $last) { Write-Progress -Activity 'NVIDIA VFG 받는 중' -Status ("{0:N0} / {1:N0} MB" -f ($done / 1MB), ($total / 1MB)) -PercentComplete $pct; $last = $pct }
    }
    $out.Close(); $in.Close(); $client.Dispose()
    Write-Progress -Activity 'NVIDIA VFG 받는 중' -Completed
    Write-Host '파일 확인 중 (SHA-256)...'
    $h = (Get-FileHash -LiteralPath $tmp -Algorithm SHA256).Hash
    if ($h -ne $sha) { throw "파일이 NVIDIA 공개 값과 달라요 (SHA-256 $h)" }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [System.IO.Compression.ZipFile]::OpenRead($tmp)
    try {
        foreach ($name in $need) {
            $e = $zip.Entries | Where-Object { $_.FullName -eq "nvvfx/libs/$name" } | Select-Object -First 1
            if (-not $e) { throw "패키지 안에 $name 이 없어요" }
            $target = Join-Path $Dest $name
            [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, $target, $true)
            $sig = Get-AuthenticodeSignature -LiteralPath $target
            if ($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'O=NVIDIA Corporation') { Remove-Item -LiteralPath $target -Force; throw "$name 의 NVIDIA 서명을 확인하지 못했어요" }
        }
        $lic = $zip.Entries | Where-Object { $_.FullName -match '\.dist-info/(LICENSE|licenses/)' }
        foreach ($e in $lic) { [System.IO.Compression.ZipFileExtensions]::ExtractToFile($e, (Join-Path $Dest ('NVIDIA-' + $e.Name)), $true) }
    } finally { $zip.Dispose() }
    Remove-Item -LiteralPath $tmp -Force
    "nvidia-vfx 0.2.0.0 $wheel sha256=$sha installed=$(Get-Date -Format s)" | Set-Content -LiteralPath (Join-Path $Dest 'SOURCE.txt') -Encoding UTF8
    Write-Host ''
    Write-Host '설치를 마쳤어요. 프레임 생성 설정에서 NVIDIA VFG를 고른 뒤 적용을 누르세요.' -ForegroundColor Green
    exit 0
} catch {
    if ($tmp -and (Test-Path -LiteralPath $tmp)) { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue }
    Write-Host ''
    Write-Host ("설치하지 못했어요: " + $_.Exception.Message) -ForegroundColor Red
    exit 1
} finally {
    if (-not $env:PK_VFG_NO_PAUSE) { Write-Host ''; Read-Host '창을 닫으려면 Enter' | Out-Null }
}
