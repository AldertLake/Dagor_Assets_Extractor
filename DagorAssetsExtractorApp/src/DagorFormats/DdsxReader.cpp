#include "DdsxReader.h"
#include <stdexcept>

namespace DagorFormats {

// -- DdsxHeader helpers -------------------------------------------------------

bool DdsxHeader::IsValid() const
{
    // Magic bytes "DDSx" in little-endian: 0x78534444
    return magic == 0x78534444;
}

bool DdsxHeader::IsCompressed() const
{
    return compressionType != 0;
}

bool DdsxHeader::IsOodleCompressed() const
{
    return IsCompressed() && compressionType == 3;
}

const char* DdsxHeader::FormatName() const
{
    // D3D format codes observed in Dagor DDSx files.
    // These come from the header's d3dFormat field and map to
    // legacy D3DFORMAT or DXGI_FORMAT values.
    switch (d3dFormat)
    {
    // Legacy D3DFORMAT values (common in older Dagor assets)
    case 827611204:  return "DXT1 (BC1)";     // MAKEFOURCC('D','X','T','1')
    case 861165636:  return "DXT3 (BC2)";     // MAKEFOURCC('D','X','T','3')
    case 894720068:  return "DXT5 (BC3)";     // MAKEFOURCC('D','X','T','5')
    case 826889281:  return "ATI1 (BC4)";     // MAKEFOURCC('A','T','I','1')
    case 843666497:  return "ATI2 (BC5)";     // MAKEFOURCC('A','T','I','2')

    // DXGI_FORMAT values (newer Dagor)
    case 71:         return "BC1_UNORM";
    case 72:         return "BC1_UNORM_SRGB";
    case 74:         return "BC2_UNORM";
    case 75:         return "BC2_UNORM_SRGB";
    case 77:         return "BC3_UNORM";
    case 78:         return "BC3_UNORM_SRGB";
    case 80:         return "BC4_UNORM";
    case 81:         return "BC4_SNORM";
    case 83:         return "BC5_UNORM";
    case 84:         return "BC5_SNORM";
    case 95:         return "BC6H_UF16";
    case 96:         return "BC6H_SF16";
    case 98:         return "BC7_UNORM";
    case 99:         return "BC7_UNORM_SRGB";
    case 28:         return "R8G8B8A8_UNORM";
    case 29:         return "R8G8B8A8_UNORM_SRGB";
    case 87:         return "B8G8R8A8_UNORM";
    case 91:         return "B8G8R8A8_UNORM_SRGB";
    case 61:         return "R8_UNORM";
    case 49:         return "R8G8_UNORM";
    case 10:         return "R16G16B16A16_FLOAT";
    case 2:          return "R32G32B32A32_FLOAT";

    default:         return "Unknown";
    }
}

const char* DdsxHeader::CompressionName() const
{
    switch (compressionType)
    {
    case 0:  return "None";
    case 1:  return "zlib";
    case 2:  return "zstd";
    case 3:  return "Oodle";
    case 4:  return "LZMA/7zip";
    default: return "Unknown";
    }
}

// -- DdsxReader ---------------------------------------------------------------

DdsxReader::DdsxReader(std::span<const uint8_t> data) : m_data(data) {}

void DdsxReader::Parse()
{
    // DDSx header is 32 bytes:
    //   0x00  uint32  magic       ("DDSx" = 0x78534444)
    //   0x04  uint32  d3dFormat
    //   0x08  uint32  flags
    //   0x0C  uint16  width
    //   0x0E  uint16  height
    //   0x10  uint8   mipCount
    //   0x11  uint8   hqPartLevels
    //   0x12  uint16  depth
    //   0x14  uint16  bitsPerPixel
    //   0x16  uint16  quality/shift nibbles
    //   0x18  uint32  memorySize  (decompressed)
    //   0x1C  uint32  packedSize  (compressed, 0 = uncompressed)
    //
    // Compression is encoded in flags, not in a separate field:
    //   flags high byte 0x20 = zstd, 0x40 = lzma/7zip, 0x60 = Oodle, 0x80 = zlib.

    if (m_data.size() < 32)
    {
        throw std::runtime_error("DDSx data too small for header (need >= 32 bytes).");
    }

    DagorIO::MemoryReader reader(m_data);

    m_header.magic           = reader.ReadUInt32();
    m_header.d3dFormat       = reader.ReadUInt32();
    m_header.flags           = reader.ReadUInt32();
    m_header.width           = reader.ReadUInt16();
    m_header.height          = reader.ReadUInt16();
    m_header.mipCount        = reader.ReadUInt8();
    m_header.hqPartLevels    = reader.ReadUInt8();
    m_header.depth           = reader.ReadUInt16();
    m_header.bitsPerPixel    = reader.ReadUInt16();
    const uint16_t quality   = reader.ReadUInt16();
    m_header.lQmip           = static_cast<uint8_t>(quality & 0x0F);
    m_header.mQmip           = static_cast<uint8_t>((quality >> 4) & 0x0F);
    m_header.dxtShift        = static_cast<uint8_t>((quality >> 8) & 0x0F);
    m_header.uQmip           = static_cast<uint8_t>((quality >> 12) & 0x0F);
    m_header.memorySize      = reader.ReadUInt32();
    m_header.packedSize      = reader.ReadUInt32();
    m_header.compressionFlag = static_cast<uint8_t>((m_header.flags >> 24) & 0xE0);

    switch (m_header.compressionFlag)
    {
    case 0x00:
        m_header.compressionType = 0;
        break;
    case 0x20:
        m_header.compressionType = 2; // zstd
        break;
    case 0x40:
        m_header.compressionType = 4; // lzma/7zip
        break;
    case 0x60:
        m_header.compressionType = 3; // Oodle
        break;
    case 0x80:
        m_header.compressionType = 1; // zlib
        break;
    default:
        m_header.compressionType = 0xFFFFFFFFu;
        break;
    }

    if (!m_header.IsValid())
    {
        throw std::runtime_error("Invalid DDSx magic (expected 0x78534444 / 'DDSx').");
    }
}

} // namespace DagorFormats
