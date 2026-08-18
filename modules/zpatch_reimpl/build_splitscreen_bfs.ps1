$ErrorActionPreference = "Stop"

$bfsToolVersion = "1.1.0"
$bfsToolCrateSha256 = "AE6B7277763BBD827D8C261E2BEB202E6F03B98C24C94CAF947351D90C8D1BC0"
$bfsToolCrateUrl = "https://static.crates.io/crates/bfstool/bfstool-$bfsToolVersion.crate"
$bfsFileVersion = "05050420"
$expectedPaths = @(
  "data/scripts/multiplayermenu.bed",
  "data/scripts/partymodemenu.bed"
)

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..\..")).Path
$assetRoot = Join-Path $PSScriptRoot "assets\splitscreen"
$outputArchive = Join-Path $PSScriptRoot "fo2_splitscreen.bfs"
$toolRoot = Join-Path $repoRoot ".tools\bfstool\$bfsToolVersion"
$toolExe = Join-Path $toolRoot "bin\bfstool.exe"
$toolSourceRoot = Join-Path $toolRoot "source"
$crateHashStamp = Join-Path $toolRoot "crate.sha256"
$toolHashStamp = Join-Path $toolRoot "bfstool.exe.sha256"
$cargoMetadataFiles = @(
  (Join-Path $toolRoot ".crates.toml"),
  (Join-Path $toolRoot ".crates2.json")
)

function Remove-CargoBuildArtifacts {
  if (Test-Path -LiteralPath $toolSourceRoot) {
    Remove-Item -LiteralPath $toolSourceRoot -Recurse -Force
  }
  foreach ($metadataFile in $cargoMetadataFiles) {
    if (Test-Path -LiteralPath $metadataFile) {
      Remove-Item -LiteralPath $metadataFile -Force
    }
  }
}

function Get-Sha256([string]$Path) {
  return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToUpperInvariant()
}

function Test-CachedTool {
  if (-not (Test-Path -LiteralPath $toolExe) -or
      -not (Test-Path -LiteralPath $crateHashStamp) -or
      -not (Test-Path -LiteralPath $toolHashStamp)) {
    return $false
  }

  $recordedCrateHash = ([System.IO.File]::ReadAllText($crateHashStamp)).Trim().ToUpperInvariant()
  $recordedToolHash = ([System.IO.File]::ReadAllText($toolHashStamp)).Trim().ToUpperInvariant()
  return $recordedCrateHash -eq $bfsToolCrateSha256 -and
         $recordedToolHash -eq (Get-Sha256 $toolExe)
}

function Install-PinnedBfsTool {
  $cargo = Get-Command cargo.exe -ErrorAction SilentlyContinue
  if (-not $cargo) {
    throw "bfstool $bfsToolVersion is not cached and cargo.exe is unavailable. Install the Rust toolchain, then rerun the build."
  }

  $tar = Get-Command tar.exe -ErrorAction SilentlyContinue
  if (-not $tar) {
    throw "bfstool $bfsToolVersion is not cached and tar.exe is unavailable. A current Windows tar.exe is required for the pinned source bootstrap."
  }

  $cacheRoot = Join-Path $toolRoot "cache"
  $crateArchive = Join-Path $cacheRoot "bfstool-$bfsToolVersion.crate"
  $download = "$crateArchive.download-$PID"

  New-Item -ItemType Directory -Path $cacheRoot -Force | Out-Null

  if (Test-Path -LiteralPath $crateArchive) {
    if ((Get-Sha256 $crateArchive) -ne $bfsToolCrateSha256) {
      Remove-Item -LiteralPath $crateArchive -Force
    }
  }

  if (-not (Test-Path -LiteralPath $crateArchive)) {
    Write-Host "Downloading pinned bfstool $bfsToolVersion source crate..."
    try {
      Invoke-WebRequest -UseBasicParsing -Uri $bfsToolCrateUrl -OutFile $download
      $downloadHash = Get-Sha256 $download
      if ($downloadHash -ne $bfsToolCrateSha256) {
        throw "bfstool source checksum mismatch: expected $bfsToolCrateSha256, got $downloadHash"
      }
      Move-Item -LiteralPath $download -Destination $crateArchive -Force
    } finally {
      if (Test-Path -LiteralPath $download) {
        Remove-Item -LiteralPath $download -Force
      }
    }
  }

  $crateHash = Get-Sha256 $crateArchive
  if ($crateHash -ne $bfsToolCrateSha256) {
    throw "Cached bfstool source checksum mismatch: expected $bfsToolCrateSha256, got $crateHash"
  }

  if (Test-Path -LiteralPath $toolSourceRoot) {
    Remove-Item -LiteralPath $toolSourceRoot -Recurse -Force
  }
  New-Item -ItemType Directory -Path $toolSourceRoot | Out-Null
  try {
    & $tar.Source -xf $crateArchive -C $toolSourceRoot
    if ($LASTEXITCODE -ne 0) {
      throw "Could not extract pinned bfstool source crate (tar exit $LASTEXITCODE)."
    }

    $crateSource = Join-Path $toolSourceRoot "bfstool-$bfsToolVersion"
    $cargoManifest = Join-Path $crateSource "Cargo.toml"
    $cargoLock = Join-Path $crateSource "Cargo.lock"
    if (-not (Test-Path -LiteralPath $cargoManifest) -or -not (Test-Path -LiteralPath $cargoLock)) {
      throw "Pinned bfstool source crate is missing Cargo.toml or Cargo.lock."
    }

    Write-Host "Building checksum-verified bfstool $bfsToolVersion into $toolRoot..."
    & $cargo.Source install --path $crateSource --locked --root $toolRoot --force
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $toolExe)) {
      throw "cargo install bfstool $bfsToolVersion failed (exit $LASTEXITCODE)."
    }

    [System.IO.File]::WriteAllText($crateHashStamp, $bfsToolCrateSha256)
    [System.IO.File]::WriteAllText($toolHashStamp, (Get-Sha256 $toolExe))
  } finally {
    Remove-CargoBuildArtifacts
  }
}

