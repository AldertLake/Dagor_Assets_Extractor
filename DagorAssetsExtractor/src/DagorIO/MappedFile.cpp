#include "DagorIO/MappedFile.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fstream>
#endif

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace DagorIO
{
    namespace
    {
        std::string FormatLastErrorMessage(DWORD code)
        {
            return std::system_category().message(static_cast<int>(code));
        }

        std::string PathFilenameUtf8(const std::filesystem::path& path)
        {
#if defined(_WIN32)
            const std::wstring wide = path.filename().wstring();
            if (wide.empty())
            {
                return {};
            }
            const int len = WideCharToMultiByte(
                CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
            if (len <= 0)
            {
                return {};
            }
            std::string utf8(static_cast<size_t>(len), '\0');
            WideCharToMultiByte(
                CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), len, nullptr, nullptr);
            return utf8;
#else
            return path.filename().string();
#endif
        }
    }

    MappedFile::~MappedFile()
    {
        Close();
    }

    MappedFile::MappedFile(MappedFile&& other) noexcept
        : m_fileHandle(std::exchange(other.m_fileHandle, nullptr))
        , m_mappingHandle(std::exchange(other.m_mappingHandle, nullptr))
        , m_data(std::exchange(other.m_data, nullptr))
        , m_size(std::exchange(other.m_size, 0))
        , m_open(std::exchange(other.m_open, false))
    {
    }

    MappedFile& MappedFile::operator=(MappedFile&& other) noexcept
    {
        if (this != &other)
        {
            Close();
            m_fileHandle = std::exchange(other.m_fileHandle, nullptr);
            m_mappingHandle = std::exchange(other.m_mappingHandle, nullptr);
            m_data = std::exchange(other.m_data, nullptr);
            m_size = std::exchange(other.m_size, 0);
            m_open = std::exchange(other.m_open, false);
        }
        return *this;
    }

    bool MappedFile::Open(const std::filesystem::path& path, std::string& error)
    {
        Close();
        error.clear();

#if defined(_WIN32)
        HANDLE fileHandle = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS,
            nullptr);

        if (fileHandle == INVALID_HANDLE_VALUE)
        {
            const DWORD err = GetLastError();
            error = "Could not open file '" + PathFilenameUtf8(path) + "': " + FormatLastErrorMessage(err);
            return false;
        }

        LARGE_INTEGER fileSizeLi = {};
        if (!GetFileSizeEx(fileHandle, &fileSizeLi) || fileSizeLi.QuadPart < 0)
        {
            const DWORD err = GetLastError();
            CloseHandle(fileHandle);
            error = "Could not determine size of '" + PathFilenameUtf8(path) + "': " + FormatLastErrorMessage(err);
            return false;
        }

        const uint64_t fileSize64 = static_cast<uint64_t>(fileSizeLi.QuadPart);
        if (fileSize64 == 0)
        {
            CloseHandle(fileHandle);
            m_open = true;
            m_size = 0;
            m_data = nullptr;
            return true;
        }

        if (fileSize64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
        {
            CloseHandle(fileHandle);
            error = "File '" + PathFilenameUtf8(path) + "' exceeds addressable memory space.";
            return false;
        }

        HANDLE mappingHandle = CreateFileMappingW(
            fileHandle,
            nullptr,
            PAGE_READONLY,
            0,
            0,
            nullptr);

        if (mappingHandle == nullptr)
        {
            const DWORD err = GetLastError();
            CloseHandle(fileHandle);
            error = "Could not create file mapping for '" + PathFilenameUtf8(path) + "': " + FormatLastErrorMessage(err);
            return false;
        }

        const void* mappedView = MapViewOfFile(mappingHandle, FILE_MAP_READ, 0, 0, 0);
        if (mappedView == nullptr)
        {
            const DWORD err = GetLastError();
            CloseHandle(mappingHandle);
            CloseHandle(fileHandle);
            error = "Could not map view of '" + PathFilenameUtf8(path) + "': " + FormatLastErrorMessage(err);
            return false;
        }

        m_fileHandle = fileHandle;
        m_mappingHandle = mappingHandle;
        m_data = static_cast<const uint8_t*>(mappedView);
        m_size = static_cast<size_t>(fileSize64);
        m_open = true;
        return true;
#else
        error = "MappedFile is currently only implemented for Windows.";
        (void)path;
        return false;
#endif
    }

    void MappedFile::Close()
    {
#if defined(_WIN32)
        if (m_data != nullptr)
        {
            UnmapViewOfFile(m_data);
            m_data = nullptr;
        }
        if (m_mappingHandle != nullptr)
        {
            CloseHandle(static_cast<HANDLE>(m_mappingHandle));
            m_mappingHandle = nullptr;
        }
        if (m_fileHandle != nullptr)
        {
            CloseHandle(static_cast<HANDLE>(m_fileHandle));
            m_fileHandle = nullptr;
        }
#endif
        m_size = 0;
        m_open = false;
    }

    std::span<const uint8_t> MappedFile::Slice(size_t offset, size_t size) const
    {
        if (offset > m_size || size > m_size - offset)
        {
            throw std::out_of_range("MappedFile::Slice out of range");
        }
        if (size == 0)
        {
            return {};
        }
        return std::span<const uint8_t>(m_data + offset, size);
    }

    bool ReadFileRange(
        const std::filesystem::path& path,
        uint64_t offset,
        size_t size,
        std::vector<uint8_t>& outData,
        std::string& error)
    {
        error.clear();
        outData.clear();

#if defined(_WIN32)
        HANDLE fileHandle = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (fileHandle == INVALID_HANDLE_VALUE)
        {
            error = "Could not open '" + PathFilenameUtf8(path) + "': " + FormatLastErrorMessage(GetLastError());
            return false;
        }

        LARGE_INTEGER fileSizeLi = {};
        if (!GetFileSizeEx(fileHandle, &fileSizeLi) || fileSizeLi.QuadPart < 0)
        {
            error = "Could not determine size of '" + PathFilenameUtf8(path) + "': " + FormatLastErrorMessage(GetLastError());
            CloseHandle(fileHandle);
            return false;
        }

        const uint64_t fileSize64 = static_cast<uint64_t>(fileSizeLi.QuadPart);
        if (offset > fileSize64 || static_cast<uint64_t>(size) > fileSize64 - offset)
        {
            CloseHandle(fileHandle);
            error = "Requested read range is out of bounds for '" + PathFilenameUtf8(path) + "'.";
            return false;
        }

        if (size == 0)
        {
            CloseHandle(fileHandle);
            return true;
        }

        LARGE_INTEGER seekPos = {};
        seekPos.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(fileHandle, seekPos, nullptr, FILE_BEGIN))
        {
            error = "Could not seek in '" + PathFilenameUtf8(path) + "': " + FormatLastErrorMessage(GetLastError());
            CloseHandle(fileHandle);
            return false;
        }

        outData.resize(size);
        size_t totalRead = 0;
        while (totalRead < size)
        {
            const size_t remaining = size - totalRead;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 64u * 1024u * 1024u));
            DWORD bytesRead = 0;
            if (!ReadFile(fileHandle, outData.data() + totalRead, chunk, &bytesRead, nullptr) || bytesRead == 0)
            {
                error = "Failed reading '" + PathFilenameUtf8(path) + "': " + FormatLastErrorMessage(GetLastError());
                outData.clear();
                CloseHandle(fileHandle);
                return false;
            }
            totalRead += bytesRead;
        }

        CloseHandle(fileHandle);
        return true;
