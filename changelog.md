**v1.2.4 — Cutscene-End Heading/Camera Fix**

-   Added heading snap and HMD recenter at cutscene end, fixing tracking drift after cutscenes (mirrors the existing vehicle-exit fix)

-   Added 3P?1P camera reinit, soft reset, script cam reset, and full FP rearm/guard/unclamp pipeline at cutscene end

-   Added airborne-wait gate so the fix holds off if the player is still falling/ragdolled/parachuting when the cutscene ends (e.g. "monkeys and aliens"), with a configurable timeout (`CutsceneGroundWaitMaxFrames=180`) to prevent stalling on misread flags

-   Added `CutsceneModelReset` (default `0`) — opt-in `SET_PLAYER_MODEL` fallback at cutscene end, mirroring `VehicleExitModelReset`; includes a live model/appearance snapshot before the reset to avoid restoring a stale outfit

-   Extended heading fix to cover extended control-loss periods (not just `IS_CUTSCENE_PLAYING`), via existing `ControlLossHeadingFix`