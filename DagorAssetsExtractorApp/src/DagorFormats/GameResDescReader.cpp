#include "GameResDescReader.h"

#include "DagorIO/Decompression.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace DagorFormats {
namespace {

    constexpr uint8_t kGameResDescEnvelopeMagic = 0x02;
    constexpr uint8_t kBinaryBlkPayloadMagic = 0x01;
    constexpr uint32_t kZstdMagic = 0xFD2FB528;

    uint32_t ReadUInt32At(std::span<const uint8_t> data, size_t offset)
    {
        if (offset + 4 > data.size())
        {
            throw std::runtime_error("Unexpected end of descriptor data");
        }

        return static_cast<uint32_t>(data[offset + 0]) |
               (static_cast<uint32_t>(data[offset + 1]) << 8) |
               (static_cast<uint32_t>(data[offset + 2]) << 16) |
               (static_cast<uint32_t>(data[offset + 3]) << 24);
    }

    uint32_t ReadUInt24At(std::span<const uint8_t> data, size_t offset)
    {
        if (offset + 3 > data.size())
        {
            throw std::runtime_error("Unexpected end of descriptor data");
        }

        return static_cast<uint32_t>(data[offset + 0]) |
               (static_cast<uint32_t>(data[offset + 1]) << 8) |
               (static_cast<uint32_t>(data[offset + 2]) << 16);
    }

    std::string ReadNullTerminatedString(std::span<const uint8_t> data, size_t offset)
    {
        if (offset >= data.size())
        {
            throw std::runtime_error("String offset outside descriptor data");
        }

        size_t end = offset;
        while (end < data.size() && data[end] != 0)
        {
            ++end;
        }

        if (end == data.size())
        {
            throw std::runtime_error("Unterminated string in descriptor data");
        }

        return std::string(reinterpret_cast<const char*>(data.data() + offset), end - offset);
    }

    uint32_t DecodeVlq(DagorIO::MemoryReader& reader, int step, int end, int shift = 1)
    {
        uint32_t result = 0;
        for (int i = 0; i < end; i += step)
        {
            const uint8_t raw = reader.ReadUInt8();
            const int bitShift = i * shift;
            if (bitShift >= 32)
            {
                throw std::runtime_error("Descriptor VLQ shift overflow");
            }

            result |= (static_cast<uint32_t>(raw & 0x7F) << bitShift);
            if ((raw & 0x80) == 0)
            {
                return result;
            }
        }

        throw std::runtime_error("Descriptor VLQ is too long");
    }

    struct BinaryBlkParam
    {
        std::string name;
        uint32_t flags = 0;
        uint32_t rawValue = 0;
        std::optional<std::string> stringValue;
    };

    struct BinaryBlkBlock
    {
        std::string name;
        uint32_t paramsCount = 0;
        uint32_t childCount = 0;
        uint32_t firstChild = 0;
        size_t paramsOffset = 0;
        std::vector<size_t> children;
    };

    struct BinaryBlkDocument
    {
        std::vector<std::string> names;
        std::span<const uint8_t> paramsData;
        std::span<const uint8_t> params;
        std::vector<BinaryBlkBlock> blocks;
    };

    std::vector<std::string> ParseNameMap(std::span<const uint8_t> bytes, uint32_t expectedCount)
    {
        if (bytes.size() < 2)
        {
            throw std::runtime_error("Descriptor BLK name map is too small");
        }

        std::vector<std::string> names;
        size_t start = 2; // Dagor binary BLK name maps carry a two-byte prefix.
        for (size_t i = start; i < bytes.size(); ++i)
        {
            if (bytes[i] != 0)
            {
                continue;
            }

            if (i > start)
            {
                names.emplace_back(reinterpret_cast<const char*>(bytes.data() + start), i - start);
            }
            start = i + 1;
        }

        if (expectedCount != 0 && names.size() + 1 < expectedCount)
        {
            throw std::runtime_error("Descriptor BLK name map is truncated");
        }
        while (expectedCount != 0 && names.size() < expectedCount)
        {
            // Live War Thunder descriptors count one trailing sentinel name that is
            // not present in the null-terminated name payload.
            names.emplace_back();
        }

        return names;
    }

