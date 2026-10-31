/*
 * AV1-Converter – Engine
 * ======================
 * Sucht Videos in einem Ordner (inkl. Unterordner) und wandelt sie nach AV1 um,
 * um Speicherplatz zu sparen. Bedient wird die Engine über ac_core.h – von der
 * Terminal-Version (ac_cli.c) und der grafischen Oberfläche (ac_gui.c).
 *
 * Kodierung – entspricht dem HandBrake-Profil „AV1 MKV 2160p60 4K“:
 *   * CPU: HandBrake selbst mit genau diesem Profil (Flatpak fr.handbrake.ghb
 *     bevorzugt, sonst HandBrakeCLI): SVT-AV1 10 Bit, Preset/RF laut Profil,
 *     Auto-Crop, Decomb, höchstens 3840x2160 und 60 Bilder/s (Peak), erste
 *     Tonspur als Opus 160 kbit/s Stereo, Kapitel, MKV. Ohne HandBrake: ffmpeg
 *     mit libsvtav1 und denselben Werten.
 *   * GPU (Standard, 25- bis 40-mal so schnell): ffmpeg mit VAAPI (av1_vaapi, 10 Bit).
 *     Zuschnitt wie HandBrake (dessen Scan liefert die Werte, sonst cropdetect),
 *     Deinterlacing nur bei Interlaced-Material, Peak-Framerate 60, höchstens
 *     2160p, Ton und Kapitel wie im Profil. HEVC/VP9 dekodiert die GPU, H.264 die
 *     CPU (siehe want_hw_decode). Scheitert ein GPU-Job, wird mit CPU-Dekoder und
 *     danach per HandBrake wiederholt.
 *   * GPU + CPU parallel: die GPU nimmt die größten Dateien, die CPU die
 *     kleinsten. Gemessen insgesamt langsamer als GPU allein, weil
 *     HandBrake der GPU die CPU für Dekodierung und Kopien wegnimmt.
 *
 * Sicherheit und Platz:
 *   * Jedes Ergebnis wird geprüft (AV1-Spur, Dauer). Ist es nicht mindestens
 *     min_saving % kleiner, bleibt das Original (und wird nie wieder versucht).
 *   * Originale werden erst ersetzt, wenn das Ergebnis vollständig und geprüft
 *     ist; geschrieben wird in eine versteckte Temp-Datei daneben.
 *   * Bearbeitete Dateien stehen in ~/.av1-converter/erledigt.tsv – ein neuer
 *     Lauf macht dort weiter, wo der letzte aufgehört hat. ffprobe-Ergebnisse
 *     werden zwischengespeichert, damit die Suche schnell bleibt.
 *   * Encoder laufen mit niedriger CPU- und I/O-Priorität, Standby ist gesperrt.
 *
 * Aufbau dieser Datei: Konfiguration – kleine Helfer – Lauf-Log – Prozesse –
 * Listen auf der Platte (Prüf-Cache, erledigte Dateien) – Analyse (ffprobe) –
 * Suche – HandBrake und GPU erkennen – Zuschnitt – Befehle – laufende Jobs,
 * Pause und Watchdog – Ergebnis prüfen und ablegen – Verteilung auf CPU/GPU –
 * öffentliche Schnittstelle
 *
 * Namenskonventionen: ac_* = öffentliche Schnittstelle (ac_core.h), g_* bzw.
 * run.* = globaler Zustand, GROSSBUCHSTABEN = Konstanten.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glob.h>
#include <math.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ac_core.h"

/* ---------------------------------------------------------------------------
 * Konfiguration
 * ------------------------------------------------------------------------- */
static const char *const VIDEO_EXTS[] = {"mp4", "mkv", "mov", "avi", "m4v", "webm", "flv", "wmv",
                                         "mpg", "mpeg", "ts", "m2ts", "mts", "3gp", "ogv", "divx",
                                         "asf", "f4v", "rmvb", "rm", NULL};
/* Ordner, die nie angefasst werden (Disc-Strukturen, NAS-Metadaten) */
static const char *const SKIP_DIRS[] = {"BDMV", "VIDEO_TS", "AUDIO_TS", "@eaDir", "#recycle",
                                        "lost+found", NULL};

#define FLATPAK_ID "fr.handbrake.ghb"
#define PROBE_THREADS_MIN 8     /* parallele ffprobe-Aufrufe bei der Suche: so viele wie CPU-Threads,
                                   mindestens 8 (auf dem NAS wartet ffprobe meist aufs Netz) */
#define CPU_NICE 19             /* CPU-Priorität von HandBrake: GPU-Jobs und Desktop gehen vor */
#define GPU_NICE 5              /* GPU-Jobs: vor HandBrake, damit ihre Dekodierung nicht wartet */
#define STALL_SECS 900          /* kein Fortschritt so lange -> Job abbrechen (Fehler) */
#define VERIFY_TOL 2.0          /* s Abweichung der Dauer, mindestens ... */
#define VERIFY_TOL_REL 0.01     /* ... bzw. 1 % */
#define RAM_PER_JOB_GB 3.5      /* frei nötig für einen CPU-Job bis 1440p (gemessen 3,2 GB) ... */
#define RAM_PER_JOB_4K_GB 7.0   /* ... bzw. darüber (gemessen 6,2 GB) */
#define BIG_FRAME_PIXELS (2560 * 1440)  /* darüber zählt ein CPU-Job doppelt */
#define MAX_W 3840              /* Profil: höchstens 2160p */
#define MAX_H 2160
#define MAX_FPS 60              /* Profil: Peak-Framerate */
#define TMP_TAG ".av1-tmp"      /* Temp-Dateien: .<Name>.av1-tmp.mkv */
#define ERR_TAIL 8192           /* so viel Fehlerausgabe je Job behalten */
#define EARLY_CHECK_PCT 20.0    /* ab diesem Fortschritt wird die Endgröße hochgerechnet ... */
#define EARLY_MARGIN 1.3        /* ... liegt sie so weit über dem Ziel: früh abbrechen (kein Gewinn) */

volatile sig_atomic_t ac_cancel;
char *AC_APP_DIR, *AC_RUN_LOG, *AC_DONE_DB;
static char *PROBE_CACHE, *PRESET_EXPORT;
static char *PENDING_DB;        /* ausgabe.tsv: Ergebnisse im Ausgabeordner, Original noch da */
static char *CREATED_DIRS;      /* angelegte_ordner.txt: vom Programm angelegte Ausgabeordner */

static void default_log(const char *msg)
{
    puts(msg);
    fflush(stdout);
}

AcFrontend ac_fe = {.log = default_log};

void ac_options_default(AcOptions *o)
{
    *o = (AcOptions){.mode = AC_MODE_GPU, .cpu_jobs = DEFAULT_CPU_JOBS,
                     .gpu_jobs = DEFAULT_GPU_JOBS, .rf = DEFAULT_RF,
                     .gpu_quality = DEFAULT_GPU_QUALITY, .audio = AC_AUDIO_PRESET,
                     .subs = AC_SUBS_PRESET, .min_saving = DEFAULT_MIN_SAVING,
                     .replace = false, .keep_mtime = true, .out_dir = NULL,
                     .hdr = AC_HDR_GPU_CONTAINER};
}

/* ---------------------------------------------------------------------------
 * Kleine Helfer
 * ------------------------------------------------------------------------- */
static double now_secs(void)
{
    return g_get_monotonic_time() / 1e6;
}

/* Dezimalpunkt durch Komma ersetzen (deutsche Anzeige) */
static char *decimal_comma(char *s)
{
    for (char *p = s; *p; p++)
        if (*p == '.') *p = ',';
    return s;
}

char *ac_format_bytes(double bytes, char *out, size_t sz)
{
    double a = fabs(bytes);
    if (a >= 1e12) snprintf(out, sz, "%.2f TB", bytes / 1e12);
    else if (a >= 1e11) snprintf(out, sz, "%.0f GB", bytes / 1e9);
    else if (a >= 1e9) snprintf(out, sz, "%.1f GB", bytes / 1e9);
    else if (a >= 1e6) snprintf(out, sz, "%.0f MB", bytes / 1e6);
    else snprintf(out, sz, "%.0f kB", bytes / 1e3);
    return decimal_comma(out);
}

char *ac_format_duration(double secs, char *out, size_t sz)
{
    long s = secs > 0 ? (long)(secs + 0.5) : 0;
    if (s < 60) snprintf(out, sz, "%ld s", s);
    else if (s < 3600) snprintf(out, sz, "%ld:%02ld min", s / 60, s % 60);
    else if (s < 86400) snprintf(out, sz, "%ld:%02ld h", s / 3600, s / 60 % 60);
    else if (s < 2 * 86400) snprintf(out, sz, "1 Tag %ld h", s / 3600 % 24);
    else snprintf(out, sz, "%ld Tage %ld h", s / 86400, s / 3600 % 24);
    return out;
}

char *ac_format_remaining(double secs, char *out, size_t sz)
{
    long m = secs > 0 ? lround(secs / 60) : 0;      /* auf Minuten gerundet */
    if (secs < 60) snprintf(out, sz, "unter 1 Minute");
    else if (m < 60) snprintf(out, sz, "ca. %ld min", m);
    else if (m < 24 * 60) snprintf(out, sz, "ca. %ld h %02ld min", m / 60, m % 60);
    else if (m < 2 * 24 * 60) snprintf(out, sz, "ca. 1 Tag %ld h", m / 60 % 24);
    else snprintf(out, sz, "ca. %ld Tage %ld h", m / 1440, m / 60 % 24);
    return out;
}

char *ac_format_finish(double secs_from_now, char *out, size_t sz)
{
    GDateTime *now = g_date_time_new_now_local();
    GDateTime *end = g_date_time_add_seconds(now, MAX(secs_from_now, 0));
    GDateTime *today = g_date_time_new_local(g_date_time_get_year(now), g_date_time_get_month(now),
                                             g_date_time_get_day_of_month(now), 0, 0, 0);
    double days = g_date_time_difference(end, today) / (double)G_TIME_SPAN_DAY;
    char *s = g_date_time_format(end, days < 1 ? "heute, %H:%M" : days < 2 ? "morgen, %H:%M"
                                      : days < 7 ? "%A, %d.%m., %H:%M" : "%d.%m.%Y, %H:%M");
    g_strlcpy(out, s ? s : "?", sz);
    g_free(s);
    g_date_time_unref(today);
    g_date_time_unref(end);
    g_date_time_unref(now);
    return out;
}

const char *ac_codec_name(const char *c)
{
    static const char *const names[][2] = {
        {"h264", "H.264"}, {"hevc", "HEVC"}, {"av1", "AV1"}, {"vp9", "VP9"}, {"vp8", "VP8"},
        {"mpeg2video", "MPEG-2"}, {"mpeg4", "MPEG-4"}, {"msmpeg4v3", "DivX 3"},
        {"vc1", "VC-1"}, {"wmv3", "WMV 9"}, {"wmv2", "WMV 8"}, {"prores", "ProRes"},
        {"mjpeg", "MJPEG"}, {"rv40", "RealVideo 4"}, {"flv1", "FLV"}, {"theora", "Theora"}};
    for (size_t i = 0; i < G_N_ELEMENTS(names); i++)
        if (!strcmp(c, names[i][0])) return names[i][1];
    return c;
}

/* Erfahrungswerte (gemessen: H.264 1080p -> AV1 RF 35: -65 %); nur zur Anzeige */
double ac_estimated_saving(const AcVideo *v)
{
    static const char *const strong[] = {"prores", "dnxhd", "rawvideo", "ffv1", "huffyuv",
                                         "mjpeg", "utvideo", NULL};
    static const char *const modern[] = {"hevc", "vp9", NULL};
    double s = 0.6;
    if (v->cpu_only && !v->hdr) return 0.18;        /* GPU ohne Gewinn: HandBrake schafft 13–25 % */
    if (g_strv_contains(strong, v->vcodec)) s = 0.9;
    else if (g_strv_contains(modern, v->vcodec)) s = 0.3;
    else if (!strcmp(v->vcodec, "vp8")) s = 0.45;
    double pixels = (double)MAX(1, v->width) * MAX(1, v->height);
    double kbps_per_mpix = v->kbps / (pixels / 1e6);  /* sehr kleine Bitrate: weniger Gewinn */
    if (kbps_per_mpix > 0 && kbps_per_mpix < 600) s *= 0.5;
    return s;
}

static bool has_video_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    for (int i = 0; VIDEO_EXTS[i]; i++)
        if (!g_ascii_strcasecmp(dot + 1, VIDEO_EXTS[i])) return true;
    return false;
}

static double free_space_gb(const char *dir)
{
    struct statvfs st;
    if (statvfs(dir, &st) != 0) return INFINITY;
    return (double)st.f_bavail * st.f_frsize / 1e9;
}

/* verfügbarer Arbeitsspeicher (GB) laut /proc/meminfo */
static double mem_available_gb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return INFINITY;
    char line[256];
    double kb = INFINITY;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %lf kB", &kb) == 1) break;
    fclose(f);
    return kb / 1e6;
}

static bool g_have_mkvpropedit;          /* in ac_init ermittelt */

bool ac_have_mkvpropedit(void)
{
    return g_have_mkvpropedit;
}

/* HDR-Video, das ganz zu HandBrake soll (vor dem Start entscheidbar; Dolby Vision/HDR10+ werden
 * erst im GPU-Job erkannt und dann weitergereicht) */
static bool hdr_to_handbrake(const AcVideo *v, const AcOptions *o)
{
    return v->hdr && o->mode != AC_MODE_CPU &&
           (o->hdr == AC_HDR_HANDBRAKE || (o->hdr == AC_HDR_GPU_CONTAINER && !g_have_mkvpropedit));
}

static bool command_exists(const char *name)
{
    char *p = g_find_program_in_path(name);
    g_free(p);
    return p != NULL;
}

/* ---------------------------------------------------------------------------
 * Lauf-Log (~/.av1-converter/last_run.log)
 * ------------------------------------------------------------------------- */
static GMutex g_log_lock;
static FILE *g_log_file;

void ac_log_open(void)
{
    g_mutex_lock(&g_log_lock);
    if (g_log_file) fclose(g_log_file);
    g_mkdir_with_parents(AC_APP_DIR, 0755);
    g_log_file = fopen(AC_RUN_LOG, "w");
    g_mutex_unlock(&g_log_lock);
}

void ac_log(bool show, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    g_mutex_lock(&g_log_lock);
    if (g_log_file) {
        GDateTime *dt = g_date_time_new_now_local();
        char *stamp = g_date_time_format(dt, "%Y-%m-%d %H:%M:%S");
        fprintf(g_log_file, "%s %s\n", stamp, msg);
        fflush(g_log_file);
        g_free(stamp);
        g_date_time_unref(dt);
    }
    g_mutex_unlock(&g_log_lock);
    if (show && ac_fe.log) ac_fe.log(msg);
    g_free(msg);
}

/* ---------------------------------------------------------------------------
 * Prozesse
 * Encoder laufen mit niedriger Priorität (HandBrake nice CPU_NICE, GPU-Jobs GPU_NICE)
 * und I/O-Klasse "best effort, niedrig".
 * Pausieren und Beenden wirken auf den ganzen Prozessbaum (HandBrake aus dem
 * Flatpak läuft in einer eigenen Sitzung – Prozessgruppen reichen dort nicht).
 * ------------------------------------------------------------------------- */
enum { PRIO_NORMAL, PRIO_LOW, PRIO_GPU };

/* im Kindprozess vor exec: Signale zurücksetzen, Priorität senken */
static void child_setup(gpointer data)
{
    signal(SIGPIPE, SIG_DFL);
    int prio = GPOINTER_TO_INT(data);
    if (prio != PRIO_NORMAL) {
        setpriority(PRIO_PROCESS, 0, prio == PRIO_GPU ? GPU_NICE : CPU_NICE);
        syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, 0, (2 << 13) | 7);
    }
}

