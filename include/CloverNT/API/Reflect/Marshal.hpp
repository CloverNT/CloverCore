#pragma once

#include <CloverNT/API/Reflect/Aggregate.hpp>
#include <CloverNT/API/Reflect/BridgeValue.hpp>
#include <CloverNT/API/Reflect/Class.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace CloverNT::Reflect {

struct MarshalContext {
    BorrowLease* activeLease{};
    std::size_t  depth{0};

    static constexpr std::size_t kMaxDepth = 64;
};

[[nodiscard]] inline auto prependErrorPath(Error err, const std::string_view segment) -> Error {
    if (segment.empty()) {
        return err;
    }
    std::string combined{segment};
    if (!err.message.empty() && err.message.front() != '.' && err.message.front() != '[') {
        combined += ": ";
    }
    combined += err.message;
    err.message = std::move(combined);
    return err;
}

template <class T>
struct MarshalTraits;

template <class T>
concept ToBridgeConvertible = requires(const std::remove_cvref_t<T>& val, MarshalContext& ctx) {
    { MarshalTraits<std::remove_cvref_t<T>>::toBridge(val, ctx) } -> std::same_as<Expected<BridgeValue>>;
};

template <class T>
concept FromBridgeConvertible = requires(const BridgeValue& val, MarshalContext& ctx) {
    { MarshalTraits<std::remove_cvref_t<T>>::fromBridge(val, ctx) } -> std::same_as<Expected<std::remove_cvref_t<T>>>;
};

template <class T>
[[nodiscard]] auto toBridge(const T& value, MarshalContext& ctx) -> Expected<BridgeValue> {
    if (ctx.depth >= MarshalContext::kMaxDepth) {
        return bridgeUnexpected(BridgeErrorCode::DepthExceeded, "maximum object nesting depth (64) exceeded");
    }
    ++ctx.depth;
    auto res = MarshalTraits<std::remove_cvref_t<T>>::toBridge(value, ctx);
    --ctx.depth;
    return res;
}

template <class T>
[[nodiscard]] auto toBridge(const T& value) -> Expected<BridgeValue> {
    MarshalContext ctx{};
    return toBridge(value, ctx);
}

template <class T>
[[nodiscard]] auto fromBridge(const BridgeValue& value, MarshalContext& ctx) -> Expected<std::remove_cvref_t<T>> {
    if (ctx.depth >= MarshalContext::kMaxDepth) {
        return bridgeUnexpected(BridgeErrorCode::DepthExceeded, "maximum object nesting depth (64) exceeded");
    }
    ++ctx.depth;
    auto res = MarshalTraits<std::remove_cvref_t<T>>::fromBridge(value, ctx);
    --ctx.depth;
    return res;
}

template <class T>
[[nodiscard]] auto fromBridge(const BridgeValue& value) -> Expected<std::remove_cvref_t<T>> {
    MarshalContext ctx{};
    return fromBridge<T>(value, ctx);
}

template <class T>
[[nodiscard]] auto schemaOf() -> const TypeSchema*;

template <class T = void>
class Async;

template <class T = void>
class AsyncResolver {
public:
    AsyncResolver() = default;

    explicit AsyncResolver(BridgeAsync state) noexcept : mState(std::move(state)) {}

    template <class U = T>
        requires(!std::is_void_v<U>)
    void resolve(const U& value) const {
        if (!mState) {
            return;
        }
        if (auto converted = toBridge(value); converted) {
            mState->resolve(std::move(*converted));
        } else {
            mState->reject(std::move(converted).error());
        }
    }

    template <class U = T>
        requires std::is_void_v<U>
    void resolve() const {
        if (mState) {
            mState->resolve(BridgeValue{nullptr});
        }
    }

    void reject(Error error) const {
        if (mState) {
            mState->reject(std::move(error));
        }
    }

private:
    BridgeAsync mState;
};

namespace Detail {
    template <class CoroT>
    struct AsyncPromiseBase {
        BridgeAsync state = std::make_shared<BridgeAsyncState>();

        [[nodiscard]] auto get_return_object() noexcept -> Async<CoroT> {
            return Async<CoroT>{state};
        }

        [[nodiscard]] auto initial_suspend() noexcept -> std::suspend_never {
            return {};
        }

        [[nodiscard]] auto final_suspend() noexcept -> std::suspend_never {
            return {};
        }

        void unhandled_exception() noexcept {
            try {
                std::rethrow_exception(std::current_exception());
            } catch (const std::exception& ex) {
                state->reject(bridgeError(BridgeErrorCode::ExecutionFailed, ex.what()));
            } catch (...) {
                state->reject(bridgeError(BridgeErrorCode::ExecutionFailed, "unhandled exception in async task"));
            }
        }
    };

    template <class CoroT>
    struct AsyncPromise : AsyncPromiseBase<CoroT> {
        template <class V>
            requires std::convertible_to<V, CoroT>
        void return_value(V&& value) {
            const CoroT typed = std::forward<V>(value);
            if (auto converted = toBridge(typed); converted) {
                this->state->resolve(std::move(*converted));
            } else {
                this->state->reject(std::move(converted).error());
            }
        }

        void return_value(Expected<CoroT> result) {
            if (!result) {
                this->state->reject(std::move(result).error());
                return;
            }
            if (auto converted = toBridge(*result); converted) {
                this->state->resolve(std::move(*converted));
            } else {
                this->state->reject(std::move(converted).error());
            }
        }

        void return_value(std::unexpected<Error> err) {
            this->state->reject(std::move(err).error());
        }
    };

    template <>
    struct AsyncPromise<void> : AsyncPromiseBase<void> {
        void return_void() noexcept {
            this->state->resolve(BridgeValue{nullptr});
        }
    };
} // namespace Detail

template <class T>
class Async {
public:
    using promise_type = Detail::AsyncPromise<T>;

    Async() : mState(std::make_shared<BridgeAsyncState>()) {}

    explicit Async(BridgeAsync state) noexcept : mState(std::move(state)) {}

    template <class U = T>
        requires(!std::is_void_v<U>)
    [[nodiscard]] static auto resolved(const U& value) -> Async {
        auto state = std::make_shared<BridgeAsyncState>();
        if (auto converted = toBridge(value); converted) {
            state->resolve(std::move(*converted));
        } else {
            state->reject(std::move(converted).error());
        }
        return Async{std::move(state)};
    }

    template <class U = T>
        requires std::is_void_v<U>
    [[nodiscard]] static auto resolved() -> Async {
        auto state = std::make_shared<BridgeAsyncState>();
        state->resolve(BridgeValue{nullptr});
        return Async{std::move(state)};
    }

    [[nodiscard]] static auto rejected(Error error) -> Async {
        auto state = std::make_shared<BridgeAsyncState>();
        state->reject(std::move(error));
        return Async{std::move(state)};
    }

    [[nodiscard]] auto rawState() const noexcept -> const BridgeAsync& {
        return mState;
    }

    [[nodiscard]] bool valid() const noexcept {
        return mState != nullptr;
    }

    [[nodiscard]] bool ready() const {
        return mState && mState->ready();
    }

    [[nodiscard]] auto get() const -> Expected<T> {
        if (!mState) {
            return bridgeUnexpected(BridgeErrorCode::ExecutionFailed, "async operation is empty");
        }
        const auto& raw = mState->wait();
        if (!raw) {
            return unexpected(raw.error());
        }
        if constexpr (std::is_void_v<T>) {
            return {};
        } else {
            return fromBridge<T>(*raw);
        }
    }

    template <class Fn>
    void then(Fn&& callback) const {
        if (!mState) {
            return;
        }
        mState->then([cb = std::forward<Fn>(callback)](const Expected<BridgeValue>& raw) mutable {
            if constexpr (std::is_void_v<T>) {
                if (!raw) {
                    cb(Expected<void>{unexpected(raw.error())});
                } else {
                    cb(Expected<void>{});
                }
            } else {
                if (!raw) {
                    cb(Expected<T>{unexpected(raw.error())});
                } else {
                    cb(fromBridge<T>(*raw));
                }
            }
        });
    }

    struct Awaiter {
        BridgeAsync state;

        [[nodiscard]] bool await_ready() const {
            return !state || state->ready();
        }

        void await_suspend(std::coroutine_handle<> handle) const {
            state->then([handle](const Expected<BridgeValue>&) mutable { handle.resume(); });
        }

        [[nodiscard]] auto await_resume() const -> Expected<T> {
            return Async<T>{state}.get();
        }
    };

    [[nodiscard]] auto operator co_await() const noexcept -> Awaiter {
        return Awaiter{mState};
    }

private:
    BridgeAsync mState;
};

template <class T>
struct DisableAggregate<Async<T>> : std::true_type {};

template <class T = void>
[[nodiscard]] auto makeAsync() -> std::pair<Async<T>, AsyncResolver<T>> {
    auto state = std::make_shared<BridgeAsyncState>();
    return {Async<T>{state}, AsyncResolver<T>{state}};
}

namespace Detail {

    template <class T>
    struct IsExpected : std::false_type {
        using ValueType = T;
    };

    template <class U>
    struct IsExpected<Expected<U>> : std::true_type {
        using ValueType = U;
    };

    template <class T>
    struct IsAsync : std::false_type {
        using ValueType = T;
    };

    template <class U>
    struct IsAsync<Async<U>> : std::true_type {
        using ValueType = U;
    };

    template <class T>
    struct IsOptional : std::false_type {};

    template <class U>
    struct IsOptional<std::optional<U>> : std::true_type {};

    template <class... T>
    [[nodiscard]] auto packBridgeArgs([[maybe_unused]] MarshalContext& ctx, const T&... args)
            -> Expected<std::vector<BridgeValue>> {
        std::vector<BridgeValue> packed;
        packed.reserve(sizeof...(T));
        [[maybe_unused]] std::size_t idx = 0;
        std::optional<Error>         err;
        (
                [&] {
                    if (err) {
                        return;
                    }
                    auto v = toBridge(args, ctx);
                    if (!v) {
                        err = prependErrorPath(std::move(v).error(), "arg[" + std::to_string(idx) + "]");
                        return;
                    }
                    packed.push_back(std::move(*v));
                    ++idx;
                }(),
                ...);
        if (err) {
            return unexpected(*err);
        }
        return packed;
    }

