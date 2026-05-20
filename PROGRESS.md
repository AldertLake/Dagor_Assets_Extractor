# Dagor Assets Extractor - Development Progress Registry

This file serves as the consolidated progress record and technical reference for all AI engineering agents and human developers working on the Dagor Assets Extractor project. It tracks implemented features, codebase architecture, crucial proprietary format specifications, and pending development slices.

---

## 🏗️ Codebase Architecture & Module Map

The application is built in C++17/20, targeting Windows (x64) via MSBuild/Visual Studio (`DagorAssetsExtractor.sln`), rendering with DirectX 11, and using a custom-docked ImGui UI.

### Core Modules
* **`App`** (`AppState.h/cpp`): Orchestrates global state, coordinates background scans, handles configuration loading, and controls the async Oodle dynamic library lifecycle.
* **`UI`** (`UI.h/cpp`): Handles the custom center-stacked interface using virtualized `ImGuiListClipper` lists. Implements multi-view browsing: standard search, "Group by BLK" (hierarchical pack grouping), and multi-resource descriptor relationship inspector tables. Securely scoped using `ImGui::PushID(row)` / `ImGui::PopID()` to avoid conflicting ID assertions on matching filenames.
* **`Platform`** (`Platform.h/cpp`): Manages OS-level tasks (Windows folder picker dialogs, path sanitization to prevent traversal, standard directories like `%APPDATA%`, and conversion between UTF-8 and Win32 wide strings).
* **`Jobs`** (`JobSystem.h/cpp`): Thread-safe worker queue managing background operations (`std::thread`). Eliminates detached threads, handles lifecycle joins during app shutdown, and catches exceptions.
* **`AssetIndex`** (`AssetIndex.h/cpp`):
  - **Scanner**: Scans the WT game directory for markers (`aces.exe`, folders, container types) to construct the resource registry.
  - **Index Cache**: Fast save/load path at `%APPDATA%\DagorAssetsExtractor\index-cache.txt` storing verified files with size/timestamp validation.
  - **PackContents**: Manages thread-safe lazy-loading of container sub-assets (`PackContentsCache` with `std::mutex`). Enforces a 256MB per-file safety parsing limit.
  - **BlkRelationships**: Recursively resolves `.blk` dependencies (`gameResPack` -> GRP, `ddsxTexPack` -> DXP) into index links.
* **`DependencyGraph`** (`DependencyGraph.h/cpp`): Orchestrates asynchronous relationship building between GameRes descriptors, GRP packages, and DXP texture packs. Matches descriptor resource names to GRP sub-assets and texture slot references to DXP textures using preferenced BLK groupings.
* **`DagorIO`** (`MemoryReader.h/cpp`): Safe, endian-aware binary reader for memory-mapped parsing of headers, tables, and payloads.
* **`DagorFormats`**: Binary container and metadata parsers:
  - `VromfsReader`: Parses `VRFx` uncompressed filesystem containers.
  - `DxpReader`: Parses `.dxp.bin` texture packages and references separate DDSx headers.
  - `GrpReader`: Robust dynamic table parser for both legacy byte-version (`GRP\x00` - `GRP\x02`) and modern ASCII (`GRP2` / `GRP3`) Game Resource Pack containers. Automatically decodes sub-asset payloads, offsets, sizes, and class ID metadata (e.g. `RendInst`, `DynModel`, `Anim2Data`).
  - `DdsxReader`: Parses proprietary DDSx texture metadata headers and identifies compression signatures.
  - `BlkReader`: Parses text-based BLK files, supporting typed properties, nested blocks, and comments.
  - `RendInstReader`: Parses proprietary `RIND` binary 3D static geometry buffers (vertices, normals, UVs, LODs, material slot primitive index ranges).
  - `GameResDescReader`: Decodes binary GameRes descriptors (`riDesc.bin`/`dynModelDesc.bin`) wrapped in `0x02 + 24-bit compressed size` envelopes. Embeds a custom binary BLK data reader for extracting resource block names, string parameters, material slots, and texture index assignments.
* **`Export`**: Output serialization:
  - `ExportManifest` (`.export-manifest.json` format): Emits JSON records documenting exported assets, source packages, exact output paths, warnings, and extractor build metadata.
  - `TextureExporter`: Reconstructs standard `.dds` files from DDSx headers/payloads (passthrough or decompressed). Handles modern DX10 Extended (`DdsHeaderDX10` for BC6H/BC7) and legacy FourCC (DXT1/3/5, ATI1/2) header generation. Also exports readable `.png`/`.tga` images for supported uncompressed DDSx textures (`R8`, `R8G8`, `R8G8B8A8`, `B8G8R8A8`) and can fall back to `.dds` when readable conversion is unavailable.
  - `ModelExporter`: Converts multi-material `RIND` geometry into standard Wavefront `.obj` (highest LOD) or fully-compliant binary glTF 2.0 `.glb` files with nested LOD scenes. Correctly packs and maps texture/material references out-of-the-box.

