/*
** i_auxcanvas.cpp
** Offscreen 2D canvas and CPU readback for the second-screen panel.
**
**---------------------------------------------------------------------------
** Copyright 2026 Selaco Android port contributors
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
** A second screen cannot have a second Vulkan surface: SDL refuses a second Android window, the
** surface belongs to the device rather than the swapchain, and anything that takes focus makes
** AppActive false and freezes BOTH screens. So the panel is fed the other way round - the engine
** draws into an ordinary offscreen canvas that it already knows how to render, and the pixels are
** copied back to host memory for Java to blit. No extra surface, no swapchain, no present queue.
**
** The canvas itself is AUXCANVAS, declared in our own wadsrc/static/animdefs.txt. The engine's
** existing AllCanvases loop (hw_entrypoint.cpp) renders it inside the normal frame, so drawing
** costs no extra submit; only the readback stalls, and that is why it is edge-triggered rather
** than per frame.
**
** WHAT THIS COSTS THE MAIN RENDER PATH, because it is not free and an earlier version of this header
** implied it was: VK_IMAGE_USAGE_TRANSFER_SRC_BIT is now on every hardware canvas image
** (vk_hwtexture.cpp, the isHardwareCanvas branch), and I_AuxCanvasReadback records a copy plus two
** layout transitions and then drains every outstanding fence - ~30 ms in a real level.
**
** Four entry points, all called from i_auxpanel.cpp: I_AuxCanvasClear, I_AuxCanvasDrawTestPattern,
** I_AuxCanvasRenderPending and I_AuxCanvasReadback. The test pattern is a diagnostic (aux_canvas_zscript
** 0) rather than the only content: real content comes from i_auxcodexview.cpp.
*/

#include <memory>

#include <zvulkan/vulkanbuilders.h>
#include <zvulkan/vulkanobjects.h>

#include "c_cvars.h"
#include "printf.h"
#include "v_2ddrawer.h"
#include "v_video.h"
#include "textures.h"
#include "zstring.h"
#include "vulkan/system/vk_renderdevice.h"
#include "vulkan/system/vk_commandbuffer.h"
#include "vulkan/renderer/vk_renderstate.h"
#include "vulkan/textures/vk_hwtexture.h"
#include "vulkan/textures/vk_imagetransition.h"

#include "i_auxvmreflect.h"   // GetTextureCanvas, declared once for the aux translation units

// Gate for who owns this canvas. 0 = the C++ test pattern below; 3 = the whole PDAMenu3 desktop, all
// six tabs, drawn by VM reflection; 4 = "Wii U mode", where the player's OWN PDA is moved to this
// canvas by redirecting the engine's M_Drawer into it while the menu is open; 5 = both, resolved per
// frame - 3 while the PDA is shut, 4 while it is open. See i_auxcodexview.cpp for mode 3, and MODE 4 in
// i_auxmenuview.cpp, plus the mode
// dispatch in i_auxpanel.cpp.
//
// 1 and 2 ARE GONE and the numbering is deliberately left with holes in it, so notes and muscle memory
// that name a mode still name the same one. 1 was a ZScript test pattern and 2 was Selaco's
// PDAManualWindow on its own; mode 3 draws the whole desktop including that same Manual tab, so 2 had
// nothing left to offer. i_auxpanel.cpp says so once if either is selected.
//
// Exactly one of them ever draws in a given frame, because they all write the same AUXCANVAS and with
// more than one active they would fight over it, and an in-flight frame-cost measurement must not be
// perturbed by any of them drawing unasked. 5 is not a third implementation: it selects between 3 and 4,
// which keep their own code, latches and cvar values so a failure can still be attributed to one piece.
//
// Modes 0 and 3 are projections nobody asked for and are edge-triggered on a content change. Mode 4 is
// the player's own menu, is live, and is paced. 5 is the shipping combination.
//
// DEFAULT 5, because this stopped being a diagnostic switch and became the feature switch. It was 0,
// which meant a fresh install put the blue/green/red test pattern on the second screen and the player
// had no way to reach the dashboard - the shipped autoexec.cfg sets nothing, and flags 0 means a
// console `aux_canvas_zscript 5` does not survive a restart. Modes 0, 3 and 4 are now the diagnostics
// you select deliberately, and 5 is what you get by not choosing.
//
// Flags stay 0 rather than becoming CVAR_ARCHIVE, and that is deliberate on two counts. Archiving it
// would make a debugging session's mode stick silently forever, which is the failure CLAUDE.md
// describes - an archived cvar left unset is not "default", it is whatever the last profile did. And
// adding it to the shipped autoexec instead would break that file's own rule, which is that a cvar
// belongs there only when Selaco's menus and defaults cannot express it. Here the default can.
//
// aux_panel (CVAR_ARCHIVE | CVAR_GLOBALCONFIG, i_auxpanel.cpp) remains the genuinely user-facing
// setting: whether the second screen is used at all. This one picks what it shows.
CVAR(Int, aux_canvas_zscript, 5, 0)

