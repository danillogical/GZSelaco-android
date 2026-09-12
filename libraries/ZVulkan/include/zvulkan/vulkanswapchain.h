#pragma once

#include "vulkandevice.h"
#include "vulkanobjects.h"

class VulkanSemaphore;
class VulkanFence;

class VulkanSurfaceCapabilities
{
public:
	VkSurfaceCapabilitiesKHR Capabilites = { };
#ifdef WIN32
	VkSurfaceCapabilitiesFullScreenExclusiveEXT FullScreenExclusive = { VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_FULL_SCREEN_EXCLUSIVE_EXT };
#else
	struct { void* pNext = nullptr; VkBool32 fullScreenExclusiveSupported = 0; } FullScreenExclusive;
#endif
	std::vector<VkPresentModeKHR> PresentModes;
	std::vector<VkSurfaceFormatKHR> Formats;
};

class VulkanSwapChain
{
public:
	VulkanSwapChain(VulkanDevice* device);
	~VulkanSwapChain();

	void Create(int width, int height, int imageCount, bool vsync, bool hdr, bool exclusivefullscreen);
	bool Lost() const { return lost; }
	void SetLost() { lost = true; }

	// Swap in a freshly created VkSurfaceKHR, tearing down the swapchain first.
	// Needed on Android, where the surface dies with the window.
	void RecreateSurface(VkSurfaceKHR newSurface);

	int Width() const { return actualExtent.width; }
	int Height() const { return actualExtent.height; }
	VkSurfaceFormatKHR Format() const { return format; }

	// The mode actually granted, which is not necessarily the one requested: relaxed FIFO is
	// optional, so a vsync request can land on plain FIFO. The frame limiter has to distinguish them
	// because only plain FIFO holds a late present until the next vblank.
	VkPresentModeKHR PresentMode() const { return presentMode; }

	int ImageCount() const { return (int)images.size(); }
	VulkanImage* GetImage(int index) { return images[index].get(); }
	VulkanImageView* GetImageView(int index) { return views[index].get(); }

	int AcquireImage(VulkanSemaphore* semaphore = nullptr, VulkanFence* fence = nullptr);
	void QueuePresent(int imageIndex, VulkanSemaphore* semaphore = nullptr);

private:
	void SelectFormat(const VulkanSurfaceCapabilities& caps, bool hdr);

	bool CreateSwapchain(int width, int height, int imageCount, bool vsync, bool hdr, bool exclusivefullscreen);

	VulkanSurfaceCapabilities GetSurfaceCapabilities(bool exclusivefullscreen);

	VulkanDevice* device = nullptr;
	bool lost = true;

	VkExtent2D actualExtent = {};
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkSurfaceFormatKHR format = {};
	VkPresentModeKHR presentMode;
	std::vector<std::unique_ptr<VulkanImage>> images;
	std::vector<std::unique_ptr<VulkanImageView>> views;

	VulkanSwapChain(const VulkanSwapChain&) = delete;
	VulkanSwapChain& operator=(const VulkanSwapChain&) = delete;
};
