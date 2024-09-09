//> includes
#include "vk_engine.h"

#include <sdl2/SDL.h>
#include <sdl2/SDL_vulkan.h>

#include <vkbootstrap/VkBootstrap.h>

#define VMA_IMPLEMENTATION

#define VMA_LEAK_LOG_FORMAT(format, ...) do { \
       printf((format), __VA_ARGS__); \
       printf("\n"); \
   } while(false)

#include <vma/vk_mem_alloc.h>

#include <imgui/imgui.h>
#include <imgui/imgui_impl_sdl2.h>
#include <imgui/imgui_impl_vulkan.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "vk_initializers.h"
#include "vk_types.h"
#include "vk_images.h"
#include "vk_pipelines.h"

#include <chrono>
#include <thread>

VulkanEngine* loadedEngine = nullptr;

VulkanEngine& VulkanEngine::Get() { return *loadedEngine; }

constexpr bool useValidationLayers = true;
void VulkanEngine::Init()
{
    // only one engine initialization is allowed with the application.
    assert(loadedEngine == nullptr);
    loadedEngine = this;

    // We initialize SDL and create a window with it.
    SDL_Init(SDL_INIT_VIDEO);

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);

    window = SDL_CreateWindow(
        "Vulkan Engine",
        SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED,
        windowExtent.width,
        windowExtent.height,
        window_flags);

    InitVulkan();

    InitSwapchain();

    InitCommands();

    InitSyncStructures();

    InitDescriptors();

    InitPipelines();

    InitImGui();

    InitDefaultData();

    isInitialized = true;

    mainCamera.velocity = glm::vec3(0.0f);
    mainCamera.position = glm::vec3(30.f, -00.f, -085.f);
    mainCamera.position = glm::vec3(0.f, 0.f, 0.f);

    mainCamera.pitch = 0;
    mainCamera.yaw = 0;

    std::string structurePath = { "resources/sponza.glb" };
    auto structureFile = LoadGltf(this, structurePath);

    assert(structureFile.has_value());

    loadedScenes["structure"] = *structureFile;

    std::string cubePath = { "resources/cube.glb" };
    auto cubeFile = LoadGltf(this, cubePath);

    assert(cubeFile.has_value());

    loadedScenes["cube"] = *cubeFile;
}

void VulkanEngine::Cleanup()
{
    if (isInitialized) {
        vkDeviceWaitIdle(device);

        loadedScenes.clear();

        mainDeletionQueue.Flush();

        for (int i = 0; i < FRAME_OVERLAP; i++)
        {
            vkDestroyCommandPool(device, frames[i].commandPool, nullptr);

            vkDestroyFence(device, frames[i].renderFence, nullptr);
            vkDestroySemaphore(device, frames[i].renderSemaphore, nullptr);
            vkDestroySemaphore(device, frames[i].swapchainSemaphore, nullptr);

        }

        DestroySwapchain();

        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyDevice(device, nullptr);

        vkb::destroy_debug_utils_messenger(instance, debugMessenger);
        vkDestroyInstance(instance, nullptr);

        SDL_DestroyWindow(window);
    }

    // clear engine pointer
    loadedEngine = nullptr;
}

