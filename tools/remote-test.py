#!/usr/bin/env python3
"""The relay, end to end, in a real browser. A real `hako` runs `:remote on` in
a pty against a local relay (relay/relay --dev); headless Chromium with a
virtual passkey (prf on) opens the link page, makes the account with an
invite and links the computer. Then the browser forgets everything (a fresh
device), signs in with the passkey, which unwraps the account key, picks the
computer, and runs a turn on it through the relay. Fake engine, no network.

Needs playwright + chromium, a built ../hako-code/hako, relay/relay and a
`make site RELAY_URL=http://127.0.0.1:18793` dist (make remote-test does both)."""
import os, re, sys, time, pty, fcntl, termios, struct, select, signal, subprocess, tempfile
try:
    from playwright.sync_api import sync_playwright
except ImportError:
    sys.exit("pip install playwright && playwright install chromium")
R = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
HAKO = R + "/hako-code/hako"; FAKE = R + "/hako-code/tools/tty/fakehakm.py"
RELAY = R + "/hako-studio/relay/relay"; DIST = R + "/hako-studio/dist"
RP, SP = 18793, 8794
STUDIO = "http://localhost:%d" % SP
for f in (HAKO, RELAY, DIST + "/index.html"):
    if not os.path.exists(f): sys.exit("missing " + f)
W = tempfile.mkdtemp(prefix="remote-"); HOME = W + "/home"; PROJ = os.path.realpath(W) + "/proj"
if os.environ.get("DEBUG"): print("work dir", W)
os.makedirs(HOME + "/.hako/models/hako-sho"); os.makedirs(PROJ)
open(HOME + "/.hako/models/hako-sho/hako-sho.mlf2", "w").write("fake")
open(HOME + "/.hakorc", "w").write("ai_provider=mithraeum\nai_model=hako-sho\nshow_splash=0\nai_auto_approve=0\n")
slug = PROJ.replace("/", "-"); os.makedirs(HOME + "/.hako/projects/" + slug)
open(HOME + "/.hako/projects/" + slug + "/trust", "w").write("granted=%d\n" % int(time.time()))
procs = []
def bg(args, **kw):
    p = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=open(W + "/" + os.path.basename(args[0]) + ".out", "w"),
                         stderr=subprocess.STDOUT, **kw)
    procs.append(p); return p
import atexit
atexit.register(lambda: [p.kill() for p in procs])
invite = subprocess.check_output([RELAY, "invite", "1", "-d", W + "/relay"]).decode().strip()
bg([RELAY, "-p", str(RP), "-d", W + "/relay", "-o", STUDIO, "--dev", "-v"])
bg(["python3", R + "/hako-studio/tools/serve-static.py", DIST, str(SP)])
time.sleep(0.8)

# the desktop: hako in a wide pty, so the link line does not wrap
pid, fd = pty.fork()
if pid == 0:
    os.chdir(PROJ)
    os.execve(HAKO, [HAKO], dict(os.environ, HOME=HOME, HAKO_HAKM=FAKE, TERM="xterm-256color",
              HAKO_RELAY_URL="http://127.0.0.1:%d" % RP, HAKO_STUDIO_URL=STUDIO, HAKO_NO_BROWSER="1"))
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 400, 0, 0))
atexit.register(lambda: os.kill(pid, signal.SIGKILL))
seen = b""
# read the pty all the time: a full pty buffer blocks hako mid-turn
import threading
def reader():
    global seen
    while True:
        r, _, _ = select.select([fd], [], [], 0.2)
        if r:
            try: seen += os.read(fd, 65536)
            except OSError: return