---

## 🛠️ Proprietary Format Specifications & Technical Gotchas

This section records hard-won technical specifications decoded from the game files, critical for correct parsing and expansion.

### 1. DDSx Texture Headers & Decompression
* **Header Size**: 32 bytes.
* **Layout**:
  - `0x00` - `0x03`: Magic (`DDSx` or variant)
  - `0x1C`: `packedSz` (packed size on disk), **NOT** a compression enum!
  - **Compression Type**: Decoded from the high byte/high bits of the DDSx `flags` field (not `0x1C`):
    - `0x00`: None (uncompressed)
    - `0x20`: **zstd**
    - `0x60`: **Oodle**
    - `0x80`: **zlib**
    - `0x40`: **LZMA** (detected, currently unsupported)
* **Mip Reordering (`FLG_REV_MIP_ORDER`)**: If this flag is set in the DDSx header, mips are stored on disk in reverse order (smallest mip first, progressing to largest). The exporter **must reverse** this sequence back into standard DDS layout (largest mip first) to prevent corrupted textures in standard image viewers.
* **DDS Generation**:
  - Legacy compressed formats (DXT1, DXT3, DXT5, ATI1, ATI2) write legacy FourCC DDS headers.
  - Modern formats (BC6H, BC7) write standard DX10 extended DDS headers (`DdsHeaderDX10`) for DirectXTex/Photoshop compatibility.
* **Readable Export Support**:
  - `.png` export uses Windows Imaging Component (WIC).
  - `.tga` export uses an internal uncompressed writer.
  - Readable export currently supports `BC1-BC5` plus `R8`, `R8G8`, `R8G8B8A8`, `B8G8R8A8`, `R16G16B16A16_FLOAT`, and `R32G32B32A32_FLOAT` DDSx textures, and writes only the top mip level.
  - If the user selects `.png`/`.tga` for an unsupported texture and `preserveOriginalDDS` is enabled, the exporter falls back to `.dds` instead of failing.
  - `LZMA/7zip` DDSx records currently fall back to raw `.ddsx` export with a manifest warning because decompression is still unsupported.
* **Oodle Dynamic Linking**: Requires `oo2core_6_win64.dll`. The app now resolves the bundled DLL automatically from the build output or repo `third_party` folder instead of asking the user for a path in settings. The app build copies the DLL beside the executable, and if the DLL is missing or fails to load, Oodle-compressed textures fall back to raw `.ddsx` export containing the DDSx header plus compressed payload.

### 2. GRP (Game Resource Pack) Containers
* **Magics Supported**: Legacy `GRP\x00` - `GRP\x02` and modern ASCII `GRP2` / `GRP3` headers.
* **GRP2 Header Structure**: 16-byte `GrpHeader` followed by name map and resource entry tables. Resource name strings, class IDs, payload offsets, and payload sizes are resolved dynamically.
* **GRP Class IDs**: Metadata class IDs correspond to specific Dagor asset types:
  - `0x27C80FA9`: `RendInst` (Static models; currently exportable).
  - `0xB4B7D9C4`: `DynModel` (Dynamic/skeletal models; pending Milestone 5 export work).
  - `0x40C586F9`: `Anim2Data` (Animation trees; unsupported).
  - `0x56F81B6D`: `GeomNodeTree` (Skeletal rigs; now parsed for metadata in GRP inspection).

### 3. VROMFS Containers
* **Magic Supported**: `VRFx`.
* **Structure**: Header, file name table, and file data table. Packed/compressed VROMFS files throw structured exceptions since they are not currently decompressed.

### 4. BLK Text Format
* Supports typed parameters (e.g. `gameResPack:t="sample_vehicle"`).
* Supports comments (single-line `#` and `//`, block `/* ... */`), nested blocks, duplicate keys, and quoted strings.
* Used to parse `ddsxTexPack` and `gameResPack` paths to link BLK rules to physical container files.

### 5. RendInst (`RIND`) Static Models
* Decoded proprietary header and chunk parameters.
* Stores nested Level-of-Detail (LOD) blocks.
* Extracts vertex data (float X/Y/Z), normals, UV coordinates (float U/V), and triangular face indices.
* Supports multi-material primitives mapping distinct index ranges to specific material slots.

