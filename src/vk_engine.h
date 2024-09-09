#pragma once

#include "vk_types.h"
#include "vk_descriptors.h"
#include "vk_loader.h"
#include "vk_particles.h"
#include "camera.h"

struct DeletionQueue
{
	std::deque<std::function<void()>> deletors;

	void PushFunction(std::function<void()>&& function)
	{
		deletors.push_back(function);
	}

	void Flush()
	{
		// reverse iterate over queue
		for (auto it = deletors.rbegin(); it != deletors.rend(); it++)
		{
			(*it)(); // call functors 
		}

		deletors.clear();
	}
};

struct FrameData
{
	VkCommandPool commandPool;
	VkCommandBuffer mainCommandBuffer;

	VkSemaphore swapchainSemaphore, renderSemaphore;
	VkFence renderFence;

	DeletionQueue deletionQueue;
	DescriptorAllocatorGrowable frameDescriptors;
};

struct GPUSceneData
{
	glm::mat4 view;
	glm::mat4 proj;
	glm::mat4 viewproj;
	glm::vec4 ambientColor;
	glm::vec4 lightPosition;
	glm::vec4 lightColor;
	glm::vec3 viewPosition;
	float shadowFarPlane;
	float attenuationFallOff;
	float shadowBias;
	int shadowAASamples;
	float gridSamplingDiskModifier;
};

struct DepthMapGeometryData
{
	glm::mat4 shadowMatricies[6];
};

struct ParticleSceneData
{
	glm::mat4 view;
	glm::mat4 projection;
	float particleSize;
	int textureArraySize;
};

struct GLTFMetallicRoughness
{
	MaterialPipeline opaquePipeline;
	MaterialPipeline transparentPipeline;
	MaterialPipeline skyboxPipeline;

	VkDescriptorSetLayout materialLayout;

	struct MaterialConstants
	{
		glm::vec4 colorFactors;
		glm::vec4 metalRoughFactors;

		glm::vec4 padding[14];
	};

	struct MaterialResources
	{
		AllocatedImage colorImage;
		VkSampler colorSampler;
		AllocatedImage metallicRoughnessImage;
		VkSampler metallicRoughnessSampler;
		AllocatedImage normalImage;
		VkSampler normalSampler;
		AllocatedImage occlusionImage;
		VkSampler occlusionSampler;
		AllocatedImage emissionImage;
		VkSampler emissionSampler;
		VkBuffer dataBuffer;
		uint32_t dataBufferOffset;

	};

	DescriptorWriter writer;

	void BuildPipelines(VulkanEngine* engine);
	void ClearResources(VkDevice device);

	MaterialInstance WriteMaterial(VkDevice device, MaterialPass pass, const MaterialResources& resources, DescriptorAllocatorGrowable& descriptorAllocator);
};

struct MeshNode : public Node
{
	std::shared_ptr<MeshAsset> mesh;

	virtual void Draw(const glm::mat4& topMatrix, DrawContext& context) override;
};

struct RenderObject
{
	uint32_t indexCount;
	uint32_t firstIndex;
	VkBuffer indexBuffer;

	MaterialInstance* material;
	Bounds bounds;
	glm::mat4 transform;
	VkDeviceAddress vertexBufferAddress;
};

struct DrawContext
{
	std::vector<RenderObject> OpaqueSurfaces;
	std::vector<RenderObject> TransparentSurfaces;
};

struct EngineStats
{
	float frameTime;
	float framesPerSecond;
	int triangleCount;
	int drawcallCount;
	float sceneUpdateTime;
	float meshDrawTime;
	float uptime;
};

struct EngineSettings
{
	bool hdrOn{ true };
	VkSampleCountFlagBits msaaSamples{ VK_SAMPLE_COUNT_8_BIT };
};

constexpr unsigned int FRAME_OVERLAP = 2;

class VulkanEngine 
{
public:

	bool isInitialized{ false };
	int frameNumber{ 0 };
	bool stopRendering{ false };
	bool resizeRequested{ false };
	VkExtent2D windowExtent{ 1920 , 1080 };

	struct SDL_Window* window{ nullptr };

	static VulkanEngine& Get();

	Camera mainCamera;

	VkInstance instance;
	VkDebugUtilsMessengerEXT debugMessenger;
	VkPhysicalDevice physicalDevice;
	VkDevice device;
	VkSurfaceKHR surface;

	VkSwapchainKHR swapchain;
	VkFormat swapchainImageFormat;

