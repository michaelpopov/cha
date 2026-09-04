import AppKit
import Darwin
import WebKit

private let applicationName = "CHA"
private let runtimePort = 8086

private enum LauncherError: LocalizedError {
    case setupCancelled
    case incompleteApplication
    case invalidAPIKey
    case setupFailed
    case cannotStart
    case stoppedUnexpectedly

    var errorDescription: String? {
        switch self {
        case .setupCancelled:
            return nil
        case .incompleteApplication:
            return "This copy of CHA is incomplete. Replace it with a fresh copy and try again."
        case .invalidAPIKey:
            return "The API key must not be empty or contain a line break."
        case .setupFailed:
            return "CHA couldn't finish setting itself up. Quit CHA and try again."
        case .cannotStart:
            return "CHA couldn't open. Close CHA if it is already running, then try again."
        case .stoppedUnexpectedly:
            return "CHA stopped unexpectedly. Quit CHA and open it again."
        }
    }
}

private enum RuntimeState {
    case unavailable
    case ready
    case occupied
}

private struct RuntimeRecord {
    let processIdentifier: Int32
    let executablePath: String
}

private struct DownloadDestination {
    let temporaryURL: URL
    let finalURL: URL
}

@MainActor
private final class ApplicationDelegate: NSObject, NSApplicationDelegate,
    WKNavigationDelegate, WKDownloadDelegate {
    private let fileManager = FileManager.default
    private var window: NSWindow!
    private var webView: WKWebView!
    private var runtime: Process?
    private var runtimeLog: FileHandle?
    private var downloadDestinations: [ObjectIdentifier: DownloadDestination] = [:]
    private var quitting = false
    private var readinessDeadline = Date()

    private var supportDirectory: URL {
        fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent(applicationName, isDirectory: true)
    }

    private var configFile: URL {
        supportDirectory.appendingPathComponent("cha.toml")
    }

    private var environmentFile: URL {
        supportDirectory.appendingPathComponent(".env")
    }

    private var databaseFile: URL {
        supportDirectory.appendingPathComponent("cha.sqlite3")
    }

    private var logFile: URL {
        supportDirectory.appendingPathComponent("launcher.log")
    }

    private var runtimeRecordFile: URL {
        supportDirectory.appendingPathComponent("runtime")
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        installMenus()
        showWindow()

        do {
            let apiKey = try prepareApplicationData()
            openApplication(apiKey: apiKey)
        } catch LauncherError.setupCancelled {
            NSApp.terminate(nil)
        } catch {
            showFatalError(error)
        }
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }

    func applicationWillTerminate(_ notification: Notification) {
        quitting = true
        if runtime?.isRunning == true {
            runtime?.terminate()
        }
        runtimeLog?.closeFile()
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
        let apiKeyItem = applicationMenu.addItem(
            withTitle: "Change API Key…",
            action: #selector(changeAPIKey(_:)),
            keyEquivalent: "")
        apiKeyItem.target = self
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

    private func prepareApplicationData() throws -> String {
        try createPrivateDirectory(supportDirectory)
        try createPrivateDirectory(supportDirectory.appendingPathComponent("logs", isDirectory: true))

        // Rewritten on every launch: the private address and database name below are
        // also compiled into this launcher, and an edited copy would leave it
        // opening the wrong address or importing over the wrong file.
        let config = """
            data = "\(databaseFile.lastPathComponent)"

            [web]
            host = "127.0.0.1"
            port = \(runtimePort)

            [logging]
            file = "logs/cha.log"
            level = "info"
            """
        try writePrivateFile(config, to: configFile)

        let apiKey = try loadOrAskForAPIKey()
        if !fileManager.fileExists(atPath: databaseFile.path) {
            try importInitialDatabase(apiKey: apiKey)
        }
        return apiKey
    }

    private func createPrivateDirectory(_ url: URL) throws {
        try fileManager.createDirectory(
            at: url,
            withIntermediateDirectories: true,
            attributes: [.posixPermissions: 0o700])
        try fileManager.setAttributes([.posixPermissions: 0o700], ofItemAtPath: url.path)
    }

    private func writePrivateFile(_ contents: String, to url: URL) throws {
        try contents.write(to: url, atomically: true, encoding: .utf8)
        try fileManager.setAttributes([.posixPermissions: 0o600], ofItemAtPath: url.path)
    }

    private func loadOrAskForAPIKey() throws -> String {
        if let inherited = ProcessInfo.processInfo.environment["OPENAI_API_KEY"],
           !inherited.isEmpty {
            return inherited
        }

        // The same reading chaweb's dotenv parser gives the file: the entry may
        // be indented, and the value is trimmed before one matching pair of
        // quotes comes off. Anything else would work under chaweb but not here,
        // because the key this returns is exported over the file.
        let contents = (try? String(contentsOf: environmentFile, encoding: .utf8)) ?? ""
        for line in contents.split(whereSeparator: \.isNewline) {
            let entry = line.trimmingCharacters(in: .whitespaces)
            guard entry.hasPrefix("OPENAI_API_KEY=") else { continue }
            var value = entry.dropFirst("OPENAI_API_KEY=".count)
                .trimmingCharacters(in: .whitespaces)
            if value.count >= 2,
               (value.hasPrefix("\"") && value.hasSuffix("\""))
                   || (value.hasPrefix("'") && value.hasSuffix("'")) {
                value.removeFirst()
                value.removeLast()
            }
            if !value.isEmpty {
                return value
            }
        }

        let value = try askForAPIKey(title: "Set up CHA", cancelTitle: "Quit")
        try writePrivateFile("OPENAI_API_KEY=\(value)\n", to: environmentFile)
        return value
    }

    private func askForAPIKey(title: String, cancelTitle: String) throws -> String {
        let field = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 360, height: 24))
        field.placeholderString = "OpenAI API key"

        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = "Enter the OpenAI API key CHA should use. It will be stored securely on this Mac."
        alert.accessoryView = field
        alert.addButton(withTitle: "Continue")
        alert.addButton(withTitle: cancelTitle)
        alert.window.initialFirstResponder = field

        guard alert.runModal() == .alertFirstButtonReturn else {
            throw LauncherError.setupCancelled
        }

        let value = field.stringValue.trimmingCharacters(in: .whitespaces)
        guard !value.isEmpty, !value.contains("\n"), !value.contains("\r") else {
            throw LauncherError.invalidAPIKey
        }
        return value
    }

    // The key is read once at launch, so a corrected one applies to the next
    // run. Restarting CHA underneath a live conversation would be more
    // machinery than a mistyped key is worth.
    @objc private func changeAPIKey(_ sender: Any?) {
        do {
            let value = try askForAPIKey(title: "Change API Key", cancelTitle: "Cancel")
            try writePrivateFile("OPENAI_API_KEY=\(value)\n", to: environmentFile)
            showNotice(
                "API key saved",
                detail: "CHA uses the new key the next time it starts.")
        } catch LauncherError.setupCancelled {
            // The dialog was dismissed.
        } catch {
            showNotice(
                "CHA could not save the API key",
                detail: "Quit CHA, open it again, and retry the change.")
        }
    }

    private func showNotice(_ message: String, detail: String) {
        let alert = NSAlert()
        alert.messageText = message
        alert.informativeText = detail
        alert.addButton(withTitle: "OK")
        alert.runModal()
    }

    private func bundledURL(_ name: String, isDirectory: Bool = false) throws -> URL {
        guard let resources = Bundle.main.resourceURL else {
            throw LauncherError.incompleteApplication
        }
        let url = resources.appendingPathComponent(name, isDirectory: isDirectory)
        guard fileManager.fileExists(atPath: url.path) else {
            throw LauncherError.incompleteApplication
        }
        return url
    }

    private func helperURL() throws -> URL {
        let url = Bundle.main.bundleURL
            .appendingPathComponent("Contents/Helpers/chaweb")
        guard fileManager.isExecutableFile(atPath: url.path) else {
            throw LauncherError.incompleteApplication
        }
        return url
    }

    private func importInitialDatabase(apiKey: String) throws {
        let process = Process()
        process.executableURL = try helperURL()
        process.arguments = [
            "--config", configFile.path,
            "--import", try bundledURL("import-seed", isDirectory: true).path,
        ]
        var environment = ProcessInfo.processInfo.environment
        environment["OPENAI_API_KEY"] = apiKey
        process.environment = environment

        let output = Pipe()
        process.standardOutput = output
        process.standardError = output
        try process.run()
        let data = output.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        guard process.terminationStatus == 0 else {
            if !data.isEmpty {
                try? writePrivateFile(String(decoding: data, as: UTF8.self), to: logFile)
            }
            throw LauncherError.setupFailed
        }
    }

    private func startRuntime(apiKey: String) throws {
        guard let resources = Bundle.main.resourceURL else {
            throw LauncherError.incompleteApplication
        }

        // Replaced rather than appended: the output that explains a failure is
        // this run's, and appending would grow the file for the life of the Mac.
        fileManager.createFile(atPath: logFile.path, contents: nil)
        try fileManager.setAttributes([.posixPermissions: 0o600], ofItemAtPath: logFile.path)
        let handle = try FileHandle(forWritingTo: logFile)
        runtimeLog = handle

        let process = Process()
        process.executableURL = try helperURL()
        process.arguments = [
            "--root", resources.path,
            "--config", configFile.path,
        ]
        var environment = ProcessInfo.processInfo.environment
        environment["OPENAI_API_KEY"] = apiKey
        process.environment = environment
        process.standardOutput = handle
        process.standardError = handle
        process.terminationHandler = { [weak self] process in
            DispatchQueue.main.async {
                guard let self else { return }
                self.removeRuntimeRecord(for: process.processIdentifier)
                guard !self.quitting else { return }
                self.showFatalError(LauncherError.stoppedUnexpectedly)
            }
        }
        try process.run()
        runtime = process
        guard let executablePath = executablePath(for: process.processIdentifier) else {
            process.terminate()
            throw LauncherError.cannotStart
        }
        do {
            try writePrivateFile(
                "\(process.processIdentifier)\n\(executablePath)\n",
                to: runtimeRecordFile)
        } catch {
            process.terminate()
            throw error
        }
        readinessDeadline = Date().addingTimeInterval(10)
    }

    private func openApplication(apiKey: String) {
        inspectRuntime { [weak self] state in
            guard let self, !self.quitting else { return }
            switch state {
            case .unavailable:
                self.startAndOpenApplication(apiKey: apiKey)
            case .ready:
                self.stopPreviousRuntime(apiKey: apiKey)
            case .occupied:
                self.showFatalError(LauncherError.cannotStart)
            }
        }
    }

    private func startAndOpenApplication(apiKey: String) {
        do {
            try startRuntime(apiKey: apiKey)
            waitForRuntime()
        } catch {
            showFatalError(error)
        }
    }

    // A force-quit can leave the previous runtime alive. The private record lets
    // a later copy verify exactly which process it created, stop it, and start
    // from the new application bundle instead of mixing two versions.
    private func stopPreviousRuntime(apiKey: String) {
        guard let record = readRuntimeRecord() else {
            showFatalError(LauncherError.cannotStart)
            return
        }

        if let currentPath = executablePath(for: record.processIdentifier) {
            guard currentPath == record.executablePath else {
                showFatalError(LauncherError.cannotStart)
                return
            }
            if kill(record.processIdentifier, SIGTERM) != 0 && errno != ESRCH {
                showFatalError(LauncherError.cannotStart)
                return
            }
        }

        readinessDeadline = Date().addingTimeInterval(12)
        waitForPreviousRuntime(record, apiKey: apiKey)
    }

    private func waitForPreviousRuntime(_ record: RuntimeRecord, apiKey: String) {
        guard !quitting else { return }
        if executablePath(for: record.processIdentifier) != record.executablePath {
            removeRuntimeRecord(for: record.processIdentifier)
            startAndOpenApplication(apiKey: apiKey)
        } else if Date() < readinessDeadline {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in
                self?.waitForPreviousRuntime(record, apiKey: apiKey)
            }
        } else {
            showFatalError(LauncherError.cannotStart)
        }
    }

    private func readRuntimeRecord() -> RuntimeRecord? {
        guard let contents = try? String(contentsOf: runtimeRecordFile, encoding: .utf8),
              let separator = contents.firstIndex(of: "\n"),
              let processIdentifier = Int32(contents[..<separator]),
              processIdentifier > 1 else { return nil }
        let pathStart = contents.index(after: separator)
        let executablePath = String(contents[pathStart...])
            .trimmingCharacters(in: .newlines)
        guard !executablePath.isEmpty else { return nil }
        return RuntimeRecord(
            processIdentifier: processIdentifier,
            executablePath: executablePath)
    }

    private func executablePath(for processIdentifier: Int32) -> String? {
        var buffer = [CChar](repeating: 0, count: 4096)
        guard proc_pidpath(processIdentifier, &buffer, UInt32(buffer.count)) > 0 else {
            return nil
        }
        return String(cString: buffer)
    }

    private func removeRuntimeRecord(for processIdentifier: Int32) {
        guard readRuntimeRecord()?.processIdentifier == processIdentifier else { return }
        try? fileManager.removeItem(at: runtimeRecordFile)
    }

    private func inspectRuntime(_ completion: @escaping (RuntimeState) -> Void) {
        guard let url = URL(string: "http://127.0.0.1:\(runtimePort)/health") else {
            completion(.unavailable)
            return
        }

        var request = URLRequest(url: url)
        request.timeoutInterval = 0.5
        URLSession.shared.dataTask(with: request) { data, response, _ in
            let state: RuntimeState
            if let response = response as? HTTPURLResponse {
                let body = data.flatMap {
                    try? JSONSerialization.jsonObject(with: $0) as? [String: Any]
                }
                if response.statusCode == 200,
                   body?["ready"] as? Bool == true,
                   let liveSessionCount = body?["live_session_count"] as? Int,
                   liveSessionCount >= 0 {
                    state = .ready
                } else {
                    state = .occupied
                }
            } else {
                state = .unavailable
            }
            DispatchQueue.main.async {
                completion(state)
            }
        }.resume()
    }

    private func waitForRuntime() {
        inspectRuntime { [weak self] state in
            guard let self, !self.quitting else { return }
            if state == .ready {
                self.showApplication()
            } else if self.runtime?.isRunning == true && Date() < self.readinessDeadline {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) {
                    self.waitForRuntime()
                }
            } else if self.runtime?.isRunning == true {
                self.showFatalError(LauncherError.cannotStart)
            }
        }
    }

    private func showApplication() {
        guard webView == nil,
              let url = URL(string: "http://127.0.0.1:\(runtimePort)/") else { return }
        let view = WKWebView(frame: .zero)
        view.allowsMagnification = true
        view.navigationDelegate = self
        webView = view
        window.contentView = view
        view.load(URLRequest(url: url))
        window.makeFirstResponder(view)
    }

    func webView(
        _ webView: WKWebView,
        decidePolicyFor navigationAction: WKNavigationAction,
        decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        if #available(macOS 11.3, *), navigationAction.shouldPerformDownload {
            decisionHandler(.download)
        } else {
            decisionHandler(.allow)
        }
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
        if runtime?.isRunning == true {
            runtime?.terminate()
        }

        let alert = NSAlert()
        alert.alertStyle = .critical
        alert.messageText = "CHA cannot continue"
        alert.informativeText = (error as? LauncherError)?.localizedDescription
            ?? LauncherError.cannotStart.localizedDescription
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
