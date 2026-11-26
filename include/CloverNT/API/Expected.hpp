#pragma once

#include <cstdint>
#include <exception>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace CloverNT {

enum class ErrorCategory : std::uint8_t {
    General,
    Argument,
    Filesystem,
    System,
    DynamicLibrary,
    Module,
    Plugin,
    Event,
    Memory,
    Logger,
    Json,
};

enum class CommonErrorCode : std::int32_t {
    Unknown = 0,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    InvalidState,
    OperationFailed,
    NotLoaded,
    AlreadyLoaded,
    Inactive,
    Unsupported,
    CallbackFailed,
    ParseFailed,
};

struct Error {
    ErrorCategory               category{ErrorCategory::General};
    std::int32_t                code{static_cast<std::int32_t>(CommonErrorCode::Unknown)};
    std::string                 message;
    std::optional<std::int64_t> nativeCode;
};

template <class T>
using Expected = std::expected<T, Error>;

[[nodiscard]] inline auto normalizeErrorMessage(std::string_view message) -> std::string {
    while (!message.empty() && (message.front() == ' ' || message.front() == '\t')) {
        message.remove_prefix(1);
    }
    if (message.empty()) {
        return {};
    }

    constexpr std::string_view kIpcPrefix       = "Error invoking remote method '";
    constexpr std::string_view kErrorPrefixes[] = {
            "Error:",
            "TypeError:",
            "RangeError:",
            "SyntaxError:",
            "ReferenceError:",
            "URIError:",
            "EvalError:",
            "AggregateError:",
    };

    std::string_view stripped = message;
    bool             changed  = true;
    while (changed) {
        changed = false;
        while (!stripped.empty() && (stripped.front() == ' ' || stripped.front() == '\t')) {
            stripped.remove_prefix(1);
        }
        if (stripped.starts_with(kIpcPrefix)) {
            if (const auto closePos = stripped.find("':", kIpcPrefix.size()); closePos != std::string_view::npos) {
                auto rest = stripped.substr(closePos + 2);
                while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
                    rest.remove_prefix(1);
                }
                if (!rest.empty()) {
                    stripped = rest;
                    changed  = true;
                    continue;
                }
            }
        }
        for (const auto prefix: kErrorPrefixes) {
            if (stripped.starts_with(prefix)) {
                auto rest = stripped.substr(prefix.size());
                while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) {
                    rest.remove_prefix(1);
                }
                if (!rest.empty()) {
                    stripped = rest;
                    changed  = true;
                    break;
                }
            }
        }
    }

    return std::string(stripped);
}

[[nodiscard]] inline auto makeError(const ErrorCategory category,
                                    const std::string&  message,
                                    const std::int32_t  code = static_cast<std::int32_t>(CommonErrorCode::Unknown),
                                    const std::optional<std::int64_t> nativeCode = std::nullopt) -> Error {
    return Error{
            .category   = category,
            .code       = code,
            .message    = normalizeErrorMessage(message),
            .nativeCode = nativeCode,
    };
}

[[nodiscard]] inline auto makeError(const ErrorCategory               category,
                                    const CommonErrorCode             code,
                                    const std::string&                message,
                                    const std::optional<std::int64_t> nativeCode = std::nullopt) -> Error {
    return makeError(category, message, static_cast<std::int32_t>(code), nativeCode);
}

[[nodiscard]] inline auto unexpected(const Error& error) -> std::unexpected<Error> {
    return std::unexpected(error);
}

[[nodiscard]] inline auto unexpected(const ErrorCategory category,
                                     const std::string&  message,
                                     const std::int32_t  code = static_cast<std::int32_t>(CommonErrorCode::Unknown),
                                     const std::optional<std::int64_t> nativeCode = std::nullopt)
        -> std::unexpected<Error> {
    return unexpected(makeError(category, message, code, nativeCode));
}

[[nodiscard]] inline auto unexpected(const ErrorCategory               category,
                                     const CommonErrorCode             code,
                                     const std::string&                message,
                                     const std::optional<std::int64_t> nativeCode = std::nullopt)
        -> std::unexpected<Error> {
    return unexpected(makeError(category, code, message, nativeCode));
}

[[nodiscard]] inline auto fromStdErrorCode(std::error_code     error,
                                           const ErrorCategory category = ErrorCategory::System,
                                           std::string         context  = {}) -> Error {
    auto message = std::move(context);
    if (!message.empty()) {
        message += ": ";
    }
    message += error.message();
    return makeError(category, message, error.value(), error.value());
}

[[nodiscard]] inline auto fromException(std::exception const& exception,
                                        const ErrorCategory   category = ErrorCategory::General,
                                        const CommonErrorCode code     = CommonErrorCode::OperationFailed) -> Error {
    return makeError(category, code, exception.what());
}

[[nodiscard]] inline auto fromCurrentException(const ErrorCategory   category = ErrorCategory::General,
                                               const CommonErrorCode code = CommonErrorCode::OperationFailed) -> Error {
    auto exceptionPtr = std::current_exception();
    if (!exceptionPtr) {
        return makeError(category, code, "unknown exception");
    }
    try {
        std::rethrow_exception(exceptionPtr);
    } catch (std::exception const& exception) {
        return fromException(exception, category, code);
    } catch (...) {
        return makeError(category, code, "unknown exception");
    }
}

[[nodiscard]] inline auto errorMessage(Error const& error) noexcept -> std::string const& {
    return error.message;
}

} // namespace CloverNT
