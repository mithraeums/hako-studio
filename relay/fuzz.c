/* SPDX-License-Identifier: GPL-3.0-or-later — Copyright (C) 2026 Zachary Blauser */
/* fuzz.c — mutation fuzzer for the relay: whole HTTP requests through the
   connection handler (headers, chunked bodies, every route), and the store
   file through its loader. Everything a request carries comes from strangers.

   One device is signed in to an account with a passkey challenge and an email
   code already open, so mutations of a well-formed sign-in get past the first
   check instead of stopping at "start again". No network: email is off and
   GitHub/Google point at a refused http address.

   A small seeded mutator under ASan + UBSan, like hako's tools/fuzz.c:
   `make fuzz` (FUZZ_ITERS=N, FUZZ_SEED=N to replay). A crash leaves the input
   in /tmp/hako-relay-fuzz-crash.txt. */
#include "relay.c"
#undef main

#define T0 "1111111111111111111111111111111111111111111111111111111111111111"
#define T1 "2222222222222222222222222222222222222222222222222222222222222222"
#define CHAL "QUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUE"
#define NSEEDS 40

static char seeds[NSEEDS][8192];
static char good_cdj64[600], good_ad64[300];             /* a clientDataJSON and authenticatorData that pass */
static int nseeds;
static unsigned long long rng;

static unsigned rnd(void) {
	rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
	return (unsigned)(rng >> 16);
}

static const char *DICT[] = {
	"\r\n", "\r\n\r\n", " ", "-", "Authorization: Bearer " T0 "\r\n", "Transfer-Encoding: chunked\r\n",
	"Content-Length: ", "X-Forwarded-For: ", ",", "0\r\n\r\n", "ffffffff\r\n", ";x=y", "POST /v1/", "GET /v1/",
	"github/", "google/", "start ", "verify ", "passkey/", "email/", "\"", "\\", "\"type\"", "\"challenge\"",
	"\"origin\"", "\"crossOrigin\":true", "\"type\":\"", "\"challenge\":\"", "\"access_token\": \"", "\"sub\":\"", "\"id\":", ":", "{", "}", "=", "%", "/", "\x00", "\xff", "AAAA", "____",
};
#define NDICT (int)(sizeof(DICT) / sizeof(DICT[0]))

static void seed_req(const char *method, const char *path, const char *tok, const char *body, int chunked) {
	if (nseeds >= NSEEDS) return;
	char *s = seeds[nseeds++];
	size_t bl = strlen(body);
	int n = snprintf(s, sizeof seeds[0], "%s %s HTTP/1.1\r\nHost: relay\r\nX-Forwarded-For: 10.0.0.%d\r\n%s%s%s",
	                 method, path, nseeds, tok ? "Authorization: Bearer " : "", tok ? tok : "", tok ? "\r\n" : "");
	if (chunked) snprintf(s + n, sizeof seeds[0] - (size_t)n, "Transfer-Encoding: chunked\r\n\r\n%zx\r\n%s\r\n0\r\n\r\n", bl, body);
	else snprintf(s + n, sizeof seeds[0] - (size_t)n, "Content-Length: %zu\r\n\r\n%s", bl, body);
}

/* The passkey seeds: a clientDataJSON with the armed challenge and our origin,
   authenticatorData with our rp id hash, and a real P-256 public key. */
