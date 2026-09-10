# Mesa MR — turnip/kgsl: fix wrong operand in kgsl_syncobj_merge

Target: https://gitlab.freedesktop.org/mesa/mesa
File: `src/freedreno/vulkan/tu_knl_kgsl.cc`

**Before filing, search the tracker for an existing `kgsl_syncobj` / `0x1b0` report.**
Not done yet.

---

## Commit message

```
tu/kgsl: convert the accumulator, not the incoming sync, in kgsl_syncobj_merge

kgsl_syncobj_merge() folds a submit's waits into one sync object. Both of its
timestamp/fd transitions convert the wrong operand, and neither converts the
timestamp already accumulated in `ret`.

In the fd case with a TS accumulator, kgsl_syncobj_ts_to_fd(sync) is called on
`sync`, which the enclosing `case KGSL_SYNCOBJ_STATE_FD` guarantees is already
an fd. It should convert `&ret`, the timestamp being demoted. Nothing in
kgsl_syncobj_init(), _import() or _reset() ever sets `queue`, so for an
fd-backed syncobj it is NULL, and timestamp_to_fd() opens with
`queue->device->fd`:

    SIGSEGV, SEGV_MAPERR, fault addr 0x1b0
    #00 kgsl_syncobj_ts_to_fd
    #01 kgsl_syncobj_merge
    #02 kgsl_queue_submit
    #03 vk_queue_submit_final

0x1b0 is offsetof(struct tu_queue, device) on aarch64. With assertions enabled
the state assert in kgsl_syncobj_ts_to_fd() fires first.

The TS/TS cross-queue branch has the same root mistake: it merges against
ret.fd, which is still -1 because `ret` is in TS state, so sync_merge() is
handed an invalid fd1.

Convert &ret in both branches and merge two valid fds. Note the fd case must
pass close_fd2 = false: sync->fd is owned by the caller's syncobj.

Signed-off-by: <NAME> <EMAIL>
```

## The diff

```diff
--- a/src/freedreno/vulkan/tu_knl_kgsl.cc
+++ b/src/freedreno/vulkan/tu_knl_kgsl.cc
@@ kgsl_syncobj_merge
          if (ret.state == KGSL_SYNCOBJ_STATE_TS) {
             if (ret.queue == sync->queue) {
                ret.timestamp = max_ts(ret.timestamp, sync->timestamp);
             } else {
-               ret.state = KGSL_SYNCOBJ_STATE_FD;
-               int sync_fd = kgsl_syncobj_ts_to_fd(sync);
-               ret.fd = sync_merge_close("tu_sync", ret.fd, sync_fd, true);
+               /* ret is still TS here, so ret.fd is -1 - convert it too. */
+               int ret_fd = kgsl_syncobj_ts_to_fd(&ret);
+               int sync_fd = kgsl_syncobj_ts_to_fd(sync);
+               ret.state = KGSL_SYNCOBJ_STATE_FD;
+               ret.fd = sync_merge_close("tu_sync", ret_fd, sync_fd, true);
                assert(ret.fd >= 0);
             }
@@
          } else if (ret.state == KGSL_SYNCOBJ_STATE_TS) {
-            ret.state = KGSL_SYNCOBJ_STATE_FD;
-            int sync_fd = kgsl_syncobj_ts_to_fd(sync);
-            ret.fd = sync_merge_close("tu_sync", ret.fd, sync_fd, true);
+            /* ret is the TS one - convert it, not sync, which is already an fd. */
+            int ret_fd = kgsl_syncobj_ts_to_fd(&ret);
+            ret.state = KGSL_SYNCOBJ_STATE_FD;
+            ret.fd = sync_merge_close("tu_sync", ret_fd, sync->fd, false);
             assert(ret.fd >= 0);
          } else {
```

## Performance: no working path is modified

Every path through kgsl_syncobj_merge(), and whether the diff touches it:

| incoming | accumulator | touched | status before |
|---|---|---|---|
| SIGNALED | any | no | fine |
| UNSIGNALED | any | no | fine (early return) |
| TS | TS, same queue | no | fine (`max_ts`) |
| TS | TS, **cross-queue** | **yes** | broken: `sync_merge()` gets fd1 = -1 |
| TS | FD | no | fine |
| TS | SIGNALED | no | fine |
| FD | FD | no | fine |
| FD | **TS** | **yes** | segfault |
| FD | SIGNALED | no | fine |

Both touched paths were already broken, so the added `kgsl_syncobj_ts_to_fd()`
call in each is the cost of doing work that previously did not happen at all -
not a regression on any path that works today.

The cross-queue branch also **leaks an fd** today: `sync_merge()` ioctls on
fd1 = -1, gets EBADF, and `sync_merge_close()` returns before its `close()`
calls, so the fd just created by `kgsl_syncobj_ts_to_fd(sync)` is never
released. It then stores state = FD with fd = -1, i.e. a wait that cannot be
satisfied.

## Reproduction

GZDoom-derived engine (Selaco) on an Ayn Thor, Adreno 740, Android 13, via
libadrenotools. A submit whose waits are, in this order:

1. a semaphore signalled by a previous `vkQueueSubmit` -> TS
2. the swapchain acquire semaphore -> FD (it wraps the ANativeWindow fence)

The accumulator starts SIGNALED, so the TS is taken first and the FD then lands
in the broken branch. Segfaults on the first level load, 100% reproducible.

Reversing the two waits avoids it entirely and takes the mirror-image branch,
which is correct - that is almost certainly why this has survived: listing the
acquire semaphore first is the common convention.

## What is and is not verified

- **fd-with-TS-accumulator branch: verified.** Assertion, symbolized backtrace,
  fault address matching `offsetof(tu_queue, device)` exactly, and the fix
  confirmed on-device over many runs.
- **TS/TS cross-queue branch: by inspection only.** Not reachable on this device
  (Turnip exposes no upload-capable queue family here, so all waits are
  same-queue and take the `max_ts` path). Included because it is the same root
  mistake, but it has never been executed. Reviewers should treat it as such.

Tested on one device and one generation (A740 / KGSL) only.
