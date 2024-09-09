#include <stb_image/stb_image.h>
#include <glm/gtx/quaternion.hpp>

#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/parser.hpp>
#include <fastgltf/tools.hpp>

#include "vk_loader.h"
#include "vk_engine.h"
#include "vk_initializers.h"
#include "vk_types.h"

#include <iostream>

VkFilter ExtractFilter(fastgltf::Filter filter)
{
	switch (filter)
	{
	case fastgltf::Filter::Nearest:
	case fastgltf::Filter::NearestMipMapNearest:
	case fastgltf::Filter::NearestMipMapLinear:
		return VK_FILTER_NEAREST;

	case fastgltf::Filter::Linear:
	case fastgltf::Filter::LinearMipMapNearest:
	case fastgltf::Filter::LinearMipMapLinear:
	default:
		return VK_FILTER_LINEAR;
	}
}

VkSamplerMipmapMode ExtractMipmapMode(fastgltf::Filter filter)
{
	switch (filter)
	{
	case fastgltf::Filter::NearestMipMapNearest:
	case fastgltf::Filter::LinearMipMapNearest:
		return VK_SAMPLER_MIPMAP_MODE_NEAREST;

	case fastgltf::Filter::NearestMipMapLinear:
	case fastgltf::Filter::LinearMipMapLinear:
	default:
		return VK_SAMPLER_MIPMAP_MODE_LINEAR;
	}
}

std::optional<AllocatedImage> LoadImage(VulkanEngine* engine, fastgltf::Asset& asset, fastgltf::Image& image)
{
	AllocatedImage newImage{};

	int width, height, nrChannels;

	std::visit(
		fastgltf::visitor
		{
			[](auto& arg) {},
			[&](fastgltf::sources::URI& filePath)
			{
				assert(filePath.fileByteOffset == 0);
				assert(filePath.uri.isLocalPath());

				const std::string path(filePath.uri.path().begin(), filePath.uri.path().end());
				unsigned char* data = stbi_load(path.c_str(), &width, &height, &nrChannels, 4);
				if (data)
				{
					VkExtent3D imageSize;
					imageSize.width = width;
					imageSize.height = height;
					imageSize.depth = 1;

					newImage = engine->CreateImage(data, imageSize, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT, true);

					stbi_image_free(data);
				}
			},
			[&](fastgltf::sources::Vector& vector)
			{
				unsigned char* data = stbi_load_from_memory(vector.bytes.data(), static_cast<int>(vector.bytes.size()), &width, &height, &nrChannels, 4);
				if (data)
				{
					VkExtent3D imageSize;
					imageSize.width = width;
					imageSize.height = height;
					imageSize.depth = 1;

					newImage = engine->CreateImage(data, imageSize, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT, true);

					stbi_image_free(data);

				}
			},
			[&](fastgltf::sources::BufferView& view) 
			{
				auto& bufferView = asset.bufferViews[view.bufferViewIndex];
				auto& buffer = asset.buffers[bufferView.bufferIndex];

				std::visit(fastgltf::visitor 
				{
							   [](auto& arg) {},
							   [&](fastgltf::sources::Vector& vector) 
								{
								   unsigned char* data = stbi_load_from_memory(vector.bytes.data() + bufferView.byteOffset, static_cast<int>(bufferView.byteLength), &width, &height, &nrChannels, 4);
								   if (data) 
								   {
									   VkExtent3D imageSize;
									   imageSize.width = width;
									   imageSize.height = height;
									   imageSize.depth = 1;

									   newImage = engine->CreateImage(data, imageSize, VK_FORMAT_R8G8B8A8_UNORM,
										   VK_IMAGE_USAGE_SAMPLED_BIT,true);

									   stbi_image_free(data);
								   }
							   } 
				},
				buffer.data);
			},
		},
		image.data);

	if (newImage.image == VK_NULL_HANDLE)
	{
		return {};
	} 
	else
	{
		return newImage;
	}
}

