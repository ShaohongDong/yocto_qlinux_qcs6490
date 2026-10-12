# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
project(q6a_openhd LANGUAGES CXX)
include(GNUInstallDirs)
include(CTest)
add_library(q6a_raw_protocol src/raw_protocol.cpp src/pcap.cpp)
target_compile_features(q6a_raw_protocol PUBLIC cxx_std_17)
target_include_directories(q6a_raw_protocol PUBLIC src)
target_compile_options(q6a_raw_protocol PRIVATE -Wall -Wextra -Werror)
add_executable(q6a-openhd-probe src/raw_probe.cpp)
target_link_libraries(q6a-openhd-probe PRIVATE q6a_raw_protocol)
target_compile_options(q6a-openhd-probe PRIVATE -Wall -Wextra -Werror)
install(TARGETS q6a-openhd-probe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(PROGRAMS scripts/q6a-openhd-check scripts/q6a-openhd-monitor scripts/q6a-openhd-video-preflight
    scripts/q6a-openhd-driver-trial
    DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES config/baseline.json config/upstream-lock.json
    DESTINATION ${CMAKE_INSTALL_DATADIR}/qcom-selected-app/q6a-openhd)
if(BUILD_TESTING)
    add_executable(raw-protocol-test tests/raw_protocol_test.cpp)
    target_link_libraries(raw-protocol-test PRIVATE q6a_raw_protocol)
    target_compile_options(raw-protocol-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME raw-protocol COMMAND raw-protocol-test)
    add_executable(pcap-test tests/pcap_test.cpp)
    target_link_libraries(pcap-test PRIVATE q6a_raw_protocol)
    target_compile_options(pcap-test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME pcap-capture COMMAND pcap-test)
    add_test(NAME self-test COMMAND q6a-openhd-probe --self-test)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME inventory COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_inventory.py)
    add_test(NAME monitor-restore COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_monitor.py)
    add_test(NAME driver-readiness COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_driver_trial.py)
    add_test(NAME video-bitstream COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_video_analysis.py)
endif()
