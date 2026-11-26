#pragma once

#include <CloverNT/API/Events/EventTopic.hpp>
#include <CloverNT/API/Events/Subscription.hpp>
#include <CloverNT/API/Expected.hpp>
#include <CloverNT/API/Macros.hpp>
#include <CloverNT/API/Reflect/BridgeValue.hpp>
#include <CloverNT/API/Reflect/Marshal.hpp>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace CloverNT::Event {

struct NativeArg {
    Reflect::TypeId typeId{};
    void*           ptr{};
    bool            isConst{true};
};

class EventInvocation final {
public:
    using MaterializerFn = std::function<Expected<std::vector<Reflect::BridgeValue>>(Reflect::MarshalContext&)>;
    using WriteBackFn = std::function<Expected<void>(std::span<const Reflect::BridgeValue>, Reflect::MarshalContext&)>;

    EventInvocation(const std::string_view           topic,
                    const std::string_view           sourcePlugin,
                    const bool                       cancellable,
                    const bool                       mutablePayload,
                    const std::span<const NativeArg> nativeArgs     = {},
                    MaterializerFn                   materializer   = {},
                    WriteBackFn                      writeBack      = {},
                    void*                            jsIsolate      = nullptr,
                    const void*                      jsFastArgs     = nullptr,
                    std::size_t                      jsFastArgCount = 0) noexcept
        : mTopic(topic),
          mSourcePlugin(sourcePlugin),
          mCancellable(cancellable),
          mMutablePayload(mutablePayload),
          mNativeArgs(nativeArgs),
          mMaterializer(std::move(materializer)),
          mWriteBack(std::move(writeBack)),
          mJsIsolate(jsIsolate),
          mJsFastArgs(jsFastArgs),
          mJsFastArgCount(jsFastArgCount) {}

    EventInvocation(const EventInvocation&)            = delete;
    EventInvocation& operator=(const EventInvocation&) = delete;

    [[nodiscard]] auto topic() const noexcept -> std::string_view {
        return mTopic;
    }

    [[nodiscard]] auto sourcePlugin() const noexcept -> std::string_view {
        return mSourcePlugin;
    }

    [[nodiscard]] bool isCancellable() const noexcept {
        return mCancellable;
    }

    [[nodiscard]] bool isMutablePayload() const noexcept {
        return mMutablePayload;
    }

    [[nodiscard]] bool isCancelled() const noexcept {
        return mCancelled;
    }

    bool cancel() noexcept {
        if (!mCancellable) {
            return false;
        }
        mCancelled = true;
        return true;
    }

    [[nodiscard]] auto nativeArgs() const noexcept -> std::span<const NativeArg> {
        return mNativeArgs;
    }

    [[nodiscard]] auto jsIsolate() const noexcept -> void* {
        return mJsIsolate;
    }

    [[nodiscard]] auto jsFastArgs() const noexcept -> const void* {
        return mJsFastArgs;
    }

    [[nodiscard]] auto jsFastArgCount() const noexcept -> std::size_t {
        return mJsFastArgCount;
    }

    [[nodiscard]] auto borrowLease() const noexcept -> const std::shared_ptr<Reflect::BorrowLease>& {
        return mBorrowScope.lease();
    }

    [[nodiscard]] auto makeReadOnlyContext(const EventPriority priority,
                                           const DeliveryMode  delivery = DeliveryMode::Sync) const noexcept
            -> ReadOnlyEventContext {
        return ReadOnlyEventContext{mTopic, mSourcePlugin, priority, delivery, mCancellable, &mCancelled};
    }

    template <bool Cancellable>
    [[nodiscard]] auto makeInterceptContext(const EventPriority priority,
                                            const DeliveryMode  delivery = DeliveryMode::Sync) noexcept
            -> InterceptEventContext<Cancellable> {
        return InterceptEventContext<Cancellable>{
                mTopic,
                mSourcePlugin,
                priority,
                delivery,
                mCancellable,
                &mCancelled,
                +[](void* state) noexcept -> bool { return static_cast<EventInvocation*>(state)->cancel(); },
                this};
    }

