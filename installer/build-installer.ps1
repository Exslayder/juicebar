# Builds the Windows installer and the portable archive for JuiceBar.
# Requires Inno Setup 6+ (https://jrsoftware.org/isinfo.php). On CI Chocolatey installs it.
param(
    [Parameter(Mandatory = $true)][string]$Version
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dist = Join-Path $root "dist"
$exe = Join-Path $root "build\JuiceBar.exe"

if (-not (Test-Path $exe)) { throw "Build the application first: build\JuiceBar.exe is missing." }

function Find-Iscc {
    if ($env:ISCC -and (Test-Path $env:ISCC)) { return $env:ISCC }
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 7\ISCC.exe"),
        (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"),
        "C:\Program Files (x86)\Inno Setup 7\ISCC.exe",
        "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
        "C:\Program Files\Inno Setup 7\ISCC.exe",
        "C:\Program Files\Inno Setup 6\ISCC.exe"
    )
    foreach ($path in $candidates) { if (Test-Path $path) { return $path } }
    $command = Get-Command "iscc.exe" -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    return $null
}

$iscc = Find-Iscc
if (-not $iscc) {
    $choco = Get-Command "choco.exe" -ErrorAction SilentlyContinue
    if ($choco) {
        Write-Host "Inno Setup not found, installing with Chocolatey..."
        & $choco.Source install innosetup -y --no-progress
        $iscc = Find-Iscc
    }
}
if (-not $iscc) {
    throw "Inno Setup (ISCC.exe) was not found. Install Inno Setup 6+ or set the ISCC environment variable."
}

New-Item -ItemType Directory -Force $dist | Out-Null
Remove-Item (Join-Path $dist "JuiceBar-$Version-setup.exe") -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $dist "JuiceBar-$Version-portable.zip") -Force -ErrorAction SilentlyContinue

Write-Host "Building installer with $iscc ..."
& $iscc "/DMyVersion=$Version" (Join-Path $PSScriptRoot "juicebar.iss")
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed with exit code $LASTEXITCODE" }

Write-Host "Building portable archive ..."
$stage = Join-Path $env:TEMP ("juicebar-portable-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force $stage | Out-Null
try {
    Copy-Item $exe (Join-Path $stage "JuiceBar.exe")
    Copy-Item (Join-Path $root "LICENSE") $stage
    Copy-Item (Join-Path $root "README.md") $stage
    Copy-Item (Join-Path $root "CHANGELOG.md") $stage
    # Keeps settings and history next to the exe instead of in AppData.
    New-Item -ItemType File (Join-Path $stage "portable.flag") | Out-Null
    Compress-Archive -Path (Join-Path $stage "*") -DestinationPath (Join-Path $dist "JuiceBar-$Version-portable.zip") -Force
} finally {
    Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue
}

Get-ChildItem $dist | Select-Object Name, Length | Format-Table -AutoSize
