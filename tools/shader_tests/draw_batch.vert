#version 450
layout(location=0) out vec4 color;
void main() {
    int v=gl_VertexIndex%3;
    int t=gl_VertexIndex/3;
    vec2 positions[3]=vec2[3](vec2(-.9,-.9),vec2(.8,-.7),vec2(.1,.9));
    gl_Position=vec4(positions[v]+vec2(float(t%7)*.015,float(t%5)*.01),.2+float(t%11)*.003,1);
    color=vec4(float((t*73)%255)/255,float((t*91)%255)/255,float((t*17)%255)/255,.25+float(t%3)*.1);
}
