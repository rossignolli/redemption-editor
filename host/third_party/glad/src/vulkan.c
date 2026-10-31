/**
 * SPDX-License-Identifier: (WTFPL OR CC0-1.0) AND Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glad/vulkan.h>

#ifndef GLAD_IMPL_UTIL_C_
#define GLAD_IMPL_UTIL_C_

#ifdef _MSC_VER
#define GLAD_IMPL_UTIL_SSCANF sscanf_s
#else
#define GLAD_IMPL_UTIL_SSCANF sscanf
#endif

#endif /* GLAD_IMPL_UTIL_C_ */

#ifdef __cplusplus
extern "C" {
#endif








static void glad_vk_load_VK_VERSION_1_0(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->VERSION_1_0) return;
    context->AllocateCommandBuffers = (PFN_vkAllocateCommandBuffers) load(userptr, "vkAllocateCommandBuffers");
    context->AllocateDescriptorSets = (PFN_vkAllocateDescriptorSets) load(userptr, "vkAllocateDescriptorSets");
    context->AllocateMemory = (PFN_vkAllocateMemory) load(userptr, "vkAllocateMemory");
    context->BeginCommandBuffer = (PFN_vkBeginCommandBuffer) load(userptr, "vkBeginCommandBuffer");
    context->BindBufferMemory = (PFN_vkBindBufferMemory) load(userptr, "vkBindBufferMemory");
    context->BindImageMemory = (PFN_vkBindImageMemory) load(userptr, "vkBindImageMemory");
    context->CmdBeginQuery = (PFN_vkCmdBeginQuery) load(userptr, "vkCmdBeginQuery");
    context->CmdBeginRenderPass = (PFN_vkCmdBeginRenderPass) load(userptr, "vkCmdBeginRenderPass");
    context->CmdBindDescriptorSets = (PFN_vkCmdBindDescriptorSets) load(userptr, "vkCmdBindDescriptorSets");
    context->CmdBindIndexBuffer = (PFN_vkCmdBindIndexBuffer) load(userptr, "vkCmdBindIndexBuffer");
    context->CmdBindPipeline = (PFN_vkCmdBindPipeline) load(userptr, "vkCmdBindPipeline");
    context->CmdBindVertexBuffers = (PFN_vkCmdBindVertexBuffers) load(userptr, "vkCmdBindVertexBuffers");
    context->CmdBlitImage = (PFN_vkCmdBlitImage) load(userptr, "vkCmdBlitImage");
    context->CmdClearAttachments = (PFN_vkCmdClearAttachments) load(userptr, "vkCmdClearAttachments");
    context->CmdClearColorImage = (PFN_vkCmdClearColorImage) load(userptr, "vkCmdClearColorImage");
    context->CmdClearDepthStencilImage = (PFN_vkCmdClearDepthStencilImage) load(userptr, "vkCmdClearDepthStencilImage");
    context->CmdCopyBuffer = (PFN_vkCmdCopyBuffer) load(userptr, "vkCmdCopyBuffer");
    context->CmdCopyBufferToImage = (PFN_vkCmdCopyBufferToImage) load(userptr, "vkCmdCopyBufferToImage");
    context->CmdCopyImage = (PFN_vkCmdCopyImage) load(userptr, "vkCmdCopyImage");
    context->CmdCopyImageToBuffer = (PFN_vkCmdCopyImageToBuffer) load(userptr, "vkCmdCopyImageToBuffer");
    context->CmdCopyQueryPoolResults = (PFN_vkCmdCopyQueryPoolResults) load(userptr, "vkCmdCopyQueryPoolResults");
    context->CmdDispatch = (PFN_vkCmdDispatch) load(userptr, "vkCmdDispatch");
    context->CmdDispatchIndirect = (PFN_vkCmdDispatchIndirect) load(userptr, "vkCmdDispatchIndirect");
    context->CmdDraw = (PFN_vkCmdDraw) load(userptr, "vkCmdDraw");
    context->CmdDrawIndexed = (PFN_vkCmdDrawIndexed) load(userptr, "vkCmdDrawIndexed");
    context->CmdDrawIndexedIndirect = (PFN_vkCmdDrawIndexedIndirect) load(userptr, "vkCmdDrawIndexedIndirect");
    context->CmdDrawIndirect = (PFN_vkCmdDrawIndirect) load(userptr, "vkCmdDrawIndirect");
    context->CmdEndQuery = (PFN_vkCmdEndQuery) load(userptr, "vkCmdEndQuery");
    context->CmdEndRenderPass = (PFN_vkCmdEndRenderPass) load(userptr, "vkCmdEndRenderPass");
    context->CmdExecuteCommands = (PFN_vkCmdExecuteCommands) load(userptr, "vkCmdExecuteCommands");
    context->CmdFillBuffer = (PFN_vkCmdFillBuffer) load(userptr, "vkCmdFillBuffer");
    context->CmdNextSubpass = (PFN_vkCmdNextSubpass) load(userptr, "vkCmdNextSubpass");
    context->CmdPipelineBarrier = (PFN_vkCmdPipelineBarrier) load(userptr, "vkCmdPipelineBarrier");
    context->CmdPushConstants = (PFN_vkCmdPushConstants) load(userptr, "vkCmdPushConstants");
    context->CmdResetEvent = (PFN_vkCmdResetEvent) load(userptr, "vkCmdResetEvent");
    context->CmdResetQueryPool = (PFN_vkCmdResetQueryPool) load(userptr, "vkCmdResetQueryPool");
    context->CmdResolveImage = (PFN_vkCmdResolveImage) load(userptr, "vkCmdResolveImage");
    context->CmdSetBlendConstants = (PFN_vkCmdSetBlendConstants) load(userptr, "vkCmdSetBlendConstants");
    context->CmdSetDepthBias = (PFN_vkCmdSetDepthBias) load(userptr, "vkCmdSetDepthBias");
    context->CmdSetDepthBounds = (PFN_vkCmdSetDepthBounds) load(userptr, "vkCmdSetDepthBounds");
    context->CmdSetEvent = (PFN_vkCmdSetEvent) load(userptr, "vkCmdSetEvent");
    context->CmdSetLineWidth = (PFN_vkCmdSetLineWidth) load(userptr, "vkCmdSetLineWidth");
    context->CmdSetScissor = (PFN_vkCmdSetScissor) load(userptr, "vkCmdSetScissor");
    context->CmdSetStencilCompareMask = (PFN_vkCmdSetStencilCompareMask) load(userptr, "vkCmdSetStencilCompareMask");
    context->CmdSetStencilReference = (PFN_vkCmdSetStencilReference) load(userptr, "vkCmdSetStencilReference");
    context->CmdSetStencilWriteMask = (PFN_vkCmdSetStencilWriteMask) load(userptr, "vkCmdSetStencilWriteMask");
    context->CmdSetViewport = (PFN_vkCmdSetViewport) load(userptr, "vkCmdSetViewport");
    context->CmdUpdateBuffer = (PFN_vkCmdUpdateBuffer) load(userptr, "vkCmdUpdateBuffer");
    context->CmdWaitEvents = (PFN_vkCmdWaitEvents) load(userptr, "vkCmdWaitEvents");
    context->CmdWriteTimestamp = (PFN_vkCmdWriteTimestamp) load(userptr, "vkCmdWriteTimestamp");
    context->CreateBuffer = (PFN_vkCreateBuffer) load(userptr, "vkCreateBuffer");
    context->CreateBufferView = (PFN_vkCreateBufferView) load(userptr, "vkCreateBufferView");
    context->CreateCommandPool = (PFN_vkCreateCommandPool) load(userptr, "vkCreateCommandPool");
    context->CreateComputePipelines = (PFN_vkCreateComputePipelines) load(userptr, "vkCreateComputePipelines");
    context->CreateDescriptorPool = (PFN_vkCreateDescriptorPool) load(userptr, "vkCreateDescriptorPool");
    context->CreateDescriptorSetLayout = (PFN_vkCreateDescriptorSetLayout) load(userptr, "vkCreateDescriptorSetLayout");
    context->CreateDevice = (PFN_vkCreateDevice) load(userptr, "vkCreateDevice");
    context->CreateEvent = (PFN_vkCreateEvent) load(userptr, "vkCreateEvent");
    context->CreateFence = (PFN_vkCreateFence) load(userptr, "vkCreateFence");
    context->CreateFramebuffer = (PFN_vkCreateFramebuffer) load(userptr, "vkCreateFramebuffer");
    context->CreateGraphicsPipelines = (PFN_vkCreateGraphicsPipelines) load(userptr, "vkCreateGraphicsPipelines");
    context->CreateImage = (PFN_vkCreateImage) load(userptr, "vkCreateImage");
    context->CreateImageView = (PFN_vkCreateImageView) load(userptr, "vkCreateImageView");
    context->CreateInstance = (PFN_vkCreateInstance) load(userptr, "vkCreateInstance");
    context->CreatePipelineCache = (PFN_vkCreatePipelineCache) load(userptr, "vkCreatePipelineCache");
    context->CreatePipelineLayout = (PFN_vkCreatePipelineLayout) load(userptr, "vkCreatePipelineLayout");
    context->CreateQueryPool = (PFN_vkCreateQueryPool) load(userptr, "vkCreateQueryPool");
    context->CreateRenderPass = (PFN_vkCreateRenderPass) load(userptr, "vkCreateRenderPass");
    context->CreateSampler = (PFN_vkCreateSampler) load(userptr, "vkCreateSampler");
    context->CreateSemaphore = (PFN_vkCreateSemaphore) load(userptr, "vkCreateSemaphore");
    context->CreateShaderModule = (PFN_vkCreateShaderModule) load(userptr, "vkCreateShaderModule");
    context->DestroyBuffer = (PFN_vkDestroyBuffer) load(userptr, "vkDestroyBuffer");
    context->DestroyBufferView = (PFN_vkDestroyBufferView) load(userptr, "vkDestroyBufferView");
    context->DestroyCommandPool = (PFN_vkDestroyCommandPool) load(userptr, "vkDestroyCommandPool");
    context->DestroyDescriptorPool = (PFN_vkDestroyDescriptorPool) load(userptr, "vkDestroyDescriptorPool");
    context->DestroyDescriptorSetLayout = (PFN_vkDestroyDescriptorSetLayout) load(userptr, "vkDestroyDescriptorSetLayout");
    context->DestroyDevice = (PFN_vkDestroyDevice) load(userptr, "vkDestroyDevice");
    context->DestroyEvent = (PFN_vkDestroyEvent) load(userptr, "vkDestroyEvent");
    context->DestroyFence = (PFN_vkDestroyFence) load(userptr, "vkDestroyFence");
    context->DestroyFramebuffer = (PFN_vkDestroyFramebuffer) load(userptr, "vkDestroyFramebuffer");
    context->DestroyImage = (PFN_vkDestroyImage) load(userptr, "vkDestroyImage");
    context->DestroyImageView = (PFN_vkDestroyImageView) load(userptr, "vkDestroyImageView");
    context->DestroyInstance = (PFN_vkDestroyInstance) load(userptr, "vkDestroyInstance");
    context->DestroyPipeline = (PFN_vkDestroyPipeline) load(userptr, "vkDestroyPipeline");
    context->DestroyPipelineCache = (PFN_vkDestroyPipelineCache) load(userptr, "vkDestroyPipelineCache");
    context->DestroyPipelineLayout = (PFN_vkDestroyPipelineLayout) load(userptr, "vkDestroyPipelineLayout");
    context->DestroyQueryPool = (PFN_vkDestroyQueryPool) load(userptr, "vkDestroyQueryPool");
    context->DestroyRenderPass = (PFN_vkDestroyRenderPass) load(userptr, "vkDestroyRenderPass");
    context->DestroySampler = (PFN_vkDestroySampler) load(userptr, "vkDestroySampler");
    context->DestroySemaphore = (PFN_vkDestroySemaphore) load(userptr, "vkDestroySemaphore");
    context->DestroyShaderModule = (PFN_vkDestroyShaderModule) load(userptr, "vkDestroyShaderModule");
    context->DeviceWaitIdle = (PFN_vkDeviceWaitIdle) load(userptr, "vkDeviceWaitIdle");
    context->EndCommandBuffer = (PFN_vkEndCommandBuffer) load(userptr, "vkEndCommandBuffer");
    context->EnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties) load(userptr, "vkEnumerateDeviceExtensionProperties");
    context->EnumerateDeviceLayerProperties = (PFN_vkEnumerateDeviceLayerProperties) load(userptr, "vkEnumerateDeviceLayerProperties");
    context->EnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties) load(userptr, "vkEnumerateInstanceExtensionProperties");
    context->EnumerateInstanceLayerProperties = (PFN_vkEnumerateInstanceLayerProperties) load(userptr, "vkEnumerateInstanceLayerProperties");
    context->EnumeratePhysicalDevices = (PFN_vkEnumeratePhysicalDevices) load(userptr, "vkEnumeratePhysicalDevices");
    context->FlushMappedMemoryRanges = (PFN_vkFlushMappedMemoryRanges) load(userptr, "vkFlushMappedMemoryRanges");
    context->FreeCommandBuffers = (PFN_vkFreeCommandBuffers) load(userptr, "vkFreeCommandBuffers");
    context->FreeDescriptorSets = (PFN_vkFreeDescriptorSets) load(userptr, "vkFreeDescriptorSets");
    context->FreeMemory = (PFN_vkFreeMemory) load(userptr, "vkFreeMemory");
    context->GetBufferMemoryRequirements = (PFN_vkGetBufferMemoryRequirements) load(userptr, "vkGetBufferMemoryRequirements");
    context->GetDeviceMemoryCommitment = (PFN_vkGetDeviceMemoryCommitment) load(userptr, "vkGetDeviceMemoryCommitment");
    context->GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr) load(userptr, "vkGetDeviceProcAddr");
    context->GetDeviceQueue = (PFN_vkGetDeviceQueue) load(userptr, "vkGetDeviceQueue");
    context->GetEventStatus = (PFN_vkGetEventStatus) load(userptr, "vkGetEventStatus");
    context->GetFenceStatus = (PFN_vkGetFenceStatus) load(userptr, "vkGetFenceStatus");
    context->GetImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements) load(userptr, "vkGetImageMemoryRequirements");
    context->GetImageSparseMemoryRequirements = (PFN_vkGetImageSparseMemoryRequirements) load(userptr, "vkGetImageSparseMemoryRequirements");
    context->GetImageSubresourceLayout = (PFN_vkGetImageSubresourceLayout) load(userptr, "vkGetImageSubresourceLayout");
    context->GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr) load(userptr, "vkGetInstanceProcAddr");
    context->GetPhysicalDeviceFeatures = (PFN_vkGetPhysicalDeviceFeatures) load(userptr, "vkGetPhysicalDeviceFeatures");
    context->GetPhysicalDeviceFormatProperties = (PFN_vkGetPhysicalDeviceFormatProperties) load(userptr, "vkGetPhysicalDeviceFormatProperties");
    context->GetPhysicalDeviceImageFormatProperties = (PFN_vkGetPhysicalDeviceImageFormatProperties) load(userptr, "vkGetPhysicalDeviceImageFormatProperties");
    context->GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties) load(userptr, "vkGetPhysicalDeviceMemoryProperties");
    context->GetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties) load(userptr, "vkGetPhysicalDeviceProperties");
    context->GetPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties) load(userptr, "vkGetPhysicalDeviceQueueFamilyProperties");
    context->GetPhysicalDeviceSparseImageFormatProperties = (PFN_vkGetPhysicalDeviceSparseImageFormatProperties) load(userptr, "vkGetPhysicalDeviceSparseImageFormatProperties");
    context->GetPipelineCacheData = (PFN_vkGetPipelineCacheData) load(userptr, "vkGetPipelineCacheData");
    context->GetQueryPoolResults = (PFN_vkGetQueryPoolResults) load(userptr, "vkGetQueryPoolResults");
    context->GetRenderAreaGranularity = (PFN_vkGetRenderAreaGranularity) load(userptr, "vkGetRenderAreaGranularity");
    context->InvalidateMappedMemoryRanges = (PFN_vkInvalidateMappedMemoryRanges) load(userptr, "vkInvalidateMappedMemoryRanges");
    context->MapMemory = (PFN_vkMapMemory) load(userptr, "vkMapMemory");
    context->MergePipelineCaches = (PFN_vkMergePipelineCaches) load(userptr, "vkMergePipelineCaches");
    context->QueueBindSparse = (PFN_vkQueueBindSparse) load(userptr, "vkQueueBindSparse");
    context->QueueSubmit = (PFN_vkQueueSubmit) load(userptr, "vkQueueSubmit");
    context->QueueWaitIdle = (PFN_vkQueueWaitIdle) load(userptr, "vkQueueWaitIdle");
    context->ResetCommandBuffer = (PFN_vkResetCommandBuffer) load(userptr, "vkResetCommandBuffer");
    context->ResetCommandPool = (PFN_vkResetCommandPool) load(userptr, "vkResetCommandPool");
    context->ResetDescriptorPool = (PFN_vkResetDescriptorPool) load(userptr, "vkResetDescriptorPool");
    context->ResetEvent = (PFN_vkResetEvent) load(userptr, "vkResetEvent");
    context->ResetFences = (PFN_vkResetFences) load(userptr, "vkResetFences");
    context->SetEvent = (PFN_vkSetEvent) load(userptr, "vkSetEvent");
    context->UnmapMemory = (PFN_vkUnmapMemory) load(userptr, "vkUnmapMemory");
    context->UpdateDescriptorSets = (PFN_vkUpdateDescriptorSets) load(userptr, "vkUpdateDescriptorSets");
    context->WaitForFences = (PFN_vkWaitForFences) load(userptr, "vkWaitForFences");
}
static void glad_vk_load_VK_VERSION_1_1(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->VERSION_1_1) return;
    context->BindBufferMemory2 = (PFN_vkBindBufferMemory2) load(userptr, "vkBindBufferMemory2");
    context->BindImageMemory2 = (PFN_vkBindImageMemory2) load(userptr, "vkBindImageMemory2");
    context->CmdDispatchBase = (PFN_vkCmdDispatchBase) load(userptr, "vkCmdDispatchBase");
    context->CmdSetDeviceMask = (PFN_vkCmdSetDeviceMask) load(userptr, "vkCmdSetDeviceMask");
    context->CreateDescriptorUpdateTemplate = (PFN_vkCreateDescriptorUpdateTemplate) load(userptr, "vkCreateDescriptorUpdateTemplate");
    context->CreateSamplerYcbcrConversion = (PFN_vkCreateSamplerYcbcrConversion) load(userptr, "vkCreateSamplerYcbcrConversion");
    context->DestroyDescriptorUpdateTemplate = (PFN_vkDestroyDescriptorUpdateTemplate) load(userptr, "vkDestroyDescriptorUpdateTemplate");
    context->DestroySamplerYcbcrConversion = (PFN_vkDestroySamplerYcbcrConversion) load(userptr, "vkDestroySamplerYcbcrConversion");
    context->EnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion) load(userptr, "vkEnumerateInstanceVersion");
    context->EnumeratePhysicalDeviceGroups = (PFN_vkEnumeratePhysicalDeviceGroups) load(userptr, "vkEnumeratePhysicalDeviceGroups");
    context->GetBufferMemoryRequirements2 = (PFN_vkGetBufferMemoryRequirements2) load(userptr, "vkGetBufferMemoryRequirements2");
    context->GetDescriptorSetLayoutSupport = (PFN_vkGetDescriptorSetLayoutSupport) load(userptr, "vkGetDescriptorSetLayoutSupport");
    context->GetDeviceGroupPeerMemoryFeatures = (PFN_vkGetDeviceGroupPeerMemoryFeatures) load(userptr, "vkGetDeviceGroupPeerMemoryFeatures");
    context->GetDeviceQueue2 = (PFN_vkGetDeviceQueue2) load(userptr, "vkGetDeviceQueue2");
    context->GetImageMemoryRequirements2 = (PFN_vkGetImageMemoryRequirements2) load(userptr, "vkGetImageMemoryRequirements2");
    context->GetImageSparseMemoryRequirements2 = (PFN_vkGetImageSparseMemoryRequirements2) load(userptr, "vkGetImageSparseMemoryRequirements2");
    context->GetPhysicalDeviceExternalBufferProperties = (PFN_vkGetPhysicalDeviceExternalBufferProperties) load(userptr, "vkGetPhysicalDeviceExternalBufferProperties");
    context->GetPhysicalDeviceExternalFenceProperties = (PFN_vkGetPhysicalDeviceExternalFenceProperties) load(userptr, "vkGetPhysicalDeviceExternalFenceProperties");
    context->GetPhysicalDeviceExternalSemaphoreProperties = (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties) load(userptr, "vkGetPhysicalDeviceExternalSemaphoreProperties");
    context->GetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2) load(userptr, "vkGetPhysicalDeviceFeatures2");
    context->GetPhysicalDeviceFormatProperties2 = (PFN_vkGetPhysicalDeviceFormatProperties2) load(userptr, "vkGetPhysicalDeviceFormatProperties2");
    context->GetPhysicalDeviceImageFormatProperties2 = (PFN_vkGetPhysicalDeviceImageFormatProperties2) load(userptr, "vkGetPhysicalDeviceImageFormatProperties2");
    context->GetPhysicalDeviceMemoryProperties2 = (PFN_vkGetPhysicalDeviceMemoryProperties2) load(userptr, "vkGetPhysicalDeviceMemoryProperties2");
    context->GetPhysicalDeviceProperties2 = (PFN_vkGetPhysicalDeviceProperties2) load(userptr, "vkGetPhysicalDeviceProperties2");
    context->GetPhysicalDeviceQueueFamilyProperties2 = (PFN_vkGetPhysicalDeviceQueueFamilyProperties2) load(userptr, "vkGetPhysicalDeviceQueueFamilyProperties2");
    context->GetPhysicalDeviceSparseImageFormatProperties2 = (PFN_vkGetPhysicalDeviceSparseImageFormatProperties2) load(userptr, "vkGetPhysicalDeviceSparseImageFormatProperties2");
    context->TrimCommandPool = (PFN_vkTrimCommandPool) load(userptr, "vkTrimCommandPool");
    context->UpdateDescriptorSetWithTemplate = (PFN_vkUpdateDescriptorSetWithTemplate) load(userptr, "vkUpdateDescriptorSetWithTemplate");
}
static void glad_vk_load_VK_VERSION_1_2(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->VERSION_1_2) return;
    context->CmdBeginRenderPass2 = (PFN_vkCmdBeginRenderPass2) load(userptr, "vkCmdBeginRenderPass2");
    context->CmdDrawIndexedIndirectCount = (PFN_vkCmdDrawIndexedIndirectCount) load(userptr, "vkCmdDrawIndexedIndirectCount");
    context->CmdDrawIndirectCount = (PFN_vkCmdDrawIndirectCount) load(userptr, "vkCmdDrawIndirectCount");
    context->CmdEndRenderPass2 = (PFN_vkCmdEndRenderPass2) load(userptr, "vkCmdEndRenderPass2");
    context->CmdNextSubpass2 = (PFN_vkCmdNextSubpass2) load(userptr, "vkCmdNextSubpass2");
    context->CreateRenderPass2 = (PFN_vkCreateRenderPass2) load(userptr, "vkCreateRenderPass2");
    context->GetBufferDeviceAddress = (PFN_vkGetBufferDeviceAddress) load(userptr, "vkGetBufferDeviceAddress");
    context->GetBufferOpaqueCaptureAddress = (PFN_vkGetBufferOpaqueCaptureAddress) load(userptr, "vkGetBufferOpaqueCaptureAddress");
    context->GetDeviceMemoryOpaqueCaptureAddress = (PFN_vkGetDeviceMemoryOpaqueCaptureAddress) load(userptr, "vkGetDeviceMemoryOpaqueCaptureAddress");
    context->GetSemaphoreCounterValue = (PFN_vkGetSemaphoreCounterValue) load(userptr, "vkGetSemaphoreCounterValue");
    context->ResetQueryPool = (PFN_vkResetQueryPool) load(userptr, "vkResetQueryPool");
    context->SignalSemaphore = (PFN_vkSignalSemaphore) load(userptr, "vkSignalSemaphore");
    context->WaitSemaphores = (PFN_vkWaitSemaphores) load(userptr, "vkWaitSemaphores");
}
static void glad_vk_load_VK_VERSION_1_3(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->VERSION_1_3) return;
    context->CmdBeginRendering = (PFN_vkCmdBeginRendering) load(userptr, "vkCmdBeginRendering");
    context->CmdBindVertexBuffers2 = (PFN_vkCmdBindVertexBuffers2) load(userptr, "vkCmdBindVertexBuffers2");
    context->CmdBlitImage2 = (PFN_vkCmdBlitImage2) load(userptr, "vkCmdBlitImage2");
    context->CmdCopyBuffer2 = (PFN_vkCmdCopyBuffer2) load(userptr, "vkCmdCopyBuffer2");
    context->CmdCopyBufferToImage2 = (PFN_vkCmdCopyBufferToImage2) load(userptr, "vkCmdCopyBufferToImage2");
    context->CmdCopyImage2 = (PFN_vkCmdCopyImage2) load(userptr, "vkCmdCopyImage2");
    context->CmdCopyImageToBuffer2 = (PFN_vkCmdCopyImageToBuffer2) load(userptr, "vkCmdCopyImageToBuffer2");
    context->CmdEndRendering = (PFN_vkCmdEndRendering) load(userptr, "vkCmdEndRendering");
    context->CmdPipelineBarrier2 = (PFN_vkCmdPipelineBarrier2) load(userptr, "vkCmdPipelineBarrier2");
    context->CmdResetEvent2 = (PFN_vkCmdResetEvent2) load(userptr, "vkCmdResetEvent2");
    context->CmdResolveImage2 = (PFN_vkCmdResolveImage2) load(userptr, "vkCmdResolveImage2");
    context->CmdSetCullMode = (PFN_vkCmdSetCullMode) load(userptr, "vkCmdSetCullMode");
    context->CmdSetDepthBiasEnable = (PFN_vkCmdSetDepthBiasEnable) load(userptr, "vkCmdSetDepthBiasEnable");
    context->CmdSetDepthBoundsTestEnable = (PFN_vkCmdSetDepthBoundsTestEnable) load(userptr, "vkCmdSetDepthBoundsTestEnable");
    context->CmdSetDepthCompareOp = (PFN_vkCmdSetDepthCompareOp) load(userptr, "vkCmdSetDepthCompareOp");
    context->CmdSetDepthTestEnable = (PFN_vkCmdSetDepthTestEnable) load(userptr, "vkCmdSetDepthTestEnable");
    context->CmdSetDepthWriteEnable = (PFN_vkCmdSetDepthWriteEnable) load(userptr, "vkCmdSetDepthWriteEnable");
    context->CmdSetEvent2 = (PFN_vkCmdSetEvent2) load(userptr, "vkCmdSetEvent2");
    context->CmdSetFrontFace = (PFN_vkCmdSetFrontFace) load(userptr, "vkCmdSetFrontFace");
    context->CmdSetPrimitiveRestartEnable = (PFN_vkCmdSetPrimitiveRestartEnable) load(userptr, "vkCmdSetPrimitiveRestartEnable");
    context->CmdSetPrimitiveTopology = (PFN_vkCmdSetPrimitiveTopology) load(userptr, "vkCmdSetPrimitiveTopology");
    context->CmdSetRasterizerDiscardEnable = (PFN_vkCmdSetRasterizerDiscardEnable) load(userptr, "vkCmdSetRasterizerDiscardEnable");
    context->CmdSetScissorWithCount = (PFN_vkCmdSetScissorWithCount) load(userptr, "vkCmdSetScissorWithCount");
    context->CmdSetStencilOp = (PFN_vkCmdSetStencilOp) load(userptr, "vkCmdSetStencilOp");
    context->CmdSetStencilTestEnable = (PFN_vkCmdSetStencilTestEnable) load(userptr, "vkCmdSetStencilTestEnable");
    context->CmdSetViewportWithCount = (PFN_vkCmdSetViewportWithCount) load(userptr, "vkCmdSetViewportWithCount");
    context->CmdWaitEvents2 = (PFN_vkCmdWaitEvents2) load(userptr, "vkCmdWaitEvents2");
    context->CmdWriteTimestamp2 = (PFN_vkCmdWriteTimestamp2) load(userptr, "vkCmdWriteTimestamp2");
    context->CreatePrivateDataSlot = (PFN_vkCreatePrivateDataSlot) load(userptr, "vkCreatePrivateDataSlot");
    context->DestroyPrivateDataSlot = (PFN_vkDestroyPrivateDataSlot) load(userptr, "vkDestroyPrivateDataSlot");
    context->GetDeviceBufferMemoryRequirements = (PFN_vkGetDeviceBufferMemoryRequirements) load(userptr, "vkGetDeviceBufferMemoryRequirements");
    context->GetDeviceImageMemoryRequirements = (PFN_vkGetDeviceImageMemoryRequirements) load(userptr, "vkGetDeviceImageMemoryRequirements");
    context->GetDeviceImageSparseMemoryRequirements = (PFN_vkGetDeviceImageSparseMemoryRequirements) load(userptr, "vkGetDeviceImageSparseMemoryRequirements");
    context->GetPhysicalDeviceToolProperties = (PFN_vkGetPhysicalDeviceToolProperties) load(userptr, "vkGetPhysicalDeviceToolProperties");
    context->GetPrivateData = (PFN_vkGetPrivateData) load(userptr, "vkGetPrivateData");
    context->QueueSubmit2 = (PFN_vkQueueSubmit2) load(userptr, "vkQueueSubmit2");
    context->SetPrivateData = (PFN_vkSetPrivateData) load(userptr, "vkSetPrivateData");
}
static void glad_vk_load_VK_EXT_debug_utils(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->EXT_debug_utils) return;
    context->CmdBeginDebugUtilsLabelEXT = (PFN_vkCmdBeginDebugUtilsLabelEXT) load(userptr, "vkCmdBeginDebugUtilsLabelEXT");
    context->CmdEndDebugUtilsLabelEXT = (PFN_vkCmdEndDebugUtilsLabelEXT) load(userptr, "vkCmdEndDebugUtilsLabelEXT");
    context->CmdInsertDebugUtilsLabelEXT = (PFN_vkCmdInsertDebugUtilsLabelEXT) load(userptr, "vkCmdInsertDebugUtilsLabelEXT");
    context->CreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT) load(userptr, "vkCreateDebugUtilsMessengerEXT");
    context->DestroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT) load(userptr, "vkDestroyDebugUtilsMessengerEXT");
    context->QueueBeginDebugUtilsLabelEXT = (PFN_vkQueueBeginDebugUtilsLabelEXT) load(userptr, "vkQueueBeginDebugUtilsLabelEXT");
    context->QueueEndDebugUtilsLabelEXT = (PFN_vkQueueEndDebugUtilsLabelEXT) load(userptr, "vkQueueEndDebugUtilsLabelEXT");
    context->QueueInsertDebugUtilsLabelEXT = (PFN_vkQueueInsertDebugUtilsLabelEXT) load(userptr, "vkQueueInsertDebugUtilsLabelEXT");
    context->SetDebugUtilsObjectNameEXT = (PFN_vkSetDebugUtilsObjectNameEXT) load(userptr, "vkSetDebugUtilsObjectNameEXT");
    context->SetDebugUtilsObjectTagEXT = (PFN_vkSetDebugUtilsObjectTagEXT) load(userptr, "vkSetDebugUtilsObjectTagEXT");
    context->SubmitDebugUtilsMessageEXT = (PFN_vkSubmitDebugUtilsMessageEXT) load(userptr, "vkSubmitDebugUtilsMessageEXT");
}
static void glad_vk_load_VK_EXT_extended_dynamic_state(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->EXT_extended_dynamic_state) return;
    context->CmdBindVertexBuffers2EXT = (PFN_vkCmdBindVertexBuffers2EXT) load(userptr, "vkCmdBindVertexBuffers2EXT");
    context->CmdSetCullModeEXT = (PFN_vkCmdSetCullModeEXT) load(userptr, "vkCmdSetCullModeEXT");
    context->CmdSetDepthBoundsTestEnableEXT = (PFN_vkCmdSetDepthBoundsTestEnableEXT) load(userptr, "vkCmdSetDepthBoundsTestEnableEXT");
    context->CmdSetDepthCompareOpEXT = (PFN_vkCmdSetDepthCompareOpEXT) load(userptr, "vkCmdSetDepthCompareOpEXT");
    context->CmdSetDepthTestEnableEXT = (PFN_vkCmdSetDepthTestEnableEXT) load(userptr, "vkCmdSetDepthTestEnableEXT");
    context->CmdSetDepthWriteEnableEXT = (PFN_vkCmdSetDepthWriteEnableEXT) load(userptr, "vkCmdSetDepthWriteEnableEXT");
    context->CmdSetFrontFaceEXT = (PFN_vkCmdSetFrontFaceEXT) load(userptr, "vkCmdSetFrontFaceEXT");
    context->CmdSetPrimitiveTopologyEXT = (PFN_vkCmdSetPrimitiveTopologyEXT) load(userptr, "vkCmdSetPrimitiveTopologyEXT");
    context->CmdSetScissorWithCountEXT = (PFN_vkCmdSetScissorWithCountEXT) load(userptr, "vkCmdSetScissorWithCountEXT");
    context->CmdSetStencilOpEXT = (PFN_vkCmdSetStencilOpEXT) load(userptr, "vkCmdSetStencilOpEXT");
    context->CmdSetStencilTestEnableEXT = (PFN_vkCmdSetStencilTestEnableEXT) load(userptr, "vkCmdSetStencilTestEnableEXT");
    context->CmdSetViewportWithCountEXT = (PFN_vkCmdSetViewportWithCountEXT) load(userptr, "vkCmdSetViewportWithCountEXT");
}
static void glad_vk_load_VK_EXT_extended_dynamic_state2(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->EXT_extended_dynamic_state2) return;
    context->CmdSetDepthBiasEnableEXT = (PFN_vkCmdSetDepthBiasEnableEXT) load(userptr, "vkCmdSetDepthBiasEnableEXT");
    context->CmdSetLogicOpEXT = (PFN_vkCmdSetLogicOpEXT) load(userptr, "vkCmdSetLogicOpEXT");
    context->CmdSetPatchControlPointsEXT = (PFN_vkCmdSetPatchControlPointsEXT) load(userptr, "vkCmdSetPatchControlPointsEXT");
    context->CmdSetPrimitiveRestartEnableEXT = (PFN_vkCmdSetPrimitiveRestartEnableEXT) load(userptr, "vkCmdSetPrimitiveRestartEnableEXT");
    context->CmdSetRasterizerDiscardEnableEXT = (PFN_vkCmdSetRasterizerDiscardEnableEXT) load(userptr, "vkCmdSetRasterizerDiscardEnableEXT");
}
static void glad_vk_load_VK_EXT_extended_dynamic_state3(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->EXT_extended_dynamic_state3) return;
    context->CmdSetAlphaToCoverageEnableEXT = (PFN_vkCmdSetAlphaToCoverageEnableEXT) load(userptr, "vkCmdSetAlphaToCoverageEnableEXT");
    context->CmdSetAlphaToOneEnableEXT = (PFN_vkCmdSetAlphaToOneEnableEXT) load(userptr, "vkCmdSetAlphaToOneEnableEXT");
    context->CmdSetColorBlendAdvancedEXT = (PFN_vkCmdSetColorBlendAdvancedEXT) load(userptr, "vkCmdSetColorBlendAdvancedEXT");
    context->CmdSetColorBlendEnableEXT = (PFN_vkCmdSetColorBlendEnableEXT) load(userptr, "vkCmdSetColorBlendEnableEXT");
    context->CmdSetColorBlendEquationEXT = (PFN_vkCmdSetColorBlendEquationEXT) load(userptr, "vkCmdSetColorBlendEquationEXT");
    context->CmdSetColorWriteMaskEXT = (PFN_vkCmdSetColorWriteMaskEXT) load(userptr, "vkCmdSetColorWriteMaskEXT");
    context->CmdSetConservativeRasterizationModeEXT = (PFN_vkCmdSetConservativeRasterizationModeEXT) load(userptr, "vkCmdSetConservativeRasterizationModeEXT");
    context->CmdSetCoverageModulationModeNV = (PFN_vkCmdSetCoverageModulationModeNV) load(userptr, "vkCmdSetCoverageModulationModeNV");
    context->CmdSetCoverageModulationTableEnableNV = (PFN_vkCmdSetCoverageModulationTableEnableNV) load(userptr, "vkCmdSetCoverageModulationTableEnableNV");
    context->CmdSetCoverageModulationTableNV = (PFN_vkCmdSetCoverageModulationTableNV) load(userptr, "vkCmdSetCoverageModulationTableNV");
    context->CmdSetCoverageReductionModeNV = (PFN_vkCmdSetCoverageReductionModeNV) load(userptr, "vkCmdSetCoverageReductionModeNV");
    context->CmdSetCoverageToColorEnableNV = (PFN_vkCmdSetCoverageToColorEnableNV) load(userptr, "vkCmdSetCoverageToColorEnableNV");
    context->CmdSetCoverageToColorLocationNV = (PFN_vkCmdSetCoverageToColorLocationNV) load(userptr, "vkCmdSetCoverageToColorLocationNV");
    context->CmdSetDepthClampEnableEXT = (PFN_vkCmdSetDepthClampEnableEXT) load(userptr, "vkCmdSetDepthClampEnableEXT");
    context->CmdSetDepthClipEnableEXT = (PFN_vkCmdSetDepthClipEnableEXT) load(userptr, "vkCmdSetDepthClipEnableEXT");
    context->CmdSetDepthClipNegativeOneToOneEXT = (PFN_vkCmdSetDepthClipNegativeOneToOneEXT) load(userptr, "vkCmdSetDepthClipNegativeOneToOneEXT");
    context->CmdSetExtraPrimitiveOverestimationSizeEXT = (PFN_vkCmdSetExtraPrimitiveOverestimationSizeEXT) load(userptr, "vkCmdSetExtraPrimitiveOverestimationSizeEXT");
    context->CmdSetLineRasterizationModeEXT = (PFN_vkCmdSetLineRasterizationModeEXT) load(userptr, "vkCmdSetLineRasterizationModeEXT");
    context->CmdSetLineStippleEnableEXT = (PFN_vkCmdSetLineStippleEnableEXT) load(userptr, "vkCmdSetLineStippleEnableEXT");
    context->CmdSetLogicOpEnableEXT = (PFN_vkCmdSetLogicOpEnableEXT) load(userptr, "vkCmdSetLogicOpEnableEXT");
    context->CmdSetPolygonModeEXT = (PFN_vkCmdSetPolygonModeEXT) load(userptr, "vkCmdSetPolygonModeEXT");
    context->CmdSetProvokingVertexModeEXT = (PFN_vkCmdSetProvokingVertexModeEXT) load(userptr, "vkCmdSetProvokingVertexModeEXT");
    context->CmdSetRasterizationSamplesEXT = (PFN_vkCmdSetRasterizationSamplesEXT) load(userptr, "vkCmdSetRasterizationSamplesEXT");
    context->CmdSetRasterizationStreamEXT = (PFN_vkCmdSetRasterizationStreamEXT) load(userptr, "vkCmdSetRasterizationStreamEXT");
    context->CmdSetRepresentativeFragmentTestEnableNV = (PFN_vkCmdSetRepresentativeFragmentTestEnableNV) load(userptr, "vkCmdSetRepresentativeFragmentTestEnableNV");
    context->CmdSetSampleLocationsEnableEXT = (PFN_vkCmdSetSampleLocationsEnableEXT) load(userptr, "vkCmdSetSampleLocationsEnableEXT");
    context->CmdSetSampleMaskEXT = (PFN_vkCmdSetSampleMaskEXT) load(userptr, "vkCmdSetSampleMaskEXT");
    context->CmdSetShadingRateImageEnableNV = (PFN_vkCmdSetShadingRateImageEnableNV) load(userptr, "vkCmdSetShadingRateImageEnableNV");
    context->CmdSetTessellationDomainOriginEXT = (PFN_vkCmdSetTessellationDomainOriginEXT) load(userptr, "vkCmdSetTessellationDomainOriginEXT");
    context->CmdSetViewportSwizzleNV = (PFN_vkCmdSetViewportSwizzleNV) load(userptr, "vkCmdSetViewportSwizzleNV");
    context->CmdSetViewportWScalingEnableNV = (PFN_vkCmdSetViewportWScalingEnableNV) load(userptr, "vkCmdSetViewportWScalingEnableNV");
}
#if defined(VK_USE_PLATFORM_WIN32_KHR)
static void glad_vk_load_VK_EXT_full_screen_exclusive(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->EXT_full_screen_exclusive) return;
    context->AcquireFullScreenExclusiveModeEXT = (PFN_vkAcquireFullScreenExclusiveModeEXT) load(userptr, "vkAcquireFullScreenExclusiveModeEXT");
    context->GetDeviceGroupSurfacePresentModes2EXT = (PFN_vkGetDeviceGroupSurfacePresentModes2EXT) load(userptr, "vkGetDeviceGroupSurfacePresentModes2EXT");
    context->GetPhysicalDeviceSurfacePresentModes2EXT = (PFN_vkGetPhysicalDeviceSurfacePresentModes2EXT) load(userptr, "vkGetPhysicalDeviceSurfacePresentModes2EXT");
    context->ReleaseFullScreenExclusiveModeEXT = (PFN_vkReleaseFullScreenExclusiveModeEXT) load(userptr, "vkReleaseFullScreenExclusiveModeEXT");
}

