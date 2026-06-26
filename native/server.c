/*
 * clingo-web-native: Minimal HTTP server for the Clingo ASP solver.
 * Pure C, statically linked with libmicrohttpd.
 * No JVM, no framework overhead — starts in milliseconds, uses ~3MB RAM.
 *
 * Routes:
 *   GET  /        -> HTML UI (identical look to Java version)
 *   POST /run     -> run clingo, return plain-text output
 */

#define _GNU_SOURCE
#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

#define PORT        8080
#define MAX_BODY    (512 * 1024)   /* 512 KB max POST body */
#define TIMEOUT_SEC 30

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

/* Decode application/x-www-form-urlencoded value in-place. Returns new length. */
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

/* Extract a form field from urlencoded body. Returns malloc'd string or NULL. */
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
    /* worst case: every char becomes &amp; (5 bytes) */
    char *out = malloc(n * 5 + 1);
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
    /* Write program to a temp file */
    char tmppath[] = "/tmp/clingo_XXXXXX.lp";
    int fd = mkstemps(tmppath, 3);
    if (fd < 0) return strdup("ERROR: cannot create temp file");

    size_t plen = strlen(program);
    if (write(fd, program, plen) != (ssize_t)plen) {
        close(fd); unlink(tmppath);
        return strdup("ERROR: cannot write temp file");
    }
    close(fd);

    /* Pipe for stdout+stderr */
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
        /* Child */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execl("/usr/bin/clingo", "clingo", tmppath, "0", NULL);
        /* execl failed */
        const char *msg = "ERROR: clingo not found at /usr/bin/clingo\n";
        write(STDOUT_FILENO, msg, strlen(msg));
        _exit(127);
    }

    /* Parent: read with timeout via alarm */
    close(pipefd[1]);

    /* Read output (up to 2MB) */
    size_t buf_size = 256 * 1024;
    size_t buf_used = 0;
    char *buf = malloc(buf_size);
    if (!buf) { close(pipefd[0]); waitpid(pid, NULL, 0); unlink(tmppath); return strdup("ERROR: OOM"); }

    /* Non-blocking read loop with timeout */
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
} ReqState;

/* ---------------------------------------------------- MHD request handler - */
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

    /* ---- First call for this connection: allocate state ---------------- */
    if (*con_cls == NULL) {
        ReqState *st = calloc(1, sizeof(ReqState));
        if (!st) return MHD_NO;
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

    /* ---- Build response ---------------------------------------------- */
    struct MHD_Response *resp = NULL;
    int ret;

    if (strcmp(method, "GET") == 0 && strcmp(url, "/") == 0) {
        /* Serve main page with default program */
        char *escaped = html_escape(DEFAULT_PROGRAM);
        if (!escaped) return MHD_NO;

        size_t len = sizeof(HTML_PAGE) - 1
                   + strlen(escaped)
                   + sizeof(HTML_MID) - 1
                   + 64  /* output div */
                   + sizeof(HTML_END) - 1;
        char *page = malloc(len);
        if (!page) { free(escaped); return MHD_NO; }

        int sz = snprintf(page, len,
            "%s%s%s"
            "<div class=\"output empty\">Noch kein Ergebnis &mdash; Programm ausf&uuml;hren...</div>\n"
            "%s",
            HTML_PAGE, escaped, HTML_MID, HTML_END);
        free(escaped);

        resp = MHD_create_response_from_buffer(sz, page, MHD_RESPMEM_MUST_FREE);
        MHD_add_response_header(resp, "Content-Type", "text/html; charset=utf-8");
        ret = MHD_queue_response(con, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    if (strcmp(method, "POST") == 0 && strcmp(url, "/run") == 0) {
        char *program = form_field(st->body, st->body_len, "program");
        if (!program) program = strdup("");

        char *result_raw = run_clingo(program);
        char *prog_esc   = html_escape(program);
        char *result_esc = html_escape(result_raw);
        free(program);
        free(result_raw);

        int is_error = (prog_esc && strncmp(result_esc ? result_esc : "", "ERROR", 5) == 0);
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
            "%s%s%s"
            "<div class=\"%s\">%s</div>\n"
            "%s",
            HTML_PAGE, prog_esc ? prog_esc : "", HTML_MID,
            cls, result_esc ? result_esc : "",
            HTML_END);
        free(prog_esc);
        free(result_esc);

        resp = MHD_create_response_from_buffer(sz, page, MHD_RESPMEM_MUST_FREE);
        MHD_add_response_header(resp, "Content-Type", "text/html; charset=utf-8");
        ret = MHD_queue_response(con, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }

    /* 404 */
    const char *not_found = "404 Not Found";
    resp = MHD_create_response_from_buffer(strlen(not_found), (void *)not_found, MHD_RESPMEM_PERSISTENT);
    ret = MHD_queue_response(con, MHD_HTTP_NOT_FOUND, resp);
    MHD_destroy_response(resp);
    return ret;
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
    fprintf(stdout, "clingo-web-native listening on port %d\n", PORT);
    fflush(stdout);
    /* Block forever */
    pause();
    MHD_stop_daemon(d);
    return 0;
}
