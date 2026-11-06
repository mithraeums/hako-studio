// An image picked in the browser build must reach the provider as the same bytes.
//
// The worker stages the picked image in the store and the prompt names its path;
// the agent attaches it (hkImagesFromLine, the path hake uses). Through the store
// that only kept text, the PNG's bytes came back as U+FFFD, the agent's magic check
// refused them, and the model got the words without the picture. Also: a binary
// file already in OPFS must load back as bytes, and text must stay text.
//
//   node tools/image-test.js [hako.wasm]

import { readFileSync, existsSync } from 'node:fs';
import { hakoImports, HakoExit, opfsStorage } from './hako-glue.js';

const wasm = readFileSync(process.argv[2] || (existsSync('hako.wasm') ? 'hako.wasm' : 'dist/hako.wasm'));
let failed = 0;
const check = (ok, what) => { console.log((ok ? 'ok   ' : 'FAIL ') + what); if (!ok) failed = 1; };

/* a 1x1 png: magic, IHDR, IDAT, IEND, NULs throughout like any real image */
const png = Uint8Array.from(Buffer.from(
	'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==', 'base64'));
const b64 = Buffer.from(png).toString('base64');

/* OPFS, faked: what a returning visitor's store holds */
const entry = (name, data) => [name, { kind: 'file', getFile: async () => {
	const f = new Blob([data]);
	return { size: f.size, lastModified: 0, arrayBuffer: () => f.arrayBuffer(), text: () => f.text() };
} }];
const kept = [entry('old.png', png), entry('note.txt', 'é ünïcode')];
Object.defineProperty(globalThis, 'navigator', { configurable: true, value: { storage: { getDirectory: async () => ({
	entries: async function* () {
		yield ['hako', { kind: 'directory', entries: async function* () { for (const e of kept) yield e; } }];
	},
}) } } });

const store = await opfsStorage();
const back = store.read('/hako/old.png');
check(back instanceof Uint8Array && Buffer.from(back).equals(Buffer.from(png)), 'a stored image loads back as its bytes');
check(store.read('/hako/note.txt') === 'é ünïcode', 'a stored text file loads back as text');

/* the provider: records what it was sent */
const sent = [];
store.http = (req) => {
	sent.push(req.body || '');
	return 'data: {"choices":[{"delta":{"content":"a dot"}}]}\n\ndata: [DONE]\n\n';
};
store.mkdirp('/workspace');
store.write('/hako/.hakorc', 'ai_provider=openai\nai_api_key=sk-test\nai_model=gpt-4o\n', false);

function turn(lines) {
	const out = [];
	const memory = { buffer: new ArrayBuffer(0) };
	const imports = hakoImports({ memory, storage: store, log: (fd, l) => out.push(l) });
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
	const inst = new WebAssembly.Instance(new WebAssembly.Module(wasm), {
		wasi_snapshot_preview1: imports.wasi_snapshot_preview1, env: imports.env,
	});
	Object.defineProperty(memory, 'buffer', { get: () => inst.exports.memory.buffer });
	imports.setMalloc(inst.exports.malloc);
	try { inst.exports._start(); } catch (e) { if (!(e instanceof HakoExit)) throw e; }
	return out;
}

/* what the worker does on 'image': the bytes go in as they are */
const staged = '/hako/.hako/inbox/test.png';
store.mkdirp('/hako/.hako/inbox');
store.write(staged, png, false);

const out = turn([JSON.stringify({ type: 'prompt', text: 'what is this ' + staged })]);
const body = sent.find((b) => b.includes('what is this')) || '';
check(body.includes('data:image/png;base64,' + b64), 'the provider gets the image, byte for byte');
check(!body.includes(staged), 'the prompt carries [image #N], not the staged path');
const logged = [...store.list('/hako/.hako/projects')].filter((p) => p.endsWith('.png'));
check(logged.length === 1 && Buffer.from(store.read(logged[0])).equals(Buffer.from(png)), 'the session keeps its own copy of the image');
if (failed) console.log(out.join('\n'));

/* a text write is still text: sessions and the rc are read as strings */
const jsonl = [...store.list('/hako/.hako/projects')].find((p) => p.endsWith('.jsonl'));
check(typeof store.read(jsonl) === 'string', 'a session log stays text');

console.log(failed ? 'image-test: FAIL' : 'image-test: ok');
process.exit(failed);