#endif
static void glad_vk_load_VK_EXT_mesh_shader(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->EXT_mesh_shader) return;
    context->CmdDrawMeshTasksEXT = (PFN_vkCmdDrawMeshTasksEXT) load(userptr, "vkCmdDrawMeshTasksEXT");
    context->CmdDrawMeshTasksIndirectCountEXT = (PFN_vkCmdDrawMeshTasksIndirectCountEXT) load(userptr, "vkCmdDrawMeshTasksIndirectCountEXT");
    context->CmdDrawMeshTasksIndirectEXT = (PFN_vkCmdDrawMeshTasksIndirectEXT) load(userptr, "vkCmdDrawMeshTasksIndirectEXT");
}
static void glad_vk_load_VK_EXT_multi_draw(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->EXT_multi_draw) return;
    context->CmdDrawMultiEXT = (PFN_vkCmdDrawMultiEXT) load(userptr, "vkCmdDrawMultiEXT");
    context->CmdDrawMultiIndexedEXT = (PFN_vkCmdDrawMultiIndexedEXT) load(userptr, "vkCmdDrawMultiIndexedEXT");
}
static void glad_vk_load_VK_EXT_private_data(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->EXT_private_data) return;
    context->CreatePrivateDataSlotEXT = (PFN_vkCreatePrivateDataSlotEXT) load(userptr, "vkCreatePrivateDataSlotEXT");
    context->DestroyPrivateDataSlotEXT = (PFN_vkDestroyPrivateDataSlotEXT) load(userptr, "vkDestroyPrivateDataSlotEXT");
    context->GetPrivateDataEXT = (PFN_vkGetPrivateDataEXT) load(userptr, "vkGetPrivateDataEXT");
    context->SetPrivateDataEXT = (PFN_vkSetPrivateDataEXT) load(userptr, "vkSetPrivateDataEXT");
}
static void glad_vk_load_VK_EXT_tooling_info(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->EXT_tooling_info) return;
    context->GetPhysicalDeviceToolPropertiesEXT = (PFN_vkGetPhysicalDeviceToolPropertiesEXT) load(userptr, "vkGetPhysicalDeviceToolPropertiesEXT");
}
static void glad_vk_load_VK_EXT_transform_feedback(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->EXT_transform_feedback) return;
    context->CmdBeginQueryIndexedEXT = (PFN_vkCmdBeginQueryIndexedEXT) load(userptr, "vkCmdBeginQueryIndexedEXT");
    context->CmdBeginTransformFeedbackEXT = (PFN_vkCmdBeginTransformFeedbackEXT) load(userptr, "vkCmdBeginTransformFeedbackEXT");
    context->CmdBindTransformFeedbackBuffersEXT = (PFN_vkCmdBindTransformFeedbackBuffersEXT) load(userptr, "vkCmdBindTransformFeedbackBuffersEXT");
    context->CmdDrawIndirectByteCountEXT = (PFN_vkCmdDrawIndirectByteCountEXT) load(userptr, "vkCmdDrawIndirectByteCountEXT");
    context->CmdEndQueryIndexedEXT = (PFN_vkCmdEndQueryIndexedEXT) load(userptr, "vkCmdEndQueryIndexedEXT");
    context->CmdEndTransformFeedbackEXT = (PFN_vkCmdEndTransformFeedbackEXT) load(userptr, "vkCmdEndTransformFeedbackEXT");
}
static void glad_vk_load_VK_KHR_acceleration_structure(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_acceleration_structure) return;
    context->BuildAccelerationStructuresKHR = (PFN_vkBuildAccelerationStructuresKHR) load(userptr, "vkBuildAccelerationStructuresKHR");
    context->CmdBuildAccelerationStructuresIndirectKHR = (PFN_vkCmdBuildAccelerationStructuresIndirectKHR) load(userptr, "vkCmdBuildAccelerationStructuresIndirectKHR");
    context->CmdBuildAccelerationStructuresKHR = (PFN_vkCmdBuildAccelerationStructuresKHR) load(userptr, "vkCmdBuildAccelerationStructuresKHR");
    context->CmdCopyAccelerationStructureKHR = (PFN_vkCmdCopyAccelerationStructureKHR) load(userptr, "vkCmdCopyAccelerationStructureKHR");
    context->CmdCopyAccelerationStructureToMemoryKHR = (PFN_vkCmdCopyAccelerationStructureToMemoryKHR) load(userptr, "vkCmdCopyAccelerationStructureToMemoryKHR");
    context->CmdCopyMemoryToAccelerationStructureKHR = (PFN_vkCmdCopyMemoryToAccelerationStructureKHR) load(userptr, "vkCmdCopyMemoryToAccelerationStructureKHR");
    context->CmdWriteAccelerationStructuresPropertiesKHR = (PFN_vkCmdWriteAccelerationStructuresPropertiesKHR) load(userptr, "vkCmdWriteAccelerationStructuresPropertiesKHR");
    context->CopyAccelerationStructureKHR = (PFN_vkCopyAccelerationStructureKHR) load(userptr, "vkCopyAccelerationStructureKHR");
    context->CopyAccelerationStructureToMemoryKHR = (PFN_vkCopyAccelerationStructureToMemoryKHR) load(userptr, "vkCopyAccelerationStructureToMemoryKHR");
    context->CopyMemoryToAccelerationStructureKHR = (PFN_vkCopyMemoryToAccelerationStructureKHR) load(userptr, "vkCopyMemoryToAccelerationStructureKHR");
    context->CreateAccelerationStructureKHR = (PFN_vkCreateAccelerationStructureKHR) load(userptr, "vkCreateAccelerationStructureKHR");
    context->DestroyAccelerationStructureKHR = (PFN_vkDestroyAccelerationStructureKHR) load(userptr, "vkDestroyAccelerationStructureKHR");
    context->GetAccelerationStructureBuildSizesKHR = (PFN_vkGetAccelerationStructureBuildSizesKHR) load(userptr, "vkGetAccelerationStructureBuildSizesKHR");
    context->GetAccelerationStructureDeviceAddressKHR = (PFN_vkGetAccelerationStructureDeviceAddressKHR) load(userptr, "vkGetAccelerationStructureDeviceAddressKHR");
    context->GetDeviceAccelerationStructureCompatibilityKHR = (PFN_vkGetDeviceAccelerationStructureCompatibilityKHR) load(userptr, "vkGetDeviceAccelerationStructureCompatibilityKHR");
    context->WriteAccelerationStructuresPropertiesKHR = (PFN_vkWriteAccelerationStructuresPropertiesKHR) load(userptr, "vkWriteAccelerationStructuresPropertiesKHR");
}
static void glad_vk_load_VK_KHR_bind_memory2(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_bind_memory2) return;
    context->BindBufferMemory2KHR = (PFN_vkBindBufferMemory2KHR) load(userptr, "vkBindBufferMemory2KHR");
    context->BindImageMemory2KHR = (PFN_vkBindImageMemory2KHR) load(userptr, "vkBindImageMemory2KHR");
}
static void glad_vk_load_VK_KHR_buffer_device_address(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_buffer_device_address) return;
    context->GetBufferDeviceAddressKHR = (PFN_vkGetBufferDeviceAddressKHR) load(userptr, "vkGetBufferDeviceAddressKHR");
    context->GetBufferOpaqueCaptureAddressKHR = (PFN_vkGetBufferOpaqueCaptureAddressKHR) load(userptr, "vkGetBufferOpaqueCaptureAddressKHR");
    context->GetDeviceMemoryOpaqueCaptureAddressKHR = (PFN_vkGetDeviceMemoryOpaqueCaptureAddressKHR) load(userptr, "vkGetDeviceMemoryOpaqueCaptureAddressKHR");
}
static void glad_vk_load_VK_KHR_copy_commands2(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_copy_commands2) return;
    context->CmdBlitImage2KHR = (PFN_vkCmdBlitImage2KHR) load(userptr, "vkCmdBlitImage2KHR");
    context->CmdCopyBuffer2KHR = (PFN_vkCmdCopyBuffer2KHR) load(userptr, "vkCmdCopyBuffer2KHR");
    context->CmdCopyBufferToImage2KHR = (PFN_vkCmdCopyBufferToImage2KHR) load(userptr, "vkCmdCopyBufferToImage2KHR");
    context->CmdCopyImage2KHR = (PFN_vkCmdCopyImage2KHR) load(userptr, "vkCmdCopyImage2KHR");
    context->CmdCopyImageToBuffer2KHR = (PFN_vkCmdCopyImageToBuffer2KHR) load(userptr, "vkCmdCopyImageToBuffer2KHR");
    context->CmdResolveImage2KHR = (PFN_vkCmdResolveImage2KHR) load(userptr, "vkCmdResolveImage2KHR");
}
static void glad_vk_load_VK_KHR_create_renderpass2(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_create_renderpass2) return;
    context->CmdBeginRenderPass2KHR = (PFN_vkCmdBeginRenderPass2KHR) load(userptr, "vkCmdBeginRenderPass2KHR");
    context->CmdEndRenderPass2KHR = (PFN_vkCmdEndRenderPass2KHR) load(userptr, "vkCmdEndRenderPass2KHR");
    context->CmdNextSubpass2KHR = (PFN_vkCmdNextSubpass2KHR) load(userptr, "vkCmdNextSubpass2KHR");
    context->CreateRenderPass2KHR = (PFN_vkCreateRenderPass2KHR) load(userptr, "vkCreateRenderPass2KHR");
}
static void glad_vk_load_VK_KHR_descriptor_update_template(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_descriptor_update_template) return;
    context->CmdPushDescriptorSetWithTemplateKHR = (PFN_vkCmdPushDescriptorSetWithTemplateKHR) load(userptr, "vkCmdPushDescriptorSetWithTemplateKHR");
    context->CreateDescriptorUpdateTemplateKHR = (PFN_vkCreateDescriptorUpdateTemplateKHR) load(userptr, "vkCreateDescriptorUpdateTemplateKHR");
    context->DestroyDescriptorUpdateTemplateKHR = (PFN_vkDestroyDescriptorUpdateTemplateKHR) load(userptr, "vkDestroyDescriptorUpdateTemplateKHR");
    context->UpdateDescriptorSetWithTemplateKHR = (PFN_vkUpdateDescriptorSetWithTemplateKHR) load(userptr, "vkUpdateDescriptorSetWithTemplateKHR");
}
static void glad_vk_load_VK_KHR_device_group(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_device_group) return;
    context->AcquireNextImage2KHR = (PFN_vkAcquireNextImage2KHR) load(userptr, "vkAcquireNextImage2KHR");
    context->CmdDispatchBaseKHR = (PFN_vkCmdDispatchBaseKHR) load(userptr, "vkCmdDispatchBaseKHR");
    context->CmdSetDeviceMaskKHR = (PFN_vkCmdSetDeviceMaskKHR) load(userptr, "vkCmdSetDeviceMaskKHR");
    context->GetDeviceGroupPeerMemoryFeaturesKHR = (PFN_vkGetDeviceGroupPeerMemoryFeaturesKHR) load(userptr, "vkGetDeviceGroupPeerMemoryFeaturesKHR");
    context->GetDeviceGroupPresentCapabilitiesKHR = (PFN_vkGetDeviceGroupPresentCapabilitiesKHR) load(userptr, "vkGetDeviceGroupPresentCapabilitiesKHR");
    context->GetDeviceGroupSurfacePresentModesKHR = (PFN_vkGetDeviceGroupSurfacePresentModesKHR) load(userptr, "vkGetDeviceGroupSurfacePresentModesKHR");
    context->GetPhysicalDevicePresentRectanglesKHR = (PFN_vkGetPhysicalDevicePresentRectanglesKHR) load(userptr, "vkGetPhysicalDevicePresentRectanglesKHR");
}
static void glad_vk_load_VK_KHR_display_swapchain(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_display_swapchain) return;
    context->CreateSharedSwapchainsKHR = (PFN_vkCreateSharedSwapchainsKHR) load(userptr, "vkCreateSharedSwapchainsKHR");
}
static void glad_vk_load_VK_KHR_draw_indirect_count(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_draw_indirect_count) return;
    context->CmdDrawIndexedIndirectCountKHR = (PFN_vkCmdDrawIndexedIndirectCountKHR) load(userptr, "vkCmdDrawIndexedIndirectCountKHR");
    context->CmdDrawIndirectCountKHR = (PFN_vkCmdDrawIndirectCountKHR) load(userptr, "vkCmdDrawIndirectCountKHR");
}
static void glad_vk_load_VK_KHR_dynamic_rendering(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_dynamic_rendering) return;
    context->CmdBeginRenderingKHR = (PFN_vkCmdBeginRenderingKHR) load(userptr, "vkCmdBeginRenderingKHR");
    context->CmdEndRenderingKHR = (PFN_vkCmdEndRenderingKHR) load(userptr, "vkCmdEndRenderingKHR");
}
static void glad_vk_load_VK_KHR_external_memory_capabilities(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_external_memory_capabilities) return;
    context->GetPhysicalDeviceExternalBufferPropertiesKHR = (PFN_vkGetPhysicalDeviceExternalBufferPropertiesKHR) load(userptr, "vkGetPhysicalDeviceExternalBufferPropertiesKHR");
}
#if defined(VK_USE_PLATFORM_WIN32_KHR)
static void glad_vk_load_VK_KHR_external_memory_win32(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_external_memory_win32) return;
    context->GetMemoryWin32HandleKHR = (PFN_vkGetMemoryWin32HandleKHR) load(userptr, "vkGetMemoryWin32HandleKHR");
    context->GetMemoryWin32HandlePropertiesKHR = (PFN_vkGetMemoryWin32HandlePropertiesKHR) load(userptr, "vkGetMemoryWin32HandlePropertiesKHR");
}