    template <class Tuple, std::size_t... Is>
    [[nodiscard]] constexpr auto countMinArgs(std::index_sequence<Is...>) noexcept -> std::size_t {
        std::size_t minCount = sizeof...(Is);
        auto        scan     = [&]<std::size_t I>() {
            using Elem = std::remove_cvref_t<std::tuple_element_t<I, Tuple>>;
            if (minCount == I + 1 && IsOptional<Elem>::value) {
                minCount = I;
            }
        };
        auto walkBack = [&]<std::size_t... Rev>(std::index_sequence<Rev...>) {
            (scan.template operator()<sizeof...(Is) - 1 - Rev>(), ...);
        };
        if constexpr (sizeof...(Is) > 0) {
            walkBack(std::make_index_sequence<sizeof...(Is)>{});
        }
        return minCount;
    }

    template <class Tuple, std::size_t... Is>
    [[nodiscard]] auto
    unpackBridgeArgs(std::span<const BridgeValue> args, MarshalContext& ctx, std::index_sequence<Is...>)
            -> Expected<std::tuple<std::remove_cvref_t<std::tuple_element_t<Is, Tuple>>...>> {
        constexpr std::size_t kMax = sizeof...(Is);
        constexpr std::size_t kMin = countMinArgs<Tuple>(std::index_sequence<Is...>{});
        if (args.size() < kMin) {
            return bridgeUnexpected(BridgeErrorCode::ArityMismatch,
                                    "expected at least " + std::to_string(kMin) + " argument(s), got " +
                                            std::to_string(args.size()));
        }
        (void) kMax;
        std::tuple<std::remove_cvref_t<std::tuple_element_t<Is, Tuple>>...> out{};
        std::optional<Error>                                                firstErr;
        (
                [&] {
                    if (firstErr) {
                        return;
                    }
                    using Param = std::remove_cvref_t<std::tuple_element_t<Is, Tuple>>;
                    if (Is >= args.size()) {
                        return; // Trailing optional left as std::nullopt
                    }
                    auto converted = fromBridge<Param>(args[Is], ctx);
                    if (!converted) {
                        firstErr = prependErrorPath(std::move(converted).error(), "arg[" + std::to_string(Is) + "]");
                        return;
                    }
                    std::get<Is>(out) = std::move(*converted);
                }(),
                ...);
        if (firstErr) {
            return unexpected(*firstErr);
        }
        return out;
    }

    template <class R, class InvokeFn>
    [[nodiscard]] auto invokeAndMarshalReturn(MarshalContext& ctx, InvokeFn&& fn) -> Expected<BridgeValue> {
        try {
            if constexpr (std::is_void_v<R>) {
                fn();
                return BridgeValue{nullptr};
            } else if constexpr (IsExpected<R>::value) {
                auto result = fn();
                if (!result) {
                    return unexpected(std::move(result).error());
                }
                if constexpr (std::is_void_v<typename IsExpected<R>::ValueType>) {
                    return BridgeValue{nullptr};
                } else {
                    return toBridge(*result, ctx);
                }
            } else {
                auto result = fn();
                return toBridge(result, ctx);
            }
        } catch (const std::exception& ex) {
            return bridgeUnexpected(BridgeErrorCode::ExecutionFailed, ex.what());
        } catch (...) {
            return bridgeUnexpected(BridgeErrorCode::ExecutionFailed, "callback threw an unknown exception");
        }
    }

    template <class Fn, class R, class... Args>
    [[nodiscard]] auto makeCppBridgeCallable(Fn fn, std::string ownerPlugin = currentOwnerPluginName())
            -> BridgeCallable {
        using DecayedFn = std::decay_t<Fn>;
        auto holder     = std::make_shared<DecayedFn>(std::move(fn));
        auto activity   = acquirePluginActivity(ownerPlugin);
        auto cell       = std::make_shared<RevocableObjectCell>(holder,
                                                                holder.get(),
                                                                nullptr,
                                                                /*isConst=*/
                                                                false,
                                                                ownerPlugin,
                                                                std::move(activity),
                                                                "Callable");
        registerPluginObjectCell(ownerPlugin, cell);

        const CoreBridgeCallable::InvokeFn invokeFn =
                +[](void* rawFn, const std::span<const BridgeValue> args) -> Expected<BridgeValue> {
            const BorrowScopeGuard borrowScope;
            MarshalContext         ctx{.activeLease = borrowScope.lease().get()};
            auto unpacked = unpackBridgeArgs<std::tuple<Args...>>(args, ctx, std::index_sequence_for<Args...>{});
            if (!unpacked) {
                return unexpected(std::move(unpacked).error());
            }
            auto& callable = *static_cast<DecayedFn*>(rawFn);
            return invokeAndMarshalReturn<R>(ctx, [&]() -> decltype(auto) {
                return std::apply([&](auto&... callArgs) -> decltype(auto) { return callable(std::move(callArgs)...); },
                                  *unpacked);
            });
        };

        return std::make_shared<CoreBridgeCallable>(std::move(cell), invokeFn);
    }

} // namespace Detail

template <class Sig>
class Callable;

template <class R, class... Args>
class Callable<R(Args...)> {
public:
    using UnwrappedAsyncReturn = Detail::IsAsync<R>::ValueType;

    Callable() = default;

    explicit Callable(BridgeCallable impl) noexcept : mImpl(std::move(impl)) {}

    template <class Fn>
        requires(!std::same_as<std::remove_cvref_t<Fn>, Callable> && std::invocable<Fn&, Args...>)
    // ReSharper disable once CppNonExplicitConvertingConstructor
    Callable(Fn&& fn)
        : mImpl(Detail::makeCppBridgeCallable<std::decay_t<Fn>, std::invoke_result_t<Fn&, Args...>, Args...>(
                  std::forward<Fn>(fn))) {}

    [[nodiscard]] bool valid() const noexcept {
        return mImpl != nullptr;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return valid();
    }

    [[nodiscard]] auto raw() const noexcept -> const BridgeCallable& {
        return mImpl;
    }

    [[nodiscard]] bool canInvokeSync() const noexcept {
        return mImpl && mImpl->alive() && mImpl->canInvokeSyncHere();
    }

    [[nodiscard]] auto post(const Args&... args) const -> Expected<void> {
        if (!mImpl) {
            return bridgeUnexpected(BridgeErrorCode::ExecutionFailed, "callback is empty");
        }
        MarshalContext ctx{};
        auto           packed = Detail::packBridgeArgs(ctx, args...);
        if (!packed) {
            return unexpected(std::move(packed).error());
        }
        return mImpl->post(std::move(*packed));
    }

    [[nodiscard]] auto invokeAsync(const Args&... args) const -> Async<UnwrappedAsyncReturn> {
        if (!mImpl) {
            return Async<UnwrappedAsyncReturn>::rejected(
                    bridgeError(BridgeErrorCode::ExecutionFailed, "callback is empty"));
        }
        MarshalContext ctx{};
        auto           packed = Detail::packBridgeArgs(ctx, args...);
        if (!packed) {
            return Async<UnwrappedAsyncReturn>::rejected(std::move(packed).error());
        }
        return Async<UnwrappedAsyncReturn>{mImpl->invokeAsync(std::move(*packed))};
    }

    [[nodiscard]] auto tryInvoke(const Args&... args) const -> Expected<R> {
        if (!mImpl) {
            return bridgeUnexpected(BridgeErrorCode::ExecutionFailed, "callback is empty");
        }
        if constexpr (Detail::IsAsync<R>::value) {
            return invokeAsync(args...);
        } else {
            if (!mImpl->alive()) {
                return bridgeUnexpected(BridgeErrorCode::ObjectRevoked, "callback is no longer available");
            }
            if (!mImpl->canInvokeSyncHere()) {
                if constexpr (std::is_void_v<R>) {
                    return post(args...);
                } else {
                    return bridgeUnexpected(
                            BridgeErrorCode::CrossThreadSyncCallForbidden,
                            "cannot synchronously call non-void JavaScript function from a background thread");
                }
            }
            const BorrowScopeGuard borrowScope;
            MarshalContext         ctx{.activeLease = borrowScope.lease().get()};
            auto                   packed = Detail::packBridgeArgs(ctx, args...);
            if (!packed) {
                return unexpected(std::move(packed).error());
            }
            auto rawRet = mImpl->invoke(*packed);
            if (!rawRet) {
                return unexpected(std::move(rawRet).error());
            }
            if constexpr (std::is_void_v<R>) {
                return {};
            } else {
                if (std::holds_alternative<BridgeAsync>(rawRet->data)) {
                    return bridgeUnexpected(
                            BridgeErrorCode::PromiseReturnedToSyncCaller,
                            "function returned a Promise where a synchronous return value was expected");
                }
                return fromBridge<R>(*rawRet, ctx);
            }
        }
    }

    auto operator()(const Args&... args) const -> R {
        auto result = tryInvoke(args...);
        if (!result) {
            throw std::runtime_error(result.error().message);
        }
        if constexpr (!std::is_void_v<R>) {
            return std::move(*result);
        }
        return R{};
    }

private:
    BridgeCallable mImpl;
};

template <class Sig>
struct DisableAggregate<Callable<Sig>> : std::true_type {};

template <class R, class... Args>
struct DisableAggregate<std::function<R(Args...)>> : std::true_type {};

class ObjectRef {
public:
    ObjectRef() = default;

    explicit ObjectRef(RemoteObject impl) noexcept : mImpl(std::move(impl)) {}

    [[nodiscard]] bool valid() const noexcept {
        return mImpl != nullptr;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return valid();
    }

    [[nodiscard]] auto raw() const noexcept -> const RemoteObject& {
        return mImpl;
    }

    [[nodiscard]] bool canAccessSync() const noexcept {
        return mImpl && mImpl->canAccessSyncHere();
    }

    [[nodiscard]] auto has(const std::string_view key) const -> Expected<bool> {
        if (!mImpl) {
            return bridgeUnexpected(BridgeErrorCode::ExecutionFailed, "object reference is empty");
        }
        return mImpl->hasProperty(key);
    }

    template <class T = BridgeValue>
    [[nodiscard]] auto get(const std::string_view key) const -> Expected<T> {
        if (!mImpl) {
            return bridgeUnexpected(BridgeErrorCode::ExecutionFailed, "object reference is empty");
        }
        auto rawVal = mImpl->getProperty(key);
        if (!rawVal) {
            return unexpected(std::move(rawVal).error());
        }
        return fromBridge<T>(*rawVal);
    }

