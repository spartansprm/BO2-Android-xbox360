#version 450
// GPL-3.0-only. Independent test, containing no game-derived shader code.
layout(set = 4, binding = 1, std140) uniform PixelConstants {
    vec4 color;
} constants;
layout(location = 0) out vec4 outputColor;
void main() {
    outputColor = constants.color;
}
