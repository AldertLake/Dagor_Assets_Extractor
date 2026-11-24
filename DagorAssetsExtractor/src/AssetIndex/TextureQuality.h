#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace DagorAssetsExtractor::AssetIndex
{
    enum class TextureQualityTier : uint8_t
    {
        Low = 0,
        Reduced = 1,
        Standard = 2,
        High = 3,
        UltraHigh = 4,
    };

    TextureQualityTier ClassifyTextureQuality(
        const std::filesystem::path& packPath,
        std::string_view entryName);

    int TextureQualityPriority(
        const std::filesystem::path& packPath,
        std::string_view entryName);

    std::string_view TextureQualityTierName(TextureQualityTier tier);
}
