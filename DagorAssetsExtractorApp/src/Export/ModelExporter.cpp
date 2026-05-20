#include "Export/ModelExporter.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <array>
#include <limits>
#include <iomanip>
#include <unordered_map>
#include <cctype>
#include <string_view>
#include <charconv>
#include <cmath>

namespace DagorAssetsExtractor::Export
{
    namespace
    {
        std::string JsonEscape(std::string_view value)
        {
            std::ostringstream escaped;
            escaped << std::hex << std::uppercase << std::setfill('0');
            for (unsigned char c : value)
            {
                switch (c)
                {
                case '\"': escaped << "\\\""; break;
                case '\\': escaped << "\\\\"; break;
                case '\b': escaped << "\\b"; break;
                case '\f': escaped << "\\f"; break;
                case '\n': escaped << "\\n"; break;
                case '\r': escaped << "\\r"; break;
                case '\t': escaped << "\\t"; break;
                default:
                    if (c < 0x20 || c >= 0x80)
                    {
                        escaped << "\\u" << std::setw(4) << static_cast<unsigned int>(c);
                    }
                    else
                    {
                        escaped << static_cast<char>(c);
                    }
                    break;
                }
            }
            return escaped.str();
        }

        bool PrimitiveHasValidIndices(
            const std::vector<uint32_t>& indices,
            size_t vertexCount,
            const DagorFormats::RendInstPrimitive& primitive)
        {
            if (primitive.indexCount == 0 ||
                primitive.firstIndex >= indices.size() ||
                primitive.indexCount > indices.size() - primitive.firstIndex ||
                (primitive.indexCount % 3) != 0)
            {
                return false;
            }

            for (uint32_t i = 0; i < primitive.indexCount; ++i)
            {
                if (indices[static_cast<size_t>(primitive.firstIndex) + i] >= vertexCount)
                {
                    return false;
                }
            }
            return true;
        }

        std::string JsonFloat(float value)
        {
            // glTF validators compare accessor bounds as JSON doubles against
            // binary float32 values promoted to double, so write that exact
            // promoted value instead of the shortest float32 round-trip text.
            const double exactValue = static_cast<double>(value);
            char buffer[64] = {};
            const auto result = std::to_chars(
                buffer,
                buffer + sizeof(buffer),
                exactValue,
                std::chars_format::general,
                std::numeric_limits<double>::max_digits10);

            if (result.ec == std::errc())
            {
                return std::string(buffer, result.ptr);
            }

            std::ostringstream fallback;
            fallback << std::defaultfloat << std::setprecision(std::numeric_limits<double>::max_digits10) << exactValue;
            return fallback.str();
        }

        bool NormalizeVector(float& x, float& y, float& z)
        {
            if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
            {
                x = y = z = 0.0f;
                return false;
            }

            const float lengthSq = (x * x) + (y * y) + (z * z);
            if (lengthSq <= 1e-12f)
            {
                x = y = z = 0.0f;
                return false;
            }

            const float invLength = 1.0f / std::sqrt(lengthSq);
            x *= invLength;
            y *= invLength;
            z *= invLength;
            return true;
        }

        std::array<float, 2> ConvertUvForGltf(float u, float v, DagorFormats::UvTransform transform)
        {
            (void)transform;
            return { u, 1.0f - v };
        }

        void ConvertMatrixHandedness(std::array<float, 16>& m)
        {
            m[2] = -m[2];
            m[6] = -m[6];
            m[8] = -m[8];
            m[9] = -m[9];
            m[11] = -m[11];
            m[14] = -m[14];
        }

        struct PackedStaticVertex
        {
            float x;
            float y;
            float z;
            float nx;
            float ny;
            float nz;
            float u;
            float v;
        };

        struct PackedSkinnedVertex
        {
            float x;
            float y;
            float z;
            float nx;
            float ny;
            float nz;
            float u;
            float v;
            uint8_t jointIndices[4];
            float jointWeights[4];
        };

        static_assert(sizeof(PackedStaticVertex) == 32);
        static_assert(sizeof(PackedSkinnedVertex) == 52);

