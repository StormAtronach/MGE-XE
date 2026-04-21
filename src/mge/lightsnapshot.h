#pragma once

#include "proxydx/d3d8header.h"

#include <vector>



// Per-frame snapshot of point lights that Morrowind's NiDX8LightManager
// iterates over during NiDX8LightManager::updateLights (Morrowind.exe @
// 0x6BB6C0). Gives MGE-XE a read-only view of the live renderer-iterated
// light list without having to re-patch the same 6-byte hook site used
// by MWSE's MSOC occlusion culler.
//
// Wiring:
//   1. Call InstallLightSnapshotHook() once during startup, after MWSE.dll
//      has loaded. It prefers the MWSE-exported callback registry at
//      `mwse_registerLightObservedCallback`; if that export is missing
//      (MWSE older than this feature, or MWSE not present), it falls
//      back to a direct VirtualProtect patch at 0x6BB7D4.
//   2. Call BeginFrame() at the start of each render frame (e.g. from
//      MGED3D8Device::BeginScene). This swaps the double buffer so the
//      next updateLights pass writes into a fresh buffer while the
//      previous frame's snapshot remains readable via GetSnapshot().
//   3. Read GetSnapshot() any time between BeginFrame() calls — the
//      vector is stable for the duration of the frame.
//
// Ordering:
//   The observer fires before Morrowind's own enabled check, so the
//   `enabled` field in each LightSnapshot is whatever the engine had at
//   the moment of iteration (0 after a detach, 1 otherwise). Treat
//   enabled == 0 as "skip" for shadow/effect purposes.

namespace MGE {

    struct LightSnapshot {
        void* niLight;          // pointer only — do not dereference after the frame ends
        float position[3];      // worldTransform.translation
        float radius;           // specular.r (engine overloads specular.r as fade radius)
        float ambient[3];       // ambient RGB
        float diffuse[3];       // diffuse RGB
        float attenConst;
        float attenLinear;
        float attenQuadratic;
        unsigned char enabled;  // +0x90; 0 if engine disabled this light pre-submit
    };

    // Install the observer (idempotent). Returns true if installed via the
    // MWSE callback registry, false if installed via the fallback patch.
    // Returns false and logs on hard failure.
    bool InstallLightSnapshotHook();

    // Swap write/read buffers. Call once per frame before the renderer
    // iterates lights.
    void BeginFrame();

    // Read-only view of the previous frame's snapshot. Stable until the
    // next BeginFrame() call. Safe to call from any render-thread code.
    const std::vector<LightSnapshot>& GetSnapshot();

}
