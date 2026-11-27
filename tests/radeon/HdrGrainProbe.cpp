// SPDX-License-Identifier: GPL-3.0-only
// Actual D3D12 old/new shader comparison. Readback is confined to this probe.
#include "FgProbeSupport.h"

float decodeHalf(uint16_t h) {
    const unsigned exponent = (h >> 10) & 31;
    return (h & 0x8000 ? -1.f : 1.f) * (exponent
        ? std::ldexp(1.f + float(h & 1023) / 1024, int(exponent) - 15)
        : std::ldexp(float(h & 1023), -24));
}
int main(int argc, char** argv) { try {
    require(argc == 3, "usage: HdrGrainProbe old.dxil new.dxil");
    Host host; StateTracker states;
    constexpr unsigned width = 256, height = 64;
    auto input = makeTexture(host.device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, false);
    auto output = makeTexture(host.device.Get(), width, height, DXGI_FORMAT_R16G16B16A16_FLOAT, true);
    Transfer upload(host, input.Get(), 8, true), read(host, output.Get(), 8, false);
    ComputePass passes[2];
    for (unsigned i = 0; i < 2; ++i) {
        std::ifstream file(argv[i + 1], std::ios::binary);
        std::vector<uint8_t> code((std::istreambuf_iterator<char>(file)), {});
        require(passes[i].create(host.device.Get(), code, 2, 1, 1), "shader creation");
        auto cpu = passes[i].heap->GetCPUDescriptorHandleForHeapStart();
        makeSrv(host.device.Get(), input.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, cpu);
        cpu.ptr += passes[i].increment;
        makeUav(host.device.Get(), output.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, cpu);
    }
    auto run = [&](unsigned shader, const std::vector<uint16_t>& pixels, bool accepted,
                   float exposure = 0.f, float peakOverride = 0.f) {
        auto& pass = passes[shader]; upload.write(pixels.data()); host.begin();
        states.transition(host.list.Get(), input.Get(), D3D12_RESOURCE_STATE_COPY_DEST);
        upload.copy(host.list.Get(), input.Get(), true);
        states.transition(host.list.Get(), input.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        states.transition(host.list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        float constants[]{std::bit_cast<float>(width), std::bit_cast<float>(height),
            accepted ? 111.f : 125.f, accepted ? 91.f : 75.f,
            accepted ? 55.f : 44.f, peakOverride > 0 ? peakOverride : accepted ? 716.f : 1000.f, exposure, 0};
        auto gpu = pass.heap->GetGPUDescriptorHandleForHeapStart();
        pass.bind(host.list.Get(), constants, gpu.ptr, gpu.ptr + pass.increment);
        host.list->Dispatch((width+15)/16, (height+15)/16, 1);
        states.uavBarrier(host.list.Get(), output.Get());
        states.transition(host.list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE);
        read.copy(host.list.Get(), output.Get(), false); host.complete();
        auto bytes = read.read(); std::vector<float> values(pixels.size());
        for (size_t i = 0; i < values.size(); ++i) {
            uint16_t value; memcpy(&value, bytes.data()+i*2, 2); values[i] = decodeHalf(value);
            require(std::isfinite(values[i]) && values[i] >= 0, "invalid output");
            if (i%4 == 3) require(values[i] == .75f, "alpha changed");
            else require(values[i]*80 <= constants[5]+.5f, "peak limit exceeded");
        }
        return values;
    };
    auto pattern = [&](auto sample) {
        std::vector<uint16_t> pixels(width*height*4);
        for (unsigned y=0; y<height; ++y) for (unsigned x=0; x<width; ++x) {
            auto rgb=sample(x,y);
            for (unsigned c=0; c<3; ++c) pixels[(y*width+x)*4+c]=half(rgb[c]);
            pixels[(y*width+x)*4+3]=half(.75f);
        }
        return pixels;
    };
    auto compareUnchanged = [&](const auto& pixels, bool accepted, const char* label) {
        auto before=run(0,pixels,accepted), after=run(1,pixels,accepted);
        float worst=0;
        for(size_t i=0;i<before.size();++i) worst=std::max(worst,std::abs(before[i]-after[i]));
        require(worst <= .008f, label); // one FP16 ULP at the maximum tested luminance
        std::cout << "UNCHANGED " << label << " max_nits=" << worst*80 << '\n';
    };
    for (bool accepted : {false,true}) {
        for (float v : {0.f,.001f,.18f,.5f,.6f,.75f,.9f,1.f})
            compareUnchanged(pattern([&](unsigned,unsigned){return std::array{v,v,v};}),accepted,"uniform luminance");
        for (auto rgb : {std::array{1.f,.1f,.05f},std::array{.1f,.8f,.9f},std::array{.7f,.45f,.3f}})
            compareUnchanged(pattern([&](unsigned,unsigned){return rgb;}),accepted,"uniform color");
        compareUnchanged(pattern([](unsigned x,unsigned y) {
            float v = (x == 2*y || x == 2*y+1 || x > 190) ? .02f : .9f;
            return std::array{v,v,v};
        }),accepted,"thin diagonal and hard edge");
        compareUnchanged(pattern([](unsigned x,unsigned y) {
            float v=.35f+((x+y)%2 ? .005f : -.005f); return std::array{v,v,v};
        }),accepted,"midtone detail");
        for (float base : {.75f,.85f,.95f}) {
            std::array<double,2> noise{},mean{};
            for (unsigned shader=0;shader<2;++shader) {
                auto a=run(shader,pattern([&](unsigned x,unsigned y) {
                    float v=base+((x+y)%2 ? .005f : -.005f); return std::array{v,v,v};
                }),accepted);
                auto b=run(shader,pattern([&](unsigned x,unsigned y) {
                    float v=base+((x+y)%2 ? -.005f : .005f); return std::array{v,v,v};
                }),accepted);
                unsigned count=0;
                for(unsigned y=2;y<height-2;++y)for(unsigned x=2;x<width-2;++x) {
                    size_t i=(y*width+x)*4; noise[shader]+=std::abs(a[i]-b[i]);
                    mean[shader]+=(a[i]+b[i])*.5; ++count;
                }
                noise[shader]/=count;mean[shader]/=count;
            }
            require(noise[1] < noise[0]*.8,"highlight fluctuation reduction less than 20 percent");
            require(std::abs(mean[1]-mean[0]) < mean[0]*.01,"mean brightness changed by over one percent");
            std::cout<<"NOISE accepted="<<accepted<<" input="<<base<<" old_nits="<<noise[0]*80
                <<" new_nits="<<noise[1]*80<<" ratio="<<noise[1]/noise[0]<<'\n';
        }
    }
    std::cout<<"HDR_GRAIN_PASS\n";
    // Positive colour exposure produces valid linear values above SDR white.
    // Clamping channels before HDR makes these coloured patches white, and
    // maps distinct light levels to the same output even after lowering peak.
    auto brightColor=pattern([](unsigned,unsigned){return std::array{1.8f,1.35f,1.1f};});
    auto clipped=run(0,brightColor,true,1.15f,600.f);
    auto preserved=run(1,brightColor,true,1.15f,600.f);
    std::cout<<"HIGHLIGHT_COLOR before="<<clipped[0]<<','<<clipped[1]<<','<<clipped[2]
             <<" after="<<preserved[0]<<','<<preserved[1]<<','<<preserved[2]<<'\n';
    require(preserved[0]>preserved[1]*1.1f && preserved[1]>preserved[2]*1.05f,
            "over-white colour collapsed to white");
    float last=-1;
    for(float v:{1.05f,1.35f,1.7f,2.1f}) {
        auto patch=pattern([&](unsigned,unsigned){return std::array{v,v,v};});
        auto before=run(0,patch,true,1.15f,600.f), after=run(1,patch,true,1.15f,600.f);
        std::cout<<"HIGHLIGHT_LEVEL input="<<v<<" before="<<before[0]*80<<" after="<<after[0]*80<<'\n';
        require(after[0]*80>last+10,"exposed highlight levels clipped together");
        last=after[0]*80;
    }
    for(float v:{.03f,.18f,.35f,.5f}) {
        auto patch=pattern([&](unsigned,unsigned){return std::array{v,v,v};});
        auto before=run(0,patch,true,1.15f,600.f), after=run(1,patch,true,1.15f,600.f);
        require(std::abs(before[0]-after[0])*80 < .1f,"exposure highlight fix changed dark/mid tones");
    }
    for(float ev:{.05f,1.15f,3.f,5.f}) {
        auto ramp=pattern([&](unsigned x,unsigned){float v=std::exp2(ev)*float(x)/(width-1);return std::array{v,v,v};});
        auto after=run(1,ramp,true,ev,600.f);
        for(unsigned x=1;x<width;++x)require(after[4*x]>=after[4*(x-1)],"exposure shoulder is not monotone");
    }
    std::cout<<"HDR_HIGHLIGHT_PASS\n";return 0;
} catch(const std::exception& error) {std::cerr<<"HDR_GRAIN_FAIL "<<error.what()<<'\n';return 1;}}
