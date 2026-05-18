#pragma once

#include "DagorFormats/RendInstReader.h"
#include "Export/ExportManifest.h"
#include <filesystem>
#include <vector>
#include <string>

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
        // Export all LODs into a single GLB file
        static bool ExportToGlb(
            const std::filesystem::path& path,
            const std::vector<DagorFormats::RendInstMesh>& lods,
            const ExportManifest& manifest,
            const std::vector<MaterialInfo>& materials = {});

        // Export a single LOD to OBJ format
        static bool ExportToObj(
            const std::filesystem::path& path,
            const DagorFormats::RendInstMesh& mesh,
            const ExportManifest& manifest);
    };
}
