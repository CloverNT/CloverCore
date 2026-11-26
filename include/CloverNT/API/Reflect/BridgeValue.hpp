#pragma once

#include <CloverNT/API/Expected.hpp>
#include <CloverNT/API/Macros.hpp>
#include <CloverNT/API/Reflect/Aggregate.hpp>
#include <CloverNT/API/Reflect/TypeId.hpp>

#include <atomic>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace CloverNT::Reflect {

enum class BridgeErrorCode : std::int32_t {
    TypeMismatch = 1000,
    ArityMismatch,
    MissingField,
    IntegerOverflow,
    EnumOutOfRange,
    CircularReference,
    DepthExceeded,
    ObjectRevoked,
    BorrowExpired,
    ReadonlyViolation,
    CrossThreadSyncCallForbidden,
    PromiseReturnedToSyncCaller,
    PermissionDenied,
    MemberNotFound,
    ExecutionFailed,
};

[[nodiscard]] inline auto bridgeError(const BridgeErrorCode code, std::string message) -> Error {
    return makeError(ErrorCategory::Event, std::move(message), static_cast<std::int32_t>(code));
}

[[nodiscard]] inline auto bridgeUnexpected(const BridgeErrorCode code, std::string message) -> std::unexpected<Error> {
    return unexpected(bridgeError(code, std::move(message)));
}

class CloverNT_API PluginActivity final {
public:
    class Guard final {
    public:
        Guard() noexcept = default;
        explicit Guard(PluginActivity* activity) noexcept : mActivity(activity) {}

        Guard(const Guard&)            = delete;
        Guard& operator=(const Guard&) = delete;

        Guard(Guard&& other) noexcept : mActivity(std::exchange(other.mActivity, nullptr)) {}

        auto operator=(Guard&& other) noexcept -> Guard& {
            if (this != &other) {
                release();
                mActivity = std::exchange(other.mActivity, nullptr);
            }
            return *this;
        }

        ~Guard() {
            release();
        }

        [[nodiscard]] explicit operator bool() const noexcept {
            return mActivity != nullptr;
        }

    private:
        void release() noexcept {
            if (mActivity != nullptr) {
                mActivity->leave();
                mActivity = nullptr;
            }
        }

        PluginActivity* mActivity{};
    };

    explicit PluginActivity(std::string pluginName) noexcept : mPluginName(std::move(pluginName)) {}
    PluginActivity(const PluginActivity&)            = delete;
    PluginActivity& operator=(const PluginActivity&) = delete;

    [[nodiscard]] auto pluginName() const noexcept -> std::string_view {
        return mPluginName;
    }

    [[nodiscard]] bool isClosing() const noexcept {
        return mClosing.load(std::memory_order_acquire);
    }

    [[nodiscard]] auto tryEnter() noexcept -> Guard {
        if (mClosing.load(std::memory_order_acquire)) {
            return Guard{};
        }
        mInFlight.fetch_add(1, std::memory_order_seq_cst);
        if (mClosing.load(std::memory_order_seq_cst)) {
            leave();
            return Guard{};
        }
        return Guard{this};
    }

    void beginClose() noexcept {
        mClosing.store(true, std::memory_order_seq_cst);
    }

    void reopen() noexcept {
        mClosing.store(false, std::memory_order_seq_cst);
    }

    [[nodiscard]] auto inFlight() const noexcept -> std::uint32_t {
        return mInFlight.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool waitIdleFor(std::uint32_t timeoutMs);
    void               waitIdle();

private:
    void leave() noexcept;

    std::string             mPluginName;
    std::atomic<bool>       mClosing{false};
    std::atomic<uint32_t>   mInFlight{0};
    std::atomic<uint32_t>   mWaiters{0};
    std::mutex              mWaitMutex;
    std::condition_variable mWaitCv;
};

struct BridgeValue;
struct ClassDescriptor;

struct BorrowLease {
    std::atomic<bool>                  alive{true};
    mutable std::mutex                 hookMutex;
    std::vector<std::function<void()>> onRevokeHooks;

