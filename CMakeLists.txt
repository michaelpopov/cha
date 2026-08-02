cmake_minimum_required(VERSION 3.20)

cmake_policy(SET CMP0135 NEW)

project(cha LANGUAGES C CXX)

include(CTest)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(CHA_BUILD_TUI_DEFAULT ON)
else()
    set(CHA_BUILD_TUI_DEFAULT OFF)
endif()
option(
    CHA_BUILD_TUI
    "Build the Linux ncurses front end"
    ${CHA_BUILD_TUI_DEFAULT})

if(CHA_BUILD_TUI)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        message(FATAL_ERROR
            "CHA_BUILD_TUI is supported only on Linux; "
            "configure with -DCHA_BUILD_TUI=OFF to build chacon")
    endif()
    set(CURSES_NEED_WIDE TRUE)
    find_package(Curses REQUIRED)
    find_library(CURSES_WIDE_LIBRARY NAMES ncursesw REQUIRED)
endif()

find_package(Threads REQUIRED)
find_package(CURL QUIET)
if(WIN32 OR APPLE)
    find_package(OpenSSL QUIET)
else()
    # Unix-like builds require OpenSSL so HTTPS is always available.
    find_package(OpenSSL REQUIRED)
endif()

# Keep sanitizer instrumentation opt-in so ordinary release-like builds stay
# portable, while CI and local hardening runs use named, reproducible presets.
# This must precede FetchContent so the fetched dependencies are instrumented
# too: ThreadSanitizer reports false races when synchronization inside
# uninstrumented libraries — libuv and the HTTP transport in particular — is
# invisible to it.
set(
    CHA_SANITIZER "none" CACHE STRING
    "Instrumentation: none, address-undefined, or thread")
