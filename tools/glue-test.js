// The seam hands C a buffer it allocated with the module's malloc, and malloc
// can grow memory — which detaches every view made before it. A view taken too
// early worked on small files and failed on a long session ("Underlying
// ArrayBuffer has been detached"). Here malloc grows memory on every call, so a
// view held across an allocation fails at once.
//
//   node tools/glue-test.js

import { hakoImports } from './hako-glue.js';

const memory = new WebAssembly.Memory({ initial: 1 });
const text = 'é'.repeat(70000) + ' end';
const storage = { read: (p) => (p === '/big.jsonl' ? text : null) };
const { env, setMalloc } = hakoImports({ memory, storage, log: () => {} });
setMalloc((n) => {
	const at = memory.buffer.byteLength;
	memory.grow(Math.ceil((n + 1) / 65536));
	return at;
});

const enc = new TextEncoder(), dec = new TextDecoder();
new Uint8Array(memory.buffer).set(enc.encode('/big.jsonl\0'), 16);
let ok = true;
try {
	if (env.hkjs_fs_read(16, 64, 68) !== 0) throw new Error('read failed');
	const dv = new DataView(memory.buffer);
	const ptr = dv.getUint32(64, true), len = dv.getUint32(68, true);
	const got = dec.decode(new Uint8Array(memory.buffer, ptr, len));
	if (got !== text) throw new Error('wrong bytes back (' + got.length + ' chars)');
	if (env.hkjs_fs_read(16, 64, 68) !== 0) throw new Error('second read failed');
} catch (e) {
	ok = false;
	console.log('FAIL hkjs_fs_read across a memory grow: ' + e.message);
}
if (ok) console.log('ok   hkjs_fs_read survives memory growing under it');
process.exit(ok ? 0 : 1);
