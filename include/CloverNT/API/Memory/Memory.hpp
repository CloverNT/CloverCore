#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <CloverNT/API/Expected.hpp>
#include <CloverNT/API/Macros.hpp>
#include <CloverNT/API/Plugin/Plugin.hpp>

namespace CloverNT::Memory {

using Address    = std::uintptr_t;
using ResourceId = std::uint64_t;

enum class ErrorCode : std::uint8_t {
    Unknown = 0,
    RuntimeInactive,
    PluginUnavailable,
    InvalidArgument,
    InvalidHandle,
    NoFreeSlots,
    ModuleNotFound,
    ImportNotFound,
    MemoryNotReadable,
    MemoryNotWritable,
    ProtectionFailed,
    HookInstallFailed,
    HookRemoveFailed,
};

[[nodiscard]] inline auto makeMemoryError(ErrorCode code, std::string message) -> Error {
    return makeError(ErrorCategory::Memory, std::move(message), static_cast<std::int32_t>(code));
}

enum class MemoryProtection : std::uint32_t {
    None          = 0,
    Read          = 1 << 0,
    Write         = 1 << 1,
    Execute       = 1 << 2,
    ReadWrite     = Read | Write,
    ReadExec      = Read | Execute,
    ReadWriteExec = Read | Write | Execute,
};

constexpr auto operator|(const MemoryProtection lhs, const MemoryProtection rhs) noexcept -> MemoryProtection {
    return static_cast<MemoryProtection>(std::to_underlying(lhs) | std::to_underlying(rhs));
}

constexpr auto operator&(const MemoryProtection lhs, const MemoryProtection rhs) noexcept -> MemoryProtection {
    return static_cast<MemoryProtection>(std::to_underlying(lhs) & std::to_underlying(rhs));
}

constexpr auto hasFlag(const MemoryProtection value, const MemoryProtection flag) noexcept -> bool {
    return (value & flag) == flag;
}

class CloverNT_API Module {
public:
    Module() = default;
    Module(std::string name, Address base, std::size_t size);

    [[nodiscard]] auto name() const noexcept -> std::string const&;
    [[nodiscard]] auto base() const noexcept -> Address;
    [[nodiscard]] auto size() const noexcept -> std::size_t;
    [[nodiscard]] bool contains(Address address) const noexcept;

    [[nodiscard]] auto findExport(std::string_view symbolName) const -> Expected<Address>;
    [[nodiscard]] auto enumerateExports() const -> std::vector<std::pair<std::string, Address>>;
    [[nodiscard]] auto findSection(std::string_view sectionName) const -> Expected<std::pair<Address, std::size_t>>;

private:
    std::string mName;
    Address     mBase{};
    std::size_t mSize{};
};

struct SignatureElement {
    std::byte value{};
    std::byte mask{};

    [[nodiscard]] static constexpr auto byte(const std::byte value) noexcept -> SignatureElement {
        return {value, std::byte{0xFF}};
    }

    [[nodiscard]] static constexpr auto wildcard() noexcept -> SignatureElement {
        return {};
    }

    [[nodiscard]] constexpr bool isWildcard() const noexcept {
        return mask == std::byte{0};
    }

    [[nodiscard]] constexpr bool isConcrete() const noexcept {
        return mask == std::byte{0xFF};
    }

    [[nodiscard]] constexpr bool matches(const std::byte valueToCheck) const noexcept {
        return (valueToCheck & mask) == value;
    }
};

class CloverNT_API Signature {
public:
    Signature() = default;
    explicit Signature(std::vector<SignatureElement> elements);

    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] auto size() const noexcept -> std::size_t;
    [[nodiscard]] auto elements() const noexcept -> std::span<SignatureElement const>;
    [[nodiscard]] auto toString() const -> std::string;

private:
    std::vector<SignatureElement> mElements;
};

using SignatureView = std::span<SignatureElement const>;

namespace Detail {

    template <std::size_t N>
    struct SignatureString {
        char value[N + 1]{};

        // NOLINTNEXTLINE(google-explicit-constructor)
        consteval SignatureString(const char (&str)[N + 1]) {
            std::copy_n(str, N + 1, value);
        }

        [[nodiscard]] constexpr auto view() const noexcept -> std::string_view {
            return {value, N};
        }
    };
    template <std::size_t N>
    SignatureString(const char (&)[N]) -> SignatureString<N - 1>;

