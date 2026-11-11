


#version 450
#extension GL_EXT_samplerless_texture_functions : require

layout(set = 0, binding = 0) uniform texture2D source;

layout(push_constant) uniform Constants {
  layout(offset = 16) ivec2 displacement;
  float scale;
  uint mode;
} c;

layout(location = 0) out vec4 color;

void main() {
  vec4 v = texelFetch(source, ivec2(gl_FragCoord.xy) + c.displacement, 0) * c.scale;
  if (c.mode == 0u) {
    color = clamp(v, 0.0, 1.0);
  } else if (c.mode == 1u) {
    color = clamp(v, -1.0, 1.0);
  } else if (c.mode == 2u) {
    color = clamp(round(v), 0.0, 65535.0) / 65535.0;
  } else if (c.mode == 3u) {
    color = clamp(round(v), -32767.0, 32767.0) / 32767.0;
  } else {
    color = v;
  }
}
