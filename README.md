# FlatOut 2 Mods

Runtime mods for `FlatOut 2`, tested on the Steam version.

Fully vibecoded, rigorously tested.

## Mods

- `fo2_orbit_camera`: instant right-stick orbit control for the chase camera. Other camera modes stay untouched.
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

## `fo2_orbit_camera`

Source: `modules\orbit_camera`

Install files:

- `fo2_orbit_camera.asi`
- `fo2_orbit_camera.ini`
- `winmm.dll`

The mod controls only the stock chase camera. Cockpit, hood, bumper, replay, crash, and other camera modes stay untouched.

The right stick selects an absolute chase-camera angle:

- idle or forward: native chase view
- right: right-side view
- back: rear-facing view
- left: left-side view
- diagonals: intermediate angles

The camera snaps directly to the selected angle. It does not accumulate rotation, ease toward the target, or apply a speed limit. Releasing the stick returns control to the native chase camera.

The mod keeps separate input state for each local player. It rotates the native chase-camera offset before the game handles collision, field of view, and shake. `fo2_orbit_camera.ini` controls the deadzone, radial response curve, axis inversion, and input source.

`Source=Auto` reads the assigned SDL gamepad when the verified Zoom Platform build is present. Otherwise it reads the game's DirectInput state for stock pads or the split-screen adapter state for player four. If the plugin cannot identify the assigned device, it leaves the camera alone.

## `fo2_zpatch_reimpl`

Source: `modules\zpatch_reimpl`

Install files:

- `fo2_zpatch_reimpl.asi`
- `fo2_zpatch_reimpl.ini`
- `fo2_splitscreen.bfs`
- `fo2_splitscreen_filesystem`
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
SplitscreenFix=0
SplitscreenPostProcessingFix=0
SplitscreenZoomInputFix=1
```

Feature notes:

- `SkipLicenseScreen` hides the license/copyright artwork and removes its five-second startup delay.
- `SkipIntro` bypasses the startup intro videos.
- `UncapFPS` unlocks rendering while keeping the original game timing interval intact.
- `FramePacingFix` improves the stock frame gate by removing the old `Sleep(1)` yield that can cause uneven pacing on modern Windows.
- `RemoveVSync` requests immediate D3D9 presentation.
- `BorderlessWindowed` is available but disabled by default.
- `WidescreenFix` and `WidescreenFix_FOVScaling` fix ultrawide/widescreen menu, garage, and race camera behavior.
- `SplitscreenFix` is an opt-in two-, three-, or capability-gated four-player prototype. The party roster includes indexed input-device selectors and stores one distinct zero-based keyboard/pad index per player. Three players use the stock keyboard-plus-two-pad topology and the top-left, top-right, and bottom-left quadrants of a fixed 2x2 grid. Four players require all three pads to be connected before startup: an XInput-backed slot 3 is then installed transactionally alongside the stock keyboard and two stock pads, and the native four-quadrant grid is exposed. Four-pad/no-keyboard play is not supported.
- Split orientation is selected at runtime from the split-screen party setup and persisted by the ASI in `fo2_splitscreen_layout.lua`. `Vertical` uses left/right viewports and `Horizontal` uses top/bottom viewports; missing or invalid state defaults to `Horizontal`, which is also the safe fallback if native layout support cannot be installed. BED scripts never load the state file directly; a native `Input` query reports the authoritative active layout and capability.
- `SplitscreenPostProcessingFix` is a separate experimental opt-in. It performs the shared post-process once with a full-device viewport and restores the final player viewport afterward; leave it off if a shader shows seams or edge artifacts.
- `SplitscreenZoomInputFix` is version- and capability-gated Zoom Platform compatibility. For the verified Zoom build it translates Zoom-synthesized Return back to the originating pad slot during split ready prompts without suppressing the input. Unknown/no-Zoom installations keep the core split fix unchanged.

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
- Rust/Cargo and internet access for the first build only. The build downloads
  the `bfstool` 1.1.0 source crate, verifies its pinned SHA-256, and compiles it
  into the ignored `.tools\` cache. Later builds reuse the verified cached tool.

Build all mods:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

The top-level build creates `dist\` with all runtime files:

- `winmm.dll`
- `fo2_orbit_camera.asi`
- `fo2_orbit_camera.ini`
- `fo2_zpatch_reimpl.asi`
- `fo2_zpatch_reimpl.ini`
- `fo2_splitscreen.bfs`
- `fo2_splitscreen_filesystem`
- `dinput8.dll`
- `fo2_xinput_rumble.ini`
- `fo2_skip_track.asi`
- `fo2_skip_track.ini`

Install the built files to the default Steam path:

```powershell
powershell -ExecutionPolicy Bypass -File .\install.ps1
```

Each module also has its own `build.ps1` for building only that mod.

The split-screen BFS is a generated artifact, not versioned source. Its two
inputs live under `modules\zpatch_reimpl\assets\splitscreen`; the zpatch module
build creates a v2/file-version-`05050420` archive, checks that it lists exactly
the two expected BED paths, extracts it to a temporary directory, and verifies
both payload hashes before the top-level build copies it into `dist\`. The
19-byte `fo2_splitscreen_filesystem` file is likewise regenerated without a BOM
or trailing line ending.

`bfstool` 1.1.0 does not produce byte-identical archive metadata on every run
because some equal-frequency Huffman metadata is emitted from randomized map
iteration. Builds are therefore reproducible semantically (same paths and
payload hashes), but the generated BFS SHA-256 may differ between builds.
