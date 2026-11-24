#pragma once

#include "DagorFormats/RendInstReader.h"
#include "DagorFormats/DynModelReader.h"
#include "DagorFormats/GeomNodeTreeReader.h"
#include "Export/ExportManifest.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace DagorAssetsExtractor::Export
{
    struct MaterialInfo
    {
        std::string name;
        std::string diffuseTexturePath; // Path relative to GLB or empty
        std::filesystem::path diffuseTextureSourcePath; // Exported image file to embed into GLB when requested
        std::shared_ptr<const std::vector<uint8_t>> diffuseTextureEmbeddedBytes; // Optional in-memory PNG/JPEG bytes
        std::string normalTexturePath; // Path relative to GLB or empty
        std::filesystem::path normalTextureSourcePath; // Exported normal map image to embed
        std::shared_ptr<const std::vector<uint8_t>> normalTextureEmbeddedBytes;
        std::string metallicRoughnessTexturePath; // Path relative to GLB or empty
        std::filesystem::path metallicRoughnessTextureSourcePath; // Exported metallic/roughness texture to embed
        std::shared_ptr<const std::vector<uint8_t>> metallicRoughnessTextureEmbeddedBytes;
        std::string occlusionTexturePath; // Path relative to GLB or empty
        std::filesystem::path occlusionTextureSourcePath; // Exported occlusion texture to embed
        std::shared_ptr<const std::vector<uint8_t>> occlusionTextureEmbeddedBytes;
        std::string nativePackedNormalPbrTexturePath; // Native War Thunder _n image for custom extension metadata
        std::filesystem::path nativePackedNormalPbrTextureSourcePath; // Native packed image to embed
        std::shared_ptr<const std::vector<uint8_t>> nativePackedNormalPbrTextureEmbeddedBytes;
    };

    struct GlbExportOptions
    {
        bool embedTexturesInGlb = true;
        bool bypassAnimationRebase = false;
        std::function<void(const std::string&)> diagnosticLog;
    };

    struct AnimationNodeTrack
    {
        std::string nodeName;
        std::vector<float> times;
        std::vector<std::array<float, 3>> translations;
        std::vector<std::array<float, 4>> rotations;
        std::vector<std::array<float, 3>> scales;
    };

    struct AnimationClip
    {
        std::string name;
        bool additive = false;
        std::vector<AnimationNodeTrack> nodeTracks;
        std::vector<std::string> diagnostics;
    };

    class ModelExporter
    {
    public:
        // Export all LODs into a single GLB file (Static)
        static bool ExportToGlb(
            const std::filesystem::path& path,
            const std::vector<DagorFormats::RendInstMesh>& lods,
            const ExportManifest& manifest,
            const std::vector<MaterialInfo>& materials = {},
            const GlbExportOptions& options = GlbExportOptions{});

        // Export all LODs into a single GLB file (Skinned)
        static bool ExportToGlb(
            const std::filesystem::path& path,
            const std::vector<DagorFormats::DynModelMesh>& lods,
            const std::vector<DagorFormats::GeomNodeTreeNode>& skeleton,
            const ExportManifest& manifest,
            const std::vector<MaterialInfo>& materials = {},
            bool includeSkeletonRig = true,
            const GlbExportOptions& options = GlbExportOptions{},
            const std::vector<AnimationClip>& animations = {});
    };
}
