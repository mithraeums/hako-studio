// hako studio — the browser half of the platform seam in hako.c.
//
// hako.c never learns what a fetch or an OPFS handle is: it calls hkjs_*, and
// these answer. The native build swaps the same six functions for curl and
// POSIX. No agent logic lives here, and none ever should — a JS reimplementation
// of the loop is the one change that would end this project's reason to exist.
//
// The module is built by wasi-sdk clang, so it also imports wasi_snapshot_preview1.
// Everything it needs from that is supplied below: stdout/stderr, a clock, and
// randomness. Files do NOT go through WASI — they go through hkjs_fs_*, which is
// what lets the same C run against OPFS in a tab and a real filesystem natively.

export function hakoImports({ memory, storage, log }) {
	const dec = new TextDecoder();
	const enc = new TextEncoder();
	const view = () => new DataView(memory.buffer);
	const bytes = () => new Uint8Array(memory.buffer);

	const readStr = (ptr, len) => {
		if (len !== undefined) return dec.decode(bytes().subarray(ptr, ptr + len));
		const b = bytes();
		let end = ptr;
		while (b[end]) end++;
		return dec.decode(b.subarray(ptr, end));
	};

	// C owns the memory it is handed back, so allocate through the module.
	let malloc = null;
	const setMalloc = (fn) => { malloc = fn; };
	const writeStr = (s) => {
		const buf = enc.encode(s);
		const ptr = malloc(buf.length + 1);
		bytes().set(buf, ptr);
		bytes()[ptr + buf.length] = 0;
		return ptr;
	};

	// ---- WASI: only what a browser can honestly provide ---------------------
	const ERRNO_OK = 0, ERRNO_BADF = 8, ERRNO_NOSYS = 52;
	let stdoutLine = '', stderrLine = '';

	const emit = (fd, text) => {
		let buf = fd === 1 ? stdoutLine : stderrLine;
		buf += text;
		let nl;
		while ((nl = buf.indexOf('\n')) >= 0) {
			log(fd, buf.slice(0, nl));
			buf = buf.slice(nl + 1);
		}
		if (fd === 1) stdoutLine = buf; else stderrLine = buf;
	};

	const wasi = {
		fd_write(fd, iovs, iovsLen, nwritten) {
			const dv = view();
			let total = 0, text = '';
			for (let i = 0; i < iovsLen; i++) {
				const p = dv.getUint32(iovs + i * 8, true);
				const l = dv.getUint32(iovs + i * 8 + 4, true);
				text += dec.decode(bytes().subarray(p, p + l));
				total += l;
			}
			if (fd === 1 || fd === 2) emit(fd, text);
			dv.setUint32(nwritten, total, true);
			return ERRNO_OK;
		},
		// stdin does not exist here: every prompt the agent would have read from a
		// terminal is answered by the front end instead.
		fd_read: (fd, iovs, iovsLen, nread) => { view().setUint32(nread, 0, true); return ERRNO_OK; },
		fd_close: () => ERRNO_OK,
		fd_seek: (fd, off, whence, out) => { view().setBigUint64(out, 0n, true); return ERRNO_OK; },
		fd_fdstat_get(fd, out) {
			const dv = view();
			// Regular file, never a character device: isatty() must be false here,
			// or the agent renders its interactive terminal UI (boxes, pickers,
			// ANSI) instead of the JSONL a front end can parse.
			dv.setUint8(out, 4);
			dv.setUint16(out + 2, 0, true);
			dv.setBigUint64(out + 8, 0n, true);
			dv.setBigUint64(out + 16, 0n, true);
			return ERRNO_OK;
		},
		fd_prestat_get: () => ERRNO_BADF,               // no preopened directories
		fd_prestat_dir_name: () => ERRNO_BADF,
		path_open: () => ERRNO_NOSYS,                   // files go through hkjs_fs_*
		path_filestat_get: () => ERRNO_NOSYS,
		path_unlink_file: () => ERRNO_NOSYS,
		path_create_directory: () => ERRNO_NOSYS,
		fd_filestat_get: () => ERRNO_NOSYS,
		fd_readdir: () => ERRNO_NOSYS,
		clock_time_get(id, precision, out) {
			view().setBigUint64(out, BigInt(Date.now()) * 1000000n, true);
			return ERRNO_OK;
		},
		random_get(ptr, len) {
			crypto.getRandomValues(bytes().subarray(ptr, ptr + len));
			return ERRNO_OK;
		},
		environ_sizes_get(count, size) {
			const dv = view();
			dv.setUint32(count, 1, true);
			dv.setUint32(size, enc.encode('HOME=/hako\0').length, true);
			return ERRNO_OK;
		},
		environ_get(ptrs, buf) {
			// One variable, because paths in this build are OPFS keys and every
			// one of them is rooted at HOME.
			const dv = view();
			const s = enc.encode('HOME=/hako\0');
			bytes().set(s, buf);
			dv.setUint32(ptrs, buf, true);
			return ERRNO_OK;
		},
		args_sizes_get(count, size) {
			const dv = view();
			dv.setUint32(count, 0, true);
			dv.setUint32(size, 0, true);
			return ERRNO_OK;
		},
		args_get: () => ERRNO_OK,
		proc_exit: (code) => { throw new HakoExit(code); },
		sched_yield: () => ERRNO_OK,
		/* sleep(). Device-code logins poll on it, and answering ENOSYS turns that
		   poll into a hot loop hammering the token endpoint. There is no way to
		   yield a worker without SharedArrayBuffer, so this spins — bounded, and
		   only ever reached while waiting on a person. */
		poll_oneoff(inPtr, outPtr, n, nevents) {
			const dv = view();
			let until = 0;
			for (let i = 0; i < n; i++) {
				const sub = inPtr + i * 48;
				if (dv.getUint8(sub + 8) !== 0) continue;      /* clock subscriptions only */
				const ns = Number(dv.getBigUint64(sub + 24, true));
				until = Math.max(until, Math.min(ns / 1e6, 30000));
			}
			const end = Date.now() + until;
			while (Date.now() < end) { /* wait */ }
			for (let i = 0; i < n; i++) {
				const ev = outPtr + i * 32;
				dv.setBigUint64(ev, dv.getBigUint64(inPtr + i * 48, true), true);
				dv.setUint16(ev + 8, 0, true);
				dv.setUint8(ev + 10, 0);
			}
			dv.setUint32(nevents, n, true);
			return ERRNO_OK;
		},
		// The rest of what wasi-libc references. Answering ENOSYS is honest: this
		// build's files go through hkjs_fs_*, so nothing should reach these.
		fd_fdstat_set_flags: () => ERRNO_OK,
		path_readlink: () => ERRNO_NOSYS,
		path_rename: () => ERRNO_NOSYS,
		path_symlink: () => ERRNO_NOSYS,
	};

	/* Paths arrive as the agent built them, which includes forms like
	   "/work/." from a tool called with ".". The store is keyed by exact
	   string, so an unnormalised path matches nothing and reads as an empty
	   directory rather than an error. */
	const norm = (p) => {
		const abs = String(p || '');
		const out = [];
		for (const seg of abs.split('/')) {
			if (!seg || seg === '.') continue;
			if (seg === '..') { out.pop(); continue; }
			out.push(seg);
		}
		return '/' + out.join('/');
	};

	// ---- the seam -----------------------------------------------------------
	// C blocks, so these must return values, not promises. That rules out the
	// main thread — and rules Asyncify unnecessary: inside a Web Worker the
	// platform already offers synchronous versions of both halves. OPFS has
	// createSyncAccessHandle() for files, and XMLHttpRequest is still allowed to
	// be synchronous off the main thread. The worker blocks; the page does not.
	const env = {
		hkjs_http_fetch(methodPtr, urlPtr, headersPtr, bodyPtr, stream) {
			// The key is the user's and goes straight to the provider: no server
			// of ours is in the path, by design.
			const text = storage.http({
				method: readStr(methodPtr),
				url: readStr(urlPtr),
				headers: parseHeaders(readStr(headersPtr)),
				body: readStr(bodyPtr),
				stream,
			});
			return text === null ? 0 : writeStr(text);
		},
		hkjs_fs_read(pathPtr, outPtrPtr, outLenPtr) {
			const text = storage.read(norm(readStr(pathPtr)));
			if (text === null || text === undefined) return -1;
			const dv = view();
			dv.setUint32(outPtrPtr, writeStr(text), true);
			dv.setUint32(outLenPtr, enc.encode(text).length, true);
			return 0;
		},
		hkjs_fs_write: (pathPtr, dataPtr, len, append) =>
			storage.write(norm(readStr(pathPtr)), readStr(dataPtr, len), !!append) ? 0 : -1,
		hkjs_fs_stat(pathPtr, sizePtr, mtimePtr, isDirPtr) {
			const st = storage.stat(norm(readStr(pathPtr)));
			if (!st) return 0;
			const dv = view();
			if (sizePtr)  dv.setUint32(sizePtr, st.size | 0, true);
			if (mtimePtr) dv.setUint32(mtimePtr, st.mtime | 0, true);
			if (isDirPtr) dv.setUint32(isDirPtr, st.isDir ? 1 : 0, true);
			return 1;
		},
		hkjs_fs_mkdirp: (pathPtr) => storage.mkdirp(norm(readStr(pathPtr))) ? 0 : -1,
		hkjs_fs_remove: (pathPtr) => storage.remove(norm(readStr(pathPtr))) ? 0 : -1,
		/* One level of a directory: "name" per line, directories with a trailing
		   slash — the shape hkListDir has always produced. */
		hkjs_fs_list(pathPtr) {
			const base = norm(readStr(pathPtr));
			const pre = base === '/' ? '/' : base + '/';
			const names = new Set();
			for (const f of storage.list(base)) {
				if (!f.startsWith(pre)) continue;
				const rest = f.slice(pre.length);
				const cut = rest.indexOf('/');
				names.add(cut === -1 ? rest : rest.slice(0, cut) + '/');
			}
			if (storage.listDirs) {
				for (const d of storage.listDirs(base)) {
					if (!d.startsWith(pre)) continue;
					const rest = d.slice(pre.length);
					if (rest && rest.indexOf('/') === -1) names.add(rest + '/');
				}
			}
			return writeStr([...names].sort().join('\n') + (names.size ? '\n' : ''));
		},
	};

	return { wasi_snapshot_preview1: wasi, env, setMalloc };
}

