#include "runtime_bridge.h"

#include "util/path_name.h"
#include "util/private_filesystem.h"

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl.h>

#include <WebView2.h>
#include <WebView2EnvironmentOptions.h>

#include <nlohmann/json.hpp>
#include <shlwapi.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;

namespace {

constexpr wchar_t kApplicationName[] = L"CHA";
constexpr wchar_t kWindowClass[] = L"CHA.MainWindow";
constexpr wchar_t kApplicationId[] = L"com.michaelpopov.cha";
constexpr int kApplicationIcon = 101;

constexpr UINT kFileExit = 1001;
constexpr UINT kDatabaseImport = 1101;
constexpr UINT kDatabaseExport = 1102;
constexpr UINT kDatabaseUpload = 1103;
constexpr UINT kDatabaseDownload = 1104;
constexpr UINT kOperationComplete = WM_APP + 1;
constexpr UINT kFatalError = WM_APP + 2;
constexpr UINT kDeliveryReady = WM_APP + 3;
constexpr UINT kShutdownDone = WM_APP + 4;
constexpr UINT kNativeSaveComplete = WM_APP + 5;
constexpr wchar_t kPasswordWindowClass[] = L"CHA.PasswordDialog";

class LaunchCancelled final : public std::exception {
public:
    const char* what() const noexcept override { return "Vault unlock cancelled"; }
};

std::wstring wide_from_utf8(std::string_view value) {
    if (value.empty()) return {};
    const int required = ::MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (required == 0) {
        throw std::runtime_error("Failed to convert UTF-8 text");
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            required) == 0) {
        throw std::runtime_error("Failed to convert UTF-8 text");
    }
    return result;
}

std::wstring hresult_message(HRESULT result, std::wstring_view action) {
    wchar_t* detail = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(result),
        0,
        reinterpret_cast<wchar_t*>(&detail),
        0,
        nullptr);
    std::wstring message(action);
    if (length != 0 && detail != nullptr) {
        message += L": ";
        message.append(detail, length);
        while (!message.empty()
               && (message.back() == L'\r' || message.back() == L'\n')) {
            message.pop_back();
        }
    }
    if (detail != nullptr) ::LocalFree(detail);
    return message;
}

std::filesystem::path local_application_data() {
    wchar_t* value = nullptr;
    const HRESULT result = ::SHGetKnownFolderPath(
        FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &value);
    if (FAILED(result) || value == nullptr) {
        if (value != nullptr) ::CoTaskMemFree(value);
        throw std::runtime_error("Failed to locate local application data");
    }
    const std::filesystem::path path(value);
    ::CoTaskMemFree(value);
    return path;
}

void ensure_private_directory(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, error);
    if (!error && status.type() != std::filesystem::file_type::not_found) {
        cha::require_directory(path);
        return;
    }
    if (error && error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error(
            "Failed to inspect directory '" + cha::utf8_path(path) + "'");
    }
    cha::create_private_directory(path);
}

bool starts_with_case_insensitive(
    std::wstring_view value,
    std::wstring_view prefix) {
    return value.size() >= prefix.size()
        && ::CompareStringOrdinal(
               value.data(),
               static_cast<int>(prefix.size()),
               prefix.data(),
               static_cast<int>(prefix.size()),
               TRUE)
            == CSTR_EQUAL;
}

std::wstring take_com_string(wchar_t* value) {
    const std::wstring result = value == nullptr ? std::wstring{} : value;
    if (value != nullptr) ::CoTaskMemFree(value);
    return result;
}

std::string take_bridge_error(char* value) {
    const std::string result = value == nullptr
        ? "CHA encountered an unknown error."
        : std::string(value);
    cha_string_free(value);
    return result;
}

struct PasswordDialogState {
    HWND edit{};
    std::wstring password;
    bool accepted{};
};

LRESULT CALLBACK password_dialog_procedure(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<PasswordDialogState*>(
        ::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<PasswordDialogState*>(create->lpCreateParams);
        ::SetWindowLongPtrW(
            window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) {
        return ::DefWindowProcW(window, message, wparam, lparam);
    }
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wparam) == IDOK) {
            const int length = ::GetWindowTextLengthW(state->edit);
            state->password.resize(static_cast<std::size_t>(length) + 1);
            if (length != 0) {
                ::GetWindowTextW(state->edit, state->password.data(), length + 1);
            }
            state->password.resize(static_cast<std::size_t>(length));
            state->accepted = true;
            ::DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wparam) == IDCANCEL) {
            ::DestroyWindow(window);
            return 0;
        }
        break;
    case WM_CLOSE:
        ::DestroyWindow(window);
        return 0;
    default:
        break;
    }
    return ::DefWindowProcW(window, message, wparam, lparam);
}

std::optional<std::string> prompt_for_vault_password(
    HINSTANCE instance, HWND owner, std::wstring_view vault_name, std::wstring_view error) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = password_dialog_procedure;
        window_class.hInstance = instance;
        window_class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = kPasswordWindowClass;
        if (::RegisterClassExW(&window_class) == 0
            && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error("Failed to create the password prompt");
        }
        registered = true;
    }

    PasswordDialogState state;
    const int width = 420;
    const int height = 175;
    RECT owner_bounds{};
    ::GetWindowRect(owner, &owner_bounds);
    const int x = owner_bounds.left
        + ((owner_bounds.right - owner_bounds.left) - width) / 2;
    const int y = owner_bounds.top
        + ((owner_bounds.bottom - owner_bounds.top) - height) / 2;
    const std::wstring title = L"Open vault \u201c" + std::wstring(vault_name) + L"\u201d";
    HWND dialog = ::CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        kPasswordWindowClass,
        title.c_str(),
        WS_CAPTION | WS_SYSMENU | WS_POPUP,
        x,
        y,
        width,
        height,
        owner,
        nullptr,
        instance,
        &state);
    if (dialog == nullptr) {
        throw std::runtime_error("Failed to create the password prompt");
    }
    const std::wstring message = error.empty()
        ? L"Enter the vault password."
        : std::wstring(error);
    const HFONT font = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    HWND label = ::CreateWindowExW(
        0, L"STATIC", message.c_str(), WS_CHILD | WS_VISIBLE,
        18, 16, 380, 34, dialog, nullptr, instance, nullptr);
    state.edit = ::CreateWindowExW(
        WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP
            | ES_PASSWORD | ES_AUTOHSCROLL,
        18, 54, 380, 24, dialog, nullptr, instance, nullptr);
    HWND open = ::CreateWindowExW(
        0, L"BUTTON", L"Open vault", WS_CHILD | WS_VISIBLE | WS_TABSTOP
            | BS_DEFPUSHBUTTON,
        220, 96, 86, 26, dialog,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), instance, nullptr);
    HWND cancel = ::CreateWindowExW(
        0, L"BUTTON", L"Quit", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        312, 96, 86, 26, dialog,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), instance, nullptr);
    for (HWND control : {label, state.edit, open, cancel}) {
        ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }

    ::EnableWindow(owner, FALSE);
    ::ShowWindow(dialog, SW_SHOW);
    ::SetFocus(state.edit);
    MSG message_value{};
    while (::IsWindow(dialog)) {
        const BOOL result = ::GetMessageW(&message_value, nullptr, 0, 0);
        if (result <= 0) break;
        if (!::IsDialogMessageW(dialog, &message_value)) {
            ::TranslateMessage(&message_value);
            ::DispatchMessageW(&message_value);
        }
    }
    ::EnableWindow(owner, TRUE);
    ::SetForegroundWindow(owner);
    if (!state.accepted) return std::nullopt;
    return cha::utf8_from_wide(state.password);
}

enum class DatabaseOperation {
    import_configuration,
    export_configuration,
    upload,
    download,
};

std::wstring operation_title(DatabaseOperation operation) {
    switch (operation) {
    case DatabaseOperation::import_configuration: return L"Import";
    case DatabaseOperation::export_configuration: return L"Export";
    case DatabaseOperation::upload: return L"Upload";
    case DatabaseOperation::download: return L"Download";
    }
    return L"Database operation";
}

