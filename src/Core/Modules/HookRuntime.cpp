#include <CloverNT/API/Plugin/PluginManager.hpp>
#include <CloverNT/Core/Modules/HookRuntime.hpp>
#include <CloverNT/Runtime/Kernel.h>

#include <ranges>
#include <utility>

#include <Mortis/Detail/ImportHookImpl.hpp>
#include <Mortis/Process.hpp>

namespace CloverNT::Core::Modules {
namespace {

    [[nodiscard]] auto toErrorCode(const Mortis::ErrorCode code) noexcept -> Memory::ErrorCode {
        switch (code) {
        case Mortis::ErrorCode::MemoryNotReadable:
            return Memory::ErrorCode::MemoryNotReadable;
        case Mortis::ErrorCode::MemoryNotWritable:
            return Memory::ErrorCode::MemoryNotWritable;
        case Mortis::ErrorCode::ProtectionFailed:
            return Memory::ErrorCode::ProtectionFailed;
        case Mortis::ErrorCode::HookInstallFailed:
            return Memory::ErrorCode::HookInstallFailed;
        case Mortis::ErrorCode::HookRemoveFailed:
            return Memory::ErrorCode::HookRemoveFailed;
        case Mortis::ErrorCode::NoFreeSlots:
            return Memory::ErrorCode::NoFreeSlots;
        case Mortis::ErrorCode::ImportNotFound:
            return Memory::ErrorCode::ImportNotFound;
        case Mortis::ErrorCode::ModuleNotFound:
            return Memory::ErrorCode::ModuleNotFound;
        case Mortis::ErrorCode::InvalidArgument:
            return Memory::ErrorCode::InvalidArgument;
        case Mortis::ErrorCode::Unknown:
        default:
            return Memory::ErrorCode::Unknown;
        }
    }

    [[nodiscard]] auto fromMortis(Mortis::Result<void> const& result) -> Expected<void> {
        if (result) {
            return {};
        }
        return unexpected(Memory::makeMemoryError(toErrorCode(result.code()), result.error()));
    }

    void appendError(std::string& errors, const std::string& message) {
        if (!errors.empty()) {
            errors += '\n';
        }
        errors += message;
    }

    [[nodiscard]] auto makeNopBytes(const std::size_t count) -> std::vector<std::uint8_t> {
        std::vector<std::uint8_t> nops;
#if defined(CloverNT_ARCH_X64)
        nops.assign(count, 0x90);
#elif defined(CloverNT_ARCH_ARM64)
        const auto nopCount = count / 4;
        nops.resize(nopCount * 4);
        for (std::size_t i = 0; i < nopCount; ++i) {
            nops[i * 4 + 0] = 0x1F;
            nops[i * 4 + 1] = 0x20;
            nops[i * 4 + 2] = 0x03;
            nops[i * 4 + 3] = 0xD5;
        }
#else
    #error "CloverNT: unsupported architecture for NOP patch encoding"
#endif
        return nops;
    }

} // namespace

class HookRuntime::RuntimeResource {
public:
    RuntimeResource(std::string                                      owner,
                    std::shared_ptr<Memory::Detail::ResourcePayload> payload,
                    const bool                                       enabled = false)
        : mOwner(std::move(owner)), mPayload(std::move(payload)), mEnabled(enabled) {}

    virtual ~RuntimeResource() = default;

    RuntimeResource(RuntimeResource const&)            = delete;
    RuntimeResource& operator=(RuntimeResource const&) = delete;

    [[nodiscard]] auto owner() const noexcept -> std::string const& {
        return mOwner;
    }

    [[nodiscard]] bool isEnabled() const noexcept {
        return mEnabled;
    }

    [[nodiscard]] bool isSuspended() const noexcept {
        return mSuspended;
    }

    void setSuspended(const bool suspended) noexcept {
        mSuspended = suspended;
    }

    auto enable() -> Expected<void> {
        if (mRemoved) {
            return unexpected(Memory::makeMemoryError(Memory::ErrorCode::InvalidHandle, "Resource has been removed"));
        }
        if (mEnabled) {
            return {};
        }

        if (auto result = enableImpl(); !result) {
            return result;
        }
        mEnabled = true;
        return {};
    }

