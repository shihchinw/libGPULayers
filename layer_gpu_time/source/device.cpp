/*
 * SPDX-License-Identifier: MIT
 * ----------------------------------------------------------------------------
 * Copyright (c) 2024-2025 Arm Limited
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 * ----------------------------------------------------------------------------
 */

#include <array>
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <vector>
#include <cstring>
#include <nlohmann/json.hpp>

#include "framework/utils.hpp"

#include "device.hpp"
#include "instance.hpp"

using json = nlohmann::json;

/**
 * @brief The dispatch lookup for all of the created Vulkan instances.
 */
static std::unordered_map<void*, std::unique_ptr<Device>> g_devices;

static void patchDeviceFeatures(
    Instance& instance,
    VkPhysicalDevice physicalDevice,
    vku::safe_VkDeviceCreateInfo& createInfo,
    std::vector<std::string>& supported)
{
    bool hasSync2 = false;
    bool hasHostQueryReset = false;
    for (const auto& ext : supported)
    {
        if (ext == VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME)
        {
            hasSync2 = true;
        }
        if (ext == VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME)
        {
            hasHostQueryReset = true;
        }
    }

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

    VkPhysicalDeviceSynchronization2Features sync2FeaturesSupported{};
    sync2FeaturesSupported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;

    VkPhysicalDeviceHostQueryResetFeatures hostQueryFeaturesSupported{};
    hostQueryFeaturesSupported.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES;

    void** nextChain = &features2.pNext;
    if (hasSync2)
    {
        *nextChain = &sync2FeaturesSupported;
        nextChain = &sync2FeaturesSupported.pNext;
    }
    if (hasHostQueryReset)
    {
        *nextChain = &hostQueryFeaturesSupported;
        nextChain = &hostQueryFeaturesSupported.pNext;
    }
    *nextChain = nullptr;

    if (hasSync2 || hasHostQueryReset)
    {
        if (instance.driver.vkGetPhysicalDeviceFeatures2)
        {
            instance.driver.vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);
        }
        else if (instance.driver.vkGetPhysicalDeviceFeatures2KHR)
        {
            instance.driver.vkGetPhysicalDeviceFeatures2KHR(physicalDevice, &features2);
        }
        else
        {
            sync2FeaturesSupported.synchronization2 = VK_TRUE;
            hostQueryFeaturesSupported.hostQueryReset = VK_TRUE;
        }
    }

    if (hasSync2 && sync2FeaturesSupported.synchronization2 != VK_TRUE)
    {
        hasSync2 = false;
    }
    if (hasHostQueryReset && hostQueryFeaturesSupported.hostQueryReset != VK_TRUE)
    {
        hasHostQueryReset = false;
    }

    std::vector<std::string> extensionsToEnable;
    if (hasSync2)
    {
        bool enabled = false;
        for (uint32_t i = 0; i < createInfo.enabledExtensionCount; ++i)
        {
            if (std::strcmp(createInfo.ppEnabledExtensionNames[i], VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME) == 0)
            {
                enabled = true;
                break;
            }
        }
        if (!enabled)
        {
            extensionsToEnable.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
        }
    }
    if (hasHostQueryReset)
    {
        bool enabled = false;
        for (uint32_t i = 0; i < createInfo.enabledExtensionCount; ++i)
        {
            if (std::strcmp(createInfo.ppEnabledExtensionNames[i], VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME) == 0)
            {
                enabled = true;
                break;
            }
        }
        if (!enabled)
        {
            extensionsToEnable.push_back(VK_EXT_HOST_QUERY_RESET_EXTENSION_NAME);
        }
    }

    if (!extensionsToEnable.empty())
    {
        std::vector<std::string> extensions;
        for (uint32_t i = 0; i < createInfo.enabledExtensionCount; ++i)
        {
            extensions.push_back(createInfo.ppEnabledExtensionNames[i]);
        }
        for (const auto& ext : extensionsToEnable)
        {
            extensions.push_back(ext);
        }

        if (createInfo.ppEnabledExtensionNames)
        {
            for (uint32_t i = 0; i < createInfo.enabledExtensionCount; ++i)
            {
                delete[] createInfo.ppEnabledExtensionNames[i];
            }
            delete[] createInfo.ppEnabledExtensionNames;
        }

        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        const char** newExtArray = new const char*[createInfo.enabledExtensionCount];
        for (uint32_t i = 0; i < createInfo.enabledExtensionCount; ++i)
        {
            char* str = new char[extensions[i].size() + 1];
            std::strcpy(str, extensions[i].c_str());
            newExtArray[i] = str;
        }
        createInfo.ppEnabledExtensionNames = newExtArray;
    }

    if (hasSync2)
    {
        void* cur = const_cast<void*>(createInfo.pNext);
        vku::safe_VkPhysicalDeviceSynchronization2Features* sync2Features = nullptr;
        while (cur != nullptr)
        {
            VkBaseOutStructure* header = reinterpret_cast<VkBaseOutStructure*>(cur);
            if (header->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES)
            {
                sync2Features = reinterpret_cast<vku::safe_VkPhysicalDeviceSynchronization2Features*>(cur);
                break;
            }
            cur = header->pNext;
        }

        if (sync2Features != nullptr)
        {
            sync2Features->synchronization2 = VK_TRUE;
            LAYER_LOG("Forced synchronization2 feature to VK_TRUE in existing pNext chain.");
        }
        else
        {
            auto* newSync2Node = new vku::safe_VkPhysicalDeviceSynchronization2Features{};
            newSync2Node->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
            newSync2Node->pNext = const_cast<void*>(createInfo.pNext);
            newSync2Node->synchronization2 = VK_TRUE;
            createInfo.pNext = newSync2Node;
            LAYER_LOG("Appended VkPhysicalDeviceSynchronization2Features to VkDeviceCreateInfo pNext chain.");
        }
    }

    if (hasHostQueryReset)
    {
        void* cur = const_cast<void*>(createInfo.pNext);
        vku::safe_VkPhysicalDeviceHostQueryResetFeatures* hostQueryResetFeatures = nullptr;
        while (cur != nullptr)
        {
            VkBaseOutStructure* header = reinterpret_cast<VkBaseOutStructure*>(cur);
            if (header->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES)
            {
                hostQueryResetFeatures = reinterpret_cast<vku::safe_VkPhysicalDeviceHostQueryResetFeatures*>(cur);
                break;
            }
            cur = header->pNext;
        }

        if (hostQueryResetFeatures != nullptr)
        {
            hostQueryResetFeatures->hostQueryReset = VK_TRUE;
            LAYER_LOG("Forced hostQueryReset feature to VK_TRUE in existing pNext chain.");
        }
        else
        {
            auto* newHostResetNode = new vku::safe_VkPhysicalDeviceHostQueryResetFeatures{};
            newHostResetNode->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_QUERY_RESET_FEATURES;
            newHostResetNode->pNext = const_cast<void*>(createInfo.pNext);
            newHostResetNode->hostQueryReset = VK_TRUE;
            createInfo.pNext = newHostResetNode;
            LAYER_LOG("Appended VkPhysicalDeviceHostQueryResetFeatures to VkDeviceCreateInfo pNext chain.");
        }
    }
}

