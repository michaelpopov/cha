cmake_minimum_required(VERSION 3.20)

cmake_policy(SET CMP0135 NEW)

project(cha LANGUAGES C CXX)

include(CTest)

set(CHA_PACKAGE_VERSION "development" CACHE STRING
    "Version reported by the native application")

find_package(Threads REQUIRED)
# Apple's SDK libcurl exports the WebSocket API but cannot perform ws/wss
# transfers. Use our bundled curl for both development and packaged macOS builds.
# Elsewhere, an older system curl lacks CURLWS_NOAUTOPONG (8.14), so the
# bundled curl replaces it.
if(NOT APPLE)
    find_package(CURL 8.14 QUIET)
endif()
if(WIN32)
    # Static OpenSSL so the application carries no OpenSSL DLLs. Which copy to
    # pick depends on the CRT this build uses: mixing the two puts a separate
    # heap on either side of the static library boundary. The packaged build
    # asks for the static CRT; a plain local build gets the dynamic default.
    set(OPENSSL_USE_STATIC_LIBS TRUE)
    if(CMAKE_MSVC_RUNTIME_LIBRARY MATCHES "^MultiThreaded(Debug)?$")
        set(OPENSSL_MSVC_STATIC_RT TRUE)
    else()
        set(OPENSSL_MSVC_STATIC_RT FALSE)
    endif()
endif()
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
endif()

include(FetchContent)

# Keep fetched runtime dependencies inside the executable. In particular,
# Windows builds must not require a separately copied libcurl DLL.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

if(APPLE OR NOT CURL_FOUND)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
    set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
    set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
    set(HTTP_ONLY ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_WEBSOCKETS OFF CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    if(WIN32)
        set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
    elseif(APPLE)
        # This backend is supported by our pinned curl 8.14.1; upgrading to
        # 8.15 or later requires choosing another TLS backend.
        set(CURL_USE_SECTRANSP ON CACHE BOOL "" FORCE)
        set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
        # Do not pick up a Homebrew nghttp2 dylib in the portable app bundle.
        set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
        # curl 8.14.1's symbol check detects pipe2 in newer SDKs even when
        # deploying to macOS versions before its introduction in macOS 27.
        set(HAVE_PIPE2 0 CACHE INTERNAL "Use curl's portable pipe fallback" FORCE)
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

if(WIN32)
    # WebView2 is the Windows equivalent of the WKWebView used by CHA.app.
    # Only the small SDK and static loader are linked here; the independently
    # serviced Evergreen WebView2 Runtime remains a machine prerequisite.
    FetchContent_Declare(
        webview2_sdk
        URL https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/1.0.4191.47/microsoft.web.webview2.1.0.4191.47.nupkg
        URL_HASH SHA256=f492bbf547d0da329553b6727435b677579b1e9f91cc9e4a1ad029366d5f23d0
        SOURCE_SUBDIR cmake-not-present)
    FetchContent_MakeAvailable(webview2_sdk)

    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(CHA_WEBVIEW2_ARCH x86)
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(CHA_WEBVIEW2_ARCH arm64)
    else()
        set(CHA_WEBVIEW2_ARCH x64)
    endif()

    add_library(cha_webview2_loader STATIC IMPORTED)
    set_target_properties(
        cha_webview2_loader
        PROPERTIES
            IMPORTED_LOCATION
                "${webview2_sdk_SOURCE_DIR}/build/native/${CHA_WEBVIEW2_ARCH}/WebView2LoaderStatic.lib"
            INTERFACE_INCLUDE_DIRECTORIES
                "${webview2_sdk_SOURCE_DIR}/build/native/include")
endif()

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
    sqlcipher_source
    URL https://github.com/sqlcipher/sqlcipher/archive/refs/tags/v4.19.0.zip
    URL_HASH SHA256=57290a5e83858bc8435701ec9be8b1d4f36a0ee1b9e85af1ad8fbb995b058ca2
)
FetchContent_MakeAvailable(sqlcipher_source)

