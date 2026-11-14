/*
 * SPDX-License-Identifier: MIT
 * -----------------------------------------------------------------------------
 * Copyright (c) 2026 Arm Limited
 */

#pragma once

#include <cstdint>

#include "comms/comms_interface.hpp"

/**
 * @brief Encoder for lightweight shader dump progress notifications.
 */
class ShaderDumpComms
{
public:
    explicit ShaderDumpComms(Comms::CommsInterface& comms);

    void txCaptureStarted();
    void txProgress(uint64_t shaderCount, uint64_t pipelineCount, uint64_t frameCount);
    void txCaptureFinished(uint64_t shaderCount, uint64_t pipelineCount, uint64_t frameCount);

private:
    void txMessage(const char* type, uint64_t shaderCount, uint64_t pipelineCount, uint64_t frameCount);

    Comms::EndpointID endpoint {Comms::NO_ENDPOINT};
    Comms::CommsInterface& comms;
};
