
// XE Common.fx
// MGE XE 0.16.0
// Shared structures and functions



//------------------------------------------------------------
// Uniform variables

shared float2 rcpRes;
shared float shadowRcpRes;
shared matrix world, view, proj;
// vertexBlendPalette: scratchpad slot for non-skinning matrices (renderShadowDebug's shadowToCameraProj[2], XE Main.fx shadow lookup).
// Distinct from the skinning bone palette below.
shared matrix vertexBlendPalette[4];
shared matrix shadowViewProj[2];
// bonePalette: per-draw bone matrices in view space (positional weights, slots 0..numWeights-1).
//   Used by 2a-style draws: MW captures up to 4 matrices per partition, MGE replays positionally.
// bonePaletteGlobal: per-MESH bone matrices keyed by GLOBAL bone index (2b path).
//   Used when the caller has lifted the per-partition cap and supplies the entire skinInstance's
//   bone palette. Per-vertex blend indices select into this larger table. 32 mat4 = 128 const regs.
// meshWorldview: per-draw worldview matrix for non-skinned meshes (replaces the historical reuse of vertexBlendPalette[0]).
shared matrix bonePalette[4];
shared matrix bonePaletteGlobal[32];
shared matrix meshWorldview;
shared bool hasAlpha, hasBones, hasVCol;
shared float alphaRef, materialAlpha;
// numWeights: number of bone weights per vertex (2/3/4). 0 when not skinning. Replaces the D3DRS_VERTEXBLEND enum reuse.
shared int numWeights;

shared float3 eyePos, footPos;
shared float3 sunVec, sunVecView, sunCol, sunAmb;
shared float3 skyCol, fogColNear, fogColFar;
shared float4 skyScatterColFar;
shared float fogStart, fogRange;
shared float nearFogStart, nearFogRange;
shared float nearViewRange;
shared float3 sunPos;
shared float sunVis;
shared float2 windVec;
shared float niceWeather;
shared float time;


//------------------------------------------------------------
// Textures

shared texture tex0, tex1, tex2, tex3;

sampler sampBaseTex = sampler_state { texture = <tex0>; minfilter = anisotropic; magfilter = linear; mipfilter = linear; addressu = wrap; addressv = wrap; };
sampler sampNormals = sampler_state { texture = <tex1>; minfilter = anisotropic; magfilter = linear; mipfilter = linear; addressu = wrap; addressv = wrap; };
sampler sampDetail = sampler_state { texture = <tex2>; minfilter = anisotropic; magfilter = linear; mipfilter = linear; addressu = wrap; addressv = wrap; };
sampler sampWater3d = sampler_state { texture = <tex1>; minfilter = linear; magfilter = linear; mipfilter = none; addressu = wrap; addressv = wrap; addressw = wrap; };
sampler sampDepth = sampler_state { texture = <tex3>; minfilter = linear; magfilter = linear; mipfilter = none; addressu = clamp; addressv = clamp; };
sampler sampDepthPoint = sampler_state { texture = <tex3>; minfilter = point; magfilter = point; mipfilter = none; addressu = clamp; addressv = clamp; };


//------------------------------------------------------------
// Distant land statics / grass
// diffuse in color, emissive in normal.w

struct StatVertIn {
    float4 pos : POSITION;
    float4 normal : NORMAL;
    float4 color : COLOR0;
    float2 texcoords : TEXCOORD0;
};

struct StatVertInstIn {
    float4 pos : POSITION;
    float4 normal : NORMAL;
    float4 color : COLOR0;
    float2 texcoords : TEXCOORD0;
    float4 world0 : TEXCOORD1;
    float4 world1 : TEXCOORD2;
    float4 world2 : TEXCOORD3;
};

struct StatVertOut {
    float4 pos : POSITION;
    centroid float4 color : COLOR0;
    centroid float4 fog : TEXCOORD0;
    float3 texcoords_range : TEXCOORD1;
};

//------------------------------------------------------------
// Depth buffer for deferred rendering

struct DepthVertOut {
	float4 pos : POSITION;
    float alpha : COLOR0;
	half2 texcoords : TEXCOORD0;
    float depth : TEXCOORD1;
};

