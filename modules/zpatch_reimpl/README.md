# FO2 ZPatch Reimplementation

ASI reimplementation of selected `ZPatchFO2_v2.4` patches for the current Steam version of `FlatOut 2`.

## Install

Copy these files next to `FlatOut2.exe`:

- `fo2_zpatch_reimpl.asi`
- `fo2_zpatch_reimpl.ini`
- `fo2_splitscreen.bfs`
- `fo2_splitscreen_filesystem`
- `winmm.dll`

`winmm.dll` is the Ultimate ASI Loader. It is included in release packages.

## Reimplemented Patches

- `SkipIntro`: skips the startup intro videos.
- `UncapFPS`: bypasses the render gate while keeping the original game timing interval intact.
- `RemoveVSync`: requests immediate D3D9 presentation.
- `BorderlessWindowed`: optional borderless windowed mode.
- `WidescreenFix`: patches widescreen menu/layout scaling for modern aspect ratios.
- `WidescreenFix_FOVScaling`: patches garage/race camera FOV and projection behavior for widescreen and ultrawide displays.
- `SplitscreenFix`: mounts the repo-owned split-screen menu archive and enables the dormant native two-player paths only while the game reports `GM_SPLITSCREEN`.
- `SplitscreenPostProcessingFix`: experimental, separately opt-in full-frame post-processing for split screen. It has no behavioral effect unless `SplitscreenFix` is also enabled.
- `SplitscreenZoomInputFix`: optional compatibility for Zoom Platform's controller-to-`SendInput` Return synthesis at split-screen ready prompts. It has no effect unless `SplitscreenFix` is enabled.

## New Patches

- `SkipLicenseScreen`: hides the license/copyright artwork and removes its five-second startup delay.
- `FramePacingFix`: removes the stock frame limiter's `Sleep(1)` yield and requests 1 ms timer resolution. This patch is not from original ZPatchFO2. It is included to reduce uneven pacing on modern Windows while preserving the original timing interval.

## Omitted Patches

- `UseOpenSpy`: omitted because the current Steam executable already uses OpenSpy.
- `TryToSkipAllErrors`: omitted because blindly skipping game errors is risky and can hide real crashes or data problems.
- Menu car file-size limit patches: not implemented.
- Menu car backface-culling patch: not implemented.

## Split-screen Prototype

`SplitscreenFix=1` is opt-in. It requires `fo2_splitscreen.bfs` and `fo2_splitscreen_filesystem` next to the executable. The filesystem list contains exactly `fo2_splitscreen.bfs` with no BOM or trailing line ending; the stock parser treats a terminal CR/LF as an additional empty archive record. The archive contains two replacements: `data/scripts/multiplayermenu.bed`, derived from ZPatchFO2 v2.4, and the stock-PC `data/scripts/partymodemenu.bed` with its PC virtual-keyboard path preserved. The race-start function finishes populating both local-player records before setting `GM_SPLITSCREEN` and posting `EVENT_RACE_BEGIN` once. ZPatch posted that one-shot event from inside the population loop.

Player creation keeps the stock PC name and car flow. The first unclaimed zero-based input index is used as a provisional default when available; assignment is edited directly on the two-player party roster instead of opening a separate screen. At two players the otherwise-disabled `ADD PLAYER` action becomes `INPUT DEVICES`. Its inline editor shows two fixed-width rows using `index+1: Input.GetControllerName(index)`, so identical pads remain distinguishable without changing the stock name/car strings or coordinates. Up/Down chooses the player and Left/Right cycles the current device. Choosing the other player's device atomically swaps the two assignments, so keyboard/pad ownership can be reversed without ever creating a duplicate. Confirm commits, while Back restores the assignments captured on entry. The editor re-reads `Input.GetNumControllers()` for every change and launch validation rejects missing, out-of-range, or duplicate indices. The stored zero-based `input_index` is copied directly to `PlayerInfo.Controller`; keyboard plus one controller normally uses 0/1, while controller plus controller requires two enumerated pads and normally uses 1/2. Both party launch and final race launch revalidate exactly two distinct in-range indices.

Device assignment no longer depends on `GUI:GetLastActiveController`, controller allow masks, or native keyboard attribution. The PC menus remain shared/any-device; their allowed-controller table is defensively reset when entering player setup, the party roster, and the race selector. With exactly two players the party roster focuses `GO RACE` by default, so either keyboard Enter or a controller confirm can advance without changing player ownership.

The ready/start gate at `0x0045BC5E` uses the event's zero-based physical source index and the current unaccepted prompt ordinal at `GameFlow+0x9CC`. It reads that prompt's explicit `PlayerInfo.Controller` from `GameFlow+0x624+ordinal*0x44`, validates all indices against the input-manager device count, and accepts only an exact integer match. A match still runs the stock duplicate-source check at `0x0045F960`; a mismatch or invalid pointer/index takes the stock handled-event epilogue without advancing the prompt.

