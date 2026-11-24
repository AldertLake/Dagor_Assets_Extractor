#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace DagorFormats
{
    struct BlkProperty
    {
        std::string name;
        std::string type;
        std::string value;
        uint32_t line = 1;
        bool quoted = false;
    };

    struct BlkBlock
    {
        std::string name;
        std::string type;
        uint32_t line = 1;
        std::vector<BlkProperty> properties;
        std::vector<BlkBlock> children;
    };

    bool IsBinaryBlk(std::string_view text);

    class BlkReader
    {
    public:
        explicit BlkReader(std::string_view text);

        void Parse();

        const BlkBlock& GetRoot() const { return m_root; }

    private:
        void ParseBlockContents(BlkBlock& block, bool stopAtClosingBrace);
        void ParseBinary();
        BlkProperty ParseProperty(std::string name, std::string type, uint32_t line);
        BlkBlock ParseChildBlock(std::string name, std::string type, uint32_t line);

        void SkipIgnored();
        bool Match(char expected);
        bool IsAtEnd() const;
        char Peek() const;
        char Advance();

        std::string ParseIdentifier();
        std::string ParseTypeName();
        std::string ParseValue(bool& quoted);
        std::string ParseQuotedValue();
        std::string ParseRawValue();

        [[noreturn]] void Fail(const char* message) const;

        std::string_view m_text;
        size_t m_position = 0;
        uint32_t m_line = 1;
        BlkBlock m_root;
    };
}