    auto disable() -> Expected<void> {
        if (mRemoved || !mEnabled) {
            return {};
        }

        if (auto result = disableImpl(); !result) {
            return result;
        }
        mEnabled = false;
        return {};
    }

    auto remove() -> Expected<void> {
        if (mRemoved) {
            return {};
        }

        if (mEnabled) {
            if (auto result = disableImpl(); !result) {
                return result;
            }
            mEnabled = false;
        }

        if (mPayload) {
            mPayload->quiesce();
        }

        mRemoved   = true;
        mSuspended = false;
        mPayload.reset();
        return {};
    }

private:
    virtual auto enableImpl() -> Expected<void>  = 0;
    virtual auto disableImpl() -> Expected<void> = 0;

    std::string                                      mOwner;
    std::shared_ptr<Memory::Detail::ResourcePayload> mPayload;
    bool                                             mEnabled{};
    bool                                             mRemoved{};
    bool                                             mSuspended{};
};

namespace {

    class InlineHookResource final : public HookRuntime::RuntimeResource {
    public:
        InlineHookResource(std::string                                      owner,
                           std::shared_ptr<Memory::Detail::ResourcePayload> payload,
                           const Memory::Address                            target,
                           const Memory::Address                            detour,
                           const int                                        priority,
                           void**                                           originalSlot)
            : RuntimeResource(std::move(owner), std::move(payload)),
              mTarget(reinterpret_cast<void*>(target)),
              mDetour(reinterpret_cast<void*>(detour)),
              mPriority(priority),
              mOriginalSlot(originalSlot) {}

    private:
        auto enableImpl() -> Expected<void> override {
            const CloverHook handle =
                    CloverHookInline(reinterpret_cast<CloverAddress>(mTarget), mDetour, mOriginalSlot);
            if (handle == 0) {
                return unexpected(
                        Memory::makeMemoryError(Memory::ErrorCode::HookInstallFailed, "Inline hook install failed"));
            }
            mHandle = handle;
            return {};
        }

        auto disableImpl() -> Expected<void> override {
            if (mHandle == 0) {
                return {};
            }
            const bool ok = CloverHookRemove(mHandle) != 0;
            mHandle       = 0;
            if (!ok) {
                return unexpected(
                        Memory::makeMemoryError(Memory::ErrorCode::HookRemoveFailed, "Inline hook remove failed"));
            }
            return {};
        }

        void*                mTarget{};
        void*                mDetour{};
        [[maybe_unused]] int mPriority{};
        void**               mOriginalSlot{};
        CloverHook           mHandle{0};
    };

    class ImportHookResource final : public HookRuntime::RuntimeResource {
    public:
        ImportHookResource(std::string                                      owner,
                           std::shared_ptr<Memory::Detail::ResourcePayload> payload,
                           std::string                                      moduleName,
                           std::string                                      importModule,
                           std::string                                      functionName,
                           const Memory::Address                            detour,
                           void**                                           originalSlot)
            : RuntimeResource(std::move(owner), std::move(payload)),
              mModuleName(std::move(moduleName)),
              mImportModule(std::move(importModule)),
              mFunctionName(std::move(functionName)),
              mDetour(reinterpret_cast<void*>(detour)),
              mOriginalSlot(originalSlot) {}

    private:
        auto enableImpl() -> Expected<void> override {
            void*      previous = nullptr;
            const auto result   = Mortis::ImportHookImpl::PatchImportEntry(
                    mModuleName, mImportModule, mFunctionName, mDetour, &previous);
            if (!result) {
                auto        code    = toErrorCode(result.code());
                std::string message = result.error();
                if (previous != nullptr) {
                    if (const auto rollback = Mortis::ImportHookImpl::UnpatchImportEntry(
                                mModuleName, mImportModule, mFunctionName, previous);
                        !rollback) {
                        code = Memory::ErrorCode::HookInstallFailed;
                        appendError(message, "Rollback failed after import patch failure: " + rollback.error());
                    }
                }
                return unexpected(Memory::makeMemoryError(code, std::move(message)));
            }
            if (mOriginal == nullptr) {
                mOriginal = previous;
            }
            if (mOriginalSlot != nullptr) {
                *mOriginalSlot = mOriginal;
            }
            return {};
        }

