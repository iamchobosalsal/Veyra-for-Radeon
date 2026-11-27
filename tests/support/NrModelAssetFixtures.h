// SPDX-License-Identifier: GPL-3.0-only
#pragma once
// Synthetic sparse metadata fixtures from pinned Lmxxf 0.44 (4764e4f).
// No real model contents; these fixtures must never be used for neural inference.
#include <windows.h>
#include <winioctl.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>
namespace veyra::tests::nrassets {
namespace fs = std::filesystem;
struct Tensor { const char* name; uint64_t f16Bytes; };
constexpr Tensor smallTensors[] = {
    {"block0-attention", 16450ull},
    {"block0-ffn", 17472ull},
    {"block1-attention", 16450ull},
    {"block1-ffn", 17472ull},
    {"block14-attention", 164104ull},
    {"block14-ds", 65536ull},
    {"block14-ffn", 295168ull},
    {"block15-attention", 590352ull},
    {"block15-ffn", 1180160ull},
    {"block22-attention", 590352ull},
    {"block22-ds", 262144ull},
    {"block22-ffn", 1180160ull},
    {"block23-attention", 2229280ull},
    {"block23-ffwd", 1048576ull},
    {"block23-ffwd-projection", 525312ull},
    {"block30-attention", 2229280ull},
    {"block30-ffwd", 1048576ull},
    {"block30-ffwd-projection", 525312ull},
    {"block31-contract", 8390656ull},
    {"block31-expand", 8388608ull},
    {"block31-projection", 2099200ull},
    {"block31-qkv", 6291520ull},
    {"block32-contract", 8390656ull},
    {"block32-expand", 8388608ull},
    {"block32-projection", 2099200ull},
    {"block32-qkv", 6291520ull},
    {"block33-contract", 8390656ull},
    {"block33-expand", 8388608ull},
    {"block33-projection", 2099200ull},
    {"block33-qkv", 6291520ull},
    {"block34-contract", 8390656ull},
    {"block34-expand", 8388608ull},
    {"block34-projection", 2099200ull},
    {"block34-qkv", 6291520ull},
    {"block35-contract", 8390656ull},
    {"block35-expand", 8388608ull},
    {"block35-projection", 2099200ull},
    {"block35-qkv", 6291520ull},
    {"block36-contract", 8390656ull},
    {"block36-expand", 8388608ull},
    {"block36-projection", 2099200ull},
    {"block36-qkv", 6291520ull},
    {"block37-contract", 8390656ull},
    {"block37-expand", 8388608ull},
    {"block37-projection", 2099200ull},
    {"block37-qkv", 6291520ull},
    {"block38-contract", 8390656ull},
    {"block38-expand", 8388608ull},
    {"block38-projection", 2099200ull},
    {"block38-qkv", 6291520ull},
    {"block4-attention", 16450ull},
    {"block4-ds", 4096ull},
    {"block4-ffn", 17472ull},
    {"block40-attention", 2229280ull},
    {"block40-ffwd", 1048576ull},
    {"block40-ffwd-projection", 525312ull},
    {"block47-attention", 2229280ull},
    {"block47-ffwd", 1048576ull},
    {"block47-ffwd-projection", 525312ull},
    {"block48-attention", 590352ull},
    {"block48-ffn", 1180160ull},
    {"block48-weights", 262656ull},
    {"block5-attention", 49284ull},
    {"block5-ffn", 73856ull},
    {"block55-attention", 590352ull},
    {"block55-ffn", 1180160ull},
    {"block56-attention", 164104ull},
    {"block56-ffn", 295168ull},
    {"block56-weights", 65792ull},
    {"block61-attention", 164104ull},
    {"block61-ffn", 295168ull},
    {"block62-attention", 49284ull},
    {"block62-ffn", 73856ull},
    {"block62-weights", 16512ull},
    {"block65-attention", 49284ull},
    {"block65-ffn", 73856ull},
    {"block66-attention", 16450ull},
    {"block66-ffn", 17472ull},
    {"block66-weights", 4160ull},
    {"block69-attention", 16450ull},
    {"block69-ffn", 17472ull},
    {"block8-attention", 49284ull},
    {"block8-ds", 16384ull},
    {"block8-ffn", 73856ull},
    {"block9-attention", 164104ull},
    {"block9-ffn", 295168ull},
    {"decoder39-weights", 1049600ull},
    {"head-matrix", 1048576ull},
    {"post70-attention", 16450ull},
    {"post70-ffn", 17472ull},
    {"post70-head", 192ull},
    {"post70-scales", 128ull},
};
constexpr Tensor originalTensors[] = {
    {"block0-attention", 16450ull},
    {"block0-ffn", 17472ull},
    {"block1-attention", 16450ull},
    {"block1-ffn", 17472ull},
    {"block10-attention", 164104ull},
    {"block10-ffn", 295168ull},
    {"block11-attention", 164104ull},
    {"block11-ffn", 295168ull},
    {"block12-attention", 164104ull},
    {"block12-ffn", 295168ull},
    {"block13-attention", 164104ull},
    {"block13-ffn", 295168ull},
    {"block14-attention", 164104ull},
    {"block14-ds", 65536ull},
    {"block14-ffn", 295168ull},
    {"block15-attention", 590352ull},
    {"block15-ffn", 1180160ull},
    {"block16-attention", 590352ull},
    {"block16-ffn", 1180160ull},
    {"block17-attention", 590352ull},
    {"block17-ffn", 1180160ull},
    {"block18-attention", 590352ull},
    {"block18-ffn", 1180160ull},
    {"block19-attention", 590352ull},
    {"block19-ffn", 1180160ull},
    {"block2-attention", 16450ull},
    {"block2-ffn", 17472ull},
    {"block20-attention", 590352ull},
    {"block20-ffn", 1180160ull},
    {"block21-attention", 590352ull},
    {"block21-ffn", 1180160ull},
    {"block22-attention", 590352ull},
    {"block22-ds", 262144ull},
    {"block22-ffn", 1180160ull},
    {"block23-attention", 2229280ull},
    {"block23-ffwd", 1048576ull},
    {"block23-ffwd-projection", 525312ull},
    {"block24-attention", 2229280ull},
    {"block24-ffwd", 1048576ull},
    {"block24-ffwd-projection", 525312ull},
    {"block25-attention", 2229280ull},
    {"block25-ffwd", 1048576ull},
    {"block25-ffwd-projection", 525312ull},
    {"block26-attention", 2229280ull},
    {"block26-ffwd", 1048576ull},
    {"block26-ffwd-projection", 525312ull},
    {"block27-attention", 2229280ull},
    {"block27-ffwd", 1048576ull},
    {"block27-ffwd-projection", 525312ull},
    {"block28-attention", 2229280ull},
    {"block28-ffwd", 1048576ull},
    {"block28-ffwd-projection", 525312ull},
    {"block29-attention", 2229280ull},
    {"block29-ffwd", 1048576ull},
    {"block29-ffwd-projection", 525312ull},
    {"block3-attention", 16450ull},
    {"block3-ffn", 17472ull},
    {"block30-attention", 2229280ull},
    {"block30-ffwd", 1048576ull},
    {"block30-ffwd-projection", 525312ull},
    {"block31-contract", 8390656ull},
    {"block31-expand", 8388608ull},
    {"block31-projection", 2099200ull},
    {"block31-qkv", 6291520ull},
    {"block32-contract", 8390656ull},
    {"block32-expand", 8388608ull},
    {"block32-projection", 2099200ull},
    {"block32-qkv", 6291520ull},
    {"block33-contract", 8390656ull},
    {"block33-expand", 8388608ull},
    {"block33-projection", 2099200ull},
    {"block33-qkv", 6291520ull},
    {"block34-contract", 8390656ull},
    {"block34-expand", 8388608ull},
    {"block34-projection", 2099200ull},
    {"block34-qkv", 6291520ull},
    {"block35-contract", 8390656ull},
    {"block35-expand", 8388608ull},
    {"block35-projection", 2099200ull},
    {"block35-qkv", 6291520ull},
    {"block36-contract", 8390656ull},
    {"block36-expand", 8388608ull},
    {"block36-projection", 2099200ull},
    {"block36-qkv", 6291520ull},
    {"block37-contract", 8390656ull},
    {"block37-expand", 8388608ull},
    {"block37-projection", 2099200ull},
    {"block37-qkv", 6291520ull},
    {"block38-contract", 8390656ull},
    {"block38-expand", 8388608ull},
    {"block38-projection", 2099200ull},
    {"block38-qkv", 6291520ull},
    {"block4-attention", 16450ull},
    {"block4-ds", 4096ull},
    {"block4-ffn", 17472ull},
    {"block40-attention", 2229280ull},
    {"block40-ffwd", 1048576ull},
    {"block40-ffwd-projection", 525312ull},
    {"block41-attention", 2229280ull},
    {"block41-ffwd", 1048576ull},
    {"block41-ffwd-projection", 525312ull},
    {"block42-attention", 2229280ull},
    {"block42-ffwd", 1048576ull},
    {"block42-ffwd-projection", 525312ull},
    {"block43-attention", 2229280ull},
    {"block43-ffwd", 1048576ull},
    {"block43-ffwd-projection", 525312ull},
    {"block44-attention", 2229280ull},
    {"block44-ffwd", 1048576ull},
    {"block44-ffwd-projection", 525312ull},
    {"block45-attention", 2229280ull},
    {"block45-ffwd", 1048576ull},
    {"block45-ffwd-projection", 525312ull},
    {"block46-attention", 2229280ull},
    {"block46-ffwd", 1048576ull},
    {"block46-ffwd-projection", 525312ull},
    {"block47-attention", 2229280ull},
    {"block47-ffwd", 1048576ull},
    {"block47-ffwd-projection", 525312ull},
    {"block48-attention", 590352ull},
    {"block48-ffn", 1180160ull},
    {"block48-weights", 262656ull},
    {"block49-attention", 590352ull},
    {"block49-ffn", 1180160ull},
    {"block5-attention", 49284ull},
    {"block5-ffn", 73856ull},
    {"block50-attention", 590352ull},
    {"block50-ffn", 1180160ull},
    {"block51-attention", 590352ull},
    {"block51-ffn", 1180160ull},
    {"block52-attention", 590352ull},
    {"block52-ffn", 1180160ull},
    {"block53-attention", 590352ull},
    {"block53-ffn", 1180160ull},
    {"block54-attention", 590352ull},
    {"block54-ffn", 1180160ull},
    {"block55-attention", 590352ull},
    {"block55-ffn", 1180160ull},
    {"block56-attention", 164104ull},
    {"block56-ffn", 295168ull},
    {"block56-weights", 65792ull},
    {"block57-attention", 164104ull},
    {"block57-ffn", 295168ull},
    {"block58-attention", 164104ull},
    {"block58-ffn", 295168ull},
    {"block59-attention", 164104ull},
    {"block59-ffn", 295168ull},
    {"block6-attention", 49284ull},
    {"block6-ffn", 73856ull},
    {"block60-attention", 164104ull},
    {"block60-ffn", 295168ull},
    {"block61-attention", 164104ull},
    {"block61-ffn", 295168ull},
    {"block62-attention", 49284ull},
    {"block62-ffn", 73856ull},
    {"block62-weights", 16512ull},
    {"block63-attention", 49284ull},
    {"block63-ffn", 73856ull},
    {"block64-attention", 49284ull},
    {"block64-ffn", 73856ull},
    {"block65-attention", 49284ull},
    {"block65-ffn", 73856ull},
    {"block66-attention", 16450ull},
    {"block66-ffn", 17472ull},
    {"block66-weights", 4160ull},
    {"block67-attention", 16450ull},
    {"block67-ffn", 17472ull},
    {"block68-attention", 16450ull},
    {"block68-ffn", 17472ull},
    {"block69-attention", 16450ull},
    {"block69-ffn", 17472ull},
    {"block7-attention", 49284ull},
    {"block7-ffn", 73856ull},
    {"block8-attention", 49284ull},
    {"block8-ds", 16384ull},
    {"block8-ffn", 73856ull},
    {"block9-attention", 164104ull},
    {"block9-ffn", 295168ull},
    {"decoder39-weights", 1049600ull},
    {"head-matrix", 1048576ull},
    {"post70-attention", 16450ull},
    {"post70-ffn", 17472ull},
    {"post70-head", 192ull},
    {"post70-scales", 128ull},
};

constexpr const char* codecs[] = {
    "native_codec_encode.hlsl", "native_codec_decode.hlsl", "native_game_rgb_input.hlsl"
};
constexpr const char* kernels[] = {
    "c32_prefix_reference.hsaco", "multihead-reference.hsaco",
    "deep_reference.hsaco", "boundary_reference.hsaco"
};
inline void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
inline void textFile(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
    require(bool(stream), "fixture text write failed");
}
inline void sparseFile(const fs::path& path, uint64_t bytes) {
    fs::create_directories(path.parent_path());
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    require(file != INVALID_HANDLE_VALUE, "fixture sparse file create failed");
    DWORD returned = 0;
    const bool sparse = DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0,
                                       nullptr, 0, &returned, nullptr) != 0;
    LARGE_INTEGER end{}; end.QuadPart = static_cast<LONGLONG>(bytes);
    const bool sized = sparse && SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && SetEndOfFile(file);
    CloseHandle(file);
    require(sized, "fixture requires sparse-file support; no large zero-filled allocation attempted");
}
inline void smallManifest(const fs::path& root) {
    textFile(root / "small" / "model-manifest.json",
        "{\"model\": \"small\", \"sha256\": \"2bc19bc09a7b89dd44f73e2ac0108fcc586e2154bdd0830f242f4f040e28789f\"}\n");
}
inline void modules(const fs::path& directory, const std::string& architecture = {}) {
    const auto target = architecture.empty() ? directory : directory / architecture;
    std::vector<std::string> names;
    for (const auto* name : kernels) names.emplace_back(name);
    for (unsigned i = 4; i < 24; ++i) names.emplace_back("fixture-" + std::to_string(i) + ".hsaco");
    std::string sums;
    for (const auto& name : names) {
        // Real bytes with a real SHA-256, but deliberately not an executable kernel.
        textFile(target / name, "x");
        sums += "2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881 "
             + (architecture.empty() ? name : architecture + "/" + name) + "\n";
    }
    textFile(directory / "SHA256SUMS", sums);
}
inline void makeAssets(const fs::path& root, bool withSmall, bool withOriginal) {
    fs::create_directories(root);
    for (const auto* codec : codecs) textFile(root / "shaders" / codec, "// synthetic codec fixture\n");
    modules(root / "HIP");
    // Upstream FindWeightsDir needs the Original prefix sentinel even for Small.
    sparseFile(root / "block0-ffn.f16", 8736ull * 2);
    if (withOriginal) for (const auto& tensor : originalTensors)
        sparseFile(root / (std::string(tensor.name) + ".f16"), tensor.f16Bytes);
    if (withSmall) {
        smallManifest(root);
        for (const auto& tensor : smallTensors)
            sparseFile(root / "small" / (std::string(tensor.name) + ".f16"), tensor.f16Bytes);
    }
}
struct Scratch {
    fs::path parent = fs::weakly_canonical(fs::temp_directory_path());
    fs::path root = parent / ("veyra-nr-assets-tests-" + std::to_string(GetCurrentProcessId())
                              + "-" + std::to_string(GetTickCount64()));
    Scratch() { require(fs::create_directory(root), "unique fixture directory already exists"); }
    ~Scratch() {
        // Delete only this test's freshly created, absolute direct child of temp.
        std::error_code error;
        const auto resolved = fs::weakly_canonical(root, error);
        if (!error && resolved.parent_path() == parent &&
            resolved.filename().wstring().find(L"veyra-nr-assets-tests-") == 0)
            fs::remove_all(resolved, error);
    }
};
}
