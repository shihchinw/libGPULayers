/* SPDX-License-Identifier: MIT */
#pragma once

#include <mutex>
#include <vector>
#include <vulkan/utility/vk_safe_struct.hpp>

/** Own the driver-visible inputs until a deferred operation is destroyed.
 * Completion observation and output consumption are serialized: no observer may
 * return completion to the application while another is still reading outputs.
 */
struct DeferredRayTracingPipelines
{
    DeferredRayTracingPipelines(uint32_t count, const VkRayTracingPipelineCreateInfoKHR* source, VkPipeline* outputs)
        : pipelines(outputs)
    {
        owned.reserve(count);
        infos.reserve(count);
        for (uint32_t i = 0; i < count; ++i) owned.emplace_back(&source[i]);
        for (auto& info : owned) infos.push_back(*info.ptr());
    }

    template<typename Query, typename Record>
    void complete(Query query, Record record)
    {
        std::lock_guard<std::mutex> guard(mutex);
        if (recorded) return;
        const auto result = query();
        if (result == VK_NOT_READY) return;
        if (result == VK_SUCCESS) record(infos, pipelines);
        recorded = true;
    }

    std::vector<vku::safe_VkRayTracingPipelineCreateInfoKHR> owned;
    std::vector<VkRayTracingPipelineCreateInfoKHR> infos;
    VkPipeline* pipelines;
    std::mutex mutex;
    bool recorded {false};
};
