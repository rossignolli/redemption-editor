// The host's Vulkan backend: the layer VK_LAYER_redemption_editor.
//
// The instance, device, swapchain and present entry points follow ReShade's
// own layer (external/reshade/source/vulkan/vulkan_hooks*.cpp, BSD-3-Clause
// OR MIT, Copyright (C) 2014 Patrick Mours), cut down to what the editor
// needs: no per object tracking (the editor only touches the back buffer and
// the resources it creates through reshade::api::device), the same device
// features and extensions ReShade turns on for its runtime, the same
// immediate command list synchronization on present. reshade::api::device /
// command_queue / command_list are ReShade's Vulkan implementation itself
// (vulkan_impl_*.cpp compiled from the submodule).
//
// Per present of a swapchain the host knows: host::OnPresent (core overlay,
// ImGui into the back buffer, core present), then the queue's immediate
// command list is submitted waiting the present's wait semaphores and
// signaling its own, and the present waits that one (ReShade's order).

#include "host_backend.h"
#include "host_imgui_renderer.h"
#include "host_log.h"

#include "vulkan_hooks.hpp"
#include "vulkan_impl_device.hpp"
#include "vulkan_impl_command_queue.hpp"
#include "vulkan_impl_type_convert.hpp"

#include "imgui_spirv.h"

#include <intrin.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#pragma intrinsic(_ReturnAddress)

namespace host::vk
{
    namespace
    {
        using reshade::vulkan::device_impl;
        using reshade::vulkan::command_queue_impl;

        constexpr char kLayerName[] = "VK_LAYER_redemption_editor";

        struct InstanceData
        {
            VkInstance handle = VK_NULL_HANDLE;
            uint32_t apiVersion = 0;
            GladVulkanContext dispatch{};
        };

        struct DeviceData
        {
            VkDevice handle = VK_NULL_HANDLE;
            PFN_vkGetDeviceProcAddr nextGetDeviceProcAddr = nullptr;
            PFN_vkDestroyDevice nextDestroyDevice = nullptr;
            PFN_vkQueuePresentKHR nextQueuePresent = nullptr;
            PFN_vkCreateSwapchainKHR nextCreateSwapchain = nullptr;
            PFN_vkDestroySwapchainKHR nextDestroySwapchain = nullptr;
            device_impl* impl = nullptr; // null: a device the host does not run on (no swapchain or graphics queue)
            ImGuiRenderer renderer;
        };

        class VkBackend;

        std::mutex g_mutex; // the maps below; never held across a call down the chain
        std::unordered_map<void*, InstanceData> g_instances;   // by dispatch key
        std::unordered_map<void*, DeviceData*> g_devices;      // by dispatch key
        std::unordered_map<VkSurfaceKHR, HWND> g_surfaces;
        std::unordered_map<VkSwapchainKHR, VkBackend*> g_swapchains;

