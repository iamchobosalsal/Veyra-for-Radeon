// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "veyra/pipeline/GpuPassUtils.h"
#include <dxgi1_6.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace veyra::pipeline;
namespace {
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
void hr(HRESULT value,const char* why){require(SUCCEEDED(value),why);}
uint16_t half(float v){const uint32_t b=std::bit_cast<uint32_t>(v);if(v==0)return 0;return uint16_t(((b>>16)&0x8000)|(((b>>23)&255)-112)<<10|((b>>13)&1023));}
struct Host{
 ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> alloc;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;
 HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);UINT64 serial=0;
 Host(){ComPtr<IDXGIFactory6> factory;hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
  for(UINT i=0;!device;++i){ComPtr<IDXGIAdapter1> adapter;if(FAILED(factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&adapter))))break;
   DXGI_ADAPTER_DESC1 d{};adapter->GetDesc1(&d);if(d.VendorId!=0x1002||(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE))continue;
   if(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device))))std::wcout<<L"ADAPTER "<<d.Description<<std::endl;}
  require(bool(device),"AMD GPU required");D3D12_COMMAND_QUEUE_DESC q{};hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
  hr(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&alloc)),"allocator");hr(device->CreateCommandList(0,q.Type,alloc.Get(),nullptr,IID_PPV_ARGS(&list)),"list");hr(list->Close(),"close");
  hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");require(event!=nullptr,"event");}
 ~Host(){CloseHandle(event);}
 void begin(){hr(alloc->Reset(),"allocator reset");hr(list->Reset(alloc.Get(),nullptr),"reset");}
 void complete(){hr(list->Close(),"close");ID3D12CommandList* batch[]{list.Get()};queue->ExecuteCommandLists(1,batch);hr(queue->Signal(fence.Get(),++serial),"signal");hr(fence->SetEventOnCompletion(serial,event),"event completion");require(WaitForSingleObject(event,20000)==WAIT_OBJECT_0,"GPU timeout");hr(device->GetDeviceRemovedReason(),"device removed");}
};
struct Transfer{
 ComPtr<ID3D12Resource> resource;D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};unsigned w,h,bpp;
 Transfer(Host& c,ID3D12Resource* texture,unsigned bytes,bool upload):bpp(bytes){const auto d=texture->GetDesc();w=unsigned(d.Width);h=d.Height;UINT64 size=0;c.device->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&size);
  if(upload)resource=makeUploadBuffer(c.device.Get(),size);else{D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;D3D12_RESOURCE_DESC r{};r.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;r.Width=size;r.Height=1;r.DepthOrArraySize=r.MipLevels=1;r.SampleDesc.Count=1;r.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;hr(c.device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&r,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&resource)),"readback");}require(bool(resource),"transfer");}
 void write(const void* data){uint8_t* p=nullptr;hr(resource->Map(0,nullptr,reinterpret_cast<void**>(&p)),"upload map");for(unsigned y=0;y<h;++y)memcpy(p+y*fp.Footprint.RowPitch,static_cast<const uint8_t*>(data)+size_t(y)*w*bpp,w*bpp);resource->Unmap(0,nullptr);}
 std::vector<uint8_t> read(){uint8_t* p=nullptr;hr(resource->Map(0,nullptr,reinterpret_cast<void**>(&p)),"readback map");std::vector<uint8_t> out(size_t(w)*h*bpp);for(unsigned y=0;y<h;++y)memcpy(out.data()+size_t(y)*w*bpp,p+y*fp.Footprint.RowPitch,w*bpp);resource->Unmap(0,nullptr);return out;}
 void copy(ID3D12GraphicsCommandList* list,ID3D12Resource* texture,bool upload){D3D12_TEXTURE_COPY_LOCATION buf{},tex{};buf.pResource=resource.Get();buf.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;buf.PlacedFootprint=fp;tex.pResource=texture;tex.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(upload?&tex:&buf,0,0,0,upload?&buf:&tex,nullptr);}
};
using Pixel=std::array<uint8_t,4>;
float capsule(float x,float y,float ax,float ay,float bx,float by,float radius){float px=x-ax,py=y-ay,dx=bx-ax,dy=by-ay;float t=std::clamp((px*dx+py*dy)/std::max(.001f,dx*dx+dy*dy),0.f,1.f);return std::hypot(px-t*dx,py-t*dy)-radius;}
struct Scene{
 unsigned w,h;bool orbit=false;float step=8;
 float distance(float x,float y,float frame)const{
  x=(x-(orbit?0:frame*step))*640/w;y=y*360/h;
  return std::min({capsule(x,y,280,85,280,85,22),capsule(x,y,280,130,280,205,35),capsule(x,y,263,210,256,315,13),capsule(x,y,298,210,306,315,13),capsule(x,y,244,135,225,210,11),capsule(x,y,316,135,337,210,11)})*w/640;
 }
 Pixel pixel(unsigned x,unsigned y,float frame)const{
  float dist=distance(x+.5f,y+.5f,frame),cover=std::clamp(.5f-dist,0.f,1.f);
  float xx=x+(orbit?frame*step:0),yy=float(y);float tex=22*std::sin(xx*.19f)+15*std::sin(yy*.23f)+12*std::sin((xx+yy)*.083f);
  std::array<float,3> bg{95+tex,130+tex,80+tex};if(int(xx)%113<5)bg={35,40,30};
  const float cx=x-(orbit?0:frame*step);float ct=16*std::sin(cx*.41f)+14*std::sin(yy*.31f);
  std::array<float,3> fg{180+ct,85+ct,55+ct};if(yy<110.f*h/360)fg={165+ct,125+ct,95+ct};
  return {uint8_t(std::clamp(bg[0]*(1-cover)+fg[0]*cover,0.f,255.f)),uint8_t(std::clamp(bg[1]*(1-cover)+fg[1]*cover,0.f,255.f)),uint8_t(std::clamp(bg[2]*(1-cover)+fg[2]*cover,0.f,255.f)),255};
 }
};
void save(const std::filesystem::path& path,unsigned w,unsigned h,const uint8_t* rgba){std::ofstream out(path,std::ios::binary);out<<"P6\n"<<w<<' '<<h<<"\n255\n";for(size_t i=0;i<size_t(w)*h;++i)out.write(reinterpret_cast<const char*>(rgba+i*4),3);require(bool(out),"save image");}
struct Error{double sum=0;uint64_t count=0,bad=0;void add(const uint8_t* got,const Pixel& truth){float d=0;for(unsigned c=0;c<3;++c)d+=std::abs(int(got[c])-int(truth[c]));d/=3;sum+=d;++count;bad+=d>20;}double mean()const{return sum/std::max<uint64_t>(count,1);}double badRatio()const{return double(bad)/std::max<uint64_t>(count,1);}};
}
