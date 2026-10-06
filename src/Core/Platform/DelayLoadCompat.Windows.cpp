#include <CloverNT/API/Logger.hpp>
#include <CloverNT/Core/Platform/DelayLoadCompat.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <delayimp.h>
#include <windows.h>

namespace CloverNT::Core::Platform {
namespace {

    auto logger() -> Logger& {
        static auto instance = LoggerRegistry::getInstance().getOrCreate("CloverCore");
        return *instance;
    }

    constexpr UINT kIncompatibleQqntExitCode = 0xC10FE500u;

    std::mutex               gMissingMutex;
    std::vector<std::string> gMissing;

    [[noreturn]] void bailOutIncompatible() {
        std::lock_guard lock(gMissingMutex);

        logger().error("incompatible QQNT runtime; unshimmed delay-imports:");
        for (const auto& m: gMissing) {
            logger().error("    {}", m);
            OutputDebugStringA(("unshimmed delay-import: " + m + "\n").c_str());
        }

        TerminateProcess(GetCurrentProcess(), kIncompatibleQqntExitCode);
        std::unreachable();
    }

    [[noreturn]] void unresolvedImportTripwire() {
        bailOutIncompatible();
    }

    struct DelayLoadShim {
        const char* readableName;
        const char* importName;
        FARPROC (*handler)(HMODULE runtimeModule);
    };

    FARPROC shimCreateHandle(const HMODULE rt) {
        return GetProcAddress(rt, "?CreateHandle@HandleScope@v8@@KAPEA_KPEAVIsolate@2@_K@Z");
    }

    FARPROC shimFunctionNew(const HMODULE rt) {
        return GetProcAddress(
                rt,
                "?New@Function@v8@@SA?AV?$MaybeLocal@VFunction@v8@@@2@V?$Local@VContext@v8@@@2@P6AXAEBV?"
                "$FunctionCallbackInfo@VValue@v8@@@2@@ZV?$Local@VData@v8@@@2@HW4ConstructorBehavior@2@W4SideEffectType@"
                "2@@Z");
    }

    using SlowGetTaggedFn = void* (*) (void* self, int index, unsigned short tag);
    std::atomic<SlowGetTaggedFn> gSlowGetTagged{nullptr};

    void* slowGetAlignedThunk(void* self, int index) { // old ABI: (this, index)
        const auto fn = gSlowGetTagged.load(std::memory_order_acquire);
        return fn != nullptr ? fn(self, index, /*kEmbedderDataTypeTagDefault*/ 0) : nullptr;
    }

    FARPROC shimSlowGetAlignedPointer(const HMODULE rt) {
        const auto tagged = reinterpret_cast<SlowGetTaggedFn>(
                GetProcAddress(rt, "?SlowGetAlignedPointerFromInternalField@Object@v8@@AEAAPEAXHG@Z"));
        if (tagged == nullptr) {
            return nullptr;
        }
        gSlowGetTagged.store(tagged, std::memory_order_release);
        return reinterpret_cast<FARPROC>(&slowGetAlignedThunk);
    }

    constexpr DelayLoadShim kShims[] = {
            {.readableName = "v8::HandleScope::CreateHandle (internal::Isolate* -> Isolate*)",
             .importName   = "?CreateHandle@HandleScope@v8@@KAPEA_KPEAVIsolate@internal@2@_K@Z",
             .handler      = &shimCreateHandle},
            {.readableName = "v8::Function::New (data Local<Value> -> Local<Data>)",
             .importName   = "?New@Function@v8@@SA?AV?$MaybeLocal@VFunction@v8@@@2@V?$Local@VContext@v8@@@2@P6AXAEBV?"
                             "$FunctionCallbackInfo@VValue@v8@@@2@@ZV?$Local@VValue@v8@@@2@HW4ConstructorBehavior@2@"
                             "W4SideEffectType@2@@Z",
             .handler      = &shimFunctionNew},
            {.readableName = "v8::Object::SlowGetAlignedPointerFromInternalField (+EmbedderDataTypeTag=0)",
             .importName   = "?SlowGetAlignedPointerFromInternalField@Object@v8@@AEAAPEAXH@Z",
             .handler      = &shimSlowGetAlignedPointer},
    };

    void recordMissing(const char* dll, const char* sym) {
        std::lock_guard lock(gMissingMutex);
        std::string     entry = std::string(dll) + " : " + sym;
        if (std::find(gMissing.begin(), gMissing.end(), entry) == gMissing.end()) {
            gMissing.push_back(std::move(entry));
        }
    }

    FARPROC WINAPI delayFailureHook(const unsigned dliNotify, const PDelayLoadInfo pdli) {
        if (dliNotify != dliFailGetProc || pdli == nullptr) {
            return nullptr;
        }

        const char* const sym = pdli->dlp.fImportByName ? pdli->dlp.szProcName : "<imported by ordinal>";
        const char* const dll = pdli->szDll != nullptr ? pdli->szDll : "<unknown module>";

        // Dispatch by symbol to the matching shim handler.
        if (pdli->dlp.fImportByName) {
            for (const auto& [readableName, importName, handler]: kShims) {
                if (std::strcmp(sym, importName) != 0) {
                    continue;
                }
                if (const auto replacement = handler(pdli->hmodCur); replacement != nullptr) {
                    logger().info("using shim: {}", readableName);
                    return replacement;
                }
                logger().error("shim '{}' could not resolve its runtime target for {}.", readableName, sym);
                break;
            }
        }

        recordMissing(dll, sym);
        return reinterpret_cast<FARPROC>(&unresolvedImportTripwire);
    }

} // namespace

extern "C" const PfnDliHook __pfnDliFailureHook2 = &delayFailureHook;

void failFastOnMissingDelayImports(const char* context) {
    bool hasMissing = false;
    {
        std::lock_guard lock(gMissingMutex);
        hasMissing = !gMissing.empty();
    }
    if (hasMissing) {
        logger().error("incompatible QQNT runtime; delay-import bind of {} left unshimmed symbols.",
                       context != nullptr ? context : "<module>");
        bailOutIncompatible();
    }
}

} // namespace CloverNT::Core::Platform