void VulkanEngine::Draw()
{
    drawExtent.height = std::min(swapchainExtent.height, drawImage.imageExtent.height) * renderScale;
    drawExtent.width = std::min(swapchainExtent.width, drawImage.imageExtent.width) * renderScale;

    UpdateScene();

    // wait until gpu has finished last frame, 1 sec timeout
    VK_CHECK(vkWaitForFences(device, 1, &GetCurrentFrame().renderFence, true, 1000000000));

    GetCurrentFrame().deletionQueue.Flush();
    GetCurrentFrame().frameDescriptors.ClearPools(device);

    VK_CHECK(vkResetFences(device, 1, &GetCurrentFrame().renderFence));

    uint32_t swapchainImageIndex;
    VkResult e = vkAcquireNextImageKHR(device, swapchain, 1000000000, GetCurrentFrame().swapchainSemaphore, nullptr, &swapchainImageIndex);
    if (e == VK_ERROR_OUT_OF_DATE_KHR)
    {
        resizeRequested = true;
        return;
    }

    VkCommandBuffer cmd = GetCurrentFrame().mainCommandBuffer;

    VK_CHECK(vkResetCommandBuffer(cmd, 0));

    VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

    // transition draw image into general layoutn to write into it

    vkutil::TransititionImage(cmd, colorImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

    vkutil::ClearImage(cmd, colorImage.image, VK_IMAGE_LAYOUT_GENERAL, VkClearColorValue{ 0.51f, 0.89f, 1.0f });

    vkutil::TransititionImage(cmd, drawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkutil::TransititionImage(cmd, colorImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    vkutil::TransititionImage(cmd, depthImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    vkutil::TransititionImage(cmd, depthCubemapImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    DrawDepthMap(cmd);

    UpdateSceneNonShadow();

    DrawSkybox(cmd);

    vkutil::TransititionImage(cmd, depthCubemapImage.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

    DrawGeometry(cmd);

    //DrawParticles(cmd);

    vkutil::TransititionImage(cmd, colorImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

    if (engineSettings.msaaSamples == VK_SAMPLE_COUNT_1_BIT)
    {
        vkutil::CopyImageToImage(cmd, colorImage.image, drawImage.image, drawExtent, drawExtent);
    }
    else
    {
        vkutil::ResolveImage(cmd, colorImage.image, drawImage.image, drawImage.imageExtent);
    }

    // transition draw image and swapchain image into correct transfer layouts
    vkutil::TransititionImage(cmd, drawImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    vkutil::TransititionImage(cmd, swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkutil::CopyImageToImage(cmd, drawImage.image, swapchainImages[swapchainImageIndex], drawExtent, swapchainExtent);

    vkutil::TransititionImage(cmd, swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

    DrawImGui(cmd, swapchainImageViews[swapchainImageIndex]);

    vkutil::TransititionImage(cmd, swapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    // finalize command buffer
    VK_CHECK(vkEndCommandBuffer(cmd));
    //

    VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info(cmd);

    VkSemaphoreSubmitInfo waitInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR, GetCurrentFrame().swapchainSemaphore);
    VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, GetCurrentFrame().renderSemaphore);

    VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo, &signalInfo, &waitInfo);

    VK_CHECK(vkQueueSubmit2(graphicsQueue, 1, &submit, GetCurrentFrame().renderFence));

    VkPresentInfoKHR presentInfo = {};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.pNext = nullptr;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.swapchainCount = 1;

    presentInfo.pWaitSemaphores = &GetCurrentFrame().renderSemaphore;
    presentInfo.waitSemaphoreCount = 1;

    presentInfo.pImageIndices = &swapchainImageIndex;

    VkResult presentResult = vkQueuePresentKHR(graphicsQueue, &presentInfo);
    if (presentResult == VK_ERROR_OUT_OF_DATE_KHR)
    {
        resizeRequested = true;
    }

    frameNumber++;
}

void VulkanEngine::Run()
{
    SDL_Event e;
    bool bQuit = false;

    // main loop
    while (!bQuit) {
        auto start = std::chrono::system_clock::now();
        // Handle events on queue
        while (SDL_PollEvent(&e) != 0) {
            // close the window when user alt-f4s or clicks the X button
            if (e.type == SDL_QUIT)
                bQuit = true;

            if (e.type == SDL_WINDOWEVENT) {
                if (e.window.event == SDL_WINDOWEVENT_MINIMIZED) {
                    stopRendering = true;
                }
                if (e.window.event == SDL_WINDOWEVENT_RESTORED) {
                    stopRendering = false;
                }
            }

            mainCamera.ProcessSDLEvent(e);

            // send sdl event to imgui for handling
            ImGui_ImplSDL2_ProcessEvent(&e);
        }

        if (resizeRequested)
        {
            ResizeSwapchain();
        }

        // do not draw if we are minimized
        if (stopRendering) {
            // throttle the speed to avoid the endless spinning
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL2_NewFrame(window);
        ImGui::NewFrame();

        /*
        if (ImGui::Begin("Settings"))
        {
            ImGui::SliderFloat("Render Scale", &renderScale, 0.3f, 1.0f);

            ComputeEffect& selected = backgroundEffects[currentBackgroundEffect];

            ImGui::Text("Selected Effect: ", selected.name);

            ImGui::SliderInt("Effect Index", &currentBackgroundEffect, 0, backgroundEffects.size() - 1);

            ImGui::InputFloat4("data1", (float*)&selected.data.data1);
            ImGui::InputFloat4("data2", (float*)&selected.data.data2);
            ImGui::InputFloat4("data3", (float*)&selected.data.data3);
            ImGui::InputFloat4("data4", (float*)&selected.data.data4);

            ImGui::End();
        }
        */
        if (ImGui::Begin("Settings"))
        {
            if (ImGui::CollapsingHeader("Rendering Statistics"))
            {
                ImGui::Text("Framerate %f fps", stats.framesPerSecond);
                ImGui::Text("Frametime %f ms", stats.frameTime);
                ImGui::Text("Draw Time %f ms", stats.meshDrawTime);
                ImGui::Text("Update Time %f ms", stats.sceneUpdateTime);
                ImGui::Text("Triangles %i", stats.triangleCount);
                ImGui::Text("Draws %i", stats.drawcallCount);
            }

            if (ImGui::CollapsingHeader("Scene Data"))
            {
                ImGui::Text("Camera Position: %f %f %f", mainCamera.position.x, mainCamera.position.y, mainCamera.position.z);
            }

            if (ImGui::CollapsingHeader("Lighting Data"))
            {
                ImGui::InputFloat4("Light (X, Y, Z, Stength)", (float*)&sceneData.lightPosition);
                ImGui::InputFloat4("Light Color (R, G, B, A)", (float*)&sceneData.lightColor);
                ImGui::InputFloat4("Ambient Color", (float*)&sceneData.ambientColor);
                ImGui::InputFloat("Attenuation Fall-Off", (float*)&sceneData.attenuationFallOff);
            }

            if (ImGui::CollapsingHeader("Shadow Data"))
            {
                ImGui::InputFloat("Shadow Draw Distance", (float*)&sceneData.shadowFarPlane);
                ImGui::InputFloat("Shadow Bias", (float*)&sceneData.shadowBias);
                ImGui::InputInt("Shadow Anti-Aliasing Samples ", (int*)&sceneData.shadowAASamples);
                ImGui::InputFloat("PCF Sampling Modifier", (float*)&sceneData.gridSamplingDiskModifier);
            }

            ImGui::SetWindowPos(ImVec2(0, 0), true);
        }
        ImGui::End();

        ImGui::Render();

        Draw();

        auto end = std::chrono::system_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        stats.frameTime = elapsed.count() / 1000.0f;
        stats.framesPerSecond = 1000.0f / stats.frameTime;
    }
}

void VulkanEngine::InitVulkan()
{
    vkb::InstanceBuilder builder;

    // create vulkan instance with basic debugging
    vkb::Result instRet = builder.set_app_name("Vulkan Engine")
        .request_validation_layers(useValidationLayers)
        .use_default_debug_messenger()
        .require_api_version(1, 3, 0)
        .build();

    vkb::Instance vkbInst = instRet.value();

    // grab the instance
    instance = vkbInst.instance;
    debugMessenger = vkbInst.debug_messenger;

    SDL_Vulkan_CreateSurface(window, instance, &surface);

    // vulkan 1.3 features
    VkPhysicalDeviceVulkan13Features features13{};
    features13.dynamicRendering = true;
    features13.synchronization2 = true;

    // vulkan 1.2 features
    VkPhysicalDeviceVulkan12Features features12{};
    features12.bufferDeviceAddress = true;
    features12.descriptorIndexing = true;
    features12.shaderOutputViewportIndex = true;
    features12.shaderOutputLayer = true;

    // vulkan features
    VkPhysicalDeviceFeatures features{};
    features.geometryShader = true;

    // select gpu
    vkb::PhysicalDeviceSelector selector(vkbInst);
    vkb::PhysicalDevice selectedPhysicalDevice = selector
        .set_minimum_version(1, 3)
        .set_required_features_13(features13)
        .set_required_features_12(features12)
        .set_required_features(features)
        .set_surface(surface)
        .select()
        .value();

    selectedPhysicalDevice.enable_extension_if_present(VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME); // for debugging 

    vkb::DeviceBuilder deviceBuilder{ selectedPhysicalDevice };

    vkb::Device vkbDevice = deviceBuilder.build().value();

    // grab the device
    device = vkbDevice.device; 
    physicalDevice = selectedPhysicalDevice.physical_device;

    // get a graphics queue
    graphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
    graphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();

   // initialize memory allocator 
    VmaAllocatorCreateInfo allocatorInfo = {};
    allocatorInfo.physicalDevice = physicalDevice;
    allocatorInfo.device = device;
    allocatorInfo.instance = instance;
    allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
    vmaCreateAllocator(&allocatorInfo, &allocator);

    mainDeletionQueue.PushFunction([&]() {
        vmaDestroyAllocator(allocator);
    });
}

void VulkanEngine::InitSwapchain()
{

    CreateSwapchain(windowExtent.width, windowExtent.height);

    // set draw image size to match window
    VkExtent3D drawImageExtent =
    {
        windowExtent.width,
        windowExtent.height,
        1
    };

    // setting draw format to 32 bit float
    drawImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    drawImage.imageExtent = drawImageExtent;

    VkImageUsageFlags drawImageUsages{};
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT;
    drawImageUsages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    VkImageCreateInfo rimgInfo = vkinit::image_create_info(drawImage.imageFormat, drawImageUsages, drawImageExtent, VK_SAMPLE_COUNT_1_BIT);
    VmaAllocationCreateInfo rimgAllocInfo = {};
    rimgAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    rimgAllocInfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vmaCreateImage(allocator, &rimgInfo, &rimgAllocInfo, &drawImage.image, &drawImage.allocation, nullptr);
    VkImageViewCreateInfo rViewInfo = vkinit::imageview_create_info(drawImage.imageFormat, drawImage.image, VK_IMAGE_ASPECT_COLOR_BIT);
    VK_CHECK(vkCreateImageView(device, &rViewInfo, nullptr, &drawImage.imageView));

    depthImage.imageFormat = VK_FORMAT_D32_SFLOAT;
    depthImage.imageExtent = drawImageExtent;
    VkImageUsageFlags depthImageUsages{};
    depthImageUsages |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthImageUsages |= VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageCreateInfo dimgInfo = vkinit::image_create_info(depthImage.imageFormat, depthImageUsages, drawImageExtent, engineSettings.msaaSamples);
    vmaCreateImage(allocator, &dimgInfo, &rimgAllocInfo, &depthImage.image, &depthImage.allocation, nullptr);
    VkImageViewCreateInfo dviewInfo = vkinit::imageview_create_info(depthImage.imageFormat, depthImage.image, VK_IMAGE_ASPECT_DEPTH_BIT);
    VK_CHECK(vkCreateImageView(device, &dviewInfo, nullptr, &depthImage.imageView));

    // depth cubemap (for shadow mapping)
    VkExtent3D cubemapImageExtent =
    {
        depthCubemapSize,
        depthCubemapSize,
        1
    };

    depthCubemapImage.imageFormat = VK_FORMAT_D16_UNORM;
    depthCubemapImage.imageExtent = cubemapImageExtent;
    dimgInfo = vkinit::cubemap_create_info(depthCubemapImage.imageFormat, depthImageUsages, cubemapImageExtent, VK_SAMPLE_COUNT_1_BIT);
    vmaCreateImage(allocator, &dimgInfo, &rimgAllocInfo, &depthCubemapImage.image, &depthCubemapImage.allocation, nullptr);
    dviewInfo = vkinit::cubemap_imageview_create_info(depthCubemapImage.imageFormat, depthCubemapImage.image, VK_IMAGE_ASPECT_DEPTH_BIT);
    VK_CHECK(vkCreateImageView(device, &dviewInfo, nullptr, &depthCubemapImage.imageView));

    // setting draw format to 32 bit float
    colorImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    colorImage.imageExtent = drawImageExtent;

    VkImageUsageFlags colorImageUsages{};
    colorImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    colorImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    colorImageUsages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    VkImageCreateInfo colorImgInfo = vkinit::image_create_info(colorImage.imageFormat, colorImageUsages, drawImageExtent, engineSettings.msaaSamples);
    VmaAllocationCreateInfo colorImgAllocInfo = {};
    colorImgAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    colorImgAllocInfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vmaCreateImage(allocator, &colorImgInfo, &colorImgAllocInfo, &colorImage.image, &colorImage.allocation, nullptr);
    VkImageViewCreateInfo colorViewInfo = vkinit::imageview_create_info(colorImage.imageFormat, colorImage.image, VK_IMAGE_ASPECT_COLOR_BIT);
    VK_CHECK(vkCreateImageView(device, &colorViewInfo, nullptr, &colorImage.imageView));


    mainDeletionQueue.PushFunction([=]() {
        vkDestroyImageView(device, drawImage.imageView, nullptr);
        vmaDestroyImage(allocator, drawImage.image, drawImage.allocation);

        vkDestroyImageView(device, depthImage.imageView, nullptr);
        vmaDestroyImage(allocator, depthImage.image, depthImage.allocation);

        vkDestroyImageView(device, depthCubemapImage.imageView, nullptr);
        vmaDestroyImage(allocator, depthCubemapImage.image, depthCubemapImage.allocation);

        vkDestroyImageView(device, colorImage.imageView, nullptr);
        vmaDestroyImage(allocator, colorImage.image, colorImage.allocation);

    });

}

void VulkanEngine::InitCommands()
{
    VkCommandPoolCreateInfo commandPoolInfo = vkinit::command_pool_create_info(graphicsQueueFamily, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

    for (int i = 0; i < FRAME_OVERLAP; i++)
    {
        VK_CHECK(vkCreateCommandPool(device, &commandPoolInfo, nullptr, &frames[i].commandPool));

        // allocate default command buffer uses for rendering
        VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(frames[i].commandPool, 1);

        VK_CHECK(vkAllocateCommandBuffers(device, &cmdAllocInfo, &frames[i].mainCommandBuffer));
    }

    VK_CHECK(vkCreateCommandPool(device, &commandPoolInfo, nullptr, &immCommandPool));

    VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(immCommandPool, 1);

    VK_CHECK(vkAllocateCommandBuffers(device, &cmdAllocInfo, &immCommandBuffer));

    mainDeletionQueue.PushFunction([=]()
        {
            vkDestroyCommandPool(device, immCommandPool, nullptr);
        });
}

void VulkanEngine::InitSyncStructures()
{
    // create sync strucues, 1 fence and 2 semaphores

    VkFenceCreateInfo fenceCreateInfo = vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);
    VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();

    for (int i = 0; i < FRAME_OVERLAP; i++)
    {
        VK_CHECK(vkCreateFence(device, &fenceCreateInfo, nullptr, &frames[i].renderFence));

        VK_CHECK(vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &frames[i].swapchainSemaphore));
        VK_CHECK(vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &frames[i].renderSemaphore));
    }

    VK_CHECK(vkCreateFence(device, &fenceCreateInfo, nullptr, &immFence));
    mainDeletionQueue.PushFunction([=]() { vkDestroyFence(device, immFence, nullptr); });
}

void VulkanEngine::CreateSwapchain(uint32_t width, uint32_t height)
{
    vkb::SwapchainBuilder swapchainBuilder{ physicalDevice, device, surface };

    VkColorSpaceKHR colorSpace;
    if (engineSettings.hdrOn == true)
    {
        swapchainImageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        colorSpace = VK_COLOR_SPACE_HDR10_HLG_EXT;
    }
    else
    {
        swapchainImageFormat = VK_FORMAT_B8G8R8A8_UNORM;
        colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    }

    vkb::Swapchain vkbSwapchain = swapchainBuilder
        //.use_default_format_selection()
        .set_desired_format(VkSurfaceFormatKHR{ .format = swapchainImageFormat, .colorSpace = colorSpace })
        //use vsync present mode
        .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR)
        .set_desired_extent(width, height)
        .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
        .build()
        .value();

    swapchainExtent = vkbSwapchain.extent;
    // store swapchain and images
    swapchain = vkbSwapchain.swapchain;
    swapchainImages = vkbSwapchain.get_images().value();
    swapchainImageViews = vkbSwapchain.get_image_views().value();
}

void VulkanEngine::DestroySwapchain()
{
    vkDestroySwapchainKHR(device, swapchain, nullptr);

    for (int i = 0; i < swapchainImageViews.size(); i++) {

        vkDestroyImageView(device, swapchainImageViews[i], nullptr);
    }
}

void VulkanEngine::InitDescriptors()
{
    // TODO: Work out what draw image actually needs 

    std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> sizes =
    {
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1},
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 }
    };

    globalDescriptorAllocator.InitPools(device, 10, sizes);

    {
        DescriptorLayoutBuilder builder;
        builder.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        builder.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        gpuSceneDataDescriptorLayout = builder.Build(device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
    }

    DescriptorWriter writer;
    writer.WriteImage(0, drawImage.imageView, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);

    mainDeletionQueue.PushFunction([&]() {
        vkDestroyDescriptorSetLayout(device, gpuSceneDataDescriptorLayout, nullptr);
        });

    for (int i = 0; i < FRAME_OVERLAP; i++)
    {
        std::vector<DescriptorAllocatorGrowable::PoolSizeRatio> frameSizes =
        {
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 3},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 3},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4},
        };

        frames[i].frameDescriptors = DescriptorAllocatorGrowable{};
        frames[i].frameDescriptors.InitPools(device, 1000, frameSizes);

        mainDeletionQueue.PushFunction([&, i]() // vma exception from shutdown caused by this 
            {
                frames[i].frameDescriptors.DestroyPools(device);
            });
    }

}

void VulkanEngine::InitPipelines()
{
    metalRoughMaterial.BuildPipelines(this);
    InitSkyboxPipeline();
    InitDepthMapPipeline();
    InitParticlePipeline();
}

void VulkanEngine::ImmediateSubmit(std::function<void(VkCommandBuffer cmd)>&& function)
{
    VK_CHECK(vkResetFences(device, 1, &immFence));
    VK_CHECK(vkResetCommandBuffer(immCommandBuffer, 0));

    VkCommandBuffer cmd = immCommandBuffer;

    VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);

    VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));

    function(cmd);

    VK_CHECK(vkEndCommandBuffer(cmd));

    VkCommandBufferSubmitInfo cmdInfo = vkinit::command_buffer_submit_info(cmd);
    VkSubmitInfo2 submit = vkinit::submit_info(&cmdInfo, nullptr, nullptr);

    VK_CHECK(vkQueueSubmit2(graphicsQueue, 1, &submit, immFence));

    VK_CHECK(vkWaitForFences(device, 1, &immFence, true, 9999999999));
}