        template <typename TVertex>
        size_t RepairNormals(std::vector<TVertex>& vertices, const std::vector<uint32_t>& indices)
        {
            if (vertices.empty())
            {
                return 0;
            }

            std::vector<std::array<float, 3>> accumulated(vertices.size(), { 0.0f, 0.0f, 0.0f });
            for (size_t i = 0; i + 2 < indices.size(); i += 3)
            {
                const uint32_t i0 = indices[i];
                const uint32_t i1 = indices[i + 1];
                const uint32_t i2 = indices[i + 2];
                if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size() ||
                    i0 == i1 || i1 == i2 || i0 == i2)
                {
                    continue;
                }

                const auto& a = vertices[i0];
                const auto& b = vertices[i1];
                const auto& c = vertices[i2];
                const float abx = b.x - a.x;
                const float aby = b.y - a.y;
                const float abz = b.z - a.z;
                const float acx = c.x - a.x;
                const float acy = c.y - a.y;
                const float acz = c.z - a.z;
                float nx = (aby * acz) - (abz * acy);
                float ny = (abz * acx) - (abx * acz);
                float nz = (abx * acy) - (aby * acx);
                if (!NormalizeVector(nx, ny, nz))
                {
                    continue;
                }

                for (uint32_t index : { i0, i1, i2 })
                {
                    accumulated[index][0] += nx;
                    accumulated[index][1] += ny;
                    accumulated[index][2] += nz;
                }
            }

            size_t repaired = 0;
            for (size_t i = 0; i < vertices.size(); ++i)
            {
                auto& vertex = vertices[i];
                float nx = vertex.nx;
                float ny = vertex.ny;
                float nz = vertex.nz;
                if (NormalizeVector(nx, ny, nz))
                {
                    vertex.nx = nx;
                    vertex.ny = ny;
                    vertex.nz = nz;
                    continue;
                }

                nx = accumulated[i][0];
                ny = accumulated[i][1];
                nz = accumulated[i][2];
                if (!NormalizeVector(nx, ny, nz))
                {
                    nx = 0.0f;
                    ny = 0.0f;
                    nz = 1.0f;
                }

                vertex.nx = nx;
                vertex.ny = ny;
                vertex.nz = nz;
                ++repaired;
            }

            return repaired;
        }

        template <typename TMesh>
        std::vector<MaterialInfo> BuildEffectiveMaterials(
            const std::vector<TMesh>& meshes,
            const std::vector<MaterialInfo>& inputMaterials)
        {
            std::vector<MaterialInfo> materials = inputMaterials;

            size_t maxMaterialIndex = 0;
            bool hasMaterialIndex = false;
            for (const auto& mesh : meshes)
            {
                for (const auto& primitive : mesh.primitives)
                {
                    if (primitive.indexCount == 0)
                    {
                        continue;
                    }
                    maxMaterialIndex = std::max(maxMaterialIndex, static_cast<size_t>(primitive.materialIndex));
                    hasMaterialIndex = true;
                }
            }

            if (hasMaterialIndex && materials.size() <= maxMaterialIndex)
            {
                materials.resize(maxMaterialIndex + 1);
            }

            for (size_t i = 0; i < materials.size(); ++i)
            {
                if (materials[i].name.empty())
                {
                    materials[i].name = "material_" + std::to_string(i);
                }
            }

            return materials;
        }

