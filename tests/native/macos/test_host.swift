import AppKit
import Darwin
import Foundation
import WebKit

private enum ProbeExpectation: String {
    case pass
    case fail
    case timeout
    case audio
    case media
    case parity
    case flow
    case reload
    case rendererFail = "renderer-fail"
    case stall
    case quit
}

private struct HostOptions {
    var assets: URL
    var expectation = ProbeExpectation.pass
    var timeoutMs = 45000
    var config: URL?
    var devOrigin: URL?
}

private enum HostError: LocalizedError {
    case usage
    case probeFailed(String)
    case timedOut

    var errorDescription: String? {
        switch self {
        case .usage:
            return "usage: cha_macos_native_test_host --assets <dir> [--config <dir>] [--dev-origin http://127.0.0.1:5173] [--expect pass|fail|timeout|audio|media|parity|flow|reload|renderer-fail|stall|quit] [--timeout-ms N]"
        case .probeFailed(let detail):
            return detail
        case .timedOut:
            return "native probe timed out"
        }
    }
}

private func parseOptions() throws -> HostOptions {
    let arguments = Array(CommandLine.arguments.dropFirst())
    var assets: URL?
    var expectation = ProbeExpectation.pass
    var timeoutMs = 45000
    var config: URL?
    var devOrigin: URL?
    var index = 0
    while index < arguments.count {
        let argument = arguments[index]
        func takeValue() throws -> String {
            index += 1
            guard index < arguments.count else { throw HostError.usage }
            return arguments[index]
        }
        switch argument {
        case "--assets":
            assets = URL(fileURLWithPath: try takeValue(), isDirectory: true)
        case "--config":
            config = URL(fileURLWithPath: try takeValue(), isDirectory: true)
        case "--expect":
            guard let parsed = ProbeExpectation(rawValue: try takeValue()) else {
                throw HostError.usage
            }
            expectation = parsed
        case "--timeout-ms":
            guard let parsed = Int(try takeValue()), parsed > 0 else {
                throw HostError.usage
            }
            timeoutMs = parsed
        case "--dev-origin":
            let value = try takeValue()
            guard value == "http://127.0.0.1:5173",
                  let parsed = URL(string: value) else {
                throw HostError.usage
            }
            devOrigin = parsed
        default:
            throw HostError.usage
        }
        index += 1
    }
    guard let assets else { throw HostError.usage }
    return HostOptions(
        assets: assets,
        expectation: expectation,
        timeoutMs: timeoutMs,
        config: config,
        devOrigin: devOrigin)
}

private func isProbe(_ expectation: ProbeExpectation) -> Bool {
    switch expectation {
    case .pass, .fail, .timeout, .audio, .media: return true
    default: return false
    }
}

private struct TestRuntimeHandle: @unchecked Sendable {
    let pointer: OpaquePointer
}

@MainActor
private final class NativeTestHost: NSObject, WKNavigationDelegate, WKUIDelegate {
    private let options: HostOptions
    private var probeReceiver: ChaProbeReceiver?
    private var nativeBridge: ChaNativeBridgeReceiver?
    private var runtime: OpaquePointer?
    private var webView: WKWebView!
    private var window: NSWindow?
    private var finished = false
    private var flowPhase = 0
    private var initialFlowStarted = false
    private var rendererFailures = 0
    private var lastFlowConnection: String?

