#pragma once

#include "DagorFormats/RendInstReader.h"

#include <string>
#include <string_view>
#include <vector>

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

    enum class TextureContentLayout
    {
        Unknown = 0,
        BaseColor,
        ConventionalNormal,
        WarThunderPackedNormalPbr,
        AmbientOcclusion,
        PackedOrm,
        PackedRma,
        PackedMra,
    };

    enum class TextureMapKind
    {
        BaseColor = 0,
        Normal,
        AmbientOcclusion,
        Smoothness,
        Roughness,
        Metallic,
        MetallicRoughness,
        OcclusionRoughnessMetallic,
    };

    enum class TextureDetectionConfidence
    {
        None = 0,
        NameBased,
        StrongNameBased,
    };

    struct DetectedTextureMap
    {
        TextureMapKind kind = TextureMapKind::BaseColor;
        std::string label;
        std::string channels;
        bool derived = false;
    };

    struct TextureMapAnalysis
    {
        TextureContentLayout layout = TextureContentLayout::Unknown;
        TextureDetectionConfidence confidence = TextureDetectionConfidence::None;
        std::string summary;
        std::string caution;
        std::vector<DetectedTextureMap> maps;
    };

    struct ResolvedMaterialTextures
    {
        std::string baseColorTextureName;
        std::string normalTextureName;
        std::string metallicRoughnessTextureName;
        std::string occlusionTextureName;
        std::string packedNormalPbrTextureName;
    };

    ResolvedMaterialTextures ResolveMaterialTextures(const DagorFormats::InternalMaterial& material);

    MaterialTextureSemantic ClassifyDescriptorBinding(
        std::string_view shaderClass,
        std::string_view slotName,
        std::string_view textureName);

    std::string NormalizeTextureSemanticName(std::string_view value);
    TextureMapAnalysis AnalyzeTextureMaps(std::string_view textureName);
    std::string_view TextureMapKindName(TextureMapKind kind);
    std::string_view TextureMapKindFileSuffix(TextureMapKind kind);
    bool LooksLikePackedOrm(std::string_view textureName);
    bool LooksLikePackedNormalMap(std::string_view textureName);
}
