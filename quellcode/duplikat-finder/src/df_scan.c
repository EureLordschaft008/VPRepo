/*
 * Duplikat-Finder – Suchlauf: Ordner lesen, Vergleichsbilder holen, vergleichen
 * =============================================================================
 * 1. Ordner lesen (Filter: Endungen, Größe, Pfadtexte, Links, ignorierte Dateien)
 * 2. Neue oder geänderte Dateien von Hilfsprozessen lesen lassen – CPU- und
 *    GPU-Prozesse gleichzeitig aus einer gemeinsamen Warteschlange. Videos, die
 *    die GPU nicht dekodieren kann, gehen an die CPU. Abstürze, Hänger und
 *    Speicherfresser treffen nur den Hilfsprozess: er wird neu gestartet, die
 *    Datei einmal auf der anderen Hardware versucht und sonst als nicht
 *    lesbar gemerkt. Ergebnisse landen sofort (paketweise) in der Datenbank.
 *    "Rücksicht": braucht ein anderes Programm die CPU oder wird der Speicher
 *    knapp, pausieren einzelne CPU-Prozesse, bis wieder Luft ist.
 * 3. Teilclips (optional): Ton-Fingerabdrücke holen (Chromaprint)
 * 4. Vergleich: jedes Video mit jedem (innerhalb der erlaubten
 *    Laufzeitabweichung), mehrere Threads, 64-Bit-pHash je Vergleichsbild ->
 *    Paare -> Gruppen. 100 000 Videos ~ 5·10⁹ Paare: wenige Sekunden.
 * 5. Teilclips: Ton-Index (Stichprobe der Fingerabdruck-Werte), Versatz-
 *    Abstimmung, Prüfung Bit für Bit, auf Wunsch Bildprüfung am Versatz.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "df_internal.h"

#define SCAN_TIMEOUT 150        /* s je Datei (Vergleichsbilder) */
#define CPU_RSS_LIMIT 2500      /* MB je Hilfsprozess */
#define GPU_RSS_LIMIT 5000
#define MIN_FREE_MB 1200        /* darunter starten keine neuen Dateien */
#define COMMIT_SECS 2.0
#define FP_ITEM_SECS 0.12384    /* Chromaprint: Abstand der Fingerabdruck-Werte (11025 Hz) */

/* ---------------------------------------------------------------------------
 * Gemeinsamer Zustand (Fortschritt, Jobs, Pause)
 * ------------------------------------------------------------------------- */
typedef struct {
    char path[1024];
    bool gpu, busy, held;
    double started;
} Slot;

static struct {
    GMutex lock;
    Slot slot[MAX_CPU_WORKERS + MAX_GPU_WORKERS];
    int nslots;
    bool running;
    int done, total, gpu_done, cpu_done, failed;
    double t0;
    /* Tempo: Zeitstempel der letzten fertigen Dateien */
    double hist_t[256];
    bool hist_gpu[256];
    int hist_n, hist_pos;
    double cpu_load;
    int active_cpu, held_cpu;
} S;

static atomic_bool g_paused;
static atomic_int g_allowed_cpu;        /* so viele CPU-Prozesse dürfen neue Dateien nehmen */
static atomic_bool g_mem_low;

void df_set_paused(bool on)
{
    atomic_store(&g_paused, on);
}

bool df_paused(void)
{
    return atomic_load(&g_paused);
}

int df_jobs_snapshot(DfJobInfo *out, int max)
{
    g_mutex_lock(&S.lock);
    int n = 0;
    double now = df_now();
    for (int i = 0; i < S.nslots && n < max; i++) {
        DfJobInfo *j = &out[n++];
        g_strlcpy(j->path, S.slot[i].path, sizeof j->path);
        j->gpu = S.slot[i].gpu;
        j->busy = S.slot[i].busy;
        j->held = S.slot[i].held;
        j->secs = S.slot[i].busy ? now - S.slot[i].started : 0;
    }
    g_mutex_unlock(&S.lock);
    return n;
}

bool df_progress(DfProgress *p)
{
    memset(p, 0, sizeof *p);
    g_mutex_lock(&S.lock);
    bool running = S.running;
    p->done = S.done;
    p->total = S.total;
    p->gpu_done = S.gpu_done;
    p->cpu_done = S.cpu_done;
    p->failed = S.failed;
    p->cpu_load = S.cpu_load;
    p->active_cpu = S.active_cpu;
    p->held_cpu = S.held_cpu;
    double now = df_now();
    /* Tempo der letzten 60 s (bzw. der letzten 256 Dateien) */
    int n = 0, ng = 0;
    double oldest = now;
    for (int i = 0; i < S.hist_n; i++) {
        double t = S.hist_t[i];
        if (now - t > 60) continue;
        n++;
        if (S.hist_gpu[i]) ng++;
        if (t < oldest) oldest = t;
    }
    double span = fmax(now - oldest, fmin(now - S.t0, 60));
    if (span > 1 && n > 0) {
        p->files_per_sec = n / span;
        p->gpu_per_sec = ng / span;
        p->cpu_per_sec = (n - ng) / span;
    }
    p->eta = p->files_per_sec > 0 ? (p->total - p->done) / p->files_per_sec : -1;
    g_mutex_unlock(&S.lock);
    return running;
}

static void progress_file_done(bool gpu, bool ok)
{
    g_mutex_lock(&S.lock);
    S.done++;
    if (!ok) S.failed++;
    else if (gpu) S.gpu_done++;
    else S.cpu_done++;
    S.hist_t[S.hist_pos] = df_now();
    S.hist_gpu[S.hist_pos] = gpu;
    S.hist_pos = (S.hist_pos + 1) % 256;
    if (S.hist_n < 256) S.hist_n++;
    g_mutex_unlock(&S.lock);
}

static void progress_reset(int total, int nslots)
{
    g_mutex_lock(&S.lock);
    memset(S.slot, 0, sizeof S.slot);
    S.nslots = nslots;
    S.running = true;
    S.done = S.gpu_done = S.cpu_done = S.failed = 0;
    S.total = total;
    S.t0 = df_now();
    S.hist_n = S.hist_pos = 0;
    g_mutex_unlock(&S.lock);
}

static void set_slot(int i, const char *path, bool gpu, bool busy, bool held)
{
    g_mutex_lock(&S.lock);
    Slot *s = &S.slot[i];
    if (path) g_strlcpy(s->path, path, sizeof s->path);
    else if (!busy) s->path[0] = 0;
    s->gpu = gpu;
    if (busy && !s->busy) s->started = df_now();
    s->busy = busy;
    s->held = held;
    g_mutex_unlock(&S.lock);
}

/* ---------------------------------------------------------------------------
 * 1. Ordner lesen
 * ------------------------------------------------------------------------- */
typedef struct {
    char *path;
    long long size, mtime, inode;
    bool image;
} Item;

typedef struct {
    const DfOptions *o;
    GArray *items;              /* Item */
    GHashTable *seen;           /* Pfade (überlappende Ordner) */
    GHashTable *inodes;         /* dev:inode (Hardlinks) */
    GHashTable *dirs;           /* besuchte Ordner dev:inode (Link-Schleifen) */
    GHashTable *ignored;
    char **excludes;            /* kanonische Pfade */
    int nexcludes;
    double last_report;
} Walk;

static bool is_excluded_dir(Walk *w, const char *path)
{
    for (int i = 0; i < w->nexcludes; i++) {
        size_t n = strlen(w->excludes[i]);
        if (!strncmp(path, w->excludes[i], n) && (path[n] == 0 || path[n] == '/')) return true;
    }
    return false;
}

static bool path_passes(const DfOptions *o, const char *path)
{
    for (int i = 0; i < o->path_excludes.n; i++)
        if (df_glob_or_contains(path, o->path_excludes.v[i])) return false;
    if (!o->path_contains.n) return true;
    for (int i = 0; i < o->path_contains.n; i++)
        if (df_glob_or_contains(path, o->path_contains.v[i])) return true;
    return false;
}

