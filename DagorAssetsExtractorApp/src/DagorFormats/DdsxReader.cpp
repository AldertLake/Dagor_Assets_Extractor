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
    return packedSize > 0;
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
    //   0x11  uint8   depth
    //   0x12  uint16  reserved
    //   0x14  uint32  memorySize  (decompressed)
    //   0x18  uint32  packedSize  (compressed, 0 = uncompressed)
    //   0x1C  uint32  compressionType

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
    m_header.depth           = reader.ReadUInt8();
    m_header.reserved        = reader.ReadUInt16();
    m_header.memorySize      = reader.ReadUInt32();
    m_header.packedSize      = reader.ReadUInt32();
    m_header.compressionType = reader.ReadUInt32();

    if (!m_header.IsValid())
    {
        throw std::runtime_error("Invalid DDSx magic (expected 0x78534444 / 'DDSx').");
    }
}

} // namespace DagorFormats
