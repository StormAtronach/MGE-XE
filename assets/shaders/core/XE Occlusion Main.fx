#include "XE Common.fx"

#include "XE Mod Landscape.fx"

float4 StaticOcclusionVS(float4 pos : POSITION) : POSITION {
    return mul(mul(mul(pos, world), view), proj);
}

float4 StaticOcclusionPS(float4 pos : POSITION) : COLOR0 {
    return float4(1.0, 1.0, 1.0, 1.0);
}

float4 LandscapeOcclusionPS(LandVertOut IN) : COLOR0 {
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

        VertexShader = compile vs_3_0 LandscapeVS();
        PixelShader = compile ps_3_0 LandscapeOcclusionPS();
    }

    Pass Static {
        ZEnable = true;
        ZWriteEnable = true;
        ZFunc = LessEqual;
        CullMode = CW;

        AlphaBlendEnable = false;
        AlphaTestEnable = false;

        VertexShader = compile vs_3_0 StaticOcclusionVS();
        PixelShader = compile ps_3_0 StaticOcclusionPS();
    }
}