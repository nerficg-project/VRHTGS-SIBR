#version 450

layout(location = 0) in vec4 in_vertex;
layout(location = 1) in vec4 in_texcoord;
layout(location = 2) in vec4 in_color;

uniform float x_offset;
uniform float y_offset;
uniform float scale_x;
uniform float scale_y;
uniform float dist_from_viewer;
uniform mat4 head_c2w;
uniform mat4 eye_mvp;

out vec4 texcoord;
out vec4 color;

void main(void) {
  vec4 vertex_pos = in_vertex;
  vertex_pos.x = (vertex_pos.x + x_offset) * scale_x;
  vertex_pos.y = (vertex_pos.y + y_offset) * scale_y;
  vertex_pos.z = -dist_from_viewer;
  vertex_pos.w = 1.0f;
  gl_Position = eye_mvp * head_c2w * vertex_pos;

  texcoord    = in_texcoord;
  color       = in_color;
}
