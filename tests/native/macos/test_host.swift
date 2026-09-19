import AppKit
import Foundation
import WebKit

private enum ProbeExpectation: String {
    case pass
    case fail
    case timeout
    case audio
}

private struct HostOptions {
    var assets: URL
    var expectation = ProbeExpectation.pass
    var timeoutMs = 20000
}

private enum HostError: LocalizedError {
    case usage
    case probeFailed(String)
    case timedOut

    var errorDescription: String? {
        switch self {
        case .usage:
            return "usage: cha_macos_native_test_host --assets <dir> [--expect pass|fail|timeout|audio] [--timeout-ms N]"
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
    var timeoutMs = 20000
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
        default:
            throw HostError.usage
        }
        index += 1
    }
    guard let assets else { throw HostError.usage }
    return HostOptions(assets: assets, expectation: expectation, timeoutMs: timeoutMs)
}

@MainActor
private final class NativeTestHost: NSObject, WKNavigationDelegate, WKUIDelegate {
    private let options: HostOptions
    private let receiver: ChaProbeReceiver
    private var webView: WKWebView!
    private var finished = false

    init(options: HostOptions) {
        self.options = options
        let configuration: WKWebViewConfiguration
        let built = makeFeasibilityWebViewConfiguration(assetRoot: options.assets)
        configuration = built.0
        receiver = built.2
        super.init()
        let view = WKWebView(frame: NSRect(x: 0, y: 0, width: 800, height: 600),
                             configuration: configuration)
        view.navigationDelegate = self
        view.uiDelegate = self
        receiver.attach(to: view)
        webView = view
    }

    func start() {
        FileHandle.standardError.write(Data("runtime_listener=none\n".utf8))
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
        switch options.expectation {
        case .timeout:
            return
        case .fail:
            evaluate("return {ok:false, reason:'intentional assertion failure'}")
        case .pass:
            evaluate(passProbeSource)
        case .audio:
            evaluate(audioProbeSource)
        }
    }

    func webView(
        _ webView: WKWebView,
        didFail navigation: WKNavigation!,
        withError error: Error
    ) {
        finish(error: HostError.probeFailed(error.localizedDescription))
    }

    func webView(
        _ webView: WKWebView,
        didFailProvisionalNavigation navigation: WKNavigation!,
        withError error: Error
    ) {
        finish(error: HostError.probeFailed(error.localizedDescription))
    }

    func webView(
        _ webView: WKWebView,
        decidePolicyFor navigationAction: WKNavigationAction,
        decisionHandler: @escaping (WKNavigationActionPolicy) -> Void
    ) {
        guard let url = navigationAction.request.url, isChaAssetURL(url) else {
            decisionHandler(.cancel)
            return
        }
        if let frame = navigationAction.targetFrame, !frame.isMainFrame, !isChaAssetURL(url) {
            decisionHandler(.cancel)
            return
        }
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
        if type == .microphone, isChaAssetOrigin(origin), frame.isMainFrame {
            decisionHandler(.grant)
        } else {
            decisionHandler(.deny)
        }
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

    private func handleProbe(_ result: Any?) {
        guard let report = result as? [String: Any] else {
            finish(error: HostError.probeFailed("probe returned no report"))
            return
        }
        if let json = try? JSONSerialization.data(withJSONObject: report),
           let text = String(data: json, encoding: .utf8) {
            FileHandle.standardOutput.write(Data((text + "\n").utf8))
        }
        if let ok = report["ok"] as? Bool, ok {
            finish(error: nil)
            return
        }
        let reason = report["reason"] as? String ?? "probe failed"
        finish(error: HostError.probeFailed(reason))
    }

    private func finish(error: Error?) {
        guard !finished else { return }
        finished = true
        if let error {
            FileHandle.standardError.write(Data(("FAIL \(error.localizedDescription)\n").utf8))
            exit(1)
        }
        FileHandle.standardError.write(Data("PASS\n".utf8))
        exit(0)
    }
}

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
  const report = {ok: false, origin: location.origin};
  const audio = await fetch('/probe/audio', {cache: 'no-store'});
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
      new Promise((_, reject) => setTimeout(() => reject(new Error('mic-timeout')), 2500)),
    ]);
    const tracks = stream.getAudioTracks();
    report.microphone = tracks.length > 0 && tracks[0].readyState === 'live' ? 'captured' : 'silent';
    tracks.forEach((track) => track.stop());
  } catch (error) {
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
            application.setActivationPolicy(.accessory)
            let host = NativeTestHost(options: options)
            host.start()
            application.run()
        } catch {
            FileHandle.standardError.write(Data(("FAIL \(error.localizedDescription)\n").utf8))
            exit(2)
        }
    }
}
