/*
 * SPDX-License-Identifier: MIT
 * -----------------------------------------------------------------------------
 * Copyright (c) 2026 Arm Limited
 */

#include "shader_dump.hpp"

#include "framework/device_dispatch_table.hpp"
#include "framework/instance_dispatch_table.hpp"
#include "framework/utils.hpp"
#include "layer_comms.hpp"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>
#include <vulkan/utility/vk_struct_helper.hpp>

namespace
{
using json = nlohmann::json;

uint32_t rol(uint32_t value, uint32_t shift)
{
    return (value << shift) | (value >> (32 - shift));
}

/** Small self-contained SHA-1 implementation for content-addressed file names. */
std::string sha1(const void* data, size_t size)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    uint64_t bitCount = static_cast<uint64_t>(size) * 8;
    std::vector<uint8_t> input(bytes, bytes + size);
    input.push_back(0x80);
    while ((input.size() % 64) != 56)
    {
        input.push_back(0);
    }
    for (int i = 7; i >= 0; --i)
    {
        input.push_back(static_cast<uint8_t>(bitCount >> (i * 8)));
    }

    uint32_t h0 = 0x67452301;
    uint32_t h1 = 0xEFCDAB89;
    uint32_t h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476;
    uint32_t h4 = 0xC3D2E1F0;
    for (size_t offset = 0; offset < input.size(); offset += 64)
    {
        std::array<uint32_t, 80> w {};
        for (uint32_t i = 0; i < 16; ++i)
        {
            const size_t p = offset + 4 * i;
            w[i] = (static_cast<uint32_t>(input[p]) << 24) | (static_cast<uint32_t>(input[p + 1]) << 16) |
                   (static_cast<uint32_t>(input[p + 2]) << 8) | input[p + 3];
        }
        for (uint32_t i = 16; i < 80; ++i)
        {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;
        for (uint32_t i = 0; i < 80; ++i)
        {
            uint32_t f = 0;
            uint32_t k = 0;
            if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t temp = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (uint32_t word : {h0, h1, h2, h3, h4})
    {
        stream << std::setw(8) << word;
    }
    return stream.str();
}

std::string csv(const std::string& value)
{
    std::string escaped {"\""};
    for (char c : value)
    {
        if (c == '\"') escaped += "\"\"";
        else escaped += c;
    }
    return escaped + "\"";
}

std::string stageName(VkShaderStageFlagBits stage)
{
    switch (stage)
    {
    case VK_SHADER_STAGE_VERTEX_BIT: return "vertex";
    case VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT: return "tessellation_control";
    case VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT: return "tessellation_evaluation";
    case VK_SHADER_STAGE_GEOMETRY_BIT: return "geometry";
    case VK_SHADER_STAGE_FRAGMENT_BIT: return "fragment";
    case VK_SHADER_STAGE_COMPUTE_BIT: return "compute";
    case VK_SHADER_STAGE_RAYGEN_BIT_KHR: return "raygen";
    case VK_SHADER_STAGE_ANY_HIT_BIT_KHR: return "any_hit";
    case VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR: return "closest_hit";
    case VK_SHADER_STAGE_MISS_BIT_KHR: return "miss";
    case VK_SHADER_STAGE_INTERSECTION_BIT_KHR: return "intersection";
    case VK_SHADER_STAGE_CALLABLE_BIT_KHR: return "callable";
    case VK_SHADER_STAGE_TASK_BIT_EXT: return "task";
    case VK_SHADER_STAGE_MESH_BIT_EXT: return "mesh";
    default: return "unknown";
    }
}

std::string statisticValue(const VkPipelineExecutableStatisticKHR& statistic)
{
    switch (statistic.format)
    {
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR: return statistic.value.b32 ? "true" : "false";
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR: return std::to_string(statistic.value.i64);
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR: return std::to_string(statistic.value.u64);
    case VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR: return std::to_string(statistic.value.f64);
    default: return "unknown";
    }
}
}  // namespace

ShaderDump::ShaderDump(const std::string& outputPath_, bool clearOutput, bool frameSummary, uint32_t frameStart, uint32_t frameCount,
                       bool detailed_, ShaderDumpComms* comms_)
    : outputPath(outputPath_),
      writeFrameSummary(frameSummary),
      detailed(detailed_),
      firstCapturedFrame(frameStart),
      capturedFrameCount(frameCount),
      comms(comms_)
{
    if (outputPath.empty())
    {
        return;
    }
    std::error_code error;
    if (clearOutput)
    {
        std::filesystem::remove_all(outputPath, error);
        error.clear();
    }
    std::filesystem::create_directories(std::filesystem::path(outputPath) / "shaders", error);
    std::filesystem::create_directories(std::filesystem::path(outputPath) / "pipelines", error);
    std::filesystem::create_directories(std::filesystem::path(outputPath) / "tables", error);
    active = !error;
    if (!active)
    {
        LAYER_ERR("Failed to create shader dump directory: %s", outputPath.c_str());
    }
    else if (comms)
    {
        comms->txCaptureStarted();
    }
}

ShaderDump::~ShaderDump()
{
    if (active && comms)
    {
        comms->txCaptureFinished(shaderCount, pipelineCount, writtenFrameCount);
    }
}

bool ShaderDump::enabled() const { return active; }

void ShaderDump::recordDeviceInfo(VkPhysicalDevice physicalDevice, const InstanceDispatchTable& driver,
                                 uint32_t applicationApiVersion, bool executableInfoEnabled)
{
    if (!active) return;

    VkPhysicalDeviceProperties properties {};
    driver.vkGetPhysicalDeviceProperties(physicalDevice, &properties);
    bool driverPropertiesSupported = properties.apiVersion >= VK_API_VERSION_1_2;
    if (!driverPropertiesSupported)
    {
        uint32_t count = 0;
        if (driver.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr) == VK_SUCCESS)
        {
            std::vector<VkExtensionProperties> extensions(count);
            if (driver.vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, extensions.data()) == VK_SUCCESS)
            {
                for (const auto& extension : extensions)
                    driverPropertiesSupported |= std::strcmp(extension.extensionName, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME) == 0;
            }
        }
    }

