#include "Export/ModelExporter.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <limits>
#include <iomanip>

namespace DagorAssetsExtractor::Export
{
    bool ModelExporter::ExportToGlb(
        const std::filesystem::path& path,
        const std::vector<DagorFormats::RendInstMesh>& lods_in,
        const ExportManifest& manifest,
        const std::vector<MaterialInfo>& materials)
    {
        std::vector<DagorFormats::RendInstMesh> lods;
        for (const auto& lod_in : lods_in)
        {
            if (lod_in.vertices.empty() || lod_in.indices.empty())
                continue;

            DagorFormats::RendInstMesh validLod = lod_in;
            // Remove primitives with 0 indices
            validLod.primitives.erase(
                std::remove_if(validLod.primitives.begin(), validLod.primitives.end(),
                    [](const DagorFormats::RendInstPrimitive& p) { return p.indexCount == 0; }),
                validLod.primitives.end());

            if (validLod.primitives.empty())
            {
                // Create a default primitive if all were empty or none existed
                validLod.primitives.push_back({ 0, 0, static_cast<uint32_t>(validLod.indices.size()) });
            }

            lods.push_back(std::move(validLod));
        }

        if (lods.empty())
        {
            return false;
        }

        // We will store all vertex buffers and index buffers in one continuous BIN chunk.
        // For each LOD, we will have:
        // - Vertices bufferView
        // - Indices bufferView
        // - Position accessor
        // - Normal accessor
        // - UV accessor
        // - Index accessor

        struct LodBinInfo
        {
            size_t vertByteOffset;
            size_t vertByteLength;
            size_t indexByteOffset;
            size_t indexByteLength;

            float minX = std::numeric_limits<float>::max();
            float minY = std::numeric_limits<float>::max();
            float minZ = std::numeric_limits<float>::max();
            float maxX = -std::numeric_limits<float>::max();
            float maxY = -std::numeric_limits<float>::max();
            float maxZ = -std::numeric_limits<float>::max();
        };

        std::vector<LodBinInfo> binInfos;
        binInfos.reserve(lods.size());

        std::vector<uint8_t> binBuffer;
        
        for (const auto& mesh : lods)
        {
            LodBinInfo info;

            // Vertices start at the current boundary
            // Vertices are array of Vertex structs (32 bytes each)
            info.vertByteOffset = binBuffer.size();
            info.vertByteLength = mesh.vertices.size() * sizeof(DagorFormats::Vertex);

            if (!mesh.vertices.empty())
            {
                const uint8_t* vertBytes = reinterpret_cast<const uint8_t*>(mesh.vertices.data());
                binBuffer.insert(binBuffer.end(), vertBytes, vertBytes + info.vertByteLength);

                for (const auto& v : mesh.vertices)
                {
                    info.minX = std::min(info.minX, v.x);
                    info.minY = std::min(info.minY, v.y);
                    info.minZ = std::min(info.minZ, v.z);
                    info.maxX = std::max(info.maxX, v.x);
                    info.maxY = std::max(info.maxY, v.y);
                    info.maxZ = std::max(info.maxZ, v.z);
                }
            }
            else
            {
                info.minX = info.minY = info.minZ = 0.0f;
                info.maxX = info.maxY = info.maxZ = 0.0f;
            }

            // Indices start at current boundary (pad BIN buffer to 4 bytes if necessary, though Vertex is 32 bytes, so it's already aligned)
            while (binBuffer.size() % 4 != 0)
            {
                binBuffer.push_back(0);
            }

            info.indexByteOffset = binBuffer.size();
            info.indexByteLength = mesh.indices.size() * sizeof(uint32_t);

            if (!mesh.indices.empty())
            {
                const uint8_t* indexBytes = reinterpret_cast<const uint8_t*>(mesh.indices.data());
                binBuffer.insert(binBuffer.end(), indexBytes, indexBytes + info.indexByteLength);
            }

            binInfos.push_back(info);

            // Pad boundary for next LOD
            while (binBuffer.size() % 4 != 0)
            {
                binBuffer.push_back(0);
            }
        }

        // Generate JSON
        std::stringstream json;
        json << "{\n";
        json << "  \"asset\": {\n";
        json << "    \"version\": \"2.0\",\n";
        json << "    \"generator\": \"DagorAssetsExtractor\"\n";
        json << "  },\n";

        // Scene definition
        json << "  \"scene\": 0,\n";
        json << "  \"scenes\": [\n";
        json << "    {\n";
        json << "      \"nodes\": [";
        for (size_t i = 0; i < lods.size(); ++i)
        {
            json << i;
            if (i + 1 < lods.size()) json << ", ";
        }
        json << "]\n";
        json << "    }\n";
        json << "  ],\n";

        // Nodes definition
        json << "  \"nodes\": [\n";
        for (size_t i = 0; i < lods.size(); ++i)
        {
            json << "    {\n";
            json << "      \"name\": \"" << lods[i].name << "\",\n";
            json << "      \"mesh\": " << i << "\n";
            json << "    }";
            if (i + 1 < lods.size()) json << ",";
            json << "\n";
        }
        json << "  ],\n";

        // Pre-calculate accessor indices for indices
        std::vector<std::vector<size_t>> indexAccessorIndices(lods.size());
        size_t currentAccessor = lods.size() * 3;
        for (size_t i = 0; i < lods.size(); ++i)
        {
            size_t primCount = lods[i].primitives.size();
            if (primCount == 0) primCount = 1; // Default primitive
            for (size_t p = 0; p < primCount; ++p)
            {
                indexAccessorIndices[i].push_back(currentAccessor++);
            }
        }

        // Meshes definition
        json << "  \"meshes\": [\n";
        for (size_t i = 0; i < lods.size(); ++i)
        {
            const auto& mesh = lods[i];
            json << "    {\n";
            json << "      \"name\": \"" << mesh.name << "\",\n";
            json << "      \"primitives\": [\n";
            
            auto prims = mesh.primitives;
            if (prims.empty())
            {
                prims.push_back({ 0, 0, static_cast<uint32_t>(mesh.indices.size()) });
            }

            for (size_t p = 0; p < prims.size(); ++p)
            {
                const auto& prim = prims[p];
                json << "        {\n";
                json << "          \"attributes\": {\n";
                json << "            \"POSITION\": " << (i * 3) << ",\n";
                json << "            \"NORMAL\": " << (i * 3 + 1) << ",\n";
                json << "            \"TEXCOORD_0\": " << (i * 3 + 2) << "\n";
                json << "          },\n";
                json << "          \"indices\": " << indexAccessorIndices[i][p] << ",\n";
                
                if (!materials.empty())
                {
                    size_t matIdx = static_cast<size_t>(prim.materialIndex);
                    if (matIdx >= materials.size()) matIdx = 0;
                    json << "          \"material\": " << matIdx << ",\n";
                }
                
                json << "          \"mode\": 4\n"; // TRIANGLES
                json << "        }";
                if (p + 1 < prims.size()) json << ",";
                json << "\n";
            }
            json << "      ]\n";
            json << "    }";
            if (i + 1 < lods.size()) json << ",";
            json << "\n";
        }
        json << "  ],\n";

        if (!materials.empty())
        {
            // Materials definition
            json << "  \"materials\": [\n";
            for (size_t i = 0; i < materials.size(); ++i)
            {
                json << "    {\n";
                json << "      \"name\": \"" << materials[i].name << "\",\n";
                json << "      \"pbrMetallicRoughness\": {\n";
                json << "        \"baseColorTexture\": {\n";
                json << "          \"index\": " << i << "\n";
                json << "        },\n";
                json << "        \"metallicFactor\": 0.0,\n";
                json << "        \"roughnessFactor\": 1.0\n";
                json << "      }\n";
                json << "    }";
                if (i + 1 < materials.size()) json << ",";
                json << "\n";
            }
            json << "  ],\n";

            // Textures definition
            json << "  \"textures\": [\n";
            for (size_t i = 0; i < materials.size(); ++i)
            {
                json << "    {\n";
                json << "      \"sampler\": 0,\n";
                json << "      \"source\": " << i << "\n";
                json << "    }";
                if (i + 1 < materials.size()) json << ",";
                json << "\n";
            }
            json << "  ],\n";

            // Images definition
            json << "  \"images\": [\n";
            for (size_t i = 0; i < materials.size(); ++i)
            {
                json << "    {\n";
                json << "      \"uri\": \"" << materials[i].diffuseTexturePath << "\"\n";
                json << "    }";
                if (i + 1 < materials.size()) json << ",";
                json << "\n";
            }
            json << "  ],\n";

            // Samplers definition
            json << "  \"samplers\": [\n";
            json << "    {\n";
            json << "      \"magFilter\": 9729,\n"; // LINEAR
            json << "      \"minFilter\": 9987,\n"; // LINEAR_MIPMAP_LINEAR
            json << "      \"wrapS\": 10497,\n";    // REPEAT
            json << "      \"wrapT\": 10497\n";     // REPEAT
            json << "    }\n";
            json << "  ],\n";
        }

        // BufferViews definition
        json << "  \"bufferViews\": [\n";
        // Vertex bufferViews first
        for (size_t i = 0; i < lods.size(); ++i)
        {
            json << "    {\n";
            json << "      \"buffer\": 0,\n";
            json << "      \"byteOffset\": " << binInfos[i].vertByteOffset << ",\n";
            json << "      \"byteLength\": " << binInfos[i].vertByteLength << ",\n";
            json << "      \"byteStride\": 32,\n";
            json << "      \"target\": 34962\n"; // ARRAY_BUFFER
            json << "    },\n";
        }
        // Index bufferViews next
        for (size_t i = 0; i < lods.size(); ++i)
        {
            json << "    {\n";
            json << "      \"buffer\": 0,\n";
            json << "      \"byteOffset\": " << binInfos[i].indexByteOffset << ",\n";
            json << "      \"byteLength\": " << binInfos[i].indexByteLength << ",\n";
            json << "      \"target\": 34963\n"; // ELEMENT_ARRAY_BUFFER
            json << "    }";
            if (i + 1 < lods.size()) json << ",";
            json << "\n";
        }
        json << "  ],\n";

        // Accessors definition
        json << "  \"accessors\": [\n";
        // Accessors for positions, normals, UVs
        for (size_t i = 0; i < lods.size(); ++i)
        {
            const auto& mesh = lods[i];
            const auto& info = binInfos[i];

            // POSITION
            json << "    {\n";
            json << "      \"bufferView\": " << i << ",\n";
            json << "      \"byteOffset\": 0,\n";
            json << "      \"componentType\": 5126,\n"; // FLOAT
            json << "      \"count\": " << mesh.vertices.size() << ",\n";
            json << "      \"type\": \"VEC3\",\n";
            json << "      \"max\": [" << info.maxX << ", " << info.maxY << ", " << info.maxZ << "],\n";
            json << "      \"min\": [" << info.minX << ", " << info.minY << ", " << info.minZ << "]\n";
            json << "    },\n";

            // NORMAL
            json << "    {\n";
            json << "      \"bufferView\": " << i << ",\n";
            json << "      \"byteOffset\": 12,\n";
            json << "      \"componentType\": 5126,\n"; // FLOAT
            json << "      \"count\": " << mesh.vertices.size() << ",\n";
            json << "      \"type\": \"VEC3\"\n";
            json << "    },\n";

            // TEXCOORD_0
            json << "    {\n";
            json << "      \"bufferView\": " << i << ",\n";
            json << "      \"byteOffset\": 24,\n";
            json << "      \"componentType\": 5126,\n"; // FLOAT
            json << "      \"count\": " << mesh.vertices.size() << ",\n";
            json << "      \"type\": \"VEC2\"\n";
            json << "    },\n";
        }

        // Accessors for indices (one per primitive)
        for (size_t i = 0; i < lods.size(); ++i)
        {
            const auto& mesh = lods[i];
            auto prims = mesh.primitives;
            if (prims.empty())
            {
                prims.push_back({ 0, 0, static_cast<uint32_t>(mesh.indices.size()) });
            }

            for (size_t p = 0; p < prims.size(); ++p)
            {
                const auto& prim = prims[p];
                json << "    {\n";
                json << "      \"bufferView\": " << (lods.size() + i) << ",\n";
                json << "      \"byteOffset\": " << (prim.firstIndex * sizeof(uint32_t)) << ",\n";
                json << "      \"componentType\": 5125,\n"; // UNSIGNED_INT
                json << "      \"count\": " << prim.indexCount << ",\n";
                json << "      \"type\": \"SCALAR\"\n";
                json << "    }";
                if (i + 1 < lods.size() || p + 1 < prims.size()) json << ",";
                json << "\n";
            }
        }
        json << "  ],\n";

        // Buffer definition
        json << "  \"buffers\": [\n";
        json << "    {\n";
        json << "      \"byteLength\": " << binBuffer.size() << "\n";
        json << "    }\n";
        json << "  ]\n";
        json << "}\n";

        std::string jsonStr = json.str();

        // Pad JSON to 4-byte boundary
        while (jsonStr.size() % 4 != 0)
        {
            jsonStr.push_back(' ');
        }

        // Open binary stream
        std::ofstream out(path, std::ios::binary);
        if (!out)
        {
            return false;
        }

        // Write GLB Header
        uint32_t magic = 0x46546C67; // "glTF"
        uint32_t version = 2;
        uint32_t totalLength = 12 + 8 + static_cast<uint32_t>(jsonStr.size()) + 8 + static_cast<uint32_t>(binBuffer.size());

        out.write(reinterpret_cast<const char*>(&magic), 4);
        out.write(reinterpret_cast<const char*>(&version), 4);
        out.write(reinterpret_cast<const char*>(&totalLength), 4);

        // Write Chunk 0 (JSON)
        uint32_t jsonChunkLen = static_cast<uint32_t>(jsonStr.size());
        uint32_t jsonChunkType = 0x4E4F534A; // "JSON"
        out.write(reinterpret_cast<const char*>(&jsonChunkLen), 4);
        out.write(reinterpret_cast<const char*>(&jsonChunkType), 4);
        out.write(jsonStr.data(), jsonStr.size());

        // Write Chunk 1 (BIN)
        uint32_t binChunkLen = static_cast<uint32_t>(binBuffer.size());
        uint32_t binChunkType = 0x004E4942; // "BIN\0"
        out.write(reinterpret_cast<const char*>(&binChunkLen), 4);
        out.write(reinterpret_cast<const char*>(&binChunkType), 4);
        out.write(reinterpret_cast<const char*>(binBuffer.data()), binBuffer.size());

        out.close();

        // Write Manifest file beside GLB
        std::filesystem::path manifestPath = path;
        manifestPath.replace_extension(".export-manifest.json");
        manifest.WriteToFile(manifestPath);

        return true;
    }

