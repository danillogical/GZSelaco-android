
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

	// Point both stream allocators at the buffer for this frame in flight.
	void SetPipelinePos(int pos);

	std::unique_ptr<IIndexBuffer> FanToTrisIndexBuffer;

private:
	void CreateFanToTrisIndexBuffer();

	VulkanRenderDevice* fb = nullptr;

	std::list<VkHardwareBuffer*> Buffers;
};

class VkStreamBuffer
{
public:
	VkStreamBuffer(VkBufferManager* buffers, size_t structSize, size_t count, int pipelineNbr = 1);
	~VkStreamBuffer();

	uint32_t NextStreamDataBlock();
	void Reset() { mStreamDataOffset = 0; }

	// Select the buffer for this frame in flight. One buffer per frame rather than regions inside
	// One buffer per frame in flight, same mechanism and same reason as the engine-side buffers;
	// see VulkanRenderDevice::InitializeState.
	void SetPipelinePos(int pos);

	// The buffer for the frame currently being recorded.
	VkHardwareDataBuffer* UniformBuffer = nullptr;

private:
	enum { maxPipelineNbr = 4 };
	VkHardwareDataBuffer* mPipeline[maxPipelineNbr] = {};
	int mPipelineNbr = 1;
	int mPipelinePos = 0;

	uint32_t mBlockSize = 0;
	uint32_t mStreamDataOffset = 0;
};
