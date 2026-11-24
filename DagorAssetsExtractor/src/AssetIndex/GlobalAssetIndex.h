#pragma once

#include "AssetIndex/AssetIndex.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace DagorAssetsExtractor::AssetIndex
{
    struct DescriptorResourceLink;

    struct IndexedGrpEntry
    {
        std::filesystem::path packPath;
        std::string name;
        uint32_t classId = 0;
        uint32_t offset = 0;
        uint32_t size = 0;
    };

    struct IndexedDxpTexture
    {
        std::filesystem::path packPath;
        std::string entryName;
        int qualityRank = 0;
    };

    struct CachedGrpPackSnapshot
    {
        std::filesystem::path path;
        uintmax_t sizeBytes = 0;
        int64_t lastWriteTicks = 0;
        std::vector<IndexedGrpEntry> entries;
    };

    struct CachedDxpPackSnapshot
    {
        std::filesystem::path path;
        uintmax_t sizeBytes = 0;
        int64_t lastWriteTicks = 0;
        std::vector<std::string> entryNames;
    };

    struct CachedPackMetadata
    {
        std::vector<CachedGrpPackSnapshot> grpPacks;
        std::vector<CachedDxpPackSnapshot> dxpPacks;
    };

    struct TransparentStringHash
    {
        using is_transparent = void;
        size_t operator()(std::string_view sv) const noexcept
        {
            return std::hash<std::string_view>{}(sv);
        }
    };

    struct TransparentWStringHash
    {
        using is_transparent = void;
        size_t operator()(std::wstring_view sv) const noexcept
        {
            return std::hash<std::wstring_view>{}(sv);
        }
    };

    struct GlobalAssetIndex
    {
        std::vector<IndexedGrpEntry> grpEntries;
        std::unordered_map<std::string, std::vector<size_t>, TransparentStringHash, std::equal_to<>> grpByExactName;
        std::unordered_map<std::string, std::vector<size_t>, TransparentStringHash, std::equal_to<>> grpByLowerName;
        std::vector<size_t> anim2DataIndices;

        std::unordered_map<std::string, std::vector<IndexedDxpTexture>, TransparentStringHash, std::equal_to<>> dxpByNormalizedTextureName;

        std::unordered_map<std::wstring, std::vector<size_t>, TransparentWStringHash, std::equal_to<>> linksByDescriptorPath;
        std::unordered_map<std::wstring, std::vector<size_t>, TransparentWStringHash, std::equal_to<>> linksByGrpPath;
        std::unordered_map<std::wstring, std::vector<std::pair<size_t, size_t>>, TransparentWStringHash, std::equal_to<>> linksByDxpPath;
        std::unordered_map<std::string, std::vector<size_t>, TransparentStringHash, std::equal_to<>> linksByResourceOrGrpName;

        bool hasPackMetadata = false;

        static std::wstring MakePathKey(const std::filesystem::path& path);
        static std::string NormalizeTextureName(std::string_view name);

        const IndexedGrpEntry* FindGrpEntry(
            std::string_view name,
            uint32_t preferredClassId = 0,
            const std::filesystem::path* preferredPack = nullptr) const;

        const std::vector<IndexedDxpTexture>* FindDxpTextures(std::string_view rawOrNormalizedTexName) const;

        const DescriptorResourceLink* FindDescriptorLink(
            const std::vector<DescriptorResourceLink>& links,
            std::string_view entryName,
            const std::filesystem::path& packPath) const;
    };

    std::shared_ptr<GlobalAssetIndex> BuildGlobalAssetIndex(
        const std::vector<DescriptorResourceLink>* links,
        const CachedPackMetadata* packMetadata);

    std::shared_ptr<GlobalAssetIndex> BuildPackOnlyGlobalAssetIndex(
        const std::vector<ResourceRecord>& records);
}
