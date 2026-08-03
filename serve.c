/* hako-web serve — transport bridge, v0.
 *
 * Spawns `hako --pipe` and bridges its JSONL protocol to a browser over plain
 * HTTP + Server-Sent Events. No agent logic lives here and none ever will:
 * every message is forwarded verbatim in one direction or the other.
 *
 * This is scaffolding. It folds into hako.c as `hako --serve` once the platform
 * seam lands; keeping it standalone tonight avoids touching a working binary.
 *
 *   cc -O2 -Wall -Wextra -o hako-serve serve.c -lpthread
 *   ./hako-serve --dir ~/code/project
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HW_VERSION      "0.0.1"
#define HW_RING         4096          /* events retained for late joiners */
#define HW_LINE_MAX     (1024 * 256)  /* one JSONL line from the child */

static struct {
	int         port;
	const char *bind_addr;
	const char *web_dir;
	const char *work_dir;
	const char *hako_bin;
	int         yolo;
} O = { 8787, "127.0.0.1", ".", NULL, NULL, 0 };

/* ---- child ---- */
static int   g_agent_in   = -1;
static int   g_agent_out = -1;
static pid_t g_agent_pid  = -1;
static int   g_agent_dead = 0;
static pthread_mutex_t g_write_lock = PTHREAD_MUTEX_INITIALIZER;

/* id of the tool_request the child is currently blocked on, 0 when none. Lets
   /api/approve record a decision the front end can replay as already-settled. */
static long g_pending_tool_id = 0;

/* Set while we are deliberately killing the child, so its reader thread doesn't
   report the exit as a crash. */
static volatile int g_restarting = 0;
static pthread_t    g_reader_tid;
static int          g_reader_live = 0;

/* ---- event ring ---- */
static char           *g_ring[HW_RING];
static long            g_ring_next  = 0;     /* id of the next event written */
static long            g_ring_floor = 0;     /* replay starts here; /api/reset raises it */
static pthread_mutex_t g_ring_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_ring_cond = PTHREAD_COND_INITIALIZER;

/* Drop replay history without disturbing live cursors: ids stay monotonic, the
   floor just moves up. A reload after this replays nothing stale. */
static void hwRingReset(void) {
	pthread_mutex_lock(&g_ring_lock);
	for (int i = 0; i < HW_RING; i++) { free(g_ring[i]); g_ring[i] = NULL; }
	g_ring_floor = g_ring_next;
	pthread_cond_broadcast(&g_ring_cond);
	pthread_mutex_unlock(&g_ring_lock);
}

static void hwRingPush(const char *json) {
	pthread_mutex_lock(&g_ring_lock);
	int slot = (int)(g_ring_next % HW_RING);
	free(g_ring[slot]);
	g_ring[slot] = strdup(json);
	g_ring_next++;
	pthread_cond_broadcast(&g_ring_cond);
	pthread_mutex_unlock(&g_ring_lock);
}

/* ---- helpers ---- */

/* Minimal JSON string escape. Control chars below 0x20 become \uXXXX. */
static void hwJsonEscape(const char *s, char *out, size_t cap) {
	size_t o = 0;
	for (; s && *s && o + 8 < cap; s++) {
		unsigned char c = (unsigned char)*s;
		switch (c) {
			case '"':  out[o++]='\\'; out[o++]='"';  break;
			case '\\': out[o++]='\\'; out[o++]='\\'; break;
			case '\n': out[o++]='\\'; out[o++]='n';  break;
			case '\r': out[o++]='\\'; out[o++]='r';  break;
			case '\t': out[o++]='\\'; out[o++]='t';  break;
			default:
				if (c < 0x20) o += (size_t)snprintf(out+o, cap-o, "\\u%04x", c);
				else          out[o++] = (char)c;
		}
	}
	out[o] = '\0';
}

static int hwWriteAll(int fd, const char *buf, size_t len) {
	while (len > 0) {
		ssize_t n = write(fd, buf, len);
		if (n < 0) { if (errno == EINTR) continue; return -1; }
		buf += n; len -= (size_t)n;
	}
	return 0;
}

/* Send one JSONL line to the child's stdin. */
static int hwSendToAgent(const char *json) {
	if (g_agent_dead || g_agent_in < 0) return -1;
	pthread_mutex_lock(&g_write_lock);
	int rc = hwWriteAll(g_agent_in, json, strlen(json));
	if (rc == 0) rc = hwWriteAll(g_agent_in, "\n", 1);
	pthread_mutex_unlock(&g_write_lock);
	return rc;
}

/* ---- child reader: JSONL out -> ring ---- */
static void *hwReaderThread(void *arg) {
	(void)arg;
	char *line = malloc(HW_LINE_MAX);
	size_t len = 0;
	if (!line) return NULL;

	for (;;) {
		char c;
		ssize_t n = read(g_agent_out, &c, 1);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) continue;
			break;
		}
		if (c == '\n') {
			line[len] = '\0';
			if (len > 0) {
				const char *tr = strstr(line, "\"type\":\"tool_request\"");
				if (tr) {
					const char *idp = strstr(line, "\"id\":");
					g_pending_tool_id = idp ? atol(idp + 5) : 0;
				}
				hwRingPush(line);
			}
			len = 0;
		} else if (len + 1 < HW_LINE_MAX) {
			line[len++] = c;
		}
	}

	free(line);
	g_agent_dead = 1;
	if (!g_restarting)
		hwRingPush("{\"type\":\"error\",\"text\":\"agent process exited\"}");
	return NULL;
}