#endif
#if defined(VK_USE_PLATFORM_WIN32_KHR)
static void glad_vk_load_VK_KHR_external_semaphore_win32(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_external_semaphore_win32) return;
    context->GetSemaphoreWin32HandleKHR = (PFN_vkGetSemaphoreWin32HandleKHR) load(userptr, "vkGetSemaphoreWin32HandleKHR");
    context->ImportSemaphoreWin32HandleKHR = (PFN_vkImportSemaphoreWin32HandleKHR) load(userptr, "vkImportSemaphoreWin32HandleKHR");
}

#endif
static void glad_vk_load_VK_KHR_get_memory_requirements2(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_get_memory_requirements2) return;
    context->GetBufferMemoryRequirements2KHR = (PFN_vkGetBufferMemoryRequirements2KHR) load(userptr, "vkGetBufferMemoryRequirements2KHR");
    context->GetImageMemoryRequirements2KHR = (PFN_vkGetImageMemoryRequirements2KHR) load(userptr, "vkGetImageMemoryRequirements2KHR");
    context->GetImageSparseMemoryRequirements2KHR = (PFN_vkGetImageSparseMemoryRequirements2KHR) load(userptr, "vkGetImageSparseMemoryRequirements2KHR");
}
static void glad_vk_load_VK_KHR_get_physical_device_properties2(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_get_physical_device_properties2) return;
    context->GetPhysicalDeviceFeatures2KHR = (PFN_vkGetPhysicalDeviceFeatures2KHR) load(userptr, "vkGetPhysicalDeviceFeatures2KHR");
    context->GetPhysicalDeviceFormatProperties2KHR = (PFN_vkGetPhysicalDeviceFormatProperties2KHR) load(userptr, "vkGetPhysicalDeviceFormatProperties2KHR");
    context->GetPhysicalDeviceImageFormatProperties2KHR = (PFN_vkGetPhysicalDeviceImageFormatProperties2KHR) load(userptr, "vkGetPhysicalDeviceImageFormatProperties2KHR");
    context->GetPhysicalDeviceMemoryProperties2KHR = (PFN_vkGetPhysicalDeviceMemoryProperties2KHR) load(userptr, "vkGetPhysicalDeviceMemoryProperties2KHR");
    context->GetPhysicalDeviceProperties2KHR = (PFN_vkGetPhysicalDeviceProperties2KHR) load(userptr, "vkGetPhysicalDeviceProperties2KHR");
    context->GetPhysicalDeviceQueueFamilyProperties2KHR = (PFN_vkGetPhysicalDeviceQueueFamilyProperties2KHR) load(userptr, "vkGetPhysicalDeviceQueueFamilyProperties2KHR");
    context->GetPhysicalDeviceSparseImageFormatProperties2KHR = (PFN_vkGetPhysicalDeviceSparseImageFormatProperties2KHR) load(userptr, "vkGetPhysicalDeviceSparseImageFormatProperties2KHR");
}
static void glad_vk_load_VK_KHR_maintenance4(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_maintenance4) return;
    context->GetDeviceBufferMemoryRequirementsKHR = (PFN_vkGetDeviceBufferMemoryRequirementsKHR) load(userptr, "vkGetDeviceBufferMemoryRequirementsKHR");
    context->GetDeviceImageMemoryRequirementsKHR = (PFN_vkGetDeviceImageMemoryRequirementsKHR) load(userptr, "vkGetDeviceImageMemoryRequirementsKHR");
    context->GetDeviceImageSparseMemoryRequirementsKHR = (PFN_vkGetDeviceImageSparseMemoryRequirementsKHR) load(userptr, "vkGetDeviceImageSparseMemoryRequirementsKHR");
}
static void glad_vk_load_VK_KHR_maintenance5(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_maintenance5) return;
    context->CmdBindIndexBuffer2KHR = (PFN_vkCmdBindIndexBuffer2KHR) load(userptr, "vkCmdBindIndexBuffer2KHR");
    context->GetDeviceImageSubresourceLayoutKHR = (PFN_vkGetDeviceImageSubresourceLayoutKHR) load(userptr, "vkGetDeviceImageSubresourceLayoutKHR");
    context->GetImageSubresourceLayout2KHR = (PFN_vkGetImageSubresourceLayout2KHR) load(userptr, "vkGetImageSubresourceLayout2KHR");
    context->GetRenderingAreaGranularityKHR = (PFN_vkGetRenderingAreaGranularityKHR) load(userptr, "vkGetRenderingAreaGranularityKHR");
}
static void glad_vk_load_VK_KHR_push_descriptor(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_push_descriptor) return;
    context->CmdPushDescriptorSetKHR = (PFN_vkCmdPushDescriptorSetKHR) load(userptr, "vkCmdPushDescriptorSetKHR");
    context->CmdPushDescriptorSetWithTemplateKHR = (PFN_vkCmdPushDescriptorSetWithTemplateKHR) load(userptr, "vkCmdPushDescriptorSetWithTemplateKHR");
}
static void glad_vk_load_VK_KHR_ray_tracing_maintenance1(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_ray_tracing_maintenance1) return;
    context->CmdTraceRaysIndirect2KHR = (PFN_vkCmdTraceRaysIndirect2KHR) load(userptr, "vkCmdTraceRaysIndirect2KHR");
}
static void glad_vk_load_VK_KHR_ray_tracing_pipeline(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_ray_tracing_pipeline) return;
    context->CmdSetRayTracingPipelineStackSizeKHR = (PFN_vkCmdSetRayTracingPipelineStackSizeKHR) load(userptr, "vkCmdSetRayTracingPipelineStackSizeKHR");
    context->CmdTraceRaysIndirectKHR = (PFN_vkCmdTraceRaysIndirectKHR) load(userptr, "vkCmdTraceRaysIndirectKHR");
    context->CmdTraceRaysKHR = (PFN_vkCmdTraceRaysKHR) load(userptr, "vkCmdTraceRaysKHR");
    context->CreateRayTracingPipelinesKHR = (PFN_vkCreateRayTracingPipelinesKHR) load(userptr, "vkCreateRayTracingPipelinesKHR");
    context->GetRayTracingCaptureReplayShaderGroupHandlesKHR = (PFN_vkGetRayTracingCaptureReplayShaderGroupHandlesKHR) load(userptr, "vkGetRayTracingCaptureReplayShaderGroupHandlesKHR");
    context->GetRayTracingShaderGroupHandlesKHR = (PFN_vkGetRayTracingShaderGroupHandlesKHR) load(userptr, "vkGetRayTracingShaderGroupHandlesKHR");
    context->GetRayTracingShaderGroupStackSizeKHR = (PFN_vkGetRayTracingShaderGroupStackSizeKHR) load(userptr, "vkGetRayTracingShaderGroupStackSizeKHR");
}
static void glad_vk_load_VK_KHR_surface(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_surface) return;
    context->DestroySurfaceKHR = (PFN_vkDestroySurfaceKHR) load(userptr, "vkDestroySurfaceKHR");
    context->GetPhysicalDeviceSurfaceCapabilitiesKHR = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR) load(userptr, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    context->GetPhysicalDeviceSurfaceFormatsKHR = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR) load(userptr, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    context->GetPhysicalDeviceSurfacePresentModesKHR = (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR) load(userptr, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    context->GetPhysicalDeviceSurfaceSupportKHR = (PFN_vkGetPhysicalDeviceSurfaceSupportKHR) load(userptr, "vkGetPhysicalDeviceSurfaceSupportKHR");
}
static void glad_vk_load_VK_KHR_swapchain(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_swapchain) return;
    context->AcquireNextImage2KHR = (PFN_vkAcquireNextImage2KHR) load(userptr, "vkAcquireNextImage2KHR");
    context->AcquireNextImageKHR = (PFN_vkAcquireNextImageKHR) load(userptr, "vkAcquireNextImageKHR");
    context->CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR) load(userptr, "vkCreateSwapchainKHR");
    context->DestroySwapchainKHR = (PFN_vkDestroySwapchainKHR) load(userptr, "vkDestroySwapchainKHR");
    context->GetDeviceGroupPresentCapabilitiesKHR = (PFN_vkGetDeviceGroupPresentCapabilitiesKHR) load(userptr, "vkGetDeviceGroupPresentCapabilitiesKHR");
    context->GetDeviceGroupSurfacePresentModesKHR = (PFN_vkGetDeviceGroupSurfacePresentModesKHR) load(userptr, "vkGetDeviceGroupSurfacePresentModesKHR");
    context->GetPhysicalDevicePresentRectanglesKHR = (PFN_vkGetPhysicalDevicePresentRectanglesKHR) load(userptr, "vkGetPhysicalDevicePresentRectanglesKHR");
    context->GetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR) load(userptr, "vkGetSwapchainImagesKHR");
    context->QueuePresentKHR = (PFN_vkQueuePresentKHR) load(userptr, "vkQueuePresentKHR");
}
static void glad_vk_load_VK_KHR_synchronization2(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_synchronization2) return;
    context->CmdPipelineBarrier2KHR = (PFN_vkCmdPipelineBarrier2KHR) load(userptr, "vkCmdPipelineBarrier2KHR");
    context->CmdResetEvent2KHR = (PFN_vkCmdResetEvent2KHR) load(userptr, "vkCmdResetEvent2KHR");
    context->CmdSetEvent2KHR = (PFN_vkCmdSetEvent2KHR) load(userptr, "vkCmdSetEvent2KHR");
    context->CmdWaitEvents2KHR = (PFN_vkCmdWaitEvents2KHR) load(userptr, "vkCmdWaitEvents2KHR");
    context->CmdWriteBufferMarker2AMD = (PFN_vkCmdWriteBufferMarker2AMD) load(userptr, "vkCmdWriteBufferMarker2AMD");
    context->CmdWriteTimestamp2KHR = (PFN_vkCmdWriteTimestamp2KHR) load(userptr, "vkCmdWriteTimestamp2KHR");
    context->GetQueueCheckpointData2NV = (PFN_vkGetQueueCheckpointData2NV) load(userptr, "vkGetQueueCheckpointData2NV");
    context->QueueSubmit2KHR = (PFN_vkQueueSubmit2KHR) load(userptr, "vkQueueSubmit2KHR");
}
static void glad_vk_load_VK_KHR_timeline_semaphore(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_timeline_semaphore) return;
    context->GetSemaphoreCounterValueKHR = (PFN_vkGetSemaphoreCounterValueKHR) load(userptr, "vkGetSemaphoreCounterValueKHR");
    context->SignalSemaphoreKHR = (PFN_vkSignalSemaphoreKHR) load(userptr, "vkSignalSemaphoreKHR");
    context->WaitSemaphoresKHR = (PFN_vkWaitSemaphoresKHR) load(userptr, "vkWaitSemaphoresKHR");
}
#if defined(VK_USE_PLATFORM_WIN32_KHR)
static void glad_vk_load_VK_KHR_win32_surface(GladVulkanContext *context, GLADuserptrloadfunc load, void* userptr) {
    if(!context->KHR_win32_surface) return;
    context->CreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR) load(userptr, "vkCreateWin32SurfaceKHR");
    context->GetPhysicalDeviceWin32PresentationSupportKHR = (PFN_vkGetPhysicalDeviceWin32PresentationSupportKHR) load(userptr, "vkGetPhysicalDeviceWin32PresentationSupportKHR");
}

