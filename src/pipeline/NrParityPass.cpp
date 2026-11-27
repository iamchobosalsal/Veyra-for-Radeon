// SPDX-License-Identifier: GPL-3.0-only
#include "veyra/pipeline/NrParityPass.h"
#include <bit>
#include <fstream>

namespace veyra::pipeline {
namespace {
std::vector<uint8_t> read(const std::filesystem::path& path) {
    std::ifstream f(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};
}
D3D12_CPU_DESCRIPTOR_HANDLE cpu(const ComputePass& p,UINT slot) {
    return {p.heap->GetCPUDescriptorHandleForHeapStart().ptr+SIZE_T(slot)*p.increment};
}
UINT64 gpu(const ComputePass& p,UINT slot) {
    return p.heap->GetGPUDescriptorHandleForHeapStart().ptr+UINT64(slot)*p.increment;
}
void sourceTransition(ID3D12GraphicsCommandList* cmd,ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};cmd->ResourceBarrier(1,&b);
}
}
bool NrParityPass::open(ID3D12Device* d,const std::filesystem::path& shaders,uint32_t w,uint32_t h) {
    if(!d||!w||!h||!shaders.is_absolute())return false;
    width_=w;height_=h;
    linear_=makeTexture(d,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,false);
    proxy_=makeTexture(d,w,h,DXGI_FORMAT_R8G8B8A8_UNORM,true);
    neural_=makeTexture(d,w,h,DXGI_FORMAT_R8G8B8A8_UNORM,false);
    decoded_=makeTexture(d,w,h,DXGI_FORMAT_R16G16B16A16_FLOAT,true);
    auto enc=read(shaders/L"ParityEncode.dxil"),dec=read(shaders/L"ParityDecode.dxil");
    if(!linear_||!proxy_||!neural_||!decoded_||enc.empty()||dec.empty()||
       !encode_.create(d,enc,2,1,1)||!decode_.create(d,dec,4,3,1))return false;
    DescriptorStager stager;if(!stager.initialize(d,4))return false;
    stager.stageSrv(linear_.Get(),nullptr,encode_.heap.Get(),0);
    makeUav(d,proxy_.Get(),DXGI_FORMAT_R8G8B8A8_UNORM,cpu(encode_,1));
    stager.stageSrv(linear_.Get(),nullptr,decode_.heap.Get(),0);
    stager.stageSrv(proxy_.Get(),nullptr,decode_.heap.Get(),1);
    stager.stageSrv(neural_.Get(),nullptr,decode_.heap.Get(),2);
    makeUav(d,decoded_.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,cpu(decode_,3));
    return true;
}
void NrParityPass::encode(ID3D12GraphicsCommandList* cmd,ID3D12Resource* input) {
    sourceTransition(cmd,input,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    states_.transition(cmd,linear_.Get(),D3D12_RESOURCE_STATE_COPY_DEST);cmd->CopyResource(linear_.Get(),input);
    sourceTransition(cmd,input,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    states_.transition(cmd,linear_.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    states_.transition(cmd,proxy_.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const float c[8]={1,1,1,0,std::bit_cast<float>(width_),std::bit_cast<float>(height_),0,0};
    encode_.bind(cmd,c,gpu(encode_,0),gpu(encode_,1));cmd->Dispatch((width_+15)/16,(height_+15)/16,1);
    states_.uavBarrier(cmd,proxy_.Get());states_.transition(cmd,proxy_.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}
void NrParityPass::decode(ID3D12GraphicsCommandList* cmd,ID3D12Resource* destination,bool identity) {
    auto* output=identity?linear_.Get():decoded_.Get();
    if(!identity){
        states_.transition(cmd,neural_.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        states_.transition(cmd,decoded_.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const float c[8]={1,1,1,0,std::bit_cast<float>(width_),std::bit_cast<float>(height_),0,0};
        decode_.bind(cmd,c,gpu(decode_,0),gpu(decode_,3));cmd->Dispatch((width_+15)/16,(height_+15)/16,1);
        states_.uavBarrier(cmd,decoded_.Get());
    }
    states_.transition(cmd,output,D3D12_RESOURCE_STATE_COPY_SOURCE);cmd->CopyResource(destination,output);
}
}