`SplitscreenZoomInputFix=1` addresses a Zoom Platform compatibility issue without modifying or removing Zoom. The supported Zoom build translates a controller confirmation into `USER32!SendInput` keyboard Return, causing the split ready gate to see keyboard source zero regardless of which pad was pressed. For the verified Zoom PE timestamp `0x692D78D4` and `SizeOfImage 0x106000`, the ASI parses the live import table for `USER32.dll!SendInput`, verifies the IAT target is the real User32 export, and atomically replaces only that import pointer. Its x86 thunk accepts only the verified key-edge return sites, captures Zoom's controller-edge pointer, validates the `+0x0C..+0x17` edge range against Zoom's live device vector, and reads the corresponding native game input slot. A Return key-down is queued and forwarded only when that slot already matches the current prompt ordinal's explicit selection. Unknown, out-of-range, or wrong-pad key-downs are fail-closed and reported as inserted so they cannot masquerade as physical keyboard slot zero; key-up and unrelated mixed-array entries pass through to avoid stuck input. When the valid resulting ready event arrives as keyboard source zero, a fresh (at most 750 ms), single-use queue item must match the same controller slot and prompt ordinal before translation. Physical keyboard input has no Zoom queue item and therefore retains ordinary source-zero behavior. If Zoom is absent, older game versions continue normally. Unknown Zoom timestamps/image layouts, missing imports, or pre-hooked IAT targets skip only this optional compatibility layer with a clear log; core split-screen remains installed. The original pointer is retained and restored on DLL detach if the slot still belongs to this ASI.

The ASI mounts the archive through the game's stock filesystem-list parser. The startup hook at `0x0054FF2E` configures the logical local-player count at `[0x008E844C]+0x08` to two when `SplitscreenFix` is enabled. It separately replays the stock write of `EDI=1` to `[0x008E844C]+0x14`, which xref analysis identifies as mutable current-player/action context; changing both fields through an earlier `mov edi,2` is unnecessarily invasive. The count hook is necessarily feature-scoped rather than mode-scoped: its containing function is called only from the startup path at `0x00521134`, before game flow or a race mode exists. The split-screen launch guard at `0x004947C4` requires the `+0x08` count to be at least two; leaving it at one posts event `0x7E0` and returns to the menu.

The other four input detours use the live game-mode field at `[*0x008E8410 + 0x464]`: value `10` selects split-screen routing, while a null game-flow pointer or any other mode replays the exact stock instructions and branches. This avoids ZPatch's four permanent input-routing bypasses outside split-screen mode. Disabling `SplitscreenFix` leaves both stock startup writes unchanged because none of the hooks are installed.

The mount, one feature-scoped startup-slot hook, the ready-owner gate, four mode-scoped routing hooks, and the post-processing dispatch hook form one signature-checked eight-site transaction. The implementation validates the pinned executable layout, PE timestamp, all eight complete instruction sequences, and both support files before its first write. It installs the mount last, verifies every branch, and restores every earlier site if installation fails. With `SplitscreenPostProcessingFix=0`, the post-processing dispatch replays the exact single stock call.

`SplitscreenPostProcessingFix=1` is experimental. The stock call at `0x004CBB26` can run with player 2's half-screen viewport selected even though the post-process implementation captures and filters shared full-device targets. The hook temporarily selects `(0,0,renderer_width,renderer_height)`, invokes the stock post process exactly once, restores layout record 1's viewport, and resumes. A repeated post pass is deliberately avoided because it would feed already-processed shared targets into the second pass. This may still expose a split seam or shader-specific edge artifacts and adds a full-frame post-process cost, so it defaults to off and can be toggled independently of input/split functionality.

`GM_SPLITSCREEN` is deliberately set only at race launch; the shared party-menu creation path calls `GameFlow.ClearRace()`, so cancel/back cannot leave split routing active.

The versioned source for the archive is under `assets/splitscreen`; `fo2_splitscreen.bfs` itself is an ignored build output. The module build uses checksum-pinned `bfstool` 1.1.0 to create a v2, file-version-`05050420` archive, then lists and extracts it and checks both payload hashes. The source crate is pinned to SHA-256 `AE6B7277763BBD827D8C261E2BEB202E6F03B98C24C94CAF947351D90C8D1BC0` and is licensed MIT OR Apache-2.0. The tool is cached under the repository's ignored `.tools` directory after the first Rust/Cargo build. `bfstool` metadata is not byte-reproducible across processes because its Huffman tie ordering passes through randomized map iteration; the verified file set and contents are deterministic even when the final archive hash differs.

## Default Config

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
```

## Build

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

The module build generates `fo2_splitscreen.bfs` from the versioned files under `assets\splitscreen` and writes the exact 19-byte `fo2_splitscreen_filesystem` list. The first build requires Rust/Cargo, `tar.exe`, and internet access to obtain checksum-verified `bfstool` 1.1.0 source; later builds reuse the verified `.tools` cache. Archive bytes may vary because upstream metadata ordering is nondeterministic, so the build verifies the exact path list and extracted payload hashes instead.