---

## 🧪 Verification & Test Suite

The project includes an automatic test suite in the `tests/AssetIndexTests` project.

* **Test Execution**:
  ```powershell
  # Compile and run the test suite
  powershell -ExecutionPolicy Bypass -File scripts/run_tests.ps1
  ```
* **Coverage**: Validates endian safe reader, uncompressed Vromfs, Grp, Dxp containers, DDSx headers (including DX10 formats), text Blk parser, JobSystem state, decompression helpers, ExportManifest serialization, DDSx mip-reversal, RIND binary model parsing, GeomNodeTree skeleton parsing/metadata extraction, multi-material glTF/OBJ export outputs, GRP2 metadata parsing, and unified textured model sidecar exporters.
* **Fixtures**: Uses hand-crafted synthetic fixture files under `tests/fixtures/synthetic_wt/`. These fixtures contain valid structural headers but no copyrighted game assets.

---

## 🚀 Active Open Tasks & Next Steps (AI Roadmap)

AI models picking up the next coding task should prioritize the following objectives:

1. **Milestone 5: Dynamic Models & Skeletal Hierarchies (IN PROGRESS)**:
   - **GRP skeleton parsing**: Implemented for `GeomNodeTree` GRP entries. The extractor now decodes node names, parent-child mappings, local transforms, global rest transforms, and inverse bind matrices from skeleton payloads for inspection/testing.
   - **Skinned export integration**: Remaining work is to feed parsed `GeomNodeTree` skeletons into `DynModel` export so the future glTF skin path can reuse the hierarchy/bind data already being computed.
   - **Skinned Weight Bindings**: Parse `dynmodel` skinned vertex buffers, extracting up to 4 joint indices (`uint8_t`) and normalized blend weights (`float`) per vertex. Serialize these inputs into glTF vertex attributes (`JOINTS_0` and `WEIGHTS_0`).
   - **Skeletal glTF Node Serialization**: Write joint bone nodes as a hierarchical tree of glTF nodes. Bind the mesh node to the skin definition to enable out-of-the-box animated dynamic meshes.
2. **PBR Material Properties**:
   - Extend the GLB exporter to extract and map more complex texture maps (normal maps, metallic/roughness, ambient occlusion) from the descriptor materials when their slots are reliably identified.
3. **Readable Texture Export Follow-up**:   - Expand `.png`/`.tga` conversion beyond today's `BC1-BC5` plus `R8`, `R8G8`, `R8G8B8A8`, `B8G8R8A8`, `R16G16B16A16_FLOAT`, and `R32G32B32A32_FLOAT` coverage. Modern HDR/compressed formats such as `BC6H` / `BC7` and other uncommon DDSx layouts still need a heavier transcode backend such as DirectXTex.
   - `LZMA/7zip` DDSx records no longer hard-fail export; they fall back to raw `.ddsx`, but true decompression is still unsupported.
5. **Real Game Validation**:
   - Validate descriptor-to-pack matching and texture decompression/mapping against real War Thunder `riDesc.bin`, `dynModelDesc.bin`, `.grp`, and `.dxp.bin` containers (beyond the synthetic test fixtures).

---

## 📈 Consolidated Project Milestones & Chronological Timeline

### Phase 1: App Foundation & Discovery Scan (Day 1 - Day 2)
* **Milestones**: Created initial D3D11/ImGui application shell. Setup persistent settings (`settings.ini`), folder picker, and game folder validation criteria.
* **Indexing Cache**: Added external index cache file (`index-cache.txt`) to skip re-scans by validating size/timestamp of game files.
* **Refactoring**: Split massive initial `main.cpp` into clean modules (`App`, `Platform`, `UI`, `AssetIndex`).

### Phase 2: Binary Reader & Initial Formats (Day 2 - Day 3)
* **DagorIO**: Implemented safe `MemoryReader` for memory-mapped parsing.
* **Vromfs & DXP Parsers**: Added support for uncompressed `VRFx` containers and `.dxp.bin` textures packs (saving sub-assets to cache).
* **Test Harness**: Created `AssetIndexTests` console project and PowerShell test script (`run_tests.ps1`) pointing to synthetic fixtures.

### Phase 3: Lazy Loading & Thread Safety (Day 4 - Day 5)
* **Lazy Parsing**: Implemented thread-safe `PackContentsCache` for DXP/VROMFS, parsing sub-assets only when requested in the Inspector. Added 256MB safety limits.
* **Job System**: Created `Jobs::JobSystem` to govern C++ background workers, resolving detached-thread safety issues.
* **BlkReader**: Created text BLK parser and added an async Inspector preview panel.
* **BLK Relationships**: Added `BlkRelationships` logic to construct physical GRP/DXP pack mappings from text BLKs.

