/* SPDX-License-Identifier: GPL-3.0-or-later — Copyright (C) 2026 Zachary Blauser */
/* test.c — the relay's account side against a real ./relay (make test).
 * A software passkey (P-256, the bytes a browser would send), the email code
 * (read from the relay's --dev log), invites, routing kept inside an account,
 * revoke, and the store surviving a restart. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

static int port = 18791, fails, fixed_ip;
static unsigned ipn;
static char dir[256], logp[300];
static pid_t relay_pid;
static const char *relay_bin = "./relay";
static const char *origin = "http://localhost:8791";

static void ck(int ok, const char *what) {
	if (!ok) { fails++; printf("FAIL relay %s\n", what); }
}

static void hex_enc(char *out, const unsigned char *b, size_t n) {
	static const char d[] = "0123456789abcdef";
	for (size_t i = 0; i < n; i++) { out[2 * i] = d[b[i] >> 4]; out[2 * i + 1] = d[b[i] & 15]; }
	out[2 * n] = '\0';
}

static void hex_dec(unsigned char *out, const char *h, size_t n) {
	for (size_t i = 0; i < n; i++) { unsigned v; sscanf(h + 2 * i, "%2x", &v); out[i] = (unsigned char)v; }
}

static void b64url(char *out, const unsigned char *b, size_t n) {
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

static void digest(const EVP_MD *md, unsigned char *out, const void *a, size_t n) {
	unsigned int l;
	EVP_Digest(a, n, out, &l, md, NULL);
}

/* ---- http ---- */

static int dial(void) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa)) { close(fd); return -1; }
	struct timeval tv = { 5, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	return fd;
}

/* One request; the status code, with the body in out. */
static int http(const char *method, const char *path, const char *token, const char *body, char *out, size_t cap) {
	int fd = dial();
	if (fd < 0) return -1;
	char head[512];
	size_t bl = body ? strlen(body) : 0;
	/* as Caddy would forward it: a new client address per call, so the per-IP
	   sign-in limit is tested on its own below */
	unsigned ip = fixed_ip ? 1 : ++ipn;
	int n = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: localhost\r\nX-Forwarded-For: 10.%u.%u.%u\r\n"
	                 "Content-Length: %zu\r\n%s%s%s\r\n", method, path, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255,
	                 bl, token ? "Authorization: Bearer " : "", token ? token : "", token ? "\r\n" : "");
	if (write(fd, head, (size_t)n) != n || (bl && write(fd, body, bl) != (ssize_t)bl)) { close(fd); return -1; }
	static char buf[65536];
	size_t got = 0;
	ssize_t r;
	while (got + 1 < sizeof buf && (r = read(fd, buf + got, sizeof buf - 1 - got)) > 0) got += (size_t)r;
	close(fd);
	buf[got] = '\0';
	int code = 0;
	sscanf(buf, "HTTP/1.1 %d", &code);
	char *b = strstr(buf, "\r\n\r\n");
	if (out) snprintf(out, cap, "%s", b ? b + 4 : "");
	return code;
}

/* ---- a device: X25519 key, signed in by proving it ---- */

typedef struct { unsigned char s[32], pub[32]; char pubh[65], token[65], id[33], acct[17]; } Dev;

static void dev_new(Dev *d) {
	memset(d, 0, sizeof *d);
	RAND_bytes(d->s, 32);
	EVP_PKEY *k = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, d->s, 32);
	size_t n = 32;
	EVP_PKEY_get_raw_public_key(k, d->pub, &n);
	EVP_PKEY_free(k);
	hex_enc(d->pubh, d->pub, 32);
}

