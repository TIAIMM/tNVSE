# CMake build

The root CMake project is the sole supported build entry point for tNVSE.
Source files are discovered recursively under `tnvse/Src` and
`commonlib_nv/Src`; Visual Studio project files are generated into `out/` and
must not be maintained in the source tree.

## Requirements

- Visual Studio 2026 with the MSVC v145 toolset and Win32 desktop workload.
- CMake 4.3.1 or newer. The VS2026 bundled executable is:
  `D:\Visual Studio Community 2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`.
- Initialized dependencies: `git submodule update --init --recursive`.
- The restored `Microsoft.DXSDK.D3DX` package under `tnvse/packages`.

The main `tnvse` and `commonlib_nv` targets intentionally retain the legacy
Visual Studio projects' multibyte character-set contract (`_MBCS`). They do
not add `/utf-8`; third-party targets keep only the encoding switches already
present in their original dedicated projects.

## Configure and build

Run these commands from the repository root in PowerShell:

```powershell
$cmake = 'D:\Visual Studio Community 2026\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
& $cmake --preset vs2026-win32
& $cmake --build --preset vs2026-win32-release
```

The Release DLL is written to:

```text
out/build/vs2026-win32/bin/Release/tnvse.dll
```

The build also:

- builds the reduced FreeType, HarfBuzz, libunibreak, and commonlib targets;
- compiles all seven native D3D9 shaders and runs the shader ABI verifier;
- copies the D3DX runtime DLLs next to the build output;
- copies `tnvse.dll` and the shaders to the live mod when
  `TNVSE_PLUGIN_PATH` points to its `NVSE/plugins` directory.

## Visual Studio folder workflow

Open the repository root with **File > Open > Folder**. Under **Tools >
Options > CMake > General**, enable **Always use CMakePresets.json**, then
reopen the folder. Select these entries in the CMake toolbar:

- Configure preset: `Visual Studio 2026 - Win32 - v145`
- Build preset: `Build tNVSE Debug (Win32/v145)` or
  `Build tNVSE Release (Win32/v145)`

In CMake Targets View, build `tnvse.dll`, not `commonlib_nv`. Building the
main DLL pulls in all libraries and native shaders and then performs live
deployment. `Install tNVSE` creates the staging install tree and is not part
of the normal edit-build-run loop.

The build presets intentionally omit `jobs`. Visual Studio then lets MSBuild
choose its native parallelism instead of translating a zero job count into
the invalid command-line argument `--parallel 0`.

To select or change the live deployment directory explicitly:

```powershell
& $cmake --preset vs2026-win32 -DTNVSE_PLUGIN_PATH='E:\NVCNTest\MO2\mods\tNVSE\NVSE\plugins'
```

## Debug and staging install

```powershell
& $cmake --build --preset vs2026-win32-debug
& $cmake --install out/build/vs2026-win32 --config Release --prefix out/package/tNVSE
```

The install tree contains the plugin DLL, compiled shaders, XML font
configuration, and bundled font files in their mod-relative directories.
