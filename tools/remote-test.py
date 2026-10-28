#!/usr/bin/env python3
"""The relay, end to end, in a real browser. A real `hako` runs `:remote on` in
a pty against a local relay (relay/relay --dev); headless Chromium with a
virtual passkey (prf on) opens the link page, makes the account with an
invite and links the computer. Then the browser forgets everything (a fresh
device), signs in with the passkey, which unwraps the account key, picks the
computer, and runs a turn on it through the relay. Fake engine, no network.

Needs playwright + chromium, a built ../hako-code/hako, relay/relay and a
`make site RELAY_URL=http://127.0.0.1:18793` dist (make remote-test does both)."""
import glob, os, re, sys, time, pty, fcntl, termios, struct, select, signal, subprocess, tempfile
try:
    from playwright.sync_api import sync_playwright
except ImportError:
    sys.exit("pip install playwright && playwright install chromium")
R = os.path.realpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
HAKO = R + "/hako-code/hako"; FAKE = R + "/hako-code/tools/tty/fakehakm.py"
RELAY = R + "/hako-studio/relay/relay"; DIST = R + "/hako-studio/dist-remote"   # its own build: dist/ may be serving a hand test
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
invite, invite2 = subprocess.check_output([RELAY, "invite", "2", "-d", W + "/relay"]).decode().split()
# a fake GitHub and Google: authorize sends the browser straight back with a
# code, and the code (with the client secret) trades for a token that reads
# /user (GitHub) or /v1/userinfo (Google)
GP = 18797
import http.server, urllib.parse, json as _json
GH = {"code": "good", "hits": 0}      # which GitHub user authorize answers for, and how often it was asked
class FakeGitHub(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def answer(self, code, body, loc=None):
        self.send_response(code)
        if loc: self.send_header("Location", loc)
        self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        u = urllib.parse.urlparse(self.path); q = urllib.parse.parse_qs(u.query)
        if u.path == "/login/oauth/authorize" and q.get("client_id") == ["Iv1.test"]:
            GH["hits"] += 1
            return self.answer(302, b"", STUDIO + "/?code=" + GH["code"] + "&state=" + q["state"][0])
        if u.path == "/o/oauth2/v2/auth" and q.get("client_id") == ["g.test"] and q.get("redirect_uri") == [STUDIO + "/"] and q.get("scope") == ["openid"]:
            return self.answer(302, b"", STUDIO + "/?state=" + q["state"][0] + "&code=4%2Fgood&scope=openid&authuser=0&prompt=consent")
        if u.path == "/user" and self.headers.get("Authorization") == "Bearer gho_one":
            return self.answer(200, _json.dumps({"login": "octo", "id": 4242}).encode())
        if u.path == "/user" and self.headers.get("Authorization") == "Bearer gho_two":
            return self.answer(200, _json.dumps({"login": "kat", "id": 5151}).encode())
        if u.path == "/v1/userinfo" and self.headers.get("Authorization") == "Bearer ya29.one":
            return self.answer(200, b'{\n  "sub": "110169484474386276334"\n}')
        self.answer(401, b'{"message":"Bad credentials"}')
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length") or 0)).decode()
        if self.path == "/token":
            f = urllib.parse.parse_qs(body)
            ok = f.get("client_secret") == ["GOCSPX-sek"] and f.get("code") == ["4/good"] and f.get("redirect_uri") == [STUDIO + "/"]
            return self.answer(200 if ok else 400, b'{"access_token": "ya29.one", "id_token": "e30.e30.x"}' if ok else b'{"error": "invalid_grant"}')
        ok = "client_secret=sekret" in body and ("code=good" in body or "code=fresh" in body)
        tok = b"gho_two" if "code=fresh" in body else b"gho_one"
        self.answer(200, b'{"access_token":"' + tok + b'","token_type":"bearer"}' if ok else b'{"error":"bad_verification_code"}')
import threading
threading.Thread(target=http.server.ThreadingHTTPServer(("127.0.0.1", GP), FakeGitHub).serve_forever, daemon=True).start()
open(W + "/relay/github", "w").write("Iv1.test sekret\n")
open(W + "/relay/google", "w").write("g.test GOCSPX-sek\n")
bg([RELAY, "-p", str(RP), "-d", W + "/relay", "-o", STUDIO, "-l", "300", "--dev", "-G", "http://127.0.0.1:%d" % GP, "-v"])
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
ck("\u2588\u2580\u2580\u2580\u2580\u2580\u2588".encode() in seen.split(b"#link=")[0].split(b"link this computer")[-1], "desktop: and a QR code of it, for a phone")
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
    ck(host in row.inner_text() and pg.locator('.modal .opt:not(:has-text("add a device"))').count() == 1
       and "signed in with a passkey" in pg.locator('.modal').inner_text() and "signed in with a passkey" in pg.inner_text('#acct'),
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
    ck(not pg.evaluate("performance.getEntriesByType('resource').some((e) => e.name.includes('/v1/auth/'))"),
       "page: a reload keeps this tab's sign-in, no trip to sign in again")
    pg.fill('#input', ':auto'); pg.click('#send')
    pg.wait_for_timeout(2000)
    ck("not from a remote device" in pg.inner_text('#log'), "page: :auto is refused from a remote device")
    remote_url = pg.url
    # the same session from Safari's engine: Chromium (signed in) binds a WebKit
    # device to the account, which leaves it signed in without K (as an email or
    # SSO sign-in would); :remote pair's code hands it K
    K = pg.evaluate("(async () => hex(await kvGet('k')))()")
    wb = p.webkit.launch(); wp = wb.new_context(viewport={'width': 390, 'height': 844}).new_page()
    werrs = []; wp.on('pageerror', lambda e: werrs.append(str(e)))
    wlog = []
    if os.environ.get("DEBUG"):
        wp.add_init_script("window.hakoDebug = 1")
        wp.on('console', lambda m: wlog.append("%.2f %s" % (time.time() % 100, m.text)))
    wp.goto(STUDIO + "/"); wp.wait_for_timeout(1500)
    wpub = wp.evaluate("(async () => { await relay.signIn(); return hex(relay.pub); })()")
    ck(pg.evaluate("(async (k) => { await relay.signIn(); return (await relay.req('POST', '/v1/bind', k)).status; })(%r)" % wpub) == 200,
       "webkit: a second device joins the account, signed in without the account key")
    mark = len(seen)
    os.write(fd, b":remote pair\r")
    pm = until(rb"(http://localhost:\d+/#pair=([0-9a-f]{32})\.([0-9a-f]{64}))")
    ck(pm is not None and "\u2580".encode() in seen[mark:] and "\u2584".encode() in seen[mark:],
       "desktop: :remote pair draws a code and prints its address")
    if pm:
        wp.goto(pm.group(1).decode())
        try: wp.wait_for_selector('.modal .note:has-text("added")', timeout=15000)
        except Exception: pass
        ck(wp.evaluate("(async () => hex(await kvGet('k') || new Uint8Array(0)))()") == K and '#pair=' not in wp.url,
           "webkit: the code hands this browser the account key, and leaves the address bar")
        ck(until(rb"remote: paired") is not None, "desktop: says a browser was paired")
        again = wp.evaluate("(async (a) => { try { await pairDevice(a[0], a[1]); return 'took it'; } catch (e) { return e.message; } })(%s)"
                            % [pm.group(2).decode(), pm.group(3).decode()])
        ck(again.startswith("no answer"), "webkit: a code works once (%s)" % again[:40])
        ck("#pair=" not in pg.inner_text('#log'), "page: the pair code never reaches the attached device")
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
    ck("SLOW from safari" in wp.inner_text('#log'), "webkit: a reload brings the transcript back")
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
    # sign-ins at once share one: the relay keeps one token per device, so the
    # page must not hold the one it replaced (the 401 after a GitHub/Google return)
    raced = pg.evaluate("""(async () => {
        const req = relay.req;
        let asked = 0;
        relay.req = (m, path, body) => { if (path === '/v1/auth/challenge') asked++; return req.call(relay, m, path, body); };
        relay.forget();
        try { await Promise.all([relay.signIn(), relay.signIn(), relay.signIn()]); }
        catch (e) { return [e.message]; } finally { relay.req = req; }
        const r = await relay.req('POST', '/v1/webauthn/challenge');
        return asked === 1 && r.status === 200 ? [] : ['challenges ' + asked, 'status ' + r.status];
    })()""")
    ck(raced == [], "page: sign-ins at once share one, and keep the token the relay holds %s" % raced[:3])
    ic = b.new_context(viewport={'width': 390, 'height': 844}); ip = ic.new_page()
    ip.goto(STUDIO + "/#invite=abcde-fghij")
    try: ip.wait_for_selector('.modal', timeout=10000); ip.wait_for_timeout(1500)
    except Exception: pass
    heads = ip.locator('.modal h2').all_inner_texts()
    ck(len(heads) == 1 and "browser" not in heads[0], "page: an invite link opens the account sheet with no notice over it %s" % heads)
    ic.close()
    # GitHub, then Google: Chromium (signed in) adds that id to the account; a
    # fresh browser scans a pair code, signs in with it, comes back and is paired
    for sp, sname, scode in (("github", "GitHub", "good"), ("google", "Google", "4/good")):
        added = pg.evaluate("""(async ([p, code]) => {
            await relay.signIn();
            const s = await relay.req('POST', '/v1/' + p + '/start');
            const st = new URL(s.text).searchParams.get('state');
            return (await relay.req('POST', '/v1/' + p + '/verify', code + ' ' + st + ' -')).text;
        })""", [sp, scode])
        ck(added.split(" ")[0] == pg.evaluate("relay.acct") and added.endswith("old"), "%s: a signed-in browser adds %s to its account" % (sp, sname))
        gc = b.new_context(viewport={'width': 390, 'height': 844}); gp = gc.new_page()
        gerrs = []; gp.on('pageerror', lambda e: gerrs.append(str(e)))
        os.write(fd, b":remote pair\r"); time.sleep(1.5)
        gm = re.findall(rb"http://localhost:\d+/#pair=[0-9a-f]{32}\.[0-9a-f]{64}", seen)
        if gm:
            gp.goto(gm[-1].decode())
            try:
                gp.click('.modal button:has-text("sign in with %s")' % sname, timeout=10000)
                gp.wait_for_selector('.modal .note:has-text("added")', timeout=15000)
            except Exception: pass
            ck(gp.evaluate("(async () => hex(await kvGet('k') || new Uint8Array(0)))()") == K
               and gp.evaluate("relay.acct") == pg.evaluate("relay.acct"),
               "%s: a fresh browser signs in with %s mid-pairing and gets the account key" % (sp, sname))
            ck("code=" not in gp.url and "#pair=" not in gp.url, "%s: the code and the pair secret leave the address bar" % sp)
            ck(gp.locator('#acct svg').count() == 1 and ("signed in with " + sname) in gp.inner_text('#acct')
               and ("signed in with " + sname) in gp.locator('.modal').inner_text(),
               "%s: the menu foot and the sheet say signed in with %s, with its mark" % (sp, sname))
            gdev = gp.evaluate("relay.id")
            gp.locator('.modal button:has-text("sign out")').click(); gp.wait_for_timeout(1500)
            ck(gp.evaluate("(async () => { await relay.signIn(); return relay.acct + '|' + (await kvGet('k') ? 'k' : '') + '|' + (relay.id !== %r); })()" % gdev) == "||true"
               and "sign in" in gp.inner_text('#acct')
               and gdev not in pg.evaluate("(async () => (await relay.req('GET', '/v1/devices')).text)()"),
               "%s: sign out drops the device from the account and forgets the key; the menu says sign in" % sp)
            gp.evaluate("document.getElementById('acct-btn').click()")
            gp.wait_for_selector('.modal h2:has-text("sign in")', timeout=10000)
            ck(gp.locator('.modal .signin button:has-text("sign in with %s") svg' % sname).count() == 1,
               "%s: signed out, the menu foot opens sign in, the %s button carries its mark" % (sp, sname))
        else: ck(False, "%s: a second :remote pair code" % sp)
        ck(not gerrs, "%s: no JS errors %s" % (sp, gerrs[:2]))
        gc.close()

    # a new GitHub user: the invite finishes the sign-in (no second trip), the new
    # account's key is made here, and one browser pairs another with its own code
    kq = "(async () => hex(await kvGet('k') || new Uint8Array(0)))()"
    GH["code"] = "fresh"
    nerrs = []
    def fresh():
        c = b.new_context(viewport={'width': 390, 'height': 844}); q = c.new_page()
        q.on('pageerror', lambda e: nerrs.append(str(e))); return c, q
    nc, np_ = fresh()
    np_.goto(STUDIO + "/#signin")
    np_.click('.modal button:has-text("sign in with GitHub")', timeout=10000)
    try: np_.wait_for_selector('.modal button:has-text("continue with GitHub")', timeout=15000)
    except Exception: pass
    hits = GH["hits"]
    ck("needs an invite code" in np_.locator('.modal').inner_text() and np_.locator('.modal input').first.is_visible(),
       "invite: a new GitHub user is asked for an invite, with continue")
    np_.fill('.modal input >> nth=0', invite2)
    np_.click('.modal button:has-text("continue with GitHub")')
    try: np_.wait_for_selector('.modal .acctinfo', timeout=15000)
    except Exception: pass
    nacct = np_.evaluate("relay.acct")
    ck(nacct and nacct != pg.evaluate("relay.acct") and GH["hits"] == hits and "signed in with GitHub" in np_.locator('.modal').inner_text(),
       "invite: continue finishes it, a new account, GitHub not asked again")
    nk = np_.evaluate(kq)
    named = np_.evaluate('''(async () => { const K = await kvGet('k'), r = await relay.req('GET', '/v1/devices');
        const l = r.text.split('\\n').find((x) => x.startsWith(relay.id + ' '));
        if (!l || l.split(' ')[3] === '-') return 'unnamed: ' + r.status + ' ' + r.text.slice(0, 300) + ' id ' + relay.id;
        const b = unb64u(l.split(' ')[3]);
        const n = hk.run('hkw_name_open', cat(K, b), 32 + b.length); return n > 0 ? new TextDecoder().decode(hk.out(n)) : ''; })()''')
    ck(len(nk) == 64 and named.startswith("browser:") and "does not hold" not in np_.locator('.modal').inner_text(),
       "key: a new account's key is made here, and the browser lists itself sealed (%s)" % named)
    np_.locator('.modal .opt:has-text("add a device")').click()
    np_.wait_for_selector('.modal svg.qr', timeout=10000)
    plink = np_.locator('.modal input.pairlink').input_value()
    ck(re.search(r"#pair=%s\.[0-9a-f]{64}$" % np_.evaluate("relay.id"), plink) is not None,
       "pair: add a device on a browser shows a QR and a link to its own pair code")
    xc, xp = fresh()
    xp.goto(plink)
    try:
        xp.click('.modal button:has-text("sign in with GitHub")', timeout=10000)
        xp.wait_for_selector('.modal .note:has-text("added")', timeout=15000)
        np_.wait_for_selector('.modal .note:has-text("added. it holds")', timeout=5000)
    except Exception: pass
    ck(xp.evaluate(kq) == nk and xp.evaluate("relay.acct") == nacct and np_.locator('.modal svg.qr').count() == 0,
       "pair: another browser opens it, signs in, and gets the key from the first browser")
    np_.locator('.modal button:has-text("close")').last.click()
    # a holder is left (xp): a third browser waits for a pairing; once none is left, it makes the key
    np_.evaluate("hakoAccount.signOut()"); np_.wait_for_timeout(1000)
    yc, yp = fresh()
    yp.goto(STUDIO + "/#signin")
    yp.click('.modal button:has-text("sign in with GitHub")', timeout=10000)
    try: yp.wait_for_selector('.modal .acctinfo', timeout=15000)
    except Exception: pass
    ck(yp.evaluate("relay.acct") == nacct and yp.evaluate(kq) == "" and "does not hold" in yp.locator('.modal').inner_text(),
       "key: while another browser holds it, a new one waits for a pairing")
    yp.locator('.modal button:has-text("close")').last.click()
    xp.evaluate("hakoAccount.signOut()"); xp.wait_for_timeout(1000)
    yp.evaluate("document.getElementById('acct-btn').click()")
    try: yp.wait_for_selector('.modal .acctinfo', timeout=10000)
    except Exception: pass
    yk = yp.evaluate(kq)
    ck(len(yk) == 64 and yk != nk and "does not hold" not in yp.locator('.modal').inner_text(),
       "key: once no device or passkey holds it, the browser makes a new one")
    ck(not nerrs, "invite/pair: no JS errors %s" % nerrs[:2])
    for c in (nc, xc, yc): c.close()
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
    told = seen.count(b"a device on your account connected")
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
        ck(seen.count(b"a device on your account connected") == told,
           "desktop: a device it already announced reconnects quietly after its own relay link dropped")
    except Exception as e:
        ck(False, "desktop: a dead upload reconnects, and the session answers again (%s %s %s)" % (ups, ups2, str(e)[:50]))
    # the page's own stream dies (a network change, a phone waking up): another stream for the
    # same device takes its place at the relay, then goes away. The page must come back by itself
    pg.evaluate("""(() => { const c = new AbortController();
        fetch(RELAY_URL + '/v1/down', { headers: { Authorization: 'Bearer ' + relay.token }, signal: c.signal })
            .then(() => setTimeout(() => c.abort(), 300), () => {}); })()""")
    pg.wait_for_timeout(7000)
    ck(pg.locator('.modal').count() == 0 and "hey after the upload died" in pg.inner_text('#log'),
       "page: a dropped connection comes back by itself, transcript and all, no dialog")
    turns = pg.locator('.turn.model').count()
    try:
        pg.fill('#input', 'hey after the drop'); pg.click('#send', timeout=5000)
        pg.wait_for_function("document.querySelectorAll('.turn.model').length > %d" % turns, timeout=20000)
        ck(True, "page: after coming back, a turn answers")
    except Exception as e:
        ck(False, "page: after coming back, a turn answers (%s)" % str(e)[:60])
    # the relay replaced the page's token (this device signed in somewhere else): the stream
    # drops, the reconnect gets a 401, signs in again and comes back
    emark = len(errs)
    stale = pg.evaluate("""(async () => { const old = relay.token, at = relay.at;
        relay.forget(); await relay.signIn(); const fresh = relay.token;
        Object.assign(relay, { token: old, at }); relay.keep(Object.assign(relay.kept(), { token: old }));
        const c = new AbortController();
        fetch(RELAY_URL + '/v1/down', { headers: { Authorization: 'Bearer ' + fresh }, signal: c.signal })
            .then(() => setTimeout(() => c.abort(), 300), () => {});
        return old; })()""")
    pg.wait_for_timeout(8000)
    errs[emark:] = [e for e in errs[emark:] if "status of 401" not in e]   # the browser logs the 401 it was meant to get
    turns = pg.locator('.turn.model').count()
    try:
        pg.fill('#input', 'hey with a new token'); pg.click('#send', timeout=5000)
        pg.wait_for_function("document.querySelectorAll('.turn.model').length > %d" % turns, timeout=20000)
        ck(pg.evaluate("relay.token") != stale and pg.locator('.modal').count() == 0,
           "page: a token the relay replaced is dropped on the 401, signed in again, and the session answers")
    except Exception as e:
        ck(False, "page: a token the relay replaced is dropped on the 401, signed in again, and the session answers (%s)" % str(e)[:60])
    # an image pasted on the phone crosses the relay sealed and lands in the desk's images dir;
    # 600 KB of noise, so it spans many frames
    pg.evaluate("""(() => { const n = 600000, b = new Uint8Array(n); for (let i = 0; i < n; i++) b[i] = (i * 2654435761) >>> 24;
        b.set([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
        const dt = new DataTransfer(); dt.items.add(new File([b], 'image.png', { type: 'image/png' }));
        $('input').dispatchEvent(new ClipboardEvent('paste', { clipboardData: dt, bubbles: true, cancelable: true })); })()""")
    turns = pg.locator('.turn.model').count()
    try: pg.wait_for_function("$('send').textContent.trim() === 'send'", timeout=15000)
    except Exception: pass
    pg.fill('#input', 'see this'); pg.click('#send')
    try: pg.wait_for_function("document.querySelectorAll('.turn.model').length > %d" % turns, timeout=30000)
    except Exception: pass
    imgs = glob.glob(HOME + "/.hako/projects/*/images/*.png")
    ck("see this [image #1]" in pg.inner_text('#log') and any(os.path.getsize(i) == 600000 for i in imgs),
       "page: a pasted image crosses the relay whole and the message names it (%s | tray %r | %r)"
       % ([os.path.getsize(i) for i in imgs], pg.inner_text('#tray'), pg.inner_text('#log')[-160:]))
    ck(not errs, "page: no JS errors %s" % errs[:2])
    b.close()

store = open(W + "/relay/store").read() if os.path.exists(W + "/relay/store") else ""
ck(store.count("\nb ") + store.startswith("b ") >= 2 and "hey" not in store, "relay: stores keys and bindings, no content")
ck(store.count("\nl ") + store.startswith("l ") == 3 and not re.search("4242|5151|octo|kat |110169|ya29|GOCSPX", store), "relay: GitHub and Google are one keyed hash each, no id, name or token")
print("\n".join(res))
bad = [r for r in res if r.startswith("FAIL")]
if bad:
    print("logs in", W); sys.exit(1)
print("remote-test: all passed")
