#include "GeomNodeTreeReader.h"

#include "DagorIO/BinaryReader.h"
#include "DagorIO/Decompression.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string_view>

namespace DagorFormats
{
    namespace
    {
        constexpr uint32_t kNodeOffsetMask = 0x000FFFFFu;
        constexpr uint32_t kCompressedNodeCountMask = 0x7FFFFFFFu;
        constexpr uint32_t kBlockSizeMask = 0x3FFFFFFFu;
        constexpr size_t kMatrixElementCount = 16;
        constexpr size_t kMatrixByteSize = sizeof(float) * kMatrixElementCount;
        constexpr size_t kPatchableTabSize = 16;
        constexpr size_t kPatchablePtrSize = 8;
        constexpr size_t kOldNodeSize = (kMatrixByteSize * 2) + kPatchableTabSize + (kPatchablePtrSize * 2);
        constexpr unsigned kBlockCompressionNone = 0;
        constexpr unsigned kBlockCompressionZstd = 1;
        constexpr unsigned kBlockCompressionOodle = 2;
        constexpr float kSingularEpsilon = 1e-8f;

        uint32_t ReadU32(std::span<const uint8_t> data, size_t offset)
        {
            if (offset > data.size() || data.size() - offset < sizeof(uint32_t))
            {
                throw std::runtime_error("GeomNodeTree read out of bounds");
            }

            return static_cast<uint32_t>(data[offset]) |
                   (static_cast<uint32_t>(data[offset + 1]) << 8) |
                   (static_cast<uint32_t>(data[offset + 2]) << 16) |
                   (static_cast<uint32_t>(data[offset + 3]) << 24);
        }

        std::array<float, 16> ReadMatrix(std::span<const uint8_t> data, size_t offset)
        {
            if (offset > data.size() || data.size() - offset < kMatrixByteSize)
            {
                throw std::runtime_error("GeomNodeTree matrix is out of bounds");
            }

            std::array<float, 16> matrix = {};
            DagorIO::MemoryReader reader(data.subspan(offset, kMatrixByteSize));
            for (float& element : matrix)
            {
                element = reader.ReadFloat();
            }
            return matrix;
        }

        std::string ReadCString(std::span<const uint8_t> data, uint32_t offset)
        {
            if (offset >= data.size())
            {
                throw std::runtime_error("GeomNodeTree name offset is out of bounds");
            }

            size_t end = offset;
            while (end < data.size() && data[end] != 0)
            {
                ++end;
            }

            if (end == data.size())
            {
                throw std::runtime_error("GeomNodeTree name is not null-terminated");
            }

            return std::string(reinterpret_cast<const char*>(data.data() + offset), end - offset);
        }

        std::array<float, 16> MultiplyAffine(const std::array<float, 16>& lhs, const std::array<float, 16>& rhs)
        {
            std::array<float, 16> result = {};
            for (size_t row = 0; row < 4; ++row)
            {
                for (size_t col = 0; col < 4; ++col)
                {
                    float sum = 0.0f;
                    for (size_t k = 0; k < 4; ++k)
                    {
                        sum += lhs[row * 4 + k] * rhs[k * 4 + col];
                    }
                    result[row * 4 + col] = sum;
                }
            }
            return result;
        }

        bool InvertAffine(const std::array<float, 16>& matrix, std::array<float, 16>& inverse)
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
            if (std::fabs(det) <= kSingularEpsilon)
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

            const float tx = matrix[12];
            const float ty = matrix[13];
            const float tz = matrix[14];
            inverse[12] = -((tx * inverse[0]) + (ty * inverse[4]) + (tz * inverse[8]));
            inverse[13] = -((tx * inverse[1]) + (ty * inverse[5]) + (tz * inverse[9]));
            inverse[14] = -((tx * inverse[2]) + (ty * inverse[6]) + (tz * inverse[10]));
            return true;
        }

