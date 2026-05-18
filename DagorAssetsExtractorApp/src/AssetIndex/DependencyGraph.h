#pragma once

#include "AssetIndex/AssetIndex.h"
#include "AssetIndex/BlkRelationships.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace DagorAssetsExtractor::AssetIndex
{
    struct DescriptorTextureLink
    {
        uint32_t textureIndex = 0;
        std::string textureName;
        std::optional<std::filesystem::path> matchedDxpPath;
        std::optional<std::string> matchedDxpEntryName;
    };

    struct DescriptorResourceLink
    {
        std::filesystem::path descriptorPath;
        std::string resourceName;
        uint32_t resId = 0;
        std::vector<std::string> textureNames;
        std::vector<std::string> materialNames;
        std::vector<uint32_t> materialTextureRefs;
        std::optional<std::filesystem::path> matchedGrpPath;
        std::optional<std::string> matchedGrpEntryName;
        uint32_t matchedGrpClassId = 0;
        std::string matchedGrpClassName;
        bool matchedGrpEntryCanExportStaticModel = false;
        std::vector<DescriptorTextureLink> textureLinks;
    };

    struct DescriptorResourceLinkBuildResult
    {
        std::vector<DescriptorResourceLink> links;
        size_t descriptorFilesParsed = 0;
        size_t descriptorFilesSkipped = 0;
        size_t parseErrors = 0;
        size_t grpPacksParsed = 0;
        size_t dxpPacksParsed = 0;
        std::vector<std::string> warnings;
    };

    DescriptorResourceLinkBuildResult BuildDescriptorResourceLinks(
        const std::vector<ResourceRecord>& records,
        const std::vector<BlkResourceLink>* blkLinks = nullptr,
        uintmax_t maxDescriptorBytes = 256 * 1024 * 1024,
        uintmax_t maxPackBytes = 256 * 1024 * 1024);
}