export class HakoExit extends Error {
	constructor(code) { super('exit ' + code); this.code = code; }
}

// Headers arrive in curl's `-H 'K: V'` form because that is what the native
// backend needed. Parse them back rather than fork the C signature.
export function parseHeaders(raw) {
	const out = {};
	const re = /-H\s+'([^:]+):\s*([^']*)'/g;
	let m;
	while ((m = re.exec(raw || '')) !== null) out[m[1].trim()] = m[2].trim();
	return out;
}

// ---- a real folder on the real disk ----------------------------------------
// showDirectoryPicker() gives a live handle to a directory the user chose.
// Chromium only. The API is async and C blocks, so the folder is mirrored into
// memory once, served synchronously, and changed files written back per turn.
export async function dirStorage(handle) {
	const files = new Map();          // "/Notes/todo.md" -> contents
	const dirs = new Set(['/']);
	const dirty = new Set();
	const gone = new Set();
	const SKIP = new Set(['.git', 'node_modules', '.DS_Store', '.hako']);
	const MAX_FILE = 2 * 1024 * 1024;

	const key = (p) => ('/' + String(p).split('/').filter(Boolean).join('/'));
	const parts = (p) => key(p).split('/').filter(Boolean);

	async function load(dir, prefix, depth) {
		if (depth > 8) return;
		for await (const [name, h] of dir.entries()) {
			if (SKIP.has(name)) continue;
			const p = prefix + '/' + name;
			if (h.kind === 'directory') { dirs.add(p); await load(h, p, depth + 1); }
			else {
				try {
					const f = await h.getFile();
					if (f.size > MAX_FILE) continue;
					const text = await f.text();
					if (text.indexOf('\u0000') >= 0) continue;   /* binary */
					files.set(p, text);
				} catch { /* unreadable: treat as absent */ }
			}
		}
	}
	await load(handle, '', 0);

	async function dirHandle(p, create) {
		let dir = handle;
		for (const seg of parts(p).slice(0, -1))
			dir = await dir.getDirectoryHandle(seg, { create });
		return dir;
	}

	return {
		read: (p) => (files.has(key(p)) ? files.get(key(p)) : null),
		write(p, data, append) {
			const k = key(p);
			files.set(k, append && files.has(k) ? files.get(k) + data : data);
			dirty.add(k);
			gone.delete(k);
			return true;
		},
		stat(p) {
			const k = key(p);
			if (files.has(k)) return { size: files.get(k).length, mtime: 0, isDir: false };
			if (dirs.has(k) || k === '/') return { size: 0, mtime: 0, isDir: true };
			for (const f of files.keys()) if (f.startsWith(k + '/')) return { size: 0, mtime: 0, isDir: true };
			return null;
		},
		mkdirp(p) { dirs.add(key(p)); return true; },
		list(prefix) {
			const k = key(prefix);
			return [...files.keys()].filter((f) => k === '/' ? true : f.startsWith(k + '/'));
		},
		listDirs(prefix) {
			const k = key(prefix) === '/' ? '' : key(prefix);
			return [...dirs].filter((d) => d.startsWith(k + '/'));
		},
		remove(p) { const k = key(p); files.delete(k); dirty.delete(k); dirs.delete(k); gone.add(k); return true; },

		async flush() {
			for (const d of dirs) {
				if (d === '/') continue;
				try {
					let cur = handle;
					for (const seg of parts(d)) cur = await cur.getDirectoryHandle(seg, { create: true });
				} catch { /* denied: the write below will report it */ }
			}
			for (const k of dirty) {
				try {
					const dir = await dirHandle(k, true);
					const fh = await dir.getFileHandle(parts(k).pop(), { create: true });
					const w = await fh.createWritable();
					await w.write(files.get(k));
					await w.close();
				} catch { /* permission revoked or locked: mirror still holds it */ }
			}
			dirty.clear();
			for (const k of gone) {
				try { (await dirHandle(k, false)).removeEntry(parts(k).pop()); } catch {}
			}
			gone.clear();
		},
	};
}

