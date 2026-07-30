# RealVRCompat ASI

Compatibility shim for the original `RealVR.asi` on newer GTA V builds, tested against GTA V `1.0.3788.0` with ScriptHookV `v3788.0/1013.34`.

## Problem Solved

After exiting a vehicle in first-person mode, HMD head tracking breaks - the camera no longer rotates with head movement and only responds to controller input. Hospital respawn restores tracking automatically, but this workaround is not practical during gameplay.

## Solution

**Default (recommended): non-destructive camera re-init plus direct state pokes.** On vehicle exit the mod cycles GTA's native first/third-person ped cameras (`FirstPersonSoftReset`) and restores RealVR's internal HMD-tracking flag (`g_RVRData+0x840`) to fix HMD rotation tracking. None of this touches the player's ped, model, weapons, or position. An older scripted-camera variant (`ScriptCamReset`) is available as a fallback but is off by default - see "RealVR's internal state" below for why it was replaced.

A separate `HeadingControl` (character turns to face HMD direction) issue after vehicle exit is handled by a third approach: rather than writing RealVR's state directly, it calls RealVR's own registered keyboard-hotkey handler in-process with a synthesized keypress. See "RealVR's internal state" and "HeadingControl after vehicle exit" below.

**Optional fallback: Instant Player Model Reset.** The mod can still recreate the player's ped via `SET_PLAYER_MODEL` (preserving appearance, velocity, and falling animations) as a more forceful way to trigger RealVR's re-initialization. This is now **off by default** (`VehicleExitModelReset=0`) because it caused two game-breaking regressions:
- It unconditionally recreated the ped on every vehicle exit, even mid-mission-cutscene, which could race a mission script's own teleport/hand-off and drop the recreated ped through the map (e.g. after delivering a stolen car, or the motorcycle repo mission).
- `SET_PLAYER_MODEL` resets the ped to the new model's default loadout, and the mod never captured/restored weapons - so exiting a vehicle silently stripped your guns.

If you turn it back on, it is now guarded:
- Only fires if you were actually in first-person when you exited (nothing to fix otherwise).
- Waits for `IS_PLAYER_CONTROL_ON` before touching the ped (`ModelResetRequireControl=1`), deferring rather than firing mid-cutscene/mid-teleport, and giving up after `ModelResetDeferMaxFrames` if control never returns.
- Snapshots and restores your weapons/ammo/equipped weapon around the model swap (`ModelResetPreserveWeapons=1`).

