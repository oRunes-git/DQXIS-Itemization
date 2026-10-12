# DQXIS-Itemization

Source and original distribution archive for the Dragon Quest XI S Itemization mod, public release **1.0** (internal build `independent-crafting-v1.16.0`).

This repository publishes the preserved Itemization source from before subsequent endgame-mod development. It does not include the endgame mod, private saves, player item history, logs or recovery backups.

## Original Nexus distribution

`release/Itemization v1.0.zip` is the unchanged local release archive prepared on October 9, 2026 at 16:28 Europe/Berlin, before the reported Nexus upload at 16:53. Its SHA-256 is:

```text
9e373f6a564b321f15a499ee03a0c32aecd4a35d54cca52c3a123fe9638ca4d7
```

This identifies the local archive; the Nexus-hosted file has not been independently downloaded and compared. `release/package-manifest.json` contains the distribution's individual file hashes.

## Security-review entry points

- `src/load_probe.cpp` builds `load-probe.exe`. It finds the running `DRAGON QUEST XI S.exe`, checks the supported executable signature, and loads `itemization-probe.dll` into that process using Windows process-memory and remote-thread APIs. These operations can trigger antivirus detection; they are required by this launch mechanism.
- `src/native_probe.cpp`, its supporting headers in `src/`, and `include/` implement native game hooks and Itemization mechanics. MinHook is bundled under `third_party/MinHook`.
- `installer/Install.ps1` and `installer/Install.cmd` are the original distribution installer scripts. They install the packaged files, handle compatibility checks/backups and create the launch shortcut.
- `launcher/Launch-itemization.ps1` and `.cmd` are the original distribution launch scripts. They launch the game through Steam when necessary and invoke the loader.
- `config/` contains the original release JSON configuration. Persistent player item history is intentionally absent.
- `SOURCE-SNAPSHOT.json` records hashes of the preserved source, dependencies, build script and review configuration.

Matching preserved build outputs were compared with the original ZIP. The distributed binary SHA-256 values are:

| File | SHA-256 |
|---|---|
| `itemization-probe.dll` | `94a837fcd9cf7d25d5ed36d287fe30c755d4bc58609340a08a996a6e23d7195d` |
| `load-probe.exe` | `e574a247105a1b8b099705914fba5239781c87e44743cb9d0858145611f25f93` |

Publishing source assists review; it is not a security certification.

## Build the native DLL and loader

Requirements: Windows x64, Visual Studio C++ Build Tools with the x64 MSVC compiler and a Windows SDK, and PowerShell. The original script locates the installation using `vswhere.exe` in the standard Visual Studio Installer directory.

From the repository root:

```powershell
./build-probe.ps1 -BuildDirectory build/review
```

This compiles `build/review/itemization-probe.dll` and `build/review/load-probe.exe`. It does not install the mod, start the game or invoke the loader. The preserved build used Visual Studio 2022 Build Tools, C++17, `/O2` and `/MT`. Rebuilding with a different toolchain, path or timestamp may produce different binary hashes; byte-identical reproducibility has not been established.

The installer expects the complete extracted distribution layout, including `Payload`, `Guides`, the README and package manifest. The review copies under `installer/` and `launcher/` are provided for inspection; use the complete original ZIP for installation. PAK assets are preserved in that ZIP. The native build script does not regenerate them.

## Licences and documentation

The original project licence is retained in `LICENSE`. Dependencies retain their own licences in `third_party/MinHook/LICENSE.txt` and `third_party/nlohmann/LICENSE.MIT`. Player and developer guides are under `docs/`.