    VkPhysicalDeviceDriverProperties driverProperties = vku::InitStructHelper();
    auto getProperties2 = driver.vkGetPhysicalDeviceProperties2;
    if (!getProperties2) getProperties2 = driver.vkGetPhysicalDeviceProperties2KHR;
    const bool queried = driverPropertiesSupported && getProperties2;
    if (queried)
    {
        VkPhysicalDeviceProperties2 query = vku::InitStructHelper(&driverProperties);
        getProperties2(physicalDevice, &query);
    }

    auto hex = [](uint32_t value) {
        std::ostringstream text;
        text << "0x" << std::hex << std::setfill('0') << std::setw(8) << value;
        return text.str();
    };
    std::ostringstream uuid;
    for (auto byte : properties.pipelineCacheUUID)
        uuid << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(byte);
    json info {{"schema_version", 1}, {"source", "capture Vulkan physical device"},
               {"application_api_version", std::to_string(VK_API_VERSION_MAJOR(applicationApiVersion)) + "." +
                                           std::to_string(VK_API_VERSION_MINOR(applicationApiVersion)) + "." +
                                           std::to_string(VK_API_VERSION_PATCH(applicationApiVersion))},
               {"pipeline_executable_info_enabled", executableInfoEnabled},
               {"device_name", properties.deviceName}, {"vendor_id", hex(properties.vendorID)},
               {"device_id", hex(properties.deviceID)}, {"api_version_raw", properties.apiVersion},
               {"api_version", std::to_string(VK_API_VERSION_MAJOR(properties.apiVersion)) + "." +
                               std::to_string(VK_API_VERSION_MINOR(properties.apiVersion)) + "." +
                               std::to_string(VK_API_VERSION_PATCH(properties.apiVersion))},
               {"driver_version_raw", properties.driverVersion}, {"driver_version_hex", hex(properties.driverVersion)},
               {"pipeline_cache_uuid", uuid.str()}, {"driver_properties_available", queried},
               {"driver_name", queried ? json(driverProperties.driverName) : json(nullptr)},
               {"driver_info", queried ? json(driverProperties.driverInfo) : json(nullptr)},
               {"driver_id", queried ? json(driverProperties.driverID) : json(nullptr)}};
    if (!queried)
        info["unavailable_reason"] = driverPropertiesSupported ? "Physical device properties2 query unavailable" :
                                                            "Vulkan 1.2 / VK_KHR_driver_properties unavailable";

