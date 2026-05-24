# Dagor Assets Extractor Project Plan

## Goal

Build a lightweight Windows desktop tool for War Thunder modders and artists to browse local Dagor resource packs, inspect assets, and export models/textures into common creation-friendly formats.

The tool should:

- Ask the user for the War Thunder game location on first run.
- Save the game location in editable settings.
- Load a content-browser style view of Dagor resource packs and discovered assets.
- Provide fast search, filtering, icons, previews, and batch export.
- Export textures to common formats, providing an application setting to select the preferred format (such as DDS, PNG, or TGA).
- Export models as accurately as possible, including materials, texture references, skeletons, and eventually animations where the Dagor data can be decoded reliably.
- Remain read-only against the game install and avoid heavy full-unpack workflows unless the user explicitly exports something.

## Current Repository Baseline

The current repository is a standalone C++20 Windows application:

- Visual Studio solution: `DagorAssetsExtractor.sln`
- App project: `DagorAssetsExtractorApp/DagorAssetsExtractor.vcxproj`
- UI/rendering baseline: `DagorAssetsExtractorApp/src/main.cpp`
- Vendored UI dependency: `third_party/imgui`

The app currently contains only a Win32 + D3D11 + Dear ImGui test window. There is no existing Dagor parser, settings system, content browser, exporter, file index, preview renderer, or background job system.

Important scope note: the repo instructions describe an Unreal Engine plugin, but the actual project is currently a standalone ImGui/D3D11 app. This plan assumes the standalone app direction because the requested UI is ImGui and the existing code already matches that. If this must become an Unreal Engine plugin later, that should be treated as a separate architecture decision before implementation.

## Technical Research & Binary Format Specifications

To build a flawless and robust C++ extractor, the system architecture and decoders must implement the proprietary Dagor Engine specifications collected during format research.

### 1. VROMFS Virtual File System
The Virtual Read-Only Memory File System (`.vromfs.bin`) stores configuration, logical rules, and scripts. It exists in 5 structural variants dictating how string tables and metadata are compiled:

| Variant ID | Format Designation | Architectural Description |
| :--- | :--- | :--- |
| `0x01` | **FAT** | Uncompressed, self-contained standalone binary archive with inline string tables and payloads. |
| `0x02` | **FAT_ZST** | Structurally identical to FAT, but payloads are compressed using standard Zstandard (ZSTD) algorithms. |
| `0x03` | **SLIM** | Human-readable paths and keys are stripped and outlined into a global Name Map (`nm`) file at the virtual root. |
| `0x04` | **SLIM_ZST** | Highly optimized distribution form of the SLIM format, compressed using Zstandard. |
| `0x05` | **SLIM_ZSTD_DICT**| SLIM format compressed utilizing a pre-trained Zstandard dictionary to maximize redundancy elimination. |

#### ULEB128 & Name Map Resolution (Slim Variants)
In SLIM formats, string identifiers are replaced with sequential integer indices. Decoders must parse the global Name Map (`nm`) first:
1. Strings are packed in a contiguous null-terminated character array.
2. The `nm` file header utilizes **Unsigned Little Endian Base 128 (ULEB128)** encoding for sizes (number of strings and total size in bytes).
3. The ULEB128 decoding algorithm loops over bytes: extracts the lower 7 bits (`byte & 0x7F`) and shifts them into a 32-bit or 64-bit accumulator. It terminates when a byte's MSB is `0` (`(byte & 0x80) == 0`).
4. Read strings into a persistent `std::vector<std::string>` in heap memory. Subsequent headers use these indices to resolve file names and property keys.

---

### 2. DataBlock (BLK) Configuration Graphs
Dagor uses compiled binary Block (`.blk`) files for scene trees, dependencies, and settings. It is structured as a recursive acyclic node graph.

#### Binary BLK Primitive Types
The binary BLK format supports 12 primitive data types identified by a 1-byte code:

| Byte ID | Type Name | C++ Memory Layout | Payload Size | Storage Paradigm |
| :--- | :--- | :--- | :--- | :--- |
| `0x01` | **String** | Zero-terminated char array | Variable | Outline (Offset-based) |
| `0x02` | **Int** | `int32_t` | 4 bytes | Inline |
| `0x03` | **Float** | `float` | 4 bytes | Inline |
| `0x04` | **Float2** | `float[2]` | 8 bytes | Outline |
| `0x05` | **Float3** | `float[3]` | 12 bytes | Outline |
| `0x06` | **Float4** | `float[4]` | 16 bytes | Outline |
| `0x07` | **Int2** | `int32_t[2]` | 8 bytes | Outline |
| `0x08` | **Int3** | `int32_t[3]` | 12 bytes | Outline |
| `0x09` | **Bool** | `uint8_t` (padded to 4) | 4 bytes | Inline |
| `0x0a` | **Color** | `uint8_t[4]` (RGBA) | 4 bytes | Inline |
| `0x0b` | **Float12**| `float[3][4]` (Matrix) | 48 bytes | Outline |
| `0x0c` | **Long** | `int64_t` | 8 bytes | Outline |

