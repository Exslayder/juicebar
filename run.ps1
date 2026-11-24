# Starts a development instance with its own data folder, so nothing lands in %APPDATA%.
param(
    [switch]$Panel,                 # open the panel on start (handy for screenshots)
    [string]$Data = "",             # defaults to .devdata next to this script
    [switch]$Stay                   # do not stop a running instance first
)

$root = $PSScriptRoot
$exe = Join-Path $root "build\WattPill.exe"
if (-not (Test-Path $exe)) { & (Join-Path $root "build.ps1") }

if (-not $Stay) {
    Get-Process WattPill -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep -Milliseconds 400
}
if (-not $Data) { $Data = Join-Path $root ".devdata" }
New-Item -ItemType Directory -Force $Data | Out-Null

$arguments = @("--data", "`"$Data`"")
if ($Panel) { $arguments += "--panel" }
Start-Process -FilePath $exe -ArgumentList $arguments
Write-Host "WattPill started with data in $Data"
