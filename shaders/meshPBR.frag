#version 450

#extension GL_GOOGLE_include_directive : require
#include "input_structures.glsl"

layout (location = 0) in vec3 inWorldPos;
layout (location = 1) in vec3 inNormal;
layout (location = 2) in vec3 inColor;
layout (location = 3) in vec2 inUV;


layout (location = 0) out vec4 outFragColor;

const float PI = 3.14159265359;

vec3 gridSamplingDisk[20] = vec3[]
(
   vec3(1, 1,  1), vec3( 1, -1,  1), vec3(-1, -1,  1), vec3(-1, 1,  1), 
   vec3(1, 1, -1), vec3( 1, -1, -1), vec3(-1, -1, -1), vec3(-1, 1, -1),
   vec3(1, 1,  0), vec3( 1, -1,  0), vec3(-1, -1,  0), vec3(-1, 1,  0),
   vec3(1, 0,  1), vec3(-1,  0,  1), vec3( 1,  0, -1), vec3(-1, 0, -1),
   vec3(0, 1,  1), vec3( 0, -1,  1), vec3( 0, -1, -1), vec3( 0, 1, -1)
);

// ----------------------------------------------------------------------------

float ShadowCalculation(vec3 worldPosition)
{
    vec3 fragToLight = worldPosition - sceneData.lightPosition.xyz;;

    float currentDepth = length(fragToLight);

    float shadow = 0.0;
    float bias = sceneData.shadowBias;
    int samples = sceneData.shadowAASamples;
    float viewDistance = length(sceneData.viewPosition - worldPosition);
    float diskRadius = (1.0 + (viewDistance / sceneData.shadowFarPlane)) / 25.0; // first 25 is far plane for shadow;
    for(int i = 0; i < samples; ++i)
    {
        float closestDepth = texture(depthMap, fragToLight + (gridSamplingDisk[i] * sceneData.gridSamplingDiskModifier) * diskRadius).r;
        closestDepth *= sceneData.shadowFarPlane;   // 25 is far plane for shadow
        if(currentDepth - bias > closestDepth)
            shadow += 1.0;
    }
    shadow /= float(samples);
        
    return shadow;
}

// ----------------------------------------------------------------------------
vec3 getNormalFromMap()
{
    vec3 tangentNormal = texture(normalTex, inUV).rgb * 2.0 - 1.0;

    vec3 Q1  = dFdx(inWorldPos);
    vec3 Q2  = dFdy(inWorldPos);
    vec2 st1 = dFdx(inUV);
    vec2 st2 = dFdy(inUV);

    vec3 N   = normalize(inNormal);
    vec3 T  = normalize(Q1*st2.t - Q2*st1.t);
    vec3 B  = -normalize(cross(N, T));
    mat3 TBN = mat3(T, B, N);

    return normalize(TBN * tangentNormal);
}
// ----------------------------------------------------------------------------
float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness*roughness;
    float a2 = a*a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH*NdotH;

    float nom   = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;

    return nom / denom;
}
// ----------------------------------------------------------------------------
float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r*r) / 8.0;

    float nom   = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return nom / denom;
}
// ----------------------------------------------------------------------------
float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}
// ----------------------------------------------------------------------------
vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ----------------------------------------------------------------------------

void main() 
{
    vec4 colorTexture = texture(colorTex,inUV);
    if (colorTexture.a < 0.1)
        discard;

    vec3 albedo = pow(colorTexture.rgb, vec3(2.2)) * materialData.colorFactors.xyz;
    float metallic = texture(metalRoughTex, inUV).b * materialData.metalRoughFactors.x;
    float roughness = texture(metalRoughTex, inUV).g * materialData.metalRoughFactors.y;
    float ao = texture(occlusionTex, inUV).r;

    vec3 N = getNormalFromMap();
    vec3 V = normalize(sceneData.viewPosition.xyz - inWorldPos);

    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);

    vec3 Lo = vec3(0.0);
    // if multiple lights start loop here
    // light radiance
    vec3 L = normalize(sceneData.lightPosition.xyz - inWorldPos);
    vec3 H = normalize(V + L);
    float lightDistance = length(sceneData.lightPosition.xyz - inWorldPos);
    float attenuation = 1.0 / pow(lightDistance, sceneData.attenuationFallOff);
    vec3 radiance = sceneData.lightColor.xyz * attenuation * sceneData.lightPosition.w; // w holds power

    // Cook-Torrance BRDF
    float NDF = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 numerator = NDF * G * F;
    float denominator = 1.0 * max(dot(N, V), 0.0) * max(dot(N,L), 0.0) + 0.0001;
    vec3 specular = numerator / denominator;

    vec3 kS = F;
    vec3 kD = vec3(1.0) - kS;
    kD *= 1.0 - metallic;

    float NdotL = max(dot(N, L), 0.0);

    Lo += (kD * albedo / PI + specular) * radiance * NdotL;

    // end loop for multiple lights

    vec3 ambient = sceneData.ambientColor.xyz * albedo * ao;

    vec3 emission = texture(emissionTex, inUV).rgb;

    float shadow = ShadowCalculation(inWorldPos);    

    //vec3 color = ambient + Lo + emission;
    vec3 color = ambient + (1.0 - shadow) * (Lo + emission);

    // HDR tonemapping
    color = color / (color + vec3(1.0));
    
    // gamma correct
    color = pow(color, vec3(1.0/2.2));
    
    outFragColor = vec4(color, 1.0);
    //outFragColor = texture(depthMap, inWorldPos);
    //outFragColor = vec4(shadow, shadow, shadow, 1.0f);
}