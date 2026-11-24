#include "AssetIndex/AssetIndex.h"
#include "AssetIndex/BinaryCache.h"
#include "Jobs/JobSystem.h"
#include "../Platform/Platform.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cwctype>
#include <deque>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace DagorAssetsExtractor::AssetIndex
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr uint32_t kIndexBinarySchemaVersion = 2;

        std::wstring ToLowerWide(std::wstring value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](wchar_t c) {
                return static_cast<wchar_t>(towlower(c));
            });
            return value;
        }

        std::wstring NormalizePathForCompare(const fs::path& path)
        {
            std::wstring key = path.lexically_normal().wstring();
            std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) {
                return static_cast<wchar_t>(towlower(c));
            });
            while (key.size() > 1 && (key.back() == L'\\' || key.back() == L'/'))
            {
                if (key.size() == 3 && key[1] == L':')
                {
                    break;
                }
                key.pop_back();
            }
            return key;
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

        bool EqualsInsensitiveAsciiWide(std::wstring_view left, std::wstring_view right)
        {
            if (left.size() != right.size())
            {
                return false;
            }

            for (size_t i = 0; i < left.size(); ++i)
            {
                const wchar_t lc = (left[i] >= L'A' && left[i] <= L'Z') ? static_cast<wchar_t>(left[i] - L'A' + L'a') : left[i];
                const wchar_t rc = (right[i] >= L'A' && right[i] <= L'Z') ? static_cast<wchar_t>(right[i] - L'A' + L'a') : right[i];
                if (lc != rc)
                {
                    return false;
                }
            }
            return true;
        }

        bool EndsWithInsensitiveAsciiWide(std::wstring_view value, std::wstring_view lowerSuffix)
        {
            if (lowerSuffix.size() > value.size())
            {
                return false;
            }

            const size_t offset = value.size() - lowerSuffix.size();
            for (size_t i = 0; i < lowerSuffix.size(); ++i)
            {
                wchar_t ch = value[offset + i];
                if (ch >= L'A' && ch <= L'Z')
                {
                    ch = static_cast<wchar_t>(ch - L'A' + L'a');
                }
                if (ch != lowerSuffix[i])
                {
                    return false;
                }
            }
            return true;
        }

        std::optional<AssetType> ClassifyFileNameWide(std::wstring_view fileName)
        {
            if (EndsWithInsensitiveAsciiWide(fileName, L".vromfs.bin"))
            {
                return AssetType::Vromfs;
            }
            if (EndsWithInsensitiveAsciiWide(fileName, L".dxp.bin"))
            {
                return AssetType::Dxp;
            }
            if (EndsWithInsensitiveAsciiWide(fileName, L".grp"))
            {
                return AssetType::Grp;
            }
            if (EqualsInsensitiveAsciiWide(fileName, L"ridesc.bin") ||
                EqualsInsensitiveAsciiWide(fileName, L"dynmodeldesc.bin"))
            {
                return AssetType::Descriptor;
            }
            if (EndsWithInsensitiveAsciiWide(fileName, L".blk"))
            {
                return AssetType::Blk;
            }
            return std::nullopt;
        }

        bool ShouldSkipDirectoryName(std::wstring_view dirName)
        {
            return EqualsInsensitiveAsciiWide(dirName, L"replays") ||
                   EqualsInsensitiveAsciiWide(dirName, L"screenshots") ||
                   EqualsInsensitiveAsciiWide(dirName, L".cache") ||
                   EqualsInsensitiveAsciiWide(dirName, L"userskins") ||
                   EqualsInsensitiveAsciiWide(dirName, L".obsolete");
        }