    std::string ResolveBlockName(const std::vector<std::string>& names, uint32_t encodedNameId)
    {
        // Dagor Asset Explorer treats block name ids as two-based in this binary BLK flavor.
        if (encodedNameId >= 2)
        {
            const size_t index = static_cast<size_t>(encodedNameId - 2);
            if (index < names.size())
            {
                return names[index];
            }
        }

        return {};
    }

    std::string ResolveParamName(const std::vector<std::string>& names, uint32_t flags)
    {
        const uint32_t encodedNameId = flags & 0x00FFFFFFu;
        if (encodedNameId == 0)
        {
            return {};
        }

        const size_t index = static_cast<size_t>(encodedNameId - 1);
        if (index < names.size())
        {
            return names[index];
        }

        return {};
    }

    BinaryBlkDocument ParseBinaryBlk(std::span<const uint8_t> payload)
    {
        DagorIO::MemoryReader reader(payload);
        const uint8_t magic = reader.ReadUInt8();
        if (magic != kBinaryBlkPayloadMagic)
        {
            throw std::runtime_error("Descriptor payload is not a supported binary BLK block");
        }

        const uint32_t namesCount = DecodeVlq(reader, 1, 5, 7);
        const uint32_t namesSize = DecodeVlq(reader, 1, 5, 7);
        if (reader.GetRemaining() < namesSize)
        {
            throw std::runtime_error("Descriptor BLK name map extends past end of payload");
        }

        const std::span<const uint8_t> nameBytes = reader.ReadSpan(namesSize);

        const uint32_t blocksCount = DecodeVlq(reader, 7, 0x23);
        const uint32_t paramsCount = DecodeVlq(reader, 7, 0x23);
        const uint32_t paramsDataSize = DecodeVlq(reader, 7, 0x23);
        if (blocksCount == 0)
        {
            throw std::runtime_error("Descriptor BLK contains no blocks");
        }
        if (reader.GetRemaining() < paramsDataSize)
        {
            throw std::runtime_error("Descriptor BLK parameter data extends past end of payload");
        }

        BinaryBlkDocument document;
        document.names = ParseNameMap(nameBytes, namesCount);
        document.paramsData = reader.ReadSpan(paramsDataSize);

        const size_t paramsBytes = static_cast<size_t>(paramsCount) * 8;
        if (reader.GetRemaining() < paramsBytes)
        {
            throw std::runtime_error("Descriptor BLK parameter table extends past end of payload");
        }
        document.params = reader.ReadSpan(paramsBytes);

        document.blocks.reserve(blocksCount);
        size_t nextParamsOffset = 0;
        for (uint32_t blockIndex = 0; blockIndex < blocksCount; ++blockIndex)
        {
            const uint32_t encodedNameId = DecodeVlq(reader, 7, 0x23);
            BinaryBlkBlock block;
            block.name = ResolveBlockName(document.names, encodedNameId);
            block.paramsCount = DecodeVlq(reader, 7, 0x15);
            block.childCount = DecodeVlq(reader, 7, 0x15);
            if (block.childCount != 0)
            {
                block.firstChild = DecodeVlq(reader, 7, 0x23);
            }
            block.paramsOffset = nextParamsOffset;

            const size_t blockParamsBytes = static_cast<size_t>(block.paramsCount) * 8;
            if (nextParamsOffset + blockParamsBytes > document.params.size())
            {
                throw std::runtime_error("Descriptor BLK block parameter range is invalid");
            }
            nextParamsOffset += blockParamsBytes;

            document.blocks.push_back(std::move(block));
        }

        for (size_t blockIndex = 0; blockIndex < document.blocks.size(); ++blockIndex)
        {
            BinaryBlkBlock& block = document.blocks[blockIndex];
            if (block.childCount == 0)
            {
                continue;
            }

            if (static_cast<size_t>(block.firstChild) + block.childCount > document.blocks.size())
            {
                throw std::runtime_error("Descriptor BLK child block range is invalid");
            }

            block.children.reserve(block.childCount);
            for (uint32_t i = 0; i < block.childCount; ++i)
            {
                block.children.push_back(static_cast<size_t>(block.firstChild + i));
            }
        }

        return document;
    }

