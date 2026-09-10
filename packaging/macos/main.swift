import AppKit
import Darwin
import WebKit

private let applicationName = "CHA"

private enum LauncherError: LocalizedError {
    case incompleteApplication
    case cannotStart

    var errorDescription: String? {
        switch self {
        case .incompleteApplication:
            return "This copy of CHA is incomplete. Replace it with a fresh copy and try again."
        case .cannotStart:
            return "CHA couldn't open. Close CHA if it is already running, then try again."
        }
    }
}

private struct RuntimeBridgeError: LocalizedError {
    let message: String

    var errorDescription: String? { message }
}

// The runtime is process-owned and its C++ boundary serializes shutdown and
// database maintenance. This wrapper makes that explicit when work moves off
// the AppKit main actor.
private struct RuntimeHandle: @unchecked Sendable {
    let pointer: OpaquePointer
}

private enum DatabaseOperation {
    case importConfiguration
    case exportConfiguration
    case upload
    case download

    var progressTitle: String {
        switch self {
        case .importConfiguration: return "Importing"
        case .exportConfiguration: return "Exporting"
        case .upload: return "Uploading"
        case .download: return "Downloading"
        }
    }

    var title: String {
        switch self {
        case .importConfiguration: return "Import"
        case .exportConfiguration: return "Export"
        case .upload: return "Upload"
        case .download: return "Download"
        }
    }

    var reloadsApplication: Bool {
        self == .importConfiguration || self == .download
    }
}

private struct DownloadDestination {
    let temporaryURL: URL
    let finalURL: URL
}