std::optional<std::shared_ptr<LoadedGLTF>> LoadGltf(VulkanEngine* engine, std::string_view filePath)
{
	fmt::println("Loading GLTF file: {}", filePath);

	std::shared_ptr<LoadedGLTF> scene = std::make_shared<LoadedGLTF>();
	scene->creator = engine;
	LoadedGLTF& file = *scene.get();

	fastgltf::Parser parser{};

	constexpr auto gltfOptions = fastgltf::Options::DontRequireValidAssetMember | fastgltf::Options::AllowDouble | fastgltf::Options::LoadGLBBuffers | fastgltf::Options::LoadExternalBuffers;
	// fastgltf::Options::LoadExternalImages;

	fastgltf::GltfDataBuffer data;
	data.loadFromFile(filePath);

	fastgltf::Asset gltf;

	std::filesystem::path path = filePath;

	auto type = fastgltf::determineGltfFileType(&data);
	if (type == fastgltf::GltfType::glTF) 
	{
		auto load = parser.loadGLTF(&data, path.parent_path(), gltfOptions);
		if (load)
		{
			gltf = std::move(load.get());
		}
		else 
		{
			std::cerr << "Failed to load glTF: " << fastgltf::to_underlying(load.error()) << std::endl;
			return {};
		}
	}
	else if (type == fastgltf::GltfType::GLB) 
	{
		auto load = parser.loadBinaryGLTF(&data, path.parent_path(), gltfOptions);
		if (load) 
		{
			gltf = std::move(load.get());
		}
		else 
		{
			std::cerr << "Failed to load glTF: " << fastgltf::to_underlying(load.error()) << std::endl;
			return {};
		}
	}
	else 
	{
		std::cerr << "Failed to determine glTF container" << std::endl;
		return {};
	}

	std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> sizes = { 
		{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3},
		{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3},
		{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}
	};

	file.descriptorPool.InitPools(engine->device, gltf.materials.size(), sizes);

	for (fastgltf::Sampler sampler : gltf.samplers)
	{
		VkSamplerCreateInfo sample = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .pNext = nullptr };
		sample.maxLod = VK_LOD_CLAMP_NONE;
		sample.minLod = 0;

		sample.magFilter = ExtractFilter(sampler.magFilter.value_or(fastgltf::Filter::Nearest));
		sample.minFilter = ExtractFilter(sampler.minFilter.value_or(fastgltf::Filter::Nearest));

		sample.mipmapMode = ExtractMipmapMode(sampler.minFilter.value_or(fastgltf::Filter::Nearest));

		VkSampler newSampler;
		vkCreateSampler(engine->device, &sample, nullptr, &newSampler);

		file.samplers.push_back(newSampler);
	}

	std::vector<std::shared_ptr<MeshAsset>> meshes;
	std::vector<std::shared_ptr<Node>> nodes;
	std::vector<AllocatedImage> images;
	std::vector<std::shared_ptr<GLTFMaterial>> materials;

	for (fastgltf::Image& image : gltf.images)
	{
		std::optional<AllocatedImage> allocatedImage = LoadImage(engine, gltf, image);

		if (allocatedImage.has_value())
		{
			images.push_back(*allocatedImage);
			file.images[image.name.c_str()] = *allocatedImage;
		}
		else
		{
			images.push_back(engine->errorImage);
			std::cout << "gltf failed to load texture: " << image.name << std::endl;
		}
	}

	file.materialDataBuffer = engine->CreateBuffer(sizeof(GLTFMetallicRoughness::MaterialConstants) * gltf.materials.size(), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	int dataIndex = 0;
	GLTFMetallicRoughness::MaterialConstants* sceneMaterialConstants = (GLTFMetallicRoughness::MaterialConstants*)file.materialDataBuffer.info.pMappedData;

	for (fastgltf::Material& material : gltf.materials) 
	{
		std::shared_ptr<GLTFMaterial> newMat = std::make_shared<GLTFMaterial>();
		materials.push_back(newMat);
		file.materials[material.name.c_str()] = newMat;

		GLTFMetallicRoughness::MaterialConstants constants;
		constants.colorFactors.x = material.pbrData.baseColorFactor[0];
		constants.colorFactors.y = material.pbrData.baseColorFactor[1];
		constants.colorFactors.z = material.pbrData.baseColorFactor[2];
		constants.colorFactors.w = material.pbrData.baseColorFactor[3];

		constants.metalRoughFactors.x = material.pbrData.metallicFactor;
		constants.metalRoughFactors.y = material.pbrData.roughnessFactor;

		sceneMaterialConstants[dataIndex] = constants;

		MaterialPass passType = MaterialPass::MainColor;
		if (material.alphaMode == fastgltf::AlphaMode::Blend) 
		{
			passType = MaterialPass::Transparent;
		}

		GLTFMetallicRoughness::MaterialResources materialResources;
		materialResources.colorImage = engine->whiteImage;
		materialResources.colorSampler = engine->defaultSamplerLinear;
		materialResources.metallicRoughnessImage = engine->whiteImage;
		materialResources.metallicRoughnessSampler = engine->defaultSamplerLinear;
		materialResources.normalImage = engine->whiteImage; // TODO: Work out default value for this.
		materialResources.normalSampler = engine->defaultSamplerLinear;
		materialResources.occlusionImage = engine->whiteImage;
		materialResources.occlusionSampler = engine->defaultSamplerLinear;
		materialResources.emissionImage = engine->blackImage;
		materialResources.emissionSampler = engine->defaultSamplerLinear;

		materialResources.dataBuffer = file.materialDataBuffer.buffer;
		materialResources.dataBufferOffset = dataIndex * sizeof(GLTFMetallicRoughness::MaterialConstants);
		if (material.pbrData.baseColorTexture.has_value()) // base color texure
		{
			size_t img = gltf.textures[material.pbrData.baseColorTexture.value().textureIndex].imageIndex.value();
			size_t sampler = gltf.textures[material.pbrData.baseColorTexture.value().textureIndex].samplerIndex.value();

			materialResources.colorImage = images[img];
			materialResources.colorSampler = file.samplers[sampler];
		}

		if (material.pbrData.metallicRoughnessTexture.has_value()) // metallic roughness texture
		{
			size_t img = gltf.textures[material.pbrData.metallicRoughnessTexture.value().textureIndex].imageIndex.value();
			size_t sampler = gltf.textures[material.pbrData.metallicRoughnessTexture.value().textureIndex].samplerIndex.value();

			materialResources.metallicRoughnessImage = images[img];
			materialResources.metallicRoughnessSampler = file.samplers[sampler];
		}

		if (material.occlusionTexture.has_value())
		{
			size_t img = gltf.textures[material.occlusionTexture.value().textureIndex].imageIndex.value();
			size_t sampler = gltf.textures[material.occlusionTexture.value().textureIndex].samplerIndex.value();

			materialResources.occlusionImage = images[img];
			materialResources.occlusionSampler = file.samplers[sampler];
		}

		if (material.normalTexture.has_value())
		{
			size_t img = gltf.textures[material.normalTexture.value().textureIndex].imageIndex.value();
			size_t sampler = gltf.textures[material.normalTexture.value().textureIndex].samplerIndex.value();

			materialResources.normalImage = images[img];
			materialResources.normalSampler = file.samplers[sampler];
		}

		if (material.emissiveTexture.has_value())
		{
			size_t img = gltf.textures[material.emissiveTexture.value().textureIndex].imageIndex.value();
			size_t sampler = gltf.textures[material.emissiveTexture.value().textureIndex].samplerIndex.value();

			materialResources.emissionImage = images[img];
			materialResources.emissionSampler = file.samplers[sampler];
		}

		newMat->data = engine->metalRoughMaterial.WriteMaterial(engine->device, passType, materialResources, file.descriptorPool);

		dataIndex++;
	}

	std::vector<uint32_t> indices;
	std::vector<Vertex> vertices;

	for (fastgltf::Mesh& mesh : gltf.meshes)
	{
		std::shared_ptr<MeshAsset> newMesh = std::make_shared<MeshAsset>();
		meshes.push_back(newMesh);
		file.meshes[mesh.name.c_str()] = newMesh;
		newMesh->name = mesh.name;

		indices.clear();
		vertices.clear();

		for (auto&& p : mesh.primitives) 
		{
			GeoSurface newSurface;
			newSurface.startIndex = (uint32_t)indices.size();
			newSurface.count = (uint32_t)gltf.accessors[p.indicesAccessor.value()].count;

			size_t initial_vtx = vertices.size();

			// load indexes
			{
				fastgltf::Accessor& indexaccessor = gltf.accessors[p.indicesAccessor.value()];
				indices.reserve(indices.size() + indexaccessor.count);

				fastgltf::iterateAccessor<std::uint32_t>(gltf, indexaccessor,
					[&](std::uint32_t idx) 
					{
						indices.push_back(idx + initial_vtx);
					});
			}

			// load vertex positions
			{
				fastgltf::Accessor& posAccessor = gltf.accessors[p.findAttribute("POSITION")->second];
				vertices.resize(vertices.size() + posAccessor.count);

				fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, posAccessor,
					[&](glm::vec3 v, size_t index)
					{
						Vertex newvtx;
						newvtx.position = v;
						newvtx.normal = { 1, 0, 0 };
						newvtx.color = glm::vec4{ 1.f };
						newvtx.uv_x = 0;
						newvtx.uv_y = 0;
						vertices[initial_vtx + index] = newvtx;
					});
			}

			// load vertex normals
			auto normals = p.findAttribute("NORMAL");
			if (normals != p.attributes.end()) 
			{

				fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, gltf.accessors[(*normals).second],
					[&](glm::vec3 v, size_t index) 
					{
						vertices[initial_vtx + index].normal = v;
					});
			}

			// load UVs
			auto uv = p.findAttribute("TEXCOORD_0");
			if (uv != p.attributes.end()) 
			{

				fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, gltf.accessors[(*uv).second],
					[&](glm::vec2 v, size_t index) 
					{
						vertices[initial_vtx + index].uv_x = v.x;
						vertices[initial_vtx + index].uv_y = v.y;
					});
			}

			// load vertex colors
			auto colors = p.findAttribute("COLOR_0");
			if (colors != p.attributes.end()) 
			{
				fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf, gltf.accessors[(*colors).second],
					[&](glm::vec4 v, size_t index) 
					{
						vertices[initial_vtx + index].color = v;
					});
			}

			if (p.materialIndex.has_value())
			{
				newSurface.material = materials[p.materialIndex.value()];
			}
			else 
			{
				newSurface.material = materials[0];
			}

			glm::vec3 minPos = vertices[initial_vtx].position;
			glm::vec3 maxPos = vertices[initial_vtx].position;
			for (int i = initial_vtx; i < vertices.size(); i++)
			{
				minPos = glm::min(minPos, vertices[i].position);
				maxPos = glm::max(maxPos, vertices[i].position);
			}

			newSurface.bounds.origin = (maxPos + minPos) / 2.0f;
			newSurface.bounds.extents = (maxPos - minPos) / 2.0f;
			newSurface.bounds.sphereRadius = glm::length(newSurface.bounds.extents);

			newMesh->surfaces.push_back(newSurface);
		}

		newMesh->meshBuffers = engine->UploadMesh(indices, vertices);
	}

	for (fastgltf::Node& node : gltf.nodes) 
	{
		std::shared_ptr<Node> newNode;

		if (node.meshIndex.has_value()) 
		{
			newNode = std::make_shared<MeshNode>();
			static_cast<MeshNode*>(newNode.get())->mesh = meshes[*node.meshIndex];
		}
		else 
		{
			newNode = std::make_shared<Node>();
		}

		nodes.push_back(newNode);
		file.nodes[node.name.c_str()];

		std::visit(fastgltf::visitor{ [&](fastgltf::Node::TransformMatrix matrix) {
										  memcpy(&newNode->localTransform, matrix.data(), sizeof(matrix));
									  },
					   [&](fastgltf::Node::TRS transform) {
						   glm::vec3 tl(transform.translation[0], transform.translation[1],
							   transform.translation[2]);
						   glm::quat rot(transform.rotation[3], transform.rotation[0], transform.rotation[1],
							   transform.rotation[2]);
						   glm::vec3 sc(transform.scale[0], transform.scale[1], transform.scale[2]);

						   glm::mat4 tm = glm::translate(glm::mat4(1.0f), tl);
						   glm::mat4 rm = glm::toMat4(rot);
						   glm::mat4 sm = glm::scale(glm::mat4(1.0f), sc);

						   newNode->localTransform = tm * rm * sm;
					   } },
			node.transform);
	}

	for (int i = 0; i < gltf.nodes.size(); i++) 
	{
		fastgltf::Node& node = gltf.nodes[i];
		std::shared_ptr<Node>& sceneNode = nodes[i];

		for (auto& c : node.children) 
		{
			sceneNode->children.push_back(nodes[c]);
			nodes[c]->parent = sceneNode;
		}
	}

	// find nodes with no parents to assign them to top
	for (auto& node : nodes) 
	{
		if (node->parent.lock() == nullptr) 
		{
			file.topNodes.push_back(node);
			node->RefreshTransform(glm::mat4{ 1.0f });
		}
	}
	return scene;
}

