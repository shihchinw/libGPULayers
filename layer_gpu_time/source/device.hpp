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

/**
 * @file Declares the root class for layer management of VkDevice objects.
 *
 * Role summary
 * ============
 *
 * Devices represent the core context used by the application to connect to the
 * underlying graphics driver. A device object is the dispatch root for the
 * Vulkan driver, so device commands all take some form of dispatchable handle
 * that can be resolved into a unique per-device key. For the driver this key
 * would simply be a pointer directly to the driver-internal device object, but
 * for our layer we use a device dispatch key as an index in to the map to find
 * the layer's driver object.
 *
 * Key properties
 * ==============
 *
 * Vulkan devices are designed to be used concurrently by multiple application
 * threads. An application can have multiple concurrent devices, and use each
 * device from multiple threads.
 *
 * Access to the layer driver structures must therefore be kept thread-safe.
 * For sake of simplicity, we generally implement this by:
 *   - Holding a global lock whenever any thread is inside layer code.
 *   - Releasing the global lock whenever the layer calls a driver function.
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <utility>
#include <atomic>
#include <mutex>
#include <vulkan/utility/vk_safe_struct.hpp>
#include <vulkan/vk_layer.h>

#include "layer_comms.hpp"
#include "comms/comms_module.hpp"
#include "framework/device_dispatch_table.hpp"
#include "instance.hpp"
#include "trackers/device.hpp"

extern std::mutex g_vulkanLock;

/**
 * @brief Function pointer type for patching VkDeviceCreateInfo.
 */
using DeviceCreatePatchPtr = void (*)(Instance& instance,
                                      VkPhysicalDevice physicalDevice,
                                      vku::safe_VkDeviceCreateInfo& createInfo,
                                      std::vector<std::string>& supported);

/**
 * @brief This class implements the layer state tracker for a single device.
 */
class Device
{
public:
    /**
     * @brief The number of query pool rings used to buffer frames and prevent CPU-GPU pipeline stalls.
     */
    static constexpr size_t POOL_COUNT = 3;

    /**
     * @brief The maximum query capacity allocated for each timestamp query pool.
     */
    static constexpr uint32_t QUERY_COUNT = 16384;

public:
    /**
     * @brief Store a new device into the global store of dispatchable devices.
     *
     * @param handle   The dispatchable device handle to use as an indirect key.
     * @param device   The @c Device object to store.
     */
    static void store(
        VkDevice handle,
        std::unique_ptr<Device> device);

    /**
     * @brief Fetch a device from the global store of dispatchable devices.
     *
     * @param handle   The dispatchable device handle to use as an indirect lookup.
     *
     * @return The layer device context.
     */
    static Device* retrieve(
        VkDevice handle);

    /**
     * @brief Fetch a device from the global store of dispatchable devices.
     *
     * @param handle   The dispatchable queue handle to use as an indirect lookup.
     *
     * @return The layer device context.
     */
    static Device* retrieve(
        VkQueue handle);

    /**
     * @brief Fetch a device from the global store of dispatchable devices.
     *
     * @param handle   The dispatchable command buffer handle to use as an indirect lookup.
     *
     * @return The layer device context.
     */
    static Device* retrieve(
        VkCommandBuffer handle);

    /**
     * @brief Drop a device from the global store of dispatchable devices.
     *
     * This must be called before the driver VkDevice has been destroyed, as
     * we deference the native device handle to get the dispatch key.
     *
     * @param handle   The dispatchable device handle to use as an indirect lookup.
     *
     * @return Returns the ownership of the Device object to the caller.
     */
    static std::unique_ptr<Device> destroy(
        VkDevice handle);

