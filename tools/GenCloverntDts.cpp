#include <CloverNT/API/Events/BuiltInEvents.hpp>
#include <CloverNT/API/Reflect/Reflect.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/AlkaBindings.hpp>

#include <array>
#include <cctype>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <ios>
#include <map>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <typeindex>
#include <typeinfo>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include <Alka/Ts/Generator.hpp>

namespace {

using BuiltinTopics = CloverNT::Event::Events::All;

[[nodiscard]] auto stripModuleWrapper(const std::string& dts) -> std::string {
    const auto open = dts.find("declare module");
    if (open == std::string::npos) {
        return dts;
    }
    const auto braceOpen  = dts.find('{', open);
    const auto braceClose = dts.rfind('}');
    if (braceOpen == std::string::npos || braceClose == std::string::npos || braceClose <= braceOpen) {
        return dts;
    }
    std::string       out  = dts.substr(0, open);
    const std::string body = dts.substr(braceOpen + 1, braceClose - braceOpen - 1);
    for (std::size_t start = 0; start < body.size();) {
        const auto        nl    = body.find('\n', start);
        const std::string line  = body.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        std::size_t       strip = 0;
        while (strip < 4 && strip < line.size() && line[strip] == ' ') {
            ++strip;
        }
        out += line.substr(strip);
        out += '\n';
        if (nl == std::string::npos) {
            break;
        }
        start = nl + 1;
    }
    return out;
}

template <Alka::BoundModule Mod>
void collectBoundModuleSymbols(Alka::TsNameRegistry& reg) {
    const std::string path{Alka::modulePathOf<Mod>()};
    Alka::forEachMember<Mod>([&]<typename T0>(const T0&) {
        using Desc = std::remove_cvref_t<T0>;
        if constexpr (Desc::kind == Alka::MemberKind::ClassRef) {
            using T = Desc::Type;
            reg.add(typeid(T), path, std::string(Alka::boundNameOf<T>()));
        } else if constexpr (Desc::kind == Alka::MemberKind::EnumType || Desc::kind == Alka::MemberKind::ErrorType) {
            reg.add(typeid(typename Desc::Type), path, std::string(Desc::name));
        } else if constexpr (Desc::kind == Alka::MemberKind::Submodule) {
            collectBoundModuleSymbols<typename Desc::Module>(reg);
        }
    });
}

template <class T>
consteval bool isMutableBuiltinPayload() {
    using Clean = std::remove_cvref_t<T>;
    return []<std::size_t... Is>(std::index_sequence<Is...>) {
        return ((std::same_as<std::remove_cvref_t<typename std::tuple_element_t<Is, BuiltinTopics>::Payload>, Clean> &&
                 std::tuple_element_t<Is, BuiltinTopics>::isMutable) ||
                ... || false);
    }(std::make_index_sequence<std::tuple_size_v<BuiltinTopics>>{});
}

template <class T>
struct IsStdSpan : std::false_type {};

template <class U, std::size_t E>
struct IsStdSpan<std::span<U, E>> : std::true_type {
    using Element = std::remove_cv_t<U>;
};

template <class T>
struct IsByteSpan : std::false_type {};

template <std::size_t E>
struct IsByteSpan<std::span<std::uint8_t, E>> : std::true_type {};

template <std::size_t E>
struct IsByteSpan<std::span<const std::uint8_t, E>> : std::true_type {};

template <class T>
struct IsStdVector : std::false_type {};

template <class U, class A>
struct IsStdVector<std::vector<U, A>> : std::true_type {
    using Element = U;
};

template <class T>
struct IsStdArray : std::false_type {};

template <class U, std::size_t N>
struct IsStdArray<std::array<U, N>> : std::true_type {
    using Element = U;
};

template <class T>
struct IsStringKeyedMap : std::false_type {};

template <class V, class C, class A>
struct IsStringKeyedMap<std::map<std::string, V, C, A>> : std::true_type {
    using Value = V;
};

template <class V, class H, class E, class A>
struct IsStringKeyedMap<std::unordered_map<std::string, V, H, E, A>> : std::true_type {
    using Value = V;
};

template <class T>
struct IsStdPair : std::false_type {};

template <class A, class B>
struct IsStdPair<std::pair<A, B>> : std::true_type {
    using First  = A;
    using Second = B;
};

template <class T>
struct IsStdTuple : std::false_type {};

template <class... Ts>
struct IsStdTuple<std::tuple<Ts...>> : std::true_type {};

template <class T>
struct IsStdVariant : std::false_type {};

template <class... Ts>
struct IsStdVariant<std::variant<Ts...>> : std::true_type {};

template <class T>
struct IsBorrowed : std::false_type {};

template <class U>
struct IsBorrowed<CloverNT::Reflect::Borrowed<U>> : std::true_type {
    using Inner = std::remove_const_t<U>;
};

template <class T>
struct IsTypedCallable : std::false_type {};

template <class R, class... Args>
struct IsTypedCallable<CloverNT::Reflect::Callable<R(Args...)>> : std::true_type {
    using Return    = R;
    using ArgsTuple = std::tuple<Args...>;
};

template <class T>
[[nodiscard]] auto resolveEventTsType(const Alka::TsNameRegistry& reg) -> std::string;

template <class Tuple, std::size_t... Is>
[[nodiscard]] auto formatCallableParams(const Alka::TsNameRegistry& reg, std::index_sequence<Is...>) -> std::string {
    std::string out;
    (
            [&] {
                if (!out.empty()) {
                    out += ", ";
                }
                out += std::format("arg{}: {}", Is, resolveEventTsType<std::tuple_element_t<Is, Tuple>>(reg));
            }(),
            ...);
    return out;
}

template <class T>
[[nodiscard]] auto resolveEventTsType(const Alka::TsNameRegistry& reg) -> std::string {
    using Clean = std::remove_cvref_t<T>;
    if constexpr (std::is_void_v<Clean>) {
        return "void";
    } else if constexpr (std::same_as<Clean, std::vector<std::uint8_t>> || IsByteSpan<Clean>::value) {
        return "Uint8Array";
    } else if constexpr (std::same_as<Clean, CloverNT::Reflect::BridgeValue>) {
        return "unknown";
    } else if constexpr (std::same_as<Clean, CloverNT::Reflect::ObjectRef>) {
        return "Record<string, unknown>";
    } else if constexpr (std::same_as<Clean, CloverNT::Reflect::BridgeCallable>) {
        return "(...args: unknown[]) => unknown";
    } else if constexpr (IsTypedCallable<Clean>::value) {
        using ArgsTuple = IsTypedCallable<Clean>::ArgsTuple;
        const Alka::TsNameRegistry::FlipScope flip(reg);
        const std::string                     params =
                formatCallableParams<ArgsTuple>(reg, std::make_index_sequence<std::tuple_size_v<ArgsTuple>>{});
        return std::format("({}) => {}", params, resolveEventTsType<typename IsTypedCallable<Clean>::Return>(reg));
    } else if constexpr (CloverNT::Reflect::Detail::IsAsync<Clean>::value) {
        return std::format("Promise<{}>",
                           resolveEventTsType<typename CloverNT::Reflect::Detail::IsAsync<Clean>::ValueType>(reg));
    } else if constexpr (IsBorrowed<Clean>::value) {
        return resolveEventTsType<typename IsBorrowed<Clean>::Inner>(reg);
    } else if constexpr (CloverNT::Reflect::Detail::IsOptional<Clean>::value) {
        return resolveEventTsType<typename Clean::value_type>(reg) + " | undefined";
    } else if constexpr (IsStdVector<Clean>::value) {
        return Alka::Detail::parenthesizeCompound(resolveEventTsType<typename IsStdVector<Clean>::Element>(reg)) + "[]";
    } else if constexpr (IsStdSpan<Clean>::value) {
        return Alka::Detail::parenthesizeCompound(resolveEventTsType<typename IsStdSpan<Clean>::Element>(reg)) + "[]";
    } else if constexpr (IsStdArray<Clean>::value) {
        return Alka::Detail::parenthesizeCompound(resolveEventTsType<typename IsStdArray<Clean>::Element>(reg)) + "[]";
    } else if constexpr (IsStringKeyedMap<Clean>::value) {
        return std::format("Record<string, {}>", resolveEventTsType<typename IsStringKeyedMap<Clean>::Value>(reg));
    } else if constexpr (IsStdPair<Clean>::value) {
        return std::format("[{}, {}]",
                           resolveEventTsType<typename IsStdPair<Clean>::First>(reg),
                           resolveEventTsType<typename IsStdPair<Clean>::Second>(reg));
    } else if constexpr (IsStdTuple<Clean>::value) {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            std::string elems;
            (
                    [&] {
                        if (!elems.empty()) {
                            elems += ", ";
                        }
                        elems += resolveEventTsType<std::tuple_element_t<Is, Clean>>(reg);
                    }(),
                    ...);
            return "[" + elems + "]";
        }(std::make_index_sequence<std::tuple_size_v<Clean>>{});
    } else if constexpr (IsStdVariant<Clean>::value) {
        return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            std::string alts;
            (
                    [&] {
                        if (!alts.empty()) {
                            alts += " | ";
                        }
                        alts += resolveEventTsType<std::variant_alternative_t<Is, Clean>>(reg);
                    }(),
                    ...);
            return alts;
        }(std::make_index_sequence<std::variant_size_v<Clean>>{});
    } else if constexpr (Alka::TsNameable<Clean>) {
        if (reg.find(typeid(Clean)) != nullptr) {
            return reg.refer(typeid(Clean), CloverNT::Reflect::shortTypeNameOf<Clean>.view());
        }
        return Alka::TsTypeName<Clean>::get(reg).mText;
    } else if constexpr (CloverNT::Reflect::ReflectedAggregate<Clean> || CloverNT::Reflect::ReflectedClassType<Clean>) {
        return reg.refer(typeid(Clean), CloverNT::Reflect::shortTypeNameOf<Clean>.view());
    } else if constexpr (std::is_enum_v<Clean>) {
        if (reg.find(typeid(Clean)) != nullptr) {
            return reg.refer(typeid(Clean), CloverNT::Reflect::shortTypeNameOf<Clean>.view());
        }
        return "number";
    } else {
        static_assert(!sizeof(Clean),
                      "GenCloverntDts: unsupported C++ type in built-in EventTopic; ensure it is reflectable or bound");
        return "unknown";
    }
}

