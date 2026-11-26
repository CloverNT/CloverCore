#include <CloverNT/API/Events/EventBus.hpp>
#include <CloverNT/API/Plugin/JsPlugin.hpp>
#include <CloverNT/API/Plugin/PluginManager.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/AlkaBindings.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Binding.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Electron.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Lifecycle.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/ReflectBridge.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Support.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/JsRuntimeState.hpp>
#include <CloverNT/Core/Platform/NodeEnvironment.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Alka/Convert/Records.hpp>
#include <Alka/Runtime/Install.hpp>

namespace CloverNT::Core::Modules {

namespace {

    auto allocateImmortalSelfCell(JsRuntime::State* owner) -> JsRuntime::State::SelfCell* {
        static std::mutex                             poolMutex;
        static std::deque<JsRuntime::State::SelfCell> pool;
        std::lock_guard                               lock(poolMutex);
        auto&                                         cell = pool.emplace_back();
        cell.state.store(owner, std::memory_order_release);
        return &cell;
    }

    auto allocateImmortalPluginBinding(JsRuntime::State* owner, JsRuntime::State::JsPluginRecord* record)
            -> JsRuntime::State::PluginBinding* {
        static std::mutex                                  poolMutex;
        static std::deque<JsRuntime::State::PluginBinding> pool;
        std::lock_guard                                    lock(poolMutex);
        auto&                                              binding = pool.emplace_back();
        binding.state.store(owner, std::memory_order_release);
        binding.record.store(record, std::memory_order_release);
        binding.alive.store(true, std::memory_order_release);
        return &binding;
    }

