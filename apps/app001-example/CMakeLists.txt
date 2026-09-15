cmake_minimum_required(VERSION 3.22)
project(qcom_app_example LANGUAGES CXX)

add_executable(qcom_app_example src/main.cpp)
target_compile_features(qcom_app_example PRIVATE cxx_std_17)
target_compile_options(qcom_app_example PRIVATE -Wall -Wextra -Werror)

install(TARGETS qcom_app_example RUNTIME DESTINATION bin)