template <class T>
void ensureReflectedTypesEmitted(Alka::TsNameRegistry&                reg,
                                 std::unordered_set<std::type_index>& emitted,
                                 std::string&                         extraDecls) {
    using Clean = std::remove_cvref_t<T>;
    if constexpr (std::is_void_v<Clean> || std::same_as<Clean, std::vector<std::uint8_t>> || IsByteSpan<Clean>::value) {
        return;
    } else if constexpr (IsTypedCallable<Clean>::value) {
        ensureReflectedTypesEmitted<typename IsTypedCallable<Clean>::Return>(reg, emitted, extraDecls);
        using ArgsTuple = IsTypedCallable<Clean>::ArgsTuple;
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (ensureReflectedTypesEmitted<std::tuple_element_t<Is, ArgsTuple>>(reg, emitted, extraDecls), ...);
        }(std::make_index_sequence<std::tuple_size_v<ArgsTuple>>{});
    } else if constexpr (CloverNT::Reflect::Detail::IsAsync<Clean>::value) {
        ensureReflectedTypesEmitted<typename CloverNT::Reflect::Detail::IsAsync<Clean>::ValueType>(
                reg, emitted, extraDecls);
    } else if constexpr (IsBorrowed<Clean>::value) {
        ensureReflectedTypesEmitted<typename IsBorrowed<Clean>::Inner>(reg, emitted, extraDecls);
    } else if constexpr (CloverNT::Reflect::Detail::IsOptional<Clean>::value) {
        ensureReflectedTypesEmitted<typename Clean::value_type>(reg, emitted, extraDecls);
    } else if constexpr (IsStdVector<Clean>::value) {
        ensureReflectedTypesEmitted<typename IsStdVector<Clean>::Element>(reg, emitted, extraDecls);
    } else if constexpr (IsStdSpan<Clean>::value) {
        ensureReflectedTypesEmitted<typename IsStdSpan<Clean>::Element>(reg, emitted, extraDecls);
    } else if constexpr (IsStdArray<Clean>::value) {
        ensureReflectedTypesEmitted<typename IsStdArray<Clean>::Element>(reg, emitted, extraDecls);
    } else if constexpr (IsStringKeyedMap<Clean>::value) {
        ensureReflectedTypesEmitted<typename IsStringKeyedMap<Clean>::Value>(reg, emitted, extraDecls);
    } else if constexpr (IsStdPair<Clean>::value) {
        ensureReflectedTypesEmitted<typename IsStdPair<Clean>::First>(reg, emitted, extraDecls);
        ensureReflectedTypesEmitted<typename IsStdPair<Clean>::Second>(reg, emitted, extraDecls);
    } else if constexpr (IsStdTuple<Clean>::value) {
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (ensureReflectedTypesEmitted<std::tuple_element_t<Is, Clean>>(reg, emitted, extraDecls), ...);
        }(std::make_index_sequence<std::tuple_size_v<Clean>>{});
    } else if constexpr (IsStdVariant<Clean>::value) {
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (ensureReflectedTypesEmitted<std::variant_alternative_t<Is, Clean>>(reg, emitted, extraDecls), ...);
        }(std::make_index_sequence<std::variant_size_v<Clean>>{});
    } else if constexpr (CloverNT::Reflect::ReflectedAggregate<Clean>) {
        if (reg.find(typeid(Clean)) != nullptr || !emitted.insert(std::type_index(typeid(Clean))).second) {
            return;
        }
        const std::string shortName{CloverNT::Reflect::shortTypeNameOf<Clean>.view()};
        reg.add(typeid(Clean), reg.mCurrentModule, shortName);

        Clean                 dummy{};
        [[maybe_unused]] auto refs = CloverNT::Reflect::tieAggregate(dummy);
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (ensureReflectedTypesEmitted<std::remove_cvref_t<decltype(std::get<Is>(refs))>>(reg, emitted, extraDecls),
             ...);
        }(std::make_index_sequence<CloverNT::Reflect::aggregateArity<Clean>>{});

        constexpr bool    kMutable = isMutableBuiltinPayload<Clean>();
        const std::string prefix   = kMutable ? "    " : "    readonly ";
        std::string       decl     = std::format("export interface {} {{\n", shortName);
        [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (
                    [&] {
                        using FieldT              = std::remove_cvref_t<decltype(std::get<Is>(refs))>;
                        constexpr auto kFieldName = CloverNT::Reflect::aggregateFieldName<Clean, Is>;
                        if constexpr (CloverNT::Reflect::Detail::IsOptional<FieldT>::value) {
                            decl += std::format("{}{}?: {};\n",
                                                prefix,
                                                kFieldName,
                                                resolveEventTsType<typename FieldT::value_type>(reg));
                        } else {
                            decl += std::format("{}{}: {};\n", prefix, kFieldName, resolveEventTsType<FieldT>(reg));
                        }
                    }(),
                    ...);
        }(std::make_index_sequence<CloverNT::Reflect::aggregateArity<Clean>>{});
        decl += "}\n";
        extraDecls += decl;
    } else if constexpr (CloverNT::Reflect::ReflectedClassType<Clean>) {
        if (reg.find(typeid(Clean)) != nullptr || !emitted.insert(std::type_index(typeid(Clean))).second) {
            return;
        }
        const std::string shortName{CloverNT::Reflect::shortTypeNameOf<Clean>.view()};
        reg.add(typeid(Clean), reg.mCurrentModule, shortName);
        extraDecls += std::format("export class {} {{\n    [key: string]: unknown;\n}}\n", shortName);
    }
}

