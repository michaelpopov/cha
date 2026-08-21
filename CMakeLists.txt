cmake_minimum_required(VERSION 3.20)

cmake_policy(SET CMP0135 NEW)

project(cha LANGUAGES C CXX)

include(CTest)

find_package(Threads REQUIRED)
find_package(CURL QUIET)
# Prompt-cache keys use SHA-256, so OpenSSL is required independently of the
# TLS backend libcurl happens to use on this platform.
find_package(OpenSSL REQUIRED)

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

# Keep fetched runtime dependencies inside the executable. In particular,
# Windows builds must not require a separately copied libcurl DLL.
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
        target_compile_options(${target} PRIVATE /W4 /EHsc)
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
    ${CMAKE_CURRENT_BINARY_DIR}/generated/application_guide.cpp
    src/util/environment.cpp
    src/util/logging.cpp
    src/util/path_name.cpp
    src/util/public_name.cpp
    src/util/text.cpp
    src/util/text_template.cpp
    src/util/thread_pool.cpp
    src/chat/character.cpp
    src/chat/transcript.cpp
    src/agents/character.cpp
    src/agents/model_context.cpp
    src/agents/generation_batch.cpp
    src/agents/generation_executor.cpp
    src/agents/provider_client.cpp
    src/agents/provider_response.cpp
    src/agents/responses_api.cpp
    src/agents/sse_framer.cpp
    src/agents/character_config.cpp
    src/session/controller_update.cpp
    src/session/forum_characters.cpp
    src/session/session_archive.cpp
    src/session/session_catalog.cpp
    src/session/session_controller.cpp
    src/session/session_database.cpp
    src/session/session_label.cpp
    src/session/session_lease.cpp
    src/session/session_repository.cpp
    src/session/session_snapshot.cpp
    src/workspace/workspace_definition.cpp
    src/workspace/workspace_runtime.cpp
    src/workspace/session_open.cpp
    src/workspace/builtins.cpp
)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/application_guide.cpp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/resources/application-guide.md -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/application_guide.cpp -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
    DEPENDS resources/application-guide.md cmake/embed_text.cmake
    VERBATIM)

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
        OpenSSL::Crypto
)
cha_enable_warnings(cha_core)

