#version 450
#extension GL_EXT_buffer_reference : require

struct Vertex {
	vec3 position;
	float uvX;
	vec3 normal;
	float uvY;
	vec4 color;
}; 

layout(buffer_reference, std430) readonly buffer VertexBuffer{ 
	Vertex vertices[];
};

struct ParticlePosition 
{
	vec3 particlePosition;
	float lifetime;
	float aliveTime;
};

layout(buffer_reference, std430) readonly buffer PositionBuffer{ 
	ParticlePosition positions[];
};

//push constants block
layout( push_constant ) uniform constants
{	
	mat4 renderMatrix;
	VertexBuffer vertexBuffer;
	PositionBuffer positionBuffer;
} PushConstants;

layout(location = 0) out vec2 outTexCoords;

layout(location = 1) out float outLifetime;

layout(location = 2) out float outAliveTime;

layout(binding = 0) uniform  ParticleData
{   
	mat4 view;
	mat4 projection;
	float particleSize;
	int textureArraySize;
} particleData;


void main()
{
	Vertex v = PushConstants.vertexBuffer.vertices[gl_VertexIndex];
	ParticlePosition p = PushConstants.positionBuffer.positions[gl_InstanceIndex];

	vec4 position_viewspace = particleData.view * vec4( p.particlePosition.xyz, 1 ); // ,movement handled by (movement * dt * position.w (AliveTime)

   position_viewspace.xy += (particleData.particleSize * (p.aliveTime / p.lifetime)) * (v.position.xy - vec2(0.5));

   outLifetime = p.lifetime;
   outAliveTime = p.aliveTime;
   outTexCoords = vec2(v.uvX, v.uvY);
   gl_Position = particleData.projection * position_viewspace;
}