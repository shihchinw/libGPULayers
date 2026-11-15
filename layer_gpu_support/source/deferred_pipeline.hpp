/* SPDX-License-Identifier: MIT */
#pragma once

#include <mutex>
#include <vector>
#include <vulkan/utility/vk_safe_struct.hpp>
#include "pipeline_capture.hpp"

/** Own the driver-visible inputs until a deferred operation is destroyed.
 * Completion observation and output consumption are serialized: no observer may
 * return completion to the application while another is still reading outputs.
 */
struct DeferredRayTracingPipelines : PipelineCaptureInfos<VkRayTracingPipelineCreateInfoKHR, vku::safe_VkRayTracingPipelineCreateInfoKHR>
{
    DeferredRayTracingPipelines(uint32_t count, const VkRayTracingPipelineCreateInfoKHR* source, VkPipeline* outputs,
                               bool statistics = false)
        : PipelineCaptureInfos(count, source, statistics, true), pipelines(outputs), statisticsRequested(statistics) {}

    template<typename Query, typename Record>
    void complete(Query query, Record record)
    {
        std::lock_guard<std::mutex> guard(mutex);
        if (recorded) return;
        const auto result = query();
        if (result == VK_NOT_READY) return;
        copyFeedback();
        if (result == VK_SUCCESS) record(infos, pipelines);
        recorded = true;
    }

    VkPipeline* pipelines;
    bool statisticsRequested;
    std::mutex mutex;
    bool recorded {false};
};
