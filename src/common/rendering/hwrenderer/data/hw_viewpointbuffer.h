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

	// Set when the frame slot drives rotation from outside (Vulkan). Clear() must then NOT
	// advance the pipeline itself: the descriptor set is written once at BeginFrame, while Clear()
	// runs later and repeatedly during the frame, so a rotation inside Clear() would leave the
	// descriptor pointing at a different buffer than the one being written.
	bool mExternalPipeline = false;

	void CheckSize();

public:
	// Select the buffer for this frame in flight. Drives rotation instead of Clear().
	void SetPipelinePos(int pos);

	// The buffer currently being written, which is what a Vulkan descriptor set must bind.
	IDataBuffer* GetBuffer() const { return mBuffer; }

	HWViewpointBuffer(int pipelineNbr = 1);
	~HWViewpointBuffer();
	void Clear();
	int Bind(FRenderState &di, unsigned int index);
	void Set2D(FRenderState &di, int width, int height, int pll = 0);
	int SetViewpoint(FRenderState &di, HWViewpointUniforms *vp);
	unsigned int GetBlockSize() const { return mBlockSize; }
};

