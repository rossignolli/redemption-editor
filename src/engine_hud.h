#pragma once

// On-screen text through the engine's debug text and objective log.

namespace replay::hud
{
    void DrawText(const char* text, float x, float y, float scale, int r, int g, int b, int a);
    // The mod's development messages (debug mode only): the product speaks
    // through the editor.
    void SetObjectivesEnabled(bool enabled);
    void ShowObjective(const char* message);
    // Shown in both modes: what the player must act on.
    void ShowNotice(const char* message);
    // Render export: no objective or notice while the picture belongs to
    // the video (they would be burnt into it).
    void SetSuppressed(bool suppressed);
}
