import Foundation
import WebKit

let chaAssetScheme = "cha"
let chaAssetHost = "app"
let chaAssetOrigin = "cha://app"

// Keep this aligned with the Windows host's policy in packaging/windows/main.cpp.
// Native loaders must apply
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

final class ChaAssetSchemeHandler: NSObject, WKURLSchemeHandler {
    private let root: URL
    private let lock = NSLock()
    private var stopped = Set<ObjectIdentifier>()
    var readMedia: ((String) -> (type: String, body: Data)?)?
    var readMediaChunk: ((String, UInt64) -> (status: Int, type: String, body: Data, complete: Bool))?

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
            if let offsetHeader = urlSchemeTask.request.value(forHTTPHeaderField: "X-CHA-Audio-Offset") {
                guard let offset = UInt64(offsetHeader), let readMediaChunk else {
                    finish(task: urlSchemeTask, identity: identity, url: requestURL,
                           status: 400, type: "text/plain", cache: "no-store", body: Data(), csp: false)
                    return
                }
                let chunk = readMediaChunk(id, offset)
                finish(task: urlSchemeTask, identity: identity, url: requestURL,
                       status: chunk.status, type: chunk.type, cache: "no-store", body: chunk.body,
                       csp: false, extraHeaders: ["X-CHA-Audio-Complete": chunk.complete ? "1" : "0"])
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
        csp: Bool,
        extraHeaders: [String: String] = [:]
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
        headers.merge(extraHeaders) { _, new in new }
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