        std::string ModuleOf(const void* address)
        {
            HMODULE module = nullptr;
            if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    static_cast<LPCWSTR>(address), &module))
                return "?";
            char path[MAX_PATH]{};
            GetModuleFileNameA(module, path, MAX_PATH);
            const char* name = std::strrchr(path, '\\');
            return name ? name + 1 : path;
        }

        InstanceData* FindInstance(const void* dispatchable)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const auto it = g_instances.find(dispatch_key_from_handle(dispatchable));
            return it == g_instances.end() ? nullptr : &it->second;
        }

        DeviceData* FindDevice(const void* dispatchable)
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const auto it = g_devices.find(dispatch_key_from_handle(dispatchable));
            return it == g_devices.end() ? nullptr : it->second;
        }

        // --------------------------------------------------------------
        // The Backend of one swapchain
        // --------------------------------------------------------------
        class VkBackend final : public host::Backend
        {
        public:
            VkBackend(DeviceData* device, VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR& info, HWND window, std::vector<VkImage> images) :
                m_device(device), m_swapchain(swapchain), m_info(info), m_window(window), m_images(std::move(images))
            {
                m_info.pNext = nullptr;
                m_info.pQueueFamilyIndices = nullptr;
            }

            reshade::api::device_api Api() const override { return reshade::api::device_api::vulkan; }
            reshade::api::device* Device() override { return m_device->impl; }
            reshade::api::command_queue* Queue() override { return m_device->impl->_primary_graphics_queue; }
            reshade::api::resource CurrentBackBuffer() override { return BackBuffer(m_index); }
            reshade::api::format BackBufferFormat() const override { return reshade::vulkan::convert_format(m_info.imageFormat); }
            void BackBufferSize(uint32_t& w, uint32_t& h) const override
            {
                w = m_info.imageExtent.width;
                h = m_info.imageExtent.height;
            }
            HWND Window() const override { return m_window; }
            void* Native() const override { return reinterpret_cast<void*>(m_swapchain); }
            uint32_t BackBufferCount() const override { return static_cast<uint32_t>(m_images.size()); }
            uint32_t BackBufferIndex() const override { return m_index; }
            reshade::api::resource BackBuffer(uint32_t index) override
            {
                return index < m_images.size() ? reshade::api::resource{reinterpret_cast<uint64_t>(m_images[index])} : reshade::api::resource{0};
            }
            ImTextureID TextureId(reshade::api::resource_view view) override { return static_cast<ImTextureID>(view.handle); }

            bool ImGuiInit() override
            {
                using namespace reshade::api;
                ImGuiIO& io = ImGui::GetIO();
                io.BackendRendererName = "RedemptionEditorHost (Vulkan)";
                io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;
                // The UI is drawn through the non sRGB view, as ReShade does.
                const format target = format_to_default_typed(BackBufferFormat(), 0);
                if (m_views.empty())
                {
                    for (size_t i = 0; i < m_images.size(); ++i)
                    {
                        resource_view view{0};
                        if (!m_device->impl->create_resource_view(BackBuffer(static_cast<uint32_t>(i)), resource_usage::render_target,
                                resource_view_desc(target), &view))
                        {
                            HOST_ERROR("HOST_IMGUI failed reason=back_buffer_view image=%u fmt=%u", static_cast<unsigned>(i), static_cast<unsigned>(target));
                            DestroyViews();
                            return false;
                        }
                        m_views.push_back(view);
                    }
                }
                return m_device->renderer.Init(m_device->impl, Queue(), target, ShaderCode{kImGuiVsSpirv, sizeof(kImGuiVsSpirv)},
                    ShaderCode{kImGuiPsSpirv, sizeof(kImGuiPsSpirv)});
            }

            void ImGuiNewFrame() override {}

            void ImGuiRender(ImDrawData* data) override
            {
                using namespace reshade::api;
                if (m_index >= m_views.size() || !m_device->renderer.Ready())
                    return;
                command_list* const cmd = Queue()->get_immediate_command_list();
                const resource back = CurrentBackBuffer();
                cmd->barrier(back, resource_usage::present, resource_usage::render_target);
                m_device->renderer.Render(cmd, data, m_views[m_index]);
                cmd->barrier(back, resource_usage::render_target, resource_usage::present);
            }

            void ImGuiShutdown() override
            {
                // The device's renderer (pipeline, ImGui's textures) stays for the
                // next swapchain; this swapchain's views go.
                if (!m_views.empty())
                    Queue()->wait_idle();
                DestroyViews();
            }

            DeviceData* Owner() const { return m_device; }
            VkSwapchainKHR Handle() const { return m_swapchain; }
            void SetIndex(uint32_t index) { m_index = index; }
            bool firstPresentLogged = false;

        private:
            void DestroyViews()
            {
                for (const reshade::api::resource_view view : m_views)
                    m_device->impl->destroy_resource_view(view);
                m_views.clear();
            }

            DeviceData* const m_device;
            const VkSwapchainKHR m_swapchain;
            VkSwapchainCreateInfoKHR m_info;
            const HWND m_window;
            const std::vector<VkImage> m_images;
            std::vector<reshade::api::resource_view> m_views;
            uint32_t m_index = 0;
        };

        // --------------------------------------------------------------
        // Instance
        // --------------------------------------------------------------
        struct VkLayerInstanceLink
        {
            VkLayerInstanceLink* pNext;
            PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
            PFN_vkGetInstanceProcAddr pfnNextGetPhysicalDeviceProcAddr;
        };
        struct VkLayerInstanceCreateInfo
        {
            VkStructureType sType; // VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO
            const void* pNext;
            VkLayerFunction function;
            union {
                VkLayerInstanceLink* pLayerInfo;
            } u;
        };
        struct VkLayerDeviceLink
        {
            VkLayerDeviceLink* pNext;
            PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
            PFN_vkGetDeviceProcAddr pfnNextGetDeviceProcAddr;
        };
        struct VkLayerDeviceCreateInfo
        {
            VkStructureType sType; // VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO
            const void* pNext;
            VkLayerFunction function;
            union {
                VkLayerDeviceLink* pLayerInfo;
            } u;
        };

        VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance)
        {
            const void* caller = _ReturnAddress();
            const auto link = find_layer_info<VkLayerInstanceCreateInfo>(pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, VK_LAYER_LINK_INFO);
            if (!link || !link->u.pLayerInfo)
            {
                HOST_ERROR("HOST_START failed reason=no_layer_link_info (not loaded by the Vulkan loader as a layer)");
                return VK_ERROR_INITIALIZATION_FAILED;
            }
            const PFN_vkGetInstanceProcAddr nextGipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
            const auto next = reinterpret_cast<PFN_vkCreateInstance>(nextGipa(nullptr, "vkCreateInstance"));
            // Every layer below this one, in call order (the bench needs to know
            // whether ReShade's layer is below: then the host sees the game's
            // picture before ReShade's effects and ReShade draws over the editor).
            std::string chain;
            for (const auto* l = link->u.pLayerInfo->pNext; l; l = l->pNext)
                chain += (chain.empty() ? "" : ">") + ModuleOf(reinterpret_cast<const void*>(l->pfnNextGetInstanceProcAddr));
            HOST_LOG("HOST_LAYER chain_below=%s>%s", ModuleOf(reinterpret_cast<const void*>(nextGipa)).c_str(), chain.c_str());
            link->u.pLayerInfo = link->u.pLayerInfo->pNext;
            if (!next)
                return VK_ERROR_INITIALIZATION_FAILED;

            // Where the host sits: the module that called down into this layer
            // (vulkan-1.dll when nothing is above, ReShade64.dll when ReShade's
            // implicit layer is above) and the one this layer calls (vulkan-1.dll
            // when it is the last layer).
            std::string layers;
            for (uint32_t i = 0; i < pCreateInfo->enabledLayerCount; ++i)
                layers += std::string(i ? "," : "") + pCreateInfo->ppEnabledLayerNames[i];
            HOST_LOG("HOST_LAYER vkCreateInstance above=%s below=%s app_layers=[%s] reshade_module=%s", ModuleOf(caller).c_str(),
                ModuleOf(reinterpret_cast<const void*>(nextGipa)).c_str(), layers.c_str(),
                (GetModuleHandleW(L"ReShade64.dll") || GetModuleHandleW(L"ReShade64_vk.dll")) ? "loaded" : "none");

            VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
            if (pCreateInfo->pApplicationInfo)
                app = *pCreateInfo->pApplicationInfo;
            // ReShade's runtime requires 1.1 (core GetPhysicalDeviceProperties2 and
            // friends in its device implementation); the host uses the same code.
            if (app.apiVersion < VK_API_VERSION_1_1)
                app.apiVersion = VK_API_VERSION_1_1;
            VkInstanceCreateInfo info = *pCreateInfo;
            info.pApplicationInfo = &app;
            const VkResult result = next(&info, pAllocator, pInstance);
            if (result != VK_SUCCESS)
            {
                HOST_WARN("HOST_LAYER vkCreateInstance failed result=%d", static_cast<int>(result));
                return result;
            }

            InstanceData data;
            data.handle = *pInstance;
            data.apiVersion = app.apiVersion;
            data.dispatch.GetInstanceProcAddr = nextGipa;
            data.dispatch.EnumerateInstanceExtensionProperties = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
                GetProcAddress(GetModuleHandleW(L"vulkan-1.dll"), "vkEnumerateInstanceExtensionProperties"));
            gladLoadVulkanContextUserPtr(&data.dispatch, VK_NULL_HANDLE,
                [](void* user, const char* name) -> GLADapiproc {
                    const auto& instance = *static_cast<const InstanceData*>(user);
                    if (0 == std::strcmp(name, "vkGetInstanceProcAddr"))
                        return reinterpret_cast<GLADapiproc>(instance.dispatch.GetInstanceProcAddr);
                    if (0 == std::strcmp(name, "vkEnumerateInstanceExtensionProperties"))
                        return reinterpret_cast<GLADapiproc>(instance.dispatch.EnumerateInstanceExtensionProperties);
                    return reinterpret_cast<GLADapiproc>(instance.dispatch.GetInstanceProcAddr(instance.handle, name));
                },
                &data);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_instances[dispatch_key_from_handle(*pInstance)] = data;
            }
            HOST_LOG("HOST_LAYER instance=%p api=%u.%u", static_cast<void*>(*pInstance), VK_API_VERSION_MAJOR(app.apiVersion),
                VK_API_VERSION_MINOR(app.apiVersion));
            return VK_SUCCESS;
        }

        void VKAPI_CALL DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
        {
            if (!instance)
                return;
            PFN_vkDestroyInstance next = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                const auto it = g_instances.find(dispatch_key_from_handle(instance));
                if (it != g_instances.end())
                {
                    next = it->second.dispatch.DestroyInstance;
                    g_instances.erase(it);
                }
            }
            if (next)
                next(instance, pAllocator);
        }

        VkResult VKAPI_CALL CreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator,
            VkSurfaceKHR* pSurface)
        {
            InstanceData* const data = FindInstance(instance);
            if (!data || !data->dispatch.CreateWin32SurfaceKHR)
                return VK_ERROR_INITIALIZATION_FAILED;
            const VkResult result = data->dispatch.CreateWin32SurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
            if (result == VK_SUCCESS)
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_surfaces[*pSurface] = pCreateInfo->hwnd;
            }
            return result;
        }

        void VKAPI_CALL DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* pAllocator)
        {
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_surfaces.erase(surface);
            }
            InstanceData* const data = FindInstance(instance);
            if (data && data->dispatch.DestroySurfaceKHR)
                data->dispatch.DestroySurfaceKHR(instance, surface, pAllocator);
        }

        // --------------------------------------------------------------
        // Device
        // --------------------------------------------------------------
        VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
            VkDevice* pDevice)
        {
            const auto link = find_layer_info<VkLayerDeviceCreateInfo>(pCreateInfo->pNext, VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, VK_LAYER_LINK_INFO);
            InstanceData* const instancePtr = FindInstance(physicalDevice);
            if (!link || !link->u.pLayerInfo || !instancePtr)
                return VK_ERROR_INITIALIZATION_FAILED;
            const InstanceData instance = *instancePtr;
            const PFN_vkGetInstanceProcAddr nextGipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
            const PFN_vkGetDeviceProcAddr nextGdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
            const auto next = reinterpret_cast<PFN_vkCreateDevice>(nextGipa(instance.handle, "vkCreateDevice"));
            link->u.pLayerInfo = link->u.pLayerInfo->pNext;
            if (!next)
                return VK_ERROR_INITIALIZATION_FAILED;

            uint32_t familyCount = 0;
            instance.dispatch.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
            std::vector<VkQueueFamilyProperties> families(familyCount);
            instance.dispatch.GetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());

            uint32_t graphicsFamily = UINT32_MAX;
            for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i)
            {
                const uint32_t family = pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex;
                if (family < familyCount && pCreateInfo->pQueueCreateInfos[i].queueCount > 0 && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
                {
                    graphicsFamily = family;
                    break;
                }
            }

            std::vector<const char*> extensions(pCreateInfo->ppEnabledExtensionNames, pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
            const auto enabled = [&extensions](const char* name) {
                return std::find_if(extensions.begin(), extensions.end(), [name](const char* e) { return std::strcmp(e, name) == 0; }) != extensions.end();
            };

            VkPhysicalDeviceFeatures features{};
            const auto features2 = find_in_structure_chain<VkPhysicalDeviceFeatures2>(pCreateInfo->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
            if (features2)
                features = features2->features;
            else if (pCreateInfo->pEnabledFeatures)
                features = *pCreateInfo->pEnabledFeatures;

            bool eligible = enabled(VK_KHR_SWAPCHAIN_EXTENSION_NAME) && graphicsFamily != UINT32_MAX;
            bool pushDescriptor = false, dynamicRendering = false, timelineSemaphore = false;
            std::string skipped = !enabled(VK_KHR_SWAPCHAIN_EXTENSION_NAME) ? "no_swapchain_extension" : graphicsFamily == UINT32_MAX ? "no_graphics_queue" : "";
            if (eligible)
            {
                uint32_t count = 0;
                instance.dispatch.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr);
                std::vector<VkExtensionProperties> available(count);
                instance.dispatch.EnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, available.data());
                const auto add = [&](const char* name) {
                    if (enabled(name))
                        return true;
                    if (std::find_if(available.begin(), available.end(), [name](const VkExtensionProperties& p) {
                            return std::strncmp(p.extensionName, name, VK_MAX_EXTENSION_NAME_SIZE) == 0;
                        }) == available.end())
                        return false;
                    extensions.push_back(name);
                    return true;
                };
                // What ReShade turns on for its runtime (vulkan_hooks_device.cpp), the
                // parts reshade::api's Vulkan implementation relies on.
                if (instance.apiVersion < VK_API_VERSION_1_3 && !add(VK_EXT_PRIVATE_DATA_EXTENSION_NAME))
                {
                    eligible = false;
                    skipped = "no_private_data";
                }
                if (eligible)
                {
                    add(VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME);
                    add(VK_KHR_SWAPCHAIN_MUTABLE_FORMAT_EXTENSION_NAME);
                    pushDescriptor = add(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
                    dynamicRendering = instance.apiVersion >= VK_API_VERSION_1_3 || add(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
                    if (dynamicRendering && instance.apiVersion < VK_API_VERSION_1_3)
                    {
                        add(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
                        add(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
                    }
                    timelineSemaphore = instance.apiVersion >= VK_API_VERSION_1_2 || add(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
                    VkPhysicalDeviceFeatures supported{};
                    instance.dispatch.GetPhysicalDeviceFeatures(physicalDevice, &supported);
                    features.samplerAnisotropy |= supported.samplerAnisotropy;
                    features.shaderImageGatherExtended |= supported.shaderImageGatherExtended;
                    features.shaderStorageImageReadWithoutFormat |= supported.shaderStorageImageReadWithoutFormat;
                    features.shaderStorageImageWriteWithoutFormat |= supported.shaderStorageImageWriteWithoutFormat;
                }
            }

            VkDeviceCreateInfo info = *pCreateInfo;
            info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
            info.ppEnabledExtensionNames = extensions.data();

            VkDevicePrivateDataCreateInfo privateDataInfo{VK_STRUCTURE_TYPE_DEVICE_PRIVATE_DATA_CREATE_INFO};
            VkPhysicalDevicePrivateDataFeatures privateDataFeature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES};
            VkPhysicalDeviceDynamicRenderingFeatures dynamicRenderingFeature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
            VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeature{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
            if (eligible)
            {
                // Same patching as ReShade (it writes into the application's
                // structures where they exist).
                if (features2)
                    const_cast<VkPhysicalDeviceFeatures2*>(features2)->features = features;
                else
                    info.pEnabledFeatures = &features;

                // One private data slot; another layer (ReShade) may have asked for
                // its own already: one more on the same request then.
                if (const auto existing = find_in_structure_chain<VkDevicePrivateDataCreateInfo>(info.pNext, VK_STRUCTURE_TYPE_DEVICE_PRIVATE_DATA_CREATE_INFO))
                    const_cast<VkDevicePrivateDataCreateInfo*>(existing)->privateDataSlotRequestCount += 1;
                else
                {
                    privateDataInfo.pNext = info.pNext;
                    privateDataInfo.privateDataSlotRequestCount = 1;
                    info.pNext = &privateDataInfo;
                }

                if (const auto v13 = find_in_structure_chain<VkPhysicalDeviceVulkan13Features>(info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES))
                {
                    const_cast<VkPhysicalDeviceVulkan13Features*>(v13)->privateData = VK_TRUE;
                    dynamicRendering = v13->dynamicRendering != VK_FALSE;
                }
                else
                {
                    if (const auto existing = find_in_structure_chain<VkPhysicalDevicePrivateDataFeatures>(info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRIVATE_DATA_FEATURES))
                        const_cast<VkPhysicalDevicePrivateDataFeatures*>(existing)->privateData = VK_TRUE;
                    else
                    {
                        privateDataFeature.pNext = const_cast<void*>(info.pNext);
                        privateDataFeature.privateData = VK_TRUE;
                        info.pNext = &privateDataFeature;
                    }
                    if (const auto existing = find_in_structure_chain<VkPhysicalDeviceDynamicRenderingFeatures>(info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES))
                        dynamicRendering = existing->dynamicRendering != VK_FALSE;
                    else if (dynamicRendering)
                    {
                        dynamicRenderingFeature.pNext = const_cast<void*>(info.pNext);
                        dynamicRenderingFeature.dynamicRendering = VK_TRUE;
                        info.pNext = &dynamicRenderingFeature;
                    }
                }
                if (const auto v12 = find_in_structure_chain<VkPhysicalDeviceVulkan12Features>(info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES))
                    const_cast<VkPhysicalDeviceVulkan12Features*>(v12)->timelineSemaphore = VK_TRUE;
                else if (const auto existing = find_in_structure_chain<VkPhysicalDeviceTimelineSemaphoreFeatures>(info.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES))
                    timelineSemaphore = existing->timelineSemaphore != VK_FALSE;
                else if (timelineSemaphore)
                {
                    timelineFeature.pNext = const_cast<void*>(info.pNext);
                    timelineFeature.timelineSemaphore = VK_TRUE;
                    info.pNext = &timelineFeature;
                }
            }

            const VkResult result = next(physicalDevice, &info, pAllocator, pDevice);
            if (result != VK_SUCCESS)
            {
                HOST_ERROR("HOST_START failed reason=vkCreateDevice result=%d (with the host's extensions/features)", static_cast<int>(result));
                return result;
            }

            auto* data = new DeviceData();
            data->handle = *pDevice;
            data->nextGetDeviceProcAddr = nextGdpa;
            data->nextDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(nextGdpa(*pDevice, "vkDestroyDevice"));
            data->nextQueuePresent = reinterpret_cast<PFN_vkQueuePresentKHR>(nextGdpa(*pDevice, "vkQueuePresentKHR"));
            data->nextCreateSwapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(nextGdpa(*pDevice, "vkCreateSwapchainKHR"));
            data->nextDestroySwapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(nextGdpa(*pDevice, "vkDestroySwapchainKHR"));
            if (eligible)
            {
                struct Loader
                {
                    VkDevice device;
                    VkInstance instance;
                    PFN_vkGetDeviceProcAddr gdpa;
                    PFN_vkGetInstanceProcAddr gipa;
                    const GladVulkanContext* instanceDispatch;
                } loader{*pDevice, instance.handle, nextGdpa, nextGipa, &instance.dispatch};
                GladVulkanContext dispatch = instance.dispatch;
                dispatch.GetDeviceProcAddr = nextGdpa;
                dispatch.GetInstanceProcAddr = nextGipa;
                // ReShade's loader callback: instance level names from the instance
                // chain, the rest from the device chain below this layer.
                gladLoadVulkanContextUserPtr(&dispatch, physicalDevice,
                    [](void* user, const char* name) -> GLADapiproc {
                        const auto& l = *static_cast<const Loader*>(user);
                        if (0 == std::strcmp(name, "vkGetDeviceProcAddr"))
                            return reinterpret_cast<GLADapiproc>(l.gdpa);
                        if (0 == std::strcmp(name, "vkGetInstanceProcAddr"))
                            return reinterpret_cast<GLADapiproc>(l.gipa);
                        if (0 == std::strcmp(name, "vkEnumerateInstanceVersion"))
                            return reinterpret_cast<GLADapiproc>(l.instanceDispatch->EnumerateInstanceVersion);
                        if (0 == std::strcmp(name, "vkEnumerateDeviceLayerProperties"))
                            return reinterpret_cast<GLADapiproc>(l.instanceDispatch->EnumerateDeviceLayerProperties);
                        if (0 == std::strcmp(name, "vkEnumerateInstanceLayerProperties"))
                            return reinterpret_cast<GLADapiproc>(l.instanceDispatch->EnumerateInstanceLayerProperties);
                        if (0 == std::strcmp(name, "vkEnumerateDeviceExtensionProperties"))
                            return reinterpret_cast<GLADapiproc>(l.instanceDispatch->EnumerateDeviceExtensionProperties);
                        if (0 == std::strcmp(name, "vkEnumerateInstanceExtensionProperties"))
                            return reinterpret_cast<GLADapiproc>(l.instanceDispatch->EnumerateInstanceExtensionProperties);
                        if (0 == std::strcmp(name, "vkEnumeratePhysicalDevices"))
                            return reinterpret_cast<GLADapiproc>(l.instanceDispatch->EnumeratePhysicalDevices);
                        if (0 == std::strcmp(name, "vkEnumeratePhysicalDeviceGroups"))
                            return reinterpret_cast<GLADapiproc>(l.instanceDispatch->EnumeratePhysicalDeviceGroups);
                        if (0 == std::strcmp(name, "vkCreateDevice") || 0 == std::strcmp(name, "vkCreateInstance") ||
                            0 == std::strcmp(name, "vkDestroyInstance") || 0 == std::strcmp(name, "vkSubmitDebugUtilsMessageEXT") ||
                            0 == std::strcmp(name, "vkCreateDebugUtilsMessengerEXT") || 0 == std::strcmp(name, "vkDestroyDebugUtilsMessengerEXT") ||
                            (std::strstr(name, "Properties") != nullptr && std::strstr(name, "AccelerationStructures") == nullptr &&
                                std::strstr(name, "Handle") == nullptr) ||
                            (std::strstr(name, "Surface") != nullptr && std::strstr(name, "DeviceGroupSurface") == nullptr) ||
                            std::strstr(name, "PhysicalDevice") != nullptr)
                            return reinterpret_cast<GLADapiproc>(l.gipa(l.instance, name));
                        return reinterpret_cast<GLADapiproc>(l.gdpa(l.device, name));
                    },
                    &loader);
                dispatch.KHR_push_descriptor &= pushDescriptor ? 1 : 0;
                dispatch.KHR_dynamic_rendering &= dynamicRendering ? 1 : 0;
                dispatch.KHR_timeline_semaphore &= timelineSemaphore ? 1 : 0;
                // The host turns none of these on: ReShade's code must not take their paths.
                dispatch.EXT_custom_border_color = 0;
                dispatch.EXT_extended_dynamic_state = 0;
                dispatch.EXT_conservative_rasterization = 0;
                dispatch.KHR_ray_tracing_pipeline = 0;
                dispatch.KHR_acceleration_structure = 0;
                dispatch.KHR_buffer_device_address = 0;
                if (instance.apiVersion < VK_API_VERSION_1_2)
                {
                    dispatch.CreateRenderPass2 = dispatch.CreateRenderPass2KHR;
                    dispatch.CmdBeginRenderPass2 = dispatch.CmdBeginRenderPass2KHR;
                    dispatch.CmdNextSubpass2 = dispatch.CmdNextSubpass2KHR;
                    dispatch.CmdEndRenderPass2 = dispatch.CmdEndRenderPass2KHR;
                    dispatch.CmdDrawIndirectCount = dispatch.CmdDrawIndirectCountKHR;
                    dispatch.CmdDrawIndexedIndirectCount = dispatch.CmdDrawIndexedIndirectCountKHR;
                    dispatch.GetSemaphoreCounterValue = dispatch.GetSemaphoreCounterValueKHR;
                    dispatch.WaitSemaphores = dispatch.WaitSemaphoresKHR;
                    dispatch.SignalSemaphore = dispatch.SignalSemaphoreKHR;
                    dispatch.GetBufferDeviceAddress = dispatch.GetBufferDeviceAddressKHR;
                }
                if (instance.apiVersion < VK_API_VERSION_1_3)
                {
                    dispatch.CmdBeginRendering = dispatch.CmdBeginRenderingKHR;
                    dispatch.CmdEndRendering = dispatch.CmdEndRenderingKHR;
                    dispatch.CmdPipelineBarrier2 = dispatch.CmdPipelineBarrier2KHR;
                    dispatch.CmdWriteTimestamp2 = dispatch.CmdWriteTimestamp2KHR;
                    dispatch.QueueSubmit2 = dispatch.QueueSubmit2KHR;
                    dispatch.CmdCopyBuffer2 = dispatch.CmdCopyBuffer2KHR;
                    dispatch.CmdCopyImage2 = dispatch.CmdCopyImage2KHR;
                    dispatch.CmdCopyBufferToImage2 = dispatch.CmdCopyBufferToImage2KHR;
                    dispatch.CmdCopyImageToBuffer2 = dispatch.CmdCopyImageToBuffer2KHR;
                    dispatch.CmdBlitImage2 = dispatch.CmdBlitImage2KHR;
                    dispatch.CmdResolveImage2 = dispatch.CmdResolveImage2KHR;
                    dispatch.CmdSetCullMode = dispatch.CmdSetCullModeEXT;
                    dispatch.CmdSetFrontFace = dispatch.CmdSetFrontFaceEXT;
                    dispatch.CmdSetPrimitiveTopology = dispatch.CmdSetPrimitiveTopologyEXT;
                    dispatch.CmdSetViewportWithCount = dispatch.CmdSetViewportWithCountEXT;
                    dispatch.CmdSetScissorWithCount = dispatch.CmdSetScissorWithCountEXT;
                    dispatch.CmdBindVertexBuffers2 = dispatch.CmdBindVertexBuffers2EXT;
                    dispatch.CmdSetDepthTestEnable = dispatch.CmdSetDepthTestEnableEXT;
                    dispatch.CmdSetDepthWriteEnable = dispatch.CmdSetDepthWriteEnableEXT;
                    dispatch.CmdSetDepthCompareOp = dispatch.CmdSetDepthCompareOpEXT;
                    dispatch.CmdSetDepthBoundsTestEnable = dispatch.CmdSetDepthBoundsTestEnableEXT;
                    dispatch.CmdSetStencilTestEnable = dispatch.CmdSetStencilTestEnableEXT;
                    dispatch.CmdSetStencilOp = dispatch.CmdSetStencilOpEXT;
                    dispatch.CreatePrivateDataSlot = dispatch.CreatePrivateDataSlotEXT;
                    dispatch.DestroyPrivateDataSlot = dispatch.DestroyPrivateDataSlotEXT;
                    dispatch.GetPrivateData = dispatch.GetPrivateDataEXT;
                    dispatch.SetPrivateData = dispatch.SetPrivateDataEXT;
                }

                data->impl = new device_impl(*pDevice, physicalDevice, instance.handle, instance.apiVersion, dispatch, features);
                // Every queue of the device (ReShade's vkCreateDevice): the first queue
                // of the graphics family is the one the editor records into.
                for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; ++i)
                {
                    const VkDeviceQueueCreateInfo& qci = pCreateInfo->pQueueCreateInfos[i];
                    for (uint32_t q = 0; q < qci.queueCount; ++q)
                    {
                        VkDeviceQueueInfo2 queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2};
                        queueInfo.flags = qci.flags;
                        queueInfo.queueFamilyIndex = qci.queueFamilyIndex;
                        queueInfo.queueIndex = q;
                        VkQueue queue = VK_NULL_HANDLE;
                        dispatch.GetDeviceQueue2(*pDevice, &queueInfo, &queue);
                        if (!queue)
                            continue;
                        // The loader sets the dispatch pointer of queues it returns, not of
                        // queues fetched from inside a layer (ReShade does the same).
                        *reinterpret_cast<void**>(queue) = *reinterpret_cast<void**>(*pDevice);
                        auto* queueImpl = new reshade::vulkan::object_data<VK_OBJECT_TYPE_QUEUE>(data->impl, qci.queueFamilyIndex, families[qci.queueFamilyIndex], queue);
                        data->impl->register_object<VK_OBJECT_TYPE_QUEUE>(queue, queueImpl);
                        if (qci.queueFamilyIndex == graphicsFamily && q == 0)
                        {
                            data->impl->_primary_graphics_queue = queueImpl;
                            data->impl->_primary_graphics_queue_family_index = graphicsFamily;
                        }
                    }
                }
                HOST_LOG("HOST_DEVICE device=%p api=%u.%u graphics_family=%u push_descriptor=%d dynamic_rendering=%d timeline_semaphore=%d immediate_list=%s",
                    static_cast<void*>(*pDevice), VK_API_VERSION_MAJOR(instance.apiVersion), VK_API_VERSION_MINOR(instance.apiVersion), graphicsFamily,
                    pushDescriptor ? 1 : 0, dynamicRendering ? 1 : 0, timelineSemaphore ? 1 : 0,
                    data->impl->_primary_graphics_queue && data->impl->_primary_graphics_queue->get_immediate_command_list() ? "ok" : "missing");
            }
            else
                HOST_LOG("HOST_DEVICE device=%p skipped reason=%s", static_cast<void*>(*pDevice), skipped.c_str());

            // The device layers below this one, by the module of the next vkGetDeviceProcAddr.
            HOST_LOG("HOST_LAYER vkCreateDevice above=%s below=%s", ModuleOf(_ReturnAddress()).c_str(),
                ModuleOf(reinterpret_cast<const void*>(nextGdpa)).c_str());
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_devices[dispatch_key_from_handle(*pDevice)] = data;
            }
            return VK_SUCCESS;
        }

        void VKAPI_CALL DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
        {
            if (!device)
                return;
            DeviceData* data = nullptr;
            std::vector<VkBackend*> orphans;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                const auto it = g_devices.find(dispatch_key_from_handle(device));
                if (it == g_devices.end())
                    return;
                data = it->second;
                g_devices.erase(it);
                for (auto sc = g_swapchains.begin(); sc != g_swapchains.end();)
                {
                    if (sc->second->Owner() == data)
                    {
                        orphans.push_back(sc->second);
                        sc = g_swapchains.erase(sc);
                    }
                    else
                        ++sc;
                }
            }
            for (VkBackend* backend : orphans)
            {
                host::OnBackendDestroyed(*backend);
                delete backend;
            }
            if (data->impl)
            {
                data->renderer.Shutdown();
                const std::vector<command_queue_impl*> queues = data->impl->_queues;
                for (command_queue_impl* queue : queues)
                {
                    auto* queueData = static_cast<reshade::vulkan::object_data<VK_OBJECT_TYPE_QUEUE>*>(queue);
                    data->impl->unregister_object<VK_OBJECT_TYPE_QUEUE, false>(queueData->_orig);
                    delete queueData;
                }
                delete data->impl;
            }
            const PFN_vkDestroyDevice next = data->nextDestroyDevice;
            delete data;
            if (next)
                next(device, pAllocator);
        }

        // --------------------------------------------------------------
        // Swapchain and present
        // --------------------------------------------------------------
        VkResult VKAPI_CALL CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator,
            VkSwapchainKHR* pSwapchain)
        {
            DeviceData* const data = FindDevice(device);
            if (!data)
                return VK_ERROR_INITIALIZATION_FAILED;
            const PFN_vkCreateSwapchainKHR next = data->nextCreateSwapchain;
            if (!next)
                return VK_ERROR_INITIALIZATION_FAILED;
            if (!data->impl || !data->impl->_primary_graphics_queue)
                return next(device, pCreateInfo, pAllocator, pSwapchain);

            VkSwapchainCreateInfoKHR info = *pCreateInfo;
            // What ReShade adds: the editor draws into the image and copies from it.
            info.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            std::vector<VkFormat> formats;
            VkImageFormatListCreateInfoKHR formatList{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR};
            const VkFormat unorm = reshade::vulkan::convert_format(reshade::api::format_to_default_typed(reshade::vulkan::convert_format(info.imageFormat), 0));
            const VkFormat srgb = reshade::vulkan::convert_format(reshade::api::format_to_default_typed(reshade::vulkan::convert_format(info.imageFormat), 1));
            if (unorm != srgb && info.imageFormat != unorm)
            {
                // An sRGB swapchain: the UI is drawn through the UNORM view.
                formats = {unorm, srgb};
                info.flags |= VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR;
                if (const auto existing = find_in_structure_chain<VkImageFormatListCreateInfoKHR>(pCreateInfo->pNext, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO_KHR))
                {
                    formats.insert(formats.end(), existing->pViewFormats, existing->pViewFormats + existing->viewFormatCount);
                    std::sort(formats.begin(), formats.end());
                    formats.erase(std::unique(formats.begin(), formats.end()), formats.end());
                    const_cast<VkImageFormatListCreateInfoKHR*>(existing)->viewFormatCount = static_cast<uint32_t>(formats.size());
                    const_cast<VkImageFormatListCreateInfoKHR*>(existing)->pViewFormats = formats.data();
                }
                else
                {
                    formatList.pNext = info.pNext;
                    formatList.viewFormatCount = static_cast<uint32_t>(formats.size());
                    formatList.pViewFormats = formats.data();
                    info.pNext = &formatList;
                }
            }
            std::vector<uint32_t> familyList;
            if (info.imageSharingMode == VK_SHARING_MODE_CONCURRENT)
            {
                familyList.push_back(data->impl->_primary_graphics_queue_family_index);
                for (uint32_t i = 0; i < info.queueFamilyIndexCount; ++i)
                    if (info.pQueueFamilyIndices[i] != data->impl->_primary_graphics_queue_family_index)
                        familyList.push_back(info.pQueueFamilyIndices[i]);
                info.queueFamilyIndexCount = static_cast<uint32_t>(familyList.size());
                info.pQueueFamilyIndices = familyList.data();
            }

            // A recreation (resolution or mode change): the driver may free the old
            // swapchain's images inside this call and hand their handles to the new
            // one, so everything on the old images goes first, as ReShade does: the
            // core's runtime resources, the UI views, the images' private data.
            if (info.oldSwapchain != VK_NULL_HANDLE)
            {
                VkBackend* old = nullptr;
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    const auto it = g_swapchains.find(info.oldSwapchain);
                    if (it != g_swapchains.end())
                    {
                        old = it->second;
                        g_swapchains.erase(it);
                    }
                }
                if (old)
                {
                    host::OnBackendDestroyed(*old);
                    for (uint32_t i = 0; i < old->BackBufferCount(); ++i)
                        data->impl->unregister_object<VK_OBJECT_TYPE_IMAGE>(reinterpret_cast<VkImage>(old->BackBuffer(i).handle));
                    delete old;
                    HOST_LOG("HOST_SWAPCHAIN retired %p (recreated)", reinterpret_cast<void*>(info.oldSwapchain));
                }
            }

            const VkResult result = next(device, &info, pAllocator, pSwapchain);
            if (result != VK_SUCCESS)
            {
                HOST_WARN("HOST_SWAPCHAIN create failed result=%d", static_cast<int>(result));
                return result;
            }

            HWND window = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                const auto it = g_surfaces.find(info.surface);
                if (it != g_surfaces.end())
                    window = it->second;
            }
            uint32_t count = 0;
            data->impl->_dispatch_table.GetSwapchainImagesKHR(device, *pSwapchain, &count, nullptr);
            std::vector<VkImage> images(count);
            data->impl->_dispatch_table.GetSwapchainImagesKHR(device, *pSwapchain, &count, images.data());
            images.resize(count);
            // reshade::api's Vulkan code finds an image's description in its private data.
            for (VkImage image : images)
            {
                auto& imageData = *data->impl->register_object<VK_OBJECT_TYPE_IMAGE>(image);
                imageData.create_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
                imageData.create_info.imageType = VK_IMAGE_TYPE_2D;
                imageData.create_info.format = info.imageFormat;
                imageData.create_info.extent = {info.imageExtent.width, info.imageExtent.height, 1};
                imageData.create_info.mipLevels = 1;
                imageData.create_info.arrayLayers = info.imageArrayLayers;
                imageData.create_info.samples = VK_SAMPLE_COUNT_1_BIT;
                imageData.create_info.usage = info.imageUsage;
                imageData.create_info.sharingMode = info.imageSharingMode;
                imageData.create_info.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                if ((info.flags & VK_SWAPCHAIN_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT_KHR) != 0)
                    imageData.create_info.flags |= VK_IMAGE_CREATE_SPLIT_INSTANCE_BIND_REGIONS_BIT;
                if ((info.flags & VK_SWAPCHAIN_CREATE_PROTECTED_BIT_KHR) != 0)
                    imageData.create_info.flags |= VK_IMAGE_CREATE_PROTECTED_BIT;
                if ((info.flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) != 0)
                    imageData.create_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
            }

            auto* backend = new VkBackend(data, *pSwapchain, info, window, images);
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                g_swapchains[*pSwapchain] = backend;
            }
            HOST_LOG("HOST_START api=vulkan layer=%s swapchain=%ux%u fmt=%d usage=0x%x app_usage=0x%x flags=0x%x images=%u present_mode=%d hwnd=%p old=%p",
                kLayerName, info.imageExtent.width, info.imageExtent.height, static_cast<int>(info.imageFormat), static_cast<unsigned>(info.imageUsage),
                static_cast<unsigned>(pCreateInfo->imageUsage), static_cast<unsigned>(info.flags), count, static_cast<int>(info.presentMode),
                static_cast<void*>(window), reinterpret_cast<void*>(info.oldSwapchain));
            return VK_SUCCESS;
        }

        void VKAPI_CALL DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator)
        {
            DeviceData* const data = FindDevice(device);
            if (!data)
                return;
            const PFN_vkDestroySwapchainKHR next = data->nextDestroySwapchain;
            VkBackend* backend = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                const auto it = g_swapchains.find(swapchain);
                if (it != g_swapchains.end())
                {
                    backend = it->second;
                    g_swapchains.erase(it);
                }
            }
            if (backend)
            {
                host::OnBackendDestroyed(*backend);
                for (uint32_t i = 0; i < backend->BackBufferCount(); ++i)
                    data->impl->unregister_object<VK_OBJECT_TYPE_IMAGE>(reinterpret_cast<VkImage>(backend->BackBuffer(i).handle));
                delete backend;
                HOST_LOG("HOST_SWAPCHAIN destroyed %p", reinterpret_cast<void*>(swapchain));
            }
            if (next)
                next(device, swapchain, pAllocator);
        }

        VkResult VKAPI_CALL QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
        {
            DeviceData* const data = FindDevice(queue);
            if (!data)
                return VK_ERROR_DEVICE_LOST;
            const PFN_vkQueuePresentKHR next = data->nextQueuePresent;
            command_queue_impl* const primary = data->impl && host::Active() ? data->impl->_primary_graphics_queue : nullptr;
            command_queue_impl* const queueImpl =
                data->impl ? data->impl->get_private_data_for_object<VK_OBJECT_TYPE_QUEUE, true>(queue) : nullptr;
            if (!primary || !queueImpl || !next)
                return next ? next(queue, pPresentInfo) : VK_ERROR_DEVICE_LOST;

            VkPresentInfoKHR presentInfo = *pPresentInfo;
            const bool secondary = primary != queueImpl;
            if (secondary)
                std::lock(queueImpl->_mutex, primary->_mutex);
            else
                queueImpl->_mutex.lock();

            bool drew = false;
            for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i)
            {
                VkBackend* backend = nullptr;
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    const auto it = g_swapchains.find(pPresentInfo->pSwapchains[i]);
                    if (it != g_swapchains.end())
                        backend = it->second;
                }
                if (!backend || !backend->Window())
                    continue;
                backend->SetIndex(pPresentInfo->pImageIndices[i]);
                if (!backend->firstPresentLogged)
                {
                    backend->firstPresentLogged = true;
                    HOST_LOG("HOST_LAYER vkQueuePresentKHR swapchain=%p above=%s queue_family_primary=%s wait_semaphores=%u",
                        reinterpret_cast<void*>(pPresentInfo->pSwapchains[i]), ModuleOf(_ReturnAddress()).c_str(), secondary ? "no" : "yes",
                        pPresentInfo->waitSemaphoreCount);
                }
                host::OnPresent(*backend);
                drew = host::Active();
            }

            if (drew)
            {
                // ReShade's synchronization (vulkan_hooks_device.cpp vkQueuePresentKHR):
                // the immediate command list waits the present's semaphores and
                // signals its own, which the present then waits.
                std::vector<VkPipelineStageFlags> stages(presentInfo.waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
                submit.waitSemaphoreCount = presentInfo.waitSemaphoreCount;
                submit.pWaitSemaphores = presentInfo.pWaitSemaphores;
                submit.pWaitDstStageMask = stages.data();
                if (secondary)
                {
                    // Presenting from another queue than the one recorded into: signal the
                    // semaphores again on the present queue, then the graphics queue's
                    // list waits them.
                    submit.signalSemaphoreCount = presentInfo.waitSemaphoreCount;
                    submit.pSignalSemaphores = presentInfo.pWaitSemaphores;
                    data->impl->_dispatch_table.QueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
                    submit.signalSemaphoreCount = 0;
                    submit.pSignalSemaphores = nullptr;
                    primary->flush_immediate_command_list(submit);
                }
                queueImpl->flush_immediate_command_list(submit);
                presentInfo.waitSemaphoreCount = submit.waitSemaphoreCount;
                presentInfo.pWaitSemaphores = submit.pWaitSemaphores;
                data->impl->advance_transient_descriptor_pool();
            }

            const VkResult result = next(queue, &presentInfo);
            if (secondary)
                primary->_mutex.unlock();
            queueImpl->_mutex.unlock();
            return result;
        }

        // --------------------------------------------------------------
        // Layer entry points
        // --------------------------------------------------------------
        PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* pName);
        PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* pName);

