#pragma once

#include <CloverNT/API/Expected.hpp>
#include <CloverNT/Core/Modules/ModuleBase.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace CloverNT::Core::Modules {

template <typename T>
class ModuleRef {
public:
    explicit ModuleRef(std::shared_ptr<T> ptr) noexcept : mPtr(std::move(ptr)) {}

    [[nodiscard]] auto get() const noexcept -> T& {
        return *mPtr;
    }
    [[nodiscard]] auto operator->() const noexcept -> T* {
        return mPtr.get();
    }
    [[nodiscard]] auto operator*() const noexcept -> T& {
        return *mPtr;
    }
    [[nodiscard]] auto shared() const noexcept -> std::shared_ptr<T> const& {
        return mPtr;
    }

private:
    std::shared_ptr<T> mPtr;
};

class Manager {
public:
    Manager(Manager const&)            = delete;
    Manager& operator=(Manager const&) = delete;

    using ModuleFactory = std::function<std::unique_ptr<ModuleBase>()>;

    struct ModuleStatus {
        std::string name;
        std::string description;
        bool        loaded{};
        bool        enabled{};
    };

    static auto getInstance() -> Manager&;

    [[nodiscard]] auto initialize() -> Expected<bool>;
    [[nodiscard]] auto destroy() -> Expected<void>;

    template <typename T>
    [[nodiscard]] auto registerModule(std::string name, std::string description) -> Expected<void> {
        return registerModule<T>(std::move(name), std::move(description), [] { return std::make_unique<T>(); });
    }

    template <typename T>
    [[nodiscard]] auto registerModule(std::string name, std::string description, ModuleFactory factory)
            -> Expected<void> {
        static_assert(std::is_base_of_v<ModuleBase, T>, "Registered module must inherit from ModuleBase");

        std::lock_guard lock(mMutex);
        auto            type = std::type_index(typeid(T));
        if (mModules.contains(type)) {
            return unexpected(ErrorCategory::Module,
                              CommonErrorCode::AlreadyExists,
                              "Module type already registered: " + std::string(type.name()));
        }

        auto [nameIt, inserted] = mModuleNameMap.try_emplace(name, type);
        if (!inserted) {
            return unexpected(
                    ErrorCategory::Module, CommonErrorCode::AlreadyExists, "Module already registered: " + name);
        }

        try {
            mModules.emplace(
                    type, std::make_shared<ModuleInfo<T>>(std::move(factory), std::move(name), std::move(description)));
            mModuleOrder.push_back(type);
        } catch (...) {
            mModuleNameMap.erase(nameIt);
            return unexpected(fromCurrentException(ErrorCategory::Module));
        }
        return {};
    }

    [[nodiscard]] auto loadAll() -> Expected<void>;
    [[nodiscard]] auto enableAll() -> Expected<void>;
    [[nodiscard]] auto disableAll() -> Expected<void>;
    [[nodiscard]] auto unloadAll() -> Expected<void>;

    template <typename T>
    [[nodiscard]] auto loadModule() -> Expected<void> {
        std::shared_ptr<ModuleInfoBase> info;
        {
            std::lock_guard lock(mMutex);
            info = findInfoLocked(std::type_index(typeid(T)));
        }
        return loadModuleImpl(info);
    }

    [[nodiscard]] auto loadModule(std::string_view name) -> Expected<void>;

    template <typename T>
    [[nodiscard]] auto enableModule() -> Expected<void> {
        std::shared_ptr<ModuleInfoBase> info;
        {
            std::lock_guard lock(mMutex);
            info = findInfoLocked(std::type_index(typeid(T)));
        }
        return enableModuleImpl(info);
    }

    [[nodiscard]] auto enableModule(std::string_view name) -> Expected<void>;

    template <typename T>
    [[nodiscard]] auto disableModule() -> Expected<void> {
        std::shared_ptr<ModuleInfoBase> info;
        {
            std::lock_guard lock(mMutex);
            info = findInfoLocked(std::type_index(typeid(T)));
        }
        return disableModuleImpl(info);
    }

    [[nodiscard]] auto disableModule(std::string_view name) -> Expected<void>;

    template <typename T>
    [[nodiscard]] auto unloadModule() -> Expected<void> {
        std::shared_ptr<ModuleInfoBase> info;
        {
            std::lock_guard lock(mMutex);
            info = findInfoLocked(std::type_index(typeid(T)));
        }
        return unloadModuleImpl(info);
    }

    [[nodiscard]] auto unloadModule(std::string_view name) -> Expected<void>;

    template <typename T>
    auto getModule() const -> std::shared_ptr<T> {
        std::lock_guard lock(mMutex);
        auto const      info = const_cast<Manager*>(this)->findInfoLocked(std::type_index(typeid(T)));
        if (!info) {
            return nullptr;
        }
        return std::static_pointer_cast<T>(info->get());
    }

    template <typename T>
    [[nodiscard]] auto requireModule() const -> Expected<ModuleRef<T>> {
        auto module = getModule<T>();
        if (!module) {
            return unexpected(ErrorCategory::Module,
                              CommonErrorCode::NotLoaded,
                              "Module is not loaded: " + std::string(typeid(T).name()));
        }
        return ModuleRef<T>(std::move(module));
    }

    [[nodiscard]] auto modules() const -> std::vector<ModuleStatus>;
    [[nodiscard]] auto moduleInfo(std::string_view name) const -> std::optional<ModuleStatus>;

private:
    Manager() = default;
    ~Manager();

    struct ModuleInfoBase;

    template <typename T>
    struct ModuleInfo;