# SQLCipher releases contain the canonical source tree rather than the generated
# SQLite amalgamation. Generate it once at configure time, then compile it with
# the same flags and toolchain as the rest of CHA.
set(sqlcipher_amalgamation_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/_deps/sqlcipher-amalgamation")
file(MAKE_DIRECTORY "${sqlcipher_amalgamation_DIR}")
if(NOT EXISTS "${sqlcipher_amalgamation_DIR}/sqlite3.c")
    if(WIN32)
        find_program(CHA_NMAKE_EXECUTABLE nmake REQUIRED)
        file(TO_NATIVE_PATH "${sqlcipher_source_SOURCE_DIR}" sqlcipher_source_native)
        execute_process(
            COMMAND
                "${CHA_NMAKE_EXECUTABLE}"
                /f "${sqlcipher_source_SOURCE_DIR}/Makefile.msc"
                "TOP=${sqlcipher_source_native}"
                NO_TCL=1
                sqlite3.c
            WORKING_DIRECTORY "${sqlcipher_amalgamation_DIR}"
            RESULT_VARIABLE sqlcipher_generate_result)
    else()
        find_program(CHA_MAKE_EXECUTABLE make REQUIRED)
        execute_process(
            COMMAND
                "${sqlcipher_source_SOURCE_DIR}/configure"
                --disable-shared
                --disable-tcl
                --with-tempstore=yes
            WORKING_DIRECTORY "${sqlcipher_amalgamation_DIR}"
            RESULT_VARIABLE sqlcipher_configure_result)
        if(NOT sqlcipher_configure_result EQUAL 0)
            message(FATAL_ERROR "Failed to configure SQLCipher")
        endif()
        execute_process(
            COMMAND "${CHA_MAKE_EXECUTABLE}" sqlite3.c
            WORKING_DIRECTORY "${sqlcipher_amalgamation_DIR}"
            RESULT_VARIABLE sqlcipher_generate_result)
    endif()
    if(NOT sqlcipher_generate_result EQUAL 0)
        message(FATAL_ERROR "Failed to generate the SQLCipher amalgamation")
    endif()
endif()

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
    "${sqlcipher_amalgamation_DIR}/sqlite3.c"
)
set_target_properties(cha_sqlite3 PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(
    cha_sqlite3
    SYSTEM PUBLIC
        "${sqlcipher_amalgamation_DIR}"
)
target_compile_definitions(
    cha_sqlite3
    PUBLIC
        SQLITE_HAS_CODEC=1
    PRIVATE
        SQLITE_DQS=0
        SQLITE_EXTRA_INIT=sqlcipher_extra_init
        SQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown
        SQLITE_TEMP_STORE=2
        SQLITE_OMIT_DEPRECATED
        SQLITE_OMIT_LOAD_EXTENSION
        SQLITE_THREADSAFE=1
        SQLITE_DEFAULT_FILE_PERMISSIONS=0600
)
target_link_libraries(cha_sqlite3 PUBLIC OpenSSL::Crypto)

add_library(cha_lib STATIC
    ${CMAKE_CURRENT_BINARY_DIR}/generated/application_guide.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/generated/character_voice.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/generated/new_character_template.cpp
    src/util/crypto.cpp
    src/util/curl.cpp
    src/util/private_filesystem.cpp
    src/util/logging.cpp
    src/util/owner_wake_signal.cpp
    src/util/path_name.cpp
    src/util/public_name.cpp
    src/util/text.cpp
    src/util/text_template.cpp
    src/util/text_source.cpp
    src/util/toml_file.cpp
    src/chat/character_metadata.cpp
    src/chat/transcript.cpp
    src/characters/character.cpp
    src/characters/model_context.cpp
    src/providers/api_key_store.cpp
    src/providers/voice_output_config.cpp
    src/providers/providers.cpp
    src/providers/provider_client.cpp
    src/providers/openai_oauth.cpp
    src/providers/chat_completions_api.cpp
    src/providers/responses_api.cpp
    src/providers/sse_framer.cpp
    src/characters/character_config.cpp
    src/session/controller_update.cpp
    src/session/session_controller.cpp
    src/storage/session_database.cpp
    src/storage/session_label.cpp
    src/storage/session_lease.cpp
    src/storage/session_repository.cpp
    src/storage/session_storage_layout.cpp
    src/storage/sqlite_storage.cpp
    src/storage/workspace_session_database.cpp
    src/app/application_config.cpp
    src/providers/fish_audio.cpp
    src/app/r2_database_transfer.cpp
    src/workspace/workspace.cpp
    src/workspace/workspace_config_store.cpp
    src/session/session_open.cpp
    src/app/application.cpp
    src/app/background_jobs.cpp
    src/app/vault_operations.cpp
    src/app/workspace_operations.cpp
    src/app/settings_operations.cpp
    src/runtime/session_output.cpp
    src/media/audio_download.cpp
    src/app/media_operations.cpp
    src/media/media_resources.cpp
    src/media/pending_media_registry.cpp
    src/media/xai_transcript.cpp
    src/media/xai_curl_socket.cpp
    src/media/xai_voice_session.cpp
    src/runtime/command_queue.cpp
    src/runtime/request_parser.cpp
    src/runtime/protocol.cpp
    src/session/session_markdown.cpp
    src/session/session_mirror.cpp
    src/runtime/session_projection.cpp
    src/runtime/live_session.cpp
    src/runtime/live_session_manager.cpp
    src/runtime/text_input.cpp
    src/bridge/bridge_protocol.cpp
    src/bridge/bridge_router.cpp
    src/bridge/settings_dispatch.cpp
    src/bridge/workspace_dispatch.cpp
)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/application_guide.cpp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/resources/application-guide.md -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/application_guide.cpp -DSYMBOL=embedded_application_guide -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
    DEPENDS resources/application-guide.md cmake/embed_text.cmake
    VERBATIM)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/character_voice.cpp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/resources/characters/character-voice.md -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/character_voice.cpp -DSYMBOL=embedded_character_voice -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
    DEPENDS resources/characters/character-voice.md cmake/embed_text.cmake
    VERBATIM)

