#pragma once
#ifdef CloverNT_SYSTEM_WINDOWS
    #define CloverNT_EXPORT __declspec(dllexport)
    #define CloverNT_IMPORT __declspec(dllimport)
    #include <Windows.h>
    #if defined(CloverNT_KERNEL_EXPORTS)
        #include <Mortis/Inject.hpp>
        #define CloverNT_RESTORE_AFTER_INJECT() (void) Mortis::ImportTableInjector::RestoreAfterWith()
    #else
        #define CloverNT_RESTORE_AFTER_INJECT() ((void) 0)
    #endif
using ModuleHandle = HMODULE;

    #define LIBRARY_ENTRYPOINT(NAMESPACE, LOAD_FUNC, UNLOAD_FUNC)                                                      \
        extern "C" BOOL WINAPI DllMain(HINSTANCE hModule, DWORD reason, LPVOID) {                                      \
            switch (reason) {                                                                                          \
            case DLL_PROCESS_ATTACH:                                                                                   \
                CloverNT_RESTORE_AFTER_INJECT();                                                                       \
                DisableThreadLibraryCalls(hModule);                                                                    \
                NAMESPACE::LOAD_FUNC(hModule);                                                                         \
                break;                                                                                                 \
            case DLL_PROCESS_DETACH:                                                                                   \
                NAMESPACE::UNLOAD_FUNC(hModule);                                                                       \
                break;                                                                                                 \
            case DLL_THREAD_ATTACH:                                                                                    \
            case DLL_THREAD_DETACH:                                                                                    \
                break;                                                                                                 \
            default:                                                                                                   \
                break;                                                                                                 \
            }                                                                                                          \
            return TRUE;                                                                                               \
        }


#elif defined(CloverNT_SYSTEM_LINUX)
    #define CloverNT_EXPORT __attribute__((visibility("default")))
    #define CloverNT_IMPORT
    #include <cstddef>
    #include <unistd.h>
using ModuleHandle = void*;

    #define LIBRARY_ENTRYPOINT(NAMESPACE, LOAD_FUNC, UNLOAD_FUNC)                                                      \
        __attribute__((constructor)) static void loadLibrary() {                                                       \
            ::unsetenv("LD_PRELOAD");                                                                                  \
            NAMESPACE::LOAD_FUNC(nullptr);                                                                             \
        }                                                                                                              \
        [[maybe_unused]] static constexpr auto* kCloverUnloadFn = &NAMESPACE::UNLOAD_FUNC;
#else
    #error "Unsupported platform"
#endif

#if defined(CloverNT_EXPORTS)
    #define CloverNT_API CloverNT_EXPORT
#elif defined(CloverNT_API_STATIC)
    #define CloverNT_API
#else
    #define CloverNT_API CloverNT_IMPORT
#endif