//------------------------------------------------------------
// Full-screen deferred pass, used for reconstructing position from depth

struct DeferredOut {
    float4 pos : POSITION;
    float4 tex : TEXCOORD0;
    float3 eye : TEXCOORD1;
};

//------------------------------------------------------------
// Morrowind FVF

struct MorrowindVertIn {
    float4 pos : POSITION;
    float4 normal : NORMAL;
    float4 blendweights : BLENDWEIGHT;
    float4 color : COLOR0;
    float2 texcoords : TEXCOORD0;
};

struct TransformedVert {
    float4 pos;
    float4 worldpos;
    float4 viewpos;
    float4 normal;
};

//------------------------------------------------------------
// Suppress warning X3205: conversion from larger type to smaller, possible loss of data
#pragma warning( disable : 3205 3571 )
// Suppress warning X3571: pow(f, e) will not work for negative f
#pragma warning( disable : 3571 )


//------------------------------------------------------------
// Fogging functions, horizon to sky colour approximation

#ifdef USE_EXPFOG

float fogScalar(float dist) {
    float x = (dist - fogStart) / fogRange;
    return saturate(exp(-x));
}

float fogMWScalar(float dist) {
    return saturate((nearFogRange - dist) / (nearFogRange - nearFogStart));
}

#else

float fogScalar(float dist) {
    return saturate((nearFogRange - dist) / (nearFogRange - nearFogStart));
}

float fogMWScalar(float dist) {
    return fogScalar(dist);
}

#endif

#ifdef USE_SCATTERING
    static const float3 newSkyCol = lerp(skyCol, skyScatterColFar.rgb, skyScatterColFar.a);
    static const float sunaltitude = pow(1 + sunPos.z, 10);
    static const float sunaltitude_a = 2.8 + 4.3 / sunaltitude;
    static const float sunaltitude_b = saturate(1 - exp2(-1.9 * sunaltitude));
    static const float sunaltitude_c = saturate(exp(-4 * sunPos.z)) * saturate(sunaltitude);

    float3 outscatter, inscatter;

    float4 fogColourScatter(float3 dir, float fogdist, float fog, float3 skyColDirectional) {
        skyColDirectional *= 1 - fog;

        if(niceWeather > 0.001 && eyePos.z > /*WaterLevel*/-1) {
            float suncos = dot(dir, sunPos);
            float mie = (1.58 / (1.24 - suncos)) * sunaltitude_c;
            float rayl = 1 - 0.09 * mie;

            float atmdep = 1.33 * exp(-2 * saturate(dir.z));
            float3 sunscatter = lerp(inscatter, outscatter, 0.5 * (1 + suncos));
            float3 att = atmdep * sunscatter * (sunaltitude_a + 0.7*mie);
            att = (1 - exp(-fogdist * att)) / att;

            float3 color = 0.125 * mie + newSkyCol * rayl;
            color *= att * (1.17*atmdep + 0.89) * sunaltitude_b;
            color = lerp(skyColDirectional, color, niceWeather);

            return float4(color, fog);
        }
        else {
            return float4(skyColDirectional, fog);
        }
    }

    float4 fogColour(float3 dir, float dist) {
        float fogdist = (dist - fogStart) / fogRange;
        float fog = (dist > nearViewRange) ? saturate(exp(-fogdist)) : fogMWScalar(dist);
        fogdist = saturate(0.224 * fogdist);

        return fogColourScatter(dir, fogdist, fog, fogColFar);
    }

    float4 fogColourWater(float3 dir, float dist) {
        float fogdist = (dist - fogStart) / fogRange;
        float fog = saturate(exp(-fogdist));
        fogdist = saturate(0.224 * fogdist);

        return fogColourScatter(dir, fogdist, fog, fogColFar);
    }

    float4 fogColourSky(float3 dir) {
        float3 skyColDirectional = lerp(fogColFar, skyCol, 1 - pow(saturate(1 - 2.22 * saturate(dir.z - 0.075)), 1.15));
        return fogColourScatter(dir, 1, 0, skyColDirectional);
    }
