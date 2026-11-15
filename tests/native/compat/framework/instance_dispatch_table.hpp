/* Minimal driver-double table; no layer function lookup maps in host tests. */
#pragma once
#include "framework/instance_functions.hpp"
struct InstanceDispatchTable {
    PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties {nullptr};
    PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2 {nullptr};
    PFN_vkGetPhysicalDeviceProperties2KHR vkGetPhysicalDeviceProperties2KHR {nullptr};
    PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties {nullptr};
};
