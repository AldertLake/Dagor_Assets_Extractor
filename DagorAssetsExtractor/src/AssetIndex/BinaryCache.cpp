#include "AssetIndex/BinaryCache.h"

#include "Platform/Platform.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <thread>

namespace DagorAssetsExtractor::AssetIndex
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr uint64_t kCrc64EcmaPoly = 0xC96C5795D7870F42ull;

        const std::array<uint64_t, 256>& GetCrc64Table()
        {
            static const std::array<uint64_t, 256> kTable = []() {
                std::array<uint64_t, 256> table = {};
                for (uint64_t i = 0; i < 256; ++i)
                {
                    uint64_t crc = i;
                    for (int bit = 0; bit < 8; ++bit)
                    {
                        if ((crc & 1ull) != 0)
                        {
                            crc = (crc >> 1) ^ kCrc64EcmaPoly;
                        }
                        else
                        {
                            crc >>= 1;
                        }
                    }
                    table[static_cast<size_t>(i)] = crc;
                }
                return table;
            }();
            return kTable;
        }

        uint16_t ReadLe16(std::span<const uint8_t> data, size_t offset)
        {
            return static_cast<uint16_t>(data[offset]) |
                   static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1]) << 8);
        }

        uint32_t ReadLe32(std::span<const uint8_t> data, size_t offset)
        {
            return static_cast<uint32_t>(data[offset]) |
                   (static_cast<uint32_t>(data[offset + 1]) << 8) |
                   (static_cast<uint32_t>(data[offset + 2]) << 16) |
                   (static_cast<uint32_t>(data[offset + 3]) << 24);
        }

        uint64_t ReadLe64(std::span<const uint8_t> data, size_t offset)
        {
            return static_cast<uint64_t>(ReadLe32(data, offset)) |
                   (static_cast<uint64_t>(ReadLe32(data, offset + 4)) << 32);
        }

        void AppendLe16(std::vector<uint8_t>& out, uint16_t value)
        {
            out.push_back(static_cast<uint8_t>(value & 0xFFu));
            out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
        }

        void AppendLe32(std::vector<uint8_t>& out, uint32_t value)
        {
            out.push_back(static_cast<uint8_t>(value & 0xFFu));
            out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
            out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
            out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
        }

        void AppendLe64(std::vector<uint8_t>& out, uint64_t value)
        {
            AppendLe32(out, static_cast<uint32_t>(value & 0xFFFFFFFFull));
            AppendLe32(out, static_cast<uint32_t>((value >> 32) & 0xFFFFFFFFull));
        }

        bool ReplaceCacheFileAtomic(const fs::path& tempPath, const fs::path& targetPath, std::string& error)
        {
#if defined(_WIN32)
            if (MoveFileExW(
                    tempPath.wstring().c_str(),
                    targetPath.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
            {
                return true;
            }

            const std::error_code ec(static_cast<int>(GetLastError()), std::system_category());
            error = "Could not replace binary cache file: " + ec.message();
            return false;
#else
            std::error_code ec;
            fs::rename(tempPath, targetPath, ec);
            if (!ec)
            {
                return true;
            }

            fs::remove(targetPath, ec);
            ec.clear();
            fs::rename(tempPath, targetPath, ec);
            if (ec)
            {
                error = "Could not replace binary cache file: " + ec.message();
                return false;
            }
            return true;
#endif
        }
    }

    bool CheckedAdd(size_t a, size_t b, size_t& out)
    {
        if (b > std::numeric_limits<size_t>::max() - a)
        {
            return false;
        }
        out = a + b;
        return true;
    }

    bool CheckedMul(size_t a, size_t b, size_t& out)
    {
        if (a != 0 && b > std::numeric_limits<size_t>::max() / a)
        {
            return false;
        }
        out = a * b;
        return true;
    }

    uint64_t ComputeCrc64(std::span<const uint8_t> data)
    {
        const auto& table = GetCrc64Table();
        uint64_t crc = 0xFFFFFFFFFFFFFFFFull;
        for (const uint8_t byte : data)
        {
            crc = table[(crc ^ byte) & 0xFFu] ^ (crc >> 8);
        }
        return crc ^ 0xFFFFFFFFFFFFFFFFull;
    }

    void BinaryWriter::Reserve(size_t bytes)
    {
        m_buffer.reserve(bytes);
    }

    void BinaryWriter::WriteU8(uint8_t value)
    {
        m_buffer.push_back(value);
    }

    void BinaryWriter::WriteBool(bool value)
    {
        m_buffer.push_back(value ? 1u : 0u);
    }

    void BinaryWriter::WriteU16(uint16_t value)
    {
        AppendLe16(m_buffer, value);
    }

    void BinaryWriter::WriteU32(uint32_t value)
    {
        AppendLe32(m_buffer, value);
    }

    void BinaryWriter::WriteU64(uint64_t value)
    {
        AppendLe64(m_buffer, value);
    }

    void BinaryWriter::WriteI64(int64_t value)
    {
        AppendLe64(m_buffer, static_cast<uint64_t>(value));
    }

    void BinaryWriter::WriteBytes(std::span<const uint8_t> bytes)
    {
        if (!bytes.empty())
        {
            m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());
        }
    }

    void BinaryWriter::WriteString(std::string_view value)
    {
        const uint32_t len = static_cast<uint32_t>(
            std::min<size_t>(value.size(), std::numeric_limits<uint32_t>::max()));
        WriteU32(len);
        if (len > 0)
        {
            const uint8_t* ptr = reinterpret_cast<const uint8_t*>(value.data());
            m_buffer.insert(m_buffer.end(), ptr, ptr + len);
        }
    }

    bool BinaryCursor::ReadU8(uint8_t& out)
    {
        if (Remaining() < 1)
        {
            return false;
        }
        out = m_data[m_offset++];
        return true;
    }

    bool BinaryCursor::ReadBool(bool& out)
    {
        uint8_t value = 0;
        if (!ReadU8(value))
        {
            return false;
        }
        out = value != 0;
        return true;
    }

    bool BinaryCursor::ReadU16(uint16_t& out)
    {
        if (Remaining() < sizeof(uint16_t))
        {
            return false;
        }
        out = ReadLe16(m_data, m_offset);
        m_offset += sizeof(uint16_t);
        return true;
    }

    bool BinaryCursor::ReadU32(uint32_t& out)
    {
        if (Remaining() < sizeof(uint32_t))
        {
            return false;
        }
        out = ReadLe32(m_data, m_offset);
        m_offset += sizeof(uint32_t);
        return true;
    }

    bool BinaryCursor::ReadU64(uint64_t& out)
    {
        if (Remaining() < sizeof(uint64_t))
        {
            return false;
        }
        out = ReadLe64(m_data, m_offset);
        m_offset += sizeof(uint64_t);
        return true;
    }

    bool BinaryCursor::ReadI64(int64_t& out)
    {
        uint64_t raw = 0;
        if (!ReadU64(raw))
        {
            return false;
        }
        out = static_cast<int64_t>(raw);
        return true;
    }

    bool BinaryCursor::ReadBytes(size_t count, std::span<const uint8_t>& out)
    {
        if (Remaining() < count)
        {
            return false;
        }
        out = m_data.subspan(m_offset, count);
        m_offset += count;
        return true;
    }

    bool BinaryCursor::ReadStringView(std::string_view& out)
    {
        uint32_t len = 0;
        if (!ReadU32(len))
        {
            return false;
        }
        std::span<const uint8_t> bytes;
        if (!ReadBytes(len, bytes))
        {
            return false;
        }
        out = std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        return true;
    }

    bool BinaryCursor::ReadString(std::string& out)
    {
        std::string_view sv;
        if (!ReadStringView(sv))
        {
            return false;
        }
        out.assign(sv.data(), sv.size());
        return true;
    }

    uint32_t StringTableBuilder::Intern(std::string_view value)
    {
        if (const auto it = m_lookup.find(value); it != m_lookup.end())
        {
            return it->second;
        }

        const uint32_t id = static_cast<uint32_t>(m_offsetsAndLengths.size());
        const uint32_t len = static_cast<uint32_t>(
            std::min<size_t>(value.size(), std::numeric_limits<uint32_t>::max()));
        m_offsetsAndLengths.push_back(len);
        m_blob.insert(m_blob.end(), value.data(), value.data() + len);
        m_lookup.emplace(std::string(value.data(), len), id);
        return id;
    }

    bool StringTableBuilder::Write(BinaryWriter& writer, std::string& error) const
    {
        if (m_offsetsAndLengths.size() > std::numeric_limits<uint32_t>::max() ||
            m_blob.size() > std::numeric_limits<uint32_t>::max())
        {
            error = "Binary cache string table exceeded 32-bit size bounds.";
            return false;
        }

        writer.WriteU32(static_cast<uint32_t>(m_offsetsAndLengths.size()));
        writer.WriteU32(static_cast<uint32_t>(m_blob.size()));
        for (const uint32_t len : m_offsetsAndLengths)
        {
            writer.WriteU32(len);
        }
        if (!m_blob.empty())
        {
            writer.WriteBytes(std::span<const uint8_t>(
                reinterpret_cast<const uint8_t*>(m_blob.data()),
                m_blob.size()));
        }
        return true;
    }

    bool ReadStringTable(
        BinaryCursor& cursor,
        std::vector<std::string_view>& outEntries,
        std::string& error)
    {
        uint32_t count = 0;
        uint32_t blobBytes = 0;
        if (!cursor.ReadU32(count) || !cursor.ReadU32(blobBytes))
        {
            error = "Binary cache string table header is truncated.";
            return false;
        }

        size_t lengthsBytes = 0;
        if (!CheckedMul(static_cast<size_t>(count), sizeof(uint32_t), lengthsBytes))
        {
            error = "Binary cache string table count overflows.";
            return false;
        }

        std::span<const uint8_t> lengthsSpan;
        if (!cursor.ReadBytes(lengthsBytes, lengthsSpan))
        {
            error = "Binary cache string table lengths are truncated.";
            return false;
        }

        std::span<const uint8_t> blobSpan;
        if (!cursor.ReadBytes(blobBytes, blobSpan))
        {
            error = "Binary cache string table blob is truncated.";
            return false;
        }

        outEntries.clear();
        outEntries.reserve(count);
        size_t offset = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t len = ReadLe32(lengthsSpan, static_cast<size_t>(i) * sizeof(uint32_t));
            size_t nextOffset = 0;
            if (!CheckedAdd(offset, static_cast<size_t>(len), nextOffset) || nextOffset > blobSpan.size())
            {
                error = "Binary cache string table entry is out of bounds or corrupted.";
                return false;
            }
            outEntries.emplace_back(
                reinterpret_cast<const char*>(blobSpan.data() + offset),
                static_cast<size_t>(len));
            offset = nextOffset;
        }

        if (offset != blobSpan.size())
        {
            error = "Binary cache string table size mismatch (corrupted).";
            return false;
        }

        return true;
    }

    bool WriteBinaryCacheFile(
        const fs::path& cachePath,
        BinaryCacheKind kind,
        uint32_t schemaVersion,
        const fs::path& gameRoot,
        std::span<const uint8_t> payload,
        std::string& error)
    {
        if (cachePath.empty())
        {
            error = "Cache path is empty.";
            return false;
        }

        std::error_code ec;
        if (cachePath.has_parent_path())
        {
            fs::create_directories(cachePath.parent_path(), ec);
            if (ec)
            {
                error = "Could not create binary cache folder: " + ec.message();
                return false;
            }
        }

        BinaryCacheHeader header;
        header.magic = kBinaryCacheMagic;
        header.formatVersion = kBinaryCacheFormatVersion;
        header.kind = static_cast<uint16_t>(kind);
        header.schemaVersion = schemaVersion;
        header.compressionType = static_cast<uint32_t>(BinaryCacheCompression::None);
        header.rootHash = Platform::ComputeFnv1a64(Platform::CanonicalizePathUtf8(gameRoot));
        header.payloadSize = static_cast<uint64_t>(payload.size());
        header.payloadChecksum = ComputeCrc64(payload);
        header.reserved = 0;

        BinaryWriter headerWriter;
        headerWriter.Reserve(sizeof(BinaryCacheHeader));
        headerWriter.WriteU32(header.magic);
        headerWriter.WriteU16(header.formatVersion);
        headerWriter.WriteU16(header.kind);
        headerWriter.WriteU32(header.schemaVersion);
        headerWriter.WriteU32(header.compressionType);
        headerWriter.WriteU64(header.rootHash);
        headerWriter.WriteU64(header.payloadSize);
        headerWriter.WriteU64(header.payloadChecksum);
        headerWriter.WriteU64(header.reserved);

        const auto tidHash = std::hash<std::thread::id>{}(std::this_thread::get_id());
        fs::path tempPath = cachePath;
        tempPath += L".tmp." + std::to_wstring(tidHash);

        {
            std::ofstream file(tempPath, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                error = "Could not open temporary binary cache file for writing.";
                return false;
            }

            const std::span<const uint8_t> hdrSpan = headerWriter.Span();
            file.write(reinterpret_cast<const char*>(hdrSpan.data()), static_cast<std::streamsize>(hdrSpan.size()));
            if (!payload.empty())
            {
                file.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
            }
            file.flush();
            if (!file)
            {
                file.close();
                fs::remove(tempPath, ec);
                error = "Failed while writing binary cache file.";
                return false;
            }
        }

        if (!ReplaceCacheFileAtomic(tempPath, cachePath, error))
        {
            fs::remove(tempPath, ec);
            return false;
        }

        return true;
    }

    BinaryCacheOpenResult OpenBinaryCacheFile(
        const fs::path& cachePath,
        BinaryCacheKind expectedKind,
        uint32_t expectedSchemaVersion)
    {
        BinaryCacheOpenResult result;

        std::error_code ec;
        if (cachePath.empty() || !fs::exists(cachePath, ec) || ec)
        {
            result.error = "No cache file found.";
            return result;
        }

        result.fileFound = true;

        std::string mapError;
        if (!result.mapped.Open(cachePath, mapError))
        {
            result.error = mapError;
            return result;
        }

        const std::span<const uint8_t> bytes = result.mapped.Span();
        if (bytes.size() < 4)
        {
            result.corrupt = true;
            result.error = "Cache file is truncated.";
            return result;
        }

        const uint32_t magic = ReadLe32(bytes, 0);
        if (magic != kBinaryCacheMagic)
        {
            result.isBinaryMagic = false;
            result.error = "Not a DAEC binary cache file.";
            return result;
        }

        result.isBinaryMagic = true;
        if (bytes.size() < sizeof(BinaryCacheHeader))
        {
            result.corrupt = true;
            result.error = "Binary cache header is truncated.";
            return result;
        }

        result.header.magic = magic;
        result.header.formatVersion = ReadLe16(bytes, 4);
        result.header.kind = ReadLe16(bytes, 6);
        result.header.schemaVersion = ReadLe32(bytes, 8);
        result.header.compressionType = ReadLe32(bytes, 12);
        result.header.rootHash = ReadLe64(bytes, 16);
        result.header.payloadSize = ReadLe64(bytes, 24);
        result.header.payloadChecksum = ReadLe64(bytes, 32);
        result.header.reserved = ReadLe64(bytes, 40);

        if (result.header.formatVersion != kBinaryCacheFormatVersion ||
            result.header.kind != static_cast<uint16_t>(expectedKind))
        {
            result.error = "Binary cache has an unsupported format version or kind.";
            return result;
        }

        if (result.header.schemaVersion != expectedSchemaVersion)
        {
            result.error = "Binary cache schema version is outdated.";
            return result;
        }

        if (result.header.compressionType != static_cast<uint32_t>(BinaryCacheCompression::None))
        {
            result.error = "Binary cache compression mode is unsupported.";
            return result;
        }

        const size_t availablePayload = bytes.size() - sizeof(BinaryCacheHeader);
        if (result.header.payloadSize != static_cast<uint64_t>(availablePayload))
        {
            result.corrupt = true;
            result.error = "Binary cache payload is truncated or size mismatch.";
            return result;
        }

        const std::span<const uint8_t> payload = bytes.subspan(sizeof(BinaryCacheHeader));
        const uint64_t actualChecksum = ComputeCrc64(payload);
        if (actualChecksum != result.header.payloadChecksum)
        {
            result.corrupt = true;
            result.error = "Binary cache payload checksum mismatch (corrupted).";
            return result;
        }

        result.payload = payload;
        result.valid = true;
        return result;
    }
}
