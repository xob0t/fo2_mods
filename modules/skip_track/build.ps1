$ErrorActionPreference = "Stop"

$src = Join-Path $PSScriptRoot "fo2_skip_track.cpp"
$out = Join-Path $PSScriptRoot "fo2_skip_track.dll"
$asi = Join-Path $PSScriptRoot "fo2_skip_track.asi"
$obj = Join-Path $PSScriptRoot "fo2_skip_track.obj"

if (Test-Path $obj) {
  Remove-Item -LiteralPath $obj -Force
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path $vswhere)) {
    throw "Could not find cl.exe in PATH and vswhere.exe is missing."
  }

  $installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $installationPath) {
    throw "Could not find a Visual Studio installation with x86 C++ tools."
  }

  $vcvars = Join-Path $installationPath "VC\Auxiliary\Build\vcvars32.bat"
  if (-not (Test-Path $vcvars)) {
    throw "Could not find vcvars32.bat at $vcvars"
  }

  $cmd = @(
    "call `"$vcvars`"",
    "cl /nologo /std:c++17 /EHsc /O2 /MT /LD /Fo:`"$obj`" /Fe:`"$out`" `"$src`" user32.lib"
  ) -join " && "

  cmd /c $cmd
} else {
  & cl.exe /nologo /std:c++17 /EHsc /O2 /MT /LD /Fo:"$obj" /Fe:"$out" "$src" user32.lib
}

if ($LASTEXITCODE -ne 0) {
  throw "Build failed with exit code $LASTEXITCODE"
}

Copy-Item -LiteralPath $out -Destination $asi -Force
Write-Host "Built $out and $asi"