void VulkanEngine::InitImGui()
{
    VkDescriptorPoolSize poolSizes[] = { { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 } };


    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 1000;
    poolInfo.poolSizeCount = (uint32_t)std::size(poolSizes);
    poolInfo.pPoolSizes = poolSizes;

    VkDescriptorPool imguiPool;
    VK_CHECK(vkCreateDescriptorPool(device, &poolInfo, nullptr, &imguiPool));

    ImGui::CreateContext();

    ImGui_ImplSDL2_InitForVulkan(window);

    ImGui_ImplVulkan_InitInfo initInfo = {};
    initInfo.Instance = instance;
    initInfo.PhysicalDevice = physicalDevice;
    initInfo.Device = device;
    initInfo.Queue = graphicsQueue;
    initInfo.DescriptorPool = imguiPool;
    initInfo.MinImageCount = 3;
    initInfo.ImageCount = 3;
    initInfo.UseDynamicRendering = true;
    initInfo.ColorAttachmentFormat = VK_FORMAT_B8G8R8A8_UNORM;

    initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;

    ImGui_ImplVulkan_Init(&initInfo, VK_NULL_HANDLE);

    ImmediateSubmit([&](VkCommandBuffer cmd) { ImGui_ImplVulkan_CreateFontsTexture(cmd); });

    ImGui_ImplVulkan_DestroyFontUploadObjects();

    mainDeletionQueue.PushFunction([=]()
        {
            vkDestroyDescriptorPool(device, imguiPool, nullptr);
            ImGui_ImplVulkan_Shutdown();
        });
}

void VulkanEngine::DrawImGui(VkCommandBuffer cmd, VkImageView targetImageView)
{
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(targetImageView, nullptr, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderingInfo renderInfo = vkinit::rendering_info(swapchainExtent, &colorAttachment, nullptr);

    vkCmdBeginRendering(cmd, &renderInfo);

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);

    vkCmdEndRendering(cmd);
}


