#version 450
#extension GL_EXT_buffer_reference : require

struct Vertex {

	vec3 position;
	float uv_x;
	vec3 normal;
	float uv_y;
	vec4 color;
}; 

layout(buffer_reference, std430) readonly buffer VertexBuffer{ 
	Vertex vertices[];
};

//push constants block
layout( push_constant ) uniform constants
{	
	mat4 world_matrix;
	vec4 lightPosition;
    float shadowFarPlane;
	VertexBuffer vertexBuffer;
} PushConstants;

layout (location = 0) in vec4 inFragPos;

void main()
{
    float lightDistance = length(inFragPos.xyz - PushConstants.lightPosition.xyz);
    
    // map to [0;1] range by dividing by far_plane
    lightDistance = lightDistance / PushConstants.shadowFarPlane; // TODO: Replace 10 with actual value (Far plane)
    
    // write this as modified depth
    gl_FragDepth = lightDistance;
}