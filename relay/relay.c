/* SPDX-License-Identifier: GPL-3.0-or-later — Copyright (C) 2026 Zachary Blauser */
/* relay.c — the hako relay (.claude/hako-studio/RELAY.md §6).
 *
 * Carries end-to-end-encrypted frames between devices. It never holds a key
 * that opens them: a frame is "<device id> <base64>" and stays base64 here.
 * Plain HTTP/1.1 on loopback; Caddy does TLS in front. libcrypto supplies
 * X25519 and BLAKE2b for the key-possession login, and the passkey signature
 * checks. Server only, never shipped to users.
 *
 *   POST /v1/auth/challenge   <pub hex>                -> <challenge> <relay ephemeral>
 *   POST /v1/auth/prove       <challenge> <mac hex>    -> <token> <device id> <account|->
 *   GET  /v1/health
 * with a device token (Bearer):
 *   GET  /v1/down             frames for this device, one per line
 *   POST /v1/up               lines "<to id> <base64>", streamed or not; same account only
 *   POST /v1/webauthn/challenge                        -> <challenge b64url>
 *   POST /v1/passkey/register <invite|-> <cred> <clientData> <authData> <spki>  -> <account>
 *   POST /v1/passkey/login    <cred> <clientData> <authData> <signature>       -> <account> <wrapped|->
 *   POST /v1/passkey/key      <cred> <wrapped>         the account key, wrapped by that passkey's prf
 *   POST /v1/email/start      <address>                mails a six-digit code
 *   POST /v1/email/verify     <code> <invite|->        -> <account> <new|old>
 *   POST /v1/github/start                              -> <github authorize url>
 *   POST /v1/github/verify    <code> <state> <invite|-> -> <account> <new|old>
 *   POST /v1/google/start, /v1/google/verify           the same, by Google
 * and signed in to an account:
 *   GET  /v1/devices          lines "<id> <pub hex> <online 0|1> <sealed name|->"
 *   POST /v1/bind             <pub hex>                adds a device to this account
 *   POST /v1/name             <sealed name b64>        this device's name, sealed by the devices
 *   POST /v1/revoke           <device id>
 *
 * Stored (RELAY.md §6): account ids, passkey public keys and their wrapped
 * account keys, keyed hashes of sign-in emails and GitHub or Google account
 * ids, device public keys with sealed names, spent invites. Never an email
 * address, a GitHub or Google token, an IP or a frame. GitHub sign-in reads
 * <dir>/github ("<client id> <client secret>"), Google <dir>/google, and both
 * ask the provider through curl, its config on stdin.
 *
 *   relay invite [n] [-d dir]   prints n single-use invite codes
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define RELAY_VERSION "0.0.2"
#define MAX_DEV    4096
#define MAX_CHAL   512
#define MAX_IP     1024
#define MAX_CONN   1024
#define MAX_FRAME  (256 * 1024)                 /* decoded bytes */
#define MAX_B64    ((MAX_FRAME + 2) / 3 * 4)
#define MAX_LINE   (MAX_B64 + 64)
#define RATE_BPS   (1024.0 * 1024.0)            /* per device, sustained */
#define RATE_BURST (4.0 * 1024.0 * 1024.0)
#define TOKEN_LIFE 3600
#define KEEPALIVE_S 25
#define UP_IDLE_S  900
#define MAX_ACCT   256
#define MAX_CRED   1024
#define MAX_LOGIN  1024
#define MAX_BIND   2048
#define MAX_SPENT  4096
#define DEV_PER_ACCT 32
#define MAILS_PER_MIN 20                        /* all addresses together */

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

typedef struct {
	int used;
	char id[33];
	unsigned char pub[32];
	char token[65];
	time_t tok_exp, seen;
	int down;                  /* the open /v1/down socket, or -1 */
	pthread_mutex_t w;         /* guards down and every write to it */
	double bucket;
	struct timeval bucket_t;
	char acct[17];             /* the account it is bound to, or "" */
	char wa_chal[44];          /* the open passkey challenge (b64url) */
	time_t wa_exp;
	char mail_hash[65], mail_code[65];
	time_t mail_exp;
	int mail_tries;
	char sso_state[33];        /* the open GitHub or Google sign-in, one try */
	int sso_p;                 /* which of ssos[] it is */
	time_t sso_exp;
	char hold[65];             /* a sign-in that stopped at "invite needed": its login hash */
	time_t hold_exp;
	int hold_tries;
} Dev;

typedef struct { int used; char id[17]; } Acct;
typedef struct { int used; char acct[17], cred[344], spki[804], wrapped[128]; } Cred;
typedef struct { int used; char acct[17], hash[65]; } Login;
typedef struct { int used; char acct[17], pub[65], name[260]; } Bind;

typedef struct { int used; char id[33]; unsigned char pub[32], epriv[32]; time_t exp; } Chal;
typedef struct { char ip[46]; int n; time_t start; } IpSlot;

static Dev devs[MAX_DEV];
static Chal chals[MAX_CHAL];
static IpSlot ips[MAX_IP];
static pthread_mutex_t dlock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t clock_ = PTHREAD_MUTEX_INITIALIZER;
static int conns;
static int verbose, dev_mode;
static int auth_per_min = 30;                          /* sign-in steps per IP per minute (-l) */

static Acct accts[MAX_ACCT];
static Cred creds[MAX_CRED];
static Login logins[MAX_LOGIN];
static Bind binds[MAX_BIND];
static char spent[MAX_SPENT][65];
static pthread_mutex_t slock = PTHREAD_MUTEX_INITIALIZER;   /* the store; never held with dlock */
static char data_dir[512] = "relay-data";
static char origin[256] = "http://localhost:8791";
static char rp_id[256] = "localhost";
static char mailer[512];
/* A sign-in provider: its three endpoints (-G points them at a fake) and
   <dir>/<name> ("<client id> <client secret>"; empty = off). oidc: Google's
   flavor, with a redirect_uri, scope openid and a string "sub". */
typedef struct {
	const char *name;
	int oidc;
	char auth[256], token[256], user[256];
	char id[128], secret[96];
} Sso;
static Sso ssos[] = {
	{ "github", 0, "https://github.com/login/oauth/authorize", "https://github.com/login/oauth/access_token",
	  "https://api.github.com/user", "", "" },
	{ "google", 1, "https://accounts.google.com/o/oauth2/v2/auth", "https://oauth2.googleapis.com/token",
	  "https://openidconnect.googleapis.com/v1/userinfo", "", "" },
};
#define NSSO ((int)(sizeof ssos / sizeof ssos[0]))
static char redirect_enc[800];                         /* "<origin>/", form-encoded */
static char sso_on[64];                                /* the startup line's ", sign-in by github google" */
static unsigned char secret[32];

/* ---- small helpers ---- */

static void hex_enc(char *out, const unsigned char *b, size_t n) {
	static const char d[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) { out[2 * i] = d[b[i] >> 4]; out[2 * i + 1] = d[b[i] & 15]; }
	out[2 * n] = '\0';
}

static int hex_val(int c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	return -1;
}

/* exactly 2n lowercase hex digits */
static int hex_dec(unsigned char *out, const char *h, size_t n) {
	for (size_t i = 0; i < n; i++) {
		int a = hex_val(h[2 * i]), b = a < 0 ? -1 : hex_val(h[2 * i + 1]);
		if (b < 0) return -1;
		out[i] = (unsigned char)(a << 4 | b);
	}
	return 0;
}

static int is_hex(const char *s, size_t n) {
	for (size_t i = 0; i < n; i++) if (hex_val(s[i]) < 0) return 0;
	return 1;
}

static int is_b64(const char *s, size_t n) {
	if (n == 0 || n % 4) return 0;
	for (size_t i = 0; i < n; i++) {
		char c = s[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
		      || c == '+' || c == '/' || (c == '=' && i >= n - 2))) return 0;
	}
	return 1;
}

static void blake2b(unsigned char out[64], const unsigned char *a, size_t al,
                    const unsigned char *b, size_t bl, const unsigned char *c, size_t cl) {
	EVP_MD_CTX *x = EVP_MD_CTX_new();
	unsigned int n = 64;
	EVP_DigestInit_ex(x, EVP_blake2b512(), NULL);
	EVP_DigestUpdate(x, a, al);
	if (bl) EVP_DigestUpdate(x, b, bl);
	if (cl) EVP_DigestUpdate(x, c, cl);
	EVP_DigestFinal_ex(x, out, &n);
	EVP_MD_CTX_free(x);
}

static void device_id(char out[33], const unsigned char pub[32]) {
	unsigned char h[64];
	blake2b(h, pub, 32, NULL, 0, NULL, 0);
	hex_enc(out, h, 16);
}

/* X25519 through libcrypto. Returns 0, or -1 for a failure or an all-zero
   result (a low-order point). */
static int x25519(unsigned char out[32], const unsigned char priv[32], const unsigned char pub[32]) {
	EVP_PKEY *k = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, priv, 32);
	EVP_PKEY *p = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, pub, 32);
	EVP_PKEY_CTX *c = k ? EVP_PKEY_CTX_new(k, NULL) : NULL;
	size_t n = 32;
	int ok = c && p && EVP_PKEY_derive_init(c) == 1 && EVP_PKEY_derive_set_peer(c, p) == 1
	         && EVP_PKEY_derive(c, out, &n) == 1 && n == 32;
	EVP_PKEY_CTX_free(c);
	EVP_PKEY_free(p);
	EVP_PKEY_free(k);
	unsigned char acc = 0;
	for (int i = 0; ok && i < 32; i++) acc |= out[i];
	return ok && acc ? 0 : -1;
}

static int x25519_pub(unsigned char pub[32], const unsigned char priv[32]) {
	EVP_PKEY *k = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, priv, 32);
	size_t n = 32;
	int ok = k && EVP_PKEY_get_raw_public_key(k, pub, &n) == 1 && n == 32;
	EVP_PKEY_free(k);
	return ok ? 0 : -1;
}

