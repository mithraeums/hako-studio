// hako studio — the worker that runs the agent.
//
// Why a worker at all: hako.c blocks. It calls hk_http_fetch and waits. On the
// main thread that would freeze the page and the seam could only return
// promises, which C cannot read. Inside a worker both halves have synchronous
// forms — OPFS createSyncAccessHandle for files, synchronous XMLHttpRequest for
// HTTP — so the C loop runs unmodified and the page stays responsive.
//
// Why one instance per turn: the alternative is keeping the module alive and
// blocking its stdin between turns, which needs SharedArrayBuffer + Atomics.wait
// and therefore COOP/COEP headers — headers a static host like GitHub Pages
// cannot set. Instantiating per turn costs ~50 ms and needs no headers at all.
// State survives because it lives in OPFS, exactly as it survives on disk
// natively: same code, same files, same sessions.

import { hakoImports, HakoExit, opfsStorage, dirStorage, nativeStorage, nativeRoot, mountedStorage } from './hako-glue.js';

let wasmBytes = null;
let wasmModule = null;          /* compiled once; recompiling per turn is what a phone runs out of memory on */
let device = null;               /* the agent's own state: key, sessions */
let storage = null;              /* what the agent sees: device + the real folder */
let folder = null;               /* the folder the user picked, if any */
let nativeBase = null;           /* set when a native shell is serving the folder */

let bootUrl = 'hako.wasm';

/* A worker can be terminated under memory pressure and silently replaced. The
   page has no way to know, so every message that needs state checks for it
   rather than failing on a null module. */
async function ensureBooted() {
	if (!wasmModule || !storage) await boot(bootUrl);
}

async function boot(wasmUrl) {
	bootUrl = wasmUrl || bootUrl;
	// Handles must be resolved before the module runs: everything after this
	// point is synchronous, by design.
	try {
		globalThis.__hakoRoot = await navigator.storage.getDirectory();
		device = await opfsStorage();
	} catch (e) {
		/* Safari gives a private window no on-device storage, and says so as an
		   "unknown transient" failure that tells nobody what to do. */
		throw new Error('this browser gave the page no on-device storage (' + (e && e.name || e) +
			'). A private window does that — open hako studio in a normal window.');
	}
	device.mkdirp(WORKSPACE);
	storage = mountedStorage(device, null, null);
	await device.flush();
	nativeBase = (await nativeRoot('')) === null ? null : '';
	if (nativeBase !== null) await mountFolder(null);
	wasmBytes = await (await fetch(wasmUrl)).arrayBuffer();
	wasmModule = await WebAssembly.compile(wasmBytes);
	wasmBytes = null;           /* the module holds what is needed from here */
	/* The page has no other way to learn where the agent is working, and a page
	   pointed at the wrong directory shows an empty file list beside a file the
	   agent just wrote. */
	return { ok: true, dir: workDir, folder: folder ? workDir : '' };
}

/* Where work goes when the platform offers no directory API. On this device
   only; the picker lists it and can save files out to Files. */
const WORKSPACE = '/workspace';

let workDir = WORKSPACE;

