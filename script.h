#pragma once

#include "enums.h"
#include "main.h"
#include "natives.h"
#include "types.h"

void ScriptMain();
void OnModuleDetach() noexcept;
void OnModuleDetachComplete() noexcept;
void OnKeyboardMessage(
    DWORD key,
    WORD repeats,
    BYTE scanCode,
    BOOL isExtended,
    BOOL isWithAlt,
    BOOL wasDownBefore,
    BOOL isUpNow);
