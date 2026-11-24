<p align="center"><img src="DagorAssetsExtractor/assets/ui/DAE.ico" alt="DAE icon" width="64" height="64"></p>

<h1 align="center">Dagor Assets Extractor</h1>

Dagor Assets Extractor is a software for extracting assets from titles built on the Dagor engine, intended for learning purposes :) The tool currently supports:

- Exporting static models with textures and materials.
- Exporting animated models (skeletons + animations).
- Exporting textures to PNG, DDS, or TGA formats.

## Prerequisites

- Windows machine
- Visual Studio with the Desktop development with C++ workload (or another environment that can open CMake-generated solutions)
- CMake (usually included with Visual Studio)

## Build (Visual Studio)

> The software use DirectXTex, Win32 API & D3D11 & for decompression use o2core_6_win64.dll meaning you can't compile on non windows systems.

1. From the project root run:

	GenerateProjects.bat

2. Open the generated solution in the `build/vs2026` folder (DagorAssetsExtractor.slnx) with Visual Studio.
3. Choose a build configuration: `Debug` or `Release`.
4. Choose platform: `x64`.
5. Build the solution (Build → Build Solution).

## Disclaimer

This software is provided for learning purposes only. We take no responsibility for how it is used.