#include "VromfsReader.h"
#include <stdexcept>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <array>
#include <vector>
#include <cstring>
#include "DagorIO/Decompression.h"

namespace DagorFormats {

namespace {
    uint32_t ReadUInt32LE(const uint8_t* data) {
        return static_cast<uint32_t>(data[0]) |
            (static_cast<uint32_t>(data[1]) << 8) |
            (static_cast<uint32_t>(data[2]) << 16) |
            (static_cast<uint32_t>(data[3]) << 24);
    }

    void AppendUInt32LE(std::vector<uint8_t>& out, uint32_t value) {
        out.push_back(static_cast<uint8_t>(value & 0xFFu));
        out.push_back(static_cast<uint8_t>((value >> 8) & 0xFFu));
        out.push_back(static_cast<uint8_t>((value >> 16) & 0xFFu));
        out.push_back(static_cast<uint8_t>((value >> 24) & 0xFFu));
    }

    void DecodeObfuscatedBlock16(
        std::span<const uint8_t> src,
        const std::array<uint32_t, 4>& masks,
        std::vector<uint8_t>& out)
    {
        for (size_t i = 0; i < 4; ++i) {
            const uint32_t decoded = ReadUInt32LE(src.data() + i * 4) ^ masks[i];
            AppendUInt32LE(out, decoded);
        }
    }

