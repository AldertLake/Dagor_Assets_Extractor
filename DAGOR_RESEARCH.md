# **Architecture and Parsing Methodologies for Dagor Engine Resource Containers**

## **Introduction to the Dagor Engine Ecosystem**

The Dagor Engine, engineered and maintained by Gaijin Entertainment, operates as the foundational architectural framework powering prominent simulation and combat titles such as War Thunder, Enlisted, and Cuisine Royale.1 Over its extensive iterative history, the engine has cultivated a highly specialized, proprietary asset serialization pipeline. This pipeline is intrinsically designed to optimize runtime memory allocation, minimize network distribution payloads for frequent updates, and accelerate disk input/output operations during level streaming.1 The recent partial open-sourcing of the Dagor Engine and its associated toolchain has provided unprecedented visibility into its internal mechanics. However, constructing a native C++ utility capable of autonomously extracting, interpreting, and exporting 3D models, skeletal hierarchies, encoded textures, and animation tracks requires a profound comprehension of its deeply layered container formats.  
The resource ecosystem of the Dagor Engine is fundamentally data-driven, relying heavily upon an Entity Component System (ECS) architecture.1 Within this paradigm, game logic, physical parameters, and visual properties are strictly decoupled from the executable binaries. Instead, they are customized and instantiated via intricate configuration files that act as logical pointers to binary geometry and texture payloads.1 Consequently, extracting a complete, render-ready 3D asset with its associated kinematic rig and high-resolution textures is not a simple file conversion task. It is an exercise in traversing a highly interconnected web of virtual file systems, serialized binary blocks, and nested resource packs.5 This report details the structural paradigms, compression algorithms, and binary layouts of the primary Dagor Engine resource containers, providing a comprehensive, expert-level blueprint for developing a robust C++ parsing and extraction toolchain.

## **The Dagor Asset Compilation Pipeline**

To effectively reverse-engineer and extract the proprietary formats utilized by War Thunder, a C++ developer must first understand the forward compilation pipeline established by the engine's official toolset. The Dagor Engine relies on an extensive suite of Python scripts and compiled utility binaries to transform raw developmental assets into optimized, encrypted game resources.  
The primary orchestration script, build\_all.py, is utilized to compile the toolkit from the source code, generating the necessary utilities such as the shader compiler, the VROMFS packer, and the dabuild executable.3 The dabuild utility is responsible for taking raw assets located in development directories and packaging them into Game Resource Packs (GRP) and Texture Packs (DXP), which are then staged for the virtual file system.1 Game logic, location data, scripts, and vehicle configurations are built into Virtual Read-Only Memory File System (VROMFS) dumps.1  
A critical aspect of this pipeline is the implementation of data integrity checks. VROMFS archives are frequently signed, and the engine validates these cryptographic signatures against internal keys during the load sequence to ensure that the data has not been maliciously altered or corrupted.1 Furthermore, the compilation process aggressively minimizes entropy. By consolidating redundant strings into global dictionaries and utilizing advanced compression algorithms, the pipeline produces binaries that are highly interdependent. A custom C++ extraction tool must essentially simulate the Dagor Engine's internal resource manager, replicating the memory mapping and cross-referencing logic that the engine natively performs during runtime.

## **VROMFS Virtual File System Mechanics**

The Virtual Read-Only Memory File System (VROMFS) serves as the bedrock container format for the Dagor Engine.1 Rather than distributing thousands of loose configuration files and discrete scripts, assets are compiled into cohesive .vromfs.bin dumps (e.g., aces.vromfs.bin, char.vromfs.bin, regional.vromfs.bin).8 This architecture expedites loading times by allowing the engine to mount the archive directly into memory and perform rapid sequential reads.1

### **Structural Variants and Compression Entropy**

Historically, the VROMFS architecture relied on standalone archives that were heavily compressed using variations of the Lempel-Ziv-Markov chain algorithm (LZMA).8 Older versions of the format utilized a modified LZMA header and tail, alongside a specific offset, necessitating custom decompression logic or specific tools like vromfs\_unpack.py to handle zlib.error: Error \-3 while decompressing data: incorrect header check exceptions.8  
However, as the engine evolved to support the massive scale of modern client updates, the format underwent a significant architectural shift. The pipeline transitioned toward utilizing Zstandard (ZSTD) compression and sophisticated dictionary-based encoding.12 This transition indicates a strategic emphasis on delta-patching efficiency and rapid decompression throughput. By reducing string duplication across binary files, the engine minimizes the entropy of the overall package. The contemporary VROMFS format exists in five primary structural variants, which dictate how the internal file tables and metadata are resolved :

