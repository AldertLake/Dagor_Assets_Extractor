#include "Anim2DataReader.h"

#include "DagorIO/Decompression.h"

#include <acl/core/compressed_tracks.h>
#include <acl/decompression/decompress.h>
#include <rtm/quatf.h>
#include <rtm/vector4f.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_map>
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
        constexpr uint32_t kChannelTypePosition = 1;
        constexpr uint32_t kChannelTypeScale = 2;
        constexpr uint32_t kChannelTypeRotation = 5;

        constexpr size_t kAnimHeaderSize = 16;
        constexpr size_t kPatchableTabSize = 16;
        constexpr size_t kPatchablePtrSize = 8;
        constexpr size_t kAnimDataChanSize = 40;
        constexpr size_t kAnimKeyLabelSize = 16;
        constexpr uint16_t kInvalidTrackIndex = 0xFFFFu;
        constexpr size_t kMinAclHeaderSize = 40;

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

        uint16_t ReadU16(std::span<const uint8_t> data, size_t offset)
        {
            if (offset > data.size() || data.size() - offset < sizeof(uint16_t))
            {
                throw std::runtime_error("Anim2Data read out of bounds");
            }

            return static_cast<uint16_t>(data[offset]) |
                   static_cast<uint16_t>(data[offset + 1] << 8);
        }

        int32_t ReadI32(std::span<const uint8_t> data, size_t offset)
        {
            return static_cast<int32_t>(ReadU32(data, offset));
        }

        float ReadF32(std::span<const uint8_t> data, size_t offset)
        {
            const uint32_t raw = ReadU32(data, offset);
            float value = 0.0f;
            std::memcpy(&value, &raw, sizeof(value));
            return value;
        }

        uint64_t ReadU64(std::span<const uint8_t> data, size_t offset)
        {
            const uint64_t lo = ReadU32(data, offset);
            const uint64_t hi = ReadU32(data, offset + sizeof(uint32_t));
            return lo | (hi << 32);
        }

        constexpr uint32_t kInvalidOffset = 0xFFFFFFFFu;

        uint32_t ReadPatchablePtrOffset(std::span<const uint8_t> data, size_t offset)
        {
            const uint32_t rawOffset = ReadU32(data, offset);
            if (static_cast<int32_t>(rawOffset) < 0)
            {
                return kInvalidOffset;
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

        struct TransformDecompressionSettings : public acl::default_transform_decompression_settings
        {
            static constexpr acl::compressed_tracks_version16 version_supported()
            {
                return acl::compressed_tracks_version16::latest;
            }

            static constexpr bool is_wrapping_supported()
            {
                return false;
            }
        };

        struct Float3DecompressionSettings : public acl::default_scalar_decompression_settings
        {
            static constexpr acl::compressed_tracks_version16 version_supported()
            {
                return acl::compressed_tracks_version16::latest;
            }

            static constexpr bool is_wrapping_supported()
            {
                return false;
            }

            static constexpr bool is_track_type_supported(acl::track_type8 type)
            {
                return type == acl::track_type8::float3f;
            }
        };

        struct TransformTrackWriter : public acl::track_writer
        {
            std::array<float, 3>* translation = nullptr;
            std::array<float, 4>* rotation = nullptr;
            std::array<float, 3>* scale = nullptr;

            static constexpr acl::default_sub_track_mode get_default_scale_mode()
            {
                return acl::default_sub_track_mode::constant;
            }

            void RTM_SIMD_CALL write_rotation(uint32_t, rtm::quatf_arg0 value)
            {
                if (rotation)
                {
                    *rotation = {
                        rtm::quat_get_x(value),
                        rtm::quat_get_y(value),
                        rtm::quat_get_z(value),
                        rtm::quat_get_w(value)
                    };
                }
            }

            void RTM_SIMD_CALL write_translation(uint32_t, rtm::vector4f_arg0 value)
            {
                if (translation)
                {
                    *translation = {
                        rtm::vector_get_x(value),
                        rtm::vector_get_y(value),
                        rtm::vector_get_z(value)
                    };
                }
            }

            void RTM_SIMD_CALL write_scale(uint32_t, rtm::vector4f_arg0 value)
            {
                if (scale)
                {
                    *scale = {
                        rtm::vector_get_x(value),
                        rtm::vector_get_y(value),
                        rtm::vector_get_z(value)
                    };
                }
            }
        };

        struct Float3TrackWriter : public acl::track_writer
        {
            std::array<float, 3>* value = nullptr;

            void RTM_SIMD_CALL write_float3(uint32_t, rtm::vector4f_arg0 sampledValue)
            {
                if (value)
                {
                    *value = {
                        rtm::vector_get_x(sampledValue),
                        rtm::vector_get_y(sampledValue),
                        rtm::vector_get_z(sampledValue)
                    };
                }
            }
        };

        const acl::compressed_tracks* MakeAclTracks(
            std::span<const uint8_t> dump,
            uint32_t offset,
            std::string& warning)
        {
            if (offset == kInvalidOffset || offset >= dump.size())
            {
                warning = "ACL track pointer is null or out of bounds.";
                return nullptr;
            }
            if (dump.size() - offset < kMinAclHeaderSize)
            {
                warning = "ACL track pointer is too close to the end of the Anim2Data dump.";
                return nullptr;
            }

            const uint32_t aclSize = ReadU32(dump, offset);
            if (aclSize < kMinAclHeaderSize || aclSize > dump.size() - offset)
            {
                warning = "ACL compressed_tracks size is outside the Anim2Data dump.";
                return nullptr;
            }

            acl::error_result error;
            const acl::compressed_tracks* tracks = acl::make_compressed_tracks(dump.data() + offset, &error);
            if (!tracks)
            {
                warning = std::string("ACL compressed_tracks validation failed: ") + error.c_str();
                return nullptr;
            }
            if (const acl::error_result valid = tracks->is_valid(false); valid.any())
            {
                warning = std::string("ACL compressed_tracks is invalid: ") + valid.c_str();
                return nullptr;
            }
            return tracks;
        }

        bool IsFiniteVec3(const std::array<float, 3>& value)
        {
            return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
        }

        bool IsFiniteQuat(const std::array<float, 4>& value)
        {
            return std::isfinite(value[0]) && std::isfinite(value[1]) &&
                   std::isfinite(value[2]) && std::isfinite(value[3]);
        }

        void NormalizeQuat(std::array<float, 4>& value)
        {
            const float lengthSq =
                (value[0] * value[0]) +
                (value[1] * value[1]) +
                (value[2] * value[2]) +
                (value[3] * value[3]);
            if (!std::isfinite(lengthSq) || lengthSq <= 1e-12f)
            {
                value = { 0.0f, 0.0f, 0.0f, 1.0f };
                return;
            }

            const float invLength = 1.0f / std::sqrt(lengthSq);
            value[0] *= invLength;
            value[1] *= invLength;
            value[2] *= invLength;
            value[3] *= invLength;
        }

        std::vector<float> BuildSampleTimes(const acl::compressed_tracks& tracks)
        {
            const uint32_t sampleCount = tracks.get_num_samples_per_track();
            const float sampleRate = tracks.get_sample_rate();
            std::vector<float> times;
            times.reserve(sampleCount);

            if (sampleCount == 0 || !std::isfinite(sampleRate) || sampleRate <= 0.0f)
            {
                return times;
            }

            const float duration = tracks.get_duration();
            for (uint32_t i = 0; i < sampleCount; ++i)
            {
                float time = static_cast<float>(i) / sampleRate;
                if (i + 1 == sampleCount && std::isfinite(duration) && duration >= 0.0f)
                {
                    time = duration;
                }
                times.push_back(time);
            }
            return times;
        }

        void UpdateClipTiming(Anim2DataClipInfo& clip, const acl::compressed_tracks& tracks)
        {
            const float duration = tracks.get_duration();
            const float sampleRate = tracks.get_sample_rate();
            if (std::isfinite(duration))
            {
                clip.durationSeconds = std::max(clip.durationSeconds, duration);
            }
            if (std::isfinite(sampleRate))
            {
                clip.sampleRate = std::max(clip.sampleRate, sampleRate);
            }
            clip.sampleCount = std::max(clip.sampleCount, tracks.get_num_samples_per_track());
        }

        void AppendTransformSamples(
            std::span<const uint8_t> dump,
            const Anim2DataChannelInfo& channel,
            Anim2DataClipInfo& clip)
        {
            std::string warning;
            const acl::compressed_tracks* tracks = MakeAclTracks(dump, channel.animTracksOffset, warning);
            if (!tracks)
            {
                clip.warnings.push_back("Transform channel skipped: " + warning);
                return;
            }
            if (tracks->get_track_type() != acl::track_type8::qvvf)
            {
                clip.warnings.push_back("Rotation channel did not contain ACL qvvf transform tracks.");
                return;
            }

            acl::decompression_context<TransformDecompressionSettings> context;
            if (!context.initialize(*tracks))
            {
                clip.warnings.push_back("ACL transform decompression context failed to initialize.");
                return;
            }

            const std::vector<float> times = BuildSampleTimes(*tracks);
            if (times.empty())
            {
                clip.warnings.push_back("ACL transform channel has no sample times.");
                return;
            }
            UpdateClipTiming(clip, *tracks);

            for (const Anim2DataNodeTrackInfo& node : channel.nodes)
            {
                if (node.name.empty())
                {
                    clip.warnings.push_back("Transform channel has an unnamed node; skipped.");
                    continue;
                }
                if (node.trackIndex == kInvalidTrackIndex || node.trackIndex >= tracks->get_num_tracks())
                {
                    clip.warnings.push_back(
                        "Transform node " + node.name +
                        " references invalid ACL track " + std::to_string(node.trackIndex) + ".");
                    continue;
                }

                Anim2DataNodeAnimation nodeAnim;
                nodeAnim.nodeName = node.name;
                nodeAnim.times = times;
                nodeAnim.translations.reserve(times.size());
                nodeAnim.rotations.reserve(times.size());
                nodeAnim.scales.reserve(times.size());

                for (float time : times)
                {
                    context.seek(time, acl::sample_rounding_policy::none);

                    std::array<float, 3> translation = { 0.0f, 0.0f, 0.0f };
                    std::array<float, 4> rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
                    std::array<float, 3> scale = { 1.0f, 1.0f, 1.0f };
                    TransformTrackWriter writer;
                    writer.translation = &translation;
                    writer.rotation = &rotation;
                    writer.scale = &scale;
                    context.decompress_track(node.trackIndex, writer);

                    if (!IsFiniteVec3(translation))
                    {
                        translation = { 0.0f, 0.0f, 0.0f };
                    }
                    if (!IsFiniteQuat(rotation))
                    {
                        rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
                    }
                    NormalizeQuat(rotation);
                    if (!IsFiniteVec3(scale))
                    {
                        scale = { 1.0f, 1.0f, 1.0f };
                    }

                    nodeAnim.translations.push_back(translation);
                    nodeAnim.rotations.push_back(rotation);
                    nodeAnim.scales.push_back(scale);
                }

                clip.nodes.push_back(std::move(nodeAnim));
            }
        }

        void AppendFloat3Samples(
            std::span<const uint8_t> dump,
            const Anim2DataChannelInfo& channel,
            Anim2DataClipInfo& clip)
        {
            std::string warning;
            const acl::compressed_tracks* tracks = MakeAclTracks(dump, channel.animTracksOffset, warning);
            if (!tracks)
            {
                clip.warnings.push_back("Float3 channel skipped: " + warning);
                return;
            }
            if (tracks->get_track_type() != acl::track_type8::float3f)
            {
                return;
            }

            acl::decompression_context<Float3DecompressionSettings> context;
            if (!context.initialize(*tracks))
            {
                clip.warnings.push_back("ACL float3 decompression context failed to initialize.");
                return;
            }

            const std::vector<float> times = BuildSampleTimes(*tracks);
            if (times.empty())
            {
                clip.warnings.push_back("ACL float3 channel has no sample times.");
                return;
            }
            UpdateClipTiming(clip, *tracks);

            for (const Anim2DataNodeTrackInfo& node : channel.nodes)
            {
                if (node.name.empty())
                {
                    clip.warnings.push_back("Float3 channel has an unnamed node; skipped.");
                    continue;
                }
                if (node.trackIndex == kInvalidTrackIndex || node.trackIndex >= tracks->get_num_tracks())
                {
                    clip.warnings.push_back(
                        "Float3 node " + node.name +
                        " references invalid ACL track " + std::to_string(node.trackIndex) + ".");
                    continue;
                }

                Anim2DataNodeAnimation nodeAnim;
                nodeAnim.nodeName = node.name;
                nodeAnim.times = times;
                if (channel.channelType == kChannelTypePosition)
                {
                    nodeAnim.translations.reserve(times.size());
                }
                else if (channel.channelType == kChannelTypeScale)
                {
                    nodeAnim.scales.reserve(times.size());
                }
                else
                {
                    continue;
                }

                for (float time : times)
                {
                    context.seek(time, acl::sample_rounding_policy::none);
                    std::array<float, 3> value = channel.channelType == kChannelTypeScale
                        ? std::array<float, 3>{ 1.0f, 1.0f, 1.0f }
                        : std::array<float, 3>{ 0.0f, 0.0f, 0.0f };
                    Float3TrackWriter writer;
                    writer.value = &value;
                    context.decompress_track(node.trackIndex, writer);
                    if (!IsFiniteVec3(value))
                    {
                        value = channel.channelType == kChannelTypeScale
                            ? std::array<float, 3>{ 1.0f, 1.0f, 1.0f }
                            : std::array<float, 3>{ 0.0f, 0.0f, 0.0f };
                    }

                    if (channel.channelType == kChannelTypePosition)
                    {
                        nodeAnim.translations.push_back(value);
                    }
                    else
                    {
                        nodeAnim.scales.push_back(value);
                    }
                }

                clip.nodes.push_back(std::move(nodeAnim));
            }
        }

        void SampleAclClips(std::span<const uint8_t> dump, Anim2DataInfo& info)
        {
            Anim2DataClipInfo clip;
            clip.name = "Anim2Data";
            if (info.additive)
            {
                clip.warnings.push_back(
                    "Animation is additive (ANIM 0x301); exported curves are raw additive data and may require the original reference pose to play exactly like Dagor.");
            }

            std::unordered_set<uint32_t> sampledTransformOffsets;
            for (const Anim2DataChannelInfo& channel : info.channels)
            {
                if (!channel.hasAclTracks)
                {
                    continue;
                }
                if (channel.dataType == kDataTypeQuat && channel.channelType == kChannelTypeRotation)
                {
                    if (sampledTransformOffsets.insert(channel.animTracksOffset).second)
                    {
                        AppendTransformSamples(dump, channel, clip);
                    }
                    continue;
                }
                if (channel.dataType == kDataTypePoint3 &&
                    (channel.channelType == kChannelTypePosition || channel.channelType == kChannelTypeScale))
                {
                    AppendFloat3Samples(dump, channel, clip);
                }
            }

            if (!clip.nodes.empty() || !clip.warnings.empty())
            {
                info.decodeWarnings.insert(info.decodeWarnings.end(), clip.warnings.begin(), clip.warnings.end());
                info.clips.push_back(std::move(clip));
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
                channel.animTracksOffset = ReadPatchablePtrOffset(dump, chanOffset);
                channel.nodeTrackOffset = ReadPatchablePtrOffset(dump, chanOffset + kPatchablePtrSize);
                channel.nodeCount = ReadU32(dump, chanOffset + (kPatchablePtrSize * 2));
                channel.channelType = ReadU32(dump, chanOffset + (kPatchablePtrSize * 2) + sizeof(uint32_t));
                channel.nodeWeightOffset = ReadPatchablePtrOffset(
                    dump,
                    chanOffset + (kPatchablePtrSize * 2) + (sizeof(uint32_t) * 2));
                channel.nodeNameOffset = ReadPatchablePtrOffset(
                    dump,
                    chanOffset + (kPatchablePtrSize * 2) + (sizeof(uint32_t) * 2) + kPatchablePtrSize);
                channel.hasAclTracks =
                    channel.animTracksOffset != kInvalidOffset &&
                    channel.animTracksOffset < dump.size() &&
                    dump.size() - channel.animTracksOffset >= kMinAclHeaderSize;

                std::string hexDump = "";
                for (size_t b = 0; b < kAnimDataChanSize && (chanOffset + b) < dump.size(); ++b)
                {
                    char buf[4];
                    sprintf_s(buf, "%02X ", dump[chanOffset + b]);
                    hexDump += buf;
                }

                m_info.decodeWarnings.push_back(
                    "Channel " + std::to_string(i) +
                    ": dataType=" + std::to_string(dataType) +
                    ", channelType=" + std::to_string(channel.channelType) +
                    ", nodeCount=" + std::to_string(channel.nodeCount) +
                    ", animTracksOffset=" + std::to_string(channel.animTracksOffset) +
                    ", nodeTrackOffset=" + std::to_string(channel.nodeTrackOffset) +
                    ", nodeNameOffset=" + std::to_string(channel.nodeNameOffset) +
                    ", dumpSize=" + std::to_string(dump.size()) +
                    ", hasAclTracks=" + std::to_string(channel.hasAclTracks) +
                    " | Hex: " + hexDump
                );

                if (channel.nodeCount > 0)
                {
                    if (channel.nodeNameOffset != kInvalidOffset)
                    {
                        ValidateSpan(
                            dump,
                            channel.nodeNameOffset,
                            static_cast<size_t>(channel.nodeCount) * kPatchablePtrSize,
                            "node-name table");
                    }
                    if (channel.nodeTrackOffset != kInvalidOffset)
                    {
                        ValidateSpan(
                            dump,
                            channel.nodeTrackOffset,
                            static_cast<size_t>(channel.nodeCount) * sizeof(uint16_t),
                            "node-track table");
                    }
                    if (channel.nodeWeightOffset != kInvalidOffset)
                    {
                        ValidateSpan(
                            dump,
                            channel.nodeWeightOffset,
                            static_cast<size_t>(channel.nodeCount) * sizeof(float),
                            "node-weight table");
                    }

                    channel.nodes.reserve(channel.nodeCount);
                    for (uint32_t node = 0; node < channel.nodeCount; ++node)
                    {
                        Anim2DataNodeTrackInfo nodeInfo;
                        nodeInfo.trackIndex = channel.nodeTrackOffset != kInvalidOffset
                            ? ReadU16(dump, static_cast<size_t>(channel.nodeTrackOffset) + (static_cast<size_t>(node) * sizeof(uint16_t)))
                            : static_cast<uint16_t>(node);
                        nodeInfo.weight = channel.nodeWeightOffset != kInvalidOffset
                            ? ReadF32(dump, static_cast<size_t>(channel.nodeWeightOffset) + (static_cast<size_t>(node) * sizeof(float)))
                            : 1.0f;

                        const uint32_t nameOffset = channel.nodeNameOffset != kInvalidOffset
                            ? ReadPatchablePtrOffset(
                                dump,
                                static_cast<size_t>(channel.nodeNameOffset) + (static_cast<size_t>(node) * kPatchablePtrSize))
                            : kInvalidOffset;
                        if (nameOffset != kInvalidOffset)
                        {
                            nodeInfo.name = ReadCString(dump, nameOffset);
                            rememberNodeName(nodeInfo.name);
                        }
                        channel.nodes.push_back(std::move(nodeInfo));
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
                if (nameOffset == kInvalidOffset)
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
        SampleAclClips(dump, m_info);
    }
}
