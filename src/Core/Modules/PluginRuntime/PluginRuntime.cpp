#include <CloverNT/API/Events/EventBus.hpp>
#include <CloverNT/API/Exception.hpp>
#include <CloverNT/API/Logger.hpp>
#include <CloverNT/API/Plugin/JsPlugin.hpp>
#include <CloverNT/API/Plugin/NativePlugin.hpp>
#include <CloverNT/API/Plugin/PluginLoader.hpp>
#include <CloverNT/API/Plugin/PluginManager.hpp>
#include <CloverNT/API/Utils/System.hpp>
#include <CloverNT/Core/Modules/HookRuntime.hpp>
#include <CloverNT/Core/Modules/Manager.hpp>
#include <CloverNT/Core/Modules/PluginRuntime.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/JsRuntime.hpp>
#include <CloverNT/Core/Modules/PluginRuntime/PluginResolver.hpp>
#include <CloverNT/Core/Platform/NodeEnvironment.hpp>
#include <CloverNT/Core/Utils/System.hpp>
#include <CloverNT/Runtime/Kernel.h>

#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace CloverNT {
namespace {

    using Core::Modules::Resolver::CorePluginName;

    thread_local std::weak_ptr<Plugin::Plugin> tCurrentPlugin;

    class CurrentPluginScope {
    public:
        explicit CurrentPluginScope(const std::shared_ptr<Plugin::Plugin>& plugin) : mPrevious(tCurrentPlugin) {
            tCurrentPlugin = plugin;
        }

        CurrentPluginScope(CurrentPluginScope const&)                    = delete;
        auto operator=(CurrentPluginScope const&) -> CurrentPluginScope& = delete;

        ~CurrentPluginScope() {
            tCurrentPlugin = mPrevious;
        }

    private:
        std::weak_ptr<Plugin::Plugin> mPrevious;
    };

    auto logger() -> Logger& {
        static auto instance = LoggerRegistry::getInstance().getOrCreate("CloverCore");
        return *instance;
    }

    void logInfo(std::string const& message) {
        logger().info("{}", message);
    }

    // "id:name" -> {"id","name"}; "name" -> {"", "name"}
    auto splitNamespace(std::string_view full) -> std::pair<std::string_view, std::string_view> {
        const auto pos = full.find(':');
        if (pos == std::string_view::npos) {
            return {std::string_view{}, full};
        }
        return {full.substr(0, pos), full.substr(pos + 1)};
    }

    auto categoryOf(std::string_view type) -> std::string_view {
        if (type == "native") {
            return "clover";
        }
        if (type == "js") {
            return "cloverjs";
        }
        return type; // e.g. "core"
    }

    void logError(std::string const& message) {
        logger().error("{}", message);
    }

    void appendError(std::vector<std::string>& errors, std::string message) {
        logError(message);
        errors.emplace_back(std::move(message));
    }

    auto joinErrors(std::vector<std::string> const& errors) -> std::string {
        std::string message;
        for (auto const& error: errors) {
            if (!message.empty()) {
                message += '\n';
            }
            message += error;
        }
        return message;
    }

    auto hookRuntime() -> Expected<Core::Modules::ModuleRef<Core::Modules::HookRuntime>> {
        return Core::Modules::Manager::getInstance().requireModule<Core::Modules::HookRuntime>();
    }

    std::string pluginLabel(std::string_view name, std::optional<Plugin::Version> const& version) {
        return version ? std::format("{} v{}", name, version->toString()) : std::string(name);
    }

    auto makeJsPlugin(Plugin::Manifest manifest, std::filesystem::path const& pluginDirectory)
            -> Expected<std::shared_ptr<Plugin::JsPlugin>> {
        const auto resolveEntry = [&](std::string const& relative) -> std::optional<std::filesystem::path> {
            auto resolved = (pluginDirectory / relative).lexically_normal();
            if (std::error_code entryEc; std::filesystem::path(relative).is_absolute() ||
                                         !Utils::System::IsPathInside(resolved, pluginDirectory) ||
                                         !std::filesystem::exists(resolved, entryEc) || entryEc) {
                return std::nullopt;
            }
            return resolved;
        };

        std::optional<std::filesystem::path> mainEntry;
        std::optional<std::filesystem::path> preloadEntry;
        std::optional<std::filesystem::path> rendererEntry;
        bool                                 entryError = false;
        if (!manifest.entry.empty()) {
            mainEntry  = resolveEntry(manifest.entry);
            entryError = entryError || !mainEntry;
        }
        if (manifest.preloadEntry) {
            preloadEntry = resolveEntry(*manifest.preloadEntry);
            entryError   = entryError || !preloadEntry;
        }
        if (manifest.rendererEntry) {
            rendererEntry = resolveEntry(*manifest.rendererEntry);
            entryError    = entryError || !rendererEntry;
        }
        if (entryError || (!mainEntry && !preloadEntry && !rendererEntry)) {
            return unexpected(makeError(ErrorCategory::Plugin, CommonErrorCode::NotFound, "invalid JS entry path(s)"));
        }
        return std::make_shared<Plugin::JsPlugin>(
                std::move(manifest), pluginDirectory, mainEntry, preloadEntry, rendererEntry);
    }

} // namespace
} // namespace CloverNT

