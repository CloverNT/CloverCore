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
    Dl_info info{};
    return ::dladdr(__builtin_return_address(0), &info) != 0 ? info.dli_fbase : nullptr;
#endif
}

} // namespace CloverNT::Core::Utils::System
