# FO2 Orbit Camera

`FO2 Orbit Camera` is a 32-bit ASI plugin for the Steam version of `FlatOut 2`. It adds instant right-stick orbit control to the stock chase camera. It does not change any other camera mode.

## Install

Copy these files next to `FlatOut2.exe`:

- `fo2_orbit_camera.asi`
- `fo2_orbit_camera.ini`
- `winmm.dll`

The repository's top-level build and install scripts package and install all three files.

## Input and camera behavior

The right stick selects an absolute chase-camera angle:

- idle or forward: native chase direction at `0` degrees
- right: right-side view at `+90` degrees
- back, by pulling the stick down: rear-facing view at `180` degrees
- left: left-side view at `-90` degrees
- diagonals: intermediate angles

The camera snaps directly to the selected angle. It does not accumulate rotation, ease toward the target, or apply a speed limit. Holding left or right keeps a fixed side view. Releasing the stick returns control to the native chase camera.

The plugin keeps separate input state for each local player. It rotates the native chase-camera offset before the game handles collision, field of view, and shake. Cockpit, hood, bumper, replay, crash, and other camera modes stay untouched.

`Source=Auto` reads the assigned SDL gamepad when the verified Zoom Platform build is present. Otherwise it reads the game's DirectInput state for stock pads or the split-screen adapter state for player four. If the plugin cannot identify the assigned device, it leaves the camera alone.

See `fo2_orbit_camera.ini` for the deadzone, radial response curve, axis inversion, and input-source settings.

`Source=Auto` is the default. `Source=ZoomSDL` requires the verified Zoom/SDL build. `Source=Native` reads only native DirectInput state and the player-four adapter. `AxisCurveExponent` changes how far the stick must move without changing the selected angle.

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
