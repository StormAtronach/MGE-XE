#pragma once

#include "proxydx/d3d8header.h"

#include <unordered_map>
#include <vector>



struct RenderedState {
    IDirect3DTexture9* texture;
    IDirect3DVertexBuffer9* vb;
    UINT vbOffset, vbStride;
    IDirect3DIndexBuffer9* ib;
    DWORD ibBase;
    DWORD fvf;
    DWORD zWrite, cullMode;
    // numWeights: bone weights per vertex (2/3/4) when skinning, 0 otherwise.
    // Captured from D3DRS_VERTEXBLEND, translated from the D3DVBF enum at capture time.
    int numWeights;
    // usesGlobalPalette: 2b path flag. When true, the FFE shader uses the
    // global-indexed bonePaletteGlobal[] (selected by per-vertex blend indices)
    // instead of the positional bonePalette[]. Set by the 2b draw-replacement
    // handler before invoking ffeshader; defaults false for all 2a-style draws.
    bool usesGlobalPalette;
    // bonePaletteGlobal: pointer to the per-mesh bone-matrix palette uploaded
    // when usesGlobalPalette is set. Each entry is a 4x4 D3DXMATRIX. Owned by
    // the takeover caller (typically a stack buffer); lifetime must span the
    // renderMorrowind call. For 2c batched mode this is the MEGA-palette
    // (N instances × bonesPerInstance, concatenated).
    const D3DXMATRIX* bonePaletteGlobal;
    int bonePaletteGlobalCount;
    // usesBatchedPalette: 2c flag. When true, FFE selects the batched shader
    // variant and the takeover binds a stream-1 VB with per-instance offsets
    // + uses D3D9 hardware instancing. Requires usesGlobalPalette also true.
    bool usesBatchedPalette;
    // Hardware-instancing parameters (2c batched mode). Stream-1 holds
    // batchInstances DWORDs of per-instance bone-offset (instanceSlot*bonesPerInstance).
    IDirect3DVertexBuffer9* batchInstanceVB;
    IDirect3DVertexDeclaration9* batchVertexDecl;
    int batchInstances;
    int batchBonesPerInstance;
    D3DXMATRIX worldTransforms[4];
    D3DXMATRIX viewTransform;
    // worldViewTransforms slots 0..numWeights-1 hold bone matrices in view space when skinning.
    // When not skinning, slot 0 is the mesh worldview and slots 1..3 are uninitialized.
    D3DXMATRIX worldViewTransforms[4];
    D3DCOLORVALUE diffuseMaterial;
    BYTE blendEnable, srcBlend, destBlend;
    BYTE alphaTest, alphaFunc, alphaRef;
    BYTE useLighting, useFog, matSrcDiffuse, matSrcEmissive;

    D3DPRIMITIVETYPE primType;
    UINT baseIndex, minIndex, vertCount, startIndex, primCount;
};

struct FragmentState {
    struct Stage {
        BYTE colorOp, colorArg1, colorArg2;
        BYTE alphaOp, alphaArg1, alphaArg2;
        BYTE colorArg0, alphaArg0, resultArg;
        DWORD texcoordIndex;
        DWORD texTransformFlags;
        float bumpEnvMat[2][2];
        float bumpLumiScale, bumpLumiBias;
    } stage[8];

    struct Material {
        D3DCOLORVALUE diffuse, ambient, emissive;
    } material;
};

struct LightState {
    struct Light {
        D3DLIGHTTYPE type;
        D3DCOLORVALUE diffuse;
        D3DVECTOR position;     // position / normalized direction
        D3DVECTOR viewspacePos;
        union {
            D3DVECTOR falloff;  // constant, linear, quadratic
            D3DVECTOR ambient;  // for directional lights
        };
    };

    D3DCOLORVALUE globalAmbient;
    std::unordered_map<DWORD, Light> lights;
    std::unordered_map<DWORD, bool> lightsTransformed;
    std::vector<DWORD> active;
};

