/*
 * SPDX-License-Identifier: MIT
 * -----------------------------------------------------------------------------
 * Copyright (c) 2026 Arm Limited
 */

#pragma once

#include <cstdint>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>

struct DeviceDispatchTable;
struct InstanceDispatchTable;
class ShaderDumpComms;

/**
 * @brief Persistent, content-addressed dump of SPIR-V and pipeline statistics.
 *
 * The class owns no Vulkan objects. All handles passed to it remain owned by
 * the application and are only used while they are valid.
 */
class ShaderDump
{
public:
    ShaderDump(const std::string& outputPath, bool clearOutput, bool frameSummary, uint32_t frameStart, uint32_t frameCount,
               bool detailed, ShaderDumpComms* comms);
    ~ShaderDump();

    bool enabled() const;

    void recordDeviceInfo(VkPhysicalDevice physicalDevice, const InstanceDispatchTable& driver,
                          uint32_t applicationApiVersion, bool executableInfoEnabled);

    void recordShaderModule(VkShaderModule shaderModule,
                            const uint32_t* code,
                            size_t codeWords,
                            const uint32_t* originalCode,
                            size_t originalCodeWords);

    void recordComputePipeline(VkDevice device,
                               const DeviceDispatchTable& driver,
                               VkPipeline pipeline,
                               const VkComputePipelineCreateInfo& createInfo,
                               bool captureStatistics);

    void recordGraphicsPipeline(VkDevice device,
                                const DeviceDispatchTable& driver,
                                VkPipeline pipeline,
                                const VkGraphicsPipelineCreateInfo& createInfo,
                                bool captureStatistics);

    void recordRayTracingPipeline(VkDevice device,
                                  const DeviceDispatchTable& driver,
                                  VkPipeline pipeline,
                                  const VkRayTracingPipelineCreateInfoKHR& createInfo,
                                  bool captureStatistics);

    void beginCommandBuffer(VkCommandBuffer commandBuffer);
    void bindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint, VkPipeline pipeline);
    void beginLabel(VkCommandBuffer commandBuffer, const VkDebugUtilsLabelEXT& label);
    void endLabel(VkCommandBuffer commandBuffer);
    void recordDraw(VkCommandBuffer commandBuffer, bool indirect = false);
    void recordDispatch(VkCommandBuffer commandBuffer);
    void recordTraceRays(VkCommandBuffer commandBuffer);
    void recordObjectName(const VkDebugUtilsObjectNameInfoEXT& nameInfo);
    void endFrame();

private:
    std::string lookupShader(VkShaderModule shaderModule) const;
    void recordPipeline(VkDevice device,
                        const DeviceDispatchTable& driver,
                        VkPipeline pipeline,
                        const char* kind,
                        const VkPipelineShaderStageCreateInfo* stages,
                        uint32_t stageCount,
                        bool captureStatistics);
    void publishProgress(bool force = false);

    std::string outputPath;
    bool active {false};
    mutable std::mutex lock;
    std::unordered_map<uint64_t, std::string> shaderModules;
    std::unordered_map<uintptr_t, std::string> pipelines;

public:
    struct CommandBufferState
    {
        std::unordered_map<VkPipelineBindPoint, uintptr_t> boundPipelines;
        std::vector<std::string> labels;
    };
    struct Usage
    {
        uint64_t draws {0};
        uint64_t dispatches {0};
        uint64_t traceRays {0};
        uint64_t indirectDrawCommands {0};
    };

private:
    std::unordered_map<uintptr_t, CommandBufferState> commandBuffers;
    std::unordered_map<std::string, Usage> frameUsage;
    uint64_t frameNumber {0};
    bool writeFrameSummary {true};
    bool detailed {false};
    uint32_t firstCapturedFrame {0};
    uint32_t capturedFrameCount {0};
    uint64_t nextPipelineId {1};
    ShaderDumpComms* comms {nullptr};
    uint64_t shaderCount {0};
    uint64_t pipelineCount {0};
    uint64_t writtenFrameCount {0};
    std::chrono::steady_clock::time_point lastProgress {};
};