    init(options: HostOptions) {
        self.options = options
        super.init()
        if isProbe(options.expectation) && options.devOrigin == nil {
            let built = makeFeasibilityWebViewConfiguration(assetRoot: options.assets)
            probeReceiver = built.2
            let view = WKWebView(
                frame: NSRect(x: 0, y: 0, width: 800, height: 600),
                configuration: built.0)
            view.navigationDelegate = self
            view.uiDelegate = self
            built.2.attach(to: view)
            webView = view
            return
        }
        guard let config = options.config else {
            FileHandle.standardError.write(Data("FAIL missing --config for native flow\n".utf8))
            exit(2)
        }
        var error: UnsafeMutablePointer<CChar>?
        var passwordError: Int32 = 0
        let created = config.path.withCString { configPath in
            options.assets.path.withCString { resourcePath in
                "".withCString { token in
                    "".withCString { password in
                        cha_runtime_create(
                            configPath, resourcePath, token, password, 0,
                            &passwordError, &error)
                    }
                }
            }
        }
        guard let created else {
            let message = error.map { String(cString: $0) } ?? "runtime create failed"
            cha_string_free(error)
            FileHandle.standardError.write(Data("FAIL \(message)\n".utf8))
            exit(2)
        }
        runtime = created
        FileHandle.standardError.write(
            Data("runtime_listener=\(cha_runtime_port(created) == 0 ? "none" : "http")\n".utf8))
        if cha_runtime_port(created) != 0 {
            FileHandle.standardError.write(Data("FAIL native runtime opened a listener\n".utf8))
            exit(1)
        }
        let built = makeNativeWebViewConfiguration(assetRoot: options.assets)
        let view = WKWebView(
            frame: NSRect(x: 0, y: 0, width: 800, height: 600),
            configuration: built.0)
        view.navigationDelegate = self
        view.uiDelegate = self
        let receiver = ChaNativeBridgeReceiver(runtime: created)
        if let origin = options.devOrigin {
            receiver.isTrustedOrigin = { securityOrigin in
                isChaAssetOrigin(securityOrigin)
                    || (securityOrigin.protocol.caseInsensitiveCompare("http") == .orderedSame
                        && securityOrigin.host == "127.0.0.1"
                        && securityOrigin.port == (origin.port ?? 5173))
            }
        }
        receiver.attach(to: view, mediaHandler: built.1)
        nativeBridge = receiver
        webView = view
    }

    func start() {
        if options.devOrigin != nil || options.expectation == .audio || options.expectation == .media {
            let window = NSWindow(
                contentRect: NSRect(x: 0, y: 0, width: 1040, height: 760),
                styleMask: [.titled, .closable, .miniaturizable, .resizable],
                backing: .buffered,
                defer: false)
            window.title = "CHA"
            window.contentView = webView
            window.center()
            window.makeKeyAndOrderFront(nil)
            NSApp.setActivationPolicy(.regular)
            NSApp.activate(ignoringOtherApps: true)
            self.window = window
        }
        if let origin = options.devOrigin {
            FileHandle.standardError.write(
                Data("loader=dev-origin \(origin.absoluteString)\n".utf8))
            webView.load(URLRequest(url: origin))
            return
        }
        FileHandle.standardError.write(Data("loader=WKURLSchemeHandler cha://app\n".utf8))
        webView.load(URLRequest(url: URL(string: "\(chaAssetOrigin)/")!))
        DispatchQueue.main.asyncAfter(deadline: .now() + .milliseconds(options.timeoutMs)) {
            [weak self] in
            self?.finish(error: HostError.timedOut)
        }
    }

    func webView(
        _ webView: WKWebView,
        didFinish navigation: WKNavigation!
    ) {
        if options.devOrigin != nil { return }
        switch options.expectation {
        case .timeout:
            return
        case .fail:
            evaluate("return {ok:false, reason:'intentional assertion failure'}")
        case .pass:
            evaluate(passProbeSource)
        case .audio:
            evaluate(audioProbeSource)
        case .media:
            evaluate(audioProbeSource.replacingOccurrences(
                of: "const requireCapture = true;", with: "const requireCapture = false;"))
        case .parity:
            do {
                guard let path = ProcessInfo.processInfo.environment["CHA_NATIVE_PARITY_SCRIPT"] else {
                    throw HostError.usage
                }
                let script = try String(contentsOfFile: path, encoding: .utf8)
                evaluate(script + "\nreturn await nativeParity();")
            } catch {
                finish(error: error)
            }
        case .flow, .reload, .rendererFail, .stall, .quit:
            if flowPhase == 0 {
                guard !initialFlowStarted else { return }
                initialFlowStarted = true
            }
            runFlow()
        }
    }

    func webViewWebContentProcessDidTerminate(_ webView: WKWebView) {
        rendererFailures += 1
        nativeBridge?.prepareDocumentReplacement()
        if rendererFailures >= 3 {
            finish(error: HostError.probeFailed("renderer failed repeatedly"))
            return
        }
        webView.reload()
    }

