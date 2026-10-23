# Changelog

All notable changes to hako studio (the browser workspace). Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
The page ships two ways: hosted at hako.mithraeum.studio (built nightly from hako-code `main`) and
embedded in `hako --serve`. The agent itself is hako-code; its changes are in that repo's CHANGELOG.

## [v0.0.6] - 2026-10-23

### fixed
- a home-screen app on iOS still left a strip under the composer: there `innerHeight` is a status bar short too, so it sizes to the screen

## [v0.0.5] - 2026-10-23

### changed
- **☰ → code lists your code sessions right in the menu**, with **+ add a device** (how to share a folder with `:remote on`), instead of a popup on every tap. signed out, the menu still offers chat · code: a home-screen app starts with nothing stored and had no way in
- **signing in says what the beta is.** code sessions are a closed beta; the sheet says what an account keeps (a random id, your passkey's public key, your account key locked by it, your devices' public keys and sealed names) and what it never does (chats, files, prompts, provider keys). without an account, chat runs in this browser and keeps its history there. **request access** opens your mail app to contact@mithraeum.studio; nothing is kept. email codes show only when the relay can send them
- **themes preview as you tap them**; closing settings keeps the last one and says so in the status line, never in the chat (it used to post `:theme` per tap, each one a chat line)

### fixed
- a home-screen app on iOS sized the page a status bar short (a strip under the composer), and the closed menu's shadow showed down the left edge
- the page signs in to the relay once per ten minutes instead of on every tap (sign-ins are rate limited)

## [v0.0.4] - 2026-10-22

### added
- **code** (invite beta): sign in with a passkey (face id, a fingerprint) or an emailed code, and ☰ → code lists the code sessions running on your computers, one per shared folder, by folder and machine. open one and the page becomes that session: the same chat as the terminal, every byte sealed by `hako.wasm` before it leaves the tab. ☰ → chat goes back to this browser's own agent; code goes back to the last session. the passkey unlocks your account key on a new device; the relay never sees it. nothing about accounts shows until you are signed in
- `relay/`: the relay itself (C, libcrypto), with `make test`; `make remote-test` runs the whole thing in a real browser

### changed
- `:login` opens the sign-in menu and `:login api` the API-key one, the same rows the terminal shows; bare `:models` and `:providers` open the pickers too. on the hosted page sign-in says it needs `hako --serve`
- the status line follows hako's `state` event; the old "scrape the printed list" picker is gone
- on a remote device (`hako --serve --token`, e.g. over tailscale) the folder browser starts in the project and the system picker is hidden: hako keeps a phone inside the folder it was started in
- the model's reply appears as it is written (hako's `delta` events) and is replaced by the finished message

### fixed
- safari opened the session only after its first keepalive (up to 25 s): what you typed sat there "thinking" and stop did nothing. the relay starts every stream with a few bytes
- a reload mid-turn keeps the turn: stop is offered, the input waits

## [v0.0.3] - 2026-10-15

### changed: the new look (2026-10-12)
- `washi` is the default theme; `mithraeum` is the same inks on a dark ground. 19 themes
- the page is printed in two inks, ultramarine and vermilion on washi; the header carries the コ mark and the home-screen icon is the block katakana ス

### fixed
- **every command failed in a long session** with "Underlying ArrayBuffer has been detached". reading a file took a view of wasm memory and then allocated, and an allocation can grow memory, which detaches the view; each command resumes its session first, so a big enough session broke all of them. pinned by `tools/glue-test.js` (in `make check`)
- **settings showed two themes selected** on a touch screen: the last row tapped keeps `:hover`, and hover was styled exactly like the selection. hover is lighter now, and only on devices that hover
- **safari private windows failed with "The operation failed for an unknown transient reason".** webkit gives a private window no on-device storage; the page now says that, and to open a normal window
- **every past session was dated Dec 31** (browser storage keeps no file times); the date now comes from the session id. sessions with no prompt in them are no longer listed, so a page load does not add an "(empty)" row
- the hosted build's settings no longer offer "where to look for model weights": the browser has no engine and no disk to search. `hako --serve` keeps it
- with no provider chosen, the key notice said the key is "sent only to none"; it now says to pick a provider first
- **a failed tool looked like a success, and in the new look a success looked like a failure.** a tool result starting `error:` now shows in the error color, and tool lines in `washi` and `mithraeum` take the theme's string color instead of vermilion
- **pickers froze the page.** `:model`, `:provider`, `:sessions`, `:theme` and the model's own `ask_user` arrive as `choice_request`; the page had no handler, so the agent waited forever. they render as a choice box now, answered by index, and settle when the turn ends
- **the hosted build was stuck on august's agent.** `hako.c` had stopped compiling for wasm (fixed in hako-code, which now runs `make wasm-check`), so the nightly deploy would have failed as soon as hako-code v0.2.4 reached `main`

### changed
- **all 19 themes**, the same list and names as hake and hako, generated from hake.c (was 4)
- `make serve` sends nothing cacheable (it used python's plain server, which let a stale worker outlive a fresh page); the unused `tools/run-node.mjs` is gone; `session-test.js` loads the wasm the same way
- **the browser bundle is 59% smaller**: 489 KB → 201 KB (gzip 152 → 86 KB), built `-Os` with debug names stripped. every phone that opens the page downloads it

### added
- a share-preview tile (`og.png`) and description, so a link to hako.mithraeum.studio unfurls with a card
- a banner, like the rest of the suite's READMEs
- `make ui-test`; a headless browser drives the page against `hako --serve` and the fake engine: a turn, an approval, the tool's output, the folder picker, two uploads of one name, desktop and phone widths, no JS errors
- `make test`; `check`, plus `ui-test` when playwright and a built hako-code are present
- choice and approval boxes show their verdict with a little space after the question

## [v0.0.2]

The first public release of the page: chat, tool-approval boxes, folder browser, per-folder
sessions, model picker, themes, uploads, add-to-home-screen. Shipped with hako-code v0.2.3,
which carries the details.
