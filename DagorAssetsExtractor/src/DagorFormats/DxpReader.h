#pragma once
#include "DagorIO/BinaryReader.h"
#include <string>
#include <vector>
#include <span>

namespace DagorFormats {

struct DxpFileInfo {
    std::string name;
    uint32_t offset;
    uint32_t size;
    std::span<const uint8_t> headerDdsx; // 32 bytes (0x20)
};

class DxpReader {
public:
    explicit DxpReader(std::span<const uint8_t> data);

    void Parse();

    const std::vector<DxpFileInfo>& GetFiles() const { return m_files; }
    std::span<const uint8_t> GetFileData(const DxpFileInfo& fileInfo) const;

private:
    std::span<const uint8_t> m_data;
    std::vector<DxpFileInfo> m_files;
};

} // namespace DagorFormats
