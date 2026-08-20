# FO2 XInput Rumble

Experimental `dinput8.dll` proxy for `FlatOut 2` that adds XInput controller rumble from gameplay events.

The module has one configured XInput output. It does not route rumble by split-screen player. In a split race, contact or damage from any local car can rumble that controller.

The PC executable contains old force-feedback plumbing, but testing did not produce reliable rumble events with an XInput controller during races. This proxy forwards normal DirectInput calls to Windows and adds XInput rumble from observed car events.

Current best build:

- Rumbles on local player damage.
- Rumbles on raw local player contact events before the game decides whether the contact becomes damage.
- Ignores AI/opponent damage using the player-car signature found in testing: race info `0x36c == 1` and car field `0x6b10 == 0`.
- Does not install the broader collision-sound fallback because it picked up nearby/world collision audio and could interfere with race controls.

## Install

Copy these files next to `FlatOut2.exe`:

- `dinput8.dll`
- `fo2_xinput_rumble.ini`

## Config

Default config:

```ini
[rumble]
enabled=1
controller_index=0
max_strength=100
default_duration_ms=250
log=1
directional_enabled=1
directional_bias=75
directional_min_motor=25
directional_invert=0

[synthetic]
enabled=0
buttons=A
left_strength=70
right_strength=100
pulse_ms=180
require_left_trigger=0
require_right_trigger=30

[events]
enabled=1
envelope_enabled=1
envelope_min_strength=70
envelope_kick_strength=100
envelope_kick_ms=45
envelope_gap_ms=25
envelope_sustain_percent=80
envelope_sustain_min_ms=220
envelope_sustain_max_ms=520
damage_enabled=1
damage_player_only=1
damage_player_index=-1
damage_min_delta_per_mille=1
damage_full_delta_per_mille=45
damage_min_strength=30
damage_max_strength=100
damage_pulse_ms=220
damage_cooldown_ms=90
offense_damage_enabled=1
offense_damage_window_ms=600
offense_damage_min_strength=70
offense_damage_max_strength=100
offense_damage_pulse_ms=520
offense_damage_cooldown_ms=120
contact_enabled=1
contact_min_delta_per_mille=8000
contact_full_delta_per_mille=220000
contact_min_strength=8
contact_max_strength=45
contact_pulse_ms=80
contact_cooldown_ms=220
contact_car_min_strength=65
contact_car_max_strength=100
contact_car_pulse_ms=420
contact_car_cooldown_ms=120
rubble_contact_enabled=1
scrape_enabled=1
scrape_min_delta_per_mille=5000
scrape_full_delta_per_mille=60000
scrape_min_strength=35
scrape_max_strength=100
scrape_pulse_ms=260
scrape_cooldown_ms=50
scrape_grain_ms=32
scrape_low_percent=20
scrape_min_side_per_mille=450
scrape_steering_threshold=7000
scrape_directional_bias=75
scrape_directional_min_motor=30
landing_enabled=1
landing_airborne_gap_ms=300
landing_min_delta_per_mille=15000
landing_full_delta_per_mille=180000
landing_min_strength=25
landing_max_strength=85
landing_pulse_ms=160
landing_cooldown_ms=600
collision_sound_enabled=0
collision_sound_min_volume=80
collision_sound_full_volume=650
collision_sound_min_strength=18
collision_sound_max_strength=70
collision_sound_pulse_ms=120
collision_sound_cooldown_ms=140
```

`buttons` accepts comma-separated XInput names such as `A`, `B`, `X`, `Y`, `LEFT_SHOULDER`, `RIGHT_SHOULDER`, `LB`, `RB`, `START`, `BACK`, `DPAD_UP`, `DPAD_DOWN`, `DPAD_LEFT`, and `DPAD_RIGHT`.

Synthetic button rumble is disabled by default because it is only a fallback test path.

Damage rumble scales from `damage_min_strength` to `damage_max_strength` as the per-hit damage delta approaches `damage_full_delta_per_mille`.

Crash envelopes are used for strong damage and car-to-car contact events. They play a sharp kick, a short gap, then a sustained rumble so big hits feel more like an impact instead of a flat buzz.

Offense damage rumble bridges the rear-end/opponent-damage case. If an opponent takes damage within `offense_damage_window_ms` of a matching recent player contact, the mod treats it as player-caused and emits a pulse even if the player's own car did not take damage. The mod remembers a small ring of recent player contacts so tiny follow-up scrapes do not immediately erase the car contact.

Rumble pulses are merged while active: a weaker event will not reduce an ongoing stronger crash pulse.

Directional rumble biases contact/offense pulses toward the estimated hit side of the player car. If left/right feels reversed, set `directional_invert=1`. `directional_bias` controls how much the far-side motor is reduced, while `directional_min_motor` keeps the quieter side from going fully silent.

Contact rumble comes from the local player's raw contact loop. Car-to-car contacts use a stronger tuning path than props/rubble so opponent hits still feel meaningful when the player's own damage does not increase. Set `rubble_contact_enabled=0` to disable the regular prop/rubble collision thumps while keeping car impacts and wall scrape separate.

Scrape rumble is a separate side-contact layer for sustained wall/prop rubbing. It fires for non-car contacts that are far enough toward the left or right side of the player car. If the contact point is ambiguous, it uses left-stick steering above `scrape_steering_threshold` as the scrape side. `scrape_grain_ms` and `scrape_low_percent` create a fast high/low texture so wall rubs feel scratchy instead of smooth.

Landing rumble uses surface/world contacts from the same player-car contact loop. It only fires after a short gap without ground contact, which keeps normal driving from buzzing constantly while still catching rough landings.

`collision_sound_enabled` is intentionally off. It can detect more impacts, but testing showed it reacts to nearby/world collision sounds rather than strictly player-car collisions.

## Build

Run from a Visual Studio Developer PowerShell or a normal PowerShell with Visual Studio Build Tools installed:

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

The build emits `dinput8.dll`.

## Notes

This is still reverse-engineering work. The reliable player identifier came from live testing/logs, not official game symbols. The current hook addresses are for the tested FlatOut 2 executable build.

Known follow-up: powerful rear-end hits against opponents can still fail to rumble. Those cases may use a different collision/damage path or expose a different contact object type than the current player-car contact hook.
