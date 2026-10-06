#pragma once

#include <CloverNT/API/Macros.hpp>

#include <atomic>

namespace CloverNT::Event {

class CloverNT_API Cancellable {
public:
    [[nodiscard]] bool isCancelled() const noexcept;
    void               setCancelled(bool cancelled = true) noexcept;

private:
    std::atomic<bool> mCancelled{false};
};

} // namespace CloverNT::Event
