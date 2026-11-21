# Builds JuiceBar.exe with the bundled MinGW-w64 (UCRT) toolchain.
# The toolchain lives in .toolchain\mingw64 so nothing is installed system-wide.
param(
    [string]$Version = "0.1.0",
    [switch]$Tests,
    [switch]$VerboseOutput
)

$ErrorActionPreference = "Continue"
$root = $PSScriptRoot
$bin = Join-Path $root ".toolchain\mingw64\bin"
$gxx = Join-Path $bin "g++.exe"
$windres = Join-Path $bin "windres.exe"

if (-not (Test-Path $gxx)) {
    throw "MinGW-w64 not found at $bin. Run: powershell -File scripts\toolchain.ps1"
}

$build = Join-Path $root "build"
New-Item -ItemType Directory -Force $build | Out-Null

# Shared version header for the resource script.
@"
#pragma once
#define JB_VERSION_STRING "$Version"
"@ | Set-Content -Path (Join-Path $build "version.h") -Encoding ASCII

& $windres -i (Join-Path $root "res\app.rc") -o (Join-Path $build "app_res.o") `
    --include-dir (Join-Path $root "res") --include-dir $build
if ($LASTEXITCODE -ne 0) { throw "windres failed" }

$sources = Get-ChildItem (Join-Path $root "src\*.cpp") | Where-Object { $_.Name -ne "webpanel.cpp" } | ForEach-Object { $_.FullName }
$flags = @(
    "-std=c++17", "-O2", "-municode", "-mwindows", "-static", "-s",
    "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-missing-field-initializers",
    "-Wno-cast-function-type",
    "-D_WIN32_WINNT=0x0A00", "-DWINVER=0x0A00", "-DUNICODE", "-D_UNICODE",
    # No PE timestamp: two builds of the same commit are byte-identical, so anyone can verify a
    # release binary by rebuilding it.
    "-Wl,--no-insert-timestamp"
)
$libs = @(
    "-lgdiplus", "-lhid", "-lsetupapi", "-lcfgmgr32", "-lole32", "-loleaut32",
    "-lcomctl32", "-lgdi32", "-lshell32", "-lshlwapi", "-ladvapi32", "-luuid", "-luser32", "-ldwmapi", "-ldbghelp"
)

if ($VerboseOutput) { Write-Host "Compiling $($sources.Count) files..." }
& $gxx @flags $sources (Join-Path $build "app_res.o") -o (Join-Path $build "JuiceBar.exe") @libs
if ($LASTEXITCODE -ne 0) { throw "compilation failed" }

$exe = Get-Item (Join-Path $build "JuiceBar.exe")
Write-Host ("Built {0} ({1:N0} KB)" -f $exe.Name, ($exe.Length / 1KB))

if ($Tests) {
    & (Join-Path $root "test.ps1")
    if ($LASTEXITCODE -ne 0) { throw "tests failed" }
}
