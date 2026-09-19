import Foundation
import WebKit

let chaAssetScheme = "cha"
let chaAssetHost = "app"
let chaAssetOrigin = "cha://app"

// Keep this aligned with src/web/asset_handler.cpp. Native loaders must apply
// CSP before page scripts run; copying index.html does not keep the HTTP header.
let chaNativeContentSecurityPolicy =
    "default-src 'none'; script-src 'self'; style-src 'self'; "
    + "img-src 'self' data:; font-src 'self'; media-src 'self' blob:; connect-src 'self'; "
    + "base-uri 'none'; form-action 'none'; frame-ancestors 'none'"

func chaAssetRootURL(_ root: URL) -> URL {
    root.standardizedFileURL
}

func isChaAssetURL(_ url: URL) -> Bool {
    url.scheme?.caseInsensitiveCompare(chaAssetScheme) == .orderedSame
        && url.host?.caseInsensitiveCompare(chaAssetHost) == .orderedSame
}

func isChaAssetOrigin(_ origin: WKSecurityOrigin) -> Bool {
    origin.protocol.caseInsensitiveCompare(chaAssetScheme) == .orderedSame
        && origin.host.caseInsensitiveCompare(chaAssetHost) == .orderedSame
}

private func mimeType(for path: String) -> String {
    switch URL(fileURLWithPath: path).pathExtension.lowercased() {
    case "html": return "text/html; charset=utf-8"
    case "css": return "text/css; charset=utf-8"
    case "js": return "text/javascript; charset=utf-8"
    case "svg": return "image/svg+xml"
    case "png": return "image/png"
    case "woff2": return "font/woff2"
    case "json", "map": return "application/json"
    case "wav": return "audio/wav"
    default: return "application/octet-stream"
    }
}

func chaProbeWavData() -> Data {
    let sampleRate: UInt32 = 8000
    let samples: UInt32 = 800
    var data = Data()
    func appendASCII(_ value: String) {
        data.append(contentsOf: value.utf8)
    }
    func appendU32(_ value: UInt32) {
        var little = value.littleEndian
        withUnsafeBytes(of: &little) { data.append(contentsOf: $0) }
    }
    func appendU16(_ value: UInt16) {
        var little = value.littleEndian
        withUnsafeBytes(of: &little) { data.append(contentsOf: $0) }
    }
    appendASCII("RIFF")
    appendU32(36 + samples * 2)
    appendASCII("WAVE")
    appendASCII("fmt ")
    appendU32(16)
    appendU16(1)
    appendU16(1)
    appendU32(sampleRate)
    appendU32(sampleRate * 2)
    appendU16(2)
    appendU16(16)
    appendASCII("data")
    appendU32(samples * 2)
    for index in 0..<samples {
        let sample = Int16(sin(Double(index) * 0.4) * 8000)
        var little = UInt16(bitPattern: sample).littleEndian
        withUnsafeBytes(of: &little) { data.append(contentsOf: $0) }
    }
    return data
}

final class ChaAssetSchemeHandler: NSObject, WKURLSchemeHandler {
    private let root: URL
    private let lock = NSLock()
    private var stopped = Set<ObjectIdentifier>()
    var readMedia: ((String) -> (type: String, body: Data)?)?

    init(root: URL) {
        self.root = chaAssetRootURL(root)
        super.init()
    }