    BinaryBlkParam ReadParam(const BinaryBlkDocument& document, const BinaryBlkBlock& block, uint32_t paramIndex)
    {
        if (paramIndex >= block.paramsCount)
        {
            throw std::runtime_error("Descriptor BLK parameter index is invalid");
        }

        const size_t offset = block.paramsOffset + static_cast<size_t>(paramIndex) * 8;
        BinaryBlkParam param;
        param.flags = ReadUInt32At(document.params, offset);
        param.rawValue = ReadUInt32At(document.params, offset + 4);
        param.name = ResolveParamName(document.names, param.flags);

        const uint32_t valueType = param.flags & 0x0F000000u;
        if (valueType == 0x01000000u)
        {
            param.stringValue = ReadNullTerminatedString(document.paramsData, param.rawValue);
        }

        return param;
    }

    std::optional<std::string> FindStringParam(
        const BinaryBlkDocument& document,
        const BinaryBlkBlock& block,
        std::string_view name)
    {
        for (uint32_t i = 0; i < block.paramsCount; ++i)
        {
            const BinaryBlkParam param = ReadParam(document, block, i);
            if (param.name == name && param.stringValue)
            {
                return param.stringValue;
            }
        }

        return std::nullopt;
    }

    bool IsMaterialContainer(std::string_view name)
    {
        return name == "mat" || name == "matR" || name == "matS";
    }

    void AppendUnique(std::vector<uint32_t>& values, uint32_t value)
    {
        if (std::find(values.begin(), values.end(), value) == values.end())
        {
            values.push_back(value);
        }
    }

    void AppendUnique(std::vector<std::string>& values, const std::string& value)
    {
        if (!value.empty() && std::find(values.begin(), values.end(), value) == values.end())
        {
            values.push_back(value);
        }
    }

    void AppendUniqueMaterialTextureRef(
        std::vector<GameResDescMaterialTextureRef>& values,
        uint32_t materialIndex,
        uint32_t textureIndex,
        std::string slotName)
    {
        const auto existing = std::find_if(values.begin(), values.end(), [&](const GameResDescMaterialTextureRef& ref) {
            return ref.materialIndex == materialIndex &&
                   ref.textureIndex == textureIndex &&
                   ref.slotName == slotName;
        });
        if (existing == values.end())
        {
            GameResDescMaterialTextureRef ref;
            ref.materialIndex = materialIndex;
            ref.textureIndex = textureIndex;
            ref.slotName = std::move(slotName);
            values.push_back(std::move(ref));
        }
    }

    using DescriptorTextureSlots = std::array<std::string, 11>;

    bool TryParseTextureSlotIndex(std::string_view name, size_t& slotIndex)
    {
        if (name.size() < 2 || name[0] != 't')
        {
            return false;
        }

        size_t value = 0;
        for (size_t i = 1; i < name.size(); ++i)
        {
            const unsigned char c = static_cast<unsigned char>(name[i]);
            if (std::isdigit(c) == 0)
            {
                return false;
            }
            value = (value * 10) + static_cast<size_t>(name[i] - '0');
        }

        if (value >= std::tuple_size_v<DescriptorTextureSlots>)
        {
            return false;
        }

        slotIndex = value;
        return true;
    }

    std::vector<std::string> SplitAscii(std::string_view value, char separator)
    {
        std::vector<std::string> parts;
        size_t start = 0;
        while (start <= value.size())
        {
            const size_t end = value.find(separator, start);
            if (end == std::string_view::npos)
            {
                parts.emplace_back(value.substr(start));
                break;
            }
            parts.emplace_back(value.substr(start, end - start));
            start = end + 1;
        }
        return parts;
    }