    void addRevokeHook(std::function<void()> hook) {
        if (!hook) {
            return;
        }
        {
            std::lock_guard lock(hookMutex);
            if (alive.load(std::memory_order_acquire)) {
                onRevokeHooks.push_back(std::move(hook));
                return;
            }
        }
        hook();
    }

    void revoke() noexcept {
        if (!alive.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        std::vector<std::function<void()>> hooks;
        {
            std::lock_guard lock(hookMutex);
            hooks.swap(onRevokeHooks);
        }
        for (auto& hook: hooks) {
            try {
                hook();
            } catch (...) {}
        }
    }
};

class BorrowScopeGuard final {
public:
    BorrowScopeGuard() : mLease(std::make_shared<BorrowLease>()) {}
    BorrowScopeGuard(const BorrowScopeGuard&)            = delete;
    BorrowScopeGuard& operator=(const BorrowScopeGuard&) = delete;
    ~BorrowScopeGuard() {
        if (mLease) {
            mLease->revoke();
        }
    }

    [[nodiscard]] auto lease() const noexcept -> const std::shared_ptr<BorrowLease>& {
        return mLease;
    }

private:
    std::shared_ptr<BorrowLease> mLease;
};

template <class T>
struct Borrowed {
    T* ptr{};

    [[nodiscard]] constexpr auto get() const noexcept -> T* {
        return ptr;
    }

    [[nodiscard]] constexpr auto operator->() const noexcept -> T* {
        return ptr;
    }

    [[nodiscard]] constexpr auto operator*() const noexcept -> T& {
        return *ptr;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return ptr != nullptr;
    }
};

template <class T>
struct DisableAggregate<Borrowed<T>> : std::true_type {};

template <class T>
[[nodiscard]] constexpr auto borrow(T* ptr) noexcept -> Borrowed<T> {
    return Borrowed<T>{ptr};
}

template <class T>
[[nodiscard]] constexpr auto borrow(T& ref) noexcept -> Borrowed<T> {
    return Borrowed<T>{std::addressof(ref)};
}

class CloverNT_API RevocableObjectCell final : public std::enable_shared_from_this<RevocableObjectCell> {
public:
    struct PinnedAccess {
        PluginActivity::Guard guard;
        std::shared_ptr<void> instance;
        void*                 rawPtr{};

        [[nodiscard]] explicit operator bool() const noexcept {
            return rawPtr != nullptr;
        }
    };

    RevocableObjectCell(std::shared_ptr<void>           instance,
                        void*                           rawPtr,
                        const ClassDescriptor*          descriptor,
                        bool                            isConst,
                        std::string                     ownerPlugin,
                        std::shared_ptr<PluginActivity> activity,
                        std::string                     typeName = {});
    RevocableObjectCell(const RevocableObjectCell&)            = delete;
    RevocableObjectCell& operator=(const RevocableObjectCell&) = delete;

    [[nodiscard]] auto descriptor() const noexcept -> const ClassDescriptor* {
        return mDescriptor.load(std::memory_order_acquire);
    }

    [[nodiscard]] auto typeName() const noexcept -> std::string_view {
        return mTypeName;
    }

    [[nodiscard]] bool isConst() const noexcept {
        return mIsConst;
    }

    [[nodiscard]] auto ownerPlugin() const noexcept -> std::string_view {
        return mOwnerPlugin;
    }

    [[nodiscard]] auto identityKey() const noexcept -> const void* {
        return mIdentityKey;
    }

    [[nodiscard]] bool alive() const noexcept {
        return !mRevoked.load(std::memory_order_acquire) && (!mActivity || !mActivity->isClosing());
    }

