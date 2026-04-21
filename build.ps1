$ErrorActionPreference = "Stop"

$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"
$src = Join-Path $PSScriptRoot "fo2_skip_track.cpp"
$out = Join-Path $PSScriptRoot "fo2_skip_track.dll"
$asi = Join-Path $PSScriptRoot "fo2_skip_track.asi"
$obj = Join-Path $PSScriptRoot "fo2_skip_track.obj"

if (Test-Path $obj) {
  Remove-Item -LiteralPath $obj -Force
}

$cmd = @(
  "call `"$vcvars`"",
  "cl /nologo /std:c++17 /EHsc /O2 /MT /LD /Fe:`"$out`" `"$src`" user32.lib"
) -join " && "

cmd /c $cmd

if ($LASTEXITCODE -ne 0) {
  throw "Build failed with exit code $LASTEXITCODE"
}

Copy-Item -LiteralPath $out -Destination $asi -Force
Write-Host "Built $out and $asi"
