#pragma once

#include <CloverNT/API/Events/BuiltInEvents.hpp>
#include <CloverNT/API/Events/EventTopic.hpp>
#include <CloverNT/API/Events/Listener.hpp>
#include <CloverNT/API/Events/Subscription.hpp>
#include <CloverNT/API/Expected.hpp>
#include <CloverNT/API/Macros.hpp>
#include <CloverNT/API/Reflect/Reflect.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace CloverNT::Event {

namespace Detail {

    template <class T>
    struct IsContextType : std::false_type {};

    template <>
    struct IsContextType<ReadOnlyEventContext> : std::true_type {};

    template <bool C>
    struct IsContextType<InterceptEventContext<C>> : std::true_type {};

    template <class T>
    inline constexpr bool kIsContextParam = IsContextType<std::remove_cvref_t<T>>::value;

    template <class T>
    inline constexpr bool kIsMutableRefParam =
            std::is_lvalue_reference_v<T> && !std::is_const_v<std::remove_reference_t<T>>;

    template <class Tuple, std::size_t... Is>
    consteval auto sliceTupleTypes(std::index_sequence<Is...>) -> std::tuple<std::tuple_element_t<Is, Tuple>...>;

    template <class ArgsTuple>
    struct SplitCallbackParams {
        static constexpr std::size_t kTotalArity = std::tuple_size_v<ArgsTuple>;
        static constexpr bool        kHasContext = [] {
            if constexpr (kTotalArity == 0) {
                return false;
            } else {
                return kIsContextParam<std::tuple_element_t<kTotalArity - 1, ArgsTuple>>;
            }
        }();
        static constexpr std::size_t kPayloadArity = kHasContext ? kTotalArity - 1 : kTotalArity;

        using PayloadTuple = decltype(sliceTupleTypes<ArgsTuple>(std::make_index_sequence<kPayloadArity>{}));
        using ContextParam =
                std::conditional_t<kHasContext,
                                   std::tuple_element_t<kHasContext ? (kTotalArity - 1) : 0,
                                                        std::conditional_t<kHasContext, ArgsTuple, std::tuple<void>>>,
                                   void>;
    };

    template <class PayloadTuple>
    consteval bool hasAnyMutablePayloadRef() {
        return []<std::size_t... Is>(std::index_sequence<Is...>) {
            return (kIsMutableRefParam<std::tuple_element_t<Is, PayloadTuple>> || ... || false);
        }(std::make_index_sequence<std::tuple_size_v<PayloadTuple>>{});
    }

    CloverNT_API void
    reportListenerDispatchError(std::string_view topic, std::string_view ownerPlugin, const Error& error) noexcept;

    template <class Fn, class PayloadTuple, class ContextParam>
    class CppCallbackSlot final : public ListenerSlot {
    public:
        static constexpr std::size_t kArity   = std::tuple_size_v<PayloadTuple>;
        static constexpr bool        kHasCtx  = !std::is_void_v<ContextParam>;
        static constexpr bool        kMutates = hasAnyMutablePayloadRef<PayloadTuple>();

        CppCallbackSlot(SubscriptionId                           id,
                        EventPriority                            priority,
                        bool                                     once,
                        std::string                              ownerPlugin,
                        std::shared_ptr<Reflect::PluginActivity> activity,
                        Fn                                       fn)
            : ListenerSlot(id, priority, once, std::move(ownerPlugin), std::move(activity)), mFn(std::move(fn)) {}

        void dispatch(EventInvocation& inv) override {
            if (canUseNativeFastPath(inv, std::make_index_sequence<kArity>{})) {
                invokeFastPath(inv, std::make_index_sequence<kArity>{});
                if constexpr (kMutates) {
                    inv.invalidateBridgeCache();
                }
                return;
            }
            invokeReflectedPath(inv, std::make_index_sequence<kArity>{});
        }

    private:
        template <std::size_t... Is>
        [[nodiscard]] static bool canUseNativeFastPath(const EventInvocation& inv,
                                                       std::index_sequence<Is...>) noexcept {
            const auto native = inv.nativeArgs();
            if constexpr (kArity == 0) {
                return true;
            } else {
                if (native.size() < kArity) {
                    return false;
                }
                if constexpr (kMutates) {
                    if (!inv.isMutablePayload()) {
                        return false;
                    }
                }
                return (matchesNativeArg<std::tuple_element_t<Is, PayloadTuple>>(native[Is]) && ...);
            }
        }

