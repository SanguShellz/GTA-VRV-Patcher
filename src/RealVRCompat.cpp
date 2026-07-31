#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "version.lib")

static volatile LONG g_running = 1;
static char g_logPath[MAX_PATH] = {};
static HMODULE g_realvr = nullptr;
static HMODULE g_self = nullptr;
static HMODULE g_rvrProxy = nullptr;
static volatile LONG g_exceptionLogged = 0;
static volatile LONG g_scriptRegistered = 0;
// Address of the GTA5 global slot that holds the camera pool manager pointer.
// Resolved at startup from a RIP-relative instruction near CamPoolHint.
// 0 until found; non-zero once we locate the slot (even when slot value is null).
static uintptr_t g_camPoolSlotAddr = 0;
static uintptr_t g_camPoolSlotAddrs[8] = {};
static int g_camPoolSlotCount = 0;
static uintptr_t g_savedCamMetadataPool = 0;
static uintptr_t g_savedRVRData = 0;
static bool g_savedRVRStateValid = false;
static uint8_t g_savedRVR_10 = 0;
static uint8_t g_savedRVR_11 = 0;
static uint8_t g_savedRVR_79 = 0;
static uint8_t g_savedRVR_E0 = 0;
static uint8_t g_savedRVR_840 = 0;
static uint32_t g_savedRVR_34 = 0;
static uint32_t g_savedRVR_3C = 0;
static uintptr_t g_lastRVRMgr = 0;
static uintptr_t g_lastRVRC0 = 0;
static uintptr_t g_lastRVRTarget = 0;
static uintptr_t g_lastRVRScene = 0;

// Saved engine hook continuations for vehicle exit fix
// Captured when in first-person at game start, restored when exiting vehicle
static uint64_t g_saved_Proj = 0;
static uint64_t g_saved_FOV1stCar = 0;
static uint64_t g_saved_CamParams = 0;
static uint64_t g_saved_FOVUni = 0;
static uint64_t g_saved_FOV3rd = 0;
static uint64_t g_saved_ViewInverse = 0;
static bool g_continuations_captured = false;

// Saved player model before entering vehicle
// Captured when entering vehicle, used when exiting to restore exact model
static uint32_t g_saved_player_model = 0;


// Structure to save ped appearance (clothes, accessories)
struct ComponentData {
    int drawable;
    int texture;
    int palette;
};

struct PropData {
    int drawable;
    int texture;
};

// Global arrays to store appearance (clothes, accessories)
static ComponentData g_saved_components[12];
static PropData g_saved_props[8];

// Flag to restore appearance on next frame (allows animations to play first)
static bool g_restore_appearance_next_frame = false;
static int g_appearance_restore_ped = 0;
// How to decide the pending restore is safe to apply:
//   0 = RESTORE_ON_LANDING     - wait for IS_PED_FALLING to clear (vehicle exit:
//                                 character is genuinely airborne/falling)
//   1 = RESTORE_ON_DEATH_CLEAR - wait for the death flag to clear (a ragdolled
//                                 corpse never reads as "falling", and applying
//                                 the restore mid-death-sequence just gets
//                                 discarded by GTA's own wasted->hospital flow)
//   2 = RESTORE_ON_ARREST_CLEAR - same idea, for the busted/arrest flow
static int g_restore_trigger_mode = 0;
static const int RESTORE_ON_LANDING = 0;
static const int RESTORE_ON_DEATH_CLEAR = 1;
static const int RESTORE_ON_ARREST_CLEAR = 2;

// Flag to trigger delayed model change (for autos with longer ejection animation)
static bool g_pending_delayed_model_change = false;
static int g_pending_delayed_model_change_frames = 0;
static bool g_pending_delayed_is_motorcycle = false;

// --- Weapon preservation around InstantPlayerModelReset() ---
// SET_PLAYER_MODEL recreates the ped and resets it to the new model's default
// loadout, which in practice means the player's weapons/ammo are wiped. The
// original vehicle-exit fix never accounted for this. We snapshot the loadout
// immediately before the model swap and re-give it back at the same point we
// already restore appearance (once the falling animation finishes).
// Attachments (suppressor, scope, grip, extended clip, flashlight, etc) live
// separately from the weapon itself - HAS_PED_GOT_WEAPON/GIVE_WEAPON_TO_PED
// only deal with the base weapon, so a snapshot that only stores hash+ammo
// silently drops every attachment. GIVE_WEAPON_TO_PED does NOT re-attach
// components already on the ped for that weapon, so components must be
// snapshotted and re-given explicitly, same as the base weapon.
static const int kMaxComponentsPerWeapon = 8;
struct SavedWeapon {
    uint32_t hash;
    int ammo;
    uint32_t components[kMaxComponentsPerWeapon];
    int componentCount;
};
static const int kMaxSavedWeapons = 64;
static SavedWeapon g_saved_weapons[kMaxSavedWeapons];
static int g_saved_weapon_count = 0;
static uint32_t g_saved_selected_weapon = 0;
static bool g_weapons_saved_valid = false;

// --- Wanted-level preservation around InstantPlayerModelReset() ---
// Same root cause as the weapon wipe above: SET_PLAYER_MODEL recreates the
// ped, and the fresh CPed the game hands back starts with no wanted level,
// which reads to the player as their stars vanishing on vehicle exit (or
// after the death/arrest model reset). Snapshot it immediately before the
// swap and re-apply it at the same point weapons/appearance get restored.
static int g_saved_wanted_level = 0;
static bool g_wanted_level_saved_valid = false;

// --- Deferred / gated vehicle-exit model reset ---
// The original code called InstantPlayerModelReset() unconditionally on every
// vehicle exit. That races with mission scripts that move/teleport the player
// on foot right after a vehicle drop-off (e.g. "deliver car" / "deliver bike"
// missions), which is what causes the recreated ped to fall through the map.
// We now require the player to actually have script control before we ever
// touch the model, and we default the whole model-swap path to OFF in favor
// of the non-destructive scriptCam/soft-reset fixes below.
static bool g_pendingVehicleExitModelReset = false;
static int g_pendingVehicleExitModelResetFrames = 0;

// Saved velocity to apply to new ped after model change
static float g_saved_velocity_x = 0.0f;
static float g_saved_velocity_y = 0.0f;
static float g_saved_velocity_z = 0.0f;
static bool g_restore_velocity_next_frame = false;

// Runtime patch toggles, read from RealVRCompat.ini [patches] at startup.
// 1 = enabled (default / current behavior), 0 = disabled (to bisect the vehicle-exit bug).
static int g_patchFirstPersonJump = 0;   // 0x5477 -> 0x5687 JMP (diagnostic static skip)
static int g_patchVehicleCamNop   = 0;   // 0x55AD 9-byte NOP   (vehicle cam collection loop)
static int g_patchCamPoolWriteNop = 0;   // 0x28F1 7-byte NOP   (freeze RealVR cam-pool write)
static int g_patchForceFallback   = 0;   // 0x28D0 EB 1A        (force cam metadata fallback)
static int g_patchCamPoolResolve  = 1;   // write found camera object to +0x38098
static int g_patchCamMetaOldOffset = 1;  // 0x28B4 0x3F->0x7F (force -0x28 resolver offset)
static int g_patchRestoreCamMetadata = 1;// restore last non-null +0x38098 if RealVR clears it
static int g_patchRestoreRVRState = 1;   // restore small g_RVRData flags after vehicle transitions
static int g_patchActiveEngine    = 1;   // call RealVR's own 6 engine trampoline patches
static int g_enginePatchDelaySec  = 70;  // defer the 6 engine patches until GTA is loading/entering map
static int g_firstPersonRearmFrames = 0;// old pulse path; disabled by default
static int g_firstPersonGuardFrames = 600;// watchdog after vehicle exit to catch delayed camera reset
static int g_firstPersonGuardPulseFrames = 0;
static int g_firstPersonGuardInterval = 90;
static int g_firstPersonFlagHold = 0;    // optional diagnostic hold; can cause temporary mono/screen mode
static int g_firstPersonControlFixFrames = 0; // keep GTA look controls enabled after vehicle exit
static int g_firstPersonUnclampFrames = 0; // release yaw/pitch clamps after vehicle/ragdoll transitions
static int g_firstPersonResetRearmFrames = 0; // short adaptive rearm when RealVR drops FP state
static int g_firstPersonSoftReset = 1; // one-shot FP camera reset after vehicle exit
static int g_firstPersonSoftResetFrames = 8;
static int g_keepVehicleFirstPerson = 1; // sync independent vehicle camera to FP on vehicle entry
static int g_respawnGraceFrames = 300; // let GTA/RealVR rebuild camera after death before forcing FP
// Script-camera re-init: mirrors what GTA does during hospital respawn.
// Creates a scripted cam at the exact gameplay cam position/rotation for
// ScriptCamResetFrames frames, then destroys it. When GTA deactivates the
// scripted cam and re-activates the gameplay cam, RealVR re-runs its
// first-person camera init, restoring HMD head tracking after vehicle exit.
//
// OFF by default: FirstPersonSoftReset below (cycling GTA's native
// first/third-person ped cameras) does the same job with less risk of
// interfering with anything else RealVR is tracking, since it never
// presents a foreign camera type. An earlier theory that this was also
// responsible for RealVR's "HeadingControl" feature getting stuck off after
// vehicle exit was tested and falsified - disabling this alone didn't fix
// HeadingControl. That issue is unrelated to ScriptCamReset and is still
// open; see the RealVR-internal-state comment near GetRVRDataPtr/
// TriggerRVRRecenter for the live memory address if you want to pick it
// back up. Re-enable ScriptCamReset only if FirstPersonSoftReset alone
// doesn't restore HMD rotation tracking for you.
static int g_scriptCamReset = 0;
static int g_scriptCamResetFrames = 2;

// Less-aggressive vehicle-exit fix controls (see README "Model Reset Fallback").
// VehicleExitModelReset: 0 = never recreate the player's ped on vehicle exit
// (rely solely on ScriptCamReset/FirstPersonSoftReset above, which fix the
// actual RealVR tracking flag without touching the ped). 1 = keep the old,
// more forceful SET_PLAYER_MODEL-based fix available as an opt-in fallback.
static int g_vehicleExitModelReset = 0;
// Require PLAYER::IS_PLAYER_CONTROL_ON before InstantPlayerModelReset() is
// allowed to run for a vehicle exit. When control is off (mission cutscene,
// scripted hand-off/teleport such as a delivery mission), the reset is
// deferred rather than firing immediately, so it can no longer race a
// mission's own teleport and drop the recreated ped through the world.
static int g_modelResetRequireControl = 1;
// Maximum frames to wait for control to return before giving up on a
// deferred vehicle-exit model reset (default ~5s at 60fps).
static int g_modelResetDeferMaxFrames = 300;
// Snapshot/restore the ped's weapons + ammo around any InstantPlayerModelReset
// call (death, arrest, and the optional vehicle-exit fallback). Without this,
// SET_PLAYER_MODEL silently strips the player's loadout.
static int g_modelResetPreserveWeapons = 1;
// Same idea for the player's wanted level (police stars) - SET_PLAYER_MODEL's
// fresh ped comes back with wanted level cleared. Without this, stars vanish
// on vehicle exit whenever VehicleExitModelReset=1 is active.
static int g_modelResetPreserveWantedLevel = 1;

// Cutscene-end heading realignment. Some cutscenes hand control back with the
// ped's body heading pointing a different way than the camera direction the
// player ends up looking from (RealVR feeds HMD yaw into the gameplay cam
// each frame, so the gameplay cam heading reflects where the player is
// actually looking) - this shows up as the character being turned away from,
// sometimes exactly opposite, the HMD view. On cutscene-end we snap the ped's
// heading to match the camera and reset the relative-heading offset to 0.
static int g_cutsceneHeadingFix = 1;
// Also cycle GTA's native first/third-person ped cameras (same mechanism as
// FirstPersonSoftReset) on cutscene end. Prefer this over CutsceneScriptCamReset
// below for the same least-invasive reasoning as ScriptCamReset above.
static int g_cutsceneSoftReset = 1;
// Legacy/fallback scripted-camera re-init for cutscene end. OFF by default -
// see the ScriptCamReset comment above.
static int g_cutsceneScriptCamReset = 0;
// Frames to wait after IS_CUTSCENE_PLAYING goes false before applying the fix,
// giving GTA's own handoff (repositioning, animation blend-out) a moment to
// settle first so we don't fight it mid-transition.
static int g_cutsceneEndSettleFrames = 2;
// Optional model-reset fallback for cutscene-end, mirroring VehicleExitModelReset
// below. OFF by default: SET_PLAYER_MODEL-based ped recreation is a forceful
// fix with known side effects (see VehicleExitModelReset/Technical.md), so it
// stays opt-in here too rather than firing unconditionally on every cutscene.
static int g_cutsceneModelReset = 0;
// Some cutscenes (e.g. one that ends with the player falling) hand control
// back while still airborne; applying the heading/camera fix mid-fall fights
// the landing. We wait for a grounded state first, but cap the wait so a
// misread air/ground/ragdoll flag can't stall the fix forever - after this
// many frames we apply it anyway. 0 disables the cap (wait indefinitely).
static int g_cutsceneGroundWaitMaxFrames = 180; // ~3s @60fps
// Many in-mission "cutscenes" (dialogue hand-offs, scripted walk-and-talks)
// never register with IS_CUTSCENE_PLAYING at all - they're just the mission
// script disabling player control for a while. This treats an extended
// control-off period the same as a cutscene for the heading fix, so those
// get covered too. 0 disables this fallback detection.
static int g_controlLossHeadingFix = 1;
static int g_controlLossThresholdFrames = 45; // ~0.75s @60fps before we call it a "scripted sequence"

// Trigger RealVR's own HMD recenter (TriggerRVRRecenter) on cutscene end /
// extended control-loss end, in addition to the SET_ENTITY_HEADING-based
// fix above. This recalibrates the HMD-to-game-forward offset directly
// through RealVR's own mechanism rather than approximating it.
static int g_cutsceneRecenterFix = 1;
// Also trigger RealVR's own HMD recenter on vehicle exit. Off by default;
// enable if HMD/character sync issues show up there too.
static int g_vehicleExitRecenterFix = 0;
static volatile LONG g_enginePatchesApplied = 0;

// logic check to understand when the game has first loaded.  
// This is so that the player is unarmed when the game loads, rather than having a weapon in hand.  
// This is default functionality
static bool g_game_has_started = false;

using ScriptRegisterFn = void(*)(HMODULE, void(*)());
using ScriptUnregisterFn = void(*)(HMODULE);
using ScriptWaitFn = void(*)(DWORD);
using NativeInitFn = void(*)(uint64_t);
using NativePush64Fn = void(*)(uint64_t);
using NativeCallFn = uint64_t*(*)();

static void LoadPatchConfig() {
    char iniPath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, iniPath, MAX_PATH);
    char* slash = strrchr(iniPath, '\\');
    if (slash) slash[1] = '\0'; else iniPath[0] = '\0';
    strcat_s(iniPath, "RealVRCompat.ini");
    g_patchFirstPersonJump = GetPrivateProfileIntA("patches", "FirstPersonJump", 0, iniPath);
    g_patchVehicleCamNop   = GetPrivateProfileIntA("patches", "VehicleCamNop",   0, iniPath);
    g_patchCamPoolWriteNop = GetPrivateProfileIntA("patches", "CamPoolWriteNop", 0, iniPath);
    g_patchForceFallback   = GetPrivateProfileIntA("patches", "ForceFallback",   0, iniPath);
    g_patchCamPoolResolve  = GetPrivateProfileIntA("patches", "CamPoolResolve",  1, iniPath);
    g_patchCamMetaOldOffset = GetPrivateProfileIntA("patches", "CamMetaOldOffset", 1, iniPath);
    g_patchRestoreCamMetadata = GetPrivateProfileIntA("patches", "RestoreCamMetadata", 1, iniPath);
    g_patchRestoreRVRState = GetPrivateProfileIntA("patches", "RestoreRVRState", 1, iniPath);
    g_patchActiveEngine    = GetPrivateProfileIntA("patches", "ActiveEnginePatches", 1, iniPath);
    g_enginePatchDelaySec  = GetPrivateProfileIntA("patches", "EnginePatchDelaySec", 70, iniPath);
    g_firstPersonRearmFrames = GetPrivateProfileIntA("patches", "FirstPersonRearmFrames", 0, iniPath);
    g_firstPersonGuardFrames = GetPrivateProfileIntA("patches", "FirstPersonGuardFrames", 600, iniPath);
    g_firstPersonGuardPulseFrames = GetPrivateProfileIntA("patches", "FirstPersonGuardPulseFrames", 0, iniPath);
    g_firstPersonGuardInterval = GetPrivateProfileIntA("patches", "FirstPersonGuardInterval", 90, iniPath);
    g_firstPersonFlagHold = GetPrivateProfileIntA("patches", "FirstPersonFlagHold", 0, iniPath);
    g_firstPersonControlFixFrames = GetPrivateProfileIntA("patches", "FirstPersonControlFixFrames", 0, iniPath);
    g_firstPersonUnclampFrames = GetPrivateProfileIntA("patches", "FirstPersonUnclampFrames", 0, iniPath);
    g_firstPersonResetRearmFrames = GetPrivateProfileIntA("patches", "FirstPersonResetRearmFrames", 0, iniPath);
    g_firstPersonSoftReset = GetPrivateProfileIntA("patches", "FirstPersonSoftReset", 1, iniPath);
    g_firstPersonSoftResetFrames = GetPrivateProfileIntA("patches", "FirstPersonSoftResetFrames", 8, iniPath);
    g_keepVehicleFirstPerson = GetPrivateProfileIntA("patches", "KeepVehicleFirstPerson", 1, iniPath);
    g_respawnGraceFrames = GetPrivateProfileIntA("patches", "RespawnGraceFrames", 300, iniPath);
    g_scriptCamReset = GetPrivateProfileIntA("patches", "ScriptCamReset", 0, iniPath);
    g_scriptCamResetFrames = GetPrivateProfileIntA("patches", "ScriptCamResetFrames", 2, iniPath);
    g_vehicleExitModelReset = GetPrivateProfileIntA("patches", "VehicleExitModelReset", 1, iniPath);
    g_modelResetRequireControl = GetPrivateProfileIntA("patches", "ModelResetRequireControl", 1, iniPath);
    g_modelResetDeferMaxFrames = GetPrivateProfileIntA("patches", "ModelResetDeferMaxFrames", 300, iniPath);
    g_modelResetPreserveWeapons = GetPrivateProfileIntA("patches", "ModelResetPreserveWeapons", 1, iniPath);
    g_modelResetPreserveWantedLevel = GetPrivateProfileIntA("patches", "ModelResetPreserveWantedLevel", 1, iniPath);
    g_cutsceneHeadingFix = GetPrivateProfileIntA("patches", "CutsceneHeadingFix", 1, iniPath);
    g_cutsceneSoftReset = GetPrivateProfileIntA("patches", "CutsceneSoftReset", 1, iniPath);
    g_cutsceneScriptCamReset = GetPrivateProfileIntA("patches", "CutsceneScriptCamReset", 1, iniPath);
    g_cutsceneEndSettleFrames = GetPrivateProfileIntA("patches", "CutsceneEndSettleFrames", 2, iniPath);
    g_cutsceneModelReset = GetPrivateProfileIntA("patches", "CutsceneModelReset", 0, iniPath);
    g_cutsceneGroundWaitMaxFrames = GetPrivateProfileIntA("patches", "CutsceneGroundWaitMaxFrames", 180, iniPath);
    g_controlLossHeadingFix = GetPrivateProfileIntA("patches", "ControlLossHeadingFix", 1, iniPath);
    g_controlLossThresholdFrames = GetPrivateProfileIntA("patches", "ControlLossThresholdFrames", 45, iniPath);
    g_cutsceneRecenterFix = GetPrivateProfileIntA("patches", "CutsceneRecenterFix", 1, iniPath);
    g_vehicleExitRecenterFix = GetPrivateProfileIntA("patches", "VehicleExitRecenterFix", 0, iniPath);
    g_cutsceneRecenterFix = g_cutsceneRecenterFix ? 1 : 0;
    g_vehicleExitRecenterFix = g_vehicleExitRecenterFix ? 1 : 0;
    g_cutsceneHeadingFix = g_cutsceneHeadingFix ? 1 : 0;
    g_cutsceneSoftReset = g_cutsceneSoftReset ? 1 : 0;
    g_cutsceneScriptCamReset = g_cutsceneScriptCamReset ? 1 : 0;
    g_controlLossHeadingFix = g_controlLossHeadingFix ? 1 : 0;
    if (g_cutsceneEndSettleFrames < 0) g_cutsceneEndSettleFrames = 0;
    if (g_cutsceneEndSettleFrames > 60) g_cutsceneEndSettleFrames = 60;
    g_cutsceneModelReset = g_cutsceneModelReset ? 1 : 0;
    if (g_cutsceneGroundWaitMaxFrames < 0) g_cutsceneGroundWaitMaxFrames = 0;
    if (g_cutsceneGroundWaitMaxFrames > 1800) g_cutsceneGroundWaitMaxFrames = 1800;
    if (g_controlLossThresholdFrames < 5) g_controlLossThresholdFrames = 5;
    if (g_controlLossThresholdFrames > 600) g_controlLossThresholdFrames = 600;
    g_vehicleExitModelReset = g_vehicleExitModelReset ? 1 : 0;
    g_modelResetRequireControl = g_modelResetRequireControl ? 1 : 0;
    g_modelResetPreserveWeapons = g_modelResetPreserveWeapons ? 1 : 0;
    g_modelResetPreserveWantedLevel = g_modelResetPreserveWantedLevel ? 1 : 0;
    if (g_modelResetDeferMaxFrames < 0) g_modelResetDeferMaxFrames = 0;
    if (g_modelResetDeferMaxFrames > 1800) g_modelResetDeferMaxFrames = 1800;
    if (g_scriptCamResetFrames < 1) g_scriptCamResetFrames = 1;
    if (g_scriptCamResetFrames > 30) g_scriptCamResetFrames = 30;
    if (g_enginePatchDelaySec < 0) g_enginePatchDelaySec = 0;
    if (g_enginePatchDelaySec > 300) g_enginePatchDelaySec = 300;
    if (g_firstPersonRearmFrames < 0) g_firstPersonRearmFrames = 0;
    if (g_firstPersonRearmFrames > 240) g_firstPersonRearmFrames = 240;
    if (g_firstPersonGuardFrames < 0) g_firstPersonGuardFrames = 0;
    if (g_firstPersonGuardFrames > 1800) g_firstPersonGuardFrames = 1800;
    if (g_firstPersonGuardPulseFrames < 0) g_firstPersonGuardPulseFrames = 0;
    if (g_firstPersonGuardPulseFrames > 60) g_firstPersonGuardPulseFrames = 60;
    if (g_firstPersonGuardInterval < 1) g_firstPersonGuardInterval = 1;
    if (g_firstPersonGuardInterval > 600) g_firstPersonGuardInterval = 600;
    g_firstPersonFlagHold = g_firstPersonFlagHold ? 1 : 0;
    if (g_firstPersonControlFixFrames < 0) g_firstPersonControlFixFrames = 0;
    if (g_firstPersonControlFixFrames > 3600) g_firstPersonControlFixFrames = 3600;
    if (g_firstPersonUnclampFrames < 0) g_firstPersonUnclampFrames = 0;
    if (g_firstPersonUnclampFrames > 3600) g_firstPersonUnclampFrames = 3600;
    if (g_firstPersonResetRearmFrames < 0) g_firstPersonResetRearmFrames = 0;
    if (g_firstPersonResetRearmFrames > 120) g_firstPersonResetRearmFrames = 120;
    g_firstPersonSoftReset = g_firstPersonSoftReset ? 1 : 0;
    if (g_firstPersonSoftResetFrames < 1) g_firstPersonSoftResetFrames = 1;
    if (g_firstPersonSoftResetFrames > 120) g_firstPersonSoftResetFrames = 120;
    g_keepVehicleFirstPerson = g_keepVehicleFirstPerson ? 1 : 0;
    if (g_respawnGraceFrames < 0) g_respawnGraceFrames = 0;
    if (g_respawnGraceFrames > 1800) g_respawnGraceFrames = 1800;
}

#ifndef REALVRCOMPAT_ENABLE_ACTIVE_PATCH
#define REALVRCOMPAT_ENABLE_ACTIVE_PATCH 0
#endif

#ifndef REALVRCOMPAT_ENABLE_VERSION_PATCH
#define REALVRCOMPAT_ENABLE_VERSION_PATCH 1
#endif

static void BuildLogPath() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    if (slash) {
        slash[1] = '\0';
        strcpy_s(g_logPath, path);
        strcat_s(g_logPath, "RealVRCompat.log");
    } else {
        strcpy_s(g_logPath, "RealVRCompat.log");
    }
}

static void CLog(const char* fmt, ...) {
    if (!g_logPath[0]) BuildLogPath();

    FILE* f = nullptr;
    fopen_s(&f, g_logPath, "ab");
    if (!f) return;

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    fprintf(f, "[%02u:%02u:%02u.%03u] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\r\n");
    fclose(f);
}

static uint32_t ModuleSize(HMODULE h) {
    MODULEINFO mi = {};
    if (!h || !K32GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi))) return 0;
    return mi.SizeOfImage;
}

static HMODULE FindModuleByName(const char* wantedName) {
    HMODULE mods[1024] = {};
    DWORD needed = 0;
    HANDLE proc = GetCurrentProcess();
    if (!K32EnumProcessModules(proc, mods, sizeof(mods), &needed)) return nullptr;

    int count = (int)(needed / sizeof(HMODULE));
    for (int i = 0; i < count; ++i) {
        char path[MAX_PATH] = {};
        if (!K32GetModuleFileNameExA(proc, mods[i], path, MAX_PATH)) continue;
        const char* name = strrchr(path, '\\');
        name = name ? name + 1 : path;
        if (_stricmp(name, wantedName) == 0) return mods[i];
    }
    return nullptr;
}

