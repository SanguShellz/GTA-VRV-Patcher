> [!important]
> **THIS REPOSITORY IS NO LONGER MAINTAINED**
>
> *There will be no further updates, bug fixes, or new features for this fork.*

<h1>GTA V RealVR Patcher<br><sub><sup><em>RealVRCompat ASI</em></sup></sub></h1>

**Provides compatibility for the GTA V RealVR mod on newer GTA V Legacy builds.**

## ℹ️ Compatibility:

- `GTA-VRV-Patcher` has been tested and confirmed to work with `Steam` versions of `GTA V Legacy`:
  `v1.0.2845`, s`v1.0.3751`, `v1.0.3788.0`, and `v1.0.3889.0`.
- Other versions newer than `v1.0.2612` may also work.
- `Rockstar` and `Epic` versions should also be compatible.
- `GTA V Enhanced` is not currently compatible.
- OpenVR or OpenXR Runtimes work depending on the HMD (OpenVR [VRAPI = 2] is the default in RealVR.ini). 
- Oculus Runtime (OVR) crashes.

## ⚠️ Known Issues:
- Currently this patcher works best for free-roam gameplay.  
- Right stick camera control gets disabled after some types of cutscenes.
- Heading gets turned around after some types of cutscenes.  
- There may be compatibility issues with certain types of mods that interact with the player character such as `Player Companion`, etc.
- The right-stick camera fix reloads the character from a snapshot captured before entering a vehicle. Anything not explicitly included in that snapshot isn’t restored, and any changes or states that occur while in the vehicle aren’t captured either.

## 💾 Installation:

**Download:**
- [GTAVRV.Patcher.v1.2.7.Release.zip](https://github.com/SanguShellz/GTA-VRV-Patcher/releases/download/1.2.7/GTAVRV.Patcher.v1.2.7.Release.zip)
- Extract the archive
- Move the extracted files into the game directory.
- Note: The 00_ prefix for `00_RealVRCompat.asi` so that it loads before `RealVR.asi

**Default Install Locations:**
- **Steam:** `C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto V`
- **Epic Game Store:** `C:\Program Files\Epic Games\Grand Theft Auto V`
- **RockStar Games Launcher:**  `C:\Program Files\Rockstar Games\Grand Theft Auto V`

**Setup:**
-  [Requires latest version of Script Hook V](https://www.dev-c.com/gtav/scripthookv/)
- Run GTA V Legacy once to create a `settings.xml` 
- Extract the archive and copy the mod files into the GTA V Legacy game folder
- run `RealConfig.bat`

## 🛠️ Troubleshooting: 

- If you see this error:
	
	> CORE: An exception occurred while executing 'RealVR.asi', id 4

	It's likely 'asi\realvr.asi exists from a previous setup.  Delete it.

## ⚙️ Configuration:

- Settings for `GTA V RealVR Patcher` are configured in `RealVRCompat.ini
- Additonal settings for the `GTA V Real VR` mod are configured in `RealVR.ini`

## 📦 Building:

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

## 📋 License:

See [LICENSE.txt](LICENSE.txt)