    /**
     * @brief Create a new layer device object.
     *
     * Create info is transient, so the constructor must copy what it needs.
     *
     * @param instance               The layer instance object this device is created with.
     * @param physicalDevice         The physical device this logical device is for.
     * @param device                 The device handle this device is created with.
     * @param nlayerGetProcAddress   The vkGetProcAddress function in the driver/next layer down.
     * @param createInfo             The create info used to create the device.
     */
    Device(
        Instance* instance,
        VkPhysicalDevice physicalDevice,
        VkDevice device,
        PFN_vkGetDeviceProcAddr nlayerGetProcAddress,
        const VkDeviceCreateInfo& createInfo);

    /**
     * @brief Destroy this layer device object.
     */
    ~Device();

public:
    /**
     * @brief The driver function dispatch table.
     */
    DeviceDispatchTable driver {};

    /**
     * @brief The minimum set of device extensions needed by this layer.
     */
    static const std::vector<DeviceCreatePatchPtr> createInfoPatches;

    /**
     * @brief Callback for sending some message for the device.
     *
     * @param message   The message to send.
     */
    void txMessage(const std::string& message)
    {
        commsWrapper->txMessage(message);
    }

    /**
     * @brief Check if the communications module is currently connected.
     */
    bool isCommsConnected() const
    {
        return commsModule && commsModule->isConnected();
    }

    /**
     * @brief Get the cumulative stats for this device.
     */
    Tracker::Device& getStateTracker() { return stateTracker; }

    /**
     * @brief Execute a Vulkan workload safely with timing instrumentation.
     *
     * @param commandBuffer The command buffer recording the workload.
     * @param driverCall   The callable containing the driver call to profile.
     */
    template <typename Func>
    void executeWorkload(VkCommandBuffer commandBuffer, Func&& driverCall)
    {
        std::unique_lock<std::mutex> lock {g_vulkanLock};
        auto query = this->beginWorkload(commandBuffer);

        lock.unlock();
        driverCall();

        if (this->isFrameOfInterest)
        {
            lock.lock();
            this->endWorkload(commandBuffer, query);
        }
    }

    /**
     * @brief Write a timestamp query command to a command buffer.
     *
     * @param cb           The command buffer to write the command to.
     * @param flag         The pipeline stage flag representing when the timestamp should be captured.
     * @param pool         The Vulkan query pool to write into.
     * @param queryIndex   The query index within the pool.
     */
    void writeTimestamp(VkCommandBuffer cb, VkPipelineStageFlags2 flag, VkQueryPool pool, uint32_t queryIndex);

    /**
     * @brief Insert an execution and memory barrier command to prevent workload overlap.
     *
     * @param commandBuffer The command buffer to insert the barrier into.
     */
    void insertExecutionBarrier(VkCommandBuffer commandBuffer);

    /**
     * @brief Structure tracking queries allocated for an active single-shot workload.
     */
    struct WorkloadQuery {
        VkQueryPool pool{VK_NULL_HANDLE};
        uint32_t startIdx{0};
        uint32_t endIdx{0};
    };

    /**
     * @brief Allocate queries and write the starting timestamp for a standard workload.
     *
     * @param commandBuffer The command buffer recording the workload.
     *
     * @return A structure containing the allocated query pool and indices.
     */
    WorkloadQuery beginWorkload(VkCommandBuffer commandBuffer);

    /**
     * @brief Write the end timestamp for a standard workload and record its query indices.
     *
     * @param commandBuffer The command buffer recording the workload.
     * @param query        The workload query tracking details allocated during beginWorkload.
     */
    void endWorkload(VkCommandBuffer commandBuffer, const WorkloadQuery& query);

    /**
     * @brief Structure tracking query indexes to finish an active render pass timing.
     */
    struct RenderPassEndQuery {
        VkQueryPool pool{VK_NULL_HANDLE};
        uint32_t startIdx{0};
        uint32_t endIdx{0};
        bool hasActiveQuery{false};
    };

    /**
     * @brief Begin timing instrumentation for a render pass or dynamic rendering workload.
     *
     * @param commandBuffer The command buffer recording the render pass.
     */
    void beginRenderPassWorkload(VkCommandBuffer commandBuffer);

