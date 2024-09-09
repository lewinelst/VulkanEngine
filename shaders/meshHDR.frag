#version 450

#extension GL_GOOGLE_include_directive : require
#include "input_structures.glsl"

layout (location = 0) in vec3 inNormal;
layout (location = 1) in vec3 inColor;
layout (location = 2) in vec2 inUV;

layout (location = 0) out vec4 outFragColor;

void main() 
{
	float lightValue = max(dot(inNormal, sceneData.lightPosition.xyz), 0.1f);

	const float gamma = 2.2;
	vec3 hdrColor = inColor * texture(colorTex, inUV).rgb;
	vec3 mapped = hdrColor / (hdrColor + vec3(1.0f));
	vec3 color = pow(mapped, vec3(1.0f / gamma));
	vec3 ambient = color *  sceneData.ambientColor.xyz;

	outFragColor = vec4(color * lightValue *  sceneData.sunlightColor.w + ambient ,1.0f);
}