    template <class T = BridgeValue>
    [[nodiscard]] auto getAsync(const std::string_view key) const -> Async<T> {
        if (!mImpl) {
            return Async<T>::rejected(bridgeError(BridgeErrorCode::ExecutionFailed, "object reference is empty"));
        }
        return Async<T>{mImpl->getPropertyAsync(key)};
    }

    template <class T>
    [[nodiscard]] auto set(const std::string_view key, const T& value) const -> Expected<void> {
        if (!mImpl) {
            return bridgeUnexpected(BridgeErrorCode::ExecutionFailed, "object reference is empty");
        }
        auto bridged = toBridge(value);
        if (!bridged) {
            return unexpected(prependErrorPath(std::move(bridged).error(), key));
        }
        return mImpl->setProperty(key, std::move(*bridged));
    }

    template <class R = void, class... Args>
    [[nodiscard]] auto call(const std::string_view method, const Args&... args) const -> Expected<R> {
        if (!mImpl) {
            return bridgeUnexpected(BridgeErrorCode::ExecutionFailed, "object reference is empty");
        }
        if constexpr (Detail::IsAsync<R>::value) {
            return callAsync<typename Detail::IsAsync<R>::ValueType>(method, args...);
        } else {
            if (!mImpl->canAccessSyncHere()) {
                if constexpr (std::is_void_v<R>) {
                    return post(method, args...);
                } else {
                    return bridgeUnexpected(BridgeErrorCode::CrossThreadSyncCallForbidden,
                                            "cannot synchronously call JavaScript method '" + std::string(method) +
                                                    "' from a background thread");
                }
            }
            BorrowScopeGuard borrowScope;
            MarshalContext   ctx{.activeLease = borrowScope.lease().get()};
            auto             packed = Detail::packBridgeArgs(ctx, args...);
            if (!packed) {
                return unexpected(prependErrorPath(std::move(packed).error(), method));
            }
            auto rawRet = mImpl->invokeMethod(method, *packed);
            if (!rawRet) {
                return unexpected(std::move(rawRet).error());
            }
            if constexpr (std::is_void_v<R>) {
                return {};
            } else {
                if (std::holds_alternative<BridgeAsync>(rawRet->data)) {
                    return bridgeUnexpected(
                            BridgeErrorCode::PromiseReturnedToSyncCaller,
                            "method '" + std::string(method) +
                                    "' returned a Promise where a synchronous return value was expected");
                }
                return fromBridge<R>(*rawRet, ctx);
            }
        }
    }

    template <class... Args>
    [[nodiscard]] auto post(const std::string_view method, const Args&... args) const -> Expected<void> {
        if (!mImpl) {
            return bridgeUnexpected(BridgeErrorCode::ExecutionFailed, "object reference is empty");
        }
        MarshalContext ctx{};
        auto           packed = Detail::packBridgeArgs(ctx, args...);
        if (!packed) {
            return unexpected(prependErrorPath(std::move(packed).error(), method));
        }
        return mImpl->postMethod(method, std::move(*packed));
    }

    template <class R = void, class... Args>
    [[nodiscard]] auto callAsync(const std::string_view method, const Args&... args) const -> Async<R> {
        if (!mImpl) {
            return Async<R>::rejected(bridgeError(BridgeErrorCode::ExecutionFailed, "object reference is empty"));
        }
        MarshalContext ctx{};
        auto           packed = Detail::packBridgeArgs(ctx, args...);
        if (!packed) {
            return Async<R>::rejected(prependErrorPath(std::move(packed).error(), method));
        }
        return Async<R>{mImpl->invokeMethodAsync(method, std::move(*packed))};
    }

private:
    RemoteObject mImpl;
};

template <>
struct DisableAggregate<ObjectRef> : std::true_type {};

namespace Detail {

    template <class I, auto MemberPtr>
    [[nodiscard]] consteval auto resolveInterfaceMemberName() noexcept -> std::string_view {
        if constexpr (ReflectedInterfaceType<I>) {
            std::string_view matched{};
            const auto       tup = Interface<I>::members;
            std::apply(
                    [&](const auto&... descs) {
                        (
                                [&]<typename T0>([[maybe_unused]] const T0& d) {
                                    using D = std::remove_cvref_t<T0>;
                                    if constexpr (D::kind == MemberDescKind::Method ||
                                                  D::kind == MemberDescKind::Field) {
                                        if constexpr (std::same_as<decltype(D::pointer), decltype(MemberPtr)>) {
                                            if (D::pointer == MemberPtr) {
                                                matched = D::name;
                                            }
                                        }
                                    }
                                }(descs),
                                ...);
                    },
                    tup);
            if (!matched.empty()) {
                return matched;
            }
        }
        return memberNameOf<MemberPtr>.view();
    }

} // namespace Detail

template <class InterfaceType>
class Proxy {
public:
    Proxy() = default;

    explicit Proxy(ObjectRef object) noexcept : mObject(std::move(object)) {}

    [[nodiscard]] bool valid() const noexcept {
        return mObject.valid();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return valid();
    }

    [[nodiscard]] auto object() const noexcept -> const ObjectRef& {
        return mObject;
    }

    template <auto MethodPtr, class... CallArgs>
        requires(MemberFunctionPointerTraits<std::remove_cvref_t<decltype(MethodPtr)>>::isMemberFn)
    [[nodiscard]] auto call(const CallArgs&... args) const {
        using Traits         = MemberFunctionPointerTraits<std::remove_cvref_t<decltype(MethodPtr)>>;
        using R              = Traits::ReturnType;
        constexpr auto kName = Detail::resolveInterfaceMemberName<InterfaceType, MethodPtr>();
        return mObject.call<R>(kName, args...);
    }

    template <auto MethodPtr, class... CallArgs>
        requires(MemberFunctionPointerTraits<std::remove_cvref_t<decltype(MethodPtr)>>::isMemberFn)
    [[nodiscard]] auto callAsync(const CallArgs&... args) const {
        using Traits         = MemberFunctionPointerTraits<std::remove_cvref_t<decltype(MethodPtr)>>;
        using R              = Detail::IsAsync<typename Traits::ReturnType>::ValueType;
        constexpr auto kName = Detail::resolveInterfaceMemberName<InterfaceType, MethodPtr>();
        return mObject.callAsync<R>(kName, args...);
    }

    template <auto MethodPtr, class... CallArgs>
        requires(MemberFunctionPointerTraits<std::remove_cvref_t<decltype(MethodPtr)>>::isMemberFn)
    [[nodiscard]] auto post(const CallArgs&... args) const -> Expected<void> {
        constexpr auto kName = Detail::resolveInterfaceMemberName<InterfaceType, MethodPtr>();
        return mObject.post(kName, args...);
    }

    template <auto FieldPtr>
        requires(FieldPointerTraits<std::remove_cvref_t<decltype(FieldPtr)>>::isFieldPointer)
    [[nodiscard]] auto get() const {
        using F = std::remove_cv_t<typename FieldPointerTraits<std::remove_cvref_t<decltype(FieldPtr)>>::FieldType>;
        constexpr auto kName = Detail::resolveInterfaceMemberName<InterfaceType, FieldPtr>();
        return mObject.get<F>(kName);
    }

    template <auto FieldPtr, class Val>
        requires(FieldPointerTraits<std::remove_cvref_t<decltype(FieldPtr)>>::isFieldPointer &&
                 !FieldPointerTraits<std::remove_cvref_t<decltype(FieldPtr)>>::isConst)
    [[nodiscard]] auto set(const Val& value) const -> Expected<void> {
        using F              = FieldPointerTraits<std::remove_cvref_t<decltype(FieldPtr)>>::FieldType;
        constexpr auto kName = Detail::resolveInterfaceMemberName<InterfaceType, FieldPtr>();
        return mObject.set<F>(kName, static_cast<const F&>(value));
    }

private:
    ObjectRef mObject;
};

template <class I>
struct DisableAggregate<Proxy<I>> : std::true_type {};

template <ReflectedClassType T>
[[nodiscard]] auto classDescriptorOf() -> const ClassDescriptor*;

namespace Detail {

    template <class T>
    [[nodiscard]] consteval auto reflectedClassName() noexcept -> std::string_view {
        if constexpr (requires {
                          { Class<T>::name } -> std::convertible_to<std::string_view>;
                      }) {
            return Class<T>::name;
        } else {
            return shortTypeNameOf<T>.view();
        }
    }

    template <class T>
    struct BaseClassOf {
        using Type = void;
    };

    template <class T>
        requires requires { typename Class<T>::Base; }
    struct BaseClassOf<T> {
        using Type = Class<T>::Base;
    };

    template <class T>
        requires(
                !requires { typename Class<T>::Base; } && requires { typename decltype(Class<T>::base)::BaseType; })
    struct BaseClassOf<T> {
        using Type = decltype(Class<T>::base)::BaseType;
    };

    template <class Derived, class CurrentBase>
    void collectBaseChain(std::vector<BaseCastEntry>& out) {
        if constexpr (!std::is_void_v<CurrentBase>) {
            static_assert(std::derived_from<Derived, CurrentBase>,
                          "CloverNT::Reflect: Class<Derived>::base must be an accessible base of Derived");
            out.push_back(BaseCastEntry{
                    .baseTypeId = typeIdOf<CurrentBase>,
                    .upcast     = +[](void* ptr) noexcept -> void* {
                        return static_cast<CurrentBase*>(static_cast<Derived*>(ptr));
                    },
            });
            collectBaseChain<Derived, typename BaseClassOf<CurrentBase>::Type>(out);
        }
    }

    template <class Tuple>
    [[nodiscard]] auto paramSchemasOf() -> std::span<const TypeSchema* const> {
        if constexpr (std::tuple_size_v<Tuple> == 0) {
            return {};
        } else {
            static const auto storage = []<std::size_t... Is>(std::index_sequence<Is...>) {
                return std::array<const TypeSchema*, sizeof...(Is)>{
                        schemaOf<std::remove_cvref_t<std::tuple_element_t<Is, Tuple>>>()...};
            }(std::make_index_sequence<std::tuple_size_v<Tuple>>{});
            return storage;
        }
    }

    template <class Ret, class ArgsTuple>
    [[nodiscard]] auto callableSchemaOf() -> const CallableSchema* {
        using EffectiveRet = IsExpected<Ret>::ValueType;
        static const CallableSchema schema{
                .returnType = schemaOf<EffectiveRet>(),
                .paramTypes = paramSchemasOf<ArgsTuple>(),
                .minArgs    = countMinArgs<ArgsTuple>(std::make_index_sequence<std::tuple_size_v<ArgsTuple>>{}),
        };
        return &schema;
    }

