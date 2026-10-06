#include <CloverNT/API/Exception.hpp>

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>

namespace CloverNT {

namespace {

    [[nodiscard]] std::string formatFrame(std::size_t index, const StackTrace::Frame& frame) {
        return std::format("#{:<2} {} ({}:{}) [{}]",
                           index,
                           frame.function.empty() ? "<unknown>" : frame.function.c_str(),
                           frame.file.empty() ? "?" : frame.file.c_str(),
                           frame.line,
                           frame.module.empty() ? "?" : frame.module.c_str());
    }

    [[nodiscard]] std::string_view accessVerb(FaultAccess::Type type) noexcept {
        switch (type) {
        case FaultAccess::Type::Read:
            return "reading";
        case FaultAccess::Type::Write:
            return "writing";
        case FaultAccess::Type::Execute:
            return "executing";
        case FaultAccess::Type::Unknown:
            break;
        }
        return "accessing";
    }

    [[nodiscard]] std::string formatStructuredMessage(FaultKind                         kind,
                                                      const void*                       address,
                                                      const std::optional<FaultAccess>& access,
                                                      std::uint32_t                     nativeCode) {
        const std::string_view name        = faultKindName(kind);
        const auto             instruction = reinterpret_cast<std::uintptr_t>(address);
        if (access.has_value()) {
            return std::format("{} {} 0x{:016X} at 0x{:016X} (native 0x{:08X})",
                               name,
                               accessVerb(access->type),
                               access->address,
                               instruction,
                               nativeCode);
        }
        return std::format("{} at 0x{:016X} (native 0x{:08X})", name, instruction, nativeCode);
    }

} // namespace

StackTrace StackTrace::fromAddresses(const void* const* addresses, std::size_t count) {
    StackTrace trace;
    if (addresses != nullptr && count > 0) {
        trace.mAddresses.assign(addresses, addresses + count);
    }
    return trace;
}

std::string StackTrace::toString(std::size_t maxFrames) const {
    const auto resolved = frames();
    if (resolved.empty()) {
        return "<no stack trace>";
    }
    const std::size_t shown = std::min(resolved.size(), maxFrames);
    std::string       out;
    for (std::size_t i = 0; i < shown; ++i) {
        out += formatFrame(i, resolved[i]);
        out.push_back('\n');
    }
    if (resolved.size() > shown) {
        out += std::format("... {} more frames\n", resolved.size() - shown);
    }
    return out;
}

Exception::Exception(std::string message, ErrorCategory category, const std::int32_t code)
    : mError(makeError(category, std::move(message), code)), mStackTrace(StackTrace::capture(1)) {}

Exception::Exception(Error error) : mError(std::move(error)), mStackTrace(StackTrace::capture(1)) {}

Exception::Exception(Error error, StackTrace trace) : mError(std::move(error)), mStackTrace(std::move(trace)) {}

const char* Exception::what() const noexcept {
    return mError.message.c_str();
}

std::string Exception::describe(std::size_t maxFrames) const {
    std::string out = mError.message;
    if (!mStackTrace.empty()) {
        out += "\nstack trace:\n";
        out += mStackTrace.toString(maxFrames);
    }
    return out;
}

void Exception::log(const Logger& logger, LogLevel level, std::size_t maxFrames) const {
    logger.log(level, "exception: {}", mError.message);
    const auto resolved = mStackTrace.frames();
    if (resolved.empty()) {
        return;
    }
    const std::size_t shown = std::min(resolved.size(), maxFrames);
    logger.log(level, "stack trace ({} frames):", resolved.size());
    for (std::size_t i = 0; i < shown; ++i) {
        logger.log(level, "  {}", formatFrame(i, resolved[i]));
    }
    if (resolved.size() > shown) {
        logger.log(level, "  ... {} more frames", resolved.size() - shown);
    }
}

OperationFailedException::OperationFailedException(std::string_view detail)
    : Exception(std::format("operation failed: {}", detail),
                ErrorCategory::General,
                static_cast<std::int32_t>(CommonErrorCode::OperationFailed)) {}

InvalidArgumentException::InvalidArgumentException(std::string_view detail)
    : Exception(std::format("invalid argument: {}", detail),
                ErrorCategory::Argument,
                static_cast<std::int32_t>(CommonErrorCode::InvalidArgument)) {}

NotFoundException::NotFoundException(std::string_view detail)
    : Exception(std::format("not found: {}", detail),
                ErrorCategory::General,
                static_cast<std::int32_t>(CommonErrorCode::NotFound)) {}

InvalidStateException::InvalidStateException(std::string_view detail)
    : Exception(std::format("invalid state: {}", detail),
                ErrorCategory::General,
                static_cast<std::int32_t>(CommonErrorCode::InvalidState)) {}

UnsupportedException::UnsupportedException(std::string_view detail)
    : Exception(std::format("unsupported: {}", detail),
                ErrorCategory::General,
                static_cast<std::int32_t>(CommonErrorCode::Unsupported)) {}

std::string_view faultKindName(const FaultKind kind) noexcept {
    switch (kind) {
    case FaultKind::AccessViolation:
        return "access violation";
    case FaultKind::InPageError:
        return "in-page error";
    case FaultKind::IllegalInstruction:
        return "illegal instruction";
    case FaultKind::PrivilegedInstruction:
        return "privileged instruction";
    case FaultKind::IntegerDivideByZero:
        return "integer divide by zero";
    case FaultKind::IntegerOverflow:
        return "integer overflow";
    case FaultKind::FloatingPointError:
        return "floating-point error";
    case FaultKind::StackOverflow:
        return "stack overflow";
    case FaultKind::Breakpoint:
        return "breakpoint";
    case FaultKind::Unknown:
        break;
    }
    return "hardware fault";
}

StructuredException::StructuredException(const FaultKind                   kind,
                                         std::uint32_t                     nativeCode,
                                         const void*                       faultingInstruction,
                                         const std::optional<FaultAccess>& access,
                                         StackTrace                        trace)
    : Exception(makeError(ErrorCategory::System,
                          formatStructuredMessage(kind, faultingInstruction, access, nativeCode),
                          static_cast<std::int32_t>(CommonErrorCode::OperationFailed),
                          nativeCode),
                std::move(trace)),
      mKind(kind),
      mNativeCode(nativeCode),
      mFaultingInstruction(faultingInstruction),
      mAccess(access) {}

std::optional<FaultAccess::Type> StructuredException::accessType() const noexcept {
    if (mAccess.has_value()) {
        return mAccess->type;
    }
    return std::nullopt;
}

std::optional<std::uintptr_t> StructuredException::faultAddress() const noexcept {
    if (mAccess.has_value()) {
        return mAccess->address;
    }
    return std::nullopt;
}

void detail::throwStructured(const FaultRecord& record) {
    StackTrace                 trace = StackTrace::fromAddresses(record.frames, record.frameCount);
    std::optional<FaultAccess> access;
    if (record.hasAccess) {
        access = FaultAccess{record.accessType, record.accessAddress};
    }
    throw StructuredException(record.kind, record.nativeCode, record.address, access, std::move(trace));
}

} // namespace CloverNT