static int hwStartAgent(void) {
	int in_pipe[2], out_pipe[2];

	/* Resolve a relative --hako here, in the parent: the child chdir()s to
	   --dir before exec, which would otherwise break the path out from under it.
	   Resolve ONCE — on a restart O.hako_bin already points at hako_abs, and
	   realpath() with the same buffer as source and destination corrupts it
	   (it resolved to "/" and every restart died with EACCES). */
	static char hako_abs[4096];
	static char web_abs[4096];
	static int  paths_resolved = 0;
	if (!paths_resolved) {
		paths_resolved = 1;
		if (O.hako_bin && strchr(O.hako_bin, '/')) {
			if (!realpath(O.hako_bin, hako_abs)) {
				fprintf(stderr, "hako-serve: cannot resolve --hako %s: %s\n",
				        O.hako_bin, strerror(errno));
				return -1;
			}
			O.hako_bin = hako_abs;
		}
		if (O.web_dir && realpath(O.web_dir, web_abs)) O.web_dir = web_abs;
	}

	if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) return -1;

	pid_t pid = fork();
	if (pid < 0) return -1;

	if (pid == 0) {
		/* child */
		dup2(in_pipe[0],  STDIN_FILENO);
		dup2(out_pipe[1], STDOUT_FILENO);
		close(in_pipe[0]);  close(in_pipe[1]);
		close(out_pipe[0]); close(out_pipe[1]);
		if (O.work_dir && chdir(O.work_dir) != 0) {
			fprintf(stderr, "hako-serve: chdir(%s) failed: %s\n", O.work_dir, strerror(errno));
			_exit(127);
		}
		const char *bin = O.hako_bin ? O.hako_bin : "hako";
		if (O.yolo) execlp(bin, bin, "--pipe", "--yolo", (char *)NULL);
		else        execlp(bin, bin, "--pipe", (char *)NULL);
		fprintf(stderr, "hako-serve: exec %s failed: %s\n", bin, strerror(errno));
		_exit(127);
	}

	close(in_pipe[0]);
	close(out_pipe[1]);
	g_agent_in   = in_pipe[1];
	g_agent_out = out_pipe[0];
	g_agent_pid  = pid;

	/* Joinable on purpose: a restart must wait for this thread to finish before
	   closing fds, or it stays blocked on a stale descriptor that the next
	   pipe() promptly reuses — and then it eats the new child's output. */
	if (pthread_create(&g_reader_tid, NULL, hwReaderThread, NULL) != 0) return -1;
	g_reader_live = 1;
	return 0;
}

static void hwSendSimple(int fd, const char *status, const char *ctype, const char *body);

static void hwStopAgent(void) {
	/* Close the write end first: that alone makes hako see EOF on stdin and
	   quit, which gives the reader its EOF too. */
	if (g_agent_in >= 0) { close(g_agent_in); g_agent_in = -1; }

	if (g_agent_pid > 0) {
		kill(g_agent_pid, SIGTERM);
		waitpid(g_agent_pid, NULL, 0);
		g_agent_pid = -1;
	}
	/* Join before closing the read fd — see the note in hwStartAgent. */
	if (g_reader_live) {
		pthread_join(g_reader_tid, NULL);
		g_reader_live = 0;
	}
	if (g_agent_out >= 0) { close(g_agent_out); g_agent_out = -1; }
	g_agent_dead = 0;
	g_pending_tool_id = 0;
}

/* Restart the agent. Also the escape hatch for a child wedged in an
   interactive read (`:login`, bare `:provider`) — those block on stdin and
   nothing else can free them. */
static int hwRestartAgent(const char *new_dir) {
	g_restarting = 1;
	hwStopAgent();
	if (new_dir && *new_dir) {
		static char dir_buf[4096];
		snprintf(dir_buf, sizeof(dir_buf), "%s", new_dir);
		O.work_dir = dir_buf;
	}
	hwRingReset();     /* hwStopAgent already joined the reader, so nothing races this */
	int rc = hwStartAgent();
	g_restarting = 0;
	return rc;
}

/* ---- JSON emitters for the sidebar ---- */

static void hwAppendEscaped(char **buf, size_t *len, size_t *cap, const char *s) {
	char esc[8192];
	hwJsonEscape(s, esc, sizeof(esc));
	size_t need = strlen(esc) + 1;
	if (*len + need >= *cap) {
		size_t ncap = (*cap ? *cap : 1024);
		while (ncap < *len + need) ncap *= 2;
		char *nb = realloc(*buf, ncap);
		if (!nb) return;
		*buf = nb; *cap = ncap;
	}
	memcpy(*buf + *len, esc, strlen(esc));
	*len += strlen(esc);
	(*buf)[*len] = '\0';
}

