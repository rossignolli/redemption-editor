// The UI harness's ImGui runtime (ui_harness_runtime.h).

#include "ui_harness_runtime.h"

#include <imgui.h>
#include <backends/imgui_impl_dx11.h>

#include <Windows.h>
#include <d3d11.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

// ReShade's own table code (external\reshade\source), as ReShade fills it
// for its add-ons.
#define RESHADE_API_LIBRARY_EXPORT 1
#define RESHADE_ADDON 1
#define RESHADE_GUI 1
#include "imgui_function_table_19222.cpp"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace
{
    ID3D11Device* g_device = nullptr;
    ID3D11DeviceContext* g_context = nullptr;
    ID3D11Texture2D* g_target = nullptr;
    ID3D11RenderTargetView* g_targetView = nullptr;
    ID3D11Texture2D* g_staging = nullptr;
    int g_width = 0;
    int g_height = 0;
    imgui_function_table_19222 g_table{};
    std::vector<ID3D11ShaderResourceView*> g_textures;

    template <typename T>
    void Release(T*& p)
    {
        if (p)
            p->Release();
        p = nullptr;
    }

    bool EnsureTarget(int width, int height)
    {
        if (g_target && width == g_width && height == g_height)
            return true;
        Release(g_targetView);
        Release(g_target);
        Release(g_staging);
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(width);
        desc.Height = static_cast<UINT>(height);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_target)))
            return false;
        if (FAILED(g_device->CreateRenderTargetView(g_target, nullptr, &g_targetView)))
            return false;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &g_staging)))
            return false;
        g_width = width;
        g_height = height;
        return true;
    }

    void Error(char* error, int size, const char* text)
    {
        if (error && size > 0)
            std::snprintf(error, static_cast<std::size_t>(size), "%s", text);
    }
}

UI_HARNESS_RUNTIME_API bool UiHarnessInit(char* error, int errorSize)
{
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 1, D3D11_SDK_VERSION,
            &g_device, nullptr, &g_context)))
    {
        Error(error, errorSize, "D3D11CreateDevice(WARP) failed");
        return false;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.Fonts->AddFontDefault();
    if (!ImGui_ImplDX11_Init(g_device, g_context))
    {
        Error(error, errorSize, "ImGui_ImplDX11_Init failed");
        return false;
    }
    g_table = init_imgui_function_table_19222();
    return true;
}

UI_HARNESS_RUNTIME_API void UiHarnessShutdown()
{
    for (ID3D11ShaderResourceView*& view : g_textures)
        Release(view);
    g_textures.clear();
    if (ImGui::GetCurrentContext())
    {
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext();
    }
    Release(g_targetView);
    Release(g_target);
    Release(g_staging);
    Release(g_context);
    Release(g_device);
}

UI_HARNESS_RUNTIME_API const void* UiHarnessTable() { return &g_table; }

UI_HARNESS_RUNTIME_API std::uint64_t UiHarnessCreateTexture(const void* rgba, int width, int height)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem = rgba;
    data.SysMemPitch = static_cast<UINT>(width * 4);
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(g_device->CreateTexture2D(&desc, &data, &texture)))
        return 0;
    ID3D11ShaderResourceView* view = nullptr;
    const HRESULT hr = g_device->CreateShaderResourceView(texture, nullptr, &view);
    texture->Release();
    if (FAILED(hr))
        return 0;
    g_textures.push_back(view);
    return reinterpret_cast<std::uint64_t>(view);
}

UI_HARNESS_RUNTIME_API std::uint64_t UiHarnessCreateTextureMips(const void* const* levels, int width, int height, int count)
{
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = static_cast<UINT>(count);
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<D3D11_SUBRESOURCE_DATA> data(static_cast<std::size_t>(count));
    int w = width;
    for (int n = 0; n < count; ++n)
    {
        data[n].pSysMem = levels[n];
        data[n].SysMemPitch = static_cast<UINT>(w * 4);
        w = w > 1 ? w / 2 : 1;
    }
    ID3D11Texture2D* texture = nullptr;
    if (FAILED(g_device->CreateTexture2D(&desc, data.data(), &texture)))
        return 0;
    ID3D11ShaderResourceView* view = nullptr;
    const HRESULT hr = g_device->CreateShaderResourceView(texture, nullptr, &view);
    texture->Release();
    if (FAILED(hr))
        return 0;
    g_textures.push_back(view);
    return reinterpret_cast<std::uint64_t>(view);
}