#define HOST_VK_HOOK(name, function) \
    if (0 == std::strcmp(pName, name)) \
        return reinterpret_cast<PFN_vkVoidFunction>(function)

        PFN_vkVoidFunction VKAPI_CALL GetDeviceProcAddr(VkDevice device, const char* pName)
        {
            HOST_VK_HOOK("vkDestroyDevice", DestroyDevice);
            HOST_VK_HOOK("vkCreateSwapchainKHR", CreateSwapchainKHR);
            HOST_VK_HOOK("vkDestroySwapchainKHR", DestroySwapchainKHR);
            HOST_VK_HOOK("vkQueuePresentKHR", QueuePresentKHR);
            HOST_VK_HOOK("vkGetDeviceProcAddr", GetDeviceProcAddr);
            if (!device)
                return nullptr;
            DeviceData* const data = FindDevice(device);
            return data ? data->nextGetDeviceProcAddr(device, pName) : nullptr;
        }

        PFN_vkVoidFunction VKAPI_CALL GetInstanceProcAddr(VkInstance instance, const char* pName)
        {
            HOST_VK_HOOK("vkCreateInstance", CreateInstance);
            HOST_VK_HOOK("vkDestroyInstance", DestroyInstance);
            HOST_VK_HOOK("vkCreateDevice", CreateDevice);
            HOST_VK_HOOK("vkDestroyDevice", DestroyDevice);
            HOST_VK_HOOK("vkCreateWin32SurfaceKHR", CreateWin32SurfaceKHR);
            HOST_VK_HOOK("vkDestroySurfaceKHR", DestroySurfaceKHR);
            HOST_VK_HOOK("vkGetInstanceProcAddr", GetInstanceProcAddr);
            HOST_VK_HOOK("vkGetDeviceProcAddr", GetDeviceProcAddr);
            if (!instance)
                return nullptr;
            InstanceData* const data = FindInstance(instance);
            return data ? data->dispatch.GetInstanceProcAddr(instance, pName) : nullptr;
        }

