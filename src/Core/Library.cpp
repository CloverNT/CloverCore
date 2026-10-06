#include <CloverNT/API/Exception.hpp>
#include <CloverNT/API/Logger.hpp>
#include <CloverNT/API/Macros.hpp>
#include <CloverNT/Core/Event/PacketBridge.hpp>
#include <CloverNT/Core/Modules/EventRuntime.hpp>
#include <CloverNT/Core/Modules/HookRuntime.hpp>
#include <CloverNT/Core/Modules/Manager.hpp>
#include <CloverNT/Core/Modules/PluginRuntime.hpp>
#include <CloverNT/Core/Platform/DllEventSource.hpp>
#include <CloverNT/Core/Platform/NodeEnvironment.hpp>
#include <CloverNT/Core/Platform/NodeEnvironmentGate.hpp>

#include <exception>
#include <memory>

namespace CloverNT::Core {
namespace {
    auto logger() -> Logger& {
        static auto instance = LoggerRegistry::getInstance().getOrCreate("CloverCore");
        return *instance;
    }

    std::unique_ptr<Platform::NodeEnvironmentGate> gEnvironmentGate;

    auto registerDefaultModules(Modules::Manager& manager) -> Expected<void> {
        if (auto result =
                    manager.registerModule<Modules::EventRuntime>("EventRuntime", "Initialize CloverNT event bus.");
            !result) {
            return result;
        }
        if (auto result = manager.registerModule<Modules::HookRuntime>("HookRuntime",
                                                                       "Manage CloverNT memory hooks and patches.");
            !result) {
            return result;
        }
        if (auto result = manager.registerModule<Modules::PluginRuntime>(
                    "PluginRuntime", "Load and unload CloverNT native and JS plugins.");
            !result) {
            return result;
        }
        return {};
    }

    auto enableSubstrate(Modules::Manager& manager) -> Expected<void> {
        if (auto result = manager.enableModule<Modules::EventRuntime>(); !result) {
            return result;
        }
        return manager.enableModule<Modules::HookRuntime>();
    }

} // namespace

void Load(const ModuleHandle) {
    try {
        auto& manager     = Modules::Manager::getInstance();
        auto  initialized = manager.initialize();
        if (!initialized) {
            logger().fatal("Failed to initialize module manager: {}", initialized.error().message);
            return;
        }
        if (initialized.value()) {
            if (auto registered = registerDefaultModules(manager); !registered) {
                logger().fatal("Failed to register core modules: {}", registered.error().message);
                return;
            }
        }
        if (auto loaded = manager.loadAll(); !loaded) {
            logger().fatal("Failed to load one or more core modules: {}", loaded.error().message);
            return;
        }
        if (auto enabled = enableSubstrate(manager); !enabled) {
            logger().fatal("Failed to enable core substrate: {}", enabled.error().message);
            return;
        }

        gEnvironmentGate = std::make_unique<Platform::NodeEnvironmentGate>(Platform::NodeEnvironmentGate::Callbacks{
                .onEnvironmentReady =
                        [](const Platform::EnvironmentContext& environment) {
                            Platform::NodeEnvironment::publish(environment);
                            if (auto enabled = Modules::Manager::getInstance().enableAll(); !enabled) {
                                logger().fatal("Failed to enable modules for the Node environment: {}",
                                               enabled.error().message);
                            }
                            Event::startPacketBridge();
                        },
                .onEnvironmentDestroyed =
                        [] {
                            logger().info("Unloading CloverCore...");
                            Event::stopPacketBridge();
                            Platform::NodeEnvironment::markStopped();
                            Platform::stopDllEventSource();
                            (void) Modules::Manager::getInstance().destroy();
                            Platform::NodeEnvironment::clear();
                        },
        });
        gEnvironmentGate->arm();

        Platform::startDllEventSource();
    } catch (const Exception& e) {
        logger().fatal("Failed to load core: {}", e.describe());
    } catch (std::exception const& exception) {
        logger().fatal("Failed to load core: {}", exception.what());
    } catch (...) {
        logger().fatal("Failed to load core: unknown exception");
    }
}

void Unload(const ModuleHandle) {
    try {
        Platform::stopDllEventSource();
        gEnvironmentGate.reset();
        if (auto destroyed = Modules::Manager::getInstance().destroy(); !destroyed) {
            logger().fatal("Failed to unload core: {}", destroyed.error().message);
        }
        Platform::NodeEnvironment::clear();
    } catch (const Exception& e) {
        logger().fatal("Failed to unload core: {}", e.describe());
    } catch (std::exception const& exception) {
        logger().fatal("Failed to unload core: {}", exception.what());
    } catch (...) {
        logger().fatal("Failed to unload core: unknown exception");
    }
}

} // namespace CloverNT::Core

LIBRARY_ENTRYPOINT(CloverNT::Core, Load, Unload);
