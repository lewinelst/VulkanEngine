#version 450
#extension GL_EXT_buffer_reference : require

layout (triangles) in;
layout (triangle_strip, max_vertices=18) out;

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

layout(binding = 0) uniform ShadowMatricies
{   
	mat4 matricies[6];
} shadowMatricies;

layout (location = 0) out vec4 FragPos; // FragPos from GS (output per emitvertex)

void main()
{
    for(int face = 0; face < 6; ++face)
    {
        gl_Layer = face; // built-in variable that specifies to which face we render.
        for(int i = 0; i < 3; ++i) // for each triangle's vertices
        {
            FragPos = gl_in[i].gl_Position;
            gl_Position = shadowMatricies.matricies[face] * FragPos;
            EmitVertex();
        }    
        EndPrimitive();
    }
} 