/* SPDX-License-Identifier: MIT */
#include "deferred_pipeline.hpp"
#include "shader_dump.hpp"
#include "device.hpp"
#include "layer_device_functions.hpp"
#include "layer_comms.hpp"
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <filesystem>
#include <fstream>
#include <sstream>

// Driver-double context for invoking the actual graphics API interceptors.
std::mutex g_vulkanLock;
static Device* testDevice = nullptr;
static int forwardCalls = 0;
Device::Device(Instance* parent, VkPhysicalDevice physical, VkDevice logical,
               PFN_vkGetDeviceProcAddr, const VkDeviceCreateInfo&)
    : instance(parent), physicalDevice(physical), device(logical) {}
Device::~Device() = default;
Device* Device::retrieve(VkCommandBuffer) { return testDevice; }
Device* Device::retrieve(VkDevice) { return testDevice; }
Device* Device::retrieve(VkQueue) { return testDevice; }
bool LayerConfig::shader_dump_capture_debug_utils() const { return true; }
void ShaderDumpComms::txCaptureStarted() {}
void ShaderDumpComms::txProgress(uint64_t, uint64_t, uint64_t) {}
void ShaderDumpComms::txCaptureFinished(uint64_t, uint64_t, uint64_t) {}

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


static void testDrawHooks()
{
    const auto root=std::filesystem::temp_directory_path() / ("lgl-draw-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    VkDeviceCreateInfo deviceInfo {};
    Device context(nullptr, VK_NULL_HANDLE, VK_NULL_HANDLE, nullptr, deviceInfo);
    testDevice=&context;
    context.shaderDump=std::make_unique<ShaderDump>(root.string(),false,true,0,0,false,nullptr);
    VkPipelineShaderStageCreateInfo stage {};
    stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage.stage=VK_SHADER_STAGE_FRAGMENT_BIT;stage.pName="main";
    VkGraphicsPipelineCreateInfo create {};
    create.sType=VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    create.stageCount=1;create.pStages=&stage;
    auto pipeline=reinterpret_cast<VkPipeline>(uintptr_t(7));
    context.shaderDump->recordGraphicsPipeline(VK_NULL_HANDLE,context.driver,pipeline,create,false);
    auto commandBuffer=reinterpret_cast<VkCommandBuffer>(uintptr_t(9));
    context.shaderDump->beginCommandBuffer(commandBuffer);
    context.shaderDump->bindPipeline(commandBuffer,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    context.driver.vkCmdDraw=+[](VkCommandBuffer commandBuffer,
    uint32_t vertexCount,
    uint32_t instanceCount,
    uint32_t firstVertex,
    uint32_t firstInstance) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDraw<user_tag>(commandBuffer,17,17,17,17);
    context.driver.vkCmdDrawIndexed=+[](VkCommandBuffer commandBuffer,
    uint32_t indexCount,
    uint32_t instanceCount,
    uint32_t firstIndex,
    int32_t vertexOffset,
    uint32_t firstInstance) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDrawIndexed<user_tag>(commandBuffer,17,17,17,17,17);
    context.driver.vkCmdDrawIndexedIndirect=+[](VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    uint32_t drawCount,
    uint32_t stride) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDrawIndexedIndirect<user_tag>(commandBuffer,VK_NULL_HANDLE,17,17,17);
    context.driver.vkCmdDrawIndexedIndirect2KHR=+[](VkCommandBuffer commandBuffer,
    const VkDrawIndirect2InfoKHR* pInfo) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDrawIndexedIndirect2KHR<user_tag>(commandBuffer,nullptr);
    context.driver.vkCmdDrawIndexedIndirectCount=+[](VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDrawIndexedIndirectCount<user_tag>(commandBuffer,VK_NULL_HANDLE,17,VK_NULL_HANDLE,17,17,17);
    context.driver.vkCmdDrawIndexedIndirectCount2KHR=+[](VkCommandBuffer commandBuffer,
    const VkDrawIndirectCount2InfoKHR* pInfo) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDrawIndexedIndirectCount2KHR<user_tag>(commandBuffer,nullptr);
    context.driver.vkCmdDrawIndexedIndirectCountKHR=+[](VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDrawIndexedIndirectCountKHR<user_tag>(commandBuffer,VK_NULL_HANDLE,17,VK_NULL_HANDLE,17,17,17);
    context.driver.vkCmdDrawIndirect=+[](VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    uint32_t drawCount,
    uint32_t stride) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDrawIndirect<user_tag>(commandBuffer,VK_NULL_HANDLE,17,17,17);
    context.driver.vkCmdDrawIndirect2KHR=+[](VkCommandBuffer commandBuffer,
    const VkDrawIndirect2InfoKHR* pInfo) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDrawIndirect2KHR<user_tag>(commandBuffer,nullptr);
    context.driver.vkCmdDrawIndirectByteCount2EXT=+[](VkCommandBuffer commandBuffer,
    uint32_t instanceCount,
    uint32_t firstInstance,
    const VkBindTransformFeedbackBuffer2InfoEXT* pCounterInfo,
    uint32_t counterOffset,
    uint32_t vertexStride) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDrawIndirectByteCount2EXT<user_tag>(commandBuffer,17,17,nullptr,17,17);
    context.driver.vkCmdDrawIndirectByteCountEXT=+[](VkCommandBuffer commandBuffer,
    uint32_t instanceCount,
    uint32_t firstInstance,
    VkBuffer counterBuffer,
    VkDeviceSize counterBufferOffset,
    uint32_t counterOffset,
    uint32_t vertexStride) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDrawIndirectByteCountEXT<user_tag>(commandBuffer,17,17,VK_NULL_HANDLE,17,17,17);
    context.driver.vkCmdDrawIndirectCount=+[](VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDrawIndirectCount<user_tag>(commandBuffer,VK_NULL_HANDLE,17,VK_NULL_HANDLE,17,17,17);
    context.driver.vkCmdDrawIndirectCount2KHR=+[](VkCommandBuffer commandBuffer,
    const VkDrawIndirectCount2InfoKHR* pInfo) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDrawIndirectCount2KHR<user_tag>(commandBuffer,nullptr);
    context.driver.vkCmdDrawIndirectCountKHR=+[](VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDrawIndirectCountKHR<user_tag>(commandBuffer,VK_NULL_HANDLE,17,VK_NULL_HANDLE,17,17,17);
    context.driver.vkCmdDrawMeshTasksEXT=+[](VkCommandBuffer commandBuffer,
    uint32_t groupCountX,
    uint32_t groupCountY,
    uint32_t groupCountZ) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDrawMeshTasksEXT<user_tag>(commandBuffer,17,17,17);
    context.driver.vkCmdDrawMeshTasksIndirect2EXT=+[](VkCommandBuffer commandBuffer,
    const VkDrawIndirect2InfoKHR* pInfo) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDrawMeshTasksIndirect2EXT<user_tag>(commandBuffer,nullptr);
    context.driver.vkCmdDrawMeshTasksIndirectCount2EXT=+[](VkCommandBuffer commandBuffer,
    const VkDrawIndirectCount2InfoKHR* pInfo) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDrawMeshTasksIndirectCount2EXT<user_tag>(commandBuffer,nullptr);
    context.driver.vkCmdDrawMeshTasksIndirectCountEXT=+[](VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    VkBuffer countBuffer,
    VkDeviceSize countBufferOffset,
    uint32_t maxDrawCount,
    uint32_t stride) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDrawMeshTasksIndirectCountEXT<user_tag>(commandBuffer,VK_NULL_HANDLE,17,VK_NULL_HANDLE,17,17,17);
    context.driver.vkCmdDrawMeshTasksIndirectEXT=+[](VkCommandBuffer commandBuffer,
    VkBuffer buffer,
    VkDeviceSize offset,
    uint32_t drawCount,
    uint32_t stride) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDrawMeshTasksIndirectEXT<user_tag>(commandBuffer,VK_NULL_HANDLE,17,17,17);
    context.driver.vkCmdDrawMultiEXT=+[](VkCommandBuffer commandBuffer,
    uint32_t drawCount,
    const VkMultiDrawInfoEXT* pVertexInfo,
    uint32_t instanceCount,
    uint32_t firstInstance,
    uint32_t stride) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDrawMultiEXT<user_tag>(commandBuffer,17,nullptr,17,17,17);
    context.driver.vkCmdDrawMultiIndexedEXT=+[](VkCommandBuffer commandBuffer,
    uint32_t drawCount,
    const VkMultiDrawIndexedInfoEXT* pIndexInfo,
    uint32_t instanceCount,
    uint32_t firstInstance,
    uint32_t stride,
    const int32_t* pVertexOffset) { ++forwardCalls; require(commandBuffer==reinterpret_cast<VkCommandBuffer>(uintptr_t(9)),"command buffer not forwarded"); };
    layer_vkCmdDrawMultiIndexedEXT<user_tag>(commandBuffer,17,nullptr,17,17,17,nullptr);
    require(forwardCalls==21,"driver forwarding count incorrect");
    context.shaderDump->endFrame();
    std::ifstream stream(root/"tables"/"frame_pipeline_usage.csv");
    std::string header,line;std::getline(stream,header);std::getline(stream,line);
    require(header.find("indirect_draw_command_count")!=std::string::npos,"missing indirect count column");
    require(line.ends_with(",21,0,0,16"),"multi/indirect calls were counted as GPU draws");
    stream.close();context.shaderDump.reset();testDevice=nullptr;
    std::filesystem::remove_all(root); // host-created test directory only
    std::cout<<"PASS 21 graphics draw hooks, forwarding, CPU counts and 16 indirect commands\n";
}

int main()
{
    try {
        testDeferredOwnershipAndCompletion();
        testDrawHooks();
        std::cout << "PASS deferred ownership, pending/error paths, concurrent exactly-once completion\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
