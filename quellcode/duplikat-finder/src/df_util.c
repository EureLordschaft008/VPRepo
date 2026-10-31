/*
 * Duplikat-Finder – Hilfen der Engine: Pfade, Protokoll, Listen, Optionen,
 * Anzeige-Formate, Systemwerte (/proc)
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <fnmatch.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "df_internal.h"

volatile sig_atomic_t df_cancel;
char *DF_APP_DIR, *DF_DB_PATH, *DF_RUN_LOG, *DF_RESULT_PATH, *DF_SCHEDULED_PATH, *DF_THUMB_DIR;
DfFrontend df_fe;

static FILE *g_logf;
static GMutex g_log_lock;

double df_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void df_init(void)
{
    if (DF_APP_DIR) return;
    DF_APP_DIR = g_build_filename(g_get_home_dir(), ".duplikat-finder", NULL);
    DF_DB_PATH = g_build_filename(DF_APP_DIR, "dateien.db", NULL);
    DF_RUN_LOG = g_build_filename(DF_APP_DIR, "lauf.log", NULL);
    DF_RESULT_PATH = g_build_filename(DF_APP_DIR, "letztes-ergebnis.json", NULL);
    DF_SCHEDULED_PATH = g_build_filename(DF_APP_DIR, "geplantes-ergebnis.json", NULL);
    DF_THUMB_DIR = g_build_filename(g_get_user_cache_dir(), "duplikat-finder", "vorschau", NULL);
    g_mkdir_with_parents(DF_APP_DIR, 0755);
    df_db_open();
}

void df_shutdown(void)
{
    df_thumbs_shutdown();
    df_db_close();
    g_mutex_lock(&g_log_lock);
    if (g_logf) fclose(g_logf);
    g_logf = NULL;
    g_mutex_unlock(&g_log_lock);
}

/* Hilfsprozess: neben dem Programm (Entwicklung), sonst im lib-Ordner der Installation */
const char *df_worker_path(void)
{
    static char *path;
    static bool searched;
    if (searched) return path;
    searched = true;
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    if (exe) {
        char *dir = g_path_get_dirname(exe);
        char *cands[3] = {
            g_build_filename(dir, "duplikat-finder-worker", NULL),
            g_build_filename(dir, "..", "lib", "duplikat-finder", "duplikat-finder-worker", NULL),
            g_strdup("/usr/lib/duplikat-finder/duplikat-finder-worker"),
        };
        for (int i = 0; i < 3; i++) {
            if (!path && g_file_test(cands[i], G_FILE_TEST_IS_EXECUTABLE)) path = g_strdup(cands[i]);
            g_free(cands[i]);
        }
        g_free(dir);
        g_free(exe);
    }
    return path;
}

/* ---------------------------------------------------------------------------
 * Protokoll (~/.duplikat-finder/lauf.log)
 * ------------------------------------------------------------------------- */
void df_log_open(void)
{
    g_mutex_lock(&g_log_lock);
    if (g_logf) fclose(g_logf);
    g_mkdir_with_parents(DF_APP_DIR, 0755);
    g_logf = fopen(DF_RUN_LOG, "w");
    g_mutex_unlock(&g_log_lock);
}

