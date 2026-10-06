#include <CloverNT/API/Logger.hpp>
#include <CloverNT/Runtime/Kernel.h>

#include <condition_variable>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
    #include <windows.h>
#else
    #include <dlfcn.h>
#endif

namespace CloverNT {
namespace {
    using ConsoleEmitFn = void(CloverNT_KERNEL_CALL*)(const char*);

    ConsoleEmitFn consoleEmit() {
        static const ConsoleEmitFn fn = []() -> ConsoleEmitFn {
#if defined(_WIN32)
            if (HMODULE kernel = ::GetModuleHandleW(L"CloverLoader.dll")) {
                return reinterpret_cast<ConsoleEmitFn>(
                        reinterpret_cast<void*>(::GetProcAddress(kernel, "CloverConsoleEmit")));
            }
            return nullptr;
#else
            if (void* global = dlopen(nullptr, RTLD_NOW)) {
                auto* const f = reinterpret_cast<ConsoleEmitFn>(dlsym(global, "CloverConsoleEmit"));
                dlclose(global);
                return f;
            }
            return nullptr;
#endif
        }();
        return fn;
    }

    std::string formatPlain(const LogMessage& message) {
        const auto time = LogProtocol::formatTimestamp(message.time);
        const auto name = LogProtocol::levelName(message.level);
        if (message.title.empty()) {
            return std::format("[{} {}] {}", time, name, message.text);
        }
        return std::format("[{} {}] [{}] {}", time, name, message.title, message.text);
    }

    std::string formatColored(const LogMessage& message, const bool colorEnabled) {
        if (!colorEnabled) {
            return formatPlain(message);
        }
        const auto time  = LogProtocol::formatTimestamp(message.time);
        const auto name  = LogProtocol::levelName(message.level);
        const auto color = LogProtocol::levelColor(message.level);
        if (message.title.empty()) {
            return std::format("{}[{} {}]{} {}", color, time, name, LogProtocol::ColorReset, message.text);
        }
        return std::format(
                "{}[{} {}]{} [{}] {}", color, time, name, LogProtocol::ColorReset, message.title, message.text);
    }

    class ConsoleConsumer final : public LogConsumer {
    public:
        void consume(LogMessage const& message, const std::string_view, const std::string_view) override {
            auto line = LogProtocol::encode(message.level, message.title, message.text);
            if (const auto emit = consoleEmit()) {
                line.push_back('\n');
                emit(line.c_str()); // single process-wide exit; it holds its own lock
                return;
            }
            std::lock_guard lock(mMutex);
            std::cout << line << std::endl;
        }

        void flush() override {
            std::lock_guard lock(mMutex);
            std::cout.flush();
        }

    private:
        std::mutex mMutex;
    };

    std::shared_ptr<LogConsumer> sharedConsoleConsumer() {
        static const std::shared_ptr<LogConsumer> consumer = std::make_shared<ConsoleConsumer>();
        return consumer;
    }

    class FileConsumer final : public LogConsumer {
    public:
        explicit FileConsumer(const std::string_view path) : mFile(std::string(path), std::ios::app) {}

        [[nodiscard]] bool isOpen() const noexcept {
            return mFile.is_open();
        }

        void consume(LogMessage const&, const std::string_view plainLine, const std::string_view) override {
            std::lock_guard lock(mMutex);
            mFile << plainLine << '\n';
            mFile.flush();
        }

        void flush() override {
            std::lock_guard lock(mMutex);
            mFile.flush();
        }

    private:
        std::mutex    mMutex;
        std::ofstream mFile;
    };

    struct DispatchItem {
        LogMessage                                message;
        std::string                               plainLine;
        std::string                               coloredLine;
        std::vector<std::shared_ptr<LogConsumer>> consumers;
    };

    class LogDispatcher {
    public:
        LogDispatcher() : mWorker([this] { run(); }) {}

        ~LogDispatcher() {
            {
                std::lock_guard lock(mMutex);
                mStopping = true;
            }
            mWake.notify_all();
            if (mWorker.joinable()) {
                mWorker.join();
            }
        }

        void enqueue(DispatchItem item) {
            {
                std::lock_guard lock(mMutex);
                mQueue.emplace_back(std::move(item));
            }
            mWake.notify_one();
        }

        void flush() {
            std::unique_lock lock(mMutex);
            mIdle.wait(lock, [this] { return mQueue.empty() && mActive == 0; });
        }

