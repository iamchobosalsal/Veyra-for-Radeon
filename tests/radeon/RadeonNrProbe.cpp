// SPDX-License-Identifier: GPL-3.0-only
// Offline real-model experiment. CPU conversion/readback is diagnostic only;
// this executable is NOT the real-time capture/display path.
#ifdef VEYRA_OPEN_NR_PROBE
#include "veyra/pipeline/OpenNrBackend.h"
#include <shellapi.h>
#else
#include "veyra/pipeline/LmxxfNrBackend.h"
#endif
#include "veyra/pipeline/AmdHipPreflight.h"
#include "veyra/pipeline/AmdNrProvider.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include "veyra/gfx/GpuSchedulingPriority.h"
#include <dxgi1_6.h>
#include <d3d12sdklayers.h>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>

using namespace veyra;
using Microsoft::WRL::ComPtr;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void hr(HRESULT code, const char* message) { check(SUCCEEDED(code), message); }
// Input values are positive finite [0,1]; round to nearest, ties to even.
uint16_t half(float f) {
    const uint32_t b = std::bit_cast<uint32_t>(f);
    const int e = int((b >> 23) & 255) - 127 + 15;
    if (e <= 0) {
        if (e < -10) return 0;
        const uint32_t m = (b & 0x7fffff) | 0x800000;
        const unsigned shift = unsigned(14 - e);
        return uint16_t((m + ((1u << (shift-1))-1) + ((m >> shift)&1)) >> shift);
    }
    const uint32_t m = (b & 0x7fffff) + 0xfff + ((b >> 13)&1);
    return uint16_t((uint32_t(e) << 10) + (m >> 13));
}
float unhalf(uint16_t b) {
    const unsigned e = (b >> 10) & 31, m = b & 1023;
    float f = e == 0 ? std::ldexp(float(m), -24) :
              e == 31 ? (m ? NAN : INFINITY) : std::ldexp(float(1024+m), int(e)-25);
    return b & 0x8000 ? -f : f;
}
struct Event { HANDLE h = CreateEventW(nullptr,FALSE,FALSE,nullptr); ~Event(){if(h)CloseHandle(h);} };
double elapsed(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();
}

