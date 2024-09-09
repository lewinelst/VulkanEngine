#pragma once

#include "vk_types.h"

#include <SDL_events.h>

class Camera {
public:
	glm::vec3 velocity;
	glm::vec3 position;

	float pitch{ 0.0f };
	float yaw{ 0.0f };
	float cameraSpeed{ 1.0f };

	glm::mat4 GetViewMatrix();
	glm::mat4 GetRotationMatrix();

	void ProcessSDLEvent(SDL_Event& e);

	void Update();
};