static void hwAppendRaw(char **buf, size_t *len, size_t *cap, const char *s) {
	size_t need = strlen(s) + 1;
	if (*len + need >= *cap) {
		size_t ncap = (*cap ? *cap : 1024);
		while (ncap < *len + need) ncap *= 2;
		char *nb = realloc(*buf, ncap);
		if (!nb) return;
		*buf = nb; *cap = ncap;
	}
	memcpy(*buf + *len, s, strlen(s));
	*len += strlen(s);
	(*buf)[*len] = '\0';
}

/* hako encodes a project dir as a dash-flattened path: /a/b -> -a-b */
static void hwProjectKey(const char *dir, char *out, size_t cap) {
	char real[4096];
	if (!realpath(dir, real)) snprintf(real, sizeof(real), "%s", dir);
	size_t o = 0;
	for (size_t i = 0; real[i] && o + 2 < cap; i++)
		out[o++] = (real[i] == '/') ? '-' : real[i];
	out[o] = '\0';
}

/* A session's display name: a user-set title from the `<sid>.title` sidecar if
   there is one, else the first user line of the transcript. The sidecar keeps
   hako's jsonl untouched — the CLI reads the same file. */
static void hwSessionTitle(const char *path, char *out, size_t cap) {
	out[0] = '\0';

	char side[9100];
	snprintf(side, sizeof(side), "%.*s.title", (int)(strlen(path) - 6), path);  /* strip .jsonl */
	FILE *sf = fopen(side, "r");
	if (sf) {
		if (fgets(out, (int)cap, sf)) {
			size_t l = strlen(out);
			while (l && (out[l-1] == '\n' || out[l-1] == '\r')) out[--l] = '\0';
		}
		fclose(sf);
		if (out[0]) return;
	}

	FILE *f = fopen(path, "r");
	if (!f) return;
	char line[8192];
	while (fgets(line, sizeof(line), f)) {
		if (!strstr(line, "\"role\":\"user\"")) continue;
		const char *c = strstr(line, "\"content\":\"");
		if (!c) continue;
		c += 11;
		size_t o = 0;
		while (*c && *c != '"' && o + 1 < cap) {
			if (*c == '\\' && c[1]) { c++; if (*c == 'n') { out[o++] = ' '; c++; continue; } }
			out[o++] = *c++;
		}
		out[o] = '\0';
		break;
	}
	fclose(f);
}

static void hwServeSessions(int fd) {
	const char *home = getenv("HOME"); if (!home) home = ".";
	char key[4096], sdir[4600];
	hwProjectKey(O.work_dir ? O.work_dir : ".", key, sizeof(key));
	snprintf(sdir, sizeof(sdir), "%s/.hako/projects/%s/sessions", home, key);

	char *buf = NULL; size_t len = 0, cap = 0;
	hwAppendRaw(&buf, &len, &cap, "{\"dir\":\"");
	hwAppendEscaped(&buf, &len, &cap, O.work_dir ? O.work_dir : ".");
	hwAppendRaw(&buf, &len, &cap, "\",\"sessions\":[");

	DIR *d = opendir(sdir);
	int first = 1;
	if (d) {
		struct dirent *de;
		while ((de = readdir(d))) {
			const char *dot = strrchr(de->d_name, '.');
			if (!dot || strcmp(dot, ".jsonl") != 0) continue;
			char full[9400];
			snprintf(full, sizeof(full), "%s/%s", sdir, de->d_name);
			struct stat st;
			if (stat(full, &st) != 0) continue;

			char id[256];
			snprintf(id, sizeof(id), "%.*s", (int)(dot - de->d_name), de->d_name);
			char title[200];
			hwSessionTitle(full, title, sizeof(title));

			char head[128];
			snprintf(head, sizeof(head), "%s{\"id\":\"", first ? "" : ",");
			hwAppendRaw(&buf, &len, &cap, head);
			hwAppendEscaped(&buf, &len, &cap, id);
			hwAppendRaw(&buf, &len, &cap, "\",\"title\":\"");
			hwAppendEscaped(&buf, &len, &cap, title);
			char tail[96];
			snprintf(tail, sizeof(tail), "\",\"mtime\":%ld,\"bytes\":%ld}",
			         (long)st.st_mtime, (long)st.st_size);
			hwAppendRaw(&buf, &len, &cap, tail);
			first = 0;
		}
		closedir(d);
	}
	hwAppendRaw(&buf, &len, &cap, "]}");
	hwSendSimple(fd, "200 OK", "application/json", buf ? buf : "{\"sessions\":[]}");
	free(buf);
}