    func webView(
        _ webView: WKWebView,
        didFail navigation: WKNavigation!,
        withError error: Error
    ) {
        if options.expectation == .rendererFail && flowPhase == 1 { return }
        finish(error: HostError.probeFailed(error.localizedDescription))
    }

    func webView(
        _ webView: WKWebView,
        didFailProvisionalNavigation navigation: WKNavigation!,
        withError error: Error
    ) {
        if options.expectation == .rendererFail && flowPhase == 1 { return }
        finish(error: HostError.probeFailed(error.localizedDescription))
    }

    func webView(
        _ webView: WKWebView,
        decidePolicyFor navigationAction: WKNavigationAction,
        decisionHandler: @escaping (WKNavigationActionPolicy) -> Void
    ) {
        guard let url = navigationAction.request.url, isAllowedNavigation(url) else {
            decisionHandler(.cancel)
            return
        }
        if let frame = navigationAction.targetFrame, !frame.isMainFrame, !isAllowedNavigation(url) {
            decisionHandler(.cancel)
            return
        }
        nativeBridge?.willNavigate(navigationAction, in: webView)
        decisionHandler(.allow)
    }

    func webView(
        _ webView: WKWebView,
        createWebViewWith configuration: WKWebViewConfiguration,
        for navigationAction: WKNavigationAction,
        windowFeatures: WKWindowFeatures
    ) -> WKWebView? {
        return nil
    }

    func webView(
        _ webView: WKWebView,
        requestMediaCapturePermissionFor origin: WKSecurityOrigin,
        initiatedByFrame frame: WKFrameInfo,
        type: WKMediaCaptureType,
        decisionHandler: @escaping (WKPermissionDecision) -> Void
    ) {
        if options.expectation != .media,
           type == .microphone, frame.isMainFrame, trustedMediaOrigin(origin) {
            decisionHandler(.grant)
        } else {
            decisionHandler(.deny)
        }
    }

    private func trustedMediaOrigin(_ origin: WKSecurityOrigin) -> Bool {
        if isChaAssetOrigin(origin) { return true }
        guard let allowed = options.devOrigin else { return false }
        return origin.protocol.caseInsensitiveCompare("http") == .orderedSame
            && origin.host == "127.0.0.1"
            && origin.port == (allowed.port ?? 5173)
    }

    private func isAllowedNavigation(_ url: URL) -> Bool {
        if isChaAssetURL(url) { return true }
        guard let origin = options.devOrigin else { return false }
        return url.scheme?.caseInsensitiveCompare("http") == .orderedSame
            && url.host == "127.0.0.1"
            && url.port == (origin.port ?? 5173)
    }

    private func evaluate(_ source: String) {
        webView.callAsyncJavaScript(
            source,
            arguments: [:],
            in: nil,
            in: .page
        ) { [weak self] outcome in
            switch outcome {
            case .failure(let error):
                let nsError = error as NSError
                self?.finish(error: HostError.probeFailed(
                    "\(error.localizedDescription) \(nsError.userInfo)"))
            case .success(let result):
                self?.handleProbe(result)
            }
        }
    }

    private func runFlow() {
        evaluate(flowScript())
    }

    private func flowScript() -> String {
        if flowPhase > 0 { return restoreFlowSource }
        if options.expectation == .stall { return stallFlowSource }
        return firstFlowSource
    }

    private func handleProbe(_ result: Any?) {
        guard let report = result as? [String: Any] else {
            finish(error: HostError.probeFailed("probe returned no report"))
            return
        }
        if let json = try? JSONSerialization.data(withJSONObject: report),
           let text = String(data: json, encoding: .utf8) {
            FileHandle.standardOutput.write(Data((text + "\n").utf8))
        }
        if let ok = report["ok"] as? Bool, !ok {
            let reason = report["reason"] as? String ?? "probe failed"
            finish(error: HostError.probeFailed(reason))
            return
        }
        guard report["ok"] as? Bool == true else {
            finish(error: HostError.probeFailed("probe returned no report"))
            return
        }
        if options.expectation == .reload {
            guard nativeBridge?.connectionId != lastFlowConnection else {
                finish(error: HostError.probeFailed("document replacement reused its connection"))
                return
            }
            lastFlowConnection = nativeBridge?.connectionId
        }
        switch options.expectation {
        case .reload where flowPhase == 0:
            flowPhase = 1
            webView.reload()
        case .reload where flowPhase == 1:
            flowPhase = 2
            webView.load(URLRequest(url: URL(string: "\(chaAssetOrigin)/")!))
        case .rendererFail where flowPhase == 0:
            flowPhase = 1
            terminateOwnWebContent()
            DispatchQueue.main.asyncAfter(deadline: .now() + .seconds(2)) { [weak self] in
                guard let self, !self.finished else { return }
                self.nativeBridge?.prepareDocumentReplacement()
                self.webView.reload()
            }
        case .quit:
            shutdownAndExit(success: true)
        default:
            finish(error: nil)
        }
    }

