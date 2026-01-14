#include <windows.h>
#include <d3d11.h>
#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tchar.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cwctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace
{
    namespace fs = std::filesystem;

    ID3D11Device* GDevice = nullptr;
    ID3D11DeviceContext* GDeviceContext = nullptr;
    IDXGISwapChain* GSwapChain = nullptr;
    ID3D11RenderTargetView* GMainRenderTargetView = nullptr;

    constexpr size_t PathBufferSize = 1024;
    constexpr size_t SearchBufferSize = 256;

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
        fs::path path;
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

        bool IsValid() const
        {
            return exists && hasContent && (hasAces || hasBaseRes || hasKnownContainer);
        }
    };

    struct AppSettings
    {
        fs::path gameRoot;
        fs::path exportRoot;
        std::string textureExportFormat = "DDS";
        std::string modelExportFormat = "GLB";
        bool exportTexturesWithModels = true;
        bool preserveOriginalDDS = true;
        bool useOodleFromGameFolder = true;
        bool indexCacheEnabled = true;
        std::string lastOpenedPackFilters;
    };

    enum class LogLevel
    {
        Info,
        Warning,
        Error
    };

    struct LogEntry
    {
        std::string time;
        LogLevel level = LogLevel::Info;
        std::string message;
    };

    struct ScanState
    {
        std::atomic_bool running = false;
        std::atomic_bool cancelRequested = false;
        std::atomic_bool completed = false;
        std::atomic_bool failed = false;
        std::atomic<uint64_t> filesVisited = 0;
        std::atomic<uint64_t> recordsFound = 0;
        std::mutex mutex;
        std::string status = "Idle";
        std::string cacheStatus = "Not loaded";
        bool cacheLoaded = false;
        std::vector<ResourceRecord> records;
        std::thread worker;
    };

    struct AppState
    {
        HWND hwnd = nullptr;
        AppSettings settings;
        fs::path settingsPath;
        fs::path indexCachePath;
        ValidationResult validation;
        ScanState scan;
        std::mutex logMutex;
        std::vector<LogEntry> logs;

        std::array<char, PathBufferSize> gameRootBuffer = {};
        std::array<char, PathBufferSize> exportRootBuffer = {};
        std::array<char, SearchBufferSize> searchBuffer = {};
        std::wstring selectedPath;

        bool firstRunPromptOpen = false;
        bool settingsWindowOpen = false;
        bool aboutWindowOpen = false;
        bool showVromfs = true;
        bool showGrp = true;
        bool showDxp = true;
        bool showDescriptors = true;
        bool showBlk = true;
    };

    std::string WideToUtf8(std::wstring_view wide)
    {
        if (wide.empty())
        {
            return {};
        }

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
    }

    std::wstring Utf8ToWide(std::string_view utf8)
    {
        if (utf8.empty())
        {
            return {};
        }

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
    }

    std::string PathToUtf8(const fs::path& path)
    {
        return WideToUtf8(path.wstring());
    }

    fs::path Utf8ToPath(std::string_view value)
    {
        return fs::path(Utf8ToWide(value));
    }

    void CopyToBuffer(std::array<char, PathBufferSize>& buffer, const std::string& value)
    {
        buffer.fill('\0');
        strncpy_s(buffer.data(), buffer.size(), value.c_str(), _TRUNCATE);
    }

    std::string ToLowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
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

    std::string Trim(std::string value)
    {
        const auto isSpace = [](unsigned char c) {
            return std::isspace(c) != 0;
        };

        value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c) {
            return !isSpace(static_cast<unsigned char>(c));
        }));
        value.erase(std::find_if(value.rbegin(), value.rend(), [&](char c) {
            return !isSpace(static_cast<unsigned char>(c));
        }).base(), value.end());
        return value;
    }

    std::string NowString()
    {
        const auto now = std::chrono::system_clock::now();
        const std::time_t time = std::chrono::system_clock::to_time_t(now);
        std::tm localTime = {};
        localtime_s(&localTime, &time);

        char buffer[32] = {};
        std::strftime(buffer, sizeof(buffer), "%H:%M:%S", &localTime);
        return buffer;
    }

    std::string FormatBytes(uintmax_t bytes)
    {
        const char* units[] = { "B", "KB", "MB", "GB" };
        double value = static_cast<double>(bytes);
        size_t unit = 0;
        while (value >= 1024.0 && unit + 1 < std::size(units))
        {
            value /= 1024.0;
            ++unit;
        }

        std::ostringstream stream;
        if (unit == 0)
        {
            stream << static_cast<uintmax_t>(value) << ' ' << units[unit];
        }
        else
        {
            stream << std::fixed << std::setprecision(1) << value << ' ' << units[unit];
        }
        return stream.str();
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

    ImVec4 LevelColor(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Warning:
            return ImVec4(1.0f, 0.74f, 0.27f, 1.0f);
        case LogLevel::Error:
            return ImVec4(1.0f, 0.38f, 0.34f, 1.0f);
        default:
            return ImVec4(0.72f, 0.84f, 1.0f, 1.0f);
        }
    }

    const char* LevelName(LogLevel level)
    {
        switch (level)
        {
        case LogLevel::Warning:
            return "Warning";
        case LogLevel::Error:
            return "Error";
        default:
            return "Info";
        }
    }

    void AddLog(AppState& app, LogLevel level, std::string message)
    {
        std::lock_guard<std::mutex> lock(app.logMutex);
        app.logs.push_back(LogEntry{ NowString(), level, std::move(message) });
        if (app.logs.size() > 1000)
        {
            app.logs.erase(app.logs.begin(), app.logs.begin() + 250);
        }
    }

    std::optional<fs::path> GetKnownFolderPath(REFKNOWNFOLDERID folderId)
    {
        PWSTR rawPath = nullptr;
        if (FAILED(SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &rawPath)))
        {
            return std::nullopt;
        }

        fs::path result(rawPath);
        CoTaskMemFree(rawPath);
        return result;
    }

    fs::path GetSettingsPath()
    {
        if (const auto appData = GetKnownFolderPath(FOLDERID_RoamingAppData))
        {
            return *appData / L"DagorAssetsExtractor" / L"settings.ini";
        }

        return fs::current_path() / L"settings.ini";
    }

    fs::path GetIndexCachePath()
    {
        if (const auto appData = GetKnownFolderPath(FOLDERID_RoamingAppData))
        {
            return *appData / L"DagorAssetsExtractor" / L"index-cache.txt";
        }

        return fs::current_path() / L"index-cache.txt";
    }

    fs::path GetDefaultExportRoot()
    {
        if (const auto documents = GetKnownFolderPath(FOLDERID_Documents))
        {
            return *documents / L"DagorAssetsExtractorExports";
        }

        return fs::current_path() / L"exports";
    }

    bool ParseBool(std::string_view value, bool fallback)
    {
        const std::string normalized = ToLowerAscii(Trim(std::string(value)));
        if (normalized == "1" || normalized == "true" || normalized == "yes")
        {
            return true;
        }
        if (normalized == "0" || normalized == "false" || normalized == "no")
        {
            return false;
        }
        return fallback;
    }

    void LoadSettings(AppSettings& settings, const fs::path& path)
    {
        settings.exportRoot = GetDefaultExportRoot();

        std::ifstream file(path);
        if (!file)
        {
            return;
        }

        std::string line;
        while (std::getline(file, line))
        {
            line = Trim(line);
            if (line.empty() || line[0] == '#')
            {
                continue;
            }

            const size_t separator = line.find('=');
            if (separator == std::string::npos)
            {
                continue;
            }

            const std::string key = Trim(line.substr(0, separator));
            const std::string value = Trim(line.substr(separator + 1));

            if (key == "gameRoot")
            {
                settings.gameRoot = Utf8ToPath(value);
            }
            else if (key == "exportRoot")
            {
                settings.exportRoot = Utf8ToPath(value);
            }
            else if (key == "textureExportFormat")
            {
                settings.textureExportFormat = value;
            }
            else if (key == "modelExportFormat")
            {
                settings.modelExportFormat = value;
            }
            else if (key == "exportTexturesWithModels")
            {
                settings.exportTexturesWithModels = ParseBool(value, settings.exportTexturesWithModels);
            }
            else if (key == "preserveOriginalDDS")
            {
                settings.preserveOriginalDDS = ParseBool(value, settings.preserveOriginalDDS);
            }
            else if (key == "useOodleFromGameFolder")
            {
                settings.useOodleFromGameFolder = ParseBool(value, settings.useOodleFromGameFolder);
            }
            else if (key == "indexCacheEnabled")
            {
                settings.indexCacheEnabled = ParseBool(value, settings.indexCacheEnabled);
            }
            else if (key == "lastOpenedPackFilters")
            {
                settings.lastOpenedPackFilters = value;
            }
        }
    }

    bool SaveSettings(const AppSettings& settings, const fs::path& path, std::string& error)
    {
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        if (ec)
        {
            error = "Could not create settings folder: " + ec.message();
            return false;
        }

        std::ofstream file(path, std::ios::trunc);
        if (!file)
        {
            error = "Could not open settings file for writing.";
            return false;
        }

        file << "gameRoot=" << PathToUtf8(settings.gameRoot) << '\n';
        file << "exportRoot=" << PathToUtf8(settings.exportRoot) << '\n';
        file << "textureExportFormat=" << settings.textureExportFormat << '\n';
        file << "modelExportFormat=" << settings.modelExportFormat << '\n';
        file << "exportTexturesWithModels=" << (settings.exportTexturesWithModels ? "true" : "false") << '\n';
        file << "preserveOriginalDDS=" << (settings.preserveOriginalDDS ? "true" : "false") << '\n';
        file << "useOodleFromGameFolder=" << (settings.useOodleFromGameFolder ? "true" : "false") << '\n';
        file << "indexCacheEnabled=" << (settings.indexCacheEnabled ? "true" : "false") << '\n';
        file << "lastOpenedPackFilters=" << settings.lastOpenedPackFilters << '\n';
        return true;
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

    bool ContainsKnownContainer(const fs::path& root)
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

        while (!ec && iterator != end && visited < 20000)
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

    std::optional<fs::path> BrowseForFolder(HWND owner, const wchar_t* title, const fs::path& initialPath)
    {
        IFileOpenDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        {
            return std::nullopt;
        }

        DWORD options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options)))
        {
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        }

        dialog->SetTitle(title);

        std::error_code ec;
        if (!initialPath.empty() && fs::exists(initialPath, ec))
        {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(initialPath.wstring().c_str(), nullptr, IID_PPV_ARGS(&item))))
            {
                dialog->SetFolder(item);
                item->Release();
            }
        }

        std::optional<fs::path> selected;
        if (SUCCEEDED(dialog->Show(owner)))
        {
            IShellItem* result = nullptr;
            if (SUCCEEDED(dialog->GetResult(&result)))
            {
                PWSTR rawPath = nullptr;
                if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)))
                {
                    selected = fs::path(rawPath);
                    CoTaskMemFree(rawPath);
                }
                result->Release();
            }
        }

        dialog->Release();
        return selected;
    }

    void SetScanStatus(ScanState& scan, std::string status)
    {
        std::lock_guard<std::mutex> lock(scan.mutex);
        scan.status = std::move(status);
    }

    std::string GetScanStatus(ScanState& scan)
    {
        std::lock_guard<std::mutex> lock(scan.mutex);
        return scan.status;
    }

    std::vector<ResourceRecord> SnapshotRecords(ScanState& scan)
    {
        std::lock_guard<std::mutex> lock(scan.mutex);
        return scan.records;
    }

    std::string GetCacheStatus(ScanState& scan)
    {
        std::lock_guard<std::mutex> lock(scan.mutex);
        return scan.cacheStatus;
    }

    void SetCacheStatus(ScanState& scan, std::string status, bool loaded)
    {
        std::lock_guard<std::mutex> lock(scan.mutex);
        scan.cacheStatus = std::move(status);
        scan.cacheLoaded = loaded;
    }

    struct IndexCacheLoadResult
    {
        bool fileFound = false;
        bool rootMatched = false;
        size_t recordsLoaded = 0;
        size_t recordsSkipped = 0;
        std::string message;
        std::vector<ResourceRecord> records;
    };

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
        file << "root " << std::quoted(PathToUtf8(gameRoot)) << '\n';
        for (const ResourceRecord& record : records)
        {
            file << "record "
                 << std::quoted(PathToUtf8(record.path)) << ' '
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
                    cachedRoot = Utf8ToPath(rootUtf8);
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
                record.path = Utf8ToPath(pathUtf8);
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

    void StopScan(AppState& app)
    {
        if (app.scan.running.load())
        {
            app.scan.cancelRequested.store(true);
        }

        if (app.scan.worker.joinable())
        {
            app.scan.worker.join();
        }
    }

    void JoinFinishedScan(AppState& app)
    {
        if (!app.scan.running.load() && app.scan.worker.joinable())
        {
            app.scan.worker.join();
        }
    }

    void ClearResourceIndex(AppState& app, std::string status)
    {
        {
            std::lock_guard<std::mutex> lock(app.scan.mutex);
            app.scan.records.clear();
            app.scan.status = std::move(status);
            app.scan.cacheStatus = app.settings.indexCacheEnabled ? "Not loaded" : "Disabled";
            app.scan.cacheLoaded = false;
        }

        app.selectedPath.clear();
        app.scan.filesVisited.store(0);
        app.scan.recordsFound.store(0);
        app.scan.completed.store(false);
        app.scan.failed.store(false);
    }

    void LoadIndexCacheIntoApp(AppState& app)
    {
        if (!app.settings.indexCacheEnabled)
        {
            SetCacheStatus(app.scan, "Disabled", false);
            return;
        }

        if (!app.validation.IsValid())
        {
            SetCacheStatus(app.scan, "No valid game folder", false);
            return;
        }

        const IndexCacheLoadResult result = LoadIndexCacheRecords(app.indexCachePath, app.settings.gameRoot);
        if (!result.fileFound)
        {
            SetCacheStatus(app.scan, result.message, false);
            return;
        }

        if (!result.rootMatched)
        {
            SetCacheStatus(app.scan, result.message, false);
            AddLog(app, LogLevel::Warning, result.message);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(app.scan.mutex);
            app.scan.records = result.records;
            app.scan.status = result.records.empty() ? "Idle" : "Loaded from cache";
            app.scan.cacheStatus = result.message;
            app.scan.cacheLoaded = !result.records.empty();
        }

        app.selectedPath.clear();
        app.scan.filesVisited.store(0);
        app.scan.recordsFound.store(static_cast<uint64_t>(result.recordsLoaded));
        app.scan.completed.store(!result.records.empty());
        app.scan.failed.store(false);

        AddLog(app, LogLevel::Info, result.message);
    }

    void StartScan(AppState& app)
    {
        JoinFinishedScan(app);
        if (app.scan.running.load())
        {
            AddLog(app, LogLevel::Warning, "A scan is already running.");
            return;
        }

        if (!app.validation.IsValid())
        {
            AddLog(app, LogLevel::Warning, "Choose a valid War Thunder folder before scanning.");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(app.scan.mutex);
            app.scan.records.clear();
            app.scan.status = "Preparing scan";
            app.scan.cacheStatus = app.settings.indexCacheEnabled ? "Refreshing" : "Disabled";
            app.scan.cacheLoaded = false;
        }

        app.selectedPath.clear();
        app.scan.filesVisited.store(0);
        app.scan.recordsFound.store(0);
        app.scan.cancelRequested.store(false);
        app.scan.completed.store(false);
        app.scan.failed.store(false);
        app.scan.running.store(true);

        const fs::path root = app.settings.gameRoot;
        const fs::path cachePath = app.indexCachePath;
        const bool writeIndexCache = app.settings.indexCacheEnabled;
        AddLog(app, LogLevel::Info, "Started resource discovery for " + PathToUtf8(root));

        app.scan.worker = std::thread([&app, root, cachePath, writeIndexCache]() {
            std::error_code ec;
            try
            {
                fs::recursive_directory_iterator iterator(
                    root,
                    fs::directory_options::skip_permission_denied,
                    ec);
                const fs::recursive_directory_iterator end;

                if (ec)
                {
                    throw std::runtime_error(ec.message());
                }

                while (iterator != end)
                {
                    if (app.scan.cancelRequested.load())
                    {
                        SetScanStatus(app.scan, "Canceled");
                        AddLog(app, LogLevel::Warning, "Resource discovery was canceled.");
                        break;
                    }

                    const fs::directory_entry entry = *iterator;
                    app.scan.filesVisited.fetch_add(1);

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

                                {
                                    std::lock_guard<std::mutex> lock(app.scan.mutex);
                                    app.scan.records.push_back(std::move(record));
                                    std::ostringstream status;
                                    status << "Scanning - " << app.scan.records.size() << " records found";
                                    app.scan.status = status.str();
                                }
                                app.scan.recordsFound.fetch_add(1);
                            }
                        }
                    }

                    iterator.increment(ec);
                    if (ec)
                    {
                        AddLog(app, LogLevel::Warning, "Skipped inaccessible path during scan: " + ec.message());
                        ec.clear();
                    }
                }

                if (!app.scan.cancelRequested.load())
                {
                    std::ostringstream message;
                    message << "Resource discovery completed: "
                            << app.scan.recordsFound.load()
                            << " relevant files found after visiting "
                            << app.scan.filesVisited.load()
                            << " paths.";
                    SetScanStatus(app.scan, "Completed");
                    AddLog(app, LogLevel::Info, message.str());
                    app.scan.completed.store(true);

                    if (writeIndexCache)
                    {
                        std::string cacheError;
                        const std::vector<ResourceRecord> records = SnapshotRecords(app.scan);
                        if (SaveIndexCacheRecords(cachePath, root, records, cacheError))
                        {
                            std::ostringstream cacheMessage;
                            cacheMessage << "Index cache saved with " << records.size() << " records.";
                            SetCacheStatus(app.scan, cacheMessage.str(), true);
                            AddLog(app, LogLevel::Info, cacheMessage.str());
                        }
                        else
                        {
                            SetCacheStatus(app.scan, "Cache save failed", false);
                            AddLog(app, LogLevel::Warning, cacheError);
                        }
                    }
                }
            }
            catch (const std::exception& exception)
            {
                app.scan.failed.store(true);
                SetScanStatus(app.scan, "Failed");
                AddLog(app, LogLevel::Error, std::string("Resource discovery failed: ") + exception.what());
            }

            app.scan.running.store(false);
        });
    }

    void SyncSettingsBuffers(AppState& app)
    {
        CopyToBuffer(app.gameRootBuffer, PathToUtf8(app.settings.gameRoot));
        CopyToBuffer(app.exportRootBuffer, PathToUtf8(app.settings.exportRoot));
    }

    void SetGameRoot(AppState& app, const fs::path& gameRoot)
    {
        StopScan(app);
        app.settings.gameRoot = gameRoot;
        app.validation = ValidateGameRoot(app.settings.gameRoot);
        SyncSettingsBuffers(app);
        ClearResourceIndex(app, "Idle");
        LoadIndexCacheIntoApp(app);
    }

    void ApplyGameRootBuffer(AppState& app)
    {
        SetGameRoot(app, Utf8ToPath(app.gameRootBuffer.data()));
    }

    void SaveSettingsWithLog(AppState& app)
    {
        std::string error;
        if (SaveSettings(app.settings, app.settingsPath, error))
        {
            AddLog(app, LogLevel::Info, "Settings saved to " + PathToUtf8(app.settingsPath));
        }
        else
        {
            AddLog(app, LogLevel::Error, error);
        }
    }

    void DrawValidationSummary(const ValidationResult& validation)
    {
        ImGui::TextColored(validation.IsValid() ? ImVec4(0.43f, 0.86f, 0.55f, 1.0f) : ImVec4(1.0f, 0.49f, 0.42f, 1.0f),
            validation.IsValid() ? "Valid War Thunder folder" : "No valid game folder");

        for (const std::string& message : validation.messages)
        {
            ImGui::BulletText("%s", message.c_str());
        }
    }

    void DrawMenuBar(AppState& app, bool& running)
    {
        if (!ImGui::BeginMenuBar())
        {
            return;
        }

        if (ImGui::BeginMenu("File"))
        {
            if (ImGui::MenuItem("Choose Game Folder"))
            {
                if (const auto selected = BrowseForFolder(app.hwnd, L"Choose War Thunder Folder", app.settings.gameRoot))
                {
                    SetGameRoot(app, *selected);
                    SaveSettingsWithLog(app);
                }
            }

            if (ImGui::MenuItem("Save Settings"))
            {
                SaveSettingsWithLog(app);
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Exit"))
            {
                running = false;
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View"))
        {
            ImGui::MenuItem("Settings", nullptr, &app.settingsWindowOpen);
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Tools"))
        {
            const bool canLoadCache = app.validation.IsValid() && app.settings.indexCacheEnabled && !app.scan.running.load();
            ImGui::BeginDisabled(!canLoadCache);
            if (ImGui::MenuItem("Load Cached Index"))
            {
                LoadIndexCacheIntoApp(app);
            }
            ImGui::EndDisabled();

            const bool canScan = app.validation.IsValid() && !app.scan.running.load();
            ImGui::BeginDisabled(!canScan);
            if (ImGui::MenuItem("Rebuild Resource Index"))
            {
                StartScan(app);
            }
            ImGui::EndDisabled();

            ImGui::BeginDisabled(!app.scan.running.load());
            if (ImGui::MenuItem("Cancel Scan"))
            {
                app.scan.cancelRequested.store(true);
            }
            ImGui::EndDisabled();
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help"))
        {
            if (ImGui::MenuItem("About"))
            {
                app.aboutWindowOpen = true;
            }
            ImGui::EndMenu();
        }

        ImGui::EndMenuBar();
    }

    void DrawHeader(AppState& app)
    {
        ImGui::TextUnformatted("Dagor Assets Extractor");
        ImGui::SameLine();
        ImGui::TextColored(
            app.validation.IsValid() ? ImVec4(0.43f, 0.86f, 0.55f, 1.0f) : ImVec4(1.0f, 0.49f, 0.42f, 1.0f),
            app.validation.IsValid() ? "Ready" : "Game folder required");

        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 185.0f);
        ImGui::BeginDisabled(!app.validation.IsValid() || app.scan.running.load());
        if (ImGui::Button("Rebuild Index", ImVec2(110.0f, 0.0f)))
        {
            StartScan(app);
        }
        ImGui::EndDisabled();

        ImGui::SameLine();
        ImGui::BeginDisabled(!app.scan.running.load());
        if (ImGui::Button("Cancel", ImVec2(70.0f, 0.0f)))
        {
            app.scan.cancelRequested.store(true);
        }
        ImGui::EndDisabled();

        ImGui::Separator();
    }

    bool RecordPassesTypeFilter(const AppState& app, AssetType type)
    {
        switch (type)
        {
        case AssetType::Vromfs:
            return app.showVromfs;
        case AssetType::Grp:
            return app.showGrp;
        case AssetType::Dxp:
            return app.showDxp;
        case AssetType::Descriptor:
            return app.showDescriptors;
        case AssetType::Blk:
            return app.showBlk;
        default:
            return true;
        }
    }

    bool RecordPassesSearch(const ResourceRecord& record, const std::string& search)
    {
        if (search.empty())
        {
            return true;
        }

        const std::string haystack = ToLowerAscii(PathToUtf8(record.path));
        return haystack.find(search) != std::string::npos;
    }

    void DrawLeftPanel(AppState& app, const std::vector<ResourceRecord>& records)
    {
        ImGui::TextUnformatted("Game Install");
        ImGui::Separator();
        if (app.settings.gameRoot.empty())
        {
            ImGui::TextWrapped("No folder selected.");
        }
        else
        {
            ImGui::TextWrapped("%s", PathToUtf8(app.settings.gameRoot).c_str());
        }

        if (ImGui::Button("Choose Folder", ImVec2(-1.0f, 0.0f)))
        {
            if (const auto selected = BrowseForFolder(app.hwnd, L"Choose War Thunder Folder", app.settings.gameRoot))
            {
                SetGameRoot(app, *selected);
                SaveSettingsWithLog(app);
            }
        }

        ImGui::Spacing();
        DrawValidationSummary(app.validation);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Filters");
        ImGui::Checkbox("VROMFS", &app.showVromfs);
        ImGui::Checkbox("GRP", &app.showGrp);
        ImGui::Checkbox("DXP", &app.showDxp);
        ImGui::Checkbox("Descriptors", &app.showDescriptors);
        ImGui::Checkbox("BLK", &app.showBlk);

        uint32_t counts[5] = {};
        for (const ResourceRecord& record : records)
        {
            switch (record.type)
            {
            case AssetType::Vromfs:
                ++counts[0];
                break;
            case AssetType::Grp:
                ++counts[1];
                break;
            case AssetType::Dxp:
                ++counts[2];
                break;
            case AssetType::Descriptor:
                ++counts[3];
                break;
            case AssetType::Blk:
                ++counts[4];
                break;
            default:
                break;
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextUnformatted("Discovered");
        ImGui::BulletText("VROMFS: %u", counts[0]);
        ImGui::BulletText("GRP: %u", counts[1]);
        ImGui::BulletText("DXP: %u", counts[2]);
        ImGui::BulletText("Descriptors: %u", counts[3]);
        ImGui::BulletText("BLK: %u", counts[4]);
        ImGui::Spacing();
        ImGui::TextWrapped("Cache: %s", GetCacheStatus(app.scan).c_str());
    }

    void DrawContentBrowser(AppState& app, const std::vector<ResourceRecord>& records)
    {
        ImGui::TextUnformatted("Content Browser");
        ImGui::Separator();
        ImGui::InputTextWithHint("##Search", "Search discovered packs and descriptors", app.searchBuffer.data(), app.searchBuffer.size());

        const std::string search = ToLowerAscii(Trim(app.searchBuffer.data()));
        uint32_t shown = 0;
        uint32_t matched = 0;
        constexpr uint32_t displayLimit = 5000;

        ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("ResourceRecords", 4, flags, ImVec2(0.0f, -24.0f)))
        {
            ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 92.0f);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.40f);
            ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 0.60f);
            ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 88.0f);
            ImGui::TableHeadersRow();

            for (const ResourceRecord& record : records)
            {
                if (!RecordPassesTypeFilter(app, record.type) || !RecordPassesSearch(record, search))
                {
                    continue;
                }

                ++matched;
                if (shown >= displayLimit)
                {
                    continue;
                }

                ++shown;
                const std::wstring nativePath = record.path.wstring();
                const bool selected = nativePath == app.selectedPath;

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(AssetTypeName(record.type));

                ImGui::TableSetColumnIndex(1);
                const std::string name = PathToUtf8(record.path.filename());
                ImGuiSelectableFlags selectableFlags = ImGuiSelectableFlags_SpanAllColumns;
                if (ImGui::Selectable(name.c_str(), selected, selectableFlags))
                {
                    app.selectedPath = nativePath;
                }

                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(PathToUtf8(record.path.parent_path()).c_str());

                ImGui::TableSetColumnIndex(3);
                ImGui::TextUnformatted(FormatBytes(record.sizeBytes).c_str());
            }

            ImGui::EndTable();
        }

        if (matched > displayLimit)
        {
            ImGui::Text("Showing first %u of %u matching records.", displayLimit, matched);
        }
        else
        {
            ImGui::Text("%u matching records.", matched);
        }
    }

    std::optional<ResourceRecord> FindSelectedRecord(const AppState& app, const std::vector<ResourceRecord>& records)
    {
        if (app.selectedPath.empty())
        {
            return std::nullopt;
        }

        for (const ResourceRecord& record : records)
        {
            if (record.path.wstring() == app.selectedPath)
            {
                return record;
            }
        }

        return std::nullopt;
    }

    void DrawInspector(AppState& app, const std::vector<ResourceRecord>& records)
    {
        ImGui::TextUnformatted("Inspector");
        ImGui::Separator();

        const std::optional<ResourceRecord> selected = FindSelectedRecord(app, records);
        if (!selected)
        {
            ImGui::TextWrapped("Select a discovered resource pack, descriptor, or BLK file to inspect its metadata.");
            return;
        }

        ImGui::Text("Type: %s", AssetTypeName(selected->type));
        ImGui::Text("Name: %s", PathToUtf8(selected->path.filename()).c_str());
        ImGui::Text("Size: %s", FormatBytes(selected->sizeBytes).c_str());
        ImGui::Separator();
        ImGui::TextUnformatted("Full Path");
        ImGui::TextWrapped("%s", PathToUtf8(selected->path).c_str());
        ImGui::Separator();
        ImGui::TextUnformatted("Current Support");
        ImGui::BulletText("Discovery and metadata only.");
        ImGui::BulletText("Parsing and export are intentionally not enabled yet.");
        ImGui::BulletText("Future export manifests will record this source path and decode warnings.");
    }

    void DrawJobAndLogPanel(AppState& app)
    {
        ImGui::TextUnformatted("Jobs and Logs");
        ImGui::Separator();
        ImGui::Text("Scan: %s", GetScanStatus(app.scan).c_str());
        ImGui::SameLine();
        ImGui::Text("Visited: %llu", static_cast<unsigned long long>(app.scan.filesVisited.load()));
        ImGui::SameLine();
        ImGui::Text("Found: %llu", static_cast<unsigned long long>(app.scan.recordsFound.load()));
        ImGui::Text("Cache: %s", GetCacheStatus(app.scan).c_str());

        ImGui::Separator();

        std::vector<LogEntry> logs;
        {
            std::lock_guard<std::mutex> lock(app.logMutex);
            logs = app.logs;
        }

        if (ImGui::BeginTable("Logs", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0.0f, 0.0f)))
        {
            ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 72.0f);
            ImGui::TableSetupColumn("Level", ImGuiTableColumnFlags_WidthFixed, 82.0f);
            ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (const LogEntry& entry : logs)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(entry.time.c_str());
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(LevelColor(entry.level), "%s", LevelName(entry.level));
                ImGui::TableSetColumnIndex(2);
                ImGui::TextWrapped("%s", entry.message.c_str());
            }

            ImGui::EndTable();
        }
    }

    void DrawSettingsWindow(AppState& app)
    {
        if (!app.settingsWindowOpen)
        {
            return;
        }

        if (!ImGui::Begin("Settings", &app.settingsWindowOpen))
        {
            ImGui::End();
            return;
        }

        ImGui::TextUnformatted("Game");
        ImGui::InputText("Game Root", app.gameRootBuffer.data(), app.gameRootBuffer.size());
        ImGui::SameLine();
        if (ImGui::Button("Browse##GameRoot"))
        {
            if (const auto selected = BrowseForFolder(app.hwnd, L"Choose War Thunder Folder", app.settings.gameRoot))
            {
                SetGameRoot(app, *selected);
            }
        }

        if (ImGui::Button("Validate Game Folder"))
        {
            ApplyGameRootBuffer(app);
        }

        ImGui::Spacing();
        DrawValidationSummary(app.validation);

        ImGui::Separator();
        ImGui::TextUnformatted("Export Defaults");
        ImGui::InputText("Export Root", app.exportRootBuffer.data(), app.exportRootBuffer.size());
        ImGui::SameLine();
        if (ImGui::Button("Browse##ExportRoot"))
        {
            if (const auto selected = BrowseForFolder(app.hwnd, L"Choose Export Folder", app.settings.exportRoot))
            {
                app.settings.exportRoot = *selected;
                SyncSettingsBuffers(app);
            }
        }

        ImGui::Checkbox("Export textures with models", &app.settings.exportTexturesWithModels);
        ImGui::Checkbox("Preserve original DDS when possible", &app.settings.preserveOriginalDDS);
        ImGui::Checkbox("Use Oodle DLL from game folder if present", &app.settings.useOodleFromGameFolder);
        if (ImGui::Checkbox("Enable index cache", &app.settings.indexCacheEnabled))
        {
            if (app.settings.indexCacheEnabled)
            {
                LoadIndexCacheIntoApp(app);
            }
            else
            {
                SetCacheStatus(app.scan, "Disabled", false);
            }
        }

        ImGui::Separator();
        if (ImGui::Button("Save Settings"))
        {
            app.settings.exportRoot = Utf8ToPath(app.exportRootBuffer.data());
            SetGameRoot(app, Utf8ToPath(app.gameRootBuffer.data()));
            SaveSettingsWithLog(app);
        }
        ImGui::SameLine();
        ImGui::Text("Settings file: %s", PathToUtf8(app.settingsPath).c_str());
        ImGui::Text("Index cache: %s", PathToUtf8(app.indexCachePath).c_str());

        ImGui::End();
    }

    void DrawAboutWindow(AppState& app)
    {
        if (!app.aboutWindowOpen)
        {
            return;
        }

        if (!ImGui::Begin("About Dagor Assets Extractor", &app.aboutWindowOpen))
        {
            ImGui::End();
            return;
        }

        ImGui::TextWrapped("Lightweight read-only browser and exporter foundation for local Dagor resource packs.");
        ImGui::Separator();
        ImGui::BulletText("Current build: app shell, settings, validation, resource discovery, logs.");
        ImGui::BulletText("Game files are only scanned and are not modified.");
        ImGui::BulletText("Texture and model parsing/export are future milestones.");
        ImGui::End();
    }

    void DrawFirstRunModal(AppState& app)
    {
        if (!app.firstRunPromptOpen)
        {
            return;
        }

        ImGui::OpenPopup("First Run Setup");
        if (ImGui::BeginPopupModal("First Run Setup", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextWrapped("Choose the local War Thunder installation folder to start building the read-only resource index.");
            ImGui::Spacing();
            ImGui::InputText("Game Root", app.gameRootBuffer.data(), app.gameRootBuffer.size());
            ImGui::SameLine();
            if (ImGui::Button("Browse"))
            {
                if (const auto selected = BrowseForFolder(app.hwnd, L"Choose War Thunder Folder", app.settings.gameRoot))
                {
                    SetGameRoot(app, *selected);
                }
            }

            DrawValidationSummary(app.validation);
            ImGui::Separator();

            ImGui::BeginDisabled(!app.validation.IsValid());
            if (ImGui::Button("Use Folder", ImVec2(120.0f, 0.0f)))
            {
                ApplyGameRootBuffer(app);
                if (app.validation.IsValid())
                {
                    SaveSettingsWithLog(app);
                    app.firstRunPromptOpen = false;
                    ImGui::CloseCurrentPopup();
                }
                else
                {
                    AddLog(app, LogLevel::Warning, "The selected folder did not pass validation.");
                }
            }
            ImGui::EndDisabled();

            ImGui::SameLine();
            if (ImGui::Button("Validate", ImVec2(100.0f, 0.0f)))
            {
                ApplyGameRootBuffer(app);
            }

            ImGui::SameLine();
            if (ImGui::Button("Skip For Now", ImVec2(120.0f, 0.0f)))
            {
                app.firstRunPromptOpen = false;
                AddLog(app, LogLevel::Warning, "First-run setup skipped. A game folder is still required before indexing.");
                ImGui::CloseCurrentPopup();
            }

            ImGui::EndPopup();
        }
    }

    void DrawMainShell(AppState& app, bool& running)
    {
        JoinFinishedScan(app);

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);

        ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoNavFocus |
            ImGuiWindowFlags_MenuBar;

        ImGui::Begin("DagorAssetsExtractorMain", nullptr, flags);
        DrawMenuBar(app, running);
        DrawHeader(app);

        const std::vector<ResourceRecord> records = SnapshotRecords(app.scan);
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const float spacing = ImGui::GetStyle().ItemSpacing.x;
        const float bottomHeight = std::min(190.0f, available.y * 0.32f);
        const float topHeight = std::max(200.0f, available.y - bottomHeight - spacing);
        const float leftWidth = 290.0f;
        const float rightWidth = 340.0f;
        const float centerWidth = std::max(300.0f, available.x - leftWidth - rightWidth - spacing * 2.0f);

        ImGui::BeginChild("LeftPanel", ImVec2(leftWidth, topHeight), true);
        DrawLeftPanel(app, records);
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("ContentBrowserPanel", ImVec2(centerWidth, topHeight), true);
        DrawContentBrowser(app, records);
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("InspectorPanel", ImVec2(0.0f, topHeight), true);
        DrawInspector(app, records);
        ImGui::EndChild();

        ImGui::BeginChild("BottomPanel", ImVec2(0.0f, 0.0f), true);
        DrawJobAndLogPanel(app);
        ImGui::EndChild();

        ImGui::End();

        DrawSettingsWindow(app);
        DrawAboutWindow(app);
        DrawFirstRunModal(app);
    }

    void CreateRenderTarget()
    {
        ID3D11Texture2D* backBuffer = nullptr;
        if (SUCCEEDED(GSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))))
        {
            GDevice->CreateRenderTargetView(backBuffer, nullptr, &GMainRenderTargetView);
            backBuffer->Release();
        }
    }

    void CleanupRenderTarget()
    {
        if (GMainRenderTargetView != nullptr)
        {
            GMainRenderTargetView->Release();
            GMainRenderTargetView = nullptr;
        }
    }

    bool CreateDeviceD3D(HWND hwnd)
    {
        DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
        swapChainDesc.BufferCount = 2;
        swapChainDesc.BufferDesc.Width = 0;
        swapChainDesc.BufferDesc.Height = 0;
        swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapChainDesc.BufferDesc.RefreshRate.Numerator = 60;
        swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
        swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapChainDesc.OutputWindow = hwnd;
        swapChainDesc.SampleDesc.Count = 1;
        swapChainDesc.SampleDesc.Quality = 0;
        swapChainDesc.Windowed = TRUE;
        swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        UINT createDeviceFlags = 0;
#if defined(_DEBUG)
        createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        constexpr D3D_FEATURE_LEVEL featureLevels[] =
        {
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_0
        };

        D3D_FEATURE_LEVEL featureLevel;
        const HRESULT result = D3D11CreateDeviceAndSwapChain(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            createDeviceFlags,
            featureLevels,
            static_cast<UINT>(std::size(featureLevels)),
            D3D11_SDK_VERSION,
            &swapChainDesc,
            &GSwapChain,
            &GDevice,
            &featureLevel,
            &GDeviceContext);

        if (FAILED(result))
        {
            return false;
        }

        CreateRenderTarget();
        return true;
    }

    void CleanupDeviceD3D()
    {
        CleanupRenderTarget();

        if (GSwapChain != nullptr)
        {
            GSwapChain->Release();
            GSwapChain = nullptr;
        }

        if (GDeviceContext != nullptr)
        {
            GDeviceContext->Release();
            GDeviceContext = nullptr;
        }

        if (GDevice != nullptr)
        {
            GDevice->Release();
            GDevice = nullptr;
        }
    }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
    {
        return TRUE;
    }

    switch (msg)
    {
    case WM_SIZE:
        if (GDevice != nullptr && wParam != SIZE_MINIMIZED)
        {
            CleanupRenderTarget();
            GSwapChain->ResizeBuffers(0, static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0U) == SC_KEYMENU)
        {
            return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool comInitialized = SUCCEEDED(comResult);

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = WndProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"DagorAssetsExtractorWindowClass";

    if (RegisterClassExW(&windowClass) == 0)
    {
        if (comInitialized)
        {
            CoUninitialize();
        }
        return 1;
    }

    HWND hwnd = CreateWindowW(
        windowClass.lpszClassName,
        L"Dagor Assets Extractor",
        WS_OVERLAPPEDWINDOW,
        100,
        100,
        1420,
        880,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (hwnd == nullptr)
    {
        UnregisterClassW(windowClass.lpszClassName, instance);
        if (comInitialized)
        {
            CoUninitialize();
        }
        return 1;
    }

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        DestroyWindow(hwnd);
        UnregisterClassW(windowClass.lpszClassName, instance);
        if (comInitialized)
        {
            CoUninitialize();
        }
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(GDevice, GDeviceContext);

    AppState app;
    app.hwnd = hwnd;
    app.settingsPath = GetSettingsPath();
    app.indexCachePath = GetIndexCachePath();
    LoadSettings(app.settings, app.settingsPath);
    app.validation = ValidateGameRoot(app.settings.gameRoot);
    SyncSettingsBuffers(app);
    app.firstRunPromptOpen = !app.validation.IsValid();

    AddLog(app, LogLevel::Info, "Application started.");
    AddLog(app, LogLevel::Info, "Settings path: " + PathToUtf8(app.settingsPath));
    AddLog(app, LogLevel::Info, "Index cache path: " + PathToUtf8(app.indexCachePath));
    if (app.validation.IsValid())
    {
        AddLog(app, LogLevel::Info, "Loaded saved game folder: " + PathToUtf8(app.settings.gameRoot));
        LoadIndexCacheIntoApp(app);
    }
    else
    {
        AddLog(app, LogLevel::Warning, "No valid War Thunder folder is configured.");
        SetCacheStatus(app.scan, "No valid game folder", false);
    }

    bool running = true;
    float clearColor[4] = { 0.08f, 0.09f, 0.11f, 1.0f };

    while (running)
    {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);

            if (msg.message == WM_QUIT)
            {
                running = false;
            }
        }

        if (!running)
        {
            break;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        DrawMainShell(app, running);

        ImGui::Render();
        GDeviceContext->OMSetRenderTargets(1, &GMainRenderTargetView, nullptr);
        GDeviceContext->ClearRenderTargetView(GMainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        GSwapChain->Present(1, 0);
    }

    StopScan(app);

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(windowClass.lpszClassName, instance);

    if (comInitialized)
    {
        CoUninitialize();
    }

    return 0;
}
