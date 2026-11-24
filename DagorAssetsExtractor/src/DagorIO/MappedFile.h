#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace DagorIO
{
    /// Read-only memory-mapped file wrapper (RAII, move-only).
    /// Keeps Windows headers out of public headers via opaque handle storage.
    class MappedFile
    {
    public:
        MappedFile() = default;
        ~MappedFile();

        MappedFile(const MappedFile&) = delete;
        MappedFile& operator=(const MappedFile&) = delete;

        MappedFile(MappedFile&& other) noexcept;
        MappedFile& operator=(MappedFile&& other) noexcept;

        /// Opens and maps the file for read-only access.
        /// Empty (0-byte) files succeed with IsOpen() == true, Size() == 0, Data() == nullptr.
        bool Open(const std::filesystem::path& path, std::string& error);

        /// Unmaps the view and closes underlying OS handles.
        void Close();

        bool IsOpen() const { return m_open; }
        size_t Size() const { return m_size; }
        const uint8_t* Data() const { return m_data; }

        std::span<const uint8_t> Span() const
        {
            return (m_data != nullptr && m_size > 0)
                ? std::span<const uint8_t>(m_data, m_size)
                : std::span<const uint8_t>();
        }

        /// Returns a bounds-checked subspan, throwing std::out_of_range on overflow/OOB.
        std::span<const uint8_t> Slice(size_t offset, size_t size) const;

    private:
        void* m_fileHandle = nullptr;
        void* m_mappingHandle = nullptr;
        const uint8_t* m_data = nullptr;
        size_t m_size = 0;
        bool m_open = false;
    };

    /// Reads an exact byte range [offset, offset + size) from a file.
    bool ReadFileRange(
        const std::filesystem::path& path,
        uint64_t offset,
        size_t size,
        std::vector<uint8_t>& outData,
        std::string& error);

    /// Reads up to maxBytes from the start of a file and optionally outputs total file size.
    bool ReadFilePrefix(
        const std::filesystem::path& path,
        size_t maxBytes,
        std::vector<uint8_t>& outData,
        uint64_t* outFileSize,
        std::string& error);
}
