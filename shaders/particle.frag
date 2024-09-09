#version 450

layout (location = 0) in vec2 inTexCoords;

layout (location = 1) in float inLifetime;

layout (location = 2) in float inAliveTime;

layout (location = 0) out vec4 outFragColor;

layout(binding = 0) uniform  ParticleData
{   
	mat4 view;
	mat4 projection;
	float particleSize;
	int textureArraySize;
} particleData;

layout (binding = 1) uniform sampler2DArray particleTex;

void main() 
{
    int texNumber = int(((inLifetime - inAliveTime) / inLifetime) * particleData.textureArraySize);

    vec3 textureCoordinate = vec3(inTexCoords, texNumber); 

    vec4 color = texture(particleTex, textureCoordinate);
    
    color.a = color.a * ((inLifetime - inAliveTime) / inLifetime);

    outFragColor = color;

}