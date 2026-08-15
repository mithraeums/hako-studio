// Serves the app and the chosen folder over loopback. The agent blocks and
// WKWebView's JS-to-native channel is async, so file access goes over HTTP and
// the worker reads it with a synchronous XHR. Loopback only, behind a
// per-launch path prefix.

import Foundation
import Network

final class FolderServer {
	let token = UUID().uuidString.lowercased()
	private(set) var port: UInt16 = 0

	/// The folder the user picked, held open by a security-scoped bookmark.
	private var root: URL?
	private var listener: NWListener?
	private let queue = DispatchQueue(label: "hako.server", attributes: .concurrent)

	var baseURL: URL? {
		port == 0 ? nil : URL(string: "http://127.0.0.1:\(port)/t/\(token)/index.html")
	}

	func setRoot(_ url: URL?) { root = url }

	func start() throws {
		let params = NWParameters.tcp
		params.requiredLocalEndpoint = .hostPort(host: .ipv4(.loopback), port: .any)
		let l = try NWListener(using: params)
		l.newConnectionHandler = { [weak self] conn in
			conn.start(queue: self?.queue ?? .global())
			self?.receive(conn, buffer: Data())
		}
		l.stateUpdateHandler = { [weak self] st in
			if case .ready = st { self?.port = l.port?.rawValue ?? 0 }
		}
		l.start(queue: queue)
		listener = l
	}

	// MARK: - request plumbing

	private func receive(_ conn: NWConnection, buffer: Data) {
		conn.receive(minimumIncompleteLength: 1, maximumLength: 1 << 20) { [weak self] data, _, done, _ in
			guard let self else { return }
			var buf = buffer
			if let d = data { buf.append(d) }

			// Headers end at the blank line; a POST body may follow and the
			// Content-Length says how much of it to wait for.
			guard let headEnd = buf.range(of: Data("\r\n\r\n".utf8)) else {
				if done { conn.cancel() } else { self.receive(conn, buffer: buf) }
				return
			}
			let head = String(decoding: buf[..<headEnd.lowerBound], as: UTF8.self)
			let want = Self.contentLength(head)
			let body = buf[headEnd.upperBound...]
			if body.count < want {
				self.receive(conn, buffer: buf)
				return
			}
			self.respond(conn, head: head, body: Data(body.prefix(want)))
		}
	}

	private static func contentLength(_ head: String) -> Int {
		for line in head.split(separator: "\r\n") where line.lowercased().hasPrefix("content-length:") {
			return Int(line.split(separator: ":")[1].trimmingCharacters(in: .whitespaces)) ?? 0
		}
		return 0
	}

	private func respond(_ conn: NWConnection, head: String, body: Data) {
		let request = head.split(separator: "\r\n").first.map(String.init) ?? ""
		let parts = request.split(separator: " ")
		guard parts.count >= 2 else { return send(conn, 400, "text/plain", Data()) }
		let method = String(parts[0])
		let target = String(parts[1])

		// Every URL must carry this launch's token, or it is not ours.
		let prefix = "/t/\(token)/"
		guard target.hasPrefix(prefix) else { return send(conn, 404, "text/plain", Data("no\n".utf8)) }
		let rest = String(target.dropFirst(prefix.count))
		let path = String(rest.split(separator: "?").first ?? "")
		let query = Self.parseQuery(rest)

		if path.hasPrefix("fs/") {
			handleFS(conn, method: method, op: String(path.dropFirst(3)), query: query, body: body)
		} else {
			serveBundled(conn, name: path.isEmpty ? "index.html" : path)
		}
	}

	private static func parseQuery(_ rest: String) -> [String: String] {
		guard let q = rest.split(separator: "?").dropFirst().first else { return [:] }
		var out: [String: String] = [:]
		for pair in q.split(separator: "&") {
			let kv = pair.split(separator: "=", maxSplits: 1)
			guard let k = kv.first else { continue }
			let v = kv.count > 1 ? String(kv[1]) : ""
			out[String(k)] = v.removingPercentEncoding ?? v
		}
		return out
	}

	// MARK: - the app itself

	private func serveBundled(_ conn: NWConnection, name: String) {
		let types = ["html": "text/html", "js": "text/javascript",
		             "wasm": "application/wasm", "png": "image/png",
		             "webmanifest": "application/manifest+json"]
		let ext = (name as NSString).pathExtension
		guard !name.contains(".."),
		      let url = Bundle.main.url(forResource: (name as NSString).deletingPathExtension,
		                                withExtension: ext, subdirectory: "web"),
		      let data = try? Data(contentsOf: url) else {
			return send(conn, 404, "text/plain", Data("not found\n".utf8))
		}
		send(conn, 200, types[ext] ?? "application/octet-stream", data)
	}

	// MARK: - the folder

	/// Resolve a path from the agent against the chosen folder, refusing anything
	/// that climbs out of it. The agent is not hostile, but a model writing a
	/// path it invented should not be able to reach the rest of the device.
	private func resolve(_ path: String) -> URL? {
		guard let root else { return nil }
		let rel = path.hasPrefix("/") ? String(path.dropFirst()) : path
		let url = root.appendingPathComponent(rel).standardizedFileURL
		let base = root.standardizedFileURL.path
		guard url.path == base || url.path.hasPrefix(base + "/") else { return nil }
		return url
	}

