// SPDX-License-Identifier: GPL-3.0-only
// Calls the installed production Radeon bridge, scores actual interpolated pixels
// against an exact moving-scene midpoint. Private test process; no capture/UI.
#include "FgProbeSupport.h"
#include <nvsdk_ngx_helpers_dlssg_d3d.h>
#include <dxgi1_6.h>
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
using Init=NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long,const wchar_t*,ID3D12Device*,NVSDK_NGX_Version,const NVSDK_NGX_FeatureCommonInfo*);
using Create=NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
using Evaluate=NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback);
using Release=NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using Caps=NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
Create createFn=nullptr;Evaluate evaluateFn=nullptr;
PFN_NVSDK_NGX_Parameter_SetUI ui=nullptr;
PFN_NVSDK_NGX_Parameter_SetF sf=nullptr;
PFN_NVSDK_NGX_Parameter_SetD3d12Resource sr=nullptr;
PFN_NVSDK_NGX_Parameter_SetVoidPointer sp=nullptr;
template<class T>T proc(HMODULE m,const char* name){auto p=reinterpret_cast<T>(GetProcAddress(m,name));require(p!=nullptr,name);return p;}

}
// SDK helpers call these C entry points. Forward parameter calls to the host's
// allocator/ABI, and feature calls to the exact production bridge under test.
NVSDK_NGX_API void NVSDK_CONV NVSDK_NGX_Parameter_SetUI(NVSDK_NGX_Parameter*p,const char*n,unsigned v){ui(p,n,v);}
NVSDK_NGX_API void NVSDK_CONV NVSDK_NGX_Parameter_SetF(NVSDK_NGX_Parameter*p,const char*n,float v){sf(p,n,v);}
NVSDK_NGX_API void NVSDK_CONV NVSDK_NGX_Parameter_SetD3d12Resource(NVSDK_NGX_Parameter*p,const char*n,ID3D12Resource*v){sr(p,n,v);}
NVSDK_NGX_API void NVSDK_CONV NVSDK_NGX_Parameter_SetVoidPointer(NVSDK_NGX_Parameter*p,const char*n,void*v){sp(p,n,v);}
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList*l,NVSDK_NGX_Feature f,NVSDK_NGX_Parameter*p,NVSDK_NGX_Handle**h){return createFn(l,f,p,h);}
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList*l,const NVSDK_NGX_Handle*h,const NVSDK_NGX_Parameter*p,PFN_NVSDK_NGX_ProgressCallback c){return evaluateFn(l,h,const_cast<NVSDK_NGX_Parameter*>(p),c);}
NVSDK_NGX_API NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_EvaluateFeature_C(ID3D12GraphicsCommandList*l,const NVSDK_NGX_Handle*h,const NVSDK_NGX_Parameter*p,PFN_NVSDK_NGX_ProgressCallback_C c){require(c==nullptr,"unexpected progress callback");return evaluateFn(l,h,const_cast<NVSDK_NGX_Parameter*>(p),nullptr);}

