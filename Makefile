# hako studio — the agent in a browser.
#
# Self-contained. The only thing studio needs from hako-code is `hako.c` itself,
# taken as a build input. What it ships — index.html + hako.wasm — runs with no
# agent installed, no server of ours and no account: host it anywhere static
# files live, or run the container.
#
#   make            build the wasm, assemble dist/
#   make serve      dist/ on http://localhost:8080
#   make docker     image serving dist/
#   make check      bundle sanity: one page, nothing external, handlers live
#   make embed      index.html -> ../hako-code/hako_web.h (for `hako --serve`)
#   make dev        develop the UI against a local agent, no rebuild per edit
#
# Toolchain is clang. `make toolchain` fetches wasi-sdk (~175 MB into
# ~/.hako/toolchains, removable with rm -rf). No emscripten, no npm, no framework.

HAKO_SRC   ?= hako.c
HAKO_REPO  ?= https://raw.githubusercontent.com/mithraeums/hako-code/main/hako.c
HAKO_LOCAL ?= ../hako-code/hako.c
AGENT_DIR  ?= ../hako-code
HAKO       ?= $(AGENT_DIR)/hako
DIST       ?= dist
PORT       ?= 8080
DEV_PORT   ?= 8787
DIR        ?= .

WASI_VER   ?= 33.0
WASI_ARCH  ?= $(shell uname -m)
WASI_DIR   ?= $(HOME)/.hako/toolchains/wasi-sdk-$(WASI_VER)-$(WASI_ARCH)-macos
WASI_URL   ?= https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-33/wasi-sdk-$(WASI_VER)-$(WASI_ARCH)-macos.tar.gz
CC_WASM    ?= $(WASI_DIR)/bin/clang
SYSROOT    ?= $(WASI_DIR)/share/wasi-sysroot

# -Os + --strip-all: 489 KB -> 201 KB (gzip 152 -> 86) for every phone that opens the page.
# The agent waits on the network in a tab, so -Os costs nothing anyone can see; the
# stripped names section is only debug symbols (exports are kept by --export).
CFLAGS_WASM = --target=wasm32-wasip1 --sysroot=$(SYSROOT) -Os -Wl,--strip-all \
              -D_WASI_EMULATED_GETPID -lwasi-emulated-getpid \
              -Wl,--export=main -Wl,--export=malloc -Wl,--export=free \
              -Wl,--allow-undefined \
              -Wl,-z,stack-size=4194304 -Wl,--stack-first

.PHONY: all fetch toolchain wasm site serve serve-tls docker check embed build dev clean distclean ui-test remote-test test

all: site

# ---------- the agent source ----------
# A checkout next door wins, so local edits build; otherwise fetch the released
# source. Studio never contains the agent — it builds against it.
# A checkout next door wins, then a copy already staged here (how the container
# gets an unreleased agent), then the released source. A published build has
# neither of the first two and fetches, which is the intent.
fetch:
	@if [ -f "$(HAKO_LOCAL)" ]; then \
		cp "$(HAKO_LOCAL)" $(HAKO_SRC); echo "using local $(HAKO_LOCAL)"; \
	elif [ -f "$(HAKO_SRC)" ]; then \
		echo "using staged $(HAKO_SRC)"; \
	else \
		echo "fetching hako.c…"; \
		curl -fsSL "$(HAKO_REPO)" -o $(HAKO_SRC) || { echo "fetch failed"; exit 1; }; \
	fi
	@printf "hako.c: %s lines\n" "$$(wc -l < $(HAKO_SRC) | tr -d ' ')"

toolchain:
	@if [ -x "$(CC_WASM)" ]; then echo "wasi-sdk present: $(WASI_DIR)"; else \
		tmp=$$(mktemp -d); \
		echo "downloading wasi-sdk $(WASI_VER) (~175 MB)…"; \
		curl -fsSL "$(WASI_URL)" -o "$$tmp/wasi-sdk.tar.gz" && \
		tar xzf "$$tmp/wasi-sdk.tar.gz" -C "$$tmp" && \
		mkdir -p "$$(dirname $(WASI_DIR))" && \
		mv "$$tmp"/wasi-sdk-*/ "$(WASI_DIR)" && \
		rm -rf "$$tmp" && \
		echo "installed $(WASI_DIR)"; \
	fi

wasm: toolchain
	@[ -x "$(CC_WASM)" ] || { echo "no wasm clang — run: make toolchain"; exit 1; }
	@$(MAKE) --no-print-directory fetch
	$(CC_WASM) $(CFLAGS_WASM) $(HAKO_SRC) -o hako.wasm
	@rm -f $(HAKO_SRC)          # a build input, not a resident: studio holds no agent source
	@printf "hako.wasm: %s KB\n" "$$(( $$(wc -c < hako.wasm) / 1024 ))"