    consteval auto signatureHexDigit(const char c) noexcept -> std::uint8_t {
        if (c >= '0' && c <= '9') {
            return static_cast<std::uint8_t>(c - '0');
        }
        if (c >= 'A' && c <= 'F') {
            return static_cast<std::uint8_t>(c - 'A' + 10);
        }
        if (c >= 'a' && c <= 'f') {
            return static_cast<std::uint8_t>(c - 'a' + 10);
        }
        return 0xFF; // invalid marker
    }

    consteval auto signatureTokenCount(const std::string_view pattern) noexcept -> std::size_t {
        std::size_t count   = 0;
        bool        inToken = false;
        for (const char c: pattern) {
            if (c == ' ') {
                inToken = false;
            } else if (!inToken) {
                inToken = true;
                ++count;
            }
        }
        return count;
    }

    template <std::size_t MaxN>
    consteval auto parseSignatureTokens(const std::string_view pattern)
            -> std::pair<std::array<SignatureElement, MaxN>, std::size_t> {
        std::array<SignatureElement, MaxN> result{};
        std::size_t                        count = 0;

        std::size_t i = 0;
        while (i < pattern.size()) {
            if (pattern[i] == ' ') {
                ++i;
                continue;
            }
            const std::size_t start = i;
            while (i < pattern.size() && pattern[i] != ' ') {
                ++i;
            }
            const std::size_t len = i - start;

            if (len == 1) {
                if (pattern[start] != '?') {
                    throw "signature: single-character token must be '?'";
                }
                result[count++] = SignatureElement::wildcard();
            } else if (len == 2) {
                const char hi = pattern[start];
                const char lo = pattern[start + 1];
                if (hi == '?' && lo == '?') {
                    result[count++] = SignatureElement::wildcard();
                } else if (hi == '?') {
                    const auto loVal = signatureHexDigit(lo);
                    if (loVal == 0xFF) {
                        throw "signature: invalid hex digit";
                    }
                    result[count++] = {std::byte{loVal}, std::byte{0x0F}};
                } else if (lo == '?') {
                    const auto hiVal = signatureHexDigit(hi);
                    if (hiVal == 0xFF) {
                        throw "signature: invalid hex digit";
                    }
                    result[count++] = {static_cast<std::byte>(hiVal << 4), std::byte{0xF0}};
                } else {
                    const auto hiVal = signatureHexDigit(hi);
                    const auto loVal = signatureHexDigit(lo);
                    if (hiVal == 0xFF || loVal == 0xFF) {
                        throw "signature: invalid hex digit";
                    }
                    result[count++] = SignatureElement::byte(static_cast<std::byte>((hiVal << 4) | loVal));
                }
            } else {
                throw "signature: token must be 1 or 2 characters";
            }
        }

        if (count == 0) {
            throw "signature: empty pattern";
        }
        return {result, count};
    }

} // namespace Detail

template <Detail::SignatureString Pattern>
[[nodiscard]] consteval auto compileSignature() {
    constexpr auto maxN   = Detail::signatureTokenCount(Pattern.view());
    constexpr auto parsed = Detail::parseSignatureTokens<maxN>(Pattern.view());

    std::array<SignatureElement, parsed.second> out{};
    for (std::size_t i = 0; i < parsed.second; ++i) {
        out[i] = parsed.first[i];
    }
    return out;
}

inline namespace literals {

    /// @brief `"48 8B ? CC"_sig` → std::array<SignatureElement, N>.
    template <Detail::SignatureString Pattern>
    [[nodiscard]] consteval auto operator""_sig() {
        return compileSignature<Pattern>();
    }

    /// @brief `"48 8B ? CC"_sigv` → SignatureView backed by static storage.
    template <Detail::SignatureString Pattern>
    [[nodiscard]] constexpr auto operator""_sigv() noexcept -> SignatureView {
        static constexpr auto storage = compileSignature<Pattern>();
        return SignatureView{storage};
    }

} // namespace literals

enum class ScanAlignment : std::uint8_t {
    X1  = 1,
    X16 = 16,
};

enum class ScanHint : std::uint64_t {
    None   = 0,
    X86_64 = 1 << 0,
    Pair0  = 1 << 1,
};

constexpr auto operator|(const ScanHint lhs, const ScanHint rhs) noexcept -> ScanHint {
    return static_cast<ScanHint>(std::to_underlying(lhs) | std::to_underlying(rhs));
}

constexpr auto operator&(const ScanHint lhs, const ScanHint rhs) noexcept -> ScanHint {
    return static_cast<ScanHint>(std::to_underlying(lhs) & std::to_underlying(rhs));
}

struct ScanOptions {
    ScanAlignment alignment{ScanAlignment::X1};
    ScanHint      hints{ScanHint::None};
};