[[nodiscard]] auto inferPayloadParamName(const std::string_view shortTypeName) -> std::string {
    if (shortTypeName.ends_with("Packet")) {
        return "packet";
    }
    if (shortTypeName.ends_with("Info")) {
        return "info";
    }
    if (shortTypeName.empty() || !std::isalpha(static_cast<unsigned char>(shortTypeName.front()))) {
        return "payload";
    }
    std::string out{shortTypeName};
    for (std::size_t i = 0; i < out.size() && std::isupper(static_cast<unsigned char>(out[i])); ++i) {
        if (i > 0 && i + 1 < out.size() && std::islower(static_cast<unsigned char>(out[i + 1]))) {
            break;
        }
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    }
    for (const char ch: out) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '_')) {
            return "payload";
        }
    }
    return out;
}

template <class Topic>
[[nodiscard]] auto formatTopicSpecLine(const Alka::TsNameRegistry& reg) -> std::string {
    using Payload = Topic::Payload;
    std::string argsTuple;
    if constexpr (std::is_void_v<Payload>) {
        argsTuple = "[]";
    } else if constexpr (IsStdTuple<Payload>::value) {
        argsTuple = [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            std::string elems;
            (
                    [&] {
                        if (!elems.empty()) {
                            elems += ", ";
                        }
                        using Elem = std::tuple_element_t<Is, Payload>;
                        elems += std::format("arg{}: {}", Is, resolveEventTsType<Elem>(reg));
                    }(),
                    ...);
            return "[" + elems + "]";
        }(std::make_index_sequence<std::tuple_size_v<Payload>>{});
    } else {
        const std::string paramName = inferPayloadParamName(CloverNT::Reflect::shortTypeNameOf<Payload>.view());
        argsTuple                   = std::format("[{}: {}]", paramName, resolveEventTsType<Payload>(reg));
    }
    return std::format("    \"{}\": {{ args: {}; cancellable: {}; mutable: {} }};\n",
                       Topic::topic,
                       argsTuple,
                       Topic::isCancellable,
                       Topic::isMutable);
}