| Variant Identifier | Format Designation | Architectural Description |
| :---- | :---- | :---- |
| 0x01 | FAT | A self-contained, uncompressed (or minimally compressed) standalone binary archive containing all necessary string tables and payload data inline. |
| 0x02 | FAT\_ZST | Structurally identical to the FAT format, but the internal payload is aggressively compressed using the Zstandard algorithm. |
| 0x03 | SLIM | An optimized binary format where all internal strings (paths, keys, identifiers) are stripped from the payload and outlined to a shared Name Map (nm) file. |
| 0x04 | SLIM\_ZST | The SLIM format compressed via Zstandard, representing a highly optimized state for distribution. |
| 0x05 | SLIM\_ZSTD\_DICT | The SLIM format compressed utilizing a shared, pre-trained Zstandard dictionary, maximizing compression ratios across highly uniform data blocks. |

### **Name Map Resolution and ULEB128 Decoding**

The introduction of the "Slim" format variants fundamentally alters the extraction methodology. In Slim VROMFS archives, human-readable strings such as file paths, material designations, and parameter keys are no longer stored adjacent to their respective data payloads. Instead, they are replaced by sequential integer indices that point to a global Name Map.15 This Name Map is typically stored in an isolated nm file located at the virtual root of the file system.15  
When designing a C++ parser for Slim formats, the extraction tool must strictly prioritize the location and decoding of the nm file before any payload extraction can occur. The Name Map structure is encoded using Unsigned Little Endian Base 128 (ULEB128), a variable-length encoding system designed to store arbitrarily large integers compactly.12 The nm file begins with a sequence of ULEB128 bytes indicating the total number of strings in the table, followed immediately by another ULEB128 sequence indicating the total size of the string block in bytes.15  
A robust C++ implementation must include a ULEB128 decoding function. The algorithm requires iterating through the byte stream, extracting the lower 7 bits of each byte, and logically shifting them into a 32-bit or 64-bit unsigned integer. The iteration continues until a byte with a leading zero bit (indicating the final byte of the sequence) is encountered. Once the sizes are decoded, the parser reads the contiguous block of null-terminated strings and maps them into a rapid-access, cache-friendly structure, such as a std::vector\<std::string\> or a std::unordered\_map\<uint32\_t, std::string\>. This Name Map must remain persistent in the application's heap memory during the entire extraction process, as subsequent file header parsing across the entire VROMFS archive will strictly reference these mapped integers rather than literal string values.12

### **Header Parsing, Signature Validation, and Memory Mapping**

The file headers within a VROMFS archive define the directory hierarchy and payload boundaries. Parsing these headers requires careful attention to byte alignment and endianness. After bypassing the cryptographic signatures or magic byte identifiers at the start of the file, the parser encounters a file allocation table.15 This table delineates the exact memory offset, compressed byte size, uncompressed byte size, and specific flags for every embedded file.15  
The ubiquitous use of Zstandard compression requires the custom C++ toolchain to dynamically link against libzstd to decompress the payloads. To ensure high performance and prevent memory exhaustion when unpacking multi-gigabyte archives like aces.vromfs.bin, the extraction tool should employ memory-mapped file I/O operations (mmap on POSIX systems or MapViewOfFile on Windows Windows environments).17 By mapping the archive directly into the virtual address space, the tool can pass direct memory pointers to the ZSTD decompression contexts without allocating massive intermediate buffers, mirroring the efficient resource management strategies employed natively by the Dagor Engine.

## **DataBlock (BLK) Serialization and Vulnerability Analysis**

Within the VROMFS containers, the vast majority of hierarchical configuration data, scene setups, physical vehicle parameters, and asset dependency links are serialized into Block files (.blk).18 In the Dagor Engine codebase, these files are processed, serialized, and deserialized using the core DataBlock C++ class, frequently referenced in source files such as prog/engine/ioSys/dataBlock/blk\_core.cpp and dag\_dataBlock.h.20  
While BLK files can exist in a human-readable, plain-text format during the development phase to facilitate modding and configuration 18, the compiled game resources distributed to the client strictly utilize an optimized binary representation.12 A critical function of the extraction tool is to parse this binary format and either output the raw data structures for internal processing or reconstruct the human-readable text format for analytical purposes.

### **Binary BLK Layout and Primitive Type Semantics**

The internal structure of a binary BLK file operates fundamentally as a recursive, acyclic node graph. It consists of nested blocks, where each block can contain multiple child blocks and an array of key-value parameter pairs.18 The binary format relies on a strictly defined set of 12 distinct primitive data types, each identified by a unique byte code :