namespace CloverNT::Core::Modules {

PluginRuntime::PluginRuntime()
    : mJs(std::make_unique<JsRuntime>(JsRuntime::Callbacks{
              .onLoadStarting = [this] { mReporter.ensureHeader(); },
              .onPluginResult = [this](const std::string_view name,
                                       const bool             ok,
                                       const std::string_view error) { onJsPluginResult(name, ok, error); },
              .onPluginsDone  = [this] { onJsPluginsDone(); },
      })) {}

PluginRuntime::~PluginRuntime() = default;

void PluginRuntime::LoadReporter::reset() {
    std::lock_guard lock(mMutex);
    mLoaded         = 0;
    mHeaderPrinted  = false;
    mSummaryPrinted = false;
}

void PluginRuntime::LoadReporter::ensureHeader() {
    std::lock_guard lock(mMutex);
    if (!mHeaderPrinted) {
        logger().info("Loading plugins...");
        mHeaderPrinted = true;
    }
}

void PluginRuntime::LoadReporter::recordLoaded(const std::string_view label) {
    ensureHeader();
    std::lock_guard lock(mMutex);
    logger().info("Loaded {}", label);
    ++mLoaded;
}

void PluginRuntime::LoadReporter::recordFailed(const std::string_view name, const std::string_view reason) {
    ensureHeader();
    std::lock_guard lock(mMutex);
    logger().error("Failed to load {}: {}", name, reason);
}

void PluginRuntime::LoadReporter::finish() {
    std::lock_guard lock(mMutex);
    if (!mSummaryPrinted) {
        logger().info("Loaded {} plugin(s).", mLoaded);
        mSummaryPrinted = true;
    }
}

void PluginRuntime::onJsPluginResult(const std::string_view name, const bool ok, const std::string_view error) {
    if (const auto plugin = getPlugin(name)) {
        if (ok) {
            plugin->setState(Plugin::PluginState::Loaded);
            mReporter.recordLoaded(pluginLabel(name, plugin->manifest().version));
            return;
        }
        plugin->setState(Plugin::PluginState::Failed);
        {
            std::lock_guard lock(mMutex);
            mLoadedPlugins.erase(std::string(name));
            std::erase(mLoadOrder, std::string(name));
        }
        mJs->forget(name);
    }
    if (ok) {
        mReporter.recordLoaded(std::string(name));
    } else {
        mReporter.recordFailed(name, error);
    }
}

void PluginRuntime::onJsPluginsDone() {
    {
        std::lock_guard lock(mMutex);
        mJsPending = 0;
    }
    drainPendingExternal();
    mReporter.finish();
}

auto PluginRuntime::makePluginError(std::string message, const CommonErrorCode code) -> Error {
    logError(message);
    return makeError(ErrorCategory::Plugin, code, std::move(message));
}

auto PluginRuntime::lifecycleOperationName(const LifecycleOperation operation) noexcept -> std::string_view {
    switch (operation) {
    case LifecycleOperation::None:
        return "None";
    case LifecycleOperation::Loading:
        return "Loading";
    case LifecycleOperation::Unloading:
        return "Unloading";
    case LifecycleOperation::ShuttingDown:
        return "ShuttingDown";
    }
    return "Unknown";
}

class PluginRuntime::LifecycleGuard final {
public:
    LifecycleGuard(LifecycleGuard const&)            = delete;
    LifecycleGuard& operator=(LifecycleGuard const&) = delete;

    LifecycleGuard(LifecycleGuard&& other) noexcept : mRuntime(std::exchange(other.mRuntime, nullptr)) {}

    auto operator=(LifecycleGuard&& other) noexcept -> LifecycleGuard& {
        if (this != &other) {
            release();
            mRuntime = std::exchange(other.mRuntime, nullptr);
        }
        return *this;
    }

    ~LifecycleGuard() {
        release();
    }

    [[nodiscard]] static auto acquire(PluginRuntime& runtime, const LifecycleOperation operation, std::string message)
            -> Expected<LifecycleGuard> {
        std::lock_guard lock(runtime.mMutex);
        if (runtime.mLifecycleOperation != LifecycleOperation::None) {
            message += ": ";
            message += lifecycleOperationName(runtime.mLifecycleOperation);
            return unexpected(makePluginError(std::move(message), CommonErrorCode::InvalidState));
        }
        runtime.mLifecycleOperation = operation;
        return LifecycleGuard(runtime);
    }

private:
    explicit LifecycleGuard(PluginRuntime& runtime) noexcept : mRuntime(&runtime) {}

    void release() noexcept {
        if (mRuntime == nullptr) {
            return;
        }
        std::lock_guard lock(mRuntime->mMutex);
        mRuntime->mLifecycleOperation = LifecycleOperation::None;
        mRuntime                      = nullptr;
    }