    bool ModelExporter::ExportToObj(
        const std::filesystem::path& path,
        const DagorFormats::RendInstMesh& mesh,
        const ExportManifest& manifest)
    {
        if (mesh.vertices.empty() || mesh.indices.empty())
        {
            return false;
        }

        std::ofstream out(path);
        if (!out)
        {
            return false;
        }

        // Write header
        out << "# Exported from DagorAssetsExtractor\n";
        out << "# Resource: " << manifest.sourceResource << "\n";
        out << "# Source Pack: " << manifest.sourcePack << "\n\n";

        out << "g " << mesh.name << "\n\n";

        // Vertices: positions (v x y z)
        for (const auto& v : mesh.vertices)
        {
            out << std::fixed << std::setprecision(6) 
                << "v " << v.x << " " << v.y << " " << v.z << "\n";
        }
        out << "\n";

        // Texture coordinates (vt u v). glTF UV space has origin at top-left, OBJ typically uses bottom-left. 
        // We invert V to fit standard texture viewers.
        for (const auto& v : mesh.vertices)
        {
            out << std::fixed << std::setprecision(6) 
                << "vt " << v.u << " " << (1.0f - v.v) << "\n";
        }
        out << "\n";

        // Normals (vn nx ny nz)
        for (const auto& v : mesh.vertices)
        {
            out << std::fixed << std::setprecision(6) 
                << "vn " << v.nx << " " << v.ny << " " << v.nz << "\n";
        }
        out << "\n";

        // Faces (f v/vt/vn v/vt/vn v/vt/vn)
        // OBJ indices are 1-based.
        for (size_t i = 0; i < mesh.indices.size(); i += 3)
        {
            if (i + 2 < mesh.indices.size())
            {
                uint32_t i0 = mesh.indices[i] + 1;
                uint32_t i1 = mesh.indices[i + 1] + 1;
                uint32_t i2 = mesh.indices[i + 2] + 1;

                out << "f " << i0 << "/" << i0 << "/" << i0 << " "
                    << i1 << "/" << i1 << "/" << i1 << " "
                    << i2 << "/" << i2 << "/" << i2 << "\n";
            }
        }

        out.close();

        // Write Manifest file beside OBJ
        std::filesystem::path manifestPath = path;
        manifestPath.replace_extension(".export-manifest.json");
        manifest.WriteToFile(manifestPath);

        return true;
    }
}
