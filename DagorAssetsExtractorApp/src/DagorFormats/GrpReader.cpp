#include "GrpReader.h"
#include <stdexcept>

namespace DagorFormats {

GrpReader::GrpReader(std::span<const uint8_t> data) : m_data(data) {}

void GrpReader::Parse() {
    DagorIO::MemoryReader reader(m_data);

    if (reader.GetRemaining() < 8) {
        throw std::runtime_error("GRP file too small");
    }

    // Try to read magic. Common Dagor magics are 4 bytes.
    std::string magic = reader.ReadString(4);
    if (magic != "GRP\x01" && magic != "GRP\x02" && magic != "GRP\x00") {
        // Some GRP files might not have a magic if they are just raw data
        // but typically they do if they are containers.
        // For now, let's assume it's a container if we want to parse it here.
        throw std::runtime_error("Invalid GRP magic: " + magic);
    }

    uint32_t fileCount = reader.ReadUInt32();
    m_files.clear();
    m_files.reserve(fileCount);

    // This is a placeholder structure until the exact GRP format is confirmed.
    // Based on typical Dagor container patterns:
    for (uint32_t i = 0; i < fileCount; ++i) {
        GrpFileInfo info;
        info.name = reader.ReadNullTerminatedString();
        info.offset = reader.ReadUInt32();
        info.size = reader.ReadUInt32();
        info.uncompressedSize = reader.ReadUInt32();
        info.flags = reader.ReadUInt32();
        m_files.push_back(std::move(info));
    }
}

std::span<const uint8_t> GrpReader::GetFileData(const GrpFileInfo& file) const {
    if (file.offset + file.size > m_data.size()) {
        throw std::runtime_error("GRP file data out of bounds");
    }
    return m_data.subspan(file.offset, file.size);
}

} // namespace DagorFormats