| Byte Identifier | Type Name | C++ Memory Layout | Payload Size (Bytes) | Storage Paradigm |
| :---- | :---- | :---- | :---- | :---- |
| 0x01 | String | Zero-terminated char array | Variable | Outline (Offset-based) |
| 0x02 | Int | int32\_t | 4 | Inline |
| 0x03 | Float | float | 4 | Inline |
| 0x04 | Float2 | float | 8 | Outline |
| 0x05 | Float3 | float | 12 | Outline |
| 0x06 | Float4 | float | 16 | Outline |
| 0x07 | Int2 | int32\_t | 8 | Outline |
| 0x08 | Int3 | int32\_t | 12 | Outline |
| 0x09 | Bool | uint8\_t (padded to 4\) | 4 | Inline |
| 0x0a | Color | uint8\_t (RGBA) | 4 | Inline |
| 0x0b | Float12 | float (3x4 Matrix) | 48 | Outline |
| 0x0c | Long | int64\_t | 8 | Outline |

A critical architectural distinction within the BLK parser logic must be established between inline and outline payloads. For inline types (such as Int, Float, Bool, and Color), the 4-byte value is stored directly within the parameter block structure itself, maximizing cache locality. Conversely, for outline types (such as Float12, typically representing 3x4 transformation matrices, or variable-length strings), the parameter block does not store the data. Instead, it stores a 4-byte relative offset. This offset acts as a pointer to a dedicated data payload segment located at the end of the BLK file.  
The hierarchical nesting of the BLK structure necessitates a recursive descent parsing algorithm. The file layout utilizes sequential offset mappings to denote which child parameters and sub-blocks correspond to which parent block. The C++ parser must instantiate a recursive function that traverses the block definition table. For each node, it must resolve the string name of the block by referencing the global Name Map, iterate through the specified count of parameters associated with that block ID, and recursively process any child blocks.  
The introduction of the War Thunder 2.7.0.58 update initiated a profound structural change in BLK encoding, causing older utilities to fail with UnicodeDecodeError exceptions due to invalid start bytes.24 This update integrated Zstandard dictionary compression directly into the BLK format, requiring parsers to maintain a specialized decoding state that applies the appropriate ZSTD dictionary before attempting to read the parameter block arrays.24

### **Memory Safety, Buffer Overflows, and Fuzzing Implications**

An in-depth analysis of crash logs derived from the Dagor Engine's DataBlock::loadFromBinDump and DataBlock::\~DataBlock destructors reveals significant vulnerabilities regarding memory allocation and boundary validation.21 Extensive fuzzing tests conducted on the public repository have exposed severe AddressSanitizer (ASan) errors, specifically heap-buffer-overflows and stack-overflows triggered by \_\_asan\_memcpy operations when the engine attempts to process maliciously crafted or corrupt BLK files.21  
These crash logs indicate that the native engine logic heavily relies on raw memory copies (memcpy), deriving read lengths and offsets directly from the unverified file header. When negative sizes or astronomically large offset values are passed from the binary format into the memory loading callbacks, the engine fails to perform sufficient bounds checking, resulting in catastrophic memory corruption.27  
When engineering a custom C++ extraction utility, it is paramount to avoid these identical pitfalls. The implementation of strict boundary validation is non-negotiable. Before executing any reinterpret\_cast to map bytes to parameter structs, or utilizing memcpy to extract outline string payloads, the parser must cryptographically verify that the target memory offset, combined with the requested data size, does not exceed the total buffer length of the decompressed BLK payload. Utilizing bounds-checked types such as std::span (introduced in C++20) is highly recommended to enforce memory safety during binary deserialization, mitigating the risk of segmentation faults when encountering malformed aces.vromfs.bin data.

## **Texture Packaging: DXP Archives and DDSX Compression**

Textures utilized by 3D models in War Thunder are rarely stored as autonomous files. To reduce disk seek times and streamline the rendering pipeline, textures are aggregated into Game Resource Packs (GRP) and specifically, DdsX Packs (.dxp.bin).2 A single DXP container functions as an archive, bundling numerous compressed textures. Inside these packs, individual textures are encoded utilizing Gaijin's proprietary DDSX wrapper format.

### **The DDSX Wrapper Protocol and Magic Headers**

The DDSX format is essentially a custom header and metadata wrapper constructed around standard DirectDraw Surface (DDS) block-compressed pixel data. The fundamental purpose of this wrapper is to embed engine-specific metadata—such as mip-map streaming flags, gamma space definitions, platform-specific memory swizzling instructions, and custom compression states—directly into the asset, thereby offloading processing overhead from the CPU to the GPU's Direct Memory Access (DMA) controller.  
The binary structure of a DDSX file begins with a proprietary header, consistently identified by a specific magic byte sequence, typically 0x1a.30 Following the magic identifier, the header defines crucial parameters: the pixel format (e.g., DXT1, DXT5, ATI2, or the modern BC7 format), the absolute image dimensions (width and height), the number of mip-map levels present, and arguably the most critical parameter, the memSz (memory size).31 The memSz 32-bit integer explicitly dictates the boundaries of the compressed pixel payload within the file buffer.31

### **Decompression Strategies and Header Reconstruction**

