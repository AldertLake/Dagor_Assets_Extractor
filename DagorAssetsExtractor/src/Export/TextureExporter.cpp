#include "Export/TextureExporter.h"
#include "Export/MaterialTextureResolver.h"
#include "DagorIO/BinaryReader.h"
#include "DagorIO/MappedFile.h"
#include "Platform/Platform.h"
#include "Platform/ThreadComWic.h"
#include "Export/ExportManifest.h"
#include "DagorIO/Decompression.h"
#include "Jobs/JobSystem.h"
#include "DirectXTex.h"

#include <objbase.h>
#include <wincodec.h>

#include <array>
#include <fstream>
#include <iostream>
#include <vector>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <mutex>
#include <string_view>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace DagorAssetsExtractor::Export
{
    // DDS format constants
    constexpr uint32_t DDS_MAGIC = 0x20534444; // "DDS "
    constexpr uint32_t DDPF_FOURCC = 0x00000004;
    constexpr uint32_t DDPF_ALPHAPIXELS = 0x00000001;
    constexpr uint32_t DDPF_RGB = 0x00000040;
    constexpr uint32_t DDPF_LUMINANCE = 0x00020000;

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

    enum class RasterPixelFormat
    {
        Gray8,
        Bgra8,
    };

    struct RasterImage
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t stride = 0;
        RasterPixelFormat format = RasterPixelFormat::Bgra8;
        std::vector<uint8_t> pixels;
    };

    struct BgraColor
    {
        uint8_t b = 0;
        uint8_t g = 0;
        uint8_t r = 0;
        uint8_t a = 255;
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

        std::string NormalizeTextureFormat(std::string_view value)
        {
            const std::string upper = ToUpperAscii(value);
            if (upper == "PNG" || upper == "TGA")
            {
                return upper;
            }

            return "DDS";
        }

        std::string_view TextureFormatExtension(std::string_view normalizedFormat)
        {
            if (normalizedFormat == "PNG")
            {
                return ".png";
            }

            if (normalizedFormat == "TGA")
            {
                return ".tga";
            }

            return ".dds";
        }

        uint16_t ReadUInt16LE(const uint8_t* data)
        {
            return static_cast<uint16_t>(data[0]) |
                (static_cast<uint16_t>(data[1]) << 8);
        }

        uint32_t ReadUInt32LE(const uint8_t* data)
        {
            return static_cast<uint32_t>(data[0]) |
                (static_cast<uint32_t>(data[1]) << 8) |
                (static_cast<uint32_t>(data[2]) << 16) |
                (static_cast<uint32_t>(data[3]) << 24);
        }

        float UInt32BitsToFloat(uint32_t bits)
        {
            float value = 0.0f;
            static_assert(sizeof(value) == sizeof(bits));
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }

        float HalfToFloat(uint16_t bits)
        {
            const uint32_t sign = (bits & 0x8000u) != 0 ? 1u : 0u;
            const uint32_t exponent = (bits >> 10) & 0x1Fu;
            const uint32_t mantissa = bits & 0x03FFu;

            if (exponent == 0)
            {
                if (mantissa == 0)
                {
                    return sign != 0 ? -0.0f : 0.0f;
                }

                const float value = std::ldexp(static_cast<float>(mantissa), -24);
                return sign != 0 ? -value : value;
            }

            if (exponent == 0x1Fu)
            {
                if (mantissa == 0)
                {
                    return sign != 0
                        ? -std::numeric_limits<float>::infinity()
                        : std::numeric_limits<float>::infinity();
                }

                return std::numeric_limits<float>::quiet_NaN();
            }

            const float value = std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f, static_cast<int>(exponent) - 15);
            return sign != 0 ? -value : value;
        }

        uint8_t FloatToUnormByte(float value)
        {
            if (!std::isfinite(value))
            {
                value = 0.0f;
            }

            value = std::clamp(value, 0.0f, 1.0f);
            return static_cast<uint8_t>(value * 255.0f + 0.5f);
        }

        uint64_t ReadUInt48LE(const uint8_t* data)
        {
            uint64_t value = 0;
            for (size_t i = 0; i < 6; ++i)
            {
                value |= static_cast<uint64_t>(data[i]) << (8 * i);
            }
            return value;
        }

        uint8_t Expand5To8(uint32_t value)
        {
            return static_cast<uint8_t>((value << 3) | (value >> 2));
        }

        uint8_t Expand6To8(uint32_t value)
        {
            return static_cast<uint8_t>((value << 2) | (value >> 4));
        }

        uint8_t NormalizeSnorm8ToUnormByte(int value)
        {
            const int clamped = std::clamp(value, -127, 127);
            const int mapped = ((clamped + 127) * 255 + 127) / 254;
            return static_cast<uint8_t>(std::clamp(mapped, 0, 255));
        }

        uint8_t InterpolateByte(uint8_t a, uint8_t b, uint32_t aWeight, uint32_t bWeight, uint32_t divisor)
        {
            return static_cast<uint8_t>(
                (static_cast<uint32_t>(a) * aWeight + static_cast<uint32_t>(b) * bWeight) / divisor);
        }

        int InterpolateSigned(int a, int b, int aWeight, int bWeight, int divisor)
        {
            return (a * aWeight + b * bWeight) / divisor;
        }

        BgraColor DecodeRgb565(uint16_t value)
        {
            BgraColor color;
            color.b = Expand5To8(value & 0x1F);
            color.g = Expand6To8((value >> 5) & 0x3F);
            color.r = Expand5To8((value >> 11) & 0x1F);
            color.a = 255;
            return color;
        }

        BgraColor MixColor(const BgraColor& a, const BgraColor& b, uint32_t aWeight, uint32_t bWeight, uint32_t divisor)
        {
            return BgraColor{
                InterpolateByte(a.b, b.b, aWeight, bWeight, divisor),
                InterpolateByte(a.g, b.g, aWeight, bWeight, divisor),
                InterpolateByte(a.r, b.r, aWeight, bWeight, divisor),
                InterpolateByte(a.a, b.a, aWeight, bWeight, divisor)
            };
        }

        void WriteBgraPixel(RasterImage& image, uint32_t x, uint32_t y, const BgraColor& color)
        {
            const size_t offset = static_cast<size_t>(y) * image.stride + static_cast<size_t>(x) * 4;
            image.pixels[offset + 0] = color.b;
            image.pixels[offset + 1] = color.g;
            image.pixels[offset + 2] = color.r;
            image.pixels[offset + 3] = color.a;
        }

        void WriteGrayPixel(RasterImage& image, uint32_t x, uint32_t y, uint8_t value)
        {
            const size_t offset = static_cast<size_t>(y) * image.stride + x;
            image.pixels[offset] = value;
        }

        void BuildBc1Palette(uint16_t color0, uint16_t color1, bool allowTransparent, BgraColor palette[4])
        {
            palette[0] = DecodeRgb565(color0);
            palette[1] = DecodeRgb565(color1);

            if (allowTransparent && color0 <= color1)
            {
                palette[2] = MixColor(palette[0], palette[1], 1, 1, 2);
                palette[3] = BgraColor{ 0, 0, 0, 0 };
                return;
            }

            palette[2] = MixColor(palette[0], palette[1], 2, 1, 3);
            palette[3] = MixColor(palette[0], palette[1], 1, 2, 3);
        }

        void DecodeBc4UnormValues(const uint8_t* block, uint8_t values[16])
        {
            uint8_t palette[8] = {};
            palette[0] = block[0];
            palette[1] = block[1];

            if (palette[0] > palette[1])
            {
                for (int i = 1; i <= 6; ++i)
                {
                    palette[i + 1] = static_cast<uint8_t>(
                        ((7 - i) * static_cast<uint32_t>(palette[0]) + i * static_cast<uint32_t>(palette[1])) / 7);
                }
            }
            else
            {
                for (int i = 1; i <= 4; ++i)
                {
                    palette[i + 1] = static_cast<uint8_t>(
                        ((5 - i) * static_cast<uint32_t>(palette[0]) + i * static_cast<uint32_t>(palette[1])) / 5);
                }
                palette[6] = 0;
                palette[7] = 255;
            }

            const uint64_t indices = ReadUInt48LE(block + 2);
            for (int i = 0; i < 16; ++i)
            {
                values[i] = palette[(indices >> (i * 3)) & 0x7];
            }
        }

        void DecodeBc4SnormValues(const uint8_t* block, uint8_t values[16])
        {
            int palette[8] = {};
            palette[0] = static_cast<int>(static_cast<int8_t>(block[0]));
            palette[1] = static_cast<int>(static_cast<int8_t>(block[1]));
            if (palette[0] == -128) palette[0] = -127;
            if (palette[1] == -128) palette[1] = -127;

            if (palette[0] > palette[1])
            {
                for (int i = 1; i <= 6; ++i)
                {
                    palette[i + 1] = InterpolateSigned(palette[0], palette[1], 7 - i, i, 7);
                }
            }
            else
            {
                for (int i = 1; i <= 4; ++i)
                {
                    palette[i + 1] = InterpolateSigned(palette[0], palette[1], 5 - i, i, 5);
                }
                palette[6] = -127;
                palette[7] = 127;
            }

            const uint64_t indices = ReadUInt48LE(block + 2);
            for (int i = 0; i < 16; ++i)
            {
                values[i] = NormalizeSnorm8ToUnormByte(palette[(indices >> (i * 3)) & 0x7]);
            }
        }

        void DecodeBcColorIndices(
            const uint8_t* block,
            RasterImage& image,
            uint32_t blockX,
            uint32_t blockY,
            const BgraColor palette[4],
            const uint8_t* alphaValues = nullptr)
        {
            const uint32_t indices = ReadUInt32LE(block + 4);
            for (uint32_t py = 0; py < 4; ++py)
            {
                const uint32_t y = blockY * 4 + py;
                if (y >= image.height)
                {
                    continue;
                }

                for (uint32_t px = 0; px < 4; ++px)
                {
                    const uint32_t x = blockX * 4 + px;
                    if (x >= image.width)
                    {
                        continue;
                    }

                    const uint32_t pixelIndex = py * 4 + px;
                    BgraColor color = palette[(indices >> (pixelIndex * 2)) & 0x3];
                    if (alphaValues)
                    {
                        color.a = alphaValues[pixelIndex];
                    }
                    WriteBgraPixel(image, x, y, color);
                }
            }
        }

        constexpr uint64_t ParallelPixelThreshold = 512ull * 512ull;

        bool DecodeBlockCompressedTopMip(
            const DagorFormats::DdsxHeader& header,
            std::span<const uint8_t> topMip,
            RasterImage& image,
            std::string& error)
        {
            enum class BcMode
            {
                Bc1,
                Bc2,
                Bc3,
                Bc4Unorm,
                Bc4Snorm,
                Bc5Unorm,
                Bc5Snorm,
            };

            BcMode mode = BcMode::Bc1;
            size_t blockBytes = 0;
            bool grayOutput = false;

            switch (header.d3dFormat)
            {
            case 827611204: // DXT1
            case 71: case 72: // BC1
                mode = BcMode::Bc1;
                blockBytes = 8;
                break;

            case 861165636: // DXT3
            case 74: case 75: // BC2
                mode = BcMode::Bc2;
                blockBytes = 16;
                break;

            case 894720068: // DXT5
            case 77: case 78: // BC3
                mode = BcMode::Bc3;
                blockBytes = 16;
                break;

            case 826889281: // ATI1
            case 80: // BC4_UNORM
                mode = BcMode::Bc4Unorm;
                blockBytes = 8;
                grayOutput = true;
                break;

            case 81: // BC4_SNORM
                mode = BcMode::Bc4Snorm;
                blockBytes = 8;
                grayOutput = true;
                break;

            case 843666497: // ATI2
            case 83: // BC5_UNORM
                mode = BcMode::Bc5Unorm;
                blockBytes = 16;
                break;

            case 84: // BC5_SNORM
                mode = BcMode::Bc5Snorm;
                blockBytes = 16;
                break;

            default:
                error = "Readable PNG/TGA export does not support DDSx format code " +
                    std::to_string(header.d3dFormat) + " (" + std::string(header.FormatName()) + ").";
                return false;
            }

            const uint32_t blocksWide = std::max<uint32_t>(1, (static_cast<uint32_t>(header.width) + 3) / 4);
            const uint32_t blocksHigh = std::max<uint32_t>(1, (static_cast<uint32_t>(header.height) + 3) / 4);
            const size_t expectedBytes = static_cast<size_t>(blocksWide) * static_cast<size_t>(blocksHigh) * blockBytes;
            if (topMip.size() < expectedBytes)
            {
                error = "DDS payload does not contain enough top-mip block data for readable export.";
                return false;
            }

            image.format = grayOutput ? RasterPixelFormat::Gray8 : RasterPixelFormat::Bgra8;
            image.stride = grayOutput ? header.width : header.width * 4;
            image.pixels.assign(static_cast<size_t>(image.stride) * image.height, 0);

            auto decodeBlockRow = [&](uint32_t blockY) {
                for (uint32_t blockX = 0; blockX < blocksWide; ++blockX)
                {
                    const size_t blockIndex = static_cast<size_t>(blockY) * blocksWide + blockX;
                    const uint8_t* block = topMip.data() + blockIndex * blockBytes;

                    switch (mode)
                    {
                    case BcMode::Bc1:
                    {
                        BgraColor palette[4] = {};
                        BuildBc1Palette(ReadUInt16LE(block), ReadUInt16LE(block + 2), true, palette);
                        DecodeBcColorIndices(block, image, blockX, blockY, palette);
                        break;
                    }

                    case BcMode::Bc2:
                    {
                        uint8_t alphaValues[16] = {};
                        const uint64_t alphaBits = static_cast<uint64_t>(ReadUInt32LE(block)) |
                            (static_cast<uint64_t>(ReadUInt32LE(block + 4)) << 32);
                        for (int i = 0; i < 16; ++i)
                        {
                            alphaValues[i] = static_cast<uint8_t>(((alphaBits >> (i * 4)) & 0xF) * 17);
                        }

                        BgraColor palette[4] = {};
                        BuildBc1Palette(ReadUInt16LE(block + 8), ReadUInt16LE(block + 10), false, palette);
                        DecodeBcColorIndices(block + 8, image, blockX, blockY, palette, alphaValues);
                        break;
                    }

                    case BcMode::Bc3:
                    {
                        uint8_t alphaValues[16] = {};
                        DecodeBc4UnormValues(block, alphaValues);

                        BgraColor palette[4] = {};
                        BuildBc1Palette(ReadUInt16LE(block + 8), ReadUInt16LE(block + 10), false, palette);
                        DecodeBcColorIndices(block + 8, image, blockX, blockY, palette, alphaValues);
                        break;
                    }

                    case BcMode::Bc4Unorm:
                    case BcMode::Bc4Snorm:
                    {
                        uint8_t grayValues[16] = {};
                        if (mode == BcMode::Bc4Unorm)
                        {
                            DecodeBc4UnormValues(block, grayValues);
                        }
                        else
                        {
                            DecodeBc4SnormValues(block, grayValues);
                        }

                        for (uint32_t py = 0; py < 4; ++py)
                        {
                            const uint32_t y = blockY * 4 + py;
                            if (y >= image.height)
                            {
                                continue;
                            }

                            for (uint32_t px = 0; px < 4; ++px)
                            {
                                const uint32_t x = blockX * 4 + px;
                                if (x >= image.width)
                                {
                                    continue;
                                }

                                WriteGrayPixel(image, x, y, grayValues[py * 4 + px]);
                            }
                        }
                        break;
                    }

                    case BcMode::Bc5Unorm:
                    case BcMode::Bc5Snorm:
                    {
                        uint8_t redValues[16] = {};
                        uint8_t greenValues[16] = {};
                        if (mode == BcMode::Bc5Unorm)
                        {
                            DecodeBc4UnormValues(block, redValues);
                            DecodeBc4UnormValues(block + 8, greenValues);
                        }
                        else
                        {
                            DecodeBc4SnormValues(block, redValues);
                            DecodeBc4SnormValues(block + 8, greenValues);
                        }

                        for (uint32_t py = 0; py < 4; ++py)
                        {
                            const uint32_t y = blockY * 4 + py;
                            if (y >= image.height)
                            {
                                continue;
                            }

                            for (uint32_t px = 0; px < 4; ++px)
                            {
                                const uint32_t x = blockX * 4 + px;
                                if (x >= image.width)
                                {
                                    continue;
                                }

                                const uint32_t pixelIndex = py * 4 + px;
                                WriteBgraPixel(image, x, y, BgraColor{
                                    0,
                                    greenValues[pixelIndex],
                                    redValues[pixelIndex],
                                    255
                                });
                            }
                        }
                        break;
                    }
                    }
                }
            };

            const uint64_t pixelCount = static_cast<uint64_t>(header.width) * static_cast<uint64_t>(header.height);
            if (pixelCount >= ParallelPixelThreshold && blocksHigh >= 64)
            {
                Jobs::JobSystem::RunParallelFor(blocksHigh, 16, [&](size_t rowIndex) {
                    decodeBlockRow(static_cast<uint32_t>(rowIndex));
                });
            }
            else
            {
                for (uint32_t blockY = 0; blockY < blocksHigh; ++blockY)
                {
                    decodeBlockRow(blockY);
                }
            }

            return true;
        }

        bool DecodeFloatTopMip(
            const DagorFormats::DdsxHeader& header,
            std::span<const uint8_t> topMip,
            RasterImage& image,
            std::string& error)
        {
            const size_t pixelCount = static_cast<size_t>(header.width) * static_cast<size_t>(header.height);
            image.format = RasterPixelFormat::Bgra8;
            image.stride = header.width * 4;
            image.pixels.resize(pixelCount * 4);

            switch (header.d3dFormat)
            {
            case 10: // R16G16B16A16_FLOAT
                if (topMip.size() < pixelCount * 8)
                {
                    error = "DDS payload does not contain enough half-float top-mip data for readable export.";
                    return false;
                }

                for (size_t i = 0; i < pixelCount; ++i)
                {
                    const size_t src = i * 8;
                    const size_t dst = i * 4;
                    const float r = HalfToFloat(ReadUInt16LE(topMip.data() + src + 0));
                    const float g = HalfToFloat(ReadUInt16LE(topMip.data() + src + 2));
                    const float b = HalfToFloat(ReadUInt16LE(topMip.data() + src + 4));
                    const float a = HalfToFloat(ReadUInt16LE(topMip.data() + src + 6));
                    image.pixels[dst + 0] = FloatToUnormByte(b);
                    image.pixels[dst + 1] = FloatToUnormByte(g);
                    image.pixels[dst + 2] = FloatToUnormByte(r);
                    image.pixels[dst + 3] = FloatToUnormByte(a);
                }
                return true;

            case 2: // R32G32B32A32_FLOAT
                if (topMip.size() < pixelCount * 16)
                {
                    error = "DDS payload does not contain enough float32 top-mip data for readable export.";
                    return false;
                }

                for (size_t i = 0; i < pixelCount; ++i)
                {
                    const size_t src = i * 16;
                    const size_t dst = i * 4;
                    const float r = UInt32BitsToFloat(ReadUInt32LE(topMip.data() + src + 0));
                    const float g = UInt32BitsToFloat(ReadUInt32LE(topMip.data() + src + 4));
                    const float b = UInt32BitsToFloat(ReadUInt32LE(topMip.data() + src + 8));
                    const float a = UInt32BitsToFloat(ReadUInt32LE(topMip.data() + src + 12));
                    image.pixels[dst + 0] = FloatToUnormByte(b);
                    image.pixels[dst + 1] = FloatToUnormByte(g);
                    image.pixels[dst + 2] = FloatToUnormByte(r);
                    image.pixels[dst + 3] = FloatToUnormByte(a);
                }
                return true;

            default:
                error = "Readable PNG/TGA export does not support this float DDSx format.";
                return false;
            }
        }

        std::string FormatHresult(HRESULT value)
        {
            std::ostringstream out;
            out << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<uint32_t>(value);
            return out.str();
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

        std::filesystem::path BuildMaterialMapOutputPath(
            const std::filesystem::path& baseOutputPath,
            std::string_view assetName,
            TextureMapKind mapKind)
        {
            std::string normalizedName = NormalizeTextureSemanticName(assetName);
            if (normalizedName.empty())
            {
                normalizedName = "texture";
            }

            const std::string suffix(TextureMapKindFileSuffix(mapKind));
            const std::string_view suffixToken =
                suffix.starts_with('_') ? std::string_view(suffix).substr(1) : std::string_view(suffix);
            const bool alreadyHasSuffix =
                normalizedName == suffixToken ||
                (normalizedName.size() > suffixToken.size() &&
                 normalizedName.ends_with(suffixToken) &&
                 normalizedName[normalizedName.size() - suffixToken.size() - 1] == '_');

            std::filesystem::path outputPath = baseOutputPath;
            outputPath.replace_filename(
                normalizedName +
                (alreadyHasSuffix ? std::string() : suffix) +
                baseOutputPath.extension().string());
            return outputPath;
        }

        bool TryGetPrimaryMaterialMapKind(
            TextureContentLayout layout,
            TextureMapKind& mapKind)
        {
            switch (layout)
            {
            case TextureContentLayout::BaseColor:
                mapKind = TextureMapKind::BaseColor;
                return true;
            case TextureContentLayout::ConventionalNormal:
            case TextureContentLayout::WarThunderPackedNormalPbr:
                mapKind = TextureMapKind::Normal;
                return true;
            case TextureContentLayout::AmbientOcclusion:
                mapKind = TextureMapKind::AmbientOcclusion;
                return true;
            case TextureContentLayout::PackedOrm:
            case TextureContentLayout::PackedRma:
            case TextureContentLayout::PackedMra:
                mapKind = TextureMapKind::OcclusionRoughnessMetallic;
                return true;
            case TextureContentLayout::Unknown:
                return false;
            }
            return false;
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
            case 540492610:   // BC7 FourCC
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
            case 21:          // D3DFMT_A8R8G8B8
            case 28: case 29: // R8G8B8A8
            case 87: case 91: // B8G8R8A8
                bytesPerPixel = 4;
                return true;

            case 50: // D3DFMT_L8
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

            std::vector<uint8_t> reordered(payload.size());
            size_t sourceOffset = 0;
            size_t targetEnd = expectedSize;
            for (size_t sourceIndex = 0; sourceIndex < mipSizes.size(); ++sourceIndex)
            {
                const size_t targetIndex = mipSizes.size() - 1 - sourceIndex;
                const size_t mipSize = mipSizes[targetIndex];
                targetEnd -= mipSize;
                if (mipSize > 0)
                {
                    std::memcpy(reordered.data() + targetEnd, payload.data() + sourceOffset, mipSize);
                }
                sourceOffset += mipSize;
            }

            if (payload.size() > expectedSize)
            {
                std::memcpy(
                    reordered.data() + expectedSize,
                    payload.data() + expectedSize,
                    payload.size() - expectedSize);
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

        enum class TexturePreparationFailure
        {
            None,
            OodleUnavailable,
            OodleDecompressionFailed,
        };

        struct PreparedTexturePayload
        {
            DagorFormats::DdsxHeader header;
            std::vector<uint8_t> payload;
            std::vector<uint8_t> rawPackPayload;
            std::vector<std::string> warnings;
            std::string error;
            size_t packPayloadSize = 0;
            TexturePreparationFailure failure = TexturePreparationFailure::None;
        };

        bool PrepareTexturePayload(
            const std::filesystem::path& packPath,
            const AssetIndex::SubAssetRecord& entry,
            const std::filesystem::path& gameRootPath,
            const std::filesystem::path& oodleDllPathValue,
            bool useOodleFromGameFolder,
            PreparedTexturePayload& prepared,
            std::span<const uint8_t> mappedPackData = {})
        {
            if (entry.ddsxHeader.size() < 32)
            {
                prepared.error = "Missing DDSx header metadata for DXP entry. Reload the DXP contents before exporting.";
                return false;
            }

            const uintmax_t entryOffset = entry.offset;
            const uintmax_t entrySize = entry.size;
            if (entrySize == 0)
            {
                prepared.error = "DXP entry has no payload data.";
                return false;
            }

            DagorFormats::DdsxReader reader(entry.ddsxHeader);
            reader.Parse();
            prepared.header = reader.GetHeader();

            std::span<const uint8_t> compressedData;
            if (!mappedPackData.empty())
            {
                const uintmax_t fileSize = static_cast<uintmax_t>(mappedPackData.size());
                if (entryOffset > fileSize || entrySize > fileSize - entryOffset)
                {
                    prepared.error = "DXP entry payload is outside the pack file bounds.";
                    return false;
                }

                compressedData = mappedPackData.subspan(static_cast<size_t>(entryOffset), static_cast<size_t>(entrySize));
                prepared.packPayloadSize = compressedData.size();
            }
            else
            {
                std::ifstream file(packPath, std::ios::binary | std::ios::ate);
                if (!file)
                {
                    prepared.error = "Could not open pack file: " + Platform::PathToUtf8(packPath);
                    return false;
                }

                const std::streampos endPos = file.tellg();
                if (endPos < 0)
                {
                    prepared.error = "Could not determine pack file size.";
                    return false;
                }

                const uintmax_t fileSize = static_cast<uintmax_t>(endPos);
                if (entryOffset > fileSize || entrySize > fileSize - entryOffset)
                {
                    prepared.error = "DXP entry payload is outside the pack file bounds.";
                    return false;
                }

                file.seekg(static_cast<std::streamoff>(entry.offset), std::ios::beg);
                prepared.rawPackPayload.resize(entry.size);
                if (!file.read(
                        reinterpret_cast<char*>(prepared.rawPackPayload.data()),
                        static_cast<std::streamsize>(prepared.rawPackPayload.size())))
                {
                    prepared.error = "Could not read DDSx payload data from pack.";
                    return false;
                }
                prepared.packPayloadSize = prepared.rawPackPayload.size();
                compressedData = std::span<const uint8_t>(
                    prepared.rawPackPayload.data(),
                    prepared.rawPackPayload.size());
            }

            if (prepared.header.IsCompressed())
            {
                if (prepared.header.memorySize == 0)
                {
                    prepared.error = "Compressed DDSx entry is missing decompressed payload size. " +
                        BuildDdsxDiagnostic(prepared.header, prepared.packPayloadSize);
                    return false;
                }

                if (prepared.header.packedSize != 0 &&
                    prepared.header.packedSize != prepared.packPayloadSize)
                {
                    prepared.warnings.push_back(
                        "DXP table payload size (" + std::to_string(prepared.packPayloadSize) +
                        ") differs from DDSx packed size (" + std::to_string(prepared.header.packedSize) + ").");
                }

                const std::string gameRoot = useOodleFromGameFolder
                    ? Platform::PathToUtf8(gameRootPath)
                    : std::string();
                const std::string oodleDllPath = Platform::PathToUtf8(oodleDllPathValue);

                auto populateRawFallbackPayloadIfNeeded = [&]() {
                    if (prepared.rawPackPayload.empty() && !compressedData.empty())
                    {
                        prepared.rawPackPayload.assign(compressedData.begin(), compressedData.end());
                    }
                };

                try
                {
                    if (prepared.header.compressionType == 1)
                    {
                        prepared.payload = DagorIO::Decompressor::DecompressZlib(
                            compressedData,
                            prepared.header.memorySize);
                    }
                    else if (prepared.header.compressionType == 2)
                    {
                        prepared.payload = DagorIO::Decompressor::DecompressZstd(
                            compressedData,
                            prepared.header.memorySize);
                    }
                    else if (prepared.header.compressionType == 3)
                    {
                        const std::filesystem::path oodlePath =
                            DagorIO::Decompressor::FindOodleDll(gameRoot, oodleDllPath);
                        if (oodlePath.empty())
                        {
                            populateRawFallbackPayloadIfNeeded();
                            prepared.failure = TexturePreparationFailure::OodleUnavailable;
                            prepared.error =
                                "Oodle decompression required but oo2core_6_win64.dll was not found. Checked: " +
                                DagorIO::Decompressor::OodleSearchDescription(gameRoot, oodleDllPath) + ". " +
                                BuildDdsxDiagnostic(prepared.header, prepared.packPayloadSize);
                            return false;
                        }
                        prepared.payload = DagorIO::Decompressor::DecompressOodle(
                            compressedData,
                            prepared.header.memorySize,
                            gameRoot,
                            oodleDllPath);
                    }
                    else if (prepared.header.compressionType == 4)
                    {
                        prepared.payload = DagorIO::Decompressor::DecompressLzma(
                            compressedData,
                            prepared.header.memorySize);
                    }
                    else
                    {
                        prepared.error =
                            "Unsupported DDSx compression: " +
                            std::string(prepared.header.CompressionName()) +
                            " (normalized type " + std::to_string(prepared.header.compressionType) + "). " +
                            BuildDdsxDiagnostic(prepared.header, prepared.packPayloadSize);
                        return false;
                    }
                }
                catch (const std::exception& ex)
                {
                    if (prepared.header.compressionType == 3)
                    {
                        populateRawFallbackPayloadIfNeeded();
                        prepared.failure = TexturePreparationFailure::OodleDecompressionFailed;
                        prepared.error = std::string("Oodle decompression failed: ") + ex.what() + ". " +
                            BuildDdsxDiagnostic(prepared.header, prepared.packPayloadSize);
                        return false;
                    }

                    prepared.error = std::string("DDSx decompression failed: ") + ex.what() + ". " +
                        BuildDdsxDiagnostic(prepared.header, prepared.packPayloadSize);
                    return false;
                }

                prepared.warnings.push_back(
                    "Decompressed from " + std::string(prepared.header.CompressionName()) +
                    " (" + std::to_string(prepared.packPayloadSize) +
                    " -> " + std::to_string(prepared.payload.size()) + " bytes)");
                std::vector<uint8_t>().swap(prepared.rawPackPayload);
            }
            else if (!prepared.rawPackPayload.empty())
            {
                prepared.payload = std::move(prepared.rawPackPayload);
            }
            else
            {
                prepared.payload.assign(compressedData.begin(), compressedData.end());
            }

            uint32_t actualMips = 0;
            size_t accumulatedMipSize = 0;
            std::vector<size_t> mipSizes;
            if (CalculateDdsMipSizes(prepared.header, mipSizes))
            {
                for (const size_t mipSize : mipSizes)
                {
                    if (accumulatedMipSize + mipSize <= prepared.payload.size())
                    {
                        accumulatedMipSize += mipSize;
                        ++actualMips;
                    }
                    else
                    {
                        break;
                    }
                }
            }

            if (actualMips > 0 &&
                actualMips < std::max<uint32_t>(1, prepared.header.mipCount))
            {
                prepared.warnings.push_back(
                    "DDS payload size (" + std::to_string(prepared.payload.size()) +
                    " bytes) is smaller than expected for " +
                    std::to_string(prepared.header.mipCount) +
                    " mip(s). Clamping mip count to " + std::to_string(actualMips) +
                    " based on available data.");
                prepared.header.mipCount = static_cast<uint8_t>(actualMips);
            }

            std::string mipOrderWarning;
            std::string mipOrderError;
            if (!NormalizeMipOrderIfNeeded(
                    prepared.header,
                    prepared.payload,
                    mipOrderWarning,
                    mipOrderError))
            {
                prepared.error = mipOrderError + " " +
                    BuildDdsxDiagnostic(prepared.header, prepared.packPayloadSize);
                return false;
            }
            if (!mipOrderWarning.empty())
            {
                prepared.warnings.push_back(std::move(mipOrderWarning));
            }

            size_t expectedPayloadSize = 0;
            if (CalculateDdsPayloadSize(prepared.header, expectedPayloadSize))
            {
                if (prepared.payload.size() < expectedPayloadSize)
                {
                    prepared.error =
                        "DDS payload is too small for " + std::string(prepared.header.FormatName()) +
                        " " + std::to_string(prepared.header.width) + "x" +
                        std::to_string(prepared.header.height) + " with " +
                        std::to_string(std::max<uint32_t>(1, prepared.header.mipCount)) +
                        " mip(s): expected at least " + std::to_string(expectedPayloadSize) +
                        " bytes, got " + std::to_string(prepared.payload.size()) + ". " +
                        BuildDdsxDiagnostic(prepared.header, prepared.packPayloadSize);
                    return false;
                }

                if (prepared.payload.size() != expectedPayloadSize)
                {
                    prepared.warnings.push_back(
                        "DDS payload size (" + std::to_string(prepared.payload.size()) +
                        ") differs from expected mip data size (" +
                        std::to_string(expectedPayloadSize) + ").");
                }
            }
            else
            {
                prepared.warnings.push_back(
                    "Could not validate DDS payload size for format code " +
                    std::to_string(prepared.header.d3dFormat) + ".");
            }

            return true;
        }

        bool IsSrgbTextureFormat(uint32_t d3dFormat)
        {
            switch (d3dFormat)
            {
            case 29: // R8G8B8A8_UNORM_SRGB
            case 72: // BC1_UNORM_SRGB
            case 75: // BC2_UNORM_SRGB
            case 78: // BC3_UNORM_SRGB
            case 91: // B8G8R8A8_UNORM_SRGB
            case 99: // BC7_UNORM_SRGB
                return true;
            default:
                return false;
            }
        }

        bool DecodeTopMipToRaster(
            const DagorFormats::DdsxHeader& header,
            const std::vector<uint8_t>& payload,
            RasterImage& image,
            std::string& error)
        {
            size_t topMipBytes = 0;
            size_t totalBytes = 0;
            if (!CalculateDdsPayloadSize(header, totalBytes, &topMipBytes))
            {
                error = "Readable PNG/TGA export does not support DDSx format code " +
                    std::to_string(header.d3dFormat) + " (" + std::string(header.FormatName()) + ").";
                return false;
            }

            if (payload.size() < topMipBytes)
            {
                error = "DDS payload does not contain the full top mip for readable export.";
                return false;
            }

            const size_t pixelCount = static_cast<size_t>(header.width) * static_cast<size_t>(header.height);
            image.width = header.width;
            image.height = header.height;

            switch (header.d3dFormat)
            {
            case 827611204:
            case 861165636:
            case 894720068:
            case 826889281:
            case 843666497:
            case 71: case 72:
            case 74: case 75:
            case 77: case 78:
            case 80: case 81:
            case 83: case 84:
                return DecodeBlockCompressedTopMip(
                    header,
                    std::span<const uint8_t>(payload.data(), topMipBytes),
                    image,
                    error);

            case 28:
            case 29:
            {
                image.format = RasterPixelFormat::Bgra8;
                image.stride = header.width * 4;
                image.pixels.resize(pixelCount * 4);
                for (size_t i = 0; i < pixelCount; ++i)
                {
                    const size_t src = i * 4;
                    image.pixels[src + 0] = payload[src + 2];
                    image.pixels[src + 1] = payload[src + 1];
                    image.pixels[src + 2] = payload[src + 0];
                    image.pixels[src + 3] = payload[src + 3];
                }
                return true;
            }

            case 21:
            case 87:
            case 91:
                image.format = RasterPixelFormat::Bgra8;
                image.stride = header.width * 4;
                image.pixels.assign(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(topMipBytes));
                return true;

            case 50:
            case 61:
                image.format = RasterPixelFormat::Gray8;
                image.stride = header.width;
                image.pixels.assign(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(topMipBytes));
                return true;

            case 49:
                image.format = RasterPixelFormat::Bgra8;
                image.stride = header.width * 4;
                image.pixels.resize(pixelCount * 4);
                for (size_t i = 0; i < pixelCount; ++i)
                {
                    const size_t src = i * 2;
                    const size_t dst = i * 4;
                    image.pixels[dst + 0] = 0;
                    image.pixels[dst + 1] = payload[src + 1];
                    image.pixels[dst + 2] = payload[src + 0];
                    image.pixels[dst + 3] = 255;
                }
                return true;

            case 10:
            case 2:
                return DecodeFloatTopMip(
                    header,
                    std::span<const uint8_t>(payload.data(), topMipBytes),
                    image,
                    error);

            default:
                error = "The built-in readable PNG/TGA decoder currently supports BC1-BC5 plus uncompressed R8, R8G8, RGBA8, BGRA8, and RGBA float DDSx textures only. "
                    "Other DDSx formats are retried through DirectXTex. "
                    "This texture is " + std::string(header.FormatName()) + " (format code " + std::to_string(header.d3dFormat) + ").";
                return false;
            }
        }

        bool EncodeRasterToWicStream(
            IWICImagingFactory* factory,
            IStream* stream,
            const RasterImage& image,
            std::string& error)
        {
            IWICBitmapEncoder* encoder = nullptr;
            IWICBitmapFrameEncode* frame = nullptr;
            IPropertyBag2* properties = nullptr;

            auto releaseAll = [&]() {
                if (properties) properties->Release();
                if (frame) frame->Release();
                if (encoder) encoder->Release();
            };

            HRESULT hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
            if (FAILED(hr))
            {
                error = "Could not create the PNG encoder (" + FormatHresult(hr) + ").";
                releaseAll();
                return false;
            }

            hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
            if (FAILED(hr))
            {
                error = "Could not initialize the PNG encoder (" + FormatHresult(hr) + ").";
                releaseAll();
                return false;
            }

            hr = encoder->CreateNewFrame(&frame, &properties);
            if (FAILED(hr))
            {
                error = "Could not create the PNG frame (" + FormatHresult(hr) + ").";
                releaseAll();
                return false;
            }

            hr = frame->Initialize(properties);
            if (FAILED(hr))
            {
                error = "Could not initialize the PNG frame (" + FormatHresult(hr) + ").";
                releaseAll();
                return false;
            }

            hr = frame->SetSize(image.width, image.height);
            if (FAILED(hr))
            {
                error = "Could not set the PNG frame size (" + FormatHresult(hr) + ").";
                releaseAll();
                return false;
            }

            WICPixelFormatGUID pixelFormat = image.format == RasterPixelFormat::Gray8
                ? GUID_WICPixelFormat8bppGray
                : GUID_WICPixelFormat32bppBGRA;
            const WICPixelFormatGUID expectedPixelFormat = pixelFormat;
            hr = frame->SetPixelFormat(&pixelFormat);
            if (FAILED(hr))
            {
                error = "Could not set the PNG pixel format (" + FormatHresult(hr) + ").";
                releaseAll();
                return false;
            }

            if (!IsEqualGUID(pixelFormat, expectedPixelFormat))
            {
                error = "The PNG encoder rejected the requested pixel format.";
                releaseAll();
                return false;
            }

            hr = frame->WritePixels(
                image.height,
                image.stride,
                static_cast<UINT>(image.pixels.size()),
                const_cast<BYTE*>(image.pixels.data()));
            if (FAILED(hr))
            {
                error = "Failed while writing PNG pixels (" + FormatHresult(hr) + ").";
                releaseAll();
                return false;
            }

            hr = frame->Commit();
            if (FAILED(hr))
            {
                error = "Failed to finalize the PNG frame (" + FormatHresult(hr) + ").";
                releaseAll();
                return false;
            }

            hr = encoder->Commit();
            if (FAILED(hr))
            {
                error = "Failed to finalize the PNG file (" + FormatHresult(hr) + ").";
                releaseAll();
                return false;
            }

            releaseAll();
            return true;
        }

        bool WritePng(const std::filesystem::path& outputPath, const RasterImage& image, std::string& error)
        {
            Platform::WicFactoryLease wicLease;
            if (!wicLease.IsReady())
            {
                error = "Could not initialize WIC for PNG export (" +
                    FormatHresult(static_cast<HRESULT>(wicLease.ResultCode())) + ").";
                return false;
            }

            IWICImagingFactory* factory = wicLease.Get();
            IWICStream* stream = nullptr;
            HRESULT hr = factory->CreateStream(&stream);
            if (FAILED(hr))
            {
                error = "Could not create the WIC output stream (" + FormatHresult(hr) + ").";
                return false;
            }

            hr = stream->InitializeFromFilename(outputPath.c_str(), GENERIC_WRITE);
            if (FAILED(hr))
            {
                error = "Could not open the PNG output path (" + FormatHresult(hr) + ").";
                stream->Release();
                return false;
            }

            const bool ok = EncodeRasterToWicStream(factory, stream, image, error);
            stream->Release();
            return ok;
        }

        bool EncodeRasterToPngMemory(const RasterImage& image, std::vector<uint8_t>& outBytes, std::string& error)
        {
            outBytes.clear();
            Platform::WicFactoryLease wicLease;
            if (!wicLease.IsReady())
            {
                error = "Could not initialize WIC for PNG export (" +
                    FormatHresult(static_cast<HRESULT>(wicLease.ResultCode())) + ").";
                return false;
            }

            IStream* memStream = nullptr;
            HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &memStream);
            if (FAILED(hr) || memStream == nullptr)
            {
                error = "Could not allocate in-memory HGLOBAL stream for PNG encoding (" + FormatHresult(hr) + ").";
                return false;
            }

            if (!EncodeRasterToWicStream(wicLease.Get(), memStream, image, error))
            {
                memStream->Release();
                return false;
            }

            STATSTG stat = {};
            hr = memStream->Stat(&stat, STATFLAG_NONAME);
            if (FAILED(hr) || stat.cbSize.QuadPart == 0 ||
                stat.cbSize.QuadPart > std::numeric_limits<size_t>::max())
            {
                error = "Invalid in-memory PNG stream size (" + FormatHresult(hr) + ").";
                memStream->Release();
                return false;
            }

            HGLOBAL hGlobal = nullptr;
            hr = GetHGlobalFromStream(memStream, &hGlobal);
            if (FAILED(hr) || hGlobal == nullptr)
            {
                error = "Could not access HGLOBAL memory for encoded PNG (" + FormatHresult(hr) + ").";
                memStream->Release();
                return false;
            }

            const size_t pngSize = static_cast<size_t>(stat.cbSize.QuadPart);
            const void* locked = GlobalLock(hGlobal);
            if (locked == nullptr)
            {
                error = "Could not lock HGLOBAL memory for encoded PNG.";
                memStream->Release();
                return false;
            }

            outBytes.resize(pngSize);
            std::memcpy(outBytes.data(), locked, pngSize);
            GlobalUnlock(hGlobal);
            memStream->Release();
            return true;
        }

        bool DecodeWicDecoderToBgra(
            IWICImagingFactory* factory,
            IWICBitmapDecoder* decoder,
            RasterImage& image,
            std::string& error)
        {
            IWICBitmapFrameDecode* frame = nullptr;
            IWICFormatConverter* converter = nullptr;
            auto releaseAll = [&]() {
                if (converter) converter->Release();
                if (frame) frame->Release();
            };

            HRESULT hr = decoder->GetFrame(0, &frame);
            if (FAILED(hr))
            {
                error = "Could not read the first image frame (" + FormatHresult(hr) + ").";
                releaseAll();
                return false;
            }

            UINT width = 0;
            UINT height = 0;
            hr = frame->GetSize(&width, &height);
            if (FAILED(hr) || width == 0 || height == 0)
            {
                error = "Image has invalid dimensions.";
                releaseAll();
                return false;
            }

            hr = factory->CreateFormatConverter(&converter);
            if (FAILED(hr))
            {
                error = "Could not create WIC format converter (" + FormatHresult(hr) + ").";
                releaseAll();
                return false;
            }

            hr = converter->Initialize(
                frame,
                GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone,
                nullptr,
                0.0,
                WICBitmapPaletteTypeCustom);
            if (FAILED(hr))
            {
                error = "Could not convert image to BGRA8 (" + FormatHresult(hr) + ").";
                releaseAll();
                return false;
            }

            const uint64_t stride64 = static_cast<uint64_t>(width) * 4ull;
            const uint64_t bytes64 = stride64 * static_cast<uint64_t>(height);
            if (stride64 > std::numeric_limits<uint32_t>::max() ||
                bytes64 > std::numeric_limits<UINT>::max() ||
                bytes64 > std::numeric_limits<size_t>::max())
            {
                error = "Image is too large to load safely.";
                releaseAll();
                return false;
            }

            image.width = width;
            image.height = height;
            image.stride = static_cast<uint32_t>(stride64);
            image.format = RasterPixelFormat::Bgra8;
            image.pixels.resize(static_cast<size_t>(bytes64));
            hr = converter->CopyPixels(
                nullptr,
                image.stride,
                static_cast<UINT>(image.pixels.size()),
                image.pixels.data());
            if (FAILED(hr))
            {
                error = "Could not copy decoded image pixels (" + FormatHresult(hr) + ").";
                releaseAll();
                return false;
            }

            releaseAll();
            return true;
        }

        bool LoadWicBgra(
            const std::filesystem::path& inputPath,
            RasterImage& image,
            std::string& error)
        {
            Platform::WicFactoryLease wicLease;
            if (!wicLease.IsReady())
            {
                error = "Could not initialize WIC for image loading (" +
                    FormatHresult(static_cast<HRESULT>(wicLease.ResultCode())) + ").";
                return false;
            }

            IWICImagingFactory* factory = wicLease.Get();
            IWICBitmapDecoder* decoder = nullptr;
            HRESULT hr = factory->CreateDecoderFromFilename(
                inputPath.c_str(),
                nullptr,
                GENERIC_READ,
                WICDecodeMetadataCacheOnLoad,
                &decoder);
            if (FAILED(hr))
            {
                error = "Could not open image " + Platform::PathToUtf8(inputPath) +
                    " (" + FormatHresult(hr) + ").";
                if (decoder) decoder->Release();
                return false;
            }

            const bool ok = DecodeWicDecoderToBgra(factory, decoder, image, error);
            decoder->Release();
            return ok;
        }

        bool LoadWicBgraFromMemory(
            std::span<const uint8_t> encodedBytes,
            RasterImage& image,
            std::string& error)
        {
            if (encodedBytes.empty() || encodedBytes.size() > std::numeric_limits<DWORD>::max())
            {
                error = "In-memory image payload is empty or exceeds WIC size limits.";
                return false;
            }

            Platform::WicFactoryLease wicLease;
            if (!wicLease.IsReady())
            {
                error = "Could not initialize WIC for image loading (" +
                    FormatHresult(static_cast<HRESULT>(wicLease.ResultCode())) + ").";
                return false;
            }

            IWICImagingFactory* factory = wicLease.Get();
            IWICStream* wicStream = nullptr;
            IWICBitmapDecoder* decoder = nullptr;

            HRESULT hr = factory->CreateStream(&wicStream);
            if (FAILED(hr) || wicStream == nullptr)
            {
                error = "Could not create WIC input stream (" + FormatHresult(hr) + ").";
                return false;
            }

            hr = wicStream->InitializeFromMemory(
                const_cast<BYTE*>(encodedBytes.data()),
                static_cast<DWORD>(encodedBytes.size()));
            if (FAILED(hr))
            {
                error = "Could not initialize WIC memory stream (" + FormatHresult(hr) + ").";
                wicStream->Release();
                return false;
            }

            hr = factory->CreateDecoderFromStream(
                wicStream,
                nullptr,
                WICDecodeMetadataCacheOnLoad,
                &decoder);
            if (FAILED(hr) || decoder == nullptr)
            {
                error = "Could not decode in-memory image (" + FormatHresult(hr) + ").";
                if (decoder) decoder->Release();
                wicStream->Release();
                return false;
            }

            const bool ok = DecodeWicDecoderToBgra(factory, decoder, image, error);
            decoder->Release();
            wicStream->Release();
            return ok;
        }

        bool WriteTga(const std::filesystem::path& outputPath, const RasterImage& image, std::string& error)
        {
            std::ofstream out(outputPath, std::ios::binary);
            if (!out)
            {
                error = "Could not open output file for TGA writing.";
                return false;
            }

            uint8_t header[18] = {};
            header[2] = image.format == RasterPixelFormat::Gray8 ? 3 : 2;
            header[12] = static_cast<uint8_t>(image.width & 0xFF);
            header[13] = static_cast<uint8_t>((image.width >> 8) & 0xFF);
            header[14] = static_cast<uint8_t>(image.height & 0xFF);
            header[15] = static_cast<uint8_t>((image.height >> 8) & 0xFF);
            header[16] = image.format == RasterPixelFormat::Gray8 ? 8 : 32;
            header[17] = image.format == RasterPixelFormat::Gray8 ? 0x20 : 0x28;

            out.write(reinterpret_cast<const char*>(header), sizeof(header));
            out.write(reinterpret_cast<const char*>(image.pixels.data()), static_cast<std::streamsize>(image.pixels.size()));
            if (!out)
            {
                error = "Failed while writing TGA output.";
                return false;
            }

            return true;
        }

        bool WriteReadableImage(
            const std::filesystem::path& outputPath,
            const RasterImage& image,
            std::string_view normalizedFormat,
            std::string& error)
        {
            if (normalizedFormat == "PNG")
            {
                return WritePng(outputPath, image, error);
            }

            if (normalizedFormat == "TGA")
            {
                return WriteTga(outputPath, image, error);
            }

            error = "Unsupported readable image export format: " + std::string(normalizedFormat);
            return false;
        }

        bool NormalizeRasterToTightBgra(RasterImage& image, std::string& error);

        uint8_t DecodeModernWarThunderRoughness(uint8_t invertedRoughness)
        {
            static const std::array<uint8_t, 256> kRoughnessLut = []() {
                std::array<uint8_t, 256> lut = {};
                for (int value = 0; value < 256; ++value)
                {
                    const double encoded = static_cast<double>(value) / 255.0;
                    // War Thunder stores 1 - (1 - 0.7 * smoothness)^3; recover 1 - smoothness.
                    const double roughness =
                        (std::cbrt(std::max(0.0, 1.0 - encoded)) - 0.3) / 0.7;
                    lut[static_cast<size_t>(value)] = static_cast<uint8_t>(std::clamp(
                        std::round(std::clamp(roughness, 0.0, 1.0) * 255.0),
                        0.0,
                        255.0));
                }
                return lut;
            }();
            return kRoughnessLut[invertedRoughness];
        }

        bool BuildTextureMapImage(
            const RasterImage& source,
            TextureContentLayout layout,
            TextureMapKind mapKind,
            RasterImage& output,
            std::string& error)
        {
            if (source.format != RasterPixelFormat::Bgra8 ||
                source.stride < source.width * 4 ||
                source.pixels.size() < static_cast<size_t>(source.stride) * source.height)
            {
                error = "Decoded texture is not a valid BGRA8 image.";
                return false;
            }

            const auto supportsMap = [&]() {
                switch (layout)
                {
                case TextureContentLayout::BaseColor:
                    return mapKind == TextureMapKind::BaseColor;
                case TextureContentLayout::ConventionalNormal:
                    return mapKind == TextureMapKind::Normal;
                case TextureContentLayout::WarThunderPackedNormalPbr:
                    return mapKind == TextureMapKind::Normal ||
                        mapKind == TextureMapKind::InvertedRoughness ||
                        mapKind == TextureMapKind::Roughness ||
                        mapKind == TextureMapKind::Metallic ||
                        mapKind == TextureMapKind::MetallicRoughness ||
                        mapKind == TextureMapKind::OcclusionRoughnessMetallic;
                case TextureContentLayout::AmbientOcclusion:
                    return mapKind == TextureMapKind::AmbientOcclusion;
                case TextureContentLayout::PackedOrm:
                case TextureContentLayout::PackedRma:
                case TextureContentLayout::PackedMra:
                    return mapKind == TextureMapKind::AmbientOcclusion ||
                        mapKind == TextureMapKind::Roughness ||
                        mapKind == TextureMapKind::Metallic ||
                        mapKind == TextureMapKind::OcclusionRoughnessMetallic;
                case TextureContentLayout::Unknown:
                    return false;
                }
                return false;
            };

            if (!supportsMap())
            {
                error = "The requested " + std::string(TextureMapKindName(mapKind)) +
                    " map is not present in the detected texture layout.";
                return false;
            }

            const bool grayscale =
                mapKind == TextureMapKind::AmbientOcclusion ||
                mapKind == TextureMapKind::InvertedRoughness ||
                mapKind == TextureMapKind::Roughness ||
                mapKind == TextureMapKind::Metallic;

            output.width = source.width;
            output.height = source.height;
            output.format = grayscale ? RasterPixelFormat::Gray8 : RasterPixelFormat::Bgra8;
            output.stride = grayscale ? source.width : source.width * 4;
            output.pixels.resize(static_cast<size_t>(output.stride) * output.height);

            auto processRow = [&](uint32_t y) {
                const uint8_t* srcRow =
                    source.pixels.data() + static_cast<size_t>(y) * source.stride;
                uint8_t* dstRow =
                    output.pixels.data() + static_cast<size_t>(y) * output.stride;

                for (uint32_t x = 0; x < source.width; ++x)
                {
                    const size_t srcOffset = static_cast<size_t>(x) * 4;
                    const uint8_t sourceB = srcRow[srcOffset + 0];
                    const uint8_t sourceG = srcRow[srcOffset + 1];
                    const uint8_t sourceR = srcRow[srcOffset + 2];
                    const uint8_t sourceA = srcRow[srcOffset + 3];

                    uint8_t ao = 255;
                    uint8_t roughness = 255;
                    uint8_t metallic = 0;
                    bool hasAo = false;
                    bool hasRoughness = false;
                    bool hasMetallic = false;

                    switch (layout)
                    {
                    case TextureContentLayout::WarThunderPackedNormalPbr:
                        roughness = DecodeModernWarThunderRoughness(sourceR);
                        metallic = sourceB;
                        hasRoughness = true;
                        hasMetallic = true;
                        break;
                    case TextureContentLayout::AmbientOcclusion:
                        ao = sourceR;
                        hasAo = true;
                        break;
                    case TextureContentLayout::PackedOrm:
                        ao = sourceR;
                        roughness = sourceG;
                        metallic = sourceB;
                        hasAo = hasRoughness = hasMetallic = true;
                        break;
                    case TextureContentLayout::PackedRma:
                        roughness = sourceR;
                        metallic = sourceG;
                        ao = sourceB;
                        hasAo = hasRoughness = hasMetallic = true;
                        break;
                    case TextureContentLayout::PackedMra:
                        metallic = sourceR;
                        roughness = sourceG;
                        ao = sourceB;
                        hasAo = hasRoughness = hasMetallic = true;
                        break;
                    default:
                        break;
                    }

                    if (grayscale)
                    {
                        uint8_t value = 0;
                        switch (mapKind)
                        {
                        case TextureMapKind::AmbientOcclusion:
                            value = hasAo ? ao : 255;
                            break;
                        case TextureMapKind::InvertedRoughness:
                            value = sourceR;
                            break;
                        case TextureMapKind::Roughness:
                            value = hasRoughness ? roughness : 255;
                            break;
                        case TextureMapKind::Metallic:
                            value = hasMetallic ? metallic : 0;
                            break;
                        default:
                            break;
                        }
                        dstRow[x] = value;
                        continue;
                    }

                    const size_t dstOffset = static_cast<size_t>(x) * 4;
                    if (mapKind == TextureMapKind::BaseColor)
                    {
                        dstRow[dstOffset + 0] = sourceB;
                        dstRow[dstOffset + 1] = sourceG;
                        dstRow[dstOffset + 2] = sourceR;
                        dstRow[dstOffset + 3] = sourceA;
                    }
                    else if (mapKind == TextureMapKind::Normal)
                    {
                        if (layout == TextureContentLayout::WarThunderPackedNormalPbr)
                        {
                            const float normalX =
                                (static_cast<float>(sourceG) / 255.0f) * 2.0f - 1.0f;
                            const float normalY =
                                (static_cast<float>(sourceA) / 255.0f) * 2.0f - 1.0f;
                            const float normalZ = std::sqrt(std::max(
                                0.0f,
                                1.0f - normalX * normalX - normalY * normalY));
                            dstRow[dstOffset + 0] = FloatToUnormByte(normalZ * 0.5f + 0.5f);
                            dstRow[dstOffset + 1] = sourceA;
                            dstRow[dstOffset + 2] = sourceG;
                            dstRow[dstOffset + 3] = 255;
                        }
                        else
                        {
                            dstRow[dstOffset + 0] = sourceB;
                            dstRow[dstOffset + 1] = sourceG;
                            dstRow[dstOffset + 2] = sourceR;
                            dstRow[dstOffset + 3] = 255;
                        }
                    }
                    else
                    {
                        dstRow[dstOffset + 0] = metallic;
                        dstRow[dstOffset + 1] = roughness;
                        dstRow[dstOffset + 2] =
                            mapKind == TextureMapKind::OcclusionRoughnessMetallic
                                ? ao
                                : 255;
                        dstRow[dstOffset + 3] = 255;
                    }
                }
            };

            const uint64_t pixelCount = static_cast<uint64_t>(source.width) * static_cast<uint64_t>(source.height);
            if (pixelCount >= ParallelPixelThreshold && source.height >= 128)
            {
                Jobs::JobSystem::RunParallelFor(source.height, 64, [&](size_t rowIndex) {
                    processRow(static_cast<uint32_t>(rowIndex));
                });
            }
            else
            {
                for (uint32_t y = 0; y < source.height; ++y)
                {
                    processRow(y);
                }
            }

            return true;
        }

        bool GenerateMaterialMapsIfNeeded(
            RasterImage& image,
            std::string_view entryName,
            std::filesystem::path& outputPath,
            std::string_view normalizedFormat,
            const TextureExportOptions& options,
            TextureExportResult& result,
            std::string& error)
        {
            if (!options.generateMaterialMaps)
            {
                return true;
            }

            const TextureMapAnalysis analysis = AnalyzeTextureMaps(entryName);
            if (analysis.layout == TextureContentLayout::Unknown)
            {
                return true;
            }

            if (analysis.layout == TextureContentLayout::WarThunderPackedNormalPbr &&
                !options.convertWarThunderPackedMaps)
            {
                result.nativeWarThunderPackedMap = true;
                result.warnings.push_back(
                    "Preserved the native War Thunder _n channels without generating normal or ORM. "
                    "Core glTF material slots cannot interpret this packed layout directly.");
                return true;
            }

            if (!NormalizeRasterToTightBgra(image, error))
            {
                return false;
            }

            TextureMapKind mainKind = TextureMapKind::BaseColor;
            if (!TryGetPrimaryMaterialMapKind(analysis.layout, mainKind))
            {
                return true;
            }

            const bool memoryOnlyPng = options.encodePngInMemoryOnly && normalizedFormat == "PNG";

            RasterImage source = image;
            if (!BuildTextureMapImage(source, analysis.layout, mainKind, image, error))
            {
                return false;
            }
            outputPath = BuildMaterialMapOutputPath(outputPath, entryName, mainKind);
            if (!memoryOnlyPng && !options.overwrite && std::filesystem::exists(outputPath))
            {
                error = "Output file already exists: " + Platform::PathToUtf8(outputPath);
                return false;
            }
            result.mapOutputs.push_back({ mainKind, outputPath, {} });

            if (analysis.layout == TextureContentLayout::WarThunderPackedNormalPbr)
            {
                RasterImage ormImage;
                if (!BuildTextureMapImage(
                        source,
                        analysis.layout,
                        TextureMapKind::OcclusionRoughnessMetallic,
                        ormImage,
                        error))
                {
                    return false;
                }

                const std::filesystem::path ormOutputPath = BuildMaterialMapOutputPath(
                    outputPath,
                    entryName,
                    TextureMapKind::OcclusionRoughnessMetallic);
                if (memoryOnlyPng)
                {
                    std::vector<uint8_t> ormBytes;
                    if (!EncodeRasterToPngMemory(ormImage, ormBytes, error))
                    {
                        error = "Failed to encode in-memory ORM image: " + error;
                        return false;
                    }
                    result.mapOutputs.push_back({
                        TextureMapKind::OcclusionRoughnessMetallic,
                        ormOutputPath,
                        std::move(ormBytes),
                    });
                }
                else
                {
                    if (!options.overwrite && std::filesystem::exists(ormOutputPath))
                    {
                        error = "Output file already exists: " + Platform::PathToUtf8(ormOutputPath);
                        return false;
                    }

                    if (!WriteReadableImage(
                            ormOutputPath,
                            ormImage,
                            normalizedFormat,
                            error))
                    {
                        std::error_code cleanupError;
                        std::filesystem::remove(ormOutputPath, cleanupError);
                        error = "Failed to write generated ORM image: " + error;
                        return false;
                    }

                    result.mapOutputs.push_back({
                        TextureMapKind::OcclusionRoughnessMetallic,
                        ormOutputPath,
                        {},
                    });
                }

                result.warnings.push_back(
                    "Generated a standard normal map and ORM from modern War Thunder _n channels. "
                    "Normal X/Y use green/alpha, roughness decodes the non-linear red channel, "
                    "metallic uses blue, and ORM AO defaults to white.");
            }

            return true;
        }

        bool DecodeRasterWithDirectXTex(
            std::span<const uint8_t> ddsData,
            RasterImage& image,
            std::string& error)
        {
            DirectX::TexMetadata metadata = {};
            DirectX::ScratchImage loadedImage;
            HRESULT hr = DirectX::LoadFromDDSMemory(
                reinterpret_cast<const std::byte*>(ddsData.data()),
                ddsData.size(),
                DirectX::DDS_FLAGS_NONE,
                &metadata,
                loadedImage);
            if (FAILED(hr))
            {
                error = "DirectXTex could not load the reconstructed DDS payload (" + FormatHresult(hr) + ").";
                return false;
            }

            const DirectX::Image* workingImages = loadedImage.GetImages();
            size_t workingImageCount = loadedImage.GetImageCount();
            const DirectX::TexMetadata* workingMetadata = &metadata;
            DirectX::ScratchImage decompressedImage;

            if (DirectX::IsCompressed(workingMetadata->format))
            {
                hr = DirectX::Decompress(
                    workingImages,
                    workingImageCount,
                    *workingMetadata,
                    DXGI_FORMAT_UNKNOWN,
                    decompressedImage);
                if (FAILED(hr))
                {
                    error = "DirectXTex could not decompress the DDS payload (" + FormatHresult(hr) + ").";
                    return false;
                }

                workingImages = decompressedImage.GetImages();
                workingImageCount = decompressedImage.GetImageCount();
                workingMetadata = &decompressedImage.GetMetadata();
            }

            DirectX::ScratchImage convertedImage;
            const DXGI_FORMAT targetFormat = DirectX::IsSRGB(workingMetadata->format)
                ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
                : DXGI_FORMAT_R8G8B8A8_UNORM;

            if (workingMetadata->format != targetFormat)
            {
                hr = DirectX::Convert(
                    workingImages,
                    workingImageCount,
                    *workingMetadata,
                    targetFormat,
                    DirectX::TEX_FILTER_DEFAULT,
                    DirectX::TEX_THRESHOLD_DEFAULT,
                    convertedImage);
                if (FAILED(hr))
                {
                    error = "DirectXTex could not convert the image to RGBA8 (" + FormatHresult(hr) + ").";
                    return false;
                }

                workingImages = convertedImage.GetImages();
                workingImageCount = convertedImage.GetImageCount();
                workingMetadata = &convertedImage.GetMetadata();
            }

            const DirectX::Image* topImage = workingImages;
            if (topImage == nullptr || workingImageCount == 0)
            {
                error = "DirectXTex returned no image data for readable export.";
                return false;
            }

            if (topImage->pixels == nullptr || topImage->width == 0 || topImage->height == 0)
            {
                error = "DirectXTex returned an empty image for readable export.";
                return false;
            }

            const bool isRgba8 =
                topImage->format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                topImage->format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
            const bool isBgra8 =
                topImage->format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                topImage->format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            if (!isRgba8 && !isBgra8)
            {
                error = "DirectXTex conversion did not produce an RGBA8/BGRA8 image.";
                return false;
            }

            const uint64_t expectedStride = static_cast<uint64_t>(topImage->width) * 4ull;
            if (topImage->rowPitch < expectedStride ||
                expectedStride > std::numeric_limits<uint32_t>::max())
            {
                error = "DirectXTex returned an unexpected row pitch for readable export.";
                return false;
            }

            const uint64_t totalBytes = expectedStride * static_cast<uint64_t>(topImage->height);
            if (totalBytes > std::numeric_limits<size_t>::max())
            {
                error = "DirectXTex readable export image is too large.";
                return false;
            }

            image.width = static_cast<uint32_t>(topImage->width);
            image.height = static_cast<uint32_t>(topImage->height);
            image.stride = static_cast<uint32_t>(expectedStride);
            image.format = RasterPixelFormat::Bgra8;
            image.pixels.resize(static_cast<size_t>(totalBytes));

            auto convertRow = [&](size_t row) {
                const uint8_t* src = topImage->pixels + row * topImage->rowPitch;
                uint8_t* dst = image.pixels.data() + row * image.stride;
                if (isBgra8)
                {
                    std::memcpy(dst, src, image.stride);
                    return;
                }

                for (size_t x = 0; x < topImage->width; ++x)
                {
                    const size_t srcOffset = x * 4;
                    const size_t dstOffset = x * 4;
                    dst[dstOffset + 0] = src[srcOffset + 2];
                    dst[dstOffset + 1] = src[srcOffset + 1];
                    dst[dstOffset + 2] = src[srcOffset + 0];
                    dst[dstOffset + 3] = src[srcOffset + 3];
                }
            };

            const uint64_t pixelCount = static_cast<uint64_t>(image.width) * static_cast<uint64_t>(image.height);
            if (pixelCount >= ParallelPixelThreshold && topImage->height >= 128)
            {
                Jobs::JobSystem::RunParallelFor(topImage->height, 64, [&](size_t row) {
                    convertRow(row);
                });
            }
            else
            {
                for (size_t row = 0; row < topImage->height; ++row)
                {
                    convertRow(row);
                }
            }

            return true;
        }

        bool NormalizeRasterToTightBgra(RasterImage& image, std::string& error)
        {
            if (image.width == 0 || image.height == 0)
            {
                error = "Decoded preview has invalid dimensions.";
                return false;
            }

            const uint64_t tightStride64 = static_cast<uint64_t>(image.width) * 4ull;
            const uint64_t totalBytes64 = tightStride64 * static_cast<uint64_t>(image.height);
            if (tightStride64 > std::numeric_limits<uint32_t>::max() ||
                totalBytes64 > std::numeric_limits<size_t>::max())
            {
                error = "Decoded preview is too large.";
                return false;
            }

            const uint32_t tightStride = static_cast<uint32_t>(tightStride64);
            std::vector<uint8_t> tightPixels(static_cast<size_t>(totalBytes64));

            if (image.format == RasterPixelFormat::Gray8)
            {
                if (image.stride < image.width ||
                    image.pixels.size() < static_cast<size_t>(image.stride) * image.height)
                {
                    error = "Decoded grayscale preview has an invalid row pitch.";
                    return false;
                }

                for (uint32_t y = 0; y < image.height; ++y)
                {
                    const uint8_t* src = image.pixels.data() + static_cast<size_t>(y) * image.stride;
                    uint8_t* dst = tightPixels.data() + static_cast<size_t>(y) * tightStride;
                    for (uint32_t x = 0; x < image.width; ++x)
                    {
                        const uint8_t value = src[x];
                        const size_t dstOffset = static_cast<size_t>(x) * 4;
                        dst[dstOffset + 0] = value;
                        dst[dstOffset + 1] = value;
                        dst[dstOffset + 2] = value;
                        dst[dstOffset + 3] = 255;
                    }
                }
            }
            else
            {
                if (image.stride < tightStride ||
                    image.pixels.size() < static_cast<size_t>(image.stride) * image.height)
                {
                    error = "Decoded BGRA preview has an invalid row pitch.";
                    return false;
                }

                for (uint32_t y = 0; y < image.height; ++y)
                {
                    const uint8_t* src = image.pixels.data() + static_cast<size_t>(y) * image.stride;
                    uint8_t* dst = tightPixels.data() + static_cast<size_t>(y) * tightStride;
                    std::memcpy(dst, src, tightStride);
                }
            }

            image.stride = tightStride;
            image.format = RasterPixelFormat::Bgra8;
            image.pixels = std::move(tightPixels);
            return true;
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
            manifest.skipWrite = !options.exportJsonManifest;
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

        // Legacy uncompressed formats - no DX10 needed
        case 21: return false; // D3DFMT_A8R8G8B8
        case 50: return false; // D3DFMT_L8

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
        case 540492610: return 98;  // BC7 FourCC -> DXGI_FORMAT_BC7_UNORM
        case 21:        return 87;  // D3DFMT_A8R8G8B8 -> DXGI_FORMAT_B8G8R8A8_UNORM
        case 50:        return 61;  // D3DFMT_L8 -> DXGI_FORMAT_R8_UNORM
        default:                    return d3dFormat; // Already a DXGI format code
        }
    }

    bool TextureExporter::TestNormalizeMipOrder(
        const DagorFormats::DdsxHeader& header,
        std::vector<uint8_t>& payload,
        std::string& warning,
        std::string& error)
    {
        return NormalizeMipOrderIfNeeded(header, payload, warning, error);
    }

    TextureExportResult TextureExporter::ExportTextureInternal(
        const std::filesystem::path& packPath,
        std::span<const uint8_t> mappedPackData,
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

        try
        {
            PreparedTexturePayload prepared;
            if (!PrepareTexturePayload(
                    packPath,
                    entry,
                    options.gameRoot,
                    options.oodleDllPath,
                    options.useOodleFromGameFolder,
                    prepared,
                    mappedPackData))
            {
                result.warnings = std::move(prepared.warnings);
                if (prepared.failure == TexturePreparationFailure::OodleUnavailable ||
                    prepared.failure == TexturePreparationFailure::OodleDecompressionFailed)
                {
                    if (!options.encodePngInMemoryOnly)
                    {
                        ExportRawDdsxFallback(
                            result,
                            packPath,
                            entry,
                            options,
                            prepared.header,
                            prepared.rawPackPayload,
                            prepared.error);
                    }
                    else
                    {
                        result.error = std::move(prepared.error);
                    }
                    return result;
                }

                result.error = std::move(prepared.error);
                return result;
            }

            result.warnings = std::move(prepared.warnings);
            const DagorFormats::DdsxHeader& exportHeader = prepared.header;
            const std::vector<uint8_t>& payload = prepared.payload;

            const std::string normalizedFormat = NormalizeTextureFormat(options.format);
            const bool memoryOnlyPng = options.encodePngInMemoryOnly && normalizedFormat == "PNG";

            auto prepareOutputPath = [&](std::string_view extension, std::filesystem::path& outputPath) -> bool
            {
                outputPath = BuildSafeOutputPath(options.outputDir, entry.name, extension);
                if (options.generateMaterialMaps)
                {
                    const TextureContentLayout layout =
                        AnalyzeTextureMaps(entry.name).layout;
                    TextureMapKind primaryMapKind = TextureMapKind::BaseColor;
                    if (TryGetPrimaryMaterialMapKind(
                            layout,
                            primaryMapKind) &&
                        (layout != TextureContentLayout::WarThunderPackedNormalPbr ||
                            options.convertWarThunderPackedMaps))
                    {
                        outputPath = BuildMaterialMapOutputPath(
                            outputPath,
                            entry.name,
                            primaryMapKind);
                    }
                }

                if (memoryOnlyPng)
                {
                    return true;
                }

                std::error_code ec;
                std::filesystem::create_directories(outputPath.parent_path(), ec);
                if (ec)
                {
                    result.error = "Could not create output directory: " + ec.message();
                    return false;
                }

                if (!options.overwrite && std::filesystem::exists(outputPath))
                {
                    result.error = "Output file already exists: " + Platform::PathToUtf8(outputPath);
                    return false;
                }

                return true;
            };

            if (normalizedFormat != "DDS")
            {
                std::filesystem::path outputPath;
                if (!prepareOutputPath(TextureFormatExtension(normalizedFormat), outputPath))
                {
                    return result;
                }

                auto cleanupGeneratedMapFiles = [&]() {
                    if (!memoryOnlyPng)
                    {
                        std::error_code cleanupError;
                        for (const TextureMapOutput& mapOutput : result.mapOutputs)
                        {
                            if (mapOutput.outputPath != outputPath)
                            {
                                std::filesystem::remove(mapOutput.outputPath, cleanupError);
                                cleanupError.clear();
                            }
                        }
                    }
                    result.mapOutputs.clear();
                };

                auto emitReadableOutput = [&](const RasterImage& image, std::string& writeError) -> bool {
                    if (memoryOnlyPng)
                    {
                        if (!EncodeRasterToPngMemory(image, result.encodedBytes, writeError))
                        {
                            return false;
                        }
                        if (!result.mapOutputs.empty() && result.mapOutputs.front().outputPath == outputPath)
                        {
                            result.mapOutputs.front().encodedBytes = result.encodedBytes;
                        }
                        return true;
                    }

                    return WriteReadableImage(outputPath, image, normalizedFormat, writeError);
                };

                auto finalizeReadableSuccess = [&]() {
                    result.success = true;
                    result.outputPath = outputPath;

                    if (exportHeader.mipCount > 1)
                    {
                        result.warnings.push_back(std::string(normalizedFormat) + " export writes only the top mip level.");
                    }

                    if (!memoryOnlyPng && options.exportJsonManifest)
                    {
                        ExportManifest manifest;
                        manifest.sourcePack = Platform::PathToUtf8(packPath.filename());
                        manifest.sourceResource = entry.name;
                        manifest.assetType = "Texture (" + normalizedFormat + ")";
                        manifest.outputFiles.push_back(Platform::PathToUtf8(outputPath.filename()));
                        for (const TextureMapOutput& mapOutput : result.mapOutputs)
                        {
                            if (mapOutput.outputPath != outputPath)
                            {
                                manifest.outputFiles.push_back(
                                    Platform::PathToUtf8(mapOutput.outputPath.filename()));
                            }
                        }
                        manifest.appVersion = "0.1.0-alpha";
                        manifest.formatDecoderVersion = "DdsxReader v1.0";
                        manifest.skipWrite = false;

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
                };

                RasterImage image;
                std::string readableError;
                if (DecodeTopMipToRaster(exportHeader, payload, image, readableError) &&
                    GenerateMaterialMapsIfNeeded(image, entry.name, outputPath, normalizedFormat, options, result, readableError) &&
                    emitReadableOutput(image, readableError))
                {
                    finalizeReadableSuccess();
                    return result;
                }

                cleanupGeneratedMapFiles();
                std::vector<uint8_t> ddsData;
                std::string directXTexError;
                RasterImage directXTexImage;
                if (BuildDdsBuffer(exportHeader, payload, ddsData, directXTexError) &&
                    DecodeRasterWithDirectXTex(ddsData, directXTexImage, directXTexError) &&
                    GenerateMaterialMapsIfNeeded(directXTexImage, entry.name, outputPath, normalizedFormat, options, result, directXTexError) &&
                    emitReadableOutput(directXTexImage, directXTexError))
                {
                    result.warnings.push_back("Readable export transcoded via DirectXTex.");
                    finalizeReadableSuccess();
                    return result;
                }

                if (!directXTexError.empty())
                {
                    readableError = readableError.empty()
                        ? directXTexError
                        : (readableError + " DirectXTex fallback also failed: " + directXTexError);
                }

                if (!options.preserveOriginalDDS || memoryOnlyPng)
                {
                    cleanupGeneratedMapFiles();
                    result.error = readableError;
                    return result;
                }

                cleanupGeneratedMapFiles();
                result.warnings.push_back(
                    "Requested " + normalizedFormat + " export is unavailable for this DDSx texture. " +
                    readableError + " Falling back to DDS.");
            }

            std::filesystem::path outputPath;
            if (!prepareOutputPath(".dds", outputPath))
            {
                return result;
            }

            if (WriteDds(outputPath, exportHeader, payload, result.error))
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
                manifest.skipWrite = !options.exportJsonManifest;

                if (NeedsDX10Header(exportHeader.d3dFormat))
                {
                    manifest.decodeWarnings.push_back(
                        "DX10 extended header used for DXGI format " + std::to_string(exportHeader.d3dFormat));
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

    TexturePreviewResult TextureExporter::DecodePreviewInternal(
        const std::filesystem::path& packPath,
        std::span<const uint8_t> mappedPackData,
        const AssetIndex::SubAssetRecord& entry,
        const TexturePreviewOptions& options)
    {
        TexturePreviewResult result;

        try
        {
            PreparedTexturePayload prepared;
            if (!PrepareTexturePayload(
                    packPath,
                    entry,
                    options.gameRoot,
                    options.oodleDllPath,
                    options.useOodleFromGameFolder,
                    prepared,
                    mappedPackData))
            {
                result.error = std::move(prepared.error);
                result.warnings = std::move(prepared.warnings);
                return result;
            }

            result.warnings = std::move(prepared.warnings);
            result.srgb = IsSrgbTextureFormat(prepared.header.d3dFormat);

            RasterImage image;
            std::string builtInError;
            if (DecodeTopMipToRaster(prepared.header, prepared.payload, image, builtInError))
            {
                std::string normalizeError;
                if (NormalizeRasterToTightBgra(image, normalizeError))
                {
                    result.success = true;
                    result.width = image.width;
                    result.height = image.height;
                    result.stride = image.stride;
                    result.bgraPixels = std::move(image.pixels);
                    return result;
                }
                builtInError = std::move(normalizeError);
            }

            std::vector<uint8_t> ddsData;
            RasterImage directXTexImage;
            std::string directXTexError;
            if (BuildDdsBuffer(
                    prepared.header,
                    prepared.payload,
                    ddsData,
                    directXTexError) &&
                DecodeRasterWithDirectXTex(ddsData, directXTexImage, directXTexError) &&
                NormalizeRasterToTightBgra(directXTexImage, directXTexError))
            {
                result.success = true;
                result.width = directXTexImage.width;
                result.height = directXTexImage.height;
                result.stride = directXTexImage.stride;
                result.bgraPixels = std::move(directXTexImage.pixels);
                result.warnings.push_back("Texture preview transcoded via DirectXTex.");
                return result;
            }

            result.error = builtInError;
            if (!directXTexError.empty())
            {
                result.error = result.error.empty()
                    ? directXTexError
                    : result.error + " DirectXTex fallback also failed: " + directXTexError;
            }
            if (result.error.empty())
            {
                result.error = "Texture preview decode failed without a diagnostic.";
            }
        }
        catch (const std::exception& ex)
        {
            result.error = std::string("DDSx Parse Error: ") + ex.what();
        }

        return result;
    }

    TextureExportResult TextureExporter::ExportTexture(
        const std::filesystem::path& packPath,
        const AssetIndex::SubAssetRecord& entry,
        const TextureExportOptions& options)
    {
        return ExportTextureInternal(packPath, {}, entry, options);
    }

    TextureExportResult TextureExporter::ExportTextureFromMemory(
        const std::filesystem::path& packPath,
        std::span<const uint8_t> packData,
        const AssetIndex::SubAssetRecord& entry,
        const TextureExportOptions& options)
    {
        return ExportTextureInternal(packPath, packData, entry, options);
    }

    TexturePreviewResult TextureExporter::DecodePreview(
        const std::filesystem::path& packPath,
        const AssetIndex::SubAssetRecord& entry,
        const TexturePreviewOptions& options)
    {
        return DecodePreviewInternal(packPath, {}, entry, options);
    }

    TexturePreviewResult TextureExporter::DecodePreviewFromMemory(
        const std::filesystem::path& packPath,
        std::span<const uint8_t> packData,
        const AssetIndex::SubAssetRecord& entry,
        const TexturePreviewOptions& options)
    {
        return DecodePreviewInternal(packPath, packData, entry, options);
    }

    bool TextureExporter::EncodeBgraToPngMemory(
        const uint8_t* pixels,
        uint32_t width,
        uint32_t height,
        uint32_t stride,
        std::vector<uint8_t>& outPngBytes,
        std::string& error,
        bool isGrayscale)
    {
        outPngBytes.clear();
        const uint32_t minStride = isGrayscale ? width : width * 4u;
        if (pixels == nullptr || width == 0 || height == 0 || stride < minStride)
        {
            error = "Invalid raster image parameters for in-memory PNG encoding.";
            return false;
        }

        const uint64_t requiredBytes = static_cast<uint64_t>(stride) * static_cast<uint64_t>(height);
        if (requiredBytes > std::numeric_limits<UINT>::max() ||
            requiredBytes > std::numeric_limits<size_t>::max())
        {
            error = "Image dimensions exceed PNG buffer limits.";
            return false;
        }

        RasterImage image;
        image.width = width;
        image.height = height;
        image.stride = stride;
        image.format = isGrayscale ? RasterPixelFormat::Gray8 : RasterPixelFormat::Bgra8;
        image.pixels.assign(pixels, pixels + static_cast<size_t>(requiredBytes));
        return EncodeRasterToPngMemory(image, outPngBytes, error);
    }

    std::vector<TextureMapPreviewImage> TextureExporter::BuildMapPreviews(
        std::string_view textureName,
        const uint8_t* bgraPixels,
        uint32_t width,
        uint32_t height,
        uint32_t stride,
        uint32_t maxDimension)
    {
        std::vector<TextureMapPreviewImage> previews;
        const TextureMapAnalysis analysis = AnalyzeTextureMaps(textureName);
        if (analysis.maps.empty() || bgraPixels == nullptr ||
            width == 0 || height == 0 || stride < width * 4u ||
            maxDimension == 0)
        {
            return previews;
        }

        const double scale = std::min(
            1.0,
            static_cast<double>(maxDimension) /
                static_cast<double>(std::max(width, height)));
        const uint32_t thumbnailWidth = std::max(
            1u,
            static_cast<uint32_t>(std::round(static_cast<double>(width) * scale)));
        const uint32_t thumbnailHeight = std::max(
            1u,
            static_cast<uint32_t>(std::round(static_cast<double>(height) * scale)));

        RasterImage thumbnailSource;
        thumbnailSource.width = thumbnailWidth;
        thumbnailSource.height = thumbnailHeight;
        thumbnailSource.stride = thumbnailWidth * 4u;
        thumbnailSource.format = RasterPixelFormat::Bgra8;
        thumbnailSource.pixels.resize(
            static_cast<size_t>(thumbnailSource.stride) * thumbnailHeight);

        for (uint32_t y = 0; y < thumbnailHeight; ++y)
        {
            const uint32_t sourceY = std::min(
                height - 1,
                static_cast<uint32_t>(
                    (static_cast<uint64_t>(y) * height) / thumbnailHeight));
            const uint8_t* sourceRow =
                bgraPixels + static_cast<size_t>(sourceY) * stride;
            uint8_t* destinationRow =
                thumbnailSource.pixels.data() +
                static_cast<size_t>(y) * thumbnailSource.stride;

            for (uint32_t x = 0; x < thumbnailWidth; ++x)
            {
                const uint32_t sourceX = std::min(
                    width - 1,
                    static_cast<uint32_t>(
                        (static_cast<uint64_t>(x) * width) / thumbnailWidth));
                std::memcpy(
                    destinationRow + static_cast<size_t>(x) * 4u,
                    sourceRow + static_cast<size_t>(sourceX) * 4u,
                    4u);
            }
        }

        previews.reserve(analysis.maps.size());
        for (const DetectedTextureMap& detectedMap : analysis.maps)
        {
            RasterImage mapImage;
            std::string error;
            if (!BuildTextureMapImage(
                    thumbnailSource,
                    analysis.layout,
                    detectedMap.kind,
                    mapImage,
                    error))
            {
                continue;
            }

            if (!NormalizeRasterToTightBgra(mapImage, error))
            {
                continue;
            }

            TextureMapPreviewImage preview;
            preview.kind = detectedMap.kind;
            preview.width = mapImage.width;
            preview.height = mapImage.height;
            preview.stride = mapImage.stride;
            preview.bgraPixels = std::move(mapImage.pixels);
            previews.push_back(std::move(preview));
        }

        return previews;
    }

    TextureExportResult TextureExporter::ExportTextureMap(
        const std::filesystem::path& packPath,
        const AssetIndex::SubAssetRecord& entry,
        TextureMapKind mapKind,
        const TextureMapExportOptions& options)
    {
        std::vector<TextureExportResult> results =
            ExportTextureMaps(packPath, entry, { mapKind }, options);
        if (results.empty())
        {
            TextureExportResult result;
            result.error = "The map exporter returned no result.";
            return result;
        }
        return std::move(results.front());
    }

    std::vector<TextureExportResult> TextureExporter::ExportTextureMaps(
        const std::filesystem::path& packPath,
        const AssetIndex::SubAssetRecord& entry,
        const std::vector<TextureMapKind>& mapKinds,
        const TextureMapExportOptions& options)
    {
        std::vector<TextureExportResult> results(mapKinds.size());
        if (mapKinds.empty())
        {
            return results;
        }

        auto failAll = [&](std::string error) {
            for (TextureExportResult& result : results)
            {
                result.error = error;
            }
        };

        if (options.outputDir.empty())
        {
            failAll("Export output directory is not configured.");
            return results;
        }

        const TextureMapAnalysis analysis = AnalyzeTextureMaps(entry.name);
        if (analysis.layout == TextureContentLayout::Unknown)
        {
            failAll(analysis.caution);
            return results;
        }

        const std::string normalizedFormat = NormalizeTextureFormat(options.format);
        if (normalizedFormat == "DDS")
        {
            failAll("Individual material maps must be exported as PNG or TGA.");
            return results;
        }

        TexturePreviewOptions previewOptions;
        previewOptions.gameRoot = options.gameRoot;
        previewOptions.oodleDllPath = options.oodleDllPath;
        previewOptions.useOodleFromGameFolder = options.useOodleFromGameFolder;
        TexturePreviewResult preview = DecodePreview(packPath, entry, previewOptions);
        if (!preview.success)
        {
            failAll(std::move(preview.error));
            return results;
        }

        RasterImage source;
        source.width = preview.width;
        source.height = preview.height;
        source.stride = preview.stride;
        source.format = RasterPixelFormat::Bgra8;
        source.pixels = std::move(preview.bgraPixels);

        std::error_code ec;
        std::filesystem::create_directories(options.outputDir, ec);
        if (ec)
        {
            failAll("Could not create output directory: " + ec.message());
            return results;
        }

        for (size_t index = 0; index < mapKinds.size(); ++index)
        {
            TextureExportResult& result = results[index];
            result.warnings = preview.warnings;

            RasterImage mapImage;
            if (!BuildTextureMapImage(
                    source,
                    analysis.layout,
                    mapKinds[index],
                    mapImage,
                    result.error))
            {
                continue;
            }

            std::filesystem::path outputPath = BuildSafeOutputPath(
                options.outputDir,
                entry.name,
                TextureFormatExtension(normalizedFormat));
            outputPath = BuildMaterialMapOutputPath(
                outputPath,
                entry.name,
                mapKinds[index]);

            std::filesystem::create_directories(outputPath.parent_path(), ec);
            if (ec)
            {
                result.error = "Could not create output directory: " + ec.message();
                ec.clear();
                continue;
            }
            if (!options.overwrite && std::filesystem::exists(outputPath))
            {
                result.error =
                    "Output file already exists: " + Platform::PathToUtf8(outputPath);
                continue;
            }

            if (!WriteReadableImage(
                    outputPath,
                    mapImage,
                    normalizedFormat,
                    result.error))
            {
                continue;
            }

            result.success = true;
            result.outputPath = outputPath;
            result.mapOutputs.push_back({ mapKinds[index], outputPath });
            if (entry.texMips > 1)
            {
                result.warnings.push_back(
                    std::string(normalizedFormat) +
                    " material-map export writes only the top mip level.");
            }
        }

        return results;
    }

    namespace
    {
        bool ComposeOrmRaster(
            const RasterImage& metallicRoughness,
            const RasterImage& ambientOcclusion,
            RasterImage& orm,
            std::string& error,
            std::vector<std::string>* warnings)
        {
            if (metallicRoughness.width == 0 || metallicRoughness.height == 0 ||
                ambientOcclusion.width == 0 || ambientOcclusion.height == 0)
            {
                error = "ORM composition input has invalid dimensions.";
                return false;
            }

            if (ambientOcclusion.width != metallicRoughness.width ||
                ambientOcclusion.height != metallicRoughness.height)
            {
                if (warnings != nullptr)
                {
                    warnings->push_back(
                        "AO was resized from " +
                        std::to_string(ambientOcclusion.width) + "x" +
                        std::to_string(ambientOcclusion.height) + " to " +
                        std::to_string(metallicRoughness.width) + "x" +
                        std::to_string(metallicRoughness.height) + " for ORM composition.");
                }
            }

            orm = metallicRoughness;
            const auto sampleAo = [&](uint32_t outputX, uint32_t outputY) {
                const double sourceX =
                    (static_cast<double>(outputX) + 0.5) *
                        static_cast<double>(ambientOcclusion.width) /
                        static_cast<double>(orm.width) -
                    0.5;
                const double sourceY =
                    (static_cast<double>(outputY) + 0.5) *
                        static_cast<double>(ambientOcclusion.height) /
                        static_cast<double>(orm.height) -
                    0.5;

                const double clampedX = std::clamp(
                    sourceX,
                    0.0,
                    static_cast<double>(ambientOcclusion.width - 1));
                const double clampedY = std::clamp(
                    sourceY,
                    0.0,
                    static_cast<double>(ambientOcclusion.height - 1));
                const uint32_t x0 = static_cast<uint32_t>(std::floor(clampedX));
                const uint32_t y0 = static_cast<uint32_t>(std::floor(clampedY));
                const uint32_t x1 = std::min(x0 + 1, ambientOcclusion.width - 1);
                const uint32_t y1 = std::min(y0 + 1, ambientOcclusion.height - 1);
                const double tx = clampedX - static_cast<double>(x0);
                const double ty = clampedY - static_cast<double>(y0);

                const auto redAt = [&](uint32_t x, uint32_t y) {
                    const size_t offset =
                        static_cast<size_t>(y) * ambientOcclusion.stride +
                        static_cast<size_t>(x) * 4;
                    return static_cast<double>(ambientOcclusion.pixels[offset + 2]);
                };

                const double top = redAt(x0, y0) * (1.0 - tx) + redAt(x1, y0) * tx;
                const double bottom = redAt(x0, y1) * (1.0 - tx) + redAt(x1, y1) * tx;
                return static_cast<uint8_t>(std::clamp(
                    std::round(top * (1.0 - ty) + bottom * ty),
                    0.0,
                    255.0));
            };

            auto composeRow = [&](uint32_t y) {
                uint8_t* dstRow = orm.pixels.data() + static_cast<size_t>(y) * orm.stride;
                for (uint32_t x = 0; x < orm.width; ++x)
                {
                    const size_t offset = static_cast<size_t>(x) * 4;
                    dstRow[offset + 2] = sampleAo(x, y); // R: ambient occlusion
                    dstRow[offset + 3] = 255;
                }
            };

            const uint64_t pixelCount = static_cast<uint64_t>(orm.width) * static_cast<uint64_t>(orm.height);
            if (pixelCount >= ParallelPixelThreshold && orm.height >= 128)
            {
                Jobs::JobSystem::RunParallelFor(orm.height, 64, [&](size_t rowIndex) {
                    composeRow(static_cast<uint32_t>(rowIndex));
                });
            }
            else
            {
                for (uint32_t y = 0; y < orm.height; ++y)
                {
                    composeRow(y);
                }
            }

            return true;
        }
    }

    bool TextureExporter::ComposeOrmTexture(
        const std::filesystem::path& metallicRoughnessPath,
        const std::filesystem::path& ambientOcclusionPath,
        const std::filesystem::path& outputPath,
        std::string& error,
        std::vector<std::string>* warnings)
    {
        RasterImage metallicRoughness;
        RasterImage ambientOcclusion;
        if (!LoadWicBgra(metallicRoughnessPath, metallicRoughness, error) ||
            !LoadWicBgra(ambientOcclusionPath, ambientOcclusion, error))
        {
            return false;
        }

        RasterImage orm;
        if (!ComposeOrmRaster(metallicRoughness, ambientOcclusion, orm, error, warnings))
        {
            return false;
        }

        std::error_code ec;
        std::filesystem::create_directories(outputPath.parent_path(), ec);
        if (ec)
        {
            error = "Could not create ORM output directory: " + ec.message();
            return false;
        }

        const std::string extension = ToUpperAscii(outputPath.extension().string());
        const std::string format = extension == ".TGA" ? "TGA" : "PNG";
        return WriteReadableImage(outputPath, orm, format, error);
    }

    bool TextureExporter::ComposeOrmTextureFromMemory(
        std::span<const uint8_t> metallicRoughnessPng,
        std::span<const uint8_t> ambientOcclusionPng,
        std::vector<uint8_t>& outOrmPngBytes,
        std::string& error,
        std::vector<std::string>* warnings)
    {
        outOrmPngBytes.clear();
        RasterImage metallicRoughness;
        RasterImage ambientOcclusion;
        if (!LoadWicBgraFromMemory(metallicRoughnessPng, metallicRoughness, error) ||
            !LoadWicBgraFromMemory(ambientOcclusionPng, ambientOcclusion, error))
        {
            return false;
        }

        RasterImage orm;
        if (!ComposeOrmRaster(metallicRoughness, ambientOcclusion, orm, error, warnings))
        {
            return false;
        }

        return EncodeRasterToPngMemory(orm, outOrmPngBytes, error);
    }

    BatchTextureExportResult TextureExporter::ExportAllTextures(
        const std::filesystem::path& packPath,
        const AssetIndex::PackContents& contents,
        const TextureExportOptions& options,
        BatchProgressCallback progress)
    {
        BatchTextureExportResult batch;
        batch.total = contents.entries.size();

        if (contents.entries.empty())
        {
            return batch;
        }

        DagorIO::MappedFile mappedPack;
        std::string mapError;
        const bool hasMappedPack = mappedPack.Open(packPath, mapError) && mappedPack.Size() > 0;
        const std::span<const uint8_t> mappedPackBytes =
            hasMappedPack ? mappedPack.Span() : std::span<const uint8_t>{};

        const std::string normalizedFormat = NormalizeTextureFormat(options.format);
        std::mutex batchMutex;
        std::atomic_bool cancelRequested{ false };
        std::atomic_size_t progressIndex{ 0 };

        Jobs::JobSystem::RunBoundedWorkers(
            contents.entries.size(),
            [&](size_t index, size_t) {
                if (cancelRequested.load(std::memory_order_acquire))
                {
                    return;
                }

                const AssetIndex::SubAssetRecord& entry = contents.entries[index];
                const size_t current = progressIndex.fetch_add(1, std::memory_order_relaxed) + 1;

                if (progress && !progress(current, batch.total, entry.name))
                {
                    cancelRequested.store(true, std::memory_order_release);
                    return;
                }

                if (entry.size == 0)
                {
                    std::lock_guard<std::mutex> lock(batchMutex);
                    ++batch.skipped;
                    batch.warnings.push_back("Skipped empty entry: " + entry.name);
                    return;
                }

                if (!options.overwrite && normalizedFormat == "DDS")
                {
                    const std::filesystem::path outputPath = BuildSafeOutputPath(options.outputDir, entry.name, ".dds");
                    if (std::filesystem::exists(outputPath))
                    {
                        std::lock_guard<std::mutex> lock(batchMutex);
                        ++batch.skipped;
                        batch.warnings.push_back("Skipped existing file: " + entry.name);
                        return;
                    }
                }

                TextureExportResult result = hasMappedPack
                    ? ExportTextureFromMemory(packPath, mappedPackBytes, entry, options)
                    : ExportTexture(packPath, entry, options);

                std::lock_guard<std::mutex> lock(batchMutex);
                if (result.success)
                {
                    ++batch.succeeded;
                }
                else if (result.error.rfind("Output file already exists:", 0) == 0)
                {
                    ++batch.skipped;
                    batch.warnings.push_back("Skipped existing file: " + entry.name);
                }
                else
                {
                    ++batch.failed;
                    batch.errors.push_back(entry.name + ": " + result.error);
                }

                for (const std::string& warning : result.warnings)
                {
                    batch.warnings.push_back(entry.name + ": " + warning);
                }
            },
            1);

        if (cancelRequested.load(std::memory_order_acquire))
        {
            batch.warnings.push_back(
                "Batch export cancelled by user at " +
                std::to_string(std::min(progressIndex.load(std::memory_order_relaxed), batch.total)) +
                "/" + std::to_string(batch.total));
        }

        return batch;
    }

    bool TextureExporter::BuildDdsBuffer(
        const DagorFormats::DdsxHeader& header,
        const std::vector<uint8_t>& payload,
        std::vector<uint8_t>& ddsData,
        std::string& error)
    {
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

        const bool useDX10 = NeedsDX10Header(header.d3dFormat);
        if (useDX10)
        {
            dds.ddspf.flags = DDPF_FOURCC;
            dds.ddspf.fourCC = DX10_FOURCC;
        }
        else
        {
            uint32_t fourCC = 0;
            switch (header.d3dFormat)
            {
            case 827611204: fourCC = 0x31545844; break; // DXT1
            case 861165636: fourCC = 0x33545844; break; // DXT3
            case 894720068: fourCC = 0x35545844; break; // DXT5
            case 826889281: fourCC = 0x31495441; break; // ATI1
            case 843666497: fourCC = 0x32495441; break; // ATI2
            case 71: case 72: fourCC = 0x31545844; break; // BC1
            case 74: case 75: fourCC = 0x33545844; break; // BC2
            case 77: case 78: fourCC = 0x35545844; break; // BC3
            case 80: case 81: fourCC = 0x31495441; break; // BC4
            case 83: case 84: fourCC = 0x32495441; break; // BC5
            default:
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
                if (header.d3dFormat == 21)
                {
                    dds.ddspf.flags = DDPF_RGB | DDPF_ALPHAPIXELS;
                    dds.ddspf.rgbBitCount = 32;
                    dds.ddspf.rBitMask = 0x00FF0000;
                    dds.ddspf.gBitMask = 0x0000FF00;
                    dds.ddspf.bBitMask = 0x000000FF;
                    dds.ddspf.aBitMask = 0xFF000000;
                    break;
                }
                if (header.d3dFormat == 50)
                {
                    dds.ddspf.flags = DDPF_LUMINANCE;
                    dds.ddspf.rgbBitCount = 8;
                    dds.ddspf.rBitMask = 0x000000FF;
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

        const size_t totalSize =
            sizeof(DDS_MAGIC) +
            sizeof(DdsHeader) +
            (useDX10 ? sizeof(DdsHeaderDX10) : 0) +
            payload.size();
        ddsData.resize(totalSize);

        uint8_t* cursor = ddsData.data();
        std::memcpy(cursor, &DDS_MAGIC, sizeof(DDS_MAGIC));
        cursor += sizeof(DDS_MAGIC);
        std::memcpy(cursor, &dds, sizeof(dds));
        cursor += sizeof(dds);

        if (useDX10)
        {
            DdsHeaderDX10 dx10;
            dx10.dxgiFormat = GetDxgiFormat(header.d3dFormat);
            dx10.resourceDimension = 3; // D3D10_RESOURCE_DIMENSION_TEXTURE2D
            dx10.miscFlag = 0;
            dx10.arraySize = 1;
            dx10.miscFlags2 = 0;
            std::memcpy(cursor, &dx10, sizeof(dx10));
            cursor += sizeof(dx10);
        }

        if (!payload.empty())
        {
            std::memcpy(cursor, payload.data(), payload.size());
        }

        return true;
    }

    bool TextureExporter::WriteDds(
        const std::filesystem::path& outputPath,
        const DagorFormats::DdsxHeader& header,
        const std::vector<uint8_t>& payload,
        std::string& error)
    {
        std::vector<uint8_t> ddsData;
        if (!BuildDdsBuffer(header, payload, ddsData, error))
        {
            return false;
        }

        std::ofstream out(outputPath, std::ios::binary);
        if (!out)
        {
            error = "Could not open output file for writing.";
            return false;
        }
        out.write(reinterpret_cast<const char*>(ddsData.data()), static_cast<std::streamsize>(ddsData.size()));

        if (!out)
        {
            error = "Failed while writing DDS output.";
            return false;
        }

        return true;
    }
}
