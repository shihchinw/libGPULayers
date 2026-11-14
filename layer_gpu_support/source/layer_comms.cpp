/*
 * SPDX-License-Identifier: MIT
 * -----------------------------------------------------------------------------
 * Copyright (c) 2026 Arm Limited
 */

#include "layer_comms.hpp"

#include <memory>
#include <string>

ShaderDumpComms::ShaderDumpComms(Comms::CommsInterface& _comms)
    : comms(_comms)
{
    if (comms.isConnected())
    {
        endpoint = comms.getEndpointID("GPUShaderDump");
    }
}

void ShaderDumpComms::txCaptureStarted()
{
    txMessage("capture_started", 0, 0, 0);
}

void ShaderDumpComms::txProgress(uint64_t shaderCount, uint64_t pipelineCount, uint64_t frameCount)
{
    txMessage("progress", shaderCount, pipelineCount, frameCount);
}

void ShaderDumpComms::txCaptureFinished(uint64_t shaderCount, uint64_t pipelineCount, uint64_t frameCount)
{
    txMessage("capture_finished", shaderCount, pipelineCount, frameCount);
}

void ShaderDumpComms::txMessage(const char* type, uint64_t shaderCount, uint64_t pipelineCount, uint64_t frameCount)
{
    if (endpoint == Comms::NO_ENDPOINT)
    {
        return;
    }

    const std::string message = std::string("{\"type\":\"") + type + "\",\"shaders\":" +
                                std::to_string(shaderCount) + ",\"pipelines\":" +
                                std::to_string(pipelineCount) + ",\"frames\":" +
                                std::to_string(frameCount) + "}";
    auto data = std::make_unique<Comms::MessageData>(message.begin(), message.end());
    comms.txAsync(endpoint, std::move(data));
}