/* alle Nachkommen von root (und root selbst) mit sig beenden/anhalten */
static void signal_tree(pid_t root, int sig)
{
    GArray *pids = g_array_new(FALSE, FALSE, sizeof(pid_t));
    GArray *ppids = g_array_new(FALSE, FALSE, sizeof(pid_t));
    DIR *d = opendir("/proc");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        char path[300], buf[512];
        snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        char *close_paren = strrchr(buf, ')');     /* Name kann Leerzeichen enthalten */
        int ppid;
        char state;
        if (!close_paren || sscanf(close_paren + 2, "%c %d", &state, &ppid) != 2) continue;
        pid_t pid = atoi(e->d_name);
        g_array_append_val(pids, pid);
        g_array_append_val(ppids, ppid);
    }
    if (d) closedir(d);
    GArray *tree = g_array_new(FALSE, FALSE, sizeof(pid_t));
    g_array_append_val(tree, root);
    for (guint i = 0; i < tree->len; i++) {        /* Breitensuche über die Eltern-Beziehung */
        pid_t parent = g_array_index(tree, pid_t, i);
        for (guint k = 0; k < pids->len; k++)
            if (g_array_index(ppids, pid_t, k) == parent)
                g_array_append_val(tree, g_array_index(pids, pid_t, k));
    }
    for (guint i = 0; i < tree->len; i++) kill(g_array_index(tree, pid_t, i), sig);
    g_array_free(tree, TRUE);
    g_array_free(pids, TRUE);
    g_array_free(ppids, TRUE);
}

typedef struct {
    GPid pid;
    int out, err;
    GString *out_line, *err_line;
} Proc;

static bool proc_spawn(Proc *p, char **argv, int prio, bool want_out, bool want_err, GError **error)
{
    memset(p, 0, sizeof *p);
    p->out = p->err = -1;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL,
                                  G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH |
                                      (want_out ? 0 : G_SPAWN_STDOUT_TO_DEV_NULL) |
                                      (want_err ? 0 : G_SPAWN_STDERR_TO_DEV_NULL),
                                  child_setup, GINT_TO_POINTER(prio), &p->pid, NULL,
                                  want_out ? &p->out : NULL, want_err ? &p->err : NULL, error))
        return false;
    if (p->out >= 0) fcntl(p->out, F_SETFL, O_NONBLOCK);
    if (p->err >= 0) fcntl(p->err, F_SETFL, O_NONBLOCK);
    return true;
}

/* Daten lesen; ganze Zeilen (\n oder \r) gehen an on_line, Rest bleibt im Puffer */
typedef void (*LineFn)(void *ctx, const char *line);

static void proc_read(int *fd, GString *pending, LineFn on_line, void *ctx)
{
    char tmp[16384];
    while (*fd >= 0) {
        ssize_t r = read(*fd, tmp, sizeof tmp);
        if (r > 0) {
            for (ssize_t i = 0; i < r; i++) {
                if (tmp[i] == '\n' || tmp[i] == '\r') {
                    if (pending->len && on_line) on_line(ctx, pending->str);
                    g_string_truncate(pending, 0);
                } else if (pending->len < 65536) {
                    g_string_append_c(pending, tmp[i]);
                }
            }
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else if (r < 0 && errno == EAGAIN) {
            return;
        } else {
            if (pending->len && on_line) on_line(ctx, pending->str);
            g_string_truncate(pending, 0);
            close(*fd);
            *fd = -1;
        }
    }
}

static int proc_exit_code(int status)
{
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return -WTERMSIG(status);
    return -1;
}

/* Ausgabe sammeln (stdout und stderr getrennt, je höchstens max Byte) */
typedef struct {
    GString *out, *err;
} Capture;

static void capture_out(void *ctx, const char *line)
{
    Capture *c = ctx;
    if (c->out && c->out->len < 4 * 1024 * 1024) g_string_append_printf(c->out, "%s\n", line);
}

static void capture_err(void *ctx, const char *line)
{
    Capture *c = ctx;
    if (c->err && c->err->len < 4 * 1024 * 1024) g_string_append_printf(c->err, "%s\n", line);
}

/* kurzes Programm ausführen (ffprobe, HandBrake-Scan) -> Exit-Code, -1 bei Fehler/Timeout */
static int run_capture(char **argv, int prio, double timeout, GString *out, GString *err)
{
    Proc p;
    if (!proc_spawn(&p, argv, prio, true, true, NULL)) return -1;
    Capture cap = {out, err};
    GString *ol = g_string_new(NULL), *el = g_string_new(NULL);
    double deadline = now_secs() + timeout;
    bool killed = false;
    int status = 0;
    while (true) {
        if (!killed && (now_secs() > deadline || ac_cancel)) {
            signal_tree(p.pid, SIGKILL);
            killed = true;
        }
        if (p.out < 0 && p.err < 0) break;
        struct pollfd fds[2];
        int n = 0;
        if (p.out >= 0) fds[n++] = (struct pollfd){.fd = p.out, .events = POLLIN};
        if (p.err >= 0) fds[n++] = (struct pollfd){.fd = p.err, .events = POLLIN};
        poll(fds, (nfds_t)n, 200);
        proc_read(&p.out, ol, capture_out, &cap);
        proc_read(&p.err, el, capture_err, &cap);
    }
    while (waitpid(p.pid, &status, 0) < 0 && errno == EINTR) {}
    g_spawn_close_pid(p.pid);
    g_string_free(ol, TRUE);
    g_string_free(el, TRUE);
    return killed ? -1 : proc_exit_code(status);
}

/* Argumentliste bauen: argv_add(a, "ffmpeg", "-i", x, NULL) */
static void argv_add(GPtrArray *a, ...)
{
    va_list ap;
    va_start(ap, a);
    const char *s;
    while ((s = va_arg(ap, const char *))) g_ptr_array_add(a, g_strdup(s));
    va_end(ap);
}

static void argv_addf(GPtrArray *a, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    g_ptr_array_add(a, g_strdup_vprintf(fmt, ap));
    va_end(ap);
}

static GPtrArray *argv_new(void)
{
    return g_ptr_array_new_with_free_func(g_free);
}

static char **argv_finish(GPtrArray *a)
{
    g_ptr_array_add(a, NULL);
    return (char **)a->pdata;
}

/* ---------------------------------------------------------------------------
 * Listen auf der Platte
 *   erledigt.tsv:     Status \t Größe \t mtime \t neue Größe \t Pfad   (eine Zeile je Datei,
 *                     wird nur angehängt – die letzte Zeile je Pfad gilt)
 *   probe_cache.tsv:  Pfad \t Größe \t mtime \t Analysewerte …
 * ------------------------------------------------------------------------- */
typedef struct {
    char status[12];            /* ok, nogain, failed */
    long long size, mtime, new_size;
} DoneEntry;

static GMutex g_done_lock;

static GHashTable *done_load_counting(long *lines_out)
{
    GHashTable *t = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    long lines = 0;
    if (lines_out) *lines_out = 0;
    FILE *f = fopen(AC_DONE_DB, "r");
    if (!f) return t;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) > 0) {
        if (line[len - 1] == '\n') line[--len] = 0;
        char **fields = g_strsplit(line, "\t", 5);
        if (g_strv_length(fields) == 5) {
            DoneEntry *e = g_new0(DoneEntry, 1);
            g_strlcpy(e->status, fields[0], sizeof e->status);
            e->size = g_ascii_strtoll(fields[1], NULL, 10);
            e->mtime = g_ascii_strtoll(fields[2], NULL, 10);
            e->new_size = g_ascii_strtoll(fields[3], NULL, 10);
            g_hash_table_replace(t, g_strdup(fields[4]), e);
            lines++;
        }
        g_strfreev(fields);
    }
    free(line);
    fclose(f);
    if (lines_out) *lines_out = lines;
    return t;
}

static GHashTable *done_load(void)
{
    return done_load_counting(NULL);
}

/* Liste neu schreiben, wenn sie durch Wiederholungen viele überholte Zeilen hat
 * (nur die jeweils letzte Zeile je Datei gilt). Aufruf mit g_done_lock. */
static void done_compact(GHashTable *t, long lines)
{
    long entries = (long)g_hash_table_size(t);
    if (lines < 1000 || lines < entries * 2) return;
    GString *s = g_string_new(NULL);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, t);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        DoneEntry *e = v;
        g_string_append_printf(s, "%s\t%lld\t%lld\t%lld\t%s\n", e->status, e->size, e->mtime,
                               e->new_size, (char *)k);
    }
    if (g_file_set_contents(AC_DONE_DB, s->str, (gssize)s->len, NULL))
        ac_log(false, "Liste bearbeiteter Dateien verdichtet: %ld → %ld Zeilen", lines, entries);
    g_string_free(s, TRUE);
}

/* Ergebnis im Ausgabeordner merken, damit es später an den Platz des Originals kann:
 * Größe \t mtime des Originals \t Größe des Ergebnisses \t Original \t Ergebnis */
static void pending_append(const AcVideo *v, const char *dest, long long dest_size)
{
    if (strpbrk(v->path, "\t\n") || strpbrk(dest, "\t\n")) return;
    g_mutex_lock(&g_done_lock);
    FILE *f = fopen(PENDING_DB, "a");
    if (f) {
        fprintf(f, "%lld\t%lld\t%lld\t%s\t%s\n", v->size, v->mtime, dest_size, v->path, dest);
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
    g_mutex_unlock(&g_done_lock);
}

/* Ordner samt Eltern anlegen; neu angelegte merken (die werden nach dem Verschieben
 * wieder entfernt, wenn sie leer sind – andere nie) */
static void mkdirs_recording(const char *dir)
{
    GPtrArray *missing = g_ptr_array_new_with_free_func(g_free);
    char *d = g_strdup(dir);
    while (strcmp(d, "/") && strcmp(d, ".") && !g_file_test(d, G_FILE_TEST_EXISTS)) {
        g_ptr_array_add(missing, g_strdup(d));
        char *parent = g_path_get_dirname(d);
        g_free(d);
        d = parent;
    }
    g_free(d);
    g_mkdir_with_parents(dir, 0755);
    if (missing->len) {
        g_mutex_lock(&g_done_lock);
        FILE *f = fopen(CREATED_DIRS, "a");
        for (guint i = missing->len; f && i-- > 0;)    /* Eltern zuerst */
            if (g_file_test(missing->pdata[i], G_FILE_TEST_IS_DIR) && !strchr(missing->pdata[i], '\n'))
                fprintf(f, "%s\n", (char *)missing->pdata[i]);
        if (f) fclose(f);
        g_mutex_unlock(&g_done_lock);
    }
    g_ptr_array_free(missing, TRUE);
}

static void done_append(const char *status, const AcVideo *v, long long new_size)
{
    if (strpbrk(v->path, "\t\n")) return;
    g_mutex_lock(&g_done_lock);
    g_mkdir_with_parents(AC_APP_DIR, 0755);
    FILE *f = fopen(AC_DONE_DB, "a");
    if (f) {
        fprintf(f, "%s\t%lld\t%lld\t%lld\t%s\n", status, v->size, v->mtime, new_size, v->path);
        fflush(f);
        fsync(fileno(f));
        fclose(f);
    }
    g_mutex_unlock(&g_done_lock);
}

int ac_done_count(void)
{
    g_mutex_lock(&g_done_lock);
    GHashTable *t = done_load();
    int n = (int)g_hash_table_size(t);
    g_hash_table_unref(t);
    g_mutex_unlock(&g_done_lock);
    return n;
}

bool ac_done_clear(bool only_failed)
{
    g_mutex_lock(&g_done_lock);
    bool ok = true;
    if (!only_failed) {
        ok = g_remove(AC_DONE_DB) == 0 || errno == ENOENT;
    } else {
        char *content = NULL;
        if (g_file_get_contents(AC_DONE_DB, &content, NULL, NULL)) {
            GString *keep = g_string_new(NULL);
            char **lines = g_strsplit(content, "\n", -1);
            for (int i = 0; lines[i]; i++)
                if (g_str_has_prefix(lines[i], "ok\t")) g_string_append_printf(keep, "%s\n", lines[i]);
            ok = g_file_set_contents(AC_DONE_DB, keep->str, (gssize)keep->len, NULL);
            g_strfreev(lines);
            g_string_free(keep, TRUE);
            g_free(content);
        }
    }
    g_mutex_unlock(&g_done_lock);
    return ok;
}

/* Prüf-Cache: Zeile ohne Pfad als Wert (Größe, mtime und Analyse) */
static GHashTable *probe_cache_load(void)
{
    GHashTable *t = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    FILE *f = fopen(PROBE_CACHE, "r");
    if (!f) return t;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) > 0) {
        if (line[len - 1] == '\n') line[--len] = 0;
        char *tab = strchr(line, '\t');
        if (tab) g_hash_table_replace(t, g_strndup(line, (gsize)(tab - line)), g_strdup(tab + 1));
    }
    free(line);
    fclose(f);
    return t;
}

static void probe_cache_save(GHashTable *t)
{
    GString *s = g_string_new(NULL);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, t);
    while (g_hash_table_iter_next(&it, &k, &v)) g_string_append_printf(s, "%s\t%s\n", (char *)k, (char *)v);
    g_mkdir_with_parents(AC_APP_DIR, 0755);
    g_file_set_contents(PROBE_CACHE, s->str, (gssize)s->len, NULL);
    g_string_free(s, TRUE);
}

static char *probe_serialize(const AcVideo *v)
{
    char fps[32], dur[32], kbps[32];
    g_ascii_formatd(fps, sizeof fps, "%.4f", v->fps);
    g_ascii_formatd(dur, sizeof dur, "%.3f", v->duration);
    g_ascii_formatd(kbps, sizeof kbps, "%.0f", v->kbps);
    return g_strdup_printf("%lld\t%lld\t%s\t%s\t%d\t%d\t%d\t%s\t%s\t%s\t%d\t%d\t%s", v->size, v->mtime,
                           *v->vcodec ? v->vcodec : "-", *v->pix_fmt ? v->pix_fmt : "-", v->vindex,
                           v->width, v->height, fps, dur, kbps, v->interlaced, v->hdr,
                           v->streams && *v->streams ? v->streams : "-");
}

