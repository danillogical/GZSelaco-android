// 
//---------------------------------------------------------------------------
//
// Copyright(C) 2018 Christoph Oelckers
// All rights reserved.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see http://www.gnu.org/licenses/
//
//--------------------------------------------------------------------------
//
/*
** gl_viewpointbuffer.cpp
** Buffer data maintenance for per viewpoint uniform data
**
**/

#include "hwrenderer/data/shaderuniforms.h"
#include "hw_viewpointuniforms.h"
#include "hw_renderstate.h"
#include "hw_viewpointbuffer.h"
#include "hw_cvars.h"

static const int INITIAL_BUFFER_SIZE = 100;	// 100 viewpoints per frame should nearly always be enough

HWViewpointBuffer::HWViewpointBuffer(int pipelineNbr, int regionCount):
	mPipelineNbr(pipelineNbr)
{
	// Sized for all regions up front. It must NOT be resized later to add regions: Resize() ends
	// up in VkHardwareBuffer::Resize -> WaitForCommands -> FlushCommands ->
	// GetRenderState()->EndRenderPass(), and during InitializeState() the render state does not
	// exist yet, which segfaults.
	mRegionCount = regionCount < 1 ? 1 : regionCount;
	mRegionSize = INITIAL_BUFFER_SIZE;
	mBufferSize = mRegionSize * (unsigned int)mRegionCount;
	mBlockAlign = ((sizeof(HWViewpointUniforms) / screen->uniformblockalignment) + 1) * screen->uniformblockalignment;
	mByteSize = mBufferSize * mBlockAlign;

	for (int n = 0; n < mPipelineNbr; n++)
	{
		mBufferPipeline[n] = screen->CreateDataBuffer(VIEWPOINT_BINDINGPOINT, false, true);
		mBufferPipeline[n]->SetData(mByteSize, nullptr, BufferUsageType::Persistent);
	}

	Clear();
	mLastMappedIndex = UINT_MAX;
}

HWViewpointBuffer::~HWViewpointBuffer()
{
	delete mBuffer;
}


void HWViewpointBuffer::SetFrameRegion(int slot)
{
	if (slot < 0 || slot >= mRegionCount) slot = 0;
	mRegionSlot = slot;
	mRegionStart = (unsigned int)slot * mRegionSize;
	// The bound offset is a function of the region, so a repeated logical index across a region
	// change must not short-circuit the rebind in Bind().
	mLastMappedIndex = UINT_MAX;
}

void HWViewpointBuffer::CheckSize()
{
	if (mUploadIndex >= mRegionSize)
	{
		mBufferSize *= 2;
		mByteSize *= 2;
		for (int n = 0; n < mPipelineNbr; n++)
		{
			mBufferPipeline[n]->Resize(mByteSize);
		}
		// Region boundaries move, so anything already written this frame at a higher slot is now
		// at the wrong offset and that frame may glitch once. Growth needs >INITIAL_BUFFER_SIZE
		// viewpoints in one frame, i.e. exactly as rare as before regions existed.
		mRegionSize = mBufferSize / (unsigned int)mRegionCount;
		mRegionStart = (unsigned int)mRegionSlot * mRegionSize;
		mLastMappedIndex = UINT_MAX;
	}
}

int HWViewpointBuffer::Bind(FRenderState &di, unsigned int index)
{
	if (index != mLastMappedIndex)
	{
		mLastMappedIndex = index;
		mBuffer->BindRange(&di, (mRegionStart + index) * mBlockAlign, mBlockAlign);
		di.EnableClipDistance(0, mClipPlaneInfo[index]);
	}
	return index;
}

void HWViewpointBuffer::Set2D(FRenderState &di, int width, int height, int pll)
{
	HWViewpointUniforms matrices;

	matrices.mViewMatrix.loadIdentity();
	matrices.mNormalViewMatrix.loadIdentity();
	matrices.mViewHeight = 0;
	matrices.mGlobVis = 1.f;
	matrices.mPalLightLevels = pll;
	matrices.mClipLine.X = -10000000.0f;
	matrices.mShadowmapFilter = gl_shadowmap_filter;
	matrices.mLightBlendMode = 0;

	matrices.mProjectionMatrix.ortho(0, (float)width, (float)height, 0, -1.0f, 1.0f);
	matrices.CalcDependencies();

	CheckSize();
	mBuffer->Map();
	memcpy(((char*)mBuffer->Memory()) + (mRegionStart + mUploadIndex) * mBlockAlign, &matrices, sizeof(matrices));
	mBuffer->Unmap();

	mClipPlaneInfo.Push(0);

	Bind(di, mUploadIndex++);
}

int HWViewpointBuffer::SetViewpoint(FRenderState &di, HWViewpointUniforms *vp)
{
	CheckSize();
	mBuffer->Map();
	memcpy(((char*)mBuffer->Memory()) + (mRegionStart + mUploadIndex) * mBlockAlign, vp, sizeof(*vp));
	mBuffer->Unmap();

	mClipPlaneInfo.Push(vp->mClipHeightDirection != 0.f || vp->mClipLine.X > -10000000.0f);
	return Bind(di, mUploadIndex++);
}

void HWViewpointBuffer::Clear()
{
	bool needNewPipeline = mUploadIndex > 0; // Clear might be called multiple times before any actual rendering

	mUploadIndex = 0;
	mClipPlaneInfo.Clear();

	if (needNewPipeline)
	{
		mLastMappedIndex = UINT_MAX;

		mPipelinePos++;
		mPipelinePos %= mPipelineNbr;
	}

	mBuffer = mBufferPipeline[mPipelinePos];
}

