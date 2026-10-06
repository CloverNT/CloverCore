set(CLOVER_CORE_SOURCES
        Event/Cancellable.cpp
        Event/Listener.cpp
        Event/NamedEvent.cpp
        Module/EventRuntime.cpp
        Module/HookRuntime.cpp
        Module/Manager.cpp
        Module/PluginRuntime/PluginRuntime.cpp
        Module/PluginRuntime/PluginResolver.cpp
        Module/PluginRuntime/JsRuntime.cpp
        Module/PluginRuntime/Bridge/Binding.cpp
        Module/PluginRuntime/Bridge/AlkaBindings.cpp
        Module/PluginRuntime/Bridge/Lifecycle.cpp
        Module/PluginRuntime/Bridge/Electron.cpp
        Module/PluginRuntime/Bridge/EventExports.cpp
        Module/PluginRuntime/Bridge/Events.cpp
        Event/PacketBridge.cpp
        Platform/DllEventSource.cpp
        Platform/NodeEnvironmentGate.cpp
        $<$<PLATFORM_ID:Windows>:Platform/NodeEnvironmentGate.Windows.cpp>
        $<$<PLATFORM_ID:Windows>:Platform/DelayLoadCompat.Windows.cpp>
        $<$<PLATFORM_ID:Linux>:Platform/NodeEnvironmentGate.Linux.cpp>
        Memory/Memory.cpp
        Plugin/JsPlugin.cpp
        Plugin/NativePlugin.cpp
        Plugin/Plugin.cpp
        Plugin/PluginBase.cpp
        Plugin/Version.cpp
)
add_library(CloverCore SHARED
        Library.cpp
        ${CLOVER_CORE_SOURCES}
)
target_compile_definitions(CloverCore PRIVATE CloverNT_EXPORTS CloverNT_CORE_BUILD)
target_compile_definitions(CloverCore PRIVATE
        $<$<CONFIG:Debug>:CloverNT_DEBUG>
        ${CloverNT_SYSTEM_MACRO}
        ${CloverNT_ARCH_MACRO}
)
target_link_libraries(CloverCore PRIVATE CloverAPI libhat Mortis nlohmann_json::nlohmann_json QQNT::QQNT Alka::Alka CloverRuntime)

if (WIN32)
    target_link_libraries(CloverCore PRIVATE delayimp)
    target_link_options(CloverCore PRIVATE /DELAYLOAD:QQNT.dll)
else ()
    target_link_libraries(CloverCore PRIVATE dl)
endif ()

set_target_properties(CloverCore PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
if (NOT WIN32)
    target_link_options(CloverCore PRIVATE
            "LINKER:--exclude-libs,libc++.a:libc++abi.a:libhat.a:libMortis.a")
endif ()

if (CLOVERNT_BUILD_TESTS)
    add_library(CloverCoreTestStatic STATIC
            ${CLOVER_CORE_SOURCES}
    )
    target_include_directories(CloverCoreTestStatic PUBLIC ${CMAKE_SOURCE_DIR}/include)
    target_compile_definitions(CloverCoreTestStatic
            PUBLIC
            CloverNT_API_STATIC
            $<$<CONFIG:Debug>:CloverNT_DEBUG>
            ${CloverNT_SYSTEM_MACRO}
            ${CloverNT_ARCH_MACRO}
            PRIVATE
            CloverNT_CORE_BUILD
            CloverNT_KERNEL_STATIC
    )
    target_link_libraries(CloverCoreTestStatic PUBLIC CloverRuntimeTestStatic CloverAPI libhat Mortis nlohmann_json::nlohmann_json QQNT::QQNT Alka::Alka)
    if (NOT WIN32)
        target_link_libraries(CloverCoreTestStatic PUBLIC dl)
    endif ()
endif ()