#if defined(_WIN32)
        int64_t Win32FileTimeToTicks(const FILETIME& ft)
        {
            const uint64_t ticks =
                (static_cast<uint64_t>(ft.dwHighDateTime) << 32) |
                static_cast<uint64_t>(ft.dwLowDateTime);
            return static_cast<int64_t>(ticks);
        }

        uintmax_t Win32FileSize(DWORD high, DWORD low)
        {
            return (static_cast<uintmax_t>(high) << 32) | static_cast<uintmax_t>(low);
        }

        std::wstring JoinWin32Path(std::wstring_view dir, std::wstring_view name)
        {
            std::wstring joined;
            joined.reserve(dir.size() + 1 + name.size());
            joined.append(dir);
            if (!joined.empty() && joined.back() != L'\\' && joined.back() != L'/')
            {
                joined.push_back(L'\\');
            }
            joined.append(name);
            return joined;
        }

        std::wstring ToWin32ApiPath(std::wstring_view path)
        {
            if (path.size() < 248)
            {
                return std::wstring(path);
            }
            if (path.size() >= 4 && path.compare(0, 4, L"\\\\?\\") == 0)
            {
                return std::wstring(path);
            }
            if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\')
            {
                std::wstring extended = L"\\\\?\\UNC\\";
                extended.append(path.substr(2));
                return extended;
            }
            std::wstring extended = L"\\\\?\\";
            extended.append(path);
            return extended;
        }
