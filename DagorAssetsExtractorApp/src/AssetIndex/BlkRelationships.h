#pragma once

#include "AssetIndex/AssetIndex.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace DagorAssetsExtractor::AssetIndex
{
    struct BlkResourceLink
    {
        std::filesystem::path blkPath;
        uint32_t line = 1;
        std::string ruleName;
        std::string resourceName;
        AssetType expectedType = AssetType::Unknown;
        std::optional<std::filesystem::path> matchedPath;
    };

    struct BlkResourceLinkBuildResult
    {
        std::vector<BlkResourceLink> links;
        size_t blkFilesParsed = 0;
        size_t blkFilesSkipped = 0;
        size_t parseErrors = 0;
        std::vector<std::string> warnings;
    };

    BlkResourceLinkBuildResult BuildBlkResourceLinks(
        const std::vector<ResourceRecord>& records,
        uintmax_t maxBlkBytes = 1024 * 1024);
}