static void seed_passkeys(void) {
	char cdj[400], cdj64[600], ad64[300], cred64[40], spki64[200], body[2000];
	unsigned char ad[160], cred[16], der[200];
	memset(cred, 7, sizeof cred);
	b64url_enc(cred64, cred, sizeof cred);
	memset(ad, 0, sizeof ad);
	sha256(ad, rp_id, strlen(rp_id), NULL, 0);
	ad[32] = 0x45;                                               /* UP UV AT */
	ad[53] = 0; ad[54] = sizeof cred;
	memcpy(ad + 55, cred, sizeof cred);
	memcpy(ad + 71, "\xa5\x01\x02\x03\x26", 5);
	b64url_enc(ad64, ad, 76);
	EVP_PKEY *k = EVP_EC_gen("P-256");
	unsigned char *p = der;
	int dn = k ? i2d_PUBKEY(k, &p) : 0;
	EVP_PKEY_free(k);
	b64url_enc(spki64, der, dn > 0 ? (size_t)dn : 0);
	snprintf(cdj, sizeof cdj, "{\"type\":\"webauthn.create\",\"challenge\":\"%s\",\"origin\":\"%s\",\"crossOrigin\":false}", CHAL, origin);
	b64url_enc(cdj64, (unsigned char *)cdj, strlen(cdj));
	snprintf(body, sizeof body, "- %s %s %s %s\n", cred64, cdj64, ad64, spki64);
	seed_req("POST", "/v1/passkey/register", T0, body, 0);
	snprintf(cdj, sizeof cdj, "{\"type\":\"webauthn.get\",\"challenge\":\"%s\",\"origin\":\"%s\"}", CHAL, origin);
	b64url_enc(cdj64, (unsigned char *)cdj, strlen(cdj));
	b64url_enc(ad64, ad, 37);
	memcpy(good_cdj64, cdj64, sizeof good_cdj64);
	memcpy(good_ad64, ad64, sizeof good_ad64);
	if (nseeds < NSEEDS) snprintf(seeds[nseeds++], sizeof seeds[0], "%s", cdj);     /* raw, for run_webauthn */
	snprintf(body, sizeof body, "%s %s %s MEUCIQDx\n", cred64, cdj64, ad64);
	seed_req("POST", "/v1/passkey/login", T0, body, 1);
	snprintf(body, sizeof body, "%s %s\n", cred64, "d3JhcHBlZC1rZXktYnl0ZXMtZ29lcy1oZXJlLTQ4Ynl0ZXMtbG9uZw");
	seed_req("POST", "/v1/passkey/key", T0, body, 0);
}

static void seed_all(void) {
	seed_req("GET", "/v1/health", NULL, "", 0);
	seed_req("OPTIONS", "/v1/up", NULL, "", 0);
	seed_req("POST", "/v1/auth/challenge", NULL, "8f40c5adb68f25624ae5b214ea767a6ec94d829d3d7b5e1ad1ba6f3e2138285f\n", 0);
	seed_req("POST", "/v1/auth/prove", NULL, "0123456789abcdef0123456789abcdef 00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff\n", 0);
	seed_req("GET", "/v1/devices", T0, "", 0);
	seed_req("POST", "/v1/up", T0, "0123456789abcdef0123456789abcdef aGVsbG8\n0123456789abcdef0123456789abcdef d29ybGQ\n", 1);
	seed_req("POST", "/v1/webauthn/challenge", T0, "", 0);
	seed_req("POST", "/v1/email/start", T1, "a@b.co\n", 0);
	seed_req("POST", "/v1/email/verify", T0, "123456 -\n", 0);
	seed_req("POST", "/v1/email/verify", T1, "123456 abcde-fghij\n", 1);
	seed_req("POST", "/v1/github/start", T0, "", 0);
	seed_req("POST", "/v1/google/start", T1, "", 0);
	seed_req("POST", "/v1/github/verify", T0, "good 0123456789abcdef0123456789abcdef -\n", 0);
	seed_req("POST", "/v1/google/verify", T1, "4/0AeanS0b 0123456789abcdef0123456789abcdef abcde-fghij\n", 1);
	seed_req("POST", "/v1/bind", T0, "8f40c5adb68f25624ae5b214ea767a6ec94d829d3d7b5e1ad1ba6f3e2138285f\n", 0);
	seed_req("POST", "/v1/name", T0, "c2VhbGVkLW5hbWU\n", 0);
	seed_req("POST", "/v1/revoke", T0, "0123456789abcdef0123456789abcdef\n", 0);
	seed_req("POST", "/v1/nothing", T1, "x\n", 0);
	seed_passkeys();
	if (nseeds < NSEEDS) snprintf(seeds[nseeds++], sizeof seeds[0], "{\n  \"access_token\": \"ya29.a0Af\",\n  \"scope\": \"openid\"\n}");
	if (nseeds < NSEEDS) snprintf(seeds[nseeds++], sizeof seeds[0], "{\"login\":\"octo\",\"id\":4242,\"sub\":\"110169484474386276334\"}");
	if (nseeds < NSEEDS) snprintf(seeds[nseeds++], sizeof seeds[0], "a 0123456789abcdef\nc 0123456789abcdef Y3JlZA spki -\n"
	                              "l 0123456789abcdef %s\nb 0123456789abcdef %s c2VhbGVk\nx %s\n", T0, T0, T1);
}

