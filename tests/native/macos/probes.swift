import Foundation
import WebKit

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
        reply(id: id, body: [
            "id": id,
            "echo": body["echo"] ?? NSNull(),
            "trusted": true,
        ])
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
    let (configuration, handler) = makeNativeWebViewConfiguration(assetRoot: assetRoot)
    handler.readMedia = { id in
        id == "probe-audio" ? (type: "audio/wav", body: chaProbeWavData()) : nil
    }
    // Valid silent MPEG-1 Layer III frames. Withhold half of the clip so the
    // probe can prove playback begins before the producer reports completion.
    let frame = [UInt8](arrayLiteral: 0xff, 0xfb, 0x90, 0xc0) + [UInt8](repeating: 0, count: 413)
    let first = Data((0..<100).flatMap { _ in frame })
    let audio = first + first
    var streamStarted: Date?
    handler.readMediaChunk = { id, offset in
        guard id == "probe-stream", offset <= UInt64(audio.count) else {
            return (404, "text/plain", Data(), false)
        }
        if streamStarted == nil { streamStarted = Date() }
        let finished = Date().timeIntervalSince(streamStarted!) >= 1.0
        let available = finished ? audio.count : first.count
        let start = Int(offset)
        if start > available { return (404, "text/plain", Data(), false) }
        if start >= available && !finished { return (204, "audio/mpeg", Data(), false) }
        return (200, "audio/mpeg", audio.subdata(in: start..<available), finished)
    }
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
