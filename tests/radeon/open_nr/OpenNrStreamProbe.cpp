// SPDX-License-Identifier: GPL-3.0-only
// Sustained GPU processing benchmark. Eight distinct uploaded fixture frames
// cycle on the GPU. No presentation, decoder, audio, or per-frame pixel readback.
// Bounded four-slot command ring; CPU fence waits are benchmark scheduling only.
#include "veyra/pipeline/OpenNrBackend.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include <dxgi1_6.h>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace veyra::pipeline;
namespace {
void check(bool ok, const char* message) { if(!ok) throw std::runtime_error(message); }
void hr(HRESULT r, const char* message) { check(SUCCEEDED(r),message); }
uint16_t half(float f) {
    const uint32_t b=std::bit_cast<uint32_t>(f); const int e=int((b>>23)&255)-112;
    if(e<=0) { if(e < -10) return 0; const auto m=(b&0x7fffff)|0x800000;
        const unsigned s=unsigned(14-e); return uint16_t((m+((1u<<(s-1))-1)+((m>>s)&1))>>s); }
    return uint16_t((uint32_t(e)<<10)+(((b&0x7fffff)+0xfff+((b>>13)&1))>>13));
}
struct Event { HANDLE value=CreateEventW(nullptr,FALSE,FALSE,nullptr); ~Event(){if(value)CloseHandle(value);} };
struct Slot {
    ComPtr<ID3D12CommandAllocator> alloc[2];
    ComPtr<ID3D12GraphicsCommandList> list[2];
    ComPtr<ID3D12Resource> output;
    uint64_t completion=0;
};
int run(int argc,wchar_t** argv) {
    if(argc!=5) {std::cerr<<"Usage: stream_probe runtime 4k-eight-frames.rgba result.json modelScale\n";return 2;}
    constexpr uint32_t w=3840,h=2160,inputCount=8,slotCount=4,warmup=32,frames=240;
    const std::filesystem::path root=argv[1], source=argv[2], report=argv[3];
    const float scale=std::stof(argv[4]);
    check(!std::filesystem::exists(report),"report already exists");
    check(std::filesystem::file_size(source)==uint64_t(w)*h*4*inputCount,"invalid input size");
    ComPtr<IDXGIFactory6> factory;hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
    ComPtr<ID3D12Device> device;DXGI_ADAPTER_DESC1 desc{};
    for(UINT i=0;!device;++i) {
        ComPtr<IDXGIAdapter1> adapter;
        if(FAILED(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))))break;
        hr(adapter->GetDesc1(&desc),"description");
        if(desc.VendorId!=0x1002 || desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)continue;
        D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device));
    }
    check(bool(device),"AMD D3D12 GPU unavailable");std::wcout<<L"GPU "<<desc.Description<<std::endl;
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC q{};
    hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
    ComPtr<ID3D12Fence> fence;hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
    Event event;check(bool(event.value),"event");uint64_t serial=0;
    auto wait=[&](uint64_t value){
        if(value&&fence->GetCompletedValue()<value){
            hr(fence->SetEventOnCompletion(value,event.value),"event completion");
            check(WaitForSingleObject(event.value,5000)==WAIT_OBJECT_0,"GPU timed out");
        }
        hr(device->GetDeviceRemovedReason(),"device removed");
    };
    auto signal=[&]{hr(queue->Signal(fence.Get(),++serial),"signal");return serial;};
    std::array<Slot,slotCount> slots;
    for(auto& s:slots){
        for(unsigned i=0;i<2;++i){
            hr(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&s.alloc[i])),"allocator");
            hr(device->CreateCommandList(0,q.Type,s.alloc[i].Get(),nullptr,IID_PPV_ARGS(&s.list[i])),"list");
            hr(s.list[i]->Close(),"initial close");
        }
        s.output=makeTexture(device.Get(),w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,false);
        check(bool(s.output),"output texture");
    }
    auto begin=[](Slot& s,unsigned i){hr(s.alloc[i]->Reset(),"allocator reset");hr(s.list[i]->Reset(s.alloc[i].Get(),nullptr),"list reset");};
    auto submit=[&](Slot& s,unsigned i){hr(s.list[i]->Close(),"close");ID3D12CommandList* lists[]{s.list[i].Get()};queue->ExecuteCommandLists(1,lists);};
    std::array<ComPtr<ID3D12Resource>,inputCount> input;
    StateTracker states;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;
    auto format=slots[0].output->GetDesc();device->GetCopyableFootprints(&format,0,1,0,&footprint,nullptr,nullptr,&bytes);
    auto upload=makeUploadBuffer(device.Get(),bytes);check(bool(upload),"upload");
    std::array<uint16_t,256> linear{};
    for(unsigned i=0;i<256;++i){const float v=float(i)/255;linear[i]=half(v<=.04045f?v/12.92f:std::pow((v+.055f)/1.055f,2.4f));}
    std::ifstream in(source,std::ios::binary);std::vector<uint8_t> rgba(size_t(w)*h*4);
    for(unsigned f=0;f<inputCount;++f){
        input[f]=makeTexture(device.Get(),w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,false);check(bool(input[f]),"input texture");
        in.read(reinterpret_cast<char*>(rgba.data()),rgba.size());check(bool(in),"read input");
        uint8_t* p=nullptr;hr(upload->Map(0,nullptr,reinterpret_cast<void**>(&p)),"map");
        for(unsigned y=0;y<h;++y){auto row=reinterpret_cast<uint16_t*>(p+size_t(y)*footprint.Footprint.RowPitch);
            for(unsigned x=0;x<w;++x){const auto at=(size_t(y)*w+x)*4;for(unsigned c=0;c<3;++c)row[x*4+c]=linear[rgba[at+c]];row[x*4+3]=0x3c00;}}
        upload->Unmap(0,nullptr);auto& s=slots[0];begin(s,0);
        states.transition(s.list[0].Get(),input[f].Get(),D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprint;
        to.pResource=input[f].Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        s.list[0]->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        states.transition(s.list[0].Get(),input[f].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        submit(s,0);wait(signal());
    }
    upload.Reset();OpenNrBackend nr;
    if(!nr.open(std::filesystem::absolute(root),device.Get(),queue.Get(),w,h,true,true,scale))throw std::runtime_error(nr.error());
    uint64_t processed=0;double gpuSum=0;unsigned gpuSamples=0;
    auto frame=[&](unsigned f){
        auto& s=slots[f%slotCount];wait(s.completion);begin(s,0);
        if(!nr.recordInputs(s.list[0].Get(),input[f%inputCount].Get(),f+1,f==0,1))throw std::runtime_error(nr.error());submit(s,0);
        if(!nr.enqueue())throw std::runtime_error(nr.error());begin(s,1);
        states.transition(s.list[1].Get(),s.output.Get(),D3D12_RESOURCE_STATE_COPY_DEST);
        if(!nr.recordOutputs(s.list[1].Get(),s.output.Get()))throw std::runtime_error(nr.error());submit(s,1);
        if(!nr.retireSubmitted())throw std::runtime_error(nr.error());s.completion=signal();++processed;
        if(f>=warmup&&nr.gpuMilliseconds()>0){gpuSum+=nr.gpuMilliseconds();++gpuSamples;}
    };
    for(unsigned f=0;f<warmup;++f)frame(f);wait(signal());
    const auto start=std::chrono::steady_clock::now();
    for(unsigned f=warmup;f<warmup+frames;++f)frame(f);wait(signal());
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    const float net=nr.networkMilliseconds();if(!nr.close())throw std::runtime_error(nr.error());
    std::ofstream json(report);
    json<<"{\"pass\":true,\"width\":3840,\"height\":2160,\"modelScale\":"<<scale
        <<",\"warmupFrames\":"<<warmup<<",\"timedFrames\":"<<frames<<",\"submittedFrames\":"<<processed
        <<",\"elapsedMs\":"<<ms<<",\"processingFps\":"<<frames*1000/ms
        <<",\"nrGpuMeanMs\":"<<(gpuSamples?gpuSum/gpuSamples:0)<<",\"networkAverageMs\":"<<net
        <<",\"inFlightLimit\":4,\"distinctInputFrames\":8,\"fixtureCycles\":true,\"displayFps\":null,\"includesDecodeAudio\":false}\n";
    check(bool(json),"write report");std::cout<<"NR_STREAM processingFps="<<frames*1000/ms<<" meanGpuMs="<<(gpuSamples?gpuSum/gpuSamples:0)<<std::endl;return 0;
}
}
int wmain(int argc,wchar_t** argv){try{return run(argc,argv);}catch(const std::exception& e){std::cerr<<"NR_FAILED "<<e.what()<<std::endl;return 1;}}
