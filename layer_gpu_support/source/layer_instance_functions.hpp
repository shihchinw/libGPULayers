/* SPDX-License-Identifier: MIT */
#pragma once

template<>
VKAPI_ATTR VkResult VKAPI_CALL layer_vkCreateInstance<user_tag>(
    const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance);