    private:
        void run() {
            for (;;) {
                DispatchItem item;
                {
                    std::unique_lock lock(mMutex);
                    mWake.wait(lock, [this] { return mStopping || !mQueue.empty(); });
                    if (mStopping && mQueue.empty()) {
                        break;
                    }
                    item = std::move(mQueue.front());
                    mQueue.pop_front();
                    ++mActive;
                }

                for (const auto& consumer: item.consumers) {
                    if (!consumer) {
                        continue;
                    }
                    try {
                        consumer->consume(item.message, item.plainLine, item.coloredLine);
                    } catch (...) {}
                }

                {
                    std::lock_guard lock(mMutex);
                    --mActive;
                    if (mQueue.empty() && mActive == 0) {
                        mIdle.notify_all();
                    }
                }
            }
        }

        std::mutex               mMutex;
        std::condition_variable  mWake;
        std::condition_variable  mIdle;
        std::deque<DispatchItem> mQueue;
        std::thread              mWorker;
        std::size_t              mActive{};
        bool                     mStopping{};
    };

    LogDispatcher& dispatcher() {
        static LogDispatcher instance;
        return instance;
    }

} // namespace

LogConsumer::~LogConsumer() = default;

void LogConsumer::flush() {}

Logger::Logger(const std::string_view title) : mTitle(title) {
#ifdef CloverNT_DEBUG
    mMinLevel = LogLevel::Debug;
#endif
    setOutputConsole(true);
}

Logger::~Logger() = default;

std::string Logger::title() const {
    std::lock_guard lock(mMutex);
    return mTitle;
}

LogLevel Logger::minLevel() const noexcept {
    return mMinLevel.load(std::memory_order_relaxed);
}

bool Logger::shouldLog(LogLevel level) const noexcept {
    return static_cast<int>(level) >= static_cast<int>(mMinLevel.load(std::memory_order_relaxed));
}

void Logger::setTitle(const std::string_view title) {
    std::lock_guard lock(mMutex);
    mTitle = title;
}

void Logger::setMinLevel(const LogLevel level) noexcept {
    mMinLevel.store(level, std::memory_order_relaxed);
}

void Logger::setOutputConsole(const bool enabled) {
    std::lock_guard lock(mMutex);
    if (enabled) {
        if (mConsoleConsumerId == 0) {
            mConsoleConsumerId = addConsumerLocked(sharedConsoleConsumer());
        }
    } else if (mConsoleConsumerId != 0) {
        removeConsumerLocked(mConsoleConsumerId);
        mConsoleConsumerId = 0;
    }
}

void Logger::setOutputColor(const bool enabled) noexcept {
    std::lock_guard lock(mMutex);
    mOutputColor = enabled;
}

auto Logger::setLogFile(std::string_view filePath) -> Expected<void> {
    std::lock_guard lock(mMutex);
    if (mFileConsumerId != 0) {
        removeConsumerLocked(mFileConsumerId);
        mFileConsumerId = 0;
    }
    if (!filePath.empty()) {
        auto consumer = std::make_shared<FileConsumer>(filePath);
        if (!consumer->isOpen()) {
            return unexpected(ErrorCategory::Logger,
                              CommonErrorCode::OperationFailed,
                              "Failed to open log file: '" + std::string(filePath) + "'.");
        }
        mFileConsumerId = addConsumerLocked(std::move(consumer));
    }
    return {};
}

auto Logger::addConsumer(std::shared_ptr<LogConsumer> consumer) -> Expected<LogConsumerId> {
    std::lock_guard lock(mMutex);
    const auto      id = addConsumerLocked(std::move(consumer));
    if (id == 0) {
        return unexpected(ErrorCategory::Logger, CommonErrorCode::InvalidArgument, "Log consumer is null");
    }
    return id;
}

auto Logger::removeConsumer(const LogConsumerId id) -> Expected<void> {
    std::lock_guard lock(mMutex);
    const auto      oldSize = mConsumers.size();
    removeConsumerLocked(id);
    if (mConsoleConsumerId == id) {
        mConsoleConsumerId = 0;
    }
    if (mFileConsumerId == id) {
        mFileConsumerId = 0;
    }
    if (mConsumers.size() == oldSize) {
        return unexpected(ErrorCategory::Logger, CommonErrorCode::NotFound, "Log consumer was not found");
    }
    return {};
}

void Logger::clearConsumers() {
    std::lock_guard lock(mMutex);
    mConsumers.clear();
    mConsoleConsumerId = 0;
    mFileConsumerId    = 0;
}

std::vector<std::shared_ptr<LogConsumer>> Logger::consumers() const {
    std::lock_guard                           lock(mMutex);
    std::vector<std::shared_ptr<LogConsumer>> result;
    result.reserve(mConsumers.size());
    for (const auto& [id, consumer]: mConsumers) {
        result.emplace_back(consumer);
    }
    return result;
}

void Logger::flush() const {
    dispatcher().flush();
    for (auto& consumer: consumers()) {
        if (consumer) {
            consumer->flush();
        }
    }
}

void Logger::flushAll() {
    dispatcher().flush();
}

void Logger::logString(LogLevel level, std::string message) const noexcept {
    try {
        if (!shouldLog(level)) {
            return;
        }

        std::vector<std::shared_ptr<LogConsumer>> consumerSnapshot;
        std::string                               titleSnapshot;
        bool                                      colorSnapshot{};
        {
            std::lock_guard lock(mMutex);
            titleSnapshot = mTitle;
            colorSnapshot = mOutputColor;
            consumerSnapshot.reserve(mConsumers.size());
            for (const auto& [id, consumer]: mConsumers) {
                if (consumer) {
                    consumerSnapshot.emplace_back(consumer);
                }
            }
        }

        if (consumerSnapshot.empty()) {
            return;
        }

        LogMessage logMessage{
                .level = level,
                .title = std::move(titleSnapshot),
                .text  = std::move(message),
                .time  = Utils::System::GetCurrentTimeWithMs(),
        };
        auto plainLine   = formatPlain(logMessage);
        auto coloredLine = formatColored(logMessage, colorSnapshot);

        dispatcher().enqueue(DispatchItem{
                .message     = std::move(logMessage),
                .plainLine   = std::move(plainLine),
                .coloredLine = std::move(coloredLine),
                .consumers   = std::move(consumerSnapshot),
        });
    } catch (...) {}
}

LogConsumerId Logger::addConsumerLocked(std::shared_ptr<LogConsumer> consumer) {
    if (!consumer) {
        return 0;
    }
    const auto id = mNextConsumerId++;
    mConsumers.emplace_back(ConsumerEntry{.id = id, .consumer = std::move(consumer)});
    return id;
}

void Logger::removeConsumerLocked(LogConsumerId id) {
    std::erase_if(mConsumers, [id](ConsumerEntry const& entry) { return entry.id == id; });
}

class LoggerRegistry::Impl {
public:
    mutable std::mutex                                        mutex;
    std::map<std::string, std::weak_ptr<Logger>, std::less<>> loggers;
};

LoggerRegistry::LoggerRegistry() : mImpl(std::make_unique<Impl>()) {}

LoggerRegistry::~LoggerRegistry() = default;

LoggerRegistry& LoggerRegistry::getInstance() {
    static LoggerRegistry instance;
    return instance;
}

std::shared_ptr<Logger> LoggerRegistry::getOrCreate(const std::string_view title) const {
    std::lock_guard lock(mImpl->mutex);
    auto&           weak   = mImpl->loggers[std::string(title)];
    auto            shared = weak.lock();
    if (!shared) {
        shared = std::make_shared<Logger>(title);
        weak   = shared;
    }
    return shared;
}

std::shared_ptr<Logger> LoggerRegistry::tryGet(const std::string_view title) const {
    std::lock_guard lock(mImpl->mutex);
    const auto      it = mImpl->loggers.find(title);
    return it == mImpl->loggers.end() ? nullptr : it->second.lock();
}

bool LoggerRegistry::erase(const std::string_view title) const {
    std::lock_guard lock(mImpl->mutex);
    return mImpl->loggers.erase(std::string(title)) != 0;
}

std::vector<std::shared_ptr<Logger>> LoggerRegistry::loggers() const {
    std::lock_guard                      lock(mImpl->mutex);
    std::vector<std::shared_ptr<Logger>> result;
    for (auto it = mImpl->loggers.begin(); it != mImpl->loggers.end();) {
        if (auto logger = it->second.lock()) {
            result.emplace_back(std::move(logger));
            ++it;
        } else {
            it = mImpl->loggers.erase(it);
        }
    }
    return result;
}

} // namespace CloverNT
