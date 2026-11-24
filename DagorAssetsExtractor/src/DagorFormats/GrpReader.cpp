#include "GrpReader.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace DagorFormats {
namespace {

    constexpr size_t kModernHeaderSize = 16;
    constexpr size_t kPatchableTabSize = 16;
    constexpr size_t kGrpDataHeaderSize = kPatchableTabSize * 3;
    constexpr size_t kResEntrySize = 12;
    constexpr size_t kMaxNameLength = 2048;

    uint16_t ReadU16(std::span<const uint8_t> data, size_t offset)
    {
        if (offset > data.size() || data.size() - offset < sizeof(uint16_t))
        {
            throw std::runtime_error("GRP read out of bounds");
        }
        return static_cast<uint16_t>(data[offset]) |
               (static_cast<uint16_t>(data[offset + 1]) << 8);
    }

    uint32_t ReadU32(std::span<const uint8_t> data, size_t offset)
    {
        if (offset > data.size() || data.size() - offset < sizeof(uint32_t))
        {
            throw std::runtime_error("GRP read out of bounds");
        }
        return static_cast<uint32_t>(data[offset]) |
               (static_cast<uint32_t>(data[offset + 1]) << 8) |
               (static_cast<uint32_t>(data[offset + 2]) << 16) |
               (static_cast<uint32_t>(data[offset + 3]) << 24);
    }

    std::string ReadFourCC(std::span<const uint8_t> data)
    {
        if (data.size() < 4)
        {
            throw std::runtime_error("GRP file too small");
        }
        return std::string(reinterpret_cast<const char*>(data.data()), 4);
    }

    bool IsModernGrpMagic(const std::string& magic)
    {
        return magic == "GRP2" || magic == "GRP3";
    }

    bool IsLegacyGrpMagic(std::span<const uint8_t> data)
    {
        return data.size() >= 4 &&
               data[0] == 'G' &&
               data[1] == 'R' &&
               data[2] == 'P' &&
               (data[3] == 0 || data[3] == 1 || data[3] == 2);
    }

    bool IsRangeWithin(size_t offset, size_t size, size_t begin, size_t end)
    {
        return offset >= begin && offset <= end && size <= end - offset;
    }

    bool CheckedMul(size_t lhs, size_t rhs, size_t& out)
    {
        if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs)
        {
            return false;
        }
        out = lhs * rhs;
        return true;
    }

    struct PatchableTabRef
    {
        uint32_t offset = 0;
        uint32_t count = 0;
    };

    PatchableTabRef ReadPatchableTab(std::span<const uint8_t> data, size_t offset)
    {
        PatchableTabRef ref;
        ref.offset = ReadU32(data, offset);
        ref.count = ReadU32(data, offset + 4);
        return ref;
    }

    void ValidateTable(const PatchableTabRef& ref, size_t elemSize, size_t metadataBegin, size_t metadataEnd, const char* tableName)
    {
        if (ref.count == 0)
        {
            return;
        }

        size_t byteSize = 0;
        if (!CheckedMul(static_cast<size_t>(ref.count), elemSize, byteSize) ||
            !IsRangeWithin(ref.offset, byteSize, metadataBegin, metadataEnd))
        {
            throw std::runtime_error(std::string("GRP ") + tableName + " table is out of bounds");
        }
    }

    std::string ReadCStringAt(std::span<const uint8_t> data, size_t offset, size_t metadataBegin, size_t metadataEnd)
    {
        if (!IsRangeWithin(offset, 1, metadataBegin, metadataEnd))
        {
            throw std::runtime_error("GRP name offset is out of bounds");
        }

        const size_t maxEnd = std::min(metadataEnd, offset + kMaxNameLength);
        size_t end = offset;
        while (end < maxEnd && data[end] != 0)
        {
            ++end;
        }

        if (end == metadataEnd || (end == maxEnd && data[end] != 0))
        {
            throw std::runtime_error("GRP name is not null-terminated");
        }

        return std::string(reinterpret_cast<const char*>(data.data() + offset), end - offset);
    }

    std::vector<int32_t> ReadNameMap(std::span<const uint8_t> data, const PatchableTabRef& ref)
    {
        std::vector<int32_t> names;
        names.reserve(ref.count);
        for (uint32_t i = 0; i < ref.count; ++i)
        {
            names.push_back(static_cast<int32_t>(ReadU32(data, ref.offset + static_cast<size_t>(i) * sizeof(uint32_t))));
        }
        return names;
    }

    uint32_t ComputePayloadSizeFromSortedOffsets(
        const std::vector<uint32_t>& sortedUniqueOffsets,
        uint32_t offset,
        size_t fileSize,
        size_t payloadBegin)
    {
        if (offset < payloadBegin || offset >= fileSize)
        {
            return 0;
        }

        const auto nextIt = std::upper_bound(sortedUniqueOffsets.begin(), sortedUniqueOffsets.end(), offset);
        const uint32_t nextOffset = (nextIt != sortedUniqueOffsets.end())
            ? *nextIt
            : static_cast<uint32_t>(fileSize);

        if (nextOffset <= offset)
        {
            return 0;
        }

        return nextOffset - offset;
    }

} // namespace

GrpReader::GrpReader(std::span<const uint8_t> data) : m_data(data) {}