    [[nodiscard]] auto bridgeArgs() -> Expected<std::span<const Reflect::BridgeValue>> {
        if (!mCachedBridgeArgs.has_value()) {
            if (!mMaterializer) {
                mCachedBridgeArgs.emplace(std::vector<Reflect::BridgeValue>{});
            } else {
                Reflect::MarshalContext ctx{.activeLease = mBorrowScope.lease().get()};
                mCachedBridgeArgs.emplace(mMaterializer(ctx));
            }
        }
        if (!*mCachedBridgeArgs) {
            return unexpected(mCachedBridgeArgs->error());
        }
        return std::span<const Reflect::BridgeValue>{**mCachedBridgeArgs};
    }

    void invalidateBridgeCache() noexcept {
        mCachedBridgeArgs.reset();
        mJsFastArgs = nullptr;
    }

    [[nodiscard]] auto commitBridgeMutation(std::span<const Reflect::BridgeValue> updated) -> Expected<void> {
        if (!mMutablePayload) {
            return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ReadonlyViolation,
                                             "cannot mutate immutable event topic '" + std::string(mTopic) + "'");
        }
        mCachedBridgeArgs.emplace(std::vector(updated.begin(), updated.end()));
        mJsFastArgs = nullptr;
        if (mWriteBack) {
            Reflect::MarshalContext ctx{.activeLease = mBorrowScope.lease().get()};
            return mWriteBack(updated, ctx);
        }
        return {};
    }

private:
    std::string_view                                           mTopic;
    std::string_view                                           mSourcePlugin;
    bool                                                       mCancellable{false};
    bool                                                       mMutablePayload{false};
    bool                                                       mCancelled{false};
    std::span<const NativeArg>                                 mNativeArgs;
    MaterializerFn                                             mMaterializer;
    WriteBackFn                                                mWriteBack;
    void*                                                      mJsIsolate{nullptr};
    const void*                                                mJsFastArgs{nullptr};
    std::size_t                                                mJsFastArgCount{0};
    Reflect::BorrowScopeGuard                                  mBorrowScope;
    std::optional<Expected<std::vector<Reflect::BridgeValue>>> mCachedBridgeArgs;
};

class CloverNT_API ListenerSlot {
public:
    ListenerSlot(const SubscriptionId                     id,
                 const EventPriority                      priority,
                 const bool                               once,
                 std::string                              ownerPlugin,
                 std::shared_ptr<Reflect::PluginActivity> activity) noexcept
        : mId(id),
          mPriority(priority),
          mOnce(once),
          mOwnerPlugin(std::move(ownerPlugin)),
          mActivity(std::move(activity)) {}

    virtual ~ListenerSlot() = default;

    ListenerSlot(const ListenerSlot&)            = delete;
    ListenerSlot& operator=(const ListenerSlot&) = delete;

    [[nodiscard]] auto id() const noexcept -> SubscriptionId {
        return mId;
    }

    [[nodiscard]] auto priority() const noexcept -> EventPriority {
        return mPriority;
    }

    [[nodiscard]] bool isOnce() const noexcept {
        return mOnce;
    }

    [[nodiscard]] bool isConsumed() const noexcept {
        return mConsumed.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool tryConsumeOnce() noexcept {
        if (!mOnce) {
            return true;
        }
        return !mConsumed.exchange(true, std::memory_order_acq_rel);
    }

    [[nodiscard]] auto ownerPlugin() const noexcept -> std::string_view {
        return mOwnerPlugin;
    }

    [[nodiscard]] auto activity() const noexcept -> const std::shared_ptr<Reflect::PluginActivity>& {
        return mActivity;
    }

    virtual void dispatch(EventInvocation& inv) = 0;

private:
    SubscriptionId                           mId;
    EventPriority                            mPriority;
    bool                                     mOnce{false};
    std::atomic<bool>                        mConsumed{false};
    std::string                              mOwnerPlugin;
    std::shared_ptr<Reflect::PluginActivity> mActivity;
};

using ListenerSlotPtr = std::shared_ptr<ListenerSlot>;

} // namespace CloverNT::Event
