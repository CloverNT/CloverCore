#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

#include <CloverNT/API/Memory/Memory.hpp>

namespace CloverNT::Memory::Hook {

struct Options {
    std::weak_ptr<Plugin::Plugin> owner;
    // Honored only by inline hooks; import hooks ignore it.
    int priority{};
};

class CloverNT_API Handle {
public:
    Handle() = default;
    explicit Handle(ResourceId id) noexcept;
    ~Handle();

    Handle(Handle const&)            = delete;
    Handle& operator=(Handle const&) = delete;

    Handle(Handle&& other) noexcept;
    auto operator=(Handle&& other) noexcept -> Handle&;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] auto     id() const noexcept -> ResourceId;
    [[nodiscard]] bool     isEnabled() const;

    [[nodiscard]] auto enable() const -> Expected<void>;
    [[nodiscard]] auto disable() const -> Expected<void>;
    [[nodiscard]] auto remove() -> Expected<void>;

private:
    ResourceId mId{};
};

namespace Detail {

    template <typename T>
    struct FunctionTraits;

    template <typename R, typename... Args>
    struct FunctionTraits<R(Args...)> {
        using ReturnType                    = R;
        using ArgsTuple                     = std::tuple<Args...>;
        using Pointer                       = R (*)(Args...);
        static constexpr bool IsMember      = false;
        static constexpr auto ArgumentCount = sizeof...(Args);
    };

    template <typename R, typename... Args>
    struct FunctionTraits<R(Args...) noexcept> : FunctionTraits<R(Args...)> {};

    template <typename R, typename C, typename... Args>
    struct FunctionTraits<R (C::*)(Args...)> {
        using ReturnType                    = R;
        using ClassType                     = C;
        using ArgsTuple                     = std::tuple<Args...>;
        using Pointer                       = R (*)(C*, Args...);
        static constexpr bool IsMember      = true;
        static constexpr auto ArgumentCount = sizeof...(Args);
    };

    template <typename R, typename C, typename... Args>
    struct FunctionTraits<R (C::*)(Args...) const> {
        using ReturnType                    = R;
        using ClassType                     = C;
        using ArgsTuple                     = std::tuple<Args...>;
        using Pointer                       = R (*)(C const*, Args...);
        static constexpr bool IsMember      = true;
        static constexpr auto ArgumentCount = sizeof...(Args);
    };

    template <typename R, typename C, typename... Args>
    struct FunctionTraits<R (C::*)(Args...) noexcept> : FunctionTraits<R (C::*)(Args...)> {};

    template <typename R, typename C, typename... Args>
    struct FunctionTraits<R (C::*)(Args...) const noexcept> : FunctionTraits<R (C::*)(Args...) const> {};

    template <typename T, typename = void>
    inline constexpr bool HasFunctionTraits = false;

    template <typename T>
    inline constexpr bool HasFunctionTraits<T, std::void_t<typename FunctionTraits<T>::ReturnType>> = true;

    template <typename Sig>
    concept FunctionSignature = HasFunctionTraits<Sig> && !FunctionTraits<Sig>::IsMember;

    template <typename T>
    concept FreeFunctionPointer = std::is_pointer_v<T> && std::is_function_v<std::remove_pointer_t<T>>;

    template <typename T>
    concept MemberFunctionPointer = std::is_member_function_pointer_v<T>;

    template <typename Sig>
    class OriginalFunction;

    template <typename R, typename... Args>
    class OriginalFunction<R(Args...)> {
    public:
        using Pointer = R (*)(Args...);

        explicit OriginalFunction(void** slot) noexcept : mSlot(slot) {}

        auto operator()(Args... args) const -> R {
            auto* fn = reinterpret_cast<Pointer>(*mSlot);
            if (fn == nullptr) {
                if constexpr (std::is_void_v<R>) {
                    return;
                } else {
                    return R{};
                }
            }
            if constexpr (std::is_void_v<R>) {
                fn(std::forward<Args>(args)...);
                return;
            } else {
                return fn(std::forward<Args>(args)...);
            }
        }

        [[nodiscard]] auto get() const noexcept -> Pointer {
            return reinterpret_cast<Pointer>(*mSlot);
        }

    private:
        void** mSlot{};
    };

    template <typename MemFnPtr>
    struct MemberToFreeSig;

    template <typename R, typename C, typename... Args>
    struct MemberToFreeSig<R (C::*)(Args...)> {
        using Type = R(C*, Args...);
    };

