#pragma once

#include <atomic>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <CloverNT/API/Expected.hpp>
#include <CloverNT/API/Macros.hpp>
#include <CloverNT/API/Utils/System.hpp>

namespace CloverNT {

enum class LogLevel {
    Debug = 0,
    Info,
    Warning,
    Error,
    Fatal,
};

namespace LogProtocol {

    inline constexpr char             TitleStart   = '\x01';
    inline constexpr char             TitleEnd     = '\x02';
    inline constexpr std::string_view DefaultTitle = "QQNT";
    inline constexpr std::string_view ColorReset   = "\033[0m";

    [[nodiscard]] constexpr auto levelColor(const LogLevel level) -> std::string_view {
        switch (level) {
        case LogLevel::Debug:
            return "\033[36m";
        case LogLevel::Info:
            return "\033[32m";
        case LogLevel::Warning:
            return "\033[33m";
        case LogLevel::Error:
            return "\033[31m";
        case LogLevel::Fatal:
            return "\033[35m";
        }
        return ColorReset;
    }

    [[nodiscard]] constexpr auto levelName(const LogLevel level) -> std::string_view {
        switch (level) {
        case LogLevel::Debug:
            return "DBG";
        case LogLevel::Info:
            return "INF";
        case LogLevel::Warning:
            return "WRN";
        case LogLevel::Error:
            return "ERR";
        case LogLevel::Fatal:
            return "FTL";
        }
        return "UNK";
    }

    [[nodiscard]] inline auto formatTimestamp(const Utils::System::TimeWithMs time) -> std::string {
        return std::format("{:02}:{:02}:{:02}.{:03}", time.hour, time.minute, time.second, time.millisecond);
    }

    // <SOH><level><title><STX><text>
    [[nodiscard]] inline auto encode(const LogLevel level, const std::string_view title, const std::string_view text)
            -> std::string {
        std::string line;
        line.reserve(title.size() + text.size() + 3);
        line.push_back(TitleStart);
        line.push_back(static_cast<char>('0' + static_cast<int>(level)));
        line.append(title);
        line.push_back(TitleEnd);
        line.append(text);
        return line;
    }

    struct Decoded {
        bool             tagged{}; // false for raw output without our marker (e.g. QQ's own stdout)
        LogLevel         level{LogLevel::Info};
        std::string_view title; // empty when the line carries no title
        std::string_view text;
    };

    [[nodiscard]] inline auto decode(const std::string_view line) -> Decoded {
        if (line.size() >= 2 && line.front() == TitleStart) {
            if (const auto end = line.find(TitleEnd, 2); end != std::string_view::npos) {
                const int  digit = line[1] - '0';
                const auto level =
                        (digit >= static_cast<int>(LogLevel::Debug) && digit <= static_cast<int>(LogLevel::Fatal))
                                ? static_cast<LogLevel>(digit)
                                : LogLevel::Info;
                return Decoded{true, level, line.substr(2, end - 2), line.substr(end + 1)};
            }
        }
        return Decoded{false, LogLevel::Info, std::string_view{}, line};
    }

    [[nodiscard]] inline auto render(const Utils::System::TimeWithMs time,
                                     const LogLevel                  level,
                                     const std::string_view          title,
                                     const std::string_view          text) -> std::string {
        return std::format(
                "[{} {}] [{}] {}", formatTimestamp(time), levelName(level), title.empty() ? DefaultTitle : title, text);
    }

    // Only the [time LEVEL] header is colored by level; the [title] and message are left at the terminal's default.
    [[nodiscard]] inline auto renderColored(const Utils::System::TimeWithMs time,
                                            const LogLevel                  level,
                                            const std::string_view          title,
                                            const std::string_view          text) -> std::string {
        return std::format("{}[{} {}]{} [{}] {}",
                           levelColor(level),
                           formatTimestamp(time),
                           levelName(level),
                           ColorReset,
                           title.empty() ? DefaultTitle : title,
                           text);
    }

} // namespace LogProtocol

struct LogMessage {
    LogLevel                  level{};
    std::string               title;
    std::string               text;
    Utils::System::TimeWithMs time;
};

class CloverNT_API LogConsumer {
public:
    virtual ~LogConsumer();

    virtual void consume(LogMessage const& message, std::string_view plainLine, std::string_view coloredLine) = 0;
    virtual void flush();
};

using LogConsumerId = std::uint64_t;

class CloverNT_API Logger {
public:
    explicit Logger(std::string_view title = {});
    ~Logger();

