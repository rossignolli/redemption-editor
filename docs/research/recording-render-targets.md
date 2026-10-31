# Named render targets and UI apps: engine model

Notes behind `src/render_target_core.h` and `src/render_target.cpp`, which
record and replay the named render targets (the camp ledger "ledger_ct" and
the UI app SHOP_BROWSING that draws into it). Build 1491.50, offline dump.

```text
Engine model (build 1491.50):
  register (name, bool) -> bool, the only callee of the
    REGISTER_NAMED_RENDERTARGET handler; owner = the calling
    script's id (the active script thread's +8); a static manager.
  link (model hash), the LINK_NAMED_RENDERTARGET handler
    jumps to it: marks the model info (+0xA4 |= 4, +0x98 = script id) and
    links the model's render target texture to the script's target.
  release (name) -> bool, the only callee of the
    RELEASE_NAMED_RENDERTARGET handler; same manager.
  UI app launch (const u32* app, const u32* entry) -> int, called
    by the LAUNCH_UIAPP* handlers (one with entry) and two engine
    helpers.
The link's target is not read (the texture name inside the model); a link
is filed with the name of the latest accepted register before it (the camp
scripts register then link at once; hypothesis for other scripts).
```