[[nodiscard]] auto generateBuiltinEventSpecsDts() -> std::string {
    using CloverntMod = CloverNT::Core::Modules::bridge::CloverntModule;

    Alka::TsNameRegistry reg;
    reg.mStyle         = Alka::TsStyle::EsModule;
    reg.mCurrentModule = std::string(Alka::modulePathOf<CloverntMod>());
    collectBoundModuleSymbols<CloverntMod>(reg);

    std::unordered_set<std::type_index> emitted;
    std::string                         extraDecls;
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        (ensureReflectedTypesEmitted<typename std::tuple_element_t<Is, BuiltinTopics>::Payload>(
                 reg, emitted, extraDecls),
         ...);
    }(std::make_index_sequence<std::tuple_size_v<BuiltinTopics>>{});

    std::string out;
    if (!extraDecls.empty()) {
        out += extraDecls;
    }
    out += "\n/**\n * Built-in event topic specifications.\n */\n";
    out += "export interface CloverBuiltinEventSpecs {\n";
    [&]<std::size_t... Is>(std::index_sequence<Is...>) {
        ((out += formatTopicSpecLine<std::tuple_element_t<Is, BuiltinTopics>>(reg)), ...);
    }(std::make_index_sequence<std::tuple_size_v<BuiltinTopics>>{});
    out += "}\n";
    out += "export type CloverBuiltinEventName = keyof CloverBuiltinEventSpecs;\n";
    return out;
}

} // namespace

int main(const int argc, char** argv) {
    if (argc < 2) {
        std::println("usage: {} <output.d.ts>", argv[0]);
        return 2;
    }
    const Alka::TsGenOptions options{.mStyle = Alka::TsStyle::EsModule};
    std::string dts = stripModuleWrapper(Alka::generateDts<CloverNT::Core::Modules::bridge::CloverntModule>(options));
    dts += generateBuiltinEventSpecsDts();

    std::ofstream out(argv[1], std::ios::binary);
    if (!out) {
        std::println("GenCloverntDts: cannot open '{}' for writing\n", argv[1]);
        return 1;
    }
    out << dts;
    return out ? 0 : 1;
}
