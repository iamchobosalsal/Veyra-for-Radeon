// Smooth spatial confidence to reduce frame-to-frame gate modulation.
// Neither the input image nor neural output is blurred by this confidence filter.
// Later passes retain broad relighting and supported fine/mid-scale changes.
// Layer 1 bypasses this shader entirely. Both real model passes still execute.
Texture2D<float4> inputImage : register(t0);
Texture2D<float4> neuralImage : register(t1);
RWTexture2D<float4> refinedImage : register(u0);

// Separable radius-32 Gaussian (sigma 12), in signed square-root RGB. Blurring
// only the neural CHANGE preserves the input, rather than blurring the image.
// The broad phase reuses a 16x80 tile in five row batches. Subsequent phases
// reuse the allocations for 38x38 luma, 34x34 high-pass and 18x18 confidence.
// 32,688 bytes shared memory; one dispatch, no history, resource or CPU wait.
groupshared float3 deltaTile[1444];
groupshared float3 horizontal[1280];
float3 perceptual(float3 v) { return sign(v)*sqrt(abs(v)); }
float3 toLinear(float3 v) { return sign(v)*v*v; }
float weight(int i) { return exp(-float(i*i)*(1.0/288.0)); }

[numthreads(16,16,1)]
void main(uint3 id:SV_DispatchThreadID,uint3 group:SV_GroupID,
          uint3 tid:SV_GroupThreadID,uint index:SV_GroupIndex) {
    uint w,h;inputImage.GetDimensions(w,h);
    float norm=0;
    [unroll] for(int k=-32;k<=32;++k)norm+=weight(k);
    [loop] for(uint batch=0;batch<5;++batch) {
        for(uint i=index;i<1280;i+=256) {
            int2 p=int2(group.xy)*16+int2(i%80,batch*16+i/80)-32;
            p=clamp(p,int2(0,0),int2(w,h)-1);
            deltaTile[i]=perceptual(neuralImage[p].rgb)-perceptual(inputImage[p].rgb);
        }
        GroupMemoryBarrierWithGroupSync();
        float3 sum=0;
        [unroll] for(uint x=0;x<65;++x)
            sum+=deltaTile[tid.y*80+tid.x+x]*weight(int(x)-32);
        horizontal[(batch*16+tid.y)*16+tid.x]=sum/norm;
        GroupMemoryBarrierWithGroupSync();
    }

    float3 change=0;
    [unroll] for(uint y=0;y<65;++y)
        change+=horizontal[(tid.y+y)*16+tid.x]*weight(int(y)-32);
    change/=norm;
    // All broad-filter readers finish before the scratch storage is reused.
    GroupMemoryBarrierWithGroupSync();

    // Global luma footprint: group origin + [-11,26] on each axis.
    // Confidence's one-pixel halo needs eight HP pixels at the wide scale,
    // and each HP value needs a further two-pixel luma halo.
    for(uint i=index;i<1444;i+=256) {
        int2 p=int2(group.xy)*16+int2(i%38,i/38)-11;
        p=clamp(p,int2(0,0),int2(w,h)-1);
        float3 luma=float3(0.2126,0.7152,0.0722);
        deltaTile[i]=float3(dot(perceptual(inputImage[p].rgb),luma),
                            dot(perceptual(neuralImage[p].rgb),luma),0);
    }
    GroupMemoryBarrierWithGroupSync();
    // Global high-pass footprint: group origin + [-9,24].
    for(uint i=index;i<1156;i+=256) {
        uint2 p=uint2(i%34,i/34)+2;
        float2 mean=0;
        [unroll] for(uint y=0;y<5;++y) [unroll] for(uint x=0;x<5;++x)
            mean+=deltaTile[(p.y+y-2)*38+p.x+x-2].xy/25.0;
        horizontal[i]=float3(deltaTile[p.y*38+p.x].xy-mean,0);
    }
    GroupMemoryBarrierWithGroupSync();
    // Global confidence footprint: group origin + [-1,16]. The center's
    // coordinate within the HP tile is [8,25]; wide offsets reach [0,33].
    // Every thread participates, including in partial groups at image edges.
    for(uint i=index;i<324;i+=256) {
        float confidence=1;
        int2 p=int2(i%18,i/18)+8;
        [unroll] for(uint scale=0;scale<2;++scale) {
            uint step=scale==0?1:4;
            float2 mean=0,squares=0;float products=0;
            [unroll] for(int y=-2;y<=2;++y) [unroll] for(int x=-2;x<=2;++x) {
                int2 q=p+int2(x,y)*int(step);
                float2 v=horizontal[q.y*34+q.x].xy;
                mean+=v/25.0;squares+=v*v/25.0;products+=v.x*v.y/25.0;
            }
            float2 variance=max(squares-mean*mean,0);
            float covariance=products-mean.x*mean.y;
            float correlation=clamp(covariance/sqrt(max(variance.x*variance.y,1e-20)),-1,1);
            float ratio=sqrt(variance.y/max(variance.x,1e-12));
            float supported=smoothstep(.80,.98,correlation)*smoothstep(.65,.95,ratio)
                           *(1-smoothstep(1.25,1.65,ratio))
                           *smoothstep(.00001,.00005,sqrt(variance.x));
            confidence=min(confidence,supported);
        }
        // The raw luma tile is no longer consumed; reuse its z component.
        deltaTile[i].z=confidence;
    }
    GroupMemoryBarrierWithGroupSync();
    if(id.x>=w||id.y>=h)return;
    float confidence=0;
    [unroll] for(uint y=0;y<3;++y) [unroll] for(uint x=0;x<3;++x) {
        const float wx=x==1?2.0:1.0;
        const float wy=y==1?2.0:1.0;
        confidence+=deltaTile[(tid.y+y)*18+tid.x+x].z*(wx*wy/16.0);
    }
    float4 source=inputImage[id.xy];
    float3 rawChange=perceptual(neuralImage[id.xy].rgb)-perceptual(source.rgb);
    change+=confidence*(rawChange-change);
    // Exact whole-image identity, including signed HDR and thin lines.
    if(all(change==0)){refinedImage[id.xy]=source;return;}
    refinedImage[id.xy]=float4(toLinear(perceptual(source.rgb)+change),source.a);
}
