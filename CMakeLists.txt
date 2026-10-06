cmake_minimum_required(VERSION 4.0)
include(FetchContent)
include(cmake/Version.cmake)

project(CloverCore VERSION ${CLOVERNT_VERSION})
message(STATUS "CloverCore ${CLOVERNT_VERSION_STRING} (numeric ${CLOVERNT_VERSION})")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if (CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/EHsc>)
endif ()

add_compile_definitions(CLOVERNT_VERSION_STRING="${CLOVERNT_VERSION_STRING}")

## Platform / arch macros
if (WIN32)
    set(CloverNT_SYSTEM_MACRO CloverNT_SYSTEM_WINDOWS)
else ()
    set(CloverNT_SYSTEM_MACRO CloverNT_SYSTEM_LINUX)
endif ()

if (MSVC AND CMAKE_GENERATOR_PLATFORM)
    set(_clovernt_arch_raw "${CMAKE_GENERATOR_PLATFORM}")
elseif (CMAKE_SYSTEM_PROCESSOR)
    set(_clovernt_arch_raw "${CMAKE_SYSTEM_PROCESSOR}")
else ()
    set(_clovernt_arch_raw "${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif ()
string(TOLOWER "${_clovernt_arch_raw}" _clovernt_arch_raw_lower)
if (_clovernt_arch_raw_lower MATCHES "arm64|aarch64")
    set(CloverNT_ARCH_MACRO CloverNT_ARCH_ARM64)
else ()
    set(CloverNT_ARCH_MACRO CloverNT_ARCH_X64)
endif ()

set(LIBCXX_GITHUB_REPO "CloverNT/libcxx-build")
set(LIBCXX_ABI_NAMESPACE "__Cr")
set(QQNT_SDK_REPO "CloverNT/qqnt-sdk")
set(QQNT_SDK_VERSION "9.9.31-49738")
if (NOT WIN32)
    set(QQNT_SDK_LINK_LIBS OFF)
endif ()
include(cmake/Libc++.cmake)
include(cmake/QQNTSdk.cmake)
use_libcxx_globally()

FetchContent_Declare(
        CloverLoaderSDK
        GIT_REPOSITORY https://github.com/CloverNT/CloverLoader-Release.git
        GIT_TAG main
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
)
FetchContent_Declare(
        libhat
        GIT_REPOSITORY https://github.com/BasedInc/libhat.git
        GIT_TAG v0.11.1
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
)
FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG v3.12.0
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
)
set(MORTIS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MORTIS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
        Mortis
        GIT_REPOSITORY https://github.com/SingerBallad/Mortis.git
        GIT_TAG main
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
)
set(ALKA_V8_TARGET QQNT::QQNT)
set(ALKA_BUILD_ENGINE OFF)
FetchContent_Declare(
        Alka
        GIT_REPOSITORY https://github.com/SingerBallad/Alka.git
        GIT_TAG main
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(CloverLoaderSDK libhat nlohmann_json Mortis Alka)
target_compile_definitions(Alka PUBLIC ALKA_CONTEXT_EMBEDDER_SLOT=30)

if (WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)
endif ()

include_directories(include)

add_library(CloverAPI STATIC
        src/API/Exception.cpp
        src/API/Logger.cpp
        src/API/Utils/DynamicLibrary.cpp
        src/API/Utils/System.cpp
        $<$<PLATFORM_ID:Windows>:src/API/Exception/Exception.Windows.cpp>
        $<$<PLATFORM_ID:Linux>:src/API/Exception/Exception.Linux.cpp>
        $<$<PLATFORM_ID:Windows>:src/API/Utils/DynamicLibrary.Windows.cpp>
        $<$<PLATFORM_ID:Linux>:src/API/Utils/DynamicLibrary.Linux.cpp>
        $<$<PLATFORM_ID:Windows>:src/API/Utils/System.Windows.cpp>
        $<$<PLATFORM_ID:Linux>:src/API/Utils/System.Linux.cpp>
)
target_include_directories(CloverAPI PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(CloverAPI
        PUBLIC
        CloverNT_API_STATIC
        $<$<CONFIG:Debug>:CloverNT_DEBUG>
        ${CloverNT_SYSTEM_MACRO}
        ${CloverNT_ARCH_MACRO}
        PRIVATE
        $<$<PLATFORM_ID:Windows>:WIN32_LEAN_AND_MEAN>
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
)
target_link_libraries(CloverAPI PUBLIC CloverNT::Runtime)
if (WIN32)
    target_link_libraries(CloverAPI PUBLIC dbghelp)
else ()
    target_link_libraries(CloverAPI PUBLIC dl)
endif ()
set_target_properties(CloverAPI PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)

set(CLOVER_CORE_SOURCES
        src/Core/Event/Cancellable.cpp
        src/Core/Event/Listener.cpp
        src/Core/Event/NamedEvent.cpp
        src/Core/Modules/EventRuntime.cpp
        src/Core/Modules/HookRuntime.cpp
        src/Core/Modules/Manager.cpp
        src/Core/Modules/PluginRuntime/PluginRuntime.cpp
        src/Core/Modules/PluginRuntime/PluginResolver.cpp
        src/Core/Modules/PluginRuntime/JsRuntime.cpp
        src/Core/Modules/PluginRuntime/Bridge/Binding.cpp
        src/Core/Modules/PluginRuntime/Bridge/AlkaBindings.cpp
        src/Core/Modules/PluginRuntime/Bridge/Lifecycle.cpp
        src/Core/Modules/PluginRuntime/Bridge/Electron.cpp
        src/Core/Modules/PluginRuntime/Bridge/EventExports.cpp
        src/Core/Modules/PluginRuntime/Bridge/Events.cpp
        src/Core/Event/PacketBridge.cpp
        src/Core/Platform/DllEventSource.cpp
        src/Core/Platform/NodeEnvironmentGate.cpp
        $<$<PLATFORM_ID:Windows>:src/Core/Platform/NodeEnvironmentGate.Windows.cpp>
        $<$<PLATFORM_ID:Windows>:src/Core/Platform/DelayLoadCompat.Windows.cpp>
        $<$<PLATFORM_ID:Linux>:src/Core/Platform/NodeEnvironmentGate.Linux.cpp>
        src/Core/Memory/Memory.cpp
        src/Core/Plugin/JsPlugin.cpp
        src/Core/Plugin/NativePlugin.cpp
        src/Core/Plugin/Plugin.cpp
        src/Core/Plugin/PluginBase.cpp
        src/Core/Plugin/Version.cpp
)
add_library(CloverCore SHARED
        src/Core/Library.cpp
        ${CLOVER_CORE_SOURCES}
)
target_compile_definitions(CloverCore PRIVATE
        CloverNT_EXPORTS
        CloverNT_CORE_BUILD
        $<$<CONFIG:Debug>:CloverNT_DEBUG>
        ${CloverNT_SYSTEM_MACRO}
        ${CloverNT_ARCH_MACRO}
)
target_link_libraries(CloverCore PRIVATE
        CloverAPI libhat Mortis nlohmann_json::nlohmann_json QQNT::QQNT Alka::Alka CloverNT::Runtime)
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

option(CLOVERNT_GENERATE_DTS "Generate framework/src/clovernt.gen.d.ts from the Alka bindings at build time" ON)
if (CLOVERNT_GENERATE_DTS)
    add_executable(CloverDtsGen tools/GenCloverntDts.cpp)
    target_link_libraries(CloverDtsGen PRIVATE Alka::Alka CloverNT::Runtime)
    target_compile_definitions(CloverDtsGen PRIVATE
            CloverNT_API_STATIC ${CloverNT_SYSTEM_MACRO} ${CloverNT_ARCH_MACRO})
    set(_clovernt_dts "${CMAKE_CURRENT_SOURCE_DIR}/framework/src/clovernt.gen.d.ts")
    add_custom_command(
            OUTPUT "${_clovernt_dts}"
            COMMAND CloverDtsGen "${_clovernt_dts}"
            DEPENDS CloverDtsGen
            COMMENT "Generating framework/src/clovernt.gen.d.ts from the Alka bindings"
            VERBATIM)
    add_custom_target(CloverDts ALL DEPENDS "${_clovernt_dts}")
endif ()
