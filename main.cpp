/*
    Redemption Editor (RDR2 replay editor), the ScriptHookRDR2 script: RedemptionEditor.asi
    Built against the public Script Hook RDR2 SDK.
*/

#include "main.h"
#include "script.h"
#include "src/host_layer_env.h"
#include "host/d3d12/d3d12_asi_install.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        // The editor's own host as a Vulkan layer (before the game creates its
        // Vulkan instance; kernel32 only).
        replay::host_layer::Enable(module);
        // Its D3D12 path: a thread that loads the host once DllMain returns and
        // waits for the game's own d3d12.dll/dxgi.dll (nothing with Host=reshade).
        replay::host_d3d12::Start(module);
        scriptRegister(module, ScriptMain);
        keyboardHandlerRegister(OnKeyboardMessage);
        break;
    case DLL_PROCESS_DETACH:
        if (!reserved)
        {
            OnModuleDetach();
            keyboardHandlerUnregister(OnKeyboardMessage);
            scriptUnregister(module);
            OnModuleDetachComplete();
        }
        break;
    }

    return TRUE;
}