@MainActor
private final class ApplicationDelegate: NSObject, NSApplicationDelegate,
    NSMenuDelegate, WKNavigationDelegate, WKUIDelegate, WKDownloadDelegate {
    private let fileManager = FileManager.default
    private var window: NSWindow!
    private var webView: WKWebView!
    private var webViewTitleObservation: NSKeyValueObservation?
    private var runtime: OpaquePointer?
    private var runtimeURL: URL?
    private var runtimeToken = ""
    private var importMenuItem: NSMenuItem!
    private var exportMenuItem: NSMenuItem!
    private var uploadMenuItem: NSMenuItem!
    private var downloadMenuItem: NSMenuItem!
    private var downloadDestinations: [ObjectIdentifier: DownloadDestination] = [:]
    private var databaseOperationInProgress = false
    private var terminationPending = false
    private var quitting = false

    private var supportDirectory: URL {
        fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent(applicationName, isDirectory: true)
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        installMenus()
        showWindow()

        do {
            try prepareApplicationData()
            try startRuntime()
            showApplication()
        } catch {
            showFatalError(error)
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    // A database operation runs off the main thread against the runtime pointer,
    // and applicationWillTerminate destroys it. Quitting therefore waits for
    // the operation to hand the pointer back, rather than freeing it underneath.
    func applicationShouldTerminate(
        _ sender: NSApplication) -> NSApplication.TerminateReply {
        guard databaseOperationInProgress else { return .terminateNow }
        terminationPending = true
        return .terminateLater
    }

    func applicationWillTerminate(_ notification: Notification) {
        quitting = true
        if let runtime {
            cha_runtime_destroy(runtime)
            self.runtime = nil
        }
    }

    private func installMenus() {
        let mainMenu = NSMenu()

        let applicationItem = NSMenuItem()
        let applicationMenu = NSMenu()
        applicationMenu.addItem(
            withTitle: "About \(applicationName)",
            action: #selector(NSApplication.orderFrontStandardAboutPanel(_:)),
            keyEquivalent: "")
        applicationMenu.addItem(.separator())
        applicationMenu.addItem(
            withTitle: "Quit \(applicationName)",
            action: #selector(NSApplication.terminate(_:)),
            keyEquivalent: "q")
        applicationItem.submenu = applicationMenu
        mainMenu.addItem(applicationItem)

        let databaseItem = NSMenuItem()
        let databaseMenu = NSMenu(title: "Database")
        // Without this AppKit recomputes the items from their target/action and
        // the greying-out during an operation never shows.
        databaseMenu.autoenablesItems = false
        importMenuItem = databaseMenu.addItem(
            withTitle: "Import…",
            action: #selector(importConfiguration(_:)),
            keyEquivalent: "")
        importMenuItem.target = self
        exportMenuItem = databaseMenu.addItem(
            withTitle: "Export",
            action: #selector(exportConfiguration(_:)),
            keyEquivalent: "")
        exportMenuItem.target = self
        databaseMenu.addItem(.separator())
        uploadMenuItem = databaseMenu.addItem(
            withTitle: "Upload",
            action: #selector(uploadDatabase(_:)),
            keyEquivalent: "")
        uploadMenuItem.target = self
        downloadMenuItem = databaseMenu.addItem(
            withTitle: "Download…",
            action: #selector(downloadDatabase(_:)),
            keyEquivalent: "")
        downloadMenuItem.target = self
        updateDatabaseMenuItems()
        databaseMenu.delegate = self
        databaseItem.submenu = databaseMenu
        mainMenu.addItem(databaseItem)

        let editItem = NSMenuItem()
        let editMenu = NSMenu(title: "Edit")
        editMenu.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
        editMenu.addItem(withTitle: "Redo", action: Selector(("redo:")), keyEquivalent: "Z")
        editMenu.addItem(.separator())
        editMenu.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        editMenu.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        editMenu.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        editMenu.addItem(
            withTitle: "Select All",
            action: #selector(NSText.selectAll(_:)),
            keyEquivalent: "a")
        editItem.submenu = editMenu
        mainMenu.addItem(editItem)

        let windowItem = NSMenuItem()
        let windowMenu = NSMenu(title: "Window")
        windowMenu.addItem(
            withTitle: "Minimize",
            action: #selector(NSWindow.performMiniaturize(_:)),
            keyEquivalent: "m")
        windowMenu.addItem(
            withTitle: "Zoom",
            action: #selector(NSWindow.performZoom(_:)),
            keyEquivalent: "")
        windowItem.submenu = windowMenu
        mainMenu.addItem(windowItem)
        NSApp.windowsMenu = windowMenu

        NSApp.mainMenu = mainMenu
    }

    private func showWindow() {
        window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 1040, height: 760),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false)
        window.title = applicationName
        if !window.setFrameUsingName("CHA.MainWindow") {
            window.center()
        }
        window.setFrameAutosaveName("CHA.MainWindow")

        let starting = NSTextField(labelWithString: "Starting CHA…")
        starting.alignment = .center
        starting.font = .systemFont(ofSize: 18)
        window.contentView = starting
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    private func prepareApplicationData() throws {
        try createPrivateDirectory(supportDirectory)
    }

    private func createPrivateDirectory(_ url: URL) throws {
        try fileManager.createDirectory(
            at: url,
            withIntermediateDirectories: true,
            attributes: [.posixPermissions: 0o700])
        try fileManager.setAttributes([.posixPermissions: 0o700], ofItemAtPath: url.path)
    }

    private func showNotice(_ message: String, detail: String) {
        let alert = NSAlert()
        alert.messageText = message
        alert.informativeText = detail
        alert.addButton(withTitle: "OK")
        alert.runModal()
    }

    private func takeBridgeError(
        _ pointer: UnsafeMutablePointer<CChar>?) -> RuntimeBridgeError {
        let message = pointer.map { String(cString: $0) }
            ?? "CHA encountered an unknown error."
        cha_string_free(pointer)
        return RuntimeBridgeError(message: message)
    }

    private func startRuntime() throws {
        guard let resources = Bundle.main.resourceURL else {
            throw LauncherError.incompleteApplication
        }
        runtimeToken = UUID().uuidString + UUID().uuidString
        var bridgeError: UnsafeMutablePointer<CChar>?
        let created = supportDirectory.path.withCString { configPath in
            resources.path.withCString { resourcePath in
                runtimeToken.withCString { token in
                    cha_runtime_create(
                        configPath, resourcePath, token, &bridgeError)
                }
            }
        }
        guard let created else { throw takeBridgeError(bridgeError) }
        let port = cha_runtime_port(created)
        guard port > 0,
              let url = URL(string: "http://127.0.0.1:\(port)/") else {
            cha_runtime_destroy(created)
            throw LauncherError.cannotStart
        }
        runtime = created
        runtimeURL = url
        updateDatabaseMenuItems()
    }

    private func showApplication() {
        guard webView == nil, let runtimeURL else { return }
        let configuration = WKWebViewConfiguration()
        if let script = voiceInputConfigurationScript() {
            configuration.userContentController.addUserScript(script)
        }
        if let script = textToSpeechConfigurationScript() {
            configuration.userContentController.addUserScript(script)
        }
        configuration.mediaTypesRequiringUserActionForPlayback = []
        let view = WKWebView(frame: .zero, configuration: configuration)
        view.allowsMagnification = true
        view.navigationDelegate = self
        view.uiDelegate = self
        webView = view
        window.contentView = view
        window.makeFirstResponder(view)
        webViewTitleObservation = view.observe(\.title, options: [.new]) {
            [weak self] _, _ in
            Task { @MainActor [weak self] in
                self?.updateWindowTitle()
            }
        }

        let properties: [HTTPCookiePropertyKey: Any] = [
            .domain: "127.0.0.1",
            .path: "/",
            .name: "CHA_RUNTIME",
            .value: runtimeToken,
            HTTPCookiePropertyKey("HttpOnly"): "TRUE",
            HTTPCookiePropertyKey("SameSite"): "Strict",
        ]
        guard let cookie = HTTPCookie(properties: properties) else {
            return showFatalError(LauncherError.cannotStart)
        }
        view.configuration.websiteDataStore.httpCookieStore.setCookie(cookie) {
            view.load(URLRequest(url: runtimeURL))
        }
    }

    private func voiceInputConfigurationScript() -> WKUserScript? {
        guard let runtime,
              let url = cha_runtime_voice_input_url(runtime),
              let apiKey = cha_runtime_voice_input_api_key(runtime),
              let model = cha_runtime_voice_input_model(runtime) else {
            return nil
        }
        var languages: [String] = []
        for index in 0..<Int(cha_runtime_voice_input_language_count(runtime)) {
            if let language = cha_runtime_voice_input_language(runtime, Int32(index)) {
                languages.append(String(cString: language))
            }
        }
        var keywords: [String] = []
        for index in 0..<Int(cha_runtime_voice_input_keyword_count(runtime)) {
            if let keyword = cha_runtime_voice_input_keyword(runtime, Int32(index)) {
                keywords.append(String(cString: keyword))
            }
        }
        let configuration: [String: Any] = [
            "url": String(cString: url),
            "apiKey": String(cString: apiKey),
            "model": String(cString: model),
            "languages": languages,
            "keywords": keywords,
        ]
        guard JSONSerialization.isValidJSONObject(configuration),
              let data = try? JSONSerialization.data(withJSONObject: configuration),
              let json = String(data: data, encoding: .utf8) else {
            return nil
        }
        return WKUserScript(
            source: "Object.defineProperty(window, 'chaVoiceInput', { value: \(json) });",
            injectionTime: .atDocumentStart,
            forMainFrameOnly: true)
    }

    private func textToSpeechConfigurationScript() -> WKUserScript? {
        guard let runtime,
              let apiKey = cha_runtime_text_to_speech_api_key(runtime) else {
            return nil
        }
        let configuration: [String: Any] = [
            "url": "https://api.elevenlabs.io/v1/text-to-speech/JBFqnCBsd6RMkjVDRZzb?output_format=mp3_44100_128",
            "apiKey": String(cString: apiKey),
            "model": "eleven_multilingual_v2",
        ]
        guard let data = try? JSONSerialization.data(withJSONObject: configuration),
              let json = String(data: data, encoding: .utf8) else {
            return nil
        }
        return WKUserScript(
            source: "Object.defineProperty(window, 'chaTextToSpeech', { value: \(json) });",
            injectionTime: .atDocumentStart,
            forMainFrameOnly: true)
    }

    @objc private func uploadDatabase(_ sender: Any?) {
        performDatabaseOperation(.upload)
    }

    @objc private func downloadDatabase(_ sender: Any?) {
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = "Replace the local database?"
        alert.informativeText =
            "CHA will save the current database beside itself with a .bac suffix, then use the database downloaded from R2."
        alert.addButton(withTitle: "Download")
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        performDatabaseOperation(.download)
    }

    @objc private func importConfiguration(_ sender: Any?) {
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = "Import workspace configuration?"
        alert.informativeText =
            "CHA will replace its workspace configuration with the contents of the directory named by “modify” in the active vault's configuration."
        alert.addButton(withTitle: "Import")
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        performDatabaseOperation(.importConfiguration)
    }

    @objc private func exportConfiguration(_ sender: Any?) {
        performDatabaseOperation(.exportConfiguration)
    }

    private func performDatabaseOperation(_ operation: DatabaseOperation) {
        guard !databaseOperationInProgress, let runtime else { return }
        let runtimeHandle = RuntimeHandle(pointer: runtime)
        databaseOperationInProgress = true
        updateDatabaseMenuItems()
        window.title = "\(applicationName) — \(operation.progressTitle)…"

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            var count: UInt64 = 0
            var bridgeError: UnsafeMutablePointer<CChar>?
            let succeeded: Int32
            switch operation {
            case .importConfiguration:
                succeeded = cha_runtime_import_configuration(
                    runtimeHandle.pointer, &count, &bridgeError)
            case .exportConfiguration:
                succeeded = cha_runtime_export_configuration(
                    runtimeHandle.pointer, &count, &bridgeError)
            case .upload:
                succeeded = cha_runtime_upload(
                    runtimeHandle.pointer, &count, &bridgeError)
            case .download:
                succeeded = cha_runtime_download(
                    runtimeHandle.pointer, &count, &bridgeError)
            }
            let errorMessage = bridgeError.map { String(cString: $0) }
            cha_string_free(bridgeError)

            DispatchQueue.main.async {
                guard let self else { return }
                self.databaseOperationInProgress = false
                self.updateDatabaseMenuItems()
                if self.terminationPending {
                    self.terminationPending = false
                    NSApp.reply(toApplicationShouldTerminate: true)
                    return
                }
                guard !self.quitting else { return }
                self.updateWindowTitle()

                if succeeded < 0 {
                    return self.showFatalError(RuntimeBridgeError(
                        message: errorMessage
                            ?? "CHA can no longer reach its database."))
                }
                guard succeeded != 0 else {
                    self.showNotice(
                        "\(operation.title) failed",
                        detail: errorMessage ?? "CHA encountered an unknown error.")
                    return
                }
                if operation.reloadsApplication, let runtimeURL = self.runtimeURL {
                    self.webView.load(URLRequest(url: runtimeURL))
                }
                let detail: String
                switch operation {
                case .importConfiguration:
                    detail = "Imported \(count) files."
                case .exportConfiguration:
                    detail = "Exported \(count) files."
                case .upload, .download:
                    let size = ByteCountFormatter.string(
                        fromByteCount: Int64(count), countStyle: .file)
                    detail = "Transferred \(size)."
                }
                self.showNotice(
                    "\(operation.title) complete",
                    detail: detail)
            }
        }
    }

    private func updateWindowTitle() {
        guard !databaseOperationInProgress else { return }
        let title = webView?.title?.trimmingCharacters(in: .whitespacesAndNewlines)
        if let title, !title.isEmpty {
            window.title = title
        } else {
            window.title = applicationName
        }
    }

    func menuNeedsUpdate(_: NSMenu) {
        updateDatabaseMenuItems()
    }

    private func updateDatabaseMenuItems() {
        let canModify = runtime.map { cha_runtime_can_modify($0) != 0 } ?? false
        let canTransferR2 = runtime.map {
            cha_runtime_can_transfer_r2($0) != 0
        } ?? false
        let idle = !databaseOperationInProgress
        importMenuItem.isEnabled = idle && canModify
        exportMenuItem.isEnabled = idle && canModify
        uploadMenuItem.isEnabled = idle && canTransferR2
        downloadMenuItem.isEnabled = idle && canTransferR2
    }

    func webView(
        _ webView: WKWebView,
        decidePolicyFor navigationAction: WKNavigationAction,
        decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        if #available(macOS 11.3, *), navigationAction.shouldPerformDownload {
            decisionHandler(.download)
            return
        }

        guard let url = navigationAction.request.url else {
            decisionHandler(.allow)
            return
        }
        if isApplicationURL(url) {
            decisionHandler(.allow)
            return
        }
        // Open external links here, including target="_blank": cancelling
        // navigation prevents createWebViewWith from being called.
        openHTTPSInSystemBrowser(url)
        decisionHandler(.cancel)
    }

    func webView(
        _ webView: WKWebView,
        createWebViewWith configuration: WKWebViewConfiguration,
        for navigationAction: WKNavigationAction,
        windowFeatures: WKWindowFeatures) -> WKWebView? {
        guard let url = navigationAction.request.url else { return nil }
        if isApplicationURL(url) {
            // An internal target="_blank" has no other window to open in;
            // keep it in CHA instead of dropping it.
            self.webView?.load(navigationAction.request)
        } else {
            openHTTPSInSystemBrowser(url)
        }
        return nil
    }

    func webView(
        _ webView: WKWebView,
        runOpenPanelWith parameters: WKOpenPanelParameters,
        initiatedByFrame frame: WKFrameInfo,
        completionHandler: @escaping ([URL]?) -> Void) {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = parameters.allowsMultipleSelection
        panel.canChooseDirectories = parameters.allowsDirectories
        panel.canChooseFiles = !parameters.allowsDirectories
        panel.beginSheetModal(for: window) { result in
            completionHandler(result == .OK ? panel.urls : nil)
        }
    }

    func webView(
        _ webView: WKWebView,
        requestMediaCapturePermissionFor origin: WKSecurityOrigin,
        initiatedByFrame frame: WKFrameInfo,
        type: WKMediaCaptureType,
        decisionHandler: @escaping (WKPermissionDecision) -> Void) {
        guard type == .microphone,
              let runtimeURL,
              origin.protocol == runtimeURL.scheme,
              origin.host == runtimeURL.host,
              origin.port == runtimeURL.port else {
            decisionHandler(.deny)
            return
        }
        decisionHandler(.grant)
    }

    private func isApplicationURL(_ url: URL) -> Bool {
        guard let runtimeURL,
              url.scheme?.caseInsensitiveCompare("http") == .orderedSame,
              url.host == runtimeURL.host,
              url.port == runtimeURL.port else {
            return false
        }
        return true
    }

    private func openHTTPSInSystemBrowser(_ url: URL) {
        guard url.scheme?.caseInsensitiveCompare("https") == .orderedSame else {
            return
        }
        NSWorkspace.shared.open(url)
    }

    @available(macOS 11.3, *)
    func webView(
        _ webView: WKWebView,
        navigationAction: WKNavigationAction,
        didBecome download: WKDownload) {
        download.delegate = self
    }

    @available(macOS 11.3, *)
    func download(
        _ download: WKDownload,
        decideDestinationUsing response: URLResponse,
        suggestedFilename: String,
        completionHandler: @escaping (URL?) -> Void) {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = suggestedFilename
        panel.beginSheetModal(for: window) { result in
            guard result == .OK, let finalURL = panel.url else {
                completionHandler(nil)
                return
            }

            // WebKit requires a destination that does not exist. Download beside
            // the chosen file, then replace it only after the transfer succeeds.
            let temporaryURL = finalURL.deletingLastPathComponent()
                .appendingPathComponent(".cha-download-\(UUID().uuidString)")
            self.downloadDestinations[ObjectIdentifier(download)] = DownloadDestination(
                temporaryURL: temporaryURL,
                finalURL: finalURL)
            completionHandler(temporaryURL)
        }
    }

    @available(macOS 11.3, *)
    func downloadDidFinish(_ download: WKDownload) {
        guard let destination = downloadDestinations.removeValue(
            forKey: ObjectIdentifier(download)) else { return }
        do {
            if fileManager.fileExists(atPath: destination.finalURL.path) {
                _ = try fileManager.replaceItemAt(
                    destination.finalURL,
                    withItemAt: destination.temporaryURL)
            } else {
                try fileManager.moveItem(
                    at: destination.temporaryURL,
                    to: destination.finalURL)
            }
        } catch {
            try? fileManager.removeItem(at: destination.temporaryURL)
            showNotice(
                "CHA could not save the file",
                detail: "Choose another location and try again.")
        }
    }

    @available(macOS 11.3, *)
    func download(
        _ download: WKDownload,
        didFailWithError error: Error,
        resumeData: Data?) {
        guard let destination = downloadDestinations.removeValue(
            forKey: ObjectIdentifier(download)) else { return }
        try? fileManager.removeItem(at: destination.temporaryURL)
        guard !quitting else { return }
        showNotice(
            "CHA could not save the file",
            detail: "Choose another location and try again.")
    }

    private func showFatalError(_ error: Error) {
        guard !quitting else { return }
        quitting = true
        if let runtime {
            cha_runtime_destroy(runtime)
            self.runtime = nil
        }

        let alert = NSAlert()
        alert.alertStyle = .critical
        alert.messageText = "CHA cannot continue"
        alert.informativeText = error.localizedDescription
        alert.addButton(withTitle: "Quit")
        alert.runModal()
        NSApp.terminate(nil)
    }
}

@main
private struct Main {
    @MainActor
    static func main() {
        let application = NSApplication.shared
        let delegate = ApplicationDelegate()
        application.setActivationPolicy(.regular)
        application.delegate = delegate
        application.run()
    }
}
