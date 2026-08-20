# FlatOut 2 PC split-screen architecture

This document describes the split-screen code that ships in `fo2_zpatch_reimpl`. It targets the current Steam executable:

- image base `0x00400000`
- image size `0x00541000`
- PE timestamp `0x451D02BD`
- SHA-256 `B7F691D098A167AD0C6C6FF4604DE6F653EFDCF93B6ACBDD03960973A122641F`

The ASI checks executable bytes before every patch transaction. Unsupported layouts lose the affected capability instead of receiving a partial patch.

## What ships

- two-player horizontal split
- two-player vertical split
- three-player 2x2 grid with the bottom-right cell unused
- capability-gated four-player 2x2 grid
- per-player input assignment in the party screen
- Zoom Platform pad-identity recovery for the verified Zoom build
- optional full-frame post-processing repair
- generated BFS packaging for the two modified BED scripts

Three-player and four-player race behavior still needs broad runtime testing. The code paths build and pass static checks, but pause, restart, results, hotplug, and long-session teardown need real multiplayer sessions.

## GameFlow and player records

The global GameFlow pointer is stored at `0x008E8410`. The current mode is at `GameFlow+0x464`; split setup uses mode `10`.

PlayerInfo begins at `GameFlow+0x620`. The game owns eight records with a `0x44` stride. The fields used by the mod are:

| Offset in PlayerInfo | Meaning |
| ---: | --- |
| `+0x00` | player type |
| `+0x04` | zero-based input-manager slot |

The native local-player count helper at `0x0045DC80` checks all eight records. Local-player creation at `0x0045DA40` also loops over the records instead of assuming two players.

Each live local player receives a one-based ordinal at `live+0x368`. This field identifies the player that owns contact, camera, and race state. It is not an input-device index.

## Input manager

The input manager pointer is stored at `0x008E844C`. Its relevant fields are:

| Manager offset | Meaning |
| ---: | --- |
| `+0x04` | registered device count |
| `+0x08` | multi-player threshold state; the mod keeps this at `2` |
| `+0x10` | last aggregate input source |
| `+0x1C` | keyboard device pointer, slot 0 |
| `+0x20` | first stock pad, slot 1 |
| `+0x24` | second stock pad, slot 2 |
| `+0x28` | stock backend pointer, reused as slot 3 only after sidecar installation |

Stock PC code exposes keyboard plus two pads. That is enough for three players.

Four-player mode moves the original backend pointer into ASI-owned sidecar state and installs an XInput-backed device at manager slot 3. The adapter activates only when all stock devices exist and XInput user 2 is connected during manager construction. The transaction patches backend update, shutdown, and clear paths before publishing a device count of four.

If any check fails, the ASI restores the stock manager and advertises at most three players. The mod never treats `manager+0x28` as both a backend and a device.

The supported four-player topology is keyboard plus three pads. Four pads without a keyboard would require a fifth manager slot and a larger ready-claim table, so it is outside the current design.

## Ready prompts and ownership

The stock ready loop queries action 8 and records the source device. It then binds that source to the current PlayerInfo record and advances the ready ordinal.

The ASI validates the source against the device selected in the party screen. Bounds come from the active contiguous local-player count, capped by the installed capability. Two-player, three-player, and four-player prompts therefore use the same path.

Zoom Platform converts pad confirmation into synthetic keyboard Return input. For the verified Zoom DLL, the ASI hooks its `SendInput` import, identifies the originating SDL record, and queues its native slot. The ready hook consumes that slot once and rejects mismatched ownership. Unknown Zoom versions receive no Zoom-specific hook; native input handling remains active.

## Viewport layout

The viewport layout object lives at `GameFlow+0x9B8` and is also published through `0x00696DC8`.

The record count is at `layout+0x30`. Records begin at `layout+0x34` with a `0x18` stride:

| Record offset | Meaning |
| ---: | --- |
| `+0x00` | x |
| `+0x04` | y |
| `+0x08` | width |
| `+0x0C` | height |
| `+0x10` | owning live-player context |
| `+0x14` | zero-based ordinal |

The sole layout-builder call is at `0x0045CF1B`; the native builder is `0x00470820`.

The ASI calls the native builder first, then changes rectangle fields only:

- two-player horizontal keeps the stock top and bottom halves
- two-player vertical uses left and right halves
- three-player uses top-left, top-right, and bottom-left quadrants
- four-player keeps the stock four-quadrant result

