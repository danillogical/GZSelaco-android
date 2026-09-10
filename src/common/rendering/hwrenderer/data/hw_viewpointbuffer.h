#pragma once
#include "tarray.h"
#include "hwrenderer/data/buffers.h"

struct HWViewpointUniforms;
class FRenderState;

class HWViewpointBuffer
{
	IDataBuffer *mBuffer;
	IDataBuffer* mBufferPipeline[HW_MAX_PIPELINE_BUFFERS];
	int mPipelineNbr;
	int mPipelinePos = 0;

	unsigned int mBufferSize;
	unsigned int mBlockAlign;
	unsigned int mUploadIndex;
	unsigned int mLastMappedIndex;
	unsigned int mByteSize;
	TArray<bool> mClipPlaneInfo;

	unsigned int mBlockSize;

	// Frame-region support, for two frames in flight on Vulkan. The buffer OFFSET is biased by
	// the region while mUploadIndex stays 0-based, because mClipPlaneInfo is indexed by the same
	// counter and is rebuilt from zero every Clear(). With one region this is all identity, so
	// the GL backend is unaffected.
	int mRegionCount = 1;
	int mRegionSlot = 0;
	unsigned int mRegionStart = 0;
	unsigned int mRegionSize = 0;

	void CheckSize();

public:
	void SetFrameRegion(int slot);

	// regionCount > 1 carves the buffer into one region per frame in flight; see mRegionCount.
	HWViewpointBuffer(int pipelineNbr = 1, int regionCount = 1);
	~HWViewpointBuffer();
	void Clear();
	int Bind(FRenderState &di, unsigned int index);
	void Set2D(FRenderState &di, int width, int height, int pll = 0);
	int SetViewpoint(FRenderState &di, HWViewpointUniforms *vp);
	unsigned int GetBlockSize() const { return mBlockSize; }
};