#else
        (void)path;
        (void)offset;
        (void)size;
        error = "ReadFileRange is currently only implemented for Windows.";
        return false;
#endif
    }

    bool ReadFilePrefix(
        const std::filesystem::path& path,
        size_t maxBytes,
        std::vector<uint8_t>& outData,
        uint64_t* outFileSize,
        std::string& error)
    {
        error.clear();
        outData.clear();
        if (outFileSize != nullptr)
        {
            *outFileSize = 0;
        }

#if defined(_WIN32)
        HANDLE fileHandle = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        if (fileHandle == INVALID_HANDLE_VALUE)
        {
            error = "Could not open '" + PathFilenameUtf8(path) + "': " + FormatLastErrorMessage(GetLastError());
            return false;
        }

        LARGE_INTEGER fileSizeLi = {};
        if (!GetFileSizeEx(fileHandle, &fileSizeLi) || fileSizeLi.QuadPart < 0)
        {
            error = "Could not determine size of '" + PathFilenameUtf8(path) + "': " + FormatLastErrorMessage(GetLastError());
            CloseHandle(fileHandle);
            return false;
        }

        const uint64_t fileSize64 = static_cast<uint64_t>(fileSizeLi.QuadPart);
        if (outFileSize != nullptr)
        {
            *outFileSize = fileSize64;
        }

        const size_t toRead = static_cast<size_t>(std::min<uint64_t>(fileSize64, static_cast<uint64_t>(maxBytes)));
        if (toRead == 0)
        {
            CloseHandle(fileHandle);
            return true;
        }

        outData.resize(toRead);
        size_t totalRead = 0;
        while (totalRead < toRead)
        {
            const size_t remaining = toRead - totalRead;
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 64u * 1024u * 1024u));
            DWORD bytesRead = 0;
            if (!ReadFile(fileHandle, outData.data() + totalRead, chunk, &bytesRead, nullptr) || bytesRead == 0)
            {
                error = "Failed reading prefix of '" + PathFilenameUtf8(path) + "': " + FormatLastErrorMessage(GetLastError());
                outData.clear();
                CloseHandle(fileHandle);
                return false;
            }
            totalRead += bytesRead;
        }

        CloseHandle(fileHandle);
        return true;
#else
        (void)path;
        (void)maxBytes;
        error = "ReadFilePrefix is currently only implemented for Windows.";
        return false;
#endif
    }
}
