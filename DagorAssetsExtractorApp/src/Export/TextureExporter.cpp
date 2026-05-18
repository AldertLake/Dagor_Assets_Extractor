#include "Export/TextureExporter.h"
#include "DagorIO/BinaryReader.h"
#include "Platform/Platform.h"
#include "Export/ExportManifest.h"
#include "DagorIO/Decompression.h"

#include <fstream>
#include <iostream>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <string_view>

namespace DagorAssetsExtractor::Export
{
    // DDS format constants
    constexpr uint32_t DDS_MAGIC = 0x20534444; // "DDS "
    constexpr uint32_t DDPF_FOURCC = 0x00000004;
    constexpr uint32_t DDPF_ALPHAPIXELS = 0x00000001;
    constexpr uint32_t DDPF_RGB = 0x00000040;

    constexpr uint32_t DDSD_CAPS = 0x00000001;
    constexpr uint32_t DDSD_HEIGHT = 0x00000002;
    constexpr uint32_t DDSD_WIDTH = 0x00000004;
    constexpr uint32_t DDSD_PITCH = 0x00000008;
    constexpr uint32_t DDSD_PIXELFORMAT = 0x00001000;
    constexpr uint32_t DDSD_MIPMAPCOUNT = 0x00020000;
    constexpr uint32_t DDSD_LINEARSIZE = 0x00080000;

    constexpr uint32_t DDSCAPS_COMPLEX = 0x00000008;
    constexpr uint32_t DDSCAPS_TEXTURE = 0x00001000;
    constexpr uint32_t DDSCAPS_MIPMAP = 0x00400000;

    // DX10 magic FourCC: "DX10"
    constexpr uint32_t DX10_FOURCC = 0x30315844; // 'D','X','1','0'

    struct DdsPixelFormat {
        uint32_t size = 32;
        uint32_t flags = 0;
        uint32_t fourCC = 0;
        uint32_t rgbBitCount = 0;
        uint32_t rBitMask = 0;
        uint32_t gBitMask = 0;
        uint32_t bBitMask = 0;
        uint32_t aBitMask = 0;
    };

    struct DdsHeader {
        uint32_t size = 124;
        uint32_t flags = 0;
        uint32_t height = 0;
        uint32_t width = 0;
        uint32_t pitchOrLinearSize = 0;
        uint32_t depth = 0;
        uint32_t mipMapCount = 0;
        uint32_t reserved1[11] = {};
        DdsPixelFormat ddspf;
        uint32_t caps = 0;
        uint32_t caps2 = 0;
        uint32_t caps3 = 0;
        uint32_t caps4 = 0;
        uint32_t reserved2 = 0;
    };

    namespace
    {
        bool IsPathSeparator(char ch)
        {
            return ch == '/' || ch == '\\';
        }

        bool IsInvalidWindowsFileNameChar(unsigned char ch)
        {
            return ch < 32 || ch == '<' || ch == '>' || ch == ':' || ch == '"' ||
                   ch == '|' || ch == '?' || ch == '*';
        }

        std::string ToUpperAscii(std::string_view value)
        {
            std::string upper;
            upper.reserve(value.size());
            for (char ch : value)
            {
                if (ch >= 'a' && ch <= 'z')
                {
                    upper.push_back(static_cast<char>(ch - ('a' - 'A')));
                }
                else
                {
                    upper.push_back(ch);
                }
            }
            return upper;
        }

        bool IsReservedWindowsName(std::string_view segment)
        {
            std::string trimmed(segment);
            while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '.'))
            {
                trimmed.pop_back();
            }

            const std::string upper = ToUpperAscii(trimmed);
            if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL")
            {
                return true;
            }

            if (upper.size() == 4 &&
                (upper.rfind("COM", 0) == 0 || upper.rfind("LPT", 0) == 0) &&
                upper[3] >= '1' && upper[3] <= '9')
            {
                return true;
            }

