#pragma once

#include <CloverNT/API/Expected.hpp>
#include <CloverNT/API/Reflect/BridgeValue.hpp>
#include <CloverNT/API/Reflect/TypeId.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace CloverNT::Reflect {

template <class T>
struct CallableTraits {
    static constexpr bool valid = false;
};

template <class R, class... Args>
struct CallableTraits<R(Args...)> {
    static constexpr bool valid          = true;
    using ReturnType                     = R;
    using ArgsTuple                      = std::tuple<Args...>;
    using Signature                      = R(Args...);
    static constexpr std::size_t arity   = sizeof...(Args);
    static constexpr bool        isConst = false;
};

template <class R, class... Args>
struct CallableTraits<R (*)(Args...)> : CallableTraits<R(Args...)> {};

template <class R, class... Args>
struct CallableTraits<R (*)(Args...) noexcept> : CallableTraits<R(Args...)> {};

template <class C, class R, class... Args>
struct CallableTraits<R (C::*)(Args...)> : CallableTraits<R(Args...)> {
    using ClassType               = C;
    static constexpr bool isConst = false;
};

template <class C, class R, class... Args>
struct CallableTraits<R (C::*)(Args...) noexcept> : CallableTraits<R(Args...)> {
    using ClassType               = C;
    static constexpr bool isConst = false;
};

template <class C, class R, class... Args>
struct CallableTraits<R (C::*)(Args...) const> : CallableTraits<R(Args...)> {
    using ClassType               = C;
    static constexpr bool isConst = true;
};

template <class C, class R, class... Args>
struct CallableTraits<R (C::*)(Args...) const noexcept> : CallableTraits<R(Args...)> {
    using ClassType               = C;
    static constexpr bool isConst = true;
};

template <class F>
    requires requires { &std::remove_cvref_t<F>::operator(); }
struct CallableTraits<F> : CallableTraits<decltype(&std::remove_cvref_t<F>::operator())> {};

template <class T>
concept MonomorphicCallable = requires { typename CallableTraits<std::remove_cvref_t<T>>::Signature; };

template <class Ptr>
struct FieldPointerTraits {
    static constexpr bool isFieldPointer = false;
};

template <class C, class M>
struct FieldPointerTraits<M C::*> {
    static constexpr bool isFieldPointer = true;
    using ClassType                      = C;
    using FieldType                      = M;
    static constexpr bool isConst        = std::is_const_v<M>;
};

template <class Ptr>
struct MemberFunctionPointerTraits {
    static constexpr bool isMemberFn = false;
};

template <class C, class R, class... Args>
struct MemberFunctionPointerTraits<R (C::*)(Args...)> : CallableTraits<R (C::*)(Args...)> {
    static constexpr bool isMemberFn = true;
};

template <class C, class R, class... Args>
struct MemberFunctionPointerTraits<R (C::*)(Args...) noexcept> : CallableTraits<R (C::*)(Args...) noexcept> {
    static constexpr bool isMemberFn = true;
};

template <class C, class R, class... Args>
struct MemberFunctionPointerTraits<R (C::*)(Args...) const> : CallableTraits<R (C::*)(Args...) const> {
    static constexpr bool isMemberFn = true;
};

template <class C, class R, class... Args>
struct MemberFunctionPointerTraits<R (C::*)(Args...) const noexcept>
    : CallableTraits<R (C::*)(Args...) const noexcept> {
    static constexpr bool isMemberFn = true;
};

struct ReadonlyTag {};
inline constexpr ReadonlyTag readonly{};

template <class Base>
struct ExtendsSpec {
    using BaseType = Base;
};

template <class Base>
[[nodiscard]] consteval auto extends() noexcept -> ExtendsSpec<Base> {
    return {};
}

enum class MemberDescKind : std::uint8_t {
    Field,
    Accessor,
    Method,
    Overload,
    Ctor,
};

