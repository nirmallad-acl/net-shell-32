# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# NETSHELL32-5 (REQ-FUNC-24, REQ-FUNC-34): the firmware version comes ONLY from
# version.txt (ESP-IDF's project.cmake copies it into PROJECT_VER when PROJECT_VER
# is not already set). Everything below runs BEFORE project.cmake is included, so a
# bad version fails the configure step with a clear message instead of producing an
# image that reports a placeholder version at runtime.
if(DEFINED PROJECT_VER OR DEFINED CACHE{PROJECT_VER})
    message(FATAL_ERROR
        "PROJECT_VER must not be set on the command line (-DPROJECT_VER=...) or in the CMake "
        "cache: the firmware version is defined only by version.txt (REQ-FUNC-24). "
        "An environment variable of that name is not consulted by this check.")
endif()

set(_nsh_ver_file "${CMAKE_CURRENT_LIST_DIR}/version.txt")
if(NOT EXISTS "${_nsh_ver_file}")
    message(FATAL_ERROR "version.txt is missing at ${_nsh_ver_file} (REQ-FUNC-24).")
endif()
file(READ "${_nsh_ver_file}" _nsh_ver_raw)
# Accept exactly one line; a single trailing LF is allowed.
string(REGEX REPLACE "\n$" "" _nsh_ver "${_nsh_ver_raw}")
if(_nsh_ver MATCHES "[\r\n]")
    message(FATAL_ERROR "version.txt must contain exactly one line with LF line endings.")
endif()
if(_nsh_ver STREQUAL "" OR _nsh_ver STREQUAL "1")
    message(FATAL_ERROR "version.txt is empty or holds the placeholder value '1'.")
endif()
string(LENGTH "${_nsh_ver}" _nsh_ver_len)
if(_nsh_ver_len GREATER 31)
    message(FATAL_ERROR "version.txt content is ${_nsh_ver_len} characters; the limit is 31.")
endif()
if(NOT _nsh_ver MATCHES "^[A-Za-z0-9._+-]+$")
    message(FATAL_ERROR "version.txt contains characters outside [A-Za-z0-9._+-].")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(net-shell-32)