### Phase 4: Texture Exporter & Decompression (Day 6 - Day 8)
* **Export System**: Setup `ExportManifest` to write standard `.export-manifest.json` metadata records on every export.
* **Texture Export Pipeline**: Developed DDS reconstruction, handling legacy FourCC and DX10 extended headers. Added zlib/zstd/Oodle decompression (linking `oo2core_6_win64.dll` dynamically).
* **Robustness & Fallbacks**: Created a batch-export thread worker with progress logs. Added raw `.ddsx` fallback exports for Oodle textures if the DLL is missing.

### Phase 5: 3D Model Exporter (Day 9)
* **RIND Parser**: Decoded the proprietary binary geometry structure to extract vertices, normals, UVs, and face indices across multiple LODs.
* **Model Exporter**: Built glTF 2.0 GLB and Wavefront OBJ serializers, enabling 3D model exports with manifest logs.
* **DXP Alignment**: Fixed DXP export bugs by utilizing separate DDSx header tables and added strict filename sanitization against path traversal.

### Day 10: GameResDesc Descriptor Parser Slice
* **Zstd Decompression**: Implemented `Decompressor::DecompressZstd` to automatically extract zstd frames using the frame-declared decompressed size.
* **GameResDescReader**: Created binary GameRes descriptor reader for `riDesc.bin` / `dynModelDesc.bin`. Decodes `0x02` zstd envelopes, parses the internal binary BLK layout (name maps, child blocks, parameter records), and extracts texture names and material mappings.
* **Descriptor UI**: Wired dependency counts (textures, materials, slots) into the UI with descriptive tooltips.

### Day 11: Backend Descriptor Resource Relationship Slice
* **Relationship Resolver**: Developed `BuildDescriptorResourceLinks` to dynamically match GRP sub-assets to descriptor entries, map texture slots to texture names, and resolve textures to DXP entries using preferenced BLK groupings.

### Day 12: Relationship Inspector UI Slice
* **Background Jobs**: Integrated descriptor relationship building into the async background job thread pool.
* **UI Integration**: Added a "Rebuild Descriptor Relationships" action in the Tools menu. Created GRP, DXP, and Descriptor relationship tables in the inspector panel displaying exact matches and texture tooltips.

### Day 13: Unified Model Exporter Slice
* **Unified Textured Export**: Upgraded `ModelExporter::ExportToGlb` to support `MaterialInfo`. `AppState::RequestModelExport` now snapshots descriptor links, exports texture dependencies as sidecars, and links glTF primitives to final textures out-of-the-box.
* **Dependency Graph Refactoring**: Extracted relationship building code from `BlkRelationships` into a dedicated, clean `DependencyGraph.h/cpp` module.

### Day 14: Multi-Material & GRP Stability Slice
* **Multi-Material support**: Enhanced `RendInstReader` to parse LOD material primitive index ranges. Upgraded the glTF exporter to write multi-primitive meshes mapped to distinct material slots.
* **GRP Dynamic Table Finder**: Added robust dynamic header-table locator in `GrpReader` supporting varied Magics (`GRP\x00` - `GRP\x02` and modern `GRP2`). Uses a precise table-end heuristic to locate sub-assets and offset bounds, preventing corrupted sub-asset names.
* **UI Direct Export**: Added "Export Model" buttons to descriptor relationship lists to export resolved models directly from descriptor tabs.

### Day 15: Modern GRP2/GRP3 Parser Repair Slice
* **Modern GrpReader**: Fully implemented GRP2/GRP3 binary support in `GrpReader.cpp` to parse official 16-byte `GrpHeader` tables, patchable `GrpData` name-maps, and class IDs, with extensive diagnostic assertions.

### Day 16: GRP Export Guardrails & Stacked Layout Slice
* **Export Guardrails**: Extracted `classId` metadata (e.g. `RendInst`, `DynModel`, `Anim2Data`, `GeomNodeTree`) in GRP contents. Added `CanExportGrpEntryAsStaticModel` to block non-mesh GRP resource exports (such as skeletons or animation trees) up front, replacing low-level block assertion crashes with friendly tooltips.
* **UI Stacked Layout**: Rearranged panels into a modern center-stacked viewport layout (Inspector stacked directly above Content Browser) with class-type metadata columns in all lists.