static HMODULE FindModuleExporting(const char* exportName) {
    if (!exportName) return nullptr;

    HMODULE mods[1024] = {};
    DWORD needed = 0;
    HANDLE proc = GetCurrentProcess();
    if (!K32EnumProcessModules(proc, mods, sizeof(mods), &needed)) return nullptr;

    int count = (int)(needed / sizeof(HMODULE));
    for (int i = 0; i < count; ++i) {
        FARPROC procAddr = GetProcAddress(mods[i], exportName);
        if (procAddr) return mods[i];
    }
    return nullptr;
}

static HMODULE FindOrLoadRVRProxy() {
    if (g_rvrProxy && GetProcAddress(g_rvrProxy, "g_RVRData")) return g_rvrProxy;

    HMODULE found = FindModuleExporting("g_RVRData");
    if (found) {
        g_rvrProxy = found;
        return g_rvrProxy;
    }

    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    char* slash = strrchr(exePath, '\\');
    if (!slash) return nullptr;
    slash[1] = '\0';
    strcat_s(exePath, "d3d11.dll");

    HMODULE loaded = LoadLibraryA(exePath);
    if (loaded && GetProcAddress(loaded, "g_RVRData")) {
        g_rvrProxy = loaded;
        // Disabled: proxy loaded log
        return g_rvrProxy;
    }

    // Disabled: proxy load failed log
    return nullptr;
}

