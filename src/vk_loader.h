#pragma once

#include "vk_types.h"

#include "vk_descriptors.h"
#include <unordered_map>
#include <filesystem>

class VulkanEngine;

struct GLTFMaterial {
	MaterialInstance data;
};

struct Bounds
{
	glm::vec3 origin;
	float sphereRadius;
	glm::vec3 extents;
};

struct GeoSurface
{
	uint32_t startIndex;
	uint32_t count;
	Bounds bounds;
	std::shared_ptr<GLTFMaterial> material;
};

struct MeshAsset
{
	std::string name;

	std::vector<GeoSurface> surfaces;
	GPUMeshBuffers meshBuffers;
};

struct LoadedGLTF : public IRenderable
{
public:
	std::unordered_map<std::string, std::shared_ptr<MeshAsset>> meshes;
	std::unordered_map<std::string, std::shared_ptr<Node>> nodes;
	std::unordered_map<std::string, AllocatedImage > images;
	std::unordered_map<std::string, std::shared_ptr<GLTFMaterial>> materials;

	std::vector<std::shared_ptr<Node>> topNodes;

	std::vector<VkSampler> samplers;

	DescriptorAllocatorGrowable descriptorPool;

	AllocatedBuffer materialDataBuffer;

	VulkanEngine* creator;

	~LoadedGLTF() { ClearAll(); };

	virtual void Draw(const glm::mat4& topMatrix, DrawContext& context);

private:

	void ClearAll();
};

std::optional<std::shared_ptr<LoadedGLTF>> LoadGltf(VulkanEngine* engine, std::string_view filePath);
std::optional<AllocatedImage> LoadImage(VulkanEngine* engine, std::string filePath);
std::optional<AllocatedImage> LoadTextureArray(VulkanEngine* engine, std::string filePath, uint32_t columnNum, uint32_t rowNum, uint32_t layerAmount);
