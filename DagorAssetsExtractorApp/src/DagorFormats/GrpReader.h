#pragma once
#include "DagorIO/BinaryReader.h"
#include <cstdint>
#include <string>
#include <vector>
#include <span>

namespace DagorFormats {

struct GrpFileInfo {
    std::string name;
    uint32_t offset;
    uint32_t size;
    uint32_t uncompressedSize;
    uint32_t flags;
    uint32_t classId;
};

class GrpReader {
public:
    explicit GrpReader(std::span<const uint8_t> data);

    void Parse();

    const std::vector<GrpFileInfo>& GetFiles() const { return m_files; }
    
    std::span<const uint8_t> GetFileData(const GrpFileInfo& file) const;

private:
    void ParseLegacy();
    void ParseModern();

    std::span<const uint8_t> m_data;
    std::vector<GrpFileInfo> m_files;
};

} // namespace DagorFormats
