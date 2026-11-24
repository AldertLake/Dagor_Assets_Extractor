#include "DxpReader.h"
#include <stdexcept>

namespace DagorFormats {

DxpReader::DxpReader(std::span<const uint8_t> data) : m_data(data) {
}

void DxpReader::Parse() {
    DagorIO::MemoryReader reader(m_data);

    if (reader.GetRemaining() < 0x48) {
        throw std::runtime_error("DXP file is too small.");
    }

    std::string magic = reader.ReadString(4);
    if (magic != "DxP2") {
        throw std::runtime_error("Invalid DXP magic, expected DxP2.");
    }

    reader.Seek(0x8);
    uint16_t totalFiles = reader.ReadUInt16();

    // Read names
    reader.Seek(0x48);
    std::vector<std::string> names;
    names.reserve(totalFiles);
    for (uint16_t i = 0; i < totalFiles; ++i) {
        names.push_back(reader.ReadNullTerminatedString());
    }

    // Read DDSX headers (0x20 bytes each)
    reader.Seek(0x20);
    uint32_t ddsBlockOffset = reader.ReadUInt32() + 0x10;
    
    std::vector<std::span<const uint8_t>> ddsxHeaders;
    ddsxHeaders.reserve(totalFiles);
    for (uint16_t i = 0; i < totalFiles; ++i) {
        reader.Seek(ddsBlockOffset + i * 0x20);
        ddsxHeaders.push_back(reader.ReadSpan(0x20));
    }

    // Read Block 3 (offsets and sizes)
    reader.Seek(0x30);
    uint32_t block3Offset = reader.ReadUInt32() + 0x10;

    m_files.clear();
    m_files.reserve(totalFiles);
    for (uint16_t i = 0; i < totalFiles; ++i) {
        size_t entryStart = block3Offset + i * 0x18;
        
        reader.Seek(entryStart + 0xC);
        uint32_t offset = reader.ReadUInt32();
        
        reader.Seek(entryStart + 0x10);
        uint32_t size = reader.ReadUInt32();

        DxpFileInfo info;
        info.name = std::move(names[i]);
        info.offset = offset;
        info.size = size;
        info.headerDdsx = ddsxHeaders[i];
        
        m_files.push_back(std::move(info));
    }
}

std::span<const uint8_t> DxpReader::GetFileData(const DxpFileInfo& fileInfo) const {
    if (fileInfo.offset + fileInfo.size > m_data.size()) {
        throw std::runtime_error("DXP file data out of bounds.");
    }
    return m_data.subspan(fileInfo.offset, fileInfo.size);
}

} // namespace DagorFormats
