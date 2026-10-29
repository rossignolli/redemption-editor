#include "engine_hud.h"

#include "natives.h"

namespace replay::hud
{
    void DrawText(const char* text, float x, float y, float scale, int r, int g, int b, int a)
    {
        const char* literal = MISC::VAR_STRING(10, "LITERAL_STRING", text);
        UIDEBUG::_BG_SET_TEXT_COLOR(r, g, b, a);
        UIDEBUG::_BG_SET_TEXT_SCALE(scale, scale);
        UIDEBUG::_BG_DISPLAY_TEXT(literal, x, y);
    }

    namespace
    {
        bool g_objectivesEnabled = true;
        bool g_suppressed = false; // render export
    }

    void SetSuppressed(bool suppressed) { g_suppressed = suppressed; }

    void SetObjectivesEnabled(bool enabled) { g_objectivesEnabled = enabled; }

    void ShowObjective(const char* message)
    {
        if (!g_objectivesEnabled)
            return;
        ShowNotice(message);
    }

    void ShowNotice(const char* message)
    {
        if (g_suppressed)
            return;
        const char* literal = MISC::VAR_STRING(10, "LITERAL_STRING", message);
        UILOG::_UILOG_SET_CACHED_OBJECTIVE(literal);
        UILOG::_UILOG_PRINT_CACHED_OBJECTIVE();
        UILOG::_UILOG_CLEAR_HAS_DISPLAYED_CACHED_OBJECTIVE();
        UILOG::_UILOG_CLEAR_CACHED_OBJECTIVE();
    }
}