template <FixedString Name, auto FieldPtr, bool Readonly>
struct FieldPropDesc {
    static constexpr auto             kind    = MemberDescKind::Field;
    static constexpr std::string_view name    = Name.view();
    static constexpr auto             pointer = FieldPtr;
    static constexpr bool isReadonly = Readonly || FieldPointerTraits<std::remove_cvref_t<decltype(FieldPtr)>>::isConst;
};

template <FixedString Name, auto GetterPtr, auto SetterPtr = nullptr>
struct AccessorPropDesc {
    static constexpr auto             kind   = MemberDescKind::Accessor;
    static constexpr std::string_view name   = Name.view();
    static constexpr auto             getter = GetterPtr;
    static constexpr auto             setter = SetterPtr;
    static constexpr bool isReadonly         = std::same_as<std::remove_cvref_t<decltype(SetterPtr)>, std::nullptr_t>;
};

template <FixedString Name, auto MethodPtr>
struct MethodDesc {
    static constexpr auto             kind    = MemberDescKind::Method;
    static constexpr std::string_view name    = Name.view();
    static constexpr auto             pointer = MethodPtr;
};

template <FixedString Name, auto... MethodPtrs>
    requires(sizeof...(MethodPtrs) >= 2)
struct OverloadDesc {
    static constexpr auto             kind = MemberDescKind::Overload;
    static constexpr std::string_view name = Name.view();
};

template <class... Args>
struct CtorDesc {
    static constexpr auto kind = MemberDescKind::Ctor;
    using ArgsTuple            = std::tuple<Args...>;
};

template <auto Ptr>
    requires(FieldPointerTraits<std::remove_cvref_t<decltype(Ptr)>>::isFieldPointer)
[[nodiscard]] consteval auto prop() noexcept {
    return FieldPropDesc<memberNameOf<Ptr>, Ptr, false>{};
}

template <auto Ptr>
    requires(FieldPointerTraits<std::remove_cvref_t<decltype(Ptr)>>::isFieldPointer)
[[nodiscard]] consteval auto prop(ReadonlyTag) noexcept {
    return FieldPropDesc<memberNameOf<Ptr>, Ptr, true>{};
}

template <FixedString Name, auto Ptr>
    requires(FieldPointerTraits<std::remove_cvref_t<decltype(Ptr)>>::isFieldPointer)
[[nodiscard]] consteval auto prop() noexcept {
    return FieldPropDesc<Name, Ptr, false>{};
}

template <FixedString Name, auto Ptr>
    requires(FieldPointerTraits<std::remove_cvref_t<decltype(Ptr)>>::isFieldPointer)
[[nodiscard]] consteval auto prop(ReadonlyTag) noexcept {
    return FieldPropDesc<Name, Ptr, true>{};
}

template <auto GetterPtr>
    requires(MemberFunctionPointerTraits<std::remove_cvref_t<decltype(GetterPtr)>>::isMemberFn)
[[nodiscard]] consteval auto prop() noexcept {
    return AccessorPropDesc<memberNameOf<GetterPtr>, GetterPtr>{};
}

template <FixedString Name, auto GetterPtr>
    requires(MemberFunctionPointerTraits<std::remove_cvref_t<decltype(GetterPtr)>>::isMemberFn)
[[nodiscard]] consteval auto prop() noexcept {
    return AccessorPropDesc<Name, GetterPtr>{};
}

template <FixedString Name, auto GetterPtr, auto SetterPtr>
    requires(MemberFunctionPointerTraits<std::remove_cvref_t<decltype(GetterPtr)>>::isMemberFn &&
             MemberFunctionPointerTraits<std::remove_cvref_t<decltype(SetterPtr)>>::isMemberFn)
[[nodiscard]] consteval auto prop() noexcept {
    return AccessorPropDesc<Name, GetterPtr, SetterPtr>{};
}

template <auto MethodPtr>
    requires(MemberFunctionPointerTraits<std::remove_cvref_t<decltype(MethodPtr)>>::isMemberFn)
[[nodiscard]] consteval auto method() noexcept {
    return MethodDesc<memberNameOf<MethodPtr>, MethodPtr>{};
}

