#version 450
// GPL-3.0-only. Independent test, containing no game-derived shader code.
layout(set = 4, binding = 0, std140) uniform VertexConstants {
    vec4 positions[3];
} constants;
void main() {
    gl_Position = constants.positions[gl_VertexIndex];
}