Depending on the targeted hardware platform, the engine version, and the resolution of the texture, DDSX payloads employ highly aggressive compression algorithms layered on top of the underlying DDS block compression. A sophisticated C++ extraction tool must be capable of parsing the DDSX flags to identify the correct compression tier and dynamically route the payload to the appropriate decompression library:

1. **Oodle Compression Integration:** High-resolution textures, normal maps, and albedo maps in modern War Thunder builds frequently utilize RAD Game Tools' Oodle data compression. Because Oodle is proprietary software and Gaijin lacks the licensing permissions to distribute its source code within the public Dagor Engine repository 32, the extraction tool must dynamically load the pre-compiled oo2core\_6\_win64.dll library.2 The C++ implementation must utilize operating system APIs (such as LoadLibrary and GetProcAddress on Windows) to bind to the OodleLZ\_Decompress function pointer, passing the compressed payload delineated by the memSz parameter to retrieve the raw DDS blocks.25  
2. **Zstandard (ZSTD) Compression:** Recent architectural updates have aggressively integrated ZSTD for texture compression, aiming to replace older algorithms. This requires seamless integration with libzstd.14  
3. **Legacy LZMA Compression:** Older texture formats, or specific UI atlases, rely on traditional LZMA algorithms (pylzma). These parsers must target the specific byte offset immediately following the 32-byte (0x20) DDSX header to initiate decompression.31

Once the pixel payload is successfully decompressed into system memory, the extraction tool's task is not complete. Standard image editors and 3D modeling suites (such as Blender or Autodesk 3ds Max) cannot natively interpret raw DDSX payloads. The C++ utility must mathematically reconstruct a mathematically valid Microsoft DDS header. This involves writing the DDS magic sequence, computing the appropriate surface flags, and generating the DDPF (DirectDraw Pixel Format) structures that correspond to the BC7 or DXT5 formats identified in the original DDSX wrapper.25 The reconstructed DDS header is prepended to the decompressed pixel data, yielding a fully compliant .dds file ready for external use.

## **Static Geometry Reconstruction: The DAG Format**

The primary objective of the extraction tool is the flawless reconstruction of 3D geometry and skeletal hierarchies. The Dagor Engine mandates a strict systemic separation between static environmental geometry, physical collision boundaries, and dynamic, skinned character/vehicle entities.5  
For static geometry—designated internally as Renderable Instances (RendInst)—the engine utilizes the proprietary .dag (Dagor) format.5 Examples of RendInsts include buildings, terrain clutter, static defensive structures, and non-animated vehicle components.29 To facilitate performant rendering at extreme draw distances, a single asset is typically exported into multiple DAG files representing descending Levels of Detail (e.g., building\_a.lod00.dag, building\_a.lod01.dag, building\_a.lod02.dag).29

### **Parsing Vertex and Index Buffers**

The binary layout of a DAG file is sequentially segmented into distinct data blocks, primarily defining the vertex buffers, index buffers, and material indices.34  
The C++ parser must first navigate to the vertex buffer definition. The stride of the vertex buffer is highly variable, depending entirely on the specific properties baked into the mesh during compilation. A standard vertex block contains 32-bit floating-point arrays for positional data (x, y, z). However, the parser must conditionally check for the presence of interlaced data, including vertex normals, tangents, bitangents (essential for normal mapping), and potentially multiple UV coordinate channels used for damage decals or ambient occlusion maps.34  
Following the vertex buffer, the index buffer dictates how the vertices are connected to form faces. In accordance with the Dagor Engine's rasterization requirements, all geometry stored in DAG files is strictly triangulated.34 Depending on the total vertex count of the mesh, indices are packed as either 16-bit or 32-bit unsigned integers to conserve memory. The parser extracts these indices to reconstruct the polygonal faces of the 3D model.  
The engine also supports Constructive Solid Geometry (CSG) operations on DAG files, utilizing the Carve CSG library internally.38 However, similar to the BLK format, fuzzing tests on the ASan DAG parser harness have revealed heap-buffer-overflow vulnerabilities within functions like make\_dag\_one\_mat\_blk(), specifically when parsing malformed string lengths for material assignments.39 The extraction tool must rigorously validate the size of the string buffers before attempting to map material data.

### **Material Resolution and Dependency Injection**

A crucial architectural detail is that DAG files do not encapsulate texture data. Instead, they store material definitions utilizing a proprietary standard known as DagorMat2.37 The parser can only extract the string nomenclature of the materials applied to the mesh surfaces.  
To export a textured model, the C++ tool must simulate the engine's runtime dependency injection. The material strings extracted from the DAG must be cross-referenced with external configuration files, typically named .folder.blk or .dynmodel.blk.34 By parsing these associated BLK files, the tool resolves the precise virtual file path of the corresponding DDSX texture.36 The tool then pivots to extract that specific texture from its parent DXP archive, executing the decompression and DDS reconstruction pipeline detailed previously, ensuring the exported geometry and textures are cohesively linked.