        template <class ParamT>
        [[nodiscard]] static bool matchesNativeArg(const NativeArg& arg) noexcept {
            using Clean = std::remove_cvref_t<ParamT>;
            if (arg.ptr == nullptr || arg.typeId != Reflect::typeIdOf<Clean>) {
                return false;
            }
            if constexpr (kIsMutableRefParam<ParamT>) {
                if (arg.isConst) {
                    return false;
                }
            }
            return true;
        }

        template <class CtxParam>
        [[nodiscard]] auto makeContext(EventInvocation& inv) const noexcept {
            using CleanCtx = std::remove_cvref_t<CtxParam>;
            if constexpr (std::same_as<CleanCtx, ReadOnlyEventContext>) {
                return inv.makeReadOnlyContext(priority(), DeliveryMode::Sync);
            } else if constexpr (std::same_as<CleanCtx, InterceptEventContext<false>>) {
                return inv.makeInterceptContext<false>(priority(), DeliveryMode::Sync);
            } else {
                return inv.makeInterceptContext<true>(priority(), DeliveryMode::Sync);
            }
        }

        template <class RetFn>
        void runAndCheckReturn(const EventInvocation& inv, RetFn&& call) {
            using Ret = std::invoke_result_t<RetFn>;
            if constexpr (Reflect::Detail::IsExpected<Ret>::value) {
                auto res = call();
                if (!res) {
                    reportListenerDispatchError(inv.topic(), ownerPlugin(), res.error());
                }
            } else {
                (void) call();
            }
        }

        template <std::size_t... Is>
        void invokeFastPath(EventInvocation& inv, std::index_sequence<Is...>) {
            const auto native = inv.nativeArgs();
            if constexpr (kHasCtx) {
                auto ctx = makeContext<ContextParam>(inv);
                runAndCheckReturn(inv, [&]() -> decltype(auto) {
                    return mFn(*static_cast<std::remove_cvref_t<std::tuple_element_t<Is, PayloadTuple>>*>(
                                       native[Is].ptr)...,
                               ctx);
                });
            } else {
                runAndCheckReturn(inv, [&]() -> decltype(auto) {
                    return mFn(*static_cast<std::remove_cvref_t<std::tuple_element_t<Is, PayloadTuple>>*>(
                            native[Is].ptr)...);
                });
            }
        }

