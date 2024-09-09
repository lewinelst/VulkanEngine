#pragma once 

#include <vulkan/vulkan.h>

namespace vkutil {
	void TransititionImage(VkCommandBuffer cmd, VkImage image, VkImageLayout currentLayout, VkImageLayout newLayout);
	void CopyImageToImage(VkCommandBuffer cmd, VkImage source, VkImage destination, VkExtent2D srcSize, VkExtent2D dstSize);
	void ResolveImage(VkCommandBuffer cmd, VkImage srcImg, VkImage destinaionImage, VkExtent3D resolveImageSize);
	void GenerateMipMaps(VkCommandBuffer cmd, VkImage image, VkExtent2D imageSize);
	void ClearImage(VkCommandBuffer cmd, VkImage image, VkImageLayout currentLayout, VkClearColorValue clearColorValue);
};