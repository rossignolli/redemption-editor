#pragma once

// Render export: the audio.
//
//   A. RealtimePass (implemented): after the video pass the clip plays again
//      at 1x from the same start, and the game's own output is captured with
//      the WASAPI process loopback (Windows 10 2004+: only RDR2's audio, no
//      Discord or music). Every capture packet carries its QPC time; the probe
//      stamps the QPC of the tick the playhead left the start; the finalize
//      step cuts the WAV there and muxes it with the video copied bit for bit.
//      The sound is exactly the replay's sound; the cost is a second, real
//      time pass.
//   B. EngineLocked (not implemented): the game's mixer stepped by the export
//      clock. One pass, but needs the RDR2 mixer thread and its output found.
//   C. Offline mix from the recorded sound events: needs the game's decoders,
//      3D panning, reverb and mixer scenes. Not viable.
//
// No ReShade here: the offline audio test (tools/render_bench) links it.

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace render_export
{
    struct AudioFormat
    {
        std::uint32_t sampleRate = 48000;
        std::uint16_t channels = 2;
        std::uint16_t bitsPerSample = 32; // float
        bool isFloat = true;
    };

    // RIFF WAVE writer (sizes patched at Stop; below 4 GB: 48 kHz stereo
    // float is 1.3 GB per hour).
    class WavWriter
    {
    public:
        explicit WavWriter(std::wstring path) : m_path(std::move(path)) {}
        ~WavWriter() { Stop(); }
        bool Start(const AudioFormat& format);
        void Write(const void* frames, std::uint64_t count); // nullptr: silence
        bool Stop();
        std::uint64_t FramesWritten() const { return m_frames; }
        const AudioFormat& Format() const { return m_format; }

    private:
        std::wstring m_path;
        AudioFormat m_format;
        std::FILE* m_file = nullptr;
        std::uint64_t m_frames = 0;
    };

    // The audio of a process tree through the WASAPI process loopback
    // (ActivateAudioInterfaceAsync, AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK,
    // include the target's tree), 48 kHz float stereo, written to a WAV on a
    // capture thread. Gaps (no packet while the process is silent, or a
    // discontinuity) are filled with silence by the packets' QPC times, so
    // sample n of the file is at FirstQpc100ns() + n / 48000 s.
    class ProcessLoopbackCapture
    {
    public:
        ~ProcessLoopbackCapture() { Stop(); }
        bool Start(unsigned long processId, const std::wstring& wavPath, std::string& error);
        void Stop();
        bool Running() const { return m_running.load(); }
        // QPC of the file's first sample, in 100 ns units (WASAPI's unit).
        std::uint64_t FirstQpc100ns() const { return m_firstQpc100ns.load(); }
        std::uint64_t Frames() const { return m_frames.load(); }
        std::uint64_t Packets() const { return m_packets.load(); }
        std::uint64_t GapFrames() const { return m_gapFrames.load(); }
        const std::string& Error() const { return m_error; }

        // QPC counts to WASAPI's 100 ns units.
        static std::uint64_t Qpc100ns(std::uint64_t qpc);

    private:
        void Run(unsigned long processId);

        std::wstring m_path;
        std::thread m_thread;
        std::atomic<bool> m_running{false};
        std::atomic<bool> m_stop{false};
        std::atomic<bool> m_started{false};
        std::atomic<std::uint64_t> m_firstQpc100ns{0};
        std::atomic<std::uint64_t> m_frames{0};
        std::atomic<std::uint64_t> m_packets{0};
        std::atomic<std::uint64_t> m_gapFrames{0};
        HANDLE m_startedEvent = nullptr;
        std::string m_error;
    };
}