        template <std::size_t... Is>
        void invokeReflectedPath(EventInvocation& inv, std::index_sequence<Is...>) {
            if constexpr (kMutates) {
                if (!inv.isMutablePayload()) {
                    reportListenerDispatchError(
                            inv.topic(),
                            ownerPlugin(),
                            Reflect::bridgeError(Reflect::BridgeErrorCode::ReadonlyViolation,
                                                 "mutable reference listener invoked on read-only event '" +
                                                         std::string(inv.topic()) + "'"));
                    return;
                }
            }

            auto bridgeSpan = inv.bridgeArgs();
            if (!bridgeSpan) {
                reportListenerDispatchError(inv.topic(), ownerPlugin(), bridgeSpan.error());
                return;
            }

            [[maybe_unused]] Reflect::MarshalContext mctx{.activeLease = inv.borrowLease().get()};

            std::tuple<std::optional<std::remove_cvref_t<std::tuple_element_t<Is, PayloadTuple>>>...> unpacked;
            std::optional<Error>                                                                      err;

            (
                    [&] {
                        if (err) {
                            return;
                        }
                        using Clean = std::remove_cvref_t<std::tuple_element_t<Is, PayloadTuple>>;
                        if (Is >= bridgeSpan->size()) {
                            if constexpr (Reflect::Detail::IsOptional<Clean>::value) {
                                std::get<Is>(unpacked).emplace(std::nullopt);
                                return;
                            } else {
                                err = Reflect::bridgeError(Reflect::BridgeErrorCode::ArityMismatch,
                                                           "arg[" + std::to_string(Is) + "]: missing argument on '" +
                                                                   std::string(inv.topic()) + "'");
                                return;
                            }
                        }
                        auto converted = Reflect::fromBridge<Clean>((*bridgeSpan)[Is], mctx);
                        if (!converted) {
                            err = Reflect::prependErrorPath(std::move(converted).error(),
                                                            "arg[" + std::to_string(Is) + "]");
                            return;
                        }
                        std::get<Is>(unpacked).emplace(std::move(*converted));
                    }(),
                    ...);

            if (err) {
                reportListenerDispatchError(inv.topic(), ownerPlugin(), *err);
                return;
            }

            if constexpr (kHasCtx) {
                auto ctx = makeContext<ContextParam>(inv);
                runAndCheckReturn(inv, [&]() -> decltype(auto) { return mFn(*std::get<Is>(unpacked)..., ctx); });
            } else {
                runAndCheckReturn(inv, [&]() -> decltype(auto) { return mFn(*std::get<Is>(unpacked)...); });
            }

            if constexpr (kMutates) {
                std::vector             updated(bridgeSpan->begin(), bridgeSpan->end());
                Reflect::MarshalContext outCtx{.activeLease = inv.borrowLease().get()};
                (
                        [&] {
                            if (err) {
                                return;
                            }
                            if constexpr (kIsMutableRefParam<std::tuple_element_t<Is, PayloadTuple>>) {
                                auto marshaled = Reflect::toBridge(*std::get<Is>(unpacked), outCtx);
                                if (!marshaled) {
                                    err = Reflect::prependErrorPath(std::move(marshaled).error(),
                                                                    "arg[" + std::to_string(Is) + "]");
                                    return;
                                }
                                if (Is < updated.size()) {
                                    updated[Is] = std::move(*marshaled);
                                }
                            }
                        }(),
                        ...);
                if (!err) {
                    if (auto committed = inv.commitBridgeMutation(updated); !committed) {
                        err = committed.error();
                    }
                }
                if (err) {
                    reportListenerDispatchError(inv.topic(), ownerPlugin(), *err);
                }
            }
        }

        Fn mFn;
    };

    template <class Arg>
    [[nodiscard]] auto makeNativeArg(Arg&& arg) noexcept -> NativeArg {
        using Unref           = std::remove_reference_t<Arg>;
        using Clean           = std::remove_cv_t<Unref>;
        constexpr bool kConst = std::is_const_v<Unref> || !std::is_lvalue_reference_v<Arg>;
        return NativeArg{
                .typeId  = Reflect::typeIdOf<Clean>,
                .ptr     = const_cast<void*>(static_cast<const void*>(std::addressof(arg))),
                .isConst = kConst,
        };
    }

} // namespace Detail

class CloverNT_API EventBus final {
public:
    EventBus() = delete;

    template <Detail::IsEventTopic Topic, class Callback>
    [[nodiscard]] static auto on(Callback&&       callback,
                                 EventPriority    priority      = EventPriority::Normal,
                                 std::string_view ownerOverride = {}) -> Expected<ScopedSubscription> {
        return subscribeTyped<Topic>(std::forward<Callback>(callback), priority, /*once=*/false, ownerOverride);
    }

    template <Detail::IsEventTopic Topic, class Callback>
    [[nodiscard]] static auto once(Callback&&       callback,
                                   EventPriority    priority      = EventPriority::Normal,
                                   std::string_view ownerOverride = {}) -> Expected<ScopedSubscription> {
        return subscribeTyped<Topic>(std::forward<Callback>(callback), priority, /*once=*/true, ownerOverride);
    }