[[nodiscard]] CloverNT_API auto findModule(std::string_view moduleName = {}) -> Expected<Module>;
[[nodiscard]] CloverNT_API auto enumerateModules() -> std::vector<Module>;

[[nodiscard]] CloverNT_API auto readMemory(void* dest, Address source, std::size_t size) -> Expected<void>;
[[nodiscard]] CloverNT_API auto writeMemory(Address dest, void const* source, std::size_t size) -> Expected<void>;
[[nodiscard]] CloverNT_API auto setProtection(Address address, std::size_t size, MemoryProtection protection)
        -> Expected<MemoryProtection>;
[[nodiscard]] CloverNT_API auto setProtectionRaw(Address address, std::size_t size, MemoryProtection protection)
        -> Expected<void>;
[[nodiscard]] CloverNT_API auto queryProtection(Address address) -> Expected<MemoryProtection>;
[[nodiscard]] CloverNT_API bool isReadable(Address address, std::size_t size = 1);
[[nodiscard]] CloverNT_API bool isWritable(Address address, std::size_t size = 1);

class ScanResult {
public:
    constexpr ScanResult() noexcept = default;
    explicit constexpr ScanResult(const Address address) noexcept : mAddress(address) {}

    [[nodiscard]] constexpr bool hasResult() const noexcept {
        return mAddress != 0;
    }

    [[nodiscard]] explicit constexpr operator bool() const noexcept {
        return hasResult();
    }

    [[nodiscard]] constexpr auto get() const noexcept -> Address {
        return mAddress;
    }

    /// Reads an integral value at @p offset from the scan hit. Returns an error
    /// instead of dereferencing a null/unreadable address when the scan missed.
    template <std::integral Int>
    [[nodiscard]] auto read(const std::size_t offset = 0) const -> Expected<Int> {
        if (!hasResult()) {
            return unexpected(makeMemoryError(ErrorCode::InvalidHandle, "Scan produced no result"));
        }
        Int value{};
        if (auto result = readMemory(&value, mAddress + offset, sizeof(Int)); !result) {
            return unexpected(result.error());
        }
        return value;
    }

    /// Resolves a 32-bit relative displacement (e.g. a RIP-relative operand) at
    /// @p offset into an absolute address, validating the read first.
    [[nodiscard]] auto rel(const std::size_t offset, const std::size_t remaining = 0) const -> Expected<Address> {
        auto displacement = read<std::int32_t>(offset);
        if (!displacement) {
            return unexpected(displacement.error());
        }
        return mAddress + offset + sizeof(std::int32_t) + remaining + static_cast<Address>(*displacement);
    }

private:
    Address mAddress{};
};

struct ResourceOptions {
    std::weak_ptr<Plugin::Plugin> owner;
};

class CloverNT_API PatchHandle {
public:
    PatchHandle() = default;
    explicit PatchHandle(ResourceId id) noexcept;
    ~PatchHandle();

    PatchHandle(PatchHandle const&)            = delete;
    PatchHandle& operator=(PatchHandle const&) = delete;

    PatchHandle(PatchHandle&& other) noexcept;
    auto operator=(PatchHandle&& other) noexcept -> PatchHandle&;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] auto     id() const noexcept -> ResourceId;
    [[nodiscard]] bool     isApplied() const;

    [[nodiscard]] auto apply() const -> Expected<void>;
    [[nodiscard]] auto restore() const -> Expected<void>;
    [[nodiscard]] auto remove() -> Expected<void>;

private:
    ResourceId mId{};
};

template <class T>
    requires std::is_trivially_copyable_v<T>
[[nodiscard]] auto read(const Address address) -> Expected<T> {
    T value{};
    if (auto result = readMemory(&value, address, sizeof(T)); !result) {
        return unexpected(result.error());
    }
    return value;
}

template <class T>
    requires std::is_trivially_copyable_v<T>
[[nodiscard]] auto write(const Address address, T const& value) -> Expected<void> {
    return writeMemory(address, &value, sizeof(T));
}

[[nodiscard]] CloverNT_API auto readBytes(Address address, std::size_t size) -> Expected<std::vector<std::uint8_t>>;
[[nodiscard]] CloverNT_API auto writeBytes(Address address, std::span<std::uint8_t const> bytes) -> Expected<void>;

[[nodiscard]] CloverNT_API auto parseSignature(std::string_view pattern) -> Expected<Signature>;
[[nodiscard]] CloverNT_API auto
findFirst(std::string_view moduleName, std::string_view pattern, ScanOptions options = {}) -> ScanResult;
[[nodiscard]] CloverNT_API auto findFirst(Module const& module, std::string_view pattern, ScanOptions options = {})
        -> ScanResult;