function projectDir() {
	return '/hako/.hako/projects/' + workDir.replace(/\//g, '-');
}

async function mountFolder(handle) {
	if (nativeBase !== null) {
		folder = nativeStorage(nativeBase);
		workDir = folder.root();
	} else {
		folder = handle ? await dirStorage(handle) : null;
		workDir = handle ? '/' + handle.name : WORKSPACE;
		if (!handle) device.mkdirp(WORKSPACE);
	}
	storage = mountedStorage(device, folder, folder ? workDir : null);
	sessionId = null;
	return workDir;
}

let sessionId = null;
let agentVersion = '';

// Run the agent once with a scripted stdin, streaming its stdout back as it goes.
function runTurn(lines, onLine) {
	let quiet = false;      /* the resume is bookkeeping, not conversation */
	if (sessionId) {
		lines = [JSON.stringify({ type: 'slash', cmd: ':resume ' + sessionId }), ...lines];
		quiet = true;
	}
	const filter = (fd, line) => {
		if (!quiet) return onLine(fd, line);
		if (fd === 1 && line.indexOf('"type":"done"') >= 0) quiet = false;
		if (fd !== 1) onLine(fd, line);
	};
	const script = lines.map((l) => l + '\n').join('');
	let pos = 0;

	const memory = { buffer: new ArrayBuffer(0) };
	const imports = hakoImports({
		memory,
		storage,
		log: (fd, line) => {
			if (fd === 1 && line.indexOf('"type":"init"') >= 0) {
				/* The agent's own state is the truth about provider and model;
				   ~/.hakorc only holds what was last written to it. */
				const p = line.match(/"provider":"([^"]*)"/);
				const md = line.match(/"model":"([^"]*)"/);
				if (p || md) self.postMessage({ type: 'live', provider: p ? p[1] : '', model: md ? md[1] : '' });
				const v = line.match(/"version":"([^"]*)"/);
				if (v && v[1] && v[1] !== agentVersion) {
					agentVersion = v[1];
					self.postMessage({ type: 'version', version: agentVersion });
				}
				const m = line.match(/"session":"([^"]*)"/);
				if (!sessionId && m && m[1]) { sessionId = m[1]; self.postMessage({ type: 'session', id: sessionId }); }
			}
			filter(fd, line);
		},
	});

	// argv: the module comes up in JSONL mode, the same protocol the editor and
	// `hako --serve` already speak. One protocol, three carriers.
	const argv = ['hako', '--pipe', '--dir', workDir];
	const enc = new TextEncoder();
	imports.wasi_snapshot_preview1.args_sizes_get = (countPtr, sizePtr) => {
		const dv = new DataView(instance.exports.memory.buffer);
		dv.setUint32(countPtr, argv.length, true);
		dv.setUint32(sizePtr, argv.reduce((n, a) => n + enc.encode(a).length + 1, 0), true);
		return 0;
	};
	imports.wasi_snapshot_preview1.args_get = (ptrsPtr, bufPtr) => {
		const dv = new DataView(instance.exports.memory.buffer);
		const b = new Uint8Array(instance.exports.memory.buffer);
		let p = bufPtr;
		argv.forEach((a, i) => {
			dv.setUint32(ptrsPtr + i * 4, p, true);
			const bytes = enc.encode(a + '\0');
			b.set(bytes, p);
			p += bytes.length;
		});
		return 0;
	};
	// stdin is the scripted turn; EOF ends the run, which is what we want.
	imports.wasi_snapshot_preview1.fd_read = (fd, iovs, iovsLen, nread) => {
		const dv = new DataView(instance.exports.memory.buffer);
		const b = new Uint8Array(instance.exports.memory.buffer);
		let wrote = 0;
		for (let i = 0; i < iovsLen && pos < script.length; i++) {
			const p = dv.getUint32(iovs + i * 8, true);
			const l = dv.getUint32(iovs + i * 8 + 4, true);
			const chunk = script.slice(pos, pos + l);
			const bytes = enc.encode(chunk);
			b.set(bytes, p);
			pos += chunk.length;
			wrote += bytes.length;
		}
		dv.setUint32(nread, wrote, true);
		return 0;
	};

	const instance = new WebAssembly.Instance(wasmModule, {
		wasi_snapshot_preview1: imports.wasi_snapshot_preview1,
		env: imports.env,
	});
	Object.defineProperty(memory, 'buffer', { get: () => instance.exports.memory.buffer });
	imports.setMalloc(instance.exports.malloc);

	try {
		instance.exports._start();
	} catch (e) {
		if (!(e instanceof HakoExit)) throw e;
	}
}

/* Credentials in the browser build live where the agent already looks for them:
   ~/.hakorc, which here is an OPFS file. No new format, no second code path —
   the same parser reads it natively. */
const RC_PATH = '/hako/.hakorc';

function parseKV(text) {
	const out = {};
	for (const line of (text || '').split('\n')) {
		const eq = line.indexOf('=');
		if (eq > 0 && line[0] !== '#') out[line.slice(0, eq).trim()] = line.slice(eq + 1).trim();
	}
	return out;
}

