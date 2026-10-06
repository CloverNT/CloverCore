#include <CloverNT/API/Memory/Hook.hpp>
#include <CloverNT/API/Memory/Memory.hpp>
#include <CloverNT/Core/Modules/HookRuntime.hpp>
#include <CloverNT/Core/Modules/Manager.hpp>

#include <utility>

#include <Mortis/MemoryScanner.hpp>
#include <Mortis/Process.hpp>

namespace CloverNT::Memory {
namespace {

    [[nodiscard]] auto hookRuntime() -> Expected<Core::Modules::ModuleRef<Core::Modules::HookRuntime>> {
        return Core::Modules::Manager::getInstance().requireModule<Core::Modules::HookRuntime>();
    }

    [[nodiscard]] auto toErrorCode(const Mortis::ErrorCode code) noexcept -> ErrorCode {
        switch (code) {
        case Mortis::ErrorCode::MemoryNotReadable:
            return ErrorCode::MemoryNotReadable;
        case Mortis::ErrorCode::MemoryNotWritable:
            return ErrorCode::MemoryNotWritable;
        case Mortis::ErrorCode::ProtectionFailed:
            return ErrorCode::ProtectionFailed;
        case Mortis::ErrorCode::HookInstallFailed:
            return ErrorCode::HookInstallFailed;
        case Mortis::ErrorCode::HookRemoveFailed:
            return ErrorCode::HookRemoveFailed;
        case Mortis::ErrorCode::NoFreeSlots:
            return ErrorCode::NoFreeSlots;
        case Mortis::ErrorCode::ImportNotFound:
            return ErrorCode::ImportNotFound;
        case Mortis::ErrorCode::ModuleNotFound:
            return ErrorCode::ModuleNotFound;
        case Mortis::ErrorCode::InvalidArgument:
            return ErrorCode::InvalidArgument;
        case Mortis::ErrorCode::Unknown:
        default:
            return ErrorCode::Unknown;
        }
    }

    [[nodiscard]] auto fromMortis(Mortis::Result<void> const& result) -> Expected<void> {
        if (result) {
            return {};
        }
        return unexpected(makeMemoryError(toErrorCode(result.code()), result.error()));
    }

    [[nodiscard]] auto toMortis(MemoryProtection const protection) noexcept -> Mortis::MemoryProtection {
        return static_cast<Mortis::MemoryProtection>(std::to_underlying(protection));
    }

    [[nodiscard]] auto fromMortis(Mortis::MemoryProtection const protection) noexcept -> MemoryProtection {
        return static_cast<MemoryProtection>(std::to_underlying(protection));
    }

    [[nodiscard]] auto toMortis(ScanAlignment const alignment) noexcept -> Mortis::ScanAlignment {
        return static_cast<Mortis::ScanAlignment>(std::to_underlying(alignment));
    }

    [[nodiscard]] auto toMortis(ScanHint const hint) noexcept -> Mortis::ScanHint {
        return static_cast<Mortis::ScanHint>(std::to_underlying(hint));
    }

    [[nodiscard]] auto toMortis(ScanOptions const options) noexcept -> Mortis::ScanOptions {
        return Mortis::ScanOptions{
                .alignment = toMortis(options.alignment),
                .hints     = toMortis(options.hints),
        };
    }

    [[nodiscard]] auto toMortis(Signature const& signature) -> Mortis::Signature {
        std::vector<Mortis::SignatureElement> elements;
        elements.reserve(signature.size());
        for (const auto& [value, mask]: signature.elements()) {
            elements.push_back(Mortis::SignatureElement{
                    .value = value,
                    .mask  = mask,
            });
        }
        return Mortis::Signature(std::move(elements));
    }

    [[nodiscard]] auto toMortis(SignatureView signature) -> std::vector<Mortis::SignatureElement> {
        std::vector<Mortis::SignatureElement> elements;
        elements.reserve(signature.size());
        for (const auto& [value, mask]: signature) {
            elements.push_back(Mortis::SignatureElement{
                    .value = value,
                    .mask  = mask,
            });
        }
        return elements;
    }

    [[nodiscard]] auto fromMortis(Mortis::Signature const& signature) -> Signature {
        std::vector<SignatureElement> elements;
        elements.reserve(signature.size());
        for (const auto& [value, mask]: signature) {
            elements.push_back(SignatureElement{
                    .value = value,
                    .mask  = mask,
            });
        }
        return Signature(std::move(elements));
    }