[[nodiscard]] CloverNT_API auto findAll(std::string_view moduleName, std::string_view pattern, ScanOptions options = {})
        -> std::vector<ScanResult>;
[[nodiscard]] CloverNT_API auto findAll(Module const& module, std::string_view pattern, ScanOptions options = {})
        -> std::vector<ScanResult>;
[[nodiscard]] CloverNT_API auto
findFirst(std::string_view moduleName, Signature const& signature, ScanOptions options = {}) -> ScanResult;
[[nodiscard]] CloverNT_API auto findFirst(Module const& module, Signature const& signature, ScanOptions options = {})
        -> ScanResult;
[[nodiscard]] CloverNT_API auto
findAll(std::string_view moduleName, Signature const& signature, ScanOptions options = {}) -> std::vector<ScanResult>;
[[nodiscard]] CloverNT_API auto findAll(Module const& module, Signature const& signature, ScanOptions options = {})
        -> std::vector<ScanResult>;
[[nodiscard]] CloverNT_API auto findFirstInSection(std::string_view moduleName,
                                                   std::string_view sectionName,
                                                   Signature const& signature,
                                                   ScanOptions      options = {}) -> ScanResult;
[[nodiscard]] CloverNT_API auto findAllInSection(std::string_view moduleName,
                                                 std::string_view sectionName,
                                                 Signature const& signature,
                                                 ScanOptions      options = {}) -> std::vector<ScanResult>;

// Overloads taking a non-owning SignatureView (e.g. a compile-time `_sigv` /
// `_sig` pattern). These keep the readable pattern text out of the binary.
[[nodiscard]] CloverNT_API auto
findFirst(std::string_view moduleName, SignatureView signature, ScanOptions options = {}) -> ScanResult;
[[nodiscard]] CloverNT_API auto findFirst(Module const& module, SignatureView signature, ScanOptions options = {})
        -> ScanResult;
[[nodiscard]] CloverNT_API auto findAll(std::string_view moduleName, SignatureView signature, ScanOptions options = {})
        -> std::vector<ScanResult>;
[[nodiscard]] CloverNT_API auto findAll(Module const& module, SignatureView signature, ScanOptions options = {})
        -> std::vector<ScanResult>;
[[nodiscard]] CloverNT_API auto findFirstInSection(std::string_view moduleName,
                                                   std::string_view sectionName,
                                                   SignatureView    signature,
                                                   ScanOptions      options = {}) -> ScanResult;
[[nodiscard]] CloverNT_API auto findAllInSection(std::string_view moduleName,
                                                 std::string_view sectionName,
                                                 SignatureView    signature,
                                                 ScanOptions      options = {}) -> std::vector<ScanResult>;

[[nodiscard]] CloverNT_API auto createPatch(Address                   address,
                                            std::vector<std::uint8_t> bytes,
                                            const ResourceOptions&    options = {}) -> Expected<PatchHandle>;
[[nodiscard]] CloverNT_API auto createNopPatch(Address address, std::size_t size, const ResourceOptions& options = {})
        -> Expected<PatchHandle>;

namespace Detail {

    class CloverNT_API ResourcePayload {
    public:
        virtual ~ResourcePayload();

        virtual void quiesce() noexcept {}
    };

    [[nodiscard]] CloverNT_API auto createInlineHook(Address                              target,
                                                     Address                              detour,
                                                     int                                  priority,
                                                     void**                               originalSlot,
                                                     const std::weak_ptr<Plugin::Plugin>& owner,
                                                     std::shared_ptr<ResourcePayload> payload) -> Expected<ResourceId>;
    [[nodiscard]] CloverNT_API auto createImportHook(std::string_view                     moduleName,
                                                     std::string_view                     importModule,
                                                     std::string_view                     functionName,
                                                     Address                              detour,
                                                     void**                               originalSlot,
                                                     const std::weak_ptr<Plugin::Plugin>& owner,
                                                     std::shared_ptr<ResourcePayload> payload) -> Expected<ResourceId>;

    [[nodiscard]] CloverNT_API auto enableResource(ResourceId id) -> Expected<void>;
    [[nodiscard]] CloverNT_API auto disableResource(ResourceId id) -> Expected<void>;
    [[nodiscard]] CloverNT_API auto removeResource(ResourceId id) -> Expected<void>;
    [[nodiscard]] CloverNT_API bool isResourceEnabled(ResourceId id);

} // namespace Detail

} // namespace CloverNT::Memory