    private func terminateOwnWebContent() {
        let pipe = Pipe()
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/bin/ps")
        process.arguments = ["-axo", "pid=,ppid=,pgid=,comm="]
        process.standardOutput = pipe
        do {
            try process.run()
        } catch {
            FileHandle.standardError.write(Data("ps failed; reloading instead\n".utf8))
            return
        }
        let text = String(data: pipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        process.waitUntilExit()
        let selfPid = Int(getpid())
        let selfGroup = Int(getpgrp())
        var parent: [Int: Int] = [:]
        var groups: [Int: Int] = [:]
        var contentPids: [Int] = []
        for line in text.split(separator: "\n") {
            let parts = line.split(whereSeparator: { $0.isWhitespace }).map(String.init)
            guard parts.count >= 4,
                  let pid = Int(parts[0]),
                  let ppid = Int(parts[1]),
                  let pgid = Int(parts[2]) else {
                continue
            }
            parent[pid] = ppid
            groups[pid] = pgid
            if parts[3].localizedCaseInsensitiveContains("WebContent") {
                contentPids.append(pid)
            }
        }
        func owned(_ pid: Int) -> Bool {
            if groups[pid] == selfGroup { return true }
            var current = parent[pid]
            var hops = 0
            while let next = current, hops < 8 {
                if next == selfPid { return true }
                current = parent[next]
                hops += 1
            }
            return false
        }
        let targets = contentPids.filter(owned)
        FileHandle.standardError.write(
            Data("web content candidates \(contentPids) targets \(targets)\n".utf8))
        for pid in targets { kill(pid_t(pid), SIGKILL) }
    }

    private func shutdownAndExit(success: Bool) {
        nativeBridge?.detach()
        nativeBridge = nil
        if let runtime {
            cha_runtime_request_shutdown(runtime)
            let handle = TestRuntimeHandle(pointer: runtime)
            DispatchQueue.global(qos: .userInitiated).async {
                _ = cha_runtime_join_shutdown(handle.pointer, 10000)
                DispatchQueue.main.async {
                    cha_runtime_destroy(handle.pointer)
                    self.runtime = nil
                    self.finish(error: success ? nil : HostError.probeFailed("quit failed"))
                }
            }
            return
        }
        finish(error: success ? nil : HostError.probeFailed("quit failed"))
    }

    private func finish(error: Error?) {
        guard !finished else { return }
        finished = true
        nativeBridge?.detach()
        nativeBridge = nil
        if let runtime {
            cha_runtime_request_shutdown(runtime)
            _ = cha_runtime_join_shutdown(runtime, 2000)
            cha_runtime_destroy(runtime)
            self.runtime = nil
        }
        if let error {
            FileHandle.standardError.write(Data(("FAIL \(error.localizedDescription)\n").utf8))
            exit(1)
        }
        FileHandle.standardError.write(Data("PASS\n".utf8))
        exit(0)
    }
}

private let firstFlowSource = """
return await (async function() {
  const report = {ok: false, origin: location.origin};
  const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const until = async (label, check) => {
    const started = Date.now();
    while (Date.now() - started < 15000) {
      const value = check();
      if (value) return value;
      await wait(50);
    }
    throw new Error('timeout waiting for ' + label);
  };
  const setReactValue = (node, text) => {
    const proto = node.tagName === 'TEXTAREA'
      ? window.HTMLTextAreaElement.prototype
      : window.HTMLInputElement.prototype;
    const setter = Object.getOwnPropertyDescriptor(proto, 'value').set;
    setter.call(node, text);
    node.dispatchEvent(new Event('input', {bubbles: true}));
  };
  try {
    if (!window.__CHA_NATIVE_POST__ || !window.__CHA_NATIVE_CONNECTION_ID__) {
      report.reason = 'native bridge hooks missing';
      return report;
    }
    const input = await until('composer', () => document.querySelector('textarea[aria-label="Message"]'));
    const rpc = (() => {
      let next = 9000000;
      return function(method, params, epoch) {
        const id = next++;
        return new Promise((resolve, reject) => {
          const previous = window.__CHA_NATIVE_RECEIVE__;
          const timer = setTimeout(() => reject(new Error(method + ' timed out')), 8000);
          window.__CHA_NATIVE_RECEIVE__ = function(batch) {
            if (typeof previous === 'function') previous(batch);
            for (const message of (batch && batch.messages) || []) {
              if (message && message.id === id) {
                clearTimeout(timer);
                window.__CHA_NATIVE_RECEIVE__ = previous;
                if (message.ok) resolve(message.result);
                else reject(new Error((message.error && message.error.message) || method));
              }
            }
          };
          window.__CHA_NATIVE_POST__(JSON.stringify({
            connection_id: window.__CHA_NATIVE_CONNECTION_ID__,
            id,
            context_epoch: epoch || 0,
            method,
            params: params || {}
          }));
        });
      };
    })();
    await until('composer ready', () => {
      const node = document.querySelector('textarea[aria-label="Message"]');
      return node && !node.disabled ? node : null;
    });
    setReactValue(document.querySelector('textarea[aria-label="Message"]'), 'Hello from native host');
    await wait(50);
    const send = document.querySelector('button[aria-label="Send message"]');
    if (!send || send.disabled) {
      report.reason = 'send control was not ready';
      return report;
    }
    send.click();
    await until('generation or transcript', () => {
      return document.querySelector('button[aria-label="Stop generation"]')
        || Array.from(document.querySelectorAll('[aria-label="Conversation transcript"] *'))
          .some((node) => (node.textContent || '').indexOf('Hello from native host') !== -1);
    });
    const stop = document.querySelector('button[aria-label="Stop generation"]');
    if (stop) stop.click();
    await until('visible transcript', () => {
      const text = (document.querySelector('[aria-label="Conversation transcript"]') || {}).textContent || '';
      return text.indexOf('Hello from native host') !== -1;
    });
    report.transcript = (document.querySelector('[aria-label="Conversation transcript"]') || {}).textContent || '';
    report.sessionId = location.hash;
    // Direct probe RPCs use a separate high request-id range. Issue them only
    // after the UI flow is complete so the frontend's own monotonic request
    // sequence never has to follow an instrumented request on this document.
    const boot = await rpc('app.bootstrap', {}, 0);
    const providers = await rpc('provider.list', {}, boot.context_epoch);
    const keys = await rpc('apiKey.list', {}, boot.context_epoch);
    const r2 = await rpc('r2Storage.get', {}, boot.context_epoch);
    const auth = await rpc('openaiAuth.get', {}, boot.context_epoch);
    const runtime = await rpc('voiceInput.runtime', {}, boot.context_epoch);
    if (!Array.isArray(providers) || JSON.stringify(providers).indexOf('private-') !== -1) {
      report.reason = 'provider list missing or contained a secret';
      return report;
    }
    if (!Array.isArray(keys) || JSON.stringify(keys).indexOf('private-') !== -1) {
      report.reason = 'api key list contained a secret';
      return report;
    }
    report.providerCount = providers.length;
    report.r2 = r2;
    report.authStatus = auth && auth.status;
    report.voiceRuntimeHasKey = !!(runtime && runtime.api_key);
    report.ok = true;
    return report;
  } catch (error) {
    report.reason = String(error && error.message ? error.message : error);
    return report;
  }
})()
"""

private let restoreFlowSource = """
return await (async function() {
  const report = {ok: false, origin: location.origin, restored: true};
  const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const until = async (label, check) => {
    const started = Date.now();
    while (Date.now() - started < 15000) {
      const value = check();
      if (value) return value;
      await wait(50);
    }
    throw new Error('timeout waiting for ' + label);
  };
  try {
    if (!window.__CHA_NATIVE_POST__ || !window.__CHA_NATIVE_CONNECTION_ID__) {
      report.reason = 'native bridge hooks missing after reload';
      return report;
    }
    await until('composer', () => {
      const node = document.querySelector('textarea[aria-label="Message"]');
      return node && !node.disabled ? node : null;
    });
    report.connectionId = window.__CHA_NATIVE_CONNECTION_ID__;
    report.transcript = (document.querySelector('[aria-label="Conversation transcript"]') || {}).textContent || '';
    report.ok = true;
    return report;
  } catch (error) {
    report.reason = String(error && error.message ? error.message : error);
    return report;
  }
})()
"""

private let stallFlowSource: String = {
    let prefix = """
  const originalPost = window.__CHA_NATIVE_POST__;
  window.__CHA_HELD_ACKS__ = [];
  window.__CHA_NATIVE_POST__ = function(message) {
    try {
      const parsed = JSON.parse(message);
      if (parsed && parsed.delivery_id != null && parsed.method == null) {
        window.__CHA_HELD_ACKS__.push(message);
        if (window.__CHA_HELD_ACKS__.length > 4) {
          const held = window.__CHA_HELD_ACKS__.splice(0);
          held.forEach((ack) => originalPost(ack));
        }
        return;
      }
    } catch (error) {}
    originalPost(message);
  };
"""
    return firstFlowSource.replacingOccurrences(
        of: "    setReactValue(document.querySelector('textarea[aria-label=\"Message\"]'), 'Hello from native host');",
        with: prefix + "    setReactValue(document.querySelector('textarea[aria-label=\"Message\"]'), 'Hello from native host');")
}()

private let passProbeSource = """
return await (async function() {
  try {
  const report = {
    ok: false,
    origin: location.origin,
    protocol: location.protocol,
    secureContext: window.isSecureContext,
    moduleScripts: Array.from(document.scripts).some((script) => script.type === 'module')
  };
  if (location.protocol !== 'cha:') {
    report.reason = 'expected cha: origin';
    return report;
  }
  if (!window.isSecureContext) {
    report.reason = 'document is not a secure context';
    return report;
  }
  const missing = await fetch('/assets/__cha_missing__' + Date.now() + '.js', {cache: 'no-store'});
  report.missingAssetStatus = missing.status;
  const missingBody = await missing.text();
  report.missingAssetIsShell = missingBody.indexOf('id="root"') !== -1;
  if (missing.status === 200 || report.missingAssetIsShell) {
    report.reason = 'missing asset fell back to the shell';
    return report;
  }
  sessionStorage.setItem('cha.probe', 'ok');
  report.storage = sessionStorage.getItem('cha.probe');
  if (report.storage !== 'ok') {
    report.reason = 'sessionStorage is unavailable';
    return report;
  }
  history.pushState(null, '', '#/s/lobby/planning/');
  report.hashSession = location.hash;
  history.pushState(null, '', '#/');
  report.hashRoot = location.hash;
  if (report.hashSession !== '#/s/lobby/planning/' || report.hashRoot !== '#/') {
    report.reason = 'fragment navigation failed';
    return report;
  }
  try {
    (0, eval)('1+1');
    report.evalAllowed = true;
  } catch (error) {
    report.evalAllowed = false;
  }
  if (report.evalAllowed) {
    report.reason = 'eval was not blocked';
    return report;
  }
  try {
    await fetch('https://example.com/', {mode: 'cors', cache: 'no-store'});
    report.remoteFetch = 'completed';
  } catch (error) {
    report.remoteFetch = 'blocked';
  }
  if (report.remoteFetch !== 'blocked') {
    report.reason = 'remote fetch was not blocked';
    return report;
  }
  const echo = '<script>alert(1)</script>';
  const reply = await window.__chaProbeSend({echo: echo});
  report.roundTrip = reply && reply.echo === echo && reply.trusted === true;
  if (!report.roundTrip) {
    report.reason = 'native round trip failed';
    return report;
  }
  try {
    report.popup = window.open('https://example.com/') ? 'created' : 'denied';
  } catch (error) {
    report.popup = 'threw';
  }
  if (report.popup === 'created') {
    report.reason = 'popup was not denied';
    return report;
  }
  try { location.href = 'https://example.com/'; } catch (error) {}
  report.stayedOnOrigin = location.href.indexOf('cha://app') === 0;
  if (!report.stayedOnOrigin) {
    report.reason = 'remote navigation was not denied';
    return report;
  }
  report.ok = true;
  return report;
  } catch (error) {
    return {ok: false, reason: String(error && error.message ? error.message : error)};
  }
})()
"""

private let audioProbeSource = """
return await (async function() {
  try {
  const requireCapture = true;
  const report = {ok: false, origin: location.origin};
  const audio = await fetch('/media/probe-audio', {cache: 'no-store'});
  if (!audio.ok) {
    report.reason = 'probe audio fetch failed';
    return report;
  }
  const buffer = await audio.arrayBuffer();
  report.audioBytes = buffer.byteLength;
  const blob = new Blob([buffer], {type: 'audio/wav'});
  const objectUrl = URL.createObjectURL(blob);
  const element = new Audio();
  element.preload = 'auto';
  element.src = objectUrl;
  try {
    await new Promise((resolve, reject) => {
      element.onloadedmetadata = resolve;
      element.onerror = () => reject(new Error('audio element failed'));
      setTimeout(() => reject(new Error('audio metadata timeout')), 4000);
    });
  } catch (error) {
    URL.revokeObjectURL(objectUrl);
    element.removeAttribute('src');
    element.load();
    report.reason = 'blob object-URL playback failed: ' + String(error && error.message ? error.message : error);
    return report;
  }
  report.audioDuration = element.duration;
  if (!(report.audioDuration > 0)) {
    URL.revokeObjectURL(objectUrl);
    report.reason = 'blob object-URL playback produced no duration';
    return report;
  }
  const seekTo = Math.min(0.02, report.audioDuration);
  element.currentTime = seekTo;
  report.audioSeek = element.currentTime;
  if (!(report.audioSeek > 0) && seekTo > 0) {
    URL.revokeObjectURL(objectUrl);
    report.reason = 'blob object-URL seek failed';
    return report;
  }
  try {
    await element.play();
    report.audioPlay = 'ok';
  } catch (error) {
    URL.revokeObjectURL(objectUrl);
    report.reason = 'blob object-URL play failed: ' + String(error && error.name ? error.name : error);
    return report;
  }
  element.pause();
  URL.revokeObjectURL(objectUrl);
  element.removeAttribute('src');
  element.load();
  report.audioReleased = true;
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
    report.reason = 'mediaDevices/getUserMedia missing';
    return report;
  }
  try {
    const stream = await Promise.race([
      navigator.mediaDevices.getUserMedia({audio: true}),
      new Promise((_, reject) => setTimeout(() => reject(new Error('mic-timeout')), requireCapture ? 30000 : 4000)),
    ]);
    const tracks = stream.getAudioTracks();
    report.microphone = tracks.length > 0 && tracks[0].readyState === 'live' ? 'captured' : 'silent';
    tracks.forEach((track) => track.stop());
  } catch (error) {
    if (!requireCapture && error && error.name === 'NotAllowedError') {
      report.microphone = 'permission-denied';
      report.ok = true;
      return report;
    }
    report.microphone = 'denied-or-unavailable';
    report.reason = 'microphone capture failed: ' + String(error && error.message ? error.message : error);
    return report;
  }
  if (report.microphone !== 'captured') {
    report.reason = 'microphone was not captured';
    return report;
  }
  report.ok = true;
  return report;
  } catch (error) {
    return {ok: false, reason: String(error && error.message ? error.message : error)};
  }
})()
"""

@main
private struct Main {
    @MainActor
    static func main() {
        do {
            let options = try parseOptions()
            let application = NSApplication.shared
            application.setActivationPolicy(
                options.devOrigin == nil ? .accessory : .regular)
            let host = NativeTestHost(options: options)
            host.start()
            application.run()
        } catch {
            FileHandle.standardError.write(Data(("FAIL \(error.localizedDescription)\n").utf8))
            exit(2)
        }
    }
}