#### Inline vs. Outline Storage & Dictionary Compression
*   **Inline Types** (`0x02`, `0x03`, `0x09`, `0x0a`): The 4-byte value is stored directly in the parameter block structure.
*   **Outline Types** (Strings, Matrices, Vectors, Int arrays): The parameter block stores a 4-byte relative offset referencing a contiguous data segment located at the end of the file.
*   **Zstd Dictionary updates (2.7.0.58+)**: Modern client updates compress BLK parameter structures via pre-trained ZSTD dictionaries. Parsers must maintain a decompression state that injects the dictionary before reading parameter blocks.

#### Memory Safety & Validation
Standard parser functions (`loadFromBinDump`) are vulnerable to buffer overflows when reading invalid headers or malformed offsets. C++ implementation MUST:
1. Enforce strict boundary validation: check that `offset + size` is within bounds of the decompressed BLK buffer before reading.
2. Utilize C++20 `std::span` to wrap mapped parameter arrays securely.

---

### 3. Texture Packs (DXP) & DDSx Formats
Textures are packed into DXP archives (`.dxp.bin`) as wrapped DDSx records. Standard image editors cannot open raw DDSx files; they must be reconstructed into standard Microsoft DDS containers or transcoded to common formats (such as PNG or TGA) based on the user-selected export format setting.

#### DDSx Header Layout
DDSx files begin with a 32-byte header:
*   `0x00 - 0x03`: Magic ID (`DDSx` or custom variant).
*   `0x04 - 0x1B`: Metadata (width, height, mip levels, format flags).
*   `0x1C`: `packedSz` (packed size on disk).
*   **Compression Type**: Extracted from the high bits/high byte of the `flags` field:
    *   `0x00`: Uncompressed (None)
    *   `0x20`: **zstd** (libzstd decompression)
    *   `0x60`: **Oodle** (dynamic link to `oo2core_6_win64.dll` to call `OodleLZ_Decompress`)
    *   `0x80`: **zlib** (inflate/tinflate decompression)
    *   `0x40`: **LZMA** (pylzma decompression)

#### Mip-Map Reordering (`FLG_REV_MIP_ORDER`)
If the DDSx flags contain the reverse mip order flag, mips are written on disk in reverse order (smallest mip first, progressing to the largest high-resolution mip). The exporter **must reverse** this buffer array back into standard DDS layout (largest mip first, progressing down to 1x1) to yield readable DDS files.

#### Microsoft DDS Reconstruction
Reconstructed textures prepend a 128-byte Microsoft DDS header. Modern texture formats (BC6H, BC7) require writing the standard **DX10 Extended Header** (`DdsHeaderDX10`), whereas legacy formats (DXT1, DXT5, ATI1, ATI2) write direct FourCC descriptors.

---

### 4. Static Geometry (DAG / RIND)
Static geometry is saved in the proprietary `.dag` or compiled `RIND` (RendInst) format, featuring descending Levels of Detail (LODs):
*   **Vertex Buffers**: Use variable strides based on mesh vertex features. Coordinates include XYZ positional floats, normals, tangents, and multiple UV coordinate streams (diffuse, damage decals, or ambient light maps).
*   **Index Buffers**: Packed as 16-bit or 32-bit indices mapping to strictly triangulated faces.
*   **Material Slots**: Geometry files store names of materials utilizing `DagorMat2`. The extractor resolves textures by parsing related `.folder.blk` or `.dynmodel.blk` rules to match material shaders and texture slot assignments back to DXP pack entries.

---

### 5. Dynamic Models, Skeletal Hierarchies & Kinematics
Dynamic skinned assets (e.g., vehicles with rotating turrets, character models, aircraft) are packed inside Game Resource Packs (GRP) and managed via DynModel systems:
*   **GRP Macro-Containers**: Extracting a GRP yields separate records: `dynmodel` (mesh geometry), `skeleton` (hierarchical nodes), `phobj` (colliders and mass properties), and `fastphys` (kinematic solvers).
*   **Skeletal Trees (`GeomNodeTree`)**: Skeletons are stored as arrays of bone joint structures. Each node contains a string index, a parent index, and a local transform (translation vector and rotation quaternion). The global rest-pose is reconstructed by recursively multiplying local transforms from the root down to the leaves:
    $$T_{\text{global}} = T_{\text{parent\_global}} \times T_{\text{local}}$$