int run(int argc, wchar_t** argv) {
    if (argc != 10
#ifdef VEYRA_OPEN_NR_PROBE
        && argc != 11 && argc != 12
#endif
    ) {
        std::cerr << "Usage: radeon_nr_probe runtime input.rgba output.rgba report.json width height frames small|original strength\n";
        return 2;
    }
    const std::filesystem::path runtime=std::filesystem::absolute(argv[1]), input=argv[2], output=argv[3], report=argv[4];
    const unsigned w=std::stoul(argv[5]), h=std::stoul(argv[6]), frames=std::stoul(argv[7]);
    const std::wstring model=argv[8]; const float strength=std::stof(argv[9]);
#ifdef VEYRA_OPEN_NR_PROBE
    check(w && h && w<=3840 && h<=2160 && frames, "invalid extent/frame count");
#else
    check(w && h && w<=2560 && h<=1080 && uint64_t(w)*h<=1920*1080 && frames, "invalid extent/frame count");
#endif
    check(model==L"small" || model==L"original", "unknown model");
    bool resetMidSequence=true;
#ifdef VEYRA_OPEN_NR_PROBE
    if(argc==12){
        check(std::wstring_view(argv[11])==L"--continuous", "expected --continuous after model scale");
        resetMidSequence=false;
    }
#endif
    check(std::isfinite(strength)&&strength>=0&&strength<=1, "strength must be in [0,1]");
    check(!std::filesystem::exists(output)&&!std::filesystem::exists(report), "output/report already exists");
    const size_t bytes=size_t(w)*h*4;
    check(std::filesystem::file_size(input)==uint64_t(bytes)*frames, "raw input size does not match frames");
    std::ifstream in(input,std::ios::binary);
    std::ofstream out(output,std::ios::binary);
    check(bool(in)&&bool(out),"cannot open input/output");

    ComPtr<ID3D12Debug> debug;
    const bool debugAvailable=SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));
    if(debugAvailable)debug->EnableDebugLayer();
    ComPtr<IDXGIFactory6> factory; hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");
    ComPtr<ID3D12Device> device; ComPtr<IDXGIAdapter3> adapter; DXGI_ADAPTER_DESC1 desc{};
    for(UINT i=0;!device;++i){
        ComPtr<IDXGIAdapter1> candidate;
        if(FAILED(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&candidate))))break;
        hr(candidate->GetDesc1(&desc),"adapter description");
        if(pipeline::amdNrProvider(desc.VendorId,desc.Description)!=pipeline::AmdNrProvider::LmxxfRdna4)continue;
        if(SUCCEEDED(D3D12CreateDevice(candidate.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))))hr(candidate.As(&adapter),"adapter3");
    }
    check(bool(device),"no supported RX 9000 adapter");
    std::wcout<<L"NR_ADAPTER "<<desc.Description<<std::endl;
    gfx::requestGpuPriority(gfx::GpuPriority::High);
    wchar_t executable[32768]{};
    check(GetModuleFileNameW(nullptr,executable,32768)!=0,"executable path");
    pipeline::amdhip::setPreflightExecutable(executable);
    ComPtr<ID3D12InfoQueue> infoQueue; device.As(&infoQueue);
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC q{};
    hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
    ComPtr<ID3D12CommandAllocator> alloc[2]; ComPtr<ID3D12GraphicsCommandList> list[2];
    for(unsigned i=0;i<2;++i){
        hr(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&alloc[i])),"allocator");
        hr(device->CreateCommandList(0,q.Type,alloc[i].Get(),nullptr,IID_PPV_ARGS(&list[i])),"list");
        hr(list[i]->Close(),"initial close");
    }
    ComPtr<ID3D12Fence> fence; hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
    Event event; check(event.h,"event"); uint64_t serial=0;
    auto begin=[&](unsigned i){hr(alloc[i]->Reset(),"reset allocator");hr(list[i]->Reset(alloc[i].Get(),nullptr),"reset list");};
    auto submit=[&](unsigned i){hr(list[i]->Close(),"close list");ID3D12CommandList* lists[]{list[i].Get()};queue->ExecuteCommandLists(1,lists);};
    auto complete=[&]{
        hr(queue->Signal(fence.Get(),++serial),"signal");
        hr(fence->SetEventOnCompletion(serial,event.h),"completion event");
        check(WaitForSingleObject(event.h,60000)==WAIT_OBJECT_0,"GPU completion timeout");
        hr(device->GetDeviceRemovedReason(),"device removed");
    };
    auto color=pipeline::makeTexture(device.Get(),w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,false);
    auto result=pipeline::makeTexture(device.Get(),w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,false);
    check(color&&result,"textures");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{}; UINT64 totalBytes=0;
    const auto td=color->GetDesc(); device->GetCopyableFootprints(&td,0,1,0,&footprint,nullptr,nullptr,&totalBytes);
    auto upload=pipeline::makeUploadBuffer(device.Get(),totalBytes);check(bool(upload),"upload");
    auto bd=upload->GetDesc(); D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback;
    hr(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"readback");
    pipeline::StateTracker states;
#ifdef VEYRA_OPEN_NR_PROBE
    pipeline::OpenNrBackend nr;
    const float modelScale=argc>=11?std::stof(argv[10]):1.f;
    const auto initStart=std::chrono::steady_clock::now();
    if(!nr.open(runtime,device.Get(),queue.Get(),w,h,model==L"small",true,modelScale))throw std::runtime_error(nr.error());
    std::cout<<"NR_INIT ms="<<elapsed(initStart)<<" frame="<<w<<'x'<<h<<" modelScale="<<modelScale<<std::endl;
#else
    pipeline::LmxxfNrBackend nr;
    if(!nr.open(runtime,device.Get(),queue.Get(),desc.VendorId,model==L"small"))throw std::runtime_error(nr.error());