/* See header for documentation. */
const std::vector<DeviceCreatePatchPtr> Device::createInfoPatches {
    patchDeviceFeatures
};

/* See header for documentation. */
std::unique_ptr<Comms::CommsModule> Device::commsModule;

/* See header for documentation. */
std::unique_ptr<ProfileComms> Device::commsWrapper;

extern std::mutex g_vulkanLock;

/* See header for documentation. */
void Device::store(
    VkDevice handle,
    std::unique_ptr<Device> device
) {
    void* key = getDispatchKey(handle);
    g_devices.insert({ key, std::move(device) });
}

/* See header for documentation. */
Device* Device::retrieve(
    VkDevice handle
) {
    void* key = getDispatchKey(handle);
    assert(isInMap(key, g_devices));
    return g_devices.at(key).get();
}

/* See header for documentation. */
Device* Device::retrieve(
    VkQueue handle
) {
    void* key = getDispatchKey(handle);
    assert(isInMap(key, g_devices));
    return g_devices.at(key).get();
}

/* See header for documentation. */
Device* Device::retrieve(
    VkCommandBuffer handle
) {
    void* key = getDispatchKey(handle);
    assert(isInMap(key, g_devices));
    return g_devices.at(key).get();
}

/* See header for documentation. */
std::unique_ptr<Device> Device::destroy(
    VkDevice handle
) {
    void* key = getDispatchKey(handle);
    assert(isInMap(key, g_devices));

    auto device = std::move(g_devices.at(key));
    g_devices.erase(key);
    return device;
}

