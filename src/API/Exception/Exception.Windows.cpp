#include <CloverNT/API/Exception.hpp>

#include <Windows.h>
// dbghelp.h must follow Windows.h.
#include <dbghelp.h>

#include <cstdint>
#include <filesystem>
#include <mutex>

namespace CloverNT {

namespace {
    constexpr DWORD kBreakpointCode = 0x80000003;
    constexpr DWORD kSingleStepCode = 0x80000004;

    [[nodiscard]] auto sehCodeToKind(const DWORD code) noexcept -> FaultKind {
        switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
            return FaultKind::AccessViolation;
        case EXCEPTION_IN_PAGE_ERROR:
            return FaultKind::InPageError;
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            return FaultKind::IllegalInstruction;
        case EXCEPTION_PRIV_INSTRUCTION:
            return FaultKind::PrivilegedInstruction;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            return FaultKind::IntegerDivideByZero;
        case EXCEPTION_INT_OVERFLOW:
            return FaultKind::IntegerOverflow;
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_INVALID_OPERATION:
        case EXCEPTION_FLT_OVERFLOW:
        case EXCEPTION_FLT_UNDERFLOW:
        case EXCEPTION_FLT_DENORMAL_OPERAND:
        case EXCEPTION_FLT_INEXACT_RESULT:
        case EXCEPTION_FLT_STACK_CHECK:
            return FaultKind::FloatingPointError;
        case EXCEPTION_STACK_OVERFLOW:
            return FaultKind::StackOverflow;
        default:
            return FaultKind::Unknown;
        }
    }

    [[nodiscard]] bool shouldTranslate(const DWORD code) noexcept {
        return sehCodeToKind(code) != FaultKind::Unknown;
    }

    void fillRecord(detail::FaultRecord& record, const EXCEPTION_POINTERS* pointers) noexcept {
        const EXCEPTION_RECORD* exceptionRecord = pointers != nullptr ? pointers->ExceptionRecord : nullptr;
        if (exceptionRecord != nullptr) {
            const DWORD code  = exceptionRecord->ExceptionCode;
            record.nativeCode = static_cast<std::uint32_t>(code);
            record.kind       = sehCodeToKind(code);
            record.address    = exceptionRecord->ExceptionAddress;
            if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
                exceptionRecord->NumberParameters >= 2) {
                record.hasAccess          = true;
                const ULONG_PTR operation = exceptionRecord->ExceptionInformation[0];
                record.accessType         = operation == 0   ? FaultAccess::Type::Read
                                            : operation == 1 ? FaultAccess::Type::Write
                                            : operation == 8 ? FaultAccess::Type::Execute
                                                             : FaultAccess::Type::Unknown;
                record.accessAddress      = static_cast<std::uintptr_t>(exceptionRecord->ExceptionInformation[1]);
            }
        }