    template <class C, auto MethodPtr>
    [[nodiscard]] auto invokeSingleMemberMethod(void*                        self,
                                                const bool                   isConstReceiver,
                                                std::span<const BridgeValue> args,
                                                BorrowLease*                 activeLease) -> Expected<BridgeValue> {
        using Traits    = MemberFunctionPointerTraits<std::remove_cvref_t<decltype(MethodPtr)>>;
        using R         = Traits::ReturnType;
        using ArgsTuple = Traits::ArgsTuple;

        if constexpr (!Traits::isConst) {
            if (isConstReceiver) {
                return bridgeUnexpected(BridgeErrorCode::ReadonlyViolation,
                                        "cannot call mutating method on read-only object");
            }
        }
        MarshalContext ctx{.activeLease = activeLease};
        auto           unpacked =
                unpackBridgeArgs<ArgsTuple>(args, ctx, std::make_index_sequence<std::tuple_size_v<ArgsTuple>>{});
        if (!unpacked) {
            return unexpected(std::move(unpacked).error());
        }
        return invokeAndMarshalReturn<R>(ctx, [&]() -> decltype(auto) {
            if constexpr (Traits::isConst) {
                const auto* obj = static_cast<const C*>(self);
                return std::apply([&](auto&... a) -> decltype(auto) { return (obj->*MethodPtr)(std::move(a)...); },
                                  *unpacked);
            } else {
                auto* obj = static_cast<C*>(self);
                return std::apply([&](auto&... a) -> decltype(auto) { return (obj->*MethodPtr)(std::move(a)...); },
                                  *unpacked);
            }
        });
    }

    template <class C, class Desc>
    void appendClassMemberDesc(std::vector<PropertyDescriptor>& props,
                               std::vector<MethodDescriptor>&   methods,
                               std::vector<CtorDescriptor>&     ctors,
                               const Desc&) {
        if constexpr (Desc::kind == MemberDescKind::Field) {
            constexpr auto kPtr = Desc::pointer;
            using Traits        = FieldPointerTraits<std::remove_cvref_t<decltype(kPtr)>>;
            using F             = std::remove_cv_t<typename Traits::FieldType>;
            PropertyDescriptor p{
                    .name     = Desc::name,
                    .schema   = schemaOf<F>(),
                    .readonly = Desc::isReadonly,
                    .get      = +[](const void* self, BorrowLease* activeLease) -> Expected<BridgeValue> {
                        MarshalContext ctx{.activeLease = activeLease};
                        return toBridge(static_cast<const C*>(self)->*kPtr, ctx);
                    },
                    .set = nullptr,
            };
            if constexpr (!Desc::isReadonly) {
                p.set = +[](void* self, const BridgeValue& val) -> Expected<void> {
                    auto converted = fromBridge<F>(val);
                    if (!converted) {
                        return unexpected(prependErrorPath(std::move(converted).error(), Desc::name));
                    }
                    static_cast<C*>(self)->*kPtr = std::move(*converted);
                    return {};
                };
            }
            props.push_back(p);
        } else if constexpr (Desc::kind == MemberDescKind::Accessor) {
            constexpr auto kGet = Desc::getter;
            using GetTraits     = MemberFunctionPointerTraits<std::remove_cvref_t<decltype(kGet)>>;
            using ValType       = std::remove_cvref_t<typename IsExpected<typename GetTraits::ReturnType>::ValueType>;
            PropertyDescriptor p{
                    .name     = Desc::name,
                    .schema   = schemaOf<ValType>(),
                    .readonly = Desc::isReadonly,
                    .get      = +[](const void* self, BorrowLease* activeLease) -> Expected<BridgeValue> {
                        return invokeSingleMemberMethod<C, kGet>(const_cast<void*>(self), true, {}, activeLease);
                    },
                    .set = nullptr,
            };
            if constexpr (!Desc::isReadonly) {
                constexpr auto kSet = Desc::setter;
                p.set               = +[](void* self, const BridgeValue& val) -> Expected<void> {
                    const BridgeValue single[1]{val};
                    auto              res = invokeSingleMemberMethod<C, kSet>(self, false, single, nullptr);
                    if (!res) {
                        return unexpected(prependErrorPath(std::move(res).error(), Desc::name));
                    }
                    return {};
                };
            }
            props.push_back(p);
        } else if constexpr (Desc::kind == MemberDescKind::Method) {
            constexpr auto kFn = Desc::pointer;
            using Traits       = MemberFunctionPointerTraits<std::remove_cvref_t<decltype(kFn)>>;
            using ArgsTuple    = Traits::ArgsTuple;
            methods.push_back(MethodDescriptor{
                    .name      = Desc::name,
                    .isConst   = Traits::isConst,
                    .minArity  = countMinArgs<ArgsTuple>(std::make_index_sequence<std::tuple_size_v<ArgsTuple>>{}),
                    .maxArity  = std::tuple_size_v<ArgsTuple>,
                    .signature = callableSchemaOf<typename Traits::ReturnType, ArgsTuple>(),
                    .invoke    = &invokeSingleMemberMethod<C, kFn>,
            });
        } else if constexpr (Desc::kind == MemberDescKind::Overload) {
            [&]<FixedString Name, auto... Ptrs>(const OverloadDesc<Name, Ptrs...>&) {
                constexpr bool kAllConst =
                        (MemberFunctionPointerTraits<std::remove_cvref_t<decltype(Ptrs)>>::isConst && ...);
                constexpr std::size_t kMin = (std::min) ({countMinArgs<
                        typename MemberFunctionPointerTraits<std::remove_cvref_t<decltype(Ptrs)>>::ArgsTuple>(
                        std::make_index_sequence<std::tuple_size_v<typename MemberFunctionPointerTraits<
                                std::remove_cvref_t<decltype(Ptrs)>>::ArgsTuple>>{})...});
                constexpr std::size_t kMax = (std::max) ({std::tuple_size_v<
                        typename MemberFunctionPointerTraits<std::remove_cvref_t<decltype(Ptrs)>>::ArgsTuple>...});
                methods.push_back(MethodDescriptor{
                        .name      = Desc::name,
                        .isConst   = kAllConst,
                        .minArity  = kMin,
                        .maxArity  = kMax,
                        .signature = nullptr,
                        .invoke    = +[](void*                              self,
                                         const bool                         isConstReceiver,
                                         const std::span<const BridgeValue> args,
                                         BorrowLease*                       activeLease) -> Expected<BridgeValue> {
                            std::optional<Expected<BridgeValue>> matched;
                            std::optional<Error>                 lastErr;
                            (void) (([&] {
                                        using T = MemberFunctionPointerTraits<std::remove_cvref_t<decltype(Ptrs)>>;
                                        using A = T::ArgsTuple;
                                        constexpr auto minA =
                                                countMinArgs<A>(std::make_index_sequence<std::tuple_size_v<A>>{});
                                        if (args.size() < minA || args.size() > std::tuple_size_v<A>) {
                                            return false;
                                        }
                                        auto res = invokeSingleMemberMethod<C, Ptrs>(
                                                self, isConstReceiver, args, activeLease);
                                        if (res) {
                                            matched.emplace(std::move(res));
                                            return true;
                                        }
                                        lastErr = std::move(res).error();
                                        return false;
                                    }()) ||
                                    ...);
                            if (matched) {
                                return std::move(*matched);
                            }
                            if (lastErr) {
                                return unexpected(*lastErr);
                            }
                            return bridgeUnexpected(BridgeErrorCode::ArityMismatch,
                                                    "no overload of '" + std::string(Desc::name) + "' accepts " +
                                                            std::to_string(args.size()) + " argument(s)");
                        },
                });
            }(Desc{});
        } else if constexpr (Desc::kind == MemberDescKind::Ctor) {
            using ArgsTuple = Desc::ArgsTuple;
            ctors.push_back(CtorDescriptor{
                    .minArity   = countMinArgs<ArgsTuple>(std::make_index_sequence<std::tuple_size_v<ArgsTuple>>{}),
                    .maxArity   = std::tuple_size_v<ArgsTuple>,
                    .paramTypes = paramSchemasOf<ArgsTuple>(),
                    .construct =
                            +[](std::span<const BridgeValue> args) -> Expected<std::shared_ptr<RevocableObjectCell>> {
                        MarshalContext ctx{};
                        auto           unpacked = unpackBridgeArgs<ArgsTuple>(
                                args, ctx, std::make_index_sequence<std::tuple_size_v<ArgsTuple>>{});
                        if (!unpacked) {
                            return unexpected(std::move(unpacked).error());
                        }
                        try {
                            auto instance = std::apply([](auto&... a) { return std::make_shared<C>(std::move(a)...); },
                                                       *unpacked);
                            auto owner    = currentOwnerPluginName();
                            auto cell     = std::make_shared<RevocableObjectCell>(instance,
                                                                                  instance.get(),
                                                                                  classDescriptorOf<C>(),
                                                                                  false,
                                                                                  owner,
                                                                                  acquirePluginActivity(owner));
                            registerPluginObjectCell(owner, cell);
                            return cell;
                        } catch (const std::exception& ex) {
                            return bridgeUnexpected(BridgeErrorCode::ExecutionFailed, ex.what());
                        }
                    },
            });
        }
    }

} // namespace Detail

template <ReflectedClassType T>
[[nodiscard]] auto classDescriptorOf() -> const ClassDescriptor* {
    using Clean = std::remove_cvref_t<T>;
    using Base  = Detail::BaseClassOf<Clean>::Type;

    struct Holder {
        ClassDescriptor                 desc{};
        std::vector<BaseCastEntry>      bases;
        std::vector<PropertyDescriptor> props;
        std::vector<MethodDescriptor>   methods;
        std::vector<CtorDescriptor>     ctors;
    };

    static const Holder* instance = [] {
        auto* h        = new Holder{};
        h->desc.name   = Detail::reflectedClassName<Clean>();
        h->desc.typeId = typeIdOf<Clean>;
        if constexpr (!std::is_void_v<Base>) {
            h->desc.base = classDescriptorOf<Base>();
            Detail::collectBaseChain<Clean, Base>(h->bases);
            h->desc.baseChain = h->bases;
        }
        static const ClassDescriptor* earlyPtr = &h->desc;
        (void) earlyPtr;
        std::apply(
                [&](const auto&... m) {
                    (Detail::appendClassMemberDesc<Clean>(h->props, h->methods, h->ctors, m), ...);
                },
                Class<Clean>::members);
        h->desc.properties = h->props;
        h->desc.methods    = h->methods;
        h->desc.ctors      = h->ctors;
        return h;
    }();
    return &instance->desc;
}

