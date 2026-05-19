#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace DagorFormats
{
    struct GeomNodeTreeNode
    {
        static constexpr uint16_t InvalidIndex = 0xFFFFu;

        std::string name;
        uint16_t parentIndex = InvalidIndex;
        std::vector<uint16_t> childIndices;
        std::array<float, 16> localTransform = {};
        std::array<float, 16> worldTransform = {};
        std::array<float, 16> inverseBindTransform = {};
    };

    class GeomNodeTreeReader
    {
    public:
        explicit GeomNodeTreeReader(std::span<const uint8_t> data);
        GeomNodeTreeReader(std::vector<uint8_t>&&) = delete;

        void Parse();

        const std::vector<GeomNodeTreeNode>& GetNodes() const { return m_nodes; }
        uint32_t GetImportantNodeCount() const { return m_importantNodeCount; }
        bool HasCompressedPayload() const { return m_wasCompressed; }

    private:
        std::span<const uint8_t> m_data;
        std::vector<GeomNodeTreeNode> m_nodes;
        uint32_t m_importantNodeCount = 0;
        bool m_wasCompressed = false;
    };
}