static size_t mutate(char *buf, size_t len, size_t cap) {
	int rounds = 1 + rnd() % 8;
	while (rounds--) {
		size_t at = len ? rnd() % (len + 1) : 0;
		switch (rnd() % 8) {
		case 0: if (len) buf[rnd() % len] ^= (char)(1 << (rnd() % 8)); break;
		case 1: if (len) buf[rnd() % len] = (char)(rnd() & 0xff); break;
		case 2: if (len > 1) { size_t n = 1 + rnd() % (len - at + 1 > 16 ? 16 : len - at + 1);
				if (at + n > len) n = len - at; memmove(buf + at, buf + at + n, len - at - n); len -= n; } break;
		case 3: { const char *d = DICT[rnd() % NDICT]; size_t n = strlen(d);
				if (len + n < cap) { memmove(buf + at + n, buf + at, len - at); memcpy(buf + at, d, n); len += n; } } break;
		case 4: len = at; break;
		case 5: { const char *s = seeds[rnd() % nseeds]; size_t sl = strlen(s), from = rnd() % (sl + 1), n = sl - from;
				if (len + n < cap) { memmove(buf + at + n, buf + at, len - at); memcpy(buf + at, s + from, n); len += n; } } break;
		case 6: if (len && len * 2 < cap) { memcpy(buf + len, buf, len); len *= 2; } break;
		case 7: { size_t n = 1 + rnd() % 600; char c = (char)(rnd() % 3 ? 'A' + rnd() % 26 : rnd() & 0xff);
				if (len + n < cap) { memmove(buf + at + n, buf + at, len - at); memset(buf + at, c, n); len += n; } } break;
		}
	}
	buf[len] = '\0';
	return len;
}

/* The store as setup left it, put back before every input. */
static Acct s_accts[MAX_ACCT];
static Cred s_creds[MAX_CRED];
static Login s_logins[MAX_LOGIN];
static Bind s_binds[MAX_BIND];
static char s_spent[MAX_SPENT][65];
static char acct0[17], mail_hash0[65], mail_code0[65];

static void snapshot(int restore) {
#define SNAP(a, b) (restore ? memcpy(a, b, sizeof a) : memcpy(b, a, sizeof a))
	SNAP(accts, s_accts); SNAP(creds, s_creds); SNAP(logins, s_logins); SNAP(binds, s_binds); SNAP(spent, s_spent);
#undef SNAP
}

static void dev_reset(Dev *d, int i, const char *tok, const char *acct) {
	if (!d->used) pthread_mutex_init(&d->w, NULL);
	d->used = 1;
	d->down = -1;
	memset(d->pub, i + 1, 32);
	device_id(d->id, d->pub);
	memcpy(d->token, tok, 65);
	d->tok_exp = time(NULL) + 3600;
	snprintf(d->acct, sizeof d->acct, "%s", acct);
	snprintf(d->wa_chal, sizeof d->wa_chal, "%s", CHAL);
	d->wa_exp = time(NULL) + 600;
	memcpy(d->mail_hash, mail_hash0, 65);
	memcpy(d->mail_code, mail_code0, 65);
	d->mail_exp = time(NULL) + 600;
	d->mail_tries = 0;
	d->sso_state[0] = '\0';
	d->bucket = RATE_BURST;
	gettimeofday(&d->bucket_t, NULL);
}

