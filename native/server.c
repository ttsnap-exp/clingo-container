/*
 * clingo-web-native: Minimal HTTP server for the Clingo ASP solver.
 * Pure C with libmicrohttpd.
 * No JVM, no framework overhead — starts in milliseconds, uses ~3MB RAM.
 *
 * Routes:
 *   GET  /           -> HTML UI (browser)
 *   POST /run        -> run clingo, return HTML page with result (browser form)
 *   POST /api/solve  -> run clingo, return plain-text result (REST API)
 *
 * REST API usage:
 *   curl -X POST http://localhost:8080/api/solve \
 *        -H 'Content-Type: text/plain' \
 *        --data-binary @program.lp
 *
 *   curl -X POST http://localhost:8080/api/solve \
 *        -H 'Content-Type: text/plain' \
 *        -d 'node(1..3). edge(1,2). edge(2,3). {color(N,red);color(N,blue)}=1 :- node(N). :- edge(X,Y), color(X,C), color(Y,C). #show color/2.'
 */

#define _GNU_SOURCE
#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

#define PORT        8080
#define MAX_BODY    (512 * 1024)   /* 512 KB max POST body */
#define TIMEOUT_SEC 30

/* ---------------------------------------------------------------- Logging -- */

/* ISO-8601 timestamp into buf (at least 26 bytes). */
static void log_timestamp(char *buf, size_t size) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    gmtime_r(&tv.tv_sec, &tm);
    int ms = (int)(tv.tv_usec / 1000);
    strftime(buf, size, "%Y-%m-%dT%H:%M:%S", &tm);
    size_t len = strlen(buf);
    snprintf(buf + len, size - len, ".%03dZ", ms);
}

/*
 * log_request() — called once per completed request.
 *   method  : "GET" / "POST"
 *   url     : "/", "/run", "/api/solve", ...
 *   status  : HTTP status code returned
 *   source  : "browser" | "api" | "unknown"
 *   elapsed_ms : wall time from first byte to response (milliseconds)
 *   extra   : optional extra context, may be NULL
 */
static void log_request(const char *method, const char *url, int status,
                        const char *source, long elapsed_ms,
                        const char *extra) {
    char ts[32];
    log_timestamp(ts, sizeof(ts));
    if (extra && extra[0]) {
        fprintf(stdout, "[%s] %s %s %d  source=%-7s  elapsed=%ldms  %s\n",
                ts, method, url, status, source, elapsed_ms, extra);
    } else {
        fprintf(stdout, "[%s] %s %s %d  source=%-7s  elapsed=%ldms\n",
                ts, method, url, status, source, elapsed_ms);
    }
    fflush(stdout);
}

/* Derive source label from Accept / Content-Type headers. */
static const char *detect_source(struct MHD_Connection *con, const char *url) {
    /* /api/* is always the REST API */
    if (strncmp(url, "/api/", 5) == 0) return "api";
    /* Accept: text/html  → browser */
    const char *accept = MHD_lookup_connection_value(con, MHD_HEADER_KIND, "Accept");
    if (accept && strstr(accept, "text/html")) return "browser";
    /* Content-Type: application/x-www-form-urlencoded → browser form */
    const char *ct = MHD_lookup_connection_value(con, MHD_HEADER_KIND, "Content-Type");
    if (ct && strstr(ct, "application/x-www-form-urlencoded")) return "browser";
    return "unknown";
}

