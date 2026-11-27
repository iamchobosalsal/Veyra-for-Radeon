// SPDX-License-Identifier: GPL-3.0-only
// Real context + static image dispatch test, not a video-quality benchmark.
#include "veyra/gfx/FsrSrBackend.h"
#include "veyra/RuntimePaths.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstring>
#include <iostream>
#include <string_view>

namespace {
// Test-only CPU upload, wait and readback. The production backend remains GPU-only.
bool staticImage(ID3D12Device* device, veyra::gfx::FsrSrBackend& sr, unsigned w, unsigned h) {
    using Microsoft::WRL::ComPtr;
    using namespace veyra::pipeline;
    constexpr unsigned OW = 3840, OH = 2160;
    const unsigned pitch = (w * 4 + 255) & ~255u;
    const UINT64 planeSize = UINT64(pitch) * h;
    auto color = makeTexture(device, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    auto motion = makeTexture(device, w, h, DXGI_FORMAT_R16G16_FLOAT, false);
    auto depth = makeTexture(device, w, h, DXGI_FORMAT_R32_FLOAT, false);
    auto output = makeTexture(device, OW, OH, DXGI_FORMAT_R8G8B8A8_UNORM, true);
    auto upload = makeUploadBuffer(device, planeSize * 3);
    if (!color || !motion || !depth || !output || !upload) return false;
    ComPtr<ID3D12Resource> readback;
    auto buffer = upload->GetDesc(); buffer.Width = UINT64(OW) * OH * 4;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return false;
    uint8_t* pixels = nullptr;
    if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&pixels)))) return false;
    std::memset(pixels, 0, size_t(planeSize * 3));
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        const size_t at = size_t(y) * pitch + x * 4;
        const uint8_t value = ((x/64 + y/64) % 2) ? 32 : 224;
        pixels[at] = pixels[at+1] = pixels[at+2] = value; pixels[at+3] = 255;
        const float z = .9f; std::memcpy(pixels + 2 * planeSize + at, &z, sizeof(z));
    }
    upload->Unmap(0, nullptr);
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    D3D12_COMMAND_QUEUE_DESC q{};
    if (FAILED(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    StateTracker states;
    ID3D12Resource* inputs[]{color.Get(), motion.Get(), depth.Get()};
    const DXGI_FORMAT formats[]{DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT};
    for (unsigned i = 0; i < 3; ++i) {
        states.transition(list.Get(), inputs[i], D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = planeSize * i;
        src.PlacedFootprint.Footprint = {formats[i], w, h, 1, pitch};
        dst.pResource = inputs[i]; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        states.transition(list.Get(), inputs[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    states.transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (!sr.evaluate(list.Get(), color.Get(), motion.Get(), depth.Get(), output.Get(), w, h, OW, OH, true, 1000.f/60)) return false;
    states.transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = output.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, OW, OH, 1, OW*4};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    if (FAILED(list->Close())) return false;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
    const bool completed = SUCCEEDED(queue->Signal(fence.Get(), 1)) &&
        SUCCEEDED(fence->SetEventOnCompletion(1, event)) && WaitForSingleObject(event, 10000) == WAIT_OBJECT_0;
    CloseHandle(event);
    if (!completed) return false;
    const uint8_t* actual = nullptr; D3D12_RANGE range{0, size_t(buffer.Width)};
    if (FAILED(readback->Map(0, &range, reinterpret_cast<void**>(const_cast<uint8_t**>(&actual))))) return false;
    double light = 0, dark = 0; unsigned lights = 0, darks = 0;
    for (unsigned y = 32; y < h; y += 64) for (unsigned x = 32; x < w; x += 64) {
        const unsigned value = actual[(size_t(y) * OH / h * OW + size_t(x) * OW / w) * 4 + 1];
        if ((x/64 + y/64) % 2) { dark += value; ++darks; } else { light += value; ++lights; }
    }
    D3D12_RANGE written{0, 0}; readback->Unmap(0, &written);
    const double contrast = light/lights - dark/darks;
    const bool ok = contrast > 100;
    std::cout << "FSR_STATIC_IMAGE input=" << w << 'x' << h << " output=3840x2160 contrast=" << contrast
              << " pass=" << ok << " temporalQuality=unverified performance=unmeasured\n";
    return ok;
}
}

int main() {
    using Microsoft::WRL::ComPtr;
    using namespace veyra;
    if (!std::filesystem::exists(runtime::localDataDirectory() / "amd/fidelityfx/amd_fidelityfx_loader_dx12.dll")) {
        std::cout << "SKIP stage official FidelityFX runtimes in the isolated build/runtime_local/amd/fidelityfx directory\n";
        return 77;
    }
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 2;
    ComPtr<ID3D12Device> device;
    for (UINT i = 0; !device; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) break;
        DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
        if (d.VendorId != 0x1002 || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
            std::wcout << L"FSR_ADAPTER " << d.Description << '\n';
    }
    if (!device) return 77;
    unsigned failures = 0, checks = 0;
    const auto check = [&](bool ok) { ++checks; if (!ok) ++failures; };
    gfx::FsrSrBackend sr;
    check(sr.compiledIn());
    check(!sr.initialize(nullptr, 1920, 1080, 3840, 2160, false));
    check(!sr.initialize(device.Get(), 0, 1080, 3840, 2160, false));
    for (bool hdr : {false, true}) for (unsigned height : {1080u, 1440u}) {
        check(sr.initialize(device.Get(), height * 16 / 9, height, 3840, 2160, hdr));
        check(sr.created());
        check(!std::string_view(sr.providerVersion()).empty());
        std::cout << "FSR_CONTEXT input=" << height * 16 / 9 << 'x' << height
                  << " output=3840x2160 hdr=" << hdr << " actualProvider=" << sr.providerVersion() << '\n';
        if (!hdr && sr.created()) check(staticImage(device.Get(), sr, height * 16 / 9, height));
        sr.release(); sr.release();
        check(!sr.created() && std::string_view(sr.providerVersion()).empty());
    }
    std::cout << "FSR_LIFECYCLE checks=" << checks << " failures=" << failures
              << " temporalQuality=unverified hdrDispatch=unverified performance=unmeasured\n";
    return failures ? 1 : 0;
}