#endif
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
#if defined(_WIN32)
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        const std::wstring apiPath = ToWin32ApiPath(path.wstring());
        if (GetFileAttributesExW(apiPath.c_str(), GetFileExInfoStandard, &data) == 0)
        {
            return false;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        {
            return false;
        }

        sizeBytes = Win32FileSize(data.nFileSizeHigh, data.nFileSizeLow);
        lastWriteTicks = Win32FileTimeToTicks(data.ftLastWriteTime);
        return true;
#else
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
#endif
    }

    bool ContainsKnownContainer(const fs::path& root, uint32_t visitLimit)
    {
        std::error_code ec;
        const fs::path contentPath = root / L"content";
        const fs::path scanRoot = fs::exists(contentPath, ec) ? contentPath : root;

#if defined(_WIN32)
        std::deque<std::wstring> pending;
        pending.push_back(scanRoot.lexically_normal().wstring());
        uint32_t visited = 0;

        while (!pending.empty() && visited < visitLimit)
        {
            const std::wstring dir = std::move(pending.front());
            pending.pop_front();

            const std::wstring searchPattern = ToWin32ApiPath(JoinWin32Path(dir, L"*"));
            WIN32_FIND_DATAW fd = {};
            HANDLE findHandle = FindFirstFileExW(
                searchPattern.c_str(),
                FindExInfoBasic,
                &fd,
                FindExSearchNameMatch,
                nullptr,
                FIND_FIRST_EX_LARGE_FETCH);
            if (findHandle == INVALID_HANDLE_VALUE)
            {
                continue;
            }

            do
            {
                const std::wstring_view name(fd.cFileName);
                if (name == L"." || name == L"..")
                {
                    continue;
                }

                ++visited;
                if (visited > visitLimit)
                {
                    break;
                }

                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                {
                    continue;
                }

                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                {
                    if (!ShouldSkipDirectoryName(name))
                    {
                        pending.push_back(JoinWin32Path(dir, name));
                    }
                    continue;
                }

                if (ClassifyFileNameWide(name).has_value())
                {
                    FindClose(findHandle);
                    return true;
                }
            }
            while (FindNextFileW(findHandle, &fd) != 0);

            FindClose(findHandle);
        }

        return false;
#else
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
#endif
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
        const auto scanStart = std::chrono::steady_clock::now();
        DiscoverResult result;

        std::error_code ec;
        if (root.empty() || !fs::exists(root, ec) || !fs::is_directory(root, ec))
        {
            result.error = "Could not start resource discovery: folder does not exist or is not accessible.";
            return result;
        }

#if defined(_WIN32)
        const std::wstring normalizedRoot = root.lexically_normal().wstring();

        std::deque<std::wstring> pendingDirs;
        pendingDirs.push_back(normalizedRoot);

        std::mutex queueMutex;
        std::condition_variable queueCv;
        size_t inFlightDirs = 1;
        bool queueDone = false;

        std::atomic<uint64_t> localFilesVisited{ 0 };
        std::atomic<uint64_t> localRecordsFound{ 0 };
        std::mutex callbackMutex;

        Jobs::JobSystem* defaultPool = Jobs::JobSystem::GetDefaultPool();
        const size_t poolWorkers = defaultPool ? defaultPool->WorkerCount() : 4;
        const size_t workerSlots = std::clamp<size_t>(poolWorkers, 1, 16);

        std::vector<std::vector<ResourceRecord>> slotRecords(workerSlots);
        std::vector<std::vector<DirectoryStamp>> slotDirectories(workerSlots);

        const auto recordRootAttrs = [&]() {
            WIN32_FILE_ATTRIBUTE_DATA rootAttrs = {};
            const std::wstring apiRoot = ToWin32ApiPath(normalizedRoot);
            if (GetFileAttributesExW(apiRoot.c_str(), GetFileExInfoStandard, &rootAttrs) != 0)
            {
                slotDirectories[0].push_back(DirectoryStamp{
                    fs::path(normalizedRoot),
                    Win32FileTimeToTicks(rootAttrs.ftLastWriteTime)
                });
            }
        };
        recordRootAttrs();

        Jobs::JobSystem::RunParallelFor(
            workerSlots,
            1,
            [&](size_t, size_t workerSlot) {
                const size_t safeSlot = workerSlot < workerSlots ? workerSlot : 0;
                std::vector<ResourceRecord>& myRecords = slotRecords[safeSlot];
                std::vector<DirectoryStamp>& myDirs = slotDirectories[safeSlot];

                std::vector<std::wstring> childDirs;
                childDirs.reserve(32);

                while (true)
                {
                    if (options.cancelRequested != nullptr &&
                        options.cancelRequested->load(std::memory_order_relaxed))
                    {
                        std::lock_guard<std::mutex> lock(queueMutex);
                        queueDone = true;
                        queueCv.notify_all();
                        return;
                    }

                    std::wstring currentDir;
                    {
                        std::unique_lock<std::mutex> lock(queueMutex);
                        while (pendingDirs.empty() && !queueDone)
                        {
                            if (options.cancelRequested != nullptr &&
                                options.cancelRequested->load(std::memory_order_relaxed))
                            {
                                queueDone = true;
                                queueCv.notify_all();
                                return;
                            }
                            queueCv.wait_for(lock, std::chrono::milliseconds(5));
                        }

                        if (queueDone || pendingDirs.empty())
                        {
                            return;
                        }

                        currentDir = std::move(pendingDirs.front());
                        pendingDirs.pop_front();
                    }

                    childDirs.clear();
                    uint64_t dirVisitedCount = 0;
                    uint64_t dirRecordCount = 0;

                    const std::wstring searchPattern = ToWin32ApiPath(JoinWin32Path(currentDir, L"*"));
                    WIN32_FIND_DATAW fd = {};
                    HANDLE findHandle = FindFirstFileExW(
                        searchPattern.c_str(),
                        FindExInfoBasic,
                        &fd,
                        FindExSearchNameMatch,
                        nullptr,
                        FIND_FIRST_EX_LARGE_FETCH);

                    if (findHandle != INVALID_HANDLE_VALUE)
                    {
                        do
                        {
                            if (options.cancelRequested != nullptr &&
                                options.cancelRequested->load(std::memory_order_relaxed))
                            {
                                break;
                            }

                            const std::wstring_view fileName(fd.cFileName);
                            if (fileName == L"." || fileName == L"..")
                            {
                                continue;
                            }

                            ++dirVisitedCount;

                            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                            {
                                continue;
                            }

                            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                            {
                                if (!ShouldSkipDirectoryName(fileName))
                                {
                                    std::wstring childPath = JoinWin32Path(currentDir, fileName);
                                    myDirs.push_back(DirectoryStamp{
                                        fs::path(childPath),
                                        Win32FileTimeToTicks(fd.ftLastWriteTime)
                                    });
                                    childDirs.push_back(std::move(childPath));
                                }
                                continue;
                            }

                            if (const std::optional<AssetType> type = ClassifyFileNameWide(fileName))
                            {
                                ResourceRecord record;
                                record.path = fs::path(JoinWin32Path(currentDir, fileName));
                                record.type = *type;
                                record.sizeBytes = Win32FileSize(fd.nFileSizeHigh, fd.nFileSizeLow);
                                record.lastWriteTicks = Win32FileTimeToTicks(fd.ftLastWriteTime);

                                if (options.onRecordFound)
                                {
                                    std::lock_guard<std::mutex> cbLock(callbackMutex);
                                    options.onRecordFound(record);
                                }

                                myRecords.push_back(std::move(record));
                                ++dirRecordCount;
                            }
                        }
                        while (FindNextFileW(findHandle, &fd) != 0);

                        FindClose(findHandle);
                    }

                    if (dirVisitedCount > 0)
                    {
                        const uint64_t totalVisited =
                            localFilesVisited.fetch_add(dirVisitedCount, std::memory_order_relaxed) + dirVisitedCount;
                        if (options.filesVisitedCounter != nullptr)
                        {
                            options.filesVisitedCounter->store(totalVisited, std::memory_order_relaxed);
                        }
                        if (options.onFileVisited)
                        {
                            std::lock_guard<std::mutex> cbLock(callbackMutex);
                            options.onFileVisited(totalVisited);
                        }
                    }

                    if (dirRecordCount > 0)
                    {
                        const uint64_t totalRecords =
                            localRecordsFound.fetch_add(dirRecordCount, std::memory_order_relaxed) + dirRecordCount;
                        if (options.recordsFoundCounter != nullptr)
                        {
                            options.recordsFoundCounter->store(totalRecords, std::memory_order_relaxed);
                        }
                    }

                    {
                        std::lock_guard<std::mutex> lock(queueMutex);
                        for (std::wstring& child : childDirs)
                        {
                            pendingDirs.push_back(std::move(child));
                            ++inFlightDirs;
                        }
                        --inFlightDirs;
                        if (inFlightDirs == 0 ||
                            (options.cancelRequested != nullptr &&
                             options.cancelRequested->load(std::memory_order_relaxed)))
                        {
                            queueDone = true;
                            queueCv.notify_all();
                            return;
                        }
                        if (!childDirs.empty())
                        {
                            queueCv.notify_all();
                        }
                    }
                }
            });

        if (options.cancelRequested != nullptr && options.cancelRequested->load())
        {
            result.canceled = true;
            result.filesVisited = localFilesVisited.load();
            result.elapsedMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - scanStart).count());
            return result;
        }

        size_t totalRecords = 0;
        size_t totalDirs = 0;
        for (size_t i = 0; i < workerSlots; ++i)
        {
            totalRecords += slotRecords[i].size();
            totalDirs += slotDirectories[i].size();
        }

        result.records.reserve(totalRecords);
        result.directories.reserve(totalDirs);
        for (size_t i = 0; i < workerSlots; ++i)
        {
            result.records.insert(
                result.records.end(),
                std::make_move_iterator(slotRecords[i].begin()),
                std::make_move_iterator(slotRecords[i].end()));
            result.directories.insert(
                result.directories.end(),
                std::make_move_iterator(slotDirectories[i].begin()),
                std::make_move_iterator(slotDirectories[i].end()));
        }

        // Keep record order deterministic across parallel scans.
        struct SortItem
        {
            std::wstring key;
            size_t index;
        };
        std::vector<SortItem> sortedIndices;
        sortedIndices.reserve(result.records.size());
        for (size_t i = 0; i < result.records.size(); ++i)
        {
            sortedIndices.push_back({ NormalizePathForCompare(result.records[i].path), i });
        }
        std::sort(sortedIndices.begin(), sortedIndices.end(), [](const SortItem& left, const SortItem& right) {
            return left.key < right.key;
        });

        std::vector<ResourceRecord> sortedRecords;
        sortedRecords.reserve(result.records.size());
        for (const SortItem& item : sortedIndices)
        {
            sortedRecords.push_back(std::move(result.records[item.index]));
        }
        result.records = std::move(sortedRecords);

        result.filesVisited = localFilesVisited.load();
        result.elapsedMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - scanStart).count());
        return result;