/* Percent-decode one query parameter out of the request line. */
static void hwQueryParam(const char *req, const char *key, char *out, size_t cap) {
	out[0] = '\0';
	char pat[64];
	snprintf(pat, sizeof(pat), "%s=", key);
	const char *q = strstr(req, pat);
	if (!q) return;
	q += strlen(pat);
	size_t o = 0;
	while (*q && *q != ' ' && *q != '&' && o + 1 < cap) {
		if (*q == '%' && q[1] && q[2]) {
			char hex[3] = { q[1], q[2], 0 };
			out[o++] = (char)strtol(hex, NULL, 16);
			q += 3;
		} else if (*q == '+') { out[o++] = ' '; q++; }
		else out[o++] = *q++;
	}
	out[o] = '\0';
}

/* Weights hako knows how to fetch. Mirrors the tiers in hako.c. */
static const char *HW_KNOWN_MODELS[] = { "hako-sho", "hako-koi", NULL };

/* Installed weights, plus any *.mlf2 found in user-configured search paths.
   Nothing is copied or moved here — discovery only. */
static void hwServeModels(int fd, const char *search_csv) {
	const char *home = getenv("HOME"); if (!home) home = ".";
	char *buf = NULL; size_t len = 0, cap = 0;
	hwAppendRaw(&buf, &len, &cap, "{\"installed\":[");

	char mroot[2048];
	snprintf(mroot, sizeof(mroot), "%s/.hako/models", home);
	DIR *d = opendir(mroot);
	int first = 1;
	if (d) {
		struct dirent *de;
		while ((de = readdir(d))) {
			if (de->d_name[0] == '.') continue;
			char w[4300];
			struct stat st;
			snprintf(w, sizeof(w), "%s/%s/%s.mlf2", mroot, de->d_name, de->d_name);
			if (stat(w, &st) != 0) continue;
			char row[256];
			snprintf(row, sizeof(row), "%s{\"id\":\"", first ? "" : ",");
			hwAppendRaw(&buf, &len, &cap, row);
			hwAppendEscaped(&buf, &len, &cap, de->d_name);
			snprintf(row, sizeof(row), "\",\"bytes\":%lld}", (long long)st.st_size);
			hwAppendRaw(&buf, &len, &cap, row);
			first = 0;
		}
		closedir(d);
	}
	hwAppendRaw(&buf, &len, &cap, "],\"found\":[");

	/* Scan the user's configured folders (and ~/Downloads, which hako itself
	   already auto-links from) for loose weight files. */
	char paths[4096];
	snprintf(paths, sizeof(paths), "%s/Downloads%s%s", home,
	         (search_csv && *search_csv) ? "," : "", (search_csv && *search_csv) ? search_csv : "");
	first = 1;
	char *save = NULL;
	for (char *p = strtok_r(paths, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
		while (*p == ' ') p++;
		if (!*p) continue;
		DIR *sd = opendir(p);
		if (!sd) continue;
		struct dirent *de;
		while ((de = readdir(sd))) {
			const char *dot = strrchr(de->d_name, '.');
			if (!dot || strcmp(dot, ".mlf2") != 0) continue;
			char full[9000];
			snprintf(full, sizeof(full), "%s/%s", p, de->d_name);
			struct stat st;
			if (stat(full, &st) != 0) continue;
			char id[256];
			snprintf(id, sizeof(id), "%.*s", (int)(dot - de->d_name), de->d_name);
			hwAppendRaw(&buf, &len, &cap, first ? "{\"id\":\"" : ",{\"id\":\"");
			hwAppendEscaped(&buf, &len, &cap, id);
			hwAppendRaw(&buf, &len, &cap, "\",\"path\":\"");
			hwAppendEscaped(&buf, &len, &cap, full);
			char tail[64];
			snprintf(tail, sizeof(tail), "\",\"bytes\":%lld}", (long long)st.st_size);
			hwAppendRaw(&buf, &len, &cap, tail);
			first = 0;
		}
		closedir(sd);
	}

	hwAppendRaw(&buf, &len, &cap, "],\"known\":[");
	for (int i = 0; HW_KNOWN_MODELS[i]; i++) {
		hwAppendRaw(&buf, &len, &cap, i ? ",\"" : "\"");
		hwAppendEscaped(&buf, &len, &cap, HW_KNOWN_MODELS[i]);
		hwAppendRaw(&buf, &len, &cap, "\"");
	}
	hwAppendRaw(&buf, &len, &cap, "]}");
	hwSendSimple(fd, "200 OK", "application/json", buf ? buf : "{}");
	free(buf);
}

/* Symlink a weight found elsewhere into ~/.hako/models/<id>/<id>.mlf2 — the
   same trick hkMithraeumRelocate uses. Never copies, never deletes. */
static int hwLinkModel(const char *path) {
	const char *home = getenv("HOME"); if (!home) home = ".";
	const char *slash = strrchr(path, '/');
	const char *base  = slash ? slash + 1 : path;
	const char *dot   = strrchr(base, '.');
	if (!dot || strcmp(dot, ".mlf2") != 0) return -1;

	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;

	char id[256];
	snprintf(id, sizeof(id), "%.*s", (int)(dot - base), base);

	char dir[2600], canon[3200];
	snprintf(dir, sizeof(dir), "%s/.hako/models", home);
	mkdir(dir, 0755);
	snprintf(dir, sizeof(dir), "%s/.hako/models/%s", home, id);
	mkdir(dir, 0755);
	snprintf(canon, sizeof(canon), "%s/%s.mlf2", dir, id);
	if (stat(canon, &st) == 0) return 0;             /* already usable */
	return symlink(path, canon) == 0 ? 0 : -1;
}

/* Directory listing for the folder picker. Directories only. */
static void hwServeDirs(int fd, const char *path) {
	char real[4096];
	if (!realpath(path && *path ? path : ".", real)) snprintf(real, sizeof(real), "%s", path);

	char *buf = NULL; size_t len = 0, cap = 0;
	hwAppendRaw(&buf, &len, &cap, "{\"path\":\"");
	hwAppendEscaped(&buf, &len, &cap, real);
	hwAppendRaw(&buf, &len, &cap, "\",\"dirs\":[");

	DIR *d = opendir(real);
	int first = 1;
	if (d) {
		struct dirent *de;
		while ((de = readdir(d))) {
			if (de->d_name[0] == '.') continue;
			char full[9400];
			snprintf(full, sizeof(full), "%s/%s", real, de->d_name);
			struct stat st;
			if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
			hwAppendRaw(&buf, &len, &cap, first ? "\"" : ",\"");
			hwAppendEscaped(&buf, &len, &cap, de->d_name);
			hwAppendRaw(&buf, &len, &cap, "\"");
			first = 0;
		}
		closedir(d);
	}
	hwAppendRaw(&buf, &len, &cap, "]}");
	hwSendSimple(fd, "200 OK", "application/json", buf ? buf : "{\"dirs\":[]}");
	free(buf);
}

/* ---- HTTP ---- */

static void hwSendSimple(int fd, const char *status, const char *ctype, const char *body) {
	char head[512];
	int n = snprintf(head, sizeof(head),
		"HTTP/1.1 %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %zu\r\n"
		"Cache-Control: no-store\r\n"
		"Connection: close\r\n"
		"\r\n", status, ctype, strlen(body));
	hwWriteAll(fd, head, (size_t)n);
	hwWriteAll(fd, body, strlen(body));
}

static void hwSendFile(int fd, const char *path, const char *ctype) {
	FILE *f = fopen(path, "rb");
	if (!f) { hwSendSimple(fd, "404 Not Found", "text/plain", "not found\n"); return; }
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = malloc((size_t)sz + 1);
	if (!buf) { fclose(f); hwSendSimple(fd, "500 Internal Server Error", "text/plain", "oom\n"); return; }
	size_t got = fread(buf, 1, (size_t)sz, f);
	fclose(f);
	buf[got] = '\0';

	char head[512];
	int n = snprintf(head, sizeof(head),
		"HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
		"Cache-Control: no-store\r\nConnection: close\r\n\r\n", ctype, got);
	hwWriteAll(fd, head, (size_t)n);
	hwWriteAll(fd, buf, got);
	free(buf);
}

/* Stream the ring as SSE until the client goes away. */
static void hwServeEvents(int fd) {
	const char *head =
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/event-stream\r\n"
		"Cache-Control: no-store\r\n"
		"Connection: keep-alive\r\n"
		"\r\n";
	if (hwWriteAll(fd, head, strlen(head)) != 0) return;

	pthread_mutex_lock(&g_ring_lock);
	long cursor = g_ring_next > HW_RING ? g_ring_next - HW_RING : 0;
	if (cursor < g_ring_floor) cursor = g_ring_floor;
	pthread_mutex_unlock(&g_ring_lock);

	for (;;) {
		char *payload = NULL;

		pthread_mutex_lock(&g_ring_lock);
		/* Clamp before AND after waiting: /api/reset can raise the floor past
		   our cursor while we sleep, and skipping the re-clamp would leave the
		   cursor one ahead of the ring and eat the next real event. */
		if (cursor < g_ring_floor) cursor = g_ring_floor;
		while (cursor >= g_ring_next) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += 15;
			if (pthread_cond_timedwait(&g_ring_cond, &g_ring_lock, &ts) == ETIMEDOUT) {
				pthread_mutex_unlock(&g_ring_lock);
				if (hwWriteAll(fd, ":ka\n\n", 5) != 0) return;   /* keepalive */
				pthread_mutex_lock(&g_ring_lock);
			}
			if (cursor < g_ring_floor) cursor = g_ring_floor;
		}
		if (cursor < g_ring_next - HW_RING) cursor = g_ring_next - HW_RING;
		char *line = g_ring[(int)(cursor % HW_RING)];
		if (line) payload = strdup(line);
		cursor++;
		pthread_mutex_unlock(&g_ring_lock);

		if (payload) {
			char frame[HW_LINE_MAX + 32];
			int n = snprintf(frame, sizeof(frame), "data: %s\n\n", payload);
			free(payload);
			if (hwWriteAll(fd, frame, (size_t)n) != 0) return;
		}
	}
}