#endif



static int glad_vk_get_extensions(GladVulkanContext *context, VkPhysicalDevice physical_device, uint32_t *out_extension_count, char ***out_extensions) {
    uint32_t i;
    uint32_t instance_extension_count = 0;
    uint32_t device_extension_count = 0;
    uint32_t max_extension_count = 0;
    uint32_t total_extension_count = 0;
    char **extensions = NULL;
    VkExtensionProperties *ext_properties = NULL;
    VkResult result;

    if (context->EnumerateInstanceExtensionProperties == NULL || (physical_device != NULL && context->EnumerateDeviceExtensionProperties == NULL)) {
        return 0;
    }

    result = context->EnumerateInstanceExtensionProperties(NULL, &instance_extension_count, NULL);
    if (result != VK_SUCCESS) {
        return 0;
    }

    if (physical_device != NULL) {
        result = context->EnumerateDeviceExtensionProperties(physical_device, NULL, &device_extension_count, NULL);
        if (result != VK_SUCCESS) {
            return 0;
        }
    }

    total_extension_count = instance_extension_count + device_extension_count;
    if (total_extension_count <= 0) {
        return 0;
    }

    max_extension_count = instance_extension_count > device_extension_count
        ? instance_extension_count : device_extension_count;

    ext_properties = (VkExtensionProperties*) malloc(max_extension_count * sizeof(VkExtensionProperties));
    if (ext_properties == NULL) {
        goto glad_vk_get_extensions_error;
    }

    result = context->EnumerateInstanceExtensionProperties(NULL, &instance_extension_count, ext_properties);
    if (result != VK_SUCCESS) {
        goto glad_vk_get_extensions_error;
    }

    extensions = (char**) calloc(total_extension_count, sizeof(char*));
    if (extensions == NULL) {
        goto glad_vk_get_extensions_error;
    }

    for (i = 0; i < instance_extension_count; ++i) {
        VkExtensionProperties ext = ext_properties[i];

        size_t extension_name_length = strlen(ext.extensionName) + 1;
        extensions[i] = (char*) malloc(extension_name_length * sizeof(char));
        if (extensions[i] == NULL) {
            goto glad_vk_get_extensions_error;
        }
        memcpy(extensions[i], ext.extensionName, extension_name_length * sizeof(char));
    }

    if (physical_device != NULL) {
        result = context->EnumerateDeviceExtensionProperties(physical_device, NULL, &device_extension_count, ext_properties);
        if (result != VK_SUCCESS) {
            goto glad_vk_get_extensions_error;
        }

        for (i = 0; i < device_extension_count; ++i) {
            VkExtensionProperties ext = ext_properties[i];

            size_t extension_name_length = strlen(ext.extensionName) + 1;
            extensions[instance_extension_count + i] = (char*) malloc(extension_name_length * sizeof(char));
            if (extensions[instance_extension_count + i] == NULL) {
                goto glad_vk_get_extensions_error;
            }
            memcpy(extensions[instance_extension_count + i], ext.extensionName, extension_name_length * sizeof(char));
        }
    }

    free((void*) ext_properties);

    *out_extension_count = total_extension_count;
    *out_extensions = extensions;

    return 1;

glad_vk_get_extensions_error:
    free((void*) ext_properties);
    if (extensions != NULL) {
        for (i = 0; i < total_extension_count; ++i) {
            free((void*) extensions[i]);
        }
        free(extensions);
    }
    return 0;
}

