cmake_minimum_required(VERSION 3.20)

cmake_policy(SET CMP0135 NEW)

project(cha LANGUAGES C CXX)

include(CTest)

set(CURSES_NEED_WIDE TRUE)
find_package(Curses REQUIRED)
find_library(CURSES_WIDE_LIBRARY NAMES ncursesw REQUIRED)
find_package(Threads REQUIRED)
find_package(CURL QUIET)
find_package(OpenSSL QUIET)

include(FetchContent)

if(NOT CURL_FOUND)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
    set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
    set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
    set(HTTP_ONLY ON CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    if(OpenSSL_FOUND)
        set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
    endif()
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

FetchContent_Declare(
    sqlite_amalgamation
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
    URL_HASH SHA256=77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784
)
FetchContent_MakeAvailable(sqlite_amalgamation)

add_library(cha_sqlite3 STATIC
    "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c"
)
set_target_properties(cha_sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(
    cha_sqlite3
    SYSTEM PUBLIC
        "${sqlite_amalgamation_SOURCE_DIR}"
)
target_compile_definitions(
    cha_sqlite3
    PRIVATE
        SQLITE_DQS=0
        SQLITE_OMIT_DEPRECATED
        SQLITE_OMIT_LOAD_EXTENSION
        SQLITE_THREADSAFE=1
)

add_library(cha_core
    src/agent_identity.cpp
    src/agent_definition.cpp
    src/agent_registry.cpp
    src/agent_roster.cpp
    src/command.cpp
    src/chat_coordinator.cpp
    src/completion_client.cpp
    src/config.cpp
    src/conversation.cpp
    src/environment.cpp
    src/input_editor.cpp
    src/mention.cpp
    src/path_name.cpp
    src/session_database.cpp
    src/agent_context.cpp
    src/session_repository.cpp
    src/startup_selector.cpp
    src/terminal.cpp
    src/text.cpp
    src/transcript_renderer.cpp
    src/tui.cpp
    src/user.cpp
    src/user_events.cpp
    src/user_session.cpp
    src/workspace.cpp
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
    PRIVATE
        cha_sqlite3
)

function(cha_enable_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        target_compile_options(
            ${target}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wno-missing-field-initializers)
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    endif()
endfunction()

cha_enable_warnings(cha_core)

add_executable(cha src/main.cpp)
target_link_libraries(cha PRIVATE cha_core)
cha_enable_warnings(cha)

if(BUILD_TESTING)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(cha_tests
        tests/unit_agent_definition.cpp
        tests/unit_agent_registry.cpp
        tests/unit_agent_roster.cpp
        tests/unit_mention.cpp
        tests/unit_command.cpp
        tests/unit_config.cpp
        tests/unit_chat_coordinator.cpp
        tests/unit_completion_client.cpp
        tests/unit_conversation.cpp
        tests/unit_environment.cpp
        tests/unit_event_channel.cpp
        tests/unit_text.cpp
        tests/unit_transcript_renderer.cpp
        tests/unit_agent_context.cpp
        tests/unit_user.cpp
        tests/unit_user_session.cpp
        tests/unit_workspace.cpp
    )
    # cha_sqlite3 lets the session-database tests inspect the stored schema directly.
    target_link_libraries(cha_tests PRIVATE cha_core cha_sqlite3 GTest::gtest_main)
    cha_enable_warnings(cha_tests)

    # The integration suite, run by "make itest": it drives the real stack against
    # the checked-in workspace, including its live endpoint.
    add_executable(itest tests/integration_test.cpp)
    target_link_libraries(itest PRIVATE cha_core GTest::gtest_main)
    cha_enable_warnings(itest)
    target_compile_definitions(itest PRIVATE CHA_WORKSPACE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/workspace")

    include(GoogleTest)
    gtest_discover_tests(cha_tests)
endif()