void VulkanEngine::DrawGeometry(VkCommandBuffer cmd)
{
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(colorImage.imageView, nullptr, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(depthImage.imageView, true, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkRenderingInfo renderingInfo = vkinit::rendering_info(drawExtent, &colorAttachment, &depthAttachment);

    stats.drawcallCount = 0;
    stats.triangleCount = 0;
    auto start = std::chrono::system_clock::now();

    std::vector<uint32_t> opaqueDraws;
    opaqueDraws.reserve(mainDrawContext.OpaqueSurfaces.size());

    for (uint32_t i = 0; i < mainDrawContext.OpaqueSurfaces.size(); i++)
    {
        /* (Causes fps reduciton) TODO: Work out why
        if (IsVisible(mainDrawContext.OpaqueSurfaces[i], sceneData.viewproj))
        {
            opaqueDraws.push_back(i);
        }
        */
        opaqueDraws.push_back(i);
    }

    /* (Causes fps reduciton) TODO: Work out why
    std::sort(opaqueDraws.begin(), opaqueDraws.end(), [&](const auto& iA, const auto& iB) 
    {
        const RenderObject& A = mainDrawContext.OpaqueSurfaces[iA];
        const RenderObject& B = mainDrawContext.OpaqueSurfaces[iB];
        if (A.material == B.material) 
        {
            return A.indexBuffer < B.indexBuffer;
        }
        else 
        {
            return A.material < B.material;
        }
    });
    */

    vkCmdBeginRendering(cmd, &renderingInfo);

    //allocate a new uniform buffer for the scene data
    AllocatedBuffer gpuSceneDataBuffer = CreateBuffer(sizeof(GPUSceneData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

    //add it to the deletion queue of this frame so it gets deleted once its been used
    GetCurrentFrame().deletionQueue.PushFunction([=, this]() 
        {
            DestroyBuffer(gpuSceneDataBuffer);
        });

    //write the buffer
    GPUSceneData* sceneUniformData = (GPUSceneData*)gpuSceneDataBuffer.allocation->GetMappedData();
    *sceneUniformData = sceneData;

    //create a descriptor set that binds that buffer and update it
    VkDescriptorSet globalDescriptor = GetCurrentFrame().frameDescriptors.Allocate(device, gpuSceneDataDescriptorLayout);

    DescriptorWriter writer;
    writer.WriteBuffer(0, gpuSceneDataBuffer.buffer, sizeof(GPUSceneData), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    writer.WriteImage(1, depthCubemapImage.imageView, defaultSamplerNearest, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.UpdateSet(device, globalDescriptor);

    MaterialPipeline* lastPipeline = nullptr;
    MaterialInstance* lastMaterial = nullptr;
    VkBuffer lastIndexBuffer = VK_NULL_HANDLE;

    auto draw = [&](const RenderObject& r) 
    {
        if (r.material != lastMaterial) 
        {
            lastMaterial = r.material;
            //rebind pipeline and descriptors if the material changed
            if (r.material->pipeline != lastPipeline) 
            {

                lastPipeline = r.material->pipeline;
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.material->pipeline->pipeline);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.material->pipeline->layout, 0, 1, &globalDescriptor, 0, nullptr);

                VkViewport viewport = {};
                viewport.x = 0.0f;
                viewport.y = 0.0f;
                viewport.width = windowExtent.width;
                viewport.height = windowExtent.height;
                viewport.minDepth = 0.0f;
                viewport.maxDepth = 1.0f;

                vkCmdSetViewport(cmd, 0, 1, &viewport);

                VkRect2D scissor = {};
                scissor.offset.x = 0.0f;
                scissor.offset.y = 0.0f;
                scissor.extent.width = viewport.width;
                scissor.extent.height = viewport.height;

                vkCmdSetScissor(cmd, 0, 1, &scissor);
            }

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.material->pipeline->layout, 1, 1, &r.material->materialSet, 0, nullptr);
        }
        //rebind index buffer if needed
        if (r.indexBuffer != lastIndexBuffer) 
        {
            lastIndexBuffer = r.indexBuffer;
            vkCmdBindIndexBuffer(cmd, r.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        }

        GPUDrawPushConstants pushConstants;
        pushConstants.renderMatrix = r.transform;
        pushConstants.vertexBuffer = r.vertexBufferAddress;

        vkCmdPushConstants(cmd, r.material->pipeline->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GPUDrawPushConstants), &pushConstants);

        vkCmdDrawIndexed(cmd, r.indexCount, 1, r.firstIndex, 0, 0);
        //stats
        stats.drawcallCount += 1;
        stats.triangleCount += r.indexCount / 3;
    };

    for (auto& r : opaqueDraws) 
    {
        draw(mainDrawContext.OpaqueSurfaces[r]);
    }


    for (auto& r : mainDrawContext.TransparentSurfaces)
    {
        draw(r);
    }

    vkCmdEndRendering(cmd);

    mainDrawContext.OpaqueSurfaces.clear();
    mainDrawContext.TransparentSurfaces.clear();

    auto end = std::chrono::system_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    stats.meshDrawTime = elapsed.count() / 1000.0f;
}

AllocatedBuffer VulkanEngine::CreateBuffer(size_t allocSize, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage)
{
    VkBufferCreateInfo bufferInfo = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bufferInfo.pNext = nullptr;
    bufferInfo.size = allocSize;

    bufferInfo.usage = usage;

    VmaAllocationCreateInfo vmaallocInfo = {};
    vmaallocInfo.usage = memoryUsage;
    vmaallocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    AllocatedBuffer newBuffer;

    VK_CHECK(vmaCreateBuffer(allocator, &bufferInfo, &vmaallocInfo, &newBuffer.buffer, &newBuffer.allocation, &newBuffer.info));

    return newBuffer;
}

void VulkanEngine::DestroyBuffer(const AllocatedBuffer& buffer)
{
    vmaDestroyBuffer(allocator, buffer.buffer, buffer.allocation);
}

GPUMeshBuffers VulkanEngine::UploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices)
{
    const size_t vertexBufferSize = vertices.size() * sizeof(Vertex);
    const size_t indexBufferSize = indices.size() * sizeof(uint32_t);

    GPUMeshBuffers newSurface;

    newSurface.vertexBuffer = CreateBuffer(vertexBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

    VkBufferDeviceAddressInfo deviceAdressInfo{ .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,.buffer = newSurface.vertexBuffer.buffer };
    newSurface.vertexBufferAddress = vkGetBufferDeviceAddress(device, &deviceAdressInfo);

    newSurface.indexBuffer = CreateBuffer(indexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

    AllocatedBuffer staging = CreateBuffer(vertexBufferSize + indexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

    void* data = staging.allocation->GetMappedData();

    // copy vertex buffer
    memcpy(data, vertices.data(), vertexBufferSize);
    // copy index buffer
    memcpy((char*)data + vertexBufferSize, indices.data(), indexBufferSize);

    ImmediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy vertexCopy{ 0 };
        vertexCopy.dstOffset = 0;
        vertexCopy.srcOffset = 0;
        vertexCopy.size = vertexBufferSize;

        vkCmdCopyBuffer(cmd, staging.buffer, newSurface.vertexBuffer.buffer, 1, &vertexCopy);

        VkBufferCopy indexCopy{ 0 };
        indexCopy.dstOffset = 0;
        indexCopy.srcOffset = vertexBufferSize;
        indexCopy.size = indexBufferSize;

        vkCmdCopyBuffer(cmd, staging.buffer, newSurface.indexBuffer.buffer, 1, &indexCopy);
        });

    DestroyBuffer(staging);
        
    return newSurface;
}

GPUParticleBuffers VulkanEngine::UploadParticles(std::span<ParticleGPUData> particlesGPUData)
{
    const size_t bufferSize = particlesGPUData.size() * sizeof(ParticleGPUData);

    GPUParticleBuffers newParticles;

    newParticles.particleBuffer = CreateBuffer(bufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

    VkBufferDeviceAddressInfo deviceAdressInfo{ .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,.buffer = newParticles.particleBuffer.buffer };
    newParticles.particleBufferAddress = vkGetBufferDeviceAddress(device, &deviceAdressInfo);

    AllocatedBuffer staging = CreateBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

    void* data = staging.allocation->GetMappedData();
    
    // copy vertex buffer
    memcpy(data, particlesGPUData.data(), bufferSize);

    ImmediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy particleCopy{ 0 };
        particleCopy.dstOffset = 0;
        particleCopy.srcOffset = 0;
        particleCopy.size = bufferSize;

        vkCmdCopyBuffer(cmd, staging.buffer, newParticles.particleBuffer.buffer, 1, &particleCopy);
        });

    DestroyBuffer(staging);

    newParticles.bufferSize = bufferSize;

    return newParticles;
}

void VulkanEngine::UpdateParticles(GPUParticleBuffers& buffer, std::span<ParticleGPUData> particlesGPUData)
{
    AllocatedBuffer staging = CreateBuffer(buffer.bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);

    void* data = staging.allocation->GetMappedData();

    // copy vertex buffer
    memcpy(data, particlesGPUData.data(), buffer.bufferSize);

    ImmediateSubmit([&](VkCommandBuffer cmd) {
        VkBufferCopy particleCopy{ 0 };
        particleCopy.dstOffset = 0;
        particleCopy.srcOffset = 0;
        particleCopy.size = buffer.bufferSize;

        vkCmdCopyBuffer(cmd, staging.buffer, buffer.particleBuffer.buffer, 1, &particleCopy);
        });

    DestroyBuffer(staging);
}

void VulkanEngine::InitDefaultData() {

    uint32_t white = glm::packUnorm4x8(glm::vec4(1, 1, 1, 1));
    whiteImage = CreateImage((void*)&white, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT);

    uint32_t grey = glm::packUnorm4x8(glm::vec4(0.66f, 0.66f, 0.66f, 1));
    greyImage = CreateImage((void*)&grey, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT);

    uint32_t black = glm::packUnorm4x8(glm::vec4(0, 0, 0, 0));
    blackImage = CreateImage((void*)&black, VkExtent3D{ 1, 1, 1 }, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT);

    uint32_t magenta = glm::packUnorm4x8(glm::vec4(1, 0, 1, 1));
    std::array<uint32_t, 16 * 16 > pixels; //for 16x16 checkerboard texture
    for (int x = 0; x < 16; x++) {
        for (int y = 0; y < 16; y++) {
            pixels[y * 16 + x] = ((x % 2) ^ (y % 2)) ? magenta : black;
        }
    }
    errorImage = CreateImage(pixels.data(), VkExtent3D{ 16, 16, 1 }, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT);

    VkSamplerCreateInfo sampler = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };

    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;

    vkCreateSampler(device, &sampler, nullptr, &defaultSamplerNearest);

    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    vkCreateSampler(device, &sampler, nullptr, &defaultSamplerLinear);

    GLTFMetallicRoughness::MaterialResources materialResources;

    materialResources.colorImage = whiteImage;
    materialResources.colorSampler = defaultSamplerLinear;
    materialResources.metallicRoughnessImage = whiteImage;
    materialResources.metallicRoughnessSampler = defaultSamplerLinear;
    materialResources.normalImage = whiteImage;
    materialResources.normalSampler = defaultSamplerLinear;
    materialResources.occlusionImage = whiteImage;
    materialResources.occlusionSampler = defaultSamplerLinear;
    materialResources.emissionImage = whiteImage;
    materialResources.emissionSampler = defaultSamplerLinear;

    //set the uniform buffer for the material data
    AllocatedBuffer materialConstants = CreateBuffer(sizeof(GLTFMetallicRoughness::MaterialConstants), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

    //write the buffer
    GLTFMetallicRoughness::MaterialConstants* sceneUniformData = (GLTFMetallicRoughness::MaterialConstants*)materialConstants.allocation->GetMappedData();
    sceneUniformData->colorFactors = glm::vec4{ 1,1,1,1 };
    sceneUniformData->metalRoughFactors = glm::vec4{ 1,0.5,0,0 };

    mainDeletionQueue.PushFunction([=, this]() 
        {
            DestroyBuffer(materialConstants);
        });

    materialResources.dataBuffer = materialConstants.buffer;
    materialResources.dataBufferOffset = 0;

    defaultData = metalRoughMaterial.WriteMaterial(device, MaterialPass::MainColor, materialResources, globalDescriptorAllocator);

    sceneData.ambientColor = glm::vec4(0.01f);
    sceneData.lightColor = glm::vec4(0.94f, 0.75f, 0.44f, 1.0f);
    sceneData.lightPosition = glm::vec4(0.0f, 1.0f, 0.0f, 25.0f);
    sceneData.attenuationFallOff = 2.0f;
    sceneData.shadowFarPlane = 25.0f;
    sceneData.shadowBias = 0.15;
    sceneData.shadowAASamples = 20;
    sceneData.gridSamplingDiskModifier = 1.0;

    std::array<Vertex, 36>  skyboxCubeVertices;
    skyboxCubeVertices[0].position =  { -0.5f, -0.5f, -0.5f };
    skyboxCubeVertices[1].position = { 0.5f, -0.5f, -0.5f };
    skyboxCubeVertices[2].position = { 0.5f,  0.5f, -0.5f };
    skyboxCubeVertices[3].position = { 0.5f,  0.5f, -0.5f };
    skyboxCubeVertices[4].position = { -0.5f,  0.5f, -0.5f };
    skyboxCubeVertices[5].position = { -0.5f, -0.5f, -0.5f };
    skyboxCubeVertices[6].position =  { -0.5f, -0.5f,  0.5f };
    skyboxCubeVertices[7].position = { 0.5f, -0.5f,  0.5f };
    skyboxCubeVertices[8].position = { 0.5f,  0.5f,  0.5f };
    skyboxCubeVertices[9].position = { 0.5f,  0.5f,  0.5f };
    skyboxCubeVertices[10].position = { -0.5f,  0.5f,  0.5f };
    skyboxCubeVertices[11].position = { -0.5f, -0.5f,  0.5f };
    skyboxCubeVertices[12].position = { -0.5f,  0.5f,  0.5f };
    skyboxCubeVertices[13].position = { -0.5f,  0.5f, -0.5f };
    skyboxCubeVertices[14].position = { -0.5f, -0.5f, -0.5f };
    skyboxCubeVertices[15].position = { -0.5f, -0.5f, -0.5f };
    skyboxCubeVertices[16].position = { -0.5f, -0.5f,  0.5f };
    skyboxCubeVertices[17].position = { -0.5f,  0.5f,  0.5f };
    skyboxCubeVertices[18].position = { 0.5f,  0.5f,  0.5f };
    skyboxCubeVertices[19].position = { 0.5f,  0.5f, -0.5f };
    skyboxCubeVertices[20].position = { 0.5f, -0.5f, -0.5f };
    skyboxCubeVertices[21].position = { 0.5f, -0.5f, -0.5f };
    skyboxCubeVertices[22].position = { 0.5f, -0.5f,  0.5f };
    skyboxCubeVertices[23].position = { 0.5f,  0.5f,  0.5f };
    skyboxCubeVertices[24].position = { -0.5f, -0.5f, -0.5f };
    skyboxCubeVertices[25].position = { 0.5f, -0.5f, -0.5f };
    skyboxCubeVertices[26].position = { 0.5f, -0.5f,  0.5f };
    skyboxCubeVertices[27].position = { 0.5f, -0.5f,  0.5f };
    skyboxCubeVertices[28].position = { -0.5f, -0.5f,  0.5f };
    skyboxCubeVertices[29].position = { -0.5f, -0.5f, -0.5f };
    skyboxCubeVertices[30].position = { -0.5f,  0.5f, -0.5f };
    skyboxCubeVertices[31].position = { 0.5f,  0.5f, -0.5f };
    skyboxCubeVertices[32].position = { 0.5f,  0.5f,  0.5f };
    skyboxCubeVertices[33].position = { 0.5f,  0.5f,  0.5f };
    skyboxCubeVertices[34].position = { -0.5f,  0.5f,  0.5f };
    skyboxCubeVertices[35].position = { -0.5f,  0.5f, -0.5f };

    skyboxCubeVertices[0].uv_x = 0.0f;
    skyboxCubeVertices[1].uv_x = 1.0f;
    skyboxCubeVertices[2].uv_x = 1.0f;
    skyboxCubeVertices[3].uv_x = 1.0f;
    skyboxCubeVertices[4].uv_x = 0.0f;
    skyboxCubeVertices[5].uv_x = 0.0f;
    skyboxCubeVertices[6].uv_x = 0.0f;
    skyboxCubeVertices[7].uv_x = 1.0f;
    skyboxCubeVertices[8].uv_x = 1.0f;
    skyboxCubeVertices[9].uv_x = 1.0f;
    skyboxCubeVertices[10].uv_x = 0.0f;
    skyboxCubeVertices[11].uv_x = 0.0f;
    skyboxCubeVertices[12].uv_x = 1.0f;
    skyboxCubeVertices[13].uv_x = 1.0f;
    skyboxCubeVertices[14].uv_x = 0.0f;
    skyboxCubeVertices[15].uv_x = 0.0f;
    skyboxCubeVertices[16].uv_x = 0.0f;
    skyboxCubeVertices[17].uv_x = 1.0f;
    skyboxCubeVertices[18].uv_x = 1.0f;
    skyboxCubeVertices[19].uv_x = 1.0f;
    skyboxCubeVertices[20].uv_x = 0.0f;
    skyboxCubeVertices[21].uv_x = 0.0f;
    skyboxCubeVertices[22].uv_x = 0.0f;
    skyboxCubeVertices[23].uv_x = 1.0f;
    skyboxCubeVertices[24].uv_x = 0.0f;
    skyboxCubeVertices[25].uv_x = 1.0f;
    skyboxCubeVertices[26].uv_x = 1.0f;
    skyboxCubeVertices[27].uv_x = 1.0f;
    skyboxCubeVertices[28].uv_x = 0.0f;
    skyboxCubeVertices[29].uv_x = 0.0f;
    skyboxCubeVertices[30].uv_x = 0.0f;
    skyboxCubeVertices[31].uv_x = 1.0f;
    skyboxCubeVertices[32].uv_x = 1.0f;
    skyboxCubeVertices[33].uv_x = 1.0f;
    skyboxCubeVertices[34].uv_x = 0.0f;
    skyboxCubeVertices[35].uv_x = 0.0f;

    skyboxCubeVertices[0].uv_y =  0.0f;
    skyboxCubeVertices[1].uv_y =  0.0f;
    skyboxCubeVertices[2].uv_y =  1.0f;
    skyboxCubeVertices[3].uv_y =  1.0f;
    skyboxCubeVertices[4].uv_y =  1.0f;
    skyboxCubeVertices[5].uv_y =  0.0f;
    skyboxCubeVertices[6].uv_y =  0.0f;
    skyboxCubeVertices[7].uv_y =  0.0f;
    skyboxCubeVertices[8].uv_y =  1.0f;
    skyboxCubeVertices[9].uv_y =  1.0f;
    skyboxCubeVertices[10].uv_y = 1.0f;
    skyboxCubeVertices[11].uv_y = 0.0f;
    skyboxCubeVertices[12].uv_y = 0.0f;
    skyboxCubeVertices[13].uv_y = 1.0f;
    skyboxCubeVertices[14].uv_y = 1.0f;
    skyboxCubeVertices[15].uv_y = 1.0f;
    skyboxCubeVertices[16].uv_y = 0.0f;
    skyboxCubeVertices[17].uv_y = 0.0f;
    skyboxCubeVertices[18].uv_y = 0.0f;
    skyboxCubeVertices[19].uv_y = 1.0f;
    skyboxCubeVertices[20].uv_y = 1.0f;
    skyboxCubeVertices[21].uv_y = 1.0f;
    skyboxCubeVertices[22].uv_y = 0.0f;
    skyboxCubeVertices[23].uv_y = 0.0f;
    skyboxCubeVertices[24].uv_y = 1.0f;
    skyboxCubeVertices[25].uv_y = 1.0f;
    skyboxCubeVertices[26].uv_y = 0.0f;
    skyboxCubeVertices[27].uv_y = 0.0f;
    skyboxCubeVertices[28].uv_y = 0.0f;
    skyboxCubeVertices[29].uv_y = 1.0f;
    skyboxCubeVertices[30].uv_y = 1.0f;
    skyboxCubeVertices[31].uv_y = 1.0f;
    skyboxCubeVertices[32].uv_y = 0.0f;
    skyboxCubeVertices[33].uv_y = 0.0f;
    skyboxCubeVertices[34].uv_y = 0.0f;
    skyboxCubeVertices[35].uv_y = 1.0f;

    std::array<uint32_t, 36> skyboxCubeIndices =
    {
        // front and back
        0, 3, 2,
        2, 1, 0,
        4, 5, 6,
        6, 7 ,4,
        // left and right
        11, 8, 9,
        9, 10, 11,
        12, 13, 14,
        14, 15, 12,
        // bottom and top
        16, 17, 18,
        18, 19, 16,
        20, 21, 22,
        22, 23, 20
    };

    skyboxCube = UploadMesh(skyboxCubeIndices, skyboxCubeVertices);
    skyboxImage = LoadImage(this, "resources/textures/skybox2.hdr").value();

    mainDeletionQueue.PushFunction([=]() 
        {
            DestroyImage(skyboxImage);
            DestroyBuffer(skyboxCube.vertexBuffer);
            DestroyBuffer(skyboxCube.indexBuffer);
        });

    std::array<Vertex, 6>  particleVerticies;
    particleVerticies[0].position = { 0.0f, 0.5f, 0.0f };
    particleVerticies[1].position = { 0.0f, -0.5f, 0.0f };
    particleVerticies[2].position = { 1.0f, -0.5f, 0.0f };
    particleVerticies[3].position = { 0.0f, 0.5f, 0.0f };
    particleVerticies[4].position = { 1.0f, -0.5f, 0.0f };
    particleVerticies[5].position = { 1.0f, 0.5f, 0.0f };

    particleVerticies[0].uv_x = 0.0f;
    particleVerticies[1].uv_x = 0.0f;
    particleVerticies[2].uv_x = 1.0f;
    particleVerticies[3].uv_x = 0.0f;
    particleVerticies[4].uv_x = 1.0f;
    particleVerticies[5].uv_x = 1.0f;

    particleVerticies[0].uv_y = 0.0f;
    particleVerticies[1].uv_y = 1.0f;
    particleVerticies[2].uv_y = 1.0f;
    particleVerticies[3].uv_y = 0.0f;
    particleVerticies[4].uv_y = 1.0f;
    particleVerticies[5].uv_y = 0.0f;

    std::array<uint32_t, 6> particleIndices =
    {
        0, 1, 3,
        1, 2, 3
    };

    particleBillboard = UploadMesh(particleIndices, particleVerticies);
    particleEmitter = new ParticleEmitter(this, glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.5f, 0.0f, 0.5f), 200, 6, 9);
    particleSmokeImage = LoadTextureArray(this, "resources/textures/Cloud02_8x8.tga", 8, 8, 64).value();


    mainDeletionQueue.PushFunction([=]()
        {
            DestroyImage(particleSmokeImage);
            DestroyBuffer(particleBillboard.vertexBuffer);
            DestroyBuffer(particleBillboard.indexBuffer);
            DestroyBuffer(particleEmitter->particleBuffers.particleBuffer);
        });
}

void VulkanEngine::ResizeSwapchain()
{
    vkDeviceWaitIdle(device);

    DestroySwapchain();

    int width, height;
    SDL_GetWindowSize(window, &width, &height);
    windowExtent.width = width;
    windowExtent.height = height;

    CreateSwapchain(windowExtent.width, windowExtent.height);

    resizeRequested = false;
}

AllocatedImage VulkanEngine::CreateImage(VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped)
{
    AllocatedImage newImage;
    newImage.imageFormat = format;
    newImage.imageExtent = size;

    VkImageCreateInfo imageInfo = vkinit::image_create_info(format, usage, size, VK_SAMPLE_COUNT_1_BIT);
    if (mipmapped)
    {
        imageInfo.mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(size.width, size.height)))) + 1;
    }

    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    allocInfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VK_CHECK(vmaCreateImage(allocator, &imageInfo, &allocInfo, &newImage.image, &newImage.allocation, nullptr));

    VkImageAspectFlags  aspectFlag = VK_IMAGE_ASPECT_COLOR_BIT;
    if (format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM)
    {
        aspectFlag = VK_IMAGE_ASPECT_DEPTH_BIT;
    }

    VkImageViewCreateInfo viewInfo = vkinit::imageview_create_info(format, newImage.image, aspectFlag);
    viewInfo.subresourceRange.levelCount = imageInfo.mipLevels;

    VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &newImage.imageView));
    
    return newImage;
}