    template <Detail::IsEventTopic Topic, class Callback>
    [[nodiscard]] static auto monitor(Callback&& callback, std::string_view ownerOverride = {})
            -> Expected<ScopedSubscription> {
        using Traits = Reflect::CallableTraits<std::decay_t<Callback>>;
        static_assert(Traits::valid, "EventBus::monitor callback must have an unambiguous call signature");
        using Split = Detail::SplitCallbackParams<typename Traits::ArgsTuple>;
        if constexpr (std::is_void_v<typename Topic::Payload>) {
            static_assert(Split::kPayloadArity == 0, "Void EventTopic callbacks do not take a payload parameter");
        } else {
            static_assert(Split::kPayloadArity <= 1, "EventTopic callbacks accept at most 1 payload parameter");
        }
        static_assert(!Detail::hasAnyMutablePayloadRef<typename Split::PayloadTuple>(),
                      "Monitor listeners are strictly read-only; take `const Payload&` or `Payload` by value");
        if constexpr (Split::kHasContext) {
            static_assert(!std::same_as<std::remove_cvref_t<typename Split::ContextParam>, InterceptEventContext<true>>,
                          "Monitor listeners cannot cancel events; take `const ReadOnlyEventContext&` instead");
        }
        if constexpr (!std::is_void_v<typename Topic::Payload> && Split::kPayloadArity == 1) {
            using P0 = std::remove_cvref_t<std::tuple_element_t<0, typename Split::PayloadTuple>>;
            static_assert(std::same_as<P0, typename Topic::Payload> || std::same_as<P0, Reflect::ObjectRef>,
                          "Callback payload type does not match Topic::Payload");
        }
        return subscribeImpl(Topic::topic,
                             std::forward<Callback>(callback),
                             EventPriority::Monitor,
                             /*once=*/
                             false,
                             ownerOverride,
                             /*normalizeBare=*/
                             false);
    }

    template <Detail::IsEventTopic Topic, class PayloadRef>
        requires(!std::is_void_v<typename Topic::Payload> && Topic::isKernelOnly &&
                 std::same_as<std::remove_cvref_t<PayloadRef>, typename Topic::Payload>)
    static auto emit(PayloadRef&& payload, KernelPublishToken token) -> Expected<bool> {
        if constexpr (Topic::isMutable) {
            static_assert(std::is_lvalue_reference_v<PayloadRef> &&
                                  !std::is_const_v<std::remove_reference_t<PayloadRef>>,
                          "Mutable EventTopic requires a non-const lvalue Payload& on emit");
        }
        return emitKernelPayload<Topic>(std::forward<PayloadRef>(payload), token);
    }

    template <Detail::IsEventTopic Topic>
        requires(std::is_void_v<typename Topic::Payload> && Topic::isKernelOnly)
    static auto emit(const KernelPublishToken token) -> Expected<bool> {
        EventInvocation inv{Topic::topic,
                            "CloverCore",
                            Topic::isCancellable,
                            /*mutablePayload=*/false,
                            {},
                            [](Reflect::MarshalContext&) -> Expected<std::vector<Reflect::BridgeValue>> {
                                return std::vector<Reflect::BridgeValue>{};
                            },
                            {}};
        return dispatchKernelInvocation(inv, token);
    }

    template <Detail::IsEventTopic Topic, class PayloadRef>
        requires(!std::is_void_v<typename Topic::Payload> && !Topic::isKernelOnly &&
                 std::same_as<std::remove_cvref_t<PayloadRef>, typename Topic::Payload>)
    static auto emit(PayloadRef&& payload) -> Expected<bool> {
        if constexpr (Topic::isMutable) {
            static_assert(std::is_lvalue_reference_v<PayloadRef> &&
                                  !std::is_const_v<std::remove_reference_t<PayloadRef>>,
                          "Mutable EventTopic requires a non-const lvalue Payload& on emit");
        }
        return emitArgsImpl(Topic::topic,
                            Topic::isCancellable,
                            Topic::isMutable,
                            /*normalizeBare=*/
                            false,
                            std::forward<PayloadRef>(payload));
    }

    template <Detail::IsEventTopic Topic>
        requires(std::is_void_v<typename Topic::Payload> && !Topic::isKernelOnly)
    static auto emit() -> Expected<bool> {
        return emitArgsImpl(Topic::topic, Topic::isCancellable, /*mutablePayload=*/false, /*normalizeBare=*/false);
    }

