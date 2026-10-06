#include <CloverNT/API/Exception.hpp>
#include <CloverNT/API/Logger.hpp>
#include <CloverNT/Core/Modules/Manager.hpp>

#include <algorithm>
#include <exception>
#include <ranges>

namespace CloverNT::Core::Modules {
namespace {

    Logger& logger() {
        static auto instance = LoggerRegistry::getInstance().getOrCreate("CloverCore");
        return *instance;
    }

    [[nodiscard]] auto aggregateModuleErrors(const std::string_view operation, std::vector<std::string> const& errors)
            -> Expected<void> {
        if (errors.empty()) {
            return {};
        }

        std::string message;
        for (auto const& error: errors) {
            if (!message.empty()) {
                message += '\n';
            }
            message += error;
        }
        return unexpected(ErrorCategory::Module,
                          CommonErrorCode::OperationFailed,
                          std::string(operation) + " failed:\n" + message);
    }

} // namespace

Manager::~Manager() {
    (void) destroy();
}

Manager& Manager::getInstance() {
    static Manager instance;
    return instance;
}

auto Manager::initialize() -> Expected<bool> {
    std::lock_guard lock(mMutex);
    if (mInitialized && !mDestroyed) {
        return false;
    }

    mModules.clear();
    mModuleNameMap.clear();
    mModuleOrder.clear();

    mInitialized = true;
    mDestroyed   = false;
    return true;
}

auto Manager::destroy() -> Expected<void> {
    std::vector<std::shared_ptr<ModuleInfoBase>> unloadOrder;
    {
        std::lock_guard lock(mMutex);
        if (!mInitialized || mDestroyed) {
            return {};
        }

        unloadOrder.reserve(mModuleOrder.size());
        for (auto& it: std::views::reverse(mModuleOrder)) {
            if (auto info = findInfoLocked(it)) {
                unloadOrder.push_back(info);
            }
        }
    }

    std::vector<std::string> errors;
    for (auto const& info: unloadOrder) {
        if (auto result = unloadModuleImpl(info); !result) {
            auto message = std::format("Module '{}': {}", info->name(), result.error().message);
            logger().error("{}", message);
            errors.emplace_back(std::move(message));
        }
    }

    if (!errors.empty()) {
        return aggregateModuleErrors("destroy", errors);
    }

    std::lock_guard lock(mMutex);
    mModules.clear();
    mModuleNameMap.clear();
    mModuleOrder.clear();
    mDestroyed = true;
    return {};
}

auto Manager::loadAll() -> Expected<void> {
    std::vector<std::shared_ptr<ModuleInfoBase>> loadOrder;
    {
        std::lock_guard lock(mMutex);
        loadOrder.reserve(mModuleOrder.size());
        for (auto const& type: mModuleOrder) {
            if (auto info = findInfoLocked(type)) {
                loadOrder.push_back(info);
            }
        }
    }

    std::vector<std::string> errors;
    for (auto const& info: loadOrder) {
        if (auto loaded = loadModuleImpl(info); !loaded) {
            auto message = std::format("Module '{}': {}", info->name(), loaded.error().message);
            logger().error("{}", message);
            errors.emplace_back(std::move(message));
        }
    }
    return aggregateModuleErrors("loadAll", errors);
}

auto Manager::enableAll() -> Expected<void> {
    std::vector<std::shared_ptr<ModuleInfoBase>> enableOrder;
    {
        std::lock_guard lock(mMutex);
        enableOrder.reserve(mModuleOrder.size());
        for (auto const& type: mModuleOrder) {
            if (auto info = findInfoLocked(type)) {
                enableOrder.push_back(info);
            }
        }
    }

    std::vector<std::string> errors;
    for (auto const& info: enableOrder) {
        if (auto enabled = enableModuleImpl(info); !enabled) {
            auto message = std::format("Module '{}': {}", info->name(), enabled.error().message);
            logger().error("{}", message);
            errors.emplace_back(std::move(message));
        }
    }
    return aggregateModuleErrors("enableAll", errors);
}

auto Manager::disableAll() -> Expected<void> {
    std::vector<std::shared_ptr<ModuleInfoBase>> disableOrder;
    {
        std::lock_guard lock(mMutex);
        disableOrder.reserve(mModuleOrder.size());
        for (auto const& type: std::views::reverse(mModuleOrder)) {
            if (auto info = findInfoLocked(type)) {
                disableOrder.push_back(info);
            }
        }
    }

    std::vector<std::string> errors;
    for (auto const& info: disableOrder) {
        if (auto disabled = disableModuleImpl(info); !disabled) {
            auto message = std::format("Module '{}': {}", info->name(), disabled.error().message);
            logger().error("{}", message);
            errors.emplace_back(std::move(message));
        }
    }
    return aggregateModuleErrors("disableAll", errors);
}

auto Manager::unloadAll() -> Expected<void> {
    std::vector<std::shared_ptr<ModuleInfoBase>> unloadOrder;
    {
        std::lock_guard lock(mMutex);
        unloadOrder.reserve(mModuleOrder.size());
        for (auto const& type: std::views::reverse(mModuleOrder)) {
            if (auto info = findInfoLocked(type)) {
                unloadOrder.push_back(info);
            }
        }
    }

    std::vector<std::string> errors;
    for (auto const& info: unloadOrder) {
        if (auto unloaded = unloadModuleImpl(info); !unloaded) {
            auto message = std::format("Module '{}': {}", info->name(), unloaded.error().message);
            logger().error("{}", message);
            errors.emplace_back(std::move(message));
        }
    }
    return aggregateModuleErrors("unloadAll", errors);
}

auto Manager::loadModule(const std::string_view name) -> Expected<void> {
    std::shared_ptr<ModuleInfoBase> info;
    {
        std::lock_guard lock(mMutex);
        info = findInfoLocked(name);
    }
    return loadModuleImpl(info);
}

auto Manager::enableModule(const std::string_view name) -> Expected<void> {
    std::shared_ptr<ModuleInfoBase> info;
    {
        std::lock_guard lock(mMutex);
        info = findInfoLocked(name);
    }
    return enableModuleImpl(info);
}

auto Manager::disableModule(const std::string_view name) -> Expected<void> {
    std::shared_ptr<ModuleInfoBase> info;
    {
        std::lock_guard lock(mMutex);
        info = findInfoLocked(name);
    }
    return disableModuleImpl(info);
}

auto Manager::unloadModule(const std::string_view name) -> Expected<void> {
    std::shared_ptr<ModuleInfoBase> info;
    {
        std::lock_guard lock(mMutex);
        info = findInfoLocked(name);
    }
    return unloadModuleImpl(info);
}

std::vector<Manager::ModuleStatus> Manager::modules() const {
    std::lock_guard           lock(mMutex);
    std::vector<ModuleStatus> result;
    result.reserve(mModuleOrder.size());
    for (auto const& type: mModuleOrder) {
        auto const it = mModules.find(type);
        if (it == mModules.end()) {
            continue;
        }

        auto const& info = *it->second;
        result.push_back(ModuleStatus{
                .name        = info.name(),
                .description = info.description(),
                .loaded      = info.isLoaded(),
                .enabled     = info.isEnabled(),
        });
    }
    return result;
}

std::optional<Manager::ModuleStatus> Manager::moduleInfo(const std::string_view name) const {
    std::lock_guard lock(mMutex);
    const auto      info = const_cast<Manager*>(this)->findInfoLocked(name);
    if (!info) {
        return std::nullopt;
    }

    return ModuleStatus{
            .name        = info->name(),
            .description = info->description(),
            .loaded      = info->isLoaded(),
            .enabled     = info->isEnabled(),
    };
}

auto Manager::findInfoLocked(std::type_index const& type) -> std::shared_ptr<ModuleInfoBase> {
    auto const it = mModules.find(type);
    return it == mModules.end() ? nullptr : it->second;
}

auto Manager::findInfoLocked(const std::string_view name) -> std::shared_ptr<ModuleInfoBase> {
    auto const nameIt = mModuleNameMap.find(std::string(name));
    if (nameIt == mModuleNameMap.end()) {
        return nullptr;
    }
    return findInfoLocked(nameIt->second);
}

auto Manager::loadModuleImpl(std::shared_ptr<ModuleInfoBase> const& info) -> Expected<void> {
    if (!info) {
        return unexpected(ErrorCategory::Module, CommonErrorCode::NotFound, "Module was not found");
    }

    if (!info->isLoaded()) {
        try {
            if (auto result = info->load(); !result) {
                return result;
            }
        } catch (const Exception& e) {
            return unexpected(e.error());
        } catch (...) {
            return unexpected(fromCurrentException(ErrorCategory::Module));
        }
    }
    return {};
}

auto Manager::unloadModuleImpl(std::shared_ptr<ModuleInfoBase> const& info) -> Expected<void> {
    if (!info) {
        return unexpected(ErrorCategory::Module, CommonErrorCode::NotFound, "Module was not found");
    }

    if (info->isLoaded()) {
        try {
            return info->unload();
        } catch (const Exception& e) {
            return unexpected(e.error());
        } catch (...) {
            return unexpected(fromCurrentException(ErrorCategory::Module));
        }
    }
    return {};
}

auto Manager::enableModuleImpl(std::shared_ptr<ModuleInfoBase> const& info) -> Expected<void> {
    if (!info) {
        return unexpected(ErrorCategory::Module, CommonErrorCode::NotFound, "Module was not found");
    }

    if (!info->isLoaded()) {
        if (auto result = loadModuleImpl(info); !result) {
            return result;
        }
    }
    if (!info->isEnabled()) {
        try {
            return info->enable();
        } catch (const Exception& e) {
            return unexpected(e.error());
        } catch (...) {
            return unexpected(fromCurrentException(ErrorCategory::Module));
        }
    }
    return {};
}

auto Manager::disableModuleImpl(std::shared_ptr<ModuleInfoBase> const& info) -> Expected<void> {
    if (!info) {
        return unexpected(ErrorCategory::Module, CommonErrorCode::NotFound, "Module was not found");
    }

    if (info->isEnabled()) {
        try {
            return info->disable();
        } catch (const Exception& e) {
            return unexpected(e.error());
        } catch (...) {
            return unexpected(fromCurrentException(ErrorCategory::Module));
        }
    }
    return {};
}

} // namespace CloverNT::Core::Modules
