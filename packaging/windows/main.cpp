#include "runtime_bridge.h"

#include "util/path_name.h"
#include "util/private_filesystem.h"

#include <windows.h>

#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl.h>

#include <WebView2.h>

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
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

std::string random_access_token() {
    std::array<unsigned char, 32> bytes{};
    if (!BCRYPT_SUCCESS(::BCryptGenRandom(
            nullptr,
            bytes.data(),
            static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        throw std::runtime_error("Failed to create the private runtime token");
    }
    constexpr char digits[] = "0123456789abcdef";
    std::string token;
    token.reserve(bytes.size() * 2);
    for (const unsigned char byte : bytes) {
        token.push_back(digits[byte >> 4]);
        token.push_back(digits[byte & 0x0f]);
    }
    return token;
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

struct LaunchOptions {
    bool smoke_test{};
    std::optional<std::filesystem::path> data_root;
};

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
    if (count == 3 && std::wstring_view(arguments[1]) == L"--smoke-test") {
        return {
            .smoke_test = true,
            .data_root = std::filesystem::path(arguments[2]),
        };
    }
    throw std::runtime_error("CHA does not accept command-line arguments");
}

class WindowsApplication final
    : public std::enable_shared_from_this<WindowsApplication> {
public:
    ~WindowsApplication() {
        if (operation_thread_.joinable()) operation_thread_.join();
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
        window_class.hCursor = ::LoadCursorW(
            nullptr, MAKEINTRESOURCEW(32512));
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
        runtime_token_ = random_access_token();
        const std::filesystem::path resources = cha::executable_directory();
        const std::string config = cha::utf8_path(config_directory_);
        const std::string resource_path = cha::utf8_path(resources);
        char* bridge_error = nullptr;
        runtime_ = cha_runtime_create(
            config.c_str(),
            resource_path.c_str(),
            runtime_token_.c_str(),
            &bridge_error);
        if (runtime_ == nullptr) {
            throw std::runtime_error(take_bridge_error(bridge_error));
        }
        const int32_t port = cha_runtime_port(runtime_);
        if (port <= 0) {
            throw std::runtime_error("CHA could not start its private server");
        }
        runtime_origin_ = L"http://127.0.0.1:" + std::to_wstring(port);
        runtime_url_ = runtime_origin_ + L"/";
        update_database_menu_items();
    }

    void start_webview() {
        const std::shared_ptr<WindowsApplication> self = shared_from_this();
        const HRESULT result = ::CreateCoreWebView2EnvironmentWithOptions(
            nullptr,
            webview_directory_.c_str(),
            nullptr,
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
        result = install_runtime_cookie();
        if (FAILED(result)) {
            post_fatal_error(hresult_message(
                result, L"CHA could not secure its private browser session"));
            return S_OK;
        }
        install_voice_input_and_navigate();
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
                        && is_application_uri(uri);
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
        }
    }

    HRESULT install_runtime_cookie() {
        ComPtr<ICoreWebView2_2> webview2;
        HRESULT result = webview_.As(&webview2);
        if (FAILED(result)) return result;
        ComPtr<ICoreWebView2CookieManager> manager;
        result = webview2->get_CookieManager(&manager);
        if (FAILED(result)) return result;
        ComPtr<ICoreWebView2Cookie> cookie;
        const std::wstring token = wide_from_utf8(runtime_token_);
        result = manager->CreateCookie(
            L"CHA_RUNTIME", token.c_str(), L"127.0.0.1", L"/", &cookie);
        if (FAILED(result)) return result;
        if (FAILED(result = cookie->put_IsHttpOnly(TRUE))) return result;
        if (FAILED(result = cookie->put_SameSite(
                COREWEBVIEW2_COOKIE_SAME_SITE_KIND_STRICT))) {
            return result;
        }
        if (FAILED(result = cookie->put_Expires(-1.0))) return result;
        return manager->AddOrUpdateCookie(cookie.Get());
    }

    std::wstring voice_input_script() const {
        const char* const url = cha_runtime_voice_input_url(runtime_);
        const char* const api_key = cha_runtime_voice_input_api_key(runtime_);
        const char* const model = cha_runtime_voice_input_model(runtime_);
        if (url == nullptr || api_key == nullptr || model == nullptr) return {};

        nlohmann::json configuration{
            {"url", url},
            {"apiKey", api_key},
            {"model", model},
            {"languages", nlohmann::json::array()},
            {"keywords", nlohmann::json::array()},
            {"blockDurationMs",
             cha_runtime_voice_input_block_duration_s(runtime_) * 1000},
        };
        for (int32_t index = 0;
             index < cha_runtime_voice_input_language_count(runtime_);
             ++index) {
            if (const char* value = cha_runtime_voice_input_language(runtime_, index)) {
                configuration["languages"].push_back(value);
            }
        }
        for (int32_t index = 0;
             index < cha_runtime_voice_input_keyword_count(runtime_);
             ++index) {
            if (const char* value = cha_runtime_voice_input_keyword(runtime_, index)) {
                configuration["keywords"].push_back(value);
            }
        }
        return wide_from_utf8(
            "Object.defineProperty(window, 'chaVoiceInput', { value: "
            + configuration.dump() + " });");
    }

    void install_voice_input_and_navigate() {
        const std::wstring script = voice_input_script();
        if (script.empty()) {
            navigate_home();
            return;
        }
        const std::shared_ptr<WindowsApplication> self = shared_from_this();
        const HRESULT result = webview_->AddScriptToExecuteOnDocumentCreated(
            script.c_str(),
            Callback<ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler>(
                [self](HRESULT status, LPCWSTR) {
                    if (FAILED(status)) {
                        self->post_fatal_error(hresult_message(
                            status, L"CHA could not configure voice input"));
                    } else {
                        self->navigate_home();
                    }
                    return S_OK;
                }).Get());
        if (FAILED(result)) {
            post_fatal_error(hresult_message(
                result, L"CHA could not configure voice input"));
        }
    }

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

    bool is_application_uri(std::wstring_view uri) const {
        if (uri == runtime_origin_) return true;
        const std::wstring prefix = runtime_origin_ + L"/";
        if (starts_with_case_insensitive(uri, prefix)) return true;
        const std::wstring blob_prefix = L"blob:" + prefix;
        return starts_with_case_insensitive(uri, blob_prefix);
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
        close_now();
    }

    void close_now() {
        if (closing_) return;
        closing_ = true;
        shutdown_runtime();
        if (window_ != nullptr && ::IsWindow(window_)) {
            ::DestroyWindow(window_);
        }
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
        close_now();
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
    std::string runtime_token_;
    std::wstring runtime_origin_;
    std::wstring runtime_url_;
    ComPtr<ICoreWebView2Environment> environment_;
    ComPtr<ICoreWebView2Controller> controller_;
    ComPtr<ICoreWebView2> webview_;
    std::thread operation_thread_;
    bool smoke_test_{};
    bool initial_navigation_pending_{};
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
    } catch (const std::exception& error) {
        if (!smoke_test) {
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
        return 1;
    }
}