AllocatedImage VulkanEngine::CreateImage(void* data, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped)
{
    size_t dataSize = size.depth * size.width * size.height * 4;
    AllocatedBuffer uploadbuffer = CreateBuffer(dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

    memcpy(uploadbuffer.info.pMappedData, data, dataSize);

    AllocatedImage newImage = CreateImage(size, format, usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, mipmapped);

    ImmediateSubmit([&](VkCommandBuffer cmd) 
        {
        vkutil::TransititionImage(cmd, newImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

        VkBufferImageCopy copyRegion = {};
        copyRegion.bufferOffset = 0;
        copyRegion.bufferRowLength = 0;
        copyRegion.bufferImageHeight = 0;

        copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.imageSubresource.mipLevel = 0;
        copyRegion.imageSubresource.baseArrayLayer = 0;
        copyRegion.imageSubresource.layerCount = 1;
        copyRegion.imageExtent = size;

        // copy the buffer into the image
        vkCmdCopyBufferToImage(cmd, uploadbuffer.buffer, newImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
            &copyRegion);

        if (mipmapped)
        {
            vkutil::GenerateMipMaps(cmd, newImage.image, VkExtent2D{ newImage.imageExtent.width,newImage.imageExtent.height });
        }
        else
        {
            vkutil::TransititionImage(cmd, newImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        });

    DestroyBuffer(uploadbuffer);

    return newImage;
}

AllocatedImage VulkanEngine::CreateImageArray(VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped, uint32_t layerAmount)
{
    AllocatedImage newImage;
    newImage.imageFormat = format;
    newImage.imageExtent = size;

    VkImageCreateInfo imageInfo = vkinit::texture_array_create_info(format, usage, size, VK_SAMPLE_COUNT_1_BIT, layerAmount);
    if (mipmapped)
    {
        imageInfo.mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(size.width, size.height)))) + 1;
    }

    VmaAllocationCreateInfo allocInfo = {};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    allocInfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    VK_CHECK(vmaCreateImage(allocator, &imageInfo, &allocInfo, &newImage.image, &newImage.allocation, nullptr));

    VkImageAspectFlags  aspectFlag = VK_IMAGE_ASPECT_COLOR_BIT;

    VkImageViewCreateInfo viewInfo = vkinit::texture_array_imageview_create_info(format, newImage.image, aspectFlag, layerAmount);
    viewInfo.subresourceRange.levelCount = imageInfo.mipLevels;

    VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &newImage.imageView));

    return newImage;
}

