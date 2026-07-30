## Problem Solved

After exiting a vehicle in first-person mode, HMD head tracking breaks - the camera no longer rotates with head movement and only responds to controller input. Hospital respawn restores tracking automatically, but this workaround is not practical during gameplay.

## Solution

**Default (recommended): non-destructive camera re-init plus direct state pokes.** On vehicle exit the mod cycles GTA's native first/third-person ped cameras (`FirstPersonSoftReset`) and restores RealVR's internal HMD-tracking flag (`g_RVRData+0x840`) to fix HMD rotation tracking. None of this touches the player's ped, model, weapons, or position. An older scripted-camera variant (`ScriptCamReset`) is available as a fallback but is off by default - see "RealVR's internal state" below for why it was replaced.

A separate `HeadingControl` (character turns to face HMD direction) issue after vehicle exit was investigated but is **not currently fixed** by this shim - a direct-write approach was tried and reverted because it didn't change RealVR's actual behavior. See "RealVR's internal state" below for what was tried and why it's still open.

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
  - **`g_RVRData + 0x821`** (byte, write `1` to trigger): requests an HMD recenter, identical to pressing `NUMPAD /`. `g_RVRData` is the same pointer this shim already resolves via `RealVR+0x38020` for the HMD-tracking flag. **This is the mechanism actually in use** (`TriggerRVRRecenter`), used for the cutscene-end and optional vehicle-exit recenter fixes below.
  - **`RealVR.asi_base + 0x384F8`** (int32): the live `HeadingControl` value (0=always, 1=only aiming, 2=never - matches `RealVR.ini`'s documented range), located during the same disassembly pass. A vehicle-exit fix that directly wrote this address was tried and didn't actually resolve the issue - only the raw readback value changed, not RealVR's behavior - so it was **removed**. The address is left documented here as a starting point for revisiting the problem later; nothing in the current shim reads or writes it.
- `T` (dominant eye) and other hotkeys were decoded the same way and matched their documented behavior exactly, which is what gives confidence the `0x821`/`0x384F8` addresses above were read correctly out of the mod's own hotkey-handling code rather than guessed.

This shim calls directly into `g_RVRData+0x821` for the recenter fix - the same state the `NUMPAD /` hotkey itself flips - rather than trying to coax the same result out of GTA natives or camera tricks.

### Vehicle Exit Handling (default path)
1. **Detect Exit**: When the player exits a vehicle while in first-person, arm `FirstPersonSoftReset`.
2. **Re-init Camera**: Cycle GTA's native ped camera between third-person and first-person (`SET_FOLLOW_PED_CAM_VIEW_MODE`) to refresh HMD rotation tracking.
3. **Restore Tracking Flag**: Directly write `1` back to RealVR's internal HMD-tracking flag (`g_RVRData+0x840`) in case RealVR cleared it on vehicle entry.
4. **Optional recenter** (`VehicleExitRecenterFix`, off by default): also fire RealVR's own HMD recenter (`g_RVRData+0x821`, the same mechanism used for cutscene-end below), if HMD rotation tracking still isn't fully right after the above.

No ped, model, or weapon changes are involved in this path. `ScriptCamReset` (the scripted-camera variant used in an earlier version of this fix) is off by default; re-enable it only if `FirstPersonSoftReset` alone doesn't restore HMD rotation tracking for you.

`HeadingControl` (character turns to face HMD direction) getting stuck after vehicle exit is a known, separate issue that this path does not address - see "RealVR's internal state" above.

### Vehicle Exit Handling (opt-in `VehicleExitModelReset=1` fallback)
1. **Detect Entry Attempt (v1.2.5+)**: When `GET_VEHICLE_PED_IS_TRYING_TO_ENTER` returns a non-zero handle, snapshot the current weapon loadout via `SaveCurrentWeapons`. This must fire before the engine's boarding sequence assigns weapons; doing it here ensures the snapshot reflects what the player actually carried before entering.
2. **Detect Exit**: When player exits a vehicle in first-person and has script control, call `InstantPlayerModelReset()`. If control is off (mission cutscene/teleport), defer until control returns or the timeout elapses.
3. **Change Model**: `SET_PLAYER_MODEL(player, savedModel)` - apply same model immediately.
4. **Restore Appearance + Weapons**: After falling animation ends (via `IS_PED_FALLING`), restore all saved:
   - Component variations (12 types: head, beard, hair, torso, legs, hands, feet, etc.)
   - Prop variations (8 types: hats, glasses, ears, watches, bracelets, etc.)
   - Weapons/ammo/equipped weapon snapshotted right before the model swap
4. **Preserve Camera**: GTA V automatically preserves HMD tracking after model change

### Appearance and Weapon Capture
- **Weapon snapshot (v1.2.5+)**: `SaveCurrentWeapons` fires when `GET_VEHICLE_PED_IS_TRYING_TO_ENTER` first returns a non-zero vehicle handle — i.e. the moment the player's "get in" animation begins, before GTA's engine assigns any weapons to the ped as part of the boarding sequence. Taking the snapshot here ensures that if the player was unarmed before boarding, the restored loadout after exiting is also empty. Prior to v1.2.5, the snapshot was taken inside `InstantPlayerModelReset` at model-swap time, after the engine had already assigned weapons, so unarmed players would exit vehicles armed.
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
- `GET_VEHICLE_PED_IS_TRYING_TO_ENTER` (0x814FA8BE5449445D) - Detect when the player has chosen to enter a vehicle, used to snapshot weapons before the engine's "get in vehicle" sequence assigns them
- `IS_PED_IN_AIR` (0x886E37EC497200B6) - Used by the cutscene-end ground-wait gate
- `IS_PED_RAGDOLL` (0x47E4E977581C5B55) - Used by the cutscene-end ground-wait gate
- `IS_PED_PARACHUTE_FREE_FALLING` (0x7DCE8BDA0F1C1200) - Used by the cutscene-end ground-wait gate
- `GET_ENTITY_HEIGHT_ABOVE_GROUND` (0x1DD55701034110E5) - Used by the cutscene-end ground-wait gate to determine grounded state
- `IS_CUTSCENE_PLAYING` (0xD3C2E180A40F031E) - Detect cutscene end for the heading fix
- `GET_GAMEPLAY_CAM_ROT` (0x837765A25378F0BB) - Read the camera's world heading to realign the ped to
- `GET_ENTITY_HEADING` / `SET_ENTITY_HEADING` (0xE83D4F9BA2A38914 / 0x8E2530AA8ADA980E) - Read/snap the ped's body heading

### RealVR's Own Internal State (used directly, not via natives)
Found by disassembling `RealVR.asi` itself - see "RealVR's internal state" above for how this was located.
- `g_RVRData + 0x821` (byte) - write `1` to request an HMD recenter, identical to the `NUMPAD /` hotkey. `g_RVRData` is resolved via `RealVR+0x38020`, the same pointer already used for the `+0x840` HMD-tracking flag. **Actively used** by this shim.
- `RealVR.asi_base + 0x384F8` (int32) - the live `HeadingControl` value (0/1/2), identical to what the `Y` hotkey cycles through. Located via disassembly but **not currently used**: an earlier attempt to write it directly on vehicle exit didn't actually change RealVR's behavior and was removed. Documented here for anyone revisiting the issue.

```cpp
static uint8_t* GetRVRDataPtr();                 // resolves g_RVRData via RealVR+0x38020
static bool TriggerRVRRecenter(const char*);     // g_RVRData+0x821 = 1
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

- `VehicleExitModelReset` (default `0`) - recreate the player's ped via `SET_PLAYER_MODEL` on vehicle exit, same as the original behavior.
- `ModelResetRequireControl` (default `1`) - only apply the reset once `IS_PLAYER_CONTROL_ON` is true, deferring (not skipping) while a cutscene/mission teleport has control locked. Turning this off restores the old racy behavior - not recommended.
- `ModelResetDeferMaxFrames` (default `300`, ~5s) - how long a deferred reset waits for control to return before giving up

- `ModelResetPreserveWeapons` (default `1`) - snapshot weapons/ammo/equipped weapon before the swap and re-give them after appearance is restored (or once death/arrest actually clears - see "Death/Arrest Handling" above).
- `ModelResetPreserveWantedLevel` (default `1`) - snapshot the player's wanted level (stars) before the swap and re-apply it at the same point weapons are re-given. Same root cause as the weapon wipe: the ped `SET_PLAYER_MODEL` recreates comes back with wanted level cleared.

### HeadingControl after vehicle exit (known open issue)

`HeadingControl` (character turns to face HMD direction) can end up stuck after vehicle exit. This is **not fixed** by the current shim. Two approaches were tried and abandoned during investigation:
1. A theory that `ScriptCamReset` disrupted RealVR's internal `CameraType`/`PlayerMode` tracking - tested and falsified: the problem persisted even with `ScriptCamReset` off and only the native on-foot camera cycle (`FirstPersonSoftReset`) in play.
2. Directly writing RealVR's live `HeadingControl` value (`RealVR+0x384F8`, found via disassembly) - implemented and tested, but only changed the raw readback value, not RealVR's actual behavior. Reverted.

Calling RealVR's own registered ScriptHookV keyboard callback with a synthesized `Y` keypress (rather than poking the value in memory) remains an unexplored option, but the callback's entry point hasn't been reliably located or verified yet - see "RealVR's internal state" above for what the disassembly has confirmed so far.

### Cutscene Heading Fix (opt-out)

- `CutsceneHeadingFix` (default `1`) - snap the ped's heading to match the camera once a cutscene (or extended control-loss period) ends. Set to `0` to disable entirely if it ever fights a specific mission's own camera handling.
- `CutsceneRecenterFix` (default `1`) - trigger RealVR's own HMD recenter (identical to the `NUMPAD /` hotkey) at the same time. This is the primary fix for HMD/character desync; the heading snap above complements it by also realigning the ped's movement-heading.
- `CutsceneSoftReset` (default `1`) - cycle the native first/third-person ped cameras on cutscene end.
- `CutsceneScriptCamReset` (default `0`) - older scripted-camera variant, available as a fallback if the above isn't enough.
- `CutsceneModelReset` (default `0`) - same opt-in `SET_PLAYER_MODEL` ped-recreation fallback as `VehicleExitModelReset`, applied at cutscene end instead of vehicle exit. Off by default for the same reasons - keep it off unless the non-destructive fixes above don't reliably restore tracking for a specific cutscene.
- `CutsceneGroundWaitMaxFrames` (default `180`, ~3s @60fps) - some cutscenes (e.g. one that ends with the player falling and having to navigate to the ground) hand control back while the player is still airborne. Applying the fix mid-fall fights the landing, so the fix waits for a grounded, non-falling, non-ragdolled, non-parachuting state first. This caps how long it waits before giving up and applying the fix anyway, so a misread air/ground flag can't stall it forever. `0` waits indefinitely.
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

**Exited a vehicle armed even though you were unarmed when you entered**:
- Fixed in v1.2.5. GTA's engine assigns weapons to the ped as part of its "get in vehicle" boarding sequence. Before v1.2.5, `SaveCurrentWeapons` was called inside `InstantPlayerModelReset` — after that sequence had already run — so engine-assigned weapons were included in the snapshot and re-given on exit. The fix moves the snapshot to the `GET_VEHICLE_PED_IS_TRYING_TO_ENTER` trigger, which fires before the engine's boarding assignment.
- If you see this on v1.2.5+, confirm the log shows `saved N weapon(s)` at the pre-entry trigger (`trying to enter` in the log context), not later.

**Weapons disappear after exiting a vehicle**:
- Caused by the `VehicleExitModelReset` fallback path: `SET_PLAYER_MODEL` resets the ped to its default (unarmed) loadout. `VehicleExitModelReset=0` is the default and avoids this entirely, since no model swap occurs on vehicle exit.
- If you enable `VehicleExitModelReset=1`, confirm `ModelResetPreserveWeapons=1` (default) so the loadout is snapshotted and re-given automatically.

**Weapons disappear after respawning from a hospital/police station**:
- This was a real bug (fixed): the death/arrest state cleanup ran *after* `InstantPlayerModelReset()` instead of before, which wiped the pending weapon-restore flag that call had just set - so weapons were snapshotted but never actually given back. Also, the restore used to wait on `IS_PED_FALLING`, which never clears for a ragdolled corpse; it now waits for the death/arrest flag itself to clear (i.e. the respawn has actually happened).
- Should no longer occur. If it does, check the log for `appearance/weapons/wanted-level restored for ped` lines around your respawn - if you don't see one, `ModelResetPreserveWeapons` may be disabled, or something is holding the death/arrest flag true past the actual respawn.

**Wanted level (stars) disappears after exiting a vehicle**:
- Same cause as the weapon-wipe bug above: the `VehicleExitModelReset` fallback path recreates the ped via `SET_PLAYER_MODEL`, and the fresh ped comes back with wanted level cleared, the same way it comes back unarmed.
- Fixed by `ModelResetPreserveWantedLevel=1` (default) - snapshots `GET_PLAYER_WANTED_LEVEL` before the swap and re-applies it with `SET_PLAYER_WANTED_LEVEL` + `SET_PLAYER_WANTED_LEVEL_NOW` at the same point weapons/appearance are restored. Check the log for `saved wanted level` / `restored wanted level` lines if stars still vanish.
- As with weapons, this only matters if `VehicleExitModelReset=1` is enabled - the default `FirstPersonSoftReset` path never touches the ped, so no wanted level is ever lost there in the first place.

**HeadingControl (character turns to face HMD direction) stops working after exiting a vehicle**:
- **Known unresolved issue - no fix is currently implemented.** See "HeadingControl after vehicle exit (known open issue)" above for what's been tried (a `ScriptCamReset`-interference theory, falsified; a direct memory-write fix, reverted because it didn't change RealVR's actual behavior) and what a working fix would likely require.
- Workaround: toggle `HeadingControl` manually with RealVR's own `Y` hotkey after exiting the vehicle.

**Character is facing away from (e.g. 180° opposite) the HMD view after a cutscene**:
- Fixed by `CutsceneHeadingFix=1` (default), which snaps the ped's heading to the camera's once a cutscene - or an extended control-loss period, for missions that don't use the engine cutscene system - ends. If you still see it: try raising `CutsceneEndSettleFrames` (a mission may be repositioning the ped slightly after the fix runs, racing it), and check the log for `cutscene-end heading fix` and `extended control-loss end detected` lines to see what was captured and when.
- If your case is a mission-scripted moment rather than a true cutscene, confirm `ControlLossHeadingFix=1` (default) and that `ControlLossThresholdFrames` (default 45) isn't longer than the control-loss window in that specific mission - lower it if the scripted moment is brief.
- If a specific mission's own cutscene handling conflicts with this (rare), you can disable it per-mission by setting `CutsceneHeadingFix=0` and re-enabling it after, or report the mission name so it can be special-cased.