template <FixedString Name, auto MethodPtr>
    requires(MemberFunctionPointerTraits<std::remove_cvref_t<decltype(MethodPtr)>>::isMemberFn)
[[nodiscard]] consteval auto method() noexcept {
    return MethodDesc<Name, MethodPtr>{};
}

template <FixedString Name, auto... MethodPtrs>
    requires(sizeof...(MethodPtrs) >= 2 &&
             (MemberFunctionPointerTraits<std::remove_cvref_t<decltype(MethodPtrs)>>::isMemberFn && ...))
[[nodiscard]] consteval auto overload() noexcept {
    return OverloadDesc<Name, MethodPtrs...>{};
}

template <class... Args>
[[nodiscard]] consteval auto ctor() noexcept {
    return CtorDesc<Args...>{};
}

template <class... Descs>
[[nodiscard]] consteval auto members(Descs... descs) noexcept {
    return std::tuple<Descs...>{descs...};
}

struct PropertyDescriptor {
    using GetterFn = auto (*)(const void* self, BorrowLease* activeLease) -> Expected<BridgeValue>;
    using SetterFn = auto (*)(void* self, const BridgeValue& value) -> Expected<void>;

    std::string_view  name;
    const TypeSchema* schema{};
    bool              readonly{false};
    GetterFn          get{};
    SetterFn          set{};
};

struct MethodDescriptor {
    using InvokeFn = auto (*)(void*                        self,
                              bool                         isConstReceiver,
                              std::span<const BridgeValue> args,
                              BorrowLease*                 activeLease) -> Expected<BridgeValue>;

    std::string_view      name;
    bool                  isConst{false};
    std::size_t           minArity{};
    std::size_t           maxArity{};
    const CallableSchema* signature{};
    InvokeFn              invoke{};
};

struct CtorDescriptor {
    using ConstructFn = auto (*)(std::span<const BridgeValue> args) -> Expected<std::shared_ptr<RevocableObjectCell>>;

    std::size_t                        minArity{};
    std::size_t                        maxArity{};
    std::span<const TypeSchema* const> paramTypes{};
    ConstructFn                        construct{};
};

struct BaseCastEntry {
    using UpcastFn = auto (*)(void* derivedPtr) noexcept -> void*;

    TypeId   baseTypeId{};
    UpcastFn upcast{};
};

struct ClassDescriptor {
    std::string_view                    name;
    TypeId                              typeId{};
    const ClassDescriptor*              base{};
    std::span<const BaseCastEntry>      baseChain{};
    std::span<const PropertyDescriptor> properties{};
    std::span<const MethodDescriptor>   methods{};
    std::span<const CtorDescriptor>     ctors{};

    [[nodiscard]] auto castTo(void* instancePtr, const TypeId targetType) const noexcept -> void* {
        if (instancePtr == nullptr) {
            return nullptr;
        }
        if (typeId == targetType) {
            return instancePtr;
        }
        for (const auto& [baseTypeId, upcast]: baseChain) {
            if (baseTypeId == targetType) {
                return upcast(instancePtr);
            }
        }
        return nullptr;
    }

    [[nodiscard]] auto findProperty(const std::string_view propName) const noexcept -> const PropertyDescriptor* {
        for (const auto& prop: properties) {
            if (prop.name == propName) {
                return &prop;
            }
        }
        return base != nullptr ? base->findProperty(propName) : nullptr;
    }

    [[nodiscard]] auto findMethod(const std::string_view methodName) const noexcept -> const MethodDescriptor* {
        for (const auto& m: methods) {
            if (m.name == methodName) {
                return &m;
            }
        }
        return base != nullptr ? base->findMethod(methodName) : nullptr;
    }
};

struct InterfaceMemberRequirement {
    std::string_view name;
    bool             isMethod{false};
    bool             readonly{false};
};

struct InterfaceDescriptor {
    std::string_view                            name;
    TypeId                                      typeId{};
    std::span<const InterfaceMemberRequirement> requirements{};
};

} // namespace CloverNT::Reflect
