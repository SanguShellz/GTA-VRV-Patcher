<h1>GTA V RealVR Compatibility Patcher<br><sub><sup><em>RealVRCompat ASI</em></sup></sub></h1>

Compatibility shim for the original `RealVR.asi` on newer GTA V builds. Tested against GTA V `1.0.3889.0` with ScriptHookV `v3889.0/1158.13`.

## 🛠 Building

**️Prerequisites**:
- Visual Studio 2026
- ScriptHookV SDK (v3788.0 or compatible)
- Windows SDK

**️Build Steps**:
```bash
cd "path\to\project"
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\msbuild.exe" RealVRCompat.vcxproj /p:Configuration=Release /p:Platform=x64
```

Output: `bin\00_RealVRCompat.asi`

## 💾 Installation:
- Run GTA V Legacy once to create a `settings.xml` 
- Extract the archive and copy the mod files into the GTA V Legacy game folder
- run `RealConfig.bat`
- Requires latest Script Hook V: [https://www.dev-c.com/gtav/scripthookv/](https://www.dev-c.com/gtav/scripthookv/)
- ❗Important❗:  
	-	Turn on the controller before launching the game with the VR mod; otherwise, the game will not detect any input.
	
	- If you see this error:

		> CORE: An exception occurred while executing 'RealVR.asi', id
4

	    It's likely 'asi\realvr.asi exists from a previous setup.  Delete it..

	The 00_ prefix for `00_RealVRCompat.asi` so that it loads before `RealVR.asi

## ⚙️ Configuration

Settings are configured in `RealVRCompat.ini`. Most users can leave the defaults unchanged.

### Recommended Configuration

```ini
[patches]
FirstPersonJump=1
VehicleCamNop=1
CamPoolWriteNop=0
ForceFallback=1
CamPoolResolve=1
CamMetaOldOffset=1
RestoreCamMetadata=1
RestoreRVRState=1

ActiveEnginePatches=0
EnginePatchDelaySec=70

FirstPersonRearmFrames=120
FirstPersonGuardFrames=600
FirstPersonGuardPulseFrames=0
FirstPersonGuardInterval=90
FirstPersonFlagHold=0
FirstPersonControlFixFrames=0
FirstPersonResetRearmFrames=0
FirstPersonUnclampFrames=120

FirstPersonSoftReset=1
FirstPersonSoftResetFrames=8

KeepVehicleFirstPerson=0
RespawnGraceFrames=300

ScriptCamReset=0
ScriptCamResetFrames=2

VehicleExitModelReset=1
ModelResetRequireControl=1
ModelResetDeferMaxFrames=300
ModelResetPreserveWeapons=1
ModelResetPreserveWantedLevel=1
VehicleExitRecenterFix=0

CutsceneHeadingFix=1
CutsceneRecenterFix=1
CutsceneSoftReset=1
CutsceneScriptCamReset=0
CutsceneEndSettleFrames=2
CutsceneModelReset=0
CutsceneGroundWaitMaxFrames=180

ControlLossHeadingFix=1
ControlLossThresholdFrames=45
```


## ℹ️ Compatibility

- GTA V build `1.0.3889.0`
- Not compatible with the GTA V Enhanced Edition

## 📋 License

See [LICENSE.txt](LICENSE.txt)
