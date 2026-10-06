#pragma once
#include <CloverNT/API/Plugin/Manifest.hpp>
#include <CloverNT/API/Plugin/Plugin.hpp>
#include <CloverNT/API/Plugin/PluginManager.hpp>
#include <CloverNT/Core/Modules/ModuleBase.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace CloverNT::Core::Modules {

class JsRuntime;

class PluginRuntime final : public ModuleBase {
public:
    struct ManifestEntry {
        Plugin::Manifest      manifest;
        std::filesystem::path directory;
    };

    PluginRuntime();
    ~PluginRuntime() override;

    PluginRuntime(PluginRuntime const&)            = delete;
    PluginRuntime& operator=(PluginRuntime const&) = delete;

    [[nodiscard]] auto onLoad() -> Expected<void> override;
    [[nodiscard]] auto onUnload() -> Expected<void> override;
    [[nodiscard]] auto onEnable() -> Expected<void> override;
    [[nodiscard]] auto onDisable() -> Expected<void> override;

    [[nodiscard]] auto        getPlugin(std::string_view name) const -> std::shared_ptr<Plugin::Plugin>;
    [[nodiscard]] static auto currentPlugin() -> std::shared_ptr<Plugin::Plugin>;
    [[nodiscard]] auto        plugins() const -> std::vector<std::shared_ptr<Plugin::Plugin>>;
    [[nodiscard]] auto        scan() const -> std::vector<Plugin::PluginListing>;

    [[nodiscard]] auto unloadPlugin(std::string_view name) -> Expected<void>;
    [[nodiscard]] auto loadPlugin(std::string_view name) -> Expected<void>;
    [[nodiscard]] auto reloadPlugin(std::string_view name) -> Expected<void>;
    [[nodiscard]] auto unloadAll() -> Expected<void>;

    [[nodiscard]] auto rescan() -> Expected<std::size_t>;

    [[nodiscard]] auto registerLoader(std::shared_ptr<Plugin::PluginLoader> loader) -> Expected<void>;
    [[nodiscard]] auto unregisterLoader(std::string_view id) -> Expected<void>;

private:
    [[nodiscard]] auto teardownPlugin(std::shared_ptr<Plugin::Plugin> const& plugin,
                                      bool                                   recoverable,
                                      bool                                   finalShutdown) -> Expected<void>;
    [[nodiscard]] auto initializeState() -> Expected<void>;
    [[nodiscard]] auto loadConfiguredPlugins() -> Expected<void>;
    [[nodiscard]] auto unloadLoadedPlugins() -> Expected<void>;
    [[nodiscard]] auto unloadLoadedPluginsImpl(bool finalShutdown) -> Expected<void>;
    [[nodiscard]] auto clearState() -> Expected<void>;

    void onJsPluginResult(std::string_view name, bool ok, std::string_view error);
    void onJsPluginsDone();

    [[nodiscard]] auto resolveRoute(std::string_view name) const
            -> std::pair<std::shared_ptr<Plugin::PluginLoader>, std::string>;
    [[nodiscard]] auto        findLoaderLocked(std::string_view id) const -> std::shared_ptr<Plugin::PluginLoader>;
    void                      drainPendingExternal();
    [[nodiscard]] static auto makePluginError(std::string     message,
                                              CommonErrorCode code = CommonErrorCode::OperationFailed) -> Error;

    enum class LifecycleOperation {
        None,
        Loading,
        Unloading,
        ShuttingDown,
    };

    class LifecycleGuard;

    [[nodiscard]] static auto lifecycleOperationName(LifecycleOperation operation) noexcept -> std::string_view;

    class LoadReporter {
    public:
        void reset();
        void ensureHeader();
        void recordLoaded(std::string_view label);
        void recordFailed(std::string_view name, std::string_view reason);
        void finish();

    private:
        std::mutex mMutex;
        int        mLoaded{};
        bool       mHeaderPrinted{};
        bool       mSummaryPrinted{};
    };

    std::unique_ptr<JsRuntime>                                          mJs;
    mutable std::recursive_mutex                                        mMutex;
    std::filesystem::path                                               mCoreDirectory;
    std::filesystem::path                                               mPluginsDirectory;
    std::map<std::string, std::shared_ptr<Plugin::Plugin>, std::less<>> mLoadedPlugins;
    std::map<std::string, ManifestEntry, std::less<>>                   mManifests;
    std::vector<std::string>                                            mLoadOrder;
    LifecycleOperation                                                  mLifecycleOperation{LifecycleOperation::None};
    bool                                                                mInitialized{};
    bool                                                                mConfiguredPluginsLoaded{};
    bool                                                                mHasFailedUnload{};
    int                                                                 mJsPending{};
    LoadReporter                                                        mReporter;

    struct PendingExternal {
        std::string           name;
        std::string           loaderId; // == manifest.type
        std::filesystem::path directory;
    };
    std::vector<PendingExternal>                       mPendingExternal;
    std::vector<std::shared_ptr<Plugin::PluginLoader>> mLoaders;
};

} // namespace CloverNT::Core::Modules
