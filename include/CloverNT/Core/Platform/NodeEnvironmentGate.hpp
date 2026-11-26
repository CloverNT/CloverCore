#pragma once
#include <CloverNT/Core/Platform/NodeEnvironment.hpp>

#include <functional>
#include <memory>

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

    struct State;

private:
    static auto createState(Callbacks callbacks) -> std::shared_ptr<State>;
    static void onPreloadState(const std::shared_ptr<State>& state,
                               node::Environment*            env,
                               v8::Local<v8::Value>          process,
                               v8::Local<v8::Value>          require);
    static void onCleanup(void* arg);
    void        removeCleanupHook();

    std::shared_ptr<State> mState;

    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace CloverNT::Core::Platform
