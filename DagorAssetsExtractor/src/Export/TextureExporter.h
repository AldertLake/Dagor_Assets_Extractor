#pragma once

#include "AssetIndex/AssetIndex.h"
#include "AssetIndex/PackContents.h"
#include "DagorFormats/DdsxReader.h"
#include "Export/MaterialTextureResolver.h"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace DagorAssetsExtractor::Export
{
    struct TextureExportOptions
    {
        std::filesystem::path outputDir;
        std::filesystem::path gameRoot;
        std::filesystem::path oodleDllPath;
        std::string format = "DDS"; // "DDS", "PNG", "TGA"
        bool preserveOriginalDDS = true;
        bool overwrite = true;
        bool useOodleFromGameFolder = true;
        bool exportJsonManifest = false;
        bool generateMaterialMaps = false;
        bool convertWarThunderPackedMaps = true;
        bool encodePngInMemoryOnly = false;
    };

    struct TextureMapOutput
    {
        TextureMapKind kind = TextureMapKind::BaseColor;
        std::filesystem::path outputPath;
        std::vector<uint8_t> encodedBytes;
    };

    struct TextureExportResult
    {
        bool success = false;
        std::string error;
        std::filesystem::path outputPath;
        std::vector<uint8_t> encodedBytes;
        std::vector<std::string> warnings;
        std::vector<TextureMapOutput> mapOutputs;
        bool nativeWarThunderPackedMap = false;
    };

    struct TextureMapExportOptions
    {
        std::filesystem::path outputDir;
        std::filesystem::path gameRoot;
        std::filesystem::path oodleDllPath;
        std::string format = "PNG";
        bool overwrite = true;
        bool useOodleFromGameFolder = true;
    };

    struct TexturePreviewOptions
    {
        std::filesystem::path gameRoot;
        std::filesystem::path oodleDllPath;
        bool useOodleFromGameFolder = true;
    };

    struct TexturePreviewResult
    {
        bool success = false;
        std::string error;
        std::vector<std::string> warnings;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t stride = 0;
        std::vector<uint8_t> bgraPixels;
        bool srgb = false;
    };

    struct TextureMapPreviewImage
    {
        TextureMapKind kind = TextureMapKind::BaseColor;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t stride = 0;
        std::vector<uint8_t> bgraPixels;
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

        static TextureExportResult ExportTextureFromMemory(
            const std::filesystem::path& packPath,
            std::span<const uint8_t> packData,
            const AssetIndex::SubAssetRecord& entry,
            const TextureExportOptions& options);

        static TexturePreviewResult DecodePreview(
            const std::filesystem::path& packPath,
            const AssetIndex::SubAssetRecord& entry,
            const TexturePreviewOptions& options);

        static TexturePreviewResult DecodePreviewFromMemory(
            const std::filesystem::path& packPath,
            std::span<const uint8_t> packData,
            const AssetIndex::SubAssetRecord& entry,
            const TexturePreviewOptions& options);

        static bool EncodeBgraToPngMemory(
            const uint8_t* pixels,
            uint32_t width,
            uint32_t height,
            uint32_t stride,
            std::vector<uint8_t>& outPngBytes,
            std::string& error,
            bool isGrayscale = false);

        static std::vector<TextureMapPreviewImage> BuildMapPreviews(
            std::string_view textureName,
            const uint8_t* bgraPixels,
            uint32_t width,
            uint32_t height,
            uint32_t stride,
            uint32_t maxDimension = 192);

        static TextureExportResult ExportTextureMap(
            const std::filesystem::path& packPath,
            const AssetIndex::SubAssetRecord& entry,
            TextureMapKind mapKind,
            const TextureMapExportOptions& options);

        static std::vector<TextureExportResult> ExportTextureMaps(
            const std::filesystem::path& packPath,
            const AssetIndex::SubAssetRecord& entry,
            const std::vector<TextureMapKind>& mapKinds,
            const TextureMapExportOptions& options);

        static bool ComposeOrmTexture(
            const std::filesystem::path& metallicRoughnessPath,
            const std::filesystem::path& ambientOcclusionPath,
            const std::filesystem::path& outputPath,
            std::string& error,
            std::vector<std::string>* warnings = nullptr);

        static bool ComposeOrmTextureFromMemory(
            std::span<const uint8_t> metallicRoughnessPng,
            std::span<const uint8_t> ambientOcclusionPng,
            std::vector<uint8_t>& outOrmPngBytes,
            std::string& error,
            std::vector<std::string>* warnings = nullptr);

        static BatchTextureExportResult ExportAllTextures(
            const std::filesystem::path& packPath,
            const AssetIndex::PackContents& contents,
            const TextureExportOptions& options,
            BatchProgressCallback progress = nullptr);

        // Unit test helpers to expose private methods
        static bool TestNeedsDX10Header(uint32_t d3dFormat) { return NeedsDX10Header(d3dFormat); }
        static uint32_t TestGetDxgiFormat(uint32_t d3dFormat) { return GetDxgiFormat(d3dFormat); }
        static bool TestNormalizeMipOrder(
            const DagorFormats::DdsxHeader& header,
            std::vector<uint8_t>& payload,
            std::string& warning,
            std::string& error);

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

        static bool BuildDdsBuffer(
            const DagorFormats::DdsxHeader& header,
            const std::vector<uint8_t>& payload,
            std::vector<uint8_t>& ddsData,
            std::string& error);

        // Returns true if the format requires a DX10 extended header
        static bool NeedsDX10Header(uint32_t d3dFormat);

        // Returns the DXGI format enum value for the DX10 header
        static uint32_t GetDxgiFormat(uint32_t d3dFormat);

        static TextureExportResult ExportTextureInternal(
            const std::filesystem::path& packPath,
            std::span<const uint8_t> mappedPackData,
            const AssetIndex::SubAssetRecord& entry,
            const TextureExportOptions& options);

        static TexturePreviewResult DecodePreviewInternal(
            const std::filesystem::path& packPath,
            std::span<const uint8_t> mappedPackData,
            const AssetIndex::SubAssetRecord& entry,
            const TexturePreviewOptions& options);
    };
}