### Day 17: Multi-Resource Descriptors & ImGui Hover Fix Slice
* **Multi-Resource Table**: Upgraded descriptor-side inspector to list all resolved resources inside the selected descriptor file (instead of stopping at the first match) with per-row export triggers and class metadata.
* **ImGui ID Conflict Resolution**: Fixed long-standing programmer warnings on hover by enclosing list render culls inside standard `ImGui::PushID(row)` and `ImGui::PopID()` scopes in both the virtualized Content Browser list and the Inspector Sub-Assets table.
* **Compilation Verification**: Resolved MSVC `LNK1168` file locks by terminating active background instances of the application (`DagorAssetsExtractor.exe`), compiling clean releases successfully.

### Day 18: Readable Texture Export Slice
* **Texture Format Settings**: Finished the user-facing texture export preference by adding a settings combo for `DDS` / `PNG` / `TGA`, wiring `preserveOriginalDDS` into `TextureExportOptions`, and persisting the optional `oodleDllPath` in `settings.ini`.
* **Readable DXP Export**: Extended `TextureExporter` to emit `.png` (via WIC) and `.tga` (via an internal writer) for supported uncompressed DDSx textures. Readable export currently targets the top mip only and gracefully falls back to `.dds` when the user requests `PNG` / `TGA` for unsupported formats and fallback is enabled.
* **Batch/Path Hygiene**: Stopped assuming `.dds` inside batch-export preflight checks for non-DDS requests, and kept all texture outputs on the existing sanitized DXP path builder.
* **Verification**: Added synthetic tests covering readable `.png` export, readable `.tga` export, and `PNG -> DDS` fallback behavior, then reran `scripts/run_tests.ps1` successfully.

### Day 19: BCn Readable Texture Export Slice
* **BCn Top-Mip Decode**: Extended readable `PNG` / `TGA` texture export to decode the top mip of common block-compressed DDSx textures in software: `DXT1/BC1`, `DXT3/BC2`, `DXT5/BC3`, `ATI1/BC4`, and `ATI2/BC5`, including DXGI and legacy FourCC variants.
* **Normal-Map Friendly Output**: Mapped `BC5` exports into BGRA output using the decoded red/green channels so common two-channel Dagor textures now produce readable files instead of immediately falling back to DDS.
* **UI Messaging Refresh**: Updated the settings/help text so the app now advertises readable export support for `BC1-BC5` plus the previously supported uncompressed formats.
* **Verification**: Added synthetic readable-export tests for `BC1 -> TGA` and `BC5 -> TGA`, updated the fallback coverage to use an unsupported `BC7` texture, and planned a fresh `scripts/run_tests.ps1` pass after the code changes.

### Day 20: Expandable UI Panels Slice
* **UI Expandability**: Made the Content Browser and Inspector panels vertically resizable inside the central stack panel. Implemented an interactive horizontal splitter bar between them, displaying a double-arrow drag cursor (`ResizeNS`) on hover.
* **Search Bar Width Refinement**: Configured the Content Browser's search input box to expand to the full available horizontal width (`ImGui::SetNextItemWidth(-1.0f)`), creating a balanced, modern UI layout.
* **Persistent UI State**: Declared an `inspectorHeight` member variable in the global `AppState` struct to persistently track the user's resized splitter state.
* **Safe Clamp Boundaries**: Added strict clamping guards to ensure that panel sizes remain within a safe range (minimum 100px for either panel), preventing visual overflow or out-of-bounds UI state.
* **Verification**: Compiled the entire solution and ran the synthetic test suite using `scripts/run_tests.ps1`, confirming that the new UI layout modifications compile flawlessly with zero warnings or errors.

### Day 21: Split Texture Support & DDSx Mip Count Clamping Slice
* **Split Texture Mip Clamping**: Resolved "DDS payload too small" export errors by dynamically calculating the actual number of mipmap levels present in the decompressed payload. When the texture size is smaller than expected due to split high-quality packages, the exporter now clamps `mipCount` to the number of mips actually present in the data rather than failing validation.
* **Compliant DDS Header Reconstruction**: Passed the clamped mip count to DDS container generation (`WriteDds`), yielding standard-compliant, fully openable DDS files with correct `mipMapCount` metadata.
* **Verification**: Compiled successfully with MSBuild (0 errors, 0 warnings) and passed all unit tests in `AssetIndexTests.exe`.