template <ReflectedInterfaceType I>
[[nodiscard]] auto interfaceDescriptorOf() -> const InterfaceDescriptor* {
    using Clean = std::remove_cvref_t<I>;
    struct Holder {
        InterfaceDescriptor                     desc{};
        std::vector<InterfaceMemberRequirement> reqs;
    };
    static const Holder* instance = [] {
        auto* h = new Holder{};
        if constexpr (requires {
                          { Interface<Clean>::name } -> std::convertible_to<std::string_view>;
                      }) {
            h->desc.name = Interface<Clean>::name;
        } else {
            h->desc.name = shortTypeNameOf<Clean>.view();
        }
        h->desc.typeId = typeIdOf<Clean>;
        std::apply(
                [&](const auto&... m) {
                    (
                            [&]<typename T0>([[maybe_unused]] const T0& d) {
                                using D = std::remove_cvref_t<T0>;
                                if constexpr (D::kind == MemberDescKind::Method ||
                                              D::kind == MemberDescKind::Overload) {
                                    h->reqs.push_back({.name = D::name, .isMethod = true, .readonly = true});
                                } else if constexpr (D::kind == MemberDescKind::Field ||
                                                     D::kind == MemberDescKind::Accessor) {
                                    h->reqs.push_back({.name = D::name, .isMethod = false, .readonly = D::isReadonly});
                                }
                            }(m),
                            ...);
                },
                Interface<Clean>::members);
        h->desc.requirements = h->reqs;
        return h;
    }();
    return &instance->desc;
}

template <>
struct MarshalTraits<BridgeValue> {
    static auto toBridge(const BridgeValue& v, MarshalContext&) -> Expected<BridgeValue> {
        return v;
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext&) -> Expected<BridgeValue> {
        return v;
    }
};

template <>
struct MarshalTraits<std::nullptr_t> {
    static auto toBridge(std::nullptr_t, MarshalContext&) -> Expected<BridgeValue> {
        return BridgeValue{nullptr};
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext&) -> Expected<std::nullptr_t> {
        if (!v.isNull()) {
            return bridgeUnexpected(BridgeErrorCode::TypeMismatch, "expected null, got " + std::string(v.kindName()));
        }
        return nullptr;
    }
};

template <>
struct MarshalTraits<bool> {
    static auto toBridge(const bool v, MarshalContext&) -> Expected<BridgeValue> {
        return BridgeValue{v};
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext&) -> Expected<bool> {
        if (const auto* b = std::get_if<bool>(&v.data)) {
            return *b;
        }
        return bridgeUnexpected(BridgeErrorCode::TypeMismatch, "expected boolean, got " + std::string(v.kindName()));
    }
};

template <std::integral Int>
    requires(!std::same_as<Int, bool>)
struct MarshalTraits<Int> {
    static constexpr std::int64_t  kMaxSafeJsInt  = 9007199254740991ll;  // 2^53 - 1
    static constexpr std::uint64_t kMaxSafeJsUInt = 9007199254740991ull; // 2^53 - 1

    static auto toBridge(const Int v, MarshalContext&) -> Expected<BridgeValue> {
        if constexpr (std::is_signed_v<Int>) {
            return BridgeValue{static_cast<std::int64_t>(v)};
        } else {
            return BridgeValue{static_cast<std::uint64_t>(v)};
        }
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext&) -> Expected<Int> {
        constexpr auto lo = (std::numeric_limits<Int>::min)();
        constexpr auto hi = (std::numeric_limits<Int>::max)();

        if (const auto* i = std::get_if<std::int64_t>(&v.data)) {
            if constexpr (std::is_signed_v<Int>) {
                if (*i < static_cast<std::int64_t>(lo) || *i > static_cast<std::int64_t>(hi)) {
                    return bridgeUnexpected(BridgeErrorCode::IntegerOverflow,
                                            "integer " + std::to_string(*i) + " out of range for " +
                                                    std::string(shortTypeNameOf<Int>.view()));
                }
                return static_cast<Int>(*i);
            } else {
                if (*i < 0 || static_cast<std::uint64_t>(*i) > static_cast<std::uint64_t>(hi)) {
                    return bridgeUnexpected(BridgeErrorCode::IntegerOverflow,
                                            "integer " + std::to_string(*i) + " out of range for unsigned " +
                                                    std::string(shortTypeNameOf<Int>.view()));
                }
                return static_cast<Int>(static_cast<std::uint64_t>(*i));
            }
        }
        if (const auto* u = std::get_if<std::uint64_t>(&v.data)) {
            if (*u > static_cast<std::uint64_t>(hi)) {
                return bridgeUnexpected(BridgeErrorCode::IntegerOverflow,
                                        "unsigned integer " + std::to_string(*u) + " out of range for " +
                                                std::string(shortTypeNameOf<Int>.view()));
            }
            return static_cast<Int>(*u);
        }
        if (const auto* d = std::get_if<double>(&v.data)) {
            if (!std::isfinite(*d) || std::trunc(*d) != *d) {
                return bridgeUnexpected(BridgeErrorCode::TypeMismatch,
                                        "expected an exact integer, got non-integer number");
            }
            if constexpr (std::is_signed_v<Int>) {
                if (*d < -static_cast<double>(kMaxSafeJsInt) || *d > static_cast<double>(kMaxSafeJsInt)) {
                    return bridgeUnexpected(
                            BridgeErrorCode::IntegerOverflow,
                            "number exceeds 53-bit safe integer precision; use BigInt for 64-bit integers");
                }
                const auto iv = static_cast<std::int64_t>(*d);
                if (iv < static_cast<std::int64_t>(lo) || iv > static_cast<std::int64_t>(hi)) {
                    return bridgeUnexpected(BridgeErrorCode::IntegerOverflow,
                                            "integer out of range for " + std::string(shortTypeNameOf<Int>.view()));
                }
                return static_cast<Int>(iv);
            } else {
                if (*d < 0.0 || *d > static_cast<double>(kMaxSafeJsUInt)) {
                    return bridgeUnexpected(BridgeErrorCode::IntegerOverflow,
                                            "number out of unsigned safe integer range");
                }
                const auto uv = static_cast<std::uint64_t>(*d);
                if (uv > static_cast<std::uint64_t>(hi)) {
                    return bridgeUnexpected(BridgeErrorCode::IntegerOverflow,
                                            "integer out of range for " + std::string(shortTypeNameOf<Int>.view()));
                }
                return static_cast<Int>(uv);
            }
        }
        return bridgeUnexpected(BridgeErrorCode::TypeMismatch,
                                "expected integer (" + std::string(shortTypeNameOf<Int>.view()) + "), got " +
                                        std::string(v.kindName()));
    }
};

template <std::floating_point Float>
struct MarshalTraits<Float> {
    static auto toBridge(const Float v, MarshalContext&) -> Expected<BridgeValue> {
        return BridgeValue{static_cast<double>(v)};
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext&) -> Expected<Float> {
        if (const auto* d = std::get_if<double>(&v.data)) {
            return static_cast<Float>(*d);
        }
        if (const auto* i = std::get_if<std::int64_t>(&v.data)) {
            return static_cast<Float>(*i);
        }
        if (const auto* u = std::get_if<std::uint64_t>(&v.data)) {
            return static_cast<Float>(*u);
        }
        return bridgeUnexpected(BridgeErrorCode::TypeMismatch, "expected number, got " + std::string(v.kindName()));
    }
};

template <class E>
    requires std::is_enum_v<E>
struct MarshalTraits<E> {
    using Underlying = std::underlying_type_t<E>;

    static auto toBridge(const E v, MarshalContext& ctx) -> Expected<BridgeValue> {
        return MarshalTraits<Underlying>::toBridge(static_cast<Underlying>(v), ctx);
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext& ctx) -> Expected<E> {
        auto raw = MarshalTraits<Underlying>::fromBridge(v, ctx);
        if (!raw) {
            return unexpected(std::move(raw).error());
        }
        return static_cast<E>(*raw);
    }
};

template <>
struct MarshalTraits<std::string> {
    static auto toBridge(const std::string& v, MarshalContext&) -> Expected<BridgeValue> {
        return BridgeValue{v};
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext&) -> Expected<std::string> {
        if (const auto* s = std::get_if<std::string>(&v.data)) {
            return *s;
        }
        return bridgeUnexpected(BridgeErrorCode::TypeMismatch, "expected string, got " + std::string(v.kindName()));
    }
};

template <>
struct MarshalTraits<std::string_view> {
    static auto toBridge(const std::string_view v, MarshalContext&) -> Expected<BridgeValue> {
        return BridgeValue{std::string{v}};
    }
};

template <std::size_t N>
struct MarshalTraits<char[N]> {
    static auto toBridge(const char (&v)[N], MarshalContext&) -> Expected<BridgeValue> {
        return BridgeValue{std::string{v}};
    }
};

template <>
struct MarshalTraits<const char*> {
    static auto toBridge(const char* const v, MarshalContext&) -> Expected<BridgeValue> {
        return BridgeValue{v != nullptr ? std::string{v} : std::string{}};
    }
};

template <>
struct MarshalTraits<std::vector<std::uint8_t>> {
    static auto toBridge(const std::vector<std::uint8_t>& v, MarshalContext&) -> Expected<BridgeValue> {
        return BridgeValue{BridgeValue::Bytes{v}};
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext&) -> Expected<std::vector<std::uint8_t>> {
        if (const auto* bytes = std::get_if<BridgeValue::Bytes>(&v.data)) {
            return *bytes;
        }
        return bridgeUnexpected(BridgeErrorCode::TypeMismatch, "expected Uint8Array, got " + std::string(v.kindName()));
    }
};

template <>
struct MarshalTraits<std::span<const std::uint8_t>> {
    static auto toBridge(const std::span<const std::uint8_t> v, MarshalContext&) -> Expected<BridgeValue> {
        return BridgeValue{BridgeValue::Bytes{v.begin(), v.end()}};
    }
};