void LoadedGLTF::Draw(const glm::mat4& topMatrix, DrawContext& context)
{
	for (auto& node : topNodes)
	{
		node->Draw(topMatrix, context);
	}
}

void LoadedGLTF::ClearAll()
{
	VkDevice device = creator->device;

	descriptorPool.DestroyPools(device);
	creator->DestroyBuffer(materialDataBuffer);

	for (auto& [k, v] : meshes)
	{
		creator->DestroyBuffer(v->meshBuffers.indexBuffer);
		creator->DestroyBuffer(v->meshBuffers.vertexBuffer);
	}

	for (auto& [k, v] : images)
	{
		if (v.image == creator->errorImage.image)
		{
			continue;
		}

		creator->DestroyImage(v);
	}

	for (auto& sampler : samplers)
	{
		vkDestroySampler(device, sampler, nullptr);
	}
}

std::optional<AllocatedImage> LoadImage(VulkanEngine* engine, std::string filePath)
{
	AllocatedImage newImage{};

	unsigned char* data;
	int width, height, nrChannels;

	fmt::println("Loading texture file: {}", filePath);

	data = stbi_load(filePath.c_str(), &width, &height, &nrChannels, 4);

	if (data)
	{
		VkExtent3D imageSize;
		imageSize.width = width;
		imageSize.height = height;
		imageSize.depth = 1;

		newImage = engine->CreateImage(data, imageSize, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT, false);

		stbi_image_free(data);
	}

	if (newImage.image == VK_NULL_HANDLE)
	{
		return {};
	}
	else
	{
		return newImage;
	}

}

std::optional<AllocatedImage> LoadTextureArray(VulkanEngine* engine, std::string filePath, uint32_t columnNum, uint32_t rowNum, uint32_t layerAmount)
{
	AllocatedImage newImage{};

	unsigned char* data;
	int width, height, nrChannels;

	fmt::println("Loading texture file: {}", filePath);

	data = stbi_load(filePath.c_str(), &width, &height, &nrChannels, 4);

	if (data)
	{
		VkExtent3D imageSize;
		imageSize.width = width;
		imageSize.height = height;
		imageSize.depth = 1;

		newImage = engine->CreateImageArray(data, imageSize, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT, false, columnNum, rowNum, layerAmount);

		stbi_image_free(data);
	}

	if (newImage.image == VK_NULL_HANDLE)
	{
		return {};
	}
	else
	{
		return newImage;
	}

}