/* See header for documentation. */
Device::Device(
    Instance* _instance,
    VkPhysicalDevice _physicalDevice,
    VkDevice _device,
    PFN_vkGetDeviceProcAddr nlayerGetProcAddress,
    const VkDeviceCreateInfo& createInfo
):
    instance(_instance),
    physicalDevice(_physicalDevice),
    device(_device)
{
    UNUSED(createInfo);

    initDriverDeviceDispatchTable(device, nlayerGetProcAddress, driver);

    // Init the shared comms module for the first device built
    {
        std::lock_guard<std::mutex> lock { g_vulkanLock };
        if (!commsModule)
        {
            commsModule = std::make_unique<Comms::CommsModule>("lglcomms");
            commsWrapper = std::make_unique<ProfileComms>(*commsModule);
        }
    }

    // Determine the driver version and emit the preamble message
    VkPhysicalDeviceProperties deviceProperties;
    instance->driver.vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);

    LAYER_LOG("Device::Device() - Constructing device. Name: %s", deviceProperties.deviceName);

    VkPhysicalDeviceLimits deviceLimits = deviceProperties.limits;
    if (deviceLimits.timestampPeriod == 0) {
        LAYER_ERR("Selected device does not support timestamp queries!");
        return;
    }
    timestampPeriod = deviceLimits.timestampPeriod;
    LAYER_LOG("Timestamp period: %f ns", (double)timestampPeriod);

    if (driver.vkCmdWriteTimestamp2)
    {
        writeTimestampFn = &Device::writeTimestampV2;
    }
    else if (driver.vkCmdWriteTimestamp2KHR)
    {
        writeTimestampFn = &Device::writeTimestampV2KHR;
    }
    else
    {
        writeTimestampFn = &Device::writeTimestampLegacy;
    }

    if (driver.vkCmdPipelineBarrier2)
    {
        insertExecutionBarrierFn = &Device::insertExecutionBarrierV2;
    }
    else if (driver.vkCmdPipelineBarrier2KHR)
    {
        insertExecutionBarrierFn = &Device::insertExecutionBarrierV2KHR;
    }
    else
    {
        insertExecutionBarrierFn = &Device::insertExecutionBarrierLegacy;
    }

    VkQueryPoolCreateInfo queryPoolInfo {
        .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .queryType = VK_QUERY_TYPE_TIMESTAMP,
        .queryCount = QUERY_COUNT,
        .pipelineStatistics = 0,
    };

    for (size_t i = 0; i < queryPools.size(); ++i)
    {
        auto result = driver.vkCreateQueryPool(device, &queryPoolInfo, nullptr, &queryPools[i]);
        if (result != VK_SUCCESS)
        {
            LAYER_ERR("Failed vkCreateQueryPool() for timestamp query for pool index: %zu", i);
        }
    }
    LAYER_LOG("Device construction completed. Comms connected status: %s",
              (isCommsConnected() ? "TRUE" : "FALSE"));
}

Device::~Device()
{
    processDeferredFrames(true);
    for (size_t i = 0; i < POOL_COUNT; ++i)
    {
        if (queryPools[i] != VK_NULL_HANDLE)
        {
            driver.vkDestroyQueryPool(device, queryPools[i], nullptr);
        }
    }
}

