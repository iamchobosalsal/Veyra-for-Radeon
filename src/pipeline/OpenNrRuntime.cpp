// SPDX-License-Identifier: GPL-3.0-only
// OpenNR through the released player's versioned AMD C ABI. This is not HIP
// and is identified as OpenNR by GetStatus and VeyraOpenNrRuntimeVersion.
#define LMXXF_NR_RUNTIME_EXPORTS
#include "../../third_party/lmxxf/LmxxfNrApi.h"
#include "veyra/pipeline/OpenNrBackend.h"
#include "veyra/pipeline/AmdNrProvider.h"
#include "veyra/pipeline/GpuPassUtils.h"
#include <dxgi1_6.h>
#include <cstring>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <cstdio>

#ifndef VEYRA_OPEN_NR_ORIGINAL
#define VEYRA_OPEN_NR_ORIGINAL 0
#endif
namespace {
using namespace veyra::pipeline;
thread_local std::string error;
enum class State { Idle, Prepared, Inputs, Enqueued, Outputs };
struct Context {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Resource> output,source;
    std::filesystem::path root;
    OpenNrBackend backend;
    State state=State::Idle;
    bool reset=true,failed=false,outputUsed=false;
    uint32_t width=0,height=0;
    uint64_t frame=0,evaluations=0;
    float intensity=1;
    ID3D12GraphicsCommandList* inputList=nullptr;
};
// C ABI calls are serialized by the player's GPU-owner thread. Each layer owns
// its own context/history/resources. No process-wide environment is changed.
std::unordered_map<void*,std::unique_ptr<Context>> contexts;
int fail(const std::string& text,int code=LMXXF_NR_INVALID_ARGUMENT){error=text;return code;}
template<class F> int guard(F&& f) noexcept {
    try{return f();}catch(const std::exception& e){return fail(e.what(),LMXXF_NR_FAILED);}
    catch(...){return fail("OpenNR exception",LMXXF_NR_FAILED);}
}
Context* context(void* ptr){auto i=contexts.find(ptr);return i==contexts.end()?nullptr:i->second.get();}
bool job(Context* c,void* j,State s){return c&&j==c&&c->state==s&&!c->failed;}
std::filesystem::path runtimeRoot(const wchar_t* assets){return std::filesystem::path(assets).parent_path()/L"open-nr";}
bool assetsReady(const std::filesystem::path& root){
    return root.is_absolute()&&std::filesystem::is_regular_file(root/L"dlssnr-amd"/(VEYRA_OPEN_NR_ORIGINAL?L"dlssnr.bin":L"dlssnr-small.bin"))&&
           std::filesystem::is_regular_file(root/L"shaders/ParityEncode.dxil")&&std::filesystem::is_regular_file(root/L"shaders/ParityDecode.dxil")&&
           std::filesystem::is_regular_file(root/L"dlssnr-amd/shaders/shader-constants.txt");
}
int caps(LmxxfNrCapabilities* out){return guard([&]()->int{
    if(!out||out->struct_size!=sizeof(*out))return fail("OpenNR capability size");
    *out={sizeof(*out),LMXXF_NR_ABI_VERSION,1920,1080,1,0,0,0,1};return LMXXF_NR_OK;
});}
int create(const LmxxfNrCreateInfo* info,void** out){return guard([&]()->int{
    if(out)*out=nullptr;
    if(!info||!out||info->struct_size!=sizeof(*info)||!info->device||!info->queue||!info->assets_directory||info->flags)
        return fail("OpenNR create arguments (no silent fallback)");
    auto root=runtimeRoot(info->assets_directory);
    if(!assetsReady(root))return fail("OpenNR model or Veyra parity shaders missing",LMXXF_NR_UNAVAILABLE);
    auto c=std::make_unique<Context>();c->device=static_cast<ID3D12Device*>(info->device);c->queue=static_cast<ID3D12CommandQueue*>(info->queue);c->root=root;
    ComPtr<ID3D12Device> owner;
    if(FAILED(c->queue->GetDevice(IID_PPV_ARGS(&owner)))||owner.Get()!=c->device.Get()||c->queue->GetDesc().Type!=D3D12_COMMAND_LIST_TYPE_DIRECT)
        return fail("OpenNR queue/device mismatch");
    ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter1> adapter;DXGI_ADAPTER_DESC1 desc{};
    if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))||
       FAILED(factory->EnumAdapterByLuid(c->device->GetAdapterLuid(),IID_PPV_ARGS(&adapter)))||
       FAILED(adapter->GetDesc1(&desc))||amdNrProvider(desc.VendorId,desc.Description)!=AmdNrProvider::LmxxfRdna4)
        return fail("OpenNR RDNA4 shaders require a Radeon RX 9000 device; keep native NVIDIA NR on RTX",LMXXF_NR_UNAVAILABLE);
    auto key=c.get();contexts.emplace(key,std::move(c));*out=key;return LMXXF_NR_OK;
});}
int drain(void* p){return guard([&]()->int{
    auto c=context(p);if(!c)return fail("OpenNR invalid context");
    if(!c->backend.close())return fail(c->backend.error(),LMXXF_NR_FAILED);
    c->source.Reset();c->output.Reset();c->state=State::Idle;c->reset=true;c->outputUsed=false;c->width=c->height=0;return LMXXF_NR_OK;
});}
int destroy(void* p){return guard([&]()->int{if(auto r=drain(p);r!=LMXXF_NR_OK)return r;contexts.erase(p);return LMXXF_NR_OK;});}
int session(void* p){return context(p)?LMXXF_NR_OK:fail("OpenNR invalid session");}
int prepare(void* p,const LmxxfNrFrameInfo* info,LmxxfNrJob* out){return guard([&]()->int{
    auto c=context(p);
    if(!c||!info||!out||out->struct_size!=sizeof(*out)||c->failed||c->state!=State::Idle||
       (info->struct_size!=sizeof(*info)&&info->struct_size!=LMXXF_NR_FRAME_INFO_V1_SIZE))return fail("OpenNR frame state/size");
    out->handle=out->private_output=nullptr;
    if(!info->color||!info->command_list||!info->color_width||!info->color_height||info->color_width>2560||info->color_height>1080||
       uint64_t(info->color_width)*info->color_height>1920*1080||info->color_state!=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE||
       (info->flags&~LMXXF_NR_FRAME_FLAG_STRENGTH)||info->debug_view||info->model_scale!=1)
        return fail("OpenNR supports linear SDR, native model extent and normal output only");
    const float strength=(info->flags&LMXXF_NR_FRAME_FLAG_STRENGTH)?info->transfer_strength:1.f;
    if(!std::isfinite(strength)||strength<0||strength>1||((info->flags&LMXXF_NR_FRAME_FLAG_STRENGTH)&&info->color_strength!=1))
        return fail("OpenNR strength must be finite 0..1; colour resolve belongs to Veyra");
    if(info->struct_size==sizeof(*info)&&(info->exposure||info->pre_exposure!=1||info->exposure_scale!=1))
        return fail("OpenNR expects Veyra's already-normalised linear frame");
    auto source=static_cast<ID3D12Resource*>(info->color);auto d=source->GetDesc();ComPtr<ID3D12Device> owner;
    if(FAILED(source->GetDevice(IID_PPV_ARGS(&owner)))||owner.Get()!=c->device.Get()||d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||
       d.Width!=info->color_width||d.Height!=info->color_height||d.Format!=DXGI_FORMAT_R16G16B16A16_FLOAT||d.MipLevels!=1||d.DepthOrArraySize!=1||d.SampleDesc.Count!=1)
        return fail("OpenNR source texture mismatch");
    if(!c->backend.ready()||c->width!=d.Width||c->height!=d.Height){
        if(!c->backend.open(c->root,c->device.Get(),c->queue.Get(),info->color_width,info->color_height,!VEYRA_OPEN_NR_ORIGINAL,true,1.f,true)){
            c->failed=true;return fail(c->backend.error(),LMXXF_NR_UNAVAILABLE);
        }
        c->output=makeTexture(c->device.Get(),info->color_width,info->color_height,DXGI_FORMAT_R16G16B16A16_FLOAT,false);
        if(!c->output){c->failed=true;return fail("OpenNR output allocation",LMXXF_NR_FAILED);}
        c->width=info->color_width;c->height=info->color_height;c->reset=true;c->outputUsed=false;
    }
    c->source=source;c->frame=info->frame_id;c->intensity=strength;c->inputList=static_cast<ID3D12GraphicsCommandList*>(info->command_list);
    c->state=State::Prepared;out->handle=c;out->private_output=c->output.Get();return LMXXF_NR_OK;
});}
int inputs(void* p,void* j,void* list){return guard([&]()->int{
    auto c=context(p);if(!job(c,j,State::Prepared)||list!=c->inputList)return fail("OpenNR input order/list");
    if(!c->backend.recordInputs(c->inputList,c->source.Get(),c->frame,c->reset,c->intensity)){
        c->failed=true;return fail(c->backend.error(),LMXXF_NR_FAILED);
    }
    c->state=State::Inputs;return LMXXF_NR_OK;
});}
int enqueue(void* p,void* j,void* queue){return guard([&]()->int{
    auto c=context(p);if(!job(c,j,State::Inputs)||queue!=c->queue.Get())return fail("OpenNR enqueue order/queue");
    if(!c->backend.enqueue()){c->failed=true;return fail(c->backend.error(),LMXXF_NR_FAILED);}
    c->state=State::Enqueued;++c->evaluations;c->reset=false;return LMXXF_NR_OK;
});}
int outputs(void* p,void* j,void* list){return guard([&]()->int{
    auto c=context(p);if(!job(c,j,State::Enqueued)||!list)return fail("OpenNR output order");
    auto cmd=static_cast<ID3D12GraphicsCommandList*>(list);
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    // The resource is COMMON on its first use; subsequent jobs leave it SRV.
    b.Transition={c->output.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,c->outputUsed?D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE:D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST};
    cmd->ResourceBarrier(1,&b);
    if(!c->backend.recordOutputs(cmd,c->output.Get())){c->failed=true;return fail(c->backend.error(),LMXXF_NR_FAILED);}
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;b.Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;cmd->ResourceBarrier(1,&b);
    c->outputUsed=true;c->state=State::Outputs;return LMXXF_NR_OK;
});}
int retire(void* p,void* j){return guard([&]()->int{
    auto c=context(p);if(!job(c,j,State::Outputs))return fail("OpenNR retire order");
    if(!c->backend.retireSubmitted()){c->failed=true;return fail(c->backend.error(),LMXXF_NR_FAILED);}
    c->state=State::Idle;c->source.Reset();return LMXXF_NR_OK;
});}
int cancel(void* p,void* j){return guard([&]()->int{
    auto c=context(p);if(!job(c,j,State::Prepared))return fail("OpenNR only unrecorded jobs may be cancelled");
    c->state=State::Idle;c->source.Reset();return LMXXF_NR_OK;
});}
int reset(void* p){auto c=context(p);if(!c||c->state!=State::Idle)return fail("OpenNR reset order");c->reset=true;return LMXXF_NR_OK;}
int poll(void* p,void* j,uint32_t* state){auto c=context(p);if(!c||j!=c||!state)return fail("OpenNR poll arguments");
    *state=c->state==State::Outputs?LMXXF_NR_JOB_CONSUMER_COMPLETE:c->state==State::Enqueued?LMXXF_NR_JOB_NR_ENQUEUED:
        c->state==State::Inputs?LMXXF_NR_JOB_PRODUCER_SUBMITTED:c->state==State::Prepared?LMXXF_NR_JOB_PREPARED:LMXXF_NR_JOB_NONE;
    return LMXXF_NR_OK;
}
int status(void* p,char* buf,uint32_t size){auto c=context(p);if(!c||!buf||!size)return fail("OpenNR status arguments");
    std::snprintf(buf,size,"OpenNR Vulkan; model=%s; codec=Veyra-NVIDIA-parity; internal=%ux%u; evaluations=%llu; failed=%u; custom tone/structure/style unavailable in released AMD ABI",
        VEYRA_OPEN_NR_ORIGINAL?"Original":"Small",c->width,c->height,static_cast<unsigned long long>(c->evaluations),c->failed?1:0);return LMXXF_NR_OK;
}
int lastError(char* buf,uint32_t size){if(!buf||!size)return LMXXF_NR_INVALID_ARGUMENT;std::snprintf(buf,size,"%s",error.c_str());return LMXXF_NR_OK;}
int timings(void* p,LmxxfNrTimings* out){if(!context(p)||!out||out->struct_size!=sizeof(*out))return fail("OpenNR timing arguments");
    // The upstream average has no completed-frame identity. Never report it as
    // a per-frame HIP timing. Veyra's full-stage GPU timestamps remain usable.
    *out={sizeof(*out),0,0,0,0};return LMXXF_NR_OK;
}
int execute(void*,void*,void*){return fail("Use the split producer/consumer submission contract",LMXXF_NR_NOT_IMPLEMENTED);}
}
extern "C" __declspec(dllexport) uint32_t VeyraOpenNrRuntimeVersion(){return 1;}
extern "C" __declspec(dllexport) uint32_t VeyraLmxxfModelMode(){return VEYRA_OPEN_NR_ORIGINAL;}
extern "C" __declspec(dllexport) uint32_t VeyraLmxxfControlsCompatible(){return 1;}
extern "C" __declspec(dllexport) uint32_t VeyraLmxxfModelAssetsReady(const wchar_t* assets){
    try{return assets&&assetsReady(runtimeRoot(assets));}catch(...){return 0;}
}
extern "C" LMXXF_NR_EXPORT int32_t LmxxfNrGetApi(uint32_t version,LmxxfNrApi* out){
    if(!out||version!=LMXXF_NR_ABI_VERSION)return LMXXF_NR_UNSUPPORTED_ABI;
    const uint32_t size=out->struct_size;if(size!=sizeof(*out)&&size!=LMXXF_NR_API_V1_SIZE)return LMXXF_NR_INVALID_ARGUMENT;
    LmxxfNrApi api{size,LMXXF_NR_ABI_VERSION,caps,create,destroy,session,prepare,inputs,enqueue,outputs,execute,cancel,poll,retire,reset,drain,status,lastError,timings};
    std::memcpy(out,&api,size);return LMXXF_NR_OK;
}
