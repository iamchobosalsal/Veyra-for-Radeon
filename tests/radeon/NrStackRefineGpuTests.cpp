// SPDX-License-Identifier: GPL-3.0-only
// Analytic contracts on the real shader; waits/readbacks are diagnostic only.
#include "FgProbeSupport.h"
int main(int argc,char** argv){try{
    require(argc==2||argc==3,"usage: test NrStackRefine.dxil [benchmark]");
    const bool benchmark=argc==3;
    Host host;StateTracker states;const unsigned w=benchmark?1920:177,h=benchmark?1080:113;
    auto input=makeTexture(host.device.Get(),w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,false);
    auto neural=makeTexture(host.device.Get(),w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,false);
    auto output=makeTexture(host.device.Get(),w,h,DXGI_FORMAT_R32G32B32A32_FLOAT,true);
    Transfer uploadInput(host,input.Get(),16,true),uploadNeural(host,neural.Get(),16,true),read(host,output.Get(),16,false);
    std::ifstream file(argv[1],std::ios::binary);std::vector<uint8_t> code((std::istreambuf_iterator<char>(file)),{});
    ComputePass pass;require(pass.create(host.device.Get(),code,3,2,1),"pass");
    makeSrv(host.device.Get(),input.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT,cpuHandleOf(pass,0));
    makeSrv(host.device.Get(),neural.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT,cpuHandleOf(pass,1));
    makeUav(host.device.Get(),output.Get(),DXGI_FORMAT_R32G32B32A32_FLOAT,cpuHandleOf(pass,2));
    auto image=[&](auto sample){
        std::vector<float> data(w*h*4);
        for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x){
            const auto rgb=sample(x,y);auto* p=data.data()+(y*w+x)*4;
            std::copy(rgb.begin(),rgb.end(),p);p[3]=.375f;
        }return data;
    };
    auto run=[&](const auto& a,const auto& b){
        uploadInput.write(a.data());uploadNeural.write(b.data());host.begin();
        states.transition(host.list.Get(),input.Get(),D3D12_RESOURCE_STATE_COPY_DEST);uploadInput.copy(host.list.Get(),input.Get(),true);
        states.transition(host.list.Get(),neural.Get(),D3D12_RESOURCE_STATE_COPY_DEST);uploadNeural.copy(host.list.Get(),neural.Get(),true);
        states.transition(host.list.Get(),input.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        states.transition(host.list.Get(),neural.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        states.transition(host.list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const float unused[8]={};pass.bind(host.list.Get(),unused,gpuHandleOf(pass,0).ptr,gpuHandleOf(pass,2).ptr);
        host.list->Dispatch((w+15)/16,(h+15)/16,1);states.uavBarrier(host.list.Get(),output.Get());
        states.transition(host.list.Get(),output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE);read.copy(host.list.Get(),output.Get(),false);host.complete();
        const auto bytes=read.read();std::vector<float> result(w*h*4);memcpy(result.data(),bytes.data(),bytes.size());
        for(size_t i=0;i<result.size();++i){require(std::isfinite(result[i]),"nonfinite output");if(i%4==3)require(result[i]==.375f,"alpha changed");}
        return result;
    };
    if(benchmark){
        auto a=image([](unsigned x,unsigned y){return std::array{.1f+.0001f*x,.2f+.0001f*y,.3f};});
        auto b=image([](unsigned x,unsigned y){return std::array{.3f+.0002f*x,.1f+.0001f*y,.2f};});
        run(a,b); // warm-up, uploads and readback are outside the timestamp interval
        ComPtr<ID3D12QueryHeap> heap;D3D12_QUERY_HEAP_DESC desc{};desc.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP;desc.Count=2;
        hr(host.device->CreateQueryHeap(&desc,IID_PPV_ARGS(&heap)),"benchmark query heap");
        UINT64 frequency=0;hr(host.queue->GetTimestampFrequency(&frequency),"timestamp frequency");
        for(unsigned batch=0;batch<5;++batch){
            host.begin();states.transition(host.list.Get(),output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const float unused[8]={};pass.bind(host.list.Get(),unused,gpuHandleOf(pass,0).ptr,gpuHandleOf(pass,2).ptr);
            host.list->EndQuery(heap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
            for(unsigned i=0;i<8;++i){host.list->Dispatch((w+15)/16,(h+15)/16,1);states.uavBarrier(host.list.Get(),output.Get());}
            host.list->EndQuery(heap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);
            host.list->ResolveQueryData(heap.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,read.resource.Get(),0);host.complete();
            UINT64* timestamps=nullptr;hr(read.resource->Map(0,nullptr,reinterpret_cast<void**>(&timestamps)),"timestamp map");
            std::cout<<"SHADER_GPU_MS "<<double(timestamps[1]-timestamps[0])*1000/frequency/8<<'\n';
            read.resource->Unmap(0,nullptr);
        }return 0;
    }
    const auto identity=image([](unsigned x,unsigned y){return std::array{(x==y||x==2*y)?3.f:.04f,-.15f,.8f};});
    require(run(identity,identity)==identity,"identity/thin-line/signed-HDR changed");
    std::cout<<"PASS exact identity, thin lines, HDR and partial thread groups\n";
    const auto flat=image([](unsigned,unsigned){return std::array{.16f,.16f,.16f};});
    const auto change=image([](unsigned,unsigned){return std::array{.36f,-.25f,4.f};});
    auto result=run(flat,change);
    for(size_t i=0;i<result.size();++i)require(std::abs(result[i]-change[i])<2e-5f,"lighting/color/HDR reconstruction removed");
    std::cout<<"PASS new model lighting/color retained (not first-layer bypass)\n";
    const auto noise=image([](unsigned x,unsigned y){float v=.6f+((x+y)%2?.1f:-.1f);return std::array{v*v,v*v,v*v};});
    const auto detail=image([](unsigned x,unsigned y){float v=.4f+((x+y)%2?.025f:-.025f);return std::array{v*v,v*v,v*v};});
    // Fourfold contrast is an adversarial reconstruction, unlike the modest
    // aligned enhancement below. Bound it instead of requiring all new detail
    // to be removed: retain >=80% of existing contrast and reject >=5/6 of
    // the excessive added amplitude. Unsupported speckle on a flat input must
    // lose >=90% of its amplitude. These are distinct quality contracts.
    for(bool textured:{false,true}){
        result=run(textured?detail:flat,noise);
        float minAmplitude=1, maxAmplitude=-1, mean=0; unsigned count=0;
        for(unsigned y=33;y<h-33;++y)for(unsigned x=33;x<w-33;++x){
            const float p=std::sqrt(std::max(0.f,result[(y*w+x)*4]));
            const float amplitude=(p-.6f)*((x+y)%2?1.f:-1.f);
            minAmplitude=std::min(minAmplitude,amplitude);
            maxAmplitude=std::max(maxAmplitude,amplitude);mean+=p;++count;
        }
        require(std::abs(mean/count-.6f)<.002f,"speckle constraint changed the broad tone shift");
        if(textured){
            require(minAmplitude>=.020f-1e-4f,"excessive reconstruction erased existing texture");
            require(maxAmplitude<=.0375f+1e-4f,"fourfold recursive contrast was not constrained");
        }else require(std::max(std::abs(minAmplitude),std::abs(maxAmplitude))<=.010f+1e-4f,"unsupported neural speckle survives on flat input");
        std::cout<<"PASS excessive contrast constrained textured="<<textured<<" amplitudeRange="<<minAmplitude<<'/'<<maxAmplitude<<'\n';
    }
    // An aligned 15-25% reconstruction gain is useful new structure, not noise.
    // Preserve >=70% of the requested ADDITIONAL amplitude, while bounding
    // overshoot and retaining the tone change. A blanket low-pass of the delta
    // fails this contract even though it perfectly preserves the first layer.
    // Several periods and orientations avoid fitting one checkerboard pattern.
    for(unsigned period:{8u,16u,32u})for(float gain:{1.15f,1.25f})for(bool diagonal:{false,true}){
        auto wave=[&](unsigned x,unsigned y){return std::sin((float(x)+(diagonal?float(y)*.75f:0.f))*6.28318530718f/period);};
        const auto before=image([&](unsigned x,unsigned y){float v=.4f+.025f*wave(x,y);return std::array{v*v,v*v,v*v};});
        const auto after=image([&](unsigned x,unsigned y){float v=.6f+.025f*gain*wave(x,y);return std::array{v*v,v*v,v*v};});
        const auto r=run(before,after);
        double sx=0,sy=0,sxx=0,sxy=0,syy=0,n=0;
        for(unsigned y=33;y<h-33;++y)for(unsigned x=33;x<w-33;++x){
            const double s=wave(x,y),v=std::sqrt(std::max(0.f,r[(y*w+x)*4]));
            sx+=s;sy+=v;sxx+=s*s;sxy+=s*v;syy+=v*v;n+=1;
        }
        const double variance=sxx-sx*sx/n;
        const double amplitude=(sxy-sx*sy/n)/variance;
        const double center=(sy-amplitude*sx)/n;
        const double ratio=amplitude/.025;
        const double retainedAddition=(ratio-1)/(gain-1);
        const double unexplained=std::sqrt(std::max(0.,(syy-sy*sy/n-amplitude*amplitude*variance)/n));
        require(retainedAddition>=.70,"coherent second-pass detail improvement was suppressed");
        require(retainedAddition<=1.15,"coherent detail gain overshot the requested improvement");
        require(std::abs(center-.6)<.002,"coherent reconstruction lost the broad tone shift");
        require(unexplained<.002,"coherent reconstruction acquired visible spatial modulation");
        std::cout<<"PASS useful new structure period="<<period<<" gain="<<gain<<" diagonal="<<diagonal<<" retainedAddition="<<retainedAddition<<" residual="<<unexplained<<'\n';
    }
    const auto smooth=image([](unsigned,unsigned){return std::array{.36f,.36f,.36f};});
    result=run(detail,smooth);
    float retained=0;
    for(unsigned y=33;y<h-33;++y)for(unsigned x=33;x<w-33;++x){
        const float d=std::sqrt(result[(y*w+x)*4])-.6f;
        require(d*((x+y)%2?1.f:-1.f)>.020f,"source texture erased or inverted by the next layer");
        retained+=std::abs(d);
    }
    std::cout<<"PASS existing fine structure survives a flat next-layer result amplitude="<<retained/((w-66)*(h-66))<<'\n';
    const auto inverted=image([](unsigned x,unsigned y){float v=.6f+((x+y)%2?-.1f:.1f);return std::array{v*v,v*v,v*v};});
    result=run(detail,inverted);
    for(unsigned y=33;y<h-33;++y)for(unsigned x=33;x<w-33;++x){
        const float d=std::sqrt(result[(y*w+x)*4])-.6f;
        require(d*((x+y)%2?1.f:-1.f)>.020f,"inverted neural detail flattened source structure");
    }
    std::cout<<"PASS contradictory neural texture cannot erase source structure\n";
    // The previous tiny filter passes checkerboard tests but erases broader
    // hair/cloth/face structure. Test independent sinusoidal landmarks at
    // 16 and 32 pixels, both flattened and contradicted by the next model.
    for(unsigned period:{16u,32u})for(bool opposite:{false,true}){
        const auto pattern=image([&](unsigned x,unsigned){float v=.4f+.025f*std::sin(x*6.28318530718f/period);return std::array{v*v,v*v,v*v};});
        const auto next=image([&](unsigned x,unsigned){float v=.6f-(opposite?.06f:0.f)*std::sin(x*6.28318530718f/period);return std::array{v*v,v*v,v*v};});
        const auto r=run(pattern,next);float amplitude=0,energy=0;
        for(unsigned x=33;x<w-33;++x){float s=std::sin(x*6.28318530718f/period);amplitude+=(std::sqrt(r[((h/2)*w+x)*4])-.6f)*s;energy+=.025f*s*s;}
        const float ratio=amplitude/energy;
        if(!opposite)require(ratio>.8f,"mid-scale structure erased by next layer");
        if(opposite)require(ratio>.6f,"contradictory neural structure displaced source landmarks");
        std::cout<<"PASS mid-scale structure period="<<period<<" opposite="<<opposite<<" amplitudeRatio="<<ratio<<'\n';
    }
    // Coarsening can remain positively correlated because broad lighting is
    // unchanged: the second result loses fine texture while quadrupling the
    // coarser pattern. Independent frequency projections must catch this;
    // whole-neighborhood luminance correlation alone is not sufficient.
    const auto mixed=image([&](unsigned x,unsigned){
        float v=.35f+.001f*x+.025f*std::sin(x*6.28318530718f/8)+.015f*std::sin(x*6.28318530718f/32);
        return std::array{v*v,v*v,v*v};
    });
    const auto coarse=image([&](unsigned x,unsigned){
        float v=.55f+.001f*x+.005f*std::sin(x*6.28318530718f/8)+.060f*std::sin(x*6.28318530718f/32);
        return std::array{v*v,v*v,v*v};
    });
    const auto coarsened=run(mixed,coarse);
    // Three complete 32-pixel periods: orthogonal frequency projections, with
    // the known ramp/tone removed. Keep >=70% of fine contrast and reject >=5/6
    // of the excessive coarse amplitude added by this adversarial result.
    double fineAmplitude=0,coarseAmplitude=0;unsigned frequencySamples=0;
    for(unsigned y=33;y<h-33;++y)for(unsigned x=40;x<136;++x){
        const double v=std::sqrt(std::max(0.f,coarsened[(y*w+x)*4]))-(.55+.001*x);
        fineAmplitude+=v*std::sin(x*6.28318530718/8);
        coarseAmplitude+=v*std::sin(x*6.28318530718/32);++frequencySamples;
    }
    fineAmplitude*=2./frequencySamples;coarseAmplitude*=2./frequencySamples;
    require(fineAmplitude>=.0175,"coarse neural reconstruction erased fine texture under correlated lighting");
    require(coarseAmplitude<=.0225+1e-4,"correlated lighting hid excessive coarse neural texture");
    std::cout<<"PASS correlated coarsening constrained fineAmplitude="<<fineAmplitude<<" coarseAmplitude="<<coarseAmplitude<<'\n';
    // A displaced narrow landmark is not a contrast improvement. The old line
    // must retain >=75% of its contrast and a new ghost at the displaced
    // position must stay below 25% of it, while accepting the broad tone shift.
    // This checks actual location, independently of the confidence formula.
    const unsigned oldX=w/2-4,newX=oldX+5;
    const auto line=image([&](unsigned x,unsigned){float v=.4f+(x>=oldX&&x<oldX+2?.10f:0.f);return std::array{v*v,v*v,v*v};});
    const auto movedLine=image([&](unsigned x,unsigned){float v=.6f+(x>=newX&&x<newX+2?.10f:0.f);return std::array{v*v,v*v,v*v};});
    const auto edgeResult=run(line,movedLine);
    double oldContrast=0,newContrast=0;unsigned edgeCount=0;
    for(unsigned y=33;y<h-33;++y){
        auto sample=[&](unsigned x){return std::sqrt(std::max(0.f,edgeResult[(y*w+x)*4]));};
        const float background=(sample(oldX-8)+sample(newX+9))*.5f;
        oldContrast+=(sample(oldX)+sample(oldX+1))*.5f-background;
        newContrast+=(sample(newX)+sample(newX+1))*.5f-background;++edgeCount;
    }
    oldContrast/=edgeCount;newContrast/=edgeCount;
    require(oldContrast>=.075,"second pass displaced an existing narrow landmark");
    require(std::abs(newContrast)<=.025,"second pass added a displaced ghost landmark");
    std::cout<<"PASS displaced landmark constrained oldContrast="<<oldContrast<<" ghostContrast="<<newContrast<<'\n';
    // A translated input pair must translate the result without history or
    // thread-group seams, including displacements not divisible by 16.
    auto shifted=image([&](unsigned x,unsigned y){auto p=identity.data()+(y*w+(x?x-1:0))*4;return std::array{p[0],p[1],p[2]};});
    auto varied=image([](unsigned x,unsigned y){return std::array{.2f+.001f*x,.3f+.002f*y,.4f};});
    auto variedShift=image([&](unsigned x,unsigned y){auto p=varied.data()+(y*w+(x?x-1:0))*4;return std::array{p[0],p[1],p[2]};});
    auto a=run(identity,varied),b=run(shifted,variedShift);
    float worst=0;
    for(unsigned y=33;y<h-33;++y)for(unsigned x=34;x<w-33;++x)for(unsigned c=0;c<3;++c)
        worst=std::max(worst,std::abs(a[(y*w+x-1)*4+c]-b[(y*w+x)*4+c]));
    require(worst<1e-6f,"translation/group seam mismatch");
    std::cout<<"PASS one-pixel translation error="<<worst<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<"FAILED "<<e.what()<<'\n';return 1;}}