    std::lock_guard<std::mutex> guard(lock);
    auto directory = std::filesystem::path(outputPath) / "metadata";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    // Separate files retain information for different physical devices.
    auto path = directory / ("vulkan-" + std::to_string(reinterpret_cast<uintptr_t>(physicalDevice)) + ".json");
    std::ofstream stream(path);
    stream << info.dump(2) << '\n';
    if (error || !stream) LAYER_ERR("Failed to write Vulkan capture metadata: %s", path.string().c_str());
}

void ShaderDump::recordShaderModule(VkShaderModule shaderModule,
                                    const uint32_t* code,
                                    size_t codeWords,
                                    const uint32_t* originalCode,
                                    size_t originalCodeWords)
{
    if (!active || !code || !codeWords) return;
    const size_t byteSize = codeWords * sizeof(uint32_t);
    const std::string hash = sha1(code, byteSize);
    const std::filesystem::path base = std::filesystem::path(outputPath) / "shaders" / hash;
    std::lock_guard<std::mutex> guard(lock);
    shaderModules[reinterpret_cast<uintptr_t>(shaderModule)] = hash;
    const bool newShader = !std::filesystem::exists(base.string() + ".spirv");
    if (newShader)
    {
        std::ofstream spirv(base.string() + ".spirv", std::ios::binary);
        spirv.write(reinterpret_cast<const char*>(code), static_cast<std::streamsize>(byteSize));
        if (detailed)
        {
            json metadata {
                {"schema_version", 1}, {"shader_id", "sha1:" + hash}, {"byte_size", byteSize},
                {"format", "spirv"}, {"original_sha1", sha1(originalCode, originalCodeWords * sizeof(uint32_t))},
                {"effective_sha1", hash}
            };
            std::ofstream jsonFile(base.string() + ".json");
            jsonFile << metadata.dump(2) << '\n';

            const std::filesystem::path table = std::filesystem::path(outputPath) / "tables" / "shaders.csv";
            const bool newTable = !std::filesystem::exists(table);
            std::ofstream csvFile(table, std::ios::app);
            if (newTable) csvFile << "shader_sha1,byte_size,format\n";
            csvFile << csv(hash) << ',' << byteSize << ",spirv\n";
        }
        ++shaderCount;
        publishProgress();
    }
}

std::string ShaderDump::lookupShader(VkShaderModule shaderModule) const
{
    const auto it = shaderModules.find(reinterpret_cast<uintptr_t>(shaderModule));
    return it == shaderModules.end() ? "" : it->second;
}

void ShaderDump::recordComputePipeline(VkDevice device, const DeviceDispatchTable& driver, VkPipeline pipeline,
                                       const VkComputePipelineCreateInfo& createInfo, bool captureStatistics)
{
    recordPipeline(device, driver, pipeline, "compute", &createInfo.stage, 1, captureStatistics);
}

void ShaderDump::recordGraphicsPipeline(VkDevice device, const DeviceDispatchTable& driver, VkPipeline pipeline,
                                        const VkGraphicsPipelineCreateInfo& createInfo, bool captureStatistics)
{
    recordPipeline(device, driver, pipeline, "graphics", createInfo.pStages, createInfo.stageCount, captureStatistics);
}

void ShaderDump::recordRayTracingPipeline(VkDevice device, const DeviceDispatchTable& driver, VkPipeline pipeline,
                                          const VkRayTracingPipelineCreateInfoKHR& createInfo, bool captureStatistics)
{
    recordPipeline(device, driver, pipeline, "ray_tracing", createInfo.pStages, createInfo.stageCount, captureStatistics);
}

