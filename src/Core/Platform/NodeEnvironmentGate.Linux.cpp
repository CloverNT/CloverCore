#include <CloverNT/API/Logger.hpp>
#include <CloverNT/API/Memory/Hook.hpp>
#include <CloverNT/API/Plugin/PluginManager.hpp>
#include <CloverNT/Core/Platform/NodeEnvironmentGate.hpp>

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
    explicit Impl(NodeEnvironmentGate& gate) : mGate(gate) {
        sActive = this;
    }

    ~Impl() {
        if (sActive == this) {
            sActive = nullptr;
        }
    }

    Impl(Impl const&)            = delete;
    Impl& operator=(Impl const&) = delete;

    void arm() {
        installHook();
    }

    void disarm() {
        removeHooks();
    }

private:
    void installHook() {
        std::lock_guard lock(mHookMutex);
        if (mHookInstalled) {
            return;
        }

        const auto execFn = static_cast<ExecSig*>(&node::LoadEnvironment);
        const auto strFn  = static_cast<StrSig*>(&node::LoadEnvironment);
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
        mExecHook.reset(); // InlineHandle's destructor removes the hook.
        mStrHook.reset();
        mHookInstalled = false;
    }

    static auto wrapPreload(node::EmbedderPreloadCallback preload) -> node::EmbedderPreloadCallback {
        return [preload = std::move(preload)](
                       node::Environment* e, const v8::Local<v8::Value> process, const v8::Local<v8::Value> require) {
            if (sActive != nullptr) {
                sActive->mGate.onPreload(e, process, require);
            }
            if (preload) {
                preload(e, process, require);
            }
        };
    }

    static auto detourLoadEnvExec(const Memory::Hook::OriginalFunction<ExecSig>& original,
                                  node::Environment*                             env,
                                  node::StartExecutionCallback                   cb,
                                  node::EmbedderPreloadCallback                  preload) -> v8::MaybeLocal<v8::Value> {
        return original(env, std::move(cb), wrapPreload(std::move(preload)));
    }

    static auto detourLoadEnvStr(const Memory::Hook::OriginalFunction<StrSig>& original,
                                 node::Environment*                            env,
                                 const std::string_view                        source,
                                 node::EmbedderPreloadCallback                 preload) -> v8::MaybeLocal<v8::Value> {
        return original(env, source, wrapPreload(std::move(preload)));
    }

    NodeEnvironmentGate& mGate;

    std::mutex                                         mHookMutex;
    std::optional<Memory::Hook::InlineHandle<ExecSig>> mExecHook;
    std::optional<Memory::Hook::InlineHandle<StrSig>>  mStrHook;
    bool                                               mHookInstalled{};

    static Impl* sActive;
};

NodeEnvironmentGate::Impl* NodeEnvironmentGate::Impl::sActive = nullptr;

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
