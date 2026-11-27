// SPDX-License-Identifier: GPL-3.0-only
// Diagnostic readback only. Tests the production codec, not neural inference.
#include "veyra/pipeline/NrParityPass.h"
#include "veyra/parity/RenoDxParityCodec.h"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>

using namespace veyra::pipeline;
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void hr(HRESULT value,const char* message){require(SUCCEEDED(value),message);}
uint16_t half(float v){
    const auto b=std::bit_cast<uint32_t>(v);const uint16_t sign=uint16_t((b>>16)&0x8000);
    const int e=int((b>>23)&255)-112;const uint32_t mantissa=b&0x7fffff;
    if(e<=0){if(e<-10)return sign;const uint32_t m=mantissa|0x800000;const unsigned shift=14-e;
        return sign|uint16_t((m+((1u<<(shift-1))-1)+((m>>shift)&1))>>shift);}
    return sign|uint16_t((uint32_t(e)<<10)+((mantissa+0xfff+((b>>13)&1))>>13));
}
float unhalf(uint16_t h){const int e=(h>>10)&31,m=h&1023;
    const float v=e==0?std::ldexp(float(m),-24):e==31?INFINITY:std::ldexp(float(1024+m),e-25);
    return (h&0x8000)?-v:v;
}
struct Host {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;ComPtr<ID3D12InfoQueue> messages;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);uint64_t serial=0;bool debug=false;
    Host(){
        ComPtr<ID3D12Debug> layer;debug=SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&layer)));
        if(debug)layer->EnableDebugLayer();
        ComPtr<IDXGIFactory6> factory;hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
        for(UINT i=0;!device;++i){ComPtr<IDXGIAdapter1> adapter;
            if(FAILED(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))))break;
            DXGI_ADAPTER_DESC1 desc{};adapter->GetDesc1(&desc);if(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)continue;
            if(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))))
                std::wcout<<L"CODEC_ADAPTER "<<desc.Description<<L'\n';
        }
        require(bool(device),"hardware D3D12 device");device.As(&messages);
        D3D12_COMMAND_QUEUE_DESC q{};hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
        hr(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"allocator");
        hr(device->CreateCommandList(0,q.Type,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
        hr(list->Close(),"close initial");hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
        require(event!=nullptr,"event");
    }
    ~Host(){if(event)CloseHandle(event);}
    void begin(){hr(allocator->Reset(),"allocator reset");hr(list->Reset(allocator.Get(),nullptr),"list reset");}
    void complete(){
        hr(list->Close(),"list close");ID3D12CommandList* batch[]{list.Get()};queue->ExecuteCommandLists(1,batch);
        hr(queue->Signal(fence.Get(),++serial),"signal");hr(fence->SetEventOnCompletion(serial,event),"event set");
        require(WaitForSingleObject(event,15000)==WAIT_OBJECT_0,"GPU completion timeout");hr(device->GetDeviceRemovedReason(),"device removed");
    }
    unsigned errors(){unsigned count=0;if(messages)for(UINT64 i=0;i<messages->GetNumStoredMessages();++i){
        SIZE_T size=0;messages->GetMessage(i,nullptr,&size);std::vector<uint8_t> buf(size);
        auto* msg=reinterpret_cast<D3D12_MESSAGE*>(buf.data());messages->GetMessage(i,msg,&size);
        if(msg->Severity<=D3D12_MESSAGE_SEVERITY_WARNING){++count;std::cerr<<msg->pDescription<<'\n';}
    }return count;}
};
struct Transfer {
    ComPtr<ID3D12Resource> buffer;D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};UINT64 size=0;UINT width=0,height=0,bytes=0;
    Transfer(Host& h,ID3D12Resource* texture,bool upload){
        const auto d=texture->GetDesc();width=UINT(d.Width);height=d.Height;bytes=d.Format==DXGI_FORMAT_R8G8B8A8_UNORM?4:8;
        h.device->GetCopyableFootprints(&d,0,1,0,&layout,nullptr,nullptr,&size);
        if(upload)buffer=makeUploadBuffer(h.device.Get(),size);
        else {D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=size;desc.Height=1;
            desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            hr(h.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&buffer)),"readback allocation");}
        require(bool(buffer),"transfer buffer");
    }
    void write(const void* data){uint8_t* mapped=nullptr;hr(buffer->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"upload map");
        for(UINT y=0;y<height;++y)std::memcpy(mapped+y*layout.Footprint.RowPitch,static_cast<const uint8_t*>(data)+size_t(y)*width*bytes,size_t(width)*bytes);
        buffer->Unmap(0,nullptr);
    }
    std::vector<uint8_t> read(){uint8_t* mapped=nullptr;hr(buffer->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"readback map");
        std::vector<uint8_t> result(size_t(width)*height*bytes);
        for(UINT y=0;y<height;++y)std::memcpy(result.data()+size_t(y)*width*bytes,mapped+y*layout.Footprint.RowPitch,size_t(width)*bytes);
        buffer->Unmap(0,nullptr);return result;
    }
    void copy(ID3D12GraphicsCommandList* list,ID3D12Resource* texture,bool upload){
        D3D12_TEXTURE_COPY_LOCATION buf{},tex{};buf.pResource=buffer.Get();buf.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;buf.PlacedFootprint=layout;
        tex.pResource=texture;tex.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(upload?&tex:&buf,0,0,0,upload?&buf:&tex,nullptr);
    }
};
struct Result{unsigned frames=0,proxyMax=0,identical=0;double resolveMax=0,resolveNormalized=0;uint64_t compared=0;};
void run(Host& h,const std::filesystem::path& shaders,UINT w,UINT height,Result& stats){
    NrParityPass codec;require(codec.open(h.device.Get(),shaders,w,height),"load actual Veyra codec shaders");
    auto input=makeTexture(h.device.Get(),w,height,DXGI_FORMAT_R16G16B16A16_FLOAT,false);
    auto output=makeTexture(h.device.Get(),w,height,DXGI_FORMAT_R16G16B16A16_FLOAT,false);
    Transfer in(h,input.Get(),true),neural(h,codec.neural(),true),proxy(h,codec.proxy(),false),final(h,output.Get(),false);
    StateTracker states;const size_t count=size_t(w)*height;std::vector<uint16_t> pixels(count*4);std::vector<uint8_t> network(count*4);
    constexpr float levels[]{0,0.00001f,0.001f,0.0031308f,0.02f,0.18f,0.5f,1,2,8,-0.1f};
    for(unsigned frame=0;frame<6;++frame){
        const bool identity=frame==2||frame==4;
        for(size_t p=0;p<count;++p)for(unsigned c=0;c<4;++c){
            const float value=c==3?float(p%5)/4.f:levels[(p*(c+3)+c*5+frame)%std::size(levels)];
            pixels[p*4+c]=half(value);network[p*4+c]=uint8_t((p*37+c*71+frame*23)%256);
        }
        in.write(pixels.data());neural.write(network.data());h.begin();
        states.transition(h.list.Get(),input.Get(),D3D12_RESOURCE_STATE_COPY_DEST);in.copy(h.list.Get(),input.Get(),true);
        states.transition(h.list.Get(),input.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);codec.encode(h.list.Get(),input.Get());
        // The proxy is owned by the codec and exits encode in NPSR. Restore it
        // after the diagnostic readback so the tested graph contract is intact.
        states.set(codec.proxy(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        states.transition(h.list.Get(),codec.proxy(),D3D12_RESOURCE_STATE_COPY_SOURCE);proxy.copy(h.list.Get(),codec.proxy(),false);
        states.transition(h.list.Get(),codec.proxy(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        codec.neuralToCopyDest(h.list.Get());neural.copy(h.list.Get(),codec.neural(),true);
        states.transition(h.list.Get(),output.Get(),D3D12_RESOURCE_STATE_COPY_DEST);codec.decode(h.list.Get(),output.Get(),identity);
        states.transition(h.list.Get(),output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);final.copy(h.list.Get(),output.Get(),false);h.complete();
        const auto encoded=proxy.read(),decoded=final.read();
        for(size_t p=0;p<count;++p){
            const veyra::parity::RgbF original{unhalf(pixels[p*4]),unhalf(pixels[p*4+1]),unhalf(pixels[p*4+2])};
            auto expected=veyra::parity::encodeProxy(original,unhalf(pixels[p*4+3]),{});
            const uint8_t expectedBytes[]{expected.r,expected.g,expected.b,expected.a};
            for(unsigned c=0;c<4;++c){unsigned difference=std::abs(int(encoded[p*4+c])-expectedBytes[c]);
                stats.proxyMax=std::max(stats.proxyMax,difference);require(difference<=1,"proxy differs by more than one 8-bit step");}
            if(identity)continue;
            auto rgb=[&](const std::vector<uint8_t>& bytes){return veyra::parity::RgbF{
                veyra::parity::srgbDecode(bytes[p*4]/255.),veyra::parity::srgbDecode(bytes[p*4+1]/255.),veyra::parity::srgbDecode(bytes[p*4+2]/255.)};};
            const auto reference=veyra::parity::parityDecode(original,rgb(encoded),rgb(network),{});
            const double values[]{reference.r,reference.g,reference.b,unhalf(pixels[p*4+3])};
            for(unsigned c=0;c<4;++c){uint16_t result;std::memcpy(&result,decoded.data()+(p*4+c)*2,2);const double got=unhalf(result);
                const double difference=std::abs(got-values[c]);stats.resolveMax=std::max(stats.resolveMax,difference);
                stats.resolveNormalized=std::max(stats.resolveNormalized,difference/std::max(1.,std::abs(values[c])));
                if(!std::isfinite(got)||difference>0.002*std::max(1.,std::abs(values[c]))){
                    std::cerr<<"resolve mismatch p="<<p<<" channel="<<c<<" got="<<got<<" expected="<<values[c]<<'\n';
                    throw std::runtime_error("resolve differs from independent CPU reference");}
                ++stats.compared;
            }
        }
        if(identity){require(decoded.size()==pixels.size()*2&&!std::memcmp(decoded.data(),pixels.data(),decoded.size()),"zero-strength identity must be byte exact");++stats.identical;}
        ++stats.frames;
    }
}
}
int wmain(int argc,wchar_t** argv){try{
    require(argc==2,"usage: nr_parity_gpu_tests <absolute production shaders directory>");Host host;Result result;
    run(host,std::filesystem::path(argv[1]),37,19,result);run(host,std::filesystem::path(argv[1]),191,117,result);
    const unsigned errors=host.errors();require(errors==0,"D3D12 debug errors/warnings");
    std::cout<<"CODEC_PASS frames="<<result.frames<<" identityFrames="<<result.identical<<" proxyMaxSteps="<<result.proxyMax
        <<" resolveMaxAbsolute="<<result.resolveMax<<" resolveMaxNormalized="<<result.resolveNormalized<<" comparedChannels="<<result.compared<<" debugAvailable="<<host.debug<<" debugErrors="<<errors
        <<" neuralInferenceTested=false\n";return 0;
}catch(const std::exception& e){std::cerr<<"CODEC_FAIL "<<e.what()<<'\n';return 1;}}
