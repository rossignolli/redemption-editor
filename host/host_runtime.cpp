// The host's effect_runtime and the per present order (host_runtime.h,
// host_backend.h). Everything here runs on the thread that presents.

#include "host_runtime.h"
#include "host_core_loader.h"
#include "host_input.h"
#include "host_log.h"
#include "host_owner.h"

#include "mod_identity.h"

#include <cstring>
#include <string>
#include <vector>

extern "C" IMAGE_DOS_HEADER __ImageBase;

// host_imgui_table.cpp: ReShade's own table code over the ImGui linked here.
const imgui_function_table* HostImGuiFunctionTable();

namespace host
{
    using namespace reshade::api;

    // ------------------------------------------------------------------
    // HostRuntime: what the editor uses
    // ------------------------------------------------------------------
    uint64_t HostRuntime::get_native() const
    {
        return m_backend ? reinterpret_cast<uint64_t>(m_backend->Native()) : 0;
    }

    void HostRuntime::get_private_data(const uint8_t guid[16], uint64_t* data) const
    {
        uint64_t key[2];
        std::memcpy(key, guid, sizeof(key));
        const auto it = m_privateData.find({key[0], key[1]});
        *data = it == m_privateData.end() ? 0 : it->second;
    }

    void HostRuntime::set_private_data(const uint8_t guid[16], const uint64_t data)
    {
        uint64_t key[2];
        std::memcpy(key, guid, sizeof(key));
        if (data)
            m_privateData[{key[0], key[1]}] = data;
        else
            m_privateData.erase({key[0], key[1]});
    }

    device* HostRuntime::get_device() { return m_backend ? m_backend->Device() : nullptr; }
    void* HostRuntime::get_hwnd() const { return m_backend ? m_backend->Window() : nullptr; }
    resource HostRuntime::get_back_buffer(uint32_t index) { return m_backend ? m_backend->BackBuffer(index) : resource{0}; }
    uint32_t HostRuntime::get_back_buffer_count() const { return m_backend ? m_backend->BackBufferCount() : 0; }
    uint32_t HostRuntime::get_current_back_buffer_index() const { return m_backend ? m_backend->BackBufferIndex() : 0; }
    command_queue* HostRuntime::get_command_queue() { return m_backend ? m_backend->Queue() : nullptr; }

    void HostRuntime::get_screenshot_width_and_height(uint32_t* out_width, uint32_t* out_height) const
    {
        uint32_t w = 0, h = 0;
        if (m_backend)
            m_backend->BackBufferSize(w, h);
        if (out_width)
            *out_width = w;
        if (out_height)
            *out_height = h;
    }

