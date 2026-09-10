
#pragma once

#include "zvulkan/vulkanobjects.h"
#include <list>

class VulkanRenderDevice;
class VkHardwareBuffer;
class VkHardwareDataBuffer;
class VkStreamBuffer;
class IIndexBuffer;
class IVertexBuffer;
class IDataBuffer;

class VkBufferManager
{
public:
	VkBufferManager(VulkanRenderDevice* fb);
	~VkBufferManager();

	void Init();
	void Deinit();

	IVertexBuffer* CreateVertexBuffer();
	IIndexBuffer* CreateIndexBuffer();
	IDataBuffer* CreateDataBuffer(int bindingpoint, bool ssbo, bool needsresize);

	void AddBuffer(VkHardwareBuffer* buffer);
	void RemoveBuffer(VkHardwareBuffer* buffer);

	VkHardwareDataBuffer* ViewpointUBO = nullptr;
	VkHardwareDataBuffer* LightBufferSSO = nullptr;
	VkHardwareDataBuffer* LightNodes = nullptr;
	VkHardwareDataBuffer* LightLines = nullptr;
	VkHardwareDataBuffer* LightList = nullptr;
	VkHardwareDataBuffer* BoneBufferSSO = nullptr;

	std::unique_ptr<VkStreamBuffer> MatrixBuffer;
	std::unique_ptr<VkStreamBuffer> StreamBuffer;

	// Point both stream allocators at frame slot `slot` of `slots`. See VkStreamBuffer::SetRegion.
	void SetFrameRegion(int slot, int slots);

	std::unique_ptr<IIndexBuffer> FanToTrisIndexBuffer;

private:
	void CreateFanToTrisIndexBuffer();

	VulkanRenderDevice* fb = nullptr;

	std::list<VkHardwareBuffer*> Buffers;
};

class VkStreamBuffer
{
public:
	VkStreamBuffer(VkBufferManager* buffers, size_t structSize, size_t count);
	~VkStreamBuffer();

	uint32_t NextStreamDataBlock();
	void Reset() { mStreamDataOffset = mRegionStart; }

	// Restrict allocation to one of `slots` equal regions, so that two frames in flight can
	// write concurrently without one rewinding over data the GPU is still reading.
	//
	// This costs no extra memory because the buffers are enormously oversized relative to what
	// a frame uses: MatrixBuffer is 50,000 blocks against at most ~3,500 used (one per MODIFIED
	// matrix set), StreamBuffer is 300 blocks against ~14 (one per MAX_STREAM_DATA draws). Half
	// of either still leaves 7-10x headroom. The headroom is deliberate - exhausting a buffer
	// calls WaitForStreamBuffers(), which stalls the GPU completely - so halving it must not eat
	// into the margin, and it does not.
	void SetRegion(int slot, int slots);
	uint32_t RegionStart() const { return mRegionStart; }

	VkHardwareDataBuffer* UniformBuffer = nullptr;

private:
	uint32_t mBlockSize = 0;
	uint32_t mStreamDataOffset = 0;
	uint32_t mRegionStart = 0;
	uint32_t mRegionEnd = 0;      // exclusive; 0 until SetRegion, meaning "whole buffer"
};
