#include "Anim2DataReader.h"

#include "DagorIO/Decompression.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace DagorFormats
{
    namespace
    {
        constexpr uint32_t kAnimMagic = 0x4D494E41u; // ANIM, little-endian
        constexpr uint32_t kVersion300 = 0x300u;
        constexpr uint32_t kVersion301 = 0x301u;
        constexpr uint32_t kBlockSizeMask = 0x3FFFFFFFu;
        constexpr unsigned kBlockCompressionNone = 0;
        constexpr unsigned kBlockCompressionZstd = 1;
        constexpr unsigned kBlockCompressionOodle = 2;

        constexpr uint32_t kDataTypePoint3 = 1;
        constexpr uint32_t kDataTypeQuat = 2;
        constexpr uint32_t kDataTypeReal = 3;

        constexpr size_t kAnimHeaderSize = 16;
        constexpr size_t kPatchableTabSize = 16;
        constexpr size_t kPatchablePtrSize = 8;
        constexpr size_t kAnimDataChanSize = 40;
        constexpr size_t kAnimKeyLabelSize = 16;

        uint32_t ReadU32(std::span<const uint8_t> data, size_t offset)
        {
            if (offset > data.size() || data.size() - offset < sizeof(uint32_t))
            {
                throw std::runtime_error("Anim2Data read out of bounds");
            }

            return static_cast<uint32_t>(data[offset]) |
                   (static_cast<uint32_t>(data[offset + 1]) << 8) |
                   (static_cast<uint32_t>(data[offset + 2]) << 16) |
                   (static_cast<uint32_t>(data[offset + 3]) << 24);
        }

        int32_t ReadI32(std::span<const uint8_t> data, size_t offset)
        {
            return static_cast<int32_t>(ReadU32(data, offset));
        }

        uint64_t ReadU64(std::span<const uint8_t> data, size_t offset)
        {
            const uint64_t lo = ReadU32(data, offset);
            const uint64_t hi = ReadU32(data, offset + sizeof(uint32_t));
            return lo | (hi << 32);
        }

        uint32_t ReadPatchablePtrOffset(std::span<const uint8_t> data, size_t offset)
        {
            const uint32_t rawOffset = ReadU32(data, offset);
            if (static_cast<int32_t>(rawOffset) < 0)
            {
                return 0;
            }
            return rawOffset;
        }

        struct PatchableTabRef
        {
            uint32_t offset = 0;
            uint32_t count = 0;
        };

        PatchableTabRef ReadPatchableTab(std::span<const uint8_t> data, size_t offset)
        {
            const uint64_t raw = ReadU64(data, offset);
            PatchableTabRef ref;
            ref.offset = static_cast<uint32_t>(raw & 0xFFFFFFFFu);
            ref.count = static_cast<uint32_t>(raw >> 32);
            if (ref.count == 0)
            {
                ref.offset = 0;
            }
            return ref;
        }

        void ValidateSpan(std::span<const uint8_t> data, size_t offset, size_t byteSize, const char* what)
        {
            if (offset > data.size() || data.size() - offset < byteSize)
            {
                throw std::runtime_error(std::string("Anim2Data ") + what + " is out of bounds");
            }
        }

        std::string ReadCString(std::span<const uint8_t> data, uint32_t offset)
        {
            if (offset >= data.size())
            {
                throw std::runtime_error("Anim2Data string offset is out of bounds");
            }

            size_t end = offset;
            while (end < data.size() && data[end] != 0)
            {
                ++end;
            }
            if (end == data.size())
            {
                throw std::runtime_error("Anim2Data string is not null-terminated");
            }

            return std::string(reinterpret_cast<const char*>(data.data() + offset), end - offset);
        }

        std::span<const uint8_t> UnwrapPayload(std::span<const uint8_t> data, std::vector<uint8_t>& storage, bool& compressed)
        {
            compressed = false;
            if (data.size() >= sizeof(uint32_t) && ReadU32(data, 0) == kAnimMagic)
            {
                return data;
            }

            if (data.size() < sizeof(uint32_t))
            {
                throw std::runtime_error("Anim2Data payload is too small");
            }

            const uint32_t blockHeader = ReadU32(data, 0);
            const unsigned blockFlags = blockHeader >> 30;
            const size_t blockSize = blockHeader & kBlockSizeMask;
            if (blockSize == 0 || sizeof(uint32_t) + blockSize > data.size())
            {
                throw std::runtime_error("Anim2Data payload is not an ANIM dump or a supported compressed block");
            }

            const std::span<const uint8_t> blockData(data.data() + sizeof(uint32_t), blockSize);
            switch (blockFlags)
            {
            case kBlockCompressionNone:
                return blockData;
            case kBlockCompressionZstd:
                compressed = true;
                storage = DagorIO::Decompressor::DecompressZstd(blockData);
                return storage;
            case kBlockCompressionOodle:
                if (blockData.size() < sizeof(uint32_t))
                {
                    throw std::runtime_error("Anim2Data Oodle block is truncated");
                }
                compressed = true;
                storage = DagorIO::Decompressor::DecompressOodle(
                    blockData.subspan(sizeof(uint32_t)),
                    ReadU32(blockData, 0),
                    std::string(),
                    std::string());
                return storage;
            default:
                throw std::runtime_error("Anim2Data uses an unknown block compression type");
            }
        }
    }

    Anim2DataReader::Anim2DataReader(std::span<const uint8_t> data)
        : m_data(data)
    {
    }

    void Anim2DataReader::Parse()
    {
        m_info = {};
        const std::span<const uint8_t> payload = UnwrapPayload(m_data, m_uncompressed, m_info.compressed);
        ValidateSpan(payload, 0, kAnimHeaderSize, "header");

        const uint32_t label = ReadU32(payload, 0);
        m_info.version = ReadU32(payload, 4);
        const uint32_t headerSize = ReadU32(payload, 8);
        m_info.dumpSize = ReadU32(payload, 12);

        if (label != kAnimMagic)
        {
            throw std::runtime_error("Anim2Data payload does not start with ANIM");
        }
        if (m_info.version != kVersion300 && m_info.version != kVersion301)
        {
            throw std::runtime_error("Anim2Data uses unsupported ANIM version");
        }
        if (headerSize != kAnimHeaderSize)
        {
            throw std::runtime_error("Anim2Data header size is unsupported");
        }

        const size_t dumpOffset = kAnimHeaderSize;
        const size_t dumpDataOffset = dumpOffset + m_info.dumpSize;
        ValidateSpan(payload, dumpOffset, m_info.dumpSize, "dump pool");
        ValidateSpan(payload, dumpDataOffset, kPatchableTabSize * 4, "DumpData");
        const std::span<const uint8_t> dump(payload.data() + dumpOffset, m_info.dumpSize);

        const PatchableTabRef chanPoint3 = ReadPatchableTab(payload, dumpDataOffset);
        const PatchableTabRef chanQuat = ReadPatchableTab(payload, dumpDataOffset + kPatchableTabSize);
        const PatchableTabRef chanReal = ReadPatchableTab(payload, dumpDataOffset + (kPatchableTabSize * 2));
        const PatchableTabRef noteTrack = ReadPatchableTab(payload, dumpDataOffset + (kPatchableTabSize * 3));

        std::unordered_set<std::string> seenNames;
        const auto rememberNodeName = [&](std::string name)
        {
            if (seenNames.insert(name).second)
            {
                m_info.nodeNames.push_back(std::move(name));
            }
        };

        const auto parseChannels = [&](PatchableTabRef ref, uint32_t dataType)
        {
            if (ref.count == 0)
            {
                return;
            }
            ValidateSpan(dump, ref.offset, static_cast<size_t>(ref.count) * kAnimDataChanSize, "channel table");

            for (uint32_t i = 0; i < ref.count; ++i)
            {
                const size_t chanOffset = static_cast<size_t>(ref.offset) + (static_cast<size_t>(i) * kAnimDataChanSize);
                Anim2DataChannelInfo channel;
                channel.dataType = dataType;
                const uint32_t animTracksOffset = ReadPatchablePtrOffset(dump, chanOffset);
                channel.hasAclTracks = animTracksOffset > 0 && animTracksOffset < dump.size();
                channel.nodeCount = ReadU32(dump, chanOffset + (kPatchablePtrSize * 2));
                channel.channelType = ReadU32(dump, chanOffset + (kPatchablePtrSize * 2) + sizeof(uint32_t));

                const uint32_t nodeNameOffset = ReadPatchablePtrOffset(
                    dump,
                    chanOffset + (kPatchablePtrSize * 2) + (sizeof(uint32_t) * 2) + kPatchablePtrSize);
                if (channel.nodeCount > 0)
                {
                    ValidateSpan(dump, nodeNameOffset, static_cast<size_t>(channel.nodeCount) * kPatchablePtrSize, "node-name table");
                    for (uint32_t node = 0; node < channel.nodeCount; ++node)
                    {
                        const uint32_t nameOffset = ReadPatchablePtrOffset(
                            dump,
                            static_cast<size_t>(nodeNameOffset) + (static_cast<size_t>(node) * kPatchablePtrSize));
                        if (nameOffset > 0)
                        {
                            rememberNodeName(ReadCString(dump, nameOffset));
                        }
                    }
                }

                m_info.channels.push_back(channel);
            }
        };

        parseChannels(chanPoint3, kDataTypePoint3);
        parseChannels(chanQuat, kDataTypeQuat);
        parseChannels(chanReal, kDataTypeReal);

        if (noteTrack.count > 0)
        {
            ValidateSpan(dump, noteTrack.offset, static_cast<size_t>(noteTrack.count) * kAnimKeyLabelSize, "note track");
            for (uint32_t i = 0; i < noteTrack.count; ++i)
            {
                const size_t labelOffset = static_cast<size_t>(noteTrack.offset) + (static_cast<size_t>(i) * kAnimKeyLabelSize);
                const uint32_t nameOffset = ReadPatchablePtrOffset(dump, labelOffset);
                if (nameOffset == 0)
                {
                    continue;
                }

                Anim2DataLabelInfo labelInfo;
                labelInfo.name = ReadCString(dump, nameOffset);
                labelInfo.time = ReadI32(dump, labelOffset + kPatchablePtrSize);
                m_info.labels.push_back(std::move(labelInfo));
            }
        }

        std::sort(m_info.nodeNames.begin(), m_info.nodeNames.end());
        m_info.additive = (m_info.version == kVersion301);
    }
}