/* -> true, wenn der Eintrag zur Datei passt (Größe und mtime gleich) */
static bool probe_deserialize(const char *s, AcVideo *v)
{
    char **f = g_strsplit(s, "\t", -1);
    bool ok = g_strv_length(f) == 13 && g_ascii_strtoll(f[0], NULL, 10) == v->size &&
              g_ascii_strtoll(f[1], NULL, 10) == v->mtime;
    if (ok) {
        g_strlcpy(v->vcodec, strcmp(f[2], "-") ? f[2] : "", sizeof v->vcodec);
        g_strlcpy(v->pix_fmt, strcmp(f[3], "-") ? f[3] : "", sizeof v->pix_fmt);
        v->vindex = atoi(f[4]);
        v->width = atoi(f[5]);
        v->height = atoi(f[6]);
        v->fps = g_ascii_strtod(f[7], NULL);
        v->duration = g_ascii_strtod(f[8], NULL);
        v->kbps = g_ascii_strtod(f[9], NULL);
        v->interlaced = atoi(f[10]);
        v->hdr = atoi(f[11]);
        v->streams = g_strdup(strcmp(f[12], "-") ? f[12] : "");
    }
    g_strfreev(f);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Analyse (ffprobe)
 * ------------------------------------------------------------------------- */
static double parse_rate(const char *s)
{
    int num = 0, den = 0;
    if (s && sscanf(s, "%d/%d", &num, &den) == 2 && num > 0 && den > 0) return (double)num / den;
    return 0;
}

/* Wert zu key aus einer compact-Zeile "a=1|b=2" */
static const char *field_of(char **fields, const char *key)
{
    size_t kl = strlen(key);
    for (int i = 0; fields[i]; i++)
        if (!strncmp(fields[i], key, kl) && fields[i][kl] == '=') return fields[i] + kl + 1;
    return "";
}

/* Video prüfen -> false, wenn keine Videospur lesbar ist */
static bool probe_video(AcVideo *v)
{
    GPtrArray *a = argv_new();
    argv_add(a, "ffprobe", "-v", "error", "-show_entries",
             "format=duration,bit_rate:stream=index,codec_type,codec_name,width,height,pix_fmt,"
             "field_order,avg_frame_rate,r_frame_rate,channels,color_transfer:"
             "stream_disposition=attached_pic",
             "-of", "compact=p=0", v->path, NULL);
    GString *out = g_string_new(NULL);
    int rc = run_capture(argv_finish(a), PRIO_NORMAL, 120, out, NULL);
    g_ptr_array_free(a, TRUE);
    v->vindex = -1;
    GString *streams = g_string_new(NULL);
    double fmt_duration = 0, fmt_bitrate = 0;
    char **lines = g_strsplit(out->str, "\n", -1);
    for (int i = 0; rc == 0 && lines[i]; i++) {
        if (!*lines[i]) continue;
        char **f = g_strsplit(lines[i], "|", -1);
        const char *type = field_of(f, "codec_type");
        int index = atoi(field_of(f, "index"));
        const char *codec = field_of(f, "codec_name");
        if (!*type && !*codec) {                    /* format-Zeile */
            fmt_duration = g_ascii_strtod(field_of(f, "duration"), NULL);
            fmt_bitrate = g_ascii_strtod(field_of(f, "bit_rate"), NULL);
        } else if (!strcmp(type, "video") && v->vindex < 0 &&
                   strcmp(field_of(f, "disposition:attached_pic"), "1") && atoi(field_of(f, "width")) > 0) {
            v->vindex = index;
            g_strlcpy(v->vcodec, codec, sizeof v->vcodec);
            g_strlcpy(v->pix_fmt, field_of(f, "pix_fmt"), sizeof v->pix_fmt);
            v->width = atoi(field_of(f, "width"));
            v->height = atoi(field_of(f, "height"));
            v->fps = parse_rate(field_of(f, "avg_frame_rate"));
            if (v->fps <= 0 || v->fps > 300) v->fps = parse_rate(field_of(f, "r_frame_rate"));
            const char *fo = field_of(f, "field_order");
            v->interlaced = !strcmp(fo, "tt") || !strcmp(fo, "bb") || !strcmp(fo, "tb") ||
                            !strcmp(fo, "bt");
            const char *trc = field_of(f, "color_transfer");
            v->hdr = !strcmp(trc, "smpte2084") || !strcmp(trc, "arib-std-b67");
        } else if (!strcmp(type, "audio")) {
            g_string_append_printf(streams, "%sa%d:%s:%d", streams->len ? "," : "", index,
                                   *codec ? codec : "unknown", atoi(field_of(f, "channels")));
        } else if (!strcmp(type, "subtitle")) {
            g_string_append_printf(streams, "%ss%d:%s", streams->len ? "," : "", index,
                                   *codec ? codec : "unknown");
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_string_free(out, TRUE);
    v->duration = fmt_duration;
    v->kbps = fmt_bitrate > 0 ? fmt_bitrate / 1000
              : fmt_duration > 0 ? v->size * 8.0 / fmt_duration / 1000 : 0;
    g_free(v->streams);
    v->streams = g_string_free(streams, FALSE);
    if (v->vindex < 0) v->vcodec[0] = 0;
    return v->vindex >= 0;
}

/* ---------------------------------------------------------------------------
 * Suche
 * ------------------------------------------------------------------------- */
static bool name_in(const char *name, const char *const *list)
{
    for (int i = 0; list[i]; i++)
        if (!strcmp(name, list[i])) return true;
    return false;
}

typedef struct {
    const char *exclude;        /* Ausgabeordner (liegt evtl. im Quellordner) */
    GArray *out;                /* AcVideo */
    int skipped_dirs;
    double next_report;         /* nächste Fortschrittsmeldung */
} Walk;

/* rekursiv alle Videos sammeln (keine Symlinks, keine versteckten Einträge) */
static void walk_dir(Walk *w, const char *dir, const char *rel)
{
    if (ac_cancel) return;
    DIR *d = opendir(dir);
    if (!d) {
        ac_log(true, "Ordner nicht lesbar: %s (%s)", dir, g_strerror(errno));
        return;
    }
    struct dirent *e;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;          /* versteckt, Temp-Dateien, .zfs */
        /* Bilder, Untertitel, .nfo & Co.: ohne lstat übergehen (spart Zugriffe aufs NAS) */
        if ((e->d_type == DT_REG || e->d_type == DT_LNK) && !has_video_ext(e->d_name)) continue;
        g_ptr_array_add(names, g_strdup(e->d_name));
    }
    closedir(d);
    g_ptr_array_sort_values(names, (GCompareFunc)g_strcmp0);
    for (guint i = 0; i < names->len && !ac_cancel; i++) {
        const char *name = names->pdata[i];
        char *path = g_build_filename(dir, name, NULL);
        char *r = *rel ? g_build_filename(rel, name, NULL) : g_strdup(name);
        struct stat st;
        if (lstat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (name_in(name, SKIP_DIRS)) {
                    w->skipped_dirs++;
                    ac_log(false, "Übersprungen (Disc-/Systemordner): %s", path);
                } else if (!w->exclude || strcmp(path, w->exclude)) {
                    walk_dir(w, path, r);
                }
            } else if (S_ISREG(st.st_mode) && has_video_ext(name) && st.st_size > 0) {
                AcVideo v = {0};
                v.path = path;
                v.rel = r;
                v.size = st.st_size;
                v.mtime = st.st_mtime;
                v.vindex = -1;
                g_array_append_val(w->out, v);
                path = r = NULL;
                if (ac_fe.scan_progress && now_secs() > w->next_report) {
                    w->next_report = now_secs() + 0.25;
                    ac_fe.scan_progress((int)w->out->len, -1);      /* -1: Ordner werden gelesen */
                }
            }
        }
        g_free(path);
        g_free(r);
    }
    g_ptr_array_free(names, TRUE);
}

typedef struct {
    GMutex lock;
    int probed;
    GHashTable *cache;
} ProbeShared;

static void probe_worker(gpointer data, gpointer user)
{
    AcVideo *v = data;
    ProbeShared *ps = user;
    if (!ac_cancel) {
        bool ok = probe_video(v);
        if (ok && !strpbrk(v->path, "\t\n")) {   /* Fehlschläge (z. B. NAS-Aussetzer) nicht merken */
            char *ser = probe_serialize(v);
            g_mutex_lock(&ps->lock);
            g_hash_table_replace(ps->cache, g_strdup(v->path), ser);
            g_mutex_unlock(&ps->lock);
        }
        if (!ok) v->state = AC_SKIP_UNREADABLE;
    }
    g_mutex_lock(&ps->lock);
    ps->probed++;
    g_mutex_unlock(&ps->lock);
}

static int compare_size_desc(gconstpointer a, gconstpointer b)
{
    const AcVideo *x = a, *y = b;
    return x->size < y->size ? 1 : x->size > y->size ? -1 : strcmp(x->rel, y->rel);
}

/* Name ohne Endung */
static char *stem_of(const char *path)
{
    char *base = g_path_get_basename(path);
    char *dot = strrchr(base, '.');
    if (dot && dot != base) *dot = 0;
    return base;
}

/* Ziel (.mkv) einer Datei: neben dem Original bzw. gespiegelt im Ausgabeordner */
static char *dest_path_for(const AcVideo *v, const AcOptions *o)
{
    char *stem = stem_of(v->path), *name = g_strconcat(stem, ".mkv", NULL), *dir;
    if (o->replace) {
        dir = g_path_get_dirname(v->path);
    } else {
        char *rel_dir = g_path_get_dirname(v->rel);
        dir = g_build_filename(o->out_dir, strcmp(rel_dir, ".") ? rel_dir : "", NULL);
        g_free(rel_dir);
    }
    char *dest = g_build_filename(dir, name, NULL);
    g_free(stem);
    g_free(name);
    g_free(dir);
    return dest;
}

static void adopt_results(GArray *found, const AcOptions *o);

int ac_scan(const char *src, const AcOptions *o, AcScan *s, const char **err)
{
    memset(s, 0, sizeof *s);
    double t0 = now_secs();
    if (!g_file_test(src, G_FILE_TEST_IS_DIR)) {
        *err = "Der Quellordner wurde nicht gefunden.";
        return -1;
    }
    char *real_src = realpath(src, NULL);
    char *real_out = !o->replace && o->out_dir ? realpath(o->out_dir, NULL) : NULL;
    GArray *found = g_array_new(FALSE, TRUE, sizeof(AcVideo));
    Walk w = {.exclude = real_out, .out = found};
    char *root = g_strdup(real_src ? real_src : src);
    walk_dir(&w, root, "");
    free(real_src);
    free(real_out);
    if (w.skipped_dirs)
        ac_log(true, "%d Disc- bzw. Systemordner (BDMV, VIDEO_TS, …) wurden übersprungen.",
               w.skipped_dirs);
    ac_log(false, "Suche: %u Videos in %.1f s gefunden", found->len, now_secs() - t0);
    if (ac_fe.scan_progress) ac_fe.scan_progress((int)found->len, 0);

    /* prüfen: aus dem Cache oder per ffprobe (parallel) */
    ProbeShared ps = {.cache = probe_cache_load()};
    g_mutex_init(&ps.lock);
    GThreadPool *pool = g_thread_pool_new(probe_worker, &ps,
                                           MAX(PROBE_THREADS_MIN, (int)g_get_num_processors()),
                                           FALSE, NULL);
    int from_cache = 0;
    for (guint i = 0; i < found->len; i++) {
        AcVideo *v = &g_array_index(found, AcVideo, i);
        const char *cached = g_hash_table_lookup(ps.cache, v->path);
        if (cached && probe_deserialize(cached, v)) {
            if (v->vindex < 0) v->state = AC_SKIP_UNREADABLE;
            ps.probed++;
            from_cache++;
        } else {
            g_thread_pool_push(pool, v, NULL);
        }
    }
    double next_save = now_secs() + 60;
    while (g_thread_pool_unprocessed(pool) > 0 || g_thread_pool_get_num_threads(pool) > 0) {
        g_mutex_lock(&ps.lock);
        int probed = ps.probed;
        if (now_secs() > next_save) {               /* Zwischenstand sichern (Absturz, Abbruch) */
            probe_cache_save(ps.cache);
            next_save = now_secs() + 60;
        }
        g_mutex_unlock(&ps.lock);
        if (ac_fe.scan_progress) ac_fe.scan_progress((int)found->len, probed);
        if (probed >= (int)found->len) break;
        g_usleep(150 * 1000);
    }
    g_thread_pool_free(pool, FALSE, TRUE);
    if (ac_fe.scan_progress) ac_fe.scan_progress((int)found->len, (int)found->len);
    if (!ac_cancel) {           /* Einträge gelöschter/verschobener Videos unter dieser Quelle */
        GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
        for (guint i = 0; i < found->len; i++)
            g_hash_table_add(seen, g_array_index(found, AcVideo, i).path);
        char *prefix = g_strconcat(root, "/", NULL);
        GHashTableIter it;
        gpointer key;
        int removed = 0;
        g_hash_table_iter_init(&it, ps.cache);
        while (g_hash_table_iter_next(&it, &key, NULL))
            if (g_str_has_prefix(key, prefix) && !g_hash_table_contains(seen, key)) {
                g_hash_table_iter_remove(&it);
                removed++;
            }
        if (removed) ac_log(false, "Prüf-Cache: %d veraltete Einträge entfernt", removed);
        g_free(prefix);
        g_hash_table_unref(seen);
    }
    g_free(root);
    probe_cache_save(ps.cache);
    g_hash_table_unref(ps.cache);
    g_mutex_clear(&ps.lock);
    ac_log(false, "Suche: %u Videos, %d aus dem Cache", found->len, from_cache);
    if (ac_cancel) {
        s->v = (AcVideo *)found->data;
        s->n = (int)found->len;
        g_array_free(found, FALSE);
        return 1;
    }

    /* einordnen: schon AV1, früher erledigt, schon im Ausgabeordner */
    long done_lines = 0;
    g_mutex_lock(&g_done_lock);
    GHashTable *done = done_load_counting(&done_lines);
    done_compact(done, done_lines);
    g_mutex_unlock(&g_done_lock);
    for (guint i = 0; i < found->len; i++) {
        AcVideo *v = &g_array_index(found, AcVideo, i);
        if (v->state != AC_TODO) continue;
        DoneEntry *d = g_hash_table_lookup(done, v->path);
        if (!strcmp(v->vcodec, "av1")) {
            v->state = AC_SKIP_AV1;
        } else if (d && d->size == v->size && d->mtime == v->mtime) {
            /* "nogain": die GPU (bzw. eine ältere Version) brachte keinen Gewinn – im Modus
             * „GPU + CPU parallel“ versucht es HandBrake, das schafft bei stark komprimierten
             * Videos oft noch 13–25 %; "nogain-cpu": auch HandBrake nicht, endgültig */
            if (!strcmp(d->status, "nogain") && o->mode == AC_MODE_HYBRID) v->cpu_only = true;
            else
                v->state = !strcmp(d->status, "ok") ? AC_SKIP_DONE
                           : g_str_has_prefix(d->status, "nogain") ? AC_SKIP_NOGAIN : AC_SKIP_FAILED;
        } else if (!o->replace) {
            char *dest = dest_path_for(v, o);
            if (g_file_test(dest, G_FILE_TEST_EXISTS)) v->state = AC_SKIP_EXISTS;
            g_free(dest);
        }
    }
    g_hash_table_unref(done);
    for (guint i = 0; i < found->len; i++) {       /* HDR vollständig per HandBrake */
        AcVideo *v = &g_array_index(found, AcVideo, i);
        if (v->state == AC_TODO && hdr_to_handbrake(v, o)) v->cpu_only = true;
    }
    if (!o->replace) adopt_results(found, o);
    g_array_sort(found, compare_size_desc);
    s->n = (int)found->len;
    s->v = (AcVideo *)g_array_free(found, FALSE);
    for (int i = 0; i < s->n; i++) {
        s->count[s->v[i].state]++;
        s->bytes[s->v[i].state] += s->v[i].size;
    }
    s->secs = now_secs() - t0;
    return 0;
}

void ac_scan_free(AcScan *s)
{
    for (int i = 0; i < s->n; i++) {
        g_free(s->v[i].path);
        g_free(s->v[i].rel);
        g_free(s->v[i].streams);
    }
    g_free(s->v);
    memset(s, 0, sizeof *s);
}

/* ---------------------------------------------------------------------------
 * HandBrake und GPU erkennen
 * ------------------------------------------------------------------------- */
static AcHandbrake g_hb;                /* zuletzt erkannt (für Scan und Befehle) */

/* HandBrake-Aufruf: Flatpak oder Paket */
static void hb_prefix(GPtrArray *a, const AcHandbrake *hb)
{
    if (hb->flatpak) argv_add(a, "flatpak", "run", "--die-with-parent", "--command=HandBrakeCLI",
                              FLATPAK_ID, NULL);
    else argv_add(a, "HandBrakeCLI", NULL);
}

/* "\"Key\": Wert" aus dem exportierten Profil (einfaches JSON) */
static char *json_value(const char *json, const char *key)
{
    char *pat = g_strdup_printf("\"%s\":", key);
    const char *p = strstr(json, pat);
    g_free(pat);
    if (!p) return NULL;
    p = strchr(p, ':') + 1;
    while (*p == ' ') p++;
    if (*p == '"') {
        const char *e = strchr(p + 1, '"');
        return e ? g_strndup(p + 1, (gsize)(e - p - 1)) : NULL;
    }
    size_t n = strcspn(p, ",}\n");
    return g_strstrip(g_strndup(p, n));
}

bool ac_handbrake_detect(AcHandbrake *hb)
{
    memset(hb, 0, sizeof *hb);
    GString *out = g_string_new(NULL);
    char *info_argv[] = {"flatpak", "info", FLATPAK_ID, NULL};
    hb->flatpak = command_exists("flatpak") && run_capture(info_argv, PRIO_NORMAL, 20, out, NULL) == 0;
    if (!hb->flatpak && !command_exists("HandBrakeCLI")) {
        g_string_free(out, TRUE);
        return false;
    }
    GPtrArray *a = argv_new();
    hb_prefix(a, hb);
    argv_add(a, "--version", NULL);
    g_string_truncate(out, 0);
    run_capture(argv_finish(a), PRIO_NORMAL, 60, out, NULL);
    g_ptr_array_free(a, TRUE);
    const char *ver = strstr(out->str, "HandBrake ");
    if (ver) sscanf(ver + 10, "%31s", hb->version);
    /* Profil exportieren und Kernwerte auslesen (ab Version 1.11: Preset 4, RF 35) */
    g_mkdir_with_parents(AC_APP_DIR, 0755);
    g_remove(PRESET_EXPORT);
    a = argv_new();
    hb_prefix(a, hb);
    argv_add(a, "-Z", AC_PRESET, "--preset-export", "av1-converter", "--preset-export-file",
             PRESET_EXPORT, NULL);
    run_capture(argv_finish(a), PRIO_NORMAL, 60, NULL, NULL);
    g_ptr_array_free(a, TRUE);
    g_string_free(out, TRUE);
    char *json = NULL;
    if (g_file_get_contents(PRESET_EXPORT, &json, NULL, NULL)) {
        char *enc = json_value(json, "VideoEncoder"), *preset = json_value(json, "VideoPreset"),
             *rf = json_value(json, "VideoQualitySlider");
        hb->available = enc && strstr(enc, "av1");
        if (enc) g_strlcpy(hb->encoder, enc, sizeof hb->encoder);
        if (preset) g_strlcpy(hb->svt_preset, preset, sizeof hb->svt_preset);
        hb->rf = rf ? g_ascii_strtod(rf, NULL) : DEFAULT_RF;
        g_free(enc);
        g_free(preset);
        g_free(rf);
        g_free(json);
    }
    if (hb->available) g_hb = *hb;
    ac_log(false, "HandBrake: %s %s (%s), Profil: %s, Preset %s, RF %.1f",
           hb->available ? "ja" : "nein", hb->version, hb->flatpak ? "Flatpak" : "Paket",
           hb->encoder, hb->svt_preset, hb->rf);
    return hb->available;
}

bool ac_gpu_detect(AcGpu *gpu)
{
    memset(gpu, 0, sizeof *gpu);
    glob_t g;
    if (glob("/dev/dri/renderD*", 0, NULL, &g) != 0) return false;
    for (size_t i = 0; i < g.gl_pathc && !gpu->available; i++) {
        GPtrArray *a = argv_new();
        argv_add(a, "ffmpeg", "-hide_banner", "-nostdin", "-v", "verbose", "-vaapi_device",
                 g.gl_pathv[i], "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=30:duration=0.5",
                 "-vf", "format=p010,hwupload", "-c:v", "av1_vaapi", "-f", "null", "-", NULL);
        GString *err = g_string_new(NULL);
        int rc = run_capture(argv_finish(a), PRIO_NORMAL, 30, NULL, err);
        g_ptr_array_free(a, TRUE);
        if (rc == 0) {
            gpu->available = true;
            g_strlcpy(gpu->device, g.gl_pathv[i], sizeof gpu->device);
            const char *drv = strstr(err->str, "VAAPI driver: ");
            const char *for_ = drv ? strstr(drv, " for ") : NULL;
            if (for_) {
                for_ += 5;
                size_t n = strcspn(for_, "(\n");
                char *name = g_strstrip(g_strndup(for_, n));
                g_strlcpy(gpu->name, name, sizeof gpu->name);
                g_free(name);
            }
            if (!*gpu->name) g_strlcpy(gpu->name, "VAAPI-GPU", sizeof gpu->name);
        }
        g_string_free(err, TRUE);
    }
    globfree(&g);
    ac_log(false, "GPU: %s %s", gpu->available ? gpu->name : "keine (AV1-Encode per VAAPI fehlt)",
           gpu->device);
    return gpu->available;
}

bool ac_have_ffmpeg(void)
{
    return command_exists("ffmpeg") && command_exists("ffprobe");
}

/* ---------------------------------------------------------------------------
 * Zuschnitt (schwarze Balken) für den GPU-Pfad – mit HandBrakes eigenem Scan,
 * damit das Ergebnis dem Profil entspricht; ohne HandBrake per cropdetect
 * ------------------------------------------------------------------------- */
typedef struct {
    int top, bottom, left, right;
} Crop;

static bool crop_by_handbrake(const char *path, Crop *c)
{
    if (!g_hb.available) return false;
    GPtrArray *a = argv_new();
    hb_prefix(a, &g_hb);
    argv_add(a, "-i", path, "--scan", "-t", "1", NULL);
    GString *err = g_string_new(NULL);
    int rc = run_capture(argv_finish(a), PRIO_GPU, 600, NULL, err);
    g_ptr_array_free(a, TRUE);
    const char *p = strstr(err->str, "+ autocrop: ");
    bool ok = rc == 0 && p && sscanf(p + 12, "%d/%d/%d/%d", &c->top, &c->bottom, &c->left, &c->right) == 4;
    g_string_free(err, TRUE);
    return ok;
}

static bool crop_by_cropdetect(const AcVideo *v, Crop *c)
{
    int x0 = v->width, y0 = v->height, x1 = 0, y1 = 0, samples = 0;
    for (int i = 1; i <= 10 && !ac_cancel; i++) {
        char t[32];
        g_ascii_formatd(t, sizeof t, "%.2f", v->duration * i / 11.0);
        GPtrArray *a = argv_new();
        argv_add(a, "ffmpeg", "-hide_banner", "-nostdin", "-ss", t, "-i", v->path, "-map", NULL);
        argv_addf(a, "0:%d", v->vindex);
        argv_add(a, "-frames:v", "8", "-vf", "cropdetect=limit=0.094:round=2:reset=0", "-f",
                 "null", "-", NULL);
        GString *err = g_string_new(NULL);
        run_capture(argv_finish(a), PRIO_GPU, 120, NULL, err);
        g_ptr_array_free(a, TRUE);
        const char *last = NULL, *p = err->str;
        while ((p = strstr(p, "crop="))) last = p++;
        int w, h, x, y;
        if (last && sscanf(last, "crop=%d:%d:%d:%d", &w, &h, &x, &y) == 4 && w > 0 && h > 0) {
            x0 = MIN(x0, x);
            y0 = MIN(y0, y);
            x1 = MAX(x1, x + w);
            y1 = MAX(y1, y + h);
            samples++;
        }
        g_string_free(err, TRUE);
    }
    if (samples < 3) return false;
    *c = (Crop){y0, v->height - y1, x0, v->width - x1};
    return true;
}

/* ---------------------------------------------------------------------------
 * Befehle
 * ------------------------------------------------------------------------- */
/* Ton/Untertitel aus AcVideo.streams: "a1:aac:2,s3:subrip" */
typedef struct {
    char kind;                  /* 'a' oder 's' */
    int index, channels;
    char codec[32];
} StreamInfo;

static int parse_streams(const char *s, StreamInfo *out, int max)
{
    int n = 0;
    char **items = g_strsplit(s ? s : "", ",", -1);
    for (int i = 0; items[i] && n < max; i++) {
        StreamInfo si = {0};
        char codec[32] = "";
        if (sscanf(items[i], "%c%d:%31[^:]:%d", &si.kind, &si.index, codec, &si.channels) >= 3) {
            g_strlcpy(si.codec, codec, sizeof si.codec);
            out[n++] = si;
        }
    }
    g_strfreev(items);
    return n;
}

/* Matroska kann den Untertitel aufnehmen: "copy", umwandeln: "srt", sonst NULL */
static const char *subtitle_codec_for_mkv(const char *codec)
{
    static const char *const copy[] = {"subrip", "ass", "ssa", "hdmv_pgs_subtitle",
                                       "dvd_subtitle", "dvb_subtitle", "webvtt", "text", NULL};
    if (g_strv_contains(copy, codec)) return "copy";
    if (!strcmp(codec, "mov_text")) return "srt";
    return NULL;
}

/* HandBrake mit dem Profil (CPU) */
static GPtrArray *build_handbrake_cmd(const AcVideo *v, const AcOptions *o, const char *tmp)
{
    GPtrArray *a = argv_new();
    hb_prefix(a, &g_hb);
    argv_add(a, "-Z", AC_PRESET, "-i", v->path, "-o", tmp, "--format", "av_mkv", NULL);
    if (fabs(o->rf - g_hb.rf) > 0.01) {
        char rf[32];
        g_ascii_formatd(rf, sizeof rf, "%.1f", o->rf);
        argv_add(a, "-q", rf, NULL);
    }
    if (o->audio == AC_AUDIO_ALL_OPUS)
        argv_add(a, "--all-audio", NULL);
    else if (o->audio == AC_AUDIO_ALL_COPY)
        argv_add(a, "--all-audio", "-E", "copy", "--audio-copy-mask",
                 "aac,ac3,eac3,truehd,dts,dtshd,mp2,mp3,flac,opus", "--audio-fallback", "opus", NULL);
    if (o->subs == AC_SUBS_ALL) argv_add(a, "--all-subtitles", "--subtitle-burned=none", NULL);
    return a;
}

/* Wer dekodiert bei einem GPU-Job? Die GPU spart viel CPU (4K-HEVC: 2,8 statt 9 Kerne bei
 * gleichem Tempo), ist bei H.264 aber langsamer (1080p: ~500 statt ~850 Bilder/s), weil die
 * Bilder für den Zuschnitt zurückkopiert werden. Also: HEVC/VP9 immer per GPU, H.264 nur im
 * Hybridmodus (dann braucht HandBrake die CPU). Interlaced-Material und Formate, die VAAPI
 * nicht dekodiert (z. B. H.264 10 Bit, 4:2:2), bleiben bei der CPU. */
static bool want_hw_decode(const AcVideo *v, AcMode mode)
{
    static const char *const fmts[] = {"yuv420p", "yuvj420p", "yuv420p10le", NULL};
    if (v->interlaced || !g_strv_contains(fmts, v->pix_fmt)) return false;
    if (!strcmp(v->vcodec, "hevc") || !strcmp(v->vcodec, "vp9")) return true;
    return mode == AC_MODE_HYBRID && !strcmp(v->vcodec, "h264") && strcmp(v->pix_fmt, "yuv420p10le");
}

/* Filterkette für ffmpeg: Deinterlace, Zuschnitt, höchstens 2160p.
 * hw_decode: Bilder kommen als VAAPI-Flächen – auf der GPU nach P010 wandeln, für den
 * Zuschnitt herunterladen (ffmpeg 6.1 kann VAAPI-Bilder nicht zuschneiden, und ohne den
 * Umweg verliert scale_vaapi am Dateiende Bilder) und wieder hochladen. */
static char *build_filters(const AcVideo *v, const Crop *c, bool hw_upload, bool hw_decode,
                           bool *cropped)
{
    GString *f = g_string_new(NULL);
    if (hw_decode) g_string_append(f, "scale_vaapi=format=p010,hwdownload,format=p010,");
    else if (v->interlaced) g_string_append(f, "bwdif=mode=send_frame:deint=interlaced,");
    int w = v->width - c->left - c->right, h = v->height - c->top - c->bottom;
    *cropped = false;
    if (w > 16 && h > 16 && (w != v->width || h != v->height)) {
        g_string_append_printf(f, "crop=%d:%d:%d:%d,", w & ~1, h & ~1, c->left, c->top);
        *cropped = true;
    } else {
        w = v->width;
        h = v->height;
    }
    if (w > MAX_W || h > MAX_H)
        g_string_append_printf(f, "scale=w=%d:h=%d:force_original_aspect_ratio=decrease:"
                               "force_divisible_by=2,", MAX_W, MAX_H);
    g_string_append(f, hw_upload ? "format=p010,hwupload" : "format=yuv420p10le");
    return g_string_free(f, FALSE);
}

/* ffmpeg: GPU (VAAPI) oder – ohne HandBrake – CPU mit libsvtav1 */
static GPtrArray *build_ffmpeg_cmd(const AcVideo *v, const AcOptions *o, const AcGpu *gpu,
                                   bool use_gpu, bool hw_decode, const Crop *c, const char *tmp,
                                   bool *cropped)
{
    GPtrArray *a = argv_new();
    argv_add(a, "ffmpeg", "-hide_banner", "-nostdin", "-y", "-loglevel", "error", "-progress",
             "pipe:1", "-nostats", NULL);
    if (use_gpu) argv_add(a, "-vaapi_device", gpu->device, NULL);
    if (use_gpu && hw_decode) argv_add(a, "-hwaccel", "vaapi", "-hwaccel_output_format", "vaapi", NULL);
    argv_add(a, "-i", v->path, "-map", NULL);
    argv_addf(a, "0:%d", v->vindex);
    StreamInfo st[64];
    int ns = parse_streams(v->streams, st, 64), audio_out = 0, sub_out = 0;
    GPtrArray *codec_args = argv_new();
    for (int i = 0; i < ns; i++) {
        if (st[i].kind == 'a') {
            if (o->audio == AC_AUDIO_PRESET && audio_out > 0) continue;
            argv_add(a, "-map", NULL);
            argv_addf(a, "0:%d", st[i].index);
            if (o->audio == AC_AUDIO_ALL_COPY) {
                argv_addf(codec_args, "-c:a:%d", audio_out);
                argv_add(codec_args, "copy", NULL);
            } else {                                /* Profil: Opus 160 kbit/s Stereo */
                argv_addf(codec_args, "-c:a:%d", audio_out);
                argv_add(codec_args, "libopus", NULL);
                argv_addf(codec_args, "-b:a:%d", audio_out);
                argv_add(codec_args, "160k", NULL);
                argv_addf(codec_args, "-ac:a:%d", audio_out);
                argv_add(codec_args, st[i].channels == 1 ? "1" : "2", NULL);
            }
            audio_out++;
        } else if (st[i].kind == 's' && o->subs == AC_SUBS_ALL) {
            const char *sc = subtitle_codec_for_mkv(st[i].codec);
            if (!sc) continue;
            argv_add(a, "-map", NULL);
            argv_addf(a, "0:%d", st[i].index);
            argv_addf(codec_args, "-c:s:%d", sub_out++);
            argv_add(codec_args, sc, NULL);
        }
    }
    if (o->subs == AC_SUBS_ALL) argv_add(a, "-map", "0:t?", "-c:t", "copy", NULL);  /* Schriften */
    argv_add(a, "-map_metadata", "0", "-map_chapters", "0", NULL);
    char *vf = build_filters(v, c, use_gpu, use_gpu && hw_decode, cropped);
    argv_add(a, "-vf", vf, "-fpsmax", G_STRINGIFY(MAX_FPS), NULL);
    g_free(vf);
    int gop = (int)lround(MIN(v->fps > 0 ? v->fps : 30, MAX_FPS) * 10);   /* keyint=10s */
    if (use_gpu) {
        argv_add(a, "-c:v", "av1_vaapi", "-rc_mode", "CQP", "-global_quality:v", NULL);
        argv_addf(a, "%d", o->gpu_quality);
    } else {
        argv_add(a, "-c:v", "libsvtav1", "-preset", *g_hb.svt_preset ? g_hb.svt_preset : "4",
                 "-crf", NULL);
        argv_addf(a, "%d", (int)lround(o->rf));
        argv_add(a, "-svtav1-params", "tune=1", NULL);
    }
    argv_add(a, "-g:v", NULL);
    argv_addf(a, "%d", MAX(gop, 10));
    for (guint i = 0; i < codec_args->len; i++) g_ptr_array_add(a, g_strdup(codec_args->pdata[i]));
    g_ptr_array_free(codec_args, TRUE);
    argv_add(a, "-max_muxing_queue_size", "4096", "-f", "matroska", tmp, NULL);
    return a;
}

/* ---------------------------------------------------------------------------
 * Laufende Jobs, Pause, Watchdog
 * ------------------------------------------------------------------------- */
/* ---------------------------------------------------------------------------
 * Restdauer: Modell aus Messungen.
 * Aufwand einer Datei = Bilder × Bildpunkte / Durchsatz + Fixkosten (Analyse,
 * Start, Prüfung). Während des Laufs wird das Modell an das tatsächliche Tempo
 * angepasst (siehe ac_progress).
 * ------------------------------------------------------------------------- */
#define EST_GPU_PX 1.7e9        /* Bildpunkte/s aller GPU-Jobs zusammen (1080p: ~850 Bilder/s) */
#define EST_GPU_PX_4K 1.0e9     /* ... bei 4K (100–180 Bilder/s) */
#define EST_GPU_ONE_JOB 0.68    /* nur 1 GPU-Job: 575 statt 850 Bilder/s */
#define EST_HYBRID 0.45         /* GPU neben HandBrake: ~310–400 statt ~850 Bilder/s */
#define EST_CPU_PX 45e6         /* HandBrake: 1080p ~21,5 Bilder/s */
#define EST_CPU_PX_4K 65e6      /* ... 4K ~8 Bilder/s */
#define EST_CPU_MORE_JOBS 1.16  /* 2 und mehr HandBrake-Jobs zusammen */
#define EST_FILE_SECS_GPU 2.5   /* Fixkosten je Datei: Balkenerkennung, Start, Prüfung ... */
#define EST_FILE_SECS_CPU 6.0   /* ... bei HandBrake (Scan im Encoder) */
#define EST_RECENT_SECS 1800    /* jüngeres Tempo: Fenster der letzten 30 min ... */
#define EST_RECENT_MIN 300      /* ... zählt, sobald es 5 min umfasst */

double ac_estimate_secs(const AcVideo *v, const AcOptions *o, bool gpu_available)
{
    double fps = v->fps > 0 && v->fps < 300 ? MIN(v->fps, 60) : 25;
    double frames = MAX(v->duration, 1) * fps;
    double w = MAX(v->width, 16), h = MAX(v->height, 16);
    if (w > 3840 || h > 2160) {                     /* wird auf 2160p verkleinert */
        double f = MIN(3840 / w, 2160 / h);
        w *= f;
        h *= f;
    }
    double px = w * h, big = px > 3.0e6;
    bool gpu = gpu_available && o->mode != AC_MODE_CPU && !v->cpu_only &&
               !hdr_to_handbrake(v, o);
    if (gpu) {
        int jobs = MAX(o->gpu_jobs, 1);
        double rate = (big ? EST_GPU_PX_4K : EST_GPU_PX) * (jobs == 1 ? EST_GPU_ONE_JOB : 1);
        if (o->mode == AC_MODE_HYBRID) rate *= EST_HYBRID;
        return frames * px / rate + EST_FILE_SECS_GPU / jobs;
    }
    int jobs = MAX(o->cpu_jobs, 1);
    double rate = (big ? EST_CPU_PX_4K : EST_CPU_PX) * (jobs > 1 ? EST_CPU_MORE_JOBS : 1);
    return frames * px / rate + EST_FILE_SECS_CPU / jobs;
}

/* Tempo früherer Läufe: tatsächliche Dauer / Modell, je Modus in ~/.av1-converter/tempo.ini.
 * So berücksichtigt schon die erste Schätzung, wie schnell es auf diesem PC wirklich geht
 * (andere Programme nebenher, NAS, Material). 1 = wie gemessen. */
static const char *const MODE_KEY[3] = {"cpu", "gpu", "hybrid"};

static char *tempo_path(void)
{
    return g_build_filename(AC_APP_DIR, "tempo.ini", NULL);
}

static double tempo_factor(AcMode mode)
{
    GKeyFile *kf = g_key_file_new();
    char *path = tempo_path();
    double f = 1;
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        double v = g_key_file_get_double(kf, "Tempo", MODE_KEY[mode], NULL);
        if (v > 0.2 && v < 50) f = v;
    }
    g_free(path);
    g_key_file_unref(kf);
    return f;
}

