/*
**  Vulkan backend
**  Copyright (c) 2016-2020 Magnus Norddahl
**
**  This software is provided 'as-is', without any express or implied
**  warranty.  In no event will the authors be held liable for any damages
**  arising from the use of this software.
**
**  Permission is granted to anyone to use this software for any purpose,
**  including commercial applications, and to alter it and redistribute it
**  freely, subject to the following restrictions:
**
**  1. The origin of this software must not be misrepresented; you must not
**     claim that you wrote the original software. If you use this software
**     in a product, an acknowledgment in the product documentation would be
**     appreciated but is not required.
**  2. Altered source versions must be plainly marked as such, and must not be
**     misrepresented as being the original software.
**  3. This notice may not be removed or altered from any source distribution.
**
*/

#include "vk_commandbuffer.h"
#include "vk_renderdevice.h"
#include "zvulkan/vulkanswapchain.h"
#include "zvulkan/vulkanbuilders.h"
#include "vulkan/textures/vk_framebuffer.h"
#include "vulkan/renderer/vk_renderstate.h"
#include "vulkan/renderer/vk_postprocess.h"
#include "hw_clock.h"
#include "v_video.h"
#include <cassert>

extern int rendered_commandbuffers;
int current_rendered_commandbuffers;

extern bool gpuStatActive;
extern bool keepGpuStatActive;
extern FString gpuStatOutput;

VkCommandBufferManager::VkCommandBufferManager(VulkanRenderDevice* fb, VkQueue *queue, int queueFamily, bool uploadOnly) : fb(fb)
{
	mIsUploadOnly = uploadOnly;
	fbQueue = queue;

	mCommandPool = CommandPoolBuilder()
		.QueueFamily(queueFamily)
		.DebugName("mCommandPool")
		.Create(fb->device.get());

	for (auto& semaphore : mSubmitSemaphore)
		semaphore.reset(new VulkanSemaphore(fb->device.get()));

	for (auto& fence : mSubmitFence)
		fence.reset(new VulkanFence(fb->device.get()));

	if (!mIsUploadOnly && fb->device->GraphicsTimeQueries)
	{
		mTimestampQueryPool = QueryPoolBuilder()
			.QueryType(VK_QUERY_TYPE_TIMESTAMP, MaxTimestampQueries)
			.Create(fb->device.get());

		GetDrawCommands()->resetQueryPool(mTimestampQueryPool.get(), 0, MaxTimestampQueries);
	}
}

VkCommandBufferManager::~VkCommandBufferManager()
{
}

VulkanCommandBuffer* VkCommandBufferManager::GetTransferCommands()
{
	if (!mTransferCommands)
	{
		mTransferCommands = mCommandPool->createBuffer();
		mTransferCommands->SetDebugName("VulkanRenderDevice.mTransferCommands");
		mTransferCommands->begin();
	}
	return mTransferCommands.get();
}

VulkanCommandBuffer* VkCommandBufferManager::GetDrawCommands()
{
	if (!mDrawCommands && !mIsUploadOnly)
	{
		mDrawCommands = mCommandPool->createBuffer();
		mDrawCommands->SetDebugName("VulkanRenderDevice.mDrawCommands");
		mDrawCommands->begin();
	}
	return mDrawCommands.get();
}

std::unique_ptr<VulkanCommandBuffer> VkCommandBufferManager::CreateUnmanagedCommands() {
	std::unique_ptr<VulkanCommandBuffer> cmds = mCommandPool->createBuffer();
	cmds->SetDebugName("VulkanFrameBuffer.arbitraryCommands");
	cmds->begin();

	return cmds;
}

void VkCommandBufferManager::BeginFrame()
{
	if (mNextTimestampQuery > 0)
	{
		GetDrawCommands()->resetQueryPool(mTimestampQueryPool.get(), 0, mNextTimestampQuery);
		mNextTimestampQuery = 0;
	}
}

