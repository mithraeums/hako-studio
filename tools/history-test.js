const fs = require('fs');
const html = fs.readFileSync('index.html', 'utf8');
const src = html.split('<script>')[1].split('</script>')[0];
const ids = [...html.matchAll(/id="([^"]+)"/g)].map(m => m[1]);

const rendered = [];
const mkEl = (id) => {
	const el = {
		id, style: {}, dataset: {}, children: [], hidden: false, _text: '', title: '',
		classList: { _c: new Set(), add(c){this._c.add(c);}, remove(c){this._c.delete(c);},
		             toggle(){}, contains(c){ return this._c.has(c); } },
		get textContent(){ return this._text; },
		set textContent(v){ this._text = v; this.children = []; },
		appendChild(c){ this.children.push(c); if (this.id === 'log') rendered.push(c); return c; },
		append(...c){ this.children.push(...c); },
		insertBefore(c){ this.children.push(c); if (this.id === 'log') rendered.push(c); return c; },
		remove(){}, addEventListener(){}, removeEventListener(){}, focus(){}, select(){},
		click(){}, setProperty(){}, dispatchEvent(){}, querySelectorAll(){ return []; },
		scrollIntoView(){}, getBoundingClientRect(){ return {}; }, parentNode: null,
	};
	return el;
};
const els = new Map(ids.map(i => [i, mkEl(i)]));
const body = mkEl('body');
global.document = {
	getElementById: (id) => els.get(id) || null,
	createElement: () => mkEl('new'),
	createTextNode: (t) => ({ nodeText: t }),
	addEventListener(){}, removeEventListener(){},
	body, documentElement: { style: { setProperty(){} } }, querySelectorAll: () => [],
};
global.window = { matchMedia: () => ({matches:false}), visualViewport:null, innerHeight:800, addEventListener(){} };
global.localStorage = { getItem:()=>null, setItem(){}, removeItem(){} };
global.EventSource = function(){ return {}; };
global.setTimeout = (fn)=>0; global.setInterval=()=>0; global.clearInterval=()=>{};
global.Event = function(){};

// real stored transcript
const { execSync } = require('child_process');
const file = execSync('ls -t ~/.hako/projects/-private-tmp-uptest/sessions/*.jsonl | head -1').toString().trim();
const msgs = fs.readFileSync(file,'utf8').split('\n').filter(Boolean).map(l => { try { return JSON.parse(l); } catch { return null; } }).filter(Boolean);
global.fetch = async () => ({ json: async () => ({ messages: msgs }), ok: true });

const fn = new Function(src + '\n;return { renderHistory };');
const api = fn();
api.renderHistory('test').then(() => {
	const texts = rendered.map(r => {
		const own = r._text || '';
		const kids = (r.children || []).map(k => k.nodeText || k._text || '').join('');
		return ((r.classList && [...r.classList._c].join(' ')) + ' | ' + (own + kids)).trim();
	});
	const raw = texts.filter(t => /<tool|<parameter|<observation|<invoke/.test(t));
	console.log('rendered rows:', texts.length);
	console.log('rows still containing raw markup:', raw.length);
	texts.slice(0, 12).forEach(t => console.log('  ' + t.slice(0, 100).replace(/\n/g, ' | ')));
	if (raw.length) { console.log('FAIL sample:', raw[0].slice(0, 200)); process.exit(1); }
	console.log('history render: CLEAN');
});
