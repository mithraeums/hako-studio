/* Load the page's script in a stub DOM and report anything that throws at load
   or when a control is clicked. Not a test framework — a tripwire for the class
   of bug that only shows up as "nothing happens when I tap it". */
const fs = require('fs');
const html = fs.readFileSync(process.argv[2] || 'index.html', 'utf8');
// Every inline script, in order — the page has more than one now, and testing
// only the first silently tested nothing.
const src = [...html.matchAll(/<script(?:\s[^>]*)?>([\s\S]*?)<\/script>/g)]
	.map((m) => m[1])
	.join('\n;\n');
if (!src.trim()) { console.log('smoke: FAIL — no inline script found'); process.exit(1); }

const ids = [...html.matchAll(/id="([^"]+)"/g)].map(m => m[1]);
const listeners = new Map();
const mkEl = (id) => ({
	id, style: {}, classList: { add(){}, remove(){}, toggle(){}, contains(){ return false; } },
	dataset: {}, children: [], hidden: false, textContent: '', value: '', title: '',
	appendChild(c){ this.children.push(c); return c; }, append(...c){ this.children.push(...c); },
	insertBefore(c){ this.children.push(c); return c; }, remove(){},
	addEventListener(ev, fn){ listeners.set(id + ':' + ev, fn); },
	removeEventListener(){}, focus(){}, select(){}, click(){}, scrollIntoView(){},
	setProperty(){}, dispatchEvent(){}, querySelectorAll(){ return []; },
	getBoundingClientRect(){ return {}; }, parentNode: null,
});
const els = new Map(ids.map(i => [i, mkEl(i)]));
const body = mkEl('body'); body.parentNode = body;
els.forEach(e => { e.parentNode = body; });

global.document = {
	getElementById: (id) => els.get(id) || null,
	createElement: () => mkEl('new'),
	createTextNode: (t) => ({ text: t }),
	addEventListener(){}, removeEventListener(){},
	body, documentElement: { style: { setProperty(){} } },
	querySelector: () => null,
	querySelectorAll: () => [],
};
global.window = { matchMedia: () => ({ matches: false }), visualViewport: null, innerHeight: 800,
                  addEventListener(){} };
global.localStorage = { getItem: () => null, setItem(){}, removeItem(){} };
global.fetch = async () => ({ json: async () => ({}), ok: true });
global.EventSource = function(){ return { onopen: null, onerror: null, onmessage: null }; };
global.setTimeout = (fn) => 0;
global.setInterval = () => 0;
global.clearInterval = () => {};
global.Event = function(){};

let failed = 0;
try { new Function(src)(); }
catch (e) { console.log('LOAD FAILED: ' + e.message); failed = 1; }

if (!failed) {
	for (const key of [...listeners.keys()].filter(k => k.endsWith(':click'))) {
		try { listeners.get(key)({ preventDefault(){}, target: null, stopPropagation(){} }); }
		catch (e) { console.log('CLICK ' + key + ' FAILED: ' + e.message); failed = 1; }
	}
}
// A page that binds nothing is a page that broke, not a page that passed.
if (!failed && listeners.size < 5) {
	console.log('smoke: FAIL — only ' + listeners.size + ' handlers bound');
	failed = 1;
}
console.log(failed ? 'smoke: FAIL' : 'smoke: ok (' + listeners.size + ' handlers bound)');
process.exit(failed);