        void WriteMaterialTextureSections(std::stringstream& json, const std::vector<MaterialInfo>& materials)
        {
            if (materials.empty())
            {
                return;
            }

            std::vector<int> textureIndices(materials.size(), -1);
            std::vector<std::string> textureUris;
            std::unordered_map<std::string, int> textureIndexByUri;
            for (size_t i = 0; i < materials.size(); ++i)
            {
                if (!materials[i].diffuseTexturePath.empty())
                {
                    const auto existing = textureIndexByUri.find(materials[i].diffuseTexturePath);
                    if (existing != textureIndexByUri.end())
                    {
                        textureIndices[i] = existing->second;
                        continue;
                    }

                    const int textureIndex = static_cast<int>(textureUris.size());
                    textureIndexByUri.emplace(materials[i].diffuseTexturePath, textureIndex);
                    textureUris.push_back(materials[i].diffuseTexturePath);
                    textureIndices[i] = textureIndex;
                }
            }

            json << "  \"materials\": [\n";
            for (size_t i = 0; i < materials.size(); ++i)
            {
                json << "    {\n";
                json << "      \"name\": \"" << JsonEscape(materials[i].name) << "\",\n";
                json << "      \"pbrMetallicRoughness\": {\n";
                if (textureIndices[i] >= 0)
                {
                    json << "        \"baseColorTexture\": { \"index\": " << textureIndices[i] << " },\n";
                }
                else
                {
                    json << "        \"baseColorFactor\": [1, 1, 1, 1],\n";
                }
                json << "        \"metallicFactor\": 0.0,\n";
                json << "        \"roughnessFactor\": 1.0\n";
                json << "      }\n";
                json << "    }";
                if (i + 1 < materials.size()) json << ",";
                json << "\n";
            }
            json << "  ],\n";

            if (textureUris.empty())
            {
                return;
            }

            json << "  \"textures\": [\n";
            for (size_t i = 0; i < textureUris.size(); ++i)
            {
                json << "    { \"sampler\": 0, \"source\": " << i << " }";
                if (i + 1 < textureUris.size()) json << ",";
                json << "\n";
            }
            json << "  ],\n";

            json << "  \"images\": [\n";
            for (size_t i = 0; i < textureUris.size(); ++i)
            {
                json << "    { \"uri\": \"" << JsonEscape(textureUris[i]) << "\" }";
                if (i + 1 < textureUris.size()) json << ",";
                json << "\n";
            }
            json << "  ],\n";

            json << "  \"samplers\": [\n";
            json << "    { \"magFilter\": 9729, \"minFilter\": 9987, \"wrapS\": 10497, \"wrapT\": 10497 }\n";
            json << "  ],\n";
        }
    }

    bool ModelExporter::ExportToGlb(
        const std::filesystem::path& path,
        const std::vector<DagorFormats::RendInstMesh>& lods_in,
        const ExportManifest& manifest,
        const std::vector<MaterialInfo>& materials)
    {
        std::ofstream debugLog("scratch/export_debug.txt", std::ios::app);
        debugLog << "[ExportToGlb] Exporting to " << path.string() << std::endl;

        std::vector<DagorFormats::RendInstMesh> lods;
        for (const auto& lod_in : lods_in)
        {
            if (lod_in.vertices.empty() || lod_in.indices.empty())
            {
                debugLog << "  LOD " << lod_in.name << " skipped: verts=" << lod_in.vertices.size() << ", indices=" << lod_in.indices.size() << std::endl;
                continue;
            }

            DagorFormats::RendInstMesh validLod = lod_in;
            // Remove primitives with 0 indices
            validLod.primitives.erase(
                std::remove_if(validLod.primitives.begin(), validLod.primitives.end(),
                    [](const DagorFormats::RendInstPrimitive& p) { return p.indexCount == 0; }),
                validLod.primitives.end());

            if (validLod.primitives.empty())
            {
                debugLog << "  LOD " << lod_in.name << " had empty primitives, creating default." << std::endl;
                // Create a default primitive if all were empty or none existed
                validLod.primitives.push_back({ 0, 0, static_cast<uint32_t>(validLod.indices.size()) });
            }

            const size_t repairedNormals = RepairNormals(validLod.vertices, validLod.indices);
            if (repairedNormals > 0)
            {
                debugLog << "  LOD " << validLod.name
                         << " repaired " << repairedNormals
                         << " invalid/non-unit normals before GLB export." << std::endl;
            }

            lods.push_back(std::move(validLod));
        }

        if (lods.empty())
        {
            debugLog << "[ExportToGlb] Failed: no valid LODs after filtering. Total input LODs: " << lods_in.size() << std::endl;
            return false;
        }

        // ... rest of code (I will keep my previous instrumentation of std::cout as well if possible, or just replace it)

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

            info.vertByteOffset = binBuffer.size();
            info.vertByteLength = mesh.vertices.size() * sizeof(PackedStaticVertex);

            if (!mesh.vertices.empty())
            {
                std::vector<PackedStaticVertex> packedVertices;
                packedVertices.reserve(mesh.vertices.size());

                for (const auto& v : mesh.vertices)
                {
                    const auto uv = ConvertUvForGltf(v.u, v.v, v.uvTransform);
                    PackedStaticVertex packed{};
                    packed.x = v.x;
                    packed.y = v.y;
                    packed.z = -v.z;
                    packed.nx = v.nx;
                    packed.ny = v.ny;
                    packed.nz = -v.nz;
                    packed.u = uv[0];
                    packed.v = uv[1];
                    packedVertices.push_back(packed);

                    info.minX = std::min(info.minX, v.x);
                    info.minY = std::min(info.minY, v.y);
                    info.minZ = std::min(info.minZ, -v.z);
                    info.maxX = std::max(info.maxX, v.x);
                    info.maxY = std::max(info.maxY, v.y);
                    info.maxZ = std::max(info.maxZ, -v.z);
                }

                const uint8_t* vertBytes = reinterpret_cast<const uint8_t*>(packedVertices.data());
                binBuffer.insert(binBuffer.end(), vertBytes, vertBytes + info.vertByteLength);
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
                std::vector<uint32_t> flippedIndices = mesh.indices;
                for (size_t i = 0; i + 2 < flippedIndices.size(); i += 3)
                {
                    std::swap(flippedIndices[i + 1], flippedIndices[i + 2]);
                }
                const uint8_t* indexBytes = reinterpret_cast<const uint8_t*>(flippedIndices.data());
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
            json << "      \"name\": \"" << JsonEscape(lods[i].name) << "\",\n";
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
        const std::vector<MaterialInfo> effectiveMaterials = BuildEffectiveMaterials(lods, materials);

        // Meshes definition
        json << "  \"meshes\": [\n";
        for (size_t i = 0; i < lods.size(); ++i)
        {
            const auto& mesh = lods[i];
            json << "    {\n";
            json << "      \"name\": \"" << JsonEscape(mesh.name) << "\",\n";
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
                
                if (!effectiveMaterials.empty())
                {
                    size_t matIdx = static_cast<size_t>(prim.materialIndex);
                    if (matIdx >= effectiveMaterials.size()) matIdx = 0;
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

        WriteMaterialTextureSections(json, effectiveMaterials);

        // BufferViews definition
        json << "  \"bufferViews\": [\n";
        // Vertex bufferViews first
        for (size_t i = 0; i < lods.size(); ++i)
        {
            json << "    {\n";
            json << "      \"buffer\": 0,\n";
            json << "      \"byteOffset\": " << binInfos[i].vertByteOffset << ",\n";
            json << "      \"byteLength\": " << binInfos[i].vertByteLength << ",\n";
            json << "      \"byteStride\": " << sizeof(PackedStaticVertex) << ",\n";
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
            json << "      \"max\": [" << JsonFloat(info.maxX) << ", " << JsonFloat(info.maxY) << ", " << JsonFloat(info.maxZ) << "],\n";
            json << "      \"min\": [" << JsonFloat(info.minX) << ", " << JsonFloat(info.minY) << ", " << JsonFloat(info.minZ) << "]\n";
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

    bool ModelExporter::ExportToGlb(
        const std::filesystem::path& path,
        const std::vector<DagorFormats::DynModelMesh>& lods_in,
        const std::vector<DagorFormats::GeomNodeTreeNode>& skeleton,
        const ExportManifest& manifest,
        const std::vector<MaterialInfo>& materials)
    {
        std::ofstream debugLog("scratch/export_debug.txt", std::ios::app);
        debugLog << "[ExportToGlb DynModel] Exporting to " << path.string() << std::endl;

        std::vector<DagorFormats::DynModelMesh> lods;
        for (const auto& lod_in : lods_in)
        {
            if (lod_in.vertices.empty() || lod_in.indices.empty())
            {
                debugLog << "  LOD " << lod_in.name << " skipped: verts=" << lod_in.vertices.size() << ", indices=" << lod_in.indices.size() << std::endl;
                continue;
            }

            DagorFormats::DynModelMesh validLod = lod_in;
            validLod.primitives.erase(
                std::remove_if(validLod.primitives.begin(), validLod.primitives.end(),
                    [&](const DagorFormats::RendInstPrimitive& p)
                    {
                        return !PrimitiveHasValidIndices(validLod.indices, validLod.vertices.size(), p);
                    }),
                validLod.primitives.end());

            if (validLod.primitives.empty())
            {
                DagorFormats::RendInstPrimitive defaultPrimitive{
                    0,
                    0,
                    static_cast<uint32_t>(validLod.indices.size() - (validLod.indices.size() % 3))
                };
                if (!PrimitiveHasValidIndices(validLod.indices, validLod.vertices.size(), defaultPrimitive))
                {
                    debugLog << "  LOD " << lod_in.name << " skipped: no valid primitive/index ranges." << std::endl;
                    continue;
                }
                debugLog << "  LOD " << lod_in.name << " had no valid primitives, creating bounded default." << std::endl;
                validLod.primitives.push_back(defaultPrimitive);
            }

            const size_t repairedNormals = RepairNormals(validLod.vertices, validLod.indices);
            if (repairedNormals > 0)
            {
                debugLog << "  DynModel mesh " << validLod.name
                         << " repaired " << repairedNormals
                         << " invalid/non-unit normals before GLB export." << std::endl;
            }

            lods.push_back(std::move(validLod));
        }

        if (lods.empty())
        {
            debugLog << "[ExportToGlb DynModel] Failed: no valid LODs after filtering. Total input LODs: " << lods_in.size() << std::endl;
            return false;
        }

        const auto transposeMatrix = [](const std::array<float, 16>& matrix)
        {
            return std::array<float, 16>{
                matrix[0], matrix[4], matrix[8],  matrix[12],
                matrix[1], matrix[5], matrix[9],  matrix[13],
                matrix[2], matrix[6], matrix[10], matrix[14],
                matrix[3], matrix[7], matrix[11], matrix[15],
            };
        };

        // Strip skeleton from DynModel per user request
        const bool useSkinning = false;
        const bool usesSkeletonNodes = false;

        std::unordered_map<std::string, size_t> skeletonNodeLookup;
        skeletonNodeLookup.reserve(skeleton.size());
        if (!skeleton.empty())
        {
            for (size_t i = 0; i < skeleton.size(); ++i)
            {
                skeletonNodeLookup.emplace(skeleton[i].name, i);
            }
        }

        std::vector<std::array<float, 16>> meshNodeMatrices(lods.size());
        std::vector<bool> hasMeshNodeMatrix(lods.size(), false);
        if (!skeleton.empty())
        {
            for (size_t i = 0; i < lods.size(); ++i)
            {
                if (lods[i].useSkinning || lods[i].parentNodeName.empty())
                {
                    continue;
                }

                const auto it = skeletonNodeLookup.find(lods[i].parentNodeName);
                if (it == skeletonNodeLookup.end())
                {
                    continue;
                }

                meshNodeMatrices[i] = transposeMatrix(skeleton[it->second].worldTransform);
                ConvertMatrixHandedness(meshNodeMatrices[i]);
                hasMeshNodeMatrix[i] = true;
            }
        }

        std::vector<std::vector<size_t>> attachedMeshChildren(skeleton.size());
        std::vector<bool> meshAttachedToSkeleton(lods.size(), false);

        if (useSkinning)
        {
            for (auto& mesh : lods)
            {
                if (!mesh.useSkinning || mesh.parentNodeName.empty())
                {
                    continue;
                }

                const auto it = skeletonNodeLookup.find(mesh.parentNodeName);
                if (it == skeletonNodeLookup.end() || it->second > 255)
                {
                    debugLog << "  Skinned mesh " << mesh.name
                             << " has no matching skeleton node for parent '"
                             << mesh.parentNodeName << "'; preserving parsed joint data." << std::endl;
                    continue;
                }

                const uint8_t jointIndex = static_cast<uint8_t>(it->second);
                for (auto& vertex : mesh.vertices)
                {
                    vertex.jointIndices[0] = jointIndex;
                    vertex.jointIndices[1] = 0;
                    vertex.jointIndices[2] = 0;
                    vertex.jointIndices[3] = 0;
                    vertex.jointWeights[0] = 1.0f;
                    vertex.jointWeights[1] = 0.0f;
                    vertex.jointWeights[2] = 0.0f;
                    vertex.jointWeights[3] = 0.0f;
                }
            }
        }

        const size_t vertexAccessorCountPerMesh = useSkinning ? 5 : 3;

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
            info.vertByteOffset = binBuffer.size();
            info.vertByteLength = mesh.vertices.size() * sizeof(PackedSkinnedVertex);

            if (!mesh.vertices.empty())
            {
                std::vector<PackedSkinnedVertex> packedVertices;
                packedVertices.reserve(mesh.vertices.size());

                for (const auto& v : mesh.vertices)
                {
                    const auto uv = ConvertUvForGltf(v.u, v.v, v.uvTransform);
                    PackedSkinnedVertex packed{};
                    packed.x = v.x;
                    packed.y = v.y;
                    packed.z = -v.z;
                    packed.nx = v.nx;
                    packed.ny = v.ny;
                    packed.nz = -v.nz;
                    packed.u = uv[0];
                    packed.v = uv[1];
                    for (int j = 0; j < 4; ++j)
                    {
                        packed.jointIndices[j] = v.jointIndices[j];
                        packed.jointWeights[j] = v.jointWeights[j];
                    }
                    packedVertices.push_back(packed);

                    info.minX = std::min(info.minX, v.x);
                    info.minY = std::min(info.minY, v.y);
                    info.minZ = std::min(info.minZ, -v.z);
                    info.maxX = std::max(info.maxX, v.x);
                    info.maxY = std::max(info.maxY, v.y);
                    info.maxZ = std::max(info.maxZ, -v.z);
                }

                const uint8_t* vertBytes = reinterpret_cast<const uint8_t*>(packedVertices.data());
                binBuffer.insert(binBuffer.end(), vertBytes, vertBytes + info.vertByteLength);
            }
            else
            {
                info.minX = info.minY = info.minZ = 0.0f;
                info.maxX = info.maxY = info.maxZ = 0.0f;
            }

            while (binBuffer.size() % 4 != 0) binBuffer.push_back(0);

            info.indexByteOffset = binBuffer.size();
            info.indexByteLength = mesh.indices.size() * sizeof(uint32_t);

            if (!mesh.indices.empty())
            {
                std::vector<uint32_t> flippedIndices = mesh.indices;
                for (size_t i = 0; i + 2 < flippedIndices.size(); i += 3)
                {
                    std::swap(flippedIndices[i + 1], flippedIndices[i + 2]);
                }
                const uint8_t* indexBytes = reinterpret_cast<const uint8_t*>(flippedIndices.data());
                binBuffer.insert(binBuffer.end(), indexBytes, indexBytes + info.indexByteLength);
            }

            binInfos.push_back(info);
            while (binBuffer.size() % 4 != 0) binBuffer.push_back(0);
        }

        // Add Inverse Bind Matrices to BIN buffer if skinning is enabled
        size_t ibmByteOffset = 0;
        size_t ibmByteLength = 0;
        if (useSkinning)
        {
            ibmByteOffset = binBuffer.size();
            ibmByteLength = skeleton.size() * 16 * sizeof(float);
            for (const auto& node : skeleton)
            {
                std::array<float, 16> gltfInverseBind = transposeMatrix(node.inverseBindTransform);
                ConvertMatrixHandedness(gltfInverseBind);
                const uint8_t* matrixBytes = reinterpret_cast<const uint8_t*>(gltfInverseBind.data());
                binBuffer.insert(binBuffer.end(), matrixBytes, matrixBytes + 16 * sizeof(float));
            }
            while (binBuffer.size() % 4 != 0) binBuffer.push_back(0);
        }

        // Pre-calculate accessor indices for indices
        std::vector<std::vector<size_t>> indexAccessorIndices(lods.size());
        size_t currentAccessor = lods.size() * vertexAccessorCountPerMesh;
        for (size_t i = 0; i < lods.size(); ++i)
        {
            size_t primCount = lods[i].primitives.size();
            if (primCount == 0) primCount = 1; // Default primitive
            for (size_t p = 0; p < primCount; ++p)
            {
                indexAccessorIndices[i].push_back(currentAccessor++);
            }
        }
        size_t ibmAccessorIndex = currentAccessor;
        const std::vector<MaterialInfo> effectiveMaterials = BuildEffectiveMaterials(lods, materials);

        // Generate JSON
        std::stringstream json;
        json << "{\n";
        json << "  \"asset\": {\n";
        json << "    \"version\": \"2.0\",\n";
        json << "    \"generator\": \"DagorAssetsExtractor\"\n";
        json << "  },\n";

        json << "  \"scene\": 0,\n";
        json << "  \"scenes\": [\n";
        json << "    {\n";
        json << "      \"nodes\": [0]\n"; // Root of everything (skeleton root + meshes)
        json << "    }\n";
        json << "  ],\n";

        // Nodes definition
        json << "  \"nodes\": [\n";
        // Node 0: Global root
        json << "    {\n";
        json << "      \"name\": \"Root\",\n";
        json << "      \"children\": [";
        if (usesSkeletonNodes)
        {
            bool first = true;
            for (size_t i = 0; i < skeleton.size(); ++i)
            {
                if (skeleton[i].parentIndex == DagorFormats::GeomNodeTreeNode::InvalidIndex)
                {
                    if (!first) json << ", ";
                    json << (i + 1);
                    first = false;
                }
            }
            for (size_t i = 0; i < lods.size(); ++i)
            {
                if (meshAttachedToSkeleton[i])
                {
                    continue;
                }
                if (!first) json << ", ";
                json << (skeleton.size() + 1 + i);
                first = false;
            }
        }
        else
        {
            // Mesh nodes only
            for (size_t i = 0; i < lods.size(); ++i)
            {
                json << (i + 1);
                if (i + 1 < lods.size()) json << ", ";
            }
        }
        json << "]\n";
        json << "    },\n";

        if (usesSkeletonNodes)
        {
            for (size_t i = 0; i < skeleton.size(); ++i)
            {
                const auto& node = skeleton[i];
                std::array<float, 16> gltfMatrix = transposeMatrix(node.localTransform);
                ConvertMatrixHandedness(gltfMatrix);
                json << "    {\n";
                json << "      \"name\": \"" << JsonEscape(node.name) << "\",\n";
                json << "      \"matrix\": [";
                for (int m = 0; m < 16; ++m)
                {
                    json << JsonFloat(gltfMatrix[m]);
                    if (m < 15) json << ", ";
                }
                json << "]";

                std::vector<size_t> childNodes;
                childNodes.reserve(node.childIndices.size() + attachedMeshChildren[i].size());
                for (uint16_t childIndex : node.childIndices)
                {
                    childNodes.push_back(static_cast<size_t>(childIndex) + 1);
                }
                for (size_t meshIndex : attachedMeshChildren[i])
                {
                    childNodes.push_back(skeleton.size() + 1 + meshIndex);
                }

                if (!childNodes.empty())
                {
                    json << ",\n      \"children\": [";
                    for (size_t c = 0; c < childNodes.size(); ++c)
                    {
                        json << childNodes[c];
                        if (c + 1 < childNodes.size()) json << ", ";
                    }
                    json << "]";
                }
                json << "\n";
                json << "    },\n";
            }
        }

        // Mesh nodes
        for (size_t i = 0; i < lods.size(); ++i)
        {
            json << "    {\n";
            json << "      \"name\": \"" << JsonEscape(lods[i].name) << "\",\n";
            json << "      \"mesh\": " << i;
            if (hasMeshNodeMatrix[i])
            {
                json << ",\n      \"matrix\": [";
                for (int m = 0; m < 16; ++m)
                {
                    json << JsonFloat(meshNodeMatrices[i][m]);
                    if (m < 15) json << ", ";
                }
                json << "]";
            }
            if (useSkinning && lods[i].useSkinning)
            {
                json << ",\n      \"skin\": 0";
            }
            json << "\n";
            json << "    }";
            if (i + 1 < lods.size()) json << ",";
            json << "\n";
        }
        json << "  ],\n";

        if (useSkinning)
        {
            json << "  \"skins\": [\n";
            json << "    {\n";
            json << "      \"inverseBindMatrices\": " << ibmAccessorIndex << ",\n"; // Accessor index
            json << "      \"joints\": [";
            for (size_t i = 0; i < skeleton.size(); ++i)
            {
                json << (i + 1);
                if (i + 1 < skeleton.size()) json << ", ";
            }
            json << "]\n";
            json << "    }\n";
            json << "  ],\n";
        }

        // Meshes definition
        json << "  \"meshes\": [\n";
        for (size_t i = 0; i < lods.size(); ++i)
        {
            const auto& mesh = lods[i];
            json << "    {\n";
            json << "      \"name\": \"" << JsonEscape(mesh.name) << "\",\n";
            json << "      \"primitives\": [\n";
            
            for (size_t p = 0; p < mesh.primitives.size(); ++p)
            {
                const auto& prim = mesh.primitives[p];
                const size_t accessorBase = i * vertexAccessorCountPerMesh;
                json << "        {\n";
                json << "          \"attributes\": {\n";
                json << "            \"POSITION\": " << accessorBase << ",\n";
                json << "            \"NORMAL\": " << (accessorBase + 1) << ",\n";
                json << "            \"TEXCOORD_0\": " << (accessorBase + 2) << "";
                if (useSkinning && mesh.useSkinning)
                {
                    json << ",\n            \"JOINTS_0\": " << (accessorBase + 3) << ",\n";
                    json << "            \"WEIGHTS_0\": " << (accessorBase + 4) << "\n";
                }
                else
                {
                    json << "\n";
                }
                json << "          },\n";
                json << "          \"indices\": " << indexAccessorIndices[i][p] << ",\n";
                
                if (!effectiveMaterials.empty())
                {
                    size_t matIdx = static_cast<size_t>(prim.materialIndex);
                    if (matIdx >= effectiveMaterials.size()) matIdx = 0;
                    json << "          \"material\": " << matIdx << ",\n";
                }
                
                json << "          \"mode\": 4\n";
                json << "        }";
                if (p + 1 < mesh.primitives.size()) json << ",";
                json << "\n";
            }
            json << "      ]\n";
            json << "    }";
            if (i + 1 < lods.size()) json << ",";
            json << "\n";
        }
        json << "  ],\n";

        WriteMaterialTextureSections(json, effectiveMaterials);

        // BufferViews
        json << "  \"bufferViews\": [\n";
        // Vertex bufferViews
        for (size_t i = 0; i < lods.size(); ++i)
        {
            json << "    {\n";
            json << "      \"buffer\": 0,\n";
            json << "      \"byteOffset\": " << binInfos[i].vertByteOffset << ",\n";
            json << "      \"byteLength\": " << binInfos[i].vertByteLength << ",\n";
            json << "      \"byteStride\": " << sizeof(PackedSkinnedVertex) << ",\n";
            json << "      \"target\": 34962\n";
            json << "    },\n";
        }
        // Index bufferViews
        for (size_t i = 0; i < lods.size(); ++i)
        {
            json << "    {\n";
            json << "      \"buffer\": 0,\n";
            json << "      \"byteOffset\": " << binInfos[i].indexByteOffset << ",\n";
            json << "      \"byteLength\": " << binInfos[i].indexByteLength << ",\n";
            json << "      \"target\": 34963\n";
            json << "    }";
            if (i + 1 < lods.size() || useSkinning) json << ",\n";
            else json << "\n";
        }
        if (useSkinning)
        {
            // IBM bufferView
            json << "    {\n";
            json << "      \"buffer\": 0,\n";
            json << "      \"byteOffset\": " << ibmByteOffset << ",\n";
            json << "      \"byteLength\": " << ibmByteLength << "\n";
            json << "    }\n";
        }
        json << "  ],\n";

        // Accessors
        json << "  \"accessors\": [\n";
        for (size_t i = 0; i < lods.size(); ++i)
        {
            const auto& mesh = lods[i];
            const auto& info = binInfos[i];

            json << "    { \"bufferView\": " << i << ", \"byteOffset\": 0, \"componentType\": 5126, \"count\": " << mesh.vertices.size() << ", \"type\": \"VEC3\", \"max\": [" << JsonFloat(info.maxX) << "," << JsonFloat(info.maxY) << "," << JsonFloat(info.maxZ) << "], \"min\": [" << JsonFloat(info.minX) << "," << JsonFloat(info.minY) << "," << JsonFloat(info.minZ) << "] },\n";
            json << "    { \"bufferView\": " << i << ", \"byteOffset\": 12, \"componentType\": 5126, \"count\": " << mesh.vertices.size() << ", \"type\": \"VEC3\" },\n";
            json << "    { \"bufferView\": " << i << ", \"byteOffset\": 24, \"componentType\": 5126, \"count\": " << mesh.vertices.size() << ", \"type\": \"VEC2\" }";
            if (useSkinning)
            {
                json << ",\n";
                json << "    { \"bufferView\": " << i << ", \"byteOffset\": 32, \"componentType\": 5121, \"count\": " << mesh.vertices.size() << ", \"type\": \"VEC4\" },\n";
                json << "    { \"bufferView\": " << i << ", \"byteOffset\": 36, \"componentType\": 5126, \"count\": " << mesh.vertices.size() << ", \"type\": \"VEC4\" },\n";
            }
            else
            {
                json << ",\n";
            }
        }

        // Index accessors
        for (size_t i = 0; i < lods.size(); ++i)
        {
            const auto& mesh = lods[i];
            for (size_t p = 0; p < mesh.primitives.size(); ++p)
            {
                const auto& prim = mesh.primitives[p];
                json << "    { \"bufferView\": " << (lods.size() + i) << ", \"byteOffset\": " << (prim.firstIndex * sizeof(uint32_t)) << ", \"componentType\": 5125, \"count\": " << prim.indexCount << ", \"type\": \"SCALAR\" }";
                if (i + 1 < lods.size() || p + 1 < mesh.primitives.size() || useSkinning) json << ",\n";
                else json << "\n";
            }
        }

        if (useSkinning)
        {
            // IBM accessor
            json << "    { \"bufferView\": " << (lods.size() * 2) << ", \"byteOffset\": 0, \"componentType\": 5126, \"count\": " << skeleton.size() << ", \"type\": \"MAT4\" }\n";
        }
        json << "  ],\n";

        json << "  \"buffers\": [ { \"byteLength\": " << binBuffer.size() << " } ]\n";
        json << "}\n";

        std::string jsonStr = json.str();
        while (jsonStr.size() % 4 != 0) jsonStr.push_back(' ');

        std::ofstream out(path, std::ios::binary);
        if (!out)
        {
            debugLog << "[ExportToGlb] Failed: could not open output file: " << path.string() << std::endl;
            return false;
        }

        uint32_t magic = 0x46546C67;
        uint32_t version = 2;
        uint32_t totalLength = 12 + 8 + static_cast<uint32_t>(jsonStr.size()) + 8 + static_cast<uint32_t>(binBuffer.size());

        out.write(reinterpret_cast<const char*>(&magic), 4);
        out.write(reinterpret_cast<const char*>(&version), 4);
        out.write(reinterpret_cast<const char*>(&totalLength), 4);

        uint32_t jsonChunkLen = static_cast<uint32_t>(jsonStr.size());
        uint32_t jsonChunkType = 0x4E4F534A;
        out.write(reinterpret_cast<const char*>(&jsonChunkLen), 4);
        out.write(reinterpret_cast<const char*>(&jsonChunkType), 4);
        out.write(jsonStr.data(), jsonStr.size());

        uint32_t binChunkLen = static_cast<uint32_t>(binBuffer.size());
        uint32_t binChunkType = 0x004E4942;
        out.write(reinterpret_cast<const char*>(&binChunkLen), 4);
        out.write(reinterpret_cast<const char*>(&binChunkType), 4);
        out.write(reinterpret_cast<const char*>(binBuffer.data()), binBuffer.size());

        out.close();

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
