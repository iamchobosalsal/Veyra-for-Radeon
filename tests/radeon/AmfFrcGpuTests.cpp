// SPDX-License-Identifier: GPL-3.0-only
// Isolated test of the production AMF backend. No window, capture device,
// user settings, NGX, NVOF, Qt, FFmpeg or proprietary NVIDIA SDK is needed.
// CPU waits/readback below are test-only; this is not a throughput benchmark.
#include "veyra/gfx/AmfFrcBackend.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <cmath>
#include <iostream>

using Microsoft::WRL::ComPtr;
using namespace veyra;

int main() {
    constexpr unsigned W = 640, H = 360, pitch = W * 4;
    ComPtr<ID3D12Debug> debug;
    const bool debugAvailable = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    if (debugAvailable) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 2;
    ComPtr<ID3D12Device> device;
    for (UINT i = 0; !device; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&adapter)))) break;
        DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
        if (desc.VendorId != 0x1002 || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)))) {
            std::wcout << L"AMF_ADAPTER " << desc.Description << L" deviceId=" << desc.DeviceId << '\n';
        }
    }
    if (!device) { std::cout << "SKIP no AMD D3D12 device\n"; return 77; }
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue))) ||
        FAILED(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return 2;
    list->Close();
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return 2;
    UINT64 serial = 0;
    const auto begin = [&] { return SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr)); };
    const auto submit = [&] {
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1, lists);
        return SUCCEEDED(queue->Signal(fence.Get(), ++serial)) &&
            SUCCEEDED(fence->SetEventOnCompletion(serial, event)) && WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
    };
    auto color = pipeline::makeTexture(device.Get(), W, H, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    auto output = pipeline::makeTexture(device.Get(), W, H, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    auto upload = pipeline::makeUploadBuffer(device.Get(), uint64_t(pitch) * H);
    if (!color || !output || !upload) return 2;
    ComPtr<ID3D12Resource> readback;
    auto bd = upload->GetDesc(); D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)))) return 2;
    uint8_t* pixels = nullptr;
    if (FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&pixels)))) return 2;
    pipeline::StateTracker states;
    unsigned samples = 0, midpointFailures = 0, warmupFailures = 0;
    bool ok = true;
    for (unsigned fps : {30u, 60u}) {
        gfx::AmfFrcBackend frc;
        if (!frc.initialize(device.Get(), queue.Get(), W, H, DXGI_FORMAT_R8G8B8A8_UNORM, false)) {
            ok = false; break;
        }
        for (unsigned frame = 0; ok && frame < 24; ++frame) {
            const unsigned phase = frame % 12, left = 80 + phase * 16;
            const bool reset = phase == 0;
            for (unsigned y = 0; y < H; ++y) for (unsigned x = 0; x < W; ++x) {
                const size_t at = size_t(y) * pitch + x * 4;
                const uint8_t v = y >= 140 && y < 220 && x >= left && x < left + 80 ? 240 : 32;
                pixels[at] = pixels[at+1] = pixels[at+2] = v; pixels[at+3] = 255;
            }
            if (!begin()) { ok = false; break; }
            states.transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
            D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
            src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, pitch};
            dst.pResource = color.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            states.transition(list.Get(), color.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
            ok = frc.capture(list.Get(), color.Get()) && submit() &&
                frc.generate(int64_t(frame) * 10000000 / fps, reset);
            if (!ok || !begin()) { ok = false; break; }
            states.transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
            const bool copied = frc.copyOutput(list.Get(), output.Get());
            if (copied) {
                states.transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
                src = {}; src.pResource = output.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst = {}; dst.pResource = readback.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, W, H, 1, pitch};
                list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            ok = submit(); frc.submitted();
            if (frc.warmingUp() != (phase < 2)) ++warmupFailures;
            if (!ok || phase < 2) continue;
            ++samples;
            if (!copied) { ++midpointFailures; continue; }
            const uint8_t* actual = nullptr; D3D12_RANGE range{0, size_t(pitch)*H};
            if (FAILED(readback->Map(0, &range, reinterpret_cast<void**>(const_cast<uint8_t**>(&actual))))) { ok = false; break; }
            unsigned bright = 0; uint64_t sum = 0;
            for (unsigned y = 156; y < 204; ++y) for (unsigned x = 0; x < W; ++x)
                if (actual[size_t(y)*pitch+x*4] > 200) { ++bright; sum += x; }
            const double center = bright ? double(sum)/bright : -10000;
            const double error = std::abs(center - (left + 39.5 - 8));
            // A blend can have the right centroid but two dim edges: also
            // require nearly the full 80-pixel bright bar, not its intersection.
            const double width = double(bright)/48;
            if (error > 3 || width < 74 || width > 86) ++midpointFailures;
            std::cout << "AMF_SAMPLE fps=" << fps << " frame=" << frame << " midpointError=" << error
                      << " brightWidth=" << width << " warmingUp=" << frc.warmingUp() << '\n';
            D3D12_RANGE written{0, 0}; readback->Unmap(0, &written);
        }
        frc.shutdown();
    }
    upload->Unmap(0, nullptr);
    ComPtr<ID3D12InfoQueue> info; device.As(&info);
    unsigned debugErrors = 0;
    if (info) for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0; info->GetMessage(i, nullptr, &size); std::vector<uint8_t> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        if (SUCCEEDED(info->GetMessage(i, message, &size)) && message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            ++debugErrors; std::cout << "GPU_ERROR " << message->pDescription << '\n';
        }
    }
    CloseHandle(event);
    ok = ok && samples == 40 && !midpointFailures && !warmupFailures && !debugErrors;
    std::cout << "AMF_GPU pass=" << ok << " samples=" << samples << " midpointFailures=" << midpointFailures
              << " warmupFailures=" << warmupFailures << " debugAvailable=" << debugAvailable
              << " debugErrors=" << debugErrors << " performance=unmeasured\n";
    return ok ? 0 : 1;
}
