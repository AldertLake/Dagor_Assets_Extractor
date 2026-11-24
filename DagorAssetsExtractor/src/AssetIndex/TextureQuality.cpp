#include "AssetIndex/TextureQuality.h"

#include "Platform/Platform.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

namespace DagorAssetsExtractor::AssetIndex
{
    namespace
    {
        std::string ToLowerAscii(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return value;
        }

        std::optional<TextureQualityTier> ParseExplicitQualityTag(std::string_view entryName)
        {
            const std::string lowered = ToLowerAscii(std::string(entryName));
            size_t marker = lowered.find('$');
            while (marker != std::string::npos)
            {
                const size_t tokenBegin = marker + 1;
                const size_t tokenEnd = lowered.find_first_of("$*./\\", tokenBegin);
                const std::string_view token(
                    lowered.data() + tokenBegin,
                    (tokenEnd == std::string::npos ? lowered.size() : tokenEnd) - tokenBegin);

                if (token == "uhq")
                {
                    return TextureQualityTier::UltraHigh;
                }
                if (token == "hq")
                {
                    return TextureQualityTier::High;
                }
                if (token == "tq")
                {
                    return TextureQualityTier::Reduced;
                }
                if (token == "lq")
                {
                    return TextureQualityTier::Low;
                }

                marker = lowered.find('$', tokenBegin);
            }

            return std::nullopt;
        }

        bool EndsWith(std::string_view value, std::string_view suffix)
        {
            return suffix.size() <= value.size() &&
                   value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
        }

        bool HasPackSuffix(std::string_view path, std::string_view suffix)
        {
            constexpr std::string_view extensions[] = {
                ".dxp.bin",
                ".bin",
            };

            for (const std::string_view extension : extensions)
            {
                std::string expected;
                expected.reserve(suffix.size() + extension.size());
                expected.append(suffix);
                expected.append(extension);
                if (EndsWith(path, expected))
                {
                    return true;
                }
            }
            return EndsWith(path, suffix);
        }

        bool ContainsPathDirectory(std::string_view path, std::string_view directory)
        {
            return (path.size() > directory.size() &&
                    path.compare(0, directory.size(), directory) == 0 &&
                    path[directory.size()] == '/') ||
                   path.find(std::string("/") + std::string(directory) + "/") !=
                       std::string_view::npos;
        }

        TextureQualityTier InferPackQuality(const std::filesystem::path& packPath)
        {
            std::string path = ToLowerAscii(Platform::PathToUtf8(packPath));
            std::replace(path.begin(), path.end(), '\\', '/');

            if (ContainsPathDirectory(path, "content.uhq") ||
                ContainsPathDirectory(path, "uhq_vehicles") ||
                ContainsPathDirectory(path, "uhq_tex") ||
                HasPackSuffix(path, "-uhq") ||
                HasPackSuffix(path, "_uhq"))
            {
                return TextureQualityTier::UltraHigh;
            }
            if (ContainsPathDirectory(path, "content.hq") ||
                ContainsPathDirectory(path, "hq_tex") ||
                HasPackSuffix(path, "-hq") ||
                HasPackSuffix(path, "_hq"))
            {
                return TextureQualityTier::High;
            }
            if (ContainsPathDirectory(path, "tq_tex") ||
                HasPackSuffix(path, "-tq") ||
                HasPackSuffix(path, "_tq"))
            {
                return TextureQualityTier::Reduced;
            }
            if (ContainsPathDirectory(path, "lq_tex") ||
                HasPackSuffix(path, "-lq") ||
                HasPackSuffix(path, "_lq"))
            {
                return TextureQualityTier::Low;
            }

            return TextureQualityTier::Standard;
        }
    }

    TextureQualityTier ClassifyTextureQuality(
        const std::filesystem::path& packPath,
        std::string_view entryName)
    {
        if (const std::optional<TextureQualityTier> explicitTier =
                ParseExplicitQualityTag(entryName))
        {
            return *explicitTier;
        }

        return InferPackQuality(packPath);
    }

    int TextureQualityPriority(
        const std::filesystem::path& packPath,
        std::string_view entryName)
    {
        if (const std::optional<TextureQualityTier> explicitTier =
                ParseExplicitQualityTag(entryName))
        {
            return static_cast<int>(*explicitTier) * 10 + 1;
        }

        return static_cast<int>(InferPackQuality(packPath)) * 10;
    }

    std::string_view TextureQualityTierName(TextureQualityTier tier)
    {
        switch (tier)
        {
        case TextureQualityTier::UltraHigh:
            return "ultra-high";
        case TextureQualityTier::High:
            return "high";
        case TextureQualityTier::Standard:
            return "standard";
        case TextureQualityTier::Reduced:
            return "reduced";
        case TextureQualityTier::Low:
            return "low";
        }
        return "unknown";
    }
}