static void glad_vk_free_extensions(uint32_t extension_count, char **extensions) {
    uint32_t i;

    for(i = 0; i < extension_count ; ++i) {
        free((void*) (extensions[i]));
    }

    free((void*) extensions);
}

static int glad_vk_has_extension(const char *name, uint32_t extension_count, char **extensions) {
    uint32_t i;

    for (i = 0; i < extension_count; ++i) {
        if(extensions[i] != NULL && strcmp(name, extensions[i]) == 0) {
            return 1;
        }
    }

    return 0;
}

static GLADapiproc glad_vk_get_proc_from_userptr(void *userptr, const char* name) {
    return (GLAD_GNUC_EXTENSION (GLADapiproc (*)(const char *name)) userptr)(name);
}

static int glad_vk_find_extensions_vulkan(GladVulkanContext *context, VkPhysicalDevice physical_device) {
    uint32_t extension_count = 0;
    char **extensions = NULL;
    if (!glad_vk_get_extensions(context, physical_device, &extension_count, &extensions)) return 0;

    context->EXT_conservative_rasterization = glad_vk_has_extension("VK_EXT_conservative_rasterization", extension_count, extensions);
    context->EXT_custom_border_color = glad_vk_has_extension("VK_EXT_custom_border_color", extension_count, extensions);
    context->EXT_debug_utils = glad_vk_has_extension("VK_EXT_debug_utils", extension_count, extensions);
    context->EXT_extended_dynamic_state = glad_vk_has_extension("VK_EXT_extended_dynamic_state", extension_count, extensions);
    context->EXT_extended_dynamic_state2 = glad_vk_has_extension("VK_EXT_extended_dynamic_state2", extension_count, extensions);
    context->EXT_extended_dynamic_state3 = glad_vk_has_extension("VK_EXT_extended_dynamic_state3", extension_count, extensions);
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    context->EXT_full_screen_exclusive = glad_vk_has_extension("VK_EXT_full_screen_exclusive", extension_count, extensions);

#endif
    context->EXT_graphics_pipeline_library = glad_vk_has_extension("VK_EXT_graphics_pipeline_library", extension_count, extensions);
    context->EXT_index_type_uint8 = glad_vk_has_extension("VK_EXT_index_type_uint8", extension_count, extensions);
    context->EXT_load_store_op_none = glad_vk_has_extension("VK_EXT_load_store_op_none", extension_count, extensions);
    context->EXT_mesh_shader = glad_vk_has_extension("VK_EXT_mesh_shader", extension_count, extensions);
    context->EXT_multi_draw = glad_vk_has_extension("VK_EXT_multi_draw", extension_count, extensions);
    context->EXT_private_data = glad_vk_has_extension("VK_EXT_private_data", extension_count, extensions);
    context->EXT_swapchain_colorspace = glad_vk_has_extension("VK_EXT_swapchain_colorspace", extension_count, extensions);
    context->EXT_tooling_info = glad_vk_has_extension("VK_EXT_tooling_info", extension_count, extensions);
    context->EXT_transform_feedback = glad_vk_has_extension("VK_EXT_transform_feedback", extension_count, extensions);
    context->KHR_acceleration_structure = glad_vk_has_extension("VK_KHR_acceleration_structure", extension_count, extensions);
    context->KHR_bind_memory2 = glad_vk_has_extension("VK_KHR_bind_memory2", extension_count, extensions);
    context->KHR_buffer_device_address = glad_vk_has_extension("VK_KHR_buffer_device_address", extension_count, extensions);
    context->KHR_copy_commands2 = glad_vk_has_extension("VK_KHR_copy_commands2", extension_count, extensions);
    context->KHR_create_renderpass2 = glad_vk_has_extension("VK_KHR_create_renderpass2", extension_count, extensions);
    context->KHR_dedicated_allocation = glad_vk_has_extension("VK_KHR_dedicated_allocation", extension_count, extensions);
    context->KHR_depth_stencil_resolve = glad_vk_has_extension("VK_KHR_depth_stencil_resolve", extension_count, extensions);
    context->KHR_descriptor_update_template = glad_vk_has_extension("VK_KHR_descriptor_update_template", extension_count, extensions);
    context->KHR_device_group = glad_vk_has_extension("VK_KHR_device_group", extension_count, extensions);
    context->KHR_display_swapchain = glad_vk_has_extension("VK_KHR_display_swapchain", extension_count, extensions);
    context->KHR_draw_indirect_count = glad_vk_has_extension("VK_KHR_draw_indirect_count", extension_count, extensions);
    context->KHR_dynamic_rendering = glad_vk_has_extension("VK_KHR_dynamic_rendering", extension_count, extensions);
    context->KHR_external_memory_capabilities = glad_vk_has_extension("VK_KHR_external_memory_capabilities", extension_count, extensions);
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    context->KHR_external_memory_win32 = glad_vk_has_extension("VK_KHR_external_memory_win32", extension_count, extensions);

#endif
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    context->KHR_external_semaphore_win32 = glad_vk_has_extension("VK_KHR_external_semaphore_win32", extension_count, extensions);

#endif
    context->KHR_get_memory_requirements2 = glad_vk_has_extension("VK_KHR_get_memory_requirements2", extension_count, extensions);
    context->KHR_get_physical_device_properties2 = glad_vk_has_extension("VK_KHR_get_physical_device_properties2", extension_count, extensions);
    context->KHR_image_format_list = glad_vk_has_extension("VK_KHR_image_format_list", extension_count, extensions);
    context->KHR_incremental_present = glad_vk_has_extension("VK_KHR_incremental_present", extension_count, extensions);
    context->KHR_maintenance4 = glad_vk_has_extension("VK_KHR_maintenance4", extension_count, extensions);
    context->KHR_maintenance5 = glad_vk_has_extension("VK_KHR_maintenance5", extension_count, extensions);
    context->KHR_pipeline_library = glad_vk_has_extension("VK_KHR_pipeline_library", extension_count, extensions);
    context->KHR_push_descriptor = glad_vk_has_extension("VK_KHR_push_descriptor", extension_count, extensions);
    context->KHR_ray_tracing_maintenance1 = glad_vk_has_extension("VK_KHR_ray_tracing_maintenance1", extension_count, extensions);
    context->KHR_ray_tracing_pipeline = glad_vk_has_extension("VK_KHR_ray_tracing_pipeline", extension_count, extensions);
    context->KHR_surface = glad_vk_has_extension("VK_KHR_surface", extension_count, extensions);
    context->KHR_swapchain = glad_vk_has_extension("VK_KHR_swapchain", extension_count, extensions);
    context->KHR_swapchain_mutable_format = glad_vk_has_extension("VK_KHR_swapchain_mutable_format", extension_count, extensions);
    context->KHR_synchronization2 = glad_vk_has_extension("VK_KHR_synchronization2", extension_count, extensions);
    context->KHR_timeline_semaphore = glad_vk_has_extension("VK_KHR_timeline_semaphore", extension_count, extensions);
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    context->KHR_win32_surface = glad_vk_has_extension("VK_KHR_win32_surface", extension_count, extensions);

#endif

    GLAD_UNUSED(&glad_vk_has_extension);

    glad_vk_free_extensions(extension_count, extensions);

    return 1;
}

