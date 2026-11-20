/*
 * A small HTTP/1.1 server for the network tests (header only; Linux and Windows). It listens on 127.0.0.1 on a free
 * port, runs every connection on its own thread (FFmpeg opens a new connection to seek before it closes the old
 * one) and records what it was asked for.
 *
 *   /files/<name>       a file from the root directory, with range requests (206 Partial Content)
 *   /auth/<name>        the same, but only with the header "X-Test-Token: secret" (401 otherwise)
 *   /redirect/<name>    302 to /files/<name>
 *   /status/404         404 Not Found
 *   /hang               reads the request and never answers (until the server stops)
 *   /text/<name>        the file as text, after replacing "{PORT}" with the server's port (playlists)
 */
#ifndef FFAUDIO_TEST_HTTP_SERVER_H
#define FFAUDIO_TEST_HTTP_SERVER_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL2/SDL.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET ts_socket;
#define TS_BAD_SOCKET INVALID_SOCKET
#define ts_closesocket closesocket
#define TS_SEND_FLAGS 0
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int ts_socket;
#define TS_BAD_SOCKET (-1)
#define ts_closesocket close
#define TS_SEND_FLAGS MSG_NOSIGNAL   // a client that hangs up must not kill the test with SIGPIPE
#endif

typedef struct TsServer {
    int port;
    char root[1024];
    ts_socket listener;
    SDL_Thread *thread;
    SDL_atomic_t stop, active;
    SDL_mutex *lock;
    // What clients asked for (guarded by lock)
    int requests, range_requests;
    char last_path[512];
    char last_user_agent[256];
    char paths[64][128];            // the first 64 request paths, in order
} TsServer;

typedef struct TsConn {
    TsServer *server;
    ts_socket sock;
} TsConn;

// Wait until the socket is readable, in steps that notice a stop. false on a stop or after timeout_ms.
static bool ts_wait_readable(TsServer *s, const ts_socket sock, int timeout_ms) {
    while (timeout_ms > 0 && !SDL_AtomicGet(&s->stop)) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(sock, &rd);
        struct timeval tv = { 0, 50000 };
        const int r = select((int)sock + 1, &rd, NULL, NULL, &tv);
        if (r > 0) return true;
        if (r < 0) return false;
        timeout_ms -= 50;
    }
    return false;
}

static bool ts_send_all(const ts_socket sock, const char *data, size_t len) {
    while (len > 0) {
        const int n = send(sock, data, (int)(len > 65536 ? 65536 : len), TS_SEND_FLAGS);
        if (n <= 0) return false;
        data += n;
        len -= (size_t)n;
    }
    return true;
}

static void ts_reply(const ts_socket sock, const char *status, const char *extra_headers, const char *body) {
    char head[1024];
    const size_t blen = body ? strlen(body) : 0;
    const int n = snprintf(head, sizeof(head), "HTTP/1.1 %s\r\nContent-Length: %zu\r\nConnection: close\r\n%s\r\n",
                           status, blen, extra_headers ? extra_headers : "");
    ts_send_all(sock, head, (size_t)n);
    if (blen) ts_send_all(sock, body, blen);
}

// Value of a request header (case-insensitive name), copied into out; false when absent
static bool ts_header(const char *req, const char *name, char *out, const size_t size) {
    const size_t nlen = strlen(name);
    for (const char *line = strstr(req, "\r\n"); line && line[2]; line = strstr(line + 2, "\r\n")) {
        const char *h = line + 2;
        if (SDL_strncasecmp(h, name, nlen) == 0 && h[nlen] == ':') {
            const char *v = h + nlen + 1;
            while (*v == ' ') v++;
            const char *end = strstr(v, "\r\n");
            const size_t len = end ? (size_t)(end - v) : strlen(v);
            snprintf(out, size, "%.*s", (int)(len < size ? len : size - 1), v);
            return true;
        }
    }
    return false;
}

