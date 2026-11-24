#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace DagorAssetsExtractor::Export
{
    struct ExportManifest
    {
        std::string sourcePack;
        std::string sourceResource;
        std::string assetType;
        std::vector<std::string> outputFiles;
        std::vector<std::string> textureDependencies;
        std::vector<std::string> skeletonDependencies;
        std::vector<std::string> decodeWarnings;
        std::vector<std::string> unsupportedFields;
        std::string appVersion;
        std::string formatDecoderVersion;
        bool skipWrite = false;

        bool WriteToFile(const std::filesystem::path& path) const;
    };
}