### Day 22: GeomNodeTree Skeleton Metadata Slice
* **DagorEngine Layout Verification**: Cross-checked the binary `GeomNodeTree` layout against Gaijin's open DagorEngine sources before coding. This confirmed the old-node serialized size, patchable pointer/tab encoding, and block-compression framing used by real skeleton payloads.
* **`GeomNodeTreeReader`**: Added a dedicated parser that reads `GeomNodeTree` payloads from GRP entries, supports raw and block-compressed payload containers (`zstd` and `Oodle`), decodes node names/parents/children/local transforms, and computes accumulated global rest transforms plus inverse bind matrices.
* **GRP Inspector Metadata**: Extended `PackContents` so `GeomNodeTree` entries now expose `skeletonNodeCount`, `skeletonImportantNodeCount`, and `skeletonRootName`, and summarize that data in the GRP sub-asset `formatInfo` string instead of showing only an opaque class label.
* **UI Messaging**: Updated the UI support text to explicitly advertise `GeomNodeTree` skeleton metadata parsing during GRP inspection.
* **Synthetic Coverage**: Added a dedicated `GeomNodeTreeReader` unit test plus a GRP metadata fixture test that verifies node hierarchy decoding, global transform accumulation, inverse bind values, and inspector metadata population.
* **Verification**: Rebuilt the solution and reran `powershell -ExecutionPolicy Bypass -File scripts/run_tests.ps1` successfully after fixing a test-only `std::span` lifetime bug caused by constructing `GeomNodeTreeReader` from a temporary vector.
* **Next Logical Slice**: Reuse the new skeleton reader when tackling `DynModel` parsing/export, especially for `JOINTS_0` / `WEIGHTS_0` extraction and glTF skin serialization. The dynamic mesh format itself is still not decoded in this slice.

### Day 23: Bundled Oodle & LZMA Texture Fallback Slice
* **Bundled Oodle Settings Cleanup**: Removed the user-facing Oodle DLL path and "use game folder" toggle from the settings model/UI. The app now resolves `oo2core_6_win64.dll` automatically from the bundled build output or repo `third_party` folder instead of persisting a user override in `settings.ini`.
* **Build Output Packaging**: Added a post-build copy step in the app project so `third_party\oo2core_6_win64.dll` is placed beside `DagorAssetsExtractor.exe`, matching the automatic texture-export lookup path used at runtime.
* **LZMA Raw Texture Fallback**: Updated `TextureExporter` so DDSx entries marked as `LZMA/7zip` no longer hard-fail export. They now reuse the existing raw `.ddsx` fallback path with manifest warnings, matching the current Oodle-missing behavior until true LZMA decode support is added.
* **Project Build Repair**: Fixed a pre-existing solution build issue by adding `DynModelReader.cpp/.h` to both Visual Studio projects. The implementation already existed in `src`, but the app project was not compiling/linking it.
* **Synthetic Coverage**: Added a dedicated `TextureExporter` test that verifies unsupported `LZMA/7zip` DDSx payloads export as raw `.ddsx` fallback files.
* **Verification**: Rebuilt the full solution and reran `powershell -ExecutionPolicy Bypass -File scripts/run_tests.ps1` successfully. Result: 0 build errors and `AssetIndexTests passed`.

### Day 24: Skinned glTF Model Export Slice
* **`DynModelReader`**: Implemented parsing for dynamic meshes (`DYN2`/`DYN3` magics), correctly extracting the skinned vertex buffer layout containing 3D positions, normals, UVs, 4x `uint8_t` joint indices, and 4x `float` joint weights per vertex.
* **Descriptor Skeleton Resolution**: Enhanced `GameResDescReader` to parse `skeleton` and `dynmodel` parameter names from binary BLK blocks. Updated `AppState::RequestModelExport` to dynamically locate and parse the associated `GeomNodeTree` from GRP containers when a skinned model is requested.
* **Skinned glTF Serialization**: Extended `ModelExporter` with an overload for skinned meshes. Automatically constructs the glTF node hierarchy from the skeleton bones, computes and embeds `inverseBindMatrices`, writes `JOINTS_0` and `WEIGHTS_0` vertex attributes, and binds the mesh to the skeleton using a `skin` node.
* **Inspector UI Integration**: Updated `DependencyGraph` and `PackContents` to classify `DynModel` resources as exportable skinned models. Enabled the "Export Model" buttons across both the Descriptor Inspector and GRP Sub-Assets table for these resources.
* **Synthetic Coverage**: Created a synthetic `DynModel` fixture test in `AssetIndexTests` to verify vertex weight parsing and valid glTF skin chunk serialization.
* **Verification**: Rebuilt the solution and ran `scripts/run_tests.ps1` successfully, passing the new skinned export suite.