AllocatedImage VulkanEngine::CreateImageArray(void* data, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped, uint32_t columnNum, uint32_t rowNum, uint32_t layerAmount)
{
    size_t dataSize = size.depth * size.width * size.height * 4;
    AllocatedBuffer uploadbuffer = CreateBuffer(dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

    memcpy(uploadbuffer.info.pMappedData, data, dataSize);


    size_t layerWidth = size.width / columnNum;
    size_t layerHeight = size.height / rowNum;

    VkExtent3D layerSize;
    layerSize.depth = size.depth;
    layerSize.width = layerWidth;
    layerSize.height = layerHeight;

    AllocatedImage newImage = CreateImageArray(layerSize, format, usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, mipmapped, layerAmount);

    for (uint32_t layer = 0; layer < layerAmount; layer++)
    {
        size_t layerX = layer % columnNum;
        size_t layerY = layer / columnNum;

        size_t x = layerX * layerWidth;
        size_t y = layerY * layerHeight;

        size_t offset = 4 * ((layerY * layerHeight * size.width) + (layerX * layerWidth)); // offset = (row × subImageHeight × width + column × subImageWidth) × bytesPerPixel

        ImmediateSubmit([&](VkCommandBuffer cmd)
            {
                vkutil::TransititionImage(cmd, newImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

                VkBufferImageCopy copyRegion = {};
                copyRegion.bufferOffset = offset;
                copyRegion.bufferRowLength = size.width;
                copyRegion.bufferImageHeight = size.height;

                copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                copyRegion.imageSubresource.mipLevel = 0;
                copyRegion.imageSubresource.baseArrayLayer = layer;
                copyRegion.imageSubresource.layerCount = 1;
                copyRegion.imageExtent.height = layerHeight;
                copyRegion.imageExtent.width = layerWidth;
                copyRegion.imageExtent.depth = 1;

                // copy the buffer into the image
                vkCmdCopyBufferToImage(cmd, uploadbuffer.buffer, newImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                    &copyRegion);

                if (mipmapped)
                {
                    vkutil::GenerateMipMaps(cmd, newImage.image, VkExtent2D{ newImage.imageExtent.width,newImage.imageExtent.height });
                }
                else
                {
                    vkutil::TransititionImage(cmd, newImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
            });
    }

    DestroyBuffer(uploadbuffer);

    return newImage;
}

void VulkanEngine::DestroyImage(const AllocatedImage& image)
{
    vkDestroyImageView(device, image.imageView, nullptr);
    vmaDestroyImage(allocator, image.image, image.allocation); 
}

void VulkanEngine::UpdateScene()
{
    auto start = std::chrono::system_clock::now();

    mainCamera.Update();

    glm::mat4 view = mainCamera.GetViewMatrix();

    glm::mat4 projection = glm::perspective(glm::radians(70.0f), (float)drawExtent.width / (float)drawExtent.height, nearPlane, farPlane); // (swap near and far values)

    projection[1][1] *= -1;

    sceneData.view = view;
    sceneData.proj = projection;
    sceneData.viewproj = projection * view;
    sceneData.viewPosition = mainCamera.position;

    loadedScenes["structure"]->Draw(glm::mat4{ 1.0f }, mainDrawContext);

    glm::mat4 shadowProj = glm::perspective(glm::radians(90.0f), (float)depthCubemapSize / (float)depthCubemapSize, nearPlane, sceneData.shadowFarPlane); // (swap near and far values)
    glm::vec3 lightPos = glm::vec3(sceneData.lightPosition.x, sceneData.lightPosition.y, sceneData.lightPosition.z);
    std::vector<glm::mat4> shadowMatricies;
    depthMapGeometryData.shadowMatricies[0] = shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f)); //TODO: This is causing an issue(Memory Overflow)
    depthMapGeometryData.shadowMatricies[1] = shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(-1.0f, 0.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f));
    depthMapGeometryData.shadowMatricies[2] = shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    depthMapGeometryData.shadowMatricies[3] = shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(0.0f, -1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
    depthMapGeometryData.shadowMatricies[4] = shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, -1.0f, 0.0f));
    depthMapGeometryData.shadowMatricies[5] = shadowProj * glm::lookAt(lightPos, lightPos + glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, -1.0f, 0.0f));

    particleEmitter->Update(this, glm::vec3(0.0f, 1.0f, 0.0f), stats.frameTime, mainCamera);

    auto end = std::chrono::system_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
    stats.sceneUpdateTime = elapsed.count() / 1000.0f;
}

void VulkanEngine::UpdateSceneNonShadow()
{
    glm::mat4 lightModel = glm::mat4(1.0f);
    lightModel = glm::translate(lightModel, glm::vec3(sceneData.lightPosition.x, sceneData.lightPosition.y, sceneData.lightPosition.z));
    lightModel = glm::scale(lightModel, glm::vec3(0.15, 0.15, 0.15));
    loadedScenes["cube"]->Draw(lightModel, mainDrawContext);
}

bool VulkanEngine::IsVisible(const RenderObject& object, const glm::mat4& viewProjection)
{
    std::array<glm::vec3, 8> corners
    {
        glm::vec3 { 1, 1, 1 },
        glm::vec3 { 1, 1, -1 },
        glm::vec3 { 1, -1, 1 },
        glm::vec3 { 1, -1, -1 },
        glm::vec3 { -1, 1, 1 },
        glm::vec3 { -1, 1, -1 },
        glm::vec3 { -1, -1, 1 },
        glm::vec3 { -1, -1, -1 },
    };

    glm::mat4 matrix = viewProjection * object.transform;

    glm::vec3 min = { 1.5, 1.5, 1.5 };
    glm::vec3 max = { -1.5, -1.5, -1.5 };

    for (int c = 0; c < 8; c++)
    {
        glm::vec4 v = matrix * glm::vec4(object.bounds.origin + (corners[c] * object.bounds.extents), 1.0f);

        v.x = v.x / v.w;
        v.y = v.y / v.w;
        v.z = v.z / v.w;

        min = glm::min(glm::vec3{ v.x, v.y, v.z }, min);
        max = glm::max(glm::vec3{ v.x, v.y, v.z }, max);
    }

    if (min.z > 1.0f || max.z < 0.0f || min.x > 1.0f || max.x < -1.0f || min.y > 1.0f || max.y < -1.0f) {
        return false;
    }
    else {
        return true;
    }
}

void VulkanEngine::InitSkyboxPipeline()
{
    VkShaderModule skyboxVertexShader;
    if (!vkutil::LoadShaderModule("shaders/skyboxVert.spv", device, &skyboxVertexShader))
    {
        fmt::println("Error when building the skybox vertex shader module");
    }

    VkShaderModule skyboxFragShader;
    if (!vkutil::LoadShaderModule("shaders/skyboxFrag.spv", device, &skyboxFragShader))
    {
        fmt::println("Error when building the skybox fragment shader module");
    }

    VkPushConstantRange bufferRange{};
    bufferRange.offset = 0;
    bufferRange.size = sizeof(GPUDrawPushConstants);
    bufferRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    DescriptorLayoutBuilder builder;
    builder.AddBinding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    skyboxDescriptorLayout = builder.Build(device, VK_SHADER_STAGE_FRAGMENT_BIT);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = vkinit::pipeline_layout_create_info();
    pipelineLayoutInfo.pPushConstantRanges = &bufferRange;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pSetLayouts = &skyboxDescriptorLayout;
    pipelineLayoutInfo.setLayoutCount = 1;
    VK_CHECK(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &skyboxPipelineLayout));

    PipelineBuilder pipelineBuilder;

    pipelineBuilder.pipelineLayout = skyboxPipelineLayout;
    pipelineBuilder.SetShaders(skyboxVertexShader, skyboxFragShader);
    pipelineBuilder.SetInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    pipelineBuilder.SetPolygonMode(VK_POLYGON_MODE_FILL);
    pipelineBuilder.SetCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE); // Revert to clockwise
    pipelineBuilder.SetMultisampling(engineSettings.msaaSamples);
    pipelineBuilder.DisableBlending();
    pipelineBuilder.EnableDepthtest(true, VK_COMPARE_OP_LESS); // revert to (Greater or equal to)

    pipelineBuilder.SetColorAttachmentFormat(colorImage.imageFormat);
    pipelineBuilder.SetDepthFormat(depthImage.imageFormat);

    skyboxPipeline = pipelineBuilder.BuildPipeline(device);

    vkDestroyShaderModule(device, skyboxFragShader, nullptr);
    vkDestroyShaderModule(device, skyboxVertexShader, nullptr);

    mainDeletionQueue.PushFunction([=]() {
        vkDestroyPipelineLayout(device, skyboxPipelineLayout, nullptr);
        vkDestroyPipeline(device, skyboxPipeline, nullptr);
        });
}

