// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "veyra/pipeline/AmdNrBackend.h"
#include <filesystem>
#include <memory>

namespace veyra::pipeline {
// Experimental, source-built DLSSNR-AMD adapter. No game hook or NeuralHub.
// Linear RGBA16F in/out. Caller submits input commands before enqueue(), then
// submits output commands before retireSubmitted(). Same D3D12 queue throughout.
// Queue waits are GPU-side; close() alone may wait on CPU for safe destruction.
class OpenNrBackend final : public AmdNrBackend {
public:
    OpenNrBackend();
    ~OpenNrBackend() override;
    OpenNrBackend(const OpenNrBackend&) = delete;
    OpenNrBackend& operator=(const OpenNrBackend&) = delete;
    bool open(const std::filesystem::path& root, ID3D12Device*, ID3D12CommandQueue*,
              uint32_t width, uint32_t height, bool small, bool temporal = true,
              float modelScale = 1.0f, bool veyraParity = false);
    bool close();
    bool ready() const override;
    bool admits(uint32_t, uint32_t) const override;
    bool recordInputs(ID3D12GraphicsCommandList*, ID3D12Resource*, uint64_t, bool, float) override;
    bool enqueue() override;
    bool recordOutputs(ID3D12GraphicsCommandList*, ID3D12Resource*) override;
    bool retireSubmitted() override;
    const std::string& error() const override;
    float gpuMilliseconds() const;
    float networkMilliseconds() const;
    bool historyConsumed() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string error_;
};
}