    [[nodiscard]] auto findInfoLocked(std::type_index const& type) -> std::shared_ptr<ModuleInfoBase>;
    [[nodiscard]] auto findInfoLocked(std::string_view name) -> std::shared_ptr<ModuleInfoBase>;

    [[nodiscard]] static auto loadModuleImpl(std::shared_ptr<ModuleInfoBase> const& info) -> Expected<void>;
    [[nodiscard]] static auto unloadModuleImpl(std::shared_ptr<ModuleInfoBase> const& info) -> Expected<void>;
    [[nodiscard]] static auto enableModuleImpl(std::shared_ptr<ModuleInfoBase> const& info) -> Expected<void>;
    [[nodiscard]] static auto disableModuleImpl(std::shared_ptr<ModuleInfoBase> const& info) -> Expected<void>;

    struct ModuleInfoBase {
        virtual ~ModuleInfoBase() = default;

        [[nodiscard]] virtual auto load() -> Expected<void>    = 0;
        [[nodiscard]] virtual auto unload() -> Expected<void>  = 0;
        [[nodiscard]] virtual auto enable() -> Expected<void>  = 0;
        [[nodiscard]] virtual auto disable() -> Expected<void> = 0;

        [[nodiscard]] virtual bool isLoaded() const  = 0;
        [[nodiscard]] virtual bool isEnabled() const = 0;

        [[nodiscard]] virtual auto name() const -> std::string const&        = 0;
        [[nodiscard]] virtual auto description() const -> std::string const& = 0;

        virtual auto get() -> std::shared_ptr<ModuleBase> = 0;
    };

    template <typename T>
    struct ModuleInfo final : ModuleInfoBase {
        static_assert(std::is_base_of_v<ModuleBase, T>, "T must inherit from ModuleBase");

        ModuleFactory                factory;
        std::shared_ptr<ModuleBase>  instance;
        std::string                  moduleName;
        std::string                  moduleDescription;
        std::shared_ptr<ModuleBase>  instancePtr;
        mutable std::mutex           instancePtrMutex;
        mutable std::recursive_mutex lifecycleMutex;

        ModuleInfo(ModuleFactory factory, std::string name, std::string description)
            : factory(std::move(factory)), moduleName(std::move(name)), moduleDescription(std::move(description)) {}

        [[nodiscard]] auto load() -> Expected<void> override {
            std::lock_guard lock(lifecycleMutex);
            if (instance) {
                return {};
            }

            instance = factory();
            if (!instance) {
                return unexpected(
                        ErrorCategory::Module, CommonErrorCode::OperationFailed, "Module factory returned null");
            }
            if (auto result = instance->onLoad(); !result) {
                instance.reset();
                return unexpected(result.error());
            }
            instance->mState.store(ModuleState::Loaded);
            {
                std::lock_guard instanceLock(instancePtrMutex);
                instancePtr = instance;
            }
            return {};
        }

        [[nodiscard]] auto unload() -> Expected<void> override {
            std::lock_guard lock(lifecycleMutex);
            if (!instance) {
                return {};
            }

            if (instance->state() == ModuleState::Enabled) {
                if (auto result = disable(); !result) {
                    return result;
                }
            }

            if (auto result = instance->onUnload(); !result) {
                return result;
            }
            // Stop publishing the instance before destroying it so concurrent get() readers see null.
            {
                std::lock_guard instanceLock(instancePtrMutex);
                instancePtr.reset();
            }
            instance->mState.store(ModuleState::Registered);
            instance.reset();
            return {};
        }

        [[nodiscard]] auto enable() -> Expected<void> override {
            std::lock_guard lock(lifecycleMutex);
            if (!instance) {
                return unexpected(
                        ErrorCategory::Module, CommonErrorCode::NotLoaded, "Module is not loaded: " + moduleName);
            }
            if (instance->state() == ModuleState::Enabled) {
                return {};
            }

            // Publish Enabled before onEnable() so the module observes itself enabled during start-up.
            instance->mState.store(ModuleState::Enabled);
            if (auto result = instance->onEnable(); !result) {
                instance->mState.store(ModuleState::Loaded);
                return unexpected(result.error());
            }
            return {};
        }

        [[nodiscard]] auto disable() -> Expected<void> override {
            std::lock_guard lock(lifecycleMutex);
            if (!instance || instance->state() != ModuleState::Enabled) {
                return {};
            }

            instance->mState.store(ModuleState::Loaded);
            if (auto result = instance->onDisable(); !result) {
                instance->mState.store(ModuleState::Enabled);
                return unexpected(result.error());
            }
            return {};
        }

        [[nodiscard]] bool isLoaded() const override {
            std::lock_guard instanceLock(instancePtrMutex);
            return instancePtr != nullptr;
        }

        [[nodiscard]] bool isEnabled() const override {
            std::shared_ptr<ModuleBase> module;
            {
                std::lock_guard instanceLock(instancePtrMutex);
                module = instancePtr;
            }
            return module != nullptr && module->state() == ModuleState::Enabled;
        }

        [[nodiscard]] auto name() const -> std::string const& override {
            return moduleName;
        }

        [[nodiscard]] auto description() const -> std::string const& override {
            return moduleDescription;
        }

        auto get() -> std::shared_ptr<ModuleBase> override {
            std::lock_guard instanceLock(instancePtrMutex);
            return instancePtr;
        }
    };

    bool                                                                 mInitialized{false};
    bool                                                                 mDestroyed{false};
    std::unordered_map<std::type_index, std::shared_ptr<ModuleInfoBase>> mModules;
    std::unordered_map<std::string, std::type_index>                     mModuleNameMap;
    std::vector<std::type_index>                                         mModuleOrder;
    mutable std::recursive_mutex                                         mMutex;
};

} // namespace CloverNT::Core::Modules