std::wstring operation_progress_title(DatabaseOperation operation) {
    switch (operation) {
    case DatabaseOperation::import_configuration: return L"Importing";
    case DatabaseOperation::export_configuration: return L"Exporting";
    case DatabaseOperation::upload: return L"Uploading";
    case DatabaseOperation::download: return L"Downloading";
    }
    return L"Working";
}

bool operation_reloads_application(DatabaseOperation operation) {
    return operation == DatabaseOperation::import_configuration
        || operation == DatabaseOperation::download;
}

struct OperationResult {
    DatabaseOperation operation{};
    int32_t status{};
    std::uint64_t count{};
    std::string error;
};

struct NativeSaveResult {
    std::string connection_id;
    std::string id;
    uint64_t context_epoch{};
    bool ok{};
    std::string message;
};

struct LaunchOptions {
    bool smoke_test{};
    std::optional<std::filesystem::path> data_root;
    std::optional<std::filesystem::path> assets;
    std::optional<int> cdp_port;
    std::optional<std::wstring> dev_origin;
};

struct DeliveryPayload {
    std::string connection_id;
    std::string json;
};

std::wstring without_fragment(std::wstring_view uri) {
    const auto pos = uri.find(L'#');
    return std::wstring(pos == std::wstring_view::npos ? uri : uri.substr(0, pos));
}

std::wstring native_bootstrap_script(std::string_view connection_id) {
    const std::wstring id = wide_from_utf8(connection_id);
    return L"window.__CHA_NATIVE_CONNECTION_ID__='" + id + L"';"
        L"window.__CHA_NATIVE_CONTEXT_EPOCH__=0;"
        L"window.__CHA_NATIVE_QUEUE__=window.__CHA_NATIVE_QUEUE__||[];"
        L"if(typeof window.__CHA_NATIVE_RECEIVE__!=='function'){"
        L"window.__CHA_NATIVE_RECEIVE__=function(batch){"
        L"window.__CHA_NATIVE_QUEUE__.push(batch);};}"
        L"window.__CHA_NATIVE_POST__=function(message){"
        L"if(window.chrome&&window.chrome.webview){"
        L"window.chrome.webview.postMessage(message);}};"
        L"window.__CHA_NATIVE_SAVE_PENDING__={};"
        L"window.__CHA_NATIVE_SAVE_SEQ__=0;"
        L"window.__CHA_NATIVE_SAVE_SESSION__=function(suggestedName,forumId,sessionId){"
        L"return new Promise(function(resolve,reject){"
        L"var id=String(++window.__CHA_NATIVE_SAVE_SEQ__);"
        L"window.__CHA_NATIVE_SAVE_PENDING__[id]={resolve:resolve,reject:reject};"
        L"window.__CHA_NATIVE_POST__(JSON.stringify({"
        L"native_action:'save_session',id:id,"
        L"connection_id:window.__CHA_NATIVE_CONNECTION_ID__,"
        L"context_epoch:window.__CHA_NATIVE_CONTEXT_EPOCH__,"
        L"suggested_name:suggestedName,"
        L"forum_id:forumId,session_id:sessionId"
        L"}));});};"
        L"window.__CHA_NATIVE_SAVE_DONE__=function(id,ok,message){"
        L"var pending=window.__CHA_NATIVE_SAVE_PENDING__[id];"
        L"if(!pending)return;"
        L"delete window.__CHA_NATIVE_SAVE_PENDING__[id];"
        L"if(ok)pending.resolve();else pending.reject(new Error(message||'Save failed'));};"
        L"if(window.chrome&&window.chrome.webview){"
        L"window.chrome.webview.addEventListener('message',function(event){"
        L"var batch=event.data;"
        L"if(typeof window.__CHA_NATIVE_RECEIVE__==='function'){"
        L"window.__CHA_NATIVE_RECEIVE__(batch);}"
        L"else{(window.__CHA_NATIVE_QUEUE__=window.__CHA_NATIVE_QUEUE__||[]).push(batch);}"
        L"});}";
}

// Keep this aligned with the macOS host in packaging/macos/feasibility.swift.
// One registered scheme serves the packaged files and the runtime's media
// bytes, so both arrive on a single origin and 'self' covers them in the CSP.
constexpr wchar_t kAssetScheme[] = L"cha";
constexpr wchar_t kAssetOrigin[] = L"cha://app";
// Served per document; carries that document's connection identifier.
constexpr wchar_t kBootstrapPath[] = L"/__cha-bootstrap.js";
constexpr char kNativeContentSecurityPolicy[] =
    "default-src 'none'; script-src 'self'; style-src 'self'; "
    "img-src 'self' data:; font-src 'self'; media-src 'self' blob:; connect-src 'self'; "
    "base-uri 'none'; form-action 'none'; frame-ancestors 'none'";

LaunchOptions parse_launch_options() {
    int count = 0;
    wchar_t** arguments = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (arguments == nullptr) {
        throw std::runtime_error("Failed to read the Windows command line");
    }
    struct ArgumentsGuard {
        wchar_t** value;
        ~ArgumentsGuard() { ::LocalFree(value); }
    } guard{arguments};

    if (count == 1) return {};
#if defined(CHA_NATIVE_INSTRUMENTATION)
    if (count == 3 && std::wstring_view(arguments[1]) == L"--smoke-test") {
        return {
            .smoke_test = true,
            .data_root = std::filesystem::path(arguments[2]),
        };
    }
    LaunchOptions options;
    for (int index = 1; index < count; ++index) {
        const std::wstring_view argument(arguments[index]);
        const auto require_value = [&] {
            if (index + 1 >= count) {
                throw std::runtime_error("CHA does not accept command-line arguments");
            }
            return std::wstring_view(arguments[++index]);
        };
        if (argument == L"--assets") {
            options.assets = std::filesystem::path(require_value());
        } else if (argument == L"--cdp-port") {
            options.cdp_port = std::stoi(std::wstring(require_value()));
        } else if (argument == L"--user-data") {
            options.data_root = std::filesystem::path(require_value());
        } else if (argument == L"--dev-origin") {
            const std::wstring value(require_value());
            if (value != L"http://127.0.0.1:5173") {
                throw std::runtime_error("CHA does not accept command-line arguments");
            }
            options.dev_origin = value;
        } else {
            throw std::runtime_error("CHA does not accept command-line arguments");
        }
    }
    return options;
#else
    (void)arguments;
    throw std::runtime_error("CHA does not accept command-line arguments");
#endif
}