set_property(CACHE CHA_SANITIZER PROPERTY STRINGS none address-undefined thread)
if(NOT CHA_SANITIZER STREQUAL "none")
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        message(FATAL_ERROR
            "CHA_SANITIZER=${CHA_SANITIZER} requires a GNU or Clang-family compiler")
    endif()
    if(CHA_SANITIZER STREQUAL "address-undefined")
        set(CHA_SANITIZER_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer)
    elseif(CHA_SANITIZER STREQUAL "thread")
        set(CHA_SANITIZER_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
    else()
        message(FATAL_ERROR
            "Unknown CHA_SANITIZER: ${CHA_SANITIZER}")
    endif()
    add_compile_options(${CHA_SANITIZER_FLAGS})
    add_link_options(${CHA_SANITIZER_FLAGS})

    # cpp-httplib's non-blocking resolver calls glibc getaddrinfo_a, which
    # services requests on threads created inside libanl rather than through
    # pthread_create. ThreadSanitizer cannot intercept those, and every test
    # that issues an HTTP request dies on a null dereference before it runs.
    # The blocking resolver is behaviourally equivalent here: chaweb resolves
    # one bind address at startup, and no production code path uses the
    # cpp-httplib client. Instrumented builds therefore use it.
    set(HTTPLIB_USE_NON_BLOCKING_GETADDRINFO OFF CACHE BOOL "" FORCE)
endif()

include(FetchContent)

# Keep fetched runtime dependencies inside the executables. In particular,
# Windows console builds must not require a separately copied libcurl DLL.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

if(NOT CURL_FOUND)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
    set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
    set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
    set(HTTP_ONLY ON CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    if(WIN32)
        set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
    elseif(APPLE)
        set(CURL_USE_SECTRANSP ON CACHE BOOL "" FORCE)
    else()
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
    cpp_httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.51.0.zip
)
FetchContent_MakeAvailable(cpp_httplib)

set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_DISABLE_DEFAULT_LOGGER ON CACHE BOOL "" FORCE)
if(WIN32)
    set(SPDLOG_WCHAR_FILENAMES ON CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(
    spdlog
    URL https://github.com/gabime/spdlog/archive/refs/tags/v1.17.0.zip
    URL_HASH SHA256=b11912a82d149792fef33fabd0503b13d54aeac25c1464755461d4108ea71fc2
)
FetchContent_MakeAvailable(spdlog)

set(LIBUV_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(ENABLE_CLANG_TIDY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    libuv
    URL https://github.com/libuv/libuv/archive/refs/tags/v1.52.1.zip
    URL_HASH SHA256=d56767702c5b383a78ee9124dd7bdf684f14c681f725117c3f865637a7152cba
)
FetchContent_MakeAvailable(libuv)

FetchContent_Declare(
    sqlite_amalgamation
    URL https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip
    URL_HASH SHA256=77823cb110929c2bcb0f5d48e4833b5c59a8a6e40cdea3936b99e199dbbe5784
)
FetchContent_MakeAvailable(sqlite_amalgamation)

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

add_library(cha_core STATIC
    src/util/environment.cpp
    src/util/logging.cpp
    src/util/path_name.cpp
    src/util/text.cpp
    src/util/text_template.cpp
    src/util/thread_pool.cpp
    src/util/utf8_path.cpp
    src/util/uv_event_loop.cpp
    src/transcript/transcript.cpp
    src/agents/agent.cpp
    src/agents/agent_registry.cpp
    src/agents/completion_client.cpp
    src/agents/config.cpp
    src/session/forum_characters.cpp
    src/session/session_catalog.cpp
    src/session/session_controller.cpp
    src/session/session_database.cpp
    src/session/session_lease.cpp
    src/session/workspace.cpp
    src/ui/text/command.cpp
    src/ui/text/mcast.cpp
    src/ui/text/mention.cpp
    src/ui/text/text_input.cpp
    src/ui/console/console_writer.cpp
    src/ui/console/console_startup.cpp
    src/ui/console/console_session.cpp
    src/ui/console/line_reader.cpp
    src/ui/console/system_console.cpp
    src/ui/console/transcript_emitter.cpp
    src/ui/render/transcript_writer.cpp
)

target_compile_features(cha_core PUBLIC cxx_std_20)
target_include_directories(cha_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(WIN32)
    target_compile_definitions(
        cha_core
        PUBLIC
            NOMINMAX
            WIN32_LEAN_AND_MEAN)
endif()
target_link_libraries(
    cha_core
    PUBLIC
        CURL::libcurl
        Threads::Threads
        uv_a
        nlohmann_json::nlohmann_json
        tomlplusplus::tomlplusplus
    PRIVATE
        cha_sqlite3
        spdlog::spdlog
)
cha_enable_warnings(cha_core)

add_library(cha_web STATIC
    src/ui/web/asset_handler.cpp
    src/ui/web/browser_connection_state.cpp
    src/ui/web/command_queue.cpp
    src/ui/web/http_response.cpp
    src/ui/web/http_server.cpp
    src/ui/web/json.cpp
    src/ui/web/lobby_routes.cpp
    src/ui/web/protocol.cpp
    src/ui/web/route_support.cpp
    src/ui/web/server_shutdown.cpp
    src/ui/web/session_routes.cpp
    src/ui/web/session_registry.cpp
    src/ui/web/sse_mailbox.cpp
    src/ui/web/sse_stream.cpp
    src/ui/web/wake_notifier.cpp
    src/ui/web/web_session_runtime.cpp
)
target_compile_features(cha_web PUBLIC cxx_std_20)
target_include_directories(cha_web PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(cha_web PUBLIC cha_core httplib::httplib)
cha_enable_warnings(cha_web)

if(CHA_BUILD_TUI)
    add_library(cha_tui STATIC
        src/ui/tui/input_editor.cpp
        src/ui/tui/render_plan.cpp
        src/ui/tui/screen_layout.cpp
        src/ui/tui/startup_selector.cpp
        src/ui/tui/terminal.cpp
        src/ui/tui/tui.cpp
        src/ui/tui/persona.cpp
        src/ui/tui/persona_session.cpp
    )
    target_link_libraries(cha_tui PUBLIC cha_core ${CURSES_WIDE_LIBRARY})
    target_include_directories(cha_tui SYSTEM PUBLIC ${CURSES_INCLUDE_DIRS})
    target_compile_definitions(cha_tui PUBLIC _XOPEN_SOURCE_EXTENDED)
    cha_enable_warnings(cha_tui)

    add_executable(cha_tui_app src/apps/tui_main.cpp)
    target_link_libraries(cha_tui_app PRIVATE cha_tui)
    set_target_properties(cha_tui_app PROPERTIES OUTPUT_NAME cha)
    cha_enable_warnings(cha_tui_app)
endif()

if(BUILD_TESTING)
    if(MSVC)
        # cha_tests uses MSVC's shared CRT by default. Keep the statically
        # built GoogleTest libraries on that same CRT to avoid RuntimeLibrary
        # linker mismatches.
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    set(CHA_TEST_SOURCES
        tests/util/unit_environment.cpp
        tests/util/unit_logging.cpp
        tests/util/unit_thread_pool.cpp
        tests/util/unit_concurrent_queue.cpp
        tests/util/unit_text.cpp
        tests/util/unit_text_template.cpp
        tests/util/unit_utf8_path.cpp
        tests/util/unit_uv_event_loop.cpp
        tests/transcript/unit_transcript.cpp
        tests/agents/unit_agent_context.cpp
        tests/agents/unit_agent_definition_loader.cpp
        tests/agents/unit_agent_registry.cpp
        tests/agents/unit_config_loader.cpp
        tests/agents/unit_json_serialization.cpp
        tests/session/unit_concurrent_controllers.cpp
        tests/session/unit_forum_characters.cpp
        tests/session/unit_session_catalog.cpp
        tests/session/unit_session_controller.cpp
        tests/session/unit_session_lease.cpp
        tests/session/unit_persona_loader.cpp
        tests/session/unit_workspace.cpp
        tests/ui/text/unit_command.cpp
        tests/ui/text/unit_mcast.cpp
        tests/ui/text/unit_mention.cpp
        tests/ui/text/unit_text_input.cpp
        tests/ui/render/unit_transcript_writer.cpp
        tests/ui/console/unit_console_writer.cpp
        tests/ui/console/unit_console_session.cpp
        tests/ui/console/unit_console_startup.cpp
        tests/ui/console/unit_line_reader.cpp
        tests/ui/console/unit_transcript_emitter.cpp
    )
    if(CHA_BUILD_TUI)
        list(APPEND CHA_TEST_SOURCES
            tests/ui/tui/unit_render_plan.cpp
            tests/ui/tui/unit_screen_layout.cpp
            tests/ui/tui/unit_input_editor.cpp
            tests/ui/tui/unit_persona_session.cpp
        )
    endif()
    if(NOT WIN32)
        # The socket fixture used by these transport tests is POSIX-only.
        list(APPEND CHA_TEST_SOURCES
            tests/agents/unit_completion_client.cpp)

        # Lease tests use a fresh executable rather than running C++ code in a
        # forked copy of the potentially threaded GoogleTest process.
        add_executable(
            cha_lease_test_helper
            tests/support/lease_test_helper.cpp)
        target_link_libraries(cha_lease_test_helper PRIVATE cha_core)
        target_include_directories(
            cha_lease_test_helper PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        cha_enable_warnings(cha_lease_test_helper)
    endif()

    add_executable(cha_tests ${CHA_TEST_SOURCES})
    # cha_sqlite3 lets the session-database tests inspect the stored schema directly.
    target_link_libraries(
        cha_tests
        PRIVATE cha_core cha_sqlite3 GTest::gtest_main)
    if(CHA_BUILD_TUI)
        target_link_libraries(cha_tests PRIVATE cha_tui)
    endif()
    target_include_directories(cha_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    if(TARGET cha_lease_test_helper)
        target_compile_definitions(
            cha_tests
            PRIVATE
                CHA_LEASE_TEST_HELPER="$<TARGET_FILE:cha_lease_test_helper>")
        add_dependencies(cha_tests cha_lease_test_helper)
    endif()
    cha_enable_warnings(cha_tests)

    add_executable(
        cha_web_tests
        tests/ui/web/unit_protocol.cpp
        tests/ui/web/unit_browser_connection_state.cpp
        tests/ui/web/unit_lobby_routes.cpp
        tests/ui/web/unit_session_routes.cpp
        tests/ui/web/unit_session_registry.cpp
        tests/ui/web/unit_sse_mailbox.cpp
        tests/ui/web/unit_sse_stream.cpp
        tests/ui/web/unit_web_session_runtime.cpp
        tests/support/test_workspace.cpp)
    target_link_libraries(
        cha_web_tests PRIVATE cha_web cha_sqlite3 GTest::gtest_main)
    target_include_directories(cha_web_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    cha_enable_warnings(cha_web_tests)

    add_executable(
        cha_web_stress_tests
        tests/ui/web/stress_web_sessions.cpp
        tests/support/test_workspace.cpp)
    target_link_libraries(
        cha_web_stress_tests PRIVATE cha_web cha_sqlite3 GTest::gtest_main)
    target_include_directories(
        cha_web_stress_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    cha_enable_warnings(cha_web_stress_tests)

    if(NOT WIN32)
        # These integration harnesses currently use POSIX sockets and
        # fork/exec. They test the portable console implementation on Linux
        # and macOS but are not part of the Windows build.
        add_executable(itest tests/integration/integration_test.cpp)
        target_link_libraries(itest PRIVATE cha_core GTest::gtest_main)
        target_include_directories(itest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        cha_enable_warnings(itest)
        target_compile_definitions(itest PRIVATE CHA_WORKSPACE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/workspace")

        add_executable(console_tests tests/integration/console_process_test.cpp)
        target_link_libraries(console_tests PRIVATE cha_core GTest::gtest_main)
        target_include_directories(console_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        target_compile_definitions(
            console_tests
            PRIVATE
                CHA_CONSOLE_BINARY="$<TARGET_FILE:chacon_app>"
                CHA_LEASE_TEST_HELPER="$<TARGET_FILE:cha_lease_test_helper>")
        add_dependencies(
            console_tests chacon_app cha_lease_test_helper)
        cha_enable_warnings(console_tests)

        add_executable(
            cha_web_process_tests
            tests/ui/web/process_web_server.cpp
            tests/support/test_workspace.cpp
            tests/support/web_server_process.cpp)
        target_link_libraries(cha_web_process_tests PRIVATE cha_web GTest::gtest_main)
        target_include_directories(
            cha_web_process_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        target_compile_definitions(
            cha_web_process_tests
            PRIVATE CHA_WEB_BINARY="$<TARGET_FILE:chaweb_app>")
        add_dependencies(cha_web_process_tests chaweb_app)
        cha_enable_warnings(cha_web_process_tests)
    endif()

    # Every test binary carries the sanitizer suppression list. The file is
    # empty unless the build is instrumented, so this costs ordinary builds
    # nothing, and compiling it in avoids relying on the runner to export
    # TSAN_OPTIONS.
    foreach(cha_test_target IN ITEMS
            cha_tests
            cha_web_tests
            cha_web_stress_tests
            itest
            console_tests
            cha_web_process_tests)
        if(TARGET ${cha_test_target})
            target_sources(
                ${cha_test_target}
                PRIVATE tests/support/sanitizer_suppressions.cpp)
        endif()
    endforeach()

    include(GoogleTest)
    gtest_discover_tests(cha_tests)
    gtest_discover_tests(cha_web_tests)
    gtest_discover_tests(cha_web_stress_tests PROPERTIES LABELS web_stress)
    if(TARGET console_tests)
        gtest_discover_tests(console_tests)
    endif()
    if(TARGET cha_web_process_tests)
        gtest_discover_tests(cha_web_process_tests PROPERTIES LABELS web_process)
    endif()
endif()

add_executable(chacon_app src/apps/console_main.cpp)
target_link_libraries(chacon_app PRIVATE cha_core)
set_target_properties(chacon_app PROPERTIES OUTPUT_NAME chacon)
cha_enable_warnings(chacon_app)

add_executable(chaweb_app src/apps/web_main.cpp)
target_link_libraries(chaweb_app PRIVATE cha_web)
set_target_properties(chaweb_app PROPERTIES OUTPUT_NAME chaweb)
cha_enable_warnings(chaweb_app)