    func webView(_ webView: WKWebView, start urlSchemeTask: WKURLSchemeTask) {
        let identity = ObjectIdentifier(urlSchemeTask)
        guard let requestURL = urlSchemeTask.request.url, isChaAssetURL(requestURL) else {
            urlSchemeTask.didFailWithError(NSError(
                domain: "cha.feasibility", code: 400,
                userInfo: [NSLocalizedDescriptionKey: "Unknown asset origin"]))
            return
        }

        let path = requestURL.path.isEmpty ? "/" : requestURL.path
        if path.hasPrefix("/media/") {
            let id = String(path.dropFirst("/media/".count))
            if id.contains("/") || id.contains("\\") || id.contains("..") {
                finish(task: urlSchemeTask, identity: identity, url: requestURL,
                       status: 404, type: "text/plain; charset=utf-8", cache: "no-store",
                       body: Data("not found".utf8), csp: false)
                return
            }
            if let readMedia, let media = readMedia(id) {
                finish(task: urlSchemeTask, identity: identity, url: requestURL,
                       status: 200, type: media.type, cache: "no-store",
                       body: media.body, csp: false)
            } else {
                finish(task: urlSchemeTask, identity: identity, url: requestURL,
                       status: 404, type: "text/plain; charset=utf-8", cache: "no-store",
                       body: Data("not found".utf8), csp: false)
            }
            return
        }
        if path == "/probe/audio" {
            finish(task: urlSchemeTask, identity: identity, url: requestURL,
                   status: 200, type: "audio/wav", cache: "no-store",
                   body: chaProbeWavData(), csp: false)
            return
        }

        let isShell = path == "/" || path == "/index.html"
        if !isShell, path.contains("..") {
            finish(task: urlSchemeTask, identity: identity, url: requestURL,
                   status: 404, type: "text/plain; charset=utf-8", cache: "no-store",
                   body: Data("not found".utf8), csp: false)
            return
        }

        let relative = isShell ? "index.html" : String(path.drop(while: { $0 == "/" }))
        let candidate = root.appendingPathComponent(relative).resolvingSymlinksInPath()
        let rooted = root.resolvingSymlinksInPath().path
        let allowed = candidate.path == rooted
            || candidate.path.hasPrefix(rooted.hasSuffix("/") ? rooted : rooted + "/")
        guard allowed, FileManager.default.isReadableFile(atPath: candidate.path) else {
            finish(task: urlSchemeTask, identity: identity, url: requestURL,
                   status: 404, type: "text/plain; charset=utf-8", cache: "no-store",
                   body: Data("not found".utf8), csp: false)
            return
        }

        guard let body = try? Data(contentsOf: candidate) else {
            urlSchemeTask.didFailWithError(NSError(
                domain: "cha.feasibility", code: 500,
                userInfo: [NSLocalizedDescriptionKey: "Failed to read asset"]))
            return
        }
        finish(
            task: urlSchemeTask,
            identity: identity,
            url: requestURL,
            status: 200,
            type: mimeType(for: candidate.path),
            cache: isShell ? "no-cache" : "public, max-age=31536000, immutable",
            body: body,
            csp: isShell)
    }

    func webView(_ webView: WKWebView, stop urlSchemeTask: WKURLSchemeTask) {
        lock.lock()
        stopped.insert(ObjectIdentifier(urlSchemeTask))
        lock.unlock()
    }

    private func finish(
        task: WKURLSchemeTask,
        identity: ObjectIdentifier,
        url: URL,
        status: Int,
        type: String,
        cache: String,
        body: Data,
        csp: Bool
    ) {
        lock.lock()
        let cancelled = stopped.contains(identity)
        lock.unlock()
        if cancelled { return }

        var headers = [
            "Content-Type": type,
            "Content-Length": String(body.count),
            "Cache-Control": cache,
        ]
        if csp {
            headers["Content-Security-Policy"] = chaNativeContentSecurityPolicy
        }
        guard let response = HTTPURLResponse(
            url: url, statusCode: status, httpVersion: "HTTP/1.1",
            headerFields: headers) else {
            task.didFailWithError(NSError(
                domain: "cha.feasibility", code: 500,
                userInfo: [NSLocalizedDescriptionKey: "Failed to build asset response"]))
            return
        }
        task.didReceive(response)
        task.didReceive(body)
        task.didFinish()
    }
}

final class ChaProbeReceiver: NSObject, WKScriptMessageHandler {
    private weak var webView: WKWebView?

