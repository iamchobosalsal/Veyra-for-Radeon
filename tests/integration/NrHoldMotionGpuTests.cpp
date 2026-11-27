// GPU regression for moving detail that a box-mean change detector misses.
// Uses the production NrHoldPass and shader; no capture device or app profile.
#include "veyra/pipeline/NrHoldPass.h"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace veyra::pipeline;
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void hr(HRESULT code, const char* message) { require(SUCCEEDED(code), message); }
uint16_t half(float v) {
    const auto b = std::bit_cast<uint32_t>(v);
    const uint16_t sign = uint16_t((b >> 16) & 0x8000);
    const int e = int((b >> 23) & 255) - 112;
    const uint32_t mantissa = b & 0x7fffff;
    if (e <= 0) {
        if (e < -10) return sign;
        const uint32_t m = mantissa | 0x800000;
        const unsigned shift = 14 - e;
        return sign | uint16_t((m + ((1u << (shift - 1)) - 1) + ((m >> shift) & 1)) >> shift);
    }
    return sign | uint16_t((uint32_t(e) << 10) + ((mantissa + 0xfff + ((b >> 13) & 1)) >> 13));
}
float unhalf(uint16_t h) {
    const int e = (h >> 10) & 31, m = h & 1023;
    const float v = e == 0 ? std::ldexp(float(m), -24) : e == 31 ? INFINITY : std::ldexp(float(1024 + m), e - 25);
    return (h & 0x8000) ? -v : v;
}
struct Host {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> messages;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint64_t serial = 0;
    bool debug = false;
    Host() {
        ComPtr<ID3D12Debug> layer;
        debug = SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)));
        if (debug) layer->EnableDebugLayer();
        ComPtr<IDXGIFactory6> factory;
        hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "factory");
        for (UINT i = 0; !device; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            if (FAILED(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)))) break;
            DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
            if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))))
                std::wcout << L"ADAPTER " << desc.Description << L'\n';
        }
        require(bool(device), "hardware D3D12 device"); device.As(&messages);
        D3D12_COMMAND_QUEUE_DESC q{};
        hr(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue)), "queue");
        hr(device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator)), "allocator");
        hr(device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)), "list");
        hr(list->Close(), "initial close");
        hr(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)), "fence");
        require(event != nullptr, "event");
    }
    ~Host() { if (event) CloseHandle(event); }
    void begin() { hr(allocator->Reset(), "allocator reset"); hr(list->Reset(allocator.Get(), nullptr), "list reset"); }
    void complete() {
        hr(list->Close(), "list close"); ID3D12CommandList* batch[]{list.Get()}; queue->ExecuteCommandLists(1, batch);
        hr(queue->Signal(fence.Get(), ++serial), "signal");
        hr(fence->SetEventOnCompletion(serial, event), "set event");
        require(WaitForSingleObject(event, 15000) == WAIT_OBJECT_0, "GPU timeout");
        hr(device->GetDeviceRemovedReason(), "device removed");
    }
    unsigned errors() {
        unsigned count = 0;
        if (messages) for (UINT64 i = 0; i < messages->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0; messages->GetMessage(i, nullptr, &size); std::vector<uint8_t> bytes(size);
            auto* msg = reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); messages->GetMessage(i, msg, &size);
            if (msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) { ++count; std::cerr << msg->pDescription << '\n'; }
        }
        return count;
    }
};
struct Transfer {
    ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT width = 0, height = 0;
    Transfer(Host& h, ID3D12Resource* texture, bool upload) {
        const auto d = texture->GetDesc(); width = UINT(d.Width); height = d.Height;
        UINT64 size = 0; h.device->GetCopyableFootprints(&d, 0, 1, 0, &layout, nullptr, nullptr, &size);
        if (upload) buffer = makeUploadBuffer(h.device.Get(), size);
        else {
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC desc{}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = size; desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            hr(h.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&buffer)), "readback");
        }
        require(bool(buffer), "transfer allocation");
    }
    void write(const std::vector<uint16_t>& data) {
        require(data.size() == size_t(width) * height * 4, "upload size");
        uint8_t* mapped = nullptr; hr(buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "upload map");
        for (UINT y = 0; y < height; ++y)
            std::memcpy(mapped + y * layout.Footprint.RowPitch, data.data() + size_t(y) * width * 4, size_t(width) * 8);
        buffer->Unmap(0, nullptr);
    }
    std::vector<uint16_t> read() {
        uint8_t* mapped = nullptr; hr(buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "readback map");
        std::vector<uint16_t> result(size_t(width) * height * 4);
        for (UINT y = 0; y < height; ++y)
            std::memcpy(result.data() + size_t(y) * width * 4, mapped + y * layout.Footprint.RowPitch, size_t(width) * 8);
        buffer->Unmap(0, nullptr); return result;
    }
    void copy(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, bool upload) {
        D3D12_TEXTURE_COPY_LOCATION buf{}, tex{};
        buf.pResource = buffer.Get(); buf.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; buf.PlacedFootprint = layout;
        tex.pResource = texture; tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(upload ? &tex : &buf, 0, 0, 0, upload ? &buf : &tex, nullptr);
    }
};
struct Fixture {
    Host& host;
    ComPtr<ID3D12Resource> base, current;
    Transfer uploadBase, uploadCurrent, readback;
    NrHoldPass pass;
    StateTracker states;
    Fixture(Host& h, unsigned w, unsigned height, unsigned bw, unsigned bh) : host(h),
        base(makeTexture(h.device.Get(), bw, bh, DXGI_FORMAT_R16G16B16A16_FLOAT, false)),
        current(makeTexture(h.device.Get(), w, height, DXGI_FORMAT_R16G16B16A16_FLOAT, false)),
        uploadBase(h, base.Get(), true), uploadCurrent(h, current.Get(), true), readback(h, current.Get(), false) {
        require(pass.initialize(h.device.Get(), w, height, current.Get(), base.Get()), "production hold initialization");
    }
    std::vector<uint16_t> run(const std::vector<uint16_t>& source, const std::vector<uint16_t>& nr, bool reset) {
        uploadBase.write(source); uploadCurrent.write(nr); host.begin();
        states.transition(host.list.Get(), base.Get(), D3D12_RESOURCE_STATE_COPY_DEST); uploadBase.copy(host.list.Get(), base.Get(), true);
        states.transition(host.list.Get(), current.Get(), D3D12_RESOURCE_STATE_COPY_DEST); uploadCurrent.copy(host.list.Get(), current.Get(), true);
        pass.run(host.list.Get(), states, reset, .8f, .02f);
        states.transition(host.list.Get(), pass.result(), D3D12_RESOURCE_STATE_COPY_SOURCE); readback.copy(host.list.Get(), pass.result(), false);
        host.complete(); return readback.read();
    }
};
using Pixel = std::array<float, 4>;
template<class F> std::vector<uint16_t> pixels(unsigned w, unsigned h, F fn) {
    std::vector<uint16_t> out(size_t(w) * h * 4);
    for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w; ++x) {
        const auto p = fn(x, y);
        for (unsigned c = 0; c < 4; ++c) out[(size_t(y) * w + x) * 4 + c] = half(p[c]);
    }
    return out;
}
float error(const std::vector<uint16_t>& a, const std::vector<uint16_t>& b) {
    require(a.size() == b.size(), "comparison size"); float maxError = 0;
    for (size_t i = 0; i < a.size(); ++i) maxError = std::max(maxError, std::abs(unhalf(a[i]) - unhalf(b[i])));
    return maxError;
}
}

