#version 450
layout(constant_id=0) const uint config=256u;
void main(){
    vec2 uv=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);
    gl_Position=vec4(uv*2.0-1.0+vec2(config==256u?0.0:4.0,0.0),0.8,1.0);
}
