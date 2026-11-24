#pragma once

#include <cstdint>
#include <array>
#include <span>
#include <string>
#include <vector>

namespace DagorFormats
{
    struct Anim2DataNodeTrackInfo
    {
        std::string name;
        uint16_t trackIndex = 0xFFFFu;
        float weight = 1.0f;
    };

    struct Anim2DataChannelInfo
    {
        uint32_t dataType = 0;
        uint32_t channelType = 0;
        uint32_t nodeCount = 0;
        uint32_t animTracksOffset = 0;
        uint32_t nodeTrackOffset = 0;
        uint32_t nodeWeightOffset = 0;
        uint32_t nodeNameOffset = 0;
        bool hasAclTracks = false;
        std::vector<Anim2DataNodeTrackInfo> nodes;
    };

    struct Anim2DataLabelInfo
    {
        std::string name;
        int32_t time = 0;
    };

    struct Anim2DataNodeAnimation
    {
        std::string nodeName;
        std::vector<float> times;
        std::vector<std::array<float, 3>> translations;
        std::vector<std::array<float, 4>> rotations;
        std::vector<std::array<float, 3>> scales;
    };

    struct Anim2DataClipInfo
    {
        std::string name;
        float durationSeconds = 0.0f;
        float sampleRate = 0.0f;
        uint32_t sampleCount = 0;
        std::vector<Anim2DataNodeAnimation> nodes;
        std::vector<std::string> warnings;
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
        std::vector<Anim2DataClipInfo> clips;
        std::vector<std::string> decodeWarnings;
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
