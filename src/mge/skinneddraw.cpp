// MGE-side SharedSE consumer prelude — sets SE_IS_MGE / MWSE_NO_CUSTOM_ALLOC
// and pulls the headers SharedSE expects to find pre-included. Same pattern
// as scenegraph.cpp.
#include "mge_se_prelude.h"

#include "skinneddraw.h"

#include "NIAVObject.h"
#include "NISkinInstance.h"
#include "NITransform.h"

#include "MemoryUtil.h"
#include "support/log.h"

#include "configuration.h"
#include "distantland.h"
#include "ffeshader.h"
#include "mged3d8device.h"

#include "NIMatrix33.h"

#include <atomic>
#include <unordered_map>

namespace MGE::SkinnedDraw {

    // Option-D perf instrumentation — total drawcalls per frame across the
    // entire D3D9 proxy (not just skinned). mged3d8device.cpp's
    // DrawIndexedPrimitive increments; per-frame log in onSceneEnd reads
    // and resets.
    std::atomic<int> g_rawDrawcalls{0};

    int buildBonePalette(
        const NI::SkinInstance* skinInstance,
        const NI::Transform& meshTransform,
        NI::Transform* outPalette)
    {
        if (!skinInstance || !outPalette) {
            return 0;
        }

        const NI::SkinData* const skinData = skinInstance->skinData;
        NI::AVObject* const rootParent = skinInstance->rootParent;
        NI::AVObject** const bones = skinInstance->bones;

        if (!skinData || !rootParent || !bones) {
            return 0;
        }

        const unsigned int numBones = skinData->numBones;
        if (numBones == 0 || numBones > kMaxPaletteSize) {
            return 0;
        }

        // inverseRoot = invert(rootParent->worldTransform)
        // Returns false only when scale == 0 (degenerate transform).
        NI::Transform inverseRoot;
        if (!rootParent->worldTransform.invert(&inverseRoot)) {
            return 0;
        }

        // skinToRoot collapses the three frame-of-reference hops the engine
        // does once per partition (here once per skinInstance).
        const NI::Transform skinToRoot =
            meshTransform * skinData->transform * inverseRoot;

        const NI::SkinData::BoneData* const boneData = skinData->boneData;
        for (unsigned int b = 0; b < numBones; ++b) {
            const NI::AVObject* const bone = bones[b];
            if (!bone) {
                return 0;
            }
            outPalette[b] =
                skinToRoot * bone->worldTransform * boneData[b].transform;
        }

        return static_cast<int>(numBones);
    }

    // ------------------------------------------------------------------
    // Phase 2 smoke-test observer hook
    // ------------------------------------------------------------------

    namespace {
        // Vanilla NiDX8Renderer::DrawSkinnedPrimitive2 at 0x6AEF90.
        // __thiscall: this in ECX, rest on stack, callee cleans.
        using DrawSkinnedPrimitive2_t = void(__thiscall*)(
            void* this_,
            int primitiveType,
            void* geomData,
            NI::SkinInstance* skinInstance,
            const NI::Transform* transform,
            void* worldBound,
            int unused1,
            int unused2);

        // reinterpret_cast from integer to function pointer is not constexpr
        // in MSVC. Use static const at namespace scope.
        const DrawSkinnedPrimitive2_t kVanillaDrawSkinnedPrimitive2 =
            reinterpret_cast<DrawSkinnedPrimitive2_t>(0x6AEF90);

        // -------- Engine functions called by the 2b state-setup --------
        // Signatures derived from IDA decompiles of NiDX8Renderer::
        // DrawSkinnedPrimitive2 (0x6AEF90) and SetSkinnedModelTransforms
        // (0x6ACBE0). Each is __thiscall on the appropriate `this` —
        // configurableTexturePipeline for selectAndLinkPass/buildPass,
        // renderState for updateD3DState, lightManager for setState, the
        // NiDX8Renderer itself for SetSkinnedModelTransforms.

        using SelectAndLinkPass_t = void(__thiscall*)(
            void* pipeline,
            void* propertyState,
            void* effectState,
            char hasNormals,
            char alwaysOne,
            void* geomData,
            NI::SkinInstance* skinInstance,
            const NI::Transform* transform,
            void* worldBound);
        using UpdateD3DState_t = void(__thiscall*)(
            void* renderState,
            void* propertyState);
        using BuildPass_t = int(__thiscall*)(
            void* pipeline,
            void* propertyState,
            void* effectState,
            char hasNormals,
            int textureSets,
            void* geomData,
            NI::SkinInstance* skinInstance,
            const NI::Transform* transform,
            void* worldBound);
        using SetSkinnedModelTransforms_t = void(__thiscall*)(
            void* this_,
            NI::SkinInstance* skinInstance,
            const NI::SkinPartition::Partition* partition,
            const NI::Transform* transform,
            void* worldBound);
        using ApplyPixelShader_t = void(__thiscall*)(void* pipeline);
        using ApplyVertexShader_t = void(__thiscall*)(void* pipeline, DWORD fvf);
        using BuildSkinPartitionPass_t = void(__thiscall*)(
            void* pipeline,
            void* propertyState,
            void* effectState,
            char hasNormals,
            char alwaysOne,
            void* geomData,
            NI::SkinInstance* skinInstance,
            const NI::SkinPartition::Partition* partition,
            const NI::Transform* transform,
            void* worldBound);
        using LoadBinary_t = int(__thiscall*)(
            void* pipeline,
            void* geomData,
            NI::SkinInstance* skinInstance,
            const NI::Transform* transform,
            void* worldBound);
        using LightManagerSetState_t = void(__thiscall*)(
            void* lightManager,
            void* effectState,
            void* propTexturing,
            void* propVertexColor);
        using NiBoundCopy_t = void(__thiscall*)(void* dst, const void* src);
        using EnsureAndRegisterStreamables_t = void(__thiscall*)(
            void* pipeline,
            void* propertyState,
            void* effectState,
            char hasNormals,
            char alwaysOne,
            void* geomData,
            NI::SkinInstance* skinInstance,
            const NI::Transform* transform,
            void* worldBound);

        const auto kSelectAndLinkPass =
            reinterpret_cast<SelectAndLinkPass_t>(0x6B9C40);
        const auto kUpdateD3DState =
            reinterpret_cast<UpdateD3DState_t>(0x6B82A0);
        const auto kBuildPass =
            reinterpret_cast<BuildPass_t>(0x6B9C20);
        const auto kSetSkinnedModelTransforms =
            reinterpret_cast<SetSkinnedModelTransforms_t>(0x6ACBE0);
        const auto kApplyPixelShader =
            reinterpret_cast<ApplyPixelShader_t>(0x6B9E70);
        const auto kApplyVertexShader =
            reinterpret_cast<ApplyVertexShader_t>(0x6B9E80);
        const auto kBuildSkinPartitionPass =
            reinterpret_cast<BuildSkinPartitionPass_t>(0x6B9E90);
        const auto kLoadBinary =
            reinterpret_cast<LoadBinary_t>(0x6B9C30);
        const auto kLightManagerSetState =
            reinterpret_cast<LightManagerSetState_t>(0x6BB200);
        const auto kNiBoundCopy =
            reinterpret_cast<NiBoundCopy_t>(0x6ED100);
        const auto kEnsureAndRegisterStreamables =
            reinterpret_cast<EnsureAndRegisterStreamables_t>(0x6B9D30);

        // Pre-computed table mapping numBonesPerVertex (0..4) → D3DVBF enum
        // for D3DRS_VERTEXBLEND. Lives in MW's .rdata.
        DWORD* const kVertexBlendLookup = reinterpret_cast<DWORD*>(0x7C3840);

        // NiPropertyState offsets (IDA-verified 2026-05-16).
        constexpr int kOff_propTexturing   = 0x18;
        constexpr int kOff_propVertexColor = 0x1C;

        // NiDX8Renderer::worldBound offset (NiBound, 16 bytes).
        constexpr int kOff_worldBound = 0x2DC;

        // NiDX8Renderer::d3dDevice offset. This is the device MW itself uses
        // for all draws — under MGE it's the d3d8→d3d9 proxy wrapper. Routing
        // our draws through it (rather than directly to the underlying D3D9)
        // lets the proxy capture state AND lets FFE/PPL intercept fire for
        // the per-partition DrawIndexedPrimitive, matching how vanilla's
        // skinned draws actually render under MGE.
        constexpr int kOff_d3dDevice = 0x24;

        // Shader-side cap on the global-indexed palette (XE Common.fx
        // declares bonePaletteGlobal[32]). Skins exceeding this fall back
        // to the positional 4-matrix path.
        constexpr int kBonePaletteGlobalCap = 32;

        // Convert NI::Transform → D3DXMATRIX matching NiDX8Renderer::
        // SetBoneTransform (0x6ACB10) — transpose 3x3 + scale, place
        // translation in row 3, set last column to identity-ish.
        void niTransformToD3D(const NI::Transform& src, D3DXMATRIX& dst) {
            const NI::Matrix33& r = src.rotation;
            const float s = src.scale;
            dst._11 = r.m0.x * s; dst._12 = r.m1.x * s; dst._13 = r.m2.x * s; dst._14 = 0.0f;
            dst._21 = r.m0.y * s; dst._22 = r.m1.y * s; dst._23 = r.m2.y * s; dst._24 = 0.0f;
            dst._31 = r.m0.z * s; dst._32 = r.m1.z * s; dst._33 = r.m2.z * s; dst._34 = 0.0f;
            dst._41 = src.translation.x; dst._42 = src.translation.y; dst._43 = src.translation.z; dst._44 = 1.0f;
        }

        // -------- NiDX8Renderer struct offsets --------
        // Verified via IDA type_inspect on Morrowind2.i64 2026-05-16.
        // NiDX8Renderer { NiRenderer super; ... } where NiRenderer has
        // currentPropertyState at +0xC and currentEffectState at +0x10.
        constexpr int kOff_currentPropertyState        = 0x0C;
        constexpr int kOff_currentEffectState          = 0x10;
        constexpr int kOff_renderState                 = 0x548;
        constexpr int kOff_lightManager                = 0x558;
        constexpr int kOff_configurableTexturePipeline = 0x55C;

        template<typename T>
        T readRenderField(void* this_, int offset) {
            return *reinterpret_cast<T*>(static_cast<char*>(this_) + offset);
        }