/* Read a request body, continuing past what the header read already consumed.
   `fetch` routinely sends headers and body in separate segments, so anything
   that reads the body straight out of the header buffer gets an empty string
   from a browser while working perfectly under curl. Caller frees. */
static char *hwReadBody(int fd, char *req, size_t used) {
	long want = 0;
	const char *cl = strcasestr(req, "\r\nContent-Length:");
	if (cl) want = strtol(cl + 17, NULL, 10);
	if (want < 0 || want > HW_LINE_MAX) want = 0;

	char *payload = malloc((size_t)want + 1);
	if (!payload) return NULL;

	size_t have = 0;
	char *body = strstr(req, "\r\n\r\n");
	if (body) {
		body += 4;
		have = used - (size_t)(body - req);
		if (have > (size_t)want) have = (size_t)want;
		memcpy(payload, body, have);
	}
	while (have < (size_t)want) {
		ssize_t n = read(fd, payload + have, (size_t)want - have);
		if (n <= 0) break;
		have += (size_t)n;
	}
	payload[have] = '\0';
	return payload;
}

/* Replay a stored session transcript so a :resume shows what was said. */
static void hwServeHistory(int fd, const char *sid) {
	const char *home = getenv("HOME"); if (!home) home = ".";
	char key[4096], path[9000];
	hwProjectKey(O.work_dir ? O.work_dir : ".", key, sizeof(key));
	snprintf(path, sizeof(path), "%s/.hako/projects/%s/sessions/%s.jsonl", home, key, sid);

	char *buf = NULL; size_t len = 0, cap = 0;
	hwAppendRaw(&buf, &len, &cap, "{\"messages\":[");
	FILE *f = fopen(path, "r");
	int first = 1;
	if (f) {
		char *line = malloc(HW_LINE_MAX);
		if (line) {
			while (fgets(line, HW_LINE_MAX, f)) {
				if (!strstr(line, "\"role\"")) continue;
				/* Pass each stored record through untouched — it is already the
				   JSON the front end wants, minus the trailing newline. */
				size_t l = strlen(line);
				while (l && (line[l-1] == '\n' || line[l-1] == '\r')) line[--l] = '\0';
				if (!l) continue;
				hwAppendRaw(&buf, &len, &cap, first ? "" : ",");
				hwAppendRaw(&buf, &len, &cap, line);
				first = 0;
			}
			free(line);
		}
		fclose(f);
	}
	hwAppendRaw(&buf, &len, &cap, "]}");
	hwSendSimple(fd, "200 OK", "application/json", buf ? buf : "{\"messages\":[]}");
	free(buf);
}