class FixedFunctionShader {
    struct ShaderKey {
        DWORD uvSets : 4;
        DWORD usesSkinning : 1;
        // When 1 (and usesSkinning == 1), shader uses the per-mesh indexed
        // palette (bonePaletteGlobal[] + IN.blendindices) via skinIndexed().
        // Set by the 2b handler when it has lifted the per-partition cap.
        // 2a's positional-blend path leaves this 0 (skin() against bonePalette[4]).
        DWORD usesGlobalPalette : 1;
        // 2c cross-NPC batched path. When 1 (and usesGlobalPalette == 1),
        // the shader uses skinIndexedBatched() — adds a per-instance
        // baseBoneOffset (D3D9 hardware-instancing stream-1 attribute) to
        // each bone-index lookup. The mega-palette holds N instances of
        // numBones matrices, each instance addressed by instanceSlot*numBones.
        DWORD usesBatchedPalette : 1;
        DWORD vertexColour : 1;
        DWORD heavyLighting : 1;
        // When 1, generate the USE_TEXTURE_LIGHTS variant — shader reads
        // point lights from a 1D dynamic texture (3 texels per light)
        // populated from MGE::SceneGraph::pointLights() with a runtime loop
        // count. Overrides the heavyLighting 4/8 choice.
        DWORD useTextureLightVariant : 1;
        DWORD vertexMaterial : 2;
        DWORD fogMode : 2;
        DWORD activeStages : 3;
        DWORD usesBumpmap : 1;
        DWORD bumpmapStage : 3;
        DWORD usesTexgen : 1;
        DWORD projectiveTexgen : 1;
        DWORD texgenStage : 3;

        struct Stage {
            DWORD colorOp : 6;
            DWORD colorArg1 : 6;
            DWORD colorArg2 : 6;
            DWORD colorArg0 : 6;
            DWORD alphaOpMatched : 1;
            DWORD alphaOpSelect1 : 1;
            DWORD texcoordIndex : 2;
            DWORD texcoordGen : 4;
        } stage[8];

        ShaderKey() {}
        ShaderKey(const RenderedState* rs, const FragmentState* frs, const LightState* lightrs);
        bool operator<(const ShaderKey& other) const;
        bool operator==(const ShaderKey& other) const;
        void log() const;

        struct hasher {
            std::size_t operator()(const ShaderKey& k) const;
        };
    };

    struct ShaderLRU {
        ID3DXEffect* effect;
        FixedFunctionShader::ShaderKey last_sk;
    };

    static IDirect3DDevice* device;
    static ID3DXEffectPool* constantPool;
    static std::unordered_map<ShaderKey, ID3DXEffect*, ShaderKey::hasher> cacheEffects;
    static ShaderLRU shaderLRU;
    static ID3DXEffect* effectDefaultPurple;

    // Dynamic 1D texture holding per-frame light data for the
    // USE_TEXTURE_LIGHTS shader path. Layout: 3 texels per light
    // (pos+ambient, diffuse, falloff+radius) at R32G32B32A32F. Width =
    // 3 * kMaxTexLights; height = 1. Updated once per SceneGraph
    // frameRevision via LockRect with DISCARD; bound to sampler slot 7
    // per draw when the variant is selected. See
    // evaluatePointLightsTextured in XE FixedFuncEmu.fx.
    //
    // Pool size = upper bound on per-frame snapshot lights we can store
    // on the GPU. 256 is a deliberately generous cap: dense modded
    // interiors top out around 150-200 lights. Memory: 256 * 3 texels
    // * 16 bytes/texel = 12 KB texture + ~20 KB per-call stack alloc
    // in renderMorrowind. Texture-upload, per-mesh selection, and
    // pixel-shader cost are all bounded by the actual snapshot size and
    // kMaxIndicesPerMesh — pool size doesn't enter the hot path.
    static const unsigned int kMaxTexLights = 256;
    static const unsigned int kTexelsPerLight = 3;
    // Per-mesh selected-light cap. 32 indices = 8 float4s in the
    // shader's `lightIndices` array; ps_3_0 source instruction budget
    // covers this comfortably and DXVK lifts the limit anyway. Visual
    // benefit kicks in only on dense interiors / Vivec cantons where a
    // single wall sits in range of >16 nearby lights — sparse exteriors
    // pay nothing extra (the runtime loop is bounded by the actual
    // selected count, not the cap).
    static const unsigned int kMaxIndicesPerMesh = 32;
    static IDirect3DTexture9* texLightData;

    // 2c V2 — texture-sampled bone palette for cross-NPC batching.
    // Width = kMaxBonesInPaletteTex * 4 (4 RGBA32F texels per mat4),
    // height = 1, format = D3DFMT_A32B32G32R32F, D3DUSAGE_DYNAMIC,
    // D3DPOOL_DEFAULT. Per-frame LockRect(DISCARD) upload of the mega-
    // palette built by skinneddraw.cpp's drain. Sampled in vs_3_0 via
    // tex2Dlod in skinIndexedBatched(). 4096-bone cap = 64KB texture
    // memory, plenty for 100+ batched bipeds.
    static const unsigned int kMaxBonesInPaletteTex = 4096;
    static IDirect3DTexture9* bonePaletteTex;
    // Cached SceneGraph::frameRevision() that's currently in the
    // texture. (uint64_t)-1 sentinel means "never uploaded."
    static uint64_t lastUploadedRevision;
    // Cached count of POINT lights actually packed into the texture
    // (≤ snapshot size; non-points are skipped). Used to bound the
    // per-mesh selection scan and to detect "no point lights this
    // frame" so we can skip the whole texture-light path.
    static unsigned int lastUploadedPointCount;