        // The sole xref to 0x6AEF90 is a CALL at 0x6AE00F inside
        // NiDX8Renderer::DrawSkinnedPrimitive (verified via IDA xrefs_to).
        // Original bytes: E8 7C 0F 00 00 (CALL +0xF7C -> 0x6AEF90).
        constexpr DWORD kCallSiteAddr = 0x6AE00F;

        // Cap the smoke-test log spam — every skinned draw fires this hook,
        // which is ~60-200 times per frame. Log only the first N invocations,
        // then go silent. User flips the ini back off to stop entirely.
        constexpr int kMaxSmokeLogs = 16;
        std::atomic<int> g_smokeLogsLeft{kMaxSmokeLogs};
        bool g_hookInstalled = false;

        // Higher-cap diagnostic for cross-NPC archetype-sharing verification.
        // If MW's ModelLoader::LoadNIF cache (0x4EE0C0) shares NIF roots
        // across NPC instances using the same source path, then NPCs using
        // the same body NIF should report identical `skinData=` and
        // `partition0=` pointers (skinInstance differs per NPC because each
        // has its own animated bones[]). Confirming this empirically gates
        // the cross-NPC batching architecture (the proposed grouping key is
        // (Partition*, baseTexture*, ShaderKey)).
        constexpr int kMaxArchetypeLogs = 64;
        std::atomic<int> g_archetypeLogsLeft{kMaxArchetypeLogs};

        // Per-frame palette cache. Keyed by NiSkinInstance*. Each entry holds
        // the up-to-kMaxPaletteSize NI::Transforms produced by buildBonePalette
        // for that skin during this frame. Cleared on onFrameBegin(), populated
        // on observer hook fire. Read by 2b.3c's draw replacement handler.
        struct CachedPalette {
            NI::Transform matrices[kMaxPaletteSize];
            int numBones;
        };
        // Reserve eagerly to avoid rehash-during-render. ~200 skinned shapes
        // per frame at high-end, so 256 buckets leaves headroom.
        std::unordered_map<const NI::SkinInstance*, CachedPalette> g_paletteCache;
        bool g_paletteCacheReserved = false;

        // ----------------------------------------------------------------
        // 2b.3b — Per-partition vertex-buffer rewriter
        // ----------------------------------------------------------------

        // Local mirror of NiGeometryBufferData (engine layout, see
        // skinning-offsets.md and IDA type_inspect). Used to read partition->
        // bufferData without dragging the engine struct into a header.
        // Mirrors enough fields for both the VB rewriter (offsets 0x00-0x18)
        // and the per-partition draw (extends through 0x30).
        struct EngineGeometryBufferData {
            int  geomDataRevisionID;        // 0x00
            int  flags;                     // 0x04
            DWORD fvf;                      // 0x08
            int  verticesCount;             // 0x0C
            unsigned int vertexBufferSize;  // 0x10
            void* d3dVertexBuffer;          // 0x14 — IDirect3DVertexBuffer9*
            unsigned int vertexStride;      // 0x18
            int  indexBufferEntries;        // 0x1C
            unsigned int indexBufferSize;   // 0x20
            void* indexBuffer;              // 0x24 — IDirect3DIndexBuffer9*
            int  field_28;                  // 0x28
            D3DPRIMITIVETYPE primitiveType; // 0x2C
            int  trianglesCount;            // 0x30
        };
        static_assert(sizeof(EngineGeometryBufferData) == 0x34,
            "EngineGeometryBufferData prefix-size mismatch with NiGeometryBufferData layout");

        struct RemappedVBEntry {
            IDirect3DVertexBuffer9* remappedVB = nullptr;  // parallel VB owned by us
            void*    sourceVB = nullptr;                    // tracking ptr for invalidation
            DWORD    dstFvf   = 0;                          // synthesized FVF
            unsigned dstStride = 0;                         // synthesized stride
        };
        std::unordered_map<const NI::SkinPartition::Partition*, RemappedVBEntry> g_remappedVBCache;

        // Source weight-count from FVF position mask, matching PackSkinnedVB
        // (0x6BE2B0):
        //   numBones=1 → XYZB1 + 1 weight (=1.0)
        //   numBones=2 → XYZB1 + 1 explicit weight (2nd implicit)
        //   numBones=3 → XYZB2 + 2 explicit weights (3rd implicit)
        //   numBones=4 → XYZB3 + 3 explicit weights (4th implicit)
        // We map back to *explicit* float count in the source layout — the
        // implicit weight is materialised at synthesis.
        bool srcExplicitWeightCount(DWORD srcPosMask, int& outExplicitFloats) {
            switch (srcPosMask) {
                case D3DFVF_XYZB1: outExplicitFloats = 1; return true;
                case D3DFVF_XYZB2: outExplicitFloats = 2; return true;
                case D3DFVF_XYZB3: outExplicitFloats = 3; return true;
                case D3DFVF_XYZB4: outExplicitFloats = 4; return true;
                default: return false;
            }
        }

