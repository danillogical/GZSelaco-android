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
		// One range per frame slot, back to back - see MaxTimestampQueries.
		mTimestampQueryPool = QueryPoolBuilder()
			.QueryType(VK_QUERY_TYPE_TIMESTAMP, MaxTimestampQueries * framesInFlight)
			.Create(fb->device.get());

		GetDrawCommands()->resetQueryPool(mTimestampQueryPool.get(), 0, MaxTimestampQueries * framesInFlight);
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
	if (!mTimestampQueryPool)
		return;

	// Read and recycle this slot's PREVIOUS occupancy here, not at the end of the frame.
	//
	// AdvanceFrameSlot has just waited this slot's fence, so the timestamps its last frame wrote
	// are complete and vkGetQueryPoolResults returns immediately. At the end of the frame the same
	// read would be against a frame still executing, and VK_QUERY_RESULT_WAIT_BIT would block on
	// the GPU - which is the stall pipelining exists to remove, and why this whole subsystem was
	// switched off rather than fixed when frames in flight landed.
	//
	// The consequence is that the figures are two frames old: at framesInFlight = 2 a slot comes
	// round every other frame. That is fine for a profiler and is labelled in the output.
	FrameSlotData &slot = mFrameSlots[mFrameSlot];
	if (slot.TimestampsUsed > 0)
	{
		ReadFrameSlotTimestamps(mFrameSlot);
		GetDrawCommands()->resetQueryPool(mTimestampQueryPool.get(), mFrameSlot * MaxTimestampQueries, slot.TimestampsUsed);
		slot.TimeElapsedQueries.clear();
		slot.TimestampsUsed = 0;
	}
	mNextTimestampQuery = 0;
}

