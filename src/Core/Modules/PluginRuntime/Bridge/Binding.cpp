#include <CloverNT/API/Nt/Packet.hpp>
#include <CloverNT/API/Plugin/JsPlugin.hpp>
#include <CloverNT/API/Plugin/PluginManager.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/AlkaBindings.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Binding.hpp>
#include <CloverNT/Runtime/Kernel.hpp>
#include <CloverNT/Runtime/Nt.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#if defined(_MSC_VER)
#define CLOVER_GUARD_NOINLINE __declspec(noinline)
#else
#define CLOVER_GUARD_NOINLINE __attribute__((noinline))
#endif

#ifndef CLOVERNT_VERSION_FULL
#define CLOVERNT_VERSION_FULL "0.0.0-unknown"
#endif

namespace CloverNT::Core::Modules::bridge {

namespace {
    constexpr std::string_view kCoreVersion{CLOVERNT_VERSION_FULL};
    constexpr std::int32_t     kGuardStatusSymbolMissing = -1;
    constexpr std::int32_t     kGuardStatusUnavailable   = 8;

    using CloverGuardSetLockedFn = std::int32_t(CloverNT_KERNEL_CALL*)(std::uint32_t families, CloverBool locked);

    struct GuardCallOutcome {
        std::int32_t                 status{kGuardStatusUnavailable};
        std::optional<std::uint32_t> lockedMask;
    };

    [[nodiscard]] auto currentGuardMask() noexcept -> std::optional<std::uint32_t> {
        if (!Nt::guardAvailable()) {
            return std::nullopt;
        }
        return Nt::lockedFamilies();
    }

    [[nodiscard]] auto resolveGuardSetLocked() noexcept -> CloverGuardSetLockedFn {
        static std::atomic<CloverGuardSetLockedFn> cached{nullptr};
        if (auto* fn = cached.load(std::memory_order_acquire); fn != nullptr) {
            return fn;
        }
        auto* fn = reinterpret_cast<CloverGuardSetLockedFn>(Nt::Detail::resolveLoaderSymbol("CloverSetLocked"));
        if (fn != nullptr) {
            cached.store(fn, std::memory_order_release);
        }
        return fn;
    }

    CLOVER_GUARD_NOINLINE auto callLoaderGuardSetLocked(const std::uint32_t families, const CloverBool locked) noexcept
            -> GuardCallOutcome {
        const auto fn = resolveGuardSetLocked();
        if (fn == nullptr) {
            return {.status = kGuardStatusSymbolMissing, .lockedMask = currentGuardMask()};
        }
        const std::int32_t status = fn(families, locked);
        return {.status = status, .lockedMask = currentGuardMask()};
    }

    [[nodiscard]] auto makeGuardSetResultDto(const std::int32_t                  status,
                                             const std::optional<std::uint32_t>& lockedMask) noexcept
            -> GuardSetResultDto {
        GuardSetResultDto dto{.status = status, .lockedMask = nullptr};
        if (lockedMask.has_value()) {
            dto.lockedMask = *lockedMask;
        }
        return dto;
    }

    std::string loaderVersionString() {
        return std::string{Runtime::Kernel::selfVersion()};
    }

    std::string qqVersionString() {
        return std::string{Runtime::Kernel::qqVersion()};
    }

    std::string pluginsDirString() {
        return std::string{Runtime::Kernel::pluginsDir()};
    }
} // namespace

void Logger::factory(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        if (auto* state = stateOf(args); state != nullptr) {
            v8::Isolate*      isolate = args.GetIsolate();
            const std::string scope   = args.Length() > 0 ? toUtf8(isolate, args[0]) : std::string{};
            args.GetReturnValue().Set(create(*state, isolate->GetCurrentContext(), scope));
        }
    });
}

void Logger::sink(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        auto* state = stateOf(args);
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
        if (const auto* state = stateOf(args); state != nullptr) {
            (void) state->runEventLoopOnce();
        }
    });
}

void LinkedBinding::getGuardLocked(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        v8::Isolate* isolate = args.GetIsolate();
        if (const auto mask = currentGuardMask(); mask.has_value()) {
            args.GetReturnValue().Set(v8::Integer::NewFromUnsigned(isolate, *mask));
        } else {
            args.GetReturnValue().SetNull();
        }
    });
}