void Device::writeTimestampV2(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t queryIndex)
{
    driver.vkCmdWriteTimestamp2(commandBuffer, stage, pool, queryIndex);
}

void Device::writeTimestampV2KHR(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t queryIndex)
{
    driver.vkCmdWriteTimestamp2KHR(commandBuffer, stage, pool, queryIndex);
}

void Device::writeTimestampLegacy(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t queryIndex)
{
    (void)stage;
    driver.vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, queryIndex);
}

void Device::writeTimestamp(VkCommandBuffer commandBuffer, VkPipelineStageFlags2 stage, VkQueryPool pool, uint32_t queryIndex)
{
    if (queryIndex >= QUERY_COUNT) {
        LAYER_ERR("Query pool overflow...");
    } else if (writeTimestampFn) {
        (this->*writeTimestampFn)(commandBuffer, stage, pool, queryIndex);
    }
}

void Device::insertExecutionBarrierV2(VkCommandBuffer commandBuffer)
{
    VkDependencyInfo depInfo{};
    depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    depInfo.pNext = nullptr;
    depInfo.dependencyFlags = 0;

    VkMemoryBarrier2 barrier2{};
    barrier2.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier2.pNext = nullptr;
    barrier2.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier2.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    barrier2.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier2.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;

    depInfo.memoryBarrierCount = 1;
    depInfo.pMemoryBarriers = &barrier2;

    driver.vkCmdPipelineBarrier2(commandBuffer, &depInfo);
}

void Device::insertExecutionBarrierV2KHR(VkCommandBuffer commandBuffer)
{
    VkDependencyInfo depInfo{};
    depInfo.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    depInfo.pNext = nullptr;
    depInfo.dependencyFlags = 0;

    VkMemoryBarrier2 barrier2{};
    barrier2.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2;
    barrier2.pNext = nullptr;
    barrier2.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier2.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;
    barrier2.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier2.dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT;

    depInfo.memoryBarrierCount = 1;
    depInfo.pMemoryBarriers = &barrier2;

    driver.vkCmdPipelineBarrier2KHR(commandBuffer, &depInfo);
}

void Device::insertExecutionBarrierLegacy(VkCommandBuffer commandBuffer)
{
    VkMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    barrier.pNext = nullptr;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;

    driver.vkCmdPipelineBarrier(
        commandBuffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        1, &barrier,
        0, nullptr,
        0, nullptr
    );
}

void Device::insertExecutionBarrier(VkCommandBuffer commandBuffer)
{
    if (!isFrameOfInterest)
    {
        return;
    }
    if (insertExecutionBarrierFn)
    {
        (this->*insertExecutionBarrierFn)(commandBuffer);
    }
}

bool Device::allocateAndWriteStartTimestamp(VkCommandBuffer commandBuffer, VkQueryPool& outPool, uint32_t& outStartIdx, uint32_t& outEndIdx)
{
    outPool = getQueryPoolForCommandBuffer(commandBuffer);
    uint32_t poolIdx = getPoolIndex(outPool);
    uint32_t startIdx = queryPoolIndices[poolIdx].fetch_add(2);
    if (startIdx + 1 < QUERY_COUNT)
    {
        outStartIdx = startIdx;
        outEndIdx = startIdx + 1;
        insertExecutionBarrier(commandBuffer);

        writeTimestamp(commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, outPool, startIdx);
        return true;
    }
    return false;
}

void Device::writeEndTimestampAndRecord(VkCommandBuffer commandBuffer, VkQueryPool pool, uint32_t startIdx, uint32_t endIdx)
{
    insertExecutionBarrier(commandBuffer);

    writeTimestamp(commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, pool, endIdx);
    std::lock_guard<std::mutex> lockCb(cbQueriesMutex);
    cbQueries[commandBuffer].push_back({startIdx, endIdx});
}

