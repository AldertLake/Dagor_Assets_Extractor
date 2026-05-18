#pragma once

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
    // A single sub-asset entry inside a container (VROMFS, DXP, etc.)
    struct SubAssetRecord
    {
        std::string name;          // Internal name / path inside the container
        uint32_t    offset  = 0;   // Byte offset within the container
        uint32_t    size    = 0;   // Size in bytes
        std::string formatInfo;    // For textures: format string; for descriptors: class; for others: empty

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

        // Clear the entire cache.
        void Clear();

    private:
        mutable std::mutex m_mutex;
        std::unordered_map<std::wstring, std::shared_ptr<PackContents>> m_cache;
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

    // Parse the contents of a GameRes descriptor file (.bin) from disk.
    // Returns a PackContents with entries populated, or with error set.
    std::shared_ptr<PackContents> ParseDescriptorContents(const std::filesystem::path& path, size_t maxFileSize = 256 * 1024 * 1024);

} // namespace DagorAssetsExtractor::AssetIndex
