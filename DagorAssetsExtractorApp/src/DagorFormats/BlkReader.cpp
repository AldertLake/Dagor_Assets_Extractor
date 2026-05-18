#include "BlkReader.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

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

        ParseBlockContents(m_root, false);
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
}
