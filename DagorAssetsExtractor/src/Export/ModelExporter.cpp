#include "Export/ModelExporter.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <array>
#include <limits>
#include <iomanip>
#include <unordered_map>
#include <unordered_set>
#include <cctype>
#include <string_view>
#include <charconv>
#include <cmath>
#include <optional>

namespace DagorAssetsExtractor::Export
{
    namespace
    {
        constexpr std::string_view WarThunderPackedMaterialExtension =
            "DAGOR_material_war_thunder_packed_n";

        struct Tangent4
        {
            float x, y, z, w;
        };

        template <typename TVertex>
        std::vector<Tangent4> ComputeTangents(const std::vector<TVertex>& vertices, const std::vector<uint32_t>& indices)
        {
            if (vertices.empty()) return {};

            std::vector<std::array<float, 3>> tan1(vertices.size(), {0.0f, 0.0f, 0.0f});
            std::vector<std::array<float, 3>> tan2(vertices.size(), {0.0f, 0.0f, 0.0f});

            for (size_t i = 0; i + 2 < indices.size(); i += 3)
            {
                uint32_t i0 = indices[i];
                uint32_t i1 = indices[i + 1];
                uint32_t i2 = indices[i + 2];

                if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
                    continue;

                const auto& v0 = vertices[i0];
                const auto& v1 = vertices[i1];
                const auto& v2 = vertices[i2];

                float x1 = v1.x - v0.x;
                float x2 = v2.x - v0.x;
                float y1 = v1.y - v0.y;
                float y2 = v2.y - v0.y;
                float z1 = v1.z - v0.z;
                float z2 = v2.z - v0.z;

                float s1 = v1.u - v0.u;
                float s2 = v2.u - v0.u;
                float t1 = v1.v - v0.v;
                float t2 = v2.v - v0.v;

                float r = (s1 * t2 - s2 * t1);
                float dirX = 1.0f, dirY = 0.0f, dirZ = 0.0f;
                float sdirX = 0.0f, sdirY = 1.0f, sdirZ = 0.0f;

                if (std::fabs(r) > 1e-12f)
                {
                    float div = 1.0f / r;
                    dirX = (t2 * x1 - t1 * x2) * div;
                    dirY = (t2 * y1 - t1 * y2) * div;
                    dirZ = (t2 * z1 - t1 * z2) * div;

                    sdirX = (s1 * x2 - s2 * x1) * div;
                    sdirY = (s1 * y2 - s2 * y1) * div;
                    sdirZ = (s1 * z2 - s2 * z1) * div;
                }

                tan1[i0][0] += dirX; tan1[i0][1] += dirY; tan1[i0][2] += dirZ;
                tan1[i1][0] += dirX; tan1[i1][1] += dirY; tan1[i1][2] += dirZ;
                tan1[i2][0] += dirX; tan1[i2][1] += dirY; tan1[i2][2] += dirZ;

                tan2[i0][0] += sdirX; tan2[i0][1] += sdirY; tan2[i0][2] += sdirZ;
                tan2[i1][0] += sdirX; tan2[i1][1] += sdirY; tan2[i1][2] += sdirZ;
                tan2[i2][0] += sdirX; tan2[i2][1] += sdirY; tan2[i2][2] += sdirZ;
            }

            std::vector<Tangent4> tangents(vertices.size());
            for (size_t i = 0; i < vertices.size(); ++i)
            {
                const auto& v = vertices[i];
                float nx = v.nx;
                float ny = v.ny;
                float nz = v.nz;

                float lenN = std::sqrt(nx * nx + ny * ny + nz * nz);
                if (lenN > 1e-6f) { nx /= lenN; ny /= lenN; nz /= lenN; }

                float t[3] = { tan1[i][0], tan1[i][1], tan1[i][2] };

                float dot = nx * t[0] + ny * t[1] + nz * t[2];
                float orthoX = t[0] - dot * nx;
                float orthoY = t[1] - dot * ny;
                float orthoZ = t[2] - dot * nz;

                float lenT = std::sqrt(orthoX * orthoX + orthoY * orthoY + orthoZ * orthoZ);
                if (lenT > 1e-6f)
                {
                    orthoX /= lenT;
                    orthoY /= lenT;
                    orthoZ /= lenT;
                }
                else
                {
                    if (std::fabs(nx) > 0.9f)
                    {
                        orthoX = 0.0f; orthoY = 1.0f; orthoZ = 0.0f;
                    }
                    else
                    {
                        orthoX = 1.0f; orthoY = 0.0f; orthoZ = 0.0f;
                    }
                    float dotF = nx * orthoX + ny * orthoY + nz * orthoZ;
                    orthoX -= dotF * nx;
                    orthoY -= dotF * ny;
                    orthoZ -= dotF * nz;
                    float lenF = std::sqrt(orthoX * orthoX + orthoY * orthoY + orthoZ * orthoZ);
                    if (lenF > 1e-6f) { orthoX /= lenF; orthoY /= lenF; orthoZ /= lenF; }
                }

                float crossX = ny * orthoZ - nz * orthoY;
                float crossY = nz * orthoX - nx * orthoZ;
                float crossZ = nx * orthoY - ny * orthoX;

                float dotB = crossX * tan2[i][0] + crossY * tan2[i][1] + crossZ * tan2[i][2];
                float w = (dotB < 0.0f) ? -1.0f : 1.0f;

                tangents[i] = { orthoX, orthoY, orthoZ, w };
            }

            return tangents;
        }

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

        template <typename TVertex>
        bool IsDegenerateTriangle(
            const std::vector<TVertex>& vertices,
            uint32_t i0,
            uint32_t i1,
            uint32_t i2);

        struct MeshAuditStats
        {
            bool hasFiniteBounds = false;
            float minX = std::numeric_limits<float>::max();
            float minY = std::numeric_limits<float>::max();
            float minZ = std::numeric_limits<float>::max();
            float maxX = -std::numeric_limits<float>::max();
            float maxY = -std::numeric_limits<float>::max();
            float maxZ = -std::numeric_limits<float>::max();
            size_t nonFinitePositions = 0;
            size_t nonFiniteNormals = 0;
            size_t zeroNormals = 0;
            size_t validPrimitives = 0;
            size_t invalidRangePrimitives = 0;
            size_t invalidIndexPrimitives = 0;
            size_t degenerateTriangles = 0;
            size_t outOfRangeMaterials = 0;
            std::vector<uint32_t> materialSlots;
        };

        std::string FormatAuditFloat(float value)
        {
            if (!std::isfinite(value))
            {
                return "nan";
            }

            std::ostringstream text;
            text << std::fixed << std::setprecision(3) << value;
            return text.str();
        }

        std::string FormatBounds(const MeshAuditStats& stats)
        {
            if (!stats.hasFiniteBounds)
            {
                return "none";
            }

            std::ostringstream text;
            text << "["
                 << FormatAuditFloat(stats.minX) << "," << FormatAuditFloat(stats.minY) << "," << FormatAuditFloat(stats.minZ)
                 << "]-["
                 << FormatAuditFloat(stats.maxX) << "," << FormatAuditFloat(stats.maxY) << "," << FormatAuditFloat(stats.maxZ)
                 << "]";
            return text.str();
        }

        std::string FormatMaterialSlots(std::vector<uint32_t> slots)
        {
            if (slots.empty())
            {
                return "none";
            }

            std::sort(slots.begin(), slots.end());
            slots.erase(std::unique(slots.begin(), slots.end()), slots.end());

            std::ostringstream text;
            const size_t maxListed = std::min<size_t>(slots.size(), 12);
            for (size_t i = 0; i < maxListed; ++i)
            {
                if (i > 0)
                {
                    text << ",";
                }
                text << slots[i];
            }
            if (slots.size() > maxListed)
            {
                text << ",+" << (slots.size() - maxListed);
            }
            return text.str();
        }

