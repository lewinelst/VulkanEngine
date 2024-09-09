#include "camera.h"
#include <glm/gtx/transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <iostream>

void Camera::Update()
{
	glm::mat4 cameraRotation = GetRotationMatrix();
	position += glm::vec3(cameraRotation * glm::vec4(velocity * 0.5f, 0.0f));
}

void Camera::ProcessSDLEvent(SDL_Event& e)
{
	if (e.type == SDL_KEYDOWN)
	{
		if (e.key.keysym.sym == SDLK_w) { velocity.z = -1 * cameraSpeed; }
		if (e.key.keysym.sym == SDLK_s) { velocity.z = 1 * cameraSpeed; }
		if (e.key.keysym.sym == SDLK_a) { velocity.x = -1 * cameraSpeed; }
		if (e.key.keysym.sym == SDLK_d) { velocity.x = 1 * cameraSpeed; }
	}

	if (e.type == SDL_KEYUP)
	{
		if (e.key.keysym.sym == SDLK_w) { velocity.z = 0; }
		if (e.key.keysym.sym == SDLK_s) { velocity.z = 0; }
		if (e.key.keysym.sym == SDLK_a) { velocity.x = 0; }
		if (e.key.keysym.sym == SDLK_d) { velocity.x = 0; }
	}

	if (e.type == SDL_MOUSEMOTION)
	{
		yaw += (float)e.motion.xrel / 200.0f;
		pitch -= (float)e.motion.yrel / 200.0f;
	}

	if (e.type == SDL_MOUSEWHEEL)
	{
		if (e.wheel.y > 0)
		{
			if (cameraSpeed < 2.0f)
			{
				cameraSpeed += 0.05;
			}
		}
		else if (e.wheel.y < 0)
		{
			if (cameraSpeed > 0.0f)
			{
				cameraSpeed -= 0.05;
			}
		}
	}
}

glm::mat4 Camera::GetViewMatrix()
{
	glm::mat4 cameraTranslation = glm::translate(glm::mat4(1.0f), position);
	glm::mat4 cameraRotation = GetRotationMatrix();
	return glm::inverse(cameraTranslation * cameraRotation);
}

glm::mat4 Camera::GetRotationMatrix()
{
	glm::quat pitchRotation = glm::angleAxis(pitch, glm::vec3{ 1.0f, 0.0f, 0.0f });
	glm::quat yawRotation = glm::angleAxis(yaw, glm::vec3{ 0.0f, -1.0f, 0.0f });

	return glm::toMat4(yawRotation) * glm::toMat4(pitchRotation);
}