template <std::size_t N>
struct MarshalTraits<std::array<std::uint8_t, N>> {
    static auto toBridge(const std::array<std::uint8_t, N>& v, MarshalContext&) -> Expected<BridgeValue> {
        return BridgeValue{BridgeValue::Bytes{v.begin(), v.end()}};
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext&) -> Expected<std::array<std::uint8_t, N>> {
        const auto* bytes = std::get_if<BridgeValue::Bytes>(&v.data);
        if (bytes == nullptr) {
            return bridgeUnexpected(BridgeErrorCode::TypeMismatch,
                                    "expected Uint8Array[" + std::to_string(N) + "], got " + std::string(v.kindName()));
        }
        if (bytes->size() != N) {
            return bridgeUnexpected(BridgeErrorCode::ArityMismatch,
                                    "expected Uint8Array of length " + std::to_string(N) + ", got length " +
                                            std::to_string(bytes->size()));
        }
        std::array<std::uint8_t, N> out{};
        for (std::size_t i = 0; i < N; ++i) {
            out[i] = (*bytes)[i];
        }
        return out;
    }
};

template <class T>
struct MarshalTraits<std::optional<T>> {
    static auto toBridge(const std::optional<T>& v, MarshalContext& ctx) -> Expected<BridgeValue> {
        if (!v.has_value()) {
            return BridgeValue{nullptr};
        }
        return Reflect::toBridge(*v, ctx);
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext& ctx) -> Expected<std::optional<T>> {
        if (v.isNull()) {
            return std::optional<T>{std::nullopt};
        }
        auto inner = Reflect::fromBridge<T>(v, ctx);
        if (!inner) {
            return unexpected(std::move(inner).error());
        }
        return std::optional<T>{std::move(*inner)};
    }
};

template <class... Ts>
struct MarshalTraits<std::variant<Ts...>> {
    static auto toBridge(const std::variant<Ts...>& v, MarshalContext& ctx) -> Expected<BridgeValue> {
        return std::visit([&](const auto& alt) { return Reflect::toBridge(alt, ctx); }, v);
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext& ctx) -> Expected<std::variant<Ts...>> {
        std::optional<std::variant<Ts...>> matched;
        (void) (([&]<class Alt>(std::type_identity<Alt>) {
                    auto res = Reflect::fromBridge<Alt>(v, ctx);
                    if (res) {
                        matched.emplace(std::in_place_type<Alt>, std::move(*res));
                        return true;
                    }
                    return false;
                }(std::type_identity<Ts>{})) ||
                ...);
        if (!matched) {
            return bridgeUnexpected(BridgeErrorCode::TypeMismatch,
                                    "value of type " + std::string(v.kindName()) +
                                            " did not match any variant alternative");
        }
        return std::move(*matched);
    }
};

template <class A, class B>
struct MarshalTraits<std::pair<A, B>> {
    static auto toBridge(const std::pair<A, B>& v, MarshalContext& ctx) -> Expected<BridgeValue> {
        auto first = Reflect::toBridge(v.first, ctx);
        if (!first) {
            return unexpected(prependErrorPath(std::move(first).error(), "[0]"));
        }
        auto second = Reflect::toBridge(v.second, ctx);
        if (!second) {
            return unexpected(prependErrorPath(std::move(second).error(), "[1]"));
        }
        BridgeValue::Array arr;
        arr.reserve(2);
        arr.push_back(std::move(*first));
        arr.push_back(std::move(*second));
        return BridgeValue{std::move(arr)};
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext& ctx) -> Expected<std::pair<A, B>> {
        const auto* arr = std::get_if<BridgeValue::Array>(&v.data);
        if (arr == nullptr || arr->size() != 2) {
            return bridgeUnexpected(BridgeErrorCode::TypeMismatch, "expected 2-element tuple array");
        }
        auto first = Reflect::fromBridge<A>((*arr)[0], ctx);
        if (!first) {
            return unexpected(prependErrorPath(std::move(first).error(), "[0]"));
        }
        auto second = Reflect::fromBridge<B>((*arr)[1], ctx);
        if (!second) {
            return unexpected(prependErrorPath(std::move(second).error(), "[1]"));
        }
        return std::pair<A, B>{std::move(*first), std::move(*second)};
    }
};

template <class... Ts>
struct MarshalTraits<std::tuple<Ts...>> {
    static auto toBridge(const std::tuple<Ts...>& v, MarshalContext& ctx) -> Expected<BridgeValue> {
        BridgeValue::Array arr;
        arr.reserve(sizeof...(Ts));
        std::optional<Error> err;
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (
                    [&] {
                        if (err) {
                            return;
                        }
                        auto elem = Reflect::toBridge(std::get<Is>(v), ctx);
                        if (!elem) {
                            err = prependErrorPath(std::move(elem).error(), "[" + std::to_string(Is) + "]");
                            return;
                        }
                        arr.push_back(std::move(*elem));
                    }(),
                    ...);
        }(std::index_sequence_for<Ts...>{});
        if (err) {
            return unexpected(*err);
        }
        return BridgeValue{std::move(arr)};
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext& ctx) -> Expected<std::tuple<Ts...>> {
        const auto* arr = std::get_if<BridgeValue::Array>(&v.data);
        if (arr == nullptr || arr->size() != sizeof...(Ts)) {
            return bridgeUnexpected(BridgeErrorCode::TypeMismatch,
                                    "expected " + std::to_string(sizeof...(Ts)) + "-element tuple array");
        }
        std::tuple<Ts...>    out{};
        std::optional<Error> err;
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (
                    [&] {
                        if (err) {
                            return;
                        }
                        auto elem = Reflect::fromBridge<Ts>((*arr)[Is], ctx);
                        if (!elem) {
                            err = prependErrorPath(std::move(elem).error(), "[" + std::to_string(Is) + "]");
                            return;
                        }
                        std::get<Is>(out) = std::move(*elem);
                    }(),
                    ...);
        }(std::index_sequence_for<Ts...>{});
        if (err) {
            return unexpected(*err);
        }
        return out;
    }
};

template <class T>
    requires(!std::same_as<T, std::uint8_t>)
struct MarshalTraits<std::vector<T>> {
    static auto toBridge(const std::vector<T>& v, MarshalContext& ctx) -> Expected<BridgeValue> {
        BridgeValue::Array arr;
        arr.reserve(v.size());
        for (std::size_t i = 0; i < v.size(); ++i) {
            auto elem = Reflect::toBridge(v[i], ctx);
            if (!elem) {
                return unexpected(prependErrorPath(std::move(elem).error(), "[" + std::to_string(i) + "]"));
            }
            arr.push_back(std::move(*elem));
        }
        return BridgeValue{std::move(arr)};
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext& ctx) -> Expected<std::vector<T>> {
        const auto* arr = std::get_if<BridgeValue::Array>(&v.data);
        if (arr == nullptr) {
            return bridgeUnexpected(BridgeErrorCode::TypeMismatch, "expected array, got " + std::string(v.kindName()));
        }
        std::vector<T> out;
        out.reserve(arr->size());
        for (std::size_t i = 0; i < arr->size(); ++i) {
            auto elem = Reflect::fromBridge<T>((*arr)[i], ctx);
            if (!elem) {
                return unexpected(prependErrorPath(std::move(elem).error(), "[" + std::to_string(i) + "]"));
            }
            out.push_back(std::move(*elem));
        }
        return out;
    }
};

template <class T, std::size_t N>
    requires(!std::same_as<T, std::uint8_t>)
struct MarshalTraits<std::array<T, N>> {
    static auto toBridge(const std::array<T, N>& v, MarshalContext& ctx) -> Expected<BridgeValue> {
        BridgeValue::Array arr;
        arr.reserve(N);
        for (std::size_t i = 0; i < N; ++i) {
            auto elem = Reflect::toBridge(v[i], ctx);
            if (!elem) {
                return unexpected(prependErrorPath(std::move(elem).error(), "[" + std::to_string(i) + "]"));
            }
            arr.push_back(std::move(*elem));
        }
        return BridgeValue{std::move(arr)};
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext& ctx) -> Expected<std::array<T, N>> {
        const auto* arr = std::get_if<BridgeValue::Array>(&v.data);
        if (arr == nullptr || arr->size() != N) {
            return bridgeUnexpected(BridgeErrorCode::TypeMismatch, "expected array of length " + std::to_string(N));
        }
        std::array<T, N> out{};
        for (std::size_t i = 0; i < N; ++i) {
            auto elem = Reflect::fromBridge<T>((*arr)[i], ctx);
            if (!elem) {
                return unexpected(prependErrorPath(std::move(elem).error(), "[" + std::to_string(i) + "]"));
            }
            out[i] = std::move(*elem);
        }
        return out;
    }
};

template <class V, class Compare, class Alloc>
struct MarshalTraits<std::map<std::string, V, Compare, Alloc>> {
    using MapType = std::map<std::string, V, Compare, Alloc>;

    static auto toBridge(const MapType& v, MarshalContext& ctx) -> Expected<BridgeValue> {
        BridgeValue::Object obj;
        obj.reserve(v.size());
        for (const auto& [k, val]: v) {
            auto elem = Reflect::toBridge(val, ctx);
            if (!elem) {
                return unexpected(prependErrorPath(std::move(elem).error(), "." + k));
            }
            obj.emplace_back(k, std::move(*elem));
        }
        return BridgeValue{std::move(obj)};
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext& ctx) -> Expected<MapType> {
        if (const auto* obj = std::get_if<BridgeValue::Object>(&v.data)) {
            MapType out;
            for (const auto& [k, val]: *obj) {
                auto elem = Reflect::fromBridge<V>(val, ctx);
                if (!elem) {
                    return unexpected(prependErrorPath(std::move(elem).error(), "." + k));
                }
                out.insert_or_assign(k, std::move(*elem));
            }
            return out;
        }
        if (const auto* remote = std::get_if<RemoteObject>(&v.data); remote != nullptr && *remote) {
            auto keys = (*remote)->propertyNames();
            if (!keys) {
                return unexpected(std::move(keys).error());
            }
            MapType out;
            for (const auto& k: *keys) {
                auto rawProp = (*remote)->getProperty(k);
                if (!rawProp) {
                    return unexpected(prependErrorPath(std::move(rawProp).error(), "." + k));
                }
                auto elem = Reflect::fromBridge<V>(*rawProp, ctx);
                if (!elem) {
                    return unexpected(prependErrorPath(std::move(elem).error(), "." + k));
                }
                out.insert_or_assign(k, std::move(*elem));
            }
            return out;
        }
        return bridgeUnexpected(BridgeErrorCode::TypeMismatch, "expected object map, got " + std::string(v.kindName()));
    }
};

