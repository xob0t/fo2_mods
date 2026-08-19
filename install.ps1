param(
  [string]$GamePath = "D:\SteamLibrary\steamapps\common\FlatOut2"
)

$ErrorActionPreference = "Stop"

$dist = Join-Path $PSScriptRoot "dist"
if (-not (Test-Path -LiteralPath $dist)) {
  powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "build.ps1")
}

if (-not (Test-Path -LiteralPath (Join-Path $GamePath "FlatOut2.exe"))) {
  throw "FlatOut2.exe was not found in: $GamePath"
}

$files = @(
  "winmm.dll",
  "fo2_orbit_camera.asi",
  "fo2_orbit_camera.ini",
  "fo2_skip_track.asi",
  "fo2_skip_track.ini",
  "dinput8.dll",
  "fo2_xinput_rumble.ini",
  "fo2_zpatch_reimpl.asi",
  "fo2_zpatch_reimpl.ini",
  "fo2_splitscreen.bfs",
  "fo2_splitscreen_filesystem"
)

foreach ($file in $files) {
  Copy-Item -LiteralPath (Join-Path $dist $file) -Destination (Join-Path $GamePath $file) -Force
}

Write-Host "Installed FO2 mod suite to $GamePath"