### Day 25: Float Texture Export & Mip Regression Slice
* **Readable Float Texture Export**: Extended `TextureExporter` readable `PNG` / `TGA` output to decode `R16G16B16A16_FLOAT` and `R32G32B32A32_FLOAT` DDSx top mips into 8-bit BGRA output using local half-float / float conversion and clamped channel normalization.
* **Batch Skip Semantics**: Fixed `ExportAllTextures(..., overwrite=false)` so existing readable-output targets now count as skipped instead of failed, aligning batch `PNG` / `TGA` behavior with the existing DDS path.       
* **Mip Regression Coverage**: Added synthetic texture tests for reverse-mip-order DDSx normalization and split/high-quality mip clamping, including validation of parsed DDSx quality fields and the final DDS `mipMapCount`.  
* **Readable Coverage Expansion**: Added synthetic readable-export tests for `RGBA16F -> TGA`, `RGBA32F -> TGA`, and batch readable-PNG skip behavior.
* **Build Repair**: Added the missing `BlkReader::ParseBinary()` definition so the current tree links cleanly again while packed-binary BLK parsing remains explicitly unsupported.
* **Verification**: Rebuilt the full solution and reran `powershell -ExecutionPolicy Bypass -File scripts/run_tests.ps1` successfully. Result: 0 build errors and `AssetIndexTests passed`.

### Day 26: Generic Binary BLK Parser Slice
* **`Binary BLK Parser`**: Implemented `ParseBinary` in `BlkReader.cpp` to correctly decode binary/packed BLK files (`kBinaryBlkPayloadMagic = 0x01` and `0x00`).
* **ZSTD Dictionary support foundation**: Added ZSTD decompression step for `0xFD2FB528` magic blocks, laying the groundwork for dictionary decompression in the future.
* **Primitive Types Decoder**: Implemented full VLQ-based decoding for all 12 Dagor primitive BLK parameter types, including Outline types (Strings, Arrays, Matrices) mapped via `paramsData`, and Inline types (Int, Float, Bool, Color).
* **`BlkProperty` Integration**: Mapped the decoded binary structures to the existing text-based `BlkProperty` and `BlkBlock` structures so the UI and rest of the app work flawlessly with both text and binary configurations.
* **Synthetic Coverage**: Added `TestBinaryBlkReader` in `AssetIndexTests/main.cpp` using a synthetic binary payload constructed with `AppendVlq`.
* **Verification**: Compiled the solution and successfully passed the test suite using `scripts/run_tests.ps1`.

### Day 27: ShaderMeshResource Header Phase 1 Slice
* **Reference Alignment**: Cross-checked the `Dagor-Asset-Explorer` Python `RendInst` / `DynModel` `_readHeader` path and limited this slice to the pre-`MatVData` metadata layout.
* **`DynModelReader` Phase 1**: Updated real GRP `DynModel` payload handling so the decompressed `ShaderMeshResource` data is wrapped in `DagorIO::MemoryReader` and parsed for `lodHdrInfo`, derived LOD header size, RI struct size, texture count, material count, vertex-data count, and packed MVD header flags.
* **Texture Name Map Guardrails**: Added bounded parsing for inline texture name maps using the Python-compatible offset-table layout, while safely skipping descriptor-backed `0xFFFFFFFF` counts and zero-texture resources.
* **Intentional Stop Point**: Removed the hard-coded decompressed payload dump and replaced the generic unsupported-format throw with a Phase 1 diagnostic log followed by `Phase 1 complete: MatVData parsing is required next.` so later vertex/index decoding remains a separate slice.
* **Verification**: Rebuilt the full solution and reran `powershell -ExecutionPolicy Bypass -File scripts\run_tests.ps1` successfully. Result: 0 build warnings, 0 build errors, and `AssetIndexTests passed`.

### Day 28: ShaderMeshResource MatVData and Geometry Alignment Slice
* **Phase 1 Header Correction**: Corrected `RendInstReader` and `DynModelReader` to parse the Phase 1 Header from the raw sub-asset payload `m_data` (where it sits uncompressed at offset 0) rather than the decompressed stream (which contains only the `MatVData` payload).
* **MatVData Struct Alignment**: Aligned the starting offset `mvdStartPos` of the `MatVData` reader inside `m_decompressedPayload`. When compression is used, `MatVData` starts at offset 0 of the decompressed payload.
* **Header and Stream Reading**: Read `matOfs` (offset 0), `matCnt` (offset 4), skipped 8 bytes, and read `mvdGvdOfs` (offset 16) and `mvdGvdCnt` (offset 20) of the decompressed stream. This correctly maps to GVD blocks, fixing the zero LOD/GVD count bug for all static and skinned models.
* **Verification**: Rebuilt the solution and successfully passed the test suite, confirming 100% success on all Model Diagnostics for `af_chimney_metal_a` and `coal_cart`.

