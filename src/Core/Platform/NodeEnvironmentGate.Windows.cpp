#include <CloverNT/API/Events/BuiltInEvents.hpp>
#include <CloverNT/API/Events/EventBus.hpp>
#include <CloverNT/API/Events/Subscription.hpp>
#include <CloverNT/API/Exception.hpp>
#include <CloverNT/API/Logger.hpp>
#include <CloverNT/API/Memory/Hook.hpp>
#include <CloverNT/API/Plugin/PluginManager.hpp>
#include <CloverNT/Core/Platform/DelayLoadCompat.hpp>
#include <CloverNT/Core/Platform/NodeEnvironmentGate.hpp>

#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include <delayimp.h>
#include <windows.h>

namespace CloverNT::Core::Platform {
namespace {

    auto logger() -> Logger& {
        static auto instance = LoggerRegistry::getInstance().getOrCreate("CloverCore");
        return *instance;
    }

    using ExecSig = v8::MaybeLocal<v8::Value>(node::Environment*,
                                              node::StartExecutionCallback,
                                              node::EmbedderPreloadCallback);
    using StrSig  = v8::MaybeLocal<v8::Value>(node::Environment*, std::string_view, node::EmbedderPreloadCallback);

    [[nodiscard]] bool isModuleLoaded(const wchar_t* name) {
        return GetModuleHandleW(name) != nullptr;
    }

    [[nodiscard]] bool bindDelayImports(const char* moduleName) {
        const HRESULT hr = __HrLoadAllImportsForDll(moduleName);
        failFastOnMissingDelayImports(moduleName);
        return SUCCEEDED(hr);
    }

    template <class Fn>
        requires std::is_pointer_v<Fn> && std::is_function_v<std::remove_pointer_t<Fn>>
    [[nodiscard]] auto resolveImportThunk(const Fn importTarget) -> Fn {
#if defined(CloverNT_ARCH_X64)
        try {
            return guardSeh([&]() -> Fn {
                const auto* code = reinterpret_cast<const std::uint8_t*>(importTarget);
                if (code == nullptr) {
                    return importTarget;
                }
                if (code[0] == 0xE9) {
                    std::int32_t rel = 0;
                    std::memcpy(&rel, code + 1, sizeof(rel));
                    code = code + 5 + rel;
                }
                if (code[0] == 0xFF && code[1] == 0x25) {
                    std::int32_t displacement = 0;
                    std::memcpy(&displacement, code + 2, sizeof(displacement));
                    const auto* slot = code + 6 + displacement;
                    return *reinterpret_cast<Fn const*>(slot);
                }
                if (code[0] == 0x48 && code[1] == 0x8D && code[2] == 0x05) {
                    std::int32_t displacement = 0;
                    std::memcpy(&displacement, code + 3, sizeof(displacement));
                    const auto* slot     = code + 7 + displacement; // &__imp_<sym>
                    const auto  resolved = *reinterpret_cast<Fn const*>(slot);
                    if (resolved != nullptr && reinterpret_cast<const std::uint8_t*>(resolved) != code) {
                        return resolved;
                    }
                }
                return importTarget;
            });
        } catch (const StructuredException&) {
            return importTarget;
        }
#else
        return importTarget;
#endif
    }

} // namespace

struct NodeEnvironmentGate::Impl {
    explicit Impl(std::shared_ptr<State> state) : mState(std::move(state)) {
        std::lock_guard lock(sActiveMutex);
        sActiveState = mState;
    }

    ~Impl() {
        clearActive();
    }

    Impl(Impl const&)            = delete;
    Impl& operator=(Impl const&) = delete;

    void arm() {
        if (!mDllSub) {
            auto sub = CloverNT::Event::EventBus::on<CloverNT::Event::Events::DllLoad>(
                    [this](const CloverNT::Event::DllLoadInfo& info) {
                        if (_stricmp(info.baseName.c_str(), "QQNT.dll") == 0) {
                            installHook();
                        }
                    },
                    CloverNT::Event::EventPriority::High,
                    "CloverCore");
            if (sub) {
                mDllSub = std::move(*sub);
            } else {
                logger().error("NodeEnvironmentGate: failed to subscribe to DllLoad: {}", sub.error().message);
            }
        }
        if (isModuleLoaded(L"QQNT.dll")) {
            installHook();
        }
    }

    void disarm() {
        clearActive();
        mDllSub.unsubscribe();
        removeHooks();
    }

private:
    void clearActive() {
        std::lock_guard lock(sActiveMutex);
        if (const auto current = sActiveState.lock(); current == mState) {
            sActiveState.reset();
        }
    }