static void sha256(unsigned char out[32], const void *a, size_t al, const void *b, size_t bl) {
	EVP_MD_CTX *x = EVP_MD_CTX_new();
	unsigned int n = 32;
	EVP_DigestInit_ex(x, EVP_sha256(), NULL);
	EVP_DigestUpdate(x, a, al);
	if (bl) EVP_DigestUpdate(x, b, bl);
	EVP_DigestFinal_ex(x, out, &n);
	EVP_MD_CTX_free(x);
}

/* hex HMAC-SHA256(secret, tag NUL s): what stands in for an email address (or
   an SSO subject), so the address itself is never stored. */
static void keyed_hash(char out[65], const char *tag, const char *s) {
	unsigned char m[32] = {0};
	size_t n = 32;
	EVP_MAC *mac = EVP_MAC_fetch(NULL, "HMAC", NULL);
	EVP_MAC_CTX *x = mac ? EVP_MAC_CTX_new(mac) : NULL;
	OSSL_PARAM ps[] = { OSSL_PARAM_construct_utf8_string("digest", "SHA256", 0), OSSL_PARAM_construct_end() };
	if (x && EVP_MAC_init(x, secret, 32, ps) == 1) {
		EVP_MAC_update(x, (const unsigned char *)tag, strlen(tag) + 1);
		EVP_MAC_update(x, (const unsigned char *)s, strlen(s));
		EVP_MAC_final(x, m, &n, 32);
	}
	EVP_MAC_CTX_free(x);
	EVP_MAC_free(mac);
	hex_enc(out, m, 32);
}

static int b64url_val(int c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '-') return 62;
	if (c == '_') return 63;
	return -1;
}

/* Unpadded base64url, as WebAuthn uses it. Returns the length, or -1. */
static long b64url_dec(unsigned char *out, size_t cap, const char *s, size_t n) {
	size_t o = 0;
	unsigned v = 0;
	int bits = 0;
	if (n % 4 == 1) return -1;
	for (size_t i = 0; i < n; i++) {
		int d = b64url_val(s[i]);
		if (d < 0) return -1;
		v = (v << 6) | (unsigned)d;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (o >= cap) return -1;
			out[o++] = (unsigned char)(v >> bits);
		}
	}
	return (long)o;
}

static int is_b64url(const char *s, size_t n) {
	for (size_t i = 0; i < n; i++) if (b64url_val(s[i]) < 0) return 0;
	return n > 0;
}

/* The string value of "key" in the browser's clientDataJSON. The browser
   writes it (type, challenge, origin come first and hold no escapes), so an
   escape anywhere in the value is refused rather than decoded. */
static int json_str(const char *j, const char *key, char *out, size_t cap) {
	char pat[32];
	snprintf(pat, sizeof pat, "\"%s\"", key);
	const char *p = strstr(j, pat);
	if (!p) return -1;
	p += strlen(pat);
	while (*p == ' ') p++;
	if (*p++ != ':') return -1;
	while (*p == ' ') p++;
	if (*p++ != '"') return -1;
	size_t n = 0;
	for (; *p && *p != '"'; p++) {
		if (*p == '\\' || n + 1 >= cap) return -1;
		out[n++] = *p;
	}
	if (*p != '"') return -1;
	out[n] = '\0';
	return 0;
}

static void rand_hex(char *out, size_t bytes) {
	unsigned char b[32];
	if (bytes > sizeof b || RAND_bytes(b, (int)bytes) != 1) { out[0] = '\0'; return; }
	hex_enc(out, b, bytes);
}

/* ---- the store: one flat file, rewritten whole on every change ----
   a <account>
   c <account> <credential id> <spki b64url> <wrapped account key|->
   l <account> <keyed hash of an email or SSO subject>
   b <account> <device pub hex> <sealed name|->
   x <spent invite hash>
   Caller holds slock. */
static int store_save(void) {
	char path[600], tmp[600];
	snprintf(path, sizeof path, "%s/store", data_dir);
	snprintf(tmp, sizeof tmp, "%s/store.tmp", data_dir);
	int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL;
	if (!f) { if (fd >= 0) close(fd); return -1; }
	for (int i = 0; i < MAX_ACCT; i++) if (accts[i].used) fprintf(f, "a %s\n", accts[i].id);
	for (int i = 0; i < MAX_CRED; i++) if (creds[i].used)
		fprintf(f, "c %s %s %s %s\n", creds[i].acct, creds[i].cred, creds[i].spki, creds[i].wrapped[0] ? creds[i].wrapped : "-");
	for (int i = 0; i < MAX_LOGIN; i++) if (logins[i].used) fprintf(f, "l %s %s\n", logins[i].acct, logins[i].hash);
	for (int i = 0; i < MAX_BIND; i++) if (binds[i].used)
		fprintf(f, "b %s %s %s\n", binds[i].acct, binds[i].pub, binds[i].name[0] ? binds[i].name : "-");
	for (int i = 0; i < MAX_SPENT; i++) if (spent[i][0]) fprintf(f, "x %s\n", spent[i]);
	int ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
	ok = fclose(f) == 0 && ok;
	if (!ok || rename(tmp, path)) { unlink(tmp); return -1; }
	return 0;
}

static void copy_field(char *dst, size_t cap, const char *src) {
	if (!src || !strcmp(src, "-")) { dst[0] = '\0'; return; }
	snprintf(dst, cap, "%s", src);
}

static void store_load(void) {
	char path[600];
	snprintf(path, sizeof path, "%s/store", data_dir);
	FILE *f = fopen(path, "r");
	if (!f) return;
	char *line = NULL;
	size_t cap = 0;
	int na = 0, nc = 0, nl = 0, nb = 0, nx = 0;
	while (getline(&line, &cap, f) > 0) {
		line[strcspn(line, "\r\n")] = '\0';
		char *save = NULL, *k = strtok_r(line, " ", &save);
		char *f1 = strtok_r(NULL, " ", &save), *f2 = strtok_r(NULL, " ", &save);
		char *f3 = strtok_r(NULL, " ", &save), *f4 = strtok_r(NULL, " ", &save);
		if (!k || !f1) continue;
		if (!strcmp(k, "a") && na < MAX_ACCT) { accts[na].used = 1; copy_field(accts[na++].id, 17, f1); }
		else if (!strcmp(k, "c") && f3 && nc < MAX_CRED) {
			Cred *c = &creds[nc++];
			c->used = 1;
			copy_field(c->acct, sizeof c->acct, f1); copy_field(c->cred, sizeof c->cred, f2);
			copy_field(c->spki, sizeof c->spki, f3); copy_field(c->wrapped, sizeof c->wrapped, f4);
		} else if (!strcmp(k, "l") && f2 && nl < MAX_LOGIN) {
			logins[nl].used = 1;
			copy_field(logins[nl].acct, 17, f1); copy_field(logins[nl++].hash, 65, f2);
		} else if (!strcmp(k, "b") && f2 && nb < MAX_BIND) {
			Bind *b = &binds[nb++];
			b->used = 1;
			copy_field(b->acct, sizeof b->acct, f1); copy_field(b->pub, sizeof b->pub, f2);
			copy_field(b->name, sizeof b->name, f3);
		} else if (!strcmp(k, "x") && nx < MAX_SPENT) copy_field(spent[nx++], 65, f1);
	}
	free(line);
	fclose(f);
}

/* The account a device key is bound to, into out ("" if none). */
static void bind_acct(const char *pubhex, char out[17]) {
	out[0] = '\0';
	pthread_mutex_lock(&slock);
	for (int i = 0; i < MAX_BIND; i++)
		if (binds[i].used && !strcmp(binds[i].pub, pubhex)) { memcpy(out, binds[i].acct, 17); break; }
	pthread_mutex_unlock(&slock);
}

/* Binds pubhex to acct. Caller holds slock. 0, or -1 when the key belongs to
   another account (revoke it there first), the account is at its device cap,
   or the store is full. */
static int bind_add(const char *acct, const char *pubhex) {
	Bind *spare = NULL, *hit = NULL;
	int n = 0;
	for (int i = 0; i < MAX_BIND; i++) {
		Bind *b = &binds[i];
		if (b->used && !strcmp(b->pub, pubhex)) hit = b;
		else if (b->used && !strcmp(b->acct, acct)) n++;
		else if (!b->used && !spare) spare = b;
	}
	if (hit) return strcmp(hit->acct, acct) ? -1 : 0;
	if (n >= DEV_PER_ACCT || (!hit && !spare)) return -1;
	hit = spare;
	memset(hit, 0, sizeof *hit);
	hit->used = 1;
	snprintf(hit->pub, sizeof hit->pub, "%s", pubhex);
	memcpy(hit->acct, acct, 17);
	return store_save();
}

/* A new account id. Caller holds slock. */
static Acct *acct_new(void) {
	for (int i = 0; i < MAX_ACCT; i++) {
		if (accts[i].used) continue;
		rand_hex(accts[i].id, 8);
		if (!accts[i].id[0]) return NULL;
		accts[i].used = 1;
		return &accts[i];
	}
	return NULL;
}

static void invite_norm(char *out, size_t cap, const char *in) {
	size_t n = 0;
	for (; *in && n + 1 < cap; in++) {
		if (*in == '-' || *in == ' ') continue;
		out[n++] = (char)((*in >= 'A' && *in <= 'Z') ? *in + 32 : *in);
	}
	out[n] = '\0';
}

/* Spends an invite code: it must be in <dir>/invites (hashes, written by
   `relay invite`) and not yet spent. Caller holds slock. 1 when it was good. */
static int invite_take(const char *code) {
	char norm[64], h[65], path[600], line[128];
	unsigned char d[32];
	invite_norm(norm, sizeof norm, code);
	if (strlen(norm) < 10) return 0;
	sha256(d, "hako invite ", 12, norm, strlen(norm));
	hex_enc(h, d, 32);
	for (int i = 0; i < MAX_SPENT; i++) if (!strcmp(spent[i], h)) return 0;
	snprintf(path, sizeof path, "%s/invites", data_dir);
	FILE *f = fopen(path, "r");
	int found = 0;
	while (f && !found && fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = '\0';
		found = !strcmp(line, h);
	}
	if (f) fclose(f);
	if (!found) return 0;
	for (int i = 0; i < MAX_SPENT; i++) if (!spent[i][0]) { memcpy(spent[i], h, 65); return store_save() == 0; }
	return 0;
}