# ---------- what gets shipped ----------
# One page, one wasm, one glue file. Nothing else, and nothing phoned home.
# The worker and the wasm are one unit with the page. A browser holding a stale
# copy of either produces errors that look like logic bugs, so every build gets
# a stamp the URLs carry.
site: wasm
	@mkdir -p $(DIST)
	@stamp=$$(date +%s); \
	sed -e "s|<title>hako</title>|<meta name=\"hako-mode\" content=\"wasm\"><meta name=\"hako-build\" content=\"$$stamp\"><title>hako</title>|" \
	    -e "s|new Worker('hako-worker.js'|new Worker('hako-worker.js?v=$$stamp'|" \
	    -e "s|wasmUrl: 'hako.wasm'|wasmUrl: 'hako.wasm?v=$$stamp'|" \
	    $(if $(RELAY_URL),-e "s|<title>hako</title>|<meta name=\"hako-relay\" content=\"$(RELAY_URL)\"><title>hako</title>|") \
	    index.html > $(DIST)/index.html; \
	echo "build stamp $$stamp"
	@cp hako.wasm tools/hako-glue.js tools/hako-worker.js $(DIST)/
	@cp icon-180.png $(DIST)/ 2>/dev/null || true
	@cp og.png $(DIST)/ 2>/dev/null || true      # the share-preview tile
	@printf '%s\n' '{"name":"hako studio","short_name":"hako","start_url":"/","display":"standalone","background_color":"#ece7da","theme_color":"#ece7da","icons":[{"src":"/icon-180.png","sizes":"180x180","type":"image/png","purpose":"any maskable"}]}' > $(DIST)/manifest.webmanifest
	@echo "dist/ ready — static files, no backend:"
	@ls -la $(DIST) | tail -n +2 | awk 'NF>3 {printf "  %-24s %8s\n", $$9, $$5}'

# No caching: a stale worker or wasm under a fresh page reads as a logic bug.
serve: site
	@echo "http://localhost:$(PORT)   (ctrl-c to stop)"
	@python3 tools/serve-static.py $(DIST) $(PORT)

# HTTPS, because the browser build needs a secure context: OPFS and workers are
# unavailable over plain HTTP to anything but localhost. Self-signed, for testing
# from a phone or tablet on the same network.
TLS_PORT ?= 8443
serve-tls: site
	@python3 tools/serve-tls.py $(DIST) $(TLS_PORT)

# A checkout next door is not in the build context, so stage it: without this
# the image quietly builds the last released agent instead of this one.
docker:
	@if [ -f "$(HAKO_LOCAL)" ]; then cp "$(HAKO_LOCAL)" $(HAKO_SRC); echo "staging local $(HAKO_LOCAL)"; fi
	docker build -t hako-studio .
	@rm -f $(HAKO_SRC)
	@echo "run it:  docker run --rm -p 8080:80 hako-studio"

# ---------- for people running the native agent ----------
embed:
	@sh tools/embed.sh index.html $(AGENT_DIR)/hako_web.h

build: embed
	$(MAKE) -C $(AGENT_DIR)

# --web serves index.html off disk: edit, reload, no rebuild.
dev:
	$(HAKO) --serve --port $(DEV_PORT) --web . --dir $(DIR)

# ---------- checks ----------
# The bundle must stay self-contained: a CDN reference or a hosted font puts a
# third party inside the trust boundary and breaks the offline case outright.
check:
	@fail=0; \
	if grep -nE 'src="https?:|href="https?:|@import|fonts\.googleapis|cdn\.' index.html; then \
		echo "FAIL: external resource referenced above"; fail=1; fi; \
	n=$$(ls *.html 2>/dev/null | wc -l | tr -d ' '); \
	if [ "$$n" != "1" ]; then echo "FAIL: expected one HTML file, found $$n"; fail=1; fi; \
	echo "index.html: $$(wc -c < index.html | tr -d ' ') bytes"; \
	if command -v node >/dev/null 2>&1; then \
		node tools/smoke.js index.html || fail=1; \
		node tools/glue-test.js || fail=1; \
		node tools/history-test.js >/dev/null 2>&1 || echo "(history test skipped — no stored sessions)"; \
		if [ -f hako.wasm ] || [ -f $(DIST)/hako.wasm ]; then node tools/session-test.js || fail=1; \
		else echo "(session test skipped — no wasm built)"; fi; \
	else echo "(smoke skipped — no node)"; fi; \
	if [ -f "$(AGENT_DIR)/hako_web.h" ]; then \
		want=$$(shasum -a 256 index.html 2>/dev/null | cut -c1-16); \
		have=$$(sed -n 's/.*HAKO_WEB_SRC_HASH "\(.*\)".*/\1/p' "$(AGENT_DIR)/hako_web.h"); \
		if [ -n "$$want" ] && [ "$$want" != "$$have" ]; then \
			echo "FAIL: $(AGENT_DIR)/hako_web.h is stale (built from $$have, index.html is $$want) — run: make embed"; \
			fail=1; \
		else echo "embed: hako_web.h matches index.html"; fi; \
	fi; \
	[ $$fail -eq 0 ] && echo "check: ok"

clean:
	rm -rf $(DIST) hako.wasm

distclean: clean
	rm -f $(HAKO_SRC)

# Every check this repo has. ui-test needs playwright and a built ../hako-code/hako.
test: check
	@if [ -f ../hako-edit/tools/themes.py ]; then python3 ../hako-edit/tools/themes.py --check; else echo "themes: no ../hako-edit checkout — skipped"; fi
	@if python3 -c 'import playwright' 2>/dev/null && [ -x ../hako-code/hako ]; then \
		$(MAKE) --no-print-directory ui-test; \
	else echo "ui-test: skipped (needs playwright and a built ../hako-code/hako)"; fi

# Browser test against a built ../hako-code/hako and its fake engine (needs playwright)
ui-test:
	python3 tools/ui-test.py

# The relay end to end: a virtual passkey links a real `hako` (:remote on in a
# pty) through a real relay, then a fresh browser signs in and drives a turn.
remote-test:
	@$(MAKE) --no-print-directory -C relay relay
	@$(MAKE) --no-print-directory site RELAY_URL=http://127.0.0.1:18793 DIST=dist-remote
	python3 tools/remote-test.py