add_library(cha_web STATIC
    src/web/application_config.cpp
    src/web/asset_handler.cpp
    src/web/browser_connection_state.cpp
    src/web/command_queue.cpp
    src/web/http_response.cpp
    src/web/http_server.cpp
    src/web/json.cpp
    src/web/lobby_routes.cpp
    src/web/protocol.cpp
    src/web/route_support.cpp
    src/web/session_markdown.cpp
    src/web/session_projection.cpp
    src/web/server_shutdown.cpp
    src/web/session_routes.cpp
    src/web/live_session.cpp
    src/web/live_session_manager.cpp
    src/web/sse_mailbox.cpp
    src/web/sse_stream.cpp
    src/web/text_command.cpp
    src/web/text_input.cpp
    src/web/text_mention.cpp
    src/web/text_multicast.cpp
    src/web/workspace_backup.cpp
    src/web/owner_wake_signal.cpp
)
target_compile_features(cha_web PUBLIC cxx_std_20)
target_include_directories(cha_web PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(
    cha_web
    PUBLIC
        cha_core
        httplib::httplib
        nlohmann_json::nlohmann_json)
cha_enable_warnings(cha_web)

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
        tests/util/unit_public_name.cpp
        tests/util/unit_utf8_path.cpp
        tests/chat/unit_transcript.cpp
        tests/agents/unit_model_context.cpp
        tests/agents/unit_character_definition_loader.cpp
        tests/agents/unit_generation_batch.cpp
        tests/agents/unit_generation_executor.cpp
        tests/agents/unit_provider_response.cpp
        tests/agents/unit_responses_api.cpp
        tests/agents/unit_sse_framer.cpp
        tests/agents/unit_config_loader.cpp
        tests/session/unit_concurrent_controllers.cpp
        tests/session/unit_forum_characters.cpp
        tests/session/unit_session_archive.cpp
        tests/session/unit_session_catalog.cpp
        tests/session/unit_session_controller.cpp
        tests/session/unit_session_lease.cpp
        tests/session/unit_session_repository.cpp
        tests/session/unit_controller_update.cpp
        tests/application/unit_workspace_definition.cpp
        tests/application/unit_workspace_runtime.cpp
        tests/application/unit_session_open.cpp
        tests/application/unit_builtins.cpp
        tests/support/test_workspace.cpp
    )
    if(NOT WIN32)
        # The socket fixture used by these transport tests is POSIX-only.
        list(APPEND CHA_TEST_SOURCES
            tests/agents/unit_provider_client.cpp)

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
        PRIVATE
            cha_core
            cha_sqlite3
            GTest::gtest_main)
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
        tests/web/unit_application_config.cpp
        tests/web/unit_asset_handler.cpp
        tests/web/unit_protocol.cpp
        tests/web/unit_session_markdown.cpp
        tests/web/unit_session_projection.cpp
        tests/web/unit_browser_connection_state.cpp
        tests/web/unit_lobby_routes.cpp
        tests/web/unit_session_routes.cpp
        tests/web/unit_live_session_manager.cpp
        tests/web/unit_sse_mailbox.cpp
        tests/web/unit_sse_stream.cpp
        tests/web/unit_text_command.cpp
        tests/web/unit_text_input.cpp
        tests/web/unit_text_mention.cpp
        tests/web/unit_text_multicast.cpp
        tests/web/unit_workspace_backup.cpp
        tests/web/unit_live_session.cpp
        tests/support/test_workspace.cpp)
    target_link_libraries(
        cha_web_tests PRIVATE cha_web cha_sqlite3 GTest::gtest_main)
    target_include_directories(cha_web_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    cha_enable_warnings(cha_web_tests)

    add_executable(
        cha_web_stress_tests
        tests/web/stress_web_sessions.cpp
        tests/support/test_workspace.cpp)
    target_link_libraries(
        cha_web_stress_tests PRIVATE cha_web cha_sqlite3 GTest::gtest_main)
    target_include_directories(
        cha_web_stress_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    cha_enable_warnings(cha_web_stress_tests)

    if(NOT WIN32)
        # The live-provider and web process harnesses currently use POSIX
        # sockets and fork/exec, so they are not part of the Windows build.
        add_executable(itest tests/integration/integration_test.cpp)
        target_link_libraries(
            itest PRIVATE cha_core GTest::gtest_main)
        target_include_directories(itest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        cha_enable_warnings(itest)
        target_compile_definitions(itest PRIVATE CHA_WORKSPACE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/workspace")

        add_executable(
            cha_web_process_tests
            tests/web/process_web_server.cpp
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
    if(TARGET cha_web_process_tests)
        gtest_discover_tests(cha_web_process_tests PROPERTIES LABELS web_process)
    endif()
endif()

add_executable(chaweb_app src/web_main.cpp)
target_link_libraries(chaweb_app PRIVATE cha_web)
set_target_properties(chaweb_app PROPERTIES OUTPUT_NAME chaweb)
cha_enable_warnings(chaweb_app)
add_custom_command(
    TARGET chaweb_app
    POST_BUILD
    COMMAND
        ${CMAKE_COMMAND}
        -DEXECUTABLE=$<TARGET_FILE:chaweb_app>
        -DOUTPUT_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/bin
        -DWEBAPP_DIST=${CMAKE_CURRENT_SOURCE_DIR}/webapp/dist
        -DWORKSPACE_DIRECTORY=${CMAKE_CURRENT_SOURCE_DIR}/workspace
        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_local_deployment.cmake
    VERBATIM)
