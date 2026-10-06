#include <CloverNT/API/Events/DllEvent.hpp>
#include <CloverNT/API/Events/EventBus.hpp>
#include <CloverNT/API/Events/Listener.hpp>
#include <CloverNT/API/Exception.hpp>
#include <CloverNT/API/Logger.hpp>
#include <CloverNT/API/Memory/Hook.hpp>
#include <CloverNT/API/Plugin/PluginManager.hpp>
#include <CloverNT/Core/Platform/DelayLoadCompat.hpp>
#include <CloverNT/Core/Platform/NodeEnvironmentGate.hpp>

#include <atomic>
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
        failFastOnMissingDelayImports(moduleName); // logs all missing + exits if any remain
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
    explicit Impl(NodeEnvironmentGate& gate) : mGate(gate) {
        sActive.store(this, std::memory_order_release);
    }

    ~Impl() {
        auto expected = this;
        sActive.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    }

    Impl(Impl const&)            = delete;
    Impl& operator=(Impl const&) = delete;

    void arm() {
        if (!mDllListener) {
            auto listener = Event::EventBus::getInstance().emplaceListener<Event::DllLoadEvent>(
                    [this](const Event::DllLoadEvent& event) {
                        if (event.baseName() == "QQNT.dll") {
                            installHook();
                        }
                    },
                    Event::EventPriority::High,
                    Plugin::PluginManager::getPlugin("CloverCore"));
            if (listener) {
                mDllListener = listener.value();
            } else {
                logger().error("NodeEnvironmentGate: failed to subscribe to DllLoadEvent: {}",
                               listener.error().message);
            }
        }
        if (isModuleLoaded(L"QQNT.dll")) {
            installHook();
        }
    }

    void disarm() {
        if (mDllListener) {
            (void) Event::EventBus::removeListener(mDllListener);
            mDllListener.reset();
        }
        removeHooks();
    }

private:
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
                if (const Impl* const active = sActive.load(std::memory_order_acquire); active != nullptr) {
                    active->mGate.onPreload(e, process, require);
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
                                            node::EmbedderPreloadCallback preload) -> v8::MaybeLocal<v8::Value> {
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
                                           node::EmbedderPreloadCallback preload) -> v8::MaybeLocal<v8::Value> {
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

    NodeEnvironmentGate& mGate;
    Event::ListenerPtr   mDllListener;

    std::mutex                                         mHookMutex;
    std::optional<Memory::Hook::InlineHandle<ExecSig>> mExecHook;
    std::optional<Memory::Hook::InlineHandle<StrSig>>  mStrHook;
    bool                                               mHookInstalled{};

    static std::atomic<Impl*> sActive;
};

std::atomic<NodeEnvironmentGate::Impl*> NodeEnvironmentGate::Impl::sActive{nullptr};

NodeEnvironmentGate::NodeEnvironmentGate(Callbacks callbacks) : mCallbacks(std::move(callbacks)) {}

NodeEnvironmentGate::~NodeEnvironmentGate() {
    disarm();
}

void NodeEnvironmentGate::arm() {
    if (!mImpl) {
        mImpl = std::make_unique<Impl>(*this);
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