	std::vector<VkImage> swapchainImages;
	std::vector<VkImageView> swapchainImageViews;
	VkExtent2D swapchainExtent;

	FrameData frames[FRAME_OVERLAP];
	FrameData& GetCurrentFrame() { return frames[frameNumber % FRAME_OVERLAP]; };

	VkQueue graphicsQueue;
	uint32_t graphicsQueueFamily;

	DeletionQueue mainDeletionQueue;

	VmaAllocator allocator;

	// draw resources
	AllocatedImage drawImage;
	AllocatedImage depthImage;
	AllocatedImage colorImage;
	VkExtent2D drawExtent;
	float renderScale = 1.0f;

	DescriptorAllocatorGrowable globalDescriptorAllocator;

	VkFence immFence;
	VkCommandBuffer immCommandBuffer;
	VkCommandPool immCommandPool;

	GPUSceneData sceneData;

	VkDescriptorSetLayout gpuSceneDataDescriptorLayout;

	AllocatedImage whiteImage;
	AllocatedImage blackImage;
	AllocatedImage greyImage;
	AllocatedImage errorImage;

	VkSampler defaultSamplerLinear;
	VkSampler defaultSamplerNearest;

	MaterialInstance defaultData;
	GLTFMetallicRoughness metalRoughMaterial;

	DrawContext mainDrawContext;
	std::unordered_map<std::string, std::shared_ptr<Node>> loadedNodes;

	std::unordered_map<std::string, std::shared_ptr<LoadedGLTF>> loadedScenes;

	VkPipeline skyboxPipeline;
	VkPipelineLayout skyboxPipelineLayout;
	VkDescriptorSetLayout skyboxDescriptorLayout;
	GPUMeshBuffers skyboxCube;
	AllocatedImage skyboxImage;

	VkPipeline depthMapPipeline;
	VkPipelineLayout depthMapPipelineLayout;
	VkDescriptorSetLayout depthMapDescriptorLayout;
	AllocatedImage depthCubemapImage;
	DepthMapGeometryData depthMapGeometryData;
	float depthCubemapSize = 1920;

	VkPipeline particlePipeline;
	VkPipelineLayout particlePipelineLayout;
	VkDescriptorSetLayout particleDescriptorLayout;
	GPUMeshBuffers particleBillboard;
	AllocatedImage particleSmokeImage;
	ParticleEmitter* particleEmitter;

	EngineStats stats;

	EngineSettings engineSettings;

	float nearPlane = 0.1f;
	float farPlane = 10000.0f;

	//initializes everything in the engine
	void Init();

	//shuts down the engine
	void Cleanup();

	//draw loop
	void Draw();

	//run main loop
	void Run();

	GPUMeshBuffers UploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices);

	GPUParticleBuffers UploadParticles(std::span<ParticleGPUData> particlesGPUData);
	void UpdateParticles(GPUParticleBuffers& buffer, std::span<ParticleGPUData> particlesGPUData);

	AllocatedBuffer CreateBuffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage);

	AllocatedImage CreateImage(VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped = false);
	AllocatedImage CreateImage(void* data, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped = false);
	AllocatedImage CreateImageArray(VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped, uint32_t layerAmount);
	AllocatedImage CreateImageArray(void* data, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped, uint32_t columnNum, uint32_t rowNum, uint32_t layerAmount);
	void DestroyImage(const AllocatedImage& image);
	void DestroyBuffer(const AllocatedBuffer& buffer);



private:
	void InitVulkan();
	void InitSwapchain();
	void InitCommands();
	void InitSyncStructures();

	void CreateSwapchain(uint32_t width, uint32_t height);

	void DestroySwapchain();

	void DrawDepthMap(VkCommandBuffer cmd);

	void DrawSkybox(VkCommandBuffer cmd);

	void DrawGeometry(VkCommandBuffer cmd);

	void DrawParticles(VkCommandBuffer cmd);

	void InitDescriptors();

	void InitPipelines();

	void InitDepthMapPipeline();

	void InitSkyboxPipeline();

	void InitParticlePipeline();

	void InitImGui();

	void DrawImGui(VkCommandBuffer cmd, VkImageView targetImageView);

	void ImmediateSubmit(std::function<void(VkCommandBuffer cmd)>&& function);

	void InitDefaultData();

	void ResizeSwapchain();

	void UpdateScene();

	void UpdateSceneNonShadow();

	VkSampleCountFlagBits GetMaxUsableSampleCount();
	bool IsVisible(const RenderObject& object, const glm::mat4& viewProjection);
};