/* One request through conn_thread over a socketpair; the reply's status code
   (0 for none). */
static int run_request(const char *buf, size_t len) {
	int s[2], big = 1 << 20;
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, s)) return 0;
	for (int i = 0; i < 2; i++) {
		setsockopt(s[i], SOL_SOCKET, SO_SNDBUF, &big, sizeof big);
		setsockopt(s[i], SOL_SOCKET, SO_RCVBUF, &big, sizeof big);
	}
	for (size_t w = 0; w < len; ) {
		ssize_t k = write(s[1], buf + w, len - w);
		if (k <= 0) break;
		w += (size_t)k;
	}
	shutdown(s[1], SHUT_WR);
	conn_thread((void *)(intptr_t)s[0]);                        /* closes s[0] */
	char head[16] = "", sink[4096];
	ssize_t got = read(s[1], head, sizeof head - 1);
	while (read(s[1], sink, sizeof sink) > 0) {}
	close(s[1]);
	return got > 12 && !strncmp(head, "HTTP/1.1 ", 9) ? atoi(head + 9) : 0;
}

static void reset(void) {
	snapshot(1);
	memset(ips, 0, sizeof ips);
	memset(chals, 0, sizeof chals);
	dev_reset(&devs[0], 0, T0, acct0);
	dev_reset(&devs[1], 1, T1, "");
}

/* The seeds must reach deep, or the run only fuzzes the front door. */
static int seeds_reach(void) {
	static const struct { const char *path; int status; } want[] = {
		{ "/v1/health", 200 }, { "/v1/devices", 200 }, { "/v1/up", 204 }, { "/v1/email/verify", 200 },
		{ "/v1/passkey/register", 200 }, { "/v1/github/start", 200 }, { "/v1/google/start", 200 },
		{ "/v1/bind", 200 }, { "/v1/name", 200 },
	};
	int bad = 0;
	for (size_t w = 0; w < sizeof want / sizeof want[0]; w++)
		for (int i = 0; i < nseeds; i++) {
			const char *sp = strchr(seeds[i], ' ');
			if (!sp || strncmp(sp + 1, want[w].path, strlen(want[w].path)) || sp[1 + strlen(want[w].path)] != ' ') continue;
			reset();
			int st = run_request(seeds[i], strlen(seeds[i]));
			if (st != want[w].status) { fprintf(stderr, "relay fuzz: seed %s answered %d, not %d\n", want[w].path, st, want[w].status); bad++; }
			break;
		}
	return bad;
}

static void run_store(const char *buf, size_t len) {
	char path[600];
	snprintf(path, sizeof path, "%s/store", data_dir);
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fwrite(buf, 1, len, f);
	fclose(f);
	memset(accts, 0, sizeof accts); memset(creds, 0, sizeof creds); memset(logins, 0, sizeof logins);
	memset(binds, 0, sizeof binds); memset(spent, 0, sizeof spent);
	store_load();
	unlink(path);
}

/* The input as a browser's clientDataJSON, then as authenticatorData, each
   beside a part that passes: wa_check against an armed challenge. */
static void run_webauthn(const char *buf, size_t len) {
	static char enc[2100];
	unsigned char cdj[2048], ad[2048];
	long cn, an;
	if (len > 1500) len = 1500;
	b64url_enc(enc, (const unsigned char *)buf, len);
	dev_reset(&devs[1], 1, T1, "");
	(void)wa_check(&devs[1], "webauthn.get", enc, good_ad64, cdj, &cn, ad, &an);
	dev_reset(&devs[1], 1, T1, "");
	(void)wa_check(&devs[1], "webauthn.get", good_cdj64, enc, cdj, &cn, ad, &an);
	(void)spki_key(enc);
}

