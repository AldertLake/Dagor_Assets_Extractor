#pragma once
#include <vector>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace DagorIO {

class Decompressor {
public:
    // Zlib decompression
    static std::vector<uint8_t> DecompressZlib(std::span<const uint8_t> compressedData, size_t uncompressedSize);

    // Zstd decompression
    static std::vector<uint8_t> DecompressZstd(std::span<const uint8_t> compressedData, size_t uncompressedSize);
    static std::vector<uint8_t> DecompressZstd(std::span<const uint8_t> compressedData);

    // Oodle decompression (requires loading DLL from game folder)
    static std::vector<uint8_t> DecompressOodle(
        std::span<const uint8_t> compressedData,
        size_t uncompressedSize,
        const std::string& gameRoot,
        const std::string& explicitDllPath = {});

    // Helper to check if Oodle is available
    static bool IsOodleAvailable(const std::string& gameRoot, const std::string& explicitDllPath = {});

    // Locate the user-owned Oodle DLL without modifying the game install.
    static std::filesystem::path FindOodleDll(const std::string& gameRoot, const std::string& explicitDllPath = {});

    // Human-readable list of paths checked by FindOodleDll().
    static std::string OodleSearchDescription(const std::string& gameRoot, const std::string& explicitDllPath = {});
};

} // namespace DagorIO
