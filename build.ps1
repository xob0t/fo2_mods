$ErrorActionPreference = "Stop"

$dist = Join-Path $PSScriptRoot "dist"
if (Test-Path -LiteralPath $dist) {
  Remove-Item -LiteralPath $dist -Recurse -Force
}
New-Item -ItemType Directory -Path $dist | Out-Null

$modules = @(
  @{
    Name = "skip_track"
    Path = Join-Path $PSScriptRoot "modules\skip_track"
    Outputs = @("fo2_skip_track.asi", "fo2_skip_track.ini")
  },
  @{
    Name = "xinput_rumble"
    Path = Join-Path $PSScriptRoot "modules\xinput_rumble"
    Outputs = @("dinput8.dll", "fo2_xinput_rumble.ini")
  },
  @{
    Name = "zpatch_reimpl"
    Path = Join-Path $PSScriptRoot "modules\zpatch_reimpl"
    Outputs = @("fo2_zpatch_reimpl.asi", "fo2_zpatch_reimpl.ini")
  }
)

foreach ($module in $modules) {
  Write-Host "Building $($module.Name)..."
  powershell -ExecutionPolicy Bypass -File (Join-Path $module.Path "build.ps1")

  foreach ($output in $module.Outputs) {
    $source = Join-Path $module.Path $output
    if (-not (Test-Path -LiteralPath $source)) {
      throw "Expected build output missing: $source"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $dist $output) -Force
  }
}

$loader = Join-Path $PSScriptRoot "third_party\UltimateASILoader\winmm.dll"
if (-not (Test-Path -LiteralPath $loader)) {
  throw "Expected ASI loader missing: $loader"
}
Copy-Item -LiteralPath $loader -Destination (Join-Path $dist "winmm.dll") -Force

Write-Host "Built release package in $dist"
