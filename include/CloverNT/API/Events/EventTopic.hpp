#pragma once

#include <CloverNT/API/Reflect/TypeId.hpp>

#include <concepts>
#include <cstdint>
#include <string_view>

namespace CloverNT::Core::Modules {
class EventRuntime;
} // namespace CloverNT::Core::Modules

namespace CloverNT::Core::Event {
struct PacketBridgeAccess;
} // namespace CloverNT::Core::Event

namespace CloverNT::Core::Platform {
struct DllEventSourceAccess;
} // namespace CloverNT::Core::Platform

namespace CloverNT::Event {

enum class EventPriority : std::uint8_t {
    Lowest  = 0,
    Low     = 1,
    Normal  = 2,
    High    = 3,
    Highest = 4,
    Monitor = 5,
};

enum class DeliveryMode : std::uint8_t {
    Sync  = 0,
    Async = 1,
};

class ReadOnlyEventContext {
public:
    constexpr ReadOnlyEventContext(std::string_view topic,
                                   std::string_view sourcePlugin,
                                   EventPriority    priority,
                                   DeliveryMode     delivery,
                                   bool             cancellable,
                                   const bool*      cancelledFlag) noexcept
        : mTopic(topic),
          mSourcePlugin(sourcePlugin),
          mPriority(priority),
          mDelivery(delivery),
          mCancellable(cancellable),
          mCancelledFlag(cancelledFlag) {}

    [[nodiscard]] constexpr auto topic() const noexcept -> std::string_view {
        return mTopic;
    }

    [[nodiscard]] constexpr auto sourcePlugin() const noexcept -> std::string_view {
        return mSourcePlugin;
    }

    [[nodiscard]] constexpr auto priority() const noexcept -> EventPriority {
        return mPriority;
    }

    [[nodiscard]] constexpr auto delivery() const noexcept -> DeliveryMode {
        return mDelivery;
    }

    [[nodiscard]] constexpr bool isCancellable() const noexcept {
        return mCancellable && mDelivery == DeliveryMode::Sync;
    }

    [[nodiscard]] constexpr bool isCancelled() const noexcept {
        return mCancelledFlag != nullptr && *mCancelledFlag;
    }

protected:
    std::string_view mTopic;
    std::string_view mSourcePlugin;
    EventPriority    mPriority{EventPriority::Normal};
    DeliveryMode     mDelivery{DeliveryMode::Sync};
    bool             mCancellable{false};
    const bool*      mCancelledFlag{nullptr};
};

template <bool Cancellable>
class InterceptEventContext : public ReadOnlyEventContext {
public:
    using CancelCallback = bool (*)(void* state) noexcept;

    constexpr InterceptEventContext(std::string_view topic,
                                    std::string_view sourcePlugin,
                                    EventPriority    priority,
                                    DeliveryMode     delivery,
                                    bool             cancellable,
                                    const bool*      cancelledFlag,
                                    CancelCallback   cancelFn    = nullptr,
                                    void*            cancelState = nullptr) noexcept
        : ReadOnlyEventContext(topic, sourcePlugin, priority, delivery, cancellable, cancelledFlag),
          mCancelFn(cancelFn),
          mCancelState(cancelState) {}

    bool cancel() noexcept
        requires Cancellable
    {
        if (!isCancellable() || mCancelFn == nullptr) {
            return false;
        }
        return mCancelFn(mCancelState);
    }

private:
    CancelCallback mCancelFn{nullptr};
    void*          mCancelState{nullptr};
};

class KernelPublishToken final {
    constexpr KernelPublishToken() noexcept = default;

    friend class Core::Modules::EventRuntime;
    friend struct Core::Event::PacketBridgeAccess;
    friend struct Core::Platform::DllEventSourceAccess;
};

template <Reflect::FixedString Topic,
          class PayloadT,
          bool Mutable     = false,
          bool Cancellable = false,
          bool KernelOnly  = false>
struct EventTopic {
    using Payload                                   = PayloadT;
    static constexpr std::string_view topic         = Topic.view();
    static constexpr bool             isMutable     = Mutable;
    static constexpr bool             isCancellable = Cancellable;
    static constexpr bool             isKernelOnly  = KernelOnly;
};

namespace Detail {

    template <class T>
    concept IsEventTopic = requires {
        typename T::Payload;
        { T::topic } -> std::convertible_to<std::string_view>;
        { T::isMutable } -> std::convertible_to<bool>;
        { T::isCancellable } -> std::convertible_to<bool>;
        { T::isKernelOnly } -> std::convertible_to<bool>;
    };

} // namespace Detail

} // namespace CloverNT::Event
