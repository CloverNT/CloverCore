#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <type_traits>

namespace CloverNT::Reflect {

template <std::size_t N>
struct FixedString {
    std::array<char, N + 1> mChars{};

    consteval FixedString() = default;

    // ReSharper disable once CppNonExplicitConvertingConstructor
    consteval FixedString(const char (&str)[N + 1]) noexcept {
        for (std::size_t i = 0; i < N; ++i) {
            mChars[i] = str[i];
        }
        mChars[N] = '\0';
    }

    [[nodiscard]] static consteval auto from(const std::string_view sv) noexcept -> FixedString {
        FixedString out{};
        for (std::size_t i = 0; i < N && i < sv.size(); ++i) {
            out.mChars[i] = sv[i];
        }
        out.mChars[N] = '\0';
        return out;
    }

    [[nodiscard]] constexpr auto view() const noexcept -> std::string_view {
        return {mChars.data(), N};
    }

    [[nodiscard]] constexpr auto c_str() const noexcept -> const char* {
        return mChars.data();
    }

    [[nodiscard]] constexpr auto size() const noexcept -> std::size_t {
        return N;
    }

    // ReSharper disable once CppNonExplicitConversionOperator
    constexpr operator std::string_view() const noexcept {
        return view();
    }
};

template <std::size_t M>
FixedString(const char (&)[M]) -> FixedString<M - 1>;

[[nodiscard]] constexpr auto fnv1a64(const std::string_view text) noexcept -> std::uint64_t {
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char ch: text) {
        hash ^= static_cast<std::uint64_t>(ch);
        hash *= 1099511628211ull;
    }
    return hash == 0 ? 1ull : hash;
}

struct TypeId {
    std::uint64_t hash{};
    std::uint32_t size{};
    std::uint32_t alignment{};

    constexpr auto operator<=>(const TypeId&) const noexcept = default;

    [[nodiscard]] constexpr explicit operator bool() const noexcept {
        return hash != 0;
    }
};

namespace Detail {

    template <class T>
    [[nodiscard]] consteval auto rawTypeSig() noexcept -> std::string_view {
#if defined(__clang__) || defined(__GNUC__)
        return __PRETTY_FUNCTION__;
#elif defined(_MSC_VER)
        return __FUNCSIG__;
#else
    #error "CloverNT::Reflect: unsupported compiler"
#endif
    }

    template <auto V>
    [[nodiscard]] consteval auto rawValueSig() noexcept -> std::string_view {
#if defined(__clang__) || defined(__GNUC__)
        return __PRETTY_FUNCTION__;
#elif defined(_MSC_VER)
        return __FUNCSIG__;
#else
    #error "CloverNT::Reflect: unsupported compiler"
#endif
    }

    [[nodiscard]] consteval auto extractBetween(const std::string_view sig,
                                                const std::string_view openMarker,
                                                const std::string_view closeMarker) noexcept -> std::string_view {
        const std::size_t open = sig.find(openMarker);
        if (open == std::string_view::npos) {
            return {};
        }
        const auto        rest  = sig.substr(open + openMarker.size());
        const std::size_t close = rest.rfind(closeMarker);
        if (close == std::string_view::npos) {
            return {};
        }
        return rest.substr(0, close);
    }

    [[nodiscard]] consteval auto extractGccPayload(const std::string_view sig,
                                                   const std::string_view openMarker) noexcept -> std::string_view {
        const std::size_t open = sig.find(openMarker);
        if (open == std::string_view::npos) {
            return {};
        }
        const auto rest  = sig.substr(open + openMarker.size());
        int        depth = 0;
        for (std::size_t i = 0; i < rest.size(); ++i) {
            const char ch = rest[i];
            if (ch == '<' || ch == '(' || ch == '[' || ch == '{') {
                ++depth;
            } else if (ch == '>' || ch == ')' || ch == ']' || ch == '}') {
                if (depth == 0) {
                    return ch == ']' ? rest.substr(0, i) : std::string_view{};
                }
                --depth;
            } else if (ch == ';' && depth == 0) {
                return rest.substr(0, i);
            }
        }
        return {};
    }

    template <class T>
    [[nodiscard]] consteval auto typePayload() noexcept -> std::string_view {
#if defined(__clang__)
        return extractBetween(rawTypeSig<T>(), "[T = ", "]");
#elif defined(__GNUC__)
        return extractGccPayload(rawTypeSig<T>(), "[with T = ");
#else
        return extractBetween(rawTypeSig<T>(), "rawTypeSig<", ">(void)");
#endif
    }

    template <auto V>
    [[nodiscard]] consteval auto valuePayload() noexcept -> std::string_view {
#if defined(__clang__)
        return extractBetween(rawValueSig<V>(), "[V = ", "]");
#elif defined(__GNUC__)
        const auto autoForm = extractGccPayload(rawValueSig<V>(), "[with auto V = ");
        return !autoForm.empty() ? autoForm : extractGccPayload(rawValueSig<V>(), "[with V = ");
#else
        return extractBetween(rawValueSig<V>(), "rawValueSig<", ">(void)");
#endif
    }