// Latched on the first failure and never retried.
//
// Same discipline as the panel: a canvas that cannot be resolved or copied will not start working,
// and retrying every frame would turn a broken optional feature into a permanent cost on the main
// render path. One diagnostic line, then silence forever.
static bool AuxCanvasBroken = false;

static FCanvas *AuxCanvas = nullptr;

// The canvas's ACTUAL size, read off the resolved texture - deliberately not named AuxCanvasWidth,
// because AuxView::AuxCanvasWidth in i_auxvmreflect.h is a different thing: the size the layout math
// in both modes ASSUMES. These are what animdefs.txt really produced; those are what the C++ expects
// it to have produced. AuxCanvasResolve compares them and says so if they have drifted apart.
static int AuxCanvasActualWidth = 0, AuxCanvasActualHeight = 0;

// Allocated on the first readback and kept for the life of the process, unlike CopyScreenToBuffer
// which builds a fresh buffer and a fresh image per call. That is fine for a one-shot screenshot
// and wrong for something that repeats: this path runs again on every content change.
static std::unique_ptr<VulkanBuffer> AuxStaging;
static uint8_t *AuxStagingPixels = nullptr;

// Resolve the canvas once and cache it. Returns nullptr and latches AuxCanvasBroken on failure.
//
// The pointer is safe to hold because FCanvasTexture owns its FCanvas for the texture's whole life
// and the texture manager owns the texture for the life of the process. A full texture-manager
// reinitialisation would invalidate it, which does not happen on this target.
static FCanvas *AuxCanvasResolve()
{
	if (AuxCanvasBroken)
		return nullptr;
	if (AuxCanvas != nullptr)
		return AuxCanvas;

	FCanvas *canvas = GetTextureCanvas(AuxView::AuxCanvasName);
	if (canvas == nullptr || canvas->Tex == nullptr)
	{
		Printf(TEXTCOLOR_YELLOW "AuxCanvas: %s is not a canvas texture, offscreen canvas disabled\n", AuxView::AuxCanvasName);
		AuxCanvasBroken = true;
		return nullptr;
	}

	AuxCanvas = canvas;
	AuxCanvasActualWidth = canvas->Tex->GetWidth();
	AuxCanvasActualHeight = canvas->Tex->GetHeight();

	// The one place the animdefs lump and the C++ can be checked against each other. animdefs.txt is
	// authoritative - it is what actually creates the texture - but both modes lay their menu out
	// against the compile-time AuxView constants, and DesktopBaselineHeight is derived from them. Edit
	// the lump without editing the header and nothing errors: the desktop is simply laid out for a
	// panel that is not the one it lands on, which shows up as the clipped tab bar ("ALOGS" at the left
	// edge, "MAI" at the right) rather than as anything that names the cause. Warn rather than latch -
	// a wrongly-scaled panel still beats no panel, and the readback does not care about the size.
	if (AuxCanvasActualWidth != (int)AuxView::AuxCanvasWidth || AuxCanvasActualHeight != (int)AuxView::AuxCanvasHeight)
	{
		Printf(TEXTCOLOR_YELLOW "AuxCanvas: %s is %dx%d but i_auxvmreflect.h expects %dx%d - the desktop "
			"will be laid out for the wrong panel and the tab bar will clip. Keep the canvastexture line "
			"in wadsrc/static/animdefs.txt and the AuxView canvas constants in step.\n",
			AuxView::AuxCanvasName, AuxCanvasActualWidth, AuxCanvasActualHeight,
			(int)AuxView::AuxCanvasWidth, (int)AuxView::AuxCanvasHeight);
	}

	// The format is derived rather than read back: VulkanImage does not store the VkFormat it was
	// created with, so vk_hwtexture.cpp's own expression is the only source of truth for it.
	// R8G8B8A8_UNORM is byte-identical to Android ARGB_8888 and needs no conversion; SFLOAT would
	// mean the buffer size, the byte layout and the Java-side Bitmap config are all wrong.
	const bool hdr = canvas->Tex->IsHDR();
	Printf("AuxCanvas: resolved %s %dx%d, format %s\n", AuxView::AuxCanvasName, AuxCanvasActualWidth, AuxCanvasActualHeight,
		hdr ? "VK_FORMAT_R32G32B32A32_SFLOAT" : "VK_FORMAT_R8G8B8A8_UNORM");

	return AuxCanvas;
}

