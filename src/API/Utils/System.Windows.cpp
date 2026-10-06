#include <CloverNT/API/Utils/System.hpp>

#include <cwchar>
#include <iterator>
#include <string>

#include <Windows.h>

extern "C" __declspec(dllimport) DWORD WINAPI K32GetMappedFileNameW(HANDLE, LPVOID, LPWSTR, DWORD);

namespace CloverNT::Utils::System {

namespace {

    constexpr std::size_t kNtPathCeiling = 0x8000; // ~32767 wide chars: the real NT path limit

    template <typename Fill>
    std::wstring growToFit(Fill&& fill) {
        std::wstring buffer(MAX_PATH, L'\0');
        for (;;) {
            const DWORD len = fill(buffer.data(), static_cast<DWORD>(buffer.size()));
            if (len == 0) {
                return {};
            }
            if (len < buffer.size() || buffer.size() >= kNtPathCeiling) {
                buffer.resize(len <= buffer.size() ? len : buffer.size());
                return buffer;
            }
            buffer.resize(buffer.size() * 2);
        }
    }

    std::wstring deviceToDrive(const std::wstring& devicePath) {
        wchar_t     drives[512]{};
        const DWORD count = GetLogicalDriveStringsW(std::size(drives) - 1, drives);
        if (count == 0) {
            return {};
        }
        for (const wchar_t* p = drives; *p != L'\0'; p += std::wcslen(p) + 1) {
            const wchar_t letter[3] = {p[0], L':', L'\0'};
            std::wstring  target(MAX_PATH, L'\0');
            DWORD         n = 0;
            for (;;) {
                n = QueryDosDeviceW(letter, target.data(), static_cast<DWORD>(target.size()));
                if (n != 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER || target.size() >= kNtPathCeiling) {
                    break;
                }
                target.resize(target.size() * 2);
            }
            if (n == 0) {
                continue;
            }
            const std::wstring device(target); // first of the double-null-terminated list
            if (devicePath.size() > device.size() &&
                _wcsnicmp(devicePath.c_str(), device.c_str(), device.size()) == 0 &&
                devicePath[device.size()] == L'\\') {
                return std::wstring(letter) + devicePath.substr(device.size());
            }
        }
        return {};
    }

} // namespace

auto GetCurrentPlatform() noexcept -> Platform {
    return Platform::Windows;
}

auto GetCurrentPlatformName() noexcept -> std::string_view {
    return "windows";
}

auto GetCurrentTimeWithMs() noexcept -> TimeWithMs {
    SYSTEMTIME localTime{};
    GetLocalTime(&localTime);
    return TimeWithMs{
            .hour        = localTime.wHour,
            .minute      = localTime.wMinute,
            .second      = localTime.wSecond,
            .millisecond = localTime.wMilliseconds,
    };
}

auto GetModuleFilePath(const ModuleHandle module) -> std::filesystem::path {
    if (auto name = growToFit([module](wchar_t* buf, const DWORD cap) { return GetModuleFileNameW(module, buf, cap); });
        !name.empty()) {
        return {std::move(name)};
    }

    if (module != nullptr) {
        if (auto mapped = growToFit([module](wchar_t* buf, DWORD cap) {
                return K32GetMappedFileNameW(GetCurrentProcess(), module, buf, cap);
            });
            !mapped.empty()) {
            if (auto dos = deviceToDrive(mapped); !dos.empty()) {
                return {std::move(dos)};
            }
        }
    }
    return {};
}

auto GetModuleDirectory(const ModuleHandle module) -> std::filesystem::path {
    if (const auto full = GetModuleFilePath(module); !full.empty()) {
        return full.parent_path();
    }
    return std::filesystem::current_path();
}

} // namespace CloverNT::Utils::System