void ShaderDump::recordPipeline(VkDevice device, const DeviceDispatchTable& driver, VkPipeline pipeline, const char* kind,
                                const VkPipelineShaderStageCreateInfo* stages, uint32_t stageCount, bool captureStatistics)
{
    if (!active || pipeline == VK_NULL_HANDLE) return;
    std::lock_guard<std::mutex> guard(lock);
    json shaderList = json::array();
    std::string signatureSource {kind};
    for (uint32_t i = 0; i < stageCount; ++i)
    {
        const auto& stage = stages[i];
        const std::string shader = lookupShader(stage.module);
        shaderList.push_back({{"sha1", shader}, {"stage", stageName(stage.stage)},
                              {"entry_point", stage.pName ? stage.pName : ""}});
        signatureSource += "|" + shader + "|" + std::to_string(stage.stage) + "|" + (stage.pName ? stage.pName : "");
    }
    const std::string signature = sha1(signatureSource.data(), signatureSource.size());
    pipelines[reinterpret_cast<uintptr_t>(pipeline)] = signature;
    const uint64_t instanceId = nextPipelineId++;
    json report {{"schema_version", 1}, {"pipeline_instance_id", instanceId}, {"pipeline_signature", "sha1:" + signature},
                 {"signature_quality", "shader_stages"}, {"kind", kind}, {"shaders", shaderList},
                 {"capture_status", captureStatistics ? "pending" : "statistics_disabled"}, {"executables", json::array()}};

    if (captureStatistics)
    {
        VkPipelineInfoKHR info = vku::InitStructHelper();
        info.pipeline = pipeline;
        uint32_t executableCount = 0;
        VkResult result = driver.vkGetPipelineExecutablePropertiesKHR(device, &info, &executableCount, nullptr);
        if (result == VK_SUCCESS)
        {
            std::vector<VkPipelineExecutablePropertiesKHR> properties(executableCount);
            for (auto& property : properties) property = vku::InitStructHelper();
            result = driver.vkGetPipelineExecutablePropertiesKHR(device, &info, &executableCount, properties.data());
            if (result == VK_SUCCESS || result == VK_INCOMPLETE)
            {
                report["capture_status"] = "statistics_captured";
                for (uint32_t executable = 0; executable < executableCount; ++executable)
                {
                    json executableJson {{"index", executable}, {"name", properties[executable].name},
                                         {"description", properties[executable].description},
                                         {"stages", properties[executable].stages},
                                         {"subgroup_size", properties[executable].subgroupSize}, {"statistics", json::array()}};
                    VkPipelineExecutableInfoKHR executableInfo = vku::InitStructHelper();
                    executableInfo.pipeline = pipeline;
                    executableInfo.executableIndex = executable;
                    uint32_t statisticCount = 0;
                    if (driver.vkGetPipelineExecutableStatisticsKHR(device, &executableInfo, &statisticCount, nullptr) == VK_SUCCESS)
                    {
                        std::vector<VkPipelineExecutableStatisticKHR> statistics(statisticCount);
                        for (auto& statistic : statistics) statistic = vku::InitStructHelper();
                        if (driver.vkGetPipelineExecutableStatisticsKHR(device, &executableInfo, &statisticCount, statistics.data()) == VK_SUCCESS)
                        {
                            for (uint32_t statistic = 0; statistic < statisticCount; ++statistic)
                            {
                                executableJson["statistics"].push_back({{"name", statistics[statistic].name},
                                    {"description", statistics[statistic].description}, {"format", statistics[statistic].format},
                                    {"value", statisticValue(statistics[statistic])}});
                            }
                        }
                    }
                    report["executables"].push_back(std::move(executableJson));
                }
            }
            else report["capture_status"] = "properties_query_failed";
        }
        else report["capture_status"] = "properties_query_failed";
    }

    const auto filename = std::filesystem::path(outputPath) / "pipelines" /
                          (signature + "-" + std::to_string(instanceId) + ".json");
    std::ofstream file(filename);
    file << report.dump(2) << '\n';

    if (detailed)
    {
        const std::filesystem::path executableTable = std::filesystem::path(outputPath) / "tables" /
                                                      "pipeline_executables.csv";
        const bool newExecutableTable = !std::filesystem::exists(executableTable);
        std::ofstream executableCsv(executableTable, std::ios::app);
        if (newExecutableTable)
        {
            executableCsv << "pipeline_instance_id,pipeline_signature,kind,executable_index,name,description,stages,subgroup_size,capture_status\n";
        }

        const std::filesystem::path statisticTable = std::filesystem::path(outputPath) / "tables" / "statistics_raw.csv";
        const bool newStatisticTable = !std::filesystem::exists(statisticTable);
        std::ofstream statisticCsv(statisticTable, std::ios::app);
        if (newStatisticTable)
        {
            statisticCsv << "pipeline_instance_id,pipeline_signature,executable_index,name,description,format,value\n";
        }

        for (const auto& executable : report["executables"])
        {
            executableCsv << instanceId << ',' << csv(signature) << ',' << csv(kind) << ',' << executable["index"] << ','
                          << csv(executable["name"]) << ',' << csv(executable["description"]) << ','
                          << executable["stages"] << ',' << executable["subgroup_size"] << ','
                          << csv(report["capture_status"]) << '\n';
            for (const auto& statistic : executable["statistics"])
            {
                statisticCsv << instanceId << ',' << csv(signature) << ',' << executable["index"] << ','
                             << csv(statistic["name"]) << ',' << csv(statistic["description"]) << ','
                             << statistic["format"] << ',' << csv(statistic["value"]) << '\n';
            }
        }
    }
    ++pipelineCount;
    publishProgress();
}