// ---- a real folder, by way of a native shell -------------------------------
// A WebView shell holds the folder grant and serves it over loopback. Its
// JS-to-native channel is async and the agent blocks, so this uses sync XHR.
export function nativeStorage(base) {
	const call = (method, path, query, body) => {
		const x = new XMLHttpRequest();
		const qs = Object.entries(query).map(([k, v]) => k + '=' + encodeURIComponent(v)).join('&');
		/* Relative: the shell serves under a per-launch path prefix. */
		x.open(method, base + path + '?' + qs, false);
		try { x.send(body === undefined ? null : body); } catch (e) { return null; }
		return x.status >= 200 && x.status < 300 ? x.responseText : null;
	};
	const json = (r) => { try { return r === null ? null : JSON.parse(r); } catch (e) { return null; } };
	return {
		root: () => (json(call('GET', 'fs/root', {})) || {}).path || '/',
		read: (path) => call('GET', 'fs/read', { path }),
		write: (path, data, append) => call('POST', 'fs/write', { path, append: append ? 1 : 0 }, data) !== null,
		stat: (path) => json(call('GET', 'fs/stat', { path })),
		mkdirp: (path) => call('POST', 'fs/mkdirp', { path }) !== null,
		remove: (path) => call('POST', 'fs/remove', { path }) !== null,
		list: (prefix) => (json(call('GET', 'fs/list', { prefix })) || {}).files || [],
		listDirs: (prefix) => (json(call('GET', 'fs/listdirs', { prefix })) || {}).dirs || [],
		flush: async () => {},          /* the shell writes through */
	};
}

