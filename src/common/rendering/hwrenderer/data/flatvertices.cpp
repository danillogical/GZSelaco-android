/*
** hw_flatvertices.cpp
** Creates flat vertex data for hardware rendering.
**
**---------------------------------------------------------------------------
** Copyright 2010-2020 Christoph Oelckers
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
*/

#include "c_cvars.h"
#include "c_dispatch.h"
#include "flatvertices.h"
#include "v_video.h"
#include "cmdlib.h"
#include "printf.h"
#include "hwrenderer/data/buffers.h"

//==========================================================================
//
//
//
//==========================================================================

FFlatVertexBuffer::FFlatVertexBuffer(int width, int height, int pipelineNbr):
	mPipelineNbr(pipelineNbr)
{
	vbo_shadowdata.Resize(NUM_RESERVED);

	// the first quad is reserved for handling coordinates through uniforms.
	vbo_shadowdata[0].Set(0, 0, 0, 0, 0);
	vbo_shadowdata[1].Set(1, 0, 0, 0, 0);
	vbo_shadowdata[2].Set(2, 0, 0, 0, 0);
	vbo_shadowdata[3].Set(3, 0, 0, 0, 0);

	// and the second one for the fullscreen quad used for blend overlays.
	vbo_shadowdata[4].Set(0, 0, 0, 0, 0);
	vbo_shadowdata[5].Set(0, (float)height, 0, 0, 1);
	vbo_shadowdata[6].Set((float)width, 0, 0, 1, 0);
	vbo_shadowdata[7].Set((float)width, (float)height, 0, 1, 1);

	// and this is for the postprocessing copy operation
	vbo_shadowdata[8].Set(-1.0f, -1.0f, 0, 0.0f, 0.0f);
	vbo_shadowdata[9].Set(3.0f, -1.0f, 0, 2.f, 0.0f);
	vbo_shadowdata[10].Set(-1.0f, 3.0f, 0, 0.0f, 2.f);
	vbo_shadowdata[11].Set(3.0f, 3.0f, 0, 2.f, 2.f); // Note: not used anymore

	// The next two are the stencil caps.
	vbo_shadowdata[12].Set(-32767.0f, 32767.0f, -32767.0f, 0, 0);
	vbo_shadowdata[13].Set(-32767.0f, 32767.0f, 32767.0f, 0, 0);
	vbo_shadowdata[14].Set(32767.0f, 32767.0f, 32767.0f, 0, 0);
	vbo_shadowdata[15].Set(32767.0f, 32767.0f, -32767.0f, 0, 0);

	vbo_shadowdata[16].Set(-32767.0f, -32767.0f, -32767.0f, 0, 0);
	vbo_shadowdata[17].Set(-32767.0f, -32767.0f, 32767.0f, 0, 0);
	vbo_shadowdata[18].Set(32767.0f, -32767.0f, 32767.0f, 0, 0);
	vbo_shadowdata[19].Set(32767.0f, -32767.0f, -32767.0f, 0, 0);

	mIndexBuffer = screen->CreateIndexBuffer();
	int data[4] = {};
	mIndexBuffer->SetData(4, data, BufferUsageType::Static); // On Vulkan this may not be empty, so set some dummy defaults to avoid crashes.


	for (int n = 0; n < mPipelineNbr; n++)
	{
		mVertexBufferPipeline[n] = screen->CreateVertexBuffer();

		unsigned int bytesize = BUFFER_SIZE * sizeof(FFlatVertex);
		mVertexBufferPipeline[n]->SetData(bytesize, nullptr, BufferUsageType::Persistent);

		static const FVertexBufferAttribute format[] = {
			{ 0, VATTR_VERTEX, VFmt_Float3, (int)myoffsetof(FFlatVertex, x) },
			{ 0, VATTR_TEXCOORD, VFmt_Float2, (int)myoffsetof(FFlatVertex, u) },
			{ 0, VATTR_LIGHTMAP, VFmt_Float3, (int)myoffsetof(FFlatVertex, lu) },
		};

		mVertexBufferPipeline[n]->SetFormat(1, 3, sizeof(FFlatVertex), format);
	}

	mVertexBuffer = mVertexBufferPipeline[mPipelinePos];

	mIndex = mCurIndex = NUM_RESERVED;
	mNumReserved = NUM_RESERVED;
	Copy(0, NUM_RESERVED);
}

