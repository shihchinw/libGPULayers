/* Minimal driver-double table. Production layout is validated by the Android build. */
#pragma once
#include "framework/device_functions.hpp"
struct DeviceDispatchTable {
    PFN_vkCmdDraw vkCmdDraw {nullptr};
    PFN_vkCmdDrawIndexed vkCmdDrawIndexed {nullptr};
    PFN_vkCmdDrawIndexedIndirect vkCmdDrawIndexedIndirect {nullptr};
    PFN_vkCmdDrawIndexedIndirect2KHR vkCmdDrawIndexedIndirect2KHR {nullptr};
    PFN_vkCmdDrawIndexedIndirectCount vkCmdDrawIndexedIndirectCount {nullptr};
    PFN_vkCmdDrawIndexedIndirectCount2KHR vkCmdDrawIndexedIndirectCount2KHR {nullptr};
    PFN_vkCmdDrawIndexedIndirectCountKHR vkCmdDrawIndexedIndirectCountKHR {nullptr};
    PFN_vkCmdDrawIndirect vkCmdDrawIndirect {nullptr};
    PFN_vkCmdDrawIndirect2KHR vkCmdDrawIndirect2KHR {nullptr};
    PFN_vkCmdDrawIndirectByteCount2EXT vkCmdDrawIndirectByteCount2EXT {nullptr};
    PFN_vkCmdDrawIndirectByteCountEXT vkCmdDrawIndirectByteCountEXT {nullptr};
    PFN_vkCmdDrawIndirectCount vkCmdDrawIndirectCount {nullptr};
    PFN_vkCmdDrawIndirectCount2KHR vkCmdDrawIndirectCount2KHR {nullptr};
    PFN_vkCmdDrawIndirectCountKHR vkCmdDrawIndirectCountKHR {nullptr};
    PFN_vkCmdDrawMeshTasksEXT vkCmdDrawMeshTasksEXT {nullptr};
    PFN_vkCmdDrawMeshTasksIndirect2EXT vkCmdDrawMeshTasksIndirect2EXT {nullptr};
    PFN_vkCmdDrawMeshTasksIndirectCount2EXT vkCmdDrawMeshTasksIndirectCount2EXT {nullptr};
    PFN_vkCmdDrawMeshTasksIndirectCountEXT vkCmdDrawMeshTasksIndirectCountEXT {nullptr};
    PFN_vkCmdDrawMeshTasksIndirectEXT vkCmdDrawMeshTasksIndirectEXT {nullptr};
    PFN_vkCmdDrawMultiEXT vkCmdDrawMultiEXT {nullptr};
    PFN_vkCmdDrawMultiIndexedEXT vkCmdDrawMultiIndexedEXT {nullptr};
    PFN_vkBeginCommandBuffer vkBeginCommandBuffer {nullptr};
    PFN_vkCmdBindPipeline vkCmdBindPipeline {nullptr};
    PFN_vkCmdBeginDebugUtilsLabelEXT vkCmdBeginDebugUtilsLabelEXT {nullptr};
    PFN_vkCmdEndDebugUtilsLabelEXT vkCmdEndDebugUtilsLabelEXT {nullptr};
    PFN_vkCmdDebugMarkerBeginEXT vkCmdDebugMarkerBeginEXT {nullptr};
    PFN_vkCmdDebugMarkerEndEXT vkCmdDebugMarkerEndEXT {nullptr};
    PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT {nullptr};
    PFN_vkQueuePresentKHR vkQueuePresentKHR {nullptr};
    PFN_vkGetPipelineExecutablePropertiesKHR vkGetPipelineExecutablePropertiesKHR {nullptr};
    PFN_vkGetPipelineExecutableStatisticsKHR vkGetPipelineExecutableStatisticsKHR {nullptr};
};