void LinkedBinding::setGuardLocked(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args,
                    [&] {
                        v8::Isolate*     isolate = args.GetIsolate();
                        const auto       context = isolate->GetCurrentContext();
                        Alka::ConvertCtx cx{isolate, context};

                        const Alka::Promise<GuardSetResultDto> promise(cx);
                        if (!promise.valid()) {
                            return;
                        }
                        args.GetReturnValue().Set(Alka::toJs(cx, promise));

                        const auto*    state    = stateOf(args);
                        const uint32_t families = args.Length() > 0 ? args[0]->Uint32Value(context).FromMaybe(0u) : 0u;
                        const CloverBool locked = args.Length() > 1 && args[1]->BooleanValue(isolate) ? 1 : 0;

                        if (state == nullptr || !state->isAttached()) {
                            (void) promise.resolve(makeGuardSetResultDto(kGuardStatusUnavailable, currentGuardMask()));
                            return;
                        }

        auto settler = promise.settler();
        try {
            std::thread([settler = std::move(settler), families, locked]() mutable {
                const auto [status, lockedMask] = callLoaderGuardSetLocked(families, locked);
                (void) settler.resolve(makeGuardSetResultDto(status, lockedMask));
            }).detach();
        } catch (...) {
            (void) settler.resolve(makeGuardSetResultDto(kGuardStatusUnavailable, currentGuardMask()));
        }
    });
}