#undef HOST_VK_HOOK
    }
}

// The loader's negotiation (loader interface 2), exported by
// RedemptionEditorHost.def as vkNegotiateLoaderLayerInterfaceVersion,
// vkGetInstanceProcAddr and vkGetDeviceProcAddr.
struct HostVkNegotiateLayerInterface
{
    int sType; // LAYER_NEGOTIATE_INTERFACE_STRUCT = 1
    void* pNext;
    uint32_t loaderLayerInterfaceVersion;
    PFN_vkGetInstanceProcAddr pfnGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr pfnGetDeviceProcAddr;
    PFN_vkGetInstanceProcAddr pfnGetPhysicalDeviceProcAddr;
};

extern "C" VkResult VKAPI_CALL HostVkNegotiateLoaderLayerInterfaceVersion(HostVkNegotiateLayerInterface* pVersionStruct)
{
    if (!pVersionStruct || pVersionStruct->sType != 1)
        return VK_ERROR_INITIALIZATION_FAILED;
    HOST_LOG("HOST_LAYER negotiate loader_interface=%u", pVersionStruct->loaderLayerInterfaceVersion);
    pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetInstanceProcAddr = host::vk::GetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = host::vk::GetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

extern "C" PFN_vkVoidFunction VKAPI_CALL HostVkGetInstanceProcAddr(VkInstance instance, const char* pName)
{
    return host::vk::GetInstanceProcAddr(instance, pName);
}

extern "C" PFN_vkVoidFunction VKAPI_CALL HostVkGetDeviceProcAddr(VkDevice device, const char* pName)
{
    return host::vk::GetDeviceProcAddr(device, pName);
}