        auto disableImpl() -> Expected<void> override {
            if (mOriginal == nullptr) {
                return {};
            }
            if (const auto result = Mortis::ImportHookImpl::UnpatchImportEntry(
                        mModuleName, mImportModule, mFunctionName, mOriginal);
                !result) {
                return unexpected(Memory::makeMemoryError(toErrorCode(result.code()), result.error()));
            }
            return {};
        }

        std::string mModuleName;
        std::string mImportModule;
        std::string mFunctionName;
        void*       mDetour{};
        void*       mOriginal{};
        void**      mOriginalSlot{};
    };

    class PatchResource final : public HookRuntime::RuntimeResource {
    public:
        PatchResource(std::string owner, const Memory::Address address, std::vector<std::uint8_t> bytes)
            : RuntimeResource(std::move(owner), nullptr), mAddress(address), mBytes(std::move(bytes)) {}

        PatchResource(std::string owner, const Memory::Address address, const std::size_t nopSize)
            : RuntimeResource(std::move(owner), nullptr), mAddress(address), mBytes(makeNopBytes(nopSize)) {}

    private:
        auto enableImpl() -> Expected<void> override {
            if (mBytes.empty()) {
                return {};
            }

            if (!mOriginalCaptured) {
                mOriginalBytes.resize(mBytes.size());
                if (const auto read =
                            Mortis::Process::ReadMemory(mOriginalBytes.data(), mAddress, mOriginalBytes.size());
                    !read) {
                    mOriginalBytes.clear();
                    return unexpected(Memory::makeMemoryError(toErrorCode(read.code()), read.error()));
                }
                mOriginalCaptured = true;
            }

            if (const auto write = Mortis::Process::WriteMemory(mAddress, mBytes.data(), mBytes.size()); !write) {
                auto        code    = toErrorCode(write.code());
                std::string message = write.error();
                if (const auto restored =
                            Mortis::Process::WriteMemory(mAddress, mOriginalBytes.data(), mOriginalBytes.size());
                    !restored) {
                    code = Memory::ErrorCode::HookInstallFailed;
                    appendError(message, "Rollback failed after patch write failure: " + restored.error());
                }
                return unexpected(Memory::makeMemoryError(code, std::move(message)));
            }

            return {};
        }

        auto disableImpl() -> Expected<void> override {
            if (!mOriginalCaptured || mOriginalBytes.empty()) {
                return {};
            }
            return fromMortis(Mortis::Process::WriteMemory(mAddress, mOriginalBytes.data(), mOriginalBytes.size()));
        }

