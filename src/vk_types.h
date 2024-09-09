#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <span>
#include <array>
#include <functional>
#include <deque>

#include <vulkan/vulkan.h>
#include <vulkan/vk_enum_string_helper.h>
#include <vma/vk_mem_alloc.h> // add new line to additional include dependencies with library

#include <fmt/core.h>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>


#define VK_CHECK(x)                                                     \
    do {                                                                \
        VkResult err = x;                                               \
        if (err) {                                                      \
            fmt::println("Detected Vulkan error: {}", string_VkResult(err)); \
            abort();                                                    \
        }                                                               \
    } while (0)

struct AllocatedImage
{
    VkImage image;
    VkImageView imageView;
    VmaAllocation allocation;
    VkExtent3D imageExtent;
    VkFormat imageFormat;
};

struct AllocatedBuffer
{
    VkBuffer buffer;
    VmaAllocation allocation;
    VmaAllocationInfo info;
};

struct Vertex
{
    glm::vec3 position;
    float uv_x;
    glm::vec3 normal;
    float uv_y;
    glm::vec4 color;
};

struct GPUMeshBuffers
{
    AllocatedBuffer indexBuffer;
    AllocatedBuffer vertexBuffer;
    VkDeviceAddress vertexBufferAddress;
};

struct GPUParticleBuffers
{
    AllocatedBuffer particleBuffer;
    VkDeviceAddress particleBufferAddress;
    size_t bufferSize;
};

struct GPUDrawPushConstants
{
    glm::mat4 renderMatrix;
    VkDeviceAddress vertexBuffer;
};

struct GPUDrawPushDepthConstants
{
    glm::mat4 renderMatrix;
    glm::vec4 lightPosition;
    float farPlane;
    VkDeviceAddress vertexBuffer;
};

struct GPUDrawPushParticleConstants
{
    glm::mat4 renderMatrix;
    VkDeviceAddress vertexBuffer;
    VkDeviceAddress particlePositionBuffer;
};

enum class MaterialPass : uint8_t
{
    MainColor,
    Transparent,
    Other
};

struct MaterialPipeline
{
    VkPipeline pipeline;
    VkPipelineLayout layout;
};

struct MaterialInstance
{
    MaterialPipeline* pipeline;
    VkDescriptorSet materialSet;
    MaterialPass passType;
};

struct DrawContext;

class IRenderable
{
    virtual void Draw(const glm::mat4& topMatrix, DrawContext& context) = 0;
};

struct Node : public IRenderable
{
    std::weak_ptr<Node> parent;
    std::vector<std::shared_ptr<Node>> children;

    glm::mat4 localTransform;
    glm::mat4 worldTransform;

    void RefreshTransform(const glm::mat4& parentMatrix)
    {
        worldTransform = parentMatrix * localTransform;
        for (auto c : children)
        {
            c->RefreshTransform(worldTransform);
        }
    }

    virtual void Draw(const glm::mat4& topMatrix, DrawContext& context)
    {
        for (auto& c : children)
        {
            c->Draw(topMatrix, context);
        }
    }
};
