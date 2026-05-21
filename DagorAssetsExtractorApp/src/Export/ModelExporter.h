#pragma once

#include "DagorFormats/RendInstReader.h"
#include "DagorFormats/DynModelReader.h"
#include "DagorFormats/GeomNodeTreeReader.h"
#include "Export/ExportManifest.h"
#include <filesystem>
#include <vector>
#include <string>
#include <optional>

namespace DagorAssetsExtractor::Export
{
    struct MaterialInfo
    {
        std::string name;
        std::string diffuseTexturePath; // Path relative to GLB or empty
    };

    class ModelExporter
    {
    public:
        // Export all LODs into a single GLB file (Static)
        static bool ExportToGlb(
            const std::filesystem::path& path,
            const std::vector<DagorFormats::RendInstMesh>& lods,
            const ExportManifest& manifest,
            const std::vector<MaterialInfo>& materials = {});

        // Export all LODs into a single GLB file (Skinned)
        static bool ExportToGlb(
            const std::filesystem::path& path,
            const std::vector<DagorFormats::DynModelMesh>& lods,
            const std::vector<DagorFormats::GeomNodeTreeNode>& skeleton,
            const ExportManifest& manifest,
            const std::vector<MaterialInfo>& materials = {},
            bool includeSkeletonRig = true);

        // Export a single LOD to OBJ format
        static bool ExportToObj(
            const std::filesystem::path& path,
            const DagorFormats::RendInstMesh& mesh,
            const ExportManifest& manifest);
    };
}