/* ------------------------------------------------------------------ HTML -- */
static const char HTML_PAGE[] =
"<!DOCTYPE html>\n"
"<html lang=\"de\">\n"
"<head>\n"
"<meta charset=\"UTF-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"<title>Clingo Web Solver</title>\n"
"<style>\n"
"*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}\n"
":root{\n"
"  --bg:#111210;--surface:#1a1b19;--surface2:#222420;--border:#2e302c;\n"
"  --text:#d0d1cc;--muted:#6b6d67;--accent:#4f9e6f;--accent-h:#3d8259;\n"
"  --error:#c05050;--radius:6px;\n"
"  --mono:'JetBrains Mono','Fira Code','Cascadia Code',monospace}\n"
"html,body{height:100%}\n"
"body{font-family:system-ui,-apple-system,sans-serif;background:var(--bg);\n"
"  color:var(--text);display:flex;flex-direction:column;min-height:100vh}\n"
"header{background:var(--surface);border-bottom:1px solid var(--border);\n"
"  padding:.75rem 1.5rem;display:flex;align-items:center;gap:.75rem}\n"
"header svg{color:var(--accent);flex-shrink:0}\n"
"header h1{font-size:1.05rem;font-weight:600;letter-spacing:.01em}\n"
"header span{font-size:.8rem;color:var(--muted)}\n"
"main{flex:1;display:grid;grid-template-columns:1fr 1fr;gap:0;overflow:hidden}\n"
".pane{display:flex;flex-direction:column;overflow:hidden}\n"
".pane+.pane{border-left:1px solid var(--border)}\n"
".pane-header{background:var(--surface);border-bottom:1px solid var(--border);\n"
"  padding:.5rem 1rem;font-size:.75rem;font-weight:600;letter-spacing:.06em;\n"
"  text-transform:uppercase;color:var(--muted);display:flex;align-items:center;justify-content:space-between}\n"
"textarea{flex:1;background:var(--bg);color:var(--text);font-family:var(--mono);\n"
"  font-size:.85rem;line-height:1.6;border:none;outline:none;resize:none;padding:1rem;tab-size:4}\n"
"textarea:focus{background:#131511}\n"
".output{flex:1;font-family:var(--mono);font-size:.85rem;line-height:1.6;\n"
"  padding:1rem;overflow-y:auto;white-space:pre-wrap;word-break:break-all;color:var(--text)}\n"
".output.empty{color:var(--muted);font-style:italic}\n"
".output.error{color:var(--error)}\n"
"footer{background:var(--surface);border-top:1px solid var(--border);\n"
"  padding:.6rem 1rem;display:flex;align-items:center;gap:.75rem}\n"
"button[type=submit]{background:var(--accent);color:#fff;border:none;\n"
"  border-radius:var(--radius);padding:.45rem 1.2rem;font-size:.85rem;\n"
"  font-weight:600;cursor:pointer;transition:background .15s}\n"
"button[type=submit]:hover{background:var(--accent-h)}\n"
"button[type=submit]:active{transform:scale(.97)}\n"
".hint{font-size:.75rem;color:var(--muted);margin-left:auto}\n"
"@media(max-width:700px){main{grid-template-columns:1fr;grid-template-rows:1fr 1fr}\n"
".pane+.pane{border-left:none;border-top:1px solid var(--border)}}\n"
"</style></head>\n"
"<body>\n"
"<header>\n"
"<svg width=\"22\" height=\"22\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\">\n"
"<polyline points=\"16 18 22 12 16 6\"/><polyline points=\"8 6 2 12 8 18\"/>\n"
"</svg>\n"
"<h1>Clingo Web Solver</h1><span>Answer Set Programming</span>\n"
"</header>\n"
"<form method=\"post\" action=\"/run\" style=\"flex:1;display:contents\">\n"
"<main>\n"
"<div class=\"pane\">\n"
"<div class=\"pane-header\"><span>&#9654; Program (.lp)</span></div>\n"
"<textarea name=\"program\" spellcheck=\"false\" autocorrect=\"off\" autocapitalize=\"off\">";

static const char HTML_MID[] =
"</textarea></div>\n"
"<div class=\"pane\">\n"
"<div class=\"pane-header\"><span>&#9654; Output</span></div>\n";

static const char HTML_END[] =
"</div></main>\n"
"<footer>\n"
"<button type=\"submit\">&#9654;&nbsp; Ausf&uuml;hren</button>\n"
"<span class=\"hint\">Clingo 0 &mdash; alle Antwortmengen</span>\n"
"</footer></form></body></html>\n";

static const char DEFAULT_PROGRAM[] =
"% Graph coloring example (3 colors, 4 nodes)\n"
"node(1..4).\n"
"color(red;green;blue).\n"
"edge(1,2). edge(1,3). edge(2,3). edge(2,4). edge(3,4).\n"
"\n"
"% Assign exactly one color per node\n"
"{ assign(N, C) : color(C) } = 1 :- node(N).\n"
"\n"
"% Adjacent nodes must have different colors\n"
":- edge(X, Y), assign(X, C), assign(Y, C).\n"
"\n"
"#show assign/2.\n";

