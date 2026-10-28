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
        pg = b.new_page(viewport=vp)
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
            ck(not errs, f"[phone] no JS errors {errs[:2]}")
            pg.close(); continue
        ck("alpha.txt" in pg.inner_text('#files'), "the side panel lists the project's files")
        pg.click('#btn-dir'); pg.wait_for_timeout(1000)
        ck("sub" in pg.inner_text('body'), "browse… shows the project's folders")
        pg.keyboard.press('Escape'); pg.wait_for_timeout(300)
        if pg.locator('.modal').count(): pg.locator('.modal button', has_text='close').first.click()
        pg.fill('#input', 'hey'); pg.click('#send')
        pg.wait_for_selector('.turn.model', timeout=20000)
        ck(len(pg.inner_text('.turn.model')) > 0, "a plain turn gets a reply")
        pg.wait_for_timeout(800)
        idle(pg); pg.fill('#input', 'TOOL please'); pg.click('#send')
        pg.wait_for_selector('.ask .row button', timeout=20000)
        ck(True, "a tool-approval box appears in ask mode")
        pg.click('.ask .row button.primary')
        pg.wait_for_selector('.tool.result', timeout=20000)
        ck("alpha.txt" in pg.inner_text('.tool.result'), "the tool's OUTPUT is shown, not just its name")
        pg.wait_for_timeout(1000)
        idle(pg)
        upl = W + "/up.txt"; open(upl, "w").write("uploaded\n")
        with pg.expect_file_chooser() as fc: pg.click('#attach')
        fc.value.set_files(upl)
        pg.wait_for_timeout(400)
        pg.fill('#input', 'here is a file'); pg.click('#send'); pg.wait_for_timeout(2500)
        ck(os.path.exists(PROJ + "/up.txt"), "an attached file lands in the project")
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
        ck(sw == 19 * 4, f"settings list all 19 themes, each with its swatches ({sw // 4})")
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
