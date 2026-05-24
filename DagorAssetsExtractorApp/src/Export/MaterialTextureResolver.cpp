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

        bool TextureNameLooksLikeOcclusion(std::string_view textureName)
        {
            const std::string normalized = NormalizeTextureSemanticName(textureName);
            return HasAnyToken(normalized, { "ao", "occ", "occlusion" });
        }

        bool TextureNameLooksLikeMetallicRoughness(std::string_view textureName)
        {
            const std::string normalized = NormalizeTextureSemanticName(textureName);
            if (normalized.empty())
            {
                return false;
            }

            if (HasAnyToken(normalized, { "orm", "rma", "mra", "mrao", "arm" }))
            {
                return true;
            }

            const bool hasMetal =
                HasAnyToken(normalized, { "metal", "metallic", "metalness", "mtl" });
            const bool hasRough =
                HasAnyToken(normalized, { "rough", "roughness", "rgh" });
            return hasMetal && hasRough;
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
}