//==========================================================================
//
//
//
//==========================================================================

FFlatVertexBuffer::~FFlatVertexBuffer()
{
	for (int n = 0; n < mPipelineNbr; n++)
	{
		delete mVertexBufferPipeline[n];
	}

	delete mIndexBuffer;
	mIndexBuffer = nullptr;
	mVertexBuffer = nullptr;
}

//==========================================================================
//
//
//
//==========================================================================

void FFlatVertexBuffer::OutputResized(int width, int height)
{
	vbo_shadowdata[4].Set(0, 0, 0, 0, 0);
	vbo_shadowdata[5].Set(0, (float)height, 0, 0, 1);
	vbo_shadowdata[6].Set((float)width, 0, 0, 1, 0);
	vbo_shadowdata[7].Set((float)width, (float)height, 0, 1, 1);
	Copy(4, 4);
}

//==========================================================================
//
//
//
//==========================================================================

std::pair<FFlatVertex *, unsigned int> FFlatVertexBuffer::AllocVertices(unsigned int count)
{
	FFlatVertex *p = GetBuffer();
	auto index = mCurIndex.fetch_add(count);
	if (index + count >= BUFFER_SIZE_TO_USE)
	{
		// If a single scene needs 2'000'000 vertices there must be something very wrong.
		I_FatalError("Out of vertex memory. Tried to allocate more than %u vertices for a single frame", index + count);
	}

	// Track the real high-water mark, because BUFFER_SIZE is the single largest memory decision in the
	// renderer and nothing measures it. 2,000,000 vertices is 61 MiB per pipeline copy - 96% of the
	// memory that two frames in flight adds - and the static geometry alone is only ~102k, so the
	// constant is provisioned roughly 20x above anything observed. It cannot be shrunk on a guess,
	// because an overrun is I_FatalError rather than a wasted page. Print with `vertexpeak`.
	unsigned int seen = index + count;
	unsigned int prev = mPeakIndex.load(std::memory_order_relaxed);
	while (seen > prev && !mPeakIndex.compare_exchange_weak(prev, seen, std::memory_order_relaxed))
		;

	return std::make_pair(p, index);
}

// Reports the largest vertex index any single frame has actually needed, against the compile-time
// ceiling. Deliberately a CCMD rather than a stat: the number only means anything after a real play
// session, and the point of it is to decide a constant, not to watch it live.
CCMD(vertexpeak)
{
	if (screen == nullptr || screen->mVertexData == nullptr)
	{
		Printf("No vertex buffer yet.\n");
		return;
	}
	FFlatVertexBuffer *fvb = screen->mVertexData;
	unsigned int peak = fvb->GetPeakIndex();
	Printf("Flat vertex high-water: %u of %u (%.1f%% of BUFFER_SIZE)\n",
		peak, FFlatVertexBuffer::BUFFER_SIZE, 100.0 * peak / FFlatVertexBuffer::BUFFER_SIZE);
	Printf("  static region (mIndex): %u\n", fvb->mIndex);
	Printf("  per copy: %.2f MB, x%d pipeline copies = %.2f MB\n",
		FFlatVertexBuffer::BUFFER_SIZE * sizeof(FFlatVertex) / 1048576.0, fvb->mPipelineNbr,
		fvb->mPipelineNbr * FFlatVertexBuffer::BUFFER_SIZE * sizeof(FFlatVertex) / 1048576.0);
}

//==========================================================================
//
//
//
//==========================================================================

