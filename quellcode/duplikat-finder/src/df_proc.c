/*
 * Duplikat-Finder – Hilfsprozesse starten, befragen, überwachen
 * ==============================================================
 * Jede Anfrage hat eine Zeitgrenze und eine Speichergrenze: hängt ein
 * Hilfsprozess (kaputte Datei, GPU-Treiber) oder braucht er zu viel Speicher,
 * wird er beendet; der Aufrufer startet einen neuen. So kann keine einzelne
 * Datei das Programm zum Absturz bringen oder den Rechner lahmlegen.
 *
 * Außerdem: GPU-Erkennung, Vorschaubilder (eigene kleine Prozessgruppe) und
 * der Vergleich zweier einzelner Dateien.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "df_internal.h"

extern char **environ;

/* ---------------------------------------------------------------------------
 * Start und Ende
 * ------------------------------------------------------------------------- */
static bool read_line(DfProc *p, char *out, size_t sz, double deadline);

bool df_proc_start(DfProc *p, const char *gpu_device, int nice_level, bool white_borders)
{
    memset(p, 0, sizeof *p);
    p->pid = -1;
    p->in_fd = p->out_fd = -1;
    const char *exe = df_worker_path();
    if (!exe) return false;
    int in_pipe[2], out_pipe[2];
    if (pipe2(in_pipe, O_CLOEXEC) != 0) return false;
    if (pipe2(out_pipe, O_CLOEXEC) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return false;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in_pipe[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out_pipe[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, def;
    sigemptyset(&none);
    sigemptyset(&def);
    sigaddset(&def, SIGPIPE);
    sigaddset(&def, SIGINT);
    sigaddset(&def, SIGTERM);
    sigaddset(&def, SIGHUP);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &def);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    char nice_s[16];
    snprintf(nice_s, sizeof nice_s, "%d", nice_level);
    const char *argv[10];
    int argc = 0;
    argv[argc++] = exe;
    if (gpu_device) {
        argv[argc++] = "--gpu";
        argv[argc++] = gpu_device;
    }
    argv[argc++] = "--nice";
    argv[argc++] = nice_s;
    if (white_borders) argv[argc++] = "--white-borders";
    argv[argc] = NULL;
    int rc = posix_spawn(&p->pid, exe, &fa, &attr, (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (rc != 0) {
        close(in_pipe[1]);
        close(out_pipe[0]);
        p->pid = -1;
        return false;
    }
    p->in_fd = in_pipe[1];
    p->out_fd = out_pipe[0];
    p->gpu = gpu_device != NULL;
    char hello[256];
    if (!read_line(p, hello, sizeof hello, df_now() + 20) || strncmp(hello, "H\tOK", 4) != 0) {
        df_proc_stop(p);
        return false;
    }
    char *name = strchr(hello + 5, '\t');
    g_strlcpy(p->name, name && name[1] ? name + 1 : "", sizeof p->name);
    return true;
}

/* Prozess beenden (sofort, er hat keinen Zustand) und einsammeln */
void df_proc_stop(DfProc *p)
{
    if (p->in_fd >= 0) close(p->in_fd);
    if (p->out_fd >= 0) close(p->out_fd);
    p->in_fd = p->out_fd = -1;
    if (p->pid > 0) {
        kill(p->pid, SIGKILL);
        /* hängt er im Treiber (D-Zustand), nicht ewig warten */
        for (int i = 0; i < 200; i++) {
            pid_t r = waitpid(p->pid, NULL, WNOHANG);
            if (r == p->pid || (r < 0 && errno == ECHILD)) break;
            usleep(10000);
        }
    }
    p->pid = -1;
    p->rpos = p->rlen = 0;
}

/* ---------------------------------------------------------------------------
 * Lesen mit Zeitgrenze
 * ------------------------------------------------------------------------- */
typedef struct {
    double deadline;
    long max_rss_mb;
    double next_check;
    DfReplyStatus fail;
} Watch;

/* Daten nachlesen -> false bei Ende/Zeitüberschreitung/Speicher (w->fail) */
static bool fill(DfProc *p, Watch *w)
{
    if (p->rpos > 0 && p->rpos == p->rlen) p->rpos = p->rlen = 0;
    while (true) {
        double now = df_now();
        if (now >= w->deadline) {
            w->fail = DF_R_TIMEOUT;
            return false;
        }
        if (df_cancel) {
            w->fail = DF_R_DEAD;
            return false;
        }
        if (w->max_rss_mb > 0 && now >= w->next_check) {
            w->next_check = now + 1;
            long rss = df_proc_rss_mb(p->pid);
            if (rss > w->max_rss_mb) {
                w->fail = DF_R_MEMORY;
                return false;
            }
        }
        struct pollfd pfd = {.fd = p->out_fd, .events = POLLIN};
        int r = poll(&pfd, 1, 250);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) continue;
        if (p->rlen == sizeof p->rbuf) {        /* Puffer voll: nach vorn schieben */
            memmove(p->rbuf, p->rbuf + p->rpos, p->rlen - p->rpos);
            p->rlen -= p->rpos;
            p->rpos = 0;
        }
        ssize_t n = read(p->out_fd, p->rbuf + p->rlen, sizeof p->rbuf - p->rlen);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            w->fail = DF_R_DEAD;
            return false;
        }
        p->rlen += (size_t)n;
        return true;
    }
}

static bool read_line_w(DfProc *p, char *out, size_t sz, Watch *w)
{
    size_t o = 0;
    while (true) {
        while (p->rpos < p->rlen) {
            char c = p->rbuf[p->rpos++];
            if (c == '\n') {
                out[o] = 0;
                return true;
            }
            if (o + 1 < sz) out[o++] = c;
        }
        if (!fill(p, w)) return false;
    }
}

static bool read_exact(DfProc *p, unsigned char *out, size_t n, Watch *w)
{
    size_t o = 0;
    while (o < n) {
        if (p->rpos < p->rlen) {
            size_t k = p->rlen - p->rpos;
            if (k > n - o) k = n - o;
            memcpy(out + o, p->rbuf + p->rpos, k);
            p->rpos += k;
            o += k;
            continue;
        }
        if (!fill(p, w)) return false;
    }
    return true;
}

static bool read_line(DfProc *p, char *out, size_t sz, double deadline)
{
    Watch w = {.deadline = deadline};
    return read_line_w(p, out, sz, &w);
}

static bool write_all(int fd, const char *s, size_t n)
{
    while (n) {
        ssize_t w = write(fd, s, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        s += w;
        n -= (size_t)w;
    }
    return true;
}

char *df_escape_path(const char *path)
{
    GString *s = g_string_sized_new(strlen(path) + 8);
    for (const char *c = path; *c; c++) {
        if (*c == '\\') g_string_append(s, "\\\\");
        else if (*c == '\t') g_string_append(s, "\\t");
        else if (*c == '\n') g_string_append(s, "\\n");
        else g_string_append_c(s, *c);
    }
    return g_string_free(s, FALSE);
}

DfReplyStatus df_proc_request(DfProc *p, const char *cmd, const char *path, double timeout,
                              long max_rss_mb, DfReply *r)
{
    memset(r, 0, sizeof *r);
    if (p->pid <= 0) return r->status = DF_R_DEAD;
    char *esc = path ? df_escape_path(path) : NULL;
    char *line = esc ? g_strdup_printf("%s\t%s\n", cmd, esc) : g_strdup_printf("%s\n", cmd);
    bool sent = write_all(p->in_fd, line, strlen(line));
    g_free(line);
    g_free(esc);
    Watch w = {.deadline = df_now() + timeout, .max_rss_mb = max_rss_mb};
    char hdr[256];
    if (!sent || !read_line_w(p, hdr, sizeof hdr, &w)) {
        r->status = sent ? w.fail : DF_R_DEAD;
        df_proc_stop(p);
        return r->status;
    }
    char status[16] = "";
    size_t tl = 0, bl = 0;
    if (sscanf(hdr, "R\t%15[^\t]\t%zu\t%zu", status, &tl, &bl) != 3 || tl > (64u << 20) ||
        bl > (512u << 20)) {
        df_proc_stop(p);
        return r->status = DF_R_DEAD;
    }
    r->text = g_malloc(tl + 1);
    r->bin = bl ? g_malloc(bl) : NULL;
    r->bin_len = bl;
    if (!read_exact(p, (unsigned char *)r->text, tl, &w) || (bl && !read_exact(p, r->bin, bl, &w))) {
        DfReplyStatus st = w.fail;
        df_reply_free(r);
        df_proc_stop(p);
        return r->status = st;
    }
    r->text[tl] = 0;
    r->status = !strcmp(status, "OK") ? DF_R_OK : !strcmp(status, "SW") ? DF_R_SW : DF_R_ERR;
    return r->status;
}

void df_reply_free(DfReply *r)
{
    g_free(r->text);
    g_free(r->bin);
    r->text = NULL;
    r->bin = NULL;
    r->bin_len = 0;
}

const char *df_kv(const char *text, const char *key, char *buf, size_t sz)
{
    size_t kl = strlen(key);
    for (const char *s = text; s && *s;) {
        const char *e = strchr(s, '\n');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n > kl && !strncmp(s, key, kl) && s[kl] == '=') {
            size_t vl = n - kl - 1;
            if (vl >= sz) vl = sz - 1;
            memcpy(buf, s + kl + 1, vl);
            buf[vl] = 0;
            return buf;
        }
        s = e ? e + 1 : NULL;
    }
    return NULL;
}

static double kv_num(const char *text, const char *key)
{
    char b[64];
    return df_kv(text, key, b, sizeof b) ? g_ascii_strtod(b, NULL) : 0;
}

bool df_parse_scan_reply(const DfReply *r, DfFile *f)
{
    char b[64];
    f->duration = kv_num(r->text, "duration");
    f->width = (int)kv_num(r->text, "width");
    f->height = (int)kv_num(r->text, "height");
    f->fps = (float)kv_num(r->text, "fps");
    f->bitrate = (int)kv_num(r->text, "bitrate");
    f->abitrate = (int)kv_num(r->text, "abitrate");
    f->asr = (int)kv_num(r->text, "asr");
    f->ach = (int)kv_num(r->text, "ach");
    f->image = kv_num(r->text, "image") > 0;
    g_strlcpy(f->vcodec, df_kv(r->text, "vcodec", b, sizeof b) ? b : "", sizeof f->vcodec);
    g_strlcpy(f->acodec, df_kv(r->text, "acodec", b, sizeof b) ? b : "", sizeof f->acodec);
    int k = (int)kv_num(r->text, "frames");
    if (k < 1 || k > DF_MAX_FRAMES || r->bin_len != (size_t)k * sizeof(DfFrame)) return false;
    f->nframes = k;
    f->frames = g_memdup2(r->bin, r->bin_len);
    /* Bitrate fehlt im Container (z. B. MKV ohne Angabe): aus Größe und Dauer */
    if (f->bitrate <= 0 && f->duration > 1 && f->size > 0)
        f->bitrate = (int)(f->size * 8 / f->duration / 1000);
    return true;
}

/* ---------------------------------------------------------------------------
 * GPU-Erkennung
 * ------------------------------------------------------------------------- */
bool df_gpu_detect(DfGpu *gpu)
{
    memset(gpu, 0, sizeof *gpu);
    if (!df_worker_path()) return false;
    glob_t g;
    if (glob("/dev/dri/renderD*", 0, NULL, &g) != 0) return false;
    for (size_t i = 0; i < g.gl_pathc && !gpu->available; i++) {
        DfProc p;
        if (!df_proc_start(&p, g.gl_pathv[i], 0, false)) continue;
        gpu->available = true;
        g_strlcpy(gpu->device, g.gl_pathv[i], sizeof gpu->device);
        g_strlcpy(gpu->name, *p.name ? p.name : "VAAPI-GPU", sizeof gpu->name);
        df_proc_stop(&p);
    }
    globfree(&g);
    df_log(false, "GPU: %s %s", gpu->available ? gpu->name : "keine (VAAPI-Dekodierung fehlt)",
           gpu->device);
    return gpu->available;
}

/* ---------------------------------------------------------------------------
 * Vorschaubilder: eigene kleine Gruppe von Prozessen (bis 3), unabhängig vom Suchlauf
 * ------------------------------------------------------------------------- */
#define THUMB_PROCS 3
static struct {
    GMutex lock;
    GCond cond;
    DfProc proc[THUMB_PROCS];
    bool busy[THUMB_PROCS];
    bool gpu_known;
    DfGpu gpu;
} T;

void df_thumbs_set_gpu(const DfGpu *gpu)
{
    g_mutex_lock(&T.lock);
    T.gpu = *gpu;
    T.gpu_known = true;
    g_mutex_unlock(&T.lock);
}

static int thumbs_acquire(void)
{
    g_mutex_lock(&T.lock);
    int slot = -1;
    while (slot < 0) {
        for (int i = 0; i < THUMB_PROCS && slot < 0; i++)
            if (!T.busy[i]) slot = i;
        if (slot < 0) g_cond_wait(&T.cond, &T.lock);
    }
    T.busy[slot] = true;
    g_mutex_unlock(&T.lock);
    return slot;
}

static void thumbs_release(int slot)
{
    g_mutex_lock(&T.lock);
    T.busy[slot] = false;
    g_cond_signal(&T.cond);
    g_mutex_unlock(&T.lock);
}

bool df_thumbs_get(const char *path, int k, int width, bool gpu_ok, DfThumbs *t)
{
    memset(t, 0, sizeof *t);
    int slot = thumbs_acquire();
    DfProc *p = &T.proc[slot];
    /* Slot 0 nutzt die GPU (falls vorhanden), die anderen die CPU */
    bool want_gpu = gpu_ok && slot == 0 && T.gpu_known && T.gpu.available;
    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        bool gpu = want_gpu && attempt == 0;
        if (p->pid > 0 && p->gpu != gpu) df_proc_stop(p);
        if (p->pid <= 0 && !df_proc_start(p, gpu ? T.gpu.device : NULL, 5, false)) break;
        char cmd[64];
        snprintf(cmd, sizeof cmd, "T\t%d\t%d\t%d", k, width, width * 9 / 16);
        DfReply r;
        DfReplyStatus st = df_proc_request(p, cmd, path, 60, 3000, &r);
        if (st == DF_R_OK) {
            int i = 0;
            size_t off = 0;
            for (const char *s = r.text; s && *s && i < DF_MAX_FRAMES; i++) {
                int w = 0, h = 0;
                sscanf(s, "%d %d", &w, &h);
                t->w[i] = w;
                t->h[i] = h;
                size_t sz = (size_t)w * h * 3;
                if (w > 0 && h > 0 && off + sz <= r.bin_len) {
                    t->rgb[i] = g_memdup2(r.bin + off, sz);
                    off += sz;
                }
                const char *e = strchr(s, '\n');
                s = e ? e + 1 : NULL;
            }
            t->n = i;
            ok = i > 0;
        }
        df_reply_free(&r);
        if (st != DF_R_SW && st != DF_R_DEAD && st != DF_R_TIMEOUT) break;
        if (df_cancel) break;
    }
    thumbs_release(slot);
    return ok;
}

void df_thumbs_free(DfThumbs *t)
{
    for (int i = 0; i < DF_MAX_FRAMES; i++) g_free(t->rgb[i]);
    memset(t, 0, sizeof *t);
}

void df_thumbs_shutdown(void)
{
    g_mutex_lock(&T.lock);
    for (int i = 0; i < THUMB_PROCS; i++)
        if (!T.busy[i] && T.proc[i].pid > 0) df_proc_stop(&T.proc[i]);
    g_mutex_unlock(&T.lock);
}

/* ---------------------------------------------------------------------------
 * Zwei Dateien vergleichen (Diagnose: "Warum (k)ein Treffer?")
 * ------------------------------------------------------------------------- */
static bool scan_one(DfProc *p, const char *path, int k, DfFile *f, char *err, size_t errsz)
{
    memset(f, 0, sizeof *f);
    f->path = g_strdup(path);
    GStatBuf sb;
    if (g_stat(path, &sb) == 0) {
        f->size = sb.st_size;
        f->mtime = sb.st_mtime;
    }
    char cmd[32];
    snprintf(cmd, sizeof cmd, "S\t%d", k);
    DfReply r;
    DfReplyStatus st = df_proc_request(p, cmd, path, 300, 4000, &r);
    bool ok = st == DF_R_OK && df_parse_scan_reply(&r, f);
    if (!ok)
        snprintf(err, errsz, "%s: %s", path,
                 st == DF_R_ERR ? r.text : st == DF_R_TIMEOUT ? "Zeitüberschreitung"
                 : st == DF_R_MEMORY ? "zu viel Speicher" : "Hilfsprozess abgestürzt");
    df_reply_free(&r);
    return ok;
}

bool df_pair_test(const char *a, const char *b, int frames, bool white_borders, DfPairTest *t)
{
    memset(t, 0, sizeof *t);
    DfProc p;
    if (!df_proc_start(&p, NULL, 0, white_borders)) {
        snprintf(t->error, sizeof t->error, "Hilfsprozess nicht startbar");
        return false;
    }
    bool ok = scan_one(&p, a, frames, &t->a, t->error, sizeof t->error);
    if (ok && p.pid <= 0) ok = df_proc_start(&p, NULL, 0, white_borders);
    ok = ok && scan_one(&p, b, frames, &t->b, t->error, sizeof t->error);
    df_proc_stop(&p);
    if (!ok) return false;
    t->nframes = MIN(t->a.nframes, t->b.nframes);
    for (int i = 0; i < t->nframes; i++) {
        const DfFrame *x = &t->a.frames[i], *y = &t->b.frames[i];
        bool usable = (x->flags & DF_FRAME_OK) && (y->flags & DF_FRAME_OK) &&
                      !(x->flags & DF_FRAME_FLAT) && !(y->flags & DF_FRAME_FLAT);
        t->dist[i] = usable ? __builtin_popcountll(x->hash ^ y->hash) : -1;
        t->dist_flip[i] = usable ? __builtin_popcountll(x->hash ^ y->hash_flip) : -1;
    }
    t->similarity = df_similarity(&t->a, &t->b, false);
    t->similarity_flip = df_similarity(&t->a, &t->b, true);
    t->ok = true;
    return true;
}

void df_pair_test_free(DfPairTest *t)
{
    df_file_clear(&t->a);
    df_file_clear(&t->b);
}