/* gemessenes Verhältnis merken (gleitend: die Hälfte altes, die Hälfte neues) */
static void tempo_remember(AcMode mode, double measured)
{
    if (!(measured > 0.2 && measured < 50)) return;
    GKeyFile *kf = g_key_file_new();
    char *path = tempo_path();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    double old = g_key_file_get_double(kf, "Tempo", MODE_KEY[mode], NULL);
    g_key_file_set_double(kf, "Tempo", MODE_KEY[mode], old > 0.2 && old < 50 ? (old + measured) / 2
                                                                              : measured);
    g_mkdir_with_parents(AC_APP_DIR, 0755);
    g_key_file_save_to_file(kf, path, NULL);
    g_free(path);
    g_key_file_unref(kf);
}

static double estimate_raw(AcVideo *const *sel, int n, const AcOptions *o, bool gpu_available)
{
    double t = 0;
    for (int i = 0; i < n; i++) t += ac_estimate_secs(sel[i], o, gpu_available);
    return t;
}

double ac_estimate_total(AcVideo *const *sel, int n, const AcOptions *o, bool gpu_available)
{
    AcMode mode = gpu_available ? o->mode : AC_MODE_CPU;
    return estimate_raw(sel, n, o, gpu_available) * tempo_factor(mode);
}