threading.Thread(target=reader, daemon=True).start()
def pump(secs=0.2): time.sleep(secs)
def until(pat, secs=15):
    end = time.time() + secs
    while time.time() < end:
        pump(0.2)
        m = re.search(pat, re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", seen))
        if m: return m
    return None
pump(2.0)
os.write(fd, b":remote on 8797\r")
m = until(rb"(http://localhost:\d+/#link=[0-9a-f]{64}\.[0-9a-f]{64})")
res = []
def ck(c, what): res.append(("ok  " if c else "FAIL") + " " + what)
ck(m is not None, "desktop: :remote on shows a link page")
link = m.group(1).decode() if m else STUDIO
if not m and os.environ.get("DEBUG"): print("TTY0:", [l.strip()[:200] for l in re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", seen).decode(errors="replace").splitlines() if "remote" in l or "link" in l or "rror" in l])

with sync_playwright() as p:
    b = p.chromium.launch()
    ctx = b.new_context(viewport={'width': 1280, 'height': 800})
    pg = ctx.new_page()
    errs = []; pg.on('pageerror', lambda e: errs.append(str(e)))
    pg.on('console', lambda m: errs.append(m.text) if m.type == 'error' else None)
    cdp = ctx.new_cdp_session(pg)
    cdp.send('WebAuthn.enable')
    cdp.send('WebAuthn.addVirtualAuthenticator', {'options': {'protocol': 'ctap2', 'transport': 'internal',
             'hasResidentKey': True, 'hasUserVerification': True, 'isUserVerified': True, 'hasPrf': True,
             'automaticPresenceSimulation': True}})
    pg.goto(link); pg.wait_for_selector('.modal', timeout=10000)
    pg.wait_for_timeout(1500)
    if os.environ.get("DEBUG"): print("LINKMODAL:", link[:60], [mo.inner_text().replace("\n", " | ") for mo in pg.locator('.modal').all()])
    pg.fill('.modal input >> nth=0', invite)
    pg.click('.modal button:has-text("new account")')
    pg.wait_for_selector('.modal .note:has-text("linked")', timeout=15000)
    ck(True, "page: the passkey makes the account and links the computer")
    ck('#link=' not in pg.url, "page: the link secret leaves the address bar")
    ck(until(rb"remote: linked") is not None, "desktop: linked, the account key arrived")

    # a fresh device: nothing stored, the same synced passkey
    pg.goto(STUDIO + "/"); pg.wait_for_timeout(500)
    pg.evaluate("new Promise(r => { const q = indexedDB.deleteDatabase('hako-account'); q.onsuccess = q.onerror = q.onblocked = r; })")
    pg.goto(STUDIO + "/"); pg.wait_for_timeout(1500)
    if pg.locator('.modal').count(): pg.locator('.modal button').last.click()
    ck(pg.locator('#modes').count() == 1 and pg.locator('#modes').get_attribute('hidden') is None,
       "page: signed out, the menu still offers chat · code (a home-screen app starts with nothing stored)")
    pg.evaluate("document.getElementById('mode-code').click()")
    pg.wait_for_selector('.modal button:has-text("sign in with a passkey")', timeout=10000)
    sheet = pg.locator('.modal').inner_text()
    ck("closed beta" in sheet and "never your chats" in sheet and "this browser" in sheet
       and pg.locator('.modal button:has-text("request access")').count() == 1,
       "page: signed out, code says it is a closed beta, what an account keeps, and offers to request access")
    pg.locator('.modal button:has-text("close")').last.click()
    pg.goto(STUDIO + "/#signin"); pg.wait_for_selector('.modal button:has-text("sign in with a passkey")', timeout=10000)
    ck(pg.locator('.modal input').first.is_hidden(), "page: #signin asks for no invite")
    pg.click('.modal button:has-text("sign in with a passkey")')
    host = os.uname().nodename.split('.')[0]
    row = pg.wait_for_selector('.modal .opt:has-text("proj")', timeout=15000)
    ck(host in row.inner_text() and pg.locator('.modal .opt').count() == 1,
       "page: a fresh device signs in, the passkey unwraps the key, the code session shows by folder and host (and only it)")
    row.click()
    pg.wait_for_function("location.hash.startsWith('#remote=')", timeout=5000)
    pg.wait_for_timeout(2500)
    ck(until(rb"a device on your account connected") is not None, "desktop: the page's session arrives")
    ck(pg.locator('#btn-dir').is_hidden() and pg.locator('#mode-code.on').count() == 1, "page: in code, no folder browsing; the menu says code")
    ck("proj" in pg.inner_text('#project-list'), "page: the menu lists the project (the shared folder)")
    ck(pg.locator('#here').is_visible(), "page: in a session, the folder's own chats show too")
    for mo in pg.locator('.modal').all(): print("modal open:", mo.inner_text().replace("\n", " | "))
    if os.environ.get("DEBUG"):
        print(open(W + "/relay.out").read()[-3000:]); print("\n".join(l for l in re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", seen).decode(errors="replace").splitlines() if "remote" in l or "relay" in l)); print(errs)
    pg.fill('#input', 'hey from the relay'); pg.click('#send')
    t0 = time.time()
    if os.environ.get("DEBUG"): print("sent at %.3f" % (t0 % 1000))
    try:
        pg.wait_for_selector('.turn.model', timeout=20000)
        if os.environ.get("DEBUG"): print("turn took %.1fs, seen at %.3f" % (time.time() - t0, time.time() % 1000))
        ck(len(pg.inner_text('.turn.model')) > 0, "page: a turn on the computer, through the relay")
    except Exception as e:
        ck(False, "page: a turn on the computer, through the relay (%s)" % str(e)[:60])
        if os.environ.get("DEBUG"): print("LOG:", pg.inner_text('#log')[-1500:]); print("META:", pg.inner_text('#meta')); pump(3); print("LOG2:", pg.inner_text('#log')[-800:]); print("HTML:", pg.inner_html('#log')[-1500:])
    ck(until(rb"hey from the relay") is not None, "desktop: the prompt shows in the terminal session")
    ck("?t=" not in pg.inner_text('#log') and "#link=" not in pg.inner_text('#log'),
       "page: the desktop's token and link lines never reach a device")
    pg.wait_for_timeout(800)
    pg.fill('#input', 'TOOL please'); pg.click('#send')
    try:
        pg.click('.ask:not(.settled) button.primary', timeout=20000)
        pg.wait_for_timeout(2500)
        log = open([os.path.join(dp, f) for dp, _, fs in os.walk(HOME + "/.hako/projects") for f in fs if f.endswith(".jsonl")][0]).read()
        ck("approval answered on a remote device" in log and "worked for" in pg.inner_text('#log').split('TOOL please')[-1],
           "page: the tool asks here, is approved here, and the turn finishes")
    except Exception as e:
        ck(False, "page: the tool asks here, is approved here, and the turn finishes (%s)" % str(e)[:60])
    pg.fill('#input', 'ASK me'); pg.click('#send')
    try:
        pg.click('.ask:not(.settled) button:has-text("blue")', timeout=20000)
        pg.wait_for_timeout(2500)
        log = open([os.path.join(dp, f) for dp, _, fs in os.walk(HOME + "/.hako/projects") for f in fs if f.endswith(".jsonl")][0]).read()
        ck("choice answered on a remote device" in log, "page: the computer's question is answered from here")
    except Exception as e:
        ck(False, "page: the computer's question is answered from here (%s)" % str(e)[:60])
        if os.environ.get("DEBUG"): print("LOG4:", pg.inner_text('#log')[-700:]); print("HTML4:", pg.inner_html('#log')[-1200:]); print("TTY4:", re.sub(rb"\x1b\[[0-9;?]*[A-Za-z]", b"", seen)[-2500:].decode(errors="replace"))
    # a slow turn (a cold local model): the text shows at once, stop ends it, a reload agrees
    pg.wait_for_timeout(1500)
    pg.fill('#input', 'SLOW turn please'); pg.click('#send')
    pg.wait_for_timeout(2500)
    ck("SLOW turn please" in pg.inner_text('#log'), "page: a slow turn shows what was sent at once")
    ck(pg.inner_text('#send').strip() == 'stop', "page: and offers stop while it runs")
    pg.click('#send'); pg.wait_for_timeout(3000)
    ck(pg.inner_text('#send').strip() == 'send', "page: stop ends the turn")
    ck(until(rb"interrupted", 5) is not None, "desktop: the turn was interrupted")
    pg.reload(); pg.wait_for_timeout(4000)
    ck(pg.inner_text('#send').strip() == 'send' and not pg.locator('.thinking').count(), "page: after a reload, not stuck thinking")
    pg.fill('#input', ':auto'); pg.click('#send')
    pg.wait_for_timeout(2000)
    ck("not from a remote device" in pg.inner_text('#log'), "page: :auto is refused from a remote device")
    remote_url = pg.url
    # the same session from Safari's engine: Chromium (signed in) binds a WebKit
    # device to the account and hands it K, as a passkey or the QR would
    K = pg.evaluate("(async () => hex(await kvGet('k')))()"); acct = pg.evaluate("kvGet('acct')")
    wb = p.webkit.launch(); wp = wb.new_context(viewport={'width': 390, 'height': 844}).new_page()
    werrs = []; wp.on('pageerror', lambda e: werrs.append(str(e)))
    wlog = []
    if os.environ.get("DEBUG"):
        wp.add_init_script("window.hakoDebug = 1")
        wp.on('console', lambda m: wlog.append("%.2f %s" % (time.time() % 100, m.text)))
    wp.goto(STUDIO + "/"); wp.wait_for_timeout(1500)
    wpub = wp.evaluate("(async () => { await relay.signIn(); return hex(relay.pub); })()")
    ck(pg.evaluate("(async (k) => { await relay.signIn(); return (await relay.req('POST', '/v1/bind', k)).status; })(%r)" % wpub) == 200,
       "webkit: a second device joins the account")
    wp.evaluate("(async (a) => { await kvSet('k', unhex(a[0])); await kvSet('acct', a[1]); })(%s)" % [K, acct])
    wp.goto("about:blank"); wp.goto(remote_url); wp.wait_for_timeout(3500)
    if os.environ.get("DEBUG"):
        print("WK dbg2:", wp.evaluate("(() => { hakoTrace('probe'); return window.hakoLog; })()"))
        print("WK dbg:", wp.evaluate("[window.hakoDebug, typeof hakoTrace, (window.hakoLog||[]).length, location.href, typeof REMOTE_ID !== 'undefined' && REMOTE_ID]"))
        print("\n".join(wp.evaluate("window.hakoLog || []")[-40:]))
        print("WK meta:", wp.inner_text('#meta'), "| modals:", [m.inner_text()[:120] for m in wp.locator('.modal').all()])
        print("WK log:", wp.inner_text('#log')[-400:].replace("\n", " / "))
        print("WK state:", wp.evaluate("(async () => { try { await remote.ready; return 'ready'; } catch (e) { return 'err ' + e.message; } })()"))
        print("WK fetch:", wp.evaluate("(async () => { try { const r = await Promise.race([fetch('/api/state'), new Promise((_, j) => setTimeout(() => j(new Error('timeout 6s')), 6000))]); return r.status + ' ' + (await r.text()).slice(0, 80); } catch (e) { return 'err ' + e.message + ' ' + (e.stack || '').slice(0, 300); } })()"))
    wp.fill('#input', 'SLOW from safari'); wp.click('#send'); wp.wait_for_timeout(2500)
    ck("SLOW from safari" in wp.inner_text('#log') and wp.inner_text('#send').strip() == 'stop',
       "webkit: the text shows at once and stop is offered")
    said = seen.count(b"a device on your account connected")
    wp.reload(); wp.wait_for_timeout(4000)
    ck(wp.inner_text('#send').strip() == 'stop', "webkit: a reload mid-turn still offers stop")
    kids = ",".join(subprocess.run(["pgrep", "-P", str(pid)], capture_output=True, text=True).stdout.split())
    held = subprocess.run(["lsof", "-nP", "-a", "-p", kids, "-iTCP", "-sTCP:LISTEN"], capture_output=True, text=True).stdout if kids else ""
    ck(not held.strip(), "desktop: the curls hako starts do not inherit its listening socket")
    ck(said >= 1 and seen.count(b"a device on your account connected") == said
       and "device on your account" not in wp.inner_text('#log'),
       "desktop: a device that reconnects is announced once, at the desk only")
    wp.click('#send'); wp.wait_for_timeout(3000)
    ck(wp.inner_text('#send').strip() == 'send', "webkit: stop ends it")
    wp.fill('#input', 'hey from safari'); wp.click('#send')
    try:
        wp.wait_for_selector('.turn.model >> nth=-1', timeout=15000); wp.wait_for_timeout(1500)
        ck("hey from safari" in wp.inner_text('#log') and wp.inner_text('#send').strip() == 'send', "webkit: a normal turn streams back")
    except Exception as e:
        ck(False, "webkit: a normal turn streams back (%s)" % str(e)[:60])
    ck(not werrs, "webkit: no JS errors %s" % werrs[:2])
    wb.close()
    pg.click('#mode-chat'); pg.wait_for_timeout(2000)
    ck('#remote=' not in pg.url and pg.locator('#mode-chat.on').count() == 1, "page: chat goes back to this browser's own agent")
    pg.wait_for_timeout(1500)
    ck(pg.locator('#projects').is_visible() and pg.locator('#here').is_visible()
       and pg.locator('#project-list .sess.k-code:has-text("proj")').count() == 1
       and pg.locator('#sessions .sess.k-code').count() == 0,
       "page: signed in, chat lists the code sessions (marked code) and this browser's chats together")
    pg.click('#mode-code'); pg.wait_for_timeout(3000)
    ck(pg.locator('#here').is_hidden() and pg.locator('#project-list .sess.k-code').count() == 1, "page: code lists code sessions only")
    ck('#remote=' not in pg.url and pg.locator('.modal').count() == 0 and "proj" in pg.inner_text('#project-list')
       and "add a device" in pg.inner_text('#project-list'), "page: code lists the sessions in the menu, with add a device, no popup")
    pg.locator('#project-list button:has-text("add a device")').click()
    ck(":remote on" in pg.locator('.modal').inner_text(), "page: add a device says to run :remote on in the folder")
    pg.locator('.modal button:has-text("close")').click()
    # the computer's upload dies while its download lives (the relay still lists it online):
    # hako must notice on the next keepalive and reconnect, or every reply vanishes
    ups = subprocess.run(["pgrep", "-P", str(pid), "-f", "/v1/up"], capture_output=True, text=True).stdout.split()
    for u in ups: os.kill(int(u), signal.SIGKILL)
    pg.wait_for_timeout(32000)
    pg.locator('#project-list button:has-text("proj")').click(); pg.wait_for_timeout(3000)
    ck('#remote=' in pg.url, "page: a session in the list opens it")
    ups2 = subprocess.run(["pgrep", "-P", str(pid), "-f", "/v1/up"], capture_output=True, text=True).stdout.split()
    turns = pg.locator('.turn.model').count()
    pg.fill('#input', 'hey after the upload died'); pg.click('#send')
    try:
        pg.wait_for_function("document.querySelectorAll('.turn.model').length > %d" % turns, timeout=20000)
        ck(len(ups) == 1 and len(ups2) == 1 and ups2 != ups, "desktop: a dead upload reconnects, and the session answers again")
    except Exception as e:
        ck(False, "desktop: a dead upload reconnects, and the session answers again (%s %s %s)" % (ups, ups2, str(e)[:50]))
    ck(not errs, "page: no JS errors %s" % errs[:2])
    b.close()

store = open(W + "/relay/store").read() if os.path.exists(W + "/relay/store") else ""
ck(store.count("\nb ") + store.startswith("b ") >= 2 and "hey" not in store, "relay: stores keys and bindings, no content")
print("\n".join(res))
bad = [r for r in res if r.startswith("FAIL")]
if bad:
    print("logs in", W); sys.exit(1)
print("remote-test: all passed")
