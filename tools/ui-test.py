#!/usr/bin/env python3
"""Browser test of the studio page against a real `hako --serve` and the fake
engine from hako-code (no model, no network). Drives the page the way a person
does: a turn, an approval, the tool's output, the folder picker, two uploads of
one name, a desktop and a phone viewport, and no JS errors anywhere.

Needs python3 + `pip install playwright && playwright install chromium`, and a
built ../hako-code/hako. Usage: python3 tools/ui-test.py [path/to/hako]"""
import os, sys, time, json, shutil, subprocess, tempfile, urllib.request
try:
    from playwright.sync_api import sync_playwright
except ImportError:
    sys.exit("pip install playwright && playwright install chromium")
R = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
HAKO = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else R + "/hako-code/hako"
FAKE = R + "/hako-code/tools/tty/fakehakm.py"
if not os.path.exists(HAKO): sys.exit("missing hako binary: " + HAKO + " (build hako-code first)")
W = tempfile.mkdtemp(prefix="studio-"); HOME = W + "/home"; PROJ = os.path.realpath(W) + "/proj"
os.makedirs(HOME + "/.hako/models/hako-sho"); os.makedirs(PROJ + "/sub")
open(HOME + "/.hako/models/hako-sho/hako-sho.mlf2", "w").write("fake")
open(HOME + "/.hakorc", "w").write("ai_provider=mithraeum\nai_model=hako-sho\nshow_splash=0\nai_auto_approve=0\n")
open(PROJ + "/alpha.txt", "w").write("a\n"); open(PROJ + "/sub/inner.txt", "w").write("i\n")
slug = PROJ.replace("/", "-"); os.makedirs(HOME + "/.hako/projects/" + slug)
open(HOME + "/.hako/projects/" + slug + "/trust", "w").write("granted=%d\n" % int(time.time()))
P = 18795; U = "http://127.0.0.1:%d" % P
srv = subprocess.Popen([HAKO, "--serve", "--web", R + "/hako-studio", "--port", str(P), "--dir", PROJ],
                       env=dict(os.environ, HOME=HOME, HAKO_HAKM=FAKE), stdin=subprocess.DEVNULL,
                       stdout=open(W + "/serve.out", "w"), stderr=subprocess.STDOUT)
time.sleep(1.2)
import atexit; atexit.register(srv.kill)    # a failed run must not leave its server holding the port
res = []
def ck(c, what): res.append(("ok  " if c else "FAIL") + " " + what)
def idle(pg, secs=20):
    end = time.time() + secs
    while time.time() < end:
        for btn in pg.locator('.ask:not(.settled) .row button.primary').all():
            try: btn.click()
            except Exception: pass
        if pg.inner_text('#send').strip() == 'send' and not pg.locator('.ask:not(.settled)').count(): return True
        pg.wait_for_timeout(200)
    return False
