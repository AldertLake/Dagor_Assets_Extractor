#include "AssetIndex/AssetIndex.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
namespace AssetIndex = DagorAssetsExtractor::AssetIndex;

namespace
{
    bool Expect(bool condition, const char* message)
    {
        if (!condition)
        {
            std::cerr << "FAIL: " << message << '\n';
            return false;
        }
        return true;
    }

    size_t CountType(const std::vector<AssetIndex::ResourceRecord>& records, AssetIndex::AssetType type)
    {
        size_t count = 0;
        for (const AssetIndex::ResourceRecord& record : records)
        {
            if (record.type == type)
            {
                ++count;
            }
        }
        return count;
    }
}

int main(int argc, char** argv)
{
    fs::path repoRoot = argc > 1 ? fs::path(argv[1]) : fs::current_path();
    const fs::path fixtureRoot = repoRoot / "tests" / "fixtures" / "synthetic_wt";

    bool ok = true;

    const AssetIndex::ValidationResult validation = AssetIndex::ValidateGameRoot(fixtureRoot);
    ok &= Expect(validation.IsValid(), "synthetic fixture should validate as a plausible install");
    ok &= Expect(validation.hasAces, "validation should find aces.exe");
    ok &= Expect(validation.hasContent, "validation should find content folder");
    ok &= Expect(validation.hasBaseRes, "validation should find content/base/res");
    ok &= Expect(validation.hasKnownContainer, "validation should find known Dagor markers");

    const AssetIndex::DiscoverResult discovered = AssetIndex::DiscoverResources(fixtureRoot);
    ok &= Expect(discovered.error.empty(), "discovery should complete without error");
    ok &= Expect(!discovered.canceled, "discovery should not be canceled");
    ok &= Expect(discovered.records.size() == 6, "discovery should find six synthetic Dagor records");
    ok &= Expect(CountType(discovered.records, AssetIndex::AssetType::Vromfs) == 1, "discovery should find one VROMFS");
    ok &= Expect(CountType(discovered.records, AssetIndex::AssetType::Grp) == 1, "discovery should find one GRP");
    ok &= Expect(CountType(discovered.records, AssetIndex::AssetType::Dxp) == 1, "discovery should find one DXP");
    ok &= Expect(CountType(discovered.records, AssetIndex::AssetType::Descriptor) == 2, "discovery should find two descriptors");
    ok &= Expect(CountType(discovered.records, AssetIndex::AssetType::Blk) == 1, "discovery should find one BLK");

    const std::array<std::pair<const char*, AssetIndex::AssetType>, 5> classifyCases = {
        std::pair{ "foo.vromfs.bin", AssetIndex::AssetType::Vromfs },
        std::pair{ "vehicle.GRP", AssetIndex::AssetType::Grp },
        std::pair{ "tex.dxp.bin", AssetIndex::AssetType::Dxp },
        std::pair{ "riDesc.bin", AssetIndex::AssetType::Descriptor },
        std::pair{ "rules.blk", AssetIndex::AssetType::Blk },
    };

    for (const auto& testCase : classifyCases)
    {
        const std::optional<AssetIndex::AssetType> type = AssetIndex::ClassifyPath(testCase.first);
        ok &= Expect(type.has_value() && *type == testCase.second, "classification should match expected type");
    }

    const fs::path cacheDir = fs::temp_directory_path() / "DagorAssetsExtractorTests";
    const fs::path cachePath = cacheDir / "index-cache-test.txt";
    std::error_code ec;
    fs::remove_all(cacheDir, ec);

    std::string cacheError;
    ok &= Expect(
        AssetIndex::SaveIndexCacheRecords(cachePath, fixtureRoot, discovered.records, cacheError),
        "cache save should succeed");
    if (!cacheError.empty())
    {
        std::cerr << "Cache save detail: " << cacheError << '\n';
    }

    const AssetIndex::IndexCacheLoadResult cacheLoad = AssetIndex::LoadIndexCacheRecords(cachePath, fixtureRoot);
    ok &= Expect(cacheLoad.fileFound, "cache load should find the cache file");
    ok &= Expect(cacheLoad.rootMatched, "cache load should match fixture root");
    ok &= Expect(cacheLoad.recordsLoaded == discovered.records.size(), "cache load should restore all records");
    ok &= Expect(cacheLoad.recordsSkipped == 0, "cache load should skip no fresh records");

    fs::remove_all(cacheDir, ec);

    if (!ok)
    {
        return 1;
    }

    std::cout << "AssetIndexTests passed\n";
    return 0;
}
