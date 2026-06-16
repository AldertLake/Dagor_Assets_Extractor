#include "App/AppState.h"
#include "AssetIndex/PackContents.h"
#include "AssetIndex/DependencyGraph.h"
#include "AssetIndex/TextureQuality.h"
#include "DirectXTex.h"
#include "Export/MaterialTextureResolver.h"
#include "Export/ModelExporter.h"
#include "Export/TextureExporter.h"
#include "DagorFormats/VromfsReader.h"
#include "Jobs/JobSystem.h"

#include <objbase.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    using DagorAssetsExtractor::AssetIndex::SubAssetRecord;
    using DagorAssetsExtractor::Export::ExportManifest;
    using DagorAssetsExtractor::Export::AnimationClip;
    using DagorAssetsExtractor::Export::AnimationNodeTrack;
    using DagorAssetsExtractor::Export::GlbExportOptions;
    using DagorAssetsExtractor::Export::MaterialInfo;
    using DagorAssetsExtractor::Export::MaterialTextureSemantic;
    using DagorAssetsExtractor::Export::ModelExporter;
    using DagorAssetsExtractor::Export::ResolvedMaterialTextures;
    using DagorAssetsExtractor::Export::TextureExportOptions;
    using DagorAssetsExtractor::Export::TextureExporter;
    using DagorAssetsExtractor::Export::TextureExportResult;
    using DagorAssetsExtractor::Export::TextureMapExportOptions;
    using DagorAssetsExtractor::Export::TextureMapKind;
    using DagorAssetsExtractor::Export::TexturePreviewOptions;
    using DagorAssetsExtractor::Export::TexturePreviewResult;

    [[noreturn]] void Fail(const std::string& message)
    {
        throw std::runtime_error(message);
    }

    void Expect(bool condition, const std::string& message)
    {
        if (!condition)
        {
            Fail(message);
        }
    }

    template <typename T>
    void WriteLe(std::vector<uint8_t>& data, size_t offset, T value)
    {
        for (size_t i = 0; i < sizeof(T); ++i)
        {
            data[offset + i] = static_cast<uint8_t>((static_cast<uint64_t>(value) >> (i * 8)) & 0xFFu);
        }
    }

    std::vector<uint8_t> BuildDdsxHeader(
        uint32_t d3dFormat,
        uint16_t width,
        uint16_t height,
        uint8_t mipCount,
        uint16_t bitsPerPixel,
        uint32_t flags,
        uint32_t memorySize,
        uint32_t packedSize)
    {
        std::vector<uint8_t> header(32, 0);
        WriteLe<uint32_t>(header, 0x00, 0x78534444u);
        WriteLe<uint32_t>(header, 0x04, d3dFormat);
        WriteLe<uint32_t>(header, 0x08, flags);
        WriteLe<uint16_t>(header, 0x0C, width);
        WriteLe<uint16_t>(header, 0x0E, height);
        header[0x10] = mipCount;
        header[0x11] = 0;
        WriteLe<uint16_t>(header, 0x12, 1u);
        WriteLe<uint16_t>(header, 0x14, bitsPerPixel);
        WriteLe<uint16_t>(header, 0x16, 0u);
        WriteLe<uint32_t>(header, 0x18, memorySize);
        WriteLe<uint32_t>(header, 0x1C, packedSize);
        return header;
    }

    std::filesystem::path MakeTempRoot()
    {
        const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        std::filesystem::path root = std::filesystem::temp_directory_path() /
            ("dagor_validation_" + std::to_string(stamp));
        std::filesystem::create_directories(root);
        return root;
    }

    std::vector<uint8_t> ReadFileBytes(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            Fail("Could not open file for reading: " + path.string());
        }

        file.seekg(0, std::ios::end);
        const std::streamoff size = file.tellg();
        file.seekg(0, std::ios::beg);
        if (size < 0)
        {
            Fail("Could not determine file size: " + path.string());
        }

        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (!bytes.empty() &&
            !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
        {
            Fail("Could not read file bytes: " + path.string());
        }

        return bytes;
    }

    void WriteFileBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
    {
        std::ofstream file(path, std::ios::binary);
        if (!file)
        {
            Fail("Could not open file for writing: " + path.string());
        }

        if (!bytes.empty())
        {
            file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }

        if (!file)
        {
            Fail("Could not write file bytes: " + path.string());
        }
    }

    DirectX::ScratchImage MakeRgbaImage()
    {
        DirectX::ScratchImage image;
        const HRESULT hr = image.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 1);
        if (FAILED(hr))
        {
            Fail("Initialize2D RGBA image failed.");
        }

        const DirectX::Image* base = image.GetImage(0, 0, 0);
        Expect(base != nullptr, "RGBA scratch image returned no base image.");

        for (size_t y = 0; y < 4; ++y)
        {
            for (size_t x = 0; x < 4; ++x)
            {
                uint8_t* pixel = base->pixels + y * base->rowPitch + x * 4;
                pixel[0] = static_cast<uint8_t>(x * 40);
                pixel[1] = static_cast<uint8_t>(y * 60);
                pixel[2] = static_cast<uint8_t>(255 - x * 30);
                pixel[3] = 255;
            }
        }

        return image;
    }

    DirectX::ScratchImage MakeHdrImage()
    {
        DirectX::ScratchImage image;
        const HRESULT hr = image.Initialize2D(DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 4, 1, 1);
        if (FAILED(hr))
        {
            Fail("Initialize2D HDR image failed.");
        }

        const DirectX::Image* base = image.GetImage(0, 0, 0);
        Expect(base != nullptr, "HDR scratch image returned no base image.");

        for (size_t y = 0; y < 4; ++y)
        {
            for (size_t x = 0; x < 4; ++x)
            {
                float* pixel = reinterpret_cast<float*>(base->pixels + y * base->rowPitch + x * sizeof(float) * 4);
                pixel[0] = 0.25f + static_cast<float>(x) * 0.5f;
                pixel[1] = 0.5f + static_cast<float>(y) * 0.25f;
                pixel[2] = 1.0f + static_cast<float>(x + y) * 0.125f;
                pixel[3] = 1.0f;
            }
        }

        return image;
    }

    std::vector<uint8_t> CompressSingleBlock(const DirectX::Image& source, DXGI_FORMAT format)
    {
        DirectX::ScratchImage compressed;
        const HRESULT hr = DirectX::Compress(
            source,
            format,
            DirectX::TEX_COMPRESS_DEFAULT,
            DirectX::TEX_THRESHOLD_DEFAULT,
            compressed);
        if (FAILED(hr))
        {
            Fail("DirectXTex compression failed.");
        }

        const DirectX::Image* image = compressed.GetImage(0, 0, 0);
        Expect(image != nullptr, "Compressed image is missing.");
        Expect(image->pixels != nullptr && image->slicePitch > 0, "Compressed image payload is empty.");
        return std::vector<uint8_t>(image->pixels, image->pixels + image->slicePitch);
    }

    bool WarningContains(const TextureExportResult& result, const std::string& needle)
    {
        return std::any_of(result.warnings.begin(), result.warnings.end(), [&](const std::string& warning) {
            return warning.find(needle) != std::string::npos;
        });
    }

    std::filesystem::path FindMapOutput(
        const TextureExportResult& result,
        TextureMapKind kind)
    {
        const auto outputIt = std::find_if(
            result.mapOutputs.begin(),
            result.mapOutputs.end(),
            [&](const DagorAssetsExtractor::Export::TextureMapOutput& output) {
                return output.kind == kind;
            });
        return outputIt == result.mapOutputs.end()
            ? std::filesystem::path{}
            : outputIt->outputPath;
    }

    std::array<uint8_t, 4> ReadFirstPngPixelRgba(const std::filesystem::path& path)
    {
        std::array<uint8_t, 4> pixel = {};
        {
            DirectX::TexMetadata metadata = {};
            DirectX::ScratchImage loaded;
            HRESULT hr = DirectX::LoadFromWICFile(
                path.c_str(),
                DirectX::WIC_FLAGS_IGNORE_SRGB,
                &metadata,
                loaded);
            if (FAILED(hr))
            {
                Fail("Could not load validation PNG: " + path.string());
            }

            const DirectX::Image* image = loaded.GetImage(0, 0, 0);
            Expect(image != nullptr, "Validation PNG has no top image.");

            DirectX::ScratchImage rgba;
            hr = DirectX::Convert(
                *image,
                DXGI_FORMAT_R8G8B8A8_UNORM,
                DirectX::TEX_FILTER_DEFAULT,
                DirectX::TEX_THRESHOLD_DEFAULT,
                rgba);
            if (FAILED(hr))
            {
                Fail("Could not convert validation PNG to RGBA8.");
            }

            const DirectX::Image* rgbaImage = rgba.GetImage(0, 0, 0);
            Expect(rgbaImage != nullptr && rgbaImage->pixels != nullptr, "Converted validation PNG is empty.");
            pixel = {
                rgbaImage->pixels[0],
                rgbaImage->pixels[1],
                rgbaImage->pixels[2],
                rgbaImage->pixels[3],
            };
        }
        return pixel;
    }

    void ValidateReadableExport(
        const std::filesystem::path& tempRoot,
        const std::string& assetName,
        uint32_t d3dFormat,
        uint16_t bitsPerPixel,
        const std::vector<uint8_t>& payload)
    {
        const std::filesystem::path packPath = tempRoot / (assetName + ".pack");
        WriteFileBytes(packPath, payload);

        SubAssetRecord entry;
        entry.name = assetName;
        entry.offset = 0;
        entry.size = static_cast<uint32_t>(payload.size());
        entry.ddsxHeader = BuildDdsxHeader(
            d3dFormat,
            4,
            4,
            1,
            bitsPerPixel,
            0u,
            static_cast<uint32_t>(payload.size()),
            0u);

        TextureExportOptions options;
        options.outputDir = tempRoot / "exports";
        options.format = "PNG";
        options.preserveOriginalDDS = false;
        options.overwrite = true;
        options.exportJsonManifest = false;

        const TextureExportResult result = TextureExporter::ExportTexture(packPath, entry, options);
        Expect(result.success, "Texture export failed for " + assetName + ": " + result.error);
        Expect(result.outputPath.extension() == ".png", "Expected PNG output for " + assetName + ".");
        Expect(std::filesystem::exists(result.outputPath), "Texture export did not create the PNG file.");
        Expect(WarningContains(result, "DirectXTex"), "Expected DirectXTex fallback warning for " + assetName + ".");

        const std::vector<uint8_t> bytes = ReadFileBytes(result.outputPath);
        constexpr std::array<uint8_t, 8> kPngSignature = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
        Expect(bytes.size() >= kPngSignature.size(), "PNG output is too small for " + assetName + ".");
        Expect(std::equal(kPngSignature.begin(), kPngSignature.end(), bytes.begin()),
            "Texture export did not produce a PNG signature for " + assetName + ".");
    }

    TexturePreviewResult DecodePreviewFixture(
        const std::filesystem::path& tempRoot,
        const std::string& assetName,
        uint32_t d3dFormat,
        uint16_t width,
        uint16_t height,
        uint16_t bitsPerPixel,
        const std::vector<uint8_t>& payload)
    {
        const std::filesystem::path packPath = tempRoot / (assetName + ".preview.pack");
        WriteFileBytes(packPath, payload);

        SubAssetRecord entry;
        entry.name = assetName;
        entry.offset = 0;
        entry.size = static_cast<uint32_t>(payload.size());
        entry.ddsxHeader = BuildDdsxHeader(
            d3dFormat,
            width,
            height,
            1,
            bitsPerPixel,
            0u,
            static_cast<uint32_t>(payload.size()),
            0u);

        TexturePreviewOptions options;
        options.useOodleFromGameFolder = false;
        return TextureExporter::DecodePreview(packPath, entry, options);
    }

    void RunTexturePreviewValidation(const std::filesystem::path& tempRoot)
    {
        const std::vector<uint8_t> rgbaPayload = {
            10, 20, 30, 40,
            50, 60, 70, 80,
        };
        const TexturePreviewResult rgba = DecodePreviewFixture(
            tempRoot,
            "rgba8_preview",
            28u,
            2,
            1,
            32u,
            rgbaPayload);
        Expect(rgba.success, "RGBA8 preview decode failed: " + rgba.error);
        Expect(rgba.width == 2 && rgba.height == 1, "RGBA8 preview dimensions are incorrect.");
        Expect(rgba.stride == 8, "RGBA8 preview stride is incorrect.");
        Expect(rgba.bgraPixels.size() == 8, "RGBA8 preview pixel buffer size is incorrect.");
        Expect(
            rgba.bgraPixels[0] == 30 && rgba.bgraPixels[1] == 20 &&
            rgba.bgraPixels[2] == 10 && rgba.bgraPixels[3] == 40,
            "RGBA8 preview was not converted to BGRA8.");

        const TexturePreviewResult gray = DecodePreviewFixture(
            tempRoot,
            "r8_preview",
            61u,
            2,
            1,
            8u,
            { 0, 255 });
        Expect(gray.success, "R8 preview decode failed: " + gray.error);
        Expect(gray.stride == 8 && gray.bgraPixels.size() == 8, "R8 preview was not expanded to BGRA8.");
        Expect(
            gray.bgraPixels[0] == 0 && gray.bgraPixels[1] == 0 &&
            gray.bgraPixels[2] == 0 && gray.bgraPixels[3] == 255 &&
            gray.bgraPixels[4] == 255 && gray.bgraPixels[5] == 255 &&
            gray.bgraPixels[6] == 255 && gray.bgraPixels[7] == 255,
            "R8 preview grayscale expansion is incorrect.");

        DirectX::ScratchImage rgbaImage = MakeRgbaImage();
        const DirectX::Image* rgbaBase = rgbaImage.GetImage(0, 0, 0);
        Expect(rgbaBase != nullptr, "BC7 preview validation image is missing.");
        const TexturePreviewResult bc7 = DecodePreviewFixture(
            tempRoot,
            "bc7_preview",
            98u,
            4,
            4,
            8u,
            CompressSingleBlock(*rgbaBase, DXGI_FORMAT_BC7_UNORM));
        Expect(bc7.success, "BC7 preview decode failed: " + bc7.error);
        Expect(bc7.width == 4 && bc7.height == 4 && bc7.stride == 16,
            "BC7 preview dimensions or stride are incorrect.");
        Expect(bc7.bgraPixels.size() == 64, "BC7 preview pixel buffer size is incorrect.");
    }

    std::string ExtractGlbJson(const std::filesystem::path& path)
    {
        const std::vector<uint8_t> bytes = ReadFileBytes(path);
        Expect(bytes.size() >= 20, "GLB file is too small.");

        const uint32_t jsonLength =
            static_cast<uint32_t>(bytes[12]) |
            (static_cast<uint32_t>(bytes[13]) << 8) |
            (static_cast<uint32_t>(bytes[14]) << 16) |
            (static_cast<uint32_t>(bytes[15]) << 24);
        const uint32_t jsonType =
            static_cast<uint32_t>(bytes[16]) |
            (static_cast<uint32_t>(bytes[17]) << 8) |
            (static_cast<uint32_t>(bytes[18]) << 16) |
            (static_cast<uint32_t>(bytes[19]) << 24);

        Expect(jsonType == 0x4E4F534Au, "GLB JSON chunk type is invalid.");
        Expect(bytes.size() >= 20ull + jsonLength, "GLB JSON chunk is truncated.");
        return std::string(reinterpret_cast<const char*>(bytes.data() + 20), jsonLength);
    }

    std::vector<uint8_t> ExtractGlbBin(const std::filesystem::path& path)
    {
        const std::vector<uint8_t> bytes = ReadFileBytes(path);
        Expect(bytes.size() >= 20, "GLB file is too small.");

        const uint32_t jsonLength =
            static_cast<uint32_t>(bytes[12]) |
            (static_cast<uint32_t>(bytes[13]) << 8) |
            (static_cast<uint32_t>(bytes[14]) << 16) |
            (static_cast<uint32_t>(bytes[15]) << 24);
        Expect(bytes.size() >= 20ull + jsonLength + 8ull, "GLB BIN chunk header is missing.");

        const size_t binChunkOffset = 20ull + jsonLength;
        const uint32_t binLength =
            static_cast<uint32_t>(bytes[binChunkOffset + 0]) |
            (static_cast<uint32_t>(bytes[binChunkOffset + 1]) << 8) |
            (static_cast<uint32_t>(bytes[binChunkOffset + 2]) << 16) |
            (static_cast<uint32_t>(bytes[binChunkOffset + 3]) << 24);
        const uint32_t binType =
            static_cast<uint32_t>(bytes[binChunkOffset + 4]) |
            (static_cast<uint32_t>(bytes[binChunkOffset + 5]) << 8) |
            (static_cast<uint32_t>(bytes[binChunkOffset + 6]) << 16) |
            (static_cast<uint32_t>(bytes[binChunkOffset + 7]) << 24);

        Expect(binType == 0x004E4942u, "GLB BIN chunk type is invalid.");
        Expect(bytes.size() >= binChunkOffset + 8ull + binLength, "GLB BIN chunk is truncated.");
        return std::vector<uint8_t>(
            bytes.begin() + static_cast<std::ptrdiff_t>(binChunkOffset + 8ull),
            bytes.begin() + static_cast<std::ptrdiff_t>(binChunkOffset + 8ull + binLength));
    }

    void VerifyGlbBufferViewsAndAccessors(const std::filesystem::path& glbPath)
    {
        const std::string json = ExtractGlbJson(glbPath);

        // Find "bufferViews" array
        size_t bufferViewsPos = json.find("\"bufferViews\"");
        Expect(bufferViewsPos != std::string::npos, "GLB JSON lacks bufferViews array.");

        // Find the start and end of the bufferViews array block
        size_t startBracket = json.find('[', bufferViewsPos);
        Expect(startBracket != std::string::npos, "GLB JSON bufferViews array start not found.");

        // Find matching closing bracket for bufferViews
        int bracketDepth = 1;
        size_t endBracket = startBracket + 1;
        while (endBracket < json.size() && bracketDepth > 0)
        {
            if (json[endBracket] == '[') bracketDepth++;
            else if (json[endBracket] == ']') bracketDepth--;
            endBracket++;
        }
        Expect(bracketDepth == 0, "GLB JSON bufferViews array is not properly closed.");

        std::string bufferViewsSection = json.substr(startBracket, endBracket - startBracket);

        // Count occurrences of `"buffer":` in the bufferViews array
        size_t numBufferViews = 0;
        size_t searchPos = 0;
        while ((searchPos = bufferViewsSection.find("\"buffer\"", searchPos)) != std::string::npos)
        {
            numBufferViews++;
            searchPos += 8;
        }

        Expect(numBufferViews > 0, "GLB JSON has zero bufferViews.");

        // Find all occurrences of `"bufferView": X` in the entire JSON
        searchPos = 0;
        while ((searchPos = json.find("\"bufferView\"", searchPos)) != std::string::npos)
        {
            // Skip the key and find the colon and the integer value
            size_t colonPos = json.find(':', searchPos);
            Expect(colonPos != std::string::npos, "GLB JSON bufferView key has no colon.");

            // Parse integer
            size_t valPos = colonPos + 1;
            while (valPos < json.size() && (std::isspace(static_cast<unsigned char>(json[valPos]))))
            {
                valPos++;
            }
            Expect(valPos < json.size(), "GLB JSON bufferView value is missing.");

            size_t valEnd = valPos;
            while (valEnd < json.size() && std::isdigit(static_cast<unsigned char>(json[valEnd])))
            {
                valEnd++;
            }
            Expect(valEnd > valPos, "GLB JSON bufferView value is not an integer.");

            std::string valStr = json.substr(valPos, valEnd - valPos);
            long long bufferViewIndex = std::stoll(valStr);

            Expect(bufferViewIndex >= 0 && bufferViewIndex < static_cast<long long>(numBufferViews),
                "GLB JSON bufferView index " + valStr + " is out of bounds (total bufferViews: " + std::to_string(numBufferViews) + ").");

            searchPos = valEnd;
        }
    }

    long long ParseJsonIntegerAfter(const std::string& json, size_t start, const std::string& key)
    {
        const size_t keyPos = json.find(key, start);
        Expect(keyPos != std::string::npos, "GLB JSON key not found: " + key);
        const size_t colonPos = json.find(':', keyPos);
        Expect(colonPos != std::string::npos, "GLB JSON key has no colon: " + key);

        size_t valuePos = colonPos + 1;
        while (valuePos < json.size() && std::isspace(static_cast<unsigned char>(json[valuePos])))
        {
            ++valuePos;
        }
        Expect(valuePos < json.size(), "GLB JSON integer value is missing: " + key);

        size_t valueEnd = valuePos;
        if (json[valueEnd] == '-')
        {
            ++valueEnd;
        }
        while (valueEnd < json.size() && std::isdigit(static_cast<unsigned char>(json[valueEnd])))
        {
            ++valueEnd;
        }
        Expect(valueEnd > valuePos, "GLB JSON value is not an integer: " + key);
        return std::stoll(json.substr(valuePos, valueEnd - valuePos));
    }

    size_t FindJsonObjectInArray(const std::string& json, size_t arrayStart, size_t objectIndex)
    {
        Expect(arrayStart < json.size() && json[arrayStart] == '[', "GLB JSON array start is invalid.");

        size_t currentObject = 0;
        int objectDepth = 0;
        for (size_t pos = arrayStart + 1; pos < json.size(); ++pos)
        {
            if (json[pos] == '{')
            {
                if (objectDepth == 0)
                {
                    if (currentObject == objectIndex)
                    {
                        return pos;
                    }
                    ++currentObject;
                }
                ++objectDepth;
            }
            else if (json[pos] == '}')
            {
                --objectDepth;
                Expect(objectDepth >= 0, "GLB JSON object nesting underflow.");
            }
            else if (json[pos] == ']' && objectDepth == 0)
            {
                break;
            }
        }

        Fail("GLB JSON array object index is out of bounds: " + std::to_string(objectIndex));
    }

    size_t FindJsonObjectInNamedArray(const std::string& json, const std::string& arrayName, size_t objectIndex)
    {
        const size_t arrayNamePos = json.find("\"" + arrayName + "\"");
        Expect(arrayNamePos != std::string::npos, "GLB JSON array not found: " + arrayName);
        const size_t arrayStart = json.find('[', arrayNamePos);
        Expect(arrayStart != std::string::npos, "GLB JSON array start not found: " + arrayName);
        return FindJsonObjectInArray(json, arrayStart, objectIndex);
    }

    std::array<float, 4> ReadAnimationVec4ForPath(
        const std::filesystem::path& glbPath,
        const std::string& path,
        size_t sampleIndex)
    {
        const std::string json = ExtractGlbJson(glbPath);
        const std::vector<uint8_t> bin = ExtractGlbBin(glbPath);

        const size_t pathPos = json.find("\"path\": \"" + path + "\"");
        Expect(pathPos != std::string::npos, "GLB JSON animation path not found: " + path);
        const size_t samplerKeyPos = json.rfind("\"sampler\"", pathPos);
        Expect(samplerKeyPos != std::string::npos, "GLB JSON animation channel lacks sampler.");
        const size_t samplerIndex = static_cast<size_t>(ParseJsonIntegerAfter(json, samplerKeyPos, "\"sampler\""));

        const size_t samplersPos = json.find("\"samplers\"", json.rfind("\"animations\"", pathPos));
        Expect(samplersPos != std::string::npos, "GLB JSON animation samplers array not found.");
        const size_t samplersArrayStart = json.find('[', samplersPos);
        Expect(samplersArrayStart != std::string::npos, "GLB JSON animation samplers array start not found.");
        const size_t samplerObject = FindJsonObjectInArray(json, samplersArrayStart, samplerIndex);
        const size_t outputAccessor = static_cast<size_t>(ParseJsonIntegerAfter(json, samplerObject, "\"output\""));

        const size_t accessorObject = FindJsonObjectInNamedArray(json, "accessors", outputAccessor);
        const size_t sampleCount = static_cast<size_t>(ParseJsonIntegerAfter(json, accessorObject, "\"count\""));
        Expect(sampleIndex < sampleCount, "Animation VEC4 sample index is out of bounds.");
        const size_t bufferView = static_cast<size_t>(ParseJsonIntegerAfter(json, accessorObject, "\"bufferView\""));
        const size_t bufferViewObject = FindJsonObjectInNamedArray(json, "bufferViews", bufferView);
        const size_t byteOffset = static_cast<size_t>(ParseJsonIntegerAfter(json, bufferViewObject, "\"byteOffset\""));
        const size_t sampleByteOffset = byteOffset + (sampleIndex * sizeof(float) * 4);

        Expect(sampleByteOffset + (sizeof(float) * 4) <= bin.size(), "Animation VEC4 sample is out of bounds.");
        const auto readFloat = [&](size_t offset)
        {
            float value = 0.0f;
            std::memcpy(&value, bin.data() + offset, sizeof(float));
            return value;
        };
        return {
            readFloat(sampleByteOffset + 0),
            readFloat(sampleByteOffset + 4),
            readFloat(sampleByteOffset + 8),
            readFloat(sampleByteOffset + 12)
        };
    }

    std::array<float, 4> ReadFirstAnimationVec4ForPath(
        const std::filesystem::path& glbPath,
        const std::string& path)
    {
        return ReadAnimationVec4ForPath(glbPath, path, 0);
    }

    std::array<float, 3> ReadAnimationVec3ForPath(
        const std::filesystem::path& glbPath,
        const std::string& path,
        size_t sampleIndex)
    {
        const std::string json = ExtractGlbJson(glbPath);
        const std::vector<uint8_t> bin = ExtractGlbBin(glbPath);

        const size_t pathPos = json.find("\"path\": \"" + path + "\"");
        Expect(pathPos != std::string::npos, "GLB JSON animation path not found: " + path);
        const size_t samplerKeyPos = json.rfind("\"sampler\"", pathPos);
        Expect(samplerKeyPos != std::string::npos, "GLB JSON animation channel lacks sampler.");
        const size_t samplerIndex = static_cast<size_t>(ParseJsonIntegerAfter(json, samplerKeyPos, "\"sampler\""));

        const size_t samplersPos = json.find("\"samplers\"", json.rfind("\"animations\"", pathPos));
        Expect(samplersPos != std::string::npos, "GLB JSON animation samplers array not found.");
        const size_t samplersArrayStart = json.find('[', samplersPos);
        Expect(samplersArrayStart != std::string::npos, "GLB JSON animation samplers array start not found.");
        const size_t samplerObject = FindJsonObjectInArray(json, samplersArrayStart, samplerIndex);
        const size_t outputAccessor = static_cast<size_t>(ParseJsonIntegerAfter(json, samplerObject, "\"output\""));

        const size_t accessorObject = FindJsonObjectInNamedArray(json, "accessors", outputAccessor);
        const size_t sampleCount = static_cast<size_t>(ParseJsonIntegerAfter(json, accessorObject, "\"count\""));
        Expect(sampleIndex < sampleCount, "Animation VEC3 sample index is out of bounds.");
        const size_t bufferView = static_cast<size_t>(ParseJsonIntegerAfter(json, accessorObject, "\"bufferView\""));
        const size_t bufferViewObject = FindJsonObjectInNamedArray(json, "bufferViews", bufferView);
        const size_t byteOffset = static_cast<size_t>(ParseJsonIntegerAfter(json, bufferViewObject, "\"byteOffset\""));
        const size_t sampleByteOffset = byteOffset + (sampleIndex * sizeof(float) * 3);

        Expect(sampleByteOffset + (sizeof(float) * 3) <= bin.size(), "Animation VEC3 sample is out of bounds.");
        const auto readFloat = [&](size_t offset)
        {
            float value = 0.0f;
            std::memcpy(&value, bin.data() + offset, sizeof(float));
            return value;
        };
        return {
            readFloat(sampleByteOffset + 0),
            readFloat(sampleByteOffset + 4),
            readFloat(sampleByteOffset + 8)
        };
    }

    void ExpectQuatNear(
        const std::array<float, 4>& actual,
        const std::array<float, 4>& expected,
        const std::string& message)
    {
        const float dot =
            (actual[0] * expected[0]) +
            (actual[1] * expected[1]) +
            (actual[2] * expected[2]) +
            (actual[3] * expected[3]);
        Expect(std::fabs(std::fabs(dot) - 1.0f) <= 1e-4f, message);
    }

    std::array<float, 4> MultiplyQuaternionsForTest(
        const std::array<float, 4>& left,
        const std::array<float, 4>& right)
    {
        std::array<float, 4> result{
            (left[3] * right[0]) + (left[0] * right[3]) + (left[1] * right[2]) - (left[2] * right[1]),
            (left[3] * right[1]) - (left[0] * right[2]) + (left[1] * right[3]) + (left[2] * right[0]),
            (left[3] * right[2]) + (left[0] * right[1]) - (left[1] * right[0]) + (left[2] * right[3]),
            (left[3] * right[3]) - (left[0] * right[0]) - (left[1] * right[1]) - (left[2] * right[2])
        };
        const float length =
            std::sqrt(
                (result[0] * result[0]) +
                (result[1] * result[1]) +
                (result[2] * result[2]) +
                (result[3] * result[3]));
        Expect(length > 1e-6f, "Test quaternion multiplication produced a zero-length result.");
        result[0] /= length;
        result[1] /= length;
        result[2] /= length;
        result[3] /= length;
        return result;
    }

    void ExpectVec3Near(
        const std::array<float, 3>& actual,
        const std::array<float, 3>& expected,
        const std::string& message)
    {
        Expect(std::fabs(actual[0] - expected[0]) <= 1e-4f &&
            std::fabs(actual[1] - expected[1]) <= 1e-4f &&
            std::fabs(actual[2] - expected[2]) <= 1e-4f,
            message);
    }

    float ReadLeFloat(const std::vector<uint8_t>& bytes, size_t offset)
    {
        Expect(offset + sizeof(float) <= bytes.size(), "Float read is out of bounds.");
        float value = 0.0f;
        std::memcpy(&value, bytes.data() + offset, sizeof(float));
        return value;
    }

    uint32_t ReadLeUInt32(const std::vector<uint8_t>& bytes, size_t offset)
    {
        Expect(offset + sizeof(uint32_t) <= bytes.size(), "UInt32 read is out of bounds.");
        uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(uint32_t));
        return value;
    }

    void ExpectNear(float actual, float expected, const std::string& message)
    {
        if (std::fabs(actual - expected) > 1e-4f)
        {
            Fail(message + " expected=" + std::to_string(expected) + " actual=" + std::to_string(actual));
        }
    }

    void ExpectRigidDynTriangleGeometry(
        const std::filesystem::path& glbPath,
        float expectedX,
        float expectedY,
        float expectedZ)
    {
        constexpr size_t kPackedVertexStride = 56;
        constexpr size_t kVertexCount = 3;
        constexpr size_t kIndexOffset = kPackedVertexStride * kVertexCount;

        const std::vector<uint8_t> bin = ExtractGlbBin(glbPath);
        Expect(bin.size() >= kIndexOffset + 12, "DynModel validation BIN payload is too small.");

        ExpectNear(ReadLeFloat(bin, 0), expectedX, "DynModel vertex X mismatch.");
        ExpectNear(ReadLeFloat(bin, 4), expectedY, "DynModel vertex Y mismatch.");
        ExpectNear(ReadLeFloat(bin, 8), expectedZ, "DynModel vertex Z mismatch.");
        ExpectNear(ReadLeFloat(bin, 12), 0.0f, "DynModel normal X mismatch.");
        ExpectNear(ReadLeFloat(bin, 16), 0.0f, "DynModel normal Y mismatch.");
        ExpectNear(ReadLeFloat(bin, 20), 1.0f, "DynModel normal Z mismatch.");

        Expect(ReadLeUInt32(bin, kIndexOffset + 0) == 0, "DynModel triangle index 0 mismatch.");
        Expect(ReadLeUInt32(bin, kIndexOffset + 4) == 1, "DynModel triangle index 1 mismatch.");
        Expect(ReadLeUInt32(bin, kIndexOffset + 8) == 2, "DynModel triangle index 2 mismatch.");
    }

    std::array<float, 16> MakeTranslationMatrix(float x, float y, float z)
    {
        return {
            1.0f, 0.0f, 0.0f, x,
            0.0f, 1.0f, 0.0f, y,
            0.0f, 0.0f, 1.0f, z,
            0.0f, 0.0f, 0.0f, 1.0f
        };
    }

    std::array<float, 16> MakeInverseTranslationMatrix(float x, float y, float z)
    {
        return MakeTranslationMatrix(-x, -y, -z);
    }

    DagorFormats::GeomNodeTreeNode MakeSkeletonNode(const std::string& name, float x, float y, float z)
    {
        DagorFormats::GeomNodeTreeNode node;
        node.name = name;
        node.localTransform = MakeTranslationMatrix(x, y, z);
        node.worldTransform = node.localTransform;
        node.inverseBindTransform = MakeInverseTranslationMatrix(x, y, z);
        return node;
    }

    DagorFormats::GeomNodeTreeNode MakeZRotatedSkeletonNode(const std::string& name)
    {
        DagorFormats::GeomNodeTreeNode node;
        node.name = name;
        node.localTransform = {
            0.0f, -1.0f, 0.0f, 0.0f,
            1.0f,  0.0f, 0.0f, 0.0f,
            0.0f,  0.0f, 1.0f, 0.0f,
            0.0f,  0.0f, 0.0f, 1.0f
        };
        node.worldTransform = node.localTransform;
        node.inverseBindTransform = {
             0.0f, 1.0f, 0.0f, 0.0f,
            -1.0f, 0.0f, 0.0f, 0.0f,
             0.0f, 0.0f, 1.0f, 0.0f,
             0.0f, 0.0f, 0.0f, 1.0f
        };
        return node;
    }

    DagorFormats::GeomNodeTreeNode MakeScaledSkeletonNode(
        const std::string& name, float x, float y, float z, float sx, float sy, float sz)
    {
        DagorFormats::GeomNodeTreeNode node;
        node.name = name;
        node.localTransform = {
            sx,   0.0f, 0.0f, x,
            0.0f, sy,   0.0f, y,
            0.0f, 0.0f, sz,   z,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        node.worldTransform = node.localTransform;
        node.inverseBindTransform = {
            1.0f/sx, 0.0f,    0.0f,    -x/sx,
            0.0f,    1.0f/sy, 0.0f,    -y/sy,
            0.0f,    0.0f,    1.0f/sz, -z/sz,
            0.0f,    0.0f,    0.0f,    1.0f
        };
        return node;
    }

    DagorFormats::GeomNodeTreeNode MakeChildSkeletonNodeScaledParent(
        const std::string& name,
        uint16_t parentIndex,
        float px, float py, float pz,
        float sx, float sy, float sz,
        float lx, float ly, float lz)
    {
        DagorFormats::GeomNodeTreeNode node;
        node.name = name;
        node.parentIndex = parentIndex;
        
        float wx = px + sx * lx;
        float wy = py + sy * ly;
        float wz = pz + sz * lz;
        
        node.worldTransform = {
            sx,   0.0f, 0.0f, wx,
            0.0f, sy,   0.0f, wy,
            0.0f, 0.0f, sz,   wz,
            0.0f, 0.0f, 0.0f, 1.0f
        };
        
        node.inverseBindTransform = {
            1.0f/sx, 0.0f,    0.0f,    -wx/sx,
            0.0f,    1.0f/sy, 0.0f,    -wy/sy,
            0.0f,    0.0f,    1.0f/sz, -wz/sz,
            0.0f,    0.0f,    0.0f,    1.0f
        };
        return node;
    }

    DagorFormats::DynModelMesh MakeRigidDynMesh(const std::string& name, const std::string& parentNodeName)
    {
        DagorFormats::DynModelMesh mesh;
        mesh.name = name;
        mesh.parentNodeName = parentNodeName;
        mesh.useSkinning = false;

        DagorFormats::SkinnedVertex v0{};
        v0.x = 0.0f;
        v0.y = 0.0f;
        v0.z = 0.0f;
        v0.nz = 1.0f;
        v0.u = 0.0f;
        v0.v = 0.0f;

        DagorFormats::SkinnedVertex v1 = v0;
        v1.x = 1.0f;
        v1.u = 1.0f;

        DagorFormats::SkinnedVertex v2 = v0;
        v2.y = 1.0f;
        v2.v = 1.0f;

        mesh.vertices = { v0, v1, v2 };
        mesh.indices = { 0, 1, 2 };
        mesh.primitives.push_back({ 0, 0, 3 });
        return mesh;
    }

    void RunStep2Validation(const std::filesystem::path& tempRoot)
    {
        DirectX::ScratchImage rgba = MakeRgbaImage();
        const DirectX::Image* rgbaBase = rgba.GetImage(0, 0, 0);
        Expect(rgbaBase != nullptr, "RGBA validation image is missing.");
        ValidateReadableExport(
            tempRoot,
            "bc7_validation",
            98u,
            8u,
            CompressSingleBlock(*rgbaBase, DXGI_FORMAT_BC7_UNORM));

        DirectX::ScratchImage hdr = MakeHdrImage();
        const DirectX::Image* hdrBase = hdr.GetImage(0, 0, 0);
        Expect(hdrBase != nullptr, "HDR validation image is missing.");
        ValidateReadableExport(
            tempRoot,
            "bc6h_validation",
            95u,
            8u,
            CompressSingleBlock(*hdrBase, DXGI_FORMAT_BC6H_UF16));
    }

    void RunStep3ResolverValidation()
    {
        DagorFormats::InternalMaterial dynamicMaterial;
        dynamicMaterial.shaderClass = "dynamic_vehicle";
        dynamicMaterial.textureSlots[0] = "tank_albedo";
        dynamicMaterial.textureSlots[1] = "tank_mask";
        dynamicMaterial.textureSlots[2] = "tank_normal";
        dynamicMaterial.textureSlots[3] = "tank_ao";

        const ResolvedMaterialTextures resolved = DagorAssetsExtractor::Export::ResolveMaterialTextures(dynamicMaterial);
        Expect(resolved.baseColorTextureName == "tank_albedo", "Base-color resolver picked the wrong texture.");
        Expect(resolved.normalTextureName == "tank_normal", "Normal resolver picked the wrong texture.");
        Expect(resolved.occlusionTextureName == "tank_ao", "Occlusion resolver missed the documented AO slot.");
        Expect(resolved.metallicRoughnessTextureName.empty(), "Generic mask texture should not be treated as metallic-roughness.");

        DagorFormats::InternalMaterial packedVehicleMaterial;
        packedVehicleMaterial.shaderClass = "dynamic_vehicle";
        packedVehicleMaterial.textureSlots[0] = "tank_body_c$hq";
        packedVehicleMaterial.textureSlots[2] = "tank_body_n$hq";
        packedVehicleMaterial.textureSlots[3] = "tank_body_ao$hq";
        const ResolvedMaterialTextures packedVehicleResolved =
            DagorAssetsExtractor::Export::ResolveMaterialTextures(packedVehicleMaterial);
        Expect(
            packedVehicleResolved.packedNormalPbrTextureName == "tank_body_n$hq",
            "Resolver did not mark the War Thunder packed normal/PBR source.");

        DagorFormats::InternalMaterial cockpitGaugeMaterial;
        cockpitGaugeMaterial.shaderClass = "dynamic";
        cockpitGaugeMaterial.textureSlots[0] =
            "us_gauge_oxygen_pressure_400psi_c*?na16";
        cockpitGaugeMaterial.textureSlots[2] =
            "us_gauge_oxygen_pressure_400psi_n*?na16u03";
        const ResolvedMaterialTextures cockpitGaugeResolved =
            DagorAssetsExtractor::Export::ResolveMaterialTextures(cockpitGaugeMaterial);
        Expect(
            cockpitGaugeResolved.baseColorTextureName ==
                "us_gauge_oxygen_pressure_400psi_c*?na16",
            "Resolver did not recognize a sampler-modified cockpit color texture.");
        Expect(
            cockpitGaugeResolved.normalTextureName ==
                "us_gauge_oxygen_pressure_400psi_n*?na16u03",
            "Resolver did not recognize a sampler-modified cockpit packed normal.");
        Expect(
            cockpitGaugeResolved.packedNormalPbrTextureName ==
                cockpitGaugeResolved.normalTextureName,
            "Resolver did not mark the sampler-modified cockpit _n texture as packed PBR.");

        DagorFormats::InternalMaterial invalidNormalSlotMaterial;
        invalidNormalSlotMaterial.shaderClass = "dynamic";
        invalidNormalSlotMaterial.textureSlots[2] =
            "us_gauge_oxygen_pressure_400psi_c*?na16";
        const ResolvedMaterialTextures invalidNormalSlotResolved =
            DagorAssetsExtractor::Export::ResolveMaterialTextures(invalidNormalSlotMaterial);
        Expect(
            invalidNormalSlotResolved.normalTextureName.empty(),
            "A color texture in a nominal normal slot must not be resolved as a normal map.");

        DagorFormats::InternalMaterial ormMaterial;
        ormMaterial.shaderClass = "dynamic_vehicle";
        ormMaterial.textureSlots[1] = "tank_orm";
        const ResolvedMaterialTextures ormResolved = DagorAssetsExtractor::Export::ResolveMaterialTextures(ormMaterial);
        Expect(ormResolved.metallicRoughnessTextureName == "tank_orm", "Packed ORM texture was not recognized.");
        Expect(ormResolved.occlusionTextureName == "tank_orm", "Packed ORM texture was not mapped to occlusion slot.");

        Expect(DagorAssetsExtractor::Export::LooksLikePackedOrm("tank_orm"), "LooksLikePackedOrm failed for tank_orm");
        Expect(DagorAssetsExtractor::Export::LooksLikePackedOrm("plane_rma"), "LooksLikePackedOrm failed for plane_rma");
        Expect(!DagorAssetsExtractor::Export::LooksLikePackedOrm("plane_normal"), "LooksLikePackedOrm matched plane_normal wrongly");
        Expect(DagorAssetsExtractor::Export::LooksLikePackedNormalMap("f_16c_n"), "LooksLikePackedNormalMap failed for f_16c_n");
        Expect(DagorAssetsExtractor::Export::LooksLikePackedNormalMap("f_16c_n$hq"), "LooksLikePackedNormalMap failed for f_16c_n$hq");
        Expect(!DagorAssetsExtractor::Export::LooksLikePackedNormalMap("tank_orm"), "LooksLikePackedNormalMap matched tank_orm wrongly");

        const auto packedNormalAnalysis =
            DagorAssetsExtractor::Export::AnalyzeTextureMaps("tank_body_n$hq");
        Expect(
            packedNormalAnalysis.layout ==
                DagorAssetsExtractor::Export::TextureContentLayout::WarThunderPackedNormalPbr,
            "Packed War Thunder normal/PBR layout was not detected.");
        Expect(
            std::any_of(
                packedNormalAnalysis.maps.begin(),
                packedNormalAnalysis.maps.end(),
                [](const auto& map) { return map.kind == TextureMapKind::Metallic; }),
            "Packed War Thunder texture analysis omitted metallic.");
        Expect(
            std::any_of(
                packedNormalAnalysis.maps.begin(),
                packedNormalAnalysis.maps.end(),
                [](const auto& map) {
                    return map.kind == TextureMapKind::InvertedRoughness;
                }),
            "Packed War Thunder texture analysis omitted native inverted roughness.");
        Expect(
            std::any_of(
                packedNormalAnalysis.maps.begin(),
                packedNormalAnalysis.maps.end(),
                [](const auto& map) {
                    return map.kind == TextureMapKind::OcclusionRoughnessMetallic;
                }),
            "Packed War Thunder texture analysis omitted standard ORM output.");
        Expect(
            std::none_of(
                packedNormalAnalysis.maps.begin(),
                packedNormalAnalysis.maps.end(),
                [](const auto& map) { return map.kind == TextureMapKind::AmbientOcclusion; }),
            "Packed War Thunder _n texture analysis must not claim AO.");

        const auto aoAnalysis =
            DagorAssetsExtractor::Export::AnalyzeTextureMaps("tank_body_ao$hq");
        Expect(
            aoAnalysis.layout ==
                DagorAssetsExtractor::Export::TextureContentLayout::AmbientOcclusion,
            "AO texture layout was not detected.");
        Expect(
            DagorAssetsExtractor::Export::AnalyzeTextureMaps("plane_c$hq").layout ==
                DagorAssetsExtractor::Export::TextureContentLayout::BaseColor,
            "War Thunder _c color texture layout was not detected.");
        Expect(
            DagorAssetsExtractor::Export::AnalyzeTextureMaps("plane_d").layout ==
                DagorAssetsExtractor::Export::TextureContentLayout::BaseColor,
            "Shader-dependent _d color texture layout was not detected.");
        Expect(
            DagorAssetsExtractor::Export::AnalyzeTextureMaps("plane_normal").layout ==
                DagorAssetsExtractor::Export::TextureContentLayout::ConventionalNormal,
            "Conventional normal texture layout was not detected.");
        Expect(
            DagorAssetsExtractor::Export::AnalyzeTextureMaps(
                "us_gauge_oxygen_pressure_400psi_c*?na16").layout ==
                DagorAssetsExtractor::Export::TextureContentLayout::BaseColor,
            "Sampler-modified cockpit _c texture layout was not detected.");
        Expect(
            DagorAssetsExtractor::Export::AnalyzeTextureMaps(
                "us_gauge_oxygen_pressure_400psi_n*?na16u03").layout ==
                DagorAssetsExtractor::Export::TextureContentLayout::WarThunderPackedNormalPbr,
            "Sampler-modified cockpit _n texture layout was not detected.");
        Expect(
            DagorAssetsExtractor::Export::AnalyzeTextureMaps(
                "prop_nm_spec*?abbwq0-0-1").layout ==
                DagorAssetsExtractor::Export::TextureContentLayout::ConventionalNormal,
            "Sampler-modified _nm_spec conventional normal layout was not detected.");
        Expect(
            DagorAssetsExtractor::Export::AnalyzeTextureMaps("tank_arm").layout ==
                DagorAssetsExtractor::Export::TextureContentLayout::PackedOrm,
            "ARM alias was not normalized to the standard ORM layout.");
        Expect(
            DagorAssetsExtractor::Export::AnalyzeTextureMaps("tank_mrao").layout ==
                DagorAssetsExtractor::Export::TextureContentLayout::PackedMra,
            "MRAO texture layout was not detected.");

        const auto unknownAnalysis =
            DagorAssetsExtractor::Export::AnalyzeTextureMaps("tank_body_mask");
        Expect(
            unknownAnalysis.layout ==
                DagorAssetsExtractor::Export::TextureContentLayout::Unknown &&
                unknownAnalysis.maps.empty(),
            "Unknown texture names must fail detection safely.");

        Expect(
            DagorAssetsExtractor::Export::ClassifyDescriptorBinding("dynamic_vehicle", "t3", "tank_ao") ==
                MaterialTextureSemantic::Occlusion,
            "Descriptor binding classifier missed the AO slot.");
        Expect(
            DagorAssetsExtractor::Export::ClassifyDescriptorBinding("dynamic_vehicle", "t1", "tank_mask") ==
                MaterialTextureSemantic::Unknown,
            "Descriptor binding classifier should not map generic mask textures to metallic-roughness.");
        Expect(
            DagorAssetsExtractor::Export::ClassifyDescriptorBinding("dynamic_vehicle", "t1", "tank_orm") ==
                MaterialTextureSemantic::MetallicRoughness,
            "Descriptor binding classifier missed the packed ORM texture.");
        Expect(
            DagorAssetsExtractor::Export::ClassifyDescriptorBinding(
                "dynamic",
                "t2",
                "us_gauge_oxygen_pressure_400psi_c*?na16") ==
                MaterialTextureSemantic::BaseColor,
            "Known cockpit color semantics must override a generic normal-slot guess.");
        Expect(
            DagorAssetsExtractor::Export::ClassifyDescriptorBinding(
                "dynamic",
                "t0",
                "us_gauge_oxygen_pressure_400psi_n*?na16u03") ==
                MaterialTextureSemantic::Normal,
            "Known cockpit packed-normal semantics must override a generic color-slot guess.");
    }

    void RunSettingsPersistenceValidation(const std::filesystem::path& tempRoot)
    {
        const std::filesystem::path settingsPath =
            tempRoot / "settings" / "settings.ini";
        DagorAssetsExtractor::App::AppSettings settings;
        settings.gameRoot = tempRoot / "game";
        settings.exportRoot = tempRoot / "exports";
        settings.convertWarThunderPackedMaps = false;

        std::string error;
        Expect(
            DagorAssetsExtractor::App::SaveSharedSettings(
                settings,
                settingsPath,
                "[Window][Validation]\nPos=10,10\n",
                error),
            "Could not save material conversion setting: " + error);

        DagorAssetsExtractor::App::AppSettings loaded;
        DagorAssetsExtractor::App::LoadSettings(loaded, settingsPath);
        Expect(
            !loaded.convertWarThunderPackedMaps,
            "War Thunder packed-map conversion setting was not persisted.");
        Expect(
            DagorAssetsExtractor::App::LoadImGuiSettings(settingsPath).find(
                "[Window][Validation]") != std::string::npos,
            "Saving the packed-map conversion setting discarded ImGui settings.");
    }

    void RunStep3GlbValidation(const std::filesystem::path& tempRoot)
    {
        DagorFormats::RendInstMesh mesh;
        mesh.name = "validation_mesh";
        mesh.vertices = {
            { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, DagorFormats::UvTransform::None },
            { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, DagorFormats::UvTransform::None },
            { 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, DagorFormats::UvTransform::None },
        };
        mesh.indices = { 0, 1, 2 };
        mesh.primitives.push_back({ 0, 0, 3 });

        MaterialInfo material;
        material.name = "validation_material";
        material.diffuseTexturePath = "diffuse.png";
        material.normalTexturePath = "normal.png";
        material.metallicRoughnessTexturePath = "orm.png";
        material.occlusionTexturePath = "orm.png";

        ExportManifest manifest;
        manifest.skipWrite = true;

        const std::filesystem::path glbPath = tempRoot / "validation_materials.glb";
        GlbExportOptions options;
        options.embedTexturesInGlb = false;

        Expect(
            ModelExporter::ExportToGlb(glbPath, { mesh }, manifest, { material }, options),
            "GLB export failed for material-channel validation.");
        Expect(std::filesystem::exists(glbPath), "GLB export did not create an output file.");

        const std::string json = ExtractGlbJson(glbPath);
        Expect(json.find("\"baseColorTexture\"") != std::string::npos, "GLB JSON is missing baseColorTexture.");
        Expect(json.find("\"normalTexture\"") != std::string::npos, "GLB JSON is missing normalTexture.");
        Expect(json.find("\"metallicRoughnessTexture\"") != std::string::npos, "GLB JSON is missing metallicRoughnessTexture.");
        Expect(json.find("\"occlusionTexture\"") != std::string::npos, "GLB JSON is missing occlusionTexture.");
        Expect(json.find("diffuse.png") != std::string::npos, "GLB JSON is missing the diffuse image URI.");
        Expect(json.find("normal.png") != std::string::npos, "GLB JSON is missing the normal image URI.");
        Expect(
            json.find("orm.png") != std::string::npos,
            "GLB JSON is missing the shared ORM image URI.");
        VerifyGlbBufferViewsAndAccessors(glbPath);

        MaterialInfo nativePackedMaterial;
        nativePackedMaterial.name = "native_packed_material";
        nativePackedMaterial.nativePackedNormalPbrTexturePath = "native_n.png";
        const std::filesystem::path nativeGlbPath =
            tempRoot / "validation_native_packed_material.glb";
        Expect(
            ModelExporter::ExportToGlb(
                nativeGlbPath,
                { mesh },
                manifest,
                { nativePackedMaterial },
                options),
            "GLB export failed for native packed material validation.");

        const std::string nativeJson = ExtractGlbJson(nativeGlbPath);
        Expect(
            nativeJson.find("\"extensionsUsed\": [\"DAGOR_material_war_thunder_packed_n\"]") !=
                std::string::npos,
            "Native packed GLB did not declare its optional material extension.");
        Expect(
            nativeJson.find("\"DAGOR_material_war_thunder_packed_n\"") !=
                std::string::npos,
            "Native packed GLB material extension is missing.");
        Expect(
            nativeJson.find("\"invertedRoughness\": \"R\"") != std::string::npos &&
                nativeJson.find("\"normalX\": \"G\"") != std::string::npos &&
                nativeJson.find("\"metallic\": \"B\"") != std::string::npos &&
                nativeJson.find("\"normalY\": \"A\"") != std::string::npos,
            "Native packed GLB material extension has the wrong channel contract.");
        Expect(
            nativeJson.find("\"normalTexture\"") == std::string::npos &&
                nativeJson.find("\"metallicRoughnessTexture\"") == std::string::npos &&
                nativeJson.find("\"occlusionTexture\"") == std::string::npos,
            "Native packed texture was incorrectly assigned to core glTF material slots.");
        Expect(
            nativeJson.find("native_n.png") != std::string::npos,
            "Native packed image URI is missing from GLB JSON.");
        VerifyGlbBufferViewsAndAccessors(nativeGlbPath);
    }


    void RunStep3PackedNormalMapValidation(const std::filesystem::path& tempRoot)
    {
        std::vector<uint8_t> payload(16 * 4);
        for (size_t i = 0; i < 16; ++i)
        {
            size_t idx = i * 4;
            payload[idx + 0] = 185; // R: modern non-linear inverted roughness
            payload[idx + 1] = 64;  // G: normal X
            payload[idx + 2] = 192; // B: metallic
            payload[idx + 3] = 100; // A: normal Y
        }

        const std::filesystem::path packPath = tempRoot / "packed_normal_validation.pack";
        WriteFileBytes(packPath, payload);

        SubAssetRecord entry;
        entry.name = "test_normal_n*?na16u03";
        entry.offset = 0;
        entry.size = static_cast<uint32_t>(payload.size());
        entry.ddsxHeader = BuildDdsxHeader(
            28u, // R8G8B8A8
            4,
            4,
            1,
            32u,
            0u,
            static_cast<uint32_t>(payload.size()),
            0u);

        TextureExportOptions options;
        options.outputDir = tempRoot / "exports";
        options.format = "PNG";
        options.preserveOriginalDDS = false;
        options.overwrite = true;
        options.exportJsonManifest = false;
        options.generateMaterialMaps = true;

        const TextureExportResult result = TextureExporter::ExportTexture(packPath, entry, options);
        Expect(result.success, "Packed normal map split export failed: " + result.error);
        TextureExportOptions noOverwriteOptions = options;
        noOverwriteOptions.overwrite = false;
        const TextureExportResult duplicateResult =
            TextureExporter::ExportTexture(packPath, entry, noOverwriteOptions);
        Expect(
            !duplicateResult.success &&
                duplicateResult.error.find("already exists") != std::string::npos,
            "Converted material-map export did not enforce overwrite=false on its renamed output.");

        TexturePreviewOptions previewOptions;
        const TexturePreviewResult preview =
            TextureExporter::DecodePreview(packPath, entry, previewOptions);
        Expect(preview.success, "Packed map preview decode failed: " + preview.error);
        const auto mapPreviews = TextureExporter::BuildMapPreviews(
            entry.name,
            preview.bgraPixels.data(),
            preview.width,
            preview.height,
            preview.stride,
            2);
        Expect(mapPreviews.size() == 5, "Packed _n texture did not produce five map thumbnails.");
        const auto metallicPreviewIt = std::find_if(
            mapPreviews.begin(),
            mapPreviews.end(),
            [](const auto& mapPreview) {
                return mapPreview.kind == TextureMapKind::Metallic;
            });
        Expect(
            metallicPreviewIt != mapPreviews.end() &&
                metallicPreviewIt->width == 2 &&
                metallicPreviewIt->height == 2 &&
                metallicPreviewIt->bgraPixels[0] == 192 &&
                metallicPreviewIt->bgraPixels[1] == 192 &&
                metallicPreviewIt->bgraPixels[2] == 192,
            "Metallic map thumbnail did not preserve the source blue channel.");
        const auto roughnessPreviewIt = std::find_if(
            mapPreviews.begin(),
            mapPreviews.end(),
            [](const auto& mapPreview) {
                return mapPreview.kind == TextureMapKind::Roughness;
            });
        Expect(
            roughnessPreviewIt != mapPreviews.end() &&
                roughnessPreviewIt->bgraPixels[0] == 127 &&
                roughnessPreviewIt->bgraPixels[1] == 127 &&
                roughnessPreviewIt->bgraPixels[2] == 127,
            "Roughness map thumbnail did not decode the modern War Thunder red channel.");
        const auto invertedRoughnessPreviewIt = std::find_if(
            mapPreviews.begin(),
            mapPreviews.end(),
            [](const auto& mapPreview) {
                return mapPreview.kind == TextureMapKind::InvertedRoughness;
            });
        Expect(
            invertedRoughnessPreviewIt != mapPreviews.end() &&
                invertedRoughnessPreviewIt->bgraPixels[0] == 185 &&
                invertedRoughnessPreviewIt->bgraPixels[1] == 185 &&
                invertedRoughnessPreviewIt->bgraPixels[2] == 185,
            "Native inverted-roughness thumbnail did not preserve source red.");

        const std::filesystem::path normalPath = result.outputPath;
        const std::filesystem::path generatedOrmPath =
            FindMapOutput(result, TextureMapKind::OcclusionRoughnessMetallic);

        Expect(std::filesystem::exists(normalPath), "Reconstructed normal map PNG does not exist.");
        Expect(std::filesystem::exists(generatedOrmPath), "Generated ORM PNG does not exist.");
        Expect(
            FindMapOutput(result, TextureMapKind::Normal) == normalPath,
            "Normal output was not tracked explicitly.");
        Expect(
            normalPath.stem().string().ends_with("_normal"),
            "Converted normal output filename does not identify the generated map.");
        Expect(
            generatedOrmPath.stem().string().ends_with("_orm"),
            "Converted ORM output filename does not identify the generated map.");
        Expect(
            WarningContains(result, "roughness decodes the non-linear red channel"),
            "Expected packed-channel conversion warning in result.");

        const std::array<uint8_t, 4> normalPixel = ReadFirstPngPixelRgba(normalPath);
        Expect(
            normalPixel[0] == 64 && normalPixel[1] == 100 &&
                normalPixel[2] >= 233 && normalPixel[2] <= 236 &&
                normalPixel[3] == 255,
            "Reconstructed normal channels are incorrect: R=" +
                std::to_string(normalPixel[0]) + ", G=" +
                std::to_string(normalPixel[1]) + ", B=" +
                std::to_string(normalPixel[2]) + ", A=" +
                std::to_string(normalPixel[3]));

        const std::array<uint8_t, 4> generatedOrmPixel =
            ReadFirstPngPixelRgba(generatedOrmPath);
        Expect(
            generatedOrmPixel[0] == 255 && generatedOrmPixel[1] == 127 &&
                generatedOrmPixel[2] == 192 && generatedOrmPixel[3] == 255,
            "Generated ORM is incorrect; AO must default white and metallic must use source blue.");

        TextureExportOptions nativeOptions = options;
        nativeOptions.outputDir = tempRoot / "native_exports";
        nativeOptions.convertWarThunderPackedMaps = false;
        const TextureExportResult nativeResult =
            TextureExporter::ExportTexture(packPath, entry, nativeOptions);
        Expect(
            nativeResult.success,
            "Native packed map export failed: " + nativeResult.error);
        Expect(
            nativeResult.nativeWarThunderPackedMap,
            "Native packed map export was not identified in the result.");
        Expect(
            nativeResult.mapOutputs.empty(),
            "Native packed mode generated standard material maps unexpectedly.");
        Expect(
            !nativeResult.outputPath.stem().string().ends_with("_normal") &&
                !nativeResult.outputPath.stem().string().ends_with("_orm"),
            "Native packed output was renamed as a generated standard map.");
        const std::array<uint8_t, 4> nativePixel =
            ReadFirstPngPixelRgba(nativeResult.outputPath);
        Expect(
            nativePixel[0] == 185 && nativePixel[1] == 64 &&
                nativePixel[2] == 192 && nativePixel[3] == 100,
            "Native packed output did not preserve the source RGBA channel values.");

        std::vector<uint8_t> aoPayload(16 * 4);
        for (size_t i = 0; i < 16; ++i)
        {
            const size_t idx = i * 4;
            aoPayload[idx + 0] = 42;
            aoPayload[idx + 1] = 42;
            aoPayload[idx + 2] = 42;
            aoPayload[idx + 3] = 255;
        }

        const std::filesystem::path aoPackPath = tempRoot / "ao_validation.pack";
        WriteFileBytes(aoPackPath, aoPayload);
        SubAssetRecord aoEntry = entry;
        aoEntry.name = "test_ao";
        aoEntry.size = static_cast<uint32_t>(aoPayload.size());

        TextureMapExportOptions mapOptions;
        mapOptions.outputDir = options.outputDir;
        mapOptions.format = "PNG";
        const std::vector<TextureExportResult> bulkMapResults =
            TextureExporter::ExportTextureMaps(
                packPath,
                entry,
                { TextureMapKind::Roughness, TextureMapKind::Metallic },
                mapOptions);
        Expect(
            bulkMapResults.size() == 2 &&
                bulkMapResults[0].success &&
                bulkMapResults[1].success,
            "Bulk material-map export did not return two successful outputs.");
        const std::array<uint8_t, 4> bulkRoughnessPixel =
            ReadFirstPngPixelRgba(bulkMapResults[0].outputPath);
        const std::array<uint8_t, 4> bulkMetallicPixel =
            ReadFirstPngPixelRgba(bulkMapResults[1].outputPath);
        Expect(
            bulkRoughnessPixel[0] == 127 &&
                bulkRoughnessPixel[1] == 127 &&
                bulkRoughnessPixel[2] == 127,
            "Bulk roughness export did not decode the modern non-linear inverted-roughness channel.");
        Expect(
            bulkMetallicPixel[0] == 192 &&
                bulkMetallicPixel[1] == 192 &&
                bulkMetallicPixel[2] == 192,
            "Bulk metallic export did not preserve the packed metallic channel.");

        const TextureExportResult aoResult = TextureExporter::ExportTextureMap(
            aoPackPath,
            aoEntry,
            TextureMapKind::AmbientOcclusion,
            mapOptions);
        Expect(aoResult.success, "AO map export failed: " + aoResult.error);

        const std::filesystem::path ormPath = options.outputDir / "test_normal_n_orm.png";
        std::string composeError;
        Expect(
            TextureExporter::ComposeOrmTexture(
                generatedOrmPath,
                aoResult.outputPath,
                ormPath,
                composeError),
            "ORM composition failed: " + composeError);
        const std::array<uint8_t, 4> ormPixel = ReadFirstPngPixelRgba(ormPath);
        Expect(
            ormPixel[0] == 42 && ormPixel[1] == 127 &&
                ormPixel[2] == 192 && ormPixel[3] == 255,
            "Composed ORM channels are incorrect.");

        std::vector<uint8_t> rmaPayload(16 * 4);
        for (size_t i = 0; i < 16; ++i)
        {
            const size_t idx = i * 4;
            rmaPayload[idx + 0] = 10; // R: roughness
            rmaPayload[idx + 1] = 20; // G: metallic
            rmaPayload[idx + 2] = 30; // B: AO
            rmaPayload[idx + 3] = 255;
        }

        const std::filesystem::path rmaPackPath = tempRoot / "rma_validation.pack";
        WriteFileBytes(rmaPackPath, rmaPayload);
        SubAssetRecord rmaEntry = entry;
        rmaEntry.name = "test_rma";
        rmaEntry.size = static_cast<uint32_t>(rmaPayload.size());
        const TextureExportResult rmaResult =
            TextureExporter::ExportTexture(rmaPackPath, rmaEntry, options);
        Expect(rmaResult.success, "RMA normalization failed: " + rmaResult.error);
        Expect(
            FindMapOutput(rmaResult, TextureMapKind::OcclusionRoughnessMetallic) ==
                rmaResult.outputPath,
            "Normalized RMA output was not tracked as ORM.");
        const std::array<uint8_t, 4> normalizedRmaPixel =
            ReadFirstPngPixelRgba(rmaResult.outputPath);
        Expect(
            normalizedRmaPixel[0] == 30 &&
                normalizedRmaPixel[1] == 10 &&
                normalizedRmaPixel[2] == 20,
            "RMA texture was not reordered to standard ORM.");

        SubAssetRecord armEntry = rmaEntry;
        armEntry.name = "test_arm";
        const TextureExportResult armResult =
            TextureExporter::ExportTexture(rmaPackPath, armEntry, options);
        Expect(armResult.success, "ARM normalization failed: " + armResult.error);
        const std::array<uint8_t, 4> normalizedArmPixel =
            ReadFirstPngPixelRgba(armResult.outputPath);
        Expect(
            normalizedArmPixel[0] == 10 &&
                normalizedArmPixel[1] == 20 &&
                normalizedArmPixel[2] == 30,
            "ARM texture did not preserve standard AO/roughness/metallic channel order.");

        SubAssetRecord mraoEntry = rmaEntry;
        mraoEntry.name = "test_mrao";
        const TextureExportResult mraoResult =
            TextureExporter::ExportTexture(rmaPackPath, mraoEntry, options);
        Expect(mraoResult.success, "MRAO normalization failed: " + mraoResult.error);
        const std::array<uint8_t, 4> normalizedMraoPixel =
            ReadFirstPngPixelRgba(mraoResult.outputPath);
        Expect(
            normalizedMraoPixel[0] == 30 &&
                normalizedMraoPixel[1] == 20 &&
                normalizedMraoPixel[2] == 10,
            "MRAO texture was not reordered to standard ORM.");
    }

    void RunTextureQualityValidation()
    {
        using DagorAssetsExtractor::AssetIndex::ClassifyTextureQuality;
        using DagorAssetsExtractor::AssetIndex::TextureQualityPriority;
        using DagorAssetsExtractor::AssetIndex::TextureQualityTier;

        const std::filesystem::path standardPack = "content/base/res/aircraft.dxp.bin";
        const std::filesystem::path hqPack = "content.hq/base/res/aircraft_hq.dxp.bin";
        const std::filesystem::path uhqPack = "content/uhq_vehicles/aircraft_uhq.dxp.bin";

        Expect(
            TextureQualityPriority(standardPack, "aircraft_c$uhq") >
                TextureQualityPriority(standardPack, "aircraft_c$hq") &&
            TextureQualityPriority(standardPack, "aircraft_c$hq") >
                TextureQualityPriority(standardPack, "aircraft_c") &&
            TextureQualityPriority(standardPack, "aircraft_c") >
                TextureQualityPriority(standardPack, "aircraft_c$tq") &&
            TextureQualityPriority(standardPack, "aircraft_c$tq") >
                TextureQualityPriority(standardPack, "aircraft_c$lq"),
            "Texture quality order must be $uhq > $hq > untagged > $tq > $lq.");
        Expect(
            ClassifyTextureQuality(hqPack, "aircraft_c") == TextureQualityTier::High,
            "Untagged content.hq texture was not classified as high quality.");
        Expect(
            TextureQualityPriority(hqPack, "aircraft_c$hq*") >
                TextureQualityPriority(hqPack, "aircraft_c*?na16"),
            "An explicit $hq texture must outrank an untagged qualified texture in content.hq.");
        Expect(
            ClassifyTextureQuality(uhqPack, "aircraft_c") ==
                TextureQualityTier::UltraHigh,
            "Untagged UHQ-pack texture was not classified as ultra-high quality.");
        Expect(
            ClassifyTextureQuality(hqPack, "aircraft_c$lq") == TextureQualityTier::Low,
            "Explicit low-quality tags must override high-quality pack paths.");
        Expect(
            ClassifyTextureQuality(standardPack, "aircraft_c$UHQ") ==
                TextureQualityTier::UltraHigh,
            "Texture quality tags must be case-insensitive.");
    }

    void RunVromfsValidation()
    {
        std::vector<uint8_t> file(16 + 0x90, 0);
        std::memcpy(file.data(), "VRFs", 4);
        file[6] = 'P';
        file[7] = 'C';
        WriteLe<uint32_t>(file, 0x08, 0u);
        WriteLe<uint32_t>(file, 0x0C, 0x80000000u);

        const size_t body = 16;
        WriteLe<uint32_t>(file, body + 0x00, 0x20u);
        WriteLe<uint32_t>(file, body + 0x04, 2u);
        WriteLe<uint32_t>(file, body + 0x10, 0x30u);

        WriteLe<uint32_t>(file, body + 0x20, 0x50u);

        const char* firstName = "alpha.bin";
        const char* secondName = "beta.bin";
        std::memcpy(file.data() + body + 0x50, firstName, std::strlen(firstName) + 1);
        std::memcpy(file.data() + body + 0x50 + std::strlen(firstName) + 1, secondName, std::strlen(secondName) + 1);

        WriteLe<uint32_t>(file, body + 0x30, 0x70u);
        WriteLe<uint32_t>(file, body + 0x34, 3u);
        WriteLe<uint32_t>(file, body + 0x40, 0x74u);
        WriteLe<uint32_t>(file, body + 0x44, 4u);

        file[body + 0x70] = 'a';
        file[body + 0x71] = 'b';
        file[body + 0x72] = 'c';
        file[body + 0x74] = 'd';
        file[body + 0x75] = 'e';
        file[body + 0x76] = 'f';
        file[body + 0x77] = 'g';

        DagorFormats::VromfsReader reader(file);
        reader.Parse();

        const auto& files = reader.GetFiles();
        Expect(files.size() == 2, "VROMFS validation expected two files.");
        Expect(files[0].name == "alpha.bin", "VROMFS validation parsed the wrong first filename.");
        Expect(files[1].name == "beta.bin", "VROMFS validation parsed the wrong second filename.");

        const auto firstData = reader.GetFileData(files[0]);
        const auto secondData = reader.GetFileData(files[1]);
        Expect(firstData.size() == 3 && firstData[0] == 'a' && firstData[1] == 'b' && firstData[2] == 'c',
            "VROMFS validation parsed the wrong first file payload.");
        Expect(secondData.size() == 4 && secondData[0] == 'd' && secondData[1] == 'e' &&
               secondData[2] == 'f' && secondData[3] == 'g',
            "VROMFS validation parsed the wrong second file payload.");
        Expect(!reader.GetDiagnostics().empty(), "VROMFS validation expected parser diagnostics.");
    }

    void RunJobSystemValidation()
    {
        const size_t computedWorkerCount =
            DagorAssetsExtractor::Jobs::JobSystem::ComputeWorkerCount(64, 0);
        const size_t hardwareWorkerCount = std::max<size_t>(1, std::thread::hardware_concurrency());
        Expect(computedWorkerCount >= 1, "JobSystem computed an invalid worker count.");
        Expect(computedWorkerCount <= hardwareWorkerCount, "JobSystem exceeded hardware concurrency.");

        DagorAssetsExtractor::Jobs::JobSystem jobs(2);
        std::atomic_int activeWorkers{ 0 };
        std::atomic_int maxActiveWorkers{ 0 };

        for (int i = 0; i < 6; ++i)
        {
            jobs.Start("ValidationJob_" + std::to_string(i), [&]() {
                const int active = activeWorkers.fetch_add(1) + 1;
                int observed = maxActiveWorkers.load();
                while (observed < active &&
                       !maxActiveWorkers.compare_exchange_weak(observed, active))
                {
                }

                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                activeWorkers.fetch_sub(1);
            });
        }

        jobs.JoinAll();
        Expect(maxActiveWorkers.load() <= 2, "JobSystem exceeded the configured worker cap.");
        Expect(maxActiveWorkers.load() >= 2, "JobSystem did not execute jobs concurrently.");
        Expect(jobs.RunningCount() == 0, "JobSystem still reports running jobs after JoinAll.");
    }

    void RunDynModelRigidSkinValidation(const std::filesystem::path& tempRoot)
    {
        ExportManifest manifest;
        manifest.skipWrite = true;

        const std::filesystem::path glbPath = tempRoot / "dynmodel_rigid_skin.glb";
        const std::vector<DagorFormats::GeomNodeTreeNode> skeleton = {
            MakeSkeletonNode("hardpoint", 3.0f, 4.0f, 5.0f)
        };
        const std::vector<DagorFormats::DynModelMesh> lods = {
            MakeRigidDynMesh("rigid_piece", "hardpoint")
        };

        Expect(
            ModelExporter::ExportToGlb(glbPath, lods, skeleton, manifest),
            "DynModel rigid-skin validation export failed.");

        VerifyGlbBufferViewsAndAccessors(glbPath);

        const std::string json = ExtractGlbJson(glbPath);
        Expect(json.find("\"matrix\"") == std::string::npos,
            "Rigid DynModel skeleton should be exported as TRS, not raw node matrices.");
        Expect(json.find("\"scale\": [-") == std::string::npos,
            "Rigid DynModel skeleton should not use negative scale to repair handedness.");
        Expect(json.find("\"translation\": [3, 4, -5]") != std::string::npos,
            "Rigid DynModel skeleton TRS did not preserve converted bind-pose translation.");
        Expect(json.find("\"skins\"") != std::string::npos, "Rigid DynModel export did not emit a skin.");
        Expect(
            json.find("\"name\": \"rigid_piece\",\n      \"mesh\": 0,\n      \"skin\": 0\n    }") != std::string::npos,
            "Rigid DynModel mesh node should be skinned without its own transform matrix.");
        Expect(json.find("\"hardpoint__bind\"") == std::string::npos,
            "Rigid DynModel export should not emit bind-wrapper nodes without animations.");
        ExpectRigidDynTriangleGeometry(glbPath, 3.0f, 4.0f, -5.0f);
    }

    void RunDynModelRigidPlacementWithoutSkeletonValidation(const std::filesystem::path& tempRoot)
    {
        ExportManifest manifest;
        manifest.skipWrite = true;

        const std::filesystem::path glbPath = tempRoot / "dynmodel_rigid_no_skeleton.glb";
        const std::vector<DagorFormats::GeomNodeTreeNode> skeleton = {
            MakeSkeletonNode("hardpoint_no_skeleton", 6.0f, 7.0f, 8.0f)
        };
        const std::vector<DagorFormats::DynModelMesh> lods = {
            MakeRigidDynMesh("rigid_piece_no_skeleton", "hardpoint_no_skeleton")
        };

        Expect(
            ModelExporter::ExportToGlb(glbPath, lods, skeleton, manifest, {}, false),
            "DynModel rigid no-skeleton export failed.");

        VerifyGlbBufferViewsAndAccessors(glbPath);

        const std::string json = ExtractGlbJson(glbPath);
        Expect(json.find("\"skin\"") == std::string::npos,
            "Rigid DynModel export without skeleton should not emit a skin.");
        Expect(json.find("\"matrix\"") == std::string::npos,
            "Rigid DynModel export without skeleton should bake placement instead of emitting node matrices.");
        ExpectRigidDynTriangleGeometry(glbPath, 6.0f, 7.0f, -8.0f);
    }

    void RunDynModelBindRelativeAnimationValidation(const std::filesystem::path& tempRoot)
    {
        ExportManifest manifest;
        manifest.skipWrite = true;

        std::vector<DagorFormats::GeomNodeTreeNode> skeleton;
        skeleton.reserve(6);
        for (int i = 0; i < 6; ++i)
        {
            skeleton.push_back(MakeSkeletonNode(
                "bone" + std::to_string(i),
                1.0f + static_cast<float>(i),
                0.5f * static_cast<float>(i + 1),
                0.0f));
        }

        AnimationNodeTrack animatedTrack;
        animatedTrack.nodeName = "bone0";
        animatedTrack.times = { 0.0f, 1.0f };
        animatedTrack.translations = {
            { 0.0f, 0.0f, 0.0f },
            { 0.25f, 0.0f, 0.0f }
        };

        AnimationClip clip;
        clip.name = "bind_relative";
        clip.nodeTracks.push_back(animatedTrack);
        for (int i = 1; i < 6; ++i)
        {
            AnimationNodeTrack idleTrack;
            idleTrack.nodeName = "bone" + std::to_string(i);
            idleTrack.times = { 0.0f, 1.0f };
            idleTrack.translations = {
                { 0.0f, 0.0f, 0.0f },
                { 0.0f, 0.0f, 0.0f }
            };
            clip.nodeTracks.push_back(std::move(idleTrack));
        }

        const std::filesystem::path glbPath = tempRoot / "dynmodel_bind_relative.glb";
        Expect(
            ModelExporter::ExportToGlb(
                glbPath,
                { MakeRigidDynMesh("rigid_piece_anim", "bone0") },
                skeleton,
                manifest,
                {},
                true,
                GlbExportOptions{},
                { clip }),
            "DynModel bind-relative animation export failed.");

        VerifyGlbBufferViewsAndAccessors(glbPath);

        const std::string json = ExtractGlbJson(glbPath);
        Expect(json.find("\"animations\"") != std::string::npos,
            "Bind-relative DynModel export did not emit animation data.");
        Expect(json.find("\"bone0__bind\"") != std::string::npos,
            "Bind-relative DynModel export did not emit bind-wrapper nodes.");
        Expect(json.find("skeleton_dummy") == std::string::npos,
            "Bind-relative DynModel export should not add the legacy skeleton dummy mesh.");
        Expect(
            json.find("\"name\": \"rigid_piece_anim\",\n      \"mesh\": 0,\n      \"skin\": 0\n    }") != std::string::npos,
            "Bind-relative rigid DynModel mesh node should stay skinned without an object transform.");
        ExpectRigidDynTriangleGeometry(glbPath, 1.0f, 0.5f, 0.0f);
    }

    void RunDynModelAbsoluteAnimationValidation(const std::filesystem::path& tempRoot)
    {
        ExportManifest manifest;
        manifest.skipWrite = true;

        const std::vector<DagorFormats::GeomNodeTreeNode> skeleton = {
            MakeSkeletonNode("bone_absolute_0", 2.0f, 0.0f, 0.0f),
            MakeSkeletonNode("bone_absolute_1", 4.0f, 1.0f, 0.0f)
        };

        AnimationClip clip;
        clip.name = "absolute_local";

        AnimationNodeTrack track0;
        track0.nodeName = "bone_absolute_0";
        track0.times = { 0.0f, 1.0f };
        track0.translations = {
            { 2.0f, 0.0f, 0.0f },
            { 2.5f, 0.0f, 0.0f }
        };
        clip.nodeTracks.push_back(track0);

        AnimationNodeTrack track1;
        track1.nodeName = "bone_absolute_1";
        track1.times = { 0.0f, 1.0f };
        track1.translations = {
            { 4.0f, 1.0f, 0.0f },
            { 4.5f, 1.0f, 0.0f }
        };
        clip.nodeTracks.push_back(track1);

        const std::filesystem::path glbPath = tempRoot / "dynmodel_absolute.glb";
        Expect(
            ModelExporter::ExportToGlb(
                glbPath,
                { MakeRigidDynMesh("rigid_piece_absolute", "bone_absolute_0") },
                skeleton,
                manifest,
                {},
                true,
                GlbExportOptions{},
                { clip }),
            "DynModel absolute animation export failed.");

        VerifyGlbBufferViewsAndAccessors(glbPath);

        const std::string json = ExtractGlbJson(glbPath);
        Expect(json.find("\"animations\"") != std::string::npos,
            "Absolute DynModel export did not emit animation data.");
        Expect(json.find("__bind") == std::string::npos,
            "Absolute DynModel export should not emit bind-wrapper nodes.");
        ExpectRigidDynTriangleGeometry(glbPath, 2.0f, 0.0f, 0.0f);
    }

    void RunDynModelDirectTranslationPlaceholderValidation(const std::filesystem::path& tempRoot)
    {
        ExportManifest manifest;
        manifest.skipWrite = true;

        const std::vector<DagorFormats::GeomNodeTreeNode> skeleton = {
            MakeSkeletonNode("moving_gear", 2.0f, 0.0f, 0.0f)
        };

        AnimationClip clip;
        clip.name = "direct_translation";

        AnimationNodeTrack track;
        track.nodeName = "moving_gear";
        track.times = { 0.0f, 1.0f };
        track.translations = {
            { 0.0f, 0.0f, 0.0f },
            { 4.0f, 1.0f, 0.0f }
        };
        clip.nodeTracks.push_back(track);

        const std::filesystem::path glbPath = tempRoot / "dynmodel_direct_translation.glb";
        Expect(
            ModelExporter::ExportToGlb(
                glbPath,
                { MakeRigidDynMesh("direct_translation_mesh", std::string()) },
                skeleton,
                manifest,
                {},
                true,
                GlbExportOptions{ .bypassAnimationRebase = false },
                { clip }),
            "DynModel direct translation placeholder export failed.");

        VerifyGlbBufferViewsAndAccessors(glbPath);

        const std::string json = ExtractGlbJson(glbPath);
        Expect(json.find("__bind") == std::string::npos,
            "Direct translation export should not emit bind-wrapper nodes.");

        const std::array<float, 3> firstTranslation =
            ReadAnimationVec3ForPath(glbPath, "translation", 0);
        ExpectVec3Near(
            firstTranslation,
            { 2.0f, 0.0f, 0.0f },
            "Zero translation placeholder should inherit the rest translation.");

        const std::array<float, 3> movedTranslation =
            ReadAnimationVec3ForPath(glbPath, "translation", 1);
        ExpectVec3Near(
            movedTranslation,
            { 4.0f, 1.0f, 0.0f },
            "Direct animation translation should not be offset by the rest pose.");
    }

    void RunDynModelActionFirstFrameRestPoseValidation(const std::filesystem::path& tempRoot)
    {
        ExportManifest manifest;
        manifest.skipWrite = true;

        const std::vector<DagorFormats::GeomNodeTreeNode> skeleton = {
            MakeZRotatedSkeletonNode("rotated_action_bone")
        };

        AnimationClip clip;
        clip.name = "rest_frame_rotation";

        AnimationNodeTrack track;
        track.nodeName = "rotated_action_bone";
        track.times = { 0.5f, 1.0f };
        track.rotations = {
            { 0.0f, 0.0f, 0.0f, 1.0f },
            { 0.38268343f, 0.0f, 0.0f, 0.9238795f }
        };
        clip.nodeTracks.push_back(track);

        const std::filesystem::path glbPath = tempRoot / "dynmodel_action_rest_frame.glb";
        Expect(
            ModelExporter::ExportToGlb(
                glbPath,
                { MakeRigidDynMesh("action_rest_mesh", std::string()) },
                skeleton,
                manifest,
                {},
                true,
                GlbExportOptions{ .bypassAnimationRebase = false },
                { clip }),
            "DynModel action first-frame validation export failed.");

        VerifyGlbBufferViewsAndAccessors(glbPath);

        const std::array<float, 4> firstRotation =
            ReadFirstAnimationVec4ForPath(glbPath, "rotation");
        const float xyzMagnitude =
            std::fabs(firstRotation[0]) +
            std::fabs(firstRotation[1]) +
            std::fabs(firstRotation[2]);
        const float length =
            std::sqrt(
                (firstRotation[0] * firstRotation[0]) +
                (firstRotation[1] * firstRotation[1]) +
                (firstRotation[2] * firstRotation[2]) +
                (firstRotation[3] * firstRotation[3]));
        Expect(xyzMagnitude > 0.1f,
            "Action first rotation sample should be restored to the non-identity rest pose.");
        ExpectNear(length, 1.0f, "Action first rotation sample should remain normalized.");

        const std::array<float, 4> lastRotation =
            ReadAnimationVec4ForPath(glbPath, "rotation", 2);
        const std::array<float, 4> convertedLocalDelta =
            { -0.38268343f, 0.0f, 0.0f, 0.9238795f };
        const std::array<float, 4> expectedLastRotation =
            MultiplyQuaternionsForTest(firstRotation, convertedLocalDelta);
        ExpectQuatNear(
            lastRotation,
            expectedLastRotation,
            "Identity-start action rotation samples should be rebased onto the rest pose in local node space.");

        const std::string json = ExtractGlbJson(glbPath);
        Expect(json.find("\"scale\": [-") == std::string::npos,
            "Action export should not use negative scale to repair handedness.");
    }

    void RunDynModelParentScalingAndMirroringValidation(const std::filesystem::path& tempRoot)
    {
        ExportManifest manifest;
        manifest.skipWrite = true;

        // Parent has scale sx=2.0, sy=2.0, sz=-2.0 (Z-mirrored and 2x scaled)
        // Child has local translation lx=3.0, ly=1.0, lz=5.0 relative to parent.
        const std::vector<DagorFormats::GeomNodeTreeNode> skeleton = {
            MakeScaledSkeletonNode("scaled_parent", 0.0f, 0.0f, 0.0f, 2.0f, 2.0f, -2.0f),
            MakeChildSkeletonNodeScaledParent("scaled_child", 0, 0.0f, 0.0f, 0.0f, 2.0f, 2.0f, -2.0f, 3.0f, 1.0f, 5.0f)
        };

        AnimationClip clip;
        clip.name = "parent_scaled_anim";

        // We animate the child:
        // Raw local translation: (1.0, 2.0, 3.0)
        // Raw local rotation: (0.1, 0.2, 0.3, 0.9) - needs normalization.
        // Since parent is mirrored, the rotation converted for glTF should NOT negate X and Y components.
        AnimationNodeTrack track;
        track.nodeName = "scaled_child";
        track.times = { 0.0f, 1.0f };
        track.translations = {
            { 1.0f, 2.0f, 3.0f },
            { 1.0f, 2.0f, 3.0f }
        };
        track.rotations = {
            { 0.1f, 0.2f, 0.3f, 0.9f },
            { 0.1f, 0.2f, 0.3f, 0.9f }
        };
        clip.nodeTracks.push_back(track);

        const std::filesystem::path glbPath = tempRoot / "dynmodel_parent_scaling_mirroring.glb";
        Expect(
            ModelExporter::ExportToGlb(
                glbPath,
                { MakeRigidDynMesh("piece", "scaled_child") },
                skeleton,
                manifest,
                {},
                true,
                GlbExportOptions{ .bypassAnimationRebase = false },
                { clip }),
            "DynModel parent scaling/mirroring validation export failed.");

        VerifyGlbBufferViewsAndAccessors(glbPath);

        // Read exported translation
        const std::array<float, 3> firstTranslation =
            ReadAnimationVec3ForPath(glbPath, "translation", 0);
        
        // Raw translation is (1.0, 2.0, 3.0)
        // Converted for glTF (Z negated) -> (1.0, 2.0, -3.0)
        // Parent scale is (2.0, 2.0, -2.0)
        // Scaled translation -> (2.0, 4.0, 6.0)
        ExpectVec3Near(
            firstTranslation,
            { 2.0f, 4.0f, 6.0f },
            "Child animation translation should be scaled by parent real scale.");

        // Read exported rotation
        const std::array<float, 4> firstRotation =
            ReadAnimationVec4ForPath(glbPath, "rotation", 0);

        // Since parent is mirrored, rotation conversion should bypass Z-reflection.
        // So raw rotation (0.1, 0.2, 0.3, 0.9) is only normalized, components are NOT negated!
        // Normalized (0.1, 0.2, 0.3, 0.9) / sqrt(0.01 + 0.04 + 0.09 + 0.81) = (0.1, 0.2, 0.3, 0.9) / sqrt(0.95)
        float len = std::sqrt(0.95f);
        std::array<float, 4> expectedRotation = { 0.1f / len, 0.2f / len, 0.3f / len, 0.9f / len };
        ExpectQuatNear(
            firstRotation,
            expectedRotation,
            "Child animation rotation should bypass Z-reflection when parent is mirrored.");
    }
}

