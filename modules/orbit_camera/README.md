# FO2 Orbit Camera

`FO2 Orbit Camera` is a 32-bit ASI plugin for the Steam version of `FlatOut 2`. It adds an absolute right-stick orbit view to the active chase camera while preserving the game's native tracking, collision avoidance, field of view, and shake pipeline.

## Install

Copy these files next to `FlatOut2.exe`:

- `fo2_orbit_camera.asi`
- `fo2_orbit_camera.ini`
- `winmm.dll`

The repository's top-level build and install scripts package and install all three files.

## Input and camera behavior

The right stick selects an absolute camera angle rather than accumulating rotation:

- idle or down: native rear chase direction (`0` degrees)
- right: right-side view (`+90` degrees)
- up: look-back view (`180` degrees)
- left: left-side view (`-90` degrees)
- diagonals: intermediate angles

The plugin keeps independent orbit state for each controller and resets state when camera ownership, device assignment, configuration, or chase-camera eligibility changes.

Right-stick input is capability-gated. Automatic input prefers the assigned SDL gamepad with the verified Zoom Platform build, falls back to the game's validated native DirectInput state for stock pads, and uses the split-screen adapter's validated state for player four. Unknown devices or invalid state leave the native camera unchanged.

See `fo2_orbit_camera.ini` for the shipped defaults and configurable deadzones, response curve, smoothing, return, speed limit, and axis inversion options.

`Source=Auto` is recommended. `Source=ZoomSDL` requires the verified Zoom/SDL build and otherwise leaves the stock camera untouched. `Source=Native` uses only validated native DirectInput state (and the validated player-four adapter). The radial `AxisCurveExponent` changes engagement response without changing the absolute stick direction.

## Building from source

Requirements:

- Windows
- PowerShell
- Visual Studio 2022 C++ build tools with the x86 toolchain

From this directory, run:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

The build produces `fo2_orbit_camera.dll` and copies it to `fo2_orbit_camera.asi` for installation.