    template <class Callback>
    [[nodiscard]] static auto on(std::string_view topic,
                                 Callback&&       callback,
                                 EventPriority    priority      = EventPriority::Normal,
                                 std::string_view ownerOverride = {}) -> Expected<ScopedSubscription> {
        using Traits = Reflect::CallableTraits<std::decay_t<Callback>>;
        static_assert(Traits::valid, "EventBus::on callback must have an unambiguous call signature");
        if (priority == EventPriority::Monitor) {
            using Split = Detail::SplitCallbackParams<typename Traits::ArgsTuple>;
            if constexpr (Detail::hasAnyMutablePayloadRef<typename Split::PayloadTuple>() ||
                          (Split::kHasContext && std::same_as<std::remove_cvref_t<typename Split::ContextParam>,
                                                              InterceptEventContext<true>>) ) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ReadonlyViolation,
                                                 "monitor listeners cannot mutate event arguments or cancel events");
            }
        }
        return subscribeImpl(topic,
                             std::forward<Callback>(callback),
                             priority,
                             /*once=*/
                             false,
                             ownerOverride,
                             /*normalizeBare=*/
                             true);
    }

    template <class Callback>
    [[nodiscard]] static auto once(std::string_view topic,
                                   Callback&&       callback,
                                   EventPriority    priority      = EventPriority::Normal,
                                   std::string_view ownerOverride = {}) -> Expected<ScopedSubscription> {
        using Traits = Reflect::CallableTraits<std::decay_t<Callback>>;
        static_assert(Traits::valid, "EventBus::once callback must have an unambiguous call signature");
        if (priority == EventPriority::Monitor) {
            using Split = Detail::SplitCallbackParams<typename Traits::ArgsTuple>;
            if constexpr (Detail::hasAnyMutablePayloadRef<typename Split::PayloadTuple>() ||
                          (Split::kHasContext && std::same_as<std::remove_cvref_t<typename Split::ContextParam>,
                                                              InterceptEventContext<true>>) ) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ReadonlyViolation,
                                                 "monitor listeners cannot mutate event arguments or cancel events");
            }
        }
        return subscribeImpl(topic,
                             std::forward<Callback>(callback),
                             priority,
                             /*once=*/
                             true,
                             ownerOverride,
                             /*normalizeBare=*/
                             true);
    }

    template <class Callback>
    [[nodiscard]] static auto monitor(std::string_view topic, Callback&& callback, std::string_view ownerOverride = {})
            -> Expected<ScopedSubscription> {
        using Traits = Reflect::CallableTraits<std::decay_t<Callback>>;
        static_assert(Traits::valid, "EventBus::monitor callback must have an unambiguous call signature");
        using Split = Detail::SplitCallbackParams<typename Traits::ArgsTuple>;
        static_assert(!Detail::hasAnyMutablePayloadRef<typename Split::PayloadTuple>(),
                      "Monitor listeners are strictly read-only; take `const T&` or `T` by value");
        if constexpr (Split::kHasContext) {
            static_assert(!std::same_as<std::remove_cvref_t<typename Split::ContextParam>, InterceptEventContext<true>>,
                          "Monitor listeners cannot cancel events; take `const ReadOnlyEventContext&` instead");
        }
        return subscribeImpl(topic,
                             std::forward<Callback>(callback),
                             EventPriority::Monitor,
                             /*once=*/
                             false,
                             ownerOverride,
                             /*normalizeBare=*/
                             true);
    }

    template <class... Args>
    static auto emit(std::string_view topic, Args&&... args) -> Expected<bool> {
        constexpr bool kAnyMutable =
                ((std::is_lvalue_reference_v<Args> && !std::is_const_v<std::remove_reference_t<Args>>) || ... || false);
        return emitArgsImpl(topic,
                            /*cancellable=*/
                            true,
                            /*mutablePayload=*/
                            kAnyMutable,
                            /*normalizeBare=*/
                            true,
                            std::forward<Args>(args)...);
    }

    [[nodiscard]] static auto allocateSubscriptionId() noexcept -> SubscriptionId;

    [[nodiscard]] static auto normalizeSubscribeTopic(std::string_view rawTopic,
                                                      std::string_view ownerPlugin,
                                                      bool             normalizeBare = true) -> Expected<std::string>;

    [[nodiscard]] static auto normalizeEmitTopic(std::string_view rawTopic,
                                                 std::string_view ownerPlugin,
                                                 bool             normalizeBare = true) -> Expected<std::string>;

    [[nodiscard]] static auto addSlot(std::string_view topic, ListenerSlotPtr slot) -> Expected<SubscriptionId>;
    static bool               off(SubscriptionId id) noexcept;
    [[nodiscard]] static bool hasSubscription(SubscriptionId id) noexcept;
    [[nodiscard]] static auto listenerCount(std::string_view topic = {}) noexcept -> std::size_t;
    [[nodiscard]] static auto topics() -> std::vector<std::string>;

    [[nodiscard]] static auto dispatchPluginInvocation(EventInvocation& inv) -> Expected<bool>;
    [[nodiscard]] static auto dispatchKernelInvocation(EventInvocation& inv, KernelPublishToken) -> Expected<bool>;

    [[nodiscard]] static auto clearPlugin(std::string_view pluginName) -> Expected<void>;
    [[nodiscard]] static auto waitPluginIdle(std::string_view pluginName) -> Expected<void>;