void df_log(bool show, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    g_mutex_lock(&g_log_lock);
    if (g_logf) {
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        fprintf(g_logf, "%02d:%02d:%02d  %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, msg);
        fflush(g_logf);
    }
    g_mutex_unlock(&g_log_lock);
    if (show && df_fe.log) df_fe.log(msg);
    g_free(msg);
}

/* ---------------------------------------------------------------------------
 * Standby verhindern: Sperre bei systemd-logind. Sie gilt, solange der
 * Dateideskriptor offen ist – stürzt das Programm ab, fällt sie von selbst weg.
 * ------------------------------------------------------------------------- */
static GMutex g_inhibit_lock;
static int g_inhibit_fd = -1, g_inhibit_count;

void df_inhibit_sleep(bool on, const char *why)
{
    g_mutex_lock(&g_inhibit_lock);
    if (on && g_inhibit_count++ == 0) {
        GError *err = NULL;
        GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
        GUnixFDList *fds = NULL;
        GVariant *r = bus ? g_dbus_connection_call_with_unix_fd_list_sync(
                                bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                                "org.freedesktop.login1.Manager", "Inhibit",
                                g_variant_new("(ssss)", "sleep:idle", DF_NAME,
                                              why ? why : "Duplikat-Finder arbeitet", "block"),
                                G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &fds,
                                NULL, &err)
                          : NULL;
        if (r) {
            gint32 idx;
            g_variant_get(r, "(h)", &idx);
            g_inhibit_fd = g_unix_fd_list_get(fds, idx, NULL);
            g_variant_unref(r);
        }
        if (g_inhibit_fd < 0)
            df_log(false, "Standby-Sperre nicht möglich: %s", err ? err->message : "?");
        g_clear_error(&err);
        if (fds) g_object_unref(fds);
        if (bus) g_object_unref(bus);
    } else if (!on && g_inhibit_count > 0 && --g_inhibit_count == 0 && g_inhibit_fd >= 0) {
        close(g_inhibit_fd);
        g_inhibit_fd = -1;
    }
    g_mutex_unlock(&g_inhibit_lock);
}

/* ---------------------------------------------------------------------------
 * Listen und Optionen
 * ------------------------------------------------------------------------- */
void df_strlist_add(DfStrList *l, const char *s)
{
    l->v = g_realloc_n(l->v, (gsize)l->n + 2, sizeof *l->v);
    l->v[l->n++] = g_strdup(s);
    l->v[l->n] = NULL;
}

void df_strlist_free(DfStrList *l)
{
    for (int i = 0; i < l->n; i++) g_free(l->v[i]);
    g_free(l->v);
    l->v = NULL;
    l->n = 0;
}

void df_strlist_copy(DfStrList *dst, const DfStrList *src)
{
    memset(dst, 0, sizeof *dst);
    for (int i = 0; i < src->n; i++) df_strlist_add(dst, src->v[i]);
}

int df_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 4;
}

/* CPU-Prozesse: alle Kerne bis auf 2 (Oberfläche, Datenbank, andere Programme) */
int df_default_cpu_workers(void)
{
    int n = df_cpu_count() - 2;
    return n < 1 ? 1 : n > MAX_CPU_WORKERS ? MAX_CPU_WORKERS : n;
}

void df_options_default(DfOptions *o)
{
    memset(o, 0, sizeof *o);
    o->subdirs = true;
    o->skip_symlinks = true;
    o->skip_hardlinks = true;
    o->skip_hidden = true;
    o->frames = DEFAULT_FRAMES;
    o->similarity = DEFAULT_SIMILARITY;
    o->flipped = true;
    o->duration_diff = DEFAULT_DURATION_DIFF;
    o->partial_min_ratio = 10;
    o->partial_audio = 80;
    o->partial_visual = true;
    o->partial_visual_pct = 85;
    o->cpu_workers = df_default_cpu_workers();
    o->gpu_workers = DEFAULT_GPU_WORKERS;
    o->considerate = true;
}

void df_options_free(DfOptions *o)
{
    df_strlist_free(&o->includes);
    df_strlist_free(&o->excludes);
    df_strlist_free(&o->path_contains);
    df_strlist_free(&o->path_excludes);
}

void df_options_copy(DfOptions *dst, const DfOptions *src)
{
    *dst = *src;
    df_strlist_copy(&dst->includes, &src->includes);
    df_strlist_copy(&dst->excludes, &src->excludes);
    df_strlist_copy(&dst->path_contains, &src->path_contains);
    df_strlist_copy(&dst->path_excludes, &src->path_excludes);
}

/* ---------------------------------------------------------------------------
 * Dateien
 * ------------------------------------------------------------------------- */
void df_file_clear(DfFile *f)
{
    g_free(f->path);
    g_free(f->frames);
    memset(f, 0, sizeof *f);
}

