#include "AssetIndex/PackContents.h"
#include "DirectXTex.h"
#include "Export/MaterialTextureResolver.h"
#include "Export/ModelExporter.h"
#include "Export/TextureExporter.h"
#include "DagorFormats/VromfsReader.h"
#include "Jobs/JobSystem.h"

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

        DagorFormats::InternalMaterial ormMaterial;
        ormMaterial.shaderClass = "dynamic_vehicle";
        ormMaterial.textureSlots[1] = "tank_orm";
        const ResolvedMaterialTextures ormResolved = DagorAssetsExtractor::Export::ResolveMaterialTextures(ormMaterial);
        Expect(ormResolved.metallicRoughnessTextureName == "tank_orm", "Packed ORM texture was not recognized.");

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
        material.occlusionTexturePath = "ao.png";

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
        Expect(json.find("orm.png") != std::string::npos, "GLB JSON is missing the metallic-roughness image URI.");
        Expect(json.find("ao.png") != std::string::npos, "GLB JSON is missing the occlusion image URI.");
    }

    void RunVromfsValidation()
    {
        std::vector<uint8_t> file(16 + 0x90, 0);
        std::memcpy(file.data(), "VRFs", 4);
        file[6] = 'P';
        file[7] = 'C';
        WriteLe<uint32_t>(file, 0x08, 0u);
        WriteLe<uint32_t>(file, 0x0C, 0x81000000u);

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

        const std::string json = ExtractGlbJson(glbPath);
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

        const std::string json = ExtractGlbJson(glbPath);
        Expect(json.find("\"animations\"") != std::string::npos,
            "Absolute DynModel export did not emit animation data.");
        Expect(json.find("__bind") == std::string::npos,
            "Absolute DynModel export should not emit bind-wrapper nodes.");
        ExpectRigidDynTriangleGeometry(glbPath, 2.0f, 0.0f, 0.0f);
    }
}

int main()
{
    try
    {
        const std::filesystem::path tempRoot = MakeTempRoot();
        RunStep2Validation(tempRoot);
        RunStep3ResolverValidation();
        RunStep3GlbValidation(tempRoot);
        RunVromfsValidation();
        RunJobSystemValidation();
        RunDynModelRigidSkinValidation(tempRoot);
        RunDynModelRigidPlacementWithoutSkeletonValidation(tempRoot);
        RunDynModelBindRelativeAnimationValidation(tempRoot);
        RunDynModelAbsoluteAnimationValidation(tempRoot);
        std::cout << "Validation succeeded: " << tempRoot.string() << std::endl;
        return 0;
    }
    catch (const std::exception& ex)
    {
        std::cerr << "Validation failed: " << ex.what() << std::endl;
        return 1;
    }
}