void FFlatVertexBuffer::Copy(int start, int count)
{
	// Defer ONLY when rotation is driven from the frame slot, i.e. Vulkan. Mark every slot -
	// including the current one - and let ReseedStaticIfNeeded do the write from SetPipelinePos,
	// which runs at the top of BeginFrame after AdvanceFrameSlot has waited on that slot's fence.
	//
	// Writing "just the current slot" here is NOT safe on Vulkan either.
	// OutputResized reaches Copy via DFrameBuffer::Update, and VulkanRenderDevice::Update calls
	// Super::Update() immediately after WaitForCommands(true) - which submits and presents WITHOUT
	// waiting. The current slot is therefore precisely the one whose command buffers were submitted
	// three lines earlier and are still fetching vertices from this Persistent, permanently mapped
	// buffer, where Upload() is a no-op and nothing serialises the store.
	//
	// The reseed covers [0, mIndex), a superset of any (start, count) a caller can pass, so the range
	// arguments only matter on the eager path below.
	if (mExternalPipeline && mPipelineNbr > 1)
	{
		for (int n = 0; n < mPipelineNbr; n++)
			mPipelineReseed[n] = true;
		return;
	}

	// Eager path: every slot, written now. Used by GL/GLES, which rotate from Swap() AFTER drawing so
	// there is no start-of-frame point to defer to, and by the constructor before any backend has
	// claimed rotation. Safe for GL because writing a buffer the GPU may still be reading makes the
	// driver rename it implicitly, and safe in the constructor because nothing is in flight yet.
	//
	// This is also the only path where the source offset matters - &vbo_shadowdata[start], not [0].
	// OutputResized computes the new fullscreen quad into vbo_shadowdata[4..7] and calls Copy(4, 4);
	// a [0] source writes the QUAD_INDEX marker quad over it instead. Upstream GZDoom has the same
	// line. The deferred path above sidesteps it by rewriting the whole reserved region.
	IVertexBuffer* old = mVertexBuffer;

	for (int n = 0; n < mPipelineNbr; n++)
	{
		mVertexBuffer = mVertexBufferPipeline[n];
		Map();
		memcpy(GetBuffer(start), &vbo_shadowdata[start], count * sizeof(FFlatVertex));
		Unmap();
		mVertexBuffer->Upload(start * sizeof(FFlatVertex), count * sizeof(FFlatVertex));
		mPipelineReseed[n] = false;
	}

	mVertexBuffer = old;
}

// Bring the current slot's static region up to date. On Vulkan this runs from SetPipelinePos in
// BeginFrame, after AdvanceFrameSlot has waited on this slot's fence - the only point at which
// writing it is safe.
//
// THE RANGE IS [0, mIndex), NOT NUM_RESERVED. That distinction is the whole bug in the first
// attempt at this. `mNumReserved` is 20 - the uniform quad, fullscreen quad, present quad and
// stencil caps - but the front-of-buffer region Copy() maintains is `mIndex`, which
// CreateVBO (hw_vertexbuilder.cpp:490) grows to the entire static sector geometry via
// `Copy(0, fvb->mIndex)`; on a real level that is ~102k vertices. Refreshing only 20 left the other
// slot with none of the level's static flats, which alternated every frame and looked exactly like
// dynamic lights flickering while standing still.
void FFlatVertexBuffer::ReseedStaticIfNeeded()
{
	if (!mPipelineReseed[mPipelinePos])
		return;

	// Validate BEFORE consuming the flag. Clearing it first and then bailing out would leave this slot
	// holding stale static geometry indefinitely - until some later Copy() happened to re-mark it -
	// which is the same wrong-geometry-every-other-frame symptom described above, except permanent
	// instead of alternating. mIndex can legitimately exceed vbo_shadowdata.Size() in the window where
	// CreateVertices has shrunk the array back to NUM_RESERVED but CreateVBO has not yet updated
	// mIndex.
	const unsigned int count = mIndex;
	if (count == 0 || count > vbo_shadowdata.Size())
		return;

	mPipelineReseed[mPipelinePos] = false;

	Map();
	memcpy(GetBuffer(0), &vbo_shadowdata[0], count * sizeof(FFlatVertex));
	Unmap();
	mVertexBuffer->Upload(0, count * sizeof(FFlatVertex));
}