if (-not (Test-CachedTool)) {
  Install-PinnedBfsTool
}

# Migrate caches made by earlier helper revisions and never retain Cargo's
# extracted source/target tree after a successful or cached bootstrap.
Remove-CargoBuildArtifacts

$toolVersionOutput = (& $toolExe --version 2>&1 | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $toolVersionOutput -ne "bfstool $bfsToolVersion") {
  throw "Unexpected bfstool executable: expected 'bfstool $bfsToolVersion', got '$toolVersionOutput'."
}

if (-not (Test-Path -LiteralPath $assetRoot)) {
  throw "Split-screen asset directory is missing: $assetRoot"
}

$actualSourcePaths = @(
  Get-ChildItem -LiteralPath $assetRoot -Recurse -File |
    ForEach-Object { $_.FullName.Substring($assetRoot.Length + 1).Replace("\", "/") } |
    Sort-Object
)
$sortedExpectedPaths = @($expectedPaths | Sort-Object)
if (($actualSourcePaths -join "`n") -ne ($sortedExpectedPaths -join "`n")) {
  throw "Split-screen assets must contain exactly: $($sortedExpectedPaths -join ', '). Found: $($actualSourcePaths -join ', ')"
}

$temporaryArchive = "$outputArchive.tmp-$PID"
$verifyRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("fo2-splitscreen-bfs-verify-" + [Guid]::NewGuid().ToString("N"))

try {
  if (Test-Path -LiteralPath $temporaryArchive) {
    Remove-Item -LiteralPath $temporaryArchive -Force
  }

  & $toolExe archive `
    --compression zlib `
    --format v2 `
    --file-version $bfsFileVersion `
    --filter all `
    --copy-filter none `
    --level 9 `
    --align-front `
    --align 4096 `
    --no-progress `
    $temporaryArchive `
    $assetRoot
  if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $temporaryArchive)) {
    throw "bfstool failed to create the split-screen BFS (exit $LASTEXITCODE)."
  }

  $archiveBytes = [System.IO.File]::ReadAllBytes($temporaryArchive)
  if ($archiveBytes.Length -lt 20 -or
      [System.Text.Encoding]::ASCII.GetString($archiveBytes, 0, 4) -ne "bfs1" -or
      [System.BitConverter]::ToString($archiveBytes, 4, 4).Replace("-", "") -ne $bfsFileVersion -or
      [System.BitConverter]::ToUInt32($archiveBytes, 12) -ne $expectedPaths.Count -or
      [System.BitConverter]::ToUInt32($archiveBytes, 16) -ne 997) {
    throw "Generated archive has an unexpected BFS v2 header."
  }

  $listedPaths = @(
    & $toolExe list --format v2 --raw --no-progress $temporaryArchive |
      ForEach-Object { $_.ToString().Trim() } |
      Where-Object { $_ }
  )
  if ($LASTEXITCODE -ne 0 -or ($listedPaths -join "`n") -ne ($sortedExpectedPaths -join "`n")) {
    throw "Generated archive listing mismatch. Expected: $($sortedExpectedPaths -join ', '). Found: $($listedPaths -join ', ')"
  }

  New-Item -ItemType Directory -Path $verifyRoot | Out-Null
  & $toolExe extract --format v2 --no-progress $temporaryArchive $verifyRoot
  if ($LASTEXITCODE -ne 0) {
    throw "bfstool failed to extract the generated archive for verification (exit $LASTEXITCODE)."
  }

  $extractedPaths = @(
    Get-ChildItem -LiteralPath $verifyRoot -Recurse -File |
      ForEach-Object { $_.FullName.Substring($verifyRoot.Length + 1).Replace("\", "/") } |
      Sort-Object
  )
  if (($extractedPaths -join "`n") -ne ($sortedExpectedPaths -join "`n")) {
    throw "Round-trip extraction produced unexpected files: $($extractedPaths -join ', ')"
  }

  foreach ($relativePath in $expectedPaths) {
    $sourcePath = Join-Path $assetRoot ($relativePath.Replace("/", "\"))
    $extractedPath = Join-Path $verifyRoot ($relativePath.Replace("/", "\"))
    $sourceHash = Get-Sha256 $sourcePath
    $extractedHash = Get-Sha256 $extractedPath
    if ($sourceHash -ne $extractedHash) {
      throw "Round-trip hash mismatch for ${relativePath}: source=$sourceHash extracted=$extractedHash"
    }
  }

  Move-Item -LiteralPath $temporaryArchive -Destination $outputArchive -Force
} finally {
  if (Test-Path -LiteralPath $temporaryArchive) {
    Remove-Item -LiteralPath $temporaryArchive -Force
  }
  if (Test-Path -LiteralPath $verifyRoot) {
    Remove-Item -LiteralPath $verifyRoot -Recurse -Force
  }
}

Write-Host "Built and round-trip verified $outputArchive"
Write-Host "bfstool=$bfsToolVersion crate_sha256=$bfsToolCrateSha256 archive_sha256=$(Get-Sha256 $outputArchive)"