/* The small parsers, straight. */
static void run_helpers(const char *buf, size_t len) {
	char out[2048], line[2048], *f[6];
	unsigned char bin[2048];
	(void)json_str(buf, "type", out, 32);
	(void)json_str(buf, "access_token", out, 64);
	(void)json_str(buf, "sub", out, 48);
	(void)b64url_dec(bin, sizeof bin, buf, len < 2000 ? len : 2000);
	(void)is_b64url(buf, len);
	form_enc(out, 64, buf);
	(void)email_norm(out, 300, buf);
	invite_norm(out, 32, buf);
	(void)sso_at(buf, "start");
	(void)header(buf, "Authorization");
	snprintf(line, sizeof line, "%s", buf);
	(void)fields(line, f, 6);
}

static void on_hang(int sig) {
	(void)sig;
	static const char m[] = "relay fuzz: HANG (>5s on one input), input in /tmp/hako-relay-fuzz-crash.txt\n";
	if (write(2, m, sizeof m - 1) < 0) {}
	_exit(1);
}

int main(void) {
	long iters = getenv("FUZZ_ITERS") ? atol(getenv("FUZZ_ITERS")) : 100000;
	rng = getenv("FUZZ_SEED") ? strtoull(getenv("FUZZ_SEED"), NULL, 10) : (unsigned long long)time(NULL);
	unsigned long long seed = rng;
	if (!rng) rng = 88172645463325252ULL;
	char tmpl[] = "/tmp/hako-relay-fuzz.XXXXXX";
	if (!mkdtemp(tmpl)) { perror("mkdtemp"); return 1; }
	snprintf(data_dir, sizeof data_dir, "%s", tmpl);
	if (load_secret()) { fprintf(stderr, "relay fuzz: no secret\n"); return 1; }
	signal(SIGPIPE, SIG_IGN);
	signal(SIGALRM, on_hang);
	for (int i = 0; i < NSSO; i++) {                               /* on, but unreachable: curl refuses http */
		snprintf(ssos[i].id, sizeof ssos[i].id, "fuzz.%s", ssos[i].name);
		snprintf(ssos[i].secret, sizeof ssos[i].secret, "sekret");
		snprintf(ssos[i].token, sizeof ssos[i].token, "http://127.0.0.1:9/token");
		snprintf(ssos[i].user, sizeof ssos[i].user, "http://127.0.0.1:9/user");
	}
	Acct *a = acct_new();
	if (!a) return 1;
	memcpy(acct0, a->id, 17);
	{
		unsigned char cd[32];
		keyed_hash(mail_hash0, "email", "a@b.co");
		sha256(cd, "123456", 6, mail_hash0, 64);
		hex_enc(mail_code0, cd, 32);
	}
	dev_reset(&devs[0], 0, T0, acct0);
	dev_reset(&devs[1], 1, T1, "");
	dev_bind_locked(&devs[0], acct0);
	snapshot(0);
	seed_all();
	if (seeds_reach()) return 1;

	static char buf[1 << 16];
	size_t len = 0;
	for (long i = 0; i < iters; i++) {
		if (i % 16 == 0 || !len) { const char *s = seeds[rnd() % nseeds]; len = strlen(s); memcpy(buf, s, len + 1); }
		len = mutate(buf, len, sizeof buf - 1);
		FILE *f = fopen("/tmp/hako-relay-fuzz-crash.txt", "wb");
		if (f) { fwrite(buf, 1, len, f); fclose(f); }
		alarm(5);
		reset();
		(void)run_request(buf, len);
		run_helpers(buf, len);
		run_webauthn(buf, len);
		if (i % 8 == 0) run_store(buf, len);
		alarm(0);
	}
	remove("/tmp/hako-relay-fuzz-crash.txt");
	char cmd[700];
	snprintf(cmd, sizeof cmd, "rm -rf %s", data_dir);
	if (system(cmd)) {}
	printf("relay fuzz: %ld inputs, seed %llu, no sanitizer reports\n", iters, seed);
	return 0;
}
