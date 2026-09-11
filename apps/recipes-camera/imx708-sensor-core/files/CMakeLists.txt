# SPDX-License-Identifier: GPL-2.0-only
cmake_minimum_required(VERSION 3.22)
project(imx708_sensor_core LANGUAGES CXX)
include(GNUInstallDirs)
include(CTest)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(IMX708_REFERENCE_SOURCE "" CACHE FILEPATH "Pinned upstream imx708.c")
if(NOT EXISTS "${IMX708_REFERENCE_SOURCE}")
    message(FATAL_ERROR "Set IMX708_REFERENCE_SOURCE to the pinned upstream imx708.c")
endif()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/registers.hpp"
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/generate-registers.py"
            "${IMX708_REFERENCE_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/registers.hpp"
    DEPENDS "${IMX708_REFERENCE_SOURCE}" generate-registers.py VERBATIM)
add_library(imx708_sensor_core STATIC sensor.cpp "${CMAKE_CURRENT_BINARY_DIR}/registers.hpp")
set_target_properties(imx708_sensor_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(imx708_sensor_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
                           PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(imx708_sensor_core PUBLIC cxx_std_17)
target_compile_options(imx708_sensor_core PRIVATE -Wall -Wextra -Werror)
add_executable(imx708-register-dump dump.cpp)
target_link_libraries(imx708-register-dump PRIVATE imx708_sensor_core)
install(TARGETS imx708_sensor_core imx708-register-dump
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES sensor.hpp DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/imx708)
set(IMX708_CHI_CDK "" CACHE PATH "External reference CHI-CDK (candidate ABI only)")
if(IMX708_CHI_CDK)
    add_library(imx708_chi SHARED chi_adapter.cpp)
    target_include_directories(imx708_chi PRIVATE "${IMX708_CHI_CDK}/api/sensor"
                               "${IMX708_CHI_CDK}/api/common")
    target_compile_definitions(imx708_chi PRIVATE _LINUX)
    target_link_libraries(imx708_chi PRIVATE imx708_sensor_core)
    target_compile_options(imx708_chi PRIVATE -Wall -Wextra -Werror)
    set_target_properties(imx708_chi PROPERTIES PREFIX "" OUTPUT_NAME "com.qti.sensor.imx708_wide")
    # Reference candidate stays in the build tree; production packaging requires
    # verified CamX format/ABI compatibility and a matching sensor module binary.
    add_executable(chi-callback-test chi_test.cpp)
    target_compile_definitions(chi-callback-test PRIVATE _LINUX)
    target_include_directories(chi-callback-test PRIVATE "${IMX708_CHI_CDK}/api/sensor"
                               "${IMX708_CHI_CDK}/api/common")
    target_link_libraries(chi-callback-test PRIVATE ${CMAKE_DL_LIBS})
    if(BUILD_TESTING)
        add_test(NAME chi-exposure-callbacks COMMAND chi-callback-test $<TARGET_FILE:imx708_chi>)
    endif()
endif()
if(BUILD_TESTING)
    add_executable(sensor-test sensor_test.cpp)
    target_link_libraries(sensor-test PRIVATE imx708_sensor_core)
    add_test(NAME sensor-registers-and-controls COMMAND sensor-test)
    add_test(NAME reference-integrity COMMAND Python3::Interpreter
             "${CMAKE_CURRENT_SOURCE_DIR}/test_generator.py" "${IMX708_REFERENCE_SOURCE}")
endif()
