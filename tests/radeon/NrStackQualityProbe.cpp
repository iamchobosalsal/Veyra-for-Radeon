// SPDX-License-Identifier: GPL-3.0-only
// Offline real-model stack diagnostic. Readbacks/waits are test-only.
#include "veyra/pipeline/EnhanceGraph.h"
#include "veyra/pipeline/AmdHipPreflight.h"
#include "veyra/gfx/D3D12DeviceContext.h"
#include "veyra/gfx/CommandSlotRing.h"
#include "veyra/gfx/GpuSchedulingPriority.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <stdexcept>
extern "C" {
#include <libavutil/frame.h>
}
using namespace veyra;
namespace {
void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
void dump(gfx::D3D12DeviceContext& ctx,gfx::CommandSlotRing& ring,
          pipeline::EnhanceGraph& graph,ID3D12Resource* texture,const std::filesystem::path& path){
    require(texture&&!std::filesystem::exists(path),"missing texture or existing output");
    const auto d=texture->GetDesc();
    require(d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT,"expected FP16");
    const unsigned rowBytes=unsigned(d.Width)*8,pitch=(rowBytes+255)&~255u;
    D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width=UINT64(pitch)*d.Height;bd.Height=bd.DepthOrArraySize=bd.MipLevels=bd.SampleDesc.Count=1;
    bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
    Microsoft::WRL::ComPtr<ID3D12Resource> rb;
    require(SUCCEEDED(ctx.device()->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&rb))),"readback allocation");
    Status status;unsigned slot;auto* list=ring.acquireNext(slot,status);require(list,"readback list");
    const auto before=graph.diagnosticResourceState(texture);
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={texture,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,D3D12_RESOURCE_STATE_COPY_SOURCE};
    if(before!=D3D12_RESOURCE_STATE_COPY_SOURCE)list->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=texture;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource=rb.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.PlacedFootprint.Footprint={d.Format,unsigned(d.Width),d.Height,1,pitch};
    list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
    if(before!=D3D12_RESOURCE_STATE_COPY_SOURCE)list->ResourceBarrier(1,&barrier);
    require(ring.submitAndSignal(slot)&&ring.waitIdle(),"readback wait");
    void* data=nullptr;D3D12_RANGE range{0,SIZE_T(bd.Width)};
    require(SUCCEEDED(rb->Map(0,&range,&data)),"readback map");
    std::ofstream out(path,std::ios::binary);
    for(unsigned y=0;y<d.Height;++y)out.write(static_cast<char*>(data)+size_t(y)*pitch,rowBytes);
    D3D12_RANGE empty{};rb->Unmap(0,&empty);require(bool(out),"readback write");
}
int run(int argc,wchar_t** argv){
    require(argc>=9&&argc<=12,"usage: probe runtimeRoot source.rgba outputDir width height layers frames nrWidth [lastResidualGain] [originalModelBitmask] [flow]");
    const auto runtime=std::filesystem::absolute(argv[1]),input=std::filesystem::path(argv[2]),out=std::filesystem::path(argv[3]);
    const unsigned w=std::stoul(argv[4]),h=std::stoul(argv[5]),layers=std::stoul(argv[6]),frames=std::stoul(argv[7]),nrW=std::stoul(argv[8]),nrH=nrW*h/w;
    require(w&&h&&layers<=4&&frames&&nrW&&nrH,"bad dimensions");
    std::filesystem::create_directories(out);
    require(std::filesystem::file_size(input)==uint64_t(w)*h*4*frames,"input length");
    wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);pipeline::amdhip::setPreflightExecutable(exe);
    gfx::requestGpuPriority(gfx::GpuPriority::High);
    gfx::D3D12DeviceContext ctx;gfx::CommandSlotRing ring;Status status=Status::Ok;
    gfx::DeviceContextDesc device;
    require(ctx.initialize(device,status)&&ring.initialize(ctx.device(),ctx.directQueue(),ctx.fence(),ctx.fenceEvent(),8,status),"device init");
    pipeline::EnhanceGraphDesc d;
    d.sourceWidth=d.workWidth=w;d.sourceHeight=d.workHeight=h;d.rgbInput=true;
    d.enableNr=layers>0;d.enableSr=d.enableFg=false;
    d.nrRuntime=engine::NrRuntime::LmxxfAmd;
    d.nrMotion=d.srMotion=d.fgMotion=engine::MotionSource::Zero;
    d.runtimeAbsPath=(runtime/L"nvidia").wstring();
    d.nrWidth=nrW;d.nrHeight=nrH;
    d.nrLayersModel.resize(layers);d.nrLayersResidual.resize(layers);
    if(argc>=10&&layers)d.nrLayersResidual.back().total=std::stof(argv[9]);
    const unsigned originals=argc>=11?std::stoul(argv[10]):0;
    require(originals<(1u<<layers),"original model bitmask out of range");
    for(unsigned i=0;i<layers;++i){auto& m=d.nrLayersModel[i];m.performance=(originals&(1u<<i))==0;m.style=0;m.intensity=1;}
    if(argc==12){
        require(std::wstring_view(argv[11])==L"flow","unknown motion mode");
        d.nrMotion=engine::MotionSource::OpticalFlow;
        d.enableNvofStandalone=true;
        d.opticalFlowBackend=engine::OpticalFlowBackend::AmdFidelityFx;
    }
    d.nrLayersTemporal.assign(layers,false);
    d.nrLayersSizePolicy.assign(layers,pipeline::NrSizePolicy::Native);
    d.nrLayersExtent.assign(layers,{nrW,nrH});
    pipeline::EnhanceGraph graph(ctx,ring);
    require(graph.initialize(d)&&graph.createViews(),"graph init");
    AVFrame* frame=av_frame_alloc();require(frame,"frame allocation");
    frame->format=AV_PIX_FMT_RGBA;frame->width=int(w);frame->height=int(h);
    frame->color_range=AVCOL_RANGE_JPEG;frame->color_trc=AVCOL_TRC_IEC61966_2_1;frame->color_primaries=AVCOL_PRI_BT709;
    require(av_frame_get_buffer(frame,32)>=0,"frame buffer");
    std::ifstream stream(input,std::ios::binary);
    for(unsigned f=0;f<frames;++f){
        for(unsigned y=0;y<h;++y)stream.read(reinterpret_cast<char*>(frame->data[0]+y*frame->linesize[0]),size_t(w)*4);
        require(bool(stream),"input read");
        pipeline::EnhanceGraph::FrameOutputs result;
        require(graph.process(frame,f*33.333,f==0||(frames>2&&f==frames-1),result,f+1)&&ring.waitIdle(),"graph process");
        const auto timing=graph.gpuMetrics();
        for(unsigned i=0;i<layers;++i){const auto& sample=timing.gpu[size_t(diagnostics::nrLayerStage(i))];
            if(sample.milliseconds)std::cout<<"frame="<<f<<" layer="<<i+1<<" gpuQueueMs="<<*sample.milliseconds<<std::endl;
        }
        const auto name="frame"+std::to_string(f);
        dump(ctx,ring,graph,graph.diagnosticNrBase(),out/(name+"-base.rgba16f"));
        for(unsigned i=0;i<layers;++i){
            dump(ctx,ring,graph,graph.diagnosticNrLayerInput(i),out/(name+"-input"+std::to_string(i+1)+".rgba16f"));
            dump(ctx,ring,graph,graph.diagnosticNrLayerNeural(i),out/(name+"-neural"+std::to_string(i+1)+".rgba16f"));
            dump(ctx,ring,graph,graph.diagnosticNrLayerOutput(i),out/(name+"-layer"+std::to_string(i+1)+".rgba16f"));
        }
        std::cout<<"frame="<<f<<" layers="<<layers<<" evaluated="<<graph.metrics().nrEvaluateCount<<std::endl;
    }
    require(graph.metrics().nrEvaluateCount==uint64_t(frames)*layers,"all layers must infer");
    av_frame_free(&frame);ring.drainQueue();graph.shutdown();return 0;
}
}
int wmain(int argc,wchar_t** argv){
    if(argc>1&&std::wstring_view(argv[1])==L"--amd-hip-preflight")return pipeline::amdhip::preflightMain(argc,argv);
    try{return run(argc,argv);}catch(const std::exception& e){std::cerr<<"STACK_FAILED "<<e.what()<<std::endl;return 1;}
}
