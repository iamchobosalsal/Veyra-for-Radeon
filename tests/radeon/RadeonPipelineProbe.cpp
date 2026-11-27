// SPDX-License-Identifier: GPL-3.0-only
// Headless, synthetic backend probe; NOT a display/capture latency benchmark.
// No CPU wait between capture, generate and output submission. The per-input
// completion wait/readback is confined to this diagnostic, never the engine.
#include "veyra/gfx/AmfFrcBackend.h"
#include "veyra/gfx/FsrSrBackend.h"
#include "veyra/gfx/FsrFgPresenter.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include <dxgi1_6.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string_view>

using Microsoft::WRL::ComPtr;
using namespace veyra;
namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void checked(HRESULT hr, const char* why) { require(SUCCEEDED(hr), why); }
struct Event {
    HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~Event() { if (handle) CloseHandle(handle); }
};
double mean(const std::vector<double>& v) { return std::accumulate(v.begin(), v.end(), 0.0) / v.size(); }
double percentile95(std::vector<double> v) { std::sort(v.begin(), v.end()); return v[size_t(std::ceil(.95 * v.size())) - 1]; }

int probe(std::string_view mode, unsigned height, unsigned fps) {
    constexpr unsigned frames = 72, warmup = 12;
    const bool useSr = mode == "sr" || mode == "sr-amf";
    const bool useAmf = mode == "amf" || mode == "sr-amf";
    const bool useFsrFg = mode == "fsr3fg" || mode == "fsr4fg";
    const bool useFg = useAmf || useFsrFg;
    const unsigned w = height * 16 / 9, h = height;
    const unsigned ow = useSr ? 3840 : w, oh = useSr ? 2160 : h;
    const unsigned pitch = (w * 4 + 255) & ~255u, opitch = (ow * 4 + 255) & ~255u;
    const UINT64 plane = UINT64(pitch) * h;
    ComPtr<IDXGIFactory6> factory;
    checked(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    ComPtr<ID3D12Device> device;
    ComPtr<IDXGIAdapter3> adapter;
    DXGI_ADAPTER_DESC1 adapterDesc{};
    for (UINT i = 0; !device; ++i) {
        ComPtr<IDXGIAdapter1> candidate;
        if (FAILED(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&candidate)))) break;
        candidate->GetDesc1(&adapterDesc);
        if (adapterDesc.VendorId != 0x1002 || (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        if (SUCCEEDED(D3D12CreateDevice(candidate.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
            checked(candidate.As(&adapter), "DXGI memory query interface");
    }
    if (!device) { std::cout << "SKIP no AMD D3D12 device\n"; return 77; }
    std::wcout << L"PROBE_ADAPTER " << adapterDesc.Description << '\n';
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC q{};
    checked(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
    ComPtr<ID3D12CommandAllocator> allocator[2];
    ComPtr<ID3D12GraphicsCommandList> list[2];
    for (unsigned i = 0; i < 2; ++i) {
        checked(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator[i])), "allocator");
        checked(device->CreateCommandList(0, q.Type, allocator[i].Get(), nullptr, IID_PPV_ARGS(&list[i])), "list");
        checked(list[i]->Close(), "initial close");
    }
    ComPtr<ID3D12Fence> fence;
    checked(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
    Event event; require(event.handle != nullptr, "event");
    UINT64 serial = 0, frequency = 0;
    checked(queue->GetTimestampFrequency(&frequency), "timestamp frequency");
    ComPtr<ID3D12QueryHeap> queries;
    D3D12_QUERY_HEAP_DESC queryDesc{}; queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; queryDesc.Count = 3;
    checked(device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&queries)), "timestamp heap");
    const auto buffer = [&](UINT64 size) {
        ComPtr<ID3D12Resource> result;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC d{}; d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = size; d.Height = 1; d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        checked(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&result)), "readback");
        return result;
    };
    auto times = buffer(3 * sizeof(UINT64));
    auto pixelsBack = buffer(UINT64(opitch) * oh);
    const auto begin = [&](unsigned i) {
        checked(allocator[i]->Reset(), "allocator reset");
        checked(list[i]->Reset(allocator[i].Get(), nullptr), "list reset");
    };
    const auto submit = [&](unsigned i) {
        checked(list[i]->Close(), "list close");
        ID3D12CommandList* lists[]{list[i].Get()}; queue->ExecuteCommandLists(1, lists);
    };
    const auto complete = [&] {
        checked(queue->Signal(fence.Get(), ++serial), "completion signal");
        checked(fence->SetEventOnCompletion(serial, event.handle), "completion event");
        require(WaitForSingleObject(event.handle, 10000) == WAIT_OBJECT_0, "GPU completion timeout");
        checked(device->GetDeviceRemovedReason(), "device removed");
    };
    auto color = pipeline::makeTexture(device.Get(), w, h, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    auto motion = pipeline::makeTexture(device.Get(), w, h, DXGI_FORMAT_R16G16_FLOAT, false);
    auto depth = pipeline::makeTexture(device.Get(), w, h, DXGI_FORMAT_R32_FLOAT, false);
    auto scaled = pipeline::makeTexture(device.Get(), ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, true);
    auto generated = pipeline::makeTexture(device.Get(), ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, useFsrFg);
    auto upload = pipeline::makeUploadBuffer(device.Get(), plane * 4);
    require(color && motion && depth && scaled && generated && upload, "fixture allocation");
    uint8_t* bytes = nullptr;
    checked(upload->Map(0, nullptr, reinterpret_cast<void**>(&bytes)), "upload map");
    std::memset(bytes, 0, size_t(plane * 4));
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        const size_t at = size_t(y) * pitch + x * 4;
        const uint8_t value = (x / 64 + y / 64) % 2 ? 32 : 48;
        bytes[at] = bytes[at+1] = bytes[at+2] = value; bytes[at+3] = 255;
        const float z = .9f; std::memcpy(bytes + plane * 2 + at, &z, sizeof(z));
        bytes[plane*3+at] = bytes[plane*3+at+1] = bytes[plane*3+at+2] = 240; bytes[plane*3+at+3] = 255;
    }
    upload->Unmap(0, nullptr);
    pipeline::StateTracker states;
    const auto copyPlane = [&](unsigned index, ID3D12Resource* dstResource, DXGI_FORMAT format, unsigned dx = 0, unsigned dy = 0, const D3D12_BOX* box = nullptr) {
        states.transition(list[0].Get(), dstResource, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = upload.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = plane * index; src.PlacedFootprint.Footprint = {format, w, h, 1, pitch};
        dst.pResource = dstResource; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list[0]->CopyTextureRegion(&dst, dx, dy, 0, &src, box);
    };
    begin(0);
    copyPlane(1, motion.Get(), DXGI_FORMAT_R16G16_FLOAT);
    copyPlane(2, depth.Get(), DXGI_FORMAT_R32_FLOAT);
    states.transition(list[0].Get(), motion.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    states.transition(list[0].Get(), depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    submit(0); complete();
    gfx::FsrSrBackend sr;
    gfx::AmfFrcBackend amf;
    gfx::FsrFgPresenter fsrFg;
    if (useSr) require(sr.initialize(device.Get(), w, h, ow, oh, false), "FSR SR initialization");
    if (useAmf) require(amf.initialize(device.Get(), queue.Get(), ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, false), "AMF initialization");
    if (useFsrFg) require(fsrFg.initializeIndependent(device.Get(), ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, mode == "fsr4fg"), "FSR FG initialization");
    std::vector<double> srMs, fgMs, queueSpanMs, completionMs;
    double warmupMaxMs = 0;
    unsigned outputs = 0, measuredOutputs = 0, missedBudgets = 0;
    UINT64 peakUsage = 0;
    const unsigned step = w / 240, barWidth = w / 8;
    for (unsigned frame = 0; frame < frames; ++frame) {
        const auto started = std::chrono::steady_clock::now();
        begin(0);
        copyPlane(0, color.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
        const D3D12_BOX box{0, 0, 0, barWidth, h / 3, 1};
        copyPlane(3, color.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, w / 4 + frame * step, h / 3, &box);
        list[0]->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        ID3D12Resource* real = color.Get();
        if (useSr) {
            states.transition(list[0].Get(), color.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            states.transition(list[0].Get(), scaled.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            require(sr.evaluate(list[0].Get(), color.Get(), motion.Get(), depth.Get(), scaled.Get(), w, h, ow, oh, frame == 0, 1000.f / fps), "FSR dispatch");
            real = scaled.Get();
        }
        list[0]->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
        if (useAmf) {
            states.transition(list[0].Get(), real, D3D12_RESOURCE_STATE_COPY_SOURCE);
            require(amf.capture(list[0].Get(), real), "AMF capture");
        }
        if (useFsrFg) {
            for (auto* r : {real, motion.Get(), depth.Get(), generated.Get()})
                states.transition(list[0].Get(), r, D3D12_RESOURCE_STATE_COMMON);
            require(fsrFg.generate(list[0].Get(), real, motion.Get(), depth.Get(), generated.Get(), frame == 0, 1000.f / fps), "FSR FG dispatch");
            list[0]->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
            if (frame > 0) { ++outputs; if (frame >= warmup) ++measuredOutputs; }
        }
        submit(0);
        // The production fence protocol, with no CPU wait after capture.
        if (useAmf) require(amf.generate(int64_t(frame) * 10000000 / fps, frame == 0), "AMF generate");
        begin(1);
        if (useAmf) {
            states.transition(list[1].Get(), generated.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
            const bool copied = amf.copyOutput(list[1].Get(), generated.Get());
            if (!amf.warmingUp()) {
                require(copied, "missing AMF output after warmup");
                ++outputs; if (frame >= warmup) ++measuredOutputs;
            }
        }
        if (!useFsrFg) list[1]->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2);
        list[1]->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 3, times.Get(), 0);
        submit(1);
        if (useAmf) amf.submitted();
        complete();
        const double elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        if (frame < warmup) warmupMaxMs = std::max(warmupMaxMs, elapsed);
        UINT64* timestamps = nullptr; D3D12_RANGE read{0, 3 * sizeof(UINT64)};
        checked(times->Map(0, &read, reinterpret_cast<void**>(&timestamps)), "timestamp map");
        const double scale = 1000.0 / frequency;
        if (frame >= warmup) {
            srMs.push_back(useSr ? (timestamps[1] - timestamps[0]) * scale : 0.0);
            fgMs.push_back(useFsrFg ? (timestamps[2] - timestamps[1]) * scale : 0.0);
            queueSpanMs.push_back((timestamps[2] - timestamps[0]) * scale);
            completionMs.push_back(elapsed);
            if (elapsed > 1000.0 / fps) ++missedBudgets;
        }
        D3D12_RANGE written{0, 0}; times->Unmap(0, &written);
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
        checked(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory), "memory usage");
        peakUsage = std::max(peakUsage, memory.CurrentUsage);
    }
    // One quality sample outside the timed region. Pixel threshold detects
    // missing/blank output and gross displacement; not a game quality score.
    auto* finalImage = useFg ? generated.Get() : scaled.Get();
    begin(0);
    states.transition(list[0].Get(), finalImage, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
    src.pResource = finalImage; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.pResource = pixelsBack.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, ow, oh, 1, opitch};
    list[0]->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    submit(0); complete();
    const uint8_t* actual = nullptr; D3D12_RANGE read{0, size_t(opitch) * oh};
    checked(pixelsBack->Map(0, &read, reinterpret_cast<void**>(const_cast<uint8_t**>(&actual))), "pixel map");
    unsigned bright = 0; UINT64 sum = 0;
    for (unsigned x = 0; x < ow; ++x) if (actual[size_t(oh / 2) * opitch + x * 4] > 200) { ++bright; sum += x; }
    D3D12_RANGE written{0, 0}; pixelsBack->Unmap(0, &written);
    const double factor = double(ow) / w;
    const double expected = (w / 4 + (frames-1) * step + barWidth * .5 - (useFg ? step * .5 : 0)) * factor - .5;
    const double centerError = bright ? std::abs(double(sum) / bright - expected) : 99999;
    const double widthError = std::abs(double(bright) - barWidth * factor);
    const bool quality = centerError <= 6 * factor && widthError <= 12 * factor;
    // A repeated endpoint is half a step off. A simple 50/50 blend can have
    // the right centroid but loses one step of bright width. Reject both.
    // These tolerances are derived from the fixture motion, not each backend.
    const bool interpolation = quality && centerError <= std::max(.75, step * factor * .25)
        && widthError <= std::max(2.0, step * factor * .5);
    std::cout << std::fixed << std::setprecision(4)
        << "RADEON_PROBE {\"mode\":\"" << mode << "\",\"width\":" << w << ",\"height\":" << h
        << ",\"outputWidth\":" << ow << ",\"outputHeight\":" << oh << ",\"inputPtsFps\":" << fps
        << ",\"targetOutputFps\":" << fps * (useFg ? 2 : 1) << ",\"completedInputs\":" << frames
        << ",\"generatedOutputs\":" << outputs << ",\"measuredSamples\":" << completionMs.size()
        << ",\"measuredGeneratedOutputs\":" << measuredOutputs << ",\"srGpuMeanMs\":" << mean(srMs)
        << ",\"srGpuP95Ms\":" << percentile95(srMs) << ",\"fsrFgGpuMeanMs\":" << mean(fgMs)
        << ",\"queueSpanMeanMs\":" << mean(queueSpanMs) << ",\"excludedWarmupMaxMs\":" << warmupMaxMs
        << ",\"completionMeanMs\":" << mean(completionMs) << ",\"completionP95Ms\":" << percentile95(completionMs)
        << ",\"inputBudgetMisses\":" << missedBudgets << ",\"processLocalMemoryPeakMiB\":" << peakUsage / (1024.0 * 1024)
        << ",\"centerErrorPixels\":" << centerError << ",\"brightWidthErrorPixels\":" << widthError
        << ",\"qualitySmokePass\":" << (quality ? "true" : "false")
        << ",\"interpolationEvidencePass\":" << (useFg ? interpolation ? "true" : "false" : "null")
        << ",\"provider\":\"" << (useSr ? sr.providerVersion() : "none")
        << "\",\"fgProvider\":\"" << (useFsrFg ? fsrFg.providerVersion() : useAmf ? amf.runtimeVersion() : "none")
        << "\",\"motionInput\":\"zero\",\"depthInput\":0.9,\"paced\":false,\"displayFps\":null,\"audioSync\":null,\"endToEndLatencyMs\":null}\n";
    return quality && (!useFg || interpolation) ? 0 : 1;
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 4, "usage: radeon_pipeline_probe <sr|amf|sr-amf|fsr3fg|fsr4fg> <1080|1440> <30|60>");
        const std::string_view mode = argv[1];
        require(mode == "sr" || mode == "amf" || mode == "sr-amf" || mode == "fsr3fg" || mode == "fsr4fg", "invalid mode");
        const unsigned height = std::stoul(argv[2]), fps = std::stoul(argv[3]);
        require((height == 1080 || height == 1440) && (fps == 30 || fps == 60), "invalid dimensions/fps");
        return probe(mode, height, fps);
    } catch (const std::exception& e) { std::cerr << "PROBE_FAILED " << e.what() << '\n'; return 2; }
}