## **Dynamic Models, Skeletal Hierarchies, and Kinematics**

The extraction of static geometry represents only a fraction of the engine's capabilities. For aircraft, ground vehicles, infantry characters, and any asset requiring complex skeletal articulation, the Dagor Engine utilizes the Game Resource Pack (GRP) and Dynamic Renderable Scene (DynModel) architectures.5

### **The GRP Archive Architecture**

A GRP (.grp) file acts as a macro-container, an overarching archive that bundles all the disparate components necessary to render and simulate a dynamic entity.29 Unpacking a GRP reveals a cluster of specialized binary formats 5:

* dynmodel: The core dynamic mesh and skinning data.  
* skeleton (GeomNodeTree): The hierarchical bone structure.  
* phobj: The physics objects, encompassing collision meshes, mass data, and inertia tensors.  
* fastphys: Simplified physics approximations for rapid collision queries.  
* Char: Internal character class data.

### **GeomNodeTree and Skeletal Extraction**

To reconstruct an articulated model, the C++ parser must first process the skeleton component, internally represented by the GeomNodeTree class.5 This structure defines the hierarchical parent-child relationships of the joints (bones). The binary data contains an array of nodes, each defined by a string identifier (e.g., Bip01\_Pelvis, turret\_joint) and a local transform.  
The parser must extract these local transforms, which are typically stored as a combination of translation vectors and rotation quaternions (or full 4x4 transformation matrices). To reconstruct the global rest pose of the skeleton for export to formats like FBX or GLTF, the tool must recursively multiply the local transform of each child node by the accumulated global transform of its parent node, traversing from the root joint down to the terminal effectors.

### **DynModel Parsing and Vertex Weighting**

Once the skeletal hierarchy is established in memory, the parser shifts focus to the dynmodel file.5 The dynmodel contains the actual vertex and index buffers for the mesh. However, unlike the static DAG format, the vertex buffer in a DynModel is significantly more complex, as it must support hardware skinning.  
In addition to positional coordinates, normals, and UVs, the vertex buffer includes blend weights and blend indices. The C++ tool must parse these arrays, which typically consist of 4-byte vectors representing up to four bone influences per vertex. The blend indices map directly to the index of the joints defined in the previously parsed GeomNodeTree, while the blend weights dictate the percentage of influence that specific bone exercises over the vertex during articulation.

### **AnimChar and Kinematic Animation Tracks**

Animation tracks in the Dagor Engine are managed by the AnimChar system.5 These files contain the cinematic and mechanical animations applied to the DynModels (e.g., landing gear deployment, character walking cycles).  
Animations are not stored as per-frame vertex deformations; rather, they are stored as compressed tracks of keyframes that manipulate the quaternions and translation vectors of the GeomNodeTree joints over a specific timeline.5 Parsing the AnimChar binary requires extracting the framerate (time steps) and the arrays of keyframe values. The C++ exporter must reconstruct the interpolation curves—typically employing Spherical Linear Interpolation (Slerp) for quaternions and standard linear interpolation for translation vectors—to accurately bake the animation tracks into the exported 3D format, ensuring synchronization with the reconstructed skeleton.

## **C++ Toolchain System Architecture and Optimization**

Developing a high-performance C++ parser for War Thunder's proprietary formats requires synthesizing the parsing logic for VROMFS, BLK, DDSX, DAG, and DynModel into a cohesive, automated pipeline. The architecture of such a tool should draw heavily from the design patterns exhibited in existing open-source ecosystems, notably the Python-based wt-tools repository and the highly performant Rust-based wt\_blk and wt\_ext\_cli libraries.12

### **Memory Management and Concurrency**

The sheer volume of data contained within a typical War Thunder installation dictates that the C++ toolchain must be aggressively optimized for memory usage and multithreading. Relying on standard file I/O operations (std::ifstream::read) to process gigabyte-sized VROMFS dumps introduces severe bottlenecking.  
As discussed, Memory-Mapped Files are imperative. The tool should utilize memory mapping to expose the VROMFS and DXP archives directly to the virtual address space. This approach allows multiple worker threads to concurrently parse different file headers and execute ZSTD or Oodle decompression contexts on isolated memory segments without the overhead of thread-locking standard file descriptors or copying massive buffers onto the heap.17

### **Type-Safe Deserialization**

When implementing the BLK and DAG deserializers, the code must mirror the strict memory alignments expected by the Dagor Engine. The use of precisely packed struct definitions is mandatory:

C++

\#pragma pack(push, 1\)  
struct BlkParamHeader {  
    uint32\_t nameId;  // Integer index pointing to the global Name Map  
    uint8\_t  typeId;  // Primitive type identifier (0x01 to 0x0c)  
    uint32\_t payload; // Inline value or absolute offset to the outline payload  
};  
\#pragma pack(pop)