// Queue a fixed test pattern into the canvas and mark it dirty so the frame's AllCanvases loop
// renders it.
//
// THE ASYMMETRY IS THE POINT. A vertical flip, an R/B channel swap and a wrong stride or copy
// region are three different bugs that all produce a plausible-looking image, and a centred or
// symmetric pattern would hide the first two entirely. With this layout one screenshot separates
// them: the green bar names the left edge, the blue bar names the top edge, the red block names the
// top-left corner by colour as well as position, and the lone white block names the bottom-right.
//
// Colour-only quads because a test pattern needs nothing more, NOT because Shape2D is unsafe on a
// canvas: buffersToDestroy (src/common/2d/v_2ddrawer.cpp:580) is a file-scope global rather than a
// per-drawer member, so the twod->OnFrameDone() that runs every frame (src/d_main.cpp:932) drains
// shapes pushed by a canvas drawer just as it drains the main screen's.
// Paint the whole canvas opaque black.
//
// Required before any owner draws something that does not cover every pixel. RenderTextureView does
// not clear the attachment, and canvas->Drawer.Clear() in the AllCanvases loop clears the COMMAND
// LIST rather than the texture - so the texture keeps whatever the last owner left there. That
// showed up on device as the C++ test pattern's blue/green/red/white edges framing the PDA desktop:
// the pattern had been drawn at the title screen, the desktop drew over it once the level loaded,
// and everything the desktop did not cover was still the pattern.
void I_AuxCanvasClear()
{
	FCanvas *canvas = AuxCanvasResolve();
	if (canvas == nullptr)
		return;

	canvas->Drawer.AddColorOnlyQuad(0, 0, AuxCanvasActualWidth, AuxCanvasActualHeight, PalEntry(255, 0, 0, 0));
	canvas->Tex->NeedUpdate();
}

void I_AuxCanvasDrawTestPattern()
{
	FCanvas *canvas = AuxCanvasResolve();
	if (canvas == nullptr)
		return;

	const int w = AuxCanvasActualWidth;
	const int h = AuxCanvasActualHeight;

	canvas->Drawer.AddColorOnlyQuad(0, 0, w, h, PalEntry(255, 24, 24, 32));	// dark fill
	canvas->Drawer.AddColorOnlyQuad(0, 0, 155, 135, PalEntry(255, 255, 0, 0));	// red, top-left block
	canvas->Drawer.AddColorOnlyQuad(0, 0, 40, h, PalEntry(255, 0, 255, 0));	// green, left edge
	canvas->Drawer.AddColorOnlyQuad(0, 0, w, 40, PalEntry(255, 0, 0, 255));	// blue, top edge
	canvas->Drawer.AddColorOnlyQuad(w - 100, h - 100, 100, 100, PalEntry(255, 255, 255, 255));	// white, bottom-right

	// Without this the canvas is never picked up: CheckNeedsUpdate is what the AllCanvases loop
	// gates on, and RenderTextureView clears the flag again on the way out.
	canvas->Tex->NeedUpdate();
}

