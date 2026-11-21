<#
.SYNOPSIS
    为 wss 冒烟测试生成本地自签证书（仅联调，不入库）。

.DESCRIPTION
    生成 build/test-certs/cert.pem（自签，同时当 CA 用）与 key.pem：
      CN = localhost
      SAN = DNS:localhost, IP:127.0.0.1, IP:::1
    默认有效期 825 天。私钥只落在 build/ 下，而 build/ 已被 .gitignore 忽略，
    不要把它提交到仓库。

.PARAMETER OutDir
    输出目录，默认 <repo>/build/test-certs

.PARAMETER Days
    证书有效期（天），默认 825

.PARAMETER OpenSsl
    openssl.exe 路径；默认依次找 PATH、Git for Windows

.PARAMETER Force
    已存在时覆盖重新生成

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\certs\gen_test_certs.ps1

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests\certs\gen_test_certs.ps1 -Force -Days 3650
#>
param(
    [string]$OutDir = (Join-Path $PSScriptRoot '..\..\build\test-certs'),
    [int]$Days = 825,
    [string]$OpenSsl = '',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

function Resolve-OpenSsl {
    param([string]$Explicit)
    if ($Explicit) {
        if (-not (Test-Path $Explicit)) {
            throw "指定的 openssl 不存在: $Explicit"
        }
        return $Explicit
    }
    $cmd = Get-Command openssl -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }
    $candidates = @(
        (Join-Path $env:ProgramFiles 'Git\usr\bin\openssl.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Git\usr\bin\openssl.exe'),
        (Join-Path $env:ProgramFiles 'Git\mingw64\bin\openssl.exe')
    )
    foreach ($p in $candidates) {
        if ($p -and (Test-Path $p)) {
            return $p
        }
    }
    throw "找不到 openssl。请安装 Git for Windows，或用 -OpenSsl 指定 openssl.exe。"
}

$OutDir = [System.IO.Path]::GetFullPath($OutDir)
$certPath = Join-Path $OutDir 'cert.pem'
$keyPath = Join-Path $OutDir 'key.pem'

if ((Test-Path $certPath) -and (Test-Path $keyPath) -and -not $Force) {
    Write-Host "已存在: $certPath"
    Write-Host "已存在: $keyPath"
    Write-Host "如需重新生成请加 -Force。"
    exit 0
}

$openssl = Resolve-OpenSsl $OpenSsl
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# openssl 3.x 的配置文件；SAN 里同时给域名与两个回环 IP，
# 这样 wss://localhost 与 wss://127.0.0.1 两条校验路径都能测到。
$cfgPath = Join-Path $OutDir 'openssl-test.cnf'
@'
[req]
distinguished_name = dn
prompt = no
x509_extensions = v3_req

[dn]
CN = localhost

[v3_req]
basicConstraints = critical,CA:TRUE
keyUsage = critical,digitalSignature,keyEncipherment,keyCertSign
extendedKeyUsage = serverAuth
subjectAltName = @alt_names

[alt_names]
DNS.1 = localhost
IP.1 = 127.0.0.1
IP.2 = ::1
'@ | Set-Content -Path $cfgPath -Encoding ascii

& $openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days $Days `
    -keyout $keyPath -out $certPath -config $cfgPath
if ($LASTEXITCODE -ne 0) {
    throw "openssl 生成证书失败（exit $LASTEXITCODE）"
}

Write-Host ''
Write-Host "证书: $certPath"
Write-Host "私钥: $keyPath  （仅本地联调，不要提交）"
Write-Host ('用法: $env:XRTC_CA_FILE = "' + $certPath + '"')
Write-Host ''
& $openssl x509 -in $certPath -noout -subject -dates -ext subjectAltName
