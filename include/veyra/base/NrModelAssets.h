// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <filesystem>
#include <string>
namespace veyra::base {
struct NrModelAssetStatus { bool ready=false; std::string issue; };
struct NrModelAssets {
    std::filesystem::path directory;
    NrModelAssetStatus shared,smallModel,originalModel;
    bool anyReady() const { return smallModel.ready || originalModel.ready; }
};
// Only for creation of a new layer. Existing layer choices are never migrated.
constexpr bool nrPerformanceForNewLayer(bool preferred,bool smallAvailable,bool originalAvailable) {
    if(preferred?smallAvailable:originalAvailable)return preferred;
    if(preferred?originalAvailable:smallAvailable)return !preferred;
    return preferred;
}
// Metadata/readability checks, not hashes or proof that GPU inference will succeed.
NrModelAssets inspectNrModelAssets(const std::filesystem::path& directory);
// Bounded discovery: asset root, small/weights subfolder, or an old portable root.
NrModelAssets findNrModelAssets(const std::filesystem::path& selectedFolder);
// Missing file means no selection. Invalid content sets issue. A removed selected
// directory is still returned: never silently fall back to another model folder.
std::filesystem::path readNrModelAssetsReference(const std::filesystem::path& file,
                                                std::string* issue=nullptr);
}
