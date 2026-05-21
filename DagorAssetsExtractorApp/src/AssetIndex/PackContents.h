#pragma once

#include "AssetIndex/AssetIndex.h"
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

namespace DagorAssetsExtractor::AssetIndex
{
    struct DescriptorMaterialTextureRef
    {
        uint32_t materialIndex = 0;
        uint32_t textureIndex = 0;
        std::string slotName;
    };

    // A single sub-asset entry inside a container (VROMFS, DXP, etc.)
    struct SubAssetRecord
    {
        std::string name;          // Internal name / path inside the container
        uint32_t    offset  = 0;   // Byte offset within the container
        uint32_t    size    = 0;   // Size in bytes
        std::string formatInfo;    // For textures: format string; for descriptors: class; for others: empty
        uint32_t    grpClassId = 0; // GRP resource class id when available
        uint32_t    skeletonNodeCount = 0;
        uint32_t    skeletonImportantNodeCount = 0;
        std::string skeletonRootName;

        // DDSx-specific fields (only populated for DXP texture sub-assets)
        uint16_t    texWidth   = 0;
        uint16_t    texHeight  = 0;
        uint8_t     texMips    = 0;
        std::string texFormat;     // Human-readable format name
        std::string texCompression; // Compression method name
        std::vector<uint8_t> ddsxHeader; // Raw 32-byte DDSx header stored separately by DXP packs

        // Descriptor-specific fields (only populated for GameResDesc entries)
        uint32_t    resId      = 0;
        std::vector<uint32_t> dependencyIds;
        std::vector<std::string> textureNames;
        std::vector<std::string> materialNames;
        std::vector<std::string> materialDomains;
        std::vector<std::string> materialShaderClasses;
        std::vector<DescriptorMaterialTextureRef> materialTextureRefs;
        std::string skeletonName;
        std::string dynModelName;
    };

    // Parsed contents of a single container pack, loaded on demand.
    struct PackContents
    {
        std::filesystem::path sourcePath;     // The container file that was parsed
        std::string           containerType;  // "VROMFS", "DXP", etc.
        std::vector<SubAssetRecord> entries;
        std::string           error;          // Non-empty if parsing failed
        bool                  parsed = false; // True once parsing has been attempted
    };

    // Thread-safe cache for lazily-loaded PackContents.
    // The UI thread requests contents by path; if not cached, the cache
    // returns a "not ready" result and the caller can trigger an async parse.
    class PackContentsCache
    {
    public:
        // Returns a shared_ptr to the contents if already parsed, or nullptr.
        std::shared_ptr<const PackContents> Get(const std::filesystem::path& packPath) const;

        // Store parsed contents into the cache. Thread-safe.
        void Put(const std::filesystem::path& packPath, std::shared_ptr<PackContents> contents);

        // Check if contents are cached for a given path.
        bool Has(const std::filesystem::path& packPath) const;

        // Remove cached contents for a given path (e.g., after re-scan).
        void Remove(const std::filesystem::path& packPath);

        // Number of parsed containers currently retained by the cache.
        size_t Size() const;

        // Remove every cached container except the path that should stay visible.
        size_t ClearExcept(const std::filesystem::path& keepPath);

        // Enforce a maximum retained count using least-recently-used eviction.
        size_t TrimToLimit(size_t maxEntries, const std::filesystem::path& keepPath);

        // Clear the entire cache.
        void Clear();

    private:
        struct CachedContents
        {
            std::shared_ptr<PackContents> contents;
            uint64_t lastAccess = 0;
        };

        mutable std::mutex m_mutex;
        mutable uint64_t m_nextAccess = 1;
        mutable std::unordered_map<std::wstring, CachedContents> m_cache;
    };

    // Parse the contents of a VROMFS container from disk.
    // Returns a PackContents with entries populated, or with error set.
    std::shared_ptr<PackContents> ParseVromfsContents(const std::filesystem::path& path, size_t maxFileSize = 256 * 1024 * 1024);

    // Parse the contents of a DXP container from disk, including DDSx header metadata.
    // Returns a PackContents with entries populated, or with error set.
    std::shared_ptr<PackContents> ParseDxpContents(const std::filesystem::path& path, size_t maxFileSize = 256 * 1024 * 1024);

    // Parse the contents of a GRP container from disk.
    // Returns a PackContents with entries populated, or with error set.
    std::shared_ptr<PackContents> ParseGrpContents(const std::filesystem::path& path, size_t maxFileSize = 256 * 1024 * 1024);

    // Describe a known Dagor GRP resource class id.
    std::string DescribeGrpClassId(uint32_t classId);

    // Whether the current tool can export this GRP entry as a static model.
    bool CanExportGrpEntryAsStaticModel(const SubAssetRecord& entry);

    // Whether the current tool can export this GRP entry as a skinned model.
    bool CanExportGrpEntryAsSkinnedModel(const SubAssetRecord& entry);

    // Check if sub-asset export is supported for the given entry and container type.
    bool IsExportSupported(const SubAssetRecord& entry, AssetType containerType);

    // Get an informative tooltip message describing why export is unsupported for this sub-asset.
    std::string GetUnsupportedExportTooltip(const SubAssetRecord& entry);
    std::string GetUnsupportedExportTooltip(uint32_t grpClassId, const std::string& fallbackClassName);

    // Parse the contents of a GameRes descriptor file (.bin) from disk.
    // Returns a PackContents with entries populated, or with error set.
    std::shared_ptr<PackContents> ParseDescriptorContents(const std::filesystem::path& path, size_t maxFileSize = 256 * 1024 * 1024);

} // namespace DagorAssetsExtractor::AssetIndex
