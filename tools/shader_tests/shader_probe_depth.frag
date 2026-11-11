#version 450
layout(location=0) out vec4 color;
void main(){gl_FragDepth=.2;color=vec4(.2,.3,.8,1);}