static void ts_serve_file(TsServer *s, const ts_socket sock, const char *name, const char *req, const bool as_text) {
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", s->root, name);
    FILE *f = (strstr(name, "..") == NULL) ? fopen(path, "rb") : NULL;
    if (!f) {
        ts_reply(sock, "404 Not Found", NULL, "no such file");
        return;
    }
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (as_text) { // small text file with the port filled in
        char *text = malloc((size_t)size + 1), *out = malloc((size_t)size * 2 + 64);
        if (text && out && fread(text, 1, (size_t)size, f) == (size_t)size) {
            text[size] = '\0';
            char port[16], *o = out;
            snprintf(port, sizeof(port), "%d", s->port);
            for (const char *p = text; *p;) {
                if (!strncmp(p, "{PORT}", 6)) { o += sprintf(o, "%s", port); p += 6; }
                else *o++ = *p++;
            }
            *o = '\0';
            ts_reply(sock, "200 OK", "Content-Type: application/vnd.apple.mpegurl\r\n", out);
        }
        free(text);
        free(out);
        fclose(f);
        return;
    }

    long from = 0, to = size - 1;
    char range[128];
    const bool ranged = ts_header(req, "Range", range, sizeof(range)) && sscanf(range, "bytes=%ld-%ld", &from, &to) >= 1;
    if (ranged) {
        if (to >= size || to < from) to = size - 1;
        SDL_LockMutex(s->lock);
        s->range_requests++;
        SDL_UnlockMutex(s->lock);
    }
    if (from >= size && size > 0) {
        char h[128];
        snprintf(h, sizeof(h), "Content-Range: bytes */%ld\r\n", size);
        ts_reply(sock, "416 Range Not Satisfiable", h, NULL);
        fclose(f);
        return;
    }
    char head[512];
    const int n = ranged
        ? snprintf(head, sizeof(head), "HTTP/1.1 206 Partial Content\r\nContent-Length: %ld\r\nContent-Range: bytes %ld-%ld/%ld\r\n"
                   "Accept-Ranges: bytes\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n\r\n",
                   to - from + 1, from, to, size)
        : snprintf(head, sizeof(head), "HTTP/1.1 200 OK\r\nContent-Length: %ld\r\nAccept-Ranges: bytes\r\n"
                   "Content-Type: application/octet-stream\r\nConnection: close\r\n\r\n", size);
    if (ts_send_all(sock, head, (size_t)n)) {
        fseek(f, from, SEEK_SET);
        char buf[16384];
        long left = to - from + 1;
        while (left > 0 && !SDL_AtomicGet(&s->stop)) {
            const size_t want = left > (long)sizeof(buf) ? sizeof(buf) : (size_t)left;
            const size_t got = fread(buf, 1, want, f);
            if (!got || !ts_send_all(sock, buf, got)) break;
            left -= (long)got;
        }
    }
    fclose(f);
}

static int ts_connection(void *arg) {
    TsConn *c = arg;
    TsServer *s = c->server;
    char req[8192];
    size_t len = 0;
    while (len < sizeof(req) - 1 && ts_wait_readable(s, c->sock, 5000)) {
        const int n = recv(c->sock, req + len, (int)(sizeof(req) - 1 - len), 0);
        if (n <= 0) break;
        len += (size_t)n;
        req[len] = '\0';
        if (strstr(req, "\r\n\r\n")) break;
    }
    req[len] = '\0';
    char method[16] = "", path[512] = "";
    if (sscanf(req, "%15s %511s", method, path) == 2) {
        SDL_LockMutex(s->lock);
        if (s->requests < 64) snprintf(s->paths[s->requests], sizeof(s->paths[0]), "%s", path);
        s->requests++;
        snprintf(s->last_path, sizeof(s->last_path), "%s", path);
        if (!ts_header(req, "User-Agent", s->last_user_agent, sizeof(s->last_user_agent))) s->last_user_agent[0] = '\0';
        SDL_UnlockMutex(s->lock);

        char token[64];
        if (!strncmp(path, "/files/", 7)) ts_serve_file(s, c->sock, path + 7, req, false);
        else if (!strncmp(path, "/text/", 6)) ts_serve_file(s, c->sock, path + 6, req, true);
        else if (!strncmp(path, "/auth/", 6)) {
            if (ts_header(req, "X-Test-Token", token, sizeof(token)) && !strcmp(token, "secret"))
                ts_serve_file(s, c->sock, path + 6, req, false);
            else
                ts_reply(c->sock, "401 Unauthorized", NULL, "token needed");
        }
        else if (!strncmp(path, "/redirect/", 10)) {
            char loc[600];
            snprintf(loc, sizeof(loc), "Location: /files/%s\r\n", path + 10);
            ts_reply(c->sock, "302 Found", loc, NULL);
        }
        else if (!strcmp(path, "/status/404")) ts_reply(c->sock, "404 Not Found", NULL, "not here");
        else if (!strcmp(path, "/hang")) { while (!SDL_AtomicGet(&s->stop)) SDL_Delay(20); }
        else ts_reply(c->sock, "404 Not Found", NULL, "unknown");
    }
    ts_closesocket(c->sock);
    free(c);
    SDL_AtomicAdd(&s->active, -1);
    return 0;
}