    template <typename R, typename C, typename... Args>
    struct MemberToFreeSig<R (C::*)(Args...) const> {
        using Type = R(C const*, Args...);
    };

    template <typename R, typename C, typename... Args>
    struct MemberToFreeSig<R (C::*)(Args...) noexcept> {
        using Type = R(C*, Args...);
    };

    template <typename R, typename C, typename... Args>
    struct MemberToFreeSig<R (C::*)(Args...) const noexcept> {
        using Type = R(C const*, Args...);
    };

    template <typename FnPtr>
    using DeducedSignature = std::remove_pointer_t<typename FunctionTraits<std::remove_pointer_t<FnPtr>>::Pointer>;

    struct DetourProbe {
        struct AnyReturn {
            template <typename T>
            operator T() const noexcept;
        };

        template <typename... Args>
        auto operator()(Args&&...) const noexcept -> AnyReturn;
    };

    template <typename>
    struct DetourSigExtract;

    template <typename R, typename C, typename First, typename... Args>
    struct DetourSigExtract<R (C::*)(First, Args...) const> {
        using Type = R(Args...);
    };

    template <typename R, typename C, typename First, typename... Args>
    struct DetourSigExtract<R (C::*)(First, Args...)> {
        using Type = R(Args...);
    };

    template <typename R, typename C, typename First, typename... Args>
    struct DetourSigExtract<R (C::*)(First, Args...) const noexcept> {
        using Type = R(Args...);
    };

    template <typename R, typename C, typename First, typename... Args>
    struct DetourSigExtract<R (C::*)(First, Args...) noexcept> {
        using Type = R(Args...);
    };

    template <typename R, typename First, typename... Args>
    struct DetourSigExtract<R(First, Args...)> {
        using Type = R(Args...);
    };

    template <typename R, typename First, typename... Args>
    struct DetourSigExtract<R(First, Args...) noexcept> {
        using Type = R(Args...);
    };

    template <typename T>
    inline constexpr bool IsOriginalFunctionRef = false;

    template <typename Sig>
    inline constexpr bool IsOriginalFunctionRef<OriginalFunction<Sig>&> = true;

    template <typename T, typename = void>
    inline constexpr bool HasConcreteCallOp = false;

    template <typename T>
    inline constexpr bool HasConcreteCallOp<T, std::void_t<decltype(&T::operator())>> = true;

    template <typename Detour>
    consteval auto DeduceDetourSig() {
        using D = std::remove_cvref_t<Detour>;
        if constexpr (std::is_class_v<D>) {
            if constexpr (HasConcreteCallOp<D>) {
                return static_cast<DetourSigExtract<decltype(&D::operator())>::Type*>(nullptr);
            } else {
                return static_cast<DetourSigExtract<decltype(&D::template operator()<DetourProbe>)>::Type*>(nullptr);
            }
        } else if constexpr (std::is_pointer_v<D> && std::is_function_v<std::remove_pointer_t<D>>) {
            using FnSig  = std::remove_pointer_t<D>;
            using Traits = FunctionTraits<FnSig>;
            if constexpr (Traits::ArgumentCount > 0) {
                using FirstArg = std::tuple_element_t<0, typename Traits::ArgsTuple>;
                if constexpr (IsOriginalFunctionRef<FirstArg>) {
                    return static_cast<DetourSigExtract<FnSig>::Type*>(nullptr);
                } else {
                    return static_cast<void*>(nullptr);
                }
            } else {
                return static_cast<void*>(nullptr);
            }
        } else {
            return static_cast<void*>(nullptr);
        }
    }

    template <typename Detour>
    using DetourSignature = std::remove_pointer_t<decltype(DeduceDetourSig<Detour>())>;

    template <typename Detour>
    concept AutoDeducibleDetour =
            requires { typename DetourSignature<Detour>; } && FunctionSignature<DetourSignature<Detour>>;

    template <typename Detour, typename Sig, typename = void>
    inline constexpr bool IsDetourInvocable = false;

    template <typename Detour, typename R, typename... Args>
    inline constexpr bool
            IsDetourInvocable<Detour,
                              R(Args...),
                              std::void_t<decltype(std::declval<Detour>()(std::declval<OriginalFunction<R(Args...)>&>(),
                                                                          std::declval<Args>()...))>> =
                    std::is_invocable_r_v<R, Detour, OriginalFunction<R(Args...)>&, Args...>;

    template <typename Detour, typename Sig>
    concept DetourCallable = FunctionSignature<Sig> && IsDetourInvocable<Detour, Sig>;

    template <typename Sig, int MaxSlots = 64>
    class SlotPool;