class WindowsApplication final
    : public std::enable_shared_from_this<WindowsApplication> {
public:
    ~WindowsApplication() {
        if (save_thread_.joinable()) save_thread_.join();
        if (operation_thread_.joinable()) operation_thread_.join();
        if (shutdown_thread_.joinable()) shutdown_thread_.join();
        shutdown_runtime();
        if (window_ != nullptr && ::IsWindow(window_)) {
            ::DestroyWindow(window_);
        }
    }

    void start(
        HINSTANCE instance,
        int show_command,
        const LaunchOptions& options) {
        instance_ = instance;
        smoke_test_ = options.smoke_test;
        assets_ = options.assets;
        cdp_port_ = options.cdp_port;
        dev_origin_ = options.dev_origin;
        if (!assets_) {
            assets_ = cha::executable_directory() / "web";
        }
        create_window(show_command);
        prepare_application_data(options.data_root);
        start_runtime();
        start_webview();
    }

    int run() {
        MSG message{};
        while (true) {
            const BOOL result = ::GetMessageW(&message, nullptr, 0, 0);
            if (result == 0) break;
            if (result == -1) return 1;
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        return exit_code_;
    }

private:
    static LRESULT CALLBACK window_procedure(
        HWND window,
        UINT message,
        WPARAM wparam,
        LPARAM lparam) {
        WindowsApplication* application = reinterpret_cast<WindowsApplication*>(
            ::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            application = static_cast<WindowsApplication*>(create->lpCreateParams);
            ::SetWindowLongPtrW(
                window,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(application));
            application->window_ = window;
        }
        return application == nullptr
            ? ::DefWindowProcW(window, message, wparam, lparam)
            : application->handle_message(message, wparam, lparam);
    }

    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_SIZE:
            resize_contents();
            return 0;
        case WM_SETFOCUS:
            if (controller_) {
                controller_->MoveFocus(
                    COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            }
            return 0;
        case WM_INITMENUPOPUP:
            if (reinterpret_cast<HMENU>(wparam) == database_menu_) {
                update_database_menu_items();
            }
            return 0;
        case WM_COMMAND:
            handle_command(LOWORD(wparam));
            return 0;
        case WM_CLOSE:
            request_close();
            return 0;
        case kOperationComplete:
            finish_database_operation(
                reinterpret_cast<OperationResult*>(lparam));
            return 0;
        case kFatalError:
            show_fatal_error(
                std::unique_ptr<std::wstring>(
                    reinterpret_cast<std::wstring*>(lparam)));
            return 0;
        case kDeliveryReady:
            deliver_batch(reinterpret_cast<DeliveryPayload*>(lparam));
            return 0;
        case kShutdownDone:
            finish_shutdown();
            return 0;
        case kNativeSaveComplete:
            finish_native_save(
                reinterpret_cast<NativeSaveResult*>(lparam));
            return 0;
        case WM_DESTROY:
            window_ = nullptr;
            ::PostQuitMessage(exit_code_);
            return 0;
        default:
            return ::DefWindowProcW(window_, message, wparam, lparam);
        }
    }

    void create_window(int show_command) {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = window_procedure;
        window_class.hInstance = instance_;
        window_class.hIcon = static_cast<HICON>(::LoadImageW(
            instance_,
            MAKEINTRESOURCEW(kApplicationIcon),
            IMAGE_ICON,
            0,
            0,
            LR_DEFAULTSIZE));
        window_class.hIconSm = static_cast<HICON>(::LoadImageW(
            instance_,
            MAKEINTRESOURCEW(kApplicationIcon),
            IMAGE_ICON,
            16,
            16,
            0));
        window_class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground =
            reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        window_class.lpszClassName = kWindowClass;
        if (::RegisterClassExW(&window_class) == 0) {
            throw std::runtime_error("Failed to register the CHA window");
        }

        HMENU main_menu = ::CreateMenu();
        HMENU file_menu = ::CreatePopupMenu();
        database_menu_ = ::CreatePopupMenu();
        if (main_menu == nullptr || file_menu == nullptr
            || database_menu_ == nullptr) {
            throw std::runtime_error("Failed to create the CHA menus");
        }
        ::AppendMenuW(file_menu, MF_STRING, kFileExit, L"E&xit");
        ::AppendMenuW(main_menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file_menu), L"&File");
        ::AppendMenuW(database_menu_, MF_STRING, kDatabaseImport, L"&Import...");
        ::AppendMenuW(database_menu_, MF_STRING, kDatabaseExport, L"&Export");
        ::AppendMenuW(database_menu_, MF_SEPARATOR, 0, nullptr);
        ::AppendMenuW(database_menu_, MF_STRING, kDatabaseUpload, L"&Upload");
        ::AppendMenuW(database_menu_, MF_STRING, kDatabaseDownload, L"&Download...");
        ::AppendMenuW(
            main_menu,
            MF_POPUP,
            reinterpret_cast<UINT_PTR>(database_menu_),
            L"&Database");

        RECT rectangle{0, 0, 1040, 760};
        ::AdjustWindowRectEx(&rectangle, WS_OVERLAPPEDWINDOW, TRUE, 0);
        window_ = ::CreateWindowExW(
            0,
            kWindowClass,
            kApplicationName,
            WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            rectangle.right - rectangle.left,
            rectangle.bottom - rectangle.top,
            nullptr,
            main_menu,
            instance_,
            this);
        if (window_ == nullptr) {
            throw std::runtime_error("Failed to create the CHA window");
        }
        resize_contents();
        if (!smoke_test_) {
            ::ShowWindow(window_, show_command);
            ::UpdateWindow(window_);
        }
    }

    void prepare_application_data(
        const std::optional<std::filesystem::path>& override_root) {
        data_root_ = override_root.value_or(local_application_data() / L"CHA");
        if (!data_root_.is_absolute()) {
            data_root_ = std::filesystem::absolute(data_root_).lexically_normal();
        }
        if (!std::filesystem::exists(data_root_.parent_path())) {
            throw std::runtime_error("The application data parent does not exist");
        }
        ensure_private_directory(data_root_);
        config_directory_ = data_root_ / L"config";
        webview_directory_ = data_root_ / L"webview";
        ensure_private_directory(config_directory_);
        ensure_private_directory(webview_directory_);
    }

    void start_runtime() {
        const std::filesystem::path resources = cha::executable_directory();
        const std::string config = cha::utf8_path(config_directory_);
        const std::string resource_path = cha::utf8_path(resources);
        char* requirement_error = nullptr;
        char* vault_name = nullptr;
        const int32_t password_required = cha_runtime_requires_password(
            config.c_str(), resource_path.c_str(), &vault_name, &requirement_error);
        if (password_required < 0) {
            throw std::runtime_error(take_bridge_error(requirement_error));
        }
        const std::wstring selected_vault_name = wide_from_utf8(vault_name);
        cha_string_free(vault_name);
        std::string password;
        if (password_required != 0) {
            const auto entered = prompt_for_vault_password(instance_, window_, selected_vault_name, L"");
            if (!entered) throw LaunchCancelled();
            password = *entered;
        }
        while (runtime_ == nullptr) {
            char* bridge_error = nullptr;
            int32_t password_error = 0;
            runtime_ = cha_runtime_create(
                config.c_str(),
                resource_path.c_str(),
                password.c_str(),
                &password_error,
                &bridge_error);
            if (runtime_ != nullptr) break;
            const std::string message = take_bridge_error(bridge_error);
            if (password_error == 0) throw std::runtime_error(message);
            const auto entered = prompt_for_vault_password(
                instance_, window_, selected_vault_name, wide_from_utf8(message));
            if (!entered) throw LaunchCancelled();
            password = *entered;
        }
        if (dev_origin_) {
            runtime_origin_ = *dev_origin_;
            runtime_url_ = runtime_origin_ + L"/";
        } else {
            runtime_origin_ = kAssetOrigin;
            runtime_url_ = std::wstring(kAssetOrigin) + L"/";
        }
        cha_runtime_set_delivery_callback(runtime_, native_delivery, this);
        update_database_menu_items();
    }

    static void native_delivery(
        void* context, const char* connection_id, const char* json) {
        auto* self = static_cast<WindowsApplication*>(context);
        if (self == nullptr || connection_id == nullptr || json == nullptr) {
            return;
        }
        self->post_delivery(connection_id, json);
    }

    void post_delivery(const char* connection_id, const char* json) {
        auto payload = std::make_unique<DeliveryPayload>(
            DeliveryPayload{connection_id, json});
        DeliveryPayload* const posted = payload.release();
        if (!::PostMessageW(
                window_, kDeliveryReady, 0, reinterpret_cast<LPARAM>(posted))) {
            delete posted;
        }
    }

    void start_webview() {
        // Browser arguments come from the options below, never from the
        // environment, so a hostile environment cannot inject them.
        ::SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", nullptr);
#if !defined(CHA_NATIVE_INSTRUMENTATION)
        ::SetEnvironmentVariableW(L"CHA_DEV_ORIGIN", nullptr);
#endif
        const auto options = Make<CoreWebView2EnvironmentOptions>();
        const auto scheme =
            Make<CoreWebView2CustomSchemeRegistration>(kAssetScheme);
        if (!options || !scheme) {
            throw std::runtime_error("Failed to initialize WebView2");
        }
        // Secure so the application is a secure context and may ask for the
        // microphone; an authority so cha://app/... carries a real host and
        // the pages share one origin.
        scheme->put_TreatAsSecure(TRUE);
        scheme->put_HasAuthorityComponent(TRUE);
        LPCWSTR allowed[] = {kAssetOrigin};
        scheme->SetAllowedOrigins(ARRAYSIZE(allowed), allowed);
        ICoreWebView2CustomSchemeRegistration* registrations[] = {scheme.Get()};
        HRESULT result = options->SetCustomSchemeRegistrations(
            ARRAYSIZE(registrations), registrations);
#if defined(CHA_NATIVE_INSTRUMENTATION)
        if (SUCCEEDED(result) && cdp_port_) {
            const std::wstring arguments =
                L"--remote-debugging-port=" + std::to_wstring(*cdp_port_);
            result = options->put_AdditionalBrowserArguments(arguments.c_str());
        }
#endif
        if (FAILED(result)) {
            throw std::runtime_error(cha::utf8_from_wide(
                hresult_message(result, L"Failed to initialize WebView2")));
        }
        const std::shared_ptr<WindowsApplication> self = shared_from_this();
        result = ::CreateCoreWebView2EnvironmentWithOptions(
            nullptr,
            webview_directory_.c_str(),
            options.Get(),
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [self](HRESULT status, ICoreWebView2Environment* environment) {
                    return self->environment_created(status, environment);
                }).Get());
        if (FAILED(result)) {
            throw std::runtime_error(cha::utf8_from_wide(
                hresult_message(result, L"Failed to initialize WebView2")));
        }
    }

    HRESULT environment_created(
        HRESULT status,
        ICoreWebView2Environment* environment) {
        if (closing_) return S_OK;
        if (FAILED(status) || environment == nullptr) {
            post_fatal_error(hresult_message(
                status, L"The Microsoft Edge WebView2 Runtime could not start"));
            return S_OK;
        }
        environment_ = environment;
        const std::shared_ptr<WindowsApplication> self = shared_from_this();
        const HRESULT result = environment_->CreateCoreWebView2Controller(
            window_,
            Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [self](HRESULT controller_status, ICoreWebView2Controller* controller) {
                    return self->controller_created(controller_status, controller);
                }).Get());
        if (FAILED(result)) {
            post_fatal_error(hresult_message(
                result, L"The CHA browser window could not be created"));
        }
        return S_OK;
    }

    HRESULT controller_created(
        HRESULT status,
        ICoreWebView2Controller* controller) {
        if (closing_) return S_OK;
        if (FAILED(status) || controller == nullptr) {
            post_fatal_error(hresult_message(
                status, L"The CHA browser window could not be created"));
            return S_OK;
        }
        controller_ = controller;
        HRESULT result = controller_->get_CoreWebView2(&webview_);
        if (FAILED(result) || !webview_) {
            post_fatal_error(hresult_message(
                result, L"The CHA browser is not available"));
            return S_OK;
        }
        resize_contents();
        controller_->put_IsVisible(TRUE);

        install_webview_handlers();
        result = install_native_origin();
        if (FAILED(result)) {
            post_fatal_error(hresult_message(
                result, L"CHA could not map its packaged assets"));
            return S_OK;
        }
        navigate_home();
        return S_OK;
    }

    void install_webview_handlers() {
        EventRegistrationToken ignored{};
        webview_->add_NavigationStarting(
            Callback<ICoreWebView2NavigationStartingEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) {
                    wchar_t* raw_uri = nullptr;
                    if (FAILED(args->get_Uri(&raw_uri))) return S_OK;
                    const std::wstring uri = take_com_string(raw_uri);
                    if (!is_application_uri(uri)) {
                        args->put_Cancel(TRUE);
                        open_https(uri);
                        return S_OK;
                    }
                    wchar_t* raw_current = nullptr;
                    std::wstring current;
                    if (webview_ && SUCCEEDED(webview_->get_Source(&raw_current))) {
                        current = take_com_string(raw_current);
                    }
                    const bool hash_only = !current.empty()
                        && without_fragment(uri) == without_fragment(current)
                        && uri != current;
                    // A hash change keeps the document, so it keeps its
                    // connection. Anything else replaces the document and so
                    // needs a fresh one, which the shell will hand it.
                    if (!hash_only) {
                        replace_document_connection();
                    }
                    return S_OK;
                }).Get(),
            &ignored);
        webview_->add_ProcessFailed(
            Callback<ICoreWebView2ProcessFailedEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs* args) {
                    COREWEBVIEW2_PROCESS_FAILED_KIND kind{};
                    if (FAILED(args->get_ProcessFailedKind(&kind))) return S_OK;
                    if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED
                        || kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED
                        || kind
                            == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE) {
                        handle_renderer_failure();
                    }
                    return S_OK;
                }).Get(),
            &ignored);
        webview_->add_NewWindowRequested(
            Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) {
                    wchar_t* raw_uri = nullptr;
                    if (SUCCEEDED(args->get_Uri(&raw_uri))) {
                        const std::wstring uri = take_com_string(raw_uri);
                        if (is_application_uri(uri)) {
                            webview_->Navigate(uri.c_str());
                        } else {
                            open_https(uri);
                        }
                    }
                    args->put_Handled(TRUE);
                    return S_OK;
                }).Get(),
            &ignored);
        webview_->add_PermissionRequested(
            Callback<ICoreWebView2PermissionRequestedEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) {
                    COREWEBVIEW2_PERMISSION_KIND kind{};
                    wchar_t* raw_uri = nullptr;
                    const bool readable = SUCCEEDED(args->get_PermissionKind(&kind))
                        && SUCCEEDED(args->get_Uri(&raw_uri));
                    const std::wstring uri = take_com_string(raw_uri);
                    const bool allowed = readable
                        && kind == COREWEBVIEW2_PERMISSION_KIND_MICROPHONE
                        && is_application_uri(uri)
                        && current_document_is_trusted();
                    args->put_State(allowed
                        ? COREWEBVIEW2_PERMISSION_STATE_ALLOW
                        : COREWEBVIEW2_PERMISSION_STATE_DENY);
                    return S_OK;
                }).Get(),
            &ignored);
        webview_->add_DocumentTitleChanged(
            Callback<ICoreWebView2DocumentTitleChangedEventHandler>(
                [this](ICoreWebView2* sender, IUnknown*) {
                    if (database_operation_in_progress_) return S_OK;
                    wchar_t* raw_title = nullptr;
                    if (SUCCEEDED(sender->get_DocumentTitle(&raw_title))) {
                        std::wstring title = take_com_string(raw_title);
                        if (title.empty()) title = kApplicationName;
                        ::SetWindowTextW(window_, title.c_str());
                    }
                    return S_OK;
                }).Get(),
            &ignored);
        webview_->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) {
                    if (!initial_navigation_pending_) return S_OK;
                    initial_navigation_pending_ = false;
                    BOOL succeeded = FALSE;
                    if (FAILED(args->get_IsSuccess(&succeeded)) || !succeeded) {
                        post_fatal_error(L"CHA could not load its browser application.");
                    } else if (smoke_test_) {
                        ::PostMessageW(window_, WM_CLOSE, 0, 0);
                    }
                    return S_OK;
                }).Get(),
            &ignored);

        ComPtr<ICoreWebView2_4> webview4;
        if (SUCCEEDED(webview_.As(&webview4))) {
            webview4->add_DownloadStarting(
                Callback<ICoreWebView2DownloadStartingEventHandler>(
                    [this](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) {
                        choose_download_destination(args);
                        return S_OK;
                    }).Get(),
                &ignored);
            webview4->add_FrameCreated(
                Callback<ICoreWebView2FrameCreatedEventHandler>(
                    [](ICoreWebView2*, ICoreWebView2FrameCreatedEventArgs* args) {
                        ComPtr<ICoreWebView2Frame> frame;
                        if (FAILED(args->get_Frame(&frame)) || !frame) return S_OK;
                        ComPtr<ICoreWebView2Frame3> frame3;
                        if (FAILED(frame.As(&frame3))) return S_OK;
                        EventRegistrationToken frame_token{};
                        frame3->add_PermissionRequested(
                            Callback<ICoreWebView2FramePermissionRequestedEventHandler>(
                                [](ICoreWebView2Frame*,
                                   ICoreWebView2PermissionRequestedEventArgs2* frame_args) {
                                    frame_args->put_State(
                                        COREWEBVIEW2_PERMISSION_STATE_DENY);
                                    frame_args->put_Handled(TRUE);
                                    return S_OK;
                                }).Get(),
                            &frame_token);
                        return S_OK;
                    }).Get(),
                &ignored);
        }
    }

    HRESULT respond_with_bytes(
        ICoreWebView2WebResourceRequestedEventArgs* args,
        int status,
        std::wstring_view reason,
        std::wstring_view headers,
        const std::string& body) {
        ComPtr<IStream> stream;
        stream.Attach(::SHCreateMemStream(
            reinterpret_cast<const BYTE*>(body.data()),
            static_cast<UINT>(body.size())));
        if (!stream) return E_OUTOFMEMORY;
        ComPtr<ICoreWebView2WebResourceResponse> response;
        const HRESULT result = environment_->CreateWebResourceResponse(
            stream.Get(),
            status,
            std::wstring(reason).c_str(),
            std::wstring(headers).c_str(),
            &response);
        if (FAILED(result)) return result;
        return args->put_Response(response.Get());
    }

    // Puts the bootstrap ahead of the application bundle. The bundle is a
    // deferred module script, so this classic script always runs first.
    static std::string with_bootstrap_script(const std::string& html) {
        constexpr std::string_view head = "</head>";
        const std::string tag = std::string("<script src=\"")
            + cha::utf8_from_wide(kBootstrapPath) + "\"></script>";
        const auto at = html.find(head);
        if (at == std::string::npos) return tag + html;
        return html.substr(0, at) + tag + html.substr(at);
    }

    // Keep this aligned with mimeType(for:) in packaging/macos/feasibility.swift.
    static std::wstring asset_content_type(const std::filesystem::path& file) {
        const std::wstring extension = file.extension().wstring();
        if (extension == L".html") return L"text/html; charset=utf-8";
        if (extension == L".css") return L"text/css; charset=utf-8";
        if (extension == L".js") return L"text/javascript; charset=utf-8";
        if (extension == L".svg") return L"image/svg+xml";
        if (extension == L".png") return L"image/png";
        if (extension == L".woff2") return L"font/woff2";
        if (extension == L".json" || extension == L".map") {
            return L"application/json";
        }
        if (extension == L".wav") return L"audio/wav";
        return L"application/octet-stream";
    }

    HRESULT handle_media_resource(
        ICoreWebView2WebResourceRequestedEventArgs* args,
        std::wstring_view resource_id) {
        const std::string id = cha::utf8_from_wide(resource_id);
        const auto not_found = [&] {
            return respond_with_bytes(
                args, 404, L"Not Found",
                L"Content-Type: text/plain; charset=utf-8\nCache-Control: no-store",
                "not found");
        };
        if (id.empty() || id.find('/') != std::string::npos
            || id.find('\\') != std::string::npos
            || id.find("..") != std::string::npos
            || runtime_ == nullptr || connection_id_.empty()) {
            return not_found();
        }
        char* mime = nullptr;
        void* bytes = nullptr;
        uint64_t size = 0;
        char* error = nullptr;
        const int32_t ok = cha_runtime_read_resource(
            runtime_, connection_id_.c_str(), id.c_str(),
            &mime, &bytes, &size, &error);
        cha_string_free(error);
        if (ok == 0 || bytes == nullptr) {
            cha_string_free(mime);
            cha_bytes_free(bytes);
            return not_found();
        }
        const std::string type = mime ? mime : "application/octet-stream";
        const std::string body(
            static_cast<const char*>(bytes),
            static_cast<std::size_t>(size));
        cha_string_free(mime);
        cha_bytes_free(bytes);
        const std::wstring headers =
            L"Content-Type: " + wide_from_utf8(type)
            + L"\nCache-Control: no-store";
        return respond_with_bytes(args, 200, L"OK", headers, body);
    }

    HRESULT handle_asset_resource(
        ICoreWebView2WebResourceRequestedEventArgs* args) {
        ComPtr<ICoreWebView2WebResourceRequest> request;
        if (FAILED(args->get_Request(&request)) || !request) return S_OK;
        wchar_t* raw_uri = nullptr;
        if (FAILED(request->get_Uri(&raw_uri))) return S_OK;
        const std::wstring uri = take_com_string(raw_uri);
        const std::wstring origin(kAssetOrigin);
        if (uri.rfind(origin + L"/media/", 0) == 0) {
            return handle_media_resource(args, uri.substr(origin.size() + 7));
        }
        if (dev_origin_ && uri.rfind(*dev_origin_ + L"/media/", 0) == 0) {
            return handle_media_resource(args, uri.substr(dev_origin_->size() + 7));
        }
        const auto not_found = [&] {
            return respond_with_bytes(
                args, 404, L"Not Found",
                L"Content-Type: text/plain; charset=utf-8\nCache-Control: no-store",
                "not found");
        };
        if (uri == origin + kBootstrapPath
            || (dev_origin_ && uri == *dev_origin_ + kBootstrapPath)) {
            if (connection_id_.empty()) return not_found();
            return respond_with_bytes(
                args, 200, L"OK",
                L"Content-Type: text/javascript; charset=utf-8\nCache-Control: no-store",
                cha::utf8_from_wide(native_bootstrap_script(connection_id_)));
        }
        if (uri.rfind(origin + L"/", 0) != 0) return not_found();
        std::wstring relative = uri.substr(origin.size() + 1);
        if (const auto query = relative.find_first_of(L"?#");
            query != std::wstring::npos) {
            relative.erase(query);
        }
        const bool shell = relative.empty() || relative == L"index.html";
        if (shell) relative = L"index.html";
        const auto file = resolve_asset(relative);
        if (!file) return not_found();
        return respond_with_asset(args, *file, shell);
    }

    // Resolves a request path inside the asset directory, or nothing if it
    // would escape. Rooted and drive-qualified paths matter here because
    // std::filesystem::path concatenation drops the left operand when the
    // right one is rooted, so "/C:/secrets" would otherwise be served
    // verbatim. The containment check also covers symlinks and junctions.
    std::optional<std::filesystem::path> resolve_asset(
        const std::wstring& relative) const {
        if (relative.empty()) return std::nullopt;
        // No colon can appear in a packaged asset name, and rejecting it stops
        // both drive letters and NTFS alternate data streams.
        if (relative.find(L':') != std::wstring::npos) return std::nullopt;
        if (relative.find(L'\\') != std::wstring::npos) return std::nullopt;
        const std::filesystem::path candidate(relative);
        if (candidate.has_root_name() || candidate.has_root_directory()) {
            return std::nullopt;
        }
        for (const auto& part : candidate) {
            if (part == L".." || part == L".") return std::nullopt;
        }
        std::error_code error;
        const auto root = std::filesystem::weakly_canonical(*assets_, error);
        if (error) return std::nullopt;
        const auto resolved =
            std::filesystem::weakly_canonical(root / candidate, error);
        if (error) return std::nullopt;
        const auto inside = resolved.lexically_relative(root);
        if (inside.empty() || *inside.begin() == L"..") return std::nullopt;
        return resolved;
    }

    HRESULT respond_with_asset(
        ICoreWebView2WebResourceRequestedEventArgs* args,
        const std::filesystem::path& file,
        bool shell) {
        const auto not_found = [&] {
            return respond_with_bytes(
                args, 404, L"Not Found",
                L"Content-Type: text/plain; charset=utf-8\nCache-Control: no-store",
                "not found");
        };
        std::ifstream input(file, std::ios::binary);
        if (!input) return not_found();
        const std::string body{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        std::wstring headers = L"Content-Type: " + asset_content_type(file)
            + (shell
                ? L"\nCache-Control: no-cache\nContent-Security-Policy: "
                    + wide_from_utf8(kNativeContentSecurityPolicy)
                : L"\nCache-Control: public, max-age=31536000, immutable");
        return respond_with_bytes(
            args, 200, L"OK", headers,
            shell ? with_bootstrap_script(body) : body);
    }

    HRESULT install_native_origin() {
        if (!assets_) return E_INVALIDARG;
        HRESULT result = webview_->AddWebResourceRequestedFilter(
            (std::wstring(kAssetOrigin) + L"/*").c_str(),
            COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        if (FAILED(result)) return result;
        if (dev_origin_) {
            // Vite serves frontend assets; bootstrap and media belong to the host.
            result = webview_->AddWebResourceRequestedFilter(
                (*dev_origin_ + kBootstrapPath).c_str(),
                COREWEBVIEW2_WEB_RESOURCE_CONTEXT_SCRIPT);
            if (FAILED(result)) return result;
            result = webview_->AddWebResourceRequestedFilter(
                (*dev_origin_ + L"/media/*").c_str(),
                COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
            if (FAILED(result)) return result;
        }
        EventRegistrationToken ignored{};
        const std::shared_ptr<WindowsApplication> self = shared_from_this();
        result = webview_->add_WebResourceRequested(
            Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                [self](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) {
                    return self->handle_asset_resource(args);
                }).Get(),
            &ignored);
        if (FAILED(result)) return result;
        result = webview_->add_WebMessageReceived(
            Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [self](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) {
                    return self->handle_native_message(args);
                }).Get(),
            &ignored);
        return result;
    }

    // AddScriptToExecuteOnDocumentCreated is asynchronous, so a script
    // registered as a navigation starts can miss the document it was meant
    // for. The shell is served from here instead, so the bootstrap is served
    // as an ordinary classic script whose contents are generated per document.
    // It is fetched before the deferred module bundle runs, so the connection
    // identifier is in place before the application starts.
    HRESULT replace_document_connection() {
        if (runtime_ == nullptr) return E_UNEXPECTED;
        if (!connection_id_.empty()) {
            cha_runtime_close_connection(runtime_, connection_id_.c_str());
            connection_id_.clear();
        }
        char* error = nullptr;
        char* opened = cha_runtime_open_connection(runtime_, &error);
        if (opened == nullptr) {
            take_bridge_error(error);
            return E_FAIL;
        }
        connection_id_ = opened;
        cha_string_free(opened);
        return S_OK;
    }

    HRESULT handle_native_message(
        ICoreWebView2WebMessageReceivedEventArgs* args) {
        wchar_t* raw_source = nullptr;
        if (FAILED(args->get_Source(&raw_source))) return S_OK;
        const std::wstring source = take_com_string(raw_source);
        if (!is_trusted_main_document(source) || connection_id_.empty()
            || runtime_ == nullptr) {
            return S_OK;
        }
        wchar_t* raw_message = nullptr;
        std::string payload;
        if (SUCCEEDED(args->TryGetWebMessageAsString(&raw_message))) {
            payload = cha::utf8_from_wide(take_com_string(raw_message));
        } else {
            wchar_t* raw_json = nullptr;
            if (FAILED(args->get_WebMessageAsJson(&raw_json))) return S_OK;
            payload = cha::utf8_from_wide(take_com_string(raw_json));
        }
        if (handle_native_save(payload)) return S_OK;
        cha_runtime_handle_message(
            runtime_, connection_id_.c_str(), payload.c_str());
        return S_OK;
    }

    bool handle_native_save(const std::string& payload) {
        nlohmann::json json;
        try {
            json = nlohmann::json::parse(payload);
        } catch (const nlohmann::json::exception&) {
            return false;
        }
        if (!json.is_object()) {
            return false;
        }
        const auto action = json.find("native_action");
        if (action == json.end()) return false;
        if (!action->is_string() || action->get<std::string>() != "save_session") {
            return true;
        }
        const auto string_field = [&](const char* name)
            -> std::optional<std::string> {
            const auto value = json.find(name);
            if (value == json.end() || !value->is_string()) return std::nullopt;
            return value->get<std::string>();
        };
        const auto id_value = string_field("id");
        const auto connection_value = string_field("connection_id");
        const auto suggested_value = string_field("suggested_name");
        const auto forum_value = string_field("forum_id");
        const auto session_value = string_field("session_id");
        if (!id_value || !connection_value || !suggested_value
            || !forum_value || !session_value) {
            return true;
        }
        const std::string& id = *id_value;
        const std::string& originating_connection = *connection_value;
        const std::string& suggested = *suggested_value;
        const std::string& forum_id = *forum_value;
        const std::string& session_id = *session_value;
        if (originating_connection != connection_id_
            || !json.contains("context_epoch")
            || !json["context_epoch"].is_number_unsigned()) {
            return true;
        }
        const uint64_t request_epoch = json["context_epoch"].get<uint64_t>();
        uint64_t epoch = 0;
        if (forum_id.empty() || session_id.empty() || runtime_ == nullptr
            || request_epoch == 0
            || request_epoch > 9007199254740991ULL
            || cha_runtime_context_epoch(runtime_, &epoch) == 0
            || epoch != request_epoch) {
            return true;
        }
        if (save_dialog_ || save_operation_in_progress_) {
            complete_native_save(
                originating_connection,
                id,
                request_epoch,
                false,
                "Another save is in progress.");
            return true;
        }
        ComPtr<IFileSaveDialog> dialog;
        HRESULT result = ::CoCreateInstance(
            CLSID_FileSaveDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog));
        if (SUCCEEDED(result)) {
            save_dialog_ = dialog;
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
            const std::wstring name = wide_from_utf8(suggested);
            dialog->SetFileName(name.c_str());
            result = dialog->Show(window_);
            save_dialog_.Reset();
        }
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            complete_native_save(
                originating_connection, id, request_epoch, true, "cancelled");
            return true;
        }
        uint64_t current_epoch = 0;
        if (closing_ || connection_id_ != originating_connection
            || cha_runtime_context_epoch(runtime_, &current_epoch) == 0
            || current_epoch != request_epoch) {
            return true;
        }
        ComPtr<IShellItem> item;
        wchar_t* raw_destination = nullptr;
        if (SUCCEEDED(result)) result = dialog->GetResult(&item);
        if (SUCCEEDED(result)) {
            result = item->GetDisplayName(SIGDN_FILESYSPATH, &raw_destination);
        }
        const std::wstring destination = take_com_string(raw_destination);
        if (FAILED(result) || destination.empty()) {
            complete_native_save(
                originating_connection, id, request_epoch, false, "save failed");
            return true;
        }
        const std::string path = cha::utf8_from_wide(destination);
        ChaRuntime* const runtime = runtime_;
        HWND const window = window_;
        save_operation_in_progress_ = true;
        try {
            save_thread_ = std::thread([
                runtime,
                window,
                epoch,
                forum_id,
                session_id,
                path,
                originating_connection,
                id,
                request_epoch] {
                auto completed = std::make_unique<NativeSaveResult>();
                completed->connection_id = originating_connection;
                completed->id = id;
                completed->context_epoch = request_epoch;
                char* error = nullptr;
                const int32_t status = cha_runtime_export_session(
                    runtime,
                    epoch,
                    forum_id.c_str(),
                    session_id.c_str(),
                    path.c_str(),
                    &error);
                completed->ok = status == 1;
                completed->message = error ? error : "";
                cha_string_free(error);
                NativeSaveResult* const posted = completed.release();
                if (!::PostMessageW(
                        window,
                        kNativeSaveComplete,
                        0,
                        reinterpret_cast<LPARAM>(posted))) {
                    delete posted;
                }
            });
        } catch (...) {
            save_operation_in_progress_ = false;
            complete_native_save(
                originating_connection,
                id,
                request_epoch,
                false,
                "The save could not start.");
        }
        return true;
    }

    void finish_native_save(NativeSaveResult* raw_result) {
        const std::unique_ptr<NativeSaveResult> result(raw_result);
        if (save_thread_.joinable()) save_thread_.join();
        save_operation_in_progress_ = false;
        if (!result || closing_) return;
        complete_native_save(
            result->connection_id,
            result->id,
            result->context_epoch,
            result->ok,
            result->message);
    }

    void complete_native_save(
        const std::string& connection,
        const std::string& id,
        uint64_t context_epoch,
        bool ok,
        const std::string& message) {
        if (!webview_ || connection != connection_id_) return;
        uint64_t current_epoch = 0;
        if (runtime_ == nullptr
            || cha_runtime_context_epoch(runtime_, &current_epoch) == 0
            || current_epoch != context_epoch) {
            return;
        }
        const std::wstring encoded = wide_from_utf8(
            "if(typeof window.__CHA_NATIVE_SAVE_DONE__==='function'){"
            "window.__CHA_NATIVE_SAVE_DONE__("
            + nlohmann::json(id).dump() + ","
            + std::string(ok ? "true" : "false") + ","
            + nlohmann::json(message).dump()
            + ");}");
        webview_->ExecuteScript(encoded.c_str(), nullptr);
    }

    void deliver_batch(DeliveryPayload* raw) {
        const std::unique_ptr<DeliveryPayload> payload(raw);
        if (!payload || closing_ || !webview_) return;
        if (payload->connection_id != connection_id_) return;
        const std::wstring encoded = wide_from_utf8(payload->json);
        webview_->PostWebMessageAsJson(encoded.c_str());
    }

    void handle_renderer_failure() {
        if (closing_) return;
        renderer_failures_ += 1;
        if (renderer_failures_ >= 3) {
            post_fatal_error(
                L"CHA's browser process stopped repeatedly. Quit and open CHA again.");
            return;
        }
        navigate_home();
    }

    // The connection is replaced by the NavigationStarting handler, which is
    // the single owner of document identity; navigating is all that is needed.
    void navigate_home() {
        if (closing_) return;
        initial_navigation_pending_ = true;
        const HRESULT result = webview_->Navigate(runtime_url_.c_str());
        if (FAILED(result)) {
            initial_navigation_pending_ = false;
            post_fatal_error(hresult_message(
                result, L"CHA could not load its browser application"));
        }
    }

    std::wstring current_top_level_source() const {
        if (!webview_) return {};
        wchar_t* raw_current = nullptr;
        if (FAILED(webview_->get_Source(&raw_current))) return {};
        return take_com_string(raw_current);
    }

    bool current_document_is_trusted() const {
        return is_application_uri(current_top_level_source());
    }

    bool is_trusted_main_document(std::wstring_view sender) const {
        const std::wstring current = current_top_level_source();
        return is_application_uri(sender)
            && is_application_uri(current)
            && without_fragment(sender) == without_fragment(current);
    }

    bool is_application_uri(std::wstring_view uri) const {
        const auto document = without_fragment(uri);
        const auto matches_shell = [&](std::wstring_view origin) {
            return document == origin
                || document == std::wstring(origin) + L"/"
                || document == std::wstring(origin) + L"/index.html";
        };
        return matches_shell(runtime_origin_)
            || (dev_origin_ && matches_shell(kAssetOrigin));
    }

    static void open_https(std::wstring_view uri) {
        if (!starts_with_case_insensitive(uri, L"https://")) return;
        const std::wstring address(uri);
        ::ShellExecuteW(nullptr, L"open", address.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    void choose_download_destination(
        ICoreWebView2DownloadStartingEventArgs* args) {
        args->put_Handled(TRUE);
        wchar_t* raw_path = nullptr;
        std::wstring suggested;
        if (SUCCEEDED(args->get_ResultFilePath(&raw_path))) {
            suggested = std::filesystem::path(
                take_com_string(raw_path)).filename().wstring();
        }
        if (suggested.empty()) suggested = L"CHA download";

        ComPtr<IFileSaveDialog> dialog;
        HRESULT result = ::CoCreateInstance(
            CLSID_FileSaveDialog,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog));
        if (SUCCEEDED(result)) {
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
            dialog->SetFileName(suggested.c_str());
            result = dialog->Show(window_);
        }
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            args->put_Cancel(TRUE);
            return;
        }
        if (FAILED(result)) {
            args->put_Cancel(TRUE);
            show_notice(
                L"CHA could not save the file",
                L"Choose another location and try again.");
            return;
        }
        ComPtr<IShellItem> item;
        result = dialog->GetResult(&item);
        wchar_t* raw_destination = nullptr;
        if (SUCCEEDED(result)) {
            result = item->GetDisplayName(SIGDN_FILESYSPATH, &raw_destination);
        }
        const std::wstring destination = take_com_string(raw_destination);
        if (FAILED(result) || destination.empty()
            || FAILED(args->put_ResultFilePath(destination.c_str()))) {
            args->put_Cancel(TRUE);
            show_notice(
                L"CHA could not save the file",
                L"Choose another location and try again.");
        }
    }

    void handle_command(UINT command) {
        switch (command) {
        case kFileExit:
            request_close();
            break;
        case kDatabaseImport:
            if (::MessageBoxW(
                    window_,
                    L"CHA will replace its workspace configuration with the contents of the directory named by 'modify' in the active vault's configuration.",
                    L"Import workspace configuration?",
                    MB_ICONWARNING | MB_OKCANCEL) == IDOK) {
                perform_database_operation(
                    DatabaseOperation::import_configuration);
            }
            break;
        case kDatabaseExport:
            perform_database_operation(DatabaseOperation::export_configuration);
            break;
        case kDatabaseUpload:
            perform_database_operation(DatabaseOperation::upload);
            break;
        case kDatabaseDownload:
            if (::MessageBoxW(
                    window_,
                    L"CHA will save the current database beside itself with a .bac suffix, then use the database downloaded from R2.",
                    L"Replace the local database?",
                    MB_ICONWARNING | MB_OKCANCEL) == IDOK) {
                perform_database_operation(DatabaseOperation::download);
            }
            break;
        default:
            break;
        }
    }

    void perform_database_operation(DatabaseOperation operation) {
        if (database_operation_in_progress_ || runtime_ == nullptr) return;
        database_operation_in_progress_ = true;
        update_database_menu_items();
        const std::wstring progress = std::wstring(kApplicationName) + L" - "
            + operation_progress_title(operation) + L"...";
        ::SetWindowTextW(window_, progress.c_str());

        operation_thread_ = std::thread([this, operation] {
            auto result = std::make_unique<OperationResult>();
            result->operation = operation;
            char* bridge_error = nullptr;
            switch (operation) {
            case DatabaseOperation::import_configuration:
                result->status = cha_runtime_import_configuration(
                    runtime_, &result->count, &bridge_error);
                break;
            case DatabaseOperation::export_configuration:
                result->status = cha_runtime_export_configuration(
                    runtime_, &result->count, &bridge_error);
                break;
            case DatabaseOperation::upload:
                result->status = cha_runtime_upload(
                    runtime_, &result->count, &bridge_error);
                break;
            case DatabaseOperation::download:
                result->status = cha_runtime_download(
                    runtime_, &result->count, &bridge_error);
                break;
            }
            if (bridge_error != nullptr) {
                result->error = take_bridge_error(bridge_error);
            }
            OperationResult* const posted = result.release();
            if (!::PostMessageW(
                    window_,
                    kOperationComplete,
                    0,
                    reinterpret_cast<LPARAM>(posted))) {
                delete posted;
            }
        });
    }

    void finish_database_operation(OperationResult* raw_result) {
        const std::unique_ptr<OperationResult> result(raw_result);
        if (operation_thread_.joinable()) operation_thread_.join();
        database_operation_in_progress_ = false;
        update_database_menu_items();
        if (close_pending_) {
            close_now();
            return;
        }
        if (!result) return;
        if (result->status < 0) {
            post_fatal_error(result->error.empty()
                ? L"CHA can no longer reach its database."
                : wide_from_utf8(result->error));
            return;
        }
        if (result->status == 0) {
            show_notice(
                operation_title(result->operation) + L" failed",
                result->error.empty()
                    ? L"CHA encountered an unknown error."
                    : wide_from_utf8(result->error));
            update_document_title();
            return;
        }
        if (operation_reloads_application(result->operation)) {
            webview_->Navigate(runtime_url_.c_str());
        }
        const bool files = result->operation
                == DatabaseOperation::import_configuration
            || result->operation == DatabaseOperation::export_configuration;
        const std::wstring unit = files ? L" files." : L" bytes.";
        show_notice(
            operation_title(result->operation) + L" complete",
            (files
                    ? (result->operation == DatabaseOperation::import_configuration
                           ? L"Imported "
                           : L"Exported ")
                    : L"Transferred ")
                + std::to_wstring(result->count) + unit);
        update_document_title();
    }

    void update_document_title() {
        if (!webview_) {
            ::SetWindowTextW(window_, kApplicationName);
            return;
        }
        wchar_t* raw_title = nullptr;
        if (FAILED(webview_->get_DocumentTitle(&raw_title))) return;
        std::wstring title = take_com_string(raw_title);
        if (title.empty()) title = kApplicationName;
        ::SetWindowTextW(window_, title.c_str());
    }

    void update_database_menu_items() {
        if (database_menu_ == nullptr) return;
        const bool idle = !database_operation_in_progress_;
        const bool can_modify = runtime_ != nullptr
            && cha_runtime_can_modify(runtime_) != 0;
        const bool can_transfer = runtime_ != nullptr
            && cha_runtime_can_transfer_r2(runtime_) != 0;
        enable_menu_item(kDatabaseImport, idle && can_modify);
        enable_menu_item(kDatabaseExport, idle && can_modify);
        enable_menu_item(kDatabaseUpload, idle && can_transfer);
        enable_menu_item(kDatabaseDownload, idle && can_transfer);
        if (window_ != nullptr) ::DrawMenuBar(window_);
    }

    void enable_menu_item(UINT command, bool enabled) const {
        ::EnableMenuItem(
            database_menu_,
            command,
            MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
    }

    void resize_contents() {
        if (window_ == nullptr) return;
        RECT bounds{};
        ::GetClientRect(window_, &bounds);
        if (controller_) controller_->put_Bounds(bounds);
    }

    void request_close() {
        if (closing_) return;
        if (database_operation_in_progress_) {
            close_pending_ = true;
            ::SetWindowTextW(window_, L"CHA - Finishing database operation...");
            return;
        }
        begin_shutdown();
    }

    void begin_shutdown() {
        if (closing_) return;
        closing_ = true;
        if (save_dialog_) {
            save_dialog_->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
            save_dialog_.Reset();
        }
        if (!connection_id_.empty() && runtime_ != nullptr) {
            cha_runtime_close_connection(runtime_, connection_id_.c_str());
            connection_id_.clear();
        }
        if (runtime_ != nullptr) {
            cha_runtime_set_delivery_callback(runtime_, nullptr, nullptr);
            cha_runtime_request_shutdown(runtime_);
            ChaRuntime* const runtime = runtime_;
            HWND window = window_;
            std::thread save_thread = std::move(save_thread_);
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::seconds{10};
            if (shutdown_thread_.joinable()) shutdown_thread_.join();
            shutdown_thread_ = std::thread([
                runtime,
                window,
                deadline,
                save_thread = std::move(save_thread)]() mutable {
                if (save_thread.joinable()) {
                    const auto now = std::chrono::steady_clock::now();
                    if (now >= deadline) ::ExitProcess(ERROR_TIMEOUT);
                    const auto wait = std::chrono::duration_cast<
                        std::chrono::milliseconds>(deadline - now);
                    const DWORD wait_ms = static_cast<DWORD>(std::max<int64_t>(
                        1,
                        std::min<int64_t>(
                            wait.count(),
                            std::numeric_limits<DWORD>::max() - 1)));
                    if (::WaitForSingleObject(
                            save_thread.native_handle(), wait_ms)
                        != WAIT_OBJECT_0) {
                        ::ExitProcess(ERROR_TIMEOUT);
                    }
                    save_thread.join();
                }
                const auto now = std::chrono::steady_clock::now();
                if (now >= deadline) ::ExitProcess(ERROR_TIMEOUT);
                const auto remaining = std::chrono::duration_cast<
                    std::chrono::milliseconds>(deadline - now);
                const int grace_ms = static_cast<int>(std::max<int64_t>(
                    1,
                    std::min<int64_t>(
                        remaining.count(),
                        std::numeric_limits<int>::max())));
                if (cha_runtime_join_shutdown(runtime, grace_ms) == 0) {
                    ::ExitProcess(ERROR_TIMEOUT);
                }
                if (window != nullptr) {
                    ::PostMessageW(window, kShutdownDone, 0, 0);
                }
            });
            return;
        }
        finish_shutdown();
    }

    void finish_shutdown() {
        if (shutdown_thread_.joinable()) shutdown_thread_.join();
        shutdown_runtime();
        if (window_ != nullptr && ::IsWindow(window_)) {
            ::DestroyWindow(window_);
        }
    }

    void close_now() {
        begin_shutdown();
    }

    void shutdown_runtime() {
        webview_.Reset();
        if (controller_) controller_->Close();
        controller_.Reset();
        environment_.Reset();
        if (runtime_ != nullptr) {
            cha_runtime_destroy(runtime_);
            runtime_ = nullptr;
        }
    }

    void post_fatal_error(std::wstring message) {
        if (closing_ || fatal_error_posted_) return;
        fatal_error_posted_ = true;
        auto value = std::make_unique<std::wstring>(std::move(message));
        std::wstring* const posted = value.release();
        if (!::PostMessageW(
                window_,
                kFatalError,
                0,
                reinterpret_cast<LPARAM>(posted))) {
            delete posted;
        }
    }

    void show_fatal_error(std::unique_ptr<std::wstring> message) {
        fatal_error_posted_ = false;
        if (closing_) return;
        exit_code_ = 1;
        if (!smoke_test_) {
            ::MessageBoxW(
                window_,
                message ? message->c_str() : L"CHA encountered an unknown error.",
                L"CHA cannot continue",
                MB_OK | MB_ICONERROR);
        }
        // Waits for a running database operation, which uses the runtime.
        request_close();
    }

    void show_notice(
        const std::wstring& title,
        const std::wstring& detail) const {
        if (smoke_test_) return;
        ::MessageBoxW(window_, detail.c_str(), title.c_str(), MB_OK);
    }

    HINSTANCE instance_{};
    HWND window_{};
    HMENU database_menu_{};
    ChaRuntime* runtime_{};
    std::filesystem::path data_root_;
    std::filesystem::path config_directory_;
    std::filesystem::path webview_directory_;
    std::wstring runtime_origin_;
    std::wstring runtime_url_;
    std::optional<std::filesystem::path> assets_;
    std::optional<int> cdp_port_;
    std::optional<std::wstring> dev_origin_;
    ComPtr<ICoreWebView2Environment> environment_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
    ComPtr<IFileSaveDialog> save_dialog_;
    std::thread save_thread_;
    std::thread operation_thread_;
    std::thread shutdown_thread_;
    bool smoke_test_{};
    std::string connection_id_;
    int renderer_failures_{};
    bool initial_navigation_pending_{};
    bool save_operation_in_progress_{};
    bool database_operation_in_progress_{};
    bool close_pending_{};
    bool closing_{};
    bool fatal_error_posted_{};
    int exit_code_{};
};

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show_command) {
    bool smoke_test = false;
    bool com_initialized = false;
    try {
        const LaunchOptions options = parse_launch_options();
        smoke_test = options.smoke_test;
        const HRESULT com_result = ::CoInitializeEx(
            nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(com_result)) {
            throw std::runtime_error("Failed to initialize Windows COM");
        }
        com_initialized = true;
        (void)::SetCurrentProcessExplicitAppUserModelID(kApplicationId);

        std::shared_ptr<WindowsApplication> application =
            std::make_shared<WindowsApplication>();
        application->start(instance, show_command, options);
        const int result = application->run();
        application.reset();
        ::CoUninitialize();
        return result;
    } catch (const LaunchCancelled&) {
        if (com_initialized) ::CoUninitialize();
        return 0;
    } catch (const std::exception& error) {
        const bool argument_error = std::string_view(error.what()).find(
            "command-line arguments") != std::string_view::npos;
        if (!smoke_test && !argument_error) {
            std::wstring message;
            try {
                message = wide_from_utf8(error.what());
            } catch (...) {
                message = L"CHA encountered an unknown error.";
            }
            ::MessageBoxW(
                nullptr,
                message.c_str(),
                L"CHA cannot continue",
                MB_OK | MB_ICONERROR);
        }
        if (com_initialized) ::CoUninitialize();
        return argument_error ? 2 : 1;
    }
}
