#include "BlkReader.h"

#include "DagorIO/BinaryReader.h"
#include "DagorIO/Decompression.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <span>

namespace DagorFormats
{
    namespace
    {
        bool IsDelimiter(char c)
        {
            return std::isspace(static_cast<unsigned char>(c)) ||
                   c == ':' || c == '=' || c == '{' || c == '}' ||
                   c == ';' || c == ',';
        }

        void TrimInPlace(std::string& value)
        {
            const auto isNotSpace = [](unsigned char c) {
                return std::isspace(c) == 0;
            };

            value.erase(value.begin(), std::find_if(value.begin(), value.end(), isNotSpace));
            value.erase(std::find_if(value.rbegin(), value.rend(), isNotSpace).base(), value.end());
        }

        uint32_t ReadVlq(DagorIO::MemoryReader& reader)
        {
            uint32_t result = 0;
            int bitShift = 0;
            while (bitShift < 35)
            {
                const uint8_t raw = reader.ReadUInt8();
                result |= (static_cast<uint32_t>(raw & 0x7F) << bitShift);
                if ((raw & 0x80) == 0)
                {
                    return result;
                }
                bitShift += 7;
            }
            throw std::runtime_error("BLK VLQ is too long");
        }

        std::vector<std::string_view> ParseBlkNameMap(std::span<const uint8_t> bytes, uint32_t expectedCount)
        {
            if (bytes.size() < 2)
            {
                return {};
            }

            std::vector<std::string_view> names;
            if (expectedCount > 0)
            {
                names.reserve(expectedCount);
            }
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

            return names;
        }

        std::string_view ResolveBlkName(const std::vector<std::string_view>& names, uint32_t encodedNameId, bool isBlock)
        {
            const size_t offset = isBlock ? 2 : 1;
            if (encodedNameId >= offset)
            {
                const size_t index = static_cast<size_t>(encodedNameId - offset);
                if (index < names.size())
                {
                    return names[index];
                }
            }
            return {};
        }