    std::string JoinAscii(const std::vector<std::string>& parts, char separator)
    {
        std::ostringstream out;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (i != 0)
            {
                out << separator;
            }
            out << parts[i];
        }
        return out.str();
    }

    std::string TextureNameToMaterialComponent(std::string value)
    {
        const size_t star = value.find('*');
        if (star != std::string::npos)
        {
            value.resize(star);
        }

        std::vector<std::string> parts = SplitAscii(value, '_');
        bool removedTextureToken = false;
        for (size_t i = parts.size(); i > 1; --i)
        {
            const size_t partIndex = i - 1;
            if (parts[partIndex] != "tex")
            {
                continue;
            }

            if (partIndex + 1 < parts.size() && parts[partIndex + 1].size() <= 2)
            {
                parts.erase(parts.begin() + static_cast<std::ptrdiff_t>(partIndex + 1));
            }
            parts.erase(parts.begin() + static_cast<std::ptrdiff_t>(partIndex));
            removedTextureToken = true;
            break;
        }

        if (!removedTextureToken && !parts.empty() && parts.back().size() < 2)
        {
            parts.pop_back();
        }

        return JoinAscii(parts, '_');
    }

    void AppendMaterialNameComponent(std::vector<std::string>& components, const std::string& textureName)
    {
        if (!textureName.empty())
        {
            std::string component = TextureNameToMaterialComponent(textureName);
            if (!component.empty())
            {
                components.push_back(std::move(component));
            }
        }
    }

    bool ContainsAscii(std::string_view value, std::string_view needle)
    {
        return value.find(needle) != std::string_view::npos;
    }

    std::string GenerateMaterialName(std::string_view shaderClass, const DescriptorTextureSlots& slots)
    {
        bool hasTexture = false;
        for (const std::string& slot : slots)
        {
            if (!slot.empty())
            {
                hasTexture = true;
                break;
            }
        }

        if (!hasTexture)
        {
            return std::string(shaderClass);
        }

        std::vector<std::string> components;
        const bool layered = ContainsAscii(shaderClass, "layered");
        const bool dynamic = ContainsAscii(shaderClass, "dynamic");

        if (!layered)
        {
            AppendMaterialNameComponent(components, slots[0]);
            AppendMaterialNameComponent(components, slots[1]);
        }

        size_t detailStart = 4;
        if (!dynamic || layered)
        {
            detailStart = 3;
        }

        for (size_t i = detailStart; i < slots.size(); i += 2)
        {
            AppendMaterialNameComponent(components, slots[i]);
            if (slots[i].empty() && i + 1 < slots.size())
            {
                AppendMaterialNameComponent(components, slots[i + 1]);
            }
        }

        return JoinAscii(components, '_');
    }

    std::string MakeMaterialNameUniqueIfNeeded(
        const std::vector<std::string>& existingNames,
        const std::vector<DescriptorTextureSlots>& existingSlots,
        std::string candidate,
        const DescriptorTextureSlots& candidateSlots,
        uint32_t materialIndex)
    {
        if (candidate.empty())
        {
            candidate = "material_" + std::to_string(materialIndex);
        }

        for (size_t i = 0; i < existingNames.size(); ++i)
        {
            if (existingNames[i] != candidate)
            {
                continue;
            }
            if (i < existingSlots.size() && existingSlots[i] == candidateSlots)
            {
                return candidate;
            }

            const std::string base = candidate;
            for (uint32_t suffix = 1; suffix < 100000; ++suffix)
            {
                std::string suffixed = base + "_" + std::to_string(suffix);
                if (std::find(existingNames.begin(), existingNames.end(), suffixed) == existingNames.end())
                {
                    return suffixed;
                }
            }

            return base + "_" + std::to_string(materialIndex);
        }

        return candidate;
    }

    std::string MaterialDomainForBlock(std::string_view blockName)
    {
        return blockName == "matS" ? "skinned" : "rigid";
    }

    std::string MakeDescriptorClassSummary(const GameResDescEntry& entry)
    {
        std::ostringstream out;
        if (!entry.materials.empty())
        {
            out << "Model";
        }
        else if (!entry.textures.empty())
        {
            out << "Textured resource";
        }
        else
        {
            out << "Resource";
        }

        if (!entry.textures.empty())
        {
            out << " " << entry.textures.size() << " tex";
        }
        if (!entry.materials.empty())
        {
            out << " " << entry.materials.size() << " mat";
        }

        return out.str();
    }

    GameResDescEntry BuildEntryFromBlock(
        const BinaryBlkDocument& document,
        size_t blockIndex,
        uint32_t resId)
    {
        const BinaryBlkBlock& block = document.blocks[blockIndex];

        GameResDescEntry entry;
        entry.name = block.name.empty() ? ("resource_" + std::to_string(resId)) : block.name;
        entry.resId = resId;

        // Read parameters of the resource block itself
        for (uint32_t i = 0; i < block.paramsCount; ++i)
        {
            const BinaryBlkParam param = ReadParam(document, block, i);
            if (param.stringValue)
            {
                if (param.name == "skeleton")
                {
                    entry.skeletonName = *param.stringValue;
                }
                else if (param.name == "dynmodel")
                {
                    entry.dynModelName = *param.stringValue;
                }
            }
        }

        for (const size_t childIndex : block.children)
        {
            const BinaryBlkBlock& child = document.blocks[childIndex];
            if (child.name == "tex")
            {
                for (uint32_t i = 0; i < child.paramsCount; ++i)
                {
                    const BinaryBlkParam param = ReadParam(document, child, i);
                    if (param.stringValue)
                    {
                        AppendUnique(entry.textures, *param.stringValue);
                    }
                }
                continue;
            }
        }

        std::vector<DescriptorTextureSlots> materialSlots;
        constexpr std::string_view materialContainerOrder[] = { "matR", "mat", "matS" };
        for (const std::string_view materialContainerName : materialContainerOrder)
        {
            for (const size_t childIndex : block.children)
            {
                const BinaryBlkBlock& child = document.blocks[childIndex];
                if (child.name != materialContainerName)
                {
                    continue;
                }

                for (const size_t materialBlockIndex : child.children)
                {
                    const BinaryBlkBlock& materialBlock = document.blocks[materialBlockIndex];
                    DescriptorTextureSlots slots = {};
                    std::string shaderClass;
                    if (const std::optional<std::string> clsParam = FindStringParam(document, materialBlock, "cls"))
                    {
                        shaderClass = *clsParam;
                        AppendUnique(entry.shaders, *clsParam);
                    }

                    const uint32_t materialIndex = static_cast<uint32_t>(entry.materials.size());
                    for (uint32_t i = 0; i < materialBlock.paramsCount; ++i)
                    {
                        const BinaryBlkParam param = ReadParam(document, materialBlock, i);
                        const uint32_t valueType = param.flags & 0x0F000000u;
                        if (valueType == 0x02000000u)
                        {
                            size_t slotIndex = 0;
                            if (TryParseTextureSlotIndex(param.name, slotIndex))
                            {
                                AppendUnique(entry.dependencyIds, param.rawValue);
                                if (param.rawValue < entry.textures.size())
                                {
                                    slots[slotIndex] = entry.textures[param.rawValue];
                                }
                                AppendUniqueMaterialTextureRef(
                                    entry.materialTextureRefs,
                                    materialIndex,
                                    param.rawValue,
                                    param.name);
                            }
                        }
                    }

                    std::string materialName = GenerateMaterialName(shaderClass, slots);
                    if (materialName.empty())
                    {
                        materialName = shaderClass.empty() ? materialBlock.name : shaderClass;
                    }
                    materialName = MakeMaterialNameUniqueIfNeeded(
                        entry.materials,
                        materialSlots,
                        std::move(materialName),
                        slots,
                        materialIndex);

                    entry.materials.push_back(std::move(materialName));
                    entry.materialDomains.push_back(MaterialDomainForBlock(child.name));
                    entry.materialShaderClasses.push_back(std::move(shaderClass));
                    materialSlots.push_back(std::move(slots));
                }
            }
        }

        entry.className = MakeDescriptorClassSummary(entry);
        return entry;
    }

    bool LooksLikeLegacyFlatTable(std::span<const uint8_t> data)
    {
        if (data.size() < 8)
        {
            return false;
        }

        const uint32_t resCount = ReadUInt32At(data, 0);
        const uint32_t strTableOffset = ReadUInt32At(data, 4);
        const size_t entryTableBytes = static_cast<size_t>(resCount) * 20;
        return resCount > 0 &&
               resCount <= 100000 &&
               8 + entryTableBytes <= data.size() &&
               strTableOffset >= 8 + entryTableBytes &&
               strTableOffset < data.size();
    }

    bool BlockMatchesResourceFilter(
        const BinaryBlkDocument& document,
        const BinaryBlkBlock& block,
        const std::unordered_set<std::string>& resourceNameFilter)
    {
        if (resourceNameFilter.contains(block.name))
        {
            return true;
        }

        for (uint32_t i = 0; i < block.paramsCount; ++i)
        {
            const BinaryBlkParam param = ReadParam(document, block, i);
            if (param.stringValue && resourceNameFilter.contains(*param.stringValue))
            {
                return true;
            }
        }

        return false;
    }

    void ParseLegacyFlatTable(
        std::span<const uint8_t> data,
        std::vector<GameResDescEntry>& entries,
        const std::unordered_set<std::string>* resourceNameFilter)
    {
        DagorIO::MemoryReader reader(data);
        const uint32_t resCount = reader.ReadUInt32();
        const uint32_t strTableOffset = reader.ReadUInt32();

        entries.clear();
        entries.reserve(resCount);

        for (uint32_t i = 0; i < resCount; ++i)
        {
            const uint32_t nameOffsetRel = reader.ReadUInt32();
            const uint32_t classOffsetRel = reader.ReadUInt32();
            const uint32_t resId = reader.ReadUInt32();
            const uint32_t numDeps = reader.ReadUInt32();
            const uint32_t depsOffset = reader.ReadUInt32();

            GameResDescEntry entry;
            entry.resId = resId;
            entry.name = ReadNullTerminatedString(data, strTableOffset + nameOffsetRel);
            entry.className = ReadNullTerminatedString(data, strTableOffset + classOffsetRel);
            if (resourceNameFilter != nullptr &&
                !resourceNameFilter->empty() &&
                !resourceNameFilter->contains(entry.name))
            {
                continue;
            }

            if (numDeps > 0)
            {
                const size_t depsBytes = static_cast<size_t>(numDeps) * 4;
                if (static_cast<size_t>(depsOffset) + depsBytes <= data.size())
                {
                    entry.dependencyIds.reserve(numDeps);
                    for (uint32_t dep = 0; dep < numDeps; ++dep)
                    {
                        entry.dependencyIds.push_back(ReadUInt32At(data, depsOffset + static_cast<size_t>(dep) * 4));
                    }
                }
            }

            entries.push_back(std::move(entry));
        }
    }

    std::span<const uint8_t> PrepareDescriptorPayload(
        std::span<const uint8_t> data,
        std::vector<uint8_t>& decompressedData)
    {
        if (data.empty())
        {
            throw std::runtime_error("GameResDesc data is empty");
        }

        if (data[0] == kGameResDescEnvelopeMagic)
        {
            if (data.size() < 4)
            {
                throw std::runtime_error("GameResDesc zstd envelope is truncated");
            }

            const uint32_t compressedSize = ReadUInt24At(data, 1);
            if (compressedSize == 0 || static_cast<size_t>(compressedSize) > data.size() - 4)
            {
                throw std::runtime_error("GameResDesc zstd envelope has an invalid compressed size");
            }

            decompressedData = DagorIO::Decompressor::DecompressZstd(data.subspan(4, compressedSize));
            return decompressedData;
        }

        if (data.size() >= 4 && ReadUInt32At(data, 0) == kZstdMagic)
        {
            decompressedData = DagorIO::Decompressor::DecompressZstd(data);
            return decompressedData;
        }

        return data;
    }

} // namespace

