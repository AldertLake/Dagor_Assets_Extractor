#pragma once

#include <string>
#include <string_view>
#include <filesystem>
#include <optional>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <shellapi.h>
#include <shlobj.h>
#else
typedef void* HWND;
#endif

namespace DagorAssetsExtractor::Platform {

    std::wstring Utf8ToWide(std::string_view utf8);
    std::string WideToUtf8(std::wstring_view wide);

    std::string PathToUtf8(const std::filesystem::path& path);
    std::filesystem::path Utf8ToPath(std::string_view value);

    std::optional<std::filesystem::path> GetKnownFolderPathRoamingAppData();
    std::optional<std::filesystem::path> GetKnownFolderPathDocuments();

    std::filesystem::path GetExecutableDirectory();
    std::filesystem::path GetCacheRootPath();
    std::filesystem::path GetSettingsPath();
    std::filesystem::path GetIndexCachePath();
    std::filesystem::path GetDescriptorLinksCachePath();
    std::filesystem::path GetDefaultExportRoot();
    bool ShowPathInExplorer(const std::filesystem::path& path);

    std::optional<std::filesystem::path> BrowseForFolder(HWND owner, const wchar_t* title, const std::filesystem::path& initialPath);
    std::optional<std::filesystem::path> BrowseForFile(HWND owner, const wchar_t* title, const std::filesystem::path& initialPath);

}
