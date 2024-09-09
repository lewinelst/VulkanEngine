#version 450

#extension GL_GOOGLE_include_directive : require
#include "input_structures.glsl"

layout (location = 0) in vec3 inFragPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec3 inColor;
layout (location = 3) in vec2 inUV;

layout (location = 0) out vec4 outFragColor;

void main() 
{
	vec3 color = inColor * texture(colorTex,inUV).xyz;

	vec3 ambient = color *  sceneData.ambientColor.xyz;

	vec3 lightDir = normalize(sceneData.lightPosition.xyz - inFragPos);
	vec3 normal = normalize(inNormal);
	float diff = max(dot(lightDir, normal), 0.0);
    vec3 diffuse = diff * color;

	vec3 viewDir = normalize(sceneData.viewPosition.xyz - inFragPos);
    vec3 reflectDir = reflect(-lightDir, normal);
    float spec = 0.0;

	vec3 halfwayDir = normalize(lightDir + viewDir);  
    spec = pow(max(dot(normal, halfwayDir), 0.0), 32.0);

	vec3 specular = vec3(0.3) * spec; // assuming bright white light color
    outFragColor = vec4((ambient + diffuse + specular), 1.0);

}