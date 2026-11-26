#include <CloverNT/API/Logger.hpp>
#include <CloverNT/API/Utils/System.hpp>
#include <CloverNT/Core/Platform/NodeEnvironmentGate.hpp>
#include <CloverNT/Core/Utils/System.hpp>

#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include <QQNT/uv.h>

namespace CloverNT::Core::Platform {

struct NodeEnvironmentGate::State {
    Callbacks               callbacks;
    std::recursive_mutex    mutex;
    v8::Isolate*            isolate{};
    std::thread::id         loopThreadId{};
    std::shared_ptr<State>* cleanupToken{};
    bool                    captured{};
    bool                    cleanupHookRegistered{};
    bool                    destroyed{};

    explicit State(Callbacks cb) : callbacks(std::move(cb)) {}
};

namespace {

    auto logger() -> Logger& {
        static auto instance = LoggerRegistry::getInstance().getOrCreate("CloverCore");
        return *instance;
    }

    struct Holder {
        std::mutex            mutex;
        node::Environment*    env{};
        v8::Isolate*          isolate{};
        v8::Global<v8::Value> require;
        std::filesystem::path coreDir;
        bool                  present{};
        bool                  runnable{};
        uv_thread_t           loopThread{};
    };

    Holder& holder() {
        static auto* instance = new Holder();
        return *instance;
    }

} // namespace

void NodeEnvironment::publish(const EnvironmentContext& context) {
    auto& [mutex, env, isolate, require, coreDir, present, runnable, loopThread] = holder();
    std::lock_guard lock(mutex);
    env     = context.env;
    isolate = context.isolate;
    require.Reset(context.isolate, context.require);
    coreDir    = context.coreDir;
    present    = true;
    runnable   = true;
    loopThread = uv_thread_self();
}

void NodeEnvironment::clear() {
    auto& [mutex, env, isolate, require, coreDir, present, runnable, loopThread] = holder();
    std::lock_guard lock(mutex);
    if (present && isolate != nullptr) {
        const auto self = uv_thread_self();
        if (uv_thread_equal(&self, &loopThread) != 0) {
            require.Reset();
        }
    }
    env     = nullptr;
    isolate = nullptr;
    coreDir.clear();
    present  = false;
    runnable = false;
}

bool NodeEnvironment::isPresent() {
    auto& [mutex, env, isolate, require, coreDir, present, runnable, loopThread] = holder();
    std::lock_guard lock(mutex);
    return present;
}

bool NodeEnvironment::isRunnable() {
    auto& [mutex, env, isolate, require, coreDir, present, runnable, loopThread] = holder();
    std::lock_guard lock(mutex);
    return runnable;
}

void NodeEnvironment::markStopped() {
    auto& [mutex, env, isolate, require, coreDir, present, runnable, loopThread] = holder();
    std::lock_guard lock(mutex);
    runnable = false;
}

bool NodeEnvironment::current(EnvironmentContext& out) {
    auto& [mutex, env, isolate, require, coreDir, present, runnable, loopThread] = holder();
    std::lock_guard lock(mutex);
    if (!present || isolate == nullptr) {
        return false;
    }
    if (const auto self = uv_thread_self(); uv_thread_equal(&self, &loopThread) == 0) {
        return false;
    }
    out.env     = env;
    out.isolate = isolate;
    out.context = isolate->GetCurrentContext();
    out.require = require.Get(isolate);
    out.coreDir = coreDir;
    return true;
}

auto NodeEnvironmentGate::createState(Callbacks callbacks) -> std::shared_ptr<State> {
    return std::make_shared<State>(std::move(callbacks));
}

void NodeEnvironmentGate::onPreload(node::Environment*         env,
                                    const v8::Local<v8::Value> process,
                                    const v8::Local<v8::Value> require) {
    onPreloadState(mState, env, process, require);
}

void NodeEnvironmentGate::onPreloadState(const std::shared_ptr<State>& state,
                                         node::Environment*            env,
                                         const v8::Local<v8::Value>    process,
                                         const v8::Local<v8::Value>    require) {
    try {
        (void) process;
        if (!state) {
            return;
        }
        v8::Isolate* isolate = v8::Isolate::GetCurrent();
        if (isolate == nullptr) {
            return;
        }

        std::shared_ptr<State>* token = nullptr;
        {
            std::lock_guard lock(state->mutex);
            if (state->destroyed || state->captured) {
                return;
            }
            state->captured     = true;
            state->isolate      = isolate;
            state->loopThreadId = std::this_thread::get_id();
            token               = new std::shared_ptr(state);
            state->cleanupToken = token;
        }

        node::AddEnvironmentCleanupHook(isolate, &NodeEnvironmentGate::onCleanup, token);

        std::function<void(const EnvironmentContext&)> onReady;
        {
            std::lock_guard lock(state->mutex);
            if (state->destroyed) {
                if (state->cleanupToken == token) {
                    node::RemoveEnvironmentCleanupHook(isolate, &NodeEnvironmentGate::onCleanup, token);
                    state->cleanupToken = nullptr;
                    delete token;
                }
                return;
            }
            state->cleanupHookRegistered = true;
            onReady                      = state->callbacks.onEnvironmentReady;
        }

        const EnvironmentContext context{
                .env     = env,
                .isolate = isolate,
                .context = isolate->GetCurrentContext(),
                .require = require,
                .coreDir = CloverNT::Utils::System::GetModuleDirectory(Utils::System::GetCurrentModuleHandle()),
        };
        if (onReady) {
            onReady(context);
        }
    } catch (const std::exception& e) {
        logger().error("NodeEnvironmentGate::onPreload faulted: {}", e.what());
    } catch (...) {
        logger().error("NodeEnvironmentGate::onPreload faulted with an unknown exception");
    }
}

void NodeEnvironmentGate::onCleanup(void* arg) {
    auto* token = static_cast<std::shared_ptr<State>*>(arg);
    if (token == nullptr) {
        return;
    }
    const std::shared_ptr<State> state = *token;
    delete token;

    try {
        if (!state) {
            return;
        }
        std::function<void()> destroyed;
        {
            std::lock_guard lock(state->mutex);
            state->cleanupToken          = nullptr;
            state->cleanupHookRegistered = false;
            if (state->destroyed) {
                return;
            }
            state->destroyed = true;
            destroyed        = std::move(state->callbacks.onEnvironmentDestroyed);
            state->callbacks = {};
        }
        if (destroyed) {
            destroyed();
        }
    } catch (const std::exception& e) {
        logger().error("NodeEnvironmentGate::onCleanup faulted: {}", e.what());
    } catch (...) {
        logger().error("NodeEnvironmentGate::onCleanup faulted with an unknown exception");
    }
}

void NodeEnvironmentGate::removeCleanupHook() {
    if (!mState) {
        return;
    }
    std::lock_guard lock(mState->mutex);
    mState->destroyed = true;
    mState->callbacks = {};
    if (mState->cleanupHookRegistered && mState->isolate != nullptr && mState->cleanupToken != nullptr &&
        std::this_thread::get_id() == mState->loopThreadId) {
        auto* token                   = mState->cleanupToken;
        mState->cleanupToken          = nullptr;
        mState->cleanupHookRegistered = false;
        node::RemoveEnvironmentCleanupHook(mState->isolate, &NodeEnvironmentGate::onCleanup, token);
        delete token;
    }
}

} // namespace CloverNT::Core::Platform
