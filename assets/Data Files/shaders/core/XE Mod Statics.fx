
// XE Mod Statics.fx
// MGE XE 0.16.0
// Distant statics rendering. Can be used as a core mod.


//------------------------------------------------------------
// Common functions

TransformedVert transformStaticVert(StatVertIn IN) {
    // Transforms with implicit depth bias
    TransformedVert v;

    v.worldpos = mul(IN.pos, world);
    v.viewpos = mul(v.worldpos, view);
    v.pos = mul(v.viewpos, proj);
    return v;
}

float4 lightStaticVert(StatVertIn IN) {
    // Decompress normal
    float4 normal = float4(normalize(2 * IN.normal.xyz - 1), 0);
    normal = mul(normal, world);

    // Lighting (worldspace)
    // Emissive is stored in the 4th value of the normal vector
    float emissive = IN.normal.w;
    float3 light = sunCol * saturate(dot(normal.xyz, -sunVec)) + sunAmb + emissive;

    return float4(IN.color.rgb * light, IN.color.a);
}

float2 texcoordsModifier(StatVertIn IN) {
    float2 tc = IN.texcoords;

    if (hasVCol) {
        // Linked to animateUV static flag
        // Render with fixed scrolling that approximates ghostfence
        tc.y += fmod(0.08 * time, 1);
    }
    return tc;
}

//------------------------------------------------------------
// DISTANT_STATIC_DISSOLVE - gradual fade of distant statics at the near handoff
//
// Distant statics are selected by a sphere cull at nearViewRange - 768
// (cullDistantStatics_kickoff). Without a fade they appear/disappear in a
// single frame as they cross that boundary, and inside the MW/MGE blend
// band they re-composite over Morrowind's real copy as a double-image.
// We fade the LOD out as it approaches the camera so it is nearly
// invisible right where the cull adds/removes it: fully present in the
// distance, dissolved away by staticFadeStart inside nearViewRange, via a
// screen-stable dither so there is no hard pop and no crawl when still.

// staticFadeStart must land keep=0 near the cull boundary (~768 inside
// nearViewRange) or statics still pop at the cull edge. 1000 puts keep=0
// at ~nearViewRange-1000, just inside it.
static const float staticFadeStart = 1000.0;   // distance inside nearViewRange where statics fully fade out (tunable)
static const float staticFadeWidth = 1024.0;   // width of the gradual fade band (tunable)

// Keep-fraction: 0 = dissolved (closer than the boundary), 1 = fully
// present. Exteriors only; the interior VS forces keep = 1.
float staticDissolveKeep(float dist) {
    return saturate((dist - (nearViewRange - staticFadeStart)) / staticFadeWidth);
}

// Interleaved gradient noise (Jimenez). Depends only on pixel position, so
// the dissolve stipple is stable frame-to-frame while the camera is still.
float staticDissolveDither(float2 vpos) {
    return frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715))));
}

//------------------------------------------------------------
// Statics rendering

StatVertOut StaticExteriorVS(StatVertIn IN) {
    StatVertOut OUT;
    TransformedVert v = transformStaticVert(IN);
    OUT.pos = v.pos;
    OUT.color = lightStaticVert(IN);

    // Fogging (exterior)
    float3 eyevec = v.worldpos.xyz - eyePos.xyz;
    float dist = length(eyevec);
    OUT.fog = fogColour(eyevec / dist, dist);

    // DISTANT_STATIC_DISSOLVE: gradual fade (carried in the otherwise-
    // unused vertex colour alpha)
    OUT.color.a = staticDissolveKeep(dist);

    OUT.texcoords_range = float3(texcoordsModifier(IN), dist);
    return OUT;
}

StatVertOut StaticInteriorVS (StatVertIn IN) {
    StatVertOut OUT;
    TransformedVert v = transformStaticVert(IN);
    OUT.pos = v.pos;
    OUT.color = lightStaticVert(IN);

    // Fogging (interior)
    float dist = length(v.viewpos.xyz);
    OUT.fog = fogMWColour(dist);

    // DISTANT_STATIC_DISSOLVE: interiors use the clip plane in
    // renderDistantStatics, so keep the LOD fully present here
    OUT.color.a = 1.0;

    OUT.texcoords_range = float3(texcoordsModifier(IN), dist);
    return OUT;
}

float4 StaticPS (StatVertOut IN, float2 vpos : VPOS): COLOR0 {
    // DISTANT_STATIC_DISSOLVE: distance fade (exteriors) - discard a
    // screen-stable fraction of pixels so the LOD fades instead of
    // hard-popping. The interior VS sets keep = 1, so dither (< 1) never
    // discards there.
    clip(IN.color.a - staticDissolveDither(vpos));

    float2 texcoords = IN.texcoords_range.xy;
    float range = IN.texcoords_range.z;

    float4 result = tex2D(sampBaseTex, texcoords);
    result.rgb *= IN.color.rgb;
    result.rgb = fogApply(result.rgb, IN.fog);

    // Alpha to coverage conversion
    result.a = calc_coverage(result.a, 133.0/255.0, 2.0);

    return result;
}

//------------------------------------------------------------
// Depth buffer output

DepthVertOut DepthStaticVS (StatVertIn IN) {
    DepthVertOut OUT;

    TransformedVert v = transformStaticVert(IN);
    OUT.pos = v.pos;

    OUT.depth = OUT.pos.w;
    OUT.alpha = 1;
    OUT.texcoords = texcoordsModifier(IN);

    return OUT;
}

float4 DepthStaticPS (DepthVertOut IN) : COLOR0 {
    clip(IN.depth - nearViewRange);

    if(hasAlpha) {
        float alpha = tex2D(sampBaseTex, IN.texcoords).a;
        clip(alpha - 133.0/255.0);
    }
    return IN.depth;
}
