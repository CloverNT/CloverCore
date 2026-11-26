#pragma once
#include <CloverNT/API/Events/Subscription.hpp>
#include <CloverNT/API/Logger.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/JsRuntime.hpp>

#include <Alka/Runtime/Dispatcher.hpp>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <QQNT/v8-cppgc.h>

#include <QQNT/node.h>
#include <QQNT/uv.h>

namespace CloverNT::Plugin {
class JsPlugin;
} // namespace CloverNT::Plugin

namespace CloverNT::Core::Modules {

struct LoggerHandle {
    std::shared_ptr<Logger> logger;
};

struct JsRuntime::State {
    enum class LifecycleOp { Load, Unload, Reload };

    struct SelfCell {
        std::atomic<State*> state{};
    };

    explicit State(Callbacks callbacks);
    ~State();

    State(State const&)            = delete;
    State& operator=(State const&) = delete;

    [[nodiscard]] auto selfCell() const noexcept -> SelfCell* {
        return mSelfCell;
    }

    [[nodiscard]] bool isAttached() const;
    [[nodiscard]] bool isOnMainThread() const noexcept;
    void               attach(node::Environment*     env,
                              v8::Isolate*           isolate,
                              v8::Local<v8::Context> context,
                              v8::Local<v8::Value>   require,
                              std::filesystem::path  coreDir);
    void               detach();

    [[nodiscard]] auto enqueue(std::shared_ptr<Plugin::JsPlugin> plugin) -> Expected<void>;
    void               forget(std::string_view name);
    [[nodiscard]] auto loadPlugin(std::string_view name) -> Expected<void>;
    [[nodiscard]] auto unloadPlugin(std::string_view name) -> Expected<void>;
    [[nodiscard]] auto reloadPlugin(std::string_view name) -> Expected<void>;

    void               detachState();
    void               abandonHandlesOffThread() noexcept;
    [[nodiscard]] auto callHandler(const v8::Global<v8::Function>& handler, std::string_view name) const
            -> Expected<void>;
    [[nodiscard]] auto dispatchLifecycle(LifecycleOp op, std::string_view name) -> Expected<void>;
    [[nodiscard]] auto runLifecycle(LifecycleOp op, std::string_view name) -> Expected<void>;
    static void        onLifecycleAsync(uv_async_t* handle);
    void               drainLifecycleQueue(const Expected<void>& reason);

    static void onAlkaAsync(uv_async_t* handle);
    void        syncAlkaLoopRef() noexcept;
    void        trackJsSubscription(CloverNT::Event::SubscriptionId id);
    void        untrackJsSubscription(CloverNT::Event::SubscriptionId id) noexcept;
    void        clearAllJsSubscriptions() noexcept;

    void               registerLoaderCallbacks(std::string id, v8::Local<v8::Object> config);
    void               unregisterLoaderCallbacks(std::string_view id);
    [[nodiscard]] auto loaderLifecycle(std::string_view id, LifecycleOp op, std::string_view name) const
            -> Expected<void>;

    [[nodiscard]] int  runEventLoopOnce() const;
    [[nodiscard]] auto requireModule(v8::Local<v8::Context> context, const std::string& path) const
            -> v8::MaybeLocal<v8::Value>;
    static auto        logger() -> Logger&;
    [[nodiscard]] auto loggerHandle(const std::string& scope) -> LoggerHandle&;
    [[nodiscard]] auto resolveLogger(const std::string& scope) -> std::shared_ptr<Logger>;
    [[nodiscard]] auto resolveRendererLogger(const std::string& scope) -> std::shared_ptr<Logger>;

    [[nodiscard]] auto buildPluginInfoArray(v8::Local<v8::Context> context) const -> v8::Local<v8::Value>;
    void               tickCallback();
    static void        registerPreloadOn(v8::Local<v8::Context> context,
                                         v8::Local<v8::Object>  session,
                                         const std::string&     id,
                                         const std::string&     filePath);
    static void
         unregisterPreloadOn(v8::Local<v8::Context> context, v8::Local<v8::Object> session, const std::string& id);
    void reloadAllWindows() const;

    SelfCell*                                      mSelfCell{};
    Callbacks                                      mCallbacks;
    mutable std::recursive_mutex                   mMutex;
    node::Environment*                             mEnv{};
    v8::Isolate*                                   mIsolate{};
    v8::Global<v8::Context>                        mContext;
    v8::Global<v8::Function>                       mRequire;
    std::filesystem::path                          mCoreDir;
    std::vector<std::shared_ptr<Plugin::JsPlugin>> mQueue;
    bool                                           mAttached{};
    bool                                           mDetaching{};
    mutable bool                                   mShutdownHookRegistered{};
    bool                                           mShutdownUnloadDone{};