    [[nodiscard]] auto fromMortis(const Mortis::ScanResult result) noexcept -> ScanResult {
        return ScanResult(result.get());
    }

    [[nodiscard]] auto fromMortis(Mortis::Module const& module) -> Module {
        return {module.name(), module.base(), module.size()};
    }

    [[nodiscard]] auto toMortis(Module const& module) -> Mortis::Module {
        return {module.name(), module.base(), module.size()};
    }

} // namespace

Module::Module(std::string name, const Address base, const std::size_t size)
    : mName(std::move(name)), mBase(base), mSize(size) {}

auto Module::name() const noexcept -> std::string const& {
    return mName;
}

auto Module::base() const noexcept -> Address {
    return mBase;
}

auto Module::size() const noexcept -> std::size_t {
    return mSize;
}

bool Module::contains(const Address address) const noexcept {
    return address >= mBase && (address - mBase) < mSize;
}

auto Module::findExport(const std::string_view symbolName) const -> Expected<Address> {
    auto result = toMortis(*this).findExport(symbolName);
    if (!result) {
        return unexpected(makeMemoryError(ErrorCode::ImportNotFound, "Export not found: " + std::string(symbolName)));
    }
    return *result;
}

auto Module::enumerateExports() const -> std::vector<std::pair<std::string, Address>> {
    std::vector<std::pair<std::string, Address>> result;
    for (auto&& [name, address]: toMortis(*this).enumerateExports()) {
        result.emplace_back(name, address);
    }
    return result;
}

auto Module::findSection(const std::string_view sectionName) const -> Expected<std::pair<Address, std::size_t>> {
    auto result = toMortis(*this).findSection(sectionName);
    if (!result) {
        return unexpected(makeMemoryError(ErrorCode::ModuleNotFound, "Section not found: " + std::string(sectionName)));
    }
    return *result;
}

Signature::Signature(std::vector<SignatureElement> elements) : mElements(std::move(elements)) {}

bool Signature::empty() const noexcept {
    return mElements.empty();
}

auto Signature::size() const noexcept -> std::size_t {
    return mElements.size();
}

auto Signature::elements() const noexcept -> std::span<SignatureElement const> {
    return mElements;
}

auto Signature::toString() const -> std::string {
    return toMortis(*this).toString();
}

PatchHandle::PatchHandle(const ResourceId id) noexcept : mId(id) {}

PatchHandle::~PatchHandle() {
    try {
        (void) remove();
    } catch (...) {}
}

PatchHandle::PatchHandle(PatchHandle&& other) noexcept : mId(std::exchange(other.mId, 0)) {}

auto PatchHandle::operator=(PatchHandle&& other) noexcept -> PatchHandle& {
    if (this != &other) {
        try {
            (void) remove();
        } catch (...) {}
        mId = std::exchange(other.mId, 0);
    }
    return *this;
}

PatchHandle::operator bool() const noexcept {
    return mId != 0;
}

auto PatchHandle::id() const noexcept -> ResourceId {
    return mId;
}

bool PatchHandle::isApplied() const {
    const auto runtime = hookRuntime();
    return runtime && runtime->get().isResourceEnabled(mId);
}

auto PatchHandle::apply() const -> Expected<void> {
    const auto runtime = hookRuntime();
    if (!runtime) {
        return unexpected(runtime.error());
    }
    return runtime->get().enableResource(mId);
}

auto PatchHandle::restore() const -> Expected<void> {
    const auto runtime = hookRuntime();
    if (!runtime) {
        return unexpected(runtime.error());
    }
    return runtime->get().disableResource(mId);
}

auto PatchHandle::remove() -> Expected<void> {
    if (mId == 0) {
        return {};
    }

    auto runtime = hookRuntime();
    if (!runtime) {
        return unexpected(runtime.error());
    }

    if (auto removed = runtime->get().removeResource(mId); !removed) {
        return unexpected(removed.error());
    }

    mId = 0;
    return {};
}

auto findModule(const std::string_view moduleName) -> Expected<Module> {
    const auto module = Mortis::Process::FindModule(moduleName);
    if (!module) {
        return unexpected(makeMemoryError(ErrorCode::ModuleNotFound, "Module not found: " + std::string(moduleName)));
    }
    return fromMortis(*module);
}

auto enumerateModules() -> std::vector<Module> {
    const auto          mortisModules = Mortis::Process::EnumerateModules();
    std::vector<Module> modules;
    modules.reserve(mortisModules.size());
    for (const auto& module: mortisModules) {
        modules.emplace_back(fromMortis(module));
    }
    return modules;
}

auto readMemory(void* dest, const Address source, const std::size_t size) -> Expected<void> {
    return fromMortis(Mortis::Process::ReadMemory(dest, source, size));
}

auto writeMemory(const Address dest, void const* source, const std::size_t size) -> Expected<void> {
    return fromMortis(Mortis::Process::WriteMemory(dest, source, size));
}

auto setProtection(const Address address, const std::size_t size, const MemoryProtection protection)
        -> Expected<MemoryProtection> {
    const auto result = Mortis::Process::SetProtection(address, size, toMortis(protection));
    if (!result) {
        return unexpected(makeMemoryError(toErrorCode(result.code()), result.error()));
    }
    return fromMortis(result.value());
}

auto setProtectionRaw(const Address address, const std::size_t size, const MemoryProtection protection)
        -> Expected<void> {
    return fromMortis(Mortis::Process::SetProtectionRaw(address, size, toMortis(protection)));
}

auto queryProtection(const Address address) -> Expected<MemoryProtection> {
    const auto result = Mortis::Process::QueryProtection(address);
    if (!result) {
        return unexpected(makeMemoryError(toErrorCode(result.code()), result.error()));
    }
    return fromMortis(result.value());
}

bool isReadable(const Address address, const std::size_t size) {
    return Mortis::Process::IsReadable(address, size);
}

bool isWritable(const Address address, const std::size_t size) {
    return Mortis::Process::IsWritable(address, size);
}

auto readBytes(const Address address, const std::size_t size) -> Expected<std::vector<std::uint8_t>> {
    std::vector<std::uint8_t> bytes(size);
    if (auto result = readMemory(bytes.data(), address, bytes.size()); !result) {
        return unexpected(result.error());
    }
    return bytes;
}

auto writeBytes(const Address address, const std::span<std::uint8_t const> bytes) -> Expected<void> {
    return writeMemory(address, bytes.data(), bytes.size());
}

auto parseSignature(const std::string_view pattern) -> Expected<Signature> {
    const auto signature = Mortis::MemoryScanner::ParseSignature(pattern);
    if (!signature) {
        return unexpected(
                makeMemoryError(ErrorCode::InvalidArgument, "Invalid signature pattern: " + std::string(pattern)));
    }
    return fromMortis(*signature);
}

auto findFirst(const std::string_view moduleName, const std::string_view pattern, const ScanOptions options)
        -> ScanResult {
    return fromMortis(Mortis::MemoryScanner::FindFirst(moduleName, pattern, toMortis(options)));
}

auto findFirst(const Module& module, const std::string_view pattern, const ScanOptions options) -> ScanResult {
    return fromMortis(Mortis::MemoryScanner::FindFirst(toMortis(module), pattern, toMortis(options)));
}

auto findAll(const std::string_view moduleName, const std::string_view pattern, const ScanOptions options)
        -> std::vector<ScanResult> {
    const auto              mortisResults = Mortis::MemoryScanner::FindAll(moduleName, pattern, toMortis(options));
    std::vector<ScanResult> results;
    results.reserve(mortisResults.size());
    for (const auto& result: mortisResults) {
        results.emplace_back(fromMortis(result));
    }
    return results;
}

auto findAll(const Module& module, const std::string_view pattern, const ScanOptions options)
        -> std::vector<ScanResult> {
    const auto mortisResults = Mortis::MemoryScanner::FindAll(toMortis(module), pattern, toMortis(options));
    std::vector<ScanResult> results;
    results.reserve(mortisResults.size());
    for (const auto& result: mortisResults) {
        results.emplace_back(fromMortis(result));
    }
    return results;
}

auto findFirst(const std::string_view moduleName, const Signature& signature, const ScanOptions options) -> ScanResult {
    auto mortisSignature = toMortis(signature);
    return fromMortis(Mortis::MemoryScanner::FindFirst(moduleName, mortisSignature, toMortis(options)));
}

auto findFirst(const Module& module, const Signature& signature, const ScanOptions options) -> ScanResult {
    auto mortisSignature = toMortis(signature);
    return fromMortis(Mortis::MemoryScanner::FindFirst(toMortis(module), mortisSignature, toMortis(options)));
}

auto findAll(const std::string_view moduleName, Signature const& signature, const ScanOptions options)
        -> std::vector<ScanResult> {
    auto       mortisSignature = toMortis(signature);
    const auto mortisResults   = Mortis::MemoryScanner::FindAll(moduleName, mortisSignature, toMortis(options));
    std::vector<ScanResult> results;
    results.reserve(mortisResults.size());
    for (const auto& result: mortisResults) {
        results.emplace_back(fromMortis(result));
    }
    return results;
}

auto findAll(Module const& module, Signature const& signature, const ScanOptions options) -> std::vector<ScanResult> {
    auto       mortisSignature = toMortis(signature);
    const auto mortisResults   = Mortis::MemoryScanner::FindAll(toMortis(module), mortisSignature, toMortis(options));
    std::vector<ScanResult> results;
    results.reserve(mortisResults.size());
    for (const auto& result: mortisResults) {
        results.emplace_back(fromMortis(result));
    }
    return results;
}

auto findFirstInSection(const std::string_view moduleName,
                        const std::string_view sectionName,
                        const Signature&       signature,
                        const ScanOptions      options) -> ScanResult {
    auto mortisSignature = toMortis(signature);
    return fromMortis(
            Mortis::MemoryScanner::FindInSection(moduleName, sectionName, mortisSignature, toMortis(options)));
}

auto findAllInSection(const std::string_view moduleName,
                      const std::string_view sectionName,
                      const Signature&       signature,
                      const ScanOptions      options) -> std::vector<ScanResult> {
    auto       mortisSignature = toMortis(signature);
    const auto mortisResults =
            Mortis::MemoryScanner::FindAllInSection(moduleName, sectionName, mortisSignature, toMortis(options));
    std::vector<ScanResult> results;
    results.reserve(mortisResults.size());
    for (const auto result: mortisResults) {
        results.emplace_back(fromMortis(result));
    }
    return results;
}

auto findFirst(const std::string_view moduleName, const SignatureView signature, const ScanOptions options)
        -> ScanResult {
    const auto mortisSignature = toMortis(signature);
    return fromMortis(Mortis::MemoryScanner::FindFirst(moduleName, mortisSignature, toMortis(options)));
}

auto findFirst(const Module& module, const SignatureView signature, const ScanOptions options) -> ScanResult {
    const auto mortisSignature = toMortis(signature);
    return fromMortis(Mortis::MemoryScanner::FindFirst(toMortis(module), mortisSignature, toMortis(options)));
}

auto findAll(const std::string_view moduleName, const SignatureView signature, const ScanOptions options)
        -> std::vector<ScanResult> {
    const auto mortisSignature = toMortis(signature);
    const auto mortisResults   = Mortis::MemoryScanner::FindAll(moduleName, mortisSignature, toMortis(options));
    std::vector<ScanResult> results;
    results.reserve(mortisResults.size());
    for (const auto& result: mortisResults) {
        results.emplace_back(fromMortis(result));
    }
    return results;
}

auto findAll(const Module& module, const SignatureView signature, const ScanOptions options)
        -> std::vector<ScanResult> {
    const auto mortisSignature = toMortis(signature);
    const auto mortisResults   = Mortis::MemoryScanner::FindAll(toMortis(module), mortisSignature, toMortis(options));
    std::vector<ScanResult> results;
    results.reserve(mortisResults.size());
    for (const auto& result: mortisResults) {
        results.emplace_back(fromMortis(result));
    }
    return results;
}

auto findFirstInSection(const std::string_view moduleName,
                        const std::string_view sectionName,
                        const SignatureView    signature,
                        const ScanOptions      options) -> ScanResult {
    const auto mortisSignature = toMortis(signature);
    return fromMortis(
            Mortis::MemoryScanner::FindInSection(moduleName, sectionName, mortisSignature, toMortis(options)));
}

auto findAllInSection(const std::string_view moduleName,
                      const std::string_view sectionName,
                      const SignatureView    signature,
                      const ScanOptions      options) -> std::vector<ScanResult> {
    const auto mortisSignature = toMortis(signature);
    const auto mortisResults =
            Mortis::MemoryScanner::FindAllInSection(moduleName, sectionName, mortisSignature, toMortis(options));
    std::vector<ScanResult> results;
    results.reserve(mortisResults.size());
    for (const auto result: mortisResults) {
        results.emplace_back(fromMortis(result));
    }
    return results;
}

auto createPatch(const Address address, std::vector<std::uint8_t> bytes, const ResourceOptions& options)
        -> Expected<PatchHandle> {
    auto runtime = hookRuntime();
    if (!runtime) {
        return unexpected(runtime.error());
    }
    return runtime->get().createPatch(address, std::move(bytes), options);
}

auto createNopPatch(const Address address, const std::size_t size, const ResourceOptions& options)
        -> Expected<PatchHandle> {
    auto runtime = hookRuntime();
    if (!runtime) {
        return unexpected(runtime.error());
    }
    return runtime->get().createNopPatch(address, size, options);
}

namespace Detail {

