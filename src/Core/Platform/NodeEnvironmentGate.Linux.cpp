#include <CloverNT/API/Logger.hpp>
#include <CloverNT/API/Memory/Hook.hpp>
#include <CloverNT/API/Plugin/PluginManager.hpp>
#include <CloverNT/Core/Platform/NodeEnvironmentGate.hpp>

#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

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
        installHook();
    }

    void disarm() {
        clearActive();
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

        constexpr auto execFn = static_cast<ExecSig*>(&node::LoadEnvironment);
        constexpr auto strFn  = static_cast<StrSig*>(&node::LoadEnvironment);
        bool           any    = false;
        const auto     owner  = Plugin::PluginManager::getPlugin("CloverCore");
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
        mExecHook.reset(); // InlineHandle's destructor removes the hook.
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

    std::shared_ptr<State> mState;

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
