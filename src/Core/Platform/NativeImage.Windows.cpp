#include <CloverNT/API/Logger.hpp>
#include <CloverNT/Core/Platform/NativeImage.hpp>

#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <Windows.h>

namespace CloverNT::Core::Platform {
namespace {

    auto logger() -> Logger& {
        static auto instance = LoggerRegistry::getInstance().getOrCreate("CloverCore");
        return *instance;
    }

    [[nodiscard]] auto imageError(std::string message, const CommonErrorCode code = CommonErrorCode::OperationFailed)
            -> std::unexpected<Error> {
        return unexpected(makeError(ErrorCategory::Plugin, code, std::move(message)));
    }

    [[nodiscard]] bool ownedByRunningProcess(std::filesystem::path const& directory) {
        const auto name = directory.filename().string();
        DWORD      pid  = 0;
        if (const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), pid);
            error != std::errc{} || end != name.data() + name.size() || pid == GetCurrentProcessId()) {
            return false;
        }
        const auto process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (process == nullptr) {
            return false;
        }
        const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
        CloseHandle(process);
        return running;
    }

    [[nodiscard]] auto imageRoot() -> Expected<std::filesystem::path> {
        static std::once_flag        once;
        static std::filesystem::path root;
        static std::error_code       failure;
        std::call_once(once, [] {
            const auto temp = std::filesystem::temp_directory_path(failure);
            if (failure) {
                return;
            }
            const auto      base = temp / "CloverNT" / "images";
            std::error_code ec;
            for (std::filesystem::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
                if (!ownedByRunningProcess(it->path())) {
                    std::error_code removeEc;
                    std::filesystem::remove_all(it->path(), removeEc);
                }
            }
            root = base / std::to_string(GetCurrentProcessId());
        });
        if (failure) {
            return imageError("cannot locate the temp directory: " + failure.message());
        }
        return root;
    }

    [[nodiscard]] auto readImage(std::filesystem::path const& path) -> Expected<std::vector<char>> {
        std::ifstream in(path, std::ios::binary | std::ios::ate);
        if (!in) {
            return imageError("cannot open " + path.string(), CommonErrorCode::NotFound);
        }
        std::vector<char> bytes((in.tellg()));
        in.seekg(0);
        if (!in.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
            return imageError("cannot read " + path.string());
        }
        return bytes;
    }

    [[nodiscard]] auto writeImage(std::filesystem::path const& path, std::span<const char> bytes) -> Expected<void> {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out || !out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())) || !out.flush()) {
            return imageError("cannot write " + path.string());
        }
        return {};
    }

    [[nodiscard]] auto importedModules(std::span<const char> image) -> std::vector<std::string> {
        std::vector<std::string> names;
        const auto               read = [&]<class T>(const std::size_t offset, T& out) {
            if (offset > image.size() || image.size() - offset < sizeof(T)) {
                return false;
            }
            std::memcpy(&out, image.data() + offset, sizeof(T));
            return true;
        };

        IMAGE_DOS_HEADER dos{};
        if (!read(0, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0) {
            return names;
        }
        const auto       ntOffset = static_cast<std::size_t>(dos.e_lfanew);
        IMAGE_NT_HEADERS nt{};
        if (!read(ntOffset, nt) || nt.Signature != IMAGE_NT_SIGNATURE ||
            nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR_MAGIC) {
            return names;
        }
        const auto& directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (directory.VirtualAddress == 0) {
            return names;
        }

        std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
        const auto                        sectionOffset =
                ntOffset + offsetof(IMAGE_NT_HEADERS, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
        for (std::size_t i = 0; i < sections.size(); ++i) {
            if (!read(sectionOffset + i * sizeof(IMAGE_SECTION_HEADER), sections[i])) {
                return names;
            }
        }
        const auto fileOffset = [&](const DWORD rva) -> std::optional<std::size_t> {
            for (auto const& section: sections) {
                if (rva >= section.VirtualAddress && rva - section.VirtualAddress < section.SizeOfRawData) {
                    return static_cast<std::size_t>(rva - section.VirtualAddress) + section.PointerToRawData;
                }
            }
            return std::nullopt;
        };

        auto descriptorOffset = fileOffset(directory.VirtualAddress);
        for (IMAGE_IMPORT_DESCRIPTOR descriptor{};
             descriptorOffset && read(*descriptorOffset, descriptor) && descriptor.Name != 0;
             *descriptorOffset += sizeof(descriptor)) {
            if (const auto nameOffset = fileOffset(descriptor.Name); nameOffset && *nameOffset < image.size()) {
                const char* name = image.data() + *nameOffset;
                names.emplace_back(name, strnlen(name, image.size() - *nameOffset));
            }
        }
        return names;
    }

    [[nodiscard]] auto preloadSiblings(std::vector<std::string> const& imports,
                                       std::filesystem::path const&    entryDirectory,
                                       std::filesystem::path const&    pluginDirectory)
            -> Expected<std::vector<CloverNT::Utils::DynamicLibrary>> {
        std::vector<CloverNT::Utils::DynamicLibrary> loaded;
        for (auto const& name: imports) {
            const auto file = std::filesystem::path(name);
            if (GetModuleHandleW(file.c_str()) != nullptr) {
                continue;
            }
            const auto candidate = entryDirectory / file;
            if (std::error_code ec; !std::filesystem::is_regular_file(candidate, ec)) {
                continue;
            }
            Utils::DynamicLibrary dependency;
            if (auto result = dependency.load(candidate, pluginDirectory); !result) {
                return imageError("failed to load dependency " + candidate.string() + ": " + result.error().message);
            }
            loaded.push_back(std::move(dependency));
        }
        return loaded;
    }

} // namespace

auto loadNativeImage(Utils::DynamicLibrary& library,
                     const std::string_view           pluginName,
                     std::filesystem::path const&     entryPath,
                     std::filesystem::path const&     pluginDirectory) -> Expected<std::filesystem::path> {
    auto root = imageRoot();
    if (!root) {
        return unexpected(root.error());
    }
    static std::atomic<std::uint64_t> sequence{0};
    const auto                        directory = *root / (std::string(pluginName) + "-" + std::to_string(++sequence));

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return imageError("cannot create " + directory.string() + ": " + ec.message());
    }

    const auto fail = [&](const Error& error) -> std::unexpected<Error> {
        discardNativeImage(directory);
        return unexpected(error);
    };

    auto bytes = readImage(entryPath);
    if (!bytes) {
        return fail(bytes.error());
    }
    const auto image = directory / entryPath.filename();
    if (auto written = writeImage(image, *bytes); !written) {
        return fail(written.error());
    }
    auto siblings = preloadSiblings(importedModules(*bytes), entryPath.parent_path(), pluginDirectory);
    if (!siblings) {
        return fail(siblings.error());
    }
    if (auto loaded = library.load(image, pluginDirectory); !loaded) {
        return fail(loaded.error());
    }
    return directory;
}

void discardNativeImage(std::filesystem::path const& imageDirectory) noexcept {
    if (imageDirectory.empty()) {
        return;
    }
    try {
        if (std::error_code ec; std::filesystem::remove_all(imageDirectory, ec) == static_cast<std::uintmax_t>(-1)) {
            logger().warn("failed to remove plugin image copy {}: {}", imageDirectory.string(), ec.message());
            return;
        }
        std::error_code ec;
        std::filesystem::remove(imageDirectory.parent_path(), ec);
    } catch (...) {}
}

} // namespace CloverNT::Core::Platform
