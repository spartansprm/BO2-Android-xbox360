#version 450
// compare_pixels.cpp: a full-screen triangle with 16 float4 interpolators that vary across the screen, for the
// pixel shaders under test (their TEXCOORD inputs are at locations 0-15).
layout(location = 0) out vec4 o[16];
void main() {
  vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
  for (int i = 0; i < 16; ++i) {
    o[i] = vec4(p.x * float(i + 1) * 0.37 - 0.3, p.y * float(i + 2) * 0.21 + 0.1,
                (p.x + p.y) * 0.5 * float(i % 3) - 0.25, 1.0 - p.x * 0.4 + p.y * 0.05 * float(i));
  }
}
