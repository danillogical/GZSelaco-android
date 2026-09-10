
#pragma once

#include "zvulkan/vulkanobjects.h"
#include <array>
#include <map>

class VulkanRenderDevice;
enum class PPFilterMode;
enum class PPWrapMode;

class VkFramebufferManager
{
public:
	VkFramebufferManager(VulkanRenderDevice* fb);
	~VkFramebufferManager();

	void AcquireImage();
	void QueuePresent();

	std::map<int, std::unique_ptr<VulkanFramebuffer>> Framebuffers;

	std::shared_ptr<VulkanSwapChain> SwapChain;
	int PresentImageIndex = -1;

	// One binary semaphore of each kind was correct only while exactly one frame was in flight.
	// With two, frame N's submit would signal RenderFinished while frame N-1's present might not
	// have waited on it yet - two pending signals on a binary semaphore with no wait between,
	// which the spec leaves undefined.
	//
	// They are indexed differently on purpose, because what makes each safe to reuse differs:
	//  - image-available, by FRAME SLOT: reuse is gated by AdvanceFrameSlot having waited on that
	//    slot's fences, so the submit that waited on it has definitely executed.
	//  - render-finished, by SWAPCHAIN IMAGE INDEX: reuse requires having re-acquired that image,
	//    which is itself proof its previous present completed. A frame-slot index would NOT prove
	//    that, since the frame fence signals when rendering ends, not when presentation does.
	enum { maxSemaphoreSlots = 8 };
	std::unique_ptr<VulkanSemaphore> SwapChainImageAvailableSemaphore[maxSemaphoreSlots];
	std::unique_ptr<VulkanSemaphore> RenderFinishedSemaphore[maxSemaphoreSlots];
	int AcquireSlot = 0;

	VulkanSemaphore* ImageAvailableSemaphore() { return SwapChainImageAvailableSemaphore[AcquireSlot].get(); }
	VulkanSemaphore* FrameFinishedSemaphore()
	{
		int i = (PresentImageIndex >= 0 && PresentImageIndex < maxSemaphoreSlots) ? PresentImageIndex : 0;
		return RenderFinishedSemaphore[i].get();
	}

private:
	VulkanRenderDevice* fb = nullptr;
	int CurrentWidth = 0;
	int CurrentHeight = 0;
	bool CurrentVSync = false;
	bool CurrentHdr = false;
	bool CurrentExclusiveFullscreen = false;
};