#else
    float4 fogColour(float3 dir, float dist) {
        float f = fogScalar(dist);
        return float4((1 - f) * fogColFar, f);
    }

    float4 fogColourWater(float3 dir, float dist) {
        return fogColour(dir, dist);
    }

    float4 fogColourSky(float3 dir) {
        float3 skyColDirectional = lerp(fogColFar, skyCol, 1 - pow(saturate(1 - 2.22 * saturate(dir.z - 0.075)), 1.15));
        return float4(skyColDirectional, 0);
    }
#endif

float4 fogMWColour(float dist) {
    float f = fogMWScalar(dist);
    return float4((1 - f) * fogColNear, f);
}

float3 fogApply(float3 c, float4 f) {
    return f.a * c + f.rgb;
}

//------------------------------------------------------------
// Is point above water function, for exteriors only

bool isAboveSeaLevel(float3 pos) {
    return (pos.z > -1);
}

//------------------------------------------------------------
// Instancing matrix decompression and multiply

float4 instancedMul(float4 pos, float4 m0, float4 m1, float4 m2) {
    float4 v;
    v.x = dot(pos, m0);
    v.y = dot(pos, m1);
    v.z = dot(pos, m2);
    v.w = pos.w;

    return v;
}

//------------------------------------------------------------
// Skinning, fixed-function
// Uses worldview matrices for numerical accuracy

float4 skin(float4 pos, float4 blend) {
    // Derive the final weight (1 - sum of stored weights) for the highest-index bone,
    // matching the D3D9 FFP convention. numWeights is the per-vertex bone count (2/3/4).
    if(numWeights == 2)
        blend[1] = 1 - blend[0];
    else if(numWeights == 3)
        blend[2] = 1 - (blend[0] + blend[1]);
    else if(numWeights == 4)
        blend[3] = 1 - (blend[0] + blend[1] + blend[2]);

    float4 viewpos = mul(pos, bonePalette[0]) * blend[0];

    if(numWeights >= 2)
        viewpos += mul(pos, bonePalette[1]) * blend[1];
    if(numWeights >= 3)
        viewpos += mul(pos, bonePalette[2]) * blend[2];
    if(numWeights >= 4)
        viewpos += mul(pos, bonePalette[3]) * blend[3];

    return viewpos;
}

//------------------------------------------------------------
// Skinning, per-mesh palette (2b path)
//
// Indexed blending against bonePaletteGlobal[]. Each vertex carries its
// own per-bone indices via IN.blendindices (D3DDECLUSAGE_BLENDINDICES,
// typically D3DCOLOR-packed 4×UBYTE4 → reinterpreted as float4).
//
// The caller (MGE skinneddraw handler) populates bonePaletteGlobal with the
// full skinInstance bone palette (up to 32 entries) and rewrites each
// partition's blend-index attribute through partition->bones[] so the
// per-vertex indices refer into the global table rather than the per-
// partition local one.
//
// numWeights still applies (2/3/4 weights per vertex). The final weight
// is derived as (1 - sum of stored), matching the D3D9 FFP convention.

float4 skinIndexed(float4 pos, float4 weights, float4 indices) {
    if(numWeights == 2)
        weights[1] = 1 - weights[0];
    else if(numWeights == 3)
        weights[2] = 1 - (weights[0] + weights[1]);
    else if(numWeights == 4)
        weights[3] = 1 - (weights[0] + weights[1] + weights[2]);

    float4 viewpos = mul(pos, bonePaletteGlobal[indices[0]]) * weights[0];

    if(numWeights >= 2)
        viewpos += mul(pos, bonePaletteGlobal[indices[1]]) * weights[1];
    if(numWeights >= 3)
        viewpos += mul(pos, bonePaletteGlobal[indices[2]]) * weights[2];
    if(numWeights >= 4)
        viewpos += mul(pos, bonePaletteGlobal[indices[3]]) * weights[3];

    return viewpos;
}

