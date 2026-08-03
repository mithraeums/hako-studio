<p align="center">
  <a href="https://mithraeums.github.io">
    <b>hako studio</b>
  </a>
</p>

<p align="center">
  <em>The hako agent in a browser tab. One binary, your files, your models, any device on your network.</em>
</p>

<p align="center">
  <img src="https://img.shields.io/badge/version-v0.0.1-b89656?style=flat-square&labelColor=14130f" alt="v0.0.1"/>
  <img src="https://img.shields.io/badge/license-GPL--3.0-c8c2b2?style=flat-square&labelColor=14130f" alt="GPL-3.0"/>
  <img src="https://img.shields.io/badge/C99-single%20file-c8c2b2?style=flat-square&labelColor=14130f" alt="C99 single file"/>
  <img src="https://img.shields.io/badge/deps-none-c8c2b2?style=flat-square&labelColor=14130f" alt="no dependencies"/>
</p>

<p align="center">
  <sub><a href="https://mithraeums.github.io">site</a> &nbsp;·&nbsp; <a href="https://github.com/mithraeums/hako-code">hako-code</a> &nbsp;·&nbsp; <a href="https://github.com/mithraeums/hako">hako (models)</a> &nbsp;·&nbsp; <a href="https://github.com/mithraeums/hako-edit">hako-edit</a> &nbsp;·&nbsp; <a href="https://github.com/mithraeums">org</a></sub>
</p>

<br>

<!-- Screenshots live in the website repo (assets/readme-screenshots/hako-studio/) like the other repos. -->

<table>
  <tr>
    <td width="33%" valign="top"><b>No backend.</b><br/><sub>A C program serves a page and pipes it to <code>hako</code>. No database, no account, no telemetry. Your files never leave the machine you ran it on.</sub></td>
    <td width="33%" valign="top"><b>One agent, two faces.</b><br/><sub>The browser and the CLI share sessions, credentials, skills and trust. Start a conversation in the terminal, finish it in a tab.</sub></td>
    <td width="33%" valign="top"><b>Every device you own.</b><br/><sub>Loopback by default; bind to the LAN and drive your desktop from a phone or tablet — real shell, real files, local models.</sub></td>
  </tr>
</table>

<p align="center"><sub><b>—— I ——</b></sub></p>

## Overview

- **A bridge, not a second agent.** `hako-serve` spawns `hako --pipe` and forwards its JSONL
  protocol to the browser over HTTP + Server-Sent Events. No agent logic lives here: the loop,
  the providers, the tools and the session format are all `hako` itself. One brain, one place
  to fix it.
- **Shared state with the CLI.** Sessions, `~/.hako/credentials`, skills, `~/.hakorc` and
  per-project trust are the same files the terminal uses. `:resume` in either surface picks up
  the other's conversation.
- **Tool calls ask first.** Every `write_file`, `edit_file`, `run_shell` and read pauses for
  **allow once · deny · always**, mirroring the CLI's `y`/`n`/`a` scoping — project-wide for
  reads and writes, exact-command for shell.
- **Any folder, per-folder sessions.** Browse the filesystem, open a project, and the sidebar
  lists that project's conversations. Resume renders the full stored transcript; delete removes
  the jsonl.
- **Local models first.** Installed weights, loose `.mlf2` files found in `~/Downloads` or any
  folder you add (symlinked into place — never copied, never moved), and `:pull` for the rest.
  Cloud catalogs come from the provider.
- **Four themes and a thinking indicator.** Five spinner styles with elapsed time, because a
  local 3B turn runs for minutes with nothing streamed back. Both persist on your device.
- **Nothing to install.** libc and pthread. No framework, no CDN, no build step for the page.

<p align="center"><sub><b>—— II ——</b></sub></p>

## Build & Run

```sh
make                                   # cc -O2 -Wall -Wextra, no libraries
./hako-serve --dir ~/code/my-project
# open http://127.0.0.1:8787
```

```
--port N       listen port (default 8787)
--bind ADDR    bind address (default 127.0.0.1)
--dir PATH     working directory for the agent
--web PATH     directory holding index.html (default .)
--hako PATH    hako binary (default: hako on PATH)
--yolo         auto-approve every tool call
```

> Requires [`hako`](https://github.com/mithraeums/hako-code) on PATH, built with the `--pipe`
> tool-approval round-trip. Without it the agent denies tool calls when stdin is not a tty, and
> only `--yolo` runs tools.

```sh
make asan       # ASan + UBSan build
make run        # build and serve the current directory
```

<p align="center"><sub><b>—— III ——</b></sub></p>

## Interface

| Area | What it does |
|---|---|
| **sidebar** | working directory · `open folder…` · settings · sessions in this folder · new session · restart |
| **session row** | click to resume with full transcript; `✎` renames it; `×` deletes the stored jsonl |
| **approval box** | `allow once` · `deny` · `always` — answered boxes stay settled across a reload |
| **header** | live provider/model and turn count, read from the agent's own output |
| **settings** | theme · thinking indicator · extra folders to search for model weights · forget local settings |

Type `:` commands exactly as in the CLI — `:trust`, `:usage`, `:sessions`, `:pull <model>`,
`:help`. Bare `:model`, `:provider` and `:theme` open the browser's own pickers; the CLI's
built-in pickers read stdin, which the bridge needs.

Enter sends, Shift+Enter adds a line. Under 720px the sidebar becomes a drawer.

<p align="center"><sub><b>—— IV ——</b></sub></p>

## Where things are kept

```
~/.hako/projects/<encoded-cwd>/sessions/   conversations (shared with the CLI)
  <id>.jsonl                               the transcript, written by hako
  <id>.title                               optional name you gave it (sidecar)
~/.hako/credentials                        provider keys and OAuth tokens
~/.hako/models/<id>/<id>.mlf2              local weights
browser localStorage                       theme, spinner, model search paths
```

The server holds nothing. It keeps a 4096-event replay buffer in memory so a reload restores
the current view, and that is all — no logging, no history of its own, nothing written to disk
that `hako` did not write.

What you send to a **cloud** model goes to that provider under your own key. Local models go
nowhere.

<p align="center"><sub><b>—— V ——</b></sub></p>

## Security

- **Loopback by default.** `--bind` to anything else exposes an unauthenticated agent: anyone
  who can reach the port can run tools as you. There is no pairing token — use it on a network
  you trust, or a VPN.
- **`--yolo` approves everything**, including shell commands and file writes, with no prompt.
  The approval boxes exist so you don't need it.
- Cross-origin POSTs are rejected. Session ids are validated against path traversal.
- `hako`'s own trust gate still applies: an untrusted directory refuses every tool until
  `:trust`.

<p align="center"><sub><b>—— VI ——</b></sub></p>

## Contributing

If you share the belief that simplicity empowers creativity, feel free to contribute.

- Forking this repo
- Submitting a Pull Request
- Bug reports and feature requests

Please ensure your code follows the existing style: C99, tabs, `snake_case` fields, no
third-party libraries, and no agent logic in the bridge.

<p align="center"><sub><a href="LICENSE">— SEE LICENSE —</a> &nbsp;·&nbsp; GPL-3.0</sub></p>

<p align="center"><sub><em>— deus sol invictus mithras —</em></sub></p>
