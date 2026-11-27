// Output stabiliser (anti-flicker), stage 5 of
// docs/NR_FG_OPTIMIZATION_PLAN_2026-09-29.md.
//
// Independent implementation of the behaviour publicly described by
// DLSS5-Feeder 1.17/1.18's "Output stabiliser": hold the previous output where
// the source has not visibly changed. No code was taken from that project; only
// the described behaviour. See THIRD_PARTY_NOTICES.md.
//
// Deliberate differences from that description, and why:
//   * No reprojection. The plan explicitly excludes it: a wrong motion vector
//     would drag the previous output into a region that did move, which is the
//     ghosting this pass exists to avoid. Regions that moved simply stop
//     holding.
//   * The change test compares the *source* (the image going into NR) against
//     the previous source, not against our own output. Comparing the output
//     with itself would feed the hold back into its own decision and let a held
//     region stay held forever, which is a smear.
Texture2D<float4> Current : register(t0);        // this frame's NR result
Texture2D<float4> PreviousOutput : register(t1); // what we emitted last frame
Texture2D<float4> Base : register(t2);           // this frame's NR input
Texture2D<float4> PreviousBase : register(t3);   // that input one frame ago
RWTexture2D<float4> Output : register(u0);
RWTexture2D<float4> NextBase : register(u1);
cbuffer Settings : register(b0) {
    uint2 Size;      // NR output extent (the dispatch grid)
    uint2 BaseSize;  // source extent; may differ under the realtime NR policy
    float HoldStrength;   // 0 is refused by the host, which skips the dispatch
    float Tolerance;      // relative source-color delta that counts as unchanged
    uint UseHistory;      // 0 on reset: publish current, seed history, hold nothing
};
// Perceptually weighted RGB distance in linear working color. Take absolute
// differences BEFORE reducing: moving detail can have identical box means,
// and different colors can have identical luminance.
float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }
int2 ClampBase(int2 p) { return clamp(p, int2(0, 0), int2(BaseSize) - int2(1, 1)); }
int2 ClampOutput(int2 p) { return clamp(p, int2(0, 0), int2(Size) - int2(1, 1)); }
float SourceChange(int2 p, float2 scale) {
    float distance = 0, energy = 0, centerDelta = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            // History was written at OUTPUT extent by NextBase below. Read
            // it there, and map only the current source to BaseSize. This also
            // matches the half-texel convention used to seed the anchor.
            const int2 q = ClampOutput(p + int2(x, y));
            const int2 source = ClampBase(int2((float2(q) + .5) * scale));
            const float3 a = Base.Load(int3(source, 0)).rgb;
            const float3 b = PreviousBase.Load(int3(q, 0)).rgb;
            if (!all(isfinite(a)) || !all(isfinite(b))) return 1e30;
            const float d = Luma(abs(a - b));
            const float e = max(max(Luma(abs(a)), Luma(abs(b))), 1e-4);
            distance += d;
            energy += e;
            // Do not dilute a changed center pixel among eight unchanged
            // neighbors; that would leave a trail behind a thin moving edge.
            if (x == 0 && y == 0) centerDelta = d / e;
        }
    return max(centerDelta, distance / energy);
}
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const int2 p = int2(id.xy);
    if (any(p >= int2(Size))) return;
    const float4 current = Current.Load(int3(p, 0));
    // Publish this frame's source unconditionally as the next anchor: the anchor
    // must describe the source as it is now, whatever we decide about the hold.
    const float2 scale = float2(BaseSize) / float2(Size);
    const int2 baseTexel = ClampBase(int2((float2(p) + .5) * scale));
    NextBase[p] = Base.Load(int3(baseTexel, 0));
    // A non-finite sample must not be held: publish it and let the next frame
    // decide, rather than freezing corruption in place.
    if (!all(isfinite(current))) { Output[p] = current; return; }
    if (UseHistory == 0) { Output[p] = current; return; }
    const float4 previous = PreviousOutput.Load(int3(p, 0));
    if (!all(isfinite(previous)) || previous.a < .999) { Output[p] = current; return; }
    const float delta = SourceChange(p, scale);
    // Moved (or visibly changed) beyond tolerance: publish the new value and
    // hold nothing. This is what keeps motion from smearing.
    if (delta > Tolerance) { Output[p] = current; return; }
    // Unchanged: blend toward the previous output.
    Output[p] = float4(lerp(current.rgb, previous.rgb, HoldStrength), current.a);
}
