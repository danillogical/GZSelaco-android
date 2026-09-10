# Vulkan report — Selaco / GZDoom

Three findings from porting this engine to Android arm64 (Ayn Thor, Adreno 740,
Android 13), all in Vulkan device setup and submission. They are independent and
belong to different projects, so they are separated below.

Checked against upstream GZDoom `c26ce2e6c` (2026-08-10) to establish ownership.

| # | issue | owner | severity |
|---|---|---|---|
| 1 | `queueFamilyIndex = (uint32_t)-1` passed to `vkCreateDevice` | **Selaco only** | invalid usage, crashes some drivers |
| 2 | Wait order triggers a Mesa/Turnip null deref | **upstream GZDoom** | legal code, but no Adreno/KGSL support in practice |
| 3 | Wait on a binary semaphore with no pending signal | **upstream GZDoom** | undefined per spec, benign so far |

Only #1 is a bug in your own code. #2 is a two-line reordering that costs nothing
and makes the engine work on Mesa/Turnip. #3 is a spec violation that no driver
tested actually punishes.

---

## 1. `queueFamilyIndex = (uint32_t)-1` reaches `vkCreateDevice`

**Selaco-specific.** Upstream GZDoom has no `UploadFamily`; its
`VulkanDevice::CreateDevice` builds a `std::set<int> neededFamilies` and guards
both entries with `!= -1`, so it cannot hit this.

`libraries/ZVulkan/src/vulkandevice.cpp`, in the constructor:

```cpp
// Test to see if we can fit more upload queues
int rqt = (UploadFamily == GraphicsFamily ? 1 : 0) + (PresentFamily == UploadFamily ? 1 : 0);
UploadQueuesSupported = selectedDevice.Device->QueueFamilies[UploadFamily].queueCount - rqt;
```

`UploadFamily` is an `int` defaulting to `-1`, and is `-1` whenever no
upload-capable queue family was found. So this is `QueueFamilies[-1]`: an
out-of-bounds read on a `std::vector`. The garbage `queueCount` it returns leaves
`UploadQueuesSupported` positive roughly half the time, and then in
`CreateDevice`:

```cpp
for (int x = 0; x < numUploadQueues && x < UploadQueuesSupported; x++) {
    uploadFamilySlots.push_back(CreateOrModifyQueueInfo(queueCreateInfos, UploadFamily, queuePriority));
}
```

`CreateOrModifyQueueInfo` takes `uint32_t family`, so `-1` silently becomes
`4294967295` and lands in a `VkDeviceQueueCreateInfo`. Your own debug output
shows it:

```
Queue Family: 0  # of queues: 1
Queue Family: 4294967295  # of queues: 1
Graphics Family: 0
Present Family: -2
Upload Family: -1
```

`VUID-VkDeviceQueueCreateInfo-queueFamilyIndex-00381` requires
`queueFamilyIndex` to be less than `queueFamilyCount`. Qualcomm's proprietary
driver silently ignores the bogus entry. Mesa/Turnip indexes
`physical_device->queue_families[qfi]` and `device->queues[qfi]` with it and
segfaults inside `tu_CreateDevice`. Validation layers should flag it on any
platform.

Note the same function later has `if (GraphicsFamily != -1)` before
`vkGetDeviceQueue`, so the codebase already expects these to be `-1` sometimes —
`PresentFamily` is guarded three lines below the loop above. `UploadFamily` was
simply missed.

**Fix** — mirror what `PresentFamily` already does:

```cpp
if (UploadFamily >= 0)
{
    int rqt = (UploadFamily == GraphicsFamily ? 1 : 0) + (PresentFamily == UploadFamily ? 1 : 0);
    UploadQueuesSupported = selectedDevice.Device->QueueFamilies[UploadFamily].queueCount - rqt;
}
else
{
    UploadQueuesSupported = 0;
}
```

and for clarity on the loop:

```cpp
for (int x = 0; UploadFamily >= 0 && x < numUploadQueues && x < UploadQueuesSupported; x++) {
```

**Verified:** 0 crashes in 10 Turnip launches, versus 3 in 6 before, with
`4294967295` gone from the queue create list. No change on Qualcomm — it still
gets its second upload queue on family 0 (`Family: 0, # of queues: 2`), because
the guard only fires when the family genuinely was not found.

**Two things that made this expensive to find**, in case they help someone else:
the ~50% hit rate meant a single run either way "proved" a false cause, and the
fault address was **identical across two different driver builds**, which looks
like a driver-side constant but was really `qfi` being the same constant every
time.

---

## 2. Wait order prevents the engine running on Mesa/Turnip

**Upstream GZDoom**, `src/common/rendering/vulkan/system/vk_commandbuffer.cpp`
(lines 115–122 at `c26ce2e6c`; same code in Selaco):

