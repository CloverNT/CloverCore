#include <CloverNT/API/Plugin/Plugin.hpp>

#include <utility>

namespace CloverNT::Plugin {

Plugin::Plugin(Manifest manifest, const bool unloadable, const bool builtin)
    : mManifest(std::move(manifest)), mUnloadable(unloadable), mBuiltin(builtin) {}

Plugin::~Plugin() = default;

auto Plugin::manifest() const noexcept -> Manifest const& {
    return mManifest;
}

auto Plugin::name() const noexcept -> std::string_view {
    return mManifest.name;
}

auto Plugin::type() const noexcept -> std::string_view {
    return mManifest.type;
}
bool Plugin::isUnloadable() const noexcept {
    return mUnloadable;
}

bool Plugin::isBuiltin() const noexcept {
    return mBuiltin;
}

auto Plugin::state() const noexcept -> PluginState {
    return mState.load(std::memory_order_acquire);
}

void Plugin::setState(const PluginState state) noexcept {
    mState.store(state, std::memory_order_release);
}

void Plugin::setUnloadable(const bool unloadable) noexcept {
    mUnloadable = unloadable;
}

} // namespace CloverNT::Plugin
