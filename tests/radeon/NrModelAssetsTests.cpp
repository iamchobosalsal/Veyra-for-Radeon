// SPDX-License-Identifier: GPL-3.0-only
// Metadata-only synthetic filesystem tests; no model weights or HIP inference.
// Fixture shapes come from pinned Lmxxf 0.44 (4764e4f), SmallAssetsAvailable and
// RunGraph/WeightElements, independently of the application's readiness tables.
#include "veyra/base/NrModelAssets.h"
#include <windows.h>
#include <winioctl.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
namespace fs = std::filesystem;
using veyra::base::inspectNrModelAssets;
using veyra::base::findNrModelAssets;
using veyra::base::readNrModelAssetsReference;
#include "../support/NrModelAssetFixtures.h"
using namespace veyra::tests::nrassets;

int main() {
    unsigned checks = 0, failed = 0;
    auto check = [&](bool result, const std::string& name) {
        ++checks;
        if (!result) { ++failed; std::cerr << "FAIL " << name << '\n'; }
    };
    struct NewLayerCase { bool preferred, smallAvailable, originalAvailable, expected; const char* name; };
    constexpr NewLayerCase newLayerCases[] = {
        {false, false, false, false, "new Original preference retained when neither model exists"},
        {false, false, true,  false, "new Original preference retained when only Original exists"},
        {false, true,  false, true,  "new Original preference switches to available Small"},
        {false, true,  true,  false, "new Original preference retained when both models exist"},
        {true,  false, false, true,  "new Small preference retained when neither model exists"},
        {true,  false, true,  false, "new Small preference switches to available Original"},
        {true,  true,  false, true,  "new Small preference retained when only Small exists"},
        {true,  true,  true,  true,  "new Small preference retained when both models exist"},
    };
    for (const auto& item : newLayerCases)
        check(veyra::base::nrPerformanceForNewLayer(item.preferred, item.smallAvailable,
                                                  item.originalAvailable) == item.expected, item.name);
    try {
        Scratch scratch;
        auto absent = inspectNrModelAssets(scratch.root / "absent");
        check(!absent.shared.ready && !absent.smallModel.ready && !absent.originalModel.ready && !absent.anyReady(),
              "absent directory never advertises either model");
        const auto empty = scratch.root / "empty"; fs::create_directory(empty);
        check(!inspectNrModelAssets(empty).anyReady(), "fresh empty assets reject Original unconditionally-ready regression");
        const auto full = scratch.root / L"\uD55C\uAE00 \uACBD\uB85C" / "assets";
        makeAssets(full, true, true);
        auto ready = inspectNrModelAssets(full);
        check(ready.shared.ready && ready.smallModel.ready && ready.originalModel.ready && ready.anyReady(),
              "complete mixed-purpose metadata fixture admits both models");
        if (!ready.anyReady()) std::cerr << "Fixture issue: " << ready.shared.issue << " / " << ready.smallModel.issue << " / " << ready.originalModel.issue << '\n';
        for (const auto& tensor : originalTensors) {
            const auto path = full / (std::string(tensor.name) + ".f16");
            fs::remove(path);
            const auto status = inspectNrModelAssets(full);
            check(!status.originalModel.ready && !status.smallModel.ready,
                  std::string("both models require Original startup tensor ") + tensor.name);
            sparseFile(path, tensor.f16Bytes);
        }
        for (const auto& tensor : smallTensors) {
            const auto path = full / "small" / (std::string(tensor.name) + ".f16");
            fs::remove(path);
            const auto status = inspectNrModelAssets(full);
            check(!status.smallModel.ready && status.originalModel.ready, std::string("Small independently requires ") + tensor.name);
            sparseFile(path, tensor.f16Bytes);
        }
        const auto damaged = full / "block62-weights.f16";
        sparseFile(damaged, 2);
        const auto truncated = inspectNrModelAssets(full);
        check(!truncated.originalModel.ready && !truncated.smallModel.ready,
              "truncated Original startup tensor rejects both models");
        sparseFile(damaged, 8256ull * 2);
        const auto override = full / "small" / "post70-head.f32";
        sparseFile(override, 1);
        check(!inspectNrModelAssets(full).smallModel.ready, "bad existing f32 cannot fall back to valid f16");
        sparseFile(override, 96ull * 4);
        check(inspectNrModelAssets(full).smallModel.ready, "valid f32 takes precedence alongside valid f16");
        fs::remove(override);
        textFile(full / "small" / "model-manifest.json", "{\"model\": \"original\"}\n");
        check(!inspectNrModelAssets(full).smallModel.ready && inspectNrModelAssets(full).originalModel.ready,
              "wrong Small manifest does not disable valid Original");
        smallManifest(full);
        for (const auto* codec : codecs) {
            fs::remove(full / "shaders" / codec);
            check(!inspectNrModelAssets(full).anyReady(), std::string("shared codec required: ") + codec);
            textFile(full / "shaders" / codec, "// synthetic codec fixture\n");
        }
        fs::rename(full / "shaders" / "native_codec_decode.hlsl", full / "native_codec_decode.hlsl");
        check(!inspectNrModelAssets(full).anyReady(), "codec files cannot be split across shader directories");
        fs::rename(full / "native_codec_decode.hlsl", full / "shaders" / "native_codec_decode.hlsl");
        for (const auto* kernel : kernels) {
            fs::remove(full / "HIP" / kernel);
            check(!inspectNrModelAssets(full).anyReady(), std::string("shared kernel required: ") + kernel);
            textFile(full / "HIP" / kernel, "x");
        }
        textFile(full / "HIP" / "SHA256SUMS", "");
        check(!inspectNrModelAssets(full).anyReady(), "empty module manifest rejected");
        modules(full / "HIP");
        {
            std::ofstream sums(full / "HIP" / "SHA256SUMS", std::ios::app);
            sums << "2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881 missing-extra.hsaco\n";
        }
        check(!inspectNrModelAssets(full).anyReady(), "any missing manifest entry fails even with 24 other modules");
        modules(full / "HIP");
        const auto smallOnly = scratch.root / "small-only";
        makeAssets(smallOnly, true, false);
        auto partial = inspectNrModelAssets(smallOnly);
        // Pinned bridge prewarms Original during normal 1080p Small startup.
        // Small-only metadata must not be advertised as a runnable session.
        check(partial.shared.ready && !partial.smallModel.ready && !partial.originalModel.ready &&
              partial.smallModel.issue.find("Small 1080p startup also requires Original weights:") == 0,
              "Small-only folder reports its Original startup dependency");
        const auto originalOnly = scratch.root / "original-only";
        makeAssets(originalOnly, false, true);
        partial = inspectNrModelAssets(originalOnly);
        check(partial.shared.ready && !partial.smallModel.ready && partial.originalModel.ready,
              "Original does not require Small assets");
        check(findNrModelAssets(full).anyReady(), "find accepts direct Unicode assets root");
        check(findNrModelAssets(full / "small").smallModel.ready, "find normalizes selected Small subfolder");
        const auto portable = scratch.root / "portable";
        const auto managed = portable / "runtime_local" / "radeon" / "nr-assets";
        makeAssets(managed, false, true);
        check(findNrModelAssets(portable).originalModel.ready, "find discovers portable shared assets");
        const auto legacy = scratch.root / "legacy";
        makeAssets(legacy / "runtime" / "amd-nr" / "assets", true, true);
        check(findNrModelAssets(legacy).smallModel.ready, "find discovers legacy runtime assets");
        const auto removed = scratch.root / "moved-assets";
        fs::rename(managed, removed);
        check(!inspectNrModelAssets(managed).anyReady(), "removed selected folder invalidates cached-looking readiness");
        check(!findNrModelAssets(scratch.root / "does-not-exist").anyReady(), "invalid import selection rejected");
        const auto reference = scratch.root / "nr-assets-path.txt";
        std::string issue = "old issue";
        check(readNrModelAssetsReference(reference, &issue).empty() && issue.empty(),
              "missing reference means no selection and clears old error");
        const auto utf8 = full.generic_u8string();
        const std::string pathText(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        textFile(reference, pathText + "\r\n");
        check(readNrModelAssetsReference(reference, &issue) == full.lexically_normal() && issue.empty(),
              "Unicode absolute path reference round-trips with Windows line ending");
        const auto gone = managed.generic_u8string();
        textFile(reference, std::string(reinterpret_cast<const char*>(gone.data()), gone.size()));
        check(readNrModelAssetsReference(reference, &issue) == managed.lexically_normal() && issue.empty(),
              "removed selected path remains explicit instead of silently falling back");
        for (const auto& bad : std::vector<std::string>{"", "relative/assets", "C:/one\nC:/two", std::string("C:/one\0hidden", 13)}) {
            textFile(reference, bad);
            check(readNrModelAssetsReference(reference, &issue).empty() && !issue.empty(),
                  "malformed or relative saved reference rejected");
        }
        textFile(reference, std::string(32769, 'x'));
        check(readNrModelAssetsReference(reference, &issue).empty() && !issue.empty(),
              "oversized saved reference rejected without unbounded read");
        textFile(reference, pathText);
        check(readNrModelAssetsReference(reference, &issue) == full.lexically_normal() && issue.empty(),
              "valid replacement clears previous malformed reference error");

    } catch (const std::exception& error) {
        ++failed; std::cerr << "FAIL unexpected fixture exception: " << error.what() << '\n';
    }
    std::cout << "NR asset metadata: " << checks << " checks, " << failed
              << " failures. Synthetic sparse files only; no HIP or neural inference measured.\n";
    return failed ? 1 : 0;
}