*   **Skinned Mesh Deformations**: The `dynmodel` vertex buffer contains blend weights (up to four bones per vertex, float values) and blend indices (mapping directly to joint indices in the skeleton tree). To export to glTF 2.0, the exporter must reconstruct the **Inverse Bind Matrices (IBM)** for each joint by inverting the global bind transform:
    $$\text{IBM} = T_{\text{global}}^{-1}$$
*   **Animation Tracks (`AnimChar` / `.a2d`)**: Animation tracks are packed timeline keyframes representing translation vectors and rotation quaternions per joint. To play smoothly, the exporter maps keyframes onto glTF animation channels, using **Spherical Linear Interpolation (Slerp)** to interpolate rotations between keyframes, and standard Linear Interpolation (Lerp) for translations.

---

## Codebase System Architecture & Core Modules

The current codebase is implemented as a standalone Windows desktop application utilizing C++20, DirectX 11, and Dear ImGui. The physical directory structure and class modules map as follows:

```txt
DagorAssetsExtractorApp/
  src/
    App/
      AppState.h/cpp       # Lifecycle, configuration, Oodle DLL loader, scan logic
    Platform/
      Platform.h/cpp       # Win32 file pickers, path traversal checks, UTF-8 strings
    UI/
      UI.h/cpp             # Docked ImGui UI, list clippers, flat & group-by-BLK views
    Jobs/
      JobSystem.h/cpp      # Thread-safe multi-worker background job queue
    DagorIO/
      MemoryReader.h/cpp   # Safe, endian-aware stream mapping and reading
    DagorFormats/
      VromfsReader.h/cpp   # Reads uncompressed VRFx containers
      DxpReader.h/cpp      # Reads texture packs and resolves DDSx records
      GrpReader.h/cpp      # Reads GRP containers with dynamic table finders (GRP2 support)
      DdsxReader.h/cpp     # Parses DDSx headers and handles decompression types
      BlkReader.h/cpp      # Text-based BLK property and comment parser
      RendInstReader.h/cpp # Parses RIND binary static mesh buffers and LOD material ranges
      GameResDescReader.h/cpp # Reads riDesc/dynModelDesc and parses binary BLK name maps
    AssetIndex/
      AssetIndex.h/cpp     # Scans directories, maps index-cache, maintains registry
      DependencyGraph.h/cpp # Resolves BLK pack links and descriptor resource relationships
    Export/
      ExportManifest.h/cpp # Emits .export-manifest.json records on every export
      TextureExporter.h/cpp # DDS reconstruction, DX10 BC7 structures, mip reversing
      ModelExporter.h/cpp   # Converts RIND meshes to GLB (glTF 2.0) and Wavefront OBJ
```

### Safety Cast Struct Declarations
Parsers cast memory-mapped file buffers directly into strictly aligned headers. Endian-safety checks and size bounds validations are run before pointer dereferencing:

```cpp
#pragma pack(push, 1)
struct DDSxHeader {
    uint32_t magic;      // 'DDSx'
    uint32_t format;     // DDS pixel format token (FourCC or DX10 marker)
    uint32_t width;      // Image width in pixels
    uint32_t height;     // Image height in pixels
    uint32_t mipLevels;  // Total mip levels stored
    uint32_t flags;      // Decompression type (high bits) and mip ordering flags
    uint32_t packedSz;   // Length of compressed data payload
    uint32_t dummy;      // Padding byte
};

struct BlkParamHeader {
    uint32_t nameId;     // Index referencing the Name Map
    uint8_t  typeId;     // Primitive type enum (0x01 to 0x0C)
    uint32_t payload;    // Inline data value or relative offset to outline data
};
#pragma pack(pop)
```

---

## Implementation Roadmap & Milestones

### Milestone 0: Scope & Legal Safeguards (COMPLETED)
*   **Deliverables**: Set standalone C++ desktop direction using DirectX 11 + Dear ImGui. Defined legal boundaries: read-only local game access, no process manipulation, zero redistribution of War Thunder assets or proprietary Oodle DLL files.

### Milestone 1: App Foundation & Settings (COMPLETED)
*   **Deliverables**: Created App shell UI, integrated Win32 folder selection, and built persistent `settings.ini` storage. Added background logging and UI consoles.

