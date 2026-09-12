
#ifndef _HW__VERTEXBUFFER_H
#define _HW__VERTEXBUFFER_H

#include "tarray.h"
#include "hwrenderer/data/buffers.h"
#include <atomic>
#include <mutex>

class FRenderState;
struct secplane_t;

struct FFlatVertex
{
	float x, z, y;	// world position
	float u, v;		// texture coordinates
	float lu, lv;	// lightmap texture coordinates
	float lindex;	// lightmap texture index

	void Set(float xx, float zz, float yy, float uu, float vv)
	{
		x = xx;
		z = zz;
		y = yy;
		u = uu;
		v = vv;
		lindex = -1.0f;
	}

	void Set(float xx, float zz, float yy, float uu, float vv, float llu, float llv, float llindex)
	{
		x = xx;
		z = zz;
		y = yy;
		u = uu;
		v = vv;
		lu = llu;
		lv = llv;
		lindex = llindex;
	}

	void SetVertex(float _x, float _y, float _z = 0)
	{
		x = _x;
		z = _y;
		y = _z;
	}

	void SetTexCoord(float _u = 0, float _v = 0)
	{
		u = _u;
		v = _v;
	}

};

class FFlatVertexBuffer
{
public:
	TArray<FFlatVertex> vbo_shadowdata;
	TArray<uint32_t> ibo_data;

	int mPipelineNbr;
	int mPipelinePos = 0;

	// Which pipeline slots still owe a copy of the front-of-buffer static region. Copy() writes only
	// the slot that is current; the others are brought up to date as each becomes current.
	bool mPipelineReseed[HW_MAX_PIPELINE_BUFFERS] = {};

	// Whether rotation is driven externally from the frame slot (Vulkan) rather than by this class.
	//
	// This decides whether Copy() may DEFER its write. Deferring is only safe if the reseed happens
	// at the START of a frame, before any draw - which is where Vulkan calls SetPipelinePos. GL and
	// GLES rotate from NextPipelineBuffer() inside Swap(), i.e. AFTER the frame has been drawn and
	// presented, so deferring there means the frame draws from a slot that was never filled: garbage
	// on the first frame, and the previous level's static geometry on the first frame of every level.
	// GLES is always >= 2 slots, so that is not hypothetical.
	bool mExternalPipeline = false;

	IVertexBuffer* mVertexBuffer;
	IVertexBuffer *mVertexBufferPipeline[HW_MAX_PIPELINE_BUFFERS];
	IIndexBuffer *mIndexBuffer;



	unsigned int mIndex;
	std::atomic<unsigned int> mCurIndex;
	unsigned int mNumReserved;


	unsigned int mMapStart;

	static const unsigned int BUFFER_SIZE = 2000000;
	static const unsigned int BUFFER_SIZE_TO_USE = BUFFER_SIZE-500;

public:
	enum
	{
		QUAD_INDEX = 0,
		FULLSCREEN_INDEX = 4,
		PRESENT_INDEX = 8,
		STENCILTOP_INDEX = 12,
		STENCILBOTTOM_INDEX = 16,

		NUM_RESERVED = 20
	};

	FFlatVertexBuffer(int width, int height, int pipelineNbr = 1);
	~FFlatVertexBuffer();

	void OutputResized(int width, int height);
	std::pair<IVertexBuffer *, IIndexBuffer *> GetBufferObjects() const 
	{
		return std::make_pair(mVertexBuffer, mIndexBuffer);
	}

	void Copy(int start, int count);

	// Refresh the current slot's copy of the static region [0, mIndex) if Copy() has written another
	// slot since this one was last current. NOT limited to NUM_RESERVED - see the definition.
	void ReseedStaticIfNeeded();

	FFlatVertex *GetBuffer(int index) const
	{
		FFlatVertex *ff = (FFlatVertex*)mVertexBuffer->Memory();
		return &ff[index];
	}

	FFlatVertex *GetBuffer() const
	{
		return GetBuffer(mCurIndex);
	}

	std::pair<FFlatVertex *, unsigned int> AllocVertices(unsigned int count);

	void Reset()
	{
		mCurIndex = mIndex;
	}

	// Select the buffer for this frame in flight.
	//
	// Upstream allocates mPipelineNbr vertex buffers but never actually switches between them -
	// mPipelinePos is set once in the constructor and only used by Copy() to seed the reserved
	// quads into all of them. GL gets away with that because writing a buffer the GPU is still
	// reading makes the driver rename it implicitly; Vulkan has no such magic, so the rotation has
	// to be real here. Unlike the UBOs there is no descriptor set involved - this is bound with
	// vkCmdBindVertexBuffers via GetBufferObjects(), and VkRenderState rebinds when the handle
	// changes - so switching the handle is all that is required.
	void SetPipelinePos(int pos)
	{
		mExternalPipeline = true;
		if (mPipelineNbr <= 1) return;
		int next = pos % mPipelineNbr;
		if (next != mPipelinePos)
		{
			mPipelinePos = next;
			mVertexBuffer = mVertexBufferPipeline[mPipelinePos];
		}
		ReseedStaticIfNeeded();
	}

	// GL/GLES only, from Swap(). Deliberately does NOT reseed: this runs after the frame is drawn,
	// and Copy() writes every slot eagerly when mExternalPipeline is false, so there is never
	// anything owed here. It also sits one line before WaitSync(), so a large memcpy here would
	// precede the fence that guarantees the GPU has finished with the slot.
	void NextPipelineBuffer()
	{
		mPipelinePos++;
		mPipelinePos %= mPipelineNbr;

		mVertexBuffer = mVertexBufferPipeline[mPipelinePos];
	}

	void Map()
	{
		mMapStart = mCurIndex;
		mVertexBuffer->Map();
	}

	void Unmap()
	{
		mVertexBuffer->Unmap();
		mVertexBuffer->Upload(mMapStart * sizeof(FFlatVertex), (mCurIndex - mMapStart) * sizeof(FFlatVertex));
	}

	void DropSync()
	{
		mVertexBuffer->GPUDropSync();
	}

	void WaitSync()
	{
		mVertexBuffer->GPUWaitSync();
	}

	int GetPipelinePos() 
	{ 
		return mPipelinePos; 
	}
};

#endif
