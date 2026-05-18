#pragma once

#include "AssetIndex/AssetIndex.h"
#include "AssetIndex/PackContents.h"
#include "DagorFormats/DdsxReader.h"
#include <filesystem>
#include <string>
#include <vector>
#include <functional>

namespace DagorAssetsExtractor::Export
{
    struct TextureExportOptions
    {
        std::filesystem::path outputDir;
        std::filesystem::path gameRoot;
        std::filesystem::path oodleDllPath;
        std::string format = "DDS"; // "DDS", "PNG", etc.
        bool overwrite = true;
        bool useOodleFromGameFolder = true;
    };

    struct TextureExportResult
    {
        bool success = false;
        std::string error;
        std::filesystem::path outputPath;
        std::vector<std::string> warnings;
    };

    struct BatchTextureExportResult
    {
        size_t total = 0;
        size_t succeeded = 0;
        size_t failed = 0;
        size_t skipped = 0;
        std::vector<std::string> errors;
        std::vector<std::string> warnings;
    };

    // Progress callback for batch exports: (current, total, textureName) -> should_continue
    using BatchProgressCallback = std::function<bool(size_t current, size_t total, const std::string& name)>;

    class TextureExporter
    {
    public:
        static TextureExportResult ExportTexture(
            const std::filesystem::path& packPath,
            const AssetIndex::SubAssetRecord& entry,
            const TextureExportOptions& options);

        static BatchTextureExportResult ExportAllTextures(
            const std::filesystem::path& packPath,
            const AssetIndex::PackContents& contents,
            const TextureExportOptions& options,
            BatchProgressCallback progress = nullptr);

        // Unit test helpers to expose private methods
        static bool TestNeedsDX10Header(uint32_t d3dFormat) { return NeedsDX10Header(d3dFormat); }
        static uint32_t TestGetDxgiFormat(uint32_t d3dFormat) { return GetDxgiFormat(d3dFormat); }

    private:
        // DX10 extended header for DXGI formats that lack legacy FourCC mapping
        struct DdsHeaderDX10
        {
            uint32_t dxgiFormat = 0;
            uint32_t resourceDimension = 3; // D3D10_RESOURCE_DIMENSION_TEXTURE2D
            uint32_t miscFlag = 0;
            uint32_t arraySize = 1;
            uint32_t miscFlags2 = 0;
        };

        static bool WriteDds(
            const std::filesystem::path& outputPath,
            const DagorFormats::DdsxHeader& header,
            const std::vector<uint8_t>& payload,
            std::string& error);

        // Returns true if the format requires a DX10 extended header
        static bool NeedsDX10Header(uint32_t d3dFormat);

        // Returns the DXGI format enum value for the DX10 header
        static uint32_t GetDxgiFormat(uint32_t d3dFormat);
    };
}
