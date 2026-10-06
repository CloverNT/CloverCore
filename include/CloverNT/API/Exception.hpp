#pragma once
#include <CloverNT/API/Expected.hpp>
#include <CloverNT/API/Logger.hpp>
#include <CloverNT/API/Macros.hpp>

#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace CloverNT {

class CloverNT_API StackTrace {
public:
    struct Frame {
        const void*   address{};
        std::string   function; // demangled symbol name, empty if unresolved
        std::string   module;   // owning module/DLL file name, empty if unknown
        std::string   file;     // source file name, empty if no line info
        std::uint32_t line{};   // source line, 0 if unknown
    };

    StackTrace() = default;

    [[nodiscard]] static auto capture(std::size_t skip = 0, std::size_t maxFrames = 32) noexcept -> StackTrace;
    [[nodiscard]] static auto fromAddresses(const void* const* addresses, std::size_t count) -> StackTrace;

    [[nodiscard]] bool empty() const noexcept {
        return mAddresses.empty();
    }
    [[nodiscard]] auto size() const noexcept -> std::size_t {
        return mAddresses.size();
    }
    [[nodiscard]] auto addresses() const noexcept -> const std::vector<const void*>& {
        return mAddresses;
    }

    [[nodiscard]] auto frames() const -> std::vector<Frame>;
    [[nodiscard]] auto toString(std::size_t maxFrames = 16) const -> std::string;

private:
    std::vector<const void*> mAddresses;
};

class CloverNT_API Exception : public std::exception {
public:
    explicit Exception(std::string   message,
                       ErrorCategory category = ErrorCategory::General,
                       std::int32_t  code     = static_cast<std::int32_t>(CommonErrorCode::OperationFailed));

    explicit Exception(Error error);

    [[nodiscard]] auto what() const noexcept -> const char* override;

    [[nodiscard]] auto error() const noexcept -> const Error& {
        return mError;
    }
    [[nodiscard]] auto category() const noexcept -> ErrorCategory {
        return mError.category;
    }
    [[nodiscard]] auto code() const noexcept -> std::int32_t {
        return mError.code;
    }
    [[nodiscard]] auto stackTrace() const noexcept -> const StackTrace& {
        return mStackTrace;
    }

    [[nodiscard]] auto describe(std::size_t maxFrames = 16) const -> std::string;

    void log(const Logger& logger, LogLevel level = LogLevel::Error, std::size_t maxFrames = 16) const;

protected:
    Exception(Error error, StackTrace trace);

private:
    Error      mError;
    StackTrace mStackTrace;
};

class CloverNT_API OperationFailedException final : public Exception {
public:
    explicit OperationFailedException(std::string_view detail);
};

class CloverNT_API InvalidArgumentException final : public Exception {
public:
    explicit InvalidArgumentException(std::string_view detail);
};

class CloverNT_API NotFoundException final : public Exception {
public:
    explicit NotFoundException(std::string_view detail);
};

class CloverNT_API InvalidStateException final : public Exception {
public:
    explicit InvalidStateException(std::string_view detail);
};

class CloverNT_API UnsupportedException final : public Exception {
public:
    explicit UnsupportedException(std::string_view detail);
};

enum class FaultKind : std::uint8_t {
    Unknown,
    AccessViolation,       // EXCEPTION_ACCESS_VIOLATION / SIGSEGV
    InPageError,           // EXCEPTION_IN_PAGE_ERROR     / SIGBUS
    IllegalInstruction,    // EXCEPTION_ILLEGAL_INSTRUCTION / SIGILL
    PrivilegedInstruction, // EXCEPTION_PRIV_INSTRUCTION  / SIGILL (privileged)
    IntegerDivideByZero,   // EXCEPTION_INT_DIVIDE_BY_ZERO / SIGFPE (FPE_INTDIV)
    IntegerOverflow,       // EXCEPTION_INT_OVERFLOW      / SIGFPE (FPE_INTOVF)
    FloatingPointError,    // EXCEPTION_FLT_*             / SIGFPE
    StackOverflow,         // EXCEPTION_STACK_OVERFLOW
    Breakpoint,            // EXCEPTION_BREAKPOINT        / SIGTRAP
};