Device::WorkloadQuery Device::beginWorkload(VkCommandBuffer commandBuffer)
{
    WorkloadQuery q{};
    if (isFrameOfInterest)
    {
        if (!allocateAndWriteStartTimestamp(commandBuffer, q.pool, q.startIdx, q.endIdx))
        {
            LAYER_ERR("[TRACE-GPU-TIME] beginWorkload() - QUERY POOL OVERFLOW!");
            q.pool = VK_NULL_HANDLE;
        }
    }
    return q;
}

void Device::endWorkload(VkCommandBuffer commandBuffer, const WorkloadQuery& query)
{
    if (isFrameOfInterest && query.pool != VK_NULL_HANDLE)
    {
        writeEndTimestampAndRecord(commandBuffer, query.pool, query.startIdx, query.endIdx);
    }
}

void Device::beginRenderPassWorkload(VkCommandBuffer commandBuffer)
{
    if (isFrameOfInterest)
    {
        VkQueryPool pool;
        uint32_t startIdx, endIdx;
        if (allocateAndWriteStartTimestamp(commandBuffer, pool, startIdx, endIdx))
        {
            std::lock_guard<std::mutex> lockCb(cbQueriesMutex);
            activeStartQuery[commandBuffer] = startIdx;
        }
        else
        {
            LAYER_ERR("[TRACE-GPU-TIME] beginRenderPassWorkload() - QUERY POOL OVERFLOW!");
        }
    }
}

Device::RenderPassEndQuery Device::preEndRenderPassWorkload(VkCommandBuffer commandBuffer)
{
    RenderPassEndQuery q{};
    if (isFrameOfInterest)
    {
        q.pool = getQueryPoolForCommandBuffer(commandBuffer);

        std::lock_guard<std::mutex> lockCb(cbQueriesMutex);
        auto it = activeStartQuery.find(commandBuffer);
        if (it != activeStartQuery.end())
        {
            q.startIdx = it->second;
            q.endIdx = q.startIdx + 1;
            activeStartQuery.erase(it);
            q.hasActiveQuery = true;
        }
    }
    return q;
}

void Device::postEndRenderPassWorkload(VkCommandBuffer commandBuffer, const RenderPassEndQuery& query)
{
    if (query.hasActiveQuery)
    {
        writeEndTimestampAndRecord(commandBuffer, query.pool, query.startIdx, query.endIdx);
    }
}

void Device::clearFrameQueries()
{
    std::lock_guard<std::mutex> lockCb(cbQueriesMutex);
    cbQueries.clear();
    activeStartQuery.clear();
    cbPools.clear();
}

void Device::registerSubmittedWorkload(
    VkCommandBuffer commandBuffer,
    uint32_t queryIdx,
    const std::string& workloadType,
    const std::vector<std::string>& debugStack
) {
    Device::PendingQuery pq;
    pq.workloadType = workloadType;
    pq.debugStack = debugStack;
    pq.hasQuery = false;

    if (commandBuffer != VK_NULL_HANDLE)
    {
        std::lock_guard<std::mutex> lockCb(this->cbQueriesMutex);
        auto it = this->cbQueries.find(commandBuffer);
        if (it != this->cbQueries.end() && queryIdx < it->second.size())
        {
            uint32_t startIdx = it->second[queryIdx].first;
            uint32_t endIdx = it->second[queryIdx].second;

            VkQueryPool pool = VK_NULL_HANDLE;
            auto poolIt = this->cbPools.find(commandBuffer);
            if (poolIt != this->cbPools.end())
            {
                pool = poolIt->second;
            }

            if (pool != VK_NULL_HANDLE)
            {
                pq.pool = pool;
                pq.startIdx = startIdx;
                pq.endIdx = endIdx;
                pq.hasQuery = true;
            }
        }
        else
        {
            LAYER_ERR("Missing query indices for Query Index %u on CommandBuffer %p for Workload: %s", queryIdx, (void*)commandBuffer, workloadType.c_str());
        }
    }

    {
        std::lock_guard<std::mutex> lockPending(this->pendingMutex);
        this->currentFrameQueries.push_back(pq);
    }
}