static int dev_sign_in(Dev *d) {
	char out[256];
	if (http("POST", "/v1/auth/challenge", NULL, d->pubh, out, sizeof out) != 200) return -1;
	unsigned char cid[16], epub[32], k[32], mac[64];
	hex_dec(cid, out, 16);
	hex_dec(epub, out + 33, 32);
	EVP_PKEY *a = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, NULL, d->s, 32);
	EVP_PKEY *b = EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, NULL, epub, 32);
	EVP_PKEY_CTX *c = EVP_PKEY_CTX_new(a, NULL);
	size_t kl = 32;
	EVP_PKEY_derive_init(c);
	EVP_PKEY_derive_set_peer(c, b);
	EVP_PKEY_derive(c, k, &kl);
	EVP_PKEY_CTX_free(c); EVP_PKEY_free(a); EVP_PKEY_free(b);
	unsigned char in[80];
	memcpy(in, k, 32); memcpy(in + 32, cid, 16); memcpy(in + 48, d->pub, 32);
	digest(EVP_blake2b512(), mac, in, 80);
	char body[128], mh[65];
	hex_enc(mh, mac, 32);
	snprintf(body, sizeof body, "%.32s %s", out, mh);
	if (http("POST", "/v1/auth/prove", NULL, body, out, sizeof out) != 200) return -1;
	char acct[32] = "";
	if (sscanf(out, "%64s %32s %31s", d->token, d->id, acct) != 3) return -1;
	snprintf(d->acct, sizeof d->acct, "%s", strcmp(acct, "-") ? acct : "");
	return 0;
}

/* ---- a software passkey: what navigator.credentials hands the page ---- */

typedef struct { EVP_PKEY *k; unsigned char id[16]; char id64[32], spki64[200]; } Passkey;

static void pk_new(Passkey *p) {
	p->k = EVP_EC_gen("P-256");
	RAND_bytes(p->id, 16);
	b64url(p->id64, p->id, 16);
	unsigned char *der = NULL;
	int n = i2d_PUBKEY(p->k, &der);
	b64url(p->spki64, der, (size_t)n);
	OPENSSL_free(der);
}

static void client_data(char *out64, const char *type, const char *chal, const char *org) {
	char j[400];
	snprintf(j, sizeof j, "{\"type\":\"%s\",\"challenge\":\"%s\",\"origin\":\"%s\",\"crossOrigin\":false}", type, chal, org);
	b64url(out64, (const unsigned char *)j, strlen(j));
}

/* authenticatorData: rpIdHash, flags, count, and for a registration the
   attested credential data (aaguid, id length, id, then key bytes we skip). */
static size_t auth_data(unsigned char *ad, const char *rp, int flags, const Passkey *p) {
	digest(EVP_sha256(), ad, rp, strlen(rp));
	ad[32] = (unsigned char)flags;
	memset(ad + 33, 0, 4);
	if (!(flags & 0x40)) return 37;
	memset(ad + 37, 0, 16);
	ad[53] = 0; ad[54] = 16;
	memcpy(ad + 55, p->id, 16);
	memset(ad + 71, 0xa5, 77);
	return 148;
}

static int wa_chal(Dev *d, char *chal) {
	char out[64];
	if (http("POST", "/v1/webauthn/challenge", d->token, "", out, sizeof out) != 200) return -1;
	out[strcspn(out, "\n")] = '\0';
	strcpy(chal, out);
	return 0;
}

static int pk_register(Dev *d, Passkey *p, const char *invite, const char *org, int flags, char *out, size_t cap) {
	char chal[64], cd[600], ad64[300], body[2000];
	unsigned char ad[200];
	if (wa_chal(d, chal)) return -1;
	client_data(cd, "webauthn.create", chal, org);
	b64url(ad64, ad, auth_data(ad, "localhost", flags, p));
	snprintf(body, sizeof body, "%s %s %s %s %s", invite, p->id64, cd, ad64, p->spki64);
	return http("POST", "/v1/passkey/register", d->token, body, out, cap);
}

static int pk_login(Dev *d, Passkey *p, int bad_sig, char *out, size_t cap) {
	char chal[64], cd[600], ad64[100], sig64[200], body[2000];
	unsigned char ad[64], msg[128], sig[128];
	if (wa_chal(d, chal)) return -1;
	client_data(cd, "webauthn.get", chal, origin);
	size_t adn = auth_data(ad, "localhost", 0x05, p);
	b64url(ad64, ad, adn);
	unsigned char cdj[400];
	char j[400];
	snprintf(j, sizeof j, "{\"type\":\"webauthn.get\",\"challenge\":\"%s\",\"origin\":\"%s\",\"crossOrigin\":false}", chal, origin);
	memcpy(cdj, j, strlen(j));
	memcpy(msg, ad, adn);
	digest(EVP_sha256(), msg + adn, cdj, strlen(j));
	EVP_MD_CTX *x = EVP_MD_CTX_new();
	size_t sl = sizeof sig;
	EVP_DigestSignInit(x, NULL, EVP_sha256(), NULL, p->k);
	EVP_DigestSign(x, sig, &sl, msg, adn + 32);
	EVP_MD_CTX_free(x);
	if (bad_sig) msg[0] ^= 1, sig[sl - 1] ^= 1;
	b64url(sig64, sig, sl);
	snprintf(body, sizeof body, "%s %s %s %s", p->id64, cd, ad64, sig64);
	return http("POST", "/v1/passkey/login", d->token, body, out, cap);
}

