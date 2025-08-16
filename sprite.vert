#version 450

layout(location = 0) in mediump vec2 QuadCoord;
layout(location = 1) in vec4 PosOffsetScale;
layout(location = 2) in vec4 TexOffsetScale;
layout(location = 3) in mediump vec4 Rotation;
layout(location = 4) in mediump vec4 Color;
layout(location = 5) in mediump float Layer;

layout(std140, set = 0, binding = 0) uniform Scene
{
    vec3 inv_resolution;
    vec3 pos_offset_pixels;
};

layout(location = 0) flat out mediump vec4 vColor;

#if defined(HAVE_UV) && HAVE_UV
    layout(location = 1) out highp vec2 vTex;
    layout(set = 3, binding = 0, std140) uniform Globals
    {
        vec2 tex_resolution;
        vec2 inv_tex_resolution;
    } constants;
#endif

layout(constant_id =  8) const float PREROTATE_MATRIX_0 = 1.0;
layout(constant_id =  9) const float PREROTATE_MATRIX_1 = 0.0;
layout(constant_id = 10) const float PREROTATE_MATRIX_2 = 0.0;
layout(constant_id = 11) const float PREROTATE_MATRIX_3 = 1.0;

void prerotate_fixup_clip_xy()
{
	gl_Position.xy =
			mat2(PREROTATE_MATRIX_0, PREROTATE_MATRIX_1,
			     PREROTATE_MATRIX_2, PREROTATE_MATRIX_3) *
			     gl_Position.xy;
}


void main()
{
    vec2 QuadPos = (mat2(Rotation.xy, Rotation.zw) * QuadCoord) * 0.5 + 0.5;
    vec2 pos_pixels = QuadPos * PosOffsetScale.zw + PosOffsetScale.xy;
    vec2 pos_ndc = (pos_pixels + pos_offset_pixels.xy) * inv_resolution.xy;
    gl_Position = vec4(2.0 * pos_ndc - 1.0, (Layer + pos_offset_pixels.z) * inv_resolution.z, 1.0);

#if defined(HAVE_UV) && HAVE_UV
    vec2 uv = ((QuadCoord * 0.5 + 0.5) * TexOffsetScale.zw + TexOffsetScale.xy) * constants.inv_tex_resolution;
    vTex = uv;
#endif

    vColor = Color;
	prerotate_fixup_clip_xy();
}