void VulkanEngine::DrawSkybox(VkCommandBuffer cmd)
{
    //begin a render pass  connected to our draw image
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(colorImage.imageView, nullptr, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(depthImage.imageView, true, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkRenderingInfo renderInfo = vkinit::rendering_info(drawExtent, &colorAttachment, &depthAttachment);
    vkCmdBeginRendering(cmd, &renderInfo);

    //set dynamic viewport and scissor
    VkViewport viewport = {};
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = drawExtent.width;
    viewport.height = drawExtent.height;
    viewport.minDepth = 0.f;
    viewport.maxDepth = 1.f;

    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {};
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent.width = viewport.width;
    scissor.extent.height = viewport.height;

    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyboxPipeline);

    //bind a texture
    VkDescriptorSet imageSet = GetCurrentFrame().frameDescriptors.Allocate(device, skyboxDescriptorLayout);
    {
        DescriptorWriter writer;
        writer.WriteImage(0, skyboxImage.imageView, defaultSamplerNearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

        writer.UpdateSet(device, imageSet);
    }

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyboxPipelineLayout, 0, 1, &imageSet, 0, nullptr);

    glm::mat4 view = glm::mat4(glm::mat3(mainCamera.GetViewMatrix()));

    glm::mat4 projection = glm::perspective(glm::radians(70.0f), (float)drawExtent.width / (float)drawExtent.height, nearPlane, farPlane); // (swap near and far values)

    projection[1][1] *= -1;

    GPUDrawPushConstants pushConstants;
    pushConstants.renderMatrix = projection * view;
    pushConstants.vertexBuffer = skyboxCube.vertexBufferAddress;

    vkCmdPushConstants(cmd, skyboxPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GPUDrawPushConstants), &pushConstants);
    vkCmdBindIndexBuffer(cmd, skyboxCube.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    //vkCmdDrawIndexed(cmd, 36, 1, 0, 0, 0);
    vkCmdDraw(cmd, 36, 1, 0, 0);

    vkCmdEndRendering(cmd);
}

void VulkanEngine::InitDepthMapPipeline()
{
    VkShaderModule depthMapVertexShader;
    if (!vkutil::LoadShaderModule("shaders/depthMapVert.spv", device, &depthMapVertexShader))
    {
        fmt::println("Error when building the depth map vertex shader module");
    }

    VkShaderModule depthMapGeometryShader;
    if (!vkutil::LoadShaderModule("shaders/depthMapGeom.spv", device, &depthMapGeometryShader))
    {
        fmt::println("Error when building the depth map geometry shader module");
    }

    VkShaderModule depthMapFragmentShader;
    if (!vkutil::LoadShaderModule("shaders/depthMapFrag.spv", device, &depthMapFragmentShader))
    {
        fmt::println("Error when building the depth map fragment shader module");
    }

    VkPushConstantRange bufferRange{};
    bufferRange.offset = 0;
    bufferRange.size = sizeof(GPUDrawPushDepthConstants);
    bufferRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    DescriptorLayoutBuilder builder;
    builder.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    depthMapDescriptorLayout = builder.Build(device, VK_SHADER_STAGE_GEOMETRY_BIT);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = vkinit::pipeline_layout_create_info();
    pipelineLayoutInfo.pPushConstantRanges = &bufferRange;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pSetLayouts = &depthMapDescriptorLayout;
    pipelineLayoutInfo.setLayoutCount = 1;
    VK_CHECK(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &depthMapPipelineLayout));

    PipelineBuilder pipelineBuilder;

    pipelineBuilder.pipelineLayout = depthMapPipelineLayout;
    pipelineBuilder.SetShaders(depthMapVertexShader, depthMapGeometryShader, depthMapFragmentShader);
    pipelineBuilder.SetInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    pipelineBuilder.SetPolygonMode(VK_POLYGON_MODE_FILL);
    pipelineBuilder.SetCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE); // Revert to clockwise
    pipelineBuilder.SetMultisampling(VK_SAMPLE_COUNT_1_BIT);
    pipelineBuilder.DisableBlending();
    pipelineBuilder.EnableDepthtest(true, VK_COMPARE_OP_LESS); // revert to (Greater or equal to)

    pipelineBuilder.DisableColorAttachment();
    pipelineBuilder.SetDepthFormat(depthCubemapImage.imageFormat);

    depthMapPipeline = pipelineBuilder.BuildPipeline(device);

    vkDestroyShaderModule(device, depthMapVertexShader, nullptr);
    vkDestroyShaderModule(device, depthMapGeometryShader, nullptr);
    vkDestroyShaderModule(device, depthMapFragmentShader, nullptr);

    mainDeletionQueue.PushFunction([=]() {
        vkDestroyPipelineLayout(device, depthMapPipelineLayout, nullptr);
        vkDestroyDescriptorSetLayout(device, depthMapDescriptorLayout, nullptr); // TODO: Work out whether this is correct.
        vkDestroyPipeline(device, depthMapPipeline, nullptr);
        });
}