    PluginRuntime* mRuntime{};
};

auto PluginRuntime::onLoad() -> Expected<void> {
    return initializeState();
}

auto PluginRuntime::onUnload() -> Expected<void> {
    return clearState();
}

auto PluginRuntime::onEnable() -> Expected<void> {
    if (Platform::EnvironmentContext environment; Platform::NodeEnvironment::current(environment)) {
        mJs->attach(environment);
    }
    return loadConfiguredPlugins();
}

auto PluginRuntime::onDisable() -> Expected<void> {
    auto result = unloadLoadedPlugins();
    mJs->detach();
    return result;
}

auto PluginRuntime::getPlugin(const std::string_view name) const -> std::shared_ptr<Plugin::Plugin> {
    std::lock_guard lock(mMutex);
    const auto      it = mLoadedPlugins.find(name);
    return it == mLoadedPlugins.end() ? nullptr : it->second;
}

auto PluginRuntime::currentPlugin() -> std::shared_ptr<Plugin::Plugin> {
    return tCurrentPlugin.lock();
}

auto PluginRuntime::plugins() const -> std::vector<std::shared_ptr<Plugin::Plugin>> {
    std::lock_guard                              lock(mMutex);
    std::vector<std::shared_ptr<Plugin::Plugin>> result;
    result.reserve(mLoadedPlugins.size());
    for (const auto& plugin: mLoadedPlugins | std::views::values) {
        if (plugin && plugin->state() == Plugin::PluginState::Loaded) {
            result.emplace_back(plugin);
        }
    }
    return result;
}

auto PluginRuntime::registerLoader(std::shared_ptr<Plugin::PluginLoader> loader) -> Expected<void> {
    if (!loader) {
        return unexpected(makePluginError("loader is null", CommonErrorCode::InvalidArgument));
    }
    const auto id = std::string(loader->id());
    if (id.empty() || id == "clover" || id == "cloverjs" || id.find(':') != std::string::npos) {
        return unexpected(makePluginError("invalid loader id: '" + id + "'", CommonErrorCode::InvalidArgument));
    }
    {
        std::lock_guard lock(mMutex);
        for (auto const& existing: mLoaders) {
            if (existing && existing->id() == id) {
                return unexpected(makePluginError("loader already registered: " + id, CommonErrorCode::AlreadyExists));
            }
        }
        mLoaders.emplace_back(std::move(loader));
    }
    logInfo("registered plugin loader '" + id + "'");
    return {};
}

auto PluginRuntime::unregisterLoader(const std::string_view id) -> Expected<void> {
    {
        std::lock_guard lock(mMutex);
        const auto      before = mLoaders.size();
        std::erase_if(mLoaders, [&](auto const& loader) { return loader && loader->id() == id; });
        if (mLoaders.size() == before) {
            return unexpected(makePluginError("loader not registered: " + std::string(id), CommonErrorCode::NotFound));
        }
    }
    logInfo("unregistered plugin loader '" + std::string(id) + "'");
    return {};
}

auto PluginRuntime::findLoaderLocked(const std::string_view id) const -> std::shared_ptr<Plugin::PluginLoader> {
    for (auto const& loader: mLoaders) {
        if (loader && loader->id() == id) {
            return loader;
        }
    }
    return nullptr;
}

auto PluginRuntime::resolveRoute(const std::string_view name) const
        -> std::pair<std::shared_ptr<Plugin::PluginLoader>, std::string> {
    const auto [prefix, bare] = splitNamespace(name);

    std::lock_guard lock(mMutex);

    const bool prefixKnown =
            !prefix.empty() && (prefix == "clover" || prefix == "cloverjs" || findLoaderLocked(prefix) != nullptr);
    std::string pluginName = prefixKnown ? std::string(bare) : std::string(name);

    std::string category;
    if (const auto it = mManifests.find(pluginName); it != mManifests.end()) {
        category = std::string(categoryOf(it->second.manifest.type));
    } else if (const auto lit = mLoadedPlugins.find(pluginName); lit != mLoadedPlugins.end()) {
        category = std::string(categoryOf(lit->second->manifest().type));
    } else if (prefixKnown) {
        category = std::string(prefix); // unknown plugin: trust the explicit prefix
    }

    if (category.empty() || category == "clover" || category == "cloverjs") {
        return {nullptr, std::move(pluginName)};
    }
    return {findLoaderLocked(category), std::move(pluginName)};
}

auto PluginRuntime::scan() const -> std::vector<Plugin::PluginListing> {
    std::filesystem::path              pluginsDirectory;
    std::set<std::string, std::less<>> loadedNames;
    {
        std::lock_guard lock(mMutex);
        if (!mInitialized) {
            return {};
        }
        pluginsDirectory = mPluginsDirectory;
        for (auto const& name: mLoadedPlugins | std::views::keys) {
            loadedNames.emplace(name);
        }
    }

    std::vector<Plugin::PluginListing> result;
    std::error_code                    ec;
    if (!std::filesystem::exists(pluginsDirectory, ec) || ec) {
        return result;
    }
    std::filesystem::directory_iterator iterator(pluginsDirectory, ec);
    if (ec) {
        return result;
    }
    for (auto const& entry: iterator) {
        if (std::error_code entryEc; !entry.is_directory(entryEc) || entryEc) {
            continue;
        }
        auto manifest = Resolver::loadManifest(entry.path());
        if (!manifest) {
            continue;
        }
        auto const& value    = manifest.value();
        const auto  category = categoryOf(value.type);
        result.emplace_back(Plugin::PluginListing{
                .name        = std::string(category) + ":" + value.name,
                .type        = std::string(category),
                .version     = value.version,
                .author      = value.author.value_or(std::string{}),
                .description = value.description.value_or(std::string{}),
                .passive     = value.passive,
                .loaded      = loadedNames.contains(value.name),
        });
    }
    return result;
}

auto PluginRuntime::initializeState() -> Expected<void> {
    std::lock_guard lock(mMutex);
    if (mInitialized) {
        return {};
    }

    mLoadedPlugins.clear();
    mManifests.clear();
    mLoadOrder.clear();
    mPendingExternal.clear();
    mConfiguredPluginsLoaded = false;
    mHasFailedUnload         = false;

    const auto module = Utils::System::GetCurrentModuleHandle();
    mCoreDirectory    = CloverNT::Utils::System::GetModuleDirectory(module);
    mPluginsDirectory = std::filesystem::path(CloverPluginsDirectory());

    auto coreManifest = Resolver::makeCoreManifest();
    auto corePlugin   = std::make_shared<Plugin::Plugin>(coreManifest, false, true);
    corePlugin->setState(Plugin::PluginState::Loaded);
    mLoadedPlugins.emplace(coreManifest.name, corePlugin);
    mLoadOrder.emplace_back(coreManifest.name);
    mManifests.emplace(coreManifest.name, ManifestEntry{coreManifest, mCoreDirectory});

    std::error_code ec;
    if (!std::filesystem::exists(mPluginsDirectory, ec)) {
        if (ec) {
            return unexpected(makePluginError("failed to access plugins directory: " + ec.message(),
                                              CommonErrorCode::OperationFailed));
        }

        mInitialized = true;
        logInfo("plugins directory does not exist: " + mPluginsDirectory.string());
        return {};
    }

    std::filesystem::directory_iterator iterator(mPluginsDirectory, ec);
    if (ec) {
        return unexpected(makePluginError("failed to enumerate plugins directory: " + ec.message(),
                                          CommonErrorCode::OperationFailed));
    }

    for (auto const& entry: iterator) {
        if (std::error_code entryEc; !entry.is_directory(entryEc) || entryEc) {
            continue;
        }

        auto manifest = Resolver::loadManifest(entry.path());
        if (!manifest) {
            logError(entry.path().string() + ": " + manifest.error().message);
            continue;
        }
        auto manifestValue = std::move(manifest.value());
        if (manifestValue.type == "raw") {
            continue; // raw plugins belong to the bottom layer (loaded by CloverLoader.dll)
        }
        auto manifestName = manifestValue.name;
        if (mManifests.contains(manifestName)) {
            logError("duplicate plugin name: " + manifestName);
            continue;
        }
        mManifests.emplace(std::move(manifestName), ManifestEntry{std::move(manifestValue), entry.path()});
    }

    mInitialized = true;
    return {};
}

auto PluginRuntime::loadConfiguredPlugins() -> Expected<void> {
    auto guard = LifecycleGuard::acquire(
            *this, LifecycleOperation::Loading, "Plugin runtime is already performing a lifecycle operation");
    if (!guard) {
        return unexpected(guard.error());
    }

    bool needsInitialize = false;
    {
        std::lock_guard lock(mMutex);
        needsInitialize = !mInitialized;
    }
    if (needsInitialize) {
        if (auto result = initializeState(); !result) {
            return result;
        }
    }

    Resolver::ManifestMap manifests;
    {
        std::lock_guard lock(mMutex);
        if (mHasFailedUnload) {
            return unexpected(makePluginError(
                    "Plugin runtime has failed unload residue; shutdown or reinitialize before enabling plugins",
                    CommonErrorCode::InvalidState));
        }
        if (mConfiguredPluginsLoaded) {
            return {};
        }
        manifests = mManifests;
        mLoadOrder.clear();
        mLoadOrder.emplace_back(CorePluginName);
        mPendingExternal.clear();
    }

    mReporter.reset();
    mJsPending = 0;

    auto [selected, log] = Resolver::resolveLoadSet(manifests);
    if (!log.empty()) {
        logError(log);
    }

    auto order = Resolver::sortLoadOrder(manifests, selected);
    if (!order) {
        return unexpected(makePluginError(order.error().message, CommonErrorCode::InvalidState));
    }

    for (auto const& name: order.value()) {
        if (name != CorePluginName) {
            mReporter.ensureHeader();
            break;
        }
    }

    std::set<std::string, std::less<>> failed;
    for (auto const& name: order.value()) {
        if (name == CorePluginName) {
            continue;
        }
        auto const& entry    = manifests.at(name);
        auto        manifest = entry.manifest;

        if (manifest.type == "raw") {
            continue; // raw plugins are the bottom layer's domain (loaded by CloverLoader.dll)
        }

        bool dependencyFailed = false;
        {
            std::lock_guard lock(mMutex);
            for (const auto& dependency: manifest.dependencies | std::views::keys) {
                if (failed.contains(dependency) || !mLoadedPlugins.contains(dependency)) {
                    dependencyFailed = true;
                }
            }
        }
        if (dependencyFailed) {
            failed.emplace(name);
            mReporter.recordFailed(name, "a dependency failed to load");
            continue;
        }

        std::error_code ec;
        auto            pluginDirectory = std::filesystem::absolute(entry.directory, ec).lexically_normal();
        if (ec) {
            failed.emplace(name);
            mReporter.recordFailed(name, "invalid directory: " + ec.message());
            continue;
        }

        if (manifest.type != "native" && manifest.type != "js") {
            auto plugin = std::make_shared<Plugin::Plugin>(std::move(manifest), /*unloadable*/ true);
            plugin->setState(Plugin::PluginState::Loading);
            {
                std::lock_guard lock(mMutex);
                mLoadedPlugins.emplace(std::string(plugin->name()), plugin);
                mLoadOrder.emplace_back(plugin->name());
                mPendingExternal.push_back(PendingExternal{std::string(plugin->name()),
                                                           std::string(categoryOf(plugin->manifest().type)),
                                                           pluginDirectory});
            }
            continue;
        }

        if (manifest.type == "js") {
            auto built = makeJsPlugin(std::move(manifest), pluginDirectory);
            if (!built) {
                failed.emplace(name);
                mReporter.recordFailed(name, built.error().message);
                continue;
            }
            auto plugin = built.value();
            plugin->setState(Plugin::PluginState::Loading);
            {
                std::lock_guard lock(mMutex);
                mLoadedPlugins.emplace(std::string(plugin->name()), plugin);
                mLoadOrder.emplace_back(plugin->name());
            }
            if (auto scheduled = mJs->enqueue(plugin); scheduled) {
                ++mJsPending;
            } else {
                mReporter.recordFailed(name, "failed to schedule: " + scheduled.error().message);
            }
            continue;
        }

        auto pluginEntry = (pluginDirectory / manifest.entry).lexically_normal();
        if (std::filesystem::path(manifest.entry).is_absolute() ||
            !CloverNT::Utils::System::IsPathInside(pluginEntry, pluginDirectory) ||
            !std::filesystem::exists(pluginEntry, ec) || ec) {
            failed.emplace(name);
            mReporter.recordFailed(name, "invalid entry path: " + manifest.entry);
            continue;
        }

        auto plugin = std::make_shared<Plugin::NativePlugin>(std::move(manifest), pluginDirectory, pluginEntry);
        plugin->setState(Plugin::PluginState::Loading);

        if (auto loadedLibrary = plugin->mLibrary.load(plugin->entryPath(), plugin->directory()); !loadedLibrary) {
            plugin->setState(Plugin::PluginState::Failed);
            failed.emplace(name);
            mReporter.recordFailed(name, "failed to load library: " + loadedLibrary.error().message);
            continue;
        }

        auto loadCallback = plugin->mLibrary.getSymbol<Plugin::NativePlugin::Callback>("clovernt_plugin_load");
        if (!loadCallback) {
            (void) plugin->mLibrary.close();
            plugin->setState(Plugin::PluginState::Failed);
            failed.emplace(name);
            mReporter.recordFailed(name, "does not export clovernt_plugin_load: " + loadCallback.error().message);
            continue;
        }
        plugin->mLoadCallback = loadCallback.value();

        bool loaded = false;
        try {
            CurrentPluginScope currentPlugin(plugin);
            loaded = guardVeh([&] { return plugin->mLoadCallback(*plugin); });
        } catch (const std::exception& e) {
            loaded = false;
            mReporter.recordFailed(name, std::string("load callback threw: ") + e.what());
        } catch (...) {
            loaded = false;
        }

        if (!loaded) {
            if (auto runtime = hookRuntime()) {
                (void) runtime->get().clearPluginResources(plugin->name());
            }
            (void) Event::EventBus::clearPlugin(plugin->name());
            (void) plugin->mLibrary.close();
            plugin->setState(Plugin::PluginState::Failed);
            failed.emplace(name);
            mReporter.recordFailed(name, "load callback failed");
            continue;
        }

        plugin->setUnloadable(plugin->hasUnloadHandler());

        std::string loadedLabel = pluginLabel(plugin->name(), plugin->manifest().version);
        {
            std::lock_guard lock(mMutex);
            plugin->setState(Plugin::PluginState::Loaded);
            mLoadedPlugins.emplace(std::string(plugin->name()), plugin);
            mLoadOrder.emplace_back(plugin->name());
        }
        mReporter.recordLoaded(loadedLabel);
    }

    if (mJsPending == 0) {
        drainPendingExternal();
        mReporter.finish();
    }

    {
        std::lock_guard lock(mMutex);
        mConfiguredPluginsLoaded = true;
        mHasFailedUnload         = false;
    }
    return {};
}

void PluginRuntime::drainPendingExternal() {
    std::vector<PendingExternal> pending;
    {
        std::lock_guard lock(mMutex);
        pending.swap(mPendingExternal);
    }
    for (auto const& item: pending) {
        std::shared_ptr<Plugin::PluginLoader> loader;
        std::shared_ptr<Plugin::Plugin>       plugin;
        {
            std::lock_guard lock(mMutex);
            loader              = findLoaderLocked(item.loaderId);
            const auto pluginIt = mLoadedPlugins.find(item.name);
            plugin              = pluginIt == mLoadedPlugins.end() ? nullptr : pluginIt->second;
        }
        if (!plugin) {
            continue; // unloaded/removed before the drain ran
        }
        const auto drop = [&](std::string_view reason) {
            plugin->setState(Plugin::PluginState::Failed);
            {
                std::lock_guard lock(mMutex);
                mLoadedPlugins.erase(item.name);
                std::erase(mLoadOrder, item.name);
            }
            mReporter.recordFailed(item.name, reason);
        };
        if (!loader) {
            drop("no loader registered for type '" + item.loaderId + "'");
            continue;
        }
        if (auto result = loader->load(item.name); !result) {
            drop(result.error().message);
            continue;
        }
        plugin->setState(Plugin::PluginState::Loaded);
        mReporter.recordLoaded(pluginLabel(item.name, plugin->manifest().version));
    }
}

auto PluginRuntime::rescan() -> Expected<std::size_t> {
    std::filesystem::path pluginsDirectory;
    {
        std::lock_guard lock(mMutex);
        if (!mInitialized) {
            return unexpected(makePluginError("plugin runtime is not initialized", CommonErrorCode::InvalidState));
        }
        pluginsDirectory = mPluginsDirectory;
    }

    std::error_code ec;
    if (!std::filesystem::exists(pluginsDirectory, ec) || ec) {
        return std::size_t{0};
    }
    std::filesystem::directory_iterator iterator(pluginsDirectory, ec);
    if (ec) {
        return unexpected(makePluginError("failed to enumerate plugins directory: " + ec.message(),
                                          CommonErrorCode::OperationFailed));
    }

    bool discoveredAny = false;
    for (auto const& entry: iterator) {
        if (std::error_code entryEc; !entry.is_directory(entryEc) || entryEc) {
            continue;
        }
        auto manifest = Resolver::loadManifest(entry.path());
        if (!manifest) {
            continue; // no/invalid clover.json here — not ours to load
        }
        auto value = std::move(manifest.value());
        if (value.type == "raw") {
            continue;
        }
        std::lock_guard lock(mMutex);
        if (mManifests.contains(value.name)) {
            continue; // already known from the initial scan or an earlier rescan
        }
        auto name = value.name;
        mManifests.emplace(std::move(name), ManifestEntry{std::move(value), entry.path()});
        discoveredAny = true;
    }
    if (!discoveredAny) {
        return std::size_t{0};
    }

    Resolver::ManifestMap manifests;
    {
        std::lock_guard lock(mMutex);
        manifests = mManifests;
    }
    auto [selected, selectionLog] = Resolver::resolveLoadSet(manifests);
    if (!selectionLog.empty()) {
        logError(selectionLog);
    }
    auto order = Resolver::sortLoadOrder(manifests, selected);
    if (!order) {
        return unexpected(makePluginError(order.error().message, CommonErrorCode::InvalidState));
    }

    std::size_t queued = 0;
    for (auto const& name: order.value()) {
        if (name == CorePluginName) {
            continue;
        }
        {
            std::lock_guard lock(mMutex);
            if (mLoadedPlugins.contains(name)) {
                continue; // already loaded or pending from the initial load / an earlier rescan
            }
        }
        auto const& entry    = manifests.at(name);
        auto        manifest = entry.manifest;
        if (manifest.type == "raw" || manifest.type == "native" || manifest.type == "js") {
            continue; // rescan only hot-loads external-type plugins via their registered loader
        }

        std::error_code dirEc;
        auto            pluginDirectory = std::filesystem::absolute(entry.directory, dirEc).lexically_normal();
        if (dirEc) {
            mReporter.recordFailed(name, "invalid directory: " + dirEc.message());
            continue;
        }

        auto plugin = std::make_shared<Plugin::Plugin>(std::move(manifest), /*unloadable*/ true);
        plugin->setState(Plugin::PluginState::Loading);
        {
            std::lock_guard lock(mMutex);
            mLoadedPlugins.emplace(std::string(plugin->name()), plugin);
            mLoadOrder.emplace_back(plugin->name());
            mPendingExternal.push_back(PendingExternal{
                    std::string(plugin->name()), std::string(categoryOf(plugin->manifest().type)), pluginDirectory});
        }
        ++queued;
    }

    if (queued == 0) {
        return std::size_t{0};
    }

    bool jsInFlight = false;
    {
        std::lock_guard lock(mMutex);
        jsInFlight = mJsPending > 0;
    }
    if (!jsInFlight) {
        drainPendingExternal();
    }
    return queued;
}

auto PluginRuntime::teardownPlugin(std::shared_ptr<Plugin::Plugin> const& plugin,
                                   const bool                             recoverable,
                                   const bool                             finalShutdown) -> Expected<void> {
    const std::string name(plugin->name());
    const auto        native = std::dynamic_pointer_cast<Plugin::NativePlugin>(plugin);

    std::shared_ptr<Plugin::PluginLoader> externalLoader;
    if (!native) {
        if (const auto category = categoryOf(plugin->manifest().type); category != "clover" && category != "cloverjs") {
            std::lock_guard lock(mMutex);
            externalLoader = findLoaderLocked(category);
        }
    }

    plugin->setState(Plugin::PluginState::Unloading);

    auto runtime = hookRuntime();
    if (native && !runtime && recoverable) {
        std::lock_guard lock(mMutex);
        plugin->setState(Plugin::PluginState::Loaded);
        return unexpected(runtime.error());
    }

    bool suspended = true;
    if (native && runtime) {
        if (auto result = runtime->get().suspendPluginResources(name); !result) {
            suspended = false;
            if (recoverable) {
                std::lock_guard lock(mMutex);
                plugin->setState(Plugin::PluginState::Loaded);
                return unexpected(
                        makePluginError(name + " failed to suspend plugin resources: " + result.error().message));
            }
        }
    }

    bool        callbackOk = true;
    std::string callbackError;
    if (native) {
        if (native->hasUnloadHandler() && suspended) {
            try {
                CurrentPluginScope currentPlugin(native);
                callbackOk = guardVeh([&] { return native->invokeUnloadHandler(); });
            } catch (const std::exception& e) {
                callbackOk    = false;
                callbackError = name + " unload callback threw: " + e.what();
            } catch (...) {
                callbackOk = false;
            }
            if (!callbackOk && callbackError.empty()) {
                callbackError = name + " unload callback failed";
            }
        } else if (!suspended) {
            callbackOk    = false;
            callbackError = name + " unload callback skipped because resources could not be suspended";
        }
    } else if (externalLoader) {
        if (auto result = externalLoader->unload(name);
            !result && result.error().code != static_cast<std::int32_t>(CommonErrorCode::Inactive)) {
            callbackOk    = false;
            callbackError = name + " unload failed: " + result.error().message;
        }
    } else if (Platform::NodeEnvironment::isRunnable()) {
        if (auto result = mJs->unloadPlugin(name);
            !result && result.error().code != static_cast<std::int32_t>(CommonErrorCode::Inactive)) {
            callbackOk    = false;
            callbackError = name + " unload failed: " + result.error().message;
        }
    }

    if (!callbackOk && recoverable) {
        std::string resumeError;
        auto        restored = Plugin::PluginState::Loaded;
        if (native && runtime && suspended) {
            if (auto resumed = runtime->get().resumePluginResources(name); !resumed) {
                resumeError = " and resource resume failed: " + resumed.error().message;
                restored    = Plugin::PluginState::Failed;
            }
        }
        {
            std::lock_guard lock(mMutex);
            plugin->setState(restored);
        }
        return unexpected(makePluginError((callbackError.empty() ? name + " unload callback failed" : callbackError) +
                                                  resumeError,
                                          CommonErrorCode::CallbackFailed));
    }

    std::vector<std::string> errors;
    if (!callbackOk) {
        appendError(errors, callbackError.empty() ? name + " unload callback failed; forcing cleanup" : callbackError);
    }

    bool cleanupOk = callbackOk;
    if (native) {
        if (runtime) {
            if (auto cleared = runtime->get().clearPluginResources(name); !cleared) {
                appendError(errors, name + " failed to clear plugin resources: " + cleared.error().message);
                cleanupOk = false;
            }
        } else {
            appendError(errors, name + " resource runtime unavailable during unload: " + runtime.error().message);
            cleanupOk = false;
        }
    }
    if (auto clearedEvents = Event::EventBus::clearPlugin(name); !clearedEvents) {
        appendError(errors, name + " failed to clear plugin events: " + clearedEvents.error().message);
        cleanupOk = false;
    }
    if (auto idle = Event::EventBus::waitPluginIdle(name); !idle) {
        appendError(errors, name + " failed to wait for plugin event listeners: " + idle.error().message);
        cleanupOk = false;
    }

    if (native) {
        if (cleanupOk) {
            if (auto closed = native->mLibrary.close(); !closed) {
                appendError(errors, name + " failed to free library: " + closed.error().message);
            }
        } else if (finalShutdown) {
            native->mLibrary.releaseWithoutClose();
        } else {
            appendError(errors, name + " library close skipped because plugin cleanup was incomplete");
        }
    } else if (externalLoader) {
    } else {
        mJs->forget(name);
    }

    {
        std::lock_guard lock(mMutex);
        mLoadedPlugins.erase(name);
        std::erase(mLoadOrder, name);
        plugin->setState(errors.empty() ? Plugin::PluginState::Unloaded : Plugin::PluginState::Failed);
    }

    if (!errors.empty()) {
        return unexpected(makePluginError(name + " unload cleanup failed:\n" + joinErrors(errors)));
    }
    logInfo(name + " unloaded");
    return {};
}

auto PluginRuntime::unloadPlugin(const std::string_view name) -> Expected<void> {
    auto [externalLoader, pluginName] = resolveRoute(name);
    auto guard                        = LifecycleGuard::acquire(
            *this, LifecycleOperation::Unloading, "Plugin runtime is already performing a lifecycle operation");
    if (!guard) {
        return unexpected(guard.error());
    }

    if (externalLoader) {
        std::shared_ptr<Plugin::Plugin> external;
        {
            std::lock_guard lock(mMutex);
            const auto      it = mLoadedPlugins.find(pluginName);
            if (it == mLoadedPlugins.end()) {
                return unexpected(makePluginError("plugin is not loaded: " + pluginName, CommonErrorCode::NotFound));
            }
            if (pluginName == CorePluginName || it->second->isBuiltin()) {
                return unexpected(
                        makePluginError("CloverCore cannot be unloaded directly", CommonErrorCode::Unsupported));
            }
            for (auto const& [otherName, otherPlugin]: mLoadedPlugins) {
                if (otherName != pluginName && Resolver::manifestDependsOn(otherPlugin->manifest(), pluginName)) {
                    return unexpected(makePluginError(
                            std::format("{} cannot be unloaded because {} depends on it", pluginName, otherName),
                            CommonErrorCode::InvalidState));
                }
            }
            external = it->second;
        }
        if (auto result = externalLoader->unload(pluginName); !result) {
            return unexpected(result.error());
        }
        {
            std::lock_guard lock(mMutex);
            mLoadedPlugins.erase(pluginName);
            std::erase(mLoadOrder, pluginName);
            external->setState(Plugin::PluginState::Unloaded);
        }
        logInfo(pluginName + " unloaded");
        return {};
    }

    std::shared_ptr<Plugin::Plugin> plugin;
    {
        std::lock_guard lock(mMutex);
        const auto      pluginIt = mLoadedPlugins.find(pluginName);
        if (pluginIt == mLoadedPlugins.end()) {
            return unexpected(makePluginError("plugin is not loaded: " + pluginName, CommonErrorCode::NotFound));
        }
        if (pluginName == CorePluginName || pluginIt->second->isBuiltin()) {
            return unexpected(makePluginError("CloverCore cannot be unloaded directly", CommonErrorCode::Unsupported));
        }
        if (const auto currentState = pluginIt->second->state();
            currentState != Plugin::PluginState::Loaded && currentState != Plugin::PluginState::Failed) {
            return unexpected(
                    makePluginError(pluginName + " is not in a loaded or failed state", CommonErrorCode::InvalidState));
        }
        for (auto const& [otherName, otherPlugin]: mLoadedPlugins) {
            if (otherName == pluginName) {
                continue;
            }
            if (Resolver::manifestDependsOn(otherPlugin->manifest(), pluginName)) {
                return unexpected(makePluginError(
                        std::format("{} cannot be unloaded because {} depends on it", pluginName, otherName),
                        CommonErrorCode::InvalidState));
            }
        }

        const auto native = std::dynamic_pointer_cast<Plugin::NativePlugin>(pluginIt->second);
        const auto js     = std::dynamic_pointer_cast<Plugin::JsPlugin>(pluginIt->second);
        if (native && !native->hasUnloadHandler()) {
            return unexpected(
                    makePluginError(pluginName + " does not support hot unload", CommonErrorCode::Unsupported));
        }
        if (!native && !js) {
            return unexpected(
                    makePluginError(pluginName + " does not support hot unload", CommonErrorCode::Unsupported));
        }
        plugin = pluginIt->second;
    }

    return teardownPlugin(plugin, /*recoverable*/ true, /*finalShutdown*/ false);
}

auto PluginRuntime::reloadPlugin(const std::string_view name) -> Expected<void> {
    auto [externalLoader, pluginName] = resolveRoute(name);
    auto guard                        = LifecycleGuard::acquire(
            *this, LifecycleOperation::Unloading, "Plugin runtime is already performing a lifecycle operation");
    if (!guard) {
        return unexpected(guard.error());
    }

    if (externalLoader) {
        {
            std::lock_guard lock(mMutex);
            const auto      it = mLoadedPlugins.find(pluginName);
            if (it == mLoadedPlugins.end()) {
                return unexpected(makePluginError("plugin is not loaded: " + pluginName, CommonErrorCode::NotFound));
            }
            if (pluginName == CorePluginName || it->second->isBuiltin()) {
                return unexpected(makePluginError("CloverCore cannot be reloaded", CommonErrorCode::Unsupported));
            }
        }
        if (auto result = externalLoader->reload(pluginName); !result) {
            return unexpected(makePluginError(pluginName + " reload failed: " + result.error().message,
                                              CommonErrorCode::CallbackFailed));
        }
        logInfo(pluginName + " reloaded");
        return {};
    }

    std::shared_ptr<Plugin::JsPlugin> js;
    {
        std::lock_guard lock(mMutex);
        const auto      pluginIt = mLoadedPlugins.find(pluginName);
        if (pluginIt == mLoadedPlugins.end()) {
            return unexpected(makePluginError("plugin is not loaded: " + pluginName, CommonErrorCode::NotFound));
        }
        if (pluginName == CorePluginName || pluginIt->second->isBuiltin()) {
            return unexpected(makePluginError("CloverCore cannot be reloaded", CommonErrorCode::Unsupported));
        }
        js = std::dynamic_pointer_cast<Plugin::JsPlugin>(pluginIt->second);
        if (!js) {
            return unexpected(
                    makePluginError(pluginName + " only JS plugins support reload", CommonErrorCode::Unsupported));
        }
        if (js->state() != Plugin::PluginState::Loaded) {
            return unexpected(makePluginError(pluginName + " is not in Loaded state", CommonErrorCode::InvalidState));
        }
        for (auto const& [otherName, otherPlugin]: mLoadedPlugins) {
            if (otherName != pluginName && Resolver::manifestDependsOn(otherPlugin->manifest(), pluginName)) {
                return unexpected(makePluginError(
                        std::format("{} cannot be reloaded because {} depends on it", pluginName, otherName),
                        CommonErrorCode::InvalidState));
            }
        }
        js->setState(Plugin::PluginState::Loading);
    }

    auto result = mJs->reloadPlugin(pluginName);
    if (!result) {
        {
            std::lock_guard lock(mMutex);
            js->setState(Plugin::PluginState::Failed);
            mLoadedPlugins.erase(pluginName);
            std::erase(mLoadOrder, pluginName);
        }
        mJs->forget(pluginName);
        return unexpected(makePluginError(pluginName + " reload failed: " + result.error().message,
                                          CommonErrorCode::CallbackFailed));
    }
    {
        std::lock_guard lock(mMutex);
        js->setState(Plugin::PluginState::Loaded);
    }
    logInfo(pluginName + " reloaded");
    return {};
}

auto PluginRuntime::loadPlugin(std::string_view name) -> Expected<void> {
    auto [externalLoader, pluginName] = resolveRoute(name);
    auto guard                        = LifecycleGuard::acquire(
            *this, LifecycleOperation::Loading, "Plugin runtime is already performing a lifecycle operation");
    if (!guard) {
        return unexpected(guard.error());
    }

    if (externalLoader) {
        Plugin::Manifest manifest;
        {
            std::lock_guard lock(mMutex);
            if (mLoadedPlugins.contains(pluginName)) {
                return unexpected(makePluginError(pluginName + " is already loaded", CommonErrorCode::AlreadyExists));
            }
            const auto it = mManifests.find(pluginName);
            if (it == mManifests.end()) {
                return unexpected(makePluginError("unknown plugin: " + pluginName, CommonErrorCode::NotFound));
            }
            manifest = it->second.manifest;
            for (const auto& dependency: manifest.dependencies | std::views::keys) {
                if (!mLoadedPlugins.contains(dependency)) {
                    return unexpected(
                            makePluginError(std::format("{} is missing dependency {}", pluginName, dependency),
                                            CommonErrorCode::InvalidState));
                }
            }
        }
        if (auto result = externalLoader->load(pluginName); !result) {
            return unexpected(makePluginError(pluginName + " load failed: " + result.error().message,
                                              CommonErrorCode::CallbackFailed));
        }
        auto plugin = std::make_shared<Plugin::Plugin>(std::move(manifest), /*unloadable*/ true);
        plugin->setState(Plugin::PluginState::Loaded);
        std::string label = pluginLabel(plugin->name(), plugin->manifest().version);
        {
            std::lock_guard lock(mMutex);
            mLoadedPlugins.emplace(pluginName, plugin);
            mLoadOrder.emplace_back(pluginName);
        }
        logInfo("Loaded " + label);
        return {};
    }

    std::filesystem::path pluginsDirectory;
    {
        std::lock_guard lock(mMutex);
        if (mLoadedPlugins.contains(pluginName)) {
            return unexpected(makePluginError(pluginName + " is already loaded", CommonErrorCode::AlreadyExists));
        }
        pluginsDirectory = mPluginsDirectory;
    }

    const auto pluginDirectory = pluginsDirectory / pluginName;
    auto       manifestResult  = Resolver::loadManifest(pluginDirectory);
    if (!manifestResult) {
        return unexpected(makePluginError(pluginName + " manifest error: " + manifestResult.error().message,
                                          CommonErrorCode::ParseFailed));
    }
    auto manifest = std::move(manifestResult.value());
    if (manifest.type != "js") {
        return unexpected(
                makePluginError(pluginName + " only JS plugins support hot load", CommonErrorCode::Unsupported));
    }

    {
        std::lock_guard lock(mMutex);
        for (const auto& dependency: manifest.dependencies | std::views::keys) {
            if (!mLoadedPlugins.contains(dependency)) {
                return unexpected(makePluginError(std::format("{} is missing dependency {}", pluginName, dependency),
                                                  CommonErrorCode::InvalidState));
            }
        }
    }

    auto built = makeJsPlugin(std::move(manifest), pluginDirectory);
    if (!built) {
        return unexpected(makePluginError(pluginName + ": " + built.error().message, CommonErrorCode::NotFound));
    }
    auto plugin = built.value();
    plugin->setState(Plugin::PluginState::Loading);
    {
        std::lock_guard lock(mMutex);
        mLoadedPlugins.emplace(pluginName, plugin);
        mLoadOrder.emplace_back(pluginName);
    }
    if (auto scheduled = mJs->enqueue(plugin); !scheduled) {
        std::lock_guard lock(mMutex);
        mLoadedPlugins.erase(pluginName);
        std::erase(mLoadOrder, pluginName);
        return unexpected(makePluginError(pluginName + " failed to schedule: " + scheduled.error().message));
    }

    auto result = mJs->loadPlugin(pluginName);
    {
        std::lock_guard lock(mMutex);
        if (result) {
            plugin->setState(Plugin::PluginState::Loaded);
        } else {
            plugin->setState(Plugin::PluginState::Failed);
            mLoadedPlugins.erase(pluginName);
            std::erase(mLoadOrder, pluginName);
        }
    }
    if (!result) {
        mJs->forget(pluginName);
        return unexpected(makePluginError(pluginName + " load failed: " + result.error().message,
                                          CommonErrorCode::CallbackFailed));
    }
    logInfo("Loaded " + pluginLabel(plugin->name(), plugin->manifest().version));
    return {};
}

auto PluginRuntime::unloadAll() -> Expected<void> {
    return unloadLoadedPlugins();
}

auto PluginRuntime::unloadLoadedPlugins() -> Expected<void> {
    auto guard = LifecycleGuard::acquire(
            *this, LifecycleOperation::Unloading, "Plugin runtime is already performing a lifecycle operation");
    if (!guard) {
        return unexpected(guard.error());
    }
    return unloadLoadedPluginsImpl(false);
}

auto PluginRuntime::unloadLoadedPluginsImpl(const bool finalShutdown) -> Expected<void> {
    std::vector<std::string> order;
    {
        std::lock_guard lock(mMutex);
        if (!mInitialized) {
            return {};
        }
        order = mLoadOrder;
    }

    std::vector<std::string> errors;
    bool                     anyFailed = false;
    for (auto& it: std::views::reverse(order)) {
        if (it == CorePluginName) {
            continue;
        }
        std::shared_ptr<Plugin::Plugin> plugin;
        {
            std::lock_guard lock(mMutex);
            const auto      pluginIt = mLoadedPlugins.find(it);
            if (pluginIt == mLoadedPlugins.end()) {
                continue;
            }
            plugin = pluginIt->second;
        }

        if (auto torn = teardownPlugin(plugin, /*recoverable*/ false, finalShutdown); !torn) {
            errors.emplace_back(torn.error().message);
            anyFailed = true;
            if (!finalShutdown) {
                break;
            }
        }
    }

    {
        std::lock_guard lock(mMutex);
        mConfiguredPluginsLoaded = false;
        mHasFailedUnload         = !finalShutdown && anyFailed;
    }

    if (!errors.empty()) {
        return unexpected(makeError(ErrorCategory::Plugin, CommonErrorCode::OperationFailed, joinErrors(errors)));
    }
    return {};
}

auto PluginRuntime::clearState() -> Expected<void> {
    auto guard = LifecycleGuard::acquire(
            *this, LifecycleOperation::ShuttingDown, "Plugin runtime is already performing a lifecycle operation");
    if (!guard) {
        return unexpected(guard.error());
    }

    {
        std::lock_guard lock(mMutex);
        if (!mInitialized) {
            return {};
        }
    }

    auto unloadResult = unloadLoadedPluginsImpl(true);

    {
        std::lock_guard lock(mMutex);
        mLoadedPlugins.clear();
        mManifests.clear();
        mLoadOrder.clear();
        mPendingExternal.clear();
        mCoreDirectory.clear();
        mPluginsDirectory.clear();
        mConfiguredPluginsLoaded = false;
        mHasFailedUnload         = false;
        mInitialized             = false;
    }
    return unloadResult;
}

} // namespace CloverNT::Core::Modules

