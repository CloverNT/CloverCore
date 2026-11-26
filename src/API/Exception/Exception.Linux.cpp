#ifndef _GNU_SOURCE
    #define _GNU_SOURCE 1
#endif

#include <CloverNT/API/Exception.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <mutex>
#include <utility>
#include <vector>

#include <csignal>
#include <setjmp.h>
#include <signal.h>
#include <ucontext.h>

#if __has_include(<execinfo.h>)
    #include <cxxabi.h>
    #include <dlfcn.h>
    #include <execinfo.h>

    #include <cstdlib>
    #define CLOVERNT_HAVE_EXECINFO 1
#else
    #define CLOVERNT_HAVE_EXECINFO 0
#endif

namespace CloverNT {

namespace {

    constexpr int         kSignals[]   = {SIGSEGV, SIGBUS, SIGILL, SIGFPE};
    constexpr std::size_t kSignalCount = std::size(kSignals);

    struct sigaction gOldActions[kSignalCount];
    std::atomic      gHandlersInstalled{false};
    std::atomic      gActiveGuards{0};

    struct VehFrame {
        sigjmp_buf           landingPad;
        detail::FaultRecord* record;
    };

    thread_local VehFrame* tlsVehFrame = nullptr;

    struct GuardScope {
        VehFrame* previous;
        ~GuardScope() {
            tlsVehFrame = previous;
            gActiveGuards.fetch_sub(1, std::memory_order_release);
        }
    };

    [[nodiscard]] auto extractFaultPc(const void* context) noexcept -> const void* {
        if (context == nullptr) {
            return nullptr;
        }
        const auto* uctx = static_cast<const ucontext_t*>(context);
#if defined(__x86_64__)
        return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(uctx->uc_mcontext.gregs[REG_RIP]));
#elif defined(__aarch64__)
        return reinterpret_cast<const void*>(static_cast<std::uintptr_t>(uctx->uc_mcontext.pc));
#else
        (void) uctx;
        return nullptr;
#endif
    }

    [[nodiscard]] auto signalToKind(int signalNumber, const siginfo_t* info) noexcept -> FaultKind {
        switch (signalNumber) {
        case SIGSEGV:
            return FaultKind::AccessViolation;
        case SIGBUS:
            return FaultKind::InPageError;
        case SIGILL:
            if (info != nullptr && (info->si_code == ILL_PRVOPC || info->si_code == ILL_PRVREG)) {
                return FaultKind::PrivilegedInstruction;
            }
            return FaultKind::IllegalInstruction;
        case SIGFPE:
            if (info != nullptr && info->si_code == FPE_INTDIV) {
                return FaultKind::IntegerDivideByZero;
            }
            if (info != nullptr && info->si_code == FPE_INTOVF) {
                return FaultKind::IntegerOverflow;
            }
            return FaultKind::FloatingPointError;
        default:
            return FaultKind::Unknown;
        }
    }