    v8::Global<v8::Object>              mElectron;
    v8::Global<v8::Object>              mApp;
    v8::Global<v8::Object>              mIpcMain;
    v8::Global<v8::Object>              mModuleBuiltin; // require('module')
    v8::Global<v8::Object>              mModuleCache;   // Module._cache
    v8::Global<v8::Function>            mOriginalModuleLoad;
    v8::Global<v8::Function>            mPatchedModuleLoad;
    bool                                mModuleLoadPatched{};
    v8::Global<v8::Function>            mRendererLogListener;
    v8::Global<v8::Function>            mSessionCreatedListener;
    v8::Global<v8::Function>            mAppQuitListener;
    bool                                mRendererEntriesHandled{};
    v8::Global<v8::Function>            mProcessTickCallback;
    bool                                mElectronReady{};
    std::vector<v8::Global<v8::Object>> mSessions;

    struct WebRequestHeadersEntry {
        std::uint64_t            id{};
        v8::Global<v8::Function> listener;
    };
    struct WebRequestHeadersMux {
        SelfCell*                           stateCell{};
        v8::Global<v8::Object>              webRequest;
        std::vector<WebRequestHeadersEntry> entries;
    };
    std::uint64_t                                      mNextHeadersHandlerId{1};
    std::vector<std::unique_ptr<WebRequestHeadersMux>> mHeadersMuxes;
    std::uint64_t                                      mNextTimerToken{1};

    struct Disposer {
        enum class Kind { RemoveListener, RemoveHandler, RemoveHeadersReceived, ClearTimer, CallFn };
        Kind                     kind{};
        v8::Global<v8::Object>   target;   // app / ipcMain / session.webRequest
        std::string              key;      // event name or channel
        v8::Global<v8::Function> listener; // RemoveListener
        v8::Global<v8::Value>    timerId;  // ClearTimer
        std::uint64_t            timerToken{};
        std::uint64_t            headersHandlerId{};
        bool                     interval{};
        v8::Global<v8::Function> fn; // CallFn (onUnload / returned dispose)
    };
    struct JsPluginRecord {
        std::string                          name;
        std::optional<std::filesystem::path> mainPath;
        std::optional<std::filesystem::path> preloadPath;
        std::optional<std::filesystem::path> rendererPath;
        std::string                          preloadId; // "clovernt:<name>"
        std::vector<std::string>             cacheKeys;
        std::vector<Disposer>                disposers;    // run LIFO on unload
        v8::Global<v8::Function>             onUnload;     // object-form {onLoad,onUnload}
        v8::Global<v8::Value>                onUnloadThis; // receiver for onUnload
    };
    std::map<std::string, std::unique_ptr<JsPluginRecord>, std::less<>> mRecords;

    struct PluginBinding {
        std::atomic<State*>          state{};
        std::atomic<JsPluginRecord*> record{};
        std::atomic<bool>            alive{true};
    };
    [[nodiscard]] auto          allocatePluginBinding(JsPluginRecord* record) -> PluginBinding*;
    std::vector<PluginBinding*> mBindings;

    struct LoaderCallbacks {
        v8::Global<v8::Function> load;
        v8::Global<v8::Function> unload;
        v8::Global<v8::Function> reload;
    };
    std::map<std::string, std::shared_ptr<LoaderCallbacks>, std::less<>> mLoaders;

    uv_thread_t mMainThread{};
    uv_async_t* mLifecycleAsync{};
    bool        mAsyncInited{};

    struct PendingOp {
        LifecycleOp                  kind;
        std::string                  name;
        std::promise<Expected<void>> promise;
    };
    std::mutex             mLifecycleQueueMutex;
    std::vector<PendingOp> mLifecycleQueue;

    uv_async_t*                                         mAlkaAsync{};
    bool                                                mAlkaAsyncInited{};
    Alka::Dispatcher                                    mAlkaDispatcher;
    bool                                                mAlkaReferenced{};
    std::mutex                                          mJsSubMutex;
    std::unordered_set<CloverNT::Event::SubscriptionId> mJsEventSubIds;

    std::mutex                                    mLoggerMutex;
    std::unordered_map<std::string, LoggerHandle> mLoggers;
    std::unordered_set<std::string>               mRendererScopes;
};

} // namespace CloverNT::Core::Modules
