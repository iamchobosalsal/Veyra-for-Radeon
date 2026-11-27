// SPDX-License-Identifier: GPL-3.0-only
// Private offline integration entrypoint for NrStackQualityProbe. Production
// graph/settings/model code is used unchanged; readbacks are diagnostic only.
#include "veyra/engine/GraphDescription.h"
#include "veyra/RuntimePaths.h"
#include "veyra/Log.h"
#include <DirectXPackedVector.h>
#include <chrono>
#include <cstring>
#include <dxgi1_4.h>
#include <cmath>
#include <set>
#include <iomanip>
namespace veyra::test {
inline void integrationRequire(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct IntegrationPixels {
    uint32_t width=0,height=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;
    uint64_t hash=14695981039346656037ull,nonfinite=0;
    double minimum=1e30,maximum=-1e30,mean=0,variance=0;
};
inline IntegrationPixels inspectIntegrationTexture(gfx::D3D12DeviceContext& ctx,gfx::CommandSlotRing& ring,
        pipeline::EnhanceGraph& graph,ID3D12Resource* texture,const std::filesystem::path& cropPath={}) {
    integrationRequire(texture!=nullptr,"output texture missing");const auto d=texture->GetDesc();
    integrationRequire(d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT||d.Format==DXGI_FORMAT_R10G10B10A2_UNORM||d.Format==DXGI_FORMAT_R8G8B8A8_UNORM,"unexpected output format");
    const uint32_t bytesPerPixel=d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;
    const uint32_t rowBytes=uint32_t(d.Width)*bytesPerPixel,pitch=(rowBytes+255)&~255u;
    D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=UINT64(pitch)*d.Height;
    bd.Height=bd.DepthOrArraySize=bd.MipLevels=bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;Microsoft::WRL::ComPtr<ID3D12Resource> rb;
    integrationRequire(SUCCEEDED(ctx.device()->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&rb))),"integration readback allocation");
    Status status;unsigned slot;auto* list=ring.acquireNext(slot,status);integrationRequire(list,"integration readback list");
    const auto before=graph.diagnosticResourceState(texture);D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={texture,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,D3D12_RESOURCE_STATE_COPY_SOURCE};
    if(before!=D3D12_RESOURCE_STATE_COPY_SOURCE)list->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=texture;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource=rb.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint.Footprint={d.Format,uint32_t(d.Width),d.Height,1,pitch};
    list->CopyTextureRegion(&to,0,0,0,&from,nullptr);std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
    if(before!=D3D12_RESOURCE_STATE_COPY_SOURCE)list->ResourceBarrier(1,&barrier);
    integrationRequire(ring.submitAndSignal(slot)&&ring.waitIdle(),"integration readback completion");
    void* data=nullptr;D3D12_RANGE range{0,SIZE_T(bd.Width)};integrationRequire(SUCCEEDED(rb->Map(0,&range,&data)),"integration readback map");
    IntegrationPixels result;result.width=uint32_t(d.Width);result.height=d.Height;result.format=d.Format;
    const auto decode=[&](const uint8_t* pixel,unsigned channel){
        if(d.Format==DXGI_FORMAT_R16G16B16A16_FLOAT){uint16_t value;std::memcpy(&value,pixel+channel*2,2);return double(DirectX::PackedVector::XMConvertHalfToFloat(value));}
        if(d.Format==DXGI_FORMAT_R10G10B10A2_UNORM){uint32_t value;std::memcpy(&value,pixel,4);return double((value>>(channel*10))&1023)/1023;}
        return pixel[channel]/255.;
    };
    double sum=0,sumSquared=0;uint64_t count=0;
    for(unsigned y=0;y<d.Height;++y){const auto* row=static_cast<const uint8_t*>(data)+size_t(y)*pitch;
        for(unsigned byte=0;byte<rowBytes;++byte){result.hash^=row[byte];result.hash*=1099511628211ull;}
        // Sample every fourth pixel/row for finite/range/variance validation;
        // every byte still participates in the output identity hash.
        if(y%4)continue;
        for(unsigned x=0;x<d.Width;x+=4)for(unsigned c=0;c<3;++c){const double v=decode(row+size_t(x)*bytesPerPixel,c);
            if(!std::isfinite(v)){++result.nonfinite;continue;}result.minimum=std::min(result.minimum,v);result.maximum=std::max(result.maximum,v);sum+=v;sumSquared+=v*v;++count;}
    }
    if(!cropPath.empty()){
        integrationRequire(!std::filesystem::exists(cropPath),"refusing to overwrite crop");
        const unsigned w=std::min(640u,result.width),h=std::min(360u,result.height),x=std::min(result.width-w,uint32_t(result.width*0.31)-w/2),y=std::min(result.height-h,uint32_t(result.height*0.39)-h/2);
        std::ofstream crop(cropPath,std::ios::binary);for(unsigned row=0;row<h;++row)crop.write(static_cast<const char*>(data)+size_t(y+row)*pitch+size_t(x)*bytesPerPixel,size_t(w)*bytesPerPixel);
        integrationRequire(bool(crop),"crop write");
    }
    D3D12_RANGE empty{};rb->Unmap(0,&empty);result.mean=sum/std::max<uint64_t>(count,1);result.variance=sumSquared/std::max<uint64_t>(count,1)-result.mean*result.mean;
    integrationRequire(result.nonfinite==0&&result.minimum>=-1&&result.maximum<64&&result.variance>1e-6,"invalid, uniform, or nonfinite graph pixels");
    return result;
}
inline int nrIntegrationMain(int argc,wchar_t** argv) {
    integrationRequire(argc==7,"usage: --integration source4k.rgba output-dir layers off|rife|fsr sdr|hdr frames");
    const std::filesystem::path input=std::filesystem::absolute(argv[1]),out=std::filesystem::absolute(argv[2]);
    const unsigned layers=std::stoul(argv[3]),frames=std::stoul(argv[6]);const std::wstring_view fgName=argv[4],hdrName=argv[5];
    integrationRequire(layers>=1&&layers<=3&&frames>=4&&frames<=6,"integration layers/frames");
    integrationRequire(fgName==L"off"||fgName==L"rife"||fgName==L"fsr","integration FG backend");
    integrationRequire(hdrName==L"sdr"||hdrName==L"hdr","integration HDR mode");
    constexpr unsigned w=3840,h=2160;const bool useFg=fgName!=L"off",hdr=hdrName==L"hdr";
    integrationRequire(std::filesystem::file_size(input)==uint64_t(w)*h*4*frames,"integration fixture length");
    std::filesystem::create_directories(out);Logger::instance().openFile((out/L"engine.log").wstring());Logger::instance().setConsoleEnabled(false);
    wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);pipeline::amdhip::setPreflightExecutable(exe);gfx::requestGpuPriority(gfx::GpuPriority::Normal);
    gfx::D3D12DeviceContext ctx;gfx::CommandSlotRing ring;Status status=Status::Ok;gfx::DeviceContextDesc device;
    integrationRequire(ctx.initialize(device,status)&&ctx.adapter().vendorId==0x1002&&ring.initialize(ctx.device(),ctx.directQueue(),ctx.fence(),ctx.fenceEvent(),8,status),"integration AMD device");
    engine::EnhancementSettings settings;settings.nr=true;settings.nrRuntime=engine::NrRuntime::LmxxfAmd;settings.nrPolicy=pipeline::NrSizePolicy::Realtime;
    settings.model.performance=true;settings.model.style=0;settings.model.intensity=1;settings.nrTemporal=false;settings.nrHoldStrength=0;settings.nrReuse=engine::NrReuseMode::Off;
    settings.nrLayerCount=layers;for(unsigned i=0;i<layers;++i){auto& layer=settings.nrLayers[i];layer.enabled=true;layer.runtime=settings.nrRuntime;layer.model=settings.model;layer.temporal=false;layer.sizePolicy=settings.nrPolicy;}
    settings.nrMotion=engine::MotionSource::OpticalFlow;settings.opticalFlowBackend=engine::OpticalFlowBackend::AmdFidelityFx;
    settings.frameGenerationBackend=fgName==L"fsr"?engine::FrameGenerationBackend::Fsr:engine::FrameGenerationBackend::Anr;
    settings.multiplier=useFg?2:1;settings.videoHdr.enabled=hdr;settings.videoHdr.peakNits=600;
    integrationRequire(settings.validate().empty(),"integration settings validation");
    engine::StageRequest request;request.nr=true;request.fg=useFg;request.fgMultiplier=2;request.width=w;request.height=h;request.nvidiaAdapter=false;request.amdNr=true;request.uncompressedSource=true;
    pipeline::EnhanceGraphDesc desc;engine::describeStages(request,settings,desc);desc.rgbInput=true;desc.hdrOutput=hdr;desc.runtimeAbsPath=runtime::localRuntimeDirectory().wstring();
    integrationRequire(desc.fixedExecutionPlanError.empty()&&desc.workWidth==w&&desc.workHeight==h&&desc.nrLayersExtent.size()==layers,"production resolution/chain mapping");
    for(const auto& extent:desc.nrLayersExtent)integrationRequire(extent==pipeline::Extent{1920,1080},"expected Radeon realtime NR extent");
    pipeline::EnhanceGraph graph(ctx,ring);integrationRequire(graph.initialize(desc)&&graph.createViews(),"integration graph init");
    integrationRequire(graph.nrEnabled()&&graph.nrCreated()&&graph.nrLayerCount()==layers&&graph.videoHdrActive()==hdr&&(!useFg||graph.fgCreated()),"requested stages actually active");
    Microsoft::WRL::ComPtr<IDXGIFactory4> factory;Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
    if(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))factory->EnumAdapterByLuid(ctx.device()->GetAdapterLuid(),IID_PPV_ARGS(&adapter));
    uint64_t peakVram=0,generatedCount=0,realCount=0;std::set<uint64_t> layerHashes;
    AVFrame* frame=av_frame_alloc();integrationRequire(frame,"integration frame allocation");frame->format=AV_PIX_FMT_RGBA;frame->width=w;frame->height=h;
    frame->color_range=AVCOL_RANGE_JPEG;frame->color_trc=AVCOL_TRC_BT709;frame->color_primaries=AVCOL_PRI_BT709;integrationRequire(av_frame_get_buffer(frame,32)>=0,"integration frame buffer");
    std::ifstream source(input,std::ios::binary);std::ofstream observations(out/L"observations.jsonl");const auto started=std::chrono::steady_clock::now();
    for(unsigned f=0;f<frames;++f){for(unsigned y=0;y<h;++y)source.read(reinterpret_cast<char*>(frame->data[0]+size_t(y)*frame->linesize[0]),w*4);integrationRequire(bool(source),"integration input read");
        const bool reset=f==0||f==frames-1;pipeline::EnhanceGraph::FrameOutputs result;
        integrationRequire(graph.process(frame,f*1000./30,reset,result,f+1)&&ring.waitIdle()&&graph.resolveGeneration(result),"integration graph process/resolve");
        integrationRequire(result.batch.count==(useFg&&!reset?2u:1u),"expected real/generated batch count");
        bool realSeen=false,generatedSeen=false;
        for(unsigned i=0;i<result.batch.count;++i){const auto& item=result.batch.frames[i];integrationRequire(item.lease&&item.lease->texture&&item.lease->ready(),"completed output lease");
            integrationRequire(item.identity.sourceFrameId==f+1,"source identity preserved");
            if(item.kind==pipeline::FrameKind::Generated){integrationRequire(useFg&&!reset&&item.validity==pipeline::GenerationValidity::Valid,"generated frame validity");++generatedCount;generatedSeen=true;}
            else {integrationRequire(item.kind==pipeline::FrameKind::Real,"no silent repeated-frame substitution");++realCount;realSeen=true;}
            const auto td=item.lease->texture->GetDesc();integrationRequire(td.Width==w&&td.Height==h&&td.Format==(hdr?DXGI_FORMAT_R10G10B10A2_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM),"final 4K SDR/HDR format");
            const std::string kind=item.kind==pipeline::FrameKind::Generated?"generated":"real";
            const auto pixels=inspectIntegrationTexture(ctx,ring,graph,item.lease->texture.Get(),f==frames-2?out/(kind+"-subject.raw"):std::filesystem::path{});
            observations<<"{\"frame\":"<<f<<",\"kind\":\""<<kind<<"\",\"format\":"<<int(pixels.format)<<",\"hash\":\""<<std::hex<<pixels.hash<<std::dec<<"\",\"mean\":"<<pixels.mean<<",\"variance\":"<<pixels.variance<<",\"min\":"<<pixels.minimum<<",\"max\":"<<pixels.maximum<<",\"pts100ns\":"<<item.pts100ns<<"}\n";
        }
        integrationRequire(realSeen&&generatedSeen==(useFg&&!reset),"required actual frame kinds");
        if(f==frames-2)for(unsigned layer=0;layer<layers;++layer){
            const auto inputPixels=inspectIntegrationTexture(ctx,ring,graph,graph.diagnosticNrLayerInput(layer));
            const auto neural=inspectIntegrationTexture(ctx,ring,graph,graph.diagnosticNrLayerNeural(layer));
            const auto pixels=inspectIntegrationTexture(ctx,ring,graph,graph.diagnosticNrLayerOutput(layer),out/("layer"+std::to_string(layer+1)+"-subject.rgba16f"));
            integrationRequire(inputPixels.width==neural.width&&inputPixels.height==neural.height&&inputPixels.format==neural.format&&inputPixels.hash!=neural.hash,"actual NR inference must change same-size input");layerHashes.insert(pixels.hash);
            observations<<"{\"frame\":"<<f<<",\"kind\":\"nr-layer\",\"layer\":"<<layer+1<<",\"hash\":\""<<std::hex<<pixels.hash<<std::dec<<"\",\"inputHash\":\""<<std::hex<<inputPixels.hash<<std::dec<<"\",\"mean\":"<<pixels.mean<<",\"variance\":"<<pixels.variance<<",\"min\":"<<pixels.minimum<<",\"max\":"<<pixels.maximum<<"}\n";
        }
        if(adapter){DXGI_QUERY_VIDEO_MEMORY_INFO memory{};if(SUCCEEDED(adapter->QueryVideoMemoryInfo(0,DXGI_MEMORY_SEGMENT_GROUP_LOCAL,&memory)))peakVram=std::max<uint64_t>(peakVram,memory.CurrentUsage);}
        std::cout<<"INTEGRATION_FRAME frame="<<f<<" reset="<<reset<<" batch="<<result.batch.count<<" nrEvaluations="<<graph.metrics().nrEvaluateCount<<" generated="<<generatedCount<<std::endl;
    }
    const auto metrics=graph.metrics();integrationRequire(metrics.nrEvaluateCount==uint64_t(frames)*layers&&realCount==frames&&generatedCount==(useFg?frames-2:0)&&layerHashes.size()==layers,"integration final accounting and distinct layer outputs");
    integrationRequire(metrics.nvofFrameFailures==0,"guidance backend failures");
    observations.flush();integrationRequire(bool(observations),"integration observations write");
    av_frame_free(&frame);ring.drainQueue();graph.shutdown();
    std::cout<<"INTEGRATION_PASS layers="<<layers<<" hdr="<<hdr<<" fg="<<(fgName==L"fsr"?"fsr":fgName==L"rife"?"rife":"off")<<" source=3840x2160 nr=1920x1080 output=3840x2160 frames="<<frames<<" nrEvaluations="<<metrics.nrEvaluateCount<<" generated="<<generatedCount<<" flowEvaluations="<<metrics.amdOfExecuteCount<<" peakLocalVramMiB="<<peakVram/(1024.*1024)<<" seconds="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()<<std::endl;
    return 0;
}
}
