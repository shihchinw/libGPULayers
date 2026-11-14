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
