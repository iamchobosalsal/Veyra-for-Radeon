// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "veyra/pipeline/GpuPassUtils.h"
#include <filesystem>

namespace veyra::pipeline {
// The same proxy/resolve shaders as NVIDIA NR in EnhanceGraph. Resources and
// descriptors stay fixed for a session; all frame work stays on the GPU.
class NrParityPass {
public:
    bool open(ID3D12Device*, const std::filesystem::path& shaders, uint32_t, uint32_t);
    void encode(ID3D12GraphicsCommandList*, ID3D12Resource* linearInput);
    void decode(ID3D12GraphicsCommandList*, ID3D12Resource* destination, bool identity);
    ID3D12Resource* proxy() const { return proxy_.Get(); }
    ID3D12Resource* neural() const { return neural_.Get(); }
    void neuralToCopyDest(ID3D12GraphicsCommandList* list) { states_.transition(list, neural_.Get(), D3D12_RESOURCE_STATE_COPY_DEST); }
private:
    uint32_t width_=0,height_=0;
    ComputePass encode_,decode_;
    ComPtr<ID3D12Resource> linear_,proxy_,neural_,decoded_;
    StateTracker states_;
};
}
