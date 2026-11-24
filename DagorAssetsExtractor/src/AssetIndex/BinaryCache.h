#pragma once

#include "DagorIO/MappedFile.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace DagorAssetsExtractor::AssetIndex
{
    constexpr uint32_t kBinaryCacheMagic = 0x43454144u; // "DAEC" little-endian
    constexpr uint16_t kBinaryCacheFormatVersion = 2;

    enum class BinaryCacheKind : uint16_t
    {
        Index = 1,
        DescriptorLinks = 2
    };

    enum class BinaryCacheCompression : uint32_t
    {
        None = 0
    };

    struct BinaryCacheHeader
    {
        uint32_t magic = kBinaryCacheMagic;
        uint16_t formatVersion = kBinaryCacheFormatVersion;
        uint16_t kind = 0;
        uint32_t schemaVersion = 0;
        uint32_t compressionType = static_cast<uint32_t>(BinaryCacheCompression::None);
        uint64_t rootHash = 0;
        uint64_t payloadSize = 0;
        uint64_t payloadChecksum = 0;
        uint64_t reserved = 0;
    };

    static_assert(sizeof(BinaryCacheHeader) == 48, "BinaryCacheHeader must stay 48 bytes.");

    bool CheckedAdd(size_t a, size_t b, size_t& out);
    bool CheckedMul(size_t a, size_t b, size_t& out);
    uint64_t ComputeCrc64(std::span<const uint8_t> data);

    class BinaryWriter
    {
    public:
        void Reserve(size_t bytes);
        size_t Size() const { return m_buffer.size(); }
        std::span<const uint8_t> Span() const { return m_buffer; }
        std::vector<uint8_t> TakeBuffer() { return std::move(m_buffer); }

        void WriteU8(uint8_t value);
        void WriteBool(bool value);
        void WriteU16(uint16_t value);
        void WriteU32(uint32_t value);
        void WriteU64(uint64_t value);
        void WriteI64(int64_t value);
        void WriteBytes(std::span<const uint8_t> bytes);
        void WriteString(std::string_view value);

    private:
        std::vector<uint8_t> m_buffer;
    };

    class BinaryCursor
    {
    public:
        explicit BinaryCursor(std::span<const uint8_t> data)
            : m_data(data)
        {
        }

        size_t Remaining() const { return m_data.size() - m_offset; }
        size_t Offset() const { return m_offset; }
        bool IsAtEnd() const { return m_offset == m_data.size(); }

        bool ReadU8(uint8_t& out);
        bool ReadBool(bool& out);
        bool ReadU16(uint16_t& out);
        bool ReadU32(uint32_t& out);
        bool ReadU64(uint64_t& out);
        bool ReadI64(int64_t& out);
        bool ReadBytes(size_t count, std::span<const uint8_t>& out);
        bool ReadStringView(std::string_view& out);
        bool ReadString(std::string& out);

    private:
        std::span<const uint8_t> m_data;
        size_t m_offset = 0;
    };

    class StringTableBuilder
    {
    public:
        uint32_t Intern(std::string_view value);
        size_t Count() const { return m_offsetsAndLengths.size(); }
        bool Write(BinaryWriter& writer, std::string& error) const;

    private:
        struct TransparentHash
        {
            using is_transparent = void;
            size_t operator()(std::string_view sv) const noexcept
            {
                return std::hash<std::string_view>{}(sv);
            }
        };

        std::vector<uint32_t> m_offsetsAndLengths;
        std::vector<char> m_blob;
        std::unordered_map<std::string, uint32_t, TransparentHash, std::equal_to<>> m_lookup;
    };

    bool ReadStringTable(
        BinaryCursor& cursor,
        std::vector<std::string_view>& outEntries,
        std::string& error);

    struct BinaryCacheOpenResult
    {
        bool fileFound = false;
        bool isBinaryMagic = false;
        bool corrupt = false;
        bool valid = false;
        BinaryCacheHeader header;
        DagorIO::MappedFile mapped;
        std::span<const uint8_t> payload;
        std::string error;
    };

    bool WriteBinaryCacheFile(
        const std::filesystem::path& cachePath,
        BinaryCacheKind kind,
        uint32_t schemaVersion,
        const std::filesystem::path& gameRoot,
        std::span<const uint8_t> payload,
        std::string& error);

    BinaryCacheOpenResult OpenBinaryCacheFile(
        const std::filesystem::path& cachePath,
        BinaryCacheKind expectedKind,
        uint32_t expectedSchemaVersion);
}
