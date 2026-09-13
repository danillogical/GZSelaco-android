#pragma once

#include <zvulkan/vulkandevice.h>
#include <zvulkan/vulkanobjects.h>
#include "zstring.h"
#include "hwrenderer/data/buffers.h"   // HW_MAX_PIPELINE_BUFFERS, for the static_assert below

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
	// It was a cvar so the change could be A/B'd, and that is done: 35.7 -> 27.2 ms. Changing it at
	// runtime is a use-after-free, because AdvanceFrameSlot stops retiring slots at the same moment
	// FinishFrameWait starts clearing them, tearing down retained delete lists while a frame is still
	// executing. A constant removes that whole class of bug.
	enum { framesInFlight = 2 };

	// The engine buffers this depth drives are fixed-size arrays of HW_MAX_PIPELINE_BUFFERS, and the
	// Vulkan path passes framesInFlight straight into them without clamping - unlike GL and GLES,
	// which both clamp. HW_MAX_PIPELINE_BUFFERS is 2 on non-Android, so this currently sits exactly on
	// the boundary and raising it would be an out-of-bounds write into four separate objects with no
	// diagnostic. Fail the build instead.
	static_assert(framesInFlight <= HW_MAX_PIPELINE_BUFFERS,
		"framesInFlight exceeds HW_MAX_PIPELINE_BUFFERS - the rotated engine buffers would overflow");

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

		// DECLARATION ORDER IS LOAD-BEARING: DescriptorPools must come before Descriptors.
		//
		// Members are destroyed in reverse declaration order, so this frees the sets first and the
		// pools second. The other way round is a double free: vkDestroyDescriptorPool implicitly
		// frees every set allocated from that pool, and ~VulkanDescriptorSet then frees itself
		// against it again. Sorting these two lines, or inserting a new type between them, is enough
		// to reintroduce it.
		//
		// It will not show up in testing on this device. Qualcomm's driver tolerates the double free
		// silently; only Turnip reports it, as `Scudo ERROR: invalid chunk state when deallocating`
		// inside tu_FreeDescriptorSets. See FinishFrameWait for the cross-list ordering, which is the
		// same hazard between two DeleteLists rather than within one.
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

	// Drop the RETAINED in-flight frames' delete lists. DeleteFrameObjects only replaces the current
	// list, so at two frames in flight the previous frame's list survives it - and at teardown that
	// list outlives the descriptor pools (mDescriptorSetManager is destroyed before mCommands), so
	// every descriptor set still in it frees itself against a destroyed pool. Call this before the
	// managers go away. Safe only with the GPU idle.
	void DropRetainedFrames();

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
	int mNextSubmit = 0;

	// Which SUBMIT last used each fence, and how far each in-flight frame got.
	//
	// A single representation answers both "is this fence in use" and "whose is it". Serials start at 1
	// (pre-incremented), so 0 unambiguously means free - there is no separate outstanding flag, and so
	// no way for the two to disagree.
	//
	// A bool alone could not answer "whose fence is this": it records that an index is in use,
	// not which frame owns it - and there are only maxConcurrentSubmitCount fences for an unbounded
	// number of submits per frame, so two consecutive frames' index sets overlap as soon as
	// submits(N) + submits(N+1) > maxConcurrentSubmitCount. Retiring frame N then waited on fences
	// already reused by frame N+1, blocking until N+1 was nearly complete: two frames in flight
	// silently degenerated to one, and kept the extra buffers. Four submits per frame is the default,
	// so 4+4 sat exactly on the boundary and any extra flush tipped it over.
	//
	// A monotonic serial makes ownership explicit. A fence whose serial is newer than the retiring
	// frame's last submit belongs to a later frame and is skipped - safely, because the reuse path in
	// FlushCommands waits before reusing an index, so the retiring frame's work on it has already
	// completed.
	uint64_t mSubmitSerial = 0;
	uint64_t mFenceSerial[maxConcurrentSubmitCount] = {};
	uint64_t mCurrentFrameLastSerial = 0;

	struct TimestampQuery
	{
		FString name;
		uint32_t startIndex;
		uint32_t endIndex;
	};

	// Resources belonging to one in-flight frame, freed only once its fences have signalled.
	struct FrameSlotData
	{
		uint64_t LastSerial = 0;   // highest submit serial this frame produced; 0 = no submits
		std::unique_ptr<DeleteList> TransferDeleteList;
		std::unique_ptr<DeleteList> DrawDeleteList;

		// The groups this slot's frame recorded, kept until its timestamps are read two frames
		// later. The names and index pairs belong to the frame that wrote them, so they cannot
		// live in a single shared vector the way they did when the pool was per-device.
		std::vector<TimestampQuery> TimeElapsedQueries;
		uint32_t TimestampsUsed = 0;   // how much of this slot's range the frame consumed
	};
	FrameSlotData mFrameSlots[framesInFlight];
	int mFrameSlot = 0;

	// Whether the immediately preceding submit actually signalled its semaphore.
	//
	// The wait and the signal are not symmetric: a submit signals mSubmitSemaphore only
	// when !lastsubmit, but the following submit used to wait on it unconditionally. As
	// every frame ends via SubmitAndWait with lastsubmit = true, that made each frame
	// boundary wait on a binary semaphore with no pending signal operation, which the
	// spec leaves undefined. No driver tested actually misbehaves on it.
	bool mPrevSubmitSignalled = false;

	void FinishFrameWait(bool uploadOnly, bool clockIt);

	// Raised from 100. The scene pass now has ~10 groups (opaque plus its six draw lists,
	// decals, tborder, translucent) at 2 timestamps each, and a frame with portals or
	// mirrors re-enters RenderScene several times - three sets in one frame has been
	// observed. Add the postprocess chain and 100 was close enough to the ceiling to start
	// silently dropping groups, since PushGroup just stops recording when it runs out.
	//
	// This is the PER-SLOT count. The pool holds framesInFlight of these ranges back to back,
	// so slot s owns [s * MaxTimestampQueries, (s+1) * MaxTimestampQueries) and a frame being
	// recorded never writes indices a frame still executing is using. Sized from framesInFlight
	// directly so the two cannot drift apart.
	enum { MaxTimestampQueries = 256 };
	std::unique_ptr<VulkanQueryPool> mTimestampQueryPool;
	int mNextTimestampQuery = 0;         // offset WITHIN the current slot's range, not absolute
	std::vector<size_t> mGroupStack;

	// Read the timestamps the given slot recorded when it was last used, into gpuStatOutput.
	// Only safe for a slot whose fence AdvanceFrameSlot has just waited on.
	void ReadFrameSlotTimestamps(int slot);
};