    template <FunctionSignature Sig>
    class HookState;

    template <typename R, typename... Args>
    class HookState<R(Args...)> : public Memory::Detail::ResourcePayload {
    public:
        using Signature = R(Args...);
        using FnPtr     = R (*)(Args...);

        HookState() = default;
        ~HookState() override;

        HookState(HookState const&)            = delete;
        HookState& operator=(HookState const&) = delete;

        [[nodiscard]] auto original() const noexcept -> FnPtr {
            return reinterpret_cast<FnPtr>(mOriginal);
        }

        [[nodiscard]] auto rawFunction() const noexcept -> FnPtr {
            return mRawFunction;
        }

        [[nodiscard]] auto originalSlot() noexcept -> void** {
            return &mOriginal;
        }

        void setSlot(const int slotIndex, FnPtr rawFunction) noexcept {
            mSlotIndex   = slotIndex;
            mRawFunction = rawFunction;
        }

        void releaseSlot() noexcept {
            mSlotIndex = -1;
        }

        void quiesce() noexcept override;

        virtual auto invoke(OriginalFunction<R(Args...)>& original, Args... args) -> R = 0;

    private:
        void* mOriginal{};
        FnPtr mRawFunction{};
        int   mSlotIndex{-1};
    };

    template <typename Sig, typename Detour>
    class CallbackState;

    template <typename R, typename... Args, typename Detour>
    class CallbackState<R(Args...), Detour> final : public HookState<R(Args...)> {
    public:
        explicit CallbackState(Detour&& detour) : mDetour(std::forward<Detour>(detour)) {}

        auto invoke(OriginalFunction<R(Args...)>& original, Args... args) -> R override {
            if constexpr (std::is_void_v<R>) {
                std::invoke(mDetour, original, std::forward<Args>(args)...);
                return;
            } else {
                return std::invoke(mDetour, original, std::forward<Args>(args)...);
            }
        }

    private:
        std::remove_cvref_t<Detour> mDetour;
    };

    template <typename R, typename... Args, int MaxSlots>
    class SlotPool<R(Args...), MaxSlots> {
    public:
        using State = HookState<R(Args...)>;
        using FnPtr = R (*)(Args...);

        static auto instance() -> SlotPool& {
            static SlotPool pool;
            return pool;
        }

        [[nodiscard]] auto allocate(std::shared_ptr<State> const& state) -> std::optional<std::pair<int, FnPtr>> {
            std::lock_guard lock(mMutex);
            for (int i = 0; i < MaxSlots; ++i) {
                if (!mOccupied[i] && mInFlight[i].load(std::memory_order_acquire) == 0) {
                    mOccupied[i] = true;
                    mStates[i]   = state;
                    return std::pair{i, kTable[i]};
                }
            }
            return std::nullopt;
        }

        void release(int index) {
            if (index < 0 || index >= MaxSlots) {
                return;
            }

            std::lock_guard lock(mMutex);
            mStates[index].reset();
            mOccupied[index] = false;
        }

        void drain(int index) {
            if (index < 0 || index >= MaxSlots) {
                return;
            }
            while (mInFlight[index].load(std::memory_order_acquire) != 0) {
                std::this_thread::yield();
            }
        }

    private:
        SlotPool() = default;

        [[nodiscard]] auto state(int index) -> std::shared_ptr<State> {
            std::lock_guard lock(mMutex);
            return mStates[index].lock();
        }

        template <int I>
        static auto trampoline(Args... args) -> R {
            auto& pool = instance();
            pool.mInFlight[I].fetch_add(1, std::memory_order_acq_rel);
            struct InFlightGuard {
                std::atomic<int>* counter;
                ~InFlightGuard() {
                    counter->fetch_sub(1, std::memory_order_release);
                }
            } inFlightGuard{&pool.mInFlight[I]};

            auto state = pool.state(I);
            if (!state) {
                if constexpr (std::is_void_v<R>) {
                    return;
                } else {
                    return R{};
                }
            }

            OriginalFunction<R(Args...)> original(state->originalSlot());
            if constexpr (std::is_void_v<R>) {
                state->invoke(original, std::forward<Args>(args)...);
                return;
            } else {
                return state->invoke(original, std::forward<Args>(args)...);
            }
        }

        template <int... Is>
        static constexpr auto makeTable(std::integer_sequence<int, Is...>) -> std::array<FnPtr, MaxSlots> {
            return {&trampoline<Is>...};
        }

        static constexpr auto kTable = makeTable(std::make_integer_sequence<int, MaxSlots>{});

