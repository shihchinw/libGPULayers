/* SPDX-License-Identifier: MIT */
#pragma once

#include <vector>
#include <memory>
#include <type_traits>
#include <stdexcept>
#include <algorithm>
#include <vulkan/utility/vk_safe_struct.hpp>
#include <vulkan/utility/vk_struct_helper.hpp>

template<typename Info>
bool hasCaptureStatistics(const Info& info)
{
    const auto* flags2 = vku::FindStructInPNextChain<VkPipelineCreateFlags2CreateInfoKHR>(
        const_cast<void*>(info.pNext));
    const auto flags = flags2 ? flags2->flags : static_cast<VkPipelineCreateFlags2KHR>(info.flags);
    return (flags & VK_PIPELINE_CREATE_2_CAPTURE_STATISTICS_BIT_KHR) != 0;
}

/** Patch only owned pNext chains; flags2 takes precedence over legacy flags. */
template<typename Info, typename SafeInfo>
struct PipelineCaptureInfos
{
    PipelineCaptureInfos(uint32_t count, const Info* source, bool statistics, bool alwaysOwn = false)
        : originals(source)
    {
        infos.reserve(count);
        if (alwaysOwn)
        {
            if constexpr (std::is_constructible_v<SafeInfo, const Info*>)
            {
                owned.reserve(count);
                for (uint32_t i = 0; i < count; ++i) owned.emplace_back(&source[i]);
                for (auto& info : owned) infos.push_back(*info.ptr());
            }
            else throw std::logic_error("This pipeline type does not support deferred ownership");
        }
        else
        {
            // Immediate calls only need owned pNext chains. In particular, do
            // not deep-copy graphics state pointers that Vulkan may ignore.
            infos.assign(source, source + count);
            if (statistics)
                for (auto& info : infos)
                {
                    if (!vku::FindStructInPNextChain<VkPipelineCreateFlags2CreateInfoKHR>(const_cast<void*>(info.pNext)))
                        continue;
                    auto chain = std::shared_ptr<void>(vku::SafePnextCopy(info.pNext), vku::FreePnextChain);
                    info.pNext = chain.get();
                    chains.push_back(std::move(chain));
                }
        }
        if (!statistics) return;
        for (auto& info : infos)
        {
            auto* flags2 = vku::FindStructInPNextChain<VkPipelineCreateFlags2CreateInfoKHR>(
                const_cast<void*>(info.pNext));
            if (flags2) flags2->flags |= VK_PIPELINE_CREATE_2_CAPTURE_STATISTICS_BIT_KHR;
            else info.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR;
        }
    }
    // Safe pNext copies own output storage too. Return feedback to the app
    // before exposing completion, including failed pipeline creations.
    void copyFeedback() const
    {
        for (size_t i = 0; i < infos.size(); ++i)
        {
            if (originals[i].pNext == infos[i].pNext) continue;
            const auto* from = vku::FindStructInPNextChain<VkPipelineCreationFeedbackCreateInfo>(const_cast<void*>(infos[i].pNext));
            const auto* to = vku::FindStructInPNextChain<VkPipelineCreationFeedbackCreateInfo>(const_cast<void*>(originals[i].pNext));
            if (!from || !to) continue;
            if (from->pPipelineCreationFeedback && to->pPipelineCreationFeedback)
                *to->pPipelineCreationFeedback = *from->pPipelineCreationFeedback;
            if (from->pPipelineStageCreationFeedbacks && to->pPipelineStageCreationFeedbacks)
                std::copy_n(from->pPipelineStageCreationFeedbacks,
                    std::min(from->pipelineStageCreationFeedbackCount, to->pipelineStageCreationFeedbackCount),
                    to->pPipelineStageCreationFeedbacks);
        }
    }
    const Info* originals;
    std::vector<SafeInfo> owned;
    std::vector<Info> infos;
    std::vector<std::shared_ptr<void>> chains;
};