```cpp
if (mNextSubmit > 0)
    submit.AddWait(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mSubmitSemaphore[...].get());

if (finish && framebuffers->PresentImageIndex != -1)
{
    submit.AddWait(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, framebuffers->SwapChainImageAvailableSemaphore.get());
    ...
}
```

**This code is correct.** `pWaitSemaphores` is a set of conditions that must all
be satisfied; the spec imposes no ordering, and the only per-index requirement is
that `pWaitDstStageMask[i]` pairs with `pWaitSemaphores[i]`, which `AddWait`
preserves. We are not reporting a bug in the engine here.

But Mesa/Turnip's KGSL backend has a bug that this ordering triggers.
`kgsl_syncobj_merge()` folds a submit's waits into one sync object, walking them
in the order the application passed them, with an accumulator that starts
SIGNALED. So the *first* wait decides which branch the second one takes:

| order | result |
|---|---|
| internal-submit semaphore, then acquire | converts the wrong operand, dereferences a null `queue` — segfault |
| acquire, then internal-submit semaphore | the mirror-image branch, which is written correctly |

A semaphore signalled by a previous `vkQueueSubmit` is timestamp-backed; the
swapchain acquire semaphore is fd-backed, since on Android it wraps the
`ANativeWindow` fence. The engine adds the timestamp-backed one first, so every
Turnip user segfaults on the first level load. Adding the acquire wait first
avoids it completely — and that is also the more common convention, which is
presumably why Turnip's other branch is well tested and this one was not.

**Suggested change** — move the acquire wait above the cross-submit wait:

```cpp
bool presenting = finish && framebuffers->PresentImageIndex != -1;

if (presenting)
    submit.AddWait(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, framebuffers->SwapChainImageAvailableSemaphore.get());

if (mNextSubmit > 0)
    submit.AddWait(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, mSubmitSemaphore[...].get());

if (presenting)
    submit.AddSignal(framebuffers->RenderFinishedSemaphores[framebuffers->PresentImageIndex].get());
```

**Verified** on an Adreno 740 with two independent, unmodified drivers: a pristine
upstream Mesa build, and MrPurple's stock T30 prebuilt
(`driverVersion 26.3.0-T30-1.4.359`, reporting Mesa 26.2.99) — the same binary
that segfaulted before the change. Levels load, no crashes.

The Mesa bug is being reported separately; this change is worth taking anyway,
because drivers reach Android devices via third-party prebuilts that lag Mesa
main by months, and an application cannot choose which driver the user has.

Measured performance impact: none. Steady-state frame time moved 0.06 ms on
Qualcomm and 0.20 ms on Turnip, both inside window-to-window variance, over six
runs. There is a theoretical ~2-syscall-per-frame cost to this ordering, which is
four orders of magnitude below measurable.

---

## 3. Waiting on a binary semaphore with no pending signal

**Upstream GZDoom**, same function. `mSubmitSemaphore[currentIndex]` is signalled
only when `!lastsubmit`:

```cpp
if (mNextSubmit > 0)
    submit.AddWait(..., mSubmitSemaphore[(mNextSubmit - 1) % maxConcurrentSubmitCount].get());
...
if (!lastsubmit)
    submit.AddSignal(mSubmitSemaphore[currentIndex].get());
```

but it is waited on whenever `mNextSubmit > 0`. Every frame ends via
`WaitForCommands` → `FlushCommands(finish, true, ...)`, i.e. `lastsubmit = true`,
so the last submit of each frame does not signal — and the first submit of the
next frame waits on it anyway, because `mNextSubmit` is only reset afterwards.

Waiting on a binary semaphore with no pending signal operation is undefined
behaviour. In practice every driver tested tolerates it, and fixing it did **not**
resolve the Turnip crash above (that was #2), so this is a correctness cleanup
rather than a bug fix.

Introduced around `ecd2dc6300` / `ed134c9b19` (2022); still present in master.

**Suggested change** — track whether the previous submit actually signalled:

```cpp
if (mNextSubmit > 0 && mPrevSubmitSignalled)
    submit.AddWait(..., mSubmitSemaphore[...].get());
...
if (!lastsubmit)
{
    submit.AddSignal(mSubmitSemaphore[currentIndex].get());
    mPrevSubmitSignalled = true;
}
else
{
    mPrevSubmitSignalled = false;
}
```

---

## Environment

- Ayn Thor, Snapdragon 8 Gen 2, Adreno 740, Android 13
- Engine built for arm64, Vulkan backend, SDL2
- Drivers: Qualcomm 512.676.53 (Vulkan 1.3.128); Mesa/Turnip via libadrenotools
- All findings on one device and one GPU generation

Happy to supply tombstones, symbolized backtraces, or `i_benchmark` logs for any
of the above.