            return false;
        }

        std::string SanitizePathSegment(std::string_view segment)
        {
            std::string safe;
            safe.reserve(segment.size());
            for (unsigned char ch : segment)
            {
                safe.push_back(IsInvalidWindowsFileNameChar(ch) ? '_' : static_cast<char>(ch));
            }

            while (!safe.empty() && (safe.back() == ' ' || safe.back() == '.'))
            {
                safe.back() = '_';
            }

            if (safe.empty())
            {
                safe = "_";
            }

            if (safe == "." || safe == ".." || IsReservedWindowsName(safe))
            {
                safe += "_";
            }

            return safe;
        }

        std::filesystem::path BuildSafeOutputPath(
            const std::filesystem::path& outputDir,
            const std::string& assetName,
            std::string_view extension)
        {
            std::filesystem::path relative;
            std::string segment;

            auto flushSegment = [&]() {
                if (segment.empty() || segment == ".")
                {
                    segment.clear();
                    return;
                }

                relative /= SanitizePathSegment(segment);
                segment.clear();
            };

            for (char ch : assetName)
            {
                if (IsPathSeparator(ch))
                {
                    flushSegment();
                }
                else
                {
                    segment.push_back(ch);
                }
            }
            flushSegment();

            if (relative.empty())
            {
                relative = "texture";
            }

            std::filesystem::path outputPath = outputDir / relative;
            outputPath += extension;
            return outputPath;
        }

        bool GetBlockCompressionBytes(uint32_t d3dFormat, uint32_t& blockBytes)
        {
            switch (d3dFormat)
            {
            case 827611204: // DXT1
            case 826889281: // ATI1
            case 71: case 72: // BC1
            case 80: case 81: // BC4
                blockBytes = 8;
                return true;

            case 861165636: // DXT3
            case 894720068: // DXT5
            case 843666497: // ATI2
            case 74: case 75: // BC2
            case 77: case 78: // BC3
            case 83: case 84: // BC5
            case 95: case 96: // BC6H
            case 98: case 99: // BC7
                blockBytes = 16;
                return true;

            default:
                return false;
            }
        }

        bool GetBytesPerPixel(uint32_t d3dFormat, uint32_t& bytesPerPixel)
        {
            switch (d3dFormat)
            {
            case 28: case 29: // R8G8B8A8
            case 87: case 91: // B8G8R8A8
                bytesPerPixel = 4;
                return true;

            case 61: // R8
                bytesPerPixel = 1;
                return true;

            case 49: // R8G8
                bytesPerPixel = 2;
                return true;

            case 10: // R16G16B16A16_FLOAT
                bytesPerPixel = 8;
                return true;

            case 2: // R32G32B32A32_FLOAT
                bytesPerPixel = 16;
                return true;

            default:
                return false;
            }
        }

        bool CalculateDdsMipSizes(
            const DagorFormats::DdsxHeader& header,
            std::vector<size_t>& mipSizes)
        {
            if (header.width == 0 || header.height == 0)
            {
                return false;
            }

            uint32_t blockBytes = 0;
            uint32_t bytesPerPixel = 0;
            const bool blockCompressed = GetBlockCompressionBytes(header.d3dFormat, blockBytes);
            const bool uncompressed = !blockCompressed && GetBytesPerPixel(header.d3dFormat, bytesPerPixel);

            if (!blockCompressed && !uncompressed)
            {
                return false;
            }

            mipSizes.clear();
            uint32_t width = std::max<uint32_t>(1, header.width);
            uint32_t height = std::max<uint32_t>(1, header.height);
            const uint32_t mipCount = std::max<uint32_t>(1, header.mipCount);

            for (uint32_t mip = 0; mip < mipCount; ++mip)
            {
                uint64_t mipBytes = 0;
                if (blockCompressed)
                {
                    const uint64_t blocksWide = std::max<uint64_t>(1, (static_cast<uint64_t>(width) + 3) / 4);
                    const uint64_t blocksHigh = std::max<uint64_t>(1, (static_cast<uint64_t>(height) + 3) / 4);
                    mipBytes = blocksWide * blocksHigh * blockBytes;
                }
                else
                {
                    mipBytes = static_cast<uint64_t>(width) * height * bytesPerPixel;
                }

                if (mipBytes > std::numeric_limits<size_t>::max())
                {
                    return false;
                }

                mipSizes.push_back(static_cast<size_t>(mipBytes));
                width = std::max<uint32_t>(1, width / 2);
                height = std::max<uint32_t>(1, height / 2);
            }

            return true;
        }

        bool CalculateDdsPayloadSize(
            const DagorFormats::DdsxHeader& header,
            size_t& totalBytes,
            size_t* topMipBytes = nullptr)
        {
            std::vector<size_t> mipSizes;
            if (!CalculateDdsMipSizes(header, mipSizes))
            {
                return false;
            }

            uint64_t total = 0;
            for (size_t i = 0; i < mipSizes.size(); ++i)
            {
                if (i == 0 && topMipBytes)
                {
                    *topMipBytes = mipSizes[i];
                }

                if (total > std::numeric_limits<size_t>::max() - mipSizes[i])
                {
                    return false;
                }

                total += mipSizes[i];
            }

            totalBytes = static_cast<size_t>(total);
            return true;
        }

        bool NormalizeMipOrderIfNeeded(
            const DagorFormats::DdsxHeader& header,
            std::vector<uint8_t>& payload,
            std::string& warning,
            std::string& error)
        {
            constexpr uint32_t FLG_REV_MIP_ORDER = 0x00040000;
            if ((header.flags & FLG_REV_MIP_ORDER) == 0 || header.mipCount <= 1)
            {
                return true;
            }

            std::vector<size_t> mipSizes;
            if (!CalculateDdsMipSizes(header, mipSizes))
            {
                warning = "DDSx reverse-mip-order flag is set, but this format's mip sizes are not known.";
                return true;
            }

            size_t expectedSize = 0;
            for (const size_t mipSize : mipSizes)
            {
                if (expectedSize > std::numeric_limits<size_t>::max() - mipSize)
                {
                    error = "DDS mip payload size overflow while normalizing reverse mip order.";
                    return false;
                }
                expectedSize += mipSize;
            }

            if (payload.size() < expectedSize)
            {
                return true;
            }

            std::vector<uint8_t> reordered;
            reordered.reserve(payload.size());
            std::vector<std::vector<uint8_t>> mips(mipSizes.size());

            size_t sourceOffset = 0;
            for (size_t sourceIndex = 0; sourceIndex < mipSizes.size(); ++sourceIndex)
            {
                const size_t targetIndex = mipSizes.size() - 1 - sourceIndex;
                const size_t mipSize = mipSizes[targetIndex];
                mips[targetIndex].assign(
                    payload.begin() + static_cast<std::ptrdiff_t>(sourceOffset),
                    payload.begin() + static_cast<std::ptrdiff_t>(sourceOffset + mipSize));
                sourceOffset += mipSize;
            }

            for (const std::vector<uint8_t>& mip : mips)
            {
                reordered.insert(reordered.end(), mip.begin(), mip.end());
            }

            if (payload.size() > expectedSize)
            {
                reordered.insert(reordered.end(), payload.begin() + static_cast<std::ptrdiff_t>(expectedSize), payload.end());
                warning = "Reordered reverse DDSx mip payload and preserved trailing bytes.";
            }
            else
            {
                warning = "Reordered reverse DDSx mip payload to standard DDS mip order.";
            }

            payload = std::move(reordered);
            return true;
        }

        std::string HexValue(uint64_t value, int width)
        {
            std::ostringstream out;
            out << "0x" << std::uppercase << std::hex << std::setw(width) << std::setfill('0') << value;
            return out.str();
        }

        std::string FormatFourCC(uint32_t value)
        {
            char chars[5] = {
                static_cast<char>(value & 0xFF),
                static_cast<char>((value >> 8) & 0xFF),
                static_cast<char>((value >> 16) & 0xFF),
                static_cast<char>((value >> 24) & 0xFF),
                '\0'
            };

            for (int i = 0; i < 4; ++i)
            {
                const unsigned char ch = static_cast<unsigned char>(chars[i]);
                if (ch < 32 || ch > 126)
                {
                    return {};
                }
            }

            return std::string(chars, 4);
        }

        std::string BuildDdsxDiagnostic(const DagorFormats::DdsxHeader& header, size_t dxpPayloadSize)
        {
            std::ostringstream out;
            out << "DDSx header: format=" << header.FormatName();

            const std::string fourCC = FormatFourCC(header.d3dFormat);
            if (!fourCC.empty())
            {
                out << " (" << fourCC << ")";
            }

            out << ", d3dFormat=" << header.d3dFormat
                << ", flags=" << HexValue(header.flags, 8)
                << ", compression=" << header.CompressionName()
                << " (flag=" << HexValue(header.compressionFlag, 2) << ")"
                << ", dimensions=" << header.width << "x" << header.height
                << ", mips=" << static_cast<uint32_t>(header.mipCount)
                << ", depth=" << header.depth
                << ", bpp=" << header.bitsPerPixel
                << ", memSz=" << header.memorySize
                << ", packedSz=" << header.packedSize
                << ", dxpPayloadSize=" << dxpPayloadSize;
            return out.str();
        }

        bool WriteRawDdsx(
            const std::filesystem::path& outputPath,
            std::span<const uint8_t> header,
            std::span<const uint8_t> payload,
            std::string& error)
        {
            std::ofstream out(outputPath, std::ios::binary);
            if (!out)
            {
                error = "Could not open raw DDSx output file for writing.";
                return false;
            }

            out.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
            out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
            if (!out)
            {
                error = "Failed while writing raw DDSx output.";
                return false;
            }

            return true;
        }

        bool ExportRawDdsxFallback(
            TextureExportResult& result,
            const std::filesystem::path& packPath,
            const AssetIndex::SubAssetRecord& entry,
            const TextureExportOptions& options,
            const DagorFormats::DdsxHeader& header,
            const std::vector<uint8_t>& packPayload,
            const std::string& reason)
        {
            const std::filesystem::path outputPath = BuildSafeOutputPath(options.outputDir, entry.name, ".ddsx");
            std::error_code ec;
            std::filesystem::create_directories(outputPath.parent_path(), ec);
            if (ec)
            {
                result.error = "Could not create raw DDSx output directory: " + ec.message();
                return false;
            }

            if (!options.overwrite && std::filesystem::exists(outputPath))
            {
                result.error = "Raw DDSx output file already exists: " + Platform::PathToUtf8(outputPath);
                return false;
            }

            std::string writeError;
            if (!WriteRawDdsx(outputPath, entry.ddsxHeader, packPayload, writeError))
            {
                result.error = writeError;
                return false;
            }

            result.success = true;
            result.outputPath = outputPath;
            result.warnings.push_back(reason);
            result.warnings.push_back("Exported raw DDSx fallback because decoded DDS output is not available.");

            ExportManifest manifest;
            manifest.sourcePack = Platform::PathToUtf8(packPath.filename());
            manifest.sourceResource = entry.name;
            manifest.assetType = "Texture (DDSx raw fallback)";
            manifest.outputFiles.push_back(Platform::PathToUtf8(outputPath.filename()));
            manifest.appVersion = "0.1.0-alpha";
            manifest.formatDecoderVersion = "DdsxReader v1.0";
            manifest.decodeWarnings = result.warnings;
            manifest.unsupportedFields.push_back(
                "Decoded DDS export requires " + std::string(header.CompressionName()) + " decompression.");

            std::filesystem::path manifestPath = outputPath;
            manifestPath.replace_extension(".export-manifest.json");
            if (!manifest.WriteToFile(manifestPath))
            {
                result.warnings.push_back("Could not write raw DDSx export manifest.");
            }

            return true;
        }
    }

    bool TextureExporter::NeedsDX10Header(uint32_t d3dFormat)
    {
        // Formats that have no legacy FourCC mapping need a DX10 extended header.
        // BC6H (95, 96), BC7 (98, 99), and various uncompressed DXGI formats
        // that don't fit the legacy DDS pixel format.
        switch (d3dFormat)
        {
        // Legacy D3DFORMAT FourCC values - no DX10 needed
        case 827611204: return false; // DXT1
        case 861165636: return false; // DXT3
        case 894720068: return false; // DXT5
        case 826889281: return false; // ATI1
        case 843666497: return false; // ATI2

        // DXGI BC1-BC5 can be mapped to legacy FourCC - no DX10 needed
        case 71: case 72: return false; // BC1
        case 74: case 75: return false; // BC2
        case 77: case 78: return false; // BC3
        case 80: case 81: return false; // BC4
        case 83: case 84: return false; // BC5

        // RGBA8 can be mapped to legacy pixel format - no DX10 needed
        case 28: case 29: return false; // R8G8B8A8_UNORM / SRGB

        // Everything else needs DX10
        default: return true;
        }
    }

    uint32_t TextureExporter::GetDxgiFormat(uint32_t d3dFormat)
    {
        // Map legacy D3DFORMAT FourCC codes to their DXGI equivalents
        switch (d3dFormat)
        {
        case 827611204: return 71;  // DXT1 -> DXGI_FORMAT_BC1_UNORM
        case 861165636: return 74;  // DXT3 -> DXGI_FORMAT_BC2_UNORM
        case 894720068: return 77;  // DXT5 -> DXGI_FORMAT_BC3_UNORM
        case 826889281: return 80;  // ATI1 -> DXGI_FORMAT_BC4_UNORM
        case 843666497: return 83;  // ATI2 -> DXGI_FORMAT_BC5_UNORM
        default:                    return d3dFormat; // Already a DXGI format code
        }
    }

    TextureExportResult TextureExporter::ExportTexture(
        const std::filesystem::path& packPath,
        const AssetIndex::SubAssetRecord& entry,
        const TextureExportOptions& options)
    {
        TextureExportResult result;

        if (entry.ddsxHeader.size() < 32)
        {
            result.error = "Missing DDSx header metadata for DXP entry. Reload the DXP contents before exporting.";
            return result;
        }

        if (options.outputDir.empty())
        {
            result.error = "Export output directory is not configured.";
            return result;
        }

        std::ifstream file(packPath, std::ios::binary | std::ios::ate);
        if (!file)
        {
            result.error = "Could not open pack file: " + Platform::PathToUtf8(packPath);
            return result;
        }

        const std::streampos endPos = file.tellg();
        if (endPos < 0)
        {
            result.error = "Could not determine pack file size.";
            return result;
        }

        const uintmax_t fileSize = static_cast<uintmax_t>(endPos);
        const uintmax_t entryOffset = entry.offset;
        const uintmax_t entrySize = entry.size;

        if (entrySize == 0)
        {
            result.error = "DXP entry has no payload data.";
            return result;
        }

        if (entryOffset > fileSize || entrySize > fileSize - entryOffset)
        {
            result.error = "DXP entry payload is outside the pack file bounds.";
            return result;
        }

        try
        {
            DagorFormats::DdsxReader reader(entry.ddsxHeader);
            reader.Parse();
            const auto& header = reader.GetHeader();

            file.seekg(static_cast<std::streamoff>(entry.offset), std::ios::beg);
            std::vector<uint8_t> packPayload(entry.size);
            if (!file.read(reinterpret_cast<char*>(packPayload.data()), static_cast<std::streamsize>(packPayload.size())))
            {
                result.error = "Could not read DDSx payload data from pack.";
                return result;
            }

            std::vector<uint8_t> payload;
            if (header.IsCompressed())
            {
                if (header.memorySize == 0)
                {
                    result.error = "Compressed DDSx entry is missing decompressed payload size. " +
                        BuildDdsxDiagnostic(header, packPayload.size());
                    return result;
                }

                if (header.packedSize != 0 && header.packedSize != packPayload.size())
                {
                    result.warnings.push_back("DXP table payload size (" + std::to_string(packPayload.size()) +
                        ") differs from DDSx packed size (" + std::to_string(header.packedSize) + ").");
                }

                std::span<const uint8_t> compressedData(packPayload.data(), packPayload.size());
                std::string gameRoot = options.useOodleFromGameFolder
                    ? Platform::PathToUtf8(options.gameRoot)
                    : std::string();
                std::string oodleDllPath = Platform::PathToUtf8(options.oodleDllPath);

                try
                {
                    if (header.compressionType == 1) // zlib
                    {
                        payload = DagorIO::Decompressor::DecompressZlib(compressedData, header.memorySize);
                    }
                    else if (header.compressionType == 2) // zstd
                    {
                        payload = DagorIO::Decompressor::DecompressZstd(compressedData, header.memorySize);
                    }
                    else if (header.compressionType == 3) // Oodle
                    {
                        const std::filesystem::path oodlePath = DagorIO::Decompressor::FindOodleDll(gameRoot, oodleDllPath);
                        if (oodlePath.empty())
                        {
                            const std::string reason =
                                "Oodle decompression required but oo2core_6_win64.dll was not found. Checked: " +
                                DagorIO::Decompressor::OodleSearchDescription(gameRoot, oodleDllPath) + ". " +
                                BuildDdsxDiagnostic(header, packPayload.size());
                            ExportRawDdsxFallback(result, packPath, entry, options, header, packPayload, reason);
                            return result;
                        }
                        payload = DagorIO::Decompressor::DecompressOodle(compressedData, header.memorySize, gameRoot, oodleDllPath);
                    }
                    else
                    {
                        result.error = "Unsupported DDSx compression: " + std::string(header.CompressionName()) +
                            " (normalized type " + std::to_string(header.compressionType) + "). " +
                            BuildDdsxDiagnostic(header, packPayload.size());
                        return result;
                    }
                }
                catch (const std::exception& ex)
                {
                    if (header.compressionType == 3)
                    {
                        const std::string reason = std::string("Oodle decompression failed: ") + ex.what() + ". " +
                            BuildDdsxDiagnostic(header, packPayload.size());
                        ExportRawDdsxFallback(result, packPath, entry, options, header, packPayload, reason);
                        return result;
                    }

                    result.error = std::string("DDSx decompression failed: ") + ex.what() + ". " +
                        BuildDdsxDiagnostic(header, packPayload.size());
                    return result;
                }

                result.warnings.push_back("Decompressed from " + std::string(header.CompressionName()) +
                    " (" + std::to_string(packPayload.size()) + " -> " + std::to_string(payload.size()) + " bytes)");
            }
            else
            {
                payload = std::move(packPayload);
            }

            std::string mipOrderWarning;
            std::string mipOrderError;
            if (!NormalizeMipOrderIfNeeded(header, payload, mipOrderWarning, mipOrderError))
            {
                result.error = mipOrderError + " " + BuildDdsxDiagnostic(header, packPayload.size());
                return result;
            }
            if (!mipOrderWarning.empty())
            {
                result.warnings.push_back(mipOrderWarning);
            }

            size_t expectedPayloadSize = 0;
            if (CalculateDdsPayloadSize(header, expectedPayloadSize))
            {
                if (payload.size() < expectedPayloadSize)
                {
                    result.error = "DDS payload is too small for " + std::string(header.FormatName()) +
                        " " + std::to_string(header.width) + "x" + std::to_string(header.height) +
                        " with " + std::to_string(std::max<uint32_t>(1, header.mipCount)) +
                        " mip(s): expected at least " + std::to_string(expectedPayloadSize) +
                        " bytes, got " + std::to_string(payload.size()) + ". " +
                        BuildDdsxDiagnostic(header, packPayload.size());
                    return result;
                }

                if (payload.size() != expectedPayloadSize)
                {
                    result.warnings.push_back("DDS payload size (" + std::to_string(payload.size()) +
                        ") differs from expected mip data size (" + std::to_string(expectedPayloadSize) + ").");
                }
            }
            else
            {
                result.warnings.push_back("Could not validate DDS payload size for format code " +
                    std::to_string(header.d3dFormat) + ".");
            }

            // Prepare output path
            std::filesystem::path outputPath = BuildSafeOutputPath(options.outputDir, entry.name, ".dds");
            std::error_code ec;
            std::filesystem::create_directories(outputPath.parent_path(), ec);
            if (ec)
            {
                result.error = "Could not create output directory: " + ec.message();
                return result;
            }

            if (!options.overwrite && std::filesystem::exists(outputPath))
            {
                result.error = "Output file already exists: " + Platform::PathToUtf8(outputPath);
                return result;
            }

            if (WriteDds(outputPath, header, payload, result.error))
            {
                result.success = true;
                result.outputPath = outputPath;

                // Write Export Manifest
                ExportManifest manifest;
                manifest.sourcePack = Platform::PathToUtf8(packPath.filename());
                manifest.sourceResource = entry.name;
                manifest.assetType = "Texture (DDSx)";
                manifest.outputFiles.push_back(Platform::PathToUtf8(outputPath.filename()));
                manifest.appVersion = "0.1.0-alpha";
                manifest.formatDecoderVersion = "DdsxReader v1.0";

                if (NeedsDX10Header(header.d3dFormat))
                {
                    manifest.decodeWarnings.push_back(
                        "DX10 extended header used for DXGI format " + std::to_string(header.d3dFormat));
                }

                for (const std::string& warning : result.warnings)
                {
                    manifest.decodeWarnings.push_back(warning);
                }

                std::filesystem::path manifestPath = outputPath;
                manifestPath.replace_extension(".export-manifest.json");
                if (!manifest.WriteToFile(manifestPath))
                {
                    result.warnings.push_back("Could not write export manifest.");
                }
            }
        }
        catch (const std::exception& ex)
        {
            result.error = std::string("DDSx Parse Error: ") + ex.what();
        }

        return result;
    }

    BatchTextureExportResult TextureExporter::ExportAllTextures(
        const std::filesystem::path& packPath,
        const AssetIndex::PackContents& contents,
        const TextureExportOptions& options,
        BatchProgressCallback progress)
    {
        BatchTextureExportResult batch;
        batch.total = contents.entries.size();

        for (size_t i = 0; i < contents.entries.size(); ++i)
        {
            const AssetIndex::SubAssetRecord& entry = contents.entries[i];

            // Report progress and check for cancellation
            if (progress)
            {
                if (!progress(i + 1, batch.total, entry.name))
                {
                    batch.warnings.push_back("Batch export cancelled by user at " +
                        std::to_string(i + 1) + "/" + std::to_string(batch.total));
                    break;
                }
            }

            // Skip entries with zero size
            if (entry.size == 0)
            {
                ++batch.skipped;
                batch.warnings.push_back("Skipped empty entry: " + entry.name);
                continue;
            }

            // Check if output already exists and overwrite is disabled
            std::filesystem::path outputPath = options.outputDir / (entry.name + ".dds");
            if (!options.overwrite && std::filesystem::exists(outputPath))
            {
                ++batch.skipped;
                batch.warnings.push_back("Skipped existing file: " + entry.name);
                continue;
            }

            TextureExportResult result = ExportTexture(packPath, entry, options);

            if (result.success)
            {
                ++batch.succeeded;
            }
            else
            {
                ++batch.failed;
                batch.errors.push_back(entry.name + ": " + result.error);
            }

            for (const std::string& w : result.warnings)
            {
                batch.warnings.push_back(entry.name + ": " + w);
            }
        }

        return batch;
    }

    bool TextureExporter::WriteDds(
        const std::filesystem::path& outputPath,
        const DagorFormats::DdsxHeader& header,
        const std::vector<uint8_t>& payload,
        std::string& error)
    {
        std::ofstream out(outputPath, std::ios::binary);
        if (!out)
        {
            error = "Could not open output file for writing.";
            return false;
        }

        DdsHeader dds;
        dds.flags = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT;
        dds.width = header.width;
        dds.height = header.height;
        dds.caps = DDSCAPS_TEXTURE;

        size_t topMipSize = 0;
        size_t totalMipSize = 0;
        uint32_t blockBytes = 0;
        uint32_t bytesPerPixel = 0;
        if (CalculateDdsPayloadSize(header, totalMipSize, &topMipSize) &&
            GetBlockCompressionBytes(header.d3dFormat, blockBytes))
        {
            dds.flags |= DDSD_LINEARSIZE;
            dds.pitchOrLinearSize = static_cast<uint32_t>(std::min<size_t>(topMipSize, std::numeric_limits<uint32_t>::max()));
        }
        else if (GetBytesPerPixel(header.d3dFormat, bytesPerPixel))
        {
            dds.flags |= DDSD_PITCH;
            dds.pitchOrLinearSize = header.width * bytesPerPixel;
        }

        if (header.mipCount > 1)
        {
            dds.flags |= DDSD_MIPMAPCOUNT;
            dds.mipMapCount = header.mipCount;
            dds.caps |= DDSCAPS_COMPLEX | DDSCAPS_MIPMAP;
        }

        bool useDX10 = NeedsDX10Header(header.d3dFormat);

        if (useDX10)
        {
            // Use DX10 extended header for formats without legacy FourCC
            dds.ddspf.flags = DDPF_FOURCC;
            dds.ddspf.fourCC = DX10_FOURCC;
        }
        else
        {
            // Legacy FourCC mapping
            uint32_t fourCC = 0;
            switch (header.d3dFormat)
            {
            case 827611204: fourCC = 0x31545844; break; // DXT1
            case 861165636: fourCC = 0x33545844; break; // DXT3
            case 894720068: fourCC = 0x35545844; break; // DXT5
            case 826889281: fourCC = 0x31495441; break; // ATI1
            case 843666497: fourCC = 0x32495441; break; // ATI2

            // DXGI formats that can be mapped to FourCC
            case 71: case 72: fourCC = 0x31545844; break; // BC1
            case 74: case 75: fourCC = 0x33545844; break; // BC2
            case 77: case 78: fourCC = 0x35545844; break; // BC3
            case 80: case 81: fourCC = 0x31495441; break; // BC4
            case 83: case 84: fourCC = 0x32495441; break; // BC5

            default:
                // Fallback for RGBA8
                if (header.d3dFormat == 28 || header.d3dFormat == 29)
                {
                    dds.ddspf.flags = DDPF_RGB | DDPF_ALPHAPIXELS;
                    dds.ddspf.rgbBitCount = 32;
                    dds.ddspf.rBitMask = 0x000000FF;
                    dds.ddspf.gBitMask = 0x0000FF00;
                    dds.ddspf.bBitMask = 0x00FF0000;
                    dds.ddspf.aBitMask = 0xFF000000;
                    break;
                }
                error = "Unsupported D3D format for DDS passthrough: " + std::to_string(header.d3dFormat);
                return false;
            }

            if (fourCC != 0)
            {
                dds.ddspf.flags = DDPF_FOURCC;
                dds.ddspf.fourCC = fourCC;
            }
        }

        // Write DDS magic
        out.write(reinterpret_cast<const char*>(&DDS_MAGIC), 4);
        // Write 124-byte DDS header
        out.write(reinterpret_cast<const char*>(&dds), sizeof(dds));

        // Write DX10 extended header if needed
        if (useDX10)
        {
            DdsHeaderDX10 dx10;
            dx10.dxgiFormat = GetDxgiFormat(header.d3dFormat);
            dx10.resourceDimension = 3; // D3D10_RESOURCE_DIMENSION_TEXTURE2D
            dx10.miscFlag = 0;
            dx10.arraySize = 1;
            dx10.miscFlags2 = 0;
            out.write(reinterpret_cast<const char*>(&dx10), sizeof(dx10));
        }

        // Write pixel data
        out.write(reinterpret_cast<const char*>(payload.data()), payload.size());

        if (!out)
        {
            error = "Failed while writing DDS output.";
            return false;
        }

        return true;
    }
}