int main()
{
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(comHr);
    try
    {
        const std::filesystem::path tempRoot = MakeTempRoot();
        RunStep2Validation(tempRoot);
        RunTexturePreviewValidation(tempRoot);
        RunStep3ResolverValidation();
        RunSettingsPersistenceValidation(tempRoot);
        RunTextureQualityValidation();
        RunStep3GlbValidation(tempRoot);
        RunStep3PackedNormalMapValidation(tempRoot);
        RunVromfsValidation();
        RunJobSystemValidation();
        RunDynModelRigidSkinValidation(tempRoot);
        RunDynModelRigidPlacementWithoutSkeletonValidation(tempRoot);
        RunDynModelBindRelativeAnimationValidation(tempRoot);
        RunDynModelAbsoluteAnimationValidation(tempRoot);
        RunDynModelDirectTranslationPlaceholderValidation(tempRoot);
        RunDynModelActionFirstFrameRestPoseValidation(tempRoot);
        RunDynModelParentScalingAndMirroringValidation(tempRoot);
        std::cout << "Validation succeeded: " << tempRoot.string() << std::endl;

        // Performance test with all records
        {
            std::cout << "\n=== GRP Performance Test (All Records) ===" << std::endl;
            const std::filesystem::path cachePath = "C:\\Users\\AldertLake\\Documents\\Random Projects\\Dagor_Assets_Extractor\\build\\vs2026\\bin\\windows\\x86_64\\Debug\\cache\\index-cache";
            const std::filesystem::path gameRoot = "D:\\SteamLibrary\\steamapps\\common\\War Thunder";
            const std::filesystem::path grpPath = "D:\\SteamLibrary\\steamapps\\common\\War Thunder\\content\\base\\res\\heliaircrafts.grp";
            
            if (std::filesystem::exists(cachePath) && std::filesystem::exists(grpPath))
            {
                auto loadResult = DagorAssetsExtractor::AssetIndex::LoadIndexCacheRecords(cachePath, gameRoot);
                std::cout << "Loaded " << loadResult.records.size() << " records from cache." << std::endl;

                if (!loadResult.records.empty())
                {
                    DagorAssetsExtractor::AssetIndex::DescriptorResourceLinkBuildOptions options;
                    options.focusPath = grpPath;
                    options.progress = [](const DagorAssetsExtractor::AssetIndex::DescriptorResourceLinkProgress& progress) {
                        std::cout << "Progress: [" << progress.stage << "] " << progress.item 
                                  << " (" << progress.current << "/" << progress.total << ")" << std::endl;
                        return true;
                    };

                    auto start = std::chrono::high_resolution_clock::now();
                    auto result = DagorAssetsExtractor::AssetIndex::BuildDescriptorResourceLinks(loadResult.records, options);
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::high_resolution_clock::now() - start).count();

                    std::cout << "Build status: " << (result.canceled ? "CANCELED" : "SUCCESS") << std::endl;
                    std::cout << "Warnings: " << result.warnings.size() << std::endl;
                    std::cout << "Elapsed: " << elapsed << " ms" << std::endl;
                }
            }
            else
            {
                std::cout << "Cache or GRP path not found." << std::endl;
            }
            std::cout << "==========================================\n" << std::endl;
        }

        if (uninitializeCom) CoUninitialize();
        return 0;
    }
    catch (const std::exception& ex)
    {
        if (uninitializeCom) CoUninitialize();
        std::cerr << "Validation failed: " << ex.what() << std::endl;
        return 1;
    }
}