static void send_all(int fd, const char *b, size_t n) {
	while (n > 0) {
		ssize_t w = send(fd, b, n, MSG_NOSIGNAL);
		if (w <= 0) { if (w < 0 && errno == EINTR) continue; return; }
		b += w; n -= (size_t)w;
	}
}

/* The studio page is another origin (static hosting); only it may call us
   from a browser. */
static char cors[320];

static void reply(int fd, const char *status, const char *body) {
	char head[640];
	int n = snprintf(head, sizeof head,
		"HTTP/1.1 %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\n"
		"Cache-Control: no-store\r\n%sConnection: close\r\n\r\n", status, strlen(body), cors);
	send_all(fd, head, (size_t)n);
	send_all(fd, body, strlen(body));
}

/* ---- request reading ---- */

typedef struct {
	int fd;
	char *buf;                 /* leftover bytes after the headers, then refills */
	size_t have, at;
	int chunked, done;
	long remain;               /* Content-Length left, or the current chunk's */
} Body;

static int raw_fill(Body *b) {
	if (b->at < b->have) return 1;
	ssize_t r = recv(b->fd, b->buf, 16384, 0);
	if (r <= 0) return 0;
	b->have = (size_t)r; b->at = 0;
	return 1;
}

static int raw_byte(Body *b) {
	if (!raw_fill(b)) return -1;
	return (unsigned char)b->buf[b->at++];
}

/* One body byte, through chunked framing when the request used it. -1 at the end. */
static int body_byte(Body *b) {
	if (b->done) return -1;
	if (!b->chunked) {
		if (b->remain <= 0) { b->done = 1; return -1; }
		int c = raw_byte(b);
		if (c < 0) { b->done = 1; return -1; }
		b->remain--;
		return c;
	}
	while (b->remain == 0) {
		long size = 0;
		int c, digits = 0;
		while ((c = raw_byte(b)) >= 0 && c != '\r' && c != '\n' && c != ';') {
			int v = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10
			      : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
			if (v < 0 || ++digits > 8) { b->done = 1; return -1; }
			size = size * 16 + v;
		}
		while (c >= 0 && c != '\n') c = raw_byte(b);              /* extensions, CRLF */
		if (c < 0) { b->done = 1; return -1; }
		if (size == 0) { b->done = 1; return -1; }
		b->remain = size;
	}
	int c = raw_byte(b);
	if (c < 0) { b->done = 1; return -1; }
	if (--b->remain == 0) {                                          /* the CRLF after a chunk */
		int r = raw_byte(b);
		if (r == '\r') r = raw_byte(b);
		if (r != '\n') { b->done = 1; }
	}
	return c;
}

/* A line without its newline. Returns its length, -1 at the end, -2 if too long. */
static long body_line(Body *b, char *out, size_t cap) {
	size_t n = 0;
	int c;
	while ((c = body_byte(b)) >= 0) {
		if (c == '\n') { out[n] = '\0'; return (long)n; }
		if (n + 1 >= cap) {
			while ((c = body_byte(b)) >= 0 && c != '\n') {}
			return -2;
		}
		out[n++] = (char)c;
	}
	if (n == 0) return -1;
	out[n] = '\0';
	return (long)n;
}

static const char *header(const char *req, const char *name) {
	size_t nl = strlen(name);
	for (const char *p = strstr(req, "\r\n"); p; p = strstr(p + 2, "\r\n")) {
		if (!strncasecmp(p + 2, name, nl) && p[2 + nl] == ':') {
			p += 3 + nl;
			while (*p == ' ') p++;
			return p;
		}
	}
	return NULL;
}

/* ---- devices, challenges, throttles ---- */

static Dev *dev_by_token(const char *req) {
	const char *a = header(req, "Authorization");
	if (!a || strncmp(a, "Bearer ", 7)) return NULL;
	a += 7;
	if (strlen(a) < 64 || !is_hex(a, 64)) return NULL;
	time_t now = time(NULL);
	Dev *hit = NULL;
	pthread_mutex_lock(&dlock);
	for (int i = 0; i < MAX_DEV; i++)
		if (devs[i].used && devs[i].tok_exp > now && CRYPTO_memcmp(devs[i].token, a, 64) == 0) hit = &devs[i];
	if (hit) hit->seen = now;
	pthread_mutex_unlock(&dlock);
	return hit;
}

static Dev *dev_by_id(const char *id) {
	Dev *hit = NULL;
	pthread_mutex_lock(&dlock);
	for (int i = 0; i < MAX_DEV && !hit; i++) if (devs[i].used && !strcmp(devs[i].id, id)) hit = &devs[i];
	pthread_mutex_unlock(&dlock);
	return hit;
}

/* The device's slot, made if new. When full, the longest-idle slot with no
   stream and no live token is reused. */
static Dev *dev_upsert(const unsigned char pub[32]) {
	char id[33];
	device_id(id, pub);
	time_t now = time(NULL);
	Dev *hit = NULL, *spare = NULL;
	pthread_mutex_lock(&dlock);
	for (int i = 0; i < MAX_DEV && !hit; i++) {
		Dev *d = &devs[i];
		if (d->used && !strcmp(d->id, id)) hit = d;
		else if (!d->used && !spare) spare = d;
	}
	if (!hit && !spare) {
		for (int i = 0; i < MAX_DEV; i++) {
			Dev *d = &devs[i];
			if (d->down < 0 && d->tok_exp < now && now - d->seen > 7200 && (!spare || d->seen < spare->seen)) spare = d;
		}
	}
	if (!hit && spare) {
		hit = spare;
		if (!hit->used) { pthread_mutex_init(&hit->w, NULL); hit->down = -1; }
		hit->used = 1;
		memcpy(hit->id, id, 33);
		memcpy(hit->pub, pub, 32);
		hit->acct[0] = hit->wa_chal[0] = hit->mail_code[0] = '\0';
		hit->bucket = RATE_BURST;
		gettimeofday(&hit->bucket_t, NULL);
	}
	if (hit) hit->seen = now;
	pthread_mutex_unlock(&dlock);
	return hit;
}

/* Spend n bytes of the device's allowance. 0 when it is over the rate. */
static int dev_spend(Dev *d, size_t n) {
	struct timeval t;
	gettimeofday(&t, NULL);
	pthread_mutex_lock(&dlock);
	double dt = (double)(t.tv_sec - d->bucket_t.tv_sec) + (double)(t.tv_usec - d->bucket_t.tv_usec) / 1e6;
	d->bucket_t = t;
	d->bucket += dt * RATE_BPS;
	if (d->bucket > RATE_BURST) d->bucket = RATE_BURST;
	int ok = d->bucket >= (double)n;
	if (ok) d->bucket -= (double)n;
	pthread_mutex_unlock(&dlock);
	return ok;
}

static int ip_allow(const char *ip) {
	time_t now = time(NULL);
	unsigned h = 5381;
	for (const char *p = ip; *p; p++) h = h * 33 + (unsigned char)*p;
	pthread_mutex_lock(&dlock);
	IpSlot *s = &ips[h % MAX_IP];
	if (strcmp(s->ip, ip) || now - s->start >= 60) {
		snprintf(s->ip, sizeof s->ip, "%s", ip);
		s->n = 0;
		s->start = now;
	}
	int ok = ++s->n <= auth_per_min;
	pthread_mutex_unlock(&dlock);
	return ok;
}

/* Write one line to a device's stream, or report that it has none. */
static int dev_deliver(Dev *d, const char *line, size_t n) {
	pthread_mutex_lock(&d->w);
	int fd = d->down;
	if (fd >= 0) send_all(fd, line, n);
	pthread_mutex_unlock(&d->w);
	return fd >= 0;
}

/* ---- routes ---- */

static void route_challenge(int fd, Body *b, const char *ip) {
	char line[160];
	if (!ip_allow(ip)) { reply(fd, "429 Too Many Requests", "slow down\n"); return; }
	long n = body_line(b, line, sizeof line);
	unsigned char pub[32];
	if (n != 64 || hex_dec(pub, line, 32)) { reply(fd, "400 Bad Request", "want: <public key hex>\n"); return; }
	unsigned char cid[16], epub[32];
	char out[160], cidh[33], epubh[65];
	time_t now = time(NULL);
	pthread_mutex_lock(&dlock);
	Chal *c = NULL;
	for (int i = 0; i < MAX_CHAL && !c; i++) if (!chals[i].used || chals[i].exp < now) c = &chals[i];
	if (c) {
		c->used = 1;
		c->exp = now + 60;
		memcpy(c->pub, pub, 32);
		if (RAND_bytes(cid, 16) != 1 || RAND_bytes(c->epriv, 32) != 1 || x25519_pub(epub, c->epriv)) c->used = 0;
		else hex_enc(c->id, cid, 16);
	}
	int ok = c && c->used;
	if (ok) { memcpy(cidh, c->id, 33); hex_enc(epubh, epub, 32); }
	pthread_mutex_unlock(&dlock);
	if (!ok) { reply(fd, "503 Service Unavailable", "busy\n"); return; }
	snprintf(out, sizeof out, "%s %s\n", cidh, epubh);
	reply(fd, "200 OK", out);
}

/* The device proves it holds the private key for its public key: the MAC is
   keyed by X25519(device secret, relay ephemeral), which only it and we can
   compute. One try per challenge. */
