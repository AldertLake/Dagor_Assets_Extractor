#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace DagorAssetsExtractor::AssetIndex
{
    enum class AssetType
    {
        Vromfs,
        Grp,
        Dxp,
        Descriptor,
        Blk,
        Unknown
    };

    struct ResourceRecord
    {
        std::filesystem::path path;
        AssetType type = AssetType::Unknown;
        uintmax_t sizeBytes = 0;
        int64_t lastWriteTicks = 0;
    };

    struct ValidationResult
    {
        bool exists = false;
        bool hasAces = false;
        bool hasContent = false;
        bool hasBaseRes = false;
        bool hasKnownContainer = false;
        std::vector<std::string> messages;

        bool IsValid() const;
    };

    struct IndexCacheLoadResult
    {
        bool fileFound = false;
        bool rootMatched = false;
        size_t recordsLoaded = 0;
        size_t recordsSkipped = 0;
        std::string message;
        std::vector<ResourceRecord> records;
    };

    struct DiscoverOptions
    {
        const std::atomic_bool* cancelRequested = nullptr;
        std::function<void(uint64_t)> onFileVisited;
        std::function<void(const ResourceRecord&)> onRecordFound;
    };

    struct DiscoverResult
    {
        std::vector<ResourceRecord> records;
        uint64_t filesVisited = 0;
        bool canceled = false;
        std::string error;
    };

    std::string PathToUtf8(const std::filesystem::path& path);
    std::filesystem::path Utf8ToPath(std::string_view value);
    std::string ToLowerAscii(std::string value);

    const char* AssetTypeName(AssetType type);
    const char* AssetTypeCacheName(AssetType type);
    std::optional<AssetType> ParseAssetType(std::string_view value);

    std::optional<AssetType> ClassifyPath(const std::filesystem::path& path);
    int64_t FileTimeToTicks(std::filesystem::file_time_type fileTime);
    bool TryReadFileMetadata(const std::filesystem::path& path, uintmax_t& sizeBytes, int64_t& lastWriteTicks);

    bool ContainsKnownContainer(const std::filesystem::path& root, uint32_t visitLimit = 20000);
    ValidationResult ValidateGameRoot(const std::filesystem::path& root);
    DiscoverResult DiscoverResources(const std::filesystem::path& root, const DiscoverOptions& options = {});

    bool SaveIndexCacheRecords(
        const std::filesystem::path& cachePath,
        const std::filesystem::path& gameRoot,
        const std::vector<ResourceRecord>& records,
        std::string& error);

    IndexCacheLoadResult LoadIndexCacheRecords(
        const std::filesystem::path& cachePath,
        const std::filesystem::path& gameRoot);
}