        // Synthesize a parallel VB with a UNIFORM normalised layout, regardless
        // of the source partition's numBones. Layout (per vertex):
        //   [12 pos][16 explicit weights (4 floats)][4 indices UBYTE4][srcSuffix]
        //   dstStride = 32 + srcSuffix
        //   dstFvf    = (src extras) | D3DFVF_XYZB5 | D3DFVF_LASTBETA_UBYTE4
        //
        // Why uniform: enables Phase 2 (concatenate all partitions of one skin
        // into a merged VB+IB → one DrawIndexedPrimitive per skin instead of N).
        // Different partitions can have different numBones (and thus different
        // source XYZBn) but identical dst layout permits direct memcpy concat.
        //
        // The materialised explicit weights satisfy `sum(weights) == 1` for any
        // partition. Padded weights (for partitions with <4 bones) are 0 and
        // their paired index is 0 — contribution is 0×bonePaletteGlobal[0] = 0,
        // safe regardless of what's at slot 0. HLSL skinIndexed() reads numWeights
        // and derives the implicit weight; for our pre-materialised layout this
        // derivation reproduces the value we wrote, so no shader change required.
        //
        // Cached by partition*. partition->bones[] and source layout are stable
        // post-NIF-load; we re-synthesise only if the source VB pointer rotates.
        IDirect3DVertexBuffer9* getRemappedVertexBuffer(
            const NI::SkinPartition::Partition* partition,
            DWORD* outFvf = nullptr,
            unsigned* outStride = nullptr)
        {
            auto fillOut = [&](DWORD fvf, unsigned stride) {
                if (outFvf) *outFvf = fvf;
                if (outStride) *outStride = stride;
            };

            if (!partition || !partition->bufferData || !partition->bones) {
                return nullptr;
            }
            const auto* bd = reinterpret_cast<const EngineGeometryBufferData*>(
                partition->bufferData);
            if (!bd->d3dVertexBuffer) {
                return nullptr;
            }

            // Cache hit if source VB unchanged.
            auto cacheIt = g_remappedVBCache.find(partition);
            if (cacheIt != g_remappedVBCache.end()
                && cacheIt->second.sourceVB == bd->d3dVertexBuffer)
            {
                fillOut(cacheIt->second.dstFvf, cacheIt->second.dstStride);
                return cacheIt->second.remappedVB;
            }

            // Determine source layout from FVF.
            const DWORD srcFvf     = bd->fvf;
            const DWORD srcPosMask = srcFvf & D3DFVF_POSITION_MASK;
            int srcExplicitFloats = 0;
            if (!srcExplicitWeightCount(srcPosMask, srcExplicitFloats)) {
                static bool s_loggedOnce = false;
                if (!s_loggedOnce) {
                    s_loggedOnce = true;
                    LOG::logline(
                        "-- [2b-vb] partition=%p fvf=0x%08X (posMask=0x%X) "
                        "— not a skinned format we recognise; falling back",
                        partition, srcFvf, srcPosMask);
                }
                return nullptr;
            }

            // Pack partition->bones[0..3] into a 4-byte index block. Same
            // bytes for every vertex in the partition. Unused slots = 0
            // (paired with weight=0 below, contribution = 0×palette[0]).
            const unsigned short* boneMap = partition->bones;
            const unsigned short numLocalBones = partition->numBones;
            DWORD indexBlock = 0;
            for (unsigned short i = 0; i < numLocalBones && i < 4; ++i) {
                const unsigned short b = boneMap[i];
                if (b > 255) {
                    // UBYTE4 only has 8 bits per index. partition->bones[] is
                    // already constrained to the partition's own bone count
                    // (≤ ~30 in vanilla, ≤ 60 in modded) and indexes into the
                    // skinInstance->bones[] array. We cap buildBonePalette at
                    // kMaxPaletteSize=64, which fits in a byte; >255 would
                    // indicate corrupt NIF data.
                    static bool s_loggedOnce = false;
                    if (!s_loggedOnce) {
                        s_loggedOnce = true;
                        LOG::logline(
                            "-- [2b-vb] partition=%p has bone index %u > 255 "
                            "(can't fit in UBYTE4); falling back",
                            partition, b);
                    }
                    return nullptr;
                }
                indexBlock |= (b & 0xFF) << (i * 8);
            }

            // Uniform normalised destination layout. See header comment.
            const DWORD    dstFvf      = (srcFvf & ~D3DFVF_POSITION_MASK)
                                         | D3DFVF_XYZB5 | D3DFVF_LASTBETA_UBYTE4;
            const unsigned srcStride   = bd->vertexStride;
            const unsigned posBytes    = 12;
            const unsigned srcWeights  = srcExplicitFloats * 4;
            const unsigned srcPrefix   = posBytes + srcWeights;
            const int      srcSuffix   = static_cast<int>(srcStride) - static_cast<int>(srcPrefix);
            if (srcSuffix < 0) {
                LOG::logline(
                    "!! [2b-vb] partition=%p stride %u < expected prefix %u "
                    "(FVF 0x%08X) — refusing to rewrite",
                    partition, srcStride, srcPrefix, srcFvf);
                return nullptr;
            }
            const unsigned dstWeights  = 16;          // always 4 explicit weight floats
            const unsigned dstPrefix   = posBytes + dstWeights;
            const unsigned dstIndices  = 4;
            const unsigned dstStride   = dstPrefix + dstIndices + srcSuffix;

            auto* device = DistantLand::device;
            if (!device) {
                return nullptr;
            }

            IDirect3DVertexBuffer9* newVB = nullptr;
            HRESULT hr = device->CreateVertexBuffer(
                dstStride * bd->verticesCount,
                D3DUSAGE_WRITEONLY,
                dstFvf,
                D3DPOOL_DEFAULT,
                &newVB,
                nullptr);
            if (FAILED(hr) || !newVB) {
                LOG::logline(
                    "!! [2b-vb] CreateVertexBuffer failed for partition=%p (hr=0x%08X)",
                    partition, hr);
                return nullptr;
            }

            auto* srcVB = static_cast<IDirect3DVertexBuffer9*>(bd->d3dVertexBuffer);
            void* srcPtr = nullptr;
            hr = srcVB->Lock(0, 0, &srcPtr, D3DLOCK_READONLY);
            if (FAILED(hr) || !srcPtr) {
                newVB->Release();
                return nullptr;
            }

            void* dstPtr = nullptr;
            hr = newVB->Lock(0, 0, &dstPtr, D3DLOCK_DISCARD);
            if (FAILED(hr) || !dstPtr) {
                srcVB->Unlock();
                newVB->Release();
                return nullptr;
            }

            // partitionNumBones drives weight materialisation. PackSkinnedVB
            // (0x6BE2B0) stores (numBones - 1) explicit weights with the last
            // implicit (= 1 − Σ explicit), EXCEPT numBones=1 which stores
            // 1 explicit weight (= 1.0). We re-derive the full 4-weight tuple
            // here so downstream concat (Phase 2) doesn't need to care which
            // source layout each vertex came from.
            const int partNumBones = static_cast<int>(partition->numBones);
            const BYTE* const srcBytes = static_cast<const BYTE*>(srcPtr);
            BYTE* const       dstBytes = static_cast<BYTE*>(dstPtr);
            for (int v = 0; v < bd->verticesCount; ++v) {
                const BYTE* const sv = srcBytes + v * srcStride;
                BYTE*       const dv = dstBytes + v * dstStride;

                // Position (12 bytes) — straight copy.
                memcpy(dv, sv, posBytes);

                // Materialise 4 explicit weights from source.
                const float* const srcW = reinterpret_cast<const float*>(sv + posBytes);
                float dstW[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
                if (partNumBones == 1) {
                    // Source has 1 explicit weight = 1.0; pad rest to 0.
                    dstW[0] = srcW[0];
                } else {
                    // Source has (numBones - 1) explicit weights; final implicit.
                    float sum = 0.0f;
                    for (int i = 0; i < partNumBones - 1; ++i) {
                        dstW[i] = srcW[i];
                        sum += srcW[i];
                    }
                    dstW[partNumBones - 1] = 1.0f - sum;
                }
                memcpy(dv + posBytes, dstW, sizeof(dstW));

                // 4-byte UBYTE4 indices, constant per partition.
                *reinterpret_cast<DWORD*>(dv + dstPrefix) = indexBlock;

                // Suffix (normal / color / texcoords) — straight copy from
                // the matching offset in the source.
                if (srcSuffix > 0) {
                    memcpy(dv + dstPrefix + dstIndices, sv + srcPrefix, srcSuffix);
                }
            }

            newVB->Unlock();
            srcVB->Unlock();

            if (cacheIt != g_remappedVBCache.end()) {
                if (cacheIt->second.remappedVB) {
                    cacheIt->second.remappedVB->Release();
                }
                cacheIt->second = { newVB, bd->d3dVertexBuffer, dstFvf, dstStride };
            } else {
                g_remappedVBCache[partition] = { newVB, bd->d3dVertexBuffer, dstFvf, dstStride };
            }
            fillOut(dstFvf, dstStride);
            return newVB;
        }

        // ----------------------------------------------------------------
        // Phase 2 — Per-skinInstance merged VB+IB (one drawcall per shape)
        // ----------------------------------------------------------------

        // A skin's partitions all share the same NiTriShape and therefore the
        // same material/texture/shader signature. Post-Phase 1 they also share
        // the same synthesized VB layout (uniform XYZB5|LASTBETA_UBYTE4 +
        // suffix). So we can concatenate all partition VBs into one merged
        // VB and all partition IBs into one merged IB (with vertex-offset
        // added to each index) — then the color pass is a single
        // DrawIndexedPrimitive covering the whole shape.
        //
        // The per-partition synthesized VBs remain valid and are still pushed
        // to DistantLand::recordMW so the depth/shadow positional path renders
        // each partition independently (depth shader doesn't know about the
        // indexed palette; staying per-partition keeps it working without
        // touching XE Depth.fx / XE Mod Shadow.fx).
        struct MergedSkinEntry {
            IDirect3DVertexBuffer9* vb = nullptr;
            IDirect3DIndexBuffer9*  ib = nullptr;
            DWORD     fvf       = 0;       // common normalised FVF
            unsigned  stride    = 0;       // common normalised stride
            UINT      totalVertices  = 0;
            UINT      totalTriangles = 0;
            D3DFORMAT ibFormat  = D3DFMT_INDEX16;
            int       partitionCount = 0;
            // Per-partition source VB pointers captured at build time. If any
            // rotates (MW rebuilt the VB), the merged entry is stale.
            std::vector<void*> witnessSourceVBs;
        };
        std::unordered_map<const NI::SkinInstance*, MergedSkinEntry>
            g_mergedSkinCache;

        // Returns a cached merged entry for the skin, or nullptr if:
        //   - skin has ≤1 partition (no merge benefit)
        //   - any partition's synthesized VB can't be built
        //   - partitions have inconsistent layout (shouldn't happen post-Phase 1)
        //   - total vertex count exceeds the index format's range
        //   - allocation / lock failure
        // Builds + caches on first success per skinInstance.
        const MergedSkinEntry* getMergedSkin(
            const NI::SkinInstance* skinInstance)
        {
            if (!skinInstance) return nullptr;
            const NI::SkinData* const skinData = skinInstance->skinData;
            const NI::SkinPartition* const skinPart = skinData
                ? static_cast<const NI::SkinPartition*>(skinData->partition)
                : nullptr;
            if (!skinPart || skinPart->partitionCount <= 1) return nullptr;

            const NI::SkinPartition::Partition* const parts = skinPart->partitions;
            const int partCount = static_cast<int>(skinPart->partitionCount);

            // Cache lookup with witness validation.
            auto cacheIt = g_mergedSkinCache.find(skinInstance);
            if (cacheIt != g_mergedSkinCache.end()) {
                const auto& e = cacheIt->second;
                bool valid = static_cast<int>(e.witnessSourceVBs.size()) == partCount;
                for (int p = 0; valid && p < partCount; ++p) {
                    const auto* bd = reinterpret_cast<const EngineGeometryBufferData*>(
                        parts[p].bufferData);
                    void* cur = bd ? bd->d3dVertexBuffer : nullptr;
                    if (e.witnessSourceVBs[p] != cur) valid = false;
                }
                if (valid) return &cacheIt->second;
                // Stale → release and rebuild.
                if (e.vb) e.vb->Release();
                if (e.ib) e.ib->Release();
                g_mergedSkinCache.erase(cacheIt);
            }

            // Build pass 1: gather per-partition synthesized VBs, verify
            // uniform layout, sum totals, capture witnesses.
            DWORD     commonFvf    = 0;
            unsigned  commonStride = 0;
            UINT      totalVerts   = 0;
            UINT      totalTris    = 0;
            D3DFORMAT ibFmt        = D3DFMT_INDEX16;
            std::vector<IDirect3DVertexBuffer9*> partVBs(partCount, nullptr);
            std::vector<void*> witnessVBs(partCount, nullptr);
            for (int p = 0; p < partCount; ++p) {
                const auto& part = parts[p];
                const auto* bd = reinterpret_cast<const EngineGeometryBufferData*>(
                    part.bufferData);
                if (!bd || !bd->indexBuffer) return nullptr;
                DWORD    fvf    = 0;
                unsigned stride = 0;
                IDirect3DVertexBuffer9* synthVB =
                    getRemappedVertexBuffer(&part, &fvf, &stride);
                if (!synthVB) return nullptr;
                if (p == 0) {
                    commonFvf = fvf;
                    commonStride = stride;
                    // First partition's IB format dictates merged IB format.
                    auto* srcIB = static_cast<IDirect3DIndexBuffer9*>(bd->indexBuffer);
                    D3DINDEXBUFFER_DESC desc;
                    if (FAILED(srcIB->GetDesc(&desc))) return nullptr;
                    ibFmt = desc.Format;
                } else if (fvf != commonFvf || stride != commonStride) {
                    // Should not happen post-Phase 1 normalisation. If it
                    // ever does, defer to per-partition rendering.
                    return nullptr;
                }
                partVBs[p]    = synthVB;
                witnessVBs[p] = bd->d3dVertexBuffer;
                totalVerts   += bd->verticesCount;
                totalTris    += bd->trianglesCount;
            }

            // Vertex-count cap for uint16 indices.
            if (ibFmt == D3DFMT_INDEX16 && totalVerts > 0xFFFF) {
                return nullptr;
            }

            auto* device = DistantLand::device;
            if (!device) return nullptr;

            IDirect3DVertexBuffer9* mvb = nullptr;
            HRESULT hr = device->CreateVertexBuffer(
                totalVerts * commonStride, D3DUSAGE_WRITEONLY, commonFvf,
                D3DPOOL_DEFAULT, &mvb, nullptr);
            if (FAILED(hr) || !mvb) {
                LOG::logline(
                    "!! [2b-merge] CreateVertexBuffer failed for skin=%p (hr=0x%08X)",
                    skinInstance, hr);
                return nullptr;
            }

            const unsigned bytesPerIndex = (ibFmt == D3DFMT_INDEX16) ? 2u : 4u;
            IDirect3DIndexBuffer9* mib = nullptr;
            hr = device->CreateIndexBuffer(
                totalTris * 3 * bytesPerIndex, D3DUSAGE_WRITEONLY, ibFmt,
                D3DPOOL_DEFAULT, &mib, nullptr);
            if (FAILED(hr) || !mib) {
                LOG::logline(
                    "!! [2b-merge] CreateIndexBuffer failed for skin=%p (hr=0x%08X)",
                    skinInstance, hr);
                mvb->Release();
                return nullptr;
            }

            // Lock merged VB+IB once, fill partition-by-partition.
            void* mvbPtr = nullptr;
            hr = mvb->Lock(0, 0, &mvbPtr, D3DLOCK_DISCARD);
            if (FAILED(hr) || !mvbPtr) {
                mvb->Release(); mib->Release(); return nullptr;
            }
            void* mibPtr = nullptr;
            hr = mib->Lock(0, 0, &mibPtr, D3DLOCK_DISCARD);
            if (FAILED(hr) || !mibPtr) {
                mvb->Unlock();
                mvb->Release(); mib->Release(); return nullptr;
            }

            BYTE* vbDst = static_cast<BYTE*>(mvbPtr);
            BYTE* ibDst = static_cast<BYTE*>(mibPtr);
            UINT  vbOffsetVerts = 0;
            bool  ok = true;

            for (int p = 0; p < partCount && ok; ++p) {
                const auto& part = parts[p];
                const auto* bd = reinterpret_cast<const EngineGeometryBufferData*>(
                    part.bufferData);

                // Concatenate partition VB.
                void* srcVbPtr = nullptr;
                if (FAILED(partVBs[p]->Lock(0, 0, &srcVbPtr, D3DLOCK_READONLY))
                    || !srcVbPtr) { ok = false; break; }
                const unsigned partVbBytes = bd->verticesCount * commonStride;
                memcpy(vbDst, srcVbPtr, partVbBytes);
                partVBs[p]->Unlock();
                vbDst += partVbBytes;

                // Concatenate partition IB with vertex-offset adjustment.
                auto* srcIB = static_cast<IDirect3DIndexBuffer9*>(bd->indexBuffer);
                void* srcIbPtr = nullptr;
                if (FAILED(srcIB->Lock(0, 0, &srcIbPtr, D3DLOCK_READONLY))
                    || !srcIbPtr) { ok = false; break; }
                const unsigned partIndexCount = bd->trianglesCount * 3;
                if (ibFmt == D3DFMT_INDEX16) {
                    const uint16_t* src = static_cast<const uint16_t*>(srcIbPtr);
                    uint16_t* dst = reinterpret_cast<uint16_t*>(ibDst);
                    for (unsigned i = 0; i < partIndexCount; ++i) {
                        dst[i] = static_cast<uint16_t>(src[i] + vbOffsetVerts);
                    }
                    ibDst += partIndexCount * 2;
                } else {
                    const uint32_t* src = static_cast<const uint32_t*>(srcIbPtr);
                    uint32_t* dst = reinterpret_cast<uint32_t*>(ibDst);
                    for (unsigned i = 0; i < partIndexCount; ++i) {
                        dst[i] = src[i] + vbOffsetVerts;
                    }
                    ibDst += partIndexCount * 4;
                }
                srcIB->Unlock();
                vbOffsetVerts += bd->verticesCount;
            }

            mib->Unlock();
            mvb->Unlock();

            if (!ok) {
                mvb->Release(); mib->Release(); return nullptr;
            }

            auto& entry = g_mergedSkinCache[skinInstance];
            entry.vb              = mvb;
            entry.ib              = mib;
            entry.fvf             = commonFvf;
            entry.stride          = commonStride;
            entry.totalVertices   = totalVerts;
            entry.totalTriangles  = totalTris;
            entry.ibFormat        = ibFmt;
            entry.partitionCount  = partCount;
            entry.witnessSourceVBs = std::move(witnessVBs);
            return &entry;
        }

        // ----------------------------------------------------------------
        // 2c — Cross-NPC batched drain (one drawcall per archetype group)
        // ----------------------------------------------------------------

        // Snapshot of one DSP2 observer call. Carries everything the drain
        // needs to (a) re-call MW state-setup helpers for the batch
        // representative and (b) look up the per-instance palette.
        struct QueuedDraw {
            void*               renderer;
            void*               geomData;
            NI::SkinInstance*   skinInstance;
            const NI::Transform* transform;
            void*               worldBound;
            int                 primitiveType;
            int                 unused1;
            int                 unused2;
            int                 cachedPaletteCount;  // skinData->numBones
            int                 buildPassFlags;      // v15 from kBuildPass at observer time
            int                 loadBinaryFlags;     // binary flags at observer time
        };
        std::vector<QueuedDraw> g_drawQueue;

        // 2c V2 — texture-based palette has its own cap
        // (FixedFunctionShader::getMaxBonesInPaletteTex()). The old const-
        // register kMaxBatchPalette alias is gone; use the texture-side
        // constant directly to make the dependency explicit.

        // Reusable per-frame VB for the hardware-instancing stream-1
        // (per-instance baseBoneOffset). One FLOAT per instance.
        IDirect3DVertexBuffer9* g_instanceOffsetVB = nullptr;
        constexpr unsigned int kInstanceOffsetVBCapacity = 256;

        // Cached vertex declarations keyed by stream-0 FVF. Built once per
        // unique FVF via D3DXDeclaratorFromFVF + appended INSTANCEDATA
        // element on stream 1 (FLOAT1 → TEXCOORD7 semantic, matching the
        // FFE-generated input struct).
        std::unordered_map<DWORD, IDirect3DVertexDeclaration9*> g_batchVertexDecls;

        bool ensureInstanceOffsetVB() {
            if (g_instanceOffsetVB) return true;
            auto* device = DistantLand::device;
            if (!device) return false;
            HRESULT hr = device->CreateVertexBuffer(
                kInstanceOffsetVBCapacity * sizeof(float),
                D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC,
                0, D3DPOOL_DEFAULT,
                &g_instanceOffsetVB, nullptr);
            if (FAILED(hr) || !g_instanceOffsetVB) {
                g_instanceOffsetVB = nullptr;
                return false;
            }
            return true;
        }

        IDirect3DVertexDeclaration9* getBatchVertexDecl(DWORD srcFvf) {
            auto it = g_batchVertexDecls.find(srcFvf);
            if (it != g_batchVertexDecls.end()) return it->second;

            // Generate stream-0 declaration from the FVF, find the END
            // marker, append a stream-1 INSTANCEDATA element, terminate.
            D3DVERTEXELEMENT9 elements[MAX_FVF_DECL_SIZE + 2];
            if (FAILED(D3DXDeclaratorFromFVF(srcFvf, elements))) return nullptr;
            int n = 0;
            while (n < MAX_FVF_DECL_SIZE && elements[n].Stream != 0xFF) ++n;
            if (n >= MAX_FVF_DECL_SIZE - 1) return nullptr;
            // Stream 1, offset 0, FLOAT1, default method, TEXCOORD7 — matches
            // `float baseBoneOffset : TEXCOORD7` emitted by the FFE generator
            // when ShaderKey::usesBatchedPalette is set.
            elements[n]   = { 1, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT,
                              D3DDECLUSAGE_TEXCOORD, 7 };
            elements[n+1] = D3DDECL_END();

            auto* device = DistantLand::device;
            if (!device) return nullptr;
            IDirect3DVertexDeclaration9* decl = nullptr;
            HRESULT hr = device->CreateVertexDeclaration(elements, &decl);
            if (FAILED(hr) || !decl) {
                LOG::logline(
                    "!! [2c] CreateVertexDeclaration failed for fvf=0x%08X (hr=0x%08X)",
                    srcFvf, hr);
                return nullptr;
            }
            g_batchVertexDecls[srcFvf] = decl;
            return decl;
        }

        // Diagnostic counters for drain stats.
        std::atomic<int> g_drainLogsLeft{kMaxSmokeLogs};

        // -------- Option-D per-frame perf instrumentation --------
        // Aggregated over kPerfReportFrames frames, logged once, then reset.
        // Goal: answer "is skinned drawcall load actually a big slice of
        // the frame?" before deciding between A (re-enable 2c V1 with
        // [32] cap) and C (texture-based palette).
        constexpr int kPerfReportFrames = 60;
        struct PerfFrame {
            int rawDrawcallsAtStart = 0;
            int takeoverInvocations  = 0;
            int singlePartitionSkins = 0;
            int multiPartitionSkins  = 0;
            int positionalFallback   = 0;
            int colorDrawsSkinned    = 0;
        };
        struct PerfAccum {
            int       frames                   = 0;
            long long totalPresentDeltaTicks   = 0;
            long long totalRawDrawcalls        = 0;
            long long totalTakeoverInvocations = 0;
            long long totalSinglePartition     = 0;
            long long totalMultiPartition      = 0;
            long long totalPositionalFallback  = 0;
            long long totalColorDrawsSkinned   = 0;
        };
        PerfFrame g_perfFrame;
        PerfAccum g_perfAccum;
        LARGE_INTEGER g_perfQpcFreq{};
        bool g_perfQpcFreqInit = false;
        // True frame time is measured Present-to-Present in onPresent().
        // First Present has no prior timestamp; we skip its delta and
        // start accumulating from the second.
        LARGE_INTEGER g_lastPresentTicks{};
        bool g_lastPresentValid = false;

        // Render N instances of one archetype as a single batched draw via
        // D3D9 hardware instancing. Re-calls MW state-setup helpers for the
        // representative member, builds a mega-palette (N × bonesPerInstance
        // matrices) in view space, populates the per-instance offset stream,
        // and issues one DrawIndexedPrimitive with batchInstances = N.
        //
        // Falls back to per-member inline render only on hard failure (alloc,
        // palette overflow, etc.) — split-batching is a follow-up.
        void renderBatchedGroup(
            const NI::SkinPartition::Partition* partition,
            std::vector<QueuedDraw*>& members,
            int& outBatchesIssued,
            int& outInstancesCovered)
        {
            if (members.empty() || !partition) return;
            auto* device = DistantLand::device;
            if (!device) return;

            const QueuedDraw& rep = *members[0];
            const int N = static_cast<int>(members.size());
            const int bonesPerInstance = rep.cachedPaletteCount;
            if (bonesPerInstance <= 0) return;

            // Palette overflow guard. With VTF the cap is the texture
            // width (kMaxBonesInPaletteTex), not the SM3 const budget,
            // so split-batching is far less likely to be needed.
            if (static_cast<unsigned>(N) > kInstanceOffsetVBCapacity
                || static_cast<unsigned>(N * bonesPerInstance) > FixedFunctionShader::getMaxBonesInPaletteTex())
            {
                // TODO: split into sub-batches. For V2, skip batched path —
                // the per-member observer-time recordMW push already covers
                // depth/shadow; we just lose color batching for the group.
                return;
            }

            // Get the per-Partition synth VB (cached, shared across all
            // same-NIF instances). This is stream 0 for the batched draw.
            DWORD    synthFvf    = 0;
            unsigned synthStride = 0;
            IDirect3DVertexBuffer9* synthVB =
                getRemappedVertexBuffer(partition, &synthFvf, &synthStride);
            if (!synthVB) return;

            const auto* bd = reinterpret_cast<const EngineGeometryBufferData*>(
                partition->bufferData);
            if (!bd || !bd->indexBuffer) return;

            auto* decl = getBatchVertexDecl(synthFvf);
            if (!decl) return;
            if (!ensureInstanceOffsetVB()) return;

            // ---- Mesh-wide state setup for the representative ----
            // The proxy-captured globalRs/frs/lrs gets populated as a side
            // effect — we then use it for the batched render.
            void* propertyState = readRenderField<void*>(rep.renderer, kOff_currentPropertyState);
            void* effectState   = readRenderField<void*>(rep.renderer, kOff_currentEffectState);
            void* renderState   = readRenderField<void*>(rep.renderer, kOff_renderState);
            void* pipeline      = readRenderField<void*>(rep.renderer, kOff_configurableTexturePipeline);
            void* lightManager  = readRenderField<void*>(rep.renderer, kOff_lightManager);

            const char hasNormals = (bd->fvf & D3DFVF_NORMAL) ? 1 : 0;
            const int  textureSets = static_cast<int>(
                (bd->fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT);

            kSelectAndLinkPass(
                pipeline, propertyState, effectState,
                hasNormals, /*alwaysOne*/ 1,
                rep.geomData, rep.skinInstance, rep.transform, rep.worldBound);
            kUpdateD3DState(renderState, propertyState);
            int v15 = kBuildPass(
                pipeline, propertyState, effectState,
                hasNormals, textureSets,
                rep.geomData, rep.skinInstance, rep.transform, rep.worldBound);

            if (!(v15 & 0x10000000)) {
                void* propTex = readRenderField<void*>(propertyState, kOff_propTexturing);
                void* propVC  = readRenderField<void*>(propertyState, kOff_propVertexColor);
                kLightManagerSetState(lightManager, effectState, propTex, propVC);
            }
            if (v15 & 0x40000000) {
                // Mesh wants ensureAndRegister-only behavior — no draws.
                kEnsureAndRegisterStreamables(
                    pipeline, propertyState, effectState,
                    hasNormals, 1,
                    rep.geomData, rep.skinInstance, rep.transform, rep.worldBound);
                return;
            }
            int binary = kLoadBinary(
                pipeline, rep.geomData, rep.skinInstance, rep.transform, rep.worldBound);
            if (!(binary & 0x8000000)) {
                kApplyPixelShader(pipeline);
            }
            kNiBoundCopy(
                static_cast<char*>(rep.renderer) + kOff_worldBound,
                rep.worldBound);
            if (!(binary & 0x4000000)) {
                kApplyVertexShader(pipeline, synthFvf);
            }
            if (!(binary & 0x1000000)) {
                kBuildSkinPartitionPass(
                    pipeline, propertyState, effectState,
                    hasNormals, 1,
                    rep.geomData, rep.skinInstance, partition, rep.transform, rep.worldBound);
            }

            const RenderedState* globalRs = MGEProxyState::getRenderedState();
            FragmentState*       frs      = MGEProxyState::getFragmentState();
            LightState*          lrs      = MGEProxyState::getLightState();

            // ---- Build mega-palette directly into bonePaletteTex ----
            // 4 RGBA32F texels per mat4. Layout per instance: 4*bonesPerInstance
            // texels at offset (instanceSlot * bonesPerInstance * 4) texels.
            // LockRect(DISCARD) per batch so the driver can rename buffers
            // and avoid render-thread stalls. Sampled in VS via tex2Dlod
            // in skinIndexedBatched.
            if (!FixedFunctionShader::getBonePaletteTex()) return;
            const unsigned totalBones = static_cast<unsigned>(N * bonesPerInstance);
            const unsigned totalTexels = totalBones * 4;
            if (totalBones > FixedFunctionShader::getMaxBonesInPaletteTex()) {
                // Beyond texture capacity — split-batching would handle this
                // but isn't implemented in V2. Skip; per-member fallback in
                // caller covers the depth/shadow recordMW path.
                return;
            }
            D3DLOCKED_RECT locked;
            HRESULT hr = FixedFunctionShader::getBonePaletteTex()->LockRect(
                0, &locked, nullptr, D3DLOCK_DISCARD);
            if (FAILED(hr) || !locked.pBits) return;
            float* dst = static_cast<float*>(locked.pBits);  // 4 floats per texel
            for (int i = 0; i < N; ++i) {
                const QueuedDraw& m = *members[i];
                int cnt = 0;
                const NI::Transform* p = getCachedPalette(m.skinInstance, &cnt);
                if (!p || cnt != bonesPerInstance) {
                    // Heterogeneous palette size in the group — shouldn't
                    // happen since same Partition ⇒ same skinData ⇒ same
                    // numBones. If it does, skip the batched draw (caller's
                    // recordMW push already covers depth/shadow).
                    FixedFunctionShader::getBonePaletteTex()->UnlockRect(0);
                    return;
                }
                for (int b = 0; b < bonesPerInstance; ++b) {
                    D3DXMATRIX worldMat, viewMat;
                    niTransformToD3D(p[b], worldMat);
                    D3DXMatrixMultiply(&viewMat, &worldMat, &globalRs->viewTransform);
                    // Write 4 rows × 4 floats = 16 floats per mat4. D3D9
                    // D3DXMATRIX is row-major in memory.
                    const float* src = &viewMat._11;
                    const unsigned baseFloat = (i * bonesPerInstance + b) * 16;
                    memcpy(dst + baseFloat, src, 16 * sizeof(float));
                }
            }
            FixedFunctionShader::getBonePaletteTex()->UnlockRect(0);

            // ---- Per-instance offset stream ----
            void* offsetPtr = nullptr;
            hr = g_instanceOffsetVB->Lock(
                0, N * sizeof(float), &offsetPtr, D3DLOCK_DISCARD);
            if (FAILED(hr) || !offsetPtr) return;
            float* offsets = static_cast<float*>(offsetPtr);
            for (int i = 0; i < N; ++i) {
                offsets[i] = static_cast<float>(i * bonesPerInstance);
            }
            g_instanceOffsetVB->Unlock();

            // ---- Build batched RenderedState + emit ----
            // Note: bonePaletteGlobal (const-register) is NOT set — the
            // shader samples bonePaletteTex instead when usesBatchedPalette.
            RenderedState batchRs = *globalRs;
            batchRs.vb         = synthVB;
            batchRs.vbOffset   = 0;
            batchRs.vbStride   = synthStride;
            batchRs.fvf        = synthFvf;
            batchRs.ib         = static_cast<IDirect3DIndexBuffer9*>(bd->indexBuffer);
            batchRs.ibBase     = 0;
            batchRs.primType   = static_cast<D3DPRIMITIVETYPE>(rep.primitiveType);
            batchRs.baseIndex  = 0;
            batchRs.minIndex   = 0;
            batchRs.vertCount  = bd->verticesCount;
            batchRs.startIndex = 0;
            batchRs.primCount  = bd->trianglesCount;
            batchRs.numWeights = 4;
            batchRs.usesGlobalPalette       = true;
            batchRs.bonePaletteGlobal       = nullptr;  // VTF path uses texture
            batchRs.bonePaletteGlobalCount  = 0;
            batchRs.usesBatchedPalette      = true;
            batchRs.batchInstanceVB         = g_instanceOffsetVB;
            batchRs.batchVertexDecl         = decl;
            batchRs.batchInstances          = N;
            batchRs.batchBonesPerInstance   = bonesPerInstance;

            device->SetStreamSource(0, batchRs.vb, 0, batchRs.vbStride);
            device->SetIndices(batchRs.ib);
            // FVF binding is overridden by SetVertexDeclaration inside
            // renderMorrowind (gated on usesBatchedPalette). No SetFVF here.

            FixedFunctionShader::renderMorrowind(&batchRs, frs, lrs);

            kEnsureAndRegisterStreamables(
                pipeline, propertyState, effectState,
                hasNormals, 1,
                rep.geomData, rep.skinInstance, rep.transform, rep.worldBound);

            ++outBatchesIssued;
            outInstancesCovered += N;
        }

        // __fastcall on MSVC x86 for a free function: arg0 in ECX, arg1 in
        // EDX, remaining args on stack with callee cleanup. That matches
        // __thiscall semantics (this in ECX, callee cleans) if we treat the
        // EDX slot as a discarded scratch param. The CALL site we redirect
        // doesn't set EDX deliberately, so it's safe to ignore.
        void __fastcall observerDrawSkinnedPrimitive2(
            void* this_,
            void* /*edx scratch*/,
            int primitiveType,
            void* geomData,
            NI::SkinInstance* skinInstance,
            const NI::Transform* transform,
            void* worldBound,
            int unused1,
            int unused2)
        {
            // Build our per-mesh palette and stash it in the per-frame cache.
            // Multiple partitions of the same skinInstance fire DrawSkinned-
            // Primitive2 only once per Display() — but multi-pass Click()s
            // (e.g. arm camera + main camera) can call us twice for the same
            // skin in a frame. The cache de-dupes that within onFrameBegin's
            // lifetime.
            if (skinInstance && transform) {
                auto [it, inserted] = g_paletteCache.try_emplace(skinInstance);
                if (inserted) {
                    it->second.numBones = buildBonePalette(
                        skinInstance, *transform, it->second.matrices);

                    // Sparse log: first few invocations only, for sanity.
                    int slot = g_smokeLogsLeft.fetch_sub(1);
                    if (slot > 0 && it->second.numBones > 0) {
                        LOG::logline(
                            "-- [2b-smoke] DSP2 skin=%p numBones=%d bone0.t=(%.3f,%.3f,%.3f) bone0.s=%.4f",
                            skinInstance, it->second.numBones,
                            it->second.matrices[0].translation.x,
                            it->second.matrices[0].translation.y,
                            it->second.matrices[0].translation.z,
                            it->second.matrices[0].scale);
                    }

                    // ---- [2b-archetype] cross-NPC sharing diagnostic ----
                    // Logs (skinInstance, skinData, partition0, partCount)
                    // sparsely so we can verify that two NPCs using the same
                    // body NIF report the SAME skinData/partition0 pointers
                    // while their skinInstance pointers differ. That's the
                    // empirical gate before committing to cross-NPC batching
                    // by (Partition*, baseTexture*, ShaderKey).
                    //
                    // Grep `[2b-archetype]` and look for skinData= values
                    // that repeat across DIFFERENT skin= pointers — those are
                    // batch candidates for the same source mesh.
                    int aSlot = g_archetypeLogsLeft.fetch_sub(1);
                    if (aSlot > 0) {
                        const NI::SkinData* sd = skinInstance->skinData;
                        const NI::SkinPartition* sp = sd
                            ? static_cast<const NI::SkinPartition*>(sd->partition)
                            : nullptr;
                        const NI::SkinPartition::Partition* p0 =
                            (sp && sp->partitionCount > 0) ? &sp->partitions[0] : nullptr;
                        const unsigned int pc = sp ? sp->partitionCount : 0;
                        const auto* bd0 = (p0)
                            ? reinterpret_cast<const EngineGeometryBufferData*>(p0->bufferData)
                            : nullptr;
                        void* srcVB0 = bd0 ? bd0->d3dVertexBuffer : nullptr;
                        LOG::logline(
                            "-- [2b-archetype] skin=%p skinData=%p partition0=%p "
                            "srcVB0=%p partCount=%u numBones=%d",
                            skinInstance, sd, p0, srcVB0, pc, it->second.numBones);
                    }
                }
            }

            // 2b.3c.2a — When the takeover flag is on AND we have a built
            // palette for this skin, iterate the partitions ourselves and
            // issue raw DrawIndexedPrimitive calls per partition. This is
            // still an INCOMPLETE takeover:
            //   - No MW state-setup is called (selectAndLinkPass etc.) — the
            //     bound shader, textures, and per-bone WORLDMATRIX values
            //     are whatever the previous draw left behind.
            //   - VB binding uses MW's ORIGINAL vertex buffer (local bone
            //     indices), not our remapped one yet.
            //   - The currently-bound shader almost certainly doesn't know
            //     about skinning for this geometry.
            // Expected result: NPCs APPEAR but render garbled (wrong colors,
            // wrong skinning, possibly T-pose). Visibility proves the
            // partition iteration + raw draw path works end-to-end.
            // 2b.3c.2b will add the proper state setup; 2b.3c.2c switches
            // to remapped VB + global palette.
            const bool takeover =
                Configuration.MeshLevelSkinning
                && skinInstance
                && transform
                && g_paletteCache.count(skinInstance);
            if (takeover) {
                // 2b.3c.2b — Full state-setup takeover. Replaces the vanilla
                // DrawSkinnedPrimitive2 entirely. We:
                //   1. Call MW's selectAndLinkPass / updateD3DState / buildPass
                //      via their addresses to set up shader/texture state on
                //      the renderer + D3D9 device. (lightManager::setState
                //      omitted for now — lighting may be wrong but mesh should
                //      still be visible.)
                //   2. For each partition, call MW's SetSkinnedModelTransforms
                //      to push per-bone WORLDMATRIX values, then SetStreamSource/
                //      SetIndices/SetFVF/DrawIndexedPrimitive directly.
                //
                // applyVertexShader / applyPixelShader / buildSkinPartitionPass /
                // loadBinary are skipped — those are conditional refinements; if
                // mesh renders without them we're done with this iteration.
                //
                // 2b.3c.2c will swap to remapped VB + global palette + indexed
                // blending in the shader.

                const NI::SkinData* skinData = skinInstance->skinData;
                const NI::SkinPartition* skinPartition =
                    skinData ? static_cast<const NI::SkinPartition*>(skinData->partition) : nullptr;
                // We render via the raw D3D9 device (DistantLand::device),
                // bypassing MGE's d3d8 proxy. Tried going through the proxy
                // (this_->d3dDevice) and crashed: MW's d3dDevice is typed
                // IDirect3DDevice8 with D3D8 vtable layout (e.g. 3-arg
                // SetStreamSource), incompatible with our IDirect3DDevice9*
                // cast (4-arg SetStreamSource). Bypassing the proxy means our
                // draws use D3D9 FFP instead of the FFE/PPL HLSL substitution
                // — alpha-handling divergence for PPL users is a known
                // outstanding issue to address separately.
                auto* device = DistantLand::device;

                static std::atomic<int> diagLeft{kMaxSmokeLogs};
                int partTotal = 0, partSubmitted = 0;
                int partGlobalPalette = 0, partPositionalFallback = 0;
                int mergedDrawcalls = 0, collapsedParts = 0;
                int skipMergeBuild  = 0;
                // Reason counters for why the 2b path was skipped:
                int skipNoCachedPalette = 0;
                int skipPaletteTooLarge = 0;
                int skipNoRemappedVB    = 0;

                if (skinPartition && device && skinPartition->partitionCount > 0) {
                    // Read renderer struct fields.
                    void* propertyState = readRenderField<void*>(this_, kOff_currentPropertyState);
                    void* effectState   = readRenderField<void*>(this_, kOff_currentEffectState);
                    void* renderState   = readRenderField<void*>(this_, kOff_renderState);
                    void* pipeline      = readRenderField<void*>(this_, kOff_configurableTexturePipeline);

                    // Derive hasNormals / textureSets from the first partition's
                    // buffer FVF, matching the engine's prologue logic.
                    const NI::SkinPartition::Partition* partitions = skinPartition->partitions;
                    const auto* bd0 = reinterpret_cast<const EngineGeometryBufferData*>(partitions[0].bufferData);
                    char hasNormals = (bd0 && (bd0->fvf & D3DFVF_NORMAL) != 0) ? 1 : 0;
                    int  textureSets = bd0 ? static_cast<int>((bd0->fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT) : 1;

                    void* lightManager = readRenderField<void*>(this_, kOff_lightManager);

                    // ---- Mesh-wide setup ----
                    kSelectAndLinkPass(
                        pipeline, propertyState, effectState,
                        hasNormals, /*alwaysOne*/ 1,
                        geomData, skinInstance, transform, worldBound);
                    kUpdateD3DState(renderState, propertyState);
                    int v15 = kBuildPass(
                        pipeline, propertyState, effectState,
                        hasNormals, textureSets,
                        geomData, skinInstance, transform, worldBound);

                    if (!(v15 & 0x10000000)) {
                        void* propTex = readRenderField<void*>(propertyState, kOff_propTexturing);
                        void* propVC  = readRenderField<void*>(propertyState, kOff_propVertexColor);
                        kLightManagerSetState(lightManager, effectState, propTex, propVC);
                    }

                    // Vanilla bails to ensureAndRegisterStreamables only on
                    // this flag — partition loop is skipped entirely.
                    if (v15 & 0x40000000) {
                        kEnsureAndRegisterStreamables(
                            pipeline, propertyState, effectState,
                            hasNormals, 1,
                            geomData, skinInstance, transform, worldBound);
                    } else {
                        // ---- loadBinary + applyPixelShader (mesh-wide) ----
                        // Populates texture-stage + pipeline state via the
                        // MGE proxy. Captured into frs / lightrs / parts of rs.
                        int binary = kLoadBinary(
                            pipeline, geomData, skinInstance, transform, worldBound);
                        if (!(binary & 0x8000000)) {
                            kApplyPixelShader(pipeline);
                        }
                        kNiBoundCopy(
                            static_cast<char*>(this_) + kOff_worldBound,
                            worldBound);

                        // FFE pivot, iteration 2: local-copy variant. We do
                        // NOT mutate the global rs (last attempt poisoned it
                        // and broke static draws). Instead, snapshot the
                        // global per partition, patch the snapshot, hand it
                        // to renderMorrowind. Global stays clean for MW's
                        // subsequent intercepted draws.
                        //
                        // No raw D3DRS_VERTEXBLEND set here — FFE doesn't use
                        // D3D9 FFP vertex blending; it skins in HLSL via
                        // rs.numWeights + bone palette. We just ensure
                        // localRs.numWeights matches the partition.
                        const RenderedState* globalRs = MGEProxyState::getRenderedState();
                        FragmentState*       frs      = MGEProxyState::getFragmentState();
                        LightState*          lrs      = MGEProxyState::getLightState();

                        const unsigned int partCount = skinPartition->partitionCount;
                        const int v19 = v15 & 0x800000;

                        // ---- Lift palette computation out of the loop ----
                        // Same cachedPalette → same view-space paletteBuf for
                        // every partition. Pre-computing avoids redundant
                        // niTransformToD3D + matrix-multiply N times. Also
                        // lets the merged-skin color draw (Phase 2) use the
                        // same buffer without duplicating the work.
                        int cachedCount = 0;
                        const NI::Transform* cachedPalette =
                            getCachedPalette(skinInstance, &cachedCount);
                        const bool globalFeasible =
                            cachedPalette
                            && cachedCount > 0
                            && cachedCount <= kBonePaletteGlobalCap;
                        D3DXMATRIX paletteBuf[kBonePaletteGlobalCap];
                        if (globalFeasible) {
                            for (int b = 0; b < cachedCount; ++b) {
                                D3DXMATRIX worldMat;
                                niTransformToD3D(cachedPalette[b], worldMat);
                                D3DXMatrixMultiply(
                                    &paletteBuf[b], &worldMat,
                                    &globalRs->viewTransform);
                            }
                        }

                        // ---- Phase 2: try to build merged VB+IB upfront ----
                        // Eligible when: multi-partition AND global path is
                        // feasible (we still need the indexed shader path for
                        // cross-partition bone references). On success: one
                        // color drawcall after the loop instead of N.
                        const MergedSkinEntry* merged = nullptr;
                        if (partCount > 1 && globalFeasible) {
                            merged = getMergedSkin(skinInstance);
                            if (!merged) ++skipMergeBuild;
                        }

                        // ---- 2c V2: defer single-partition global-path draws ----
                        // Cross-NPC batching via VTF (texture-sampled bone
                        // palette). Gated on Configuration.MeshLevelSkinningBatch
                        // — disabled by default; enable in ini to opt in.
                        // Multi-partition skins stay on Phase 2's merged inline
                        // path; positional-fallback skins stay inline too.
                        const bool deferForBatching = Configuration.MeshLevelSkinningBatch
                            && (partCount == 1) && globalFeasible && !merged;

                        for (unsigned int p = 0; p < partCount; ++p) {
                            ++partTotal;
                            const auto& part = partitions[p];
                            const auto* bd = reinterpret_cast<const EngineGeometryBufferData*>(part.bufferData);
                            if (!bd || !bd->d3dVertexBuffer) continue;

                            // SetSkinnedModelTransforms mutates the GLOBAL
                            // rs.worldViewTransforms[0..3] via proxy capture.
                            // That's the bone-matrix data renderMorrowind
                            // reads. We don't have a way around this; the
                            // global mutation here is matched by the next
                            // partition / next skin's overwrite.
                            if (!v19) {
                                kSetSkinnedModelTransforms(
                                    this_, skinInstance, &part, transform, worldBound);
                            }
                            if (!(binary & 0x4000000)) {
                                kApplyVertexShader(pipeline, bd->fvf);
                            }
                            if (!(binary & 0x1000000)) {
                                kBuildSkinPartitionPass(
                                    pipeline, propertyState, effectState,
                                    hasNormals, 1,
                                    geomData, skinInstance, &part, transform, worldBound);
                            }

                            // Snapshot global → local AFTER per-partition
                            // captures so worldViewTransforms reflect this
                            // partition's bones.
                            RenderedState localRs = *globalRs;
                            localRs.vbOffset  = 0;
                            localRs.vbStride  = bd->vertexStride;
                            localRs.ib        = static_cast<IDirect3DIndexBuffer9*>(bd->indexBuffer);
                            localRs.ibBase    = 0;
                            localRs.fvf       = bd->fvf;
                            localRs.primType  = static_cast<D3DPRIMITIVETYPE>(primitiveType);
                            localRs.baseIndex = 0;
                            localRs.minIndex  = 0;
                            localRs.vertCount = part.numVertices;
                            localRs.startIndex = 0;
                            localRs.primCount = bd->trianglesCount;
                            localRs.numWeights = part.numBonesPerVertex;
                            // Defaults — the positional path. Overridden below
                            // if we can use the global-palette (2b) path.
                            localRs.vb        = static_cast<IDirect3DVertexBuffer9*>(bd->d3dVertexBuffer);
                            localRs.usesGlobalPalette = false;
                            localRs.bonePaletteGlobal = nullptr;
                            localRs.bonePaletteGlobalCount = 0;

                            // Per-skin diagnostic accounting (reasons we
                            // would have skipped the global path on this
                            // partition — counted even when merged absorbs
                            // the color draw).
                            if (!cachedPalette || cachedCount <= 0) {
                                ++skipNoCachedPalette;
                            } else if (cachedCount > kBonePaletteGlobalCap) {
                                ++skipPaletteTooLarge;
                            }

                            // Per-partition synthesized VB. Needed for either
                            // path (per-partition color draw or recordMW push
                            // for depth/shadow).
                            DWORD    synthFvf    = 0;
                            unsigned synthStride = 0;
                            IDirect3DVertexBuffer9* synthVB = globalFeasible
                                ? getRemappedVertexBuffer(&part, &synthFvf, &synthStride)
                                : nullptr;
                            if (globalFeasible && !synthVB) ++skipNoRemappedVB;

                            // Bind synth VB into localRs for the recordMW
                            // push (so depth/shadow gets the normalised
                            // XYZB5|LASTBETA_UBYTE4 layout — positional
                            // skin() reads BLENDWEIGHT.xy/xyz/xyzw per
                            // numWeights and derivation reproduces the
                            // materialised implicit weight).
                            if (synthVB) {
                                localRs.vb       = synthVB;
                                localRs.fvf      = synthFvf;
                                localRs.vbStride = synthStride;
                            }

                            // ---- Per-partition color draw ----
                            // Skipped when merged: the post-loop merged draw
                            // covers ALL partitions in one call.
                            // Skipped when deferForBatching: the drain at
                            // onSceneEnd() issues one batched draw covering
                            // all same-Partition NPCs across this frame.
                            if (!merged && !deferForBatching) {
                                if (synthVB) {
                                    // Global indexed-palette path (transient
                                    // mutation — reset after draw so the
                                    // recordMW copy below doesn't carry
                                    // usesGlobalPalette into the depth shader).
                                    localRs.usesGlobalPalette       = true;
                                    localRs.bonePaletteGlobal       = paletteBuf;
                                    localRs.bonePaletteGlobalCount  = cachedCount;
                                    device->SetStreamSource(0, localRs.vb, 0, localRs.vbStride);
                                    device->SetIndices(localRs.ib);
                                    device->SetFVF(localRs.fvf);
                                    FixedFunctionShader::renderMorrowind(&localRs, frs, lrs);
                                    ++partSubmitted;
                                    ++partGlobalPalette;
                                    localRs.usesGlobalPalette       = false;
                                    localRs.bonePaletteGlobal       = nullptr;
                                    localRs.bonePaletteGlobalCount  = 0;
                                } else {
                                    // Positional fallback (iter2/3 path).
                                    // Used when global path is infeasible.
                                    device->SetStreamSource(0, localRs.vb, 0, localRs.vbStride);
                                    device->SetIndices(localRs.ib);
                                    device->SetFVF(localRs.fvf);
                                    FixedFunctionShader::renderMorrowind(&localRs, frs, lrs);
                                    ++partSubmitted;
                                    ++partPositionalFallback;
                                }
                            }

                            // Push into recordMW so MGE's depth + shadow
                            // passes pick up this skinned draw. Vanilla MW
                            // gets this for free via inspectIndexedPrimitive's
                            // intercept, but our takeover suppresses MW's
                            // DrawIndexedPrimitive so the intercept never
                            // fires. Even when the color pass is merged, the
                            // per-partition recordMW push is what keeps
                            // depth/shadow correct (those shaders use the
                            // positional palette via worldViewTransforms).
                            // Mirrors the gating logic in inspectIndexedPrimitive
                            // (distantland.cpp:951).
                            if (localRs.zWrite) {
                                const auto& stage0 = frs->stage[0];
                                const bool isDecal =
                                    stage0.texcoordIndex != 0
                                    && (stage0.colorArg1 == D3DTA_TEXTURE
                                        || stage0.colorArg2 == D3DTA_TEXTURE);
                                if (!isDecal) {
                                    DistantLand::recordMW.emplace_back(localRs);
                                    if (localRs.alphaFunc == D3DCMP_GREATER) {
                                        DistantLand::recordMW.back().alphaRef++;
                                    }
                                }
                            }
                        }

                        // ---- 2c: enqueue for cross-NPC drain ----
                        // Single-partition global-path skins skip the per-
                        // partition color draw and instead queue here. The
                        // drain at onSceneEnd() groups by Partition* and
                        // emits one batched DrawIndexedPrimitive per group
                        // via D3D9 hardware instancing.
                        if (deferForBatching) {
                            QueuedDraw q;
                            q.renderer            = this_;
                            q.geomData            = geomData;
                            q.skinInstance        = skinInstance;
                            q.transform           = transform;
                            q.worldBound          = worldBound;
                            q.primitiveType       = primitiveType;
                            q.unused1             = unused1;
                            q.unused2             = unused2;
                            q.cachedPaletteCount  = cachedCount;
                            q.buildPassFlags      = v15;
                            q.loadBinaryFlags     = binary;
                            g_drawQueue.push_back(q);
                        }

                        // ---- Phase 2: one merged color draw ----
                        // Issued once per skin after the partition loop, when
                        // merged was successfully built. Covers ALL partitions
                        // in a single DrawIndexedPrimitive via the indexed-
                        // blend HLSL path.
                        if (merged) {
                            RenderedState mergedRs = *globalRs;
                            mergedRs.vb         = merged->vb;
                            mergedRs.ib         = merged->ib;
                            mergedRs.vbStride   = merged->stride;
                            mergedRs.fvf        = merged->fvf;
                            mergedRs.vbOffset   = 0;
                            mergedRs.ibBase     = 0;
                            mergedRs.primType   = static_cast<D3DPRIMITIVETYPE>(primitiveType);
                            mergedRs.baseIndex  = 0;
                            mergedRs.minIndex   = 0;
                            mergedRs.vertCount  = merged->totalVertices;
                            mergedRs.startIndex = 0;
                            mergedRs.primCount  = merged->totalTriangles;
                            // numWeights=4 is safe for any source partition
                            // because Phase 1 materialised all 4 explicit
                            // weights — padded entries are 0 and contribute
                            // 0 to the position sum.
                            mergedRs.numWeights        = 4;
                            mergedRs.usesGlobalPalette = true;
                            mergedRs.bonePaletteGlobal = paletteBuf;
                            mergedRs.bonePaletteGlobalCount = cachedCount;

                            device->SetStreamSource(0, mergedRs.vb, 0, mergedRs.vbStride);
                            device->SetIndices(mergedRs.ib);
                            device->SetFVF(mergedRs.fvf);
                            FixedFunctionShader::renderMorrowind(&mergedRs, frs, lrs);
                            ++mergedDrawcalls;
                            collapsedParts = static_cast<int>(partCount);
                            // No recordMW push for the merged draw — depth/
                            // shadow coverage comes from the per-partition
                            // pushes inside the loop above.
                        }

                        kEnsureAndRegisterStreamables(
                            pipeline, propertyState, effectState,
                            hasNormals, 1,
                            geomData, skinInstance, transform, worldBound);
                    }
                }

                int slot = diagLeft.fetch_sub(1);
                if (slot > 0) {
                    const int totalColorDraws = partSubmitted + mergedDrawcalls;
                    LOG::logline(
                        "-- [2b-takeover] skin=%p partTotal=%d colorDraws=%d "
                        "(merged=%d collapsed=%d global=%d positional=%d "
                        "skipNoCache=%d skipTooLarge=%d skipNoVB=%d skipMergeBuild=%d)",
                        skinInstance, partTotal, totalColorDraws,
                        mergedDrawcalls, collapsedParts,
                        partGlobalPalette, partPositionalFallback,
                        skipNoCachedPalette, skipPaletteTooLarge, skipNoRemappedVB,
                        skipMergeBuild);
                }

                // Option-D — accumulate per-frame perf counters.
                ++g_perfFrame.takeoverInvocations;
                if (skinPartition && skinPartition->partitionCount == 1) {
                    ++g_perfFrame.singlePartitionSkins;
                } else if (skinPartition && skinPartition->partitionCount > 1) {
                    ++g_perfFrame.multiPartitionSkins;
                }
                g_perfFrame.positionalFallback += partPositionalFallback;
                g_perfFrame.colorDrawsSkinned  += partSubmitted + mergedDrawcalls;
                return;
            }

            // Hand off to vanilla — MW's own SetSkinnedModelTransforms +
            // SetTransform writes still happen, captured at the D3D9 proxy
            // by mged3d8device.cpp captureTransform.
            kVanillaDrawSkinnedPrimitive2(
                this_, primitiveType, geomData, skinInstance, transform,
                worldBound, unused1, unused2);
        }
    } // namespace

    void onFrameBegin() {
        // Lazy reserve on first call. Sized to comfortably hold the upper
        // end of skinned-shape counts per frame seen in heavy mod stacks.
        if (!g_paletteCacheReserved) {
            g_paletteCache.reserve(256);
            g_paletteCacheReserved = true;
        }
        g_paletteCache.clear();

        // Option-D — start of frame counters (frame TIME is measured
        // separately via onPresent's Present-to-Present delta).
        g_perfFrame.rawDrawcallsAtStart = g_rawDrawcalls.load(std::memory_order_relaxed);
        g_perfFrame.takeoverInvocations  = 0;
        g_perfFrame.singlePartitionSkins = 0;
        g_perfFrame.multiPartitionSkins  = 0;
        g_perfFrame.positionalFallback   = 0;
        g_perfFrame.colorDrawsSkinned    = 0;

        // 2c — also clear the deferred-draw queue defensively. onSceneEnd()
        // drains it normally, but if a frame ended without renderStage1
        // firing (loading screens, menus, scene transitions), queue entries
        // would carry over and reference NiSkinInstance/SkinData pointers
        // that may have been freed by save reload — leading to access
        // violations at drain time (crash root cause: skinneddraw.cpp:1540
        // dereferencing stale sp->partitionCount after a save load).
        // Per-frame clear is the right gate: any entry not drained in its
        // own frame is by definition unsafe to consume.
        g_drawQueue.clear();
    }

    void onSceneEnd() {
        // Drain only runs when there's a queue (i.e. 2c batching enabled
        // AND eligible draws were deferred this frame). We can't early-
        // return on empty queue though — the Option-D perf log lives at
        // the BOTTOM of this function and needs to accumulate every frame
        // regardless of batching state. So drain in a gated block, then
        // always fall through to perf accumulation + reporting.
        if (!g_drawQueue.empty()) {
        // Group by partition0* (single-partition skins only — V1 scope).
        // Same Partition* ⇒ same NIF source ⇒ batch-compatible (verified
        // empirically via the [2b-archetype] diagnostic; same skinData/
        // partition/srcVB repeat across distinct skinInstance pointers).
        std::unordered_map<const NI::SkinPartition::Partition*,
                           std::vector<QueuedDraw*>> groups;
        groups.reserve(64);
        for (auto& d : g_drawQueue) {
            if (!d.skinInstance) continue;
            // Stale-entry guard: paletteCache is per-frame cleared. If
            // d.skinInstance isn't present, this queue entry survived from
            // a previous frame (drain skipped, e.g. load-screen transition)
            // and its NiSkinInstance/SkinData pointers may have been freed
            // by save reload. count() on a stale pointer is safe (just
            // hashes the integer), but dereferencing it isn't — bail before
            // touching d.skinInstance->skinData.
            if (!g_paletteCache.count(d.skinInstance)) continue;
            const NI::SkinData* sd = d.skinInstance->skinData;
            const NI::SkinPartition* sp = sd
                ? static_cast<const NI::SkinPartition*>(sd->partition)
                : nullptr;
            if (!sp || sp->partitionCount == 0) continue;
            groups[&sp->partitions[0]].push_back(&d);
        }

        int batchesIssued    = 0;
        int instancesCovered = 0;
        int singletonsDrained = 0;
        int oversized        = 0;

        for (auto& kv : groups) {
            auto& members = kv.second;
            const QueuedDraw& rep = *members[0];
            const int N = static_cast<int>(members.size());

            // Even for singletons we go through the batched path (with
            // batchInstances=1). Keeps the code path uniform; the shader
            // sees baseBoneOffset=0 and the math reduces to the single-NPC
            // case. SetStreamSourceFreq(0, INDEXEDDATA|1) is a no-op.
            if (static_cast<unsigned>(N * rep.cachedPaletteCount) > FixedFunctionShader::getMaxBonesInPaletteTex()) {
                ++oversized;
                // TODO: split-batching. For V2, leave the group rendered
                // only via the recordMW depth/shadow path (color won't
                // appear). Logged so we can size the future split logic.
                continue;
            }

            renderBatchedGroup(kv.first, members,
                batchesIssued, instancesCovered);
            if (N == 1) ++singletonsDrained;
        }

        int slot = g_drainLogsLeft.fetch_sub(1);
        if (slot > 0) {
            LOG::logline(
                "-- [2c-drain] queued=%u groups=%u batches=%d instances=%d "
                "singletons=%d oversized=%d",
                static_cast<unsigned>(g_drawQueue.size()),
                static_cast<unsigned>(groups.size()),
                batchesIssued, instancesCovered,
                singletonsDrained, oversized);
        }

        g_drawQueue.clear();
        } // end of `if (!g_drawQueue.empty())` drain gate

        // -------- Option-D — end-of-frame counter snapshot --------
        // Frame TIME is measured Present-to-Present in onPresent(); we
        // only accumulate counts here. The actual log line is emitted
        // from onPresent once kPerfReportFrames have elapsed.
        const int rawDrawcallsThisFrame =
            g_rawDrawcalls.load(std::memory_order_relaxed)
            - g_perfFrame.rawDrawcallsAtStart;

        g_perfAccum.totalRawDrawcalls       += rawDrawcallsThisFrame;
        g_perfAccum.totalTakeoverInvocations += g_perfFrame.takeoverInvocations;
        g_perfAccum.totalSinglePartition    += g_perfFrame.singlePartitionSkins;
        g_perfAccum.totalMultiPartition     += g_perfFrame.multiPartitionSkins;
        g_perfAccum.totalPositionalFallback += g_perfFrame.positionalFallback;
        g_perfAccum.totalColorDrawsSkinned  += g_perfFrame.colorDrawsSkinned;
    }

    void onPresent() {
        if (!g_perfQpcFreqInit) {
            QueryPerformanceFrequency(&g_perfQpcFreq);
            g_perfQpcFreqInit = true;
        }
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);

        if (g_lastPresentValid) {
            const long long deltaTicks = now.QuadPart - g_lastPresentTicks.QuadPart;
            g_perfAccum.frames                += 1;
            g_perfAccum.totalPresentDeltaTicks += deltaTicks;

            if (g_perfAccum.frames >= kPerfReportFrames && g_perfQpcFreq.QuadPart > 0) {
                const double avgFrameMs =
                    1000.0 * (double)g_perfAccum.totalPresentDeltaTicks
                    / ((double)g_perfQpcFreq.QuadPart * g_perfAccum.frames);
                const double avgFps = (avgFrameMs > 0.0) ? 1000.0 / avgFrameMs : 0.0;
                const double avgRawDrawcalls =
                    (double)g_perfAccum.totalRawDrawcalls / g_perfAccum.frames;
                const double avgSkinnedDraws =
                    (double)g_perfAccum.totalColorDrawsSkinned / g_perfAccum.frames;
                const double avgTakeovers =
                    (double)g_perfAccum.totalTakeoverInvocations / g_perfAccum.frames;
                LOG::logline(
                    "-- [perf] frames=%d frameTime=%.2fms (%.0ffps) "
                    "proxyDraws=%.0f skinnedTakeoverDraws=%.0f takeovers=%.1f "
                    "single=%lld multi=%lld posFallback=%lld "
                    "(NOTE: proxyDraws excludes MGE's raw-device draws and our takeover; treat as MW-only subset)",
                    g_perfAccum.frames, avgFrameMs, avgFps,
                    avgRawDrawcalls, avgSkinnedDraws, avgTakeovers,
                    g_perfAccum.totalSinglePartition,
                    g_perfAccum.totalMultiPartition,
                    g_perfAccum.totalPositionalFallback);
                g_perfAccum = {};
            }
        }
        g_lastPresentTicks = now;
        g_lastPresentValid = true;
    }

    const NI::Transform* getCachedPalette(
        const NI::SkinInstance* skinInstance,
        int* outNumBones)
    {
        auto it = g_paletteCache.find(skinInstance);
        if (it == g_paletteCache.end()) {
            if (outNumBones) *outNumBones = 0;
            return nullptr;
        }
        if (outNumBones) *outNumBones = it->second.numBones;
        return it->second.matrices;
    }

    void installSmokeTestHook() {
        if (g_hookInstalled) {
            return;
        }

        // se::memory::genCallEnforced verifies the existing instruction is a
        // CALL (0xE8) AND that its current target matches `previousTo` before
        // overwriting. If the MW binary ever shifts (mod, patch, different
        // version) the assertion fails and the hook silently doesn't install
        // instead of corrupting random bytes.
        const bool ok = se::memory::genCallEnforced(
            kCallSiteAddr,
            /*previousTo*/ 0x6AEF90,
            reinterpret_cast<DWORD>(&observerDrawSkinnedPrimitive2));

        if (!ok) {
            LOG::logline(
                "!! [2b-smoke] genCallEnforced failed at 0x%X; the CALL at "
                "that address no longer points at NiDX8Renderer::"
                "DrawSkinnedPrimitive2 (0x6AEF90). Hook NOT installed.",
                kCallSiteAddr);
            return;
        }

        g_hookInstalled = true;
        g_smokeLogsLeft.store(kMaxSmokeLogs);
        LOG::logline(
            "-- [2b-smoke] DrawSkinnedPrimitive2 observer installed; "
            "logging first %d invocations.",
            kMaxSmokeLogs);
    }

} // namespace MGE::SkinnedDraw
