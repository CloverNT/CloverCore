add_library(CloverAPI STATIC
        Exception.cpp
        Logger.cpp
        Utils/DynamicLibrary.cpp
        Utils/System.cpp
        $<$<PLATFORM_ID:Windows>:Exception/Exception.Windows.cpp>
        $<$<PLATFORM_ID:Linux>:Exception/Exception.Linux.cpp>
        $<$<PLATFORM_ID:Windows>:Utils/DynamicLibrary.Windows.cpp>
        $<$<PLATFORM_ID:Linux>:Utils/DynamicLibrary.Linux.cpp>
        $<$<PLATFORM_ID:Windows>:Utils/System.Windows.cpp>
        $<$<PLATFORM_ID:Linux>:Utils/System.Linux.cpp>
)

get_filename_component(CloverNT_API_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../include" ABSOLUTE)

target_include_directories(CloverAPI PUBLIC ${CloverNT_API_INCLUDE_DIR})
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

if (WIN32)
    target_link_libraries(CloverAPI PUBLIC dbghelp)
else ()
    target_link_libraries(CloverAPI PUBLIC dl)
endif ()

set_target_properties(CloverAPI PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