static void walk_dir(Walk *w, const char *root)
{
    GPtrArray *stack = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(stack, g_strdup(root));
    while (stack->len && !df_cancel) {
        char *dir = g_ptr_array_steal_index(stack, stack->len - 1);
        int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        DIR *d = dfd >= 0 ? fdopendir(dfd) : NULL;
        if (!d) {
            if (dfd >= 0) close(dfd);
            df_log(false, "Ordner nicht lesbar: %s (%s)", dir, g_strerror(errno));
            g_free(dir);
            continue;
        }
        struct dirent *de;
        while ((de = readdir(d)) && !df_cancel) {
            const char *name = de->d_name;
            if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) continue;
            if (w->o->skip_hidden && name[0] == '.') continue;
            struct stat st;
            if (fstatat(dfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
            if (S_ISLNK(st.st_mode)) {
                if (w->o->skip_symlinks || fstatat(dfd, name, &st, 0) != 0) continue;
            }
            char *path = g_build_filename(dir, name, NULL);
            if (S_ISDIR(st.st_mode)) {
                char key[48];
                snprintf(key, sizeof key, "%llx:%llx", (unsigned long long)st.st_dev,
                         (unsigned long long)st.st_ino);
                if (w->o->subdirs && !is_excluded_dir(w, path) &&
                    !g_hash_table_contains(w->dirs, key)) {
                    g_hash_table_add(w->dirs, g_strdup(key));
                    g_ptr_array_add(stack, path);
                    path = NULL;
                }
                g_free(path);
                continue;
            }
            if (!S_ISREG(st.st_mode)) {
                g_free(path);
                continue;
            }
            bool video = df_is_video_ext(name), image = !video && df_is_image_ext(name);
            if ((!video && !(image && w->o->images)) ||
                (w->o->min_size > 0 && st.st_size < w->o->min_size) ||
                (w->o->max_size > 0 && st.st_size > w->o->max_size) || st.st_size == 0 ||
                g_hash_table_contains(w->seen, path) || g_hash_table_contains(w->ignored, path) ||
                !path_passes(w->o, path)) {
                g_free(path);
                continue;
            }
            if (w->o->skip_hardlinks && st.st_nlink > 1) {
                char key[48];
                snprintf(key, sizeof key, "%llx:%llx", (unsigned long long)st.st_dev,
                         (unsigned long long)st.st_ino);
                if (g_hash_table_contains(w->inodes, key)) {
                    g_free(path);
                    continue;
                }
                g_hash_table_add(w->inodes, g_strdup(key));
            }
            Item it = {path, st.st_size, st.st_mtime, (long long)st.st_ino, image};
            g_hash_table_add(w->seen, path);
            g_array_append_val(w->items, it);
            double now = df_now();
            if (now - w->last_report > 0.25) {
                w->last_report = now;
                if (df_fe.progress) df_fe.progress(DF_PHASE_WALK, (int)w->items->len, -1);
            }
        }
        closedir(d);
        g_free(dir);
    }
    g_ptr_array_free(stack, TRUE);
}

static int cmp_item_path(const void *a, const void *b)
{
    return strcmp(((const Item *)a)->path, ((const Item *)b)->path);
}

/* -> Array von Item (sortiert nach Pfad) */
static GArray *walk_all(const DfOptions *o)
{
    Walk w = {.o = o};
    w.items = g_array_new(FALSE, FALSE, sizeof(Item));
    w.seen = g_hash_table_new(g_str_hash, g_str_equal);
    w.inodes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    w.dirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    w.ignored = df_db_ignored_load();
    w.excludes = g_new0(char *, (gsize)o->excludes.n + 1);
    for (int i = 0; i < o->excludes.n; i++) {
        char *c = g_canonicalize_filename(o->excludes.v[i], NULL);
        size_t n = strlen(c);
        while (n > 1 && c[n - 1] == '/') c[--n] = 0;
        w.excludes[w.nexcludes++] = c;
    }
    for (int i = 0; i < o->includes.n && !df_cancel; i++) {
        char *root = g_canonicalize_filename(o->includes.v[i], NULL);
        if (is_excluded_dir(&w, root)) {
            g_free(root);
            continue;
        }
        if (!g_file_test(root, G_FILE_TEST_IS_DIR)) {
            df_log(true, "Ordner nicht gefunden: %s", root);
            g_free(root);
            continue;
        }
        struct stat st;
        if (stat(root, &st) == 0) {
            char key[48];
            snprintf(key, sizeof key, "%llx:%llx", (unsigned long long)st.st_dev,
                     (unsigned long long)st.st_ino);
            g_hash_table_add(w.dirs, g_strdup(key));
        }
        walk_dir(&w, root);
        g_free(root);
    }
    g_hash_table_destroy(w.seen);
    g_hash_table_destroy(w.inodes);
    g_hash_table_destroy(w.dirs);
    g_hash_table_destroy(w.ignored);
    g_strfreev(w.excludes);
    g_array_sort(w.items, cmp_item_path);
    return w.items;
}

static void items_free(GArray *a)
{
    for (guint i = 0; i < a->len; i++) g_free(g_array_index(a, Item, i).path);
    g_array_free(a, TRUE);
}

/* ---------------------------------------------------------------------------
 * 2. Vergleichsbilder lesen (Hilfsprozesse)
 * ------------------------------------------------------------------------- */
typedef enum { JOB_SCAN, JOB_AUDIO } JobKind;

typedef struct {
    JobKind kind;
    const DfOptions *o;
    const DfGpu *gpu;
    /* Aufgaben */
    Item **todo;                /* JOB_SCAN */
    DfFile **afiles;            /* JOB_AUDIO */
    int ntodo;
    atomic_int next;            /* nächste Aufgabe aus der Hauptliste */
    GMutex lock;
    GQueue cpu_queue;           /* GPU kann sie nicht: Index in todo */
    int cpu_threads, gpu_threads;
    atomic_int pending;         /* noch nicht fertige Aufgaben */
    GAsyncQueue *results;       /* fertige Einträge (Result*) */
} Pool;

typedef struct {
    int index;
    bool gpu, ok;
    DfDbEntry *e;               /* JOB_SCAN */
    uint32_t *fp;               /* JOB_AUDIO */
    int nfp, status;
    char *error;
} Result;

typedef struct {
    Pool *pool;
    int slot;
    bool gpu;
} Worker;

/* nächste Aufgabe -> Index oder -1 (fertig) */
static int take_job(Worker *wk, bool *from_cpu_queue)
{
    Pool *p = wk->pool;
    *from_cpu_queue = false;
    while (!df_cancel) {
        /* alles verteilt und (für CPU-Prozesse) keine Rückläufer der GPU mehr zu erwarten? */
        if (atomic_load(&p->next) >= p->ntodo && (wk->gpu || atomic_load(&p->pending) <= 0)) {
            g_mutex_lock(&p->lock);
            bool empty = g_queue_is_empty(&p->cpu_queue);
            g_mutex_unlock(&p->lock);
            if (empty) return -1;
        }
        bool held = false;
        if (atomic_load(&g_paused)) held = true;
        else if (!wk->gpu && wk->slot >= atomic_load(&g_allowed_cpu)) held = true;
        else if (atomic_load(&g_mem_low) && wk->slot > 0) held = true;
        if (held) {
            set_slot(wk->slot, NULL, wk->gpu, false, true);
            g_usleep(200000);
            continue;
        }
        if (!wk->gpu || p->kind == JOB_AUDIO) {
            g_mutex_lock(&p->lock);
            gpointer x = g_queue_pop_head(&p->cpu_queue);
            g_mutex_unlock(&p->lock);
            if (x) {
                *from_cpu_queue = true;
                return GPOINTER_TO_INT(x) - 1;
            }
        }
        int i = atomic_fetch_add(&p->next, 1);
        if (i < p->ntodo) return i;
        atomic_store(&p->next, p->ntodo);
        /* Hauptliste leer: CPU-Prozesse warten noch auf Rückläufer der GPU */
        if (wk->gpu || atomic_load(&p->pending) <= 0) return -1;
        g_usleep(100000);
    }
    return -1;
}

static const char *status_text(DfReplyStatus st)
{
    switch (st) {
    case DF_R_TIMEOUT: return "Zeitüberschreitung (Datei hängt beim Dekodieren)";
    case DF_R_MEMORY: return "brauchte zu viel Arbeitsspeicher";
    case DF_R_DEAD: return "Hilfsprozess abgestürzt (Datei defekt?)";
    default: return "Fehler";
    }
}

static gpointer worker_thread(gpointer data)
{
    Worker *wk = data;
    Pool *p = wk->pool;
    const DfOptions *o = p->o;
    DfProc proc, cpu_proc;                      /* cpu_proc: GPU-Thread ohne CPU-Prozesse */
    memset(&proc, 0, sizeof proc);
    memset(&cpu_proc, 0, sizeof cpu_proc);
    proc.pid = cpu_proc.pid = -1;
    int nice_level = o->considerate ? 15 : 5;
    while (!df_cancel) {
        bool from_cpu_queue;
        int idx = take_job(wk, &from_cpu_queue);
        if (idx < 0) break;
        const char *path = p->kind == JOB_SCAN ? p->todo[idx]->path : p->afiles[idx]->path;
        set_slot(wk->slot, path, wk->gpu, true, false);
        Result *res = g_new0(Result, 1);
        res->index = idx;
        bool done = false;
        /* bis zu 2 Versuche: abgestürzt/hängt -> einmal neu (GPU: dann per CPU) */
        for (int attempt = 0; attempt < 2 && !done && !df_cancel; attempt++) {
            bool use_gpu = wk->gpu && attempt == 0 && p->kind == JOB_SCAN;
            DfProc *pr = use_gpu || !wk->gpu ? &proc : &cpu_proc;
            if (pr->pid <= 0 &&
                !df_proc_start(pr, use_gpu ? p->gpu->device : NULL, nice_level, o->white_borders)) {
                res->error = g_strdup("Hilfsprozess nicht startbar");
                break;
            }
            DfReply r;
            DfReplyStatus st;
            if (p->kind == JOB_SCAN) {
                char cmd[32];
                snprintf(cmd, sizeof cmd, "S\t%d", o->frames);
                st = df_proc_request(pr, cmd, path, p->todo[idx]->image ? 40 : SCAN_TIMEOUT,
                                     use_gpu ? GPU_RSS_LIMIT : CPU_RSS_LIMIT, &r);
            } else {
                double secs = 120 + p->afiles[idx]->size / 25e6;   /* ganze Datei lesen */
                st = df_proc_request(pr, "A\t0", path, secs, CPU_RSS_LIMIT, &r);
            }
            if (df_cancel) {
                df_reply_free(&r);
                break;
            }
            if (st == DF_R_SW) {                /* GPU kann es nicht -> an die CPU */
                df_reply_free(&r);
                if (p->cpu_threads > 0) {
                    g_mutex_lock(&p->lock);
                    g_queue_push_tail(&p->cpu_queue, GINT_TO_POINTER(idx + 1));
                    g_mutex_unlock(&p->lock);
                    g_free(res);
                    res = NULL;
                    done = true;
                } /* sonst: zweiter Versuch mit eigenem CPU-Prozess */
                continue;
            }
            if (st == DF_R_OK) {
                res->ok = true;
                res->gpu = use_gpu;
                if (p->kind == JOB_SCAN) {
                    DfDbEntry *e = g_new0(DfDbEntry, 1);
                    e->f.path = g_strdup(path);
                    e->f.size = p->todo[idx]->size;
                    e->f.mtime = p->todo[idx]->mtime;
                    e->inode = p->todo[idx]->inode;
                    e->k = o->frames;
                    e->white_borders = o->white_borders;
                    if (!df_parse_scan_reply(&r, &e->f)) {
                        e->status = 1;
                        e->error = g_strdup("unvollständige Antwort");
                        res->ok = false;
                    }
                    res->e = e;
                } else {
                    char b[32];
                    double secs = df_kv(r.text, "seconds", b, sizeof b) ? g_ascii_strtod(b, NULL) : 0;
                    double silent = df_kv(r.text, "silent", b, sizeof b) ? g_ascii_strtod(b, NULL) : 0;
                    res->nfp = (int)(r.bin_len / 4);
                    res->fp = (uint32_t *)g_memdup2(r.bin, r.bin_len);
                    res->status = secs > 0 && silent > secs * 0.9 ? 2 : 0;   /* 2 = stumm */
                }
                done = true;
            } else if (st == DF_R_ERR) {
                res->error = g_strdup(r.text && *r.text ? r.text : "nicht lesbar");
                done = true;
            } else {                            /* abgestürzt, hängt, zu viel Speicher */
                g_free(res->error);
                res->error = g_strdup(status_text(st));
                df_log(false, "%s: %s (%s, Versuch %d)", path, res->error,
                       use_gpu ? "GPU" : "CPU", attempt + 1);
                /* hängt sie schon auf der CPU, hilft ein zweiter CPU-Versuch nicht */
                if (!use_gpu && st != DF_R_DEAD) done = true;
            }
            df_reply_free(&r);
        }
        if (res) {
            if (!done && !res->error) res->error = g_strdup("abgebrochen");
            if (p->kind == JOB_SCAN && !res->e) {
                DfDbEntry *e = g_new0(DfDbEntry, 1);
                e->f.path = g_strdup(path);
                e->f.size = p->todo[idx]->size;
                e->f.mtime = p->todo[idx]->mtime;
                e->inode = p->todo[idx]->inode;
                e->k = o->frames;
                e->white_borders = o->white_borders;
                e->status = 1;
                e->error = g_strdup(res->error);
                res->e = e;
            }
            if (df_cancel && !done) {           /* abgebrochen: nichts merken */
                if (res->e) df_db_entry_free(res->e);
                g_free(res->fp);
                g_free(res->error);
                g_free(res);
            } else {
                g_async_queue_push(p->results, res);
            }
            atomic_fetch_sub(&p->pending, 1);
        }
        set_slot(wk->slot, NULL, wk->gpu, false, false);
    }
    df_proc_stop(&proc);
    df_proc_stop(&cpu_proc);
    set_slot(wk->slot, NULL, wk->gpu, false, false);
    return NULL;
}

/* Rücksicht auf andere Programme: CPU-Last ohne die eigenen Hilfsprozesse messen */
typedef struct {
    unsigned long long busy, total;
    double own;
    bool valid;
} LoadSample;

static double own_cpu_secs(void)
{
    /* Kindprozesse dieses Programms: alle Hilfsprozesse (über /proc/self/task/.../children) */
    double sum = 0;
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/task", (int)getpid());
    DIR *d = opendir(path);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        char cp[320];
        snprintf(cp, sizeof cp, "/proc/%d/task/%s/children", (int)getpid(), de->d_name);
        FILE *f = fopen(cp, "r");
        if (!f) continue;
        int pid;
        while (fscanf(f, "%d", &pid) == 1) {
            double s = df_proc_cpu_secs(pid);
            if (s > 0) sum += s;
        }
        fclose(f);
    }
    closedir(d);
    return sum;
}