By casting the memory-mapped byte stream directly into these structures, the tool minimizes parsing overhead. However, the developer must remain vigilant regarding endianness and boundary validation. Every offset read from a BlkParamHeader or a DDSX header must be sanitized against the maximum bounds of the file mapping to prevent the segmentation faults explicitly warned about in the engine's fuzzing reports.27

## **Conclusion**

The asset serialization pipeline of the Dagor Engine represents a sophisticated, decoupled architecture explicitly designed to maximize runtime memory efficiency and minimize delta-update bandwidth. By isolating strings into global Name Maps, compressing geometry and textures with state-of-the-art dictionary algorithms, and systemically separating static meshes from dynamic skeletal hierarchies, Gaijin Entertainment has engineered a framework that inherently resists simple, sequential data extraction.  
For a C++ developer tasked with exporting 3D models, textures, skeletons, and animations from War Thunder, the process extends far beyond reading isolated file formats; it necessitates the reconstruction of a complex, relational database. The extraction utility must flawlessly navigate virtual file system mount points, decode ULEB128 metadata streams, maintain rapid-access global string lookup tables, recursively deserialize acyclic BLK node graphs, dynamically bind to proprietary decompression libraries, and mathematically reconstruct skeletal transform matrices and vertex weight assignments. By strictly adhering to the memory layouts, ECS dependency injection patterns, and rigorous algorithmic safety strategies delineated in this report, a robust, high-performance C++ extraction and export pipeline can be successfully realized.

#### **Works cited**