template <class V, class Hash, class Eq, class Alloc>
struct MarshalTraits<std::unordered_map<std::string, V, Hash, Eq, Alloc>> {
    using MapType = std::unordered_map<std::string, V, Hash, Eq, Alloc>;

    static auto toBridge(const MapType& v, MarshalContext& ctx) -> Expected<BridgeValue> {
        BridgeValue::Object obj;
        obj.reserve(v.size());
        for (const auto& [k, val]: v) {
            auto elem = Reflect::toBridge(val, ctx);
            if (!elem) {
                return unexpected(prependErrorPath(std::move(elem).error(), "." + k));
            }
            obj.emplace_back(k, std::move(*elem));
        }
        return BridgeValue{std::move(obj)};
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext& ctx) -> Expected<MapType> {
        auto ordered = MarshalTraits<std::map<std::string, V>>::fromBridge(v, ctx);
        if (!ordered) {
            return unexpected(std::move(ordered).error());
        }
        return MapType{ordered->begin(), ordered->end()};
    }
};

template <class T>
struct MarshalTraits<Async<T>> {
    static auto toBridge(const Async<T>& v, MarshalContext&) -> Expected<BridgeValue> {
        return BridgeValue{v.rawState()};
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext& ctx) -> Expected<Async<T>> {
        if (const auto* async = std::get_if<BridgeAsync>(&v.data); async != nullptr && *async) {
            return Async<T>{*async};
        }
        if constexpr (std::is_void_v<T>) {
            return Async<void>::resolved();
        } else {
            auto direct = Reflect::fromBridge<T>(v, ctx);
            if (!direct) {
                return unexpected(std::move(direct).error());
            }
            return Async<T>::resolved(*direct);
        }
    }
};

template <class R, class... Args>
struct MarshalTraits<Callable<R(Args...)>> {
    static auto toBridge(const Callable<R(Args...)>& v, MarshalContext&) -> Expected<BridgeValue> {
        if (!v.valid()) {
            return BridgeValue{nullptr};
        }
        return BridgeValue{v.raw()};
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext&) -> Expected<Callable<R(Args...)>> {
        if (const auto* fn = std::get_if<BridgeCallable>(&v.data); fn != nullptr && *fn) {
            return Callable<R(Args...)>{*fn};
        }
        return bridgeUnexpected(BridgeErrorCode::TypeMismatch, "expected function, got " + std::string(v.kindName()));
    }
};

template <class R, class... Args>
struct MarshalTraits<std::function<R(Args...)>> {
    static auto toBridge(const std::function<R(Args...)>& v, MarshalContext&) -> Expected<BridgeValue> {
        if (!v) {
            return BridgeValue{nullptr};
        }
        return BridgeValue{Detail::makeCppBridgeCallable<std::function<R(Args...)>, R, Args...>(v)};
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext& ctx) -> Expected<std::function<R(Args...)>> {
        if (v.isNull()) {
            return std::function<R(Args...)>{};
        }
        auto callable = MarshalTraits<Callable<R(Args...)>>::fromBridge(v, ctx);
        if (!callable) {
            return unexpected(std::move(callable).error());
        }
        return std::function<R(Args...)>{
                [c = std::move(*callable)](Args... args) -> R { return c(std::forward<Args>(args)...); }};
    }
};

template <class Fn>
    requires(MonomorphicCallable<Fn> && !ReflectedClassType<Fn> && !ReflectedAggregate<Fn> && !std::is_function_v<Fn> &&
             !std::is_pointer_v<Fn> && !Detail::IsOptional<Fn>::value && !DisableAggregate<Fn>::value)
struct MarshalTraits<Fn> {
    using Traits = CallableTraits<Fn>;

    static auto toBridge(const Fn& fn, MarshalContext&) -> Expected<BridgeValue> {
        return [&]<class... Args>(std::type_identity<std::tuple<Args...>>) -> Expected<BridgeValue> {
            return BridgeValue{Detail::makeCppBridgeCallable<Fn, typename Traits::ReturnType, Args...>(fn)};
        }(std::type_identity<typename Traits::ArgsTuple>{});
    }
};

template <>
struct MarshalTraits<ObjectRef> {
    static auto toBridge(const ObjectRef& v, MarshalContext&) -> Expected<BridgeValue> {
        if (!v.valid()) {
            return BridgeValue{nullptr};
        }
        return BridgeValue{v.raw()};
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext&) -> Expected<ObjectRef> {
        if (const auto* remote = std::get_if<RemoteObject>(&v.data); remote != nullptr && *remote) {
            return ObjectRef{*remote};
        }
        return bridgeUnexpected(BridgeErrorCode::TypeMismatch,
                                "expected object reference, got " + std::string(v.kindName()));
    }
};

template <class I>
struct MarshalTraits<Proxy<I>> {
    static auto toBridge(const Proxy<I>& v, MarshalContext& ctx) -> Expected<BridgeValue> {
        return MarshalTraits<ObjectRef>::toBridge(v.object(), ctx);
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext& ctx) -> Expected<Proxy<I>> {
        auto obj = MarshalTraits<ObjectRef>::fromBridge(v, ctx);
        if (!obj) {
            return unexpected(std::move(obj).error());
        }
        if constexpr (ReflectedInterfaceType<I>) {
            if (obj->canAccessSync()) {
                const auto* desc = interfaceDescriptorOf<I>();
                for (const auto& req: desc->requirements) {
                    auto exists = obj->has(req.name);
                    if (!exists || !*exists) {
                        return bridgeUnexpected(BridgeErrorCode::MissingField,
                                                "object does not implement required member '" + std::string(req.name) +
                                                        "' of interface '" + std::string(desc->name) + "'");
                    }
                    if (req.isMethod) {
                        auto prop = obj->raw()->getProperty(req.name);
                        if (!prop || !std::holds_alternative<BridgeCallable>(prop->data)) {
                            return bridgeUnexpected(BridgeErrorCode::TypeMismatch,
                                                    "member '" + std::string(req.name) + "' of interface '" +
                                                            std::string(desc->name) + "' must be a function");
                        }
                    }
                }
            }
        }
        return Proxy<I>{std::move(*obj)};
    }
};

template <class T>
    requires ReflectedClassType<std::remove_const_t<T>>
struct MarshalTraits<std::shared_ptr<T>> {
    using Clean = std::remove_const_t<T>;

    static auto toBridge(const std::shared_ptr<T>& v, MarshalContext&) -> Expected<BridgeValue> {
        if (!v) {
            return BridgeValue{nullptr};
        }
        auto owner = currentOwnerPluginName();
        auto cell  = std::make_shared<RevocableObjectCell>(std::const_pointer_cast<Clean>(v),
                                                           const_cast<Clean*>(v.get()),
                                                           classDescriptorOf<Clean>(),
                                                           std::is_const_v<T>,
                                                           owner,
                                                           acquirePluginActivity(owner));
        registerPluginObjectCell(owner, cell);
        return BridgeValue{std::move(cell)};
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext&) -> Expected<std::shared_ptr<T>> {
        if (v.isNull()) {
            return std::shared_ptr<T>{};
        }
        const auto* cellPtr = std::get_if<std::shared_ptr<RevocableObjectCell>>(&v.data);
        if (cellPtr == nullptr || !*cellPtr) {
            return bridgeUnexpected(BridgeErrorCode::TypeMismatch,
                                    "expected object '" + std::string(shortTypeNameOf<Clean>.view()) + "', got " +
                                            std::string(v.kindName()));
        }
        const auto& cell = *cellPtr;
        if (!std::is_const_v<T> && cell->isConst()) {
            return bridgeUnexpected(BridgeErrorCode::ReadonlyViolation,
                                    "cannot pass read-only object '" + std::string(cell->typeName()) +
                                            "' as mutable reference");
        }
        auto [sharedVoid, rawPtr] = cell->shareRaw();
        const auto* desc          = cell->descriptor();
        if (!sharedVoid || rawPtr == nullptr || desc == nullptr) {
            return bridgeUnexpected(BridgeErrorCode::ObjectRevoked,
                                    "object '" + std::string(cell->typeName()) + "' is no longer available");
        }
        void* casted = desc->castTo(rawPtr, typeIdOf<Clean>);
        if (casted == nullptr) {
            return bridgeUnexpected(BridgeErrorCode::TypeMismatch,
                                    "expected object '" + std::string(shortTypeNameOf<Clean>.view()) +
                                            "', got incompatible '" + std::string(desc->name) + "'");
        }
        return std::shared_ptr<T>{std::move(sharedVoid), static_cast<T*>(casted)};
    }
};

template <class T>
    requires ReflectedClassType<std::remove_const_t<T>>
struct MarshalTraits<std::weak_ptr<T>> {
    static auto toBridge(const std::weak_ptr<T>& v, MarshalContext& ctx) -> Expected<BridgeValue> {
        return MarshalTraits<std::shared_ptr<T>>::toBridge(v.lock(), ctx);
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext& ctx) -> Expected<std::weak_ptr<T>> {
        auto sp = MarshalTraits<std::shared_ptr<T>>::fromBridge(v, ctx);
        if (!sp) {
            return unexpected(std::move(sp).error());
        }
        return std::weak_ptr<T>{*sp};
    }
};

template <class T>
    requires ReflectedClassType<std::remove_const_t<T>>
struct MarshalTraits<Borrowed<T>> {
    using Clean = std::remove_const_t<T>;

