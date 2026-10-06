#pragma once
#include <filesystem>

#include <CloverNT/API/Plugin/Plugin.hpp>
#include <CloverNT/API/Utils/DynamicLibrary.hpp>

namespace CloverNT::Core::Modules {
class PluginRuntime;
}

namespace CloverNT::Plugin {

class CloverNT_API NativePlugin : public Plugin {
public:
    using Callback = bool (*)(NativePlugin&);
    using UnloadFn = bool (*)();

    NativePlugin(Manifest manifest, std::filesystem::path directory, std::filesystem::path entryPath);
    ~NativePlugin() override;

    [[nodiscard]] auto  directory() const noexcept -> std::filesystem::path const&;
    [[nodiscard]] auto  entryPath() const noexcept -> std::filesystem::path const&;
    [[nodiscard]] void* nativeHandle() const noexcept;

    [[nodiscard]] bool hasUnloadHandler() const noexcept {
        return mUnloadFn != nullptr;
    }
    [[nodiscard]] bool invokeUnloadHandler() const {
        return mUnloadFn != nullptr ? mUnloadFn() : true;
    }

private:
    std::filesystem::path mDirectory;
    std::filesystem::path mEntryPath;
    Utils::DynamicLibrary mLibrary;
    Callback              mLoadCallback{};
    UnloadFn              mUnloadFn{};

    friend void setUnloadHandler(NativePlugin& plugin, UnloadFn fn) noexcept;
    friend class PluginManager;
    friend class Core::Modules::PluginRuntime;
};

inline void setUnloadHandler(NativePlugin& plugin, const NativePlugin::UnloadFn fn) noexcept {
    plugin.mUnloadFn = fn;
}

using NativePluginPtr = std::shared_ptr<NativePlugin>;

} // namespace CloverNT::Plugin