static void route_prove(int fd, Body *b, const char *ip) {
	char line[200];
	if (!ip_allow(ip)) { reply(fd, "429 Too Many Requests", "slow down\n"); return; }
	long n = body_line(b, line, sizeof line);
	unsigned char mac[32], cid[16];
	if (n != 32 + 1 + 64 || line[32] != ' ' || hex_dec(cid, line, 16) || hex_dec(mac, line + 33, 32)) {
		reply(fd, "400 Bad Request", "want: <challenge> <mac hex>\n");
		return;
	}
	line[32] = '\0';
	unsigned char pub[32], epriv[32];
	int found = 0;
	time_t now = time(NULL);
	pthread_mutex_lock(&dlock);
	for (int i = 0; i < MAX_CHAL; i++) {
		Chal *c = &chals[i];
		if (c->used && c->exp >= now && !strcmp(c->id, line)) {
			memcpy(pub, c->pub, 32);
			memcpy(epriv, c->epriv, 32);
			OPENSSL_cleanse(c, sizeof *c);
			found = 1;
			break;
		}
	}
	pthread_mutex_unlock(&dlock);
	unsigned char k[32], want[64];
	int ok = found && x25519(k, epriv, pub) == 0;
	if (ok) {
		blake2b(want, k, 32, cid, 16, pub, 32);
		ok = CRYPTO_memcmp(want, mac, 32) == 0;
	}
	OPENSSL_cleanse(epriv, sizeof epriv);
	OPENSSL_cleanse(k, sizeof k);
	if (!ok) { reply(fd, "403 Forbidden", "no\n"); return; }
	Dev *d = dev_upsert(pub);
	if (!d) { reply(fd, "503 Service Unavailable", "full\n"); return; }
	unsigned char t[32];
	char out[160], pubhex[65], acct[17];
	hex_enc(pubhex, pub, 32);
	bind_acct(pubhex, acct);
	if (RAND_bytes(t, 32) != 1) { reply(fd, "503 Service Unavailable", "no entropy\n"); return; }
	pthread_mutex_lock(&dlock);
	hex_enc(d->token, t, 32);
	d->tok_exp = now + TOKEN_LIFE;
	memcpy(d->acct, acct, 17);
	snprintf(out, sizeof out, "%s %s %s\n", d->token, d->id, acct[0] ? acct : "-");
	pthread_mutex_unlock(&dlock);
	OPENSSL_cleanse(t, sizeof t);
	reply(fd, "200 OK", out);
}

/* Holds the socket open; frames arrive from other connections' threads. A
   newer stream for the same device replaces this one. */
static void route_down(int fd, Dev *d) {
	char head[640];
	/* nosniff: Safari otherwise holds back the first few hundred bytes of a
	   text/plain stream to guess its type, and the first replies sit there */
	snprintf(head, sizeof head, "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nCache-Control: no-store\r\n"
	         "X-Content-Type-Options: nosniff\r\nX-Accel-Buffering: no\r\n%sConnection: close\r\n\r\n", cors);
	struct timeval tv = { 10, 0 };
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
	pthread_mutex_lock(&d->w);
	if (d->down >= 0) shutdown(d->down, SHUT_RDWR);
	d->down = fd;
	send_all(fd, head, strlen(head));
	/* Safari resolves the fetch only once body bytes arrive (and buffers the
	   first few hundred): a burst of keepalives now, so it opens at once */
	char pad[1024];
	memset(pad, '\n', sizeof pad);
	send_all(fd, pad, sizeof pad);
	pthread_mutex_unlock(&d->w);
	for (;;) {
		struct pollfd p = { fd, POLLIN, 0 };
		int r = poll(&p, 1, KEEPALIVE_S * 1000);
		if (r < 0 && errno == EINTR) continue;
		if (r != 0) {
			char junk[256];
			if (r < 0 || recv(fd, junk, sizeof junk, 0) <= 0) break;
			continue;
		}
		pthread_mutex_lock(&d->w);
		int mine = d->down == fd;
		if (mine) send_all(fd, "\n", 1);
		pthread_mutex_unlock(&d->w);
		if (!mine) break;
	}
	pthread_mutex_lock(&d->w);
	if (d->down == fd) d->down = -1;
	pthread_mutex_unlock(&d->w);
}