    std::vector<uint8_t> RebuildVromfsZstdStream(std::span<const uint8_t> packedData) {
        if (packedData.size() < 16) {
            return std::vector<uint8_t>(packedData.begin(), packedData.end());
        }

        static constexpr std::array<uint32_t, 4> kFirstMasks = {
            0xAA55AA55u, 0xF00FF00Fu, 0xAA55AA55u, 0x12481248u
        };
        static constexpr std::array<uint32_t, 4> kSecondMasks = {
            0x12481248u, 0xAA55AA55u, 0xF00FF00Fu, 0xAA55AA55u
        };

        std::vector<uint8_t> rebuilt;
        rebuilt.reserve(packedData.size());

        DecodeObfuscatedBlock16(packedData.first<16>(), kFirstMasks, rebuilt);

        if (packedData.size() >= 32) {
            const size_t middleAlignedSize = ((packedData.size() - 32) / 4) * 4;
            rebuilt.insert(rebuilt.end(), packedData.begin() + 16, packedData.begin() + 16 + middleAlignedSize);
            DecodeObfuscatedBlock16(
                std::span<const uint8_t>(packedData.data() + 16 + middleAlignedSize, 16),
                kSecondMasks,
                rebuilt);

            const size_t tailOffset = 16 + middleAlignedSize + 16;
            rebuilt.insert(rebuilt.end(), packedData.begin() + tailOffset, packedData.end());
            return rebuilt;
        }

        const size_t middleAlignedSize = ((packedData.size() - 16) / 4) * 4;
        rebuilt.insert(rebuilt.end(), packedData.begin() + 16, packedData.begin() + 16 + middleAlignedSize);
        rebuilt.insert(rebuilt.end(), packedData.begin() + 16 + middleAlignedSize, packedData.end());
        return rebuilt;
    }
}

VromfsReader::VromfsReader(std::span<const uint8_t> data) : m_data(data) {}

void VromfsReader::AddDiagnostic(std::string message) {
    m_diagnostics.push_back(message);
    std::clog << "[VROMFS] " << message << '\n';
}

void VromfsReader::Parse() {
    m_files.clear();
    m_diagnostics.clear();

    DagorIO::MemoryReader reader(m_data);
    const size_t inputSize = m_data.size();
    const auto formatAscii = [](std::string_view value) {
        std::string out;
        out.reserve(value.size());
        for (unsigned char c : value) {
            out.push_back(std::isprint(c) != 0 ? static_cast<char>(c) : '.');
        }
        return out;
    };
    AddDiagnostic("Begin parse: input bytes=" + std::to_string(inputSize));

    if (reader.GetRemaining() < 16) {
        throw std::runtime_error("File too small to be VROMFS");
    }

    std::string magic = reader.ReadString(4);
    if (magic != "VRFs" && magic != "VRFx") {
        throw std::runtime_error("Invalid VROMFS magic");
    }

    std::string platform = reader.ReadString(4);
    uint32_t originalSize = reader.ReadUInt32();
    
    const uint32_t packedAndType = reader.ReadUInt32();
    const uint32_t packedSizeLow24 = packedAndType & 0x00FFFFFFu;
    const uint8_t type = static_cast<uint8_t>((packedAndType >> 24) & 0xFFu);
    const uint8_t typeClass = type & 0xC0u;
    const uint8_t packedSizeHighBits = type & 0x3Fu;
    // Dagor VROMFS stores the packed body size across the low 24 header bits and
    // the low 6 bits of the type byte. Large archives such as aces.vromfs.bin use
    // those extra bits once the packed body grows past 16 MiB.
    const uint32_t packedSize = packedSizeLow24 | (static_cast<uint32_t>(packedSizeHighBits) << 24);
    AddDiagnostic(
        "Header: magic=" + magic +
        ", platform=" + formatAscii(platform) +
        ", originalSize=" + std::to_string(originalSize) +
        ", packedSize=" + std::to_string(packedSize) +
        ", packedSizeLow24=" + std::to_string(packedSizeLow24) +
        ", packedSizeHighBits=" + std::to_string(packedSizeHighBits) +
        ", type=0x" + [&]() {
            std::ostringstream out;
            out << std::hex << std::uppercase << static_cast<unsigned int>(type);
            return out.str();
        }() +
        ", typeClass=0x" + [&]() {
            std::ostringstream out;
            out << std::hex << std::uppercase << static_cast<unsigned int>(typeClass);
            return out.str();
        }() +
        ", typeLowBits=0x" + [&]() {
            std::ostringstream out;
            out << std::hex << std::uppercase << static_cast<unsigned int>(packedSizeHighBits);
            return out.str();
        }());

    if (magic == "VRFx") {
        if (reader.GetRemaining() < 8) {
            throw std::runtime_error("Unexpected end of file reading VRFx header");
        }
        reader.Skip(8); // ext_header: size, flags, version
        AddDiagnostic("Skipped VRFx extended header (8 bytes).");
    }

    bool isPacked = false;
    size_t bodyBaseOffset = reader.GetPosition();
    if (typeClass == 0x80 && packedSize > 0) {
        isPacked = true;
        // Zlib
        auto compressedData = reader.ReadSpan(packedSize);
        AddDiagnostic("Detected zlib-packed VROMFS body. Compressed bytes=" + std::to_string(compressedData.size()));
        m_decompressedData = DagorIO::Decompressor::DecompressZlib(compressedData, originalSize);
        m_data = m_decompressedData;
        reader = DagorIO::MemoryReader(m_data);
        bodyBaseOffset = 0;
        AddDiagnostic("Decompressed zlib body to " + std::to_string(m_data.size()) + " bytes.");
    } else if (typeClass == 0xC0) {
        isPacked = true;
        // Zstd
        auto compressedData = reader.ReadSpan(packedSize);
        AddDiagnostic("Detected zstd-packed VROMFS body. Compressed bytes=" + std::to_string(compressedData.size()));

        std::vector<uint8_t> rebuiltStream = RebuildVromfsZstdStream(compressedData);
        const bool rebuiltChanged =
            rebuiltStream.size() != compressedData.size() ||
            !std::equal(rebuiltStream.begin(), rebuiltStream.end(), compressedData.begin(), compressedData.end());
        if (rebuiltChanged) {
            AddDiagnostic("Rebuilt obfuscated zstd body before decompression.");
        }

        try {
            m_decompressedData = DagorIO::Decompressor::DecompressZstd(rebuiltStream, originalSize);
        } catch (const std::exception& rebuiltError) {
            if (!rebuiltChanged) {
                throw;
            }

            AddDiagnostic(
                std::string("Rebuilt zstd body failed to decompress: ") + rebuiltError.what() +
                ". Retrying raw packed body.");
            m_decompressedData = DagorIO::Decompressor::DecompressZstd(compressedData, originalSize);
        }

        m_data = m_decompressedData;
        reader = DagorIO::MemoryReader(m_data);
        bodyBaseOffset = 0;
        AddDiagnostic("Decompressed zstd body to " + std::to_string(m_data.size()) + " bytes.");
    }
    else {
        AddDiagnostic(
            "Body is not additionally packed. Body base offset=" + std::to_string(bodyBaseOffset) +
            ", file bytes=" + std::to_string(m_data.size()) + '.');
    }

    if (reader.GetRemaining() < 16) {
        AddDiagnostic("Body has fewer than 16 bytes after header handling; treating as empty.");
        return; // Empty body
    }

    const uint32_t filenameTableOffsetRaw = reader.ReadUInt32();
    uint32_t filesCount = reader.ReadUInt32();
    reader.Skip(8);
    const uint32_t filedataTableOffsetRaw = reader.ReadUInt32();

    auto resolveOffset = [&](uint32_t rawOffset, std::string_view label) -> size_t
    {
        const size_t absoluteOffset = static_cast<size_t>(rawOffset);
        const size_t relativeOffset = bodyBaseOffset + absoluteOffset;
        const bool relativeValid = relativeOffset <= m_data.size();
        const bool absoluteValid = absoluteOffset <= m_data.size();

        if (relativeValid) {
            if (bodyBaseOffset != 0 && absoluteValid && absoluteOffset != relativeOffset) {
                AddDiagnostic(
                    std::string(label) + " resolved as body-relative: raw=" + std::to_string(rawOffset) +
                    ", absoluteCandidate=" + std::to_string(absoluteOffset) +
                    ", chosen=" + std::to_string(relativeOffset));
            }
            return relativeOffset;
        }

        if (absoluteValid) {
            AddDiagnostic(
                std::string(label) + " fell back to absolute offset: raw=" + std::to_string(rawOffset) +
                ", bodyBase=" + std::to_string(bodyBaseOffset));
            return absoluteOffset;
        }

        throw std::runtime_error(
            std::string(label) + " offset " + std::to_string(rawOffset) +
            " is outside parsed data (bodyBase=" + std::to_string(bodyBaseOffset) +
            ", dataSize=" + std::to_string(m_data.size()) + ").");
    };

    const size_t filenameTableOffset = resolveOffset(filenameTableOffsetRaw, "filenameTable");
    const size_t filedataTableOffset = resolveOffset(filedataTableOffsetRaw, "filedataTable");
    AddDiagnostic(
        "Directory tables: filesCount=" + std::to_string(filesCount) +
        ", filenameTableRaw=" + std::to_string(filenameTableOffsetRaw) +
        ", filenameTableResolved=" + std::to_string(filenameTableOffset) +
        ", filedataTableRaw=" + std::to_string(filedataTableOffsetRaw) +
        ", filedataTableResolved=" + std::to_string(filedataTableOffset));

    if (filesCount == 0) {
        AddDiagnostic("VROMFS contains zero files.");
        return;
    }

    reader.Seek(filenameTableOffset);
    const uint32_t firstFilenameOffsetRaw = reader.ReadUInt32();
    const size_t firstFilenameOffset = resolveOffset(firstFilenameOffsetRaw, "firstFilename");
    AddDiagnostic(
        "Filename table: firstFilenameRaw=" + std::to_string(firstFilenameOffsetRaw) +
        ", firstFilenameResolved=" + std::to_string(firstFilenameOffset));

    std::vector<std::string> filenames;
    filenames.reserve(filesCount);
    reader.Seek(firstFilenameOffset);
    for (uint32_t i = 0; i < filesCount; ++i) {
        filenames.push_back(reader.ReadNullTerminatedString());
        if (i < 8) {
            AddDiagnostic("Filename[" + std::to_string(i) + "]=" + filenames.back());
        }
    }

    reader.Seek(filedataTableOffset);
    m_files.reserve(filesCount);
    for (uint32_t i = 0; i < filesCount; ++i) {
        const uint32_t fileOffsetRaw = reader.ReadUInt32();
        const uint32_t fileSize = reader.ReadUInt32();
        reader.Skip(8); // unknown record tail

        const size_t fileOffset = resolveOffset(fileOffsetRaw, "fileData[" + std::to_string(i) + "]");
        if (fileOffset > m_data.size() || static_cast<uint64_t>(fileOffset) + fileSize > m_data.size()) {
            throw std::runtime_error(
                "File record " + std::to_string(i) + " is out of bounds: offset=" + std::to_string(fileOffset) +
                ", size=" + std::to_string(fileSize) + ", dataSize=" + std::to_string(m_data.size()) + '.');
        }

        if (i < 8) {
            AddDiagnostic(
                "File[" + std::to_string(i) + "]: name=" + filenames[i] +
                ", offsetRaw=" + std::to_string(fileOffsetRaw) +
                ", offsetResolved=" + std::to_string(fileOffset) +
                ", size=" + std::to_string(fileSize));
        }

        m_files.push_back({filenames[i], static_cast<uint32_t>(fileOffset), fileSize});
    }

    AddDiagnostic("Parse complete: files=" + std::to_string(m_files.size()) + ", packed=" + std::string(isPacked ? "true" : "false"));
}

std::span<const uint8_t> VromfsReader::GetFileData(const VromfsFileInfo& fileInfo) const {
    if (fileInfo.offset + fileInfo.size > m_data.size()) {
        throw std::runtime_error("File data out of bounds");
    }
    return m_data.subspan(fileInfo.offset, fileInfo.size);
}

} // namespace DagorFormats
