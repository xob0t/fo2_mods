# FO2 ZPatch reimplementation

This 32-bit ASI ports selected ZPatchFO2 2.4 fixes to the current Steam build of `FlatOut 2`. It also contains the PC split-screen implementation used by this repository.

The hooks target one executable layout. Every patch checks its expected bytes before writing. A mismatched executable skips the affected patch instead of guessing.

## Install

Copy these files next to `FlatOut2.exe`:

- `fo2_zpatch_reimpl.asi`
- `fo2_zpatch_reimpl.ini`
- `fo2_splitscreen.bfs`
- `fo2_splitscreen_filesystem`
- `winmm.dll`

The split-screen files are harmless while `SplitscreenFix=0`.

## General fixes

- `SkipLicenseScreen` removes the license artwork and its five-second delay.
- `SkipIntro` skips the startup videos.
- `UncapFPS` removes the stock render cap.
- `FramePacingFix` removes the old `Sleep(1)` yield and requests a 1 ms timer period.
- `RemoveVSync` requests immediate D3D9 presentation.
- `BorderlessWindowed` enables borderless windowed mode.
- `WidescreenFix` repairs menu and HUD scaling on wide displays.
- `WidescreenFix_FOVScaling` uses the active viewport aspect for race and garage projection.

`FramePacingFix` is a repository addition, not an original ZPatchFO2 option.

## Split screen

Set `SplitscreenFix=1` to enable local split screen. The party screen assigns one input device to each player before the race.

Two players can choose horizontal or vertical layout in the same input editor. Horizontal is the default. The ASI stores the choice in `fo2_splitscreen_layout.lua` and applies it without a restart.

Three players use keyboard plus two pads and occupy the top-left, top-right, and bottom-left cells of a 2x2 grid.

Four players require keyboard plus three pads. The third pad must appear as XInput user 2 when the input manager starts. If the adapter cannot prove that setup, the party menu stops at three players. Four-pad play without a keyboard is not supported.

The native world, camera, and render loops already handle four local players. The ASI supplies the missing PC input slot, ready-prompt routing, three-player viewport layout, HUD placement, and menu flow.

`SplitscreenZoomInputFix=1` supports the verified Zoom Platform build. Zoom turns pad confirmation into synthetic keyboard Return input, so the ASI records the originating SDL pad and restores the correct player slot. Unknown Zoom builds do not receive this compatibility hook.

`SplitscreenPostProcessingFix=1` runs the shared post-process once across the full frame, then restores the last player viewport. It is experimental and remains off by default.

## Menu-car limits

The menu preview loader has separate model, skin, material, and surface limits.

- `MenuCarMaxModelFileSize` controls the model input allocation. Stock is `524288` bytes.
- `MenuCarMaxSkinFileSize` controls the skin input allocation. Stock is `2097152` bytes.
- `MenuCarBackfaceCulling=1` uses counter-clockwise culling while drawing the menu car.
- `MenuCarMaxSurfaces` controls bounded replacement material and surface tables. Stock is `16`; accepted values are 1 through 4096.

The file-size options reproduce the allocation operands used by ZPatchFO2. `MenuCarMaxSurfaces` is a repository extension for models that exceed both fixed 16-record tables. It redirects parser destinations, validates material links, and rejects invalid resource-tree links before dereference. Leave it at `16` unless a menu model needs more records.

## Default config

```ini
[General]
Log=1

[Fixes]
SkipLicenseScreen=1
SkipIntro=1
UncapFPS=1
FramePacingFix=1
RemoveVSync=1
BorderlessWindowed=0
WidescreenFix=1
WidescreenFix_FOVScaling=1
SplitscreenFix=0
SplitscreenPostProcessingFix=0
SplitscreenZoomInputFix=1
MenuCarBackfaceCulling=0
MenuCarMaxModelFileSize=524288
MenuCarMaxSkinFileSize=2097152
MenuCarMaxSurfaces=16
```

The log file is `fo2_zpatch_reimpl.log` beside the game executable.

## Generated split-screen archive

The two BED sources live under `assets/splitscreen`. The build creates `fo2_splitscreen.bfs`, lists and extracts it, checks both payload hashes, and writes the exact 19-byte `fo2_splitscreen_filesystem` mount list.

The build pins the `bfstool` 1.1.0 source crate by SHA-256 and caches the compiled executable under `.tools`. Upstream Huffman tie ordering makes archive metadata vary between processes, so validation compares paths and extracted bytes rather than a fixed archive hash.

## Build

Requirements:

- Windows
- PowerShell
- Visual Studio 2022 C++ build tools with the x86 toolchain
- Rust and Cargo for the first BFS build
- internet access for the first BFS build

Run:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

Later builds reuse the verified `.tools` cache.