typedef struct {
    const AcVideo *v;
    bool gpu, fallback, hw_decode;
    double est;                 /* geschätzter Aufwand (s) laut Modell */
    char what[64];
    double pct, fps;
    double start, phase_start;  /* active_clock() */
    double last_progress;       /* active_clock() der letzten Änderung */
    GPid pid;                   /* 0 = kein Prozess */
} Job;

static struct {
    GMutex lock;
    GPtrArray *jobs;            /* Job * */
    bool active, paused;
    double pause_started, paused_total;
    double t0;
    double bytes_total, bytes_done;
    long long in_ok, out_ok;
    int files_done, files_total;
    double cpu_load;
    bool cpu_valid;
    GPtrArray *temps;           /* char *: Temp-Dateien laufender Jobs */
    AcOptions opts;             /* für die Schätzung */
    bool gpu_ok;
    double est_total, est_done; /* geschätzter Aufwand insgesamt / fertiger Dateien */
    double factor;              /* Tempo früherer Läufe (für die Schätzung vor der ersten Messung) */
    double sample_t[64], sample_done[64];       /* Verlauf für das jüngere Tempo */
    int samples, sample_next;
    double next_sample;
} run = {.jobs = NULL};

/* Laufzeit ohne Pausen */
static double active_clock(void)
{
    double t = now_secs() - run.paused_total;
    if (run.paused) t -= now_secs() - run.pause_started;
    return t;
}

static Job *job_new(const AcVideo *v, bool gpu, bool hw_decode, bool fallback)
{
    Job *j = g_new0(Job, 1);
    j->v = v;
    j->gpu = gpu;
    j->hw_decode = gpu && hw_decode;
    j->fallback = fallback;
    g_mutex_lock(&run.lock);
    j->start = j->phase_start = j->last_progress = active_clock();
    g_strlcpy(j->what, "Start", sizeof j->what);
    j->est = ac_estimate_secs(v, &run.opts, run.gpu_ok);
    g_ptr_array_add(run.jobs, j);
    g_mutex_unlock(&run.lock);
    return j;
}

static void job_free(Job *j)
{
    g_mutex_lock(&run.lock);
    g_ptr_array_remove(run.jobs, j);
    g_mutex_unlock(&run.lock);
    g_free(j);
}

static void job_phase(Job *j, const char *what)
{
    g_mutex_lock(&run.lock);
    g_strlcpy(j->what, what, sizeof j->what);
    j->pct = j->fps = 0;
    j->phase_start = j->last_progress = active_clock();
    g_mutex_unlock(&run.lock);
}

static void job_progress(Job *j, double pct, double fps)
{
    g_mutex_lock(&run.lock);
    if (pct > j->pct + 0.001 || fps != j->fps) j->last_progress = active_clock();
    j->pct = CLAMP(pct, 0, 100);
    if (fps >= 0) j->fps = fps;
    g_mutex_unlock(&run.lock);
}

void ac_set_paused(bool on)
{
    g_mutex_lock(&run.lock);
    if (on != run.paused) {
        if (on) run.pause_started = now_secs();
        else run.paused_total += now_secs() - run.pause_started;
        run.paused = on;
        for (guint i = 0; run.jobs && i < run.jobs->len; i++) {
            Job *j = run.jobs->pdata[i];
            if (j->pid) signal_tree(j->pid, on ? SIGSTOP : SIGCONT);
        }
    }
    g_mutex_unlock(&run.lock);
}

bool ac_paused(void)
{
    g_mutex_lock(&run.lock);
    bool p = run.paused;
    g_mutex_unlock(&run.lock);
    return p;
}

static void temp_register(const char *path)
{
    g_mutex_lock(&run.lock);
    if (!run.temps) run.temps = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(run.temps, g_strdup(path));
    g_mutex_unlock(&run.lock);
}

static void temp_remove(const char *path)
{
    g_remove(path);
    g_mutex_lock(&run.lock);
    for (guint i = 0; run.temps && i < run.temps->len; i++)
        if (!strcmp(run.temps->pdata[i], path)) {
            g_ptr_array_remove_index_fast(run.temps, i);
            break;
        }
    g_mutex_unlock(&run.lock);
}

int ac_cleanup_temps(void)
{
    int n = 0;
    g_mutex_lock(&run.lock);
    for (guint i = 0; run.temps && i < run.temps->len; i++)
        if (g_remove(run.temps->pdata[i]) == 0) n++;
    if (run.temps) g_ptr_array_set_size(run.temps, 0);
    g_mutex_unlock(&run.lock);
    return n;
}

/* Ausgabe eines Encoders auswerten */
typedef struct {
    Job *job;
    bool handbrake;
    double duration;
    GString *err_tail;          /* letzte Fehlerausgabe */
    const char *tmp;            /* entstehende Datei (für die Hochrechnung) */
    double max_bytes;           /* Zielgröße: größer = kein Gewinn (0 = nicht prüfen) */
    double projected;           /* Hochrechnung beim frühen Abbruch */
    double early_pct;
} EncodeCtx;

/* HandBrake: "Encoding: task 1 of 1, 45.12 % (30.50 fps, avg 31.20 fps, ETA 00h12m03s)" */
static void on_encoder_stdout(void *ctx, const char *line)
{
    EncodeCtx *e = ctx;
    if (e->handbrake) {
        const char *p = strstr(line, "Encoding: task ");
        int task = 1, tasks = 1;
        double pct = 0, fps = -1;
        if (p && sscanf(p, "Encoding: task %d of %d, %lf %%", &task, &tasks, &pct) == 3) {
            const char *f = strstr(p, "% (");
            if (f) fps = g_ascii_strtod(f + 3, NULL);
            job_progress(e->job, ((task - 1) * 100.0 + pct) / MAX(tasks, 1), fps);
        }
        return;
    }
    if (g_str_has_prefix(line, "out_time_us=") || g_str_has_prefix(line, "out_time_ms=")) {
        double t = g_ascii_strtod(line + 12, NULL) / 1e6;
        if (e->duration > 0 && t > 0) job_progress(e->job, t / e->duration * 100, -1);
    } else if (g_str_has_prefix(line, "fps=")) {
        double fps = g_ascii_strtod(line + 4, NULL);
        g_mutex_lock(&run.lock);
        e->job->fps = fps;
        g_mutex_unlock(&run.lock);
    }
}

static void on_encoder_stderr(void *ctx, const char *line)
{
    EncodeCtx *e = ctx;
    if (e->handbrake && (strstr(line, "Encoding: task") || strstr(line, "Scanning title"))) return;
    g_string_append_printf(e->err_tail, "%s\n", line);
    if (e->err_tail->len > ERR_TAIL) g_string_erase(e->err_tail, 0, (gssize)(e->err_tail->len - ERR_TAIL));
}

/* Hochrechnung der Endgröße: wird die Datei sicher nicht klein genug? */
static bool hopeless(EncodeCtx *e)
{
    g_mutex_lock(&run.lock);
    double pct = e->job->pct;
    g_mutex_unlock(&run.lock);
    struct stat st;
    if (e->max_bytes <= 0 || pct < EARLY_CHECK_PCT || stat(e->tmp, &st) != 0) return false;
    e->projected = st.st_size / (pct / 100);
    e->early_pct = pct;
    return e->projected > e->max_bytes * EARLY_MARGIN;
}

/* Encoder laufen lassen: Fortschritt, Pause, Abbruch, Watchdog -> Exit-Code
 * (-1: abgebrochen oder hängend, EARLY_NOGAIN: hochgerechnet kein Gewinn) */
#define EARLY_NOGAIN (-1000)
static int run_encoder(Job *j, char **argv, EncodeCtx *e, char **why)
{
    Proc p;
    GError *error = NULL;
    if (!proc_spawn(&p, argv, j->gpu ? PRIO_GPU : PRIO_LOW, true, true, &error)) {
        *why = g_strdup_printf("Start fehlgeschlagen: %s", error->message);
        g_error_free(error);
        return -1;
    }
    g_mutex_lock(&run.lock);
    j->pid = p.pid;
    if (run.paused) signal_tree(p.pid, SIGSTOP);    /* während der Vorbereitung pausiert */
    g_mutex_unlock(&run.lock);
    GString *ol = g_string_new(NULL), *el = g_string_new(NULL);
    double kill_at = 0, next_check = now_secs() + 10;
    bool stalled = false, early = false;
    int status = 0;
    while (p.out >= 0 || p.err >= 0) {
        struct pollfd fds[2];
        int n = 0;
        if (p.out >= 0) fds[n++] = (struct pollfd){.fd = p.out, .events = POLLIN};
        if (p.err >= 0) fds[n++] = (struct pollfd){.fd = p.err, .events = POLLIN};
        poll(fds, (nfds_t)n, 250);
        proc_read(&p.out, ol, on_encoder_stdout, e);
        proc_read(&p.err, el, on_encoder_stderr, e);
        g_mutex_lock(&run.lock);
        bool hang = !run.paused && active_clock() - j->last_progress > STALL_SECS;
        g_mutex_unlock(&run.lock);
        if (!kill_at && !run.paused && now_secs() > next_check) {
            next_check = now_secs() + 10;
            early = hopeless(e);
        }
        if (!kill_at && (ac_cancel || hang || early)) {
            stalled = hang && !ac_cancel;
            early = early && !ac_cancel;
            signal_tree(p.pid, SIGTERM);
            signal_tree(p.pid, SIGCONT);
            kill_at = now_secs() + 5;
        } else if (kill_at && now_secs() > kill_at) {
            signal_tree(p.pid, SIGKILL);
            kill_at = INFINITY;
        }
    }
    while (waitpid(p.pid, &status, 0) < 0 && errno == EINTR) {}
    g_mutex_lock(&run.lock);
    j->pid = 0;
    g_mutex_unlock(&run.lock);
    g_spawn_close_pid(p.pid);
    g_string_free(ol, TRUE);
    g_string_free(el, TRUE);
    if (stalled) *why = g_strdup_printf("kein Fortschritt seit %d min – abgebrochen", STALL_SECS / 60);
    if (early) return EARLY_NOGAIN;
    return kill_at ? -1 : proc_exit_code(status);
}

/* ---------------------------------------------------------------------------
 * Ergebnis prüfen und ablegen
 * ------------------------------------------------------------------------- */
/* AV1-Spur vorhanden und Dauer passt? */
static bool verify_output(const AcVideo *v, const char *tmp, char **why)
{
    AcVideo out = {.path = (char *)tmp, .vindex = -1};
    struct stat st;
    if (stat(tmp, &st) != 0 || st.st_size == 0) {
        *why = g_strdup("keine Ausgabedatei");
        return false;
    }
    out.size = st.st_size;
    bool ok = probe_video(&out);
    if (!ok || strcmp(out.vcodec, "av1")) {
        *why = g_strdup("Ergebnis enthält keine AV1-Videospur");
        g_free(out.streams);
        return false;
    }
    double tol = MAX(VERIFY_TOL, v->duration * VERIFY_TOL_REL);
    if (v->duration > 0 && fabs(out.duration - v->duration) > tol) {
        *why = g_strdup_printf("Dauer passt nicht (%.1f s statt %.1f s)", out.duration, v->duration);
        ok = false;
    }
    g_free(out.streams);
    return ok;
}

/* fertige Temp-Datei an ihren Platz bringen -> endgültiger Pfad, NULL bei Fehler */
static char *place_result(const AcVideo *v, const AcOptions *o, const char *tmp, char **why)
{
    char *dest = dest_path_for(v, o);
    bool same_as_src = o->replace && !strcmp(dest, v->path);
    if (!same_as_src && g_file_test(dest, G_FILE_TEST_EXISTS)) {
        /* Name schon vergeben (z. B. Film.mkv neben Film.mp4): Film.av1.mkv */
        char *stem = stem_of(v->path), *dir = g_path_get_dirname(dest);
        char *name = g_strconcat(stem, ".av1.mkv", NULL);
        g_free(dest);
        dest = g_build_filename(dir, name, NULL);
        g_free(stem);
        g_free(dir);
        g_free(name);
        if (g_file_test(dest, G_FILE_TEST_EXISTS)) {
            *why = g_strdup_printf("Ziel existiert schon: %s", dest);
            g_free(dest);
            return NULL;
        }
    }
    struct stat orig;
    if (stat(v->path, &orig) == 0) chmod(tmp, orig.st_mode & 07777);   /* Rechte des Originals */
    if (o->keep_mtime) {                            /* Änderungsdatum des Originals */
        struct timespec times[2] = {{.tv_nsec = UTIME_OMIT}, {.tv_sec = v->mtime}};
        utimensat(AT_FDCWD, tmp, times, 0);
    }
    if (rename(tmp, dest) != 0) {
        *why = g_strdup_printf("Umbenennen fehlgeschlagen: %s", g_strerror(errno));
        g_free(dest);
        return NULL;
    }
    temp_remove(tmp);                               /* nur noch abmelden */
    if (o->replace && !same_as_src && g_remove(v->path) != 0)
        ac_log(true, "  Hinweis: Original konnte nicht gelöscht werden: %s (%s)", v->path,
               g_strerror(errno));
    return dest;
}

/* ---------------------------------------------------------------------------
 * HDR: Helligkeitsangaben des Originals lesen und per mkvpropedit in den MKV-Container
 * des GPU-Ergebnisses schreiben (av1_vaapi in ffmpeg 6.1 schreibt sie nicht selbst)
 * ------------------------------------------------------------------------- */