namespace
{
    // UiHarnessInput: the keys held now, every key ever held (to release), the button.
    std::vector<int> g_keysDown;
    std::vector<int> g_keysSeen;
    bool g_mouseDown = false;
    bool g_mouseSeen = false;
}

UI_HARNESS_RUNTIME_API void UiHarnessInput(const int* keysDown, int count, bool mouseDown)
{
    g_keysDown.assign(keysDown, keysDown + (count > 0 ? count : 0));
    for (int key : g_keysDown)
        if (std::find(g_keysSeen.begin(), g_keysSeen.end(), key) == g_keysSeen.end())
            g_keysSeen.push_back(key);
    g_mouseDown = mouseDown;
    g_mouseSeen = g_mouseSeen || mouseDown;
}

UI_HARNESS_RUNTIME_API void UiHarnessNewFrame(int width, int height, double timeSeconds, float mouseX, float mouseY)
{
    EnsureTarget(width, height);
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    io.DeltaTime = 1.0f / 60.0f;
    (void)timeSeconds;
    io.AddMousePosEvent(mouseX, mouseY);
    for (int key : g_keysSeen)
        io.AddKeyEvent(static_cast<ImGuiKey>(key), std::find(g_keysDown.begin(), g_keysDown.end(), key) != g_keysDown.end());
    if (g_mouseSeen)
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, g_mouseDown);
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    // The game's picture stand-in: a dusk sky over dark ground, mid tones
    // (the editor's panels are translucent over the game).
    ImDrawList* back = ImGui::GetBackgroundDrawList();
    const float w = static_cast<float>(width), h = static_cast<float>(height);
    back->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), ImVec2(w, h * 0.55f), IM_COL32(92, 110, 132, 255), IM_COL32(92, 110, 132, 255),
        IM_COL32(176, 150, 118, 255), IM_COL32(176, 150, 118, 255));
    back->AddRectFilledMultiColor(ImVec2(0.0f, h * 0.55f), ImVec2(w, h), IM_COL32(88, 74, 56, 255), IM_COL32(88, 74, 56, 255),
        IM_COL32(46, 40, 32, 255), IM_COL32(46, 40, 32, 255));
}

UI_HARNESS_RUNTIME_API bool UiHarnessEndFrame(bool render, std::uint8_t* rgba)
{
    ImGui::Render();
    if (!render)
        return true;
    const float clear[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    g_context->OMSetRenderTargets(1, &g_targetView, nullptr);
    g_context->ClearRenderTargetView(g_targetView, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (!rgba)
        return true;
    g_context->CopyResource(g_staging, g_target);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_context->Map(g_staging, 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    for (int y = 0; y < g_height; ++y)
        std::memcpy(rgba + static_cast<std::size_t>(y) * g_width * 4, static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch,
            static_cast<std::size_t>(g_width) * 4);
    g_context->Unmap(g_staging, 0);
    // Opaque like the backdrop: the PNG has no alpha.
    return true;
}

UI_HARNESS_RUNTIME_API bool UiHarnessSavePng(const wchar_t* path, const std::uint8_t* rgba, int width, int height)
{
    static bool comReady = false;
    if (!comReady)
    {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        comReady = SUCCEEDED(co) || co == RPC_E_CHANGED_MODE;
    }
    std::vector<std::uint8_t> bgr(static_cast<std::size_t>(width) * height * 3);
    for (std::size_t n = 0, count = static_cast<std::size_t>(width) * height; n < count; ++n)
    {
        bgr[n * 3 + 0] = rgba[n * 4 + 2];
        bgr[n * 3 + 1] = rgba[n * 4 + 1];
        bgr[n * 3 + 2] = rgba[n * 4 + 0];
    }
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
        SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)));
    if (!ok)
    {
        std::fprintf(stderr, "UiHarnessSavePng: WIC setup failed\n");
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    ok = ok && SUCCEEDED(frame->SetPixelFormat(&format)) && IsEqualGUID(format, GUID_WICPixelFormat24bppBGR) &&
        SUCCEEDED(frame->WritePixels(static_cast<UINT>(height), static_cast<UINT>(width) * 3, static_cast<UINT>(bgr.size()), bgr.data())) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    Release(frame);
    Release(encoder);
    Release(stream);
    Release(factory);
    return ok;
}
