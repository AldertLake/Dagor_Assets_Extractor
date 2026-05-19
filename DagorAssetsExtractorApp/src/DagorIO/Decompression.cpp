#include "Decompression.h"
#include <windows.h>
#include <stdexcept>
#include <array>
#include <filesystem>
#include <limits>
#include <sstream>
#include "tinf.h"
#include "zstd.h"

namespace DagorIO {

typedef int(__stdcall* OodleLZ_Decompress_Func)(const void* src, size_t srcLen, void* dst, size_t dstLen, int fuzz, int crc, int verbosity, void* context, size_t unused, void* malloc, void* free, void* userdata);

namespace {
    std::filesystem::path GetExecutableDirectory() {
        std::array<wchar_t, MAX_PATH> buffer{};
        const DWORD written = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0 || written >= buffer.size()) {
            return {};
        }
        return std::filesystem::path(buffer.data()).parent_path();
    }

    std::vector<std::filesystem::path> BuildOodleSearchPaths(const std::string& gameRoot, const std::string& explicitDllPath) {
        const std::filesystem::path root(gameRoot);
        const std::filesystem::path explicitPath(explicitDllPath);
        const std::filesystem::path exeDir = GetExecutableDirectory();

        std::vector<std::filesystem::path> paths;
        if (!explicitPath.empty()) {
            paths.push_back(explicitPath);
        }

        if (!root.empty()) {
            paths.push_back(root / "oo2core_6_win64.dll");
            paths.push_back(root / "win64" / "oo2core_6_win64.dll");
            paths.push_back(root / "binaries" / "oo2core_6_win64.dll");
            paths.push_back(root / "bin" / "oo2core_6_win64.dll");
        }

        if (!exeDir.empty()) {
            paths.push_back(exeDir / "oo2core_6_win64.dll");
        }

        return paths;
    }

    std::vector<std::filesystem::path> BuildBundledOodleSearchPaths() {
        const std::filesystem::path exeDir = GetExecutableDirectory();
        std::vector<std::filesystem::path> paths;

        if (exeDir.empty()) {
            return paths;
        }

        paths.push_back(exeDir / "oo2core_6_win64.dll");

        std::filesystem::path current = exeDir;
        for (int depth = 0; depth < 4 && !current.empty(); ++depth) {
            paths.push_back(current / "third_party" / "oo2core_6_win64.dll");
            current = current.parent_path();
        }

        return paths;
    }
}

// Note: Some versions of Oodle have a simpler signature or different parameter counts.
// The 6-parameter version is often a wrapper or a different export.
// We'll try to find the correct one or use the most common one.
// War Thunder's oo2core_6_win64.dll usually has OodleLZ_Decompress.

std::vector<uint8_t> Decompressor::DecompressZlib(std::span<const uint8_t> compressedData, size_t uncompressedSize) {
    std::vector<uint8_t> result(uncompressedSize);
    unsigned int destLen = static_cast<unsigned int>(uncompressedSize);
    int status = tinf_zlib_uncompress(result.data(), &destLen, compressedData.data(), static_cast<unsigned int>(compressedData.size()));

    if (status != TINF_OK) {
        throw std::runtime_error("Zlib decompression (tinf) failed with status " + std::to_string(status));
    }

    if (destLen != uncompressedSize) {
        result.resize(destLen);
    }

    return result;
}

std::vector<uint8_t> Decompressor::DecompressZstd(std::span<const uint8_t> compressedData, size_t uncompressedSize) {
    std::vector<uint8_t> result(uncompressedSize);
    size_t status = ZSTD_decompress(result.data(), uncompressedSize, compressedData.data(), compressedData.size());

    if (ZSTD_isError(status)) {
        throw std::runtime_error("Zstd decompression failed: " + std::string(ZSTD_getErrorName(status)));
    }

    if (status != uncompressedSize) {
        result.resize(status);
    }

    return result;
}

std::vector<uint8_t> Decompressor::DecompressZstd(std::span<const uint8_t> compressedData) {
    const unsigned long long frameSize = ZSTD_getFrameContentSize(compressedData.data(), compressedData.size());
    if (frameSize == ZSTD_CONTENTSIZE_ERROR) {
        throw std::runtime_error("Zstd decompression failed: invalid frame");
    }
    if (frameSize == ZSTD_CONTENTSIZE_UNKNOWN) {
        throw std::runtime_error("Zstd decompression failed: frame does not declare decompressed size");
    }
    if (frameSize > static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
        throw std::runtime_error("Zstd decompression failed: frame is too large for this process");
    }

    return DecompressZstd(compressedData, static_cast<size_t>(frameSize));
}

