#include <CloverNT/API/Utils/System.hpp>

#include <ctime>
#include <string>
#include <system_error>

#include <dlfcn.h>
#include <unistd.h>

namespace CloverNT::Utils::System {

auto GetCurrentPlatform() noexcept -> Platform {
    return Platform::Linux;
}

auto GetCurrentPlatformName() noexcept -> std::string_view {
    return "linux";
}

auto GetCurrentTimeWithMs() noexcept -> TimeWithMs {
    timespec    now{};
    std::time_t seconds      = 0;
    auto        milliseconds = 0;

    if (clock_gettime(CLOCK_REALTIME, &now) == 0) {
        seconds      = now.tv_sec;
        milliseconds = static_cast<int>(now.tv_nsec / 1'000'000);
    } else {
        seconds = std::time(nullptr);
    }

    std::tm localTime{};
    if (seconds == static_cast<std::time_t>(-1) || localtime_r(&seconds, &localTime) == nullptr) {
        return {};
    }

    return TimeWithMs{
            .hour        = static_cast<std::uint16_t>(localTime.tm_hour),
            .minute      = static_cast<std::uint16_t>(localTime.tm_min),
            .second      = static_cast<std::uint16_t>(localTime.tm_sec),
            .millisecond = static_cast<std::uint16_t>(milliseconds),
    };
}

auto GetModuleFilePath(ModuleHandle module) -> std::filesystem::path {
    if (module == nullptr) {
        std::string buffer(256, '\0');
        for (;;) {
            const ssize_t len = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
            if (len <= 0) {
                return {};
            }
            if (static_cast<std::size_t>(len) < buffer.size()) {
                buffer.resize(static_cast<std::size_t>(len));
                return std::filesystem::path(std::move(buffer));
            }
            if (buffer.size() >= (1u << 20)) { // 1 MiB sanity ceiling
                return {};
            }
            buffer.resize(buffer.size() * 2);
        }
    }
    Dl_info info{};
    if (::dladdr(module, &info) != 0 && info.dli_fname != nullptr) {
        std::error_code ec;
        auto            absolute = std::filesystem::absolute(std::filesystem::path(info.dli_fname), ec);
        return ec ? std::filesystem::path(info.dli_fname) : absolute;
    }
    return {};
}

auto GetModuleDirectory(ModuleHandle module) -> std::filesystem::path {
    Dl_info info{};
    if (const auto* address = module != nullptr ? module : reinterpret_cast<void*>(&GetModuleDirectory);
        dladdr(address, &info) != 0 && info.dli_fname != nullptr) {
        std::error_code ec;
        const auto      absolute = std::filesystem::absolute(std::filesystem::path(info.dli_fname), ec);
        if (!ec) {
            return absolute.parent_path();
        }
    }

    std::error_code ec;
    auto            current = std::filesystem::current_path(ec);
    return ec ? std::filesystem::path{"."} : current;
}

} // namespace CloverNT::Utils::System
