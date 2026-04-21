#include "lightsnapshot.h"
#include "support/log.h"

#include <windows.h>
#include <cstring>



namespace MGE {

    // NiLight memory layout (verified against the renderer's use in
    // updateLights @ 0x6BB6C0). All offsets are from the NiLight base.
    static const size_t kOffEnabled   = 0x90;
    static const size_t kOffPosition  = 0x64;  // worldTransform.translation (3 x float)
    static const size_t kOffAmbient   = 0xAC;  // RGBA; we only take RGB
    static const size_t kOffDiffuse   = 0xB8;
    static const size_t kOffSpecular  = 0xC4;  // specular.r overloaded as fade radius
    static const size_t kOffAttenK    = 0xD0;  // PointLight only; safe to read as the
    static const size_t kOffAttenL    = 0xD4;  // hook site iterates point lights.
    static const size_t kOffAttenQ    = 0xD8;

    // Hook site inside NiDX8LightManager::updateLights — the `mov al, [ebx+0x90]`
    // that reads the enabled flag. Same address used by MWSE's MSOC cull patch;
    // we prefer to register with MWSE's shared callback registry and only fall
    // back to patching this site directly if the MWSE export is missing.
    static const DWORD kHookSite = 0x6BB7D4;

    // Double buffer: the render thread appends to g_writeBuf during
    // updateLights iteration; BeginFrame() swaps it with g_readBuf. Callers
    // of GetSnapshot() always read the prior frame's buffer.
    static std::vector<LightSnapshot> g_writeBuf;
    static std::vector<LightSnapshot> g_readBuf;

    // Per-frame dedup. The renderer may walk the same NiLight more than once
    // per frame (root-node re-entrancy); we only want one snapshot entry per
    // pointer per frame. Linear scan over a vector of pointers beats
    // unordered_set for N <= ~50, which is the Morrowind ceiling.
    static std::vector<void*> g_seenThisFrame;

    static bool g_installed = false;


    //---------------------------------------------------------------------
    // Capture one light. Called once per iteration of updateLights, once per
    // unique NiLight* per frame. Must be cheap — no allocation on the
    // steady path (vector reserves below amortize).

    static inline void captureOne(void* niLight) {
        if (!niLight) {
            return;
        }

        // Dedup
        for (void* p : g_seenThisFrame) {
            if (p == niLight) {
                return;
            }
        }
        g_seenThisFrame.push_back(niLight);

        LightSnapshot s;
        s.niLight = niLight;

        const unsigned char* base = static_cast<const unsigned char*>(niLight);
        s.enabled = *(base + kOffEnabled);

        std::memcpy(s.position, base + kOffPosition, sizeof(float) * 3);
        std::memcpy(s.ambient,  base + kOffAmbient,  sizeof(float) * 3);
        std::memcpy(s.diffuse,  base + kOffDiffuse,  sizeof(float) * 3);

        s.radius        = *reinterpret_cast<const float*>(base + kOffSpecular);
        s.attenConst    = *reinterpret_cast<const float*>(base + kOffAttenK);
        s.attenLinear   = *reinterpret_cast<const float*>(base + kOffAttenL);
        s.attenQuadratic = *reinterpret_cast<const float*>(base + kOffAttenQ);

        g_writeBuf.push_back(s);
    }


    //---------------------------------------------------------------------
    // Observer callback — registered with MWSE's callback registry. Fires
    // once per iteration of updateLights before the engine's enabled check.
    // MWSE passes the NiLight* as void* to keep the ABI stable.

    extern "C" static void __cdecl LightObserver(void* niLight) {
        captureOne(niLight);
    }


    //---------------------------------------------------------------------
    // Solo-hook fallback. Only used if MWSE.dll does not export
    // mwse_registerLightObservedCallback (MWSE absent, or older build).
    //
    // At 0x6BB7D4 the original instruction is `mov al, [ebx+0x90]` (6 bytes:
    // 8A 83 90 00 00 00). ebx points to the NiLight currently being iterated.
    // We overwrite the 6 bytes with a 5-byte call + 1-byte NOP that jumps to
    // this thunk, which saves flags/regs, forwards ebx to captureOne, then
    // re-executes the replaced read and returns.

    static unsigned char g_origBytes[6];

    static __declspec(naked) void LightSnapshotThunk() {
        __asm {
            pushad
            pushfd
            push ebx              // niLight*
            call captureOne
            add esp, 4
            popfd
            popad
            mov al, [ebx + 0x90]  // replay the replaced read
            ret
        }
    }

    static bool installFallbackPatch() {
        void* site = reinterpret_cast<void*>(kHookSite);
        DWORD oldProtect = 0;
        if (!VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            LOG::logline("!! MGE light-snapshot: VirtualProtect failed at 0x%08X", kHookSite);
            return false;
        }

        std::memcpy(g_origBytes, site, 6);

        // E8 rel32 call, then a 1-byte NOP.
        unsigned char* p = static_cast<unsigned char*>(site);
        DWORD rel = reinterpret_cast<DWORD>(&LightSnapshotThunk) - (kHookSite + 5);
        p[0] = 0xE8;
        p[1] = static_cast<unsigned char>(rel & 0xFF);
        p[2] = static_cast<unsigned char>((rel >> 8) & 0xFF);
        p[3] = static_cast<unsigned char>((rel >> 16) & 0xFF);
        p[4] = static_cast<unsigned char>((rel >> 24) & 0xFF);
        p[5] = 0x90;

        DWORD dummy;
        VirtualProtect(site, 6, oldProtect, &dummy);
        return true;
    }


    //---------------------------------------------------------------------
    // Public API

    bool InstallLightSnapshotHook() {
        if (g_installed) {
            return true;
        }

        g_writeBuf.reserve(64);
        g_readBuf.reserve(64);
        g_seenThisFrame.reserve(64);

        // Prefer MWSE's shared registry so we don't fight over the hook site.
        typedef void (__cdecl* RegFn)(void (__cdecl*)(void*));
        HMODULE mwse = GetModuleHandleA("MWSE.dll");
        if (mwse) {
            RegFn reg = reinterpret_cast<RegFn>(
                GetProcAddress(mwse, "mwse_registerLightObservedCallback"));
            if (reg) {
                reg(&LightObserver);
                g_installed = true;
                LOG::logline("-- MGE light-snapshot: registered via MWSE callback");
                return true;
            }
        }

        // Fallback: own the hook ourselves.
        if (!installFallbackPatch()) {
            return false;
        }
        g_installed = true;
        LOG::logline("-- MGE light-snapshot: installed fallback patch at 0x%08X", kHookSite);
        return false;
    }

    void BeginFrame() {
        g_readBuf.swap(g_writeBuf);
        g_writeBuf.clear();
        g_seenThisFrame.clear();
    }

    const std::vector<LightSnapshot>& GetSnapshot() {
        return g_readBuf;
    }

}
