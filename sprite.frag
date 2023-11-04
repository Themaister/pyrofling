#version 450
precision highp float;
precision highp int;

layout(location = 0) out mediump vec4 Color;

layout(location = 0) flat in mediump vec4 vColor;

#if defined(HAVE_BASECOLORMAP) && HAVE_BASECOLORMAP
layout(location = 1) in highp vec2 vTex;
layout(set = 2, binding = 0) uniform mediump sampler2D uTex;
#endif

void main()
{
#if defined(HAVE_BASECOLORMAP) && HAVE_BASECOLORMAP
    mediump vec4 color = texture(uTex, vTex);
    #if defined(VARIANT_BIT_4) && VARIANT_BIT_4
        color = vec4(1.0, 1.0, 1.0, color.r);
    #endif
#else
    mediump vec4 color = vec4(1.0);
#endif

    color *= vColor;
	Color = color;
}
