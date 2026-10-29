#pragma once

// A JPEG file from 24-bit BGR rows, with the Windows Imaging Component
// (windowscodecs, part of Windows: no library ships). Worker threads only
// (it initialises COM on the calling thread, multithreaded, and leaves it as
// it found it).

#include <Windows.h>
#include <objbase.h>
#include <wincodec.h>

#include <cstdint>
#include <cstdio>
#include <string>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace replay::qa::jpeg
{
    // `bgr`: height rows of width * 3 bytes. quality 0..1.
    inline bool WriteBgr(const std::wstring& path, const std::uint8_t* bgr, std::uint32_t width, std::uint32_t height, float quality,
        std::string& error)
    {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool uninit = SUCCEEDED(init); // S_FALSE too: balanced by CoUninitialize
        if (FAILED(init) && init != RPC_E_CHANGED_MODE)
        {
            error = "coinit";
            return false;
        }
        IWICImagingFactory* factory = nullptr;
        IWICStream* stream = nullptr;
        IWICBitmapEncoder* encoder = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        IPropertyBag2* options = nullptr;
        bool ok = false;
        const auto step = [&](HRESULT hr, const char* what) {
            if (FAILED(hr))
            {
                char text[64];
                std::snprintf(text, sizeof(text), "%s_0x%08lx", what, static_cast<unsigned long>(hr));
                error = text;
                return false;
            }
            return true;
        };
        do
        {
            if (!step(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "factory"))
                break;
            if (!step(factory->CreateStream(&stream), "stream") ||
                !step(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "file"))
                break;
            if (!step(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder), "encoder") ||
                !step(encoder->Initialize(stream, WICBitmapEncoderNoCache), "encoder_init"))
                break;
            if (!step(encoder->CreateNewFrame(&frame, &options), "frame"))
                break;
            PROPBAG2 option{};
            option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT value;
            VariantInit(&value);
            value.vt = VT_R4;
            value.fltVal = quality < 0.0f ? 0.0f : (quality > 1.0f ? 1.0f : quality);
            options->Write(1, &option, &value);
            if (!step(frame->Initialize(options), "frame_init") || !step(frame->SetSize(width, height), "size"))
                break;
            WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
            if (!step(frame->SetPixelFormat(&format), "format"))
                break;
            if (format != GUID_WICPixelFormat24bppBGR)
            {
                error = "format_changed";
                break;
            }
            const UINT stride = width * 3;
            if (!step(frame->WritePixels(height, stride, stride * height, const_cast<BYTE*>(bgr)), "pixels") ||
                !step(frame->Commit(), "commit") || !step(encoder->Commit(), "encoder_commit"))
                break;
            ok = true;
        } while (false);
        if (options)
            options->Release();
        if (frame)
            frame->Release();
        if (encoder)
            encoder->Release();
        if (stream)
            stream->Release();
        if (factory)
            factory->Release();
        if (uninit)
            CoUninitialize();
        if (!ok)
            DeleteFileW(path.c_str());
        return ok;
    }
}
