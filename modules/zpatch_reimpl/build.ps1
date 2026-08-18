$ErrorActionPreference = "Stop"

$src = Join-Path $PSScriptRoot "fo2_zpatch_reimpl.cpp"
$out = Join-Path $PSScriptRoot "fo2_zpatch_reimpl.asi"
$obj = Join-Path $PSScriptRoot "fo2_zpatch_reimpl.obj"
$splitscreenFilesystem = Join-Path $PSScriptRoot "fo2_splitscreen_filesystem"
$splitscreenFilesystemTemp = "$splitscreenFilesystem.tmp-$PID"
$splitscreenBuild = Join-Path $PSScriptRoot "build_splitscreen_bfs.ps1"

# Build the BFS from its versioned script sources. The helper bootstraps a
# checksum-pinned bfstool into the repo-local ignored tool cache and verifies
# the archive by listing and extracting it before this module is packaged.
& $splitscreenBuild

# The stock filesystem-list parser treats a terminal CR/LF as another empty
# archive record. Generate the one-entry list without a BOM or line ending.
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
try {
  [System.IO.File]::WriteAllText($splitscreenFilesystemTemp, "fo2_splitscreen.bfs", $utf8NoBom)
  $filesystemBytes = [System.IO.File]::ReadAllBytes($splitscreenFilesystemTemp)
  if ($filesystemBytes.Length -ne 19 -or
      [System.Text.Encoding]::ASCII.GetString($filesystemBytes) -ne "fo2_splitscreen.bfs") {
    throw "Generated split-screen filesystem list must be exactly 19 ASCII bytes with no BOM or line ending."
  }
  Move-Item -LiteralPath $splitscreenFilesystemTemp -Destination $splitscreenFilesystem -Force
} finally {
  if (Test-Path -LiteralPath $splitscreenFilesystemTemp) {
    Remove-Item -LiteralPath $splitscreenFilesystemTemp -Force
  }
}

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
    "cl /nologo /std:c++17 /EHsc /O2 /MT /LD /Fo:`"$obj`" /Fe:`"$out`" `"$src`" user32.lib d3d9.lib dxguid.lib"
  ) -join " && "

  cmd /c $cmd
} else {
  & cl.exe /nologo /std:c++17 /EHsc /O2 /MT /LD /Fo:"$obj" /Fe:"$out" "$src" user32.lib d3d9.lib dxguid.lib
}

if ($LASTEXITCODE -ne 0) {
  throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host "Built $out"
