#include <glm/gtc/matrix_transform.hpp>

#include "vk_particles.h"
#include "vk_engine.h"

// emmitter
ParticleEmitter::ParticleEmitter(VulkanEngine* engine, glm::vec3 emitterPos, glm::vec3 spawnDimensions, uint32_t particleNum, uint32_t lowerLife, uint32_t upperLife)
{

	// set particle lifespan 
	this->lowerLife = lowerLife; // was 22
	this->upperLife = upperLife; // was 25
	radius = 1.0f;
	dimensions = spawnDimensions;

	// set emitter location
	this->emitterPos = emitterPos;

	// create 400 particles

	this->particles.resize(particleNum);
	for (int i = 0; i < this->particles.size(); ++i)
	{
		// give every particle a random position within sphere
		glm::vec3 partPos = GetSpawnPosition(spawnDimensions);
		this->particles[i].startingPos = glm::vec3(emitterPos.x + (partPos.x * radius), emitterPos.y + (partPos.y * radius), emitterPos.z + (partPos.z * radius));
		this->particles[i].position = glm::vec3(emitterPos.x + (partPos.x * radius), emitterPos.y + (partPos.y * radius), emitterPos.z + (partPos.z * radius));
		this->particles[i].lifetime = RandomFloat(0, upperLife); // between 5 and 10 seconds lifetime 
		this->particles[i].aliveTime = 0.0f;
	}

	std::vector<ParticleGPUData> particlesGPUData;
	particlesGPUData.reserve(particles.size());

	std::transform(std::cbegin(particles), std::cend(particles),
		std::back_inserter(particlesGPUData),
		[](const auto& particle) {
			ParticleGPUData particleData;
			particleData.position = particle.position;
			particleData.aliveTime = particle.aliveTime;
			particleData.lifetime = particle.lifetime;
			return particleData;
		}
	);

	particleBuffers = engine->UploadParticles(particlesGPUData);
}

ParticleEmitter::~ParticleEmitter()
{

}

void ParticleEmitter::Update(VulkanEngine* engine, glm::vec3 movement, const float ft, Camera camera)
{
	this->cameraDistance = glm::length(camera.position - this->emitterPos); // sets camera distance for emitter transparency sorting

	for (int i = 0; i < this->particles.size(); ++i)
	{
		float frameTime = ft / 1000.0f; // converting to seconds

		this->particles[i].aliveTime += frameTime;
		this->particles[i].position += (movement * frameTime); // move particle based on direction (need to respawn if goes out of bounds) 

		if (this->particles[i].aliveTime >= this->particles[i].lifetime)
		{
			glm::vec3 partPos = GetSpawnPosition(dimensions);
			this->particles[i].position = glm::vec3(emitterPos.x + (partPos.x), emitterPos.y + (partPos.y * radius), emitterPos.z + (partPos.z * radius));
			this->particles[i].lifetime = RandomFloat(lowerLife, upperLife); // between 3 and 4 seconds lifetime 
			this->particles[i].aliveTime = 0.0f;
		}

		// stores camera distance 
		particles[i].cameraDistance = glm::length(camera.position - particles[i].position);
	}

	std::sort(this->particles.begin(), this->particles.end(), [](Particle a, Particle b) {return a.cameraDistance > b.cameraDistance; }); // sorting for transparency (check this is needed)

	std::vector<ParticleGPUData> particlesGPUData;
	particlesGPUData.reserve(particles.size());

	std::transform(std::cbegin(particles), std::cend(particles),
		std::back_inserter(particlesGPUData),
		[](const auto& particle) {
			ParticleGPUData particleData;
			particleData.position = particle.position;
			particleData.aliveTime = particle.aliveTime;
			particleData.lifetime = particle.lifetime;
			return particleData;
		}
	);

	engine->UpdateParticles(particleBuffers, particlesGPUData);
}

glm::vec3 ParticleEmitter::GetSpawnPosition(glm::vec3 spawnDimensions) 
{
	glm::vec3 pos = glm::vec3(RandomFloat(0.0f, spawnDimensions.x), RandomFloat(0.0f, spawnDimensions.y), RandomFloat(0.0f, spawnDimensions.z));;

	return pos;
}

void ParticleEmitter::Reset()
{
	for (int i = 0; i < this->particles.size(); i++)
	{
		this->particles[i].aliveTime = this->particles[i].lifetime;
	}
}

float ParticleEmitter::RandomFloat(float a, float b) {
	float random = ((float)rand()) / (float)RAND_MAX;
	float diff = b - a;
	float r = random * diff;
	return a + r;
}