    /**
     * @brief Perform pre-end actions before ending a render pass to safely retrieve query details.
     *
     * @param commandBuffer The command buffer recording the render pass.
     *
     * @return A structure containing the query pool and indices to be used after the render pass has ended.
     */
    RenderPassEndQuery preEndRenderPassWorkload(VkCommandBuffer commandBuffer);

    /**
     * @brief Complete timing instrumentation after a render pass has officially ended.
     *
     * @param commandBuffer The command buffer recording the render pass.
     * @param query        The render pass end query tracking details retrieved in preEndRenderPassWorkload.
     */
    void postEndRenderPassWorkload(VkCommandBuffer commandBuffer, const RenderPassEndQuery& query);

    /**
     * @brief Clear all cached queries and command buffer associations for the current frame.
     */
    void clearFrameQueries();

    /**
     * @brief Structure representing a resolved workload query with contextual details.
     */
    struct PendingQuery {
        std::string workloadType;
        std::vector<std::string> debugStack;
        VkQueryPool pool{VK_NULL_HANDLE};
        uint32_t startIdx{0};
        uint32_t endIdx{0};
        bool hasQuery{false};
    };

    /**
     * @brief Structure representing all tracked queries resolved for a specific frame.
     */
    struct PendingFrame {
        uint64_t frameID{0};
        uint32_t poolIdx{0};
        std::vector<PendingQuery> queries;
    };

    /**
     * @brief Mutex protecting access to the pending and current frame query queues.
     */
    std::mutex pendingMutex;

    /**
     * @brief Array containing resolved queries recorded during the active frame.
     */
    std::vector<PendingQuery> currentFrameQueries;

    /**
     * @brief Array containing queued frames waiting for their query results to be ready on the GPU.
     */
    std::vector<PendingFrame> pendingFrames;

    /**
     * @brief Process queued frames, pull GPU results asynchronously, and transmit query messages.
     *
     * @param flushAll     True to block-wait and resolve all frames, false otherwise.
     */
    void processDeferredFrames(bool flushAll);

    /**
     * @brief The duration in nanoseconds for a single query increment on the physical device.
     */
    float timestampPeriod{1.0f};

    /**
     * @brief Register a completed and submitted workload's timing data to the pending frame list.
     *
     * @param commandBuffer The command buffer associated with the submitted workload.
     * @param queryIdx     The sequential index of the query in the command buffer's query list.
     * @param workloadType The descriptive name representing the type of the workload.
     * @param debugStack   The current debug label stack for contextual callstack info.
     */
    void registerSubmittedWorkload(
        VkCommandBuffer commandBuffer,
        uint32_t queryIdx,
        const std::string& workloadType,
        const std::vector<std::string>& debugStack
    );

private:
    /**
     * @brief Array containing the Vulkan query pool handles.
     */
    std::array<VkQueryPool, POOL_COUNT> queryPools;

    /**
     * @brief Atomic query allocators for each pool tracking the next available slot.
     */
    std::atomic<uint32_t> queryPoolIndices[POOL_COUNT]{};

    /**
     * @brief Map associating command buffers with their active query pool ring allocations.
     */
    std::unordered_map<VkCommandBuffer, VkQueryPool> cbPools;

    /**
     * @brief Retrieve or allocate a Vulkan query pool for the specified command buffer.
     *
     * @param commandBuffer The command buffer requesting the pool.
     *
     * @return The query pool handle.
     */
    VkQueryPool getQueryPoolForCommandBuffer(VkCommandBuffer commandBuffer) {
        std::lock_guard<std::mutex> lock(cbQueriesMutex);
        auto it = cbPools.find(commandBuffer);
        if (it != cbPools.end()) {
            return it->second;
        }
        VkQueryPool pool = queryPools[currentQueryPoolIndex];
        cbPools[commandBuffer] = pool;
        return pool;
    }