function readConfig() {
	return parseKV(storage.read(RC_PATH));
}

function writeConfig(patch) {
	const cfg = { ...readConfig(), ...patch };
	const body = Object.entries(cfg)
		.filter(([, v]) => v !== null && v !== undefined && v !== '')
		.map(([k, v]) => `${k}=${v}`)
		.join('\n') + '\n';
	storage.mkdirp('/hako');
	return storage.write(RC_PATH, body, false);
}

// Never hand the key back to the page — it has no use for it, and a UI that
// cannot leak what it never receives is easier to trust.
function safeConfig() {
	const c = readConfig();
	/* ~/.hakorc only holds what was written to it. The agent keeps the live
	   provider and model in its own state, which is what a reload must show. */
	const st = parseKV(storage.read('/hako/.hako/state'));
	return {
		provider: st.ai_provider || c.ai_provider || '',
		model: st.ai_model || c.ai_model || '',
		has_key: !!((c.ai_api_key && c.ai_api_key.length) ||
		            storage.read('/hako/.hako/credentials')),
	};
}

self.onmessage = async (ev) => {
	const msg = ev.data || {};
	try {
		if (msg.type === 'boot') {
			const info = await boot(msg.wasmUrl || 'hako.wasm');
			self.postMessage({ type: 'ready', ...info });
			return;
		}
		await ensureBooted();
		if (msg.type === 'config') {
			// The key never leaves this device: it goes into OPFS and from there
			// straight to the provider on each request.
			writeConfig(msg.patch || {});
			await storage.flush();          /* a key must survive a reload */
			self.postMessage({ type: 'config', config: safeConfig() });
			return;
		}
		if (msg.type === 'state') {
			self.postMessage({ type: 'config', config: safeConfig() });
			return;
		}
		if (msg.type === 'dirs') {
			/* Synthesised from the mirror: OPFS has no readdir the C side can call,
			   but the whole tree is already in memory here. */
			const base = msg.path || workDir;
			const dirs = new Set(), files = [];
			const under = (p) => p.startsWith(base === '/' ? '/' : base + '/');
			for (const p of storage.list(base)) {
				if (!under(p) || p.startsWith('/hako/')) continue;
				const rest = p.slice(base === '/' ? 1 : base.length + 1);
				const cut = rest.indexOf('/');
				if (cut === -1) files.push({ name: rest, bytes: (storage.read(p) || '').length });
				else dirs.add(rest.slice(0, cut));
			}
			for (const d of storage.listDirs(base)) {
				if (!under(d) || d.startsWith('/hako/')) continue;
				const rest = d.slice(base === '/' ? 1 : base.length + 1);
				if (rest && rest.indexOf('/') === -1) dirs.add(rest);
			}
			self.postMessage({ type: 'dirs', id: msg.id, path: base, dirs: [...dirs], files });
			return;
		}
		if (msg.type === 'mount') {
			const path = await mountFolder(msg.handle || null);
			self.postMessage({ type: 'open', id: msg.id, path, files: msg.handle ? storage.list(path).length : 0 });
			return;
		}
		if (msg.type === 'open') {
			if (msg.path) {
				storage.mkdirp(msg.path);
				workDir = msg.path;
			}
			sessionId = null;
			self.postMessage({ type: 'open', id: msg.id, path: workDir });
			return;
		}
		if (msg.type === 'mkdir') {
			storage.mkdirp(msg.path);
			await storage.flush();
			self.postMessage({ type: 'open', id: msg.id, path: workDir });
			return;
		}
		if (msg.type === 'file-read') {
			self.postMessage({ type: 'ok', id: msg.id, text: storage.read(msg.path) });
			return;
		}
		if (msg.type === 'file-delete') {
			/* A folder is a prefix here, so removing one means removing what is
			   under it — there is no directory object to unlink. */
			const p = msg.path;
			let n = 0;
			for (const f of storage.list(p)) { storage.remove(f); n++; }
			storage.remove(p);
			if (n === 0) n = 1;
			await storage.flush();
			self.postMessage({ type: 'ok', id: msg.id, ok: true, removed: n });
			return;
		}
		if (msg.type === 'file-write') {
			storage.mkdirp(msg.path.replace(/\/[^/]*$/, '') || '/');
			storage.write(msg.path, msg.text, false);
			await storage.flush();
			self.postMessage({ type: 'ok', id: msg.id, ok: true });
			return;
		}
		if (msg.type === 'session-use') {
			sessionId = msg.session || null;
			self.postMessage({ type: 'sessions', id: msg.id, sessions: [] });
			return;
		}
		if (msg.type === 'session-delete') {
			for (const p of storage.list(projectDir())) {
				if (p.endsWith('/' + msg.session + '.jsonl') || p.endsWith('/' + msg.session + '.title'))
					storage.remove(p);
			}
			if (sessionId === msg.session) sessionId = null;
			await storage.flush();
			self.postMessage({ type: 'sessions', id: msg.id, sessions: [] });
			return;
		}
		if (msg.type === 'session-rename') {
			for (const p of storage.list(projectDir())) {
				if (p.endsWith('/' + msg.session + '.jsonl')) {
					const side = p.replace(/\.jsonl$/, '.title');
					if (msg.title && msg.title.trim()) storage.write(side, msg.title.trim() + '\n', false);
					else storage.remove(side);
				}
			}
			await storage.flush();
			self.postMessage({ type: 'sessions', id: msg.id, sessions: [] });
			return;
		}
		if (msg.type === 'sessions') {
			/* ~/.hako/projects/<enc>/sessions/<id>.jsonl, scoped to this folder. */
			const paths = storage.list(projectDir());
			const out = [];
			for (const p of paths) {
				if (!p.endsWith('.jsonl')) continue;
				const id = p.split('/').pop().replace(/\.jsonl$/, '');
				const body = storage.read(p) || '';
				const sidecar = storage.read(p.replace(/\.jsonl$/, '.title'));
				let title = sidecar ? sidecar.trim() : '', turns = 0;
				for (const line of body.split('\n')) {
					if (!line.trim()) continue;
					turns++;
					if (!title) {
						try {
							const m = JSON.parse(line);
							if (m.role === 'user' && m.content) title = m.content.slice(0, 80);
						} catch (e) { /* skip */ }
					}
				}
				/* OPFS keeps no mtime; the id starts with its creation time in hex. */
				const born = parseInt(id.slice(0, 8), 16);
				if (!title && id !== sessionId) continue;   /* every page load opens one; one with no prompt in it is noise */
				out.push({ id, title, mtime: born > 1e9 ? born : 0, turns, path: p });
			}
			self.postMessage({ type: 'sessions', id: msg.id, sessions: out });
			return;
		}
		if (msg.type === 'history') {
			const paths = storage.list(projectDir()).filter((p) => p.endsWith(msg.session + '.jsonl'));
			const messages = [];
			if (paths.length) {
				for (const line of (storage.read(paths[0]) || '').split('\n')) {
					if (!line.trim()) continue;
					try { messages.push(JSON.parse(line)); } catch (e) { /* skip */ }
				}
			}
			self.postMessage({ type: 'history', id: msg.id, messages });
			return;
		}
		if (msg.type === 'run') {
			// Whatever the core prints on stdout is the JSONL protocol; stderr is
			// diagnostics. Pass both up and let the page decide.
			runTurn(msg.lines, (fd, line) => {
				if (fd === 1) self.postMessage({ type: 'out', line });
				else self.postMessage({ type: 'log', line });
			});
			await storage.flush();          /* persist what the turn wrote */
			self.postMessage({ type: 'done' });
			return;
		}
	} catch (e) {
		/* Name the operation: "unknown transient reason" alone is unactionable. */
		console.error(e && e.stack || e);
		self.postMessage({ type: 'error',
			error: (msg.type || 'worker') + ': ' + String(e && e.message || e) });
	}
};
