<p align="center">
  <a href="https://mithraeums.github.io">
    <b>hako studio</b>
  </a>
</p>

<p align="center">
  <em>The hako agent in a browser tab. One binary, your files, your models, any device on your network.</em>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/version-v0.0.2-b89656?style=flat-square&labelColor=14130f" alt="v0.0.2"/>
  <img src="https://img.shields.io/badge/license-GPL--3.0-c8c2b2?style=flat-square&labelColor=14130f" alt="GPL-3.0"/>
  <img src="https://img.shields.io/badge/UI-single%20file-c8c2b2?style=flat-square&labelColor=14130f" alt="single file UI"/>
  <img src="https://img.shields.io/badge/deps-none-c8c2b2?style=flat-square&labelColor=14130f" alt="no dependencies"/>
</p>

<p align="center">
  <sub><a href="https://mithraeums.github.io">site</a> &nbsp;·&nbsp; <a href="https://github.com/mithraeums/hako-code">hako-code</a> &nbsp;·&nbsp; <a href="https://github.com/mithraeums/hako">hako (models)</a> &nbsp;·&nbsp; <a href="https://github.com/mithraeums/hako-edit">hako-edit</a> &nbsp;·&nbsp; <a href="https://github.com/mithraeums">org</a></sub>
</p>

<br>

<!-- Screenshots live in the website repo (assets/readme-screenshots/hako-studio/) like the other repos. -->

<table>
  <tr>
    <td width="33%" valign="top"><b>No backend.</b><br/><sub>Static files and your own device. No database, no account, no telemetry, nothing proxied. A conversation, a key and a file never reach a server of ours — there isn't one.</sub></td>
    <td width="33%" valign="top"><b>One agent, three shapes.</b><br/><sub>The same <code>hako.c</code> compiled natively and to WebAssembly. Hosted page, container, or <code>hako --serve</code> — same loop, same tools, same session format. Nothing reimplemented in JavaScript.</sub></td>
    <td width="33%" valign="top"><b>Every device you own.</b><br/><sub>Run it from the machine with your files and drive it from the couch, or open the page on a phone and let the agent run there. <code>--token</code> when it leaves the loopback.</sub></td>
  </tr>
</table>

<p align="center"><sub><b>—— I ——</b></sub></p>

## Overview

- **Three ways to run it, one piece of software.** `hako.c` is compiled twice —
  natively for the binary, and to WebAssembly for the browser. What differs is
  only what sits underneath, which is what decides whether there is a shell and
  a real filesystem.

  | | how | files | shell · local models |
  |---|---|---|---|
  | **hosted** | a static page, e.g. `hako.mithraeum.studio` | browser storage, or a folder you grant (Chromium desktop) | no |
  | **container** | `docker run -p 8080:80 hako-studio` | same as hosted — the agent runs in *your* browser, not in the container | no |
  | **binary** | `hako --serve` | real folders on that machine | yes |

- **What the browser build cannot do**, because a browser sandbox does not permit
  it: run a shell, run local hako models (a subprocess), or complete an OAuth
  sign-in (provider token endpoints refuse cross-origin requests). It takes API
  keys, and those go from the page straight to the provider. Work is kept in
  browser storage unless a real folder is granted — **clearing site data erases
  it**, so save what matters out of the files list.
- **It *is* the agent.** `hako --serve` is a mode of the agent binary, not a wrapper around it:
  the same loop, providers, tools and session format that run in the terminal, carried to the
  browser over HTTP + Server-Sent Events. Nothing is reimplemented in JavaScript, and there is
  no child process to keep alive. Update `hako`, and studio updates with it. In the browser the
  same code runs as WebAssembly against a platform seam — a JS reimplementation of the agent
  loop is the one change this project will not take.
- **Shared state with the CLI** *(binary)*. Sessions, `~/.hako/credentials`, skills, `~/.hakorc`
  and per-project trust are the same files the terminal uses. `:resume` in either surface picks
  up the other's conversation. In the browser the same layout exists, in that origin's storage.
- **Tool calls ask first** *(binary)*. Every `write_file`, `edit_file`, `run_shell` and read
  pauses for **allow once · deny · always**, mirroring the CLI's `y`/`n`/`a` scoping —
  project-wide for reads and writes, exact-command for shell. A wasm turn cannot pause to ask,
  so the browser build does not gate: there the boundary is the sandbox itself.
- **Any folder, per-folder sessions.** Browse, open a project, and the sidebar lists that
  project's conversations. Resume renders the full stored transcript; delete removes the jsonl.
  A session belongs to a directory, never to a provider or a model.
- **Local models first** *(binary)*. Installed weights, loose `.mlf2` files found in `~/Downloads`
  or any folder you add (symlinked into place — never copied, never moved), and `:pull` for the
  rest. Cloud catalogs come from the provider. The browser build lists only what a page can
  reach, so local models and OAuth providers are hidden there rather than offered and broken.