    [[nodiscard]] auto pin() const -> Expected<PinnedAccess>;
    [[nodiscard]] auto shareRaw() const -> std::pair<std::shared_ptr<void>, void*>;
    void               revoke() noexcept;

private:
    std::atomic<const ClassDescriptor*> mDescriptor{nullptr};
    std::string                         mTypeName;
    const void*                         mIdentityKey{};
    bool                                mIsConst{false};
    std::string                         mOwnerPlugin;
    std::shared_ptr<PluginActivity>     mActivity;
    std::atomic<bool>                   mRevoked{false};
    mutable std::shared_mutex           mMutex;
    std::shared_ptr<void>               mInstance;
    void*                               mRawPtr{};
};

struct BorrowedObjectHandle {
    void*                        rawPtr{};
    const ClassDescriptor*       descriptor{};
    bool                         isConst{false};
    std::string                  ownerPlugin;
    std::shared_ptr<BorrowLease> lease;

    [[nodiscard]] bool alive() const noexcept {
        return rawPtr != nullptr && lease && lease->alive.load(std::memory_order_acquire);
    }
};

class BridgeAsyncState;
using BridgeAsync = std::shared_ptr<BridgeAsyncState>;

class CloverNT_API BridgeCallableImpl {
public:
    BridgeCallableImpl()                                     = default;
    virtual ~BridgeCallableImpl()                            = default;
    BridgeCallableImpl(const BridgeCallableImpl&)            = delete;
    BridgeCallableImpl& operator=(const BridgeCallableImpl&) = delete;

    [[nodiscard]] virtual auto ownerPlugin() const noexcept -> std::string_view = 0;
    [[nodiscard]] virtual bool alive() const noexcept {
        return true;
    }
    [[nodiscard]] virtual bool canInvokeSyncHere() const noexcept = 0;

    [[nodiscard]] virtual auto nativeTag() const noexcept -> std::uint64_t {
        return 0;
    }

    [[nodiscard]] virtual auto invoke(std::span<const BridgeValue> args) -> Expected<BridgeValue> = 0;
    [[nodiscard]] virtual auto post(std::vector<BridgeValue> args) -> Expected<void>              = 0;
    [[nodiscard]] virtual auto invokeAsync(std::vector<BridgeValue> args) -> BridgeAsync          = 0;
};

using BridgeCallable = std::shared_ptr<BridgeCallableImpl>;

class CloverNT_API CoreBridgeCallable final : public BridgeCallableImpl {
public:
    using InvokeFn = auto (*)(void* rawFn, std::span<const BridgeValue> args) -> Expected<BridgeValue>;

    CoreBridgeCallable(std::shared_ptr<RevocableObjectCell> cell, InvokeFn invokeFn) noexcept;
    CoreBridgeCallable(const CoreBridgeCallable&)            = delete;
    CoreBridgeCallable& operator=(const CoreBridgeCallable&) = delete;

    [[nodiscard]] auto ownerPlugin() const noexcept -> std::string_view override;
    [[nodiscard]] bool alive() const noexcept override;
    [[nodiscard]] bool canInvokeSyncHere() const noexcept override;
    [[nodiscard]] auto invoke(std::span<const BridgeValue> args) -> Expected<BridgeValue> override;
    [[nodiscard]] auto post(std::vector<BridgeValue> args) -> Expected<void> override;
    [[nodiscard]] auto invokeAsync(std::vector<BridgeValue> args) -> BridgeAsync override;

private:
    std::shared_ptr<RevocableObjectCell> mCell;
    InvokeFn                             mInvokeFn{};
};

class CloverNT_API RemoteObjectImpl {
public:
    RemoteObjectImpl()                                   = default;
    virtual ~RemoteObjectImpl()                          = default;
    RemoteObjectImpl(const RemoteObjectImpl&)            = delete;
    RemoteObjectImpl& operator=(const RemoteObjectImpl&) = delete;

    [[nodiscard]] virtual bool alive() const noexcept {
        return true;
    }
    [[nodiscard]] virtual bool canAccessSyncHere() const noexcept = 0;
    [[nodiscard]] virtual auto nativeTag() const noexcept -> std::uint64_t {
        return 0;
    }