/* --------------------------------------------------------- URL decoding -- */
static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t url_decode(char *dst, const char *src, size_t src_len) {
    size_t di = 0;
    for (size_t si = 0; si < src_len; ) {
        if (src[si] == '+') {
            dst[di++] = ' '; si++;
        } else if (src[si] == '%' && si + 2 < src_len) {
            int hi = hex_val(src[si+1]);
            int lo = hex_val(src[si+2]);
            if (hi >= 0 && lo >= 0) {
                dst[di++] = (char)((hi << 4) | lo);
                si += 3;
            } else {
                dst[di++] = src[si++];
            }
        } else {
            dst[di++] = src[si++];
        }
    }
    dst[di] = '\0';
    return di;
}

static char *form_field(const char *body, size_t body_len, const char *key) {
    size_t klen = strlen(key);
    const char *p = body;
    const char *end = body + body_len;
    while (p < end) {
        const char *amp = memchr(p, '&', end - p);
        size_t pair_len = amp ? (size_t)(amp - p) : (size_t)(end - p);
        if (pair_len > klen + 1 && memcmp(p, key, klen) == 0 && p[klen] == '=') {
            const char *val = p + klen + 1;
            size_t val_len = pair_len - klen - 1;
            char *out = malloc(val_len + 1);
            if (!out) return NULL;
            url_decode(out, val, val_len);
            return out;
        }
        p = amp ? amp + 1 : end;
    }
    return NULL;
}

/* -------------------------------------------------------- HTML escaping -- */
static char *html_escape(const char *s) {
    size_t n = strlen(s);
    char *out = malloc(n * 6 + 1);
    if (!out) return NULL;
    char *d = out;
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
            case '&':  memcpy(d, "&amp;",  5); d += 5; break;
            case '<':  memcpy(d, "&lt;",   4); d += 4; break;
            case '>':  memcpy(d, "&gt;",   4); d += 4; break;
            case '"':  memcpy(d, "&quot;", 6); d += 6; break;
            case '\'': memcpy(d, "&#39;",  5); d += 5; break;
            default:   *d++ = s[i]; break;
        }
    }
    *d = '\0';
    return out;
}

/* -------------------------------------------------------- Clingo runner -- */
static char *run_clingo(const char *program) {
    char tmppath[] = "/tmp/clingo_XXXXXX.lp";
    int fd = mkstemps(tmppath, 3);
    if (fd < 0) return strdup("ERROR: cannot create temp file");

    size_t plen = strlen(program);
    if (write(fd, program, plen) != (ssize_t)plen) {
        close(fd); unlink(tmppath);
        return strdup("ERROR: cannot write temp file");
    }
    close(fd);

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        unlink(tmppath);
        return strdup("ERROR: cannot create pipe");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]); close(pipefd[1]);
        unlink(tmppath);
        return strdup("ERROR: fork failed");
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl("/usr/bin/clingo", "clingo", tmppath, "0", NULL);
        const char *msg = "ERROR: clingo not found at /usr/bin/clingo\n";
        write(STDOUT_FILENO, msg, strlen(msg));
        _exit(127);
    }

    close(pipefd[1]);

    size_t buf_size = 256 * 1024;
    size_t buf_used = 0;
    char *buf = malloc(buf_size);
    if (!buf) {
        close(pipefd[0]); waitpid(pid, NULL, 0); unlink(tmppath);
        return strdup("ERROR: OOM");
    }

    struct timeval tv = { .tv_sec = TIMEOUT_SEC, .tv_usec = 0 };
    fd_set rfds;
    int timed_out = 0;

    while (1) {
        FD_ZERO(&rfds);
        FD_SET(pipefd[0], &rfds);
        int sr = select(pipefd[0] + 1, &rfds, NULL, NULL, &tv);
        if (sr < 0) break;
        if (sr == 0) { timed_out = 1; break; }
        if (buf_used + 1 >= buf_size) {
            buf_size *= 2;
            char *nb = realloc(buf, buf_size);
            if (!nb) break;
            buf = nb;
        }
        ssize_t r = read(pipefd[0], buf + buf_used, buf_size - buf_used - 1);
        if (r <= 0) break;
        buf_used += (size_t)r;
    }
    buf[buf_used] = '\0';
    close(pipefd[0]);

    if (timed_out) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        unlink(tmppath);
        free(buf);
        char msg[64];
        snprintf(msg, sizeof(msg), "ERROR: Clingo timed out after %d seconds.", TIMEOUT_SEC);
        return strdup(msg);
    }

    int status;
    waitpid(pid, &status, 0);
    unlink(tmppath);
    return buf;
}

