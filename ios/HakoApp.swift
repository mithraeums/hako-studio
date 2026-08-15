// A folder grant, a loopback server (FolderServer.swift), and a WKWebView
// showing the same index.html the site serves. No agent logic lives here: the
// agent is hako.c compiled to wasm, running in that web view. The app exists
// because no iOS browser can open a directory, and only a native process can
// hold a security-scoped bookmark.

import SwiftUI
import UIKit
import UniformTypeIdentifiers
import WebKit

@main
struct HakoApp: App {
	@StateObject private var shell = Shell()

	var body: some Scene {
		WindowGroup {
			ZStack {
				Color(red: 0.075, green: 0.071, blue: 0.035).ignoresSafeArea()
				if let url = shell.pageURL {
					WebView(url: url)
						.ignoresSafeArea(.container, edges: .bottom)
				} else {
					ChooseFolder { shell.pickFolder() }
				}
			}
			.onAppear { shell.start() }
		}
	}
}

// MARK: - shell state

final class Shell: NSObject, ObservableObject, UIDocumentPickerDelegate {
	@Published var pageURL: URL?
	private let server = FolderServer()

	/// A bookmark survives relaunch; a path does not.
	private let bookmarkKey = "hako.folder.bookmark"

	func start() {
		try? server.start()
		if let folder = restoreBookmark() {
			server.setRoot(folder)
			openPage()
		}
	}

	func pickFolder() {
		let picker = UIDocumentPickerViewController(forOpeningContentTypes: [.folder])
		picker.allowsMultipleSelection = false
		picker.delegate = self
		UIApplication.shared.connectedScenes
			.compactMap { ($0 as? UIWindowScene)?.keyWindow?.rootViewController }
			.first?
			.present(picker, animated: true)
	}

	func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
		guard let folder = urls.first else { return }
		guard folder.startAccessingSecurityScopedResource() else { return }
		defer { folder.stopAccessingSecurityScopedResource() }
		if let data = try? folder.bookmarkData(options: .minimalBookmark,
		                                       includingResourceValuesForKeys: nil,
		                                       relativeTo: nil) {
			UserDefaults.standard.set(data, forKey: bookmarkKey)
		}
		server.setRoot(folder)
		openPage()
	}

	private func restoreBookmark() -> URL? {
		guard let data = UserDefaults.standard.data(forKey: bookmarkKey) else { return nil }
		var stale = false
		guard let url = try? URL(resolvingBookmarkData: data, options: [],
		                         relativeTo: nil, bookmarkDataIsStale: &stale) else { return nil }
		if stale {
			if url.startAccessingSecurityScopedResource() {
				defer { url.stopAccessingSecurityScopedResource() }
				if let fresh = try? url.bookmarkData(options: .minimalBookmark,
				                                     includingResourceValuesForKeys: nil,
				                                     relativeTo: nil) {
					UserDefaults.standard.set(fresh, forKey: bookmarkKey)
				}
			}
		}
		return url
	}

	private func openPage() {
		DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { [weak self] in
			guard let self else { return }
			if let url = self.server.baseURL { self.pageURL = url }
			else { self.openPage() }
		}
	}
}

// MARK: - views

struct ChooseFolder: View {
	var action: () -> Void

	var body: some View {
		VStack(spacing: 20) {
			Text("hako studio")
				.font(.system(.title, design: .monospaced))
				.foregroundColor(Color(red: 0.72, green: 0.59, blue: 0.34))
			Text("Choose any folder to work in. Files are read and written in place — a synced folder stays in sync, a local one stays local.")
				.font(.system(.footnote, design: .monospaced))
				.foregroundColor(Color(red: 0.85, green: 0.83, blue: 0.76))
				.multilineTextAlignment(.center)
				.padding(.horizontal, 32)
			Button("choose folder…", action: action)
				.font(.system(.body, design: .monospaced))
				.padding(.horizontal, 18).padding(.vertical, 10)
				.overlay(Rectangle().stroke(Color(red: 0.72, green: 0.59, blue: 0.34), lineWidth: 1))
				.foregroundColor(Color(red: 0.72, green: 0.59, blue: 0.34))
		}
	}
}

struct WebView: UIViewRepresentable {
	let url: URL

	func makeUIView(context: Context) -> WKWebView {
		let cfg = WKWebViewConfiguration()
		cfg.allowsInlineMediaPlayback = true
		let w = WKWebView(frame: .zero, configuration: cfg)
		w.isOpaque = false
		w.backgroundColor = .black
		w.scrollView.contentInsetAdjustmentBehavior = .never
		w.load(URLRequest(url: url))
		return w
	}

	func updateUIView(_ view: WKWebView, context: Context) {
		if view.url != url { view.load(URLRequest(url: url)) }
	}
}