namespace CloverNT::Plugin {
namespace {

    Expected<Core::Modules::ModuleRef<Core::Modules::PluginRuntime>> pluginRuntimeModule() {
        return Core::Modules::Manager::getInstance().requireModule<Core::Modules::PluginRuntime>();
    }

} // namespace

PluginManager::PluginManager() = default;

PluginManager::~PluginManager() = default;

PluginManager& PluginManager::getInstance() {
    static PluginManager instance;
    return instance;
}

std::shared_ptr<Plugin> PluginManager::getPlugin(const std::string_view name) {
    const auto runtime = pluginRuntimeModule();
    return runtime ? runtime->get().getPlugin(name) : nullptr;
}

std::shared_ptr<Plugin> PluginManager::currentPlugin() {
    const auto runtime = pluginRuntimeModule();
    if (!runtime) {
        return nullptr;
    }
    if (auto scoped = Core::Modules::PluginRuntime::currentPlugin()) {
        return scoped;
    }
    return runtime->get().getPlugin(CorePluginName);
}

std::vector<std::shared_ptr<Plugin>> PluginManager::plugins() {
    const auto runtime = pluginRuntimeModule();
    return runtime ? runtime->get().plugins() : std::vector<std::shared_ptr<Plugin>>{};
}

std::vector<PluginListing> PluginManager::scan() {
    const auto runtime = pluginRuntimeModule();
    return runtime ? runtime->get().scan() : std::vector<PluginListing>{};
}

auto PluginManager::unloadPlugin(const std::string_view name) -> Expected<void> {
    auto runtime = pluginRuntimeModule();
    if (!runtime) {
        return unexpected(runtime.error());
    }
    return runtime->get().unloadPlugin(name);
}

auto PluginManager::loadPlugin(const std::string_view name) -> Expected<void> {
    auto runtime = pluginRuntimeModule();
    if (!runtime) {
        return unexpected(runtime.error());
    }
    return runtime->get().loadPlugin(name);
}

auto PluginManager::reloadPlugin(const std::string_view name) -> Expected<void> {
    auto runtime = pluginRuntimeModule();
    if (!runtime) {
        return unexpected(runtime.error());
    }
    return runtime->get().reloadPlugin(name);
}

auto PluginManager::unloadAllPlugins() -> Expected<void> {
    auto runtime = pluginRuntimeModule();
    if (!runtime) {
        return unexpected(runtime.error());
    }
    return runtime->get().unloadAll();
}

auto PluginManager::rescan() -> Expected<std::size_t> {
    auto runtime = pluginRuntimeModule();
    if (!runtime) {
        return unexpected(runtime.error());
    }
    return runtime->get().rescan();
}

auto PluginManager::registerLoader(std::shared_ptr<PluginLoader> loader) -> Expected<void> {
    auto runtime = pluginRuntimeModule();
    if (!runtime) {
        return unexpected(runtime.error());
    }
    return runtime->get().registerLoader(std::move(loader));
}

auto PluginManager::unregisterLoader(const std::string_view id) -> Expected<void> {
    auto runtime = pluginRuntimeModule();
    if (!runtime) {
        return unexpected(runtime.error());
    }
    return runtime->get().unregisterLoader(id);
}

} // namespace CloverNT::Plugin