#else
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
            if (options.filesVisitedCounter != nullptr)
            {
                options.filesVisitedCounter->store(result.filesVisited, std::memory_order_relaxed);
            }
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
                        if (options.recordsFoundCounter != nullptr)
                        {
                            options.recordsFoundCounter->store(
                                static_cast<uint64_t>(result.records.size()),
                                std::memory_order_relaxed);
                        }

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

        result.elapsedMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - scanStart).count());
        return result;
#endif
    }

    bool SaveIndexCacheRecords(
        const fs::path& cachePath,
        const fs::path& gameRoot,
        const std::vector<ResourceRecord>& records,
        std::string& error,
        const std::vector<DirectoryStamp>* directories)
    {
        BinaryWriter writer;
        writer.Reserve(256 + records.size() * 48 + (directories ? directories->size() * 32 : 0));

        const std::string canonicalRoot = Platform::CanonicalizePathUtf8(gameRoot);
        writer.WriteString(canonicalRoot);

        StringTableBuilder dirTable;
        std::vector<uint32_t> recordParentIds;
        std::vector<std::string> recordFileNames;
        recordParentIds.reserve(records.size());
        recordFileNames.reserve(records.size());

        for (const ResourceRecord& record : records)
        {
            const std::string parentUtf8 = Platform::PathToUtf8(record.path.parent_path());
            const std::string fileUtf8 = Platform::PathToUtf8(record.path.filename());
            recordParentIds.push_back(dirTable.Intern(parentUtf8));
            recordFileNames.push_back(fileUtf8);
        }

        if (!dirTable.Write(writer, error))
        {
            return false;
        }

        writer.WriteU32(static_cast<uint32_t>(records.size()));
        for (size_t i = 0; i < records.size(); ++i)
        {
            writer.WriteU32(recordParentIds[i]);
            writer.WriteString(recordFileNames[i]);
            writer.WriteU8(static_cast<uint8_t>(records[i].type));
            writer.WriteU64(static_cast<uint64_t>(records[i].sizeBytes));
            writer.WriteI64(records[i].lastWriteTicks);
        }

        const uint32_t dirCount = directories ? static_cast<uint32_t>(directories->size()) : 0u;
        writer.WriteU32(dirCount);
        if (directories != nullptr)
        {
            for (const DirectoryStamp& dir : *directories)
            {
                writer.WriteString(Platform::PathToUtf8(dir.path));
                writer.WriteI64(dir.lastWriteTicks);
            }
        }

        return WriteBinaryCacheFile(
            cachePath,
            BinaryCacheKind::Index,
            kIndexBinarySchemaVersion,
            gameRoot,
            writer.Span(),
            error);
    }

    namespace
    {
        IndexCacheLoadResult LoadLegacyAsciiIndexCache(
            const fs::path& cachePath,
            const fs::path& gameRoot,
            bool validateOnDisk)
        {
            IndexCacheLoadResult result;
            result.legacyFormat = true;

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
                result.corrupt = true;
                result.message = "Index cache has an unsupported format.";
                return result;
            }

            fs::path cachedRoot;
            std::vector<ResourceRecord> parsedRecords;

            std::string line;
            while (std::getline(file, line))
            {
                if (line.empty()) continue;
                std::stringstream ss(line);
                std::string tag;
                if (!(ss >> tag)) continue;

                if (tag == "root")
                {
                    std::string rootUtf8;
                    if (ss >> std::quoted(rootUtf8))
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
                    if (!(ss >> std::quoted(pathUtf8) >> typeName >> cachedSize >> cachedTicks))
                    {
                        result.corrupt = true;
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
            }

            if (cachedRoot.empty())
            {
                result.corrupt = true;
                result.message = "Index cache is missing its game root.";
                return result;
            }

            result.rootMatched = SamePathForCache(cachedRoot, gameRoot);
            if (!result.rootMatched)
            {
                result.message = "Index cache belongs to another game folder.";
                return result;
            }

            if (!validateOnDisk)
            {
                result.recordsLoaded = parsedRecords.size();
                result.records = std::move(parsedRecords);
            }
            else
            {
                result.records.reserve(parsedRecords.size());
                for (ResourceRecord& parsed : parsedRecords)
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

                    result.records.push_back(std::move(parsed));
                    ++result.recordsLoaded;
                }
            }

            std::ostringstream message;
            message << "Loaded " << result.recordsLoaded << " cached records (legacy ASCII)";
            if (result.recordsSkipped > 0)
            {
                message << " (" << result.recordsSkipped << " stale or invalid skipped)";
            }
            message << '.';
            result.message = message.str();
            return result;
        }
    }

    IndexCacheLoadResult LoadIndexCacheRecords(
        const fs::path& cachePath,
        const fs::path& gameRoot,
        bool validateOnDisk)
    {
        const auto loadStart = std::chrono::steady_clock::now();
        IndexCacheLoadResult result;

        BinaryCacheOpenResult opened = OpenBinaryCacheFile(
            cachePath,
            BinaryCacheKind::Index,
            kIndexBinarySchemaVersion);

        if (!opened.fileFound)
        {
            result.message = "No index cache file found.";
            return result;
        }

        result.fileFound = true;
        if (!opened.isBinaryMagic)
        {
            opened.mapped.Close();
            IndexCacheLoadResult legacy = LoadLegacyAsciiIndexCache(cachePath, gameRoot, validateOnDisk);
            legacy.elapsedMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - loadStart).count());
            return legacy;
        }

        if (opened.corrupt)
        {
            result.corrupt = true;
            result.message = opened.error;
            return result;
        }

        if (!opened.valid)
        {
            result.message = opened.error;
            return result;
        }

        const uint64_t expectedRootHash =
            Platform::ComputeFnv1a64(Platform::CanonicalizePathUtf8(gameRoot));
        if (opened.header.rootHash != expectedRootHash)
        {
            result.message = "Index cache belongs to another game folder.";
            return result;
        }

        BinaryCursor cursor(opened.payload);
        std::string_view cachedRootUtf8;
        if (!cursor.ReadStringView(cachedRootUtf8))
        {
            result.corrupt = true;
            result.message = "Index cache is missing its game root (truncated).";
            return result;
        }

        if (cachedRootUtf8 != Platform::CanonicalizePathUtf8(gameRoot))
        {
            result.message = "Index cache belongs to another game folder.";
            return result;
        }

        result.rootMatched = true;

        std::vector<std::string_view> dirViews;
        std::string tableError;
        if (!ReadStringTable(cursor, dirViews, tableError))
        {
            result.corrupt = true;
            result.message = tableError;
            return result;
        }

        std::vector<std::wstring> dirWstrings;
        dirWstrings.reserve(dirViews.size());
        for (const std::string_view dirView : dirViews)
        {
            dirWstrings.push_back(Platform::Utf8ToWide(dirView));
        }

        uint32_t recordCount = 0;
        if (!cursor.ReadU32(recordCount))
        {
            result.corrupt = true;
            result.message = "Index cache record count is truncated.";
            return result;
        }

        result.records.reserve(recordCount);
        for (uint32_t i = 0; i < recordCount; ++i)
        {
            uint32_t parentDirId = 0;
            std::string_view fileNameUtf8;
            uint8_t rawType = 0;
            uint64_t sizeBytes = 0;
            int64_t lastWriteTicks = 0;

            if (!cursor.ReadU32(parentDirId) ||
                !cursor.ReadStringView(fileNameUtf8) ||
                !cursor.ReadU8(rawType) ||
                !cursor.ReadU64(sizeBytes) ||
                !cursor.ReadI64(lastWriteTicks))
            {
                result.corrupt = true;
                result.message = "Index cache record entry is truncated or malformed.";
                result.records.clear();
                return result;
            }

            if (parentDirId >= dirWstrings.size() || rawType > static_cast<uint8_t>(AssetType::Unknown))
            {
                result.corrupt = true;
                result.message = "Index cache record references an out-of-bounds directory or invalid asset type.";
                result.records.clear();
                return result;
            }

            std::wstring fullPath = dirWstrings[parentDirId];
            const std::wstring wideName = Platform::Utf8ToWide(fileNameUtf8);
            if (!fullPath.empty() && fullPath.back() != L'\\' && fullPath.back() != L'/')
            {
                fullPath.push_back(L'\\');
            }
            fullPath.append(wideName);

            ResourceRecord record;
            record.path = fs::path(std::move(fullPath));
            record.type = static_cast<AssetType>(rawType);
            record.sizeBytes = static_cast<uintmax_t>(sizeBytes);
            record.lastWriteTicks = lastWriteTicks;

            if (validateOnDisk)
            {
                uintmax_t currentSize = 0;
                int64_t currentTicks = 0;
                if (!TryReadFileMetadata(record.path, currentSize, currentTicks) ||
                    currentSize != record.sizeBytes ||
                    currentTicks != record.lastWriteTicks)
                {
                    ++result.recordsSkipped;
                    continue;
                }
            }

            result.records.push_back(std::move(record));
            ++result.recordsLoaded;
        }

        uint32_t dirStampCount = 0;
        if (!cursor.ReadU32(dirStampCount))
        {
            result.corrupt = true;
            result.message = "Index cache directory stamp table is truncated.";
            result.records.clear();
            return result;
        }

        result.directories.reserve(dirStampCount);
        for (uint32_t i = 0; i < dirStampCount; ++i)
        {
            std::string_view dirPathUtf8;
            int64_t ticks = 0;
            if (!cursor.ReadStringView(dirPathUtf8) || !cursor.ReadI64(ticks))
            {
                result.corrupt = true;
                result.message = "Index cache directory stamp entry is truncated.";
                result.records.clear();
                result.directories.clear();
                return result;
            }

            result.directories.push_back(DirectoryStamp{
                Platform::Utf8ToPath(dirPathUtf8),
                ticks
            });
        }

        result.elapsedMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - loadStart).count());

        std::ostringstream message;
        message << "Loaded " << result.recordsLoaded << " cached records in " << result.elapsedMs << " ms";
        if (result.recordsSkipped > 0)
        {
            message << " (" << result.recordsSkipped << " stale or invalid skipped)";
        }
        message << '.';
        result.message = message.str();
        return result;
    }

    IndexDeltaResult ComputeIndexDelta(
        const fs::path& root,
        const std::vector<ResourceRecord>& cachedRecords,
        const DiscoverOptions& options)
    {
        const auto start = std::chrono::steady_clock::now();
        IndexDeltaResult delta;

        DiscoverResult live = DiscoverResources(root, options);
        delta.filesVisited = live.filesVisited;
        if (live.canceled)
        {
            delta.canceled = true;
            delta.elapsedMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count());
            return delta;
        }
        if (!live.error.empty())
        {
            delta.error = std::move(live.error);
            delta.elapsedMs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count());
            return delta;
        }

        struct CachedFingerprint
        {
            AssetType type = AssetType::Unknown;
            uintmax_t sizeBytes = 0;
            int64_t lastWriteTicks = 0;
            bool seen = false;
        };

        std::unordered_map<std::wstring, CachedFingerprint> cachedMap;
        cachedMap.reserve(cachedRecords.size());
        for (const ResourceRecord& cached : cachedRecords)
        {
            cachedMap.emplace(
                NormalizePathForCompare(cached.path),
                CachedFingerprint{ cached.type, cached.sizeBytes, cached.lastWriteTicks, false });
        }

        for (const ResourceRecord& record : live.records)
        {
            const std::wstring key = NormalizePathForCompare(record.path);
            const auto it = cachedMap.find(key);
            if (it == cachedMap.end())
            {
                ++delta.addedCount;
            }
            else
            {
                it->second.seen = true;
                if (it->second.type != record.type ||
                    it->second.sizeBytes != record.sizeBytes ||
                    it->second.lastWriteTicks != record.lastWriteTicks)
                {
                    ++delta.modifiedCount;
                }
            }
        }

        for (const auto& [_, entry] : cachedMap)
        {
            if (!entry.seen)
            {
                ++delta.removedCount;
            }
        }

        delta.hasChanges = (delta.addedCount > 0 || delta.modifiedCount > 0 || delta.removedCount > 0);
        delta.updatedRecords = std::move(live.records);
        delta.updatedDirectories = std::move(live.directories);
        delta.elapsedMs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count());
        return delta;
    }
}
