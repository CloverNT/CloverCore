#include <CloverNT/API/Plugin/PluginBase.hpp>

#include <CloverNT/API/Exception.hpp>
#include <CloverNT/API/Logger.hpp>
#include <CloverNT/API/Plugin/NativePlugin.hpp>

namespace CloverNT::Plugin {

void PluginBase::bindSelf(NativePlugin& plugin) noexcept {
    mSelf = &plugin;
}

void PluginBase::clearSelf() noexcept {
    mSelf = nullptr;
}

NativePlugin& PluginBase::self() const {
    if (mSelf == nullptr) {
        throw InvalidStateException("PluginBase used before it was bound to a plugin");
    }
    return *mSelf;
}

Logger& PluginBase::logger() const {
    return *LoggerRegistry::getInstance().getOrCreate(name());
}

std::string_view PluginBase::name() const {
    return self().name();
}

std::string_view PluginBase::type() const {
    return self().type();
}

Manifest const& PluginBase::manifest() const {
    return self().manifest();
}

std::filesystem::path const& PluginBase::directory() const {
    return self().directory();
}

std::filesystem::path PluginBase::dataDir() const {
    return self().directory() / "data";
}

std::filesystem::path PluginBase::configDir() const {
    return self().directory() / "config";
}

} // namespace CloverNT::Plugin
