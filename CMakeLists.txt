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
    src/util/environment.cpp
    src/util/path_name.cpp
    src/util/text.cpp
    src/conversation/conversation.cpp
            src/agents/agent_registry.cpp
        src/agents/completion_client.cpp
    src/application/chat_coordinator.cpp
    src/application/session_notices.cpp
    src/application/response_controller.cpp
    src/application/workspace_service.cpp
    src/agents/agent.cpp
    src/agents/config.cpp
    src/storage/session_database.cpp
    src/storage/session_repository.cpp
    src/storage/workspace.cpp
    src/interfaces/text/command.cpp
    src/interfaces/text/mention.cpp
    src/interfaces/text/text_input.cpp
    src/interfaces/terminal/input_editor.cpp
    src/interfaces/terminal/startup_selector.cpp
    src/interfaces/terminal/terminal.cpp
    src/interfaces/terminal/transcript_renderer.cpp
    src/interfaces/terminal/tui.cpp
    src/interfaces/terminal/user.cpp
        src/interfaces/terminal/user_session.cpp
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

add_executable(cha src/apps/tui_main.cpp)
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
        tests/util/unit_environment.cpp
        tests/util/unit_text.cpp
        tests/conversation/unit_conversation.cpp
        tests/agents/unit_agent_context.cpp
        tests/agents/unit_agent_registry.cpp
        tests/agents/unit_agent_roster.cpp
        tests/agents/unit_completion_client.cpp
        tests/util/unit_event_channel.cpp
        tests/application/unit_chat_coordinator.cpp
        tests/application/unit_workspace_service.cpp
        tests/agents/unit_agent_definition_loader.cpp
        tests/agents/unit_config_loader.cpp
        tests/storage/unit_workspace.cpp
        tests/interfaces/text/unit_command.cpp
        tests/interfaces/text/unit_mention.cpp
        tests/interfaces/text/unit_text_input.cpp
        tests/interfaces/terminal/unit_transcript_renderer.cpp
        tests/interfaces/terminal/unit_user.cpp
        tests/interfaces/terminal/unit_user_session.cpp
    )
    # cha_sqlite3 lets the session-database tests inspect the stored schema directly.
    target_link_libraries(cha_tests PRIVATE cha_core cha_sqlite3 GTest::gtest_main)
    target_include_directories(cha_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    cha_enable_warnings(cha_tests)

    # The integration suite, run by "make itest": it drives the real stack against
    # the checked-in workspace, including its live endpoint.
    add_executable(itest tests/integration/integration_test.cpp)
    target_link_libraries(itest PRIVATE cha_core GTest::gtest_main)
    target_include_directories(itest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    cha_enable_warnings(itest)
    target_compile_definitions(itest PRIVATE CHA_WORKSPACE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/workspace")

    include(GoogleTest)
    gtest_discover_tests(cha_tests)
endif()
