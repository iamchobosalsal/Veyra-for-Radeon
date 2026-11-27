// SPDX-License-Identifier: GPL-3.0-only
#include "FgProbeSupport.h"
#include "veyra/pipeline/RifeDmlBackend.h"

float unhalf(uint16_t h){const float a=std::ldexp(float(h&1023),-24);const unsigned e=(h>>10)&31;return (h&0x8000?-1.f:1.f)*(e?std::ldexp(1.f+float(h&1023)/1024,int(e)-15):a);}
float pq(float nits){const float p=std::pow(std::clamp(nits/10000.f,0.f,1.f),2610.f/16384.f);return std::pow((3424.f/4096.f+2413.f/128.f*p)/(1+2392.f/128.f*p),2523.f/32.f);}
int main(int argc,char** argv){try{
 require(argc==3,"usage: hdr_pipeline_probe VideoHdrExpand.dxil RIFE-runtime");
 Host h;StateTracker states;constexpr unsigned w=640,ht=360;
 auto input=makeTexture(h.device.Get(),w,ht,DXGI_FORMAT_R16G16B16A16_FLOAT,false);
 auto output=makeTexture(h.device.Get(),w,ht,DXGI_FORMAT_R16G16B16A16_FLOAT,true);
 Transfer upload(h,input.Get(),8,true),read(h,output.Get(),8,false);
 std::ifstream file(argv[1],std::ios::binary);std::vector<uint8_t> code((std::istreambuf_iterator<char>(file)),{});ComputePass pass;
 require(pass.create(h.device.Get(),code,2,1,1),"HDR shader");auto cpu=pass.heap->GetCPUDescriptorHandleForHeapStart();makeSrv(h.device.Get(),input.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,cpu);cpu.ptr+=pass.increment;makeUav(h.device.Get(),output.Get(),DXGI_FORMAT_R16G16B16A16_FLOAT,cpu);
 std::vector<uint16_t> pixels(w*ht*4);
 for(unsigned y=0;y<ht;y++)for(unsigned x=0;x<w;x++){float v=x<3?(x==0?0.f:x==1?.18f:1.f):float(x-3)/(w-4);for(unsigned c=0;c<3;c++)pixels[(y*w+x)*4+c]=half(v);pixels[(y*w+x)*4+3]=half(.75f);}
 upload.write(pixels.data());h.begin();states.transition(h.list.Get(),input.Get(),D3D12_RESOURCE_STATE_COPY_DEST);upload.copy(h.list.Get(),input.Get(),true);states.transition(h.list.Get(),input.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);states.transition(h.list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
 float constants[]{std::bit_cast<float>(w),std::bit_cast<float>(ht),125,75,44,1000,0,0};auto gpu=pass.heap->GetGPUDescriptorHandleForHeapStart();pass.bind(h.list.Get(),constants,gpu.ptr,gpu.ptr+pass.increment);h.list->Dispatch((w+15)/16,(ht+15)/16,1);states.uavBarrier(h.list.Get(),output.Get());states.transition(h.list.Get(),output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);read.copy(h.list.Get(),output.Get(),false);h.complete();auto bytes=read.read();auto val=[&](unsigned x,unsigned c=0){uint16_t v;memcpy(&v,bytes.data()+x*8+c*2,2);return unhalf(v);};
 require(val(0)==0,"black lifted");require(std::abs(val(1)*80-44)<.1,"18% gray incorrect");require(std::abs(val(2)*80-1000)<1,"peak clipped");
 for(unsigned x=3;x<w;x++){require(std::isfinite(val(x))&&val(x)>=0&&val(x)<=12.5f,"HDR invalid value");require(val(x)==val(x,1)&&val(x)==val(x,2),"neutral hue shift");require(val(x,3)==.75f,"alpha changed");if(x>3)require(val(x)>=val(x-1),"nonmonotone tone curve");}
 std::cout<<"HDR_EXPAND PASS black="<<val(0)*80<<" gray="<<val(1)*80<<" peak="<<val(2)*80<<" nits\n";
 RifeDmlBackend rife;h.begin();rife.open(h.device.Get(),h.list.Get(),std::filesystem::absolute(argv[2]),w,ht,1.f);h.complete();
 for(auto format:{DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT}){
  const unsigned bpp=format==DXGI_FORMAT_R10G10B10A2_UNORM?4:8;
  auto a=makeTexture(h.device.Get(),w,ht,format,false),b=makeTexture(h.device.Get(),w,ht,format,false),o=makeTexture(h.device.Get(),w,ht,format,true);
  Transfer ua(h,a.Get(),bpp,true),ub(h,b.Get(),bpp,true),rr(h,o.Get(),bpp,false);
  for(float nits:{0.f,1.f,44.f,203.f,600.f,1000.f,2000.f}){
   std::vector<uint8_t> data(size_t(w)*ht*bpp);unsigned q=unsigned(std::round(pq(nits)*1023));
   for(size_t i=0;i<size_t(w)*ht;i++)if(bpp==4){uint32_t v=q|(q<<10)|(q<<20)|0xc0000000;memcpy(data.data()+i*4,&v,4);}else{uint16_t v[4]{half(nits/80),half(nits/80),half(nits/80),half(1)};memcpy(data.data()+i*8,v,8);}
   ua.write(data.data());ub.write(data.data());h.begin();for(auto pair:{std::pair{a.Get(),&ua},std::pair{b.Get(),&ub}}){states.transition(h.list.Get(),pair.first,D3D12_RESOURCE_STATE_COPY_DEST);pair.second->copy(h.list.Get(),pair.first,true);states.transition(h.list.Get(),pair.first,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}states.transition(h.list.Get(),o.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);rife.record(h.list.Get(),a.Get(),b.Get(),o.Get());states.uavBarrier(h.list.Get(),o.Get());states.transition(h.list.Get(),o.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);rr.copy(h.list.Get(),o.Get(),false);h.complete();auto got=rr.read();double worst=0;
   for(size_t i=0;i<size_t(w)*ht;i++)for(unsigned c=0;c<3;c++){if(bpp==4){uint32_t v;memcpy(&v,got.data()+i*4,4);worst=std::max(worst,double(std::abs(int((v>>(10*c))&1023)-int(q))));}else{uint16_t v;memcpy(&v,got.data()+i*8+c*2,2);worst=std::max(worst,double(std::abs(unhalf(v)-unhalf(half(nits/80)))));}}
   require(worst<=(bpp==4?1.0:.03125),"RIFE HDR luminance was not preserved");std::cout<<"RIFE_HDR PASS format="<<format<<" nits="<<nits<<" worst="<<worst<<std::endl;
  }
 }
 return 0;
}catch(const std::exception&e){std::cerr<<"ERROR "<<e.what()<<std::endl;return 2;}}
