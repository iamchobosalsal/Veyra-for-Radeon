// SPDX-License-Identifier: GPL-3.0-only
// Bounded offline synthetic compatibility check, not a capture/display benchmark.
// All CPU waits/readbacks belong to this test process; no player/UI is opened.
#include "FgProbeSupport.h"
#include "veyra/gfx/FsrFgPresenter.h"
#include "veyra/gfx/FsrSrBackend.h"
#include "veyra/gfx/FsrRuntimeSelection.h"
#include "veyra/RuntimePaths.h"
#include <chrono>
#include <iomanip>
#include <limits>
#include <string_view>

namespace {
constexpr bool selectionContracts() {
    using veyra::gfx::selectFsrRuntime;
    const auto missing=selectFsrRuntime(false,false,false,false);
    const auto orphan=selectFsrRuntime(false,false,true,true);
    const auto srOnly=selectFsrRuntime(false,true,true,false);
    const auto fgOnly=selectFsrRuntime(false,true,false,true);
    const auto full=selectFsrRuntime(false,true,true,true);
    const auto open=selectFsrRuntime(true,false,false,false);
    const auto coexist=selectFsrRuntime(true,true,true,true);
    return !missing.upscale&&!missing.frameGeneration&&!orphan.upscale&&!orphan.frameGeneration
        &&srOnly.upscale&&!srOnly.frameGeneration&&!fgOnly.upscale&&fgOnly.frameGeneration
        &&full.upscale&&full.frameGeneration&&full.mlFrameGeneration
        &&open.openSource&&open.upscale&&open.frameGeneration&&!open.mlFrameGeneration
        &&coexist.openSource&&coexist.upscale&&coexist.frameGeneration&&!coexist.mlFrameGeneration;
}
static_assert(selectionContracts(),"open runtime precedence and optional component admission");
struct Case {
    const char* name;
    unsigned width, height, outputWidth, outputHeight;
    bool sr, fg;
    DXGI_FORMAT format;
};
float unhalf(uint16_t value) {
    const unsigned e=(value>>10)&31, m=value&1023;
    float result=e==0 ? std::ldexp(float(m),-24) : e==31
        ? (m?std::numeric_limits<float>::quiet_NaN():std::numeric_limits<float>::infinity())
        : std::ldexp(float(m+1024),int(e)-25);
    return value&0x8000 ? -result : result;
}
float pq(float nits) {
    constexpr float m1=2610.f/16384.f,m2=2523.f/32.f,c1=3424.f/4096.f,c2=2413.f/128.f,c3=2392.f/128.f;
    const float p=std::pow(nits/10000.f,m1);
    return std::pow((c1+c2*p)/(1+c3*p),m2);
}
unsigned bpp(DXGI_FORMAT f) { return f==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4; }
float light(DXGI_FORMAT f) { return f==DXGI_FORMAT_R16G16B16A16_FLOAT?6.25f:f==DXGI_FORMAT_R10G10B10A2_UNORM?pq(500.f):.9f; }
float dark(DXGI_FORMAT f) { return f==DXGI_FORMAT_R16G16B16A16_FLOAT?.25f:f==DXGI_FORMAT_R10G10B10A2_UNORM?pq(20.f):.08f; }
void storePixel(uint8_t* p,DXGI_FORMAT f,float v) {
    if(f==DXGI_FORMAT_R16G16B16A16_FLOAT) { const uint16_t a[]{half(v),half(v),half(v),half(1.f)};std::memcpy(p,a,8); }
    else if(f==DXGI_FORMAT_R10G10B10A2_UNORM) { const auto c=uint32_t(std::lround(std::clamp(v,0.f,1.f)*1023));const uint32_t a=c|(c<<10)|(c<<20)|0xc0000000u;std::memcpy(p,&a,4); }
    else { const auto c=uint8_t(std::lround(std::clamp(v,0.f,1.f)*255));p[0]=p[1]=p[2]=c;p[3]=255; }
}
float readPixel(const uint8_t* p,DXGI_FORMAT f) {
    if(f==DXGI_FORMAT_R16G16B16A16_FLOAT) { uint16_t v;std::memcpy(&v,p,2);return unhalf(v); }
    if(f==DXGI_FORMAT_R10G10B10A2_UNORM) { uint32_t v;std::memcpy(&v,p,4);return float(v&1023)/1023; }
    return p[0]/255.f;
}
struct Fixture {
    unsigned w,h,step,bar;DXGI_FORMAT format;
    ComPtr<ID3D12Resource> color,motion,depth;
    Transfer colors,motions,depths;
    std::vector<uint8_t> colorBytes;
    std::vector<std::array<uint16_t,2>> motionValues;
    std::vector<float> depthValues;
    Fixture(Host& host,unsigned width,unsigned height,DXGI_FORMAT f):w(width),h(height),step(std::max(4u,width/80)),bar(width/8),format(f),
        color(makeTexture(host.device.Get(),w,h,f,false)),motion(makeTexture(host.device.Get(),w,h,DXGI_FORMAT_R16G16_FLOAT,false)),depth(makeTexture(host.device.Get(),w,h,DXGI_FORMAT_R32_FLOAT,false)),
        colors(host,color.Get(),bpp(f),true),motions(host,motion.Get(),4,true),depths(host,depth.Get(),4,true),colorBytes(size_t(w)*h*bpp(f)),motionValues(size_t(w)*h),depthValues(size_t(w)*h,.9f) {}
    void upload(Host& host,StateTracker& states,unsigned frame) {
        const unsigned start=w/4+frame*step;
        for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
            const size_t i=size_t(y)*w+x;const bool bright=x>=start&&x<start+bar&&y>=h/3&&y<2*h/3;
            const float v=bright?light(format):dark(format)*(1.f+.15f*float((x/32+y/32)%2));
            storePixel(colorBytes.data()+i*bpp(format),format,v);
            motionValues[i]={half(bright&&frame? -float(step):0.f),0};
        }
        colors.write(colorBytes.data());motions.write(motionValues.data());depths.write(depthValues.data());
        for(auto item:{std::pair{color.Get(),&colors},std::pair{motion.Get(),&motions},std::pair{depth.Get(),&depths}}) {
            states.transition(host.list.Get(),item.first,D3D12_RESOURCE_STATE_COPY_DEST);item.second->copy(host.list.Get(),item.first,true);
        }
    }
};
struct Stats { double center=0,width=0,minimum=0,maximum=0;uint64_t nonfinite=0; };
Stats stats(const std::vector<uint8_t>& pixels,unsigned w,unsigned h,DXGI_FORMAT format) {
    Stats out;out.minimum=std::numeric_limits<double>::infinity();out.maximum=-out.minimum;double sum=0;
    const float threshold=(light(format)+dark(format))*0.5f;
    for(size_t i=0;i<size_t(w)*h;++i) {
        const float v=readPixel(pixels.data()+i*bpp(format),format);
        if(!std::isfinite(v)){++out.nonfinite;continue;}
        out.minimum=std::min(out.minimum,double(v));out.maximum=std::max(out.maximum,double(v));
        if(i/w==h/2&&v>threshold){out.width+=1;sum+=double(i%w);}
    }
    out.center=out.width?sum/out.width:-9999;return out;
}
void assertRange(const Stats& s,DXGI_FORMAT format) {
    require(s.nonfinite==0&&s.width>0,"finite nonblank output");
    require(s.minimum>=-.05&&s.maximum<=light(format)*1.4+.05,"reasonable output range");
    require(s.maximum>=light(format)*.7,"bright patch preserved (HDR must not clamp to SDR)");
}
void copyForRead(Host& host,StateTracker& states,ID3D12Resource* resource,Transfer& target) {
    states.transition(host.list.Get(),resource,D3D12_RESOURCE_STATE_COPY_SOURCE);target.copy(host.list.Get(),resource,false);
}
void checkCase(Host& host,veyra::gfx::FsrSrBackend& sr,veyra::gfx::FsrFgPresenter& fg,const Case& c) {
    constexpr unsigned frames=6,resetAt=3;
    const auto started=std::chrono::steady_clock::now();StateTracker states;
    Fixture input(host,c.width,c.height,c.format),display(host,c.outputWidth,c.outputHeight,c.format);
    auto scaled=makeTexture(host.device.Get(),c.outputWidth,c.outputHeight,c.format,true),generated=makeTexture(host.device.Get(),c.outputWidth,c.outputHeight,c.format,true);
    Transfer scaledRead(host,scaled.Get(),bpp(c.format),false),generatedRead(host,generated.Get(),bpp(c.format),false);
    const bool hdr=c.format!=DXGI_FORMAT_R8G8B8A8_UNORM;
    if(c.sr) {
        require(sr.initialize(host.device.Get(),c.width,c.height,c.outputWidth,c.outputHeight,hdr)&&sr.created(),"SR context");
        require(std::string_view(sr.providerVersion()).find("3.1.4")!=std::string_view::npos,"expected actual open SR3.1.4 provider");
    }
    if(c.fg) {
        require(fg.initializeIndependent(host.device.Get(),c.outputWidth,c.outputHeight,c.format,false)&&fg.available()&&fg.maxGeneratedFrames()==1,"FG context and 2X capability");
        require(std::string_view(fg.providerVersion()).find("1.1.3")!=std::string_view::npos,"expected actual open FG1.1.3 provider");
    }
    wchar_t loaded[32768]{};const auto loadedModule=GetModuleHandleW(L"amd_fidelityfx_dx12.dll");
    require(loadedModule&&GetModuleFileNameW(loadedModule,loaded,32768),"actual open module loaded");
    require(std::filesystem::equivalent(std::filesystem::path(loaded),veyra::runtime::localDataDirectory()/L"amd/fidelityfx/amd_fidelityfx_dx12.dll"),"actual DLL must be isolated staged runtime");
    std::wcout<<L"RUNTIME_LOADED "<<loaded<<std::endl;
    std::cout<<"CASE_BEGIN name="<<c.name<<" format="<<int(c.format)<<" input="<<c.width<<'x'<<c.height<<" output="<<c.outputWidth<<'x'<<c.outputHeight<<" srProvider="<<sr.providerVersion()<<" fgProvider="<<fg.providerVersion()<<std::endl;
    const auto srEvaluationsBefore=sr.evaluateCount(),srFailuresBefore=sr.failureCount();
    unsigned generatedChecks=0;double worstCenter=0,worstWidth=0;
    for(unsigned frame=0;frame<frames;++frame) {
        const bool reset=frame==0||frame==resetAt;host.begin();input.upload(host,states,frame);
        if(c.fg)display.upload(host,states,frame);
        ID3D12Resource* real=input.color.Get();
        if(c.sr) {
            for(auto* r:{input.color.Get(),input.motion.Get(),input.depth.Get()})states.transition(host.list.Get(),r,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            states.transition(host.list.Get(),scaled.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            require(sr.evaluate(host.list.Get(),input.color.Get(),input.motion.Get(),input.depth.Get(),scaled.Get(),c.width,c.height,c.outputWidth,c.outputHeight,reset,1000.f/30),"SR dispatch");
            states.uavBarrier(host.list.Get(),scaled.Get());real=scaled.Get();
        }
        if(c.fg) {
            for(auto* r:{real,display.motion.Get(),display.depth.Get(),generated.Get()})states.transition(host.list.Get(),r,D3D12_RESOURCE_STATE_COMMON);
            require(fg.generate(host.list.Get(),real,display.motion.Get(),display.depth.Get(),generated.Get(),reset,1000.f/30),"FG dispatch");
            copyForRead(host,states,generated.Get(),generatedRead);
        }
        if(c.sr)copyForRead(host,states,scaled.Get(),scaledRead);
        host.complete();
        const double ratio=double(c.outputWidth)/c.width;
        const double current=(c.width/4+frame*input.step+input.bar*.5)*ratio-.5;
        if(c.sr) {
            const auto s=stats(scaledRead.read(),c.outputWidth,c.outputHeight,c.format);
            std::cout<<"SR_FRAME case="<<c.name<<" frame="<<frame<<" reset="<<reset<<" center="<<s.center<<" expected="<<current<<" width="<<s.width<<" min="<<s.minimum<<" max="<<s.maximum<<std::endl;assertRange(s,c.format);
            require(std::abs(s.center-current)<=std::max(3.,input.step*ratio*.3),"SR bright patch position");
            require(std::abs(s.width-input.bar*ratio)<=std::max(6.,input.step*ratio*.5),"SR bright patch width");
        }
        if(c.fg) {
            const auto s=stats(generatedRead.read(),c.outputWidth,c.outputHeight,c.format);assertRange(s,c.format);
            const double expected=current-(reset?0:input.step*ratio*.5),ce=std::abs(s.center-expected),we=std::abs(s.width-input.bar*ratio);
            std::cout<<"FRAME case="<<c.name<<" frame="<<frame<<" reset="<<reset<<" center="<<s.center<<" expected="<<expected<<" centerError="<<ce<<" widthError="<<we<<" min="<<s.minimum<<" max="<<s.maximum<<std::endl;
            // Reset reports are retained as evidence without requiring byte identity.
            // For established history, reject a held endpoint or a wide double image.
            if(!reset) {
                require(ce<std::max(1.25,input.step*ratio*.4),"FG midpoint position (not held endpoint)");
                require(we<=std::max(3.,input.step*ratio*.6),"FG bright patch width (not broad double image)");
                ++generatedChecks;worstCenter=std::max(worstCenter,ce);worstWidth=std::max(worstWidth,we);
            }
        }
    }
    if(c.sr)require(sr.evaluateCount()-srEvaluationsBefore==frames&&sr.failureCount()==srFailuresBefore,"SR dispatch counters");
    if(c.fg)require(generatedChecks==4&&!fg.failed(),"FG generated checks/capability");
    fg.shutdown();sr.release();require(!fg.available()&&!sr.created(),"context shutdown");
    std::cout<<"CASE_PASS name="<<c.name<<" frames="<<frames<<" resets=2 generatedChecks="<<generatedChecks<<" worstCenter="<<worstCenter<<" worstWidth="<<worstWidth<<" wallSeconds="<<std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count()<<std::endl;
}
}
int main(int argc,char**argv) {try {
    require(argc==2,"usage: veyra_fsr_open_probe selection|small|hdr4k");
    const std::string_view mode=argv[1];if(mode=="selection"){require(selectionContracts(),"runtime selection contracts");std::cout<<"OPEN_FSR_SELECTION_PASS cases=7"<<std::endl;return 0;}require(mode=="small"||mode=="hdr4k","mode");
    const auto module=veyra::runtime::localDataDirectory()/L"amd/fidelityfx/amd_fidelityfx_dx12.dll";
    require(std::filesystem::is_regular_file(module),"stage open FidelityFX monolith beside private probe");
    std::wcout<<L"RUNTIME_EXPECTED "<<module.wstring()<<std::endl;
    Host host;veyra::gfx::FsrSrBackend sr;veyra::gfx::FsrFgPresenter fg;
    require(sr.compiledIn()&&fg.compiledIn(),"compiled backends");
    if(mode=="small") {
        checkCase(host,sr,fg,{"sr-sdr",640,360,1280,720,true,false,DXGI_FORMAT_R8G8B8A8_UNORM});
        checkCase(host,sr,fg,{"fg-sdr",1280,720,1280,720,false,true,DXGI_FORMAT_R8G8B8A8_UNORM});
        checkCase(host,sr,fg,{"sr-fg-resize-sdr",960,540,1920,1080,true,true,DXGI_FORMAT_R8G8B8A8_UNORM});
    } else {
        checkCase(host,sr,fg,{"sr-fg-scrgb-4k",1920,1080,3840,2160,true,true,DXGI_FORMAT_R16G16B16A16_FLOAT});
        checkCase(host,sr,fg,{"fg-pq-4k",3840,2160,3840,2160,false,true,DXGI_FORMAT_R10G10B10A2_UNORM});
    }
    std::cout<<"OPEN_FSR_PASS mode="<<mode<<" capture=none presentation=none latency=unmeasured"<<std::endl;return 0;
} catch(const std::exception&e) {std::cerr<<"OPEN_FSR_FAIL "<<e.what()<<std::endl;return 1;}}