void GrpReader::Parse()
{
    if (m_data.size() < 8)
    {
        throw std::runtime_error("GRP file too small");
    }

    m_files.clear();

    const std::string magic = ReadFourCC(m_data);
    if (IsModernGrpMagic(magic))
    {
        ParseModern();
        return;
    }

    if (IsLegacyGrpMagic(m_data))
    {
        ParseLegacy();
        return;
    }

    throw std::runtime_error("Invalid GRP magic: " + magic);
}

void GrpReader::ParseLegacy()
{
    DagorIO::MemoryReader reader(m_data);
    reader.Skip(4);

    const uint32_t fileCount = reader.ReadUInt32();
    if (fileCount > 100000)
    {
        throw std::runtime_error("GRP legacy file count is unreasonable");
    }

    m_files.reserve(fileCount);
    for (uint32_t i = 0; i < fileCount; ++i)
    {
        GrpFileInfo info;
        info.name = reader.ReadNullTerminatedString();
        info.offset = reader.ReadUInt32();
        info.size = reader.ReadUInt32();
        info.uncompressedSize = reader.ReadUInt32();
        info.flags = reader.ReadUInt32();
        info.classId = 0;
        m_files.push_back(std::move(info));
    }
}

void GrpReader::ParseModern()
{
    if (m_data.size() < kModernHeaderSize + kGrpDataHeaderSize)
    {
        throw std::runtime_error("GRP2/GRP3 file too small");
    }

    const uint32_t fullDataSize = ReadU32(m_data, 8);
    const uint32_t restFileSize = ReadU32(m_data, 12);
    if (static_cast<uint64_t>(restFileSize) + kModernHeaderSize != m_data.size())
    {
        throw std::runtime_error("GRP2/GRP3 restFileSize does not match file size");
    }

    const size_t metadataBegin = kModernHeaderSize;
    const size_t metadataEnd = kModernHeaderSize + static_cast<size_t>(fullDataSize);
    if (fullDataSize < kGrpDataHeaderSize || metadataEnd > m_data.size())
    {
        throw std::runtime_error("GRP2/GRP3 descriptor size is out of bounds");
    }

    const PatchableTabRef nameMapRef = ReadPatchableTab(m_data, kModernHeaderSize);
    const PatchableTabRef resTableRef = ReadPatchableTab(m_data, kModernHeaderSize + kPatchableTabSize);
    const PatchableTabRef resDataRef = ReadPatchableTab(m_data, kModernHeaderSize + kPatchableTabSize * 2);

    ValidateTable(nameMapRef, sizeof(uint32_t), metadataBegin, metadataEnd, "name map");
    ValidateTable(resTableRef, kResEntrySize, metadataBegin, metadataEnd, "resource");
    if (resDataRef.count > 0)
    {
        const size_t resDataElemSize = ReadFourCC(m_data) == "GRP2" ? 24 : 16;
        ValidateTable(resDataRef, resDataElemSize, metadataBegin, metadataEnd, "resource dependency");
    }

    const std::vector<int32_t> nameMap = ReadNameMap(m_data, nameMapRef);

    std::vector<uint32_t> sortedUniqueOffsets;
    sortedUniqueOffsets.reserve(resTableRef.count);
    for (uint32_t i = 0; i < resTableRef.count; ++i)
    {
        const size_t entryOffset = resTableRef.offset + static_cast<size_t>(i) * kResEntrySize;
        const uint32_t payloadOffset = ReadU32(m_data, entryOffset + 4);
        if (payloadOffset >= metadataEnd && payloadOffset < m_data.size())
        {
            sortedUniqueOffsets.push_back(payloadOffset);
        }
    }
    std::sort(sortedUniqueOffsets.begin(), sortedUniqueOffsets.end());
    sortedUniqueOffsets.erase(
        std::unique(sortedUniqueOffsets.begin(), sortedUniqueOffsets.end()),
        sortedUniqueOffsets.end());

    m_files.reserve(resTableRef.count);
    for (uint32_t i = 0; i < resTableRef.count; ++i)
    {
        const size_t entryOffset = resTableRef.offset + static_cast<size_t>(i) * kResEntrySize;
        const uint32_t classId = ReadU32(m_data, entryOffset);
        const uint32_t payloadOffset = ReadU32(m_data, entryOffset + 4);
        const uint16_t resId = ReadU16(m_data, entryOffset + 8);

        GrpFileInfo info;
        info.classId = classId;
        info.offset = payloadOffset;
        info.size = ComputePayloadSizeFromSortedOffsets(sortedUniqueOffsets, payloadOffset, m_data.size(), metadataEnd);
        info.uncompressedSize = info.size;
        info.flags = 0;

        if (resId < nameMap.size() && nameMap[resId] >= 0)
        {
            info.name = ReadCStringAt(m_data, static_cast<size_t>(nameMap[resId]), metadataBegin, metadataEnd);
        }
        else
        {
            info.name = "resource_" + std::to_string(i);
        }

        m_files.push_back(std::move(info));
    }
}

std::span<const uint8_t> GrpReader::GetFileData(const GrpFileInfo& file) const
{
    if (file.offset > m_data.size() || file.size > m_data.size() - file.offset)
    {
        throw std::runtime_error("GRP file data out of bounds");
    }
    return m_data.subspan(file.offset, file.size);
}

} // namespace DagorFormats