    /**
     * @brief Resolve the array ring index for a given Vulkan query pool handle.
     *
     * @param pool         The query pool handle.
     *
     * @return The index representing the pool inside the queryPools array.
     */
    uint32_t getPoolIndex(VkQueryPool pool) const {
        for (uint32_t i = 0; i < POOL_COUNT; ++i) {
            if (queryPools[i] == pool) {
                return i;
            }
        }
        return 0;
    }

    /**
     * @brief Mutex protecting the command buffer query and pool maps.
     */
    std::mutex cbQueriesMutex;

    /**
     * @brief Map storing lists of active start-end query pairs per command buffer.
     */
    std::unordered_map<VkCommandBuffer, std::vector<std::pair<uint32_t, uint32_t>>> cbQueries;

    /**
     * @brief Map storing open start timestamps for unfinished render pass operations.
     */
    std::unordered_map<VkCommandBuffer, uint32_t> activeStartQuery;

    /**
     * @brief Function pointer cached during construction to write the timestamp via the optimal hardware route.
     */
    void (Device::*writeTimestampFn)(VkCommandBuffer, VkPipelineStageFlags2, VkQueryPool, uint32_t) {nullptr};

    /**
     * @brief Function pointer cached during construction to insert barriers via the optimal hardware route.
     */
    void (Device::*insertExecutionBarrierFn)(VkCommandBuffer) {nullptr};

    /**
     * @brief Optimal write timestamp implementation using Vulkan 1.3 synchronization2.
     */
    void writeTimestampV2(VkCommandBuffer, VkPipelineStageFlags2, VkQueryPool, uint32_t);

    /**
     * @brief Fallback write timestamp implementation using VK_KHR_synchronization2.
     */
    void writeTimestampV2KHR(VkCommandBuffer, VkPipelineStageFlags2, VkQueryPool, uint32_t);

    /**
     * @brief Fallback write timestamp implementation using standard Vulkan 1.0.
     */
    void writeTimestampLegacy(VkCommandBuffer, VkPipelineStageFlags2, VkQueryPool, uint32_t);

    /**
     * @brief Optimal barrier implementation using Vulkan 1.3 synchronization2.
     */
    void insertExecutionBarrierV2(VkCommandBuffer);

    /**
     * @brief Fallback barrier implementation using VK_KHR_synchronization2.
     */
    void insertExecutionBarrierV2KHR(VkCommandBuffer);

    /**
     * @brief Fallback barrier implementation using standard Vulkan 1.0.
     */
    void insertExecutionBarrierLegacy(VkCommandBuffer);

    /**
     * @brief Helper to allocate indices and issue the starting execution barrier and timestamp query.
     */
    bool allocateAndWriteStartTimestamp(VkCommandBuffer commandBuffer, VkQueryPool& outPool, uint32_t& outStartIdx, uint32_t& outEndIdx);

    /**
     * @brief Helper to issue the ending execution barrier, timestamp query, and register the query pair.
     */
    void writeEndTimestampAndRecord(VkCommandBuffer commandBuffer, VkQueryPool pool, uint32_t startIdx, uint32_t endIdx);

public:
    /**
     * @brief The instance this device is created with.
     */
    const Instance* instance;

    /**
     * @brief The physical device this device is created with.
     */
    const VkPhysicalDevice physicalDevice;

    /**
     * @brief The device handle this device is created with.
     */
    const VkDevice device;

public:
    /**
     * @brief Is this frame being profiled?
     */
    bool isFrameOfInterest {false};

    /**
     * @brief Atomic integer tracking the monotonically increasing frame ID.
     */
    std::atomic<uint64_t> currentFrameID{0};

    /**
     * @brief Index of the active query pool ring slot being recorded for the current frame of interest.
     */
    uint32_t currentQueryPoolIndex{0};

private:
    /**
     * @brief State tracker for this device.
     */
    Tracker::Device stateTracker;

    /**
    * @brief Shared network communications module.
    */
    static std::unique_ptr<Comms::CommsModule> commsModule;

    /**
     * @brief Shared network communications message encoder.
     */
    static std::unique_ptr<ProfileComms> commsWrapper;
};