    void installHook() {
        std::lock_guard lock(mHookMutex);
        if (mHookInstalled) {
            return;
        }
        if (!isModuleLoaded(L"QQNT.dll")) {
            return;
        }
        if (!bindDelayImports("QQNT.dll")) {
            logger().error("NodeEnvironmentGate: failed to resolve QQNT.dll delay imports");
            return;
        }

        const auto execFn = resolveImportThunk(static_cast<ExecSig*>(&node::LoadEnvironment));
        const auto strFn  = resolveImportThunk(static_cast<StrSig*>(&node::LoadEnvironment));
        bool       any    = false;
        const auto owner  = Plugin::PluginManager::getPlugin("CloverCore");
        if (auto hook = Memory::Hook::Inline::create(execFn, &Impl::detourLoadEnvExec, {.owner = owner}); hook) {
            mExecHook.emplace(std::move(hook).value());
            any = true;
        } else {
            logger().error("NodeEnvironmentGate: failed to hook LoadEnvironment(exec): {}", hook.error().message);
        }
        if (auto hook = Memory::Hook::Inline::create(strFn, &Impl::detourLoadEnvStr, {.owner = owner}); hook) {
            mStrHook.emplace(std::move(hook).value());
            any = true;
        } else {
            logger().error("NodeEnvironmentGate: failed to hook LoadEnvironment(str): {}", hook.error().message);
        }

        if (any) {
            mHookInstalled = true;
        }
    }

    void removeHooks() {
        std::lock_guard lock(mHookMutex);
        mExecHook.reset();
        mStrHook.reset();
        mHookInstalled = false;
    }

    static auto wrapPreload(node::EmbedderPreloadCallback preload) -> node::EmbedderPreloadCallback {
        return [preload = std::move(preload)](
                       node::Environment* e, const v8::Local<v8::Value> process, const v8::Local<v8::Value> require) {
            try {
                std::shared_ptr<State> state;
                {
                    std::lock_guard lock(sActiveMutex);
                    state = sActiveState.lock();
                }
                if (state) {
                    onPreloadState(state, e, process, require);
                }
                if (preload) {
                    preload(e, process, require);
                }
            } catch (const std::exception& exception) {
                logger().error("NodeEnvironmentGate: preload callback faulted, degrading silently: {}",
                               exception.what());
            } catch (...) {
                logger().error("NodeEnvironmentGate: preload callback faulted with an unknown exception");
            }
        };
    }

    static auto detourLoadEnvExec(const Memory::Hook::OriginalFunction<ExecSig>& original,
                                  node::Environment*                             env,
                                  node::StartExecutionCallback                   cb,
                                  node::EmbedderPreloadCallback                  preload) -> v8::MaybeLocal<v8::Value> {
        try {
            return original(env, std::move(cb), wrapPreload(std::move(preload)));
        } catch (const std::exception& exception) {
            logger().error("NodeEnvironmentGate: LoadEnvironment(exec) detour faulted, degrading silently: {}",
                           exception.what());
        } catch (...) {
            logger().error("NodeEnvironmentGate: LoadEnvironment(exec) detour faulted with an unknown exception");
        }
        return v8::MaybeLocal<v8::Value>{};
    }

    static auto detourLoadEnvStr(const Memory::Hook::OriginalFunction<StrSig>& original,
                                 node::Environment*                            env,
                                 const std::string_view                        source,
                                 node::EmbedderPreloadCallback                 preload) -> v8::MaybeLocal<v8::Value> {
        try {
            return original(env, source, wrapPreload(std::move(preload)));
        } catch (const std::exception& exception) {
            logger().error("NodeEnvironmentGate: LoadEnvironment(str) detour faulted, degrading silently: {}",
                           exception.what());
        } catch (...) {
            logger().error("NodeEnvironmentGate: LoadEnvironment(str) detour faulted with an unknown exception");
        }
        return v8::MaybeLocal<v8::Value>{};
    }

    std::shared_ptr<State>              mState;
    CloverNT::Event::ScopedSubscription mDllSub;

    std::mutex                                         mHookMutex;
    std::optional<Memory::Hook::InlineHandle<ExecSig>> mExecHook;
    std::optional<Memory::Hook::InlineHandle<StrSig>>  mStrHook;
    bool                                               mHookInstalled{};

    static std::mutex           sActiveMutex;
    static std::weak_ptr<State> sActiveState;
};

std::mutex                                NodeEnvironmentGate::Impl::sActiveMutex;
std::weak_ptr<NodeEnvironmentGate::State> NodeEnvironmentGate::Impl::sActiveState;

NodeEnvironmentGate::NodeEnvironmentGate(Callbacks callbacks) : mState(createState(std::move(callbacks))) {}

NodeEnvironmentGate::~NodeEnvironmentGate() {
    disarm();
}

void NodeEnvironmentGate::arm() {
    if (!mImpl) {
        mImpl = std::make_unique<Impl>(mState);
    }
    mImpl->arm();
}

void NodeEnvironmentGate::disarm() {
    if (mImpl) {
        mImpl->disarm();
    }
    removeCleanupHook();
}

} // namespace CloverNT::Core::Platform
