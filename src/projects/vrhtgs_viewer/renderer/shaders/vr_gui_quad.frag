#version 450

layout(location = 0) out vec4 out_color;

layout(binding = 0) uniform sampler2D sourceTex;

uniform bool flip = false;

in vec4 texcoord;

void main(void)
{
    vec2 uv = texcoord.xy;
    if (flip) uv.y = 1.0 - uv.y;
    vec4 tex = texture(sourceTex, uv);
    out_color = vec4(tex.rgb, 1.0);
}