/* ---- the relay process ---- */

static void relay_start(void) {
	relay_pid = fork();
	if (relay_pid == 0) {
		FILE *l = freopen(logp, "a", stderr);
		(void)l;
		char p[16];
		snprintf(p, sizeof p, "%d", port);
		execl(relay_bin, relay_bin, "-p", p, "-d", dir, "-o", origin, "--dev", (char *)NULL);
		_exit(127);
	}
	for (int i = 0; i < 50 && http("GET", "/v1/health", NULL, NULL, NULL, 0) != 200; i++) usleep(50000);
}

static void relay_stop(void) {
	kill(relay_pid, SIGTERM);
	waitpid(relay_pid, NULL, 0);
}

/* The newest code the --dev relay printed. */
static void last_code(char out[8]) {
	FILE *f = fopen(logp, "r");
	char line[256];
	out[0] = '\0';
	while (f && fgets(line, sizeof line, f)) {
		const char *c = strstr(line, "sign-in code ");
		if (c) snprintf(out, 8, "%.6s", c + 13);
	}
	if (f) fclose(f);
}

static void invites(char codes[][16], int n) {
	char cmd[400];
	snprintf(cmd, sizeof cmd, "%s invite %d -d %s", relay_bin, n, dir);
	FILE *p = popen(cmd, "r");
	for (int i = 0; i < n && p && fgets(codes[i], 16, p); i++) codes[i][strcspn(codes[i], "\n")] = '\0';
	if (p) pclose(p);
}

/* Sends one frame from a to b over /v1/up; returns what b's stream received
   (or the offline notice a's stream got), within a second. */