static void route_up(int fd, Body *b, Dev *from) {
	char *line = malloc(MAX_LINE), *out = malloc(MAX_LINE);
	if (!line || !out) { free(line); free(out); reply(fd, "503 Service Unavailable", "memory\n"); return; }
	struct timeval tv = { UP_IDLE_S, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	long n, frames = 0, bad = 0;
	while ((n = body_line(b, line, MAX_LINE)) != -1) {
		if (n == 0) continue;                                        /* keepalive */
		char *sp = n > 33 ? line + 32 : NULL;
		if (n == -2 || !sp || *sp != ' ' || !is_hex(line, 32) || !is_b64(sp + 1, (size_t)(n - 33))) { bad++; continue; }
		if (!dev_spend(from, (size_t)n)) { bad++; continue; }
		*sp = '\0';
		Dev *to = dev_by_id(line);
		if (to) {                                                    /* one account, or not at all */
			pthread_mutex_lock(&dlock);
			if (!from->acct[0] || strcmp(from->acct, to->acct)) to = NULL;
			pthread_mutex_unlock(&dlock);
		}
		int m = snprintf(out, MAX_LINE, "%s %s\n", from->id, sp + 1);
		int ok = to && dev_deliver(to, out, (size_t)m);
		if (verbose) {
			struct timeval tv;
			gettimeofday(&tv, NULL);
			fprintf(stderr, "relay: %ld.%03d frame %.8s -> %.8s %zu %s\n", (long)tv.tv_sec % 1000, (int)(tv.tv_usec / 1000),
			        from->id, line, (size_t)n, ok ? "delivered" : "offline");
		}
		if (!ok) {
			m = snprintf(out, MAX_LINE, "! offline %s\n", line);
			dev_deliver(from, out, (size_t)m);
		}
		frames++;
	}
	char msg[64];
	snprintf(msg, sizeof msg, "ok %ld %ld\n", frames, bad);
	reply(fd, "200 OK", msg);
	free(line);
	free(out);
}

/* ---- accounts (RELAY.md §4, §6) ---- */

static void b64url_enc(char *out, const unsigned char *b, size_t n) {
	static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
	size_t o = 0;
	unsigned v = 0;
	int bits = 0;
	for (size_t i = 0; i < n; i++) {
		v = (v << 8) | b[i];
		bits += 8;
		while (bits >= 6) { bits -= 6; out[o++] = t[(v >> bits) & 63]; }
	}
	if (bits) out[o++] = t[(v << (6 - bits)) & 63];
	out[o] = '\0';
}

static void dev_acct(Dev *d, char out[17]) {
	pthread_mutex_lock(&dlock);
	memcpy(out, d->acct, 17);
	pthread_mutex_unlock(&dlock);
}

/* Binds the device to acct in the store, then its live slot. Caller holds
   slock; the slot is set after, by dev_bound. */
static int dev_bind_locked(Dev *d, const char *acct) {
	char pub[65];
	hex_enc(pub, d->pub, 32);
	return bind_add(acct, pub);
}

static void dev_bound(Dev *d, const char *acct) {
	pthread_mutex_lock(&dlock);
	memcpy(d->acct, acct, 17);
	pthread_mutex_unlock(&dlock);
}

/* Splits a request line into up to max space-separated fields in place. */
static int fields(char *line, char **f, int max) {
	int n = 0;
	char *save = NULL;
	char *t = strtok_r(line, " ", &save);
	for (; t && n < max; t = strtok_r(NULL, " ", &save)) f[n++] = t;
	return t ? -1 : n;
}

static void route_wa_challenge(int fd, Dev *d) {
	unsigned char c[32];
	char out[48];
	if (RAND_bytes(c, 32) != 1) { reply(fd, "503 Service Unavailable", "no entropy\n"); return; }
	b64url_enc(out, c, 32);
	pthread_mutex_lock(&dlock);
	memcpy(d->wa_chal, out, 44);
	d->wa_exp = time(NULL) + 120;
	pthread_mutex_unlock(&dlock);
	strcat(out, "\n");
	reply(fd, "200 OK", out);
}

/* Checks clientDataJSON and authenticatorData against the device's open
   challenge (spent here, one try), our origin and rp id, with the user present
   and verified. On success cdj (NUL-ended) and ad hold the decoded bytes. */
static int wa_check(Dev *d, const char *type, const char *cdj64, const char *ad64,
                    unsigned char *cdj, long *cdjn, unsigned char *ad, long *adn) {
	char want[44], v[300];
	pthread_mutex_lock(&dlock);
	memcpy(want, d->wa_chal, sizeof want);
	int live = want[0] && d->wa_exp >= time(NULL);
	d->wa_chal[0] = '\0';
	pthread_mutex_unlock(&dlock);
	if (!live) return -1;
	*cdjn = b64url_dec(cdj, 2047, cdj64, strlen(cdj64));
	*adn = b64url_dec(ad, 2048, ad64, strlen(ad64));
	if (*cdjn <= 0 || *adn < 37) return -1;
	cdj[*cdjn] = '\0';
	const char *j = (const char *)cdj;
	if (json_str(j, "type", v, sizeof v) || strcmp(v, type)) return -1;
	if (json_str(j, "challenge", v, sizeof v) || strcmp(v, want)) return -1;
	if (json_str(j, "origin", v, sizeof v) || strcmp(v, origin)) return -1;
	const char *co = strstr(j, "\"crossOrigin\"");
	if (co) {
		co += 13;
		while (*co == ' ' || *co == ':') co++;
		if (!strncmp(co, "true", 4)) return -1;
	}
	unsigned char rh[32];
	sha256(rh, rp_id, strlen(rp_id), NULL, 0);
	if (CRYPTO_memcmp(ad, rh, 32)) return -1;
	return (ad[32] & 0x05) == 0x05 ? 0 : -1;                         /* UP and UV */
}

/* A passkey public key we can check: P-256 (ES256) or RSA >= 2048 (RS256,
   Windows Hello). Returns the key, or NULL. */
static EVP_PKEY *spki_key(const char *spki64) {
	unsigned char der[600];
	long n = b64url_dec(der, sizeof der, spki64, strlen(spki64));
	if (n <= 0) return NULL;
	const unsigned char *p = der;
	EVP_PKEY *k = d2i_PUBKEY(NULL, &p, n);
	if (!k) return NULL;
	char g[32] = "";
	size_t gl = 0;
	int ok = (EVP_PKEY_is_a(k, "EC") && EVP_PKEY_get_group_name(k, g, sizeof g, &gl) == 1 && !strcmp(g, "prime256v1"))
	         || (EVP_PKEY_is_a(k, "RSA") && EVP_PKEY_get_bits(k) >= 2048);
	if (!ok) { EVP_PKEY_free(k); return NULL; }
	return k;
}

static int wa_verify(const char *spki64, const unsigned char *ad, size_t adn,
                     const unsigned char *cdj, size_t cdjn, const unsigned char *sig, size_t sign) {
	EVP_PKEY *k = spki_key(spki64);
	unsigned char *msg = malloc(adn + 32);
	EVP_MD_CTX *x = EVP_MD_CTX_new();
	int ok = 0;
	if (k && msg && x) {
		memcpy(msg, ad, adn);
		sha256(msg + adn, cdj, cdjn, NULL, 0);
		ok = EVP_DigestVerifyInit(x, NULL, EVP_sha256(), NULL, k) == 1
		     && EVP_DigestVerify(x, sig, sign, msg, adn + 32) == 1;
	}
	EVP_MD_CTX_free(x);
	free(msg);
	EVP_PKEY_free(k);
	return ok ? 0 : -1;
}

/* <invite|-> <credential id> <clientDataJSON> <authenticatorData> <spki>
   A new account needs an invite; a device already signed in adds a passkey
   to its own account with "-". */
static void route_pk_register(int fd, Body *b, Dev *d, const char *ip) {
	char line[8192], *f[5], acct[17], out[32];
	unsigned char cdj[2048], ad[2048], cred[256];
	long cdjn, adn;
	if (!ip_allow(ip)) { reply(fd, "429 Too Many Requests", "slow down\n"); return; }
	if (body_line(b, line, sizeof line) < 0 || fields(line, f, 5) != 5 || strlen(f[1]) > 340
	    || !is_b64url(f[1], strlen(f[1])) || strlen(f[4]) > 800) { reply(fd, "400 Bad Request", "fields\n"); return; }
	long cn = b64url_dec(cred, sizeof cred, f[1], strlen(f[1]));
	if (cn <= 0 || wa_check(d, "webauthn.create", f[2], f[3], cdj, &cdjn, ad, &adn)) { reply(fd, "403 Forbidden", "no\n"); return; }
	/* attested credential data: aaguid(16) idlen(2) id, after rpIdHash(32) flags(1) count(4) */
	size_t L = adn >= 55 ? (size_t)ad[53] << 8 | ad[54] : 0;
	EVP_PKEY *k = spki_key(f[4]);
	int ok = k && (ad[32] & 0x40) && L == (size_t)cn && 55 + L <= (size_t)adn && !memcmp(ad + 55, cred, L);
	EVP_PKEY_free(k);
	if (!ok) { reply(fd, "403 Forbidden", "no\n"); return; }
	dev_acct(d, acct);
	const char *err = NULL;
	pthread_mutex_lock(&slock);
	Cred *spare = NULL;
	for (int i = 0; i < MAX_CRED && !err; i++) {
		if (creds[i].used && !strcmp(creds[i].cred, f[1])) err = "409 Conflict";
		else if (!creds[i].used && !spare) spare = &creds[i];
	}
	if (!err && !spare) err = "503 Service Unavailable";
	if (!err && !acct[0]) {
		Acct *a = strcmp(f[0], "-") && invite_take(f[0]) ? acct_new() : NULL;
		if (a) memcpy(acct, a->id, 17);
		else err = "403 Forbidden";
	}
	if (!err) {
		memset(spare, 0, sizeof *spare);
		spare->used = 1;
		memcpy(spare->acct, acct, 17);
		snprintf(spare->cred, sizeof spare->cred, "%s", f[1]);
		snprintf(spare->spki, sizeof spare->spki, "%s", f[4]);
		if (dev_bind_locked(d, acct) || store_save()) { spare->used = 0; err = "503 Service Unavailable"; }
	}
	pthread_mutex_unlock(&slock);
	if (err) { reply(fd, err, "no\n"); return; }
	dev_bound(d, acct);
	snprintf(out, sizeof out, "%s\n", acct);
	reply(fd, "200 OK", out);
}

/* <credential id> <clientDataJSON> <authenticatorData> <signature> */
static void route_pk_login(int fd, Body *b, Dev *d, const char *ip) {
	char line[8192], *f[4], acct[17] = "", spki[804] = "", wrapped[128] = "", out[192];
	unsigned char cdj[2048], ad[2048], sig[600];
	long cdjn, adn;
	if (!ip_allow(ip)) { reply(fd, "429 Too Many Requests", "slow down\n"); return; }
	if (body_line(b, line, sizeof line) < 0 || fields(line, f, 4) != 4) { reply(fd, "400 Bad Request", "fields\n"); return; }
	long sn = b64url_dec(sig, sizeof sig, f[3], strlen(f[3]));
	int ok = sn > 0 && wa_check(d, "webauthn.get", f[1], f[2], cdj, &cdjn, ad, &adn) == 0;
	pthread_mutex_lock(&slock);
	for (int i = 0; ok && i < MAX_CRED; i++)
		if (creds[i].used && !strcmp(creds[i].cred, f[0])) {
			memcpy(acct, creds[i].acct, 17);
			memcpy(spki, creds[i].spki, sizeof spki);
			memcpy(wrapped, creds[i].wrapped, sizeof wrapped);
			break;
		}
	pthread_mutex_unlock(&slock);
	if (!ok || !acct[0] || wa_verify(spki, ad, (size_t)adn, cdj, (size_t)cdjn, sig, (size_t)sn)) {
		reply(fd, "403 Forbidden", "no\n");
		return;
	}
	pthread_mutex_lock(&slock);
	int r = dev_bind_locked(d, acct);
	pthread_mutex_unlock(&slock);
	if (r) { reply(fd, "409 Conflict", "this device belongs to another account\n"); return; }
	dev_bound(d, acct);
	snprintf(out, sizeof out, "%s %s\n", acct, wrapped[0] ? wrapped : "-");
	reply(fd, "200 OK", out);
}

/* <credential id> <wrapped account key>: sealed in the browser under that
   passkey's prf output, which we never see. */
static void route_pk_key(int fd, Body *b, Dev *d) {
	char line[1024], *f[2], acct[17];
	dev_acct(d, acct);
	if (body_line(b, line, sizeof line) < 0 || fields(line, f, 2) != 2 || strlen(f[1]) > 120
	    || !is_b64url(f[1], strlen(f[1]))) { reply(fd, "400 Bad Request", "fields\n"); return; }
	int ok = 0;
	pthread_mutex_lock(&slock);
	for (int i = 0; i < MAX_CRED; i++)
		if (creds[i].used && !strcmp(creds[i].cred, f[0]) && !strcmp(creds[i].acct, acct)) {
			snprintf(creds[i].wrapped, sizeof creds[i].wrapped, "%s", f[1]);
			ok = store_save() == 0;
			break;
		}
	pthread_mutex_unlock(&slock);
	reply(fd, ok ? "200 OK" : "403 Forbidden", ok ? "ok\n" : "no\n");
}

/* An address we will put in a To: header: one @, nothing that could end the
   header or name a second recipient. Lowercased into out. */
static int email_norm(char *out, size_t cap, const char *in) {
	size_t n = strlen(in);
	int at = 0;
	if (n < 3 || n > 254 || n >= cap) return -1;
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)in[i];
		if (c <= ' ' || c >= 0x7f || strchr("<>()[]\\,;:\"", c)) return -1;
		if (c == '@') at++;
		out[i] = (char)((c >= 'A' && c <= 'Z') ? c + 32 : c);
	}
	out[n] = '\0';
	return at == 1 && out[0] != '@' && out[n - 1] != '@' ? 0 : -1;
}

/* A pipe no other child inherits: each sign-in thread forks, and a stray write
   end held by someone else's curl would keep this one's stdin open. */
static int pipe_cx(int p[2]) {
	if (pipe(p)) return -1;
	fcntl(p[0], F_SETFD, FD_CLOEXEC);
	fcntl(p[1], F_SETFD, FD_CLOEXEC);
	return 0;
}

/* Hands the code to the mailer (`<mailer> -t`, message on stdin), or in --dev
   prints it. The address goes no further than the mailer's stdin. */
static int send_code(const char *addr, const char *code) {
	if (dev_mode) { fprintf(stderr, "relay: sign-in code %s\n", code); return 0; }
	if (!mailer[0]) return -1;
	int p[2];
	if (pipe_cx(p)) return -1;
	pid_t pid = fork();
	if (pid == 0) {
		int nul = open("/dev/null", O_WRONLY);
		dup2(p[0], 0);
		if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); }
		close(p[0]); close(p[1]);
		execl(mailer, mailer, "-t", (char *)NULL);
		_exit(127);
	}
	close(p[0]);
	if (pid < 0) { close(p[1]); return -1; }
	FILE *f = fdopen(p[1], "w");
	if (f) {
		fprintf(f, "To: %s\nFrom: hako <noreply@mithraeum.studio>\nSubject: your hako code %s\n\n"
		           "%s\n\nIt works for ten minutes. If you did not ask for it, ignore this.\n", addr, code, code);
		fclose(f);
	} else close(p[1]);
	int st = 0;
	while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static void route_email_start(int fd, Body *b, Dev *d, const char *ip) {
	static pthread_mutex_t ml = PTHREAD_MUTEX_INITIALIZER;
	static time_t win;
	static int sent;
	char line[300], addr[256], code[8], h[65];
	unsigned char r[4], cd[32];
	if (!ip_allow(ip)) { reply(fd, "429 Too Many Requests", "slow down\n"); return; }
	if (body_line(b, line, sizeof line) < 0 || email_norm(addr, sizeof addr, line)) { reply(fd, "400 Bad Request", "address\n"); return; }
	time_t now = time(NULL);
	pthread_mutex_lock(&ml);
	if (now - win >= 60) { win = now; sent = 0; }
	int room = ++sent <= MAILS_PER_MIN;
	pthread_mutex_unlock(&ml);
	if (!room || RAND_bytes(r, 4) != 1) { reply(fd, "429 Too Many Requests", "try again in a minute\n"); return; }
	snprintf(code, sizeof code, "%06u", (unsigned)(((uint32_t)r[0] << 24 | (uint32_t)r[1] << 16 | (uint32_t)r[2] << 8 | r[3]) % 1000000u));
	keyed_hash(h, "email", addr);
	sha256(cd, code, 6, h, 64);
	pthread_mutex_lock(&dlock);
	memcpy(d->mail_hash, h, 65);
	hex_enc(d->mail_code, cd, 32);
	d->mail_exp = now + 600;
	d->mail_tries = 0;
	pthread_mutex_unlock(&dlock);
	int ok = send_code(addr, code) == 0;
	OPENSSL_cleanse(addr, sizeof addr);
	OPENSSL_cleanse(code, sizeof code);
	reply(fd, ok ? "200 OK" : "503 Service Unavailable", ok ? "sent\n" : "email sign-in is off\n");
}