add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/generated/new_character_template.cpp
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/resources/characters/CHARACTER.md -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/generated/new_character_template.cpp -DSYMBOL=embedded_new_character_template -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
    DEPENDS resources/characters/CHARACTER.md cmake/embed_text.cmake
    VERBATIM)

target_compile_features(cha_lib PUBLIC cxx_std_20)
target_include_directories(cha_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
if(WIN32)
    target_compile_definitions(
        cha_lib
        PUBLIC
            NOMINMAX
            WIN32_LEAN_AND_MEAN)
    target_link_libraries(cha_lib PUBLIC advapi32 PRIVATE bcrypt)
elseif(APPLE)
    find_library(CHA_COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
    target_link_libraries(cha_lib PRIVATE "${CHA_COREFOUNDATION_FRAMEWORK}")
endif()
target_link_libraries(
    cha_lib
    PUBLIC
        CURL::libcurl
        Threads::Threads
        uv_a
        nlohmann_json::nlohmann_json
        tomlplusplus::tomlplusplus
        cha_sqlite3
    PRIVATE
        spdlog::spdlog
)
target_link_libraries(cha_lib PRIVATE OpenSSL::Crypto)
cha_enable_warnings(cha_lib)

target_compile_definitions(
    cha_lib PRIVATE CHA_PACKAGE_VERSION="${CHA_PACKAGE_VERSION}")

# The instrumented Windows host uses this helper for its actual WebView2
# workflow even in packaging builds, where the full GoogleTest suite is off.
if(WIN32 AND NOT BUILD_TESTING)
    add_executable(
        cha_prepare_test_vault EXCLUDE_FROM_ALL
        tests/native/prepare_test_vault.cpp
        tests/support/test_workspace.cpp)
    target_link_libraries(
        cha_prepare_test_vault PRIVATE cha_lib cha_sqlite3)
    target_include_directories(
        cha_prepare_test_vault PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    cha_enable_warnings(cha_prepare_test_vault)
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

    # Outbound-provider test listeners only. Production does not use cpp-httplib.
    set(HTTPLIB_USE_NON_BLOCKING_GETADDRINFO OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        cpp_httplib
        URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.51.0.zip
    )
    FetchContent_MakeAvailable(cpp_httplib)

    set(CHA_TEST_SOURCES
        tests/util/unit_private_filesystem.cpp
        tests/util/unit_logging.cpp
        tests/util/unit_concurrent_queue.cpp
        tests/util/unit_text.cpp
        tests/util/unit_text_template.cpp
        tests/util/unit_public_name.cpp
        tests/util/unit_utf8_path.cpp
        tests/util/unit_toml_file.cpp
        tests/chat/unit_transcript.cpp
        tests/agents/unit_model_context.cpp
        tests/providers/unit_providers.cpp
        tests/providers/unit_api_key_store.cpp
        tests/providers/unit_openai_oauth.cpp
        tests/providers/unit_chat_completions_api.cpp
        tests/providers/unit_responses_api.cpp
        tests/providers/unit_sse_framer.cpp
        tests/session/unit_concurrent_controllers.cpp
        tests/session/unit_session_controller.cpp
        tests/storage/unit_session_lease.cpp
        tests/storage/unit_session_repository.cpp
        tests/storage/unit_workspace_session_database.cpp
        tests/session/unit_controller_update.cpp
        tests/workspace/unit_workspace.cpp
        tests/workspace/unit_workspace_config_store.cpp
        tests/session/unit_session_open.cpp
        tests/workspace/unit_builtins.cpp
        tests/support/test_workspace.cpp
    )
    if(NOT WIN32)
        # The socket fixture used by these transport tests is POSIX-only.
        list(APPEND CHA_TEST_SOURCES
            tests/providers/unit_provider_client.cpp)

        # Lease tests use a fresh executable rather than running C++ code in a
        # forked copy of the potentially threaded GoogleTest process.
        add_executable(
            cha_lease_test_helper
            tests/support/lease_test_helper.cpp)
        target_link_libraries(cha_lease_test_helper PRIVATE cha_lib)
        target_include_directories(
            cha_lease_test_helper PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        cha_enable_warnings(cha_lease_test_helper)
    endif()

    add_executable(cha_tests ${CHA_TEST_SOURCES})
    # cha_sqlite3 lets the session-database tests inspect the stored schema directly.
    target_link_libraries(
        cha_tests
        PRIVATE
            cha_lib
            cha_sqlite3
            GTest::gtest_main)
    target_include_directories(cha_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_definitions(
        cha_tests
        PRIVATE
            CHA_IMPORT_SEED_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/packaging/shared/import-seed")
    if(TARGET cha_lease_test_helper)
        target_compile_definitions(
            cha_tests
            PRIVATE
                CHA_LEASE_TEST_HELPER="$<TARGET_FILE:cha_lease_test_helper>")
        add_dependencies(cha_tests cha_lease_test_helper)
    endif()
    cha_enable_warnings(cha_tests)

    add_executable(
        cha_app_tests
        tests/app/unit_application.cpp
        tests/app/unit_background_jobs.cpp
        tests/media/unit_pending_media_registry.cpp
        tests/media/unit_media_resources.cpp
        tests/app/unit_media_operations.cpp
        tests/app/unit_settings_operations.cpp
        tests/app/unit_vault_maintenance.cpp
        tests/runtime/unit_session_output.cpp
        tests/app/unit_session_retirement.cpp
        tests/providers/unit_fish_audio.cpp
        tests/media/unit_audio_download.cpp
        tests/media/unit_xai_transcript.cpp
        tests/media/unit_xai_voice.cpp
        tests/support/xai_fake_server.cpp
        tests/app/unit_application_config.cpp
        tests/app/unit_r2_database_transfer.cpp
        tests/runtime/unit_protocol.cpp
        tests/runtime/unit_wire_fixtures.cpp
        tests/session/unit_session_markdown.cpp
        tests/session/unit_session_mirror.cpp
        tests/runtime/unit_session_projection.cpp
        tests/runtime/unit_live_session_manager.cpp
        tests/runtime/unit_text_command.cpp
        tests/runtime/unit_text_input.cpp
        tests/runtime/unit_text_mention.cpp
        tests/runtime/unit_text_multicast.cpp
        tests/runtime/unit_live_session.cpp
        tests/support/test_workspace.cpp)
    target_link_libraries(
        cha_app_tests
        PRIVATE
            cha_lib
            cha_sqlite3
            httplib::httplib
            GTest::gtest_main)
    target_include_directories(
        cha_app_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_definitions(
        cha_app_tests
        PRIVATE
            CHA_WIRE_FIXTURE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wire"
            CHA_XAI_FIXTURE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/docs/fixtures/xai")
    if(TARGET cha_lease_test_helper)
        target_compile_definitions(
            cha_app_tests
            PRIVATE
                CHA_LEASE_TEST_HELPER="$<TARGET_FILE:cha_lease_test_helper>")
        add_dependencies(cha_app_tests cha_lease_test_helper)
    endif()
    add_executable(
        cha_xai_fake_server
        tests/media/xai_fake_server_main.cpp
        tests/support/xai_fake_server.cpp)
    target_compile_features(cha_xai_fake_server PRIVATE cxx_std_20)
    target_include_directories(
        cha_xai_fake_server PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_link_libraries(
        cha_xai_fake_server PRIVATE nlohmann_json::nlohmann_json Threads::Threads)
    if(WIN32)
        target_link_libraries(cha_xai_fake_server PRIVATE ws2_32)
    endif()
    cha_enable_warnings(cha_xai_fake_server)
    cha_enable_warnings(cha_app_tests)

    add_executable(
        cha_bridge_tests
        tests/bridge/unit_bridge_protocol.cpp
        tests/bridge/unit_bridge_router.cpp
        tests/support/test_workspace.cpp)
    target_link_libraries(
        cha_bridge_tests PRIVATE cha_lib cha_sqlite3 GTest::gtest_main)
    target_include_directories(
        cha_bridge_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_compile_definitions(
        cha_bridge_tests
        PRIVATE
            CHA_WIRE_FIXTURE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/wire")
    if(TARGET cha_lease_test_helper)
        target_compile_definitions(
            cha_bridge_tests
            PRIVATE
                CHA_LEASE_TEST_HELPER="$<TARGET_FILE:cha_lease_test_helper>")
        add_dependencies(cha_bridge_tests cha_lease_test_helper)
    endif()
    cha_enable_warnings(cha_bridge_tests)

    add_executable(
        cha_native_runtime_tests
        tests/native/unit_runtime_bridge.cpp
        packaging/macos/runtime_bridge.cpp
        tests/support/test_workspace.cpp)
    target_link_libraries(
        cha_native_runtime_tests
        PRIVATE
            cha_lib
            cha_sqlite3
            GTest::gtest_main)
    target_include_directories(
        cha_native_runtime_tests
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/tests"
            "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos")
    cha_enable_warnings(cha_native_runtime_tests)

    add_executable(
        cha_prepare_test_vault
        tests/native/prepare_test_vault.cpp
        tests/support/test_workspace.cpp)
    target_link_libraries(
        cha_prepare_test_vault PRIVATE cha_lib cha_sqlite3)
    target_include_directories(
        cha_prepare_test_vault PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    cha_enable_warnings(cha_prepare_test_vault)

    if(NOT WIN32)
        add_executable(
            cha_interrupted_app_helper
            tests/integration/interrupted_app_helper.cpp)
        target_link_libraries(cha_interrupted_app_helper PRIVATE cha_lib)
        cha_enable_warnings(cha_interrupted_app_helper)
        add_executable(
            itest
            tests/integration/integration_test.cpp
            tests/integration/application_integration_test.cpp
            tests/support/test_workspace.cpp)
        target_link_libraries(
            itest PRIVATE cha_lib GTest::gtest_main)
        target_include_directories(itest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
        target_compile_definitions(itest PRIVATE
            CHA_INTERRUPTED_APP_HELPER="$<TARGET_FILE:cha_interrupted_app_helper>")
        add_dependencies(itest cha_interrupted_app_helper)
        cha_enable_warnings(itest)
    endif()

    # Every test binary carries the sanitizer suppression list. The file is
    # empty unless the build is instrumented, so this costs ordinary builds
    # nothing, and compiling it in avoids relying on the runner to export
    # TSAN_OPTIONS.
    foreach(cha_test_target IN ITEMS
            cha_tests
            cha_app_tests
            cha_bridge_tests
            cha_native_runtime_tests
            itest)
        if(TARGET ${cha_test_target})
            target_sources(
                ${cha_test_target}
                PRIVATE tests/support/sanitizer_suppressions.cpp)
        endif()
    endforeach()

    include(GoogleTest)
    gtest_discover_tests(cha_tests)
    gtest_discover_tests(cha_app_tests PROPERTIES TIMEOUT 60)
    gtest_discover_tests(cha_bridge_tests)
    gtest_discover_tests(cha_native_runtime_tests)
endif()

if(APPLE)
    add_library(
        cha_macos_runtime SHARED
        packaging/macos/runtime_bridge.cpp)
    target_link_libraries(cha_macos_runtime PRIVATE cha_lib)
    target_include_directories(
        cha_macos_runtime PRIVATE packaging/macos)
    set_target_properties(
        cha_macos_runtime
        PROPERTIES
            OUTPUT_NAME ChaRuntime
            INSTALL_NAME_DIR "@rpath"
            BUILD_WITH_INSTALL_RPATH TRUE
            MACOSX_RPATH TRUE)
    cha_enable_warnings(cha_macos_runtime)
endif()
if(WIN32)
    set(CHA_WINDOWS_ICON "${CMAKE_CURRENT_BINARY_DIR}/generated/CHA.ico")
    # Listed as a source, CMake merges the manifest into the executable under
    # both the Ninja and Visual Studio generators. A raw /MANIFESTINPUT link
    # flag works only under the latter.
    set(CHA_WINDOWS_MANIFEST packaging/windows/CHA.manifest)
    configure_file(
        packaging/windows/CHA.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/generated/CHA.rc
        @ONLY)
    add_custom_command(
        OUTPUT ${CHA_WINDOWS_ICON}
        COMMAND
            powershell -NoProfile -ExecutionPolicy Bypass
            -File ${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/make-icon.ps1
            -Output ${CHA_WINDOWS_ICON}
        DEPENDS packaging/windows/make-icon.ps1
        VERBATIM)
    add_executable(
        cha_windows_app WIN32
        packaging/windows/main.cpp
        packaging/macos/runtime_bridge.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/generated/CHA.rc
        ${CHA_WINDOWS_ICON}
        ${CHA_WINDOWS_MANIFEST})
    target_include_directories(
        cha_windows_app PRIVATE packaging/macos)
    # The host calls the wide Win32 entry points exclusively; without UNICODE
    # the resource-id macros (IDC_ARROW and friends) expand to their narrow
    # form and will not convert to LPCWSTR.
    target_compile_definitions(
        cha_windows_app PRIVATE UNICODE _UNICODE)
    target_link_libraries(
        cha_windows_app
        PRIVATE
            cha_lib
            cha_webview2_loader
            comdlg32
            ole32
            shell32
            shlwapi
            user32)
    set_target_properties(cha_windows_app PROPERTIES OUTPUT_NAME CHA)
    cha_enable_warnings(cha_windows_app)

    add_executable(
        cha_windows_test_app WIN32
        packaging/windows/main.cpp
        packaging/macos/runtime_bridge.cpp
        ${CMAKE_CURRENT_BINARY_DIR}/generated/CHA.rc
        ${CHA_WINDOWS_ICON}
        ${CHA_WINDOWS_MANIFEST})
    target_include_directories(
        cha_windows_test_app PRIVATE packaging/macos)
    target_compile_definitions(
        cha_windows_test_app PRIVATE CHA_NATIVE_INSTRUMENTATION UNICODE _UNICODE)
    target_link_libraries(
        cha_windows_test_app
        PRIVATE
            cha_lib
            cha_webview2_loader
            comdlg32
            ole32
            shell32
            shlwapi
            user32)
    set_target_properties(cha_windows_test_app PROPERTIES OUTPUT_NAME CHATest)
    cha_enable_warnings(cha_windows_test_app)
endif()