        template <typename TVertex>
        MeshAuditStats AuditMeshGeometry(
            const std::vector<TVertex>& vertices,
            const std::vector<uint32_t>& indices,
            const std::vector<DagorFormats::RendInstPrimitive>& primitives,
            size_t materialCount)
        {
            MeshAuditStats stats;
            for (const auto& vertex : vertices)
            {
                if (std::isfinite(vertex.x) && std::isfinite(vertex.y) && std::isfinite(vertex.z))
                {
                    stats.hasFiniteBounds = true;
                    stats.minX = std::min(stats.minX, vertex.x);
                    stats.minY = std::min(stats.minY, vertex.y);
                    stats.minZ = std::min(stats.minZ, vertex.z);
                    stats.maxX = std::max(stats.maxX, vertex.x);
                    stats.maxY = std::max(stats.maxY, vertex.y);
                    stats.maxZ = std::max(stats.maxZ, vertex.z);
                }
                else
                {
                    ++stats.nonFinitePositions;
                }

                const float normalLenSq = (vertex.nx * vertex.nx) + (vertex.ny * vertex.ny) + (vertex.nz * vertex.nz);
                if (!std::isfinite(normalLenSq))
                {
                    ++stats.nonFiniteNormals;
                }
                else if (normalLenSq <= 1e-12f)
                {
                    ++stats.zeroNormals;
                }
            }

            for (const DagorFormats::RendInstPrimitive& primitive : primitives)
            {
                if (primitive.indexCount == 0 ||
                    primitive.firstIndex >= indices.size() ||
                    primitive.indexCount > indices.size() - primitive.firstIndex ||
                    (primitive.indexCount % 3) != 0)
                {
                    ++stats.invalidRangePrimitives;
                    continue;
                }

                bool hasOutOfRangeIndex = false;
                for (uint32_t i = 0; i < primitive.indexCount; ++i)
                {
                    if (indices[static_cast<size_t>(primitive.firstIndex) + i] >= vertices.size())
                    {
                        hasOutOfRangeIndex = true;
                        break;
                    }
                }
                if (hasOutOfRangeIndex)
                {
                    ++stats.invalidIndexPrimitives;
                    continue;
                }

                ++stats.validPrimitives;
                stats.materialSlots.push_back(primitive.materialIndex);
                if (materialCount > 0 && primitive.materialIndex >= materialCount)
                {
                    ++stats.outOfRangeMaterials;
                }

                for (uint32_t offset = 0; offset + 2 < primitive.indexCount; offset += 3)
                {
                    const size_t src = static_cast<size_t>(primitive.firstIndex) + offset;
                    if (IsDegenerateTriangle(vertices, indices[src], indices[src + 1], indices[src + 2]))
                    {
                        ++stats.degenerateTriangles;
                    }
                }
            }

            return stats;
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

        std::array<float, 16> TransposeMatrix(const std::array<float, 16>& matrix)
        {
            return {
                matrix[0], matrix[4], matrix[8],  matrix[12],
                matrix[1], matrix[5], matrix[9],  matrix[13],
                matrix[2], matrix[6], matrix[10], matrix[14],
                matrix[3], matrix[7], matrix[11], matrix[15],
            };
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

        std::array<float, 3> TransformPointRowMajor(
            const std::array<float, 16>& matrix,
            const std::array<float, 3>& point)
        {
            return {
                (matrix[0] * point[0]) + (matrix[1] * point[1]) + (matrix[2] * point[2]) + matrix[3],
                (matrix[4] * point[0]) + (matrix[5] * point[1]) + (matrix[6] * point[2]) + matrix[7],
                (matrix[8] * point[0]) + (matrix[9] * point[1]) + (matrix[10] * point[2]) + matrix[11]
            };
        }

        std::array<float, 3> TransformDirectionRowMajor(
            const std::array<float, 16>& matrix,
            const std::array<float, 3>& direction)
        {
            return {
                (matrix[0] * direction[0]) + (matrix[1] * direction[1]) + (matrix[2] * direction[2]),
                (matrix[4] * direction[0]) + (matrix[5] * direction[1]) + (matrix[6] * direction[2]),
                (matrix[8] * direction[0]) + (matrix[9] * direction[1]) + (matrix[10] * direction[2])
            };
        }

        std::array<float, 16> MultiplyAffineRowMajor(
            const std::array<float, 16>& left,
            const std::array<float, 16>& right)
        {
            std::array<float, 16> result = {};
            result[0] = (left[0] * right[0]) + (left[1] * right[4]) + (left[2] * right[8]);
            result[1] = (left[0] * right[1]) + (left[1] * right[5]) + (left[2] * right[9]);
            result[2] = (left[0] * right[2]) + (left[1] * right[6]) + (left[2] * right[10]);
            result[3] = (left[0] * right[3]) + (left[1] * right[7]) + (left[2] * right[11]) + left[3];

            result[4] = (left[4] * right[0]) + (left[5] * right[4]) + (left[6] * right[8]);
            result[5] = (left[4] * right[1]) + (left[5] * right[5]) + (left[6] * right[9]);
            result[6] = (left[4] * right[2]) + (left[5] * right[6]) + (left[6] * right[10]);
            result[7] = (left[4] * right[3]) + (left[5] * right[7]) + (left[6] * right[11]) + left[7];

            result[8] = (left[8] * right[0]) + (left[9] * right[4]) + (left[10] * right[8]);
            result[9] = (left[8] * right[1]) + (left[9] * right[5]) + (left[10] * right[9]);
            result[10] = (left[8] * right[2]) + (left[9] * right[6]) + (left[10] * right[10]);
            result[11] = (left[8] * right[3]) + (left[9] * right[7]) + (left[10] * right[11]) + left[11];

            result[15] = 1.0f;
            return result;
        }

        float DotVec3(const std::array<float, 3>& left, const std::array<float, 3>& right)
        {
            return (left[0] * right[0]) + (left[1] * right[1]) + (left[2] * right[2]);
        }

        std::array<float, 3> CrossVec3(const std::array<float, 3>& left, const std::array<float, 3>& right)
        {
            return {
                (left[1] * right[2]) - (left[2] * right[1]),
                (left[2] * right[0]) - (left[0] * right[2]),
                (left[0] * right[1]) - (left[1] * right[0])
            };
        }

        float LengthVec3(const std::array<float, 3>& value)
        {
            return std::sqrt(DotVec3(value, value));
        }

        bool NormalizeVec3(std::array<float, 3>& value)
        {
            const float length = LengthVec3(value);
            if (!std::isfinite(length) || length <= 1e-6f)
            {
                value = { 0.0f, 0.0f, 0.0f };
                return false;
            }

            const float invLength = 1.0f / length;
            value[0] *= invLength;
            value[1] *= invLength;
            value[2] *= invLength;
            return true;
        }

        bool InvertAffineRowMajor(const std::array<float, 16>& matrix, std::array<float, 16>& inverse)
        {
            const float m00 = matrix[0];
            const float m01 = matrix[1];
            const float m02 = matrix[2];
            const float m10 = matrix[4];
            const float m11 = matrix[5];
            const float m12 = matrix[6];
            const float m20 = matrix[8];
            const float m21 = matrix[9];
            const float m22 = matrix[10];

            const float c00 = (m11 * m22) - (m12 * m21);
            const float c01 = (m02 * m21) - (m01 * m22);
            const float c02 = (m01 * m12) - (m02 * m11);
            const float c10 = (m12 * m20) - (m10 * m22);
            const float c11 = (m00 * m22) - (m02 * m20);
            const float c12 = (m02 * m10) - (m00 * m12);
            const float c20 = (m10 * m21) - (m11 * m20);
            const float c21 = (m01 * m20) - (m00 * m21);
            const float c22 = (m00 * m11) - (m01 * m10);

            const float det = (m00 * c00) + (m01 * c10) + (m02 * c20);
            if (!std::isfinite(det) || std::fabs(det) <= 1e-8f)
            {
                return false;
            }

            const float invDet = 1.0f / det;
            inverse = {};
            inverse[0] = c00 * invDet;
            inverse[1] = c01 * invDet;
            inverse[2] = c02 * invDet;
            inverse[4] = c10 * invDet;
            inverse[5] = c11 * invDet;
            inverse[6] = c12 * invDet;
            inverse[8] = c20 * invDet;
            inverse[9] = c21 * invDet;
            inverse[10] = c22 * invDet;
            inverse[15] = 1.0f;

            const float tx = matrix[3];
            const float ty = matrix[7];
            const float tz = matrix[11];
            inverse[3] = -((tx * inverse[0]) + (ty * inverse[1]) + (tz * inverse[2]));
            inverse[7] = -((tx * inverse[4]) + (ty * inverse[5]) + (tz * inverse[6]));
            inverse[11] = -((tx * inverse[8]) + (ty * inverse[9]) + (tz * inverse[10]));
            return true;
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

            const MaterialInfo& material = selection.materials[remapIt->second];
            return !material.diffuseTexturePath.empty() ||
                   !material.normalTexturePath.empty() ||
                   !material.metallicRoughnessTexturePath.empty() ||
                   !material.occlusionTexturePath.empty() ||
                   !material.nativePackedNormalPbrTexturePath.empty();
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

        struct MaterialTextureIndices
        {
            int diffuseIndex = -1;
            int normalIndex = -1;
            int metallicRoughnessIndex = -1;
            int occlusionIndex = -1;
            int nativePackedNormalPbrIndex = -1;
        };

        struct GlbTextureSections
        {
            std::vector<MaterialTextureIndices> materialTextureIndices;
            std::vector<GlbImageInfo> images;
            size_t embeddedImageCount = 0;
            bool usesWarThunderPackedMaterialExtension = false;
        };

        GlbTextureSections PrepareMaterialTextures(
            const std::vector<MaterialInfo>& materials,
            const GlbExportOptions& options,
            std::vector<uint8_t>& binBuffer,
            std::ostream& debugLog)
        {
            GlbTextureSections sections;
            sections.materialTextureIndices.resize(materials.size());

            std::unordered_map<std::string, int> textureIndexByKey;

            auto getOrAddTexture = [&](
                const std::string& texturePath,
                const std::filesystem::path& sourcePath,
                const std::shared_ptr<const std::vector<uint8_t>>& embeddedBytes) -> int {
                if (texturePath.empty())
                {
                    return -1;
                }

                const std::string dedupeKey = !sourcePath.empty()
                    ? ("source:" + sourcePath.lexically_normal().generic_string())
                    : ("uri:" + texturePath);

                const auto existing = textureIndexByKey.find(dedupeKey);
                if (existing != textureIndexByKey.end())
                {
                    return existing->second;
                }

                GlbImageInfo image;
                image.uri = texturePath;

                if (options.embedTexturesInGlb && ((embeddedBytes && !embeddedBytes->empty()) || !sourcePath.empty()))
                {
                    const std::filesystem::path mimePath = !sourcePath.empty()
                        ? sourcePath
                        : std::filesystem::path(texturePath);
                    image.mimeType = MimeTypeForTexturePath(mimePath);
                    if (image.mimeType.empty() && embeddedBytes && embeddedBytes->size() >= 8)
                    {
                        constexpr std::array<uint8_t, 8> kPngSig = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
                        if (std::equal(kPngSig.begin(), kPngSig.end(), embeddedBytes->begin()))
                        {
                            image.mimeType = "image/png";
                        }
                    }

                    if (!image.mimeType.empty())
                    {
                        auto appendImageBytes = [&](const std::vector<uint8_t>& bytes) {
                            while (binBuffer.size() % 4 != 0)
                            {
                                binBuffer.push_back(0);
                            }
                            image.byteOffset = binBuffer.size();
                            image.byteLength = bytes.size();
                            image.embeddedOrdinal = sections.embeddedImageCount++;
                            image.embedded = true;
                            binBuffer.insert(binBuffer.end(), bytes.begin(), bytes.end());
                            while (binBuffer.size() % 4 != 0)
                            {
                                binBuffer.push_back(0);
                            }
                        };

                        if (embeddedBytes && !embeddedBytes->empty())
                        {
                            appendImageBytes(*embeddedBytes);
                        }
                        else if (std::optional<std::vector<uint8_t>> bytes = ReadBinaryFile(sourcePath))
                        {
                            appendImageBytes(*bytes);
                        }
                        else
                        {
                            debugLog << "  Texture embed skipped; could not read "
                                     << sourcePath.string() << std::endl;
                        }
                    }
                    else
                    {
                        debugLog << "  Texture embed skipped; unsupported glTF image type for "
                                 << mimePath.string() << std::endl;
                    }
                }

                const int textureIndex = static_cast<int>(sections.images.size());
                textureIndexByKey.emplace(dedupeKey, textureIndex);
                sections.images.push_back(std::move(image));
                return textureIndex;
            };

            for (size_t i = 0; i < materials.size(); ++i)
            {
                sections.materialTextureIndices[i].diffuseIndex = getOrAddTexture(
                    materials[i].diffuseTexturePath,
                    materials[i].diffuseTextureSourcePath,
                    materials[i].diffuseTextureEmbeddedBytes);
                sections.materialTextureIndices[i].normalIndex = getOrAddTexture(
                    materials[i].normalTexturePath,
                    materials[i].normalTextureSourcePath,
                    materials[i].normalTextureEmbeddedBytes);
                sections.materialTextureIndices[i].metallicRoughnessIndex = getOrAddTexture(
                    materials[i].metallicRoughnessTexturePath,
                    materials[i].metallicRoughnessTextureSourcePath,
                    materials[i].metallicRoughnessTextureEmbeddedBytes);
                sections.materialTextureIndices[i].occlusionIndex = getOrAddTexture(
                    materials[i].occlusionTexturePath,
                    materials[i].occlusionTextureSourcePath,
                    materials[i].occlusionTextureEmbeddedBytes);
                sections.materialTextureIndices[i].nativePackedNormalPbrIndex =
                    getOrAddTexture(
                        materials[i].nativePackedNormalPbrTexturePath,
                        materials[i].nativePackedNormalPbrTextureSourcePath,
                        materials[i].nativePackedNormalPbrTextureEmbeddedBytes);
                sections.usesWarThunderPackedMaterialExtension |=
                    sections.materialTextureIndices[i].nativePackedNormalPbrIndex >= 0;
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
                if (textureSections.materialTextureIndices[i].diffuseIndex >= 0)
                {
                    json << "        \"baseColorTexture\": { \"index\": " << textureSections.materialTextureIndices[i].diffuseIndex << " }";
                }
                else
                {
                    json << "        \"baseColorFactor\": [1, 1, 1, 1]";
                }

                if (textureSections.materialTextureIndices[i].metallicRoughnessIndex >= 0)
                {
                    json << ",\n";
                    json << "        \"metallicRoughnessTexture\": { \"index\": "
                         << textureSections.materialTextureIndices[i].metallicRoughnessIndex << " },\n";
                    json << "        \"metallicFactor\": 1.0,\n";
                    json << "        \"roughnessFactor\": 1.0\n";
                }
                else
                {
                    json << ",\n";
                    json << "        \"metallicFactor\": 0.0,\n";
                    json << "        \"roughnessFactor\": 1.0\n";
                }
                json << "      }";

                if (textureSections.materialTextureIndices[i].normalIndex >= 0)
                {
                    json << ",\n      \"normalTexture\": { \"index\": " << textureSections.materialTextureIndices[i].normalIndex << " }";
                }

                if (textureSections.materialTextureIndices[i].occlusionIndex >= 0)
                {
                    json << ",\n      \"occlusionTexture\": { \"index\": " << textureSections.materialTextureIndices[i].occlusionIndex << " }";
                }

                if (textureSections.materialTextureIndices[i].nativePackedNormalPbrIndex >= 0)
                {
                    json << ",\n      \"extensions\": {\n";
                    json << "        \"" << WarThunderPackedMaterialExtension << "\": {\n";
                    json << "          \"texture\": { \"index\": "
                         << textureSections.materialTextureIndices[i].nativePackedNormalPbrIndex << " },\n";
                    json << "          \"channels\": {\n";
                    json << "            \"invertedRoughness\": \"R\",\n";
                    json << "            \"normalX\": \"G\",\n";
                    json << "            \"metallic\": \"B\",\n";
                    json << "            \"normalY\": \"A\"\n";
                    json << "          },\n";
                    json << "          \"normalZ\": \"sqrt(max(0, 1 - x*x - y*y))\",\n";
                    json << "          \"roughness\": \"clamp((cbrt(1-R)-0.3)/0.7,0,1)\",\n";
                    json << "          \"ambientOcclusion\": \"not_present\"\n";
                    json << "        }\n";
                    json << "      }";
                }

                json << "\n";
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

        void WriteMaterialExtensionsUsed(
            std::stringstream& json,
            const GlbTextureSections& textureSections)
        {
            if (!textureSections.usesWarThunderPackedMaterialExtension)
            {
                return;
            }

            json << "  \"extensionsUsed\": [\""
                 << WarThunderPackedMaterialExtension << "\"],\n";
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

        struct RigidAnimationBindingInfo
        {
            std::string nodeName;
            size_t targetNodeIndex = 0;
            uint16_t parentJoint = 0;
        };

        std::array<float, 3> ConvertAnimationTranslationForGltf(const std::array<float, 3>& value)
        {
            return { value[0], value[1], -value[2] };
        }

        std::array<float, 4> ConvertAnimationRotationForGltf(std::array<float, 4> value, bool parentIsMirrored = false)
        {
            // Dagor mesh positions are converted with a Z reflection. For local
            // quaternions, S * R * S maps (x, y, z, w) to (-x, -y, z, w).
            // If the parent is mirrored in real world coordinates, the parent's Z-reflection
            // cancels the global Z-reflection.
            if (!parentIsMirrored)
            {
                value = { -value[0], -value[1], value[2], value[3] };
            }
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

        bool NearlyEqualFloat(float left, float right, float epsilon = 1e-4f)
        {
            return std::fabs(left - right) <= epsilon;
        }

        bool NearlyEqualVec3(
            const std::array<float, 3>& left,
            const std::array<float, 3>& right,
            float epsilon = 1e-4f)
        {
            return NearlyEqualFloat(left[0], right[0], epsilon) &&
                NearlyEqualFloat(left[1], right[1], epsilon) &&
                NearlyEqualFloat(left[2], right[2], epsilon);
        }

        bool NearlyEqualQuat(
            const std::array<float, 4>& left,
            const std::array<float, 4>& right,
            float epsilon = 1e-4f)
        {
            const float dot =
                (left[0] * right[0]) +
                (left[1] * right[1]) +
                (left[2] * right[2]) +
                (left[3] * right[3]);
            return std::fabs(std::fabs(dot) - 1.0f) <= epsilon;
        }

        bool IsIdentityTranslation(const std::array<float, 3>& translation)
        {
            return NearlyEqualVec3(translation, { 0.0f, 0.0f, 0.0f });
        }

        bool IsIdentityRotation(const std::array<float, 4>& rotation)
        {
            return NearlyEqualQuat(rotation, { 0.0f, 0.0f, 0.0f, 1.0f });
        }

        bool IsIdentityScale(const std::array<float, 3>& scale)
        {
            return NearlyEqualVec3(scale, { 1.0f, 1.0f, 1.0f });
        }

        void NormalizeQuatInPlace(std::array<float, 4>& rotation)
        {
            const float lengthSq =
                (rotation[0] * rotation[0]) +
                (rotation[1] * rotation[1]) +
                (rotation[2] * rotation[2]) +
                (rotation[3] * rotation[3]);
            if (!std::isfinite(lengthSq) || lengthSq <= 1e-12f)
            {
                rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
                return;
            }

            const float invLength = 1.0f / std::sqrt(lengthSq);
            rotation[0] *= invLength;
            rotation[1] *= invLength;
            rotation[2] *= invLength;
            rotation[3] *= invLength;
        }

        std::array<float, 4> MultiplyQuaternions(
            const std::array<float, 4>& left,
            const std::array<float, 4>& right)
        {
            std::array<float, 4> result{
                (left[3] * right[0]) + (left[0] * right[3]) + (left[1] * right[2]) - (left[2] * right[1]),
                (left[3] * right[1]) - (left[0] * right[2]) + (left[1] * right[3]) + (left[2] * right[0]),
                (left[3] * right[2]) + (left[0] * right[1]) - (left[1] * right[0]) + (left[2] * right[3]),
                (left[3] * right[3]) - (left[0] * right[0]) - (left[1] * right[1]) - (left[2] * right[2]),
            };
            NormalizeQuatInPlace(result);
            return result;
        }

        std::array<float, 4> InverseQuaternion(std::array<float, 4> rotation)
        {
            NormalizeQuatInPlace(rotation);
            return { -rotation[0], -rotation[1], -rotation[2], rotation[3] };
        }

        GltfNodeTrs ApplyAdditiveTrackToBase(
            const GltfNodeTrs& base,
            const std::array<float, 3>& translation,
            const std::array<float, 4>& rotation,
            const std::array<float, 3>& scale)
        {
            GltfNodeTrs result;
            result.translation = {
                base.translation[0] + translation[0],
                base.translation[1] + translation[1],
                base.translation[2] + translation[2],
            };
            result.rotation = MultiplyQuaternions(base.rotation, rotation);
            result.scale = {
                base.scale[0] * scale[0],
                base.scale[1] * scale[1],
                base.scale[2] * scale[2],
            };
            return result;
        }

        GltfNodeTrs DecomposeGltfMatrix(const std::array<float, 16>& matrix)
        {
            GltfNodeTrs trs;
            trs.translation = { matrix[12], matrix[13], matrix[14] };

            std::array<float, 3> xAxis = { matrix[0], matrix[1], matrix[2] };
            std::array<float, 3> yAxis = { matrix[4], matrix[5], matrix[6] };
            std::array<float, 3> zAxis = { matrix[8], matrix[9], matrix[10] };
            trs.scale = { LengthVec3(xAxis), LengthVec3(yAxis), LengthVec3(zAxis) };

            for (float& scale : trs.scale)
            {
                if (!std::isfinite(scale) || scale <= 1e-6f)
                {
                    scale = 1.0f;
                }
            }

            xAxis[0] /= trs.scale[0];
            xAxis[1] /= trs.scale[0];
            xAxis[2] /= trs.scale[0];
            yAxis[0] /= trs.scale[1];
            yAxis[1] /= trs.scale[1];
            yAxis[2] /= trs.scale[1];
            zAxis[0] /= trs.scale[2];
            zAxis[1] /= trs.scale[2];
            zAxis[2] /= trs.scale[2];

            const float handedness = DotVec3(CrossVec3(xAxis, yAxis), zAxis);
            if (std::isfinite(handedness) && handedness < 0.0f)
            {
                trs.scale[2] = -trs.scale[2];
            }

            if (!NormalizeVec3(xAxis))
            {
                xAxis = { 1.0f, 0.0f, 0.0f };
            }

            yAxis = {
                yAxis[0] - (DotVec3(xAxis, yAxis) * xAxis[0]),
                yAxis[1] - (DotVec3(xAxis, yAxis) * xAxis[1]),
                yAxis[2] - (DotVec3(xAxis, yAxis) * xAxis[2])
            };
            if (!NormalizeVec3(yAxis))
            {
                yAxis = { 0.0f, 1.0f, 0.0f };
            }

            zAxis = CrossVec3(xAxis, yAxis);
            if (!NormalizeVec3(zAxis))
            {
                zAxis = { 0.0f, 0.0f, 1.0f };
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

        struct PreparedAnimationNodeTrack
        {
            std::vector<float> times;
            std::vector<std::array<float, 3>> translations;
            std::vector<std::array<float, 4>> rotations;
            std::vector<std::array<float, 3>> scales;
        };

        void RestoreZeroTranslationPlaceholders(
            std::vector<std::array<float, 3>>& translations,
            const GltfNodeTrs& restTrs,
            std::ostream& debugLog)
        {
            size_t replacedCount = 0;
            for (std::array<float, 3>& translation : translations)
            {
                if (IsIdentityTranslation(translation))
                {
                    translation = restTrs.translation;
                    replacedCount++;
                }
            }
            if (replacedCount > 0)
            {
                debugLog << "    [RestoreZeroTranslationPlaceholders] Replaced " << replacedCount 
                         << " identity translations with rest/inherited translation: [" 
                         << restTrs.translation[0] << ", " << restTrs.translation[1] << ", " << restTrs.translation[2] << "]" << std::endl;
            }
        }

        void RebaseIdentityRotationPlaceholders(
            std::vector<std::array<float, 4>>& rotations,
            const GltfNodeTrs& restTrs,
            std::ostream& debugLog)
        {
            if (rotations.empty() ||
                IsIdentityRotation(restTrs.rotation) ||
                !IsIdentityRotation(rotations.front()))
            {
                return;
            }

            debugLog << "    [RebaseIdentityRotationPlaceholders] First rotation is identity; rebasing rotation track using rest rotation: ["
                     << restTrs.rotation[0] << ", " << restTrs.rotation[1] << ", " << restTrs.rotation[2] << ", " << restTrs.rotation[3] << "]" << std::endl;

            const std::array<float, 4> invFirstRotation = InverseQuaternion(rotations.front());
            for (std::array<float, 4>& rotation : rotations)
            {
                const std::array<float, 4> delta =
                    MultiplyQuaternions(invFirstRotation, rotation);
                rotation = MultiplyQuaternions(restTrs.rotation, delta);
            }
            rotations.front() = restTrs.rotation;
        }

        void AlignPreparedTrackStart(
            PreparedAnimationNodeTrack& prepared)
        {
            if (prepared.times.empty())
            {
                return;
            }

            constexpr float kFirstSampleEpsilon = 1e-5f;
            if (prepared.times.front() > kFirstSampleEpsilon)
            {
                prepared.times.insert(prepared.times.begin(), 0.0f);
                if (!prepared.translations.empty())
                {
                    prepared.translations.insert(prepared.translations.begin(), prepared.translations.front());
                }
                if (!prepared.rotations.empty())
                {
                    prepared.rotations.insert(prepared.rotations.begin(), prepared.rotations.front());
                }
                if (!prepared.scales.empty())
                {
                    prepared.scales.insert(prepared.scales.begin(), prepared.scales.front());
                }
                return;
            }

            prepared.times.front() = 0.0f;
        }

        std::array<float, 3> LerpVec3(
            const std::array<float, 3>& left,
            const std::array<float, 3>& right,
            float t)
        {
            return {
                left[0] + ((right[0] - left[0]) * t),
                left[1] + ((right[1] - left[1]) * t),
                left[2] + ((right[2] - left[2]) * t)
            };
        }

        std::array<float, 4> NlerpQuat(
            std::array<float, 4> left,
            std::array<float, 4> right,
            float t)
        {
            const float dot =
                (left[0] * right[0]) +
                (left[1] * right[1]) +
                (left[2] * right[2]) +
                (left[3] * right[3]);
            if (dot < 0.0f)
            {
                right[0] = -right[0];
                right[1] = -right[1];
                right[2] = -right[2];
                right[3] = -right[3];
            }

            std::array<float, 4> blended{
                left[0] + ((right[0] - left[0]) * t),
                left[1] + ((right[1] - left[1]) * t),
                left[2] + ((right[2] - left[2]) * t),
                left[3] + ((right[3] - left[3]) * t)
            };
            NormalizeQuatInPlace(blended);
            return blended;
        }

        std::array<float, 16> ComposeRowMajorMatrixFromTrs(const GltfNodeTrs& trs)
        {
            const float x = trs.rotation[0];
            const float y = trs.rotation[1];
            const float z = trs.rotation[2];
            const float w = trs.rotation[3];

            const float xx = x * x;
            const float yy = y * y;
            const float zz = z * z;
            const float xy = x * y;
            const float xz = x * z;
            const float yz = y * z;
            const float wx = w * x;
            const float wy = w * y;
            const float wz = w * z;

            const float m00 = 1.0f - (2.0f * (yy + zz));
            const float m01 = 2.0f * (xy - wz);
            const float m02 = 2.0f * (xz + wy);
            const float m10 = 2.0f * (xy + wz);
            const float m11 = 1.0f - (2.0f * (xx + zz));
            const float m12 = 2.0f * (yz - wx);
            const float m20 = 2.0f * (xz - wy);
            const float m21 = 2.0f * (yz + wx);
            const float m22 = 1.0f - (2.0f * (xx + yy));

            return {
                m00 * trs.scale[0], m01 * trs.scale[1], m02 * trs.scale[2], trs.translation[0],
                m10 * trs.scale[0], m11 * trs.scale[1], m12 * trs.scale[2], trs.translation[1],
                m20 * trs.scale[0], m21 * trs.scale[1], m22 * trs.scale[2], trs.translation[2],
                0.0f,               0.0f,               0.0f,               1.0f
            };
        }

        PreparedAnimationNodeTrack PrepareAnimationTrackForSampling(
            const AnimationClip& clip,
            const AnimationNodeTrack& track,
            const GltfNodeTrs& restTrs,
            const GltfNodeTrs& targetRestTrs,
            std::ostream& debugLog,
            const std::array<float, 3>& parentScale = { 1.0f, 1.0f, 1.0f },
            bool restoreZeroTranslationPlaceholders = true,
            bool bypassAnimationRebase = false)
        {
            debugLog << "  [PrepareAnimationTrackForSampling] Node: " << track.nodeName 
                     << " Clip: " << clip.name << " samples: " << track.times.size() << std::endl;
            PreparedAnimationNodeTrack prepared;
            prepared.times = track.times;
            
            prepared.translations.reserve(track.translations.size());
            for (const std::array<float, 3>& translation : track.translations)
            {
                std::array<float, 3> converted = ConvertAnimationTranslationForGltf(translation);
                converted[0] *= parentScale[0];
                converted[1] *= parentScale[1];
                converted[2] *= parentScale[2];
                prepared.translations.push_back(converted);
            }

            const bool parentIsMirrored = (parentScale[2] < 0.0f);
            prepared.rotations.reserve(track.rotations.size());
            for (const std::array<float, 4>& rotation : track.rotations)
            {
                prepared.rotations.push_back(ConvertAnimationRotationForGltf(rotation, parentIsMirrored));
            }

            prepared.scales = track.scales;

            const size_t sampleCount = prepared.times.size();
            debugLog << "    Raw keyframe counts: trans=" << track.translations.size() 
                     << " rot=" << track.rotations.size() 
                     << " scale=" << track.scales.size() << std::endl;
            
            if (sampleCount > 0)
            {
                if (!prepared.translations.empty())
                {
                    debugLog << "    First frame raw translation: [" << track.translations.front()[0] << ", " << track.translations.front()[1] << ", " << track.translations.front()[2] << "]"
                             << " -> converted: [" << prepared.translations.front()[0] << ", " << prepared.translations.front()[1] << ", " << prepared.translations.front()[2] << "]" << std::endl;
                    debugLog << "    Rest translation: [" << restTrs.translation[0] << ", " << restTrs.translation[1] << ", " << restTrs.translation[2] << "]"
                             << " targetRest: [" << targetRestTrs.translation[0] << ", " << targetRestTrs.translation[1] << ", " << targetRestTrs.translation[2] << "]" << std::endl;
                }
                if (!prepared.rotations.empty())
                {
                    debugLog << "    First frame raw rotation: [" << track.rotations.front()[0] << ", " << track.rotations.front()[1] << ", " << track.rotations.front()[2] << ", " << track.rotations.front()[3] << "]"
                             << " -> converted: [" << prepared.rotations.front()[0] << ", " << prepared.rotations.front()[1] << ", " << prepared.rotations.front()[2] << ", " << prepared.rotations.front()[3] << "]" << std::endl;
                    debugLog << "    Rest rotation: [" << restTrs.rotation[0] << ", " << restTrs.rotation[1] << ", " << restTrs.rotation[2] << ", " << restTrs.rotation[3] << "]"
                             << " targetRest: [" << targetRestTrs.rotation[0] << ", " << targetRestTrs.rotation[1] << ", " << targetRestTrs.rotation[2] << ", " << targetRestTrs.rotation[3] << "]" << std::endl;
                }
            }

            const auto expandVec3 = [sampleCount](std::vector<std::array<float, 3>>& values, const std::array<float, 3>& fallback)
            {
                if (sampleCount == 0)
                {
                    return;
                }
                if (values.empty())
                {
                    values.resize(sampleCount, fallback);
                }
                else if (values.size() < sampleCount)
                {
                    values.resize(sampleCount, values.back());
                }
            };
            const auto expandQuat = [sampleCount](std::vector<std::array<float, 4>>& values, const std::array<float, 4>& fallback)
            {
                if (sampleCount == 0)
                {
                    return;
                }
                if (values.empty())
                {
                    values.resize(sampleCount, fallback);
                }
                else if (values.size() < sampleCount)
                {
                    values.resize(sampleCount, values.back());
                }
            };

            if (clip.additive)
            {
                debugLog << "    Clip is additive. Combining track values with rest pose." << std::endl;
                if (sampleCount > 0)
                {
                    expandVec3(prepared.translations, { 0.0f, 0.0f, 0.0f });
                    expandQuat(prepared.rotations, { 0.0f, 0.0f, 0.0f, 1.0f });
                    expandVec3(prepared.scales, { 1.0f, 1.0f, 1.0f });

                    for (size_t sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
                    {
                        const GltfNodeTrs combined = ApplyAdditiveTrackToBase(
                            restTrs,
                            prepared.translations[sampleIndex],
                            prepared.rotations[sampleIndex],
                            prepared.scales[sampleIndex]);
                        prepared.translations[sampleIndex] = combined.translation;
                        prepared.rotations[sampleIndex] = combined.rotation;
                        prepared.scales[sampleIndex] = combined.scale;
                    }
                }
            }
            else if (restoreZeroTranslationPlaceholders && !bypassAnimationRebase)
            {
                if (!prepared.translations.empty() &&
                    !IsIdentityTranslation(restTrs.translation))
                {
                    RestoreZeroTranslationPlaceholders(prepared.translations, restTrs, debugLog);
                }
                RebaseIdentityRotationPlaceholders(prepared.rotations, restTrs, debugLog);
            }

            AlignPreparedTrackStart(prepared);
            return prepared;
        }

        GltfNodeTrs EvaluatePreparedAnimationTrackAtTime(
            const PreparedAnimationNodeTrack* track,
            const GltfNodeTrs& restTrs,
            float time)
        {
            if (!track || !HasValidAnimationTimes(track->times))
            {
                return restTrs;
            }

            const std::vector<float>& times = track->times;
            if (times.size() == 1)
            {
                GltfNodeTrs evaluated = restTrs;
                if (!track->translations.empty())
                {
                    evaluated.translation = track->translations.front();
                }
                if (!track->rotations.empty())
                {
                    evaluated.rotation = track->rotations.front();
                }
                if (!track->scales.empty())
                {
                    evaluated.scale = track->scales.front();
                }
                return evaluated;
            }

            size_t upperIndex = static_cast<size_t>(
                std::lower_bound(times.begin(), times.end(), time) - times.begin());
            if (upperIndex == 0)
            {
                upperIndex = 1;
            }
            else if (upperIndex >= times.size())
            {
                upperIndex = times.size() - 1;
            }

            const size_t lowerIndex = upperIndex - 1;
            const float t0 = times[lowerIndex];
            const float t1 = times[upperIndex];
            const float alpha =
                (upperIndex == lowerIndex || !std::isfinite(t1 - t0) || std::fabs(t1 - t0) <= 1e-6f)
                    ? 0.0f
                    : std::clamp((time - t0) / (t1 - t0), 0.0f, 1.0f);

            GltfNodeTrs evaluated = restTrs;
            if (!track->translations.empty())
            {
                const auto getTranslation = [&](size_t index) -> const std::array<float, 3>&
                {
                    return track->translations[std::min(index, track->translations.size() - 1)];
                };
                evaluated.translation = LerpVec3(getTranslation(lowerIndex), getTranslation(upperIndex), alpha);
            }
            if (!track->rotations.empty())
            {
                const auto getRotation = [&](size_t index) -> const std::array<float, 4>&
                {
                    return track->rotations[std::min(index, track->rotations.size() - 1)];
                };
                evaluated.rotation = NlerpQuat(getRotation(lowerIndex), getRotation(upperIndex), alpha);
            }
            if (!track->scales.empty())
            {
                const auto getScale = [&](size_t index) -> const std::array<float, 3>&
                {
                    return track->scales[std::min(index, track->scales.size() - 1)];
                };
                evaluated.scale = LerpVec3(getScale(lowerIndex), getScale(upperIndex), alpha);
            }
            return evaluated;
        }

        enum class AnimationBindingMode
        {
            AbsoluteLocal,
            BindRelativeDelta
        };

        AnimationBindingMode DetectAnimationBindingMode(
            const std::vector<AnimationClip>& animations,
            const std::unordered_map<std::string, size_t>& skeletonNodeLookup,
            const std::vector<GltfNodeTrs>& restTrs,
            const std::vector<DagorFormats::GeomNodeTreeNode>& skeleton,
            const std::vector<std::array<float, 3>>& skeletonWorldScalesReal,
            std::ostream& debugLog)
        {
            size_t restLikeSamples = 0;
            size_t defaultLikeSamples = 0;

            debugLog << "  [DetectAnimationBindingMode] Probing animation tracks relative to rest pose:" << std::endl;
            for (const AnimationClip& clip : animations)
            {
                debugLog << "    Probing clip: " << clip.name << " with " << clip.nodeTracks.size() << " node tracks." << std::endl;
                for (const AnimationNodeTrack& track : clip.nodeTracks)
                {
                    const auto nodeIt = skeletonNodeLookup.find(track.nodeName);
                    if (nodeIt == skeletonNodeLookup.end() || nodeIt->second >= restTrs.size())
                    {
                        debugLog << "      Track node '" << track.nodeName << "' not found in skeleton." << std::endl;
                        continue;
                    }

                    const size_t skeletonNodeIndex = nodeIt->second;
                    const GltfNodeTrs& rest = restTrs[skeletonNodeIndex];
                    
                    std::array<float, 3> parentScale = { 1.0f, 1.0f, 1.0f };
                    if (skeletonNodeIndex < skeleton.size())
                    {
                        const uint16_t parentIndex = skeleton[skeletonNodeIndex].parentIndex;
                        if (parentIndex != DagorFormats::GeomNodeTreeNode::InvalidIndex && parentIndex < skeletonWorldScalesReal.size())
                        {
                            parentScale = skeletonWorldScalesReal[parentIndex];
                        }
                    }
                    const bool parentIsMirrored = (parentScale[2] < 0.0f);

                    debugLog << "      Node '" << track.nodeName << "' (skeleton index " << skeletonNodeIndex << "):" << std::endl;
                    debugLog << "        Rest pose: translation=[" << rest.translation[0] << ", " << rest.translation[1] << ", " << rest.translation[2] << "], "
                             << "rotation=[" << rest.rotation[0] << ", " << rest.rotation[1] << ", " << rest.rotation[2] << ", " << rest.rotation[3] << "], "
                             << "scale=[" << rest.scale[0] << ", " << rest.scale[1] << ", " << rest.scale[2] << "]" << std::endl;

                    if (!track.translations.empty() && !IsIdentityTranslation(rest.translation))
                    {
                        std::array<float, 3> firstTranslation =
                            ConvertAnimationTranslationForGltf(track.translations.front());
                        firstTranslation[0] *= parentScale[0];
                        firstTranslation[1] *= parentScale[1];
                        firstTranslation[2] *= parentScale[2];
                        debugLog << "        Track translation (front): raw=[" 
                                 << track.translations.front()[0] << ", " << track.translations.front()[1] << ", " << track.translations.front()[2] << "], "
                                 << "converted=[" << firstTranslation[0] << ", " << firstTranslation[1] << ", " << firstTranslation[2] << "]" << std::endl;
                        if (NearlyEqualVec3(firstTranslation, rest.translation))
                        {
                            ++restLikeSamples;
                        }
                        else if (IsIdentityTranslation(firstTranslation))
                        {
                            ++defaultLikeSamples;
                        }
                    }

                    if (!track.rotations.empty() && !IsIdentityRotation(rest.rotation))
                    {
                        const std::array<float, 4> firstRotation =
                            ConvertAnimationRotationForGltf(track.rotations.front(), parentIsMirrored);
                        debugLog << "        Track rotation (front): raw=[" 
                                 << track.rotations.front()[0] << ", " << track.rotations.front()[1] << ", " << track.rotations.front()[2] << ", " << track.rotations.front()[3] << "], "
                                 << "converted=[" << firstRotation[0] << ", " << firstRotation[1] << ", " << firstRotation[2] << ", " << firstRotation[3] << "]" << std::endl;
                        if (NearlyEqualQuat(firstRotation, rest.rotation))
                        {
                            ++restLikeSamples;
                        }
                        else if (IsIdentityRotation(firstRotation))
                        {
                            ++defaultLikeSamples;
                        }
                    }

                    if (!track.scales.empty() && !IsIdentityScale(rest.scale))
                    {
                        debugLog << "        Track scale (front): raw=[" 
                                 << track.scales.front()[0] << ", " << track.scales.front()[1] << ", " << track.scales.front()[2] << "]" << std::endl;
                        if (NearlyEqualVec3(track.scales.front(), rest.scale))
                        {
                            ++restLikeSamples;
                        }
                        else if (IsIdentityScale(track.scales.front()))
                        {
                            ++defaultLikeSamples;
                        }
                    }
                }
            }

            debugLog << "  Animation binding mode probe summary: restLikeSamples=" << restLikeSamples
                     << ", defaultLikeSamples=" << defaultLikeSamples << std::endl;

            if (defaultLikeSamples >= 6 && defaultLikeSamples > (restLikeSamples * 2))
            {
                debugLog << "  Animation binding mode decision: bind-relative delta wrapper." << std::endl;
                return AnimationBindingMode::BindRelativeDelta;
            }

            debugLog << "  Animation binding mode decision: absolute local transforms." << std::endl;
            return AnimationBindingMode::AbsoluteLocal;
        }

        template <typename TValue, typename TCompare>
        bool CurveIsConstant(
            const std::vector<TValue>& values,
            const TValue& reference,
            TCompare&& compare)
        {
            return std::all_of(values.begin(), values.end(), [&](const TValue& value) {
                return compare(value, reference);
            });
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
            TConvert convert,
            bool skipCurve = false)
        {
            if (skipCurve || !HasValidAnimationTimes(times) || values.size() != times.size())
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
            const std::vector<size_t>& skeletonTargetNodeIndices,
            const std::vector<GltfNodeTrs>& skeletonRestTrs,
            const std::vector<DagorFormats::GeomNodeTreeNode>& skeleton,
            const std::vector<std::array<float, 3>>& skeletonWorldScalesReal,
            bool emitSkeletonNodes,
            bool useBindWrapperAnimationNodes,
            bool bypassAnimationRebase,
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

            debugLog << "  [PrepareAnimationSections] Preparing skinned animation sections for " << animations.size() << " clips:" << std::endl;
            size_t nextBufferView = bufferViewBase;
            for (const AnimationClip& clip : animations)
            {
                GlbAnimationClipInfo clipInfo;
                clipInfo.name = clip.name.empty() ? "animation" : clip.name;
                debugLog << "    Clip: " << clipInfo.name << " nodeTracks: " << clip.nodeTracks.size() << std::endl;

                for (const AnimationNodeTrack& track : clip.nodeTracks)
                {
                    const auto nodeIt = skeletonNodeLookup.find(track.nodeName);
                    if (nodeIt == skeletonNodeLookup.end())
                    {
                        debugLog << "      Animation clip " << clipInfo.name
                                 << " skipped node '" << track.nodeName
                                 << "': no matching skeleton node." << std::endl;
                        continue;
                    }

                    const size_t skeletonNode = nodeIt->second;
                    if (skeletonNode >= emittedSkeletonNodes.size() ||
                        !emittedSkeletonNodes[skeletonNode] ||
                        skeletonNode >= skeletonTargetNodeIndices.size() ||
                        skeletonTargetNodeIndices[skeletonNode] == std::numeric_limits<size_t>::max())
                    {
                        debugLog << "      Animation clip " << clipInfo.name
                                 << " skipped node '" << track.nodeName
                                 << "': skeleton node was not emitted." << std::endl;
                        continue;
                    }

                    const size_t targetNodeIndex = skeletonTargetNodeIndices[skeletonNode];
                    const GltfNodeTrs& restTrs = skeletonRestTrs[skeletonNode];
                    const GltfNodeTrs targetRestTrs =
                        useBindWrapperAnimationNodes ? GltfNodeTrs{} : restTrs;
                    const auto compareVec3 = [](const std::array<float, 3>& left, const std::array<float, 3>& right)
                    {
                        return NearlyEqualVec3(left, right);
                    };
                    const auto compareQuat = [](const std::array<float, 4>& left, const std::array<float, 4>& right)
                    {
                        return NearlyEqualQuat(left, right);
                    };

                    std::array<float, 3> parentScale = { 1.0f, 1.0f, 1.0f };
                    if (skeletonNode < skeleton.size())
                    {
                        const uint16_t parentIndex = skeleton[skeletonNode].parentIndex;
                        if (parentIndex != DagorFormats::GeomNodeTreeNode::InvalidIndex && parentIndex < skeletonWorldScalesReal.size())
                        {
                            parentScale = skeletonWorldScalesReal[parentIndex];
                        }
                    }

                    const PreparedAnimationNodeTrack preparedTrack =
                        PrepareAnimationTrackForSampling(
                            clip,
                            track,
                            restTrs,
                            targetRestTrs,
                            debugLog,
                            parentScale,
                            !useBindWrapperAnimationNodes,
                            bypassAnimationRebase);
                    const std::vector<std::array<float, 3>>& preparedTranslations = preparedTrack.translations;
                    const std::vector<std::array<float, 4>>& preparedRotations = preparedTrack.rotations;
                    const std::vector<std::array<float, 3>>& preparedScales = preparedTrack.scales;

                    const bool skipTranslationCurve =
                        !preparedTranslations.empty() &&
                        CurveIsConstant(preparedTranslations, std::array<float, 3>{ 0.0f, 0.0f, 0.0f }, compareVec3) &&
                        IsIdentityTranslation(targetRestTrs.translation);
                    const bool skipRotationCurve =
                        !preparedRotations.empty() &&
                        CurveIsConstant(preparedRotations, std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f }, compareQuat) &&
                        IsIdentityRotation(targetRestTrs.rotation);
                    const bool skipScaleCurve =
                        !preparedScales.empty() &&
                        CurveIsConstant(preparedScales, std::array<float, 3>{ 1.0f, 1.0f, 1.0f }, compareVec3) &&
                        IsIdentityScale(targetRestTrs.scale);

                    const bool redundantTranslationCurve =
                        !preparedTranslations.empty() &&
                        CurveIsConstant(preparedTranslations, targetRestTrs.translation, compareVec3);
                    const bool redundantRotationCurve =
                        !preparedRotations.empty() &&
                        CurveIsConstant(preparedRotations, targetRestTrs.rotation, compareQuat);
                    const bool redundantScaleCurve =
                        !preparedScales.empty() &&
                        CurveIsConstant(preparedScales, targetRestTrs.scale, compareVec3);

                    debugLog << "      Curve status for '" << track.nodeName << "' -> targetNode " << targetNodeIndex << ":" << std::endl;
                    debugLog << "        translation: skip=" << skipTranslationCurve << " (redundant=" << redundantTranslationCurve << ")" << std::endl;
                    debugLog << "        rotation:    skip=" << skipRotationCurve    << " (redundant=" << redundantRotationCurve << ")" << std::endl;
                    debugLog << "        scale:       skip=" << skipScaleCurve       << " (redundant=" << redundantScaleCurve << ") [hardcoded skip = true]" << std::endl;

                    if (skipTranslationCurve || redundantTranslationCurve)
                    {
                        debugLog << "      Animation clip " << clipInfo.name
                                 << " skipped translation curve for node '" << track.nodeName
                                 << "' because it does not move away from the bind pose." << std::endl;
                    }
                    AppendAnimationCurve(
                        binBuffer,
                        clipInfo,
                        track.nodeName,
                        targetNodeIndex,
                        "translation",
                        "VEC3",
                        nextBufferView,
                        preparedTrack.times,
                        preparedTranslations,
                        [](const std::array<float, 3>& value) { return value; },
                        skipTranslationCurve || redundantTranslationCurve);

                    if (skipRotationCurve || redundantRotationCurve)
                    {
                        debugLog << "      Animation clip " << clipInfo.name
                                 << " skipped rotation curve for node '" << track.nodeName
                                 << "' because it does not move away from the bind pose." << std::endl;
                    }
                    AppendAnimationCurve(
                        binBuffer,
                        clipInfo,
                        track.nodeName,
                        targetNodeIndex,
                        "rotation",
                        "VEC4",
                        nextBufferView,
                        preparedTrack.times,
                        preparedRotations,
                        [](const std::array<float, 4>& value) { return value; },
                        skipRotationCurve || redundantRotationCurve);

                    if (skipScaleCurve || redundantScaleCurve)
                    {
                        debugLog << "      Animation clip " << clipInfo.name
                                 << " skipped scale curve for node '" << track.nodeName
                                 << "' because it does not move away from the bind pose." << std::endl;
                    }
                    AppendAnimationCurve(
                        binBuffer,
                        clipInfo,
                        track.nodeName,
                        targetNodeIndex,
                        "scale",
                        "VEC3",
                        nextBufferView,
                        preparedTrack.times,
                        preparedScales,
                        [](const std::array<float, 3>& value) { return value; },
                        true);
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

        GlbAnimationSections PrepareRigidNodeAnimationSections(
            const std::vector<AnimationClip>& animations,
            const std::vector<DagorFormats::GeomNodeTreeNode>& skeleton,
            const std::vector<GltfNodeTrs>& skeletonLocalTrsGltf,
            const std::vector<std::array<float, 16>>& skeletonWorldMatricesRealRowMajor,
            const std::vector<std::array<float, 3>>& skeletonWorldScalesReal,
            const std::vector<RigidAnimationBindingInfo>& rigidBindings,
            size_t bufferViewBase,
            std::vector<uint8_t>& binBuffer,
            std::ostream& debugLog)
        {
            GlbAnimationSections sections;
            if (animations.empty() || skeleton.empty() || rigidBindings.empty())
            {
                return sections;
            }

            debugLog << "  [PrepareRigidNodeAnimationSections] Preparing rigid animation sections for " << animations.size() << " clips:" << std::endl;
            size_t nextBufferView = bufferViewBase;
            for (const AnimationClip& clip : animations)
            {
                GlbAnimationClipInfo clipInfo;
                clipInfo.name = clip.name.empty() ? "animation" : clip.name;
                debugLog << "    Clip: " << clipInfo.name << " nodeTracks: " << clip.nodeTracks.size() << std::endl;

                std::vector<const AnimationNodeTrack*> tracksByNode(skeleton.size(), nullptr);
                std::vector<PreparedAnimationNodeTrack> preparedTracks(skeleton.size());
                std::vector<bool> hasPreparedTrack(skeleton.size(), false);
                for (const AnimationNodeTrack& track : clip.nodeTracks)
                {
                    const auto nodeIt = std::find_if(
                        skeleton.begin(),
                        skeleton.end(),
                        [&](const DagorFormats::GeomNodeTreeNode& node)
                        {
                            return node.name == track.nodeName;
                        });
                    if (nodeIt == skeleton.end())
                    {
                        debugLog << "      Track node '" << track.nodeName << "' not found in skeleton; skipped in rigid prep." << std::endl;
                        continue;
                    }

                    const size_t skeletonNodeIndex = static_cast<size_t>(nodeIt - skeleton.begin());
                    if (skeletonNodeIndex >= skeletonLocalTrsGltf.size() ||
                        !HasValidAnimationTimes(track.times))
                    {
                        debugLog << "      Track node '" << track.nodeName << "' has invalid times or invalid skeleton index." << std::endl;
                        continue;
                    }

                    std::array<float, 3> parentScale = { 1.0f, 1.0f, 1.0f };
                    if (skeletonNodeIndex < skeleton.size())
                    {
                        const uint16_t parentIndex = skeleton[skeletonNodeIndex].parentIndex;
                        if (parentIndex != DagorFormats::GeomNodeTreeNode::InvalidIndex && parentIndex < skeletonWorldScalesReal.size())
                        {
                            parentScale = skeletonWorldScalesReal[parentIndex];
                        }
                    }

                    tracksByNode[skeletonNodeIndex] = &track;
                    preparedTracks[skeletonNodeIndex] = PrepareAnimationTrackForSampling(
                        clip,
                        track,
                        skeletonLocalTrsGltf[skeletonNodeIndex],
                        skeletonLocalTrsGltf[skeletonNodeIndex],
                        debugLog,
                        parentScale);
                    hasPreparedTrack[skeletonNodeIndex] = true;
                }

                for (const RigidAnimationBindingInfo& binding : rigidBindings)
                {
                    if (binding.parentJoint >= skeleton.size())
                    {
                        continue;
                    }

                    const std::array<float, 16> restWorldRowMajor = skeletonWorldMatricesRealRowMajor[binding.parentJoint];

                    std::array<float, 16> inverseRestWorld = {};
                    if (!InvertAffineRowMajor(restWorldRowMajor, inverseRestWorld))
                    {
                        debugLog << "  Rigid animation clip " << clipInfo.name
                                 << " skipped mesh node '" << binding.nodeName
                                 << "': parent rest world matrix is non-invertible." << std::endl;
                        continue;
                    }

                    std::vector<size_t> ancestorChain;
                    size_t nodeIndex = binding.parentJoint;
                    size_t guard = 0;
                    while (nodeIndex != static_cast<size_t>(DagorFormats::GeomNodeTreeNode::InvalidIndex) &&
                           nodeIndex < skeleton.size() &&
                           guard++ < skeleton.size())
                    {
                        ancestorChain.push_back(nodeIndex);
                        nodeIndex = skeleton[nodeIndex].parentIndex;
                    }
                    std::reverse(ancestorChain.begin(), ancestorChain.end());

                    std::vector<float> sampleTimes;
                    for (size_t skeletonNodeIndex : ancestorChain)
                    {
                        if (!hasPreparedTrack[skeletonNodeIndex])
                        {
                            continue;
                        }

                        sampleTimes.insert(
                            sampleTimes.end(),
                            preparedTracks[skeletonNodeIndex].times.begin(),
                            preparedTracks[skeletonNodeIndex].times.end());
                    }

                    if (sampleTimes.empty())
                    {
                        continue;
                    }

                    std::sort(sampleTimes.begin(), sampleTimes.end());
                    sampleTimes.erase(
                        std::unique(
                            sampleTimes.begin(),
                            sampleTimes.end(),
                            [](float left, float right)
                            {
                                return std::fabs(left - right) <= 1e-6f;
                            }),
                        sampleTimes.end());

                    std::vector<std::array<float, 3>> translations;
                    std::vector<std::array<float, 4>> rotations;
                    std::vector<std::array<float, 3>> scales;
                    translations.reserve(sampleTimes.size());
                    rotations.reserve(sampleTimes.size());
                    scales.reserve(sampleTimes.size());

                    for (float time : sampleTimes)
                    {
                        std::array<float, 16> animatedWorld = {
                            1.0f, 0.0f, 0.0f, 0.0f,
                            0.0f, 1.0f, 0.0f, 0.0f,
                            0.0f, 0.0f, 1.0f, 0.0f,
                            0.0f, 0.0f, 0.0f, 1.0f
                        };

                        for (size_t skeletonNodeIndex : ancestorChain)
                        {
                            const PreparedAnimationNodeTrack* preparedTrack =
                                hasPreparedTrack[skeletonNodeIndex]
                                    ? &preparedTracks[skeletonNodeIndex]
                                    : nullptr;
                            const GltfNodeTrs localTrs = EvaluatePreparedAnimationTrackAtTime(
                                preparedTrack,
                                skeletonLocalTrsGltf[skeletonNodeIndex],
                                time);
                            animatedWorld = MultiplyAffineRowMajor(
                                animatedWorld,
                                ComposeRowMajorMatrixFromTrs(localTrs));
                        }

                        const std::array<float, 16> rigidDeltaRowMajor =
                            MultiplyAffineRowMajor(animatedWorld, inverseRestWorld);
                        const std::array<float, 16> gltfDelta = TransposeMatrix(rigidDeltaRowMajor);
                        const GltfNodeTrs deltaTrs = DecomposeGltfMatrix(gltfDelta);
                        translations.push_back(deltaTrs.translation);
                        rotations.push_back(deltaTrs.rotation);
                        scales.push_back(deltaTrs.scale);
                    }

                    const auto compareVec3 = [](const std::array<float, 3>& left, const std::array<float, 3>& right)
                    {
                        return NearlyEqualVec3(left, right);
                    };
                    const auto compareQuat = [](const std::array<float, 4>& left, const std::array<float, 4>& right)
                    {
                        return NearlyEqualQuat(left, right);
                    };

                    const bool redundantTranslationCurve =
                        !translations.empty() &&
                        CurveIsConstant(translations, std::array<float, 3>{ 0.0f, 0.0f, 0.0f }, compareVec3);
                    const bool redundantRotationCurve =
                        !rotations.empty() &&
                        CurveIsConstant(rotations, std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f }, compareQuat);
                    const bool redundantScaleCurve =
                        !scales.empty() &&
                        CurveIsConstant(scales, std::array<float, 3>{ 1.0f, 1.0f, 1.0f }, compareVec3);

                    AppendAnimationCurve(
                        binBuffer,
                        clipInfo,
                        binding.nodeName,
                        binding.targetNodeIndex,
                        "translation",
                        "VEC3",
                        nextBufferView,
                        sampleTimes,
                        translations,
                        [](const std::array<float, 3>& value) { return value; },
                        redundantTranslationCurve);
                    AppendAnimationCurve(
                        binBuffer,
                        clipInfo,
                        binding.nodeName,
                        binding.targetNodeIndex,
                        "rotation",
                        "VEC4",
                        nextBufferView,
                        sampleTimes,
                        rotations,
                        [](const std::array<float, 4>& value) { return value; },
                        redundantRotationCurve);
                    AppendAnimationCurve(
                        binBuffer,
                        clipInfo,
                        binding.nodeName,
                        binding.targetNodeIndex,
                        "scale",
                        "VEC3",
                        nextBufferView,
                        sampleTimes,
                        scales,
                        [](const std::array<float, 3>& value) { return value; },
                        redundantScaleCurve);
                }

                if (!clipInfo.curves.empty())
                {
                    sections.bufferViewCount += clipInfo.curves.size() * 2;
                    sections.clips.push_back(std::move(clipInfo));
                }
                else
                {
                    debugLog << "  Direct-node animation clip " << clipInfo.name
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

        const MaterialSelection materialSelection = SelectUsedMaterials(lods, materials);
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

        struct LodBinInfo
        {
            size_t vertByteOffset;
            size_t vertByteLength;
            size_t indexByteOffset;
            size_t indexByteLength;
            size_t tangentByteOffset = 0;
            size_t tangentByteLength = 0;
            bool hasTangent = false;

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
        
        for (size_t meshIdx = 0; meshIdx < lods.size(); ++meshIdx)
        {
            const auto& mesh = lods[meshIdx];
            LodBinInfo info;

            info.vertByteOffset = binBuffer.size();
            info.vertByteLength = mesh.vertices.size() * sizeof(PackedStaticVertex);

            std::vector<uint32_t> meshIndices = mesh.indices;

            std::vector<PackedStaticVertex> packedVertices;
            if (!mesh.vertices.empty())
            {
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

                    info.minX = std::min(info.minX, packed.x);
                    info.minY = std::min(info.minY, packed.y);
                    info.minZ = std::min(info.minZ, packed.z);
                    info.maxX = std::max(info.maxX, packed.x);
                    info.maxY = std::max(info.maxY, packed.y);
                    info.maxZ = std::max(info.maxZ, packed.z);
                }

                RecomputePackedNormals(packedVertices, meshIndices);
                const uint8_t* vertBytes = reinterpret_cast<const uint8_t*>(packedVertices.data());
                binBuffer.insert(binBuffer.end(), vertBytes, vertBytes + info.vertByteLength);
            }
            else
            {
                info.minX = info.minY = info.minZ = 0.0f;
                info.maxX = info.maxY = info.maxZ = 0.0f;
            }

            while (binBuffer.size() % 4 != 0)
            {
                binBuffer.push_back(0);
            }

            info.indexByteOffset = binBuffer.size();
            info.indexByteLength = mesh.indices.size() * sizeof(uint32_t);

            if (!meshIndices.empty())
            {
                const uint8_t* indexBytes = reinterpret_cast<const uint8_t*>(meshIndices.data());
                binBuffer.insert(binBuffer.end(), indexBytes, indexBytes + info.indexByteLength);
            }

            if (meshWritesTexcoords[meshIdx])
            {
                std::vector<Tangent4> computedTangents = ComputeTangents(packedVertices, meshIndices);
                if (!computedTangents.empty())
                {
                    info.hasTangent = true;
                    while (binBuffer.size() % 4 != 0)
                    {
                        binBuffer.push_back(0);
                    }
                    info.tangentByteOffset = binBuffer.size();
                    info.tangentByteLength = computedTangents.size() * sizeof(Tangent4);
                    const uint8_t* tangentBytes = reinterpret_cast<const uint8_t*>(computedTangents.data());
                    binBuffer.insert(binBuffer.end(), tangentBytes, tangentBytes + info.tangentByteLength);
                }
            }

            binInfos.push_back(info);

            while (binBuffer.size() % 4 != 0)
            {
                binBuffer.push_back(0);
            }
        }

        struct StaticAccessorInfo
        {
            size_t position = 0;
            size_t normal = 0;
            size_t uv = 0;
            size_t tangent = 0;
            bool hasTangent = false;
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
            if (binInfos[i].hasTangent)
            {
                accessorInfos[i].hasTangent = true;
                accessorInfos[i].tangent = currentAccessor++;
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

        std::vector<size_t> tangentBufferViews(lods.size(), 0);
        size_t currentBufferView = lods.size() * 2;
        for (size_t i = 0; i < lods.size(); ++i)
        {
            if (binInfos[i].hasTangent)
            {
                tangentBufferViews[i] = currentBufferView++;
            }
        }
        const size_t imageBufferViewBase = currentBufferView;
        const GlbTextureSections textureSections = PrepareMaterialTextures(materialSelection.materials, options, binBuffer, debugLog);
        
        // Generate JSON
        std::stringstream json;
        json << "{\n";
        json << "  \"asset\": {\n";
        json << "    \"version\": \"2.0\",\n";
        json << "    \"generator\": \"DagorAssetsExtractor\"\n";
        json << "  },\n";
        WriteMaterialExtensionsUsed(json, textureSections);

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
                    json << "            \"TEXCOORD_0\": " << accessorInfos[i].uv;
                    if (accessorInfos[i].hasTangent)
                    {
                        json << ",\n";
                        json << "            \"TANGENT\": " << accessorInfos[i].tangent << "\n";
                    }
                    else
                    {
                        json << "\n";
                    }
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

        WriteMaterialTextureSections(json, materialSelection.materials, textureSections, imageBufferViewBase);

        // BufferViews definition
        json << "  \"bufferViews\": [\n";
        bool firstBufferView = true;
        const auto writeBufferViewSeparator = [&]() {
            if (!firstBufferView) json << ",\n";
            firstBufferView = false;
        };

        // Vertex bufferViews first
        for (size_t i = 0; i < lods.size(); ++i)
        {
            writeBufferViewSeparator();
            json << "    {\n";
            json << "      \"buffer\": 0,\n";
            json << "      \"byteOffset\": " << binInfos[i].vertByteOffset << ",\n";
            json << "      \"byteLength\": " << binInfos[i].vertByteLength << ",\n";
            json << "      \"byteStride\": " << sizeof(PackedStaticVertex) << ",\n";
            json << "      \"target\": 34962\n"; // ARRAY_BUFFER
            json << "    }";
        }
        // Index bufferViews next
        for (size_t i = 0; i < lods.size(); ++i)
        {
            writeBufferViewSeparator();
            json << "    {\n";
            json << "      \"buffer\": 0,\n";
            json << "      \"byteOffset\": " << binInfos[i].indexByteOffset << ",\n";
            json << "      \"byteLength\": " << binInfos[i].indexByteLength << ",\n";
            json << "      \"target\": 34963\n"; // ELEMENT_ARRAY_BUFFER
            json << "    }";
        }
        // Tangent bufferViews next
        for (size_t i = 0; i < lods.size(); ++i)
        {
            if (binInfos[i].hasTangent)
            {
                writeBufferViewSeparator();
                json << "    {\n";
                json << "      \"buffer\": 0,\n";
                json << "      \"byteOffset\": " << binInfos[i].tangentByteOffset << ",\n";
                json << "      \"byteLength\": " << binInfos[i].tangentByteLength << ",\n";
                json << "      \"target\": 34962\n"; // ARRAY_BUFFER
                json << "    }";
            }
        }
        if (textureSections.embeddedImageCount > 0)
        {
            writeBufferViewSeparator();
            WriteEmbeddedImageBufferViews(json, textureSections);
        }
        json << "\n  ],\n";

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

            if (binInfos[i].hasTangent)
            {
                writeAccessorSeparator();
                json << "    { \"bufferView\": " << tangentBufferViews[i]
                     << ", \"byteOffset\": 0, \"componentType\": 5126, \"count\": " << mesh.vertices.size()
                     << ", \"type\": \"VEC4\" }";
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
        json << "\n  ],\n";

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
        const auto emitDiagnostic = [&](const std::string& message)
        {
            debugLog << "  " << message << std::endl;
            if (options.diagnosticLog)
            {
                options.diagnosticLog(message);
            }
        };

        emitDiagnostic(
            "GLB input summary: meshes=" + std::to_string(lods_in.size()) +
            ", skeletonNodes=" + std::to_string(skeleton.size()) +
            ", includeSkeletonRig=" + std::string(includeSkeletonRig ? "true" : "false") +
            ", animationClips=" + std::to_string(animations.size()) +
            ", materialSlots=" + std::to_string(materials.size()));

        std::vector<DagorFormats::DynModelMesh> lods;
        for (size_t lodIndex = 0; lodIndex < lods_in.size(); ++lodIndex)
        {
            const auto& lod_in = lods_in[lodIndex];
            const MeshAuditStats inputAudit =
                AuditMeshGeometry(lod_in.vertices, lod_in.indices, lod_in.primitives, materials.size());
            emitDiagnostic(
                "mesh audit input[" + std::to_string(lodIndex) + "] name='" + lod_in.name +
                "', parent='" + lod_in.parentNodeName +
                "', skinned=" + std::string(lod_in.useSkinning ? "true" : "false") +
                ", vertices=" + std::to_string(lod_in.vertices.size()) +
                ", indices=" + std::to_string(lod_in.indices.size()) +
                ", primitives=" + std::to_string(lod_in.primitives.size()) +
                ", validPrimitives=" + std::to_string(inputAudit.validPrimitives) +
                ", invalidRangePrimitives=" + std::to_string(inputAudit.invalidRangePrimitives) +
                ", invalidIndexPrimitives=" + std::to_string(inputAudit.invalidIndexPrimitives) +
                ", degenerateTriangles=" + std::to_string(inputAudit.degenerateTriangles) +
                ", outOfRangeMaterials=" + std::to_string(inputAudit.outOfRangeMaterials) +
                ", nonFinitePositions=" + std::to_string(inputAudit.nonFinitePositions) +
                ", nonFiniteNormals=" + std::to_string(inputAudit.nonFiniteNormals) +
                ", zeroNormals=" + std::to_string(inputAudit.zeroNormals) +
                ", materialSlots=" + FormatMaterialSlots(inputAudit.materialSlots) +
                ", bounds=" + FormatBounds(inputAudit));

            if (lod_in.vertices.empty() || lod_in.indices.empty())
            {
                emitDiagnostic(
                    "mesh skipped before GLB write: name='" + lod_in.name +
                    "', reason=empty vertices or indices, vertices=" + std::to_string(lod_in.vertices.size()) +
                    ", indices=" + std::to_string(lod_in.indices.size()));
                continue;
            }

            DagorFormats::DynModelMesh validLod = lod_in;
            const size_t primitivesBeforeValidation = validLod.primitives.size();
            validLod.primitives.erase(
                std::remove_if(validLod.primitives.begin(), validLod.primitives.end(),
                    [&](const DagorFormats::RendInstPrimitive& p)
                    {
                        return !PrimitiveHasValidIndices(validLod.indices, validLod.vertices.size(), p);
                    }),
                validLod.primitives.end());
            const size_t invalidPrimitivesRemoved = primitivesBeforeValidation - validLod.primitives.size();
            if (invalidPrimitivesRemoved > 0)
            {
                emitDiagnostic(
                    "mesh primitive validation removed primitives: name='" + lod_in.name +
                    "', removed=" + std::to_string(invalidPrimitivesRemoved) +
                    ", remaining=" + std::to_string(validLod.primitives.size()));
            }

            if (validLod.primitives.empty())
            {
                DagorFormats::RendInstPrimitive defaultPrimitive{
                    0,
                    0,
                    static_cast<uint32_t>(validLod.indices.size() - (validLod.indices.size() % 3))
                };
                if (!PrimitiveHasValidIndices(validLod.indices, validLod.vertices.size(), defaultPrimitive))
                {
                    emitDiagnostic(
                        "mesh skipped before GLB write: name='" + lod_in.name +
                        "', reason=no valid primitive/index ranges");
                    continue;
                }
                emitDiagnostic(
                    "mesh had no valid primitives; creating bounded default primitive: name='" + lod_in.name +
                    "', indexCount=" + std::to_string(defaultPrimitive.indexCount));
                validLod.primitives.push_back(defaultPrimitive);
            }

            const size_t removedDegenerateTriangles = FilterDegenerateTriangles(validLod);
            if (removedDegenerateTriangles > 0)
            {
                emitDiagnostic(
                    "mesh degenerate filter removed triangles: name='" + validLod.name +
                    "', removedTriangles=" + std::to_string(removedDegenerateTriangles));
            }
            if (validLod.indices.empty() || validLod.primitives.empty())
            {
                emitDiagnostic(
                    "mesh skipped before GLB write: name='" + lod_in.name +
                    "', reason=all triangles were degenerate");
                continue;
            }

            const size_t repairedNormals = RepairNormals(validLod.vertices, validLod.indices);
            if (repairedNormals > 0)
            {
                emitDiagnostic(
                    "mesh normal repair changed vertices: name='" + validLod.name +
                    "', repairedNormals=" + std::to_string(repairedNormals));
            }

            const MeshAuditStats outputAudit =
                AuditMeshGeometry(validLod.vertices, validLod.indices, validLod.primitives, materials.size());
            emitDiagnostic(
                "mesh audit export[" + std::to_string(lods.size()) + "] name='" + validLod.name +
                "', vertices=" + std::to_string(validLod.vertices.size()) +
                ", indices=" + std::to_string(validLod.indices.size()) +
                ", primitives=" + std::to_string(validLod.primitives.size()) +
                ", validPrimitives=" + std::to_string(outputAudit.validPrimitives) +
                ", degenerateTriangles=" + std::to_string(outputAudit.degenerateTriangles) +
                ", outOfRangeMaterials=" + std::to_string(outputAudit.outOfRangeMaterials) +
                ", materialSlots=" + FormatMaterialSlots(outputAudit.materialSlots) +
                ", bounds=" + FormatBounds(outputAudit));

            lods.push_back(std::move(validLod));
        }

        if (lods.empty())
        {
            emitDiagnostic("GLB export failed: no valid meshes after filtering. inputMeshes=" + std::to_string(lods_in.size()));
            return false;
        }

        const auto makeGltfColumnMajorMatrix = [&](const std::array<float, 16>& matrix)
        {
            std::array<float, 16> gltfMatrix = TransposeMatrix(matrix);
            ConvertMatrixHandedness(gltfMatrix);
            return gltfMatrix;
        };
        const auto makeGltfRowMajorMatrix = [&](const std::array<float, 16>& matrix)
        {
            return TransposeMatrix(makeGltfColumnMajorMatrix(matrix));
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

        std::vector<std::array<float, 3>> skeletonWorldScalesReal(skeleton.size());
        std::vector<std::array<float, 16>> skeletonWorldMatricesRealRowMajor(skeleton.size());
        std::vector<std::array<float, 16>> skeletonWorldMatricesGltfRowMajor(skeleton.size());
        for (size_t i = 0; i < skeleton.size(); ++i)
        {
            std::array<float, 16> gltfWorldMatrix = makeGltfColumnMajorMatrix(skeleton[i].worldTransform);
            GltfNodeTrs worldTrs = DecomposeGltfMatrix(gltfWorldMatrix);
            skeletonWorldScalesReal[i] = worldTrs.scale;
            skeletonWorldMatricesRealRowMajor[i] = ComposeRowMajorMatrixFromTrs(worldTrs);
            worldTrs.scale = { 1.0f, 1.0f, 1.0f };
            skeletonWorldMatricesGltfRowMajor[i] = ComposeRowMajorMatrixFromTrs(worldTrs);
        }

        std::vector<GltfNodeTrs> skeletonRestTrs(skeleton.size());
        for (size_t i = 0; i < skeleton.size(); ++i)
        {
            const uint16_t parentIndex = skeleton[i].parentIndex;
            std::array<float, 16> localMatrix;
            if (parentIndex == DagorFormats::GeomNodeTreeNode::InvalidIndex || parentIndex >= skeleton.size())
            {
                localMatrix = skeletonWorldMatricesGltfRowMajor[i];
            }
            else
            {
                std::array<float, 16> invParentWorld = {};
                if (InvertAffineRowMajor(skeletonWorldMatricesGltfRowMajor[parentIndex], invParentWorld))
                {
                    localMatrix = MultiplyAffineRowMajor(invParentWorld, skeletonWorldMatricesGltfRowMajor[i]);
                }
                else
                {
                    localMatrix = skeletonWorldMatricesGltfRowMajor[i];
                }
            }

            GltfNodeTrs localTrs = DecomposeGltfMatrix(TransposeMatrix(localMatrix));
            localTrs.scale = { 1.0f, 1.0f, 1.0f };
            skeletonRestTrs[i] = localTrs;
        }

        const AnimationBindingMode animationBindingMode =
            DetectAnimationBindingMode(animations, skeletonNodeLookup, skeletonRestTrs, skeleton, skeletonWorldScalesReal, debugLog);
        const bool useBindWrapperAnimationNodes =
            emitSkeletonNodes &&
            !animations.empty() &&
            animationBindingMode == AnimationBindingMode::BindRelativeDelta;

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

        const auto hasExplicitSkinBinding = [](const DagorFormats::DynModelMesh& mesh)
        {
            return !mesh.skinJointNodeNames.empty() &&
                mesh.skinJointNodeNames.size() == mesh.skinInverseBindTransforms.size();
        };

        struct DynExportMesh
        {
            DagorFormats::DynModelMesh mesh;
            bool skinned = false;
            bool explicitSkin = false;
            bool bakeBindPoseSkin = false;
            bool bakeRigidTransform = false;
            bool usesSkeletonPlacement = false;
            bool animateAsRigidNode = false;
            uint16_t parentJoint = 0;
            bool hasNodeMatrix = false;
            std::array<float, 16> nodeMatrix = {};
            std::vector<uint16_t> skinJoints;
            std::vector<std::array<float, 16>> skinInverseBindMatrices;
        };

        std::vector<DynExportMesh> exportMeshes;
        exportMeshes.reserve(lods.size());
        for (const auto& mesh : lods)
        {
            DynExportMesh exportMesh;
            exportMesh.mesh = mesh;
            const uint16_t jointIndex = resolveJoint(mesh);
            const bool hasPlaceholderSkinWeights = mesh.useSkinning && hasOnlyRootWeights(mesh);
            const bool hasExplicitSkin = mesh.useSkinning && hasExplicitSkinBinding(mesh);

            if (hasExplicitSkin && hasPlacementSkeleton)
            {
                bool explicitSkinValid = true;
                exportMesh.skinJoints.reserve(mesh.skinJointNodeNames.size());
                exportMesh.skinInverseBindMatrices = mesh.skinInverseBindTransforms;
                for (const std::string& jointName : mesh.skinJointNodeNames)
                {
                    const auto jointIt = skeletonNodeLookup.find(jointName);
                    if (jointIt == skeletonNodeLookup.end() ||
                        jointIt->second > std::numeric_limits<uint16_t>::max())
                    {
                        explicitSkinValid = false;
                        debugLog << "  DynModel mesh " << mesh.name
                                 << " could not resolve explicit skin joint '"
                                 << jointName << "' to the placement skeleton." << std::endl;
                        break;
                    }
                    exportMesh.skinJoints.push_back(static_cast<uint16_t>(jointIt->second));
                }

                if (explicitSkinValid && !hasPlaceholderSkinWeights)
                {
                    if (emitSkeletonNodes)
                    {
                        exportMesh.skinned = true;
                        exportMesh.explicitSkin = true;
                    }
                    else
                    {
                        exportMesh.bakeBindPoseSkin = true;
                    }
                }
                else if (!explicitSkinValid)
                {
                    exportMesh.skinJoints.clear();
                    exportMesh.skinInverseBindMatrices.clear();
                }
            }

            if (!exportMesh.explicitSkin &&
                emitSkeletonNodes &&
                mesh.useSkinning &&
                (!hasPlaceholderSkinWeights || mesh.name == "skeleton_dummy"))
            {
                exportMesh.skinned = true;
            }
            else if (emitSkeletonNodes && jointIndex != kNoJoint)
            {
                exportMesh.skinned = true;
                exportMesh.bakeRigidTransform = true;
                exportMesh.usesSkeletonPlacement = true;
                exportMesh.animateAsRigidNode = false;
                exportMesh.parentJoint = jointIndex;
            }
            else if (!emitSkeletonNodes && jointIndex != kNoJoint && (!mesh.useSkinning || hasPlaceholderSkinWeights))
            {
                exportMesh.bakeRigidTransform = true;
                exportMesh.parentJoint = jointIndex;
            }

            exportMeshes.push_back(std::move(exportMesh));
        }

        if (exportMeshes.empty())
        {
            emitDiagnostic("GLB export failed: no exportable meshes after skeleton layout preparation.");
            return false;
        }

        size_t skinnedMeshCount = 0;
        size_t bakedRigidMeshCount = 0;
        size_t bakedBindPoseMeshCount = 0;
        size_t skeletonPlacementMeshCount = 0;
        for (size_t meshIndex = 0; meshIndex < exportMeshes.size(); ++meshIndex)
        {
            const DynExportMesh& exportMesh = exportMeshes[meshIndex];
            if (exportMesh.skinned)
            {
                ++skinnedMeshCount;
            }
            if (exportMesh.bakeRigidTransform)
            {
                ++bakedRigidMeshCount;
            }
            if (exportMesh.bakeBindPoseSkin)
            {
                ++bakedBindPoseMeshCount;
            }
            if (exportMesh.usesSkeletonPlacement)
            {
                ++skeletonPlacementMeshCount;
            }

            emitDiagnostic(
                "mesh layout[" + std::to_string(meshIndex) + "] name='" + exportMesh.mesh.name +
                "', skinned=" + std::string(exportMesh.skinned ? "true" : "false") +
                ", explicitSkin=" + std::string(exportMesh.explicitSkin ? "true" : "false") +
                ", bakeBindPoseSkin=" + std::string(exportMesh.bakeBindPoseSkin ? "true" : "false") +
                ", bakeRigidTransform=" + std::string(exportMesh.bakeRigidTransform ? "true" : "false") +
                ", usesSkeletonPlacement=" + std::string(exportMesh.usesSkeletonPlacement ? "true" : "false") +
                ", parentJoint=" + (exportMesh.parentJoint == kNoJoint ? std::string("none") : std::to_string(exportMesh.parentJoint)) +
                ", skinJoints=" + std::to_string(exportMesh.skinJoints.size()));
        }

        emitDiagnostic(
            "mesh layout summary: exportMeshes=" + std::to_string(exportMeshes.size()) +
            ", skinned=" + std::to_string(skinnedMeshCount) +
            ", bakedRigid=" + std::to_string(bakedRigidMeshCount) +
            ", bakedBindPose=" + std::to_string(bakedBindPoseMeshCount) +
            ", skeletonPlacement=" + std::to_string(skeletonPlacementMeshCount));

        std::unordered_set<std::string> exportMeshNames;
        exportMeshNames.reserve(exportMeshes.size());
        for (const DynExportMesh& exportMesh : exportMeshes)
        {
            exportMeshNames.insert(exportMesh.mesh.name);
        }

        std::vector<std::string> skeletonExportNames(skeleton.size());
        for (size_t skeletonIndex = 0; skeletonIndex < skeleton.size(); ++skeletonIndex)
        {
            skeletonExportNames[skeletonIndex] = skeleton[skeletonIndex].name;
            if (exportMeshNames.contains(skeletonExportNames[skeletonIndex]))
            {
                skeletonExportNames[skeletonIndex] += "__node";
            }
        }

        const MaterialSelection materialSelection = SelectUsedMaterials(lods, materials);
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
                    if (!exportMesh.usesSkeletonPlacement)
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

        constexpr size_t kInvalidNodeIndex = std::numeric_limits<size_t>::max();
        std::vector<size_t> skeletonBindNodeIndices(skeleton.size(), kInvalidNodeIndex);
        std::vector<size_t> skeletonTargetNodeIndices(skeleton.size(), kInvalidNodeIndex);
        size_t emittedSkeletonNodeCount = 0;
        for (size_t i = 0; i < skeleton.size(); ++i)
        {
            if (!emittedSkeletonNodes[i])
            {
                continue;
            }

            skeletonBindNodeIndices[i] = emittedSkeletonNodeCount++;
            if (useBindWrapperAnimationNodes)
            {
                skeletonTargetNodeIndices[i] = emittedSkeletonNodeCount++;
            }
            else
            {
                skeletonTargetNodeIndices[i] = skeletonBindNodeIndices[i];
            }
        }

        const auto meshNodeIndex = [&](size_t meshIndex)
        {
            return (emitSkeletonNodes ? emittedSkeletonNodeCount : 0) + meshIndex;
        };

        struct GlbSkinInfo
        {
            std::vector<size_t> jointNodeIndices;
            std::vector<std::array<float, 16>> inverseBindMatrices;
            std::vector<uint16_t> skeletonJointIndices;
        };

        std::vector<GlbSkinInfo> skinInfos;
        std::vector<size_t> meshSkinIndices(exportMeshes.size(), kInvalidNodeIndex);
        std::vector<uint16_t> globalFallbackJointRemap(skeleton.size(), kNoJoint);
        size_t globalFallbackSkinIndex = kInvalidNodeIndex;

        if (emitSkeletonNodes)
        {
            for (size_t meshIndex = 0; meshIndex < exportMeshes.size(); ++meshIndex)
            {
                const DynExportMesh& exportMesh = exportMeshes[meshIndex];
                if (!exportMesh.skinned)
                {
                    continue;
                }

                if (exportMesh.explicitSkin)
                {
                    GlbSkinInfo skinInfo;
                    skinInfo.jointNodeIndices.reserve(exportMesh.skinJoints.size());
                    skinInfo.skeletonJointIndices.reserve(exportMesh.skinJoints.size());
                    skinInfo.inverseBindMatrices.reserve(exportMesh.skinJoints.size());
                    for (size_t jointSlot = 0; jointSlot < exportMesh.skinJoints.size(); ++jointSlot)
                    {
                        uint16_t skeletonJoint = exportMesh.skinJoints[jointSlot];
                        if (skeletonJoint >= skeletonTargetNodeIndices.size() ||
                            skeletonTargetNodeIndices[skeletonJoint] == kInvalidNodeIndex)
                        {
                            continue;
                        }
                        skinInfo.jointNodeIndices.push_back(skeletonTargetNodeIndices[skeletonJoint]);
                        skinInfo.skeletonJointIndices.push_back(skeletonJoint);

                        std::array<float, 16> invBindMatrix = {};
                        if (skeletonJoint < skeletonWorldMatricesGltfRowMajor.size() &&
                            InvertAffineRowMajor(skeletonWorldMatricesGltfRowMajor[skeletonJoint], invBindMatrix))
                        {
                            skinInfo.inverseBindMatrices.push_back(invBindMatrix);
                        }
                        else
                        {
                            std::array<float, 16> rawIbm = exportMesh.skinInverseBindMatrices[jointSlot];
                            std::array<float, 16> gltfIbm = makeGltfRowMajorMatrix(rawIbm);
                            skinInfo.inverseBindMatrices.push_back(gltfIbm);
                        }
                    }

                    if (skinInfo.jointNodeIndices.size() == skinInfo.inverseBindMatrices.size() &&
                        !skinInfo.jointNodeIndices.empty())
                    {
                        meshSkinIndices[meshIndex] = skinInfos.size();
                        skinInfos.push_back(std::move(skinInfo));
                        continue;
                    }

                    debugLog << "  DynModel mesh " << exportMesh.mesh.name
                             << " explicit skin mapping was incomplete; falling back to the legacy full-skeleton skin."
                             << std::endl;
                }

                if (globalFallbackSkinIndex == kInvalidNodeIndex)
                {
                    GlbSkinInfo skinInfo;
                    skinInfo.jointNodeIndices.reserve(emittedSkeletonNodeCount);
                    skinInfo.inverseBindMatrices.reserve(emittedSkeletonNodeCount);
                    skinInfo.skeletonJointIndices.reserve(emittedSkeletonNodeCount);
                    uint16_t jointIndex = 0;
                    for (size_t skeletonIndex = 0; skeletonIndex < skeleton.size(); ++skeletonIndex)
                    {
                        if (!emittedSkeletonNodes[skeletonIndex])
                        {
                            continue;
                        }
                        if (skeletonTargetNodeIndices[skeletonIndex] == kInvalidNodeIndex)
                        {
                            continue;
                        }
                        skinInfo.jointNodeIndices.push_back(skeletonTargetNodeIndices[skeletonIndex]);

                        std::array<float, 16> invBindMatrix = {};
                        if (skeletonIndex < skeletonWorldMatricesGltfRowMajor.size() &&
                            InvertAffineRowMajor(skeletonWorldMatricesGltfRowMajor[skeletonIndex], invBindMatrix))
                        {
                            skinInfo.inverseBindMatrices.push_back(invBindMatrix);
                        }
                        else
                        {
                            std::array<float, 16> rawIbm = skeleton[skeletonIndex].inverseBindTransform;
                            std::array<float, 16> gltfIbm = makeGltfRowMajorMatrix(rawIbm);
                            skinInfo.inverseBindMatrices.push_back(gltfIbm);
                        }

                        skinInfo.skeletonJointIndices.push_back(static_cast<uint16_t>(skeletonIndex));
                        if (skeletonIndex < globalFallbackJointRemap.size())
                        {
                            globalFallbackJointRemap[skeletonIndex] = jointIndex++;
                        }
                    }
                    globalFallbackSkinIndex = skinInfos.size();
                    skinInfos.push_back(std::move(skinInfo));
                }

                meshSkinIndices[meshIndex] = globalFallbackSkinIndex;
            }
        }

        struct LodBinInfo
        {
            size_t vertByteOffset;
            size_t vertByteLength;
            size_t indexByteOffset;
            size_t indexByteLength;
            size_t tangentByteOffset = 0;
            size_t tangentByteLength = 0;
            bool hasTangent = false;

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
            std::vector<uint32_t> meshIndices = mesh.indices;

            std::vector<PackedSkinnedVertex> packedVertices;

            if (!mesh.vertices.empty())
            {
                packedVertices.reserve(mesh.vertices.size());

                for (const auto& v : mesh.vertices)
                {
                    const auto uv = ConvertUvForGltf(v.u, v.v, v.uvTransform);
                    std::array<float, 3> positionGltf = { v.x, v.y, -v.z };
                    std::array<float, 3> normalGltf = { v.nx, v.ny, -v.nz };

                    if (exportMesh.bakeBindPoseSkin &&
                        exportMesh.skinJoints.size() == exportMesh.skinInverseBindMatrices.size())
                    {
                        std::array<float, 3> bakedPosition = { 0.0f, 0.0f, 0.0f };
                        std::array<float, 3> bakedNormal = { 0.0f, 0.0f, 0.0f };
                        float totalWeight = 0.0f;
                        for (int jointSlot = 0; jointSlot < 4; ++jointSlot)
                        {
                            const float weight = v.jointWeights[jointSlot];
                            const uint16_t skinJointIndex = v.jointIndices[jointSlot];
                            if (!std::isfinite(weight) || weight <= 1e-6f ||
                                skinJointIndex >= exportMesh.skinJoints.size())
                            {
                                continue;
                            }

                            const uint16_t skeletonJoint = exportMesh.skinJoints[skinJointIndex];
                            if (skeletonJoint >= skeleton.size())
                            {
                                continue;
                            }

                            // Convert the source bind matrix to the same glTF-space row-major convention
                            // used by the reconstructed skeleton world matrices.
                            std::array<float, 16> gltfInverseBind =
                                makeGltfRowMajorMatrix(exportMesh.skinInverseBindMatrices[skinJointIndex]);

                            const std::array<float, 16> skinMatrixGltf =
                                MultiplyAffineRowMajor(
                                    skeletonWorldMatricesRealRowMajor[skeletonJoint],
                                    gltfInverseBind);

                            const std::array<float, 3> transformedPosition =
                                TransformPointRowMajor(skinMatrixGltf, positionGltf);
                            const std::array<float, 3> transformedNormal =
                                TransformDirectionRowMajor(skinMatrixGltf, normalGltf);

                            bakedPosition[0] += transformedPosition[0] * weight;
                            bakedPosition[1] += transformedPosition[1] * weight;
                            bakedPosition[2] += transformedPosition[2] * weight;
                            bakedNormal[0] += transformedNormal[0] * weight;
                            bakedNormal[1] += transformedNormal[1] * weight;
                            bakedNormal[2] += transformedNormal[2] * weight;
                            totalWeight += weight;
                        }

                        if (totalWeight > 1e-6f)
                        {
                            const float invWeight = 1.0f / totalWeight;
                            bakedPosition[0] *= invWeight;
                            bakedPosition[1] *= invWeight;
                            bakedPosition[2] *= invWeight;
                            bakedNormal[0] *= invWeight;
                            bakedNormal[1] *= invWeight;
                            bakedNormal[2] *= invWeight;

                            positionGltf = bakedPosition;
                            normalGltf = bakedNormal;
                            NormalizeVec3(normalGltf);
                        }
                    }

                    if (exportMesh.bakeRigidTransform && exportMesh.parentJoint < skeleton.size())
                    {
                        positionGltf = TransformPointRowMajor(
                            skeletonWorldMatricesRealRowMajor[exportMesh.parentJoint],
                            positionGltf);
                        normalGltf = TransformDirectionRowMajor(
                            skeletonWorldMatricesRealRowMajor[exportMesh.parentJoint],
                            normalGltf);
                        NormalizeVec3(normalGltf);
                    }

                    PackedSkinnedVertex packed{};
                    packed.x = positionGltf[0];
                    packed.y = positionGltf[1];
                    packed.z = positionGltf[2];
                    packed.nx = normalGltf[0];
                    packed.ny = normalGltf[1];
                    packed.nz = normalGltf[2];
                    packed.u = uv[0];
                    packed.v = uv[1];
                    if (exportMesh.skinned)
                    {
                        if (exportMesh.usesSkeletonPlacement && !mesh.useSkinning)
                        {
                            uint16_t jointIndex = exportMesh.parentJoint;
                            if (jointIndex < globalFallbackJointRemap.size() &&
                                globalFallbackJointRemap[jointIndex] != kNoJoint)
                            {
                                jointIndex = globalFallbackJointRemap[jointIndex];
                            }
                            else
                            {
                                jointIndex = 0;
                            }
                            packed.jointIndices[0] = jointIndex;
                            packed.jointWeights[0] = 1.0f;
                            for (int j = 1; j < 4; ++j)
                            {
                                packed.jointIndices[j] = 0;
                                packed.jointWeights[j] = 0.0f;
                            }
                        }
                        else
                        {
                            for (int j = 0; j < 4; ++j)
                            {
                                uint16_t jointIndex = static_cast<uint16_t>(v.jointIndices[j]);
                                if (exportMesh.explicitSkin)
                                {
                                    if (jointIndex >= exportMesh.skinJoints.size())
                                    {
                                        jointIndex = 0;
                                    }
                                }
                                else if (jointIndex < globalFallbackJointRemap.size() &&
                                         globalFallbackJointRemap[jointIndex] != kNoJoint)
                                {
                                    jointIndex = globalFallbackJointRemap[jointIndex];
                                }
                                else
                                {
                                    jointIndex = 0;
                                }
                                packed.jointIndices[j] = jointIndex;
                                packed.jointWeights[j] = v.jointWeights[j];
                            }
                            NormalizeJointWeights(packed);
                        }
                    }
                    packedVertices.push_back(packed);

                    info.minX = std::min(info.minX, packed.x);
                    info.minY = std::min(info.minY, packed.y);
                    info.minZ = std::min(info.minZ, packed.z);
                    info.maxX = std::max(info.maxX, packed.x);
                    info.maxY = std::max(info.maxY, packed.y);
                    info.maxZ = std::max(info.maxZ, packed.z);
                }

                RecomputePackedNormals(packedVertices, meshIndices);
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

            if (!meshIndices.empty())
            {
                const uint8_t* indexBytes = reinterpret_cast<const uint8_t*>(meshIndices.data());
                binBuffer.insert(binBuffer.end(), indexBytes, indexBytes + info.indexByteLength);
            }

            if (meshWritesTexcoords[meshIndex] && !packedVertices.empty())
            {
                std::vector<Tangent4> computedTangents = ComputeTangents(packedVertices, meshIndices);
                if (!computedTangents.empty())
                {
                    info.hasTangent = true;
                    while (binBuffer.size() % 4 != 0)
                    {
                        binBuffer.push_back(0);
                    }
                    info.tangentByteOffset = binBuffer.size();
                    info.tangentByteLength = computedTangents.size() * sizeof(Tangent4);
                    const uint8_t* tangentBytes = reinterpret_cast<const uint8_t*>(computedTangents.data());
                    binBuffer.insert(binBuffer.end(), tangentBytes, tangentBytes + info.tangentByteLength);
                }
            }

            binInfos.push_back(info);
            emitDiagnostic(
                "mesh audit gltf[" + std::to_string(meshIndex) + "] name='" + mesh.name +
                "', vertexBytes=" + std::to_string(info.vertByteLength) +
                ", indexBytes=" + std::to_string(info.indexByteLength) +
                ", tangentBytes=" + std::to_string(info.tangentByteLength) +
                ", hasTangents=" + std::string(info.hasTangent ? "true" : "false") +
                ", bounds=[" + FormatAuditFloat(info.minX) + "," + FormatAuditFloat(info.minY) + "," + FormatAuditFloat(info.minZ) +
                "]-[" + FormatAuditFloat(info.maxX) + "," + FormatAuditFloat(info.maxY) + "," + FormatAuditFloat(info.maxZ) + "]");
            while (binBuffer.size() % 4 != 0) binBuffer.push_back(0);
        }

        struct SkinBufferInfo
        {
            size_t byteOffset = 0;
            size_t byteLength = 0;
        };
        std::vector<SkinBufferInfo> skinBufferInfos(skinInfos.size());
        for (size_t skinIndex = 0; skinIndex < skinInfos.size(); ++skinIndex)
        {
            while (binBuffer.size() % 4 != 0)
            {
                binBuffer.push_back(0);
            }

            skinBufferInfos[skinIndex].byteOffset = binBuffer.size();
            skinBufferInfos[skinIndex].byteLength =
                skinInfos[skinIndex].inverseBindMatrices.size() * 16 * sizeof(float);
            for (const auto& inverseBindMatrix : skinInfos[skinIndex].inverseBindMatrices)
            {
                std::array<float, 16> gltfInverseBind = TransposeMatrix(inverseBindMatrix);
                const uint8_t* matrixBytes = reinterpret_cast<const uint8_t*>(gltfInverseBind.data());
                binBuffer.insert(binBuffer.end(), matrixBytes, matrixBytes + (16 * sizeof(float)));
            }
        }
        while (binBuffer.size() % 4 != 0) binBuffer.push_back(0);

        const GlbTextureSections textureSections = PrepareMaterialTextures(materialSelection.materials, options, binBuffer, debugLog);

        size_t currentBufferView = exportMeshes.size() * 2;
        std::vector<size_t> skinBufferViewIndices(skinInfos.size(), 0);
        for (size_t skinIndex = 0; skinIndex < skinInfos.size(); ++skinIndex)
        {
            skinBufferViewIndices[skinIndex] = currentBufferView++;
        }

        std::vector<size_t> tangentBufferViews(exportMeshes.size(), 0);
        for (size_t i = 0; i < exportMeshes.size(); ++i)
        {
            if (binInfos[i].hasTangent)
            {
                tangentBufferViews[i] = currentBufferView++;
            }
        }

        const size_t imageBufferViewBase = currentBufferView;
        const size_t animationBufferViewBase = imageBufferViewBase + textureSections.embeddedImageCount;
        GlbAnimationSections animationSections = PrepareAnimationSections(
            animations,
            skeletonNodeLookup,
            emittedSkeletonNodes,
            skeletonTargetNodeIndices,
            skeletonRestTrs,
            skeleton,
            skeletonWorldScalesReal,
            emitSkeletonNodes,
            useBindWrapperAnimationNodes,
            options.bypassAnimationRebase,
            animationBufferViewBase,
            binBuffer,
            debugLog);
        std::vector<RigidAnimationBindingInfo> rigidAnimationBindings;
        rigidAnimationBindings.reserve(exportMeshes.size());
        for (size_t meshIndex = 0; meshIndex < exportMeshes.size(); ++meshIndex)
        {
            const DynExportMesh& exportMesh = exportMeshes[meshIndex];
            if (!exportMesh.animateAsRigidNode || exportMesh.parentJoint >= skeleton.size())
            {
                continue;
            }

            rigidAnimationBindings.push_back({
                exportMesh.mesh.name,
                meshNodeIndex(meshIndex),
                exportMesh.parentJoint
            });
        }
        GlbAnimationSections rigidAnimationSections = PrepareRigidNodeAnimationSections(
            animations,
            skeleton,
            skeletonRestTrs,
            skeletonWorldMatricesRealRowMajor,
            skeletonWorldScalesReal,
            rigidAnimationBindings,
            animationBufferViewBase + animationSections.bufferViewCount,
            binBuffer,
            debugLog);
        if (!rigidAnimationSections.clips.empty())
        {
            animationSections.bufferViewCount += rigidAnimationSections.bufferViewCount;
            for (GlbAnimationClipInfo& rigidClip : rigidAnimationSections.clips)
            {
                auto clipIt = std::find_if(
                    animationSections.clips.begin(),
                    animationSections.clips.end(),
                    [&](const GlbAnimationClipInfo& clipInfo)
                    {
                        return clipInfo.name == rigidClip.name;
                    });
                if (clipIt != animationSections.clips.end())
                {
                    clipIt->curves.insert(
                        clipIt->curves.end(),
                        std::make_move_iterator(rigidClip.curves.begin()),
                        std::make_move_iterator(rigidClip.curves.end()));
                }
                else
                {
                    animationSections.clips.push_back(std::move(rigidClip));
                }
            }
        }

        // Reindex all animation curves sequentially to match their final merged order in the JSON
        size_t nextAnimBufferView = animationBufferViewBase;
        for (GlbAnimationClipInfo& clip : animationSections.clips)
        {
            for (GlbAnimationCurveInfo& curve : clip.curves)
            {
                curve.timeBufferView = nextAnimBufferView++;
                curve.valueBufferView = nextAnimBufferView++;
            }
        }

        std::unordered_map<size_t, GltfNodeTrs> animatedNodeInitialTransforms;
        if (!useBindWrapperAnimationNodes && !animationSections.clips.empty())
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
                        skeletonRestTrs[skeletonNode]);
                    (void)inserted;
                }
            }
        }
        struct AccessorInfo
        {
            size_t position = 0;
            size_t normal = 0;
            size_t uv = 0;
            size_t tangent = 0;
            bool hasTangent = false;
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
            if (binInfos[i].hasTangent)
            {
                accessorInfos[i].hasTangent = true;
                accessorInfos[i].tangent = currentAccessor++;
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
        std::vector<size_t> skinAccessorIndices(skinInfos.size(), 0);
        for (size_t skinIndex = 0; skinIndex < skinInfos.size(); ++skinIndex)
        {
            skinAccessorIndices[skinIndex] = currentAccessor++;
        }
        AllocateAnimationAccessors(animationSections, currentAccessor);

        // Generate JSON
        std::stringstream json;
        json << "{\n";
        json << "  \"asset\": {\n";
        json << "    \"version\": \"2.0\",\n";
        json << "    \"generator\": \"DagorAssetsExtractor\"\n";
        json << "  },\n";
        WriteMaterialExtensionsUsed(json, textureSections);

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
                    json << skeletonBindNodeIndices[i];
                    firstSceneNode = false;
                }
            }
        }
        for (size_t i = 0; i < exportMeshes.size(); ++i)
        {
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
        const auto writeNodeTrs = [&](const GltfNodeTrs& trs)
        {
            json << "      \"translation\": [" << JsonFloat(trs.translation[0]) << ", "
                 << JsonFloat(trs.translation[1]) << ", " << JsonFloat(trs.translation[2]) << "],\n";
            json << "      \"rotation\": [" << JsonFloat(trs.rotation[0]) << ", "
                 << JsonFloat(trs.rotation[1]) << ", " << JsonFloat(trs.rotation[2]) << ", "
                 << JsonFloat(trs.rotation[3]) << "],\n";
            json << "      \"scale\": [" << JsonFloat(trs.scale[0]) << ", "
                 << JsonFloat(trs.scale[1]) << ", " << JsonFloat(trs.scale[2]) << "]";
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
                if (useBindWrapperAnimationNodes)
                {
                    writeNodeSeparator();
                    json << "    {\n";
                    json << "      \"name\": \"" << JsonEscape(skeletonExportNames[i] + "__bind") << "\"";
                    json << ",\n";
                    writeNodeTrs(skeletonRestTrs[i]);
                    json << ",\n";
                    json << "      \"children\": [" << skeletonTargetNodeIndices[i] << "]\n";
                    json << "    }";

                    writeNodeSeparator();
                    json << "    {\n";
                    json << "      \"name\": \"" << JsonEscape(skeletonExportNames[i]) << "\"";

                    std::vector<size_t> childNodes;
                    childNodes.reserve(node.childIndices.size());
                    for (uint16_t childIndex : node.childIndices)
                    {
                        if (childIndex < skeleton.size() && emittedSkeletonNodes[childIndex])
                        {
                            childNodes.push_back(skeletonBindNodeIndices[childIndex]);
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
                else
                {
                    writeNodeSeparator();
                    json << "    {\n";
                    json << "      \"name\": \"" << JsonEscape(skeletonExportNames[i]) << "\"";
                    json << ",\n";
                    if (const auto animatedTrsIt = animatedNodeInitialTransforms.find(i);
                        animatedTrsIt != animatedNodeInitialTransforms.end())
                    {
                        writeNodeTrs(animatedTrsIt->second);
                    }
                    else
                    {
                        writeNodeTrs(skeletonRestTrs[i]);
                    }

                    std::vector<size_t> childNodes;
                    childNodes.reserve(node.childIndices.size());
                    for (uint16_t childIndex : node.childIndices)
                    {
                        if (childIndex < skeleton.size() && emittedSkeletonNodes[childIndex])
                        {
                            childNodes.push_back(skeletonBindNodeIndices[childIndex]);
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
        }

        // Mesh nodes
        for (size_t i = 0; i < exportMeshes.size(); ++i)
        {
            writeNodeSeparator();
            json << "    {\n";
            json << "      \"name\": \"" << JsonEscape(exportMeshes[i].mesh.name) << "\",\n";
            json << "      \"mesh\": " << i;
            if (exportMeshes[i].skinned && meshSkinIndices[i] != kInvalidNodeIndex)
            {
                json << ",\n      \"skin\": " << meshSkinIndices[i];
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
            const auto findLowestCommonAncestor = [&](const std::vector<uint16_t>& skeletonJoints) -> size_t
            {
                if (skeletonJoints.empty())
                {
                    return kInvalidNodeIndex;
                }

                std::vector<size_t> ancestors;
                size_t current = skeletonJoints.front();
                size_t guard = 0;
                while (current != static_cast<size_t>(DagorFormats::GeomNodeTreeNode::InvalidIndex) &&
                       current < skeleton.size() &&
                       guard++ < skeleton.size())
                {
                    ancestors.push_back(current);
                    current = skeleton[current].parentIndex;
                }

                if (ancestors.empty())
                {
                    return kInvalidNodeIndex;
                }

                for (size_t jointIdx = 1; jointIdx < skeletonJoints.size(); ++jointIdx)
                {
                    std::vector<size_t> jointAncestors;
                    current = skeletonJoints[jointIdx];
                    guard = 0;
                    while (current != static_cast<size_t>(DagorFormats::GeomNodeTreeNode::InvalidIndex) &&
                           current < skeleton.size() &&
                           guard++ < skeleton.size())
                    {
                        jointAncestors.push_back(current);
                        current = skeleton[current].parentIndex;
                    }

                    ancestors.erase(
                        std::remove_if(
                            ancestors.begin(),
                            ancestors.end(),
                            [&](size_t candidate)
                            {
                                return std::find(jointAncestors.begin(), jointAncestors.end(), candidate) == jointAncestors.end();
                            }),
                        ancestors.end());
                    if (ancestors.empty())
                    {
                        return kInvalidNodeIndex;
                    }
                }

                return ancestors.front();
            };

            json << "  \"skins\": [\n";
            for (size_t skinIndex = 0; skinIndex < skinInfos.size(); ++skinIndex)
            {
                const GlbSkinInfo& skinInfo = skinInfos[skinIndex];
                json << "    {\n";
                const size_t skeletonRoot = findLowestCommonAncestor(skinInfo.skeletonJointIndices);
                if (skeletonRoot != kInvalidNodeIndex &&
                    skeletonRoot < skeletonTargetNodeIndices.size() &&
                    skeletonTargetNodeIndices[skeletonRoot] != kInvalidNodeIndex)
                {
                    json << "      \"skeleton\": " << skeletonTargetNodeIndices[skeletonRoot] << ",\n";
                }
                json << "      \"inverseBindMatrices\": " << skinAccessorIndices[skinIndex] << ",\n";
                json << "      \"joints\": [";
                for (size_t jointIndex = 0; jointIndex < skinInfo.jointNodeIndices.size(); ++jointIndex)
                {
                    if (jointIndex != 0)
                    {
                        json << ", ";
                    }
                    json << skinInfo.jointNodeIndices[jointIndex];
                }
                json << "]\n";
                json << "    }";
                if (skinIndex + 1 < skinInfos.size())
                {
                    json << ",";
                }
                json << "\n";
            }
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
                    if (accessorInfos[i].hasTangent)
                    {
                        json << ",\n";
                        json << "            \"TANGENT\": " << accessorInfos[i].tangent;
                    }
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
        bool firstBufferView = true;
        const auto writeBufferViewSeparator = [&]() {
            if (!firstBufferView) json << ",\n";
            firstBufferView = false;
        };

        // Vertex bufferViews
        for (size_t i = 0; i < exportMeshes.size(); ++i)
        {
            writeBufferViewSeparator();
            json << "    {\n";
            json << "      \"buffer\": 0,\n";
            json << "      \"byteOffset\": " << binInfos[i].vertByteOffset << ",\n";
            json << "      \"byteLength\": " << binInfos[i].vertByteLength << ",\n";
            json << "      \"byteStride\": " << sizeof(PackedSkinnedVertex) << ",\n";
            json << "      \"target\": 34962\n";
            json << "    }";
        }
        // Index bufferViews
        for (size_t i = 0; i < exportMeshes.size(); ++i)
        {
            writeBufferViewSeparator();
            json << "    {\n";
            json << "      \"buffer\": 0,\n";
            json << "      \"byteOffset\": " << binInfos[i].indexByteOffset << ",\n";
            json << "      \"byteLength\": " << binInfos[i].indexByteLength << ",\n";
            json << "      \"target\": 34963\n";
            json << "    }";
        }
        for (size_t skinIndex = 0; skinIndex < skinInfos.size(); ++skinIndex)
        {
            writeBufferViewSeparator();
            json << "    {\n";
            json << "      \"buffer\": 0,\n";
            json << "      \"byteOffset\": " << skinBufferInfos[skinIndex].byteOffset << ",\n";
            json << "      \"byteLength\": " << skinBufferInfos[skinIndex].byteLength << "\n";
            json << "    }";
        }
        // Tangent bufferViews
        for (size_t i = 0; i < exportMeshes.size(); ++i)
        {
            if (binInfos[i].hasTangent)
            {
                writeBufferViewSeparator();
                json << "    {\n";
                json << "      \"buffer\": 0,\n";
                json << "      \"byteOffset\": " << binInfos[i].tangentByteOffset << ",\n";
                json << "      \"byteLength\": " << binInfos[i].tangentByteLength << ",\n";
                json << "      \"target\": 34962\n"; // ARRAY_BUFFER
                json << "    }";
            }
        }
        if (textureSections.embeddedImageCount > 0)
        {
            writeBufferViewSeparator();
            WriteEmbeddedImageBufferViews(json, textureSections);
        }
        if (animationSections.bufferViewCount > 0)
        {
            writeBufferViewSeparator();
            WriteAnimationBufferViews(json, animationSections);
        }
        json << "\n  ],\n";

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
            if (binInfos[i].hasTangent)
            {
                writeAccessorSeparator();
                json << "    { \"bufferView\": " << tangentBufferViews[i]
                     << ", \"byteOffset\": 0, \"componentType\": 5126, \"count\": " << mesh.vertices.size()
                     << ", \"type\": \"VEC4\" }";
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

        for (size_t skinIndex = 0; skinIndex < skinInfos.size(); ++skinIndex)
        {
            writeAccessorSeparator();
            json << "    { \"bufferView\": " << skinBufferViewIndices[skinIndex]
                 << ", \"byteOffset\": 0, \"componentType\": 5126, \"count\": "
                 << skinInfos[skinIndex].inverseBindMatrices.size()
                 << ", \"type\": \"MAT4\" }";
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
}