/* Signs d in by h, the keyed hash of an email or a GitHub or Google id, and answers
   "<account> <new|old>": the hash's account, a new one with an invite, or
   (for a device already in an account) the hash added to that account.
   Nonzero when it answered with an error. */
static int login_take(int fd, Dev *d, const char *h, const char *invite) {
	char acct[17], out[32];
	const char *err = NULL;
	int made = 0;
	dev_acct(d, acct);
	pthread_mutex_lock(&slock);
	Login *hit = NULL, *spare = NULL;
	for (int i = 0; i < MAX_LOGIN; i++) {
		if (logins[i].used && !strcmp(logins[i].hash, h)) hit = &logins[i];
		else if (!logins[i].used && !spare) spare = &logins[i];
	}
	if (hit && acct[0] && strcmp(hit->acct, acct)) err = "409 Conflict";
	else if (hit) memcpy(acct, hit->acct, 17);
	else if (!spare) err = "503 Service Unavailable";
	else {
		if (!acct[0]) {
			Acct *a = strcmp(invite, "-") && invite_take(invite) ? acct_new() : NULL;
			if (a) { memcpy(acct, a->id, 17); made = 1; }
			else err = "402 Payment Required";                           /* stands for "invite needed" */
		}
		if (!err) { spare->used = 1; memcpy(spare->acct, acct, 17); memcpy(spare->hash, h, 65); }
	}
	if (!err && (dev_bind_locked(d, acct) || store_save())) err = "503 Service Unavailable";
	pthread_mutex_unlock(&slock);
	pthread_mutex_lock(&dlock);
	if (err && !strncmp(err, "402", 3) && strcmp(d->hold, h)) {        /* route_invite's retries keep their count */
		memcpy(d->hold, h, 65);
		d->hold_exp = time(NULL) + 600;
		d->hold_tries = 0;
	}
	else if (!err) d->hold[0] = '\0';
	pthread_mutex_unlock(&dlock);
	if (err) { reply(fd, err, !strncmp(err, "402", 3) ? "invite needed\n" : "no\n"); return -1; }
	dev_bound(d, acct);
	snprintf(out, sizeof out, "%s %s\n", acct, made ? "new" : "old");
	reply(fd, "200 OK", out);
	return 0;
}

/* <code> <invite|->: the address's account (login_take). */
static void route_email_verify(int fd, Body *b, Dev *d, const char *ip) {
	char line[200], *f[2], h[65];
	unsigned char cd[32];
	if (!ip_allow(ip)) { reply(fd, "429 Too Many Requests", "slow down\n"); return; }
	if (body_line(b, line, sizeof line) < 0 || fields(line, f, 2) != 2 || strlen(f[0]) != 6) { reply(fd, "400 Bad Request", "fields\n"); return; }
	pthread_mutex_lock(&dlock);
	int live = d->mail_code[0] && d->mail_exp >= time(NULL) && d->mail_tries++ < 5;
	char want[65];
	memcpy(h, d->mail_hash, 65);
	memcpy(want, d->mail_code, 65);
	pthread_mutex_unlock(&dlock);
	sha256(cd, f[0], 6, h, 64);
	char got[65];
	hex_enc(got, cd, 32);
	if (!live || CRYPTO_memcmp(got, want, 64)) { reply(fd, "403 Forbidden", "wrong or expired code\n"); return; }
	pthread_mutex_lock(&dlock);
	d->hold[0] = '\0';
	pthread_mutex_unlock(&dlock);
	if (login_take(fd, d, h, f[1])) return;
	pthread_mutex_lock(&dlock);
	d->mail_code[0] = '\0';
	pthread_mutex_unlock(&dlock);
}

/* ---- GitHub and Google sign-in: the relay trades the provider's one-time
   code for the account's id and keeps only its keyed hash, like an email ---- */

#define SSO_CHARS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-"

static int sso_safe(const char *s, size_t max, const char *set) {
	size_t n = strlen(s);
	return n && n <= max && strspn(s, set) == n;
}

/* s as a form value ("" when it would not fit). */
static void form_enc(char *out, size_t cap, const char *s) {
	size_t j = 0;
	for (; *s && j + 4 < cap; s++) {
		if (strchr(SSO_CHARS "~", *s)) out[j++] = *s;
		else j += (size_t)snprintf(out + j, cap - j, "%%%02X", (unsigned char)*s);
	}
	out[*s ? 0 : j] = '\0';
}

/* curl with its whole config on stdin (-K -): the client secret and the token
   never reach argv, where ps shows them. Returns the body's length or -1. */