struct FaultAccess {
    enum class Type : std::uint8_t { Read, Write, Execute, Unknown };
    Type           type{Type::Unknown};
    std::uintptr_t address{}; // the inaccessible address
};

[[nodiscard]] CloverNT_API auto faultKindName(FaultKind kind) noexcept -> std::string_view;

class CloverNT_API StructuredException final : public Exception {
public:
    StructuredException(FaultKind                         kind,
                        std::uint32_t                     nativeCode,
                        const void*                       faultingInstruction,
                        const std::optional<FaultAccess>& access,
                        StackTrace                        trace);

    [[nodiscard]] auto kind() const noexcept -> FaultKind {
        return mKind;
    }
    [[nodiscard]] auto nativeCode() const noexcept -> std::uint32_t {
        return mNativeCode;
    }
    [[nodiscard]] auto faultingInstruction() const noexcept -> const void* {
        return mFaultingInstruction;
    }

    [[nodiscard]] auto accessType() const noexcept -> std::optional<FaultAccess::Type>;
    [[nodiscard]] auto faultAddress() const noexcept -> std::optional<std::uintptr_t>;

private:
    FaultKind                  mKind{FaultKind::Unknown};
    std::uint32_t              mNativeCode{};
    const void*                mFaultingInstruction{};
    std::optional<FaultAccess> mAccess;
};

#ifdef CloverNT_SYSTEM_WINDOWS
[[nodiscard]] CloverNT_API auto sehCodeName(std::uint32_t code) noexcept -> std::string_view;
#endif

namespace detail {

    inline constexpr std::size_t kFaultMaxFrames = 32;

    struct FaultRecord {
        FaultKind         kind{FaultKind::Unknown};
        std::uint32_t     nativeCode{};
        const void*       address{};
        bool              hasAccess{};
        FaultAccess::Type accessType{FaultAccess::Type::Unknown};
        std::uintptr_t    accessAddress{};
        std::size_t       frameCount{};
        const void*       frames[kFaultMaxFrames]{};
    };

    [[noreturn]] CloverNT_API void throwStructured(const FaultRecord& record);

    [[nodiscard]] CloverNT_API bool vehTry(void (*invoke)(void*), void* ctx, FaultRecord& record);

#ifdef CloverNT_SYSTEM_WINDOWS
    [[nodiscard]] CloverNT_API bool sehTry(void (*invoke)(void*), void* ctx, FaultRecord& record);
#endif

    template <typename Fn>
    auto runGuarded(Fn&& fn, bool (*tryFn)(void (*)(void*), void*, FaultRecord&)) {
        using Result = std::invoke_result_t<Fn&>;
        FaultRecord record{};
        if constexpr (std::is_void_v<Result>) {
            struct Ctx {
                Fn&         fn;
                static auto run(void* self) -> void {
                    static_cast<Ctx*>(self)->fn();
                }
            } ctx{fn};
            if (!tryFn(&Ctx::run, &ctx, record)) {
                throwStructured(record);
            }
        } else {
            std::optional<Result> result;
            struct Ctx {
                Fn&                    fn;
                std::optional<Result>& out;
                static auto            run(void* self) -> void {
                    auto* ctx = static_cast<Ctx*>(self);
                    ctx->out.emplace(ctx->fn());
                }
            } ctx{fn, result};
            if (!tryFn(&Ctx::run, &ctx, record)) {
                throwStructured(record);
            }
            return std::move(*result);
        }
    }

} // namespace detail

template <typename Fn>
auto guardVeh(Fn&& fn) {
    return detail::runGuarded(std::forward<Fn>(fn), &detail::vehTry);
}

#ifdef CloverNT_SYSTEM_WINDOWS
template <typename Fn>
auto guardSeh(Fn&& fn) {
    return detail::runGuarded(std::forward<Fn>(fn), &detail::sehTry);
}
#endif

} // namespace CloverNT
