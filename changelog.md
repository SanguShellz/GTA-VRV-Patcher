**v1.2.5  Early Weapon Snapshot**

-   Fixed: weapon snapshot (`SaveCurrentWeapons`) is now taken when `GET_VEHICLE_PED_IS_TRYING_TO_ENTER` becomes true — before the engine's "get in vehicle" sequence assigns weapons to the ped — rather than at model-reset time. Previously, calling `SaveCurrentWeapons` after that sequence had already run meant the engine-assigned weapons were included in the restore, so a player who entered a vehicle unarmed would exit it armed.

-   Removed the `SaveCurrentWeapons` call from inside `InstantPlayerModelReset`: the snapshot is now always taken at pre-entry time (see above), making the late in-reset call redundant and removing the window where the snapshot could capture engine-assigned weapons.

-   Added debug log line for the selected weapon hash in `SaveCurrentWeapons` to make diagnosing weapon-restore failures easier (`Output of the selected weapon is : 0x...`).

**v1.2.4  Cutscene-End Heading/Camera Fix**

-   Added heading snap and HMD recenter at cutscene end, fixing tracking drift after cutscenes (mirrors the existing vehicle-exit fix)

-   Added 3P→1P camera reinit, soft reset, script cam reset, and full FP rearm/guard/unclamp pipeline at cutscene end

-   Added airborne-wait gate so the fix holds off if the player is still falling/ragdolled/parachuting when the cutscene ends (e.g. "monkeys and aliens"), with a configurable timeout (`CutsceneGroundWaitMaxFrames=180`) to prevent stalling on misread flags

-   Added `CutsceneModelReset` (default `0`)  opt-in `SET_PLAYER_MODEL` fallback at cutscene end, mirroring `VehicleExitModelReset`; includes a live model/appearance snapshot before the reset to avoid restoring a stale outfit

-   Extended heading fix to cover extended control-loss periods (not just `IS_CUTSCENE_PLAYING`), via existing `ControlLossHeadingFix`