        std::mutex                                 mMutex;
        std::array<bool, MaxSlots>                 mOccupied{};
        std::array<std::atomic<int>, MaxSlots>     mInFlight{};
        std::array<std::weak_ptr<State>, MaxSlots> mStates{};
    };

    template <typename R, typename... Args>
    HookState<R(Args...)>::~HookState() {
        if (mSlotIndex >= 0) {
            SlotPool<R(Args...)>::instance().release(mSlotIndex);
            mSlotIndex = -1;
        }
    }

    template <typename R, typename... Args>
    void HookState<R(Args...)>::quiesce() noexcept {
        if (mSlotIndex >= 0) {
            SlotPool<R(Args...)>::instance().drain(mSlotIndex);
        }
    }

    template <typename MemFnPtr>
        requires MemberFunctionPointer<MemFnPtr>
    [[nodiscard]] auto memberFunctionAddress(MemFnPtr fn) -> Address {
        if constexpr (sizeof(MemFnPtr) == sizeof(void*)) {
            return std::bit_cast<Address>(fn);
        } else if constexpr (sizeof(MemFnPtr) == 2 * sizeof(void*)) {
            struct Repr {
                std::uintptr_t ptr;
                std::ptrdiff_t adj;
            };
            auto repr = std::bit_cast<Repr>(fn);
            return (repr.ptr & 1U) != 0 ? Address{} : static_cast<Address>(repr.ptr);
        } else {
            static_assert(sizeof(MemFnPtr) == 0, "Unsupported member function pointer layout");
            return {};
        }
    }

    template <FunctionSignature Sig, typename Detour>
        requires DetourCallable<Detour, Sig>
    [[nodiscard]] auto makeState(Detour&& detour) -> Expected<std::shared_ptr<HookState<Sig>>> {
        auto state      = std::make_shared<CallbackState<Sig, Detour>>(std::forward<Detour>(detour));
        auto allocation = SlotPool<Sig>::instance().allocate(state);
        if (!allocation) {
            return unexpected(makeMemoryError(ErrorCode::NoFreeSlots, "No free hook slots"));
        }

        auto [slotIndex, rawFunction] = *allocation;
        state->setSlot(slotIndex, rawFunction);
        return state;
    }

} // namespace Detail

template <Detail::FunctionSignature Sig>
using OriginalFunction = Detail::OriginalFunction<Sig>;

template <Detail::FunctionSignature Sig>
class InlineHandle {
public:
    using FnPtr = Detail::FunctionTraits<Sig>::Pointer;

    InlineHandle() = default;
    InlineHandle(Handle handle, std::weak_ptr<Detail::HookState<Sig>> state) noexcept
        : mHandle(std::move(handle)), mState(std::move(state)) {}

    InlineHandle(InlineHandle const&)                        = delete;
    InlineHandle& operator=(InlineHandle const&)             = delete;
    InlineHandle(InlineHandle&&) noexcept                    = default;
    auto operator=(InlineHandle&&) noexcept -> InlineHandle& = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(mHandle);
    }

    [[nodiscard]] auto id() const noexcept -> ResourceId {
        return mHandle.id();
    }

    [[nodiscard]] bool isEnabled() const {
        return mHandle.isEnabled();
    }

    [[nodiscard]] auto original() const noexcept -> FnPtr {
        if (auto state = mState.lock()) {
            return state->original();
        }
        return nullptr;
    }

    [[nodiscard]] auto enable() const -> Expected<void> {
        return mHandle.enable();
    }

    [[nodiscard]] auto disable() const -> Expected<void> {
        return mHandle.disable();
    }

    [[nodiscard]] auto remove() -> Expected<void> {
        return mHandle.remove();
    }

private:
    Handle                                mHandle;
    std::weak_ptr<Detail::HookState<Sig>> mState;
};

template <Detail::FunctionSignature Sig>
class ImportHandle {
public:
    ImportHandle() = default;
    explicit ImportHandle(Handle handle) noexcept : mHandle(std::move(handle)) {}

    ImportHandle(ImportHandle const&)                        = delete;
    ImportHandle& operator=(ImportHandle const&)             = delete;
    ImportHandle(ImportHandle&&) noexcept                    = default;
    auto operator=(ImportHandle&&) noexcept -> ImportHandle& = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(mHandle);
    }

    [[nodiscard]] auto id() const noexcept -> ResourceId {
        return mHandle.id();
    }

    [[nodiscard]] bool isEnabled() const {
        return mHandle.isEnabled();
    }

    [[nodiscard]] auto enable() const -> Expected<void> {
        return mHandle.enable();
    }

    [[nodiscard]] auto disable() const -> Expected<void> {
        return mHandle.disable();
    }

    [[nodiscard]] auto remove() -> Expected<void> {
        return mHandle.remove();
    }