    struct AbandonedV8Handles {
        v8::Global<v8::Context>                                                                context;
        v8::Global<v8::Function>                                                               require;
        v8::Global<v8::Object>                                                                 electron;
        v8::Global<v8::Object>                                                                 app;
        v8::Global<v8::Object>                                                                 ipcMain;
        v8::Global<v8::Object>                                                                 moduleBuiltin;
        v8::Global<v8::Object>                                                                 moduleCache;
        v8::Global<v8::Function>                                                               originalModuleLoad;
        v8::Global<v8::Function>                                                               patchedModuleLoad;
        v8::Global<v8::Function>                                                               rendererLogListener;
        v8::Global<v8::Function>                                                               sessionCreatedListener;
        v8::Global<v8::Function>                                                               appQuitListener;
        v8::Global<v8::Function>                                                               processTickCallback;
        std::vector<v8::Global<v8::Object>>                                                    sessions;
        std::vector<std::unique_ptr<JsRuntime::State::WebRequestHeadersMux>>                   headersMuxes;
        std::map<std::string, std::unique_ptr<JsRuntime::State::JsPluginRecord>, std::less<>>  records;
        std::map<std::string, std::shared_ptr<JsRuntime::State::LoaderCallbacks>, std::less<>> loaders;
        Alka::Dispatcher                                                                       alkaDispatcher;
    };

} // namespace

JsRuntime::State::State(Callbacks callbacks)
    : mSelfCell(allocateImmortalSelfCell(this)), mCallbacks(std::move(callbacks)) {}

JsRuntime::State::~State() {
    detach();
    if (mSelfCell != nullptr) {
        mSelfCell->state.store(nullptr, std::memory_order_release);
    }
    bool needAbandon = false;
    {
        std::lock_guard lock(mMutex);
        needAbandon = mIsolate != nullptr && !isOnMainThread();
    }
    if (needAbandon) {
        abandonHandlesOffThread();
    }
}

auto JsRuntime::State::allocatePluginBinding(JsPluginRecord* record) -> PluginBinding* {
    auto*           binding = allocateImmortalPluginBinding(this, record);
    std::lock_guard lock(mMutex);
    mBindings.push_back(binding);
    return binding;
}

bool JsRuntime::State::isAttached() const {
    std::lock_guard lock(mMutex);
    return mIsolate != nullptr && !mDetaching;
}

bool JsRuntime::State::isOnMainThread() const noexcept {
    const uv_thread_t self = uv_thread_self();
    return uv_thread_equal(&self, &mMainThread) != 0;
}

void JsRuntime::State::attach(node::Environment*           env,
                              v8::Isolate*                 isolate,
                              const v8::Local<v8::Context> context,
                              const v8::Local<v8::Value>   require,
                              std::filesystem::path        coreDir) {
    if (isolate == nullptr) {
        return;
    }
    {
        std::lock_guard lock(mMutex);
        if (mAttached) {
            return;
        }
        if (mSelfCell != nullptr) {
            mSelfCell->state.store(this, std::memory_order_release);
        }
        mDetaching = false;
        mEnv       = env;
        mIsolate   = isolate;
        mContext.Reset(isolate, context);
        if (!require.IsEmpty() && require->IsFunction()) {
            mRequire.Reset(isolate, require.As<v8::Function>());
        }
        mCoreDir    = std::move(coreDir);
        mMainThread = uv_thread_self();
        mAttached   = true;
    }

    if (auto* loop = node::GetCurrentEventLoop(isolate); loop != nullptr) {
        if (!mAsyncInited) {
            auto* handle = new uv_async_t{};
            handle->data = this;
            if (uv_async_init(loop, handle, &State::onLifecycleAsync) == 0) {
                std::lock_guard lock(mMutex);
                mLifecycleAsync = handle;
                mAsyncInited    = true;
            } else {
                delete handle;
                logger().error("JsRuntime::attach: uv_async_init failed; lifecycle dispatch is disabled");
            }
        }

        if (!mAlkaAsyncInited) {
            auto* alkaHandle = new uv_async_t{};
            alkaHandle->data = this;
            if (uv_async_init(loop, alkaHandle, &State::onAlkaAsync) == 0) {
                uv_unref(reinterpret_cast<uv_handle_t*>(alkaHandle));
                {
                    std::lock_guard lock(mMutex);
                    mAlkaAsync       = alkaHandle;
                    mAlkaAsyncInited = true;
                    mAlkaReferenced  = false;
                    mAlkaDispatcher  = Alka::Dispatcher(isolate);
                }
                auto* const selfCell = mSelfCell;
                mAlkaDispatcher.setWakeup([alkaHandle]() noexcept { (void) uv_async_send(alkaHandle); });
                mAlkaDispatcher.setActivityHook([selfCell, alkaHandle]() noexcept {
                    auto* live = selfCell != nullptr ? selfCell->state.load(std::memory_order_acquire) : nullptr;
                    if (live == nullptr) {
                        return;
                    }
                    if (live->isOnMainThread()) {
                        live->syncAlkaLoopRef();
                    } else {
                        (void) uv_async_send(alkaHandle);
                    }
                });
            } else {
                delete alkaHandle;
                logger().error("JsRuntime::attach: uv_async_init failed for AlkaLoopBridge");
            }
        }
    }

    bridge::ensureAlkaClasses(isolate);

    node::AddLinkedBinding(env, "clovernt", &bridge::LinkedBinding::registerBinding, mSelfCell);

    bridge::Clovernt::install(*this);
    bridge::ElectronRuntime::installDetection(*this);
}

void JsRuntime::State::onAlkaAsync(uv_async_t* handle) {
    auto* self = static_cast<State*>(handle != nullptr ? handle->data : nullptr);
    if (self == nullptr) {
        return;
    }
    v8::Isolate*     isolate = nullptr;
    Alka::Dispatcher dispatcher;
    {
        std::lock_guard lock(self->mMutex);
        if (self->mIsolate == nullptr || self->mDetaching || self->mContext.IsEmpty()) {
            return;
        }
        isolate    = self->mIsolate;
        dispatcher = self->mAlkaDispatcher;
    }
    const v8::Isolate::Scope isolateScope(isolate);
    const v8::HandleScope    handleScope(isolate);
    const auto               context = self->mContext.Get(isolate);
    const v8::Context::Scope contextScope(context);

    (void) dispatcher.drain();
    self->tickCallback();
    self->syncAlkaLoopRef();
}

void JsRuntime::State::syncAlkaLoopRef() noexcept {
    std::lock_guard lock(mMutex);
    if (!mAlkaAsyncInited || mAlkaAsync == nullptr || mDetaching) {
        return;
    }
    const bool wantRef = mAlkaDispatcher.busy();
    if (wantRef == mAlkaReferenced) {
        return;
    }
    mAlkaReferenced = wantRef;
    auto* raw       = reinterpret_cast<uv_handle_t*>(mAlkaAsync);
    if (wantRef) {
        uv_ref(raw);
    } else {
        uv_unref(raw);
    }
}

void JsRuntime::State::trackJsSubscription(const CloverNT::Event::SubscriptionId id) {
    if (id == CloverNT::Event::kInvalidSubscriptionId) {
        return;
    }
    std::lock_guard lock(mJsSubMutex);
    mJsEventSubIds.insert(id);
}

void JsRuntime::State::untrackJsSubscription(const CloverNT::Event::SubscriptionId id) noexcept {
    std::lock_guard lock(mJsSubMutex);
    mJsEventSubIds.erase(id);
}

void JsRuntime::State::clearAllJsSubscriptions() noexcept {
    std::vector<CloverNT::Event::SubscriptionId> ids;
    {
        std::lock_guard lock(mJsSubMutex);
        ids.assign(mJsEventSubIds.begin(), mJsEventSubIds.end());
        mJsEventSubIds.clear();
    }
    for (const auto id: ids) {
        (void) CloverNT::Event::EventBus::off(id);
    }
}

void JsRuntime::State::abandonHandlesOffThread() noexcept {
    std::vector<std::string> loaderIds;
    std::vector<std::string> pluginNames;
    {
        std::lock_guard lock(mMutex);
        if (mAlkaDispatcher) {
            mAlkaDispatcher.setActivityHook({});
            mAlkaDispatcher.setWakeup({});
        }
        if (mLifecycleAsync != nullptr) {
            mLifecycleAsync->data = nullptr;
            mLifecycleAsync       = nullptr;
            mAsyncInited          = false;
        }
        if (mAlkaAsync != nullptr) {
            mAlkaAsync->data = nullptr;
            mAlkaAsync       = nullptr;
            mAlkaAsyncInited = false;
            mAlkaReferenced  = false;
        }
        for (auto* binding: mBindings) {
            if (binding != nullptr) {
                binding->alive.store(false, std::memory_order_release);
                binding->record.store(nullptr, std::memory_order_release);
                binding->state.store(nullptr, std::memory_order_release);
            }
        }
        mBindings.clear();
        for (const auto& name: mRecords | std::views::keys) {
            pluginNames.push_back(name);
        }
        for (const auto& id: mLoaders | std::views::keys) {
            loaderIds.push_back(id);
        }

        auto* leaked                   = new AbandonedV8Handles{};
        leaked->context                = std::move(mContext);
        leaked->require                = std::move(mRequire);
        leaked->electron               = std::move(mElectron);
        leaked->app                    = std::move(mApp);
        leaked->ipcMain                = std::move(mIpcMain);
        leaked->moduleBuiltin          = std::move(mModuleBuiltin);
        leaked->moduleCache            = std::move(mModuleCache);
        leaked->originalModuleLoad     = std::move(mOriginalModuleLoad);
        leaked->patchedModuleLoad      = std::move(mPatchedModuleLoad);
        leaked->rendererLogListener    = std::move(mRendererLogListener);
        leaked->sessionCreatedListener = std::move(mSessionCreatedListener);
        leaked->appQuitListener        = std::move(mAppQuitListener);
        leaked->processTickCallback    = std::move(mProcessTickCallback);
        leaked->sessions               = std::move(mSessions);
        leaked->headersMuxes           = std::move(mHeadersMuxes);
        leaked->records                = std::move(mRecords);
        leaked->loaders                = std::move(mLoaders);
        leaked->alkaDispatcher         = std::move(mAlkaDispatcher);

        mEnv      = nullptr;
        mIsolate  = nullptr;
        mAttached = false;
    }

    clearAllJsSubscriptions();
    for (const auto& name: pluginNames) {
        (void) CloverNT::Event::EventBus::clearPlugin(name);
    }
    for (const auto& id: loaderIds) {
        (void) Plugin::PluginManager::unregisterLoader(id);
    }
    bridge::clearBindings(*this);
}

void JsRuntime::State::detach() {
    {
        std::lock_guard lock(mMutex);
        if (!mAttached && mIsolate == nullptr) {
            return; // already detached / never attached
        }
        mDetaching = true;
    }

    if (!isOnMainThread()) {
        if (mSelfCell != nullptr) {
            mSelfCell->state.store(nullptr, std::memory_order_release);
        }
        {
            std::lock_guard lock(mMutex);
            if (mAlkaDispatcher) {
                mAlkaDispatcher.setActivityHook({});
                mAlkaDispatcher.setWakeup({});
            }
            if (mLifecycleAsync != nullptr) {
                mLifecycleAsync->data = nullptr;
            }
            if (mAlkaAsync != nullptr) {
                mAlkaAsync->data = nullptr;
            }
            for (auto* binding: mBindings) {
                if (binding != nullptr) {
                    binding->alive.store(false, std::memory_order_release);
                    binding->record.store(nullptr, std::memory_order_release);
                    binding->state.store(nullptr, std::memory_order_release);
                }
            }
        }
        clearAllJsSubscriptions();
        logger().error(
                "JsRuntime::detach called off the JS main thread; neutralized callbacks and deferred V8 teardown");
        drainLifecycleQueue(jsError("JS runtime detached", CommonErrorCode::Inactive));
        return;
    }

    drainLifecycleQueue(jsError("JS runtime detached", CommonErrorCode::Inactive));

    {
        std::lock_guard lock(mMutex);
        if (mAsyncInited && mLifecycleAsync != nullptr) {
            mLifecycleAsync->data = nullptr;
            uv_close(reinterpret_cast<uv_handle_t*>(mLifecycleAsync),
                     [](uv_handle_t* handle) { delete reinterpret_cast<uv_async_t*>(handle); });
            mLifecycleAsync = nullptr;
            mAsyncInited    = false;
        }
    }
    detachState();
}

auto JsRuntime::State::enqueue(std::shared_ptr<Plugin::JsPlugin> plugin) -> Expected<void> {
    if (!plugin) {
        return jsError("JS plugin is null", CommonErrorCode::InvalidArgument);
    }
    std::lock_guard lock(mMutex);
    if (mDetaching) {
        return jsError("JS runtime is detaching", CommonErrorCode::Inactive);
    }
    for (auto& existing: mQueue) {
        if (existing && existing->name() == plugin->name()) {
            existing = std::move(plugin);
            return {};
        }
    }
    mQueue.push_back(std::move(plugin));
    return {};
}

void JsRuntime::State::forget(const std::string_view name) {
    std::lock_guard lock(mMutex);
    std::erase_if(mQueue, [&](auto const& plugin) { return plugin && plugin->name() == name; });
}

auto JsRuntime::State::loadPlugin(const std::string_view name) -> Expected<void> {
    return runLifecycle(LifecycleOp::Load, name);
}
auto JsRuntime::State::unloadPlugin(const std::string_view name) -> Expected<void> {
    return runLifecycle(LifecycleOp::Unload, name);
}
auto JsRuntime::State::reloadPlugin(const std::string_view name) -> Expected<void> {
    return runLifecycle(LifecycleOp::Reload, name);
}

void JsRuntime::State::detachState() {
    std::vector<std::string> remainingPlugins;
    std::vector<std::string> loaderIds;
    {
        std::lock_guard lock(mMutex);
        for (const auto& name: mRecords | std::views::keys) {
            remainingPlugins.push_back(name);
        }
        for (const auto& id: mLoaders | std::views::keys) {
            loaderIds.push_back(id);
        }
    }

    for (const auto& name: std::views::reverse(remainingPlugins)) {
        (void) bridge::PluginLifecycle::unload(*this, name);
    }
    for (const auto& id: loaderIds) {
        (void) Plugin::PluginManager::unregisterLoader(id);
    }

    std::lock_guard lock(mMutex);

    if (mSelfCell != nullptr) {
        mSelfCell->state.store(nullptr, std::memory_order_release);
    }

    if (mAlkaDispatcher) {
        mAlkaDispatcher.setActivityHook({});
        mAlkaDispatcher.setWakeup({});
    }
    clearAllJsSubscriptions();

    if (mIsolate != nullptr && !mContext.IsEmpty()) {
        const v8::Isolate::Scope isolateScope(mIsolate);
        const v8::HandleScope    handleScope(mIsolate);
        const auto               context = mContext.Get(mIsolate);
        const v8::Context::Scope contextScope(context);
        const v8::TryCatch       tryCatch(mIsolate);

        if (mModuleLoadPatched && !mModuleBuiltin.IsEmpty() && !mOriginalModuleLoad.IsEmpty()) {
            const auto mod     = mModuleBuiltin.Get(mIsolate);
            const auto curLoad = getProp(context, mod, "_load");
            if (!curLoad.IsEmpty() && !mPatchedModuleLoad.IsEmpty() &&
                curLoad->StrictEquals(mPatchedModuleLoad.Get(mIsolate))) {
                (void) mod->Set(context, v8Str(mIsolate, "_load"), mOriginalModuleLoad.Get(mIsolate));
            }
        }

        if (!bridge::CloverContext::isShuttingDown()) {
            if (!mIpcMain.IsEmpty()) {
                const auto ipc = mIpcMain.Get(mIsolate);
                if (!mRendererLogListener.IsEmpty()) {
                    invokeMethod(context,
                                 ipc,
                                 "removeListener",
                                 {v8Str(mIsolate, "clovernt:log"), mRendererLogListener.Get(mIsolate)});
                }
                if (mRendererEntriesHandled) {
                    invokeMethod(context, ipc, "removeHandler", {v8Str(mIsolate, "clovernt:renderer-entries")});
                }
            }
            if (!mApp.IsEmpty()) {
                const auto app = mApp.Get(mIsolate);
                if (!mSessionCreatedListener.IsEmpty()) {
                    invokeMethod(context,
                                 app,
                                 "removeListener",
                                 {v8Str(mIsolate, "session-created"), mSessionCreatedListener.Get(mIsolate)});
                }
                if (!mAppQuitListener.IsEmpty()) {
                    invokeMethod(
                            context, app, "removeListener", {v8Str(mIsolate, "quit"), mAppQuitListener.Get(mIsolate)});
                }
            }
            for (auto& session: mSessions) {
                if (!session.IsEmpty()) {
                    unregisterPreloadOn(context, session.Get(mIsolate), "clovernt-framework");
                }
            }
            for (auto& mux: mHeadersMuxes) {
                if (mux && !mux->webRequest.IsEmpty()) {
                    invokeMethod(context, mux->webRequest.Get(mIsolate), "onHeadersReceived", {v8::Null(mIsolate)});
                }
            }
        }

        (void) context->Global()->Delete(context, v8Str(mIsolate, "clovernt"));
    }

    if (mIsolate != nullptr) {
        bridge::disposeReflectIsolate(mIsolate);
        Alka::disposeIsolate(mIsolate);
    }
    mAlkaDispatcher = Alka::Dispatcher{};

    if (mAlkaAsyncInited && mAlkaAsync != nullptr) {
        mAlkaAsync->data = nullptr;
        uv_close(reinterpret_cast<uv_handle_t*>(mAlkaAsync),
                 [](uv_handle_t* handle) { delete reinterpret_cast<uv_async_t*>(handle); });
        mAlkaAsync       = nullptr;
        mAlkaAsyncInited = false;
        mAlkaReferenced  = false;
    }

    bridge::clearBindings(*this);
    mEnv     = nullptr;
    mIsolate = nullptr;
    mContext.Reset();
    mRequire.Reset();
    for (auto& cb: mLoaders | std::views::values) {
        if (cb) {
            cb->load.Reset();
            cb->unload.Reset();
            cb->reload.Reset();
        }
    }
    mLoaders.clear();

    for (auto* binding: mBindings) {
        if (binding != nullptr) {
            binding->alive.store(false, std::memory_order_release);
            binding->record.store(nullptr, std::memory_order_release);
            binding->state.store(nullptr, std::memory_order_release);
        }
    }
    mBindings.clear();
    mHeadersMuxes.clear();
    mRecords.clear();
    for (auto& session: mSessions) {
        session.Reset();
    }
    mSessions.clear();
    mElectron.Reset();
    mApp.Reset();
    mIpcMain.Reset();
    mModuleBuiltin.Reset();
    mModuleCache.Reset();
    mOriginalModuleLoad.Reset();
    mPatchedModuleLoad.Reset();
    mModuleLoadPatched = false;
    mRendererLogListener.Reset();
    mSessionCreatedListener.Reset();
    mAppQuitListener.Reset();
    mRendererEntriesHandled = false;
    mProcessTickCallback.Reset();
    mElectronReady = false;

    mShutdownHookRegistered = false;
    mShutdownUnloadDone     = false;

    mAttached = false;
}

auto JsRuntime::State::callHandler(const v8::Global<v8::Function>& handler, const std::string_view name) const
        -> Expected<void> {
    v8::Isolate* isolate = mIsolate;
    if (isolate == nullptr || handler.IsEmpty()) {
        return jsError("JS lifecycle handler is not available", CommonErrorCode::Inactive);
    }
    const v8::Isolate::Scope isolateScope(isolate);
    const v8::HandleScope    handleScope(isolate);
    const auto               context = mContext.Get(isolate);
    const v8::Context::Scope contextScope(context);
    const v8::TryCatch       tryCatch(isolate);

    v8::Local<v8::String> nameValue;
    if (!v8::String::NewFromUtf8(isolate, name.data(), v8::NewStringType::kNormal, static_cast<int>(name.size()))
                 .ToLocal(&nameValue)) {
        return jsError("failed to build plugin name argument");
    }
    v8::Local<v8::Value> args[] = {nameValue};
    v8::Local<v8::Value> resultValue;
    if (!handler.Get(isolate)->Call(context, context->Global(), 1, args).ToLocal(&resultValue)) {
        if (tryCatch.HasCaught()) {
            logger().error(
                    "Plugin '{}' lifecycle handler threw: {}", name, describeException(isolate, context, tryCatch));
        }
        return jsError(extractExceptionMessage(isolate, context, tryCatch), CommonErrorCode::CallbackFailed);
    }
    if (!resultValue->IsObject()) {
        return {};
    }
    const auto           resultObject = resultValue.As<v8::Object>();
    v8::Local<v8::Value> okValue;
    const bool ok = resultObject->Get(context, v8::String::NewFromUtf8Literal(isolate, "ok")).ToLocal(&okValue) &&
                    okValue->BooleanValue(isolate);
    if (ok) {
        return {};
    }
    std::string          message;
    v8::Local<v8::Value> errorValue;
    if (resultObject->Get(context, v8::String::NewFromUtf8Literal(isolate, "error")).ToLocal(&errorValue) &&
        !errorValue->IsUndefined() && !errorValue->IsNull()) {
        message = extractValueErrorMessage(isolate, context, errorValue);
    }
    return jsError(message.empty() ? "lifecycle handler failed" : message, CommonErrorCode::CallbackFailed);
}

auto JsRuntime::State::dispatchLifecycle(const LifecycleOp op, const std::string_view name) -> Expected<void> {
    switch (op) {
    case LifecycleOp::Load: {
        std::shared_ptr<Plugin::JsPlugin> plugin;
        {
            std::lock_guard lock(mMutex);
            for (auto const& queued: mQueue) {
                if (queued && queued->name() == name) {
                    plugin = queued;
                    break;
                }
            }
        }
        if (!plugin) {
            return jsError("JS plugin is not queued: " + std::string(name), CommonErrorCode::NotFound);
        }
        return bridge::PluginLifecycle::load(*this, *plugin);
    }
    case LifecycleOp::Unload:
        return bridge::PluginLifecycle::unload(*this, name);
    case LifecycleOp::Reload:
        return bridge::PluginLifecycle::reload(*this, name);
    }
    return jsError("unknown lifecycle operation");
}

auto JsRuntime::State::runLifecycle(const LifecycleOp op, const std::string_view name) -> Expected<void> {
    v8::Isolate* isolate = nullptr;
    {
        std::lock_guard lock(mMutex);
        if (mIsolate == nullptr || !mAsyncInited || mDetaching) {
            return jsError("JS runtime is not attached", CommonErrorCode::Inactive);
        }
        isolate = mIsolate;
    }
    if (isOnMainThread()) {
        const v8::Isolate::Scope isolateScope(isolate);
        return dispatchLifecycle(op, name);
    }

    PendingOp pending{.kind = op, .name = std::string(name), .promise = {}};
    auto      future = pending.promise.get_future();
    {
        std::lock_guard lock(mMutex);
        if (mIsolate == nullptr || !mAsyncInited || mDetaching) {
            return jsError("JS runtime detached", CommonErrorCode::Inactive);
        }
        {
            std::lock_guard queueLock(mLifecycleQueueMutex);
            mLifecycleQueue.push_back(std::move(pending));
        }
        uv_async_send(mLifecycleAsync);
    }
    return future.get();
}

void JsRuntime::State::onLifecycleAsync(uv_async_t* handle) {
    auto* self = static_cast<State*>(handle->data);
    if (self == nullptr) {
        return;
    }
    std::vector<PendingOp> ops;
    {
        std::lock_guard lock(self->mLifecycleQueueMutex);
        ops.swap(self->mLifecycleQueue);
    }

    v8::Isolate* isolate = self->mIsolate;
    if (isolate == nullptr) {
        for (auto& op: ops) {
            op.promise.set_value(jsError("JS runtime detached", CommonErrorCode::Inactive));
        }
        return;
    }

    v8::Isolate::Scope isolateScope(isolate);
    v8::HandleScope    handleScope(isolate);

    for (auto& [kind, name, promise]: ops) {
        Expected<void> outcome;
        try {
            outcome = self->dispatchLifecycle(kind, name);
        } catch (const std::exception& exception) {
            logger().error("JsRuntime lifecycle dispatch faulted: {}", exception.what());
            outcome = jsError(std::string("lifecycle dispatch faulted: ") + exception.what(),
                              CommonErrorCode::OperationFailed);
        } catch (...) {
            logger().error("JsRuntime lifecycle dispatch faulted with an unknown exception");
            outcome = jsError("lifecycle dispatch faulted", CommonErrorCode::OperationFailed);
        }
        promise.set_value(std::move(outcome));
    }
}

void JsRuntime::State::drainLifecycleQueue(const Expected<void>& reason) {
    std::vector<PendingOp> ops;
    {
        std::lock_guard lock(mLifecycleQueueMutex);
        ops.swap(mLifecycleQueue);
    }
    for (auto& op: ops) {
        op.promise.set_value(reason);
    }
}

void JsRuntime::State::registerLoaderCallbacks(std::string id, const v8::Local<v8::Object> config) {
    v8::Isolate* isolate = mIsolate;
    if (isolate == nullptr) {
        return;
    }
    const auto context   = isolate->GetCurrentContext();
    auto       callbacks = std::make_shared<LoaderCallbacks>();
    const auto store     = [&](const char* key, v8::Global<v8::Function>& slot) {
        v8::Local<v8::Value> value;
        if (config->Get(context, v8::String::NewFromUtf8(isolate, key).ToLocalChecked()).ToLocal(&value) &&
            value->IsFunction()) {
            slot.Reset(isolate, value.As<v8::Function>());
        }
    };
    store("load", callbacks->load);
    store("unload", callbacks->unload);
    store("reload", callbacks->reload);

    std::lock_guard lock(mMutex);
    mLoaders[std::move(id)] = std::move(callbacks);
}

void JsRuntime::State::unregisterLoaderCallbacks(const std::string_view id) {
    {
        std::lock_guard lock(mMutex);
        if (const auto it = mLoaders.find(id); it != mLoaders.end()) {
            std::shared_ptr<LoaderCallbacks> removed = std::move(it->second);
            mLoaders.erase(it);
        }
    }
}

auto JsRuntime::State::loaderLifecycle(const std::string_view id,
                                       const LifecycleOp      op,
                                       const std::string_view name) const -> Expected<void> {
    {
        std::lock_guard lock(mMutex);
        if (mIsolate == nullptr) {
            return jsError("JS runtime is not attached", CommonErrorCode::Inactive);
        }
    }
    if (!isOnMainThread()) {
        return jsError("loader lifecycle must run on the JS main thread", CommonErrorCode::Inactive);
    }
    std::shared_ptr<LoaderCallbacks> cb;
    {
        std::lock_guard lock(mMutex);
        const auto      it = mLoaders.find(id);
        if (it == mLoaders.end() || !it->second) {
            return jsError("loader not registered: " + std::string(id), CommonErrorCode::NotFound);
        }
        cb = it->second;
    }
    const v8::Global<v8::Function>& handler = op == LifecycleOp::Load     ? cb->load
                                              : op == LifecycleOp::Unload ? cb->unload
                                                                          : cb->reload;
    return callHandler(handler, name);
}

int JsRuntime::State::runEventLoopOnce() const {
    v8::Isolate* isolate = nullptr;
    {
        std::lock_guard lock(mMutex);
        isolate = mIsolate;
    }
    if (isolate == nullptr || !isOnMainThread()) {
        return 0;
    }
    if (auto* loop = node::GetCurrentEventLoop(isolate); loop != nullptr) {
        return uv_run(loop, UV_RUN_ONCE);
    }
    return 0;
}

auto JsRuntime::State::requireModule(const v8::Local<v8::Context> context, const std::string& path) const
        -> v8::MaybeLocal<v8::Value> {
    v8::Isolate* isolate = mIsolate != nullptr ? mIsolate : v8::Isolate::GetCurrent();
    if (mRequire.IsEmpty()) {
        return {};
    }
    const auto            require = mRequire.Get(isolate);
    v8::Local<v8::String> pathValue;
    if (!v8::String::NewFromUtf8(isolate, path.c_str()).ToLocal(&pathValue)) {
        return {};
    }

    // require('module')
    v8::Local<v8::Value> moduleArgs[] = {v8::String::NewFromUtf8Literal(isolate, "module")};
    v8::Local<v8::Value> moduleValue;
    if (!require->Call(context, context->Global(), 1, moduleArgs).ToLocal(&moduleValue) || !moduleValue->IsObject()) {
        return {};
    }
    // module.createRequire
    v8::Local<v8::Value> createRequireValue;
    if (!moduleValue.As<v8::Object>()
                 ->Get(context, v8::String::NewFromUtf8Literal(isolate, "createRequire"))
                 .ToLocal(&createRequireValue) ||
        !createRequireValue->IsFunction()) {
        return {};
    }
    // const userRequire = module.createRequire(entry)
    v8::Local<v8::Value> createRequireArgs[] = {pathValue};
    v8::Local<v8::Value> userRequireValue;
    if (!createRequireValue.As<v8::Function>()
                 ->Call(context, moduleValue, 1, createRequireArgs)
                 .ToLocal(&userRequireValue) ||
        !userRequireValue->IsFunction()) {
        return {};
    }
    // userRequire(entry) -> plugin exports (may throw)
    v8::Local<v8::Value> entryArgs[] = {pathValue};
    return userRequireValue.As<v8::Function>()->Call(context, context->Global(), 1, entryArgs);
}

v8::Local<v8::Value> JsRuntime::State::buildPluginInfoArray(const v8::Local<v8::Context> context) const {
    v8::Isolate* isolate = mIsolate != nullptr ? mIsolate : v8::Isolate::GetCurrent();

    std::vector<std::shared_ptr<Plugin::JsPlugin>> snapshot;
    {
        std::lock_guard lock(mMutex);
        snapshot = mQueue;
    }

    std::vector<bridge::PluginListDto> dtos;
    dtos.reserve(snapshot.size());
    for (auto const& plugin: snapshot) {
        if (!plugin) {
            continue;
        }
        bridge::PluginListDto dto;
        dto.name = std::string(plugin->name());
        if (plugin->mainEntry()) {
            dto.main = plugin->mainEntry()->string();
        }
        if (plugin->preloadEntry()) {
            dto.preload = plugin->preloadEntry()->string();
        }
        if (plugin->rendererEntry()) {
            dto.renderer = plugin->rendererEntry()->string();
        }
        dtos.push_back(std::move(dto));
    }

    Alka::ConvertCtx cx{isolate, context};
    return Alka::toJs(cx, dtos);
}

void JsRuntime::State::tickCallback() {
    v8::Isolate* isolate = mIsolate;
    if (isolate == nullptr) {
        return;
    }
    const auto context = mContext.Get(isolate);
    if (mProcessTickCallback.IsEmpty()) {
        if (const auto processVal = getProp(context, context->Global(), "process");
            !processVal.IsEmpty() && processVal->IsObject()) {
            if (const auto tickVal = getProp(context, processVal.As<v8::Object>(), "_tickCallback");
                !tickVal.IsEmpty() && tickVal->IsFunction()) {
                mProcessTickCallback.Reset(isolate, tickVal.As<v8::Function>());
            }
        }
    }
    if (!mProcessTickCallback.IsEmpty()) {
        const v8::TryCatch tryCatch(isolate);
        (void) mProcessTickCallback.Get(isolate)->Call(context, context->Global(), 0, nullptr);
    }
}

void JsRuntime::State::registerPreloadOn(const v8::Local<v8::Context> context,
                                         const v8::Local<v8::Object>  session,
                                         const std::string&           id,
                                         const std::string&           filePath) {
    v8::Isolate*       isolate = v8::Isolate::GetCurrent();
    const v8::TryCatch tryCatch(isolate);
    const auto         options = v8::Object::New(isolate);
    setProp(context, options, "type", v8Str(isolate, "frame"));
    setProp(context, options, "id", v8Str(isolate, id));
    setProp(context, options, "filePath", v8Str(isolate, filePath));
    invokeMethod(context, session, "registerPreloadScript", {options});
    if (tryCatch.HasCaught()) {
        logger().error("registerPreloadScript({}) failed: {}", id, describeException(isolate, context, tryCatch));
    }
}

void JsRuntime::State::unregisterPreloadOn(const v8::Local<v8::Context> context,
                                           const v8::Local<v8::Object>  session,
                                           const std::string&           id) {
    v8::Isolate*       isolate = v8::Isolate::GetCurrent();
    const v8::TryCatch tryCatch(isolate);
    invokeMethod(context, session, "unregisterPreloadScript", {v8Str(isolate, id)});
}

void JsRuntime::State::reloadAllWindows() const {
    v8::Isolate* isolate = mIsolate;
    if (isolate == nullptr || mElectron.IsEmpty()) {
        return;
    }
    const v8::Isolate::Scope isolateScope(isolate);
    const v8::HandleScope    handleScope(isolate);
    const auto               context = mContext.Get(isolate);
    const v8::Context::Scope contextScope(context);
    const v8::TryCatch       tryCatch(isolate);
    const auto               webContentsVal = getProp(context, mElectron.Get(isolate), "webContents");
    if (webContentsVal.IsEmpty() || !webContentsVal->IsObject()) {
        return;
    }
    const auto           webContents = webContentsVal.As<v8::Object>();
    const auto           getAllVal   = getProp(context, webContents, "getAllWebContents");
    v8::Local<v8::Value> allVal;
    if (getAllVal.IsEmpty() || !getAllVal->IsFunction() ||
        !getAllVal.As<v8::Function>()->Call(context, webContents, 0, nullptr).ToLocal(&allVal) || !allVal->IsArray()) {
        return;
    }
    const auto all = allVal.As<v8::Array>();
    for (std::uint32_t i = 0; i < all->Length(); ++i) {
        v8::Local<v8::Value> wcVal;
        if (!all->Get(context, i).ToLocal(&wcVal) || !wcVal->IsObject()) {
            continue;
        }
        const auto           wc         = wcVal.As<v8::Object>();
        const auto           getTypeVal = getProp(context, wc, "getType");
        v8::Local<v8::Value> typeVal;
        if (getTypeVal.IsEmpty() || !getTypeVal->IsFunction() ||
            !getTypeVal.As<v8::Function>()->Call(context, wc, 0, nullptr).ToLocal(&typeVal)) {
            continue;
        }
        if (toUtf8(isolate, typeVal) == "window") {
            invokeMethod(context, wc, "reload", {});
        }
    }
}

Logger& JsRuntime::State::logger() {
    static auto instance = LoggerRegistry::getInstance().getOrCreate("CloverCore");
    return *instance;
}

LoggerHandle& JsRuntime::State::loggerHandle(const std::string& scope) {
    std::lock_guard lock(mLoggerMutex);
    if (const auto it = mLoggers.find(scope); it != mLoggers.end()) {
        return it->second;
    }
    return mLoggers.emplace(scope, LoggerHandle{LoggerRegistry::getInstance().getOrCreate(scope)}).first->second;
}

std::shared_ptr<Logger> JsRuntime::State::resolveLogger(const std::string& scope) {
    return loggerHandle(scope).logger;
}

std::shared_ptr<Logger> JsRuntime::State::resolveRendererLogger(const std::string& scope) {
    constexpr std::size_t maxScopes = 256;
    std::string           resolved  = scope;
    {
        std::lock_guard lock(mLoggerMutex);
        if (!mRendererScopes.contains(scope)) {
            if (mRendererScopes.size() < maxScopes) {
                mRendererScopes.insert(scope);
            } else {
                resolved = "renderer";
            }
        }
    }
    return resolveLogger(resolved);
}

JsRuntime::JsRuntime(Callbacks callbacks) : mState(std::make_unique<State>(std::move(callbacks))) {}

JsRuntime::~JsRuntime() = default;

void JsRuntime::attach(const Platform::EnvironmentContext& environment) const {
    mState->attach(environment.env, environment.isolate, environment.context, environment.require, environment.coreDir);
}

void JsRuntime::detach() const {
    mState->detach();
}

auto JsRuntime::enqueue(std::shared_ptr<Plugin::JsPlugin> plugin) const -> Expected<void> {
    return mState->enqueue(std::move(plugin));
}

void JsRuntime::forget(const std::string_view name) const {
    mState->forget(name);
}

auto JsRuntime::loadPlugin(const std::string_view name) const -> Expected<void> {
    return mState->loadPlugin(name);
}

auto JsRuntime::unloadPlugin(const std::string_view name) const -> Expected<void> {
    return mState->unloadPlugin(name);
}

auto JsRuntime::reloadPlugin(const std::string_view name) const -> Expected<void> {
    return mState->reloadPlugin(name);
}

} // namespace CloverNT::Core::Modules
