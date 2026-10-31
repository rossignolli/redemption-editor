#pragma once

// The effect_runtime the core sees under the host (no ReShade): the API
// independent part (window, keys through host::input, screenshot readback,
// block_input_next_frame) over the Backend of the swapchain being presented.
// Only what the editor uses is implemented; every other method returns a
// failure or an empty value and logs HOST_UNSUPPORTED <name> once.

#include "host_backend.h"

#include <cstdint>
#include <map>
#include <utility>

namespace host
{
    class HostRuntime final : public reshade::api::effect_runtime
    {
    public:
        void SetBackend(Backend* backend) { m_backend = backend; }
        Backend* GetBackend() const { return m_backend; }

        // api_object
        uint64_t get_native() const override;
        void get_private_data(const uint8_t guid[16], uint64_t* data) const override;
        void set_private_data(const uint8_t guid[16], const uint64_t data) override;
        // device_object
        reshade::api::device* get_device() override;

        // effect_runtime: what the editor uses
        void* get_hwnd() const override;
        reshade::api::resource get_back_buffer(uint32_t index) override;
        uint32_t get_back_buffer_count() const override;
        uint32_t get_current_back_buffer_index() const override;
        reshade::api::command_queue* get_command_queue() override;
        bool capture_screenshot(void* pixels) override;
        void get_screenshot_width_and_height(uint32_t* out_width, uint32_t* out_height) const override;
        bool is_key_down(uint32_t keycode) const override;
        bool is_key_pressed(uint32_t keycode) const override;
        bool is_key_released(uint32_t keycode) const override;
        bool is_mouse_button_down(uint32_t button) const override;
        bool is_mouse_button_pressed(uint32_t button) const override;
        bool is_mouse_button_released(uint32_t button) const override;
        void get_mouse_cursor_position(uint32_t* out_x, uint32_t* out_y, int16_t* out_wheel_delta = nullptr) const override;
        void block_input_next_frame() override;
        void save_screenshot(const char* postfix = nullptr) override;