/* Is a shell hosting us? */
export async function nativeRoot(base) {
	try {
		const r = await fetch(base + 'fs/root');
		if (!r.ok) return null;
		return (await r.json()).path || null;
	} catch (e) { return null; }
}

// ---- routing ---------------------------------------------------------------
// Agent state (the API key above all) stays on the device; the work goes in the
// chosen folder. One path prefix decides which, so hako.c sees a single store.
export function mountedStorage(device, mount, mountAt) {
	const pick = (p) => (mount && mountAt && (p === mountAt || p.startsWith(mountAt + '/')))
		? { s: mount, rel: p.slice(mountAt.length) || '/' }
		: { s: device, rel: p };
	return {
		http: (req) => device.http(req),
		read: (p) => { const { s, rel } = pick(p); return s.read(rel); },
		write: (p, d, a) => { const { s, rel } = pick(p); return s.write(rel, d, a); },
		stat: (p) => { const { s, rel } = pick(p); return s.stat(rel); },
		mkdirp: (p) => { const { s, rel } = pick(p); return s.mkdirp(rel); },
		remove: (p) => { const { s, rel } = pick(p); return s.remove(rel); },
		list(prefix) {
			const { s, rel } = pick(prefix);
			const out = s.list(rel);
			return s === mount ? out.map((f) => mountAt + (f === '/' ? '' : f)) : out;
		},
		listDirs(prefix) {
			const { s, rel } = pick(prefix);
			const out = s.listDirs ? s.listDirs(rel) : [];
			return s === mount ? out.map((d) => mountAt + d) : out;
		},
		async flush() { await device.flush(); if (mount) await mount.flush(); },
	};
}