// Whether a queued draw has not been rendered into the canvas yet.
//
// THIS IS WHAT MAKES AN EDGE-TRIGGERED READBACK CORRECT, and it is not obvious. The AllCanvases loop
// that actually renders this canvas lives in RenderView (hw_entrypoint.cpp:363), which d_main.cpp
// calls at :1115 - AFTER I_AuxPanelFrame at :1102. So a draw queued during I_AuxPanelFrame does not
// reach the image until later in the SAME frame, and a readback issued immediately after the draw
// returns the PREVIOUS contents. With a fixed 30-frame interval that only meant the panel lagged by
// one interval and nobody noticed, because both test patterns are static. With a readback that fires
// once per content change it would mean the new content is never pushed at all.
//
// Two other reasons the answer can be "still pending" for a while: RenderView is only called for
// GS_LEVEL and GS_TITLELEVEL with gametic != 0 (d_main.cpp:1107), and a canvas that has never
// rendered has nothing to copy. A caller that keeps waiting is behaving correctly in both cases.
bool I_AuxCanvasRenderPending()
{
	FCanvas *canvas = AuxCanvasResolve();
	if (canvas == nullptr)
		return false;

	return canvas->Tex->CheckNeedsUpdate() || canvas->Tex->bFirstUpdate;
}

