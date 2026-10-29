#include "editor_bridge.h"

#include <atomic>

namespace
{
    std::atomic_bool g_shuttingDown{false};
    std::atomic<replay::editor::SharedTransport*> g_transport{nullptr};
    HANDLE g_mapping = nullptr;
    std::uint64_t g_snapshotSequence = 0;

    replay::editor::SharedTransport* EnsureTransport() noexcept
    {
        replay::editor::SharedTransport* existing =
            g_transport.load(std::memory_order_acquire);
        if (existing || g_shuttingDown.load(std::memory_order_acquire))
            return existing;

        const std::uint32_t processId = GetCurrentProcessId();
        wchar_t mappingName[replay::editor::kSharedTransportNameCapacity]{};
        if (!replay::editor::FormatSharedTransportName(
                mappingName,
                replay::editor::kSharedTransportNameCapacity,
                processId))
        {
            return nullptr;
        }

        HANDLE mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0,
            static_cast<DWORD>(sizeof(replay::editor::SharedTransport)),
            mappingName);
        if (!mapping)
            return nullptr;

        void* view = MapViewOfFile(
            mapping,
            FILE_MAP_READ | FILE_MAP_WRITE,
            0,
            0,
            sizeof(replay::editor::SharedTransport));
        if (!view)
        {
            CloseHandle(mapping);
            return nullptr;
        }

        if (g_shuttingDown.load(std::memory_order_acquire))
        {
            UnmapViewOfFile(view);
            CloseHandle(mapping);
            return nullptr;
        }

        auto* transport =
            static_cast<replay::editor::SharedTransport*>(view);
        // Publish the offline mapping before initialization so a concurrent
        // loader-lock detach can always find and close the gate.
        replay::editor::SetSharedTransportOffline(*transport);
        g_mapping = mapping;
        g_transport.store(transport, std::memory_order_release);
        std::uint64_t epoch =
            GetTickCount64() ^
            (static_cast<std::uint64_t>(processId) << 32) ^
            reinterpret_cast<std::uintptr_t>(transport);
        if (epoch == 0)
            epoch = 1;
        replay::editor::InitializeSharedTransport(
            *transport,
            processId,
            epoch);
        if (g_shuttingDown.load(std::memory_order_acquire))
            replay::editor::SetSharedTransportOffline(*transport);
        return transport;
    }
}

namespace replay::editor
{
    void BeginShutdown() noexcept
    {
        g_shuttingDown.store(true, std::memory_order_release);
        SharedTransport* transport =
            g_transport.load(std::memory_order_acquire);
        if (transport)
            SetSharedTransportOffline(*transport);
    }

    void FinishShutdown() noexcept
    {
        SharedTransport* transport =
            g_transport.exchange(nullptr, std::memory_order_acq_rel);
        if (transport)
            UnmapViewOfFile(transport);
        if (g_mapping)
        {
            CloseHandle(g_mapping);
            g_mapping = nullptr;
        }
    }

    void Publish(const Snapshot& source)
    {
        if (g_shuttingDown.load(std::memory_order_acquire))
            return;
        SharedTransport* transport = EnsureTransport();
        if (!transport || g_shuttingDown.load(std::memory_order_acquire))
        {
            if (transport)
                SetSharedTransportOffline(*transport);
            return;
        }

        // Static, like the caller's (about 130 KB with the project); only the
        // script thread publishes.
        static Snapshot snapshot;
        snapshot = source;
        snapshot.sequence = ++g_snapshotSequence;
        PublishSharedSnapshot(
            *transport,
            snapshot,
            GetCurrentProcessId());
    }

    bool TryPopCommand(Command& command)
    {
        if (g_shuttingDown.load(std::memory_order_acquire))
            return false;
        SharedTransport* transport = EnsureTransport();
        return transport &&
            !g_shuttingDown.load(std::memory_order_acquire) &&
            TryPopSharedCommand(
            *transport,
            command,
            GetCurrentProcessId());
    }

    bool ConsumeQueueOverflowed()
    {
        SharedTransport* transport =
            g_transport.load(std::memory_order_acquire);
        return transport && ConsumeSharedCommandOverflow(*transport);
    }

    std::uint64_t OnlineEpoch() noexcept
    {
        SharedTransport* transport =
            g_transport.load(std::memory_order_acquire);
        if (!transport ||
            !SharedTransportOnline(*transport, GetCurrentProcessId()))
        {
            return 0;
        }
        return static_cast<std::uint64_t>(AtomicRead64(&transport->epoch));
    }
}
