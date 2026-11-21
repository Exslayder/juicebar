# Runs the unit tests for the forecast model, the history format and the voltage curve.
$ErrorActionPreference = "Continue"
$root = $PSScriptRoot
$bin = Join-Path $root ".toolchain\mingw64\bin"
$gxx = Join-Path $bin "g++.exe"

if (-not (Test-Path $gxx)) { throw "MinGW-w64 not found at $bin" }

$build = Join-Path $root "build"
New-Item -ItemType Directory -Force $build | Out-Null

& $gxx -std=c++17 -O2 -static -Wall -Wextra -I (Join-Path $root "src") `
    (Join-Path $root "tests\test_core.cpp") `
    (Join-Path $root "src\estimate.cpp") `
    (Join-Path $root "src\history.cpp") `
    (Join-Path $root "src\util.cpp") `
    -o (Join-Path $build "tests.exe")
if ($LASTEXITCODE -ne 0) { throw "test build failed" }

& (Join-Path $build "tests.exe")
$code = $LASTEXITCODE
if ($code -ne 0) { Write-Host "Tests failed ($code)." -ForegroundColor Red }
else { Write-Host "Tests passed." -ForegroundColor Green }
exit $code
