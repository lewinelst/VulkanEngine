#pragma once

#include "camera.h"

#include <string>
#include <vector>
#include <random>
#include <functional>
#include <iostream>
#include <optional>
#include "vk_types.h"


class VulkanEngine;

struct Particle
{
	glm::vec3 position;
	glm::vec3 startingPos;
	float lifetime;
	float aliveTime;
	float textureNum;
	float cameraDistance;

	bool operator<(Particle& comparisonParticle) { // comparison function used transparency sorting
		return this->cameraDistance > comparisonParticle.cameraDistance;
	}
};

struct ParticleGPUData
{
	glm::vec3 position;
	float lifetime;
	float aliveTime;
	float padding[3];
};

class ParticleEmitter
{
public:

	ParticleEmitter(VulkanEngine* engine, glm::vec3 emitterPos, glm::vec3 spawnDimensions, uint32_t particleNum, uint32_t lowerLife, uint32_t upperLife);
	~ParticleEmitter();

	void Update(VulkanEngine* engine, glm::vec3 movement, const float ft, Camera camera);
	void Reset();

	glm::vec3 emitterPos;
	float cameraDistance;

	std::vector< Particle > particles;
	GPUParticleBuffers particleBuffers;

private:
	float lowerLife;
	float upperLife;
	float particleSize;
	float radius;

	glm::vec3 dimensions;

	glm::vec3 GetSpawnPosition(glm::vec3 spawnDimensions);

	float RandomFloat(float a, float b);
};