    [[nodiscard]] virtual auto hasProperty(std::string_view key) -> Expected<bool>                    = 0;
    [[nodiscard]] virtual auto getProperty(std::string_view key) -> Expected<BridgeValue>             = 0;
    [[nodiscard]] virtual auto setProperty(std::string_view key, BridgeValue value) -> Expected<void> = 0;
    [[nodiscard]] virtual auto propertyNames() -> Expected<std::vector<std::string>>                  = 0;

    [[nodiscard]] virtual auto invokeMethod(std::string_view method, std::span<const BridgeValue> args)
            -> Expected<BridgeValue>                                                                                = 0;
    [[nodiscard]] virtual auto postMethod(std::string_view method, std::vector<BridgeValue> args) -> Expected<void> = 0;
    [[nodiscard]] virtual auto invokeMethodAsync(std::string_view method, std::vector<BridgeValue> args)
            -> BridgeAsync                                                           = 0;
    [[nodiscard]] virtual auto getPropertyAsync(std::string_view key) -> BridgeAsync = 0;
};

using RemoteObject = std::shared_ptr<RemoteObjectImpl>;

struct BridgeValue {
    using Bytes  = std::vector<std::uint8_t>;
    using Array  = std::vector<BridgeValue>;
    using Object = std::vector<std::pair<std::string, BridgeValue>>;

    using Storage = std::variant<std::nullptr_t,
                                 bool,
                                 std::int64_t,
                                 std::uint64_t,
                                 double,
                                 std::string,
                                 Bytes,
                                 Array,
                                 Object,
                                 std::shared_ptr<RevocableObjectCell>,
                                 BorrowedObjectHandle,
                                 RemoteObject,
                                 BridgeCallable,
                                 BridgeAsync>;

    Storage data{nullptr};

    BridgeValue() = default;
    // ReSharper disable once CppNonExplicitConvertingConstructor
    BridgeValue(std::nullptr_t) noexcept : data(nullptr) {}
    // ReSharper disable once CppNonExplicitConvertingConstructor
    BridgeValue(const bool v) noexcept : data(v) {}
    // ReSharper disable once CppNonExplicitConvertingConstructor
    BridgeValue(const std::int64_t v) noexcept : data(v) {}
    // ReSharper disable once CppNonExplicitConvertingConstructor
    BridgeValue(const std::uint64_t v) noexcept : data(v) {}
    // ReSharper disable once CppNonExplicitConvertingConstructor
    BridgeValue(const double v) noexcept : data(v) {}
    // ReSharper disable once CppNonExplicitConvertingConstructor
    BridgeValue(std::string v) noexcept : data(std::move(v)) {}
    // ReSharper disable once CppNonExplicitConvertingConstructor
    BridgeValue(Bytes v) noexcept : data(std::move(v)) {}
    // ReSharper disable once CppNonExplicitConvertingConstructor
    BridgeValue(Array v) noexcept : data(std::move(v)) {}
    // ReSharper disable once CppNonExplicitConvertingConstructor
    BridgeValue(Object v) noexcept : data(std::move(v)) {}
    // ReSharper disable once CppNonExplicitConvertingConstructor
    BridgeValue(std::shared_ptr<RevocableObjectCell> v) noexcept : data(std::move(v)) {}
    // ReSharper disable once CppNonExplicitConvertingConstructor
    BridgeValue(BorrowedObjectHandle v) noexcept : data(std::move(v)) {}
    // ReSharper disable once CppNonExplicitConvertingConstructor
    BridgeValue(RemoteObject v) noexcept : data(std::move(v)) {}
    // ReSharper disable once CppNonExplicitConvertingConstructor
    BridgeValue(BridgeCallable v) noexcept : data(std::move(v)) {}
    // ReSharper disable once CppNonExplicitConvertingConstructor
    BridgeValue(BridgeAsync v) noexcept : data(std::move(v)) {}

    [[nodiscard]] bool isNull() const noexcept {
        return std::holds_alternative<std::nullptr_t>(data);
    }

