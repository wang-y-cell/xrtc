# 手动从 GitHub Release 拉取 WebRTC 预编译包（可选；CMake 配置时也会自动下载）
param(
    [string]$Url = "https://github.com/wang-y-cell/xrtc/releases/download/v1.0.0/webrtc.7z",
    [string]$OutDir = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
)

$ErrorActionPreference = "Stop"
$archive = Join-Path $env:TEMP "webrtc.7z"

Write-Host "Downloading $Url ..."
Invoke-WebRequest -Uri $Url -OutFile $archive

Write-Host "Extracting to $OutDir ..."
$sevenZip = @(
    "$env:ProgramFiles\7-Zip\7z.exe",
    "${env:ProgramFiles(x86)}\7-Zip\7z.exe",
    "7z"
) | Where-Object { $_ -eq "7z" -or (Test-Path $_) } | Select-Object -First 1

if ($sevenZip) {
    & $sevenZip x -y "-o$OutDir" $archive | Out-Null
} else {
    throw "未找到 7-Zip。请安装 7-Zip，或直接运行 cmake 配置（会自动下载并解压）。"
}

Remove-Item $archive -Force
$includeOk = Test-Path (Join-Path $OutDir "webrtc\include\api\peer_connection_interface.h")
$dbgOk = Test-Path (Join-Path $OutDir "webrtc\lib_debug\webrtc.lib")
$relOk = Test-Path (Join-Path $OutDir "webrtc\lib_release\webrtc.lib")
if (-not ($includeOk -and $dbgOk -and $relOk)) {
    throw "解压后布局不正确，需要 webrtc/include、webrtc/lib_debug/webrtc.lib、webrtc/lib_release/webrtc.lib"
}
Write-Host "OK: webrtc SDK ready under $(Join-Path $OutDir 'webrtc')"