    ResourcePayload::~ResourcePayload() = default;

    auto createInlineHook(const Address                        target,
                                             const Address                        detour,
                                             const int                            priority,
                                             void**                               originalSlot,
                                             const std::weak_ptr<Plugin::Plugin>& owner,
                                             std::shared_ptr<ResourcePayload>     payload) -> Expected<ResourceId> {
        auto runtime = hookRuntime();
        if (!runtime) {
            return unexpected(runtime.error());
        }
        return runtime->get().createInlineHook(target, detour, priority, originalSlot, owner, std::move(payload));
    }

    auto createImportHook(const std::string_view               moduleName,
                                             const std::string_view               importModule,
                                             const std::string_view               functionName,
                                             const Address                        detour,
                                             void**                               originalSlot,
                                             const std::weak_ptr<Plugin::Plugin>& owner,
                                             std::shared_ptr<ResourcePayload>     payload) -> Expected<ResourceId> {
        auto runtime = hookRuntime();
        if (!runtime) {
            return unexpected(runtime.error());
        }
        return runtime->get().createImportHook(
                moduleName, importModule, functionName, detour, originalSlot, owner, std::move(payload));
    }

    auto enableResource(const ResourceId id) -> Expected<void> {
        auto runtime = hookRuntime();
        if (!runtime) {
            return unexpected(runtime.error());
        }
        return runtime->get().enableResource(id);
    }