static void governor_tick(const DfOptions *o, LoadSample *prev, int cpu_workers)
{
    long avail = df_mem_available_mb();
    atomic_store(&g_mem_low, avail >= 0 && avail < MIN_FREE_MB);
    LoadSample cur = {0};
    if (!df_read_cpu_times(&cur.busy, &cur.total)) return;
    cur.own = own_cpu_secs();
    cur.valid = true;
    if (prev->valid && cur.total > prev->total) {
        double ncpu = df_cpu_count();
        double hz = (double)sysconf(_SC_CLK_TCK);
        double dt_all = (double)(cur.total - prev->total) / hz / ncpu;   /* Sekunden Wanduhr */
        double busy_cores = (double)(cur.busy - prev->busy) / hz / fmax(dt_all, 1e-3);
        double own_cores = (cur.own - prev->own) / fmax(dt_all, 1e-3);
        if (own_cores < 0) own_cores = 0;
        double other = busy_cores - own_cores;
        if (other < 0) other = 0;
        int allowed = cpu_workers;
        if (o->considerate) {
            /* je ~voll ausgelastetem Kern eines anderen Programms ein CPU-Prozess weniger;
             * ein Kern bleibt immer für Oberfläche und System */
            int free_cores = (int)floor(ncpu - other - 1 + 0.3);
            allowed = free_cores < cpu_workers ? free_cores : cpu_workers;
            if (allowed < 1) allowed = 1;
        }
        atomic_store(&g_allowed_cpu, allowed);
        g_mutex_lock(&S.lock);
        S.cpu_load = 100.0 * busy_cores / ncpu;
        S.active_cpu = allowed;
        S.held_cpu = cpu_workers - allowed;
        g_mutex_unlock(&S.lock);
    }
    *prev = cur;
}

