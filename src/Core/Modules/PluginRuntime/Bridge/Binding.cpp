#include <CloverNT/API/Nt/Packet.hpp>
#include <CloverNT/API/Plugin/JsPlugin.hpp>
#include <CloverNT/API/Plugin/PluginManager.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/AlkaBindings.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Binding.hpp>
#include <CloverNT/Runtime/Kernel.hpp>
#include <CloverNT/Runtime/Nt.h>

#include <atomic>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

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

    // Must remain a non-inlined function inside CloverCore so CloverLoader's RtlCaptureStackBackTrace /
    // backtrace identifies CloverCore as the immediate external caller frame.
    CLOVER_GUARD_NOINLINE auto callLoaderGuardSetLocked(const std::uint32_t families, const CloverBool locked) noexcept
            -> GuardCallOutcome {
        const auto fn = resolveGuardSetLocked();
        if (fn == nullptr) {
            return {.status = kGuardStatusSymbolMissing, .lockedMask = currentGuardMask()};
        }
        const std::int32_t status = fn(families, locked);
        return {.status = status, .lockedMask = currentGuardMask()};
    }

    struct GuardAsyncTask {
        std::mutex                        mutex;
        JsRuntime::State*                 state{};
        v8::Isolate*                      isolate{};
        v8::Global<v8::Context>           context;
        v8::Global<v8::Promise::Resolver> resolver;
        uv_async_t*                       async{};
        bool                              alive{true};
        std::uint32_t                     families{};
        CloverBool                        locked{};
        std::int32_t                      status{kGuardStatusUnavailable};
        std::optional<std::uint32_t>      lockedMask;
    };

    std::mutex                                      gPendingGuardMutex;
    std::vector<std::shared_ptr<GuardAsyncTask>>    gPendingGuardTasks;

    void closeGuardAsyncHandle(uv_async_t* handle) noexcept {
        if (handle == nullptr) {
            return;
        }
        uv_close(reinterpret_cast<uv_handle_t*>(handle), [](uv_handle_t* h) {
            delete static_cast<std::shared_ptr<GuardAsyncTask>*>(h->data);
            delete reinterpret_cast<uv_async_t*>(h);
        });
    }

    auto makeGuardResultObject(v8::Isolate*                        isolate,
                               const v8::Local<v8::Context>        context,
                               const std::int32_t                  status,
                               const std::optional<std::uint32_t>& lockedMask) -> v8::Local<v8::Object> {
        const auto obj = v8::Object::New(isolate);
        setProp(context, obj, "status", v8::Integer::New(isolate, status));
        if (lockedMask.has_value()) {
            setProp(context, obj, "lockedMask", v8::Integer::NewFromUnsigned(isolate, *lockedMask));
        } else {
            setProp(context, obj, "lockedMask", v8::Null(isolate));
        }
        return obj;
    }

    void onGuardAsyncDone(uv_async_t* handle) {
        auto* holder = static_cast<std::shared_ptr<GuardAsyncTask>*>(handle->data);
        if (holder == nullptr || !*holder) {
            return;
        }
        const auto task = *holder;

        v8::Isolate*                      isolate = nullptr;
        v8::Global<v8::Context>           ctxGlobal;
        v8::Global<v8::Promise::Resolver> resolverGlobal;
        std::int32_t                      status = kGuardStatusUnavailable;
        std::optional<std::uint32_t>      lockedMask;
        uv_async_t*                       toClose = nullptr;
        {
            std::lock_guard lock(task->mutex);
            if (!task->alive) {
                return;
            }
            task->alive    = false;
            toClose        = task->async;
            task->async    = nullptr;
            isolate        = task->isolate;
            status         = task->status;
            lockedMask     = task->lockedMask;
            ctxGlobal      = std::move(task->context);
            resolverGlobal = std::move(task->resolver);
        }

        {
            std::lock_guard pendingLock(gPendingGuardMutex);
            std::erase(gPendingGuardTasks, task);
        }

        if (isolate != nullptr && !ctxGlobal.IsEmpty() && !resolverGlobal.IsEmpty()) {
            const v8::HandleScope    handleScope(isolate);
            const auto               context = ctxGlobal.Get(isolate);
            const v8::Context::Scope contextScope(context);
            const auto               resultObj = makeGuardResultObject(isolate, context, status, lockedMask);
            (void) resolverGlobal.Get(isolate)->Resolve(context, resultObj);
            resolverGlobal.Reset();
            ctxGlobal.Reset();
        }

        closeGuardAsyncHandle(toClose);
    }

    std::string loaderVersionString() {
        return std::string{CloverNT::Runtime::Kernel::selfVersion()};
    }
    std::string qqVersionString() {
        return std::string{CloverNT::Runtime::Kernel::qqVersion()};
    }
    std::string pluginsDirString() {
        return std::string{CloverNT::Runtime::Kernel::pluginsDir()};
    }
} // namespace

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
    guardedCallback(args, [&] {
        v8::Isolate*                     isolate = args.GetIsolate();
        const auto                       context = isolate->GetCurrentContext();
        v8::Local<v8::Promise::Resolver> resolver;
        if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
            return;
        }
        args.GetReturnValue().Set(resolver->GetPromise());

        auto*          state    = dataOf<JsRuntime::State>(args);
        const uint32_t families = args.Length() > 0 ? args[0]->Uint32Value(context).FromMaybe(0u) : 0u;
        const CloverBool locked = args.Length() > 1 && args[1]->BooleanValue(isolate) ? 1 : 0;

        auto* loop = state != nullptr ? node::GetCurrentEventLoop(isolate) : nullptr;
        if (state == nullptr || loop == nullptr) {
            (void) resolver->Resolve(
                    context, makeGuardResultObject(isolate, context, kGuardStatusUnavailable, currentGuardMask()));
            return;
        }

        auto task      = std::make_shared<GuardAsyncTask>();
        task->state    = state;
        task->isolate  = isolate;
        task->context.Reset(isolate, context);
        task->resolver.Reset(isolate, resolver);
        task->families = families;
        task->locked   = locked;

        auto* async  = new uv_async_t{};
        async->data  = new std::shared_ptr<GuardAsyncTask>(task);
        if (uv_async_init(loop, async, &onGuardAsyncDone) != 0) {
            delete static_cast<std::shared_ptr<GuardAsyncTask>*>(async->data);
            delete async;
            task->resolver.Reset();
            task->context.Reset();
            (void) resolver->Resolve(
                    context, makeGuardResultObject(isolate, context, kGuardStatusUnavailable, currentGuardMask()));
            return;
        }
        task->async = async;

        {
            std::lock_guard pendingLock(gPendingGuardMutex);
            gPendingGuardTasks.push_back(task);
        }

        try {
            std::thread([task] {
                const auto outcome = callLoaderGuardSetLocked(task->families, task->locked);
                std::lock_guard lock(task->mutex);
                task->status     = outcome.status;
                task->lockedMask = outcome.lockedMask;
                if (task->alive && task->async != nullptr) {
                    (void) uv_async_send(task->async);
                }
            }).detach();
        } catch (...) {
            uv_async_t* toClose = nullptr;
            {
                std::lock_guard lock(task->mutex);
                task->alive = false;
                toClose     = task->async;
                task->async = nullptr;
                task->resolver.Reset();
                task->context.Reset();
            }
            {
                std::lock_guard pendingLock(gPendingGuardMutex);
                std::erase(gPendingGuardTasks, task);
            }
            closeGuardAsyncHandle(toClose);
            (void) resolver->Resolve(
                    context, makeGuardResultObject(isolate, context, kGuardStatusUnavailable, currentGuardMask()));
        }
    });
}

void LinkedBinding::cancelPendingGuardRequests(JsRuntime::State& state) {
    std::vector<std::shared_ptr<GuardAsyncTask>> toCancel;
    {
        std::lock_guard pendingLock(gPendingGuardMutex);
        for (auto it = gPendingGuardTasks.begin(); it != gPendingGuardTasks.end();) {
            if (*it && (*it)->state == &state) {
                toCancel.push_back(std::move(*it));
                it = gPendingGuardTasks.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& task: toCancel) {
        uv_async_t* toClose = nullptr;
        {
            std::lock_guard lock(task->mutex);
            if (task->alive) {
                task->alive = false;
                toClose     = task->async;
                task->async = nullptr;
                task->resolver.Reset();
                task->context.Reset();
            }
        }
        closeGuardAsyncHandle(toClose);
    }
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

        const auto external = v8::External::New(isolate, state);
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
