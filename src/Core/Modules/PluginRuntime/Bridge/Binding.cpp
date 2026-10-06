#include <CloverNT/API/Plugin/JsPlugin.hpp>
#include <CloverNT/API/Plugin/PluginManager.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/AlkaBindings.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Binding.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace CloverNT::Core::Modules::bridge {

void Logger::factory(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        if (auto* state = dataOf<JsRuntime::State>(args); state != nullptr) {
            v8::Isolate*      isolate = args.GetIsolate();
            const std::string scope   = args.Length() > 0 ? toUtf8(isolate, args[0]) : std::string{};
            args.GetReturnValue().Set(create(*state, isolate->GetCurrentContext(), scope));
        }
    });
}

void Logger::sink(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        auto* state = dataOf<JsRuntime::State>(args);
        if (state == nullptr) {
            return;
        }
        v8::Isolate* isolate = args.GetIsolate();
        const auto   context = isolate->GetCurrentContext();
        const int level = args.Length() > 0 ? args[0]->Int32Value(context).FromMaybe(static_cast<int>(LogLevel::Info))
                                            : static_cast<int>(LogLevel::Info);
        const std::string scope   = args.Length() > 1 ? toUtf8(isolate, args[1]) : std::string{};
        const std::string message = args.Length() > 2 ? toUtf8(isolate, args[2]) : std::string{};
        state->resolveLogger(scope)->log(static_cast<LogLevel>(level), message);
    });
}

auto Logger::create(JsRuntime::State& state, const v8::Local<v8::Context> context, const std::string& scope)
        -> v8::Local<v8::Value> {
    return mintLogger(context, state, scope);
}

auto PluginManager::instance(JsRuntime::State& state, const v8::Local<v8::Context> context) -> v8::Local<v8::Value> {
    return mintPluginManager(context, state);
}

void LinkedBinding::runEventLoopOnce(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        if (const auto* state = dataOf<JsRuntime::State>(args); state != nullptr) {
            state->runEventLoopOnce();
        }
    });
}

void LinkedBinding::registerBinding(const v8::Local<v8::Object>  exports,
                                    const v8::Local<v8::Value>   module,
                                    const v8::Local<v8::Context> context,
                                    void*                        priv) {
    try {
        (void) module;
        auto*           state   = static_cast<JsRuntime::State*>(priv);
        v8::Isolate*    isolate = v8::Isolate::GetCurrent();
        v8::HandleScope scope(isolate);

        setProp(context, exports, "version", v8::String::NewFromUtf8Literal(isolate, "0.0.1"));
        if (state == nullptr) {
            return;
        }

        std::string coreDir;
        {
            std::lock_guard lock(state->mMutex);
            coreDir = state->mCoreDir.string();
        }
        setProp(context, exports, "coreDir", v8Str(isolate, coreDir));

        const auto external = v8::External::New(isolate, state);
        setProp(context, exports, "log", makeFn(context, &Logger::sink, external));
        setProp(context, exports, "createLogger", makeFn(context, &Logger::factory, external));
        setProp(context, exports, "runEventLoopOnce", makeFn(context, &LinkedBinding::runEventLoopOnce, external));
        setProp(context, exports, "pluginManager", PluginManager::instance(*state, context));
    } catch (const std::exception& exception) {
        JsRuntime::State::logger().error("clovernt linked binding registration failed: {}", exception.what());
    } catch (...) {
        JsRuntime::State::logger().error("clovernt linked binding registration failed: unknown exception");
    }
}

namespace {
    struct AwaitState {
        bool                  done{};
        bool                  failed{};
        v8::Global<v8::Value> value;
        v8::Global<v8::Value> error;
    };
} // namespace

auto Deasync::create(JsRuntime::State& state, const v8::Local<v8::Context> context) -> v8::Local<v8::Object> {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    const auto   ext     = v8::External::New(isolate, &state);
    const auto   deasync = v8::Object::New(isolate);
    setProp(context, deasync, "loopWhile", makeFn(context, &Deasync::loopWhile, ext));
    setProp(context, deasync, "await", makeFn(context, &Deasync::await, ext));
    return deasync;
}