    func attach(to webView: WKWebView) {
        self.webView = webView
        webView.configuration.userContentController.add(self, name: "chaProbe")
    }

    func userContentController(
        _ userContentController: WKUserContentController,
        didReceive message: WKScriptMessage
    ) {
        guard message.name == "chaProbe",
              message.frameInfo.isMainFrame,
              isChaAssetOrigin(message.frameInfo.securityOrigin) else {
            return
        }
        guard let body = message.body as? [String: Any],
              let id = body["id"] as? String else {
            return
        }
        let configured = ProcessInfo.processInfo.environment["CHA_VOICE_PROBE_URL"] != nil
            && ProcessInfo.processInfo.environment["CHA_VOICE_PROBE_API_KEY"] != nil
        if let sdp = body["sdp"] as? String {
            Task { [weak self] in
                let answer = await Self.voiceAnswer(sdp: sdp)
                self?.reply(id: id, body: [
                    "id": id,
                    "trusted": true,
                    "voiceConfigured": configured,
                    "answer": answer as Any,
                    "missingCredentials": !configured,
                ])
            }
            return
        }
        reply(id: id, body: [
            "id": id,
            "echo": body["echo"] ?? NSNull(),
            "trusted": true,
            "voiceConfigured": configured,
        ])
    }

    // Credential-ownership spike only: a raw POST of the SDP string. This is
    // not the existing voice-input provider protocol and is not proof of setup.
    private static func voiceAnswer(sdp: String) async -> String? {
        guard let urlValue = ProcessInfo.processInfo.environment["CHA_VOICE_PROBE_URL"],
              let key = ProcessInfo.processInfo.environment["CHA_VOICE_PROBE_API_KEY"],
              let url = URL(string: urlValue) else {
            return nil
        }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("Bearer \(key)", forHTTPHeaderField: "Authorization")
        let form = "sdp=\(sdp.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed) ?? "")"
        request.httpBody = Data(form.utf8)
        do {
            let (data, _) = try await URLSession.shared.data(for: request)
            return String(data: data, encoding: .utf8)
        } catch {
            return nil
        }
    }

    private func reply(id: String, body: [String: Any]) {
        webView?.callAsyncJavaScript(
            "window.__chaProbeResolve(id, reply)",
            arguments: ["id": id, "reply": body],
            in: nil,
            in: .page,
            completionHandler: { _ in })
    }
}

func makeFeasibilityWebViewConfiguration(assetRoot: URL) -> (
    WKWebViewConfiguration, ChaAssetSchemeHandler, ChaProbeReceiver
) {
    let configuration = WKWebViewConfiguration()
    configuration.mediaTypesRequiringUserActionForPlayback = []
    let handler = ChaAssetSchemeHandler(root: assetRoot)
    configuration.setURLSchemeHandler(handler, forURLScheme: chaAssetScheme)
    let receiver = ChaProbeReceiver()
    let bootstrap = WKUserScript(
        source: """
        window.__chaProbePending = {};
        window.__chaProbeResolve = function(id, reply) {
          var pending = window.__chaProbePending[id];
          if (!pending) return;
          delete window.__chaProbePending[id];
          pending(reply);
        };
        window.__chaProbeSend = function(payload) {
          return new Promise(function(resolve, reject) {
            var id = String(Date.now()) + Math.random();
            window.__chaProbePending[id] = resolve;
            if (!window.webkit || !window.webkit.messageHandlers
                || !window.webkit.messageHandlers.chaProbe) {
              reject(new Error('probe receiver missing'));
              return;
            }
            payload = payload || {};
            payload.id = id;
            window.webkit.messageHandlers.chaProbe.postMessage(payload);
          });
        };
        """,
        injectionTime: .atDocumentStart,
        forMainFrameOnly: true)
    configuration.userContentController.addUserScript(bootstrap)
    return (configuration, handler, receiver)
}
