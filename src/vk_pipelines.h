#pragma once 
#include "vk_types.h"
#include "vk_pipelines.h"
#include "vk_initializers.h"

#include <fstream>

namespace vkutil {

	bool LoadShaderModule(const char* filePath, VkDevice device, VkShaderModule* outShaderModule);
};

class PipelineBuilder
{
public:
	std::vector<VkPipelineShaderStageCreateInfo> shaderStages;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly;
    VkPipelineRasterizationStateCreateInfo rasterizer;
    VkPipelineColorBlendAttachmentState colorBlendAttachment;
    VkPipelineMultisampleStateCreateInfo multisampling;
    VkPipelineLayout pipelineLayout;
    VkPipelineDepthStencilStateCreateInfo depthStencil;
    VkPipelineRenderingCreateInfo renderInfo;
    VkFormat colorAttachmentformat;

    PipelineBuilder() { Clear(); }

    void Clear();

    VkPipeline BuildPipeline(VkDevice device);
    void SetShaders(VkShaderModule vertexShader, VkShaderModule fragmentShader);
    void SetShaders(VkShaderModule vertexShader, VkShaderModule geometryShader, VkShaderModule fragmentShader);
    void SetInputTopology(VkPrimitiveTopology topology);
    void SetPolygonMode(VkPolygonMode mode);
    void SetCullMode(VkCullModeFlags cullMode, VkFrontFace frontFace);
    void SetMultisampilingNone();
    void SetMultisampling(VkSampleCountFlagBits sampleCount);
    void DisableBlending();
    void SetColorAttachmentFormat(VkFormat format);
    void DisableColorAttachment();
    void SetDepthFormat(VkFormat format);
    void DisableDepthtest();
    void EnableDepthtest(bool depthWriteEnable, VkCompareOp op);
    void EnableBlendingAdditive();
    void EnableBlendingAlphaBlend();

};