int main(int argc, char** argv) { try {
    require(argc == 1 || (argc == 2 && std::strcmp(argv[1], "--4k") == 0), "usage: nr_hold_motion_tests [--4k]");
    Host host;
    unsigned failures = 0;
    auto check = [&](const char* name, float value, float limit) {
        const bool pass = std::isfinite(value) && value <= limit;
        std::cout << (pass ? "PASS " : "FAIL ") << name << " error=" << value << " limit=" << limit << '\n';
        failures += !pass;
    };
    // A one-pixel wire moves within the same 3x3 box. Its box mean does not
    // change, but neither a disappearing wire nor its new position may linger.
    for (bool chromatic : {false, true}) {
        Fixture f(host, 37, 23, 37, 23);
        auto scene = [&](unsigned pos) { return pixels(37, 23, [&](unsigned x, unsigned) {
            if (chromatic) return x >= pos && x < pos + 9 ? Pixel{.7152f, 0, 0, 1} : Pixel{0, .2126f, 0, 1};
            const float v = x == pos ? .8f : .08f; return Pixel{v, v, v, 1};
        }); };
        auto first = scene(12), second = scene(13);
        f.run(first, first, true);
        check(chromatic ? "equal-luma color motion" : "one-pixel wire motion", error(f.run(second, second, false), second), .001f);
        check("explicit reset publishes current", error(f.run(first, first, true), first), 0);
    }
    // Static texture at different input/output extents must not be mistaken
    // for motion. The stored anchor is at output extent, including odd sizes.
    for (const auto dims : {std::array<unsigned, 4>{37, 23, 37, 23}, {37, 23, 74, 46}, {37, 23, 19, 11}}) {
        const auto [w, h, bw, bh] = dims; Fixture f(host, w, h, bw, bh);
        auto source = pixels(bw, bh, [](unsigned x, unsigned y) {
            const float v = ((x + y) % 3 == 0) ? .85f : .1f; return Pixel{v, v, v, 1};
        });
        auto a = pixels(w, h, [](unsigned, unsigned) { return Pixel{.4f, -.1f, 4, .75f}; });
        auto b = pixels(w, h, [](unsigned, unsigned) { return Pixel{.5f, -.2f, 5, .75f}; });
        // Full-alpha history is required by the pass. Check signed/HDR color
        // separately while retaining current alpha in the published output.
        for (size_t i = 3; i < a.size(); i += 4) a[i] = half(1);
        f.run(source, a, true);
        auto expected = b;
        for (size_t i = 0; i < expected.size(); ++i)
            if (i % 4 != 3) expected[i] = half(.2f * unhalf(b[i]) + .8f * unhalf(a[i]));
        check("static signed HDR / scaled anchor", error(f.run(source, b, false), expected), .004f);
        auto got = f.run(source, a, true);
        check("reset after history", error(got, a), 0);
        f.pass.reset(); host.begin(); f.pass.run(host.list.Get(), f.states, false, 0, .02f); host.complete();
        check("zero strength leaves history inactive", f.pass.active() ? 1.f : 0.f, 0);
    }
    // Tiny source noise should still be held, rather than disabling temporal
    // stabilization everywhere to make the motion assertions pass.
    {
        Fixture f(host, 37, 23, 37, 23);
        auto source = pixels(37, 23, [](unsigned, unsigned) { return Pixel{.5f, .5f, .5f, 1}; });
        auto a = source;
        auto b = pixels(37, 23, [](unsigned, unsigned) { return Pixel{.6f, .6f, .6f, 1}; });
        f.run(source, a, true);
        source = pixels(37, 23, [](unsigned x, unsigned y) { float v = .5f + ((x + y) % 2 ? .001f : -.001f); return Pixel{v, v, v, 1}; });
        auto expected = pixels(37, 23, [](unsigned, unsigned) { return Pixel{.52f, .52f, .52f, 1}; });
        check("small source noise retains stabilization", error(f.run(source, b, false), expected), .001f);
    }
    if (argc == 2) {
        Fixture f(host, 3840, 2160, 3840, 2160);
        auto scene = [](unsigned pos) { return pixels(3840, 2160, [=](unsigned x, unsigned) {
            const float v = x == pos ? .8f : .08f; return Pixel{v, v, v, 1};
        }); };
        auto a = scene(1920), b = scene(1921);
        f.run(a, a, true);
        check("4K one-pixel wire motion", error(f.run(b, b, false), b), .001f);
        D3D12_QUERY_HEAP_DESC desc{}; desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP; desc.Count = 2;
        ComPtr<ID3D12QueryHeap> queries;
        hr(host.device->CreateQueryHeap(&desc, IID_PPV_ARGS(&queries)), "timestamp heap");
        UINT64 frequency = 0; hr(host.queue->GetTimestampFrequency(&frequency), "timestamp frequency");
        double sum = 0, maximum = 0;
        for (unsigned i = 0; i < 12; ++i) {
            host.begin();
            host.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            f.pass.run(host.list.Get(), f.states, false, .8f, .02f);
            host.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            host.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, f.readback.buffer.Get(), 0);
            host.complete();
            UINT64* ticks = nullptr;
            hr(f.readback.buffer->Map(0, nullptr, reinterpret_cast<void**>(&ticks)), "timestamp map");
            const double ms = double(ticks[1] - ticks[0]) * 1000. / frequency;
            f.readback.buffer->Unmap(0, nullptr);
            if (i >= 2) { sum += ms; maximum = std::max(maximum, ms); }
        }
        std::cout << "4K_GPU_HOLD samples=10 meanMs=" << sum / 10 << " maxMs=" << maximum << " notEntirePipeline=true\n";
    }
    const auto errors = host.errors(); failures += errors != 0;
    std::cout << "RESULT failures=" << failures << " debugAvailable=" << host.debug << " debugMessages=" << errors << '\n';
    return failures ? 1 : 0;
} catch (const std::exception& e) { std::cerr << "ERROR " << e.what() << '\n'; return 2; } }
