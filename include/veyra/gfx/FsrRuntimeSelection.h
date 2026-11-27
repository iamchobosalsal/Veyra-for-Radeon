#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace veyra::gfx {

inline constexpr char fsrOpenSourceLibrary[] = "amd_fidelityfx_dx12.dll";
// SDK 1.1.4 identifies FG by the frame-interpolation component's 1.1.3
// version, while its upscaler identifies itself as FSR 3.1.4.
inline constexpr uint64_t fsrOpenFgProviderId = 0xF600000000401003ull;
inline constexpr uint64_t fsrOpenSrProviderId = 0xF5A5CA1E00C01004ull;
inline constexpr std::string_view fsrOpenFgProviderVersion = "1.1.3";
inline constexpr std::string_view fsrOpenSrProviderVersion = "3.1.4";

struct FsrRuntimeSelection {
    bool openSource = false;
    bool upscale = false;
    bool frameGeneration = false;
    bool mlFrameGeneration = false;
};

constexpr FsrRuntimeSelection selectFsrRuntime(bool openSource, bool loader,
                                               bool upscale, bool frameGeneration) {
    // A present public runtime is authoritative. A damaged or unsupported
    // monolith must fail explicitly, not silently switch to another provider.
    if (openSource) return {true, true, true, false};
    return {false, loader && upscale, loader && frameGeneration,
            loader && frameGeneration};
}

inline FsrRuntimeSelection findFsrRuntime(const std::filesystem::path& root) {
    const auto exists = [&](const char* name) {
        std::error_code ec;
        return std::filesystem::is_regular_file(root / name, ec);
    };
    return selectFsrRuntime(exists(fsrOpenSourceLibrary),
        exists("amd_fidelityfx_loader_dx12.dll"),
        exists("amd_fidelityfx_upscaler_dx12.dll"),
        exists("amd_fidelityfx_framegeneration_dx12.dll"));
}

} // namespace veyra::gfx
