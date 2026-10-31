// RedemptionEditorHost.dll: the Redemption Editor without ReShade.
// Loaded by the Vulkan loader as the layer VK_LAYER_redemption_editor
// (host/RedemptionEditorHost.json; RedemptionEditor.asi adds the manifest's
// folder to VK_ADD_LAYER_PATH and the name to VK_LOADER_LAYERS_ENABLE in its
// DllMain). The layer entry points are in host/vk/vk_layer.cpp and exported by
// host/RedemptionEditorHost.def. Nothing is done under the loader lock here:
// the work starts in vkCreateInstance and on the first present.

#include <Windows.h>

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(module);
    return TRUE;
}