### Milestone 2: Resource Discovery & Scanner (COMPLETED)
*   **Deliverables**: Built folder scanner detecting valid game markers (`aces.exe`, `content/base/res/`). Implemented local `%APPDATA%` index cache (`index-cache.txt`) to skip scanning unless files change. Implemented thread-safe `PackContentsCache` with 256MB lazy-parsing limits.

### Milestone 3: Texture Decompression & Export (COMPLETED)
*   **Deliverables**: Created DDSx format parsing and DDS container reconstruction. Added zstd, zlib, and dynamic Oodle (`oo2core_6_win64.dll`) decompression. Enabled legacy FourCC DDS header generation and modern DX10 Extended (`DdsHeaderDX10`) formats for BC6H/BC7 texture layers. Implemented `FLG_REV_MIP_ORDER` reordering, reversing the array to largest-mip-first.
*   **Texture Export Format Options (PENDING)**: Add a UI selector in the settings panel to allow modders to configure their preferred export format (DDS, PNG, or TGA). Integrate transcoding capabilities to convert raw decompressed DDS pixels into standard RGBA images for PNG/TGA export.

### Milestone 4: Static Model Pipeline (COMPLETED)
*   **Deliverables**: Decoded `RIND` binary geometry. Created `ModelExporter` converting meshes into standard Wavefront OBJ and binary glTF 2.0 (`.glb`) files supporting nested LOD nodes.
*   **Advanced Resource Graph**: Created `DependencyGraph` module. Decoded `riDesc.bin`/`dynModelDesc.bin` files to parse ULEB128 name maps and material slot blocks. Resolved descriptor names directly back to GRP assets, and matched material texture slots to DXP packs.
*   **Multi-Material & UI Integration**: Enabled exporting complex static meshes with multiple glTF primitives and materials linked directly to their textures. Integrated direct "Export Unified Model" actions in the ImGui inspector.

### Milestone 5: Dynamic Models & Skeletal Hierarchies (COMPLETED)
*   **Technical Tasks**:
    1.  **GRP Dynmodel Parsing**: Extend GRP container parsers to parse dynamic mesh structures (`dynmodel` records), extracting skinned vertex arrays.
    2.  **GeomNodeTree skeleton parsing**: Parse the binary skeletal tree structure from GRPs. Decode the node name map indices, parent bone mappings, and 3x4 local rest-pose matrix transformations.
    3.  **Bone Coordinate Accumulation**: Implement recursive skeleton tree traversal down parent-child chains to compute absolute joint coordinates in space:
        $$T_{\text{global}} = T_{\text{parent\_global}} \times T_{\text{local}}$$
    4.  **Inverse Bind Matrices (IBM)**: Compute the inverse bind matrix for each skeleton joint:
        $$\text{IBM} = T_{\text{global}}^{-1}$$
        Export the IBM array as a binary float float array stored in the glTF skin's buffer.
    5.  **Skinned Weight Bindings**: Parse skinned vertex buffers, extracting up to 4 joint indices (`uint8_t`) and normalized blend weights (`float`) per vertex. Serialize these inputs into glTF vertex attributes (`JOINTS_0` and `WEIGHTS_0`).
    6.  **Skeletal glTF Node Serialization**: Write the joint bone nodes as a hierarchical tree of glTF nodes. Add a `skin` parameter in the mesh's node pointing to the skin definition to bind the armature.
*   **Acceptance Criteria**:
    *   Exported dynamic assets (.glb) import successfully into Blender.
    *   Skeletal bones display in a proper hierarchical tree (armature).
    *   Mesh surfaces deform stably in the bind pose with zero vertices collapsing or exploding.

### Milestone 6: Animation Extraction & Keyframe Tracks (PENDING)
*   **Technical Tasks**:
    1.  **AnimChar & `.a2d` Parsing**: Create a parser for dynamic keyframe collections. Read animation file headers, duration parameters, and keyframe sample ticks.
    2.  **Kinematic Keyframe Extraction**: Decompress translation tracks and rotation tracks (stored as quaternions) for active joints.
    3.  **Animation Channel Mapping**: Correlate joint names in the animation tracks to joint nodes defined in the skeletal `GeomNodeTree`.
    4.  **glTF Animation Serialization**: Export tracks into standard glTF `animations` arrays:
        *   Input accessors: Keyframe times (`float`).
        *   Output accessors: Translation vectors (`vec3`) and Rotation quaternions (`vec4`).
        *   Interpolation tags: Specify LERP for translations, and **SLERP** (Spherical Linear Interpolation) for quaternions to guarantee smooth curves.
*   **Acceptance Criteria**:
    *   Exported glb assets display playable animation tracks upon import into Blender.
    *   Joint actions play at correct speeds without stuttering or out-of-bounds scale scaling.
    *   Unlinked or unsupported formats report diagnostics gracefully instead of causing app crashes.

