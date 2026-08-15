// Run hako.wasm under node with the same glue the browser uses.
//
// A browser is a slow place to find out that a wasm module traps on startup, so
// this harness instantiates it with the real imports, a memory-backed filesystem
// standing in for OPFS, and node's fetch. What fails here fails in the tab.
//
//   node tools/run-node.mjs [hako.wasm]

import { readFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { hakoImports, HakoExit } from './hako-glue.js';

const wasmPath = process.argv[2] || 'hako.wasm';
const bytes = readFileSync(wasmPath);

// Stand-in for OPFS: same interface, a Map instead of a browser.
const files = new Map();
// Synchronous, exactly like the worker: C blocks, so the seam cannot await.
// HTTP goes through a subprocess because node has no synchronous fetch — the
// browser uses synchronous XHR in the worker instead.
const storage = {
	http({ method, url, headers, body }) {
		const args = ['-s', '-X', method || 'GET', url];
		for (const k in headers) args.push('-H', `${k}: ${headers[k]}`);
		if (body) args.push('--data-binary', body);
		const r = spawnSync('curl', args, { encoding: 'utf8', maxBuffer: 32 * 1024 * 1024 });
		return r.stdout ?? '';
	},
	read: (path) => (files.has(path) ? files.get(path) : null),
	write(path, data, append) {
		files.set(path, append && files.has(path) ? files.get(path) + data : data);
		return true;
	},
	stat(path) {
		if (files.has(path)) return { size: files.get(path).length, mtime: 0, isDir: false };
		for (const k of files.keys()) if (k.startsWith(path + '/')) return { size: 0, mtime: 0, isDir: true };
		return null;
	},
	mkdirp: () => true,
	remove: (path) => files.delete(path),
};

// Feed the module stdin so it runs something, rather than hitting EOF at the
// first prompt: `node tools/run-node.mjs hako.wasm ':help' 'hello'`
const script = process.argv.slice(3).map((l) => l + '\n').join('');
let stdinPos = 0;

const out = [];
const log = (fd, line) => { out.push(line); console.log((fd === 2 ? 'stderr| ' : 'stdout| ') + line); };

const memory = { buffer: new ArrayBuffer(0) };   // replaced by the module's own
const imports = hakoImports({ memory, storage, log });

// argv: ['hako','--pipe'] so the module comes up speaking JSONL, exactly as the
// browser worker starts it.
const wargv = ['hako', '--pipe'];
const encA = new TextEncoder();
imports.wasi_snapshot_preview1.args_sizes_get = (c, sz) => {
	const dv = new DataView(instance.exports.memory.buffer);
	dv.setUint32(c, wargv.length, true);
	dv.setUint32(sz, wargv.reduce((n, a) => n + encA.encode(a).length + 1, 0), true);
	return 0;
};
imports.wasi_snapshot_preview1.args_get = (ptrs, buf) => {
	const dv = new DataView(instance.exports.memory.buffer);
	const b = new Uint8Array(instance.exports.memory.buffer);
	let p = buf;
	wargv.forEach((a, i) => { dv.setUint32(ptrs + i * 4, p, true); const e = encA.encode(a + '\0'); b.set(e, p); p += e.length; });
	return 0;
};

// Replace the EOF-only stdin with the scripted one.
imports.wasi_snapshot_preview1.fd_read = (fd, iovs, iovsLen, nread) => {
	const mem = new DataView(instance.exports.memory.buffer);
	const b = new Uint8Array(instance.exports.memory.buffer);
	let wrote = 0;
	for (let i = 0; i < iovsLen && stdinPos < script.length; i++) {
		const p = mem.getUint32(iovs + i * 8, true);
		const l = mem.getUint32(iovs + i * 8 + 4, true);
		const chunk = script.slice(stdinPos, stdinPos + l);
		const enc = new TextEncoder().encode(chunk);
		b.set(enc, p);
		stdinPos += chunk.length;
		wrote += enc.length;
	}
	mem.setUint32(nread, wrote, true);
	return 0;
};

let instance;
try {
	const { instance: inst } = await WebAssembly.instantiate(bytes, {
		wasi_snapshot_preview1: imports.wasi_snapshot_preview1,
		env: imports.env,
	});
	instance = inst;
} catch (e) {
	console.log('INSTANTIATE FAILED:', e.message);
	process.exit(1);
}

// The module exports its own memory; point the glue at it before any call.
Object.defineProperty(memory, 'buffer', { get: () => instance.exports.memory.buffer });
imports.setMalloc(instance.exports.malloc);

console.log('instantiated. exports:', Object.keys(instance.exports).slice(0, 12).join(', '));

try {
	const rc = instance.exports.main ? instance.exports.main(0, 0) : instance.exports._start();
	console.log('main returned', rc);
} catch (e) {
	if (e instanceof HakoExit) console.log('exited with', e.code);
	else console.log('TRAP:', e.message.split('\n')[0]);
}
console.log(`\n${out.length} lines of output`);