static int frame(Dev *a, Dev *b, char *got, size_t cap) {
	int dfd = dial(), afd = dial();
	char req[300];
	snprintf(req, sizeof req, "GET /v1/down HTTP/1.1\r\nAuthorization: Bearer %s\r\n\r\n", b->token);
	if (write(dfd, req, strlen(req)) < 0) return -1;
	snprintf(req, sizeof req, "GET /v1/down HTTP/1.1\r\nAuthorization: Bearer %s\r\n\r\n", a->token);
	if (write(afd, req, strlen(req)) < 0) return -1;
	usleep(150000);
	char body[64];
	snprintf(body, sizeof body, "%s aGVsbG8=\n", b->id);
	int st = http("POST", "/v1/up", a->token, body, NULL, 0);
	usleep(150000);
	struct timeval tv = { 0, 300000 };
	setsockopt(dfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	setsockopt(afd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	char buf[2048];
	ssize_t r = read(dfd, buf, sizeof buf - 1);
	buf[r > 0 ? r : 0] = '\0';
	char *hit = strstr(buf, "aGVsbG8=");
	if (hit) snprintf(got, cap, "delivered");
	else {
		r = read(afd, buf, sizeof buf - 1);
		buf[r > 0 ? r : 0] = '\0';
		snprintf(got, cap, "%s", strstr(buf, "! offline") ? "offline" : "nothing");
	}
	close(dfd);
	close(afd);
	return st;
}

int main(int argc, char **argv) {
	if (argc > 1) relay_bin = argv[1];
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stdout, NULL, _IONBF, 0);
	snprintf(dir, sizeof dir, "/tmp/hako-relay-test.%d", (int)getpid());
	snprintf(logp, sizeof logp, "%s.log", dir);
	char out[4096], codes[3][16];
	invites(codes, 3);
	ck(strlen(codes[0]) == 11 && codes[0][5] == '-', "invite codes print");
	relay_start();
	ck(http("GET", "/v1/health", NULL, NULL, NULL, 0) == 200, "health");

	Dev A, B, C, D, E;
	Passkey P;
	dev_new(&A); dev_new(&B); dev_new(&C); dev_new(&D); dev_new(&E);
	pk_new(&P);
	ck(dev_sign_in(&A) == 0 && !A.acct[0], "a new device signs in, no account");
	ck(http("POST", "/v1/webauthn/challenge", NULL, "", NULL, 0) == 401, "challenge needs a device token");

	/* passkey registration */
	ck(pk_register(&A, &P, "-", origin, 0x45, out, sizeof out) == 403, "a new account needs an invite");
	ck(pk_register(&A, &P, "aaaaa-aaaaa", origin, 0x45, out, sizeof out) == 403, "a made-up invite is refused");
	ck(pk_register(&A, &P, codes[0], "https://evil.example", 0x45, out, sizeof out) == 403, "another origin is refused");
	ck(pk_register(&A, &P, codes[0], origin, 0x41, out, sizeof out) == 403, "no user verification is refused");
	ck(pk_register(&A, &P, codes[0], origin, 0x45, out, sizeof out) == 200 && strlen(out) == 17, "register with an invite");
	char acct1[17];
	snprintf(acct1, sizeof acct1, "%.16s", out);
	Passkey Q;
	pk_new(&Q);
	ck(pk_register(&B, &Q, codes[0], origin, 0x45, out, sizeof out) != 200, "an invite works once");
	ck(dev_sign_in(&A) == 0 && !strcmp(A.acct, acct1), "sign-in now names the account");

	/* passkey login */
	ck(dev_sign_in(&B) == 0, "second device signs in");
	ck(pk_login(&B, &P, 1, out, sizeof out) == 403, "a bad signature is refused");
	ck(pk_login(&B, &P, 0, out, sizeof out) == 200 && !strncmp(out, acct1, 16) && strstr(out, " -"), "passkey login, no key wrapped yet");
	char body[512];
	snprintf(body, sizeof body, "%s %s", P.id64, "d3JhcHBlZC1hY2NvdW50LWtleS1ieXRlcw");
	ck(http("POST", "/v1/passkey/key", B.token, body, out, sizeof out) == 200, "store the wrapped key");
	ck(dev_sign_in(&B) == 0 && pk_login(&B, &P, 0, out, sizeof out) == 200 && strstr(out, "d3JhcHBlZC1hY2NvdW50LWtleS1ieXRlcw"),
	   "login hands back the wrapped key");
	{   /* a challenge is one try */
		char chal[64], cd[600], ad64[100], b2[1200];
		unsigned char ad[64];
		wa_chal(&B, chal);
		client_data(cd, "webauthn.get", chal, origin);
		b64url(ad64, ad, auth_data(ad, "localhost", 0x05, &P));
		snprintf(b2, sizeof b2, "%s %s %s AAAA", P.id64, cd, ad64);
		http("POST", "/v1/passkey/login", B.token, b2, NULL, 0);
		ck(pk_login(&B, &P, 0, out, sizeof out) == 200, "a fresh challenge after a failed one works");
		ck(http("POST", "/v1/passkey/login", B.token, b2, NULL, 0) == 403, "a spent challenge is refused");
	}

	/* email */
	char code[8];
	ck(dev_sign_in(&C) == 0, "third device signs in");
	ck(http("POST", "/v1/email/start", C.token, "Someone@Example.com", NULL, 0) == 200, "email code sent");
	last_code(code);
	ck(strlen(code) == 6, "the dev relay prints the code");
	ck(http("POST", "/v1/email/start", C.token, "a@b.c\r\nBcc: x@y.z", NULL, 0) == 400, "a header in the address is refused");
	snprintf(body, sizeof body, "%06d -", (atoi(code) + 1) % 1000000);
	ck(http("POST", "/v1/email/verify", C.token, body, NULL, 0) == 403, "a wrong code is refused");
	snprintf(body, sizeof body, "%s -", code);
	ck(http("POST", "/v1/email/verify", C.token, body, NULL, 0) == 402, "a new address needs an invite");
	snprintf(body, sizeof body, "%s %s", code, codes[1]);
	ck(http("POST", "/v1/email/verify", C.token, body, out, sizeof out) == 200 && !strncmp(out + 16, " new", 4) && strncmp(out, acct1, 16),
	   "email with an invite makes a second account");
	char acct2[17];
	snprintf(acct2, sizeof acct2, "%.16s", out);
	ck(http("POST", "/v1/email/verify", C.token, body, NULL, 0) == 403, "a code works once");
	ck(dev_sign_in(&D) == 0 && http("POST", "/v1/email/start", D.token, "someone@example.com", NULL, 0) == 200, "same address, new device");
	last_code(code);
	snprintf(body, sizeof body, "%s -", code);
	ck(http("POST", "/v1/email/verify", D.token, body, out, sizeof out) == 200 && !strncmp(out, acct2, 16) && strstr(out, " old"),
	   "the address finds its account");
	ck(http("OPTIONS", "/v1/up", NULL, NULL, out, sizeof out) == 204, "a browser preflight is answered");
	{
		char cmd[400];
		snprintf(cmd, sizeof cmd, "grep -qi 'example.com' %s/store", dir);
		ck(system(cmd) != 0, "the store holds no email address");
	}

	/* routing stays inside an account */
	ck(dev_sign_in(&A) == 0 && dev_sign_in(&B) == 0 && dev_sign_in(&C) == 0 && dev_sign_in(&D) == 0, "re-sign-in");
	char got[32];
	frame(&A, &B, got, sizeof got);
	ck(!strcmp(got, "delivered"), "same account: delivered");
	frame(&A, &C, got, sizeof got);
	ck(!strcmp(got, "offline"), "another account: refused as offline");
	ck(dev_sign_in(&E) == 0, "unbound device signs in");
	frame(&E, &A, got, sizeof got);
	ck(!strcmp(got, "offline"), "a device with no account reaches nobody");
	ck(http("GET", "/v1/devices", E.token, NULL, NULL, 0) == 403, "devices needs an account");

	/* devices, bind, revoke */
	ck(http("GET", "/v1/devices", A.token, NULL, out, sizeof out) == 200 && strstr(out, A.id) && strstr(out, B.id) && !strstr(out, C.id),
	   "devices lists this account's devices only");
	ck(http("POST", "/v1/bind", A.token, E.pubh, NULL, 0) == 200, "bind a computer's key");
	ck(http("POST", "/v1/bind", C.token, E.pubh, NULL, 0) == 409, "another account cannot take it");
	frame(&E, &A, got, sizeof got);
	ck(!strcmp(got, "delivered"), "the bound device now reaches the account");
	ck(http("POST", "/v1/name", E.token, "c2VhbGVkLW5hbWU", NULL, 0) == 200, "set a sealed name");
	ck(http("GET", "/v1/devices", A.token, NULL, out, sizeof out) == 200 && strstr(out, "c2VhbGVkLW5hbWU"), "the sealed name lists");
	ck(http("POST", "/v1/revoke", C.token, B.id, NULL, 0) == 404, "another account cannot revoke");
	ck(http("POST", "/v1/revoke", A.token, B.id, NULL, 0) == 200, "revoke");
	ck(http("GET", "/v1/devices", B.token, NULL, NULL, 0) == 401, "the revoked token is dead");
	ck(dev_sign_in(&B) == 0 && !B.acct[0], "and the device has no account");

	/* the per-IP sign-in limit */
	{
		int limited = 0;
		fixed_ip = 1;
		for (int i = 0; i < 35; i++) limited |= http("POST", "/v1/auth/challenge", NULL, A.pubh, NULL, 0) == 429;
		fixed_ip = 0;
		ck(limited, "one address is limited to 30 sign-ins a minute");
	}

	/* the store survives a restart */
	relay_stop();
	relay_start();
	ck(dev_sign_in(&A) == 0 && !strcmp(A.acct, acct1), "after a restart: the account");
	ck(dev_sign_in(&E) == 0 && !strcmp(E.acct, acct1), "after a restart: the bound computer");
	ck(dev_sign_in(&B) == 0 && pk_login(&B, &P, 0, out, sizeof out) == 200, "after a restart: passkey login");
	relay_stop();

	char cmd[700];
	snprintf(cmd, sizeof cmd, "rm -rf %s %s", dir, logp);
	if (system(cmd)) {}
	printf(fails ? "%d relay account tests failed\n" : "relay account tests passed\n", fails);
	return fails ? 1 : 0;
}