    // ReShade's runtime::get_texture_data (source/runtime.cpp, BSD-3-Clause,
    // Copyright (C) 2014 Patrick Mours), for the back buffer in its present
    // state: RGBA8 out, 10 bit quantized to 8, FP16 copied as is. Synchronous
    // (waits for the GPU), like ReShade's.
    bool HostRuntime::capture_screenshot(void* pixelsOut)
    {
        if (!m_backend || !pixelsOut)
            return false;
        device* const dev = m_backend->Device();
        command_queue* const queue = m_backend->Queue();
        const resource back = m_backend->CurrentBackBuffer();
        const resource_desc desc = dev->get_resource_desc(back);
        const format viewFormat = format_to_default_typed(desc.texture.format, 0);
        if (viewFormat != format::r8g8b8a8_unorm && viewFormat != format::b8g8r8a8_unorm &&
            viewFormat != format::r8g8b8x8_unorm && viewFormat != format::b8g8r8x8_unorm &&
            viewFormat != format::r10g10b10a2_unorm && viewFormat != format::b10g10r10a2_unorm &&
            viewFormat != format::r16g16b16a16_float)
        {
            HOST_ERROR("HOST_SCREENSHOT failed reason=format_%u", static_cast<unsigned>(desc.texture.format));
            return false;
        }
        resource intermediate{0};
        if (!dev->create_resource(resource_desc(desc.texture.width, desc.texture.height, 1, 1, viewFormat, 1, memory_heap::gpu_to_cpu,
                resource_usage::copy_dest), nullptr, resource_usage::copy_dest, &intermediate))
        {
            HOST_ERROR("HOST_SCREENSHOT failed reason=create_readback");
            return false;
        }
        command_list* const cmd = queue->get_immediate_command_list();
        cmd->barrier(back, resource_usage::present, resource_usage::copy_source);
        cmd->copy_texture_region(back, 0, nullptr, intermediate, 0, nullptr);
        cmd->barrier(back, resource_usage::copy_source, resource_usage::present);
        queue->wait_idle();

        bool ok = false;
        subresource_data mapped{};
        if (dev->map_texture_region(intermediate, 0, nullptr, map_access::read_only, &mapped))
        {
            auto* pixels = static_cast<uint8_t*>(pixelsOut);
            auto* source = static_cast<const uint8_t*>(mapped.data);
            const uint32_t pitch = format_row_pitch(viewFormat, desc.texture.width);
            for (uint32_t y = 0; y < desc.texture.height; ++y, pixels += pitch, source += mapped.row_pitch)
            {
                std::memcpy(pixels, source, pitch);
                switch (viewFormat)
                {
                case format::r8g8b8x8_unorm:
                    for (uint32_t x = 0; x < pitch; x += 4)
                        pixels[x + 3] = 0xFF;
                    break;
                case format::b8g8r8a8_unorm:
                case format::b8g8r8x8_unorm:
                    for (uint32_t x = 0; x < pitch; x += 4)
                    {
                        std::swap(pixels[x + 0], pixels[x + 2]);
                        if (viewFormat == format::b8g8r8x8_unorm)
                            pixels[x + 3] = 0xFF;
                    }
                    break;
                case format::r10g10b10a2_unorm:
                case format::b10g10r10a2_unorm:
                    for (uint32_t x = 0; x < pitch; x += 4)
                    {
                        uint32_t rgba;
                        std::memcpy(&rgba, source + x, 4);
                        pixels[x + 0] = static_cast<uint8_t>(((rgba & 0x000003FF) / 4) & 0xFF);
                        pixels[x + 1] = static_cast<uint8_t>((((rgba & 0x000FFC00) >> 10) / 4) & 0xFF);
                        pixels[x + 2] = static_cast<uint8_t>((((rgba & 0x3FF00000) >> 20) / 4) & 0xFF);
                        pixels[x + 3] = static_cast<uint8_t>((((rgba & 0xC0000000) >> 30) * 85) & 0xFF);
                        if (viewFormat == format::b10g10r10a2_unorm)
                            std::swap(pixels[x + 0], pixels[x + 2]);
                    }
                    break;
                default:
                    break;
                }
            }
            dev->unmap_texture_region(intermediate, 0);
            ok = true;
        }
        dev->destroy_resource(intermediate);
        return ok;
    }

    bool HostRuntime::is_key_down(uint32_t keycode) const { return input::KeyDown(keycode); }
    bool HostRuntime::is_key_pressed(uint32_t keycode) const { return input::KeyPressed(keycode); }
    bool HostRuntime::is_key_released(uint32_t keycode) const { return input::KeyReleased(keycode); }
    bool HostRuntime::is_mouse_button_down(uint32_t button) const { return input::MouseDown(static_cast<int>(button)); }
    bool HostRuntime::is_mouse_button_released(uint32_t button) const { return input::MouseReleased(static_cast<int>(button)); }

    bool HostRuntime::is_mouse_button_pressed(uint32_t button) const
    {
        // Not in host::input's API: the down edge from ImGui (fed by FeedImGui).
        return button < 5 && ImGui::IsMouseClicked(static_cast<ImGuiMouseButton>(button));
    }

    void HostRuntime::get_mouse_cursor_position(uint32_t* out_x, uint32_t* out_y, int16_t* out_wheel_delta) const
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (out_x)
            *out_x = io.MousePos.x > 0.0f ? static_cast<uint32_t>(io.MousePos.x) : 0;
        if (out_y)
            *out_y = io.MousePos.y > 0.0f ? static_cast<uint32_t>(io.MousePos.y) : 0;
        if (out_wheel_delta)
            *out_wheel_delta = static_cast<int16_t>(io.MouseWheel);
    }

    void HostRuntime::block_input_next_frame() { input::BlockNextFrame(true, true); }

    void HostRuntime::save_screenshot(const char*)
    {
        log::UnsupportedOnce("save_screenshot"); // lane host-extras (fpng from the readback)
    }

    // ------------------------------------------------------------------
    // HostRuntime: effects (no ReShade under the host)
    // ------------------------------------------------------------------
