cmake_minimum_required(VERSION 3.16)

project(version-server LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)

add_executable(
    version-server
    src/main.cpp
    src/version_update.cpp
)

target_include_directories(
    version-server

    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib
)

target_compile_options(version-server PRIVATE -Wall -Wextra -Wpedantic)

target_link_libraries(
    version-server

    PRIVATE
        OpenSSL::Crypto
        nlohmann_json::nlohmann_json
        Threads::Threads
)