Death and arrest still use the model reset unconditionally (control is already off in both cases by design, and there's no mission-teleport race), but weapon preservation now applies there too so you don't lose your loadout on respawn.

**Key Features**:
- Restores HMD tracking on vehicle exit via the same mechanism GTA uses for hospital respawn - no ped/model changes needed in the default configuration
- Preserves exact player appearance (clothes, accessories) - captured before entering vehicle, restored after any model reset
- Preserves weapons/ammo/equipped weapon around any model reset (death, arrest, or the opt-in vehicle-exit fallback)
- Maintains falling/falling animations - velocity is preserved during model change
- Works on death and arrest - applies same model reset to restore camera on respawn
- No visual freeze - model change (when used) is immediate and non-blocking
- Automatic appearance/weapon restoration - happens after falling animation completes

## How It Works

### RealVR's internal state (confirmed by disassembly)
An earlier version of this doc speculated that `ScriptCamReset`'s foreign scripted-camera type was confusing RealVR's internal `CameraType`/`PlayerMode` tracking and leaving `HeadingControl` stuck off. That hypothesis was tested and falsified - `HeadingControl` stayed broken even with `ScriptCamReset` off and only the native on-foot camera cycle (`FirstPersonSoftReset`) in play. Root-causing it properly meant going past `strings` and actually disassembling `RealVR.asi` (`objdump -d`):

- `RealVR.asi` imports only `ScriptHookV.dll` and `KERNEL32.dll` (no `USER32.dll`), so hotkeys aren't polled via `GetAsyncKeyState` - they go through ScriptHookV's `keyboardHandlerRegister` callback. That callback (found via the import table) turned out to do nothing but timestamp raw key events into a 255-entry table; the actual hotkey *actions* live in the main per-frame handler, which polls that table.
- Disassembling the checks against that table for `Y` (Toggle Heading Control) and `NUMPAD /` (Recenter HMD) - cross-referenced against the hotkeys documented in `hotkeys.txt` - located the exact live memory RealVR itself reads and writes:
  - **`g_RVRData + 0x821`** (byte, write `1` to trigger): requests an HMD recenter, identical to pressing `NUMPAD /`. `g_RVRData` is the same pointer this shim already resolves via `RealVR+0x38020` for the HMD-tracking flag. **This is the mechanism actually in use below.**
  - **`RealVR.asi_base + 0x384F8`** (int32): the live `HeadingControl` value (0=always, 1=only aiming, 2=never - matches `RealVR.ini`'s documented range). A vehicle-exit fix that directly toggled this address was tried and didn't actually resolve the issue - only the raw readback value changed, not RealVR's behavior - so this shim now only *reads* it, to decide how many synthesized `Y` presses are needed (see `RestoreHeadingControl` below).
  - **`RealVR.asi_base + 0x1370`**: RealVR's own ScriptHookV keyboard-callback function, the same address `keyboardHandlerRegister` was given at startup. Calling it directly with a synthesized key event runs RealVR's real hotkey logic in-process, without OS-level input simulation. This is what actually restores `HeadingControl` - see `RestoreHeadingControl` below.
- `T` (dominant eye) and other hotkeys were decoded the same way and matched their documented behavior exactly, which is what gives confidence the addresses above were read correctly out of the mod's own hotkey-handling code rather than guessed.

This shim calls directly into `g_RVRData+0x821` for the recenter fix - the same state the `NUMPAD /` hotkey itself flips - rather than trying to coax the same result out of GTA natives or camera tricks.

### Vehicle Exit Handling (default path)
1. **Detect Exit**: When the player exits a vehicle while in first-person, arm `FirstPersonSoftReset`.
2. **Re-init Camera**: Cycle GTA's native ped camera between third-person and first-person (`SET_FOLLOW_PED_CAM_VIEW_MODE`) to refresh HMD rotation tracking.
3. **Restore Tracking Flag**: Directly write `1` back to RealVR's internal HMD-tracking flag (`g_RVRData+0x840`) in case RealVR cleared it on vehicle entry.
4. **Optional recenter** (`VehicleExitRecenterFix`, off by default): also fire RealVR's own HMD recenter (`g_RVRData+0x821`, the same mechanism used for cutscene-end below), if HMD rotation tracking still isn't fully right after the above.
5. **HeadingControl restore** (`RestoreHeadingControl`, default on): if `RealVR.ini` had `HeadingControl=0` (Always) at startup, check ~65 frames later whether it drifted, and if so, restore it by calling RealVR's own hotkey-handling code directly with a synthesized `Y` keypress - see "HeadingControl after vehicle exit" below.

No ped, model, or weapon changes are involved in this path. `ScriptCamReset` (the scripted-camera variant used in an earlier version of this fix) is off by default; re-enable it only if `FirstPersonSoftReset` alone doesn't restore HMD rotation tracking for you.

### Vehicle Exit Handling (opt-in `VehicleExitModelReset=1` fallback)
1. **Detect Exit**: When player exits a vehicle in first-person and has script control, call `InstantPlayerModelReset()`. If control is off (mission cutscene/teleport), defer until control returns or the timeout elapses.
2. **Change Model**: `SET_PLAYER_MODEL(player, savedModel)` - apply same model immediately.
3. **Restore Appearance + Weapons**: After falling animation ends (via `IS_PED_FALLING`), restore all saved:
   - Component variations (12 types: head, beard, hair, torso, legs, hands, feet, etc.)
   - Prop variations (8 types: hats, glasses, ears, watches, bracelets, etc.)
   - Weapons/ammo/equipped weapon snapshotted right before the model swap
4. **Preserve Camera**: GTA V automatically preserves HMD tracking after model change

### Appearance Capture
- When entering vehicle: capture exact model hash and all 12 component + 8 prop variations
- Uses `GET_PED_DRAWABLE_VARIATION`, `GET_PED_TEXTURE_VARIATION`, `GET_PED_PALETTE_VARIATION`
- Also captures prop indices and textures via `GET_PED_PROP_INDEX`, `GET_PED_PROP_TEXTURE_INDEX`

### Death/Arrest Handling
- On death or arrest: instantly apply model change to restore camera state (control is already off by design here, so there's no mission-teleport race to guard against)
- Snapshot weapons/ammo before the swap and restore them **once the death/arrest flag actually clears** (i.e. the real hospital/station respawn has completed) - not on `IS_PED_FALLING`, which never reads true for a ragdolled corpse and would restore mid-death-sequence, right before GTA's own wasted->hospital flow discards it
- Clean up all cached state to prevent stale data from interfering after respawn - this cleanup now runs **before** `InstantPlayerModelReset()`, not after, since running it after was clearing the very restore flag the reset call had just set (weapons were snapshotted but the restore was silently cancelled every time)
- Prevent camera breaking when respawning in hospital (critical for helicopters/airplanes)

### Cutscene-End Handling
Symptom: after some cutscenes, the character's body/movement heading is turned away from - sometimes exactly 180° from - the direction the HMD is actually looking.

Two independent triggers arm the same fix, since many in-mission "cutscenes" are just the mission script disabling player control and never register as a true engine cutscene:
1. **True cutscene**: `IS_CUTSCENE_PLAYING` transitioning true -> false.
2. **Extended control loss**: `IS_PLAYER_CONTROL_ON` false for `ControlLossThresholdFrames` (default 45, ~0.75s) or more, then returning true - covers scripted mission moments that don't use the engine cutscene system. Skipped while actually dead/arrested/mid-cutscene so it doesn't double-fire or misfire during those.

Once armed, after a `CutsceneEndSettleFrames` settle window:
1. **RVR recenter** (`CutsceneRecenterFix`, default on): trigger RealVR's own HMD recenter (`g_RVRData+0x821`), identical to pressing `NUMPAD /`. This recalibrates the HMD-to-game-forward offset directly through RealVR's own mechanism, which is a more reliable resync than approximating it from outside.
2. **Realign heading**: read the gameplay camera's world heading (`GET_GAMEPLAY_CAM_ROT`) and `SET_ENTITY_HEADING` the ped to match, then reset `SET_GAMEPLAY_CAM_RELATIVE_HEADING(0)` and release the yaw/pitch clamps - complements the recenter by also realigning the ped's movement-heading, which recentering alone doesn't touch.
3. **Soft camera reset** (`CutsceneSoftReset`, default on): cycle the native first/third-person ped cameras.
4. **Scripted-cam reset** (`CutsceneScriptCamReset`, off by default): the older scripted-camera variant, available as a fallback.

This is a heading/state fix only - no ped/model recreation, so it doesn't affect weapons.

## Installation

Place these files in the GTA V game folder:

- `00_RealVRCompat.asi` - The compatibility shim (load before RealVR.asi)
- `RealVR.asi` - Original VR mod
- RealVR proxy `d3d11.dll`
- `RealVR.ini` - Configuration
- ScriptHookV/ASI loader files

The `00_` prefix is intentional so the compat shim loads before `RealVR.asi`.

## Building

**Prerequisites**:
- Visual Studio 2022
- ScriptHookV SDK (v3788.0 or compatible)
- Windows SDK

**Build Steps**:
```bash
cd "path\to\GTAVRVSC"
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\msbuild.exe" RealVRCompat.vcxproj /p:Configuration=Release /p:Platform=x64
```

Output: `bin\00_RealVRCompat.asi`

## Configuration

Edit `RealVRCompat.ini` to control behavior:

### Vehicle/Death/Arrest Response
- `FirstPersonSoftReset=1` - Default HMD re-init on vehicle exit: cycles GTA's native first/third-person ped cameras (no ped/model changes, no foreign camera type)
- `FirstPersonSoftResetFrames=8` - Frames spent in third-person before cycling back to first-person
- `ScriptCamReset=0` - Fallback scripted-camera HMD re-init, off by default (see "RealVR's internal state" above for why)
- `ScriptCamResetFrames=2` - Frames the scripted camera stays active before handing back to the gameplay camera, if `ScriptCamReset=1`
- `VehicleExitModelReset=0` - Opt-in fallback: recreate the ped via `SET_PLAYER_MODEL` on vehicle exit instead (see "Model Reset Fallback" below)
- `VehicleExitRecenterFix=0` - Also trigger RealVR's own HMD recenter on vehicle exit, off by default (see "Cutscene Heading Fix" below for the same mechanism used there by default)

### Camera Restoration
All the original patch controls still apply:
- `ForceFallback=0` - Use metadata fallback resolver
- `CamMetaOldOffset=1` - Use old offset for camera metadata
- `RestoreCamMetadata=1` - Restore camera metadata if RealVR clears it
- `RestoreRVRState=1` - Restore RealVR internal flags after transitions
- `ActiveEnginePatches=1` - Apply RealVR's 6 engine patches
- `EnginePatchDelaySec=70` - Delay before applying patches

## Technical Details

### Native Functions Used
- `SET_PLAYER_MODEL` (0x00A1CADD00108836) - Change player model
- `GET_ENTITY_MODEL` (0x9F47B058362C84B5) - Get current model
- `GET_PED_DRAWABLE_VARIATION` (0x67F3780DD425D4FC) - Get component variation
- `GET_PED_TEXTURE_VARIATION` (0x04A355E041E004E6) - Get texture variation
- `GET_PED_PALETTE_VARIATION` (0xE3DD5F2A84B42281) - Get palette variation
- `SET_PED_COMPONENT_VARIATION` (0x262B14F48D29DE80) - Set component variation
- `GET_PED_PROP_INDEX` (0x898CC20EA75BACD8) - Get prop index
- `GET_PED_PROP_TEXTURE_INDEX` (0xE131A28626F81AB2) - Get prop texture
- `SET_PED_PROP_INDEX` (0x93376B65A266EB5F) - Set prop
- `CLEAR_PED_PROP` (0x0943E5B8E078E76E) - Clear prop
- `IS_PED_FALLING` (0xFB92A102F1C4DFA3) - Check if falling
- `IS_PLAYER_BEING_ARRESTED` (0x388A47C51ABDAC8E) - Check arrest state
- `IS_PLAYER_CONTROL_ON` (0x49C32D60007AFA47) - Gate the model-reset fallback so it can't fire mid-cutscene/mid-teleport
- `HAS_PED_GOT_WEAPON` (0x8DECB02F88F428BC) - Detect which weapons to snapshot before a model swap
- `GET_AMMO_IN_PED_WEAPON` (0x015A522136D7F951) - Snapshot ammo per weapon
- `GET_SELECTED_PED_WEAPON` (0x0A6DB4965674D243) - Snapshot the currently-equipped weapon
- `GIVE_WEAPON_TO_PED` (0xBF0FD6E56C964FCB) - Re-give snapshotted weapons after a model swap
- `SET_CURRENT_PED_WEAPON` (0xADF692B254977C0C) - Re-equip the previously-selected weapon
- `IS_CUTSCENE_PLAYING` (0xD3C2E180A40F031E) - Detect cutscene end for the heading fix
- `GET_GAMEPLAY_CAM_ROT` (0x837765A25378F0BB) - Read the camera's world heading to realign the ped to
- `GET_ENTITY_HEADING` / `SET_ENTITY_HEADING` (0xE83D4F9BA2A38914 / 0x8E2530AA8ADA980E) - Read/snap the ped's body heading

### RealVR's Own Internal State (used directly, not via natives)
Found by disassembling `RealVR.asi` itself - see "RealVR's internal state" above for how this was located.
- `g_RVRData + 0x821` (byte) - write `1` to request an HMD recenter, identical to the `NUMPAD /` hotkey. `g_RVRData` is resolved via `RealVR+0x38020`, the same pointer already used for the `+0x840` HMD-tracking flag.
- `RealVR.asi_base + 0x1370` - RealVR's own ScriptHookV keyboard-callback function (the exact address `keyboardHandlerRegister` was given at startup). Called directly, in-process, with a synthesized key event to run RealVR's real hotkey logic for a given key without OS-level input simulation.
- `RealVR.asi_base + 0x384F8` (int32) - the live `HeadingControl` value (0/1/2), identical to what the `Y` hotkey cycles through. Read-only in this shim; the value is never written directly (an earlier attempt that did write it directly didn't actually change RealVR's behavior).

```cpp
static uint8_t* GetRVRDataPtr();                 // resolves g_RVRData via RealVR+0x38020
static bool TriggerRVRRecenter(const char*);     // g_RVRData+0x821 = 1
static void TriggerRealVRHotkey(DWORD, BYTE, bool); // calls RealVR+0x1370 with a synthesized key event
static bool ReadLiveHeadingControl(int&);        // reads RealVR+0x384F8 (read-only)
static void LoadRealVRIniHeadingControl();       // caches RealVR.ini's [Defaults] HeadingControl at startup
```

### Global State Management
```cpp
// Vehicle state
static uint32_t g_saved_player_model = 0;           // Model before entering vehicle
static ComponentData g_saved_components[12];         // Clothes/appearance
static PropData g_saved_props[8];                    // Accessories

// Appearance restoration timing
static bool g_restore_appearance_next_frame = false; // Defer restoration
static int g_appearance_restore_ped = 0;            // Target ped for restoration

// Delayed model change (for long ejection animations)
static bool g_pending_delayed_model_change = false;
static int g_pending_delayed_model_change_frames = 0;

// Weapon preservation around any InstantPlayerModelReset() call
static SavedWeapon g_saved_weapons[64];   // hash + ammo snapshot
static int g_saved_weapon_count = 0;
static uint32_t g_saved_selected_weapon = 0;

// Deferred vehicle-exit model reset (fallback path only)
static bool g_pendingVehicleExitModelReset = false;
static int g_pendingVehicleExitModelResetFrames = 0;
```

### State Cleanup on Death/Arrest
When `dead && !wasDead` or `arrested && !wasArrested`:
1. Reset all vehicle state variables
2. Clear appearance cache
3. Reset RealVR state snapshots (`g_savedRVRStateValid = false`)
4. Clear camera metadata pool references

This prevents stale state from interfering after respawn in hospital.

## Known Patch Points

| Purpose | RVA |
| --- | --- |
| Version global | `RealVR+0x35A50` |
| Version range immediate | `RealVR+0x1CA7` |
| Internal log callback slot | `RealVR+0x38018` |
| Proxy data slot `g_RVRData` | `RealVR+0x38020` |
| Camera metadata pool slot | `RealVR+0x38098` |
| CamMetadata fallback branch | `RealVR+0x28D0` |
| First-person null-pool guard | `RealVR+0x5477` |
| First-person mode flag | `RealVR+0x38040` |
| First-person active flag | `RealVR+0x38041` |
| Original patcher function for the 6 engine hooks | `RealVR+0x1B30` |

## Recommended Config

For stable first-person tracking and smooth vehicle transitions:

```ini
[patches]
FirstPersonJump=0
VehicleCamNop=0
CamPoolWriteNop=0
ForceFallback=0
CamPoolResolve=1
CamMetaOldOffset=1
RestoreCamMetadata=1
RestoreRVRState=1
ActiveEnginePatches=1
EnginePatchDelaySec=70

[script]
# ScriptCamReset uses a foreign scripted camera and is OFF by default.
# FirstPersonSoftReset (native on-foot camera cycle) is the default
# HMD-tracking fix instead and carries less risk of side effects. (A theory
# that ScriptCamReset was also responsible for a separate HeadingControl
# issue was tested and found false - see "RealVR's internal state" above.)
ScriptCamReset=0
ScriptCamResetFrames=2
FirstPersonSoftReset=1
FirstPersonSoftResetFrames=8

# Less-aggressive vehicle-exit fix (see "Solution" above). Leave the model
# reset OFF and rely on FirstPersonSoftReset unless you have a specific case
# that doesn't fix.
VehicleExitModelReset=0
ModelResetRequireControl=1
ModelResetDeferMaxFrames=300
ModelResetPreserveWeapons=1

# Also fire RealVR's own HMD recenter on vehicle exit (see "RealVR's internal
# state" above). Off by default.
VehicleExitRecenterFix=0

# Restore HeadingControl after vehicle exit by synthesizing RealVR's own "Y"
# hotkey event (see "HeadingControl after vehicle exit" below). Only acts if
# RealVR.ini had HeadingControl=0 (Always) at startup.
RestoreHeadingControl=1

# Cutscene-end heading realignment (see "Cutscene-End Handling" above)
CutsceneHeadingFix=1
CutsceneRecenterFix=1
CutsceneSoftReset=1
CutsceneScriptCamReset=0
CutsceneEndSettleFrames=2
ControlLossHeadingFix=1
ControlLossThresholdFrames=45
```

### Model Reset Fallback (opt-in)

If `FirstPersonSoftReset` doesn't reliably restore tracking for your setup, you can re-enable the old ped-recreation fix:

```ini
[patches]
VehicleExitModelReset=1
ModelResetRequireControl=1
ModelResetPreserveWeapons=1
```

- `VehicleExitModelReset` (default `0`) - recreate the player's ped via `SET_PLAYER_MODEL` on vehicle exit, same as the original behavior.
- `ModelResetRequireControl` (default `1`) - only apply the reset once `IS_PLAYER_CONTROL_ON` is true, deferring (not skipping) while a cutscene/mission teleport has control locked. Turning this off restores the old racy behavior - not recommended.
- `ModelResetDeferMaxFrames` (default `300`, ~5s) - how long a deferred reset waits for control to return before giving up.
- `ModelResetPreserveWeapons` (default `1`) - snapshot weapons/ammo/equipped weapon before the swap and re-give them after appearance is restored (or once death/arrest actually clears - see "Death/Arrest Handling" above).

### HeadingControl after vehicle exit (`RestoreHeadingControl`)

`HeadingControl` (character turns to face HMD direction) can end up stuck after vehicle exit. Two earlier approaches were tried and abandoned:
1. A theory that `ScriptCamReset` disrupted RealVR's internal `CameraType`/`PlayerMode` tracking - tested and falsified.
2. Directly writing RealVR's live `HeadingControl` value (`RealVR+0x384F8`, found via disassembly) - implemented and tested, but only changed the raw readback value, not RealVR's actual behavior.

The current approach calls RealVR's own registered ScriptHookV keyboard callback directly, in-process, with a synthesized "`Y` just pressed" event - the same function ScriptHookV itself invokes for a real keystroke, found at the same fixed offset (`RealVR+0x1370`) already used to register it. This isn't OS-level input simulation (no `SendInput`/`keybd_event`, no window focus dependency) - it's a direct call to RealVR's real hotkey-handling code, so any side effects a real press would have happen too, not just the value that ends up in memory.

Mechanism:
1. `RealVR.ini`'s own `[Defaults] HeadingControl` value is read once at startup and cached (`g_headingControlIniValue`) - this is RealVR's actual configured value, independent of anything this shim does.
2. On vehicle exit, if `RestoreHeadingControl=1` and the cached value is `0` (Always), a delayed check is armed for ~65 frames later (just past the existing 60-frame camera-fix window).
3. Once that delay elapses, the live `HeadingControl` value is read and compared to the cached one. If they differ, the number of forward `Y`-cycles needed to walk it back (`Always → OnlyWhenAiming → Never → Always`) is computed and queued.
4. Queued presses are fired one at a time, ~250ms apart (comfortably past RealVR's own ~100ms hotkey debounce), by calling the real handler directly.
5. If the player re-enters a vehicle mid-sequence, the pending presses are dropped rather than continuing to fire.

Only restores to whatever RealVR.ini actually specified - it never turns the feature on for someone who has it set to `OnlyWhenAiming` or `Never`, and only acts at all if the cached startup value was `Always`.

### Cutscene Heading Fix (opt-out)

- `CutsceneHeadingFix` (default `1`) - snap the ped's heading to match the camera once a cutscene (or extended control-loss period) ends. Set to `0` to disable entirely if it ever fights a specific mission's own camera handling.
- `CutsceneRecenterFix` (default `1`) - trigger RealVR's own HMD recenter (identical to the `NUMPAD /` hotkey) at the same time. This is the primary fix for HMD/character desync; the heading snap above complements it by also realigning the ped's movement-heading.
- `CutsceneSoftReset` (default `1`) - cycle the native first/third-person ped cameras on cutscene end.
- `CutsceneScriptCamReset` (default `0`) - older scripted-camera variant, available as a fallback if the above isn't enough.
- `CutsceneEndSettleFrames` (default `2`) - frames to wait after the cutscene/control-loss ends before applying the fix. Raise this if you still see a brief heading snap-back fighting a mission's own post-cutscene positioning.
- `ControlLossHeadingFix` (default `1`) - also treat an extended `IS_PLAYER_CONTROL_ON=false` period as a cutscene-end for missions that don't use the engine cutscene system. Set to `0` if this ever misfires during some non-cutscene control-loss moment (e.g. a minigame).
- `ControlLossThresholdFrames` (default `45`, ~0.75s) - how long control must be off before `ControlLossHeadingFix` treats it as a scripted sequence, to avoid reacting to brief control blips.

## Troubleshooting

**Camera still breaks after vehicle exit**:
- Ensure `FirstPersonSoftReset=1` in RealVRCompat.ini (the default HMD-tracking fix)
- If it's still not enough, try adding `ScriptCamReset=1` as well
- Check that player model is being captured (look for logs: "saved player model 0x...")
- Verify native hashes are correct for your game version

**Appearance is wrong after exiting vehicle**:
- The exact model and appearance are captured when entering the vehicle
- If you change appearance while in vehicle, those changes are lost on exit
- This is expected behavior - it restores what you looked like before entering

**Can't rotate head after respawning from death/arrest**:
- Model reset is applied automatically
- If still broken, check that `RestoreRVRState=1` is enabled

**Falling through the map after a mission that drops you off on foot** (e.g. stolen car delivery, motorcycle repo job):
- This was caused by `VehicleExitModelReset` recreating the ped at the same moment a mission script was teleporting/handing off the player. Make sure `VehicleExitModelReset=0` (the default) - the non-destructive `FirstPersonSoftReset` path doesn't touch the ped/position at all.
- If you do need `VehicleExitModelReset=1`, keep `ModelResetRequireControl=1` so the reset waits for the mission's cutscene/teleport to finish before it runs.

**Weapons disappear after exiting a vehicle**:
- Caused by the `VehicleExitModelReset` fallback path: `SET_PLAYER_MODEL` resets the ped to its default (unarmed) loadout. `VehicleExitModelReset=0` is the default and avoids this entirely, since no model swap occurs on vehicle exit.
- If you enable `VehicleExitModelReset=1`, confirm `ModelResetPreserveWeapons=1` (default) so the loadout is snapshotted and re-given automatically.

**Weapons disappear after respawning from a hospital/police station**:
- This was a real bug (fixed): the death/arrest state cleanup ran *after* `InstantPlayerModelReset()` instead of before, which wiped the pending weapon-restore flag that call had just set - so weapons were snapshotted but never actually given back. Also, the restore used to wait on `IS_PED_FALLING`, which never clears for a ragdolled corpse; it now waits for the death/arrest flag itself to clear (i.e. the respawn has actually happened).
- Should no longer occur. If it does, check the log for `appearance/weapons restored for ped` lines around your respawn - if you don't see one, `ModelResetPreserveWeapons` may be disabled, or something is holding the death/arrest flag true past the actual respawn.

**HeadingControl (character turns to face HMD direction) stops working after exiting a vehicle**:
- Fixed via `RestoreHeadingControl=1` (default) - see "HeadingControl after vehicle exit" above. Two earlier approaches were tried and ruled out first: a theory that `ScriptCamReset` disrupted RealVR's internal state tracking (falsified), and writing RealVR's live `HeadingControl` value directly (implemented, tested, didn't change RealVR's actual behavior). The current fix instead calls RealVR's own hotkey-handling code directly, synthesizing the same `Y` keypress event RealVR itself processes.
- Only takes effect if `RealVR.ini`'s `[Defaults] HeadingControl` was `0` (Always) at startup - it won't do anything if you have it set to `OnlyWhenAiming` or `Never`. Check the log for `heading control restore armed` / `heading control drifted` / `heading control restore complete` lines to see it firing.
- If it's not firing at all, confirm `RealVR.ini` is in the same folder as the game exe and has a readable `[Defaults]` section - the log line `cached RealVR.ini [Defaults] HeadingControl=...` at startup shows what was found (`-1` means it couldn't read it, and the whole feature stays inert).

**Character is facing away from (e.g. 180° opposite) the HMD view after a cutscene**:
- Fixed by `CutsceneHeadingFix=1` (default), which snaps the ped's heading to the camera's once a cutscene - or an extended control-loss period, for missions that don't use the engine cutscene system - ends. If you still see it: try raising `CutsceneEndSettleFrames` (a mission may be repositioning the ped slightly after the fix runs, racing it), and check the log for `cutscene-end heading fix` and `extended control-loss end detected` lines to see what was captured and when.
- If your case is a mission-scripted moment rather than a true cutscene, confirm `ControlLossHeadingFix=1` (default) and that `ControlLossThresholdFrames` (default 45) isn't longer than the control-loss window in that specific mission - lower it if the scripted moment is brief.
- If a specific mission's own cutscene handling conflicts with this (rare), you can disable it per-mission by setting `CutsceneHeadingFix=0` and re-enabling it after, or report the mission name so it can be special-cased.

## Project Cleanup

This project has been cleaned to include only RealVRCompat-related files:
- Removed GTAVRReal project and other unused components
- Removed reference/analysis files
- Updated .gitignore to prevent re-adding removed files
- Kept only essential files: source, headers, SDK, and build configuration

## License

See LICENSE.txt

## Compatibility

-  GTA V build 1.0.3788.0
-  Idk if the same code works in Enhanced
