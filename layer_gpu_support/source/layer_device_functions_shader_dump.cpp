/*
 * SPDX-License-Identifier: MIT
 * -----------------------------------------------------------------------------
 * Copyright (c) 2026 Arm Limited
 */

#include "device.hpp"
#include "framework/device_dispatch_table.hpp"
#include "shader_dump.hpp"

#include <cstring>
#include <mutex>

extern std::mutex g_vulkanLock;

template<>
VKAPI_ATTR VkResult VKAPI_CALL layer_vkBeginCommandBuffer<user_tag>(VkCommandBuffer commandBuffer,
                                                                    const VkCommandBufferBeginInfo* pBeginInfo)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    lock.unlock();
    const VkResult result = layer->driver.vkBeginCommandBuffer(commandBuffer, pBeginInfo);
    if (result == VK_SUCCESS && layer->shaderDump) layer->shaderDump->beginCommandBuffer(commandBuffer);
    return result;
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdBindPipeline<user_tag>(VkCommandBuffer commandBuffer,
                                                             VkPipelineBindPoint pipelineBindPoint,
                                                             VkPipeline pipeline)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->bindPipeline(commandBuffer, pipelineBindPoint, pipeline);
    lock.unlock();
    layer->driver.vkCmdBindPipeline(commandBuffer, pipelineBindPoint, pipeline);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDraw<user_tag>(VkCommandBuffer commandBuffer,
                                                     uint32_t vertexCount,
                                                     uint32_t instanceCount,
                                                     uint32_t firstVertex,
                                                     uint32_t firstInstance)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer);
    lock.unlock();
    layer->driver.vkCmdDraw(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDrawIndexed<user_tag>(VkCommandBuffer commandBuffer,
                                                            uint32_t indexCount,
                                                            uint32_t instanceCount,
                                                            uint32_t firstIndex,
                                                            int32_t vertexOffset,
                                                            uint32_t firstInstance)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer);
    lock.unlock();
    layer->driver.vkCmdDrawIndexed(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdBeginDebugUtilsLabelEXT<user_tag>(VkCommandBuffer commandBuffer,
                                                                         const VkDebugUtilsLabelEXT* pLabelInfo)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump && layer->instance->config.shader_dump_capture_debug_utils() && pLabelInfo)
    {
        layer->shaderDump->beginLabel(commandBuffer, *pLabelInfo);
    }
    lock.unlock();
    layer->driver.vkCmdBeginDebugUtilsLabelEXT(commandBuffer, pLabelInfo);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdEndDebugUtilsLabelEXT<user_tag>(VkCommandBuffer commandBuffer)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump && layer->instance->config.shader_dump_capture_debug_utils())
    {
        layer->shaderDump->endLabel(commandBuffer);
    }
    lock.unlock();
    layer->driver.vkCmdEndDebugUtilsLabelEXT(commandBuffer);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDebugMarkerBeginEXT<user_tag>(
    VkCommandBuffer commandBuffer, const VkDebugMarkerMarkerInfoEXT* pMarkerInfo)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump && layer->instance->config.shader_dump_capture_debug_utils() && pMarkerInfo)
    {
        VkDebugUtilsLabelEXT label {};
        label.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
        label.pLabelName = pMarkerInfo->pMarkerName;
        std::memcpy(label.color, pMarkerInfo->color, sizeof(label.color));
        layer->shaderDump->beginLabel(commandBuffer, label);
    }
    lock.unlock();
    layer->driver.vkCmdDebugMarkerBeginEXT(commandBuffer, pMarkerInfo);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDebugMarkerEndEXT<user_tag>(VkCommandBuffer commandBuffer)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump && layer->instance->config.shader_dump_capture_debug_utils())
    {
        layer->shaderDump->endLabel(commandBuffer);
    }
    lock.unlock();
    layer->driver.vkCmdDebugMarkerEndEXT(commandBuffer);
}

template<>
VKAPI_ATTR VkResult VKAPI_CALL layer_vkSetDebugUtilsObjectNameEXT<user_tag>(
    VkDevice device, const VkDebugUtilsObjectNameInfoEXT* pNameInfo)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(device);
    if (layer->shaderDump && layer->instance->config.shader_dump_capture_debug_utils() && pNameInfo)
    {
        layer->shaderDump->recordObjectName(*pNameInfo);
    }
    lock.unlock();
    return layer->driver.vkSetDebugUtilsObjectNameEXT(device, pNameInfo);
}

template<>
VKAPI_ATTR VkResult VKAPI_CALL layer_vkQueuePresentKHR<user_tag>(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(queue);
    lock.unlock();
    const VkResult result = layer->driver.vkQueuePresentKHR(queue, pPresentInfo);
    if (result == VK_SUCCESS && layer->shaderDump) layer->shaderDump->endFrame();
    return result;
}

