#pragma once

#include <atomic>
#include <memory>
#include <string_view>

#include <CloverNT/API/Macros.hpp>
#include <CloverNT/API/Plugin/Manifest.hpp>

namespace CloverNT::Core::Modules {
class PluginRuntime;
}

namespace CloverNT::Plugin {

enum class PluginState {
    Unloaded,
    Loading,
    Loaded,
    Unloading,
    Failed,
};

class CloverNT_API Plugin {
public:
    explicit Plugin(Manifest manifest, bool unloadable, bool builtin = false);
    virtual ~Plugin();

    Plugin(Plugin const&)            = delete;
    Plugin& operator=(Plugin const&) = delete;
    Plugin(Plugin&&)                 = delete;
    Plugin& operator=(Plugin&&)      = delete;

    [[nodiscard]] auto manifest() const noexcept -> Manifest const&;
    [[nodiscard]] auto name() const noexcept -> std::string_view;
    [[nodiscard]] auto type() const noexcept -> std::string_view;
    [[nodiscard]] bool isUnloadable() const noexcept;
    [[nodiscard]] bool isBuiltin() const noexcept;
    [[nodiscard]] auto state() const noexcept -> PluginState;

protected:
    void setState(PluginState state) noexcept;
    void setUnloadable(bool unloadable) noexcept;

private:
    Manifest                 mManifest;
    bool                     mUnloadable{false};
    bool                     mBuiltin{false};
    std::atomic<PluginState> mState{PluginState::Unloaded};

    friend class PluginManager;
    friend class Core::Modules::PluginRuntime;
};

using PluginPtr = std::shared_ptr<Plugin>;

} // namespace CloverNT::Plugin
