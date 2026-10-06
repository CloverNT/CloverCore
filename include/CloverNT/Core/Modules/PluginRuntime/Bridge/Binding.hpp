#pragma once
#include <CloverNT/API/Plugin/PluginLoader.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Support.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/JsRuntimeState.hpp>

#include <string>
#include <string_view>

namespace CloverNT::Core::Modules::bridge {

class JsPluginLoader final : public Plugin::PluginLoader {
public:
    JsPluginLoader(JsRuntime::State* state, std::string id) : mState(state), mId(std::move(id)) {}

    [[nodiscard]] auto id() const -> std::string_view override {
        return mId;
    }
    [[nodiscard]] auto load(std::string_view name) -> Expected<void> override {
        return mState->loaderLifecycle(mId, JsRuntime::State::LifecycleOp::Load, name);
    }
    [[nodiscard]] auto unload(std::string_view name) -> Expected<void> override {
        return mState->loaderLifecycle(mId, JsRuntime::State::LifecycleOp::Unload, name);
    }
    [[nodiscard]] auto reload(std::string_view name) -> Expected<void> override {
        return mState->loaderLifecycle(mId, JsRuntime::State::LifecycleOp::Reload, name);
    }

private:
    JsRuntime::State* mState;
    std::string       mId;
};

class Logger {
public:
    static void               factory(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void               sink(const v8::FunctionCallbackInfo<v8::Value>& args);
    [[nodiscard]] static auto create(JsRuntime::State& state, v8::Local<v8::Context> context, const std::string& scope)
            -> v8::Local<v8::Value>;
};

class PluginManager {
public:
    [[nodiscard]] static v8::Local<v8::Value> instance(JsRuntime::State& state, v8::Local<v8::Context> context);
};

class LinkedBinding {
public:
    static void registerBinding(v8::Local<v8::Object>  exports,
                                v8::Local<v8::Value>   module,
                                v8::Local<v8::Context> context,
                                void*                  priv);

private:
    static void runEventLoopOnce(const v8::FunctionCallbackInfo<v8::Value>& args);
};

class Deasync {
public:
    [[nodiscard]] static auto create(JsRuntime::State& state, v8::Local<v8::Context> context) -> v8::Local<v8::Object>;

private:
    static void loopWhile(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void await(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void awaitOnOk(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void awaitOnErr(const v8::FunctionCallbackInfo<v8::Value>& args);
};

class Clovernt {
public:
    static void               install(JsRuntime::State& state);
    [[nodiscard]] static auto create(JsRuntime::State& state, v8::Local<v8::Context> context) -> v8::Local<v8::Object>;

private:
    static void load(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void unload(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void reload(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void rescan(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void registerLoader(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void unregisterLoader(const v8::FunctionCallbackInfo<v8::Value>& args);
};

void pluginsGetter(v8::Local<v8::Name> property, const v8::PropertyCallbackInfo<v8::Value>& info);

} // namespace CloverNT::Core::Modules::bridge
