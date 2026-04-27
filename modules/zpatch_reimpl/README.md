# FO2 ZPatch Reimplementation

ASI reimplementation of selected `ZPatchFO2_v2.4` patches for the current Steam version of `FlatOut 2`.

## Install

Copy these files next to `FlatOut2.exe`:

- `fo2_zpatch_reimpl.asi`
- `fo2_zpatch_reimpl.ini`
- `winmm.dll`

`winmm.dll` is the Ultimate ASI Loader. It is included in release packages.

## Reimplemented Patches

- `SkipIntro`: skips the startup intro videos.
- `UncapFPS`: bypasses the render gate while keeping the original game timing interval intact.
- `RemoveVSync`: requests immediate D3D9 presentation.
- `BorderlessWindowed`: optional borderless windowed mode.
- `WidescreenFix`: patches widescreen menu/layout scaling for modern aspect ratios.
- `WidescreenFix_FOVScaling`: patches garage/race camera FOV and projection behavior for widescreen and ultrawide displays.

## New Patches

- `FramePacingFix`: removes the stock frame limiter's `Sleep(1)` yield and requests 1 ms timer resolution. This patch is not from original ZPatchFO2. It is included to reduce uneven pacing on modern Windows while preserving the original timing interval.

## Omitted Patches

- `UseOpenSpy`: omitted because the current Steam executable already uses OpenSpy.
- `TryToSkipAllErrors`: omitted because blindly skipping game errors is risky and can hide real crashes or data problems.
- `SplitscreenFix`: not implemented.
- Menu car file-size limit patches: not implemented.
- Menu car backface-culling patch: not implemented.

## Default Config

```ini
[General]
Log=1

[Fixes]
SkipIntro=1
UncapFPS=1
FramePacingFix=1
RemoveVSync=1
BorderlessWindowed=0
WidescreenFix=1
WidescreenFix_FOVScaling=1
```

## Build

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```
