#pragma once

#include "DagorFormats/RendInstReader.h"
#include "DagorFormats/DynModelReader.h"
#include "DagorFormats/GeomNodeTreeReader.h"
#include "Export/ExportManifest.h"
#include <filesystem>
#include <array>
#include <vector>
#include <string>
#include <optional>

namespace DagorAssetsExtractor::Export
{
    struct MaterialInfo
    {
        std::string name;
        std::string diffuseTexturePath; // Path relative to GLB or empty
        std::filesystem::path diffuseTextureSourcePath; // Exported image file to embed into GLB when requested
    };

    struct GlbExportOptions
    {
        bool embedTexturesInGlb = true;
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

        // Export a single LOD to OBJ format
        static bool ExportToObj(
            const std::filesystem::path& path,
            const DagorFormats::RendInstMesh& mesh,
            const ExportManifest& manifest);
    };
}
