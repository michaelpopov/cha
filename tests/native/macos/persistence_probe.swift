// Compiled only into the instrumented production host by test-persistence.py.
import AppKit
import Foundation
import WebKit

@MainActor
final class NativePersistenceProbe: NSObject, WKScriptMessageHandlerWithReply {
    private weak var view: WKWebView?
    private let config: URL
    private var firstReport: NativeViewState?
    private var phase = 0
    private var startupMenu: [String: Bool] = [:]

    init(view: WKWebView, config: URL) {
        self.view = view
        self.config = config
        super.init()
        startupMenu = checkmarks
        view.configuration.userContentController.addScriptMessageHandler(
            self, contentWorld: .page, name: "chaPersistenceTest")
    }

    func record(_ state: NativeViewState) {
        if firstReport == nil { firstReport = state }
    }

    private var menu: NSMenu { NSApp.mainMenu!.items.compactMap(\.submenu).first { $0.title == "View" }! }
    private var checkmarks: [String: Bool] {
        let keys = ["split", "sidebar", "pictures", "toolbar", "chat_toolbar", "notes_prompt", "russian"]
        return Dictionary(uniqueKeysWithValues: zip(keys, menu.items.map { $0.state == .on }))
    }

    func userContentController(_ userContentController: WKUserContentController,
        didReceive message: WKScriptMessage, replyHandler: @escaping (Any?, String?) -> Void) {
        guard let request = message.body as? [String: Any], let action = request["action"] as? String else {
            replyHandler(nil, "Invalid test request"); return
        }
        switch action {
        case "menu":
            guard let title = request["title"] as? String, let item = menu.item(withTitle: title),
                  let selector = item.action else { replyHandler(nil, "Missing View menu item"); return }
            NSApp.sendAction(selector, to: item.target, from: item)
            replyHandler(true, nil)
        case "hold":
            FileHandle.standardOutput.write(Data("PERSISTENCE_READY\n".utf8))
            // The Python runner releases this instance after another one saves.
            // Keep the main run loop free to process reports while it waits.
            DispatchQueue.global().async {
                _ = try? FileHandle.standardInput.read(upToCount: 1)
                DispatchQueue.main.async { replyHandler(true, nil) }
            }
        case "snapshot":
            replyHandler(["menu": checkmarks, "first": firstReport?.object ?? [:]], nil)
        case "writable":
            do {
                try FileManager.default.setAttributes([.posixPermissions: request["value"] as? Bool == true ? 0o700 : 0o500], ofItemAtPath: config.path)
                replyHandler(true, nil)
            } catch { replyHandler(nil, error.localizedDescription) }
        default: replyHandler(nil, "Unknown test action")
        }
    }

    func run() {
        let env = ProcessInfo.processInfo.environment
        do {
            let source = try String(contentsOfFile: env["CHA_PERSISTENCE_SCRIPT"]!, encoding: .utf8)
            let expected = try JSONSerialization.jsonObject(with: Data(env["CHA_PERSISTENCE_EXPECT"]!.utf8))
            let currentPhase = phase
            phase += 1
            view?.callAsyncJavaScript(source,
                arguments: ["expected": expected, "mode": env["CHA_PERSISTENCE_MODE"] ?? "observe", "phase": currentPhase],
                in: nil, in: .page) { outcome in
                switch outcome {
                case .failure(let error):
                    FileHandle.standardError.write(Data("FAIL persistence: \(error)\n".utf8))
                    exit(1)
                case .success(let result):
                    if result as? String == "reload" { self.firstReport = nil; self.view?.reload(); return }
                    FileHandle.standardOutput.write(Data("PASS persistence probe\n".utf8))
                    // Use the production termination negotiation and runtime shutdown.
                    NSApp.terminate(nil)
                }
            }
        } catch {
            FileHandle.standardError.write(Data("FAIL persistence: \(error)\n".utf8))
            exit(1)
        }
    }

    func reportExit(_ state: NativeViewState) {
        let report: [String: Any] = ["state": state.object, "menu": checkmarks, "first": firstReport?.object ?? [:], "startup": startupMenu]
        let data = try! JSONSerialization.data(withJSONObject: report, options: .sortedKeys)
        FileHandle.standardOutput.write(Data("PERSISTENCE_EXIT ".utf8) + data + Data("\n".utf8))
    }
}
