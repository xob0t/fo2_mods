# FlatOut 2 Mods

Runtime mods for `FlatOut 2`, tested on the Steam version.

Fully vibecoded, rigorously tested.

## Mods

- `fo2_zpatch_reimpl`: widescreen/FOV fixes, license and intro skips, FPS unlock, frame pacing fix, v-sync removal, borderless windowed mode.
- `fo2_xinput_rumble`: XInput controller rumble with directional feedback and gameplay-event rumble.
- `fo2_skip_track`: music track skip from keyboard or controller.

## Install

Download the release zip for the mod you want, then extract it into the `FlatOut 2` folder next to `FlatOut2.exe`.

ASI-based mods include `winmm.dll`, the known-good Ultimate ASI Loader build used for this game:

- SHA-256: `9A2BB218AB014FA4AD56104108C26AE48BF3E22595C126CD4F41367C799DA722`
- File version: `1.0.0.0`
- Description: `Ultimate ASI Loader`

Avoid `dxwrapper.dll` with these mods unless you specifically need it. It can make frame pacing worse.

## `fo2_zpatch_reimpl`

Source: `modules\zpatch_reimpl`

Install files:

- `fo2_zpatch_reimpl.asi`
- `fo2_zpatch_reimpl.ini`
- `winmm.dll`

Default fixes:

```ini
SkipLicenseScreen=1
SkipIntro=1
UncapFPS=1
FramePacingFix=1
RemoveVSync=1
BorderlessWindowed=0
WidescreenFix=1
WidescreenFix_FOVScaling=1
```

Feature notes:

- `SkipLicenseScreen` hides the license/copyright artwork and removes its five-second startup delay.
- `SkipIntro` bypasses the startup intro videos.
- `UncapFPS` unlocks rendering while keeping the original game timing interval intact.
- `FramePacingFix` improves the stock frame gate by removing the old `Sleep(1)` yield that can cause uneven pacing on modern Windows.
- `RemoveVSync` requests immediate D3D9 presentation.
- `BorderlessWindowed` is available but disabled by default.
- `WidescreenFix` and `WidescreenFix_FOVScaling` fix ultrawide/widescreen menu, garage, and race camera behavior.

## `fo2_xinput_rumble`

Source: `modules\xinput_rumble`

Install files:

- `dinput8.dll`
- `fo2_xinput_rumble.ini`

This is a DirectInput 8 proxy that forwards the game's normal input calls and adds XInput rumble for modern controllers.

It supports:

- player damage rumble
- player contact/collision rumble
- car-to-car impact rumble
- object/rubble contact rumble
- scrape/wall-hug rumble
- rough landing feedback
- directional rumble bias, so left/right impacts can favor the corresponding motor
- configurable strength, duration, cooldown, envelopes, and synthetic button-test rumble

The default config is tuned for an Xbox-style controller on controller index `0`.

## `fo2_skip_track`

Source: `modules\skip_track`

Install files:

- `fo2_skip_track.asi`
- `fo2_skip_track.ini`
- `winmm.dll`

Default controls:

```ini
hotkey_vk=78
controller_buttons=LEFT_SHOULDER
```

`78` is the Windows virtual-key code for `N`.

The mod hooks FlatOut 2 music playback routines and requests a new track selection when triggered, thus skipping current track.

## Build

Requirements:

- Windows
- PowerShell
- Visual Studio 2022 C++ build tools with the x86 toolchain

Build all mods:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

The top-level build creates `dist\` with all runtime files:

- `winmm.dll`
- `fo2_zpatch_reimpl.asi`
- `fo2_zpatch_reimpl.ini`
- `dinput8.dll`
- `fo2_xinput_rumble.ini`
- `fo2_skip_track.asi`
- `fo2_skip_track.ini`

Install the built files to the default Steam path:

```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

Each module also has its own `build.ps1` for building only that mod.