void Deasync::loopWhile(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        auto* state = dataOf<JsRuntime::State>(args);
        if (state == nullptr || args.Length() < 1 || !args[0]->IsFunction()) {
            return;
        }
        v8::Isolate* isolate = args.GetIsolate();
        const auto   context = isolate->GetCurrentContext();
        const auto   pred    = args[0].As<v8::Function>();
        const auto   test    = [&] {
            v8::Local<v8::Value> r;
            if (pred->Call(context, context->Global(), 0, nullptr).ToLocal(&r)) {
                return r->BooleanValue(isolate);
            }
            return false;
        };
        while (test()) {
            state->tickCallback();
            if (test()) {
                state->runEventLoopOnce();
            }
        }
    });
}

void Deasync::awaitOnOk(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        const auto* holder = dataOf<std::shared_ptr<AwaitState>>(args);
        if (holder == nullptr) {
            return;
        }
        const auto st = *holder; // keep the state alive for this callback
        delete holder;           // release the heap holder; a settled promise fires once
        if (!st) {
            return;
        }
        if (args.Length() > 0) {
            st->value.Reset(args.GetIsolate(), args[0]);
        }
        st->done = true;
    });
}
void Deasync::awaitOnErr(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        const auto* holder = dataOf<std::shared_ptr<AwaitState>>(args);
        if (holder == nullptr) {
            return;
        }
        const auto st = *holder;
        delete holder;
        if (!st) {
            return;
        }
        if (args.Length() > 0) {
            st->error.Reset(args.GetIsolate(), args[0]);
        }
        st->failed = true;
        st->done   = true;
    });
}

void Deasync::await(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        auto* state = dataOf<JsRuntime::State>(args);
        if (state == nullptr) {
            return;
        }
        v8::Isolate* isolate = args.GetIsolate();
        const auto   context = isolate->GetCurrentContext();

        auto                             st     = std::make_shared<AwaitState>();
        auto*                            holder = new std::shared_ptr(st);
        const auto                       ext    = v8::External::New(isolate, holder);
        const v8::Local<v8::Value>       input  = args.Length() > 0 ? args[0] : v8::Undefined(isolate).As<v8::Value>();
        v8::Local<v8::Promise::Resolver> resolver;
        if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
            delete holder;
            return;
        }
        (void) resolver->Resolve(context, input); // adopts input if it is a thenable/promise
        const auto promise = resolver->GetPromise();
        (void) promise->Then(
                context, makeFn(context, &Deasync::awaitOnOk, ext), makeFn(context, &Deasync::awaitOnErr, ext));

        while (!st->done) {
            state->tickCallback();
            if (!st->done) {
                state->runEventLoopOnce();
            }
        }
        if (st->failed) {
            isolate->ThrowException(st->error.IsEmpty() ? v8::Undefined(isolate).As<v8::Value>()
                                                        : st->error.Get(isolate));
            return;
        }
        args.GetReturnValue().Set(st->value.IsEmpty() ? v8::Undefined(isolate).As<v8::Value>()
                                                      : st->value.Get(isolate));
    });
}

void Clovernt::install(JsRuntime::State& state) {
    v8::Isolate* isolate = state.mIsolate;
    if (isolate == nullptr) {
        return;
    }
    v8::HandleScope    handleScope(isolate);
    const auto         context = state.mContext.Get(isolate);
    v8::Context::Scope contextScope(context);
    (void) context->Global()->Set(context, v8Str(isolate, "clovernt"), create(state, context));
}

