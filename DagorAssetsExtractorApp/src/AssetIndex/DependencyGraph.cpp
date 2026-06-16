#include "AssetIndex/DependencyGraph.h"

#include "AssetIndex/PackContents.h"
#include "AssetIndex/TextureQuality.h"
#include "Platform/Platform.h"
#include "DagorFormats/GrpReader.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace DagorAssetsExtractor::AssetIndex
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr size_t MaxWarnings = 12;
        constexpr size_t MaxDescriptorIndexThreads = 8;
        constexpr size_t ScopedDxpCandidateLimit = 48;
        constexpr int DescriptorLinkerVersion = 9;
        constexpr int DescriptorLinkCacheVersion = 4;

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

        std::wstring PathKey(const fs::path& path)
        {
            return path.lexically_normal().wstring();
        }

        bool SamePath(const fs::path& left, const fs::path& right)
        {
            return PathKey(left) == PathKey(right);
        }

        void AppendUniquePath(std::vector<fs::path>& paths, const fs::path& path)
        {
            if (std::find_if(paths.begin(), paths.end(), [&](const fs::path& existing) {
                return SamePath(existing, path);
            }) == paths.end())
            {
                paths.push_back(path);
            }
        }

        void AppendUniqueUInt(std::vector<uint32_t>& values, uint32_t value)
        {
            if (std::find(values.begin(), values.end(), value) == values.end())
            {
                values.push_back(value);
            }
        }

        void AppendUniqueString(std::vector<std::string>& values, std::string value)
        {
            if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end())
            {
                values.push_back(std::move(value));
            }
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

        std::string StripDagorTextureQualifier(std::string value)
        {
            const size_t starMarker = value.find('*');
            if (starMarker != std::string::npos)
            {
                value.resize(starMarker);
            }

            const size_t qualityMarker = value.find('$');
            if (qualityMarker != std::string::npos)
            {
                value.resize(qualityMarker);
            }

            return value;
        }

        std::vector<std::string> BuildAssetNameKeys(std::string_view value)
        {
            std::vector<std::string> keys;
            auto addKey = [&keys](std::string key) {
                if (!key.empty() && std::find(keys.begin(), keys.end(), key) == keys.end())
                {
                    keys.push_back(std::move(key));
                }
            };
            auto addKeyVariants = [&](std::string key) {
                addKey(key);
                addKey(StripFinalExtension(key));

                std::string textureKey = StripDagorTextureQualifier(std::move(key));
                addKey(textureKey);
                addKey(StripFinalExtension(std::move(textureKey)));
            };

            std::string normalized = NormalizeSlashes(std::string(value));
            normalized = ToLowerAscii(Trim(std::move(normalized)));
            addKeyVariants(normalized);

            const size_t slash = normalized.find_last_of('/');
            if (slash != std::string::npos)
            {
                std::string basename = normalized.substr(slash + 1);
                addKeyVariants(std::move(basename));
            }

            return keys;
        }

        bool NameKeysIntersect(
            const std::vector<std::string>& leftKeys,
            const std::vector<std::string>& rightKeys)
        {
            for (const std::string& leftKey : leftKeys)
            {
                if (std::find(rightKeys.begin(), rightKeys.end(), leftKey) != rightKeys.end())
                {
                    return true;
                }
            }
            return false;
        }

        std::string StripPackQualitySuffix(std::string value)
        {
            constexpr std::string_view suffixes[] = {
                "-hq", "_hq", "-uhq", "_uhq"
            };

            for (const std::string_view suffix : suffixes)
            {
                if (EndsWith(value, suffix))
                {
                    value.resize(value.size() - suffix.size());
                    break;
                }
            }

            return value;
        }

        std::string PackStemKey(const fs::path& path)
        {
            return StripPackQualitySuffix(BasenameKey(Platform::PathToUtf8(path.filename())));
        }

        bool IsNameRelatedToPackStem(const std::string& nameKey, const std::string& packStem)
        {
            if (nameKey.empty() || packStem.empty())
            {
                return false;
            }

            if (nameKey == packStem)
            {
                return true;
            }

            const auto startsWithToken = [](const std::string& value, const std::string& token) {
                return value.size() > token.size() &&
                       value.compare(0, token.size(), token) == 0 &&
                       (value[token.size()] == '_' || value[token.size()] == '-');
            };

            return startsWithToken(nameKey, packStem) || startsWithToken(packStem, nameKey);
        }

        std::vector<std::string_view> SplitNameTokens(std::string_view value)
        {
            std::vector<std::string_view> tokens;
            size_t start = 0;
            while (start < value.size())
            {
                while (start < value.size() && (value[start] == '_' || value[start] == '-'))
                {
                    ++start;
                }
                const size_t end = value.find_first_of("_-", start);
                if (end == std::string_view::npos)
                {
                    if (start < value.size())
                    {
                        tokens.push_back(value.substr(start));
                    }
                    break;
                }
                if (end > start)
                {
                    tokens.push_back(value.substr(start, end - start));
                }
                start = end + 1;
            }
            return tokens;
        }

        size_t CommonPrefixTokenCount(std::string_view left, std::string_view right)
        {
            size_t leftStart = 0;
            size_t rightStart = 0;
            size_t matched = 0;

            while (leftStart < left.size() && rightStart < right.size())
            {
                while (leftStart < left.size() && (left[leftStart] == '_' || left[leftStart] == '-'))
                {
                    ++leftStart;
                }
                while (rightStart < right.size() && (right[rightStart] == '_' || right[rightStart] == '-'))
                {
                    ++rightStart;
                }

                if (leftStart >= left.size() || rightStart >= right.size())
                {
                    break;
                }

                const size_t leftEnd = left.find_first_of("_-", leftStart);
                const size_t rightEnd = right.find_first_of("_-", rightStart);

                const std::string_view leftToken = (leftEnd == std::string_view::npos)
                    ? left.substr(leftStart)
                    : left.substr(leftStart, leftEnd - leftStart);
                const std::string_view rightToken = (rightEnd == std::string_view::npos)
                    ? right.substr(rightStart)
                    : right.substr(rightStart, rightEnd - rightStart);

                if (leftToken != rightToken)
                {
                    break;
                }

                ++matched;
                leftStart = (leftEnd == std::string_view::npos) ? left.size() : leftEnd + 1;
                rightStart = (rightEnd == std::string_view::npos) ? right.size() : rightEnd + 1;
            }

            return matched;
        }

        int NamePackAffinityScore(std::string_view name, const std::string& packStem)
        {
            int score = 0;
            for (const std::string& key : BuildAssetNameKeys(name))
            {
                if (IsNameRelatedToPackStem(key, packStem))
                {
                    score = std::max(score, 120);
                }

                const size_t commonPrefix = CommonPrefixTokenCount(key, packStem);
                if (commonPrefix >= 3)
                {
                    score = std::max(score, 80 + static_cast<int>(commonPrefix * 5));
                }
                else if (commonPrefix == 2)
                {
                    score = std::max(score, 45);
                }
            }
            return score;
        }

        bool IsPackRelatedToAnyName(const fs::path& path, const std::vector<std::string>& names)
        {
            const std::string packStem = PackStemKey(path);
            for (const std::string& name : names)
            {
                for (const std::string& key : BuildAssetNameKeys(name))
                {
                    if (IsNameRelatedToPackStem(key, packStem))
                    {
                        return true;
                    }
                }
            }

            return false;
        }

        bool IsSameDirectory(const fs::path& left, const fs::path& right)
        {
            return SamePath(left.parent_path(), right.parent_path());
        }

        bool IsAncestorOrSamePath(const fs::path& ancestor, const fs::path& path)
        {
            std::string ancestorKey = FullPathKey(Platform::PathToUtf8(ancestor.lexically_normal()));
            const std::string pathKey = FullPathKey(Platform::PathToUtf8(path.lexically_normal()));
            if (ancestorKey.empty() || pathKey.empty())
            {
                return false;
            }
            if (ancestorKey == pathKey)
            {
                return true;
            }
            if (ancestorKey.back() != '/')
            {
                ancestorKey.push_back('/');
            }
            return pathKey.rfind(ancestorKey, 0) == 0;
        }

        bool IsDescriptorInFocusResourceRoot(const ResourceRecord& descriptor, const ResourceRecord& focus)
        {
            const fs::path descriptorDir = descriptor.path.parent_path();
            const fs::path focusDir = focus.path.parent_path();
            return IsAncestorOrSamePath(descriptorDir, focusDir) ||
                   IsAncestorOrSamePath(focusDir, descriptorDir);
        }

        bool ContainsAscii(std::string_view value, std::string_view needle)
        {
            return value.find(needle) != std::string_view::npos;
        }

        struct DescriptorPackGroup
        {
            fs::path blkPath;
            std::vector<fs::path> grpPaths;
            std::vector<fs::path> dxpPaths;
        };

        int CommonPathScore(const fs::path& left, const fs::path& right);
        bool PathLooksLikePatch(const fs::path& path);
        bool PathLooksLikeHq(const fs::path& path);

        std::vector<DescriptorPackGroup> BuildDescriptorPackGroups(const std::vector<BlkResourceLink>& links)
        {
            std::vector<DescriptorPackGroup> groups;
            std::unordered_map<std::wstring, size_t> groupByBlk;

            for (const BlkResourceLink& link : links)
            {
                if (!link.matchedPath)
                {
                    continue;
                }

                const std::wstring blkKey = PathKey(link.blkPath);
                auto [it, inserted] = groupByBlk.emplace(blkKey, groups.size());
                if (inserted)
                {
                    DescriptorPackGroup group;
                    group.blkPath = link.blkPath;
                    groups.push_back(std::move(group));
                }

                DescriptorPackGroup& group = groups[it->second];
                if (link.expectedType == AssetType::Grp)
                {
                    AppendUniquePath(group.grpPaths, *link.matchedPath);
                }
                else if (link.expectedType == AssetType::Dxp)
                {
                    AppendUniquePath(group.dxpPaths, *link.matchedPath);
                }
            }

            return groups;
        }

        const ResourceRecord* FindFocusRecord(
            const std::vector<ResourceRecord>& records,
            const std::optional<fs::path>& focusPath)
        {
            if (!focusPath)
            {
                return nullptr;
            }

            for (const ResourceRecord& record : records)
            {
                if (SamePath(record.path, *focusPath))
                {
                    return &record;
                }
            }

            return nullptr;
        }

        bool IsRecordRelatedToFocus(const ResourceRecord& record, const ResourceRecord& focus)
        {
            if (SamePath(record.path, focus.path))
            {
                return true;
            }

            if (IsSameDirectory(record.path, focus.path))
            {
                return true;
            }

            const std::string focusStem = PackStemKey(focus.path);
            const std::string recordStem = PackStemKey(record.path);
            if (IsNameRelatedToPackStem(recordStem, focusStem))
            {
                return true;
            }

            return CommonPathScore(record.path, focus.path) >= 45;
        }

        bool ShouldIncludeDescriptorForFocus(const ResourceRecord& record, const ResourceRecord* focus)
        {
            if (focus == nullptr)
            {
                return true;
            }

            if (focus->type == AssetType::Descriptor)
            {
                return SamePath(record.path, focus->path);
            }

            // Descriptors have generic names such as dynModelDesc.bin and riDesc.bin and are often
            // stored in a parent resource folder rather than beside the selected GRP/DXP pack.
            // Keep scoped builds on that resource root instead of doing an exhaustive descriptor parse.
            return IsRecordRelatedToFocus(record, *focus) ||
                   IsDescriptorInFocusResourceRoot(record, *focus);
        }

        bool ShouldIncludeGrpForFocus(const ResourceRecord& record, const ResourceRecord* focus)
        {
            if (focus == nullptr)
            {
                return true;
            }

            if (focus->type == AssetType::Grp)
            {
                return SamePath(record.path, focus->path);
            }
            if (focus->type == AssetType::Descriptor || focus->type == AssetType::Dxp)
            {
                return IsRecordRelatedToFocus(record, *focus);
            }

            return true;
        }

        bool ShouldIncludeDxpForFocus(const ResourceRecord& record, const ResourceRecord* focus)
        {
            if (focus == nullptr)
            {
                return true;
            }

            if (focus->type == AssetType::Dxp)
            {
                return SamePath(record.path, focus->path);
            }
            if (focus->type == AssetType::Descriptor || focus->type == AssetType::Grp)
            {
                // HQ texture packs commonly live under content.hq/hq_tex and do not share the
                // selected GRP's folder or country-prefixed stem. Keep them as cheap path-only
                // candidates until descriptor texture names are known, then rank and cap them.
                return true;
            }

            return true;
        }

        bool DescriptorPathLooksLikeDynModelDesc(const fs::path& path)
        {
            return ContainsAscii(BasenameKey(Platform::PathToUtf8(path.filename())), "dynmodeldesc");
        }

        bool DescriptorPathLooksLikeRiDesc(const fs::path& path)
        {
            return ContainsAscii(BasenameKey(Platform::PathToUtf8(path.filename())), "ridesc");
        }

        bool LinkMatchesFocus(const DescriptorResourceLink& link, const ResourceRecord* focus)
        {
            if (focus == nullptr)
            {
                return true;
            }

            if (focus->type == AssetType::Descriptor)
            {
                return SamePath(link.descriptorPath, focus->path);
            }
            if (focus->type == AssetType::Grp)
            {
                return link.matchedGrpPath && SamePath(*link.matchedGrpPath, focus->path);
            }
            if (focus->type == AssetType::Dxp)
            {
                for (const DescriptorTextureLink& textureLink : link.textureLinks)
                {
                    if (textureLink.matchedDxpPath && SamePath(*textureLink.matchedDxpPath, focus->path))
                    {
                        return true;
                    }
                }
                return false;
            }

            return true;
        }

        void AddWarning(DescriptorResourceLinkBuildResult& result, std::string warning)
        {
            if (result.warnings.size() < MaxWarnings)
            {
                result.warnings.push_back(std::move(warning));
            }
        }

        struct LightweightGrpEntry
        {
            fs::path path;
            std::string name;
            std::vector<std::string> nameKeys;
            uint32_t classId = 0;
            std::string className;
            bool canExportStaticModel = false;
            bool canExportSkinnedModel = false;
        };

        struct LightweightDxpEntry
        {
            fs::path path;
            std::string name;
            std::vector<std::string> nameKeys;
        };

        uint16_t ReadLe16(const std::vector<uint8_t>& data, size_t offset)
        {
            if (offset > data.size() || data.size() - offset < sizeof(uint16_t))
            {
                throw std::runtime_error("metadata read out of bounds");
            }

            return static_cast<uint16_t>(data[offset]) |
                   static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1]) << 8);
        }

        uint32_t ReadLe32(const std::vector<uint8_t>& data, size_t offset)
        {
            if (offset > data.size() || data.size() - offset < sizeof(uint32_t))
            {
                throw std::runtime_error("metadata read out of bounds");
            }

            return static_cast<uint32_t>(data[offset]) |
                   (static_cast<uint32_t>(data[offset + 1]) << 8) |
                   (static_cast<uint32_t>(data[offset + 2]) << 16) |
                   (static_cast<uint32_t>(data[offset + 3]) << 24);
        }

        bool ReadFilePrefix(const fs::path& path, size_t byteCount, std::vector<uint8_t>& data, std::string& error)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                error = "Could not open " + Platform::PathToUtf8(path.filename());
                return false;
            }

            data.resize(byteCount);
            if (byteCount == 0)
            {
                return true;
            }

            file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(byteCount));
            if (!file)
            {
                error = "Could not read metadata from " + Platform::PathToUtf8(path.filename());
                return false;
            }

            return true;
        }

        bool IsRangeWithin(size_t offset, size_t size, size_t begin, size_t end)
        {
            return offset >= begin && offset <= end && size <= end - offset;
        }

        std::string ReadCStringAt(
            const std::vector<uint8_t>& data,
            size_t offset,
            size_t metadataBegin,
            size_t metadataEnd)
        {
            if (!IsRangeWithin(offset, 1, metadataBegin, metadataEnd))
            {
                throw std::runtime_error("name offset out of bounds");
            }

            constexpr size_t maxNameLength = 2048;
            const size_t maxEnd = std::min(metadataEnd, offset + maxNameLength);
            size_t end = offset;
            while (end < maxEnd && data[end] != 0)
            {
                ++end;
            }

            if (end >= metadataEnd || (end == maxEnd && data[end] != 0))
            {
                throw std::runtime_error("name is not null-terminated");
            }

            return std::string(reinterpret_cast<const char*>(data.data() + offset), end - offset);
        }

        struct PatchableTabRef
        {
            uint32_t offset = 0;
            uint32_t count = 0;
        };

        PatchableTabRef ReadPatchableTabRef(const std::vector<uint8_t>& data, size_t offset)
        {
            PatchableTabRef ref;
            ref.offset = ReadLe32(data, offset);
            ref.count = ReadLe32(data, offset + 4);
            return ref;
        }

        bool CheckedMul(size_t lhs, size_t rhs, size_t& out)
        {
            if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs)
            {
                return false;
            }
            out = lhs * rhs;
            return true;
        }

        void ValidateMetadataTable(
            const PatchableTabRef& ref,
            size_t elemSize,
            size_t metadataBegin,
            size_t metadataEnd,
            const char* tableName)
        {
            if (ref.count == 0)
            {
                return;
            }

            size_t byteSize = 0;
            if (!CheckedMul(static_cast<size_t>(ref.count), elemSize, byteSize) ||
                !IsRangeWithin(ref.offset, byteSize, metadataBegin, metadataEnd))
            {
                throw std::runtime_error(std::string("GRP metadata ") + tableName + " table out of bounds");
            }
        }

        bool ParseGrpMetadataForLinking(
            const fs::path& path,
            uintmax_t maxPackBytes,
            std::vector<LightweightGrpEntry>& entries,
            std::string& error)
        {
            std::error_code ec;
            const uintmax_t fileSize = fs::file_size(path, ec);
            if (ec || fileSize < 8)
            {
                error = "GRP file metadata is unavailable: " + Platform::PathToUtf8(path.filename());
                return false;
            }

            std::vector<uint8_t> header;
            if (!ReadFilePrefix(path, static_cast<size_t>(std::min<uintmax_t>(fileSize, 16)), header, error))
            {
                return false;
            }

            const std::string magic(reinterpret_cast<const char*>(header.data()), std::min<size_t>(4, header.size()));
            if (magic == "GRP2" || magic == "GRP3")
            {
                if (header.size() < 16)
                {
                    error = "GRP2/GRP3 header is truncated: " + Platform::PathToUtf8(path.filename());
                    return false;
                }

                try
                {
                    constexpr size_t modernHeaderSize = 16;
                    constexpr size_t patchableTabSize = 16;
                    constexpr size_t grpDataHeaderSize = patchableTabSize * 3;
                    constexpr size_t resEntrySize = 12;
                    constexpr size_t maxMetadataBytes = 64 * 1024 * 1024;

                    const uint32_t fullDataSize = ReadLe32(header, 8);
                    const size_t metadataSize = modernHeaderSize + static_cast<size_t>(fullDataSize);
                    if (fullDataSize < grpDataHeaderSize || metadataSize > fileSize || metadataSize > maxMetadataBytes)
                    {
                        error = "GRP metadata size is out of bounds: " + Platform::PathToUtf8(path.filename());
                        return false;
                    }

                    std::vector<uint8_t> data;
                    if (!ReadFilePrefix(path, metadataSize, data, error))
                    {
                        return false;
                    }

                    const size_t metadataBegin = modernHeaderSize;
                    const size_t metadataEnd = metadataSize;
                    const PatchableTabRef nameMapRef = ReadPatchableTabRef(data, modernHeaderSize);
                    const PatchableTabRef resTableRef = ReadPatchableTabRef(data, modernHeaderSize + patchableTabSize);
                    const PatchableTabRef resDataRef = ReadPatchableTabRef(data, modernHeaderSize + patchableTabSize * 2);

                    ValidateMetadataTable(nameMapRef, sizeof(uint32_t), metadataBegin, metadataEnd, "name map");
                    ValidateMetadataTable(resTableRef, resEntrySize, metadataBegin, metadataEnd, "resource");
                    if (resDataRef.count > 0)
                    {
                        const size_t resDataElemSize = magic == "GRP2" ? 24 : 16;
                        ValidateMetadataTable(resDataRef, resDataElemSize, metadataBegin, metadataEnd, "resource dependency");
                    }

                    std::vector<int32_t> nameMap;
                    nameMap.reserve(nameMapRef.count);
                    for (uint32_t i = 0; i < nameMapRef.count; ++i)
                    {
                        nameMap.push_back(static_cast<int32_t>(
                            ReadLe32(data, nameMapRef.offset + static_cast<size_t>(i) * sizeof(uint32_t))));
                    }

                    for (uint32_t i = 0; i < resTableRef.count; ++i)
                    {
                        const size_t entryOffset = resTableRef.offset + static_cast<size_t>(i) * resEntrySize;
                        const uint32_t classId = ReadLe32(data, entryOffset);
                        const uint16_t resId = ReadLe16(data, entryOffset + 8);

                        LightweightGrpEntry entry;
                        entry.path = path;
                        entry.classId = classId;
                        entry.className = DescribeGrpClassId(classId);

                        if (resId < nameMap.size() && nameMap[resId] >= 0)
                        {
                            entry.name = ReadCStringAt(data, static_cast<size_t>(nameMap[resId]), metadataBegin, metadataEnd);
                        }
                        else
                        {
                            entry.name = "resource_" + std::to_string(i);
                        }

                        SubAssetRecord exportProbe;
                        exportProbe.name = entry.name;
                        exportProbe.grpClassId = entry.classId;
                        entry.canExportStaticModel = CanExportGrpEntryAsStaticModel(exportProbe);
                        entry.canExportSkinnedModel = CanExportGrpEntryAsSkinnedModel(exportProbe);
                        entries.push_back(std::move(entry));
                    }

                    return true;
                }
                catch (const std::exception& exception)
                {
                    error = std::string("GRP metadata parse failed for ") +
                        Platform::PathToUtf8(path.filename()) + ": " + exception.what();
                    return false;
                }
            }

            if (fileSize > maxPackBytes)
            {
                error = "Skipped large legacy GRP during descriptor linking: " + Platform::PathToUtf8(path.filename());
                return false;
            }

            std::vector<uint8_t> data;
            if (!ReadFilePrefix(path, static_cast<size_t>(fileSize), data, error))
            {
                return false;
            }

            try
            {
                DagorFormats::GrpReader reader(data);
                reader.Parse();

                for (const DagorFormats::GrpFileInfo& file : reader.GetFiles())
                {
                    LightweightGrpEntry entry;
                    entry.path = path;
                    entry.name = file.name;
                    entry.classId = file.classId;
                    entry.className = DescribeGrpClassId(file.classId);

                    SubAssetRecord probe;
                    probe.name = file.name;
                    probe.grpClassId = file.classId;
                    entry.canExportStaticModel = CanExportGrpEntryAsStaticModel(probe);
                    entry.canExportSkinnedModel = CanExportGrpEntryAsSkinnedModel(probe);

                    entries.push_back(std::move(entry));
                }

                return true;
            }
            catch (const std::exception& ex)
            {
                error = std::string("Legacy GRP parse failed for ") +
                    Platform::PathToUtf8(path.filename()) + ": " + ex.what();
                return false;
            }
        }

        bool TryReadDxpNamesFromBuffer(
            const std::vector<uint8_t>& data,
            uint16_t totalFiles,
            std::vector<std::string>& names,
            bool& incomplete)
        {
            incomplete = false;
            names.clear();
            if (data.size() < 0x48)
            {
                incomplete = true;
                return false;
            }

            size_t offset = 0x48;
            while (names.size() < totalFiles)
            {
                const size_t start = offset;
                while (offset < data.size() && data[offset] != 0)
                {
                    ++offset;
                }

                if (offset >= data.size())
                {
                    incomplete = true;
                    names.clear();
                    return false;
                }

                names.emplace_back(reinterpret_cast<const char*>(data.data() + start), offset - start);
                ++offset;
            }

            return true;
        }

        bool ParseDxpMetadataForLinking(
            const fs::path& path,
            std::vector<LightweightDxpEntry>& entries,
            std::string& error)
        {
            std::error_code ec;
            const uintmax_t fileSize = fs::file_size(path, ec);
            if (ec || fileSize < 0x48)
            {
                error = "DXP file metadata is unavailable: " + Platform::PathToUtf8(path.filename());
                return false;
            }

            constexpr size_t maxMetadataBytes = 64 * 1024 * 1024;
            size_t bytesToRead = static_cast<size_t>(std::min<uintmax_t>(fileSize, 64 * 1024));
            while (true)
            {
                std::vector<uint8_t> data;
                if (!ReadFilePrefix(path, bytesToRead, data, error))
                {
                    return false;
                }

                if (data.size() < 0x48 ||
                    std::string(reinterpret_cast<const char*>(data.data()), 4) != "DxP2")
                {
                    error = "Invalid DXP magic in " + Platform::PathToUtf8(path.filename());
                    return false;
                }

                const uint16_t totalFiles = ReadLe16(data, 0x8);
                const uint32_t block3Offset = ReadLe32(data, 0x30) + 0x10;
                const size_t requiredSize = static_cast<size_t>(block3Offset) + static_cast<size_t>(totalFiles) * 0x18;

                std::vector<std::string> names;
                bool incomplete = false;
                if (TryReadDxpNamesFromBuffer(data, totalFiles, names, incomplete))
                {
                    if (data.size() >= requiredSize)
                    {
                        for (uint16_t i = 0; i < totalFiles; ++i)
                        {
                            const size_t entryStart = static_cast<size_t>(block3Offset) + static_cast<size_t>(i) * 0x18;
                            const uint32_t payloadSize = ReadLe32(data, entryStart + 0x10);
                            if (payloadSize > 0)
                            {
                                LightweightDxpEntry entry;
                                entry.path = path;
                                entry.name = std::move(names[i]);
                                entries.push_back(std::move(entry));
                            }
                        }
                        return true;
                    }
                    else
                    {
                        incomplete = true;
                    }
                }

                if (!incomplete || bytesToRead >= fileSize || bytesToRead >= maxMetadataBytes)
                {
                    error = "DXP name map is truncated or too large in " + Platform::PathToUtf8(path.filename());
                    return false;
                }

                bytesToRead = static_cast<size_t>(std::min<uintmax_t>(
                    fileSize,
                    std::min<uintmax_t>(static_cast<uintmax_t>(bytesToRead) * 2, maxMetadataBytes)));
            }
        }

        template <typename T>
        void AddIndexKeys(std::unordered_map<std::string, std::vector<size_t>>& index, const std::string& name, size_t value)
        {
            (void)sizeof(T);
            for (const std::string& key : BuildAssetNameKeys(name))
            {
                if (key.empty())
                {
                    continue;
                }

                std::vector<size_t>& bucket = index[key];
                if (std::find(bucket.begin(), bucket.end(), value) == bucket.end())
                {
                    bucket.push_back(value);
                }
            }
        }

        void AddPrecomputedIndexKeys(
            std::unordered_map<std::string, std::vector<size_t>>& index,
            const std::vector<std::string>& keys,
            size_t value)
        {
            for (const std::string& key : keys)
            {
                if (key.empty())
                {
                    continue;
                }

                std::vector<size_t>& bucket = index[key];
                if (std::find(bucket.begin(), bucket.end(), value) == bucket.end())
                {
                    bucket.push_back(value);
                }
            }
        }

        bool PathLooksLikePatch(const fs::path& path)
        {
            return ContainsAscii(FullPathKey(Platform::PathToUtf8(path)), "/patch/");
        }

        bool PathLooksLikeHq(const fs::path& path)
        {
            return ClassifyTextureQuality(path, {}) >= TextureQualityTier::High;
        }

        int CommonPathScore(const fs::path& left, const fs::path& right)
        {
            const std::string leftKey = FullPathKey(Platform::PathToUtf8(left));
            const std::string rightKey = FullPathKey(Platform::PathToUtf8(right));
            int score = 0;

            if (IsSameDirectory(left, right))
            {
                score += 40;
            }
            if (PathLooksLikePatch(left) == PathLooksLikePatch(right))
            {
                score += 10;
            }
            if (PathLooksLikeHq(left) == PathLooksLikeHq(right))
            {
                score += 10;
            }
            if (ContainsAscii(leftKey, "pkg_cockpits") && ContainsAscii(rightKey, "pkg_cockpits"))
            {
                score += 25;
            }
            if (ContainsAscii(leftKey, "/content/base/") && ContainsAscii(rightKey, "/content/base/"))
            {
                score += 10;
            }

            return score;
        }

        bool ReportDescriptorProgress(
            DescriptorResourceLinkBuildResult& result,
            const DescriptorResourceLinkBuildOptions& options,
            std::string stage,
            std::string item,
            size_t current,
            size_t total)
        {
            if (!options.progress)
            {
                return true;
            }

            DescriptorResourceLinkProgress progress;
            progress.stage = std::move(stage);
            progress.item = std::move(item);
            progress.current = current;
            progress.total = total;
            progress.descriptorFilesParsed = result.descriptorFilesParsed;
            progress.linksBuilt = result.links.size();
            progress.grpPacksParsed = result.grpPacksParsed;
            progress.dxpPacksParsed = result.dxpPacksParsed;

            if (!options.progress(progress))
            {
                result.canceled = true;
                return false;
            }

            return true;
        }

        std::vector<uint32_t> BuildTextureIndices(const SubAssetRecord& descriptorEntry)
        {
            std::vector<uint32_t> textureIndices;
            if (!descriptorEntry.materialTextureRefs.empty())
            {
                for (const DescriptorMaterialTextureRef& materialRef : descriptorEntry.materialTextureRefs)
                {
                    AppendUniqueUInt(textureIndices, materialRef.textureIndex);
                }
            }
            else if (!descriptorEntry.dependencyIds.empty())
            {
                for (const uint32_t textureIndex : descriptorEntry.dependencyIds)
                {
                    AppendUniqueUInt(textureIndices, textureIndex);
                }
            }
            else
            {
                for (size_t textureIndex = 0; textureIndex < descriptorEntry.textureNames.size(); ++textureIndex)
                {
                    AppendUniqueUInt(textureIndices, static_cast<uint32_t>(textureIndex));
                }
            }

            return textureIndices;
        }

        std::vector<std::vector<std::string>> BuildTextureKeySets(const std::vector<std::string>& textureNames)
        {
            std::vector<std::vector<std::string>> keySets;
            keySets.reserve(textureNames.size());
            for (const std::string& textureName : textureNames)
            {
                keySets.push_back(BuildAssetNameKeys(textureName));
            }
            return keySets;
        }

        bool AllTextureKeySetsIndexed(
            const std::vector<std::vector<std::string>>& keySets,
            const std::unordered_map<std::string, std::vector<size_t>>& index)
        {
            if (keySets.empty())
            {
                return false;
            }

            for (const std::vector<std::string>& keys : keySets)
            {
                bool found = false;
                for (const std::string& key : keys)
                {
                    if (index.contains(key))
                    {
                        found = true;
                        break;
                    }
                }
                if (!found)
                {
                    return false;
                }
            }
            return true;
        }

        int ScoreDxpRecordForFocus(
            const ResourceRecord& record,
            const ResourceRecord& focus,
            const std::vector<std::vector<std::string>>& focusedResourceNameKeys,
            const std::vector<std::vector<std::string>>& focusedTextureNameKeys,
            const std::string& packStem)
        {
            int score = 0;
            if (IsSameDirectory(record.path, focus.path))
            {
                score += 20;
            }
            if (PathLooksLikeHq(record.path))
            {
                score += 6;
            }
            if (PathLooksLikePatch(record.path))
            {
                score += 4;
            }
            if (packStem.find("shared") != std::string::npos ||
                packStem.find("common") != std::string::npos ||
                packStem.find("base") != std::string::npos ||
                packStem.find("general") != std::string::npos)
            {
                score += 15;
            }

            for (const std::vector<std::string>& keys : focusedResourceNameKeys)
            {
                for (const std::string& key : keys)
                {
                    if (IsNameRelatedToPackStem(key, packStem))
                    {
                        score = std::max(score, 120);
                    }
                    const size_t commonPrefix = CommonPrefixTokenCount(key, packStem);
                    if (commonPrefix >= 3)
                    {
                        score = std::max(score, 80 + static_cast<int>(commonPrefix * 5));
                    }
                    else if (commonPrefix == 2)
                    {
                        score = std::max(score, 45);
                    }
                }
            }
            for (const std::vector<std::string>& keys : focusedTextureNameKeys)
            {
                for (const std::string& key : keys)
                {
                    if (IsNameRelatedToPackStem(key, packStem))
                    {
                        score = std::max(score, 120);
                    }
                    const size_t commonPrefix = CommonPrefixTokenCount(key, packStem);
                    if (commonPrefix >= 3)
                    {
                        score = std::max(score, 80 + static_cast<int>(commonPrefix * 5));
                    }
                    else if (commonPrefix == 2)
                    {
                        score = std::max(score, 45);
                    }
                }
            }
            return score;
        }

        size_t DescriptorIndexWorkerCount(size_t recordCount)
        {
            if (recordCount == 0)
            {
                return 0;
            }

            const unsigned int hardwareThreads = std::thread::hardware_concurrency();
            const size_t hardwareCount = hardwareThreads == 0
                ? 1
                : static_cast<size_t>(hardwareThreads);
            return std::max<size_t>(
                1,
                std::min({ recordCount, hardwareCount, MaxDescriptorIndexThreads }));
        }

        long long ElapsedMilliseconds(std::chrono::steady_clock::time_point start)
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
        }

        bool BuildGrpMetadataIndexParallel(
            const std::vector<const ResourceRecord*>& grpRecords,
            uintmax_t maxPackBytes,
            DescriptorResourceLinkBuildResult& result,
            const DescriptorResourceLinkBuildOptions& options,
            std::vector<LightweightGrpEntry>& grpEntries,
            std::unordered_map<std::string, std::vector<size_t>>& grpIndex,
            size_t& workDone,
            size_t totalWork)
        {
            if (grpRecords.empty())
            {
                return true;
            }

            const size_t stageBaseWork = workDone;
            const size_t workerCount = DescriptorIndexWorkerCount(grpRecords.size());
            std::atomic<size_t> nextIndex{ 0 };
            std::atomic<size_t> filesDone{ 0 };
            std::atomic<bool> cancelRequested{ false };
            std::mutex stateMutex;

            auto worker = [&]() {
                while (!cancelRequested.load(std::memory_order_relaxed))
                {
                    const size_t recordIndex = nextIndex.fetch_add(1, std::memory_order_relaxed);
                    if (recordIndex >= grpRecords.size())
                    {
                        break;
                    }

                    const ResourceRecord& record = *grpRecords[recordIndex];
                    const auto parseStart = std::chrono::steady_clock::now();
                    std::vector<LightweightGrpEntry> localEntries;
                    std::string error;
                    const bool parsed = ParseGrpMetadataForLinking(record.path, maxPackBytes, localEntries, error);
                    std::vector<std::vector<std::string>> localKeySets;
                    if (parsed)
                    {
                        localKeySets.reserve(localEntries.size());
                        for (LightweightGrpEntry& entry : localEntries)
                        {
                            entry.nameKeys = BuildAssetNameKeys(entry.name);
                            localKeySets.push_back(entry.nameKeys);
                        }
                    }
                    const long long elapsedMs = ElapsedMilliseconds(parseStart);

                    std::lock_guard<std::mutex> lock(stateMutex);
                    if (parsed)
                    {
                        const size_t before = grpEntries.size();
                        grpEntries.insert(
                            grpEntries.end(),
                            std::make_move_iterator(localEntries.begin()),
                            std::make_move_iterator(localEntries.end()));
                        for (size_t i = before; i < grpEntries.size(); ++i)
                        {
                            AddPrecomputedIndexKeys(grpIndex, localKeySets[i - before], i);
                        }
                        ++result.grpPacksParsed;
                    }
                    else if (!error.empty())
                    {
                        AddWarning(result, error);
                    }

                    const size_t completed = filesDone.fetch_add(1, std::memory_order_relaxed) + 1;
                    std::string item = Platform::PathToUtf8(record.path.filename());
                    if (parsed)
                    {
                        item += " indexed " + std::to_string(localEntries.size()) +
                            " resources in " + std::to_string(elapsedMs) +
                            " ms (" + std::to_string(grpEntries.size()) + " total)";
                    }
                    else
                    {
                        item += " failed in " + std::to_string(elapsedMs) + " ms";
                    }

                    if (!ReportDescriptorProgress(
                            result,
                            options,
                            "GRP index",
                            std::move(item),
                            stageBaseWork + completed,
                            totalWork))
                    {
                        cancelRequested.store(true, std::memory_order_relaxed);
                    }
                }
            };

            std::vector<std::thread> workers;
            workers.reserve(workerCount);
            for (size_t i = 0; i < workerCount; ++i)
            {
                workers.emplace_back(worker);
            }
            for (std::thread& thread : workers)
            {
                thread.join();
            }

            workDone = stageBaseWork + filesDone.load(std::memory_order_relaxed);
            return !result.canceled;
        }

        bool BuildDxpMetadataIndexParallel(
            const std::vector<const ResourceRecord*>& dxpRecords,
            const std::vector<std::vector<std::string>>& focusedTextureKeySets,
            DescriptorResourceLinkBuildResult& result,
            const DescriptorResourceLinkBuildOptions& options,
            std::vector<LightweightDxpEntry>& dxpEntries,
            std::unordered_map<std::string, std::vector<size_t>>& dxpIndex,
            size_t& workDone,
            size_t totalWork)
        {
            if (dxpRecords.empty())
            {
                return true;
            }

            const size_t stageBaseWork = workDone;
            const size_t workerCount = DescriptorIndexWorkerCount(dxpRecords.size());
            std::atomic<size_t> nextIndex{ 0 };
            std::atomic<size_t> filesDone{ 0 };
            std::atomic<bool> stopRequested{ false };
            std::atomic<bool> matchedFocusedTextures{ false };
            std::mutex stateMutex;

            auto worker = [&]() {
                while (!stopRequested.load(std::memory_order_relaxed))
                {
                    const size_t recordIndex = nextIndex.fetch_add(1, std::memory_order_relaxed);
                    if (recordIndex >= dxpRecords.size())
                    {
                        break;
                    }

                    const ResourceRecord& record = *dxpRecords[recordIndex];
                    const auto parseStart = std::chrono::steady_clock::now();
                    std::vector<LightweightDxpEntry> localEntries;
                    std::string error;
                    const bool parsed = ParseDxpMetadataForLinking(record.path, localEntries, error);
                    std::vector<std::vector<std::string>> localKeySets;
                    if (parsed)
                    {
                        localKeySets.reserve(localEntries.size());
                        for (LightweightDxpEntry& entry : localEntries)
                        {
                            entry.nameKeys = BuildAssetNameKeys(entry.name);
                            localKeySets.push_back(entry.nameKeys);
                        }
                    }
                    const long long elapsedMs = ElapsedMilliseconds(parseStart);

                    std::lock_guard<std::mutex> lock(stateMutex);
                    if (parsed)
                    {
                        const size_t before = dxpEntries.size();
                        dxpEntries.insert(
                            dxpEntries.end(),
                            std::make_move_iterator(localEntries.begin()),
                            std::make_move_iterator(localEntries.end()));
                        for (size_t i = before; i < dxpEntries.size(); ++i)
                        {
                            AddPrecomputedIndexKeys(dxpIndex, localKeySets[i - before], i);
                        }
                        ++result.dxpPacksParsed;
                    }
                    else if (!error.empty())
                    {
                        AddWarning(result, error);
                    }

                    const size_t completed = filesDone.fetch_add(1, std::memory_order_relaxed) + 1;
                    std::string item = Platform::PathToUtf8(record.path.filename());
                    if (parsed)
                    {
                        item += " indexed " + std::to_string(localEntries.size()) +
                            " textures in " + std::to_string(elapsedMs) +
                            " ms (" + std::to_string(dxpEntries.size()) + " total)";
                    }
                    else
                    {
                        item += " failed in " + std::to_string(elapsedMs) + " ms";
                    }

                    if (!ReportDescriptorProgress(
                            result,
                            options,
                            "DXP index",
                            std::move(item),
                            stageBaseWork + completed,
                            totalWork))
                    {
                        stopRequested.store(true, std::memory_order_relaxed);
                        continue;
                    }

                    if (!focusedTextureKeySets.empty() &&
                        !matchedFocusedTextures.load(std::memory_order_relaxed) &&
                        AllTextureKeySetsIndexed(focusedTextureKeySets, dxpIndex))
                    {
                        matchedFocusedTextures.store(true, std::memory_order_relaxed);
                        stopRequested.store(true, std::memory_order_relaxed);
                        if (!ReportDescriptorProgress(
                                result,
                                options,
                                "DXP index",
                                "Selected texture set matched after " +
                                    std::to_string(result.dxpPacksParsed) + " DXP pack" +
                                    (result.dxpPacksParsed == 1 ? "" : "s"),
                                stageBaseWork + completed,
                                totalWork))
                        {
                            result.canceled = true;
                        }
                    }
                }
            };

            std::vector<std::thread> workers;
            workers.reserve(workerCount);
            for (size_t i = 0; i < workerCount; ++i)
            {
                workers.emplace_back(worker);
            }
            for (std::thread& thread : workers)
            {
                thread.join();
            }

            workDone = stageBaseWork + filesDone.load(std::memory_order_relaxed);
            return !result.canceled;
        }

        struct CachedEntryKeys
        {
            std::vector<std::string> nameKeys;
            std::vector<std::string> dynModelKeys;
        };

        const LightweightGrpEntry* FindBestGrpEntry(
            const SubAssetRecord& descriptorEntry,
            const fs::path& descriptorPath,
            const std::vector<LightweightGrpEntry>& entries,
            const std::unordered_map<std::string, std::vector<size_t>>& index,
            const CachedEntryKeys& keys,
            bool descriptorIsDynModel,
            bool descriptorIsRendInst)
        {
            (void)descriptorEntry;
            std::vector<size_t> candidates;
            auto addCandidates = [&](const std::vector<std::string>& nameKeys) {
                for (const std::string& key : nameKeys)
                {
                    if (const auto it = index.find(key); it != index.end())
                    {
                        for (const size_t candidate : it->second)
                        {
                            if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
                            {
                                candidates.push_back(candidate);
                            }
                        }
                    }
                }
            };

            addCandidates(keys.nameKeys);
            if (!keys.dynModelKeys.empty())
            {
                addCandidates(keys.dynModelKeys);
            }

            const LightweightGrpEntry* best = nullptr;
            int bestScore = std::numeric_limits<int>::min();
            for (const size_t candidate : candidates)
            {
                if (candidate >= entries.size())
                {
                    continue;
                }

                const LightweightGrpEntry& entry = entries[candidate];
                int score = CommonPathScore(descriptorPath, entry.path);
                if (NameKeysIntersect(entry.nameKeys, keys.nameKeys))
                {
                    score += 100;
                }
                if (!keys.dynModelKeys.empty() && NameKeysIntersect(entry.nameKeys, keys.dynModelKeys))
                {
                    score += 120;
                }

                bool related = false;
                const std::string entryPackStem = PackStemKey(entry.path);
                for (const std::string& key : keys.nameKeys)
                {
                    if (IsNameRelatedToPackStem(key, entryPackStem))
                    {
                        related = true;
                        break;
                    }
                }
                if (!related)
                {
                    for (const std::string& key : keys.dynModelKeys)
                    {
                        if (IsNameRelatedToPackStem(key, entryPackStem))
                        {
                            related = true;
                            break;
                        }
                    }
                }
                if (related)
                {
                    score += 20;
                }

                if (PathLooksLikePatch(entry.path))
                {
                    score += 15;
                }
                if (descriptorIsDynModel && entry.className == "DynModel")
                {
                    score += 25;
                }
                if (descriptorIsRendInst && entry.className == "RendInst")
                {
                    score += 25;
                }

                if (score > bestScore)
                {
                    bestScore = score;
                    best = &entry;
                }
            }

            return best;
        }

        const LightweightDxpEntry* FindBestDxpEntry(
            const DescriptorResourceLink& link,
            std::string_view textureName,
            const std::vector<LightweightDxpEntry>& entries,
            const std::unordered_map<std::string, std::vector<size_t>>& index,
            const std::vector<std::string>& textureKeys)
        {
            std::vector<size_t> candidates;
            for (const std::string& key : textureKeys)
            {
                if (const auto it = index.find(key); it != index.end())
                {
                    for (const size_t candidate : it->second)
                    {
                        if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end())
                        {
                            candidates.push_back(candidate);
                        }
                    }
                }
            }

            const LightweightDxpEntry* best = nullptr;
            int bestQuality = std::numeric_limits<int>::min();
            int bestScore = std::numeric_limits<int>::min();
            for (const size_t candidate : candidates)
            {
                if (candidate >= entries.size())
                {
                    continue;
                }

                const LightweightDxpEntry& entry = entries[candidate];
                int score = 0;
                if (NameKeysIntersect(entry.nameKeys, textureKeys))
                {
                    score += 100;
                }
                if (link.matchedGrpPath)
                {
                    score += CommonPathScore(*link.matchedGrpPath, entry.path);
                    if (IsSameDirectory(entry.path, *link.matchedGrpPath))
                    {
                        score += 30;
                    }
                }
                score += CommonPathScore(link.descriptorPath, entry.path);
                if (PathLooksLikePatch(entry.path))
                {
                    score += 15;
                }
                if (IsPackRelatedToAnyName(entry.path, { std::string(textureName), link.resourceName }))
                {
                    score += 10;
                }

                const int quality = TextureQualityPriority(entry.path, entry.name);
                if (quality > bestQuality ||
                    (quality == bestQuality && score > bestScore))
                {
                    bestQuality = quality;
                    bestScore = score;
                    best = &entry;
                }
            }

            return best;
        }

        bool IsDescriptorCacheSourceType(AssetType type)
        {
            return type == AssetType::Descriptor ||
                   type == AssetType::Grp ||
                   type == AssetType::Dxp ||
                   type == AssetType::Blk;
        }

        std::vector<ResourceRecord> BuildDescriptorCacheSourceRecords(const std::vector<ResourceRecord>& records)
        {
            std::vector<ResourceRecord> sources;
            for (const ResourceRecord& record : records)
            {
                if (IsDescriptorCacheSourceType(record.type))
                {
                    sources.push_back(record);
                }
            }

            std::sort(sources.begin(), sources.end(), [](const ResourceRecord& left, const ResourceRecord& right) {
                const int leftType = static_cast<int>(left.type);
                const int rightType = static_cast<int>(right.type);
                if (leftType != rightType)
                {
                    return leftType < rightType;
                }
                return PathKey(left.path) < PathKey(right.path);
            });
            return sources;
        }

        bool SameDescriptorCacheSources(
            const std::vector<ResourceRecord>& left,
            const std::vector<ResourceRecord>& right)
        {
            if (left.size() != right.size())
            {
                return false;
            }

            for (size_t i = 0; i < left.size(); ++i)
            {
                if (!SamePath(left[i].path, right[i].path) ||
                    left[i].type != right[i].type ||
                    left[i].sizeBytes != right[i].sizeBytes ||
                    left[i].lastWriteTicks != right[i].lastWriteTicks)
                {
                    return false;
                }
            }

            return true;
        }

        bool ReadQuotedPath(std::stringstream& stream, fs::path& path)
        {
            std::string value;
            if (!(stream >> std::quoted(value)))
            {
                return false;
            }

            path = Platform::Utf8ToPath(value);
            return true;
        }

        void WriteOptionalString(std::ostream& stream, std::string_view tag, const std::optional<std::string>& value)
        {
            if (value)
            {
                stream << tag << ' ' << std::quoted(*value) << '\n';
            }
        }

        size_t AddCachePathRef(
            std::vector<fs::path>& paths,
            std::unordered_map<std::wstring, size_t>& index,
            const fs::path& path)
        {
            const std::wstring key = PathKey(path);
            if (const auto it = index.find(key); it != index.end())
            {
                return it->second;
            }

            const size_t id = paths.size();
            paths.push_back(path);
            index.emplace(key, id);
            return id;
        }

        std::optional<size_t> FindCachePathRef(
            const std::unordered_map<std::wstring, size_t>& index,
            const std::optional<fs::path>& path)
        {
            if (!path)
            {
                return std::nullopt;
            }

            const auto it = index.find(PathKey(*path));
            if (it == index.end())
            {
                return std::nullopt;
            }
            return it->second;
        }

        void WriteStringVectorRow(
            std::ostream& stream,
            std::string_view tag,
            const std::vector<std::string>& values)
        {
            stream << tag << ' ' << values.size();
            for (const std::string& value : values)
            {
                stream << ' ' << std::quoted(value);
            }
            stream << '\n';
        }

        bool ReadStringVectorRow(std::stringstream& stream, std::vector<std::string>& values)
        {
            size_t count = 0;
            if (!(stream >> count))
            {
                return false;
            }

            values.clear();
            values.reserve(count);
            for (size_t i = 0; i < count; ++i)
            {
                std::string value;
                if (!(stream >> std::quoted(value)))
                {
                    return false;
                }
                values.push_back(std::move(value));
            }
            return true;
        }

        bool ResolveCachePathRef(
            long long id,
            const std::vector<fs::path>& paths,
            std::optional<fs::path>& outPath)
        {
            if (id < 0)
            {
                outPath.reset();
                return true;
            }
            if (static_cast<uint64_t>(id) >= paths.size())
            {
                return false;
            }
            outPath = paths[static_cast<size_t>(id)];
            return true;
        }

        bool ReplaceCacheFileWithTemp(const fs::path& tempPath, const fs::path& cachePath, std::string& error)
        {
#if defined(_WIN32)
            if (MoveFileExW(
                    tempPath.wstring().c_str(),
                    cachePath.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
            {
                return true;
            }

            const std::error_code ec(static_cast<int>(GetLastError()), std::system_category());
            error = "Could not replace descriptor-link cache: " + ec.message();
            return false;
#else
            std::error_code ec;
            fs::rename(tempPath, cachePath, ec);
            if (!ec)
            {
                return true;
            }

            fs::remove(cachePath, ec);
            ec.clear();
            fs::rename(tempPath, cachePath, ec);
            if (ec)
            {
                error = "Could not replace descriptor-link cache: " + ec.message();
                return false;
            }
            return true;
#endif
        }
    }

    DescriptorResourceLinkBuildResult BuildDescriptorResourceLinks(
        const std::vector<ResourceRecord>& records,
        const DescriptorResourceLinkBuildOptions& options)
    {
        DescriptorResourceLinkBuildResult result;

        const ResourceRecord* focusRecord = FindFocusRecord(records, options.focusPath);

        std::vector<const ResourceRecord*> descriptorRecords;
        std::vector<const ResourceRecord*> allDescriptorRecords;
        std::vector<const ResourceRecord*> grpRecords;
        std::vector<const ResourceRecord*> dxpRecords;
        descriptorRecords.reserve(records.size());
        allDescriptorRecords.reserve(records.size());
        grpRecords.reserve(records.size());
        dxpRecords.reserve(records.size());

        for (const ResourceRecord& record : records)
        {
            if (record.type == AssetType::Descriptor)
            {
                allDescriptorRecords.push_back(&record);
                if (ShouldIncludeDescriptorForFocus(record, focusRecord))
                {
                    descriptorRecords.push_back(&record);
                }
            }
            else if (record.type == AssetType::Grp)
            {
                if (ShouldIncludeGrpForFocus(record, focusRecord))
                {
                    grpRecords.push_back(&record);
                }
            }
            else if (record.type == AssetType::Dxp)
            {
                if (ShouldIncludeDxpForFocus(record, focusRecord))
                {
                    dxpRecords.push_back(&record);
                }
            }
        }

        if (focusRecord != nullptr &&
            focusRecord->type != AssetType::Descriptor &&
            descriptorRecords.empty())
        {
            descriptorRecords = allDescriptorRecords;
            AddWarning(
                result,
                "Scoped descriptor root search found no descriptor files; fell back to all descriptors.");
        }

        std::vector<std::string> descriptorResourceNameFilter;
        if (focusRecord != nullptr &&
            focusRecord->type == AssetType::Grp &&
            !descriptorRecords.empty())
        {
            std::vector<LightweightGrpEntry> focusedGrpEntries;
            std::string focusedGrpError;
            if (ParseGrpMetadataForLinking(
                    focusRecord->path,
                    options.maxPackBytes,
                    focusedGrpEntries,
                    focusedGrpError))
            {
                bool hasDynModelEntries = false;
                bool hasRendInstEntries = false;
                for (const LightweightGrpEntry& entry : focusedGrpEntries)
                {
                    AppendUniqueString(descriptorResourceNameFilter, entry.name);
                    hasDynModelEntries = hasDynModelEntries || entry.className == "DynModel";
                    hasRendInstEntries = hasRendInstEntries || entry.className == "RendInst";
                }

                if (hasDynModelEntries != hasRendInstEntries)
                {
                    descriptorRecords.erase(
                        std::remove_if(
                            descriptorRecords.begin(),
                            descriptorRecords.end(),
                            [&](const ResourceRecord* record) {
                                if (hasDynModelEntries && DescriptorPathLooksLikeRiDesc(record->path))
                                {
                                    return true;
                                }
                                if (hasRendInstEntries && DescriptorPathLooksLikeDynModelDesc(record->path))
                                {
                                    return true;
                                }
                                return false;
                            }),
                        descriptorRecords.end());
                }
            }
        }

        const bool scopedBuild = focusRecord != nullptr;
        const size_t plannedDxpWork = scopedBuild && focusRecord->type != AssetType::Dxp
            ? std::min(dxpRecords.size(), ScopedDxpCandidateLimit)
            : dxpRecords.size();
        size_t totalWork =
            std::max<size_t>(1, descriptorRecords.size() + grpRecords.size() + plannedDxpWork + 2);
        size_t workDone = 0;

        if (!ReportDescriptorProgress(
                result,
                options,
                "Descriptor pass",
                "Parsing descriptor files",
                workDone,
                totalWork))
        {
            return result;
        }

        std::vector<std::pair<fs::path, SubAssetRecord>> descriptorEntries;
        for (const ResourceRecord* record : descriptorRecords)
        {
            if (record->sizeBytes > options.maxDescriptorBytes)
            {
                ++result.descriptorFilesSkipped;
                AddWarning(result, "Skipped large descriptor: " + Platform::PathToUtf8(record->path.filename()));
                ++workDone;
                if (!ReportDescriptorProgress(
                        result,
                        options,
                        "Descriptor pass",
                        Platform::PathToUtf8(record->path.filename()),
                        workDone,
                        totalWork))
                {
                    return result;
                }
                continue;
            }

            const std::shared_ptr<PackContents> descriptorContents =
                ParseDescriptorContents(
                    record->path,
                    static_cast<size_t>(options.maxDescriptorBytes),
                    descriptorResourceNameFilter.empty() ? nullptr : &descriptorResourceNameFilter);

            if (!descriptorContents)
            {
                ++result.parseErrors;
                AddWarning(result, "Could not parse descriptor: " + Platform::PathToUtf8(record->path.filename()));
            }
            else if (!descriptorContents->error.empty())
            {
                ++result.parseErrors;
                AddWarning(result, descriptorContents->error);
            }
            else
            {
                ++result.descriptorFilesParsed;
                for (const SubAssetRecord& entry : descriptorContents->entries)
                {
                    descriptorEntries.emplace_back(record->path, entry);
                }
            }

            ++workDone;
            if (!ReportDescriptorProgress(
                    result,
                    options,
                    "Descriptor pass",
                    Platform::PathToUtf8(record->path.filename()),
                    workDone,
                    totalWork))
            {
                return result;
            }
        }

        // 1. Pre-calculate GRP search keys for all descriptor entries to avoid redundant string normalization/allocation
        std::vector<CachedEntryKeys> descriptorKeys(descriptorEntries.size());
        for (size_t i = 0; i < descriptorEntries.size(); ++i)
        {
            descriptorKeys[i].nameKeys = BuildAssetNameKeys(descriptorEntries[i].second.name);
            if (!descriptorEntries[i].second.dynModelName.empty())
            {
                descriptorKeys[i].dynModelKeys = BuildAssetNameKeys(descriptorEntries[i].second.dynModelName);
            }
        }

        std::vector<LightweightGrpEntry> grpEntries;
        std::unordered_map<std::string, std::vector<size_t>> grpIndex;
        grpEntries.reserve(grpRecords.size() * 8);
        if (!ReportDescriptorProgress(result, options, "GRP index", "Indexing GRP resource names", workDone, totalWork))
        {
            return result;
        }

        if (!BuildGrpMetadataIndexParallel(
                grpRecords,
                options.maxPackBytes,
                result,
                options,
                grpEntries,
                grpIndex,
                workDone,
                totalWork))
        {
            return result;
        }

        std::vector<std::string> focusedResourceNames;
        std::vector<std::vector<std::string>> focusedResourceNameKeys;
        std::vector<std::string> focusedTextureNames;
        std::vector<std::vector<std::string>> focusedTextureNameKeys;

        auto appendUniqueResource = [](std::vector<std::string>& names, std::vector<std::vector<std::string>>& keysList, const std::string& name) {
            if (name.empty()) return;
            if (std::find(names.begin(), names.end(), name) == names.end()) {
                names.push_back(name);
                keysList.push_back(BuildAssetNameKeys(name));
            }
        };

        if (focusRecord != nullptr)
        {
            fs::path lastPath;
            bool descriptorIsDynModel = false;
            bool descriptorIsRendInst = false;
            for (size_t entryIndex = 0; entryIndex < descriptorEntries.size(); ++entryIndex)
            {
                const auto& [descriptorPath, descriptorEntry] = descriptorEntries[entryIndex];
                if (descriptorPath != lastPath)
                {
                    lastPath = descriptorPath;
                    descriptorIsDynModel = ContainsAscii(BasenameKey(Platform::PathToUtf8(descriptorPath.filename())), "dynmodeldesc");
                    descriptorIsRendInst = ContainsAscii(BasenameKey(Platform::PathToUtf8(descriptorPath.filename())), "ridesc");
                }

                bool matchesFocus = false;
                if (focusRecord->type == AssetType::Descriptor)
                {
                    matchesFocus = SamePath(descriptorPath, focusRecord->path);
                }
                else if (focusRecord->type == AssetType::Grp)
                {
                    if (const LightweightGrpEntry* grpMatch = FindBestGrpEntry(descriptorEntry, descriptorPath, grpEntries, grpIndex, descriptorKeys[entryIndex], descriptorIsDynModel, descriptorIsRendInst))
                    {
                        matchesFocus = SamePath(grpMatch->path, focusRecord->path);
                    }
                }

                if (!matchesFocus)
                {
                    continue;
                }

                appendUniqueResource(focusedResourceNames, focusedResourceNameKeys, descriptorEntry.name);
                appendUniqueResource(focusedResourceNames, focusedResourceNameKeys, descriptorEntry.dynModelName);
                for (const uint32_t textureIndex : BuildTextureIndices(descriptorEntry))
                {
                    if (textureIndex < descriptorEntry.textureNames.size())
                    {
                        appendUniqueResource(focusedTextureNames, focusedTextureNameKeys, descriptorEntry.textureNames[textureIndex]);
                    }
                }
            }
        }

        const std::vector<std::vector<std::string>> focusedTextureKeySets =
            BuildTextureKeySets(focusedTextureNames);
        if (focusRecord != nullptr && !focusedTextureNames.empty())
        {
            struct ScoredRecord
            {
                const ResourceRecord* record;
                int score;
                int quality;
            };
            std::vector<ScoredRecord> scoredRecords;
            scoredRecords.reserve(dxpRecords.size());
            for (const ResourceRecord* record : dxpRecords)
            {
                const std::string packStem = PackStemKey(record->path);
                const int score = ScoreDxpRecordForFocus(*record, *focusRecord, focusedResourceNameKeys, focusedTextureNameKeys, packStem);
                scoredRecords.push_back({
                    record,
                    score,
                    TextureQualityPriority(record->path, {}),
                });
            }

            std::stable_sort(scoredRecords.begin(), scoredRecords.end(), [&](const ScoredRecord& left, const ScoredRecord& right) {
                if (left.score != right.score)
                {
                    return left.score > right.score;
                }
                return left.quality > right.quality;
            });

            dxpRecords.clear();
            for (const auto& sr : scoredRecords)
            {
                dxpRecords.push_back(sr.record);
            }
            
            // To keep selected-asset builds fast, index only the path-ranked DXP packs most likely
            // to contain the descriptor texture set. Global cache builds remain exhaustive.
            if (dxpRecords.size() > ScopedDxpCandidateLimit)
            {
                dxpRecords.resize(ScopedDxpCandidateLimit);
            }

            totalWork = std::max<size_t>(1, descriptorRecords.size() + grpRecords.size() + dxpRecords.size() + 2);

            if (!ReportDescriptorProgress(
                    result,
                    options,
                    "Selected scope",
                    "targetResources=" + std::to_string(focusedResourceNames.size()) +
                        ", targetTextures=" + std::to_string(focusedTextureNames.size()) +
                        ", dxpCandidates=" + std::to_string(dxpRecords.size()),
                    workDone,
                    totalWork))
            {
                return result;
            }
        }
        else if (focusRecord != nullptr && focusRecord->type != AssetType::Dxp)
        {
            dxpRecords.clear();
            totalWork = std::max<size_t>(1, descriptorRecords.size() + grpRecords.size() + 2);
            if (!ReportDescriptorProgress(
                    result,
                    options,
                    "Selected scope",
                    "No descriptor texture references matched the selected asset; skipping DXP index",
                    workDone,
                    totalWork))
            {
                return result;
            }
        }

        std::vector<LightweightDxpEntry> dxpEntries;
        std::unordered_map<std::string, std::vector<size_t>> dxpIndex;
        dxpEntries.reserve(dxpRecords.size() * 32);
        if (!ReportDescriptorProgress(result, options, "DXP index", "Indexing DXP texture names", workDone, totalWork))
        {
            return result;
        }

        if (!BuildDxpMetadataIndexParallel(
                dxpRecords,
                focusedTextureKeySets,
                result,
                options,
                dxpEntries,
                dxpIndex,
                workDone,
                totalWork))
        {
            return result;
        }

        if (!ReportDescriptorProgress(
                result,
                options,
                "Linking",
                "Resolving descriptor resources to GRP/DXP indexes",
                workDone,
                totalWork))
        {
            return result;
        }

        // 2. Pre-calculate DXP search keys for all unique texture names referenced by descriptors
        std::unordered_map<std::string, std::vector<std::string>> textureNameKeys;
        for (const auto& [path, entry] : descriptorEntries)
        {
            for (const std::string& texName : entry.textureNames)
            {
                textureNameKeys.emplace(texName, std::vector<std::string>());
            }
        }
        for (auto& [texName, keys] : textureNameKeys)
        {
            keys = BuildAssetNameKeys(texName);
        }

        const size_t workerCount = DescriptorIndexWorkerCount(descriptorEntries.size());
        std::atomic<size_t> nextIndex{ 0 };
        std::atomic<size_t> linksDone{ 0 };
        std::mutex resultsMutex;

        std::vector<std::vector<DescriptorResourceLink>> threadLinks(workerCount);
        std::vector<std::vector<std::string>> threadWarnings(workerCount);
        std::atomic<bool> linkingCancelRequested{ false };

        auto linkingWorker = [&](size_t threadId) {
            std::vector<DescriptorResourceLink>& localLinks = threadLinks[threadId];
            std::vector<std::string>& localWarnings = threadWarnings[threadId];

            fs::path lastPath;
            bool descriptorIsDynModel = false;
            bool descriptorIsRendInst = false;

            while (!linkingCancelRequested.load(std::memory_order_relaxed))
            {
                const size_t entryIndex = nextIndex.fetch_add(1, std::memory_order_relaxed);
                if (entryIndex >= descriptorEntries.size())
                {
                    break;
                }

                const auto& [descriptorPath, descriptorEntry] = descriptorEntries[entryIndex];
                if (descriptorPath != lastPath)
                {
                    lastPath = descriptorPath;
                    descriptorIsDynModel = ContainsAscii(BasenameKey(Platform::PathToUtf8(descriptorPath.filename())), "dynmodeldesc");
                    descriptorIsRendInst = ContainsAscii(BasenameKey(Platform::PathToUtf8(descriptorPath.filename())), "ridesc");
                }

                DescriptorResourceLink link;
                link.descriptorPath = descriptorPath;
                link.resourceName = descriptorEntry.name;
                link.resId = descriptorEntry.resId;
                link.textureNames = descriptorEntry.textureNames;
                link.materialNames = descriptorEntry.materialNames;
                link.materialDomains = descriptorEntry.materialDomains;
                link.materialShaderClasses = descriptorEntry.materialShaderClasses;
                link.materialTextureRefs = descriptorEntry.dependencyIds;
                link.materialTextureBindings = descriptorEntry.materialTextureRefs;

                if (const LightweightGrpEntry* grpMatch = FindBestGrpEntry(descriptorEntry, descriptorPath, grpEntries, grpIndex, descriptorKeys[entryIndex], descriptorIsDynModel, descriptorIsRendInst))
                {
                    link.matchedGrpPath = grpMatch->path;
                    link.matchedGrpEntryName = grpMatch->name;
                    link.matchedGrpClassId = grpMatch->classId;
                    link.matchedGrpClassName = grpMatch->className;
                    link.matchedGrpEntryCanExportStaticModel = grpMatch->canExportStaticModel;
                    link.matchedGrpEntryCanExportSkinnedModel = grpMatch->canExportSkinnedModel;
                }

                for (const uint32_t textureIndex : BuildTextureIndices(descriptorEntry))
                {
                    if (textureIndex >= descriptorEntry.textureNames.size())
                    {
                        std::ostringstream warning;
                        warning << "Descriptor " << Platform::PathToUtf8(descriptorPath.filename())
                                << " resource " << descriptorEntry.name
                                << " references missing texture index " << textureIndex << '.';
                        localWarnings.push_back(warning.str());
                        continue;
                    }

                    DescriptorTextureLink textureLink;
                    textureLink.textureIndex = textureIndex;
                    textureLink.textureName = descriptorEntry.textureNames[textureIndex];

                    const auto keysIt = textureNameKeys.find(textureLink.textureName);
                    std::vector<std::string> fallbackTextureKeys;
                    const std::vector<std::string>* textureKeys =
                        keysIt != textureNameKeys.end() ? &keysIt->second : nullptr;
                    if (textureKeys == nullptr)
                    {
                        fallbackTextureKeys = BuildAssetNameKeys(textureLink.textureName);
                        textureKeys = &fallbackTextureKeys;
                    }

                    if (const LightweightDxpEntry* dxpMatch = FindBestDxpEntry(link, textureLink.textureName, dxpEntries, dxpIndex, *textureKeys))
                    {
                        textureLink.matchedDxpPath = dxpMatch->path;
                        textureLink.matchedDxpEntryName = dxpMatch->name;
                    }

                    link.textureLinks.push_back(std::move(textureLink));
                }

                if (!LinkMatchesFocus(link, focusRecord))
                {
                    continue;
                }

                localLinks.push_back(std::move(link));

                const size_t completed = linksDone.fetch_add(1, std::memory_order_relaxed) + 1;
                if (completed % 250 == 0)
                {
                    if (!ReportDescriptorProgress(
                            result,
                            options,
                            "Linking",
                            std::to_string(completed) + " descriptor resources linked",
                            workDone,
                            totalWork))
                    {
                        linkingCancelRequested.store(true, std::memory_order_relaxed);
                    }
                }
            }
        };

        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (size_t i = 0; i < workerCount; ++i)
        {
            workers.emplace_back(linkingWorker, i);
        }
        for (std::thread& thread : workers)
        {
            thread.join();
        }

        if (linkingCancelRequested.load())
        {
            result.canceled = true;
            return result;
        }

        for (size_t i = 0; i < workerCount; ++i)
        {
            result.links.insert(
                result.links.end(),
                std::make_move_iterator(threadLinks[i].begin()),
                std::make_move_iterator(threadLinks[i].end()));

            for (const std::string& warning : threadWarnings[i])
            {
                AddWarning(result, warning);
            }
        }

        ++workDone;
        ReportDescriptorProgress(
            result,
            options,
            "Complete",
            "Descriptor relationships built",
            totalWork,
            totalWork);

        return result;
    }

    DescriptorResourceLinkBuildResult BuildDescriptorResourceLinks(
        const std::vector<ResourceRecord>& records,
        const std::vector<BlkResourceLink>* blkLinks,
        uintmax_t maxDescriptorBytes,
        uintmax_t maxPackBytes)
    {
        DescriptorResourceLinkBuildOptions options;
        options.blkLinks = blkLinks;
        options.maxDescriptorBytes = maxDescriptorBytes;
        options.maxPackBytes = maxPackBytes;
        return BuildDescriptorResourceLinks(records, options);
    }

    bool SaveDescriptorResourceLinkCache(
        const fs::path& cachePath,
        const fs::path& gameRoot,
        const std::vector<ResourceRecord>& records,
        const std::vector<DescriptorResourceLink>& links,
        const DescriptorResourceLinkBuildOptions& options,
        std::string& error)
    {
        std::error_code ec;
        fs::create_directories(cachePath.parent_path(), ec);
        if (ec)
        {
            error = "Could not create descriptor-link cache folder: " + ec.message();
            return false;
        }

        fs::path tempPath = cachePath;
        tempPath += ".tmp";

        std::ofstream file(tempPath, std::ios::trunc);
        if (!file)
        {
            error = "Could not open descriptor-link cache for writing.";
            return false;
        }

        file << "DagorAssetsExtractorDescriptorLinksCache " << DescriptorLinkCacheVersion << '\n';
        file << "root " << std::quoted(Platform::PathToUtf8(gameRoot)) << '\n';
        file << "linkerVersion " << DescriptorLinkerVersion << '\n';
        file << "maxDescriptorBytes " << options.maxDescriptorBytes << '\n';
        file << "maxPackBytes " << options.maxPackBytes << '\n';

        const std::vector<ResourceRecord> sources = BuildDescriptorCacheSourceRecords(records);
        file << "sourceCount " << sources.size() << '\n';
        for (const ResourceRecord& source : sources)
        {
            file << "source "
                 << std::quoted(Platform::PathToUtf8(source.path)) << ' '
                 << AssetTypeCacheName(source.type) << ' '
                 << source.sizeBytes << ' '
                 << source.lastWriteTicks << '\n';
        }

        std::vector<fs::path> pathRefs;
        std::unordered_map<std::wstring, size_t> pathRefIndex;
        for (const DescriptorResourceLink& link : links)
        {
            AddCachePathRef(pathRefs, pathRefIndex, link.descriptorPath);
            if (link.matchedGrpPath)
            {
                AddCachePathRef(pathRefs, pathRefIndex, *link.matchedGrpPath);
            }
            for (const DescriptorTextureLink& textureLink : link.textureLinks)
            {
                if (textureLink.matchedDxpPath)
                {
                    AddCachePathRef(pathRefs, pathRefIndex, *textureLink.matchedDxpPath);
                }
            }
        }

        file << "pathRefCount " << pathRefs.size() << '\n';
        for (size_t i = 0; i < pathRefs.size(); ++i)
        {
            file << "pathRef " << i << ' ' << std::quoted(Platform::PathToUtf8(pathRefs[i])) << '\n';
        }

        file << "linkCount " << links.size() << '\n';
        for (const DescriptorResourceLink& link : links)
        {
            file << "beginLink\n";
            file << "descriptorPathRef " << AddCachePathRef(pathRefs, pathRefIndex, link.descriptorPath) << '\n';
            file << "resourceName " << std::quoted(link.resourceName) << '\n';
            file << "resId " << link.resId << '\n';
            WriteStringVectorRow(file, "materialNames", link.materialNames);
            WriteStringVectorRow(file, "materialDomains", link.materialDomains);
            WriteStringVectorRow(file, "materialShaderClasses", link.materialShaderClasses);
            file << "materialTextureBindings " << link.materialTextureBindings.size();
            for (const DescriptorMaterialTextureRef& binding : link.materialTextureBindings)
            {
                file << ' ' << binding.materialIndex
                     << ' ' << binding.textureIndex
                     << ' ' << std::quoted(binding.slotName);
            }
            file << '\n';
            const std::optional<size_t> matchedGrpPathRef = FindCachePathRef(pathRefIndex, link.matchedGrpPath);
            file << "matchedGrpPathRef "
                 << (matchedGrpPathRef ? std::to_string(*matchedGrpPathRef) : std::string("-1"))
                 << '\n';
            WriteOptionalString(file, "matchedGrpEntryName", link.matchedGrpEntryName);
            file << "matchedGrpClassId " << link.matchedGrpClassId << '\n';
            file << "matchedGrpClassName " << std::quoted(link.matchedGrpClassName) << '\n';
            file << "matchedGrpCanExportStatic " << (link.matchedGrpEntryCanExportStaticModel ? 1 : 0) << '\n';
            file << "matchedGrpCanExportSkinned " << (link.matchedGrpEntryCanExportSkinnedModel ? 1 : 0) << '\n';

            file << "textureLinks " << link.textureLinks.size();
            for (const DescriptorTextureLink& textureLink : link.textureLinks)
            {
                const std::optional<size_t> dxpPathRef = FindCachePathRef(pathRefIndex, textureLink.matchedDxpPath);
                file << ' ' << textureLink.textureIndex
                     << ' ' << std::quoted(textureLink.textureName)
                     << ' ' << (dxpPathRef ? std::to_string(*dxpPathRef) : std::string("-1"))
                     << ' ' << std::quoted(textureLink.matchedDxpEntryName.value_or(std::string()));
            }
            file << '\n';

            file << "endLink\n";
        }

        if (!file)
        {
            error = "Could not finish writing descriptor-link cache.";
            return false;
        }

        file.close();

        if (!ReplaceCacheFileWithTemp(tempPath, cachePath, error))
        {
            return false;
        }

        return true;
    }

    DescriptorResourceLinkCacheLoadResult LoadDescriptorResourceLinkCache(
        const fs::path& cachePath,
        const fs::path& gameRoot,
        const std::vector<ResourceRecord>& records,
        const DescriptorResourceLinkBuildOptions& options)
    {
        DescriptorResourceLinkCacheLoadResult result;
        std::ifstream file(cachePath);
        if (!file)
        {
            result.message = "No descriptor-link cache file found.";
            return result;
        }

        result.fileFound = true;

        std::string magic;
        int cacheVersion = 0;
        if (!(file >> magic >> cacheVersion) ||
            magic != "DagorAssetsExtractorDescriptorLinksCache" ||
            cacheVersion != DescriptorLinkCacheVersion)
        {
            result.message = "Descriptor-link cache has an unsupported format.";
            return result;
        }

        fs::path cachedRoot;
        int cachedLinkerVersion = 0;
        uintmax_t cachedMaxDescriptorBytes = 0;
        uintmax_t cachedMaxPackBytes = 0;
        size_t expectedSourceCount = 0;
        size_t expectedLinkCount = 0;
        std::vector<ResourceRecord> cachedSources;
        std::vector<fs::path> pathRefs;
        std::vector<DescriptorResourceLink> loadedLinks;

        std::string line;
        while (std::getline(file, line))
        {
            if (line.empty())
            {
                continue;
            }

            std::stringstream stream(line);
            std::string tag;
            if (!(stream >> tag))
            {
                continue;
            }

            if (tag == "root")
            {
                if (!ReadQuotedPath(stream, cachedRoot))
                {
                    result.message = "Descriptor-link cache root is malformed.";
                    return result;
                }
            }
            else if (tag == "linkerVersion")
            {
                stream >> cachedLinkerVersion;
            }
            else if (tag == "maxDescriptorBytes")
            {
                stream >> cachedMaxDescriptorBytes;
            }
            else if (tag == "maxPackBytes")
            {
                stream >> cachedMaxPackBytes;
            }
            else if (tag == "sourceCount")
            {
                stream >> expectedSourceCount;
                cachedSources.reserve(expectedSourceCount);
            }
            else if (tag == "source")
            {
                std::string pathUtf8;
                std::string typeName;
                ResourceRecord source;
                if (!(stream >> std::quoted(pathUtf8) >> typeName >> source.sizeBytes >> source.lastWriteTicks))
                {
                    result.message = "Descriptor-link cache source row is malformed.";
                    return result;
                }

                const std::optional<AssetType> type = ParseAssetType(typeName);
                if (!type)
                {
                    result.message = "Descriptor-link cache source row has an unknown type.";
                    return result;
                }

                source.path = Platform::Utf8ToPath(pathUtf8);
                source.type = *type;
                cachedSources.push_back(std::move(source));
            }
            else if (tag == "pathRefCount")
            {
                size_t pathRefCount = 0;
                stream >> pathRefCount;
                pathRefs.reserve(pathRefCount);
            }
            else if (tag == "pathRef")
            {
                size_t id = 0;
                fs::path path;
                if (!(stream >> id) || !ReadQuotedPath(stream, path))
                {
                    result.message = "Descriptor-link cache path-ref row is malformed.";
                    return result;
                }
                if (id > pathRefs.size())
                {
                    result.message = "Descriptor-link cache path-ref table is sparse.";
                    return result;
                }
                if (id == pathRefs.size())
                {
                    pathRefs.push_back(std::move(path));
                }
                else
                {
                    pathRefs[id] = std::move(path);
                }
            }
            else if (tag == "linkCount")
            {
                stream >> expectedLinkCount;
                break;
            }
        }

        if (cachedRoot.empty())
        {
            result.message = "Descriptor-link cache is missing its game root.";
            return result;
        }

        result.rootMatched = SamePath(cachedRoot, gameRoot);
        if (!result.rootMatched)
        {
            result.message = "Descriptor-link cache belongs to another game folder.";
            return result;
        }

        result.optionsMatched =
            cachedLinkerVersion == DescriptorLinkerVersion &&
            cachedMaxDescriptorBytes == options.maxDescriptorBytes &&
            cachedMaxPackBytes == options.maxPackBytes;
        if (!result.optionsMatched)
        {
            result.message = "Descriptor-link cache was built by an older linker or incompatible limits.";
            return result;
        }

        if (expectedSourceCount != cachedSources.size())
        {
            result.message = "Descriptor-link cache source table is truncated.";
            return result;
        }

        const std::vector<ResourceRecord> expectedSources = BuildDescriptorCacheSourceRecords(records);
        std::sort(cachedSources.begin(), cachedSources.end(), [](const ResourceRecord& left, const ResourceRecord& right) {
            const int leftType = static_cast<int>(left.type);
            const int rightType = static_cast<int>(right.type);
            if (leftType != rightType)
            {
                return leftType < rightType;
            }
            return PathKey(left.path) < PathKey(right.path);
        });

        result.sourcesMatched = SameDescriptorCacheSources(cachedSources, expectedSources);
        if (!result.sourcesMatched)
        {
            result.message = "Descriptor-link cache is stale because resource records changed.";
            return result;
        }

        loadedLinks.reserve(expectedLinkCount);

        while (loadedLinks.size() < expectedLinkCount && std::getline(file, line))
        {
            if (line.empty())
            {
                continue;
            }

            std::stringstream stream(line);
            std::string tag;
            if (!(stream >> tag))
            {
                continue;
            }

            if (tag == "beginLink")
            {
                DescriptorResourceLink link;
                bool ended = false;
                while (std::getline(file, line))
                {
                    if (line.empty())
                    {
                        continue;
                    }

                    std::stringstream linkStream(line);
                    std::string linkTag;
                    if (!(linkStream >> linkTag))
                    {
                        continue;
                    }

                    if (linkTag == "endLink")
                    {
                        ended = true;
                        break;
                    }
                    if (linkTag == "descriptorPathRef")
                    {
                        long long pathRef = -1;
                        std::optional<fs::path> descriptorPath;
                        if (!(linkStream >> pathRef) ||
                            !ResolveCachePathRef(pathRef, pathRefs, descriptorPath) ||
                            !descriptorPath)
                        {
                            result.message = "Descriptor-link cache link descriptor path is malformed.";
                            return result;
                        }
                        link.descriptorPath = std::move(*descriptorPath);
                    }
                    else if (linkTag == "resourceName")
                    {
                        linkStream >> std::quoted(link.resourceName);
                    }
                    else if (linkTag == "resId")
                    {
                        linkStream >> link.resId;
                    }
                    else if (linkTag == "materialNames")
                    {
                        if (!ReadStringVectorRow(linkStream, link.materialNames))
                        {
                            result.message = "Descriptor-link cache material name row is malformed.";
                            return result;
                        }
                    }
                    else if (linkTag == "materialDomains")
                    {
                        if (!ReadStringVectorRow(linkStream, link.materialDomains))
                        {
                            result.message = "Descriptor-link cache material domain row is malformed.";
                            return result;
                        }
                    }
                    else if (linkTag == "materialShaderClasses")
                    {
                        if (!ReadStringVectorRow(linkStream, link.materialShaderClasses))
                        {
                            result.message = "Descriptor-link cache material shader-class row is malformed.";
                            return result;
                        }
                    }
                    else if (linkTag == "materialTextureBindings")
                    {
                        size_t bindingCount = 0;
                        if (!(linkStream >> bindingCount))
                        {
                            result.message = "Descriptor-link cache material texture binding row is malformed.";
                            return result;
                        }
                        link.materialTextureBindings.reserve(bindingCount);
                        for (size_t i = 0; i < bindingCount; ++i)
                        {
                            DescriptorMaterialTextureRef ref;
                            if (!(linkStream >> ref.materialIndex >> ref.textureIndex >> std::quoted(ref.slotName)))
                            {
                                result.message = "Descriptor-link cache material texture binding row is malformed.";
                                return result;
                            }
                            link.materialTextureBindings.push_back(std::move(ref));
                        }
                    }
                    else if (linkTag == "matchedGrpPathRef")
                    {
                        long long pathRef = -1;
                        if (!(linkStream >> pathRef) ||
                            !ResolveCachePathRef(pathRef, pathRefs, link.matchedGrpPath))
                        {
                            result.message = "Descriptor-link cache matched GRP path is malformed.";
                            return result;
                        }
                    }
                    else if (linkTag == "matchedGrpEntryName")
                    {
                        std::string value;
                        if (linkStream >> std::quoted(value))
                        {
                            link.matchedGrpEntryName = std::move(value);
                        }
                    }
                    else if (linkTag == "matchedGrpClassId")
                    {
                        linkStream >> link.matchedGrpClassId;
                    }
                    else if (linkTag == "matchedGrpClassName")
                    {
                        linkStream >> std::quoted(link.matchedGrpClassName);
                    }
                    else if (linkTag == "matchedGrpCanExportStatic")
                    {
                        int value = 0;
                        linkStream >> value;
                        link.matchedGrpEntryCanExportStaticModel = value != 0;
                    }
                    else if (linkTag == "matchedGrpCanExportSkinned")
                    {
                        int value = 0;
                        linkStream >> value;
                        link.matchedGrpEntryCanExportSkinnedModel = value != 0;
                    }
                    else if (linkTag == "textureLinks")
                    {
                        size_t textureLinkCount = 0;
                        if (!(linkStream >> textureLinkCount))
                        {
                            result.message = "Descriptor-link cache texture link row is malformed.";
                            return result;
                        }
                        link.textureLinks.reserve(textureLinkCount);
                        for (size_t i = 0; i < textureLinkCount; ++i)
                        {
                            DescriptorTextureLink textureLink;
                            long long dxpPathRef = -1;
                            std::string dxpEntryName;
                            if (!(linkStream >> textureLink.textureIndex >>
                                      std::quoted(textureLink.textureName) >>
                                      dxpPathRef >>
                                      std::quoted(dxpEntryName)) ||
                                !ResolveCachePathRef(dxpPathRef, pathRefs, textureLink.matchedDxpPath))
                            {
                                result.message = "Descriptor-link cache texture link row is malformed.";
                                return result;
                            }

                            if (!dxpEntryName.empty())
                            {
                                textureLink.matchedDxpEntryName = std::move(dxpEntryName);
                            }
                            if (textureLink.textureIndex < 100000)
                            {
                                if (textureLink.textureIndex >= link.textureNames.size())
                                {
                                    link.textureNames.resize(static_cast<size_t>(textureLink.textureIndex) + 1);
                                }
                                link.textureNames[textureLink.textureIndex] = textureLink.textureName;
                            }
                            link.textureLinks.push_back(std::move(textureLink));
                        }
                    }
                }

                if (!ended)
                {
                    result.message = "Descriptor-link cache link block is truncated.";
                    return result;
                }
                if (link.descriptorPath.empty() || link.resourceName.empty())
                {
                    result.message = "Descriptor-link cache link block is missing required fields.";
                    return result;
                }

                loadedLinks.push_back(std::move(link));
            }
        }
        if (expectedLinkCount != loadedLinks.size())
        {
            result.message = "Descriptor-link cache link table is truncated.";
            return result;
        }

        result.links = std::move(loadedLinks);
        result.loaded = true;
        result.message = "Loaded " + std::to_string(result.links.size()) + " cached descriptor links.";
        return result;
    }
}