        Memory::Address           mAddress{};
        std::vector<std::uint8_t> mBytes;
        std::vector<std::uint8_t> mOriginalBytes;
        bool                      mOriginalCaptured{};
    };

} // namespace

HookRuntime::HookRuntime() = default;

HookRuntime::~HookRuntime() = default;

auto HookRuntime::onLoad() -> Expected<void> {
    std::lock_guard lock(mMutex);
    mResources.clear();
    mPluginResources.clear();
    mNextId = 1;
    return {};
}

auto HookRuntime::onUnload() -> Expected<void> {
    std::lock_guard lock(mMutex);

    std::string errors;
    for (auto& [id, resource]: std::views::reverse(mResources)) {
        if (auto result = resource->remove(); !result) {
            appendError(errors, "Resource " + std::to_string(id) + ": " + result.error().message);
        }
    }

    if (!errors.empty()) {
        return unexpected(Memory::makeMemoryError(Memory::ErrorCode::HookRemoveFailed, std::move(errors)));
    }

    mResources.clear();
    mPluginResources.clear();
    mNextId = 1;
    return {};
}

auto HookRuntime::onEnable() -> Expected<void> {
    return {};
}

auto HookRuntime::onDisable() -> Expected<void> {
    std::lock_guard lock(mMutex);

    std::vector<Memory::ResourceId> disabled;
    for (auto const& [id, resource]: std::views::reverse(mResources)) {
        if (!resource->isEnabled()) {
            continue;
        }

        if (auto result = resource->disable(); !result) {
            std::string rollbackErrors;
            for (const auto resumeId: std::views::reverse(disabled)) {
                if (auto* toResume = findResource(resumeId); toResume != nullptr) {
                    if (auto resumed = toResume->enable(); !resumed) {
                        if (!rollbackErrors.empty()) {
                            rollbackErrors += '\n';
                        }
                        rollbackErrors += "Resource " + std::to_string(resumeId) + ": " + resumed.error().message;
                    }
                }
            }

            if (!rollbackErrors.empty()) {
                return unexpected(Memory::makeMemoryError(Memory::ErrorCode::HookInstallFailed,
                                                          "Failed to disable resource " + std::to_string(id) + ": " +
                                                                  result.error().message + "\nRollback failed:\n" +
                                                                  rollbackErrors));
            }

            return unexpected(result.error());
        }

        disabled.emplace_back(id);
    }

    return {};
}

auto HookRuntime::resolveOwner(const std::weak_ptr<Plugin::Plugin>& owner) -> Expected<std::string> {
    auto plugin = owner.lock();
    if (!plugin) {
        plugin = Plugin::PluginManager::currentPlugin();
    }
    if (!plugin) {
        return unexpected(
                Memory::makeMemoryError(Memory::ErrorCode::PluginUnavailable, "No current plugin is available"));
    }
    return std::string(plugin->name());
}

auto HookRuntime::addResource(std::unique_ptr<RuntimeResource> resource) -> Expected<Memory::ResourceId> {
    std::lock_guard lock(mMutex);
    if (!resource) {
        return unexpected(Memory::makeMemoryError(Memory::ErrorCode::InvalidArgument, "Resource is null"));
    }
    if (!isEnabled()) {
        return unexpected(Memory::makeMemoryError(Memory::ErrorCode::RuntimeInactive, "Memory runtime is not active"));
    }

    const auto id    = mNextId++;
    const auto owner = resource->owner();
    auto*      raw   = resource.get();
    try {
        mResources.emplace(id, std::move(resource));
        mPluginResources[owner].emplace(id);
    } catch (...) {
        mResources.erase(id);
        if (const auto ownerIt = mPluginResources.find(owner); ownerIt != mPluginResources.end()) {
            ownerIt->second.erase(id);
            if (ownerIt->second.empty()) {
                mPluginResources.erase(ownerIt);
            }
        }
        return unexpected(fromCurrentException(ErrorCategory::Memory));
    }

    auto rollbackAfterEnableFailure = [&](Error const& enableError,
                                          std::string  prefix) -> Expected<Memory::ResourceId> {
        std::string rollbackErrors;
        if (auto removed = removeResourceLocked(id); !removed) {
            appendError(rollbackErrors, removed.error().message);
        }

        if (!rollbackErrors.empty()) {
            return unexpected(Memory::makeMemoryError(Memory::ErrorCode::HookInstallFailed,
                                                      std::move(prefix) + enableError.message + "\nRollback failed:\n" +
                                                              rollbackErrors));
        }
        return unexpected(enableError);
    };

    try {
        if (auto enabled = raw->enable(); !enabled) {
            return rollbackAfterEnableFailure(enabled.error(),
                                              "Failed to enable resource " + std::to_string(id) + ": ");
        }
    } catch (...) {
        return rollbackAfterEnableFailure(fromCurrentException(ErrorCategory::Memory),
                                          "Resource " + std::to_string(id) + " enable threw: ");
    }
    return id;
}


auto HookRuntime::findResource(const Memory::ResourceId id) -> RuntimeResource* {
    const auto it = mResources.find(id);
    return it == mResources.end() ? nullptr : it->second.get();
}

auto HookRuntime::removeResourceLocked(const Memory::ResourceId id) -> Expected<void> {
    const auto it = mResources.find(id);
    if (it == mResources.end()) {
        return unexpected(Memory::makeMemoryError(Memory::ErrorCode::InvalidHandle, "Invalid resource handle"));
    }

    const auto owner = it->second->owner();
    if (const auto result = it->second->remove(); !result) {
        return unexpected(result.error());
    }

    if (const auto ownerIt = mPluginResources.find(owner); ownerIt != mPluginResources.end()) {
        ownerIt->second.erase(id);
        if (ownerIt->second.empty()) {
            mPluginResources.erase(ownerIt);
        }
    }
    mResources.erase(it);
    return {};
}

auto HookRuntime::createInlineHook(Memory::Address                                  target,
                                   Memory::Address                                  detour,
                                   int                                              priority,
                                   void**                                           originalSlot,
                                   const std::weak_ptr<Plugin::Plugin>&             owner,
                                   std::shared_ptr<Memory::Detail::ResourcePayload> payload)
        -> Expected<Memory::ResourceId> {
    if (target == 0 || detour == 0 || originalSlot == nullptr || !payload) {
        return unexpected(Memory::makeMemoryError(Memory::ErrorCode::InvalidArgument, "Invalid inline hook arguments"));
    }

    auto ownerName = resolveOwner(owner);
    if (!ownerName) {
        return unexpected(ownerName.error());
    }

    auto resource = std::make_unique<InlineHookResource>(
            ownerName.value(), std::move(payload), target, detour, priority, originalSlot);
    return addResource(std::move(resource));
}

auto HookRuntime::createImportHook(const std::string_view                           moduleName,
                                   const std::string_view                           importModule,
                                   const std::string_view                           functionName,
                                   Memory::Address                                  detour,
                                   void**                                           originalSlot,
                                   const std::weak_ptr<Plugin::Plugin>&             owner,
                                   std::shared_ptr<Memory::Detail::ResourcePayload> payload)
        -> Expected<Memory::ResourceId> {
    if (importModule.empty() || functionName.empty() || detour == 0 || originalSlot == nullptr || !payload) {
        return unexpected(Memory::makeMemoryError(Memory::ErrorCode::InvalidArgument, "Invalid import hook arguments"));
    }

    auto ownerName = resolveOwner(owner);
    if (!ownerName) {
        return unexpected(ownerName.error());
    }

    auto resource = std::make_unique<ImportHookResource>(ownerName.value(),
                                                         std::move(payload),
                                                         std::string(moduleName),
                                                         std::string(importModule),
                                                         std::string(functionName),
                                                         detour,
                                                         originalSlot);
    return addResource(std::move(resource));
}

auto HookRuntime::createPatch(Memory::Address                address,
                              std::vector<std::uint8_t>      bytes,
                              const Memory::ResourceOptions& options) -> Expected<Memory::PatchHandle> {
    auto owner = resolveOwner(options.owner);
    if (!owner) {
        return unexpected(owner.error());
    }

    auto id = addResource(std::make_unique<PatchResource>(owner.value(), address, std::move(bytes)));
    if (!id) {
        return unexpected(id.error());
    }
    return Memory::PatchHandle(id.value());
}

auto HookRuntime::createNopPatch(Memory::Address address, std::size_t size, const Memory::ResourceOptions& options)
        -> Expected<Memory::PatchHandle> {
    if (size == 0) {
        return unexpected(Memory::makeMemoryError(Memory::ErrorCode::InvalidArgument,
                                                  "NOP patch size must be greater than zero"));
    }
#if defined(CloverNT_ARCH_ARM64)
    if (size % 4 != 0) {
        return unexpected(Memory::makeMemoryError(Memory::ErrorCode::InvalidArgument,
                                                  "ARM64 NOP patch size must be a multiple of 4 bytes"));
    }
#endif

    auto owner = resolveOwner(options.owner);
    if (!owner) {
        return unexpected(owner.error());
    }

    auto id = addResource(std::make_unique<PatchResource>(owner.value(), address, size));
    if (!id) {
        return unexpected(id.error());
    }
    return Memory::PatchHandle(id.value());
}

auto HookRuntime::enableResource(const Memory::ResourceId id) -> Expected<void> {
    std::lock_guard lock(mMutex);
    auto*           resource = findResource(id);
    if (resource == nullptr) {
        return unexpected(Memory::makeMemoryError(Memory::ErrorCode::InvalidHandle, "Invalid resource handle"));
    }
    return resource->enable();
}

auto HookRuntime::disableResource(const Memory::ResourceId id) -> Expected<void> {
    std::lock_guard lock(mMutex);
    auto*           resource = findResource(id);
    if (resource == nullptr) {
        return unexpected(Memory::makeMemoryError(Memory::ErrorCode::InvalidHandle, "Invalid resource handle"));
    }
    return resource->disable();
}

auto HookRuntime::removeResource(const Memory::ResourceId id) -> Expected<void> {
    std::lock_guard lock(mMutex);
    return removeResourceLocked(id);
}

bool HookRuntime::isResourceEnabled(const Memory::ResourceId id) {
    std::lock_guard lock(mMutex);
    const auto*     resource = findResource(id);
    return resource != nullptr && resource->isEnabled();
}

auto HookRuntime::suspendPluginResources(const std::string_view pluginName) -> Expected<void> {
    std::lock_guard lock(mMutex);
    const auto      ownerIt = mPluginResources.find(pluginName);
    if (ownerIt == mPluginResources.end()) {
        return {};
    }

    std::vector<Memory::ResourceId> suspended;
    for (const auto ids = std::vector(ownerIt->second.begin(), ownerIt->second.end());
         auto       id: std::views::reverse(ids)) {
        auto* resource = findResource(id);
        if (resource == nullptr || !resource->isEnabled()) {
            continue;
        }

        if (const auto result = resource->disable(); !result) {
            std::string rollbackErrors;
            for (const auto resumeId: std::views::reverse(suspended)) {
                if (auto* toResume = findResource(resumeId); toResume != nullptr) {
                    if (auto resumed = toResume->enable(); !resumed) {
                        appendError(rollbackErrors,
                                    "Resource " + std::to_string(resumeId) + ": " + resumed.error().message);
                    } else {
                        toResume->setSuspended(false);
                    }
                }
            }
            if (!rollbackErrors.empty()) {
                return unexpected(Memory::makeMemoryError(
                        Memory::ErrorCode::HookInstallFailed,
                        "Failed to suspend resource " + std::to_string(id) + ": " + result.error().message +
                                "\nRollback failed; resources may be partially suspended:\n" + rollbackErrors));
            }
            return unexpected(result.error());
        }

        resource->setSuspended(true);
        suspended.emplace_back(id);
    }
    return {};
}

auto HookRuntime::resumePluginResources(const std::string_view pluginName) -> Expected<void> {
    std::lock_guard lock(mMutex);
    const auto      ownerIt = mPluginResources.find(pluginName);
    if (ownerIt == mPluginResources.end()) {
        return {};
    }

    std::string errors;
    for (const auto id: ownerIt->second) {
        auto* resource = findResource(id);
        if (resource == nullptr || !resource->isSuspended()) {
            continue;
        }
        if (auto result = resource->enable(); !result) {
            appendError(errors, "Resource " + std::to_string(id) + ": " + result.error().message);
            continue;
        }
        resource->setSuspended(false);
    }
    if (!errors.empty()) {
        return unexpected(Memory::makeMemoryError(
                Memory::ErrorCode::HookInstallFailed,
                "Failed to resume one or more plugin resources; resources may be partially resumed:\n" + errors));
    }
    return {};
}

auto HookRuntime::clearPluginResources(const std::string_view pluginName) -> Expected<void> {
    std::lock_guard lock(mMutex);
    const auto      ownerIt = mPluginResources.find(pluginName);
    if (ownerIt == mPluginResources.end()) {
        return {};
    }

    std::string errors;
    for (const auto ids = std::vector(ownerIt->second.begin(), ownerIt->second.end());
         const auto id: std::views::reverse(ids)) {
        if (auto result = removeResourceLocked(id); !result) {
            appendError(errors, "Resource " + std::to_string(id) + ": " + result.error().message);
        }
    }
    if (!errors.empty()) {
        return unexpected(Memory::makeMemoryError(
                Memory::ErrorCode::HookRemoveFailed,
                "Failed to clear one or more plugin hook resources; resources may be partially cleared:\n" + errors));
    }
    return {};
}

} // namespace CloverNT::Core::Modules
