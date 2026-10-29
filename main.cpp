/*
    Redemption Editor (RDR2 replay editor), the ScriptHookRDR2 script: RedemptionEditor.asi
    Built against the public Script Hook RDR2 SDK.
*/

#include "main.h"
#include "script.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    switch (reason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
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