/* ------------------------------------------------------- Request state -- */
typedef struct {
    char  *body;
    size_t body_len;
    struct timeval start_tv;  /* wall-clock time of first call, for elapsed logging */
} ReqState;

/* Helper: milliseconds elapsed since st->start_tv */
static long elapsed_ms(const ReqState *st) {
    struct timeval now;
    gettimeofday(&now, NULL);
    return (long)(now.tv_sec  - st->start_tv.tv_sec)  * 1000L
         + (long)(now.tv_usec - st->start_tv.tv_usec) / 1000L;
}

/* Helper: send a plain-text response */
static enum MHD_Result send_text(struct MHD_Connection *con, int status_code,
                                  char *body_heap) {
    size_t len = strlen(body_heap);
    struct MHD_Response *resp =
        MHD_create_response_from_buffer(len, body_heap, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(resp, "Content-Type", "text/plain; charset=utf-8");
    MHD_add_response_header(resp, "Access-Control-Allow-Origin", "*");
    int ret = MHD_queue_response(con, status_code, resp);
    MHD_destroy_response(resp);
    return ret;
}

/* Helper: send an HTML response */
static enum MHD_Result send_html(struct MHD_Connection *con, int status_code,
                                  char *body_heap, size_t len) {
    struct MHD_Response *resp =
        MHD_create_response_from_buffer(len, body_heap, MHD_RESPMEM_MUST_FREE);
    MHD_add_response_header(resp, "Content-Type", "text/html; charset=utf-8");
    int ret = MHD_queue_response(con, status_code, resp);
    MHD_destroy_response(resp);
    return ret;
}

/* ---------------------------------------------------- MHD request handler -- */
static enum MHD_Result handler(
    void *cls,
    struct MHD_Connection *con,
    const char *url,
    const char *method,
    const char *version,
    const char *upload_data,
    size_t *upload_data_size,
    void **con_cls)
{
    (void)cls; (void)version;

    /* ---- First call: allocate per-request state ----------------------- */
    if (*con_cls == NULL) {
        ReqState *st = calloc(1, sizeof(ReqState));
        if (!st) return MHD_NO;
        gettimeofday(&st->start_tv, NULL);
        *con_cls = st;
        return MHD_YES;
    }
    ReqState *st = (ReqState *)*con_cls;

    /* ---- Accumulate POST body ----------------------------------------- */
    if (strcmp(method, "POST") == 0 && *upload_data_size > 0) {
        size_t new_len = st->body_len + *upload_data_size;
        if (new_len > MAX_BODY) return MHD_NO;
        char *nb = realloc(st->body, new_len + 1);
        if (!nb) return MHD_NO;
        memcpy(nb + st->body_len, upload_data, *upload_data_size);
        nb[new_len] = '\0';
        st->body = nb;
        st->body_len = new_len;
        *upload_data_size = 0;
        return MHD_YES;
    }

    const char *source = detect_source(con, url);

    /* ==================================================================
     *  GET /   — HTML UI
     * ================================================================== */
    if (strcmp(method, "GET") == 0 && strcmp(url, "/") == 0) {
        char *escaped = html_escape(DEFAULT_PROGRAM);
        if (!escaped) return MHD_NO;

        size_t len = sizeof(HTML_PAGE) - 1
                   + strlen(escaped)
                   + sizeof(HTML_MID) - 1 + 80
                   + sizeof(HTML_END) - 1;
        char *page = malloc(len);
        if (!page) { free(escaped); return MHD_NO; }

        int sz = snprintf(page, len,
            "%s%s%s"
            "<div class=\"output empty\">Noch kein Ergebnis &mdash; Programm ausf&uuml;hren...</div>\n"
            "%s",
            HTML_PAGE, escaped, HTML_MID, HTML_END);
        free(escaped);

        log_request(method, url, MHD_HTTP_OK, source, elapsed_ms(st), NULL);
        return send_html(con, MHD_HTTP_OK, page, (size_t)sz);
    }

    /* ==================================================================
     *  POST /run   — browser form submit, returns HTML
     * ================================================================== */
    if (strcmp(method, "POST") == 0 && strcmp(url, "/run") == 0) {
        char *program = form_field(st->body, st->body_len, "program");
        if (!program) program = strdup("");

        char extra[64];
        snprintf(extra, sizeof(extra), "program_bytes=%zu", strlen(program));

        char *result_raw = run_clingo(program);
        int   is_error   = (strncmp(result_raw, "ERROR", 5) == 0);

        char *prog_esc   = html_escape(program);
        char *result_esc = html_escape(result_raw);
        free(program);
        free(result_raw);

        const char *cls = is_error ? "output error" : "output";
        size_t len = sizeof(HTML_PAGE) - 1
                   + (prog_esc   ? strlen(prog_esc)   : 0)
                   + sizeof(HTML_MID) - 1
                   + (result_esc ? strlen(result_esc) : 0)
                   + 128
                   + sizeof(HTML_END) - 1;
        char *page = malloc(len);
        if (!page) { free(prog_esc); free(result_esc); return MHD_NO; }

        int sz = snprintf(page, len,
            "%s%s%s<div class=\"%s\">%s</div>\n%s",
            HTML_PAGE, prog_esc ? prog_esc : "", HTML_MID,
            cls, result_esc ? result_esc : "", HTML_END);
        free(prog_esc);
        free(result_esc);

        log_request(method, url, MHD_HTTP_OK, source, elapsed_ms(st), extra);
        return send_html(con, MHD_HTTP_OK, page, (size_t)sz);
    }

    /* ==================================================================
     *  POST /api/solve   — REST endpoint, returns plain text
     *
     *  Request body = raw .lp program (Content-Type: text/plain)
     *  Response     = clingo stdout+stderr (plain text)
     *  HTTP 200     = clingo ran (check body for ERROR: prefix on failure)
     *  HTTP 400     = empty body
     * ================================================================== */
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/solve") == 0) {
        if (st->body_len == 0) {
            char *msg = strdup("ERROR: empty request body — send the .lp program as plain text\n");
            log_request(method, url, MHD_HTTP_BAD_REQUEST, source, elapsed_ms(st),
                        "reason=empty_body");
            return send_text(con, MHD_HTTP_BAD_REQUEST, msg);
        }

        /* Body is the raw program — copy to null-terminated string */
        char *program = malloc(st->body_len + 1);
        if (!program) return MHD_NO;
        memcpy(program, st->body, st->body_len);
        program[st->body_len] = '\0';

        char extra[64];
        snprintf(extra, sizeof(extra), "program_bytes=%zu", st->body_len);

        char *result = run_clingo(program);
        free(program);

        int http_status = MHD_HTTP_OK;
        log_request(method, url, http_status, source, elapsed_ms(st), extra);
        return send_text(con, http_status, result);
    }

    /* ==================================================================
     *  404
     * ================================================================== */
    log_request(method, url, MHD_HTTP_NOT_FOUND, source, elapsed_ms(st), NULL);
    char *msg = strdup("404 Not Found\n");
    return send_text(con, MHD_HTTP_NOT_FOUND, msg);
}

static void cleanup(void *cls, struct MHD_Connection *con,
                    void **con_cls, enum MHD_RequestTerminationCode tc) {
    (void)cls; (void)con; (void)tc;
    ReqState *st = (ReqState *)*con_cls;
    if (st) { free(st->body); free(st); }
    *con_cls = NULL;
}

int main(void) {
    struct MHD_Daemon *d = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD,
        PORT, NULL, NULL,
        handler, NULL,
        MHD_OPTION_NOTIFY_COMPLETED, cleanup, NULL,
        MHD_OPTION_END);
    if (!d) {
        fprintf(stderr, "ERROR: could not start HTTP server on port %d\n", PORT);
        return 1;
    }
    char ts[32];
    log_timestamp(ts, sizeof(ts));
    fprintf(stdout, "[%s] clingo-web-native started  port=%d\n", ts, PORT);
    fprintf(stdout, "[%s] routes: GET /  POST /run  POST /api/solve\n", ts);
    fflush(stdout);
    pause();
    MHD_stop_daemon(d);
    return 0;
}
