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
#include <optional>

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

        bool IsIdentityMatrix(const std::array<float, 16>& matrix)
        {
            constexpr std::array<float, 16> kIdentity = {
                1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f
            };

            for (size_t i = 0; i < matrix.size(); ++i)
            {
                if (std::fabs(matrix[i] - kIdentity[i]) > 1e-6f)
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
            uint16_t jointIndices[4];
            float jointWeights[4];
        };

        static_assert(sizeof(PackedStaticVertex) == 32);
        static_assert(sizeof(PackedSkinnedVertex) == 56);

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

        template <typename TVertex>
        void RecomputePackedNormals(std::vector<TVertex>& vertices, const std::vector<uint32_t>& indices)
        {
            if (vertices.empty())
            {
                return;
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

                accumulated[i0][0] += nx;
                accumulated[i0][1] += ny;
                accumulated[i0][2] += nz;
                accumulated[i1][0] += nx;
                accumulated[i1][1] += ny;
                accumulated[i1][2] += nz;
                accumulated[i2][0] += nx;
                accumulated[i2][1] += ny;
                accumulated[i2][2] += nz;
            }

            for (size_t i = 0; i < vertices.size(); ++i)
            {
                float faceNx = accumulated[i][0];
                float faceNy = accumulated[i][1];
                float faceNz = accumulated[i][2];
                const bool hasFaceNormal = NormalizeVector(faceNx, faceNy, faceNz);

                float nx = faceNx;
                float ny = faceNy;
                float nz = faceNz;
                if (!hasFaceNormal)
                {
                    nx = vertices[i].nx;
                    ny = vertices[i].ny;
                    nz = vertices[i].nz;
                    if (!NormalizeVector(nx, ny, nz))
                    {
                        nx = 0.0f;
                        ny = 0.0f;
                        nz = 1.0f;
                    }
                }

                vertices[i].nx = nx;
                vertices[i].ny = ny;
                vertices[i].nz = nz;
            }
        }

        void NormalizeJointWeights(PackedSkinnedVertex& vertex)
        {
            float sum = 0.0f;
            for (float weight : vertex.jointWeights)
            {
                if (std::isfinite(weight) && weight > 0.0f)
                {
                    sum += weight;
                }
            }

            if (sum <= 1e-8f)
            {
                vertex.jointIndices[0] = 0;
                vertex.jointIndices[1] = 0;
                vertex.jointIndices[2] = 0;
                vertex.jointIndices[3] = 0;
                vertex.jointWeights[0] = 1.0f;
                vertex.jointWeights[1] = 0.0f;
                vertex.jointWeights[2] = 0.0f;
                vertex.jointWeights[3] = 0.0f;
                return;
            }

            const float invSum = 1.0f / sum;
            for (float& weight : vertex.jointWeights)
            {
                weight = (std::isfinite(weight) && weight > 0.0f) ? (weight * invSum) : 0.0f;
            }
        }

        template <typename TVertex>
        bool IsDegenerateTriangle(
            const std::vector<TVertex>& vertices,
            uint32_t i0,
            uint32_t i1,
            uint32_t i2)
        {
            if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size() ||
                i0 == i1 || i1 == i2 || i0 == i2)
            {
                return true;
            }

            const auto& a = vertices[i0];
            const auto& b = vertices[i1];
            const auto& c = vertices[i2];
            const double abx = static_cast<double>(b.x) - static_cast<double>(a.x);
            const double aby = static_cast<double>(b.y) - static_cast<double>(a.y);
            const double abz = static_cast<double>(b.z) - static_cast<double>(a.z);
            const double acx = static_cast<double>(c.x) - static_cast<double>(a.x);
            const double acy = static_cast<double>(c.y) - static_cast<double>(a.y);
            const double acz = static_cast<double>(c.z) - static_cast<double>(a.z);
            const double nx = (aby * acz) - (abz * acy);
            const double ny = (abz * acx) - (abx * acz);
            const double nz = (abx * acy) - (aby * acx);
            const double areaSq = (nx * nx) + (ny * ny) + (nz * nz);
            return !std::isfinite(areaSq) || areaSq <= 1e-20;
        }

        template <typename TMesh>
        size_t FilterDegenerateTriangles(TMesh& mesh)
        {
            std::vector<DagorFormats::RendInstPrimitive> sourcePrimitives = mesh.primitives;
            if (sourcePrimitives.empty())
            {
                sourcePrimitives.push_back({ 0, 0, static_cast<uint32_t>(mesh.indices.size() - (mesh.indices.size() % 3)) });
            }

            std::vector<uint32_t> filteredIndices;
            filteredIndices.reserve(mesh.indices.size());
            std::vector<DagorFormats::RendInstPrimitive> filteredPrimitives;
            filteredPrimitives.reserve(sourcePrimitives.size());
            size_t removedTriangles = 0;

            for (const DagorFormats::RendInstPrimitive& primitive : sourcePrimitives)
            {
                if (!PrimitiveHasValidIndices(mesh.indices, mesh.vertices.size(), primitive))
                {
                    removedTriangles += primitive.indexCount / 3;
                    continue;
                }

                const uint32_t firstIndex = static_cast<uint32_t>(filteredIndices.size());
                for (uint32_t offset = 0; offset + 2 < primitive.indexCount; offset += 3)
                {
                    const size_t src = static_cast<size_t>(primitive.firstIndex) + offset;
                    const uint32_t i0 = mesh.indices[src];
                    const uint32_t i1 = mesh.indices[src + 1];
                    const uint32_t i2 = mesh.indices[src + 2];
                    if (IsDegenerateTriangle(mesh.vertices, i0, i1, i2))
                    {
                        ++removedTriangles;
                        continue;
                    }

                    filteredIndices.push_back(i0);
                    filteredIndices.push_back(i1);
                    filteredIndices.push_back(i2);
                }

                const uint32_t indexCount = static_cast<uint32_t>(filteredIndices.size() - firstIndex);
                if (indexCount > 0)
                {
                    filteredPrimitives.push_back({ primitive.materialIndex, firstIndex, indexCount });
                }
            }

            mesh.indices = std::move(filteredIndices);
            mesh.primitives = std::move(filteredPrimitives);
            return removedTriangles;
        }

        struct MaterialSelection
        {
            std::vector<MaterialInfo> materials;
            std::unordered_map<uint32_t, size_t> remap;
        };

        bool MaterialSlotHasTexture(const MaterialSelection& selection, uint32_t materialSlot)
        {
            const auto remapIt = selection.remap.find(materialSlot);
            if (remapIt == selection.remap.end() || remapIt->second >= selection.materials.size())
            {
                return false;
            }
            return !selection.materials[remapIt->second].diffuseTexturePath.empty();
        }

        template <typename TMesh>
        MaterialSelection SelectUsedMaterials(
            const std::vector<TMesh>& meshes,
            const std::vector<MaterialInfo>& inputMaterials)
        {
            std::vector<uint32_t> usedMaterialSlots;
            for (const auto& mesh : meshes)
            {
                for (const auto& primitive : mesh.primitives)
                {
                    if (primitive.indexCount == 0)
                    {
                        continue;
                    }
                    usedMaterialSlots.push_back(primitive.materialIndex);
                }
            }

            std::sort(usedMaterialSlots.begin(), usedMaterialSlots.end());
            usedMaterialSlots.erase(std::unique(usedMaterialSlots.begin(), usedMaterialSlots.end()), usedMaterialSlots.end());

            MaterialSelection selection;
            selection.materials.reserve(usedMaterialSlots.size());
            for (uint32_t materialSlot : usedMaterialSlots)
            {
                MaterialInfo material;
                if (materialSlot < inputMaterials.size())
                {
                    material = inputMaterials[materialSlot];
                }

                if (material.name.empty())
                {
                    material.name = "material_" + std::to_string(materialSlot);
                }

                selection.remap.emplace(materialSlot, selection.materials.size());
                selection.materials.push_back(std::move(material));
            }

            return selection;
        }

        std::string LowerExtension(const std::filesystem::path& path)
        {
            std::string ext = path.extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return ext;
        }

        std::string MimeTypeForTexturePath(const std::filesystem::path& path)
        {
            const std::string ext = LowerExtension(path);
            if (ext == ".png")
            {
                return "image/png";
            }
            if (ext == ".jpg" || ext == ".jpeg")
            {
                return "image/jpeg";
            }
            return {};
        }

        std::optional<std::vector<uint8_t>> ReadBinaryFile(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                return std::nullopt;
            }

            file.seekg(0, std::ios::end);
            const std::streamoff size = file.tellg();
            if (size <= 0)
            {
                return std::nullopt;
            }
            file.seekg(0, std::ios::beg);

            std::vector<uint8_t> bytes(static_cast<size_t>(size));
            if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
            {
                return std::nullopt;
            }
            return bytes;
        }

        struct GlbImageInfo
        {
            std::string uri;
            std::string mimeType;
            size_t byteOffset = 0;
            size_t byteLength = 0;
            size_t embeddedOrdinal = 0;
            bool embedded = false;
        };

        struct GlbTextureSections
        {
            std::vector<int> materialTextureIndices;
            std::vector<GlbImageInfo> images;
            size_t embeddedImageCount = 0;
        };

        GlbTextureSections PrepareMaterialTextures(
            const std::vector<MaterialInfo>& materials,
            const GlbExportOptions& options,
            std::vector<uint8_t>& binBuffer,
            std::ostream& debugLog)
        {
            GlbTextureSections sections;
            sections.materialTextureIndices.assign(materials.size(), -1);

            std::unordered_map<std::string, int> textureIndexByKey;
            for (size_t i = 0; i < materials.size(); ++i)
            {
                const MaterialInfo& material = materials[i];
                if (material.diffuseTexturePath.empty())
                {
                    continue;
                }

                const std::string dedupeKey = !material.diffuseTextureSourcePath.empty()
                    ? ("source:" + material.diffuseTextureSourcePath.lexically_normal().generic_string())
                    : ("uri:" + material.diffuseTexturePath);
                const auto existing = textureIndexByKey.find(dedupeKey);
                if (existing != textureIndexByKey.end())
                {
                    sections.materialTextureIndices[i] = existing->second;
                    continue;
                }

                GlbImageInfo image;
                image.uri = material.diffuseTexturePath;

                if (options.embedTexturesInGlb && !material.diffuseTextureSourcePath.empty())
                {
                    image.mimeType = MimeTypeForTexturePath(material.diffuseTextureSourcePath);
                    if (!image.mimeType.empty())
                    {
                        if (std::optional<std::vector<uint8_t>> bytes = ReadBinaryFile(material.diffuseTextureSourcePath))
                        {
                            while (binBuffer.size() % 4 != 0)
                            {
                                binBuffer.push_back(0);
                            }
                            image.byteOffset = binBuffer.size();
                            image.byteLength = bytes->size();
                            image.embeddedOrdinal = sections.embeddedImageCount++;
                            image.embedded = true;
                            binBuffer.insert(binBuffer.end(), bytes->begin(), bytes->end());
                            while (binBuffer.size() % 4 != 0)
                            {
                                binBuffer.push_back(0);
                            }
                        }
                        else
                        {
                            debugLog << "  Texture embed skipped; could not read "
                                     << material.diffuseTextureSourcePath.string() << std::endl;
                        }
                    }
                    else
                    {
                        debugLog << "  Texture embed skipped; unsupported glTF image type for "
                                 << material.diffuseTextureSourcePath.string() << std::endl;
                    }
                }

                const int textureIndex = static_cast<int>(sections.images.size());
                textureIndexByKey.emplace(dedupeKey, textureIndex);
                sections.images.push_back(std::move(image));
                sections.materialTextureIndices[i] = textureIndex;
            }

            return sections;
        }

        void WriteMaterialTextureSections(
            std::stringstream& json,
            const std::vector<MaterialInfo>& materials,
            const GlbTextureSections& textureSections,
            size_t imageBufferViewBase)
        {
            if (materials.empty())
            {
                return;
            }

            json << "  \"materials\": [\n";
            for (size_t i = 0; i < materials.size(); ++i)
            {
                json << "    {\n";
                json << "      \"name\": \"" << JsonEscape(materials[i].name) << "\",\n";
                json << "      \"pbrMetallicRoughness\": {\n";
                if (textureSections.materialTextureIndices[i] >= 0)
                {
                    json << "        \"baseColorTexture\": { \"index\": " << textureSections.materialTextureIndices[i] << " },\n";
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

            if (textureSections.images.empty())
            {
                return;
            }

            json << "  \"textures\": [\n";
            for (size_t i = 0; i < textureSections.images.size(); ++i)
            {
                json << "    { \"sampler\": 0, \"source\": " << i << " }";
                if (i + 1 < textureSections.images.size()) json << ",";
                json << "\n";
            }
            json << "  ],\n";

            json << "  \"images\": [\n";
            for (size_t i = 0; i < textureSections.images.size(); ++i)
            {
                const GlbImageInfo& image = textureSections.images[i];
                if (image.embedded)
                {
                    json << "    { \"bufferView\": " << (imageBufferViewBase + image.embeddedOrdinal)
                         << ", \"mimeType\": \"" << image.mimeType << "\" }";
                }
                else
                {
                    json << "    { \"uri\": \"" << JsonEscape(image.uri) << "\" }";
                }
                if (i + 1 < textureSections.images.size()) json << ",";
                json << "\n";
            }
            json << "  ],\n";

            json << "  \"samplers\": [\n";
            json << "    { \"magFilter\": 9729, \"minFilter\": 9987, \"wrapS\": 10497, \"wrapT\": 10497 }\n";
            json << "  ],\n";
        }

        void WriteEmbeddedImageBufferViews(std::stringstream& json, const GlbTextureSections& textureSections)
        {
            bool firstImageView = true;
            for (const GlbImageInfo& image : textureSections.images)
            {
                if (!image.embedded)
                {
                    continue;
                }

                if (!firstImageView)
                {
                    json << ",\n";
                }
                firstImageView = false;
                json << "    {\n";
                json << "      \"buffer\": 0,\n";
                json << "      \"byteOffset\": " << image.byteOffset << ",\n";
                json << "      \"byteLength\": " << image.byteLength << "\n";
                json << "    }";
            }
        }

        struct GlbAnimationCurveInfo
        {
            std::string nodeName;
            std::string path;
            size_t targetNodeIndex = 0;
            size_t timeBufferView = 0;
            size_t valueBufferView = 0;
            size_t timeByteOffset = 0;
            size_t timeByteLength = 0;
            size_t valueByteOffset = 0;
            size_t valueByteLength = 0;
            size_t timeAccessor = 0;
            size_t valueAccessor = 0;
            size_t sampleCount = 0;
            float minTime = 0.0f;
            float maxTime = 0.0f;
            std::string valueType;
        };

        struct GlbAnimationClipInfo
        {
            std::string name;
            std::vector<GlbAnimationCurveInfo> curves;
        };

        struct GlbAnimationSections
        {
            std::vector<GlbAnimationClipInfo> clips;
            size_t bufferViewCount = 0;
        };

        std::array<float, 3> ConvertAnimationTranslationForGltf(const std::array<float, 3>& value)
        {
            return { value[0], value[1], -value[2] };
        }

        std::array<float, 4> ConvertAnimationRotationForGltf(std::array<float, 4> value)
        {
            // Dagor mesh positions are converted with a Z reflection. For local
            // quaternions, S * R * S maps (x, y, z, w) to (-x, -y, z, w).
            value = { -value[0], -value[1], value[2], value[3] };
            const float lengthSq =
                (value[0] * value[0]) +
                (value[1] * value[1]) +
                (value[2] * value[2]) +
                (value[3] * value[3]);
            if (!std::isfinite(lengthSq) || lengthSq <= 1e-12f)
            {
                return { 0.0f, 0.0f, 0.0f, 1.0f };
            }

            const float invLength = 1.0f / std::sqrt(lengthSq);
            return {
                value[0] * invLength,
                value[1] * invLength,
                value[2] * invLength,
                value[3] * invLength
            };
        }

        bool HasValidAnimationTimes(const std::vector<float>& times)
        {
            if (times.empty())
            {
                return false;
            }

            float previous = -std::numeric_limits<float>::infinity();
            for (float time : times)
            {
                if (!std::isfinite(time) || time < previous)
                {
                    return false;
                }
                previous = time;
            }
            return true;
        }

        struct GltfNodeTrs
        {
            std::array<float, 3> translation = { 0.0f, 0.0f, 0.0f };
            std::array<float, 4> rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
            std::array<float, 3> scale = { 1.0f, 1.0f, 1.0f };
        };

        GltfNodeTrs DecomposeGltfMatrix(const std::array<float, 16>& matrix)
        {
            GltfNodeTrs trs;
            trs.translation = { matrix[12], matrix[13], matrix[14] };

            std::array<float, 3> xAxis = { matrix[0], matrix[1], matrix[2] };
            std::array<float, 3> yAxis = { matrix[4], matrix[5], matrix[6] };
            std::array<float, 3> zAxis = { matrix[8], matrix[9], matrix[10] };
            const auto length = [](const std::array<float, 3>& value)
            {
                return std::sqrt((value[0] * value[0]) + (value[1] * value[1]) + (value[2] * value[2]));
            };
            trs.scale = { length(xAxis), length(yAxis), length(zAxis) };
            const auto normalize = [](std::array<float, 3>& value, float scale)
            {
                if (std::isfinite(scale) && scale > 1e-6f)
                {
                    value[0] /= scale;
                    value[1] /= scale;
                    value[2] /= scale;
                }
            };
            normalize(xAxis, trs.scale[0]);
            normalize(yAxis, trs.scale[1]);
            normalize(zAxis, trs.scale[2]);
            for (float& scale : trs.scale)
            {
                if (!std::isfinite(scale) || scale <= 1e-6f)
                {
                    scale = 1.0f;
                }
            }

            const float m00 = xAxis[0];
            const float m01 = yAxis[0];
            const float m02 = zAxis[0];
            const float m10 = xAxis[1];
            const float m11 = yAxis[1];
            const float m12 = zAxis[1];
            const float m20 = xAxis[2];
            const float m21 = yAxis[2];
            const float m22 = zAxis[2];

            std::array<float, 4> q = { 0.0f, 0.0f, 0.0f, 1.0f };
            const float trace = m00 + m11 + m22;
            if (trace > 0.0f)
            {
                const float s = std::sqrt(trace + 1.0f) * 2.0f;
                q[3] = 0.25f * s;
                q[0] = (m21 - m12) / s;
                q[1] = (m02 - m20) / s;
                q[2] = (m10 - m01) / s;
            }
            else if (m00 > m11 && m00 > m22)
            {
                const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
                q[3] = (m21 - m12) / s;
                q[0] = 0.25f * s;
                q[1] = (m01 + m10) / s;
                q[2] = (m02 + m20) / s;
            }
            else if (m11 > m22)
            {
                const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
                q[3] = (m02 - m20) / s;
                q[0] = (m01 + m10) / s;
                q[1] = 0.25f * s;
                q[2] = (m12 + m21) / s;
            }
            else
            {
                const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
                q[3] = (m10 - m01) / s;
                q[0] = (m02 + m20) / s;
                q[1] = (m12 + m21) / s;
                q[2] = 0.25f * s;
            }
            const float qLengthSq = (q[0] * q[0]) + (q[1] * q[1]) + (q[2] * q[2]) + (q[3] * q[3]);
            if (std::isfinite(qLengthSq) && qLengthSq > 1e-12f)
            {
                const float invLength = 1.0f / std::sqrt(qLengthSq);
                trs.rotation = { q[0] * invLength, q[1] * invLength, q[2] * invLength, q[3] * invLength };
            }
            return trs;
        }

        template <typename TValue, typename TConvert>
        void AppendAnimationCurve(
            std::vector<uint8_t>& binBuffer,
            GlbAnimationClipInfo& clipInfo,
            const std::string& nodeName,
            size_t targetNodeIndex,
            const std::string& path,
            const std::string& valueType,
            size_t& nextBufferView,
            const std::vector<float>& times,
            const std::vector<TValue>& values,
            TConvert convert)
        {
            if (!HasValidAnimationTimes(times) || values.size() != times.size())
            {
                return;
            }

            GlbAnimationCurveInfo curve;
            curve.nodeName = nodeName;
            curve.path = path;
            curve.targetNodeIndex = targetNodeIndex;
            curve.sampleCount = times.size();
            curve.minTime = times.front();
            curve.maxTime = times.back();
            curve.valueType = valueType;

            while (binBuffer.size() % 4 != 0)
            {
                binBuffer.push_back(0);
            }
            curve.timeByteOffset = binBuffer.size();
            curve.timeByteLength = times.size() * sizeof(float);
            const uint8_t* timeBytes = reinterpret_cast<const uint8_t*>(times.data());
            binBuffer.insert(binBuffer.end(), timeBytes, timeBytes + curve.timeByteLength);
            while (binBuffer.size() % 4 != 0)
            {
                binBuffer.push_back(0);
            }

            std::vector<TValue> convertedValues;
            convertedValues.reserve(values.size());
            for (const TValue& value : values)
            {
                convertedValues.push_back(convert(value));
            }

            curve.valueByteOffset = binBuffer.size();
            curve.valueByteLength = convertedValues.size() * sizeof(TValue);
            const uint8_t* valueBytes = reinterpret_cast<const uint8_t*>(convertedValues.data());
            binBuffer.insert(binBuffer.end(), valueBytes, valueBytes + curve.valueByteLength);
            while (binBuffer.size() % 4 != 0)
            {
                binBuffer.push_back(0);
            }

            curve.timeBufferView = nextBufferView++;
            curve.valueBufferView = nextBufferView++;
            clipInfo.curves.push_back(std::move(curve));
        }

        GlbAnimationSections PrepareAnimationSections(
            const std::vector<AnimationClip>& animations,
            const std::unordered_map<std::string, size_t>& skeletonNodeLookup,
            const std::vector<bool>& emittedSkeletonNodes,
            const std::vector<size_t>& skeletonNodeIndices,
            bool emitSkeletonNodes,
            size_t bufferViewBase,
            std::vector<uint8_t>& binBuffer,
            std::ostream& debugLog)
        {
            GlbAnimationSections sections;
            if (animations.empty())
            {
                return sections;
            }
            if (!emitSkeletonNodes)
            {
                debugLog << "  Animation export skipped: skeleton nodes are not emitted." << std::endl;
                return sections;
            }

            size_t nextBufferView = bufferViewBase;
            for (const AnimationClip& clip : animations)
            {
                GlbAnimationClipInfo clipInfo;
                clipInfo.name = clip.name.empty() ? "animation" : clip.name;

                for (const AnimationNodeTrack& track : clip.nodeTracks)
                {
                    const auto nodeIt = skeletonNodeLookup.find(track.nodeName);
                    if (nodeIt == skeletonNodeLookup.end())
                    {
                        debugLog << "  Animation clip " << clipInfo.name
                                 << " skipped node '" << track.nodeName
                                 << "': no matching skeleton node." << std::endl;
                        continue;
                    }

                    const size_t skeletonNode = nodeIt->second;
                    if (skeletonNode >= emittedSkeletonNodes.size() ||
                        !emittedSkeletonNodes[skeletonNode] ||
                        skeletonNode >= skeletonNodeIndices.size() ||
                        skeletonNodeIndices[skeletonNode] == std::numeric_limits<size_t>::max())
                    {
                        debugLog << "  Animation clip " << clipInfo.name
                                 << " skipped node '" << track.nodeName
                                 << "': skeleton node was not emitted." << std::endl;
                        continue;
                    }

                    const size_t targetNodeIndex = skeletonNodeIndices[skeletonNode];
                    AppendAnimationCurve(
                        binBuffer,
                        clipInfo,
                        track.nodeName,
                        targetNodeIndex,
                        "translation",
                        "VEC3",
                        nextBufferView,
                        track.times,
                        track.translations,
                        ConvertAnimationTranslationForGltf);
                    AppendAnimationCurve(
                        binBuffer,
                        clipInfo,
                        track.nodeName,
                        targetNodeIndex,
                        "rotation",
                        "VEC4",
                        nextBufferView,
                        track.times,
                        track.rotations,
                        ConvertAnimationRotationForGltf);
                    AppendAnimationCurve(
                        binBuffer,
                        clipInfo,
                        track.nodeName,
                        targetNodeIndex,
                        "scale",
                        "VEC3",
                        nextBufferView,
                        track.times,
                        track.scales,
                        [](const std::array<float, 3>& value) { return value; });
                }

                for (const std::string& diagnostic : clip.diagnostics)
                {
                    debugLog << "  Animation diagnostic for " << clipInfo.name << ": " << diagnostic << std::endl;
                }

                if (!clipInfo.curves.empty())
                {
                    sections.bufferViewCount += clipInfo.curves.size() * 2;
                    sections.clips.push_back(std::move(clipInfo));
                }
                else
                {
                    debugLog << "  Animation clip " << clipInfo.name
                             << " had no exportable curves." << std::endl;
                }
            }

            return sections;
        }

        void AllocateAnimationAccessors(GlbAnimationSections& sections, size_t& currentAccessor)
        {
            for (GlbAnimationClipInfo& clip : sections.clips)
            {
                for (GlbAnimationCurveInfo& curve : clip.curves)
                {
                    curve.timeAccessor = currentAccessor++;
                    curve.valueAccessor = currentAccessor++;
                }
            }
        }

        void WriteAnimationSections(std::stringstream& json, const GlbAnimationSections& sections)
        {
            if (sections.clips.empty())
            {
                return;
            }

            json << "  \"animations\": [\n";
            for (size_t clipIndex = 0; clipIndex < sections.clips.size(); ++clipIndex)
            {
                const GlbAnimationClipInfo& clip = sections.clips[clipIndex];
                json << "    {\n";
                json << "      \"name\": \"" << JsonEscape(clip.name) << "\",\n";
                json << "      \"samplers\": [\n";
                for (size_t curveIndex = 0; curveIndex < clip.curves.size(); ++curveIndex)
                {
                    const GlbAnimationCurveInfo& curve = clip.curves[curveIndex];
                    json << "        { \"input\": " << curve.timeAccessor
                         << ", \"output\": " << curve.valueAccessor
                         << ", \"interpolation\": \"LINEAR\" }";
                    if (curveIndex + 1 < clip.curves.size())
                    {
                        json << ",";
                    }
                    json << "\n";
                }
                json << "      ],\n";
                json << "      \"channels\": [\n";
                for (size_t curveIndex = 0; curveIndex < clip.curves.size(); ++curveIndex)
                {
                    const GlbAnimationCurveInfo& curve = clip.curves[curveIndex];
                    json << "        { \"sampler\": " << curveIndex
                         << ", \"target\": { \"node\": " << curve.targetNodeIndex
                         << ", \"path\": \"" << curve.path << "\" } }";
                    if (curveIndex + 1 < clip.curves.size())
                    {
                        json << ",";
                    }
                    json << "\n";
                }
                json << "      ]\n";
                json << "    }";
                if (clipIndex + 1 < sections.clips.size())
                {
                    json << ",";
                }
                json << "\n";
            }
            json << "  ],\n";
        }

        void WriteAnimationBufferViews(std::stringstream& json, const GlbAnimationSections& sections)
        {
            bool first = true;
            for (const GlbAnimationClipInfo& clip : sections.clips)
            {
                for (const GlbAnimationCurveInfo& curve : clip.curves)
                {
                    if (!first)
                    {
                        json << ",\n";
                    }
                    first = false;
                    json << "    { \"buffer\": 0, \"byteOffset\": " << curve.timeByteOffset
                         << ", \"byteLength\": " << curve.timeByteLength << " },\n";
                    json << "    { \"buffer\": 0, \"byteOffset\": " << curve.valueByteOffset
                         << ", \"byteLength\": " << curve.valueByteLength << " }";
                }
            }
        }

        template <typename TSeparator>
        void WriteAnimationAccessors(std::stringstream& json, const GlbAnimationSections& sections, TSeparator writeAccessorSeparator)
        {
            for (const GlbAnimationClipInfo& clip : sections.clips)
            {
                for (const GlbAnimationCurveInfo& curve : clip.curves)
                {
                    writeAccessorSeparator();
                    json << "    { \"bufferView\": " << curve.timeBufferView
                         << ", \"byteOffset\": 0, \"componentType\": 5126, \"count\": " << curve.sampleCount
                         << ", \"type\": \"SCALAR\", \"min\": [" << JsonFloat(curve.minTime)
                         << "], \"max\": [" << JsonFloat(curve.maxTime) << "] }";

                    writeAccessorSeparator();
                    json << "    { \"bufferView\": " << curve.valueBufferView
                         << ", \"byteOffset\": 0, \"componentType\": 5126, \"count\": " << curve.sampleCount
                         << ", \"type\": \"" << curve.valueType << "\" }";
                }
            }
        }
    }

    bool ModelExporter::ExportToGlb(
        const std::filesystem::path& path,
        const std::vector<DagorFormats::RendInstMesh>& lods_in,
        const ExportManifest& manifest,
        const std::vector<MaterialInfo>& materials,
        const GlbExportOptions& options)
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

            const size_t removedDegenerateTriangles = FilterDegenerateTriangles(validLod);
            if (removedDegenerateTriangles > 0)
            {
                debugLog << "  LOD " << validLod.name
                         << " removed " << removedDegenerateTriangles
                         << " degenerate triangles before GLB export." << std::endl;
            }
            if (validLod.indices.empty() || validLod.primitives.empty())
            {
                debugLog << "  LOD " << lod_in.name << " skipped: all triangles were degenerate." << std::endl;
                continue;
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

        const MaterialSelection materialSelection = SelectUsedMaterials(lods, materials);
        const GlbTextureSections textureSections = PrepareMaterialTextures(materialSelection.materials, options, binBuffer, debugLog);
        std::vector<bool> meshWritesTexcoords(lods.size(), false);
        for (size_t i = 0; i < lods.size(); ++i)
        {
            for (const DagorFormats::RendInstPrimitive& primitive : lods[i].primitives)
            {
                if (MaterialSlotHasTexture(materialSelection, primitive.materialIndex))
                {
                    meshWritesTexcoords[i] = true;
                    break;
                }
            }
        }

        struct StaticAccessorInfo
        {
            size_t position = 0;
            size_t normal = 0;
            size_t uv = 0;
            std::vector<size_t> indices;
        };

        std::vector<StaticAccessorInfo> accessorInfos(lods.size());
        size_t currentAccessor = 0;
        for (size_t i = 0; i < lods.size(); ++i)
        {
            accessorInfos[i].position = currentAccessor++;
            accessorInfos[i].normal = currentAccessor++;
            if (meshWritesTexcoords[i])
            {
                accessorInfos[i].uv = currentAccessor++;
            }
        }
        for (size_t i = 0; i < lods.size(); ++i)
        {
            accessorInfos[i].indices.reserve(lods[i].primitives.size());
            for (size_t p = 0; p < lods[i].primitives.size(); ++p)
            {
                accessorInfos[i].indices.push_back(currentAccessor++);
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
                json << "            \"POSITION\": " << accessorInfos[i].position << ",\n";
                json << "            \"NORMAL\": " << accessorInfos[i].normal;
                if (meshWritesTexcoords[i] && MaterialSlotHasTexture(materialSelection, prim.materialIndex))
                {
                    json << ",\n";
                    json << "            \"TEXCOORD_0\": " << accessorInfos[i].uv << "\n";
                }
                else
                {
                    json << "\n";
                }
                json << "          },\n";
                json << "          \"indices\": " << accessorInfos[i].indices[p] << ",\n";
                
                if (!materialSelection.materials.empty())
                {
                    const auto matIt = materialSelection.remap.find(prim.materialIndex);
                    if (matIt != materialSelection.remap.end())
                    {
                        json << "          \"material\": " << matIt->second << ",\n";
                    }
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

        const size_t imageBufferViewBase = lods.size() * 2;
        WriteMaterialTextureSections(json, materialSelection.materials, textureSections, imageBufferViewBase);

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
            if (i + 1 < lods.size() || textureSections.embeddedImageCount > 0) json << ",";
            json << "\n";
        }
        if (textureSections.embeddedImageCount > 0)
        {
            WriteEmbeddedImageBufferViews(json, textureSections);
            json << "\n";
        }
        json << "  ],\n";

        // Accessors definition
        json << "  \"accessors\": [\n";
        bool firstAccessor = true;
        const auto writeAccessorSeparator = [&]()
        {
            if (!firstAccessor)
            {
                json << ",\n";
            }
            firstAccessor = false;
        };

        for (size_t i = 0; i < lods.size(); ++i)
        {
            const auto& mesh = lods[i];
            const auto& info = binInfos[i];

            writeAccessorSeparator();
            json << "    { \"bufferView\": " << i
                 << ", \"byteOffset\": 0, \"componentType\": 5126, \"count\": " << mesh.vertices.size()
                 << ", \"type\": \"VEC3\", \"max\": [" << JsonFloat(info.maxX) << ", " << JsonFloat(info.maxY) << ", " << JsonFloat(info.maxZ)
                 << "], \"min\": [" << JsonFloat(info.minX) << ", " << JsonFloat(info.minY) << ", " << JsonFloat(info.minZ) << "] }";

            writeAccessorSeparator();
            json << "    { \"bufferView\": " << i
                 << ", \"byteOffset\": 12, \"componentType\": 5126, \"count\": " << mesh.vertices.size()
                 << ", \"type\": \"VEC3\" }";

            if (meshWritesTexcoords[i])
            {
                writeAccessorSeparator();
                json << "    { \"bufferView\": " << i
                     << ", \"byteOffset\": 24, \"componentType\": 5126, \"count\": " << mesh.vertices.size()
                     << ", \"type\": \"VEC2\" }";
            }
        }

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
                writeAccessorSeparator();
                json << "    { \"bufferView\": " << (lods.size() + i)
                     << ", \"byteOffset\": " << (prim.firstIndex * sizeof(uint32_t))
                     << ", \"componentType\": 5125, \"count\": " << prim.indexCount
                     << ", \"type\": \"SCALAR\" }";
            }
        }
        json << "\n";
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
        const std::vector<MaterialInfo>& materials,
        bool includeSkeletonRig,
        const GlbExportOptions& options,
        const std::vector<AnimationClip>& animations)
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

            const size_t removedDegenerateTriangles = FilterDegenerateTriangles(validLod);
            if (removedDegenerateTriangles > 0)
            {
                debugLog << "  DynModel mesh " << validLod.name
                         << " removed " << removedDegenerateTriangles
                         << " degenerate triangles before GLB export." << std::endl;
            }
            if (validLod.indices.empty() || validLod.primitives.empty())
            {
                debugLog << "  LOD " << lod_in.name << " skipped: all triangles were degenerate." << std::endl;
                continue;
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

        const auto makeGltfMatrix = [&](const std::array<float, 16>& matrix)
        {
            std::array<float, 16> gltfMatrix = transposeMatrix(matrix);
            ConvertMatrixHandedness(gltfMatrix);
            return gltfMatrix;
        };

        const bool hasPlacementSkeleton = !skeleton.empty();
        const bool emitSkeletonNodes = includeSkeletonRig && hasPlacementSkeleton;

        std::unordered_map<std::string, size_t> skeletonNodeLookup;
        skeletonNodeLookup.reserve(skeleton.size());
        if (hasPlacementSkeleton)
        {
            for (size_t i = 0; i < skeleton.size(); ++i)
            {
                skeletonNodeLookup.emplace(skeleton[i].name, i);
            }
        }

        constexpr uint16_t kNoJoint = std::numeric_limits<uint16_t>::max();

        const auto resolveJoint = [&](const DagorFormats::DynModelMesh& mesh) -> uint16_t
        {
            if (!hasPlacementSkeleton || mesh.parentNodeName.empty())
            {
                return kNoJoint;
            }

            const auto it = skeletonNodeLookup.find(mesh.parentNodeName);
            if (it == skeletonNodeLookup.end())
            {
                debugLog << "  DynModel mesh " << mesh.name
                         << " has no matching skeleton node for parent '"
                         << mesh.parentNodeName << "'." << std::endl;
                return kNoJoint;
            }
            if (it->second > std::numeric_limits<uint16_t>::max())
            {
                debugLog << "  DynModel mesh " << mesh.name
                         << " resolved to skeleton node " << it->second
                         << ", which exceeds glTF unsigned-short joint index range." << std::endl;
                return kNoJoint;
            }

            return static_cast<uint16_t>(it->second);
        };

        const auto hasOnlyRootWeights = [](const DagorFormats::DynModelMesh& mesh)
        {
            return std::all_of(
                mesh.vertices.begin(),
                mesh.vertices.end(),
                [](const DagorFormats::SkinnedVertex& vertex)
                {
                    return vertex.jointIndices[0] == 0 &&
                        vertex.jointIndices[1] == 0 &&
                        vertex.jointIndices[2] == 0 &&
                        vertex.jointIndices[3] == 0 &&
                        std::fabs(vertex.jointWeights[0] - 1.0f) <= 1e-5f &&
                        std::fabs(vertex.jointWeights[1]) <= 1e-5f &&
                        std::fabs(vertex.jointWeights[2]) <= 1e-5f &&
                        std::fabs(vertex.jointWeights[3]) <= 1e-5f;
                });
        };

        struct DynExportMesh
        {
            DagorFormats::DynModelMesh mesh;
            bool skinned = false;
            bool hasParentJoint = false;
            uint16_t parentJoint = 0;
            bool hasNodeMatrix = false;
            std::array<float, 16> nodeMatrix = {};
        };

        if (emitSkeletonNodes)
        {
            bool hasSkinnedMesh = false;
            for (const auto& mesh : lods)
            {
                if (mesh.useSkinning && !hasOnlyRootWeights(mesh))
                {
                    hasSkinnedMesh = true;
                    break;
                }
            }

            if (!hasSkinnedMesh && !animations.empty())
            {
                DagorFormats::DynModelMesh dummyMesh;
                dummyMesh.name = "skeleton_dummy";
                dummyMesh.parentNodeName = "";
                dummyMesh.useSkinning = true;

                DagorFormats::SkinnedVertex v1{};
                v1.x = v1.y = v1.z = 0.0f;
                v1.nx = 0.0f; v1.ny = 1.0f; v1.nz = 0.0f;
                v1.u = v1.v = 0.0f;
                v1.jointIndices[0] = 0; v1.jointIndices[1] = 0; v1.jointIndices[2] = 0; v1.jointIndices[3] = 0;
                v1.jointWeights[0] = 1.0f; v1.jointWeights[1] = 0.0f; v1.jointWeights[2] = 0.0f; v1.jointWeights[3] = 0.0f;

                DagorFormats::SkinnedVertex v2 = v1;
                v2.x = 0.001f;
                DagorFormats::SkinnedVertex v3 = v1;
                v3.y = 0.001f;

                dummyMesh.vertices = { v1, v2, v3 };
                dummyMesh.indices = { 0, 1, 2 };

                DagorFormats::RendInstPrimitive prim;
                prim.materialIndex = 0;
                prim.firstIndex = 0;
                prim.indexCount = 3;
                dummyMesh.primitives = { prim };

                lods.push_back(std::move(dummyMesh));
            }
        }

        std::vector<DynExportMesh> exportMeshes;
        exportMeshes.reserve(lods.size());
        for (const auto& mesh : lods)
        {
            DynExportMesh exportMesh;
            exportMesh.mesh = mesh;
            const uint16_t jointIndex = resolveJoint(mesh);
            const bool hasPlaceholderSkinWeights = mesh.useSkinning && hasOnlyRootWeights(mesh);

            if (emitSkeletonNodes && mesh.useSkinning && (!hasPlaceholderSkinWeights || mesh.name == "skeleton_dummy"))
            {
                exportMesh.skinned = true;
            }
            else if (emitSkeletonNodes && jointIndex != kNoJoint)
            {
                exportMesh.hasParentJoint = true;
                exportMesh.parentJoint = jointIndex;
            }
            else if (!emitSkeletonNodes && jointIndex != kNoJoint && (!mesh.useSkinning || hasPlaceholderSkinWeights))
            {
                exportMesh.hasNodeMatrix = true;
                exportMesh.nodeMatrix = makeGltfMatrix(skeleton[jointIndex].worldTransform);
            }

            exportMeshes.push_back(std::move(exportMesh));
        }

        if (exportMeshes.empty())
        {
            debugLog << "[ExportToGlb DynModel] Failed: no exportable meshes after skeleton layout preparation." << std::endl;
            return false;
        }

        const bool useSkinning = std::any_of(
            exportMeshes.begin(),
            exportMeshes.end(),
            [](const DynExportMesh& mesh) { return mesh.skinned; });

        std::vector<bool> emittedSkeletonNodes(skeleton.size(), false);
        if (emitSkeletonNodes)
        {
            if (useSkinning)
            {
                std::fill(emittedSkeletonNodes.begin(), emittedSkeletonNodes.end(), true);
            }
            else
            {
                for (const DynExportMesh& exportMesh : exportMeshes)
                {
                    if (!exportMesh.hasParentJoint)
                    {
                        continue;
                    }

                    uint16_t nodeIndex = exportMesh.parentJoint;
                    size_t guard = 0;
                    while (nodeIndex != DagorFormats::GeomNodeTreeNode::InvalidIndex &&
                           nodeIndex < skeleton.size() &&
                           guard++ < skeleton.size())
                    {
                        emittedSkeletonNodes[nodeIndex] = true;
                        nodeIndex = skeleton[nodeIndex].parentIndex;
                    }
                }
            }
        }

        if (emitSkeletonNodes && !animations.empty())
        {
            size_t animationTargetNodes = 0;
            size_t missingAnimationNodes = 0;
            for (const AnimationClip& clip : animations)
            {
                for (const AnimationNodeTrack& track : clip.nodeTracks)
                {
                    const auto it = skeletonNodeLookup.find(track.nodeName);
                    if (it == skeletonNodeLookup.end())
                    {
                        ++missingAnimationNodes;
                        continue;
                    }

                    size_t nodeIndex = it->second;
                    size_t guard = 0;
                    while (nodeIndex != static_cast<size_t>(DagorFormats::GeomNodeTreeNode::InvalidIndex) &&
                           nodeIndex < skeleton.size() &&
                           guard++ < skeleton.size())
                    {
                        if (!emittedSkeletonNodes[nodeIndex])
                        {
                            ++animationTargetNodes;
                        }
                        emittedSkeletonNodes[nodeIndex] = true;
                        nodeIndex = skeleton[nodeIndex].parentIndex;
                    }
                }
            }

            debugLog << "  Animation skeleton emission summary: requestedClips=" << animations.size()
                     << ", addedSkeletonNodes=" << animationTargetNodes
                     << ", missingNodeTracks=" << missingAnimationNodes << std::endl;
        }
        else if (!animations.empty())
        {
            debugLog << "  Animation export requested but skeleton nodes are not being emitted." << std::endl;
        }

        std::vector<size_t> skeletonNodeIndices(
            skeleton.size(),
            std::numeric_limits<size_t>::max());
        size_t emittedSkeletonNodeCount = 0;
        for (size_t i = 0; i < skeleton.size(); ++i)
        {
            if (emittedSkeletonNodes[i])
            {
                skeletonNodeIndices[i] = emittedSkeletonNodeCount++;
            }
        }

        const auto meshNodeIndex = [&](size_t meshIndex)
        {
            return (emitSkeletonNodes ? emittedSkeletonNodeCount : 0) + meshIndex;
        };

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
        binInfos.reserve(exportMeshes.size());

        std::vector<uint8_t> binBuffer;
        
        for (size_t meshIndex = 0; meshIndex < exportMeshes.size(); ++meshIndex)
        {
            const DynExportMesh& exportMesh = exportMeshes[meshIndex];
            const auto& mesh = exportMesh.mesh;
            LodBinInfo info;
            info.vertByteOffset = binBuffer.size();
            info.vertByteLength = mesh.vertices.size() * sizeof(PackedSkinnedVertex);
            std::vector<uint32_t> flippedIndices = mesh.indices;
            for (size_t i = 0; i + 2 < flippedIndices.size(); i += 3)
            {
                std::swap(flippedIndices[i + 1], flippedIndices[i + 2]);
            }

            if (!mesh.vertices.empty())
            {
                std::vector<PackedSkinnedVertex> packedVertices;
                packedVertices.reserve(mesh.vertices.size());

                for (const auto& v : mesh.vertices)
                {
                    const auto uv = ConvertUvForGltf(v.u, v.v, v.uvTransform);
                    float px = v.x;
                    float py = v.y;
                    float pz = v.z;
                    float nx = v.nx;
                    float ny = v.ny;
                    float nz = v.nz;

                    PackedSkinnedVertex packed{};
                    packed.x = px;
                    packed.y = py;
                    packed.z = -pz;
                    packed.nx = nx;
                    packed.ny = ny;
                    packed.nz = -nz;
                    packed.u = uv[0];
                    packed.v = uv[1];
                    if (exportMesh.skinned)
                    {
                        for (int j = 0; j < 4; ++j)
                        {
                            uint16_t jointIndex = static_cast<uint16_t>(v.jointIndices[j]);
                            if (jointIndex >= skeleton.size())
                            {
                                jointIndex = 0;
                            }
                            packed.jointIndices[j] = jointIndex;
                            packed.jointWeights[j] = v.jointWeights[j];
                        }
                        NormalizeJointWeights(packed);
                    }
                    packedVertices.push_back(packed);

                    info.minX = std::min(info.minX, packed.x);
                    info.minY = std::min(info.minY, packed.y);
                    info.minZ = std::min(info.minZ, packed.z);
                    info.maxX = std::max(info.maxX, packed.x);
                    info.maxY = std::max(info.maxY, packed.y);
                    info.maxZ = std::max(info.maxZ, packed.z);
                }

                RepairNormals(packedVertices, flippedIndices);
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

            if (!flippedIndices.empty())
            {
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

        const MaterialSelection materialSelection = SelectUsedMaterials(lods, materials);
        const GlbTextureSections textureSections = PrepareMaterialTextures(materialSelection.materials, options, binBuffer, debugLog);
        const size_t imageBufferViewBase = (exportMeshes.size() * 2) + (useSkinning ? 1 : 0);
        const size_t animationBufferViewBase = imageBufferViewBase + textureSections.embeddedImageCount;
        GlbAnimationSections animationSections = PrepareAnimationSections(
            animations,
            skeletonNodeLookup,
            emittedSkeletonNodes,
            skeletonNodeIndices,
            emitSkeletonNodes,
            animationBufferViewBase,
            binBuffer,
            debugLog);
        std::unordered_map<size_t, GltfNodeTrs> animatedNodeInitialTransforms;
        if (!animationSections.clips.empty())
        {
            for (const AnimationClip& clip : animations)
            {
                for (const AnimationNodeTrack& track : clip.nodeTracks)
                {
                    if (!HasValidAnimationTimes(track.times))
                    {
                        continue;
                    }
                    const auto nodeIt = skeletonNodeLookup.find(track.nodeName);
                    if (nodeIt == skeletonNodeLookup.end() || nodeIt->second >= skeleton.size())
                    {
                        continue;
                    }
                    const size_t skeletonNode = nodeIt->second;
                    if (skeletonNode >= emittedSkeletonNodes.size() || !emittedSkeletonNodes[skeletonNode])
                    {
                        continue;
                    }

                    auto [trsIt, inserted] = animatedNodeInitialTransforms.emplace(
                        skeletonNode,
                        DecomposeGltfMatrix(makeGltfMatrix(skeleton[skeletonNode].localTransform)));
                    GltfNodeTrs& trs = trsIt->second;
                    if (track.translations.size() == track.times.size())
                    {
                        trs.translation = ConvertAnimationTranslationForGltf(track.translations.front());
                    }
                    if (track.rotations.size() == track.times.size())
                    {
                        trs.rotation = ConvertAnimationRotationForGltf(track.rotations.front());
                    }
                    if (track.scales.size() == track.times.size())
                    {
                        trs.scale = track.scales.front();
                    }
                    (void)inserted;
                }
            }
        }
        std::vector<bool> meshWritesTexcoords(exportMeshes.size(), false);
        for (size_t i = 0; i < exportMeshes.size(); ++i)
        {
            for (const DagorFormats::RendInstPrimitive& primitive : exportMeshes[i].mesh.primitives)
            {
                if (MaterialSlotHasTexture(materialSelection, primitive.materialIndex))
                {
                    meshWritesTexcoords[i] = true;
                    break;
                }
            }
        }

        struct AccessorInfo
        {
            size_t position = 0;
            size_t normal = 0;
            size_t uv = 0;
            size_t joints = 0;
            size_t weights = 0;
            std::vector<size_t> indices;
        };

        std::vector<AccessorInfo> accessorInfos(exportMeshes.size());
        size_t currentAccessor = 0;
        for (size_t i = 0; i < exportMeshes.size(); ++i)
        {
            accessorInfos[i].position = currentAccessor++;
            accessorInfos[i].normal = currentAccessor++;
            if (meshWritesTexcoords[i])
            {
                accessorInfos[i].uv = currentAccessor++;
            }
            if (exportMeshes[i].skinned)
            {
                accessorInfos[i].joints = currentAccessor++;
                accessorInfos[i].weights = currentAccessor++;
            }
        }
        for (size_t i = 0; i < exportMeshes.size(); ++i)
        {
            size_t primCount = exportMeshes[i].mesh.primitives.size();
            if (primCount == 0) primCount = 1;
            accessorInfos[i].indices.reserve(primCount);
            for (size_t p = 0; p < primCount; ++p)
            {
                accessorInfos[i].indices.push_back(currentAccessor++);
            }
        }
        size_t ibmAccessorIndex = 0;
        if (useSkinning)
        {
            ibmAccessorIndex = currentAccessor++;
        }
        AllocateAnimationAccessors(animationSections, currentAccessor);

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
        json << "      \"nodes\": [";
        bool firstSceneNode = true;
        if (emitSkeletonNodes)
        {
            for (size_t i = 0; i < skeleton.size(); ++i)
            {
                if (!emittedSkeletonNodes[i])
                {
                    continue;
                }
                const uint16_t parentIndex = skeleton[i].parentIndex;
                if (parentIndex == DagorFormats::GeomNodeTreeNode::InvalidIndex ||
                    parentIndex >= skeleton.size() ||
                    !emittedSkeletonNodes[parentIndex])
                {
                    if (!firstSceneNode) json << ", ";
                    json << skeletonNodeIndices[i];
                    firstSceneNode = false;
                }
            }
        }
        for (size_t i = 0; i < exportMeshes.size(); ++i)
        {
            if (exportMeshes[i].hasParentJoint)
            {
                continue;
            }
            if (!firstSceneNode) json << ", ";
            json << meshNodeIndex(i);
            firstSceneNode = false;
        }
        json << "]\n";
        json << "    }\n";
        json << "  ],\n";

        // Nodes definition
        json << "  \"nodes\": [\n";
        bool firstNode = true;
        const auto writeNodeSeparator = [&]()
        {
            if (!firstNode)
            {
                json << ",\n";
            }
            firstNode = false;
        };

        if (emitSkeletonNodes)
        {
            for (size_t i = 0; i < skeleton.size(); ++i)
            {
                if (!emittedSkeletonNodes[i])
                {
                    continue;
                }

                const auto& node = skeleton[i];
                std::array<float, 16> gltfMatrix = makeGltfMatrix(node.localTransform);
                writeNodeSeparator();
                json << "    {\n";
                json << "      \"name\": \"" << JsonEscape(node.name) << "\"";
                if (const auto animatedTrsIt = animatedNodeInitialTransforms.find(i);
                    animatedTrsIt != animatedNodeInitialTransforms.end())
                {
                    const GltfNodeTrs& trs = animatedTrsIt->second;
                    json << ",\n";
                    json << "      \"translation\": [" << JsonFloat(trs.translation[0]) << ", "
                         << JsonFloat(trs.translation[1]) << ", " << JsonFloat(trs.translation[2]) << "],\n";
                    json << "      \"rotation\": [" << JsonFloat(trs.rotation[0]) << ", "
                         << JsonFloat(trs.rotation[1]) << ", " << JsonFloat(trs.rotation[2]) << ", "
                         << JsonFloat(trs.rotation[3]) << "],\n";
                    json << "      \"scale\": [" << JsonFloat(trs.scale[0]) << ", "
                         << JsonFloat(trs.scale[1]) << ", " << JsonFloat(trs.scale[2]) << "]";
                }
                else if (!IsIdentityMatrix(gltfMatrix))
                {
                    json << ",\n";
                    json << "      \"matrix\": [";
                    for (int m = 0; m < 16; ++m)
                    {
                        json << JsonFloat(gltfMatrix[m]);
                        if (m < 15) json << ", ";
                    }
                    json << "]";
                }

                std::vector<size_t> childNodes;
                childNodes.reserve(node.childIndices.size());
                for (uint16_t childIndex : node.childIndices)
                {
                    if (childIndex < skeleton.size() && emittedSkeletonNodes[childIndex])
                    {
                        childNodes.push_back(skeletonNodeIndices[childIndex]);
                    }
                }
                for (size_t meshIndex = 0; meshIndex < exportMeshes.size(); ++meshIndex)
                {
                    if (exportMeshes[meshIndex].hasParentJoint && exportMeshes[meshIndex].parentJoint == i)
                    {
                        childNodes.push_back(meshNodeIndex(meshIndex));
                    }
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
                json << "    }";
            }
        }

        // Mesh nodes
        for (size_t i = 0; i < exportMeshes.size(); ++i)
        {
            writeNodeSeparator();
            json << "    {\n";
            json << "      \"name\": \"" << JsonEscape(exportMeshes[i].mesh.name) << "\",\n";
            json << "      \"mesh\": " << i;
            if (exportMeshes[i].skinned)
            {
                json << ",\n      \"skin\": 0";
            }
            if (exportMeshes[i].hasNodeMatrix && !IsIdentityMatrix(exportMeshes[i].nodeMatrix))
            {
                json << ",\n      \"matrix\": [";
                for (int m = 0; m < 16; ++m)
                {
                    json << JsonFloat(exportMeshes[i].nodeMatrix[m]);
                    if (m < 15) json << ", ";
                }
                json << "]";
            }
            json << "\n";
            json << "    }";
        }
        json << "\n";
        json << "  ],\n";

        if (useSkinning)
        {
            json << "  \"skins\": [\n";
            json << "    {\n";
            std::vector<size_t> skeletonRootNodes;
            for (size_t i = 0; i < skeleton.size(); ++i)
            {
                if (!emittedSkeletonNodes[i])
                {
                    continue;
                }
                const uint16_t parentIndex = skeleton[i].parentIndex;
                if (parentIndex == DagorFormats::GeomNodeTreeNode::InvalidIndex ||
                    parentIndex >= skeleton.size() ||
                    !emittedSkeletonNodes[parentIndex])
                {
                    skeletonRootNodes.push_back(skeletonNodeIndices[i]);
                }
            }
            if (skeletonRootNodes.size() == 1)
            {
                json << "      \"skeleton\": " << skeletonRootNodes.front() << ",\n";
            }
            json << "      \"inverseBindMatrices\": " << ibmAccessorIndex << ",\n"; // Accessor index
            json << "      \"joints\": [";
            bool firstJoint = true;
            for (size_t i = 0; i < skeleton.size(); ++i)
            {
                if (!emittedSkeletonNodes[i])
                {
                    continue;
                }
                if (!firstJoint) json << ", ";
                json << skeletonNodeIndices[i];
                firstJoint = false;
            }
            json << "]\n";
            json << "    }\n";
            json << "  ],\n";
        }

        // Meshes definition
        json << "  \"meshes\": [\n";
        for (size_t i = 0; i < exportMeshes.size(); ++i)
        {
            const DynExportMesh& exportMesh = exportMeshes[i];
            const auto& mesh = exportMesh.mesh;
            json << "    {\n";
            json << "      \"name\": \"" << JsonEscape(mesh.name) << "\",\n";
            json << "      \"primitives\": [\n";
            
            for (size_t p = 0; p < mesh.primitives.size(); ++p)
            {
                const auto& prim = mesh.primitives[p];
                json << "        {\n";
                json << "          \"attributes\": {\n";
                json << "            \"POSITION\": " << accessorInfos[i].position << ",\n";
                json << "            \"NORMAL\": " << accessorInfos[i].normal;
                if (meshWritesTexcoords[i] && MaterialSlotHasTexture(materialSelection, prim.materialIndex))
                {
                    json << ",\n";
                    json << "            \"TEXCOORD_0\": " << accessorInfos[i].uv;
                }
                if (exportMesh.skinned)
                {
                    json << ",\n            \"JOINTS_0\": " << accessorInfos[i].joints << ",\n";
                    json << "            \"WEIGHTS_0\": " << accessorInfos[i].weights << "\n";
                }
                else
                {
                    json << "\n";
                }
                json << "          },\n";
                json << "          \"indices\": " << accessorInfos[i].indices[p] << ",\n";
                
                if (!materialSelection.materials.empty())
                {
                    const auto matIt = materialSelection.remap.find(prim.materialIndex);
                    if (matIt != materialSelection.remap.end())
                    {
                        json << "          \"material\": " << matIt->second << ",\n";
                    }
                }
                
                json << "          \"mode\": 4\n";
                json << "        }";
                if (p + 1 < mesh.primitives.size()) json << ",";
                json << "\n";
            }
            json << "      ]\n";
            json << "    }";
            if (i + 1 < exportMeshes.size()) json << ",";
            json << "\n";
        }
        json << "  ],\n";

        WriteMaterialTextureSections(json, materialSelection.materials, textureSections, imageBufferViewBase);
        WriteAnimationSections(json, animationSections);

        // BufferViews
        json << "  \"bufferViews\": [\n";
        // Vertex bufferViews
        for (size_t i = 0; i < exportMeshes.size(); ++i)
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
        for (size_t i = 0; i < exportMeshes.size(); ++i)
        {
            json << "    {\n";
            json << "      \"buffer\": 0,\n";
            json << "      \"byteOffset\": " << binInfos[i].indexByteOffset << ",\n";
            json << "      \"byteLength\": " << binInfos[i].indexByteLength << ",\n";
            json << "      \"target\": 34963\n";
            json << "    }";
            if (i + 1 < exportMeshes.size() || useSkinning || textureSections.embeddedImageCount > 0 || animationSections.bufferViewCount > 0) json << ",\n";
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
            if (textureSections.embeddedImageCount > 0 || animationSections.bufferViewCount > 0)
            {
                json << ",\n";
            }
        }
        if (textureSections.embeddedImageCount > 0)
        {
            WriteEmbeddedImageBufferViews(json, textureSections);
            if (animationSections.bufferViewCount > 0)
            {
                json << ",\n";
            }
            else
            {
                json << "\n";
            }
        }
        if (animationSections.bufferViewCount > 0)
        {
            WriteAnimationBufferViews(json, animationSections);
            json << "\n";
        }
        json << "  ],\n";

        // Accessors
        json << "  \"accessors\": [\n";
        bool firstAccessor = true;
        const auto writeAccessorSeparator = [&]()
        {
            if (!firstAccessor)
            {
                json << ",\n";
            }
            firstAccessor = false;
        };

        for (size_t i = 0; i < exportMeshes.size(); ++i)
        {
            const DynExportMesh& exportMesh = exportMeshes[i];
            const auto& mesh = exportMesh.mesh;
            const auto& info = binInfos[i];

            writeAccessorSeparator();
            json << "    { \"bufferView\": " << i << ", \"byteOffset\": 0, \"componentType\": 5126, \"count\": " << mesh.vertices.size() << ", \"type\": \"VEC3\", \"max\": [" << JsonFloat(info.maxX) << "," << JsonFloat(info.maxY) << "," << JsonFloat(info.maxZ) << "], \"min\": [" << JsonFloat(info.minX) << "," << JsonFloat(info.minY) << "," << JsonFloat(info.minZ) << "] }";
            writeAccessorSeparator();
            json << "    { \"bufferView\": " << i << ", \"byteOffset\": 12, \"componentType\": 5126, \"count\": " << mesh.vertices.size() << ", \"type\": \"VEC3\" }";
            if (meshWritesTexcoords[i])
            {
                writeAccessorSeparator();
                json << "    { \"bufferView\": " << i << ", \"byteOffset\": 24, \"componentType\": 5126, \"count\": " << mesh.vertices.size() << ", \"type\": \"VEC2\" }";
            }
            if (exportMesh.skinned)
            {
                writeAccessorSeparator();
                json << "    { \"bufferView\": " << i << ", \"byteOffset\": 32, \"componentType\": 5123, \"count\": " << mesh.vertices.size() << ", \"type\": \"VEC4\" }";
                writeAccessorSeparator();
                json << "    { \"bufferView\": " << i << ", \"byteOffset\": 40, \"componentType\": 5126, \"count\": " << mesh.vertices.size() << ", \"type\": \"VEC4\" }";
            }
        }

        // Index accessors
        for (size_t i = 0; i < exportMeshes.size(); ++i)
        {
            const auto& mesh = exportMeshes[i].mesh;
            for (size_t p = 0; p < mesh.primitives.size(); ++p)
            {
                const auto& prim = mesh.primitives[p];
                writeAccessorSeparator();
                json << "    { \"bufferView\": " << (exportMeshes.size() + i) << ", \"byteOffset\": " << (prim.firstIndex * sizeof(uint32_t)) << ", \"componentType\": 5125, \"count\": " << prim.indexCount << ", \"type\": \"SCALAR\" }";
            }
        }

        if (useSkinning)
        {
            // IBM accessor
            writeAccessorSeparator();
            json << "    { \"bufferView\": " << (exportMeshes.size() * 2) << ", \"byteOffset\": 0, \"componentType\": 5126, \"count\": " << skeleton.size() << ", \"type\": \"MAT4\" }";
        }
        WriteAnimationAccessors(json, animationSections, writeAccessorSeparator);
        json << "\n";
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
