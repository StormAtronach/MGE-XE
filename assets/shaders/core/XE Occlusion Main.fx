#include "XE Common.fx"

#include "XE Mod Landscape.fx"
#include "XE Mod Statics.fx"

float4 StaticOcclusionVS(StatVertIn IN) : POSITION {
    TransformedVert v = transformStaticVert(IN);
    return v.pos;
}

float4 OcclusionPS(LandVertOut IN) : COLOR0 {
    return float4(1.0, 1.0, 1.0, 1.0);
}

Technique Occlusion {
    Pass Landscape {
        ZEnable = true;
        ZWriteEnable = true;
        ZFunc = LessEqual;
        CullMode = CW;

        AlphaBlendEnable = false;
        AlphaTestEnable = false;

        VertexShader = compile vs_3_0 StaticOcclusionVS();
        PixelShader = compile ps_3_0 OcclusionPS();
    }

    Pass Static {
        ZEnable = true;
        ZWriteEnable = true;
        ZFunc = LessEqual;
        CullMode = CW;

        AlphaBlendEnable = false;
        AlphaTestEnable = false;

        VertexShader = compile vs_3_0 StaticExteriorVS();
        PixelShader = compile ps_3_0 OcclusionPS();
    }
}