    Logger(Logger const&)            = delete;
    Logger& operator=(Logger const&) = delete;
    Logger(Logger&&)                 = delete;
    Logger& operator=(Logger&&)      = delete;

    [[nodiscard]] auto title() const -> std::string;
    [[nodiscard]] auto minLevel() const noexcept -> LogLevel;
    [[nodiscard]] bool shouldLog(LogLevel level) const noexcept;

    void               setTitle(std::string_view title);
    void               setMinLevel(LogLevel level) noexcept;
    void               setOutputConsole(bool enabled);
    void               setOutputColor(bool enabled) noexcept;
    [[nodiscard]] auto setLogFile(std::string_view filePath) -> Expected<void>;

    [[nodiscard]] auto addConsumer(std::shared_ptr<LogConsumer> consumer) -> Expected<LogConsumerId>;
    [[nodiscard]] auto removeConsumer(LogConsumerId id) -> Expected<void>;
    void               clearConsumers();
    [[nodiscard]] auto consumers() const -> std::vector<std::shared_ptr<LogConsumer>>;

    void        flush() const;
    static void flushAll();

    void logString(LogLevel level, std::string message) const noexcept;

    template <typename... Args>
    void log(const LogLevel level, std::format_string<Args...> fmt, Args&&... args) const {
        if (shouldLog(level)) {
            logString(level, std::format(fmt, std::forward<Args>(args)...));
        }
    }

    void log(const LogLevel level, const std::string_view message) const noexcept {
        if (shouldLog(level)) {
            logString(level, std::string(message));
        }
    }

    template <typename... Args>
    void debug(std::format_string<Args...> fmt, Args&&... args) const {
#ifdef CloverNT_DEBUG
        log(LogLevel::Debug, fmt, std::forward<Args>(args)...);
#else
        (void) fmt;
        (void) sizeof...(args);
#endif
    }

    void debug(const std::string_view message) const noexcept {
#ifdef CloverNT_DEBUG
        log(LogLevel::Debug, message);
#else
        (void) message;
#endif
    }

    template <typename... Args>
    void info(std::format_string<Args...> fmt, Args&&... args) const {
        log(LogLevel::Info, fmt, std::forward<Args>(args)...);
    }

    void info(const std::string_view message) const noexcept {
        log(LogLevel::Info, message);
    }

    template <typename... Args>
    void warn(std::format_string<Args...> fmt, Args&&... args) const {
        log(LogLevel::Warning, fmt, std::forward<Args>(args)...);
    }

    void warn(const std::string_view message) const noexcept {
        log(LogLevel::Warning, message);
    }

    template <typename... Args>
    void error(std::format_string<Args...> fmt, Args&&... args) const {
        log(LogLevel::Error, fmt, std::forward<Args>(args)...);
    }

    void error(const std::string_view message) const noexcept {
        log(LogLevel::Error, message);
    }

    template <typename... Args>
    void fatal(std::format_string<Args...> fmt, Args&&... args) const {
        log(LogLevel::Fatal, fmt, std::forward<Args>(args)...);
    }

    void fatal(const std::string_view message) const noexcept {
        log(LogLevel::Fatal, message);
    }

private:
    struct ConsumerEntry {
        LogConsumerId                id{};
        std::shared_ptr<LogConsumer> consumer;
    };

    auto addConsumerLocked(std::shared_ptr<LogConsumer> consumer) -> LogConsumerId;
    void removeConsumerLocked(LogConsumerId id);

    mutable std::mutex         mMutex;
    std::string                mTitle;
    std::atomic<LogLevel>      mMinLevel{LogLevel::Info};
    bool                       mOutputColor{true};
    LogConsumerId              mNextConsumerId{1};
    LogConsumerId              mConsoleConsumerId{};
    LogConsumerId              mFileConsumerId{};
    std::vector<ConsumerEntry> mConsumers;
};

class CloverNT_API LoggerRegistry {
public:
    LoggerRegistry(LoggerRegistry const&)            = delete;
    LoggerRegistry& operator=(LoggerRegistry const&) = delete;

    static auto getInstance() -> LoggerRegistry&;

    [[nodiscard]] auto getOrCreate(std::string_view title) const -> std::shared_ptr<Logger>;
    [[nodiscard]] auto tryGet(std::string_view title) const -> std::shared_ptr<Logger>;
    auto               erase(std::string_view title) const -> bool;
    [[nodiscard]] auto loggers() const -> std::vector<std::shared_ptr<Logger>>;

private:
    LoggerRegistry();
    ~LoggerRegistry();

    class Impl;
    std::unique_ptr<Impl> mImpl;
};

} // namespace CloverNT
