#pragma once

#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkanobjects.h>
#include "zstring.h"

class VulkanRenderDevice;

class VkCommandBufferManager
{
public:
	VkCommandBufferManager(VulkanRenderDevice* fb, VkQueue* queue, int queueFamily, bool uploadOnly = false);
	~VkCommandBufferManager();

	void BeginFrame();

	VulkanCommandBuffer* GetTransferCommands();
	VulkanCommandBuffer* GetDrawCommands();
	std::unique_ptr<VulkanCommandBuffer> CreateUnmanagedCommands();

	void FlushCommands(bool finish, bool lastsubmit = false, bool uploadOnly = false);

	void WaitForCommands(bool finish) { WaitForCommands(finish, false); }
	void WaitForCommands(bool finish, bool uploadOnly);


	// Two frames in flight. Called once at the top of each frame: retires the frame that ended,
	// then waits for the frame TWO back - the one whose resources are about to be reused - so
	// the frame in between can still be executing on the GPU while this one is recorded.
	void AdvanceFrameSlot();
	int FrameSlot() const { return mFrameSlot; }

	// Frames in flight, fixed at 2 and NOT runtime settable.
	//
	// It was a cvar so the change could be A/B'd, and that is done: 35.7 -> 27.2 ms. But changing
	// it at runtime is a use-after-free - AdvanceFrameSlot stops retiring slots at the same moment
	// FinishFrameWait starts clearing them, tearing down retained delete lists while a frame is
	// still executing. That showed up as SIGABRT in scudo::reportInvalidChunkState from
	// tu_FreeDescriptorSets: a double free Qualcomm's driver silently tolerated and Turnip, built
	// against the scudo heap checker, aborts on. A constant removes the whole class of bug.
	enum { framesInFlight = 2 };

	void PushGroup(const FString& name);
	void PopGroup();
	void UpdateGpuStats();

	VulkanRenderDevice *GetRenderDevice() { return fb; }

	class DeleteList
	{
	public:
		std::vector<std::unique_ptr<VulkanBuffer>> Buffers;
		std::vector<std::unique_ptr<VulkanSampler>> Samplers;
		std::vector<std::unique_ptr<VulkanImage>> Images;
		std::vector<std::unique_ptr<VulkanImageView>> ImageViews;
		std::vector<std::unique_ptr<VulkanFramebuffer>> Framebuffers;
		std::vector<std::unique_ptr<VulkanAccelerationStructure>> AccelStructs;
		std::vector<std::unique_ptr<VulkanDescriptorPool>> DescriptorPools;
		std::vector<std::unique_ptr<VulkanDescriptorSet>> Descriptors;
		std::vector<std::unique_ptr<VulkanShader>> Shaders;
		std::vector<std::unique_ptr<VulkanCommandBuffer>> CommandBuffers;
		size_t TotalSize = 0;

		void Add(std::unique_ptr<VulkanBuffer> obj) { if (obj) { TotalSize += obj->size; Buffers.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanSampler> obj) { if (obj) { Samplers.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanImage> obj) { if (obj) { Images.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanImageView> obj) { if (obj) { ImageViews.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanFramebuffer> obj) { if (obj) { Framebuffers.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanAccelerationStructure> obj) { if (obj) { AccelStructs.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanDescriptorPool> obj) { if (obj) { DescriptorPools.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanDescriptorSet> obj) { if (obj) { Descriptors.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanCommandBuffer> obj) { if (obj) { CommandBuffers.push_back(std::move(obj)); } }
		void Add(std::unique_ptr<VulkanShader> obj) { if (obj) { Shaders.push_back(std::move(obj)); } }
	};

	std::unique_ptr<DeleteList> TransferDeleteList = std::make_unique<DeleteList>();
	std::unique_ptr<DeleteList> DrawDeleteList = std::make_unique<DeleteList>();

	void DeleteFrameObjects(bool uploadOnly = false);

	bool IsUploadOnly() const { return mIsUploadOnly; }

private:
	void FlushCommands(VulkanCommandBuffer** commands, size_t count, VkQueue *queue, bool finish, bool lastsubmit);

	VulkanRenderDevice* fb = nullptr;
	VkQueue* fbQueue = nullptr;
	bool mIsUploadOnly;

	std::unique_ptr<VulkanCommandPool> mCommandPool;
	std::unique_ptr<VulkanCommandBuffer> mTransferCommands;
	std::unique_ptr<VulkanCommandBuffer> mDrawCommands;

	enum { maxConcurrentSubmitCount = 8 };
	std::unique_ptr<VulkanSemaphore> mSubmitSemaphore[maxConcurrentSubmitCount];
	std::unique_ptr<VulkanFence> mSubmitFence[maxConcurrentSubmitCount];
	VkFence mSubmitWaitFences[maxConcurrentSubmitCount];
	int mNextSubmit = 0;

	// Whether each pool fence has been signalled by a submit and not yet waited on and reset.
	//
	// Needed because with two frames in flight there are two places that retire a fence - the
	// reuse check in FlushCommands and AdvanceFrameSlot - and waiting on an already-reset fence
	// with no pending signal operation deadlocks. Tracking it explicitly is robust no matter how
	// many submits a frame makes, which the old `mNextSubmit >= maxConcurrentSubmitCount` test
	// was not: it assumed a fixed submits-per-frame, and vk_submit_size and mid-frame
	// WaitForStreamBuffers flushes both change that count.
	bool mFenceOutstanding[maxConcurrentSubmitCount] = {};

	// Resources belonging to one in-flight frame, freed only once its fences have signalled.
	struct FrameSlotData
	{
		std::vector<int> FenceIndices;
		std::unique_ptr<DeleteList> TransferDeleteList;
		std::unique_ptr<DeleteList> DrawDeleteList;
	};
	FrameSlotData mFrameSlots[framesInFlight];
	int mFrameSlot = 0;
	std::vector<int> mCurrentFrameFences;

	// Whether the immediately preceding submit actually signalled its semaphore.
	//
	// The wait and the signal are not symmetric: a submit signals mSubmitSemaphore only
	// when !lastsubmit, but the following submit used to wait on it unconditionally. As
	// every frame ends via SubmitAndWait with lastsubmit = true, that made each frame
	// boundary wait on a binary semaphore with no pending signal operation, which the
	// spec leaves undefined. No driver tested actually misbehaves on it.
	bool mPrevSubmitSignalled = false;

	void FinishFrameWait(bool uploadOnly, bool clockIt);

	struct TimestampQuery
	{
		FString name;
		uint32_t startIndex;
		uint32_t endIndex;
	};

	// Raised from 100. The scene pass now has ~10 groups (opaque plus its six draw lists,
	// decals, tborder, translucent) at 2 timestamps each, and a frame with portals or
	// mirrors re-enters RenderScene several times - three sets in one frame has been
	// observed. Add the postprocess chain and 100 was close enough to the ceiling to start
	// silently dropping groups, since PushGroup just stops recording when it runs out.
	enum { MaxTimestampQueries = 256 };
	std::unique_ptr<VulkanQueryPool> mTimestampQueryPool;
	int mNextTimestampQuery = 0;
	std::vector<size_t> mGroupStack;
	std::vector<TimestampQuery> timeElapsedQueries;
};