void VkCommandBufferManager::FlushCommands(VulkanCommandBuffer** commands, size_t count, VkQueue *queue, bool finish, bool lastsubmit)
{
	int currentIndex = mNextSubmit % maxConcurrentSubmitCount;

	// Reclaim this fence if a previous submit still owns it. With two frames in flight this is
	// also the throttle that stops the CPU running more than maxConcurrentSubmitCount submits
	// ahead of the GPU.
	if (mFenceOutstanding[currentIndex])
	{
		vkWaitForFences(fb->device->device, 1, &mSubmitFence[currentIndex]->fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
		vkResetFences(fb->device->device, 1, &mSubmitFence[currentIndex]->fence);
		mFenceOutstanding[currentIndex] = false;
	}

	QueueSubmit submit;

	for (size_t i = 0; i < count; i++)
		submit.AddCommandBuffer(commands[i]);

	// The swapchain acquire wait is added FIRST, ahead of the cross-submit wait below,
	// and the order matters to Mesa/Turnip on KGSL even though Vulkan treats waits as an
	// unordered set.
	//
	// Turnip folds a submit's waits into one sync object with kgsl_syncobj_merge(), which
	// walks pWaitSemaphores in order. Its accumulator starts SIGNALED, so whichever wait
	// comes first decides the branch the next one takes. A semaphore signalled by a
	// previous submit is timestamp-backed (TS); the acquire semaphore is fd-backed, since
	// it wraps the ANativeWindow fence. Put the TS first and the fd hits a branch that
	// converts the wrong operand and dereferences a null queue pointer - a segfault at
	// tu_queue::device, offset 0x1b0. Put the fd first and the TS takes the mirror-image
	// branch, which is correct.
	//
	// Vulkan attaches pWaitDstStageMask per index, and AddWait keeps each mask with its
	// semaphore, so reordering is free and changes no semantics: all waits must still be
	// satisfied before the batch executes.
	//
	// This is why the bug is not universal on Turnip - listing the acquire semaphore first
	// is the common convention, and most submits have only that one wait.
	bool presenting = finish && fb->GetFramebufferManager()->PresentImageIndex != -1;

	if (presenting)
		submit.AddWait(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, fb->GetFramebufferManager()->ImageAvailableSemaphore());

	// Only wait if the previous submit actually signalled. It signals mSubmitSemaphore
	// only when !lastsubmit (below), and every frame ends via SubmitAndWait with
	// lastsubmit = true - so waiting whenever mNextSubmit > 0 meant each frame boundary
	// waited on a binary semaphore with no pending signal operation, which the spec
	// leaves undefined. Every driver tested tolerates it, so this fixes no known crash;
	// it is just not defensible as written.
	//
	// Present in upstream GZDoom since 2022 (ecd2dc6300/ed134c9b19), still in master.
	if (mNextSubmit > 0 && mPrevSubmitSignalled)
		submit.AddWait(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mSubmitSemaphore[(mNextSubmit - 1) % maxConcurrentSubmitCount].get());

	if (presenting)
		submit.AddSignal(fb->GetFramebufferManager()->FrameFinishedSemaphore());

	if (!lastsubmit)
	{
		submit.AddSignal(mSubmitSemaphore[currentIndex].get());
		mPrevSubmitSignalled = true;
	}
	else
	{
		mPrevSubmitSignalled = false;
	}

	submit.Execute(fb->device.get(), *queue, mSubmitFence[currentIndex].get());
	mFenceOutstanding[currentIndex] = true;
	mCurrentFrameFences.push_back(currentIndex);
	mNextSubmit++;
}

void VkCommandBufferManager::FlushCommands(bool finish, bool lastsubmit, bool uploadOnly)
{
	if (!uploadOnly)
		fb->GetRenderState()->EndRenderPass();

	if ((!uploadOnly && mDrawCommands) || mTransferCommands)
	{
		VulkanCommandBuffer* commands[2];
		size_t count = 0;

		if (mTransferCommands)
		{
			mTransferCommands->end();
			commands[count++] = mTransferCommands.get();
			TransferDeleteList->Add(std::move(mTransferCommands));
		}

		if (!uploadOnly && mDrawCommands)
		{
			mDrawCommands->end();
			commands[count++] = mDrawCommands.get();
			DrawDeleteList->Add(std::move(mDrawCommands));
		}

		FlushCommands(commands, count, fbQueue, finish, lastsubmit);

		current_rendered_commandbuffers += (int)count;
	}
}

extern glcycle_t GPUWait, FPSWait;

// The wait, the recycle, and the submit counter reset - the part that must happen before any
// GPU-visible resource is rewritten.
void VkCommandBufferManager::FinishFrameWait(bool uploadOnly, bool clockIt)
{
	// Only fences a submit actually signalled and nobody has reclaimed yet. Waiting on the raw
	// [0, mNextSubmit) range would block forever on any fence already reset by the reuse path
	// above, which two frames in flight makes reachable.
	VkFence waitFences[maxConcurrentSubmitCount];
	int numWaitFences = 0;
	for (int i = 0; i < maxConcurrentSubmitCount; i++)
		if (mFenceOutstanding[i])
			waitFences[numWaitFences++] = mSubmitFence[i]->fence;

	if (numWaitFences > 0)
	{
		if (clockIt) { GPUWait.Reset(); GPUWait.Clock(); }
		vkWaitForFences(fb->device->device, numWaitFences, waitFences, VK_TRUE, std::numeric_limits<uint64_t>::max());
		vkResetFences(fb->device->device, numWaitFences, waitFences);
		if (clockIt) GPUWait.Unclock();
		for (int i = 0; i < maxConcurrentSubmitCount; i++)
			mFenceOutstanding[i] = false;
	}

	// ORDER IS LOAD-BEARING: the RETAINED slot lists must be destroyed BEFORE the current one.
	//
	// Destruction has to run oldest-first, matching the order things were retired, because a
	// descriptor set is the one object in DeleteList with an intra-list dependency: it frees itself
	// against its pool, and vkDestroyDescriptorPool ALREADY implicitly freed every set from that
	// pool. A pool retired in frame N therefore must not be destroyed before frame N-1's list has
	// freed the sets it holds from that pool - doing so makes each of those a double free.
	//
	// This ran the other way round (DeleteFrameObjects first, slots second), and that is a real crash
	// rather than a theoretical one: `Scudo ERROR: invalid chunk state when deallocating` inside
	// Turnip's tu_FreeDescriptorSets, twice in 13 minutes on an Adreno 740. Qualcomm's driver
	// silently tolerates the same double free, which is why it only shows up on Turnip.
	//
	// It cannot happen at one frame in flight - AdvanceFrameSlot fills and clears the same slot in a
	// single call, so no list is ever retained. The retained list is what two frames introduce.
	//
	// The GPU is idle by this point (all fences waited above), so this is purely CPU-side ordering.
	for (auto& slot : mFrameSlots)
	{
		slot.FenceIndices.clear();
		slot.TransferDeleteList.reset();
		slot.DrawDeleteList.reset();
	}

	DeleteFrameObjects(uploadOnly);

	mCurrentFrameFences.clear();
	mNextSubmit = 0;
}

// Retire the frame that just ended into its own slot, then take ownership of the slot belonging
// to the frame two back and wait for it. Waiting two back rather than one is the entire point:
// the frame in between stays in flight, so the GPU keeps working while this frame is recorded.
void VkCommandBufferManager::AdvanceFrameSlot()
{
	if (mIsUploadOnly)
		return;

	// Only mNextSubmit % maxConcurrentSubmitCount is meaningful once frames overlap, so rebase
	// to keep it small - it no longer resets every frame and would otherwise grow without bound.
	mNextSubmit %= maxConcurrentSubmitCount;

	// Hand the just-finished frame's garbage and fences to the slot it was using.
	FrameSlotData& ending = mFrameSlots[mFrameSlot];
	ending.FenceIndices = std::move(mCurrentFrameFences);
	ending.TransferDeleteList = std::move(TransferDeleteList);
	ending.DrawDeleteList = std::move(DrawDeleteList);
	mCurrentFrameFences.clear();

	mFrameSlot = (mFrameSlot + 1) % framesInFlight;

	FrameSlotData& reusing = mFrameSlots[mFrameSlot];

	// Clear each flag AS the fence is collected, not in a second pass afterwards.
	//
	// That ordering is load-bearing, not style. FenceIndices gets an entry per SUBMIT, so a frame
	// with more than maxConcurrentSubmitCount submits repeats indices - and it can: 4 submits per
	// frame at the default vk_submit_size, but mid-frame WaitForStreamBuffers flushes, the
	// postprocess chain and shadowmap passes all add more, and vk_submit_size 50 measured 79.
	// Testing the flag without clearing it let every repeat of an index pass, so n could exceed
	// the array and smash the stack - a corruption that surfaces anywhere later, which is exactly
	// how it presented: one crash inside tu_FreeDescriptorSets that looked like a double free, and
	// one while standing still doing nothing.
	//
	// Clearing on collection makes each fence contribute at most once, which bounds n to
	// maxConcurrentSubmitCount by construction since that is how many fences exist.
	VkFence waitFences[maxConcurrentSubmitCount];
	int n = 0;
	for (int idx : reusing.FenceIndices)
	{
		if (idx < 0 || idx >= maxConcurrentSubmitCount || !mFenceOutstanding[idx])
			continue;
		mFenceOutstanding[idx] = false;
		waitFences[n++] = mSubmitFence[idx]->fence;
	}
	assert(n <= maxConcurrentSubmitCount);

	if (n > 0)
	{
		GPUWait.Reset();
		GPUWait.Clock();
		vkWaitForFences(fb->device->device, n, waitFences, VK_TRUE, std::numeric_limits<uint64_t>::max());
		vkResetFences(fb->device->device, n, waitFences);
		GPUWait.Unclock();
	}
	reusing.FenceIndices.clear();

	// Safe now: those fences signalled, so the GPU has finished reading everything this frame
	// created two frames ago.
	reusing.TransferDeleteList.reset();
	reusing.DrawDeleteList.reset();

	TransferDeleteList = std::make_unique<DeleteList>();
	DrawDeleteList = std::make_unique<DeleteList>();
}

void VkCommandBufferManager::WaitForCommands(bool finish, bool uploadOnly)
{
	if (finish)
	{
		Finish.Reset();
		Finish.Clock();

		fb->GetFramebufferManager()->AcquireImage();

	}

	FlushCommands(finish, true, uploadOnly);

	if (finish)
	{
		// Run the limiter regardless of vsync. The GL and GLES backends already call
		// FPSLimit() unconditionally before SwapBuffers; only Vulkan skipped it when
		// vsync was on, which made vid_maxfps silently inert in the one configuration
		// a handheld actually ships with. FPSLimit() compensates for vsync itself.
		FPSWait.Reset();
		FPSWait.Clock();
		fb->FPSLimit();
		FPSWait.Unclock();

		fb->GetFramebufferManager()->QueuePresent();
	}

	// Only the frame-end call is deferrable, and never for the upload-only managers - their
	// callers (e.g. WaitForStreamBuffers) need the GPU idle on return.
	// Resources are retired in AdvanceFrameSlot instead, so a frame ends with its submit and
	// present and no wait at all. mNextSubmit is deliberately NOT reset: the fence index must keep
	// advancing across the frame boundary, or this frame would reuse fences the previous one is
	// still waiting on and serialize.
	if (finish && !mIsUploadOnly)
	{
		Finish.Unclock();
		rendered_commandbuffers = current_rendered_commandbuffers;
		current_rendered_commandbuffers = 0;
		return;
	}

	FinishFrameWait(uploadOnly, finish);

	if (finish)
	{
		Finish.Unclock();
		rendered_commandbuffers = current_rendered_commandbuffers;
		current_rendered_commandbuffers = 0;
	}
}

void VkCommandBufferManager::DeleteFrameObjects(bool uploadOnly)
{
	TransferDeleteList = std::make_unique<DeleteList>();
	if (!uploadOnly)
		DrawDeleteList = std::make_unique<DeleteList>();
}

// Oldest-first, and before the current list - same reason as in FinishFrameWait: a descriptor set
// must be freed before the pool it came from is destroyed.
void VkCommandBufferManager::DropRetainedFrames()
{
	for (auto& slot : mFrameSlots)
	{
		slot.FenceIndices.clear();
		slot.TransferDeleteList.reset();
		slot.DrawDeleteList.reset();
	}
	mCurrentFrameFences.clear();
}

void VkCommandBufferManager::PushGroup(const FString& name)
{
	if (!gpuStatActive)
		return;

	if (mNextTimestampQuery < MaxTimestampQueries && fb->device->GraphicsTimeQueries)
	{
		TimestampQuery q;
		q.name = name;
		q.startIndex = mNextTimestampQuery++;
		q.endIndex = 0;
		GetDrawCommands()->writeTimestamp(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, mTimestampQueryPool.get(), q.startIndex);
		mGroupStack.push_back(timeElapsedQueries.size());
		timeElapsedQueries.push_back(q);
	}
}

void VkCommandBufferManager::PopGroup()
{
	if (!gpuStatActive || mGroupStack.empty())
		return;

	TimestampQuery& q = timeElapsedQueries[mGroupStack.back()];
	mGroupStack.pop_back();

	if (mNextTimestampQuery < MaxTimestampQueries && fb->device->GraphicsTimeQueries)
	{
		q.endIndex = mNextTimestampQuery++;
		GetDrawCommands()->writeTimestamp(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, mTimestampQueryPool.get(), q.endIndex);
	}
}

void VkCommandBufferManager::UpdateGpuStats()
{
	// With two frames in flight the timestamp pool is shared between a frame being recorded and
	// one still executing, and reading it with VK_QUERY_RESULT_WAIT_BIT blocks on the GPU -
	// reintroducing precisely the stall pipelining exists to remove, which would make the change
	// measure as doing nothing. Per-group GPU timings are therefore unavailable in this mode.
	// rendertimes, i_benchmark's cpu gap and frame times are all unaffected.
	// Always, now that two frames in flight is unconditional.
	{
		gpuStatOutput = "";
		timeElapsedQueries.clear();
		mGroupStack.clear();
		mNextTimestampQuery = 0;
		gpuStatActive = false;
		keepGpuStatActive = false;
		return;
	}

	uint64_t timestamps[MaxTimestampQueries];
	if (mNextTimestampQuery > 0)
		mTimestampQueryPool->getResults(0, mNextTimestampQuery, sizeof(uint64_t) * mNextTimestampQuery, timestamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

	double timestampPeriod = fb->device->PhysicalDevice.Properties.Properties.limits.timestampPeriod;

	gpuStatOutput = "";
	for (auto& q : timeElapsedQueries)
	{
		if (q.endIndex <= q.startIndex)
			continue;

		int64_t timeElapsed = max(static_cast<int64_t>(timestamps[q.endIndex] - timestamps[q.startIndex]), (int64_t)0);
		double timeNS = timeElapsed * timestampPeriod;

		FString out;
		out.Format("%s=%04.2f ms\n", q.name.GetChars(), timeNS / 1000000.0f);
		gpuStatOutput += out;
	}
	timeElapsedQueries.clear();
	mGroupStack.clear();

	gpuStatActive = keepGpuStatActive;
	keepGpuStatActive = false;
}