        void*        frames[detail::kFaultMaxFrames];
        const USHORT captured =
                RtlCaptureStackBackTrace(0, static_cast<DWORD>(detail::kFaultMaxFrames), frames, nullptr);
        record.frameCount = captured;
        for (USHORT i = 0; i < captured; ++i) {
            record.frames[i] = frames[i];
        }
    }

    auto symbolMutex() -> std::mutex& {
        static std::mutex mutex;
        return mutex;
    }

    bool ensureSymbolHandler() {
        static const bool initialized = [] {
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
            return SymInitialize(GetCurrentProcess(), nullptr, TRUE) != FALSE;
        }();
        return initialized;
    }

    struct VehFrame {
        CONTEXT              landing;
        detail::FaultRecord* record;
        volatile bool        faulted;
    };

    thread_local VehFrame* tlsVehFrame = nullptr;

    struct GuardScope {
        VehFrame* previous;
        ~GuardScope() {
            tlsVehFrame = previous;
        }
    };

    auto CALLBACK vectoredHandler(EXCEPTION_POINTERS* pointers) -> LONG {
        if (const DWORD code = pointers->ExceptionRecord->ExceptionCode; !shouldTranslate(code)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        VehFrame* frame = tlsVehFrame;
        if (frame == nullptr) {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        fillRecord(*frame->record, pointers);
        frame->faulted           = true;
        *pointers->ContextRecord = frame->landing;
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    void ensureVectoredHandler() {
        static std::once_flag once;
        std::call_once(once, [] { AddVectoredExceptionHandler(1 /*first*/, &vectoredHandler); });
    }

    auto structuredFilter(const DWORD code, const EXCEPTION_POINTERS* pointers, detail::FaultRecord& record) noexcept
            -> int {
        if (!shouldTranslate(code)) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        fillRecord(record, pointers);
        return EXCEPTION_EXECUTE_HANDLER;
    }

} // namespace

auto StackTrace::capture(const std::size_t skip, const std::size_t maxFrames) noexcept -> StackTrace {
    StackTrace trace;
    if (maxFrames == 0) {
        return trace;
    }

    constexpr std::size_t kHardCap = 128;
    void*                 buffer[kHardCap];
    const std::size_t     want = std::min(maxFrames, kHardCap);

    const USHORT captured =
            RtlCaptureStackBackTrace(static_cast<DWORD>(skip + 1), static_cast<DWORD>(want), buffer, nullptr);
    trace.mAddresses.assign(buffer, buffer + captured);
    return trace;
}

auto StackTrace::frames() const -> std::vector<Frame> {
    std::vector<Frame> out;
    out.reserve(mAddresses.size());

    const std::scoped_lock lock(symbolMutex());
    const bool             haveSymbols = ensureSymbolHandler();
    HANDLE                 process     = GetCurrentProcess();

    alignas(SYMBOL_INFO) std::byte symbolStorage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(char)];
    auto*                          symbol = reinterpret_cast<SYMBOL_INFO*>(symbolStorage);

    for (const void* address: mAddresses) {
        Frame frame;
        frame.address           = address;
        const auto addressValue = reinterpret_cast<DWORD64>(address);

        if (haveSymbols) {
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen   = MAX_SYM_NAME;
            DWORD64 displacement = 0;
            if (SymFromAddr(process, addressValue, &displacement, symbol)) {
                frame.function.assign(symbol->Name, symbol->NameLen);
            }

            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct      = sizeof(IMAGEHLP_LINE64);
            DWORD lineDisplacement = 0;
            if (SymGetLineFromAddr64(process, addressValue, &lineDisplacement, &line) && line.FileName != nullptr) {
                frame.file = std::filesystem::path(line.FileName).filename().string();
                frame.line = line.LineNumber;
            }

            IMAGEHLP_MODULE64 moduleInfo{};
            moduleInfo.SizeOfStruct = sizeof(IMAGEHLP_MODULE64);
            if (SymGetModuleInfo64(process, addressValue, &moduleInfo)) {
                frame.module = moduleInfo.ModuleName;
            }
        }

        out.push_back(std::move(frame));
    }
    return out;
}

auto sehCodeName(std::uint32_t code) noexcept -> std::string_view {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "access violation";
    case EXCEPTION_IN_PAGE_ERROR:
        return "in-page error";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "illegal instruction";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "privileged instruction";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "integer divide by zero";
    case EXCEPTION_INT_OVERFLOW:
        return "integer overflow";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return "float divide by zero";
    case EXCEPTION_STACK_OVERFLOW:
        return "stack overflow";
    case EXCEPTION_DATATYPE_MISALIGNMENT:
        return "datatype misalignment";
    case 0xC0000374:
        return "heap corruption";
    case 0xC0000409:
        return "stack buffer overrun";
    case kBreakpointCode:
        return "breakpoint";
    case kSingleStepCode:
        return "single step";
    default:
        return "structured exception";
    }
}

bool detail::vehTry(void (*invoke)(void*), void* ctx, FaultRecord& record) {
    ensureVectoredHandler();

    VehFrame frame{};
    frame.record  = &record;
    frame.faulted = false;
    const GuardScope scope{tlsVehFrame};
    tlsVehFrame = &frame;

    RtlCaptureContext(&frame.landing);
    if (!frame.faulted) {
        invoke(ctx);
    }
    return !frame.faulted;
}

bool detail::sehTry(void (*invoke)(void*), void* ctx, FaultRecord& record) {
    __try {
        invoke(ctx);
        return true;
    } __except (structuredFilter(GetExceptionCode(), GetExceptionInformation(), record)) {
        return false;
    }
}

} // namespace CloverNT
