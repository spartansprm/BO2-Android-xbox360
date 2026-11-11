

#version 450
layout(location = 0) out vec4 o0;
layout(location = 1) out vec4 o1;
layout(location = 2) out vec4 o2;
layout(location = 3) out vec4 o3;
void main() {
  o0 = vec4(1.0, 0.0, 1.0, 1.0);
  o1 = o0;
  o2 = o0;
  o3 = o0;
}