typedef struct {
    bool mastering, light;      /* Mastering Display bzw. Content Light Level vorhanden */
    double prim[6], white[2];   /* rot x/y, grün x/y, blau x/y; Weißpunkt x/y (0..1) */
    double min_lum, max_lum;    /* cd/m² */
    long max_cll, max_fall;
    bool dolby_vision, hdr10plus;
} HdrMeta;

static double rational_value(const char *s)
{
    long num = 0, den = 1;
    if (sscanf(s, "%ld/%ld", &num, &den) == 2 && den) return (double)num / den;
    return g_ascii_strtod(s, NULL);
}

/* aus Container (Strom) und den ersten Bildern -> false, wenn nicht lesbar */
static bool read_hdr_meta(const AcVideo *v, HdrMeta *m)
{
    memset(m, 0, sizeof *m);
    GPtrArray *a = argv_new();
    argv_add(a, "ffprobe", "-v", "error", "-read_intervals", "%+#8", "-select_streams", NULL);
    argv_addf(a, "%d", v->vindex);
    argv_add(a, "-show_entries", "stream_side_data:frame_side_data", "-of", "default=nw=1",
             v->path, NULL);
    GString *out = g_string_new(NULL);
    int rc = run_capture(argv_finish(a), PRIO_GPU, 120, out, NULL);
    g_ptr_array_free(a, TRUE);
    static const char *const prim_keys[6] = {"red_x", "red_y", "green_x", "green_y", "blue_x", "blue_y"};
    bool seen_prim[6] = {0}, seen_white[2] = {0}, seen_lum[2] = {0}, seen_cll[2] = {0};
    char **lines = g_strsplit(out->str, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *eq = strchr(lines[i], '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = lines[i], *val = eq + 1;
        if (!strcmp(k, "side_data_type")) {
            if (strstr(val, "DOVI")) m->dolby_vision = true;
            if (strstr(val, "2094-40") || strstr(val, "HDR10+")) m->hdr10plus = true;
            continue;
        }
        for (int p = 0; p < 6; p++)                 /* erster Wert gilt (Container vor Bild) */
            if (!strcmp(k, prim_keys[p]) && !seen_prim[p]) m->prim[p] = rational_value(val), seen_prim[p] = true;
        if (!strcmp(k, "white_point_x") && !seen_white[0]) m->white[0] = rational_value(val), seen_white[0] = true;
        if (!strcmp(k, "white_point_y") && !seen_white[1]) m->white[1] = rational_value(val), seen_white[1] = true;
        if (!strcmp(k, "min_luminance") && !seen_lum[0]) m->min_lum = rational_value(val), seen_lum[0] = true;
        if (!strcmp(k, "max_luminance") && !seen_lum[1]) m->max_lum = rational_value(val), seen_lum[1] = true;
        if (!strcmp(k, "max_content") && !seen_cll[0]) m->max_cll = atol(val), seen_cll[0] = true;
        if (!strcmp(k, "max_average") && !seen_cll[1]) m->max_fall = atol(val), seen_cll[1] = true;
    }
    g_strfreev(lines);
    g_string_free(out, TRUE);
    m->mastering = seen_lum[1] && m->max_lum > 0;
    m->light = seen_cll[0] && m->max_cll > 0;
    return rc == 0;
}

/* Angaben in den Container schreiben -> false bei Fehler */
static bool write_hdr_meta(const char *mkv, const HdrMeta *m, char **why)
{
    if (!m->mastering && !m->light) return true;    /* z. B. HLG: nichts zu übertragen */
    GPtrArray *a = argv_new();
    argv_add(a, "mkvpropedit", "--quiet", mkv, "--edit", "track:v1", NULL);
    char num[G_ASCII_DTOSTR_BUF_SIZE];
#define SET(name, value) \
    do { \
        argv_add(a, "--set", NULL); \
        argv_addf(a, "%s=%s", name, g_ascii_formatd(num, sizeof num, "%.5f", value)); \
    } while (0)
    if (m->mastering) {
        static const char *const names[6] = {
            "chromaticity-coordinates-red-x", "chromaticity-coordinates-red-y",
            "chromaticity-coordinates-green-x", "chromaticity-coordinates-green-y",
            "chromaticity-coordinates-blue-x", "chromaticity-coordinates-blue-y"};
        for (int p = 0; p < 6; p++) SET(names[p], m->prim[p]);
        SET("white-coordinates-x", m->white[0]);
        SET("white-coordinates-y", m->white[1]);
        SET("max-luminance", m->max_lum);
        SET("min-luminance", m->min_lum);
    }
    if (m->light) {
        argv_add(a, "--set", NULL);
        argv_addf(a, "max-content-light=%ld", m->max_cll);
        argv_add(a, "--set", NULL);
        argv_addf(a, "max-frame-light=%ld", m->max_fall);
    }
#undef SET
    GString *out = g_string_new(NULL), *err = g_string_new(NULL);
    int rc = run_capture(argv_finish(a), PRIO_GPU, 300, out, err);
    char *cmdline = g_strjoinv(" ", (char **)a->pdata);
    ac_log(false, "HDR-Angaben: %s -> %d %s%s", cmdline, rc, out->str, err->str);
    g_free(cmdline);
    g_ptr_array_free(a, TRUE);
    bool ok = rc == 0 || rc == 1;                   /* 1 = nur Warnungen */
    if (!ok) *why = g_strdup_printf("mkvpropedit: %s", *g_strstrip(err->str) ? err->str : g_strstrip(out->str));
    g_string_free(out, TRUE);
    g_string_free(err, TRUE);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Eine Datei umwandeln
 * ------------------------------------------------------------------------- */
typedef enum { RES_OK, RES_NOGAIN, RES_FAILED, RES_RETRY_SW_DECODE, RES_RETRY_CPU,
               RES_CANCELLED } Result;

typedef struct {
    const AcOptions *o;
    const AcGpu *gpu;
    AcStats *st;
    GMutex lock;
    GCond cond;
    AcVideo *const *items;
    int n;
    bool *taken;
    int front, back;            /* erster bzw. letzter evtl. noch freier Eintrag */
    int gpu_front;              /* erster evtl. freier Eintrag ohne HDR (GPU im Hybridmodus) */
    GQueue retry;               /* AcVideo *: GPU fehlgeschlagen, CPU versucht es */
    int gpu_alive;              /* laufende GPU-Worker */
    int cpu_slots;              /* freie CPU-Plätze */
    int cpu_running;
} Sched;

static Result convert_one(Sched *s, AcVideo *v, bool gpu, bool hw_decode, bool fallback)
{
    const AcOptions *o = s->o;
    Job *j = job_new(v, gpu, hw_decode, fallback);
    char *dest = dest_path_for(v, o), *dir = g_path_get_dirname(dest);
    char *stem = stem_of(v->path), *why = NULL;
    char *tmp_name = g_strdup_printf(".%s%s-%d.mkv", stem, TMP_TAG, (int)getpid());
    char *tmp = g_build_filename(dir, tmp_name, NULL);
    double t0 = active_clock();
    Result res = RES_FAILED;
    const char *method = gpu ? (hw_decode ? "GPU (GPU-Dekoder)" : "GPU")
                         : g_hb.available ? "CPU" : "CPU (ffmpeg)";

    /* Platz auf dem Ziel abwarten (z. B. Ausgabe auf voller Platte) */
    if (o->replace) g_mkdir_with_parents(dir, 0755);
    else mkdirs_recording(dir);
    bool warned = false;
    while (!ac_cancel && free_space_gb(dir) < DISK_MIN_GB + v->size / 1e9) {
        if (!warned)
            ac_log(true, "Wenig Speicherplatz (%.1f GB frei) – warte, bis wieder Platz ist: %s",
                   free_space_gb(dir), dir);
        warned = true;
        job_phase(j, "Wartet auf Speicherplatz");
        g_usleep(30 * G_USEC_PER_SEC);
    }
    if (ac_cancel) {
        res = RES_CANCELLED;
        goto out;
    }

    /* HDR per GPU: Helligkeitsangaben des Originals merken (kommen danach in den Container);
     * Dolby Vision und HDR10+ (dynamisch) kann nur HandBrake übertragen */
    HdrMeta hdr_meta = {0};
    bool hdr_container = gpu && v->hdr && o->hdr == AC_HDR_GPU_CONTAINER;
    if (hdr_container) {
        job_phase(j, "Analyse (HDR)");
        read_hdr_meta(v, &hdr_meta);
        if (hdr_meta.dolby_vision || hdr_meta.hdr10plus) {
            ac_log(true, "HDR  %s: %s – HandBrake übernimmt (nur HandBrake überträgt das)", v->rel,
                   hdr_meta.dolby_vision ? "Dolby Vision" : "HDR10+");
            res = RES_RETRY_CPU;
            goto out;
        }
    }
    temp_register(tmp);
    EncodeCtx e = {.job = j, .duration = v->duration, .err_tail = g_string_new(NULL), .tmp = tmp,
                   .max_bytes = v->size * (1 - o->min_saving / 100.0)};
    GPtrArray *cmd;
    bool cropped = false;
    if (!gpu && g_hb.available) {
        e.handbrake = true;
        cmd = build_handbrake_cmd(v, o, tmp);
    } else {
        Crop c = {0};
        job_phase(j, "Analyse (Zuschnitt)");
        if (!crop_by_handbrake(v->path, &c) && !crop_by_cropdetect(v, &c)) c = (Crop){0};
        cmd = build_ffmpeg_cmd(v, o, s->gpu, gpu, hw_decode, &c, tmp, &cropped);
    }
    if (ac_cancel) {
        g_ptr_array_free(cmd, TRUE);
        g_string_free(e.err_tail, TRUE);
        temp_remove(tmp);
        res = RES_CANCELLED;
        goto out;
    }
    char *cmdline = g_strjoinv(" ", argv_finish(cmd));
    ac_log(false, "Start %s: %s", method, cmdline);
    g_free(cmdline);
    job_phase(j, "Umwandeln");
    int rc = run_encoder(j, (char **)cmd->pdata, &e, &why);
    g_ptr_array_free(cmd, TRUE);
    if (ac_cancel) {
        temp_remove(tmp);
        g_string_free(e.err_tail, TRUE);
        res = RES_CANCELLED;
        goto out;
    }
    if (rc == EARLY_NOGAIN) {                       /* hochgerechnet zu groß: Zeit sparen */
        char a[32], b[32];
        temp_remove(tmp);
        g_string_free(e.err_tail, TRUE);
        bool retry = gpu && o->mode == AC_MODE_HYBRID;   /* nur „GPU + CPU parallel“ */
        if (!retry) done_append(gpu ? "nogain" : "nogain-cpu", v, (long long)e.projected);
        ac_log(true, "KEIN GEWINN  %s  %s → ca. %s (hochgerechnet bei %.0f %%) – abgebrochen%s · %s",
               v->rel, ac_format_bytes((double)v->size, a, sizeof a),
               ac_format_bytes(e.projected, b, sizeof b), e.early_pct,
               retry ? ", HandBrake versucht es" : ", Original bleibt", method);
        res = retry ? RES_RETRY_CPU : RES_NOGAIN;
        goto out;
    }
    if (rc != 0 && !why) {
        char *tail = g_strstrip(g_strdup(e.err_tail->str));
        char *last = strrchr(tail, '\n');
        why = g_strdup_printf("Exit-Code %d: %s", rc, last ? last + 1 : tail);
        g_free(tail);
    }
    if (rc != 0) ac_log(false, "Fehlerausgabe:\n%s", e.err_tail->str);
    g_string_free(e.err_tail, TRUE);

    job_phase(j, "Prüfen");
    bool ok = rc == 0 && verify_output(v, tmp, &why);
    if (ok && hdr_container && !write_hdr_meta(tmp, &hdr_meta, &why)) {
        temp_remove(tmp);                           /* HDR-Angaben nie stillschweigend verlieren */
        ac_log(true, "HDR  %s: Angaben ließen sich nicht übertragen (%s) – HandBrake übernimmt",
               v->rel, why ? why : "?");
        res = RES_RETRY_CPU;
        goto out;
    }
    if (ok) {
        struct stat st;
        long long new_size = stat(tmp, &st) == 0 ? st.st_size : 0;
        double ratio = v->size > 0 ? (double)new_size / v->size : 1;
        char a[32], b[32], d[32];
        ac_format_bytes((double)v->size, a, sizeof a);
        ac_format_bytes((double)new_size, b, sizeof b);
        ac_format_duration(active_clock() - t0, d, sizeof d);
        if (ratio > 1 - o->min_saving / 100.0) {
            temp_remove(tmp);
            bool retry = gpu && o->mode == AC_MODE_HYBRID;   /* nur „GPU + CPU parallel“ */
            if (!retry) done_append(gpu ? "nogain" : "nogain-cpu", v, new_size);
            ac_log(true, "KEIN GEWINN  %s  %s → %s (%+.0f %%) – %s · %s · %s", v->rel, a, b,
                   (ratio - 1) * 100, retry ? "HandBrake versucht es" : "Original bleibt", method, d);
            res = retry ? RES_RETRY_CPU : RES_NOGAIN;
        } else {
            job_phase(j, o->replace ? "Original ersetzen" : "Ablegen");
            char *final = place_result(v, o, tmp, &why);
            if (final) {
                done_append("ok", v, new_size);
                if (!o->replace) pending_append(v, final, new_size);
                ac_log(true, "OK  %s  %s → %s (−%.0f %%) · %s%s · %s", v->rel, a, b, (1 - ratio) * 100,
                       !fallback ? method : v->hdr && o->hdr != AC_HDR_GPU_PLAIN
                                     ? "CPU (HDR vollständig)" : "CPU (Neuversuch nach GPU)",
                       cropped ? " · Balken entfernt" : "", d);
                g_mutex_lock(&s->lock);
                s->st->bytes_in += v->size;
                s->st->bytes_out += new_size;
                g_mutex_unlock(&s->lock);
                g_mutex_lock(&run.lock);
                run.in_ok += v->size;
                run.out_ok += new_size;
                g_mutex_unlock(&run.lock);
                g_free(final);
                res = RES_OK;
            }
        }
    }
    if (res == RES_FAILED) {
        temp_remove(tmp);
        if (gpu && hw_decode) {
            ac_log(true, "GPU-FEHLER  %s: %s – wird mit CPU-Dekoder wiederholt", v->rel,
                   why ? why : "?");
            res = RES_RETRY_SW_DECODE;
        } else if (gpu) {
            ac_log(true, "GPU-FEHLER  %s: %s – wird per CPU wiederholt", v->rel, why ? why : "?");
            res = RES_RETRY_CPU;
        } else {
            ac_log(true, "FEHLER  %s: %s", v->rel, why ? why : "?");
            done_append("failed", v, 0);
        }
    }
out:
    g_free(why);
    g_free(dest);
    g_free(dir);
    g_free(stem);
    g_free(tmp_name);
    g_free(tmp);
    job_free(j);
    return res;
}

/* ---------------------------------------------------------------------------
 * Verteilung auf CPU und GPU
 * ------------------------------------------------------------------------- */
/* nächste Datei nehmen: von vorn (größte) oder von hinten (kleinste);
 * skip_hdr: HDR-Videos liegen lassen (die nimmt die CPU) */
static AcVideo *take_item(Sched *s, bool from_front, bool skip_hdr)
{
    while (s->front <= s->back && s->taken[s->front]) s->front++;
    while (s->back >= s->front && s->taken[s->back]) s->back--;
    if (from_front && skip_hdr) {   /* alles vor gpu_front ist vergeben oder HDR: nie neu prüfen */
        if (s->gpu_front < s->front) s->gpu_front = s->front;
        while (s->gpu_front <= s->back && (s->taken[s->gpu_front] || s->items[s->gpu_front]->hdr))
            s->gpu_front++;
        if (s->gpu_front > s->back) return NULL;
        s->taken[s->gpu_front] = true;
        return s->items[s->gpu_front];
    }
    if (from_front) {
        for (int i = s->front; i <= s->back; i++)
            if (!s->taken[i]) {
                s->taken[i] = true;
                return s->items[i];
            }
    } else {
        for (int i = s->back; i >= s->front; i--)
            if (!s->taken[i]) {
                s->taken[i] = true;
                return s->items[i];
            }
    }
    return NULL;
}

static void wait_while_paused(void)
{
    while (!ac_cancel && ac_paused()) g_usleep(200 * 1000);
}

static void count_result(Sched *s, AcVideo *v, Result r, bool gpu, bool fallback)
{
    g_mutex_lock(&s->lock);
    if (r == RES_OK) {
        s->st->ok++;
        if (fallback) s->st->fallback_files++;
        else if (gpu) s->st->gpu_files++;
        else s->st->cpu_files++;
    } else if (r == RES_NOGAIN) {
        s->st->nogain++;
    } else if (r == RES_FAILED) {
        s->st->err++;
    }
    g_mutex_unlock(&s->lock);
    if (r == RES_CANCELLED || r == RES_RETRY_CPU || r == RES_RETRY_SW_DECODE) return;
    g_mutex_lock(&run.lock);
    run.files_done++;
    run.bytes_done += (double)v->size;
    run.est_done += ac_estimate_secs(v, &run.opts, run.gpu_ok);
    int done = run.files_done, total = run.files_total;
    g_mutex_unlock(&run.lock);
    if (ac_fe.file_done) ac_fe.file_done(done, total, r != RES_FAILED);
}

static gpointer gpu_worker(gpointer data)
{
    Sched *s = data;
    while (!ac_cancel) {
        wait_while_paused();
        g_mutex_lock(&s->lock);
        AcVideo *v = ac_cancel ? NULL : take_item(s, true, false);
        g_mutex_unlock(&s->lock);
        if (!v) break;
        if (v->hdr) ac_log(false, "Hinweis: HDR-Video per GPU – Farbraum bleibt, Helligkeitsangaben "
                           "(Mastering Display/MaxCLL) entfallen: %s", v->rel);
        Result r = convert_one(s, v, true, want_hw_decode(v, s->o->mode), false);
        if (r == RES_RETRY_SW_DECODE) r = convert_one(s, v, true, false, false);
        if (r == RES_RETRY_CPU) {
            g_mutex_lock(&run.lock);                /* Restdauer: GPU-Versuch erledigt, CPU kommt dazu */
            run.est_done += ac_estimate_secs(v, &run.opts, run.gpu_ok);
            v->cpu_only = true;
            run.est_total += ac_estimate_secs(v, &run.opts, run.gpu_ok);
            g_mutex_unlock(&run.lock);
            g_mutex_lock(&s->lock);
            g_queue_push_tail(&s->retry, v);
            g_cond_broadcast(&s->cond);
            g_mutex_unlock(&s->lock);
        }
        count_result(s, v, r, true, false);
    }
    g_mutex_lock(&s->lock);
    s->gpu_alive--;
    g_cond_broadcast(&s->cond);
    g_mutex_unlock(&s->lock);
    return NULL;
}

/* CPU-Plätze einer Datei: große Bilder (über 1440p) brauchen zwei */
static int cpu_weight(const Sched *s, const AcVideo *v)
{
    int w = (double)v->width * v->height > BIG_FRAME_PIXELS ? 2 : 1;
    return MIN(w, s->o->cpu_jobs);
}

static gpointer cpu_worker(gpointer data)
{
    Sched *s = data;
    while (!ac_cancel) {
        wait_while_paused();
        g_mutex_lock(&s->lock);
        AcVideo *v = NULL;
        bool fallback = false;
        while (!ac_cancel) {
            if ((v = g_queue_pop_head(&s->retry))) {
                fallback = true;
                break;
            }
            if (s->o->mode != AC_MODE_GPU &&
                (v = take_item(s, s->o->mode == AC_MODE_CPU, false)))
                break;
            if (s->gpu_alive == 0) break;           /* nichts mehr zu tun */
            /* auf GPU-Fehlschläge warten (jede Sekunde neu prüfen) */
            g_cond_wait_until(&s->cond, &s->lock, g_get_monotonic_time() + G_USEC_PER_SEC);
        }
        if (!v || ac_cancel) {
            g_mutex_unlock(&s->lock);
            break;
        }
        /* freie Plätze und genug Arbeitsspeicher abwarten */
        int weight = cpu_weight(s, v);
        double need_ram = weight > 1 ? RAM_PER_JOB_4K_GB : RAM_PER_JOB_GB;
        while (!ac_cancel && (s->cpu_slots < weight ||
                              (s->cpu_running > 0 && mem_available_gb() < need_ram))) {
            gint64 until = g_get_monotonic_time() + G_USEC_PER_SEC;
            g_cond_wait_until(&s->cond, &s->lock, until);
        }
        s->cpu_slots -= weight;
        s->cpu_running++;
        g_mutex_unlock(&s->lock);
        Result r = ac_cancel ? RES_CANCELLED : convert_one(s, v, false, false, fallback);
        g_mutex_lock(&s->lock);
        s->cpu_slots += weight;
        s->cpu_running--;
        g_cond_broadcast(&s->cond);
        g_mutex_unlock(&s->lock);
        count_result(s, v, r, false, fallback);
    }
    g_mutex_lock(&s->lock);
    g_cond_broadcast(&s->cond);
    g_mutex_unlock(&s->lock);
    return NULL;
}

/* CPU-Last messen (für die Anzeige) */
static gpointer cpu_monitor(gpointer data)
{
    volatile bool *stop = data;
    unsigned long long prev[2] = {0};
    while (!*stop) {
        FILE *f = fopen("/proc/stat", "r");
        unsigned long long u, n, sy, id, io, irq, sirq, steal;
        if (f && fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &u, &n, &sy, &id, &io, &irq,
                        &sirq, &steal) == 8) {
            unsigned long long busy = u + n + sy + irq + sirq + steal, total = busy + id + io;
            if (prev[1] && total > prev[1]) {
                g_mutex_lock(&run.lock);
                run.cpu_load = 100.0 * (double)(busy - prev[0]) / (double)(total - prev[1]);
                run.cpu_valid = true;
                g_mutex_unlock(&run.lock);
            }
            prev[0] = busy;
            prev[1] = total;
        }
        if (f) fclose(f);
        for (int i = 0; i < 20 && !*stop; i++) g_usleep(100 * 1000);
    }
    return NULL;
}

/* Standby sperren (systemd-logind), solange umgewandelt wird -> fd, -1 = keine Sperre */
static int standby_lock(void)
{
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, NULL);
    if (!bus) return -1;
    GUnixFDList *fds = NULL;
    GVariant *r = g_dbus_connection_call_with_unix_fd_list_sync(
        bus, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager",
        "Inhibit", g_variant_new("(ssss)", "sleep:idle", "AV1-Converter", "Videos werden umgewandelt",
                                 "block"),
        G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &fds, NULL, NULL);
    int fd = -1;
    if (r && fds) {
        gint32 index;
        g_variant_get(r, "(h)", &index);
        fd = g_unix_fd_list_get(fds, index, NULL);
    }
    if (r) g_variant_unref(r);
    if (fds) g_object_unref(fds);
    g_object_unref(bus);
    return fd;
}