/* Delete one stored session transcript from the current project. The id is
   validated by the caller; this only ever unlinks inside the project's own
   sessions dir. */
static int hwDeleteSession(const char *sid) {
	const char *home = getenv("HOME"); if (!home) home = ".";
	char key[4096], path[9000];
	hwProjectKey(O.work_dir ? O.work_dir : ".", key, sizeof(key));
	snprintf(path, sizeof(path), "%s/.hako/projects/%s/sessions/%s.title", home, key, sid);
	unlink(path);                         /* sidecar may not exist — fine */
	snprintf(path, sizeof(path), "%s/.hako/projects/%s/sessions/%s.jsonl", home, key, sid);
	return unlink(path) == 0 ? 0 : -1;
}

/* Set or clear a session's display name. An empty title removes the sidecar,
   restoring the first-message fallback. */
static int hwRenameSession(const char *sid, const char *title) {
	const char *home = getenv("HOME"); if (!home) home = ".";
	char key[4096], path[9000];
	hwProjectKey(O.work_dir ? O.work_dir : ".", key, sizeof(key));
	snprintf(path, sizeof(path), "%s/.hako/projects/%s/sessions/%s.title", home, key, sid);

	if (!title || !*title) { unlink(path); return 0; }

	FILE *f = fopen(path, "w");
	if (!f) return -1;
	for (int i = 0; title[i] && i < 200; i++)
		fputc((unsigned char)title[i] < 0x20 ? ' ' : title[i], f);   /* one line, no controls */
	fputc('\n', f);
	fclose(f);
	return 0;
}

/* Reject cross-origin posts. Browsers always send Origin on POST. */
static int hwOriginOk(const char *req) {
	const char *o = strcasestr(req, "\r\nOrigin:");
	if (!o) return 1;                     /* curl and friends */
	o += 9;
	while (*o == ' ') o++;
	return strncmp(o, "http://127.0.0.1", 16) == 0
	    || strncmp(o, "http://localhost", 16) == 0
	    || strncmp(o, "http://[::1]",     12) == 0;
}