### Milestone 7: Performance Polish, Virtualization & Release (PENDING)
*   **Technical Tasks**:
    1.  **Virtualized Content Browser**: Ensure the ImGui layout remains highly responsive by utilizing `ImGuiListClipper` on lists exceeding 100,000 assets.
    2.  **Multithreading Batch Exports**: Optimize the `JobSystem` to concurrently compress and write models/textures. Ensure thread pools are bounded to hardware concurrency.
    3.  **Heap & Bounds Verification**: Compile final binary releases with address sanitizers enabled to check that all memory-mapped stream readers are completely leak-free and secure. Expose a portable release bundle containing `settings.ini`.
*   **Acceptance Criteria**:
    *   App startup requires less than 2 seconds when reading cached indexing records.
    *   UI operates consistently at a smooth 60 FPS while background search indexing is active.
    *   A clean portable binary runs stably on target Windows rigs without dependencies.

---

## Comprehensive Validation & Verification Plan

1.  **Binary Parser Unit Tests**: Expose test frameworks utilizing synthetic mock files (under `tests/fixtures/`) simulating VROMFS containers, GRP headers, binary BLK entries, DDSx structures, and RIND vertex strides. Run automated checks via `run_tests.ps1`.
2.  **Import Smoke Checks**: Verify that exported model formats (.glb, .obj) pass Blender importer command-line smoke tests.
3.  **Visual Delta Testing**: Reconstruct DDS files and compare visual channels (albedo, normals, gamma ranges) against standard reference tool outputs (`wt-tools`).
4.  **Boundary & Fuzz Resiliency**: Supply truncated files, negative payload size variables, and out-of-bounds offsets into reader streams to verify that the validation safeguards throw clean errors without causing memory crashes.

---

## Risks & Mitigations

| Risk | Technical Impact | Mitigating Strategy |
| :--- | :--- | :--- |
| **New Game Updates Break Headers**| Binary formats shift during client updates, crashing hardcoded parsers. | Include comprehensive diagnostics and log file mismatches. Rely on dynamic heuristics (like GrpReader's table finder) instead of fixed struct sizes. |
| **Complex Math in Animation Parsing**| Mismapped coordinate systems cause rotated or distorted joints. | Implement recursive global coordinate multiplication. Export raw channels to sidecar JSON files for developer debugging. |
| **Oodle DLL Version Conflicts**| Missing or mismatched external DLLs prevent texture decompression. | Detect client DLL installations recursively in game subdirectories. Support raw fallback export of DDSx files if missing. |
| **Excessive Memory Footprint**| Parsing massive 2GB game packages causes memory exhaustion. | Implement memory-mapped file access (mmap/MapViewOfFile) instead of buffering. Restrict parsing to lazy demand queries. |

---

## Open Technical Decisions

1.  **glTF vs. glTF Embedded (.glb)**: glTF embedded binary files (`.glb`) are selected as the primary output target since they store mesh buffers, textures, skeletal joints, and animations inside a single file, eliminating missing relative-path errors.
2.  **DirectXTex Adoption**: Evaluate DirectXTex integration post Milestone 5 to handle complex hardware-backed conversions if WIC-based libraries are insufficient. Especially useful to transcode BC/DXT textures to uncompressed formats when PNG/TGA formats are chosen.
3.  **Metadata Extras**: Maintain the choice to export raw Dagor layout parameters into glTF `extras` fields, ensuring modders can read custom vehicle nodes directly.
4.  **Texture Export Format Selection**: Implement a configurable dropdown in the App Settings panel allowing the user to select the preferred export format for textures (DDS, PNG, or TGA).

---

## References & Sources Used

*   **Gaijin Entertainment Dagor Engine Source**: Direct API reference logs for DataBlock serialization (`blk_core.cpp`), GameRes parsing rules, and GeomNodeTree skeletal structures.
*   **wt-tools Repository (`klensy/wt-tools`)**: Reference Python code for VROMFS, DXP, DDSx, and BLK decoders.
*   **Dagor-Asset-Explorer (`quentin-dh/Dagor-Asset-Explorer`)**: Blueprint examples for GRP containers, DAG static models, dynamic joint matrices, and DMF skeletal hierarchies.
*   **glTF 2.0 Specification**: Format rules governing skinned meshes, joint hierarchy maps, inverse bind matrices, and keyframe translation/rotation animation loops.
*   **Microsoft DirectXTex**: Reference for DirectDraw surface formatting, DX10 extended headers, and BC6H/BC7 conversions.