// CPU call counts only: indirect command buffers are never read back.
template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDrawIndexedIndirect<user_tag>(VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    uint32_t drawCount,
    uint32_t stride)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer, true);
    lock.unlock();
    layer->driver.vkCmdDrawIndexedIndirect(commandBuffer, buffer, offset, drawCount, stride);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDrawIndexedIndirect2KHR<user_tag>(VkCommandBuffer commandBuffer,
    const VkDrawIndirect2InfoKHR* pInfo)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer, true);
    lock.unlock();
    layer->driver.vkCmdDrawIndexedIndirect2KHR(commandBuffer, pInfo);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDrawIndexedIndirectCount<user_tag>(VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer, true);
    lock.unlock();
    layer->driver.vkCmdDrawIndexedIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDrawIndexedIndirectCount2KHR<user_tag>(VkCommandBuffer commandBuffer,
    const VkDrawIndirectCount2InfoKHR* pInfo)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer, true);
    lock.unlock();
    layer->driver.vkCmdDrawIndexedIndirectCount2KHR(commandBuffer, pInfo);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDrawIndexedIndirectCountKHR<user_tag>(VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer, true);
    lock.unlock();
    layer->driver.vkCmdDrawIndexedIndirectCountKHR(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDrawIndirect<user_tag>(VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    uint32_t drawCount,
    uint32_t stride)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer, true);
    lock.unlock();
    layer->driver.vkCmdDrawIndirect(commandBuffer, buffer, offset, drawCount, stride);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDrawIndirect2KHR<user_tag>(VkCommandBuffer commandBuffer,
    const VkDrawIndirect2InfoKHR* pInfo)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer, true);
    lock.unlock();
    layer->driver.vkCmdDrawIndirect2KHR(commandBuffer, pInfo);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDrawIndirectByteCount2EXT<user_tag>(VkCommandBuffer commandBuffer,
    uint32_t instanceCount,
    uint32_t firstInstance,
    const VkBindTransformFeedbackBuffer2InfoEXT* pCounterInfo,
    uint32_t counterOffset,
    uint32_t vertexStride)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer, true);
    lock.unlock();
    layer->driver.vkCmdDrawIndirectByteCount2EXT(commandBuffer, instanceCount, firstInstance, pCounterInfo, counterOffset, vertexStride);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDrawIndirectByteCountEXT<user_tag>(VkCommandBuffer commandBuffer,
    uint32_t instanceCount,
    uint32_t firstInstance,
    VkBuffer counterBuffer,
    VkDeviceSize counterBufferOffset,
    uint32_t counterOffset,
    uint32_t vertexStride)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer, true);
    lock.unlock();
    layer->driver.vkCmdDrawIndirectByteCountEXT(commandBuffer, instanceCount, firstInstance, counterBuffer, counterBufferOffset, counterOffset, vertexStride);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDrawIndirectCount<user_tag>(VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer, true);
    lock.unlock();
    layer->driver.vkCmdDrawIndirectCount(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDrawIndirectCount2KHR<user_tag>(VkCommandBuffer commandBuffer,
    const VkDrawIndirectCount2InfoKHR* pInfo)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer, true);
    lock.unlock();
    layer->driver.vkCmdDrawIndirectCount2KHR(commandBuffer, pInfo);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDrawIndirectCountKHR<user_tag>(VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer, true);
    lock.unlock();
    layer->driver.vkCmdDrawIndirectCountKHR(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDrawMeshTasksEXT<user_tag>(VkCommandBuffer commandBuffer,
    uint32_t groupCountX,
    uint32_t groupCountY,
    uint32_t groupCountZ)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer, false);
    lock.unlock();
    layer->driver.vkCmdDrawMeshTasksEXT(commandBuffer, groupCountX, groupCountY, groupCountZ);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDrawMeshTasksIndirect2EXT<user_tag>(VkCommandBuffer commandBuffer,
    const VkDrawIndirect2InfoKHR* pInfo)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer, true);
    lock.unlock();
    layer->driver.vkCmdDrawMeshTasksIndirect2EXT(commandBuffer, pInfo);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDrawMeshTasksIndirectCount2EXT<user_tag>(VkCommandBuffer commandBuffer,
    const VkDrawIndirectCount2InfoKHR* pInfo)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer, true);
    lock.unlock();
    layer->driver.vkCmdDrawMeshTasksIndirectCount2EXT(commandBuffer, pInfo);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDrawMeshTasksIndirectCountEXT<user_tag>(VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer, true);
    lock.unlock();
    layer->driver.vkCmdDrawMeshTasksIndirectCountEXT(commandBuffer, buffer, offset, countBuffer, countBufferOffset, maxDrawCount, stride);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDrawMeshTasksIndirectEXT<user_tag>(VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    uint32_t drawCount,
    uint32_t stride)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer, true);
    lock.unlock();
    layer->driver.vkCmdDrawMeshTasksIndirectEXT(commandBuffer, buffer, offset, drawCount, stride);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDrawMultiEXT<user_tag>(VkCommandBuffer commandBuffer,
    uint32_t drawCount,
    const VkMultiDrawInfoEXT* pVertexInfo,
    uint32_t instanceCount,
    uint32_t firstInstance,
    uint32_t stride)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer, false);
    lock.unlock();
    layer->driver.vkCmdDrawMultiEXT(commandBuffer, drawCount, pVertexInfo, instanceCount, firstInstance, stride);
}

template<>
VKAPI_ATTR void VKAPI_CALL layer_vkCmdDrawMultiIndexedEXT<user_tag>(VkCommandBuffer commandBuffer,
    uint32_t drawCount,
    const VkMultiDrawIndexedInfoEXT* pIndexInfo,
    uint32_t instanceCount,
    uint32_t firstInstance,
    uint32_t stride,
    const int32_t* pVertexOffset)
{
    std::unique_lock<std::mutex> lock {g_vulkanLock};
    auto* layer = Device::retrieve(commandBuffer);
    if (layer->shaderDump) layer->shaderDump->recordDraw(commandBuffer, false);
    lock.unlock();
    layer->driver.vkCmdDrawMultiIndexedEXT(commandBuffer, drawCount, pIndexInfo, instanceCount, firstInstance, stride, pVertexOffset);
}
