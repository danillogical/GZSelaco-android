#pragma once

#include "vulkaninstance.h"

class VulkanSurface
{
public:
	VulkanSurface(std::shared_ptr<VulkanInstance> instance, VkSurfaceKHR surface);
	~VulkanSurface();

	// Destroy the current surface and adopt a new one. Only safe once nothing
	// references the old handle - notably the swapchain must already be gone.
	void ReplaceHandle(VkSurfaceKHR newSurface);

	std::shared_ptr<VulkanInstance> Instance;
	VkSurfaceKHR Surface = VK_NULL_HANDLE;

#ifdef VK_USE_PLATFORM_WIN32_KHR

	VulkanSurface(std::shared_ptr<VulkanInstance> instance, HWND window);
	HWND Window = 0;

#endif
};
