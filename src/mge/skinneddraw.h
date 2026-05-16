#pragma once

#include <atomic>

// 2b — per-mesh bone palette path. Builds a per-skinInstance bone palette
// indexed by GLOBAL bone index (as opposed to MW's per-partition local-indexed
// palette, which is capped at the D3D9 FFP's 4 WORLD matrix slots).
//
// Phase 1: standalone builder, no patch sites installed. Validates the math
// and offsets structurally at compile time via SharedSE's static_assert chain.
// Phase 2 (smoke test) adds an observer hook to compare against the engine's
// own SetSkinnedModelTransforms output. Phase 3 replaces MW's draw.
//
// Header forward-declares NI types so non-SharedSE consumers (e.g.
// mwinitpatch.cpp) can call installSmokeTestHook() without pulling the
// SharedSE prelude through their TU.

namespace NI {
    struct SkinInstance;
    struct Transform;
}

namespace MGE::SkinnedDraw {

    // Per-mesh palette cap. SM3 const-register budget is 256 vec4 registers;
    // a Transform stored as mat4 costs 4. Reserve ~16 slots for other uniforms
    // and we land at 60 bones / mesh, which covers all vanilla content.
    constexpr int kMaxPaletteSize = 60;

    // Composes the per-mesh bone palette for `skinInstance`. Each output entry
    // is the composite Transform a vertex weighted by bone `b` should multiply
    // its rest-pose position by.
    //
    // Mirrors NiDX8Renderer::SetSkinnedModelTransforms (@ 0x6ACBE0) bit-for-bit
    // (modulo float-op associativity); the only difference is indexing —
    // outPalette[b] is keyed by GLOBAL bone index (skinInstance->bones[b]),
    // not by per-partition local index.
    //
    // `outPalette` must have room for `skinInstance->skinData->numBones`
    // entries. Returns the number of palette entries written, or 0 on any
    // failure (null/missing pointer, palette overflow, non-invertible root
    // transform, missing bone).
    int buildBonePalette(
        const NI::SkinInstance* skinInstance,
        const NI::Transform& meshTransform,
        NI::Transform* outPalette);

    // Phase 2 smoke-test observer hook.
    //
    // Installs a CALL-site patch at 0x6AE00F (the sole xref to
    // NiDX8Renderer::DrawSkinnedPrimitive2 @ 0x6AEF90) that redirects the
    // call into MGE. The hook builds buildBonePalette() for every skinned
    // draw, caches it per-frame keyed by NiSkinInstance*, and tail-calls
    // the vanilla engine function so MW's per-partition
    // SetTransform(D3DTS_WORLDMATRIXn) writes still happen unchanged.
    //
    // Pure observation — no behavior change to MW's draw path. Gated by
    // Configuration.MeshLevelSkinningSmokeTest. Safe to call repeatedly;
    // subsequent calls are no-ops.
    void installSmokeTestHook();

    // Per-frame cache hook. Call once per frame (from DistantLand::
    // renderStage0) to clear the previous frame's palette cache and bump
    // the internal frame counter. Cheap no-op if no skinned draws have
    // been observed; safe to call unconditionally regardless of hook
    // install state.
    void onFrameBegin();

    // Look up a cached palette for `skinInstance` populated this frame.
    // Returns nullptr if no observer hook fired for this instance in the
    // current frame. The returned pointer remains valid until the next
    // onFrameBegin() call. `outNumBones` (if non-null) receives the bone
    // count written when the cache entry was populated.
    const NI::Transform* getCachedPalette(
        const NI::SkinInstance* skinInstance,
        int* outNumBones);

    // 2b.3b VB rewriter and 2b.3c draw replacement live inside skinneddraw.cpp.
    // No public surface needed — the handler dispatches from the observer hook
    // when Configuration.MGEMeshLevelSkinning is on.

    // 2c — drain the deferred-skinned-draw queue. Call once per scene after
    // all of MW's DrawSkinnedPrimitive2 calls have fired and BEFORE MGE's
    // depth pre-pass (i.e. at the start of DistantLand::renderStage1). When
    // 2c batching is active, color draws are deferred from the observer to
    // this drain so that same-Partition NPCs can be grouped and emitted via
    // D3D9 hardware instancing in a single drawcall per group.
    //
    // Per-partition recordMW pushes for depth/shadow already happened at
    // observer time — this drain handles only the COLOR pass.
    //
    // No-op when the queue is empty (i.e. when MeshLevelSkinning is off,
    // or no skinned draws fired this scene).
    void onSceneEnd();

    // Option-D instrumentation — incremented by mged3d8device.cpp's
    // DrawIndexedPrimitive proxy on every drawcall (skinned or otherwise),
    // read + reset by skinneddraw.cpp's per-frame perf log. The atomic is
    // process-local and relaxed (single-threaded MW render loop in
    // practice, but defensive against accidental cross-thread use).
    //
    // Caveat: this counts only proxy-routed draws (MW's own). MGE's
    // distant-land / grass / shadow / post-effect pipeline uses the raw
    // D3D9 device and bypasses this counter, as does our 2c color
    // takeover (renderMorrowind via raw device). So this number is a
    // SUBSET of true frame drawcalls.
    extern std::atomic<int> g_rawDrawcalls;

    // Called by mged3d8device.cpp's Present hook. Measures true frame-to-
    // frame wall time at the right boundary (Present → Present), which is
    // what frame-rate counters report. The previous "frameStart at
    // onFrameBegin to frameEnd at onSceneEnd" measurement only covered
    // MW's scene-render slice and undercounted real frame time by ~3x in
    // dense scenes.
    void onPresent();

} // namespace MGE::SkinnedDraw