GameResDescReader::GameResDescReader(std::span<const uint8_t> data) : m_data(data) {}

void GameResDescReader::Parse(const std::vector<std::string>* resourceNameFilter)
{
    std::vector<uint8_t> decompressedData;
    const std::span<const uint8_t> payload = PrepareDescriptorPayload(m_data, decompressedData);
    std::unordered_set<std::string> resourceNameSet;
    if (resourceNameFilter != nullptr)
    {
        resourceNameSet.reserve(resourceNameFilter->size());
        for (const std::string& name : *resourceNameFilter)
        {
            if (!name.empty())
            {
                resourceNameSet.insert(name);
            }
        }
    }
    const std::unordered_set<std::string>* filter =
        resourceNameSet.empty() ? nullptr : &resourceNameSet;

    if (payload.empty())
    {
        throw std::runtime_error("GameResDesc payload is empty");
    }

    if (payload[0] == kBinaryBlkPayloadMagic)
    {
        const BinaryBlkDocument document = ParseBinaryBlk(payload);
        if (document.blocks.empty())
        {
            throw std::runtime_error("GameResDesc binary BLK contains no root block");
        }

        m_entries.clear();
        const BinaryBlkBlock& root = document.blocks[0];
        m_entries.reserve(filter == nullptr
            ? root.children.size()
            : std::min(root.children.size(), resourceNameSet.size()));
        uint32_t resId = 0;
        for (const size_t childIndex : root.children)
        {
            const BinaryBlkBlock& child = document.blocks[childIndex];
            if (filter != nullptr && !BlockMatchesResourceFilter(document, child, *filter))
            {
                ++resId;
                continue;
            }
            m_entries.push_back(BuildEntryFromBlock(document, childIndex, resId));
            ++resId;
        }

        return;
    }

    if (LooksLikeLegacyFlatTable(payload))
    {
        ParseLegacyFlatTable(payload, m_entries, filter);
        return;
    }

    throw std::runtime_error("Unsupported GameResDesc binary layout");
}

} // namespace DagorFormats
