# Changelog

All notable changes to hako studio (the browser workspace). Format: [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
The page ships two ways: hosted at hako.mithraeum.studio (built nightly from hako-code `main`) and
embedded in `hako --serve`. The agent itself is hako-code; its changes are in that repo's CHANGELOG.

## [Unreleased]

## [v0.1.0] - 2026-10-29

### added
- ↑ and ↓ in the composer walk what you sent (from the first or last line, so a long draft still moves), and a resumed chat's lines are in it too
- replies render their markdown: headings, lists and task boxes, `code`, code blocks, quotes, tables, links. built as page elements, never as HTML, so nothing a model writes can run in the page
- a new menu: the wordmark at the top (HAKO in chat, CODE in code), two small icons to switch chat and code, then new, search, files and settings, then recents, then your account. search filters the sessions as you type
- the menu folds away entirely and a button in the header brings it back; ctrl+b (cmd+b on a Mac) does both. this browser remembers which. folded, the header shows the wordmark. on a phone it stays a drawer
- an empty chat is a landing: a greeting for the time of day (dawn, morning, afternoon, evening, late night, a few of each), the composer centered under it, and the working folder's newest files as cards. a tap on a card adds the file to your message. the first message slides the composer down to the foot (not with reduced motion)
- code, before a session is open, is a landing too: pick a folder on one of your computers, add a device, and type what to do. sending opens that session and the message goes with it
- under the composer: the folder, the model and the mode (ask, plan, auto), each opening its picker. auto stays at the computer itself
- a command menu over the composer when a line starts with `:` or `/`, from the commands hako sends (`:` runs hako, `/` drives the model, each with its category). arrows move, tab completes, enter runs, esc closes only the menu. a hako that sends no list gets the old behavior
- read aloud: a small speaker on each reply, a second tap stops it. in settings (or `voice=1` in your `.hakorc`) every reply is read. only this device's own voices, so nothing is sent anywhere; code blocks are skipped and a new message stops it
- the header says, quietly, when the session is shared (`:remote on`) and when a newer hako is out
- a session's name from hako shows in the header and the list. ✎ on the open session of a code session names it through hako (`:name`), so the terminal and every page agree; the command and its answer stay out of the chat
- the rc's theme reaches the page (hako sends it), a custom `theme.<name>.<slot>` one too
- the files sheet adds a tapped file to your message; in a code session it stays inside that folder

### changed
- washi and mithraeum print in one ink: sumi on washi, and bone on near-black for mithraeum. no blue, no red. the icon, `favicon.ico` and the share tile follow
- the default theme is `terminal`: a plain neutral page that follows your system's light or dark, with none of our inks. the 19 others are all still in settings, and a theme picked before stays picked
- the composer is one rounded box with + and send inside, centered with the chat at a readable width; your messages sit in a soft block on the right
- the chat tab no longer has the CODE block with add a device on top. code sessions are listed in recents (▣) above this browser's chats (○); add a device lives in the code landing and the account sheet
- reload this folder moved into settings

### fixed
- on a phone, a long session title ran under the HAKO/CODE wordmark in the header; the mark keeps its width now (pinned in `make ui-test`)
- hako.mithraeum.studio kept showing the old icon. the server sent the ス, but browsers keep an icon by its address and the new one had kept the old address plus `?v=2`. the build now names it by its checksum, the manifest points at that name, and `favicon.ico` and `apple-touch-icon.png` answer at the root. an icon already on a home screen stays until it is added again
- a chat with no message in it ("(empty)") was still listed when it was the open one
- on an iPhone, a swipe with the keyboard open could scroll the page itself, leaving the bar at the top and a keyboard's height of blank under it. the page no longer scrolls under the keyboard, a swipe outside the chat cannot move it, and it re-pans whenever the visible area moves
- files in the browser had no modified time, so "newest" meant nothing; they keep one now

## [v0.0.9] - 2026-10-28

### added
- **images**: paste a screenshot, drop a file or pick a photo with +, and it waits on the tray as a ▣ chip. send, and the model sees it with your message as `[image #1]`. a phone photo is shrunk to 1568 px on the device first, so a code session sends hundreds of KB through the relay, not 10 MB. needs a hako with the `image` capability; the browser's own agent cannot take images yet and says so
- a message can be only an image, no words needed

### fixed
- a code session whose connection dropped (a network change, a phone back from sleep) stopped with "the connection dropped" and a reconnect button that reloaded the page. it now reconnects by itself, says "reconnecting" in the status line while it tries, and the transcript comes back with it. only a key problem still stops with a dialog
- a phone back from sleep could sit on a connection that had died without a word, looking connected but hearing nothing until the next message went out. more than 40 seconds of silence (the relay writes every 25) now counts as dropped
- opening a code session (a reload, a phone coming back) took about ten trips to the relay one after another; it takes five now, about half the wait at a phone's usual 100 ms round trip (measured: 1.0 s → 0.56 s). a reload keeps this tab's sign-in instead of signing in again, the page no longer waits a fixed 150 ms before the handshake, requests made in the same moment go up together, and the same list asked for from several places at once is fetched once
- a reconnect holding a token the relay had replaced (this device signed in again somewhere) kept failing for up to ten minutes; the 401 now drops it and signs in again
- on an iPhone, opening the keyboard threw the input bar up to the top of the screen with blank space under it. iOS slides the visible part of the page down to show the field, and the page shrank to fit the keyboard but stayed at the top. it now follows the slide, so the bar sits right on the keyboard

## [v0.0.8] - 2026-10-27

### added
- a `:remote pair` code opens this page as "add this device": sign in (an emailed code is fine) and the browser gets your account key from that computer, so code sessions open here too. a signed-in browser without the key now says how to get it
- sign in with GitHub, once the relay is set up for it (`relay/deploy/deploy.sh github`). the relay keeps a keyed hash of your GitHub account id, never the id, your name or a token, and asks for no GitHub permissions. like an emailed code it does not carry your account key: use it mid `:remote pair`, or sign in with a passkey once
- sign in with Google, the same way (`relay/deploy/deploy.sh google`). it asks Google for `openid` only, so the relay sees nothing but the account's id, and keeps only a keyed hash of that
- relay: `POST /v1/github/start`, `/v1/github/verify` and the same two under `/v1/google/`; health says `github` and `google` when they are on. the client secrets and the tokens go to curl on stdin, never its arguments
- an account row at the foot of the menu: "sign in", or who is signed in (GitHub, Google, a passkey or email, with that mark) and the first characters of the account id. it opens the account sheet: your code sessions, add a device, switch account, sign out
- sign out drops this browser from the account on the relay as well as forgetting its keys here. switch account signs out, then asks GitHub which of its accounts (Google always asks)
- add a device, on a browser that holds your account key: a QR code and a link. open it on a phone or another browser, sign in, and this browser hands it the key, sealed under a secret only the code carries, the way `:remote pair` does on a computer. one use, while the sheet stays open
- a new account by GitHub, Google or email that stops at "invite needed" finishes with the invite: type it and continue, no second trip to GitHub or Google, no new code
- a browser that holds the account key lists itself on the relay with a name sealed under it ("browser:iPhone · Safari"), like a computer does. when no device and no passkey holds the key, nothing is sealed under it, so a browser signing in makes a new one instead of waiting for a pairing that cannot come
- relay: `POST /v1/invite` (finishes a sign-in held for ten minutes after "invite needed", five tries), `GET /v1/account` (the account id and how many passkeys hold its key), `-l` sets the sign-ins per IP per minute (30)

### changed
- the sign-in sheet: one wide button per way in, Google and GitHub with their own marks, a key for a passkey, an envelope for email; what an account keeps sits under the buttons instead of above them
- fixed: coming back from GitHub or Google could fail with a 401. the page signed in twice at once, the relay keeps one token per device, and the page kept the one it replaced. sign-ins at once now share one
- fixed: the "this runs in your browser" notice opened over the sign-in sheet on an invite link, a `#signin` link or a return from GitHub or Google. it waits for the next visit there
- fixed: coming back from GitHub or Google after an invite or `#signin` link opened two sign-in sheets, one over the other
- a browser without the account key says your code sessions are sealed with it and how to get it, instead of a red warning and "none open"; a passkey that is missing or cancelled says so instead of the browser's own error text
- the sign-in sheet links the privacy page (mithraeum.studio/privacy), under what an account keeps
- a link page (`:remote on`, now also a QR code in the terminal) opened on a browser without the account key says how to get it (add a device on one that has it, or a passkey), not just "sign in with your passkey"
- relay: `make fuzz` sends mutated requests through the relay's request handler under ASan and UBSan, signed in with a passkey challenge and an email code open so they reach past the first check, plus the store loader and the passkey parsers. 200,000 inputs clean; two planted overflows are caught
- `.gitignore` keeps a relay data dir (`relay-data/`: the hashing key, the store, sign-in secrets) out of the repo
- relay: the email, GitHub and Google sign-ins share one account step, and the relay's sockets and pipes stay out of the processes it starts

### fixed
- the browser tab and home-screen icon could stay the old art: browsers cache an icon by its address, and the new one kept the old address. it has a new one now

## [v0.0.7] - 2026-10-25

### changed
- signed in, chat lists your code sessions above this browser's chats; code lists code sessions only (inside a session, that folder's chats too)
- each row says what it is: ▣ a code session, ○ a chat in this browser

### fixed
- with no provider picked, a prompt in the browser asked to "paste your none API key". it says to pick a provider first now
- Nous Portal is in the key list, with a link to get one
- `tools/remote-test.py` checks that a dead upload reconnects, and only looks at its own hako's curls for an inherited socket

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
