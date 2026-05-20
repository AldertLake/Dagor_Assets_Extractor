#include "AssetIndex/DependencyGraph.h"

#include "AssetIndex/PackContents.h"
#include "Platform/Platform.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace DagorAssetsExtractor::AssetIndex
{
    namespace fs = std::filesystem;

    namespace
    {
        constexpr size_t MaxWarnings = 12;
        constexpr int DescriptorLinkerVersion = 6;
        constexpr int DescriptorLinkCacheVersion = 3;

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
            while (!value.empty() && value.back() == '*')
            {
                value.pop_back();
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

        bool NamesMatch(std::string_view left, std::string_view right)
        {
            const std::vector<std::string> leftKeys = BuildAssetNameKeys(left);
            const std::vector<std::string> rightKeys = BuildAssetNameKeys(right);
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

            // For selected GRP/DXP packs, descriptors are the only source that
            // can tell us which resource owns that pack, so keep descriptor pass global.
            return true;
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
                return IsRecordRelatedToFocus(record, *focus);
            }

            return true;
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
            uint32_t classId = 0;
            std::string className;
            bool canExportStaticModel = false;
            bool canExportSkinnedModel = false;
        };

        struct LightweightDxpEntry
        {
            fs::path path;
            std::string name;
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

            const std::shared_ptr<PackContents> contents = ParseGrpContents(path, static_cast<size_t>(maxPackBytes));
            if (!contents || !contents->error.empty())
            {
                error = contents ? contents->error : ("Could not parse GRP: " + Platform::PathToUtf8(path.filename()));
                return false;
            }

            for (const SubAssetRecord& subAsset : contents->entries)
            {
                LightweightGrpEntry entry;
                entry.path = path;
                entry.name = subAsset.name;
                entry.classId = subAsset.grpClassId;
                entry.className = subAsset.formatInfo;
                entry.canExportStaticModel = CanExportGrpEntryAsStaticModel(subAsset);
                entry.canExportSkinnedModel = CanExportGrpEntryAsSkinnedModel(subAsset);
                entries.push_back(std::move(entry));
            }

            return true;
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
                std::vector<std::string> names;
                bool incomplete = false;
                if (TryReadDxpNamesFromBuffer(data, totalFiles, names, incomplete))
                {
                    for (std::string& name : names)
                    {
                        LightweightDxpEntry entry;
                        entry.path = path;
                        entry.name = std::move(name);
                        entries.push_back(std::move(entry));
                    }
                    return true;
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

        bool PathLooksLikePatch(const fs::path& path)
        {
            return ContainsAscii(FullPathKey(Platform::PathToUtf8(path)), "/patch/");
        }

        bool PathLooksLikeHq(const fs::path& path)
        {
            const std::string key = FullPathKey(Platform::PathToUtf8(path));
            return ContainsAscii(key, "content.hq") || ContainsAscii(key, "/hq_tex/");
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

        const LightweightGrpEntry* FindBestGrpEntry(
            const SubAssetRecord& descriptorEntry,
            const fs::path& descriptorPath,
            const std::vector<LightweightGrpEntry>& entries,
            const std::unordered_map<std::string, std::vector<size_t>>& index)
        {
            std::vector<size_t> candidates;
            auto addCandidates = [&](std::string_view name) {
                for (const std::string& key : BuildAssetNameKeys(name))
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

            addCandidates(descriptorEntry.name);
            if (!descriptorEntry.dynModelName.empty())
            {
                addCandidates(descriptorEntry.dynModelName);
            }

            const bool descriptorIsDynModel =
                ContainsAscii(BasenameKey(Platform::PathToUtf8(descriptorPath.filename())), "dynmodeldesc");
            const bool descriptorIsRendInst =
                ContainsAscii(BasenameKey(Platform::PathToUtf8(descriptorPath.filename())), "ridesc");

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
                if (NamesMatch(entry.name, descriptorEntry.name))
                {
                    score += 100;
                }
                if (!descriptorEntry.dynModelName.empty() && NamesMatch(entry.name, descriptorEntry.dynModelName))
                {
                    score += 120;
                }
                if (IsPackRelatedToAnyName(entry.path, { descriptorEntry.name, descriptorEntry.dynModelName }))
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
            const std::unordered_map<std::string, std::vector<size_t>>& index)
        {
            std::vector<size_t> candidates;
            for (const std::string& key : BuildAssetNameKeys(textureName))
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
            int bestScore = std::numeric_limits<int>::min();
            for (const size_t candidate : candidates)
            {
                if (candidate >= entries.size())
                {
                    continue;
                }

                const LightweightDxpEntry& entry = entries[candidate];
                int score = 0;
                if (NamesMatch(entry.name, textureName))
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
                if (PathLooksLikeHq(entry.path))
                {
                    score += 5;
                }
                if (ContainsAscii(BasenameKey(entry.name), "$hq"))
                {
                    score += 5;
                }
                if (IsPackRelatedToAnyName(entry.path, { std::string(textureName), link.resourceName }))
                {
                    score += 10;
                }

                if (score > bestScore)
                {
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

        void WriteOptionalPath(std::ostream& stream, std::string_view tag, const std::optional<fs::path>& path)
        {
            if (path)
            {
                stream << tag << ' ' << std::quoted(Platform::PathToUtf8(*path)) << '\n';
            }
        }

        void WriteOptionalString(std::ostream& stream, std::string_view tag, const std::optional<std::string>& value)
        {
            if (value)
            {
                stream << tag << ' ' << std::quoted(*value) << '\n';
            }
        }
    }

    DescriptorResourceLinkBuildResult BuildDescriptorResourceLinks(
        const std::vector<ResourceRecord>& records,
        const DescriptorResourceLinkBuildOptions& options)
    {
        DescriptorResourceLinkBuildResult result;

        std::vector<BlkResourceLink> builtBlkLinks;
        if (options.blkLinks == nullptr)
        {
            if (!ReportDescriptorProgress(result, options, "BLK relationships", "Parsing BLK resource-pack hints", 0, 0))
            {
                return result;
            }

            BlkResourceLinkBuildResult blkResult = BuildBlkResourceLinks(records);
            builtBlkLinks = std::move(blkResult.links);
            for (const std::string& warning : blkResult.warnings)
            {
                AddWarning(result, "BLK relationship warning: " + warning);
            }
        }

        const std::vector<BlkResourceLink>* blkLinks = options.blkLinks ? options.blkLinks : &builtBlkLinks;
        const std::vector<DescriptorPackGroup> packGroups = BuildDescriptorPackGroups(*blkLinks);
        (void)packGroups;
        const ResourceRecord* focusRecord = FindFocusRecord(records, options.focusPath);

        std::vector<const ResourceRecord*> descriptorRecords;
        std::vector<const ResourceRecord*> grpRecords;
        std::vector<const ResourceRecord*> dxpRecords;
        descriptorRecords.reserve(records.size());
        grpRecords.reserve(records.size());
        dxpRecords.reserve(records.size());

        for (const ResourceRecord& record : records)
        {
            if (record.type == AssetType::Descriptor)
            {
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

        const size_t totalWork =
            std::max<size_t>(1, descriptorRecords.size() + grpRecords.size() + dxpRecords.size() + 1);
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
                ParseDescriptorContents(record->path, static_cast<size_t>(options.maxDescriptorBytes));

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

        std::vector<LightweightGrpEntry> grpEntries;
        std::unordered_map<std::string, std::vector<size_t>> grpIndex;
        grpEntries.reserve(grpRecords.size() * 8);
        if (!ReportDescriptorProgress(result, options, "GRP index", "Indexing GRP resource names", workDone, totalWork))
        {
            return result;
        }

        for (const ResourceRecord* record : grpRecords)
        {
            const size_t before = grpEntries.size();
            std::string error;
            if (ParseGrpMetadataForLinking(record->path, options.maxPackBytes, grpEntries, error))
            {
                ++result.grpPacksParsed;
                for (size_t i = before; i < grpEntries.size(); ++i)
                {
                    AddIndexKeys<LightweightGrpEntry>(grpIndex, grpEntries[i].name, i);
                }
            }
            else
            {
                AddWarning(result, error);
            }

            ++workDone;
            if (!ReportDescriptorProgress(
                    result,
                    options,
                    "GRP index",
                    Platform::PathToUtf8(record->path.filename()) + " (" +
                        std::to_string(grpEntries.size()) + " resources indexed)",
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

        for (const ResourceRecord* record : dxpRecords)
        {
            const size_t before = dxpEntries.size();
            std::string error;
            if (ParseDxpMetadataForLinking(record->path, dxpEntries, error))
            {
                ++result.dxpPacksParsed;
                for (size_t i = before; i < dxpEntries.size(); ++i)
                {
                    AddIndexKeys<LightweightDxpEntry>(dxpIndex, dxpEntries[i].name, i);
                }
            }
            else
            {
                AddWarning(result, error);
            }

            ++workDone;
            if (!ReportDescriptorProgress(
                    result,
                    options,
                    "DXP index",
                    Platform::PathToUtf8(record->path.filename()) + " (" +
                        std::to_string(dxpEntries.size()) + " textures indexed)",
                    workDone,
                    totalWork))
            {
                return result;
            }
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

        for (const auto& [descriptorPath, descriptorEntry] : descriptorEntries)
        {
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

            if (const LightweightGrpEntry* grpMatch = FindBestGrpEntry(descriptorEntry, descriptorPath, grpEntries, grpIndex))
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
                    AddWarning(result, warning.str());
                    continue;
                }

                DescriptorTextureLink textureLink;
                textureLink.textureIndex = textureIndex;
                textureLink.textureName = descriptorEntry.textureNames[textureIndex];

                if (const LightweightDxpEntry* dxpMatch = FindBestDxpEntry(link, textureLink.textureName, dxpEntries, dxpIndex))
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

            result.links.push_back(std::move(link));
            if (result.links.size() % 250 == 0)
            {
                if (!ReportDescriptorProgress(
                        result,
                        options,
                        "Linking",
                        std::to_string(result.links.size()) + " descriptor resources linked",
                        workDone,
                        totalWork))
                {
                    return result;
                }
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

        std::ofstream file(cachePath, std::ios::trunc);
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

        file << "linkCount " << links.size() << '\n';
        for (const DescriptorResourceLink& link : links)
        {
            file << "beginLink\n";
            file << "descriptorPath " << std::quoted(Platform::PathToUtf8(link.descriptorPath)) << '\n';
            file << "resourceName " << std::quoted(link.resourceName) << '\n';
            file << "resId " << link.resId << '\n';
            for (const std::string& textureName : link.textureNames)
            {
                file << "textureName " << std::quoted(textureName) << '\n';
            }
            for (const std::string& materialName : link.materialNames)
            {
                file << "materialName " << std::quoted(materialName) << '\n';
            }
            for (const std::string& materialDomain : link.materialDomains)
            {
                file << "materialDomain " << std::quoted(materialDomain) << '\n';
            }
            for (const std::string& materialShaderClass : link.materialShaderClasses)
            {
                file << "materialShaderClass " << std::quoted(materialShaderClass) << '\n';
            }
            for (const uint32_t materialTextureRef : link.materialTextureRefs)
            {
                file << "materialTextureRef " << materialTextureRef << '\n';
            }
            for (const DescriptorMaterialTextureRef& materialTextureBinding : link.materialTextureBindings)
            {
                file << "materialTextureBinding "
                     << materialTextureBinding.materialIndex << ' '
                     << materialTextureBinding.textureIndex << ' '
                     << std::quoted(materialTextureBinding.slotName) << '\n';
            }
            WriteOptionalPath(file, "matchedGrpPath", link.matchedGrpPath);
            WriteOptionalString(file, "matchedGrpEntryName", link.matchedGrpEntryName);
            file << "matchedGrpClassId " << link.matchedGrpClassId << '\n';
            file << "matchedGrpClassName " << std::quoted(link.matchedGrpClassName) << '\n';
            file << "matchedGrpCanExportStatic " << (link.matchedGrpEntryCanExportStaticModel ? 1 : 0) << '\n';
            file << "matchedGrpCanExportSkinned " << (link.matchedGrpEntryCanExportSkinnedModel ? 1 : 0) << '\n';

            for (const DescriptorTextureLink& textureLink : link.textureLinks)
            {
                file << "textureLink "
                     << textureLink.textureIndex << ' '
                     << std::quoted(textureLink.textureName) << ' '
                     << std::quoted(textureLink.matchedDxpPath ? Platform::PathToUtf8(*textureLink.matchedDxpPath) : std::string()) << ' '
                     << std::quoted(textureLink.matchedDxpEntryName.value_or(std::string())) << '\n';
            }

            file << "endLink\n";
        }

        if (!file)
        {
            error = "Could not finish writing descriptor-link cache.";
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
            else if (tag == "linkCount")
            {
                stream >> expectedLinkCount;
            }
            else if (tag == "beginLink")
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
                    if (linkTag == "descriptorPath")
                    {
                        if (!ReadQuotedPath(linkStream, link.descriptorPath))
                        {
                            result.message = "Descriptor-link cache link descriptor path is malformed.";
                            return result;
                        }
                    }
                    else if (linkTag == "resourceName")
                    {
                        linkStream >> std::quoted(link.resourceName);
                    }
                    else if (linkTag == "resId")
                    {
                        linkStream >> link.resId;
                    }
                    else if (linkTag == "textureName")
                    {
                        std::string value;
                        if (linkStream >> std::quoted(value))
                        {
                            link.textureNames.push_back(std::move(value));
                        }
                    }
                    else if (linkTag == "materialName")
                    {
                        std::string value;
                        if (linkStream >> std::quoted(value))
                        {
                            link.materialNames.push_back(std::move(value));
                        }
                    }
                    else if (linkTag == "materialDomain")
                    {
                        std::string value;
                        if (linkStream >> std::quoted(value))
                        {
                            link.materialDomains.push_back(std::move(value));
                        }
                    }
                    else if (linkTag == "materialShaderClass")
                    {
                        std::string value;
                        if (linkStream >> std::quoted(value))
                        {
                            link.materialShaderClasses.push_back(std::move(value));
                        }
                    }
                    else if (linkTag == "materialTextureRef")
                    {
                        uint32_t value = 0;
                        if (linkStream >> value)
                        {
                            link.materialTextureRefs.push_back(value);
                        }
                    }
                    else if (linkTag == "materialTextureBinding")
                    {
                        DescriptorMaterialTextureRef ref;
                        if (!(linkStream >> ref.materialIndex >> ref.textureIndex >> std::quoted(ref.slotName)))
                        {
                            result.message = "Descriptor-link cache material texture binding row is malformed.";
                            return result;
                        }
                        link.materialTextureBindings.push_back(std::move(ref));
                    }
                    else if (linkTag == "matchedGrpPath")
                    {
                        fs::path value;
                        if (!ReadQuotedPath(linkStream, value))
                        {
                            result.message = "Descriptor-link cache matched GRP path is malformed.";
                            return result;
                        }
                        link.matchedGrpPath = std::move(value);
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
                    else if (linkTag == "textureLink")
                    {
                        DescriptorTextureLink textureLink;
                        std::string textureName;
                        std::string dxpPath;
                        std::string dxpEntryName;
                        if (!(linkStream >> textureLink.textureIndex >> std::quoted(textureName) >> std::quoted(dxpPath) >> std::quoted(dxpEntryName)))
                        {
                            result.message = "Descriptor-link cache texture link row is malformed.";
                            return result;
                        }

                        textureLink.textureName = std::move(textureName);
                        if (!dxpPath.empty())
                        {
                            textureLink.matchedDxpPath = Platform::Utf8ToPath(dxpPath);
                        }
                        if (!dxpEntryName.empty())
                        {
                            textureLink.matchedDxpEntryName = std::move(dxpEntryName);
                        }
                        link.textureLinks.push_back(std::move(textureLink));
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
        if (expectedLinkCount != loadedLinks.size())
        {
            result.message = "Descriptor-link cache link table is truncated.";
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

        result.links = std::move(loadedLinks);
        result.loaded = true;
        result.message = "Loaded " + std::to_string(result.links.size()) + " cached descriptor links.";
        return result;
    }
}
