#pragma once

#include "DagorFormats/RendInstReader.h"

#include <string>
#include <string_view>

namespace DagorAssetsExtractor::Export
{
    enum class MaterialTextureSemantic
    {
        Unknown = 0,
        BaseColor,
        Normal,
        MetallicRoughness,
        Occlusion,
    };

    struct ResolvedMaterialTextures
    {
        std::string baseColorTextureName;
        std::string normalTextureName;
        std::string metallicRoughnessTextureName;
        std::string occlusionTextureName;
    };

    ResolvedMaterialTextures ResolveMaterialTextures(const DagorFormats::InternalMaterial& material);

    MaterialTextureSemantic ClassifyDescriptorBinding(
        std::string_view shaderClass,
        std::string_view slotName,
        std::string_view textureName);

    std::string NormalizeTextureSemanticName(std::string_view value);
    bool LooksLikePackedOrm(std::string_view textureName);
    bool LooksLikePackedNormalMap(std::string_view textureName);
}

