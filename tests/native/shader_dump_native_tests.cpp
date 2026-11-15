/* SPDX-License-Identifier: MIT */
#include "deferred_pipeline.hpp"
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

static void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void testDeferredOwnershipAndCompletion()
{
    char entry[] = "original";
    uint32_t constant = 17;
    VkSpecializationMapEntry mapping {0, 0, sizeof(constant)};
    VkSpecializationInfo specialization {1, &mapping, sizeof(constant), &constant};
    VkPipelineShaderStageCreateInfo stage {};
    stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage = VK_SHADER_STAGE_RAYGEN_BIT_KHR;
    stage.pName = entry;
    stage.pSpecializationInfo = &specialization;
    VkRayTracingPipelineCreateInfoKHR create {};
    create.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR;
    create.stageCount = 1;
    create.pStages = &stage;
    VkPipeline outputs[] = {reinterpret_cast<VkPipeline>(uintptr_t(1))};
    DeferredRayTracingPipelines pending(1, &create, outputs);
    entry[0] = 'X'; constant = 99; stage.pName = nullptr;
    require(std::strcmp(pending.infos[0].pStages[0].pName, "original") == 0, "entry point was not owned");
    require(*static_cast<const uint32_t*>(pending.infos[0].pStages[0].pSpecializationInfo->pData) == 17,
            "specialization data was not owned");
    int records = 0;
    auto record = [&](const auto& infos, VkPipeline* pipelines) {
        require(infos.size() == 1 && pipelines[0] == outputs[0], "incorrect deferred outputs");
        ++records;
    };
    pending.complete([] { return VK_NOT_READY; }, record);
    require(records == 0, "pending operation was recorded");
    std::vector<std::thread> observers;
    for (int i = 0; i < 8; ++i)
        observers.emplace_back([&] { pending.complete([] { return VK_SUCCESS; }, record); });
    for (auto& thread : observers) thread.join();
    require(records == 1, "completion was not recorded exactly once");
    DeferredRayTracingPipelines failed(1, &create, outputs);
    failed.complete([] { return VK_ERROR_OUT_OF_DEVICE_MEMORY; }, record);
    require(records == 1, "failed operation consumed outputs");
}

int main()
{
    try {
        testDeferredOwnershipAndCompletion();
        std::cout << "PASS deferred ownership, pending/error paths, concurrent exactly-once completion\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