st = json.load(urllib.request.urlopen(U + "/api/state"))
ck(all(k in st for k in ("version", "dir", "provider")), f"/api/state has version/dir/provider ({st.get('version')}, {st.get('provider')})")
with sync_playwright() as p:
    b = p.chromium.launch()
    # a home-screen app on iOS 26 (428x926): the window can claim the whole screen while fixed layers stop a status bar short
    pg = b.new_page(viewport={'width': 428, 'height': 879}, has_touch=True)
    pg.add_init_script("Object.defineProperty(navigator, 'standalone', { get: () => true }); Object.defineProperty(screen, 'height', { get: () => 926 }); Object.defineProperty(window, 'innerHeight', { get: () => 926 });")
    pg.goto(U); pg.wait_for_timeout(1000)
    ck(pg.evaluate("getComputedStyle(document.documentElement).getPropertyValue('--vh')") == '879px'
       and pg.evaluate("document.body.classList.contains('app')"), "home-screen app: the page is as tall as a fixed layer reaches, whatever the window claims")
    ck(pg.evaluate("""[...document.styleSheets].some((s) => [...s.cssRules].some((r) => r.media && r.media.mediaText.includes('display-mode: standalone')
        && [...r.cssRules].some((c) => c.selectorText === 'html' && c.style.height === '100vh')))"""), "home-screen app: the root height is 100vh, not 100% or dvh")
    ck('#probe' not in pg.content(), "the viewport probe is gone")
    ck(pg.evaluate("parseFloat(getComputedStyle(document.querySelector('header')).paddingTop)") >= 26, "home-screen app: the header clears the blur under the status bar")
    pg.close()
    # 2026-11-17, a day on the phone: a reply's markdown went in after add() scrolled, so once
    # the chat overflowed it stopped following, and a reconnect rebuilt it at its top.
    # The page's own event handler, fed a chat: no engine, nothing timed.
    pg = b.new_page(viewport={'width': 390, 'height': 844}, has_touch=True); pg.goto(U); pg.wait_for_timeout(1000)
    feed = """(n) => { for (let i = 0; i < n; i++) {
        esMessage({ data: JSON.stringify({ type: 'message', role: 'user', text: 'question ' + i }) });
        esMessage({ data: JSON.stringify({ type: 'message', role: 'ai', text: 'Okay, here is the list:\\n\\n* `a`\\n* `b`\\n* `c`\\n\\nWhat next?' }) });
        esMessage({ data: JSON.stringify({ type: 'done' }) }); } }"""
    gap = lambda: pg.evaluate("$('log').scrollHeight - $('log').scrollTop - $('log').clientHeight")
    pg.evaluate(feed, 10); pg.wait_for_timeout(100)
    tall = pg.evaluate("[$('log').scrollHeight, $('log').clientHeight]")
    ck(tall[0] > tall[1] + 300 and gap() < 40, "a long chat follows each reply to its end (%d px from the bottom, %d tall in %d)" % (gap(), *tall))
    pg.evaluate("$('log').scrollTop = 100"); pg.wait_for_timeout(100)
    pg.evaluate(feed, 1); pg.wait_for_timeout(100)
    ck(pg.evaluate("$('log').scrollTop") < 400, "scrolled up to read, a new reply leaves the reader where they are")
    pg.evaluate("clearLog()"); pg.evaluate(feed, 11); pg.wait_for_timeout(100)
    ck(gap() < 40, "a reconnect (the chat cleared and replayed) rebuilds it at its end, not its top (%d px)" % gap())
    # 11-23: the plan's usage as a ring (Claude's): the fullest window leads, a tap lists them all
    ck(pg.locator('#usage').is_hidden(), "usage: no ring before the provider says anything")
    now = int(time.time())
    st = lambda L, t='limits': pg.evaluate("([L, t]) => esMessage({ data: JSON.stringify({ type: t, limits: L }) })", [L, t])
    st({"main": 0, "note": "", "at": now, "windows": [{"name": "5h", "pct": 31, "reset": now + 7200}, {"name": "week", "pct": 5, "reset": now + 400000}]}, 'state')
    pg.wait_for_timeout(100)
    ck(pg.locator('#usage').is_visible() and pg.inner_text('#usage .n') == "31" and "5h" in pg.get_attribute('#usage', 'title'),
       "usage: the ring shows the 5 hours while the week is low")
    st({"main": 1, "note": "", "at": now, "windows": [{"name": "5h", "pct": 12, "reset": now + 7200}, {"name": "week", "pct": 93, "reset": now + 40000}]})
    pg.wait_for_timeout(100)
    ck(pg.inner_text('#usage .n') == "93" and "warn" in pg.get_attribute('#usage', 'class'), "usage: a week nearly gone leads, in the warning color")
    pg.click('#usage'); pg.wait_for_timeout(150)
    rows = pg.locator('.modal .lim .nm').all_inner_texts()
    ck(rows == ["week", "5h"] and "11h" in pg.inner_text('.modal .lim'), f"usage: a tap lists every window, the headline first, with its reset ({rows})")
    pg.keyboard.press('Escape'); pg.wait_for_timeout(100)
    st({"main": 0, "note": "", "at": now, "windows": [{"name": "5h", "pct": 70, "reset": now - 5}, {"name": "week", "pct": 40, "reset": now + 40000}]})
    pg.wait_for_timeout(100)
    ck(pg.inner_text('#usage .n') == "40", "usage: a window past its reset gives way to the next fullest")
    st({"main": -1, "note": "$4.21 left", "at": now, "windows": []})
    pg.wait_for_timeout(100)
    ck(pg.inner_text('#usage') == "$4.21 left", "usage: a prepaid key shows its balance")
    st(None); pg.wait_for_timeout(100)
    ck(pg.locator('#usage').is_hidden(), "usage: another sign-in with nothing to say clears the ring")
    # 11-18: mid-turn the button only stopped, so nothing could be said until the turn
    # ended, and :mode auto waited behind the approvals it was for. Posts recorded, not sent.
    pg.evaluate("""window.__posts = []; window.fetch = (p, o) => { if (o && o.method === "POST") __posts.push([p, o.body]); return Promise.resolve(new Response('{"ok":true}')); };
        caps.add('stop'); setBusy(true);""")
    pg.fill('#input', 'and then the tests')
    ck(pg.inner_text('#send').strip() == 'queue', "mid-turn, something typed makes the button queue, not stop (%r)" % pg.inner_text('#send'))
    pg.press('#input', 'Enter'); pg.wait_for_timeout(100)
    ck(pg.is_visible('#queued') and 'and then the tests' in pg.inner_text('#queued') and not pg.input_value('#input')
       and not pg.evaluate("__posts.length"), "Enter queues it on the page and sends nothing yet")
    ck(pg.inner_text('#send').strip() == 'stop', "the field empty again, the button stops")
    pg.fill('#input', ':mode auto'); pg.press('#input', 'Enter'); pg.wait_for_timeout(100)
    ck(pg.evaluate("JSON.stringify(__posts)") == '[["/api/slash",":mode auto"]]', "a command mid-turn goes now (%s)" % pg.evaluate("JSON.stringify(__posts)"))
    pg.evaluate("__posts.length = 0; esMessage({ data: JSON.stringify({ type: 'done' }) })"); pg.wait_for_timeout(100)
    ck(pg.evaluate("JSON.stringify(__posts)") == '[["/api/prompt","and then the tests"]]' and not pg.is_visible('#queued'),
       "the turn's done sends what was queued (%s)" % pg.evaluate("JSON.stringify(__posts)"))
    pg.evaluate("__posts.length = 0; caps.add('steer'); setBusy(true)"); pg.fill('#input', 'use tabs'); pg.press('#input', 'Enter')
    if pg.locator('#queued .go').count(): pg.click('#queued .go')
    pg.wait_for_timeout(100)
    ck(pg.evaluate("JSON.stringify(__posts)") == '[["/api/steer","use tabs"]]' and not pg.is_visible('#queued'),
       "send now goes into the running turn when hako steers (%s)" % pg.evaluate("JSON.stringify(__posts)"))
    pg.evaluate("__posts.length = 0; esMessage({ data: JSON.stringify({ type: 'done' }) })"); pg.wait_for_timeout(100)
    ck(pg.evaluate("__posts.length") == 0, "and its done sends nothing more")
    pg.evaluate("__posts.length = 0; caps.delete('steer'); setBusy(true)"); pg.fill('#input', 'stop and do this'); pg.press('#input', 'Enter')
    if pg.locator('#queued .go').count(): pg.click('#queued .go')
    pg.wait_for_timeout(100)
    ck(pg.evaluate("JSON.stringify(__posts)") == '[["/api/stop",""]]', "without steer, send now stops the turn (%s)" % pg.evaluate("JSON.stringify(__posts)"))
    pg.evaluate("__posts.length = 0; esMessage({ data: JSON.stringify({ type: 'done' }) })"); pg.wait_for_timeout(100)
    ck(pg.evaluate("JSON.stringify(__posts)") == '[["/api/prompt","stop and do this"]]', "and its done sends it")
    # 11-18: a turn's tool calls fold into one row, so the reply reads as the main thing
    pg.evaluate(r'''clearLog(); const f = (o) => esMessage({ data: JSON.stringify(o) });
        f({ type: 'message', role: 'user', text: 'look around' });
        f({ type: 'tool_start', display: 'list_dir .' }); f({ type: 'tool_end', display: '3 entries' }); f({ type: 'tool_result', text: 'a\nb' });
        f({ type: 'tool_start', display: 'read_file a' }); f({ type: 'tool_end', display: 'error: no such file' });
        f({ type: 'message', role: 'ai', by: 'sho', text: 'Here:\n\n```sh\nls -la\n```' }); f({ type: 'done' });''')
    head = pg.inner_text('#log > .work .whead') if pg.locator('#log > .work').count() else ''
    ck(pg.locator('#log > .work').count() == 1 and not pg.locator('#log > .tool').count()
       and 'worked' in head and '2 steps' in head and '1 failed' in head, "a turn's tool calls fold into one row (%r)" % head)
    ck(not pg.is_visible('#log > .work .wbody'), "folded until asked")
    if pg.locator('#log > .work .whead').count(): pg.click('#log > .work .whead')
    pg.wait_for_timeout(100)
    ck(pg.locator('#log > .work .wbody > .tool').count() == 5 and pg.is_visible('#log > .work .wbody'), "and opens to show each step")
    ck(pg.evaluate("[...$('log').children].map((e) => e.className.split(' ')[0]).join(',')") == 'turn,work,turn',
       "the reply follows the work, in order (%s)" % pg.evaluate("[...$('log').children].map((e) => e.className).join(' | ')"))
    ck(pg.locator('.model .codebox > .copy').count() == 1, "a code block in a reply has a copy button")
    ck('sho' in pg.inner_text('.model .who').lower() and pg.locator('.model .who .av').count() == 1, "a reply is marked with the hako that gave it")
    pg.evaluate("esMessage({ data: JSON.stringify({ type: 'message', role: 'user', by: 'koi', to: 'sho', text: 'q' }) })")
    ck(pg.locator('.user.peer').count() == 1 and 'koi' in pg.inner_text('.user.peer .who').lower() and pg.inner_text('#send').strip() == 'send',
       "a line asked at another hako's pane shows as theirs, and starts no turn here")
    pg.evaluate("esMessage({ data: JSON.stringify({ type: 'message', role: 'user', text: ':say lunch?' }) }); esMessage({ data: JSON.stringify({ type: 'message', role: 'chat', by: 'koi', text: 'lunch?' }) })")
    ck(pg.locator('.turn.chat').count() == 1 and 'koi' in pg.inner_text('.turn.chat .who').lower() and ':say' not in pg.inner_text('#log'),
       "a :say line shows once, as a line between people, not as the command")
    pg.evaluate("agentsNow = { me: 'sho', route: '', agents: [{ name: 'sho', me: true, state: 'busy' }, { name: 'koi' }] }; state.watching = 3; renderPresence()")
    ck(pg.locator('#presence .av').count() == 2 and pg.locator('#presence .av.busy').count() == 1 and '3 open' in pg.inner_text('#presence'),
       "the header shows who is on the chat, who is working, and how many pages are open")
    pg.evaluate("settings.backdrop = 'stars'; paintBackdrop()")
    ck(pg.evaluate("$('backdrop').dataset.bg") == 'stars' and pg.evaluate("document.body.classList.contains('grain')"), "a background is drawn, grain on by default")
    pg.evaluate("settings.grain = false; settings.backdrop = 'image'; paintBackdrop()")
    ck(pg.evaluate("$('backdrop').dataset.bg") == '' and not pg.evaluate("document.body.classList.contains('grain')"), "an image never picked falls back to plain, and grain turns off")
    pg.evaluate("settings.grain = true; settings.backdrop = 'none'; saveSettings()")
    # 11-19: a call that asked first stays one row once answered (a real turn split it in two, the box between)
    pg.evaluate(r'''window.__posts = []; window.fetch = (p, o) => Promise.resolve(new Response('{"ok":true}'));
        const f = (o) => esMessage({ data: JSON.stringify(o) }); clearLog(); setBusy(true);
        f({ type: 'message', role: 'user', text: 'read it' });
        f({ type: 'tool_start', display: 'read_file main.c' });
        f({ type: 'tool_request', id: 7, name: 'read_file', subject: 'main.c', kind: 'read' });''')
    ck(pg.is_visible('#log > .ask .row button.primary'), "the open approval shows in the chat")
    pg.click('#log > .ask .row button.primary'); pg.wait_for_timeout(50)
    pg.evaluate(r'''const f = (o) => esMessage({ data: JSON.stringify(o) });
        f({ type: 'tool_end', display: '12 lines' }); f({ type: 'tool_result', text: 'int main' });
        f({ type: 'message', role: 'ai', text: 'It prints primes.' }); f({ type: 'done' });''')
    head = pg.inner_text('#log > .work .whead') if pg.locator('#log > .work').count() else ''
    ck(pg.locator('#log > .work').count() == 1 and not pg.locator('#log > .ask').count()
       and pg.locator('#log > .work .wbody .ask.settled').count() == 1 and '1 step' in head,
       "an answered approval folds into its turn's one row (%d rows, %r)" % (pg.locator('#log > .work').count(), head))
    pg.close()
    for name, vp in (("desk", {'width': 1280, 'height': 800}), ("phone", {'width': 390, 'height': 844})):
        pg = b.new_page(viewport=vp, has_touch=(name == "phone"))
        errs = []; pg.on('pageerror', lambda e: errs.append(str(e)))
        pg.on('console', lambda m: errs.append(m.text) if m.type == 'error' else None)
        if name == "phone":   # a stand-in visualViewport, so the test can open "the keyboard" the way iOS does
            pg.add_init_script("""window.__vv = Object.assign(new EventTarget(), { width: 390, height: 844, offsetTop: 0, offsetLeft: 0, scale: 1 });
                Object.defineProperty(window, 'visualViewport', { get: () => window.__vv, configurable: true });""")
        pg.goto(U); pg.wait_for_timeout(1500)
        ck(not errs, f"[{name}] page loads with no JS errors {errs[:2]}")
        ck(pg.locator('#tabbar').is_visible() == (name == "phone"), f"[{name}] the tab bar is a phone's only")
        ck(pg.evaluate("document.documentElement.scrollWidth") <= vp['width'], f"[{name}] no sideways scroll")
        # a touch screen reacts to taps, not hovers: every :hover rule is for pointers only
        loose = pg.evaluate("""(() => { const out = [];
            const walk = (rules, gated) => { for (const r of rules) {
                if (r.cssRules && r.media) walk(r.cssRules, gated || /hover:\\s*hover/.test(r.media.mediaText));
                else if (r.selectorText && r.selectorText.includes(':hover') && !gated) out.push(r.selectorText); } };
            for (const s of document.styleSheets) walk(s.cssRules, false); return out; })()""")
        ck(not loose, f"[{name}] every :hover style sits in @media (hover: hover), so a tap never leaves one stuck {loose[:3]}")
        if name == "phone":
            # the keyboard opens: iOS shrinks the visible box to 420 and pans it 360 down to show the field
            box = lambda: pg.evaluate("""(() => { const b = document.body.getBoundingClientRect(), f = $('form').getBoundingClientRect();
                return [Math.round(b.top), Math.round(b.bottom), Math.round(f.bottom)]; })()""")
            pg.evaluate("__vv.height = 420; __vv.offsetTop = 360; __vv.dispatchEvent(new Event('resize')); __vv.dispatchEvent(new Event('scroll'))")
            pg.wait_for_timeout(200); kb = box()
            ck(kb[0] == 360 and kb[1] == 780 and kb[2] >= 760,
               f"[phone] with the keyboard open the page fills the visible box, composer at its foot over the keyboard (body {kb[0]}-{kb[1]}, form ends {kb[2]})")
            ck(not pg.locator('#tabbar').is_visible(), "[phone] the tab bar steps aside while the keyboard is up")
            pg.evaluate("__vv.height = 844; __vv.offsetTop = 0; __vv.dispatchEvent(new Event('resize'))")
            pg.wait_for_timeout(200); shut = box()
            bar = pg.evaluate("Math.round($('tabbar').getBoundingClientRect().top)")
            ck(shut[0] == 0 and shut[1] == 844 and abs(shut[2] - bar) <= 1 and bar >= 780,
               f"[phone] closing it puts the composer back at the bottom, on the tab bar (body {shut[0]}-{shut[1]}, form ends {shut[2]}, bar at {bar})")
            # 10-28 on the iPhone: with the keyboard up, a swipe scrolled the DOCUMENT and the fixed
            # page stayed behind, the bar at the top with a keyboard's height of blank under it
            pg.evaluate("__vv.height = 420; __vv.offsetTop = 360; __vv.dispatchEvent(new Event('resize')); __vv.dispatchEvent(new Event('scroll'))")
            pg.wait_for_timeout(150)
            pg.evaluate("""(() => { const d = document.createElement('div'); d.id = 'tall'; d.style.cssText = 'position:absolute;top:0;height:3000px;width:1px';
                document.documentElement.appendChild(d); window.scrollTo(0, 380); })()""")
            pg.wait_for_timeout(250)
            sy = pg.evaluate("window.scrollY"); kb2 = box()
            ck(sy == 0 and kb2[0] == 360 and kb2[2] >= 760,
               f"[phone] a page scroll under the open keyboard is undone, the composer stays on the keyboard (scrollY {sy}, body {kb2[0]}-{kb2[1]}, form ends {kb2[2]})")
            pg.evaluate("__vv.offsetTop = 300; __vv.dispatchEvent(new Event('scroll'))"); pg.wait_for_timeout(150); kb3 = box()
            ck(kb3[0] == 300 and kb3[1] == 720, f"[phone] and it re-pans when the visible box moves (body {kb3[0]}-{kb3[1]})")
            moved = pg.evaluate("""(() => { const go = (el) => { const t = new Touch({ identifier: 1, target: el, clientX: 20, clientY: 20 });
                const e = new TouchEvent('touchmove', { cancelable: true, bubbles: true, touches: [t], targetTouches: [t], changedTouches: [t] });
                el.dispatchEvent(e); return e.defaultPrevented; }; return [go(document.querySelector('header')), go($('log'))]; })()""")
            ck(moved == [True, False], f"[phone] with the keyboard up a swipe on the header cannot move the page, the chat still scrolls ({moved})")
            pg.evaluate("$('tall').remove(); __vv.height = 844; __vv.offsetTop = 0; __vv.dispatchEvent(new Event('resize'))"); pg.wait_for_timeout(150)
            # the menu is a drawer on a phone, and the header always carries the wordmark
            ck(not pg.locator('aside').is_visible() and pg.locator('#burger').is_visible() and pg.locator('#hmark svg').is_visible(),
               "[phone] the menu starts closed, the header shows the burger and the wordmark")
            pg.click('#burger'); pg.wait_for_timeout(350)
            ck(pg.locator('aside').is_visible() and pg.locator('#btn-new').is_visible(), "[phone] the burger opens the menu as a drawer")
            bg = pg.evaluate("getComputedStyle(document.querySelector('aside')).backgroundColor")
            ck(bg.startswith('rgb(') , f"[phone] the drawer is solid, the chat never shows through it ({bg})")
            pg.click('#btn-side'); pg.wait_for_timeout(350)
            ck(not pg.locator('aside').is_visible(), "[phone] its fold button closes it again")
            # the bottom bar: a computer's own page has new, recents, files, you (chat and code are the hosted page's)
            vis = pg.evaluate("[...document.querySelectorAll('#tabbar button')].filter((b) => b.offsetParent).map((b) => b.id)")
            ck(vis == ['tab-new', 'tab-recents', 'tab-files', 'tab-you'], f"[phone] the tab bar offers new, recents, files, you ({vis})")
            pg.click('#tab-recents'); pg.wait_for_timeout(350)
            ck(pg.locator('aside').is_visible(), "[phone] recents in the tab bar opens the menu")
            pg.click('#btn-side'); pg.wait_for_timeout(350)
            pg.click('#tab-files'); pg.wait_for_timeout(500)
            ck(pg.locator('.modal.sheet').is_visible(), "[phone] files in the tab bar opens the folder browser")
            pg.keyboard.press('Escape'); pg.wait_for_timeout(300)
            pg.click('#tab-you'); pg.wait_for_timeout(300)
            ck(pg.locator('.pop button', has_text='settings').is_visible(), "[phone] you in the tab bar offers profile and settings")
            pg.keyboard.press('Escape'); pg.wait_for_timeout(200)
            pg.evaluate("$('modes').hidden = false; renderTabs()")
            vis = pg.evaluate("[...document.querySelectorAll('#tabbar button')].filter((b) => b.offsetParent).map((b) => b.id)")
            ck(vis == ['tab-chat', 'tab-code', 'tab-recents', 'tab-files', 'tab-you'], f"[phone] signed in on the hosted page it offers chat and code in place of new ({vis})")
            pg.evaluate("$('modes').hidden = true; renderTabs()")
            pg.click('#burger'); pg.wait_for_timeout(350); pg.click('#btn-new'); pg.wait_for_timeout(1200)
            ck(pg.locator('body.landing').count() == 1 and pg.locator('#greet').is_visible() and not pg.locator('aside').is_visible()
               and pg.evaluate("document.documentElement.scrollWidth") <= 390,
               "[phone] new (from the drawer) closes it and shows the landing, greeting and all, no sideways scroll")
            pg.evaluate("document.getElementById('title').textContent = 'read main.c and tell me in one sentence what it does'")
            pg.wait_for_timeout(100)
            wm = pg.evaluate("(() => { const s = document.querySelector('#hmark svg'); return s ? s.getBoundingClientRect().right : 0; })()")
            tl = pg.evaluate("document.getElementById('title').getBoundingClientRect().left")
            ck(wm and wm <= tl + 0.5, f"[phone] a long session title never runs under the wordmark ({wm:.0f} <= {tl:.0f})")
            # 11-04 on the iPhone: the keyboard came up over the profile sheet and its name field
            # sat under it: the sheet was laid out on the whole screen, not the visible box, and once
            # the page undoes iOS's scroll the box is back at the top (offset 0, 420 tall)
            pg.evaluate("openProfile()"); pg.wait_for_timeout(200)
            pg.evaluate("__vv.height = 420; __vv.offsetTop = 0; __vv.dispatchEvent(new Event('resize')); __vv.dispatchEvent(new Event('scroll'))")
            pg.wait_for_timeout(200)
            r = pg.evaluate("(() => { const i = document.querySelector('.modal .profile input:not([type=file])'), r = i && i.getBoundingClientRect(); return r && [Math.round(r.top), Math.round(r.bottom)]; })()")
            ck(r and r[0] >= 0 and r[1] <= 420, f"[phone] with the keyboard up the profile name field is above it, in the visible box 0-420 ({r})")
            pg.evaluate("__vv.height = 844; __vv.offsetTop = 0; __vv.dispatchEvent(new Event('resize'))")
            pg.keyboard.press('Escape'); pg.wait_for_timeout(150)
            ck(not errs, f"[phone] no JS errors {errs[:2]}")
            pg.close(); continue
        bg = lambda: pg.evaluate("getComputedStyle(document.body).backgroundColor")
        ck(bg() == "rgb(236, 231, 218)", f"no theme in the rc: the default is mithraeum, the site's paper under a light system ({bg()})")
        pg.emulate_media(color_scheme="dark"); pg.wait_for_timeout(150)
        ck(bg() == "rgb(13, 12, 10)", f"and night when the system goes dark ({bg()})")
        pg.emulate_media(color_scheme="light"); pg.wait_for_timeout(150)
        # light or dark by hand, for mithraeum and terminal alike
        pg.evaluate("settings.appearance = 'dark'; paintTheme('mithraeum')")
        ck(bg() == "rgb(13, 12, 10)", f"appearance dark: night under a light system ({bg()})")
        pg.evaluate("paintTheme('terminal')")
        ck(bg() == "rgb(28, 28, 30)", f"terminal, the plain black and white, has its dark too ({bg()})")
        pg.evaluate("settings.appearance = 'auto'; paintTheme('terminal')")
        ck(bg() == "rgb(255, 255, 255)", f"and its light under a light system on auto ({bg()})")
        pg.evaluate("paintTheme('mithraeum')")
        ck("alpha.txt" in pg.inner_text('#files'), "the landing shows the project's recent files")
        geo = pg.evaluate("""(() => { const g = $('greet').getBoundingClientRect(), f = $('form').getBoundingClientRect(), s = $('stage').getBoundingClientRect();
            return [g.left + g.width / 2, s.left + s.width / 2, f.top, f.bottom, s.top, s.bottom, g.bottom]; })()""")
        ck(pg.locator('body.landing').count() == 1 and abs(geo[0] - geo[1]) < 3 and geo[4] + 0.25 * (geo[5] - geo[4]) < geo[2] < geo[4] + 0.7 * (geo[5] - geo[4]) and geo[6] <= geo[2],
           f"an empty chat is the landing: greeting centered, the composer under it mid-page {[round(x) for x in geo]}")
        part = pg.evaluate("$('greet').dataset.part"); hour = time.localtime().tm_hour
        want = "dawn" if 5 <= hour < 8 else "morning" if 8 <= hour < 12 else "afternoon" if 12 <= hour < 17 else "evening" if 17 <= hour < 22 else "night"
        gt = pg.inner_text('#greet').strip()
        ck(part == want and gt and gt == gt.lower(), f"the greeting is for the hour ({part}) and lowercase ({gt!r})")
        ck(pg.locator('#sessions .sess').count() == 0, "a session with no prompt in it is not listed")
        ck((pg.locator('#wordmark').get_attribute('aria-label') or '') == 'code' and pg.locator('#wordmark svg').is_visible(),
           "the menu's wordmark says code (hako on a computer)")
        pg.click('#btn-side'); pg.wait_for_timeout(400)
        ck(not pg.locator('aside').is_visible() and pg.locator('#burger').is_visible() and pg.locator('#hmark svg').is_visible(),
           "the fold button hides the menu entirely; the header gets the wordmark and a button to bring it back")
        pg.reload(); pg.wait_for_timeout(1500)
        ck(not pg.locator('aside').is_visible(), "a reload keeps it folded (remembered in this browser)")
        pg.evaluate("document.activeElement && document.activeElement.blur()"); pg.keyboard.press('ControlOrMeta+b'); pg.wait_for_timeout(400)
        ck(pg.locator('aside').is_visible() and not pg.locator('#burger').is_visible(), "ctrl+b (cmd+b) brings it back")
        pg.keyboard.press('ControlOrMeta+b'); pg.wait_for_timeout(400)
        ck(not pg.locator('aside').is_visible(), "and folds it again")
        pg.click('#burger'); pg.wait_for_timeout(400)
        ck(pg.locator('aside').is_visible(), "the header button brings it back too")
        # the command menu: none without the core's table, the old behavior
        if pg.evaluate("state.commands") is None:
            pg.fill('#input', ':th'); pg.wait_for_timeout(150)
            ck(pg.locator('#cmdmenu').is_hidden(), "a core that sends no commands table gets no menu")
        pg.evaluate("""takeState({ commands: [
            { n: 'theme', p: ':', c: 'settings', a: '[name]', d: 'pick a theme' }, { n: 'help', p: ':', c: 'app', a: '', d: 'what hako can do' },
            { n: 'compact', p: '/', c: 'model', a: '', d: 'shrink the conversation' }, { n: 'brief', p: '/', c: 'skill', a: '', d: 'short answers' },
            { n: 'name', p: ':', c: 'session', a: '[name]', d: 'name this chat' } ] })""")
        pg.fill('#input', ''); pg.type('#input', ':th'); pg.wait_for_timeout(150)
        ck(pg.locator('#cmdmenu').is_visible() and pg.locator('#cmdmenu .cmd').first.inner_text().startswith(':theme') and "settings" in pg.inner_text('#cmdmenu'),
           "typing : opens the command menu, prefix matches first, the category at the right")
        pg.keyboard.press('Escape'); pg.wait_for_timeout(100)
        ck(pg.locator('#cmdmenu').is_hidden() and pg.input_value('#input') == ':th', "esc closes the menu only, the line stays")
        pg.fill('#input', ''); pg.type('#input', '/'); pg.wait_for_timeout(100)
        n_all = pg.locator('#cmdmenu .cmd').count()
        ck(n_all == 5 and pg.locator('#cmdmenu .cmd').first.inner_text().startswith('/'), f"/ lists every command, the model's own (/) first ({n_all})")
        pg.keyboard.press('ArrowDown'); pg.keyboard.press('Tab'); pg.wait_for_timeout(100)
        ck(pg.input_value('#input') == '/compact', f"arrows move, tab completes the picked one ({pg.input_value('#input')!r})")
        pg.fill('#input', ''); pg.type('#input', ':na'); pg.keyboard.press('Tab'); pg.wait_for_timeout(100)
        ck(pg.input_value('#input') == ':name ', "a command that takes an argument completes with a space after it")
        pg.fill('#input', ''); pg.type('#input', ':hel'); pg.keyboard.press('Enter'); pg.wait_for_timeout(1500)
        ck(":help" in pg.inner_text('#log') and pg.input_value('#input') == '', "enter on a part of a command runs the picked one")
        idle(pg)
        pg.click('#btn-dir'); pg.wait_for_timeout(1000)
        ck("sub" in pg.inner_text('body'), "browse… shows the project's folders")
        pg.keyboard.press('Escape'); pg.wait_for_timeout(300)
        if pg.locator('.modal').count(): pg.locator('.modal button', has_text='close').first.click()
        pg.fill('#input', 'hey'); pg.click('#send')
        pg.wait_for_selector('.turn.model', timeout=20000)
        ck(len(pg.inner_text('.turn.model')) > 0, "a plain turn gets a reply")
        fb = pg.evaluate("[$('form').getBoundingClientRect().bottom, innerHeight]")
        ck(pg.locator('body.landing').count() == 0 and not pg.locator('#greet').is_visible() and fb[1] - fb[0] < 40,
           f"once the chat starts the composer is at the foot and the greeting gone ({fb})")
        if pg.evaluate("typeof speechSynthesis") != "undefined":
            say = pg.locator('.turn.model .say').first
            say.click(); pg.wait_for_timeout(200)
            on = pg.locator('.turn.model .say.on').count()
            say.click(); pg.wait_for_timeout(200)
            ck(on == 1 and pg.locator('.turn.model .say.on').count() == 0, "each reply has a read-aloud button; a second tap stops it")
        ck(pg.locator('#pills .pill').count() == 3 and "hako-sho" in pg.inner_text('#pills') and "ask" in pg.inner_text('#pills'),
           f"under the composer: folder, model and mode ({pg.inner_text('#pills')!r})")
        pg.wait_for_timeout(800)
        idle(pg); pg.fill('#input', 'TOOL please'); pg.click('#send')
        pg.wait_for_selector('.ask .row button', timeout=20000)
        ck(True, "a tool-approval box appears in ask mode")
        pg.click('.ask .row button.primary')
        pg.wait_for_selector('.tool.result', state='attached', timeout=20000)
        pg.click('.work .whead >> nth=-1')            # folded with the turn's other steps: open it
        ck("alpha.txt" in pg.inner_text('.tool.result'), "the tool's OUTPUT is shown, not just its name")
        pg.wait_for_selector('.turn.model li code', timeout=20000)
        ck(pg.locator('.turn.model li code').count() >= 3 and '`' not in pg.inner_text('.turn.model >> nth=-1'),
           "a reply's markdown renders: the list is a list, `code` is code, no stray backticks")
        pg.wait_for_timeout(1000)
        idle(pg)
        upl = W + "/up.txt"; open(upl, "w").write("uploaded\n")
        with pg.expect_file_chooser() as fc: pg.click('#attach')
        fc.value.set_files(upl)
        pg.wait_for_timeout(400)
        pg.fill('#input', 'here is a file'); pg.click('#send'); pg.wait_for_timeout(2500)
        ck(os.path.exists(PROJ + "/up.txt"), "an attached file lands in the project")
        pg.fill('#input', ''); pg.focus('#input'); pg.keyboard.press('ArrowUp')
        first = pg.input_value('#input'); pg.keyboard.press('ArrowUp'); second = pg.input_value('#input')
        pg.keyboard.press('ArrowDown'); pg.keyboard.press('ArrowDown')
        ck(first.startswith('here is a file') and second == 'TOOL please' and pg.input_value('#input') == '',
           f"up/down walk what was sent and come back to the empty draft ({first!r}, {second!r})")
        idle(pg)
        open(upl, "w").write("second\n")
        with pg.expect_file_chooser() as fc: pg.click('#attach')
        fc.value.set_files(upl); pg.wait_for_timeout(300)
        pg.fill('#input', 'again'); pg.click('#send'); pg.wait_for_timeout(2500)
        ck(open(PROJ + "/up.txt").read() == "uploaded\n" and len([f for f in os.listdir(PROJ) if f.startswith("up")]) == 2,
           f"a second upload of the same name does not overwrite ({sorted(os.listdir(PROJ))})")
        idle(pg)
        pg.fill('#input', ':model'); pg.click('#send'); pg.wait_for_timeout(1200)
        ck(pg.locator('.modal').count() == 1 and "hako-sho" in pg.inner_text('.modal'),
           ":model opens the model picker, listing the installed model")
        pg.locator('.modal button', has_text='close').first.click(); pg.wait_for_timeout(400)
        ck(pg.locator('#btn-settings').count() == 0, "settings left the menu's rows")
        pg.click('#acct-btn'); pg.wait_for_timeout(200)
        ck(pg.locator('.pop button', has_text='settings').count() == 1 and pg.locator('.pop button', has_text='profile').count() == 1,
           "the user at the foot of the menu opens profile and settings")
        pg.locator('.pop button', has_text='settings').click(); pg.wait_for_timeout(500)
        tiles = pg.locator('.modal .tile')
        ck(tiles.count() == 20 and tiles.nth(0).get_attribute('data-theme') == 'mithraeum' and tiles.nth(1).get_attribute('data-theme') == 'terminal'
           and pg.locator('.modal .grouplabel', has_text='light or dark').count() == 1,
           f"settings show mithraeum first, terminal next, 20 themes as previews, then light or dark ({tiles.count()})")
        before = pg.inner_text('#log')
        for t in ('nord', 'washi', 'gruvbox'):
            pg.locator(f'.modal .tile[data-theme="{t}"]').click(); pg.wait_for_timeout(250)
        ck(pg.locator('.modal').count() == 1 and pg.locator('.modal .tile.sel').get_attribute('data-theme') == 'gruvbox'
           and pg.locator('.modal .tile.sel').count() == 1,
           "tapping themes previews them in place, settings stays open, one marked")
        # compact: light or dark, a background and an indicator are each one line of choices; grain and voice are switches
        pg.locator('.modal .seg button[data-k="dark"]').click(); pg.wait_for_timeout(150)
        ck(pg.evaluate("settings.appearance") == 'dark' and pg.locator('.modal .seg button.sel[data-k="dark"]').count() == 1, "light or dark is one line, a tap picks")
        pg.locator('.modal .seg button[data-k="auto"]').click()
        pg.locator('.modal .seg button[data-k="rays"]').click(); pg.wait_for_timeout(150)
        ck(pg.evaluate("$('backdrop').dataset.bg") == 'rays', "a background is one tap")
        pg.locator('.modal .seg button[data-k="none"]').first.click()
        g = pg.locator('.modal .swrow', has_text='paper grain')
        g.click(); pg.wait_for_timeout(100)
        ck(g.get_attribute('aria-checked') == 'false' and not pg.evaluate("document.body.classList.contains('grain')"), "paper grain is a switch")
        g.click()
        pg.locator('.modal .stabs button[data-k="hako"]').click()
        pg.locator('.modal .seg button[data-k="moon"]').click()
        ck(pg.evaluate("settings.spinner") == 'moon' and pg.locator('.modal').count() == 1, "the thinking indicator is one tap and settings stays open")
        pg.locator('.modal .stabs button[data-k="look"]').click()
        pg.locator('.modal .stabs button[data-k="hako"]').click(); pg.wait_for_timeout(150)
        ck(pg.locator('.modal [data-pane="hako"]').is_visible() and not pg.locator('.modal [data-pane="look"]').is_visible()
           and "hako-sho" in pg.inner_text('.modal [data-pane="hako"]'), "settings has tabs: hako holds the model, look hides")
        pg.locator('.modal .seg button[data-k="braille"]').click()
        pg.locator('.modal .stabs button[data-k="data"]').click(); pg.wait_for_timeout(150)
        ck("forget settings" in pg.inner_text('.modal [data-pane="data"]'), "and your data holds what is kept and the forget button")
        pg.locator('.modal .stabs button[data-k="look"]').click()
        pg.keyboard.press('Escape'); pg.wait_for_timeout(1200)
        if os.environ.get("DEBUG"):
            import difflib; a = pg.inner_text('#log')
            print("META:", pg.inner_text('#meta'), len(before), len(a), list(difflib.unified_diff(before.splitlines(), a.splitlines(), lineterm=""))[:14])
        ck(pg.inner_text('#log') == before and "theme: gruvbox" in pg.inner_text('#meta'),
           "closing settings says the theme in the status line, nothing in the chat")
        sess = os.popen(f"cat {HOME}/.hako/projects/*/sessions/*.jsonl 2>/dev/null").read()
        ck("theme:" not in sess, "and nothing about the theme in the session log")
        # the rc's own theme (state.theme), a custom one in slots, indicators, a session name
        pg.evaluate("takeState({ theme: 'nord' })"); pg.wait_for_timeout(100)
        ck(bg() == "rgb(46, 52, 64)", f"the rc's theme from state is applied ({bg()})")
        pg.evaluate("takeState({ theme: 'mine', theme_slots: { bg: '#101820', fg: '#f2aa4c', accent: '#f2aa4c', err: '#ff5555' } })"); pg.wait_for_timeout(100)
        ck(bg() == "rgb(16, 24, 32)", f"and a custom rc theme renders from its slots ({bg()})")
        pg.evaluate("takeState({ remote: 1, update: { current: '0.2.10', latest: '0.2.11' } })"); pg.wait_for_timeout(100)
        ck("remote" in pg.inner_text('#ind') and "0.2.11" in pg.inner_text('#ind'), f"remote and an update show quietly in the header ({pg.inner_text('#ind')!r})")
        pg.evaluate("takeState({ theme: 'gruvbox', theme_slots: null, remote: 0, update: null })")
        # 11-04 on the phone: every tap on the chat already open resumed it again (a "resumed" line each time on the desktop)
        sent = []; on_req = lambda q: sent.append(q.post_data or '') if q.url.endswith('/api/slash') else None
        pg.on('request', on_req)
        cur = pg.locator('#sessions .sess.cur')
        if cur.count(): cur.first.click(); pg.wait_for_timeout(400)
        pg.remove_listener('request', on_req)
        ck(cur.count() and not any(':resume' in x for x in sent) and pg.locator('#log .turn.user').count(),
           f"tapping the chat that is already open just shows it, no :resume ({sent}, {cur.count()}, {pg.locator('#log .turn.user').count()})")
        pg.locator('#sessions .sessrow').first.hover(); pg.locator('#sessions .sessrow .act.more').first.click(); pg.wait_for_timeout(200)
        pg.locator('.pop button', has_text='rename').click(); pg.wait_for_timeout(300)
        pg.fill('.modal input', 'pong night'); pg.keyboard.press('Enter'); pg.wait_for_timeout(1500)
        state_file = os.popen(f"cat {HOME}/.hako/projects/*/state 2>/dev/null").read()
        echoed = pg.locator('.turn.user', has_text=':name').count() + pg.locator('.turn.system', has_text='chat named').count()
        ck(pg.inner_text('#title') == 'pong night' and not echoed and "pong night" in pg.inner_text('#sessions')
           and "session_name=pong night" in state_file,
           f"rename on the open session names it through hako (:name), quietly; header and list show it ({pg.inner_text('#title')!r}, echoed {echoed})")
        # pin, archive, view all: kept by hako beside the chat, so another device sees them
        n0 = pg.locator('#sessions .sessrow').count()
        pg.locator('#sessions .sessrow').first.hover(); pg.locator('#sessions .sessrow .act.more').first.click(); pg.wait_for_timeout(150)
        pg.locator('.pop button', has_text='pin').click(); pg.wait_for_timeout(200)
        ck(not pg.locator('#pinned-wrap').is_hidden() and "pong night" in pg.inner_text('#pinned') and "pong night" not in pg.inner_text('#sessions'),
           "pin moves a chat up into pinned, out of recents")
        marks = os.popen(f"cat {HOME}/.hako/projects/*/sessions/*.mark 2>/dev/null").read()
        other = b.new_page(viewport=vp); other.goto(U); other.wait_for_timeout(1500)
        ck(marks.strip() == "pinned" and "pong night" in other.inner_text('#pinned') and pg.evaluate("settings.pins.length") == 0,
           f"hako keeps the pin (a .mark sidecar, not this browser), and a fresh browser sees it ({marks.strip()!r})")
        other.close()
        pg.locator('#pinned .sessrow').first.hover(); pg.locator('#pinned .sessrow .act.more').first.click(); pg.wait_for_timeout(150)
        pg.locator('.pop button', has_text='archive').click(); pg.wait_for_timeout(200)
        ck(pg.locator('#pinned-wrap').is_hidden() and "pong night" not in pg.inner_text('#sessions') and not pg.locator('#btn-all').is_hidden(),
           "archive tucks it away (unpinned) and view all appears")
        pg.click('#btn-all'); pg.wait_for_timeout(300)
        pg.locator('.modal .tabs button', has_text='archived').click(); pg.wait_for_timeout(150)
        ck("pong night" in pg.inner_text('.modal .sessions') and "archived 1" in pg.inner_text('.modal .tabs'),
           "view all shows it under archived")
        pg.locator('.modal .sessrow').first.hover(); pg.locator('.modal .sessrow .act.more').first.click(); pg.wait_for_timeout(150)
        pg.locator('.pop button', has_text='unarchive').click(); pg.wait_for_timeout(150)
        pg.keyboard.press('Escape'); pg.wait_for_timeout(200)
        ck("pong night" in pg.inner_text('#sessions') and pg.locator('#sessions .sessrow').count() == n0,
           "unarchive brings it back to recents")
        # profile: a name, and a picture that never leaves the browser
        pg.click('#acct-btn'); pg.locator('.pop button', has_text='profile').click(); pg.wait_for_timeout(200)
        pg.fill('.modal .profile input:not([type=file])', 'Zach'); pg.keyboard.press('Enter'); pg.wait_for_timeout(200)
        ck("Zach" in pg.inner_text('#acct') and pg.inner_text('#acct .avatar').lower() == "z", "a name in profile shows at the foot, its first letter as the face")
        px = 'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg=='
        pg.click('#acct-btn'); pg.locator('.pop button', has_text='profile').click(); pg.wait_for_timeout(200)
        pg.set_input_files('.modal .profile input[type=file]', files=[{"name": "me.png", "mimeType": "image/png", "buffer": __import__('base64').b64decode(px)}])
        pg.wait_for_timeout(400); pg.keyboard.press('Escape'); pg.wait_for_timeout(200)
        av = pg.evaluate("settings.avatar")
        ck(av.startswith("data:image/jpeg") and "url(" in pg.evaluate("$('acct').querySelector('.avatar').style.backgroundImage")
           and not any(f.startswith('me') for f in os.listdir(PROJ)),
           f"a picture is cut down to a small jpeg kept in settings, shown at the foot, nothing written to the folder ({len(av)} chars)")
        # a pasted screenshot: a chip on the tray, then [image #N] in the message and the bytes in hako's images dir
        pg.evaluate("""(() => { const px = atob('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==');
            const dt = new DataTransfer(); dt.items.add(new File([Uint8Array.from(px, (c) => c.charCodeAt(0))], 'image.png', { type: 'image/png' }));
            $('input').dispatchEvent(new ClipboardEvent('paste', { clipboardData: dt, bubbles: true, cancelable: true })); })()""")
        pg.wait_for_timeout(300)
        ck("▣ pasted-" in pg.inner_text('#tray'), "a pasted image shows as an image chip on the tray")
        pg.fill('#input', 'what is this'); pg.click('#send'); pg.wait_for_timeout(2500)
        imgs = os.popen(f"ls {HOME}/.hako/projects/*/images/ 2>/dev/null").read().split()
        ck("what is this [image #1]" in pg.inner_text('#log') and any(i.endswith("-1.png") for i in imgs) and not pg.inner_text('#tray').strip(),
           "sending it gives the message [image #1], keeps the png in hako's images dir, and clears the tray (%s)" % imgs)
        ck(not os.path.exists(PROJ + "/image.png") and not any(f.startswith("pasted-") for f in os.listdir(PROJ)),
           "and puts nothing in the project folder")
        ck(not errs, f"no JS errors through the whole session {errs[:2]}")
    b.close()
srv.kill()
print("\n".join(res)); print("RESULT:", "PASS" if all(r.startswith("ok") for r in res) else "FAIL")
shutil.rmtree(W, ignore_errors=True)