/* Pool laufen lassen; on_result wird im aufrufenden Thread je Ergebnis aufgerufen */
static void run_pool(Pool *p, const DfOptions *o, const DfGpu *gpu, void (*on_result)(Result *, void *),
                     void *ud)
{
    int cpu = o->cpu_workers, gpuw = gpu && gpu->available ? o->gpu_workers : 0;
    if (p->kind == JOB_AUDIO) {                 /* Ton dekodiert immer die CPU */
        cpu = o->cpu_workers + gpuw;
        gpuw = 0;
        if (cpu > df_cpu_count()) cpu = df_cpu_count();
    }
    if (cpu + gpuw == 0) cpu = 1;
    if (cpu > MAX_CPU_WORKERS) cpu = MAX_CPU_WORKERS;
    if (gpuw > MAX_GPU_WORKERS) gpuw = MAX_GPU_WORKERS;
    p->cpu_threads = cpu;
    p->gpu_threads = gpuw;
    atomic_store(&p->next, 0);
    atomic_store(&p->pending, p->ntodo);
    p->results = g_async_queue_new();
    g_queue_init(&p->cpu_queue);
    atomic_store(&g_allowed_cpu, cpu);
    atomic_store(&g_mem_low, false);
    int n = cpu + gpuw;
    progress_reset(p->ntodo, n);
    Worker *wk = g_new0(Worker, n);
    GThread **th = g_new0(GThread *, n);
    /* Slots: erst die CPU-Prozesse (0..cpu-1, gedrosselt von hinten), dann die GPU */
    for (int i = 0; i < n; i++) {
        wk[i] = (Worker){p, i, i >= cpu};
        set_slot(i, NULL, wk[i].gpu, false, false);
        th[i] = g_thread_new(wk[i].gpu ? "gpu" : "cpu", worker_thread, &wk[i]);
    }
    LoadSample load = {0};
    double next_gov = 0;
    int received = 0;
    bool workers_alive = true;
    while (workers_alive) {
        Result *r = g_async_queue_timeout_pop(p->results, 200000);
        if (r) {
            received++;
            progress_file_done(r->gpu, r->ok);
            on_result(r, ud);
            if (df_fe.progress) df_fe.progress(p->kind == JOB_SCAN ? DF_PHASE_SCAN : DF_PHASE_AUDIO,
                                               S.done, p->ntodo);
        }
        double now = df_now();
        if (now >= next_gov) {
            next_gov = now + 2;
            governor_tick(o, &load, cpu);
        }
        if (!r && (atomic_load(&p->pending) <= 0 || df_cancel)) {
            /* alle fertig? (Ergebnisse können noch unterwegs sein) */
            workers_alive = false;
            for (int i = 0; i < n; i++) {
                if (th[i]) {
                    g_thread_join(th[i]);
                    th[i] = NULL;
                }
            }
            while ((r = g_async_queue_try_pop(p->results))) {
                progress_file_done(r->gpu, r->ok);
                on_result(r, ud);
            }
        }
    }
    g_async_queue_unref(p->results);
    g_queue_clear(&p->cpu_queue);
    g_mutex_lock(&S.lock);
    S.running = false;
    g_mutex_unlock(&S.lock);
    g_free(wk);
    g_free(th);
}

typedef struct {
    GHashTable *db;             /* Pfad -> DfDbEntry* */
    double last_commit;
    bool in_tx;
    DfStats *st;
} ScanCtx;

static void on_scan_result(Result *r, void *ud)
{
    ScanCtx *c = ud;
    DfDbEntry *e = r->e;
    if (!c->in_tx) {
        df_db_begin();
        c->in_tx = true;
    }
    e->f.id = df_db_put(e);
    if (r->ok && e->status == 0) {
        c->st->scanned++;
        if (r->gpu) c->st->gpu_files++;
        else c->st->cpu_files++;
    } else {
        c->st->failed++;
        df_log(true, "Nicht lesbar: %s – %s", e->f.path, e->error ? e->error : "?");
    }
    g_hash_table_replace(c->db, e->f.path, e);
    double now = df_now();
    if (now - c->last_commit > COMMIT_SECS) {
        df_db_commit();
        c->in_tx = false;
        c->last_commit = now;
    }
    g_free(r->error);
    g_free(r);
}

typedef struct {
    Pool *pool;
    DfStats *st;
} AudioCtx;

static void on_audio_result(Result *r, void *ud)
{
    AudioCtx *c = ud;
    DfFile *f = c->pool->afiles[r->index];
    int status = r->ok ? r->status : 1;
    df_db_audio_put(f->id, f->size, f->mtime, r->fp, r->nfp, status, r->error);
    if (r->ok) c->st->audio_scanned++;
    else df_log(false, "Kein Ton-Fingerabdruck: %s – %s", f->path, r->error ? r->error : "?");
    g_free(r->fp);
    g_free(r->error);
    g_free(r);
}

/* ---------------------------------------------------------------------------
 * 4. Vergleich
 * ------------------------------------------------------------------------- */
typedef struct {
    int n, k;
    int *file;                  /* Index in files */
    uint64_t *h, *hf;           /* n*k */
    uint32_t *valid, *flat;     /* Bitmasken je Video */
    uint8_t *mean;              /* n*k */
    float *dur;
    int *dir;                   /* Ordner-Nummer */
    bool *is_new;
    int64_t *id;
} Set;

typedef struct {
    int a, b;                   /* Index in files */
    float sim;
    bool flip, partial;
    double offset;              /* Teilclip: b beginnt bei offset s in a */
} Pair;

/* Ähnlichkeit in % (oder -1) zweier Vergleichsbild-Reihen */
static double frames_similarity(const DfFrame *a, const DfFrame *b, int k, bool flip)
{
    int sum = 0, mx = 0, n = 0;
    for (int i = 0; i < k; i++) {
        bool ao = a[i].flags & DF_FRAME_OK, bo = b[i].flags & DF_FRAME_OK;
        if (!ao || !bo) continue;
        bool af = a[i].flags & DF_FRAME_FLAT, bf = b[i].flags & DF_FRAME_FLAT;
        int d;
        if (af && bf) {
            if (abs(a[i].mean - b[i].mean) <= 40) continue;   /* beide Schwarzbild: neutral */
            d = 32;
        } else if (af || bf) {
            d = 32;
        } else {
            d = __builtin_popcountll(a[i].hash ^ (flip ? b[i].hash_flip : b[i].hash));
        }
        sum += d;
        if (d > mx) mx = d;
        n++;
    }
    int need = k == 1 ? 1 : k < 4 ? 2 : (k + 1) / 2;
    if (n < need) return -1;
    if (k >= 5 && n >= 4) {                     /* ein abweichendes Bild erlaubt */
        sum -= mx;
        n--;
    }
    return 100.0 * (1.0 - (double)sum / n / 64.0);
}

double df_similarity(const DfFile *a, const DfFile *b, bool flip)
{
    if (!a->nframes || a->nframes != b->nframes) return -1;
    return frames_similarity(a->frames, b->frames, a->nframes, flip);
}

static void set_init(Set *s, int n, int k)
{
    memset(s, 0, sizeof *s);
    s->n = n;
    s->k = k;
    s->file = g_new(int, n);
    s->h = g_new(uint64_t, (gsize)n * k);
    s->hf = g_new(uint64_t, (gsize)n * k);
    s->valid = g_new0(uint32_t, n);
    s->flat = g_new0(uint32_t, n);
    s->mean = g_new0(uint8_t, (gsize)n * k);
    s->dur = g_new(float, n);
    s->dir = g_new(int, n);
    s->is_new = g_new(bool, n);
    s->id = g_new(int64_t, n);
}

static void set_free(Set *s)
{
    g_free(s->file);
    g_free(s->h);
    g_free(s->hf);
    g_free(s->valid);
    g_free(s->flat);
    g_free(s->mean);
    g_free(s->dur);
    g_free(s->dir);
    g_free(s->is_new);
    g_free(s->id);
}

typedef struct {
    const Set *s;
    const DfOptions *o;
    GHashTable *notmatch;
    atomic_int next_row;
    double max_dist;            /* mittlere Bitdistanz für die Schwelle */
    int need;
    int done_rows;
    atomic_llong pairs;
} CmpCtx;

typedef struct {
    CmpCtx *c;
    GArray *out;                /* Pair */
} CmpThread;