static long curl_run(const char *config, char *out, size_t cap) {
	int in[2], o[2];
	if (pipe_cx(in)) return -1;
	if (pipe_cx(o)) { close(in[0]); close(in[1]); return -1; }
	pid_t pid = fork();
	if (pid == 0) {
		int nul = open("/dev/null", O_WRONLY);
		dup2(in[0], 0); dup2(o[1], 1);
		if (nul >= 0) dup2(nul, 2);
		execlp("curl", "curl", "-sSf", "--max-time", "10", "--proto", dev_mode ? "=http,https" : "=https", "-K", "-", (char *)NULL);
		_exit(127);
	}
	close(in[0]); close(o[1]);
	long n = 0;
	if (pid >= 0) {
		for (size_t w = 0, len = strlen(config); w < len; ) {               /* a pipe: send() would refuse it */
			ssize_t k = write(in[1], config + w, len - w);
			if (k < 0 && errno == EINTR) continue;
			if (k <= 0) break;
			w += (size_t)k;
		}
		close(in[1]);
		ssize_t r;
		while ((r = read(o[0], out + n, cap - 1 - (size_t)n)) > 0 && (size_t)(n += r) < cap - 1) {}
	} else close(in[1]);
	close(o[0]);
	out[n] = '\0';
	int st = 0;
	while (pid >= 0 && waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
	return pid >= 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0 ? n : -1;
}

/* The provider's id for the account behind a sign-in code: GitHub's numeric
   "id", Google's "sub". The token is wiped once the provider answers. */
static int sso_user(const Sso *s, const char *code, char *uid, size_t cap) {
	char cfg[4096], resp[8192], tok[2048], c[1600];
	const char *p;
	int rc = -1;
	form_enc(c, sizeof c, code);
	snprintf(cfg, sizeof cfg, "url = \"%s\"\nheader = \"Accept: application/json\"\n"
	         "data = \"client_id=%s&client_secret=%s&code=%s%s%s\"\n", s->token, s->id, s->secret, c,
	         s->oidc ? "&grant_type=authorization_code&redirect_uri=" : "", s->oidc ? redirect_enc : "");
	if (curl_run(cfg, resp, sizeof resp) >= 0 && !json_str(resp, "access_token", tok, sizeof tok) && sso_safe(tok, sizeof tok - 1, SSO_CHARS)) {
		snprintf(cfg, sizeof cfg, "url = \"%s\"\nheader = \"Accept: %s\"\n"
		         "header = \"Authorization: Bearer %s\"\nuser-agent = \"hako-relay\"\n",
		         s->user, s->oidc ? "application/json" : "application/vnd.github+json", tok);
		if (curl_run(cfg, resp, sizeof resp) < 0) {}
		else if (s->oidc) rc = !json_str(resp, "sub", uid, cap) && sso_safe(uid, 255, SSO_CHARS) ? 0 : -1;
		else if ((p = strstr(resp, "\"id\""))) {
			p += 4;
			while (*p == ' ') p++;
			if (*p++ == ':') {
				while (*p == ' ') p++;
				size_t n = strspn(p, "0123456789");
				if (n && n < 20 && n < cap) { memcpy(uid, p, n); uid[n] = '\0'; rc = 0; }
			}
		}
	}
	OPENSSL_cleanse(cfg, sizeof cfg);
	OPENSSL_cleanse(tok, sizeof tok);
	OPENSSL_cleanse(resp, sizeof resp);
	return rc;
}

/* Which provider a route "<name>/<what> ..." names, or -1. */
static int sso_at(const char *r, const char *what) {
	size_t w = strlen(what);
	for (int i = 0; i < NSSO; i++) {
		size_t n = strlen(ssos[i].name);
		if (!strncmp(r, ssos[i].name, n) && r[n] == '/' && !strncmp(r + n + 1, what, w) && r[n + 1 + w] == ' ') return i;
	}
	return -1;
}

/* Opens a sign-in for this device and answers the URL to send the browser to.
   GitHub gets no scope (public profile only); Google gets openid alone, the
   least it offers, and its userinfo then holds nothing but the "sub". */
static void route_sso_start(int fd, Dev *d, const char *ip, int i) {
	const Sso *s = &ssos[i];
	char st[33], url[1400], m[64];
	if (!ip_allow(ip)) { reply(fd, "429 Too Many Requests", "slow down\n"); return; }
	if (!s->id[0]) { snprintf(m, sizeof m, "%s sign-in is off\n", s->name); reply(fd, "503 Service Unavailable", m); return; }
	rand_hex(st, 16);
	if (!st[0]) { reply(fd, "503 Service Unavailable", "no\n"); return; }
	pthread_mutex_lock(&dlock);
	memcpy(d->sso_state, st, 33);
	d->sso_p = i;
	d->sso_exp = time(NULL) + 600;
	pthread_mutex_unlock(&dlock);
	if (s->oidc) snprintf(url, sizeof url, "%s?client_id=%s&redirect_uri=%s&response_type=code&scope=openid&state=%s&prompt=select_account\n",
	                      s->auth, s->id, redirect_enc, st);
	else snprintf(url, sizeof url, "%s?client_id=%s&state=%s&allow_signup=true\n", s->auth, s->id, st);
	reply(fd, "200 OK", url);
}

/* <code> <state> <invite|->: the provider sent the browser back with these.
   Google's codes carry a '/'. */
static void route_sso_verify(int fd, Body *b, Dev *d, const char *ip, int i) {
	const Sso *s = &ssos[i];
	char line[800], *f[3], h[65], uid[256], m[64];
	if (!ip_allow(ip)) { reply(fd, "429 Too Many Requests", "slow down\n"); return; }
	if (body_line(b, line, sizeof line) < 0 || fields(line, f, 3) != 3 || !sso_safe(f[0], 512, SSO_CHARS "/") || strlen(f[1]) != 32) {
		reply(fd, "400 Bad Request", "fields\n");
		return;
	}
	pthread_mutex_lock(&dlock);
	int live = s->id[0] && d->sso_state[0] && d->sso_p == i && d->sso_exp >= time(NULL) && !CRYPTO_memcmp(d->sso_state, f[1], 32);
	d->sso_state[0] = '\0';                                             /* one try per start */
	pthread_mutex_unlock(&dlock);
	if (!live) { reply(fd, "403 Forbidden", "start again\n"); return; }
	if (sso_user(s, f[0], uid, sizeof uid)) {
		snprintf(m, sizeof m, "%s did not confirm that sign-in\n", s->name);
		reply(fd, "502 Bad Gateway", m);
		return;
	}
	keyed_hash(h, s->name, uid);
	OPENSSL_cleanse(uid, sizeof uid);
	pthread_mutex_lock(&dlock);
	d->hold[0] = '\0';                                                 /* a fresh sign-in starts a fresh hold */
	pthread_mutex_unlock(&dlock);
	login_take(fd, d, h, f[2]);
}

/* <invite>: finishes a sign-in that stopped at "invite needed" (login_take
   kept its login hash on this device for ten minutes), so GitHub or Google
   need not ask again and an email code need not be typed again. */
static void route_invite(int fd, Body *b, Dev *d, const char *ip) {
	char line[64], h[65];
	if (!ip_allow(ip)) { reply(fd, "429 Too Many Requests", "slow down\n"); return; }
	if (body_line(b, line, sizeof line) <= 0) { reply(fd, "400 Bad Request", "want: <invite>\n"); return; }
	pthread_mutex_lock(&dlock);
	int live = d->hold[0] && d->hold_exp >= time(NULL) && d->hold_tries++ < 5;
	memcpy(h, d->hold, 65);
	pthread_mutex_unlock(&dlock);
	if (!live) { reply(fd, "403 Forbidden", "sign in again\n"); return; }
	login_take(fd, d, h, line);
}

/* "<account> <passkeys holding the account key>": with route_devices, a
   browser without K learns whether anyone could give it one. */
static void route_account(int fd, const char *acct) {
	char out[40];
	int n = 0;
	pthread_mutex_lock(&slock);
	for (int i = 0; i < MAX_CRED; i++)
		if (creds[i].used && !strcmp(creds[i].acct, acct) && creds[i].wrapped[0]) n++;
	pthread_mutex_unlock(&slock);
	snprintf(out, sizeof out, "%s %d\n", acct, n);
	reply(fd, "200 OK", out);
}

static void route_devices(int fd, const char *acct) {
	char pubs[DEV_PER_ACCT][65], names[DEV_PER_ACCT][260];
	int n = 0;
	pthread_mutex_lock(&slock);
	for (int i = 0; i < MAX_BIND && n < DEV_PER_ACCT; i++)
		if (binds[i].used && !strcmp(binds[i].acct, acct)) {
			memcpy(pubs[n], binds[i].pub, 65);
			memcpy(names[n++], binds[i].name, 260);
		}
	pthread_mutex_unlock(&slock);
	char *out = malloc((size_t)n * 400 + 1);
	if (!out) { reply(fd, "503 Service Unavailable", "memory\n"); return; }
	size_t o = 0;
	out[0] = '\0';
	for (int i = 0; i < n; i++) {
		unsigned char pub[32];
		char id[33];
		int online = 0;
		hex_dec(pub, pubs[i], 32);
		device_id(id, pub);
		Dev *x = dev_by_id(id);
		if (x) { pthread_mutex_lock(&x->w); online = x->down >= 0; pthread_mutex_unlock(&x->w); }
		o += (size_t)snprintf(out + o, 400, "%s %s %d %s\n", id, pubs[i], online, names[i][0] ? names[i] : "-");
	}
	reply(fd, "200 OK", out);
	free(out);
}

static void route_bind(int fd, Body *b, const char *acct) {
	char line[100];
	unsigned char pub[32];
	if (body_line(b, line, sizeof line) != 64 || hex_dec(pub, line, 32)) { reply(fd, "400 Bad Request", "want: <public key hex>\n"); return; }
	pthread_mutex_lock(&slock);
	int r = bind_add(acct, line);
	pthread_mutex_unlock(&slock);
	if (r) { reply(fd, "409 Conflict", "no\n"); return; }
	char id[33];
	device_id(id, pub);
	Dev *x = dev_by_id(id);
	if (x) dev_bound(x, acct);
	reply(fd, "200 OK", "ok\n");
}

static void route_name(int fd, Body *b, Dev *d, const char *acct) {
	char line[300], pub[65];
	long n = body_line(b, line, sizeof line);
	if (n <= 0 || n > 256 || !is_b64url(line, (size_t)n)) { reply(fd, "400 Bad Request", "want: <sealed name b64url>\n"); return; }
	hex_enc(pub, d->pub, 32);
	int ok = 0;
	pthread_mutex_lock(&slock);
	for (int i = 0; i < MAX_BIND; i++)
		if (binds[i].used && !strcmp(binds[i].pub, pub) && !strcmp(binds[i].acct, acct)) {
			snprintf(binds[i].name, sizeof binds[i].name, "%.256s", line);
			ok = store_save() == 0;
			break;
		}
	pthread_mutex_unlock(&slock);
	reply(fd, ok ? "200 OK" : "403 Forbidden", ok ? "ok\n" : "no\n");
}

/* Takes a device off the account at once: binding, token and stream. */
static void route_revoke(int fd, Body *b, const char *acct) {
	char line[64];
	if (body_line(b, line, sizeof line) != 32 || !is_hex(line, 32)) { reply(fd, "400 Bad Request", "want: <device id>\n"); return; }
	int ok = 0;
	pthread_mutex_lock(&slock);
	for (int i = 0; i < MAX_BIND && !ok; i++) {
		unsigned char pub[32];
		char id[33];
		if (!binds[i].used || strcmp(binds[i].acct, acct)) continue;
		hex_dec(pub, binds[i].pub, 32);
		device_id(id, pub);
		if (strcmp(id, line)) continue;
		memset(&binds[i], 0, sizeof binds[i]);
		ok = store_save() == 0;
	}
	pthread_mutex_unlock(&slock);
	if (!ok) { reply(fd, "404 Not Found", "no such device\n"); return; }
	Dev *x = dev_by_id(line);
	if (x) {
		pthread_mutex_lock(&dlock);
		x->acct[0] = '\0';
		x->tok_exp = 0;
		pthread_mutex_unlock(&dlock);
		pthread_mutex_lock(&x->w);
		if (x->down >= 0) shutdown(x->down, SHUT_RDWR);
		pthread_mutex_unlock(&x->w);
	}
	reply(fd, "200 OK", "ok\n");
}

/* ---- connections ---- */

static void *conn_thread(void *arg) {
	int fd = (int)(intptr_t)arg;
	char req[8192], ip[46] = "?";
	size_t used = 0;
	struct timeval tv = { 15, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#ifdef SO_NOSIGPIPE
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
	char *end = NULL;
	while (!end && used + 1 < sizeof req) {
		ssize_t r = recv(fd, req + used, sizeof req - used - 1, 0);
		if (r <= 0) goto out;
		used += (size_t)r;
		req[used] = '\0';
		end = strstr(req, "\r\n\r\n");
	}
	if (!end) { reply(fd, "431 Request Header Fields Too Large", "headers\n"); goto out; }
	end[2] = '\0';                                                     /* headers end at the CRLF */
	/* Behind Caddy on loopback, the client is the last X-Forwarded-For hop. */
	const char *xff = header(req, "X-Forwarded-For");
	if (xff) {
		const char *last = strrchr(xff, ','), *s = last ? last + 1 : xff;
		while (*s == ' ') s++;
		size_t l = strcspn(s, "\r\n ");
		if (l < sizeof ip) { memcpy(ip, s, l); ip[l] = '\0'; }
	}

	char bodybuf[16384];
	Body b = { fd, bodybuf, 0, 0, 0, 0, 0 };
	size_t extra = used - (size_t)(end + 4 - req);
	memcpy(bodybuf, end + 4, extra);
	b.have = extra;
	const char *te = header(req, "Transfer-Encoding"), *cl = header(req, "Content-Length");
	b.chunked = te && !strncasecmp(te, "chunked", 7);
	b.remain = b.chunked ? 0 : cl ? atol(cl) : 0;

	if (verbose) fprintf(stderr, "relay: %.*s\n", (int)strcspn(req, "\r\n"), req);
	if (!strncmp(req, "OPTIONS /v1/", 12)) {
		char head[800];
		int n = snprintf(head, sizeof head, "HTTP/1.1 204 No Content\r\n%sAccess-Control-Allow-Methods: GET, POST\r\n"
		                 "Access-Control-Allow-Headers: Authorization, Content-Type\r\nAccess-Control-Max-Age: 600\r\n"
		                 "Content-Length: 0\r\nConnection: close\r\n\r\n", cors);
		send_all(fd, head, (size_t)n);
	}
	else if (!strncmp(req, "GET /v1/health ", 15)) {                  /* the sign-ins that work here */
		char h[64] = "ok";
		if (mailer[0] || dev_mode) strcat(h, " mail");
		for (int i = 0; i < NSSO; i++) if (ssos[i].id[0]) { strcat(h, " "); strcat(h, ssos[i].name); }
		strcat(h, "\n");
		reply(fd, "200 OK", h);
	}
	else if (!strncmp(req, "POST /v1/auth/challenge ", 24)) route_challenge(fd, &b, ip);
	else if (!strncmp(req, "POST /v1/auth/prove ", 20)) route_prove(fd, &b, ip);
	else if (!strncmp(req, "GET /v1/", 8) || !strncmp(req, "POST /v1/", 9)) {
		Dev *d = dev_by_token(req);
		char acct[17] = "";
		const char *r = req + (req[0] == 'G' ? 8 : 9);
		int si;
		if (d) dev_acct(d, acct);
		if (!d) reply(fd, "401 Unauthorized", "sign in\n");
		else if (!strncmp(req, "GET /v1/down ", 13)) route_down(fd, d);
		else if (req[0] == 'G') {
			if (!strncmp(r, "devices ", 8) && acct[0]) route_devices(fd, acct);
			else if (!strncmp(r, "account ", 8) && acct[0]) route_account(fd, acct);
			else reply(fd, acct[0] ? "404 Not Found" : "403 Forbidden", "no\n");
		}
		else if (!strncmp(r, "up ", 3)) route_up(fd, &b, d);
		else if (!strncmp(r, "webauthn/challenge ", 19)) route_wa_challenge(fd, d);
		else if (!strncmp(r, "passkey/register ", 17)) route_pk_register(fd, &b, d, ip);
		else if (!strncmp(r, "passkey/login ", 14)) route_pk_login(fd, &b, d, ip);
		else if (!strncmp(r, "email/start ", 12)) route_email_start(fd, &b, d, ip);
		else if (!strncmp(r, "email/verify ", 13)) route_email_verify(fd, &b, d, ip);
		else if ((si = sso_at(r, "start")) >= 0) route_sso_start(fd, d, ip, si);
		else if ((si = sso_at(r, "verify")) >= 0) route_sso_verify(fd, &b, d, ip, si);
		else if (!strncmp(r, "invite ", 7)) route_invite(fd, &b, d, ip);
		else if (!acct[0]) reply(fd, "403 Forbidden", "sign in to an account\n");
		else if (!strncmp(r, "passkey/key ", 12)) route_pk_key(fd, &b, d);
		else if (!strncmp(r, "bind ", 5)) route_bind(fd, &b, acct);
		else if (!strncmp(r, "name ", 5)) route_name(fd, &b, d, acct);
		else if (!strncmp(r, "revoke ", 7)) route_revoke(fd, &b, acct);
		else reply(fd, "404 Not Found", "no\n");
	} else reply(fd, "404 Not Found", "no\n");
out:
	close(fd);
	pthread_mutex_lock(&clock_);
	conns--;
	pthread_mutex_unlock(&clock_);
	return NULL;
}

/* `relay invite [n]`: n codes on stdout, their hashes appended to <dir>/invites. */
static int cmd_invite(int n) {
	static const char al[] = "abcdefghijkmnpqrstuvwxyz23456789";              /* 32, no l/o/0/1 */
	char path[600];
	snprintf(path, sizeof path, "%s/invites", data_dir);
	int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);
	FILE *f = fd >= 0 ? fdopen(fd, "a") : NULL;
	if (!f) { fprintf(stderr, "relay: cannot write %s\n", path); return 1; }
	for (int i = 0; i < n; i++) {
		unsigned char r[10], d[32];
		char code[12], h[65];
		if (RAND_bytes(r, 10) != 1) { fclose(f); return 1; }
		for (int j = 0; j < 10; j++) code[j] = al[r[j] & 31];
		code[10] = '\0';
		sha256(d, "hako invite ", 12, code, 10);
		hex_enc(h, d, 32);
		fprintf(f, "%s\n", h);
		printf("%.5s-%.5s\n", code, code + 5);
	}
	return fclose(f) == 0 ? 0 : 1;
}

/* <dir>/secret keys the email hashes; made once, 0600. */
static int load_secret(void) {
	char path[600];
	snprintf(path, sizeof path, "%s/secret", data_dir);
	int fd = open(path, O_RDONLY);
	if (fd >= 0) {
		int ok = read(fd, secret, 32) == 32;
		close(fd);
		return ok ? 0 : -1;
	}
	if (RAND_bytes(secret, 32) != 1) return -1;
	fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0) return -1;
	int ok = write(fd, secret, 32) == 32 && fsync(fd) == 0;
	close(fd);
	return ok ? 0 : -1;
}