void df_file_copy(DfFile *dst, const DfFile *src)
{
    *dst = *src;
    dst->path = g_strdup(src->path);
    dst->frames = src->nframes ? g_memdup2(src->frames, sizeof(DfFrame) * (gsize)src->nframes) : NULL;
}

void df_result_free(DfResult *r)
{
    for (int i = 0; i < r->nfiles; i++) df_file_clear(&r->files[i]);
    g_free(r->files);
    g_free(r->members);
    g_free(r->groups);
    memset(r, 0, sizeof *r);
}

static const char *const VIDEO_EXTS[] = {
    "mp4", "m4v", "mkv", "webm", "avi", "mov", "wmv", "flv", "f4v", "mpg", "mpeg", "m2v", "ts",
    "m2ts", "mts", "vob", "3gp", "3g2", "ogv", "divx", "xvid", "asf", "rm", "rmvb", "mxf", "dv",
    "qt", "y4m", "m1v", "mpv", "tp", "trp", NULL};
static const char *const IMAGE_EXTS[] = {"jpg", "jpeg", "png", "webp", "bmp", "tif", "tiff",
                                         "jxl", "avif", "heic", "heif", "jfif", NULL};
/* GIF zählt als Video, wenn animiert – einfacher: als Bild (erstes Bild) */
static const char *const GIF_EXTS[] = {"gif", NULL};

static bool ext_in(const char *path, const char *const *list)
{
    const char *slash = strrchr(path, '/');
    const char *dot = strrchr(slash ? slash : path, '.');
    if (!dot || !dot[1]) return false;
    char ext[16];
    size_t n = strlen(dot + 1);
    if (n >= sizeof ext) return false;
    for (size_t i = 0; i <= n; i++) ext[i] = (char)tolower((unsigned char)dot[1 + i]);
    for (int i = 0; list[i]; i++)
        if (!strcmp(ext, list[i])) return true;
    return false;
}

bool df_is_video_ext(const char *path)
{
    return ext_in(path, VIDEO_EXTS);
}

bool df_is_image_ext(const char *path)
{
    return ext_in(path, IMAGE_EXTS) || ext_in(path, GIF_EXTS);
}

/* Muster mit * oder ? -> fnmatch auf den ganzen Pfad, sonst Teiltext (ohne Groß/klein) */
bool df_glob_or_contains(const char *path, const char *pattern)
{
    if (!pattern || !*pattern) return false;
    if (strpbrk(pattern, "*?["))
        return fnmatch(pattern, path, FNM_CASEFOLD) == 0;
    return strcasestr(path, pattern) != NULL;
}

/* ---------------------------------------------------------------------------
 * Systemwerte
 * ------------------------------------------------------------------------- */
long df_mem_available_mb(void)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %ld kB", &kb) == 1) break;
    fclose(f);
    return kb < 0 ? -1 : kb / 1024;
}

bool df_read_cpu_times(unsigned long long *busy, unsigned long long *total)
{
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return false;
    unsigned long long v[10] = {0};
    int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2],
                   &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    fclose(f);
    if (n < 4) return false;
    unsigned long long idle = v[3] + v[4], all = 0;
    for (int i = 0; i < 8; i++) all += v[i];
    *busy = all - idle;
    *total = all;
    return true;
}

long df_proc_rss_mb(pid_t pid)
{
    char p[64];
    snprintf(p, sizeof p, "/proc/%d/statm", (int)pid);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    long size, rss;
    int n = fscanf(f, "%ld %ld", &size, &rss);
    fclose(f);
    if (n != 2) return -1;
    return rss * (sysconf(_SC_PAGESIZE) / 1024) / 1024;
}

