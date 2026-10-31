#ifndef HOST_SHIM_GLAD_VULKAN_H_
#define HOST_SHIM_GLAD_VULKAN_H_

/* The glad Vulkan loader of host/third_party/glad plus the Vulkan 1.4 core
   names ReShade's Vulkan code uses for VK_KHR_maintenance5 (the glad pinned
   by ReShade generates only the KHR names from deps/khronos/vk.xml at
   vulkan=1.3). host\shims is first in the include path, so <glad/vulkan.h>
   lands here everywhere in RedemptionEditorHost. */

#include "../../third_party/glad/include/glad/vulkan.h"

#ifndef VK_VERSION_1_4
typedef VkBufferUsageFlags2KHR VkBufferUsageFlags2;
typedef VkPipelineCreateFlags2KHR VkPipelineCreateFlags2;
typedef VkBufferUsageFlags2CreateInfoKHR VkBufferUsageFlags2CreateInfo;
#define VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR
#endif

#endif
