// WindowSmash Mods on macOS: a window around the mod manager's page. tools/modmanager/server.py
// is the app (the same on Windows, where it opens in Edge's app mode); this starts it, shows its
// page in a web view, gives the page what a browser would (file pickers, confirm dialogs, saving a
// modpack file, links opened in the browser) and stops the server when the window closes.
// Built by tools/mac/build_mods_app.sh into build-mac/WindowSmash Mods.app.
import AppKit
import WebKit

enum Port {
    static let root: String = {
        if let r = Bundle.main.object(forInfoDictionaryKey: "WindowSmashRoot") as? String, !r.isEmpty,
           FileManager.default.fileExists(atPath: r + "/tools/modmanager/server.py") { return r }
        var url = Bundle.main.bundleURL
        for _ in 0..<4 {
            url.deleteLastPathComponent()
            if FileManager.default.fileExists(atPath: url.appendingPathComponent("tools/modmanager/server.py").path) { return url.path }
        }
        return NSHomeDirectory() + "/WindowSmash"
    }()
    /// WindowSmash's own Python (the installer's venv), else one on the Mac
    static var python: String {
        for p in [root + "/deps/venv/bin/python", root + "/deps/python/bin/python3", "/opt/homebrew/bin/python3",
                  "/usr/local/bin/python3", "/Library/Frameworks/Python.framework/Versions/Current/bin/python3"]
        where FileManager.default.isExecutableFile(atPath: p) { return p }
        return "/usr/bin/env"
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate, WKUIDelegate, WKNavigationDelegate, WKDownloadDelegate, WKScriptMessageHandler {
    var window: NSWindow!
    var web: WKWebView!
    var server: Process?
    var pageURL: URL?

    func applicationDidFinishLaunching(_ note: Notification) {
        let config = WKWebViewConfiguration()
        config.preferences.setValue(true, forKey: "developerExtrasEnabled")
        config.userContentController.add(self, name: "theme")   // (the page's theme: the title bar follows it)
        web = WKWebView(frame: .zero, configuration: config)
        web.uiDelegate = self
        web.navigationDelegate = self
        web.setValue(false, forKey: "drawsBackground")
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1280, height: 820),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable],
                          backing: .buffered, defer: false)
        window.title = "WindowSmash Mods"
        window.titlebarAppearsTransparent = true
        window.appearance = NSAppearance(named: .darkAqua)
        window.backgroundColor = NSColor(red: 0.043, green: 0.039, blue: 0.141, alpha: 1)
        window.minSize = NSSize(width: 720, height: 520)
        window.contentView = web
        window.center()
        window.setFrameAutosaveName("WindowSmashMods")
        window.makeKeyAndOrderFront(nil)
        menus()
        web.loadHTMLString("<body style='background:#0b0a24;color:#a69fd2;font:15px system-ui;display:grid;place-items:center;height:90vh'>Starting…</body>", baseURL: nil)
        startServer()
    }

    func startServer() {
        let p = Process()
        let py = Port.python
        p.executableURL = URL(fileURLWithPath: py)
        p.arguments = (py == "/usr/bin/env" ? ["python3"] : []) + [Port.root + "/tools/modmanager/server.py", "--tree", Port.root, "--no-window"]
        p.currentDirectoryURL = URL(fileURLWithPath: Port.root)
        let out = Pipe(), err = Pipe()
        p.standardOutput = out
        p.standardError = err
        do { try p.run() } catch {
            fail("Can't start WindowSmash's Python (\(py)): run Install WindowSmash.command first.\n\n\(error.localizedDescription)")
            return
        }
        server = p
        DispatchQueue.global().async {
            let handle = out.fileHandleForReading
            var buffer = Data()
            while true {
                let chunk = handle.availableData
                if chunk.isEmpty { break }
                buffer.append(chunk)
                if let nl = buffer.firstIndex(of: 10),
                   let obj = try? JSONSerialization.jsonObject(with: buffer[..<nl]) as? [String: Any],
                   let s = obj["url"] as? String, let u = URL(string: s) {
                    DispatchQueue.main.async { self.pageURL = u; self.web.load(URLRequest(url: u)) }
                    return
                }
            }
            let msg = String(data: err.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
            DispatchQueue.main.async { self.fail("The mod manager stopped:\n\n" + msg.suffix(1500)) }
        }
    }

    func fail(_ text: String) {
        let a = NSAlert()
        a.messageText = "WindowSmash Mods"
        a.informativeText = text
        a.runModal()
        NSApp.terminate(nil)
    }

    /// The page's theme ("dark" or "light", Settings): the window's title bar and background follow it
    func userContentController(_ controller: WKUserContentController, didReceive message: WKScriptMessage) {
        let dark = (message.body as? String) != "light"
        window.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
        window.backgroundColor = dark ? NSColor(red: 0.043, green: 0.039, blue: 0.141, alpha: 1) : NSColor(red: 0.945, green: 0.933, blue: 0.984, alpha: 1)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ app: NSApplication) -> Bool { true }
    func applicationWillTerminate(_ note: Notification) { server?.terminate() }

    func menus() {
        let main = NSMenu()
        let app = NSMenuItem(); main.addItem(app)
        let am = NSMenu()
        am.addItem(withTitle: "Quit WindowSmash Mods", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        app.submenu = am
        let edit = NSMenuItem(); main.addItem(edit)
        let em = NSMenu(title: "Edit")
        em.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
        em.addItem(.separator())
        em.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        em.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        em.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        em.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        edit.submenu = em
        let view = NSMenuItem(); main.addItem(view)
        let vm = NSMenu(title: "View")
        vm.addItem(withTitle: "Reload", action: #selector(reload), keyEquivalent: "r")
        view.submenu = vm
        NSApp.mainMenu = main
    }
    @objc func reload() { if let u = pageURL { web.load(URLRequest(url: u)) } }

    // links out of the app open in the browser
    func webView(_ webView: WKWebView, createWebViewWith configuration: WKWebViewConfiguration,
                 for action: WKNavigationAction, windowFeatures: WKWindowFeatures) -> WKWebView? {
        if let u = action.request.url { NSWorkspace.shared.open(u) }
        return nil
    }
    func webView(_ webView: WKWebView, decidePolicyFor action: WKNavigationAction,
                 decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        if action.shouldPerformDownload { decisionHandler(.download); return }
        if let u = action.request.url, let host = u.host, host != "127.0.0.1", u.scheme?.hasPrefix("http") == true,
           action.navigationType == .linkActivated {
            NSWorkspace.shared.open(u)
            decisionHandler(.cancel)
            return
        }
        decisionHandler(.allow)
    }
    func webView(_ webView: WKWebView, navigationAction: WKNavigationAction, didBecome download: WKDownload) { download.delegate = self }
    func webView(_ webView: WKWebView, navigationResponse: WKNavigationResponse, didBecome download: WKDownload) { download.delegate = self }

    // a modpack file saved by the page: ~/Downloads (shown in the Finder)
    func download(_ download: WKDownload, decideDestinationUsing response: URLResponse, suggestedFilename: String,
                  completionHandler: @escaping (URL?) -> Void) {
        let dir = FileManager.default.urls(for: .downloadsDirectory, in: .userDomainMask)[0]
        var target = dir.appendingPathComponent(suggestedFilename)
        var k = 2
        while FileManager.default.fileExists(atPath: target.path) {
            let base = (suggestedFilename as NSString).deletingPathExtension, ext = (suggestedFilename as NSString).pathExtension
            target = dir.appendingPathComponent("\(base) (\(k)).\(ext)")
            k += 1
        }
        completionHandler(target)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) { NSWorkspace.shared.activateFileViewerSelecting([target]) }
    }

    // the page's file picker, confirm() and alert()
    func webView(_ webView: WKWebView, runOpenPanelWith parameters: WKOpenPanelParameters, initiatedByFrame frame: WKFrameInfo,
                 completionHandler: @escaping ([URL]?) -> Void) {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = parameters.allowsMultipleSelection
        panel.canChooseDirectories = false
        panel.beginSheetModal(for: window) { r in completionHandler(r == .OK ? panel.urls : nil) }
    }
    func webView(_ webView: WKWebView, runJavaScriptConfirmPanelWithMessage message: String, initiatedByFrame frame: WKFrameInfo,
                 completionHandler: @escaping (Bool) -> Void) {
        let a = NSAlert()
        a.messageText = message
        a.addButton(withTitle: "OK")
        a.addButton(withTitle: "Cancel")
        a.beginSheetModal(for: window) { r in completionHandler(r == .alertFirstButtonReturn) }
    }
    func webView(_ webView: WKWebView, runJavaScriptAlertPanelWithMessage message: String, initiatedByFrame frame: WKFrameInfo,
                 completionHandler: @escaping () -> Void) {
        let a = NSAlert()
        a.messageText = message
        a.beginSheetModal(for: window) { _ in completionHandler() }
    }
}

@main
struct Main {
    static func main() {
        let app = NSApplication.shared
        let delegate = AppDelegate()
        app.delegate = delegate
        app.setActivationPolicy(.regular)
        app.activate(ignoringOtherApps: true)
        app.run()
    }
}