void VulkanEngine::DrawDepthMap(VkCommandBuffer cmd)
{

    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(drawImage.imageView, nullptr, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(depthCubemapImage.imageView, true, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkExtent2D depthMapDrawExtent;
    depthMapDrawExtent.height = depthCubemapSize;
    depthMapDrawExtent.width = depthCubemapSize;

    VkRenderingInfo renderingInfo = vkinit::rendering_info_depth_only(depthMapDrawExtent, &depthAttachment);

    std::vector<uint32_t> opaqueDraws;
    opaqueDraws.reserve(mainDrawContext.OpaqueSurfaces.size());

    for (uint32_t i = 0; i < mainDrawContext.OpaqueSurfaces.size(); i++)
    {
        opaqueDraws.push_back(i);
    }

    vkCmdBeginRendering(cmd, &renderingInfo);

    //allocate a new uniform buffer for the scene data
    AllocatedBuffer matrixDataBuffer = CreateBuffer(sizeof(DepthMapGeometryData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    //write the buffer
    DepthMapGeometryData* matrixUniformData = (DepthMapGeometryData*)matrixDataBuffer.allocation->GetMappedData();
    *matrixUniformData = depthMapGeometryData;

    //add it to the deletion queue of this frame so it gets deleted once its been used
    GetCurrentFrame().deletionQueue.PushFunction([=, this]()
        {
            DestroyBuffer(matrixDataBuffer);
        });

    VkDescriptorSet globalDescriptor = GetCurrentFrame().frameDescriptors.Allocate(device, depthMapDescriptorLayout);

    DescriptorWriter writer;
    writer.WriteBuffer(0, matrixDataBuffer.buffer, sizeof(DepthMapGeometryData), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    writer.UpdateSet(device, globalDescriptor);

    VkBuffer lastIndexBuffer = VK_NULL_HANDLE;

    auto draw = [&](const RenderObject& r)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, depthMapPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, depthMapPipelineLayout, 0, 1, &globalDescriptor, 0, nullptr);

        VkViewport viewport = {};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = depthCubemapSize;
        viewport.height = depthCubemapSize;
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;

        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor = {};
        scissor.offset.x = 0.0f;
        scissor.offset.y = 0.0f;
        scissor.extent.width = depthCubemapSize;
        scissor.extent.height = depthCubemapSize;

        vkCmdSetScissor(cmd, 0, 1, &scissor);

        //rebind index buffer if needed
        if (r.indexBuffer != lastIndexBuffer)
        {
            lastIndexBuffer = r.indexBuffer;
            vkCmdBindIndexBuffer(cmd, r.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
        }

        GPUDrawPushDepthConstants pushConstants;
        pushConstants.renderMatrix = r.transform;
        pushConstants.lightPosition = sceneData.lightPosition;
        pushConstants.farPlane = sceneData.shadowFarPlane;
        pushConstants.vertexBuffer = r.vertexBufferAddress;

        vkCmdPushConstants(cmd, depthMapPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_GEOMETRY_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GPUDrawPushDepthConstants), &pushConstants);

        vkCmdDrawIndexed(cmd, r.indexCount, 1, r.firstIndex, 0, 0);
        //stats
        stats.drawcallCount += 1;
        stats.triangleCount += r.indexCount / 3;
    };

    for (auto& r : opaqueDraws)
    {
        draw(mainDrawContext.OpaqueSurfaces[r]);
    }

    vkCmdEndRendering(cmd);
}

void VulkanEngine::InitParticlePipeline()
{
    VkShaderModule particleVertexShader;
    if (!vkutil::LoadShaderModule("shaders/particleVert.spv", device, &particleVertexShader))
    {
        fmt::println("Error when building the particle vertex shader module");
    }

    VkShaderModule particleFragShader;
    if (!vkutil::LoadShaderModule("shaders/particleFrag.spv", device, &particleFragShader))
    {
        fmt::println("Error when building the particle fragment shader module");
    }

    VkPushConstantRange bufferRange{};
    bufferRange.offset = 0;
    bufferRange.size = sizeof(GPUDrawPushParticleConstants);
    bufferRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    DescriptorLayoutBuilder builder;
    builder.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    builder.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    particleDescriptorLayout = builder.Build(device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo = vkinit::pipeline_layout_create_info();
    pipelineLayoutInfo.pPushConstantRanges = &bufferRange;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pSetLayouts = &particleDescriptorLayout;
    pipelineLayoutInfo.setLayoutCount = 1;
    VK_CHECK(vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &particlePipelineLayout));

    PipelineBuilder pipelineBuilder;

    pipelineBuilder.pipelineLayout = particlePipelineLayout;
    pipelineBuilder.SetShaders(particleVertexShader, particleFragShader);
    pipelineBuilder.SetInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    pipelineBuilder.SetPolygonMode(VK_POLYGON_MODE_FILL);
    pipelineBuilder.SetCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE); // Revert to clockwise
    pipelineBuilder.SetMultisampling(engineSettings.msaaSamples);
    pipelineBuilder.EnableBlendingAlphaBlend();
    pipelineBuilder.EnableDepthtest(false, VK_COMPARE_OP_LESS);// revert to (Greater or equal to)

    pipelineBuilder.SetColorAttachmentFormat(colorImage.imageFormat);
    pipelineBuilder.SetDepthFormat(depthImage.imageFormat);

    particlePipeline = pipelineBuilder.BuildPipeline(device);

    vkDestroyShaderModule(device, particleVertexShader, nullptr);
    vkDestroyShaderModule(device, particleFragShader, nullptr);

    mainDeletionQueue.PushFunction([=]() {
        vkDestroyPipelineLayout(device, particlePipelineLayout, nullptr);
        vkDestroyPipeline(device, particlePipeline, nullptr);
        });
}

void VulkanEngine::DrawParticles(VkCommandBuffer cmd)
{
    //begin a render pass  connected to our draw image
    VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(colorImage.imageView, nullptr, VK_IMAGE_LAYOUT_GENERAL);
    VkRenderingAttachmentInfo depthAttachment = vkinit::depth_attachment_info(depthImage.imageView, false, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

    VkRenderingInfo renderInfo = vkinit::rendering_info(drawExtent, &colorAttachment, &depthAttachment);
    vkCmdBeginRendering(cmd, &renderInfo);

    //set dynamic viewport and scissor
    VkViewport viewport = {};
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = drawExtent.width;
    viewport.height = drawExtent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor = {};
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent.width = viewport.width;
    scissor.extent.height = viewport.height;

    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particlePipeline);

    glm::mat4 view = mainCamera.GetViewMatrix();

    glm::mat4 projection = glm::perspective(glm::radians(70.0f), (float)drawExtent.width / (float)drawExtent.height, nearPlane, farPlane); // (swap near and far values)

    projection[1][1] *= -1;

    glm::mat4 model;
    glm::translate(model, particleEmitter->emitterPos);

    ParticleSceneData particleSceneData;
    particleSceneData.particleSize = 3.0f;
    particleSceneData.textureArraySize = 45;
    particleSceneData.projection = projection;
    particleSceneData.view = view;

    AllocatedBuffer particleDataBuffer = CreateBuffer(sizeof(ParticleSceneData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
    //write the buffer
    ParticleSceneData* particleUniformData = (ParticleSceneData*)particleDataBuffer.allocation->GetMappedData();
    *particleUniformData = particleSceneData;

    GetCurrentFrame().deletionQueue.PushFunction([=, this]()
        {
            DestroyBuffer(particleDataBuffer);
        });

    VkDescriptorSet globalDescriptor = GetCurrentFrame().frameDescriptors.Allocate(device, particleDescriptorLayout);
    {
        DescriptorWriter writer;
        writer.WriteBuffer(0, particleDataBuffer.buffer, sizeof(ParticleSceneData), 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
        writer.WriteImage(1, particleSmokeImage.imageView, defaultSamplerNearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
        // build uniform and bind it here
        writer.UpdateSet(device, globalDescriptor);
    }

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particlePipelineLayout, 0, 1, &globalDescriptor, 0, nullptr);

    GPUDrawPushParticleConstants pushParticleConstants;
    pushParticleConstants.renderMatrix = projection * view;
    pushParticleConstants.vertexBuffer = particleBillboard.vertexBufferAddress;
    pushParticleConstants.particlePositionBuffer = particleEmitter->particleBuffers.particleBufferAddress;

    vkCmdPushConstants(cmd, particlePipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GPUDrawPushParticleConstants), &pushParticleConstants);
    vkCmdBindIndexBuffer(cmd, particleBillboard.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    vkCmdDraw(cmd, 6, particleEmitter->particles.size(), 0, 0);

    vkCmdEndRendering(cmd);
}

VkSampleCountFlagBits VulkanEngine::GetMaxUsableSampleCount()
{
    VkPhysicalDeviceProperties physicalDeviceProperties;
    vkGetPhysicalDeviceProperties(physicalDevice, &physicalDeviceProperties);

    VkSampleCountFlags counts = physicalDeviceProperties.limits.framebufferColorSampleCounts & physicalDeviceProperties.limits.framebufferDepthSampleCounts;

    if (counts & VK_SAMPLE_COUNT_64_BIT) { return VK_SAMPLE_COUNT_64_BIT; }
    if (counts & VK_SAMPLE_COUNT_32_BIT) { return VK_SAMPLE_COUNT_32_BIT; }
    if (counts & VK_SAMPLE_COUNT_16_BIT) { return VK_SAMPLE_COUNT_16_BIT; }
    if (counts & VK_SAMPLE_COUNT_8_BIT) { return VK_SAMPLE_COUNT_8_BIT; }
    if (counts & VK_SAMPLE_COUNT_4_BIT) { return VK_SAMPLE_COUNT_4_BIT; }
    if (counts & VK_SAMPLE_COUNT_2_BIT) { return VK_SAMPLE_COUNT_2_BIT; }

    return VK_SAMPLE_COUNT_1_BIT;
}

void GLTFMetallicRoughness::BuildPipelines(VulkanEngine* engine)
{
    VkShaderModule meshVertexShader;
    if (!vkutil::LoadShaderModule("shaders/meshVert.spv", engine->device, &meshVertexShader))
    {
        fmt::println("Error when building the triangle vertex shader module");
    }

    VkShaderModule meshFragShader;
    if (!vkutil::LoadShaderModule("shaders/meshPBRFrag.spv", engine->device, &meshFragShader))
    {
        fmt::println("Error when building the triangle fragment shader module");
    }

    VkPushConstantRange matrixRange{};
    matrixRange.offset = 0;
    matrixRange.size = sizeof(GPUDrawPushConstants);
    matrixRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    DescriptorLayoutBuilder layoutBuilder;
    layoutBuilder.AddBinding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    layoutBuilder.AddBinding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    layoutBuilder.AddBinding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    layoutBuilder.AddBinding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    layoutBuilder.AddBinding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    layoutBuilder.AddBinding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

    materialLayout = layoutBuilder.Build(engine->device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

    VkDescriptorSetLayout  layouts[] = { engine->gpuSceneDataDescriptorLayout, materialLayout };

    VkPipelineLayoutCreateInfo meshLayoutInfo = vkinit::pipeline_layout_create_info();
    meshLayoutInfo.setLayoutCount = 2;
    meshLayoutInfo.pSetLayouts = layouts;
    meshLayoutInfo.pPushConstantRanges = &matrixRange;
    meshLayoutInfo.pushConstantRangeCount = 1;

    VkPipelineLayout newLayout;
    VK_CHECK(vkCreatePipelineLayout(engine->device, &meshLayoutInfo, nullptr, &newLayout));

    opaquePipeline.layout = newLayout;
    transparentPipeline.layout = newLayout;

    PipelineBuilder pipelineBuilder;
    pipelineBuilder.SetShaders(meshVertexShader, meshFragShader);
    pipelineBuilder.SetInputTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    pipelineBuilder.SetPolygonMode(VK_POLYGON_MODE_FILL);
    pipelineBuilder.SetCullMode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE); // Revert to clockwise
    pipelineBuilder.SetMultisampling(engine->engineSettings.msaaSamples);
    pipelineBuilder.DisableBlending();
    pipelineBuilder.EnableDepthtest(true, VK_COMPARE_OP_LESS); // revert to (Greater or equal to)

    pipelineBuilder.SetColorAttachmentFormat(engine->colorImage.imageFormat);
    pipelineBuilder.SetDepthFormat(engine->depthImage.imageFormat);

    pipelineBuilder.pipelineLayout = newLayout;

    opaquePipeline.pipeline = pipelineBuilder.BuildPipeline(engine->device);

    pipelineBuilder.EnableBlendingAdditive();

    pipelineBuilder.EnableDepthtest(false, VK_COMPARE_OP_LESS);

    transparentPipeline.pipeline = pipelineBuilder.BuildPipeline(engine->device);

    vkDestroyShaderModule(engine->device, meshFragShader, nullptr);
    vkDestroyShaderModule(engine->device, meshVertexShader, nullptr);
}

MaterialInstance GLTFMetallicRoughness::WriteMaterial(VkDevice device, MaterialPass pass, const MaterialResources& resources, DescriptorAllocatorGrowable& descriptorAllocator)
{
    MaterialInstance matData;
    matData.passType = pass;
    if (pass == MaterialPass::Transparent)
    {
        matData.pipeline = &transparentPipeline;
    }
    else
    {
        matData.pipeline = &opaquePipeline;
    }

    matData.materialSet = descriptorAllocator.Allocate(device, materialLayout);

    writer.clear();
    writer.WriteBuffer(0, resources.dataBuffer, sizeof(MaterialConstants), resources.dataBufferOffset, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    writer.WriteImage(1, resources.colorImage.imageView, resources.colorSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.WriteImage(2, resources.metallicRoughnessImage.imageView, resources.metallicRoughnessSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.WriteImage(3, resources.normalImage.imageView, resources.normalSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.WriteImage(4, resources.occlusionImage.imageView, resources.occlusionSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    writer.WriteImage(5, resources.emissionImage.imageView, resources.emissionSampler, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);

    writer.UpdateSet(device, matData.materialSet);

    return matData;

}

void MeshNode::Draw(const glm::mat4& topMatrix, DrawContext& context)
{
    glm::mat4 nodeMatrix = topMatrix * worldTransform;

    for (auto& s : mesh->surfaces)
    {
        RenderObject def;
        def.indexCount = s.count;
        def.firstIndex = s.startIndex;
        def.indexBuffer = mesh->meshBuffers.indexBuffer.buffer;
        def.material = &s.material->data;
        def.bounds = s.bounds;
        def.transform = nodeMatrix;
        def.vertexBufferAddress = mesh->meshBuffers.vertexBufferAddress;

        if (s.material->data.passType == MaterialPass::Transparent)
        {
            context.TransparentSurfaces.push_back(def);
        }
        else
        {
            context.OpaqueSurfaces.push_back(def);
        }
    }

    Node::Draw(topMatrix, context);
}