// Copy the rendered canvas into host-visible memory and hand back the mapped RGBA8 bytes.
//
// Raw RGBA8, no vertical flip and no 24-bit conversion: the consumer is an Android Bitmap, and the
// screenshot converter this is modelled on does both of those things only because a screenshot file
// wants them.
//
// This stalls the GPU. WaitForCommands(false) waits on every outstanding fence and then destroys
// both frame slots' delete lists on the assumption the device is idle, so it belongs OUTSIDE a
// frame - the savepic path sets the same precedent. It is also why this is edge-triggered by the
// caller rather than run every frame.
bool I_AuxCanvasReadback(const uint8_t **outPixels, int *outWidth, int *outHeight)
{
	if (outPixels == nullptr || outWidth == nullptr || outHeight == nullptr)
		return false;

	FCanvas *canvas = AuxCanvasResolve();
	if (canvas == nullptr)
		return false;

	// Nothing has rendered into the canvas yet, so its image either does not exist or holds
	// undefined contents. Copying it would return garbage that reads as a broken copy rather than
	// as "too early", which is the more expensive of the two to diagnose.
	if (canvas->Tex->bFirstUpdate)
		return false;

	if (screen == nullptr || !screen->IsVulkan())
	{
		Printf(TEXTCOLOR_YELLOW "AuxCanvas: readback needs the Vulkan backend, offscreen canvas disabled\n");
		AuxCanvasBroken = true;
		return false;
	}

	auto fb = static_cast<VulkanRenderDevice *>(screen);
	auto hwtex = static_cast<VkHardwareTexture *>(canvas->Tex->GetHardwareTexture(0, 0));
	VkTextureImage *image = hwtex != nullptr ? hwtex->GetImage(canvas->Tex, 0, 0) : nullptr;
	if (image == nullptr || !image->Image)
	{
		Printf(TEXTCOLOR_YELLOW "AuxCanvas: no canvas image to copy from, offscreen canvas disabled\n");
		AuxCanvasBroken = true;
		return false;
	}

	// The image may have been created larger than the texture asked for, and the copy region has to
	// describe the image or the rows land at the wrong stride.
	const int w = image->Image->width;
	const int h = image->Image->height;
	const size_t bytes = (size_t)w * (size_t)h * 4;

	try
	{
		if (!AuxStaging)
		{
			// HOST_COHERENT is REQUIRED, not merely preferred, because the mapping is permanent and
			// ZVulkan exposes no way to invalidate a range - on non-coherent memory the host would
			// read whatever the CPU cache happened to hold. HOST_CACHED is asked for on top of that
			// purely so the readback itself is not an uncached walk over 5 MB.
			AuxStaging = BufferBuilder()
				.Size(bytes)
				.Usage(VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU)
				.MemoryType(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
					VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
				.DebugName("AuxCanvasStaging")
				.Create(fb->device.get());

			AuxStagingPixels = (uint8_t *)AuxStaging->Map(0, bytes);
			if (AuxStagingPixels == nullptr)
			{
				AuxStaging.reset();
				Printf(TEXTCOLOR_YELLOW "AuxCanvas: staging buffer could not be mapped, offscreen canvas disabled\n");
				AuxCanvasBroken = true;
				return false;
			}
		}
		else if (AuxStaging->size < bytes)
		{
			// The canvas cannot change size without the texture being recreated, so this is a "the
			// world is not what this code assumes" case rather than a resize to handle.
			Printf(TEXTCOLOR_YELLOW "AuxCanvas: canvas grew past its staging buffer, offscreen canvas disabled\n");
			AuxCanvasBroken = true;
			return false;
		}

		// A barrier cannot be recorded inside a render pass, and RenderTextureView ends the pass the
		// same way before its own transitions.
		fb->GetRenderState()->EndRenderPass();

		VulkanCommandBuffer *cmds = fb->GetCommands()->GetDrawCommands();

		VkImageTransition()
			.AddImage(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false)
			.Execute(cmds);

		VkBufferImageCopy region = {};
		region.imageExtent.width = w;
		region.imageExtent.height = h;
		region.imageExtent.depth = 1;
		region.imageSubresource.layerCount = 1;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		cmds->copyImageToBuffer(image->Image->image, image->Layout, AuxStaging->buffer, 1, &region);

		// Restore the layout RenderTextureView leaves the canvas in, in the SAME submit as the copy.
		// The canvas is an ordinary sampled texture to the rest of the engine, and leaving it in
		// TRANSFER_SRC would silently break anything that draws it. Recording it before the stall
		// also means the barrier orders the copy's read ahead of the transition for free.
		VkImageTransition()
			.AddImage(image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, false)
			.Execute(cmds);

		// Submit and wait. The fence signal is what makes the transfer writes visible to the host,
		// so no further barrier is needed before reading the mapping.
		fb->WaitForCommands(false);
	}
	catch (const std::exception &e)
	{
		Printf(TEXTCOLOR_YELLOW "AuxCanvas: readback failed (%s), offscreen canvas disabled\n", e.what());
		AuxCanvasBroken = true;
		return false;
	}

	*outPixels = AuxStagingPixels;
	*outWidth = w;
	*outHeight = h;
	return true;
}

// Forget the resolved canvas, for the restart teardown. See I_AuxForgetScriptState in
// i_auxvmreflect.cpp for why it exists and when it runs. Nothing here does VM work; every line is a store.
//
// This is the one place AuxCanvasResolve's stated precondition stops holding. Its comment says a full
// texture-manager reinitialisation would invalidate the cached FCanvas and does not happen on this target -
// but D_Cleanup runs TexMan.DeleteAll(), which destroys the FCanvasTexture, and ~FCanvasTexture removes the
// FCanvas from AllCanvases (its only GC root) and nulls it. Left cached, the pointer is dereferenced on the
// render path on every frame after the restart, and the new AUXCANVAS the re-parsed animdefs creates is
// never picked up because the resolve short-circuits on the stale one.
//
// AuxStaging and AuxStagingPixels are deliberately left alone. They are Vulkan memory, not engine memory,
// the device is not torn down by a restart, and i_auxpanel.cpp's AuxPixelsBuffer is a direct ByteBuffer
// over that exact mapping - retiring the buffer here would dangle a JNI reference and would mean freeing a
// resource that in-flight command buffers may still reference, during teardown.
void I_AuxCanvasForgetScriptState()
{
	AuxCanvas = nullptr;

	// Cleared so a failure latched against the previous wad set does not silently disable the second screen
	// for a session that might not have it. The cost of being wrong is one re-attempt and one yellow line.
	AuxCanvasBroken = false;

	// Zeroed rather than left: they are the extents the clear and the test pattern draw at, and the next
	// resolve is what re-reads them off the texture that actually exists.
	AuxCanvasActualWidth = 0;
	AuxCanvasActualHeight = 0;
}
