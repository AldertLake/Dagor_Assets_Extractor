#include "Decompression.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <stdexcept>
#include <array>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include "tinf.h"
#include "zstd.h"
#include "LzmaDec.h"

namespace DagorIO {

typedef int(__stdcall* OodleLZ_Decompress_Full)(
    const void* src, size_t src_len, void* dst, size_t dst_size,
    int fuzz, int crc, int verbose, void* dst_base, size_t e,
    void* cb, void* cb_ctx, void* scratch, size_t scratch_size, int threadPhase);

namespace {
    struct ScopedZstdDCtx {
        ZSTD_DCtx* ctx = nullptr;

        ScopedZstdDCtx() : ctx(ZSTD_createDCtx()) {}
        ~ScopedZstdDCtx() {
            if (ctx != nullptr) {
                ZSTD_freeDCtx(ctx);
                ctx = nullptr;
            }
        }

        ScopedZstdDCtx(const ScopedZstdDCtx&) = delete;
        ScopedZstdDCtx& operator=(const ScopedZstdDCtx&) = delete;
    };

    ZSTD_DCtx* GetThreadLocalZstdDCtx() {
        thread_local ScopedZstdDCtx tlsDCtx;
        return tlsDCtx.ctx;
    }

    std::wstring NormalizeCacheKey(const std::filesystem::path& path) {
        std::wstring key = path.lexically_normal().wstring();
        std::transform(key.begin(), key.end(), key.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return key;
    }

    struct LoadedOodleLibrary {
        HMODULE moduleHandle = nullptr;
        OodleLZ_Decompress_Full decompressFn = nullptr;
    };

    struct OodleCache {
        std::mutex mutex;
        std::filesystem::path bundledDllPath;
        std::unordered_map<std::string, std::filesystem::path> resolvedPaths;
        std::unordered_map<std::wstring, LoadedOodleLibrary> loadedModules;
    };

    OodleCache& GetOodleCache() {
        static OodleCache cache;
        return cache;
    }

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

    OodleLZ_Decompress_Full GetCachedOodleDecompress(const std::filesystem::path& dllPath) {
        const std::wstring key = NormalizeCacheKey(dllPath);
        OodleCache& cache = GetOodleCache();
        std::lock_guard<std::mutex> lock(cache.mutex);

        if (const auto it = cache.loadedModules.find(key); it != cache.loadedModules.end()) {
            return it->second.decompressFn;
        }

        HMODULE hOodle = LoadLibraryW(dllPath.c_str());
        if (!hOodle) {
            throw std::runtime_error("Failed to load Oodle DLL from " + dllPath.string());
        }

        auto decompressFull = reinterpret_cast<OodleLZ_Decompress_Full>(
            GetProcAddress(hOodle, "OodleLZ_Decompress"));
        if (!decompressFull) {
            FreeLibrary(hOodle);
            throw std::runtime_error("Failed to find OodleLZ_Decompress in DLL");
        }

        cache.loadedModules.emplace(key, LoadedOodleLibrary{ hOodle, decompressFull });
        return decompressFull;
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

std::vector<uint8_t> Decompressor::DecompressZlib(std::span<const uint8_t> compressedData) {
    size_t guessSize = std::max<size_t>(1024, compressedData.size() * 4);
    const size_t maxGuessSize = 256ull * 1024ull * 1024ull;

    while (guessSize <= maxGuessSize) {
        std::vector<uint8_t> result(guessSize);
        unsigned int destLen = static_cast<unsigned int>(guessSize);
        const int status = tinf_zlib_uncompress(
            result.data(),
            &destLen,
            compressedData.data(),
            static_cast<unsigned int>(compressedData.size()));

        if (status == TINF_OK) {
            result.resize(destLen);
            return result;
        }

        if (status != TINF_BUF_ERROR) {
            throw std::runtime_error("Zlib decompression (tinf) failed with status " + std::to_string(status));
        }

        guessSize *= 2;
    }

    throw std::runtime_error("Zlib decompression failed: output exceeded size guard");
}

std::vector<uint8_t> Decompressor::DecompressZstd(std::span<const uint8_t> compressedData, size_t uncompressedSize) {
    std::vector<uint8_t> result(uncompressedSize);
    ZSTD_DCtx* dctx = GetThreadLocalZstdDCtx();
    const size_t status = (dctx != nullptr)
        ? ZSTD_decompressDCtx(dctx, result.data(), uncompressedSize, compressedData.data(), compressedData.size())
        : ZSTD_decompress(result.data(), uncompressedSize, compressedData.data(), compressedData.size());

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

namespace {
    void* SzAlloc(ISzAllocPtr, size_t size) { return std::malloc(size); }
    void SzFree(ISzAllocPtr, void* address) { std::free(address); }
    const ISzAlloc g_LzmaAlloc = { SzAlloc, SzFree };
}

std::vector<uint8_t> Decompressor::DecompressLzma(std::span<const uint8_t> compressedData, size_t uncompressedSize) {
    if (compressedData.size() < 13) {
        throw std::runtime_error("LZMA compressed data is too small (need >= 13 bytes for header)");
    }

    const uint8_t* propData = compressedData.data();
    const uint8_t* src = compressedData.data() + 13;
    size_t srcLen = compressedData.size() - 13;

    std::vector<uint8_t> result(uncompressedSize);
    size_t destLen = uncompressedSize;

    ELzmaStatus status;
    SRes res = LzmaDecode(
        result.data(), &destLen,
        src, &srcLen,
        propData, 5,
        LZMA_FINISH_ANY,
        &status,
        &g_LzmaAlloc
    );

    if (res != SZ_OK) {
        throw std::runtime_error("LZMA decompression failed with error code: " + std::to_string(res));
    }

    if (destLen != uncompressedSize) {
        result.resize(destLen);
    }

    return result;
}

std::vector<uint8_t> Decompressor::DecompressLzma(std::span<const uint8_t> compressedData) {
    if (compressedData.size() < 13) {
        throw std::runtime_error("LZMA compressed data is too small (need >= 13 bytes for header)");
    }

    uint64_t uncompressedSize = 0;
    for (int i = 0; i < 8; ++i) {
        uncompressedSize |= (static_cast<uint64_t>(compressedData[5 + i]) << (8 * i));
    }

    if (uncompressedSize == 0xFFFFFFFFFFFFFFFFULL) {
        throw std::runtime_error("LZMA stream declares unknown uncompressed size");
    }

    return DecompressLzma(compressedData, static_cast<size_t>(uncompressedSize));
}

std::filesystem::path Decompressor::FindOodleDll(const std::string& gameRoot, const std::string& explicitDllPath) {
    const std::string cacheKey = gameRoot + "\n" + explicitDllPath;
    OodleCache& cache = GetOodleCache();
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (const auto it = cache.resolvedPaths.find(cacheKey); it != cache.resolvedPaths.end()) {
            return it->second;
        }
    }

    std::filesystem::path found;
    for (const std::filesystem::path& dllPath : BuildOodleSearchPaths(gameRoot, explicitDllPath)) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(dllPath, ec)) {
            found = dllPath;
            break;
        }
    }

    if (!found.empty()) {
        std::lock_guard<std::mutex> lock(cache.mutex);
        cache.resolvedPaths.emplace(cacheKey, found);
    }
    return found;
}

std::filesystem::path Decompressor::FindBundledOodleDll() {
    OodleCache& cache = GetOodleCache();
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (!cache.bundledDllPath.empty()) {
            return cache.bundledDllPath;
        }
    }

    std::filesystem::path found;
    for (const std::filesystem::path& dllPath : BuildBundledOodleSearchPaths()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(dllPath, ec)) {
            found = dllPath;
            break;
        }
    }

    if (!found.empty()) {
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (cache.bundledDllPath.empty()) {
            cache.bundledDllPath = found;
        }
    }
    return found;
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
    const std::filesystem::path dllPath = FindOodleDll(gameRoot, explicitDllPath);
    if (dllPath.empty()) {
        throw std::runtime_error("Failed to locate oo2core_6_win64.dll. Checked: " + OodleSearchDescription(gameRoot, explicitDllPath));
    }

    const OodleLZ_Decompress_Full decompressFull = GetCachedOodleDecompress(dllPath);
    std::vector<uint8_t> result(uncompressedSize);

    const int bytesRead = decompressFull(
        compressedData.data(),
        compressedData.size(),
        result.data(),
        uncompressedSize,
        0, 0, 0, nullptr, 0, nullptr, nullptr, nullptr, 0, 3);

    if (bytesRead <= 0) {
        throw std::runtime_error("Oodle decompression failed");
    }

    return result;
}

} // namespace DagorIO
