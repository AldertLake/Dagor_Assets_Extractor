#include "Export/MaterialTextureResolver.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace DagorAssetsExtractor::Export
{
    namespace
    {
        std::string ToLowerLocal(std::string_view value)
        {
            std::string lowered(value);
            std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return lowered;
        }

        bool TryParseTextureSlotIndex(std::string_view slotName, size_t& slotIndex)
        {
            if (slotName.size() < 2 || slotName[0] != 't')
            {
                return false;
            }

            size_t value = 0;
            for (size_t i = 1; i < slotName.size(); ++i)
            {
                const unsigned char c = static_cast<unsigned char>(slotName[i]);
                if (std::isdigit(c) == 0)
                {
                    return false;
                }

                value = (value * 10) + static_cast<size_t>(slotName[i] - '0');
            }

            if (value >= 16)
            {
                return false;
            }

            slotIndex = value;
            return true;
        }


        std::vector<std::string_view> SplitTokens(std::string_view normalized)
        {
            std::vector<std::string_view> tokens;
            size_t start = 0;
            while (start < normalized.size())
            {
                const size_t end = normalized.find('_', start);
                const size_t length = (end == std::string_view::npos ? normalized.size() : end) - start;
                if (length > 0)
                {
                    tokens.push_back(normalized.substr(start, length));
                }

                if (end == std::string_view::npos)
                {
                    break;
                }

                start = end + 1;
            }

            return tokens;
        }

        bool HasToken(std::string_view normalized, std::string_view token)
        {
            const std::vector<std::string_view> tokens = SplitTokens(normalized);
            return std::find(tokens.begin(), tokens.end(), token) != tokens.end();
        }

        bool HasAnyToken(std::string_view normalized, std::initializer_list<std::string_view> candidates)
        {
            for (const std::string_view candidate : candidates)
            {
                if (HasToken(normalized, candidate))
                {
                    return true;
                }
            }

            return false;
        }

        std::vector<std::string_view> SemanticTokens(std::string_view textureName, std::string& storage)
        {
            storage = NormalizeTextureSemanticName(textureName);
            std::vector<std::string_view> tokens = SplitTokens(storage);
            while (!tokens.empty() &&
                   (tokens.back() == "hq" ||
                    tokens.back() == "uhq" ||
                    tokens.back() == "tq" ||
                    tokens.back() == "lq"))
            {
                tokens.pop_back();
            }
            return tokens;
        }

        std::string_view LastSemanticToken(std::string_view textureName, std::string& storage)
        {
            const std::vector<std::string_view> tokens = SemanticTokens(textureName, storage);
            return tokens.empty() ? std::string_view{} : tokens.back();
        }

        DetectedTextureMap MakeDetectedMap(
            TextureMapKind kind,
            std::string channels,
            bool derived = false)
        {
            DetectedTextureMap map;
            map.kind = kind;
            map.label = std::string(TextureMapKindName(kind));
            map.channels = std::move(channels);
            map.derived = derived;
            return map;
        }

        bool TextureNameLooksLikeOcclusion(std::string_view textureName)
        {
            const std::string normalized = NormalizeTextureSemanticName(textureName);
            return HasAnyToken(normalized, { "ao", "occ", "occlusion" });
        }

        bool TextureNameLooksLikeMetallicRoughness(std::string_view textureName)
        {
            return LooksLikePackedOrm(textureName);
        }

        std::vector<std::string> BuildBaseColorSlotPreference(std::string_view shaderClass)
        {
            const std::string lowered = ToLowerLocal(shaderClass);
            const bool layered = lowered.find("layered") != std::string::npos;
            const bool dynamic = lowered.find("dynamic") != std::string::npos;
            const bool detail1IsDiffuse = layered && lowered != "rendinst_simple_layered";

            if (dynamic)
            {
                if (layered)
                {
                    return { "t4", "t3", "t5", "t6", "t7", "t8", "t9", "t10", "t0" };
                }

                return { "t0", "t4", "t6", "t8", "t10" };
            }

            if (!detail1IsDiffuse || lowered == "dynamic_painted_by_mask")
            {
                return { "t0" };
            }

            return { "t3", "t5", "t7", "t9" };
        }

        std::vector<std::string> BuildNormalMapSlotPreference(std::string_view shaderClass)
        {
            const std::string lowered = ToLowerLocal(shaderClass);
            const bool layered = lowered.find("layered") != std::string::npos;
            const bool dynamic = lowered.find("dynamic") != std::string::npos;
            const bool detail1IsDiffuse = layered && lowered != "rendinst_simple_layered";

            if (dynamic)
            {
                if (layered)
                {
                    return { "t5", "t4", "t7", "t6", "t9", "t8", "t10", "t2" };
                }

                return { "t2", "t5", "t7", "t9" };
            }

            if (!detail1IsDiffuse || lowered == "dynamic_painted_by_mask")
            {
                return { "t2" };
            }

            return { "t4", "t6", "t8", "t10" };
        }

        std::vector<std::string> BuildOcclusionSlotPreference(std::string_view shaderClass)
        {
            const std::string lowered = ToLowerLocal(shaderClass);
            const bool layered = lowered.find("layered") != std::string::npos;
            const bool dynamic = lowered.find("dynamic") != std::string::npos;

            if (dynamic && !layered)
            {
                return { "t3" };
            }

            return {};
        }

        bool SlotIsInPreference(std::string_view slotName, const std::vector<std::string>& preference)
        {
            return std::find(preference.begin(), preference.end(), slotName) != preference.end();
        }

        std::string SelectTextureFromSlotPreference(
            const DagorFormats::InternalMaterial& material,
            const std::vector<std::string>& slotPreference)
        {
            for (const std::string& slot : slotPreference)
            {
                size_t slotIndex = 0;
                if (!TryParseTextureSlotIndex(slot, slotIndex) ||
                    material.textureSlots[slotIndex].empty())
                {
                    continue;
                }

                return material.textureSlots[slotIndex];
            }

            return {};
        }

        template <typename Predicate>
        std::string SelectTextureBySemanticName(
            const DagorFormats::InternalMaterial& material,
            const std::vector<std::string>& preferredSlots,
            Predicate&& predicate)
        {
            std::array<bool, 16> visited = {};

            for (const std::string& slot : preferredSlots)
            {
                size_t slotIndex = 0;
                if (!TryParseTextureSlotIndex(slot, slotIndex))
                {
                    continue;
                }

                visited[slotIndex] = true;
                const std::string& textureName = material.textureSlots[slotIndex];
                if (!textureName.empty() && predicate(textureName))
                {
                    return textureName;
                }
            }

            for (size_t slotIndex = 0; slotIndex < 16; ++slotIndex)
            {
                if (visited[slotIndex])
                {
                    continue;
                }

                const std::string& textureName = material.textureSlots[slotIndex];
                if (!textureName.empty() && predicate(textureName))
                {
                    return textureName;
                }
            }

            return {};
        }
    }

    ResolvedMaterialTextures ResolveMaterialTextures(const DagorFormats::InternalMaterial& material)
    {
        ResolvedMaterialTextures resolved;
        resolved.baseColorTextureName = SelectTextureFromSlotPreference(
            material,
            BuildBaseColorSlotPreference(material.shaderClass));
        resolved.normalTextureName = SelectTextureFromSlotPreference(
            material,
            BuildNormalMapSlotPreference(material.shaderClass));
        if (AnalyzeTextureMaps(resolved.normalTextureName).layout ==
            TextureContentLayout::WarThunderPackedNormalPbr)
        {
            resolved.packedNormalPbrTextureName = resolved.normalTextureName;
        }
        resolved.occlusionTextureName = SelectTextureFromSlotPreference(
            material,
            BuildOcclusionSlotPreference(material.shaderClass));

        if (resolved.occlusionTextureName.empty())
        {
            resolved.occlusionTextureName = SelectTextureBySemanticName(
                material,
                BuildOcclusionSlotPreference(material.shaderClass),
                TextureNameLooksLikeOcclusion);
        }

        resolved.metallicRoughnessTextureName = SelectTextureBySemanticName(
            material,
            { "t1" },
            TextureNameLooksLikeMetallicRoughness);

        if (resolved.occlusionTextureName.empty() && LooksLikePackedOrm(resolved.metallicRoughnessTextureName))
        {
            resolved.occlusionTextureName = resolved.metallicRoughnessTextureName;
        }

        return resolved;
    }

    MaterialTextureSemantic ClassifyDescriptorBinding(
        std::string_view shaderClass,
        std::string_view slotName,
        std::string_view textureName)
    {
        if (SlotIsInPreference(slotName, BuildBaseColorSlotPreference(shaderClass)))
        {
            return MaterialTextureSemantic::BaseColor;
        }

        if (SlotIsInPreference(slotName, BuildNormalMapSlotPreference(shaderClass)))
        {
            return MaterialTextureSemantic::Normal;
        }

        if (SlotIsInPreference(slotName, BuildOcclusionSlotPreference(shaderClass)) ||
            TextureNameLooksLikeOcclusion(textureName))
        {
            return MaterialTextureSemantic::Occlusion;
        }

        if (TextureNameLooksLikeMetallicRoughness(textureName))
        {
            return MaterialTextureSemantic::MetallicRoughness;
        }

        return MaterialTextureSemantic::Unknown;
    }

    std::string NormalizeTextureSemanticName(std::string_view value)
    {
        size_t start = 0;
        const size_t slash = value.find_last_of("/\\");
        if (slash != std::string_view::npos)
        {
            start = slash + 1;
        }

        size_t end = value.size();
        const size_t dot = value.find_last_of('.');
        if (dot != std::string_view::npos && dot > start)
        {
            end = dot;
        }

        std::string normalized;
        normalized.reserve(end - start);
        bool previousUnderscore = false;
        for (size_t i = start; i < end; ++i)
        {
            const unsigned char c = static_cast<unsigned char>(value[i]);
            if (std::isalnum(c) != 0)
            {
                normalized.push_back(static_cast<char>(std::tolower(c)));
                previousUnderscore = false;
            }
            else if (!previousUnderscore)
            {
                normalized.push_back('_');
                previousUnderscore = true;
            }
        }

        while (!normalized.empty() && normalized.front() == '_')
        {
            normalized.erase(normalized.begin());
        }

        while (!normalized.empty() && normalized.back() == '_')
        {
            normalized.pop_back();
        }

        return normalized;
    }

    TextureMapAnalysis AnalyzeTextureMaps(std::string_view textureName)
    {
        TextureMapAnalysis analysis;
        std::string normalized;
        const std::string_view suffix = LastSemanticToken(textureName, normalized);

        if (suffix == "n")
        {
            analysis.layout = TextureContentLayout::WarThunderPackedNormalPbr;
            analysis.confidence = TextureDetectionConfidence::StrongNameBased;
            analysis.summary = "War Thunder packed normal/PBR texture";
            analysis.caution =
                "AO is not stored in this texture. Red is smoothness, green is normal Y, "
                "blue is metallic, and alpha is normal X.";
            analysis.maps = {
                MakeDetectedMap(TextureMapKind::Normal, "A -> X, G -> Y, reconstructed Z", true),
                MakeDetectedMap(TextureMapKind::Smoothness, "R"),
                MakeDetectedMap(TextureMapKind::Roughness, "1 - R", true),
                MakeDetectedMap(TextureMapKind::Metallic, "B"),
                MakeDetectedMap(TextureMapKind::MetallicRoughness, "G = 1 - R, B = metallic", true),
            };
            return analysis;
        }

        if (suffix == "ao" || suffix == "occlusion" || suffix == "occ")
        {
            analysis.layout = TextureContentLayout::AmbientOcclusion;
            analysis.confidence = TextureDetectionConfidence::StrongNameBased;
            analysis.summary = "Ambient occlusion texture";
            analysis.caution = "Detected from the texture suffix; the red channel is exported as linear AO.";
            analysis.maps = {
                MakeDetectedMap(TextureMapKind::AmbientOcclusion, "R"),
            };
            return analysis;
        }

        if (suffix == "orm" || suffix == "arm")
        {
            analysis.layout = TextureContentLayout::PackedOrm;
            analysis.confidence = TextureDetectionConfidence::StrongNameBased;
            analysis.summary = "Packed occlusion/roughness/metallic texture";
            analysis.caution = "R = AO, G = roughness, B = metallic.";
            analysis.maps = {
                MakeDetectedMap(TextureMapKind::AmbientOcclusion, "R"),
                MakeDetectedMap(TextureMapKind::Roughness, "G"),
                MakeDetectedMap(TextureMapKind::Metallic, "B"),
                MakeDetectedMap(TextureMapKind::OcclusionRoughnessMetallic, "R = AO, G = roughness, B = metallic"),
            };
            return analysis;
        }

        if (suffix == "rma")
        {
            analysis.layout = TextureContentLayout::PackedRma;
            analysis.confidence = TextureDetectionConfidence::StrongNameBased;
            analysis.summary = "Packed roughness/metallic/occlusion texture";
            analysis.caution = "R = roughness, G = metallic, B = AO; ORM export reorders these channels.";
            analysis.maps = {
                MakeDetectedMap(TextureMapKind::AmbientOcclusion, "B"),
                MakeDetectedMap(TextureMapKind::Roughness, "R"),
                MakeDetectedMap(TextureMapKind::Metallic, "G"),
                MakeDetectedMap(TextureMapKind::OcclusionRoughnessMetallic, "Reordered to R = AO, G = roughness, B = metallic", true),
            };
            return analysis;
        }

        if (suffix == "mra" || suffix == "mrao")
        {
            analysis.layout = TextureContentLayout::PackedMra;
            analysis.confidence = TextureDetectionConfidence::StrongNameBased;
            analysis.summary = "Packed metallic/roughness/occlusion texture";
            analysis.caution = "R = metallic, G = roughness, B = AO; ORM export reorders these channels.";
            analysis.maps = {
                MakeDetectedMap(TextureMapKind::AmbientOcclusion, "B"),
                MakeDetectedMap(TextureMapKind::Roughness, "G"),
                MakeDetectedMap(TextureMapKind::Metallic, "R"),
                MakeDetectedMap(TextureMapKind::OcclusionRoughnessMetallic, "Reordered to R = AO, G = roughness, B = metallic", true),
            };
            return analysis;
        }

        if (suffix == "c")
        {
            analysis.layout = TextureContentLayout::BaseColor;
            analysis.confidence = TextureDetectionConfidence::StrongNameBased;
            analysis.summary = "Color/albedo texture";
            analysis.caution = "RGB is treated as sRGB base color; alpha remains shader-dependent.";
            analysis.maps = {
                MakeDetectedMap(TextureMapKind::BaseColor, "RGB, optional A"),
            };
            return analysis;
        }

        if (suffix == "d")
        {
            analysis.layout = TextureContentLayout::BaseColor;
            analysis.confidence = TextureDetectionConfidence::NameBased;
            analysis.summary = "Probable diffuse/detail color texture";
            analysis.caution =
                "The _d suffix is shader-dependent. It may be base color, detail diffuse, "
                "damage, or decal data; verify the model material before treating it as albedo.";
            analysis.maps = {
                MakeDetectedMap(TextureMapKind::BaseColor, "RGB, shader-dependent"),
            };
            return analysis;
        }

        if (suffix == "normal" || suffix == "norm")
        {
            analysis.layout = TextureContentLayout::ConventionalNormal;
            analysis.confidence = TextureDetectionConfidence::NameBased;
            analysis.summary = "Probable conventional RGB normal texture";
            analysis.caution = "Detected by name only; no packed War Thunder PBR channels are assumed.";
            analysis.maps = {
                MakeDetectedMap(TextureMapKind::Normal, "RGB"),
            };
            return analysis;
        }

        analysis.summary = "Map layout not detected safely";
        analysis.caution =
            "The filename does not identify a supported channel layout. The source can still be "
            "exported, but individual maps are disabled to avoid writing incorrect data.";
        return analysis;
    }

    std::string_view TextureMapKindName(TextureMapKind kind)
    {
        switch (kind)
        {
        case TextureMapKind::BaseColor: return "Base Color";
        case TextureMapKind::Normal: return "Normal";
        case TextureMapKind::AmbientOcclusion: return "Ambient Occlusion";
        case TextureMapKind::Smoothness: return "Smoothness";
        case TextureMapKind::Roughness: return "Roughness";
        case TextureMapKind::Metallic: return "Metallic";
        case TextureMapKind::MetallicRoughness: return "Metallic-Roughness";
        case TextureMapKind::OcclusionRoughnessMetallic: return "ORM";
        }
        return "Unknown";
    }

    std::string_view TextureMapKindFileSuffix(TextureMapKind kind)
    {
        switch (kind)
        {
        case TextureMapKind::BaseColor: return "_basecolor";
        case TextureMapKind::Normal: return "_normal";
        case TextureMapKind::AmbientOcclusion: return "_ao";
        case TextureMapKind::Smoothness: return "_smoothness";
        case TextureMapKind::Roughness: return "_roughness";
        case TextureMapKind::Metallic: return "_metallic";
        case TextureMapKind::MetallicRoughness: return "_metalrough";
        case TextureMapKind::OcclusionRoughnessMetallic: return "_orm";
        }
        return "_map";
    }

    bool LooksLikePackedOrm(std::string_view textureName)
    {
        const TextureContentLayout layout = AnalyzeTextureMaps(textureName).layout;
        return layout == TextureContentLayout::PackedOrm ||
            layout == TextureContentLayout::PackedRma ||
            layout == TextureContentLayout::PackedMra;
    }

    bool LooksLikePackedNormalMap(std::string_view textureName)
    {
        return AnalyzeTextureMaps(textureName).layout ==
            TextureContentLayout::WarThunderPackedNormalPbr;
    }
}