int main(int argc,char**argv){try{
 require(argc>=5&&argc<=7,"usage: fg_image_probe out-dir walk|orbit zero|ideal|reverse|block flat|ideal [height] [bridge|normalized-positive|normalized-negative|pixel-positive|pixel-negative]");
 const auto outDir=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(outDir);
 const std::string mode=argv[3],depthMode=argv[4],scaleMode=argc==7?argv[6]:"bridge";unsigned height=argc>=6?unsigned(std::stoi(argv[5])):360,width=height*16/9;
 require(std::string(argv[2])=="walk"||std::string(argv[2])=="orbit","scene");
 require(mode=="zero"||mode=="ideal"||mode=="reverse"||mode=="block","motion mode");
 require(depthMode=="flat"||depthMode=="ideal","depth mode");
 require(scaleMode=="bridge"||scaleMode=="normalized-positive"||scaleMode=="normalized-negative"||scaleMode=="pixel-positive"||scaleMode=="pixel-negative","scale mode");
 require(height>=360&&height<=2160,"height range");Scene scene{width,height,std::string(argv[2])=="orbit",8.f*height/360};Host h;
 wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);const auto app=std::filesystem::path(exe).parent_path();
 HMODULE bridge=LoadLibraryExW((app/L"runtime/experimental/anr/nvngx_dlssg.dll").c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);require(bridge!=nullptr,"load production bridge");
 auto init=proc<Init>(bridge,"NVSDK_NGX_D3D12_Init_Ext");auto result=init(1,outDir.c_str(),h.device.Get(),NVSDK_NGX_Version_API,nullptr);std::cout<<"INIT "<<std::hex<<unsigned(result)<<std::dec<<std::endl;require(result==NVSDK_NGX_Result_Success,"bridge init");
 HMODULE host=GetModuleHandleW(L"neuralhub_host.dll");require(host!=nullptr,"host");ui=proc<decltype(ui)>(host,"NVSDK_NGX_Parameter_SetUI");sf=proc<decltype(sf)>(host,"NVSDK_NGX_Parameter_SetF");sr=proc<decltype(sr)>(host,"NVSDK_NGX_Parameter_SetD3d12Resource");sp=proc<decltype(sp)>(host,"NVSDK_NGX_Parameter_SetVoidPointer");
 auto caps=proc<Caps>(host,"NVSDK_NGX_D3D12_GetCapabilityParameters");NVSDK_NGX_Parameter* params=nullptr;require(caps(&params)==NVSDK_NGX_Result_Success&&params,"parameters");
 createFn=proc<Create>(bridge,"NVSDK_NGX_D3D12_CreateFeature");evaluateFn=proc<Evaluate>(bridge,"NVSDK_NGX_D3D12_EvaluateFeature");auto release=proc<Release>(bridge,"NVSDK_NGX_D3D12_ReleaseFeature");
 // Diagnostic-only override: same feature, textures and host, but the bridge's
 // hard-coded motion scale is bypassed to test the provider's unit convention.
 if(scaleMode!="bridge")evaluateFn=proc<Evaluate>(host,"NVSDK_NGX_D3D12_EvaluateFeature");
 NVSDK_NGX_DLSSG_Create_Params cd{};cd.Width=cd.RenderWidth=width;cd.Height=cd.RenderHeight=height;cd.NativeBackbufferFormat=DXGI_FORMAT_R8G8B8A8_UNORM;
 ui(params,NVSDK_NGX_DLSSG_Parameter_ResourceAlwaysProvided_Flags,NVSDK_NGX_DLSSG_ResourceFlags_Backbuffer|NVSDK_NGX_DLSSG_ResourceFlags_MVecs|NVSDK_NGX_DLSSG_ResourceFlags_Depth);
 ui(params,NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags,NVSDK_NGX_DLSSG_ResourceFlags_HUDLess|NVSDK_NGX_DLSSG_ResourceFlags_UI|NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha|NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField);
 ui(params,"DLSSG.EnableLiteMode",0);
 NVSDK_NGX_Handle* feature=nullptr;h.begin();result=NGX_D3D12_CREATE_DLSSG(h.list.Get(),1,1,&feature,params,&cd);std::cout<<"CREATE "<<std::hex<<unsigned(result)<<std::dec<<std::endl;require(result==NVSDK_NGX_Result_Success&&feature,"create");h.complete();
 auto color=makeTexture(h.device.Get(),width,height,DXGI_FORMAT_R8G8B8A8_UNORM,false),motion=makeTexture(h.device.Get(),width,height,DXGI_FORMAT_R16G16_FLOAT,false),depth=makeTexture(h.device.Get(),width,height,DXGI_FORMAT_R32_FLOAT,false),output=makeTexture(h.device.Get(),width,height,DXGI_FORMAT_R8G8B8A8_UNORM,true),disable=makeTexture(h.device.Get(),1,1,DXGI_FORMAT_R32_UINT,true);
 require(color&&motion&&depth&&output&&disable,"textures");Transfer uc(h,color.Get(),4,true),um(h,motion.Get(),4,true),ud(h,depth.Get(),4,true),read(h,output.Get(),4,false);StateTracker states;
 NVSDK_NGX_DLSSG_Opt_Eval_Params opt{};opt.multiFrameCount=opt.multiFrameIndex=1;opt.cameraNear=.1f;opt.cameraFar=1000;opt.cameraFOV=1.5707963f;opt.cameraAspectRatio=float(width)/height;opt.cameraUp[1]=opt.cameraRight[0]=opt.cameraFwd[2]=1;
 for(int r=0;r<4;++r)for(int c=0;c<4;++c){float v=r==c?1:0;opt.cameraViewToClip[r][c]=opt.clipToCameraView[r][c]=opt.clipToLensClip[r][c]=opt.clipToPrevClip[r][c]=opt.prevClipToClip[r][c]=v;}
 const float px=1/opt.cameraAspectRatio,pz=opt.cameraFar/(opt.cameraFar-opt.cameraNear),pw=-opt.cameraNear*pz;
 opt.cameraViewToClip[0][0]=px;opt.cameraViewToClip[2][2]=pz;opt.cameraViewToClip[2][3]=1;opt.cameraViewToClip[3][2]=pw;opt.cameraViewToClip[3][3]=0;
 opt.clipToCameraView[0][0]=1/px;opt.clipToCameraView[2][2]=0;opt.clipToCameraView[2][3]=1/pw;opt.clipToCameraView[3][2]=1;opt.clipToCameraView[3][3]=-pz/pw;
 opt.cameraMotionIncluded=true;opt.motionVectorsInvalidValue=3.402823466e38f;opt.mvecScale[0]=opt.mvecScale[1]=1;
 if(scaleMode=="pixel-negative")opt.mvecScale[0]=opt.mvecScale[1]=-1;
 if(scaleMode=="normalized-positive"||scaleMode=="normalized-negative"){float sign=scaleMode=="normalized-positive"?1.f:-1.f;opt.mvecScale[0]=sign/width;opt.mvecScale[1]=sign/height;}
 opt.backbufferSubrectSize=opt.outputInterpSubrectSize=opt.mvecsSubrectSize=opt.depthSubrectSize={width,height};
 Error edge,whole,hold,background;std::vector<Pixel> pixels(size_t(width)*height),truth(pixels.size());std::vector<std::array<uint16_t,2>> vectors(pixels.size());std::vector<float> depths(pixels.size());
 auto setUll=proc<PFN_NVSDK_NGX_Parameter_SetULL>(host,"NVSDK_NGX_Parameter_SetULL");
 for(unsigned frame=0;frame<14;++frame){
  for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){size_t i=size_t(y)*width+x;pixels[i]=scene.pixel(x,y,float(frame));truth[i]=scene.pixel(x,y,float(frame)-.5f);float xx=x+.5f,yy=y+.5f;if(mode=="block"){xx=float((x/16)*16+8);yy=float((y/16)*16+8);}bool fg=scene.distance(xx,yy,float(frame))<0;float v=fg?(scene.orbit?0:-scene.step):(scene.orbit?scene.step:0);if(mode=="zero")v=0;if(mode=="reverse")v=-v;if(frame==0)v=0;vectors[i]={half(v),0};depths[i]=depthMode=="ideal"?(fg?.98f:.999f):1.f;}
  uc.write(pixels.data());um.write(vectors.data());ud.write(depths.data());h.begin();
  for(auto pair:{std::pair{color.Get(),&uc},std::pair{motion.Get(),&um},std::pair{depth.Get(),&ud}}){states.transition(h.list.Get(),pair.first,D3D12_RESOURCE_STATE_COPY_DEST);pair.second->copy(h.list.Get(),pair.first,true);}
  states.transition(h.list.Get(),color.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);states.transition(h.list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);states.transition(h.list.Get(),disable.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if(scaleMode!="bridge"){states.transition(h.list.Get(),motion.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);states.transition(h.list.Get(),depth.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);}
  opt.reset=frame==0;setUll(params,NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID,frame+1);
  NVSDK_NGX_D3D12_DLSSG_Eval_Params ep{};ep.pBackbuffer=color.Get();ep.pMVecs=motion.Get();ep.pDepth=depth.Get();ep.pOutputInterpFrame=output.Get();ep.pOutputDisableInterpolation=disable.Get();
  result=NGX_D3D12_EVALUATE_DLSSG(h.list.Get(),feature,params,&ep,&opt);require(result==NVSDK_NGX_Result_Success,"evaluate");
  states.uavBarrier(h.list.Get(),output.Get());states.transition(h.list.Get(),output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);read.copy(h.list.Get(),output.Get(),false);h.complete();const auto generated=read.read();
  if(frame>=4){for(unsigned y=24;y<height-24;++y)for(unsigned x=24;x<width-24;++x){size_t i=size_t(y)*width+x;float d=scene.distance(x+.5f,y+.5f,float(frame)-.5f);whole.add(generated.data()+i*4,truth[i]);if(std::abs(d)<12.f*height/360){edge.add(generated.data()+i*4,truth[i]);hold.add(pixels[i].data(),truth[i]);}if(d>40.f*height/360)background.add(generated.data()+i*4,truth[i]);}}
  if(frame==10){save(outDir/"generated.ppm",width,height,generated.data());save(outDir/"truth.ppm",width,height,reinterpret_cast<uint8_t*>(truth.data()));save(outDir/"source-b.ppm",width,height,reinterpret_cast<uint8_t*>(pixels.data()));}
 }
 std::cout<<"PIXEL_RESULT {\"scene\":\""<<argv[2]<<"\",\"motion\":\""<<mode<<"\",\"depth\":\""<<depthMode<<"\",\"width\":"<<width<<",\"height\":"<<height<<",\"edgeMae\":"<<edge.mean()<<",\"edgeBadRatio\":"<<edge.badRatio()<<",\"wholeMae\":"<<whole.mean()<<",\"farMae\":"<<background.mean()<<",\"holdEdgeMae\":"<<hold.mean()<<"}"<<std::endl;
 require(release(feature)==NVSDK_NGX_Result_Success,"release");return 0;
 }catch(const std::exception&e){std::cerr<<"ERROR "<<e.what()<<std::endl;return 2;}}
