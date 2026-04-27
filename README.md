# FlatOut 2 Mods

Combined source repo for the FlatOut 2 patch/mods we have been building together.

This repo keeps the three working components as separate modules, but builds and packages them as one installable set:

- `fo2_zpatch_reimpl.asi`: selected ZPatch-style fixes for the current Steam executable.
- `dinput8.dll`: XInput gamepad rumble proxy.
- `fo2_skip_track.asi`: keyboard/controller music track skip.

## Install

Build a release package:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

Copy everything from `dist\` into the `FlatOut 2` folder next to `FlatOut2.exe`.

Or install directly to the default Steam path:

```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

The release package includes the known-good `winmm.dll` ASI loader we tested with:

- SHA-256: `9A2BB218AB014FA4AD56104108C26AE48BF3E22595C126CD4F41367C799DA722`
- File version: `1.0.0.0`
- Description: `Ultimate ASI Loader`

Avoid `dxwrapper.dll` and DXVK for now. Both made frame pacing feel worse in our testing, even when they appeared functional.

## Components

### ZPatch Reimplementation

Source: `modules\zpatch_reimpl`

Current defaults:

```ini
SkipIntro=1
UncapFPS=1
FramePacingFix=1
RemoveVSync=1
BorderlessWindowed=0
WidescreenFix=1
WidescreenFix_FOVScaling=1
```

`FPSLimit` is intentionally not included. Changing the game's frame-gate interval sped up simulation, and the D3D Present-based limiter only worked reliably through `dxwrapper.dll`, which hurt pacing.

### XInput Rumble

Source: `modules\xinput_rumble`

Installs as `dinput8.dll` and forwards normal DirectInput calls while adding XInput rumble from player damage/contact events.

### Skip Track

Source: `modules\skip_track`

Default controls:

```ini
hotkey_vk=78
controller_buttons=LEFT_SHOULDER
```

`78` is the Windows virtual-key code for `N`.

## Build Requirements

- Windows
- Visual Studio 2022 C++ build tools with x86 toolchain
- PowerShell

Each module still has its own `build.ps1` for focused development. The top-level `build.ps1` calls all module builds and gathers the installable files into `dist\`.

## Notes

This is not a one-DLL merge yet. Keeping the modules separate is deliberate: the current combination is tested and stable, while a single binary would require careful hook-order and proxy-loader work.
