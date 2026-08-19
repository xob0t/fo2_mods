$ErrorActionPreference = "Stop"

$src = Join-Path $PSScriptRoot "fo2_orbit_camera.cpp"
$out = Join-Path $PSScriptRoot "fo2_orbit_camera.dll"
$asi = Join-Path $PSScriptRoot "fo2_orbit_camera.asi"
$obj = Join-Path $PSScriptRoot "fo2_orbit_camera.obj"
$testExe = Join-Path $PSScriptRoot "fo2_orbit_camera_tests.exe"
$testObj = Join-Path $PSScriptRoot "fo2_orbit_camera_tests.obj"

if (Test-Path -LiteralPath $obj) {
  Remove-Item -LiteralPath $obj -Force
}
if (Test-Path -LiteralPath $testObj) {
  Remove-Item -LiteralPath $testObj -Force
}

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
  if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "Could not find cl.exe in PATH and vswhere.exe is missing."
  }

  $installationPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
  if (-not $installationPath) {
    throw "Could not find a Visual Studio installation with x86 C++ tools."
  }

  $vcvars = Join-Path $installationPath "VC\Auxiliary\Build\vcvars32.bat"
  if (-not (Test-Path -LiteralPath $vcvars)) {
    throw "Could not find vcvars32.bat at $vcvars"
  }

  $cmd = @(
    "call `"$vcvars`"",
    "cl /nologo /std:c++17 /EHsc /O2 /MT /DORBIT_CAMERA_SELF_TEST_EXE /Fo:`"$testObj`" /Fe:`"$testExe`" `"$src`"",
    "`"$testExe`"",
    "cl /nologo /std:c++17 /EHsc /O2 /MT /LD /Fo:`"$obj`" /Fe:`"$out`" `"$src`""
  ) -join " && "

  cmd /c $cmd
} else {
  & cl.exe /nologo /std:c++17 /EHsc /O2 /MT /DORBIT_CAMERA_SELF_TEST_EXE /Fo:"$testObj" /Fe:"$testExe" "$src"
  if ($LASTEXITCODE -ne 0) {
    throw "Self-test build failed with exit code $LASTEXITCODE"
  }
  & $testExe
  if ($LASTEXITCODE -ne 0) {
    throw "Self-tests failed with exit code $LASTEXITCODE"
  }
  & cl.exe /nologo /std:c++17 /EHsc /O2 /MT /LD /Fo:"$obj" /Fe:"$out" "$src"
}

if ($LASTEXITCODE -ne 0) {
  throw "Build failed with exit code $LASTEXITCODE"
}

Copy-Item -LiteralPath $out -Destination $asi -Force
Remove-Item -LiteralPath $testExe, $testObj -Force -ErrorAction SilentlyContinue
Write-Host "Built $out and $asi"
