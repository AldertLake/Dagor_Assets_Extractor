#pragma once
#include "DagorIO/BinaryReader.h"
#include <string>
#include <vector>
#include <span>

namespace DagorFormats {

struct VromfsFileInfo {
    std::string name;
    uint32_t offset;
    uint32_t size;
};

class VromfsReader {
public:
    VromfsReader(std::span<const uint8_t> data);

    void Parse();

    const std::vector<VromfsFileInfo>& GetFiles() const { return m_files; }
    std::span<const uint8_t> GetFileData(const VromfsFileInfo& fileInfo) const;

private:
    std::span<const uint8_t> m_data;
    std::vector<uint8_t> m_decompressedData;
    std::vector<VromfsFileInfo> m_files;
};

} // namespace DagorFormats