auto Clovernt::create(JsRuntime::State& state, const v8::Local<v8::Context> context) -> v8::Local<v8::Object> {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    const auto   ext     = v8::External::New(isolate, &state);
    const auto   clover  = v8::Object::New(isolate);
    setProp(context, clover, "version", v8Str(isolate, "0.0.1"));
    setProp(context, clover, "coreDir", v8Str(isolate, state.mCoreDir.string()));
    setProp(context, clover, "log", makeFn(context, &Logger::factory, ext));
    (void) clover->SetNativeDataProperty(context, v8Str(isolate, "plugins"), &pluginsGetter, nullptr, ext);
    setProp(context, clover, "deasync", Deasync::create(state, context));
    setProp(context, clover, "events", mintEvents(context, state, "CloverCore"));
    setProp(context, clover, "packet", mintPacket(context, state));
    setProp(context, clover, "loadPlugin", makeFn(context, &Clovernt::load, ext));
    setProp(context, clover, "unloadPlugin", makeFn(context, &Clovernt::unload, ext));
    setProp(context, clover, "reloadPlugin", makeFn(context, &Clovernt::reload, ext));
    setProp(context, clover, "rescan", makeFn(context, &Clovernt::rescan, ext));
    setProp(context, clover, "registerLoader", makeFn(context, &Clovernt::registerLoader, ext));
    setProp(context, clover, "unregisterLoader", makeFn(context, &Clovernt::unregisterLoader, ext));
    return clover;
}

void pluginsGetter(v8::Local<v8::Name>, const v8::PropertyCallbackInfo<v8::Value>& info) {
    guardedCallback(info, [&] {
        if (!info.Data()->IsExternal()) {
            return;
        }
        const auto* state = static_cast<JsRuntime::State*>(info.Data().As<v8::External>()->Value());
        info.GetReturnValue().Set(state->buildPluginInfoArray(info.GetIsolate()->GetCurrentContext()));
    });
}

void Clovernt::load(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        if (args.Length() < 1) {
            args.GetReturnValue().Set(false);
            return;
        }
        args.GetReturnValue().Set(Plugin::PluginManager::loadPlugin(toUtf8(args.GetIsolate(), args[0])).has_value());
    });
}
void Clovernt::unload(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        if (args.Length() < 1) {
            args.GetReturnValue().Set(false);
            return;
        }
        args.GetReturnValue().Set(Plugin::PluginManager::unloadPlugin(toUtf8(args.GetIsolate(), args[0])).has_value());
    });
}
void Clovernt::reload(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        if (args.Length() < 1) {
            args.GetReturnValue().Set(false);
            return;
        }
        args.GetReturnValue().Set(Plugin::PluginManager::reloadPlugin(toUtf8(args.GetIsolate(), args[0])).has_value());
    });
}

void Clovernt::rescan(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        v8::Isolate* isolate = args.GetIsolate();
        const auto   result  = Plugin::PluginManager::rescan();
        args.GetReturnValue().Set(v8::Number::New(isolate, result ? static_cast<double>(result.value()) : 0.0));
    });
}

void Clovernt::registerLoader(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        auto* state = dataOf<JsRuntime::State>(args);
        if (state == nullptr || args.Length() < 1 || !args[0]->IsObject()) {
            args.GetReturnValue().Set(false);
            return;
        }
        v8::Isolate* isolate = args.GetIsolate();
        const auto   context = isolate->GetCurrentContext();
        const auto   config  = args[0].As<v8::Object>();
        const auto   idValue = getProp(context, config, "id");
        if (idValue.IsEmpty() || !idValue->IsString()) {
            args.GetReturnValue().Set(false);
            return;
        }
        const std::string id = toUtf8(isolate, idValue);
        state->registerLoaderCallbacks(id, config);
        if (const auto result = Plugin::PluginManager::registerLoader(std::make_shared<JsPluginLoader>(state, id));
            !result) {
            state->unregisterLoaderCallbacks(id);
            args.GetReturnValue().Set(false);
            return;
        }
        args.GetReturnValue().Set(true);
    });
}

void Clovernt::unregisterLoader(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        auto* state = dataOf<JsRuntime::State>(args);
        if (state == nullptr || args.Length() < 1 || !args[0]->IsString()) {
            args.GetReturnValue().Set(false);
            return;
        }
        const std::string id     = toUtf8(args.GetIsolate(), args[0]);
        const auto        result = Plugin::PluginManager::unregisterLoader(id);
        state->unregisterLoaderCallbacks(id);
        args.GetReturnValue().Set(result.has_value());
    });
}

} // namespace CloverNT::Core::Modules::bridge
