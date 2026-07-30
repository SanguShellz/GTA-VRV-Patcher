<h1>GTA V RealVR Patcher<br><sub><sup><em>RealVRCompat ASI</em></sup></sub></h1>

**Provides compatibility for the GTA V RealVR mod on newer GTA V Legacy builds.**

## ℹ️ Compatibility
- `GTA-VRV-Patcher` has been tested and confirmed to work with `Steam` versions of `GTA V Legacy`:
  `v1.0.2845`, s`v1.0.3751`, `v1.0.3788.0`, and `v1.0.3889.0`.
- Other versions newer than `v1.0.2612` may also work.
- `Rockstar` and `Epic` versions should also be compatible.
- `GTA V Enhanced` is not currently compatible.
- OpenVR or OpenXR Runtimes work depending on the HMD (OpenVR [VRAPI = 2] is the default in RealVR.ini). 
- Oculus Runtime (OVR) crashes.

## 💾 Installation:
- Run GTA V Legacy once to create a `settings.xml` 
- Extract the archive and copy the mod files into the GTA V Legacy game folder
- run `RealConfig.bat`
- Requires latest Script Hook V: [https://www.dev-c.com/gtav/scripthookv/](https://www.dev-c.com/gtav/scripthookv/)
- The 00_ prefix for `00_RealVRCompat.asi` so that it loads before `RealVR.asi

## 🛠️ Troubleshooting: 
- Turn on the controller before launching the game with the VR mod; otherwise, the game will not detect any input.
- If you see this error:
	
	> CORE: An exception occurred while executing 'RealVR.asi', id 4

	It's likely 'asi\realvr.asi exists from a previous setup.  Delete it.

## ⚙️ Configuration
- Settings for `GTA V RealVR Patcher` are configured in `RealVRCompat.ini
- Additonal settings for the `GTA V Real VR` mod are configured in `RealVR.ini`

## 📦 Building
**️Prerequisites**:
- Visual Studio 2026
- ScriptHookV SDK
- Windows SDK

**️Build Steps**:
```bash
cd "path\to\project"
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\msbuild.exe" RealVRCompat.vcxproj /p:Configuration=Release /p:Platform=x64
```
Output: `bin\00_RealVRCompat.asi`

## 📋 License
See [LICENSE.txt](LICENSE.txt)
