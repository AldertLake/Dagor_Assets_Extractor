#include "AssetIndex/PackContents.h"
#include "DagorFormats/VromfsReader.h"
#include "DagorFormats/DxpReader.h"
#include "DagorFormats/DdsxReader.h"

#include <fstream>
#include <algorithm>

namespace DagorAssetsExtractor::AssetIndex
{
    namespace fs = std::filesystem;

    // -- PackContentsCache ----------------------------------------------------

    std::shared_ptr<const PackContents> PackContentsCache::Get(const fs::path& packPath) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::wstring key = packPath.lexically_normal().wstring();
        const auto it = m_cache.find(key);
        if (it != m_cache.end())
        {
            return it->second;
        }
        return nullptr;
    }

    void PackContentsCache::Put(const fs::path& packPath, std::shared_ptr<PackContents> contents)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::wstring key = packPath.lexically_normal().wstring();
        m_cache[key] = std::move(contents);
    }

    bool PackContentsCache::Has(const fs::path& packPath) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::wstring key = packPath.lexically_normal().wstring();
        return m_cache.find(key) != m_cache.end();
    }

    void PackContentsCache::Remove(const fs::path& packPath)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const std::wstring key = packPath.lexically_normal().wstring();
        m_cache.erase(key);
    }

    void PackContentsCache::Clear()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_cache.clear();
    }

    // -- Helper: read a file into memory with size limit ----------------------

    namespace
    {
        bool ReadFileLimited(const fs::path& path, std::vector<uint8_t>& outData, size_t maxSize, std::string& error)
        {
            std::error_code ec;
            const uintmax_t fileSize = fs::file_size(path, ec);
            if (ec)
            {
                error = "Could not determine file size: " + ec.message();
                return false;
            }

            if (fileSize > maxSize)
            {
                error = "File too large for in-memory parsing (" + std::to_string(fileSize / (1024 * 1024)) + " MB).";
                return false;
            }

            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                error = "Could not open file for reading.";
                return false;
            }

            outData.resize(static_cast<size_t>(fileSize));
            file.read(reinterpret_cast<char*>(outData.data()), static_cast<std::streamsize>(fileSize));
            if (!file)
            {
                error = "Failed to read file contents.";
                return false;
            }

            return true;
        }
    }

    // -- Parse VROMFS ---------------------------------------------------------

    std::shared_ptr<PackContents> ParseVromfsContents(const fs::path& path, size_t maxFileSize)
    {
        auto contents = std::make_shared<PackContents>();
        contents->sourcePath = path;
        contents->containerType = "VROMFS";
        contents->parsed = true;

        std::vector<uint8_t> data;
        if (!ReadFileLimited(path, data, maxFileSize, contents->error))
        {
            return contents;
        }

        try
        {
            DagorFormats::VromfsReader reader(data);
            reader.Parse();

            for (const DagorFormats::VromfsFileInfo& file : reader.GetFiles())
            {
                SubAssetRecord entry;
                entry.name   = file.name;
                entry.offset = file.offset;
                entry.size   = file.size;
                contents->entries.push_back(std::move(entry));
            }
        }
        catch (const std::exception& ex)
        {
            contents->error = std::string("VROMFS parse error: ") + ex.what();
        }

        return contents;
    }

    // -- Parse DXP ------------------------------------------------------------

    std::shared_ptr<PackContents> ParseDxpContents(const fs::path& path, size_t maxFileSize)
    {
        auto contents = std::make_shared<PackContents>();
        contents->sourcePath = path;
        contents->containerType = "DXP";
        contents->parsed = true;

        std::vector<uint8_t> data;
        if (!ReadFileLimited(path, data, maxFileSize, contents->error))
        {
            return contents;
        }

        try
        {
            DagorFormats::DxpReader reader(data);
            reader.Parse();

            for (const DagorFormats::DxpFileInfo& file : reader.GetFiles())
            {
                SubAssetRecord entry;
                entry.name   = file.name;
                entry.offset = file.offset;
                entry.size   = file.size;

                // Try to parse the DDSx header for extra texture metadata
                if (file.headerDdsx.size() >= 32)
                {
                    try
                    {
                        DagorFormats::DdsxReader ddsxReader(file.headerDdsx);
                        ddsxReader.Parse();
                        const DagorFormats::DdsxHeader& hdr = ddsxReader.GetHeader();

                        entry.texWidth       = hdr.width;
                        entry.texHeight      = hdr.height;
                        entry.texMips        = hdr.mipCount;
                        entry.texFormat      = hdr.FormatName();
                        entry.texCompression = hdr.CompressionName();
                        entry.formatInfo     = std::string(hdr.FormatName()) + " " +
                                               std::to_string(hdr.width) + "x" + std::to_string(hdr.height);
                    }
                    catch (const std::exception&)
                    {
                        entry.formatInfo = "DDSx header unreadable";
                    }
                }

                contents->entries.push_back(std::move(entry));
            }
        }
        catch (const std::exception& ex)
        {
            contents->error = std::string("DXP parse error: ") + ex.what();
        }

        return contents;
    }

} // namespace DagorAssetsExtractor::AssetIndex