/* Distanz-Prüfung eines Paars (Indizes im Set) -> Ähnlichkeit oder -1 */
static inline double pair_check(const Set *s, int a, int b, bool flip, double max_dist, int need)
{
    const int k = s->k;
    const uint64_t *ha = s->h + (size_t)a * k;
    const uint64_t *hb = (flip ? s->hf : s->h) + (size_t)b * k;
    uint32_t va = s->valid[a], vb = s->valid[b], fa = s->flat[a], fb = s->flat[b];
    uint32_t both = va & vb;
    int sum = 0, mx = 0, n = 0;
    /* Früher Abbruch: (Summe − größter Abstand) wächst nur – liegt sie schon nach 3 Bildern
     * über der Grenze für alle k Bilder, wird es kein Treffer (bei fremden Videos fast immer) */
    const double early = k >= 5 ? max_dist * (k - 1) : max_dist * k;
    for (int i = 0; i < k; i++) {
        if (both >> i & 1) {
            int d = __builtin_popcountll(ha[i] ^ hb[i]);
            sum += d;
            mx = d > mx ? d : mx;
            if (++n == 3 && (k >= 5 ? sum - mx : sum) > early) return -1;
        }
    }
    uint32_t cross = (fa & vb) | (fb & va);     /* einfarbig gegen Bildinhalt */
    if (cross) {
        int c = __builtin_popcount(cross);
        sum += 32 * c;
        n += c;
        mx = 32;
    }
    uint32_t bothflat = fa & fb;
    while (bothflat) {
        int i = __builtin_ctz(bothflat);
        bothflat &= bothflat - 1;
        if (abs(s->mean[(size_t)a * k + i] - s->mean[(size_t)b * k + i]) > 40) {
            sum += 32;
            n++;
            mx = 32;
        }
    }
    if (n < need) return -1;
    if (k >= 5 && n >= 4) {
        sum -= mx;
        n--;
    }
    if (sum > max_dist * n) return -1;
    return 100.0 * (1.0 - (double)sum / n / 64.0);
}

static gpointer compare_thread(gpointer data)
{
    CmpThread *t = data;
    CmpCtx *c = t->c;
    const Set *s = c->s;
    const DfOptions *o = c->o;
    double tol = o->duration_diff >= 100 ? -1 : o->duration_diff / 100.0;
    long long pairs = 0;
    enum { BLOCK = 32 };
    while (!df_cancel) {
        int start = atomic_fetch_add(&c->next_row, BLOCK);
        if (start >= s->n) break;
        int end = MIN(start + BLOCK, s->n);
        for (int a = start; a < end; a++) {
            float limit = tol < 0 ? INFINITY : s->dur[a] * (float)(1 + tol) + 0.5f;
            for (int b = a + 1; b < s->n; b++) {
                if (s->dur[b] > limit) break;   /* nach Laufzeit sortiert */
                if (!s->is_new[a] && !s->is_new[b]) continue;
                if (o->folder_mode == DF_FOLDERS_SAME && s->dir[a] != s->dir[b]) continue;
                if (o->folder_mode == DF_FOLDERS_DIFFERENT && s->dir[a] == s->dir[b]) continue;
                pairs++;
                bool flip = false;
                double sim = pair_check(s, a, b, false, c->max_dist, c->need);
                if (sim < 0 && o->flipped) {
                    sim = pair_check(s, a, b, true, c->max_dist, c->need);
                    flip = sim >= 0;
                }
                if (sim < 0) continue;
                if (c->notmatch) {
                    gint64 key = df_pair_key(s->id[a], s->id[b]);
                    if (g_hash_table_contains(c->notmatch, &key)) continue;
                }
                Pair pr = {s->file[a], s->file[b], (float)sim, flip, false, 0};
                g_array_append_val(t->out, pr);
            }
        }
        if (df_fe.progress && start % (BLOCK * 64) == 0)
            df_fe.progress(DF_PHASE_COMPARE, start, s->n);
    }
    atomic_fetch_add(&c->pairs, pairs);
    return NULL;
}

static int cmp_by_dur(const void *x, const void *y, void *ud)
{
    const float *dur = ud;
    float a = dur[*(const int *)x], b = dur[*(const int *)y];
    return (a > b) - (a < b);
}

/* Paare eines Sets suchen */
static void compare_set(const DfOptions *o, DfFile *files, const int *idx, int n, int k,
                        const bool *is_new, GHashTable *dirs_map, GHashTable *notmatch,
                        GArray *pairs, DfStats *st)
{
    if (n < 2) return;
    /* nach Laufzeit sortieren */
    float *durs = g_new(float, n);
    int *order = g_new(int, n);
    for (int i = 0; i < n; i++) {
        durs[i] = (float)files[idx[i]].duration;
        order[i] = i;
    }
    g_qsort_with_data(order, n, sizeof *order, cmp_by_dur, durs);
    Set s;
    set_init(&s, n, k);
    for (int j = 0; j < n; j++) {
        int i = order[j];
        const DfFile *f = &files[idx[i]];
        s.file[j] = idx[i];
        s.dur[j] = durs[i];
        s.is_new[j] = is_new ? is_new[idx[i]] : true;
        s.id[j] = f->id;
        char *dir = g_path_get_dirname(f->path);
        gpointer v = g_hash_table_lookup(dirs_map, dir);
        if (!v) {
            v = GINT_TO_POINTER(g_hash_table_size(dirs_map) + 1);
            g_hash_table_insert(dirs_map, dir, v);
        } else {
            g_free(dir);
        }
        s.dir[j] = GPOINTER_TO_INT(v);
        for (int q = 0; q < k; q++) {
            const DfFrame *fr = &f->frames[q];
            s.h[(size_t)j * k + q] = fr->hash;
            s.hf[(size_t)j * k + q] = fr->hash_flip;
            s.mean[(size_t)j * k + q] = fr->mean;
            if (fr->flags & DF_FRAME_OK) {
                if (fr->flags & DF_FRAME_FLAT) s.flat[j] |= 1u << q;
                else s.valid[j] |= 1u << q;
            }
        }
    }
    g_free(durs);
    g_free(order);
    CmpCtx c = {.s = &s, .o = o, .notmatch = g_hash_table_size(notmatch) ? notmatch : NULL};
    c.max_dist = 64.0 * (1.0 - o->similarity / 100.0);
    c.need = k == 1 ? 1 : k < 4 ? 2 : (k + 1) / 2;
    int nt = df_cpu_count();
    CmpThread *t = g_new0(CmpThread, nt);
    GThread **th = g_new0(GThread *, nt);
    for (int i = 0; i < nt; i++) {
        t[i].c = &c;
        t[i].out = g_array_new(FALSE, FALSE, sizeof(Pair));
        th[i] = g_thread_new("compare", compare_thread, &t[i]);
    }
    for (int i = 0; i < nt; i++) {
        g_thread_join(th[i]);
        g_array_append_vals(pairs, t[i].out->data, t[i].out->len);
        g_array_free(t[i].out, TRUE);
    }
    st->pairs += atomic_load(&c.pairs);
    g_free(t);
    g_free(th);
    set_free(&s);
}

/* ---------------------------------------------------------------------------
 * 5. Teilclips über den Ton
 * ------------------------------------------------------------------------- */
typedef struct {
    uint32_t v, file, pos;
} FpEntry;

static int cmp_fp_entry(const void *x, const void *y)
{
    const FpEntry *a = x, *b = y;
    if (a->v != b->v) return a->v < b->v ? -1 : 1;
    if (a->file != b->file) return a->file < b->file ? -1 : 1;
    return (a->pos > b->pos) - (a->pos < b->pos);
}

static inline bool fp_sampled(uint32_t v)
{
    return ((v * 0x9E3779B1u) >> 28) == 0;      /* 1/16 der Werte, für alle Dateien gleich */
}

/* Bit-Ähnlichkeit (0..1) von clip gegen src ab Versatz off (Werte) */
static double fp_similarity(const uint32_t *clip, int nc, const uint32_t *src, int ns, int off)
{
    long errors = 0, cnt = 0;
    for (int j = 0; j < nc; j++) {
        int q = off + j;
        if (q < 0 || q >= ns) continue;
        errors += __builtin_popcount(clip[j] ^ src[q]);
        cnt++;
    }
    if (cnt < nc * 0.8 || cnt < 40) return 0;
    return 1.0 - (double)errors / (32.0 * cnt);
}

