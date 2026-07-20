cmake_minimum_required(VERSION 3.20)

cmake_policy(SET CMP0135 NEW)

project(cha LANGUAGES CXX)

include(CTest)

set(CURSES_NEED_WIDE TRUE)
find_package(Curses REQUIRED)
find_library(CURSES_WIDE_LIBRARY NAMES ncursesw REQUIRED)
find_package(Threads REQUIRED)
find_package(CURL QUIET)

include(FetchContent)

if(NOT CURL_FOUND)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
    set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
    set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
    set(HTTP_ONLY ON CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
    set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
    set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
    set(CURL_ZSTD OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        curl
        URL https://github.com/curl/curl/archive/refs/tags/curl-8_14_1.zip
    )
    FetchContent_MakeAvailable(curl)
endif()

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nlohmann_json
    URL https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.zip
)
FetchContent_MakeAvailable(nlohmann_json)

FetchContent_Declare(
    tomlplusplus
    URL https://github.com/marzer/tomlplusplus/archive/refs/tags/v3.4.0.zip
)
FetchContent_MakeAvailable(tomlplusplus)

add_library(cha_core
    src/command.cpp
    src/config.cpp
    src/conversation.cpp
    src/conversation_file.cpp
    src/environment.cpp
    src/input_editor.cpp
    src/pipe.cpp
    src/server.cpp
    src/server_context.cpp
    src/tui.cpp
    src/user.cpp
    src/user_events.cpp
    src/user_session.cpp
)

target_compile_features(cha_core PUBLIC cxx_std_20)
target_compile_definitions(cha_core PUBLIC _XOPEN_SOURCE_EXTENDED)
target_include_directories(cha_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(cha_core SYSTEM PUBLIC ${CURSES_INCLUDE_DIRS})
target_link_libraries(
    cha_core
    PUBLIC
        ${CURSES_WIDE_LIBRARY}
        CURL::libcurl
        Threads::Threads
        nlohmann_json::nlohmann_json
        tomlplusplus::tomlplusplus
)

add_executable(cha src/main.cpp)
target_link_libraries(cha PRIVATE cha_core)

if(BUILD_TESTING)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(cha_tests
        tests/unit_config.cpp
        tests/unit_conversation.cpp
        tests/unit_environment.cpp
        tests/unit_pipe.cpp
        tests/unit_server.cpp
        tests/unit_server_context.cpp
        tests/unit_user.cpp
    )
    target_link_libraries(cha_tests PRIVATE cha_core GTest::gtest_main)

    add_executable(itest tests/integration_test.cpp)
    target_link_libraries(itest PRIVATE cha_core GTest::gtest_main)

    include(GoogleTest)
    gtest_discover_tests(cha_tests)
endif()
