#version 450
layout(location = 0) out highp vec2 vUV;

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
	if (gl_VertexIndex == 0)
		gl_Position = vec4(-1.0, -1.0, 0.0, 1.0);
	else if (gl_VertexIndex == 1)
		gl_Position = vec4(+3.0, -1.0, 0.0, 1.0);
	else
		gl_Position = vec4(-1.0, +3.0, 0.0, 1.0);
    vUV = 0.5 * gl_Position.xy + 0.5;

	prerotate_fixup_clip_xy();
}
