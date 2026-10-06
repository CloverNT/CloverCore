get_filename_component(_cv_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(CLOVERNT_VERSION_TAGGED OFF)
set(_cv_tag "")
set(_cv_sha "")

find_package(Git QUIET)
if (GIT_EXECUTABLE)
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --exact-match HEAD
            WORKING_DIRECTORY "${_cv_root}"
            OUTPUT_VARIABLE _cv_tag OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _cv_tag_rc ERROR_QUIET)
    if (NOT _cv_tag_rc EQUAL 0)
        set(_cv_tag "")
    endif ()
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
            WORKING_DIRECTORY "${_cv_root}"
            OUTPUT_VARIABLE _cv_sha OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE _cv_sha_rc ERROR_QUIET)
    if (NOT _cv_sha_rc EQUAL 0)
        set(_cv_sha "")
    endif ()
endif ()

if (_cv_tag)
    set(CLOVERNT_VERSION_STRING "${_cv_tag}")
    set(CLOVERNT_VERSION_TAGGED ON)
elseif (_cv_sha)
    set(CLOVERNT_VERSION_STRING "${_cv_sha}")
elseif (EXISTS "${_cv_root}/VERSION")
    file(READ "${_cv_root}/VERSION" CLOVERNT_VERSION_STRING)
    string(STRIP "${CLOVERNT_VERSION_STRING}" CLOVERNT_VERSION_STRING)
else ()
    set(CLOVERNT_VERSION_STRING "0.0.0-unknown")
endif ()

string(REGEX REPLACE "^[vV]" "" _cv_num "${CLOVERNT_VERSION_STRING}")
if (_cv_num MATCHES "^([0-9]+\\.[0-9]+\\.[0-9]+)")
    set(CLOVERNT_VERSION "${CMAKE_MATCH_1}")
else ()
    set(CLOVERNT_VERSION "0.0.0")
endif ()

if (CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    if (FIELD STREQUAL "numeric")
        message("${CLOVERNT_VERSION}")
    elseif (FIELD STREQUAL "tagged")
        message("${CLOVERNT_VERSION_TAGGED}")
    else ()
        message("${CLOVERNT_VERSION_STRING}")
    endif ()
endif ()