    auto disableResource(const ResourceId id) -> Expected<void> {
        auto runtime = hookRuntime();
        if (!runtime) {
            return unexpected(runtime.error());
        }
        return runtime->get().disableResource(id);
    }

    auto removeResource(const ResourceId id) -> Expected<void> {
        auto runtime = hookRuntime();
        if (!runtime) {
            return unexpected(runtime.error());
        }
        return runtime->get().removeResource(id);
    }

    bool isResourceEnabled(const ResourceId id) {
        const auto runtime = hookRuntime();
        return runtime && runtime->get().isResourceEnabled(id);
    }

} // namespace Detail

namespace Hook {

    Handle::Handle(const ResourceId id) noexcept : mId(id) {}

    Handle::~Handle() {
        try {
            (void) remove();
        } catch (...) {}
    }

    Handle::Handle(Handle&& other) noexcept : mId(std::exchange(other.mId, 0)) {}

    auto Handle::operator=(Handle&& other) noexcept -> Handle& {
        if (this != &other) {
            try {
                (void) remove();
            } catch (...) {}
            mId = std::exchange(other.mId, 0);
        }
        return *this;
    }

    Handle::operator bool() const noexcept {
        return mId != 0;
    }

    auto Handle::id() const noexcept -> ResourceId {
        return mId;
    }

    bool Handle::isEnabled() const {
        const auto runtime = hookRuntime();
        return runtime && runtime->get().isResourceEnabled(mId);
    }

    auto Handle::enable() const -> Expected<void> {
        const auto runtime = hookRuntime();
        if (!runtime) {
            return unexpected(runtime.error());
        }
        return runtime->get().enableResource(mId);
    }

    auto Handle::disable() const -> Expected<void> {
        auto runtime = hookRuntime();
        if (!runtime) {
            return unexpected(runtime.error());
        }
        return runtime->get().disableResource(mId);
    }

    auto Handle::remove() -> Expected<void> {
        if (mId == 0) {
            return {};
        }

        auto runtime = hookRuntime();
        if (!runtime) {
            return unexpected(runtime.error());
        }

        if (auto removed = runtime->get().removeResource(mId); !removed) {
            return unexpected(removed.error());
        }

        mId = 0;
        return {};
    }

} // namespace Hook

} // namespace CloverNT::Memory
