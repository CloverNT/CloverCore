#pragma once

#include <CloverNT/API/Macros.hpp>

#include <cstdint>
#include <utility>

namespace CloverNT::Event {

using SubscriptionId = std::uint64_t;
using ListenerId     = SubscriptionId;

inline constexpr SubscriptionId kInvalidSubscriptionId = 0;

class CloverNT_API ScopedSubscription final {
public:
    constexpr ScopedSubscription() noexcept = default;
    constexpr explicit ScopedSubscription(const SubscriptionId id) noexcept : mId(id) {}

    ScopedSubscription(const ScopedSubscription&)            = delete;
    ScopedSubscription& operator=(const ScopedSubscription&) = delete;

    constexpr ScopedSubscription(ScopedSubscription&& other) noexcept
        : mId(std::exchange(other.mId, kInvalidSubscriptionId)) {}

    auto operator=(ScopedSubscription&& other) noexcept -> ScopedSubscription& {
        if (this != &other) {
            unsubscribe();
            mId = std::exchange(other.mId, kInvalidSubscriptionId);
        }
        return *this;
    }

    ~ScopedSubscription() {
        unsubscribe();
    }

    [[nodiscard]] constexpr auto id() const noexcept -> SubscriptionId {
        return mId;
    }

    [[nodiscard]] constexpr bool valid() const noexcept {
        return mId != kInvalidSubscriptionId;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return valid();
    }

    bool unsubscribe() noexcept;

    constexpr auto release() noexcept -> SubscriptionId {
        return std::exchange(mId, kInvalidSubscriptionId);
    }

    auto detachToPlugin() noexcept -> SubscriptionId {
        return release();
    }

private:
    SubscriptionId mId{kInvalidSubscriptionId};
};

} // namespace CloverNT::Event
