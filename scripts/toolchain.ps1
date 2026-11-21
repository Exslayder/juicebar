# Downloads the pinned portable MinGW-w64 (UCRT) toolchain into .toolchain\mingw64.
# Nothing is installed system-wide; delete .toolchain to remove it.
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tools = Join-Path $root ".toolchain"
$mingw = Join-Path $tools "mingw64"
$url = "https://github.com/brechtsanders/winlibs_mingw/releases/download/16.2.0posix-23.1.3-14.0.0-ucrt-r3/winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r3.zip"
$zip = Join-Path $tools "mingw.zip"
$sha256 = "3c69305081e4a647144339a936244fcf7d3d9e558319e1ad7be2cd2886d4b6a1"

if (Test-Path (Join-Path $mingw "bin\g++.exe")) {
    Write-Host "MinGW-w64 is already in $mingw"
    exit 0
}

New-Item -ItemType Directory -Force $tools | Out-Null
if (-not (Test-Path $zip)) {
    Write-Host "Downloading MinGW-w64 (about 270 MB)..."
    curl.exe -L --fail -o $zip $url
    if ($LASTEXITCODE -ne 0) { throw "download failed" }
}
$actual = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
if ($actual -ne $sha256) {
    Remove-Item $zip -Force
    throw "Toolchain checksum mismatch: expected $sha256, got $actual"
}

Write-Host "Extracting..."
tar.exe -xf $zip -C $tools
if ($LASTEXITCODE -ne 0) { throw "extraction failed" }
if (-not (Test-Path (Join-Path $mingw "bin\g++.exe"))) { throw "toolchain extraction did not produce $mingw" }
Remove-Item $zip -Force
Write-Host "Done: $mingw"
