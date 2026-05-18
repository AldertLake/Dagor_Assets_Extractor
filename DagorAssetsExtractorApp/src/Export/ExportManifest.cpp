#include "Export/ExportManifest.h"
#include <fstream>

namespace DagorAssetsExtractor::Export
{
    // Basic JSON string escaping
    std::string EscapeJsonString(const std::string& input)
    {
        std::string output;
        output.reserve(input.length() + 2);
        for (char c : input)
        {
            switch (c)
            {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if ('\x00' <= c && c <= '\x1f')
                {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    output += buf;
                }
                else
                {
                    output += c;
                }
            }
        }
        return output;
    }

    bool ExportManifest::WriteToFile(const std::filesystem::path& path) const
    {
        std::ofstream out(path);
        if (!out)
        {
            return false;
        }

        out << "{\n";
        out << "  \"sourcePack\": \"" << EscapeJsonString(sourcePack) << "\",\n";
        out << "  \"sourceResource\": \"" << EscapeJsonString(sourceResource) << "\",\n";
        out << "  \"assetType\": \"" << EscapeJsonString(assetType) << "\",\n";
        out << "  \"appVersion\": \"" << EscapeJsonString(appVersion) << "\",\n";
        out << "  \"formatDecoderVersion\": \"" << EscapeJsonString(formatDecoderVersion) << "\",\n";
        
        auto writeArray = [&out](const std::string& name, const std::vector<std::string>& arr) {
            out << "  \"" << name << "\": [\n";
            for (size_t i = 0; i < arr.size(); ++i)
            {
                out << "    \"" << EscapeJsonString(arr[i]) << "\"";
                if (i + 1 < arr.size()) out << ",";
                out << "\n";
            }
            out << "  ]";
        };

        writeArray("outputFiles", outputFiles); out << ",\n";
        writeArray("textureDependencies", textureDependencies); out << ",\n";
        writeArray("skeletonDependencies", skeletonDependencies); out << ",\n";
        writeArray("decodeWarnings", decodeWarnings); out << ",\n";
        writeArray("unsupportedFields", unsupportedFields); out << "\n";

        out << "}\n";

        return true;
    }
}
