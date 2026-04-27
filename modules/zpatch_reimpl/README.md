# FO2 ZPatch Reimplementation

Clean reimplementation experiment for selected `ZPatchFO2_v2.4` features on the current Steam `FlatOut 2` executable.

The goal is not to ship the old replacement exe. Current Steam `FlatOut2.exe` is already OpenSpy-patched, while the old ZPatch package includes an older exe that still contains `gamespy.com` strings. This project should target the installed Steam exe with runtime ASI patches and pattern scans where practical.

## Initial Scope

First-pass targets:

- `SkipIntro`
- `UncapFPS`
- `FramePacingFix`
- `RemoveVSync`
- `BorderlessWindowed`
- `WidescreenFix`
- Menu car file-size limits
- Menu car backface culling

Later / harder targets:

- `WidescreenFix_FOVScaling`
- Splitscreen menu/fix support

## Install For Testing

Copy these next to `FlatOut2.exe`:

- `fo2_zpatch_reimpl.asi`
- `fo2_zpatch_reimpl.ini`

Requires an ASI loader such as the existing `winmm.dll` / Ultimate ASI Loader setup.

Recommended performance setting:

```ini
UncapFPS=1
FramePacingFix=1
```

`FramePacingFix` removes the old limiter's `Sleep(1)` yield, which can cause visible 50 FPS-like pacing dips on modern Windows. `UncapFPS` keeps the stock timing interval intact and only bypasses the render gate. `FPSLimit` is intentionally not included: changing the game's frame-gate interval can speed up simulation, and the D3D Present-based limiter only worked reliably through `dxwrapper.dll`, which hurt pacing in testing.

## Build

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

## Research Notes

Observed from `D:\SteamLibrary\steamapps\common\FlatOut2\ZPatchFO2_v2.4`:

- `ZPatchFO2.asi` is a 32-bit ASI with minimal imports: `VirtualProtect`, basic file APIs, and a few `USER32` window APIs.
- `ZPatchFO2.ini` exposes the feature list we are cloning behaviorally.
- `ZPatchFO2BFS` contains `splitscreen.bfs`.
- The bundled `FlatOut2.exe` differs from the installed Steam exe mostly in OpenSpy/GameSpy strings and trailing overlay/signature data.
- Installed Steam exe already uses `openspy.net`; bundled old exe uses `gamespy.com`.
- Therefore `UseOpenSpy` should be treated as already handled for current Steam builds, or implemented as a compatibility fallback later.
