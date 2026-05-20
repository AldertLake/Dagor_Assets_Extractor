#pragma once
#include "DagorIO/BinaryReader.h"
#include <cstdint>
#include <string>
#include <vector>
#include <span>

namespace DagorFormats {

struct GameResDescMaterialTextureRef {
    uint32_t materialIndex = 0;
    uint32_t textureIndex = 0;
    std::string slotName;
};

struct GameResDescEntry {
    std::string name;
    std::string className;
    uint32_t resId = 0;
    std::vector<uint32_t> dependencyIds;
    
    std::vector<std::string> textures;
    std::vector<std::string> materials;
    std::vector<std::string> materialDomains;
    std::vector<std::string> materialShaderClasses;
    std::vector<GameResDescMaterialTextureRef> materialTextureRefs;
    std::vector<std::string> shaders;
    std::string skeletonName;
    std::string dynModelName;
};

class GameResDescReader {
public:
    explicit GameResDescReader(std::span<const uint8_t> data);

    void Parse();

    const std::vector<GameResDescEntry>& GetEntries() const { return m_entries; }

private:
    std::span<const uint8_t> m_data;
    std::vector<GameResDescEntry> m_entries;
};

} // namespace DagorFormats