//------------------------------------------------------------
// Skinning, per-mesh palette + per-instance offset (cross-NPC batching)
//
// 2c V2 — VTF (vertex texture fetch) replacement of the const-register
// batched path. The mega-palette is uploaded each frame as a 1D dynamic
// RGBA32F texture (4 texels per mat4); sampled in vs_3_0 via tex2Dlod.
// Sidesteps SM3's 256 const-register budget — batch size is now bounded
// only by texture width.
//
// Stream 1 carries one FLOAT per instance = `instanceSlot * numBones`,
// supplied via D3D9 hardware instancing. Shader adds it to each per-vertex
// bone index before reconstructing the matrix from the texture.
//
// Texture layout: width = bonePaletteTexWidth (= 4 * maxBones), height = 1.
// Each mat4 occupies 4 consecutive texels (rows 0..3 of the matrix).
// `bonePaletteRcpWidth` = 1.0 / width, set per-frame from C++.
//
// Sampler declared with POINT filter + CLAMP addressing so adjacent matrix
// rows don't bleed and out-of-range indices clamp instead of wrapping.

shared texture bonePaletteTex;
shared float bonePaletteRcpWidth;
sampler BonePaletteSampler = sampler_state {
    texture   = <bonePaletteTex>;
    MinFilter = POINT;
    MagFilter = POINT;
    MipFilter = NONE;
    AddressU  = CLAMP;
    AddressV  = CLAMP;
};

float4x4 sampleBoneMatrix(float boneIdx) {
    // Each matrix = 4 consecutive texels at u = (boneIdx*4 + row + 0.5) * rcpWidth.
    // Strength-reduce: compute u0 once, derive u1/u2/u3 by adding rcpWidth.
    float u0 = (boneIdx * 4.0 + 0.5) * bonePaletteRcpWidth;
    float u1 = u0 + bonePaletteRcpWidth;
    float u2 = u0 + 2.0 * bonePaletteRcpWidth;
    float u3 = u0 + 3.0 * bonePaletteRcpWidth;
    float4 r0 = tex2Dlod(BonePaletteSampler, float4(u0, 0.5, 0, 0));
    float4 r1 = tex2Dlod(BonePaletteSampler, float4(u1, 0.5, 0, 0));
    float4 r2 = tex2Dlod(BonePaletteSampler, float4(u2, 0.5, 0, 0));
    float4 r3 = tex2Dlod(BonePaletteSampler, float4(u3, 0.5, 0, 0));
    return float4x4(r0, r1, r2, r3);
}

float4 skinIndexedBatched(float4 pos, float4 weights, float4 indices, float baseBoneOffset) {
    if(numWeights == 2)
        weights[1] = 1 - weights[0];
    else if(numWeights == 3)
        weights[2] = 1 - (weights[0] + weights[1]);
    else if(numWeights == 4)
        weights[3] = 1 - (weights[0] + weights[1] + weights[2]);

    float4 viewpos = mul(pos, sampleBoneMatrix(indices[0] + baseBoneOffset)) * weights[0];

    if(numWeights >= 2)
        viewpos += mul(pos, sampleBoneMatrix(indices[1] + baseBoneOffset)) * weights[1];
    if(numWeights >= 3)
        viewpos += mul(pos, sampleBoneMatrix(indices[2] + baseBoneOffset)) * weights[2];
    if(numWeights >= 4)
        viewpos += mul(pos, sampleBoneMatrix(indices[3] + baseBoneOffset)) * weights[3];

    return viewpos;
}

//------------------------------------------------------------
// Vertex material to fragment colour routing

float4 vertexMaterial(float4 vertexColour) {
    if (hasVCol) {
        return vertexColour;
    }
    else {
        return float4(1, 1, 1, materialAlpha);
    }
}

//------------------------------------------------------------
// Alpha-to-coverage

// Calculates a coverage value from alpha, such that
// coverage = 1 at the alpha test level (alpha_ref) and falls off quickly (falloff_rate)
float calc_coverage(float a, float alpha_ref, float falloff_rate) {
    return saturate(falloff_rate * (a - alpha_ref) + alpha_ref);
}

//------------------------------------------------------------