#endif
    check(nr.admits(w,h),"runtime rejects extent");
    std::array<uint16_t,256> linear{};
    for(unsigned i=0;i<256;++i){float v=float(i)/255;linear[i]=half(v<=.04045f?v/12.92f:std::pow((v+.055f)/1.055f,2.4f));}
    std::array<uint8_t,65536> srgb{};
    for(unsigned i=0;i<65536;++i){float v=unhalf(uint16_t(i));if(!std::isfinite(v))v=0;v=std::clamp(v,0.f,1.f);v=v<=.0031308f?v*12.92f:1.055f*std::pow(v,1/2.4f)-.055f;srgb[i]=uint8_t(std::lround(v*255));}
    std::vector<uint8_t> source(bytes),dest(bytes);
    std::vector<double> durations; uint64_t changed=0,nonfinite=0,absoluteError=0,peakMemory=0;
    std::ofstream json(report);check(bool(json),"report open");
    json<<std::setprecision(8)<<"{\n\"model\":\""<<(model==L"small"?"small":"original")<<"\",\"width\":"<<w<<",\"height\":"<<h
        <<",\"framesRequested\":"<<frames<<",\"strength\":"<<strength<<",\"debugLayer\":"<<(debugAvailable?"true":"false")<<",\"frameResults\":[\n";
    for(unsigned f=0;f<frames;++f){
        in.read(reinterpret_cast<char*>(source.data()),std::streamsize(bytes));check(bool(in),"input read");
        uint8_t* mapped=nullptr;hr(upload->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"upload map");
        for(unsigned y=0;y<h;++y){auto* row=reinterpret_cast<uint16_t*>(mapped+size_t(y)*footprint.Footprint.RowPitch);
            for(unsigned x=0;x<w;++x){size_t p=(size_t(y)*w+x)*4;for(unsigned c=0;c<3;++c)row[x*4+c]=linear[source[p+c]];row[x*4+3]=0x3c00;}}
        upload->Unmap(0,nullptr);
        const auto start=std::chrono::steady_clock::now();
        begin(0);states.transition(list[0].Get(),color.Get(),D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprint;
        to.pResource=color.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list[0]->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        states.transition(list[0].Get(),color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        // Diagnostic reset remains the default; video previews retain history.
        const bool reset=f==0||(resetMidSequence&&frames>2&&f==frames/2);
        if(!nr.recordInputs(list[0].Get(),color.Get(),f+1,reset,strength))throw std::runtime_error(nr.error());
        submit(0);
        if(!nr.enqueue())throw std::runtime_error(nr.error());
        begin(1);states.transition(list[1].Get(),result.Get(),D3D12_RESOURCE_STATE_COPY_DEST);
        if(!nr.recordOutputs(list[1].Get(),result.Get()))throw std::runtime_error(nr.error());
        states.transition(list[1].Get(),result.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);
        from={};from.pResource=result.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to={};to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
        list[1]->CopyTextureRegion(&to,0,0,0,&from,nullptr);submit(1);
        if(!nr.retireSubmitted())throw std::runtime_error(nr.error());
        complete();durations.push_back(elapsed(start));
        uint8_t* pixels=nullptr;hr(readback->Map(0,nullptr,reinterpret_cast<void**>(&pixels)),"readback map");
        uint64_t frameChanged=0,frameInvalid=0,frameError=0;
        for(unsigned y=0;y<h;++y){const auto* row=reinterpret_cast<const uint16_t*>(pixels+size_t(y)*footprint.Footprint.RowPitch);
            for(unsigned x=0;x<w;++x){size_t p=(size_t(y)*w+x)*4;bool different=false;
                for(unsigned c=0;c<3;++c){const uint16_t v=row[x*4+c];frameInvalid+=((v&0x7c00)==0x7c00);dest[p+c]=srgb[v];const int diff=std::abs(int(dest[p+c])-source[p+c]);frameError+=diff;different|=diff>2;}
                dest[p+3]=source[p+3];frameChanged+=different;}}
        readback->Unmap(0,nullptr);out.write(reinterpret_cast<const char*>(dest.data()),std::streamsize(bytes));check(bool(out),"output write");
        changed+=frameChanged;nonfinite+=frameInvalid;absoluteError+=frameError;
        DXGI_QUERY_VIDEO_MEMORY_INFO memory{};hr(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory),"memory query");peakMemory=std::max(peakMemory,memory.CurrentUsage);
        json<<(f?",\n":"")<<"{\"index\":"<<f<<",\"reset\":"<<(reset?"true":"false")<<",\"completionMs\":"<<durations.back()<<",\"changedPixels\":"<<frameChanged<<",\"nonfiniteRgb\":"<<frameInvalid;
#ifdef VEYRA_OPEN_NR_PROBE
        json<<",\"gpuMs\":"<<nr.gpuMilliseconds()<<",\"networkAverageMs\":"<<nr.networkMilliseconds()<<",\"estimatedHistoryConsumed\":"<<(nr.historyConsumed()?"true":"false");
#endif
        json<<"}";json.flush();
        std::cout<<"NR_FRAME "<<f<<" completionMs="<<durations.back()<<" changed="<<frameChanged<<" nonfinite="<<frameInvalid;
#ifdef VEYRA_OPEN_NR_PROBE
        std::cout<<" gpuMs="<<nr.gpuMilliseconds()<<" networkMs="<<nr.networkMilliseconds()<<" history="<<nr.historyConsumed();
#endif
        std::cout<<std::endl;
    }
#ifdef VEYRA_OPEN_NR_PROBE
    if(!nr.close()) throw std::runtime_error(nr.error());
#else
    nr.close();
#endif
    complete();
    uint64_t debugErrors=0;
    if(infoQueue){for(UINT64 i=0;i<infoQueue->GetNumStoredMessages();++i){SIZE_T size=0;infoQueue->GetMessage(i,nullptr,&size);std::vector<uint8_t> storage(size);auto* msg=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        if(SUCCEEDED(infoQueue->GetMessage(i,msg,&size))&&(msg->Severity==D3D12_MESSAGE_SEVERITY_ERROR||msg->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION)){++debugErrors;std::cerr<<"D3D12_ERROR "<<msg->pDescription<<'\n';}}}
    std::vector<double> steady(durations.begin()+std::min(size_t(2),durations.size()-1),durations.end());
    const double avg=std::accumulate(steady.begin(),steady.end(),0.)/steady.size();std::sort(steady.begin(),steady.end());
    const bool pass=!nonfinite&&!debugErrors;
    json<<"\n],\"framesCompleted\":"<<frames<<",\"nonfiniteRgb\":"<<nonfinite<<",\"debugErrors\":"<<debugErrors
#ifdef VEYRA_OPEN_NR_PROBE
        <<",\"backend\":\"source-built-open-nr\",\"modelScale\":"<<modelScale<<",\"motionSource\":\"estimated\""
#endif
        <<",\"changedPixelFraction\":"<<double(changed)/(uint64_t(w)*h*frames)<<",\"meanAbsoluteRgbCodeDifference\":"<<double(absoluteError)/(uint64_t(w)*h*frames*3)
        <<",\"coldCompletionMs\":"<<durations[0]<<",\"steadyCompletionMeanMs\":"<<avg<<",\"steadyCompletionP95Ms\":"<<steady[size_t(std::ceil(.95*steady.size()))-1]
        <<",\"sampledProcessLocalMemoryPeakMiB\":"<<double(peakMemory)/(1024*1024)<<",\"displayFps\":null,\"gpuOnlyMs\":null,\"pass\":"<<(pass?"true":"false")<<"\n}\n";
    check(bool(json),"report write");return pass?0:1;
}
}
int wmain(int argc,wchar_t** argv){
#ifdef VEYRA_OPEN_NR_PROBE
    if(argc==1){
        wchar_t filename[32768]{};
        GetModuleFileNameW(nullptr,filename,32768);
        const auto launcher=std::filesystem::path(filename).parent_path()/L"VeyraOpenNR.exe";
        if(std::filesystem::is_regular_file(launcher)){
            const auto result=ShellExecuteW(nullptr,L"open",launcher.c_str(),nullptr,launcher.parent_path().c_str(),SW_SHOWNORMAL);
            if(reinterpret_cast<INT_PTR>(result)>32)return 0;
        }
        MessageBoxW(nullptr,L"영상 시험창 구성 파일을 찾지 못했습니다.\nC:\\capture\\run\\Veyra-OpenNR-Preview\\VeyraOpenNR.exe를 실행해 주세요.",L"Veyra OpenNR",MB_OK|MB_ICONERROR);
        return 2;
    }
#endif
    if(argc>1&&std::wstring_view(argv[1])==L"--amd-hip-preflight")return pipeline::amdhip::preflightMain(argc,argv);
    try{return run(argc,argv);}catch(const std::exception& e){std::cerr<<"NR_FAILED "<<e.what()<<std::endl;return 1;}
}
