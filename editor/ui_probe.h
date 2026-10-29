#pragma once

// The UI harness's probe. Built with REDEMPTION_UI_HARNESS, the editor tells
// the harness what it draws: every text with the pixels its glyphs cover, the
// lines of one paragraph as a group, the screen part drawing it
// (UI_PROBE_SCOPE) and the boxes the drawing alone does not name
// (UI_PROBE_BOX). In the add-on build every macro is empty.

#ifdef REDEMPTION_UI_HARNESS

#include <imgui.h>

namespace ui_probe
{
    struct Sink
    {
        virtual void Text(ImDrawList* draw, const char* begin, const char* end, ImVec2 inkMin, ImVec2 inkMax, float px, float capPx,
            const char* font) = 0;
        virtual void Box(const char* kind, ImVec2 min, ImVec2 max) = 0;
        virtual void PushScope(const char* name) = 0;
        virtual void PopScope() = 0;
        virtual void BeginGroup() = 0;
        virtual void EndGroup() = 0;
        // The font atlas (display_font.h): the harness's texture, as an ImTextureID.
        virtual ImU64 CreateTexture(const void* rgba, int width, int height) = 0;
        // The painted art (ui_art.h), with its mip chain.
        virtual ImU64 CreateTextureMips(const void* const* levels, int width, int height, int count) = 0;
    };

    inline Sink*& Current()
    {
        static Sink* sink = nullptr;
        return sink;
    }

    struct ScopeGuard
    {
        explicit ScopeGuard(const char* name)
        {
            if (Current())
                Current()->PushScope(name);
        }
        ~ScopeGuard()
        {
            if (Current())
                Current()->PopScope();
        }
        ScopeGuard(const ScopeGuard&) = delete;
        ScopeGuard& operator=(const ScopeGuard&) = delete;
    };

    struct GroupGuard
    {
        explicit GroupGuard(bool on) : m_on(on && Current() != nullptr)
        {
            if (m_on)
                Current()->BeginGroup();
        }
        ~GroupGuard()
        {
            if (m_on && Current())
                Current()->EndGroup();
        }
        GroupGuard(const GroupGuard&) = delete;
        GroupGuard& operator=(const GroupGuard&) = delete;

    private:
        bool m_on;
    };
}

#define UI_PROBE_JOIN2(a, b) a##b
#define UI_PROBE_JOIN(a, b) UI_PROBE_JOIN2(a, b)
#define UI_PROBE_SCOPE(name) const ::ui_probe::ScopeGuard UI_PROBE_JOIN(uiProbeScope, __LINE__)(name)
#define UI_PROBE_GROUP(on) const ::ui_probe::GroupGuard UI_PROBE_JOIN(uiProbeGroup, __LINE__)(on)
#define UI_PROBE_BOX(kind, min, max)                    \
    do                                                  \
    {                                                   \
        if (::ui_probe::Current())                      \
            ::ui_probe::Current()->Box(kind, min, max); \
    } while (0)

#else

#define UI_PROBE_SCOPE(name) ((void)0)
#define UI_PROBE_GROUP(on) ((void)0)
#define UI_PROBE_BOX(kind, min, max) ((void)0)

#endif