        std::span<const uint8_t> ReadCompressedPayload(
            DagorIO::MemoryReader& reader,
            std::vector<uint8_t>& storage)
        {
            if (reader.GetRemaining() < sizeof(uint32_t))
            {
                throw std::runtime_error("GeomNodeTree compressed block header is missing");
            }

            const uint32_t blockHeader = reader.ReadUInt32();
            const unsigned blockFlags = blockHeader >> 30;
            const size_t blockSize = blockHeader & kBlockSizeMask;
            const std::span<const uint8_t> blockData = reader.ReadSpan(blockSize);

            switch (blockFlags)
            {
            case kBlockCompressionZstd:
                storage = DagorIO::Decompressor::DecompressZstd(blockData);
                return storage;
            case kBlockCompressionOodle:
            {
                if (blockData.size() < sizeof(uint32_t))
                {
                    throw std::runtime_error("GeomNodeTree Oodle block is truncated");
                }

                const uint32_t decompressedSize = ReadU32(blockData, 0);
                storage = DagorIO::Decompressor::DecompressOodle(
                    blockData.subspan(sizeof(uint32_t)),
                    decompressedSize,
                    std::string(),
                    std::string());
                return storage;
            }
            case kBlockCompressionNone:
                throw std::runtime_error("GeomNodeTree uses unsupported legacy block compression");
            default:
                throw std::runtime_error("GeomNodeTree uses an unknown block compression type");
            }
        }
    }

    GeomNodeTreeReader::GeomNodeTreeReader(std::span<const uint8_t> data)
        : m_data(data)
    {
    }

    void GeomNodeTreeReader::Parse()
    {
        DagorIO::MemoryReader reader(m_data);
        if (reader.GetRemaining() < sizeof(uint32_t) * 2)
        {
            throw std::runtime_error("GeomNodeTree payload is too small");
        }

        const uint32_t packedOffset = reader.ReadUInt32();
        const uint32_t nodeCountField = reader.ReadUInt32();
        const uint32_t nodeCount = nodeCountField & kCompressedNodeCountMask;
        m_wasCompressed = (nodeCountField & ~kCompressedNodeCountMask) != 0;

        const uint32_t rawDataSize = packedOffset & kNodeOffsetMask;
        const uint32_t lastUnimportantCount = packedOffset >> 20;
        if (lastUnimportantCount > nodeCount)
        {
            throw std::runtime_error("GeomNodeTree important-node count is invalid");
        }

        m_importantNodeCount = nodeCount - lastUnimportantCount;
        m_nodes.clear();
        m_nodes.resize(nodeCount);

        std::vector<uint8_t> decompressedPayload;
        std::span<const uint8_t> payload =
            m_wasCompressed ? ReadCompressedPayload(reader, decompressedPayload)
                            : reader.ReadSpan(reader.GetRemaining());

        if (payload.size() < rawDataSize)
        {
            throw std::runtime_error("GeomNodeTree payload is truncated");
        }

        const std::span<const uint8_t> rawData = payload.first(rawDataSize);
        const size_t expectedNodeBytes = static_cast<size_t>(nodeCount) * kOldNodeSize;
        if (rawDataSize < expectedNodeBytes)
        {
            throw std::runtime_error("GeomNodeTree node table is truncated");
        }

        for (uint32_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex)
        {
            const size_t nodeOffset = static_cast<size_t>(nodeIndex) * kOldNodeSize;
            GeomNodeTreeNode& node = m_nodes[nodeIndex];
            node.localTransform = ReadMatrix(rawData, nodeOffset);

            const uint32_t childOffset = ReadU32(rawData, nodeOffset + (kMatrixByteSize * 2));
            const uint32_t childCount = ReadU32(rawData, nodeOffset + (kMatrixByteSize * 2) + sizeof(uint32_t));
            if (childCount > 0)
            {
                if ((childOffset % kOldNodeSize) != 0)
                {
                    throw std::runtime_error("GeomNodeTree child pointer is misaligned");
                }

                const size_t firstChildIndex = childOffset / kOldNodeSize;
                if (firstChildIndex > nodeCount || nodeCount - firstChildIndex < childCount)
                {
                    throw std::runtime_error("GeomNodeTree child range is out of bounds");
                }

                node.childIndices.reserve(childCount);
                for (uint32_t child = 0; child < childCount; ++child)
                {
                    node.childIndices.push_back(static_cast<uint16_t>(firstChildIndex + child));
                }
            }

            const uint32_t rawParentPointer = ReadU32(rawData, nodeOffset + (kMatrixByteSize * 2) + kPatchableTabSize);
            if (static_cast<int32_t>(rawParentPointer) < 0)
            {
                node.parentIndex = GeomNodeTreeNode::InvalidIndex;
            }
            else
            {
                if ((rawParentPointer % kOldNodeSize) != 0)
                {
                    throw std::runtime_error("GeomNodeTree parent pointer is misaligned");
                }

                const size_t parentIndex = rawParentPointer / kOldNodeSize;
                if (parentIndex >= nodeCount)
                {
                    throw std::runtime_error("GeomNodeTree parent pointer is out of bounds");
                }

                node.parentIndex = static_cast<uint16_t>(parentIndex);
            }

            const uint32_t nameOffset = ReadU32(rawData, nodeOffset + (kMatrixByteSize * 2) + kPatchableTabSize + kPatchablePtrSize);
            node.name = ReadCString(rawData, nameOffset);
        }

        enum class VisitState
        {
            Unvisited,
            Visiting,
            Visited
        };

        std::vector<VisitState> states(nodeCount, VisitState::Unvisited);

        const auto resolveNode = [&](auto&& self, size_t nodeIndex) -> void
        {
            if (states[nodeIndex] == VisitState::Visited)
            {
                return;
            }
            if (states[nodeIndex] == VisitState::Visiting)
            {
                throw std::runtime_error("GeomNodeTree contains a cycle");
            }

            states[nodeIndex] = VisitState::Visiting;
            GeomNodeTreeNode& node = m_nodes[nodeIndex];
            if (node.parentIndex == GeomNodeTreeNode::InvalidIndex)
            {
                node.worldTransform = node.localTransform;
            }
            else
            {
                if (node.parentIndex >= nodeCount)
                {
                    throw std::runtime_error("GeomNodeTree parent index is out of bounds");
                }

                self(self, node.parentIndex);
                node.worldTransform = MultiplyAffine(m_nodes[node.parentIndex].worldTransform, node.localTransform);
            }

            if (!InvertAffine(node.worldTransform, node.inverseBindTransform))
            {
                throw std::runtime_error("GeomNodeTree node transform is singular");
            }

            states[nodeIndex] = VisitState::Visited;
        };

        for (size_t nodeIndex = 0; nodeIndex < nodeCount; ++nodeIndex)
        {
            resolveNode(resolveNode, nodeIndex);
        }
    }
}
