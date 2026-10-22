# NETSHELL32-5 host unit tests (REQ-TEST-01..03, tech_spec.md 2.14).
#
# Standalone CMake project: it compiles ONLY the PURE source files of the new
# components (no FreeRTOS, NimBLE or other IDF code) against stub headers, with
# Unity taken from the ESP-IDF tree. One executable per test file, registered
# with add_test(). Requires host gcc and cmake, and IDF_PATH set.
#
#   cmake -S test/host -B test/host/build && cmake --build test/host/build \
#     && ctest --test-dir test/host/build --output-on-failure
#
# Mock results are decision-logic evidence only. They never validate the
# ADXL345 hardware, BLE behaviour or timing on the target (REQ-TEST-05).
cmake_minimum_required(VERSION 3.16)
project(ns5_host_tests C)

if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR "IDF_PATH is not set: the host tests take Unity from $IDF_PATH/components/unity")
endif()
set(NS5_UNITY_DIR "$ENV{IDF_PATH}/components/unity/unity/src")
if(NOT EXISTS "${NS5_UNITY_DIR}/unity.c")
    message(FATAL_ERROR "Unity not found at ${NS5_UNITY_DIR}")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
add_compile_options(-Wall -Wextra -Werror -g -O1)

# ASan + UBSan when the toolchain supports them (the fuzz test relies on them).
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
check_c_source_compiles("int main(void){return 0;}" NS5_HAVE_SANITIZERS)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(NS5_HAVE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
else()
    message(WARNING "ASan/UBSan not supported by this toolchain: tests run without sanitizers")
endif()

enable_testing()

get_filename_component(NS5_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(NS5_COMP "${NS5_ROOT}/components")

# Unity itself is third-party code: no -Werror on it.
add_library(unity STATIC "${NS5_UNITY_DIR}/unity.c")
target_include_directories(unity PUBLIC "${NS5_UNITY_DIR}")
target_compile_options(unity PRIVATE -w)

set(NS5_INCLUDES
    "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
    "${CMAKE_CURRENT_SOURCE_DIR}/mocks"
    "${NS5_COMP}/ble_host/include"            "${NS5_COMP}/ble_host/private_include"
    "${NS5_COMP}/ble_cli/include"             "${NS5_COMP}/ble_cli/private_include"
    "${NS5_COMP}/diag_cmds/include"           "${NS5_COMP}/diag_cmds/private_include"
    "${NS5_COMP}/accel_sensor/include"        "${NS5_COMP}/accel_sensor/private_include"
    "${NS5_COMP}/device_info/include"         "${NS5_COMP}/device_info/private_include"
    "${NS5_COMP}/ble_ota/include"             "${NS5_COMP}/ble_ota/private_include")

# ns5_test(<name> <extra sources...>): test_<name>.c plus the pure sources it needs.
function(ns5_test name)
    add_executable(test_${name} "${CMAKE_CURRENT_SOURCE_DIR}/test_${name}.c" ${ARGN})
    target_include_directories(test_${name} PRIVATE ${NS5_INCLUDES})
    target_link_libraries(test_${name} PRIVATE unity m)
    add_test(NAME ${name} COMMAND test_${name})
endfunction()

ns5_test(line_asm      "${NS5_COMP}/ble_cli/cli_line_asm.c")
ns5_test(frag          "${NS5_COMP}/ble_cli/cli_frag.c")
ns5_test(cli_tx        "${NS5_COMP}/ble_cli/cli_tx_gate.c"
                       "${NS5_COMP}/ble_cli/cli_line_asm.c")
ns5_test(gen_filter    "${NS5_COMP}/ble_cli/cli_tx_gate.c"
                       "${NS5_COMP}/ble_cli/cli_line_asm.c"
                       "${NS5_COMP}/ble_host/host_registry.c")
ns5_test(session       "${NS5_COMP}/diag_cmds/accel_session_core.c")
ns5_test(format        "${NS5_COMP}/diag_cmds/diag_format.c")
ns5_test(accel_sched   "${NS5_COMP}/diag_cmds/accel_sched.c"
                       "${NS5_COMP}/diag_cmds/diag_format.c")
ns5_test(host_registry "${NS5_COMP}/ble_host/host_registry.c")
ns5_test(host_exec     "${NS5_COMP}/ble_host/host_exec.c")
ns5_test(adv_payload   "${NS5_COMP}/ble_host/adv_payload.c")
ns5_test(ota_gate      "${NS5_COMP}/ble_ota/ble_ota_gate.c")
ns5_test(adxl345       "${NS5_COMP}/accel_sensor/adxl345.c"
                       "${CMAKE_CURRENT_SOURCE_DIR}/mocks/mock_bus.c")
ns5_test(accel_select  "${NS5_COMP}/accel_sensor/accel_select.c"
                       "${NS5_COMP}/accel_sensor/adxl345.c"
                       "${CMAKE_CURRENT_SOURCE_DIR}/mocks/mock_bus.c")
ns5_test(accel_sim     "${NS5_COMP}/accel_sensor/accel_sim.c")
ns5_test(devinfo       "${NS5_COMP}/device_info/devinfo_validate.c")
ns5_test(reset_reason  "${NS5_COMP}/device_info/reset_reason_map.c")