private:
    template <Detail::IsEventTopic Topic, class Callback>
    [[nodiscard]] static auto
    subscribeTyped(Callback&& callback, EventPriority priority, bool once, std::string_view ownerOverride)
            -> Expected<ScopedSubscription> {
        using Traits = Reflect::CallableTraits<std::decay_t<Callback>>;
        static_assert(Traits::valid, "EventBus callback must have an unambiguous call signature");
        using Split = Detail::SplitCallbackParams<typename Traits::ArgsTuple>;
        if constexpr (std::is_void_v<typename Topic::Payload>) {
            static_assert(Split::kPayloadArity == 0, "Void EventTopic callbacks do not take a payload parameter");
        } else {
            static_assert(Split::kPayloadArity <= 1, "EventTopic callbacks accept at most 1 payload parameter");
        }
        if constexpr (!Topic::isMutable) {
            static_assert(!Detail::hasAnyMutablePayloadRef<typename Split::PayloadTuple>(),
                          "This EventTopic is immutable; take `const Payload&` or `Payload` by value");
        }
        if constexpr (!Topic::isCancellable && Split::kHasContext) {
            static_assert(!std::same_as<std::remove_cvref_t<typename Split::ContextParam>, InterceptEventContext<true>>,
                          "This EventTopic is non-cancellable; take `const ReadOnlyEventContext&` instead");
        }
        if constexpr (!std::is_void_v<typename Topic::Payload> && Split::kPayloadArity == 1) {
            using P0 = std::remove_cvref_t<std::tuple_element_t<0, typename Split::PayloadTuple>>;
            static_assert(std::same_as<P0, typename Topic::Payload> || std::same_as<P0, Reflect::ObjectRef>,
                          "Callback payload type does not match Topic::Payload");
        }
        if (priority == EventPriority::Monitor) {
            if constexpr (Detail::hasAnyMutablePayloadRef<typename Split::PayloadTuple>() ||
                          (Split::kHasContext && std::same_as<std::remove_cvref_t<typename Split::ContextParam>,
                                                              InterceptEventContext<true>>) ) {
                return Reflect::bridgeUnexpected(Reflect::BridgeErrorCode::ReadonlyViolation,
                                                 "monitor listeners cannot mutate event arguments or cancel events");
            }
        }
        return subscribeImpl(Topic::topic,
                             std::forward<Callback>(callback),
                             priority,
                             once,
                             ownerOverride,
                             /*normalizeBare=*/
                             false);
    }

    template <class Callback>
    [[nodiscard]] static auto subscribeImpl(std::string_view       rawTopic,
                                            Callback&&             callback,
                                            EventPriority          priority,
                                            bool                   once,
                                            const std::string_view ownerOverride,
                                            const bool             normalizeBare) -> Expected<ScopedSubscription> {
        using Decayed = std::decay_t<Callback>;
        using Traits  = Reflect::CallableTraits<Decayed>;
        using Split   = Detail::SplitCallbackParams<typename Traits::ArgsTuple>;

        std::string owner = ownerOverride.empty() ? Reflect::currentOwnerPluginName() : std::string(ownerOverride);
        auto        topic = normalizeSubscribeTopic(rawTopic, owner, normalizeBare);
        if (!topic) {
            return unexpected(std::move(topic).error());
        }

        const auto id       = allocateSubscriptionId();
        auto       activity = Reflect::acquirePluginActivity(owner);
        auto       slot     = std::make_shared<
                Detail::CppCallbackSlot<Decayed, typename Split::PayloadTuple, typename Split::ContextParam>>(
                id, priority, once, std::move(owner), std::move(activity), std::forward<Callback>(callback));

        auto added = addSlot(*topic, std::move(slot));
        if (!added) {
            return unexpected(std::move(added).error());
        }
        return ScopedSubscription{*added};
    }

    template <Detail::IsEventTopic Topic, class PayloadRef>
    static auto emitKernelPayload(PayloadRef&& payload, KernelPublishToken token) -> Expected<bool> {
        std::array<NativeArg, 1> nativeArgs{Detail::makeNativeArg(std::forward<PayloadRef>(payload))};

        auto materialize = [&payload](Reflect::MarshalContext& ctx) -> Expected<std::vector<Reflect::BridgeValue>> {
            auto val = Reflect::toBridge(payload, ctx);
            if (!val) {
                return unexpected(std::move(val).error());
            }
            std::vector<Reflect::BridgeValue> out;
            out.push_back(std::move(*val));
            return out;
        };

        EventInvocation::WriteBackFn writeBack;
        if constexpr (Topic::isMutable) {
            writeBack = [&payload](std::span<const Reflect::BridgeValue> updated,
                                   Reflect::MarshalContext&              ctx) -> Expected<void> {
                if (updated.empty()) {
                    return {};
                }
                auto converted = Reflect::fromBridge<typename Topic::Payload>(updated[0], ctx);
                if (!converted) {
                    return unexpected(std::move(converted).error());
                }
                payload = std::move(*converted);
                return {};
            };
        }

        EventInvocation inv{Topic::topic,
                            "CloverCore",
                            Topic::isCancellable,
                            Topic::isMutable,
                            nativeArgs,
                            std::move(materialize),
                            std::move(writeBack)};
        return dispatchKernelInvocation(inv, token);
    }

    template <class... Args>
    static auto
    emitArgsImpl(std::string_view rawTopic, bool cancellable, bool mutablePayload, bool normalizeBare, Args&&... args)
            -> Expected<bool> {
        const std::string owner = Reflect::currentOwnerPluginName();
        auto              topic = normalizeEmitTopic(rawTopic, owner, normalizeBare);
        if (!topic) {
            return unexpected(std::move(topic).error());
        }

        std::array<NativeArg, sizeof...(Args)> nativeArgs{Detail::makeNativeArg(std::forward<Args>(args))...};
        auto                                   argRefs = std::forward_as_tuple(std::forward<Args>(args)...);

        auto materialize = [&argRefs](Reflect::MarshalContext& ctx) -> Expected<std::vector<Reflect::BridgeValue>> {
            return std::apply([&ctx](const auto&... elems) { return Reflect::Detail::packBridgeArgs(ctx, elems...); },
                              argRefs);
        };

        EventInvocation::WriteBackFn writeBack;
        if constexpr (sizeof...(Args) > 0) {
            if (mutablePayload) {
                writeBack = [&argRefs](std::span<const Reflect::BridgeValue> updated,
                                       Reflect::MarshalContext&              ctx) -> Expected<void> {
                    std::optional<Error> err;
                    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                        (
                                [&] {
                                    if (err || Is >= updated.size()) {
                                        return;
                                    }
                                    using ElemRef = std::tuple_element_t<Is, decltype(argRefs)>;
                                    if constexpr (std::is_lvalue_reference_v<ElemRef> &&
                                                  !std::is_const_v<std::remove_reference_t<ElemRef>>) {
                                        using Clean    = std::remove_cvref_t<ElemRef>;
                                        auto converted = Reflect::fromBridge<Clean>(updated[Is], ctx);
                                        if (!converted) {
                                            err = Reflect::prependErrorPath(std::move(converted).error(),
                                                                            "arg[" + std::to_string(Is) + "]");
                                            return;
                                        }
                                        std::get<Is>(argRefs) = std::move(*converted);
                                    }
                                }(),
                                ...);
                    }(std::make_index_sequence<sizeof...(Args)>{});
                    if (err) {
                        return unexpected(*err);
                    }
                    return {};
                };
            }
        }

        EventInvocation inv{
                *topic, owner, cancellable, mutablePayload, nativeArgs, std::move(materialize), std::move(writeBack)};
        return dispatchPluginInvocation(inv);
    }
};

} // namespace CloverNT::Event
