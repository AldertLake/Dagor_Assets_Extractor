#include "AssetIndex/DependencyGraph.h"

#include "AssetIndex/BinaryCache.h"
#include "AssetIndex/PackContents.h"
#include "AssetIndex/TextureQuality.h"
#include "DagorFormats/GrpReader.h"
#include "DagorIO/MappedFile.h"
#include "Jobs/JobSystem.h"
#include "Platform/Platform.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
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
        constexpr size_t ScopedDxpCandidateLimit = 48;
        constexpr int DescriptorLinkerVersion = 10;
        constexpr uint32_t kInvalidTableId = 0xFFFFFFFFu;

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
            return GlobalAssetIndex::MakePathKey(path);
        }

        bool SamePath(const fs::path& left, const fs::path& right)
        {
            return PathKey(left) == PathKey(right);
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
            keys.reserve(6);
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

        bool IsNameRelatedToPackStem(std::string_view nameKey, std::string_view packStem)
        {
            if (nameKey.empty() || packStem.empty())
            {
                return false;
            }

            if (nameKey == packStem)
            {
                return true;
            }

            const auto startsWithToken = [](std::string_view value, std::string_view token) {
                return value.size() > token.size() &&
                       value.compare(0, token.size(), token) == 0 &&
                       (value[token.size()] == '_' || value[token.size()] == '-');
            };

            return startsWithToken(nameKey, packStem) || startsWithToken(packStem, nameKey);
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

        struct PackMatchMeta
        {
            fs::path path;
            uint32_t parentDirId = 0;
            std::string packStem;
            int baseQualityPriority = 0;
            bool isPatch = false;
            bool isHq = false;
            bool isCockpits = false;
            bool isContentBase = false;
            bool isDynModelDesc = false;
            bool isRiDesc = false;
        };

        class PackMetaRegistry
        {
        public:
            uint32_t Intern(const fs::path& path)
            {
                const std::wstring key = PathKey(path);
                if (const auto it = m_indexByPath.find(key); it != m_indexByPath.end())
                {
                    return it->second;
                }

                const uint32_t id = static_cast<uint32_t>(m_metas.size());
                PackMatchMeta meta;
                meta.path = path;

                const std::wstring parentKey = PathKey(path.parent_path());
                auto [dirIt, inserted] = m_dirIndex.emplace(parentKey, static_cast<uint32_t>(m_dirIndex.size()));
                meta.parentDirId = dirIt->second;
                (void)inserted;

                const std::string lowerPath = FullPathKey(Platform::PathToUtf8(path));
                const std::string fileStem = BasenameKey(Platform::PathToUtf8(path.filename()));
                meta.packStem = StripPackQualitySuffix(fileStem);
                meta.baseQualityPriority = TextureQualityPriority(path, {});
                meta.isPatch = ContainsAscii(lowerPath, "/patch/");
                meta.isHq = ClassifyTextureQuality(path, {}) >= TextureQualityTier::High;
                meta.isCockpits = ContainsAscii(lowerPath, "pkg_cockpits");
                meta.isContentBase = ContainsAscii(lowerPath, "/content/base/");
                meta.isDynModelDesc = ContainsAscii(fileStem, "dynmodeldesc");
                meta.isRiDesc = ContainsAscii(fileStem, "ridesc");

                m_metas.push_back(std::move(meta));
                m_indexByPath.emplace(key, id);
                return id;
            }

            const PackMatchMeta& Get(uint32_t id) const
            {
                return m_metas[id];
            }

        private:
            std::vector<PackMatchMeta> m_metas;
            std::unordered_map<std::wstring, uint32_t> m_indexByPath;
            std::unordered_map<std::wstring, uint32_t> m_dirIndex;
        };

        int CommonPathScore(const PackMatchMeta& left, const PackMatchMeta& right)
        {
            int score = 0;
            if (left.parentDirId == right.parentDirId)
            {
                score += 40;
            }
            if (left.isPatch == right.isPatch)
            {
                score += 10;
            }
            if (left.isHq == right.isHq)
            {
                score += 10;
            }
            if (left.isCockpits && right.isCockpits)
            {
                score += 25;
            }
            if (left.isContentBase && right.isContentBase)
            {
                score += 10;
            }
            return score;
        }

        int CommonPathScoreSlow(const fs::path& left, const fs::path& right)
        {
            const std::string leftKey = FullPathKey(Platform::PathToUtf8(left));
            const std::string rightKey = FullPathKey(Platform::PathToUtf8(right));
            int score = 0;
            if (IsSameDirectory(left, right))
            {
                score += 40;
            }
            if (ContainsAscii(leftKey, "/patch/") == ContainsAscii(rightKey, "/patch/"))
            {
                score += 10;
            }
            if ((ClassifyTextureQuality(left, {}) >= TextureQualityTier::High) ==
                (ClassifyTextureQuality(right, {}) >= TextureQualityTier::High))
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

        const ResourceRecord* FindFocusRecord(
            const std::vector<ResourceRecord>& records,
            const std::optional<fs::path>& focusPath)
        {
            if (!focusPath)
            {
                return nullptr;
            }

            const std::wstring targetKey = PathKey(*focusPath);
            for (const ResourceRecord& record : records)
            {
                if (PathKey(record.path) == targetKey)
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

            return CommonPathScoreSlow(record.path, focus.path) >= 45;
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
                const std::wstring focusKey = PathKey(focus->path);
                for (const DescriptorTextureLink& textureLink : link.textureLinks)
                {
                    if (textureLink.matchedDxpPath && PathKey(*textureLink.matchedDxpPath) == focusKey)
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
            uint32_t packMetaId = 0;
            fs::path path;
            std::string name;
            std::vector<std::string> nameKeys;
            uint32_t classId = 0;
            std::string className;
            uint32_t offset = 0;
            uint32_t size = 0;
            bool canExportStaticModel = false;
            bool canExportSkinnedModel = false;
        };

        struct LightweightDxpEntry
        {
            uint32_t packMetaId = 0;
            fs::path path;
            std::string name;
            std::vector<std::string> nameKeys;
            int qualityPriority = 0;
        };

        uint16_t ReadLe16(std::span<const uint8_t> data, size_t offset)
        {
            if (offset > data.size() || data.size() - offset < sizeof(uint16_t))
            {
                throw std::runtime_error("metadata read out of bounds");
            }

            return static_cast<uint16_t>(data[offset]) |
                   static_cast<uint16_t>(static_cast<uint16_t>(data[offset + 1]) << 8);
        }

        uint32_t ReadLe32(std::span<const uint8_t> data, size_t offset)
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

        bool IsRangeWithin(size_t offset, size_t size, size_t begin, size_t end)
        {
            return offset >= begin && offset <= end && size <= end - offset;
        }

        std::string ReadCStringAt(
            std::span<const uint8_t> data,
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

        PatchableTabRef ReadPatchableTabRef(std::span<const uint8_t> data, size_t offset)
        {
            PatchableTabRef ref;
            ref.offset = ReadLe32(data, offset);
            ref.count = ReadLe32(data, offset + 4);
            return ref;
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

        uint32_t ComputePayloadSizeFromSortedOffsets(
            const std::vector<uint32_t>& sortedUniqueOffsets,
            uint32_t offset,
            size_t fileSize,
            size_t payloadBegin)
        {
            if (offset < payloadBegin || offset >= fileSize)
            {
                return 0;
            }

            const auto nextIt = std::upper_bound(sortedUniqueOffsets.begin(), sortedUniqueOffsets.end(), offset);
            const uint32_t nextOffset = (nextIt != sortedUniqueOffsets.end())
                ? *nextIt
                : static_cast<uint32_t>(fileSize);
            return (nextOffset > offset) ? (nextOffset - offset) : 0;
        }

        bool ParseGrpMetadataForLinking(
            const fs::path& path,
            uint32_t packMetaId,
            uintmax_t maxPackBytes,
            std::vector<LightweightGrpEntry>& entries,
            std::string& error)
        {
            DagorIO::MappedFile mapped;
            if (!mapped.Open(path, error))
            {
                return false;
            }

            const std::span<const uint8_t> data = mapped.Span();
            const size_t fileSize = data.size();
            if (fileSize < 8)
            {
                error = "GRP file metadata is unavailable: " + Platform::PathToUtf8(path.filename());
                return false;
            }

            const std::string_view magic(reinterpret_cast<const char*>(data.data()), 4);
            if (magic == "GRP2" || magic == "GRP3")
            {
                if (fileSize < 16)
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

                    const uint32_t fullDataSize = ReadLe32(data, 8);
                    const size_t metadataSize = modernHeaderSize + static_cast<size_t>(fullDataSize);
                    if (fullDataSize < grpDataHeaderSize || metadataSize > fileSize || metadataSize > maxMetadataBytes)
                    {
                        error = "GRP metadata size is out of bounds: " + Platform::PathToUtf8(path.filename());
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

                    std::vector<uint32_t> sortedUniqueOffsets;
                    sortedUniqueOffsets.reserve(resTableRef.count);
                    for (uint32_t i = 0; i < resTableRef.count; ++i)
                    {
                        const size_t entryOffset = resTableRef.offset + static_cast<size_t>(i) * resEntrySize;
                        const uint32_t payloadOffset = ReadLe32(data, entryOffset + 4);
                        if (payloadOffset >= metadataEnd && payloadOffset < fileSize)
                        {
                            sortedUniqueOffsets.push_back(payloadOffset);
                        }
                    }
                    std::sort(sortedUniqueOffsets.begin(), sortedUniqueOffsets.end());
                    sortedUniqueOffsets.erase(
                        std::unique(sortedUniqueOffsets.begin(), sortedUniqueOffsets.end()),
                        sortedUniqueOffsets.end());

                    entries.reserve(entries.size() + resTableRef.count);
                    for (uint32_t i = 0; i < resTableRef.count; ++i)
                    {
                        const size_t entryOffset = resTableRef.offset + static_cast<size_t>(i) * resEntrySize;
                        const uint32_t classId = ReadLe32(data, entryOffset);
                        const uint32_t payloadOffset = ReadLe32(data, entryOffset + 4);
                        const uint16_t resId = ReadLe16(data, entryOffset + 8);

                        LightweightGrpEntry entry;
                        entry.packMetaId = packMetaId;
                        entry.path = path;
                        entry.classId = classId;
                        entry.className = DescribeGrpClassId(classId);
                        entry.offset = payloadOffset;
                        entry.size = ComputePayloadSizeFromSortedOffsets(
                            sortedUniqueOffsets,
                            payloadOffset,
                            fileSize,
                            metadataEnd);

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

            if (maxPackBytes > 0 && fileSize > maxPackBytes)
            {
                error = "Skipped large legacy GRP during descriptor linking: " + Platform::PathToUtf8(path.filename());
                return false;
            }

            try
            {
                DagorFormats::GrpReader reader(data);
                reader.Parse();

                const auto& files = reader.GetFiles();
                entries.reserve(entries.size() + files.size());
                for (const DagorFormats::GrpFileInfo& file : files)
                {
                    LightweightGrpEntry entry;
                    entry.packMetaId = packMetaId;
                    entry.path = path;
                    entry.name = file.name;
                    entry.classId = file.classId;
                    entry.className = DescribeGrpClassId(file.classId);
                    entry.offset = file.offset;
                    entry.size = file.size;

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

        bool ParseDxpMetadataForLinking(
            const fs::path& path,
            uint32_t packMetaId,
            int baseQualityPriority,
            std::vector<LightweightDxpEntry>& entries,
            std::string& error)
        {
            DagorIO::MappedFile mapped;
            if (!mapped.Open(path, error))
            {
                return false;
            }

            const std::span<const uint8_t> data = mapped.Span();
            if (data.size() < 0x48)
            {
                error = "DXP file metadata is unavailable: " + Platform::PathToUtf8(path.filename());
                return false;
            }

            if (std::string_view(reinterpret_cast<const char*>(data.data()), 4) != "DxP2")
            {
                error = "Invalid DXP magic in " + Platform::PathToUtf8(path.filename());
                return false;
            }

            constexpr size_t maxMetadataBytes = 64 * 1024 * 1024;
            const uint16_t totalFiles = ReadLe16(data, 0x8);
            const uint32_t block3Offset = ReadLe32(data, 0x30) + 0x10;
            const size_t requiredSize = static_cast<size_t>(block3Offset) + static_cast<size_t>(totalFiles) * 0x18;
            if (requiredSize > data.size() || requiredSize > maxMetadataBytes)
            {
                error = "DXP name map or entry table is truncated or too large in " + Platform::PathToUtf8(path.filename());
                return false;
            }

            std::vector<std::string> names;
            names.reserve(totalFiles);
            size_t offset = 0x48;
            while (names.size() < totalFiles)
            {
                const size_t start = offset;
                while (offset < block3Offset && data[offset] != 0)
                {
                    ++offset;
                }

                if (offset >= block3Offset)
                {
                    error = "DXP name map is truncated or malformed in " + Platform::PathToUtf8(path.filename());
                    return false;
                }

                names.emplace_back(reinterpret_cast<const char*>(data.data() + start), offset - start);
                ++offset;
            }

            entries.reserve(entries.size() + totalFiles);
            for (uint16_t i = 0; i < totalFiles; ++i)
            {
                const size_t entryStart = static_cast<size_t>(block3Offset) + static_cast<size_t>(i) * 0x18;
                const uint32_t payloadSize = ReadLe32(data, entryStart + 0x10);
                if (payloadSize > 0)
                {
                    LightweightDxpEntry entry;
                    entry.packMetaId = packMetaId;
                    entry.path = path;
                    entry.name = std::move(names[i]);
                    entry.qualityPriority = (entry.name.find('$') != std::string::npos)
                        ? TextureQualityPriority(path, entry.name)
                        : baseQualityPriority;
                    entries.push_back(std::move(entry));
                }
            }

            return true;
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
                textureIndices.reserve(descriptorEntry.materialTextureRefs.size());
                for (const DescriptorMaterialTextureRef& materialRef : descriptorEntry.materialTextureRefs)
                {
                    AppendUniqueUInt(textureIndices, materialRef.textureIndex);
                }
            }
            else if (!descriptorEntry.dependencyIds.empty())
            {
                textureIndices.reserve(descriptorEntry.dependencyIds.size());
                for (const uint32_t textureIndex : descriptorEntry.dependencyIds)
                {
                    AppendUniqueUInt(textureIndices, textureIndex);
                }
            }
            else
            {
                textureIndices.reserve(descriptorEntry.textureNames.size());
                for (size_t textureIndex = 0; textureIndex < descriptorEntry.textureNames.size(); ++textureIndex)
                {
                    textureIndices.push_back(static_cast<uint32_t>(textureIndex));
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

        int ScoreDxpRecordForFocus(
            const PackMatchMeta& meta,
            const PackMatchMeta& focusMeta,
            const std::vector<std::vector<std::string>>& focusedResourceNameKeys,
            const std::vector<std::vector<std::string>>& focusedTextureNameKeys)
        {
            int score = 0;
            if (meta.parentDirId == focusMeta.parentDirId)
            {
                score += 20;
            }
            if (meta.isHq)
            {
                score += 6;
            }
            if (meta.isPatch)
            {
                score += 4;
            }
            const std::string& packStem = meta.packStem;
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

        long long ElapsedMilliseconds(std::chrono::steady_clock::time_point start)
        {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
        }

        bool BuildGrpMetadataIndexParallel(
            const std::vector<const ResourceRecord*>& grpRecords,
            const std::vector<uint32_t>& grpMetaIds,
            const PackMetaRegistry& metaRegistry,
            uintmax_t maxPackBytes,
            DescriptorResourceLinkBuildResult& result,
            const DescriptorResourceLinkBuildOptions& options,
            std::vector<LightweightGrpEntry>& grpEntries,
            std::unordered_map<std::string, std::vector<size_t>, TransparentStringHash, std::equal_to<>>& grpIndex,
            std::vector<CachedGrpPackSnapshot>* outPackSnapshots,
            size_t& reusedPackCount,
            size_t& workDone,
            size_t totalWork)
        {
            if (grpRecords.empty())
            {
                return true;
            }

            std::unordered_map<std::wstring, const CachedGrpPackSnapshot*> prevByPath;
            if (options.previousPackMetadata != nullptr)
            {
                prevByPath.reserve(options.previousPackMetadata->grpPacks.size());
                for (const CachedGrpPackSnapshot& snap : options.previousPackMetadata->grpPacks)
                {
                    prevByPath.emplace(PathKey(snap.path), &snap);
                }
            }

            std::vector<std::vector<LightweightGrpEntry>> perPackEntries(grpRecords.size());
            std::vector<std::string> perPackErrors(grpRecords.size());
            std::vector<uint8_t> perPackParsed(grpRecords.size(), 0);
            std::vector<uint8_t> perPackReused(grpRecords.size(), 0);

            const size_t stageBaseWork = workDone;
            std::atomic<size_t> filesDone{ 0 };
            std::atomic<size_t> runningEntryCount{ 0 };
            std::atomic<size_t> parsedPackCounter{ 0 };
            std::atomic<bool> cancelRequested{ false };
            std::mutex progressMutex;

            Jobs::JobSystem::RunParallelFor(
                grpRecords.size(),
                1,
                [&](size_t recordIndex, size_t) {
                    if (cancelRequested.load(std::memory_order_relaxed))
                    {
                        return;
                    }

                    const ResourceRecord& record = *grpRecords[recordIndex];
                    const uint32_t packMetaId = grpMetaIds[recordIndex];
                    const auto parseStart = std::chrono::steady_clock::now();

                    std::vector<LightweightGrpEntry>& localEntries = perPackEntries[recordIndex];
                    std::string error;
                    bool parsed = false;
                    bool reused = false;

                    if (!prevByPath.empty())
                    {
                        if (const auto it = prevByPath.find(PathKey(record.path));
                            it != prevByPath.end() &&
                            it->second->sizeBytes == record.sizeBytes &&
                            it->second->lastWriteTicks == record.lastWriteTicks)
                        {
                            const CachedGrpPackSnapshot& snap = *it->second;
                            localEntries.reserve(snap.entries.size());
                            for (const IndexedGrpEntry& cached : snap.entries)
                            {
                                LightweightGrpEntry entry;
                                entry.packMetaId = packMetaId;
                                entry.path = record.path;
                                entry.name = cached.name;
                                entry.classId = cached.classId;
                                entry.className = DescribeGrpClassId(cached.classId);
                                entry.offset = cached.offset;
                                entry.size = cached.size;
                                SubAssetRecord probe;
                                probe.name = entry.name;
                                probe.grpClassId = entry.classId;
                                entry.canExportStaticModel = CanExportGrpEntryAsStaticModel(probe);
                                entry.canExportSkinnedModel = CanExportGrpEntryAsSkinnedModel(probe);
                                localEntries.push_back(std::move(entry));
                            }
                            parsed = true;
                            reused = true;
                        }
                    }

                    if (!parsed)
                    {
                        parsed = ParseGrpMetadataForLinking(
                            record.path,
                            packMetaId,
                            maxPackBytes,
                            localEntries,
                            error);
                    }

                    if (parsed)
                    {
                        for (LightweightGrpEntry& entry : localEntries)
                        {
                            entry.nameKeys = BuildAssetNameKeys(entry.name);
                        }
                        perPackParsed[recordIndex] = 1;
                        perPackReused[recordIndex] = reused ? 1 : 0;
                        runningEntryCount.fetch_add(localEntries.size(), std::memory_order_relaxed);
                        parsedPackCounter.fetch_add(1, std::memory_order_relaxed);
                    }
                    else
                    {
                        perPackErrors[recordIndex] = std::move(error);
                    }

                    const long long elapsedMs = ElapsedMilliseconds(parseStart);
                    const size_t completed = filesDone.fetch_add(1, std::memory_order_relaxed) + 1;

                    if (options.progress)
                    {
                        std::string item = Platform::PathToUtf8(metaRegistry.Get(packMetaId).path.filename());
                        if (parsed)
                        {
                            item += (reused ? " reused " : " indexed ") +
                                std::to_string(localEntries.size()) +
                                " resources in " + std::to_string(elapsedMs) +
                                " ms (" + std::to_string(runningEntryCount.load(std::memory_order_relaxed)) + " total)";
                        }
                        else
                        {
                            item += " failed in " + std::to_string(elapsedMs) + " ms";
                        }

                        std::lock_guard<std::mutex> lock(progressMutex);
                        result.grpPacksParsed = parsedPackCounter.load(std::memory_order_relaxed);
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
                });

            workDone = stageBaseWork + filesDone.load(std::memory_order_relaxed);
            result.grpPacksParsed = parsedPackCounter.load(std::memory_order_relaxed);
            if (cancelRequested.load())
            {
                result.canceled = true;
                return false;
            }

            size_t totalEntries = 0;
            for (size_t i = 0; i < grpRecords.size(); ++i)
            {
                if (perPackParsed[i] != 0)
                {
                    totalEntries += perPackEntries[i].size();
                    if (perPackReused[i] != 0)
                    {
                        ++reusedPackCount;
                    }
                }
                else if (!perPackErrors[i].empty())
                {
                    AddWarning(result, perPackErrors[i]);
                }
            }

            grpEntries.reserve(grpEntries.size() + totalEntries);
            grpIndex.reserve(totalEntries * 2 + 16);
            if (outPackSnapshots != nullptr)
            {
                outPackSnapshots->reserve(grpRecords.size());
            }

            for (size_t i = 0; i < grpRecords.size(); ++i)
            {
                if (perPackParsed[i] == 0)
                {
                    continue;
                }

                if (outPackSnapshots != nullptr)
                {
                    CachedGrpPackSnapshot snap;
                    snap.path = grpRecords[i]->path;
                    snap.sizeBytes = grpRecords[i]->sizeBytes;
                    snap.lastWriteTicks = grpRecords[i]->lastWriteTicks;
                    snap.entries.reserve(perPackEntries[i].size());
                    for (const LightweightGrpEntry& entry : perPackEntries[i])
                    {
                        snap.entries.push_back(IndexedGrpEntry{
                            snap.path,
                            entry.name,
                            entry.classId,
                            entry.offset,
                            entry.size
                        });
                    }
                    outPackSnapshots->push_back(std::move(snap));
                }

                for (LightweightGrpEntry& entry : perPackEntries[i])
                {
                    const size_t entryIdx = grpEntries.size();
                    for (const std::string& key : entry.nameKeys)
                    {
                        if (key.empty())
                        {
                            continue;
                        }
                        std::vector<size_t>& bucket = grpIndex[key];
                        if (bucket.empty() || bucket.back() != entryIdx)
                        {
                            bucket.push_back(entryIdx);
                        }
                    }
                    grpEntries.push_back(std::move(entry));
                }
            }

            return !result.canceled;
        }

        bool BuildDxpMetadataIndexParallel(
            const std::vector<const ResourceRecord*>& dxpRecords,
            const std::vector<uint32_t>& dxpMetaIds,
            const PackMetaRegistry& metaRegistry,
            const std::vector<std::vector<std::string>>& focusedTextureKeySets,
            DescriptorResourceLinkBuildResult& result,
            const DescriptorResourceLinkBuildOptions& options,
            std::vector<LightweightDxpEntry>& dxpEntries,
            std::unordered_map<std::string, std::vector<size_t>, TransparentStringHash, std::equal_to<>>& dxpIndex,
            std::vector<CachedDxpPackSnapshot>* outPackSnapshots,
            size_t& reusedPackCount,
            size_t& workDone,
            size_t totalWork)
        {
            if (dxpRecords.empty())
            {
                return true;
            }

            std::unordered_map<std::wstring, const CachedDxpPackSnapshot*> prevByPath;
            if (options.previousPackMetadata != nullptr)
            {
                prevByPath.reserve(options.previousPackMetadata->dxpPacks.size());
                for (const CachedDxpPackSnapshot& snap : options.previousPackMetadata->dxpPacks)
                {
                    prevByPath.emplace(PathKey(snap.path), &snap);
                }
            }

            // Lock-free focused texture completion tracker for fast scoped early exit
            std::unordered_map<std::string, std::vector<size_t>, TransparentStringHash, std::equal_to<>> focusedKeyMap;
            std::vector<std::atomic<uint8_t>> matchedFocusFlags(focusedTextureKeySets.size());
            std::atomic<size_t> matchedFocusRemaining{ focusedTextureKeySets.size() };
            if (!focusedTextureKeySets.empty())
            {
                focusedKeyMap.reserve(focusedTextureKeySets.size() * 4);
                for (size_t texIdx = 0; texIdx < focusedTextureKeySets.size(); ++texIdx)
                {
                    matchedFocusFlags[texIdx].store(0, std::memory_order_relaxed);
                    for (const std::string& key : focusedTextureKeySets[texIdx])
                    {
                        if (!key.empty())
                        {
                            focusedKeyMap[key].push_back(texIdx);
                        }
                    }
                }
            }

            std::vector<std::vector<LightweightDxpEntry>> perPackEntries(dxpRecords.size());
            std::vector<std::string> perPackErrors(dxpRecords.size());
            std::vector<uint8_t> perPackParsed(dxpRecords.size(), 0);
            std::vector<uint8_t> perPackReused(dxpRecords.size(), 0);

            const size_t stageBaseWork = workDone;
            std::atomic<size_t> filesDone{ 0 };
            std::atomic<size_t> runningEntryCount{ 0 };
            std::atomic<size_t> parsedPackCounter{ 0 };
            std::atomic<bool> stopRequested{ false };
            std::atomic<bool> cancelRequested{ false };
            std::atomic<bool> matchedFocusedReported{ false };
            std::mutex progressMutex;

            Jobs::JobSystem::RunParallelFor(
                dxpRecords.size(),
                1,
                [&](size_t recordIndex, size_t) {
                    if (stopRequested.load(std::memory_order_relaxed))
                    {
                        return;
                    }

                    const ResourceRecord& record = *dxpRecords[recordIndex];
                    const uint32_t packMetaId = dxpMetaIds[recordIndex];
                    const int baseQual = metaRegistry.Get(packMetaId).baseQualityPriority;
                    const auto parseStart = std::chrono::steady_clock::now();

                    std::vector<LightweightDxpEntry>& localEntries = perPackEntries[recordIndex];
                    std::string error;
                    bool parsed = false;
                    bool reused = false;

                    if (!prevByPath.empty())
                    {
                        if (const auto it = prevByPath.find(PathKey(record.path));
                            it != prevByPath.end() &&
                            it->second->sizeBytes == record.sizeBytes &&
                            it->second->lastWriteTicks == record.lastWriteTicks)
                        {
                            const CachedDxpPackSnapshot& snap = *it->second;
                            localEntries.reserve(snap.entryNames.size());
                            for (const std::string& name : snap.entryNames)
                            {
                                LightweightDxpEntry entry;
                                entry.packMetaId = packMetaId;
                                entry.path = record.path;
                                entry.name = name;
                                entry.qualityPriority = (entry.name.find('$') != std::string::npos)
                                    ? TextureQualityPriority(record.path, entry.name)
                                    : baseQual;
                                localEntries.push_back(std::move(entry));
                            }
                            parsed = true;
                            reused = true;
                        }
                    }

                    if (!parsed)
                    {
                        parsed = ParseDxpMetadataForLinking(
                            record.path,
                            packMetaId,
                            baseQual,
                            localEntries,
                            error);
                    }

                    bool allFocusedMatchedNow = false;
                    if (parsed)
                    {
                        for (LightweightDxpEntry& entry : localEntries)
                        {
                            entry.nameKeys = BuildAssetNameKeys(entry.name);
                            if (!focusedKeyMap.empty())
                            {
                                for (const std::string& key : entry.nameKeys)
                                {
                                    if (const auto fit = focusedKeyMap.find(key); fit != focusedKeyMap.end())
                                    {
                                        for (const size_t texIdx : fit->second)
                                        {
                                            if (matchedFocusFlags[texIdx].exchange(1, std::memory_order_relaxed) == 0)
                                            {
                                                if (matchedFocusRemaining.fetch_sub(1, std::memory_order_relaxed) == 1)
                                                {
                                                    allFocusedMatchedNow = true;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        perPackParsed[recordIndex] = 1;
                        perPackReused[recordIndex] = reused ? 1 : 0;
                        runningEntryCount.fetch_add(localEntries.size(), std::memory_order_relaxed);
                        parsedPackCounter.fetch_add(1, std::memory_order_relaxed);
                    }
                    else
                    {
                        perPackErrors[recordIndex] = std::move(error);
                    }

                    const long long elapsedMs = ElapsedMilliseconds(parseStart);
                    const size_t completed = filesDone.fetch_add(1, std::memory_order_relaxed) + 1;

                    if (options.progress)
                    {
                        std::string item = Platform::PathToUtf8(record.path.filename());
                        if (parsed)
                        {
                            item += (reused ? " reused " : " indexed ") +
                                std::to_string(localEntries.size()) +
                                " textures in " + std::to_string(elapsedMs) +
                                " ms (" + std::to_string(runningEntryCount.load(std::memory_order_relaxed)) + " total)";
                        }
                        else
                        {
                            item += " failed in " + std::to_string(elapsedMs) + " ms";
                        }

                        std::lock_guard<std::mutex> lock(progressMutex);
                        result.dxpPacksParsed = parsedPackCounter.load(std::memory_order_relaxed);
                        if (!ReportDescriptorProgress(
                                result,
                                options,
                                "DXP index",
                                std::move(item),
                                stageBaseWork + completed,
                                totalWork))
                        {
                            cancelRequested.store(true, std::memory_order_relaxed);
                            stopRequested.store(true, std::memory_order_relaxed);
                            return;
                        }

                        if (allFocusedMatchedNow && !matchedFocusedReported.exchange(true))
                        {
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
                                cancelRequested.store(true, std::memory_order_relaxed);
                            }
                        }
                    }
                    else if (allFocusedMatchedNow)
                    {
                        stopRequested.store(true, std::memory_order_relaxed);
                    }
                });

            workDone = stageBaseWork + filesDone.load(std::memory_order_relaxed);
            result.dxpPacksParsed = parsedPackCounter.load(std::memory_order_relaxed);
            if (cancelRequested.load())
            {
                result.canceled = true;
                return false;
            }

            size_t totalEntries = 0;
            for (size_t i = 0; i < dxpRecords.size(); ++i)
            {
                if (perPackParsed[i] != 0)
                {
                    totalEntries += perPackEntries[i].size();
                    if (perPackReused[i] != 0)
                    {
                        ++reusedPackCount;
                    }
                }
                else if (!perPackErrors[i].empty())
                {
                    AddWarning(result, perPackErrors[i]);
                }
            }

            dxpEntries.reserve(dxpEntries.size() + totalEntries);
            dxpIndex.reserve(totalEntries * 2 + 16);
            if (outPackSnapshots != nullptr)
            {
                outPackSnapshots->reserve(dxpRecords.size());
            }

            for (size_t i = 0; i < dxpRecords.size(); ++i)
            {
                if (perPackParsed[i] == 0)
                {
                    continue;
                }

                if (outPackSnapshots != nullptr)
                {
                    CachedDxpPackSnapshot snap;
                    snap.path = dxpRecords[i]->path;
                    snap.sizeBytes = dxpRecords[i]->sizeBytes;
                    snap.lastWriteTicks = dxpRecords[i]->lastWriteTicks;
                    snap.entryNames.reserve(perPackEntries[i].size());
                    for (const LightweightDxpEntry& entry : perPackEntries[i])
                    {
                        snap.entryNames.push_back(entry.name);
                    }
                    outPackSnapshots->push_back(std::move(snap));
                }

                for (LightweightDxpEntry& entry : perPackEntries[i])
                {
                    const size_t entryIdx = dxpEntries.size();
                    for (const std::string& key : entry.nameKeys)
                    {
                        if (key.empty())
                        {
                            continue;
                        }
                        std::vector<size_t>& bucket = dxpIndex[key];
                        if (bucket.empty() || bucket.back() != entryIdx)
                        {
                            bucket.push_back(entryIdx);
                        }
                    }
                    dxpEntries.push_back(std::move(entry));
                }
            }

            return !result.canceled;
        }

        struct CachedEntryKeys
        {
            std::vector<std::string> nameKeys;
            std::vector<std::string> dynModelKeys;
        };

        const LightweightGrpEntry* FindBestGrpEntry(
            const PackMetaRegistry& metaRegistry,
            uint32_t descriptorMetaId,
            const std::vector<LightweightGrpEntry>& entries,
            const std::unordered_map<std::string, std::vector<size_t>, TransparentStringHash, std::equal_to<>>& index,
            const CachedEntryKeys& keys,
            bool descriptorIsDynModel,
            bool descriptorIsRendInst)
        {
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

            const PackMatchMeta& descMeta = metaRegistry.Get(descriptorMetaId);
            const LightweightGrpEntry* best = nullptr;
            int bestScore = std::numeric_limits<int>::min();
            for (const size_t candidate : candidates)
            {
                if (candidate >= entries.size())
                {
                    continue;
                }

                const LightweightGrpEntry& entry = entries[candidate];
                const PackMatchMeta& grpMeta = metaRegistry.Get(entry.packMetaId);

                int score = CommonPathScore(descMeta, grpMeta);
                if (NameKeysIntersect(entry.nameKeys, keys.nameKeys))
                {
                    score += 100;
                }
                if (!keys.dynModelKeys.empty() && NameKeysIntersect(entry.nameKeys, keys.dynModelKeys))
                {
                    score += 120;
                }

                bool related = false;
                for (const std::string& key : keys.nameKeys)
                {
                    if (IsNameRelatedToPackStem(key, grpMeta.packStem))
                    {
                        related = true;
                        break;
                    }
                }
                if (!related)
                {
                    for (const std::string& key : keys.dynModelKeys)
                    {
                        if (IsNameRelatedToPackStem(key, grpMeta.packStem))
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

                if (grpMeta.isPatch)
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
            const PackMetaRegistry& metaRegistry,
            uint32_t descriptorMetaId,
            const std::optional<uint32_t>& matchedGrpMetaId,
            const std::vector<std::string>& resourceNameKeys,
            const std::vector<LightweightDxpEntry>& entries,
            const std::unordered_map<std::string, std::vector<size_t>, TransparentStringHash, std::equal_to<>>& index,
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

            const PackMatchMeta& descMeta = metaRegistry.Get(descriptorMetaId);
            const PackMatchMeta* grpMeta = matchedGrpMetaId ? &metaRegistry.Get(*matchedGrpMetaId) : nullptr;

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
                const PackMatchMeta& dxpMeta = metaRegistry.Get(entry.packMetaId);

                int score = 0;
                if (NameKeysIntersect(entry.nameKeys, textureKeys))
                {
                    score += 100;
                }
                if (grpMeta != nullptr)
                {
                    score += CommonPathScore(*grpMeta, dxpMeta);
                    if (dxpMeta.parentDirId == grpMeta->parentDirId)
                    {
                        score += 30;
                    }
                }
                score += CommonPathScore(descMeta, dxpMeta);
                if (dxpMeta.isPatch)
                {
                    score += 15;
                }

                bool packRelated = false;
                for (const std::string& key : textureKeys)
                {
                    if (IsNameRelatedToPackStem(key, dxpMeta.packStem))
                    {
                        packRelated = true;
                        break;
                    }
                }
                if (!packRelated)
                {
                    for (const std::string& key : resourceNameKeys)
                    {
                        if (IsNameRelatedToPackStem(key, dxpMeta.packStem))
                        {
                            packRelated = true;
                            break;
                        }
                    }
                }
                if (packRelated)
                {
                    score += 10;
                }

                const int quality = entry.qualityPriority;
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

        struct SortableSourceRecord
        {
            ResourceRecord record;
            std::wstring pathKey;
        };

        std::vector<SortableSourceRecord> BuildDescriptorCacheSourceRecords(const std::vector<ResourceRecord>& records)
        {
            std::vector<SortableSourceRecord> sources;
            sources.reserve(records.size());
            for (const ResourceRecord& record : records)
            {
                if (IsDescriptorCacheSourceType(record.type))
                {
                    sources.push_back(SortableSourceRecord{ record, PathKey(record.path) });
                }
            }

            std::sort(sources.begin(), sources.end(), [](const SortableSourceRecord& left, const SortableSourceRecord& right) {
                const int leftType = static_cast<int>(left.record.type);
                const int rightType = static_cast<int>(right.record.type);
                if (leftType != rightType)
                {
                    return leftType < rightType;
                }
                return left.pathKey < right.pathKey;
            });
            return sources;
        }

        bool SameDescriptorCacheSources(
            const std::vector<SortableSourceRecord>& left,
            const std::vector<SortableSourceRecord>& right)
        {
            if (left.size() != right.size())
            {
                return false;
            }

            for (size_t i = 0; i < left.size(); ++i)
            {
                if (left[i].pathKey != right[i].pathKey ||
                    left[i].record.type != right[i].record.type ||
                    left[i].record.sizeBytes != right[i].record.sizeBytes ||
                    left[i].record.lastWriteTicks != right[i].record.lastWriteTicks)
                {
                    return false;
                }
            }

            return true;
        }

        void WriteStringIdVector(
            BinaryWriter& writer,
            StringTableBuilder& stringTable,
            const std::vector<std::string>& values)
        {
            writer.WriteU32(static_cast<uint32_t>(values.size()));
            for (const std::string& val : values)
            {
                writer.WriteU32(stringTable.Intern(val));
            }
        }

        bool ReadStringVectorFromTable(
            BinaryCursor& cursor,
            const std::vector<std::string_view>& stringTable,
            std::vector<std::string>& out,
            std::string& error)
        {
            uint32_t count = 0;
            if (!cursor.ReadU32(count))
            {
                error = "Descriptor-link cache string vector count is truncated.";
                return false;
            }
            if (count > 1000000u)
            {
                error = "Descriptor-link cache string vector count is malformed.";
                return false;
            }
            out.clear();
            out.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
            {
                uint32_t id = 0;
                if (!cursor.ReadU32(id) || id >= stringTable.size())
                {
                    error = "Descriptor-link cache string ID is out of bounds or truncated.";
                    return false;
                }
                const std::string_view sv = stringTable[id];
                out.emplace_back(sv.data(), sv.size());
            }
            return true;
        }
    }

    DescriptorResourceLinkBuildResult BuildDescriptorResourceLinks(
        const std::vector<ResourceRecord>& records,
        const DescriptorResourceLinkBuildOptions& options)
    {
        const auto buildStart = std::chrono::steady_clock::now();
        DescriptorResourceLinkBuildResult result;

        PackMetaRegistry metaRegistry;
        const ResourceRecord* focusRecord = FindFocusRecord(records, options.focusPath);

        std::vector<const ResourceRecord*> descriptorRecords;
        std::vector<const ResourceRecord*> allDescriptorRecords;
        std::vector<const ResourceRecord*> grpRecords;
        std::vector<const ResourceRecord*> dxpRecords;
        descriptorRecords.reserve(records.size() / 8 + 16);
        allDescriptorRecords.reserve(records.size() / 8 + 16);
        grpRecords.reserve(records.size() / 4 + 16);
        dxpRecords.reserve(records.size() / 4 + 16);

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
            const uint32_t focusedMetaId = metaRegistry.Intern(focusRecord->path);
            std::vector<LightweightGrpEntry> focusedGrpEntries;
            std::string focusedGrpError;
            if (ParseGrpMetadataForLinking(
                    focusRecord->path,
                    focusedMetaId,
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

        // Precompute pack metadata once per unique path across all involved records.
        const uint32_t focusMetaId = focusRecord ? metaRegistry.Intern(focusRecord->path) : 0u;
        std::vector<uint32_t> descriptorMetaIds(descriptorRecords.size());
        for (size_t i = 0; i < descriptorRecords.size(); ++i)
        {
            descriptorMetaIds[i] = metaRegistry.Intern(descriptorRecords[i]->path);
        }
        std::vector<uint32_t> grpMetaIds(grpRecords.size());
        for (size_t i = 0; i < grpRecords.size(); ++i)
        {
            grpMetaIds[i] = metaRegistry.Intern(grpRecords[i]->path);
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
                "Reading descriptor files",
                "Scanning descriptor list",
                workDone,
                totalWork))
        {
            return result;
        }

        // Stage 1: Parallel descriptor parsing
        const auto descStart = std::chrono::steady_clock::now();
        struct DescriptorParseSlot
        {
            std::shared_ptr<PackContents> contents;
            bool skipped = false;
            std::string warning;
        };
        std::vector<DescriptorParseSlot> descSlots(descriptorRecords.size());
        std::atomic<bool> descCancelRequested{ false };
        std::atomic<size_t> descDoneCount{ 0 };
        std::mutex descProgressMutex;

        Jobs::JobSystem::RunParallelFor(
            descriptorRecords.size(),
            1,
            [&](size_t idx, size_t) {
                if (descCancelRequested.load(std::memory_order_relaxed))
                {
                    return;
                }

                const ResourceRecord* record = descriptorRecords[idx];
                DescriptorParseSlot& slot = descSlots[idx];
                if (record->sizeBytes > options.maxDescriptorBytes)
                {
                    slot.skipped = true;
                    slot.warning = "Skipped large descriptor: " + Platform::PathToUtf8(record->path.filename());
                }
                else
                {
                    slot.contents = ParseDescriptorContents(
                        record->path,
                        static_cast<size_t>(options.maxDescriptorBytes),
                        descriptorResourceNameFilter.empty() ? nullptr : &descriptorResourceNameFilter);
                    if (!slot.contents)
                    {
                        slot.warning = "Could not parse descriptor: " + Platform::PathToUtf8(record->path.filename());
                    }
                    else if (!slot.contents->error.empty())
                    {
                        slot.warning = slot.contents->error;
                    }
                }

                const size_t completed = descDoneCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (options.progress)
                {
                    std::lock_guard<std::mutex> lock(descProgressMutex);
                    if (!ReportDescriptorProgress(
                            result,
                            options,
                            "Reading descriptor files",
                            Platform::PathToUtf8(record->path.filename()),
                            workDone + completed,
                            totalWork))
                    {
                        descCancelRequested.store(true, std::memory_order_relaxed);
                    }
                }
            });

        workDone += descDoneCount.load(std::memory_order_relaxed);
        if (descCancelRequested.load())
        {
            result.canceled = true;
            return result;
        }

        struct DescriptorEntryRef
        {
            uint32_t metaId = 0;
            fs::path descriptorPath;
            SubAssetRecord entry;
        };

        size_t totalDescEntries = 0;
        for (size_t idx = 0; idx < descSlots.size(); ++idx)
        {
            const DescriptorParseSlot& slot = descSlots[idx];
            if (slot.skipped)
            {
                ++result.descriptorFilesSkipped;
                AddWarning(result, slot.warning);
            }
            else if (!slot.contents || !slot.contents->error.empty())
            {
                ++result.parseErrors;
                AddWarning(result, slot.warning);
            }
            else
            {
                ++result.descriptorFilesParsed;
                totalDescEntries += slot.contents->entries.size();
            }
        }

        std::vector<DescriptorEntryRef> descriptorEntries;
        descriptorEntries.reserve(totalDescEntries);
        for (size_t idx = 0; idx < descSlots.size(); ++idx)
        {
            DescriptorParseSlot& slot = descSlots[idx];
            if (slot.skipped || !slot.contents || !slot.contents->error.empty())
            {
                continue;
            }
            const uint32_t metaId = descriptorMetaIds[idx];
            const fs::path& descPath = descriptorRecords[idx]->path;
            for (SubAssetRecord& entry : slot.contents->entries)
            {
                descriptorEntries.push_back(DescriptorEntryRef{
                    metaId,
                    descPath,
                    std::move(entry)
                });
            }
        }
        descSlots.clear();
        descSlots.shrink_to_fit();

        // Precompute GRP search keys for all descriptor entries in parallel
        std::vector<CachedEntryKeys> descriptorKeys(descriptorEntries.size());
        Jobs::JobSystem::RunParallelFor(
            descriptorEntries.size(),
            64,
            [&](size_t i, size_t) {
                descriptorKeys[i].nameKeys = BuildAssetNameKeys(descriptorEntries[i].entry.name);
                if (!descriptorEntries[i].entry.dynModelName.empty())
                {
                    descriptorKeys[i].dynModelKeys = BuildAssetNameKeys(descriptorEntries[i].entry.dynModelName);
                }
            });
        const long long descParseMs = ElapsedMilliseconds(descStart);

        // Stage 2: Parallel Lock-Free GRP Indexing
        const auto grpStart = std::chrono::steady_clock::now();
        std::vector<LightweightGrpEntry> grpEntries;
        std::unordered_map<std::string, std::vector<size_t>, TransparentStringHash, std::equal_to<>> grpIndex;
        std::shared_ptr<CachedPackMetadata> builtPackMetadata;
        if (!scopedBuild)
        {
            builtPackMetadata = std::make_shared<CachedPackMetadata>();
        }
        size_t grpReusedCount = 0;

        if (!ReportDescriptorProgress(result, options, "GRP index", "Indexing GRP resource names", workDone, totalWork))
        {
            return result;
        }

        if (!BuildGrpMetadataIndexParallel(
                grpRecords,
                grpMetaIds,
                metaRegistry,
                options.maxPackBytes,
                result,
                options,
                grpEntries,
                grpIndex,
                builtPackMetadata ? &builtPackMetadata->grpPacks : nullptr,
                grpReusedCount,
                workDone,
                totalWork))
        {
            return result;
        }
        const long long grpIndexMs = ElapsedMilliseconds(grpStart);

        // Focused texture narrowing for scoped builds
        std::vector<std::string> focusedResourceNames;
        std::vector<std::vector<std::string>> focusedResourceNameKeys;
        std::vector<std::string> focusedTextureNames;
        std::vector<std::vector<std::string>> focusedTextureNameKeys;

        auto appendUniqueResource = [](std::vector<std::string>& names, std::vector<std::vector<std::string>>& keysList, const std::string& name) {
            if (name.empty()) return;
            if (std::find(names.begin(), names.end(), name) == names.end())
            {
                names.push_back(name);
                keysList.push_back(BuildAssetNameKeys(name));
            }
        };

        if (focusRecord != nullptr)
        {
            for (size_t entryIndex = 0; entryIndex < descriptorEntries.size(); ++entryIndex)
            {
                const DescriptorEntryRef& ref = descriptorEntries[entryIndex];
                const PackMatchMeta& descMeta = metaRegistry.Get(ref.metaId);

                bool matchesFocus = false;
                if (focusRecord->type == AssetType::Descriptor)
                {
                    matchesFocus = ref.metaId == focusMetaId;
                }
                else if (focusRecord->type == AssetType::Grp)
                {
                    if (const LightweightGrpEntry* grpMatch = FindBestGrpEntry(
                            metaRegistry,
                            ref.metaId,
                            grpEntries,
                            grpIndex,
                            descriptorKeys[entryIndex],
                            descMeta.isDynModelDesc,
                            descMeta.isRiDesc))
                    {
                        matchesFocus = grpMatch->packMetaId == focusMetaId;
                    }
                }

                if (!matchesFocus)
                {
                    continue;
                }

                appendUniqueResource(focusedResourceNames, focusedResourceNameKeys, ref.entry.name);
                appendUniqueResource(focusedResourceNames, focusedResourceNameKeys, ref.entry.dynModelName);
                for (const uint32_t textureIndex : BuildTextureIndices(ref.entry))
                {
                    if (textureIndex < ref.entry.textureNames.size())
                    {
                        appendUniqueResource(focusedTextureNames, focusedTextureNameKeys, ref.entry.textureNames[textureIndex]);
                    }
                }
            }
        }

        const std::vector<std::vector<std::string>> focusedTextureKeySets =
            BuildTextureKeySets(focusedTextureNames);

        std::vector<uint32_t> dxpMetaIds;
        if (focusRecord != nullptr && !focusedTextureNames.empty())
        {
            const PackMatchMeta& focusMeta = metaRegistry.Get(focusMetaId);
            struct ScoredRecord
            {
                const ResourceRecord* record;
                uint32_t metaId;
                int score;
                int quality;
            };
            std::vector<ScoredRecord> scoredRecords;
            scoredRecords.reserve(dxpRecords.size());
            for (const ResourceRecord* record : dxpRecords)
            {
                const uint32_t metaId = metaRegistry.Intern(record->path);
                const PackMatchMeta& dxpMeta = metaRegistry.Get(metaId);
                const int score = ScoreDxpRecordForFocus(
                    dxpMeta,
                    focusMeta,
                    focusedResourceNameKeys,
                    focusedTextureNameKeys);
                scoredRecords.push_back({
                    record,
                    metaId,
                    score,
                    dxpMeta.baseQualityPriority,
                });
            }

            std::stable_sort(scoredRecords.begin(), scoredRecords.end(), [](const ScoredRecord& left, const ScoredRecord& right) {
                if (left.score != right.score)
                {
                    return left.score > right.score;
                }
                return left.quality > right.quality;
            });

            dxpRecords.clear();
            dxpMetaIds.clear();
            const size_t keepCount = std::min(scoredRecords.size(), ScopedDxpCandidateLimit);
            dxpRecords.reserve(keepCount);
            dxpMetaIds.reserve(keepCount);
            for (size_t i = 0; i < keepCount; ++i)
            {
                dxpRecords.push_back(scoredRecords[i].record);
                dxpMetaIds.push_back(scoredRecords[i].metaId);
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
            dxpMetaIds.clear();
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
        else
        {
            dxpMetaIds.resize(dxpRecords.size());
            for (size_t i = 0; i < dxpRecords.size(); ++i)
            {
                dxpMetaIds[i] = metaRegistry.Intern(dxpRecords[i]->path);
            }
        }

        // Stage 3: Parallel Lock-Free DXP Indexing
        const auto dxpStart = std::chrono::steady_clock::now();
        std::vector<LightweightDxpEntry> dxpEntries;
        std::unordered_map<std::string, std::vector<size_t>, TransparentStringHash, std::equal_to<>> dxpIndex;
        size_t dxpReusedCount = 0;

        if (!ReportDescriptorProgress(result, options, "DXP index", "Indexing DXP texture names", workDone, totalWork))
        {
            return result;
        }

        if (!BuildDxpMetadataIndexParallel(
                dxpRecords,
                dxpMetaIds,
                metaRegistry,
                focusedTextureKeySets,
                result,
                options,
                dxpEntries,
                dxpIndex,
                builtPackMetadata ? &builtPackMetadata->dxpPacks : nullptr,
                dxpReusedCount,
                workDone,
                totalWork))
        {
            return result;
        }
        const long long dxpIndexMs = ElapsedMilliseconds(dxpStart);

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

        // Stage 4: Precompute unique DXP texture search keys in parallel
        const auto linkStart = std::chrono::steady_clock::now();
        std::vector<std::string> uniqueTexNames;
        std::unordered_map<std::string, size_t, TransparentStringHash, std::equal_to<>> texNameToId;
        uniqueTexNames.reserve(descriptorEntries.size() * 2 + 16);
        texNameToId.reserve(descriptorEntries.size() * 2 + 16);
        for (const DescriptorEntryRef& ref : descriptorEntries)
        {
            for (const std::string& texName : ref.entry.textureNames)
            {
                if (!texName.empty() && texNameToId.find(texName) == texNameToId.end())
                {
                    texNameToId.emplace(texName, uniqueTexNames.size());
                    uniqueTexNames.push_back(texName);
                }
            }
        }

        std::vector<std::vector<std::string>> uniqueTexKeys(uniqueTexNames.size());
        Jobs::JobSystem::RunParallelFor(
            uniqueTexNames.size(),
            64,
            [&](size_t i, size_t) {
                uniqueTexKeys[i] = BuildAssetNameKeys(uniqueTexNames[i]);
            });

        constexpr size_t MaxLinkingWorkerSlots = 64;
        std::atomic<size_t> linksDone{ 0 };
        std::mutex resultsMutex;

        std::vector<std::vector<DescriptorResourceLink>> threadLinks(MaxLinkingWorkerSlots);
        std::vector<std::vector<std::string>> threadWarnings(MaxLinkingWorkerSlots);
        std::atomic<bool> linkingCancelRequested{ false };

        Jobs::JobSystem::RunParallelFor(
            descriptorEntries.size(),
            16,
            [&](size_t entryIndex, size_t workerSlot) {
                if (linkingCancelRequested.load(std::memory_order_relaxed))
                {
                    return;
                }

                const size_t safeSlot = workerSlot < MaxLinkingWorkerSlots ? workerSlot : 0;
                std::vector<DescriptorResourceLink>& localLinks = threadLinks[safeSlot];
                std::vector<std::string>& localWarnings = threadWarnings[safeSlot];

                const DescriptorEntryRef& ref = descriptorEntries[entryIndex];
                const SubAssetRecord& descriptorEntry = ref.entry;
                const PackMatchMeta& descMeta = metaRegistry.Get(ref.metaId);

                DescriptorResourceLink link;
                link.descriptorPath = ref.descriptorPath;
                link.resourceName = descriptorEntry.name;
                link.resId = descriptorEntry.resId;
                link.skeletonName = descriptorEntry.skeletonName;
                link.textureNames = descriptorEntry.textureNames;
                link.materialNames = descriptorEntry.materialNames;
                link.materialDomains = descriptorEntry.materialDomains;
                link.materialShaderClasses = descriptorEntry.materialShaderClasses;
                link.materialTextureRefs = descriptorEntry.dependencyIds;
                link.materialTextureBindings = descriptorEntry.materialTextureRefs;

                std::optional<uint32_t> matchedGrpMetaId;
                if (const LightweightGrpEntry* grpMatch = FindBestGrpEntry(
                        metaRegistry,
                        ref.metaId,
                        grpEntries,
                        grpIndex,
                        descriptorKeys[entryIndex],
                        descMeta.isDynModelDesc,
                        descMeta.isRiDesc))
                {
                    matchedGrpMetaId = grpMatch->packMetaId;
                    link.matchedGrpPath = grpMatch->path;
                    link.matchedGrpEntryName = grpMatch->name;
                    link.matchedGrpClassId = grpMatch->classId;
                    link.matchedGrpClassName = grpMatch->className;
                    link.matchedGrpEntryCanExportStaticModel = grpMatch->canExportStaticModel;
                    link.matchedGrpEntryCanExportSkinnedModel = grpMatch->canExportSkinnedModel;
                }

                const std::vector<uint32_t> texIndices = BuildTextureIndices(descriptorEntry);
                link.textureLinks.reserve(texIndices.size());
                for (const uint32_t textureIndex : texIndices)
                {
                    if (textureIndex >= descriptorEntry.textureNames.size())
                    {
                        if (localWarnings.size() < MaxWarnings)
                        {
                            localWarnings.push_back(
                                "Descriptor " + Platform::PathToUtf8(ref.descriptorPath.filename()) +
                                " resource " + descriptorEntry.name +
                                " references missing texture index " + std::to_string(textureIndex) + '.');
                        }
                        continue;
                    }

                    DescriptorTextureLink textureLink;
                    textureLink.textureIndex = textureIndex;
                    textureLink.textureName = descriptorEntry.textureNames[textureIndex];

                    const auto idIt = texNameToId.find(textureLink.textureName);
                    std::vector<std::string> fallbackTextureKeys;
                    const std::vector<std::string>* textureKeys =
                        (idIt != texNameToId.end()) ? &uniqueTexKeys[idIt->second] : nullptr;
                    if (textureKeys == nullptr)
                    {
                        fallbackTextureKeys = BuildAssetNameKeys(textureLink.textureName);
                        textureKeys = &fallbackTextureKeys;
                    }

                    if (const LightweightDxpEntry* dxpMatch = FindBestDxpEntry(
                            metaRegistry,
                            ref.metaId,
                            matchedGrpMetaId,
                            descriptorKeys[entryIndex].nameKeys,
                            dxpEntries,
                            dxpIndex,
                            *textureKeys))
                    {
                        textureLink.matchedDxpPath = dxpMatch->path;
                        textureLink.matchedDxpEntryName = dxpMatch->name;
                    }

                    link.textureLinks.push_back(std::move(textureLink));
                }

                if (!LinkMatchesFocus(link, focusRecord))
                {
                    return;
                }

                localLinks.push_back(std::move(link));

                const size_t completed = linksDone.fetch_add(1, std::memory_order_relaxed) + 1;
                if (options.progress && (completed % 250 == 0))
                {
                    std::lock_guard<std::mutex> lock(resultsMutex);
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
            });

        if (linkingCancelRequested.load())
        {
            result.canceled = true;
            return result;
        }

        size_t totalLinks = 0;
        for (size_t i = 0; i < MaxLinkingWorkerSlots; ++i)
        {
            totalLinks += threadLinks[i].size();
            for (const std::string& warning : threadWarnings[i])
            {
                AddWarning(result, warning);
            }
        }
        result.links.reserve(totalLinks);
        for (size_t i = 0; i < MaxLinkingWorkerSlots; ++i)
        {
            if (!threadLinks[i].empty())
            {
                result.links.insert(
                    result.links.end(),
                    std::make_move_iterator(threadLinks[i].begin()),
                    std::make_move_iterator(threadLinks[i].end()));
            }
        }

        const long long linkResolveMs = ElapsedMilliseconds(linkStart);
        const long long totalMs = ElapsedMilliseconds(buildStart);

        if (builtPackMetadata != nullptr)
        {
            result.packMetadata = builtPackMetadata;
            result.runtimeIndex = BuildGlobalAssetIndex(&result.links, builtPackMetadata.get());
        }
        else if (!result.links.empty())
        {
            result.runtimeIndex = BuildGlobalAssetIndex(&result.links, nullptr);
        }

        std::ostringstream timing;
        timing << "DescriptorParse=" << descParseMs << "ms"
               << ", GrpIndex=" << grpIndexMs << "ms"
               << (grpReusedCount > 0 ? (" (reused " + std::to_string(grpReusedCount) + ")") : std::string())
               << ", DxpIndex=" << dxpIndexMs << "ms"
               << (dxpReusedCount > 0 ? (" (reused " + std::to_string(dxpReusedCount) + ")") : std::string())
               << ", LinkResolve=" << linkResolveMs << "ms"
               << ", Total=" << totalMs << "ms";
        result.timingSummary = timing.str();

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
        std::string& error,
        const CachedPackMetadata* packMetadata)
    {
        BinaryWriter writer;
        writer.Reserve(1024 + links.size() * 192);

        const std::string canonicalRoot = Platform::CanonicalizePathUtf8(gameRoot);
        writer.WriteString(canonicalRoot);
        writer.WriteU64(static_cast<uint64_t>(options.maxDescriptorBytes));
        writer.WriteU64(static_cast<uint64_t>(options.maxPackBytes));

        const std::vector<SortableSourceRecord> sources = BuildDescriptorCacheSourceRecords(records);
        writer.WriteU32(static_cast<uint32_t>(sources.size()));

        StringTableBuilder pathTable;
        StringTableBuilder stringTable;

        // Reserve path IDs for all sources, links, and pack metadata
        std::vector<uint32_t> sourcePathIds;
        sourcePathIds.reserve(sources.size());
        for (const SortableSourceRecord& src : sources)
        {
            sourcePathIds.push_back(pathTable.Intern(Platform::PathToUtf8(src.record.path)));
        }

        // Pre-intern paths and strings from packMetadata (stored before links for partial reuse)
        const bool hasPackMeta = packMetadata != nullptr;
        struct EncodedGrpEntry
        {
            uint32_t nameId = 0;
            uint32_t classId = 0;
            uint32_t offset = 0;
            uint32_t size = 0;
        };
        struct EncodedGrpPack
        {
            uint32_t pathId = 0;
            uint64_t sizeBytes = 0;
            int64_t lastWriteTicks = 0;
            std::vector<EncodedGrpEntry> entries;
        };
        struct EncodedDxpPack
        {
            uint32_t pathId = 0;
            uint64_t sizeBytes = 0;
            int64_t lastWriteTicks = 0;
            std::vector<uint32_t> entryNameIds;
        };

        std::vector<EncodedGrpPack> encodedGrpPacks;
        std::vector<EncodedDxpPack> encodedDxpPacks;
        if (hasPackMeta)
        {
            encodedGrpPacks.reserve(packMetadata->grpPacks.size());
            for (const CachedGrpPackSnapshot& snap : packMetadata->grpPacks)
            {
                EncodedGrpPack enc;
                enc.pathId = pathTable.Intern(Platform::PathToUtf8(snap.path));
                enc.sizeBytes = static_cast<uint64_t>(snap.sizeBytes);
                enc.lastWriteTicks = snap.lastWriteTicks;
                enc.entries.reserve(snap.entries.size());
                for (const IndexedGrpEntry& entry : snap.entries)
                {
                    enc.entries.push_back(EncodedGrpEntry{
                        stringTable.Intern(entry.name),
                        entry.classId,
                        entry.offset,
                        entry.size
                    });
                }
                encodedGrpPacks.push_back(std::move(enc));
            }

            encodedDxpPacks.reserve(packMetadata->dxpPacks.size());
            for (const CachedDxpPackSnapshot& snap : packMetadata->dxpPacks)
            {
                EncodedDxpPack enc;
                enc.pathId = pathTable.Intern(Platform::PathToUtf8(snap.path));
                enc.sizeBytes = static_cast<uint64_t>(snap.sizeBytes);
                enc.lastWriteTicks = snap.lastWriteTicks;
                enc.entryNameIds.reserve(snap.entryNames.size());
                for (const std::string& name : snap.entryNames)
                {
                    enc.entryNameIds.push_back(stringTable.Intern(name));
                }
                encodedDxpPacks.push_back(std::move(enc));
            }
        }

        // Pre-intern paths and strings from links
        struct EncodedBinding
        {
            uint32_t materialIndex = 0;
            uint32_t textureIndex = 0;
            uint32_t slotNameId = 0;
        };
        struct EncodedTexLink
        {
            uint32_t textureIndex = 0;
            uint32_t textureNameId = 0;
            uint32_t dxpPathId = kInvalidTableId;
            uint32_t dxpEntryNameId = kInvalidTableId;
        };
        struct EncodedLink
        {
            uint32_t descriptorPathId = kInvalidTableId;
            uint32_t resourceNameId = 0;
            uint32_t resId = 0;
            uint32_t skeletonNameId = 0;
            std::vector<uint32_t> materialNameIds;
            std::vector<uint32_t> materialDomainIds;
            std::vector<uint32_t> materialShaderClassIds;
            std::vector<EncodedBinding> bindings;
            uint32_t matchedGrpPathId = kInvalidTableId;
            uint32_t matchedGrpEntryNameId = kInvalidTableId;
            uint32_t matchedGrpClassId = 0;
            uint32_t matchedGrpClassNameId = 0;
            bool canExportStatic = false;
            bool canExportSkinned = false;
            std::vector<EncodedTexLink> textureLinks;
        };

        std::vector<EncodedLink> encodedLinks;
        encodedLinks.reserve(links.size());
        for (const DescriptorResourceLink& link : links)
        {
            EncodedLink enc;
            enc.descriptorPathId = pathTable.Intern(Platform::PathToUtf8(link.descriptorPath));
            enc.resourceNameId = stringTable.Intern(link.resourceName);
            enc.resId = link.resId;
            enc.skeletonNameId = stringTable.Intern(link.skeletonName);

            enc.materialNameIds.reserve(link.materialNames.size());
            for (const std::string& v : link.materialNames) enc.materialNameIds.push_back(stringTable.Intern(v));

            enc.materialDomainIds.reserve(link.materialDomains.size());
            for (const std::string& v : link.materialDomains) enc.materialDomainIds.push_back(stringTable.Intern(v));

            enc.materialShaderClassIds.reserve(link.materialShaderClasses.size());
            for (const std::string& v : link.materialShaderClasses) enc.materialShaderClassIds.push_back(stringTable.Intern(v));

            enc.bindings.reserve(link.materialTextureBindings.size());
            for (const DescriptorMaterialTextureRef& ref : link.materialTextureBindings)
            {
                enc.bindings.push_back(EncodedBinding{
                    ref.materialIndex,
                    ref.textureIndex,
                    stringTable.Intern(ref.slotName)
                });
            }

            if (link.matchedGrpPath)
            {
                enc.matchedGrpPathId = pathTable.Intern(Platform::PathToUtf8(*link.matchedGrpPath));
            }
            if (link.matchedGrpEntryName)
            {
                enc.matchedGrpEntryNameId = stringTable.Intern(*link.matchedGrpEntryName);
            }
            enc.matchedGrpClassId = link.matchedGrpClassId;
            enc.matchedGrpClassNameId = stringTable.Intern(link.matchedGrpClassName);
            enc.canExportStatic = link.matchedGrpEntryCanExportStaticModel;
            enc.canExportSkinned = link.matchedGrpEntryCanExportSkinnedModel;

            enc.textureLinks.reserve(link.textureLinks.size());
            for (const DescriptorTextureLink& texLink : link.textureLinks)
            {
                EncodedTexLink encTex;
                encTex.textureIndex = texLink.textureIndex;
                encTex.textureNameId = stringTable.Intern(texLink.textureName);
                if (texLink.matchedDxpPath)
                {
                    encTex.dxpPathId = pathTable.Intern(Platform::PathToUtf8(*texLink.matchedDxpPath));
                }
                if (texLink.matchedDxpEntryName)
                {
                    encTex.dxpEntryNameId = stringTable.Intern(*texLink.matchedDxpEntryName);
                }
                enc.textureLinks.push_back(encTex);
            }

            encodedLinks.push_back(std::move(enc));
        }

        if (!pathTable.Write(writer, error) || !stringTable.Write(writer, error))
        {
            return false;
        }

        // Write sources
        for (size_t i = 0; i < sources.size(); ++i)
        {
            writer.WriteU32(sourcePathIds[i]);
            writer.WriteU8(static_cast<uint8_t>(sources[i].record.type));
            writer.WriteU64(static_cast<uint64_t>(sources[i].record.sizeBytes));
            writer.WriteI64(sources[i].record.lastWriteTicks);
        }

        // Write packMetadata (before links so partial/incremental rebuilds can still reuse it on stale sources)
        writer.WriteBool(hasPackMeta);
        if (hasPackMeta)
        {
            writer.WriteU32(static_cast<uint32_t>(encodedGrpPacks.size()));
            for (const EncodedGrpPack& grp : encodedGrpPacks)
            {
                writer.WriteU32(grp.pathId);
                writer.WriteU64(grp.sizeBytes);
                writer.WriteI64(grp.lastWriteTicks);
                writer.WriteU32(static_cast<uint32_t>(grp.entries.size()));
                for (const EncodedGrpEntry& e : grp.entries)
                {
                    writer.WriteU32(e.nameId);
                    writer.WriteU32(e.classId);
                    writer.WriteU32(e.offset);
                    writer.WriteU32(e.size);
                }
            }

            writer.WriteU32(static_cast<uint32_t>(encodedDxpPacks.size()));
            for (const EncodedDxpPack& dxp : encodedDxpPacks)
            {
                writer.WriteU32(dxp.pathId);
                writer.WriteU64(dxp.sizeBytes);
                writer.WriteI64(dxp.lastWriteTicks);
                writer.WriteU32(static_cast<uint32_t>(dxp.entryNameIds.size()));
                for (const uint32_t nameId : dxp.entryNameIds)
                {
                    writer.WriteU32(nameId);
                }
            }
        }

        // Write links
        writer.WriteU32(static_cast<uint32_t>(encodedLinks.size()));
        for (const EncodedLink& enc : encodedLinks)
        {
            writer.WriteU32(enc.descriptorPathId);
            writer.WriteU32(enc.resourceNameId);
            writer.WriteU32(enc.resId);
            writer.WriteU32(enc.skeletonNameId);

            auto writeIdList = [&](const std::vector<uint32_t>& ids) {
                writer.WriteU32(static_cast<uint32_t>(ids.size()));
                for (const uint32_t id : ids) writer.WriteU32(id);
            };
            writeIdList(enc.materialNameIds);
            writeIdList(enc.materialDomainIds);
            writeIdList(enc.materialShaderClassIds);

            writer.WriteU32(static_cast<uint32_t>(enc.bindings.size()));
            for (const EncodedBinding& b : enc.bindings)
            {
                writer.WriteU32(b.materialIndex);
                writer.WriteU32(b.textureIndex);
                writer.WriteU32(b.slotNameId);
            }

            writer.WriteU32(enc.matchedGrpPathId);
            writer.WriteU32(enc.matchedGrpEntryNameId);
            writer.WriteU32(enc.matchedGrpClassId);
            writer.WriteU32(enc.matchedGrpClassNameId);
            writer.WriteBool(enc.canExportStatic);
            writer.WriteBool(enc.canExportSkinned);

            writer.WriteU32(static_cast<uint32_t>(enc.textureLinks.size()));
            for (const EncodedTexLink& tex : enc.textureLinks)
            {
                writer.WriteU32(tex.textureIndex);
                writer.WriteU32(tex.textureNameId);
                writer.WriteU32(tex.dxpPathId);
                writer.WriteU32(tex.dxpEntryNameId);
            }
        }

        return WriteBinaryCacheFile(
            cachePath,
            BinaryCacheKind::DescriptorLinks,
            static_cast<uint32_t>(DescriptorLinkerVersion),
            gameRoot,
            writer.Span(),
            error);
    }

    DescriptorResourceLinkCacheLoadResult LoadDescriptorResourceLinkCache(
        const fs::path& cachePath,
        const fs::path& gameRoot,
        const std::vector<ResourceRecord>& records,
        const DescriptorResourceLinkBuildOptions& options)
    {
        DescriptorResourceLinkCacheLoadResult result;

        BinaryCacheOpenResult opened = OpenBinaryCacheFile(
            cachePath,
            BinaryCacheKind::DescriptorLinks,
            static_cast<uint32_t>(DescriptorLinkerVersion));

        if (!opened.fileFound)
        {
            result.message = "No descriptor-link cache file found.";
            return result;
        }

        result.fileFound = true;
        if (!opened.isBinaryMagic)
        {
            result.message = "Descriptor-link cache uses legacy format; rebuild required to upgrade to binary v2.";
            return result;
        }

        if (opened.corrupt)
        {
            result.corrupt = true;
            result.message = opened.error;
            return result;
        }

        if (!opened.valid)
        {
            result.message = opened.error;
            return result;
        }

        const std::string expectedCanonicalRoot = Platform::CanonicalizePathUtf8(gameRoot);
        if (opened.header.rootHash != Platform::ComputeFnv1a64(expectedCanonicalRoot))
        {
            result.message = "Descriptor-link cache belongs to another game folder.";
            return result;
        }

        BinaryCursor cursor(opened.payload);
        std::string_view cachedRootUtf8;
        uint64_t cachedMaxDesc = 0;
        uint64_t cachedMaxPack = 0;
        uint32_t sourceCount = 0;

        if (!cursor.ReadStringView(cachedRootUtf8) ||
            !cursor.ReadU64(cachedMaxDesc) ||
            !cursor.ReadU64(cachedMaxPack) ||
            !cursor.ReadU32(sourceCount))
        {
            result.corrupt = true;
            result.message = "Descriptor-link cache header is truncated.";
            return result;
        }

        result.rootMatched = (cachedRootUtf8 == expectedCanonicalRoot);
        if (!result.rootMatched)
        {
            result.message = "Descriptor-link cache belongs to another game folder.";
            return result;
        }

        result.optionsMatched =
            cachedMaxDesc == options.maxDescriptorBytes &&
            cachedMaxPack == options.maxPackBytes;
        if (!result.optionsMatched)
        {
            result.message = "Descriptor-link cache was built with incompatible size limits.";
            return result;
        }

        std::vector<std::string_view> pathTable;
        std::vector<std::string_view> stringTable;
        std::string tableError;
        if (!ReadStringTable(cursor, pathTable, tableError) ||
            !ReadStringTable(cursor, stringTable, tableError))
        {
            result.corrupt = true;
            result.message = "Descriptor-link cache string/path table error: " + tableError;
            return result;
        }

        std::vector<fs::path> decodedPaths;
        decodedPaths.reserve(pathTable.size());
        for (const std::string_view sv : pathTable)
        {
            decodedPaths.push_back(Platform::Utf8ToPath(sv));
        }

        std::vector<SortableSourceRecord> cachedSources;
        cachedSources.reserve(sourceCount);
        for (uint32_t i = 0; i < sourceCount; ++i)
        {
            uint32_t pathId = 0;
            uint8_t rawType = 0;
            uint64_t sizeBytes = 0;
            int64_t ticks = 0;
            if (!cursor.ReadU32(pathId) ||
                !cursor.ReadU8(rawType) ||
                !cursor.ReadU64(sizeBytes) ||
                !cursor.ReadI64(ticks) ||
                pathId >= decodedPaths.size() ||
                rawType > static_cast<uint8_t>(AssetType::Unknown))
            {
                result.corrupt = true;
                result.message = "Descriptor-link cache source table is malformed or truncated.";
                return result;
            }

            ResourceRecord rec;
            rec.path = decodedPaths[pathId];
            rec.type = static_cast<AssetType>(rawType);
            rec.sizeBytes = static_cast<uintmax_t>(sizeBytes);
            rec.lastWriteTicks = ticks;
            cachedSources.push_back(SortableSourceRecord{ std::move(rec), PathKey(decodedPaths[pathId]) });
        }

        bool hasPackMeta = false;
        if (!cursor.ReadBool(hasPackMeta))
        {
            result.corrupt = true;
            result.message = "Descriptor-link cache metadata flag is truncated.";
            return result;
        }

        std::shared_ptr<CachedPackMetadata> loadedPackMeta;
        if (hasPackMeta)
        {
            loadedPackMeta = std::make_shared<CachedPackMetadata>();
            uint32_t grpPackCount = 0;
            if (!cursor.ReadU32(grpPackCount))
            {
                result.corrupt = true;
                result.message = "Descriptor-link cache GRP metadata header is truncated.";
                return result;
            }

            loadedPackMeta->grpPacks.reserve(grpPackCount);
            for (uint32_t p = 0; p < grpPackCount; ++p)
            {
                uint32_t pathId = 0;
                uint64_t sizeBytes = 0;
                int64_t ticks = 0;
                uint32_t entryCount = 0;
                if (!cursor.ReadU32(pathId) ||
                    !cursor.ReadU64(sizeBytes) ||
                    !cursor.ReadI64(ticks) ||
                    !cursor.ReadU32(entryCount) ||
                    pathId >= decodedPaths.size())
                {
                    result.corrupt = true;
                    result.message = "Descriptor-link cache GRP pack metadata is malformed or truncated.";
                    return result;
                }

                CachedGrpPackSnapshot snap;
                snap.path = decodedPaths[pathId];
                snap.sizeBytes = static_cast<uintmax_t>(sizeBytes);
                snap.lastWriteTicks = ticks;
                snap.entries.reserve(entryCount);

                for (uint32_t e = 0; e < entryCount; ++e)
                {
                    uint32_t nameId = 0;
                    uint32_t classId = 0;
                    uint32_t offset = 0;
                    uint32_t size = 0;
                    if (!cursor.ReadU32(nameId) ||
                        !cursor.ReadU32(classId) ||
                        !cursor.ReadU32(offset) ||
                        !cursor.ReadU32(size) ||
                        nameId >= stringTable.size())
                    {
                        result.corrupt = true;
                        result.message = "Descriptor-link cache GRP pack entry is malformed or truncated.";
                        return result;
                    }
                    const std::string_view nameView = stringTable[nameId];
                    snap.entries.push_back(IndexedGrpEntry{
                        snap.path,
                        std::string(nameView.data(), nameView.size()),
                        classId,
                        offset,
                        size
                    });
                }

                loadedPackMeta->grpPacks.push_back(std::move(snap));
            }

            uint32_t dxpPackCount = 0;
            if (!cursor.ReadU32(dxpPackCount))
            {
                result.corrupt = true;
                result.message = "Descriptor-link cache DXP metadata header is truncated.";
                return result;
            }

            loadedPackMeta->dxpPacks.reserve(dxpPackCount);
            for (uint32_t p = 0; p < dxpPackCount; ++p)
            {
                uint32_t pathId = 0;
                uint64_t sizeBytes = 0;
                int64_t ticks = 0;
                uint32_t entryCount = 0;
                if (!cursor.ReadU32(pathId) ||
                    !cursor.ReadU64(sizeBytes) ||
                    !cursor.ReadI64(ticks) ||
                    !cursor.ReadU32(entryCount) ||
                    pathId >= decodedPaths.size())
                {
                    result.corrupt = true;
                    result.message = "Descriptor-link cache DXP pack metadata is malformed or truncated.";
                    return result;
                }

                CachedDxpPackSnapshot snap;
                snap.path = decodedPaths[pathId];
                snap.sizeBytes = static_cast<uintmax_t>(sizeBytes);
                snap.lastWriteTicks = ticks;
                snap.entryNames.reserve(entryCount);

                for (uint32_t e = 0; e < entryCount; ++e)
                {
                    uint32_t nameId = 0;
                    if (!cursor.ReadU32(nameId) || nameId >= stringTable.size())
                    {
                        result.corrupt = true;
                        result.message = "Descriptor-link cache DXP pack entry is malformed or truncated.";
                        return result;
                    }
                    const std::string_view nameView = stringTable[nameId];
                    snap.entryNames.emplace_back(nameView.data(), nameView.size());
                }

                loadedPackMeta->dxpPacks.push_back(std::move(snap));
            }

            result.packMetadata = loadedPackMeta;
        }

        const std::vector<SortableSourceRecord> expectedSources = BuildDescriptorCacheSourceRecords(records);
        result.sourcesMatched = SameDescriptorCacheSources(cachedSources, expectedSources);
        if (!result.sourcesMatched)
        {
            result.message = "Descriptor-link cache is stale because resource records changed (pack metadata preserved for incremental update).";
            return result;
        }

        uint32_t linkCount = 0;
        if (!cursor.ReadU32(linkCount))
        {
            result.corrupt = true;
            result.message = "Descriptor-link cache link count is truncated.";
            return result;
        }

        std::vector<DescriptorResourceLink> loadedLinks;
        loadedLinks.reserve(linkCount);

        auto readRequiredString = [&](std::string& out) -> bool {
            uint32_t id = 0;
            if (!cursor.ReadU32(id) || id >= stringTable.size())
            {
                return false;
            }
            const std::string_view sv = stringTable[id];
            out.assign(sv.data(), sv.size());
            return true;
        };

        auto readIdList = [&](std::vector<std::string>& out) -> bool {
            std::string dummyErr;
            return ReadStringVectorFromTable(cursor, stringTable, out, dummyErr);
        };

        for (uint32_t i = 0; i < linkCount; ++i)
        {
            DescriptorResourceLink link;
            uint32_t descPathId = 0;
            if (!cursor.ReadU32(descPathId) || descPathId >= decodedPaths.size())
            {
                result.corrupt = true;
                result.message = "Descriptor-link cache descriptor path ID is out of bounds or truncated.";
                return result;
            }
            link.descriptorPath = decodedPaths[descPathId];

            if (!readRequiredString(link.resourceName) ||
                !cursor.ReadU32(link.resId) ||
                !readRequiredString(link.skeletonName) ||
                !readIdList(link.materialNames) ||
                !readIdList(link.materialDomains) ||
                !readIdList(link.materialShaderClasses))
            {
                result.corrupt = true;
                result.message = "Descriptor-link cache link fields are malformed or truncated.";
                return result;
            }

            uint32_t bindingCount = 0;
            if (!cursor.ReadU32(bindingCount))
            {
                result.corrupt = true;
                result.message = "Descriptor-link cache binding count is truncated.";
                return result;
            }
            link.materialTextureBindings.reserve(bindingCount);
            for (uint32_t b = 0; b < bindingCount; ++b)
            {
                DescriptorMaterialTextureRef ref;
                if (!cursor.ReadU32(ref.materialIndex) ||
                    !cursor.ReadU32(ref.textureIndex) ||
                    !readRequiredString(ref.slotName))
                {
                    result.corrupt = true;
                    result.message = "Descriptor-link cache binding entry is malformed or truncated.";
                    return result;
                }
                link.materialTextureBindings.push_back(std::move(ref));
            }

            uint32_t grpPathId = kInvalidTableId;
            uint32_t grpEntryNameId = kInvalidTableId;
            if (!cursor.ReadU32(grpPathId) ||
                !cursor.ReadU32(grpEntryNameId) ||
                !cursor.ReadU32(link.matchedGrpClassId) ||
                !readRequiredString(link.matchedGrpClassName) ||
                !cursor.ReadBool(link.matchedGrpEntryCanExportStaticModel) ||
                !cursor.ReadBool(link.matchedGrpEntryCanExportSkinnedModel))
            {
                result.corrupt = true;
                result.message = "Descriptor-link cache GRP match fields are malformed or truncated.";
                return result;
            }

            if (grpPathId != kInvalidTableId)
            {
                if (grpPathId >= decodedPaths.size())
                {
                    result.corrupt = true;
                    result.message = "Descriptor-link cache matched GRP path ID is out of bounds.";
                    return result;
                }
                link.matchedGrpPath = decodedPaths[grpPathId];
            }
            if (grpEntryNameId != kInvalidTableId)
            {
                if (grpEntryNameId >= stringTable.size())
                {
                    result.corrupt = true;
                    result.message = "Descriptor-link cache matched GRP entry name ID is out of bounds.";
                    return result;
                }
                const std::string_view sv = stringTable[grpEntryNameId];
                link.matchedGrpEntryName.emplace(sv.data(), sv.size());
            }

            uint32_t texLinkCount = 0;
            if (!cursor.ReadU32(texLinkCount))
            {
                result.corrupt = true;
                result.message = "Descriptor-link cache texture link count is truncated.";
                return result;
            }
            link.textureLinks.reserve(texLinkCount);
            for (uint32_t t = 0; t < texLinkCount; ++t)
            {
                DescriptorTextureLink tex;
                uint32_t dxpPathId = kInvalidTableId;
                uint32_t dxpNameId = kInvalidTableId;
                if (!cursor.ReadU32(tex.textureIndex) ||
                    !readRequiredString(tex.textureName) ||
                    !cursor.ReadU32(dxpPathId) ||
                    !cursor.ReadU32(dxpNameId))
                {
                    result.corrupt = true;
                    result.message = "Descriptor-link cache texture link entry is malformed or truncated.";
                    return result;
                }

                if (dxpPathId != kInvalidTableId)
                {
                    if (dxpPathId >= decodedPaths.size())
                    {
                        result.corrupt = true;
                        result.message = "Descriptor-link cache DXP path ID is out of bounds.";
                        return result;
                    }
                    tex.matchedDxpPath = decodedPaths[dxpPathId];
                }
                if (dxpNameId != kInvalidTableId)
                {
                    if (dxpNameId >= stringTable.size())
                    {
                        result.corrupt = true;
                        result.message = "Descriptor-link cache DXP entry name ID is out of bounds.";
                        return result;
                    }
                    const std::string_view sv = stringTable[dxpNameId];
                    if (!sv.empty())
                    {
                        tex.matchedDxpEntryName.emplace(sv.data(), sv.size());
                    }
                }

                if (tex.textureIndex < 100000u)
                {
                    if (tex.textureIndex >= link.textureNames.size())
                    {
                        link.textureNames.resize(static_cast<size_t>(tex.textureIndex) + 1);
                    }
                    link.textureNames[tex.textureIndex] = tex.textureName;
                }

                link.textureLinks.push_back(std::move(tex));
            }

            loadedLinks.push_back(std::move(link));
        }

        result.links = std::move(loadedLinks);
        result.runtimeIndex = BuildGlobalAssetIndex(&result.links, loadedPackMeta.get());
        result.loaded = true;
        result.message = "Loaded " + std::to_string(result.links.size()) + " cached descriptor links.";
        return result;
    }
}