    [[nodiscard]] bool isArray() const noexcept {
        return std::holds_alternative<Array>(data);
    }

    [[nodiscard]] auto asArray() const noexcept -> const Array& {
        return std::get<Array>(data);
    }

    [[nodiscard]] bool isObject() const noexcept {
        return std::holds_alternative<Object>(data);
    }

    [[nodiscard]] auto asObject() const noexcept -> const Object& {
        return std::get<Object>(data);
    }

    [[nodiscard]] bool isBorrowedObject() const noexcept {
        return std::holds_alternative<BorrowedObjectHandle>(data);
    }

    [[nodiscard]] auto kindName() const noexcept -> std::string_view {
        return std::visit(
                []<class Alt>(const Alt&) -> std::string_view {
                    if constexpr (std::same_as<Alt, std::nullptr_t>) {
                        return "null";
                    } else if constexpr (std::same_as<Alt, bool>) {
                        return "boolean";
                    } else if constexpr (std::same_as<Alt, std::int64_t> || std::same_as<Alt, std::uint64_t> ||
                                         std::same_as<Alt, double>) {
                        return "number";
                    } else if constexpr (std::same_as<Alt, std::string>) {
                        return "string";
                    } else if constexpr (std::same_as<Alt, Bytes>) {
                        return "Uint8Array";
                    } else if constexpr (std::same_as<Alt, Array>) {
                        return "array";
                    } else if constexpr (std::same_as<Alt, Object>) {
                        return "object";
                    } else if constexpr (std::same_as<Alt, std::shared_ptr<RevocableObjectCell>> ||
                                         std::same_as<Alt, BorrowedObjectHandle>) {
                        return "native object";
                    } else if constexpr (std::same_as<Alt, RemoteObject>) {
                        return "remote object";
                    } else if constexpr (std::same_as<Alt, BridgeCallable>) {
                        return "function";
                    } else {
                        return "Promise";
                    }
                },
                data);
    }

    [[nodiscard]] auto findField(const std::string_view key) const noexcept -> const BridgeValue* {
        if (const auto* obj = std::get_if<Object>(&data)) {
            for (const auto& [k, v]: *obj) {
                if (k == key) {
                    return &v;
                }
            }
        }
        return nullptr;
    }
};

template <>
struct DisableAggregate<BridgeValue> : std::true_type {};

class CloverNT_API BridgeAsyncState final {
public:
    using Continuation = std::function<void(const Expected<BridgeValue>&)>;

    BridgeAsyncState()                                   = default;
    BridgeAsyncState(const BridgeAsyncState&)            = delete;
    BridgeAsyncState& operator=(const BridgeAsyncState&) = delete;

    void               resolve(BridgeValue value);
    void               reject(Error error);
    void               settle(Expected<BridgeValue> outcome);
    void               then(Continuation continuation);
    [[nodiscard]] bool ready() const;
    [[nodiscard]] auto wait() -> const Expected<BridgeValue>&;

private:
    mutable std::mutex                   mMutex;
    std::condition_variable              mCv;
    std::optional<Expected<BridgeValue>> mOutcome;
    std::vector<Continuation>            mContinuations;
};

CloverNT_API auto currentOwnerPluginName() -> std::string;
CloverNT_API auto acquirePluginActivity(std::string_view pluginName) -> std::shared_ptr<PluginActivity>;
CloverNT_API void registerPluginObjectCell(std::string_view pluginName, std::weak_ptr<RevocableObjectCell> cell);

class CloverNT_API ScopedOwnerPlugin final {
public:
    explicit ScopedOwnerPlugin(std::string_view pluginName) noexcept;
    ScopedOwnerPlugin(const ScopedOwnerPlugin&)            = delete;
    ScopedOwnerPlugin& operator=(const ScopedOwnerPlugin&) = delete;
    ~ScopedOwnerPlugin();

private:
    std::string_view mPrevious;
};

} // namespace CloverNT::Reflect
