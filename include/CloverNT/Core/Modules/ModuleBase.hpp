#pragma once

#include <CloverNT/API/Expected.hpp>

#include <atomic>

namespace CloverNT::Core::Modules {

enum class ModuleState {
    Registered,
    Loaded,
    Enabled,
};

class ModuleBase {
public:
    friend class Manager;

    virtual ~ModuleBase() = default;

    [[nodiscard]] virtual auto onLoad() -> Expected<void> {
        return {};
    }

    [[nodiscard]] virtual auto onUnload() -> Expected<void> {
        return {};
    }

    [[nodiscard]] virtual auto onEnable() -> Expected<void>  = 0;
    [[nodiscard]] virtual auto onDisable() -> Expected<void> = 0;

    [[nodiscard]] ModuleState state() const noexcept {
        return mState.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool isLoaded() const noexcept {
        return state() >= ModuleState::Loaded;
    }

    [[nodiscard]] bool isEnabled() const noexcept {
        return state() == ModuleState::Enabled;
    }

protected:
    std::atomic<ModuleState> mState{ModuleState::Registered};
};

} // namespace CloverNT::Core::Modules
