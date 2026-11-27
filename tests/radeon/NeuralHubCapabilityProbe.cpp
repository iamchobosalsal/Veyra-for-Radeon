// SPDX-License-Identifier: GPL-3.0-only
// Internal integration diagnostic: calls the installed provider's published
// NGX entry points. Does not copy, patch, or redistribute the provider.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_params_dlssg.h>
#include <filesystem>
#include <iostream>

using Microsoft::WRL::ComPtr;
int wmain(int argc,wchar_t** argv){
    if(argc!=3&&argc!=4)return 2;
    const auto dll=std::filesystem::absolute(argv[1]);
    const auto data=std::filesystem::absolute(argv[2]);
    std::filesystem::create_directories(data);
    HMODULE bootstrap=nullptr;
    if(argc==4){
        const auto loader=std::filesystem::absolute(argv[3]);
        bootstrap=LoadLibraryExW(loader.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!bootstrap){std::cout<<"bootstrapError="<<GetLastError()<<std::endl;return 9;}
        using Tick=DWORD(WINAPI*)();
        if(auto tick=reinterpret_cast<Tick>(GetProcAddress(bootstrap,"timeGetTime")))tick();
        for(int i=0;i<50&&!GetModuleHandleW(L"neuralhub_host.dll");++i)Sleep(100);
    }
    ComPtr<IDXGIFactory6> factory;
    if(FAILED(CreateDXGIFactory2(0,IID_PPV_ARGS(&factory))))return 3;
    ComPtr<ID3D12Device> device;
    for(UINT i=0;;++i){
        ComPtr<IDXGIAdapter1> adapter;
        if(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 desc{};adapter->GetDesc1(&desc);
        if(desc.VendorId!=0x1002||(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))continue;
        if(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)))){
            std::wcout<<L"adapter="<<desc.Description<<std::endl;break;
        }
    }
    if(!device)return 4;
    HMODULE module=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!module){std::cout<<"loadError="<<GetLastError()<<std::endl;return 5;}
    using Init=NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long,const wchar_t*,ID3D12Device*,NVSDK_NGX_Version,const NVSDK_NGX_FeatureCommonInfo*);
    using Caps=NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
    using Destroy=NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
    using Shutdown=NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
    auto init=reinterpret_cast<Init>(GetProcAddress(module,"NVSDK_NGX_D3D12_Init_Ext"));
    auto caps=reinterpret_cast<Caps>(GetProcAddress(module,"NVSDK_NGX_D3D12_GetCapabilityParameters"));
    auto destroy=reinterpret_cast<Destroy>(GetProcAddress(module,"NVSDK_NGX_D3D12_DestroyParameters"));
    auto shutdown=reinterpret_cast<Shutdown>(GetProcAddress(module,"NVSDK_NGX_D3D12_Shutdown1"));
    if(!init||!caps||!destroy||!shutdown)return 6;
    std::cout<<"init-start"<<std::endl;
    const auto initialized=init(1,data.c_str(),device.Get(),NVSDK_NGX_Version_API,nullptr);
    std::cout<<"init=0x"<<std::hex<<unsigned(initialized)<<std::endl;
    if(initialized!=NVSDK_NGX_Result_Success)return 7;
    NVSDK_NGX_Parameter* parameters=nullptr;
    const auto result=caps(&parameters);
    std::cout<<"capability=0x"<<unsigned(result)<<" parameters="<<(parameters!=nullptr)<<std::endl;
    int available=0;unsigned maximum=0;
    if(parameters){
        const auto a=parameters->Get("FrameGeneration.Available",&available);
        const auto m=parameters->Get("DLSSG.MultiFrameCountMax",&maximum);
        std::cout<<"available="<<available<<" get=0x"<<unsigned(a)<<" max="<<std::dec<<maximum<<" get=0x"<<std::hex<<unsigned(m)<<std::endl;
        destroy(parameters);
    }
    const auto stopped=shutdown(device.Get());
    std::cout<<"shutdown=0x"<<unsigned(stopped)<<std::endl;
    FreeLibrary(module);
    return result==NVSDK_NGX_Result_Success&&available&&maximum?0:8;
}
