#include "AssetIndex/BlkRelationships.h"

#include "AssetIndex/PackContents.h"
#include "DagorFormats/BlkReader.h"
#include "Platform/Platform.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace DagorAssetsExtractor::AssetIndex
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr size_t MaxWarnings = 12;

        std::string Trim(std::string value)
        {
            const auto isSpace = [](unsigned char c) {
                return std::isspace(c) != 0;
            };

            value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char c) {
                return !isSpace(static_cast<unsigned char>(c));
            }));
            value.erase(std::find_if(value.rbegin(), value.rend(), [&](char c) {
                return !isSpace(static_cast<unsigned char>(c));
            }).base(), value.end());
            return value;
        }

        bool EndsWith(std::string_view value, std::string_view suffix)
        {
            if (suffix.size() > value.size())
            {
                return false;
            }

            return std::equal(suffix.rbegin(), suffix.rend(), value.rbegin());
        }

        std::string NormalizeSlashes(std::string value)
        {
            std::replace(value.begin(), value.end(), '\\', '/');
            return value;
        }

        std::string StripKnownPackSuffix(std::string value)
        {
            if (EndsWith(value, ".dxp.bin"))
            {
                value.resize(value.size() - std::string_view(".dxp.bin").size());
            }
            else if (EndsWith(value, ".vromfs.bin"))
            {
                value.resize(value.size() - std::string_view(".vromfs.bin").size());
            }
            else if (EndsWith(value, ".grp"))
            {
                value.resize(value.size() - std::string_view(".grp").size());
            }

            return value;
        }

        std::string BasenameKey(std::string value)
        {
            value = NormalizeSlashes(std::move(value));
            const size_t slash = value.find_last_of('/');
            if (slash != std::string::npos)
            {
                value = value.substr(slash + 1);
            }
            return StripKnownPackSuffix(ToLowerAscii(Trim(std::move(value))));
        }

        std::string FullPathKey(std::string value)
        {
            value = NormalizeSlashes(std::move(value));
            return StripKnownPackSuffix(ToLowerAscii(Trim(std::move(value))));
        }

        void AddLookupKey(
            std::unordered_map<std::string, const ResourceRecord*>& lookup,
            std::string key,
            const ResourceRecord& record)
        {
            if (!key.empty() && lookup.find(key) == lookup.end())
            {
                lookup.emplace(std::move(key), &record);
            }
        }

        std::unordered_map<std::string, const ResourceRecord*> BuildResourceLookup(
            const std::vector<ResourceRecord>& records,
            AssetType type)
        {
            std::unordered_map<std::string, const ResourceRecord*> lookup;
            for (const ResourceRecord& record : records)
            {
                if (record.type != type)
                {
                    continue;
                }

                const std::string pathUtf8 = Platform::PathToUtf8(record.path);
                const std::string fileUtf8 = Platform::PathToUtf8(record.path.filename());
                AddLookupKey(lookup, FullPathKey(pathUtf8), record);
                AddLookupKey(lookup, BasenameKey(fileUtf8), record);
            }
            return lookup;
        }

        std::string StripFinalExtension(std::string value)
        {
            const size_t slash = value.find_last_of('/');
            const size_t dot = value.find_last_of('.');
            if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
            {
                value.resize(dot);
            }
            return value;
        }

        void AddWarning(BlkResourceLinkBuildResult& result, std::string warning)
        {
            if (result.warnings.size() < MaxWarnings)
            {
                result.warnings.push_back(std::move(warning));
            }
        }

        std::optional<AssetType> ExpectedTypeForBlkRule(const std::string& propertyName)
        {
            if (propertyName == "gameResPack")
            {
                return AssetType::Grp;
            }
            if (propertyName == "ddsxTexPack")
            {
                return AssetType::Dxp;
            }
            return std::nullopt;
        }

        const ResourceRecord* FindMatchingRecord(
            const std::unordered_map<std::string, const ResourceRecord*>& lookup,
            std::string_view resourceName)
        {
            const std::string fullKey = FullPathKey(std::string(resourceName));
            if (const auto it = lookup.find(fullKey); it != lookup.end())
            {
                return it->second;
            }

            const std::string basenameKey = BasenameKey(std::string(resourceName));
            if (const auto it = lookup.find(basenameKey); it != lookup.end())
            {
                return it->second;
            }

            return nullptr;
        }

        void CollectLinksFromBlock(
            const DagorFormats::BlkBlock& block,
            const fs::path& blkPath,
            const std::unordered_map<std::string, const ResourceRecord*>& grpLookup,
            const std::unordered_map<std::string, const ResourceRecord*>& dxpLookup,
            std::vector<BlkResourceLink>& links)
        {
            for (const DagorFormats::BlkProperty& property : block.properties)
            {
                const std::optional<AssetType> expectedType = ExpectedTypeForBlkRule(property.name);
                if (!expectedType)
                {
                    continue;
                }

                const auto& lookup = *expectedType == AssetType::Grp ? grpLookup : dxpLookup;
                const ResourceRecord* matched = FindMatchingRecord(lookup, property.value);

                BlkResourceLink link;
                link.blkPath = blkPath;
                link.line = property.line;
                link.ruleName = property.name;
                link.resourceName = property.value;
                link.expectedType = *expectedType;
                if (matched != nullptr)
                {
                    link.matchedPath = matched->path;
                }

                links.push_back(std::move(link));
            }

            for (const DagorFormats::BlkBlock& child : block.children)
            {
                CollectLinksFromBlock(child, blkPath, grpLookup, dxpLookup, links);
            }
        }
    }

    BlkResourceLinkBuildResult BuildBlkResourceLinks(
        const std::vector<ResourceRecord>& records,
        uintmax_t maxBlkBytes)
    {
        BlkResourceLinkBuildResult result;
        const auto grpLookup = BuildResourceLookup(records, AssetType::Grp);
        const auto dxpLookup = BuildResourceLookup(records, AssetType::Dxp);

        for (const ResourceRecord& record : records)
        {
            if (record.type != AssetType::Blk)
            {
                continue;
            }

            if (record.sizeBytes > maxBlkBytes)
            {
                ++result.blkFilesSkipped;
                AddWarning(result, "Skipped large BLK file: " + Platform::PathToUtf8(record.path.filename()));
                continue;
            }

            std::ifstream file(record.path, std::ios::binary);
            if (!file)
            {
                ++result.parseErrors;
                AddWarning(result, "Could not open BLK file: " + Platform::PathToUtf8(record.path.filename()));
                continue;
            }

            const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

            try
            {
                DagorFormats::BlkReader reader(text);
                reader.Parse();
                CollectLinksFromBlock(reader.GetRoot(), record.path, grpLookup, dxpLookup, result.links);
                ++result.blkFilesParsed;
            }
            catch (const std::exception& exception)
            {
                ++result.parseErrors;
                std::ostringstream warning;
                warning << "BLK relationship parse failed for "
                        << Platform::PathToUtf8(record.path.filename())
                        << ": " << exception.what();
                AddWarning(result, warning.str());
            }
        }

        return result;
    }
}
