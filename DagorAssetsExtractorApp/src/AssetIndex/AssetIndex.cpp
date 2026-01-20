#include "AssetIndex/AssetIndex.h"
#include "../Platform/Platform.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace DagorAssetsExtractor::AssetIndex
{
    namespace fs = std::filesystem;

    namespace
    {
        std::wstring Utf8ToWide(std::string_view utf8)
        {
            if (utf8.empty())
            {
                return {};
            }

#if defined(_WIN32)
            const int size = MultiByteToWideChar(
                CP_UTF8,
                0,
                utf8.data(),
                static_cast<int>(utf8.size()),
                nullptr,
                0);

            if (size <= 0)
            {
                return {};
            }

            std::wstring result(static_cast<size_t>(size), L'\0');
            MultiByteToWideChar(
                CP_UTF8,
                0,
                utf8.data(),
                static_cast<int>(utf8.size()),
                result.data(),
                size);
            return result;
#else
            return std::wstring(utf8.begin(), utf8.end());
#endif
        }

        std::string WideToUtf8(std::wstring_view wide)
        {
            if (wide.empty())
            {
                return {};
            }

#if defined(_WIN32)
            const int size = WideCharToMultiByte(
                CP_UTF8,
                0,
                wide.data(),
                static_cast<int>(wide.size()),
                nullptr,
                0,
                nullptr,
                nullptr);

            if (size <= 0)
            {
                return {};
            }

            std::string result(static_cast<size_t>(size), '\0');
            WideCharToMultiByte(
                CP_UTF8,
                0,
                wide.data(),
                static_cast<int>(wide.size()),
                result.data(),
                size,
                nullptr,
                nullptr);
            return result;
#else
            return std::string(wide.begin(), wide.end());
#endif
        }

        std::wstring ToLowerWide(std::wstring value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
                return static_cast<wchar_t>(towlower(c));
            });
            return value;
        }

        std::wstring NormalizePathForCompare(const fs::path& path)
        {
            return ToLowerWide(path.lexically_normal().wstring());
        }

        bool SamePathForCache(const fs::path& left, const fs::path& right)
        {
            return NormalizePathForCompare(left) == NormalizePathForCompare(right);
        }

        bool EndsWith(std::wstring_view value, std::wstring_view suffix)
        {
            if (suffix.size() > value.size())
            {
                return false;
            }

            return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
        }
    }

    bool ValidationResult::IsValid() const
    {
        return exists && hasContent && (hasAces || hasBaseRes || hasKnownContainer);
    }

    std::string ToLowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    const char* AssetTypeName(AssetType type)
    {
        switch (type)
        {
        case AssetType::Vromfs:
            return "VROMFS";
        case AssetType::Grp:
            return "GRP";
        case AssetType::Dxp:
            return "DXP";
        case AssetType::Descriptor:
            return "Descriptor";
        case AssetType::Blk:
            return "BLK";
        default:
            return "Unknown";
        }
    }

    const char* AssetTypeCacheName(AssetType type)
    {
        switch (type)
        {
        case AssetType::Vromfs:
            return "vromfs";
        case AssetType::Grp:
            return "grp";
        case AssetType::Dxp:
            return "dxp";
        case AssetType::Descriptor:
            return "descriptor";
        case AssetType::Blk:
            return "blk";
        default:
            return "unknown";
        }
    }

    std::optional<AssetType> ParseAssetType(std::string_view value)
    {
        const std::string normalized = ToLowerAscii(std::string(value));
        if (normalized == "vromfs")
        {
            return AssetType::Vromfs;
        }
        if (normalized == "grp")
        {
            return AssetType::Grp;
        }
        if (normalized == "dxp")
        {
            return AssetType::Dxp;
        }
        if (normalized == "descriptor")
        {
            return AssetType::Descriptor;
        }
        if (normalized == "blk")
        {
            return AssetType::Blk;
        }
        if (normalized == "unknown")
        {
            return AssetType::Unknown;
        }
        return std::nullopt;
    }

    std::optional<AssetType> ClassifyPath(const fs::path& path)
    {
        const std::wstring fileName = ToLowerWide(path.filename().wstring());

        if (EndsWith(fileName, L".vromfs.bin"))
        {
            return AssetType::Vromfs;
        }
        if (EndsWith(fileName, L".dxp.bin"))
        {
            return AssetType::Dxp;
        }
        if (EndsWith(fileName, L".grp"))
        {
            return AssetType::Grp;
        }
        if (fileName == L"ridesc.bin" || fileName == L"dynmodeldesc.bin")
        {
            return AssetType::Descriptor;
        }
        if (EndsWith(fileName, L".blk"))
        {
            return AssetType::Blk;
        }

        return std::nullopt;
    }

    int64_t FileTimeToTicks(fs::file_time_type fileTime)
    {
        return static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(fileTime.time_since_epoch()).count());
    }

    bool TryReadFileMetadata(const fs::path& path, uintmax_t& sizeBytes, int64_t& lastWriteTicks)
    {
        std::error_code ec;
        if (!fs::is_regular_file(path, ec) || ec)
        {
            return false;
        }

        const uintmax_t size = fs::file_size(path, ec);
        if (ec)
        {
            return false;
        }

        const fs::file_time_type writeTime = fs::last_write_time(path, ec);
        if (ec)
        {
            return false;
        }

        sizeBytes = size;
        lastWriteTicks = FileTimeToTicks(writeTime);
        return true;
    }

    bool ContainsKnownContainer(const fs::path& root, uint32_t visitLimit)
    {
        std::error_code ec;
        const fs::path contentPath = root / L"content";
        const fs::path scanRoot = fs::exists(contentPath, ec) ? contentPath : root;
        uint32_t visited = 0;

        fs::recursive_directory_iterator iterator(
            scanRoot,
            fs::directory_options::skip_permission_denied,
            ec);
        const fs::recursive_directory_iterator end;

        while (!ec && iterator != end && visited < visitLimit)
        {
            const fs::directory_entry& entry = *iterator;
            ++visited;

            std::error_code entryEc;
            if (entry.is_regular_file(entryEc) && ClassifyPath(entry.path()).has_value())
            {
                return true;
            }

            iterator.increment(ec);
        }

        return false;
    }

    ValidationResult ValidateGameRoot(const fs::path& root)
    {
        ValidationResult result;
        std::error_code ec;

        result.exists = !root.empty() && fs::exists(root, ec) && fs::is_directory(root, ec);
        if (!result.exists)
        {
            result.messages.push_back("Folder does not exist or is not accessible.");
            return result;
        }

        result.hasAces =
            fs::exists(root / L"aces.exe", ec) ||
            fs::exists(root / L"win64" / L"aces.exe", ec) ||
            fs::exists(root / L"win32" / L"aces.exe", ec);
        result.hasContent = fs::is_directory(root / L"content", ec);
        result.hasBaseRes = fs::is_directory(root / L"content" / L"base" / L"res", ec);
        result.hasKnownContainer = ContainsKnownContainer(root);

        if (result.hasAces)
        {
            result.messages.push_back("Found aces.exe marker.");
        }
        else
        {
            result.messages.push_back("aces.exe was not found at the root, win64, or win32 path.");
        }

        if (result.hasContent)
        {
            result.messages.push_back("Found content folder.");
        }
        else
        {
            result.messages.push_back("Missing content folder.");
        }

        if (result.hasBaseRes)
        {
            result.messages.push_back("Found content/base/res folder.");
        }
        else
        {
            result.messages.push_back("content/base/res was not found.");
        }

        if (result.hasKnownContainer)
        {
            result.messages.push_back("Found at least one Dagor container or descriptor.");
        }
        else
        {
            result.messages.push_back("No .vromfs.bin, .grp, .dxp.bin, descriptor, or .blk file was found in the quick scan.");
        }

        if (!result.IsValid())
        {
            result.messages.push_back("This does not look like a War Thunder installation yet.");
        }

        return result;
    }

    DiscoverResult DiscoverResources(const fs::path& root, const DiscoverOptions& options)
    {
        DiscoverResult result;
        std::error_code ec;
        fs::recursive_directory_iterator iterator(
            root,
            fs::directory_options::skip_permission_denied,
            ec);
        const fs::recursive_directory_iterator end;

        if (ec)
        {
            result.error = "Could not start resource discovery: " + ec.message();
            return result;
        }

        while (iterator != end)
        {
            if (options.cancelRequested != nullptr && options.cancelRequested->load())
            {
                result.canceled = true;
                return result;
            }

            const fs::directory_entry& entry = *iterator;
            ++result.filesVisited;
            if (options.onFileVisited)
            {
                options.onFileVisited(result.filesVisited);
            }

            std::error_code entryEc;
            if (entry.is_regular_file(entryEc))
            {
                if (const std::optional<AssetType> type = ClassifyPath(entry.path()))
                {
                    uintmax_t sizeBytes = 0;
                    int64_t lastWriteTicks = 0;
                    if (TryReadFileMetadata(entry.path(), sizeBytes, lastWriteTicks))
                    {
                        ResourceRecord record;
                        record.path = entry.path();
                        record.type = *type;
                        record.sizeBytes = sizeBytes;
                        record.lastWriteTicks = lastWriteTicks;
                        result.records.push_back(record);

                        if (options.onRecordFound)
                        {
                            options.onRecordFound(record);
                        }
                    }
                }
            }

            iterator.increment(ec);
            if (ec)
            {
                result.error = "Resource discovery stopped early: " + ec.message();
                return result;
            }
        }

        return result;
    }

    bool SaveIndexCacheRecords(
        const fs::path& cachePath,
        const fs::path& gameRoot,
        const std::vector<ResourceRecord>& records,
        std::string& error)
    {
        std::error_code ec;
        fs::create_directories(cachePath.parent_path(), ec);
        if (ec)
        {
            error = "Could not create index cache folder: " + ec.message();
            return false;
        }

        std::ofstream file(cachePath, std::ios::trunc);
        if (!file)
        {
            error = "Could not open index cache for writing.";
            return false;
        }

        file << "DagorAssetsExtractorIndexCache 1\n";
        file << "root " << std::quoted(Platform::PathToUtf8(gameRoot)) << '\n';
        for (const ResourceRecord& record : records)
        {
            file << "record "
                 << std::quoted(Platform::PathToUtf8(record.path)) << ' '
                 << AssetTypeCacheName(record.type) << ' '
                 << record.sizeBytes << ' '
                 << record.lastWriteTicks << '\n';
        }

        if (!file)
        {
            error = "Failed while writing index cache.";
            return false;
        }

        return true;
    }

    IndexCacheLoadResult LoadIndexCacheRecords(const fs::path& cachePath, const fs::path& gameRoot)
    {
        IndexCacheLoadResult result;

        std::ifstream file(cachePath);
        if (!file)
        {
            result.message = "No index cache file found.";
            return result;
        }

        result.fileFound = true;

        std::string magic;
        int version = 0;
        if (!(file >> magic >> version) || magic != "DagorAssetsExtractorIndexCache" || version != 1)
        {
            result.message = "Index cache has an unsupported format.";
            return result;
        }

        fs::path cachedRoot;
        std::vector<ResourceRecord> parsedRecords;

        std::string tag;
        while (file >> tag)
        {
            if (tag == "root")
            {
                std::string rootUtf8;
                if (file >> std::quoted(rootUtf8))
                {
                    cachedRoot = Platform::Utf8ToPath(rootUtf8);
                }
                continue;
            }

            if (tag == "record")
            {
                std::string pathUtf8;
                std::string typeName;
                uintmax_t cachedSize = 0;
                int64_t cachedTicks = 0;
                if (!(file >> std::quoted(pathUtf8) >> typeName >> cachedSize >> cachedTicks))
                {
                    result.message = "Index cache is truncated or malformed.";
                    return result;
                }

                const std::optional<AssetType> type = ParseAssetType(typeName);
                if (!type)
                {
                    ++result.recordsSkipped;
                    continue;
                }

                ResourceRecord record;
                record.path = Platform::Utf8ToPath(pathUtf8);
                record.type = *type;
                record.sizeBytes = cachedSize;
                record.lastWriteTicks = cachedTicks;
                parsedRecords.push_back(std::move(record));
                continue;
            }

            std::string ignoredLine;
            std::getline(file, ignoredLine);
        }

        if (cachedRoot.empty())
        {
            result.message = "Index cache is missing its game root.";
            return result;
        }

        result.rootMatched = SamePathForCache(cachedRoot, gameRoot);
        if (!result.rootMatched)
        {
            result.message = "Index cache belongs to another game folder.";
            return result;
        }

        for (const ResourceRecord& parsed : parsedRecords)
        {
            uintmax_t currentSize = 0;
            int64_t currentTicks = 0;
            if (!TryReadFileMetadata(parsed.path, currentSize, currentTicks) ||
                currentSize != parsed.sizeBytes ||
                currentTicks != parsed.lastWriteTicks)
            {
                ++result.recordsSkipped;
                continue;
            }

            result.records.push_back(parsed);
            ++result.recordsLoaded;
        }

        std::ostringstream message;
        message << "Loaded " << result.recordsLoaded << " cached records";
        if (result.recordsSkipped > 0)
        {
            message << " (" << result.recordsSkipped << " stale or invalid skipped)";
        }
        message << '.';
        result.message = message.str();
        return result;
    }
}