// ---- OPFS storage: where the agent's own state lives ------------------------
// OPFS only offers *one* synchronous call — createSyncAccessHandle — and even
// that requires an async getFileHandle first. There is no sync way to resolve a
// path. So the tree is mirrored into memory at boot, the seam works against the
// mirror (synchronously, as C requires), and dirty files are flushed back after
// each turn. Origin-private throughout: invisible to the user and to every other
// site, and never sent anywhere.
export async function opfsStorage() {
	const root = await navigator.storage.getDirectory();
	const files = new Map();          // "/hako/.hakorc" -> contents
	const dirs = new Set(['/']);
	const dirty = new Set();
	const gone = new Set();

	const key = (p) => ('/' + String(p).split('/').filter(Boolean).join('/'));
	const parts = (p) => key(p).split('/').filter(Boolean);

	/* The whole tree is held in memory so the blocked C side can read it
	   synchronously, so what goes in has to stay bounded. */
	const MAX_MIRROR = 4 * 1024 * 1024;
	async function load(dir, prefix) {
		for await (const [name, handle] of dir.entries()) {
			const p = prefix + '/' + name;
			if (handle.kind === 'directory') { dirs.add(p); await load(handle, p); }
			else {
				try {
					const f = await handle.getFile();
					if (f.size > MAX_MIRROR) continue;
					files.set(p, await f.text());
				} catch { /* locked or unreadable: treat as absent */ }
			}
		}
	}
	await load(root, '');

	async function dirHandle(p, create) {
		let dir = root;
		for (const seg of parts(p).slice(0, -1))
			dir = await dir.getDirectoryHandle(seg, { create });
		return dir;
	}

	return {
		// Synchronous XHR: legal off the main thread, and the only way to hand a
		// blocking C loop an HTTP response without rewriting the loop.
		http({ method, url, headers, body }) {
			try {
				const x = new XMLHttpRequest();
				x.open(method || 'GET', url, false);
				for (const k in headers) x.setRequestHeader(k, headers[k]);
				x.send(body || null);
				return x.responseText;
			} catch (e) {
				// A network error must not read as an empty response — that
				// ambiguity cost real debugging time on the native side.
				return JSON.stringify({ error: { message: String(e?.message || e) } });
			}
		},
		read: (p) => (files.has(key(p)) ? files.get(key(p)) : null),
		write(p, data, append) {
			const k = key(p);
			files.set(k, append && files.has(k) ? files.get(k) + data : data);
			dirty.add(k);
			gone.delete(k);
			return true;
		},
		stat(p) {
			const k = key(p);
			if (files.has(k)) return { size: files.get(k).length, mtime: 0, isDir: false };
			if (dirs.has(k)) return { size: 0, mtime: 0, isDir: true };
			for (const f of files.keys()) if (f.startsWith(k + '/')) return { size: 0, mtime: 0, isDir: true };
			return null;
		},
		mkdirp(p) { dirs.add(key(p)); return true; },
		list(prefix) {
			const k = key(prefix);
			return [...files.keys()].filter((f) => f.startsWith(k + '/'));
		},
		listDirs(prefix) {
			const k = key(prefix) === '/' ? '' : key(prefix);
			return [...dirs].filter((d) => d !== '/' && d.startsWith(k + '/'));
		},
		remove(p) { const k = key(p); files.delete(k); dirty.delete(k); dirs.delete(k); gone.add(k); return true; },

		async flush() {
			for (const k of dirty) {
				try {
					const dir = await dirHandle(k, true);
					const fh = await dir.getFileHandle(parts(k).pop(), { create: true });
					const w = await fh.createWritable();
					await w.write(files.get(k));
					await w.close();
				} catch (e) { /* quota or lock: the mirror still holds it this session */ }
			}
			dirty.clear();
			for (const k of gone) {
				try { (await dirHandle(k, false)).removeEntry(parts(k).pop()); } catch {}
			}
			gone.clear();
		},
	};
}