typedef struct {
    const DfOptions *o;
    DfFile *files;
    int *vid;                   /* Indizes in files (mit Fingerabdruck) */
    int *nfp;                   /* Länge der Fingerabdrücke (sie selbst bleiben in der DB) */
    int nv;
    FpEntry *index;
    size_t nindex;
    atomic_int next;
    GMutex lock;
    GArray *cands;              /* Pair (a = Quelle, b = Clip) */
    GHashTable *linked;         /* schon verbundene Paare (Schlüssel a<<32|b) */
} PartCtx;

typedef struct {
    uint64_t key;
    int votes;
} Vote;

static gpointer partial_thread(gpointer data)
{
    PartCtx *c = data;
    enum { HSIZE = 1 << 14 };
    Vote *tab = g_new0(Vote, HSIZE);
    int *used = g_new(int, HSIZE);
    while (!df_cancel) {
        int ci = atomic_fetch_add(&c->next, 1);
        if (ci >= c->nv) break;
        const DfFile *clip = &c->files[c->vid[ci]];
        int nc = c->nfp[ci], nused = 0, status;
        if (nc < 80) continue;                  /* < ~10 s Ton */
        uint32_t *cf = NULL;
        if (!df_db_audio_get(clip->id, clip->size, clip->mtime, &cf, &nc, &status) || !cf) {
            g_free(cf);
            continue;
        }
        for (int j = 0; j < nc; j++) {
            if (!fp_sampled(cf[j])) continue;
            FpEntry key = {cf[j], 0, 0};
            /* erste Stelle mit Wert v */
            size_t lo = 0, hi = c->nindex;
            while (lo < hi) {
                size_t mid = (lo + hi) / 2;
                if (c->index[mid].v < key.v) lo = mid + 1;
                else hi = mid;
            }
            size_t end = lo;
            while (end < c->nindex && c->index[end].v == key.v) end++;
            if (end - lo > 64) continue;        /* häufiger Wert: sagt nichts */
            for (size_t q = lo; q < end; q++) {
                int si = (int)c->index[q].file;
                if (si == ci) continue;
                const DfFile *src = &c->files[c->vid[si]];
                if (!(src->duration > clip->duration * 1.02)) continue;   /* Quelle ist länger */
                if (clip->duration < src->duration * c->o->partial_min_ratio / 100.0) continue;
                int off = (int)c->index[q].pos - j;
                uint64_t k = (uint64_t)si << 32 | (uint32_t)(off >> 1);   /* ±1 Wert */
                uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> 50) & (HSIZE - 1);
                while (tab[h].votes && tab[h].key != k) h = (h + 1) & (HSIZE - 1);
                if (!tab[h].votes) {
                    if (nused >= HSIZE / 2) continue;
                    tab[h].key = k;
                    used[nused++] = (int)h;
                }
                tab[h].votes++;
            }
        }
        /* beste Kandidaten prüfen (bis zu 3 Quellen) */
        for (int round = 0; round < 3; round++) {
            int best = -1, bv = 0;
            for (int u = 0; u < nused; u++)
                if (tab[used[u]].votes > bv) {
                    bv = tab[used[u]].votes;
                    best = used[u];
                }
            if (best < 0 || bv < 4) break;
            int si = (int)(tab[best].key >> 32);
            int off = (int)(int32_t)(uint32_t)tab[best].key * 2;
            tab[best].votes = 0;
            /* Versatz verfeinern (Fingerabdruck der Quelle aus der Datenbank) */
            const DfFile *src = &c->files[c->vid[si]];
            uint32_t *sf = NULL;
            int ns = 0, sst;
            if (!df_db_audio_get(src->id, src->size, src->mtime, &sf, &ns, &sst) || !sf) {
                g_free(sf);
                continue;
            }
            double bs = 0;
            int boff = off;
            for (int d = -3; d <= 4; d++) {
                double sim = fp_similarity(cf, nc, sf, ns, off + d);
                if (sim > bs) {
                    bs = sim;
                    boff = off + d;
                }
            }
            g_free(sf);
            if (bs * 100 < c->o->partial_audio) continue;
            Pair p = {c->vid[si], c->vid[ci], (float)(bs * 100), false, true, boff * FP_ITEM_SECS};
            g_mutex_lock(&c->lock);
            gint64 key = df_pair_key(p.a, p.b);
            if (!g_hash_table_contains(c->linked, &key)) g_array_append_val(c->cands, p);
            g_mutex_unlock(&c->lock);
        }
        g_free(cf);
        for (int u = 0; u < nused; u++) tab[used[u]].votes = 0, tab[used[u]].key = 0;
        if (df_fe.progress && ci % 200 == 0) df_fe.progress(DF_PHASE_PARTIAL, ci, c->nv);
    }
    g_free(tab);
    g_free(used);
    return NULL;
}

/* Bildprüfung: Bilder des Clips gegen die Quelle am Versatz */
static bool partial_visual_ok(DfProc *proc, const DfOptions *o, const DfFile *src, const DfFile *clip,
                              double offset, double *sim_out)
{
    int k = clip->nframes;
    GString *times = g_string_new("F\t");
    for (int i = 0; i < k; i++)
        g_string_append_printf(times, "%s%.3f", i ? "," : "",
                               offset + clip->duration * (i + 1) / (k + 1));
    DfReply r;
    DfReplyStatus st = df_proc_request(proc, times->str, src->path, 120, CPU_RSS_LIMIT, &r);
    g_string_free(times, TRUE);
    bool ok = false;
    if (st == DF_R_OK && r.bin_len == sizeof(DfFrame) * (size_t)k) {
        double sim = frames_similarity(clip->frames, (const DfFrame *)r.bin, k, false);
        *sim_out = sim;
        ok = sim >= o->partial_visual_pct;
    }
    df_reply_free(&r);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Gruppen bilden
 * ------------------------------------------------------------------------- */
static int uf_find(int *parent, int x)
{
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }
    return x;
}

static int cmp_member_dur(const void *x, const void *y, void *ud)
{
    const DfFile *files = ud;
    const DfMember *a = x, *b = y;
    /* Quelle vor Teilclips, sonst nach Pfad */
    if (a->partial != b->partial) return a->partial ? 1 : -1;
    return strcmp(files[a->file].path, files[b->file].path);
}

typedef struct {
    int root, count;
} RootCount;

static int cmp_group_size(const void *x, const void *y)
{
    const DfGroup *a = x, *b = y;
    if (a->similarity != b->similarity) return a->similarity > b->similarity ? -1 : 1;
    return b->n - a->n;
}