static bool WriteU32(void* addr, uint32_t value) {
    if (!addr) return false;
    DWORD old = 0;
    if (!VirtualProtect(addr, sizeof(value), PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(addr, &value, sizeof(value));
    VirtualProtect(addr, sizeof(value), old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, sizeof(value));
    return true;
}

static bool WritePtr(void* addr, void* value) {
    if (!addr) return false;
    DWORD old = 0;
    if (!VirtualProtect(addr, sizeof(value), PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(addr, &value, sizeof(value));
    VirtualProtect(addr, sizeof(value), old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, sizeof(value));
    return true;
}

static bool WriteU8(void* addr, uint8_t value) {
    if (!addr) return false;
    DWORD old = 0;
    if (!VirtualProtect(addr, sizeof(value), PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(addr, &value, sizeof(value));
    VirtualProtect(addr, sizeof(value), old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, sizeof(value));
    return true;
}

static bool WriteBytes(void* addr, const uint8_t* bytes, size_t size) {
    if (!addr || !bytes || !size) return false;
    DWORD old = 0;
    if (!VirtualProtect(addr, size, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(addr, bytes, size);
    VirtualProtect(addr, size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), addr, size);
    return true;
}

// ----------------------------------------------------------------------------
// RealVR internal state (reverse-engineered from RealVR.asi's own hotkey
// handler, not natives or approximated behavior). RealVR.asi registers a
// single ScriptHookV keyboard callback that just timestamps key events into
// a per-VK-code table; the actual hotkey ACTIONS live in the main per-frame
// handler, which polls that table. Disassembling those checks gives the
// exact memory location the NUMPAD "/" (Recenter HMD) hotkey reads/writes:
//
//   - g_RVRData+0x821 (byte): write 1 to request an HMD recenter. Confirmed
//     as the exact byte the Recenter HMD hotkey's handler sets after its
//     debounce check passes.
//
// This address was located by finding the keyboardHandlerRegister call in
// the import table, disassembling the registered callback (which only
// records raw key events), then locating the per-frame code that reads the
// VK code's table slot and the state it touches next to it. g_RVRData
// itself is the same pointer already resolved elsewhere in this file via
// RealVR+0x38020.
//
// Note: the same method also located RealVR.asi_base+0x384F8, the live
// HeadingControl value (0=always, 1=only aiming, 2=never) the "Y" hotkey
// cycles through. A vehicle-exit fix built on that address didn't resolve
// the reported HeadingControl issue and has been removed for now - the
// address is left here as a starting point for revisiting it later.
// ----------------------------------------------------------------------------
static uint8_t* GetRVRDataPtr() {
    if (!g_realvr) return nullptr;
    uint8_t** slot = (uint8_t**)((uint8_t*)g_realvr + 0x38020);
    __try {
        return *slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Requests an HMD recenter - identical to pressing NUMPAD "/" in-game. This
// recalibrates the offset between the HMD's physical orientation and the
// in-game forward direction, which is exactly the kind of resync needed
// after a cutscene (or anywhere else the two have drifted), without
// touching the ped, camera object, or heading control setting at all.
static bool TriggerRVRRecenter(const char* reason) {
    uint8_t* rvrData = GetRVRDataPtr();
    if (!rvrData) {
        CLog("compat recenter: g_RVRData unavailable, skipped (reason=%s)", reason ? reason : "-");
        return false;
    }
    bool ok = WriteU8(rvrData + 0x821, 1);
    CLog("compat recenter: %s (reason=%s)", ok ? "requested" : "write failed", reason ? reason : "-");
    return ok;
}

struct FileVersion {
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t build = 0;
    uint16_t revision = 0;
};

static bool ReadFileVersionA(const char* path, FileVersion* out) {
    if (!path || !out) return false;

    DWORD handle = 0;
    DWORD size = GetFileVersionInfoSizeA(path, &handle);
    if (!size) return false;

    uint8_t* data = new uint8_t[size];
    if (!GetFileVersionInfoA(path, 0, size, data)) {
        delete[] data;
        return false;
    }

    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    bool ok = VerQueryValueA(data, "\\", (void**)&info, &len) && info && len >= sizeof(VS_FIXEDFILEINFO);
    if (ok) {
        out->major = HIWORD(info->dwFileVersionMS);
        out->minor = LOWORD(info->dwFileVersionMS);
        out->build = HIWORD(info->dwFileVersionLS);
        out->revision = LOWORD(info->dwFileVersionLS);
    }

    delete[] data;
    return ok;
}

static int DetectRealVRVersionId(const FileVersion& v) {
    // ScriptHookV returns enum values, not the EXE build number. For
    // 1.0.3788.0 ScriptHook reports 101, as seen in RealVR's own warning path.
    if (v.major == 1 && v.minor == 0 && v.build == 3788) return 101;

    // Keep known older ScriptHook ids available for regression tests/logging.
    // These are only used when the EXE version resource matches exactly.
    if (v.major == 1 && v.minor == 0 && v.build == 2612) return 64;

    return -1;
}

static void __fastcall RealVRLogNoop(void*, uint64_t) {
}

static void LogRealVRSlot(const char* name, uint32_t rva) {
    if (!g_realvr) return;
    uint64_t value = 0;
    __try {
        value = *(uint64_t*)((uint8_t*)g_realvr + rva);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // Disabled: slot read failed log
        return;
    }
    // Disabled: slot log (too verbose)
}

static int NopIndirectCallsToRealVRSlot(HMODULE realvr, uint32_t slotRva, const char* name) {
    if (!realvr) return 0;
    uint8_t* base = (uint8_t*)realvr;
    uint32_t size = ModuleSize(realvr);
    uintptr_t target = (uintptr_t)(base + slotRva);
    uint8_t nops[6] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
    int patched = 0;

    for (uint32_t i = 0; i + 6 <= size; ++i) {
        if (base[i] != 0xFF || base[i + 1] != 0x15) continue;
        int32_t disp = 0;
        memcpy(&disp, base + i + 2, sizeof(disp));
        uintptr_t resolved = (uintptr_t)(base + i + 6 + disp);
        if (resolved != target) continue;

        uint8_t before[6] = {};
        memcpy(before, base + i, sizeof(before));
        if (WriteBytes(base + i, nops, sizeof(nops))) {
            ++patched;
            // Disabled: nop log (too verbose)
        }
    }

    // Disabled: nop slot log
    return patched;
}

static void* ResolveExportFromRVRProxy(const char* name) {
    HMODULE proxy = FindOrLoadRVRProxy();
    if (!proxy) return nullptr;
    return (void*)GetProcAddress(proxy, name);
}

static void* PatchRealVRProxySlots(HMODULE realvr, const char* reason) {
    if (!realvr) return nullptr;

    struct SlotExport {
        uint32_t slotRva;
        const char* name;
    };
    const SlotExport slots[] = {
        { 0x38020, "g_RVRData" },
        { 0x380B0, "g_fRVRGameProj" },
        { 0x38018, "RVRLog" },
        { 0x380C0, "RVRGetFrameDesc" },
        { 0x37FD8, "RVRGetPoseDesc" },
        { 0x37FE0, "RVRSeqCheck" },
        { 0x37FF0, "RVRWaitAndTrackHMD" },
    };

    HMODULE proxy = FindOrLoadRVRProxy();
    if (!proxy) {
        CLog("proxy slot patch skipped: no module exports g_RVRData (%s)", reason ? reason : "");
        return nullptr;
    }

    char proxyPath[MAX_PATH] = {};
    K32GetModuleFileNameExA(GetCurrentProcess(), proxy, proxyPath, MAX_PATH);
    CLog("proxy slot patch using module=%p path=%s reason=%s", proxy, proxyPath, reason ? reason : "");

    void* gdata = nullptr;
    for (const SlotExport& s : slots) {
        void* value = (void*)GetProcAddress(proxy, s.name);
        if (strcmp(s.name, "g_RVRData") == 0) gdata = value;
        bool ok = value && WritePtr((uint8_t*)realvr + s.slotRva, value);
        CLog("proxy slot %-18s RealVR+0x%X value=%p patch=%s",
             s.name, s.slotRva, value, ok ? "OK" : "FAIL");
    }

    return gdata;
}

static void LogCriticalRealVRSlots(const char* label) {
    // Disabled: critical slots log
    LogRealVRSlot("GetTickCount", 0x23050);
    LogRealVRSlot("GetModuleHandleA", 0x23080);
    LogRealVRSlot("WaitForSingleObjectEx", 0x23090);
    LogRealVRSlot("GetProcAddress", 0x230A0);
    LogRealVRSlot("keyboardUnregister", 0x232A0);
    LogRealVRSlot("getGameVersion", 0x232A8);
    LogRealVRSlot("getScriptHandleBase", 0x232B0);
    LogRealVRSlot("scriptWait", 0x232B8);
    LogRealVRSlot("nativePush64", 0x232C0);
    LogRealVRSlot("nativeCall", 0x232C8);
    LogRealVRSlot("nativeInit", 0x232D0);
    LogRealVRSlot("scriptRegister", 0x232D8);
    LogRealVRSlot("keyboardRegister", 0x232E0);
    LogRealVRSlot("scriptUnregister", 0x232E8);
    LogRealVRSlot("logFunction", 0x38018);
    LogRealVRSlot("rvrData", 0x38020);
    LogRealVRSlot("camMetadataPool", 0x38098);
}

static void LogAddressInfo(const char* label, uintptr_t addr) {
    if (!addr) {
        CLog("addr %-18s 0x0", label ? label : "");
        return;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    bool hasMem = VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == sizeof(mbi);

    HMODULE mods[1024] = {};
    DWORD needed = 0;
    HMODULE containing = nullptr;
    HANDLE proc = GetCurrentProcess();
    if (K32EnumProcessModules(proc, mods, sizeof(mods), &needed)) {
        int count = (int)(needed / sizeof(HMODULE));
        for (int i = 0; i < count; ++i) {
            MODULEINFO mi = {};
            if (!K32GetModuleInformation(proc, mods[i], &mi, sizeof(mi))) continue;
            uintptr_t base = (uintptr_t)mi.lpBaseOfDll;
            uintptr_t end = base + mi.SizeOfImage;
            if (addr >= base && addr < end) {
                containing = mods[i];
                break;
            }
        }
    }

    char path[MAX_PATH] = {};
    if (containing) K32GetModuleFileNameExA(proc, containing, path, MAX_PATH);

    char mapped[MAX_PATH] = {};
    GetMappedFileNameA(proc, (void*)addr, mapped, MAX_PATH);

    CLog("addr %-18s value=0x%llX module=%p moduleRva=0x%llX modulePath=%s memBase=%p allocBase=%p protect=0x%X state=0x%X type=0x%X mapped=%s",
         label ? label : "",
         (unsigned long long)addr,
         containing,
         containing ? (unsigned long long)(addr - (uintptr_t)containing) : 0ull,
         path[0] ? path : "-",
         hasMem ? mbi.BaseAddress : nullptr,
         hasMem ? mbi.AllocationBase : nullptr,
         hasMem ? mbi.Protect : 0,
         hasMem ? mbi.State : 0,
         hasMem ? mbi.Type : 0,
         mapped[0] ? mapped : "-");
}

static void LogStackNear(uintptr_t rsp) {
    if (!rsp) return;
    uintptr_t realvrBase = (uintptr_t)g_realvr;
    uintptr_t realvrEnd = realvrBase + (g_realvr ? ModuleSize(g_realvr) : 0);
    CLog("stack dump from rsp=0x%llX", (unsigned long long)rsp);
    for (int i = 0; i < 24; ++i) {
        uintptr_t* slot = (uintptr_t*)(rsp + (uintptr_t)i * sizeof(uintptr_t));
        uintptr_t value = 0;
        __try {
            value = *slot;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            CLog("stack[%02d] read failed", i);
            continue;
        }
        bool inRealVR = value >= realvrBase && value < realvrEnd;
        CLog("stack[%02d] @0x%llX = 0x%llX%s0x%llX",
             i,
             (unsigned long long)(uintptr_t)slot,
             (unsigned long long)value,
             inRealVR ? " RealVR+":" ",
             inRealVR ? (unsigned long long)(value - realvrBase) : 0ull);
        if (i < 6 && value > 0x10000) {
            char label[32] = {};
            sprintf_s(label, "stack[%02d]", i);
            LogAddressInfo(label, value);
        }
    }
}

static int RexInstrLen(const uint8_t* b);  // forward decl — defined near PatchVehicleCamNullCall
static void* TryCamPoolSlot(HMODULE realvr, uintptr_t slotAddr, const char* tag);
static void ApplyOriginalEnginePatches(HMODULE realvr);
static bool ProbeCameraStructures();  // defined after Scan(); returns true once manager is non-null
static bool LooksHeap(uintptr_t p, uint8_t* modBase, size_t modSize);
static bool LooksReadablePtr(uintptr_t p);

static void CacheCamMetadataPool(uintptr_t value, const char* reason) {
    if (!g_patchRestoreCamMetadata || value < 0x10000 || !LooksReadablePtr(value)) return;
    if (g_savedCamMetadataPool == value) return;
    g_savedCamMetadataPool = value;
    CLog("camMetadata restore: cached +0x38098=0x%llX reason=%s",
         (unsigned long long)value, reason ? reason : "-");
}

static bool RestoreCamMetadataPoolIfCleared(const char* reason) {
    if (!g_patchRestoreCamMetadata || !g_realvr || g_savedCamMetadataPool < 0x10000) return false;

    uintptr_t cur = 0;
    __try {
        cur = *(uintptr_t*)((uint8_t*)g_realvr + 0x38098);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (cur != 0) {
        CacheCamMetadataPool(cur, reason);
        return false;
    }
    if (!LooksReadablePtr(g_savedCamMetadataPool)) return false;

    bool ok = WritePtr((uint8_t*)g_realvr + 0x38098, (void*)g_savedCamMetadataPool);
    CLog("camMetadata restore: +0x38098 was null; restored 0x%llX reason=%s patch=%s",
         (unsigned long long)g_savedCamMetadataPool,
         reason ? reason : "-",
         ok ? "OK" : "FAIL");
    return ok;
}

static uintptr_t CurrentRVRData() {
    if (!g_realvr) return 0;
    uintptr_t value = 0;
    __try {
        value = *(uintptr_t*)((uint8_t*)g_realvr + 0x38020);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        value = 0;
    }
    return value;
}

template <typename T>
static T ReadRealVRSlot(uint32_t rva) {
    T value = nullptr;
    if (!g_realvr) return value;
    __try {
        value = (T)(*(uintptr_t*)((uint8_t*)g_realvr + rva));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        value = nullptr;
    }
    return value;
}

static bool PushNativeArg(uint64_t value) {
    NativePush64Fn push = ReadRealVRSlot<NativePush64Fn>(0x232C0);
    if (!push) return false;
    push(value);
    return true;
}

static bool InvokeNativeRaw(uint64_t hash, const uint64_t* args, int argc, uint64_t* out) {
    NativeInitFn init = ReadRealVRSlot<NativeInitFn>(0x232D0);
    NativeCallFn call = ReadRealVRSlot<NativeCallFn>(0x232C8);
    NativePush64Fn push = ReadRealVRSlot<NativePush64Fn>(0x232C0);
    if (!init || !call || !push) return false;
    __try {
        init(hash);
        for (int i = 0; i < argc; ++i) push(args[i]);
        uint64_t* ret = call();
        if (out) *out = ret ? *ret : 0;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static uint64_t FloatArg(float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    return (uint64_t)bits;
}

static int NativeInt0(uint64_t hash, int fallback = 0) {
    uint64_t out = 0;
    return InvokeNativeRaw(hash, nullptr, 0, &out) ? (int)out : fallback;
}

static float NativeFloat0(uint64_t hash, float fallback = 0.0f) {
    uint64_t out = 0;
    if (!InvokeNativeRaw(hash, nullptr, 0, &out)) return fallback;
    uint32_t bits = (uint32_t)out;
    float value = fallback;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static float NativeFloat1(uint64_t hash, uint64_t a, float fallback = 0.0f) {
    uint64_t args[1] = { a };
    uint64_t out = 0;
    if (!InvokeNativeRaw(hash, args, 1, &out)) return fallback;
    uint32_t bits = (uint32_t)out;
    float value = fallback;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static bool NativeBool2(uint64_t hash, uint64_t a, uint64_t b, bool fallback = false) {
    uint64_t args[2] = { a, b };
    uint64_t out = 0;
    return InvokeNativeRaw(hash, args, 2, &out) ? (out != 0) : fallback;
}

static bool NativeBool0(uint64_t hash, bool fallback = false) {
    uint64_t out = 0;
    return InvokeNativeRaw(hash, nullptr, 0, &out) ? (out != 0) : fallback;
}

static bool NativeBool1(uint64_t hash, uint64_t a, bool fallback = false) {
    uint64_t args[1] = { a };
    uint64_t out = 0;
    return InvokeNativeRaw(hash, args, 1, &out) ? (out != 0) : fallback;
}

static int NativeInt1(uint64_t hash, uint64_t a, int fallback = 0) {
    uint64_t args[1] = { a };
    uint64_t out = 0;
    return InvokeNativeRaw(hash, args, 1, &out) ? (int)out : fallback;
}

static int NativeInt2(uint64_t hash, uint64_t a, uint64_t b, int fallback = 0) {
    uint64_t args[2] = { a, b };
    uint64_t out = 0;
    return InvokeNativeRaw(hash, args, 2, &out) ? (int)out : fallback;
}

static void NativeVoid1(uint64_t hash, uint64_t a) {
    uint64_t args[1] = { a };
    InvokeNativeRaw(hash, args, 1, nullptr);
}

static void NativeVoid3(uint64_t hash, uint64_t a, uint64_t b, uint64_t c) {
    uint64_t args[3] = { a, b, c };
    InvokeNativeRaw(hash, args, 3, nullptr);
}

static void NativeVoid2(uint64_t hash, uint64_t a, uint64_t b) {
    uint64_t args[2] = { a, b };
    InvokeNativeRaw(hash, args, 2, nullptr);
}

static void NativeVoid4(uint64_t hash, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    uint64_t args[4] = { a, b, c, d };
    InvokeNativeRaw(hash, args, 4, nullptr);
}

// Helper for SET_ENTITY_VELOCITY (4 args: entity + 3 floats for x,y,z)
static void SetEntityVelocity(int entity, float x, float y, float z) {
    uint64_t args[4] = { (uint64_t)(uint32_t)entity, FloatArg(x), FloatArg(y), FloatArg(z) };
    InvokeNativeRaw(0x1C99BB7B6E96D16Full, args, 4, nullptr); // SET_ENTITY_VELOCITY
}

static void CompatScriptMain();

static bool RegisterCompatScript(const char* reason) {
    if (!g_self || !g_realvr) return false;
    if (InterlockedCompareExchange(&g_scriptRegistered, 1, 0) != 0) return true;
    ScriptRegisterFn reg = ReadRealVRSlot<ScriptRegisterFn>(0x232D8);
    if (!reg) {
        InterlockedExchange(&g_scriptRegistered, 0);
        CLog("compat script register skipped: scriptRegister slot null reason=%s", reason ? reason : "-");
        return false;
    }
    __try {
        reg(g_self, CompatScriptMain);
        CLog("compat script registered reason=%s", reason ? reason : "-");
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_scriptRegistered, 0);
        CLog("compat script register exception reason=%s", reason ? reason : "-");
        return false;
    }
}

static void CacheRVRState(const char* reason) {
    if (!g_patchRestoreRVRState) return;
    uintptr_t base = CurrentRVRData();
    if (base < 0x10000 || !LooksReadablePtr(base)) return;

    bool ok = false;
    __try {
        g_savedRVR_10 = *(uint8_t*)(base + 0x10);
        g_savedRVR_11 = *(uint8_t*)(base + 0x11);
        g_savedRVR_79 = *(uint8_t*)(base + 0x79);
        g_savedRVR_E0 = *(uint8_t*)(base + 0xE0);
        g_savedRVR_840 = *(uint8_t*)(base + 0x840);
        g_savedRVR_34 = *(uint32_t*)(base + 0x34);
        g_savedRVR_3C = *(uint32_t*)(base + 0x3C);
        ok = true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    if (!ok) return;

    if (!g_savedRVRStateValid || g_savedRVRData != base) {
        CLog("rvrState restore: cached g_RVRData=0x%llX reason=%s [10]=%u [11]=%u [79]=%u [E0]=%u [840]=%u [34]=%u [3C]=%u",
             (unsigned long long)base, reason ? reason : "-",
             g_savedRVR_10, g_savedRVR_11, g_savedRVR_79, g_savedRVR_E0, g_savedRVR_840,
             g_savedRVR_34, g_savedRVR_3C);
    }
    g_savedRVRData = base;
    g_savedRVRStateValid = true;
}

static uint64_t HashBytesSafe(const void* ptr, size_t len) {
    if (!ptr || len == 0) return 0;
    const uint8_t* p = (const uint8_t*)ptr;
    uint64_t h = 1469598103934665603ull;
    __try {
        for (size_t i = 0; i < len; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return h;
}

static void LogRVRStateSnapshot(const char* reason) {
    uintptr_t base = CurrentRVRData();
    if (base < 0x10000 || !LooksReadablePtr(base)) return;

    uint32_t state = 0, f34 = 0, f3c = 0, d4f4 = 0, da9c = 0;
    uint8_t i10 = 0, i11 = 0, d79 = 0, e0 = 0, d51d = 0, d821 = 0, d840 = 0;
    uint8_t rv38040 = 0, rv38041 = 0;
    uint32_t rv380d4 = 0, rv384fc = 0;
    uintptr_t mgr = 0, c0 = 0, target = 0, scene = 0;
    __try {
        state = *(uint32_t*)(base + 0x00);
        f34 = *(uint32_t*)(base + 0x34);
        f3c = *(uint32_t*)(base + 0x3C);
        i10 = *(uint8_t*)(base + 0x10);
        i11 = *(uint8_t*)(base + 0x11);
        d79 = *(uint8_t*)(base + 0x79);
        e0 = *(uint8_t*)(base + 0xE0);
        d4f4 = *(uint32_t*)(base + 0x4F4);
        d51d = *(uint8_t*)(base + 0x51D);
        d821 = *(uint8_t*)(base + 0x821);
        d840 = *(uint8_t*)(base + 0x840);
        da9c = *(uint32_t*)(base + 0xA9C);
        mgr = *(uintptr_t*)(base + 0x08);
        c0 = *(uintptr_t*)(base + 0xC0);
        target = *(uintptr_t*)(base - 0x1950);
        scene = *(uintptr_t*)(base - 0x1948);
        if (g_realvr) {
            rv38040 = *((uint8_t*)g_realvr + 0x38040);
            rv38041 = *((uint8_t*)g_realvr + 0x38041);
            rv380d4 = *(uint32_t*)((uint8_t*)g_realvr + 0x380D4);
            rv384fc = *(uint32_t*)((uint8_t*)g_realvr + 0x384FC);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    uint64_t h000 = HashBytesSafe((void*)(base + 0x00), 0x120);
    uint64_t h77c = HashBytesSafe((void*)(base + 0x77C), 0x70);
    uint64_t h890 = HashBytesSafe((void*)(base + 0x890), 0x200);
    uint64_t h910 = HashBytesSafe((void*)(base + 0x910), 0x200);

    CLog("rvrState snapshot %s base=0x%llX state=%u [34]=%u [3C]=%u i10=%u i11=%u d79=%u e0=%u d4f4=%u d51d=%u d821=%u d840=%u da9c=%u rv38040=%u rv38041=%u rv380d4=%u rv384fc=%u mgr=0x%llX c0=0x%llX target=0x%llX scene=0x%llX h000=%016llX h77c=%016llX h890=%016llX h910=%016llX",
         reason ? reason : "-",
         (unsigned long long)base,
         state, f34, f3c, i10, i11, d79, e0, d4f4, d51d, d821, d840, da9c,
         rv38040, rv38041, rv380d4, rv384fc,
         (unsigned long long)mgr,
         (unsigned long long)c0,
         (unsigned long long)target,
         (unsigned long long)scene,
         (unsigned long long)h000,
         (unsigned long long)h77c,
         (unsigned long long)h890,
         (unsigned long long)h910);

    g_lastRVRMgr = mgr;
    g_lastRVRC0 = c0;
    g_lastRVRTarget = target;
    g_lastRVRScene = scene;
}

static void RestoreRVRState(const char* reason) {
    if (!g_patchRestoreRVRState || !g_savedRVRStateValid) return;
    uintptr_t base = CurrentRVRData();
    if (base != g_savedRVRData || base < 0x10000 || !LooksReadablePtr(base)) return;

    uint8_t cur10 = 0, cur11 = 0, cur79 = 0, curE0 = 0, cur840 = 0;
    uint32_t cur34 = 0, cur3C = 0;
    bool okRead = false;
    __try {
        cur10 = *(uint8_t*)(base + 0x10);
        cur11 = *(uint8_t*)(base + 0x11);
        cur79 = *(uint8_t*)(base + 0x79);
        curE0 = *(uint8_t*)(base + 0xE0);
        cur840 = *(uint8_t*)(base + 0x840);
        cur34 = *(uint32_t*)(base + 0x34);
        cur3C = *(uint32_t*)(base + 0x3C);
        okRead = true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        okRead = false;
    }
    if (!okRead) return;

    bool changed = cur10 != g_savedRVR_10 || cur11 != g_savedRVR_11;
    if (!changed) return;

    bool ok = true;
    ok = WriteU8((void*)(base + 0x10), g_savedRVR_10) && ok;
    ok = WriteU8((void*)(base + 0x11), g_savedRVR_11) && ok;

    CLog("rvrState restore: restored conservative reason=%s patch=%s old[10,11,79,E0,840,34,3C]=%u,%u,%u,%u,%u,%u,%u new[10,11]=%u,%u",
         reason ? reason : "-",
         ok ? "OK" : "FAIL",
         cur10, cur11, cur79, curE0, cur840, cur34, cur3C,
         g_savedRVR_10, g_savedRVR_11);
}

// High-frequency edge-triggered monitor of RealVR+0x38098. Logs ONLY when the
// pointer value changes, so we can capture exactly what happens at a vehicle
// enter/exit transition without flooding the log. Classifies the value as
// null / inside-GTA5 (static) / heap, and dumps the rotation matrix at +0x150.
static DWORD WINAPI CamPoolDiagThread(void*) {
    uintptr_t lastVal = 0xDEADBEEF;
    uint8_t* gta5Base = (uint8_t*)GetModuleHandleA(nullptr);
    size_t gta5Size = gta5Base ? ModuleSize((HMODULE)gta5Base) : 0;
    bool probeDone = false;
    int probeTicks = 0;
    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        Sleep(150);
        if (!g_realvr) continue;
        if (!g_patchCamPoolResolve) {
            CLog("POOLDIAG disabled (CamPoolResolve=0)");
            return 0;
        }

        // One-time structural probe of GTA5's camera manager, retried until the
        // manager pointer is populated (a few seconds after world load).
        if (!probeDone && ++probeTicks >= 33) {  // ~every 5s
            probeTicks = 0;
            if (ProbeCameraStructures()) probeDone = true;
        }
        uintptr_t cur = 0xDEADBEEF;
        __try { cur = *(uintptr_t*)((uint8_t*)g_realvr + 0x38098); }
        __except(EXCEPTION_EXECUTE_HANDLER) { cur = 0xDEADBEEF; }
        if (cur == 0) RestoreCamMetadataPoolIfCleared("pooldiag");
        else if (cur != 0xDEADBEEF) {
            CacheCamMetadataPool(cur, "pooldiag");
            CacheRVRState("pooldiag");
        }
        if (cur == lastVal) continue;
        lastVal = cur;

        const char* cls = "null";
        if (cur == 0xDEADBEEF) cls = "<unreadable>";
        else if (cur >= 0x10000) {
            if (gta5Base && cur >= (uintptr_t)gta5Base && cur < (uintptr_t)gta5Base + gta5Size)
                cls = "GTA5-static";
            else cls = "heap";
        }

        float m[4] = { 0, 0, 0, 0 };
        bool mOk = false;
        if (cur > 0x10000 && cur != 0xDEADBEEF) {
            __try {
                m[0] = *(float*)(cur + 0x150); m[1] = *(float*)(cur + 0x154);
                m[2] = *(float*)(cur + 0x158); m[3] = *(float*)(cur + 0x15C);
                mOk = true;
            } __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
        // Also peek the [+0]=begin and [+8]=count fields RealVR's 0x5477 loop reads.
        uintptr_t f0 = 0; uint32_t f8 = 0; bool fOk = false;
        if (cur > 0x10000 && cur != 0xDEADBEEF) {
            __try { f0 = *(uintptr_t*)cur; f8 = *(uint32_t*)(cur + 8); fOk = true; }
            __except(EXCEPTION_EXECUTE_HANDLER) {}
        }
        if (mOk)
            CLog("POOLDIAG +0x38098 -> 0x%llX [%s] begin=0x%llX count=%u mtx=%+.3f %+.3f %+.3f %+.3f",
                 (unsigned long long)cur, cls, (unsigned long long)f0, fOk ? f8 : 0, m[0],m[1],m[2],m[3]);
        else
            CLog("POOLDIAG +0x38098 -> 0x%llX [%s] (matrix unreadable, begin=0x%llX count=%u)",
                 (unsigned long long)cur, cls, (unsigned long long)f0, fOk ? f8 : 0);
    }
    return 0;
}

static DWORD WINAPI SlotWatchThread(void*) {
    static float lastCamMatrix[4] = { 0, 0, 0, 0 };
    bool disabledLogged = false;
    for (int i = 0; i < 36 && InterlockedCompareExchange(&g_running, 1, 1); ++i) {
        Sleep(5000);
        if (!g_realvr) continue;
        if (!g_patchCamPoolResolve) {
            if (!disabledLogged) CLog("SlotWatch camPool retries disabled (CamPoolResolve=0)");
            disabledLogged = true;
            return 0;
        }
        char label[64] = {};
        sprintf_s(label, "watch +%ds", (i + 1) * 5);
        LogCriticalRealVRSlots(label);

        // Retry camMetadataPool resolution once the GTA5 camera system initializes.
        // Once RealVR+0x38098 becomes non-null we stop retrying.
        void* currentPool = nullptr;
        __try { currentPool = *(void**)((uint8_t*)g_realvr + 0x38098); }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
        if (currentPool) {
            CacheCamMetadataPool((uintptr_t)currentPool, label);
            CacheRVRState(label);
        } else if (RestoreCamMetadataPoolIfCleared(label)) {
            __try { currentPool = *(void**)((uint8_t*)g_realvr + 0x38098); }
            __except(EXCEPTION_EXECUTE_HANDLER) { currentPool = nullptr; }
        }
        if (!currentPool && g_camPoolSlotAddr) {
            void* found = nullptr;
            int slotCount = g_camPoolSlotCount > 0 ? g_camPoolSlotCount : 1;
            for (int si = 0; si < slotCount && !found; ++si) {
                uintptr_t slot = g_camPoolSlotCount > 0 ? g_camPoolSlotAddrs[si] : g_camPoolSlotAddr;
                char tag[48]; sprintf_s(tag, "retry+%ds/s%d", (i+1)*5, si);
                found = TryCamPoolSlot(g_realvr, slot, tag);
            }
            if (found) {
                CLog("camPool retry succeeded at +%ds: pool=0x%llX", (i+1)*5, (unsigned long long)(uintptr_t)found);
                CacheCamMetadataPool((uintptr_t)found, "camPool retry");
                if (g_patchActiveEngine) CLog("camPool: watcher will not apply engine patches; original/delayed thread owns them");
            }
        } else if (currentPool) {
            // Camera is already set. Check if it has changed (entered/exited vehicle).
            // Read the first 4 floats (first row of rotation matrix) to detect mode change.
            float camMatrix[4] = { 0, 0, 0, 0 };
            __try {
                camMatrix[0] = *(float*)((uintptr_t)currentPool + 0x150);
                camMatrix[1] = *(float*)((uintptr_t)currentPool + 0x154);
                camMatrix[2] = *(float*)((uintptr_t)currentPool + 0x158);
                camMatrix[3] = *(float*)((uintptr_t)currentPool + 0x15C);
            } __except(EXCEPTION_EXECUTE_HANDLER) {}

            // Check if matrix changed significantly (more than small epsilon)
            bool matrixChanged = false;
            for (int mi = 0; mi < 4; ++mi) {
                float diff = camMatrix[mi] - lastCamMatrix[mi];
                if (diff < 0) diff = -diff;
                if (diff > 0.01f) { matrixChanged = true; break; }
            }
            if (matrixChanged && i > 1) {  // Skip first few checks
                CLog("camPool: camera matrix changed at +%ds (vehicle/foot transition detected), re-scanning",
                     (i+1)*5);
                char snapBefore[80]; sprintf_s(snapBefore, "before restore %s", label);
                LogRVRStateSnapshot(snapBefore);
                RestoreRVRState(label);
                char snapAfter[80]; sprintf_s(snapAfter, "after restore %s", label);
                LogRVRStateSnapshot(snapAfter);
                if (g_camPoolSlotAddr) {
                    char tag[32]; sprintf_s(tag, "rescan+%ds", (i+1)*5);
                    void* found = TryCamPoolSlot(g_realvr, g_camPoolSlotAddr, tag);
                    if (found) {
                        CLog("camPool rescan succeeded: pool=0x%llX", (unsigned long long)(uintptr_t)found);
                    }
                }
            }
            memcpy(lastCamMatrix, camMatrix, sizeof(lastCamMatrix));
        }
    }
    return 0;
}

static void CompatScriptWait(DWORD ms) {
    ScriptWaitFn wait = ReadRealVRSlot<ScriptWaitFn>(0x232B8);
    if (wait) wait(ms);
    else Sleep(ms ? ms : 1);
}

static void ForcePedFirstPersonPulse(int frame, const char* reason) {
    // CAM::SET_FOLLOW_PED_CAM_VIEW_MODE(4)
    NativeVoid1(0x5A4F9EDF1673F704ull, 4);
    // CAM::_SET_CAM_VIEW_MODE_FOR_CONTEXT(ON_FOOT=0, FIRST_PERSON=4)
    uint64_t ctxArgs[2] = { 0, 4 };
    InvokeNativeRaw(0x2A2173E46DAECD12ull, ctxArgs, 2, nullptr);

    uint8_t fpFlagBefore = 0, modeFlagBefore = 0;
    bool fpFlagOk = false;
    if (g_realvr) {
        __try {
            modeFlagBefore = *((uint8_t*)g_realvr + 0x38040);
            fpFlagBefore = *((uint8_t*)g_realvr + 0x38041);
            fpFlagOk = true;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            fpFlagOk = false;
        }
        WriteU8((uint8_t*)g_realvr + 0x38041, 1);
    }

    if (frame <= 8 || (frame % 30) == 0) {
        CLog("compat first-person rearm pulse frame=%d reason=%s realvr[38040]=%u realvr[38041]=%u flagRead=%s",
             frame, reason ? reason : "-", modeFlagBefore, fpFlagBefore, fpFlagOk ? "OK" : "FAIL");
    }
}

static void EnableFirstPersonLookControls(int frame, const char* reason) {
    // PAD::ENABLE_CONTROL_ACTION(PLAYER_CONTROL, INPUT_LOOK_LR/UD, true)
    NativeVoid3(0x351220255D64C155ull, 0, 1, 1);
    NativeVoid3(0x351220255D64C155ull, 0, 2, 1);
    // Keep vehicle/ped look aliases alive too; harmless if the control id is not used in this context.
    NativeVoid3(0x351220255D64C155ull, 0, 3, 1);
    NativeVoid3(0x351220255D64C155ull, 0, 4, 1);

    if (frame <= 8 || (frame % 120) == 0) {
        CLog("compat first-person control fix frame=%d reason=%s controls=look_lr/look_ud",
             frame, reason ? reason : "-");
    }
}

static bool ReadCompactFPState(uint8_t* d840, uint8_t* rv38041) {
    uintptr_t base = CurrentRVRData();
    if (d840) *d840 = 0;
    if (rv38041) *rv38041 = 0;
    if (base < 0x10000 || !LooksReadablePtr(base) || !g_realvr) return false;
    __try {
        if (d840) *d840 = *(uint8_t*)(base + 0x840);
        if (rv38041) *rv38041 = *((uint8_t*)g_realvr + 0x38041);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static void ReleaseGameplayCamClamps(int frame, const char* reason) {
    // CAM::_CLAMP_GAMEPLAY_CAM_YAW(-180, 180), CAM::_CLAMP_GAMEPLAY_CAM_PITCH(-89, 89)
    NativeVoid2(0x8F993D26E0CA5E8Eull, FloatArg(-180.0f), FloatArg(180.0f));
    NativeVoid2(0xA516C198B7DCA1E1ull, FloatArg(-89.0f), FloatArg(89.0f));

    if (frame <= 8 || (frame % 120) == 0) {
        float heading = NativeFloat0(0x743607648ADD4587ull, 0.0f);
        float pitch = NativeFloat0(0x3A6867B4845BEDA2ull, 0.0f);
        CLog("compat first-person unclamp frame=%d reason=%s relHeading=%.2f relPitch=%.2f",
             frame, reason ? reason : "-", heading, pitch);
    }
}

static void BeginSoftFirstPersonReset(const char* reason) {
    // Briefly leave first person so GTA/RealVR rebuild their first-person camera state,
    // similar to the clean reset observed after hospital respawn.
    NativeVoid1(0x5A4F9EDF1673F704ull, 1); // SET_FOLLOW_PED_CAM_VIEW_MODE(third-person medium)
    uint64_t ctxThird[2] = { 0, 1 };
    InvokeNativeRaw(0x2A2173E46DAECD12ull, ctxThird, 2, nullptr);
    NativeVoid1(0xB4EC2312F4E5B1F1ull, FloatArg(0.0f));
    NativeVoid2(0x8F993D26E0CA5E8Eull, FloatArg(-180.0f), FloatArg(180.0f));
    NativeVoid2(0xA516C198B7DCA1E1ull, FloatArg(-89.0f), FloatArg(89.0f));
    CLog("compat first-person soft reset begin reason=%s third-person-bridge=1",
         reason ? reason : "-");
}

static void FinishSoftFirstPersonReset(const char* reason) {
    NativeVoid1(0x5A4F9EDF1673F704ull, 4); // SET_FOLLOW_PED_CAM_VIEW_MODE(first-person)
    uint64_t ctxFirst[2] = { 0, 4 };
    InvokeNativeRaw(0x2A2173E46DAECD12ull, ctxFirst, 2, nullptr);
    NativeVoid2(0x8F993D26E0CA5E8Eull, FloatArg(-180.0f), FloatArg(180.0f));
    NativeVoid2(0xA516C198B7DCA1E1ull, FloatArg(-89.0f), FloatArg(89.0f));
    CLog("compat first-person soft reset finish reason=%s first-person=1",
         reason ? reason : "-");
}

static void SaveCurrentAppearance(int ped) {
    // Save all component variations (clothes)
    for (int i = 0; i < 12; i++) {
        // GET_PED_DRAWABLE_VARIATION(ped, componentId)
        g_saved_components[i].drawable = NativeInt2(0x67F3780DD425D4FCull, (uint64_t)(uint32_t)ped, (uint64_t)i, 0);
        // GET_PED_TEXTURE_VARIATION(ped, componentId)
        g_saved_components[i].texture  = NativeInt2(0x04A355E041E004E6ull, (uint64_t)(uint32_t)ped, (uint64_t)i, 0);
        // GET_PED_PALETTE_VARIATION(ped, componentId)
        g_saved_components[i].palette  = NativeInt2(0xE3DD5F2A84B42281ull, (uint64_t)(uint32_t)ped, (uint64_t)i, 0);
    }

    // Save all prop variations (hats, glasses, etc.)
    for (int i = 0; i < 8; i++) {
        // GET_PED_PROP_INDEX(ped, componentId)
        g_saved_props[i].drawable = NativeInt2(0x898CC20EA75BACD8ull, (uint64_t)(uint32_t)ped, (uint64_t)i, -1);
        // GET_PED_PROP_TEXTURE_INDEX(ped, componentId)
        g_saved_props[i].texture  = NativeInt2(0xE131A28626F81AB2ull, (uint64_t)(uint32_t)ped, (uint64_t)i, 0);
    }

    CLog("compat: saved appearance for ped 0x%X (12 components + 8 props)", ped);
}

static void RestoreAppearance(int ped) {
    // Restore all component variations (clothes)
    for (int i = 0; i < 12; i++) {
        uint64_t args[5] = {
            (uint64_t)(uint32_t)ped,
            (uint64_t)i,
            (uint64_t)g_saved_components[i].drawable,
            (uint64_t)g_saved_components[i].texture,
            (uint64_t)g_saved_components[i].palette
        };
        InvokeNativeRaw(0x262B14F48D29DE80ull, args, 5, nullptr); // SET_PED_COMPONENT_VARIATION
    }

    // Restore all prop variations (hats, glasses, etc.)
    for (int i = 0; i < 8; i++) {
        if (g_saved_props[i].drawable == -1) {
            NativeVoid2(0x0943E5B8E078E76Eull, (uint64_t)(uint32_t)ped, (uint64_t)i); // CLEAR_PED_PROP
        } else {
            uint64_t args[5] = {
                (uint64_t)(uint32_t)ped,
                (uint64_t)i,
                (uint64_t)g_saved_props[i].drawable,
                (uint64_t)g_saved_props[i].texture,
                1ull // attach = true
            };
            InvokeNativeRaw(0x93376B65A266EB5Full, args, 5, nullptr); // SET_PED_PROP_INDEX
        }
    }

    CLog("compat: restored appearance for ped 0x%X", ped);
}

// Common SP weapon hashes (joaat of the WEAPON_* names). SET_PLAYER_MODEL has
// no "keep current loadout" flag, so the only reliable way to survive a model
// swap with your guns intact is to snapshot HAS_PED_GOT_WEAPON/ammo for this
// known set before the swap and GIVE_WEAPON_TO_PED them back afterward.
static const uint32_t kKnownWeaponHashes[] = {
    0x99B507EAu, // WEAPON_KNIFE
    0x678B81B1u, // WEAPON_NIGHTSTICK
    0x4E875F73u, // WEAPON_HAMMER
    0x958A4A8Fu, // WEAPON_BAT
    0x440E4788u, // WEAPON_GOLFCLUB
    0x84BD7BFDu, // WEAPON_CROWBAR
    0x1B06D571u, // WEAPON_PISTOL
    0xBFE256D4u, // WEAPON_PISTOL_MK2
    0x5EF9FEC4u, // WEAPON_COMBATPISTOL
    0x22D8FE39u, // WEAPON_APPISTOL
    0x3656C8C1u, // WEAPON_STUNGUN
    0x99AEEB3Bu, // WEAPON_PISTOL50
    0xBFD21232u, // WEAPON_SNSPISTOL
    0x88374054u, // WEAPON_SNSPISTOL_MK2
    0xD205520Eu, // WEAPON_HEAVYPISTOL
    0x083839C4u, // WEAPON_VINTAGEPISTOL
    0x47757124u, // WEAPON_FLAREGUN
    0xDC4DB296u, // WEAPON_MARKSMANPISTOL
    0xC1B3C3D1u, // WEAPON_REVOLVER
    0xCB96392Fu, // WEAPON_REVOLVER_MK2
    0x13532244u, // WEAPON_MICROSMG
    0x2BE6766Bu, // WEAPON_SMG
    0x78A97CD0u, // WEAPON_SMG_MK2
    0xEFE7E2DFu, // WEAPON_ASSAULTSMG
    0x0A3D4D34u, // WEAPON_COMBATPDW
    0xDB1AA450u, // WEAPON_MACHINEPISTOL
    0xBD248B55u, // WEAPON_MINISMG
    0x1D073A89u, // WEAPON_PUMPSHOTGUN
    0x555AF99Au, // WEAPON_PUMPSHOTGUN_MK2
    0x7846A318u, // WEAPON_SAWNOFFSHOTGUN
    0xE284C527u, // WEAPON_ASSAULTSHOTGUN
    0x9D61E50Fu, // WEAPON_BULLPUPSHOTGUN
    0xA89CB99Eu, // WEAPON_MUSKET
    0x3AABBBAAu, // WEAPON_HEAVYSHOTGUN
    0xEF951FBBu, // WEAPON_DBSHOTGUN
    0x12E82D3Du, // WEAPON_AUTOSHOTGUN
    0xBFEFFF6Du, // WEAPON_ASSAULTRIFLE
    0x394F415Cu, // WEAPON_ASSAULTRIFLE_MK2
    0x83BF0278u, // WEAPON_CARBINERIFLE
    0xFAD1F1C9u, // WEAPON_CARBINERIFLE_MK2
    0xAF113F99u, // WEAPON_ADVANCEDRIFLE
    0xC0A3098Du, // WEAPON_SPECIALCARBINE
    0x969C3D67u, // WEAPON_SPECIALCARBINE_MK2
    0x7F229F94u, // WEAPON_BULLPUPRIFLE
    0x84D6FAFDu, // WEAPON_BULLPUPRIFLE_MK2
    0x624FE830u, // WEAPON_COMPACTRIFLE
    0x9D07F764u, // WEAPON_MG
    0x7FD62962u, // WEAPON_COMBATMG
    0xDBBD7280u, // WEAPON_COMBATMG_MK2
    0x61012683u, // WEAPON_GUSENBERG
    0x05FC3C11u, // WEAPON_SNIPERRIFLE
    0x0C472FE2u, // WEAPON_HEAVYSNIPER
    0x0A914799u, // WEAPON_HEAVYSNIPER_MK2
    0xC734385Au, // WEAPON_MARKSMANRIFLE
    0x6A6C02E0u, // WEAPON_MARKSMANRIFLE_MK2
    0xB1CA77B1u, // WEAPON_RPG
    0xA284510Bu, // WEAPON_GRENADELAUNCHER
    0x42BF8A85u, // WEAPON_MINIGUN
    0x63AB0442u, // WEAPON_HOMINGLAUNCHER
    0x93E220BDu, // WEAPON_GRENADE
    0x2C3731D9u, // WEAPON_STICKYBOMB
    0xAB564B93u, // WEAPON_PROXMINE
    0xA0973D5Eu, // WEAPON_BZGAS
    0x24B17070u, // WEAPON_MOLOTOV
    0x060EC506u, // WEAPON_FIREEXTINGUISHER
    0x34A67B97u, // WEAPON_PETROLCAN
    0xFDBC8A50u, // WEAPON_SMOKEGRENADE
    0xDD5DF8D9u, // WEAPON_MACHETE
    0xF9DCBF2Du, // WEAPON_HATCHET
    0xBA45E8B8u, // WEAPON_PIPEBOMB
};
static const int kKnownWeaponCount = sizeof(kKnownWeaponHashes) / sizeof(kKnownWeaponHashes[0]);

// All known weapon component hashes (suppressors, scopes, grips, extended
// clips, flashlights, muzzle brakes, camo/finish variants, Mk2 attachments,
// etc), across every weapon in the game. HAS_PED_GOT_WEAPON_COMPONENT is
// checked per (weapon, component) pair below, so a generic "every component
// that exists" list works fine - invalid combinations for a given weapon
// just report false and are skipped.
static const uint32_t kKnownComponentHashes[] = {
    0xFA8FA10Fu, // AdvancedRifleClip01
    0x8EC1C979u, // AdvancedRifleClip02
    0x377CD377u, // AdvancedRifleVarmodLuxe
    0x31C4B22Au, // APPistolClip01
    0x249A17D5u, // APPistolClip02
    0x9B76C72Cu, // APPistolVarmodLuxe
    0xBE5EEA16u, // AssaultRifleClip01
    0xB1214F9Bu, // AssaultRifleClip02
    0xDBF0A53Du, // AssaultRifleClip03
    0x4EAD7533u, // AssaultRifleVarmodLuxe
    0x8D1307B0u, // AssaultSMGClip01
    0xBB46E417u, // AssaultSMGClip02
    0x278C78AFu, // AssaultSMGVarmodLowrider
    0x94E81BC7u, // AssaultShotgunClip01
    0x86BD7F72u, // AssaultShotgunClip02
    0x0C164F53u, // AtArAfGrip
    0x7BC4CDDCu, // AtArFlsh
    0x837445AAu, // AtArSupp
    0xA73D4664u, // AtArSupp02
    0x359B7AAEu, // AtPiFlsh
    0xC304849Au, // AtPiSupp
    0x65EA7EBBu, // AtPiSupp02
    0x75414F30u, // AtRailCover01
    0xD2443DDCu, // AtScopeLarge
    0x1C221B1Au, // AtScopeLargeFixedZoom
    0x9D2FBF29u, // AtScopeMacro
    0x3CC6BA57u, // AtScopeMacro02
    0xBC54DA77u, // AtScopeMax
    0xA0D89C42u, // AtScopeMedium
    0xAA2C45B4u, // AtScopeSmall
    0x3C00AFEDu, // AtScopeSmall02
    0xE608B35Eu, // AtSrSupp
    0xC5A12F80u, // BullpupRifleClip01
    0xB3688B0Fu, // BullpupRifleClip02
    0xA857BC78u, // BullpupRifleVarmodLow
    0xC94E550Eu, // BullpupShotgunClip01
    0x9FBE33ECu, // CarbineRifleClip01
    0x91109691u, // CarbineRifleClip02
    0xBA62E935u, // CarbineRifleClip03
    0xD89B9658u, // CarbineRifleVarmodLuxe
    0xE1FFB34Au, // CombatMGClip01
    0xD6C59CD6u, // CombatMGClip02
    0x92FECCDDu, // CombatMGVarmodLowrider
    0x4317F19Eu, // CombatPDWClip01
    0x334A5203u, // CombatPDWClip02
    0x6EB8C8DBu, // CombatPDWClip03
    0x0721B079u, // CombatPistolClip01
    0xD67B4F2Du, // CombatPistolClip02
    0xC6654D72u, // CombatPistolVarmodLowrider
    0x513F0A63u, // CompactRifleClip01
    0x59FF9BF8u, // CompactRifleClip02
    0xC607740Eu, // CompactRifleClip03
    0x29EA741Eu, // DBShotgunClip01
    0xE4E4C28Du, // FireworkClip01
    0x93E9BD99u, // FlareGunClip01
    0xDDB7390Fu, // FlashlightLight
    0x11AE5C97u, // GrenadeLauncherClip01
    0x1CE5A6A5u, // GusenbergClip01
    0xEAC8C270u, // GusenbergClip02
    0x0D4A969Au, // HeavyPistolClip01
    0x64F9C62Bu, // HeavyPistolClip02
    0x7A6A7B7Bu, // HeavyPistolVarmodLuxe
    0x324F2D5Fu, // HeavyShotgunClip01
    0x971CF6FDu, // HeavyShotgunClip02
    0x88C7DA53u, // HeavyShotgunClip03
    0x476F52F4u, // HeavySniperClip01
    0xF8132D3Fu, // HomingLauncherClip01
    0xEED9FD63u, // KnuckleVarmodBallas
    0xF3462F33u, // KnuckleVarmodBase
    0x9761D9DCu, // KnuckleVarmodDiamond
    0x50910C31u, // KnuckleVarmodDollar
    0x7DECFE30u, // KnuckleVarmodHate
    0xE28BABEFu, // KnuckleVarmodKing
    0x3F4E8AA6u, // KnuckleVarmodLove
    0xC613F685u, // KnuckleVarmodPimp
    0x08B808BBu, // KnuckleVarmodPlayer
    0x7AF3F785u, // KnuckleVarmodVagos
    0xF434EF84u, // MGClip01
    0x82158B47u, // MGClip02
    0xD6DABABEu, // MGVarmodLowrider
    0x476E85FFu, // MachinePistolClip01
    0xB92C6979u, // MachinePistolClip02
    0xA9E9CAF4u, // MachinePistolClip03
    0xCB9E41EDu, // MarksmanPistolClip01
    0xD83B4141u, // MarksmanRifleClip01
    0xCCFD2AC5u, // MarksmanRifleClip02
    0x161E9241u, // MarksmanRifleVarmodLuxe
    0xCB48AEF0u, // MicroSMGClip01
    0x10E6BA2Bu, // MicroSMGClip02
    0x487AAE09u, // MicroSMGVarmodLuxe
    0xC8DE6F06u, // MinigunClip01
    0x4ED2073Fu, // MusketClip01
    0x2297BE19u, // Pistol50Clip01
    0xD9D3AC92u, // Pistol50Clip02
    0x77B8AB2Fu, // Pistol50VarmodLuxe
    0xFED0FD71u, // PistolClip01
    0xED265A1Cu, // PistolClip02
    0xD7391086u, // PistolVarmodLuxe
    0xC5A30FEDu, // PoliceTorchFlashlight
    0xD16F1438u, // PumpShotgunClip01
    0xA2D79DDBu, // PumpShotgunVarmodLowrider
    0x4EA573B3u, // RPGClip01
    0x0384F3E8u, // RailgunClip01
    0xE9867CE3u, // RevolverClip01
    0x16EE3040u, // RevolverVarmodBoss
    0x9493B80Du, // RevolverVarmodGoon
    0x26574997u, // SMGClip01
    0x350966FBu, // SMGClip02
    0x79C77076u, // SMGClip03
    0x27872C90u, // SMGVarmodLuxe
    0xF8802ED9u, // SNSPistolClip01
    0x7B0033B3u, // SNSPistolClip02
    0x8033ECAFu, // SNSPistolVarmodLowrider
    0xC7D62225u, // SawnoffShotgunClip01
    0x85A64DF9u, // SawnoffShotgunVarmodLuxe
    0x9BC64089u, // SniperRifleClip01
    0x4032B5E7u, // SniperRifleVarmodLuxe
    0xC6C7E581u, // SpecialCarbineClip01
    0x7C8BD10Eu, // SpecialCarbineClip02
    0x6B59AEAAu, // SpecialCarbineClip03
    0x730154F2u, // SpecialCarbineVarmodLowrider
    0x9137A500u, // SwitchbladeVarmodBase
    0x5B3E7DB6u, // SwitchbladeVarmodVar1
    0xE7939662u, // SwitchbladeVarmodVar2
    0x45A3B6BBu, // VintagePistolClip01
    0x33BA12E8u, // VintagePistolClip02
    0x420FD713u, // AtSights
    0x3F3C8181u, // AtScopeSmallMk2
    0x049B2945u, // AtScopeMacroMk2
    0xC66B6542u, // AtScopeMediumMk2
    0xB99402D4u, // AtMuzzle1
    0xC867A07Bu, // AtMuzzle2
    0xDE11CBCFu, // AtMuzzle3
    0xEC9068CCu, // AtMuzzle4
    0x02E7957Au, // AtMuzzle5
    0x347EF8ACu, // AtMuzzle6
    0x4DB62ABEu, // AtMuzzle7
    0x9D65907Au, // AtArAfGrip2
    0x94F42D62u, // PistolMk2ClipNormal
    0x5ED6C128u, // PistolMk2ClipExtended
    0x4F37DF2Au, // PistolMk2ClipFMJ
    0x85FEA109u, // PistolMk2ClipHollowpoint
    0x2BBD7A3Au, // PistolMk2ClipIncendiary
    0x25CAAEAFu, // PistolMk2ClipTracer
    0x8ED4BB70u, // PistolMk2Scope
    0x43FD595Bu, // PistolMk2Flash
    0x21E34793u, // PistolMk2Compensator
    0x5C6C749Cu, // PistolMk2CamoDigital
    0x15F7A390u, // PistolMk2CamoBrushstroke
    0x968E24DBu, // PistolMk2CamoWoodland
    0x017BFA99u, // PistolMk2CamoSkull
    0xF2685C72u, // PistolMk2CamoSessanta
    0xDD2231E6u, // PistolMk2CamoPerseus
    0xBB43EE76u, // PistolMk2CamoLeopard
    0x4D901310u, // PistolMk2CamoZebra
    0x5F31B653u, // PistolMk2CamoGeometric
    0x697E19A0u, // PistolMk2CamoBoom
    0x930CB951u, // PistolMk2CamoPatriotic
    0xB4FC92B0u, // PistolMk2CamoSlideDigital
    0x1A1F1260u, // PistolMk2CamoSlideBrushstroke
    0xE4E00B70u, // PistolMk2CamoSlideWoodland
    0x2C298B2Bu, // PistolMk2CamoSlideSkull
    0xDFB79725u, // PistolMk2CamoSlideSessanta
    0x6BD7228Cu, // PistolMk2CamoSlidePerseus
    0x9DDBCF8Cu, // PistolMk2CamoSlideLeopard
    0xB319A52Cu, // PistolMk2CamoSlideZebra
    0xC6836E12u, // PistolMk2CamoSlideGeometric
    0x43B1B173u, // PistolMk2CamoSlideBoom
    0x4ABDA3FAu, // PistolMk2CamoSlidePatriotic
    0x8610343Fu, // AssaultRifleMk2ClipNormal
    0xD12ACA6Fu, // AssaultRifleMk2ClipExtended
    0xA7DD1E58u, // AssaultRifleMk2ClipArmorPiercing
    0x63E0A098u, // AssaultRifleMk2ClipFMJ
    0xFB70D853u, // AssaultRifleMk2ClipIncendiary
    0xEF2C78C1u, // AssaultRifleMk2ClipTracer
    0x43A49D26u, // AssaultRifleMk2BarrelNormal
    0x5646C26Au, // AssaultRifleMk2BarrelHeavy
    0x911B24AFu, // AssaultRifleMk2CamoDigital
    0x37E5444Bu, // AssaultRifleMk2CamoBrushstroke
    0x538B7B97u, // AssaultRifleMk2CamoWoodland
    0x25789F72u, // AssaultRifleMk2CamoSkull
    0xC5495F2Du, // AssaultRifleMk2CamoSessanta
    0xCF8B73B1u, // AssaultRifleMk2CamoPerseus
    0xA9BB2811u, // AssaultRifleMk2CamoLeopard
    0xFC674D54u, // AssaultRifleMk2CamoZebra
    0x7C7FCD9Bu, // AssaultRifleMk2CamoGeometric
    0xA5C38392u, // AssaultRifleMk2CamoBoom
    0xB9B15DB0u, // AssaultRifleMk2CamoPatriotic
    0x4C7A391Eu, // CarbineRifleMk2ClipNormal
    0x5DD5DBD5u, // CarbineRifleMk2ClipExtended
    0x255D5D57u, // CarbineRifleMk2ClipArmorPiercing
    0x44032F11u, // CarbineRifleMk2ClipFMJ
    0x3D25C2A7u, // CarbineRifleMk2ClipIncendiary
    0x1757F566u, // CarbineRifleMk2ClipTracer
    0x833637FFu, // CarbineRifleMk2BarrelNormal
    0x8B3C480Bu, // CarbineRifleMk2BarrelHeavy
    0x4BDD6F16u, // CarbineRifleMk2CamoDigital
    0x406A7908u, // CarbineRifleMk2CamoBrushstroke
    0x2F3856A4u, // CarbineRifleMk2CamoWoodland
    0xE50C424Du, // CarbineRifleMk2CamoSkull
    0xD37D1F2Fu, // CarbineRifleMk2CamoSessanta
    0x86268483u, // CarbineRifleMk2CamoPerseus
    0xF420E076u, // CarbineRifleMk2CamoLeopard
    0xAAE14DF8u, // CarbineRifleMk2CamoZebra
    0x9893A95Du, // CarbineRifleMk2CamoGeometric
    0x6B13CD3Eu, // CarbineRifleMk2CamoBoom
    0xDA55CD3Fu, // CarbineRifleMk2CamoPatriotic
    0x492B257Cu, // CombatMGMk2ClipNormal
    0x17DF42E9u, // CombatMGMk2ClipExtended
    0x29882423u, // CombatMGMk2ClipArmorPiercing
    0x57EF1CC8u, // CombatMGMk2ClipFMJ
    0xC326BDBAu, // CombatMGMk2ClipIncendiary
    0xF6649745u, // CombatMGMk2ClipTracer
    0xC34EF234u, // CombatMGMk2BarrelNormal
    0xB5E2575Bu, // CombatMGMk2BarrelHeavy
    0x4A768CB5u, // CombatMGMk2CamoDigital
    0xCCE06BBDu, // CombatMGMk2CamoBrushstroke
    0xBE94CF26u, // CombatMGMk2CamoWoodland
    0x7609BE11u, // CombatMGMk2CamoSkull
    0x48AF6351u, // CombatMGMk2CamoSessanta
    0x9186750Au, // CombatMGMk2CamoPerseus
    0x84555AA8u, // CombatMGMk2CamoLeopard
    0x1B4C088Bu, // CombatMGMk2CamoZebra
    0x0E046DFCu, // CombatMGMk2CamoGeometric
    0x028B536Eu, // CombatMGMk2CamoBoom
    0xD703C94Du, // CombatMGMk2CamoPatriotic
    0xFA1E1A28u, // HeavySniperMk2ClipNormal
    0x2CD8FF9Du, // HeavySniperMk2ClipExtended
    0xF835D6D4u, // HeavySniperMk2ClipArmorPiercing
    0x89EBDAA7u, // HeavySniperMk2ClipExplosive
    0x3BE948F6u, // HeavySniperMk2ClipFMJ
    0x0EC0F617u, // HeavySniperMk2ClipIncendiary
    0x82C10383u, // HeavySniperMk2ScopeLarge
    0xB68010B0u, // HeavySniperMk2ScopeNightvision
    0x2E43DA41u, // HeavySniperMk2ScopeThermal
    0xAC42DF71u, // HeavySniperMk2Suppressor
    0x5F7DCE4Du, // HeavySniperMk2Muzzle8
    0x6927E1A1u, // HeavySniperMk2Muzzle9
    0x909630B7u, // HeavySniperMk2BarrelNormal
    0x108AB09Eu, // HeavySniperMk2BarrelHeavy
    0xF8337D02u, // HeavySniperMk2CamoDigital
    0xC5BEDD65u, // HeavySniperMk2CamoBrushstroke
    0xE9712475u, // HeavySniperMk2CamoWoodland
    0x13AA78E7u, // HeavySniperMk2CamoSkull
    0x26591E50u, // HeavySniperMk2CamoSessanta
    0x302731ECu, // HeavySniperMk2CamoPerseus
    0xAC722A78u, // HeavySniperMk2CamoLeopard
    0xBEA4CEDDu, // HeavySniperMk2CamoZebra
    0xCD776C82u, // HeavySniperMk2CamoGeometric
    0xABC5ACC7u, // HeavySniperMk2CamoBoom
    0x6C32D2EBu, // HeavySniperMk2CamoPatriotic
    0x4C24806Eu, // SMGMk2ClipNormal
    0xB9835B2Eu, // SMGMk2ClipExtended
    0x0B5A715Fu, // SMGMk2ClipFMJ
    0x3A1BD6FAu, // SMGMk2ClipHollowpoint
    0xD99222E5u, // SMGMk2ClipIncendiary
    0x7FEA36ECu, // SMGMk2ClipTracer
    0x9FDB5652u, // SMGMk2Sights
    0xE502AB6Bu, // SMGMk2ScopeMacro
    0x3DECC7DAu, // SMGMk2ScopeSmall
    0xD9103EE1u, // SMGMk2BarrelNormal
    0xA564D78Bu, // SMGMk2BarrelHeavy
    0xC4979067u, // SMGMk2CamoDigital
    0x3815A945u, // SMGMk2CamoBrushstroke
    0x4B4B4FB0u, // SMGMk2CamoWoodland
    0xEC729200u, // SMGMk2CamoSkull
    0x48F64B22u, // SMGMk2CamoSessanta
    0x35992468u, // SMGMk2CamoPerseus
    0x24B782A5u, // SMGMk2CamoLeopard
    0xA2E67F01u, // SMGMk2CamoZebra
    0x2218FD68u, // SMGMk2CamoGeometric
    0x45C5C3C5u, // SMGMk2CamoBoom
    0x399D558Fu, // SMGMk2CamoPatriotic
};
static const int kKnownComponentCount = sizeof(kKnownComponentHashes) / sizeof(kKnownComponentHashes[0]);


// Snapshot the ped's current loadout (which weapons + how much ammo) before a
// model swap. Must be called BEFORE SET_PLAYER_MODEL - the new ped starts
// unarmed, so this is the last point the old loadout can still be read.
static void SaveCurrentWeapons(int ped) {
    g_saved_weapon_count = 0;
    g_weapons_saved_valid = false;
    
    for (int i = 0; i < kKnownWeaponCount && g_saved_weapon_count < kMaxSavedWeapons; ++i) {
        uint64_t args[3] = { (uint64_t)(uint32_t)ped, (uint64_t)kKnownWeaponHashes[i], 0ull };
        uint64_t out = 0;

        bool ok = InvokeNativeRaw(0x8DECB02F88F428BCull, args, 3, &out); // HAS_PED_GOT_WEAPON
        if (ok && out != 0) {
            int ammo = NativeInt2(0x015A522136D7F951ull, (uint64_t)(uint32_t)ped,
                                   (uint64_t)kKnownWeaponHashes[i], 0); // GET_AMMO_IN_PED_WEAPON
            g_saved_weapons[g_saved_weapon_count].hash = kKnownWeaponHashes[i];
            g_saved_weapons[g_saved_weapon_count].ammo = ammo;

            // Check every known component against this weapon and record
            // whichever ones the ped actually has fitted (suppressor, scope,
            // grip, extended clip, flashlight, etc).
            int compCount = 0;
            for (int c = 0; c < kKnownComponentCount && compCount < kMaxComponentsPerWeapon; ++c) {
                uint64_t compArgs[3] = { (uint64_t)(uint32_t)ped, (uint64_t)kKnownWeaponHashes[i], (uint64_t)kKnownComponentHashes[c] };
                uint64_t compOut = 0;
                bool compOk = InvokeNativeRaw(0xC593212475FAE340ull, compArgs, 3, &compOut); // HAS_PED_GOT_WEAPON_COMPONENT
                if (compOk && compOut != 0) {
                    g_saved_weapons[g_saved_weapon_count].components[compCount] = kKnownComponentHashes[c];
                    compCount++;
                }
            }
            g_saved_weapons[g_saved_weapon_count].componentCount = compCount;

            g_saved_weapon_count++;
        }
    }

    // NEW LOGIC

    // GET_SELECTED_PED_WEAPON(ped) -> currently equipped weapon hash
    uint64_t selArgs[1] = { (uint64_t)(uint32_t)ped };
    uint64_t selOut = 0;
    if (InvokeNativeRaw(0x0A6DB4965674D243ull, selArgs, 1, &selOut)) {
        g_saved_selected_weapon = (uint32_t)selOut;
        CLog("Output of the selected weapon is : 0x%X", g_saved_selected_weapon);
    }
        
    g_weapons_saved_valid = g_saved_weapon_count > 0;
    CLog("compat: saved %d weapon(s) for ped 0x%X, selected=0x%X", g_saved_weapon_count, ped, g_saved_selected_weapon);

}

// Re-give the snapshot taken by SaveCurrentWeapons(). Safe to call even if
// nothing was saved (no-op).
static void RestoreWeapons(int ped) {
    if (!g_weapons_saved_valid || g_saved_weapon_count <= 0) return;
        for (int i = 0; i < g_saved_weapon_count; ++i) {
            uint64_t args[5] = {
                (uint64_t)(uint32_t)ped,
                (uint64_t)g_saved_weapons[i].hash,
                (uint64_t)(uint32_t)g_saved_weapons[i].ammo,
                0ull, // isHidden
                0ull  // equipNow (we set the selected weapon explicitly below)
            };

            InvokeNativeRaw(0xBF0FD6E56C964FCBull, args, 5, nullptr); // GIVE_WEAPON_TO_PED

            // GIVE_WEAPON_TO_PED does not restore attachments - re-give each
            // component we saw on this weapon at snapshot time.
            for (int c = 0; c < g_saved_weapons[i].componentCount; ++c) {
                uint64_t compArgs[3] = {
                    (uint64_t)(uint32_t)ped,
                    (uint64_t)g_saved_weapons[i].hash,
                    (uint64_t)g_saved_weapons[i].components[c]
                };
                InvokeNativeRaw(0xD966D51AA5B28BB9ull, compArgs, 3, nullptr); // GIVE_WEAPON_COMPONENT_TO_PED
            }
    }
        if (g_saved_selected_weapon != 0) {
            NativeVoid3(0xADF692B254977C0Cull, (uint64_t)(uint32_t)ped,
                (uint64_t)g_saved_selected_weapon, 1ull); // SET_CURRENT_PED_WEAPON(equipNow=true)
        }
        
        CLog("compat: restored %d weapon(s) for ped 0x%X, selected=0x%X", g_saved_weapon_count, ped, g_saved_selected_weapon);
        g_weapons_saved_valid = false;
        g_saved_weapon_count = 0; 
}

// Snapshot the player's current wanted level before a model swap. Must be
// called BEFORE SET_PLAYER_MODEL, same timing constraint as SaveCurrentWeapons -
// takes a Player id, not a Ped handle (wanted level lives on the player, not
// the ped, but the fresh ped from the swap still comes back clean).
static void SaveCurrentWantedLevel(int player) {
    g_saved_wanted_level = NativeInt1(0xE28E54788CE8F12Dull, (uint64_t)(uint32_t)player, 0); // PLAYER::GET_PLAYER_WANTED_LEVEL
    g_wanted_level_saved_valid = true;
    CLog("compat: saved wanted level %d for player 0x%X", g_saved_wanted_level, player);
}

// Re-apply the snapshot taken by SaveCurrentWantedLevel(). Safe to call even
// if nothing was saved, or if the saved level was 0 (no-op either way).
static void RestoreWantedLevel(int player) {
    if (!g_wanted_level_saved_valid) return;
    if (g_saved_wanted_level > 0) {
        NativeVoid3(0x39FF19C64EF7DA5Bull, (uint64_t)(uint32_t)player,
                    (uint64_t)(uint32_t)g_saved_wanted_level, 0ull); // PLAYER::SET_PLAYER_WANTED_LEVEL
        NativeVoid2(0xE0A7D1E497FFCD6Full, (uint64_t)(uint32_t)player, 0ull); // PLAYER::SET_PLAYER_WANTED_LEVEL_NOW
    }
    CLog("compat: restored wanted level %d for player 0x%X", g_saved_wanted_level, player);
    g_wanted_level_saved_valid = false;
    g_saved_wanted_level = 0;
}

static void InstantPlayerModelReset(const char* reason) {
    // Instant player model reset WITHOUT freeze: use saved model, request but don't wait for load
    // The model loads in background while gameplay continues naturally
    // This restores HMD tracking while preserving animations and player physics
    __try {
        int playerId = NativeInt0(0x4F8644AF03D0E0D6ull, 0); // PLAYER::PLAYER_ID()
        int ped = NativeInt0(0xD80958FC74E988A6ull, 0); // PLAYER::PLAYER_PED_ID()

        if (playerId >= 0 && ped != 0) {
            // Use the model we captured BEFORE entering the vehicle
            // If not captured, get it now
            uint32_t modelToRestore = g_saved_player_model ? g_saved_player_model : NativeInt1(0x9F47B058362C84B5ull, (uint64_t)(uint32_t)ped, 0);

            if (modelToRestore == 0) {
                CLog("compat instant model reset: ERROR - could not get model");
                return;
            }

            CLog("compat instant model reset: restoring saved model 0x%X reason=%s", modelToRestore, reason ? reason : "-");
          
            // Same for wanted level - the recreated ped comes back with the
            // player's stars cleared unless we snapshot and re-apply them too.
            if (g_modelResetPreserveWantedLevel) {
                SaveCurrentWantedLevel(playerId);
            }

            // REQUEST_MODEL - just request, DON'T WAIT for load
            // KEY: Removing the wait loop is what eliminates the freeze!
            // The model will load in background while player continues falling/jumping/etc
            NativeVoid1(0x963D27A58DF860ACull, (uint64_t)modelToRestore); // STREAMING::REQUEST_MODEL

            // IMMEDIATELY set player model WITHOUT waiting for load
            // GTA V will display the model even if still loading (uses cached/default while loading)
            // NOTE: GTA V automatically preserves velocity/momentum during model change
            NativeVoid2(0x00A1CADD00108836ull, (uint64_t)(uint32_t)playerId, (uint64_t)modelToRestore); // PLAYER::SET_PLAYER_MODEL

            // Get the new ped after model change
            int newPed = NativeInt0(0xD80958FC74E988A6ull, 0); // PLAYER::PLAYER_PED_ID()

            // Mark to restore appearance/weapons once it's safe to do so. What
            // "safe" means depends on why we're here: after a vehicle exit the
            // character is genuinely airborne, so wait for IS_PED_FALLING to
            // clear; after death/arrest the ped is ragdolled (never reads as
            // "falling") and the real hand-off doesn't complete until GTA's own
            // wasted/busted -> hospital/station flow finishes, so wait for that
            // flag to clear instead.
            if (newPed != 0) {
                g_appearance_restore_ped = newPed;
                g_restore_appearance_next_frame = true;
                if (reason && strstr(reason, "death")) {
                    g_restore_trigger_mode = RESTORE_ON_DEATH_CLEAR;
                } else if (reason && strstr(reason, "arrest")) {
                    g_restore_trigger_mode = RESTORE_ON_ARREST_CLEAR;
                } else {
                    g_restore_trigger_mode = RESTORE_ON_LANDING;
                }
            }

            // Release model from script ownership (it will load asynchronously in background)
            NativeVoid1(0xE532F5D78798DAABull, (uint64_t)modelToRestore); // STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED

            // Force first-person camera mode
            NativeVoid1(0x5A4F9EDF1673F704ull, 4); // CAM::SET_FOLLOW_PED_CAM_VIEW_MODE(4)

            CLog("compat instant model reset: completed instantly - model loads in background, animations preserved");
        } else if (g_saved_player_model == 0) {
            CLog("compat instant model reset: ERROR - no saved model available");
        }    
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        CLog("compat instant model reset: exception caught");
    }
}

static void SyncVehicleFirstPersonCamera(const char* reason) {
    // Independent cameras store on-foot and vehicle view modes separately.
    // Keep vehicle/bike contexts in first person when the player was already using FP on foot.
    NativeVoid1(0xAC253D7842768F48ull, 4); // SET_FOLLOW_VEHICLE_CAM_VIEW_MODE
    uint64_t ctxVehicle[2] = { 1, 4 };     // IN_VEHICLE
    uint64_t ctxBike[2] = { 2, 4 };        // ON_BIKE
    InvokeNativeRaw(0x2A2173E46DAECD12ull, ctxVehicle, 2, nullptr);
    InvokeNativeRaw(0x2A2173E46DAECD12ull, ctxBike, 2, nullptr);
    CLog("compat vehicle first-person sync reason=%s vehView=4 contexts=1,2",
         reason ? reason : "-");
}

// Log notification (visual display via help text disabled for now)
static void ShowScreenNotification(const char* text) {
    if (!text) return;
    CLog("NOTIFICATION: %s", text);
    // TODO: Replace with correct native hashes for this GTA5 version
}

// Force camera reset for 60 frames after vehicle exit
static void ForceCameraFixTick(int& forceFrames) {
    if (forceFrames <= 0) return;

    // RESTORE SNAPSHOT: On first frame of vehicle exit, restore the captured continuations
    if (forceFrames == 60 && g_realvr && g_continuations_captured) {
        __try {
            uint8_t* rvrBase = (uint8_t*)g_realvr;

            // Restore HMD tracking flag
            uint8_t** g_RVRData_ptr = (uint8_t**)(rvrBase + 0x38020);
            uint8_t* g_RVRData = *g_RVRData_ptr;
            if (g_RVRData) {
                uint32_t* d840 = (uint32_t*)(g_RVRData + 0x840);
                if (*d840 != 1) {
                    *d840 = 1;
                    CLog("camera fix: restored d840=1");
                }
            }

            // Restore the 6 engine hook continuations to their captured state
            uint64_t* slot_Proj = (uint64_t*)(rvrBase + 0x38030);
            uint64_t* slot_FOV1stCar = (uint64_t*)(rvrBase + 0x38058);
            uint64_t* slot_CamParams = (uint64_t*)(rvrBase + 0x38078);
            uint64_t* slot_FOVUni = (uint64_t*)(rvrBase + 0x38088);
            uint64_t* slot_FOV3rd = (uint64_t*)(rvrBase + 0x380A0);
            uint64_t* slot_ViewInverse = (uint64_t*)(rvrBase + 0x37FD0);

            *slot_Proj = g_saved_Proj;
            *slot_FOV1stCar = g_saved_FOV1stCar;
            *slot_CamParams = g_saved_CamParams;
            *slot_FOVUni = g_saved_FOVUni;
            *slot_FOV3rd = g_saved_FOV3rd;
            *slot_ViewInverse = g_saved_ViewInverse;

            CLog("camera fix: restored all 6 continuations from snapshot");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            // Silently fail if we can't access memory
        }
    }

    // Force first-person mode for on-foot (mode 4) every frame
    NativeVoid1(0x5A4F9EDF1673F704ull, 4); // SET_FOLLOW_PED_CAM_VIEW_MODE(4)

    forceFrames--;
}

static void HoldRealVRFirstPersonFlags(int frame, const char* reason) {
    if (!g_firstPersonFlagHold || !g_realvr) return;

    uint8_t modeFlagBefore = 0, fpFlagBefore = 0;
    uint32_t camKindBefore = 0, modeStateBefore = 0;
    bool okRead = false;
    __try {
        modeFlagBefore = *((uint8_t*)g_realvr + 0x38040);
        fpFlagBefore = *((uint8_t*)g_realvr + 0x38041);
        camKindBefore = *(uint32_t*)((uint8_t*)g_realvr + 0x380D4);
        modeStateBefore = *(uint32_t*)((uint8_t*)g_realvr + 0x384FC);
        okRead = true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        okRead = false;
    }

    WriteU8((uint8_t*)g_realvr + 0x38040, 0);
    WriteU8((uint8_t*)g_realvr + 0x38041, 1);

    if (frame <= 8 || (frame % 120) == 0 || fpFlagBefore == 0 || modeFlagBefore != 0) {
        CLog("compat first-person flag hold frame=%d reason=%s realvr[38040]=%u realvr[38041]=%u rv380d4=%u rv384fc=%u read=%s",
             frame, reason ? reason : "-", modeFlagBefore, fpFlagBefore,
             camKindBefore, modeStateBefore, okRead ? "OK" : "FAIL");
    }
}

static void LogCompatFirstPersonState(const char* reason, int inVeh, int pedView, int vehView) {
    uintptr_t base = CurrentRVRData();
    uint32_t d4f4 = 0, da9c = 0, rv380d4 = 0, rv384fc = 0;
    uint8_t d51d = 0, d821 = 0, d840 = 0, rv38040 = 0, rv38041 = 0;
    bool ok = false;
    __try {
        if (base >= 0x10000 && LooksReadablePtr(base)) {
            d4f4 = *(uint32_t*)(base + 0x4F4);
            d51d = *(uint8_t*)(base + 0x51D);
            d821 = *(uint8_t*)(base + 0x821);
            d840 = *(uint8_t*)(base + 0x840);
            da9c = *(uint32_t*)(base + 0xA9C);
        }
        if (g_realvr) {
            rv38040 = *((uint8_t*)g_realvr + 0x38040);
            rv38041 = *((uint8_t*)g_realvr + 0x38041);
            rv380d4 = *(uint32_t*)((uint8_t*)g_realvr + 0x380D4);
            rv384fc = *(uint32_t*)((uint8_t*)g_realvr + 0x384FC);
        }
        ok = true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }

    CLog("compat fp state reason=%s inVeh=%d pedView=%d vehView=%d d4f4=%u d51d=%u d821=%u d840=%u da9c=%u rv38040=%u rv38041=%u rv380d4=%u rv384fc=%u read=%s",
         reason ? reason : "-", inVeh, pedView, vehView,
         d4f4, d51d, d821, d840, da9c,
         rv38040, rv38041, rv380d4, rv384fc, ok ? "OK" : "FAIL");

    // Log the 6 engine-hook continuation slots so we can detect which one
    // changes between the working (pre-vehicle) and broken (post-vehicle) states.
    // These slots hold GTA5 addresses written by RealVR's engine trampolines.
    // If head tracking breaks after vehicle exit, one of these will be wrong.
    if (g_realvr) {
        struct SlotInfo { const char* name; uint32_t rva; };
        const SlotInfo slots[] = {
            { "FOV1stCar",   0x38058 },
            { "Proj",        0x38030 },
            { "FOV3rd",      0x380A0 },
            { "FOVUni",      0x38088 },
            { "ViewInverse", 0x37FD0 },
            { "CamParams",   0x38078 },
        };
        uint8_t* gta5Base = (uint8_t*)GetModuleHandleA(nullptr);
        uint32_t gta5Size = gta5Base ? ModuleSize((HMODULE)gta5Base) : 0;
        char buf[256] = {};
        int pos = 0;
        for (const SlotInfo& s : slots) {
            uint64_t cont = 0;
            __try { cont = *(uint64_t*)((uint8_t*)g_realvr + s.rva); }
            __except(EXCEPTION_EXECUTE_HANDLER) {}
            bool inGTA5 = cont && gta5Base &&
                          (uintptr_t)cont >= (uintptr_t)gta5Base &&
                          (uintptr_t)cont < (uintptr_t)gta5Base + gta5Size;
            pos += sprintf_s(buf + pos, sizeof(buf) - pos, "%s=0x%llX(%s) ",
                             s.name, (unsigned long long)cont, inGTA5 ? "OK" : "??");
        }
        CLog("engine slots [%s]: %s", reason ? reason : "-", buf);
    }
}

// Read a Vector3 return value from a no-argument native.
// GTA5 Vector3 layout in the return buffer: {float x, u32 pad, float y, u32 pad, float z, u32 pad}
// = 3 x uint64_t where each float is in the lower 32 bits.
static bool NativeVec3_0(uint64_t hash, float out[3]) {
    NativeInitFn init = ReadRealVRSlot<NativeInitFn>(0x232D0);
    NativeCallFn call = ReadRealVRSlot<NativeCallFn>(0x232C8);
    if (!init || !call) return false;
    __try {
        init(hash);
        uint64_t* ret = call();
        if (!ret) return false;
        out[0] = *(float*)&ret[0];
        out[1] = *(float*)&ret[1];
        out[2] = *(float*)&ret[2];
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Read a Vector3 return value from a 1-argument native.
static bool NativeVec3_1(uint64_t hash, uint64_t a, float out[3]) {
    NativeInitFn init = ReadRealVRSlot<NativeInitFn>(0x232D0);
    NativeCallFn call = ReadRealVRSlot<NativeCallFn>(0x232C8);
    NativePush64Fn push = ReadRealVRSlot<NativePush64Fn>(0x232C0);
    if (!init || !call || !push) return false;
    __try {
        init(hash);
        push(a);
        uint64_t* ret = call();
        if (!ret) return false;
        out[0] = *(float*)&ret[0];
        out[1] = *(float*)&ret[1];
        out[2] = *(float*)&ret[2];
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// CREATE_CAM("DEFAULT_SCRIPTED_CAMERA", active) → Cam handle (int)
static int NativeCreateCam(const char* camName, bool active) {
    NativeInitFn init = ReadRealVRSlot<NativeInitFn>(0x232D0);
    NativeCallFn call = ReadRealVRSlot<NativeCallFn>(0x232C8);
    NativePush64Fn push = ReadRealVRSlot<NativePush64Fn>(0x232C0);
    if (!init || !call || !push) return 0;
    __try {
        init(0xC3981DCE61D9E13Full);           // CREATE_CAM
        push((uint64_t)(uintptr_t)camName);
        push(active ? 1ull : 0ull);
        uint64_t* ret = call();
        return ret ? (int)(uint32_t)*ret : 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Mirrors the hospital-respawn camera sequence: create a scripted cam at the exact
// current gameplay cam position/rotation, then activate RENDER_SCRIPT_CAMS.
// When FinishScriptCamReset deactivates it, GTA re-initialises the gameplay camera
// (IS_GAMEPLAY_CAM_RENDERING goes false → true), which triggers RealVR's first-person
// camera re-init and restores HMD head tracking.
static void BeginScriptCamReset(int* outHandle, const char* reason) {
    *outHandle = 0;
    float pos[3] = {0,0,0}, rot[3] = {0,0,0};
    bool posOk = NativeVec3_0(0x14D6F5678D8F1B37ull, pos);   // GET_GAMEPLAY_CAM_COORD
    bool rotOk = NativeVec3_1(0x837765A25378F0BBull, 2, rot); // GET_GAMEPLAY_CAM_ROT(order=2)

    int cam = NativeCreateCam("DEFAULT_SCRIPTED_CAMERA", true);
    if (!cam) {
        CLog("scriptCam reset: CREATE_CAM failed reason=%s", reason ? reason : "-");
        return;
    }

    if (posOk) {
        uint64_t args[4] = { (uint64_t)(uint32_t)cam, FloatArg(pos[0]), FloatArg(pos[1]), FloatArg(pos[2]) };
        InvokeNativeRaw(0x4D41783FB745E42Eull, args, 4, nullptr); // SET_CAM_COORD
    }
    if (rotOk) {
        uint64_t args[5] = { (uint64_t)(uint32_t)cam, FloatArg(rot[0]), FloatArg(rot[1]), FloatArg(rot[2]), 2 };
        InvokeNativeRaw(0x85973643155D0B07ull, args, 5, nullptr); // SET_CAM_ROT
    }

    // Explicitly activate the camera (required before RenderScriptCams)
    NativeVoid1(0x026FB97D0A425F84ull, (uint64_t)(uint32_t)cam); // SET_CAM_ACTIVE(cam, true)
    CLog("scriptCam reset: SET_CAM_ACTIVE cam=%d", cam);

    // Switch to script cam instantly (no ease, no transition time) → gameplay cam goes dark
    uint64_t renderArgs[5] = { 1, 0, 0, 1, 0 };
    InvokeNativeRaw(0x07E5B515DB0636FCull, renderArgs, 5, nullptr); // RENDER_SCRIPT_CAMS(true,false,0,true,false)

    *outHandle = cam;
    CLog("scriptCam reset begin cam=%d pos=%.1f,%.1f,%.1f posOk=%s rotOk=%s reason=%s",
         cam, pos[0], pos[1], pos[2], posOk ? "y" : "n", rotOk ? "y" : "n",
         reason ? reason : "-");
}

static void FinishScriptCamReset(int camHandle, const char* reason) {
    // Deactivate script cam → gameplay camera re-activates → RealVR re-inits first-person
    uint64_t renderArgs[5] = { 0, 0, 0, 1, 0 };
    InvokeNativeRaw(0x07E5B515DB0636FCull, renderArgs, 5, nullptr); // RENDER_SCRIPT_CAMS(false,...)
    if (camHandle) {
        uint64_t destroyArgs[2] = { (uint64_t)(uint32_t)camHandle, 0 };
        InvokeNativeRaw(0x865908C81A2C22E9ull, destroyArgs, 2, nullptr); // DESTROY_CAM
    }

    // Restore d840 flag (HMD tracking state) in g_RVRData
    // d840 gets cleared (0) when entering vehicle, must be restored (1) on exit
    if (g_realvr) {
        __try {
            // Read g_RVRData pointer from RealVR+0x38020
            uint8_t* rvrBase = (uint8_t*)g_realvr;
            uint8_t** g_RVRData_ptr = (uint8_t**)(rvrBase + 0x38020);
            uint8_t* g_RVRData = *g_RVRData_ptr;
            if (g_RVRData) {
                // Write 1 to g_RVRData+0x840 to re-enable HMD tracking
                uint32_t* d840 = (uint32_t*)(g_RVRData + 0x840);
                *d840 = 1;
                CLog("scriptCam reset restored d840=1 @ 0x%llX", (unsigned long long)d840);
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            CLog("scriptCam reset: failed to restore d840");
        }
    }

    CLog("scriptCam reset finish cam=%d reason=%s", camHandle, reason ? reason : "-");
}

static void CompatScriptMain() {
    CLog("compat script main started");

    int wasInVeh = 0;
    int lastPedView = -1;
    int lastVehView = -1;
    int wantedFirstPerson = 0;
    int rearmFrames = 0;
    int rearmFrameNo = 0;
    int guardFrames = 0;
    int guardCooldown = 0;
    int guardPulseNo = 0;
    int holdFrameNo = 0;
    int controlFixFrames = 0;
    int controlFixFrameNo = 0;
    int unclampFrames = 0;
    int unclampFrameNo = 0;
    int resetRearmCooldown = 0;
    int softResetFrames = 0;
    int fpStateLogCooldown = 0;
    int wasDead = 0;
    int wasArrested = 0;
    int respawnGraceFrames = 0;
    int scriptCamFrames = 0;
    int scriptCamHandle = 0;
    int wasCutscenePlaying = 0;
    int cutsceneFixPendingFrames = 0;
    int cutsceneGroundWaitFrames = 0;
    int controlOffFrames = 0;
    const char* softResetReason = "vehicle-exit";
    const char* rearmReason = "vehicle-exit";
    int camFixForceFrames = 0;  // Force camera reset for 60 frames after vehicle exit

    while (InterlockedCompareExchange(&g_running, 1, 1)) {
        __try {
            int ped = NativeInt0(0xD80958FC74E988A6ull, 0); // PLAYER::PLAYER_PED_ID()
            bool inVeh = ped != 0 && NativeBool2(0x997ABD671D25CA0Bull, (uint64_t)(uint32_t)ped, 0, false);
            int pedView = NativeInt0(0x8D4D46230B2C353Aull, -1); // GET_FOLLOW_PED_CAM_VIEW_MODE
            int vehView = NativeInt0(0xA4FF579AC0E3AAAEull, -1); // GET_FOLLOW_VEHICLE_CAM_VIEW_MODE
            int player = NativeInt0(0x4F8644AF03D0E0D6ull, 0); // PLAYER::PLAYER_ID()
            bool dead = NativeBool1(0x424D4687FA1E5652ull, (uint64_t)(uint32_t)player, false); // IS_PLAYER_DEAD
            if (!dead && ped != 0) {
                dead = NativeBool2(0x3317DEDB88C95038ull, (uint64_t)(uint32_t)ped, 1, false); // IS_PED_DEAD_OR_DYING
            }

            // Check if player is being arrested (busted screen showing)
            bool arrested = NativeBool2(0x388A47C51ABDAC8Eull, (uint64_t)(uint32_t)player, 0, false); // IS_PLAYER_BEING_ARRESTED

            // Restore appearance/weapons once it's safe - see RESTORE_ON_* comments
            // above for why the readiness check differs by trigger mode.
            if (g_restore_appearance_next_frame && g_appearance_restore_ped != 0) {
                bool readyToRestore = false;
                if (g_restore_trigger_mode == RESTORE_ON_DEATH_CLEAR) {
                    readyToRestore = !dead;
                } else if (g_restore_trigger_mode == RESTORE_ON_ARREST_CLEAR) {
                    readyToRestore = !arrested;
                } else {
                    bool isFalling = NativeBool1(0xFB92A102F1C4DFA3ull, (uint64_t)(uint32_t)g_appearance_restore_ped, true); // IS_PED_FALLING
                    readyToRestore = !isFalling;
                }

                if (readyToRestore) {
                    // The death/arrest -> respawn gap can span several seconds, during
                    // which GTA's own hand-off may have moved on to a fresh ped handle;
                    // re-resolve it now rather than trust the one captured back when the
                    // reset first armed.
                    int restorePed = g_appearance_restore_ped;
                    int triggerModeForLog = g_restore_trigger_mode;
                    if (g_restore_trigger_mode != RESTORE_ON_LANDING) {
                        int freshPed = NativeInt0(0xD80958FC74E988A6ull, 0); // PLAYER::PLAYER_PED_ID()
                        if (freshPed != 0) restorePed = freshPed;
                    }
                    RestoreAppearance(restorePed);
                    if (g_modelResetPreserveWeapons) {
                        RestoreWeapons(restorePed);
                    }
                    if (g_modelResetPreserveWantedLevel) {
                        RestoreWantedLevel(player);
                    }
                    g_restore_appearance_next_frame = false;
                    g_appearance_restore_ped = 0;
                    g_restore_trigger_mode = RESTORE_ON_LANDING;
                    CLog("compat: appearance/weapons/wanted-level restored for ped 0x%X (trigger=%d)", restorePed, triggerModeForLog);
                }
            }

            if (dead && !wasDead) {
                wantedFirstPerson = 0;
                rearmFrames = 0;
                guardFrames = 0;
                guardCooldown = 0;
                controlFixFrames = 0;
                unclampFrames = 0;
                softResetFrames = 0;
                resetRearmCooldown = 0;
                fpStateLogCooldown = 0;

                // CRITICAL: Clean up any STALE pending restore state (e.g. a
                // vehicle-exit restore that never got to finish) BEFORE arming
                // the death reset below - InstantPlayerModelReset() sets
                // g_appearance_restore_ped/g_restore_appearance_next_frame for
                // ITS OWN restore, and clearing these AFTER the call (as this
                // used to do) wiped out that fresh state immediately, so
                // weapons/appearance were snapshotted but never actually
                // restored after a hospital respawn.
                g_saved_player_model = 0;
                g_appearance_restore_ped = 0;
                g_restore_appearance_next_frame = false;
                g_pending_delayed_model_change = false;
                g_pending_delayed_model_change_frames = 0;
                g_savedRVRStateValid = false;
                g_savedCamMetadataPool = 0;

                // INSTANT MODEL RESET: Change ped immediately when dead to restore camera
                InstantPlayerModelReset("death-detected");
                CLog("compat death detected - instant model reset applied");

                CLog("compat death detected pedView=%d vehView=%d, camera forcing paused, ALL state cleaned", pedView, vehView);
            } else if (!dead && wasDead) {
                wantedFirstPerson = 0;
                respawnGraceFrames = g_respawnGraceFrames;
                CLog("compat respawn detected pedView=%d vehView=%d grace=%d", pedView, vehView, respawnGraceFrames);
            }
            wasDead = dead ? 1 : 0;

            if (arrested && !wasArrested) {
                // Clean up any STALE pending restore state BEFORE arming the
                // arrest reset below - see the death-detected block above for
                // why this ordering matters (this used to run after the call
                // and discarded its own fresh restore flag).
                g_saved_player_model = 0;
                g_appearance_restore_ped = 0;
                g_restore_appearance_next_frame = false;
                g_pending_delayed_model_change = false;
                g_pending_delayed_model_change_frames = 0;
                g_savedRVRStateValid = false;
                g_savedCamMetadataPool = 0;

                // INSTANT MODEL RESET: Change ped immediately when arrested to restore camera
                InstantPlayerModelReset("arrest-detected");
                CLog("compat arrest detected - instant model reset applied");
            }
            wasArrested = arrested ? 1 : 0;

            // Service a deferred vehicle-exit model reset (see VehicleExitModelReset
            // above): fire it the first frame script control returns, abort if the
            // player gets back into a vehicle first, or give up after the configured
            // timeout so we never wait forever on a stuck flag.
            if (g_pendingVehicleExitModelReset) {
                if (inVeh) {
                    g_pendingVehicleExitModelReset = false;
                    CLog("compat deferred vehicle-exit model reset ABORTED (re-entered vehicle)");
                } else {
                    bool controlOn = NativeBool1(0x49C32D60007AFA47ull, (uint64_t)(uint32_t)player, true); // IS_PLAYER_CONTROL_ON
                    if (controlOn) {
                        InstantPlayerModelReset("vehicle-exit-deferred");
                        g_pendingVehicleExitModelReset = false;
                        CLog("compat deferred vehicle-exit model reset FIRED (control restored)");
                    } else if (--g_pendingVehicleExitModelResetFrames <= 0) {
                        g_pendingVehicleExitModelReset = false;
                        CLog("compat deferred vehicle-exit model reset TIMED OUT waiting for player control");
                    }
                }
            }

            // Cutscene-end heading realignment: some cutscenes hand control back
            // with the ped's body heading pointing a different way than the
            // camera - since RealVR feeds HMD yaw into the gameplay cam heading
            // each frame, that mismatch shows up as the character being turned
            // away from (sometimes exactly opposite) the direction you're
            // actually looking. Wait a short settle window after the cutscene
            // ends, then snap the ped's heading to the camera's and reset the
            // relative-heading offset.
            //
            // Many in-mission "cutscenes" are just the mission script disabling
            // player control for a scripted moment and never register with
            // IS_CUTSCENE_PLAYING at all, so we also treat an extended
            // control-off period the same way (ControlLossHeadingFix).
            bool cutscenePlaying = NativeBool0(0xD3C2E180A40F031Eull, false); // IS_CUTSCENE_PLAYING
            bool controlOnNow = player >= 0 && NativeBool1(0x49C32D60007AFA47ull, (uint64_t)(uint32_t)player, true); // IS_PLAYER_CONTROL_ON
            

            if (!cutscenePlaying && wasCutscenePlaying && g_cutsceneHeadingFix) {
                cutsceneFixPendingFrames = g_cutsceneEndSettleFrames > 0 ? g_cutsceneEndSettleFrames : 1;
                cutsceneGroundWaitFrames = 0;
                CLog("compat cutscene end detected, arming heading fix in %d frame(s)", cutsceneFixPendingFrames);
            } else if (cutscenePlaying && cutsceneFixPendingFrames > 0) {
                // Another cutscene started before the settle window elapsed
                // (e.g. back-to-back cutscenes) - abort, we'll re-arm when it ends.
                cutsceneFixPendingFrames = 0;
                cutsceneGroundWaitFrames = 0;
            }
            wasCutscenePlaying = cutscenePlaying ? 1 : 0;

            if (!controlOnNow && !dead && !arrested) {
                if (controlOffFrames < 100000) ++controlOffFrames;
            } else {
                if (g_controlLossHeadingFix && g_cutsceneHeadingFix &&
                    controlOffFrames >= g_controlLossThresholdFrames &&
                    cutsceneFixPendingFrames <= 0 && !cutscenePlaying && !dead && !arrested) {
                    cutsceneFixPendingFrames = g_cutsceneEndSettleFrames > 0 ? g_cutsceneEndSettleFrames : 1;
                    cutsceneGroundWaitFrames = 0;
                    CLog("compat extended control-loss end detected (%d frames off), arming heading fix in %d frame(s)",
                         controlOffFrames, cutsceneFixPendingFrames);
                }
                controlOffFrames = 0;
            }

            if (cutsceneFixPendingFrames > 0 && !cutscenePlaying) {
                // Some cutscenes (e.g. the one that ends with the player falling
                // and having to navigate to the ground) hand control back while
                // the player is still airborne. Applying the heading/camera fix
                // mid-fall fights the landing, so wait for a safe, grounded state
                // first - but cap the wait (CutsceneGroundWaitMaxFrames) so a
                // misread air/ground/ragdoll flag can't stall the fix forever.
                bool isPlayerInTheAir = NativeBool1(0x886E37EC497200B6ull, (uint64_t)(uint32_t)ped, true); // IS_PED_IN_AIR
                bool isPlayerFalling = NativeBool1(0xFB92A102F1C4DFA3ull, (uint64_t)(uint32_t)ped, true); // IS_PED_FALLING
                bool isPlayerRagdoll = NativeBool1(0x47E4E977581C5B55ull, (uint64_t)(uint32_t)ped, true); // IS_PED_RAGDOLL
                bool isPlayerParachuting = NativeBool1(0x7DCE8BDA0F1C1200ull, (uint64_t)(uint32_t)ped, true); // IS_PED_PARACHUTE_FREE_FALLING
                float heightAboveGround = NativeFloat1(0x1DD55701034110E5ull, (uint64_t)(uint32_t)ped, 0.0f); // GET_ENTITY_HEIGHT_ABOVE_GROUND
                bool isGrounded = heightAboveGround < 0.5f;

                bool safeToApply = !isPlayerInTheAir && isGrounded && !isPlayerFalling && !isPlayerRagdoll && !isPlayerParachuting;
                bool groundWaitTimedOut = g_cutsceneGroundWaitMaxFrames > 0 && cutsceneGroundWaitFrames >= g_cutsceneGroundWaitMaxFrames;

                if (!safeToApply && !groundWaitTimedOut) {
                    if (cutsceneGroundWaitFrames == 0) {
                        CLog("compat cutscene-end: waiting for grounded state before fix (inAir=%d falling=%d ragdoll=%d parachute=%d height=%.2f)",
                             isPlayerInTheAir, isPlayerFalling, isPlayerRagdoll, isPlayerParachuting, heightAboveGround);
                    }
                    ++cutsceneGroundWaitFrames;
                } else {
                    if (!safeToApply && groundWaitTimedOut) {
                        CLog("compat cutscene-end: ground-wait timed out after %d frame(s), applying fix anyway", cutsceneGroundWaitFrames);
                    }
                    cutsceneGroundWaitFrames = 0;

                    if (--cutsceneFixPendingFrames == 0 && ped != 0) {
                        // Primary: RealVR's own HMD recenter, identical to the in-game
                        // hotkey. Recalibrates the HMD-to-game-forward offset directly
                        // through RealVR's own mechanism, independent of anything below.
                        if (g_cutsceneRecenterFix) {
                            TriggerRVRRecenter("cutscene-end");
                        }

                        float camRot[3] = {0,0,0};
                        bool rotOk = NativeVec3_1(0x837765A25378F0BBull, 2, camRot); // GET_GAMEPLAY_CAM_ROT(order=2)
                        if (rotOk) {
                            float pedHeadingBefore = NativeFloat1(0xE83D4F9BA2A38914ull, (uint64_t)(uint32_t)ped, 0.0f); // GET_ENTITY_HEADING
                            NativeVoid2(0x8E2530AA8ADA980Eull, (uint64_t)(uint32_t)ped, FloatArg(camRot[2])); // SET_ENTITY_HEADING
                            NativeVoid1(0xB4EC2312F4E5B1F1ull, FloatArg(0.0f)); // SET_GAMEPLAY_CAM_RELATIVE_HEADING(0)
                            ReleaseGameplayCamClamps(0, "cutscene-end");
                            CLog("compat cutscene-end heading fix: ped heading %.2f -> %.2f", pedHeadingBefore, camRot[2]);

                            // Optional model-reset fallback (mirrors VehicleExitModelReset):
                            // forces RealVR's tracking to re-init via SET_PLAYER_MODEL. Off by
                            // default and gated the same way as vehicle-exit - only meaningful
                            // in first-person, only once control is back, and opt-in given the
                            // known SET_PLAYER_MODEL side effects (see VehicleExitModelReset in
                            // Technical.md: it can race mission scripts and needs the weapon/
                            // wanted-level snapshot machinery InstantPlayerModelReset provides).
                            if (g_cutsceneModelReset && wantedFirstPerson) {
                                bool modelResetControlOn = !g_modelResetRequireControl ||
                                    NativeBool1(0x49C32D60007AFA47ull, (uint64_t)(uint32_t)player, true); // IS_PLAYER_CONTROL_ON
                                if (modelResetControlOn) {
                                    // Unlike vehicle-exit, there's no "entry" moment to snapshot
                                    // the model/appearance from here, so g_saved_player_model may
                                    // be stale (left over from an earlier, unrelated vehicle ride)
                                    // or never set. Refresh it from the ped's current state right
                                    // before resetting so we don't restore the wrong outfit.
                                    uint32_t freshModel = NativeInt1(0x9F47B058362C84B5ull, (uint64_t)(uint32_t)ped, 0); // ENTITY::GET_ENTITY_MODEL(ped)
                                    if (freshModel != 0) {
                                        g_saved_player_model = freshModel;
                                        SaveCurrentAppearance(ped);
                                    }
                                    InstantPlayerModelReset("cutscene-end");
                                    CLog("compat cutscene-end: model reset applied");
                                } else {
                                    CLog("compat cutscene-end: model reset SKIPPED (player control off)");
                                }
                            }

                            // CRITICAL: Full camera reinitialisation (3P -> 1P cycle), same
                            // technique as vehicle-exit - forces the engine to re-init its
                            // internal camera state.
                            NativeVoid1(0x5A4F9EDF1673F704ull, 1); // third-person
                            uint64_t ctx3P[2] = { 0, 1 };
                            InvokeNativeRaw(0x2A2173E46DAECD12ull, ctx3P, 2, nullptr);

                            NativeVoid1(0x5A4F9EDF1673F704ull, 4); // first-person
                            uint64_t ctx1P[2] = { 0, 4 };
                            InvokeNativeRaw(0x2A2173E46DAECD12ull, ctx1P, 2, nullptr);

                            CLog("compat cutscene-end: camera reinit (3P->1P cycle)");

                            // Activate 60-frame camera fix (same as vehicle-exit)
                            camFixForceFrames = 60;
                            CLog("compat cutscene-end: camera fix ACTIVATED (60 frames)");

                            if (g_cutsceneSoftReset && wantedFirstPerson) {
                                if (softResetFrames > 0) {
                                    FinishSoftFirstPersonReset(softResetReason);
                                }
                                softResetFrames = g_firstPersonSoftResetFrames;
                                softResetReason = "cutscene-end";
                                BeginSoftFirstPersonReset("cutscene-end");
                            }
                            if (g_cutsceneScriptCamReset && wantedFirstPerson) {
                                if (scriptCamFrames > 0 && scriptCamHandle) {
                                    FinishScriptCamReset(scriptCamHandle, "cutscene-end-replace");
                                    scriptCamHandle = 0; scriptCamFrames = 0;
                                }
                                BeginScriptCamReset(&scriptCamHandle, "cutscene-end");
                                scriptCamFrames = scriptCamHandle ? g_scriptCamResetFrames : 0;
                            }

                            // Full FP rearm / guard / unclamp pipeline, same as vehicle-exit
                            if (wantedFirstPerson && g_firstPersonRearmFrames > 0) {
                                rearmFrames = g_firstPersonRearmFrames;
                                rearmFrameNo = 0;
                                rearmReason = "cutscene-end";

                                guardFrames = g_firstPersonGuardFrames;
                                guardCooldown = g_firstPersonGuardInterval;
                                guardPulseNo = 0;

                                controlFixFrames = g_firstPersonControlFixFrames;
                                controlFixFrameNo = 0;

                                unclampFrames = g_firstPersonUnclampFrames;
                                unclampFrameNo = 0;

                                resetRearmCooldown = 0;

                                CLog("compat cutscene-end: rearm armed frames=%d", rearmFrames);
                                CLog("compat cutscene-end: guard armed frames=%d interval=%d pulse=%d",
                                    guardFrames, g_firstPersonGuardInterval, g_firstPersonGuardPulseFrames);

                                if (controlFixFrames > 0) {
                                    CLog("compat cutscene-end: control fix armed frames=%d", controlFixFrames);
                                }
                                if (unclampFrames > 0) {
                                    CLog("compat cutscene-end: unclamp armed frames=%d resetRearm=%d",
                                        unclampFrames, g_firstPersonResetRearmFrames);
                                }
                            }
                        } else {
                            CLog("compat cutscene-end heading fix: GET_GAMEPLAY_CAM_ROT failed, skipped");
                        }
                    }
                }
            }

            bool cameraGrace = dead || respawnGraceFrames > 0;

            if (!cameraGrace && pedView == 4) {
                wantedFirstPerson = 1;

                // SNAPSHOT: Capture engine hook continuations when in first-person at startup
                // This captures the "good" state before entering any vehicles
                if (!g_continuations_captured && g_realvr) {
                    __try {
                        uint8_t* rvrBase = (uint8_t*)g_realvr;
                        g_saved_Proj = *(uint64_t*)(rvrBase + 0x38030);
                        g_saved_FOV1stCar = *(uint64_t*)(rvrBase + 0x38058);
                        g_saved_CamParams = *(uint64_t*)(rvrBase + 0x38078);
                        g_saved_FOVUni = *(uint64_t*)(rvrBase + 0x38088);
                        g_saved_FOV3rd = *(uint64_t*)(rvrBase + 0x380A0);
                        g_saved_ViewInverse = *(uint64_t*)(rvrBase + 0x37FD0);
                        g_continuations_captured = true;

                        CLog("captured continuations: Proj=0x%llX FOV1stCar=0x%llX CamParams=0x%llX FOVUni=0x%llX FOV3rd=0x%llX ViewInverse=0x%llX",
                             (unsigned long long)g_saved_Proj, (unsigned long long)g_saved_FOV1stCar,
                             (unsigned long long)g_saved_CamParams, (unsigned long long)g_saved_FOVUni,
                             (unsigned long long)g_saved_FOV3rd, (unsigned long long)g_saved_ViewInverse);
                    } __except(EXCEPTION_EXECUTE_HANDLER) {}
                }
            }

            if (!cameraGrace && inVeh && vehView == 4) {
                wantedFirstPerson = 1;
            }

            if (!cameraGrace && !wasInVeh && inVeh && g_keepVehicleFirstPerson && wantedFirstPerson) {
                SyncVehicleFirstPersonCamera("vehicle-entry");
                vehView = NativeInt0(0xA4FF579AC0E3AAAEull, vehView);
            }

            // NEW LOGIC

            // need to check if the player is trying to enter a vehicle
            // we do this check here as the engine automatically assigns the player a weapon as soon as we get in a vehicle.
            // if we call SaveCurrentWeapons later, the player is already in the "GET_IN_VEHICLE" state has been assigned a weapon.  So when you get out of the vehicle you are armed, even if you werent when you entered
            if (!cameraGrace && !inVeh) {
                uint64_t args[1] = { (uint64_t)(uint32_t)ped };
                uint64_t out = 0;

                InvokeNativeRaw(0x814FA8BE5449445D, args, 1, &out); // check GET_VEHICLE_PED_IS_TRYING_TO_ENTER

                int vehicleTryingToEnter = (int)out;

                bool isAboutToEnterVehicle = (vehicleTryingToEnter != 0);

                if (isAboutToEnterVehicle) {
                    int ped = NativeInt0(0xD80958FC74E988A6ull, 0); // PLAYER::PLAYER_PED_ID()
                    if (ped != 0) { // check the player is real first
                        SaveCurrentWeapons(ped); 
                    }
                }
            }


            // END NEW LOGIC

            // CAPTURE PLAYER MODEL AND APPEARANCE BEFORE ENTERING VEHICLE
            // This ensures we use the EXACT model and clothes when exiting, not a generic/parent model
            if (!cameraGrace && !wasInVeh && inVeh) {
                int ped = NativeInt0(0xD80958FC74E988A6ull, 0); // PLAYER::PLAYER_PED_ID()
                if (ped != 0) {
                    g_saved_player_model = NativeInt1(0x9F47B058362C84B5ull, (uint64_t)(uint32_t)ped, 0); // ENTITY::GET_ENTITY_MODEL(ped)
                    if (g_saved_player_model != 0) {
                        CLog("compat vehicle entry: saved player model 0x%X", g_saved_player_model);
                        // Also save the current appearance (clothes, accessories)
                        SaveCurrentAppearance(ped);
                    }
                }
            }

            if (!cameraGrace && wasInVeh && !inVeh) {
                CLog("compat vehicle exit detected pedView=%d vehView=%d wantedFP=%d", pedView, vehView, wantedFirstPerson);
                LogCompatFirstPersonState("vehicle-exit", 0, pedView, vehView);

                // MODEL RESET (opt-in fallback, OFF by default - see VehicleExitModelReset):
                // Recreating the ped via SET_PLAYER_MODEL reliably restores HMD tracking,
                // but it also (a) strips weapons unless we snapshot/restore them and
                // (b) can race a mission script that is simultaneously moving/teleporting
                // the player right after a vehicle drop-off, which is what was dropping
                // players through the map after missions like the stolen-car delivery and
                // the motorcycle repo job. We only ever fire it when the player was
                // actually in first-person (nothing to fix otherwise) and, unless
                // explicitly disabled, only once the player has script control back
                // (i.e. not mid-cutscene/mid-teleport) - deferring rather than skipping
                // if control is currently off.
                if (g_vehicleExitModelReset && wantedFirstPerson) {
                    bool controlOn = !g_modelResetRequireControl ||
                                      NativeBool1(0x49C32D60007AFA47ull, (uint64_t)(uint32_t)player, true); // IS_PLAYER_CONTROL_ON
                    if (controlOn) {
                        InstantPlayerModelReset("vehicle-exit");
                        CLog("compat vehicle exit: model reset applied immediately");
                    } else {
                        g_pendingVehicleExitModelReset = true;
                        g_pendingVehicleExitModelResetFrames = g_modelResetDeferMaxFrames;
                        CLog("compat vehicle exit: model reset DEFERRED (player control off - likely mission cutscene/teleport)");
                    }
                } else if (!g_vehicleExitModelReset) {
                    CLog("compat vehicle exit: model reset SKIPPED (VehicleExitModelReset=0, using scriptCam/soft-reset only)");
                }

                // Activate camera fix for 60 frames (force FP + reset heading/pitch)
                camFixForceFrames = 60;
                ShowScreenNotification("Fix activado");
                CLog("compat vehicle exit: camera fix ACTIVATED (60 frames)");

                // CRITICAL: Reinitialize first-person camera state (like changing character does)
                // Script Hook V bug: vehicle camera state is not properly reset when exiting
                // Replicate character change: cycle through 3P→1P to reinit internal camera state

                // Step 1: Switch to third-person (this resets the internal FP camera state)
                NativeVoid1(0x5A4F9EDF1673F704ull, 1); // SET_FOLLOW_PED_CAM_VIEW_MODE(1) - third person
                uint64_t ctx3P[2] = { 0, 1 };
                InvokeNativeRaw(0x2A2173E46DAECD12ull, ctx3P, 2, nullptr);
                CLog("compat vehicle exit: cycled to third-person (reinit step 1/2)");

                // Step 2: Immediately switch back to first-person (completes reinit)
                NativeVoid1(0x5A4F9EDF1673F704ull, 4); // SET_FOLLOW_PED_CAM_VIEW_MODE(4) - first person
                uint64_t ctx1P[2] = { 0, 4 };
                InvokeNativeRaw(0x2A2173E46DAECD12ull, ctx1P, 2, nullptr);
                CLog("compat vehicle exit: cycled back to first-person (reinit step 2/2)");


                if (wantedFirstPerson && g_scriptCamReset) {
                    // Replicate the hospital-respawn camera re-init: briefly push a scripted
                    // camera (at the same position) so when we pop it GTA re-initialises the
                    // gameplay camera, triggering RealVR's first-person HMD tracking path.
                    if (scriptCamFrames > 0 && scriptCamHandle) {
                        FinishScriptCamReset(scriptCamHandle, "vehicle-exit-replace");
                        scriptCamHandle = 0; scriptCamFrames = 0;
                    }
                    BeginScriptCamReset(&scriptCamHandle, "vehicle-exit");
                    scriptCamFrames = scriptCamHandle ? g_scriptCamResetFrames : 0;
                    CLog("scriptCam reset armed frames=%d handle=%d", scriptCamFrames, scriptCamHandle);
                }
                if (wantedFirstPerson && g_firstPersonSoftReset) {
                    softResetFrames = g_firstPersonSoftResetFrames;
                    softResetReason = "vehicle-exit";
                    BeginSoftFirstPersonReset("vehicle-exit");
                    CLog("compat first-person soft reset armed frames=%d", softResetFrames);
                }
                if (wantedFirstPerson && g_vehicleExitRecenterFix) {
                    TriggerRVRRecenter("vehicle-exit");
                }
                if (wantedFirstPerson && g_firstPersonRearmFrames > 0) {
                    rearmFrames = g_firstPersonRearmFrames;
                    rearmFrameNo = 0;
                    rearmReason = "vehicle-exit";
                    guardFrames = g_firstPersonGuardFrames;
                    guardCooldown = g_firstPersonGuardInterval;
                    guardPulseNo = 0;
                    holdFrameNo = 0;
                    controlFixFrames = g_firstPersonControlFixFrames;
                    controlFixFrameNo = 0;
                    unclampFrames = g_firstPersonUnclampFrames;
                    unclampFrameNo = 0;
                    resetRearmCooldown = 0;
                    CLog("compat first-person rearm armed frames=%d", rearmFrames);
                    CLog("compat first-person guard armed frames=%d interval=%d pulse=%d",
                         guardFrames, g_firstPersonGuardInterval, g_firstPersonGuardPulseFrames);
                    if (controlFixFrames > 0) {
                        CLog("compat first-person control fix armed frames=%d", controlFixFrames);
                    }
                    if (unclampFrames > 0) {
                        CLog("compat first-person unclamp armed frames=%d resetRearm=%d",
                             unclampFrames, g_firstPersonResetRearmFrames);
                    }
                }
            }

            if (!cameraGrace && !inVeh && wantedFirstPerson && pedView == 4 && guardFrames > 0) {
                --guardFrames;
                ++holdFrameNo;
                HoldRealVRFirstPersonFlags(holdFrameNo, "post-vehicle-exit");
                if (guardCooldown > 0) --guardCooldown;
                if (rearmFrames <= 0 && guardCooldown <= 0 && g_firstPersonGuardPulseFrames > 0) {
                    rearmFrames = g_firstPersonGuardPulseFrames;
                    rearmFrameNo = 0;
                    rearmReason = "fp-watchdog";
                    guardCooldown = g_firstPersonGuardInterval;
                    ++guardPulseNo;
                    CLog("compat first-person guard pulse armed no=%d frames=%d remainingGuard=%d",
                         guardPulseNo, rearmFrames, guardFrames);
                }
            } else if (inVeh) {
                guardFrames = 0;
                guardCooldown = 0;
                holdFrameNo = 0;
                controlFixFrames = 0;
                controlFixFrameNo = 0;
                unclampFrames = 0;
                unclampFrameNo = 0;
                resetRearmCooldown = 0;
                softResetFrames = 0;
            }

            if (!cameraGrace && !inVeh && rearmFrames > 0) {
                ++rearmFrameNo;
                ForcePedFirstPersonPulse(rearmFrameNo, rearmReason);
                --rearmFrames;

                int nowPedView = NativeInt0(0x8D4D46230B2C353Aull, -1);
                int expectedFrames = g_firstPersonRearmFrames;
                if (strcmp(rearmReason, "fp-watchdog") == 0) expectedFrames = g_firstPersonGuardPulseFrames;
                else if (strcmp(rearmReason, "fp-state-reset") == 0) expectedFrames = g_firstPersonResetRearmFrames;
                if (rearmFrameNo >= expectedFrames) {
                    rearmFrames = 0;
                    CLog("compat first-person rearm complete reason=%s pedView=%d frame=%d",
                         rearmReason, nowPedView, rearmFrameNo);
                    LogCompatFirstPersonState("rearm-complete", 0, nowPedView, vehView);
                }
            }

            if (!cameraGrace && !inVeh && softResetFrames > 0) {
                --softResetFrames;
                if (softResetFrames == 0) {
                    FinishSoftFirstPersonReset(softResetReason);
                    LogCompatFirstPersonState("soft-reset-finish", 0, pedView, vehView);
                }
            }

            // Force camera fix tick: keep FP + reset heading/pitch for 60 frames after vehicle exit
            if (!cameraGrace && !inVeh && camFixForceFrames > 0) {
                ForceCameraFixTick(camFixForceFrames);
                if (camFixForceFrames == 0) {
                    ShowScreenNotification("Fix completado");
                    CLog("compat vehicle exit: camera fix COMPLETED");
                }
            }

            // Script-camera re-init tick: finish once the hold frames expire.
            // If the player re-enters a vehicle while it's pending, abort cleanly.
            if (scriptCamFrames > 0) {
                if (inVeh) {
                    FinishScriptCamReset(scriptCamHandle, "vehicle-entry-cancel");
                    scriptCamHandle = 0; scriptCamFrames = 0;
                } else {
                    --scriptCamFrames;
                    if (scriptCamFrames == 0) {
                        FinishScriptCamReset(scriptCamHandle, "vehicle-exit");
                        scriptCamHandle = 0;
                        LogCompatFirstPersonState("scriptCam-reset-finish", 0, pedView, vehView);
                    }
                }
            }

            if (!cameraGrace && !inVeh && pedView == 4 && controlFixFrames > 0) {
                ++controlFixFrameNo;
                EnableFirstPersonLookControls(controlFixFrameNo, "post-vehicle-exit");
                --controlFixFrames;
            }

            if (!cameraGrace && !inVeh && pedView == 4 && unclampFrames > 0) {
                ++unclampFrameNo;
                ReleaseGameplayCamClamps(unclampFrameNo, "post-vehicle-exit");
                --unclampFrames;
            }

            if (!cameraGrace && !inVeh && pedView == 4 && guardFrames > 0) {
                if (resetRearmCooldown > 0) --resetRearmCooldown;
                uint8_t d840 = 0, rv38041 = 0;
                if (rearmFrames <= 0 && resetRearmCooldown <= 0 &&
                    g_firstPersonResetRearmFrames > 0 &&
                    ReadCompactFPState(&d840, &rv38041) && d840 == 1 && rv38041 == 0) {
                    rearmFrames = g_firstPersonResetRearmFrames;
                    rearmFrameNo = 0;
                    rearmReason = "fp-state-reset";
                    resetRearmCooldown = 180;
                    CLog("compat first-person state reset detected d840=%u rv38041=%u rearm=%d cooldown=%d",
                         d840, rv38041, rearmFrames, resetRearmCooldown);
                    LogCompatFirstPersonState("fp-state-reset-detected", 0, pedView, vehView);
                }
            }

            if (!cameraGrace && !inVeh && pedView == 4) {
                if (fpStateLogCooldown <= 0) {
                    LogCompatFirstPersonState("foot-fp-periodic", 0, pedView, vehView);
                    fpStateLogCooldown = 300;
                } else {
                    --fpStateLogCooldown;
                }
            } else {
                fpStateLogCooldown = 0;
            }

            if (respawnGraceFrames > 0 && !dead) {
                --respawnGraceFrames;
                if (respawnGraceFrames == 0) {
                    CLog("compat respawn grace complete pedView=%d vehView=%d", pedView, vehView);
                }
            }

            if (pedView != lastPedView || vehView != lastVehView || (int)inVeh != wasInVeh) {
                CLog("compat camera state inVeh=%d pedView=%d vehView=%d wantedFP=%d rearm=%d grace=%d dead=%d",
                     inVeh ? 1 : 0, pedView, vehView, wantedFirstPerson, rearmFrames,
                     respawnGraceFrames, dead ? 1 : 0);
                if (!cameraGrace && (pedView == 4 || (inVeh && vehView == 4))) {
                    LogCompatFirstPersonState("camera-state-change", inVeh ? 1 : 0, pedView, vehView);
                }
                lastPedView = pedView;
                lastVehView = vehView;
            }
            wasInVeh = inVeh ? 1 : 0;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            static volatile LONG s_scriptExceptions = 0;
            LONG n = InterlockedIncrement(&s_scriptExceptions);
            if (n <= 8 || (n % 120) == 0) CLog("compat script exception count=%ld", n);
        }

        CompatScriptWait(0);
    }
}

static LONG WINAPI CompatVectoredExceptionHandler(EXCEPTION_POINTERS* info) {
    if (!info || !info->ExceptionRecord || !info->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const uintptr_t exceptionAddress = (uintptr_t)info->ExceptionRecord->ExceptionAddress;
    if (!g_realvr) return EXCEPTION_CONTINUE_SEARCH;
    if (code != EXCEPTION_ACCESS_VIOLATION &&
        code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW &&
        exceptionAddress != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
#if defined(_M_X64)
    uintptr_t rip = (uintptr_t)info->ContextRecord->Rip;
    uintptr_t rsp = (uintptr_t)info->ContextRecord->Rsp;
    uintptr_t rax = (uintptr_t)info->ContextRecord->Rax;
    uintptr_t rcx = (uintptr_t)info->ContextRecord->Rcx;
    uintptr_t rdx = (uintptr_t)info->ContextRecord->Rdx;
    uintptr_t rdi = (uintptr_t)info->ContextRecord->Rdi;
    uintptr_t r8 = (uintptr_t)info->ContextRecord->R8;
    uintptr_t r9 = (uintptr_t)info->ContextRecord->R9;
#else
    uintptr_t rip = 0;
    uintptr_t rsp = 0;
    uintptr_t rax = 0;
    uintptr_t rcx = 0;
    uintptr_t rdx = 0;
    uintptr_t r8 = 0;
    uintptr_t r9 = 0;
#endif
    uintptr_t realvrBase = (uintptr_t)g_realvr;
    uintptr_t realvrEnd = realvrBase + (g_realvr ? ModuleSize(g_realvr) : 0);
    uintptr_t ripRva = (rip >= realvrBase && rip < realvrEnd) ? (rip - realvrBase) : 0;

    // Known repair cases run unconditionally — they must work every time the
    // exception fires (e.g. once per frame entering first-person), not just once.
#if defined(_M_X64)
    if (code == EXCEPTION_ACCESS_VIOLATION && ripRva == 0x9EFB && rax == 0) {
        void* gdata = PatchRealVRProxySlots(g_realvr, "VEH RealVR+0x9EFB rvrData null");
        if (gdata) {
            info->ContextRecord->Rax = (DWORD64)gdata;
            CLog("VEH repaired RealVR+0x9EFB: rax <- g_RVRData %p; continuing", gdata);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }

    if (code == EXCEPTION_ACCESS_VIOLATION && ripRva == 0x5477 && rax == 0) {
        static volatile LONG s_camMetaNullReports = 0;
        uint64_t slotValue = 0;
        __try {
            slotValue = *(uint64_t*)((uint8_t*)g_realvr + 0x38098);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
        }
        info->ContextRecord->Rip = (DWORD64)((uint8_t*)g_realvr + 0x5687);
        LONG report = InterlockedIncrement(&s_camMetaNullReports);
        if (report <= 12 || (report % 300) == 0) {
            CLog("VEH repaired RealVR+0x5477: camMetadataPool slot=0x%llX; jumping to RealVR+0x5687 success-exit count=%ld",
                 (unsigned long long)slotValue, report);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // When FirstPersonJump=0, RealVR's original first-person camera routine is
    // allowed to run. On newer GTA builds the metadata array can still be stale
    // or shaped differently, so any AV inside this search/update block falls
    // back to the normal success epilogue instead of breaking ScriptHook input.
    if (code == EXCEPTION_ACCESS_VIOLATION && ripRva >= 0x5477 && ripRva <= 0x562A) {
        static volatile LONG s_firstPersonBlockReports = 0;
        uint64_t slotValue = 0;
        __try {
            slotValue = *(uint64_t*)((uint8_t*)g_realvr + 0x38098);
        } __except(EXCEPTION_EXECUTE_HANDLER) {
        }
        info->ContextRecord->Rax = 1;
        info->ContextRecord->Rip = (DWORD64)((uint8_t*)g_realvr + 0x5687);
        LONG report = InterlockedIncrement(&s_firstPersonBlockReports);
        if (report <= 16 || (report % 300) == 0) {
            CLog("VEH repaired RealVR first-person block rva=0x%llX slot=0x%llX; jumping to RealVR+0x5687 success-exit count=%ld",
                 (unsigned long long)ripRva,
                 (unsigned long long)slotValue,
                 report);
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Fallback zone (0x28D0-0x28FF): can crash when the signature resolver
    // calculates an invalid RCX/RDX. Preserve RealVR's own fallback path at
    // 0x28EC (RCX=RDI) instead of forcing RCX/RDI to null; RDI may contain the
    // last viable metadata pool candidate that must be written to +0x38098.
    if (code == EXCEPTION_ACCESS_VIOLATION && ripRva >= 0x28D0 && ripRva < 0x2900) {
        info->ContextRecord->Rip = (DWORD64)((uint8_t*)g_realvr + 0x28EC);
        CLog("VEH repaired RealVR+0x%04llX (camMetadata resolver #1): rcx=0x%llX rdx=0x%llX rdi=0x%llX; jumping to RealVR+0x28EC fallback",
             (unsigned long long)ripRva,
             (unsigned long long)rcx,
             (unsigned long long)rdx,
             (unsigned long long)rdi);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Second/third signature resolver blocks can fault on invalid scan ranges.
    // Skip them as "not found" so RealVR can keep loading and install engine hooks.
    if (code == EXCEPTION_ACCESS_VIOLATION && ripRva >= 0x2940 && ripRva < 0x299A) {
        info->ContextRecord->Rdi = 0;
        info->ContextRecord->Rip = (DWORD64)((uint8_t*)g_realvr + 0x299A);
        CLog("VEH repaired RealVR+0x%04llX (camMetadata resolver #2): rdi <- 0; jumping to RealVR+0x299A",
             (unsigned long long)ripRva);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    if (code == EXCEPTION_ACCESS_VIOLATION && ripRva >= 0x29F0 && ripRva < 0x2A3D) {
        info->ContextRecord->Rip = (DWORD64)((uint8_t*)g_realvr + 0x2A3D);
        CLog("VEH repaired RealVR+0x%04llX (camMetadata resolver #3): jumping to RealVR+0x2A3D",
             (unsigned long long)ripRva);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    // Vehicle camera: null R14 dereferences in the 0x54BA error path.
    // 0x55AD: 49 8B 36       MOV RSI, [R14]      — crashes if R14=null
    // 0x55AF: (leftover 36 + next instr)          — crash if static patch used only 2 NOPs
    // 0x55B0: 41 0F B7 46 08 MOVZX EAX, [R14+8]  — crashes if R14=null
    // Skip any of these by inspecting the instruction at RIP and advancing past it.
    if (code == EXCEPTION_ACCESS_VIOLATION &&
        (ripRva == 0x55AD || ripRva == 0x55AF || ripRva == 0x55B0 || ripRva == 0x55FC || ripRva == 0x5605)) {
        if (ripRva == 0x5605) {
            info->ContextRecord->Rip = (DWORD64)((uint8_t*)g_realvr + 0x5611);
            CLog("VEH repaired RealVR+0x5605: r14-backed loop invalid; jumping to RealVR+0x5611");
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        const uint8_t* instr = (const uint8_t*)rip;
        int skipBytes = 3; // safe default for 3-byte REX+MOV pattern
        __try {
            if ((instr[0] & 0xF0) == 0x40) {
                // Any REX prefix: delegate to helper
                skipBytes = RexInstrLen(instr);
            } else if (instr[0] == 0xFF) {
                uint8_t mod = (instr[1] >> 6) & 3, rm = instr[1] & 7;
                skipBytes = (mod == 1) ? 3 : (mod == 2 || (mod == 0 && rm == 5)) ? 6 : 2;
            } else if (instr[0] == 0x36) {
                // Segment override prefix left over from a partial NOP: skip prefix + next instr
                skipBytes = 1;
                if ((instr[1] & 0xF0) == 0x40) skipBytes += RexInstrLen(instr + 1);
                else skipBytes += 2;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {
        }
        uint8_t b0=0,b1=0,b2=0,b3=0;
        __try { b0=instr[0]; b1=instr[1]; b2=instr[2]; b3=instr[3]; } __except(EXCEPTION_EXECUTE_HANDLER) {}
        info->ContextRecord->Rip = (DWORD64)(rip + skipBytes);
        CLog("VEH repaired RealVR+0x%04llX: instr=%02X %02X %02X %02X skip=%d; continuing",
             (unsigned long long)ripRva, b0, b1, b2, b3, skipBytes);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
#endif

    // Unknown exception: log once only to avoid flooding.
    if (InterlockedCompareExchange(&g_exceptionLogged, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;

    CLog("VEH exception code=0x%08X address=0x%llX rip=0x%llX rsp=0x%llX ripRealVRRva=0x%llX",
         (unsigned)info->ExceptionRecord->ExceptionCode,
         (unsigned long long)exceptionAddress,
         (unsigned long long)rip,
         (unsigned long long)rsp,
         (unsigned long long)ripRva);
    CLog("VEH regs rax=0x%llX rcx=0x%llX rdx=0x%llX r8=0x%llX r9=0x%llX",
         (unsigned long long)rax,
         (unsigned long long)rcx,
         (unsigned long long)rdx,
         (unsigned long long)r8,
         (unsigned long long)r9);
    LogAddressInfo("exception", exceptionAddress);
    LogAddressInfo("rip", rip);
    LogAddressInfo("rax", rax);
    LogAddressInfo("rcx", rcx);
    LogAddressInfo("rdx", rdx);
    LogAddressInfo("r8", r8);
    LogAddressInfo("r9", r9);

    for (int i = 0; i < (int)info->ExceptionRecord->NumberParameters; ++i) {
        CLog("VEH param[%d]=0x%llX", i, (unsigned long long)info->ExceptionRecord->ExceptionInformation[i]);
    }

    LogCriticalRealVRSlots("at first exception");
    LogStackNear(rsp);
    return EXCEPTION_CONTINUE_SEARCH;
}

static bool PatchRealVRVersionLimit(HMODULE realvr, uint32_t versionId) {
    if (!realvr || versionId <= 64) return true;

    uint8_t* limit = (uint8_t*)realvr + 0x1CA7;
    uint8_t before = 0;
    __try {
        before = *limit;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        CLog("version limit patch failed: cannot read RealVR+0x1CA7");
        return false;
    }

    // RealVR does: eax = version - 0x24; cmp eax, 0x1C; jbe supported.
    const uint32_t minAccepted = 0x24;
    if (versionId < minAccepted || versionId - minAccepted > 0xFF) return false;
    uint8_t neededLimit = (uint8_t)(versionId - minAccepted);

    bool ok = before == neededLimit || (before == 0x1C && WriteU8(limit, neededLimit));
    uint8_t after = 0;
    __try {
        after = *limit;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    CLog("version range RealVR+0x1CA7 before=0x%02X after=0x%02X target=0x%02X patch=%s",
         before, after, neededLimit, ok ? "OK" : "FAIL");
    return ok;
}

static bool PatchCamMetadataResolverFallback(HMODULE realvr) {
    if (!realvr) return false;

    uint8_t* branch = (uint8_t*)realvr + 0x28D0;
    uint8_t before[2] = {};
    __try {
        before[0] = branch[0];
        before[1] = branch[1];
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        CLog("cam metadata fallback patch failed: cannot read RealVR+0x28D0");
        return false;
    }

    // Original: je +0x1A (jump if rcx==0) to fallback.
    // FORCE the fallback (EB 1A = JMP always). In vehicle mode, rcx==0 would cause
    // a crash in the natural path (0x28D2+), so the fallback is safer.
    // The fallback will be protected by VEH to prevent dereference crashes.
    // Gated by [patches] ForceFallback — set to 0 to keep the original conditional JE.
    bool ok = true;
    if (g_patchForceFallback) {
        ok = before[0] == 0xEB ||
             (before[0] == 0x74 && before[1] == 0x1A && WriteU8(branch, 0xEB));
    }

    uint8_t after[2] = {};
    __try {
        after[0] = branch[0];
        after[1] = branch[1];
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }

    CLog("cam metadata fallback RealVR+0x28D0 before=%02X %02X after=%02X %02X enabled=%d patch=%s",
         before[0], before[1], after[0], after[1], g_patchForceFallback, ok ? "OK" : "FAIL");

    // The resolver fallback (now always taken) writes RCX to RealVR+0x38098.
    // When entering a vehicle RCX=0 (no foot cam), so it clears the slot we set.
    // NOP the 7-byte write so our cam pool value persists across mode switches.
    // RealVR+0x28F1: 48 89 0D A0 57 03 00 = MOV QWORD PTR [RIP+0x357A0], RCX
    // Gated by [patches] CamPoolWriteNop — set to 0 to let RealVR update the slot itself.
    if (g_patchCamPoolWriteNop) {
        uint8_t* writeInstr = (uint8_t*)realvr + 0x28F1;
        uint8_t wBefore[7] = {};
        bool wOk = false;
        __try {
            for (int i = 0; i < 7; ++i) wBefore[i] = writeInstr[i];
            bool alreadyNop = (wBefore[0] == 0x90);
            bool expected = (wBefore[0]==0x48 && wBefore[1]==0x89 && wBefore[2]==0x0D);
            if (alreadyNop) { wOk = true; }
            else if (expected) {
                DWORD oldProt = 0;
                if (VirtualProtect(writeInstr, 7, PAGE_EXECUTE_READWRITE, &oldProt)) {
                    for (int i = 0; i < 7; ++i) writeInstr[i] = 0x90;
                    VirtualProtect(writeInstr, 7, oldProt, &oldProt);
                    FlushInstructionCache(GetCurrentProcess(), writeInstr, 7);
                    wOk = true;
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        CLog("cam metadata pool write RealVR+0x28F1 before=%02X %02X %02X ... patch=%s",
             wBefore[0], wBefore[1], wBefore[2], wOk ? "OK" : "FAIL");
    } else {
        CLog("cam metadata pool write RealVR+0x28F1 NOT patched (CamPoolWriteNop=0)");
    }

    return ok;
}

static bool PatchCamMetadataResolverVersionOffset(HMODULE realvr) {
    if (!realvr) return false;
    uint8_t* threshold = (uint8_t*)realvr + 0x28B4;
    uint8_t before = 0;
    __try {
        before = *threshold;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        CLog("cam metadata resolver offset patch failed: cannot read RealVR+0x28B4");
        return false;
    }

    bool ok = true;
    if (g_patchCamMetaOldOffset) {
        // RealVR chooses between two RIP-relative offsets:
        // version < 0x3F -> -0x28, version >= 0x3F -> -0x30.
        // GTA5 1.0.3788.0 with ScriptHook id 101 hits the -0x30 path, but the
        // resolved RDX is invalid. Raising the threshold to 0x7F keeps id 101
        // on the -0x28 path without changing the global version id.
        ok = before == 0x7F || (before == 0x3F && WriteU8(threshold, 0x7F));
    }

    uint8_t after = 0;
    __try {
        after = *threshold;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    CLog("cam metadata resolver version threshold RealVR+0x28B4 before=0x%02X after=0x%02X enabled=%d patch=%s",
         before, after, g_patchCamMetaOldOffset, ok ? "OK" : "FAIL");
    return ok;
}

static bool PatchFirstPersonCamFallback(HMODULE realvr) {
    if (!realvr) return false;

    // On GTA V 1.0.3788.0 the camMetadataPool slot (+0x38098) is always null.
    // The instruction at +0x5477 dereferences RAX and faults. The internal
    // error path eventually returns AL=0 at +0x56B3, leaving camera/control
    // state marked as failed. Skip to +0x5687 instead, where RealVR sets AL=1
    // and exits through the normal success epilogue.
    //
    // Displacement: 0x5687 - (0x5477 + 5) = 0x5687 - 0x547C = 0x020B
    uint8_t* branch = (uint8_t*)realvr + 0x5477;
    uint8_t before[5] = {};
    __try {
        for (int i = 0; i < 5; ++i) before[i] = branch[i];
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        CLog("first person cam patch failed: cannot read RealVR+0x5477");
        return false;
    }

    // E9 0B 02 00 00 = JMP NEAR +0x20B  (lands at RealVR+0x5687)
    const uint8_t patch[5] = { 0xE9, 0x0B, 0x02, 0x00, 0x00 };
    bool ok = (before[0] == 0xE9 && before[1] == 0x0B) ||
              WriteBytes(branch, patch, sizeof(patch));

    uint8_t after[5] = {};
    __try {
        for (int i = 0; i < 5; ++i) after[i] = branch[i];
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }

    CLog("first person cam RealVR+0x5477->0x5687 before=%02X %02X %02X %02X %02X "
         "after=%02X %02X %02X %02X %02X patch=%s",
         before[0], before[1], before[2], before[3], before[4],
         after[0],  after[1],  after[2],  after[3],  after[4],
         ok ? "OK" : "FAIL");
    return ok;
}

static int RexInstrLen(const uint8_t* b) {
    // Returns the byte length of a REX-prefixed instruction starting at b[0],
    // where b[0] is any REX byte (0x40-0x4F), b[1] is the opcode.
    // Only handles the subset of opcodes seen in this patch context.
    if (b[1] == 0x8B) {
        // MOV r64, r/m64: REX + 8B + ModRM [+ SIB] [+ disp]
        uint8_t modrm = b[2];
        uint8_t mod = (modrm >> 6) & 3;
        uint8_t rm  = modrm & 7;
        if (mod == 3) return 3;                   // register operand
        if (mod == 0 && rm == 5) return 7;        // disp32
        if (mod == 0 && rm == 4) return 4;        // SIB, no disp
        if (mod == 1 && rm == 4) return 5;        // SIB + disp8
        if (mod == 2 && rm == 4) return 8;        // SIB + disp32
        if (mod == 1) return 4;                   // disp8
        if (mod == 2) return 7;                   // disp32
        return 3;                                 // mod=0, simple [reg]
    }
    if (b[1] == 0x0F && b[2] == 0xB7) {
        // MOVZX r32, r/m16: REX + 0F B7 + ModRM [+ disp]
        uint8_t modrm = b[3];
        uint8_t mod = (modrm >> 6) & 3;
        uint8_t rm  = modrm & 7;
        if (mod == 1) return 5;                   // disp8
        if (mod == 2) return 8;                   // disp32
        if (mod == 0 && rm == 5) return 8;        // disp32
        return 4;                                 // mod=0
    }
    return 3; // conservative fallback: REX + opcode + ModRM
}

static bool PatchVehicleCamNullCall(HMODULE realvr) {
    if (!realvr) return false;

    // After the 0x54BA error path, the code at 0x55AD and 0x55B0 dereferences
    // R14 (the null camera-object result) twice:
    //   0x55AD: 49 8B 36          MOV RSI, [R14]          (3 bytes)
    //   0x55B0: 41 0F B7 46 08   MOVZX EAX, [R14+8]      (5 bytes)
    // NOP both instructions (8 bytes total) so execution continues past them
    // with RSI/EAX left at 0, which the downstream null checks handle.
    uint8_t* instr = (uint8_t*)realvr + 0x55AD;
    uint8_t before[8] = {};
    __try {
        for (int i = 0; i < 8; ++i) before[i] = instr[i];
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        CLog("vehicle cam null-call patch failed: cannot read RealVR+0x55AD");
        return false;
    }

    CLog("vehicle cam null-call RealVR+0x55AD bytes=%02X %02X %02X %02X %02X %02X %02X %02X",
         before[0], before[1], before[2], before[3],
         before[4], before[5], before[6], before[7]);

    if (before[0] == 0x90 && before[3] == 0x90) return true; // already patched

    // Compute byte spans for both instructions.
    // Instruction 1 at 0x55AD: any REX prefix (0x40-0x4F) + opcode + ...
    // Instruction 2 immediately follows.
    int len1 = 0, len2 = 0;
    if ((before[0] & 0xF0) == 0x40) {
        len1 = RexInstrLen(before);
    } else if (before[0] == 0xFF) {
        uint8_t mod = (before[1] >> 6) & 3, rm = before[1] & 7;
        len1 = (mod == 1) ? 3 : (mod == 2 || (mod == 0 && rm == 5)) ? 6 : 2;
    } else {
        len1 = 2;
    }

    const uint8_t* b2 = before + len1;
    if ((b2[0] & 0xF0) == 0x40) {
        len2 = RexInstrLen(b2);
    } else if (b2[0] == 0xFF) {
        uint8_t mod = (b2[1] >> 6) & 3, rm = b2[1] & 7;
        len2 = (mod == 1) ? 3 : (mod == 2 || (mod == 0 && rm == 5)) ? 6 : 2;
    } else {
        len2 = 0; // only NOP first instruction if second is unrecognized
    }

    int totalNop = len1 + len2;
    uint8_t nops[16] = {};
    for (int i = 0; i < totalNop && i < 16; ++i) nops[i] = 0x90;

    bool ok = WriteBytes(instr, nops, (size_t)totalNop);

    uint8_t after[8] = {};
    __try { for (int i = 0; i < 8; ++i) after[i] = instr[i]; } __except(EXCEPTION_EXECUTE_HANDLER) {}
    CLog("vehicle cam null-call RealVR+0x55AD len1=%d len2=%d nopTotal=%d "
         "after=%02X %02X %02X %02X %02X %02X %02X %02X patch=%s",
         len1, len2, totalNop,
         after[0], after[1], after[2], after[3],
         after[4], after[5], after[6], after[7],
         ok ? "OK" : "FAIL");
    return ok;
}

struct ModuleRange {
    uint8_t* base = nullptr;
    size_t size = 0;
};

static ModuleRange MainModule() {
    HMODULE h = GetModuleHandleA(nullptr);
    return { (uint8_t*)h, ModuleSize(h) };
}

using RealVRPatchFn = bool(__fastcall*)(void* patchStruct, void* continuationSlot, void* stub);

static void LogPatchStruct(HMODULE realvr, const char* name, uint32_t rva) {
    if (!realvr) return;
    uint8_t* p = (uint8_t*)realvr + rva;
    // Log 128 bytes in 16-byte rows so we can inspect the struct layout.
    CLog("patchStruct %-12s RealVR+0x%X:", name, rva);
    for (int row = 0; row < 8; ++row) {
        uint8_t buf[16] = {};
        bool ok = true;
        __try {
            for (int i = 0; i < 16; ++i) buf[i] = p[row * 16 + i];
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        if (!ok) { CLog("  +0x%02X: <read error>", row * 16); break; }
        CLog("  +0x%02X: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
             row * 16,
             buf[0],buf[1],buf[2],buf[3],buf[4],buf[5],buf[6],buf[7],
             buf[8],buf[9],buf[10],buf[11],buf[12],buf[13],buf[14],buf[15]);
    }
    // Also log the pointer-sized fields at offsets 0, 8, 16 — likely addresses stored in the struct.
    for (int f = 0; f < 6; ++f) {
        uint64_t val = 0;
        __try { val = *(uint64_t*)(p + f * 8); } __except(EXCEPTION_EXECUTE_HANDLER) {}
        CLog("  field[%d] @+0x%02X = 0x%llX  (RVA vs GTA5.exe base: 0x%llX vs module range check needed)",
             f, f * 8, (unsigned long long)val, (unsigned long long)val);
    }
}

static bool CallOriginalPatchFn(HMODULE realvr, const char* name,
                                uint32_t patchStructRva, uint32_t continuationRva,
                                uint32_t stubRva) {
    if (!realvr) return false;
    auto fn = (RealVRPatchFn)((uint8_t*)realvr + 0x1B30);
    uint8_t* patchStruct = (uint8_t*)realvr + patchStructRva;
    uint8_t* continuation = (uint8_t*)realvr + continuationRva;
    uint8_t* stub = (uint8_t*)realvr + stubRva;

    // Log struct contents and slot state before patching.
    LogPatchStruct(realvr, name, patchStructRva);

    uint64_t contBefore = 0;
    __try { contBefore = *(uint64_t*)continuation; } __except(EXCEPTION_EXECUTE_HANDLER) {}

    // Log the first 32 bytes of the stub to know what code gets installed.
    uint8_t stubBytes[32] = {};
    bool stubOk = true;
    __try { for (int i = 0; i < 32; ++i) stubBytes[i] = stub[i]; } __except(EXCEPTION_EXECUTE_HANDLER) { stubOk = false; }
    if (stubOk) {
        CLog("stub %-12s RealVR+0x%X: "
             "%02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X ...",
             name, stubRva,
             stubBytes[0],stubBytes[1],stubBytes[2],stubBytes[3],
             stubBytes[4],stubBytes[5],stubBytes[6],stubBytes[7],
             stubBytes[8],stubBytes[9],stubBytes[10],stubBytes[11],
             stubBytes[12],stubBytes[13],stubBytes[14],stubBytes[15]);
    }

    CLog("patch %-12s calling RealVR+0x1B30 struct=+0x%X cont=+0x%X stub=+0x%X contBefore=0x%llX",
         name, patchStructRva, continuationRva, stubRva, (unsigned long long)contBefore);

    bool ok = false;
    __try {
        ok = fn(patchStruct, continuation, stub);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        CLog("patch %-12s EXCEPTION in RealVR+0x1B30", name);
        return false;
    }

    uint64_t contAfter = 0;
    __try { contAfter = *(uint64_t*)continuation; } __except(EXCEPTION_EXECUTE_HANDLER) {}

    // If contAfter changed, the patcher wrote the original site address there.
    // That value tells us what GTA5.exe address was actually patched.
    ModuleRange main = MainModule();
    bool contInMain = contAfter && (uintptr_t)contAfter >= (uintptr_t)main.base &&
                      (uintptr_t)contAfter < (uintptr_t)main.base + main.size;
    CLog("patch %-12s result=%s contBefore=0x%llX contAfter=0x%llX contInGTA5=%s contRVA=0x%llX",
         name, ok ? "OK" : "FAIL",
         (unsigned long long)contBefore,
         (unsigned long long)contAfter,
         contInMain ? "YES" : "NO",
         contInMain ? (unsigned long long)(contAfter - (uintptr_t)main.base) : 0ull);

    return ok;
}

static void ApplyOriginalEnginePatches(HMODULE realvr) {
    struct PatchCall {
        const char* name;
        uint32_t patchStructRva;
        uint32_t continuationRva;
        uint32_t stubRva;
    };

    // Six calls from RealVR.asi's own patch routine at RVA 0x1CC0..0x1E1C.
    const PatchCall calls[] = {
        { "FOV1stCar",   0x35B18, 0x38058, 0x0AD10 },
        { "Proj",        0x35C80, 0x38030, 0x0AD45 },
        { "FOV3rd",      0x35AB8, 0x380A0, 0x0ADA4 },
        { "FOVUni",      0x35BB0, 0x38088, 0x0AE0F },
        { "ViewInverse", 0x35A88, 0x37FD0, 0x0AE28 },
        { "CamParams",   0x35A58, 0x38078, 0x0AE69 },
    };

    CLog("active patch pass: logging patcher fn RealVR+0x1B30 bytes:");
    uint8_t fnBytes[32] = {};
    __try { for (int i = 0; i < 32; ++i) fnBytes[i] = ((uint8_t*)realvr + 0x1B30)[i]; } __except(EXCEPTION_EXECUTE_HANDLER) {}
    CLog("  patchFn: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X ...",
         fnBytes[0],fnBytes[1],fnBytes[2],fnBytes[3],fnBytes[4],fnBytes[5],fnBytes[6],fnBytes[7],
         fnBytes[8],fnBytes[9],fnBytes[10],fnBytes[11],fnBytes[12],fnBytes[13],fnBytes[14],fnBytes[15]);

    int okCount = 0;
    for (const PatchCall& c : calls) {
        bool r = CallOriginalPatchFn(realvr, c.name, c.patchStructRva, c.continuationRva, c.stubRva);
        if (r) ++okCount;
        CLog("patch %-12s => %s (%d/%d so far)", c.name, r ? "OK" : "FAIL",
             okCount, (int)(sizeof(calls)/sizeof(calls[0])));
    }
    CLog("active original patch pass: %d/%d OK", okCount, (int)(sizeof(calls) / sizeof(calls[0])));
}

static int CountOriginalEnginePatchesInstalled(HMODULE realvr, const char* label) {
    if (!realvr) return 0;

    struct SlotCheck {
        const char* name;
        uint32_t continuationRva;
    };

    const SlotCheck slots[] = {
        { "FOV1stCar",   0x38058 },
        { "Proj",        0x38030 },
        { "FOV3rd",      0x380A0 },
        { "FOVUni",      0x38088 },
        { "ViewInverse", 0x37FD0 },
        { "CamParams",   0x38078 },
    };

    ModuleRange main = MainModule();
    int count = 0;
    for (const SlotCheck& s : slots) {
        uint64_t cont = 0;
        __try { cont = *(uint64_t*)((uint8_t*)realvr + s.continuationRva); }
        __except(EXCEPTION_EXECUTE_HANDLER) {}

        bool inMain = cont && (uintptr_t)cont >= (uintptr_t)main.base &&
                      (uintptr_t)cont < (uintptr_t)main.base + main.size;
        if (inMain) ++count;

        if (label) {
            CLog("engine slot %-12s [%s] cont=0x%llX inGTA5=%s rva=0x%llX",
                 s.name, label, (unsigned long long)cont, inMain ? "YES" : "NO",
                 inMain ? (unsigned long long)(cont - (uintptr_t)main.base) : 0ull);
        }
    }
    if (label) CLog("engine slots [%s]: %d/6 installed", label, count);
    return count;
}

static DWORD WINAPI EnginePatchThread(void*) {
    if (!g_patchActiveEngine) {
        CLog("active original patch pass disabled (ActiveEnginePatches=0)");
        return 0;
    }

    if (InterlockedCompareExchange(&g_enginePatchesApplied, 0, 0) != 0) {
        CLog("delayed engine patch pass skipped: already applied");
        return 0;
    }

    CLog("delayed engine patch pass armed: waiting %d sec before applying RealVR's 6 engine hooks",
         g_enginePatchDelaySec);

    for (int i = 0; i < g_enginePatchDelaySec && InterlockedCompareExchange(&g_running, 1, 1); ++i) {
        Sleep(1000);
        HMODULE realvr = g_realvr;
        if (realvr && CountOriginalEnginePatchesInstalled(realvr, nullptr) == 6) {
            InterlockedExchange(&g_enginePatchesApplied, 1);
            CLog("delayed engine patch pass skipped: RealVR original already installed all 6 hooks at +%ds",
                 i + 1);
            CountOriginalEnginePatchesInstalled(realvr, "original detected");
            return 0;
        }
    }

    if (!InterlockedCompareExchange(&g_running, 1, 1)) {
        CLog("delayed engine patch pass canceled: process detaching");
        return 0;
    }

    HMODULE realvr = g_realvr;
    if (!realvr) {
        CLog("delayed engine patch pass skipped: RealVR.asi missing");
        return 0;
    }

    int installed = CountOriginalEnginePatchesInstalled(realvr, "before compat fallback");
    if (installed == 6) {
        InterlockedExchange(&g_enginePatchesApplied, 1);
        CLog("delayed engine patch pass skipped: RealVR original already installed all 6 hooks");
        return 0;
    }

    if (InterlockedCompareExchange(&g_enginePatchesApplied, 1, 0) != 0) {
        CLog("delayed engine patch pass skipped: already applied by another path");
        return 0;
    }

    CLog("delayed engine patch pass: applying RealVR's 6 engine hooks now");
    ApplyOriginalEnginePatches(realvr);
    CLog("delayed engine patch pass complete");
    return 0;
}

static int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static int ParsePattern(const char* pat, uint8_t* bytes, bool* mask, int maxLen) {
    int len = 0;
    const char* p = pat;
    while (*p && len < maxLen) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        if (p[0] == '?' && p[1] == '?') {
            bytes[len] = 0;
            mask[len] = false;
            p += 2;
            ++len;
            continue;
        }
        int hi = HexVal(p[0]);
        int lo = HexVal(p[1]);
        if (hi < 0 || lo < 0) break;
        bytes[len] = (uint8_t)((hi << 4) | lo);
        mask[len] = true;
        p += 2;
        ++len;
    }
    return len;
}

static uintptr_t Scan(const ModuleRange& mod, const char* pat) {
    uint8_t bytes[128] = {};
    bool mask[128] = {};
    int len = ParsePattern(pat, bytes, mask, 128);
    if (!mod.base || !mod.size || len <= 0 || (size_t)len > mod.size) return 0;

    for (size_t i = 0; i <= mod.size - (size_t)len; ++i) {
        bool ok = true;
        for (int j = 0; j < len; ++j) {
            if (mask[j] && mod.base[i + j] != bytes[j]) {
                ok = false;
                break;
            }
        }
        if (ok) return (uintptr_t)(mod.base + i);
    }
    return 0;
}

static uintptr_t ResolveRIP(uintptr_t addr, int dispOffset, int instrLen) {
    int32_t disp = *(int32_t*)(addr + dispOffset);
    return addr + instrLen + (intptr_t)disp;
}

// Validate a candidate camera object: reads 4 floats at +0x150 and checks if they
// look like a rotation matrix row (at least one value near ±1, none > 2).
static bool ValidateCamCandidate(uintptr_t candidate, float outF[4]) {
    bool readOk = false;
    __try {
        outF[0] = *(float*)(candidate + 0x150);
        outF[1] = *(float*)(candidate + 0x154);
        outF[2] = *(float*)(candidate + 0x158);
        outF[3] = *(float*)(candidate + 0x15C);
        readOk = true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    if (!readOk) return false;
    for (int fi = 0; fi < 4; ++fi) {
        float v = outF[fi]; if (v < 0) v = -v;
        if (v > 0.5f && v < 2.0f) return true;
    }
    return false;
}

static bool ValidateRealVRPoolShape(uintptr_t candidate) {
    uintptr_t begin = 0;
    uint32_t count = 0;
    __try {
        begin = *(uintptr_t*)candidate;
        count = *(uint32_t*)(candidate + 8);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    if (!LooksReadablePtr(begin)) return false;
    if (count < 1 || count > 512) return false;

    uintptr_t first = 0;
    __try {
        first = *(uintptr_t*)begin;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return LooksReadablePtr(first);
}

// Try to find a camera object from a known GTA5 global slot address.
// Tests the slot value and up to 3 levels of pointer indirection.
// If a valid object is found, writes it to RealVR+0x38098 and returns it.
static void* TryCamPoolSlot(HMODULE realvr, uintptr_t slotAddr, const char* tag) {
    if (!slotAddr || !realvr) return nullptr;
    uintptr_t slotVal = 0;
    __try { slotVal = *(uintptr_t*)slotAddr; } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
    if (slotVal < 0x10000) return nullptr;

    CLog("camPool[%s]: manager=0x%llX", tag, (unsigned long long)slotVal);

    // The pool manager at slotVal holds pointer fields that point to camera objects.
    // Each camera object (heap-allocated) should have a 4x4 rotation matrix at +0x150.
    // Strategy: scan all 8-byte-aligned fields of the pool manager (0..0x300),
    // dereference each field as a pointer, then check +0x150 for valid rotation floats.
    // Also try one extra level of indirection on each candidate pointer.
    for (int level = 0; level <= 1; ++level) {
        uintptr_t base = slotVal;
        if (level == 1) {
            // One extra dereference of slotVal itself
            __try { base = *(uintptr_t*)slotVal; }
            __except(EXCEPTION_EXECUTE_HANDLER) { break; }
            if (base < 0x10000) break;
            CLog("camPool[%s]: level1 base=0x%llX", tag, (unsigned long long)base);
        }

        for (int off = 0; off <= 0x300; off += 8) {
            uintptr_t fieldAddr = base + off;
            uintptr_t candidate = 0;
            __try { candidate = *(uintptr_t*)fieldAddr; }
            __except(EXCEPTION_EXECUTE_HANDLER) { break; } // unreadable page, stop scanning this base
            if (candidate < 0x10000 || candidate == base) continue;

            float f[4] = {};
            if (!ValidateCamCandidate(candidate, f)) continue;

            CLog("camPool[%s]: FOUND level=%d field+0x%X -> 0x%llX [+0x150]=%+.3f %+.3f %+.3f %+.3f",
                 tag, level, off, (unsigned long long)candidate, f[0],f[1],f[2],f[3]);

            if (!ValidateRealVRPoolShape(candidate)) {
                uintptr_t begin = 0;
                uint32_t count = 0;
                __try {
                    begin = *(uintptr_t*)candidate;
                    count = *(uint32_t*)(candidate + 8);
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                }
                CLog("camPool[%s]: REJECT field+0x%X candidate=0x%llX invalid pool shape begin=0x%llX count=%u",
                     tag, off, (unsigned long long)candidate,
                     (unsigned long long)begin, count);
                continue;
            }

            void** poolSlot = (void**)((uint8_t*)realvr + 0x38098);
            __try { *poolSlot = (void*)candidate; } __except(EXCEPTION_EXECUTE_HANDLER) { continue; }
            CLog("camPool[%s]: wrote 0x%llX -> RealVR+0x38098", tag, (unsigned long long)candidate);
            return (void*)candidate;
        }
    }

    CLog("camPool[%s]: no valid cam object found in manager", tag);
    return nullptr;
}

// Phase 1 (startup): scan GTA5 near CamPoolHint for RIP-relative loads and save the slot
// address into g_camPoolSlotAddr.  Does NOT fail if the slot value is currently null.
// Phase 2 (background): TryCamPoolSlot() is called from SlotWatchThread to retry.
static void ResolveCamMetadataPool(HMODULE realvr) {
    ModuleRange mod = MainModule();
    if (!mod.base) return;

    uintptr_t hint = Scan(mod, "88 50 41 48 8B 47 40");
    if (!hint) { CLog("camPool: CamPoolHint pattern not found"); return; }
    CLog("camPool: CamPoolHint at GTA5+0x%llX", (unsigned long long)(hint - (uintptr_t)mod.base));

    // Dump 80 bytes before hint for diagnostics.
    const uint8_t* p = (const uint8_t*)(hint - 80);
    CLog("camPool: context bytes:");
    for (int row = 0; row < 6; ++row) {
        uint8_t b[16] = {};
        bool ok = true;
        __try { for (int i = 0; i < 16; ++i) b[i] = p[row*16+i]; }
        __except(EXCEPTION_EXECUTE_HANDLER) { ok = false; }
        if (!ok) break;
        CLog("  GTA5+0x%llX: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X",
             (unsigned long long)((uintptr_t)(p + row*16) - (uintptr_t)mod.base),
             b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],
             b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
    }

    // Scan backwards up to 256 bytes for ALL RIP-relative 64-bit loads near the hint.
    // Save the FIRST one found as g_camPoolSlotAddr for later retries.
    const uint8_t* scan = (const uint8_t*)(hint - 1);
    const uint8_t* scanEnd = (const uint8_t*)(hint > 256 ? hint - 256 : (uintptr_t)mod.base);
    int nFound = 0;
    while (scan > scanEnd && nFound < 12) {
        __try {
            uint8_t b0 = scan[0], b1 = scan[1], b2 = scan[2];
            if ((b0 == 0x48 || b0 == 0x4C) && b1 == 0x8B && (b2 & 0xC7) == 0x05) {
                int32_t disp = *(int32_t*)(scan + 3);
                uintptr_t slotAddr = (uintptr_t)(scan + 7) + disp;
                uintptr_t slotVal = 0;
                __try { slotVal = *(uintptr_t*)slotAddr; } __except(EXCEPTION_EXECUTE_HANDLER) {}
                CLog("camPool: RIP-load GTA5+0x%llX -> slotAddr=0x%llX curVal=0x%llX",
                     (unsigned long long)((uintptr_t)scan - (uintptr_t)mod.base),
                     (unsigned long long)slotAddr, (unsigned long long)slotVal);
                if (g_camPoolSlotAddr == 0) g_camPoolSlotAddr = slotAddr;  // save first hit
                bool alreadySaved = false;
                for (int si = 0; si < g_camPoolSlotCount; ++si) {
                    if (g_camPoolSlotAddrs[si] == slotAddr) { alreadySaved = true; break; }
                }
                if (!alreadySaved && g_camPoolSlotCount < (int)(sizeof(g_camPoolSlotAddrs) / sizeof(g_camPoolSlotAddrs[0]))) {
                    g_camPoolSlotAddrs[g_camPoolSlotCount++] = slotAddr;
                }
                // Try immediately if value is already non-null.
                if (slotVal > 0x10000) {
                    char tag[32]; sprintf_s(tag, "init+0x%llX", (unsigned long long)((uintptr_t)scan - (uintptr_t)mod.base));
                    if (TryCamPoolSlot(realvr, slotAddr, tag)) return;
                }
                ++nFound;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        --scan;
    }

    if (g_camPoolSlotAddr)
        CLog("camPool: slot saved for retry: 0x%llX (will retry in background)", (unsigned long long)g_camPoolSlotAddr);
    else
        CLog("camPool: no RIP-relative load found near CamPoolHint");
}

// ---- Deep-fix probe -------------------------------------------------------
// Reads GTA5's camera manager and searches for the structure RealVR's first-
// person loop at +0x5477 consumes:
//   pool object:  [+0]=begin ptr to array of camera ptrs, [+8]=count (u32)
//   each entry E in array: camera C = [E]; sub = [C+8]; id = [sub+8] (u32)
// We scan the manager's fields for (ptr,count) pairs that satisfy this and log
// them in full detail so we can wire the right one into +0x38098.

static bool ProbeReadPtr(uintptr_t addr, uintptr_t* out) {
    __try { *out = *(uintptr_t*)addr; return true; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool ProbeReadU32(uintptr_t addr, uint32_t* out) {
    __try { *out = *(uint32_t*)addr; return true; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool LooksHeap(uintptr_t p, uint8_t* modBase, size_t modSize) {
    if (p < 0x10000) return false;
    if (modBase && p >= (uintptr_t)modBase && p < (uintptr_t)modBase + modSize) return false; // static
    if (p >= 0x7FF000000000ull) return false;  // DLL/module space (0x7FFx...)
    return true;
}

static bool LooksReadablePtr(uintptr_t p) {
    if (p < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery((void*)p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
    if (mbi.State != MEM_COMMIT) return false;
    DWORD prot = mbi.Protect & 0xFF;
    if (prot == PAGE_NOACCESS || prot == PAGE_EXECUTE) return false;
    if (mbi.Protect & PAGE_GUARD) return false;
    return true;
}

static int ProbePoolBase(uintptr_t base, const char* label) {
    if (!LooksReadablePtr(base)) return 0;
    int candidates = 0;
    for (int off = 0; off <= 0x600; off += 8) {
        uintptr_t beginPtr = 0;
        uint32_t count = 0;
        if (!ProbeReadPtr(base + off, &beginPtr)) continue;
        if (!ProbeReadU32(base + off + 8, &count)) continue;
        if (!LooksReadablePtr(beginPtr)) continue;
        if (count < 1 || count > 512) continue;

        int validEntries = 0;
        char detail[320] = {};
        int dl = 0;
        for (uint32_t i = 0; i < count && i < 6; ++i) {
            uintptr_t entry = 0;
            if (!ProbeReadPtr(beginPtr + i * 8, &entry)) break;
            if (!LooksReadablePtr(entry)) continue;
            uintptr_t sub = 0; uint32_t id = 0; bool subOk = false, idOk = false;
            if (ProbeReadPtr(entry + 8, &sub) && LooksReadablePtr(sub)) {
                subOk = true;
                idOk = ProbeReadU32(sub + 8, &id);
            }
            float m = 0; bool mOk = false;
            __try { m = *(float*)(entry + 0x150); mOk = true; } __except(EXCEPTION_EXECUTE_HANDLER) {}
            dl += sprintf_s(detail + dl, sizeof(detail) - dl,
                            "[%u]E=0x%llX sub=%s id=%s/0x%X m150=%s ",
                            i, (unsigned long long)entry,
                            subOk ? "ok" : "no",
                            idOk ? "ok" : "no",
                            idOk ? id : 0,
                            mOk ? "ok" : "no");
            if (subOk && idOk) ++validEntries;
        }
        if (validEntries > 0) {
            ++candidates;
            CLog("PROBE: CAND base=%s+0x%X pool=0x%llX begin=0x%llX count=%u valid=%d %s",
                 label ? label : "?", off, (unsigned long long)(base + off),
                 (unsigned long long)beginPtr, count, validEntries, detail);
            if (g_realvr) {
                void* cur = nullptr;
                __try { cur = *(void**)((uint8_t*)g_realvr + 0x38098); }
                __except(EXCEPTION_EXECUTE_HANDLER) {}
                if (!cur) {
                    bool ok = WritePtr((uint8_t*)g_realvr + 0x38098, (void*)(base + off));
                    CLog("PROBE: wrote candidate pool 0x%llX -> RealVR+0x38098 patch=%s",
                         (unsigned long long)(base + off), ok ? "OK" : "FAIL");
                }
            }
        }
    }
    return candidates;
}

static bool ProbeCameraStructures() {
    ModuleRange mod = MainModule();
    if (!mod.base) return false;
    uintptr_t hint = Scan(mod, "88 50 41 48 8B 47 40");
    if (!hint) { CLog("PROBE: CamPoolHint not found"); return true; } // give up (won't appear later)

    // Find the first RIP-relative 64-bit load backwards from the hint.
    uintptr_t slotAddr = 0;
    const uint8_t* scan = (const uint8_t*)(hint - 1);
    const uint8_t* end = (const uint8_t*)(hint > 256 ? hint - 256 : (uintptr_t)mod.base);
    while (scan > end) {
        bool got = false;
        __try {
            if ((scan[0] == 0x48 || scan[0] == 0x4C) && scan[1] == 0x8B && (scan[2] & 0xC7) == 0x05) {
                int32_t disp = *(int32_t*)(scan + 3);
                slotAddr = (uintptr_t)(scan + 7) + disp;
                got = true;
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        if (got) break;
        --scan;
    }
    if (!slotAddr) { CLog("PROBE: no RIP-load slot near hint"); return true; }

    uintptr_t mgr = 0;
    if (!ProbeReadPtr(slotAddr, &mgr) || mgr < 0x10000) {
        CLog("PROBE: manager still null (slot=0x%llX), will retry", (unsigned long long)slotAddr);
        return false;  // retry later
    }
    CLog("PROBE: ===== camera manager probe =====");
    CLog("PROBE: slot=0x%llX manager=0x%llX", (unsigned long long)slotAddr, (unsigned long long)mgr);

    int candidates = 0;
    candidates += ProbePoolBase(mgr, "mgr");

    uintptr_t level1 = 0;
    if (ProbeReadPtr(mgr, &level1) && LooksReadablePtr(level1)) {
        CLog("PROBE: level1=0x%llX", (unsigned long long)level1);
        candidates += ProbePoolBase(level1, "level1");
    }

    for (int off = 0; off <= 0x300; off += 8) {
        uintptr_t ptr = 0;
        if (!ProbeReadPtr(mgr + off, &ptr) || !LooksReadablePtr(ptr)) continue;
        char label[32] = {};
        sprintf_s(label, "mgr[0x%X]", off);
        candidates += ProbePoolBase(ptr, label);
    }
    CLog("PROBE: ===== done, %d candidate array(s) =====", candidates);
    return true;
}

static void LogCurrentSites() {
    ModuleRange mod = MainModule();
    CLog("GTA5.exe base=%p size=0x%llX", mod.base, (unsigned long long)mod.size);

    struct Pat { const char* name; const char* sig; bool rip; int disp; int len; };
    const Pat pats[] = {
        { "ViewInverse", "F3 0F 11 4B 70 F3 0F 10 57 74 0F 2F D3", false, 0, 0 },
        { "FOV1stCar",  "F3 0F 11 45 43 F3 0F 11 55 2B", false, 0, 0 },
        { "Proj",       "48 89 8B 70 01 00 00 89 8B 7C 01 00 00", false, 0, 0 },
        { "FOV3rd",     "48 8B CB F3 0F 11 83 90 00 00 00 48 83", false, 0, 0 },
        { "FOVUni",     "0F 28 C8 F3 0F 11 8B 90 00 00 00 0F 28 C8", false, 0, 0 },
        { "CamParams",  "48 81 EC B0 00 00 00 F3 0F 10 02 F3 0F 10", false, 0, 0 },
        { "ViewPort",   "48 8B 15 ?? ?? ?? ?? 48 8D 2D ?? ?? ?? ?? 48 8B CD", true, 3, 7 },
        { "CamPoolHint","88 50 41 48 8B 47 40", false, 0, 0 },
    };

    for (const Pat& p : pats) {
        uintptr_t hit = Scan(mod, p.sig);
        uintptr_t value = (hit && p.rip) ? ResolveRIP(hit, p.disp, p.len) : hit;
        CLog("%-12s hit=0x%llX value=0x%llX rva=0x%llX",
             p.name,
             (unsigned long long)hit,
             (unsigned long long)value,
             value ? (unsigned long long)(value - (uintptr_t)mod.base) : 0ull);
    }
}

static DWORD WINAPI CompatThread(void*) {
    BuildLogPath();
    DeleteFileA(g_logPath);
    CLog("RealVRCompat starting");

    HMODULE realvr = nullptr;
    for (int i = 0; InterlockedCompareExchange(&g_running, 1, 1) && i < 200; ++i) {
        realvr = FindModuleByName("RealVR.asi");
        if (realvr) break;
        Sleep(50);
    }

    if (!realvr) {
        CLog("RealVR.asi not loaded; nothing to patch");
        return 0;
    }
    g_realvr = realvr;

    char path[MAX_PATH] = {};
    K32GetModuleFileNameExA(GetCurrentProcess(), realvr, path, MAX_PATH);
    CLog("RealVR.asi base=%p size=0x%X path=%s", realvr, ModuleSize(realvr), path);

    char exePath[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    FileVersion exeVersion = {};
    int detectedVersionId = -1;
    if (ReadFileVersionA(exePath, &exeVersion)) {
        detectedVersionId = DetectRealVRVersionId(exeVersion);
        CLog("GTA5.exe file version=%u.%u.%u.%u detectedRealVRId=%d",
             exeVersion.major, exeVersion.minor, exeVersion.build, exeVersion.revision,
             detectedVersionId);
    } else {
        CLog("GTA5.exe file version read failed path=%s", exePath);
    }

    uint32_t* versionGlobal = (uint32_t*)((uint8_t*)realvr + 0x35A50);
    uint32_t before = 0;
    __try {
        before = *versionGlobal;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        CLog("failed reading RealVR version global at +0x35A50");
    }

    bool versionOk = false;
#if REALVRCOMPAT_ENABLE_VERSION_PATCH
    if (detectedVersionId > 0) {
        bool rangeOk = PatchRealVRVersionLimit(realvr, (uint32_t)detectedVersionId);
        versionOk = rangeOk && WriteU32(versionGlobal, (uint32_t)detectedVersionId);
    } else {
        CLog("version global patch skipped: unsupported/unknown GTA5.exe version");
    }
#endif
    uint32_t after = 0;
    __try {
        after = *versionGlobal;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    CLog("version global +0x35A50 before=%u after=%u target=%d patch=%s",
         before, after, detectedVersionId, versionOk ? "OK" : "DISABLED");

    void** realvrLogFn = (void**)((uint8_t*)realvr + 0x38018);
    bool logFnOk = false;
    uint64_t oldLogFn = 0;
    __try {
        oldLogFn = *(uint64_t*)realvrLogFn;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    if (oldLogFn == 0) {
        logFnOk = WritePtr(realvrLogFn, (void*)&RealVRLogNoop);
    }
    uint64_t newLogFn = 0;
    __try {
        newLogFn = *(uint64_t*)realvrLogFn;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    CLog("log callback +0x38018 before=0x%llX after=0x%llX patch=%s",
         (unsigned long long)oldLogFn,
         (unsigned long long)newLogFn,
         (oldLogFn != 0 || logFnOk) ? "OK" : "FAIL");

    LoadPatchConfig();
    CLog("patch config: FirstPersonJump=%d VehicleCamNop=%d CamPoolWriteNop=%d ForceFallback=%d CamPoolResolve=%d CamMetaOldOffset=%d RestoreCamMetadata=%d RestoreRVRState=%d ActiveEnginePatches=%d EnginePatchDelaySec=%d FirstPersonRearmFrames=%d FirstPersonGuardFrames=%d FirstPersonGuardPulseFrames=%d FirstPersonGuardInterval=%d FirstPersonFlagHold=%d FirstPersonControlFixFrames=%d FirstPersonUnclampFrames=%d FirstPersonResetRearmFrames=%d FirstPersonSoftReset=%d FirstPersonSoftResetFrames=%d KeepVehicleFirstPerson=%d RespawnGraceFrames=%d ScriptCamReset=%d ScriptCamResetFrames=%d VehicleExitModelReset=%d ModelResetRequireControl=%d ModelResetDeferMaxFrames=%d ModelResetPreserveWeapons=%d CutsceneHeadingFix=%d CutsceneSoftReset=%d CutsceneScriptCamReset=%d CutsceneEndSettleFrames=%d CutsceneModelReset=%d CutsceneGroundWaitMaxFrames=%d ControlLossHeadingFix=%d ControlLossThresholdFrames=%d CutsceneRecenterFix=%d VehicleExitRecenterFix=%d",
         g_patchFirstPersonJump, g_patchVehicleCamNop, g_patchCamPoolWriteNop,
         g_patchForceFallback, g_patchCamPoolResolve, g_patchCamMetaOldOffset,
         g_patchRestoreCamMetadata, g_patchRestoreRVRState,
         g_patchActiveEngine, g_enginePatchDelaySec, g_firstPersonRearmFrames,
         g_firstPersonGuardFrames, g_firstPersonGuardPulseFrames, g_firstPersonGuardInterval,
         g_firstPersonFlagHold, g_firstPersonControlFixFrames,
         g_firstPersonUnclampFrames, g_firstPersonResetRearmFrames,
         g_firstPersonSoftReset, g_firstPersonSoftResetFrames, g_keepVehicleFirstPerson,
         g_respawnGraceFrames, g_scriptCamReset, g_scriptCamResetFrames,
         g_vehicleExitModelReset, g_modelResetRequireControl, g_modelResetDeferMaxFrames,
         g_modelResetPreserveWeapons, g_cutsceneHeadingFix, g_cutsceneSoftReset, g_cutsceneScriptCamReset,
         g_cutsceneEndSettleFrames, g_cutsceneModelReset, g_cutsceneGroundWaitMaxFrames,
         g_controlLossHeadingFix, g_controlLossThresholdFrames,
         g_cutsceneRecenterFix, g_vehicleExitRecenterFix);

    NopIndirectCallsToRealVRSlot(realvr, 0x38018, "logFunction");
    PatchCamMetadataResolverVersionOffset(realvr);
    PatchCamMetadataResolverFallback(realvr);  // internally gated by ForceFallback + CamPoolWriteNop
    if (g_patchFirstPersonJump) PatchFirstPersonCamFallback(realvr);
    else CLog("first person cam RealVR+0x5477 NOT patched (FirstPersonJump=0)");
    if (g_patchVehicleCamNop) PatchVehicleCamNullCall(realvr);
    else CLog("vehicle cam null-call RealVR+0x55AD NOT patched (VehicleCamNop=0)");
    PatchRealVRProxySlots(realvr, "pre-main");
    RegisterCompatScript("pre-main slots ready");

    LogCriticalRealVRSlots("after version patch, before RealVR main");
    LogCurrentSites();
    // Option #2: resolve camMetadataPool from GTA5 camera code at runtime.
    // Must run AFTER LogCurrentSites, which triggers the scan.
    if (g_patchCamPoolResolve) ResolveCamMetadataPool(realvr);
    else CLog("camPool resolve disabled (CamPoolResolve=0)");
    LogCriticalRealVRSlots(g_patchCamPoolResolve ? "after camPool resolve" : "after camPool untouched");
    if (g_patchActiveEngine) {
        HANDLE engine = CreateThread(nullptr, 0, EnginePatchThread, nullptr, 0, nullptr);
        if (engine) {
            CloseHandle(engine);
            CLog("active original patch pass deferred by EnginePatchDelaySec=%d", g_enginePatchDelaySec);
        } else {
            CLog("active original patch pass deferred thread creation failed: error=%lu", GetLastError());
        }
        CLog("RealVRCompat done: version/proxy patch + camPool %s + engine patches observed/deferred",
             g_patchCamPoolResolve ? "resolve" : "untouched");
    } else {
        CLog("active original patch pass disabled (camPool state still logged above)");
        CLog("RealVRCompat done: version/proxy patch + camPool %s only",
             g_patchCamPoolResolve ? "resolve" : "untouched");
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        AddVectoredExceptionHandler(1, CompatVectoredExceptionHandler);
        InterlockedExchange(&g_running, 1);
        HANDLE t = CreateThread(nullptr, 0, CompatThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
        HANDLE watch = CreateThread(nullptr, 0, SlotWatchThread, nullptr, 0, nullptr);
        if (watch) CloseHandle(watch);
        HANDLE diag = CreateThread(nullptr, 0, CamPoolDiagThread, nullptr, 0, nullptr);
        if (diag) CloseHandle(diag);
    } else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&g_running, 0);
        if (g_realvr && InterlockedCompareExchange(&g_scriptRegistered, 0, 0)) {
            ScriptUnregisterFn unreg = ReadRealVRSlot<ScriptUnregisterFn>(0x232E8);
            if (unreg) {
                __try { unreg(module); } __except(EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
    }
    return TRUE;
}