### Day 29: Format 13 Support & Skinned glTF Exporter Repair Slice
* **Format 13 Decoders**: Added support for stride 20 and stride 24 buffers under storage format 13 in both `DynModelReader.cpp` and `RendInstReader.cpp`, resolving parse failure crashes on aircraft/vehicles.
* **glTF Joint Index Alignment**: Fixed "s.bones[d] is undefined" loading errors and visual deformations in exported skinned model GLB files. Pre-calculated dynamic accessor index maps (`indexAccessorIndices`) for multi-primitive index accessors and routed `inverseBindMatrices` to the correct dynamic index rather than a hardcoded layout.
* **Verification**: Rebuilt the full solution and ran `scripts/run_tests.ps1` successfully, passing all unit tests with 0 build errors and 0 warnings.

### Day 30: GRP2/GRP3 Packed Index Loop Fix Slice
* **Packed Index Decode Correction**: Identified and resolved a core bug where the index decoder in `DynModelReader.cpp` and `RendInstReader.cpp` stopped prematurely after decoding `gvd.iCnt` indices. Because `gvd.iCnt` is often a fixed low value (e.g. 32) instead of the actual index count, this resulted in sparse-polygon mesh geometry.
* **Stream-End Loop Guard**: Corrected the packed index decoder to process LEB128/delta compressed index streams up to the calculated boundary of the compressed index block (`streamEnd = blockStartPos + vbSz + gvd.iPackedSz - 4`), matching the Python reference implementation.
* **Verification**: Rebuilt the solution and successfully ran `scripts/run_tests.ps1` with 0 compiler warnings/errors and all tests passing, verifying correct index counts (e.g. 751,641 indices decoded for `ab_205a_1` instead of 32).

### Day 31: DynModel Rigid Transform and glTF Bounds Repair Slice
* **Same-Pack Skeleton Resolution**: Updated DynModel export to try `<dynmodel>_skeleton` inside the same GRP before falling back to descriptor/global searches. This resolves cockpit/aircraft skeletons that are packed beside the DynModel but are not discoverable through descriptor metadata.
* **Rigid Mesh Scene Transforms**: Changed rigid DynModel GLB export so mesh nodes receive their matched `GeomNodeTree` world transform directly. Non-skinned models no longer emit a separate unused skeleton hierarchy, avoiding misplaced split parts when imported into Blender or web glTF viewers.
* **Exact Accessor Bounds JSON**: Replaced default stream float formatting for accessor min/max and node matrices with exact float32-promoted JSON output. This fixes Khronos validator `ACCESSOR_MAX_MISMATCH` and `ACCESSOR_ELEMENT_OUT_OF_MAX_BOUND` errors caused by rounded JSON values.
* **Verification**: Rebuilt with `scripts/run_tests.ps1` successfully. Regenerated `f_15c_golden_eagle_cockpit_smoke.glb` from the real War Thunder GRP and verified 673/673 mesh nodes have matrices, 0 position accessor bound mismatches, and 0 index-bound errors with a local GLB JSON/BIN validation script.

### Day 32: DynModel Export Hardening and Material Slot Slice
* **Material Slot Fallbacks**: Updated GLB export so primitives keep their parsed material slot indices even when descriptor textures are missing or texture export is disabled. Textureless slots now emit valid glTF materials without empty image URIs.
* **DynModel Texture Dependency Wiring**: Improved descriptor-to-GRP matching by using `dynModelName` as a GRP lookup candidate. Model export now creates material slots from descriptor material names, exports sidecar textures when the setting is enabled, and writes GLB texture URIs relative to the export root.
* **Normal Sanitization**: Added export-time normal repair for both static and DynModel GLBs. Invalid, zero, non-finite, and non-unit normals are normalized or rebuilt from triangle faces, with a final unit fallback to avoid Khronos `ACCESSOR_VECTOR3_NON_UNIT` errors.
* **Optional Skinned Tail Guardrails**: Changed optional DynModel skinned-resource parsing to log and skip unsupported/malformed skinned tails instead of failing the entire model export. Main rigid geometry remains exportable, with diagnostics including skinned-tail offsets and first bytes.
* **Verification**: Rebuilt with `scripts/run_tests.ps1` successfully and added regression coverage for textureless material slots plus normal repair. Real smoke exports for `f_15c_golden_eagle_cockpit` and `battleship_mississippi` both succeeded; local GLB validation found 0 position-bound errors, 0 normal unit errors, and 0 index-bound errors for both generated files.
