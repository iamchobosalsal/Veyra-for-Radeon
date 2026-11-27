// Actual FlowRefine GPU checks. Synthetic foreground/background motion is known,
// including thin and equal-luminance coloured detail. No live app/capture access.
#include "veyra/pipeline/GpuPassUtils.h"
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
        require(bool(device), "hardware device"); device.As(&messages);
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
    UINT width = 0, height = 0, bpp = 0;
    Transfer(Host& h, ID3D12Resource* texture, unsigned bytesPerPixel, bool upload) : bpp(bytesPerPixel) {
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
    void write(const void* data, size_t size) {
        require(size == size_t(width) * height * bpp, "upload size");
        uint8_t* mapped = nullptr; hr(buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "upload map");
        for (UINT y = 0; y < height; ++y)
            std::memcpy(mapped + y * layout.Footprint.RowPitch, static_cast<const uint8_t*>(data) + size_t(y) * width * bpp, size_t(width) * bpp);
        buffer->Unmap(0, nullptr);
    }
    std::vector<uint16_t> read() {
        require(bpp == 8 || bpp == 4, "FP16 RG/RGBA output");
        uint8_t* mapped = nullptr; hr(buffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "readback map");
        std::vector<uint16_t> result(size_t(width) * height * bpp/2);
        for (UINT y = 0; y < height; ++y)
            std::memcpy(result.data() + size_t(y) * width * bpp/2, mapped + y * layout.Footprint.RowPitch, size_t(width) * bpp);
        buffer->Unmap(0, nullptr); return result;
    }
    void copy(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, bool upload) {
        D3D12_TEXTURE_COPY_LOCATION buf{}, tex{};
        buf.pResource = buffer.Get(); buf.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; buf.PlacedFootprint = layout;
        tex.pResource = texture; tex.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(upload ? &tex : &buf, 0, 0, 0, upload ? &buf : &tex, nullptr);
    }
};
using Pixel = std::array<uint8_t, 4>; // RGBA
using Vector = std::array<int16_t, 2>;
struct Fixture {
    Host& h;
    unsigned w, height, gw, gh;
    ComPtr<ID3D12Resource> raw, previous, current, reverse, changes, output, claims, agreement, filled;
    Transfer ur, up, uc, uv, us, readback, filledReadback;
    ComputePass pass, claimPass, fillPass;
    std::vector<uint16_t> finalMotion;
    StateTracker states;
    ComPtr<ID3D12QueryHeap> timestamps;
    ComPtr<ID3D12Resource> timingReadback;
    Fixture(Host& host, unsigned width, unsigned he) : h(host), w(width), height(he), gw((w+7)/8), gh((height+7)/8),
        raw(makeTexture(h.device.Get(), gw, gh, DXGI_FORMAT_R16G16_SINT, false)),
        previous(makeTexture(h.device.Get(), w, height, DXGI_FORMAT_R8G8B8A8_UNORM, false)),
        current(makeTexture(h.device.Get(), w, height, DXGI_FORMAT_R8G8B8A8_UNORM, false)),
        reverse(makeTexture(h.device.Get(), gw, gh, DXGI_FORMAT_R16G16_SINT, false)),
        changes(makeTexture(h.device.Get(), 2, 1, DXGI_FORMAT_R32_UINT, false)),
        output(makeTexture(h.device.Get(), w, height, DXGI_FORMAT_R16G16B16A16_FLOAT, true)),
        claims(makeTexture(h.device.Get(), w, height, DXGI_FORMAT_R32_UINT, true)),
        agreement(makeTexture(h.device.Get(), w, height, DXGI_FORMAT_R16_FLOAT, true)),
        filled(makeTexture(h.device.Get(), w, height, DXGI_FORMAT_R16G16_FLOAT, true)),
        ur(h,raw.Get(),4,true), up(h,previous.Get(),4,true), uc(h,current.Get(),4,true),
        uv(h,reverse.Get(),4,true), us(h,changes.Get(),4,true), readback(h,output.Get(),8,false), filledReadback(h,filled.Get(),4,false) {
        std::vector<uint8_t> cs;
        require(pass.loadShader("FlowRefine.dxil", cs) && pass.create(h.device.Get(),cs,6,5,1), "production FlowRefine initialization");
        unsigned index = 0;
        for (auto* r : {raw.Get(),previous.Get(),current.Get(),reverse.Get(),changes.Get()})
            makeSrv(h.device.Get(),r,r->GetDesc().Format,cpuHandleOf(pass,index++));
        makeUav(h.device.Get(),output.Get(),output->GetDesc().Format,cpuHandleOf(pass,5));
        require(claimPass.loadShader("FlowClaim.dxil",cs) && claimPass.create(h.device.Get(),cs,3,1,2),"production FlowClaim initialization");
        makeSrv(h.device.Get(),output.Get(),output->GetDesc().Format,cpuHandleOf(claimPass,0));
        makeUav(h.device.Get(),claims.Get(),claims->GetDesc().Format,cpuHandleOf(claimPass,1));
        makeUav(h.device.Get(),agreement.Get(),agreement->GetDesc().Format,cpuHandleOf(claimPass,2));
        require(fillPass.loadShader("FlowFill.dxil",cs) && fillPass.create(h.device.Get(),cs,4,3,1),"production FlowFill initialization");
        makeSrv(h.device.Get(),output.Get(),output->GetDesc().Format,cpuHandleOf(fillPass,0));
        makeSrv(h.device.Get(),current.Get(),current->GetDesc().Format,cpuHandleOf(fillPass,1));
        makeSrv(h.device.Get(),agreement.Get(),agreement->GetDesc().Format,cpuHandleOf(fillPass,2));
        makeUav(h.device.Get(),filled.Get(),filled->GetDesc().Format,cpuHandleOf(fillPass,3));
        D3D12_QUERY_HEAP_DESC q{}; q.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;q.Count=2;
        hr(h.device->CreateQueryHeap(&q,IID_PPV_ARGS(&timestamps)),"timestamp heap");
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=16;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        hr(h.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&timingReadback)),"timestamp buffer");
    }
    std::vector<uint16_t> run(const std::vector<Pixel>& a, const std::vector<Pixel>& b, const std::vector<Vector>& flow, unsigned flags=2, unsigned cuts=0, Vector back={0,0}, double* ms=nullptr) {
        std::vector<Vector> reverseData(size_t(gw)*gh, back); std::array<unsigned,2> scenes{0,cuts};
        ur.write(flow.data(),flow.size()*sizeof(Vector));up.write(a.data(),a.size()*sizeof(Pixel));uc.write(b.data(),b.size()*sizeof(Pixel));
        uv.write(reverseData.data(),reverseData.size()*sizeof(Vector));us.write(scenes.data(),sizeof scenes);
        h.begin();
        auto upload=[&](ID3D12Resource* r, Transfer& t) { states.transition(h.list.Get(),r,D3D12_RESOURCE_STATE_COPY_DEST);t.copy(h.list.Get(),r,true);states.transition(h.list.Get(),r,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); };
        upload(raw.Get(),ur);upload(previous.Get(),up);upload(current.Get(),uc);upload(reverse.Get(),uv);upload(changes.Get(),us);
        states.transition(h.list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        uint32_t c[8]{gw,gh,w,height,8,std::bit_cast<uint32_t>(.004f),flags,0};
        pass.bind(h.list.Get(),reinterpret_cast<const float*>(c),gpuHandleOf(pass,0).ptr,gpuHandleOf(pass,5).ptr);
        h.list->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        h.list->Dispatch((w+15)/16,(height+15)/16,1);
        h.list->EndQuery(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
        h.list->ResolveQueryData(timestamps.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,timingReadback.Get(),0);
        states.uavBarrier(h.list.Get(),output.Get());states.transition(h.list.Get(),output.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        states.transition(h.list.Get(),claims.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        states.transition(h.list.Get(),agreement.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        unsigned indexBits=1;while((uint64_t(1)<<indexBits)<uint64_t(w)*height)++indexBits;
        for(unsigned mode=0;mode<3;++mode){uint32_t cc[8]{w,height,mode,indexBits,0,0,0,0};
            claimPass.bind(h.list.Get(),reinterpret_cast<const float*>(cc),gpuHandleOf(claimPass,0).ptr,gpuHandleOf(claimPass,1).ptr);
            h.list->Dispatch((w+15)/16,(height+15)/16,1);states.uavBarrier(h.list.Get(),claims.Get());}
        states.uavBarrier(h.list.Get(),agreement.Get());states.transition(h.list.Get(),agreement.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        states.transition(h.list.Get(),filled.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        uint32_t fc[8]{w,height,std::bit_cast<uint32_t>(.06f),std::bit_cast<uint32_t>(.002f),std::bit_cast<uint32_t>(2.f),std::bit_cast<uint32_t>(.5f),(flags&2)?1u:0u,0};
        fillPass.bind(h.list.Get(),reinterpret_cast<const float*>(fc),gpuHandleOf(fillPass,0).ptr,gpuHandleOf(fillPass,3).ptr);
        h.list->Dispatch((w+15)/16,(height+15)/16,1);
        states.uavBarrier(h.list.Get(),filled.Get());states.transition(h.list.Get(),filled.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);filledReadback.copy(h.list.Get(),filled.Get(),false);
        states.transition(h.list.Get(),output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);readback.copy(h.list.Get(),output.Get(),false);
        h.complete();
        if(ms) { UINT64 f=0;hr(h.queue->GetTimestampFrequency(&f),"timestamp frequency");UINT64* t=nullptr;hr(timingReadback->Map(0,nullptr,reinterpret_cast<void**>(&t)),"timestamp map");*ms=double(t[1]-t[0])*1000.0/double(f);timingReadback->Unmap(0,nullptr); }
        finalMotion=filledReadback.read();return readback.read();
    }
};
}
int main(int argc, char** argv) { try {
    require(argc==1 || (argc==2 && std::strcmp(argv[1],"--1080p")==0),"usage: flow_boundary_tests [--1080p]");
    Host host; unsigned failures=0, checks=0;
    auto check=[&](const char* name,float error) { ++checks; bool ok=std::isfinite(error)&&error<=.01f;failures+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<name<<" errorPx="<<error<<'\n'; };
    Fixture f(host,97,65); // odd sizes also exercise dispatch guards and row pitches
    // A thin foreground moves right by four pixels. The block at its new
    // location belongs to the background; a neighbour carries the true vector.
    for(bool vertical : {false,true}) for(bool chromatic : {false,true}) {
        std::vector<Pixel> a(size_t(f.w)*f.height),b(a.size());
        auto pixel=[&](unsigned x,unsigned y,int shift) {
            const int axis=int(vertical?y:x);
            if(axis==40+shift) return chromatic?Pixel{180,0,0,255}:Pixel{136,136,136,255};
            if(chromatic) return Pixel{0,54,0,255};
            // High-frequency background has its own (zero) motion.
            uint8_t v=uint8_t(90+((axis/3)%2)*90);return Pixel{v,v,v,255};
        };
        for(unsigned y=0;y<f.height;++y)for(unsigned x=0;x<f.w;++x){a[size_t(y)*f.w+x]=pixel(x,y,-4);b[size_t(y)*f.w+x]=pixel(x,y,0);}
        std::vector<Vector> flow(size_t(f.gw)*f.gh,Vector{0,0});
        for(unsigned y=0;y<f.gh;++y)for(unsigned x=0;x<f.gw;++x)if((vertical?y:x)==4)flow[size_t(y)*f.gw+x]=vertical?Vector{0,-4}:Vector{-4,0};
        const auto out=f.run(a,b,flow);
        float err=0, background=0, finalError=0;
        for(unsigned n=8;n<55;++n){unsigned x=vertical?n:40,y=vertical?40:n;size_t i=(size_t(y)*f.w+x)*4;err=std::max(err,std::abs(unhalf(out[i+(vertical?1:0)])+4));
            finalError=std::max(finalError,std::abs(unhalf(f.finalMotion[i/2+(vertical?1:0)])+4));
            x=vertical?n:60;y=vertical?60:n;i=(size_t(y)*f.w+x)*4;background=std::max(background,std::hypot(unhalf(out[i]),unhalf(out[i+1])));}
        check(chromatic?(vertical?"equal-luma horizontal edge":"equal-luma vertical edge"):(vertical?"thin horizontal edge":"thin vertical edge"),err);
        check("stationary background",background);
        check("foreground survives claim/fill",finalError);
        for(auto [flags,cuts] : {std::pair{6u,0u},std::pair{2u,1u},std::pair{2u,8u}}){const auto reset=f.run(a,b,flow,flags,cuts);float e=0;for(auto v:reset)e=std::max(e,std::abs(unhalf(v)));check("warmup/scene-cut clears motion",e);}
    }
    // Exact global translation is preserved; no attenuation/blanket zeroing.
    std::vector<Pixel> a(size_t(f.w)*f.height),b(a.size());
    for(unsigned y=0;y<f.height;++y)for(unsigned x=0;x<f.w;++x){a[size_t(y)*f.w+x]=Pixel{uint8_t(x*2),uint8_t(y*3),80,255};b[size_t(y)*f.w+x]=Pixel{uint8_t(std::max(0,int(x)-4)*2),uint8_t(y*3),80,255};}
    std::vector<Vector> global(size_t(f.gw)*f.gh,Vector{-4,0});
    const auto out=f.run(a,b,global);float e=0;for(unsigned y=4;y<f.height-4;++y)for(unsigned x=8;x<f.w-4;++x)e=std::max(e,std::abs(unhalf(out[(size_t(y)*f.w+x)*4])+4));check("global translation",e);
    std::fill(global.begin(),global.end(),Vector{-128,0});
    const auto nv=f.run(a,b,global,1,0,{128,0});e=0;for(unsigned y=4;y<f.height-4;++y)for(unsigned x=8;x<f.w-4;++x){size_t i=(size_t(y)*f.w+x)*4;e=std::max(e,std::abs(unhalf(nv[i])+4));e=std::max(e,std::abs(unhalf(nv[i+3])));}check("NVOF units and reverse agreement",e);
    if(argc==2){
        Fixture large(host,1920,1080);std::vector<Pixel> p(size_t(large.w)*large.height),q(p.size());
        for(unsigned y=0;y<large.height;++y)for(unsigned x=0;x<large.w;++x){p[size_t(y)*large.w+x]=Pixel{uint8_t((x/8)%256),uint8_t((y/4)%256),80,255};q[size_t(y)*large.w+x]=Pixel{uint8_t((std::max(0,int(x)-4)/8)%256),uint8_t((y/4)%256),80,255};}
        std::vector<Vector> motion(size_t(large.gw)*large.gh,Vector{-4,0});double total=0,maximum=0;
        for(unsigned k=0;k<12;++k){double ms=0;const auto result=large.run(p,q,motion,2,0,{0,0},&ms);if(k>=2){total+=ms;maximum=std::max(maximum,ms);}if(k==11)check("1080p nonzero FG guidance",std::abs(unhalf(result[(size_t(500)*large.w+500)*4])+4));}
        std::cout<<"TIMING flowRefine1080p averageMs="<<total/10<<" maxMs="<<maximum<<" samples=10\n";
    }
    check("D3D12 diagnostics",float(host.errors()));
    std::cout<<"RESULT checks="<<checks<<" failures="<<failures<<" debugLayer="<<host.debug<<'\n';return failures?1:0;
} catch(const std::exception& e){std::cerr<<"ERROR "<<e.what()<<'\n';return 2;} }