static void *hwConnThread(void *arg) {
	int fd = (int)(long)arg;
	char req[8192];
	size_t used = 0;

	/* read headers */
	while (used + 1 < sizeof(req)) {
		ssize_t n = read(fd, req + used, sizeof(req) - used - 1);
		if (n <= 0) { close(fd); return NULL; }
		used += (size_t)n;
		req[used] = '\0';
		if (strstr(req, "\r\n\r\n")) break;
	}

	if (strncmp(req, "GET / ", 6) == 0 || strncmp(req, "GET /index.html", 15) == 0) {
		char path[1024];
		snprintf(path, sizeof(path), "%s/index.html", O.web_dir);
		hwSendFile(fd, path, "text/html; charset=utf-8");
	} else if (strncmp(req, "GET /api/events", 15) == 0) {
		hwServeEvents(fd);
	} else if (strncmp(req, "POST /api/reset", 15) == 0) {
		if (!hwOriginOk(req)) hwSendSimple(fd, "403 Forbidden", "text/plain", "cross-origin denied\n");
		else { hwRingReset(); hwSendSimple(fd, "200 OK", "application/json", "{\"ok\":true}"); }
	} else if (strncmp(req, "GET /api/sessions", 17) == 0) {
		hwServeSessions(fd);
	} else if (strncmp(req, "GET /api/dirs", 13) == 0) {
		char path[4096];
		hwQueryParam(req, "path", path, sizeof(path));
		hwServeDirs(fd, path[0] ? path : (O.work_dir ? O.work_dir : "."));
	} else if (strncmp(req, "GET /api/models", 15) == 0) {
		char search[4096];
		hwQueryParam(req, "search", search, sizeof(search));
		hwServeModels(fd, search);
	} else if (strncmp(req, "POST /api/link", 14) == 0) {
		if (!hwOriginOk(req)) { hwSendSimple(fd, "403 Forbidden", "text/plain", "cross-origin denied\n"); close(fd); return NULL; }
		char *body = hwReadBody(fd, req, used);
		int rc = (body && *body) ? hwLinkModel(body) : -1;
		free(body);
		hwSendSimple(fd, rc == 0 ? "200 OK" : "400 Bad Request", "application/json",
		             rc == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
	} else if (strncmp(req, "POST /api/session/delete", 24) == 0) {
		if (!hwOriginOk(req)) { hwSendSimple(fd, "403 Forbidden", "text/plain", "cross-origin denied\n"); close(fd); return NULL; }
		char *sid = hwReadBody(fd, req, used);
		int rc = -1;
		if (sid && *sid && !strchr(sid, '/') && !strstr(sid, "..")) rc = hwDeleteSession(sid);
		fprintf(stderr, "hako-serve: delete session [%s] rc=%d\n", sid ? sid : "", rc);
		free(sid);
		hwSendSimple(fd, rc == 0 ? "200 OK" : "400 Bad Request", "application/json",
		             rc == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
	} else if (strncmp(req, "POST /api/session/rename", 24) == 0) {
		if (!hwOriginOk(req)) { hwSendSimple(fd, "403 Forbidden", "text/plain", "cross-origin denied\n"); close(fd); return NULL; }
		char *body = hwReadBody(fd, req, used);          /* "<sid>\n<title>" */
		int rc = -1;
		if (body) {
			char *nl = strchr(body, '\n');
			if (nl) {
				*nl = '\0';
				if (*body && !strchr(body, '/') && !strstr(body, ".."))
					rc = hwRenameSession(body, nl + 1);
			}
		}
		free(body);
		hwSendSimple(fd, rc == 0 ? "200 OK" : "400 Bad Request", "application/json",
		             rc == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
	} else if (strncmp(req, "GET /api/history", 16) == 0) {
		char sid[256];
		hwQueryParam(req, "id", sid, sizeof(sid));
		if (strchr(sid, '/') || strstr(sid, "..")) hwSendSimple(fd, "400 Bad Request", "text/plain", "bad id\n");
		else hwServeHistory(fd, sid);
	} else if (strncmp(req, "POST /api/restart", 17) == 0 || strncmp(req, "POST /api/open", 14) == 0) {
		if (!hwOriginOk(req)) {
			fprintf(stderr, "hako-serve: open/restart REJECTED (cross-origin)\n");
			hwSendSimple(fd, "403 Forbidden", "text/plain", "cross-origin denied\n");
			close(fd); return NULL;
		}
		char *body = hwReadBody(fd, req, used);
		int is_open = (strncmp(req, "POST /api/open", 14) == 0);
		int rc = hwRestartAgent((is_open && body && *body) ? body : NULL);
		fprintf(stderr, "hako-serve: %s -> dir=[%s] rc=%d\n",
		        is_open ? "open" : "restart", O.work_dir ? O.work_dir : "(null)", rc);
		free(body);
		hwSendSimple(fd, rc == 0 ? "200 OK" : "500 Internal Server Error",
		             "application/json", rc == 0 ? "{\"ok\":true}" : "{\"ok\":false}");
	} else if (strncmp(req, "POST /api/prompt", 16) == 0 ||
	           strncmp(req, "POST /api/slash", 15) == 0 ||
	           strncmp(req, "POST /api/approve", 17) == 0) {
		const char *kind = strncmp(req, "POST /api/slash", 15) == 0   ? "slash"
		                 : strncmp(req, "POST /api/approve", 17) == 0 ? "approve"
		                                                             : "prompt";

		if (!hwOriginOk(req)) {
			hwSendSimple(fd, "403 Forbidden", "text/plain", "cross-origin denied\n");
			close(fd); return NULL;
		}

		char *payload = hwReadBody(fd, req, used);
		if (!payload) { close(fd); return NULL; }
		size_t have = strlen(payload);

		char *esc = malloc(have * 6 + 16);
		if (esc) {
			hwJsonEscape(payload, esc, have * 6 + 16);
			const char *field = !strcmp(kind, "slash")   ? "cmd"
			                  : !strcmp(kind, "approve") ? "answer"
			                                             : "text";
			size_t cap = strlen(esc) + 64;
			char *line = malloc(cap);
			if (line) {
				snprintf(line, cap, "{\"type\":\"%s\",\"%s\":\"%s\"}", kind, field, esc);
				hwSendToAgent(line);

				/* Echo what the user sent into the ring. `hako --pipe` never
				   replays the user's own turn, so without this a refresh loses
				   it — and the front end can't tell a turn is in flight. */
				if (strcmp(kind, "approve") != 0) {
					snprintf(line, cap, "{\"type\":\"message\",\"role\":\"user\",\"text\":\"%s\"}", esc);
					hwRingPush(line);
				} else if (g_pending_tool_id) {
					/* Record the verdict so a reload renders the box settled
					   instead of offering buttons for an answered request. */
					snprintf(line, cap, "{\"type\":\"tool_decision\",\"id\":%ld,\"answer\":\"%s\"}",
					         g_pending_tool_id, esc);
					hwRingPush(line);
					g_pending_tool_id = 0;
				}
				free(line);
			}
			free(esc);
		}
		free(payload);
		hwSendSimple(fd, "200 OK", "application/json", "{\"ok\":true}");
	} else {
		hwSendSimple(fd, "404 Not Found", "text/plain", "not found\n");
	}

	close(fd);
	return NULL;
}

int main(int argc, char **argv) {
	signal(SIGPIPE, SIG_IGN);

	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if      (!strcmp(a, "--port") && i + 1 < argc) O.port      = atoi(argv[++i]);
		else if (!strcmp(a, "--bind") && i + 1 < argc) O.bind_addr = argv[++i];
		else if (!strcmp(a, "--web")  && i + 1 < argc) O.web_dir   = argv[++i];
		else if (!strcmp(a, "--dir")  && i + 1 < argc) O.work_dir  = argv[++i];
		else if (!strcmp(a, "--hako") && i + 1 < argc) O.hako_bin  = argv[++i];
		else if (!strcmp(a, "--yolo"))                 O.yolo      = 1;
		else if (!strcmp(a, "--version")) { printf("hako-serve %s\n", HW_VERSION); return 0; }
		else if (!strcmp(a, "--help")) {
			printf("hako-serve %s — bridges `hako --pipe` to a browser\n\n"
			       "  --port N      listen port (default 8787)\n"
			       "  --bind ADDR   bind address (default 127.0.0.1)\n"
			       "  --dir PATH    working directory for the agent\n"
			       "  --web PATH    directory holding index.html (default .)\n"
			       "  --hako PATH   hako binary (default: hako on PATH)\n"
			       "  --yolo        auto-approve tool calls (see warning)\n", HW_VERSION);
			return 0;
		} else { fprintf(stderr, "unknown option: %s\n", a); return 2; }
	}

	if (strcmp(O.bind_addr, "127.0.0.1") != 0) {
		fprintf(stderr,
			"\n  WARNING: binding to %s exposes an unauthenticated agent to the network.\n"
			"  There is no pairing or token check in v0. Anyone who can reach this port\n"
			"  can run the agent as you. Use 127.0.0.1 unless you know the network.\n\n",
			O.bind_addr);
	}
	if (O.yolo) {
		fprintf(stderr,
			"\n  WARNING: --yolo auto-approves every tool call, including file writes\n"
			"  and shell commands, with no prompt. The approval round-trip is not built\n"
			"  yet, so this is the only way tools work over the bridge today.\n\n");
	}

	if (hwStartAgent() != 0) {
		fprintf(stderr, "hako-serve: failed to start agent\n");
		return 1;
	}

	int srv = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port   = htons((uint16_t)O.port);
	if (inet_pton(AF_INET, O.bind_addr, &sa.sin_addr) != 1) {
		fprintf(stderr, "hako-serve: bad bind address %s\n", O.bind_addr);
		return 1;
	}
	if (bind(srv, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		fprintf(stderr, "hako-serve: bind %s:%d failed: %s\n", O.bind_addr, O.port, strerror(errno));
		return 1;
	}
	listen(srv, 16);

	printf("hako studio (bridge v%s) — http://%s:%d\n", HW_VERSION, O.bind_addr, O.port);
	printf("agent pid %d, working dir %s\n", (int)g_agent_pid, O.work_dir ? O.work_dir : ".");
	fflush(stdout);

	for (;;) {
		int fd = accept(srv, NULL, NULL);
		if (fd < 0) { if (errno == EINTR) continue; break; }
		pthread_t t;
		if (pthread_create(&t, NULL, hwConnThread, (void *)(long)fd) != 0) close(fd);
		else pthread_detach(t);
	}
	return 0;
}