    [[nodiscard]] auto savedIndex(const int signalNumber) noexcept -> int {
        for (std::size_t i = 0; i < kSignalCount; ++i) {
            if (kSignals[i] == signalNumber) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    auto chainToPrevious(int signalNumber, siginfo_t* info, void* context) noexcept -> void {
        const int index = savedIndex(signalNumber);
        if (index < 0) {
            struct sigaction dfl{};
            dfl.sa_handler = SIG_DFL;
            sigemptyset(&dfl.sa_mask);
            sigaction(signalNumber, &dfl, nullptr);
            raise(signalNumber);
            return;
        }
        const struct sigaction& previous = gOldActions[index];
        if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
            previous.sa_sigaction(signalNumber, info, context);
        } else if (previous.sa_handler == SIG_IGN) {
            return;
        } else if (previous.sa_handler != SIG_DFL && previous.sa_handler != nullptr) {
            previous.sa_handler(signalNumber);
        } else {
            sigaction(signalNumber, &previous, nullptr);
            raise(signalNumber);
        }
    }

    auto faultSignalHandler(int signalNumber, siginfo_t* info, void* context) -> void {
        if (gActiveGuards.load(std::memory_order_acquire) <= 0) {
            chainToPrevious(signalNumber, info, context);
            return;
        }
        VehFrame* frame = tlsVehFrame;
        if (frame == nullptr) {
            chainToPrevious(signalNumber, info, context);
            return;
        }

        detail::FaultRecord& record = *frame->record;
        record.nativeCode           = static_cast<std::uint32_t>(signalNumber);
        record.kind                 = signalToKind(signalNumber, info);
        const void* pc              = extractFaultPc(context);
        record.address              = pc != nullptr ? pc : (info != nullptr ? info->si_addr : nullptr);
        if (signalNumber == SIGSEGV || signalNumber == SIGBUS) {
            record.hasAccess     = true;
            record.accessType    = FaultAccess::Type::Unknown; // read/write not portably available
            record.accessAddress = reinterpret_cast<std::uintptr_t>(info != nullptr ? info->si_addr : nullptr);
        }

        if (record.address != nullptr) {
            record.frames[0]  = record.address;
            record.frameCount = 1;
        } else {
            record.frameCount = 0;
        }
        siglongjmp(frame->landingPad, 1);
    }

    struct SignalHandlerLifecycle {
        void install() {
            static std::once_flag once;
            std::call_once(once, [] {
                struct sigaction action{};
                action.sa_sigaction = &faultSignalHandler;
                sigemptyset(&action.sa_mask);
                action.sa_flags = SA_SIGINFO | SA_ONSTACK;
                for (std::size_t i = 0; i < kSignalCount; ++i) {
                    sigaction(kSignals[i], &action, &gOldActions[i]);
                }
                gHandlersInstalled.store(true, std::memory_order_release);
            });
        }

        ~SignalHandlerLifecycle() {
            if (!gHandlersInstalled.exchange(false, std::memory_order_acq_rel)) {
                return;
            }
            for (std::size_t i = 0; i < kSignalCount; ++i) {
                struct sigaction current{};
                if (sigaction(kSignals[i], nullptr, &current) == 0 && (current.sa_flags & SA_SIGINFO) != 0 &&
                    current.sa_sigaction == &faultSignalHandler) {
                    sigaction(kSignals[i], &gOldActions[i], nullptr);
                }
            }
        }
    };

    auto ensureSignalHandlers() -> void {
        static SignalHandlerLifecycle lifecycle;
        lifecycle.install();
    }

} // namespace

auto StackTrace::capture([[maybe_unused]] std::size_t skip, std::size_t maxFrames) noexcept -> StackTrace {
    StackTrace trace;
    if (maxFrames == 0) {
        return trace;
    }
#if CLOVERNT_HAVE_EXECINFO
    constexpr std::size_t kHardCap = 128;
    if (skip >= kHardCap) {
        return trace;
    }
    const std::size_t cappedMax = std::min(maxFrames, kHardCap - skip);
    const std::size_t want      = cappedMax + skip + 1; // +1 drops this capture() frame
    void*             buffer[kHardCap + 1];
    const int         rawCount = ::backtrace(buffer, static_cast<int>(want));
    if (rawCount <= 0) {
        return trace;
    }
    const auto        count = static_cast<std::size_t>(rawCount);
    const std::size_t start = std::min(skip + 1, count);
    try {
        trace.mAddresses.reserve(std::min(cappedMax, count - start));
        for (std::size_t i = start; i < count && trace.mAddresses.size() < cappedMax; ++i) {
            trace.mAddresses.push_back(buffer[i]);
        }
    } catch (...) {
        trace.mAddresses.clear();
    }
#endif
    return trace;
}

auto StackTrace::frames() const -> std::vector<Frame> {
    std::vector<Frame> out;
    out.reserve(mAddresses.size());
    for (const void* address: mAddresses) {
        Frame frame;
        frame.address = address;
#if CLOVERNT_HAVE_EXECINFO
        Dl_info info{};
        if (dladdr(address, &info) != 0) {
            if (info.dli_fname != nullptr) {
                frame.module = std::filesystem::path(info.dli_fname).filename().string();
            }
            if (info.dli_sname != nullptr) {
                int   status    = 0;
                char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
                if (status == 0 && demangled != nullptr) {
                    frame.function = demangled;
                } else {
                    frame.function = info.dli_sname;
                }
                std::free(demangled);
            }
        }
#endif
        out.push_back(std::move(frame));
    }
    return out;
}

auto detail::vehTry(void (*invoke)(void*), void* ctx, FaultRecord& record) -> bool {
    ensureSignalHandlers();

    VehFrame frame{};
    frame.record = &record;
    gActiveGuards.fetch_add(1, std::memory_order_acq_rel);
    const GuardScope scope{tlsVehFrame};
    tlsVehFrame = &frame;

    if (sigsetjmp(frame.landingPad, 1) == 0) {
        invoke(ctx);
        return true;
    }
    return false;
}

} // namespace CloverNT
