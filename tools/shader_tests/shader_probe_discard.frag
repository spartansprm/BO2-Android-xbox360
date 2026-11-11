#version 450
layout(location=0) out vec4 color;
void main(){if((int(gl_FragCoord.x)+int(gl_FragCoord.y))%2==0)discard;color=vec4(.2,.8,.3,1);}