void VkCommandBufferManager::FlushCommands(VulkanCommandBuffer** commands, size_t count, VkQueue *queue, bool finish, bool lastsubmit)
{
	int currentIndex = mNextSubmit % maxConcurrentSubmitCount;

	// Reclaim this fence if a previous submit still owns it. With two frames in flight this is
	// also the throttle that stops the CPU running more than maxConcurrentSubmitCount submits
	// ahead of the GPU.
	if (mFenceSerial[currentIndex] != 0)
	{
		vkWaitForFences(fb->device->device, 1, &mSubmitFence[currentIndex]->fence, VK_TRUE, std::numeric_limits<uint64_t>::max());
		vkResetFences(fb->device->device, 1, &mSubmitFence[currentIndex]->fence);
		mFenceSerial[currentIndex] = 0;
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
	mFenceSerial[currentIndex] = ++mSubmitSerial;
	mCurrentFrameLastSerial = mSubmitSerial;
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
			// Make the uploads visible to the draw commands submitted alongside them.
			//
			// The two command buffers go into ONE submit in order, but submission order only orders
			// execution start - it is not a memory dependency, so a vkCmdCopyBuffer here and a
			// vkCmdDrawIndexed reading the same buffer there may overlap with the write invisible to
			// the read. Synchronization validation reports it as a READ_AFTER_WRITE hazard on
			// VkHardwareBuffer.Stream: INDEX_READ at INDEX_INPUT against TRANSFER_WRITE at COPY.
			//
			// Upstream has the same gap and gets away with it at one frame in flight, where the
			// preceding frame's wait happens to serialise things. At two it is live: frame N+1's
			// uploads overlap frame N's draws by design, which is the entire point of pipelining.
			//
			// One global barrier rather than per-buffer ones: every buffer the transfer touched is
			// covered, it costs a single pipeline barrier per submit, and it cannot go stale as new
			// upload paths are added.
			PipelineBarrier()
				.AddMemory(VK_ACCESS_TRANSFER_WRITE_BIT,
					VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
					VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT)
				.Execute(mTransferCommands.get(),
					VK_PIPELINE_STAGE_TRANSFER_BIT,
					VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
					VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

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
		if (mFenceSerial[i] != 0)
			waitFences[numWaitFences++] = mSubmitFence[i]->fence;

	if (numWaitFences > 0)
	{
		if (clockIt) { GPUWait.Reset(); GPUWait.Clock(); }
		vkWaitForFences(fb->device->device, numWaitFences, waitFences, VK_TRUE, std::numeric_limits<uint64_t>::max());
		vkResetFences(fb->device->device, numWaitFences, waitFences);
		if (clockIt) GPUWait.Unclock();
		for (int i = 0; i < maxConcurrentSubmitCount; i++)
			mFenceSerial[i] = 0;
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
	// Turnip's tu_FreeDescriptorSets. Qualcomm's driver
	// silently tolerates the same double free, which is why it only shows up on Turnip.
	//
	// It cannot happen at one frame in flight - AdvanceFrameSlot fills and clears the same slot in a
	// single call, so no list is ever retained. The retained list is what two frames introduce.
	//
	// The GPU is idle by this point (all fences waited above), so this is purely CPU-side ordering.
	DropRetainedFrames();

	DeleteFrameObjects(uploadOnly);

	// Captured BEFORE mNextSubmit is reset below - it names the submit that could have left an
	// orphaned signal, and after the reset that information is gone. Only meaningful when
	// mPrevSubmitSignalled is set; see the note further down.
	const int orphanIndex = (mNextSubmit + maxConcurrentSubmitCount - 1) % maxConcurrentSubmitCount;

	// mCurrentFrameLastSerial was already cleared by DropRetainedFrames above.
	mNextSubmit = 0;

	// Recreate ONLY the semaphore that can actually hold an orphaned signal, if any.
	//
	// A binary semaphore can be left signalled with no waiter: a mid-frame flush with !lastsubmit
	// signals mSubmitSemaphore[k], and if the next FlushCommands finds nothing recorded it does not
	// submit, so nothing consumes it. Resetting mNextSubmit then loses the back-reference, and the
	// `mNextSubmit > 0` guard suppresses the wait for good. The next time index k comes round its
	// submit signals an already-signalled binary semaphore - and from then on one wait consumes one
	// signal and leaves it signalled, so every later wait on k passes instantly on a stale signal and
	// intra-frame submit ordering silently stops holding.
	//
	// mPrevSubmitSignalled records exactly that state: FlushCommands sets it iff !lastsubmit, and the
	// wait consumes it. So there is an orphan if and only if it is set, and it is on the index the next
	// submit would have waited on. Recreating all 8 was the first fix here and was far heavier than
	// needed - this path is reached per background TEXTURE UPLOAD via the transfer managers, ~2000 per
	// level load, where it was 16,000 create/destroy pairs for nothing. Destroying a signalled
	// semaphore with no pending operations is legal, and the GPU is idle here.
	if (mPrevSubmitSignalled)
	{
		mSubmitSemaphore[orphanIndex].reset(new VulkanSemaphore(fb->device.get()));
	}
	mPrevSubmitSignalled = false;
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

	// Hand the just-finished frame's garbage and its high-water submit serial to the slot it was using.
	FrameSlotData& ending = mFrameSlots[mFrameSlot];
	ending.LastSerial = mCurrentFrameLastSerial;
	ending.TransferDeleteList = std::move(TransferDeleteList);
	ending.DrawDeleteList = std::move(DrawDeleteList);
	mCurrentFrameLastSerial = 0;

	mFrameSlot = (mFrameSlot + 1) % framesInFlight;

	FrameSlotData& reusing = mFrameSlots[mFrameSlot];

	// Wait for THIS frame's outstanding fences and no others, identified by submit serial.
	//
	// Scanning all the fences and testing ownership is what makes that possible. A per-submit list of
	// indices cannot express it: there are only maxConcurrentSubmitCount fences for an unbounded
	// number of submits, so the retiring frame's list may name an index that a LATER frame has since
	// taken over, and waiting on it blocks until that later frame is nearly done - which quietly turns
	// two frames in flight back into one. See the note on mSubmitSerial in the header.
	//
	// Skipping a newer-serial fence is safe, not optimistic: the reuse path in FlushCommands waits and
	// resets before handing an index to a new submit, so the retiring frame's work on that index has
	// already completed. Skipping an already-cleared flag is safe for the same reason.
	//
	// n is bounded by construction here - one iteration per fence that exists, each contributing at
	// most once - which the previous per-submit loop was not: repeated indices could push n past the
	// array and smash the stack.
	VkFence waitFences[maxConcurrentSubmitCount];
	int n = 0;
	for (int idx = 0; idx < maxConcurrentSubmitCount; idx++)
	{
		// One test, not two. This used to check an outstanding flag first and the serial second, and
		// that ORDER was load-bearing: FinishFrameWait cleared the flag but left the serial stale, so
		// reading the serial first would have waited on an already-reset fence - a permanent hang.
		// With a single representation the hazard cannot be expressed.
		if (mFenceSerial[idx] == 0 || mFenceSerial[idx] > reusing.LastSerial)
			continue;   // free, or belongs to a later frame - not ours to wait on
		mFenceSerial[idx] = 0;
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
	reusing.LastSerial = 0;

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
		slot.LastSerial = 0;
		slot.TransferDeleteList.reset();
		slot.DrawDeleteList.reset();
	}
	mCurrentFrameLastSerial = 0;
}

void VkCommandBufferManager::PushGroup(const FString& name)
{
	if (!gpuStatActive)
		return;

	if (mNextTimestampQuery < MaxTimestampQueries && fb->device->GraphicsTimeQueries)
	{
		FrameSlotData &slot = mFrameSlots[mFrameSlot];
		TimestampQuery q;
		q.name = name;
		q.startIndex = mFrameSlot * MaxTimestampQueries + mNextTimestampQuery++;
		q.endIndex = 0;
		GetDrawCommands()->writeTimestamp(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, mTimestampQueryPool.get(), q.startIndex);
		mGroupStack.push_back(slot.TimeElapsedQueries.size());
		slot.TimeElapsedQueries.push_back(q);
	}
}

void VkCommandBufferManager::PopGroup()
{
	if (!gpuStatActive || mGroupStack.empty())
		return;

	TimestampQuery& q = mFrameSlots[mFrameSlot].TimeElapsedQueries[mGroupStack.back()];
	mGroupStack.pop_back();

	if (mNextTimestampQuery < MaxTimestampQueries && fb->device->GraphicsTimeQueries)
	{
		q.endIndex = mFrameSlot * MaxTimestampQueries + mNextTimestampQuery++;
		GetDrawCommands()->writeTimestamp(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, mTimestampQueryPool.get(), q.endIndex);
	}
}

// Read one slot's completed timestamps. Caller guarantees the slot's fence has been waited.
void VkCommandBufferManager::ReadFrameSlotTimestamps(int slot)
{
	FrameSlotData &data = mFrameSlots[slot];
	if (data.TimestampsUsed == 0)
		return;

	const uint32_t base = slot * MaxTimestampQueries;

	uint64_t timestamps[MaxTimestampQueries];
	// WAIT_BIT is kept although the results are already available: it costs nothing on a completed
	// range and turns any future mistake about which slot is safe to read into a visible stall
	// rather than silently garbled numbers.
	mTimestampQueryPool->getResults(base, data.TimestampsUsed, sizeof(uint64_t) * data.TimestampsUsed, timestamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

	double timestampPeriod = fb->device->PhysicalDevice.Properties.Properties.limits.timestampPeriod;

	gpuStatOutput = "";
	for (auto& q : data.TimeElapsedQueries)
	{
		if (q.endIndex <= q.startIndex)
			continue;

		// Indices are absolute in the pool; the results buffer starts at this slot's base.
		int64_t timeElapsed = max(static_cast<int64_t>(timestamps[q.endIndex - base] - timestamps[q.startIndex - base]), (int64_t)0);
		double timeNS = timeElapsed * timestampPeriod;

		FString out;
		out.Format("%s=%04.2f ms\n", q.name.GetChars(), timeNS / 1000000.0f);
		gpuStatOutput += out;
	}

	if (!gpuStatOutput.IsEmpty())
		gpuStatOutput += "(2 frames behind)\n";
}

void VkCommandBufferManager::UpdateGpuStats()
{
	// Hand this frame's range to the slot. BeginFrame reads it back two frames from now, once
	// AdvanceFrameSlot has waited the fence - see there for why the read cannot happen here.
	mFrameSlots[mFrameSlot].TimestampsUsed = (uint32_t)mNextTimestampQuery;
	mGroupStack.clear();

	gpuStatActive = keepGpuStatActive;
	keepGpuStatActive = false;
}

