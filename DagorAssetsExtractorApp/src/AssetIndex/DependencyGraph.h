#pragma once

#include "AssetIndex/AssetIndex.h"
#include "AssetIndex/BlkRelationships.h"
#include "AssetIndex/PackContents.h"

#include <cstdint>
#include <functional>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
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
        std::vector<std::string> materialDomains;
        std::vector<std::string> materialShaderClasses;
        std::vector<uint32_t> materialTextureRefs;
        std::vector<DescriptorMaterialTextureRef> materialTextureBindings;
        std::optional<std::filesystem::path> matchedGrpPath;
        std::optional<std::string> matchedGrpEntryName;
        uint32_t matchedGrpClassId = 0;
        std::string matchedGrpClassName;
        bool matchedGrpEntryCanExportStaticModel = false;
        bool matchedGrpEntryCanExportSkinnedModel = false;
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
        bool canceled = false;
        std::vector<std::string> warnings;
    };

    struct DescriptorResourceLinkProgress
    {
        std::string stage;
        std::string item;
        size_t current = 0;
        size_t total = 0;
        size_t descriptorFilesParsed = 0;
        size_t linksBuilt = 0;
        size_t grpPacksParsed = 0;
        size_t dxpPacksParsed = 0;
    };

    using DescriptorResourceLinkProgressCallback = std::function<bool(const DescriptorResourceLinkProgress&)>;

    struct DescriptorResourceLinkBuildOptions
    {
        const std::vector<BlkResourceLink>* blkLinks = nullptr;
        uintmax_t maxDescriptorBytes = 512 * 1024 * 1024;
        uintmax_t maxPackBytes = 512 * 1024 * 1024;
        std::optional<std::filesystem::path> focusPath;
        DescriptorResourceLinkProgressCallback progress;
    };

    struct DescriptorResourceLinkCacheLoadResult
    {
        bool fileFound = false;
        bool rootMatched = false;
        bool optionsMatched = false;
        bool sourcesMatched = false;
        bool loaded = false;
        std::string message;
        std::vector<DescriptorResourceLink> links;
    };

    DescriptorResourceLinkBuildResult BuildDescriptorResourceLinks(
        const std::vector<ResourceRecord>& records,
        const DescriptorResourceLinkBuildOptions& options);

    DescriptorResourceLinkBuildResult BuildDescriptorResourceLinks(
        const std::vector<ResourceRecord>& records,
        const std::vector<BlkResourceLink>* blkLinks = nullptr,
        uintmax_t maxDescriptorBytes = 512 * 1024 * 1024,
        uintmax_t maxPackBytes = 512 * 1024 * 1024);

    bool SaveDescriptorResourceLinkCache(
        const std::filesystem::path& cachePath,
        const std::filesystem::path& gameRoot,
        const std::vector<ResourceRecord>& records,
        const std::vector<DescriptorResourceLink>& links,
        const DescriptorResourceLinkBuildOptions& options,
        std::string& error);

    DescriptorResourceLinkCacheLoadResult LoadDescriptorResourceLinkCache(
        const std::filesystem::path& cachePath,
        const std::filesystem::path& gameRoot,
        const std::vector<ResourceRecord>& records,
        const DescriptorResourceLinkBuildOptions& options);
}
