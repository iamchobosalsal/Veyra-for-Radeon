// SPDX-License-Identifier: GPL-3.0-only
#include "veyra/pipeline/OpenNrBackend.h"
#include "veyra/pipeline/NrParityPass.h"
#include "nr_runtime.hpp"
#include "nr_pe_bridge.hpp"
#include <wrl/client.h>
#include <cmath>
#include <stdexcept>

namespace veyra::pipeline {
namespace {
void transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* texture,
                D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {texture, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
    cmd->ResourceBarrier(1, &b);
}
}
struct OpenNrBackend::Impl {
    enum class Stage { Idle, Inputs, Submitted, Outputs } stage = Stage::Idle;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    HANDLE event = nullptr;
    uint64_t lastFrame = 0, fenceValue = 0, generation = 0;
    uint32_t width = 0, height = 0;
    bool temporal = false, history = false;
    bool parity = false, identity = false;
    std::unique_ptr<NrParityPass> codec;
    nr::pe::bridge::Device* bridge = nullptr;
    nr::pe::bridge::Image image;
    nr::pe::bridge::Job* job = nullptr;
    std::unique_ptr<nr::Runtime> runtime;
    ~Impl() { if (event) CloseHandle(event); }
    bool textureMatches(ID3D12Resource* texture) const {
        if (!texture) return false;
        const auto d = texture->GetDesc();
        ComPtr<ID3D12Device> owner;
        return SUCCEEDED(texture->GetDevice(IID_PPV_ARGS(&owner))) && owner.Get() == device.Get()
            && d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width == width && d.Height == height
            && d.Format == DXGI_FORMAT_R16G16B16A16_FLOAT && d.MipLevels == 1
            && d.DepthOrArraySize == 1 && d.SampleDesc.Count == 1;
    }
};
OpenNrBackend::OpenNrBackend() = default;
OpenNrBackend::~OpenNrBackend() {
    // If a device has hung, releasing resources still in GPU use is unsafe.
    // The isolated probe owns this process. Product integration needs a worker
    // watchdog before enabling the provider in the UI.
    if (!close()) (void)impl_.release();
}
bool OpenNrBackend::open(const std::filesystem::path& root, ID3D12Device* device,
                        ID3D12CommandQueue* queue, uint32_t width, uint32_t height,
                        bool small, bool temporal, float modelScale, bool veyraParity) {
    if (!close()) return false;
    error_.clear();
    try {
        if (!device || !queue || !root.is_absolute() || !width || !height || width > 3840 || height > 2160
            || !std::isfinite(modelScale) || modelScale < 0.25f || modelScale > 1.f || (veyraParity && modelScale != 1.f))
            throw std::runtime_error("invalid OpenNR device, root or extent (maximum 3840x2160)");
        ComPtr<ID3D12Device> queueDevice;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) || queueDevice.Get() != device
            || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
            throw std::runtime_error("OpenNR requires the device's direct queue");
        // Fail explicitly rather than accepting upstream's old-model fallback.
        const auto model = root / "dlssnr-amd" / (small ? "dlssnr-small.bin" : "dlssnr.bin");
        if (!std::filesystem::is_regular_file(model)) throw std::runtime_error("requested NVIDIA model is missing");
        impl_ = std::make_unique<Impl>(); auto& p = *impl_;
        p.device = device; p.queue = queue; p.width = width; p.height = height; p.temporal = temporal;
        p.parity = veyraParity;
        if(p.parity){
            p.codec=std::make_unique<NrParityPass>();
            if(!p.codec->open(device,root/L"shaders",width,height))throw std::runtime_error("Veyra NR parity shaders unavailable");
        }
        p.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!p.event || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p.fence))))
            throw std::runtime_error("OpenNR completion fence creation failed");
        p.bridge = nr::pe::bridge::Device::get(device->GetAdapterLuid(), &error_);
        if (!p.bridge) throw std::runtime_error(error_);
        if (!p.bridge->create_d3d12(device, width, height, p.parity?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R16G16B16A16_FLOAT, &p.image, &error_))
            throw std::runtime_error(error_);
        const auto h = p.bridge->handles();
        nr::HostDevice host{};
        host.instance = h.instance; host.physical = h.physical; host.device = h.device;
        host.queue = p.bridge->queue(); host.queue_family = p.bridge->family();
        host.buffer_device_address = true;
        host.queue_lock = [&p] { p.bridge->lock(); }; host.queue_unlock = [&p] { p.bridge->unlock(); };
        nr::RuntimeConfig config{};
        config.root = root.string(); config.width = width; config.height = height;
        config.colour_format = p.parity?VK_FORMAT_R8G8B8A8_UNORM:VK_FORMAT_R16G16B16A16_SFLOAT;
        config.linear_input = !p.parity; config.native_compose=p.parity;
        config.network = small ? 1 : 0; config.model_scale = modelScale;
        nr::TemporalConfig history{}; history.enable = temporal;
        p.runtime = std::make_unique<nr::Runtime>(host, config, nr::ControlMaskConfig{}, history);
        if ((p.runtime->network() == nr::NetworkModel::Small) != small)
            throw std::runtime_error("OpenNR selected a different model");
        return true;
    } catch (const std::exception& e) { error_ = e.what(); close(); return false; }
}
bool OpenNrBackend::close() {
    if (!impl_) return true;
    auto& p = *impl_;
    if (p.bridge && p.runtime) p.bridge->cancel(p.runtime.get());
    if (p.queue && p.fence && p.event) {
        if (FAILED(p.queue->Signal(p.fence.Get(), ++p.fenceValue))
            || FAILED(p.fence->SetEventOnCompletion(p.fenceValue, p.event))
            || WaitForSingleObject(p.event, 5000) != WAIT_OBJECT_0) {
            error_ = "OpenNR shutdown: GPU completion failed"; return false;
        }
    }
    if (p.bridge && !p.bridge->wait_idle()) { error_ = "OpenNR shutdown: Vulkan queue timeout"; return false; }
    p.runtime.reset();
    if (p.bridge && p.image) p.bridge->destroy(p.image);
    impl_.reset(); return true;
}
bool OpenNrBackend::ready() const { return impl_ && impl_->runtime; }
bool OpenNrBackend::admits(uint32_t w, uint32_t h) const { return ready() && impl_->width == w && impl_->height == h; }
bool OpenNrBackend::recordInputs(ID3D12GraphicsCommandList* cmd, ID3D12Resource* source,
                                uint64_t frame, bool reset, float intensity) {
    if (!ready() || !cmd || impl_->stage != Impl::Stage::Idle || !impl_->textureMatches(source)
        || !std::isfinite(intensity) || intensity < 0 || intensity > 2) {
        error_ = "OpenNR invalid input, intensity or submission order"; return false;
    }
    auto& p = *impl_;
    try {
        const bool discontinuity = reset || !p.lastFrame || frame != p.lastFrame + 1;
        p.job = p.bridge->begin(&error_, p.runtime.get());
        if (!p.job) return false;
        p.generation = p.bridge->generation(p.job);
        p.bridge->acquire(p.job, {&p.image});
        nr::ColourFrame colour{};
        colour.image = p.image.image; colour.format = p.image.format;
        colour.width = p.width; colour.height = p.height; colour.usage = p.image.usage;
        nr::Controls controls{}; controls.intensity = intensity;
        // Match Veyra's default SF controls. The released AMD ABI has no fields
        // for custom tone/structure/style; do not pretend to forward them.
        if(p.parity)controls.automatic_mask=false;
        p.identity=intensity==0;
        nr::RecordResult recorded{};
        if (p.temporal) {
            nr::TemporalFrame history{}; history.reset = discontinuity;
            const auto r = p.runtime->record_temporal(p.bridge->command_buffer(p.job), colour, controls, history);
            recorded = r.frame; p.history = r.history_consumed;
        } else recorded = p.runtime->record(p.bridge->command_buffer(p.job), colour, controls, discontinuity);
        if (!recorded.applied || !recorded.network_dispatches) throw std::runtime_error("OpenNR did not record inference");
        p.bridge->on_drop(p.job, [&p] { p.runtime->drop_history(); });
        if (!p.bridge->end(p.job, &error_)) { p.job = nullptr; return false; }
        auto* shared = static_cast<ID3D12Resource*>(p.image.d3d);
        if(p.parity){p.codec->encode(cmd,source);source=p.codec->proxy();}
        transition(cmd, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(cmd, shared, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd->CopyResource(shared, source);
        transition(cmd, shared, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        transition(cmd, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        p.stage = Impl::Stage::Inputs; p.lastFrame = frame; return true;
    } catch (const std::exception& e) {
        if (p.job) { p.bridge->abort(p.job); p.job = nullptr; }
        p.runtime->drop_history(); error_ = e.what(); return false;
    }
}
bool OpenNrBackend::enqueue() {
    if (!ready() || impl_->stage != Impl::Stage::Inputs) { error_ = "OpenNR enqueue out of order"; return false; }
    auto& p = *impl_;
    if (!p.bridge->run_between(p.queue.Get(), p.job, p.generation)) {
        error_ = "OpenNR GPU handover failed"; p.runtime->drop_history(); return false;
    }
    p.job = nullptr; p.stage = Impl::Stage::Submitted; return true;
}
bool OpenNrBackend::recordOutputs(ID3D12GraphicsCommandList* cmd, ID3D12Resource* destination) {
    if (!ready() || !cmd || impl_->stage != Impl::Stage::Submitted || !impl_->textureMatches(destination)) {
        error_ = "OpenNR invalid output or submission order"; return false;
    }
    auto& p = *impl_; auto* shared = static_cast<ID3D12Resource*>(p.image.d3d);
    auto* target=destination;
    if(p.parity){p.codec->neuralToCopyDest(cmd);target=p.codec->neural();}
    transition(cmd, shared, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmd->CopyResource(target, shared);
    transition(cmd, shared, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    if(p.parity)p.codec->decode(cmd,destination,p.identity);
    p.stage = Impl::Stage::Outputs; return true;
}
bool OpenNrBackend::retireSubmitted() {
    if (!ready() || impl_->stage != Impl::Stage::Outputs) { error_ = "OpenNR retire out of order"; return false; }
    impl_->stage = Impl::Stage::Idle; return true;
}
const std::string& OpenNrBackend::error() const { return error_; }
float OpenNrBackend::gpuMilliseconds() const { return ready() ? impl_->runtime->last_gpu_ms() : 0; }
float OpenNrBackend::networkMilliseconds() const { return ready() ? impl_->runtime->average_network_ms() : 0; }
bool OpenNrBackend::historyConsumed() const { return ready() && impl_->history; }
}
