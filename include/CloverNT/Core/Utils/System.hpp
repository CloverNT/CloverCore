#pragma once

#include <CloverNT/API/Macros.hpp>

#ifdef CloverNT_SYSTEM_WINDOWS
// __ImageBase is a linker-provided pseudo-symbol; its name is fixed by the toolchain and cannot be renamed.
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
extern "C" char __ImageBase;
#elif defined(CloverNT_SYSTEM_LINUX)
    #include <dlfcn.h>
#endif

namespace CloverNT::Core::Utils::System {

[[nodiscard]] inline ModuleHandle GetCurrentModuleHandle() noexcept {
#ifdef CloverNT_SYSTEM_WINDOWS
    return reinterpret_cast<ModuleHandle>(&__ImageBase);
#elif defined(CloverNT_SYSTEM_LINUX)
    static int anchor = 0;
    Dl_info    info{};
    return ::dladdr(&anchor, &info) != 0 ? info.dli_fbase : nullptr;
#endif
}

[[nodiscard]] inline ModuleHandle GetModuleHandleOf(const void* address) noexcept {
#ifdef CloverNT_SYSTEM_WINDOWS
    HMODULE module = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCWSTR>(address),
                              &module) != 0
                   ? module
                   : nullptr;
#elif defined(CloverNT_SYSTEM_LINUX)
    Dl_info info{};
    return ::dladdr(address, &info) != 0 ? info.dli_fbase : nullptr;
#endif
}

} // namespace CloverNT::Core::Utils::System