/* Paare -> Ergebnis (nur Dateien in Gruppen werden übernommen) */
static void build_groups(DfFile *files, int nfiles, GArray *pairs, DfResult *res)
{
    int *parent = g_new(int, nfiles);
    float *best = g_new0(float, nfiles);
    bool *flip = g_new0(bool, nfiles);
    int *psrc = g_new(int, nfiles);
    double *poff = g_new0(double, nfiles);
    for (int i = 0; i < nfiles; i++) {
        parent[i] = i;
        psrc[i] = -1;
    }
    for (guint i = 0; i < pairs->len; i++) {
        Pair *p = &g_array_index(pairs, Pair, i);
        int ra = uf_find(parent, p->a), rb = uf_find(parent, p->b);
        if (ra != rb) parent[rb] = ra;
        if (p->partial) {
            if (p->sim > best[p->b] || psrc[p->b] < 0) {
                psrc[p->b] = p->a;
                poff[p->b] = p->offset;
            }
            if (p->sim > best[p->a] && psrc[p->a] < 0) best[p->a] = p->sim;
            if (p->sim > best[p->b]) best[p->b] = p->sim;
        } else {
            if (p->sim > best[p->a]) best[p->a] = p->sim, flip[p->a] = p->flip;
            if (p->sim > best[p->b]) best[p->b] = p->sim, flip[p->b] = p->flip;
        }
    }
    /* Gruppengrößen */
    int *grp_size = g_new0(int, nfiles);
    for (int i = 0; i < nfiles; i++) grp_size[uf_find(parent, i)]++;
    int *newidx = g_new(int, nfiles), nkept = 0, ngroups = 0;
    for (int i = 0; i < nfiles; i++) {
        newidx[i] = -1;
        if (grp_size[uf_find(parent, i)] > 1) newidx[i] = nkept++;
        if (parent[i] == i && grp_size[i] > 1) ngroups++;
    }
    res->files = g_new0(DfFile, nkept ? nkept : 1);
    res->nfiles = nkept;
    for (int i = 0; i < nfiles; i++)
        if (newidx[i] >= 0) {
            res->files[newidx[i]] = files[i];   /* übernimmt Speicher */
            memset(&files[i], 0, sizeof files[i]);
            files[i].path = NULL;
        }
    res->groups = g_new0(DfGroup, ngroups ? ngroups : 1);
    res->members = g_new0(DfMember, nkept ? nkept : 1);
    res->ngroups = ngroups;
    res->nmembers = nkept;
    /* Gruppen anlegen: Wurzel -> Gruppen-Nummer */
    int *gid = g_new(int, nfiles), g = 0, pos = 0;
    for (int i = 0; i < nfiles; i++) {
        gid[i] = -1;
        if (parent[i] == i && grp_size[i] > 1) {
            res->groups[g].first = pos;
            res->groups[g].n = 0;
            pos += grp_size[i];
            gid[i] = g++;
        }
    }
    for (int i = 0; i < nfiles; i++) {
        if (newidx[i] < 0) continue;
        DfGroup *gr = &res->groups[gid[uf_find(parent, i)]];
        DfMember *m = &res->members[gr->first + gr->n++];
        m->file = newidx[i];
        m->similarity = best[i];
        m->flipped = flip[i];
        m->partial = psrc[i] >= 0;
        m->source = psrc[i] >= 0 ? newidx[psrc[i]] : -1;
        m->offset = poff[i];
    }
    for (int i = 0; i < ngroups; i++) {
        DfGroup *gr = &res->groups[i];
        g_qsort_with_data(&res->members[gr->first], gr->n, sizeof(DfMember), cmp_member_dur,
                          res->files);
        gr->similarity = 100;
        for (int j = 0; j < gr->n; j++) {
            DfMember *m = &res->members[gr->first + j];
            if (m->similarity < gr->similarity) gr->similarity = m->similarity;
            if (m->partial) gr->partial = true;
        }
    }
    /* Gruppen nach Ähnlichkeit (höchste zuerst); Mitglieder bleiben zusammen */
    DfGroup *sorted = g_memdup2(res->groups, sizeof(DfGroup) * (gsize)MAX(ngroups, 1));
    qsort(sorted, ngroups, sizeof *sorted, cmp_group_size);
    DfMember *mem = g_new(DfMember, MAX(nkept, 1));
    int mp = 0;
    for (int i = 0; i < ngroups; i++) {
        memcpy(&mem[mp], &res->members[sorted[i].first], sizeof(DfMember) * (size_t)sorted[i].n);
        sorted[i].first = mp;
        mp += sorted[i].n;
    }
    g_free(res->groups);
    g_free(res->members);
    res->groups = sorted;
    res->members = mem;
    g_free(gid);
    g_free(newidx);
    g_free(grp_size);
    g_free(parent);
    g_free(best);
    g_free(flip);
    g_free(psrc);
    g_free(poff);
}

int df_compare(const DfOptions *o, DfFile *files, int n, const bool *is_new, DfResult *res,
               DfStats *st)
{
    double t0 = df_now();
    if (df_fe.phase) df_fe.phase(DF_PHASE_COMPARE, n);
    GArray *pairs = g_array_new(FALSE, FALSE, sizeof(Pair));
    GHashTable *notmatch = df_db_notmatch_load();
    GHashTable *dirs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    int *vid = g_new(int, MAX(n, 1)), *img = g_new(int, MAX(n, 1)), nv = 0, ni = 0;
    for (int i = 0; i < n; i++) {
        if (files[i].image) {
            if (files[i].nframes >= 1) img[ni++] = i;
        } else if (files[i].nframes == o->frames) {
            vid[nv++] = i;
        }
    }
    compare_set(o, files, vid, nv, o->frames, is_new, dirs, notmatch, pairs, st);
    if (ni > 1 && !df_cancel) {
        /* Bilder: je 1 Vergleichsbild, Laufzeit egal */
        DfOptions oi = *o;
        oi.duration_diff = 100;
        compare_set(&oi, files, img, ni, 1, is_new, dirs, notmatch, pairs, st);
    }
    st->secs_compare = df_now() - t0;
    df_log(false, "Vergleich: %d Videos, %d Bilder, %lld Paare, %u Treffer in %.1f s", nv, ni,
           st->pairs, pairs->len, st->secs_compare);

    /* Teilclips */
    if (o->partial && nv > 1 && !df_cancel) {
        double tp = df_now();
        PartCtx c = {.o = o, .files = files};
        c.vid = g_new(int, nv);
        c.nfp = g_new0(int, nv);
        /* Index: nur jeder ~16. Wert (für alle Dateien dieselben); die vollen Fingerabdrücke
         * bleiben in der Datenbank und werden zur Prüfung einzeln gelesen – so passt auch eine
         * Bibliothek mit 100 000 Videos in wenige hundert MB */
        GArray *index = g_array_new(FALSE, FALSE, sizeof(FpEntry));
        for (int i = 0; i < nv && !df_cancel; i++) {
            DfFile *f = &files[vid[i]];
            if (f->asr <= 0 || f->duration < 10) continue;
            uint32_t *fp;
            int nfp, status;
            if (!df_db_audio_get(f->id, f->size, f->mtime, &fp, &nfp, &status)) continue;
            if (status == 0 && nfp >= 80) {
                c.vid[c.nv] = vid[i];
                c.nfp[c.nv] = nfp;
                for (int j = 0; j < nfp; j++)
                    if (fp_sampled(fp[j])) {
                        FpEntry e = {fp[j], (uint32_t)c.nv, (uint32_t)j};
                        g_array_append_val(index, e);
                    }
                c.nv++;
            }
            g_free(fp);
        }
        if (df_fe.phase) df_fe.phase(DF_PHASE_PARTIAL, c.nv);
        df_log(false, "Teilclips: %d Videos mit Ton-Fingerabdruck, Index %u Einträge", c.nv,
               index->len);
        c.nindex = index->len;
        c.index = (FpEntry *)g_array_free(index, FALSE);
        qsort(c.index, c.nindex, sizeof *c.index, cmp_fp_entry);
        c.cands = g_array_new(FALSE, FALSE, sizeof(Pair));
        c.linked = g_hash_table_new_full(g_int64_hash, g_int64_equal, g_free, NULL);
        for (guint i = 0; i < pairs->len; i++) {
            Pair *p = &g_array_index(pairs, Pair, i);
            gint64 *k = g_new(gint64, 1);
            *k = df_pair_key(p->a, p->b);
            g_hash_table_add(c.linked, k);
        }
        int nt = df_cpu_count();
        GThread **th = g_new0(GThread *, nt);
        for (int i = 0; i < nt; i++) th[i] = g_thread_new("partial", partial_thread, &c);
        for (int i = 0; i < nt; i++) g_thread_join(th[i]);
        g_free(th);
        df_log(false, "Teilclips: %u Kandidaten (Ton) in %.1f s", c.cands->len, df_now() - tp);
        /* Bildprüfung */
        int kept = 0;
        DfProc proc;
        memset(&proc, 0, sizeof proc);
        proc.pid = -1;
        for (guint i = 0; i < c.cands->len && !df_cancel; i++) {
            Pair *p = &g_array_index(c.cands, Pair, i);
            gint64 key = df_pair_key(p->a, p->b);
            if (g_hash_table_contains(notmatch, &key)) continue;
            if (o->partial_visual) {
                if (proc.pid <= 0 && !df_proc_start(&proc, NULL, 10, o->white_borders)) break;
                double vs = -1;
                if (!partial_visual_ok(&proc, o, &files[p->a], &files[p->b], p->offset, &vs)) {
                    df_log(false, "Teilclip verworfen (Bild %.0f %%): %s in %s", vs,
                           files[p->b].path, files[p->a].path);
                    continue;
                }
            }
            g_array_append_val(pairs, *p);
            kept++;
        }
        df_proc_stop(&proc);
        df_log(true, "Teilclips: %d gefunden", kept);
        g_free(c.nfp);
        g_free(c.vid);
        g_free(c.index);
        g_array_free(c.cands, TRUE);
        g_hash_table_destroy(c.linked);
    }
    build_groups(files, n, pairs, res);
    res->similarity = o->similarity;
    res->frames = o->frames;
    res->scan_time = (long long)time(NULL);
    st->groups = res->ngroups;
    st->duplicates = res->nmembers - res->ngroups;
    g_array_free(pairs, TRUE);
    g_hash_table_destroy(notmatch);
    g_hash_table_destroy(dirs);
    g_free(vid);
    g_free(img);
    return df_cancel ? 1 : 0;
}

