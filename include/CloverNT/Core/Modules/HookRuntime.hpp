#pragma once

#include <CloverNT/API/Memory/Memory.hpp>
#include <CloverNT/Core/Modules/ModuleBase.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

namespace CloverNT::Core::Modules {

class HookRuntime final : public ModuleBase {
public:
    class RuntimeResource;

    HookRuntime();
    ~HookRuntime() override;

    [[nodiscard]] auto onLoad() -> Expected<void> override;
    [[nodiscard]] auto onUnload() -> Expected<void> override;
    [[nodiscard]] auto onEnable() -> Expected<void> override;
    [[nodiscard]] auto onDisable() -> Expected<void> override;

    [[nodiscard]] auto createInlineHook(Memory::Address                                  target,
                                        Memory::Address                                  detour,
                                        int                                              priority,
                                        void**                                           originalSlot,
                                        const std::weak_ptr<Plugin::Plugin>&             owner,
                                        std::shared_ptr<Memory::Detail::ResourcePayload> payload)
            -> Expected<Memory::ResourceId>;
    [[nodiscard]] auto createImportHook(std::string_view                                 moduleName,
                                        std::string_view                                 importModule,
                                        std::string_view                                 functionName,
                                        Memory::Address                                  detour,
                                        void**                                           originalSlot,
                                        const std::weak_ptr<Plugin::Plugin>&             owner,
                                        std::shared_ptr<Memory::Detail::ResourcePayload> payload)
            -> Expected<Memory::ResourceId>;
    [[nodiscard]] auto createPatch(Memory::Address                address,
                                   std::vector<std::uint8_t>      bytes,
                                   const Memory::ResourceOptions& options) -> Expected<Memory::PatchHandle>;
    [[nodiscard]] auto createNopPatch(Memory::Address address, std::size_t size, const Memory::ResourceOptions& options)
            -> Expected<Memory::PatchHandle>;

    [[nodiscard]] auto enableResource(Memory::ResourceId id) -> Expected<void>;
    [[nodiscard]] auto disableResource(Memory::ResourceId id) -> Expected<void>;
    [[nodiscard]] auto removeResource(Memory::ResourceId id) -> Expected<void>;
    [[nodiscard]] bool isResourceEnabled(Memory::ResourceId id);

    [[nodiscard]] auto suspendPluginResources(std::string_view pluginName) -> Expected<void>;
    [[nodiscard]] auto resumePluginResources(std::string_view pluginName) -> Expected<void>;
    [[nodiscard]] auto clearPluginResources(std::string_view pluginName) -> Expected<void>;

private:
    [[nodiscard]] static auto resolveOwner(const std::weak_ptr<Plugin::Plugin>& owner) -> Expected<std::string>;
    [[nodiscard]] auto        addResource(std::unique_ptr<RuntimeResource> resource) -> Expected<Memory::ResourceId>;
    [[nodiscard]] auto        findResource(Memory::ResourceId id) -> RuntimeResource*;
    [[nodiscard]] auto        removeResourceLocked(Memory::ResourceId id) -> Expected<void>;

    mutable std::recursive_mutex                                     mMutex;
    std::map<Memory::ResourceId, std::unique_ptr<RuntimeResource>>   mResources;
    std::map<std::string, std::set<Memory::ResourceId>, std::less<>> mPluginResources;
    Memory::ResourceId                                               mNextId{1};
};

} // namespace CloverNT::Core::Modules