    static auto toBridge(const Borrowed<T>& v, MarshalContext& ctx) -> Expected<BridgeValue> {
        if (v.ptr == nullptr) {
            return BridgeValue{nullptr};
        }
        if (ctx.activeLease == nullptr) {
            return bridgeUnexpected(BridgeErrorCode::BorrowExpired,
                                    "borrowed object is only valid during synchronous calls");
        }
        BorrowedObjectHandle handle{
                .rawPtr      = const_cast<Clean*>(v.ptr),
                .descriptor  = classDescriptorOf<Clean>(),
                .isConst     = std::is_const_v<T>,
                .ownerPlugin = currentOwnerPluginName(),
                .lease       = nullptr,
        };
        auto subLease = std::make_shared<BorrowLease>();
        ctx.activeLease->addRevokeHook([subLease]() noexcept { subLease->revoke(); });
        handle.lease = std::move(subLease);
        return BridgeValue{std::move(handle)};
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext&) -> Expected<Borrowed<T>> {
        if (v.isNull()) {
            return Borrowed<T>{nullptr};
        }
        if (const auto* borrowed = std::get_if<BorrowedObjectHandle>(&v.data)) {
            if (!borrowed->alive()) {
                return bridgeUnexpected(BridgeErrorCode::BorrowExpired, "borrowed object has expired");
            }
            if (!std::is_const_v<T> && borrowed->isConst) {
                return bridgeUnexpected(BridgeErrorCode::ReadonlyViolation, "cannot mutably borrow a read-only object");
            }
            void* casted = borrowed->descriptor->castTo(borrowed->rawPtr, typeIdOf<Clean>);
            if (casted == nullptr) {
                return bridgeUnexpected(BridgeErrorCode::TypeMismatch,
                                        "expected borrowed '" + std::string(shortTypeNameOf<Clean>.view()) +
                                                "', got '" + std::string(borrowed->descriptor->name) + "'");
            }
            return Borrowed<T>{static_cast<T*>(casted)};
        }
        if (const auto* cellPtr = std::get_if<std::shared_ptr<RevocableObjectCell>>(&v.data);
            cellPtr != nullptr && *cellPtr) {
            const auto& cell = *cellPtr;
            if (!std::is_const_v<T> && cell->isConst()) {
                return bridgeUnexpected(BridgeErrorCode::ReadonlyViolation, "cannot mutably borrow a read-only object");
            }
            auto [sharedVoid, rawPtr] = cell->shareRaw();
            const auto* desc          = cell->descriptor();
            if (!sharedVoid || rawPtr == nullptr || desc == nullptr || !cell->alive()) {
                return bridgeUnexpected(BridgeErrorCode::ObjectRevoked, "object is no longer available");
            }
            void* casted = desc->castTo(rawPtr, typeIdOf<Clean>);
            if (casted == nullptr) {
                return bridgeUnexpected(BridgeErrorCode::TypeMismatch,
                                        "expected '" + std::string(shortTypeNameOf<Clean>.view()) + "', got '" +
                                                std::string(desc->name) + "'");
            }
            return Borrowed<T>{static_cast<T*>(casted)};
        }
        return bridgeUnexpected(BridgeErrorCode::TypeMismatch,
                                "expected object '" + std::string(shortTypeNameOf<Clean>.view()) + "'");
    }
};

template <ReflectedAggregate T>
struct MarshalTraits<T> {
    static constexpr std::size_t kArity = aggregateArity<T>;

    static auto toBridge(const T& v, MarshalContext& ctx) -> Expected<BridgeValue> {
        BridgeValue::Object obj;
        obj.reserve(kArity);
        const auto           refs = tieAggregate(v);
        std::optional<Error> err;
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (
                    [&] {
                        if (err) {
                            return;
                        }
                        constexpr auto kFieldName = aggregateFieldName<T, Is>;
                        auto           fieldVal   = Reflect::toBridge(std::get<Is>(refs), ctx);
                        if (!fieldVal) {
                            err = prependErrorPath(std::move(fieldVal).error(), "." + std::string(kFieldName));
                            return;
                        }
                        obj.emplace_back(std::string(kFieldName), std::move(*fieldVal));
                    }(),
                    ...);
        }(std::make_index_sequence<kArity>{});
        if (err) {
            return unexpected(*err);
        }
        return BridgeValue{std::move(obj)};
    }

    static auto fromBridge(const BridgeValue& v, MarshalContext& ctx) -> Expected<T> {
        const auto* obj    = std::get_if<BridgeValue::Object>(&v.data);
        const auto* remote = std::get_if<RemoteObject>(&v.data);
        if (obj == nullptr && (remote == nullptr || !*remote)) {
            return bridgeUnexpected(BridgeErrorCode::TypeMismatch,
                                    "expected object for struct '" + std::string(shortTypeNameOf<T>.view()) +
                                            "', got " + std::string(v.kindName()));
        }

        T                    out{};
        auto                 refs = tieAggregate(out);
        std::optional<Error> err;

        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (
                    [&] {
                        if (err) {
                            return;
                        }
                        constexpr auto kFieldName = aggregateFieldName<T, Is>;
                        using FieldT              = std::remove_cvref_t<decltype(std::get<Is>(refs))>;

                        std::optional<BridgeValue> fetched;
                        const BridgeValue*         fieldPtr = nullptr;

                        if (obj != nullptr) {
                            fieldPtr = v.findField(kFieldName);
                        } else {
                            auto hasProp = (*remote)->hasProperty(kFieldName);
                            if (!hasProp) {
                                err = prependErrorPath(std::move(hasProp).error(), "." + std::string(kFieldName));
                                return;
                            }
                            if (*hasProp) {
                                auto rawProp = (*remote)->getProperty(kFieldName);
                                if (!rawProp) {
                                    err = prependErrorPath(std::move(rawProp).error(), "." + std::string(kFieldName));
                                    return;
                                }
                                fetched.emplace(std::move(*rawProp));
                                fieldPtr = &*fetched;
                            }
                        }

                        if (fieldPtr == nullptr) {
                            if constexpr (Detail::IsOptional<FieldT>::value) {
                                std::get<Is>(refs) = std::nullopt;
                                return;
                            } else {
                                err = bridgeError(BridgeErrorCode::MissingField,
                                                  "." + std::string(kFieldName) + ": missing required field in '" +
                                                          std::string(shortTypeNameOf<T>.view()) + "'");
                                return;
                            }
                        }

                        auto converted = Reflect::fromBridge<FieldT>(*fieldPtr, ctx);
                        if (!converted) {
                            err = prependErrorPath(std::move(converted).error(), "." + std::string(kFieldName));
                            return;
                        }
                        std::get<Is>(refs) = std::move(*converted);
                    }(),
                    ...);
        }(std::make_index_sequence<kArity>{});

        if (err) {
            return unexpected(*err);
        }
        return out;
    }
};

template <class T>
[[nodiscard]] auto schemaOf() -> const TypeSchema* {
    using Clean = std::remove_cvref_t<T>;
    if constexpr (std::is_void_v<Clean>) {
        static constexpr TypeSchema s{.kind = SchemaKind::Void, .name = "void", .typeId = typeIdOf<void>};
        return &s;
    } else if constexpr (std::same_as<Clean, std::nullptr_t>) {
        static constexpr TypeSchema s{.kind = SchemaKind::Null, .name = "null", .typeId = typeIdOf<std::nullptr_t>};
        return &s;
    } else if constexpr (std::same_as<Clean, bool>) {
        static constexpr TypeSchema s{.kind = SchemaKind::Bool, .name = "boolean", .typeId = typeIdOf<bool>};
        return &s;
    } else if constexpr (std::integral<Clean> && std::is_signed_v<Clean>) {
        static constexpr TypeSchema s{
                .kind = SchemaKind::Int64, .name = shortTypeNameOf<Clean>.view(), .typeId = typeIdOf<Clean>};
        return &s;
    } else if constexpr (std::integral<Clean> && std::is_unsigned_v<Clean>) {
        static constexpr TypeSchema s{
                .kind = SchemaKind::UInt64, .name = shortTypeNameOf<Clean>.view(), .typeId = typeIdOf<Clean>};
        return &s;
    } else if constexpr (std::floating_point<Clean>) {
        static constexpr TypeSchema s{.kind = SchemaKind::Float64, .name = "number", .typeId = typeIdOf<Clean>};
        return &s;
    } else if constexpr (std::is_enum_v<Clean>) {
        static constexpr TypeSchema s{
                .kind = SchemaKind::Enum, .name = shortTypeNameOf<Clean>.view(), .typeId = typeIdOf<Clean>};
        return &s;
    } else if constexpr (std::same_as<Clean, std::string> || std::same_as<Clean, std::string_view> ||
                         std::same_as<Clean, const char*>) {
        static constexpr TypeSchema s{.kind = SchemaKind::String, .name = "string", .typeId = typeIdOf<std::string>};
        return &s;
    } else if constexpr (std::same_as<Clean, std::vector<std::uint8_t>> ||
                         std::same_as<Clean, std::span<const std::uint8_t>>) {
        static constexpr TypeSchema s{
                .kind = SchemaKind::Bytes, .name = "Uint8Array", .typeId = typeIdOf<std::vector<std::uint8_t>>};
        return &s;
    } else if constexpr (Detail::IsOptional<Clean>::value) {
        static const TypeSchema s{
                .kind        = SchemaKind::Optional,
                .name        = shortTypeNameOf<Clean>.view(),
                .typeId      = typeIdOf<Clean>,
                .elementType = schemaOf<typename Clean::value_type>(),
        };
        return &s;
    } else if constexpr (Detail::IsAsync<Clean>::value) {
        static const TypeSchema s{
                .kind        = SchemaKind::Async,
                .name        = "Promise",
                .typeId      = typeIdOf<Clean>,
                .elementType = schemaOf<typename Detail::IsAsync<Clean>::ValueType>(),
        };
        return &s;
    } else if constexpr (std::same_as<Clean, ObjectRef>) {
        static constexpr TypeSchema s{
                .kind = SchemaKind::RemoteObject, .name = "object", .typeId = typeIdOf<ObjectRef>};
        return &s;
    } else if constexpr (ReflectedAggregate<Clean>) {
        struct AggHolder {
            TypeSchema               schema{};
            std::vector<FieldSchema> fields;
        };
        static const AggHolder* holder = [] {
            auto* h          = new AggHolder{};
            h->schema.kind   = SchemaKind::Aggregate;
            h->schema.name   = shortTypeNameOf<Clean>.view();
            h->schema.typeId = typeIdOf<Clean>;
            Clean                 dummy{};
            [[maybe_unused]] auto refs = tieAggregate(dummy);
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                (
                        [&] {
                            using FT = std::remove_cvref_t<decltype(std::get<Is>(refs))>;
                            h->fields.push_back(FieldSchema{
                                    .name     = aggregateFieldName<Clean, Is>,
                                    .type     = schemaOf<FT>(),
                                    .readonly = false,
                                    .optional = Detail::IsOptional<FT>::value,
                            });
                        }(),
                        ...);
            }(std::make_index_sequence<aggregateArity<Clean>>{});
            h->schema.fields = h->fields;
            return h;
        }();
        return &holder->schema;
    } else {
        static constexpr TypeSchema s{
                .kind = SchemaKind::Any, .name = shortTypeNameOf<Clean>.view(), .typeId = typeIdOf<Clean>};
        return &s;
    }
}

} // namespace CloverNT::Reflect
