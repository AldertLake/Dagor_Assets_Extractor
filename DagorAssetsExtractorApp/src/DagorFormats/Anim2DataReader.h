#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace DagorFormats
{
    struct Anim2DataChannelInfo
    {
        uint32_t dataType = 0;
        uint32_t channelType = 0;
        uint32_t nodeCount = 0;
        bool hasAclTracks = false;
    };

    struct Anim2DataLabelInfo
    {
        std::string name;
        int32_t time = 0;
    };

    struct Anim2DataInfo
    {
        uint32_t version = 0;
        uint32_t dumpSize = 0;
        bool additive = false;
        bool compressed = false;
        std::vector<Anim2DataChannelInfo> channels;
        std::vector<std::string> nodeNames;
        std::vector<Anim2DataLabelInfo> labels;
    };

    class Anim2DataReader
    {
    public:
        explicit Anim2DataReader(std::span<const uint8_t> data);

        void Parse();

        const Anim2DataInfo& GetInfo() const { return m_info; }

    private:
        std::span<const uint8_t> m_data;
        std::vector<uint8_t> m_uncompressed;
        Anim2DataInfo m_info;
    };
}
