// Render export: this process's audio sessions muted during the video
// pass (export_audio_mute.h).

#include "export_audio_mute.h"

#include <windows.h>

#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <vector>

namespace replay::exportmute
{
    namespace
    {
        struct Held
        {
            ISimpleAudioVolume* volume = nullptr;
            BOOL wasMuted = FALSE;
        };

        std::string g_marker;
        std::vector<Held> g_held;
        bool g_muted = false;

        template <typename T>
        void SafeRelease(T*& p)
        {
            if (p)
            {
                p->Release();
                p = nullptr;
            }
        }

        // Calls `visit` with every session of this process on every active
        // render device.
        template <typename Visit>
        int ForEachSession(Visit visit)
        {
            // COM stays initialised on this thread (the held volume
            // interfaces outlive this call); RPC_E_CHANGED_MODE means it is
            // already up in another mode, which works as well.
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            int count = 0;
            IMMDeviceEnumerator* enumerator = nullptr;
            if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                    reinterpret_cast<void**>(&enumerator))))
            {
                IMMDeviceCollection* devices = nullptr;
                if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)))
                {
                    UINT deviceCount = 0;
                    devices->GetCount(&deviceCount);
                    for (UINT d = 0; d < deviceCount; ++d)
                    {
                        IMMDevice* device = nullptr;
                        IAudioSessionManager2* manager = nullptr;
                        IAudioSessionEnumerator* sessions = nullptr;
                        if (SUCCEEDED(devices->Item(d, &device)) &&
                            SUCCEEDED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&manager))) &&
                            SUCCEEDED(manager->GetSessionEnumerator(&sessions)))
                        {
                            int sessionCount = 0;
                            sessions->GetCount(&sessionCount);
                            for (int s = 0; s < sessionCount; ++s)
                            {
                                IAudioSessionControl* control = nullptr;
                                IAudioSessionControl2* control2 = nullptr;
                                if (SUCCEEDED(sessions->GetSession(s, &control)) &&
                                    SUCCEEDED(control->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&control2))))
                                {
                                    DWORD pid = 0;
                                    if (SUCCEEDED(control2->GetProcessId(&pid)) && pid == GetCurrentProcessId())
                                    {
                                        ISimpleAudioVolume* volume = nullptr;
                                        if (SUCCEEDED(control2->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&volume))))
                                        {
                                            visit(volume); // takes the reference
                                            ++count;
                                        }
                                    }
                                }
                                SafeRelease(control2);
                                SafeRelease(control);
                            }
                        }
                        SafeRelease(sessions);
                        SafeRelease(manager);
                        SafeRelease(device);
                    }
                    devices->Release();
                }
                enumerator->Release();
            }
            return count;
        }

        void WriteMarker()
        {
            if (g_marker.empty())
                return;
            HANDLE file = CreateFileA(g_marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
                CloseHandle(file);
        }
    }

    void Configure(const std::string& folder)
    {
        g_marker = folder.empty() ? std::string() : folder + "\\ReplayExport.muted";
        if (g_marker.empty() || GetFileAttributesA(g_marker.c_str()) == INVALID_FILE_ATTRIBUTES)
            return;
        // A previous run ended while muted (crash, kill): sound back on.
        ForEachSession([](ISimpleAudioVolume* volume) {
            volume->SetMute(FALSE, nullptr);
            volume->Release();
        });
        DeleteFileA(g_marker.c_str());
    }

    bool Mute(int& sessions)
    {
        sessions = 0;
        if (g_muted)
            return true;
        sessions = ForEachSession([](ISimpleAudioVolume* volume) {
            Held held;
            held.volume = volume;
            volume->GetMute(&held.wasMuted);
            volume->SetMute(TRUE, nullptr);
            g_held.push_back(held);
        });
        g_muted = sessions > 0;
        if (g_muted)
            WriteMarker();
        return g_muted;
    }

    void Restore()
    {
        if (!g_muted && g_held.empty())
            return;
        for (Held& held : g_held)
        {
            held.volume->SetMute(held.wasMuted, nullptr);
            held.volume->Release();
        }
        g_held.clear();
        g_muted = false;
        if (!g_marker.empty())
            DeleteFileA(g_marker.c_str());
    }

    bool Muted() { return g_muted; }
}
