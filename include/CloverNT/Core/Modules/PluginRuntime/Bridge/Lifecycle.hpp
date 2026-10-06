#pragma once
#include <CloverNT/Core/Modules/PluginRuntime/JsRuntimeState.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace CloverNT::Plugin {
class JsPlugin;
} // namespace CloverNT::Plugin

namespace CloverNT::Core::Modules::bridge {

class CloverContext {
public:
    [[nodiscard]] static auto create(JsRuntime::State&                 state,
                                     v8::Local<v8::Context>            context,
                                     JsRuntime::State::JsPluginRecord& record) -> v8::Local<v8::Object>;

    static void runDisposers(v8::Local<v8::Context>            context,
                             JsRuntime::State::JsPluginRecord& record,
                             std::vector<std::string>&         errors);

    static void invalidateBindings(JsRuntime::State& state, const JsRuntime::State::JsPluginRecord* record);

    static void setShuttingDown(bool value);

private:
    [[nodiscard]] static auto liveBinding(const v8::FunctionCallbackInfo<v8::Value>& args)
            -> JsRuntime::State::PluginBinding*;
    static void pushListenerDisposer(const JsRuntime::State::PluginBinding* binding,
                                     v8::Isolate*                           isolate,
                                     v8::Local<v8::Object>                  target,
                                     const std::string&                     key,
                                     v8::Local<v8::Function>                listener);

    static void appOn(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void ipcOn(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void ipcHandle(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void onHeadersReceived(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void setTimer(const v8::FunctionCallbackInfo<v8::Value>& args, bool interval);
    static void setTimeout(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void setInterval(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void onUnload(const v8::FunctionCallbackInfo<v8::Value>& args);
};

class PluginLifecycle {
public:
    [[nodiscard]] static auto load(JsRuntime::State& state, const Plugin::JsPlugin& plugin) -> Expected<void>;
    [[nodiscard]] static auto unload(JsRuntime::State& state, std::string_view name) -> Expected<void>;
    [[nodiscard]] static auto reload(JsRuntime::State& state, std::string_view name) -> Expected<void>;

private:
    static void
    purgeCache(JsRuntime::State& state, v8::Local<v8::Context> context, const JsRuntime::State::JsPluginRecord& record);
};

} // namespace CloverNT::Core::Modules::bridge
