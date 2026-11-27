// Vendor-independent SDR -> HDR expansion. Input is linear BT.709 SDR;
// output is linear BT.709 scRGB (1 = 80 nits). This is a deterministic tone
// expansion, not NVIDIA's learned TrueHDR model. No per-frame exposure history.
cbuffer Settings : register(b0) {
    uint width, height;
    float contrast, saturation, middleGray, peakNits;
    float2 unused;
};
Texture2D<float4> source : register(t0);
RWTexture2D<float4> destination : register(u0);
float3 shapeColor(float3 input, float exponent) {
    float3 rgb = saturate(input);
    float y = dot(rgb, float3(.2126,.7152,.0722));
    float shaped = .18 * pow(max(y / .18, 0.0), exponent);
    rgb *= y > 1e-7 ? shaped / y : 0;
    return max(lerp(shaped.xxx, rgb, saturation / 75.0), 0);
}
float channelMax(float3 rgb) { return max(rgb.r, max(rgb.g, rgb.b)); }
[numthreads(16,16,1)]
void main(uint3 p : SV_DispatchThreadID) {
    if(p.x >= width || p.y >= height) return;
    float4 original = source[p.xy];
    // The existing UI defaults (125/75) are neutral on this backend.
    // Keep 18% gray at the user-selected absolute luminance.
    float exponent = exp2((contrast - 125.0) / 200.0);
    float3 rgb = shapeColor(original.rgb, exponent);
    float top = max(rgb.r, max(rgb.g, rgb.b));
    float paperWhite = min(middleGray / .18, peakNits);
    float guide = top;
    // Stabilize only the highlight amplification, not the original RGB detail.
    // Small bright-pixel fluctuations used to independently drive the steep
    // cubic expansion. A bounded, edge-aware guide reduces that extra contrast
    // without a temporal history, a second pass, or dark-edge halos.
    [branch] if (top > .60 && paperWhite < peakNits) {
        float sum = 4.0 * top, weightSum = 4.0;
        float tolerance = .02 + .06 * top;
        [unroll] for (int y = -1; y <= 1; ++y)
            [unroll] for (int x = -1; x <= 1; ++x) {
                if (x == 0 && y == 0) continue;
                int2 q = clamp(int2(p.xy) + int2(x,y), int2(0,0), int2(width,height)-1);
                float neighbor = channelMax(shapeColor(source[q].rgb, exponent));
                float range = saturate(1.0 - abs(neighbor - top) / tolerance);
                float weight = (x == 0 || y == 0 ? 2.0 : 1.0) * range * range;
                sum += neighbor * weight;
                weightSum += weight;
            }
        guide = sum / weightSum;
    }
    // Continuous shoulder with zero derivative at its join; dark/mid tones
    // retain their luminance and RGB ratios. Keep the original peak limit even
    // when a center pixel differs from its guide; never clip chroma per channel.
    float highlight = pow(saturate((guide - .60) / .40), 3.0);
    float gain = paperWhite + (peakNits - paperWhite) * highlight / max(guide, 1e-7);
    gain = min(gain, peakNits / max(top, 1e-7));
    destination[p.xy] = float4(rgb * (gain / 80.0), original.a);
}