bool ac_convert(AcVideo *const *sel, int n, const AcOptions *o, const AcHandbrake *hb,
                const AcGpu *gpu, AcStats *st)
{
    memset(st, 0, sizeof *st);
    if (hb && hb->available) g_hb = *hb;
    else memset(&g_hb, 0, sizeof g_hb);
    bool use_gpu = gpu && gpu->available && o->mode != AC_MODE_CPU;
    Sched s = {.o = o, .gpu = gpu, .st = st, .items = sel, .n = n, .taken = g_new0(bool, MAX(n, 1)),
               .front = 0, .back = n - 1, .cpu_slots = CLAMP(o->cpu_jobs, 1, MAX_CPU_JOBS)};
    g_mutex_init(&s.lock);
    g_cond_init(&s.cond);
    g_queue_init(&s.retry);
    AcOptions eff = *o;
    if (!use_gpu && o->mode != AC_MODE_CPU) {
        ac_log(true, "Keine nutzbare GPU – es wird nur die CPU verwendet.");
        eff.mode = AC_MODE_CPU;
    }
    s.o = &eff;
    if (eff.mode != AC_MODE_CPU)    /* GPU ohne Gewinn bzw. HDR vollständig erhalten: direkt zur CPU */
        for (int i = 0; i < n; i++) {
            if (hdr_to_handbrake(sel[i], &eff)) sel[i]->cpu_only = true;
            if (sel[i]->cpu_only) {
                s.taken[i] = true;
                g_queue_push_tail(&s.retry, sel[i]);
            }
        }

    g_mutex_lock(&run.lock);
    if (!run.jobs) run.jobs = g_ptr_array_new();
    run.active = true;
    run.paused = false;
    run.paused_total = 0;
    run.t0 = now_secs();
    run.bytes_total = run.bytes_done = 0;
    run.in_ok = run.out_ok = 0;
    run.files_done = 0;
    run.files_total = n;
    run.cpu_valid = false;
    for (int i = 0; i < n; i++) run.bytes_total += (double)sel[i]->size;
    run.opts = eff;
    run.opts.out_dir = NULL;
    run.gpu_ok = use_gpu;
    run.est_total = estimate_raw(sel, n, &eff, use_gpu);
    run.factor = tempo_factor(eff.mode);
    run.est_done = 0;
    run.samples = run.sample_next = 0;
    run.next_sample = 0;
    g_mutex_unlock(&run.lock);

    char total[32];
    ac_format_bytes(run.bytes_total, total, sizeof total);
    static const char *const mode_text[] = {"CPU (HandBrake)", "GPU (VAAPI)", "GPU + CPU parallel"};
    char est[32];
    ac_log(true, "Umwandlung von %d Videos (%s) – %s · CPU-Jobs %d · GPU-Jobs %d · %s · "
           "geschätzte Dauer ca. %s", n, total, mode_text[eff.mode], eff.cpu_jobs,
           use_gpu ? eff.gpu_jobs : 0, o->replace ? "Originale werden ersetzt" : "Ausgabeordner",
           ac_format_duration(run.est_total * run.factor, est, sizeof est));
    if (!g_hb.available && eff.mode != AC_MODE_GPU)
        ac_log(true, "HandBrake nicht gefunden – CPU-Jobs laufen mit ffmpeg (libsvtav1) und "
               "gleichwertigen Einstellungen.");

    int lock_fd = standby_lock();
    volatile bool stop_monitor = false;
    GThread *monitor = g_thread_new("cpu-last", cpu_monitor, (gpointer)&stop_monitor);
    GPtrArray *threads = g_ptr_array_new();
    if (eff.mode != AC_MODE_CPU) {
        /* Anzahl vorher festhalten: ein Worker ohne Arbeit beendet sich sofort und zählt
         * gpu_alive herunter – als Schleifengrenze würden sonst zu wenige gestartet und
         * gpu_alive käme nie auf 0 (CPU-Worker würden ewig warten) */
        int gpu_threads = CLAMP(eff.gpu_jobs, 1, MAX_GPU_JOBS);
        g_mutex_lock(&s.lock);
        s.gpu_alive = gpu_threads;
        g_mutex_unlock(&s.lock);
        for (int i = 0; i < gpu_threads; i++) g_ptr_array_add(threads, g_thread_new("gpu", gpu_worker, &s));
    }
    /* CPU-Worker: im GPU-Modus einer als Reserve für GPU-Fehlschläge */
    int cpu_threads = eff.mode == AC_MODE_GPU ? 1 : CLAMP(eff.cpu_jobs, 1, MAX_CPU_JOBS);
    for (int i = 0; i < cpu_threads; i++) g_ptr_array_add(threads, g_thread_new("cpu", cpu_worker, &s));
    for (guint i = 0; i < threads->len; i++) g_thread_join(threads->pdata[i]);
    g_ptr_array_free(threads, TRUE);
    stop_monitor = true;
    g_thread_join(monitor);
    if (lock_fd >= 0) close(lock_fd);
    ac_cleanup_temps();

    g_mutex_lock(&run.lock);
    st->secs = active_clock() - run.t0;
    if (st->secs > 120 && run.est_done > 0) tempo_remember(eff.mode, st->secs / run.est_done);
    run.active = false;
    run.paused = false;
    g_mutex_unlock(&run.lock);
    st->aborted = ac_cancel != 0;
    char in[32], out[32], saved[32], dur[32];
    ac_format_bytes((double)st->bytes_in, in, sizeof in);
    ac_format_bytes((double)st->bytes_out, out, sizeof out);
    ac_format_bytes((double)(st->bytes_in - st->bytes_out), saved, sizeof saved);
    ac_format_duration(st->secs, dur, sizeof dur);
    ac_log(true, "%s: %d umgewandelt (%s → %s, gespart %s), %d ohne Gewinn, %d Fehler · Dauer %s",
           st->aborted ? "Abgebrochen" : "Fertig", st->ok, in, out, saved, st->nogain, st->err, dur);
    g_free(s.taken);
    g_queue_clear(&s.retry);
    g_mutex_clear(&s.lock);
    g_cond_clear(&s.cond);
    return !st->aborted;
}

/* ---------------------------------------------------------------------------
 * Öffentliche Schnittstelle: Status
 * ------------------------------------------------------------------------- */
int ac_jobs_snapshot(AcJobInfo *out, int max)
{
    int n = 0;
    g_mutex_lock(&run.lock);
    double now = active_clock();
    for (guint i = 0; run.jobs && i < run.jobs->len && n < max; i++) {
        Job *j = run.jobs->pdata[i];
        AcJobInfo *o = &out[n++];
        g_strlcpy(o->rel, j->v->rel, sizeof o->rel);
        g_strlcpy(o->what, j->what, sizeof o->what);
        o->pct = j->pct;
        o->fps = j->fps;
        o->secs = now - j->start;
        double phase = now - j->phase_start;
        o->eta = j->pct > 0.5 && phase > 20 ? phase * (100 - j->pct) / j->pct : -1;
        o->gpu = j->gpu;
        o->fallback = j->fallback;
        o->hw_decode = j->hw_decode;
    }
    g_mutex_unlock(&run.lock);
    return n;
}

bool ac_progress(AcProgress *p)
{
    g_mutex_lock(&run.lock);
    bool active = run.active;
    if (active) {
        p->bytes_total = run.bytes_total;
        p->bytes_done = run.bytes_done;
        for (guint i = 0; i < run.jobs->len; i++) {        /* laufende anteilig */
            Job *j = run.jobs->pdata[i];
            if (!strcmp(j->what, "Umwandeln")) p->bytes_done += j->v->size * j->pct / 100;
        }
        p->in_ok = run.in_ok;
        p->out_ok = run.out_ok;
        p->saved = run.in_ok - run.out_ok;
        p->files_done = run.files_done;
        p->files_total = run.files_total;
        p->secs = active_clock() - run.t0;
        /* Restdauer: Restaufwand laut Modell, geteilt durch das gemessene Tempo (Aufwand je
         * Sekunde). Das jüngere Tempo (letzte 30 min) zählt stärker, damit sich die Anzeige
         * an Änderungen anpasst (z. B. eine Strecke mit 4K-Videos oder ein langsameres NAS). */
        double done = run.est_done;
        for (guint i = 0; i < run.jobs->len; i++) {
            Job *j = run.jobs->pdata[i];
            if (!strcmp(j->what, "Umwandeln")) done += j->est * j->pct / 100;
        }
        double now = p->secs;
        if (now >= run.next_sample) {               /* Verlauf alle 30 s */
            run.sample_t[run.sample_next] = now;
            run.sample_done[run.sample_next] = done;
            run.sample_next = (run.sample_next + 1) % 64;
            run.samples = MIN(run.samples + 1, 64);
            run.next_sample = now + 30;
        }
        double rest = MAX(run.est_total - done, 0), speed = 0;
        if (now > 60 && done > 0) {
            speed = done / now;
            double old_t = now, old_done = done;    /* ältester Wert im Fenster */
            for (int k = 0; k < run.samples; k++)
                if (now - run.sample_t[k] <= EST_RECENT_SECS && run.sample_t[k] < old_t) {
                    old_t = run.sample_t[k];
                    old_done = run.sample_done[k];
                }
            if (now - old_t >= EST_RECENT_MIN && done > old_done)
                speed = 0.7 * (done - old_done) / (now - old_t) + 0.3 * speed;
        }
        p->eta_measured = speed > 0;
        p->eta = speed > 0 ? rest / speed : rest * run.factor;
        p->est_total = run.est_total * run.factor;
    }
    g_mutex_unlock(&run.lock);
    return active;
}

