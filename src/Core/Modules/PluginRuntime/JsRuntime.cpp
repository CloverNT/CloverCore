#include <CloverNT/API/Plugin/JsPlugin.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/AlkaBindings.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Binding.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Electron.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Events.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Lifecycle.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/Bridge/Support.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/JsRuntimeState.hpp>
#include <CloverNT/Core/Platform/NodeEnvironment.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <Alka/Install.hpp>

namespace CloverNT::Core::Modules {

JsRuntime::State::State(Callbacks callbacks) : mCallbacks(std::move(callbacks)) {}

JsRuntime::State::~State() {
    detach();
}

bool JsRuntime::State::isAttached() const {
    std::lock_guard lock(mMutex);
    return mIsolate != nullptr;
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

    if (!mAsyncInited) {
        if (auto* loop = node::GetCurrentEventLoop(isolate); loop != nullptr) {
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
    }

    if (!mEventChannel) {
        if (auto* loop = node::GetCurrentEventLoop(isolate); loop != nullptr) {
            auto channel        = std::make_shared<bridge::JsEventChannel>();
            channel->isolate    = isolate;
            channel->mainThread = mMainThread;
            channel->state      = this;
            auto* handle        = new uv_async_t{};
            handle->data        = this;
            if (uv_async_init(loop, handle, &State::onEventAsync) == 0) {
                channel->async = handle;
                std::lock_guard lock(mMutex);
                mEventAsync       = handle;
                mEventAsyncInited = true;
                mEventChannel     = std::move(channel);
            } else {
                delete handle;
                logger().error("JsRuntime::attach: uv_async_init failed; C++->JS event delivery is disabled");
            }
        }
    }

    bridge::ensureAlkaClasses(isolate);

    node::AddLinkedBinding(env, "clovernt", &bridge::LinkedBinding::registerBinding, this);

    bridge::Clovernt::install(*this);
    bridge::ElectronRuntime::installDetection(*this);
}

void JsRuntime::State::detach() {
    {
        std::lock_guard lock(mMutex);
        if (!mAttached && mIsolate == nullptr) {
            return; // already detached / never attached
        }
        mDetaching = true;
    }

    if (const uv_thread_t self = uv_thread_self(); uv_thread_equal(&self, &mMainThread) == 0) {
        logger().error("JsRuntime::detach called off the JS main thread; skipping unsafe teardown");
        drainLifecycleQueue(jsError("JS runtime detached", CommonErrorCode::Inactive));
        return;
    }

    drainLifecycleQueue(jsError("JS runtime detached", CommonErrorCode::Inactive));

    {
        std::lock_guard lock(mMutex);
        if (mAsyncInited && mLifecycleAsync != nullptr) {
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
    std::lock_guard lock(mMutex);

    if (mEventChannel) {
        uv_async_t* eventHandle = nullptr;
        {
            const std::lock_guard channelLock(mEventChannel->mutex);
            mEventChannel->alive.store(false, std::memory_order_release);
            eventHandle          = mEventChannel->async;
            mEventChannel->async = nullptr;
        }
        clearEventSubscriptions();
        if (eventHandle != nullptr) {
            uv_close(reinterpret_cast<uv_handle_t*>(eventHandle),
                     [](uv_handle_t* handle) { delete reinterpret_cast<uv_async_t*>(handle); });
        }
        mEventAsync       = nullptr;
        mEventAsyncInited = false;
        mEventChannel.reset();
    }

    if (mIsolate != nullptr) {
        Alka::disposeIsolate(mIsolate);
    }
    bridge::clearBindings(*this);
    mEnv     = nullptr;
    mIsolate = nullptr;
    mContext.Reset();
    mRequire.Reset();
    for (auto& [load, unload, reload]: mLoaders | std::views::values) {
        load.Reset();
        unload.Reset();
        reload.Reset();
    }
    mLoaders.clear();

    for (auto& binding: mBindings) {
        if (binding) {
            binding->alive.store(false);
            binding->record = nullptr;
        }
    }
    mBindings.clear();
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
    v8::HandleScope    handleScope(isolate);
    const auto         context = mContext.Get(isolate);
    v8::Context::Scope contextScope(context);
    const v8::TryCatch tryCatch(isolate);

    v8::Local<v8::String> nameValue;
    if (!v8::String::NewFromUtf8(isolate, name.data(), v8::NewStringType::kNormal, static_cast<int>(name.size()))
                 .ToLocal(&nameValue)) {
        return jsError("failed to build plugin name argument");
    }
    v8::Local<v8::Value> args[] = {nameValue};
    v8::Local<v8::Value> resultValue;
    if (!handler.Get(isolate)->Call(context, context->Global(), 1, args).ToLocal(&resultValue)) {
        return jsError("lifecycle handler threw: " + describeException(isolate, context, tryCatch),
                       CommonErrorCode::CallbackFailed);
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
        message = toUtf8(isolate, errorValue);
    }
    return jsError(message.empty() ? "lifecycle handler reported failure" : message, CommonErrorCode::CallbackFailed);
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
    {
        std::lock_guard lock(mMutex);
        if (mIsolate == nullptr || !mAsyncInited || mDetaching) {
            return jsError("JS runtime is not attached", CommonErrorCode::Inactive);
        }
    }
    const uv_thread_t self = uv_thread_self();
    if (uv_thread_equal(&self, &mMainThread) != 0) {
        return dispatchLifecycle(op, name);
    }

    PendingOp pending{op, std::string(name), {}};
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
    const auto      context = isolate->GetCurrentContext();
    LoaderCallbacks callbacks;
    const auto      store = [&](const char* key, v8::Global<v8::Function>& slot) {
        v8::Local<v8::Value> value;
        if (config->Get(context, v8::String::NewFromUtf8(isolate, key).ToLocalChecked()).ToLocal(&value) &&
            value->IsFunction()) {
            slot.Reset(isolate, value.As<v8::Function>());
        }
    };
    store("load", callbacks.load);
    store("unload", callbacks.unload);
    store("reload", callbacks.reload);

    std::lock_guard lock(mMutex);
    mLoaders[std::move(id)] = std::move(callbacks);
}

void JsRuntime::State::unregisterLoaderCallbacks(const std::string_view id) {
    std::lock_guard lock(mMutex);
    if (const auto it = mLoaders.find(id); it != mLoaders.end()) {
        it->second.load.Reset();
        it->second.unload.Reset();
        it->second.reload.Reset();
        mLoaders.erase(it);
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
    if (const uv_thread_t self = uv_thread_self(); uv_thread_equal(&self, &mMainThread) == 0) {
        return jsError("loader lifecycle must run on the JS main thread", CommonErrorCode::Inactive);
    }
    const LoaderCallbacks* cb = nullptr;
    {
        std::lock_guard lock(mMutex);
        const auto      it = mLoaders.find(id);
        if (it == mLoaders.end()) {
            return jsError("loader not registered: " + std::string(id), CommonErrorCode::NotFound);
        }
        cb = &it->second;
    }
    const v8::Global<v8::Function>& handler = op == LifecycleOp::Load     ? cb->load
                                              : op == LifecycleOp::Unload ? cb->unload
                                                                          : cb->reload;
    return callHandler(handler, name);
}

void JsRuntime::State::runEventLoopOnce() const {
    v8::Isolate* isolate = nullptr;
    {
        std::lock_guard lock(mMutex);
        isolate = mIsolate;
    }
    if (isolate == nullptr) {
        return;
    }
    if (const uv_thread_t self = uv_thread_self(); uv_thread_equal(&self, &mMainThread) == 0) {
        return;
    }
    if (auto* loop = node::GetCurrentEventLoop(isolate); loop != nullptr) {
        uv_run(loop, UV_RUN_ONCE);
    }
}

auto JsRuntime::State::requireModule(const v8::Local<v8::Context> context, const std::string& path) const
        -> v8::MaybeLocal<v8::Value> {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
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
    // userRequire(entry) -> plugin exports (may throw; caller's TryCatch observes it)
    v8::Local<v8::Value> entryArgs[] = {pathValue};
    return userRequireValue.As<v8::Function>()->Call(context, context->Global(), 1, entryArgs);
}

v8::Local<v8::Value> JsRuntime::State::buildPluginInfoArray(const v8::Local<v8::Context> context) const {
    v8::Isolate* isolate = v8::Isolate::GetCurrent();
    const auto   array   = v8::Array::New(isolate);

    std::vector<std::shared_ptr<Plugin::JsPlugin>> snapshot;
    {
        std::lock_guard lock(mMutex);
        snapshot = mQueue;
    }
    std::uint32_t index = 0;
    for (auto const& plugin: snapshot) {
        if (!plugin) {
            continue;
        }
        const auto entry = v8::Object::New(isolate);
        setProp(context, entry, "name", v8Str(isolate, plugin->name()));
        if (plugin->mainEntry()) {
            setProp(context, entry, "main", v8Str(isolate, plugin->mainEntry()->string()));
        }
        if (plugin->preloadEntry()) {
            setProp(context, entry, "preload", v8Str(isolate, plugin->preloadEntry()->string()));
        }
        if (plugin->rendererEntry()) {
            setProp(context, entry, "renderer", v8Str(isolate, plugin->rendererEntry()->string()));
        }
        (void) array->Set(context, index++, entry);
    }
    return array;
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
    const auto         context = mContext.Get(isolate);
    const v8::TryCatch tryCatch(isolate);
    const auto         webContentsVal = getProp(context, mElectron.Get(isolate), "webContents");
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