private:
    Handle mHandle;
};

namespace Inline {

    template <Detail::FunctionSignature Sig, typename Detour>
        requires Detail::DetourCallable<Detour, Sig>
    [[nodiscard]] auto create(const Address target, Detour&& detour, const Options options = {})
            -> Expected<InlineHandle<Sig>> {
        if (target == 0) {
            return unexpected(makeMemoryError(ErrorCode::InvalidArgument, "Inline hook target is null"));
        }

        auto state = Detail::makeState<Sig>(std::forward<Detour>(detour));
        if (!state) {
            return unexpected(state.error());
        }

        auto payload = std::static_pointer_cast<Memory::Detail::ResourcePayload>(state.value());
        auto created = Memory::Detail::createInlineHook(target,
                                                        reinterpret_cast<Address>(state.value()->rawFunction()),
                                                        options.priority,
                                                        state.value()->originalSlot(),
                                                        options.owner,
                                                        std::move(payload));
        if (!created) {
            return unexpected(created.error());
        }

        return InlineHandle<Sig>(Handle(created.value()), state.value());
    }

    template <Detail::FreeFunctionPointer Fn, typename Detour>
    [[nodiscard]] auto create(Fn fn, Detour&& detour, Options options = {})
            -> Expected<InlineHandle<Detail::DeducedSignature<Fn>>> {
        using Sig = Detail::DeducedSignature<Fn>;
        return create<Sig>(reinterpret_cast<Address>(fn), std::forward<Detour>(detour), std::move(options));
    }

    template <typename Detour>
        requires Detail::AutoDeducibleDetour<Detour>
    [[nodiscard]] auto create(const Address target, Detour&& detour, Options options = {})
            -> Expected<InlineHandle<Detail::DetourSignature<Detour>>> {
        using Sig = Detail::DetourSignature<Detour>;
        return create<Sig>(target, std::forward<Detour>(detour), std::move(options));
    }

    template <auto MemFn, typename Detour>
        requires Detail::MemberFunctionPointer<decltype(MemFn)>
    [[nodiscard]] auto createMember(Detour&& detour, Options options = {})
            -> Expected<InlineHandle<typename Detail::MemberToFreeSig<decltype(MemFn)>::Type>> {
        using Sig   = Detail::MemberToFreeSig<decltype(MemFn)>::Type;
        auto target = Detail::memberFunctionAddress(MemFn);
        return create<Sig>(target, std::forward<Detour>(detour), std::move(options));
    }

    template <auto MemFn, typename Detour>
        requires Detail::MemberFunctionPointer<decltype(MemFn)>
    [[nodiscard]] auto createMember(const Address target, Detour&& detour, Options options = {})
            -> Expected<InlineHandle<typename Detail::MemberToFreeSig<decltype(MemFn)>::Type>> {
        using Sig = Detail::MemberToFreeSig<decltype(MemFn)>::Type;
        return create<Sig>(target, std::forward<Detour>(detour), std::move(options));
    }

} // namespace Inline

namespace Import {

    template <Detail::FunctionSignature Sig, typename Detour>
        requires Detail::DetourCallable<Detour, Sig>
    [[nodiscard]] auto create(const std::string_view moduleName,
                              const std::string_view importModule,
                              const std::string_view functionName,
                              Detour&&               detour,
                              const Options          options = {}) -> Expected<ImportHandle<Sig>> {
        if (importModule.empty() || functionName.empty()) {
            return unexpected(makeMemoryError(ErrorCode::InvalidArgument, "Import module and function are required"));
        }

        auto state = Detail::makeState<Sig>(std::forward<Detour>(detour));
        if (!state) {
            return unexpected(state.error());
        }

        auto payload = std::static_pointer_cast<Memory::Detail::ResourcePayload>(state.value());
        auto created = Memory::Detail::createImportHook(moduleName,
                                                        importModule,
                                                        functionName,
                                                        reinterpret_cast<Address>(state.value()->rawFunction()),
                                                        state.value()->originalSlot(),
                                                        options.owner,
                                                        std::move(payload));
        if (!created) {
            return unexpected(created.error());
        }

        return ImportHandle<Sig>(Handle(created.value()));
    }

} // namespace Import

} // namespace CloverNT::Memory::Hook
