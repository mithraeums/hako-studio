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
    for name, vp in (("desk", {'width': 1280, 'height': 800}), ("phone", {'width': 390, 'height': 844})):
        pg = b.new_page(viewport=vp, has_touch=(name == "phone"))
        errs = []; pg.on('pageerror', lambda e: errs.append(str(e)))
        pg.on('console', lambda m: errs.append(m.text) if m.type == 'error' else None)
        if name == "phone":   # a stand-in visualViewport, so the test can open "the keyboard" the way iOS does
            pg.add_init_script("""window.__vv = Object.assign(new EventTarget(), { width: 390, height: 844, offsetTop: 0, offsetLeft: 0, scale: 1 });
                Object.defineProperty(window, 'visualViewport', { get: () => window.__vv, configurable: true });""")
        pg.goto(U); pg.wait_for_timeout(1500)
        ck(not errs, f"[{name}] page loads with no JS errors {errs[:2]}")
        ck(pg.evaluate("document.documentElement.scrollWidth") <= vp['width'], f"[{name}] no sideways scroll")
        if name == "phone":
            # the keyboard opens: iOS shrinks the visible box to 420 and pans it 360 down to show the field
            box = lambda: pg.evaluate("""(() => { const b = document.body.getBoundingClientRect(), f = $('form').getBoundingClientRect();
                return [Math.round(b.top), Math.round(b.bottom), Math.round(f.bottom)]; })()""")
            pg.evaluate("__vv.height = 420; __vv.offsetTop = 360; __vv.dispatchEvent(new Event('resize')); __vv.dispatchEvent(new Event('scroll'))")
            pg.wait_for_timeout(200); kb = box()
            ck(kb[0] == 360 and kb[1] == 780 and kb[2] >= 760,
               f"[phone] with the keyboard open the page fills the visible box, composer at its foot over the keyboard (body {kb[0]}-{kb[1]}, form ends {kb[2]})")
            pg.evaluate("__vv.height = 844; __vv.offsetTop = 0; __vv.dispatchEvent(new Event('resize'))")
            pg.wait_for_timeout(200); shut = box()
            ck(shut[0] == 0 and shut[1] == 844 and shut[2] >= 824, f"[phone] closing it puts the composer back at the bottom (body {shut[0]}-{shut[1]}, form ends {shut[2]})")
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
            pg.click('#btn-side'); pg.wait_for_timeout(350)
            ck(not pg.locator('aside').is_visible(), "[phone] its fold button closes it again")
            pg.click('#burger'); pg.wait_for_timeout(350); pg.click('#btn-new'); pg.wait_for_timeout(1200)
            ck(pg.locator('body.landing').count() == 1 and pg.locator('#greet').is_visible() and not pg.locator('aside').is_visible()
               and pg.evaluate("document.documentElement.scrollWidth") <= 390,
               "[phone] new (from the drawer) closes it and shows the landing, greeting and all, no sideways scroll")
            pg.evaluate("document.getElementById('title').textContent = 'read main.c and tell me in one sentence what it does'")
            pg.wait_for_timeout(100)
            wm = pg.evaluate("(() => { const s = document.querySelector('#hmark svg'); return s ? s.getBoundingClientRect().right : 0; })()")
            tl = pg.evaluate("document.getElementById('title').getBoundingClientRect().left")
            ck(wm and wm <= tl + 0.5, f"[phone] a long session title never runs under the wordmark ({wm:.0f} <= {tl:.0f})")
            ck(not errs, f"[phone] no JS errors {errs[:2]}")
            pg.close(); continue
        bg = lambda: pg.evaluate("getComputedStyle(document.body).backgroundColor")
        ck(bg() == "rgb(255, 255, 255)", f"the default theme is terminal: a plain light page under a light system ({bg()})")
        pg.emulate_media(color_scheme="dark"); pg.wait_for_timeout(150)
        ck(bg() == "rgb(28, 28, 30)", f"and it follows the system to dark ({bg()})")
        pg.emulate_media(color_scheme="light"); pg.wait_for_timeout(150)
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
        pg.wait_for_selector('.tool.result', timeout=20000)
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
        pg.click('#btn-settings'); pg.wait_for_timeout(500)
        sw = pg.locator('.modal .swatch').count()
        ck(sw == 20 * 4 and "terminal" in pg.locator('.modal .opt:has(.swatch)').first.inner_text(),
           f"settings list terminal first, then all 19 themes, each with its swatches ({sw // 4})")
        before = pg.inner_text('#log')
        for t in ('nord', 'washi', 'gruvbox'):
            pg.locator('.modal .opt', has_text=t).first.click(); pg.wait_for_timeout(250)
        ck(pg.locator('.modal').count() == 1 and "\u25ce gruvbox" in pg.inner_text('.modal'),
           "tapping themes previews them in place, settings stays open")
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
        pg.locator('#sessions .sessrow').first.hover(); pg.locator('#sessions .sessrow .act.ren').first.click(); pg.wait_for_timeout(300)
        pg.fill('.modal input', 'pong night'); pg.keyboard.press('Enter'); pg.wait_for_timeout(1500)
        state_file = os.popen(f"cat {HOME}/.hako/projects/*/state 2>/dev/null").read()
        echoed = pg.locator('.turn.user', has_text=':name').count() + pg.locator('.turn.system', has_text='chat named').count()
        ck(pg.inner_text('#title') == 'pong night' and not echoed and "pong night" in pg.inner_text('#sessions')
           and "session_name=pong night" in state_file,
           f"the pencil on the open session names it through hako (:name), quietly; header and list show it ({pg.inner_text('#title')!r}, echoed {echoed})")
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