    // Per-mesh bbox cache (vtastek pattern). Object-space AABB computed
    // once per unique mesh by walking the vertex buffer; cache hit
    // transforms 8 corners by worldTransforms[0] to get world-space
    // bbox. MeshKey deduplicates by VB+IB+FVF+range so identical meshes
    // share a cache entry.
    struct MeshKey {
        IDirect3DVertexBuffer9* vb;
        IDirect3DIndexBuffer9*  ib;
        DWORD fvf;
        UINT  baseIndex, vertCount, startIndex, primCount;
        bool operator==(const MeshKey& o) const {
            return vb == o.vb && ib == o.ib && fvf == o.fvf
                && baseIndex == o.baseIndex && vertCount == o.vertCount
                && startIndex == o.startIndex && primCount == o.primCount;
        }
    };
    struct MeshKeyHash {
        std::size_t operator()(const MeshKey& k) const {
            std::size_t h = (std::size_t)k.vb;
            h = h * 31 + (std::size_t)k.ib;
            h = h * 31 + (std::size_t)k.fvf;
            h = h * 31 + (std::size_t)k.baseIndex;
            h = h * 31 + (std::size_t)k.vertCount;
            h = h * 31 + (std::size_t)k.startIndex;
            h = h * 31 + (std::size_t)k.primCount;
            return h;
        }
    };
    struct ObjectSpaceBBox {
        float minX, minY, minZ;
        float maxX, maxY, maxZ;
    };
    static std::unordered_map<MeshKey, ObjectSpaceBBox, MeshKeyHash> bboxCache;

    // Returns true and fills [outMin, outMax] with the world-space AABB
    // of the mesh referenced by `rs`. Returns false if the bbox can't
    // be derived (no VB, no position FVF, lock fails, unsupported prim).
    // Caches object-space bboxes by MeshKey so the VB walk runs once
    // per unique mesh per session.
    static bool computeBoundingBox(const RenderedState* rs,
        D3DXVECTOR3& outMin, D3DXVECTOR3& outMax);

    static D3DXHANDLE ehWorld, ehWorldView;
    static D3DXHANDLE ehNumWeights, ehBonePalette;
    // 2b global-indexed palette uniform (up to 32 bone matrices).
    static D3DXHANDLE ehBonePaletteGlobal;
    static D3DXHANDLE ehTex0, ehTex1, ehTex2, ehTex3, ehTex4, ehTex5;
    static D3DXHANDLE ehMaterialDiffuse, ehMaterialAmbient, ehMaterialEmissive;
    static D3DXHANDLE ehLightSceneAmbient, ehLightSunDiffuse, ehLightSunDirection;
    static D3DXHANDLE ehLightDiffuse, ehLightAmbient, ehLightPosition;
    static D3DXHANDLE ehLightFalloffQuadratic, ehLightFalloffLinear, ehLightFalloffConstant;
    // Texture-light path handles
    static D3DXHANDLE ehTexLightData, ehLightDataParams, ehLightIndices, ehTexLightView;
    // 2c V2 bone-palette texture handles.
    static D3DXHANDLE ehBonePaletteTex, ehBonePaletteRcpWidth;
    static D3DXHANDLE ehTexgenTransform, ehBumpMatrix, ehBumpLumiScaleBias;

    static float sunMultiplier, ambMultiplier;

    static ID3DXEffect* generateMWShader(const ShaderKey& sk);

public:
    static bool init(IDirect3DDevice* d, ID3DXEffectPool* pool);
    static void precacheAsync();
    static void updateLighting(float sunMult, float ambMult);
    static void renderMorrowind(const RenderedState* rs, const FragmentState* frs, LightState* lightrs);
    static void release();

    // 2c V2 cross-NPC batching surface — needed by skinneddraw.cpp's drain.
    // Hoisted to public so the drain can LockRect/UnlockRect the bone-
    // palette texture directly without round-tripping through an accessor.
    static IDirect3DTexture9* getBonePaletteTex() { return bonePaletteTex; }
    static unsigned int       getMaxBonesInPaletteTex() { return kMaxBonesInPaletteTex; }
};
