#pragma once
#include <CloverNT/Core/Platform/NodeEnvironment.hpp>

#include <functional>
#include <memory>
#include <mutex>

namespace CloverNT::Core::Platform {

class NodeEnvironmentGate {
public:
    struct Callbacks {
        std::function<void(const EnvironmentContext&)> onEnvironmentReady;
        std::function<void()>                          onEnvironmentDestroyed;
    };

    explicit NodeEnvironmentGate(Callbacks callbacks);
    ~NodeEnvironmentGate();

    NodeEnvironmentGate(NodeEnvironmentGate const&)            = delete;
    NodeEnvironmentGate& operator=(NodeEnvironmentGate const&) = delete;

    void arm();
    void disarm();

    void onPreload(node::Environment* env, v8::Local<v8::Value> process, v8::Local<v8::Value> require);

private:
    static void onCleanup(void* arg);
    void        removeCleanupHook();

    Callbacks            mCallbacks;
    std::recursive_mutex mMutex;
    v8::Isolate*         mIsolate{};
    bool                 mCaptured{};
    bool                 mCleanupHookRegistered{};
    bool                 mDestroyed{};

    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace CloverNT::Core::Platform
