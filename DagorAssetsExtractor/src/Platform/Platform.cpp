#include "Platform.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <iterator>

namespace DagorAssetsExtractor::Platform {

    namespace fs = std::filesystem;

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

    std::string PathToUtf8(const fs::path& path)
    {
        return WideToUtf8(path.wstring());
    }

    fs::path Utf8ToPath(std::string_view value)
    {
        return fs::path(Utf8ToWide(value));
    }

#if defined(_WIN32)
    static std::optional<fs::path> GetKnownFolderPath(REFKNOWNFOLDERID folderId)
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
#endif

    std::optional<fs::path> GetKnownFolderPathRoamingAppData()
    {
#if defined(_WIN32)
        return GetKnownFolderPath(FOLDERID_RoamingAppData);
#else
        return std::nullopt;
#endif
    }

    std::optional<fs::path> GetKnownFolderPathDocuments()
    {
#if defined(_WIN32)
        return GetKnownFolderPath(FOLDERID_Documents);
#else
        return std::nullopt;
#endif
    }

    fs::path GetExecutableDirectory()
    {
#if defined(_WIN32)
        std::wstring buffer(32768, L'\0');
        const DWORD length = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length > 0 && length < buffer.size())
        {
            buffer.resize(length);
            const fs::path executablePath(buffer);
            if (executablePath.has_parent_path())
            {
                return executablePath.parent_path();
            }
        }
#endif

        return fs::current_path();
    }

    fs::path GetUiAssetPath()
    {
        return GetExecutableDirectory() / L"assets" / L"ui";
    }

    fs::path GetCacheRootPath()
    {
        return GetExecutableDirectory() / L"cache";
    }

    fs::path GetSettingsPath()
    {
        return GetCacheRootPath() / L"Settings.ini";
    }

    fs::path GetIndexCachePath()
    {
        return GetCacheRootPath() / L"index-cache";
    }

    fs::path GetDescriptorLinksCachePath()
    {
        return GetCacheRootPath() / L"descriptor-links-cache";
    }

    std::string CanonicalizePathUtf8(const fs::path& path)
    {
        if (path.empty())
        {
            return {};
        }

        std::wstring wide = path.lexically_normal().wstring();
        for (wchar_t& ch : wide)
        {
            if (ch == L'\\')
            {
                ch = L'/';
            }
            else
            {
                ch = static_cast<wchar_t>(std::towlower(ch));
            }
        }

        while (wide.size() > 1 && wide.back() == L'/')
        {
            // Preserve drive root such as "c:/"
            if (wide.size() == 3 && wide[1] == L':')
            {
                break;
            }
            wide.pop_back();
        }

        return WideToUtf8(wide);
    }

    uint64_t ComputeFnv1a64(std::string_view text)
    {
        constexpr uint64_t kFnvOffsetBasis = 14695981039346656037ull;
        constexpr uint64_t kFnvPrime = 1099511628211ull;
        uint64_t hash = kFnvOffsetBasis;
        for (const unsigned char ch : text)
        {
            hash ^= static_cast<uint64_t>(ch);
            hash *= kFnvPrime;
        }
        return hash;
    }

    std::string FormatHex64(uint64_t value)
    {
        static constexpr char kHexDigits[] = "0123456789abcdef";
        std::string result(16, '0');
        for (int i = 15; i >= 0; --i)
        {
            result[static_cast<size_t>(i)] = kHexDigits[value & 0xFu];
            value >>= 4;
        }
        return result;
    }

    fs::path GetRootCacheDirectory(const fs::path& gameRoot)
    {
        if (gameRoot.empty())
        {
            return GetCacheRootPath();
        }

        const std::string canonical = CanonicalizePathUtf8(gameRoot);
        if (canonical.empty())
        {
            return GetCacheRootPath();
        }

        return GetCacheRootPath() / Utf8ToPath(FormatHex64(ComputeFnv1a64(canonical)));
    }

    fs::path GetIndexCachePathForRoot(const fs::path& gameRoot)
    {
        if (gameRoot.empty())
        {
            return GetCacheRootPath() / L"index.bin";
        }
        return GetRootCacheDirectory(gameRoot) / L"index.bin";
    }

    fs::path GetDescriptorLinksCachePathForRoot(const fs::path& gameRoot)
    {
        if (gameRoot.empty())
        {
            return GetCacheRootPath() / L"links.bin";
        }
        return GetRootCacheDirectory(gameRoot) / L"links.bin";
    }

    fs::path GetDefaultExportRoot()
    {
        if (const auto documents = GetKnownFolderPathDocuments())
        {
            return *documents / L"DagorAssetsExtractorExports";
        }

        return fs::current_path() / L"exports";
    }

    bool ShowPathInExplorer(const fs::path& path)
    {
#if defined(_WIN32)
        std::error_code ec;
        fs::path target = path;
        if (target.empty())
        {
            return false;
        }

        if (fs::is_regular_file(target, ec))
        {
            const std::wstring arguments = L"/select,\"" + target.wstring() + L"\"";
            const HINSTANCE result = ShellExecuteW(
                nullptr,
                L"open",
                L"explorer.exe",
                arguments.c_str(),
                nullptr,
                SW_SHOWNORMAL);
            return reinterpret_cast<std::intptr_t>(result) > 32;
        }

        if (!fs::exists(target, ec) && target.has_parent_path())
        {
            target = target.parent_path();
        }

        const HINSTANCE result = ShellExecuteW(
            nullptr,
            L"open",
            target.wstring().c_str(),
            nullptr,
            nullptr,
            SW_SHOWNORMAL);
        return reinterpret_cast<std::intptr_t>(result) > 32;
#else
        (void)path;
        return false;
#endif
    }

    bool OpenUrl(std::wstring_view url)
    {
#if defined(_WIN32)
        if (url.empty())
        {
            return false;
        }

        const std::wstring wideUrl(url);
        const HINSTANCE result = ShellExecuteW(
            nullptr,
            L"open",
            wideUrl.c_str(),
            nullptr,
            nullptr,
            SW_SHOWNORMAL);
        return reinterpret_cast<std::intptr_t>(result) > 32;
#else
        (void)url;
        return false;
#endif
    }

    std::optional<fs::path> BrowseForFolder(HWND owner, const wchar_t* title, const fs::path& initialPath)
    {
#if defined(_WIN32)
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
#else
        return std::nullopt;
#endif
    }

    std::optional<fs::path> BrowseForFile(HWND owner, const wchar_t* title, const fs::path& initialPath)
    {
#if defined(_WIN32)
        IFileOpenDialog* dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        {
            return std::nullopt;
        }

        DWORD options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options)))
        {
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
        }

        COMDLG_FILTERSPEC filters[] = {
            { L"Oodle DLL", L"oo2core_6_win64.dll" },
            { L"DLL files", L"*.dll" },
            { L"All files", L"*.*" },
        };
        dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
        dialog->SetTitle(title);

        std::error_code ec;
        fs::path initialFolder = initialPath;
        if (!initialFolder.empty() && fs::is_regular_file(initialFolder, ec))
        {
            initialFolder = initialFolder.parent_path();
        }

        if (!initialFolder.empty() && fs::exists(initialFolder, ec))
        {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(initialFolder.wstring().c_str(), nullptr, IID_PPV_ARGS(&item))))
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
#else
        return std::nullopt;
#endif
    }

}