void Device::processDeferredFrames(bool flushAll)
{
    std::vector<PendingFrame> framesToProcess;
    {
        std::lock_guard<std::mutex> lockPending(pendingMutex);

        auto it = pendingFrames.begin();
        while (it != pendingFrames.end())
        {
            uint64_t currentFrame = currentFrameID.load();
            // If the frame age is at least POOL_COUNT, the GPU has finished executing it.
            bool ageOk = (currentFrame >= it->frameID) && (currentFrame - it->frameID >= POOL_COUNT);
            bool sizeOk = (pendingFrames.size() > POOL_COUNT - 1);

            if (!flushAll && !ageOk && !sizeOk)
            {
                break;
            }

            bool frameReady = true;
            if (!flushAll)
            {
                for (const auto& pq : it->queries)
                {
                    if (pq.hasQuery && pq.pool != VK_NULL_HANDLE)
                    {
                        std::array<uint64_t, 2> results{0, 0};
                        VkResult res = driver.vkGetQueryPoolResults(
                            device,
                            pq.pool,
                            pq.startIdx,
                            2,
                            sizeof(results),
                            results.data(),
                            sizeof(uint64_t),
                            VK_QUERY_RESULT_64_BIT
                        );
                        if (res == VK_NOT_READY || res == VK_ERROR_DEVICE_LOST)
                        {
                            frameReady = false;
                            break;
                        }
                    }
                }
            }

            if (!frameReady)
            {
                break;
            }

            framesToProcess.push_back(std::move(*it));
            it = pendingFrames.erase(it);
        }
    }

    for (const auto& frame : framesToProcess)
    {
        LAYER_LOG("Processing Frame ID: %llu with %zu query workloads.",
                  (unsigned long long)frame.frameID, frame.queries.size());
        json startFrameMessage {
            { "type", "start_frame" },
            { "frame", frame.frameID },
        };
        txMessage(startFrameMessage.dump());

        for (const auto& pq : frame.queries)
        {
            json message {
                { "type", pq.workloadType },
                { "labels", pq.debugStack },
                { "counters", json::array() }
            };

            double duration = 0.0;
            if (pq.hasQuery && pq.pool != VK_NULL_HANDLE)
            {
                std::array<uint64_t, 2> results{0, 0};

                VkResult res = driver.vkGetQueryPoolResults(
                    device,
                    pq.pool,
                    pq.startIdx,
                    2,
                    sizeof(results),
                    results.data(),
                    sizeof(uint64_t),
                    VK_QUERY_RESULT_64_BIT
                );

                if (res == VK_SUCCESS)
                {
                    uint64_t startTimestamp = results[0];
                    uint64_t endTimestamp = results[1];
                    if (endTimestamp >= startTimestamp)
                    {
                        duration = static_cast<double>(endTimestamp - startTimestamp) * static_cast<double>(timestampPeriod) / 1000000.0;
                    }
                }
                else
                {
                    LAYER_ERR("Query FAILED - vkGetQueryPoolResults returned error: %d for workload: %s",
                              res, pq.workloadType.c_str());
                }
            }
            else
            {
                LAYER_ERR("Query SKIPPED (No valid query attached) - Workload: %s",
                          pq.workloadType.c_str());
            }

            json counter {
                { "Time", duration },
            };
            message["counters"].push_back(counter);

            txMessage(message.dump());
        }

        json endFrameMessage {
            { "type", "end_frame" }
        };
        txMessage(endFrameMessage.dump());

        uint32_t poolIdx = frame.poolIdx;
        if (driver.vkResetQueryPool)
        {
            driver.vkResetQueryPool(device, queryPools[poolIdx], 0, QUERY_COUNT);
        }
        else if (driver.vkResetQueryPoolEXT)
        {
            driver.vkResetQueryPoolEXT(device, queryPools[poolIdx], 0, QUERY_COUNT);
        }
        queryPoolIndices[poolIdx] = 0;
    }
}