bool ac_cpu_load(double *cpu)
{
    g_mutex_lock(&run.lock);
    bool ok = run.active && run.cpu_valid;
    *cpu = run.cpu_load;
    g_mutex_unlock(&run.lock);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Nachträglich: fertige Ergebnisse an den Platz der Originale verschieben
 * (wie beim Intro-Cutter, aber die Liste überdauert Programmende und Neustart)
 * ------------------------------------------------------------------------- */
typedef struct {
    long long orig_size, orig_mtime, dest_size;
    char *orig, *dest;
} Pending;

static void pending_free(gpointer p)
{
    Pending *e = p;
    g_free(e->orig);
    g_free(e->dest);
    g_free(e);
}

/* Liste lesen (doppelte Einträge: der letzte gilt) – Aufruf mit g_done_lock */
static GPtrArray *pending_load(void)
{
    GPtrArray *list = g_ptr_array_new_with_free_func(pending_free);
    GHashTable *index = g_hash_table_new(g_str_hash, g_str_equal);
    FILE *f = fopen(PENDING_DB, "r");
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while (f && (len = getline(&line, &cap, f)) > 0) {
        if (line[len - 1] == '\n') line[--len] = 0;
        char **x = g_strsplit(line, "\t", 5);
        if (g_strv_length(x) == 5) {
            Pending *e = g_new0(Pending, 1);
            e->orig_size = g_ascii_strtoll(x[0], NULL, 10);
            e->orig_mtime = g_ascii_strtoll(x[1], NULL, 10);
            e->dest_size = g_ascii_strtoll(x[2], NULL, 10);
            e->orig = g_strdup(x[3]);
            e->dest = g_strdup(x[4]);
            gpointer old = g_hash_table_lookup(index, e->orig);
            if (old) g_ptr_array_remove(list, old);
            g_ptr_array_add(list, e);
            g_hash_table_replace(index, e->orig, e);
        }
        g_strfreev(x);
    }
    free(line);
    if (f) fclose(f);
    g_hash_table_unref(index);
    return list;
}

/* Liste neu schreiben – Aufruf mit g_done_lock */
static void pending_save(GPtrArray *list)
{
    if (!list->len) {
        g_remove(PENDING_DB);
        return;
    }
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < list->len; i++) {
        Pending *e = list->pdata[i];
        g_string_append_printf(s, "%lld\t%lld\t%lld\t%s\t%s\n", e->orig_size, e->orig_mtime,
                               e->dest_size, e->orig, e->dest);
    }
    g_file_set_contents(PENDING_DB, s->str, (gssize)s->len, NULL);
    g_string_free(s, TRUE);
}

/* Ergebnisse früherer Läufe (auch älterer Programmversionen ohne Liste) in die Liste
 * aufnehmen: erfolgreich umgewandelt (laut erledigt.tsv, Original seitdem unverändert) und
 * das Ergebnis liegt noch im Ausgabeordner */
static void adopt_results(GArray *found, const AcOptions *o)
{
    g_mutex_lock(&g_done_lock);
    GPtrArray *list = pending_load();
    g_mutex_unlock(&g_done_lock);
    GHashTable *known = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < list->len; i++) g_hash_table_add(known, ((Pending *)list->pdata[i])->orig);
    int adopted = 0;
    for (guint i = 0; i < found->len; i++) {
        AcVideo *v = &g_array_index(found, AcVideo, i);
        if (v->state != AC_SKIP_DONE || g_hash_table_contains(known, v->path)) continue;
        char *dest = dest_path_for(v, o);
        if (!g_file_test(dest, G_FILE_TEST_IS_REGULAR)) {   /* Namenskonflikt: Film.av1.mkv */
            char *dir = g_path_get_dirname(dest), *stem = stem_of(v->path);
            char *name = g_strconcat(stem, ".av1.mkv", NULL);
            g_free(dest);
            dest = g_build_filename(dir, name, NULL);
            g_free(dir);
            g_free(stem);
            g_free(name);
        }
        struct stat st;
        if (stat(dest, &st) == 0 && S_ISREG(st.st_mode)) {
            pending_append(v, dest, st.st_size);
            adopted++;
        }
        g_free(dest);
    }
    if (adopted)
        ac_log(true, "%d fertige Ergebnisse früherer Läufe im Ausgabeordner gefunden – sie lassen "
               "sich an den Platz der Originale verschieben.", adopted);
    g_hash_table_unref(known);
    g_ptr_array_free(list, TRUE);
}

bool ac_pending_info(AcPending *p)
{
    memset(p, 0, sizeof *p);
    g_mutex_lock(&g_done_lock);
    GPtrArray *list = pending_load();
    g_mutex_unlock(&g_done_lock);
    for (guint i = 0; i < list->len; i++) {
        Pending *e = list->pdata[i];
        p->files++;
        p->orig_bytes += e->orig_size;
        p->dest_bytes += e->dest_size;
    }
    g_ptr_array_free(list, TRUE);
    return p->files > 0;
}

/* Datei kopieren (über Dateisystemgrenzen), Fortschritt in Byte; fsync am Ende */
static int copy_file(const char *from, const char *to, double *bytes_done,
                     void (*report)(double), char **why)
{
    int in = open(from, O_RDONLY | O_CLOEXEC);
    if (in < 0) {
        *why = g_strdup_printf("lesen: %s", g_strerror(errno));
        return -1;
    }
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (out < 0) {
        *why = g_strdup_printf("schreiben: %s", g_strerror(errno));
        close(in);
        return -1;
    }
    size_t bufsize = 4 << 20;
    char *buf = g_malloc(bufsize);
    int rc = 0;
    double last = now_secs();
    while (rc == 0) {
        ssize_t r = read(in, buf, bufsize);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) {
            *why = g_strdup_printf("lesen: %s", g_strerror(errno));
            rc = -1;
            break;
        }
        if (r == 0) break;
        for (ssize_t off = 0; off < r && rc == 0;) {
            ssize_t w = write(out, buf + off, (size_t)(r - off));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) {
                *why = g_strdup_printf("schreiben: %s", g_strerror(errno));
                rc = -1;
            } else {
                off += w;
            }
        }
        *bytes_done += (double)r;
        if (report && now_secs() - last > 0.25) {
            last = now_secs();
            report(*bytes_done);
        }
    }
    if (rc == 0 && fsync(out) != 0) {
        *why = g_strdup_printf("sichern: %s", g_strerror(errno));
        rc = -1;
    }
    g_free(buf);
    close(in);
    if (close(out) != 0 && rc == 0) {
        *why = g_strdup_printf("schließen: %s", g_strerror(errno));
        rc = -1;
    }
    return rc;
}

static struct {                 /* Fortschritt des Verschiebens (für die Callbacks) */
    int done, total;
    double bytes_total;
} g_move;

static void move_report(double bytes_done)
{
    if (ac_fe.move_progress) ac_fe.move_progress(g_move.done, g_move.total, bytes_done, g_move.bytes_total);
}

/* eine Datei an den Platz ihres Originals -> 1 verschoben, 0 übersprungen (Eintrag
 * erledigt), -1 Fehler (Eintrag bleibt für einen neuen Versuch) */
static int move_one(Pending *e, double *bytes_done, AcMoveStats *ms)
{
    char *why = NULL;
    struct stat ds, os;
    if (stat(e->dest, &ds) != 0) {
        ac_log(true, "Übersprungen (Ergebnis nicht mehr im Ausgabeordner): %s", e->dest);
        ms->skipped++;
        return 0;
    }
    if (stat(e->orig, &os) != 0) {
        ac_log(true, "Übersprungen (Original nicht mehr vorhanden – Ergebnis bleibt im "
               "Ausgabeordner): %s", e->orig);
        ms->skipped++;
        return 0;
    }
    if (os.st_size != e->orig_size || os.st_mtime != e->orig_mtime) {
        ac_log(true, "Übersprungen (Original wurde seit der Umwandlung geändert): %s", e->orig);
        ms->skipped++;
        return 0;
    }
    /* Ziel: Film.mp4 -> Film.mkv; ist der Name schon vergeben (anderes Video): Film.av1.mkv */
    char *dir = g_path_get_dirname(e->orig), *base = g_path_get_basename(e->orig);
    char *dot = strrchr(base, '.');
    if (dot && dot != base) *dot = 0;
    char *name = g_strconcat(base, ".mkv", NULL), *target = g_build_filename(dir, name, NULL);
    if (strcmp(target, e->orig) && g_file_test(target, G_FILE_TEST_EXISTS)) {
        g_free(name);
        g_free(target);
        name = g_strconcat(base, ".av1.mkv", NULL);
        target = g_build_filename(dir, name, NULL);
    }
    int result = -1;
    if (strcmp(target, e->orig) && g_file_test(target, G_FILE_TEST_EXISTS)) {
        why = g_strdup_printf("Ziel existiert schon: %s", target);
        goto out;
    }
    chmod(e->dest, os.st_mode & 07777);             /* Rechte des Originals */
    if (rename(e->dest, target) == 0) {
        *bytes_done += (double)ds.st_size;
    } else if (errno == EXDEV) {                    /* anderes Dateisystem: kopieren */
        if (free_space_gb(dir) < ds.st_size / 1e9 + 1) {
            why = g_strdup_printf("zu wenig Platz in %s", dir);
            goto out;
        }
        char *tmp_name = g_strdup_printf(".%s.av1-tmp-%d.mkv", base, (int)getpid());
        char *tmp = g_build_filename(dir, tmp_name, NULL);
        g_free(tmp_name);
        temp_register(tmp);
        struct timespec times[2] = {{.tv_nsec = UTIME_OMIT}, {.tv_sec = ds.st_mtime}};
        bool ok = copy_file(e->dest, tmp, bytes_done, move_report, &why) == 0 &&
                  chmod(tmp, os.st_mode & 07777) == 0 && utimensat(AT_FDCWD, tmp, times, 0) == 0;
        struct stat cs;
        if (ok && (stat(tmp, &cs) != 0 || cs.st_size != ds.st_size)) {   /* Kopie vollständig? */
            why = g_strdup("Kopie unvollständig");
            ok = false;
        }
        if (ok && rename(tmp, target) != 0) {
            why = g_strdup_printf("umbenennen: %s", g_strerror(errno));
            ok = false;
        }
        temp_remove(tmp);                           /* nach rename: nur noch abmelden */
        g_free(tmp);
        if (!ok) goto out;
        g_remove(e->dest);                          /* Kopie liegt sicher am Ziel */
    } else {
        why = g_strdup_printf("verschieben: %s", g_strerror(errno));
        goto out;
    }
    if (strcmp(target, e->orig) && g_remove(e->orig) != 0)
        ac_log(true, "  Hinweis: Original konnte nicht gelöscht werden: %s (%s)", e->orig,
               g_strerror(errno));
    ms->moved++;
    ms->orig_bytes += e->orig_size;
    ms->dest_bytes += ds.st_size;
    result = 1;
out:
    if (result < 0) {
        ac_log(true, "FEHLER  nicht verschoben (Original bleibt): %s: %s", e->orig, why ? why : "?");
        ms->failed++;
    }
    g_free(why);
    g_free(dir);
    g_free(base);
    g_free(name);
    g_free(target);
    return result;
}

/* leere, vom Programm angelegte Ausgabeordner entfernen (tiefste zuerst) */
static int remove_created_dirs(void)
{
    char *content = NULL;
    if (!g_file_get_contents(CREATED_DIRS, &content, NULL, NULL)) return 0;
    char **dirs = g_strsplit(content, "\n", -1);
    g_free(content);
    GPtrArray *sorted = g_ptr_array_new();
    for (int i = 0; dirs[i]; i++)
        if (*dirs[i]) g_ptr_array_add(sorted, dirs[i]);
    g_ptr_array_sort_values(sorted, (GCompareFunc)g_strcmp0);
    int removed = 0;
    GString *keep = g_string_new(NULL);
    for (guint i = sorted->len; i-- > 0;) {         /* rückwärts: Unterordner vor Eltern */
        const char *d = sorted->pdata[i];
        if (i + 1 < sorted->len && !strcmp(d, sorted->pdata[i + 1])) continue;   /* doppelt */
        if (rmdir(d) == 0) {
            removed++;
        } else if (g_file_test(d, G_FILE_TEST_IS_DIR)) {      /* nicht leer: für später merken */
            char *line = g_strconcat(d, "\n", NULL);
            g_string_prepend(keep, line);
            g_free(line);
        }
    }
    if (keep->len) g_file_set_contents(CREATED_DIRS, keep->str, (gssize)keep->len, NULL);
    else g_remove(CREATED_DIRS);
    g_string_free(keep, TRUE);
    g_ptr_array_free(sorted, TRUE);
    g_strfreev(dirs);
    return removed;
}

bool ac_move_to_originals(AcMoveStats *ms)
{
    memset(ms, 0, sizeof *ms);
    g_mutex_lock(&g_done_lock);
    GPtrArray *list = pending_load();
    g_mutex_unlock(&g_done_lock);
    g_move.total = (int)list->len;
    g_move.done = 0;
    g_move.bytes_total = 0;
    for (guint i = 0; i < list->len; i++) g_move.bytes_total += ((Pending *)list->pdata[i])->dest_size;
    char total[32];
    ac_log(true, "Verschiebe %d fertige Datei(en) (%s) an den Platz der Originale …", g_move.total,
           ac_format_bytes(g_move.bytes_total, total, sizeof total));
    GPtrArray *keep = g_ptr_array_new_with_free_func(pending_free);
    double bytes_done = 0, last_log = now_secs(), last_save = now_secs();
    move_report(0);
    guint i = 0;
    for (; i < list->len && !ac_cancel; i++) {
        Pending *e = list->pdata[i];
        double before = bytes_done;
        if (move_one(e, &bytes_done, ms) < 0) {
            list->pdata[i] = NULL;
            g_ptr_array_add(keep, e);
        }
        bytes_done = before + e->dest_size;         /* auch bei übersprungenen weiterzählen */
        g_move.done = (int)i + 1;
        move_report(bytes_done);
        if (now_secs() - last_log > 30) {
            char a[32], b[32];
            last_log = now_secs();
            ac_log(true, "  … %d von %d Dateien (%s von %s)", g_move.done, g_move.total,
                   ac_format_bytes(bytes_done, a, sizeof a), ac_format_bytes(g_move.bytes_total, b, sizeof b));
        }
        if (now_secs() - last_save > 30) {          /* Zwischenstand sichern (Absturz) */
            last_save = now_secs();
            GPtrArray *rest = g_ptr_array_new();
            for (guint k = 0; k < keep->len; k++) g_ptr_array_add(rest, keep->pdata[k]);
            for (guint k = i + 1; k < list->len; k++) g_ptr_array_add(rest, list->pdata[k]);
            g_mutex_lock(&g_done_lock);
            pending_save(rest);
            g_mutex_unlock(&g_done_lock);
            g_ptr_array_free(rest, TRUE);
        }
    }
    for (; i < list->len; i++) {                    /* abgebrochen: Rest bleibt in der Liste */
        g_ptr_array_add(keep, list->pdata[i]);
        list->pdata[i] = NULL;
    }
    g_mutex_lock(&g_done_lock);
    pending_save(keep);
    g_mutex_unlock(&g_done_lock);
    ms->dirs_removed = remove_created_dirs();
    ms->left = (int)keep->len;
    g_ptr_array_set_free_func(list, NULL);          /* übrige Einträge gehören keep oder sind */
    for (guint k = 0; k < list->len; k++)           /* verarbeitet */
        if (list->pdata[k]) pending_free(list->pdata[k]);
    g_ptr_array_free(list, TRUE);
    g_ptr_array_free(keep, TRUE);
    char a[32], b[32];
    ac_log(true, "%s: %d verschoben, %d übersprungen, %d Fehler · %s Originale gelöscht, "
           "%s eingespart%s", ac_cancel ? "Verschieben abgebrochen" : "Verschieben fertig",
           ms->moved, ms->skipped, ms->failed, ac_format_bytes((double)ms->orig_bytes, a, sizeof a),
           ac_format_bytes((double)(ms->orig_bytes - ms->dest_bytes), b, sizeof b),
           ms->dirs_removed ? " · leere Ausgabeordner entfernt" : "");
    return !ac_cancel;
}

void ac_init(void)
{
    g_have_mkvpropedit = command_exists("mkvpropedit");
    AC_APP_DIR = g_build_filename(g_get_home_dir(), ".av1-converter", NULL);
    AC_RUN_LOG = g_build_filename(AC_APP_DIR, "last_run.log", NULL);
    AC_DONE_DB = g_build_filename(AC_APP_DIR, "erledigt.tsv", NULL);
    PROBE_CACHE = g_build_filename(AC_APP_DIR, "probe_cache.tsv", NULL);
    PRESET_EXPORT = g_build_filename(AC_APP_DIR, "handbrake_profil.json", NULL);
    PENDING_DB = g_build_filename(AC_APP_DIR, "ausgabe.tsv", NULL);
    CREATED_DIRS = g_build_filename(AC_APP_DIR, "angelegte_ordner.txt", NULL);
    g_mkdir_with_parents(AC_APP_DIR, 0755);
}
