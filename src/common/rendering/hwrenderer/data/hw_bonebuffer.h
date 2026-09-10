#pragma once

#include "tarray.h"
#include "hwrenderer/data/buffers.h"
#include "common/utility/matrix.h"
#include <atomic>
#include <mutex>

class FRenderState;

class BoneBuffer
{
	IDataBuffer *mBuffer;
	IDataBuffer* mBufferPipeline[HW_MAX_PIPELINE_BUFFERS];
	int mPipelineNbr;
	int mPipelinePos = 0;

	bool mBufferType;
    std::atomic<unsigned int> mIndex;
	unsigned int mBlockAlign;
	unsigned int mBlockSize;
	unsigned int mBufferSize;
	unsigned int mByteSize;
    unsigned int mMaxUploadSize;

	// See HWViewpointBuffer::SetPipelinePos - Clear() must not rotate when the frame slot does.
	bool mExternalPipeline = false;

public:
	// Select the buffer for this frame in flight. Drives rotation instead of Clear().
	void SetPipelinePos(int pos);

	BoneBuffer(int pipelineNbr = 1);
	~BoneBuffer();

	void Clear();
	int UploadBones(const TArray<VSMatrix> &bones);
	void Map() { mBuffer->Map(); }
	void Unmap() { mBuffer->Unmap(); }
	unsigned int GetBlockSize() const { return mBlockSize; }
	bool GetBufferType() const { return mBufferType; }
	int GetBinding(unsigned int index, size_t* pOffset, size_t* pSize);

	// Only for GLES to determin how much data is in the buffer
	int GetCurrentIndex() { return mIndex; };

	// OpenGL needs the buffer to mess around with the binding.
	IDataBuffer* GetBuffer() const
	{
		return mBuffer;
	}
};
