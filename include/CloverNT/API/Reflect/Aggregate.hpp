#pragma once

#include <CloverNT/API/Reflect/TypeId.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace CloverNT::Reflect {

template <class T>
struct Class {
    static constexpr bool isReflected = false;
};

template <class T>
struct Interface {
    static constexpr bool isReflected = false;
};

template <class T>
struct DisableAggregate : std::false_type {};

namespace Detail {

    template <class T>
    struct FakeAggregateHolder {
        T value;
    };

#if defined(__clang__)
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wundefined-internal"
    #pragma clang diagnostic ignored "-Wundefined-var-template"
#endif
    template <class T>
    extern const FakeAggregateHolder<T> kFakeAggregate;
#if defined(__clang__)
    #pragma clang diagnostic pop
#endif

    template <class T, std::size_t>
    struct AnyField {
        template <class U>
            requires(!std::same_as<std::remove_cvref_t<U>, std::remove_cvref_t<T>> &&
                     !std::derived_from<std::remove_cvref_t<U>, std::remove_cvref_t<T>>)
        operator U() const noexcept;
    };

    struct AnyFieldUnconstrained {
        template <class U>
        operator U() const noexcept;
    };

    template <class T, std::size_t... Is>
    consteval bool canAggregateInitN(std::index_sequence<Is...>) noexcept {
        if constexpr (sizeof...(Is) == 0) {
            return requires { T{}; };
        } else if constexpr (sizeof...(Is) == 1) {
            return requires { T{AnyField<T, 0>{}}; };
        } else {
            return requires { T{(static_cast<void>(Is), AnyFieldUnconstrained{})...}; };
        }
    }

    inline constexpr std::size_t kMaxAggregateFields = 24;

    template <class T, std::size_t N = kMaxAggregateFields>
    [[nodiscard]] consteval auto detectAggregateArity() noexcept -> std::size_t {
        if constexpr (canAggregateInitN<T>(std::make_index_sequence<N>{})) {
            return N;
        } else if constexpr (N == 0) {
            return 0;
        } else {
            return detectAggregateArity<T, N - 1>();
        }
    }