std::filesystem::path Decompressor::FindOodleDll(const std::string& gameRoot, const std::string& explicitDllPath) {
    for (const std::filesystem::path& dllPath : BuildOodleSearchPaths(gameRoot, explicitDllPath)) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(dllPath, ec)) {
            return dllPath;
        }
    }
    return {};
}

std::filesystem::path Decompressor::FindBundledOodleDll() {
    for (const std::filesystem::path& dllPath : BuildBundledOodleSearchPaths()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(dllPath, ec)) {
            return dllPath;
        }
    }
    return {};
}

std::string Decompressor::OodleSearchDescription(const std::string& gameRoot, const std::string& explicitDllPath) {
    std::ostringstream out;
    const std::vector<std::filesystem::path> paths = BuildOodleSearchPaths(gameRoot, explicitDllPath);
    for (size_t i = 0; i < paths.size(); ++i) {
        if (i != 0) {
            out << "; ";
        }
        out << paths[i].string();
    }
    return out.str();
}

bool Decompressor::IsOodleAvailable(const std::string& gameRoot, const std::string& explicitDllPath) {
    return !FindOodleDll(gameRoot, explicitDllPath).empty();
}

std::vector<uint8_t> Decompressor::DecompressOodle(
    std::span<const uint8_t> compressedData,
    size_t uncompressedSize,
    const std::string& gameRoot,
    const std::string& explicitDllPath) {
    std::filesystem::path dllPath = FindOodleDll(gameRoot, explicitDllPath);
    if (dllPath.empty()) {
        throw std::runtime_error("Failed to locate oo2core_6_win64.dll. Checked: " + OodleSearchDescription(gameRoot, explicitDllPath));
    }

    HMODULE hOodle = LoadLibraryW(dllPath.c_str());
    if (!hOodle) {
        throw std::runtime_error("Failed to load Oodle DLL from " + dllPath.string());
    }

    // Attempt to find the decompression function.
    // There are several variants, OodleLZ_Decompress is the most common.
    auto decompress = (OodleLZ_Decompress_Func)GetProcAddress(hOodle, "OodleLZ_Decompress");
    if (!decompress) {
        FreeLibrary(hOodle);
        throw std::runtime_error("Failed to find OodleLZ_Decompress in DLL");
    }

    std::vector<uint8_t> result(uncompressedSize);

    // OodleLZ_Decompress signature can vary.
    // The most common one in recent years has many parameters.
    // We'll use the one identified in research if it matches, but dynamic loading of Oodle is tricky.
    // In wt-tools they use a 6-parameter version for some versions, but 14-parameter for others.
    // For oo2core_6, it's often the 14-parameter one.

    // For now, we'll try to call it.
    // WARNING: This might crash if the signature is wrong.
    // A safer way is to use the exact signature for the version found in WT.

    // Many implementations use:
    // int OodleLZ_Decompress(const void *src, size_t src_len, void *dst, size_t dst_size,
    //                        int fuzz, int crc, int verbose, uint8_t *dst_base, size_t e,
    //                        void *cb, void *cb_ctx, void *scratch, size_t scratch_size, int threadPhase)

    typedef int(__stdcall* OodleLZ_Decompress_Full)(const void* src, size_t src_len, void* dst, size_t dst_size,
                                                   int fuzz, int crc, int verbose, void* dst_base, size_t e,
                                                   void* cb, void* cb_ctx, void* scratch, size_t scratch_size, int threadPhase);

    auto decompressFull = (OodleLZ_Decompress_Full)GetProcAddress(hOodle, "OodleLZ_Decompress");

    int bytesRead = 0;
    if (decompressFull) {
        bytesRead = decompressFull(compressedData.data(), compressedData.size(), result.data(), uncompressedSize,
                                  0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3);
    }

    FreeLibrary(hOodle);

    if (bytesRead <= 0) {
        throw std::runtime_error("Oodle decompression failed");
    }

    return result;
}

} // namespace DagorIO
