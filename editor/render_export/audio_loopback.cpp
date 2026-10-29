// Render export: WAV writer and the WASAPI process loopback capture
// (audio_sink.h). No ReShade: also linked by the offline audio test.

#include "audio_sink.h"
#include "thread_names.h"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cstring>

#pragma comment(lib, "Mmdevapi.lib")

namespace render_export
{
    namespace
    {
        void Put32(std::FILE* f, std::uint32_t v) { std::fwrite(&v, 4, 1, f); }
        void Put16(std::FILE* f, std::uint16_t v) { std::fwrite(&v, 2, 1, f); }

        // The activation completion (must be agile: it is called on a
        // worker thread of the audio service).
        class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject
        {
        public:
            ActivationHandler() { m_done = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
            ~ActivationHandler() { CloseHandle(m_done); }

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
            {
                if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler))
                    *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
                else if (riid == __uuidof(IAgileObject))
                    *out = static_cast<IAgileObject*>(this);
                else
                {
                    *out = nullptr;
                    return E_NOINTERFACE;
                }
                AddRef();
                return S_OK;
            }
            ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
            ULONG STDMETHODCALLTYPE Release() override
            {
                const ULONG refs = --m_refs;
                if (refs == 0)
                    delete this;
                return refs;
            }
            HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override
            {
                HRESULT result = E_FAIL;
                IUnknown* unknown = nullptr;
                if (SUCCEEDED(operation->GetActivateResult(&result, &unknown)) && SUCCEEDED(result) && unknown)
                    result = unknown->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&m_client));
                if (unknown)
                    unknown->Release();
                m_result = result;
                SetEvent(m_done);
                return S_OK;
            }

            HANDLE Done() const { return m_done; }
            HRESULT Result() const { return m_result; }
            IAudioClient* TakeClient()
            {
                IAudioClient* client = m_client;
                m_client = nullptr;
                return client;
            }

        private:
            std::atomic<ULONG> m_refs{1};
            HANDLE m_done = nullptr;
            HRESULT m_result = E_PENDING;
            IAudioClient* m_client = nullptr;
        };
    }

    // ------------------------------------------------------------------
    // WavWriter
    // ------------------------------------------------------------------

    bool WavWriter::Start(const AudioFormat& format)
    {
        m_format = format;
        m_frames = 0;
        if (_wfopen_s(&m_file, m_path.c_str(), L"wb") != 0 || !m_file)
            return false;
        const std::uint16_t block = static_cast<std::uint16_t>(format.channels * format.bitsPerSample / 8);
        std::fwrite("RIFF", 1, 4, m_file);
        Put32(m_file, 0); // patched at Stop
        std::fwrite("WAVEfmt ", 1, 8, m_file);
        Put32(m_file, 16);
        Put16(m_file, format.isFloat ? 3 : 1);
        Put16(m_file, format.channels);
        Put32(m_file, format.sampleRate);
        Put32(m_file, format.sampleRate * block);
        Put16(m_file, block);
        Put16(m_file, format.bitsPerSample);
        std::fwrite("data", 1, 4, m_file);
        Put32(m_file, 0); // patched at Stop
        return true;
    }

    void WavWriter::Write(const void* frames, std::uint64_t count)
    {
        if (!m_file || count == 0)
            return;
        const std::size_t block = static_cast<std::size_t>(m_format.channels) * m_format.bitsPerSample / 8;
        if (!frames)
        {
            static const std::uint8_t zeros[8192] = {};
            std::uint64_t left = count * block;
            while (left)
            {
                const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(left, sizeof(zeros)));
                std::fwrite(zeros, 1, n, m_file);
                left -= n;
            }
        }
        else
        {
            std::fwrite(frames, 1, static_cast<std::size_t>(count * block), m_file);
        }
        m_frames += count;
    }

    bool WavWriter::Stop()
    {
        if (!m_file)
            return false;
        const std::uint64_t dataBytes = m_frames * m_format.channels * m_format.bitsPerSample / 8;
        std::fseek(m_file, 4, SEEK_SET);
        Put32(m_file, static_cast<std::uint32_t>(36 + dataBytes));
        std::fseek(m_file, 40, SEEK_SET);
        Put32(m_file, static_cast<std::uint32_t>(dataBytes));
        std::fclose(m_file);
        m_file = nullptr;
        return true;
    }

    // ------------------------------------------------------------------
    // ProcessLoopbackCapture
    // ------------------------------------------------------------------

    std::uint64_t ProcessLoopbackCapture::Qpc100ns(std::uint64_t qpc)
    {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        const std::uint64_t f = static_cast<std::uint64_t>(frequency.QuadPart);
        return (qpc / f) * 10000000ull + (qpc % f) * 10000000ull / f;
    }

    bool ProcessLoopbackCapture::Start(unsigned long processId, const std::wstring& wavPath, std::string& error)
    {
        Stop();
        m_path = wavPath;
        m_error.clear();
        m_stop = false;
        m_started = false;
        m_firstQpc100ns = 0;
        m_frames = 0;
        m_packets = 0;
        m_gapFrames = 0;
        m_startedEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_running = true;
        m_thread = std::thread([this, processId] {
            replay::threads::Scope threadName("export_audio");
            Run(processId);
        });
        // Activation is asynchronous: wait for the client to start (or fail).
        WaitForSingleObject(m_startedEvent, 5000);
        CloseHandle(m_startedEvent);
        m_startedEvent = nullptr;
        if (!m_started.load())
        {
            Stop();
            error = m_error.empty() ? "loopback_start_timeout" : m_error;
            return false;
        }
        return true;
    }

    void ProcessLoopbackCapture::Stop()
    {
        m_stop = true;
        if (m_thread.joinable())
            m_thread.join();
        m_running = false;
    }

    void ProcessLoopbackCapture::Run(unsigned long processId)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto fail = [this](const char* what, HRESULT hr) {
            char text[96];
            std::snprintf(text, sizeof(text), "%s_hr_0x%08lX", what, static_cast<unsigned long>(hr));
            m_error = text;
            if (m_startedEvent)
                SetEvent(m_startedEvent);
        };

        AUDIOCLIENT_ACTIVATION_PARAMS params{};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.TargetProcessId = processId;
        params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
        PROPVARIANT variant{};
        variant.vt = VT_BLOB;
        variant.blob.cbSize = sizeof(params);
        variant.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

        auto* handler = new ActivationHandler();
        IActivateAudioInterfaceAsyncOperation* operation = nullptr;
        HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &variant, handler, &operation);
        IAudioClient* client = nullptr;
        if (SUCCEEDED(hr) && WaitForSingleObject(handler->Done(), 5000) == WAIT_OBJECT_0 && SUCCEEDED(handler->Result()))
            client = handler->TakeClient();
        else if (SUCCEEDED(hr))
            hr = handler->Result();
        if (operation)
            operation->Release();
        handler->Release();
        if (!client)
        {
            fail("loopback_activate", hr);
            CoUninitialize();
            m_running = false;
            return;
        }

        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 2;
        format.nSamplesPerSec = 48000;
        format.wBitsPerSample = 32;
        format.nBlockAlign = 8;
        format.nAvgBytesPerSec = 48000 * 8;
        HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        IAudioCaptureClient* capture = nullptr;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 2000000, 0, &format, nullptr);
        if (SUCCEEDED(hr))
            hr = client->SetEventHandle(ready);
        if (SUCCEEDED(hr))
            hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
        WavWriter wav(m_path);
        if (SUCCEEDED(hr) && !wav.Start(AudioFormat{}))
            hr = E_ACCESSDENIED;
        if (SUCCEEDED(hr))
            hr = client->Start();
        if (FAILED(hr))
        {
            fail("loopback_initialize", hr);
            if (capture)
                capture->Release();
            client->Release();
            CloseHandle(ready);
            CoUninitialize();
            m_running = false;
            return;
        }
        // Sample 0 of the file is "now": a stream that is silent at the start
        // gets its leading silence from the first packet's time.
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const std::uint64_t first = Qpc100ns(static_cast<std::uint64_t>(now.QuadPart));
        m_firstQpc100ns = first;
        m_started = true;
        if (m_startedEvent)
            SetEvent(m_startedEvent);

        std::uint64_t written = 0;
        while (!m_stop.load())
        {
            WaitForSingleObject(ready, 50);
            UINT32 packet = 0;
            while (SUCCEEDED(capture->GetNextPacketSize(&packet)) && packet > 0)
            {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                UINT64 devicePosition = 0;
                UINT64 qpcPosition = 0;
                if (FAILED(capture->GetBuffer(&data, &frames, &flags, &devicePosition, &qpcPosition)))
                    break;
                // Where this packet belongs in the file, by its time.
                if (qpcPosition > first)
                {
                    const std::uint64_t at = (qpcPosition - first) * 48000ull / 10000000ull;
                    if (at > written + 480) // more than 10 ms late: a gap, filled with silence
                    {
                        wav.Write(nullptr, at - written);
                        m_gapFrames += at - written;
                        written = at;
                    }
                }
                wav.Write((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : data, frames);
                written += frames;
                m_frames = written;
                ++m_packets;
                capture->ReleaseBuffer(frames);
            }
        }
        client->Stop();
        wav.Stop();
        capture->Release();
        client->Release();
        CloseHandle(ready);
        CoUninitialize();
        m_running = false;
    }
}