    [[nodiscard]] consteval bool isIdentChar(const char ch) noexcept {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' ||
               ch == '$';
    }

    [[nodiscard]] consteval auto stripTagKeyword(std::string_view name) noexcept -> std::string_view {
        for (const std::string_view kw: {"class ", "struct ", "enum ", "union "}) {
            if (name.starts_with(kw)) {
                return name.substr(kw.size());
            }
        }
        return name;
    }

    [[nodiscard]] consteval auto dropNamespaceScope(const std::string_view qualified) noexcept -> std::string_view {
        int depth = 0;
        for (std::size_t i = qualified.size(); i > 1; --i) {
            const char ch = qualified[i - 1];
            if (ch == '>') {
                ++depth;
            } else if (ch == '<') {
                --depth;
            } else if (depth == 0 && ch == ':' && qualified[i - 2] == ':') {
                return qualified.substr(i);
            }
        }
        return qualified;
    }

    [[nodiscard]] consteval auto parseMemberValueName(std::string_view payload) noexcept -> std::string_view {
        while (!payload.empty() && (payload.back() == '}' || payload.back() == ')' || payload.back() == ' ')) {
            if (payload.back() == ')') {
                int         depth = 0;
                std::size_t open  = std::string_view::npos;
                for (std::size_t i = payload.size(); i > 0; --i) {
                    if (payload[i - 1] == ')') {
                        ++depth;
                    } else if (payload[i - 1] == '(' && --depth == 0) {
                        open = i - 1;
                        break;
                    }
                }
                if (open != std::string_view::npos && open > 0) {
                    payload = payload.substr(0, open);
                    continue;
                }
                break;
            }
            payload.remove_suffix(1);
        }
        std::size_t end = payload.size();
        while (end > 0 && !isIdentChar(payload[end - 1])) {
            --end;
        }
        std::size_t start = end;
        while (start > 0 && isIdentChar(payload[start - 1])) {
            --start;
        }
        return payload.substr(start, end - start);
    }

    template <class T>
    inline constexpr auto qualifiedTypeNameStorage = [] {
        constexpr std::string_view text = stripTagKeyword(typePayload<T>());
        static_assert(!text.empty(), "CloverNT::Reflect: failed to extract compile-time type name");
        return FixedString<text.size()>::from(text);
    }();

    template <class T>
    inline constexpr auto shortTypeNameStorage = [] {
        constexpr std::string_view text = dropNamespaceScope(qualifiedTypeNameStorage<T>.view());
        return FixedString<text.size()>::from(text);
    }();

    template <auto V>
    inline constexpr auto memberNameStorage = [] {
        constexpr std::string_view text = parseMemberValueName(valuePayload<V>());
        static_assert(!text.empty(),
                      "CloverNT::Reflect: failed to infer member name; pass an explicit name literal first");
        return FixedString<text.size()>::from(text);
    }();

} // namespace Detail

template <class T>
inline constexpr auto qualifiedTypeNameOf = Detail::qualifiedTypeNameStorage<std::remove_cvref_t<T>>;

template <class T>
inline constexpr auto shortTypeNameOf = Detail::shortTypeNameStorage<std::remove_cvref_t<T>>;

template <auto V>
inline constexpr auto memberNameOf = Detail::memberNameStorage<V>;

template <class T>
[[nodiscard]] consteval auto makeTypeId() noexcept -> TypeId {
    using Clean = std::remove_cvref_t<T>;
    if constexpr (std::is_void_v<Clean>) {
        return TypeId{.hash = fnv1a64("void"), .size = 0, .alignment = 0};
    } else {
        return TypeId{
                .hash      = fnv1a64(qualifiedTypeNameOf<Clean>.view()),
                .size      = static_cast<std::uint32_t>(sizeof(Clean)),
                .alignment = static_cast<std::uint32_t>(alignof(Clean)),
        };
    }
}

template <class T>
inline constexpr TypeId typeIdOf = makeTypeId<T>();

enum class SchemaKind : std::uint8_t {
    Void = 0,
    Null,
    Bool,
    Int64,
    UInt64,
    Float64,
    String,
    Bytes,
    Enum,
    Optional,
    Variant,
    Tuple,
    Array,
    Map,
    Aggregate,
    Class,
    BorrowedClass,
    RemoteObject,
    Callable,
    Async,
    Any,
};

struct TypeSchema;
struct ClassDescriptor;

struct FieldSchema {
    std::string_view  name;
    const TypeSchema* type{};
    bool              readonly{false};
    bool              optional{false};
};

struct CallableSchema {
    const TypeSchema*                  returnType{};
    std::span<const TypeSchema* const> paramTypes{};
    std::size_t                        minArgs{};
};

struct TypeSchema {
    SchemaKind                         kind{SchemaKind::Any};
    std::string_view                   name{};
    TypeId                             typeId{};
    const TypeSchema*                  elementType{};
    const TypeSchema*                  valueType{};
    std::span<const TypeSchema* const> itemTypes{};
    std::span<const FieldSchema>       fields{};
    const ClassDescriptor*             classDesc{};
    const CallableSchema*              callable{};
};

} // namespace CloverNT::Reflect