/* ---------------------------------------------------------------------------
 * Ablauf
 * ------------------------------------------------------------------------- */
static bool entry_valid(const DfDbEntry *e, const Item *it, const DfOptions *o)
{
    if (!e || e->f.size != it->size || e->f.mtime != it->mtime) return false;
    if (e->status != 0) return !o->retry_failed;        /* nicht lesbar: nicht jedes Mal neu */
    if (e->white_borders != o->white_borders) return false;
    if (it->image || e->f.image) return e->f.nframes >= 1;
    /* sehr kurze Videos (< 0,2 s) haben nur 1 Bild – trotzdem gültig */
    return e->k == o->frames && e->f.nframes >= 1;
}

static int scan_impl(const DfOptions *o, const DfGpu *gpu, DfResult *res, DfStats *st, const char **err)
{
    memset(res, 0, sizeof *res);
    memset(st, 0, sizeof *st);
    *err = NULL;
    df_cancel = 0;
    if (!df_worker_path()) {
        *err = "Der Hilfsprozess (duplikat-finder-worker) fehlt – bitte neu installieren.";
        return -1;
    }
    if (!o->includes.n) {
        *err = "Bitte mindestens einen Ordner angeben.";
        return -1;
    }
    df_log_open();
    df_log(false, "%s %s – Suchlauf", DF_NAME, DF_VERSION);
    for (int i = 0; i < o->includes.n; i++) df_log(false, "Ordner: %s", o->includes.v[i]);
    df_log(false, "Vergleichsbilder %d, Ähnlichkeit %.0f %%, Laufzeit ±%d %%, gespiegelt %s, "
           "CPU-Prozesse %d, GPU-Prozesse %d (%s)", o->frames, o->similarity, o->duration_diff,
           o->flipped ? "ja" : "nein", o->cpu_workers, gpu && gpu->available ? o->gpu_workers : 0,
           gpu && gpu->available ? gpu->name : "keine GPU");

    /* 1. Ordner */
    double t0 = df_now();
    if (df_fe.phase) df_fe.phase(DF_PHASE_WALK, -1);
    GArray *items = walk_all(o);
    st->found = (int)items->len;
    st->secs_walk = df_now() - t0;
    df_log(true, "%d Dateien gefunden (%.1f s)", st->found, st->secs_walk);
    if (df_cancel) {
        items_free(items);
        st->aborted = true;
        return 1;
    }

    /* 2. Vergleichsbilder */
    t0 = df_now();
    GHashTable *db = df_db_load_all();
    df_log(false, "Datenbank: %u Einträge (%.1f s)", g_hash_table_size(db), df_now() - t0);
    Pool pool = {.kind = JOB_SCAN, .o = o, .gpu = gpu};
    g_mutex_init(&pool.lock);
    pool.todo = g_new(Item *, MAX(items->len, 1));
    for (guint i = 0; i < items->len; i++) {
        Item *it = &g_array_index(items, Item, i);
        DfDbEntry *e = g_hash_table_lookup(db, it->path);
        if (entry_valid(e, it, o)) st->cached++;
        else pool.todo[pool.ntodo++] = it;
    }
    df_log(true, "%d aus der Datenbank, %d neu zu lesen", st->cached, pool.ntodo);
    if (df_fe.phase) df_fe.phase(DF_PHASE_SCAN, pool.ntodo);
    if (pool.ntodo) {
        ScanCtx sc = {.db = db, .last_commit = df_now(), .st = st};
        run_pool(&pool, o, gpu, on_scan_result, &sc);
        if (sc.in_tx) df_db_commit();
    }
    g_free(pool.todo);
    g_mutex_clear(&pool.lock);
    st->secs_scan = df_now() - t0;
    if (pool.ntodo)
        df_log(true, "Gelesen: %d (GPU %d, CPU %d), nicht lesbar: %d – %.0f s", st->scanned,
               st->gpu_files, st->cpu_files, st->failed, st->secs_scan);
    if (df_cancel) {
        items_free(items);
        g_hash_table_destroy(db);
        st->aborted = true;
        return 1;
    }

    /* Vergleichsliste: gefundene Dateien (+ ggf. die ganze Datenbank) */
    GHashTable *in_scan = g_hash_table_new(g_str_hash, g_str_equal);
    GArray *list = g_array_new(FALSE, TRUE, sizeof(DfFile));
    GArray *isnew = g_array_new(FALSE, TRUE, sizeof(bool));
    for (guint i = 0; i < items->len; i++) {
        Item *it = &g_array_index(items, Item, i);
        DfDbEntry *e = g_hash_table_lookup(db, it->path);
        if (!e || e->status != 0 || !e->f.nframes) continue;
        DfFile f;
        df_file_copy(&f, &e->f);
        g_array_append_val(list, f);
        bool t = true;
        g_array_append_val(isnew, t);
        g_hash_table_add(in_scan, e->f.path);
    }
    if (o->against_db) {
        GHashTable *ignored = df_db_ignored_load();
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, db);
        while (g_hash_table_iter_next(&it, &k, &v)) {
            DfDbEntry *e = v;
            if (g_hash_table_contains(in_scan, k) || e->status != 0 || !e->f.nframes) continue;
            if (g_hash_table_contains(ignored, k)) continue;
            if (!e->f.image && (e->k != o->frames || e->white_borders != o->white_borders)) continue;
            if (e->f.image && !o->images) continue;
            if (g_access(e->f.path, F_OK) != 0) continue;
            DfFile f;
            df_file_copy(&f, &e->f);
            g_array_append_val(list, f);
            bool fa = false;
            g_array_append_val(isnew, fa);
        }
        g_hash_table_destroy(ignored);
    }
    g_hash_table_destroy(in_scan);

    /* 3. Ton-Fingerabdrücke für Teilclips */
    if (o->partial && !df_cancel) {
        t0 = df_now();
        Pool ap = {.kind = JOB_AUDIO, .o = o, .gpu = gpu};
        g_mutex_init(&ap.lock);
        DfFile *lf = (DfFile *)list->data;
        ap.afiles = g_new(DfFile *, MAX(list->len, 1));
        for (guint i = 0; i < list->len; i++) {
            DfFile *f = &lf[i];
            if (f->image || f->asr <= 0 || f->duration < 10 || f->id <= 0) continue;
            uint32_t *fp;
            int nfp, status;
            if (df_db_audio_get(f->id, f->size, f->mtime, &fp, &nfp, &status)) {
                g_free(fp);
                if (status == 0 || status == 2 || !o->retry_failed) continue;
            }
            ap.afiles[ap.ntodo++] = f;
        }
        if (df_fe.phase) df_fe.phase(DF_PHASE_AUDIO, ap.ntodo);
        df_log(true, "Teilclips: %d Ton-Fingerabdrücke zu erstellen", ap.ntodo);
        if (ap.ntodo) {
            AudioCtx ac = {&ap, st};
            run_pool(&ap, o, gpu, on_audio_result, &ac);
        }
        g_free(ap.afiles);
        g_mutex_clear(&ap.lock);
        st->secs_audio = df_now() - t0;
    }

    /* 4./5. Vergleich */
    int rc = 1;
    if (!df_cancel) rc = df_compare(o, (DfFile *)list->data, (int)list->len, (bool *)isnew->data,
                                    res, st);
    for (guint i = 0; i < list->len; i++) df_file_clear(&g_array_index(list, DfFile, i));
    g_array_free(list, TRUE);
    g_array_free(isnew, TRUE);
    items_free(items);
    g_hash_table_destroy(db);
    if (rc == 0)
        df_log(true, "%d Gruppen mit %d Duplikaten (Vergleich %.1f s)", st->groups, st->duplicates,
               st->secs_compare);
    st->aborted = rc != 0;
    return rc;
}

/* Suchlauf; solange er läuft, geht der PC nicht in Standby (auch beim geplanten Lauf) */
int df_scan(const DfOptions *o, const DfGpu *gpu, DfResult *res, DfStats *st, const char **err)
{
    df_inhibit_sleep(true, "Duplikat-Finder sucht doppelte Videos");
    int rc = scan_impl(o, gpu, res, st, err);
    df_inhibit_sleep(false, NULL);
    return rc;
}
