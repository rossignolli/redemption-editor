#pragma once

#include "editor_protocol.h"

#include <cstdint>

namespace replay::editor
{
    void BeginShutdown() noexcept;
    void FinishShutdown() noexcept;
    void Publish(const Snapshot& snapshot);
    bool TryPopCommand(Command& command);
    bool ConsumeQueueOverflowed();
    std::uint64_t OnlineEpoch() noexcept;
}
