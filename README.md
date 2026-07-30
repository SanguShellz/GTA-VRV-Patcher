# RealVRCompat ASI

Compatibility shim for the original `RealVR.asi` on newer GTA V builds. Tested against GTA V `1.0.3788.0` with ScriptHookV `v3788.0/1013.34`.

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

## Installation

Place these files in the GTA V game folder:
- `00_RealVRCompat.asi` — the compatibility shim (load before `RealVR.asi`)
- `RealVR.asi` — original VR mod
- RealVR proxy `d3d11.dll`
- `RealVR.ini` — configuration
- ScriptHookV/ASI loader files

The `00_` prefix is intentional so the compat shim loads before `RealVR.asi`.

## Configuration

Edit `RealVRCompat.ini` to control behavior.

### Recommended Config

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
ModelResetPreserveWantedLevel=1

# Also fire RealVR's own HMD recenter on vehicle exit (see "RealVR's internal
# state" above). Off by default.
VehicleExitRecenterFix=0

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
ModelResetPreserveWantedLevel=1
```

## Compatibility

- GTA V build `1.0.3788.0`
- Not compatible with the GTA V Enhanced Edition

## License

See [LICENSE.txt](LICENSE.txt)