1. What is the workflow? · Issue \#141 · GaijinEntertainment/DagorEngine \- GitHub, accessed May 18, 2026, [https://github.com/GaijinEntertainment/DagorEngine/issues/141](https://github.com/GaijinEntertainment/DagorEngine/issues/141)  
2. klensy/wt-tools: War Thunder resource extraction tools \- GitHub, accessed May 18, 2026, [https://github.com/klensy/wt-tools](https://github.com/klensy/wt-tools)  
3. GitHub \- GaijinEntertainment/DagorEngine: Dagor Engine and Tools source code from Gaijin Games KFT, accessed May 18, 2026, [https://github.com/GaijinEntertainment/DagorEngine](https://github.com/GaijinEntertainment/DagorEngine)  
4. Dagor Quirrel Modules Docs \- GitHub Pages, accessed May 18, 2026, [https://gaijinentertainment.github.io/DagorEngine/api-references/quirrel-modules/index.html](https://gaijinentertainment.github.io/DagorEngine/api-references/quirrel-modules/index.html)  
5. quentin-dh/Dagor-Asset-Explorer \- GitHub, accessed May 18, 2026, [https://github.com/quentin-dh/Dagor-Asset-Explorer](https://github.com/quentin-dh/Dagor-Asset-Explorer)  
6. Releases · GaijinEntertainment/DagorEngine \- GitHub, accessed May 18, 2026, [https://github.com/GaijinEntertainment/DagorEngine/releases](https://github.com/GaijinEntertainment/DagorEngine/releases)  
7. \[Feature Request\] Make tools like DaEditorX available for build\_all.py while targeting windows arm64 · Issue \#156 · GaijinEntertainment/DagorEngine \- GitHub, accessed May 18, 2026, [https://github.com/GaijinEntertainment/DagorEngine/issues/156](https://github.com/GaijinEntertainment/DagorEngine/issues/156)  
8. "CDK: added the ability to check user aircraft in custom user missions." : r/Warthunder \- Reddit, accessed May 18, 2026, [https://www.reddit.com/r/Warthunder/comments/36sn1l/cdk\_added\_the\_ability\_to\_check\_user\_aircraft\_in/](https://www.reddit.com/r/Warthunder/comments/36sn1l/cdk_added_the_ability_to_check_user_aircraft_in/)  
9. Software \- TrID \- File Extensions and File Types Definitions list \- Marco Pontello, accessed May 18, 2026, [https://mark0.net/soft-trid-deflist-v.html](https://mark0.net/soft-trid-deflist-v.html)  
10. A better guide to localization mods | War Thunder Wiki, accessed May 18, 2026, [https://wiki.warthunder.com/cdk/1819-a-better-guide-to-localization-mods](https://wiki.warthunder.com/cdk/1819-a-better-guide-to-localization-mods)  
11. Open files content in atlases.vromfs.bin · Issue \#29 · klensy/wt-tools \- GitHub, accessed May 18, 2026, [https://github.com/klensy/wt-tools/issues/29](https://github.com/klensy/wt-tools/issues/29)  
12. wt\_blk::blk \- Rust \- Docs.rs, accessed May 18, 2026, [https://docs.rs/wt\_blk/latest/wt\_blk/blk/index.html](https://docs.rs/wt_blk/latest/wt_blk/blk/index.html)  
13. zlib.error when trying to unpack aces.vromfs.bin · Issue \#18 · klensy/wt-tools \- GitHub, accessed May 18, 2026, [https://github.com/klensy/wt-tools/issues/18](https://github.com/klensy/wt-tools/issues/18)  
14. Releases · klensy/wt-tools \- GitHub, accessed May 18, 2026, [https://github.com/klensy/wt-tools/releases](https://github.com/klensy/wt-tools/releases)  
15. Not an issue, but a question · Issue \#70 · klensy/wt-tools \- GitHub, accessed May 18, 2026, [https://github.com/klensy/wt-tools/issues/70](https://github.com/klensy/wt-tools/issues/70)  
16. 1.53 bin unpack (DEV server) · Issue \#7 · klensy/wt-tools \- GitHub, accessed May 18, 2026, [https://github.com/klensy/wt-tools/issues/7](https://github.com/klensy/wt-tools/issues/7)  
17. Off we go\! Digging into the game engine of War Thunder and interviewing its devs, accessed May 18, 2026, [https://pvs-studio.com/en/blog/posts/cpp/1098/](https://pvs-studio.com/en/blog/posts/cpp/1098/)  
18. Block file (.BLK) \- War Thunder Wiki, accessed May 18, 2026, [https://old-wiki.warthunder.com/Block\_file\_(.BLK)](https://old-wiki.warthunder.com/Block_file_\(.BLK\))  
19. Modification of existing ground units and weapons | War Thunder Wiki, accessed May 18, 2026, [https://wiki.warthunder.com/cdk/1197-modification-of-existing-ground-units-and-weapons](https://wiki.warthunder.com/cdk/1197-modification-of-existing-ground-units-and-weapons)  
20. SEGV on unknown address 0x000000000008 (pc 0x55ddeaecf1cb bp 0x7fff8d0be190 sp 0x7fff8d0be120 T0) in dag::Vector  
21. heap-buffer-overflow on address 0x7b93ec9e0040 at pc 0x561fa7509b06 bp 0x7ffe235f6300 sp 0x7ffe235f5ac0 in MemGeneralLoadCB::read(void\*, int) · Issue \#198 · GaijinEntertainment/DagorEngine \- GitHub, accessed May 18, 2026, [https://github.com/GaijinEntertainment/DagorEngine/issues/198](https://github.com/GaijinEntertainment/DagorEngine/issues/198)  
22. SEGV on unknown address (pc 0x55c0bb855123 bp 0x7ffed63e59b0 sp 0x7ffed63e5940 T0) in DataBlockShared::getBlock(unsigned int) · Issue \#196 · GaijinEntertainment/DagorEngine \- GitHub, accessed May 18, 2026, [https://github.com/GaijinEntertainment/DagorEngine/issues/196](https://github.com/GaijinEntertainment/DagorEngine/issues/196)  
23. BLK File Format \- GitHub Pages, accessed May 18, 2026, [https://gaijinentertainment.github.io/DagorEngine/dagor-tools/blk/index.html](https://gaijinentertainment.github.io/DagorEngine/dagor-tools/blk/index.html)  
24. Update to new (2.7.0.58+) vromfs/blk encoding · Issue \#64 · klensy/wt-tools \- GitHub, accessed May 18, 2026, [https://github.com/klensy/wt-tools/issues/64](https://github.com/klensy/wt-tools/issues/64)  
25. Cannot extract unit icon \*.ddsx despite using oo2core\_6\_win64.dll \#49 \- GitHub, accessed May 18, 2026, [https://github.com/klensy/wt-tools/issues/49](https://github.com/klensy/wt-tools/issues/49)  
26. can't extract the BLK file and want an explanation · Issue \#68 · klensy/wt-tools \- GitHub, accessed May 18, 2026, [https://github.com/klensy/wt-tools/issues/68](https://github.com/klensy/wt-tools/issues/68)  
27. negative-size-param: (size=-1) MemGeneralLoadCB::read(void\*, int) \#193 \- GitHub, accessed May 18, 2026, [https://github.com/GaijinEntertainment/DagorEngine/issues/193](https://github.com/GaijinEntertainment/DagorEngine/issues/193)  
28. stack-overflow on address 0x7ffd77610ff8 (pc 0x55f8f79acfe8 bp, accessed May 18, 2026, [https://github.com/GaijinEntertainment/DagorEngine/issues/195](https://github.com/GaijinEntertainment/DagorEngine/issues/195)  
29. \[Guide\] Add Models (Rendinsts) to Enlisted \- Game Mods, accessed May 18, 2026, [https://forum.enlisted.net/t/guide-add-models-rendinsts-to-enlisted/155410](https://forum.enlisted.net/t/guide-add-models-rendinsts-to-enlisted/155410)  
30. File ddsx, format 0x1a · Issue \#58 · klensy/wt-tools \- GitHub, accessed May 18, 2026, [https://github.com/klensy/wt-tools/issues/58](https://github.com/klensy/wt-tools/issues/58)  
31. Can't open ddsx files from Cuisine Royale · Issue \#51 · klensy/wt-tools, accessed May 18, 2026, [https://github.com/klensy/wt-tools/issues/51](https://github.com/klensy/wt-tools/issues/51)  
32. ddsx2dds "Oodle read error" · Issue \#2 · GaijinEntertainment, accessed May 18, 2026, [https://github.com/GaijinEntertainment/DagorEngine/issues/2](https://github.com/GaijinEntertainment/DagorEngine/issues/2)  
33. It's possible to use an official skin as template for a custom one? : r/Warthunder \- Reddit, accessed May 18, 2026, [https://www.reddit.com/r/Warthunder/comments/1kc2im8/its\_possible\_to\_use\_an\_official\_skin\_as\_template/](https://www.reddit.com/r/Warthunder/comments/1kc2im8/its_possible_to_use_an_official_skin_as_template/)  
34. Asset Viewer \- GitHub Pages, accessed May 18, 2026, [https://gaijinentertainment.github.io/DagorEngine/dagor-tools/asset-viewer/asset-viewer/asset\_viewer.html](https://gaijinentertainment.github.io/DagorEngine/dagor-tools/asset-viewer/asset-viewer/asset_viewer.html)  
35. Basic information about creating models for Dagor Engine \- War Thunder Wiki, accessed May 18, 2026, [https://wiki.warthunder.com/cdk/creation\_model](https://wiki.warthunder.com/cdk/creation_model)  
36. Checking 3D model of the aircraft in game \- War Thunder Wiki, accessed May 18, 2026, [https://old-wiki.warthunder.com/Checking\_3D\_model\_of\_the\_aircraft\_in\_game](https://old-wiki.warthunder.com/Checking_3D_model_of_the_aircraft_in_game)  
37. \[Guide\] Import 3D models into Dagor Engine (and Enlisted) \- Mods, accessed May 18, 2026, [https://forum.enlisted.net/t/guide-import-3d-models-into-dagor-engine-and-enlisted/143189](https://forum.enlisted.net/t/guide-import-3d-models-into-dagor-engine-and-enlisted/143189)  
38. What is dagCSG? · Issue \#135 · GaijinEntertainment/DagorEngine \- GitHub, accessed May 18, 2026, [https://github.com/GaijinEntertainment/DagorEngine/issues/135](https://github.com/GaijinEntertainment/DagorEngine/issues/135)  
39. heap-buffer-overflow on address 0x508000001918 at pc 0x5a445af65f07 bp 0x7fffadea2e70 sp 0x7fffadea2e68 in dag::Vector  
40. Issues · GaijinEntertainment/DagorEngine \- GitHub, accessed May 18, 2026, [https://github.com/GaijinEntertainment/DagorEngine/issues](https://github.com/GaijinEntertainment/DagorEngine/issues)  
41. Prefabs do not work in latest releases · Issue \#184 · GaijinEntertainment/DagorEngine, accessed May 18, 2026, [https://github.com/GaijinEntertainment/DagorEngine/issues/184](https://github.com/GaijinEntertainment/DagorEngine/issues/184)  
42. Dagor Engine dynmodel (WarThunder\\Enlisted) \- ZenHAX, accessed May 18, 2026, [https://zenhax.com/viewtopic.php@t=15887.html](https://zenhax.com/viewtopic.php@t=15887.html)  
43. Spine Integration | Adventure Creator Wikia | Fandom, accessed May 18, 2026, [https://adventure-creator.fandom.com/wiki/Spine\_Integration](https://adventure-creator.fandom.com/wiki/Spine_Integration)  
44. Automatic-motion-generator/retargeter.py at main \- GitHub, accessed May 18, 2026, [https://github.com/EagleW/Automatic-motion-generator/blob/main/retargeter.py](https://github.com/EagleW/Automatic-motion-generator/blob/main/retargeter.py)  
45. GitHub \- Warthunder-Open-Source-Foundation/wt\_ext\_cli: The fastest VROMFS and BLK file unpacker with uncompromised features, accessed May 18, 2026, [https://github.com/Warthunder-Open-Source-Foundation/wt\_ext\_cli](https://github.com/Warthunder-Open-Source-Foundation/wt_ext_cli)  
46. Warthunder-Open-Source-Foundation \- GitHub, accessed May 18, 2026, [https://github.com/Warthunder-Open-Source-Foundation](https://github.com/Warthunder-Open-Source-Foundation)