static int glad_vk_find_core_vulkan(GladVulkanContext *context, VkPhysicalDevice physical_device) {
    int major = 1;
    int minor = 0;

#ifdef VK_VERSION_1_1
    if (context->EnumerateInstanceVersion != NULL) {
        uint32_t version;
        VkResult result;

        result = context->EnumerateInstanceVersion(&version);
        if (result == VK_SUCCESS) {
            major = (int) VK_VERSION_MAJOR(version);
            minor = (int) VK_VERSION_MINOR(version);
        }
    }
#endif

    if (physical_device != NULL && context->GetPhysicalDeviceProperties != NULL) {
        VkPhysicalDeviceProperties properties;
        context->GetPhysicalDeviceProperties(physical_device, &properties);

        major = (int) VK_VERSION_MAJOR(properties.apiVersion);
        minor = (int) VK_VERSION_MINOR(properties.apiVersion);
    }

    context->VERSION_1_0 = (major == 1 && minor >= 0) || major > 1;
    context->VERSION_1_1 = (major == 1 && minor >= 1) || major > 1;
    context->VERSION_1_2 = (major == 1 && minor >= 2) || major > 1;
    context->VERSION_1_3 = (major == 1 && minor >= 3) || major > 1;

    return GLAD_MAKE_VERSION(major, minor);
}