void LinkedBinding::registerBinding(const v8::Local<v8::Object>  exports,
                                    const v8::Local<v8::Value>   module,
                                    const v8::Local<v8::Context> context,
                                    void*                        priv) {
    try {
        (void) module;
        auto*           cell    = static_cast<JsRuntime::State::SelfCell*>(priv);
        auto*           state   = cell != nullptr ? cell->state.load(std::memory_order_acquire) : nullptr;
        v8::Isolate*    isolate = v8::Isolate::GetCurrent();
        v8::HandleScope scope(isolate);

        setProp(context, exports, "version", v8Str(isolate, kCoreVersion));
        setProp(context, exports, "loaderVersion", v8Str(isolate, loaderVersionString()));
        setProp(context, exports, "qqVersion", v8Str(isolate, qqVersionString()));
        setProp(context, exports, "pluginsDir", v8Str(isolate, pluginsDirString()));
        if (state == nullptr) {
            return;
        }

        std::string coreDir;
        {
            std::lock_guard lock(state->mMutex);
            coreDir = state->mCoreDir.string();
        }
        setProp(context, exports, "coreDir", v8Str(isolate, coreDir));

        const auto external = v8::External::New(isolate, state->selfCell());
        setProp(context, exports, "log", makeFn(context, &Logger::sink, external));
        setProp(context, exports, "createLogger", makeFn(context, &Logger::factory, external));
        setProp(context, exports, "runEventLoopOnce", makeFn(context, &LinkedBinding::runEventLoopOnce, external));
        setProp(context, exports, "getGuardLocked", makeFn(context, &LinkedBinding::getGuardLocked, external));
        setProp(context, exports, "setGuardLocked", makeFn(context, &LinkedBinding::setGuardLocked, external));
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

auto Deasync::create(const JsRuntime::State& state, const v8::Local<v8::Context> context) -> v8::Local<v8::Object> {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    const auto   ext     = v8::External::New(isolate, state.selfCell());
    const auto   deasync = v8::Object::New(isolate);
    setProp(context, deasync, "loopWhile", makeFn(context, &Deasync::loopWhile, ext));
    setProp(context, deasync, "await", makeFn(context, &Deasync::await, ext));
    return deasync;
}

void Deasync::loopWhile(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        auto* state = stateOf(args);
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
            if (!state->isAttached()) {
                isolate->ThrowException(v8::Exception::Error(v8Str(isolate, "deasync.loopWhile: JS runtime detached")));
                return;
            }
            state->tickCallback();
            if (test()) {
                if (state->runEventLoopOnce() == 0 && test()) {
                    isolate->ThrowException(v8::Exception::Error(
                            v8Str(isolate, "deasync.loopWhile: event loop has no active handles left")));
                    return;
                }
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
        const auto st = *holder;
        delete holder;
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
    guardedCallback(args,
                    [&] {
                        auto* state = stateOf(args);
                        if (state == nullptr) {
                            return;
                        }
                        v8::Isolate* isolate = args.GetIsolate();
                        const auto   context = isolate->GetCurrentContext();

                        const auto                 st     = std::make_shared<AwaitState>();
                        auto*                      holder = new std::shared_ptr(st);
                        const auto                 ext    = v8::External::New(isolate, holder);
                        const v8::Local<v8::Value> input =
                                args.Length() > 0 ? args[0] : v8::Undefined(isolate).As<v8::Value>();
                        v8::Local<v8::Promise::Resolver> resolver;
                        if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
                            delete holder;
                            return;
                        }
                        if (resolver->Resolve(context, input).IsNothing()) {
                            delete holder;
                            return;
                        }
                        if (const auto promise = resolver->GetPromise();
                            promise->Then(context,
                                          makeFn(context, &Deasync::awaitOnOk, ext),
                                          makeFn(context, &Deasync::awaitOnErr, ext))
                                    .IsEmpty()) {
                            delete holder;
                            return;
                        }

                        while (!st->done) {
                            if (!state->isAttached()) {
                                isolate->ThrowException(
                                        v8::Exception::Error(v8Str(isolate, "deasync.await: JS runtime detached")));
                                return;
                            }
                            state->tickCallback();
                            if (!st->done) {
                                if (state->runEventLoopOnce() == 0 && !st->done) {
                                    isolate->ThrowException(
                                            v8::Exception::Error(v8Str(isolate,
                                                                       "deasync.await: promise never settled and event "
                                                                       "loop has no active handles")));
                                    return;
                                }
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
    const v8::Isolate::Scope isolateScope(isolate);
    v8::HandleScope          handleScope(isolate);
    const auto               context = state.mContext.Get(isolate);
    v8::Context::Scope       contextScope(context);
    (void) context->Global()->Set(context, v8Str(isolate, "clovernt"), create(state, context));
}

auto Clovernt::create(JsRuntime::State& state, const v8::Local<v8::Context> context) -> v8::Local<v8::Object> {
    v8::Isolate* isolate = state.mIsolate != nullptr ? state.mIsolate : v8::Isolate::GetCurrent();
    const auto   ext     = v8::External::New(isolate, state.selfCell());
    const auto   clover  = v8::Object::New(isolate);
    setProp(context, clover, "version", v8Str(isolate, kCoreVersion));
    setProp(context, clover, "loaderVersion", v8Str(isolate, loaderVersionString()));
    setProp(context, clover, "qqVersion", v8Str(isolate, qqVersionString()));
    setProp(context, clover, "coreDir", v8Str(isolate, state.mCoreDir.string()));
    setProp(context, clover, "pluginsDir", v8Str(isolate, pluginsDirString()));
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
        const auto* state = stateOf(info);
        if (state == nullptr) {
            info.GetReturnValue().Set(v8::Array::New(info.GetIsolate(), 0));
            return;
        }
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
        auto* state = stateOf(args);
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
        if (const auto result =
                    Plugin::PluginManager::registerLoader(std::make_shared<JsPluginLoader>(state->selfCell(), id));
            !result) {
            args.GetReturnValue().Set(false);
            return;
        }
        state->registerLoaderCallbacks(id, config);
        args.GetReturnValue().Set(true);
    });
}

void Clovernt::unregisterLoader(const v8::FunctionCallbackInfo<v8::Value>& args) {
    guardedCallback(args, [&] {
        auto* state = stateOf(args);
        if (state == nullptr || args.Length() < 1 || !args[0]->IsString()) {
            args.GetReturnValue().Set(false);
            return;
        }
        const std::string id     = toUtf8(args.GetIsolate(), args[0]);
        const auto        result = Plugin::PluginManager::unregisterLoader(id);
        if (result) {
            state->unregisterLoaderCallbacks(id);
        }
        args.GetReturnValue().Set(result.has_value());
    });
}

} // namespace CloverNT::Core::Modules::bridge