static int ts_accept_loop(void *arg) {
    TsServer *s = arg;
    while (!SDL_AtomicGet(&s->stop)) {
        if (!ts_wait_readable(s, s->listener, 100)) continue;
        const ts_socket c = accept(s->listener, NULL, NULL);
        if (c == TS_BAD_SOCKET) continue;
        TsConn *conn = malloc(sizeof(TsConn));
        if (!conn) { ts_closesocket(c); continue; }
        conn->server = s;
        conn->sock = c;
        SDL_AtomicAdd(&s->active, 1);
        SDL_Thread *t = SDL_CreateThread(ts_connection, "ts_conn", conn);
        if (t) SDL_DetachThread(t);
        else { SDL_AtomicAdd(&s->active, -1); ts_closesocket(c); free(conn); }
    }
    return 0;
}

// A listening socket on 127.0.0.1 and a free port (0 = any); TS_BAD_SOCKET on failure
static ts_socket ts_listen(int *port) {
#ifdef _WIN32
    static bool wsa;
    if (!wsa) { WSADATA d; wsa = WSAStartup(MAKEWORD(2, 2), &d) == 0; }
#endif
    const ts_socket l = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (l == TS_BAD_SOCKET) return TS_BAD_SOCKET;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((unsigned short)*port);
    socklen_t alen = sizeof(a);
    if (bind(l, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(l, 16) != 0 ||
        getsockname(l, (struct sockaddr *)&a, &alen) != 0) {
        ts_closesocket(l);
        return TS_BAD_SOCKET;
    }
    *port = ntohs(a.sin_port);
    return l;
}

static bool ts_start(TsServer *s, const char *root) {
    memset(s, 0, sizeof(*s));
    snprintf(s->root, sizeof(s->root), "%s", root);
    s->lock = SDL_CreateMutex();
    s->listener = ts_listen(&s->port);
    if (!s->lock || s->listener == TS_BAD_SOCKET) return false;
    s->thread = SDL_CreateThread(ts_accept_loop, "ts_accept", s);
    return s->thread != NULL;
}

static void ts_stop(TsServer *s) {
    SDL_AtomicSet(&s->stop, 1);
    if (s->thread) SDL_WaitThread(s->thread, NULL);
    for (int i = 0; i < 250 && SDL_AtomicGet(&s->active) > 0; i++) SDL_Delay(20); // connections notice the stop
    if (s->listener != TS_BAD_SOCKET) ts_closesocket(s->listener);
    if (s->lock) SDL_DestroyMutex(s->lock);
}

// How many requests asked for a path starting with `prefix`
static int ts_count(TsServer *s, const char *prefix) {
    int n = 0;
    SDL_LockMutex(s->lock);
    for (int i = 0; i < s->requests && i < 64; i++)
        if (!strncmp(s->paths[i], prefix, strlen(prefix))) n++;
    SDL_UnlockMutex(s->lock);
    return n;
}

static void ts_reset(TsServer *s) {
    SDL_LockMutex(s->lock);
    s->requests = s->range_requests = 0;
    s->last_path[0] = s->last_user_agent[0] = '\0';
    SDL_UnlockMutex(s->lock);
}

// Copies of what the server recorded, taken under its lock
static void ts_user_agent(TsServer *s, char *out, const size_t size) {
    SDL_LockMutex(s->lock);
    snprintf(out, size, "%s", s->last_user_agent);
    SDL_UnlockMutex(s->lock);
}
static int ts_range_requests(TsServer *s) {
    SDL_LockMutex(s->lock);
    const int n = s->range_requests;
    SDL_UnlockMutex(s->lock);
    return n;
}

// A port with nothing listening on it (bound once, then closed)
static int ts_closed_port(void) {
    int port = 0;
    const ts_socket l = ts_listen(&port);
    if (l != TS_BAD_SOCKET) ts_closesocket(l);
    return port;
}

#endif //FFAUDIO_TEST_HTTP_SERVER_H
