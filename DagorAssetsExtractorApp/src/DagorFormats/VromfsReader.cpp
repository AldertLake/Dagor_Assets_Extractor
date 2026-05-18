#include "VromfsReader.h"
#include <stdexcept>
#include "DagorIO/Decompression.h"

namespace DagorFormats {

VromfsReader::VromfsReader(std::span<const uint8_t> data) : m_data(data) {}

void VromfsReader::Parse() {
    DagorIO::MemoryReader reader(m_data);

    if (reader.GetRemaining() < 16) {
        throw std::runtime_error("File too small to be VROMFS");
    }

    std::string magic = reader.ReadString(4);
    if (magic != "VRFs" && magic != "VRFx") {
        throw std::runtime_error("Invalid VROMFS magic");
    }

    std::string platform = reader.ReadString(4);
    uint32_t originalSize = reader.ReadUInt32();
    
    uint32_t packedAndType = reader.ReadUInt32();
    uint32_t packedSize = packedAndType & 0x00FFFFFF;
    uint8_t type = (packedAndType >> 24) & 0xFF;

    if (magic == "VRFx") {
        if (reader.GetRemaining() < 8) {
            throw std::runtime_error("Unexpected end of file reading VRFx header");
        }
        reader.Skip(8); // ext_header: size, flags, version
    }

    bool isPacked = false;
    if (type == 0x80 && packedSize > 0) {
        isPacked = true;
        // Zlib
        auto compressedData = reader.ReadSpan(packedSize);
        m_decompressedData = DagorIO::Decompressor::DecompressZlib(compressedData, originalSize);
        m_data = m_decompressedData;
        reader = DagorIO::MemoryReader(m_data);
    } else if (type == 0xc0) {
        isPacked = true;
        // Zstd
        auto compressedData = reader.ReadSpan(packedSize);
        m_decompressedData = DagorIO::Decompressor::DecompressZstd(compressedData, originalSize);
        m_data = m_decompressedData;
        reader = DagorIO::MemoryReader(m_data);
    }

    if (reader.GetRemaining() < 16) {
        return; // Empty body
    }

    uint32_t filenameTableOffset = reader.ReadUInt32();
    uint32_t filesCount = reader.ReadUInt32();
    reader.Skip(8);
    uint32_t filedataTableOffset = reader.ReadUInt32();

    if (filesCount == 0) {
        return;
    }

    std::vector<uint32_t> filenameOffsets;
    filenameOffsets.reserve(filesCount);

    reader.Seek(filenameTableOffset);
    for (uint32_t i = 0; i < filesCount; ++i) {
        filenameOffsets.push_back(reader.ReadUInt32());
    }

    std::vector<std::string> filenames;
    filenames.reserve(filesCount);
    for (uint32_t i = 0; i < filesCount; ++i) {
        reader.Seek(filenameOffsets[i]);
        filenames.push_back(reader.ReadNullTerminatedString());
    }

    reader.Seek(filedataTableOffset);
    m_files.reserve(filesCount);
    for (uint32_t i = 0; i < filesCount; ++i) {
        uint32_t fileOffset = reader.ReadUInt32();
        uint32_t fileSize = reader.ReadUInt32();
        m_files.push_back({filenames[i], fileOffset, fileSize});
    }
}

std::span<const uint8_t> VromfsReader::GetFileData(const VromfsFileInfo& fileInfo) const {
    if (fileInfo.offset + fileInfo.size > m_data.size()) {
        throw std::runtime_error("File data out of bounds");
    }
    return m_data.subspan(fileInfo.offset, fileInfo.size);
}

} // namespace DagorFormats