int main(int argc, char **argv) {
	int port = 8790, invites = -1;
	const char *sso_base = NULL;                                      /* a fake GitHub and Google, for tests */
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-p") && i + 1 < argc) port = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-d") && i + 1 < argc) snprintf(data_dir, sizeof data_dir, "%s", argv[++i]);
		else if (!strcmp(argv[i], "-o") && i + 1 < argc) snprintf(origin, sizeof origin, "%s", argv[++i]);
		else if (!strcmp(argv[i], "-m") && i + 1 < argc) snprintf(mailer, sizeof mailer, "%s", argv[++i]);
		else if (!strcmp(argv[i], "-l") && i + 1 < argc && atoi(argv[i + 1]) > 0) auth_per_min = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-G") && i + 1 < argc) sso_base = argv[++i];
		else if (!strcmp(argv[i], "--dev")) dev_mode = 1;
		else if (!strcmp(argv[i], "-v")) verbose = 1;
		else if (!strcmp(argv[i], "--version")) { printf("relay v%s\n", RELAY_VERSION); return 0; }
		else if (!strcmp(argv[i], "invite") && i == 1) invites = (i + 1 < argc && argv[i + 1][0] != '-') ? atoi(argv[++i]) : 1;
		else {
			fprintf(stderr, "usage: relay [-p port] [-d dir] [-o origin] [-m mailer] [-l sign-ins per IP per minute] [--dev [-G fake github/google]] [-v]   (listens on 127.0.0.1)\n"
			                "       relay invite [n] [-d dir]\n");
			return 2;
		}
	}
	const char *host = strstr(origin, "://");
	host = host ? host + 3 : origin;
	snprintf(rp_id, sizeof rp_id, "%.*s", (int)strcspn(host, ":/"), host);
	snprintf(cors, sizeof cors, "Access-Control-Allow-Origin: %s\r\nVary: Origin\r\n", origin);
	if (mkdir(data_dir, 0700) && errno != EEXIST) { fprintf(stderr, "relay: cannot make %s\n", data_dir); return 1; }
	if (invites >= 0) return cmd_invite(invites > 0 && invites <= 1000 ? invites : 1);
	if (load_secret()) { fprintf(stderr, "relay: cannot read or make %s/secret\n", data_dir); return 1; }
	if (sso_base && !dev_mode) { fprintf(stderr, "relay: -G is for --dev tests\n"); return 2; }
	{
		char red[300];
		snprintf(red, sizeof red, "%s/", origin);
		form_enc(redirect_enc, sizeof redirect_enc, red);
		for (int i = 0; i < NSSO; i++) {
			Sso *s = &ssos[i];
			char path[600], *eps[3] = { s->auth, s->token, s->user };
			for (int e = 0; sso_base && e < 3; e++) {                 /* keep the path, swap the host */
				const char *q = strstr(eps[e], "://");
				char tail[256];
				snprintf(tail, sizeof tail, "%s", q && (q = strchr(q + 3, '/')) ? q : "/");
				snprintf(eps[e], sizeof s->auth, "%.120s%.130s", sso_base, tail);
			}
			snprintf(path, sizeof path, "%s/%s", data_dir, s->name);
			FILE *g = fopen(path, "r");
			if (g && (fscanf(g, "%127s %95s", s->id, s->secret) != 2 || !sso_safe(s->id, 127, SSO_CHARS) || !sso_safe(s->secret, 95, SSO_CHARS))) {
				fprintf(stderr, "relay: %s should be \"<client id> <client secret>\"\n", path);
				s->id[0] = s->secret[0] = '\0';
			}
			if (g) fclose(g);
			if (s->id[0]) { strcat(sso_on, sso_on[0] ? " " : ", sign-in by "); strcat(sso_on, s->name); }
		}
	}
	store_load();
	signal(SIGPIPE, SIG_IGN);
	int srv = socket(AF_INET, SOCK_STREAM, 0), yes = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
	fcntl(srv, F_SETFD, FD_CLOEXEC);                                  /* kept from curl and the mailer */
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(srv, (struct sockaddr *)&sa, sizeof sa) || listen(srv, 64)) {
		fprintf(stderr, "relay: cannot listen on 127.0.0.1:%d: %s\n", port, strerror(errno));
		return 1;
	}
	fprintf(stderr, "relay v%s on 127.0.0.1:%d for %s%s%s\n", RELAY_VERSION, port, origin,
	        sso_on, dev_mode ? " (dev: codes print here)" : "");
	for (;;) {
		int fd = accept(srv, NULL, NULL);
		if (fd < 0) { if (errno == EINTR || errno == ECONNABORTED) continue; sleep(1); continue; }
		fcntl(fd, F_SETFD, FD_CLOEXEC);
		pthread_mutex_lock(&clock_);
		int full = conns >= MAX_CONN;
		if (!full) conns++;
		pthread_mutex_unlock(&clock_);
		if (full) { reply(fd, "503 Service Unavailable", "busy\n"); close(fd); continue; }
		pthread_attr_t at;
		pthread_attr_init(&at);
		pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
		pthread_attr_setstacksize(&at, 256 * 1024);
		pthread_t t;
		if (pthread_create(&t, &at, conn_thread, (void *)(intptr_t)fd) != 0) {
			close(fd);
			pthread_mutex_lock(&clock_);
			conns--;
			pthread_mutex_unlock(&clock_);
		}
		pthread_attr_destroy(&at);
	}
}
