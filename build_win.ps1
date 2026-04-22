# Windows 下编译：设置 OS=Windows_NT，自动探测 WinGet 安装的 MinGW/WinLibs 的 bin
$ErrorActionPreference = "Stop"
$env:OS = "Windows_NT"
Set-Location $PSScriptRoot

$mingwBin = $null
if (Get-Command gcc -ErrorAction SilentlyContinue) {
    $mingwBin = (Split-Path (Get-Command gcc).Source -Parent)
}
if (-not $mingwBin) {
    $cand = Get-ChildItem "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Recurse -Filter "mingw32-make.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
    if ($cand) { $mingwBin = Split-Path $cand -Parent }
}
if ($mingwBin) {
    $env:Path = "$mingwBin;$env:Path"
}

if (-not (Get-Command mingw32-make -ErrorAction SilentlyContinue) -and -not (Get-Command make -ErrorAction SilentlyContinue)) {
    Write-Host "未找到 mingw32-make。可安装: winget install -e --id BrechtSanders.WinLibs.POSIX.UCRT" -ForegroundColor Yellow
    exit 1
}

$make = "mingw32-make"
if (-not (Get-Command mingw32-make -ErrorAction SilentlyContinue)) { $make = "make" }

& $make @args
exit $LASTEXITCODE
