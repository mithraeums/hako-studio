// Two turns, one store: the session must survive the instance that made it.
//
// Natively the process persists and the session id with it. In the browser each
// turn is a fresh module, so continuity depends on the worker resuming — and a
// regression here silently costs the model its memory. Cheap to check, so check.
//
//   node tools/session-test.js [hako.wasm]

import { readFileSync, existsSync } from 'node:fs';
import { hakoImports, HakoExit } from './hako-glue.js';

const bytes = readFileSync(process.argv[2] || (existsSync('hako.wasm') ? 'hako.wasm' : 'dist/hako.wasm'));
const files = new Map();
const dirs = new Set(['/', '/workspace']);
const storage = {
	http: () => '',
	read: (p) => (files.has(p) ? files.get(p) : null),
	write(p, d, a) { files.set(p, a && files.has(p) ? files.get(p) + d : d); return true; },
	stat(p) {
		if (files.has(p)) return { size: files.get(p).length, mtime: 0, isDir: false };
		if (dirs.has(p)) return { size: 0, mtime: 0, isDir: true };
		for (const k of files.keys()) if (k.startsWith(p + '/')) return { size: 0, mtime: 0, isDir: true };
		return null;
	},
	mkdirp: (p) => (dirs.add(p), true),
	remove: (p) => files.delete(p),
	list: (p) => [...files.keys()].filter((f) => f.startsWith(p + '/')),
};

function turn(lines) {
	const out = [];
	const memory = { buffer: new ArrayBuffer(0) };
	const imports = hakoImports({ memory, storage, log: (fd, l) => { if (fd === 1) out.push(l); } });
	const script = lines.map((l) => l + '\n').join('');
	let pos = 0;
	const enc = new TextEncoder();
	const argv = ['hako', '--pipe', '--dir', '/workspace'];
	imports.wasi_snapshot_preview1.args_sizes_get = (c, sz) => {
		const dv = new DataView(inst.exports.memory.buffer);
		dv.setUint32(c, argv.length, true);
		dv.setUint32(sz, argv.reduce((n, a) => n + a.length + 1, 0), true);
		return 0;
	};
	imports.wasi_snapshot_preview1.args_get = (ptrs, buf) => {
		const dv = new DataView(inst.exports.memory.buffer);
		const b = new Uint8Array(inst.exports.memory.buffer);
		let p = buf;
		argv.forEach((a, i) => { dv.setUint32(ptrs + i * 4, p, true); const e = enc.encode(a + '\0'); b.set(e, p); p += e.length; });
		return 0;
	};
	imports.wasi_snapshot_preview1.fd_read = (fd, iovs, n, nread) => {
		const dv = new DataView(inst.exports.memory.buffer);
		const b = new Uint8Array(inst.exports.memory.buffer);
		let wrote = 0;
		for (let i = 0; i < n && pos < script.length; i++) {
			const p = dv.getUint32(iovs + i * 8, true);
			const l = dv.getUint32(iovs + i * 8 + 4, true);
			const chunk = script.slice(pos, pos + l);
			const e = enc.encode(chunk);
			b.set(e, p); pos += chunk.length; wrote += e.length;
		}
		dv.setUint32(nread, wrote, true);
		return 0;
	};
	const inst = new WebAssembly.Instance(new WebAssembly.Module(bytes), {
		wasi_snapshot_preview1: imports.wasi_snapshot_preview1, env: imports.env,
	});
	Object.defineProperty(memory, 'buffer', { get: () => inst.exports.memory.buffer });
	imports.setMalloc(inst.exports.malloc);
	try { inst.exports._start(); } catch (e) { if (!(e instanceof HakoExit)) throw e; }
	return out;
}

const S = (lines) => (lines.join('\n').match(/"session":"([^"]+)"/) || [])[1];

const first = turn([JSON.stringify({ type: 'slash', cmd: ':session' })]);
const id = S(first);
if (!id) { console.log(first.join('\n')); throw new Error('FAIL: no session id in init'); }

// Turn two does what the worker does: resume, then work.
const second = turn([
	JSON.stringify({ type: 'slash', cmd: ':resume ' + id }),
	JSON.stringify({ type: 'slash', cmd: ':session' }),
]);
const said = second.join('\n');
if (!said.includes('id:      ' + id)) {
	console.log(said);
	throw new Error('FAIL: turn two did not stay in session ' + id);
}
if (!said.includes('resumed')) throw new Error('FAIL: turn two reports a new session, not a resumed one');

// And the transcript is one file, not one per turn.
// A reload restarts the worker with no session id. The agent must pick its own
// session back up from state, or every refresh silently starts a blank
// conversation and the model answers with no memory of anything.
const third = turn([JSON.stringify({ type: 'slash', cmd: ':session' })]);
const said3 = third.join('\n');
if (!said3.includes('"session":"' + id + '"')) {
	console.log(said3);
	throw new Error('FAIL: a fresh instance did not resume ' + id + ' from state');
}
if (!said3.includes('"resumed":1')) throw new Error('FAIL: fresh instance reports a new session');

const logs = storage.list('/hako/.hako/projects').filter((p) => p.endsWith('.jsonl'));
const proj = [...files.keys()].filter((k) => k.includes('/projects/'));
if (!proj.some((k) => k.includes('workspace')))
	throw new Error('FAIL: --dir ignored, project state at ' + proj.join(' '));
console.log(`session-test: ok (session ${id} survived, ${logs.length} transcript file(s))`);