void ShaderDump::beginCommandBuffer(VkCommandBuffer commandBuffer)
{
    if (!active) return;
    std::lock_guard<std::mutex> guard(lock);
    commandBuffers[reinterpret_cast<uintptr_t>(commandBuffer)] = {};
}

void ShaderDump::bindPipeline(VkCommandBuffer commandBuffer, VkPipelineBindPoint bindPoint, VkPipeline pipeline)
{
    if (!active) return;
    std::lock_guard<std::mutex> guard(lock);
    commandBuffers[reinterpret_cast<uintptr_t>(commandBuffer)].boundPipelines[bindPoint] = reinterpret_cast<uintptr_t>(pipeline);
}

void ShaderDump::beginLabel(VkCommandBuffer commandBuffer, const VkDebugUtilsLabelEXT& label)
{
    if (!active || !label.pLabelName) return;
    std::lock_guard<std::mutex> guard(lock);
    commandBuffers[reinterpret_cast<uintptr_t>(commandBuffer)].labels.emplace_back(label.pLabelName);
}

void ShaderDump::endLabel(VkCommandBuffer commandBuffer)
{
    if (!active) return;
    std::lock_guard<std::mutex> guard(lock);
    auto& labels = commandBuffers[reinterpret_cast<uintptr_t>(commandBuffer)].labels;
    if (!labels.empty()) labels.pop_back();
}

static void recordUsage(ShaderDump::CommandBufferState& state,
                        const std::unordered_map<uintptr_t, std::string>& pipelines,
                        std::unordered_map<std::string, ShaderDump::Usage>& frameUsage,
                        VkPipelineBindPoint bindPoint,
                        uint32_t usageOffset)
{
    const auto bound = state.boundPipelines.find(bindPoint);
    if (bound == state.boundPipelines.end()) return;
    const auto pipeline = pipelines.find(bound->second);
    if (pipeline == pipelines.end()) return;
    std::string labelPath;
    for (const auto& label : state.labels)
    {
        if (!labelPath.empty()) labelPath += '/';
        labelPath += label;
    }
    ShaderDump::Usage& usage = frameUsage[pipeline->second + "\x1f" + labelPath];
    if (usageOffset == 0) ++usage.draws;
    else if (usageOffset == 1) ++usage.dispatches;
    else ++usage.traceRays;
}

void ShaderDump::recordDraw(VkCommandBuffer commandBuffer)
{
    if (!active) return;
    std::lock_guard<std::mutex> guard(lock);
    recordUsage(commandBuffers[reinterpret_cast<uintptr_t>(commandBuffer)], pipelines, frameUsage,
                VK_PIPELINE_BIND_POINT_GRAPHICS, 0);
}

void ShaderDump::recordDispatch(VkCommandBuffer commandBuffer)
{
    if (!active) return;
    std::lock_guard<std::mutex> guard(lock);
    recordUsage(commandBuffers[reinterpret_cast<uintptr_t>(commandBuffer)], pipelines, frameUsage,
                VK_PIPELINE_BIND_POINT_COMPUTE, 1);
}

