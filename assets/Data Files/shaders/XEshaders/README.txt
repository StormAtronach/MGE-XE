This directory contains post-processing shader mods.


Creating new shaders
--------------------

New shaders should have a single technique tagged with an MGEinterface annotation like this:

technique T0 < string MGEinterface = "MGE XE 0"; >


Shader ordering
---------------

Shaders (since v0.14) are auto-sorted by category when added to the active shader list. This allows adding shaders without having to follow an ordering guide, and also adding shaders at runtime without manual intervention. Existing shaders without a category stay in their assigned place.

The category is specified by name in the shader annotation. 

Category annotation example from a bloom shader:
technique T0 < string MGEinterface = "MGE XE 0"; string category = "sensor"; >


The category sort ordering is:
         scene - Additional objects rendered into the scene.
	atmosphere - Atmosphere effects that render over all objects, such as mist.
	      lens - Lens effects such as depth of field.
	    sensor - Sensor effects such as bloom.
	      tone - Tone mapping and color grading.
	     final - Any effects that need to run last.


Tuning relative ordering within a category is also possible with priorityAdjust.

An example from a SSAO shader, to avoid applying darkening on top of any other shader:
technique T0 < string MGEinterface = "MGE XE 0"; string category = "scene"; int priorityAdjust = -10000; >

Using these in your shaders is recommended to improve ease of use.


Shader scripting
----------------

Shaders can be loaded at runtime and have their variables modified using MWSE.

See MWSE docs:
https://mwse.github.io/MWSE/apis/mge.shaders/
https://mwse.github.io/MWSE/types/mgeShaderHandle/


Temporal access (previous frame)
--------------------------------

Two variables are bound automatically for shaders that want to read from the
previous frame -- enabling temporal effects such as TAA, motion blur, temporal
reprojection, ghosting/trails, and frame-to-frame smoothing.

    texture prevframe;        // fully post-processed output of frame N-1
    float4x4 prevviewproj;    // view * proj matrix used to render frame N-1

Both are filled by the engine; declaring them in your shader is enough -- no
extra setup is needed. On the first frame after load (or after device reset),
`prevframe` is opaque black and `prevviewproj` is the identity matrix, so any
temporal effect should ramp in over a few frames rather than blend at full
weight on frame 1.

`prevframe` is the full result of the post-process chain from the previous
frame -- the same image that was sent to the back buffer. It is sampled the
same way as `lastshader`. Note: it is LDR (A8R8G8B8), matching the rest of
the chain.

`prevviewproj` is the camera transform from the previous frame. Combined with
the current frame's `mview` * `mproj` and a depth sample, it lets you compute
where a pixel was on the previous frame's screen, which is what you need for
reprojection-based effects.


Example: simple TAA-style blend
-------------------------------

    texture lastshader, prevframe, depthframe;
    float4x4 mview, mproj, prevviewproj;
    float2 rcpres;

    sampler sCurr = sampler_state { texture = <lastshader>; addressu = clamp; addressv = clamp; magfilter = linear; minfilter = linear; };
    sampler sPrev = sampler_state { texture = <prevframe>;  addressu = clamp; addressv = clamp; magfilter = linear; minfilter = linear; };
    sampler sDepth = sampler_state { texture = <depthframe>; addressu = clamp; addressv = clamp; magfilter = point;  minfilter = point; };

    float4 taa(in float2 tex : TEXCOORD) : COLOR0
    {
        // Reconstruct world-space position from depth
        float depth = tex2Dlod(sDepth, float4(tex, 0, 0)).r;
        float4 ndc = float4(tex.x * 2 - 1, 1 - tex.y * 2, depth, 1);
        float4x4 invVP = ...; // current inverse(view*proj), supply via your own var
        float4 worldPos = mul(ndc, invVP);
        worldPos /= worldPos.w;

        // Project into the previous frame
        float4 prevClip = mul(worldPos, prevviewproj);
        float2 prevTex = float2(prevClip.x, -prevClip.y) / prevClip.w * 0.5 + 0.5;

        float4 curr = tex2D(sCurr, tex);
        float4 prev = tex2D(sPrev, prevTex);

        // Reject samples that fell off-screen
        float valid = all(saturate(prevTex) == prevTex) ? 1 : 0;
        return lerp(curr, 0.9 * prev + 0.1 * curr, valid);
    }

(Computing inverse(view*proj) is left to the shader author -- pass it in as
your own uniform, or invert `mul(mview, mproj)` on the GPU once per pass.)


Cost
----

Adding `prevframe` to a shader costs:
  * One extra full-resolution texture sample per pass that reads it.
  * One full-resolution StretchRect copy per frame, paid once regardless of
    how many shaders use it.
  * One backbuffer-sized A8R8G8B8 texture in VRAM (~32 MB at 4K).

Adding `prevviewproj` is free -- it is a single uniform.
