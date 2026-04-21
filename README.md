# FO2 Skip Track

`FO2 Skip Track` is a small ASI plugin for `FlatOut 2` that restarts the current music selection from a single button press.

## Quick Start

1. Download the latest 32-bit [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases).
2. Put `winmm.dll` in your `FlatOut 2` folder next to `FlatOut2.exe`.
3. Copy `fo2_skip_track.asi` and `fo2_skip_track.ini` into the same folder.
4. Launch the game and press `N` or `LEFT_SHOULDER`.

GitHub release zips produced by this repo include:

- `fo2_skip_track.asi`
- `fo2_skip_track.ini`
- `winmm.dll` from the latest 32-bit [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases)

It supports:

- keyboard trigger with `N` by default
- direct XInput controller trigger with `LEFT_SHOULDER` by default

## What It Does

The plugin hooks into the running game process, calls native music restart functions discovered in the `FlatOut2.exe` binary, and exposes that behavior through a configurable hotkey and controller binding.

Current default behavior:

- keyboard: `N`
- controller: `LEFT_SHOULDER`

When triggered, the plugin requests both gameplay and title/menu music restart paths. In practice this gives one shared skip action that works across game contexts.

## Files

- `fo2_skip_track.cpp`: plugin source
- `build.ps1`: 32-bit build script for Visual Studio Build Tools
- `fo2_skip_track.ini`: sample runtime config

## Build

Requirements:

- Visual Studio 2022 Build Tools with x86 C++ tools installed

Build from PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

The build script creates:

- `fo2_skip_track.dll`
- `fo2_skip_track.asi`

## Install

Copy these files into the `FlatOut 2` game root:

- `fo2_skip_track.asi`
- `fo2_skip_track.ini`

This plugin requires an ASI loader.

Recommended loader:

- [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases)

For the Steam version of `FlatOut 2`, use the 32-bit release and place `winmm.dll` in the game root next to `FlatOut2.exe`.

Install summary:

1. Download the latest 32-bit `Ultimate ASI Loader` release from [ThirteenAG/Ultimate-ASI-Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases).
2. Extract `winmm.dll` into the `FlatOut 2` game folder.
3. Copy `fo2_skip_track.asi` and `fo2_skip_track.ini` into the same folder.

If you are using a packaged GitHub release from this repo, `winmm.dll` is already included in the zip.

## Config

Example:

```ini
[skip_track]
hotkey_vk=78
controller_buttons=LEFT_SHOULDER
```

### `hotkey_vk`

Windows virtual-key code for the keyboard trigger.

Default:

- `78` for `N`

### `controller_buttons`

One or more XInput button names joined with `+`.

Examples:

- `LEFT_SHOULDER`
- `BACK+RIGHT_SHOULDER`
- `Y`

Supported names:

- `A`
- `B`
- `X`
- `Y`
- `BACK`
- `START`
- `LEFT_SHOULDER`
- `RIGHT_SHOULDER`
- `LEFT_THUMB`
- `RIGHT_THUMB`
- `DPAD_UP`
- `DPAD_DOWN`
- `DPAD_LEFT`
- `DPAD_RIGHT`

## Notes

- The plugin is built for the 32-bit Steam version of `FlatOut 2`.
- It was tested locally with direct XInput polling.
- Function RVAs are currently hardcoded for the tested executable build. If the game executable changes, those addresses may need updating.
- The recommended loader setup uses `Ultimate ASI Loader` as `winmm.dll`.
