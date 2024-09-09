layout(set = 0, binding = 0) uniform  SceneData
{   
	mat4 view;
	mat4 proj;
	mat4 viewproj;
	vec4 ambientColor;
	vec4 lightPosition; //w for sun power
	vec4 lightColor;
	vec3 viewPosition;
	float shadowFarPlane;
	float attenuationFallOff;
	float shadowBias;
	int shadowAASamples;
	float gridSamplingDiskModifier;
} sceneData;

layout(set = 0, binding = 1) uniform samplerCube depthMap;

layout(set = 1, binding = 0) uniform GLTFMaterialData
{   
	vec4 colorFactors;
	vec4 metalRoughFactors;
} materialData;

layout(set = 1, binding = 1) uniform sampler2D colorTex;
layout(set = 1, binding = 2) uniform sampler2D metalRoughTex;
layout(set = 1, binding = 3) uniform sampler2D normalTex; 
layout(set = 1, binding = 4) uniform sampler2D occlusionTex; 
layout(set = 1, binding = 5) uniform sampler2D emissionTex; 