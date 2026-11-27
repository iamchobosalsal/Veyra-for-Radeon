// SPDX-License-Identifier: GPL-3.0-only
// ABI validation and RTX rejection; deliberately does not run RDNA4 shaders.
#include "../../../third_party/lmxxf/LmxxfNrApi.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <cstring>
using Microsoft::WRL::ComPtr;
namespace {
unsigned checks=0;
void require(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
void test(const std::filesystem::path& dir,unsigned original,ID3D12Device* device,ID3D12CommandQueue* queue){
    const auto path=dir/(original?L"LmxxfNrRuntime-Original.dll":L"LmxxfNrRuntime.dll");
    HMODULE module=LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
    require(module!=nullptr,"source-built module load");
    const auto get=reinterpret_cast<decltype(&LmxxfNrGetApi)>(GetProcAddress(module,"LmxxfNrGetApi"));
    const auto mode=reinterpret_cast<uint32_t(*)()>(GetProcAddress(module,"VeyraLmxxfModelMode"));
    const auto version=reinterpret_cast<uint32_t(*)()>(GetProcAddress(module,"VeyraOpenNrRuntimeVersion"));
    const auto ready=reinterpret_cast<uint32_t(*)(const wchar_t*)>(GetProcAddress(module,"VeyraLmxxfModelAssetsReady"));
    require(get&&mode&&version&&ready,"all ABI and identity exports");
    require(mode()==original&&version()==1,"model identity");
    require(!ready(nullptr)&&!ready(L"relative/missing/assets"),"missing assets rejected");
    const auto assets=dir/L"assets";require(ready(assets.c_str())==1,"complete private assets admitted");
    LmxxfNrApi api{};api.struct_size=sizeof(api);require(get(999,&api)==LMXXF_NR_UNSUPPORTED_ABI,"unknown ABI rejected");
    api.struct_size=4;require(get(1,&api)==LMXXF_NR_INVALID_ARGUMENT,"short table rejected");
    api={};api.struct_size=LMXXF_NR_API_V1_SIZE;require(get(1,&api)==LMXXF_NR_OK&&!api.GetTimings,"v1 table does not overrun");
    api={};api.struct_size=sizeof(api);require(get(1,&api)==LMXXF_NR_OK&&api.GetTimings,"extended table negotiated");
    LmxxfNrCapabilities caps{};require(api.QueryCapabilities(&caps)==LMXXF_NR_INVALID_ARGUMENT,"bad caps size");
    caps.struct_size=sizeof(caps);require(api.QueryCapabilities(&caps)==LMXXF_NR_OK&&caps.hip_ready==0&&caps.history_supported==1,"Vulkan capabilities do not claim HIP readiness");
    void* context=reinterpret_cast<void*>(1);require(api.Create(nullptr,&context)==LMXXF_NR_INVALID_ARGUMENT&&!context,"bad create clears handle");
    LmxxfNrCreateInfo info{};info.struct_size=sizeof(info);info.device=device;info.queue=queue;info.assets_directory=assets.c_str();info.flags=1;
    require(api.Create(&info,&context)==LMXXF_NR_INVALID_ARGUMENT&&!context,"zero-output fallback rejected");
    info.flags=0;const auto missing=dir/L"missing/assets";info.assets_directory=missing.c_str();
    require(api.Create(&info,&context)==LMXXF_NR_UNAVAILABLE&&!context,"create cannot silently substitute a missing model");
    info.assets_directory=assets.c_str();require(api.Create(&info,&context)==LMXXF_NR_UNAVAILABLE&&!context,"RTX must use native NR, not RDNA4 shaders");
    char error[1024]{};api.GetLastError(error,sizeof(error));require(std::strstr(error,"Radeon RX 9000")!=nullptr,"wrong GPU has actionable error");
    require(api.PrepareSession(nullptr)==LMXXF_NR_INVALID_ARGUMENT,"session invalid context");
    require(api.PrepareFrame(nullptr,nullptr,nullptr)==LMXXF_NR_INVALID_ARGUMENT,"frame invalid context");
    require(api.RecordInputs(nullptr,nullptr,nullptr)==LMXXF_NR_INVALID_ARGUMENT,"input invalid context");
    require(api.EnqueueHip(nullptr,nullptr,nullptr)==LMXXF_NR_INVALID_ARGUMENT,"enqueue invalid context");
    require(api.RecordOutputs(nullptr,nullptr,nullptr)==LMXXF_NR_INVALID_ARGUMENT,"output invalid context");
    require(api.Retire(nullptr,nullptr)==LMXXF_NR_INVALID_ARGUMENT,"retire invalid context");
    require(api.ResetHistory(nullptr)==LMXXF_NR_INVALID_ARGUMENT,"reset invalid context");
    require(api.Drain(nullptr)==LMXXF_NR_INVALID_ARGUMENT,"drain invalid context");
    require(api.Destroy(nullptr)==LMXXF_NR_INVALID_ARGUMENT,"destroy invalid context");
    require(api.ExecuteAfterProducer(nullptr,nullptr,nullptr)==LMXXF_NR_NOT_IMPLEMENTED,"unsupported submission explicit");
    FreeLibrary(module);
}
}
int wmain(int argc,wchar_t** argv){try{
    require(argc==2,"usage: open_nr_abi_tests <absolute staged runtime/amd-nr>");
    ComPtr<IDXGIFactory6> factory;require(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))),"factory");
    ComPtr<ID3D12Device> device;
    for(UINT i=0;!device;++i){ComPtr<IDXGIAdapter1> adapter;
        if(FAILED(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))))break;
        DXGI_ADAPTER_DESC1 desc{};adapter->GetDesc1(&desc);if(desc.VendorId==0x10de)D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device));
    }
    require(bool(device),"this negative test requires the retained NVIDIA GPU");
    D3D12_COMMAND_QUEUE_DESC q{};ComPtr<ID3D12CommandQueue> queue;
    require(SUCCEEDED(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))),"queue");
    test(std::filesystem::path(argv[1]),0,device.Get(),queue.Get());test(std::filesystem::path(argv[1]),1,device.Get(),queue.Get());
    std::cout<<"ABI_PASS checks="<<checks<<" neuralInferenceTested=false\n";return 0;
}catch(const std::exception& e){std::cerr<<"ABI_FAIL "<<e.what()<<'\n';return 1;}}