#define HOST_UNSUPPORTED_VOID(name) { log::UnsupportedOnce(name); }
#define HOST_UNSUPPORTED_RETURN(name, value) { log::UnsupportedOnce(name); return value; }

    void HostRuntime::render_effects(command_list*, resource_view, resource_view) HOST_UNSUPPORTED_VOID("render_effects")
    void HostRuntime::enumerate_uniform_variables(const char*, void (*)(effect_runtime*, effect_uniform_variable, void*), void*) HOST_UNSUPPORTED_VOID("enumerate_uniform_variables")
    effect_uniform_variable HostRuntime::find_uniform_variable(const char*, const char*) const HOST_UNSUPPORTED_RETURN("find_uniform_variable", effect_uniform_variable{0})
    void HostRuntime::get_uniform_variable_type(effect_uniform_variable, format*, uint32_t*, uint32_t*, uint32_t*) const HOST_UNSUPPORTED_VOID("get_uniform_variable_type")
    void HostRuntime::get_uniform_variable_name(effect_uniform_variable, char*, size_t* name_size) const
    {
        log::UnsupportedOnce("get_uniform_variable_name");
        if (name_size)
            *name_size = 0;
    }
    bool HostRuntime::get_annotation_bool_from_uniform_variable(effect_uniform_variable, const char*, bool*, size_t, size_t) const HOST_UNSUPPORTED_RETURN("get_annotation_bool_from_uniform_variable", false)
    bool HostRuntime::get_annotation_float_from_uniform_variable(effect_uniform_variable, const char*, float*, size_t, size_t) const HOST_UNSUPPORTED_RETURN("get_annotation_float_from_uniform_variable", false)
    bool HostRuntime::get_annotation_int_from_uniform_variable(effect_uniform_variable, const char*, int32_t*, size_t, size_t) const HOST_UNSUPPORTED_RETURN("get_annotation_int_from_uniform_variable", false)
    bool HostRuntime::get_annotation_uint_from_uniform_variable(effect_uniform_variable, const char*, uint32_t*, size_t, size_t) const HOST_UNSUPPORTED_RETURN("get_annotation_uint_from_uniform_variable", false)
    bool HostRuntime::get_annotation_string_from_uniform_variable(effect_uniform_variable, const char*, char*, size_t*) const HOST_UNSUPPORTED_RETURN("get_annotation_string_from_uniform_variable", false)
    void HostRuntime::get_uniform_value_bool(effect_uniform_variable, bool*, size_t, size_t) const HOST_UNSUPPORTED_VOID("get_uniform_value_bool")
    void HostRuntime::get_uniform_value_float(effect_uniform_variable, float*, size_t, size_t) const HOST_UNSUPPORTED_VOID("get_uniform_value_float")
    void HostRuntime::get_uniform_value_int(effect_uniform_variable, int32_t*, size_t, size_t) const HOST_UNSUPPORTED_VOID("get_uniform_value_int")
    void HostRuntime::get_uniform_value_uint(effect_uniform_variable, uint32_t*, size_t, size_t) const HOST_UNSUPPORTED_VOID("get_uniform_value_uint")
    void HostRuntime::set_uniform_value_bool(effect_uniform_variable, const bool*, size_t, size_t) HOST_UNSUPPORTED_VOID("set_uniform_value_bool")
    void HostRuntime::set_uniform_value_float(effect_uniform_variable, const float*, size_t, size_t) HOST_UNSUPPORTED_VOID("set_uniform_value_float")
    void HostRuntime::set_uniform_value_int(effect_uniform_variable, const int32_t*, size_t, size_t) HOST_UNSUPPORTED_VOID("set_uniform_value_int")
    void HostRuntime::set_uniform_value_uint(effect_uniform_variable, const uint32_t*, size_t, size_t) HOST_UNSUPPORTED_VOID("set_uniform_value_uint")
    void HostRuntime::enumerate_texture_variables(const char*, void (*)(effect_runtime*, effect_texture_variable, void*), void*) HOST_UNSUPPORTED_VOID("enumerate_texture_variables")
    effect_texture_variable HostRuntime::find_texture_variable(const char*, const char*) const HOST_UNSUPPORTED_RETURN("find_texture_variable", effect_texture_variable{0})
    void HostRuntime::get_texture_variable_name(effect_texture_variable, char*, size_t* name_size) const
    {
        log::UnsupportedOnce("get_texture_variable_name");
        if (name_size)
            *name_size = 0;
    }
    bool HostRuntime::get_annotation_bool_from_texture_variable(effect_texture_variable, const char*, bool*, size_t, size_t) const HOST_UNSUPPORTED_RETURN("get_annotation_bool_from_texture_variable", false)
    bool HostRuntime::get_annotation_float_from_texture_variable(effect_texture_variable, const char*, float*, size_t, size_t) const HOST_UNSUPPORTED_RETURN("get_annotation_float_from_texture_variable", false)
    bool HostRuntime::get_annotation_int_from_texture_variable(effect_texture_variable, const char*, int32_t*, size_t, size_t) const HOST_UNSUPPORTED_RETURN("get_annotation_int_from_texture_variable", false)
    bool HostRuntime::get_annotation_uint_from_texture_variable(effect_texture_variable, const char*, uint32_t*, size_t, size_t) const HOST_UNSUPPORTED_RETURN("get_annotation_uint_from_texture_variable", false)
    bool HostRuntime::get_annotation_string_from_texture_variable(effect_texture_variable, const char*, char*, size_t*) const HOST_UNSUPPORTED_RETURN("get_annotation_string_from_texture_variable", false)
    void HostRuntime::update_texture(effect_texture_variable, const uint32_t, const uint32_t, const void*) HOST_UNSUPPORTED_VOID("update_texture")
    void HostRuntime::get_texture_binding(effect_texture_variable, resource_view* out_srv, resource_view* out_srv_srgb) const
    {
        log::UnsupportedOnce("get_texture_binding");
        if (out_srv)
            *out_srv = resource_view{0};
        if (out_srv_srgb)
            *out_srv_srgb = resource_view{0};
    }
    void HostRuntime::update_texture_bindings(const char*, resource_view, resource_view) HOST_UNSUPPORTED_VOID("update_texture_bindings")
    void HostRuntime::enumerate_techniques(const char*, void (*)(effect_runtime*, effect_technique, void*), void*) HOST_UNSUPPORTED_VOID("enumerate_techniques")
    effect_technique HostRuntime::find_technique(const char*, const char*) HOST_UNSUPPORTED_RETURN("find_technique", effect_technique{0})
    void HostRuntime::get_technique_name(effect_technique, char*, size_t* name_size) const
    {
        log::UnsupportedOnce("get_technique_name");
        if (name_size)
            *name_size = 0;
    }
    bool HostRuntime::get_annotation_bool_from_technique(effect_technique, const char*, bool*, size_t, size_t) const HOST_UNSUPPORTED_RETURN("get_annotation_bool_from_technique", false)
    bool HostRuntime::get_annotation_float_from_technique(effect_technique, const char*, float*, size_t, size_t) const HOST_UNSUPPORTED_RETURN("get_annotation_float_from_technique", false)
    bool HostRuntime::get_annotation_int_from_technique(effect_technique, const char*, int32_t*, size_t, size_t) const HOST_UNSUPPORTED_RETURN("get_annotation_int_from_technique", false)
    bool HostRuntime::get_annotation_uint_from_technique(effect_technique, const char*, uint32_t*, size_t, size_t) const HOST_UNSUPPORTED_RETURN("get_annotation_uint_from_technique", false)
    bool HostRuntime::get_annotation_string_from_technique(effect_technique, const char*, char*, size_t*) const HOST_UNSUPPORTED_RETURN("get_annotation_string_from_technique", false)
    bool HostRuntime::get_technique_state(effect_technique) const HOST_UNSUPPORTED_RETURN("get_technique_state", false)
    void HostRuntime::set_technique_state(effect_technique, bool) HOST_UNSUPPORTED_VOID("set_technique_state")
    bool HostRuntime::get_preprocessor_definition(const char*, char*, size_t*) const HOST_UNSUPPORTED_RETURN("get_preprocessor_definition", false)
    void HostRuntime::set_preprocessor_definition(const char*, const char*) HOST_UNSUPPORTED_VOID("set_preprocessor_definition")
    void HostRuntime::render_technique(effect_technique, command_list*, resource_view, resource_view) HOST_UNSUPPORTED_VOID("render_technique")
    bool HostRuntime::get_effects_state() const HOST_UNSUPPORTED_RETURN("get_effects_state", false)
    void HostRuntime::set_effects_state(bool) HOST_UNSUPPORTED_VOID("set_effects_state")
    void HostRuntime::get_current_preset_path(char*, size_t* path_size) const
    {
        log::UnsupportedOnce("get_current_preset_path");
        if (path_size)
            *path_size = 0;
    }
    void HostRuntime::set_current_preset_path(const char*) HOST_UNSUPPORTED_VOID("set_current_preset_path")
    void HostRuntime::reorder_techniques(size_t, const effect_technique*) HOST_UNSUPPORTED_VOID("reorder_techniques")
    uint32_t HostRuntime::last_key_pressed() const HOST_UNSUPPORTED_RETURN("last_key_pressed", 0u)
    uint32_t HostRuntime::last_key_released() const HOST_UNSUPPORTED_RETURN("last_key_released", 0u)
    void HostRuntime::get_uniform_variable_effect_name(effect_uniform_variable, char*, size_t* size) const
    {
        log::UnsupportedOnce("get_uniform_variable_effect_name");
        if (size)
            *size = 0;
    }
    void HostRuntime::get_texture_variable_effect_name(effect_texture_variable, char*, size_t* size) const
    {
        log::UnsupportedOnce("get_texture_variable_effect_name");
        if (size)
            *size = 0;
    }
    void HostRuntime::get_technique_effect_name(effect_technique, char*, size_t* size) const
    {
        log::UnsupportedOnce("get_technique_effect_name");
        if (size)
            *size = 0;
    }
    void HostRuntime::save_current_preset() const HOST_UNSUPPORTED_VOID("save_current_preset")
    bool HostRuntime::get_preprocessor_definition_for_effect(const char*, const char*, char*, size_t*) const HOST_UNSUPPORTED_RETURN("get_preprocessor_definition_for_effect", false)
    void HostRuntime::set_preprocessor_definition_for_effect(const char*, const char*, const char*) HOST_UNSUPPORTED_VOID("set_preprocessor_definition_for_effect")
    bool HostRuntime::open_overlay(bool, input_source) HOST_UNSUPPORTED_RETURN("open_overlay", false)
    void HostRuntime::set_color_space(color_space) HOST_UNSUPPORTED_VOID("set_color_space")
    void HostRuntime::reset_uniform_value(effect_uniform_variable) HOST_UNSUPPORTED_VOID("reset_uniform_value")
    void HostRuntime::reload_effect_next_frame(const char*) HOST_UNSUPPORTED_VOID("reload_effect_next_frame")
    void HostRuntime::export_current_preset(const char*) const HOST_UNSUPPORTED_VOID("export_current_preset")