- **Four themes and a thinking indicator.** Five spinner styles with elapsed time, because a
  local 3B turn runs for minutes with nothing streamed back. Both persist on your device.
- **Works on a phone.** Drawer navigation, touch targets, keyboard-aware layout, add-to-home-screen.
  Serve it from the machine you are sitting at and drive it from the couch — or open the hosted
  page and let the agent run on the phone itself.
- **Nothing to install.** libc and pthread. No framework, no CDN, no build step for the page.
  The UI is one HTML file, compiled into the binary — so a single executable is the whole
  product — and the same file is what the hosted page and the container serve.

<p align="center"><sub><b>—— II ——</b></sub></p>

## Build & Run

### The page and the container

```sh
make            # build hako.wasm and assemble dist/
make serve      # dist/ on http://localhost:8080
make docker     # container image serving dist/
make check      # bundle sanity + the wasm behaviour tests
```

The toolchain is clang. `make toolchain` fetches wasi-sdk (~175 MB) into
`~/.hako/toolchains`; remove it with `rm -rf`. No emscripten, no npm, no
framework, and no build step for the page itself.

The wasm is built from [`hako-code`](https://github.com/mithraeums/hako-code):
a checkout beside this one wins, otherwise the released source is fetched.
**That source must carry the platform seam** (`hk_http` / `hk_fs` / `hk_shell` /
`hk_time`) — hako-code v0.2.3 or newer. Against anything older the wasm build
fails on a missing POSIX header.

### From the binary

The fullest of the three: real folders, a shell, local models. The UI is served
by [`hako`](https://github.com/mithraeums/hako-code) itself:

```sh
hako --serve --dir ~/code/my-project
# open http://127.0.0.1:8787
```

```
--serve        start hako studio
--port N       listen port (default 8787)
--bind ADDR    bind address (default 127.0.0.1 — anything else exposes the agent)
--dir PATH     project to open
--web DIR      serve DIR/index.html instead of the built-in bundle (UI work)
--yolo         auto-approve every tool call
```

**On a phone or tablet on the same network:** run `hako --serve --lan --dir ~/code/my-project`,
open the `phone` URL it prints, and add it to your home screen.

**Add `--token`.** `--serve` has no password; anyone who can reach the port can
run tools as you. `--token` requires a secret on every request and prints a URL
carrying it once, which the browser exchanges for a cookie:

```sh
hako --serve --lan --token --dir ~/code/my-project
```

That is also what makes it safe to reach your own machine from anywhere through
a tunnel — `cloudflared tunnel --url http://localhost:8787` gives an HTTPS URL
with no ports opened and no account.

### Serving to a phone on macOS

macOS blocks inbound connections to an **unsigned** binary while leaving loopback alone, and it
does it silently: the TCP handshake completes, then the socket is dead by the first read. The
phone spins forever, nothing is logged, and every layer above looks broken. Under `--debug` the
tell is `read=-1 errno=57` (ENOTCONN) — or no request at all.

`make` therefore signs the binary on macOS, and prefers a stable identity over an ad-hoc one:

```sh
# once — a self-signed code-signing identity called "hako-dev"
D=~/.hako/codesign && mkdir -p $D && chmod 700 $D && cd $D
openssl req -new -newkey rsa:2048 -nodes -keyout hako-dev.key -x509 -days 3650 \
  -out hako-dev.crt -subj "/CN=hako-dev/O=mithraeum" \
  -addext "extendedKeyUsage=codeSigning" -addext "basicConstraints=critical,CA:false"
openssl pkcs12 -export -inkey hako-dev.key -in hako-dev.crt -out hako-dev.p12 \
  -passout pass:hakodev -name hako-dev -certpbe PBE-SHA1-3DES -keypbe PBE-SHA1-3DES -macalg sha1
security import hako-dev.p12 -k ~/Library/Keychains/login.keychain-db -P hakodev -T /usr/bin/codesign
```

Then allow it through the firewall **once** (the firewall stays on — this is one entry):

```sh
H=/full/path/to/hako
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --add "$H"
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --unblockapp "$H"
```

The first `make` after that raises a keychain prompt — answer **Always Allow**, not "Allow".

Why the certificate rather than the ad-hoc signature `codesign -s -` gives you: the firewall
matches a *designated requirement*. With a certificate that is `identifier + certificate leaf`,
which is stable, so the allow entry survives every rebuild. Ad-hoc signing is a bare code hash,
so every single rebuild invalidates the entry and the phone hangs again. Two flags apart, and
the difference between "set it up once" and "sudo before every test".

macOS notes:
- Signing and the Rez icon are mutually exclusive — a resource fork makes `codesign` refuse the
  binary — so the signed build carries no icon. `make SIGN=0` restores the old behaviour.
- Loopback (`hako --serve` with no `--lan`) never needs any of this.

Working on the UI itself:

```sh
make dev        # run the agent with the UI served off disk — edit, reload, no rebuild
make embed      # index.html -> ../hako-code/hako_web.h (what the binary carries)
make build      # embed, then rebuild the agent
make check      # bundle sanity: one file, nothing external
```

<p align="center"><sub><b>—— III ——</b></sub></p>

## Interface

| Area | What it does |
|---|---|
| **sidebar** | working directory · `open folder…` · settings · sessions in this folder · new session · reload |
| **session row** | click to resume with full transcript; `✎` renames it; `×` deletes the stored jsonl |
| **approval box** | `allow once` · `deny` · `always` — answered boxes stay settled across a reload |
| **header** | live provider/model and turn count, read from the agent's own output |
| **files** | browse the machine's folders and files; tap a file to put its name in your message; **add from device** uploads from the phone's Files/Photos into the open project |
| **settings** | theme · thinking indicator · extra folders to search for model weights · forget local settings |

Uploads land in the open project under a sanitized basename, never overwrite (collisions become
`-1`, `-2`), cap at 128 MB, stream to disk, and announce themselves in the transcript so the agent
knows the file arrived. Add the page to your home screen and it runs chromeless with its own icon.

`:login` and friends need a terminal — over `--serve` they answer "run this in the shell" rather
than parking the agent on a stdin nobody can reach.

Type `:` commands exactly as in the CLI — `:trust`, `:usage`, `:sessions`, `:pull <model>`,
`:help`. Bare `:model`, `:provider` and `:theme` open the browser's own pickers, because the
CLI's versions read the terminal directly.

On a desktop Enter sends and Shift+Enter adds a line; on a touch device Enter is a newline and
`send` sends. While a turn is running that button becomes `stop`. Under 720px the sidebar is a
drawer, and the UI asks the agent what it supports before offering it — an older `hako` simply
shows fewer buttons.

<p align="center"><sub><b>—— IV ——</b></sub></p>

## Where things are kept

Running from the binary, on that machine's disk:

```
~/.hako/projects/<encoded-cwd>/sessions/   conversations (shared with the CLI)
  <id>.jsonl                               the transcript, written by hako
  <id>.title                               optional name you gave it (sidecar)
~/.hako/credentials                        provider keys and OAuth tokens
~/.hako/models/<id>/<id>.mlf2              local weights
browser localStorage                       theme, spinner, model search paths
```

Running in a browser, in that origin's private storage — the same paths, in a
place no file manager can open:

```
/workspace                                 your files, unless a folder is granted
/hako/.hakorc                              provider and model
/hako/.hako/credentials                    your API key, on this device only
/hako/.hako/projects/<encoded-dir>/…       sessions, exactly as above
```

**Clearing site data erases all of it.** Save anything worth keeping out of the
files list; on iOS that lands in Files. Granting a real folder (Chromium
desktop) puts your work on disk instead, while the key stays on the device and
is never written into the folder.

The server holds nothing. It keeps a 2048-event replay buffer in memory so a reload restores
the current view, and that is all — no logging, no history of its own, nothing written to disk
that `hako` did not write.

What you send to a **cloud** model goes to that provider under your own key. Local models go
nowhere.

<p align="center"><sub><b>—— V ——</b></sub></p>

## Security

- **Loopback by default.** `--bind` to anything else exposes the agent: anyone who can reach
  the port can run tools as you. `--token` requires a secret on every request; without it, use
  a network you trust or a VPN.
- **`--yolo` approves everything**, including shell commands and file writes, with no prompt.
  The approval boxes exist so you don't need it.
- Cross-origin POSTs are rejected. Session ids are validated against path traversal.
- `hako`'s own trust gate still applies: an untrusted directory refuses every tool until
  `:trust`.
- **In the browser these two are different by necessity.** A wasm turn cannot pause to ask, so
  the approval gate and the trust gate do not apply there. The boundary is the browser itself:
  no shell exists, and the only reachable files are this origin's storage or a folder the user
  granted through the browser's own picker.

<p align="center"><sub><b>—— VI ——</b></sub></p>

## Contributing

If you share the belief that simplicity empowers creativity, feel free to contribute.

- Forking this repo
- Submitting a Pull Request
- Bug reports and feature requests

Please ensure your code follows the existing style: C99, tabs, `snake_case` fields, no
third-party libraries, and no agent logic in JavaScript — that belongs in `hako.c`.

<p align="center"><sub><a href="LICENSE">— SEE LICENSE —</a> &nbsp;·&nbsp; GPL-3.0</sub></p>

<p align="center"><sub><em>— deus sol invictus mithras —</em></sub></p>