        // effect_runtime: effects (no ReShade, nothing to report)
        void render_effects(reshade::api::command_list* cmd_list, reshade::api::resource_view rtv, reshade::api::resource_view rtv_srgb) override;
        void enumerate_uniform_variables(const char* effect_name, void (*callback)(effect_runtime* runtime, reshade::api::effect_uniform_variable variable, void* user_data), void* user_data) override;
        reshade::api::effect_uniform_variable find_uniform_variable(const char* effect_name, const char* variable_name) const override;
        void get_uniform_variable_type(reshade::api::effect_uniform_variable variable, reshade::api::format* out_base_type, uint32_t* out_rows = nullptr, uint32_t* out_columns = nullptr, uint32_t* out_array_length = nullptr) const override;
        void get_uniform_variable_name(reshade::api::effect_uniform_variable variable, char* name, size_t* name_size) const override;
        bool get_annotation_bool_from_uniform_variable(reshade::api::effect_uniform_variable variable, const char* name, bool* values, size_t count, size_t array_index = 0) const override;
        bool get_annotation_float_from_uniform_variable(reshade::api::effect_uniform_variable variable, const char* name, float* values, size_t count, size_t array_index = 0) const override;
        bool get_annotation_int_from_uniform_variable(reshade::api::effect_uniform_variable variable, const char* name, int32_t* values, size_t count, size_t array_index = 0) const override;
        bool get_annotation_uint_from_uniform_variable(reshade::api::effect_uniform_variable variable, const char* name, uint32_t* values, size_t count, size_t array_index = 0) const override;
        bool get_annotation_string_from_uniform_variable(reshade::api::effect_uniform_variable variable, const char* name, char* value, size_t* value_size) const override;
        void get_uniform_value_bool(reshade::api::effect_uniform_variable variable, bool* values, size_t count, size_t array_index = 0) const override;
        void get_uniform_value_float(reshade::api::effect_uniform_variable variable, float* values, size_t count, size_t array_index = 0) const override;
        void get_uniform_value_int(reshade::api::effect_uniform_variable variable, int32_t* values, size_t count, size_t array_index = 0) const override;
        void get_uniform_value_uint(reshade::api::effect_uniform_variable variable, uint32_t* values, size_t count, size_t array_index = 0) const override;
        void set_uniform_value_bool(reshade::api::effect_uniform_variable variable, const bool* values, size_t count, size_t array_index = 0) override;
        void set_uniform_value_float(reshade::api::effect_uniform_variable variable, const float* values, size_t count, size_t array_index = 0) override;
        void set_uniform_value_int(reshade::api::effect_uniform_variable variable, const int32_t* values, size_t count, size_t array_index = 0) override;
        void set_uniform_value_uint(reshade::api::effect_uniform_variable variable, const uint32_t* values, size_t count, size_t array_index = 0) override;
        void enumerate_texture_variables(const char* effect_name, void (*callback)(effect_runtime* runtime, reshade::api::effect_texture_variable variable, void* user_data), void* user_data) override;
        reshade::api::effect_texture_variable find_texture_variable(const char* effect_name, const char* variable_name) const override;
        void get_texture_variable_name(reshade::api::effect_texture_variable variable, char* name, size_t* name_size) const override;
        bool get_annotation_bool_from_texture_variable(reshade::api::effect_texture_variable variable, const char* name, bool* values, size_t count, size_t array_index = 0) const override;
        bool get_annotation_float_from_texture_variable(reshade::api::effect_texture_variable variable, const char* name, float* values, size_t count, size_t array_index = 0) const override;
        bool get_annotation_int_from_texture_variable(reshade::api::effect_texture_variable variable, const char* name, int32_t* values, size_t count, size_t array_index = 0) const override;
        bool get_annotation_uint_from_texture_variable(reshade::api::effect_texture_variable variable, const char* name, uint32_t* values, size_t count, size_t array_index = 0) const override;
        bool get_annotation_string_from_texture_variable(reshade::api::effect_texture_variable variable, const char* name, char* value, size_t* value_size) const override;
        void update_texture(reshade::api::effect_texture_variable variable, const uint32_t width, const uint32_t height, const void* pixels) override;
        void get_texture_binding(reshade::api::effect_texture_variable variable, reshade::api::resource_view* out_srv, reshade::api::resource_view* out_srv_srgb) const override;
        void update_texture_bindings(const char* semantic, reshade::api::resource_view srv, reshade::api::resource_view srv_srgb) override;
        void enumerate_techniques(const char* effect_name, void (*callback)(effect_runtime* runtime, reshade::api::effect_technique technique, void* user_data), void* user_data) override;
        reshade::api::effect_technique find_technique(const char* effect_name, const char* technique_name) override;
        void get_technique_name(reshade::api::effect_technique technique, char* name, size_t* name_size) const override;
        bool get_annotation_bool_from_technique(reshade::api::effect_technique technique, const char* name, bool* values, size_t count, size_t array_index = 0) const override;
        bool get_annotation_float_from_technique(reshade::api::effect_technique technique, const char* name, float* values, size_t count, size_t array_index = 0) const override;
        bool get_annotation_int_from_technique(reshade::api::effect_technique technique, const char* name, int32_t* values, size_t count, size_t array_index = 0) const override;
        bool get_annotation_uint_from_technique(reshade::api::effect_technique technique, const char* name, uint32_t* values, size_t count, size_t array_index = 0) const override;
        bool get_annotation_string_from_technique(reshade::api::effect_technique technique, const char* name, char* value, size_t* value_size) const override;
        bool get_technique_state(reshade::api::effect_technique technique) const override;
        void set_technique_state(reshade::api::effect_technique technique, bool enabled) override;
        bool get_preprocessor_definition(const char* name, char* value, size_t* value_size) const override;
        void set_preprocessor_definition(const char* name, const char* value) override;
        void render_technique(reshade::api::effect_technique technique, reshade::api::command_list* cmd_list, reshade::api::resource_view rtv, reshade::api::resource_view rtv_srgb = {0}) override;
        bool get_effects_state() const override;
        void set_effects_state(bool enabled) override;
        void get_current_preset_path(char* path, size_t* path_size) const override;
        void set_current_preset_path(const char* path) override;
        void reorder_techniques(size_t count, const reshade::api::effect_technique* techniques) override;
        uint32_t last_key_pressed() const override;
        uint32_t last_key_released() const override;
        void get_uniform_variable_effect_name(reshade::api::effect_uniform_variable variable, char* effect_name, size_t* effect_name_size) const override;
        void get_texture_variable_effect_name(reshade::api::effect_texture_variable variable, char* effect_name, size_t* effect_name_size) const override;
        void get_technique_effect_name(reshade::api::effect_technique technique, char* effect_name, size_t* effect_name_size) const override;
        void save_current_preset() const override;
        bool get_preprocessor_definition_for_effect(const char* effect_name, const char* name, char* value, size_t* value_size) const override;
        void set_preprocessor_definition_for_effect(const char* effect_name, const char* name, const char* value) override;
        bool open_overlay(bool open, reshade::api::input_source source) override;
        void set_color_space(reshade::api::color_space color_space) override;
        void reset_uniform_value(reshade::api::effect_uniform_variable variable) override;
        void reload_effect_next_frame(const char* effect_name) override;
        void export_current_preset(const char* path) const override;

    private:
        Backend* m_backend = nullptr;
        std::map<std::pair<uint64_t, uint64_t>, uint64_t> m_privateData;
    };
}