        float ReadFloat(std::span<const uint8_t> data, size_t offset)
        {
            if (offset + 4 > data.size()) return 0.0f;
            uint32_t raw = static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
                           (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
            float f;
            std::memcpy(&f, &raw, sizeof(float));
            return f;
        }

        int32_t ReadInt32(std::span<const uint8_t> data, size_t offset)
        {
            if (offset + 4 > data.size()) return 0;
            return static_cast<int32_t>(data[offset]) | (static_cast<int32_t>(data[offset + 1]) << 8) |
                   (static_cast<int32_t>(data[offset + 2]) << 16) | (static_cast<int32_t>(data[offset + 3]) << 24);
        }

        uint32_t ReadUInt32(std::span<const uint8_t> data, size_t offset)
        {
            if (offset + 4 > data.size()) return 0;
            return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1]) << 8) |
                   (static_cast<uint32_t>(data[offset + 2]) << 16) | (static_cast<uint32_t>(data[offset + 3]) << 24);
        }

        int64_t ReadInt64(std::span<const uint8_t> data, size_t offset)
        {
            if (offset + 8 > data.size()) return 0;
            uint64_t low = ReadUInt32(data, offset);
            uint64_t high = ReadUInt32(data, offset + 4);
            return static_cast<int64_t>(low | (high << 32));
        }

        void AppendInt32(std::string& out, int32_t value)
        {
            char buf[32] = {};
            const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
            if (ec == std::errc{})
            {
                out.append(buf, static_cast<size_t>(ptr - buf));
            }
        }

        void AppendInt64(std::string& out, int64_t value)
        {
            char buf[32] = {};
            const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value);
            if (ec == std::errc{})
            {
                out.append(buf, static_cast<size_t>(ptr - buf));
            }
        }

        void AppendFloat(std::string& out, float value)
        {
            char buf[48] = {};
            const auto [ptr, ec] = std::to_chars(buf, buf + sizeof(buf), value, std::chars_format::general, 6);
            if (ec == std::errc{})
            {
                out.append(buf, static_cast<size_t>(ptr - buf));
            }
        }

        struct BinBlockMeta
        {
            std::string_view name;
            uint32_t paramsCount = 0;
            uint32_t childCount = 0;
            uint32_t firstChild = 0;
            size_t paramsOffset = 0;
        };

        void BuildBlkBlockTree(
            BlkBlock& outBlock,
            const std::vector<BinBlockMeta>& allBlocks,
            size_t currentIdx,
            const std::vector<std::string_view>& names,
            std::span<const uint8_t> paramsData,
            std::span<const uint8_t> paramsTable)
        {
            const BinBlockMeta& meta = allBlocks[currentIdx];
            outBlock.name.assign(meta.name.data(), meta.name.size());
            outBlock.line = static_cast<uint32_t>(currentIdx + 1);
            outBlock.properties.reserve(meta.paramsCount);

            for (uint32_t i = 0; i < meta.paramsCount; ++i)
            {
                const size_t pOff = meta.paramsOffset + static_cast<size_t>(i) * 8;
                if (pOff + 8 > paramsTable.size()) continue;

                const uint32_t flags = ReadUInt32(paramsTable, pOff);
                const uint32_t rawValue = ReadUInt32(paramsTable, pOff + 4);

                const uint32_t encodedNameId = flags & 0x00FFFFFFu;
                const uint8_t valueType = static_cast<uint8_t>(flags >> 24);

                BlkProperty prop;
                const std::string_view resolvedName = ResolveBlkName(names, encodedNameId, false);
                prop.name.assign(resolvedName.data(), resolvedName.size());
                prop.line = outBlock.line;

                switch (valueType)
                {
                case 0x01: // String
                    prop.type = "t";
                    prop.quoted = true;
                    if (rawValue < paramsData.size())
                    {
                        size_t len = 0;
                        while (rawValue + len < paramsData.size() && paramsData[rawValue + len] != 0) len++;
                        prop.value.assign(reinterpret_cast<const char*>(paramsData.data() + rawValue), len);
                    }
                    break;
                case 0x02: // Int
                    prop.type = "i";
                    AppendInt32(prop.value, static_cast<int32_t>(rawValue));
                    break;
                case 0x03: // Float
                {
                    prop.type = "r";
                    float f = 0.0f;
                    std::memcpy(&f, &rawValue, sizeof(float));
                    AppendFloat(prop.value, f);
                    break;
                }
                case 0x04: // Float2
                    prop.type = "p2";
                    AppendFloat(prop.value, ReadFloat(paramsData, rawValue));
                    prop.value.append(", ");
                    AppendFloat(prop.value, ReadFloat(paramsData, rawValue + 4));
                    break;
                case 0x05: // Float3
                    prop.type = "p3";
                    AppendFloat(prop.value, ReadFloat(paramsData, rawValue));
                    prop.value.append(", ");
                    AppendFloat(prop.value, ReadFloat(paramsData, rawValue + 4));
                    prop.value.append(", ");
                    AppendFloat(prop.value, ReadFloat(paramsData, rawValue + 8));
                    break;
                case 0x06: // Float4
                    prop.type = "p4";
                    AppendFloat(prop.value, ReadFloat(paramsData, rawValue));
                    prop.value.append(", ");
                    AppendFloat(prop.value, ReadFloat(paramsData, rawValue + 4));
                    prop.value.append(", ");
                    AppendFloat(prop.value, ReadFloat(paramsData, rawValue + 8));
                    prop.value.append(", ");
                    AppendFloat(prop.value, ReadFloat(paramsData, rawValue + 12));
                    break;
                case 0x07: // Int2
                    prop.type = "ip2";
                    AppendInt32(prop.value, ReadInt32(paramsData, rawValue));
                    prop.value.append(", ");
                    AppendInt32(prop.value, ReadInt32(paramsData, rawValue + 4));
                    break;
                case 0x08: // Int3
                    prop.type = "ip3";
                    AppendInt32(prop.value, ReadInt32(paramsData, rawValue));
                    prop.value.append(", ");
                    AppendInt32(prop.value, ReadInt32(paramsData, rawValue + 4));
                    prop.value.append(", ");
                    AppendInt32(prop.value, ReadInt32(paramsData, rawValue + 8));
                    break;
                case 0x09: // Bool
                    prop.type = "b";
                    prop.value = rawValue ? "yes" : "no";
                    break;
                case 0x0A: // Color
                {
                    prop.type = "c";
                    uint8_t a = static_cast<uint8_t>(rawValue >> 24);
                    uint8_t r = static_cast<uint8_t>((rawValue >> 16) & 0xFF);
                    uint8_t g = static_cast<uint8_t>((rawValue >> 8) & 0xFF);
                    uint8_t b = static_cast<uint8_t>(rawValue & 0xFF);
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", a, r, g, b);
                    prop.value = buf;
                    break;
                }
                case 0x0B: // Float12
                    prop.type = "m";
                    prop.value.push_back('[');
                    for (int v = 0; v < 4; ++v)
                    {
                        prop.value.push_back('[');
                        AppendFloat(prop.value, ReadFloat(paramsData, rawValue + v * 12));
                        prop.value.append(", ");
                        AppendFloat(prop.value, ReadFloat(paramsData, rawValue + v * 12 + 4));
                        prop.value.append(", ");
                        AppendFloat(prop.value, ReadFloat(paramsData, rawValue + v * 12 + 8));
                        prop.value.push_back(']');
                        if (v < 3) prop.value.push_back(' ');
                    }
                    prop.value.push_back(']');
                    break;
                case 0x0C: // Long
                    prop.type = "i64";
                    AppendInt64(prop.value, ReadInt64(paramsData, rawValue));
                    break;
                default:
                    prop.type = "unknown";
                    prop.value = "0";
                    break;
                }

                outBlock.properties.push_back(std::move(prop));
            }

            outBlock.children.reserve(meta.childCount);
            for (uint32_t c = 0; c < meta.childCount; ++c)
            {
                BlkBlock childBlock;
                BuildBlkBlockTree(
                    childBlock,
                    allBlocks,
                    static_cast<size_t>(meta.firstChild) + c,
                    names,
                    paramsData,
                    paramsTable);
                outBlock.children.push_back(std::move(childBlock));
            }
        }
    }

    BlkReader::BlkReader(std::string_view text)
        : m_text(text)
    {
        m_root.name = "<root>";
    }

    void BlkReader::Parse()
    {
        m_position = 0;
        m_line = 1;
        m_root = {};
        m_root.name = "<root>";
        m_root.line = 1;

        if (IsBinaryBlk(m_text))
        {
            ParseBinary();
        }
        else
        {
            ParseBlockContents(m_root, false);
        }
    }

    void BlkReader::ParseBinary()
    {
        std::span<const uint8_t> data(reinterpret_cast<const uint8_t*>(m_text.data()), m_text.size());
        std::vector<uint8_t> decompressedData;

        if (data.empty())
        {
            return; // Empty file
        }

        // Handle Zstd envelope if present
        if (data.size() >= 4)
        {
            uint32_t magic = ReadUInt32(data, 0);
            if (magic == 0xFD2FB528)
            {
                decompressedData = DagorIO::Decompressor::DecompressZstd(data);
                data = decompressedData;
            }
        }
        else if (data[0] == 0x02) // Alternate Zstd envelope?
        {
            // GameResDesc uses 0x02 + 24-bit size, but standard BLK does not usually.
        }

        if (data.empty()) return;

        DagorIO::MemoryReader reader(data);
        const uint8_t magic = reader.ReadUInt8();
        if (magic != 0x00 && magic != 0x01)
        {
            throw std::runtime_error("Unsupported binary BLK magic: " + std::to_string(magic));
        }

        const uint32_t namesCount = ReadVlq(reader);
        const uint32_t namesSize = ReadVlq(reader);
        if (reader.GetRemaining() < namesSize)
        {
            throw std::runtime_error("BLK name map extends past end of payload");
        }

        const std::span<const uint8_t> nameBytes = reader.ReadSpan(namesSize);
        const std::vector<std::string_view> names = ParseBlkNameMap(nameBytes, namesCount);

        const uint32_t blocksCount = ReadVlq(reader);
        const uint32_t paramsCount = ReadVlq(reader);
        const uint32_t paramsDataSize = ReadVlq(reader);

        if (blocksCount == 0)
        {
            return;
        }

        if (reader.GetRemaining() < paramsDataSize)
        {
            throw std::runtime_error("BLK parameter data extends past end of payload");
        }
        std::span<const uint8_t> paramsData = reader.ReadSpan(paramsDataSize);

        const size_t paramsBytes = static_cast<size_t>(paramsCount) * 8;
        if (reader.GetRemaining() < paramsBytes)
        {
            throw std::runtime_error("BLK parameter table extends past end of payload");
        }
        std::span<const uint8_t> paramsTable = reader.ReadSpan(paramsBytes);

        std::vector<BinBlockMeta> blocks;
        blocks.reserve(blocksCount);
        size_t nextParamsOffset = 0;

        for (uint32_t i = 0; i < blocksCount; ++i)
        {
            const uint32_t encodedNameId = ReadVlq(reader);
            BinBlockMeta block;
            block.name = ResolveBlkName(names, encodedNameId, true);
            block.paramsCount = ReadVlq(reader);
            block.childCount = ReadVlq(reader);
            if (block.childCount != 0)
            {
                block.firstChild = ReadVlq(reader);
            }
            block.paramsOffset = nextParamsOffset;

            const size_t blockParamsBytes = static_cast<size_t>(block.paramsCount) * 8;
            if (nextParamsOffset + blockParamsBytes > paramsTable.size())
            {
                throw std::runtime_error("BLK block parameter range is invalid");
            }
            nextParamsOffset += blockParamsBytes;

            blocks.push_back(block);
        }

        for (const BinBlockMeta& block : blocks)
        {
            if (block.childCount != 0 &&
                static_cast<size_t>(block.firstChild) + block.childCount > blocks.size())
            {
                throw std::runtime_error("BLK child block range is invalid");
            }
        }

        if (!blocks.empty())
        {
            // Root block is always blocks[0]
            BuildBlkBlockTree(m_root, blocks, 0, names, paramsData, paramsTable);
        }
    }

    void BlkReader::ParseBlockContents(BlkBlock& block, bool stopAtClosingBrace)
    {
        while (true)
        {
            SkipIgnored();
            if (IsAtEnd())
            {
                if (stopAtClosingBrace)
                {
                    Fail("Unexpected end of BLK block");
                }
                return;
            }

            if (Peek() == '}')
            {
                if (!stopAtClosingBrace)
                {
                    Fail("Unexpected closing brace");
                }
                Advance();
                return;
            }

            const uint32_t line = m_line;
            std::string name = ParseIdentifier();
            if (name.empty())
            {
                Fail("Expected BLK identifier");
            }

            SkipIgnored();

            std::string type;
            if (Match(':'))
            {
                SkipIgnored();
                type = ParseTypeName();
                if (type.empty())
                {
                    Fail("Expected BLK type after ':'");
                }
                SkipIgnored();
            }

            if (Match('{'))
            {
                block.children.push_back(ParseChildBlock(std::move(name), std::move(type), line));
                Match(';');
                continue;
            }

            if (Match('='))
            {
                block.properties.push_back(ParseProperty(std::move(name), std::move(type), line));
                Match(';');
                continue;
            }

            Fail("Expected '=' or '{' after BLK identifier");
        }
    }

    BlkProperty BlkReader::ParseProperty(std::string name, std::string type, uint32_t line)
    {
        SkipIgnored();

        BlkProperty property;
        property.name = std::move(name);
        property.type = std::move(type);
        property.line = line;
        property.value = ParseValue(property.quoted);
        return property;
    }

    BlkBlock BlkReader::ParseChildBlock(std::string name, std::string type, uint32_t line)
    {
        BlkBlock block;
        block.name = std::move(name);
        block.type = std::move(type);
        block.line = line;
        ParseBlockContents(block, true);
        return block;
    }

    void BlkReader::SkipIgnored()
    {
        while (!IsAtEnd())
        {
            const char c = Peek();
            if (std::isspace(static_cast<unsigned char>(c)))
            {
                Advance();
                continue;
            }

            if (c == '#')
            {
                while (!IsAtEnd() && Peek() != '\n')
                {
                    Advance();
                }
                continue;
            }

            if (c == '/' && m_position + 1 < m_text.size())
            {
                const char next = m_text[m_position + 1];
                if (next == '/')
                {
                    while (!IsAtEnd() && Peek() != '\n')
                    {
                        Advance();
                    }
                    continue;
                }

                if (next == '*')
                {
                    Advance();
                    Advance();
                    while (!IsAtEnd())
                    {
                        if (Peek() == '*' && m_position + 1 < m_text.size() && m_text[m_position + 1] == '/')
                        {
                            Advance();
                            Advance();
                            break;
                        }
                        Advance();
                    }
                    continue;
                }
            }

            return;
        }
    }

    bool BlkReader::Match(char expected)
    {
        if (IsAtEnd() || Peek() != expected)
        {
            return false;
        }

        Advance();
        return true;
    }

    bool BlkReader::IsAtEnd() const
    {
        return m_position >= m_text.size();
    }

    char BlkReader::Peek() const
    {
        return IsAtEnd() ? '\0' : m_text[m_position];
    }

    char BlkReader::Advance()
    {
        if (IsAtEnd())
        {
            return '\0';
        }

        const char c = m_text[m_position++];
        if (c == '\n')
        {
            ++m_line;
        }
        return c;
    }

    std::string BlkReader::ParseIdentifier()
    {
        const size_t start = m_position;
        while (!IsAtEnd() && !IsDelimiter(Peek()))
        {
            Advance();
        }
        return std::string(m_text.substr(start, m_position - start));
    }

    std::string BlkReader::ParseTypeName()
    {
        const size_t start = m_position;
        while (!IsAtEnd())
        {
            const char c = Peek();
            if (std::isspace(static_cast<unsigned char>(c)) || c == '=' || c == '{' || c == '}' || c == ';')
            {
                break;
            }
            Advance();
        }
        return std::string(m_text.substr(start, m_position - start));
    }

    std::string BlkReader::ParseValue(bool& quoted)
    {
        quoted = !IsAtEnd() && Peek() == '"';
        return quoted ? ParseQuotedValue() : ParseRawValue();
    }

    std::string BlkReader::ParseQuotedValue()
    {
        if (!Match('"'))
        {
            Fail("Expected quoted BLK value");
        }

        std::string value;
        while (!IsAtEnd())
        {
            const char c = Advance();
            if (c == '"')
            {
                return value;
            }

            if (c == '\\' && !IsAtEnd())
            {
                const char escaped = Advance();
                switch (escaped)
                {
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                case '\\':
                case '"':
                    value.push_back(escaped);
                    break;
                default:
                    value.push_back(escaped);
                    break;
                }
                continue;
            }

            value.push_back(c);
        }

        Fail("Unterminated quoted BLK value");
    }

    std::string BlkReader::ParseRawValue()
    {
        const size_t start = m_position;
        while (!IsAtEnd())
        {
            const char c = Peek();
            if (c == '\n' || c == '\r' || c == ';' || c == '}')
            {
                break;
            }
            if (c == '#')
            {
                break;
            }
            if (c == '/' && m_position + 1 < m_text.size())
            {
                const char next = m_text[m_position + 1];
                if (next == '/' || next == '*')
                {
                    break;
                }
            }
            Advance();
        }

        std::string value(m_text.substr(start, m_position - start));
        TrimInPlace(value);
        return value;
    }

    void BlkReader::Fail(const char* message) const
    {
        throw std::runtime_error(
            std::string(message) + " at line " + std::to_string(m_line) + ".");
    }

    bool IsBinaryBlk(std::string_view text)
    {
        if (text.size() < 4)
        {
            return false;
        }

        // Check for null bytes in the first 4 bytes
        for (size_t i = 0; i < 4 && i < text.size(); ++i)
        {
            if (text[i] == '\0')
            {
                return true;
            }
        }

        // Check for Zstd magic number (0xFD2FB528) at the start or in the first few bytes
        if (text.size() >= 8)
        {
            const unsigned char* bytes = reinterpret_cast<const unsigned char*>(text.data());
            auto matchesZstd = [](const unsigned char* p) {
                return p[0] == 0x28 && p[1] == 0xB5 && p[2] == 0x2F && p[3] == 0xFD;
            };

            if (matchesZstd(bytes) || matchesZstd(bytes + 4))
            {
                return true;
            }
        }

        // Also check if the text contains non-printable characters in the first 32 bytes
        size_t printableCount = 0;
        size_t checkLen = std::min<size_t>(32, text.size());
        for (size_t i = 0; i < checkLen; ++i)
        {
            unsigned char c = text[i];
            if ((c >= 32 && c <= 126) || c == '\r' || c == '\n' || c == '\t')
            {
                printableCount++;
            }
        }
        if (printableCount < checkLen * 0.7) // if less than 70% is printable text
        {
            return true;
        }

        return false;
    }
}