Owner pointers and ordinals remain native. Odd dimensions give the remainder to the right or bottom cell, so the rectangles cover the complete render area without a gap.

## Projection and FOV

Two native projection sites special-case a count of two because stock assumes top and bottom halves:

- `0x004C9E27`
- `0x004CBD5D`

Horizontal split doubles the viewport aspect. Vertical split halves it. Grid layouts need no count-two correction because every cell halves both width and height.

The widescreen matrix helpers read the effective viewport aspect stored at projection offset `+0x118`. They preserve vertical FOV and calculate horizontal extent from that live aspect. They do not apply a second split multiplier.

## HUD and minimap

Stock player-two HUD placement assumes a bottom viewport. Vertical and grid modes route position text through the normal top anchor.

The two stock translucent position backgrounds overlap in vertical mode, so the vertical HUD hook skips that background loop. Text remains in the native draw order.

The shared split minimap runs inside the first player's render context. The hook at `0x004C1889` flushes queued HUD work, switches to a full-device viewport, draws the map once, flushes its markers, and restores the caller's viewport and scissor state.

Zoom and other widescreen patches can replace the pointer operand used by the map scaler at `0x004C6EEB`. The centering code follows that live operand instead of reading the stock `1/640` constant. This keeps the map centered on stock and ultrawide setups.

## Post-processing

The stock render loop leaves the last player viewport active when it reaches the shared post-process call. Running the effect once per player breaks effects that expect a full render target.

`SplitscreenPostProcessingFix=1` runs the shared post-process once with a full-device viewport, then restores the final valid layout record. The hook accepts counts two through four. The option is separate because shader behavior varies between mods.

## Camera setup

The native camera initializer loads `splitcamera.ini` only when the local count equals two. The ASI changes that check to accept every split count of at least two. Each local player still owns a separate camera controller and native live-player binding.

The separate `fo2_orbit_camera` module reads the assigned device for each local player. It controls only the chase camera and leaves other camera definitions unchanged.

## Party-screen scripts

The versioned BED sources are:

- `modules/zpatch_reimpl/assets/splitscreen/data/scripts/multiplayermenu.bed`
- `modules/zpatch_reimpl/assets/splitscreen/data/scripts/partymodemenu.bed`

The party screen has four actions: add player, edit inputs, remove player, and start race. The input editor hides the roster while open and creates one row per supported player.

For two players it also exposes horizontal or vertical layout. Three-player and four-player modes use the fixed grid.

The scripts query native capability through the `Input` table. They never infer four-player support from the number of names returned by `Input.GetNumControllers`.

## BFS build

`build_splitscreen_bfs.ps1` packs the two BED files into a FlatOut 2 BFS v2 archive with file version `05050420`. It then lists and extracts the archive and compares both payloads with the source files.

The build writes `fo2_splitscreen_filesystem` as exactly 19 bytes containing `fo2_splitscreen.bfs`. It has no BOM or line ending.

`bfstool` 1.1.0 does not produce byte-identical metadata on every run because its Huffman tie ordering uses randomized map iteration. The build therefore verifies archive meaning, not a fixed BFS hash.

## Patch transactions

The implementation uses separate transactions so one optional failure does not destroy unrelated fixes:

- core split ownership and filesystem hooks
- layout, projection, HUD, minimap, and camera presentation hooks
- four-player input lifecycle
- Zoom input compatibility
- optional post-processing

Each transaction checks every signature before its first write. It verifies installed branches and rolls back in reverse order after a failed write.

## Known limits

- Four-player mode needs XInput users 0, 1, and 2 to enumerate as the three pads before input-manager construction.
- SDL, DirectInput, and XInput order can differ on unusual mixed-controller setups. The ready path fails closed rather than assigning the wrong player.
- Hotplug does not preserve controller identity when Windows renumbers device indices.
- The rumble module still has one output channel. Any local player's event can rumble the configured controller.
- Three-player and four-player pause, restart, results, and teardown need more runtime coverage.

## Runtime test matrix

Before a release, test these cases on a fresh game process:

- one player with split disabled
- two players, horizontal and vertical
- three players with keyboard plus two pads
- four players with keyboard plus three pads
- reversed keyboard and pad ownership
- Zoom absent, verified Zoom present, and unsupported Zoom present
- post-processing on and off
- 16:9, 4:3, and ultrawide displays
- pause, restart, finish, next race, and return to menus
- repeated split entry and exit without restarting the process

For four players, confirm the log contains `Splitscreen4P: installed keyboard+three-pad topology` before expecting the party menu to expose player four.