int gladLoadVulkanContextUserPtr(GladVulkanContext *context, VkPhysicalDevice physical_device, GLADuserptrloadfunc load, void *userptr) {
    int version;

#ifdef VK_VERSION_1_1
    context->EnumerateInstanceVersion  = (PFN_vkEnumerateInstanceVersion) load(userptr, "vkEnumerateInstanceVersion");
#endif
    version = glad_vk_find_core_vulkan(context, physical_device);
    if (!version) {
        return 0;
    }

    glad_vk_load_VK_VERSION_1_0(context, load, userptr);
    glad_vk_load_VK_VERSION_1_1(context, load, userptr);
    glad_vk_load_VK_VERSION_1_2(context, load, userptr);
    glad_vk_load_VK_VERSION_1_3(context, load, userptr);

    if (!glad_vk_find_extensions_vulkan(context, physical_device)) return 0;
    glad_vk_load_VK_EXT_debug_utils(context, load, userptr);
    glad_vk_load_VK_EXT_extended_dynamic_state(context, load, userptr);
    glad_vk_load_VK_EXT_extended_dynamic_state2(context, load, userptr);
    glad_vk_load_VK_EXT_extended_dynamic_state3(context, load, userptr);
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    glad_vk_load_VK_EXT_full_screen_exclusive(context, load, userptr);

#endif
    glad_vk_load_VK_EXT_mesh_shader(context, load, userptr);
    glad_vk_load_VK_EXT_multi_draw(context, load, userptr);
    glad_vk_load_VK_EXT_private_data(context, load, userptr);
    glad_vk_load_VK_EXT_tooling_info(context, load, userptr);
    glad_vk_load_VK_EXT_transform_feedback(context, load, userptr);
    glad_vk_load_VK_KHR_acceleration_structure(context, load, userptr);
    glad_vk_load_VK_KHR_bind_memory2(context, load, userptr);
    glad_vk_load_VK_KHR_buffer_device_address(context, load, userptr);
    glad_vk_load_VK_KHR_copy_commands2(context, load, userptr);
    glad_vk_load_VK_KHR_create_renderpass2(context, load, userptr);
    glad_vk_load_VK_KHR_descriptor_update_template(context, load, userptr);
    glad_vk_load_VK_KHR_device_group(context, load, userptr);
    glad_vk_load_VK_KHR_display_swapchain(context, load, userptr);
    glad_vk_load_VK_KHR_draw_indirect_count(context, load, userptr);
    glad_vk_load_VK_KHR_dynamic_rendering(context, load, userptr);
    glad_vk_load_VK_KHR_external_memory_capabilities(context, load, userptr);
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    glad_vk_load_VK_KHR_external_memory_win32(context, load, userptr);

#endif
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    glad_vk_load_VK_KHR_external_semaphore_win32(context, load, userptr);

#endif
    glad_vk_load_VK_KHR_get_memory_requirements2(context, load, userptr);
    glad_vk_load_VK_KHR_get_physical_device_properties2(context, load, userptr);
    glad_vk_load_VK_KHR_maintenance4(context, load, userptr);
    glad_vk_load_VK_KHR_maintenance5(context, load, userptr);
    glad_vk_load_VK_KHR_push_descriptor(context, load, userptr);
    glad_vk_load_VK_KHR_ray_tracing_maintenance1(context, load, userptr);
    glad_vk_load_VK_KHR_ray_tracing_pipeline(context, load, userptr);
    glad_vk_load_VK_KHR_surface(context, load, userptr);
    glad_vk_load_VK_KHR_swapchain(context, load, userptr);
    glad_vk_load_VK_KHR_synchronization2(context, load, userptr);
    glad_vk_load_VK_KHR_timeline_semaphore(context, load, userptr);
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    glad_vk_load_VK_KHR_win32_surface(context, load, userptr);

#endif


    return version;
}


int gladLoadVulkanContext(GladVulkanContext *context, VkPhysicalDevice physical_device, GLADloadfunc load) {
    return gladLoadVulkanContextUserPtr(context, physical_device, glad_vk_get_proc_from_userptr, GLAD_GNUC_EXTENSION (void*) load);
}



 


#ifdef __cplusplus
}
#endif
