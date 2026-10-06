#include <CloverNT/API/Logger.hpp>
#include <CloverNT/API/Utils/System.hpp>
#include <CloverNT/Core/Platform/NodeEnvironmentGate.hpp>
#include <CloverNT/Core/Utils/System.hpp>

#include <exception>
#include <mutex>
#include <utility>

#include <QQNT/uv.h>

namespace CloverNT::Core::Platform {

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
        uv_thread_t           loopThread{}; // thread that published the env (the Node loop thread)
    };

    Holder& holder() {
        static Holder instance;
        return instance;
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
    require.Reset();
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

void NodeEnvironmentGate::onPreload(node::Environment*         env,
                                              const v8::Local<v8::Value> process,
                                              const v8::Local<v8::Value> require) {
    try {
        (void) process;
        v8::Isolate* isolate = v8::Isolate::GetCurrent();
        if (isolate == nullptr) {
            return;
        }
        {
            std::lock_guard lock(mMutex);
            if (mCaptured) {
                return;
            }
            mCaptured = true;
            mIsolate  = isolate;
        }

        node::AddEnvironmentCleanupHook(isolate, &NodeEnvironmentGate::onCleanup, this);
        {
            std::lock_guard lock(mMutex);
            mCleanupHookRegistered = true;
        }

        const EnvironmentContext context{
                .env     = env,
                .isolate = isolate,
                .context = isolate->GetCurrentContext(),
                .require = require,
                .coreDir = CloverNT::Utils::System::GetModuleDirectory(Utils::System::GetCurrentModuleHandle()),
        };
        if (mCallbacks.onEnvironmentReady) {
            mCallbacks.onEnvironmentReady(context);
        }
    } catch (const std::exception& e) {
        logger().error("NodeEnvironmentGate::onPreload faulted: {}", e.what());
    } catch (...) {
        logger().error("NodeEnvironmentGate::onPreload faulted with an unknown exception");
    }
}

void NodeEnvironmentGate::onCleanup(void* arg) {
    try {
        auto* self = static_cast<NodeEnvironmentGate*>(arg);
        if (self == nullptr) {
            return;
        }
        std::function<void()> destroyed;
        {
            std::lock_guard lock(self->mMutex);
            if (self->mDestroyed) {
                return;
            }
            self->mDestroyed             = true;
            self->mCleanupHookRegistered = false;
            destroyed                    = self->mCallbacks.onEnvironmentDestroyed;
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
    std::lock_guard lock(mMutex);
    if (mCleanupHookRegistered && mIsolate != nullptr) {
        node::RemoveEnvironmentCleanupHook(mIsolate, &NodeEnvironmentGate::onCleanup, this);
    }
    mCleanupHookRegistered = false;
}

} // namespace CloverNT::Core::Platform