#undef HOST_UNSUPPORTED_VOID
#undef HOST_UNSUPPORTED_RETURN

    // ------------------------------------------------------------------
    // The per present order
    // ------------------------------------------------------------------
    namespace
    {
        enum class State
        {
            NotStarted,
            Running,
            Off, // refused or failed: the game runs on without the editor
        };

        State g_state = State::NotStarted;
        HostRuntime g_runtime;
        CoreLoader g_loader;
        ImGuiContext* g_imgui = nullptr;
        Backend* g_imguiBackend = nullptr; // the backend whose renderer holds the ImGui textures
        Backend* g_failedBackend = nullptr; // its UI could not be set up: no editor on it
        HWND g_inputWindow = nullptr;       // the window host::input serves
        HANDLE g_owner = nullptr;
        LARGE_INTEGER g_lastFrame{};
        LARGE_INTEGER g_frequency{};
        unsigned g_loggedGeneration = 0;
        bool g_firstPresentLogged = false;
#ifdef REDEMPTION_DEV
        bool g_testUi = false; // REDEMPTION_EDITOR_HOST_TEST_UI=1 (tools/host_layer_test)
#endif

        void LogCore(const char* text)
        {
            log::Write(log::Level::Info, text);
        }

        std::wstring Parent(const std::wstring& path)
        {
            const size_t slash = path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
        }

        // <game>\RedemptionEditor\host\RedemptionEditorHost.dll -> <game>\RedemptionEditorCore.dll
        std::wstring CorePath()
        {
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(reinterpret_cast<HMODULE>(&__ImageBase), path, MAX_PATH);
            const std::wstring game = Parent(Parent(Parent(path)));
            return game + L"\\" + replay::identity::kCoreFileW;
        }

        bool Start(Backend& backend)
        {
            // Host=native: RedemptionEditor.asi took the owner name in its DllMain
            // (before ReShade could load the add-on) on the host's behalf.
            wchar_t mode[32]{};
            GetEnvironmentVariableW(L"REDEMPTION_EDITOR_HOST_MODE", mode, 32);
            const bool native = _wcsicmp(mode, L"native") == 0;
            if (native)
                HOST_LOG("HOST_OWNER Host=native: owner held by RedemptionEditor.asi");
            else if (!TakeOwner(g_owner))
            {
                HOST_WARN("HOST_START refused reason=editor_owned_by_reshade_addon (RedemptionEditor.addon64 runs it in this process)");
                return false;
            }
            // Pinned: the Vulkan loader frees layer DLLs on vkDestroyInstance, and a
            // loaded core keeps pointers into this DLL (ImGui, the runtime).
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                reinterpret_cast<LPCWSTR>(&__ImageBase), &self);
            uint32_t w = 0, h = 0;
            backend.BackBufferSize(w, h);
            const std::wstring core = CorePath();
            // The build flavor is also the mark tools/release/package_release.py looks for in the DLL.
            HOST_LOG("HOST_RUNTIME start api=%u window=%p backbuffer=%ux%u fmt=%u core=%ls %s", static_cast<unsigned>(backend.Api()),
                backend.Window(), w, h, static_cast<unsigned>(backend.BackBufferFormat()), core.c_str(), replay::identity::kBuildFlavor);
            if (GetFileAttributesW(core.c_str()) == INVALID_FILE_ATTRIBUTES)
                HOST_WARN("HOST_CORE missing path=%ls (loaded when it appears)", core.c_str());

            if (!input::Install(backend.Window()))
                HOST_WARN("HOST_INPUT failed reason=install (the editor runs without keys)");
            g_inputWindow = backend.Window();

            g_imgui = ImGui::CreateContext();
            ImGui::SetCurrentContext(g_imgui);
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr; // nothing written next to the game
            io.LogFilename = nullptr;
            io.BackendPlatformName = "RedemptionEditorHost";
            io.Fonts->AddFontDefault();
            if (!backend.ImGuiInit())
            {
                HOST_ERROR("HOST_IMGUI failed reason=renderer_init");
                ImGui::DestroyContext(g_imgui);
                g_imgui = nullptr;
                input::Uninstall();
                return false;
            }
            g_imguiBackend = &backend;
            HOST_LOG("HOST_IMGUI ok version=%s renderer=%s", ImGui::GetVersion(), io.BackendRendererName ? io.BackendRendererName : "?");

            g_loader.Init(core, HostImGuiFunctionTable(),
                reinterpret_cast<HMODULE>(&__ImageBase), LogCore);
#ifdef REDEMPTION_DEV
            wchar_t testUi[4]{};
            g_testUi = GetEnvironmentVariableW(L"REDEMPTION_EDITOR_HOST_TEST_UI", testUi, 4) > 0 && testUi[0] == L'1';
#endif
            QueryPerformanceFrequency(&g_frequency);
            QueryPerformanceCounter(&g_lastFrame);
            return true;
        }
    }

    bool Active()
    {
        return g_state != State::Off;
    }

    void OnPresent(Backend& backend)
    {
        if (g_state == State::Off)
            return;
        if (g_state == State::NotStarted)
        {
            g_state = Start(backend) ? State::Running : State::Off;
            if (g_state == State::Off)
                return;
        }
        if (g_imguiBackend != &backend)
        {
            // A new swapchain (recreated on a resolution or mode change). A failure
            // leaves this swapchain without the editor (said once, not retried).
            if (g_failedBackend == &backend)
                return;
            if (!backend.ImGuiInit())
            {
                g_failedBackend = &backend;
                HOST_ERROR("HOST_IMGUI failed reason=renderer_init_on_new_swapchain (no editor on this swapchain)");
                return;
            }
            g_imguiBackend = &backend;
        }
        g_runtime.SetBackend(&backend);
        ImGui::SetCurrentContext(g_imgui);

        // The game may recreate its window with the swapchain (a display mode
        // change at startup: seen in game, 05:22:55, hwnd 3B0EF4 -> 3C0EF4): the
        // input follows the window being presented, or no key ever reaches the
        // editor again.
        if (backend.Window() && backend.Window() != g_inputWindow)
        {
            HOST_LOG("HOST_INPUT rebind old=%p new=%p (the game recreated its window)", static_cast<void*>(g_inputWindow),
                static_cast<void*>(backend.Window()));
            if (!input::Install(backend.Window()))
                HOST_WARN("HOST_INPUT failed reason=install_on_new_window (the editor runs without keys)");
            g_inputWindow = backend.Window();
        }

        input::BeginFrame();
        ImGuiIO& io = ImGui::GetIO();
        uint32_t w = 0, h = 0;
        backend.BackBufferSize(w, h);
        io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double delta = static_cast<double>(now.QuadPart - g_lastFrame.QuadPart) / static_cast<double>(g_frequency.QuadPart);
        g_lastFrame = now;
        io.DeltaTime = delta > 0.0 ? static_cast<float>(delta) : 1.0f / 60.0f;
        input::FeedImGui(io);
        // ReShade writes the flag at the start of every frame (true only while its
        // own window is open) and the core reads it as "ReShade's window is open"
        // (src/editor_cursor.h): left at the core's last true, it logged
        // owner=reshade with the editor closed.
        io.MouseDrawCursor = false;

        backend.ImGuiNewFrame();
        ImGui::NewFrame();
        // The core's overlay: the back buffer still without the UI (the export
        // copies it here), the editor's ImGui windows built.
        g_loader.Poll();
        if (g_loader.Loaded() && g_loader.Generation() != g_loggedGeneration)
        {
            g_loggedGeneration = g_loader.Generation();
            HOST_LOG("HOST_CORE loaded generation=%u", g_loggedGeneration);
        }
        g_loader.Overlay(&g_runtime);
#ifdef REDEMPTION_DEV
        // tools/host_layer_test: a window to see the renderer work (core or not).
        if (g_testUi)
        {
            ImGui::SetNextWindowPos(ImVec2(40.0f, 40.0f), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(480.0f, 220.0f), ImGuiCond_Always);
            ImGui::Begin("Redemption Editor host test");
            ImGui::Text("RedemptionEditorHost %ux%u frame %u", w, h, g_loader.Frame());
            ImGui::Button("A button", ImVec2(200.0f, 60.0f));
            ImGui::End();
        }
#endif
        ImGui::Render();
        backend.ImGuiRender(ImGui::GetDrawData());
        g_loader.Present(&g_runtime);

        if (!g_firstPresentLogged)
        {
            g_firstPresentLogged = true;
            HOST_LOG("HOST_PRESENT_FIRST backbuffer=%ux%u core=%s", w, h, g_loader.Loaded() ? "loaded" : "not_loaded");
        }
    }

    void OnBackendDestroyed(Backend& backend)
    {
        if (g_failedBackend == &backend)
            g_failedBackend = nullptr;
        if (g_state != State::Running)
            return;
        if (g_runtime.GetBackend() == &backend)
        {
            g_loader.DestroyRuntime(&g_runtime);
            g_runtime.SetBackend(nullptr);
        }
        if (g_imguiBackend == &backend)
        {
            ImGui::SetCurrentContext(g_imgui);
            backend.ImGuiShutdown();
            g_imguiBackend = nullptr;
        }
    }
}
