#pragma once
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Support.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/JsRuntimeState.hpp>

namespace CloverNT::Core::Modules::bridge {

class ElectronRuntime {
public:
    static void installDetection(JsRuntime::State& state);

private:
    static void onModuleLoad(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void onElectronReady(JsRuntime::State& state, v8::Local<v8::Object> electron);
    static void onSessionCreated(JsRuntime::State& state, v8::Local<v8::Object> session);
    static void registerShutdownHook(JsRuntime::State& state);
    static void driveJsLoad(JsRuntime::State& state);

    static void onAppQuitEvent(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void rendererEntries(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void sessionCreated(const v8::FunctionCallbackInfo<v8::Value>& args);
    static void whenReady(const v8::FunctionCallbackInfo<v8::Value>& args);
};

} // namespace CloverNT::Core::Modules::bridge