    template <class T, std::size_t Arity>
    constexpr auto tieAggregateImpl(T& obj) noexcept {
        if constexpr (Arity == 0) {
            return std::tuple<>{};
        } else if constexpr (Arity == 1) {
            auto& [a0] = obj;
            return std::tie(a0);
        } else if constexpr (Arity == 2) {
            auto& [a0, a1] = obj;
            return std::tie(a0, a1);
        } else if constexpr (Arity == 3) {
            auto& [a0, a1, a2] = obj;
            return std::tie(a0, a1, a2);
        } else if constexpr (Arity == 4) {
            auto& [a0, a1, a2, a3] = obj;
            return std::tie(a0, a1, a2, a3);
        } else if constexpr (Arity == 5) {
            auto& [a0, a1, a2, a3, a4] = obj;
            return std::tie(a0, a1, a2, a3, a4);
        } else if constexpr (Arity == 6) {
            auto& [a0, a1, a2, a3, a4, a5] = obj;
            return std::tie(a0, a1, a2, a3, a4, a5);
        } else if constexpr (Arity == 7) {
            auto& [a0, a1, a2, a3, a4, a5, a6] = obj;
            return std::tie(a0, a1, a2, a3, a4, a5, a6);
        } else if constexpr (Arity == 8) {
            auto& [a0, a1, a2, a3, a4, a5, a6, a7] = obj;
            return std::tie(a0, a1, a2, a3, a4, a5, a6, a7);
        } else if constexpr (Arity == 9) {
            auto& [a0, a1, a2, a3, a4, a5, a6, a7, a8] = obj;
            return std::tie(a0, a1, a2, a3, a4, a5, a6, a7, a8);
        } else if constexpr (Arity == 10) {
            auto& [a0, a1, a2, a3, a4, a5, a6, a7, a8, a9] = obj;
            return std::tie(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9);
        } else if constexpr (Arity == 11) {
            auto& [a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10] = obj;
            return std::tie(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);
        } else if constexpr (Arity == 12) {
            auto& [a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11] = obj;
            return std::tie(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
        } else if constexpr (Arity == 13) {
            auto& [a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12] = obj;
            return std::tie(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12);
        } else if constexpr (Arity == 14) {
            auto& [a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13] = obj;
            return std::tie(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13);
        } else if constexpr (Arity == 15) {
            auto& [a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14] = obj;
            return std::tie(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14);
        } else if constexpr (Arity == 16) {
            auto& [a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15] = obj;
            return std::tie(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15);
        } else if constexpr (Arity == 17) {
            auto& [a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16] = obj;
            return std::tie(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16);
        } else if constexpr (Arity == 18) {
            auto& [a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17] = obj;
            return std::tie(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17);
        } else if constexpr (Arity == 19) {
            auto& [a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18] = obj;
            return std::tie(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18);
        } else if constexpr (Arity == 20) {
            auto& [a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19] = obj;
            return std::tie(a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19);
        } else if constexpr (Arity == 21) {
            auto& [a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20] = obj;
            return std::tie(
                    a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20);
        } else if constexpr (Arity == 22) {
            auto& [a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21] =
                    obj;
            return std::tie(
                    a0, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21);
        } else if constexpr (Arity == 23) {
            auto& [a0,
                   a1,
                   a2,
                   a3,
                   a4,
                   a5,
                   a6,
                   a7,
                   a8,
                   a9,
                   a10,
                   a11,
                   a12,
                   a13,
                   a14,
                   a15,
                   a16,
                   a17,
                   a18,
                   a19,
                   a20,
                   a21,
                   a22] = obj;
            return std::tie(a0,
                            a1,
                            a2,
                            a3,
                            a4,
                            a5,
                            a6,
                            a7,
                            a8,
                            a9,
                            a10,
                            a11,
                            a12,
                            a13,
                            a14,
                            a15,
                            a16,
                            a17,
                            a18,
                            a19,
                            a20,
                            a21,
                            a22);
        } else if constexpr (Arity == 24) {
            auto& [a0,
                   a1,
                   a2,
                   a3,
                   a4,
                   a5,
                   a6,
                   a7,
                   a8,
                   a9,
                   a10,
                   a11,
                   a12,
                   a13,
                   a14,
                   a15,
                   a16,
                   a17,
                   a18,
                   a19,
                   a20,
                   a21,
                   a22,
                   a23] = obj;
            return std::tie(a0,
                            a1,
                            a2,
                            a3,
                            a4,
                            a5,
                            a6,
                            a7,
                            a8,
                            a9,
                            a10,
                            a11,
                            a12,
                            a13,
                            a14,
                            a15,
                            a16,
                            a17,
                            a18,
                            a19,
                            a20,
                            a21,
                            a22,
                            a23);
        } else {
            static_assert(Arity <= kMaxAggregateFields, "CloverNT::Reflect: aggregate exceeds 24 fields");
        }
    }

    template <auto Ptr>
    [[nodiscard]] consteval auto extractSubobjectFieldName() noexcept -> std::string_view {
        std::string_view payload = valuePayload<Ptr>();
        while (!payload.empty() && !isIdentChar(payload.back())) {
            payload.remove_suffix(1);
        }
        std::size_t start = payload.size();
        while (start > 0 && isIdentChar(payload[start - 1])) {
            --start;
        }
        return payload.substr(start);
    }

#if defined(__clang__)
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wundefined-var-template"
#endif

    template <class T, std::size_t Arity, std::size_t I>
    inline constexpr auto aggregateFieldNameStorage = [] {
        constexpr std::string_view text =
                extractSubobjectFieldName<&std::get<I>(tieAggregateImpl<const T, Arity>(kFakeAggregate<T>.value))>();
        static_assert(!text.empty(), "CloverNT::Reflect: failed to reflect aggregate field name");
        return FixedString<text.size()>::from(text);
    }();

#if defined(__clang__)
    #pragma clang diagnostic pop
#endif

    template <class T>
    struct IsStdContainerOrTuple : std::false_type {};

    template <class T, std::size_t N>
    struct IsStdContainerOrTuple<std::array<T, N>> : std::true_type {};

    template <class A, class B>
    struct IsStdContainerOrTuple<std::pair<A, B>> : std::true_type {};

    template <class... Ts>
    struct IsStdContainerOrTuple<std::tuple<Ts...>> : std::true_type {};

} // namespace Detail

template <class T>
concept ReflectedClassType = requires {
    requires !std::is_void_v<std::remove_cvref_t<T>>;
    requires Class<std::remove_cvref_t<T>>::isReflected || requires { Class<std::remove_cvref_t<T>>::members; };
};

template <class T>
concept ReflectedInterfaceType = requires {
    requires !std::is_void_v<std::remove_cvref_t<T>>;
    requires Interface<std::remove_cvref_t<T>>::isReflected || requires { Interface<std::remove_cvref_t<T>>::members; };
};

template <class T>
concept ReflectedAggregate =
        std::is_class_v<std::remove_cvref_t<T>> && std::is_aggregate_v<std::remove_cvref_t<T>> &&
        std::default_initializable<std::remove_cvref_t<T>> && !std::is_polymorphic_v<std::remove_cvref_t<T>> &&
        !ReflectedClassType<T> && !ReflectedInterfaceType<T> && !DisableAggregate<std::remove_cvref_t<T>>::value &&
        !Detail::IsStdContainerOrTuple<std::remove_cvref_t<T>>::value && requires(std::remove_cvref_t<T>& v) {
            Detail::tieAggregateImpl<std::remove_cvref_t<T>, Detail::detectAggregateArity<std::remove_cvref_t<T>>()>(v);
        };

template <ReflectedAggregate T>
inline constexpr std::size_t aggregateArity = Detail::detectAggregateArity<std::remove_cvref_t<T>>();

template <ReflectedAggregate T>
[[nodiscard]] constexpr auto tieAggregate(T& obj) noexcept {
    return Detail::tieAggregateImpl<T, aggregateArity<T>>(obj);
}

template <ReflectedAggregate T, std::size_t I>
    requires(I < aggregateArity<T>)
inline constexpr std::string_view aggregateFieldName =
        Detail::aggregateFieldNameStorage<std::remove_cvref_t<T>, aggregateArity<T>, I>.view();

template <ReflectedAggregate T>
inline constexpr auto aggregateFieldNames = []<std::size_t... Is>(std::index_sequence<Is...>) {
    return std::array<std::string_view, sizeof...(Is)>{aggregateFieldName<T, Is>...};
}(std::make_index_sequence<aggregateArity<T>>{});

} // namespace CloverNT::Reflect
