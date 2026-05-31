#include "Export/TextureExporter.h"
#include "Export/MaterialTextureResolver.h"
#include "DagorIO/BinaryReader.h"
#include "Platform/Platform.h"
#include "Export/ExportManifest.h"
#include "DagorIO/Decompression.h"
#include "Jobs/JobSystem.h"
#include "DirectXTex.h"

#include <objbase.h>
#include <wincodec.h>

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
        class ScopedComInitializer
        {
        public:
            ScopedComInitializer()
                : m_result(CoInitializeEx(nullptr, COINIT_MULTITHREADED))
            {
            }

            ~ScopedComInitializer()
            {
                if (SUCCEEDED(m_result))
                {
                    CoUninitialize();
                }
            }

            [[nodiscard]] bool IsReady() const
            {
                return SUCCEEDED(m_result) || m_result == RPC_E_CHANGED_MODE;
            }

            [[nodiscard]] HRESULT Result() const
            {
                return m_result;
            }

        private:
            HRESULT m_result = E_FAIL;
        };

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

            for (uint32_t blockY = 0; blockY < blocksHigh; ++blockY)
            {
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

        bool WritePng(const std::filesystem::path& outputPath, const RasterImage& image, std::string& error)
        {
            ScopedComInitializer com;
            if (!com.IsReady())
            {
                error = "Could not initialize COM for PNG export (" + FormatHresult(com.Result()) + ").";
                return false;
            }

            IWICImagingFactory* factory = nullptr;
            IWICStream* stream = nullptr;
            IWICBitmapEncoder* encoder = nullptr;
            IWICBitmapFrameEncode* frame = nullptr;
            IPropertyBag2* properties = nullptr;

            auto releaseAll = [&]() {
                if (properties) properties->Release();
                if (frame) frame->Release();
                if (encoder) encoder->Release();
                if (stream) stream->Release();
                if (factory) factory->Release();
            };

            const HRESULT factoryHr = CoCreateInstance(
                CLSID_WICImagingFactory,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&factory));
            if (FAILED(factoryHr))
            {
                error = "Could not create the WIC imaging factory (" + FormatHresult(factoryHr) + ").";
                releaseAll();
                return false;
            }

            HRESULT hr = factory->CreateStream(&stream);
            if (FAILED(hr))
            {
                error = "Could not create the WIC output stream (" + FormatHresult(hr) + ").";
                releaseAll();
                return false;
            }

            hr = stream->InitializeFromFilename(outputPath.c_str(), GENERIC_WRITE);
            if (FAILED(hr))
            {
                error = "Could not open the PNG output path (" + FormatHresult(hr) + ").";
                releaseAll();
                return false;
            }

            hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
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

        bool SplitNormalMapIfNeeded(
            RasterImage& image,
            std::string_view entryName,
            const std::filesystem::path& outputPath,
            std::string_view normalizedFormat,
            const TextureExportOptions& options,
            TextureExportResult& result,
            std::string& error)
        {
            if (!options.splitPackedNormalMap)
            {
                return true;
            }

            if (!LooksLikePackedNormalMap(entryName))
            {
                return true;
            }

            if (image.format != RasterPixelFormat::Bgra8)
            {
                return true;
            }

            RasterImage mrImage;
            mrImage.width = image.width;
            mrImage.height = image.height;
            mrImage.stride = image.stride;
            mrImage.format = RasterPixelFormat::Bgra8;
            mrImage.pixels.resize(image.pixels.size());

            for (size_t i = 0; i < image.pixels.size(); i += 4)
            {
                uint8_t origB = image.pixels[i + 0]; // Metallic
                uint8_t origG = image.pixels[i + 1]; // Normal Y
                uint8_t origR = image.pixels[i + 2]; // Roughness
                uint8_t origA = image.pixels[i + 3]; // Normal X

                float x = (static_cast<float>(origA) / 255.0f) * 2.0f - 1.0f;
                float y = (static_cast<float>(origG) / 255.0f) * 2.0f - 1.0f;
                float zSq = 1.0f - x * x - y * y;
                float z = (zSq > 0.0f) ? std::sqrt(zSq) : 0.0f;

                uint8_t normalX = origA;
                uint8_t normalY = origG;
                uint8_t normalZ = static_cast<uint8_t>(std::clamp(std::round((z + 1.0f) * 0.5f * 255.0f), 0.0f, 255.0f));

                image.pixels[i + 0] = normalZ; // B
                image.pixels[i + 1] = normalY; // G
                image.pixels[i + 2] = normalX; // R
                image.pixels[i + 3] = 255;     // A

                mrImage.pixels[i + 0] = static_cast<uint8_t>(255 - origB); // B (Metallic = 255 - Insulator)
                mrImage.pixels[i + 1] = static_cast<uint8_t>(255 - origR); // G (Roughness = 255 - Smoothness)
                mrImage.pixels[i + 2] = 255;                               // R (Occlusion)
                mrImage.pixels[i + 3] = 255;                               // A
            }

            std::filesystem::path mrOutputPath = outputPath;
            std::string stem = mrOutputPath.stem().string();
            std::string ext = mrOutputPath.extension().string();
            mrOutputPath.replace_filename(stem + "_metalrough" + ext);

            std::string mrError;
            if (!WriteReadableImage(mrOutputPath, mrImage, normalizedFormat, mrError))
            {
                error = "Failed to write split metallic-roughness image: " + mrError;
                return false;
            }

            result.warnings.push_back("Split packed normal map channels: normal (RGB) and metallic-roughness (ORM)");
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

            for (size_t row = 0; row < topImage->height; ++row)
            {
                const uint8_t* src = topImage->pixels + row * topImage->rowPitch;
                uint8_t* dst = image.pixels.data() + row * image.stride;
                if (isBgra8)
                {
                    std::memcpy(dst, src, image.stride);
                    continue;
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
            }

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
                    else if (header.compressionType == 4) // LZMA/7zip
                    {
                        payload = DagorIO::Decompressor::DecompressLzma(compressedData, header.memorySize);
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

            // Determine how many mips actually fit in the decompressed payload
            uint32_t actualMips = 0;
            size_t accumulatedMipSize = 0;
            std::vector<size_t> mipSizes;
            if (CalculateDdsMipSizes(header, mipSizes))
            {
                for (size_t i = 0; i < mipSizes.size(); ++i)
                {
                    if (accumulatedMipSize + mipSizes[i] <= payload.size())
                    {
                        accumulatedMipSize += mipSizes[i];
                        actualMips++;
                    }
                    else
                    {
                        break;
                    }
                }
            }

            DagorFormats::DdsxHeader exportHeader = header;
            if (actualMips > 0 && actualMips < std::max<uint32_t>(1, header.mipCount))
            {
                result.warnings.push_back(
                    "DDS payload size (" + std::to_string(payload.size()) +
                    " bytes) is smaller than expected for " + std::to_string(header.mipCount) +
                    " mip(s). Clamping mip count to " + std::to_string(actualMips) + " based on available data.");
                exportHeader.mipCount = static_cast<uint8_t>(actualMips);
            }

            std::string mipOrderWarning;
            std::string mipOrderError;
            if (!NormalizeMipOrderIfNeeded(exportHeader, payload, mipOrderWarning, mipOrderError))
            {
                result.error = mipOrderError + " " + BuildDdsxDiagnostic(exportHeader, packPayload.size());
                return result;
            }
            if (!mipOrderWarning.empty())
            {
                result.warnings.push_back(mipOrderWarning);
            }

            size_t expectedPayloadSize = 0;
            if (CalculateDdsPayloadSize(exportHeader, expectedPayloadSize))
            {
                if (payload.size() < expectedPayloadSize)
                {
                    result.error = "DDS payload is too small for " + std::string(exportHeader.FormatName()) +
                        " " + std::to_string(exportHeader.width) + "x" + std::to_string(exportHeader.height) +
                        " with " + std::to_string(std::max<uint32_t>(1, exportHeader.mipCount)) +
                        " mip(s): expected at least " + std::to_string(expectedPayloadSize) +
                        " bytes, got " + std::to_string(payload.size()) + ". " +
                        BuildDdsxDiagnostic(exportHeader, packPayload.size());
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
                    std::to_string(exportHeader.d3dFormat) + ".");
            }

            const std::string normalizedFormat = NormalizeTextureFormat(options.format);

            auto prepareOutputPath = [&](std::string_view extension, std::filesystem::path& outputPath) -> bool
            {
                outputPath = BuildSafeOutputPath(options.outputDir, entry.name, extension);
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

                auto finalizeReadableSuccess = [&]() {
                    result.success = true;
                    result.outputPath = outputPath;

                    if (exportHeader.mipCount > 1)
                    {
                        result.warnings.push_back(std::string(normalizedFormat) + " export writes only the top mip level.");
                    }

                    ExportManifest manifest;
                    manifest.sourcePack = Platform::PathToUtf8(packPath.filename());
                    manifest.sourceResource = entry.name;
                    manifest.assetType = "Texture (" + normalizedFormat + ")";
                    manifest.outputFiles.push_back(Platform::PathToUtf8(outputPath.filename()));
                    if (options.splitPackedNormalMap && LooksLikePackedNormalMap(entry.name))
                    {
                        std::filesystem::path mrPath = outputPath;
                        std::string stem = mrPath.stem().string();
                        std::string ext = mrPath.extension().string();
                        mrPath.replace_filename(stem + "_metalrough" + ext);
                        manifest.outputFiles.push_back(Platform::PathToUtf8(mrPath.filename()));
                    }
                    manifest.appVersion = "0.1.0-alpha";
                    manifest.formatDecoderVersion = "DdsxReader v1.0";
                    manifest.skipWrite = !options.exportJsonManifest;

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
                };

                RasterImage image;
                std::string readableError;
                if (DecodeTopMipToRaster(exportHeader, payload, image, readableError) &&
                    SplitNormalMapIfNeeded(image, entry.name, outputPath, normalizedFormat, options, result, readableError) &&
                    WriteReadableImage(outputPath, image, normalizedFormat, readableError))
                {
                    finalizeReadableSuccess();
                    return result;
                }

                std::vector<uint8_t> ddsData;
                std::string directXTexError;
                RasterImage directXTexImage;
                if (BuildDdsBuffer(exportHeader, payload, ddsData, directXTexError) &&
                    DecodeRasterWithDirectXTex(ddsData, directXTexImage, directXTexError) &&
                    SplitNormalMapIfNeeded(directXTexImage, entry.name, outputPath, normalizedFormat, options, result, directXTexError) &&
                    WriteReadableImage(outputPath, directXTexImage, normalizedFormat, directXTexError))
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

                if (!options.preserveOriginalDDS)
                {
                    result.error = readableError;
                    return result;
                }

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

                TextureExportResult result = ExportTexture(packPath, entry, options);

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
