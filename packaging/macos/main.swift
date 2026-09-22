import AppKit
import Darwin
import WebKit

private let applicationName = "CHA"

private enum LauncherError: LocalizedError {
    case incompleteApplication

    var errorDescription: String? {
        switch self {
        case .incompleteApplication:
            return "This copy of CHA is incomplete. Replace it with a fresh copy and try again."
        }
    }
}

private struct RuntimeBridgeError: LocalizedError {
    let message: String

    var errorDescription: String? { message }
}

// The runtime is process-owned. This wrapper makes that explicit when shutdown
// work moves off the AppKit main actor.
private struct RuntimeHandle: @unchecked Sendable {
    let pointer: OpaquePointer
}

private struct DownloadDestination {
    let temporaryURL: URL
    let finalURL: URL
}

@MainActor
private final class ApplicationDelegate: NSObject, NSApplicationDelegate,
    WKNavigationDelegate, WKUIDelegate, WKDownloadDelegate {
    private let fileManager = FileManager.default
    private var window: NSWindow!
    private var webView: WKWebView!
    private var nativeBridge: ChaNativeBridgeReceiver?
    private var webViewTitleObservation: NSKeyValueObservation?
    private var runtime: OpaquePointer?
    private var runtimeURL: URL?
    private var downloadDestinations: [ObjectIdentifier: DownloadDestination] = [:]
    private let nativeFileOperations = DispatchGroup()
    private var savePanel: NSSavePanel?
    private var quitting = false
    private var rendererFailures = 0
    private let rendererFailureLimit = 3

    private var supportDirectory: URL {
        fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent(applicationName, isDirectory: true)
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        installMenus()
        showWindow()

        do {
            try prepareApplicationData()
            guard try startRuntime() else {
                NSApp.terminate(nil)
                return
            }
            showApplication()
        } catch {
            showFatalError(error)
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    func applicationShouldTerminate(
        _ sender: NSApplication) -> NSApplication.TerminateReply {
        guard runtime != nil, !quitting else { return .terminateNow }
        quitting = true
        window.title = "Closing CHA…"
        shutdownRuntimeThen {
            NSApp.reply(toApplicationShouldTerminate: true)
        }
        return .terminateLater
    }

    func applicationWillTerminate(_ notification: Notification) {
        quitting = true
        nativeBridge?.detach()
        nativeBridge = nil
        // Join and destroy run off the UI thread from applicationShouldTerminate
        // or showFatalError. Blocking here would wait on owners for up to 10s.
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

    private func requestVaultPassword(name: String, error: String? = nil) -> String? {
        let alert = NSAlert()
        alert.messageText = "Open vault “\(name)”"
        alert.informativeText = error ?? "Enter the vault password."
        alert.addButton(withTitle: "Open vault")
        alert.addButton(withTitle: "Quit")
        let field = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 280, height: 24))
        field.placeholderString = "Password"
        alert.accessoryView = field
        alert.window.initialFirstResponder = field
        guard alert.runModal() == .alertFirstButtonReturn else { return nil }
        return field.stringValue
    }

    private func startRuntime() throws -> Bool {
        guard let resources = Bundle.main.resourceURL else {
            throw LauncherError.incompleteApplication
        }
        var requirementError: UnsafeMutablePointer<CChar>?
        var vaultName: UnsafeMutablePointer<CChar>?
        let passwordRequired = supportDirectory.path.withCString { configPath in
            resources.path.withCString { resourcePath in
                cha_runtime_requires_password(
                    configPath, resourcePath, &vaultName, &requirementError)
            }
        }
        guard passwordRequired >= 0 else {
            throw takeBridgeError(requirementError)
        }
        let selectedVaultName = vaultName.map { String(cString: $0) } ?? ""
        cha_string_free(vaultName)

        var password = ""
        if passwordRequired != 0 {
            guard let entered = requestVaultPassword(name: selectedVaultName) else { return false }
            password = entered
        }
        while true {
            var bridgeError: UnsafeMutablePointer<CChar>?
            var passwordError: Int32 = 0
            let created = supportDirectory.path.withCString { configPath in
                resources.path.withCString { resourcePath in
                    password.withCString { passwordValue in
                        cha_runtime_create(
                            configPath,
                            resourcePath,
                            passwordValue,
                            &passwordError,
                            &bridgeError)
                    }
                }
            }
            if let created {
                runtimeURL = URL(string: "\(chaAssetOrigin)/")
                runtime = created
                return true
            }
            let error = takeBridgeError(bridgeError)
            guard passwordError != 0 else { throw error }
            guard let entered = requestVaultPassword(name: selectedVaultName, error: error.message) else {
                return false
            }
            password = entered
        }
    }

    private func showApplication() {
        guard webView == nil, let runtimeURL else { return }
        guard let resources = Bundle.main.resourceURL else {
            return showFatalError(LauncherError.incompleteApplication)
        }
        let assets = resources.appendingPathComponent("web", isDirectory: true)
        let built = makeNativeWebViewConfiguration(assetRoot: assets)
        let view = WKWebView(frame: .zero, configuration: built.0)
        attachWebView(view)
        if let runtime {
            let receiver = ChaNativeBridgeReceiver(runtime: runtime)
            receiver.saveSession = {
                [weak self] connectionId, id, contextEpoch, suggestedName, forumId, sessionId in
                self?.saveNativeSession(
                    connectionId: connectionId,
                    id: id,
                    contextEpoch: contextEpoch,
                    suggestedName: suggestedName,
                    forumId: forumId,
                    sessionId: sessionId)
            }
            receiver.attach(to: view, mediaHandler: built.1)
            nativeBridge = receiver
        }
        view.load(URLRequest(url: runtimeURL))
    }

    private func attachWebView(_ view: WKWebView) {
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
    }

    private func updateWindowTitle() {
        guard !quitting else { return }
        let title = webView?.title?.trimmingCharacters(in: .whitespacesAndNewlines)
        if let title, !title.isEmpty {
            window.title = title
        } else {
            window.title = applicationName
        }
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
            nativeBridge?.willNavigate(navigationAction, in: webView)
            decisionHandler(.allow)
            return
        }
        // Open external links here, including target="_blank": cancelling
        // navigation prevents createWebViewWith from being called.
        openHTTPSInSystemBrowser(url)
        decisionHandler(.cancel)
    }

    func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
        rendererFailures += 1
        nativeBridge?.prepareDocumentReplacement()
        if rendererFailures >= rendererFailureLimit {
            showFatalError(RuntimeBridgeError(
                message: "CHA's browser process stopped repeatedly. Quit and open CHA again."))
            return
        }
        webView.reload()
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
        guard type == .microphone, frame.isMainFrame, isChaAssetOrigin(origin) else {
            decisionHandler(.deny)
            return
        }
        decisionHandler(.grant)
    }

    private func isApplicationURL(_ url: URL) -> Bool {
        isChaAssetURL(url)
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

    private func saveNativeSession(
        connectionId: String,
        id: String,
        contextEpoch: UInt64,
        suggestedName: String,
        forumId: String,
        sessionId: String
    ) {
        guard !quitting, savePanel == nil, let runtime else {
            nativeBridge?.completeSave(
                connectionId: connectionId,
                id: id,
                ok: false,
                message: "unavailable")
            return
        }
        var currentEpoch: UInt64 = 0
        if cha_runtime_context_epoch(runtime, &currentEpoch) == 0
            || currentEpoch != contextEpoch {
            nativeBridge?.completeSave(
                connectionId: connectionId,
                id: id,
                ok: false,
                message: "unavailable")
            return
        }
        let panel = NSSavePanel()
        savePanel = panel
        panel.nameFieldStringValue = suggestedName
        panel.beginSheetModal(for: window) { [weak self] result in
            guard let self else { return }
            if self.savePanel === panel { self.savePanel = nil }
            guard !self.quitting else { return }
            guard self.isCurrentNativeDocument(
                connectionId: connectionId,
                contextEpoch: contextEpoch) else { return }
            guard result == .OK, let destination = panel.url else {
                self.nativeBridge?.completeSave(
                    connectionId: connectionId,
                    id: id,
                    ok: true,
                    message: "cancelled")
                return
            }
            let path = destination.path
            self.nativeFileOperations.enter()
            let operations = self.nativeFileOperations
            DispatchQueue.global(qos: .userInitiated).async {
                defer { operations.leave() }
                var error: UnsafeMutablePointer<CChar>?
                let status = forumId.withCString { forum in
                    sessionId.withCString { session in
                        cha_runtime_export_session(
                            runtime,
                            contextEpoch,
                            forum,
                            session,
                            path,
                            &error)
                    }
                }
                let message = error.map { String(cString: $0) } ?? ""
                cha_string_free(error)
                DispatchQueue.main.async {
                    guard self.isCurrentNativeDocument(
                        connectionId: connectionId,
                        contextEpoch: contextEpoch) else { return }
                    self.nativeBridge?.completeSave(
                        connectionId: connectionId,
                        id: id,
                        ok: status == 1,
                        message: message)
                }
            }
        }
    }

    private func isCurrentNativeDocument(
        connectionId: String,
        contextEpoch: UInt64
    ) -> Bool {
        guard nativeBridge?.connectionId == connectionId, let runtime else {
            return false
        }
        var currentEpoch: UInt64 = 0
        return cha_runtime_context_epoch(runtime, &currentEpoch) != 0
            && currentEpoch == contextEpoch
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

    private func shutdownRuntimeThen(_ completion: @escaping () -> Void) {
        if let panel = savePanel {
            savePanel = nil
            panel.cancel(nil)
        }
        nativeBridge?.detach()
        nativeBridge = nil
        guard let runtime else {
            completion()
            return
        }
        cha_runtime_request_shutdown(runtime)
        let handle = RuntimeHandle(pointer: runtime)
        let operations = nativeFileOperations
        let deadline = DispatchTime.now() + .seconds(10)
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            if operations.wait(timeout: deadline) == .timedOut {
                FileHandle.standardError.write(
                    Data("CHA shutdown timed out waiting for a file operation\n".utf8))
                _exit(EXIT_FAILURE)
            }
            let now = DispatchTime.now().uptimeNanoseconds
            guard now < deadline.uptimeNanoseconds else {
                _exit(EXIT_FAILURE)
            }
            let remainingNanoseconds = deadline.uptimeNanoseconds - now
            let remainingMilliseconds = min(
                UInt64(Int32.max),
                (remainingNanoseconds + 999_999) / 1_000_000)
            guard cha_runtime_join_shutdown(
                handle.pointer,
                Int32(remainingMilliseconds)) != 0 else {
                FileHandle.standardError.write(
                    Data("CHA application shutdown timed out\n".utf8))
                _exit(EXIT_FAILURE)
            }
            DispatchQueue.main.async {
                cha_runtime_destroy(handle.pointer)
                self?.runtime = nil
                completion()
            }
        }
    }

    private func showFatalError(_ error: Error) {
        guard !quitting else { return }
        quitting = true
        shutdownRuntimeThen {
            let alert = NSAlert()
            alert.alertStyle = .critical
            alert.messageText = "CHA cannot continue"
            alert.informativeText = error.localizedDescription
            alert.addButton(withTitle: "Quit")
            alert.runModal()
            NSApp.terminate(nil)
        }
    }
}

private func parseLaunchOptions() {
    let arguments = Array(CommandLine.arguments.dropFirst()).filter { argument in
        if argument.hasPrefix("-psn")
            || (argument.hasPrefix("-") && !argument.hasPrefix("--")) {
            FileHandle.standardError.write(
                Data("warning: ignoring unused launch argument \(argument)\n".utf8))
            return false
        }
        return true
    }
    if !arguments.isEmpty {
        FileHandle.standardError.write(
            Data("CHA does not accept command-line arguments\n".utf8))
        exit(2)
    }
}

@main
private struct Main {
    @MainActor
    static func main() {
        parseLaunchOptions()
        let application = NSApplication.shared
        let delegate = ApplicationDelegate()
        application.setActivationPolicy(.regular)
        application.delegate = delegate
        application.run()
    }
}
