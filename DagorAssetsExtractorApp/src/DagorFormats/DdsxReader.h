#pragma once
#include "DagorIO/BinaryReader.h"
#include <cstdint>
#include <string>
#include <span>

namespace DagorFormats {

// Known DXGI / D3D formats commonly found in DDSx textures.
// Not exhaustive — we map what we can recognize and mark the rest as Unknown.
enum class DdsxFormat : uint32_t
{
    Unknown = 0,
    DXT1    = 1,  // BC1
    DXT3    = 3,  // BC2
    DXT5    = 5,  // BC3
    ATI1    = 6,  // BC4
    ATI2    = 7,  // BC5
    BC7     = 10,
    RGBA8   = 20,
    R8      = 30,
    L8      = 31,
};

struct DdsxHeader
{
    uint32_t magic         = 0;   // Expected: 'DDSx'
    uint32_t d3dFormat     = 0;   // Raw D3D/DXGI format code
    uint32_t flags         = 0;
    uint16_t width         = 0;
    uint16_t height        = 0;
    uint8_t  mipCount      = 0;
    uint8_t  hqPartLevels  = 0;
    uint16_t depth         = 1;   // For 3D / array textures
    uint16_t bitsPerPixel  = 0;
    uint8_t  lQmip         = 0;
    uint8_t  mQmip         = 0;
    uint8_t  dxtShift      = 0;
    uint8_t  uQmip         = 0;
    uint32_t memorySize    = 0;   // Decompressed payload size
    uint32_t packedSize    = 0;   // Compressed payload size (0 = uncompressed)
    uint8_t  compressionFlag = 0; // Header flags high byte: 0x20 zstd, 0x40 lzma, 0x60 oodle, 0x80 zlib
    uint32_t compressionType = 0; // Normalized: 0 none, 1 zlib, 2 zstd, 3 oodle, 4 lzma/7zip

    // Derived helpers
    bool IsValid() const;
    bool IsCompressed() const;
    bool IsOodleCompressed() const;
    const char* FormatName() const;
    const char* CompressionName() const;
};

// Reads and parses a standalone DDSx header from raw bytes.
// Expects at least 32 bytes of data.
class DdsxReader {
public:
    explicit DdsxReader(std::span<const uint8_t> data);

    void Parse();

    const DdsxHeader& GetHeader() const { return m_header; }

private:
    std::span<const uint8_t> m_data;
    DdsxHeader m_header;
};

} // namespace DagorFormats