void ShaderDump::recordTraceRays(VkCommandBuffer commandBuffer)
{
    if (!active) return;
    std::lock_guard<std::mutex> guard(lock);
    recordUsage(commandBuffers[reinterpret_cast<uintptr_t>(commandBuffer)], pipelines, frameUsage,
                VK_PIPELINE_BIND_POINT_RAY_TRACING_KHR, 2);
}

void ShaderDump::recordObjectName(const VkDebugUtilsObjectNameInfoEXT& nameInfo)
{
    if (!active || !detailed || !nameInfo.pObjectName) return;
    std::lock_guard<std::mutex> guard(lock);
    std::string objectId;
    if (nameInfo.objectType == VK_OBJECT_TYPE_SHADER_MODULE)
    {
        const auto it = shaderModules.find(static_cast<uintptr_t>(nameInfo.objectHandle));
        if (it != shaderModules.end()) objectId = "sha1:" + it->second;
    }
    else if (nameInfo.objectType == VK_OBJECT_TYPE_PIPELINE)
    {
        const auto it = pipelines.find(static_cast<uintptr_t>(nameInfo.objectHandle));
        if (it != pipelines.end()) objectId = "sha1:" + it->second;
    }
    if (objectId.empty()) return;

    const std::filesystem::path table = std::filesystem::path(outputPath) / "tables" / "debug_object_names.csv";
    const bool newTable = !std::filesystem::exists(table);
    std::ofstream file(table, std::ios::app);
    if (newTable) file << "object_type,object_id,debug_name\n";
    file << csv(nameInfo.objectType == VK_OBJECT_TYPE_PIPELINE ? "pipeline" : "shader_module") << ','
         << csv(objectId) << ',' << csv(nameInfo.pObjectName) << '\n';
}

void ShaderDump::endFrame()
{
    if (!active) return;
    std::lock_guard<std::mutex> guard(lock);
    ++frameNumber;
    const uint64_t firstFrame = firstCapturedFrame ? firstCapturedFrame : 1;
    const bool insideCaptureWindow = writeFrameSummary && frameNumber >= firstFrame &&
                                     (!capturedFrameCount || frameNumber < firstFrame + capturedFrameCount);
    if (!insideCaptureWindow)
    {
        frameUsage.clear();
        return;
    }
    if (frameUsage.empty()) return;

    json report;
    std::ofstream file;
    if (detailed)
    {
        const std::filesystem::path frameFile = std::filesystem::path(outputPath) / "frames" /
                                                (std::to_string(frameNumber) + ".json");
        std::error_code error;
        std::filesystem::create_directories(frameFile.parent_path(), error);
        report = {{"schema_version", 1}, {"frame", frameNumber}, {"usage", json::array()}};
        file.open(frameFile);
    }
    const std::filesystem::path table = std::filesystem::path(outputPath) / "tables" / "frame_pipeline_usage.csv";
    const bool newTable = !std::filesystem::exists(table);
    std::ofstream csvFile(table, std::ios::app);
    if (newTable) csvFile << "frame,pipeline_signature,label_path,draw_count,dispatch_count,trace_rays_count\n";
    for (const auto& [key, usage] : frameUsage)
    {
        const size_t separator = key.find('\x1f');
        const std::string signature = key.substr(0, separator);
        const std::string label = key.substr(separator + 1);
        if (detailed)
        {
            report["usage"].push_back({{"pipeline_signature", signature}, {"label_path", label},
                                        {"draw_count", usage.draws}, {"dispatch_count", usage.dispatches},
                                        {"trace_rays_count", usage.traceRays}});
        }
        csvFile << frameNumber << ',' << csv(signature) << ',' << csv(label) << ',' << usage.draws << ','
                << usage.dispatches << ',' << usage.traceRays << '\n';
    }
    if (detailed)
    {
        file << report.dump(2) << '\n';
    }
    ++writtenFrameCount;
    publishProgress();
    frameUsage.clear();
}

void ShaderDump::publishProgress(bool force)
{
    if (!comms)
    {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (!force && lastProgress.time_since_epoch().count() != 0 && now - lastProgress < std::chrono::seconds(1))
    {
        return;
    }

    lastProgress = now;
    comms->txProgress(shaderCount, pipelineCount, writtenFrameCount);
}