	private func handleFS(_ conn: NWConnection, method: String, op: String,
	                      query: [String: String], body: Data) {
		guard let root else { return send(conn, 404, "text/plain", Data("no folder\n".utf8)) }
		let fm = FileManager.default

		// The bookmark is only usable inside this pair of calls.
		let scoped = root.startAccessingSecurityScopedResource()
		defer { if scoped { root.stopAccessingSecurityScopedResource() } }

		switch op {
		case "root":
			return sendJSON(conn, "{\"path\":\"/\(root.lastPathComponent)\"}")

		case "read":
			guard let u = resolve(query["path"] ?? ""), let d = Self.read(u) else {
				return send(conn, 404, "text/plain", Data())
			}
			return send(conn, 200, "text/plain; charset=utf-8", d)

		case "write":
			guard let u = resolve(query["path"] ?? "") else { return send(conn, 403, "text/plain", Data()) }
			try? fm.createDirectory(at: u.deletingLastPathComponent(), withIntermediateDirectories: true)
			Self.write(u, body, append: query["append"] == "1")
			return sendJSON(conn, "{\"ok\":true}")

		case "stat":
			guard let u = resolve(query["path"] ?? "") else { return send(conn, 404, "text/plain", Data()) }
			var isDir: ObjCBool = false
			// A not-yet-downloaded file exists; it just has no bytes here yet.
			// Reporting it missing would have the agent create it from scratch
			// and overwrite the copy sitting on another device.
			guard fm.fileExists(atPath: u.path, isDirectory: &isDir) else {
				return send(conn, 404, "text/plain", Data())
			}
			let attrs = try? fm.attributesOfItem(atPath: u.path)
			let size = (attrs?[.size] as? Int) ?? 0
			let mtime = Int((attrs?[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0)
			return sendJSON(conn, "{\"size\":\(size),\"mtime\":\(mtime),\"isDir\":\(isDir.boolValue)}")

		case "mkdirp":
			guard let u = resolve(query["path"] ?? "") else { return send(conn, 403, "text/plain", Data()) }
			try? fm.createDirectory(at: u, withIntermediateDirectories: true)
			return sendJSON(conn, "{\"ok\":true}")

		case "remove":
			guard let u = resolve(query["path"] ?? "") else { return send(conn, 403, "text/plain", Data()) }
			try? fm.removeItem(at: u)
			return sendJSON(conn, "{\"ok\":true}")

		case "list", "listdirs":
			let dirsOnly = (op == "listdirs")
			guard let base = resolve(query["prefix"] ?? "/") else { return sendJSON(conn, "{\"files\":[]}") }
			var out: [String] = []
			let want: Set<URLResourceKey> = [.isDirectoryKey]
			if let e = fm.enumerator(at: base, includingPropertiesForKeys: Array(want)) {
				for case let u as URL in e {
					let isDir = (try? u.resourceValues(forKeys: want))?.isDirectory ?? false
					if isDir != dirsOnly { continue }
					if u.lastPathComponent.hasPrefix(".") { e.skipDescendants(); continue }
					let full = u.standardizedFileURL.path
					let rootPath = root.standardizedFileURL.path
					out.append("/" + root.lastPathComponent + String(full.dropFirst(rootPath.count)))
					if out.count > 5000 { break }
				}
			}
			let items = out.map { "\"\($0.replacingOccurrences(of: "\"", with: "\\\""))\"" }.joined(separator: ",")
			return sendJSON(conn, "{\"\(dirsOnly ? "dirs" : "files")\":[\(items)]}")

		default:
			return send(conn, 404, "text/plain", Data())
		}
	}

	// MARK: - reading and writing
	//
	// A synced folder has a second writer, so access is coordinated. An evicted
	// file is a placeholder and must be downloaded before it can be read.

	private static func materialize(_ url: URL) {
		let keys: Set<URLResourceKey> = [.ubiquitousItemDownloadingStatusKey]
		guard let st = try? url.resourceValues(forKeys: keys).ubiquitousItemDownloadingStatus,
		      st != .current else { return }
		try? FileManager.default.startDownloadingUbiquitousItem(at: url)
		let deadline = Date().addingTimeInterval(20)
		while Date() < deadline {
			if (try? url.resourceValues(forKeys: keys).ubiquitousItemDownloadingStatus) == .current { return }
			Thread.sleep(forTimeInterval: 0.1)
		}
	}

	static func read(_ url: URL) -> Data? {
		materialize(url)
		var out: Data?
		var err: NSError?
		NSFileCoordinator().coordinate(readingItemAt: url, options: [], error: &err) { u in
			out = try? Data(contentsOf: u)
		}
		return out
	}

	static func write(_ url: URL, _ body: Data, append: Bool) {
		if append { materialize(url) }
		var err: NSError?
		NSFileCoordinator().coordinate(writingItemAt: url,
		                               options: append ? [] : .forReplacing, error: &err) { u in
			if append, let h = try? FileHandle(forWritingTo: u) {
				defer { try? h.close() }
				_ = try? h.seekToEnd()
				try? h.write(contentsOf: body)
			} else {
				try? body.write(to: u, options: .atomic)
			}
		}
	}

	// MARK: - replies

	private func sendJSON(_ conn: NWConnection, _ json: String) {
		send(conn, 200, "application/json", Data(json.utf8))
	}

	private func send(_ conn: NWConnection, _ status: Int, _ type: String, _ body: Data) {
		var head = "HTTP/1.1 \(status) \(status == 200 ? "OK" : "ERR")\r\n"
		head += "Content-Type: \(type)\r\n"
		head += "Content-Length: \(body.count)\r\n"
		head += "Cache-Control: no-store\r\n\r\n"
		var out = Data(head.utf8)
		out.append(body)
		conn.send(content: out, completion: .contentProcessed { _ in conn.cancel() })
	}
}