double df_proc_cpu_secs(pid_t pid)
{
    char p[64], buf[1024];
    snprintf(p, sizeof p, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(p, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *s = strrchr(buf, ')');
    if (!s) return -1;
    unsigned long ut = 0, st = 0;
    /* nach ")": Feld 3 (Zustand) ... Feld 14 utime, 15 stime */
    if (sscanf(s + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2)
        return -1;
    return (double)(ut + st) / sysconf(_SC_CLK_TCK);
}

/* ---------------------------------------------------------------------------
 * Anzeige
 * ------------------------------------------------------------------------- */
/* Zahl mit Komma als Dezimaltrenner */
static void comma_number(double v, int digits, char *out, size_t sz)
{
    snprintf(out, sz, "%.*f", digits, v);
    char *dot = strchr(out, '.');
    if (dot) *dot = ',';
}

char *df_format_bytes(double bytes, char *out, size_t sz)
{
    static const char *const unit[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int u = 0;
    while (fabs(bytes) >= 1000 && u < 5) {
        bytes /= 1000;
        u++;
    }
    char num[32];
    comma_number(bytes, u == 0 ? 0 : fabs(bytes) < 10 ? 2 : fabs(bytes) < 100 ? 1 : 0, num,
                 sizeof num);
    snprintf(out, sz, "%s %s", num, unit[u]);
    return out;
}

char *df_format_duration(double secs, char *out, size_t sz)
{
    if (!(secs >= 0)) secs = 0;
    long s = lround(secs);
    if (s >= 3600) snprintf(out, sz, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
    else snprintf(out, sz, "%ld:%02ld", s / 60, s % 60);
    return out;
}

char *df_format_eta(double secs, char *out, size_t sz)
{
    if (!(secs >= 0) || secs > 86400 * 60) {
        snprintf(out, sz, "unbekannt");
        return out;
    }
    long s = lround(secs);
    if (s < 60) snprintf(out, sz, "unter 1 min");
    else if (s < 3600) snprintf(out, sz, "ca. %ld min", (s + 30) / 60);
    else if (s < 86400) snprintf(out, sz, "ca. %ld h %02ld min", s / 3600, s / 60 % 60);
    else snprintf(out, sz, "ca. %ld Tage %ld h", s / 86400, s / 3600 % 24);
    return out;
}

char *df_format_date(long long t, char *out, size_t sz)
{
    time_t tt = (time_t)t;
    struct tm tm;
    if (t <= 0 || !localtime_r(&tt, &tm)) {
        snprintf(out, sz, "–");
        return out;
    }
    strftime(out, sz, "%d.%m.%Y %H:%M", &tm);
    return out;
}

const char *df_codec_name(const char *c)
{
    static const char *const map[][2] = {
        {"h264", "H.264"}, {"hevc", "HEVC"}, {"av1", "AV1"}, {"vp9", "VP9"}, {"vp8", "VP8"},
        {"mpeg4", "MPEG-4"}, {"mpeg2video", "MPEG-2"}, {"mpeg1video", "MPEG-1"},
        {"wmv3", "WMV9"}, {"wmv2", "WMV8"}, {"wmv1", "WMV7"}, {"vc1", "VC-1"}, {"msmpeg4v3", "DivX 3"},
        {"flv1", "FLV"}, {"h263", "H.263"}, {"theora", "Theora"}, {"prores", "ProRes"},
        {"mjpeg", "JPEG"}, {"png", "PNG"}, {"webp", "WebP"}, {"gif", "GIF"}, {"bmp", "BMP"},
        {"tiff", "TIFF"}, {"aac", "AAC"}, {"mp3", "MP3"}, {"opus", "Opus"}, {"vorbis", "Vorbis"},
        {"ac3", "AC-3"}, {"eac3", "E-AC-3"}, {"dts", "DTS"}, {"flac", "FLAC"}, {"truehd", "TrueHD"},
        {"pcm_s16le", "PCM"}, {"wmav2", "WMA"}, {"rv40", "RealVideo"}, {"cook", "RealAudio"},
    };
    if (!c || !*c) return "";
    for (size_t i = 0; i < G_N_ELEMENTS(map); i++)
        if (!strcmp(c, map[i][0])) return map[i][1];
    return c;
}
