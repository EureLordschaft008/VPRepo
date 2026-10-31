/*
 * Intro-Cutter 5.6.1 – Engine (C-Portierung der Python-Version)
 * =============================================================
 * Erkennt ein wiederkehrendes Intro in vielen Videos (inkl. Unterordner) und
 * schneidet es heraus. Bedient wird die Engine über ic_core.h – von der
 * Terminal-Version (ic_cli.c) und der grafischen Oberfläche (ic_gui.c).
 *
 * Funktionsumfang (wie die Python-Version 5.4):
 *   * Installiert ffmpeg bei Bedarf (apt). Weitere Abhängigkeiten gibt es
 *     nicht: pHash (bitgleich zu imagehash.phash), CPU-Messung (wie psutil)
 *     und JSON sind eingebaut – kein Python, kein venv
 *   * Erkennung per perceptual Hash – robust gegen unterschiedliche
 *     Seitenverhältnisse (proportionale Skalierung + Padding)
 *   * Vier Schnittmodi:
 *       1) Copy: verlustfrei am ersten Keyframe nach dem Intro (automatisch erkannte
 *          Intros: am letzten Keyframe im Intro – nie Inhalt weg). Das Ergebnis wird
 *          geprüft; bei Problemen wird automatisch neu kodiert (GPU, sonst CPU)
 *       2) Framegenau per CPU (x264; AV1-Quellen und .webm -> SVT-AV1)
 *       3) Framegenau per GPU (VAAPI, Codec wie die Quelle) – CPU als Reserve
 *       4) Framegenau per GPU + CPU parallel (maximales Tempo): die GPU nimmt die
 *          größten Dateien, die CPU (x264) parallel die kleinsten; x264 läuft mit
 *          niedrigerer Priorität, damit die GPU-Jobs nie ausgebremst werden
 *   * Alternativ Outro-Modus: Erkennung am Ende der Videos (Vergleichsframe in
 *     der Outro-Mitte, vom Dateiende gemessen); abgeschnitten wird die
 *     angegebene Länge am Ende – im Copy-Modus ohne Neukodierung
 *   * Übernimmt ALLE Spuren: Video, sämtliche Tonspuren, Untertitel, Kapitel,
 *     Anhänge (Schriften) und Cover. Ton und Untertitel werden auch beim
 *     Neukodieren nur kopiert
 *   * Korrekte Behandlung von Startzeitstempeln != 0 (z. B. TV-Aufnahmen .ts/.mpg)
 *   * Ergebnisprüfung (Dauer, Zeitachse, Auflösung, Ton-/Bildstart) – sonst Fallback
 *   * Watchdog für jeden ffmpeg-Lauf (Hänger, GPU-Anstoßen, GPU-Abschaltung),
 *     Statuszeile für lange Jobs, Ablauf-Log unter ~/.intro-cutter/last_run.log
 *   * Parallelität: CPU-Governor (CPU_TARGET %), feste GPU-Sessions,
 *     I/O-Limit pro Dateisystem für Copy-Jobs (HDD/NAS: 1, SSD: 3)
 *   * Zusätzlich zur Python-Version: GPU-Bremse (Auslastung höchstens
 *     GPU_TARGET %, damit nebenher z. B. Videos flüssig laufen),
 *     HW-Dekodierung, die Parameterwechsel mitten im Video übersteht
 *     (zusammengeschnittene Videos), gründlichere Erkennung mit mehreren
 *     Vergleichsbildern (Median), Pause (Jobs werden angehalten, der Watchdog
 *     zählt die Pause nicht) und Standby-Sperre (systemd-logind) während der Arbeit
 *   * Größte Dateien zuerst, optionales In-Place-Ersetzen der Originale,
 *     Disk-Schutz (DISK_MIN_GB), laufend gesicherter Hash-Cache
 *     (~/.intro-cutter/hash_cache.json – austauschbar mit der Python-Version)
 *   * Sicheres Verschieben: ein Original wird nie vorab gelöscht; über
 *     Dateisystemgrenzen wird erst vollständig kopiert, dann atomar ersetzt
 *   * Dateinamen mit ungültigem UTF-8 werden unterstützt
 *   * Hinterlässt keine temporären Dateien (auch nicht bei Abbruch)
 *   * Ausgelegt auf sehr große Sammlungen (getestet mit 100 000 Videos): der Aufwand
 *     wächst linear, der Hash-Cache wird ohne Zwischenbaum gelesen und geschrieben
 *   * Für die Oberfläche: Videovorschau (Bildstrom von ffmpeg, Ton über PulseAudio/
 *     PipeWire) zum Festlegen des Schnittpunkts, Hash-Cache ansehen und aufräumen,
 *     Fortschritt beim Verschieben, leere Ausgabeordner danach entfernen, ungefähre
 *     Restdauer der Bearbeitung (aus dem bisherigen Tempo gelernt)
 *   * Automatische Erkennung: wiederkehrende Intros ohne Referenz-Video finden (auch
 *     mehrere verschiedene), je Video bildgenau geschnitten; im Zweifel nicht schneiden
 *
 * Aufbau dieser Datei (Abschnitte in dieser Reihenfolge):
 *   Konfiguration – kleine Helfer (Speicher, Puffer, String-Listen) – Zeit und
 *   Zahlen – Pfade – Front-end und Lauf-Log – Prozesse – JSON – Hash-Cache –
 *   Temp-Dateien – Erkennung (pHash eines Frames) – Analyse (ffprobe) –
 *   Schnitt-Befehle – laufende Jobs – Pause und GPU-Bremse – Standby-Sperre –
 *   Watchdog – Parallelität (Governor, Gates) – Encoder-Wahl und Schnitt –
 *   GPU-Erkennung – Verschieben und Speicherplatz – Restdauer der Bearbeitung –
 *   Verarbeitung (Dispatcher) – Erkennung aller Videos – Abhängigkeiten – öffentliche
 *   Schnittstelle (ic_core.h) – Videovorschau – Hash-Cache ansehen und aufräumen –
 *   automatische Intro-Erkennung (ohne Referenz-Video)
 *
 * Namenskonventionen: ic_* = öffentliche Schnittstelle (ic_core.h), g_* = globaler
 * Zustand dieser Datei, GROSSBUCHSTABEN = Konstanten und Makros. Zeitangaben sind
 * Sekunden (double), sofern der Name nichts anderes sagt (z. B. *_us).
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "ic_core.h"

extern char **environ;

/* ---------------------------------------------------------------------------
 * Konfiguration (bei Bedarf anpassen; weitere Werte in ic_core.h)
 * ------------------------------------------------------------------------- */
static const char *const VIDEO_EXTS[] = {".mp4", ".mkv", ".mov", ".avi", ".m4v", ".webm",
                                         ".flv", ".wmv", ".mpg", ".mpeg", ".ts", NULL};

#define CPU_LOW 70              /* darunter werden CPU-Jobs hochgefahren */
int CPU_COUNT;                  /* Anzahl CPUs (wie os.cpu_count()) */
int MAX_WORKERS;                /* max(2, CPU_COUNT * 2): Obergrenze gleichzeitiger CPU-Jobs */
int POOL_THREADS;               /* max(16, CPU_COUNT * 4): Threads (warten fast nur auf ffmpeg) */

#define REENCODE_CRF "18"       /* x264: kleiner = bessere Qualität / größere Datei */
#define REENCODE_PRESET "veryfast"
#define REENCODE_THREADS 2      /* Threads pro CPU-Neukodierung */
#define CPU_START_GAP 1.0       /* s zwischen zwei neuen CPU-Jobs: der Governor sieht die Last
                                   eines Jobs, bevor der nächste startet */
#define RESUME_HOLD 3.0         /* s nach dem Fortsetzen keine neuen CPU-Jobs: erst laufen die
                                   angehaltenen wieder an */
#define SVTAV1_CRF "30"         /* CPU-Encoder für AV1-Quellen und .webm */
#define SVTAV1_PRESET "8"

static const char *VAAPI_DEVICE = NULL;  /* NULL = automatisch (/dev/dri/renderD*) */
#define GPU_QP_H264 "20"        /* Qualität (CQP): kleiner = besser / größer */
#define GPU_QP_HEVC "22"
#define GPU_Q_AV1 "90"          /* AV1: q_idx 0–255 */

#define IO_JOBS_HDD 1           /* parallele Copy-Jobs je HDD-/NAS-/unbekanntem Dateisystem */
#define IO_JOBS_SSD 3           /* parallele Copy-Jobs je SSD-Dateisystem */

#define VALIDATE_TOL 0.75       /* Toleranz (s) bei der Dauerprüfung des Ergebnisses */

char *APP_DIR;                  /* ~/.intro-cutter */
char *LOG_FILE;                 /* last_matches.txt */
char *AUTO_LOG_FILE;            /* last_auto_matches.txt: Ergebnis der automatischen Erkennung */
char *RUN_LOG;                  /* last_run.log: Ablauf + Watchdog-Ereignisse */
char *HASH_CACHE;               /* hash_cache.json */
char *SELFTEST_CLIP;            /* gpu_selftest.mp4 */
#define CACHE_SAVE_SECS 30

/* Watchdog für ffmpeg-Läufe (Sekunden) */
#define STALL_POKE_SECS 30      /* kein Fortschritt -> GPU mit Mini-Encode "anstoßen" */
#define STALL_KILL_SECS 150     /* kein Fortschritt -> ffmpeg abbrechen, nächste Methode */
#define HARD_STALL_SECS 300     /* Frames stehen (trotz CPU-Aktivität) -> abbrechen */
#define CRAWL_WINDOW_SECS 300   /* GPU-Job: Tempo über dieses Fenster messen ... */
#define CRAWL_MIN_SPEED 0.1     /* ... unter 0,1x Echtzeit gilt als hängend */
#define END_GRACE_SECS 20       /* fertig geschrieben, aber Prozess endet nicht -> abbrechen */
#define GPU_TROUBLE_MAX 2       /* so viele GPU-Hänger -> GPU für den Rest des Laufs aus */
#define HEARTBEAT_SECS 60       /* Statuszeile, wenn länger nichts fertig wurde */
#define MOVE_PROGRESS_SECS 10   /* Fortschrittszeile beim Verschieben an den Originalplatz */

/* Interne Konstanten */
#define HASH_PX 32
#define PIPELINE "pad32-gray-v2"    /* Pipeline-Version (Teil des Cache-Schlüssels) */
#define NOHASH "-"                  /* Cache-Marker: Frame nicht lesbar */
#define HASH_VF "scale=32:32:force_original_aspect_ratio=decrease," \
                "pad=32:32:(ow-iw)/2:(oh-ih)/2:color=black,format=gray"

/* Container ohne verlässlichen Keyframe-Index: hier wird beim Schnitt nicht
 * gesprungen, sondern von vorn gelesen und verworfen (langsamer, aber exakt) */
static const char *const SEEK_OUTPUT_EXTS[] = {".ts", ".mpg", ".mpeg", ".flv", ".wmv", NULL};
/* Erkennung: bei TS/PS liefert Springen oft einen falschen Frame (getestet) */
static const char *const DETECT_OUTPUT_SEEK_EXTS[] = {".ts", ".mpg", ".mpeg", NULL};
/* Springen in diesen Formaten (Outro-Erkennung, Vorschaubilder): so weit vor
 * dem Frame einsteigen (s), damit sicher ein Keyframe dazwischen liegt */
#define SEEK_PREROLL 20.0
/* Vergleichsbilder, die höchstens so weit (s) auseinander liegen, werden in
 * einem Durchgang dekodiert statt je Bild neu zu springen */
#define SEQ_SPAN 10.0
/* ... aber nur in Containern mit verlässlichen Zeitstempeln: dort ist das Bild
 * bitgleich zum Einzelsprung (AVI & Co. zählen nach einem Sprung anders) */
static const char *const SEQ_EXTS[] = {".mp4", ".m4v", ".mov", ".mkv", ".webm", NULL};
static const char *const MP4_LIKE[] = {".mp4", ".m4v", ".mov", NULL};
/* AV1 neu kodieren (CPU): in diesen Containern bleibt eine AV1-Quelle AV1 */
static const char *const AV1_CONTAINERS[] = {".mp4", ".m4v", ".mkv", ".webm", NULL};
static const char *const NETWORK_FS[] = {"nfs", "nfs4", "cifs", "smb3", "smbfs", "sshfs",
                                         "fuse.sshfs", "9p", "davfs", "fuse.rclone",
                                         "fuse.mergerfs", "zfs", NULL};
/* Cover-Codec -> Dateiendung -> MIME-Typ */
static const char *const IMG_EXT[][3] = {{"mjpeg", "jpg", "image/jpeg"},
                                         {"png", "png", "image/png"},
                                         {"bmp", "bmp", "image/bmp"},
                                         {"gif", "gif", "image/gif"},
                                         {"webp", "webp", "image/webp"}};
static const char *const GPU_CODECS[] = {"h264", "hevc", "av1", NULL};

/* ---------------------------------------------------------------------------
 * Kleine Helfer: Speicher, Puffer, String-Listen
 * ------------------------------------------------------------------------- */
static void die_out_of_memory(void)
{
    fputs("Speicher erschöpft\n", stderr);
    exit(1);
}

/* malloc, das bei Speichermangel das Programm beendet (spart NULL-Prüfungen) */
void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die_out_of_memory();
    return p;
}

/* realloc, das bei Speichermangel das Programm beendet */
void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p) die_out_of_memory();
    return p;
}

/* strdup, das bei Speichermangel das Programm beendet */
char *xstrdup(const char *s)
{
    char *p = strdup(s ? s : "");
    if (!p) die_out_of_memory();
    return p;
}

/* strndup, das bei Speichermangel das Programm beendet */
static char *xstrndup(const char *s, size_t n)
{
    char *p = strndup(s, n);
    if (!p) die_out_of_memory();
    return p;
}

__attribute__((format(printf, 1, 2)))
/* printf in einen neu angelegten String (vom Aufrufer freizugeben) */
char *xasprintf(const char *fmt, ...)
{
    char *p;
    va_list ap;
    va_start(ap, fmt);
    int r = vasprintf(&p, fmt, ap);
    va_end(ap);
    if (r < 0) die_out_of_memory();
    return p;
}

/* true, wenn s in der NULL-terminierten Liste set vorkommt */
static bool str_in_list(const char *s, const char *const *set)
{
    for (; s && *set; set++)
        if (strcmp(s, *set) == 0) return true;
    return false;
}

/* true, wenn s mit prefix beginnt */
static bool starts_with(const char *s, const char *prefix)
{
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

/* Puffer so vergrößern, dass noch extra Bytes plus NUL hineinpassen */
static void buf_reserve(Buf *b, size_t extra)
{
    if (b->n + extra + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + extra + 1) cap *= 2;
        b->p = xrealloc(b->p, cap);
        b->cap = cap;
    }
}

/* n Bytes anhängen (der Puffer bleibt NUL-terminiert) */
void buf_add(Buf *b, const void *d, size_t n)
{
    buf_reserve(b, n);
    memcpy(b->p + b->n, d, n);
    b->n += n;
    b->p[b->n] = 0;
}

/* String anhängen */
void buf_add_str(Buf *b, const char *s) { buf_add(b, s, strlen(s)); }
/* ein Zeichen anhängen */
void buf_add_char(Buf *b, char c) { buf_add(b, &c, 1); }

__attribute__((format(printf, 2, 3)))
/* formatierten Text anhängen */
void buf_printf(Buf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    buf_reserve(b, (size_t)n);
    va_start(ap, fmt);
    vsnprintf(b->p + b->n, (size_t)n + 1, fmt, ap);
    va_end(ap);
    b->n += (size_t)n;
}

/* Inhalt als C-String (nie NULL, auch bei leerem Puffer) */
const char *buf_str(Buf *b)
{
    buf_reserve(b, 0);
    b->p[b->n] = 0;
    return b->p;
}

/* Inhalt als eigenen String übernehmen; der Puffer ist danach leer */
char *buf_detach(Buf *b)
{
    buf_str(b);
    char *p = b->p;
    b->p = NULL;
    b->n = b->cap = 0;
    return p;
}

/* Inhalt leeren, Speicher behalten */
static void buf_clear(Buf *b)
{
    b->n = 0;
    if (b->p) b->p[0] = 0;
}

/* Speicher des Puffers freigeben */
void buf_free(Buf *b)
{
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

/* Kopie von s anhängen */
void strlist_add(StrList *l, const char *s)
{
    if (l->n + 2 > l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->v = xrealloc(l->v, (size_t)l->cap * sizeof *l->v);
    }
    l->v[l->n++] = xstrdup(s);
    l->v[l->n] = NULL;
}

__attribute__((format(printf, 2, 3)))
/* formatierten String anhängen */
static void strlist_add_printf(StrList *l, const char *fmt, ...)
{
    char *p;
    va_list ap;
    va_start(ap, fmt);
    int r = vasprintf(&p, fmt, ap);
    va_end(ap);
    if (r < 0) die_out_of_memory();
    strlist_add(l, p);
    free(p);
}

/* mehrere Strings anhängen, Liste mit NULL abschließen */
__attribute__((sentinel))
/* mehrere Strings anhängen (Aufruf mit NULL abschließen) */
static void strlist_add_many(StrList *l, ...)
{
    va_list ap;
    va_start(ap, l);
    for (const char *s; (s = va_arg(ap, const char *));) strlist_add(l, s);
    va_end(ap);
}

/* alle Einträge einer anderen Liste anhängen */
static void strlist_add_list(StrList *l, const StrList *o)
{
    for (int i = 0; i < o->n; i++) strlist_add(l, o->v[i]);
}

/* true, wenn s in der Liste steht */
static bool strlist_contains(const StrList *l, const char *s)
{
    for (int i = 0; i < l->n; i++)
        if (strcmp(l->v[i], s) == 0) return true;
    return false;
}

/* ersten Eintrag gleich s entfernen */
static void strlist_remove(StrList *l, const char *s)
{
    for (int i = 0; i < l->n; i++)
        if (strcmp(l->v[i], s) == 0) {
            free(l->v[i]);
            memmove(l->v + i, l->v + i + 1, (size_t)(l->n - i) * sizeof *l->v);
            l->n--;
            return;
        }
}

/* Einträge mit sep verbunden als neuer String */
static char *strlist_join(const StrList *l, const char *sep)
{
    Buf b = {0};
    for (int i = 0; i < l->n; i++) {
        if (i) buf_add_str(&b, sep);
        buf_add_str(&b, l->v[i]);
    }
    return buf_detach(&b);
}

/* Einträge und Liste freigeben */
void strlist_free(StrList *l)
{
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    l->v = NULL;
    l->n = l->cap = 0;
}

/* ---------------------------------------------------------------------------
 * Zeit und Zahlen (Formate wie in Python)
 * ------------------------------------------------------------------------- */
volatile sig_atomic_t ic_cancel = 0;           /* Strg+C / SIGTERM / EOF / "Abbrechen" */

double monotonic_secs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

/* schlafen, auch Bruchteile von Sekunden */
static void sleep_secs(double s)
{
    struct timespec ts = {(time_t)s, (long)((s - (double)(time_t)s) * 1e9)};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR && !ic_cancel) {}
}

/* float(x) wie Python: ganze Zeichenkette muss eine Zahl sein; NaN -> ungültig */
bool parse_float(const char *s, double *out)
{
    if (!s) return false;
    while (isspace((unsigned char)*s)) s++;
    if (!*s || strpbrk(s, "xX")) return false;         /* keine Hex-Zahlen */
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s) return false;
    while (isspace((unsigned char)*end)) end++;
    if (*end || isnan(v)) return false;
    *out = v;
    return true;
}

/* ganze Zahl wie int() in Python lesen (Leerraum erlaubt) -> false bei Fehler */
bool parse_int(const char *s, long *out)
{
    if (!s) return false;
    while (isspace((unsigned char)*s)) s++;
    if (!*s) return false;
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || errno) return false;
    while (isspace((unsigned char)*end)) end++;
    if (*end) return false;
    *out = v;
    return true;
}

/* round(x, nd) wie Python (korrekt gerundet über die Dezimaldarstellung) */
static double python_round(double x, int nd)
{
    char buf[512];
    snprintf(buf, sizeof buf, "%.*f", nd, x);
    return strtod(buf, NULL);
}

/* repr(float) wie Python: kürzeste eindeutige Darstellung ("2.5", "45.0", "1e-05") */
static void python_float_repr(double x, char *out, size_t sz)
{
    if (isnan(x)) { snprintf(out, sz, "nan"); return; }
    if (isinf(x)) { snprintf(out, sz, x > 0 ? "inf" : "-inf"); return; }
    if (x == 0) { snprintf(out, sz, signbit(x) ? "-0.0" : "0.0"); return; }
    char buf[64], digits[32];
    for (int prec = 1; prec <= 17; prec++) {
        snprintf(buf, sizeof buf, "%.*e", prec - 1, x);
        if (strtod(buf, NULL) == x) break;
    }
    const char *p = buf + (buf[0] == '-');
    int ndigits = 0;
    for (; *p && *p != 'e'; p++)
        if (isdigit((unsigned char)*p)) digits[ndigits++] = *p;
    while (ndigits > 1 && digits[ndigits - 1] == '0') ndigits--;
    digits[ndigits] = 0;
    int exp10 = atoi(p + 1), decpt = exp10 + 1;
    Buf b = {0};
    if (x < 0) buf_add_char(&b, '-');
    if (decpt <= -4 || decpt > 16) {
        buf_add_char(&b, digits[0]);
        if (ndigits > 1) {
            buf_add_char(&b, '.');
            buf_add_str(&b, digits + 1);
        }
        buf_printf(&b, "e%c%02d", exp10 < 0 ? '-' : '+', abs(exp10));
    } else if (decpt <= 0) {
        buf_add_str(&b, "0.");
        for (int i = 0; i < -decpt; i++) buf_add_char(&b, '0');
        buf_add_str(&b, digits);
    } else if (decpt >= ndigits) {
        buf_add_str(&b, digits);
        for (int i = ndigits; i < decpt; i++) buf_add_char(&b, '0');
        buf_add_str(&b, ".0");
    } else {
        buf_add(&b, digits, (size_t)decpt);
        buf_add_char(&b, '.');
        buf_add_str(&b, digits + decpt);
    }
    snprintf(out, sz, "%s", buf_str(&b));
    buf_free(&b);
}

/* Dauer als "1 h 2 min 3 s" (gerundet wie Pythons round) */
char *format_duration(double s, char *out, size_t sz)
{
    long long v = (long long)nearbyint(s);     /* round() wie Python (Banker's Rounding) */
    long long h = v / 3600, r = v % 3600;
    if (r < 0) { r += 3600; h--; }
    long long m = r / 60, sec = r % 60;
    if (h)
        snprintf(out, sz, "%lld h %lld min %lld s", h, m, sec);
    else if (m)
        snprintf(out, sz, "%lld min %lld s", m, sec);
    else
        snprintf(out, sz, "%lld s", sec);
    return out;
}

/* ---------------------------------------------------------------------------
 * Pfade (Semantik wie os.path)
 * ------------------------------------------------------------------------- */
/* os.path.splitext(p)[1].lower() */
static void path_extension(const char *p, char *out, size_t sz)
{
    const char *slash = strrchr(p, '/'), *base = slash ? slash + 1 : p;
    const char *dot = strrchr(base, '.');
    const char *q = base;
    while (q < dot && *q == '.') q++;
    if (!dot || q == dot) {                    /* keine Endung (auch ".bashrc") */
        out[0] = 0;
        return;
    }
    snprintf(out, sz, "%s", dot);
    for (char *c = out; *c; c++) *c = (char)tolower((unsigned char)*c);
}

/* letzter Pfadbestandteil (wie os.path.basename) */
static const char *path_basename(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

/* Verzeichnisanteil eines Pfads (wie os.path.dirname) */
static char *path_dirname(const char *p)
{
    const char *s = strrchr(p, '/');
    if (!s) return xstrdup("");
    size_t n = (size_t)(s - p) + 1;
    size_t k = n;
    while (k > 0 && p[k - 1] == '/') k--;
    return k ? xstrndup(p, k) : xstrndup(p, n);    /* nur Schrägstriche: beibehalten */
}

/* Pfade verbinden (wie os.path.join: ein absoluter zweiter Teil ersetzt den ersten) */
char *path_join(const char *a, const char *b)
{
    if (b[0] == '/' || !a[0]) return xstrdup(b);
    size_t n = strlen(a);
    return a[n - 1] == '/' ? xasprintf("%s%s", a, b) : xasprintf("%s/%s", a, b);
}

/* Pfad normalisieren: doppelte Schrägstriche, "." und ".." auflösen (wie os.path.normpath) */
char *normpath(const char *path)
{
    if (!*path) return xstrdup(".");
    int initial = 0;
    if (path[0] == '/') initial = (path[1] == '/' && path[2] != '/') ? 2 : 1;
    StrList comps = {0};
    for (const char *p = path; *p;) {
        while (*p == '/') p++;
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - s);
        if (n == 0 || (n == 1 && s[0] == '.')) continue;
        if (n == 2 && s[0] == '.' && s[1] == '.' &&
            !(!initial && comps.n == 0) && !(comps.n && !strcmp(comps.v[comps.n - 1], ".."))) {
            if (comps.n) {
                free(comps.v[--comps.n]);
                comps.v[comps.n] = NULL;
            }
            continue;
        }
        char *c = xstrndup(s, n);
        strlist_add(&comps, c);
        free(c);
    }
    Buf b = {0};
    for (int i = 0; i < initial; i++) buf_add_char(&b, '/');
    for (int i = 0; i < comps.n; i++) {
        if (i) buf_add_char(&b, '/');
        buf_add_str(&b, comps.v[i]);
    }
    if (!b.n) buf_add_char(&b, '.');
    strlist_free(&comps);
    return buf_detach(&b);
}

/* os.path.expanduser: "~" und "~user" am Anfang */
char *expanduser(const char *p)
{
    if (p[0] != '~') return xstrdup(p);
    const char *slash = strchr(p, '/');
    size_t ulen = slash ? (size_t)(slash - p - 1) : strlen(p) - 1;
    const char *home = NULL;
    struct passwd *pw_entry;
    if (ulen == 0) {
        home = getenv("HOME");
        if (!home) {
            pw_entry = getpwuid(getuid());
            if (!pw_entry) return xstrdup(p);
            home = pw_entry->pw_dir;
        }
    } else {
        char *user = xstrndup(p + 1, ulen);
        pw_entry = getpwnam(user);
        free(user);
        if (!pw_entry) return xstrdup(p);
        home = pw_entry->pw_dir;
    }
    size_t home_len = strlen(home);
    while (home_len > 0 && home[home_len - 1] == '/') home_len--;
    char *r = xasprintf("%.*s%s", (int)home_len, home, slash ? slash : "");
    if (!*r) {
        free(r);
        return xstrdup("/");
    }
    return r;
}

/* os.path.realpath: löst Symlinks auf; nicht existierende Teile bleiben stehen */
char *realpath_lenient(const char *path)
{
    char buf[PATH_MAX];
    if (realpath(path, buf)) return xstrdup(buf);
    char *abs_path;
    if (path[0] == '/') {
        abs_path = xstrdup(path);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof cwd)) return normpath(path);
        abs_path = path_join(cwd, path);
    }
    Buf resolved = {0};
    for (const char *p = abs_path; *p;) {
        while (*p == '/') p++;
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t n = (size_t)(p - s);
        if (n == 0 || (n == 1 && s[0] == '.')) continue;
        if (n == 2 && s[0] == '.' && s[1] == '.') {
            char *slash = resolved.n ? strrchr(resolved.p, '/') : NULL;
            if (slash) {
                resolved.n = (size_t)(slash - resolved.p);
                resolved.p[resolved.n] = 0;
            }
            continue;
        }
        buf_add_char(&resolved, '/');
        buf_add(&resolved, s, n);
        if (realpath(buf_str(&resolved), buf)) {
            buf_clear(&resolved);
            if (strcmp(buf, "/") != 0) buf_add_str(&resolved, buf);
        }
    }
    free(abs_path);
    if (!resolved.n) buf_add_char(&resolved, '/');
    return buf_detach(&resolved);
}

/* os.makedirs(path, exist_ok=True) -> 0 oder -1 (errno gesetzt). Ist created gesetzt,
 * werden die dabei neu angelegten Ordner in dieser Reihenfolge vermerkt (Eltern vor
 * Kindern) – so lassen sich später genau diese wieder entfernen, wenn sie leer sind. */
static int mkdirs_recording(const char *path, StrList *created)
{
    if (!*path) {
        errno = ENOENT;
        return -1;
    }
    char *tmp = xstrdup(path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(tmp, 0777) == 0) {
            if (created) strlist_add(created, tmp);
        } else if (errno != EEXIST) {
            struct stat st;
            if (stat(tmp, &st) != 0 || !S_ISDIR(st.st_mode)) {
                int e = errno;
                free(tmp);
                errno = e;
                return -1;
            }
        }
        *p = '/';
    }
    int rc = 0;
    if (mkdir(tmp, 0777) == 0) {
        if (created) strlist_add(created, tmp);
    } else {
        struct stat st;
        if (stat(tmp, &st) != 0) rc = -1;
        else if (!S_ISDIR(st.st_mode)) { errno = EEXIST; rc = -1; }
    }
    free(tmp);
    return rc;
}

/* os.makedirs(path, exist_ok=True) -> 0 oder -1 (errno gesetzt) */
static int mkdirs(const char *path)
{
    return mkdirs_recording(path, NULL);
}

/* true, wenn p eine Datei ist (auch über einen Symlink) */
bool is_file(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

/* true, wenn p ein Verzeichnis ist */
bool is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Dateigröße in Byte, 0 wenn nicht lesbar (wie _size() im Python-Original) */
static long long file_size(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (long long)st.st_size : 0;
}

/* true, wenn die Datei existiert und nicht leer ist */
static bool file_not_empty(const char *p)
{
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

/* Datei löschen, Fehler ignorieren (wie _rm() im Python-Original) */
static void remove_file(const char *p)
{
    if (p && is_file(p)) unlink(p);
}

/* shutil.command_exists(name) is not None */
static bool command_exists(const char *name)
{
    const char *path = getenv("PATH");
    if (!path) path = "/bin:/usr/bin";
    while (true) {
        const char *e = strchr(path, ':');
        size_t n = e ? (size_t)(e - path) : strlen(path);
        char *dir = xstrndup(path, n);
        char *full = path_join(dir, name);
        bool ok = access(full, X_OK) == 0 && !is_dir(full);
        free(dir);
        free(full);
        if (ok) return true;
        if (!e) return false;
        path = e + 1;
    }
}

/* ---------------------------------------------------------------------------
 * Front-end, Ausgabe, Lauf-Log
 * ------------------------------------------------------------------------- */
/* Standard-Front-end: Ausgabe auf stdout; bei wenig Platz nichts verschieben
 * und anhalten (die Oberflächen setzen eigene Callbacks) */
static void default_print(const char *msg)
{
    fputs(msg, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

/* Standard ohne Oberfläche: bei wenig Platz nichts verschieben */
static bool default_ask_disk_move(double free_space_gb, int n_finished)
{
    (void)free_space_gb;
    (void)n_finished;
    return false;
}

/* Standard ohne Oberfläche: bei wenig Platz nicht weiterlaufen */
static bool default_ask_disk_continue(double free_space_gb)
{
    (void)free_space_gb;
    return false;
}

IcFrontend ic_fe = {.log = default_print, .info = default_print,
                    .ask_disk_move = default_ask_disk_move,
                    .ask_disk_continue = default_ask_disk_continue};

/* Hinweis an das Front-end (kommt nicht ins Lauf-Log) */
__attribute__((format(printf, 1, 2)))
/* Hinweis an die Oberfläche geben (printf-Format) */
static void frontend_info(const char *fmt, ...)
{
    char *msg;
    va_list ap;
    va_start(ap, fmt);
    int r = vasprintf(&msg, fmt, ap);
    va_end(ap);
    if (r < 0) die_out_of_memory();
    if (ic_fe.info) ic_fe.info(msg);
    free(msg);
}

/* Threads starten, ohne dass sie Signale abfangen (das erledigt der Hauptthread) */
static int start_thread(pthread_t *thread, void *(*thread_main)(void *), void *arg)
{
    sigset_t blocked, old_mask;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    sigaddset(&blocked, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &blocked, &old_mask);
    int rc = pthread_create(thread, NULL, thread_main, arg);
    pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
    return rc;
}

static pthread_mutex_t g_print_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *g_run_log;

/* neues Lauf-Log ~/.intro-cutter/last_run.log anlegen (ersetzt das vorige) */
void ic_log_open(void)
{
    pthread_mutex_lock(&g_print_lock);
    if (g_run_log) fclose(g_run_log);
    g_run_log = mkdirs(APP_DIR) == 0 ? fopen(RUN_LOG, "we") : NULL;
    pthread_mutex_unlock(&g_print_lock);
}

/* Meldung an das Front-end (thread-sicher) und ins Lauf-Log */
void ic_log(bool show, const char *fmt, ...)
{
    char *msg;
    va_list ap;
    va_start(ap, fmt);
    int r = vasprintf(&msg, fmt, ap);
    va_end(ap);
    if (r < 0) die_out_of_memory();
    pthread_mutex_lock(&g_print_lock);
    if (show && ic_fe.log) ic_fe.log(msg);
    if (g_run_log) {
        char ts[16];
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(ts, sizeof ts, "%H:%M:%S ", &tm);
        const char *s = msg;
        while (isspace((unsigned char)*s)) s++;
        size_t n = strlen(s);
        while (n && isspace((unsigned char)s[n - 1])) n--;
        fprintf(g_run_log, "%s%.*s\n", ts, (int)n, s);
        fflush(g_run_log);
    }
    pthread_mutex_unlock(&g_print_lock);
    free(msg);
}

/* ---------------------------------------------------------------------------
 * Prozesse (ffmpeg, ffprobe & Co.)
 * ------------------------------------------------------------------------- */
enum { IO_INHERIT, IO_NULL, IO_PIPE };

typedef struct {
    pid_t pid;
    int out, err;           /* Lese-Enden der Pipes (-1 = keine / geschlossen) */
    bool reaped, lost;
    int status;
} ChildProc;

/* nice/ionice voranstellen, damit ffmpeg dem Nutzer Vorrang lässt.
 * PRIO_LOW (nice 19): Modus 4 reiht die x264-Jobs hinter die GPU-Jobs ein. */
enum { PRIO_NONE, PRIO_NICE, PRIO_LOW };
static const char *NICE_PREFIX[3][6];
static int NICE_N[3];

/* Befehlspräfixe für niedrige Priorität ermitteln (ionice -c 3, nice -n 15 bzw. 19) */
static void init_priority_prefixes(void)
{
    static const char *const level[3] = {NULL, "15", "19"};
    for (int p = PRIO_NICE; p <= PRIO_LOW; p++) {
        const char **v = NICE_PREFIX[p];
        int *n = &NICE_N[p];
        if (command_exists("ionice")) {              /* IO-Klasse "idle" */
            v[(*n)++] = "ionice";
            v[(*n)++] = "-c";
            v[(*n)++] = "3";
        }
        if (command_exists("nice")) {       /* niedrige CPU-Priorität */
            v[(*n)++] = "nice";
            v[(*n)++] = "-n";
            v[(*n)++] = level[p];
        }
    }
}

/* Dateideskriptor schließen und auf -1 setzen */
static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

/* Startet cmd (Pipes mit O_CLOEXEC, damit parallel gestartete Prozesse sie
 * nicht erben). prio: nice-Stufe (Index in NICE_PREFIX); stdin_mode, stdout_mode,
 * stderr_mode: IO_INHERIT, IO_NULL oder IO_PIPE -> 0 oder errno-Wert */
static int spawn_child(ChildProc *c, const StrList *cmd, int prio, int stdin_mode, int stdout_mode,
                       int stderr_mode)
{
    memset(c, 0, sizeof *c);
    c->out = c->err = -1;
    int out_pipe[2] = {-1, -1}, err_pipe[2] = {-1, -1};
    if ((stdout_mode == IO_PIPE && pipe2(out_pipe, O_CLOEXEC) != 0) ||
        (stderr_mode == IO_PIPE && pipe2(err_pipe, O_CLOEXEC) != 0)) {
        int saved_errno = errno;
        for (int i = 0; i < 2; i++) {
            close_fd(&out_pipe[i]);
            close_fd(&err_pipe[i]);
        }
        return saved_errno;
    }
    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_init(&file_actions);
    if (stdin_mode == IO_NULL)
        posix_spawn_file_actions_addopen(&file_actions, 0, "/dev/null", O_RDONLY, 0);
    if (stdout_mode == IO_PIPE) posix_spawn_file_actions_adddup2(&file_actions, out_pipe[1], 1);
    else if (stdout_mode == IO_NULL)
        posix_spawn_file_actions_addopen(&file_actions, 1, "/dev/null", O_WRONLY, 0);
    if (stderr_mode == IO_PIPE) posix_spawn_file_actions_adddup2(&file_actions, err_pipe[1], 2);
    else if (stderr_mode == IO_NULL)
        posix_spawn_file_actions_addopen(&file_actions, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t no_signals, default_signals;
    sigemptyset(&no_signals);
    sigemptyset(&default_signals);
    sigaddset(&default_signals, SIGPIPE);
    sigaddset(&default_signals, SIGINT);
    sigaddset(&default_signals, SIGTERM);
    sigaddset(&default_signals, SIGHUP);
    posix_spawnattr_setsigmask(&attr, &no_signals);
    posix_spawnattr_setsigdefault(&attr, &default_signals);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    int argc = 0;
    char **argv = xmalloc((size_t)(NICE_N[prio] + cmd->n + 1) * sizeof *argv);
    for (int i = 0; i < NICE_N[prio]; i++) argv[argc++] = (char *)NICE_PREFIX[prio][i];
    for (int i = 0; i < cmd->n; i++) argv[argc++] = cmd->v[i];
    argv[argc] = NULL;
    int rc = posix_spawnp(&c->pid, argv[0], &file_actions, &attr, argv, environ);
    free(argv);
    posix_spawn_file_actions_destroy(&file_actions);
    posix_spawnattr_destroy(&attr);
    close_fd(&out_pipe[1]);
    close_fd(&err_pipe[1]);
    if (rc != 0) {
        close_fd(&out_pipe[0]);
        close_fd(&err_pipe[0]);
        return rc;
    }
    c->out = out_pipe[0];
    c->err = err_pipe[0];
    if (c->out >= 0) fcntl(c->out, F_SETFL, O_NONBLOCK);
    if (c->err >= 0) fcntl(c->err, F_SETFL, O_NONBLOCK);
    return 0;
}

/* true, wenn der Kindprozess beendet ist (wie Popen.poll() is not None in Python) */
static bool child_exited(ChildProc *c)
{
    if (c->reaped) return true;
    int st;
    pid_t r = waitpid(c->pid, &st, WNOHANG);
    if (r == c->pid) {
        c->reaped = true;
        c->status = st;
    } else if (r < 0 && errno == ECHILD) {
        c->reaped = c->lost = true;
    }
    return c->reaped;
}

/* auf das Ende des Kindprozesses warten, höchstens timeout Sekunden -> true, wenn beendet */
static bool wait_child(ChildProc *c, double timeout)
{
    double end = monotonic_secs() + timeout;
    while (!child_exited(c)) {
        if (monotonic_secs() >= end) return false;
        struct timespec ts = {0, 20 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    return true;
}

/* Exit-Code wie Popen.returncode (negativ = beendendes Signal) */
static int child_exit_code(const ChildProc *c)
{
    if (!c->reaped || c->lost) return -1;
    if (WIFEXITED(c->status)) return WEXITSTATUS(c->status);
    if (WIFSIGNALED(c->status)) return -WTERMSIG(c->status);
    return -1;
}

/* Kindprozess sofort beenden (SIGKILL) */
static void kill_child(ChildProc *c)
{
    if (!c->reaped) kill(c->pid, SIGKILL);
}

/* wartet im Hintergrund auf einen aufgegebenen Prozess, damit kein Zombie bleibt */
static void *reaper_thread(void *arg)
{
    pid_t pid = (pid_t)(intptr_t)arg;
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    return NULL;
}

/* Prozess hängt (z. B. im GPU-Treiber) und lässt sich nicht beenden:
 * aufgeben und im Hintergrund einsammeln, sobald er doch endet */
static void abandon_child(ChildProc *c)
{
    close_fd(&c->out);
    close_fd(&c->err);
    if (c->reaped) return;
    pthread_t t;
    if (start_thread(&t, reaper_thread, (void *)(intptr_t)c->pid) == 0) pthread_detach(t);
    c->reaped = c->lost = true;
}

/* verfügbare Daten lesen (fd ist nicht blockierend); am Pipe-Ende wird fd geschlossen */
static void read_available(int *fd, Buf *b)
{
    char tmp[65536];
    while (*fd >= 0) {
        ssize_t r = read(*fd, tmp, sizeof tmp);
        if (r > 0) {
            if (b) buf_add(b, tmp, (size_t)r);
        } else if (r < 0 && errno == EINTR) {
            continue;
        } else if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        } else {
            close_fd(fd);
        }
    }
}

/* Programm ausführen -> rc; stdout/stderr landen in out/err (NULL = verwerfen).
 * timeout < 0: unbegrenzt. Bei Zeitüberschreitung (oder Strg+C) wird der
 * Prozess beendet; hängt er im Treiber fest (nicht beendbar), wird nach 10 s
 * aufgegeben statt ewig zu warten. */
static int run_capture_output(const StrList *cmd, bool nice, double timeout, Buf *out, Buf *err)
{
    ChildProc c;
    int e = spawn_child(&c, cmd, nice ? PRIO_NICE : PRIO_NONE, IO_NULL, out ? IO_PIPE : IO_NULL,
                        err ? IO_PIPE : IO_NULL);
    if (e) {
        if (err) buf_printf(err, "[Errno %d] %s: '%s'", e, strerror(e), cmd->v[0]);
        return -1;
    }
    double deadline = timeout >= 0 ? monotonic_secs() + timeout : INFINITY;
    bool killed = false;
    while (true) {
        double now = monotonic_secs();
        if (!killed && (now >= deadline || ic_cancel)) {
            killed = true;
            kill_child(&c);
            deadline = now + 10;
        } else if (killed && now >= deadline) {
            if (out) buf_clear(out);
            if (err) buf_clear(err);
            abandon_child(&c);
            break;
        }
        if (c.out < 0 && c.err < 0) {
            if (child_exited(&c)) break;
            struct timespec ts = {0, 20 * 1000 * 1000};
            nanosleep(&ts, NULL);
            continue;
        }
        struct pollfd poll_fds[2];
        int n = 0;
        if (c.out >= 0) poll_fds[n++] = (struct pollfd){.fd = c.out, .events = POLLIN};
        if (c.err >= 0) poll_fds[n++] = (struct pollfd){.fd = c.err, .events = POLLIN};
        double wait = fmin(0.25, deadline - now);
        poll(poll_fds, (nfds_t)n, wait > 0 ? (int)(wait * 1000) + 1 : 0);
        read_available(&c.out, out);
        read_available(&c.err, err);
    }
    if (killed) {
        if (err) buf_add_str(err, "\nZeitüberschreitung");
        return -1;
    }
    return child_exit_code(&c);
}

/* ffmpeg/ffprobe mit niedriger Priorität ausführen -> rc (wie run_ffmpeg() in Python) */
static int run_ffmpeg(const StrList *cmd, double timeout, Buf *out, Buf *err)
{
    return run_capture_output(cmd, true, timeout, out, err);
}

/* letzte aussagekräftige Fehlerzeile von ffmpeg (max. 140 Zeichen) */
static void last_error_line(const char *err, char *out, size_t sz)
{
    const char *best = NULL;
    size_t best_n = 0;
    for (const char *p = err ? err : ""; *p;) {
        const char *s = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        const char *e = p;
        if (*p) p++;
        if (e - s >= 4 && !strncmp(s, "Svt[", 4)) continue;
        while (s < e && isspace((unsigned char)*s)) s++;
        while (e > s && isspace((unsigned char)e[-1])) e--;
        if (e > s) {
            best = s;
            best_n = (size_t)(e - s);
        }
    }
    if (!best) {
        snprintf(out, sz, "ffmpeg-Fehler");
        return;
    }
    size_t n = 0;                               /* 140 Zeichen (nicht Bytes) */
    for (int chars = 0; n < best_n && chars < 140; chars++) {
        n++;
        while (n < best_n && ((unsigned char)best[n] & 0xC0) == 0x80) n++;
    }
    snprintf(out, sz, "%.*s", (int)n, best);
}

/* ---------------------------------------------------------------------------
 * JSON (ffprobe-Ausgabe lesen, Hash-Cache lesen/schreiben)
 * ------------------------------------------------------------------------- */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JsonValue {
    JType type;
    double num;             /* J_NUM, J_BOOL */
    char *str;              /* J_STR (UTF-8) */
    int n, cap;
    char **keys;            /* J_OBJ */
    struct JsonValue **items; /* J_ARR, J_OBJ */
} JsonValue;

typedef struct {
    const char *pos, *end;
    int depth;
} JsonParser;

/* JSON-Wert samt Inhalt freigeben */
static void json_free(JsonValue *v)
{
    if (!v) return;
    free(v->str);
    for (int i = 0; i < v->n; i++) {
        if (v->keys) free(v->keys[i]);
        json_free(v->items[i]);
    }
    free(v->keys);
    free(v->items);
    free(v);
}

/* neuen, leeren JSON-Wert vom angegebenen Typ anlegen */
static JsonValue *json_new_value(JType t)
{
    JsonValue *v = xmalloc(sizeof *v);
    memset(v, 0, sizeof *v);
    v->type = t;
    return v;
}

/* Element an ein Array bzw. Schlüssel und Wert an ein Objekt anhängen */
static void json_append_item(JsonValue *v, char *key, JsonValue *item)
{
    if (v->n == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = xrealloc(v->items, (size_t)v->cap * sizeof *v->items);
        if (v->type == J_OBJ) v->keys = xrealloc(v->keys, (size_t)v->cap * sizeof *v->keys);
    }
    if (v->type == J_OBJ) v->keys[v->n] = key;
    v->items[v->n++] = item;
}

/* Leerraum im JSON-Text überspringen */
static void json_skip_space(JsonParser *parser)
{
    while (parser->pos < parser->end && strchr(" \t\r\n", *parser->pos) && *parser->pos)
        parser->pos++;
}

/* Unicode-Codepoint als UTF-8 an den Puffer anhängen */
static void utf8_encode(Buf *b, unsigned codepoint)
{
    char u[4];
    if (codepoint < 0x80) {
        buf_add_char(b, (char)codepoint);
    } else if (codepoint < 0x800) {
        u[0] = (char)(0xC0 | codepoint >> 6);
        u[1] = (char)(0x80 | (codepoint & 0x3F));
        buf_add(b, u, 2);
    } else if (codepoint < 0x10000) {
        u[0] = (char)(0xE0 | codepoint >> 12);
        u[1] = (char)(0x80 | (codepoint >> 6 & 0x3F));
        u[2] = (char)(0x80 | (codepoint & 0x3F));
        buf_add(b, u, 3);
    } else {
        u[0] = (char)(0xF0 | codepoint >> 18);
        u[1] = (char)(0x80 | (codepoint >> 12 & 0x3F));
        u[2] = (char)(0x80 | (codepoint >> 6 & 0x3F));
        u[3] = (char)(0x80 | (codepoint & 0x3F));
        buf_add(b, u, 4);
    }
}

/* strikte UTF-8-Dekodierung wie Python -> Länge (0 = ungültiges Byte) */
static int utf8_decode(const unsigned char *p, unsigned *codepoint)
{
    unsigned c = p[0], cont_min = 0x80, cont_max = 0xBF;
    int n;
    if (c < 0x80) {
        *codepoint = c;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) {
        n = 2;
        *codepoint = c & 0x1F;
    } else if (c >= 0xE0 && c <= 0xEF) {
        n = 3;
        *codepoint = c & 0x0F;
        if (c == 0xE0) cont_min = 0xA0;
        if (c == 0xED) cont_max = 0x9F;     /* keine Surrogate */
    } else if (c >= 0xF0 && c <= 0xF4) {
        n = 4;
        *codepoint = c & 0x07;
        if (c == 0xF0) cont_min = 0x90;
        if (c == 0xF4) cont_max = 0x8F;
    } else {
        return 0;
    }
    for (int i = 1; i < n; i++) {
        unsigned d = p[i];
        if (i == 1 ? (d < cont_min || d > cont_max) : (d < 0x80 || d > 0xBF)) return 0;
        *codepoint = *codepoint << 6 | (d & 0x3F);
    }
    return n;
}

/* vier Hex-Ziffern einer \uXXXX-Escape-Folge lesen */
static bool json_parse_hex4(JsonParser *parser, unsigned *out)
{
    if (parser->end - parser->pos < 4) return false;
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = parser->pos[i];
        int h = c >= '0' && c <= '9' ? c - '0'
              : c >= 'a' && c <= 'f' ? c - 'a' + 10
              : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (h < 0) return false;
        v = v << 4 | (unsigned)h;
    }
    parser->pos += 4;
    *out = v;
    return true;
}

/* Zeichenkette ab '"'. Einzelne \udc80–\udcff sind Bytes, die Python per
 * surrogateescape kodiert hat (Dateinamen, die kein gültiges UTF-8 sind) */
static char *json_parse_string(JsonParser *parser)
{
    if (parser->pos >= parser->end || *parser->pos != '"') return NULL;
    parser->pos++;
    Buf b = {0};
    buf_reserve(&b, 0);
    while (parser->pos < parser->end) {
        char c = *parser->pos++;
        if (c == '"') return buf_detach(&b);
        if (c != '\\') {
            buf_add_char(&b, c);
            continue;
        }
        if (parser->pos >= parser->end) break;
        c = *parser->pos++;
        unsigned codepoint, low_surrogate;
        switch (c) {
        case '"': case '\\': case '/': buf_add_char(&b, c); break;
        case 'b': buf_add_char(&b, '\b'); break;
        case 'f': buf_add_char(&b, '\f'); break;
        case 'n': buf_add_char(&b, '\n'); break;
        case 'r': buf_add_char(&b, '\r'); break;
        case 't': buf_add_char(&b, '\t'); break;
        case 'u':
            if (!json_parse_hex4(parser, &codepoint)) goto fail;
            if (codepoint >= 0xD800 && codepoint <= 0xDBFF && parser->end - parser->pos >= 6 &&
                parser->pos[0] == '\\' && parser->pos[1] == 'u') {
                const char *save = parser->pos;
                parser->pos += 2;
                if (json_parse_hex4(parser, &low_surrogate) && low_surrogate >= 0xDC00 &&
                    low_surrogate <= 0xDFFF)
                    codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low_surrogate - 0xDC00);
                else
                    parser->pos = save;
            }
            if (codepoint >= 0xDC80 && codepoint <= 0xDCFF)
                buf_add_char(&b, (char)(codepoint - 0xDC00));
            else
                utf8_encode(&b, codepoint);
            break;
        default:
            goto fail;
        }
    }
fail:
    buf_free(&b);
    return NULL;
}

/* einen beliebigen JSON-Wert lesen (rekursiv) */
static JsonValue *json_parse_value(JsonParser *parser)
{
    json_skip_space(parser);
    if (parser->pos >= parser->end || parser->depth >= 256) return NULL;
    parser->depth++;
    JsonValue *v = NULL;
    char c = *parser->pos;
    size_t left = (size_t)(parser->end - parser->pos);
    if (c == '{' || c == '[') {
        bool obj = c == '{';
        char close = obj ? '}' : ']';
        v = json_new_value(obj ? J_OBJ : J_ARR);
        parser->pos++;
        json_skip_space(parser);
        if (parser->pos < parser->end && *parser->pos == close) {
            parser->pos++;
            goto done;
        }
        while (true) {
            char *key = NULL;
            if (obj) {
                json_skip_space(parser);
                key = json_parse_string(parser);
                json_skip_space(parser);
                if (!key || parser->pos >= parser->end || *parser->pos != ':') {
                    free(key);
                    goto fail;
                }
                parser->pos++;
            }
            JsonValue *item = json_parse_value(parser);
            if (!item) {
                free(key);
                goto fail;
            }
            json_append_item(v, key, item);
            json_skip_space(parser);
            if (parser->pos < parser->end && *parser->pos == ',') {
                parser->pos++;
                continue;
            }
            if (parser->pos < parser->end && *parser->pos == close) {
                parser->pos++;
                break;
            }
            goto fail;
        }
    } else if (c == '"') {
        char *s = json_parse_string(parser);
        if (!s) goto fail;
        v = json_new_value(J_STR);
        v->str = s;
    } else if (left >= 4 && !strncmp(parser->pos, "true", 4)) {
        v = json_new_value(J_BOOL);
        v->num = 1;
        parser->pos += 4;
    } else if (left >= 5 && !strncmp(parser->pos, "false", 5)) {
        v = json_new_value(J_BOOL);
        parser->pos += 5;
    } else if (left >= 4 && !strncmp(parser->pos, "null", 4)) {
        v = json_new_value(J_NULL);
        parser->pos += 4;
    } else {
        const char *s = parser->pos;
        while (parser->pos < parser->end && *parser->pos && strchr("+-0123456789.eE", *parser->pos))
            parser->pos++;
        if (parser->pos == s) goto fail;
        char *tok = xstrndup(s, (size_t)(parser->pos - s)), *e;
        v = json_new_value(J_NUM);
        v->num = strtod(tok, &e);
        bool ok = *e == 0;
        free(tok);
        if (!ok) goto fail;
    }
done:
    parser->depth--;
    return v;
fail:
    json_free(v);
    return NULL;
}

/* JSON-Text parsen -> Wurzelwert oder NULL bei Fehler */
static JsonValue *json_parse(const char *s, size_t n)
{
    JsonParser parser = {s, s + n, 0};
    JsonValue *v = json_parse_value(&parser);
    json_skip_space(&parser);
    if (v && parser.pos != parser.end) {
        json_free(v);
        return NULL;
    }
    return v;
}

/* Wert zu key in einem JSON-Objekt (NULL, wenn nicht vorhanden); bei doppelten Schlüsseln
 * gewinnt wie bei Pythons dict der letzte Eintrag */
static JsonValue *json_get(const JsonValue *object, const char *key)
{
    if (!object || object->type != J_OBJ) return NULL;
    for (int i = object->n - 1; i >= 0; i--)
        if (!strcmp(object->keys[i], key)) return object->items[i];
    return NULL;
}

/* String-Inhalt eines JSON-Werts (NULL, wenn es kein String ist) */
static const char *json_string_of(const JsonValue *v)
{
    return v && v->type == J_STR ? v->str : NULL;
}

/* Zahl eines JSON-Werts, auch als String (wie _f() im Python-Original) -> false, wenn keine Zahl */
static bool json_number_of(const JsonValue *v, double *out)
{
    if (!v) return false;
    if (v->type == J_NUM || v->type == J_BOOL) {
        if (isnan(v->num)) return false;
        *out = v->num;
        return true;
    }
    return v->type == J_STR && parse_float(v->str, out);
}

/* Wahrheitswert wie in Python (0, leer und null sind falsch) */
static bool json_truthy(const JsonValue *v)
{
    if (!v) return false;
    switch (v->type) {
    case J_BOOL: case J_NUM: return v->num != 0;
    case J_STR: return v->str[0] != 0;
    case J_ARR: case J_OBJ: return v->n > 0;
    default: return false;
    }
}

/* Anzeige wie Pythons backslashreplace: ungültige UTF-8-Bytes als \udcXX */
char *utf8_escape(const char *s)
{
    Buf b = {0};
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        unsigned codepoint;
        int len = utf8_decode(p, &codepoint);
        if (len) {
            buf_add(&b, p, (size_t)len);
            p += len;
        } else {
            buf_printf(&b, "\\udc%02x", *p++);
        }
    }
    return buf_detach(&b);
}

/* JSON-String wie Python json.dump (ensure_ascii): Nicht-ASCII als \uXXXX,
 * ungültige UTF-8-Bytes als \udcXX (surrogateescape) */
static void json_write_string(Buf *b, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;
    buf_add_char(b, '"');
    while (*p) {
        unsigned codepoint;
        int len = utf8_decode(p, &codepoint);
        if (!len) {
            buf_printf(b, "\\u%04x", 0xDC00 + *p++);
            continue;
        }
        p += len;
        switch (codepoint) {
        case '"': buf_add_str(b, "\\\""); break;
        case '\\': buf_add_str(b, "\\\\"); break;
        case '\n': buf_add_str(b, "\\n"); break;
        case '\r': buf_add_str(b, "\\r"); break;
        case '\t': buf_add_str(b, "\\t"); break;
        case '\b': buf_add_str(b, "\\b"); break;
        case '\f': buf_add_str(b, "\\f"); break;
        default:
            if (codepoint >= 0x20 && codepoint < 0x7F) {
                buf_add_char(b, (char)codepoint);
            } else if (codepoint < 0x10000) {
                buf_printf(b, "\\u%04x", codepoint);
            } else {
                codepoint -= 0x10000;
                buf_printf(b, "\\u%04x\\u%04x", 0xD800 + (codepoint >> 10),
                           0xDC00 + (codepoint & 0x3FF));
            }
        }
    }
    buf_add_char(b, '"');
}

/* ganze Datei in einen Puffer lesen -> false bei Fehler */
static bool read_file(const char *path, Buf *b)
{
    FILE *f = fopen(path, "rbe");
    if (!f) return false;
    char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, f)) > 0) buf_add(b, tmp, n);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Hash-Cache: Schlüssel -> Hash (Reihenfolge bleibt erhalten wie beim dict)
 * ------------------------------------------------------------------------- */
typedef struct {
    char **keys, **values;
    size_t n, cap;
    size_t *slots;          /* offene Adressierung: Index + 1, 0 = frei */
    size_t slot_count;
} HashCache;

/* FNV-1a-Hash eines Strings (für die Cache-Tabelle) */
static uint64_t string_hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;           /* FNV-1a */
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 1099511628211ULL;
    return h;
}

/* Tabellenplatz für key (offene Adressierung, lineare Suche) */
static size_t *cache_find_slot(const HashCache *c, const char *key)
{
    size_t i = (size_t)string_hash(key) & (c->slot_count - 1);
    while (c->slots[i] && strcmp(c->keys[c->slots[i] - 1], key) != 0)
        i = (i + 1) & (c->slot_count - 1);
    return &c->slots[i];
}

/* Cache-Eintrag zu key (NULL, wenn keiner) */
static const char *cache_get(const HashCache *c, const char *key)
{
    if (!c->slot_count) return NULL;
    size_t s = *cache_find_slot(c, key);
    return s ? c->values[s - 1] : NULL;
}

/* Cache-Eintrag setzen bzw. überschreiben; übernimmt key und val (beide mit malloc
 * angelegt). Ein vorhandener Schlüssel behält seine Position (wie beim dict). */
static void cache_put(HashCache *c, char *key, char *val)
{
    if ((c->n + 1) * 2 > c->slot_count) {           /* Tabelle vergrößern */
        c->slot_count = c->slot_count ? c->slot_count * 2 : 1024;
        free(c->slots);
        c->slots = xmalloc(c->slot_count * sizeof *c->slots);
        memset(c->slots, 0, c->slot_count * sizeof *c->slots);
        for (size_t i = 0; i < c->n; i++) *cache_find_slot(c, c->keys[i]) = i + 1;
    }
    size_t *s = cache_find_slot(c, key);
    if (*s) {
        free(key);
        free(c->values[*s - 1]);
        c->values[*s - 1] = val;
        return;
    }
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 1024;
        c->keys = xrealloc(c->keys, c->cap * sizeof *c->keys);
        c->values = xrealloc(c->values, c->cap * sizeof *c->values);
    }
    c->keys[c->n] = key;
    c->values[c->n] = val;
    *s = ++c->n;
}

/* Cache-Eintrag setzen bzw. überschreiben (Kopien von key und val) */
static void cache_set(HashCache *c, const char *key, const char *val)
{
    cache_put(c, xstrdup(key), xstrdup(val));
}

/* Speicher des Caches freigeben */
static void cache_free(HashCache *c)
{
    for (size_t i = 0; i < c->n; i++) {
        free(c->keys[i]);
        free(c->values[i]);
    }
    free(c->keys);
    free(c->values);
    free(c->slots);
    memset(c, 0, sizeof *c);
}

/* Hash-Cache aus ~/.intro-cutter/hash_cache.json laden (fehlend oder defekt = leer).
 * Das JSON-Objekt wird Eintrag für Eintrag direkt in die Tabelle gelesen – ohne
 * Zwischenbaum, damit auch ein Cache mit Millionen Einträgen (100 000 Videos mit je
 * 9 Vergleichsbildern) nur etwa halb so viel Speicher braucht. */
static void cache_load(HashCache *c)
{
    Buf b = {0};
    if (!read_file(HASH_CACHE, &b) || !b.p) {
        buf_free(&b);
        return;
    }
    JsonParser parser = {b.p, b.p + b.n, 0};
    json_skip_space(&parser);
    bool ok = parser.pos < parser.end && *parser.pos == '{';
    if (ok) parser.pos++;
    json_skip_space(&parser);
    if (ok && parser.pos < parser.end && *parser.pos == '}') {
        parser.pos++;                               /* leeres Objekt */
    } else {
        while (ok) {
            json_skip_space(&parser);
            char *key = json_parse_string(&parser);
            json_skip_space(&parser);
            if (!key || parser.pos >= parser.end || *parser.pos != ':') {
                free(key);
                ok = false;
                break;
            }
            parser.pos++;
            JsonValue *value = json_parse_value(&parser);
            if (!value) {
                free(key);
                ok = false;
                break;
            }
            if (value->type == J_STR) {             /* nur Strings sind Hashes */
                cache_put(c, key, value->str);
                value->str = NULL;
            } else {
                free(key);
            }
            json_free(value);
            json_skip_space(&parser);
            if (parser.pos < parser.end && *parser.pos == ',') {
                parser.pos++;
                continue;
            }
            if (parser.pos < parser.end && *parser.pos == '}') parser.pos++;
            else ok = false;
            break;
        }
    }
    json_skip_space(&parser);
    if (!ok || parser.pos != parser.end) cache_free(c);    /* defekte Datei: leer beginnen */
    buf_free(&b);
}

/* atomar speichern (Temp-Datei + rename), Format wie Pythons json.dump. Geschrieben
 * wird Eintrag für Eintrag, ohne den ganzen Text im Speicher aufzubauen. */
static bool cache_save(const HashCache *c)
{
    char *dir = path_dirname(HASH_CACHE);
    mkdirs(dir);
    free(dir);
    char *tmp = xasprintf("%s.tmp", HASH_CACHE);
    FILE *f = fopen(tmp, "wbe");
    bool ok = f != NULL;
    if (ok) setvbuf(f, NULL, _IOFBF, 1 << 20);
    Buf entry = {0};
    if (ok) ok = fputc('{', f) != EOF;
    for (size_t i = 0; ok && i < c->n; i++) {
        buf_clear(&entry);
        if (i) buf_add_str(&entry, ", ");
        json_write_string(&entry, c->keys[i]);
        buf_add_str(&entry, ": ");
        json_write_string(&entry, c->values[i]);
        ok = fwrite(entry.p, 1, entry.n, f) == entry.n;
    }
    if (ok) ok = fputc('}', f) != EOF;
    if (f && fclose(f) != 0) ok = false;
    ok = ok && rename(tmp, HASH_CACHE) == 0;
    if (!ok) unlink(tmp);
    free(tmp);
    buf_free(&entry);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Temp-Dateien
 * ------------------------------------------------------------------------- */
static StrList g_temp_files;
static pthread_mutex_t g_temp_lock = PTHREAD_MUTEX_INITIALIZER;

/* Temp-Datei merken, damit sie bei einem Abbruch gelöscht wird */
static void temp_register(const char *p)
{
    pthread_mutex_lock(&g_temp_lock);
    if (!strlist_contains(&g_temp_files, p)) strlist_add(&g_temp_files, p);
    pthread_mutex_unlock(&g_temp_lock);
}

/* Temp-Datei wieder austragen (verschoben oder gelöscht) */
static void temp_unregister(const char *p)
{
    if (!p) return;
    pthread_mutex_lock(&g_temp_lock);
    strlist_remove(&g_temp_files, p);
    pthread_mutex_unlock(&g_temp_lock);
}

/* Temp-Pfad NEBEN dem Ziel (gleiches Dateisystem -> Rename statt Kopie) */
static char *temp_path_for(const char *dest)
{
    char *dir = path_dirname(dest);
    char *name = xasprintf(".introcut_tmp_%d_%s", (int)getpid(), path_basename(dest));
    char *p = path_join(dir, name);
    free(dir);
    free(name);
    temp_register(p);
    return p;
}

/* alle gemerkten Temp-Dateien löschen -> Anzahl */
int ic_cleanup_temps(void)
{
    pthread_mutex_lock(&g_temp_lock);
    StrList paths = g_temp_files;
    memset(&g_temp_files, 0, sizeof g_temp_files);
    pthread_mutex_unlock(&g_temp_lock);
    int n = 0;
    for (int i = 0; i < paths.n; i++)
        if (is_file(paths.v[i]) && unlink(paths.v[i]) == 0) n++;
    strlist_free(&paths);
    return n;
}

/* ---------------------------------------------------------------------------
 * Erkennung (seitenverhältnis-robust)
 * ------------------------------------------------------------------------- */
/* DCT-II mit scipy-Skalierung (norm=None), erste n_out Koeffizienten. Rekursive
 * Gerade/Ungerade-Zerlegung wie bei pocketfft: exakt symmetrische Anteile
 * (einfarbige Flächen, gespiegelte Bilder) ergeben exakte Nullen – nur so
 * sind die Hashes auch in diesen Grenzfällen bitgleich zu imagehash.phash */
static void dct_type2(const double *x, int n, double *out, int n_out)
{
    if (n == 1) {
        if (n_out > 0) out[0] = 2.0 * x[0];
        return;
    }
    int half = n / 2, n_even = (n_out + 1) / 2;
    double sums[HASH_PX / 2], diffs[HASH_PX / 2], even_out[HASH_PX / 2];
    for (int i = 0; i < half; i++) {
        sums[i] = x[i] + x[n - 1 - i];
        diffs[i] = x[i] - x[n - 1 - i];
    }
    if (n_even > half) n_even = half;
    dct_type2(sums, half, even_out, n_even);    /* gerade k: DCT der Summen */
    for (int j = 0; j < n_even; j++) out[2 * j] = even_out[j];
    for (int k = 1; k < n_out; k += 2) {        /* ungerade k: aus den Differenzen */
        double acc = 0;
        for (int i = 0; i < half; i++) acc += diffs[i] * cos(M_PI * k * (2 * i + 1) / (2.0 * n));
        out[k] = 2.0 * acc;
    }
}

/* qsort-Vergleich für double */
static int compare_doubles(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

/* imagehash.phash eines 32x32-Graustufenbilds (hash_size 8) */
static uint64_t phash_of_32x32(const unsigned char *pixels)
{
    double column[HASH_PX], column_dct[8][HASH_PX], coeffs[8], low_freq[64], sorted[64];
    for (int j = 0; j < HASH_PX; j++) {          /* dct(pixels, axis=0) */
        for (int i = 0; i < HASH_PX; i++) column[i] = pixels[i * HASH_PX + j];
        dct_type2(column, HASH_PX, coeffs, 8);
        for (int row = 0; row < 8; row++) column_dct[row][j] = coeffs[row];
    }
    for (int row = 0; row < 8; row++) { /* dct(..., axis=1), nur die 8x8 tiefen Frequenzen */
        dct_type2(column_dct[row], HASH_PX, coeffs, 8);
        for (int col = 0; col < 8; col++) low_freq[row * 8 + col] = coeffs[col];
    }
    memcpy(sorted, low_freq, sizeof low_freq);
    qsort(sorted, 64, sizeof *sorted, compare_doubles);
    double median = (sorted[31] + sorted[32]) / 2; /* numpy.median */
    uint64_t hash = 0;
    for (int i = 0; i < 64; i++) hash = hash << 1 | (low_freq[i] > median);
    return hash;
}

/* Container-Dauer (s) -> 0 = unbekannt */
static double probe_duration(const char *path)
{
    StrList cmd = {0};
    strlist_add_many(&cmd, "ffprobe", "-v", "error", "-show_entries", "format=duration",
                     "-of", "csv=p=0", path, NULL);
    Buf out = {0};
    double d = 0;
    if (run_ffmpeg(&cmd, -1, &out, NULL) == 0 && out.n) {
        out.p[strcspn(out.p, "\r\n")] = 0;
        if (!parse_float(out.p, &d) || !(d > 0)) d = 0;
    }
    buf_free(&out);
    strlist_free(&cmd);
    return d;
}

/* Eingabe mit Sprung auf t Sekunden ab Dateistart. TS/PS & Co. ohne Index:
 * der Sprung landet meist nicht auf einem Keyframe (dann kommt evtl. gar kein
 * Frame) – mit Vorlauf springen und den Rest bis zum Frame dekodieren */
static void add_seeking_input(StrList *cmd, const char *video, double t)
{
    char ext[32];
    path_extension(video, ext, sizeof ext);
    if (!str_in_list(ext, SEEK_OUTPUT_EXTS)) {
        strlist_add(cmd, "-ss");
        strlist_add_printf(cmd, "%.6f", t);
        strlist_add_many(cmd, "-i", video, NULL);
        return;
    }
    double preroll_start = t > SEEK_PREROLL ? t - SEEK_PREROLL : 0;
    if (preroll_start > 0) {
        strlist_add(cmd, "-ss");
        strlist_add_printf(cmd, "%.6f", preroll_start);
    }
    strlist_add_many(cmd, "-i", video, "-ss", NULL);
    strlist_add_printf(cmd, "%.6f", t - preroll_start);
}

/* Eingabe mit Sprung zum Vergleichsframe (für Hash und Vorschau).
 * from_end: ts Sekunden vor dem Dateiende (Outro) – Videos, die kürzer sind,
 * gelten wie bei Intros als nicht lesbar. -> false, wenn kein Frame möglich */
static bool add_frame_input(StrList *cmd, const char *video, const char *ts, bool from_end)
{
    char ext[32];
    path_extension(video, ext, sizeof ext);
    if (!from_end) {
        if (str_in_list(ext, DETECT_OUTPUT_SEEK_EXTS)) /* TS/PS: Springen landet oft falsch */
            strlist_add_many(cmd, "-i", video, "-ss", ts, NULL);
        else
            strlist_add_many(cmd, "-ss", ts, "-i", video, NULL);
        return true;
    }
    double dur = probe_duration(video), back, pos;
    if (dur <= 0 || !parse_float(ts, &back) || (pos = dur - back) < 0) return false;
    add_seeking_input(cmd, video, pos);
    return true;
}

/* Liest einen Frame über eine Pipe (ohne Temp-Datei), skaliert ihn
 * proportional in ein 32x32-Graustufenbild (schwarzer Rand bei abweichendem
 * Seitenverhältnis) und berechnet den pHash -> false, wenn nicht lesbar.
 * single_thread: Decoder mit 1 Thread – spart bei vielen parallelen Dateien
 * CPU-Zeit (keine Frame-Thread-Verwaltung pro Prozess).
 * from_end: Zeitpunkt vom Dateiende aus gemessen (Outro-Erkennung). */
static bool hash_frame(const char *video, const char *ts, bool single_thread, bool from_end,
                       uint64_t *hash)
{
    StrList cmd = {0};
    strlist_add_many(&cmd, "ffmpeg", "-nostdin", "-v", "error", NULL);
    /* viele parallele Prozesse: je ein Decoder- und Filter-Thread */
    if (single_thread) strlist_add_many(&cmd, "-threads", "1", "-filter_threads", "1", NULL);
    if (!add_frame_input(&cmd, video, ts, from_end)) {
        strlist_free(&cmd);
        return false;
    }
    strlist_add_many(&cmd, "-frames:v", "1", "-vf", HASH_VF, "-f", "rawvideo", "-", NULL);
    Buf out = {0};
    int rc = run_ffmpeg(&cmd, -1, &out, NULL);
    bool ok = rc == 0 && out.n >= HASH_PX * HASH_PX;
    if (ok) *hash = phash_of_32x32((const unsigned char *)out.p);
    buf_free(&out);
    strlist_free(&cmd);
    return ok;
}

/* Mehrere Vergleichsframes eines Videos in EINEM ffmpeg-Lauf – je Zeitpunkt
 * dieselbe Frame-Auswahl wie hash_frame() (gemeinsamer Cache). need: welche
 * der k Zeitpunkte gelesen werden sollen. Liefert der Sammellauf nicht alle
 * Bilder (z. B. Video kürzer als ein Zeitpunkt), wird einzeln nachgelesen. */
static void hash_frames(const char *video, char ts[][24], const bool *need, int k,
                        bool single_thread, bool from_end, uint64_t *hash, bool *ok)
{
    int wanted[IC_MAX_FRAMES], n_wanted = 0;
    for (int j = 0; j < k; j++)
        if (need[j]) wanted[n_wanted++] = j;
    if (n_wanted == 1) {
        ok[wanted[0]] = hash_frame(video, ts[wanted[0]], single_thread, from_end, &hash[wanted[0]]);
        return;
    }
    if (!n_wanted) return;
    char ext[32];
    path_extension(video, ext, sizeof ext);
    double t[IC_MAX_FRAMES] = {0}, dur = from_end ? probe_duration(video) : 0;
    bool fine = !from_end || dur > 0;
    for (int i = 0; i < n_wanted && fine; i++) {
        double v;
        fine = parse_float(ts[wanted[i]], &v);
        t[i] = from_end ? dur - v : v;
        if (t[i] < 0) fine = false;
    }
    if (fine && !ic_cancel) {
        StrList cmd = {0};
        Buf filtergraph = {0};
        strlist_add_many(&cmd, "ffmpeg", "-nostdin", "-v", "error", NULL);
        if (single_thread) strlist_add_many(&cmd, "-filter_complex_threads", "1", NULL);
        bool from_start = !from_end && str_in_list(ext, DETECT_OUTPUT_SEEK_EXTS);
        double tmin = t[0], tmax = t[0];            /* Outro: Zeitpunkte absteigend */
        for (int i = 1; i < n_wanted; i++) {
            tmin = fmin(tmin, t[i]);
            tmax = fmax(tmax, t[i]);
        }
        if (from_start || (tmax - tmin <= SEQ_SPAN && str_in_list(ext, SEQ_EXTS))) {
            /* ein Eingang, in einem Durchgang dekodiert und aufgeteilt. TS/PS beim Intro:
             * von vorn (wie "-i video -ss ts"); sonst Sprung zum ersten Bild – bei TS & Co.
             * mit Vorlauf wie add_seeking_input() */
            double base = 0;
            if (!from_start)
                base = from_end && str_in_list(ext, SEEK_OUTPUT_EXTS)
                           ? (tmin > SEEK_PREROLL ? tmin - SEEK_PREROLL : 0) : tmin;
            if (single_thread) strlist_add_many(&cmd, "-threads", "1", NULL);
            if (base > 0) {
                strlist_add(&cmd, "-ss");
                strlist_add_printf(&cmd, "%.6f", base);
            }
            strlist_add_many(&cmd, "-i", video, NULL);
            buf_printf(&filtergraph, "[0:V:0]split=%d", n_wanted);
            for (int i = 0; i < n_wanted; i++) buf_printf(&filtergraph, "[s%d]", i);
            buf_add_str(&filtergraph, ";");
            for (int i = 0; i < n_wanted; i++)
                buf_printf(&filtergraph,
                           "[s%d]trim=start=%.6f,trim=end_frame=1,setpts=PTS-STARTPTS,%s[b%d];", i,
                           t[i] - base, HASH_VF, i);
        } else {
            for (int i = 0; i < n_wanted; i++) {    /* je Zeitpunkt ein Eingang mit Sprung */
                if (single_thread) strlist_add_many(&cmd, "-threads", "1", NULL);
                double rest = -1;
                if (from_end && str_in_list(ext, SEEK_OUTPUT_EXTS)) { /* wie add_seeking_input() */
                    double preroll_start = t[i] > SEEK_PREROLL ? t[i] - SEEK_PREROLL : 0;
                    if (preroll_start > 0) {
                        strlist_add(&cmd, "-ss");
                        strlist_add_printf(&cmd, "%.6f", preroll_start);
                    }
                    rest = t[i] - preroll_start;
                } else {
                    strlist_add(&cmd, "-ss");
                    strlist_add_printf(&cmd, "%.6f", t[i]);
                }
                strlist_add_many(&cmd, "-i", video, NULL);
                buf_printf(&filtergraph, "[%d:V:0]", i);
                if (rest >= 0) buf_printf(&filtergraph, "trim=start=%.6f,", rest);
                buf_printf(&filtergraph, "trim=end_frame=1,setpts=PTS-STARTPTS,%s[b%d];",
                           HASH_VF, i);
            }
        }
        for (int i = 0; i < n_wanted; i++) buf_printf(&filtergraph, "[b%d]", i);
        buf_printf(&filtergraph, "concat=n=%d:v=1:a=0[out]", n_wanted);
        strlist_add_many(&cmd, "-filter_complex", buf_str(&filtergraph), "-map", "[out]",
                         "-fps_mode", "passthrough", "-f", "rawvideo", "-", NULL);
        Buf out = {0};
        int rc = run_ffmpeg(&cmd, -1, &out, NULL);
        size_t frame_bytes = HASH_PX * HASH_PX;
        bool got_all = rc == 0 && out.n == frame_bytes * (size_t)n_wanted;
        if (got_all)
            for (int i = 0; i < n_wanted; i++) {
                hash[wanted[i]] =
                    phash_of_32x32((const unsigned char *)out.p + frame_bytes * (size_t)i);
                ok[wanted[i]] = true;
            }
        buf_free(&out);
        buf_free(&filtergraph);
        strlist_free(&cmd);
        if (got_all) return;
    }
    for (int i = 0; i < n_wanted && !ic_cancel; i++) /* einzeln nachlesen */
        ok[wanted[i]] = hash_frame(video, ts[wanted[i]], single_thread, from_end, &hash[wanted[i]]);
}

/* Distanz eines Videos: Median über alle Vergleichsbilder (nicht lesbare zählen
 * mit 64) -> false (gilt als nicht lesbar), wenn nicht mehr als die Hälfte
 * der Bilder lesbar war – z. B. Video kürzer als das Intro */
static bool video_distance(const uint64_t *ref_hash, const uint64_t *h, const bool *ok, int k,
                           Result *r)
{
    int dist[IC_MAX_FRAMES] = {0}, sorted[IC_MAX_FRAMES] = {0}, n_readable = 0;
    for (int j = 0; j < k; j++) {
        dist[j] = ok[j] ? __builtin_popcountll(ref_hash[j] ^ h[j]) : 64;
        n_readable += ok[j];
    }
    if (n_readable * 2 <= k) return false;
    for (int j = 0; j < k; j++) sorted[j] = dist[j];
    for (int i = 1; i < k; i++)
        for (int j = i; j > 0 && sorted[j] < sorted[j - 1]; j--) {
            int x = sorted[j];
            sorted[j] = sorted[j - 1];
            sorted[j - 1] = x;
        }
    r->dist = sorted[k / 2];
    r->nd = k;
    for (int j = 0; j < k; j++) r->d[j] = ok[j] ? (unsigned char)dist[j] : 255;
    return true;
}

/* Cache-Schlüssel wie in der Python-Version: realpath|Größe|mtime|Zeitpunkt|Pipeline.
 * Outro-Hashes (vom Dateiende) bekommen die Pipeline-Endung "+eof".
 * Hier der Teil, der für alle Zeitpunkte eines Videos gleich ist: prefix bekommt
 * "realpath|Größe|mtime|" (freigeben!), suffix die Endung nach der Pipeline-Version.
 * stat() und realpath() laufen so nur einmal je Video – bei 100 000 Videos auf
 * einem NAS spart das viele Netzwerkzugriffe. -> false, wenn das Video fehlt */
static bool cache_key_parts(const char *video, bool from_end, char **prefix, const char **suffix)
{
    struct stat st;
    if (stat(video, &st) != 0) return false;
    char *real = realpath_lenient(video);
    /* st_mtime wie Python: (double)sec + nsec * 1e-9, dann int() */
    double mtime = (double)st.st_mtim.tv_sec + (double)st.st_mtim.tv_nsec * 1e-9;
    char ext[32];
    path_extension(video, ext, sizeof ext);
    /* +os: neue Frame-Auswahl -> alte (evtl. falsche) Hashes verwerfen */
    *suffix = from_end ? "+eof" : str_in_list(ext, DETECT_OUTPUT_SEEK_EXTS) ? "+os" : "";
    *prefix = xasprintf("%s|%lld|%lld|", real, (long long)st.st_size, (long long)mtime);
    free(real);
    return true;
}

/* vollständiger Cache-Schlüssel zum Zeitpunkt ts (Teile aus cache_key_parts) */
static char *cache_key_at(const char *prefix, const char *ts, const char *suffix)
{
    return xasprintf("%s%s|%s%s", prefix, ts, PIPELINE, suffix);
}

/* Ordner rekursiv nach Videos durchsuchen (wie os.walk, Ordner-Symlinks werden nicht verfolgt) */
static void scan_dir(const char *dir, const char *rel, const char *exclude, VideoList *out)
{
    if (ic_cancel) return;
    char *real = realpath_lenient(dir);
    size_t exclude_len = strlen(exclude);
    bool skip = !strcmp(real, exclude) ||
                (!strncmp(real, exclude, exclude_len) && real[exclude_len] == '/');
    free(real);
    if (skip) return;
    DIR *d = opendir(dir);
    if (!d) return;
    StrList subdirs = {0};
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *name = e->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        char *full = path_join(dir, name);
        bool isdir = e->d_type == DT_DIR, islink = false;
        if (e->d_type == DT_LNK || e->d_type == DT_UNKNOWN) {
            struct stat st;
            if (lstat(full, &st) == 0) islink = S_ISLNK(st.st_mode);
            isdir = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (isdir) {
            /* wie os.walk: Ordner-Symlinks nicht folgen */
            if (!islink) strlist_add(&subdirs, name);
        } else if (!starts_with(name, ".introcut_")) {     /* eigene Temp-Dateien */
            char ext[32];
            path_extension(name, ext, sizeof ext);
            if (str_in_list(ext, VIDEO_EXTS)) {
                if (out->n == out->cap) {
                    out->cap = out->cap ? out->cap * 2 : 256;
                    out->v = xrealloc(out->v, (size_t)out->cap * sizeof *out->v);
                }
                out->v[out->n].path = full;
                out->v[out->n].cut_len = 0;
                out->v[out->n++].rel = rel[0] ? path_join(rel, name) : xstrdup(name);
                full = NULL;
            }
        }
        free(full);
    }
    closedir(d);
    for (int i = 0; i < subdirs.n; i++) {
        char *subdir_path = path_join(dir, subdirs.v[i]);
        char *subdir_rel = rel[0] ? path_join(rel, subdirs.v[i]) : xstrdup(subdirs.v[i]);
        scan_dir(subdir_path, subdir_rel, exclude, out);
        free(subdir_path);
        free(subdir_rel);
    }
    strlist_free(&subdirs);
}

/* qsort-Vergleich: Videos nach relativem Pfad */
static int compare_videos(const void *a, const void *b)
{
    return strcmp(((const Video *)a)->path, ((const Video *)b)->path);
}

/* alle Videos unter src finden (ohne den Ausgabeordner), sortiert nach Pfad */
static void find_videos(const char *src, const char *exclude_dir, VideoList *out)
{
    char *exclude = realpath_lenient(exclude_dir);
    scan_dir(src, "", exclude, out);
    free(exclude);
    if (out->n) qsort(out->v, (size_t)out->n, sizeof *out->v, compare_videos);
}

/* ---------------------------------------------------------------------------
 * Analyse
 * ------------------------------------------------------------------------- */
typedef struct {
    int index;
    char *codec, *filename, *mimetype;      /* NULL = nicht vorhanden */
} CoverPic;

typedef struct {
    int index;                              /* -1 = fehlt */
    char *type;                             /* codec_type, NULL = fehlt */
} StreamEntry;

typedef struct {
    double start;                           /* Startzeit des Containers */
    double duration;                        /* 0 = unbekannt */
    StreamEntry *streams;
    int stream_count;
    int main_video, first_audio;            /* Stream-Index, -1 = keiner */
    CoverPic *covers;                       /* Cover (attached_pic) */
    int cover_count;
    int attachment_count;
    char *video_codec;
    bool is_10bit;
    int width, height;                      /* 0 = unbekannt */
} MediaInfo;

/* MediaInfo leeren (Stream-Indizes -1 = keiner) */
static void media_info_init(MediaInfo *info)
{
    memset(info, 0, sizeof *info);
    info->main_video = info->first_audio = -1;
}

/* Speicher einer MediaInfo freigeben */
static void media_info_free(MediaInfo *info)
{
    for (int i = 0; i < info->stream_count; i++) free(info->streams[i].type);
    for (int i = 0; i < info->cover_count; i++) {
        free(info->covers[i].codec);
        free(info->covers[i].filename);
        free(info->covers[i].mimetype);
    }
    free(info->streams);
    free(info->covers);
    free(info->video_codec);
    media_info_init(info);
}

/* Kopie von s oder NULL */
static char *strdup_or_null(const char *s)
{
    return s ? xstrdup(s) : NULL;
}

/* nächste Zeile aus einem Puffer (wird dabei zerlegt); NULL am Ende */
static char *next_line(char **cur)
{
    char *s = *cur;
    if (!s || !*s) return NULL;
    char *e = s + strcspn(s, "\r\n");
    if (*e == '\r' && e[1] == '\n') *e++ = 0;
    if (*e) *e++ = 0;
    *cur = e;
    return s;
}

/* line.split(",") -> Anzahl Felder (höchstens max werden gespeichert) */
static int split_csv_line(char *line, char **f, int max)
{
    int n = 0;
    while (true) {
        char *c = strchr(line, ',');
        if (c) *c = 0;
        if (n < max) f[n] = line;
        n++;
        if (!c) return n;
        line = c + 1;
    }
}

/* Container- und Stream-Infos (eine ffprobe-Abfrage) */
static bool probe_media(const char *path, MediaInfo *info)
{
    media_info_init(info);
    StrList cmd = {0};
    strlist_add_many(&cmd, "ffprobe", "-v", "error", "-print_format", "json", "-show_entries",
                     "format=start_time,duration:"
                     "stream=index,codec_type,codec_name,pix_fmt,bits_per_raw_sample,width,height:"
                     "stream_disposition=attached_pic:stream_tags=filename,mimetype",
                     path, NULL);
    Buf out = {0};
    int rc = run_ffmpeg(&cmd, -1, &out, NULL);
    strlist_free(&cmd);
    JsonValue *d = rc == 0 ? json_parse(out.n ? out.p : "{}", out.n ? out.n : 2) : NULL;
    buf_free(&out);
    if (!d || d->type != J_OBJ) {
        json_free(d);
        return false;
    }
    JsonValue *fmt = json_get(d, "format"), *streams = json_get(d, "streams");
    double v;
    if (json_number_of(json_get(fmt, "start_time"), &v)) info->start = v;
    if (json_number_of(json_get(fmt, "duration"), &v)) info->duration = v;
    if (streams && streams->type == J_ARR) {
        info->streams = xmalloc((size_t)streams->n * sizeof *info->streams);
        for (int i = 0; i < streams->n; i++) {
            JsonValue *s = streams->items[i], *disp = json_get(s, "disposition");
            StreamEntry *r = &info->streams[info->stream_count++];
            r->index = json_number_of(json_get(s, "index"), &v) ? (int)v : -1;
            r->type = strdup_or_null(json_string_of(json_get(s, "codec_type")));
            const char *t = r->type ? r->type : "";
            if (!strcmp(t, "video")) {
                if (json_truthy(json_get(disp, "attached_pic"))) {
                    JsonValue *tags = json_get(s, "tags");
                    info->covers = xrealloc(info->covers,
                                            (size_t)(info->cover_count + 1) * sizeof *info->covers);
                    CoverPic *p = &info->covers[info->cover_count++];
                    p->index = r->index;
                    p->codec = strdup_or_null(json_string_of(json_get(s, "codec_name")));
                    p->filename = strdup_or_null(json_string_of(json_get(tags, "filename")));
                    p->mimetype = strdup_or_null(json_string_of(json_get(tags, "mimetype")));
                } else if (info->main_video < 0) {
                    info->main_video = r->index;
                    info->video_codec = strdup_or_null(json_string_of(json_get(s, "codec_name")));
                    double w = 0, h = 0, bits = 0;
                    if (json_number_of(json_get(s, "width"), &w) &&
                        json_number_of(json_get(s, "height"), &h) && w && h) {
                        info->width = (int)w;
                        info->height = (int)h;
                    }
                    const char *pix_fmt = json_string_of(json_get(s, "pix_fmt"));
                    if (!pix_fmt) pix_fmt = "";
                    json_number_of(json_get(s, "bits_per_raw_sample"), &bits);
                    info->is_10bit = strstr(pix_fmt, "p010") || strstr(pix_fmt, "p012") ||
                                     strstr(pix_fmt, "p016") || strstr(pix_fmt, "p10") ||
                                     strstr(pix_fmt, "p12") || strstr(pix_fmt, "p14") ||
                                     strstr(pix_fmt, "p16") || bits >= 10;
                }
            } else if (!strcmp(t, "audio") && info->first_audio < 0) {
                info->first_audio = r->index;
            } else if (!strcmp(t, "attachment")) {
                info->attachment_count++;
            }
        }
    }
    json_free(d);
    return true;
}

typedef struct {
    int idx;
    double t;
} StreamTime;

typedef struct {
    StreamTime *v;
    int n;
} StreamTimes;

/* Zeitpunkt für den Stream stream_index -> false, wenn keiner bekannt ist */
static bool stream_time_of(const StreamTimes *times, int stream_index, double *t)
{
    for (int i = 0; i < times->n; i++)
        if (times->v[i].idx == stream_index) {
            *t = times->v[i].t;
            return true;
        }
    return false;
}

/* Erster Zeitstempel je Stream (aus den ersten Sekunden der Datei) */
static StreamTimes first_stream_times(const char *path)
{
    StreamTimes times = {0};
    StrList cmd = {0};
    strlist_add_many(&cmd, "ffprobe", "-v", "error",
                     "-show_entries", "packet=stream_index,pts_time,dts_time",
                     "-of", "csv=p=0", "-read_intervals", "%+4", path, NULL);
    Buf out = {0};
    run_ffmpeg(&cmd, -1, &out, NULL);
    strlist_free(&cmd);
    char *cursor = out.p, *line, *fields[3];
    while ((line = next_line(&cursor))) {
        long stream_index;
        double time, known_time;
        if (split_csv_line(line, fields, 3) < 3 || !parse_int(fields[0], &stream_index)) continue;
        if (!parse_float(fields[1], &time) && !parse_float(fields[2], &time)) continue;
        if (!stream_time_of(&times, (int)stream_index, &known_time)) {
            times.v = xrealloc(times.v, (size_t)(times.n + 1) * sizeof *times.v);
            times.v[times.n++] = (StreamTime){(int)stream_index, time};
        } else if (time < known_time) {
            for (int i = 0; i < times.n; i++)
                if (times.v[i].idx == (int)stream_index) times.v[i].t = time;
        }
    }
    buf_free(&out);
    return times;
}

/* Ende eines Streams (letztes Paket: pts + Dauer) relativ zum Dateistart –
 * liest nur die letzten Sekunden der Datei -> 0 = unbekannt */
static double stream_end_time(const char *path, const MediaInfo *info, int stream_index)
{
    if (stream_index < 0 || info->duration <= 0) return 0;
    StrList cmd = {0};
    strlist_add_many(&cmd, "ffprobe", "-v", "error", "-select_streams", NULL);
    strlist_add_printf(&cmd, "%d", stream_index);
    strlist_add_many(&cmd, "-show_entries", "packet=pts_time,dts_time,duration_time", "-of",
                     "csv=p=0", "-read_intervals", NULL);
    double from = info->start + info->duration - 30;
    strlist_add_printf(&cmd, "%.3f%%", from > 0 ? from : 0);
    strlist_add(&cmd, path);
    Buf out = {0};
    run_ffmpeg(&cmd, -1, &out, NULL);
    strlist_free(&cmd);
    double end = 0;
    char *cursor = out.p, *line, *f[3];
    while ((line = next_line(&cursor))) {
        double t, d;
        if (split_csv_line(line, f, 3) < 3) continue;
        if (!parse_float(f[0], &t) && !parse_float(f[1], &t)) continue;
        if (!parse_float(f[2], &d) || d < 0) d = 0;
        if (t + d - info->start > end) end = t + d - info->start;
    }
    buf_free(&out);
    return end;
}

/* Outro: Untertitel dürfen über den Schnitt hinausragen (die Container-Dauer
 * wächst dann mit) – maßgeblich ist, wo das Bild endet -> Abweichung in s
 * (> 0: zu kurz), ohne Messwert bleibt es bei diff */
static double outro_length_error(const MediaInfo *src, const char *src_path,
                                 const MediaInfo *out_info, const char *out, double removed,
                                 double diff)
{
    double keep = src->duration - removed,
           video_end = stream_end_time(out, out_info, out_info->main_video);
    if (video_end <= 0) return diff;
    double d = keep - video_end;
    if (d > VALIDATE_TOL) {                         /* endet das Bild schon im Original früher? */
        double src_video_end = stream_end_time(src_path, src, src->main_video);
        if (src_video_end > 0 && src_video_end < keep) d = src_video_end - video_end;
    }
    return d;
}

/* Beginnen Ton und Bild der Ausgabe gemeinsam (höchstens 0,5 s versetzt)? Beim Outro bleibt
 * der Anfang, wie er ist – ein Versatz, den schon das Original hat, zählt dort nicht. */
static bool starts_together(double video_start, double audio_start, bool outro,
                            const MediaInfo *src, const char *src_path)
{
    double offset = fabs(video_start - audio_start);
    if (offset <= 0.5) return true;
    if (!outro || src->main_video < 0 || src->first_audio < 0) return false;
    StreamTimes src_times = first_stream_times(src_path);
    double src_video, src_audio;
    bool same_as_source = stream_time_of(&src_times, src->main_video, &src_video) &&
                          stream_time_of(&src_times, src->first_audio, &src_audio) &&
                          fabs(offset - fabs(src_video - src_audio)) <= 0.5;
    free(src_times.v);
    return same_as_source;
}

/* Prüft das Schnittergebnis -> ok, sonst Grund in why.
 * outro: removed wurde am Ende abgeschnitten (Bildende statt Container-Dauer prüfen). */
static bool validate_cut(const MediaInfo *src, const char *src_path, const char *out,
                         double removed, bool outro, char *reason, size_t reason_size)
{
    MediaInfo out_info;
    bool ok = false;
    reason[0] = 0;
    if (!probe_media(out, &out_info) || out_info.main_video < 0 || !out_info.duration) {
        snprintf(reason, reason_size, "Ergebnis nicht lesbar");
        media_info_free(&out_info);
        return false;
    }
    double diff = (src->duration - out_info.duration) - removed, video_start, audio_start;
    if (outro && src->duration && fabs(diff) > VALIDATE_TOL)
        diff = outro_length_error(src, src_path, &out_info, out, removed, diff);
    StreamTimes first_times = {0};
    if (src->duration && fabs(diff) > VALIDATE_TOL) {
        snprintf(reason, reason_size, "Dauer weicht um %+.1f s ab", diff);
    } else if (out_info.start > 2.5) {
        snprintf(reason, reason_size, "Zeitachse beginnt erst bei %.1f s", out_info.start);
    } else if (src->width && out_info.width &&
               (src->width != out_info.width || src->height != out_info.height)) {
        snprintf(reason, reason_size, "Auflösung %dx%d statt %dx%d", out_info.width,
                 out_info.height, src->width, src->height);
    } else if (first_times = first_stream_times(out),
               !stream_time_of(&first_times, out_info.main_video, &video_start)) {
        snprintf(reason, reason_size, "Video beginnt nicht am Anfang");
    } else if (out_info.first_audio >= 0 &&
               (!stream_time_of(&first_times, out_info.first_audio, &audio_start) ||
                !starts_together(video_start, audio_start, outro, src, src_path))) {
        snprintf(reason, reason_size, "Ton und Bild beginnen versetzt");
    } else {
        ok = true;
    }
    free(first_times.v);
    media_info_free(&out_info);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Schnitt: Befehle
 * ------------------------------------------------------------------------- */
/* Letzter Keyframe des Hauptvideos, an dem Copy schneiden darf, ohne Inhalt mitzunehmen: im
 * Intro oder genau auf dem ersten Bild danach (dort setzen Encoder oft einen, weil die Szene
 * wechselt – dann ist auch Copy bildgenau). Für automatisch erkannte Intros; es bleibt
 * höchstens ein Rest des Intros. Liest die Pakete vom Dateistart bis 1 s nach dem Schnitt.
 * -> true + (pts_rel, dts_rel) relativ zum Dateistart; *seek_pad: so weit darf der Sprung
 * hinter den Keyframe zielen, ohne den nächsten zu erreichen */
static bool find_keyframe_before(const char *path, const MediaInfo *info, double cut_len,
                                 double *pts_rel, double *dts_rel, double *seek_pad)
{
    double start = info->start, target = start + cut_len;
    StrList cmd = {0};
    strlist_add_many(&cmd, "ffprobe", "-v", "error", "-select_streams", NULL);
    strlist_add_printf(&cmd, "%d", info->main_video);
    strlist_add_many(&cmd, "-show_entries", "packet=pts_time,dts_time,flags", "-of", "csv=p=0",
                     "-read_intervals", NULL);
    strlist_add_printf(&cmd, "%.3f%%%.3f", start, target + 1);   /* absolutes Ende */
    strlist_add(&cmd, path);
    Buf out = {0};
    run_ffmpeg(&cmd, -1, &out, NULL);
    strlist_free(&cmd);
    /* erstes Bild des Videos, erstes Bild nach dem Schnitt, Keyframes (pts, dts) */
    double first = INFINITY, first_kept = INFINITY;
    double (*keyframes)[2] = NULL;
    int nkeyframes = 0, keyframes_cap = 0;
    char *cursor = out.p, *line, *f[3];
    while ((line = next_line(&cursor))) {
        if (split_csv_line(line, f, 3) < 3) continue;
        double pts, dts;
        bool have_pts = parse_float(f[0], &pts), have_dts = parse_float(f[1], &dts);
        if (!have_pts) {
            pts = dts;
            have_pts = have_dts;
        }
        if (!have_pts) continue;
        first = fmin(first, pts);
        if (pts >= target - 1e-4) first_kept = fmin(first_kept, pts);
        if (!strchr(f[2], 'K')) continue;
        if (nkeyframes == keyframes_cap) {
            keyframes_cap = keyframes_cap ? 2 * keyframes_cap : 64;
            keyframes = xrealloc(keyframes, (size_t)keyframes_cap * sizeof *keyframes);
        }
        keyframes[nkeyframes][0] = pts;
        keyframes[nkeyframes++][1] = have_dts ? dts : pts;
    }
    buf_free(&out);
    int best = -1;
    for (int i = 0; i < nkeyframes; i++)        /* spätester, der keinen Inhalt mitnimmt */
        if (keyframes[i][0] <= first_kept + 1e-6 &&
            (best < 0 || keyframes[i][0] > keyframes[best][0]))
            best = i;
    double next = INFINITY;                     /* nächster Keyframe danach */
    for (int i = 0; best >= 0 && i < nkeyframes; i++)
        if (keyframes[i][0] > keyframes[best][0] + 1e-6) next = fmin(next, keyframes[i][0]);
    bool ok = isfinite(first_kept) && best >= 0 && keyframes[best][0] > first + 0.001;
    if (ok) {                                   /* nicht nur der Anfang des Videos */
        *pts_rel = keyframes[best][0] - start;
        *dts_rel = keyframes[best][1] - start;
        *seek_pad = isfinite(next) ? fmin(0.25, (next - keyframes[best][0]) / 2) : 0.25;
    }
    free(keyframes);
    return ok;
}

/* Erster Keyframe des Hauptvideos bei/nach dem Intro-Ende. Liest erst ab
 * kurz vor dem Intro-Ende (nicht ab Dateianfang).
 * -> true + (pts_rel, dts_rel) relativ zum Dateistart */
static bool find_cut_keyframe(const char *path, const MediaInfo *info, double cut_len,
                              const char *ext, double *pts_rel, double *dts_rel)
{
    double start = info->start, target = start + cut_len;
    double margin = str_in_list(ext, SEEK_OUTPUT_EXTS) ? 3.0 : 1.0;
    double from_abs = fmax(start, target - margin);     /* read_intervals arbeitet absolut */
    for (int pass = 0; pass < 2; pass++) {
        StrList cmd = {0};
        strlist_add_many(&cmd, "ffprobe", "-v", "error", "-select_streams", NULL);
        strlist_add_printf(&cmd, "%d", info->main_video);
        strlist_add_many(&cmd, "-show_entries", "packet=pts_time,dts_time,flags", "-of", "csv=p=0",
                         "-read_intervals", NULL);
        strlist_add_printf(&cmd, pass == 0 ? "%.3f%%+40" : "%.3f%%", from_abs);
        strlist_add(&cmd, path);
        Buf out = {0};
        run_ffmpeg(&cmd, -1, &out, NULL);
        strlist_free(&cmd);
        bool found = false;
        double best_pts = 0, best_dts = 0;
        char *cursor = out.p, *line, *f[3];
        while ((line = next_line(&cursor))) {
            if (split_csv_line(line, f, 3) < 3 || !strchr(f[2], 'K')) continue;
            double pts, dts;
            bool have_pts = parse_float(f[0], &pts), have_dts = parse_float(f[1], &dts);
            if (!have_pts) {
                pts = dts;
                have_pts = have_dts;
            }
            if (!have_pts || pts < target - 0.001) continue;
            if (!found || pts < best_pts) {
                found = true;
                best_pts = pts;
                best_dts = have_dts ? dts : pts;
            }
        }
        buf_free(&out);
        if (found) {
            *pts_rel = best_pts - start;
            *dts_rel = best_dts - start;
            return true;
        }
    }
    return false;
}

typedef struct {
    StrList cover_input; /* zweite Eingabe (Cover bei MP4) */
    StrList map_args;
    StrList attach_args; /* Cover als MKV-Anhang */
    StrList temp_files; /* extrahierte Cover-Bilder */
    StrList notes;
    int video_out_index; /* Ausgabe-Index des Hauptvideos */
} StreamPlan;

/* Speicher eines Stream-Plans freigeben */
static void stream_plan_free(StreamPlan *st)
{
    strlist_free(&st->cover_input);
    strlist_free(&st->map_args);
    strlist_free(&st->attach_args);
    strlist_free(&st->temp_files);
    strlist_free(&st->notes);
}

/* Dateiendung und MIME-Typ zu einem Cover-Codec ({codec, endung, mime}), NULL wenn unbekannt */
static const char *const *cover_image_format(const char *codec)
{
    for (size_t i = 0; i < sizeof IMG_EXT / sizeof *IMG_EXT; i++)
        if (codec && !strcmp(codec, IMG_EXT[i][0])) return IMG_EXT[i];
    return NULL;
}

/* Stream-Zuordnung: alle Spuren übernehmen. Cover-Bilder werden gesondert
 * behandelt, da sie sonst die Zeitachse verschieben bzw. (MKV) als
 * Videospur statt als Anhang geschrieben würden. */
static void plan_streams(const char *src, const MediaInfo *info, const char *ext,
                         const char *temp, StreamPlan *plan)
{
    memset(plan, 0, sizeof *plan);
    strlist_add_many(&plan->map_args, "-map", "0", NULL);
    for (int i = 0; i < info->cover_count; i++) {
        strlist_add(&plan->map_args, "-map");
        strlist_add_printf(&plan->map_args, "-0:%d", info->covers[i].index);
    }
    /* Ausgabe-Index des Hauptvideos (Reihenfolge wie Eingabe, ohne Daten/Cover) */
    for (int i = 0; i < info->stream_count; i++) {
        const StreamEntry *s = &info->streams[i];
        if (s->index == info->main_video) break;
        bool is_cover = false;
        for (int j = 0; j < info->cover_count; j++) is_cover |= info->covers[j].index == s->index;
        if (!s->type || !strcmp(s->type, "data") || !strcmp(s->type, "unknown") || is_cover)
            continue;
        plan->video_out_index++;
    }
    if (!info->cover_count) return;
    if (str_in_list(ext, MP4_LIKE)) {
        /* Cover aus zweiter Eingabe ohne Sprung (liest nur 0,1 s) */
        strlist_add_many(&plan->cover_input, "-t", "0.1", "-i", src, NULL);
        for (int i = 0; i < info->cover_count; i++) {
            strlist_add(&plan->map_args, "-map");
            strlist_add_printf(&plan->map_args, "1:%d", info->covers[i].index);
        }
    } else if (!strcmp(ext, ".mkv")) {
        int k = info->attachment_count;
        for (int j = 0; j < info->cover_count; j++) {
            const CoverPic *p = &info->covers[j];
            const char *const *fmt = cover_image_format(p->codec);
            char *img = NULL;
            bool ok = false;
            if (fmt) {
                img = xasprintf("%s.cover%d.%s", temp, j, fmt[1]);
                temp_register(img);
                strlist_add(&plan->temp_files, img);
                StrList cmd = {0};
                strlist_add_many(&cmd, "ffmpeg", "-nostdin", "-y", "-v", "error", "-i", src,
                                 "-map", NULL);
                strlist_add_printf(&cmd, "0:%d", p->index);
                strlist_add_many(&cmd, "-c", "copy", "-frames:v", "1", "-f", "image2", img, NULL);
                ok = run_ffmpeg(&cmd, -1, NULL, NULL) == 0 && file_not_empty(img);
                strlist_free(&cmd);
            }
            if (!ok) {
                strlist_add(&plan->notes, "Cover-Bild entfernt");
                free(img);
                continue;
            }
            strlist_add_many(&plan->attach_args, "-attach", img, NULL);
            strlist_add_printf(&plan->attach_args, "-metadata:s:t:%d", k);
            strlist_add_printf(&plan->attach_args, "mimetype=%s",
                               p->mimetype && *p->mimetype ? p->mimetype : fmt[2]);
            strlist_add_printf(&plan->attach_args, "-metadata:s:t:%d", k);
            if (p->filename && *p->filename)
                strlist_add_printf(&plan->attach_args, "filename=%s", p->filename);
            else
                strlist_add_printf(&plan->attach_args, "filename=cover.%s", fmt[1]);
            k++;
            free(img);
        }
    } else {
        strlist_add(&plan->notes, "Cover-Bild entfernt");
    }
}

/* ffmpeg-Befehl: Intro per Stream-Copy abschneiden, Schnitt am Keyframe kf_rel (s ab Dateistart);
 * seek_pad: so weit hinter den Keyframe springen (sonst 0,25 s, s. u.) */
static void build_copy_cmd(StrList *cmd, const char *src, const char *target, const StreamPlan *st,
                     double kf_rel, double kf_dts_rel, double seek_pad, const char *ext)
{
    bool seek_out = str_in_list(ext, SEEK_OUTPUT_EXTS);
    strlist_add_many(cmd, "ffmpeg", "-nostdin", "-y", "-v", "error", NULL);
    if (seek_out) {
        /* ohne Index: von vorn lesen, alles vor dem Keyframe verwerfen */
        strlist_add_many(cmd, "-i", src, NULL);
    } else {
        /* +0,25 s: ffmpeg zieht bei B-Frames intern ~0,13 s ab und würde sonst
         * auf dem VORHERIGEN Keyframe landen (Intro-Rest) */
        strlist_add(cmd, "-ss");
        strlist_add_printf(cmd, "%.3f", kf_rel + seek_pad);
        strlist_add_many(cmd, "-i", src, NULL);
        strlist_add_list(cmd, &st->cover_input);
    }
    strlist_add_list(cmd, &st->map_args);
    strlist_add_many(cmd, "-dn", "-ignore_unknown", "-c", "copy", NULL);
    if (seek_out) {
        strlist_add(cmd, "-ss");
        strlist_add_printf(cmd, "%.3f", fmax(0.0, kf_dts_rel - 0.001));
    } else {
        strlist_add_many(cmd, "-copypriorss:s", "0", NULL);
    }
    strlist_add_many(cmd, "-avoid_negative_ts", "make_zero", "-max_muxing_queue_size",
                     "4096", NULL);
    strlist_add_list(cmd, &st->attach_args);
    strlist_add(cmd, target);
}

typedef struct {
    char name[32];          /* Encoder, z. B. "hevc_vaapi" */
    char label[48];         /* z. B. "GPU HEVC 10 Bit" */
    StrList args;
    char filter[96];        /* "" = keiner */
    char hw_filter[128];    /* bei HW-Dekodierung statt filter ("" = filter nehmen) */
    char tag[8];            /* "" = keiner */
    bool hw, hwdecode;
    bool low_prio;          /* Modus 4: CPU-Encode mit nice 19 */
    char device[PATH_MAX];
} Encoder;

/* Outro ohne Neukodierung: alles bis keep Sekunden übernehmen. Am Ende braucht
 * es keinen Keyframe – der Schnitt ist bis auf wenige Frames genau. */
static void build_outro_copy_cmd(StrList *cmd, const char *src, const char *target,
                                 const StreamPlan *st, double keep)
{
    strlist_add_many(cmd, "ffmpeg", "-nostdin", "-y", "-v", "error", "-i", src, NULL);
    strlist_add_list(cmd, &st->cover_input);
    strlist_add_list(cmd, &st->map_args);
    strlist_add_many(cmd, "-dn", "-ignore_unknown", "-c", "copy", "-t", NULL);
    strlist_add_printf(cmd, "%.3f", keep);
    strlist_add_many(cmd, "-avoid_negative_ts", "make_zero", "-max_muxing_queue_size",
                     "4096", NULL);
    strlist_add_list(cmd, &st->attach_args);
    strlist_add(cmd, target);
}

/* keep > 0: Outro-Modus – von vorn lesen und nach keep Sekunden aufhören */
static void build_encode_cmd(StrList *cmd, const char *src, const char *target,
                             const StreamPlan *st, double cut_len, double keep, const char *ext,
                             const Encoder *venc)
{
    bool seek_out = str_in_list(ext, SEEK_OUTPUT_EXTS);
    strlist_add_many(cmd, "ffmpeg", "-nostdin", "-y", "-v", "error", NULL);
    if (venc->hw) {
        strlist_add(cmd, "-init_hw_device");
        strlist_add_printf(cmd, "vaapi=va:%s", venc->device);
        strlist_add_many(cmd, "-filter_hw_device", "va", NULL);
        if (venc->hwdecode)
            strlist_add_many(cmd, "-hwaccel", "vaapi", "-hwaccel_device", "va",
                             "-hwaccel_output_format", "vaapi", NULL);
    }
    if (keep > 0) {
        strlist_add_many(cmd, "-i", src, NULL);
        strlist_add_list(cmd, &st->cover_input);
    } else if (seek_out) {
        strlist_add_many(cmd, "-i", src, NULL);
    } else {
        /* Input-Seeking: springt zum Keyframe davor und verwirft bis zur exakten
         * Position – framegenau, ohne das ganze Intro zu dekodieren */
        strlist_add(cmd, "-ss");
        strlist_add_printf(cmd, "%.3f", cut_len);
        strlist_add_many(cmd, "-i", src, NULL);
        strlist_add_list(cmd, &st->cover_input);
    }
    strlist_add_list(cmd, &st->map_args);
    strlist_add_many(cmd, "-dn", "-ignore_unknown", "-c", "copy", NULL);
    if (keep > 0) {
        strlist_add(cmd, "-t");
        strlist_add_printf(cmd, "%.3f", keep);
    } else if (seek_out) {
        strlist_add(cmd, "-ss");
        strlist_add_printf(cmd, "%.3f", cut_len);
    } else {
        strlist_add_many(cmd, "-copypriorss", "0", NULL);
    }
    strlist_add_printf(cmd, "-c:%d", st->video_out_index);
    strlist_add(cmd, venc->name);
    strlist_add_list(cmd, &venc->args);
    bool hw_frames = venc->hw && venc->hwdecode && venc->hw_filter[0];
    const char *filter = hw_frames ? venc->hw_filter : venc->filter;
    if (filter[0]) {
        strlist_add_printf(cmd, "-filter:%d", st->video_out_index);
        strlist_add(cmd, filter);
    }
    if (hw_frames) {        /* kein Software-Skalierer beim Neuaufbau der Filterkette */
        strlist_add_printf(cmd, "-autoscale:%d", st->video_out_index);
        strlist_add(cmd, "0");
    }
    if (venc->tag[0]) {
        strlist_add_printf(cmd, "-tag:%d", st->video_out_index);
        strlist_add(cmd, venc->tag);
    }
    strlist_add_many(cmd, "-avoid_negative_ts", "make_zero", "-max_muxing_queue_size",
                     "4096", NULL);
    strlist_add_list(cmd, &st->attach_args);
    strlist_add(cmd, target);
}

/* ---------------------------------------------------------------------------
 * Laufende Jobs (Statuszeile), Dispatcher-Stand, GPU-Zustand
 * ------------------------------------------------------------------------- */
typedef struct Job {
    struct Job *prev, *next;
    char *rel;
    char what[48];
    double pct;
    double since;
    double phase_since;     /* Beginn der aktuellen Phase (der Fortschritt zählt ab hier) */
    double bytes;           /* Größe der Datei (für die Restdauer) */
    bool gpu;               /* läuft (bzw. startet) auf der GPU */
} Job;

static Job *g_jobs_head, *g_jobs_tail;          /* nach Startzeit geordnet */
static pthread_mutex_t g_jobs_lock = PTHREAD_MUTEX_INITIALIZER;

static double active_clock(void);

/* laufenden Job für die Statusanzeige eintragen; bytes: Größe der Datei */
static Job *job_register(const char *rel, bool gpu, double bytes)
{
    Job *j = xmalloc(sizeof *j);
    memset(j, 0, sizeof *j);
    j->rel = xstrdup(rel);
    j->gpu = gpu;
    j->bytes = bytes;
    snprintf(j->what, sizeof j->what, "Start");
    double since = active_clock();
    pthread_mutex_lock(&g_jobs_lock);
    j->since = j->phase_since = since;
    j->prev = g_jobs_tail;
    if (g_jobs_tail) g_jobs_tail->next = j;
    else g_jobs_head = j;
    g_jobs_tail = j;
    pthread_mutex_unlock(&g_jobs_lock);
    return j;
}

/* Job aus der Statusanzeige austragen und freigeben */
static void job_unregister(Job *j)
{
    pthread_mutex_lock(&g_jobs_lock);
    if (j->prev) j->prev->next = j->next;
    else g_jobs_head = j->next;
    if (j->next) j->next->prev = j->prev;
    else g_jobs_tail = j->prev;
    pthread_mutex_unlock(&g_jobs_lock);
    free(j->rel);
    free(j);
}

/* neue Phase setzen (z. B. "GPU H264"); der Fortschritt beginnt wieder bei 0 */
static void job_set_phase(Job *j, const char *what)
{
    if (!j) return;
    double now = active_clock();
    pthread_mutex_lock(&g_jobs_lock);
    snprintf(j->what, sizeof j->what, "%s", what);
    j->pct = 0.0;
    j->phase_since = now;
    if (starts_with(what, "GPU"))
        j->gpu = true;
    else if (starts_with(what, "CPU") || !strcmp(what, "Copy"))
        j->gpu = false;
    pthread_mutex_unlock(&g_jobs_lock);
}

/* Fortschritt des Jobs in Prozent setzen */
static void job_set_percent(Job *j, double pct)
{
    if (!j) return;
    pthread_mutex_lock(&g_jobs_lock);
    j->pct = pct;
    pthread_mutex_unlock(&g_jobs_lock);
}

static pthread_mutex_t g_sched_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_sched_pending, g_sched_inflight;   /* vom Dispatcher gepflegt */

static void schedule_update(int pending, int inflight)
{
    pthread_mutex_lock(&g_sched_lock);
    g_sched_pending = pending;
    g_sched_inflight = inflight;
    pthread_mutex_unlock(&g_sched_lock);
}

static pthread_mutex_t g_gpu_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_gpu_hangs;
static bool g_gpu_disabled;

/* GPU-Hänger zählen; nach GPU_TROUBLE_MAX GPU für den Rest des Laufs aus */
static void gpu_note_hang(const char *what)
{
    pthread_mutex_lock(&g_gpu_lock);
    g_gpu_hangs++;
    if (g_gpu_hangs >= GPU_TROUBLE_MAX && !g_gpu_disabled) {
        g_gpu_disabled = true;
        ic_log(true, "  !!! GPU hängt wiederholt (%s) – restliche Dateien per CPU.", what);
    }
    pthread_mutex_unlock(&g_gpu_lock);
}

/* true, wenn die GPU für den Rest des Laufs abgeschaltet ist */
static bool gpu_is_disabled(void)
{
    pthread_mutex_lock(&g_gpu_lock);
    bool d = g_gpu_disabled;
    pthread_mutex_unlock(&g_gpu_lock);
    return d;
}

/* ---------------------------------------------------------------------------
 * Laufende ffmpeg-Jobs: Pause (vom Nutzer) und GPU-Bremse
 * Alle Jobs aus run_ffmpeg_watched() sind hier mit einem pidfd eingetragen. Signale gehen
 * über pidfds: ein gerade beendeter Job kann so nie mit einem fremden Prozess
 * verwechselt werden, der seine PID erbt.
 *
 * GPU-Bremse: hält die Auslastung der GPU bei höchstens GPU_TARGET %, damit
 * nebenher z. B. Videos flüssig laufen. Gemessen wird die Video-Einheit
 * (amdgpu: vcn_busy_percent, sonst gpu_busy_percent); liegt sie darüber,
 * werden alle laufenden GPU-Jobs in jedem Takt kurz angehalten (SIGSTOP/SIGCONT).
 * ------------------------------------------------------------------------- */
/* Taktlänge der Bremse (s); je Takt eine Pause, lang genug, dass die GPU-Warteschlange leerläuft */
#define THROTTLE_PERIOD 0.5
#define THROTTLE_MAX 0.6        /* höchstens 60 % der Zeit angehalten */

typedef struct RunningProc {
    int pidfd;
    bool gpu;
    struct RunningProc *next;
} RunningProc;

static pthread_mutex_t g_running_lock = PTHREAD_MUTEX_INITIALIZER;
static RunningProc *g_running;          /* laufende Jobs */
static bool g_paused;                   /* vom Nutzer pausiert */
static double g_pause_since, g_pause_total_secs;
static bool g_brake_running, g_brake_holding; /* Bremse läuft / GPU-Jobs gerade angehalten */
static pthread_t g_brake_thread_id;
static char g_gpu_load_path[PATH_MAX];  /* sysfs-Datei mit der Auslastung in % */
static double g_gpu_load = NAN, g_brake_share; /* geglättete Last, Bremse 0..1 */
static double g_gpu_load_sum, g_brake_share_sum, g_brake_share_max;
static long g_brake_samples;
static bool g_brake_engaged;            /* Bremse hat in diesem Lauf gegriffen */

/* Signal an alle laufenden ffmpeg-Prozesse senden (nur an GPU-Jobs, wenn gpu_only);
 * der Aufrufer hält g_running_lock */
static void signal_running(int sig, bool gpu_only)
{
    for (RunningProc *p = g_running; p; p = p->next)
        if (p->gpu || !gpu_only) syscall(SYS_pidfd_send_signal, p->pidfd, sig, NULL, 0);
}

/* Uhr ohne Pausen – für Watchdog und Laufzeiten */
static double active_clock(void)
{
    pthread_mutex_lock(&g_running_lock);
    double now = monotonic_secs(),
           t = now - g_pause_total_secs - (g_paused ? now - g_pause_since : 0);
    pthread_mutex_unlock(&g_running_lock);
    return t;
}

/* Bearbeitung anhalten bzw. fortsetzen (siehe ic_core.h) */
void ic_set_paused(bool on)
{
    pthread_mutex_lock(&g_running_lock);
    bool change = on != g_paused;
    if (change) {
        double now = monotonic_secs();
        if (on)
            g_pause_since = now;
        else
            g_pause_total_secs += now - g_pause_since;
        g_paused = on;
        signal_running(on ? SIGSTOP : SIGCONT, false);
        if (!on) g_brake_holding = false;   /* die Bremse beginnt ihren Takt neu */
    }
    pthread_mutex_unlock(&g_running_lock);
    if (change) ic_log(false, on ? "  Bearbeitung pausiert" : "  Bearbeitung fortgesetzt");
}

/* true, wenn der Nutzer die Bearbeitung pausiert hat */
bool ic_paused(void)
{
    pthread_mutex_lock(&g_running_lock);
    bool p = g_paused;
    pthread_mutex_unlock(&g_running_lock);
    return p;
}

/* Pause aufheben und gemessene Pausenzeit zurücksetzen (Beginn eines Laufs) */
static void pause_reset(void)
{
    pthread_mutex_lock(&g_running_lock);
    if (g_paused) signal_running(SIGCONT, false);
    g_paused = false;
    g_pause_total_secs = 0;
    pthread_mutex_unlock(&g_running_lock);
}

/* Prozentwert 0–100 aus einer sysfs-Datei lesen -> false, wenn nicht lesbar */
static bool read_percent_file(const char *file, double *v)
{
    char buf[32];
    int fd = open(file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = 0;
    buf[strcspn(buf, "\r\n")] = 0;
    return parse_float(buf, v) && *v >= 0 && *v <= 100;
}

/* Regelschleife der GPU-Bremse: misst die Last und hält GPU-Jobs taktweise an */
static void *gpu_brake_thread(void *arg)
{
    (void)arg;
    unsigned seed = (unsigned)getpid();
    double now = monotonic_secs(), next_sample = now, next_ctl = now + 0.5, period = now;
    double ewma = NAN;
    while (true) {
        pthread_mutex_lock(&g_running_lock);
        bool keep_running = g_brake_running, jobs = false, paused = g_paused;
        for (RunningProc *p = g_running; p && !jobs; p = p->next) jobs = p->gpu;
        pthread_mutex_unlock(&g_running_lock);
        if (!keep_running) break;
        now = monotonic_secs();
        double v;
        if (!jobs) {
            ewma = NAN;                     /* ohne GPU-Jobs nicht messen (GPU darf schlafen) */
        } else if (now >= next_sample) {
            if (read_percent_file(g_gpu_load_path, &v))
                ewma = isnan(ewma) ? v : ewma + (v - ewma) * 0.1;      /* ~1 s geglättet */
            /* unregelmäßig messen: kein Gleichtakt mit den Pausen */
            next_sample = now + 0.06 + 0.08 * rand_r(&seed) / (double)RAND_MAX;
        }
        pthread_mutex_lock(&g_running_lock);
        if (now >= next_ctl) {              /* Regler: 2x pro Sekunde */
            next_ctl = now + 0.5;
            double p = g_brake_share;
            if (!jobs || paused || isnan(ewma))
                p = 0;
            else if (ewma > GPU_TARGET)
                p += 0.005 + 0.003 * (ewma - GPU_TARGET);
            else if (ewma < GPU_TARGET - 3)
                p -= 0.01;
            g_brake_share = p = fmin(THROTTLE_MAX, fmax(0.0, p));
            g_gpu_load = ewma;
            if (jobs && !paused && !isnan(ewma)) {
                g_gpu_load_sum += ewma;
                g_brake_share_sum += p;
                g_brake_share_max = fmax(g_brake_share_max, p);
                g_brake_samples++;
            }
            if (p > 0.005 && !g_brake_engaged) {
                g_brake_engaged = true;
                pthread_mutex_unlock(&g_running_lock);
                ic_log(true, "  GPU-Auslastung über %d %% – GPU-Jobs werden kurz pausiert.",
                       GPU_TARGET);
                pthread_mutex_lock(&g_running_lock);
            }
        }
        if (now - period >= THROTTLE_PERIOD) period = now;
        if (!g_paused) {                    /* Pause des Nutzers hat Vorrang */
            bool stop = jobs && g_brake_share > 0.005 &&
                        now - period < g_brake_share * THROTTLE_PERIOD;
            if (stop != g_brake_holding) {
                signal_running(stop ? SIGSTOP : SIGCONT, true);
                g_brake_holding = stop;
            }
        }
        pthread_mutex_unlock(&g_running_lock);
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    pthread_mutex_lock(&g_running_lock);
    if (g_brake_holding && !g_paused) signal_running(SIGCONT, true);
    g_brake_holding = false;
    pthread_mutex_unlock(&g_running_lock);
    return NULL;
}

/* Messdatei zum VAAPI-Gerät, z. B. /dev/dri/renderD128 ->
 * /sys/class/drm/renderD128/device/vcn_busy_percent */
static bool gpu_load_file(const char *device, char *out, size_t sz)
{
#ifdef IC_TEST_GPU_LOAD
    const char *test = getenv("IC_GPU_LOAD_FILE");      /* nur Test-Builds */
    if (test && *test) {
        snprintf(out, sz, "%s", test);
        return true;
    }
#endif
    char *real = realpath_lenient(device), *base = strrchr(real, '/');
    static const char *const NAMES[] = {"vcn_busy_percent", "gpu_busy_percent"};
    bool ok = false;
    for (size_t i = 0; i < 2 && !ok; i++) {
        snprintf(out, sz, "/sys/class/drm/%s/device/%s", base ? base + 1 : real, NAMES[i]);
        double v;
        ok = read_percent_file(out, &v);
    }
    free(real);
    return ok;
}

/* GPU-Bremse für einen Lauf starten (nur, wenn die GPU-Last messbar ist) */
static void gpu_brake_start(const GpuCaps *gpu)
{
    g_gpu_load = NAN;
    g_brake_share = g_gpu_load_sum = g_brake_share_sum = g_brake_share_max = 0;
    g_brake_samples = 0;
    g_brake_engaged = false;
    if (!gpu || !gpu->device[0]) return;
    if (!gpu_load_file(gpu->device, g_gpu_load_path, sizeof g_gpu_load_path)) {
        ic_log(false, "  GPU-Auslastung nicht messbar – keine Begrenzung auf %d %%.", GPU_TARGET);
        return;
    }
    g_brake_running = true;
    if (pthread_create(&g_brake_thread_id, NULL, gpu_brake_thread, NULL) != 0)
        g_brake_running = false;
}

/* GPU-Bremse beenden und ihre Statistik ins Lauf-Log schreiben */
static void gpu_brake_stop(void)
{
    pthread_mutex_lock(&g_running_lock);
    bool was_running = g_brake_running;
    g_brake_running = false;
    pthread_mutex_unlock(&g_running_lock);
    if (!was_running) return;
    pthread_join(g_brake_thread_id, NULL);
    if (g_brake_engaged && g_brake_samples)
        ic_log(false,
               "  GPU-Bremse: Ø %.0f %% der Zeit pausiert (max. %.0f %%), GPU-Last Ø %.0f %%",
               100 * g_brake_share_sum / g_brake_samples, 100 * g_brake_share_max,
               g_gpu_load_sum / g_brake_samples);
    g_gpu_load = NAN;
    g_brake_share = 0;
}

/* Job anmelden -> pidfd (-1: nicht steuerbar, z. B. Kernel ohne pidfd).
 * Startet er während einer Pause, wird er sofort mit angehalten. */
static int running_add(pid_t pid, bool gpu)
{
    int fd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    RunningProc *p = xmalloc(sizeof *p);
    p->pidfd = fd;
    p->gpu = gpu;
    pthread_mutex_lock(&g_running_lock);
    p->next = g_running;
    g_running = p;
    if (g_paused) syscall(SYS_pidfd_send_signal, fd, SIGSTOP, NULL, 0);
    pthread_mutex_unlock(&g_running_lock);
    return fd;
}

/* Job wieder abmelden (am Ende von run_ffmpeg_watched) */
static void running_remove(int fd)
{
    if (fd < 0) return;
    pthread_mutex_lock(&g_running_lock);
    for (RunningProc **link = &g_running; *link; link = &(*link)->next)
        if ((*link)->pidfd == fd) {
            RunningProc *p = *link;
            *link = p->next;
            free(p);
            break;
        }
    if (g_brake_holding || g_paused) syscall(SYS_pidfd_send_signal, fd, SIGCONT, NULL, 0);
    pthread_mutex_unlock(&g_running_lock);
    close(fd);
}

/* ---------------------------------------------------------------------------
 * Standby verhindern (systemd-logind), solange gesucht oder bearbeitet wird.
 * systemd-inhibit läuft als Kind; sein stdin ist eine Pipe von uns: endet
 * Intro-Cutter – auch durch einen Absturz –, liest es EOF und die Sperre fällt.
 * ------------------------------------------------------------------------- */
static pthread_mutex_t g_inhibit_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_inhibit_users, g_inhibit_pipe = -1;
static pid_t g_inhibit_pid;

/* Standby sperren; die erste Anforderung startet systemd-inhibit, weitere zählen nur mit */
static void standby_lock_begin(const char *why)
{
    pthread_mutex_lock(&g_inhibit_lock);
    if (g_inhibit_users++ == 0 && command_exists("systemd-inhibit")) {
        int p[2];
        if (pipe2(p, O_CLOEXEC) == 0) {
            posix_spawn_file_actions_t file_actions;
            posix_spawn_file_actions_init(&file_actions);
            posix_spawn_file_actions_adddup2(&file_actions, p[0], 0);
            posix_spawn_file_actions_addopen(&file_actions, 1, "/dev/null", O_WRONLY, 0);
            posix_spawn_file_actions_addopen(&file_actions, 2, "/dev/null", O_WRONLY, 0);
            posix_spawnattr_t attr;
            posix_spawnattr_init(&attr);
            sigset_t none, dflt;
            sigemptyset(&none);
            sigemptyset(&dflt);
            sigaddset(&dflt, SIGPIPE);
            sigaddset(&dflt, SIGINT);
            sigaddset(&dflt, SIGTERM);
            sigaddset(&dflt, SIGHUP);
            posix_spawnattr_setsigmask(&attr, &none);
            posix_spawnattr_setsigdefault(&attr, &dflt);
            posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
            char *whyarg = xasprintf("--why=%s", why);
            char *argv[] = {"systemd-inhibit", "--what=sleep:idle", "--who=Intro-Cutter", whyarg,
                            "--mode=block", "cat", NULL};
            pid_t pid;
            if (posix_spawnp(&pid, argv[0], &file_actions, &attr, argv, environ) == 0) {
                g_inhibit_pid = pid;
                g_inhibit_pipe = p[1];
                p[1] = -1;
            }
            free(whyarg);
            posix_spawn_file_actions_destroy(&file_actions);
            posix_spawnattr_destroy(&attr);
            close(p[0]);
            if (p[1] >= 0) close(p[1]);
            /* lehnt logind ab, endet systemd-inhibit sofort */
            struct timespec ts = {0, 150 * 1000 * 1000};
            nanosleep(&ts, NULL);
            if (g_inhibit_pipe >= 0 && waitpid(g_inhibit_pid, NULL, WNOHANG) == g_inhibit_pid) {
                close(g_inhibit_pipe);
                g_inhibit_pipe = -1;
                ic_log(false, "  Standby-Sperre nicht möglich (systemd-inhibit lehnte ab).");
            }
        }
    }
    pthread_mutex_unlock(&g_inhibit_lock);
}

/* Standby-Sperre freigeben, sobald sie niemand mehr braucht */
static void standby_lock_end(void)
{
    pthread_mutex_lock(&g_inhibit_lock);
    if (g_inhibit_users > 0 && --g_inhibit_users == 0 && g_inhibit_pipe >= 0) {
        close(g_inhibit_pipe);              /* EOF -> cat und systemd-inhibit enden */
        g_inhibit_pipe = -1;
        int i = 0;
        while (waitpid(g_inhibit_pid, NULL, WNOHANG) == 0 && i++ < 100) {
            struct timespec ts = {0, 20 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
        if (i > 100) {
            kill(g_inhibit_pid, SIGTERM);
            waitpid(g_inhibit_pid, NULL, 0);
        }
    }
    pthread_mutex_unlock(&g_inhibit_lock);
}

/* true, wenn der GPU-Encoder name (h264, hevc, av1) nutzbar ist */
static bool caps_has_encoder(const GpuCaps *c, const char *name)
{
    if (!c) return false;
    if (!strcmp(name, "h264")) return c->h264;
    if (!strcmp(name, "hevc")) return c->hevc;
    return c->av1;
}

/* true, wenn der GPU-Encoder name auch 10 Bit kodieren kann */
static bool caps_has_10bit(const GpuCaps *c, const char *name)
{
    if (!c) return false;
    if (!strcmp(name, "hevc")) return c->hevc10;
    if (!strcmp(name, "av1")) return c->av110;
    return false;                   /* H.264: 10 Bit wird nicht genutzt */
}

/* Nutzbarkeit eines GPU-Encoders eintragen */
static void caps_set_encoder(GpuCaps *c, const char *name, bool v)
{
    if (!strcmp(name, "h264")) c->h264 = v;
    else if (!strcmp(name, "hevc")) c->hevc = v;
    else c->av1 = v;
}

/* true, wenn mindestens ein GPU-Encoder nutzbar ist */
static bool caps_has_any(const GpuCaps *c)
{
    return c && (c->h264 || c->hevc || c->av1);
}

/* Qualitätsparameter für den VAAPI-Encoder name anhängen (konstante Qualität) */
static void add_gpu_quality_args(StrList *l, const char *name)
{
    if (!strcmp(name, "h264"))
        strlist_add_many(l, "-rc_mode", "CQP", "-qp", GPU_QP_H264, NULL);
    else if (!strcmp(name, "hevc"))
        strlist_add_many(l, "-rc_mode", "CQP", "-qp", GPU_QP_HEVC, NULL);
    else
        strlist_add_many(l, "-rc_mode", "CQP", "-global_quality", GPU_Q_AV1, NULL);
}

/* Mini-Encode auf der GPU (~0,2 s). Weckt einen hängenden VAAPI-Job auf,
 * falls Treiber/GPU erst durch weitere GPU-Aktivität weitermachen. */
static bool gpu_poke(const char *dev, const GpuCaps *caps)
{
    const char *name = "h264";
    for (const char *const *n = GPU_CODECS; *n; n++)
        if (caps_has_encoder(caps, *n)) {
            name = *n;
            break;
        }
    ic_log(true, "  Watchdog: kein Fortschritt – stoße GPU an (%s) ...", name);
    StrList cmd = {0};
    strlist_add_many(&cmd, "ffmpeg", "-nostdin", "-v", "error", "-init_hw_device", NULL);
    strlist_add_printf(&cmd, "vaapi=va:%s", dev);
    strlist_add_many(&cmd, "-filter_hw_device", "va", "-f", "lavfi", "-i",
                     "testsrc2=size=160x120:rate=25", "-t", "0.2", "-vf", "format=nv12,hwupload",
                     "-c:v", NULL);
    strlist_add_printf(&cmd, "%s_vaapi", name);
    add_gpu_quality_args(&cmd, name);
    strlist_add_many(&cmd, "-f", "null", "-", NULL);
    int rc = run_ffmpeg(&cmd, 20, NULL, NULL);
    strlist_free(&cmd);
    return rc == 0;
}

/* ---------------------------------------------------------------------------
 * Watchdog für lange ffmpeg-Läufe
 * ------------------------------------------------------------------------- */
typedef struct {
    const char *device;
    const GpuCaps *caps;
} PokeTarget;

/* CPU-Zeit (user + system, s) eines Prozesses – wie psutil cpu_times() */
static bool process_cpu_secs(pid_t pid, double *out)
{
    char path[64], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return false;
    buf[n] = 0;
    char *p = strrchr(buf, ')');                /* Prozessname kann Leerzeichen enthalten */
    unsigned long long user_ticks, system_ticks;
    if (!p || sscanf(p + 1, " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
                     &user_ticks, &system_ticks) != 2)
        return false;
    *out = (double)(user_ticks + system_ticks) / (double)sysconf(_SC_CLK_TCK);
    return true;
}

#define ERR_KEEP 60             /* so viele stderr-Zeilen behalten */

typedef struct {
    char *line[ERR_KEEP];
    int head, n;
} ErrorRing;

/* stderr-Zeile merken (es bleiben nur die letzten ERR_KEEP Zeilen) */
static void error_ring_push(ErrorRing *ring, const char *line, size_t len)
{
    int i = (ring->head + ring->n) % ERR_KEEP;
    if (ring->n == ERR_KEEP) {
        free(ring->line[ring->head]);
        ring->head = (ring->head + 1) % ERR_KEEP;
        ring->n--;
        i = (ring->head + ring->n) % ERR_KEEP;
    }
    ring->line[i] = xstrndup(line, len);
    ring->n++;
}

/* vollständige Zeilen aus b entnehmen (\n, \r oder \r\n); final: Rest auch */
static void take_complete_lines(Buf *b, bool final,
                                void (*on_line)(void *, char *, size_t), void *ctx)
{
    size_t start = 0;
    for (size_t i = 0; i < b->n; i++) {
        if (b->p[i] != '\n' && b->p[i] != '\r') continue;
        if (b->p[i] == '\r' && i + 1 == b->n && !final) break;   /* evtl. folgt \n */
        on_line(ctx, b->p + start, i - start);
        if (b->p[i] == '\r' && i + 1 < b->n && b->p[i + 1] == '\n') i++;
        start = i + 1;
    }
    if (final && start < b->n) {
        on_line(ctx, b->p + start, b->n - start);
        start = b->n;
    }
    if (start) {
        memmove(b->p, b->p + start, b->n - start);
        b->n -= start;
        b->p[b->n] = 0;
    }
}

/* Callback für jede stderr-Zeile von ffmpeg */
static void on_stderr_line(void *ctx, char *s, size_t n)
{
    error_ring_push(ctx, s, n);
}

/* Messpunkt der Tempo-Prüfung: Zeitpunkt t und bis dahin erzeugte Ausgabezeit v (beides s) */
typedef struct {
    double t, v;
} SpeedSample;

typedef struct {
    Job *job;
    double expected;            /* erwartete Ausgabedauer (s), 0 = unbekannt */
    bool gpu_job;
    double now;
    char *cur_frame, *cur_out_us, *cur_size;    /* aktueller Fortschrittsblock */
    char *marked_frame, *marked_out_us, *marked_size; /* letzter Stand (Marker) */
    bool have_marker, resumed_after_poke, got_line;
    double last_progress_at, last_alive_at, poked_at, ended_at;
    double cpu_secs, cpu_secs_at, cpu_secs_at_progress, cpu_progress_at;
    SpeedSample *speed_samples;     /* Messpunkte; gültig ist [speed_first, speed_count) */
    size_t speed_first, speed_count, speed_capacity;
} Watchdog;

/* true, wenn beide Strings gleich oder beide NULL sind */
static bool same_or_both_null(const char *a, const char *b)
{
    return (!a && !b) || (a && b && !strcmp(a, b));
}

/* String-Feld durch eine Kopie von v ersetzen (der alte Wert wird freigegeben) */
static void replace_str(char **dst, const char *v)
{
    free(*dst);
    *dst = v ? xstrdup(v) : NULL;
}

/* eine Zeile der -progress-Ausgabe verarbeiten */
static void on_progress_line(void *ctx, char *s, size_t n)
{
    Watchdog *w = ctx;
    while (n && isspace((unsigned char)*s)) s++, n--;
    while (n && isspace((unsigned char)s[n - 1])) n--;
    if (!n) return;
    w->got_line = true;
    char *line = xstrndup(s, n), *equals = strchr(line, '=');
    const char *k = line, *v = "";
    if (equals) {
        *equals = 0;
        v = equals + 1;
    }
    if (!strcmp(k, "frame")) replace_str(&w->cur_frame, v);
    else if (!strcmp(k, "out_time_us")) replace_str(&w->cur_out_us, v);
    else if (!strcmp(k, "total_size")) replace_str(&w->cur_size, v);
    else if (!strcmp(k, "progress")) {
        if (!w->have_marker || !same_or_both_null(w->cur_frame, w->marked_frame) ||
            !same_or_both_null(w->cur_out_us, w->marked_out_us) ||
            !same_or_both_null(w->cur_size, w->marked_size)) {
            w->have_marker = true;
            replace_str(&w->marked_frame, w->cur_frame);
            replace_str(&w->marked_out_us, w->cur_out_us);
            replace_str(&w->marked_size, w->cur_size);
            w->last_progress_at = w->last_alive_at = w->now;
            w->cpu_secs_at_progress = w->cpu_secs;
            w->cpu_progress_at = w->cpu_secs_at;
            if (!isnan(w->poked_at)) w->resumed_after_poke = true;
        }
        double t;
        if (parse_float(w->cur_out_us, &t) && t > 0) {
            if (w->expected) job_set_percent(w->job, fmin(99.9, t / 1e6 / w->expected * 100));
            if (w->gpu_job) {
                if (w->speed_count == w->speed_capacity) {
                    if (w->speed_first) {
                        memmove(w->speed_samples, w->speed_samples + w->speed_first,
                                (w->speed_count - w->speed_first) * sizeof *w->speed_samples);
                        w->speed_count -= w->speed_first;
                        w->speed_first = 0;
                    } else {
                        w->speed_capacity = w->speed_capacity ? w->speed_capacity * 2 : 256;
                        w->speed_samples = xrealloc(w->speed_samples,
                                                    w->speed_capacity * sizeof *w->speed_samples);
                    }
                }
                w->speed_samples[w->speed_count].t = w->now;
                w->speed_samples[w->speed_count++].v = t / 1e6;
                while (w->now - w->speed_samples[w->speed_first].t > CRAWL_WINDOW_SECS)
                    w->speed_first++;
            }
        }
        if (!strcmp(v, "end") && isnan(w->ended_at)) w->ended_at = w->now;
    }
    free(line);
}

/* Lange ffmpeg-Läufe mit Watchdog (Fortschritt über -progress) -> rc.
 * event: ""             normal beendet
 *        "poke-ok"      hing, lief nach Anstoßen der GPU weiter
 *        "teardown"     fertig geschrieben, hing aber beim Beenden -> abgebrochen
 *        "stall"        kein Fortschritt mehr (Prozess wartet) -> abgebrochen
 *        "spin"         kein Fortschritt, aber Dauer-CPU-Last (Endlosschleife) -> abgebrochen
 *        "crawl"        GPU-Job extrem langsam (< CRAWL_MIN_SPEED) -> abgebrochen
 *        "+unkillable"  Prozess ließ sich nicht beenden (hängt im Treiber)
 * gpu_job: CPU-Last zählt NICHT als Lebenszeichen (ein GPU-Job, der nur CPU
 * verbrennt, ohne Frames zu liefern, hängt in einer Schleife).
 * low_prio: mit nice 19 statt 15 starten (Modus 4, x264 hinter den GPU-Jobs). */
static int run_ffmpeg_watched(const StrList *cmd_in, Job *job, double expected,
                              const PokeTarget *poke, bool gpu_job, bool low_prio, char *event,
                              size_t evsz, Buf *errout)
{
    event[0] = 0;
    while (ic_paused() && !ic_cancel) sleep_secs(0.2);  /* pausiert: noch nicht starten */
    StrList cmd = {0};
    strlist_add(&cmd, cmd_in->v[0]);
    strlist_add_many(&cmd, "-progress", "pipe:1", "-nostats", NULL);
    for (int i = 1; i < cmd_in->n; i++) strlist_add(&cmd, cmd_in->v[i]);
    ChildProc c;
    int e = spawn_child(&c, &cmd, low_prio ? PRIO_LOW : PRIO_NICE, IO_NULL, IO_PIPE, IO_PIPE);
    strlist_free(&cmd);
    if (e) {
        buf_printf(errout, "[Errno %d] %s: '%s'", e, strerror(e), cmd_in->v[0]);
        return -1;
    }
    int proc_fd = running_add(c.pid, gpu_job); /* Pause bzw. GPU-Bremse darf ihn anhalten */
    Watchdog w;
    memset(&w, 0, sizeof w);
    w.job = job;
    w.expected = expected;
    w.gpu_job = gpu_job;
    /* Zeit ohne Pausen; steht unser eigener Prozess (Strg+Z), zählt die Lücke nicht */
    double started_at = active_clock(), cpu_check = started_at, wall = monotonic_secs(),
           stopped_secs = 0;
    w.last_progress_at = w.last_alive_at = started_at;
    w.cpu_secs = w.cpu_secs_at = w.cpu_secs_at_progress = w.cpu_progress_at = NAN;
    w.poked_at = w.ended_at = NAN;
    Buf obuf = {0}, ebuf = {0};
    ErrorRing ring;
    memset(&ring, 0, sizeof ring);
    bool proc_ok = true, out_done = false;
    int pokes = 0;
    while (true) {
        struct pollfd poll_fds[2];
        nfds_t n = 0;
        if (c.out >= 0) poll_fds[n++] = (struct pollfd){.fd = c.out, .events = POLLIN};
        if (c.err >= 0) poll_fds[n++] = (struct pollfd){.fd = c.err, .events = POLLIN};
        /* Takt 1 s wie in der Python-Version; nach Ende der Ausgabe kürzer,
         * damit das Prozessende ohne Verzögerung bemerkt wird */
        poll(n ? poll_fds : NULL, n, c.out >= 0 ? 1000 : 50);
        double m = monotonic_secs();
        if (m - wall > 5) stopped_secs += m - wall - 1;
        wall = m;
        double now = w.now = active_clock() - stopped_secs;
        w.got_line = false;
        if (c.out >= 0) {
            read_available(&c.out, &obuf);
            take_complete_lines(&obuf, c.out < 0, on_progress_line, &w);
            if (c.out < 0) out_done = true;
        }
        if (c.err >= 0) {
            read_available(&c.err, &ebuf);
            take_complete_lines(&ebuf, c.err < 0, on_stderr_line, &ring);
        }
        if (ic_cancel) break;                   /* Strg+C: sofort beenden */
        /* CPU-Zeit als zusätzliches Lebenszeichen (z. B. beim Überspringen des Intros) */
        if (proc_ok && !c.reaped && now - cpu_check >= 5) {
            double cpu_now;
            cpu_check = now;
            if (process_cpu_secs(c.pid, &cpu_now)) {
                if (!isnan(w.cpu_secs) && cpu_now - w.cpu_secs >= 0.3) w.last_alive_at = now;
                if (isnan(w.cpu_secs_at_progress)) {
                    w.cpu_secs_at_progress = cpu_now;
                    w.cpu_progress_at = now;
                }
                w.cpu_secs = cpu_now;
                w.cpu_secs_at = now;
            } else {
                proc_ok = false;
            }
        }
        if (child_exited(&c) && (out_done || !w.got_line)) break;
        if (!isnan(w.ended_at)) {
            if (now - w.ended_at > END_GRACE_SECS) {
                snprintf(event, evsz, "teardown");
                break;
            }
            continue;
        }
        /* GPU-Jobs: nur echter Fortschritt zählt – CPU-Last allein ist kein Lebenszeichen */
        double idle = now -
                      (gpu_job ? w.last_progress_at : fmax(w.last_progress_at, w.last_alive_at));
        if (poke && pokes < 3 && idle > STALL_POKE_SECS &&
            (isnan(w.poked_at) || now - w.poked_at > STALL_POKE_SECS)) {
            w.poked_at = now;
            pokes++;
            gpu_poke(poke->device, poke->caps);
            continue;
        }
        if (idle > STALL_KILL_SECS || now - w.last_progress_at > HARD_STALL_SECS) {
            /* seit dem letzten Fortschritt noch >= 30 % einer CPU verbraucht: ffmpeg dreht leer
             * ("spin"), sonst hängt es ("stall") */
            double cpu_window = w.cpu_secs_at - w.cpu_progress_at;
            bool busy = !isnan(w.cpu_secs) && !isnan(w.cpu_secs_at_progress) && cpu_window >= 4 &&
                        (w.cpu_secs - w.cpu_secs_at_progress) / cpu_window >= 0.3;
            snprintf(event, evsz, busy ? "spin" : "stall");
            break;
        }
        /* GPU-Job, der über das ganze Messfenster zu wenig Ausgabezeit erzeugt ("crawl") */
        size_t sample_count = w.speed_count - w.speed_first;
        if (gpu_job && sample_count >= 2) {
            const SpeedSample *oldest = &w.speed_samples[w.speed_first],
                              *newest = &w.speed_samples[w.speed_count - 1];
            double span = newest->t - oldest->t;
            double speed = (newest->v - oldest->v) / span;  /* Ausgabesekunden je Sekunde */
            if (span >= 0.9 * CRAWL_WINDOW_SECS && speed < CRAWL_MIN_SPEED) {
                snprintf(event, evsz, "crawl");
                break;
            }
        }
    }
    if (!child_exited(&c)) {
        kill_child(&c);
        if (!wait_child(&c, 10)) {
            size_t ev_len = strlen(event);
            snprintf(event + ev_len, evsz - ev_len, "+unkillable");
            abandon_child(&c);
        }
    }
    running_remove(proc_fd);
    if (c.err >= 0) {                               /* Rest von stderr einsammeln */
        read_available(&c.err, &ebuf);
        take_complete_lines(&ebuf, true, on_stderr_line, &ring);
    }
    close_fd(&c.out);
    close_fd(&c.err);
    if (!event[0] && !isnan(w.poked_at) && w.resumed_after_poke) snprintf(event, evsz, "poke-ok");
    int rc = child_exit_code(&c);
    if (starts_with(event, "teardown"))
        rc = 0;         /* Datei war bereits komplett geschrieben – die Prüfung entscheidet */
    for (int i = 0; i < ring.n; i++) {
        char *ev_len = ring.line[(ring.head + i) % ERR_KEEP];
        buf_add_str(errout, ev_len);
        buf_add_char(errout, '\n');
        free(ev_len);
    }
    free(w.cur_frame);
    free(w.cur_out_us);
    free(w.cur_size);
    free(w.marked_frame);
    free(w.marked_out_us);
    free(w.marked_size);
    free(w.speed_samples);
    buf_free(&obuf);
    buf_free(&ebuf);
    return rc;
}

static const char *const EVENT_TEXT[][2] = {
    {"stall", "hing (kein Fortschritt, Prozess wartet) – abgebrochen"},
    {"spin", "hing in Endlosschleife (kein Fortschritt, Dauer-CPU-Last) – abgebrochen"},
    {"crawl", "extrem langsam (< 0,1x Echtzeit) – abgebrochen"},
    {"teardown", "hing beim Beenden – abgebrochen, Datei geprüft"},
    {"poke-ok", "hing, lief nach Anstoßen der GPU weiter"},
};

/* Watchdog-Ereignis als lesbarer Text */
static char *watchdog_event_text(const char *event, char *out, size_t size)
{
    size_t n = strcspn(event, "+");
    const char *base = event;
    for (size_t i = 0; i < sizeof EVENT_TEXT / sizeof *EVENT_TEXT; i++)
        if (strlen(EVENT_TEXT[i][0]) == n && !strncmp(event, EVENT_TEXT[i][0], n))
            base = EVENT_TEXT[i][1];
    snprintf(out, size, "%s%s", base,
             strstr(event, "unkillable") ? " (Prozess hängt im Treiber fest)" : "");
    return out;
}

/* true, wenn das Ereignis einen Hänger bedeutet (für die GPU-Abschaltung) */
static bool watchdog_event_is_hang(const char *event)
{
    return strstr(event, "stall") || strstr(event, "spin") || strstr(event, "crawl") ||
           strstr(event, "unkillable");
}

/* ---------------------------------------------------------------------------
 * Parallelität: Governor (CPU), Gates (CPU/GPU/I/O)
 * ------------------------------------------------------------------------- */
static void cond_init_monotonic(pthread_cond_t *cond)
{
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
}

/* absoluter Zeitpunkt in secs Sekunden (CLOCK_MONOTONIC) – für pthread_cond_timedwait */
static struct timespec deadline_in(double secs)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long long ns = ts.tv_nsec + (long long)(secs * 1e9);
    ts.tv_sec += (time_t)(ns / 1000000000LL);
    ts.tv_nsec = (long)(ns % 1000000000LL);
    return ts;
}

/* CPU-Zeitzähler aus /proc/stat lesen: user nice system idle iowait irq softirq steal
 * guest guest_nice -> false bei Fehler */
static bool read_cpu_counters(double counters[10])
{
#ifdef IC_TEST_CPU_LOAD
    /* nur Test-Builds: Last = Grundlast + 12 % je laufendem (nicht angehaltenem) Job */
    static double fake_counters[10];
    const char *base_env = getenv("IC_CPU_BASE");
    int jobs = 0;
    pthread_mutex_lock(&g_running_lock);
    for (RunningProc *p = g_running; p; p = p->next) jobs++;
    bool paused = g_paused;
    pthread_mutex_unlock(&g_running_lock);
    double load = fmin(100.0, (base_env ? atof(base_env) : 20.0) + (paused ? 0 : 12.0 * jobs));
    fake_counters[0] += load;
    fake_counters[3] += 100 - load;
    memcpy(counters, fake_counters, sizeof fake_counters);
    return true;
#endif
    FILE *fp = fopen("/proc/stat", "re");
    if (!fp) return false;
    unsigned long long raw[10] = {0};
    int fields_read = fscanf(fp, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &raw[0],
                             &raw[1], &raw[2], &raw[3], &raw[4], &raw[5], &raw[6], &raw[7],
                             &raw[8], &raw[9]);
    fclose(fp);
    for (int i = 0; i < 10; i++) counters[i] = (double)raw[i];
    return fields_read >= 4;
}

/* System-CPU-Last in % zwischen zwei Messungen – Rechenweg wie psutil.cpu_busy_percent */
static double cpu_busy_percent(const double before[10], const double after[10])
{
    double delta[10], total = 0;
    for (int i = 0; i < 10; i++) {
        delta[i] = fmax(0.0, after[i] - before[i]);
        total += delta[i];
    }
    total -= delta[8] + delta[9];           /* guest ist in user/nice schon enthalten */
    double busy = total - delta[3] - delta[4]; /* idle + iowait */
    return total > 0 ? python_round(busy / total * 100, 1) : 0.0;
}

/* Regelt die erlaubte Anzahl CPU-Jobs anhand der System-CPU-Last.
 * Lange Jobs (Neukodierung, settle > 0): unter CPU_LOW % wird um einen Job
 * hochgefahren – erst nach einer Einschwingzeit seit dem letzten Job-Start –,
 * über CPU_TARGET % um einen gedrosselt.
 * Kurze Jobs (Erkennung, settle = 0): dort startet ständig ein neuer Job, eine
 * Einschwingzeit käme nie zustande. Stattdessen wird jede Sekunde anteilig
 * nachgeregelt (Ziel CPU_TARGET %, höchstens x1,5 je Schritt). */
typedef struct {
    int max, target;
    double settle, last_start, cpu;
    bool stop, running;
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_t thr;
} Governor;

static int clamp_int(int v, int lo, int hi);

/* Mess- und Regelschleife des Governors (einmal pro Sekunde) */
static void *governor_thread(void *arg)
{
    Governor *gov = arg;
    double prev_counters[10], cur_counters[10];
    bool was_paused = false;
    if (!read_cpu_counters(prev_counters)) {    /* keine Messung möglich */
        pthread_mutex_lock(&gov->lock);
        gov->target = gov->max / 2 > 1 ? gov->max / 2 : 1;
        pthread_mutex_unlock(&gov->lock);
        return NULL;
    }
    pthread_mutex_lock(&gov->lock);
    while (!gov->stop) {
        struct timespec ts = deadline_in(1.0);
        while (!gov->stop && pthread_cond_timedwait(&gov->wake, &gov->lock, &ts) != ETIMEDOUT) {}
        if (gov->stop) break;
        pthread_mutex_unlock(&gov->lock);
        bool ok = read_cpu_counters(cur_counters);
        double cpu = ok ? cpu_busy_percent(prev_counters, cur_counters) : 0.0;
        if (ok) memcpy(prev_counters, cur_counters, sizeof prev_counters);
        bool paused = ic_paused();
        pthread_mutex_lock(&gov->lock);
        if (!ok) continue;
        gov->cpu = cpu;
        /* Pause des Nutzers: die niedrige Last sagt nichts über unsere (angehaltenen)
         * Jobs – nicht hochregeln; nach dem Fortsetzen erst wieder einschwingen */
        if (paused || was_paused) {
            if (!paused) gov->last_start = monotonic_secs();
            was_paused = paused;
            continue;
        }
        if (gov->settle <= 0) {                 /* kurze Jobs: anteilig */
            if (cpu < CPU_LOW) {
                int t = (int)lround(gov->target * fmin(1.5, CPU_TARGET / fmax(cpu, 10.0)));
                gov->target = clamp_int(t > gov->target ? t : gov->target + 1, 1, gov->max);
            } else if (cpu > CPU_TARGET) {
                int t = (int)(gov->target * CPU_TARGET / cpu);
                gov->target = clamp_int(t < gov->target ? t : gov->target - 1, 1, gov->max);
            }
            continue;
        }
        bool settled = monotonic_secs() - gov->last_start >= gov->settle;
        if (cpu < CPU_LOW && settled)
            gov->target = gov->target + 1 < gov->max ? gov->target + 1 : gov->max;
        else if (cpu > CPU_TARGET)
            gov->target = gov->target - 1 > 1 ? gov->target - 1 : 1;
    }
    pthread_mutex_unlock(&gov->lock);
    return NULL;
}

/* v auf den Bereich lo..hi begrenzen */
static int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* Governor anlegen und seinen Messthread starten */
static void governor_init(Governor *gov, int max_workers, int initial, double settle)
{
    memset(gov, 0, sizeof *gov);
    gov->max = max_workers > 1 ? max_workers : 1;
    gov->target = clamp_int(initial, 1, gov->max);
    gov->settle = settle;
    pthread_mutex_init(&gov->lock, NULL);
    cond_init_monotonic(&gov->wake);
    gov->running = start_thread(&gov->thr, governor_thread, gov) == 0;
}

/* Startwert und Einschwingzeit neu setzen (Beginn der Bearbeitung) */
static void governor_configure(Governor *gov, int initial, double settle)
{
    pthread_mutex_lock(&gov->lock);
    gov->target = clamp_int(initial, 1, gov->max);
    gov->settle = settle;
    pthread_mutex_unlock(&gov->lock);
}

/* Job-Start melden: die Einschwingzeit beginnt neu */
static void governor_note_start(Governor *gov)
{
    pthread_mutex_lock(&gov->lock);
    gov->last_start = monotonic_secs();
    pthread_mutex_unlock(&gov->lock);
}

/* aktuell erlaubte Anzahl gleichzeitiger Jobs */
static int governor_target(Governor *gov)
{
    pthread_mutex_lock(&gov->lock);
    int t = gov->target;
    pthread_mutex_unlock(&gov->lock);
    return t;
}

/* aktuelles Ziel und zuletzt gemessene CPU-Last */
static void governor_status(Governor *gov, int *target, double *cpu)
{
    pthread_mutex_lock(&gov->lock);
    *target = gov->target;
    *cpu = gov->cpu;
    pthread_mutex_unlock(&gov->lock);
}

/* Messthread beenden */
static void governor_stop(Governor *gov)
{
    pthread_mutex_lock(&gov->lock);
    gov->stop = true;
    pthread_cond_broadcast(&gov->wake);
    pthread_mutex_unlock(&gov->lock);
    if (gov->running) pthread_join(gov->thr, NULL);
    gov->running = false;
}

/* Begrenzt gleichzeitige Jobs; Limit fest oder dynamisch (Governor).
 * Wartende gate_acquire()-Aufrufe (Fallbacks laufender Dateien) haben Vorrang
 * vor gate_try_acquire() des Dispatchers, damit angefangene Dateien zuerst
 * fertig werden. */
typedef struct {
    int fixed_limit;
    Governor *gov;          /* != NULL: Limit = Governor-Ziel, Job-Start wird gemeldet */
    int running, waiting;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    const char *kind;
} Gate;

/* Gate anlegen: fester Grenzwert oder, mit gov, Grenze laut Governor */
static Gate *gate_new(int fixed, Governor *gov, const char *kind)
{
    Gate *g = xmalloc(sizeof *g);
    memset(g, 0, sizeof *g);
    g->fixed_limit = fixed;
    g->gov = gov;
    g->kind = kind;
    pthread_mutex_init(&g->lock, NULL);
    cond_init_monotonic(&g->cond);
    return g;
}

/* aktuelle Grenze des Gates (mindestens 1) */
static int gate_limit(Gate *g)
{
    int l = g->gov ? governor_target(g->gov) : g->fixed_limit;
    return l > 1 ? l : 1;
}

/* Platz belegen, wenn einer frei ist und niemand wartet -> false, wenn voll */
static bool gate_try_acquire(Gate *g)
{
    pthread_mutex_lock(&g->lock);
    bool ok = g->waiting == 0 && g->running < gate_limit(g);
    if (ok) {
        g->running++;
        if (g->gov) governor_note_start(g->gov);
    }
    pthread_mutex_unlock(&g->lock);
    return ok;
}

/* Platz belegen und notfalls warten (Vorrang vor gate_try_acquire) */
static void gate_acquire(Gate *g)
{
    pthread_mutex_lock(&g->lock);
    g->waiting++;
    while (g->running >= gate_limit(g)) {       /* Limit kann sich ohne Signal ändern */
        struct timespec ts = deadline_in(0.5);
        pthread_cond_timedwait(&g->cond, &g->lock, &ts);
    }
    g->waiting--;
    g->running++;
    if (g->gov) governor_note_start(g->gov);
    pthread_mutex_unlock(&g->lock);
}

/* Platz freigeben und Wartende wecken */
static void gate_release(Gate *g)
{
    pthread_mutex_lock(&g->lock);
    if (g->running > 0) g->running--;
    pthread_cond_broadcast(&g->cond);
    pthread_mutex_unlock(&g->lock);
}

/* Gate freigeben */
static void gate_free(Gate *g)
{
    if (!g) return;
    pthread_mutex_destroy(&g->lock);
    pthread_cond_destroy(&g->cond);
    free(g);
}

/* vom Dispatcher reservierte Slots eines Jobs; release() ist mehrfach aufrufbar */
typedef struct {
    Gate *gates[2];
    int n;
    pthread_mutex_t lock;
} Reservation;

/* alle Plätze einer Reservierung freigeben (mehrfacher Aufruf ist unschädlich) */
static void reservation_release(Reservation *r)
{
    pthread_mutex_lock(&r->lock);
    Gate *g[2] = {r->gates[0], r->gates[1]};
    int n = r->n;
    r->n = 0;
    pthread_mutex_unlock(&r->lock);
    for (int i = 0; i < n; i++) gate_release(g[i]);
}

/* mehrere Gates gemeinsam belegen – alle oder keines -> NULL, wenn eines voll ist */
static Reservation *reservation_try(Gate *const *gates, int n)
{
    Gate *acquired[2];
    int k = 0;
    for (int i = 0; i < n && k < 2; i++) {
        Gate *gate = gates[i];
        bool duplicate = !gate;
        for (int j = 0; j < k; j++) duplicate |= acquired[j] == gate;
        if (duplicate) continue;
        if (!gate_try_acquire(gate)) {
            for (int j = 0; j < k; j++) gate_release(acquired[j]);
            return NULL;
        }
        acquired[k++] = gate;
    }
    Reservation *reservation = xmalloc(sizeof *reservation);
    memset(reservation, 0, sizeof *reservation);
    for (int j = 0; j < k; j++) reservation->gates[j] = acquired[j];
    reservation->n = k;
    pthread_mutex_init(&reservation->lock, NULL);
    return reservation;
}

/* Reservierung freigeben (ihre Plätze müssen schon freigegeben sein) */
static void reservation_free(Reservation *r)
{
    if (!r) return;
    pthread_mutex_destroy(&r->lock);
    free(r);
}

/* "SSD", "HDD" oder "Netz/ZFS" für das Dateisystem von path */
static const char *filesystem_kind(const char *path)
{
    const char *kind = "HDD";                   /* im Zweifel vorsichtig */
    StrList cmd = {0};
    strlist_add_many(&cmd, "findmnt", "-n", "-o", "SOURCE,FSTYPE", "-T", path, NULL);
    Buf out = {0};
    int rc = run_capture_output(&cmd, false, 10, &out, NULL);
    strlist_free(&cmd);
    char src[PATH_MAX], fstype[64];
    if (rc != -1 && out.p && sscanf(out.p, "%4095s %63s", src, fstype) == 2) {
        if (str_in_list(fstype, NETWORK_FS) || starts_with(fstype, "fuse")) {
            kind = "Netz/ZFS";
        } else if (starts_with(src, "/dev/")) {
            src[strcspn(src, "[")] = 0;         /* btrfs: /dev/x[/subvol] */
            buf_clear(&out);
            strlist_add_many(&cmd, "lsblk", "-ndo", "ROTA", src, NULL);
            char rota[8];
            if (run_capture_output(&cmd, false, 10, &out, NULL) != -1 && out.p &&
                sscanf(out.p, "%7s", rota) == 1 && !strcmp(rota, "0"))
                kind = "SSD";
            strlist_free(&cmd);
        }
    }
    buf_free(&out);
    return kind;
}

typedef struct {
    dev_t dev;
    bool valid;
    Gate *gate;
} FsGateEntry;

static FsGateEntry *g_fs_gates;
static int g_fs_gate_count;
static pthread_mutex_t g_fs_lock = PTHREAD_MUTEX_INITIALIZER;

/* vorhandenes I/O-Gate zum Dateisystem dev suchen (g_fs_lock muss gehalten sein) */
static Gate *filesystem_gate_find(bool valid, dev_t dev)
{
    for (int i = 0; i < g_fs_gate_count; i++)
        if (g_fs_gates[i].valid == valid && (!valid || g_fs_gates[i].dev == dev))
            return g_fs_gates[i].gate;
    return NULL;
}

/* ein Gate je Dateisystem (Gerätenummer) */
static Gate *filesystem_gate(const char *path)
{
    struct stat st;
    bool valid = stat(path, &st) == 0;
    dev_t dev = valid ? st.st_dev : 0;
    pthread_mutex_lock(&g_fs_lock);
    Gate *gate = filesystem_gate_find(valid, dev);
    pthread_mutex_unlock(&g_fs_lock);
    if (gate) return gate;
    const char *kind = filesystem_kind(path);
    Gate *new_gate = gate_new(!strcmp(kind, "SSD") ? IO_JOBS_SSD : IO_JOBS_HDD, NULL, kind);
    pthread_mutex_lock(&g_fs_lock);
    gate = filesystem_gate_find(valid, dev);
    if (!gate) {
        g_fs_gates = xrealloc(g_fs_gates, (size_t)(g_fs_gate_count + 1) * sizeof *g_fs_gates);
        g_fs_gates[g_fs_gate_count++] = (FsGateEntry){dev, valid, new_gate};
        gate = new_gate;
        new_gate = NULL;
    }
    pthread_mutex_unlock(&g_fs_lock);
    gate_free(new_gate);
    return gate;
}

/* ---------------------------------------------------------------------------
 * Schnitt: Encoder-Wahl und Durchführung
 * ------------------------------------------------------------------------- */
static void choose_cpu_encoder(const char *ext, const char *src_codec, Encoder *enc)
{
    memset(enc, 0, sizeof *enc);
    /* Läuft sonst nichts mehr (letzte Datei), darf ein CPU-Job mehr Threads nutzen */
    int threads = REENCODE_THREADS;
    pthread_mutex_lock(&g_sched_lock);
    if (g_sched_pending == 0 && g_sched_inflight <= 1) {
        int t = CPU_COUNT * CPU_TARGET / 100;
        threads = t > REENCODE_THREADS ? t : REENCODE_THREADS;
    }
    pthread_mutex_unlock(&g_sched_lock);
    /* WebM erlaubt kein H.264; AV1-Quellen bleiben AV1, wo der Container es kann */
    bool av1 = !strcmp(ext, ".webm") ||
               (src_codec && !strcmp(src_codec, "av1") && str_in_list(ext, AV1_CONTAINERS));
    if (av1) {
        snprintf(enc->name, sizeof enc->name, "libsvtav1");
        snprintf(enc->label, sizeof enc->label, "CPU AV1");
        strlist_add_many(&enc->args, "-crf", SVTAV1_CRF, "-preset", SVTAV1_PRESET,
                         "-svtav1-params", NULL);
        strlist_add_printf(&enc->args, "lp=%d", threads);
    } else {
        snprintf(enc->name, sizeof enc->name, "libx264");
        snprintf(enc->label, sizeof enc->label, "CPU x264");
        strlist_add_many(&enc->args, "-crf", REENCODE_CRF, "-preset", REENCODE_PRESET,
                         "-threads", NULL);
        strlist_add_printf(&enc->args, "%d", threads);
    }
}

/* Codec wie die Quelle – soweit Container und GPU es erlauben */
static bool choose_gpu_encoder(const MediaInfo *info, const char *vcodec, const char *ext,
                               const GpuCaps *caps, Encoder *enc)
{
    if (!caps) return false;
    const char *src_codec = vcodec ? vcodec : "", *order[3] = {"hevc", "h264", NULL};
    static const char *const OLD[] = {".avi", ".flv", ".wmv", ".mpg", ".mpeg", NULL};
    bool src_10bit = info->is_10bit, h264_8 = !strcmp(src_codec, "h264") && !src_10bit;
    if (!strcmp(ext, ".webm")) {                    /* WebM: nur AV1 */
        order[0] = "av1";
        order[1] = NULL;
    } else if (str_in_list(ext, OLD)) {             /* Altcontainer: H.264 */
        order[0] = "h264";
        order[1] = NULL;
    } else if (h264_8) {                            /* auch .ts */
        order[0] = "h264";
        order[1] = "hevc";
    } else if (!strcmp(src_codec, "av1") && strcmp(ext, ".ts") != 0) {
        order[0] = "av1";
        order[1] = "hevc";
        order[2] = "h264";
    }                                               /* sonst: HEVC, 10-Bit-H.264, VP9, MPEG-2 ... */
    for (int i = 0; i < 3 && order[i]; i++) {
        const char *name = order[i];
        if (!caps_has_encoder(caps, name)) continue;
        bool keep10 = src_10bit && strcmp(name, "h264") != 0 && caps_has_10bit(caps, name);
        memset(enc, 0, sizeof *enc);
        snprintf(enc->name, sizeof enc->name, "%s_vaapi", name);
        add_gpu_quality_args(&enc->args, name);
        /* 10 -> 8 Bit auf der GPU */
        snprintf(enc->filter, sizeof enc->filter, "format=nv12|p010|vaapi,hwupload%s",
                 src_10bit && !keep10 ? ",scale_vaapi=format=nv12" : "");
        /* HW-Dekodierung: wechselt mitten im Video der Parametersatz (z. B. bei
         * zusammengeschnittenen Videos), legt der Decoder einen neuen Bildpuffer an
         * und ffmpeg baut die Filterkette neu auf – mit einem Software-Skalierer am
         * Ende, der keine GPU-Bilder verarbeiten kann ("Error while filtering:
         * Function not implemented"). Daher ohne diesen Skalierer (-autoscale 0)
         * und Größe und Format stattdessen auf der GPU festhalten. */
        if (info->width > 0 && info->height > 0)
            snprintf(enc->hw_filter, sizeof enc->hw_filter,
                     "format=nv12|p010|vaapi,hwupload,scale_vaapi=w=%d:h=%d:format=%s", info->width,
                     info->height, keep10 ? "p010" : "nv12");
        if (!strcmp(name, "hevc") && str_in_list(ext, MP4_LIKE))
            snprintf(enc->tag, sizeof enc->tag, "hvc1");
        enc->hw = true;
        enc->hwdecode = caps->hwdecode != 0;
        snprintf(enc->device, sizeof enc->device, "%s", caps->device);
        char upper_name[8];
        size_t k = 0;
        for (; name[k] && k < sizeof upper_name - 1; k++)
            upper_name[k] = (char)toupper((unsigned char)name[k]);
        upper_name[k] = 0;
        snprintf(enc->label, sizeof enc->label, "GPU %s%s", upper_name, keep10 ? " 10 Bit" : "");
        return true;
    }
    return false;
}

/* keep > 0: Outro-Modus (am Ende abschneiden), sonst Intro am ersten Keyframe danach bzw.
 * (keyframe_before, automatisch erkannte Intros) am letzten im Intro */
static bool cut_by_copy(const char *src, const char *target, const MediaInfo *info,
                        double cut_len, double keep, bool keyframe_before, const char *ext,
                        const StreamPlan *plan, Job *job, double *cut_at, char *reason,
                        size_t reason_size, char *event, size_t event_size)
{
    event[0] = reason[0] = 0;
    StrList cmd = {0};
    double kf_rel = cut_len, expected = keep;
    if (keep > 0) {
        build_outro_copy_cmd(&cmd, src, target, plan, keep);
    } else {
        double kf_dts_rel, seek_pad = 0.25;
        if (keyframe_before
                ? !find_keyframe_before(src, info, cut_len, &kf_rel, &kf_dts_rel, &seek_pad)
                : !find_cut_keyframe(src, info, cut_len, ext, &kf_rel, &kf_dts_rel)) {
            snprintf(reason, reason_size, keyframe_before
                                              ? "kein Keyframe im Intro – Copy nähme Inhalt mit"
                                              : "kein Keyframe nach dem Intro gefunden");
            return false;
        }
        build_copy_cmd(&cmd, src, target, plan, kf_rel, kf_dts_rel, seek_pad, ext);
        expected = info->duration ? info->duration - kf_rel : 0;
    }
    job_set_phase(job, "Copy");
    Buf err = {0};
    int rc = run_ffmpeg_watched(&cmd, job, expected, NULL, false, false, event, event_size, &err);
    strlist_free(&cmd);
    bool ok = false;
    if (rc != 0 || !file_not_empty(target)) {
        if (event[0]) watchdog_event_text(event, reason, reason_size);
        else last_error_line(err.p, reason, reason_size);
    } else {
        ok = validate_cut(info, src, target, kf_rel, keep > 0, reason, reason_size);
        *cut_at = kf_rel;
    }
    buf_free(&err);
    return ok;
}

/* Schnitt mit Neukodierung (CPU oder GPU) -> true, wenn das Ergebnis die Prüfung besteht */
static bool cut_by_encode(const char *src, const char *target, const MediaInfo *info,
                          double cut_len, double keep, const char *ext, const StreamPlan *plan,
                          const Encoder *encoder, Job *job, const PokeTarget *poke, char *reason,
                          size_t reason_size, char *event, size_t event_size)
{
    event[0] = reason[0] = 0;
    job_set_phase(job, encoder->label);
    StrList cmd = {0};
    build_encode_cmd(&cmd, src, target, plan, cut_len, keep, ext, encoder);
    Buf err = {0};
    int rc = run_ffmpeg_watched(&cmd, job, info->duration ? info->duration - cut_len : 0, poke,
                                encoder->hw, encoder->low_prio, event, event_size, &err);
    strlist_free(&cmd);
    bool ok = false;
    if (rc != 0 || !file_not_empty(target)) {
        if (event[0]) watchdog_event_text(event, reason, reason_size);
        else last_error_line(err.p, reason, reason_size);
    } else {
        ok = validate_cut(info, src, target, cut_len, keep > 0, reason, reason_size);
    }
    buf_free(&err);
    return ok;
}

typedef struct {
    bool success;
    double cut_at;          /* tatsächlich abgeschnitten (s) */
    char method[48];        /* Schnittmethode, "-" bei Fehler */
    char *note;
} CutResult;

/* Schneidet die ersten (mit outro: die letzten) cut_len Sekunden weg. slot: vom
 * Dispatcher reservierter Platz (I/O bei Copy, sonst GPU/CPU) – wird spätestens am
 * Ende freigegeben. low_prio_cpu: CPU-Encodes mit nice 19 (Modus 4 – GPU-Jobs haben
 * Vorrang). */
static void cut_video(const char *src, const char *target, double cut_len, bool outro,
                      bool keyframe_before, const char *mode, Reservation *slot, Gate *cpu_gate,
                      Gate *gpu_gate, const GpuCaps *gpu, Job *job, const char *rel,
                      bool low_prio_cpu, CutResult *result)
{
    memset(result, 0, sizeof *result);
    snprintf(result->method, sizeof result->method, "-");
    char ext[32], reason[512], event[64], event_msg[256];
    path_extension(src, ext, sizeof ext);
    MediaInfo info;
    if (!probe_media(src, &info) || info.main_video < 0) {
        reservation_release(slot);
        media_info_free(&info);
        result->note = xstrdup("nicht lesbar oder kein Video");
        return;
    }
    StreamPlan plan;
    plan_streams(src, &info, ext, target, &plan);
    StrList notes = {0};
    strlist_add_list(&notes, &plan.notes);
    const char *attempts[2];
    int n_attempts = 0;
    double keep = 0;                                /* Outro: so viel bleibt stehen */
    if (outro) {
        keep = info.duration - cut_len;
        if (info.duration <= 0 || keep <= 0) {
            reservation_release(slot);
            strlist_add(&notes, info.duration <= 0 ? "Dauer unbekannt – Outro nicht abschneidbar"
                                         : "Video ist nicht länger als das Outro");
            goto done;
        }
    }
    if (!strcmp(mode, "copy")) {
        double cut_at = 0;
        bool ok = cut_by_copy(src, target, &info, cut_len, keep, keyframe_before, ext, &plan,
                              job, &cut_at, reason, sizeof reason, event, sizeof event);
        reservation_release(slot);                  /* I/O-Slot sofort freigeben */
        if (event[0])
            ic_log(true, "  Watchdog: %s Copy %s", rel,
                   watchdog_event_text(event, event_msg, sizeof event_msg));
        if (ok) {
            result->success = true;
            result->cut_at = cut_at;
            snprintf(result->method, sizeof result->method, "Copy");
            goto done;
        }
        strlist_add_printf(&notes, "Copy verworfen: %s", reason);
        remove_file(target);
        if (gpu) attempts[n_attempts++] = "gpu";
        attempts[n_attempts++] = "cpu";
    } else if (!strcmp(mode, "gpu")) {
        attempts[n_attempts++] = "gpu";
        attempts[n_attempts++] = "cpu";
    } else {
        attempts[n_attempts++] = "cpu";
    }

    bool reserved = strcmp(mode, "copy") != 0;      /* 1. Versuch hat schon einen Slot */
    for (int n = 0; n < n_attempts && !ic_cancel; n++) {
        bool owns_slot = reserved && n == 0, use_gpu = !strcmp(attempts[n], "gpu");
        if (use_gpu && gpu_is_disabled()) {
            if (owns_slot) reservation_release(slot);
            strlist_add(&notes, "GPU für diesen Lauf deaktiviert");
            continue;
        }
        Encoder encoder;
        bool have = true;
        if (use_gpu)
            have = choose_gpu_encoder(&info, info.video_codec, ext, gpu, &encoder);
        else
            choose_cpu_encoder(ext, info.video_codec, &encoder);
        if (!use_gpu && low_prio_cpu) encoder.low_prio = true;
        if (!have) {
            if (owns_slot) reservation_release(slot);
            strlist_add(&notes, "GPU: kein passender Encoder");
            continue;
        }
        PokeTarget poke = {encoder.device, gpu};
        Gate *gate = use_gpu ? gpu_gate : cpu_gate;
        if (!owns_slot) gate_acquire(gate);
        bool ok = false;
        if (!ic_cancel) {
            ok = cut_by_encode(src, target, &info, cut_len, keep, ext, &plan, &encoder, job,
                               encoder.hw ? &poke : NULL, reason, sizeof reason, event,
                               sizeof event);
            if (event[0])
                ic_log(true, "  Watchdog: %s %s %s", rel, encoder.label,
                       watchdog_event_text(event, event_msg, sizeof event_msg));
            bool hung = watchdog_event_is_hang(event);
            if (hung && encoder.hw) gpu_note_hang(rel);
            if (!ok && !hung && encoder.hw && encoder.hwdecode && !ic_cancel) {
                /* z. B. 10-Bit-H.264/MPEG-4: GPU kann es nicht dekodieren ->
                 * Software-Dekodierung + Upload, Encoder bleibt auf der GPU */
                remove_file(target);
                strlist_add_printf(&notes, "%s mit HW-Dekodierung verworfen: %s",
                                   encoder.label, reason);
                encoder.hwdecode = false;
                ok = cut_by_encode(src, target, &info, cut_len, keep, ext, &plan, &encoder, job,
                                   &poke,
                                   reason, sizeof reason, event, sizeof event);
                if (event[0]) {
                    ic_log(true, "  Watchdog: %s %s (SW-Decode) %s", rel, encoder.label,
                           watchdog_event_text(event, event_msg, sizeof event_msg));
                    if (watchdog_event_is_hang(event)) gpu_note_hang(rel);
                }
            }
        }
        if (owns_slot) reservation_release(slot);
        else gate_release(gate);
        if (ok) {
            if (event[0])
                strlist_add_printf(&notes, "%s %s", encoder.label,
                                   watchdog_event_text(event, event_msg, sizeof event_msg));
            result->success = true;
            result->cut_at = cut_len;
            snprintf(result->method, sizeof result->method, "%s", encoder.label);
            strlist_free(&encoder.args);
            goto done;
        }
        strlist_add_printf(&notes, "%s verworfen: %s", encoder.label, reason);
        remove_file(target);
        strlist_free(&encoder.args);
    }
done:
    result->note = strlist_join(&notes, "; ");
    reservation_release(slot);
    for (int i = 0; i < plan.temp_files.n; i++) {
        remove_file(plan.temp_files.v[i]);
        temp_unregister(plan.temp_files.v[i]);
    }
    stream_plan_free(&plan);
    strlist_free(&notes);
    media_info_free(&info);
}

/* ---------------------------------------------------------------------------
 * GPU (VAAPI) erkennen und selbst testen
 * ------------------------------------------------------------------------- */
static bool gpu_encoder_works(const char *dev, const char *name, const char *pix_fmt)
{
    StrList cmd = {0};
    strlist_add_many(&cmd, "ffmpeg", "-nostdin", "-v", "error", "-init_hw_device", NULL);
    strlist_add_printf(&cmd, "vaapi=va:%s", dev);
    strlist_add_many(&cmd, "-filter_hw_device", "va", "-f", "lavfi", "-i",
                     "testsrc2=size=320x240:rate=25", "-t", "0.4", "-vf", NULL);
    strlist_add_printf(&cmd, "format=%s,hwupload", pix_fmt);
    strlist_add(&cmd, "-c:v");
    strlist_add_printf(&cmd, "%s_vaapi", name);
    add_gpu_quality_args(&cmd, name);
    strlist_add_many(&cmd, "-f", "null", "-", NULL);
    int rc = run_ffmpeg(&cmd, 60, NULL, NULL);
    strlist_free(&cmd);
    return rc == 0;
}

/* kurzes Testvideo für den GPU-Selbsttest anlegen (einmalig, im Datenordner) */
static bool ensure_selftest_clip(void)
{
    if (file_not_empty(SELFTEST_CLIP)) return true;
    mkdirs(APP_DIR);
    StrList cmd = {0};
    strlist_add_many(&cmd, "ffmpeg", "-nostdin", "-y", "-v", "error",
                     "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=25",
                     "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000",
                     "-t", "3", "-c:v", "libx264", "-preset", "ultrafast", "-g", "25",
                     "-c:a", "aac", SELFTEST_CLIP, NULL);
    int rc = run_ffmpeg(&cmd, 120, NULL, NULL);
    strlist_free(&cmd);
    return rc == 0 && file_not_empty(SELFTEST_CLIP);
}

/* Kompletter Pfad (Dekodieren, Filterkette, Encoder, Prüfung) am Testclip */
static bool gpu_pipeline_works(const char *clip, GpuCaps *caps)
{
    MediaInfo info;
    if (!probe_media(clip, &info)) {
        media_info_free(&info);
        return false;
    }
    char *out = xasprintf("%s/.gpu_selftest_%d.mp4", APP_DIR, (int)getpid());
    bool ok_all = true;
    for (const char *const *n = GPU_CODECS; *n && !ic_cancel; n++) {
        if (!caps_has_encoder(caps, *n)) continue;
        GpuCaps single;
        memset(&single, 0, sizeof single);
        snprintf(single.device, sizeof single.device, "%s", caps->device);
        single.hwdecode = caps->hwdecode;
        caps_set_encoder(&single, *n, true);
        Encoder encoder;
        StreamPlan plan;
        char reason[512] = "", event[64] = "", event_msg[256];
        bool have = choose_gpu_encoder(&info, *n, ".mkv", &single, &encoder);
        plan_streams(clip, &info, ".mp4", out, &plan);
        bool ok = have &&
                  cut_by_encode(clip, out, &info, 1.0, 0, ".mp4", &plan, &encoder, NULL, NULL,
                                reason, sizeof reason, event, sizeof event);
        remove_file(out);
        ic_log(false, "  GPU-Selbsttest %s (HW-Decode %s): %s%s%s%s", *n,
               single.hwdecode == 1 ? "ja" : "nein", ok ? "ok" : "fehlgeschlagen: ",
               ok ? "" : reason, event[0] ? " – " : "",
               event[0] ? watchdog_event_text(event, event_msg, sizeof event_msg) : "");
        if (!ok) {
            caps_set_encoder(caps, *n, false);
            ok_all = false;
        }
        stream_plan_free(&plan);
        if (have) strlist_free(&encoder.args);
    }
    free(out);
    media_info_free(&info);
    return ok_all || caps_has_any(caps);
}

/* VAAPI-Geräte prüfen: welche Encoder funktionieren tatsächlich (Selbsttest) -> false ohne GPU */
static bool gpu_detect(GpuCaps *out)
{
    if (!GPU_ENABLE || !command_exists("ffmpeg")) return false;
    StrList devs = {0};
    if (VAAPI_DEVICE) {
        strlist_add(&devs, VAAPI_DEVICE);
    } else {
        glob_t glob_result;
        if (glob("/dev/dri/renderD*", 0, NULL, &glob_result) == 0)
            for (size_t i = 0; i < glob_result.gl_pathc; i++)
                strlist_add(&devs, glob_result.gl_pathv[i]);
        globfree(&glob_result);
    }
    bool found = false;
    for (int d = 0; d < devs.n && !found && !ic_cancel; d++) {
        const char *device = devs.v[d];
        if (!*device || access(device, F_OK) != 0) continue;
        GpuCaps caps;
        memset(&caps, 0, sizeof caps);
        snprintf(caps.device, sizeof caps.device, "%s", device);
        caps.hwdecode = -1;
        for (const char *const *n = GPU_CODECS; *n; n++) {
            bool ok = gpu_encoder_works(device, *n, "nv12");
            caps_set_encoder(&caps, *n, ok);
            if (ok && !strcmp(*n, "hevc")) caps.hevc10 = gpu_encoder_works(device, *n, "p010");
            if (ok && !strcmp(*n, "av1")) caps.av110 = gpu_encoder_works(device, *n, "p010");
        }
        if (!caps_has_any(&caps)) continue;
        if (ensure_selftest_clip()) {
            GpuCaps base = caps;
            caps.hwdecode = 1;
            if (!gpu_pipeline_works(SELFTEST_CLIP, &caps)) {
                caps = base;                        /* Software-Dekodierung + Upload */
                caps.hwdecode = 0;
                if (!gpu_pipeline_works(SELFTEST_CLIP, &caps)) continue;
            }
        } else {
            caps.hwdecode = 0;
        }
        if (caps_has_any(&caps)) {
            *out = caps;
            found = true;
        }
    }
    strlist_free(&devs);
    return found;
}

/* GPU-Fähigkeiten als lesbarer Text anhängen */
static void gpu_summary(const GpuCaps *caps, Buf *b)
{
    if (!caps) {
        buf_add_str(b, "GPU (VAAPI): nicht verfügbar – Neukodieren nur per CPU.");
        return;
    }
    static const char *const LABEL[][2] = {{"h264", "H.264"}, {"hevc", "HEVC"}, {"av1", "AV1"}};
    buf_printf(b, "GPU (VAAPI, %s): ", caps->device);
    bool first = true;
    for (int i = 0; i < 3; i++) {
        if (!caps_has_encoder(caps, LABEL[i][0])) continue;
        buf_printf(b, "%s%s%s", first ? "" : ", ", LABEL[i][1],
                   caps_has_10bit(caps, LABEL[i][0]) ? " (10 Bit)" : "");
        first = false;
    }
    buf_printf(b, " – HW-Dekodierung: %s", caps->hwdecode == 1 ? "ja" : "nein");
}

/* ---------------------------------------------------------------------------
 * Verschieben & Speicherplatz
 * ------------------------------------------------------------------------- */
static double free_space_gb(const char *path)
{
    struct statvfs st;
    if (statvfs(path, &st) != 0) return INFINITY;
    return (double)st.f_bavail * (double)st.f_frsize / (1024.0 * 1024.0 * 1024.0);
}

/* Datei kopieren inkl. Rechten und Zeitstempeln (wie copyfileobj + copystat);
 * die Daten werden mit fsync auf den Datenträger geschrieben */
static int copy_file(const char *from, const char *to, void (*on_progress)(void *, long long),
                     void *progress_ctx)
{
    int in_fd = open(from, O_RDONLY | O_CLOEXEC), saved_errno = 0, rc = 0;
    if (in_fd < 0) return -1;
    struct stat st;
    int out_fd = fstat(in_fd, &st) == 0
                     ? open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666)
                     : -1;
    if (out_fd < 0) {
        saved_errno = errno;
        close(in_fd);
        errno = saved_errno;
        return -1;
    }
    enum { CHUNK = 1 << 20 };
    char *buf = xmalloc(CHUNK);
    long long copied = 0;
    while (rc == 0) {
        ssize_t r = read(in_fd, buf, CHUNK);
        if (r == 0) break;
        if (r < 0) {
            if (errno == EINTR) continue;
            rc = -1;
            saved_errno = errno;
            break;
        }
        for (ssize_t written = 0; written < r;) {
            ssize_t w = write(out_fd, buf + written, (size_t)(r - written));
            if (w < 0 && errno == EINTR) continue;
            if (w < 0) {
                rc = -1;
                saved_errno = errno;
                break;
            }
            written += w;
        }
        copied += r;
        if (rc == 0 && on_progress) on_progress(progress_ctx, copied);
    }
    free(buf);
    if (rc == 0 && fsync(out_fd) != 0) {
        rc = -1;
        saved_errno = errno;
    }
    if (rc == 0) {
        fchmod(out_fd, st.st_mode & 07777);
        struct timespec ts[2] = {st.st_atim, st.st_mtim};
        futimens(out_fd, ts);
    }
    if (close(out_fd) != 0 && rc == 0) {
        rc = -1;
        saved_errno = errno;
    }
    close(in_fd);
    errno = saved_errno;
    return rc;
}

/* new an den Platz von dst setzen – dst wird dabei NIE vorab gelöscht -> 0/-1:
 *   * Gleiches Dateisystem: atomares Rename.
 *   * Nur bei EXDEV (anderes Dateisystem): Kopie in eine Temp-Datei neben dst,
 *     fsync, dann atomar über dst; erst danach wird new gelöscht. on_progress
 *     (darf NULL sein) erfährt dabei laufend die schon kopierten Byte.
 *   * Bei jedem anderen Fehler bleibt dst unverändert; halb kopierte Dateien
 *     werden entfernt (errno bleibt gesetzt). */
static int replace_file_reporting(const char *new, const char *dst,
                                  void (*on_progress)(void *, long long), void *progress_ctx)
{
    if (!is_file(new)) {                            /* fertige Datei fehlt */
        errno = ENOENT;
        return -1;
    }
    if (rename(new, dst) == 0) return 0;
    if (errno != EXDEV) return -1;
    char *dir = path_dirname(dst), *name = xasprintf(".introcut_copy_%d_%s", (int)getpid(),
                                                     path_basename(dst));
    char *tmp = path_join(*dir ? dir : ".", name);
    free(dir);
    free(name);
    temp_register(tmp);
    int rc = copy_file(new, tmp, on_progress, progress_ctx);
    if (rc == 0) rc = rename(tmp, dst);             /* gleiches Dateisystem wie dst -> atomar */
    int e = errno;
    if (rc != 0) remove_file(tmp);                  /* halb kopierte Datei wegräumen */
    temp_unregister(tmp);
    free(tmp);
    if (rc != 0) {
        errno = e;
        return -1;
    }
    unlink(new);                                    /* Kopie liegt sicher am Ziel */
    return 0;
}

/* new an den Platz von dst setzen (siehe replace_file_reporting) */
static int replace_file(const char *new, const char *dst)
{
    return replace_file_reporting(new, dst, NULL, NULL);
}

/* ---------------------------------------------------------------------------
 * Restdauer der Bearbeitung
 * Gemessen wird, wie viel Arbeit im Lauf der Zeit (ohne Pausen) erledigt ist: fertige
 * Dateien ganz, laufende nach ihrem Fortschritt. Die Arbeit an einer Datei hat einen festen
 * Teil (Analyse, Prüfung) und einen, der mit ihrer Größe wächst – wie sie sich verteilen,
 * zeigen die fertigen Dateien (Dauer ≈ fest + je GB; bei Copy zählt der feste Teil viel,
 * beim Neukodieren fast nur die Größe). Das Tempo stammt aus den letzten ETA_WINDOW_SECS.
 * Gegen Ende laufen nicht mehr alle Plätze parallel: sind nur noch wenige Dateien offen,
 * werden sie deshalb nach diesem Modell einzeln auf die belegten Plätze verplant – vor dem
 * so geplanten letzten Job kann der Lauf nicht fertig sein. Das geschätzte Ende wird
 * geglättet, damit die Anzeige ruhig herunterzählt, statt bei jeder fertigen Datei zu
 * springen.
 * ------------------------------------------------------------------------- */
#define ETA_SAMPLE_SECS 2           /* höchstens so oft ein Messpunkt */
#define ETA_WINDOW_SECS 600         /* Tempo aus den letzten 10 Minuten */
#define ETA_WARMUP_SECS 20.0        /* vorher gibt es keine Schätzung */
#define ETA_SMOOTH_SHARE 0.15       /* das Ende folgt neuen Schätzungen mit einer Zeitkonstante */
#define ETA_SMOOTH_MAX_SECS 60.0    /* von 15 % der Restdauer, höchstens einer Minute */
#define ETA_FIXED_PRIOR_SECS 1.0    /* Annahme für den festen Aufwand je Datei; sie zählt wie */
#define ETA_PRIOR_FILES 3.0         /* so viele Dateien, bis die Messung selbst genug zeigt */
#define ETA_TAIL_FILES 64           /* so wenige offene Dateien: das Ende Datei für Datei planen */
#define ETA_MAX_JOBS 256            /* laufende Jobs, die die Planung berücksichtigt */
#define ETA_SAMPLES (ETA_WINDOW_SECS / ETA_SAMPLE_SECS + 2)
#define ETA_LOG_FIRST_SECS 120      /* Terminal: erste Zeile mit der Restdauer nach 2 Minuten, */
#define ETA_LOG_SECS 300            /* danach alle 5 Minuten */

typedef struct {
    double t;                       /* Zeitpunkt (Uhr ohne Pausen) */
    double bytes, files;            /* bis dahin erledigt, laufende Dateien anteilig */
} EtaSample;

typedef struct {
    bool active;                    /* eine Bearbeitung läuft */
    double started;                 /* ihr Beginn (Uhr ohne Pausen) */
    double total_bytes, done_bytes; /* alle bzw. fertige Dateien (auch gescheiterte) */
    int total_files, done_files;
    double sum_s, sum_d, sum_ss, sum_sd;    /* erfolgreiche Dateien: Größe s (GB), Dauer d (s) */
    int measured;                   /* ihre Anzahl */
    EtaSample samples[ETA_SAMPLES]; /* Ring der Messpunkte, der älteste bei first */
    int first, count;
    double end;                     /* geglättetes Ende (Uhr ohne Pausen), NAN = unbekannt */
    double fraction;                /* erledigter Anteil beim letzten Messpunkt */
} EtaState;

static pthread_mutex_t g_eta_lock = PTHREAD_MUTEX_INITIALIZER;
static EtaState g_eta;

/* Schätzung für eine neue Bearbeitung beginnen: files Dateien mit zusammen total_bytes Byte */
static void eta_begin(int files, double total_bytes, double now)
{
    pthread_mutex_lock(&g_eta_lock);
    memset(&g_eta, 0, sizeof g_eta);
    g_eta.active = true;
    g_eta.started = now;
    g_eta.total_files = files;
    g_eta.total_bytes = total_bytes;
    g_eta.end = NAN;
    pthread_mutex_unlock(&g_eta_lock);
}

/* Bearbeitung zu Ende: keine Schätzung mehr */
static void eta_finish(void)
{
    pthread_mutex_lock(&g_eta_lock);
    g_eta.active = false;
    pthread_mutex_unlock(&g_eta_lock);
}

/* eine Datei ist fertig: bytes groß, ok: erfolgreich in secs (ohne Pausen) bearbeitet */
static void eta_file_done(double bytes, bool ok, double secs)
{
    pthread_mutex_lock(&g_eta_lock);
    g_eta.done_files++;
    g_eta.done_bytes += bytes;
    if (ok && secs > 0) {                       /* gescheiterte sagen über die Dauer nichts */
        double gb = bytes / 1e9;
        g_eta.sum_s += gb;
        g_eta.sum_d += secs;
        g_eta.sum_ss += gb * gb;
        g_eta.sum_sd += gb * secs;
        g_eta.measured++;
    }
    pthread_mutex_unlock(&g_eta_lock);
}

/* Dauer einer Datei ≈ fixed + per_gb · GB, aus den fertigen Dateien: Ausgleichsgerade, deren
 * fester Teil zu ETA_FIXED_PRIOR_SECS hin gezogen wird – sonst ließe er sich nicht bestimmen,
 * solange die fertigen Dateien ähnlich groß sind (die größten kommen zuerst dran)
 * -> false, solange weniger als zwei Dateien fertig sind */
static bool eta_model(const EtaState *e, double *fixed, double *per_gb)
{
    if (e->measured < 2) return false;
    /* minimiert Summe (d - fest - je_gb·s)² + ETA_PRIOR_FILES·(fest - ETA_FIXED_PRIOR_SECS)² */
    double n = e->measured + ETA_PRIOR_FILES;
    double sum_d = e->sum_d + ETA_PRIOR_FILES * ETA_FIXED_PRIOR_SECS;
    double det = n * e->sum_ss - e->sum_s * e->sum_s;
    if (det <= 0) return false;
    *fixed = (sum_d * e->sum_ss - e->sum_s * e->sum_sd) / det;
    *per_gb = (n * e->sum_sd - e->sum_s * sum_d) / det;
    if (*per_gb < 0) {                          /* Dauer wächst nicht mit der Größe */
        *per_gb = 0;
        *fixed = sum_d / n;
    } else if (*fixed < 0) {                    /* kein fester Teil erkennbar */
        *fixed = 0;
        *per_gb = e->sum_sd / e->sum_ss;
    }
    return true;
}

/* Anteil der Größe an der Arbeit (0..1) nach dem Modell; ohne Modell zählt nur die Größe */
static double eta_size_weight(const EtaState *e)
{
    double fixed, per_gb;
    if (e->total_bytes <= 0) return 0;
    if (!eta_model(e, &fixed, &per_gb)) return 1;
    double by_size = per_gb * e->total_bytes / 1e9, by_count = fixed * e->total_files;
    return by_size + by_count > 0 ? by_size / (by_size + by_count) : 1;
}

typedef struct {
    double share;                   /* erledigter Anteil 0..1 */
    double busy;                    /* Zeit seit Beginn der aktuellen Phase (s) */
    double bytes;
} EtaJob;

/* Zeit bis zum Ende des letzten Jobs, wenn die offenen Dateien (Größen in der Reihenfolge der
 * Abarbeitung) nacheinander auf den jeweils zuerst frei werdenden der belegten Plätze kommen;
 * Dauer je Datei nach dem Modell, laufende Jobs nach ihrem eigenen Tempo */
static double eta_plan_tail(const EtaJob *jobs, int njobs, const double *pending, int npending,
                            double fixed, double per_gb)
{
    double left[ETA_MAX_JOBS], longest = 0;
    for (int k = 0; k < njobs; k++) {
        const EtaJob *j = &jobs[k];
        if (j->share >= 0.05 && j->busy >= 5)
            left[k] = j->busy * (1 - j->share) / j->share;
        else
            left[k] = fmax(0, fixed + per_gb * j->bytes / 1e9 - j->busy);
    }
    for (int i = 0; i < npending; i++) {
        int first_free = 0;
        for (int k = 1; k < njobs; k++)
            if (left[k] < left[first_free]) first_free = k;
        left[first_free] += fixed + per_gb * pending[i] / 1e9;
    }
    for (int k = 0; k < njobs; k++) longest = fmax(longest, left[k]);
    return longest;
}

/* erledigter Anteil der Arbeit (0..1) an einem Messpunkt */
static double eta_work(const EtaState *e, const EtaSample *s, double size_weight)
{
    double by_size = e->total_bytes > 0 ? s->bytes / e->total_bytes : 0;
    double by_count = e->total_files > 0 ? s->files / e->total_files : 0;
    return size_weight * by_size + (1 - size_weight) * by_count;
}

/* Messpunkt nehmen (höchstens alle ETA_SAMPLE_SECS; in Pausen steht die Uhr) und die
 * Schätzung erneuern – ruft der Dispatcher regelmäßig auf. pending: Größen der noch nicht
 * begonnenen Dateien in der Reihenfolge der Abarbeitung. */
static void eta_sample(double now, const double *pending, int npending)
{
    EtaJob jobs[ETA_MAX_JOBS];
    int njobs = 0;
    double running_bytes = 0, running_files = 0, slowest_left = 0;
    double projected_secs = 0, projected_gb = 0;    /* Tempo der laufenden Jobs */
    int projected_jobs = 0;
    pthread_mutex_lock(&g_jobs_lock);
    for (Job *j = g_jobs_head; j; j = j->next) {
        double share = fmin(1, fmax(0, j->pct / 100)), busy = now - j->phase_since;
        running_bytes += share * j->bytes;
        running_files += share;
        if (share >= 0.05 && busy >= 5) {       /* Rest dieses Jobs bei gleichem Tempo */
            slowest_left = fmax(slowest_left, busy * (1 - share) / share);
            projected_secs += busy / share;
            projected_gb += j->bytes / 1e9;
            projected_jobs++;
        }
        if (njobs < ETA_MAX_JOBS) jobs[njobs++] = (EtaJob){share, busy, j->bytes};
    }
    pthread_mutex_unlock(&g_jobs_lock);
    pthread_mutex_lock(&g_eta_lock);
    EtaState *e = &g_eta;
    const EtaSample *last = e->count ? &e->samples[(e->first + e->count - 1) % ETA_SAMPLES]
                                     : NULL;
    if (!e->active || (last && now - last->t < ETA_SAMPLE_SECS)) {
        pthread_mutex_unlock(&g_eta_lock);
        return;
    }
    double since_last = last ? now - last->t : 0;
    if (e->count == ETA_SAMPLES) {              /* Ring voll: den ältesten überschreiben */
        e->first = (e->first + 1) % ETA_SAMPLES;
        e->count--;
    }
    EtaSample *newest = &e->samples[(e->first + e->count++) % ETA_SAMPLES];
    *newest = (EtaSample){now, e->done_bytes + running_bytes, e->done_files + running_files};
    while (e->count > 2 && now - e->samples[e->first].t > ETA_WINDOW_SECS) {
        e->first = (e->first + 1) % ETA_SAMPLES;
        e->count--;
    }
    const EtaSample *oldest = &e->samples[e->first];
    double weight = eta_size_weight(e), done = eta_work(e, newest, weight);
    double gained = done - eta_work(e, oldest, weight), span = now - oldest->t;
    e->fraction = fmin(1, fmax(0, done));
    /* wenige offene Dateien: das Ende Datei für Datei planen (Modell aus den fertigen Dateien,
     * vorher aus dem Tempo der laufenden) */
    double fixed = ETA_FIXED_PRIOR_SECS, per_gb = 0, planned = 0;
    bool model = eta_model(e, &fixed, &per_gb);
    if (!model && projected_gb > 0) {
        per_gb = fmax(0, projected_secs - fixed * projected_jobs) / projected_gb;
        model = true;
    }
    if (model && njobs && npending <= ETA_TAIL_FILES)
        planned = eta_plan_tail(jobs, njobs, pending, npending, fixed, per_gb);
    if (now - e->started >= ETA_WARMUP_SECS && span >= ETA_WARMUP_SECS / 2 && gained > 1e-9) {
        double by_rate = fmax(0, 1 - done) * span / gained;
        double estimated_end = now + fmax(by_rate, fmax(slowest_left, planned));
        if (isnan(e->end)) {
            e->end = estimated_end;
        } else {                                /* neuer Schätzung nur nach und nach folgen */
            double tau = fmin(ETA_SMOOTH_MAX_SECS, fmax(1, ETA_SMOOTH_SHARE * (e->end - now)));
            e->end += (estimated_end - e->end) * (1 - exp(-since_last / tau));
        }
    }
    pthread_mutex_unlock(&g_eta_lock);
}

/* Restdauer gerundet: "weniger als 1 Min.", "ca. 7 Min.", ab 10 Minuten in 5-Minuten-
 * Schritten ("ca. 25 Min.", "ca. 2 Std. 10 Min."), ab 10 Stunden in Stunden, ab 2 Tagen
 * in Tagen und Stunden */
static void format_remaining(double secs, char *out, size_t size)
{
    long minutes = lround(secs / 60);
    if (minutes >= 10) minutes = lround(secs / 300) * 5;
    long hours = lround(secs / 3600), days = hours / 24;
    if (secs < 60)
        snprintf(out, size, "weniger als 1 Min.");
    else if (minutes < 60)
        snprintf(out, size, "ca. %ld Min.", minutes);
    else if (minutes < 600 && minutes % 60)
        snprintf(out, size, "ca. %ld Std. %ld Min.", minutes / 60, minutes % 60);
    else if (minutes < 600)
        snprintf(out, size, "ca. %ld Std.", minutes / 60);
    else if (hours < 48)
        snprintf(out, size, "ca. %ld Std.", hours);
    else if (hours % 24)
        snprintf(out, size, "ca. %ld Tage %ld Std.", days, hours % 24);
    else
        snprintf(out, size, "ca. %ld Tage", days);
}

/* Beginn (Mitternacht, Ortszeit) des Tags days Tage nach today */
static time_t start_of_day(const struct tm *today, int days)
{
    struct tm midnight = *today;
    midnight.tm_hour = midnight.tm_min = midnight.tm_sec = 0;
    midnight.tm_mday += days;                   /* mktime rechnet über Monatsgrenzen */
    midnight.tm_isdst = -1;
    return mktime(&midnight);
}

/* Uhrzeit, zu der die Arbeit fertig sein dürfte: "gegen 23:40", "morgen gegen 03:10" bzw.
 * "am 14.10. gegen 03:10" – ab 10 Minuten Restdauer auf 5 Minuten gerundet */
static void format_end_clock(double remaining, char *out, size_t size)
{
    time_t now = time(NULL);
    double step = remaining >= 600 ? 300 : 60;
    time_t end = (time_t)(llround(((double)now + remaining) / step) * step);
    struct tm today, then;
    localtime_r(&now, &today);
    localtime_r(&end, &then);
    char clock[16];
    strftime(clock, sizeof clock, "%H:%M", &then);
    if (end < start_of_day(&today, 1))
        snprintf(out, size, "gegen %s", clock);
    else if (end < start_of_day(&today, 2))
        snprintf(out, size, "morgen gegen %s", clock);
    else
        snprintf(out, size, "am %d.%d. gegen %s", then.tm_mday, then.tm_mon + 1, clock);
}

/* Restdauer der laufenden Bearbeitung (siehe ic_core.h) */
bool ic_process_estimate(IcEstimate *out)
{
    double now = active_clock();
    bool paused = ic_paused();
    pthread_mutex_lock(&g_eta_lock);
    bool active = g_eta.active;
    out->fraction = g_eta.fraction;
    out->remaining = !active || isnan(g_eta.end) ? -1 : fmax(0, g_eta.end - now);
    out->elapsed = active ? now - g_eta.started : 0;
    out->paused = paused;
    pthread_mutex_unlock(&g_eta_lock);
    return active;
}

/* Restdauer als Text (siehe ic_core.h) */
void ic_format_estimate(const IcEstimate *e, bool with_clock, char *out, size_t size)
{
    char left[64], clock[64];
    if (e->remaining < 0) {
        snprintf(out, size, "%s", e->paused ? "pausiert" : "wird ermittelt …");
        return;
    }
    format_remaining(e->remaining, left, sizeof left);
    if (e->paused) {
        snprintf(out, size, "%s (pausiert)", left);
    } else if (!with_clock || e->remaining < 60) {
        snprintf(out, size, "%s", left);
    } else {
        format_end_clock(e->remaining, clock, sizeof clock);
        snprintf(out, size, "%s · fertig %s", left, clock);
    }
}

/* Terminal: Stand und Restdauer als Zeile (sobald es eine Schätzung gibt) */
static void log_estimate(int done, int total)
{
    IcEstimate e;
    char text[160];
    if (!ic_fe.show_heartbeat || !ic_process_estimate(&e) || e.remaining < 0) return;
    ic_format_estimate(&e, true, text, sizeof text);
    ic_log(true, "  … %d von %d Dateien fertig · Restdauer %s", done, total, text);
}

/* ---------------------------------------------------------------------------
 * Verarbeitung
 * ------------------------------------------------------------------------- */
/* eine erfolgreich geschnittene Datei für die Methode zählen (neue Methoden hinten anfügen) */
static void stats_count_method(Stats *stats, const char *method)
{
    for (int i = 0; i < stats->method_count; i++)
        if (!strcmp(stats->methods[i].method, method)) {
            stats->methods[i].files++;
            return;
        }
    if (stats->method_count < (int)(sizeof stats->methods / sizeof *stats->methods)) {
        MethodCount *entry = &stats->methods[stats->method_count++];
        snprintf(entry->method, sizeof entry->method, "%s", method);
        entry->files = 1;
    }
}

/* Warteschlange fertiger Jobs (Worker -> Hauptthread) */
typedef struct DoneNode {
    struct DoneNode *next;
} DoneNode;

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    DoneNode *head, *tail;
} DoneQueue;

/* Warteschlange fertiger Jobs anlegen */
static void done_queue_init(DoneQueue *q)
{
    memset(q, 0, sizeof *q);
    pthread_mutex_init(&q->lock, NULL);
    cond_init_monotonic(&q->cond);
}

/* fertigen Job anhängen und den Dispatcher wecken */
static void done_queue_push(DoneQueue *q, DoneNode *n)
{
    pthread_mutex_lock(&q->lock);
    n->next = NULL;
    if (q->tail) q->tail->next = n;
    else q->head = n;
    q->tail = n;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->lock);
}

/* wartet bis zu secs auf fertige Jobs -> alle fertigen (FIFO) oder NULL */
static DoneNode *done_queue_wait(DoneQueue *q, double secs)
{
    pthread_mutex_lock(&q->lock);
    if (!q->head) {
        struct timespec ts = deadline_in(secs);
        while (!q->head && pthread_cond_timedwait(&q->cond, &q->lock, &ts) != ETIMEDOUT) {}
    }
    DoneNode *h = q->head;
    q->head = q->tail = NULL;
    pthread_mutex_unlock(&q->lock);
    return h;
}

/* Warteschlange freigeben */
static void done_queue_destroy(DoneQueue *q)
{
    pthread_mutex_destroy(&q->lock);
    pthread_cond_destroy(&q->cond);
}

typedef struct {
    const char *out, *mode;
    double cut_len;             /* Länge des Intros bzw. Outros (s) */
    bool delete_orig, outro;
    Gate *cpu_gate, *gpu_gate;
    const GpuCaps *gpu;
    DoneQueue dq;
    StrList *created_dirs;      /* neu angelegte Ausgabeordner (siehe mkdirs_recording) */
    pthread_mutex_t created_lock;
} ProcessContext;

typedef struct {
    DoneNode node;              /* muss erstes Element sein */
    pthread_t thr;
    bool joinable;
    ProcessContext *ctx;
    const Video *video;
    Reservation *reservation;
    const char *sub_mode;       /* Modus 4: "gpu" oder "cpu" für diese Datei */
    long long orig_size;        /* Größe des Originals (Byte) */
    double cut_len;             /* Schnittlänge dieser Datei (s) */
    bool keyframe_before;       /* eigener Schnitt (automatisch erkannt): Copy nie in den Inhalt */
    double started_at, finished_at;     /* Uhr ohne Pausen (für die Restdauer) */
    char *dest, *temp;
    CutResult result;
} CutTask;

/* Thread einer Datei: Ziel vorbereiten, schneiden, Ergebnis in die Warteschlange stellen */
static void *cut_task_thread(void *arg)
{
    CutTask *t = arg;
    ProcessContext *c = t->ctx;
    const char *v = t->video->path, *rel = t->video->rel;
    const char *first = t->sub_mode ? t->sub_mode : c->mode;
    t->started_at = active_clock();
    t->orig_size = file_size(v);
    Job *job = job_register(rel, !strcmp(first, "gpu"), (double)t->orig_size);
    ic_log(false, "  Start: %s", rel);
    t->dest = c->delete_orig ? xstrdup(v) : path_join(c->out, rel);
    char *dir = c->delete_orig ? NULL : path_dirname(t->dest);
    int dir_rc = 0, e = 0;
    if (dir) {
        pthread_mutex_lock(&c->created_lock);
        dir_rc = mkdirs_recording(dir, c->created_dirs);
        e = errno;
        pthread_mutex_unlock(&c->created_lock);
    }
    if (dir_rc != 0) {                              /* nie den ganzen Lauf abbrechen */
        memset(&t->result, 0, sizeof t->result);
        snprintf(t->result.method, sizeof t->result.method, "-");
        t->result.note = xasprintf("Ausnahme: [Errno %d] %s: '%s'", e, strerror(e), dir);
        reservation_release(t->reservation);
        free(t->dest);
        t->dest = NULL;
    } else {
        t->temp = temp_path_for(t->dest);
        cut_video(v, t->temp, t->cut_len, c->outro, t->keyframe_before,
                  t->sub_mode ? t->sub_mode : c->mode, t->reservation, c->cpu_gate, c->gpu_gate,
                  c->gpu, job, rel, !strcmp(c->mode, "hybrid"), &t->result);
    }
    free(dir);
    job_unregister(job);
    t->finished_at = active_clock();
    done_queue_push(&c->dq, &t->node);
    return NULL;
}

/* Schnitt-Job freigeben (wartet vorher auf seinen Thread) */
static void cut_task_free(CutTask *t)
{
    if (t->joinable) pthread_join(t->thr, NULL);
    reservation_free(t->reservation);
    free(t->dest);
    free(t->temp);
    free(t->result.note);
    free(t);
}

/* Statuszeile "… läuft: …" (nur Terminal), wenn länger keine Datei fertig wurde */
static void log_running_jobs(void)
{
    if (!ic_fe.show_heartbeat) return;          /* die GUI zeigt die Jobs ohnehin an */
    Buf b = {0};
    char ts[64];
    double now = active_clock();
    pthread_mutex_lock(&g_jobs_lock);
    for (Job *j = g_jobs_head; j; j = j->next)
        buf_printf(&b, "%s%s %s %.0f %% (%s)", b.n ? " | " : "", j->rel, j->what, j->pct,
                   format_duration(now - j->since, ts, sizeof ts));
    pthread_mutex_unlock(&g_jobs_lock);
    if (b.n) ic_log(true, "  … läuft: %s", b.p);
    buf_free(&b);
}

/* Fortschritt beim Verschieben an den Platz der Originale (für ic_fe.move_progress) */
typedef struct {
    int done, total;            /* Dateien */
    double bytes_done;          /* Byte der schon fertig verschobenen Dateien */
    double bytes_total;
    double last_report;         /* Zeitpunkt der letzten Meldung */
} MoveProgress;

/* Fortschritt melden – höchstens fünfmal je Sekunde, außer force */
static void move_progress_report(MoveProgress *mp, double bytes_done, bool force)
{
    double now = monotonic_secs();
    if (!ic_fe.move_progress || (!force && now - mp->last_report < 0.2)) return;
    mp->last_report = now;
    ic_fe.move_progress(mp->done, mp->total, bytes_done, mp->bytes_total);
}

/* Kopierfortschritt einer Datei (über Dateisystemgrenzen) in den Gesamtfortschritt */
static void on_move_copy_progress(void *ctx, long long copied)
{
    MoveProgress *mp = ctx;
    move_progress_report(mp, mp->bytes_done + (double)copied, false);
}

/* Fertige Dateien an den Platz der Originale setzen; genau die tatsächlich
 * verschobenen werden ausgetragen (die übrigen rücken in einem Durchgang auf –
 * linear auch bei 100 000 Dateien). Scheitert eine Datei, bleiben Original und
 * fertige Datei unverändert. Der Fortschritt geht an ic_fe.move_progress, bei
 * langen Läufen kommt alle MOVE_PROGRESS_SECS eine Zeile ins Protokoll.
 * -> Anzahl verschobener Dateien */
static int move_to_originals(StrList *fin_dest, StrList *fin_orig, Stats *stats)
{
    int moved = 0, kept = 0, total = fin_dest->n;
    double last_report = monotonic_secs();
    long long *sizes = xmalloc((size_t)(total ? total : 1) * sizeof *sizes);
    MoveProgress progress = {.total = total};
    for (int i = 0; i < total; i++) {
        sizes[i] = file_size(fin_dest->v[i]);
        progress.bytes_total += (double)sizes[i];
    }
    move_progress_report(&progress, 0, true);
    for (int i = 0; i < total; i++) {
        if (replace_file_reporting(fin_dest->v[i], fin_orig->v[i], on_move_copy_progress,
                                   &progress) == 0) {
            moved++;
            stats->replaced++;
            free(fin_dest->v[i]);
            free(fin_orig->v[i]);
        } else {
            int e = errno;
            stats->move_err++;
            ic_log(true,
                   "    Konnte nicht verschieben (Original bleibt erhalten): %s: [Errno %d] %s",
                   fin_orig->v[i], e, strerror(e));
            fin_dest->v[kept] = fin_dest->v[i];
            fin_orig->v[kept++] = fin_orig->v[i];
        }
        progress.done = i + 1;
        progress.bytes_done += (double)sizes[i];
        move_progress_report(&progress, progress.bytes_done, i + 1 == total);
        if (monotonic_secs() - last_report >= MOVE_PROGRESS_SECS) {
            last_report = monotonic_secs();
            ic_log(true, "  … %d von %d Dateien verschoben (%.1f von %.1f GB)", i + 1, total,
                   progress.bytes_done / 1e9, progress.bytes_total / 1e9);
        }
    }
    free(sizes);
    fin_dest->n = fin_orig->n = kept;
    if (fin_dest->v) fin_dest->v[kept] = NULL;  /* Listen bleiben NULL-terminiert */
    if (fin_orig->v) fin_orig->v[kept] = NULL;
    return moved;
}

/* Ordner aus dirs entfernen, sofern sie leer sind – in umgekehrter Reihenfolge des
 * Anlegens, also Unterordner vor ihren Eltern. Ordner mit Inhalt bleiben (rmdir löscht
 * nie Dateien). -> Anzahl entfernter Ordner */
static int remove_empty_dirs(const StrList *dirs)
{
    int removed = 0;
    for (int i = dirs->n - 1; i >= 0; i--)
        if (rmdir(dirs->v[i]) == 0) removed++;
    return removed;
}

/* Datei k aus der Warteliste nehmen und in einem eigenen Thread bearbeiten */
static void launch_cut_task(ProcessContext *ctx, const Video **pending, Gate **gate_of,
                            double *pending_bytes, int *npending, int *inflight, int k,
                            Reservation *reservation, const char *sub_mode)
{
    CutTask *t = xmalloc(sizeof *t);
    memset(t, 0, sizeof *t);
    t->ctx = ctx;
    t->video = pending[k];
    t->cut_len = pending[k]->cut_len > 0 ? pending[k]->cut_len : ctx->cut_len;
    t->keyframe_before = pending[k]->cut_len > 0 && !ctx->outro;
    t->reservation = reservation;
    t->sub_mode = sub_mode;
    memmove(pending + k, pending + k + 1, (size_t)(*npending - k - 1) * sizeof *pending);
    memmove(gate_of + k, gate_of + k + 1, (size_t)(*npending - k - 1) * sizeof *gate_of);
    memmove(pending_bytes + k, pending_bytes + k + 1,
            (size_t)(*npending - k - 1) * sizeof *pending_bytes);
    (*npending)--;
    schedule_update(*npending, *inflight + 1);
    (*inflight)++;
    if (start_thread(&t->thr, cut_task_thread, t) == 0)
        t->joinable = true;
    else
        cut_task_thread(t);                 /* kein Thread verfügbar: direkt ausführen */
}

typedef struct {
    const Video *v;
    long long size;
    int idx;
} SizedVideo;

/* qsort-Vergleich: größte Datei zuerst, bei gleicher Größe in Fundreihenfolge */
static int compare_size_desc(const void *a, const void *b)
{
    const SizedVideo *x = a, *y = b;
    if (x->size != y->size) return x->size < y->size ? 1 : -1;
    return x->idx - y->idx;                         /* stabil wie Pythons sorted() */
}

/* qsort-Vergleich für Strings */
static int compare_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* -> false bei Abbruch. Im Keep-Modus landen fertige (noch nicht an den Platz
 * der Originale verschobene) Dateien in fin_dest/fin_orig. */
static bool process_videos(const Video *const *matches, int nmatch, const char *out,
                           double cut_len, bool outro, const char *mode, bool delete_orig,
                           Governor *gov, const GpuCaps *gpu, Stats *stats,
                           StrList *fin_dest, StrList *fin_orig, StrList *created_dirs)
{
    memset(stats, 0, sizeof *stats);
    bool copy = !strcmp(mode, "copy"), hybrid = !strcmp(mode, "hybrid");
    if (!delete_orig && mkdirs_recording(out, created_dirs) != 0) {
        frontend_info("Ausgabeordner kann nicht angelegt werden: %s: %s", out, strerror(errno));
        return false;
    }
    /* Governor für CPU-Encodes: vorsichtig starten, Einschwingzeit abwarten.
     * Modus 4: GPU-Jobs brauchen selbst etwas CPU -> CPU-Jobs halb so viele zum Start */
    int cpu_start = !strcmp(mode, "cpu") ? CPU_COUNT / (2 * REENCODE_THREADS)
                  : hybrid              ? CPU_COUNT / (4 * REENCODE_THREADS) : 2;
    governor_configure(gov, cpu_start > 1 ? cpu_start : 1, 6.0);
    Gate *cpu_gate = gate_new(0, gov, "CPU");
    Gate *gpu_gate = gate_new(GPU_WORKERS, NULL, "GPU");
    Gate *out_gate = delete_orig ? NULL : filesystem_gate(out);

    /* Größte Dateien zuerst -> kein langer Nachzügler am Ende */
    SizedVideo *sized = xmalloc((size_t)(nmatch ? nmatch : 1) * sizeof *sized);
    double total_bytes = 0;                     /* für die Restdauer */
    for (int i = 0; i < nmatch; i++) {
        sized[i] = (SizedVideo){matches[i], file_size(matches[i]->path), i};
        total_bytes += (double)sized[i].size;
    }
    qsort(sized, (size_t)nmatch, sizeof *sized, compare_size_desc);
    const Video **pending = xmalloc((size_t)(nmatch ? nmatch : 1) * sizeof *pending);
    Gate **gate_of = xmalloc((size_t)(nmatch ? nmatch : 1) * sizeof *gate_of);
    double *pending_bytes = xmalloc((size_t)(nmatch ? nmatch : 1) * sizeof *pending_bytes);
    int npending = nmatch, total = nmatch, done = 0;
    for (int i = 0; i < nmatch; i++) {
        pending[i] = sized[i].v;
        pending_bytes[i] = (double)sized[i].size;
    }
    free(sized);

    int n_fs = 0;                               /* Copy: verschiedene Quell-Dateisysteme */
    if (copy) {
        StrList kinds = {0};
        Gate **seen = xmalloc((size_t)(nmatch ? nmatch : 1) * sizeof *seen);
        for (int i = 0; i < nmatch; i++) {          /* 1x pro Datei ermitteln */
            gate_of[i] = filesystem_gate(pending[i]->path);
            bool known = false;
            for (int j = 0; j < n_fs; j++) known |= seen[j] == gate_of[i];
            if (!known) seen[n_fs++] = gate_of[i];
            if (!strlist_contains(&kinds, gate_of[i]->kind)) strlist_add(&kinds, gate_of[i]->kind);
        }
        qsort(kinds.v, (size_t)kinds.n, sizeof *kinds.v, compare_str);
        Buf b = {0};
        buf_printf(&b, "  I/O-Limit (%d Dateisystem(e)): ", n_fs);
        for (int i = 0; i < kinds.n; i++)
            buf_printf(&b, "%s%s -> %d Copy-Job(s) parallel", i ? ", " : "", kinds.v[i],
                       !strcmp(kinds.v[i], "SSD") ? IO_JOBS_SSD : IO_JOBS_HDD);
        frontend_info("%s", buf_str(&b));
        buf_free(&b);
        free(seen);
        strlist_free(&kinds);
    }

    pthread_mutex_lock(&g_gpu_lock);
    g_gpu_hangs = 0;
    g_gpu_disabled = false;
    pthread_mutex_unlock(&g_gpu_lock);

    gpu_brake_start(gpu);                               /* GPU-Last begrenzen */
    ProcessContext ctx = {.out = out, .mode = mode, .cut_len = cut_len,
                          .delete_orig = delete_orig, .outro = outro, .cpu_gate = cpu_gate,
                          .gpu_gate = gpu_gate, .gpu = gpu, .created_dirs = created_dirs};
    pthread_mutex_init(&ctx.created_lock, NULL);
    done_queue_init(&ctx.dq);
    int inflight = 0;
    bool paused = false, stop_all = false;      /* paused: Disk-Schutz */
    bool user_was_paused = false;
    double last_beat = active_clock(), next_cpu_start = 0;  /* Uhr ohne Pausen */
    double next_estimate_line = last_beat + ETA_LOG_FIRST_SECS;
    eta_begin(nmatch, total_bytes, last_beat);

    while ((npending || inflight) && !stop_all && !ic_cancel) {
        schedule_update(npending, inflight);
        /* --- Disk-Schutz (nur Keep-Modus) --- */
        if (!delete_orig && !paused && free_space_gb(out) < DISK_MIN_GB)
            paused = true;          /* keine neuen Tasks mehr starten -> laufende abwarten */
        if (paused && !inflight) {
            double free_gb = free_space_gb(out);
            frontend_info("\n!!! Nur noch %.1f GB frei auf dem Ausgabe-Datenträger "
                          "(Schwelle %d GB).", free_gb, DISK_MIN_GB);
            if (fin_dest->n && ic_fe.ask_disk_move(free_gb, fin_dest->n)) {
                int before = fin_dest->n, moved = move_to_originals(fin_dest, fin_orig, stats);
                if (moved < before)
                    ic_log(true,
                           "  %d Datei(en) verschoben, Platz freigegeben. %d nicht verschoben.",
                           moved, before - moved);
                else
                    ic_log(true, "  %d Datei(en) verschoben, Platz freigegeben.", moved);
            }
            if (ic_cancel) break;
            if (!ic_fe.ask_disk_continue(free_space_gb(out))) {
                stop_all = true;
                stats->aborted = true;
                break;
            }
            paused = false;
            continue;
        }

        /* --- neue Tasks starten (nur wenn der passende Slot frei ist; nicht während
         *     einer Pause des Nutzers) --- */
        bool user_paused = ic_paused();
        double tnow = active_clock();
        if (user_was_paused && !user_paused) next_cpu_start = tnow + RESUME_HOLD;
        user_was_paused = user_paused;
        bool hold = paused || user_paused;
        /* CPU-Jobs gestaffelt starten (höchstens einer je CPU_START_GAP), sonst
         * überschießt die Last, bevor der Governor sie messen kann */
        bool cpu_ok = tnow >= next_cpu_start;
        if (!hold && hybrid) {
            /* Modus 4: die GPU (schnell) nimmt die größte, die CPU die kleinste offene Datei */
            while (npending && inflight < POOL_THREADS) {
                Reservation *slot = gpu_is_disabled() ? NULL : reservation_try(&gpu_gate, 1);
                const char *sub_mode = "gpu";
                int k = 0;
                if (!slot && cpu_ok) {
                    slot = reservation_try(&cpu_gate, 1);
                    sub_mode = "cpu";
                    k = npending - 1;
                    if (slot) {
                        cpu_ok = false;
                        next_cpu_start = tnow + CPU_START_GAP;
                    }
                }
                if (!slot) break;           /* GPU und CPU voll (bzw. CPU noch gesperrt) */
                launch_cut_task(&ctx, pending, gate_of, pending_bytes, &npending, &inflight, k,
                                slot, sub_mode);
            }
        } else if (!hold) {
            bool cpu_jobs = !copy && strcmp(mode, "gpu") != 0;
            /* Copy: Dateisysteme, deren Slot in diesem Durchgang voll war – ihre Dateien
             * werden übersprungen, statt bei 100 000 wartenden Dateien jede einzeln zu
             * versuchen */
            Gate *full_gates[16];
            int n_full = 0;
            for (int k = 0; k < npending && inflight < POOL_THREADS;) {
                if (cpu_jobs && !cpu_ok) break;
                if (copy) {
                    bool known_full = false;
                    for (int j = 0; j < n_full && !known_full; j++)
                        known_full = full_gates[j] == gate_of[k];
                    if (known_full) {
                        k++;
                        continue;
                    }
                }
                Gate *want[2] = {copy ? gate_of[k] : !strcmp(mode, "gpu") ? gpu_gate : cpu_gate,
                                 copy ? out_gate : NULL};
                Reservation *slot = reservation_try(want, 2);
                if (!slot) {
                    if (!copy) break;       /* gemeinsamer Slot voll -> nicht weitersuchen */
                    /* Copy: evtl. ist ein anderes Dateisystem frei; ist der Ziel-Slot
                     * voll, sind nach n_fs Versuchen alle Dateisysteme vermerkt */
                    if (n_full < (int)(sizeof full_gates / sizeof *full_gates))
                        full_gates[n_full++] = gate_of[k];
                    if (n_full >= n_fs) break;
                    k++;
                    continue;
                }
                launch_cut_task(&ctx, pending, gate_of, pending_bytes, &npending, &inflight, k,
                                slot, NULL);
                if (cpu_jobs) {
                    cpu_ok = false;
                    next_cpu_start = tnow + CPU_START_GAP;
                }
            }
        }
        if (!inflight) {
            sleep_secs(0.2);
            continue;
        }

        DoneNode *d = done_queue_wait(&ctx.dq, 0.5);
        double now = active_clock();
        if (d) {
            last_beat = now;
        } else if (now - last_beat >= HEARTBEAT_SECS) {
            last_beat = now;
            log_running_jobs();
        }
        eta_sample(now, pending_bytes, npending);
        if (now >= next_estimate_line) {
            next_estimate_line = now + ETA_LOG_SECS;
            log_estimate(done, total);
        }
        while (d && !ic_cancel) {
            CutTask *t = (CutTask *)d;
            d = d->next;
            inflight--;
            done++;
            const char *rel = t->video->rel;
            bool ok = t->result.success;
            char *note = t->result.note &&
                         *t->result.note ? xasprintf("  [%s]", t->result.note) : xstrdup("");
            long long new_size = ok ? file_size(t->temp) : 0;
            if (!ok) {
                stats->err++;
                remove_file(t->temp);
                temp_unregister(t->temp);
                ic_log(true, "  [%d/%d] FEHLER %s%s", done, total, rel, note);
            } else if (replace_file(t->temp, t->dest) != 0) {
                /* Temp -> endgültiger Ort gescheitert: nur für diese Datei ein
                 * Fehler – das Original bleibt unangetastet, der Lauf geht weiter */
                int e = errno;
                ok = false;
                stats->err++;
                remove_file(t->temp);
                temp_unregister(t->temp);
                ic_log(true, "  [%d/%d] FEHLER %s  [Verschieben fehlgeschlagen, Original "
                       "unverändert: [Errno %d] %s]", done, total, rel, e, strerror(e));
            } else {
                temp_unregister(t->temp);
                stats->ok++;
                stats_count_method(stats, t->result.method);
                stats->saved_bytes += t->orig_size - new_size;
                stats->total_cut += t->result.cut_at;
                double extra = t->result.cut_at - t->cut_len;
                char trim[96] = "";
                if (!strcmp(t->result.method, "Copy") && extra > 0.05) {
                    stats->trimmed_files++;
                    stats->max_trim = fmax(stats->max_trim, extra);
                    snprintf(trim, sizeof trim, "  (Schnitt bei %.1fs, %.1fs Inhalt mit)",
                             t->result.cut_at, extra);
                } else if (!strcmp(t->result.method, "Copy") && extra < -0.05) {
                    stats->rest_files++;            /* Keyframe im Intro: Rest bleibt stehen */
                    stats->max_rest = fmax(stats->max_rest, -extra);
                    snprintf(trim, sizeof trim, "  (Schnitt bei %.1fs, %.1fs Intro bleiben)",
                             t->result.cut_at, -extra);
                }
                if (delete_orig) {
                    stats->replaced++;
                    ic_log(true, "  [%d/%d] OK %s (ersetzt) %s%s%s", done, total, t->result.method,
                           rel, trim, note);
                } else {
                    strlist_add(fin_dest, t->dest);
                    strlist_add(fin_orig, t->video->path);
                    ic_log(true, "  [%d/%d] OK %s %s%s%s", done, total, t->result.method, rel,
                           trim, note);
                }
            }
            eta_file_done((double)t->orig_size, ok, t->finished_at - t->started_at);
            if (ic_fe.file_done) ic_fe.file_done(done, total, ok);
            free(note);
            cut_task_free(t);
        }
        while (d) {                         /* nach Strg+C: Ergebnisse verwerfen */
            CutTask *t = (CutTask *)d;
            d = d->next;
            inflight--;
            cut_task_free(t);
        }
    }
    /* Abbruch: laufende Jobs enden sofort (ffmpeg wird beendet) – abwarten, verwerfen */
    while (inflight) {
        for (DoneNode *d = done_queue_wait(&ctx.dq, 0.5); d;) {
            CutTask *t = (CutTask *)d;
            d = d->next;
            inflight--;
            cut_task_free(t);
        }
    }
    schedule_update(0, 0);
    eta_finish();
    gpu_brake_stop();
    bool aborted = ic_cancel;
    free(pending);
    free(gate_of);
    free(pending_bytes);
    done_queue_destroy(&ctx.dq);
    pthread_mutex_destroy(&ctx.created_lock);
    gate_free(cpu_gate);
    gate_free(gpu_gate);
    return !aborted;
}

/* ---------------------------------------------------------------------------
 * Erkennung: alle offenen Dateien hashen (Parallelität über den Governor)
 * ------------------------------------------------------------------------- */
typedef struct {
    DoneNode node;          /* muss erstes Element sein */
    pthread_t thr;
    bool joinable;
    const Video *video;
    int k;                  /* Vergleichsbilder */
    char (*ts)[24];         /* ihre Zeitpunkte (gemeinsam) */
    char *key[IC_MAX_FRAMES];
    bool need[IC_MAX_FRAMES];           /* noch zu lesen (sonst aus dem Cache) */
    bool ok[IC_MAX_FRAMES];
    uint64_t hash[IC_MAX_FRAMES];
    bool single, from_end;
    Gate *gate;
    DoneQueue *dq;
} DetectTask;

/* Thread einer Datei bei der Suche: fehlende Vergleichsbilder hashen */
static void *detect_task_thread(void *arg)
{
    DetectTask *t = arg;
    hash_frames(t->video->path, t->ts, t->need, t->k, t->single, t->from_end, t->hash, t->ok);
    gate_release(t->gate);
    done_queue_push(t->dq, &t->node);
    return NULL;
}

/* Such-Job freigeben (wartet vorher auf seinen Thread) */
static void detect_task_free(DetectTask *t)
{
    if (t->joinable) pthread_join(t->thr, NULL);
    for (int j = 0; j < t->k; j++) free(t->key[j]);
    free(t);
}

/* qsort-Vergleich: kleinste Distanz zuerst, sonst Fundreihenfolge (stabil) */
static int compare_results(const void *a, const void *b)
{
    const Result *x = a, *y = b;
    return x->dist != y->dist ? x->dist - y->dist : x->order - y->order;
}

/* 16 Hex-Ziffern als 64-Bit-Zahl lesen (Format im Cache) -> false bei Fehler */
static bool parse_hex64(const char *s, uint64_t *out)
{
    if (strlen(s) != 16) return false;
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        int c = tolower((unsigned char)s[i]);
        int h = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (h < 0) return false;
        v = v << 4 | (unsigned)h;
    }
    *out = v;
    return true;
}

/* offene Dateien parallel hashen (Anzahl regelt der Governor) und auswerten */
static void run_detection(DetectTask **pend, int npend, Governor *gov, HashCache *cache,
                          const uint64_t *ref_hash, Result *results, int *nres, int *skipped)
{
    bool single = npend >= CPU_COUNT;           /* viele Dateien -> 1 Decoder-Thread */
    Gate *gate = gate_new(0, gov, "CPU");
    DoneQueue done_queue;
    done_queue_init(&done_queue);
    double last_save = monotonic_secs(), save_interval = CACHE_SAVE_SECS;
    int next = 0, inflight = 0, done = 0;
    while ((next < npend || inflight) && !ic_cancel) {
        while (next < npend && inflight < POOL_THREADS && gate_try_acquire(gate)) {
            DetectTask *t = pend[next++];
            t->single = single;
            t->gate = gate;
            t->dq = &done_queue;
            inflight++;
            if (start_thread(&t->thr, detect_task_thread, t) == 0)
                t->joinable = true;
            else
                detect_task_thread(t);
        }
        if (!inflight) {
            sleep_secs(0.2);
            continue;
        }
        for (DoneNode *d = done_queue_wait(&done_queue, 0.5); d;) {
            DetectTask *t = (DetectTask *)d;
            d = d->next;
            inflight--;
            if (!ic_cancel) {           /* nach Strg+C: nichts mehr in den Cache */
                for (int j = 0; j < t->k; j++) {
                    if (!t->need[j] || !t->key[j]) continue;
                    char hash_hex[17];
                    snprintf(hash_hex, sizeof hash_hex, "%016llx", (unsigned long long)t->hash[j]);
                    cache_set(cache, t->key[j], t->ok[j] ? hash_hex : NOHASH);
                }
                if (monotonic_secs() - last_save > save_interval) {
                    double save_started = monotonic_secs();
                    cache_save(cache);
                    last_save = monotonic_secs();
                    /* sehr großer Cache (viele 100 MB): höchstens etwa 5 % der Zeit
                     * mit Zwischenspeichern verbringen */
                    save_interval = fmax(CACHE_SAVE_SECS, 20 * (last_save - save_started));
                }
                Result *result = &results[*nres];
                memset(result, 0, sizeof *result);
                result->v = t->video;
                result->order = *nres;
                if (video_distance(ref_hash, t->hash, t->ok, t->k, result))
                    (*nres)++;
                else
                    (*skipped)++;
                done++;
                if (ic_fe.detect_progress) {        /* Terminal: nur alle 25 Dateien */
                    int target_workers;
                    double cpu;
                    governor_status(gov, &target_workers, &cpu);
                    ic_fe.detect_progress(done, npend, target_workers, gov->max, cpu);
                }
            }
            detect_task_free(t);
        }
    }
    while (inflight)                            /* Abbruch: laufende Leser abwarten */
        for (DoneNode *d = done_queue_wait(&done_queue, 0.5); d; inflight--) {
            DetectTask *t = (DetectTask *)d;
            d = d->next;
            detect_task_free(t);
        }
    for (int i = next; i < npend; i++) detect_task_free(pend[i]);
    done_queue_destroy(&done_queue);
    gate_free(gate);
}

/* ---------------------------------------------------------------------------
 * Abhängigkeiten sicherstellen (nur ffmpeg/ffprobe nötig)
 * ------------------------------------------------------------------------- */
static void run_or_die(const StrList *cmd)
{
    char *s = strlist_join(cmd, " ");
    printf("  $ %s\n", s);
    ChildProc c;
    int rc = -1;
    if (spawn_child(&c, cmd, PRIO_NONE, IO_INHERIT, IO_INHERIT, IO_INHERIT) == 0) {
        int st;
        while (waitpid(c.pid, &st, 0) < 0 && errno == EINTR) {}
        c.reaped = true;
        c.status = st;
        rc = child_exit_code(&c);
    }
    if (rc != 0) {
        fprintf(stderr, "Abbruch: Befehl fehlgeschlagen (%d): %s\n", rc, s);
        exit(1);
    }
    free(s);
}

/* ffmpeg und ffprobe sicherstellen, notfalls per apt installieren (Terminal) */
void ic_setup_dependencies(void)
{
    if (command_exists("ffmpeg") && command_exists("ffprobe")) return;
    printf("Installiere Systempakete (sudo-Passwort wird evtl. abgefragt): ffmpeg\n");
    StrList cmd = {0};
    strlist_add_many(&cmd, "sudo", "apt-get", "update", NULL);
    run_or_die(&cmd);
    strlist_free(&cmd);
    strlist_add_many(&cmd, "sudo", "apt-get", "install", "-y", "ffmpeg", NULL);
    run_or_die(&cmd);
    strlist_free(&cmd);
}

/* ---------------------------------------------------------------------------
 * Hauptprogramm
 * ------------------------------------------------------------------------- */
void ic_init(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    CPU_COUNT = n > 0 ? (int)n : 4;
    MAX_WORKERS = CPU_COUNT * 2 > 2 ? CPU_COUNT * 2 : 2;
    POOL_THREADS = CPU_COUNT * 4 > 16 ? CPU_COUNT * 4 : 16;
    APP_DIR = expanduser("~/.intro-cutter");
    LOG_FILE = path_join(APP_DIR, "last_matches.txt");
    AUTO_LOG_FILE = path_join(APP_DIR, "last_auto_matches.txt");
    RUN_LOG = path_join(APP_DIR, "last_run.log");
    HASH_CACHE = path_join(APP_DIR, "hash_cache.json");
    SELFTEST_CLIP = path_join(APP_DIR, "gpu_selftest.mp4");
    init_priority_prefixes();
}

/* Videoliste freigeben */
static void videos_free(VideoList *vl)
{
    for (int i = 0; i < vl->n; i++) {
        free(vl->v[i].path);
        free(vl->v[i].rel);
    }
    free(vl->v);
    memset(vl, 0, sizeof *vl);
}

/* ---------------------------------------------------------------------------
 * Öffentliche Schnittstelle (ic_core.h)
 * ------------------------------------------------------------------------- */
bool ic_have_ffmpeg(void)
{
    return command_exists("ffmpeg") && command_exists("ffprobe");
}

/* GPU (VAAPI) erkennen und selbst testen (siehe ic_core.h) */
bool ic_gpu_detect(GpuCaps *out)
{
    return gpu_detect(out);
}

/* GPU-Fähigkeiten als neuer String (siehe ic_core.h) */
char *ic_gpu_summary(const GpuCaps *caps)
{
    Buf b = {0};
    gpu_summary(caps, &b);
    return buf_detach(&b);
}

/* Zeitpunkt des Vergleichsbilds (Intro-Mitte) in Python-Darstellung */
void ic_ref_timestamp(double cut_len, char *ts, size_t sz)
{
    python_float_repr(python_round(cut_len / 2, 3), ts, sz);
}

/* tatsächliche Zahl der Vergleichsbilder (ungerade; bei sehr kurzen Intros weniger) */
int ic_frames_for(double cut_len, int frames)
{
    int k = frames < 1 ? 1 : frames > IC_MAX_FRAMES ? IC_MAX_FRAMES : frames;
    if (k % 2 == 0) k--;                    /* ungerade: eindeutiger Median */
    while (k > 1 && cut_len / (k + 1) < 0.1) k -= 2;       /* mind. 0,1 s Abstand */
    return k;
}

/* Zeitpunkte der Vergleichsbilder, gleichmäßig über das Intro verteilt */
void ic_ref_timestamps(double cut_len, int frames, char ts[][24])
{
    if (frames <= 1) {
        ic_ref_timestamp(cut_len, ts[0], sizeof ts[0]);
        return;
    }
    for (int j = 0; j < frames; j++)    /* j+1 von frames+1 Teilen; die Mitte wie oben */
        python_float_repr(python_round(cut_len * (j + 1) / (frames + 1), 3), ts[j], sizeof ts[j]);
}

/* Vergleichsframe als PNG – derselbe Frame, den hash_frame() liest */
bool ic_ref_frame_png(const char *video, double cut_len, bool outro, const char *png)
{
    char ts[64];
    ic_ref_timestamp(cut_len, ts, sizeof ts);
    StrList cmd = {0};
    strlist_add_many(&cmd, "ffmpeg", "-nostdin", "-y", "-v", "error", NULL);
    if (!add_frame_input(&cmd, video, ts, outro)) {
        strlist_free(&cmd);
        return false;
    }
    strlist_add_many(&cmd, "-frames:v", "1", "-vf", "scale=iw*sar:ih,scale='min(320,iw)':-2",
                     "-f", "image2", "-c:v", "png", png, NULL);
    int rc = run_ffmpeg(&cmd, 30, NULL, NULL);
    strlist_free(&cmd);
    return rc == 0 && file_not_empty(png);
}

/* die eigentliche Suche; ic_detect sperrt drumherum den Standby */
static int detect_videos(const char *ref, double cut_len, const char *src, const char *out,
                       int threshold, int frames, bool outro, IcDetect *d, const char **err)
{
    memset(d, 0, sizeof *d);
    *err = NULL;
    int k = ic_frames_for(cut_len, frames);
    d->frames = k;
    ic_ref_timestamp(cut_len, d->ts, sizeof d->ts);
    ic_ref_timestamps(cut_len, k, d->tss);
    uint64_t ref_hash[IC_MAX_FRAMES];
    bool ref_ok[IC_MAX_FRAMES] = {false}, need_all[IC_MAX_FRAMES] = {false};
    for (int j = 0; j < k; j++) need_all[j] = true;
    hash_frames(ref, d->tss, need_all, k, false, outro, ref_hash, ref_ok);
    bool ref_fine = true;
    for (int j = 0; j < k; j++) ref_fine &= ref_ok[j];
    if (!ref_fine) {
        if (ic_cancel) return 1;
        *err = outro ? "Konnte aus dem Referenz-Video keinen Frame lesen. Ist die Outro-Länge "
                       "plausibel?"
                     : "Konnte aus dem Referenz-Video keinen Frame lesen. Ist die Intro-Länge "
                       "plausibel?";
        return -1;
    }
    find_videos(src, out, &d->videos);
    if (ic_cancel) return 1;
    if (!d->videos.n) {
        *err = "Keine Videos im Quellordner gefunden.";
        return -1;
    }

    Governor gov;                               /* kurze Jobs: ohne Einschwingzeit regeln */
    governor_init(&gov, MAX_WORKERS, CPU_COUNT / 2 > 2 ? CPU_COUNT / 2 : 2, 0.0);
    HashCache cache = {0};
    cache_load(&cache);
    double started_at = monotonic_secs();
    int n = d->videos.n, npend = 0;
    d->results = xmalloc((size_t)n * sizeof *d->results);
    DetectTask **pend = xmalloc((size_t)n * sizeof *pend);
    for (int i = 0; i < n && !ic_cancel; i++) {
        const Video *v = &d->videos.v[i];
        DetectTask *t = xmalloc(sizeof *t);
        memset(t, 0, sizeof *t);
        t->video = v;
        t->k = k;
        t->ts = d->tss;
        t->from_end = outro;
        bool missing = false;
        char *key_prefix = NULL;
        const char *key_suffix = "";
        bool have_key = cache_key_parts(v->path, outro, &key_prefix, &key_suffix);
        for (int j = 0; j < k; j++) {       /* je Zeitpunkt aus dem Cache, sonst lesen */
            t->key[j] = have_key ? cache_key_at(key_prefix, d->tss[j], key_suffix) : NULL;
            const char *cached = t->key[j] ? cache_get(&cache, t->key[j]) : NULL;
            if (cached && !strcmp(cached, NOHASH)) {
                t->ok[j] = false;
            } else if (cached && parse_hex64(cached, &t->hash[j])) {
                t->ok[j] = true;
            } else {
                t->need[j] = missing = true;
            }
        }
        free(key_prefix);
        if (missing) {
            pend[npend++] = t;
            continue;
        }
        Result *result = &d->results[d->nres];
        memset(result, 0, sizeof *result);
        result->v = v;
        result->order = d->nres;
        if (video_distance(ref_hash, t->hash, t->ok, k, result))
            d->nres++;
        else
            d->skipped++;
        detect_task_free(t);
    }
    if (ic_cancel) {                            /* Abbruch schon bei der Vorbereitung */
        for (int i = 0; i < npend; i++) detect_task_free(pend[i]);
        free(pend);
        governor_stop(&gov);
        cache_free(&cache);
        return 1;
    }
    if (ic_fe.detect_start) ic_fe.detect_start(n, n - npend, npend);
    run_detection(pend, npend, &gov, &cache, ref_hash, d->results, &d->nres, &d->skipped);
    free(pend);
    cache_save(&cache);                         /* auch bei Abbruch (Zwischenstand) */
    d->secs = monotonic_secs() - started_at;
    governor_stop(&gov);
    cache_free(&cache);
    if (ic_cancel) return 1;

    qsort(d->results, (size_t)d->nres, sizeof *d->results, compare_results);
    while (d->nmatch < d->nres && d->results[d->nmatch].dist <= threshold) d->nmatch++;
    char *dir = path_dirname(LOG_FILE);
    mkdirs(dir);
    free(dir);
    FILE *match_log = fopen(LOG_FILE, "we");
    if (match_log) {
        for (int i = 0; i < d->nmatch; i++)
            fprintf(match_log, "%s%d\t%s", i ? "\n" : "", d->results[i].dist,
                    d->results[i].v->path);
        fputc('\n', match_log);
        fclose(match_log);
    }
    return 0;
}

/* Intro bzw. Outro in allen Videos suchen (siehe ic_core.h) */
int ic_detect(const char *ref, double cut_len, const char *src, const char *out,
              int threshold, int frames, bool outro, IcDetect *d, const char **err)
{
    standby_lock_begin("Intro-Cutter sucht Intros");
    int rc = detect_videos(ref, cut_len, src, out, threshold, frames, outro, d, err);
    standby_lock_end();
    return rc;
}

/* Ergebnis einer Suche freigeben */
void ic_detect_free(IcDetect *d)
{
    free(d->results);
    videos_free(&d->videos);
    memset(d, 0, sizeof *d);
}

/* CPU-Last laut Governor der laufenden Bearbeitung */
static pthread_mutex_t g_gov_lock = PTHREAD_MUTEX_INITIALIZER;
static Governor *g_process_governor;

/* ausgewählte Videos schneiden (siehe ic_core.h) */
bool ic_process(const Video *const *selected, int n, const char *out, double cut_len, bool outro,
                const char *mode, bool delete_orig, const GpuCaps *gpu, IcRun *r)
{
    memset(r, 0, sizeof *r);
    standby_lock_begin("Intro-Cutter bearbeitet Videos");
    pause_reset();
    Governor gov;
    governor_init(&gov, MAX_WORKERS, CPU_COUNT / 2 > 2 ? CPU_COUNT / 2 : 2, 1.0);
    pthread_mutex_lock(&g_gov_lock);
    g_process_governor = &gov;
    pthread_mutex_unlock(&g_gov_lock);
    double started_at = active_clock();
    bool ok = process_videos(selected, n, out, cut_len, outro, mode, delete_orig, &gov, gpu,
                             &r->stats, &r->fin_dest, &r->fin_orig, &r->created_dirs);
    r->secs = active_clock() - started_at;      /* ohne Pausen */
    pthread_mutex_lock(&g_gov_lock);
    g_process_governor = NULL;
    pthread_mutex_unlock(&g_gov_lock);
    governor_stop(&gov);
    pause_reset();
    standby_lock_end();
    return ok;
}

/* fertige Dateien an den Platz der Originale verschieben -> Anzahl */
int ic_move_finished(IcRun *r)
{
    standby_lock_begin("Intro-Cutter verschiebt fertige Dateien");
    int moved = move_to_originals(&r->fin_dest, &r->fin_orig, &r->stats);
    standby_lock_end();
    r->dirs_removed = remove_empty_dirs(&r->created_dirs);
    return moved;
}

/* Ergebnis einer Bearbeitung freigeben */
void ic_run_free(IcRun *r)
{
    strlist_free(&r->fin_dest);
    strlist_free(&r->fin_orig);
    strlist_free(&r->created_dirs);
}

/* aktuelle GPU-Last und Bremse für die Anzeige (siehe ic_core.h) */
bool ic_gpu_load(double *load, double *pause)
{
    pthread_mutex_lock(&g_running_lock);
    bool ok = g_brake_running && !isnan(g_gpu_load);
    *load = g_gpu_load;
    *pause = g_brake_share;
    pthread_mutex_unlock(&g_running_lock);
    return ok;
}

/* CPU-Last laut Governor der laufenden Bearbeitung */
bool ic_cpu_load(double *cpu)
{
    pthread_mutex_lock(&g_gov_lock);
    bool ok = g_process_governor != NULL;
    if (ok) {
        int target;
        governor_status(g_process_governor, &target, cpu);
    }
    pthread_mutex_unlock(&g_gov_lock);
    return ok;
}

/* Laufzeit eines Videos in Sekunden (0 = unbekannt) */
double ic_duration(const char *video)
{
    return probe_duration(video);
}

/* Vorschaubild: das Hauptvideo (kein Cover) bei t s, proportional in w x h */
bool ic_frame_rgb(const char *video, double t, int w, int h, unsigned char *rgb)
{
    StrList cmd = {0};
    strlist_add_many(&cmd, "ffmpeg", "-nostdin", "-v", "error", "-threads", "2", NULL);
    add_seeking_input(&cmd, video, t > 0 ? t : 0);
    strlist_add_many(&cmd, "-map", "0:V:0", "-frames:v", "1", "-vf", NULL);
    strlist_add_printf(&cmd, "scale=iw*sar:ih,scale=%d:%d:force_original_aspect_ratio=decrease,"
                  "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=black,format=rgb24", w, h, w, h);
    strlist_add_many(&cmd, "-f", "rawvideo", "-pix_fmt", "rgb24", "-", NULL);
    Buf out = {0};
    int rc = run_ffmpeg(&cmd, 30, &out, NULL);
    size_t need = (size_t)w * (size_t)h * 3;
    bool ok = rc == 0 && out.n >= need;
    if (ok) memcpy(rgb, out.p, need);
    buf_free(&out);
    strlist_free(&cmd);
    return ok;
}

/* Momentaufnahme der laufenden Jobs für die Anzeige -> Anzahl */
int ic_jobs_snapshot(IcJobInfo *out, int max)
{
    int n = 0;
    double now = active_clock();
    pthread_mutex_lock(&g_jobs_lock);
    for (Job *j = g_jobs_head; j && n < max; j = j->next, n++) {
        snprintf(out[n].rel, sizeof out[n].rel, "%s", j->rel);
        snprintf(out[n].what, sizeof out[n].what, "%s", j->what);
        out[n].pct = j->pct;
        out[n].secs = now - j->since;
        out[n].gpu = j->gpu;
    }
    pthread_mutex_unlock(&g_jobs_lock);
    return n;
}

/* ---------------------------------------------------------------------------
 * Videovorschau: Eigenschaften, fortlaufende Bilder (Wiedergabe, Zeitleiste)
 * ------------------------------------------------------------------------- */
/* Bildrate aus ffprobe-Text wie "30000/1001" oder "25" -> 0, wenn unbekannt */
static double frame_rate_of(const char *s)
{
    if (!s) return 0;
    double num = 0, den = 1;
    if (sscanf(s, "%lf/%lf", &num, &den) < 1 || den <= 0) return 0;
    return num / den;
}

/* Dauer, Bildrate, Anzeigegröße und Ton eines Videos (siehe ic_core.h) */
bool ic_video_props(const char *video, IcVideoProps *p)
{
    memset(p, 0, sizeof *p);
    StrList cmd = {0};
    strlist_add_many(&cmd, "ffprobe", "-v", "error", "-show_entries",
                     "format=duration:stream=codec_type,width,height,avg_frame_rate,"
                     "r_frame_rate,sample_aspect_ratio:stream_disposition=attached_pic",
                     "-of", "json", video, NULL);
    Buf out = {0};
    int rc = run_ffmpeg(&cmd, 30, &out, NULL);
    strlist_free(&cmd);
    JsonValue *root = rc == 0 && out.p ? json_parse(out.p, out.n) : NULL;
    buf_free(&out);
    if (!root) return false;
    json_number_of(json_get(json_get(root, "format"), "duration"), &p->duration);
    const JsonValue *streams = json_get(root, "streams");
    for (int i = 0; streams && streams->type == J_ARR && i < streams->n; i++) {
        const JsonValue *s = streams->items[i];
        const char *type = json_string_of(json_get(s, "codec_type"));
        if (!type) continue;
        if (!strcmp(type, "audio")) p->has_audio = true;
        bool cover = json_truthy(json_get(json_get(s, "disposition"), "attached_pic"));
        if (strcmp(type, "video") != 0 || cover || p->width) continue;
        double w = 0, h = 0;
        json_number_of(json_get(s, "width"), &w);
        json_number_of(json_get(s, "height"), &h);
        /* Pixel-Seitenverhältnis (z. B. anamorphe DVD-Aufnahmen) in die Breite einrechnen */
        int sar_num = 1, sar_den = 1;
        const char *sar = json_string_of(json_get(s, "sample_aspect_ratio"));
        if (sar && sscanf(sar, "%d:%d", &sar_num, &sar_den) == 2 && sar_num > 0 && sar_den > 0)
            w = w * sar_num / sar_den;
        p->width = (int)lround(w);
        p->height = (int)lround(h);
        p->fps = frame_rate_of(json_string_of(json_get(s, "avg_frame_rate")));
        if (p->fps <= 0 || p->fps > 240)
            p->fps = frame_rate_of(json_string_of(json_get(s, "r_frame_rate")));
        if (p->fps <= 0 || p->fps > 240) p->fps = 0;
    }
    json_free(root);
    return p->duration > 0 || p->width > 0;
}

struct IcFrameStream {
    ChildProc child;            /* ffmpeg, Bilder kommen über child.out */
    size_t frame_bytes;         /* w * h * 3 */
    double start, step;         /* Zeitpunkt des ersten Bilds, Abstand der Bilder (s) */
    long frames_read;
};

/* Startzeit des Containers (s): Bezugspunkt für -ss und für die Zeitpunkte der Bilder. Die
 * zuletzt gefragte Datei wird gemerkt (das Markierfenster fragt oft nach derselben) – mit
 * Größe und Änderungszeit, damit eine neu geschriebene Datei gleichen Namens neu gilt. */
static pthread_mutex_t g_origin_lock = PTHREAD_MUTEX_INITIALIZER;
static char *g_origin_path;
static struct stat g_origin_stat;
static double g_origin_value;

static double container_start_of(const char *video)
{
    struct stat st;
    if (stat(video, &st) != 0) memset(&st, 0, sizeof st);
    pthread_mutex_lock(&g_origin_lock);
    bool known = g_origin_path && !strcmp(g_origin_path, video) &&
                 st.st_size == g_origin_stat.st_size &&
                 st.st_mtim.tv_sec == g_origin_stat.st_mtim.tv_sec &&
                 st.st_mtim.tv_nsec == g_origin_stat.st_mtim.tv_nsec;
    double start = g_origin_value;
    pthread_mutex_unlock(&g_origin_lock);
    if (known) return start;
    StrList cmd = {0};
    Buf out = {0};
    strlist_add_many(&cmd, "ffprobe", "-v", "error", "-show_entries", "format=start_time", "-of",
                     "csv=p=0", video, NULL);
    if (run_ffmpeg(&cmd, 30, &out, NULL) != 0 || !out.p || !parse_float(out.p, &start)) start = 0;
    strlist_free(&cmd);
    buf_free(&out);
    pthread_mutex_lock(&g_origin_lock);
    free(g_origin_path);
    g_origin_path = xstrdup(video);
    g_origin_stat = st;
    g_origin_value = start;
    pthread_mutex_unlock(&g_origin_lock);
    return start;
}

/* Eingang für Bilder ab from (s ab Dateistart): grob davor einsteigen (bei .ts/.mpg
 * großzügig, dort springt ffmpeg nur ungefähr) und die echten Zeitstempel behalten
 * (-copyts). Die genaue Auswahl macht danach trim im Filter – so stimmt das Bild, egal wie
 * genau das Format springen kann und wo seine Videospur beginnt. */
static void add_stream_input(StrList *cmd, const char *video, double from, bool thumbs)
{
    char ext[32];
    path_extension(video, ext, sizeof ext);
    double entry = from - (str_in_list(ext, SEEK_OUTPUT_EXTS) ? SEEK_PREROLL : 0);
    strlist_add_many(cmd, "ffmpeg", "-nostdin", "-v", "error", NULL);
    /* Zeitleiste: B-Bilder und Deblocking auslassen – für kleine Bilder unsichtbar, aber
     * deutlich schneller */
    if (thumbs) strlist_add_many(cmd, "-skip_frame", "noref", "-skip_loop_filter", "all", NULL);
    strlist_add(cmd, "-noaccurate_seek");
    if (entry > 0) {
        strlist_add(cmd, "-ss");
        strlist_add_printf(cmd, "%.6f", entry);
    }
    strlist_add_many(cmd, "-i", video, "-copyts", NULL);
}

/* ffmpeg aus cmd mit Bildern auf stdout starten; Bild i hat den Zeitpunkt start + i * step */
static IcFrameStream *frame_stream_spawn(const StrList *cmd, int w, int h, double start,
                                         double step, bool nice)
{
    IcFrameStream *s = xmalloc(sizeof *s);
    memset(s, 0, sizeof *s);
    s->frame_bytes = (size_t)w * (size_t)h * 3;
    s->start = start;
    s->step = step;
    if (spawn_child(&s->child, cmd, nice ? PRIO_NICE : PRIO_NONE, IO_NULL, IO_PIPE, IO_NULL)) {
        free(s);
        return NULL;
    }
    return s;
}

/* ffmpeg für fortlaufende Bilder (und ggf. Ton) starten (siehe ic_core.h) */
IcFrameStream *ic_frame_stream_open(const char *video, double start, double length, int w, int h,
                                    double fps, bool thumbs, bool sound)
{
    if (w <= 0 || h <= 0 || fps <= 0 || length <= 0) return NULL;
    start = fmax(0, start);
    double origin = container_start_of(video), from = origin + start, to = from + length;
    StrList cmd = {0};
    add_stream_input(&cmd, video, start, thumbs);
    /* Ausgabe 1: Bilder als RGB24 über stdout; die Zeitachse beginnt bei start mit 0 */
    strlist_add_many(&cmd, "-map", "0:V:0", "-vf", NULL);
    strlist_add_printf(&cmd, "trim=start=%.6f:end=%.6f,setpts=PTS-STARTPTS,fps=%.6f,"
                       "scale=iw*sar:ih,scale=%d:%d:force_original_aspect_ratio=decrease,"
                       "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=black,format=rgb24", from, to, fps,
                       w, h, w, h);
    strlist_add_many(&cmd, "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1", NULL);
    /* Ausgabe 2: erste Tonspur ab derselben Stelle über PulseAudio/PipeWire; der kleine
     * Puffer hält ffmpeg nahe an Echtzeit, Bild und Ton bleiben so beieinander */
    if (sound) {
        strlist_add_many(&cmd, "-map", "0:a:0", "-af", NULL);
        strlist_add_printf(&cmd, "atrim=start=%.6f:end=%.6f,asetpts=PTS-STARTPTS", from, to);
        strlist_add_many(&cmd, "-f", "pulse", "-name", "Intro-Cutter", "-buffer_duration", "200",
                         "Vorschau", NULL);
    }
    IcFrameStream *s = frame_stream_spawn(&cmd, w, h, start, 1.0 / fps, thumbs);
    strlist_free(&cmd);
    return s;
}

/* Bilder einzeln, wie sie im Video stehen (siehe ic_core.h) */
IcFrameStream *ic_frame_stream_open_frames(const char *video, double from, double until,
                                           int count, int w, int h)
{
    if (w <= 0 || h <= 0 || count <= 0) return NULL;
    from = fmax(0, from);
    double origin = container_start_of(video);
    char end[48] = "";
    if (until > from) snprintf(end, sizeof end, ":end=%.6f", origin + until);
    StrList cmd = {0};
    add_stream_input(&cmd, video, from, false);
    /* passthrough: jedes Bild genau einmal, ohne Angleichen der Bildrate */
    strlist_add_many(&cmd, "-map", "0:V:0", "-fps_mode", "passthrough", "-frames:v", NULL);
    strlist_add_printf(&cmd, "%d", count);
    strlist_add(&cmd, "-vf");
    strlist_add_printf(&cmd, "trim=start=%.6f%s,setpts=PTS-STARTPTS,scale=iw*sar:ih,scale=%d:%d:"
                       "force_original_aspect_ratio=decrease,pad=%d:%d:(ow-iw)/2:(oh-ih)/2:"
                       "color=black,format=rgb24", origin + from, end, w, h, w, h);
    strlist_add_many(&cmd, "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1", NULL);
    IcFrameStream *s = frame_stream_spawn(&cmd, w, h, from, 0, false);
    strlist_free(&cmd);
    return s;
}

/* Zeitpunkte der Bilder eines Videos (siehe ic_core.h). Gelesen werden nur die Pakete
 * (ohne Dekodieren) – das geht auch für einige Minuten Video schnell. */
int ic_frame_times(const char *video, double start, double length, double **times)
{
    *times = NULL;
    /* -ss zählt ab der Startzeit des Containers; ffprobe -read_intervals dagegen absolut */
    double container_start = container_start_of(video);
    StrList cmd = {0};
    Buf out = {0};
    /* etwas früher und länger lesen: bei B-Bildern stehen die Pakete nicht in der
     * Reihenfolge der Anzeige. Das Ende absolut angeben – ein "+Dauer" zählte ab dem
     * Keyframe, auf dem ffprobe landet, und endete so bis zu einen Keyframe-Abstand zu früh. */
    strlist_add_many(&cmd, "ffprobe", "-v", "error", "-select_streams", "V:0", "-show_entries",
                     "packet=pts_time,dts_time", "-of", "csv=p=0", "-read_intervals", NULL);
    strlist_add_printf(&cmd, "%.6f%%%.6f", fmax(0, container_start + start - 1),
                       container_start + start + length + 3);
    strlist_add(&cmd, video);
    int rc = run_ffmpeg(&cmd, 60, &out, NULL);
    strlist_free(&cmd);
    int n = 0, cap = 0;
    double *list = NULL;
    char *cursor = out.p, *line, *fields[2];
    while (rc == 0 && (line = next_line(&cursor))) {
        double t;
        int found = split_csv_line(line, fields, 2);
        bool ok = (found >= 1 && parse_float(fields[0], &t)) ||     /* pts, sonst dts */
                  (found >= 2 && parse_float(fields[1], &t));
        if (!ok) continue;
        t -= container_start;
        if (t < start - 1e-6 || t > start + length + 1e-6) continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 1024;
            list = xrealloc(list, (size_t)cap * sizeof *list);
        }
        list[n++] = t;
    }
    buf_free(&out);
    if (n) qsort(list, (size_t)n, sizeof *list, compare_doubles);
    int unique = 0;                                     /* doppelte Zeitpunkte zusammenfassen */
    for (int i = 0; i < n; i++)
        if (!unique || list[i] - list[unique - 1] > 1e-7) list[unique++] = list[i];
    if (!unique) {
        free(list);
        return 0;
    }
    *times = list;
    return unique;
}

/* nächstes Bild lesen (siehe ic_core.h); wartet in kurzen Schritten, damit *stop wirkt */
bool ic_frame_stream_read(IcFrameStream *s, unsigned char *rgb, double *t,
                          const volatile int *stop)
{
    size_t got = 0;
    while (got < s->frame_bytes) {
        if ((stop && *stop) || s->child.out < 0) return false;
        struct pollfd pfd = {.fd = s->child.out, .events = POLLIN};
        int ready = poll(&pfd, 1, 50);
        if (ready < 0 && errno != EINTR) return false;
        if (ready <= 0) continue;
        ssize_t n = read(s->child.out, rgb + got, s->frame_bytes - got);
        if (n > 0) {
            got += (size_t)n;
        } else if (n == 0) {
            return false;                               /* ffmpeg ist fertig */
        } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            return false;
        }
    }
    if (t) *t = s->start + (double)s->frames_read * s->step;
    s->frames_read++;
    return true;
}

/* ffmpeg beenden und den Strom freigeben */
void ic_frame_stream_close(IcFrameStream *s)
{
    if (!s) return;
    kill_child(&s->child);
    close_fd(&s->child.out);
    if (!wait_child(&s->child, 5)) abandon_child(&s->child);
    free(s);
}

static pthread_once_t g_sound_once = PTHREAD_ONCE_INIT;
static bool g_sound_supported;

/* einmalig prüfen, ob ffmpeg die Ausgabe über PulseAudio kennt */
static void check_sound_support(void)
{
    StrList cmd = {0};
    strlist_add_many(&cmd, "ffmpeg", "-hide_banner", "-devices", NULL);
    Buf out = {0};
    g_sound_supported = run_capture_output(&cmd, false, 10, &out, NULL) == 0 && out.p &&
                        strstr(out.p, " pulse ") != NULL;
    buf_free(&out);
    strlist_free(&cmd);
}

/* kann ffmpeg Ton ausgeben? (siehe ic_core.h) */
bool ic_sound_supported(void)
{
    pthread_once(&g_sound_once, check_sound_support);
    return g_sound_supported;
}

/* ---------------------------------------------------------------------------
 * Hash-Cache ansehen und aufräumen (samt Kennbilder-Cache der automatischen Erkennung)
 * ------------------------------------------------------------------------- */
static long kb_cache_filter_videos(bool (*drop)(void *ctx, const char *path, long long size,
                                                long long mtime),
                                   void *ctx);
static void kb_cache_summary(long long *bytes, int *videos);
/* Cache-Schlüssel "realpath|Größe|mtime|Zeitpunkt|Pipeline" von rechts zerlegen (der
 * Pfad selbst darf '|' enthalten): Länge des Pfads und des Anfangs bis einschließlich
 * "mtime|", Größe, mtime, Outro-Eintrag? -> false bei fremdem Format */
static bool cache_key_split(const char *key, size_t *path_len, size_t *prefix_len,
                            long long *size, long long *mtime, bool *outro)
{
    const char *sep[4];                 /* die letzten vier '|', von hinten gezählt */
    int found = 0;
    for (const char *p = key + strlen(key); p > key && found < 4;)
        if (*--p == '|') sep[found++] = p;
    if (found < 4) return false;
    char *end;
    *size = strtoll(sep[3] + 1, &end, 10);
    if (end != sep[2] || end == sep[3] + 1) return false;
    *mtime = strtoll(sep[2] + 1, &end, 10);
    if (end != sep[1] || end == sep[2] + 1) return false;
    *path_len = (size_t)(sep[3] - key);
    *prefix_len = (size_t)(sep[1] - key) + 1;
    *outro = strstr(sep[0] + 1, "+eof") != NULL;
    return true;
}

/* Übersicht über den Cache: Videos mit Anzahl gespeicherter Bilder (siehe ic_core.h) */
bool ic_cache_read(IcCacheInfo *info)
{
    memset(info, 0, sizeof *info);
    kb_cache_summary(&info->kb_bytes, &info->kb_videos);
    struct stat st;
    if (stat(HASH_CACHE, &st) != 0) return info->kb_bytes > 0;
    info->hash_file = true;
    info->file_bytes = st.st_size;
    info->modified = (double)st.st_mtim.tv_sec;
    HashCache cache = {0};
    cache_load(&cache);
    info->entries = (long)cache.n;
    /* nach Schlüssel sortiert liegen alle Einträge eines Videos (gleicher Anfang
     * "realpath|Größe|mtime|") direkt hintereinander */
    char **keys = xmalloc((cache.n ? cache.n : 1) * sizeof *keys);
    memcpy(keys, cache.keys, cache.n * sizeof *keys);
    qsort(keys, cache.n, sizeof *keys, compare_str);
    int cap = 0;
    const char *group_key = NULL;
    size_t group_prefix = 0;
    for (size_t i = 0; i < cache.n; i++) {
        size_t path_len, prefix_len;
        long long size, mtime;
        bool outro;
        if (!cache_key_split(keys[i], &path_len, &prefix_len, &size, &mtime, &outro)) {
            info->other++;
            continue;
        }
        if (!group_key || prefix_len != group_prefix ||
            memcmp(keys[i], group_key, prefix_len) != 0) {
            if (info->n == cap) {
                cap = cap ? cap * 2 : 256;
                info->videos = xrealloc(info->videos, (size_t)cap * sizeof *info->videos);
            }
            info->videos[info->n++] = (IcCacheVideo){xstrndup(keys[i], path_len), size, mtime,
                                                     0, 0};
            group_key = keys[i];
            group_prefix = prefix_len;
        }
        info->videos[info->n - 1].frames++;
        info->videos[info->n - 1].outro_frames += outro;
    }
    free(keys);
    cache_free(&cache);
    return true;
}

/* Speicher einer Cache-Übersicht freigeben */
void ic_cache_info_free(IcCacheInfo *info)
{
    for (int i = 0; i < info->n; i++) free(info->videos[i].path);
    free(info->videos);
    memset(info, 0, sizeof *info);
}

/* Änderungszeit wie im Cache-Schlüssel (wie Pythons int(st_mtime)) */
static long long cache_mtime_of(const struct stat *st)
{
    return (long long)((double)st->st_mtim.tv_sec + (double)st->st_mtim.tv_nsec * 1e-9);
}

/* Cache neu schreiben ohne die Einträge, für die drop() true liefert -> Anzahl
 * entfernter Einträge, -1 bei Fehler. Die übrigen behalten ihre Reihenfolge. */
static long cache_filter(bool (*drop)(void *ctx, const char *key, size_t path_len,
                                      size_t prefix_len, long long size, long long mtime),
                         void *ctx)
{
    if (!is_file(HASH_CACHE)) return 0;
    HashCache cache = {0}, kept = {0};
    cache_load(&cache);
    long removed = 0;
    for (size_t i = 0; i < cache.n; i++) {
        size_t path_len, prefix_len;
        long long size, mtime;
        bool outro;
        bool remove = cache_key_split(cache.keys[i], &path_len, &prefix_len, &size, &mtime,
                                      &outro) &&
                      drop(ctx, cache.keys[i], path_len, prefix_len, size, mtime);
        if (remove) {
            removed++;
            free(cache.keys[i]);
            free(cache.values[i]);
        } else {
            cache_put(&kept, cache.keys[i], cache.values[i]);   /* übernimmt beide */
        }
    }
    free(cache.keys);                       /* die Strings gehören jetzt kept bzw. sind frei */
    free(cache.values);
    free(cache.slots);
    bool ok = removed == 0 || cache_save(&kept);
    cache_free(&kept);
    return ok ? removed : -1;
}

/* drop() für ic_cache_remove_videos: Pfad steht in der Menge ctx */
static bool drop_listed_video(void *ctx, const char *key, size_t path_len, size_t prefix_len,
                              long long size, long long mtime)
{
    (void)prefix_len;
    (void)size;
    (void)mtime;
    char *path = xstrndup(key, path_len);
    bool listed = cache_get(ctx, path) != NULL;
    free(path);
    return listed;
}

/* drop() für die Kennbilder in ic_cache_remove_videos: Pfad steht in der Menge ctx */
static bool kb_drop_listed(void *ctx, const char *path, long long size, long long mtime)
{
    (void)size;
    (void)mtime;
    return cache_get(ctx, path) != NULL;
}

/* alle Einträge bestimmter Videos entfernen (siehe ic_core.h) */
long ic_cache_remove_videos(const char *const *paths, int n)
{
    HashCache listed = {0};                 /* als Menge: Pfad -> "" */
    for (int i = 0; i < n; i++) cache_set(&listed, paths[i], "");
    long removed = cache_filter(drop_listed_video, &listed);
    long kb_removed = kb_cache_filter_videos(kb_drop_listed, &listed);
    cache_free(&listed);
    return removed < 0 || kb_removed < 0 ? -1 : removed + kb_removed;
}

/* drop() für ic_cache_remove_stale: Video fehlt oder hat sich geändert. ctx merkt sich
 * das Ergebnis je "realpath|Größe|mtime|", damit jedes Video nur einmal geprüft wird. */
static bool drop_stale_video(void *ctx, const char *key, size_t path_len, size_t prefix_len,
                             long long size, long long mtime)
{
    char *prefix = xstrndup(key, prefix_len);
    const char *known = cache_get(ctx, prefix);
    bool stale;
    if (known) {
        stale = *known == '1';
    } else {
        char *path = xstrndup(key, path_len);
        struct stat st;
        stale = stat(path, &st) != 0 || (long long)st.st_size != size ||
                cache_mtime_of(&st) != mtime;
        cache_set(ctx, prefix, stale ? "1" : "0");
        free(path);
    }
    free(prefix);
    return stale;
}

/* drop() für die Kennbilder in ic_cache_remove_stale: Video fehlt oder hat sich geändert */
static bool kb_drop_stale(void *ctx, const char *path, long long size, long long mtime)
{
    (void)ctx;
    struct stat st;
    return stat(path, &st) != 0 || (long long)st.st_size != size || cache_mtime_of(&st) != mtime;
}

/* Einträge fehlender oder geänderter Videos entfernen (siehe ic_core.h) */
long ic_cache_remove_stale(void)
{
    HashCache checked = {0};
    long removed = cache_filter(drop_stale_video, &checked);
    cache_free(&checked);
    long kb_removed = kb_cache_filter_videos(kb_drop_stale, NULL);
    return removed < 0 || kb_removed < 0 ? -1 : removed + kb_removed;
}

/* Cache-Dateien löschen (siehe ic_core.h) */
bool ic_cache_clear(void)
{
    char *kb = path_join(APP_DIR, "kennbilder.bin");
    char *tmp = xasprintf("%s.tmp", HASH_CACHE), *kb_tmp = xasprintf("%s.tmp", kb);
    unlink(tmp);
    unlink(kb_tmp);
    bool ok = unlink(HASH_CACHE) == 0 || errno == ENOENT;
    ok = (unlink(kb) == 0 || errno == ENOENT) && ok;
    free(tmp);
    free(kb_tmp);
    free(kb);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Automatische Intro-Erkennung: wiederkehrende Intros ohne Referenz-Video finden
 *
 * 1. Kennbilder: von jedem Video die ersten max_len + AUTO_EXTRA_SECS Sekunden Bild für
 *    Bild – Wahrnehmungs-Hash (wie bei der normalen Suche), Helligkeit und Detail
 *    (Streuung der Grauwerte; Schwarzbilder haben fast keins) und der echte Zeitstempel
 *    jedes Bilds. Sie bleiben in ~/.intro-cutter/kennbilder.bin, ein neuer Lauf liest nur
 *    neue oder geänderte Videos.
 * 2. Kandidaten: ein Index über die Hashes findet die Videos, die mit ähnlichen Bildern
 *    beginnen – auch bei 100 000 Videos schnell.
 * 3. Paarvergleich: genauer Versatz zweier Videos, dann Bild für Bild bis zum ersten
 *    Bild, ab dem sie sich unterscheiden (in der folgenden Sekunde sind mindestens 70 %
 *    der Bilder verschieden).
 * 4. Gruppen: verbundene Videos haben dasselbe Intro. Das Intro-Ende eines Videos ist
 *    der (untere) Median über seine Partner – Duplikate oder gleiche Fassungen verlängern
 *    es so nicht.
 * 5. Sicherheit: geschnitten wird nur, was ganz am Anfang beginnt (davor höchstens
 *    AUTO_MAX_LEAD_SECS Schwarzbild), innerhalb der Suchlänge endet, mindestens
 *    IC_AUTO_MIN_INTRO Sekunden dauert und in mindestens min_videos Videos vorkommt.
 *    Alles andere ist "unsicher" (mit Grund) und wird nicht vorausgewählt.
 * ------------------------------------------------------------------------- */
#define AUTO_EXTRA_SECS 5.0         /* über die Suchlänge hinaus lesen: das Ende erkennen */
#define AUTO_UNIFORM_DETAIL 12      /* Streuung ×2 darunter: gleichmäßiges Bild (Schwarzbild) */
#define AUTO_SAME_MEAN 10           /* gleichmäßige Bilder gleichen sich bis zu diesem Abstand */
#define AUTO_MATCH_BITS 10          /* Hash-Abstand für "gleiches Bild" */
#define AUTO_VOTE_BITS 6            /* strenger: für die Suche nach dem Versatz */
#define AUTO_CHANGE_BITS 4          /* so viel anders als der Vorgänger: Bild bewegt sich */
#define AUTO_MAX_LEAD_SECS 1.0      /* vor dem Intro höchstens so viel Schwarzbild */
#define AUTO_DIVERGE_SECS 1.0       /* Abweichung: in der folgenden Sekunde ... */
#define AUTO_DIVERGE_SHARE 0.7      /* ... mindestens 70 % der Bilder verschieden */
#define AUTO_ANCHOR_STEP 0.5        /* Indexbilder je Video alle 0,5 s */
#define AUTO_QUERY_STEP 0.25        /* gesucht wird damit alle 0,25 s */
#define AUTO_BUCKET_SCAN 32         /* je Indexfach höchstens so viele Einträge prüfen */
#define AUTO_MIN_VOTES 3            /* so viele Treffer beim gleichen Versatz: Kandidat */
#define AUTO_CANDIDATES 8           /* je Video so viele Kandidaten genau vergleichen */
#define AUTO_EXTRA_PARTNERS 12      /* fehlen Partner: so viele Vergleiche mehr als nötig */
#define AUTO_EDGE_SHARE 0.9         /* Ende so nah an der Suchgrenze: unsicher */
#define KB_PIPELINE "kb1"           /* Version der Kennbilder (Teil des Cache-Schlüssels) */
#define KB_MAGIC 0x3152424bu        /* Kennung eines Eintrags in kennbilder.bin ("KBR1") */
#define KB_COMPACT_BYTES (64LL << 20)   /* ab dieser Größe veraltete Einträge entfernen */

/* Kennbilder eines Videos (Bild für Bild, aufsteigende Zeit) */
typedef struct {
    int n;
    float *t;                       /* Zeitpunkt (s ab Dateistart, so wie -ss ihn versteht) */
    uint64_t *hash;
    uint8_t *mean;                  /* Helligkeit 0..255 */
    uint8_t *detail;                /* Streuung der Grauwerte ×2 */
    float read_secs;                /* so weit gelesen */
    bool exact;                     /* jedes Bild hat seinen eigenen Zeitstempel */
} Kennbilder;

/* Speicher für n Bilder anlegen */
static void kb_alloc(Kennbilder *k, int n)
{
    k->n = n;
    k->t = xmalloc((size_t)(n ? n : 1) * sizeof *k->t);
    k->hash = xmalloc((size_t)(n ? n : 1) * sizeof *k->hash);
    k->mean = xmalloc((size_t)(n ? n : 1));
    k->detail = xmalloc((size_t)(n ? n : 1));
}

static void kb_free(Kennbilder *k)
{
    free(k->t);
    free(k->hash);
    free(k->mean);
    free(k->detail);
    memset(k, 0, sizeof *k);
}

/* Kennbilder der ersten secs Sekunden lesen: ein ffmpeg-Lauf verkleinert jedes Bild
 * (Seitenverhältnis samt Pixelform beachtet) und liefert zweimal 32x32 Grauwerte – mit Rand
 * wie HASH_VF für den Hash, gestreckt für Helligkeit und Detail. showinfo nennt je Bild den
 * Zeitstempel als ganze Zahl samt Zeitbasis; mit -copyts bleiben die echten Zeitstempel
 * erhalten und werden auf den Dateistart umgerechnet – exakt wie -ss beim Schneiden, auch
 * bei TV-Aufnahmen mit großen Zeitstempeln. */
static bool kb_extract(const char *video, double secs, bool single_thread, Kennbilder *kb)
{
    memset(kb, 0, sizeof *kb);
    double origin = container_start_of(video);
    StrList cmd = {0};
    strlist_add_many(&cmd, "ffmpeg", "-nostdin", "-hide_banner", "-nostats", "-v", "info", NULL);
    if (single_thread) strlist_add_many(&cmd, "-threads", "1", "-filter_threads", "1", NULL);
    strlist_add_many(&cmd, "-copyts", "-i", video, "-filter_complex", NULL);
    strlist_add_printf(&cmd,
                       "[0:V:0]trim=end=%.6f,"
                       "scale=w='max(2,trunc(min(128,128*iw*sar/ih)/2)*2)'"
                       ":h='max(2,trunc(min(128,128*ih/(iw*sar))/2)*2)':flags=area,"
                       "showinfo,split[a][b];[a]" HASH_VF "[ha];[b]scale=32:32,format=gray[hb];"
                       "[ha][hb]hstack[out]", origin + secs);
    strlist_add_many(&cmd, "-map", "[out]", "-fps_mode", "passthrough", "-f", "rawvideo", "-",
                     NULL);
    Buf out = {0}, err = {0};
    int rc = run_ffmpeg(&cmd, 600, &out, &err);
    strlist_free(&cmd);
    const size_t frame_bytes = 2 * HASH_PX * HASH_PX;
    int frames = rc == 0 ? (int)(out.n / frame_bytes) : 0;
    /* Zeitbasis und Zeitstempel aus den showinfo-Zeilen */
    double tb = 0;
    long tb_num = 0, tb_den = 0;
    const char *config = err.p ? strstr(err.p, "config in time_base:") : NULL;
    if (config && sscanf(config, "config in time_base: %ld/%ld", &tb_num, &tb_den) == 2 &&
        tb_den > 0)
        tb = (double)tb_num / (double)tb_den;
    double *times = xmalloc((size_t)(frames ? frames : 1) * sizeof *times);
    int ntimes = 0;
    bool exact = tb > 0;
    char *cursor = err.p, *line;
    while (frames && cursor && (line = next_line(&cursor))) {
        const char *n_field = strstr(line, "] n:"), *pts_field;
        if (!n_field || !(pts_field = strstr(n_field, " pts:"))) continue;
        long long pts;
        if (sscanf(pts_field, " pts:%lld", &pts) != 1) {
            exact = false;                      /* Bild ohne Zeitstempel */
            continue;
        }
        if (ntimes < frames) times[ntimes++] = (double)pts * tb - origin;
    }
    int n = frames < ntimes ? frames : ntimes;
    exact = exact && frames == ntimes;
    for (int i = 1; i < n && exact; i++) exact = times[i] > times[i - 1];
    if (n > 0) {
        kb_alloc(kb, n);
        for (int i = 0; i < n; i++) {
            const unsigned char *row = (const unsigned char *)out.p + (size_t)i * frame_bytes;
            unsigned char hash_px[HASH_PX * HASH_PX];
            double sum = 0, sum2 = 0;
            for (int y = 0; y < HASH_PX; y++) {
                memcpy(hash_px + y * HASH_PX, row + y * 2 * HASH_PX, HASH_PX);
                for (int x = 0; x < HASH_PX; x++) {
                    double p = row[y * 2 * HASH_PX + HASH_PX + x];
                    sum += p;
                    sum2 += p * p;
                }
            }
            double mean = sum / (HASH_PX * HASH_PX);
            double spread = sqrt(fmax(0, sum2 / (HASH_PX * HASH_PX) - mean * mean));
            kb->t[i] = (float)times[i];
            kb->hash[i] = phash_of_32x32(hash_px);
            kb->mean[i] = (uint8_t)lround(mean);
            kb->detail[i] = (uint8_t)fmin(255, lround(spread * 2));
        }
        kb->read_secs = (float)secs;
        kb->exact = exact;
    }
    free(times);
    buf_free(&out);
    buf_free(&err);
    return n > 0;
}

/* ---- Kennbilder-Cache: ~/.intro-cutter/kennbilder.bin ----
 * Einträge werden nur angehängt: Kennung, Länge des Schlüssels, Schlüssel ("realpath|Größe|
 * mtime|kb1"), gelesene Sekunden, Anzahl Bilder, Merker (Bit 0: exakt), dann die Zeitpunkte,
 * Hashes, Helligkeiten und Details, zuletzt eine Prüfsumme. Gilt der neueste Eintrag je
 * Schlüssel; ein abgebrochener letzter Eintrag wird beim Öffnen abgeschnitten. */
typedef struct {
    FILE *file;
    char *path;
    HashCache where;                /* Schlüssel -> "Position:Länge" des neuesten Eintrags */
    long long stale_bytes;          /* durch neuere Einträge überholte Byte */
} KbCache;

/* FNV-1a (32 Bit) über Daten – Prüfsumme der Einträge */
static uint32_t kb_checksum(const void *data, size_t n)
{
    const unsigned char *p = data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

/* Cache-Schlüssel eines Videos (NULL, wenn es fehlt) */
static char *kb_key(const char *video)
{
    char *prefix;
    const char *suffix;
    if (!cache_key_parts(video, false, &prefix, &suffix)) return NULL;
    char *key = xasprintf("%s%s", prefix, KB_PIPELINE);
    free(prefix);
    return key;
}

/* nur die neuesten Einträge (ohne die mit drop[i] für c->where.keys[i]) in eine neue Datei
 * schreiben und sie an die Stelle der alten setzen -> false bei Fehler (dann ist der Cache
 * geschlossen; beim nächsten Öffnen wird das Verzeichnis neu aufgebaut) */
static bool kb_cache_rewrite(KbCache *c, const bool *drop)
{
    char *tmp = xasprintf("%s.tmp", c->path);
    FILE *out = fopen(tmp, "wbe");
    bool ok = out != NULL;
    char *copy = NULL;
    size_t copy_cap = 0;
    for (size_t i = 0; ok && i < c->where.n; i++) {
        long long pos, length;
        if (sscanf(c->where.values[i], "%lld:%lld", &pos, &length) != 2) continue;
        if (drop && drop[i]) continue;
        if ((size_t)length > copy_cap) {
            copy_cap = (size_t)length;
            copy = xrealloc(copy, copy_cap);
        }
        off_t new_pos = ftello(out);
        ok = fseeko(c->file, pos, SEEK_SET) == 0 &&
             fread(copy, 1, (size_t)length, c->file) == (size_t)length &&
             fwrite(copy, 1, (size_t)length, out) == (size_t)length;
        free(c->where.values[i]);
        c->where.values[i] = xasprintf("%lld:%lld", (long long)new_pos, length);
    }
    free(copy);
    ok = out && fclose(out) == 0 && ok;
    if (ok && rename(tmp, c->path) == 0) {
        fclose(c->file);
        c->file = fopen(c->path, "a+be");
        c->stale_bytes = 0;
    } else {
        ok = false;
        unlink(tmp);
        /* Positionen stimmen nicht mehr: Verzeichnis verwerfen, beim nächsten Mal neu */
        cache_free(&c->where);
        if (c->file) fclose(c->file);
        c->file = NULL;
    }
    free(tmp);
    return ok;
}

/* Datei einlesen: Inhaltsverzeichnis aufbauen, kaputten Rest abschneiden, bei viel
 * Veraltetem neu schreiben */
static void kb_cache_open(KbCache *c)
{
    memset(c, 0, sizeof *c);
    c->path = path_join(APP_DIR, "kennbilder.bin");
    mkdirs(APP_DIR);
    c->file = fopen(c->path, "a+be");
    if (!c->file) return;
    fseeko(c->file, 0, SEEK_SET);
    off_t valid_end = 0;
    for (;;) {
        off_t pos = ftello(c->file);
        uint32_t magic, key_len, n;
        float read_secs;
        uint8_t flags;
        if (fread(&magic, 4, 1, c->file) != 1 || magic != KB_MAGIC) break;
        if (fread(&key_len, 4, 1, c->file) != 1 || key_len == 0 || key_len > 65536) break;
        char *key = xmalloc(key_len + 1);
        bool ok = fread(key, 1, key_len, c->file) == key_len &&
                  fread(&read_secs, 4, 1, c->file) == 1 && fread(&n, 4, 1, c->file) == 1 &&
                  fread(&flags, 1, 1, c->file) == 1 && n < 1000000;
        long long length = 4 + 4 + (long long)key_len + 4 + 4 + 1 + (long long)n * 14 + 4;
        ok = ok && fseeko(c->file, pos + length, SEEK_SET) == 0;
        struct stat st;
        ok = ok && fstat(fileno(c->file), &st) == 0 && pos + length <= st.st_size;
        if (!ok) {
            free(key);
            break;
        }
        key[key_len] = 0;
        const char *old = cache_get(&c->where, key);
        long long old_pos, old_len;
        if (old && sscanf(old, "%lld:%lld", &old_pos, &old_len) == 2) c->stale_bytes += old_len;
        cache_put(&c->where, key, xasprintf("%lld:%lld", (long long)pos, length));
        valid_end = pos + length;
    }
    struct stat st;
    if (fstat(fileno(c->file), &st) == 0 && st.st_size > valid_end) {
        fflush(c->file);
        if (ftruncate(fileno(c->file), valid_end) != 0) {}      /* abgebrochener Rest */
    }
    if (valid_end > KB_COMPACT_BYTES && c->stale_bytes * 2 > valid_end)
        kb_cache_rewrite(c, NULL);              /* mehr als die Hälfte veraltet */
}

/* Kennbilder zu key aus dem Cache -> false, wenn keine (oder beschädigt) */
static bool kb_cache_get(KbCache *c, const char *key, Kennbilder *kb)
{
    memset(kb, 0, sizeof *kb);
    const char *where = c->file ? cache_get(&c->where, key) : NULL;
    long long pos, length;
    if (!where || sscanf(where, "%lld:%lld", &pos, &length) != 2 || length < 25) return false;
    unsigned char *rec = xmalloc((size_t)length);
    bool ok = fseeko(c->file, pos, SEEK_SET) == 0 &&
              fread(rec, 1, (size_t)length, c->file) == (size_t)length;
    uint32_t key_len, n, sum;
    float read_secs;
    if (ok) {
        memcpy(&key_len, rec + 4, 4);
        memcpy(&sum, rec + length - 4, 4);
        ok = sum == kb_checksum(rec + 4, (size_t)length - 8) &&
             (long long)key_len + 25 <= length;
    }
    if (ok) {
        const unsigned char *p = rec + 8 + key_len;
        memcpy(&read_secs, p, 4);
        memcpy(&n, p + 4, 4);
        uint8_t flags = p[8];
        p += 9;
        ok = n > 0 && 8 + (long long)key_len + 9 + (long long)n * 14 + 4 == length;
        if (ok) {
            kb_alloc(kb, (int)n);
            memcpy(kb->t, p, (size_t)n * 4);
            memcpy(kb->hash, p + (size_t)n * 4, (size_t)n * 8);
            memcpy(kb->mean, p + (size_t)n * 12, n);
            memcpy(kb->detail, p + (size_t)n * 13, n);
            kb->read_secs = read_secs;
            kb->exact = flags & 1;
        }
    }
    free(rec);
    return ok;
}

/* Kennbilder zu key anhängen */
static void kb_cache_put(KbCache *c, const char *key, const Kennbilder *kb)
{
    if (!c->file || kb->n <= 0) return;
    Buf rec = {0};
    uint32_t magic = KB_MAGIC, key_len = (uint32_t)strlen(key), n = (uint32_t)kb->n;
    uint8_t flags = kb->exact ? 1 : 0;
    buf_add(&rec, &magic, 4);
    buf_add(&rec, &key_len, 4);
    buf_add(&rec, key, key_len);
    buf_add(&rec, &kb->read_secs, 4);
    buf_add(&rec, &n, 4);
    buf_add(&rec, &flags, 1);
    buf_add(&rec, kb->t, (size_t)n * 4);
    buf_add(&rec, kb->hash, (size_t)n * 8);
    buf_add(&rec, kb->mean, n);
    buf_add(&rec, kb->detail, n);
    uint32_t sum = kb_checksum(rec.p + 4, rec.n - 4);
    buf_add(&rec, &sum, 4);
    fseeko(c->file, 0, SEEK_END);
    off_t pos = ftello(c->file);
    if (fwrite(rec.p, 1, rec.n, c->file) == rec.n && fflush(c->file) == 0) {
        const char *old = cache_get(&c->where, key);
        long long old_pos, old_len;
        if (old && sscanf(old, "%lld:%lld", &old_pos, &old_len) == 2) c->stale_bytes += old_len;
        cache_put(&c->where, xstrdup(key), xasprintf("%lld:%lld", (long long)pos,
                                                     (long long)rec.n));
    }
    buf_free(&rec);
}

static void kb_cache_close(KbCache *c)
{
    if (c->file) fclose(c->file);
    cache_free(&c->where);
    free(c->path);
    memset(c, 0, sizeof *c);
}

/* Schlüssel "realpath|Größe|mtime|kb1" von rechts zerlegen (der Pfad darf '|' enthalten)
 * -> false bei fremdem Format */
static bool kb_key_split(const char *key, size_t *path_len, long long *size, long long *mtime)
{
    const char *sep[3];                 /* die letzten drei '|', von hinten gezählt */
    int found = 0;
    for (const char *p = key + strlen(key); p > key && found < 3;)
        if (*--p == '|') sep[found++] = p;
    if (found < 3) return false;
    char *end;
    *size = strtoll(sep[2] + 1, &end, 10);
    if (end != sep[1] || end == sep[2] + 1) return false;
    *mtime = strtoll(sep[1] + 1, &end, 10);
    if (end != sep[0] || end == sep[1] + 1) return false;
    *path_len = (size_t)(sep[2] - key);
    return true;
}

/* Kennbilder-Cache neu schreiben ohne die Videos, für die drop() true liefert (dabei auch
 * überholte Einträge weglassen) -> Anzahl entfernter Videos, -1 bei Fehler */
static long kb_cache_filter_videos(bool (*drop)(void *ctx, const char *path, long long size,
                                                long long mtime),
                                   void *ctx)
{
    char *file = path_join(APP_DIR, "kennbilder.bin");
    bool exists = is_file(file);
    free(file);
    if (!exists) return 0;
    KbCache c;
    kb_cache_open(&c);
    if (!c.file) {
        kb_cache_close(&c);
        return -1;
    }
    bool *dropped = xmalloc((c.where.n ? c.where.n : 1) * sizeof *dropped);
    long removed = 0;
    for (size_t i = 0; i < c.where.n; i++) {
        size_t path_len;
        long long size, mtime;
        dropped[i] = false;
        if (!kb_key_split(c.where.keys[i], &path_len, &size, &mtime)) continue;
        char *path = xstrndup(c.where.keys[i], path_len);
        dropped[i] = drop(ctx, path, size, mtime);
        free(path);
        removed += dropped[i];
    }
    bool ok = (!removed && !c.stale_bytes) || kb_cache_rewrite(&c, dropped);
    free(dropped);
    kb_cache_close(&c);
    return ok ? removed : -1;
}

/* Größe des Kennbilder-Caches und Anzahl der Videos darin (0, wenn es ihn nicht gibt) */
static void kb_cache_summary(long long *bytes, int *videos)
{
    *bytes = 0;
    *videos = 0;
    char *file = path_join(APP_DIR, "kennbilder.bin");
    bool exists = is_file(file);
    free(file);
    if (!exists) return;
    KbCache c;
    kb_cache_open(&c);
    struct stat st;
    *videos = (int)c.where.n;
    if (c.file && fstat(fileno(c.file), &st) == 0) *bytes = st.st_size;
    kb_cache_close(&c);
}

/* ---- Vergleich zweier Videos ---- */

/* gleichmäßiges Bild (Schwarzbild, einfarbig)? */
static bool kb_uniform(const Kennbilder *k, int i)
{
    return k->detail[i] < AUTO_UNIFORM_DETAIL;
}

/* gleiches Bild? Gleichmäßige gleichen sich bei ähnlicher Helligkeit, sonst zählt der Hash */
static bool kb_same(const Kennbilder *a, int i, const Kennbilder *b, int j)
{
    bool uniform_a = kb_uniform(a, i), uniform_b = kb_uniform(b, j);
    if (uniform_a || uniform_b)
        return uniform_a && uniform_b && abs((int)a->mean[i] - (int)b->mean[j]) < AUTO_SAME_MEAN;
    return __builtin_popcountll(a->hash[i] ^ b->hash[j]) <= AUTO_MATCH_BITS;
}

/* Bild, das t am nächsten liegt */
static int kb_nearest(const Kennbilder *k, double t)
{
    int low = 0, high = k->n - 1;
    while (low < high) {
        int middle = low + (high - low) / 2;
        if (k->t[middle] < t)
            low = middle + 1;
        else
            high = middle;
    }
    if (low > 0 && fabs(k->t[low - 1] - t) <= fabs(k->t[low] - t)) low--;
    return low;
}

/* mittlere Bildrate */
static double kb_fps(const Kennbilder *k)
{
    double span = k->n > 1 ? k->t[k->n - 1] - k->t[0] : 0;
    return span > 0 ? (k->n - 1) / span : 25;
}

/* erstes informatives Bild, wenn davor nur gleichmäßige liegen und es höchstens
 * AUTO_MAX_LEAD_SECS nach dem Start kommt -> -1, wenn das Video so nicht beginnt */
static int kb_lead(const Kennbilder *k)
{
    for (int i = 0; i < k->n && k->t[i] <= AUTO_MAX_LEAD_SECS; i++)
        if (!kb_uniform(k, i)) return i;
    return -1;
}

static int compare_floats(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return x < y ? -1 : x > y;
}

/* Zahl der informativen Bilder von a bis max_len, die bei Versatz delta zu b passen */
static int kb_offset_score(const Kennbilder *a, int lead_a, const Kennbilder *b, double delta,
                           double max_len)
{
    double half_b = 0.6 / kb_fps(b);
    int score = 0;
    for (int i = lead_a; i < a->n && a->t[i] <= max_len; i++) {
        if (kb_uniform(a, i)) continue;
        int j = kb_nearest(b, a->t[i] - delta);
        score += fabs(b->t[j] - (a->t[i] - delta)) <= half_b && kb_same(a, i, b, j);
    }
    return score;
}

/* genauer Versatz delta = t_a - t_b zweier Videos. Kandidaten: der Abstand der ersten
 * informativen Bilder (Beginn des Intros), der Median der Zeitdifferenzen eindeutig
 * zuzuordnender Bilder (Bilder von a, die sich deutlich vom Vorgänger unterscheiden, und ihr
 * klar ähnlichstes Bild in b nahe t - estimate) und die Schätzung aus dem Index. Es gilt der
 * Kandidat, bei dem die meisten Bilder zusammenpassen – bei Gleichstand der Beginn des
 * Intros (steht ein Bild doppelt, z. B. 50 aus 25 Bildern/s, passen mehrere Versätze gleich
 * gut; maßgeblich ist dann das erste Bild). */
static double kb_exact_offset(const Kennbilder *a, int lead_a, const Kennbilder *b, int lead_b,
                              double estimate, double max_len)
{
    float *diffs = xmalloc((size_t)a->n * sizeof *diffs);
    int count = 0;
    for (int i = lead_a + 1; i < a->n && a->t[i] <= max_len; i++) {
        if (kb_uniform(a, i) || __builtin_popcountll(a->hash[i] ^ a->hash[i - 1]) <
                                    AUTO_CHANGE_BITS)
            continue;
        int center = kb_nearest(b, a->t[i] - estimate), best = -1, best_bits = 65,
            second_bits = 65;
        for (int j = center - 3; j <= center + 3; j++) {
            if (j < 0 || j >= b->n || kb_uniform(b, j)) continue;
            int bits = __builtin_popcountll(a->hash[i] ^ b->hash[j]);
            if (bits < best_bits) {
                second_bits = best_bits;
                best_bits = bits;
                best = j;
            } else if (bits < second_bits) {
                second_bits = bits;
            }
        }
        if (best >= 0 && best_bits <= AUTO_VOTE_BITS && second_bits - best_bits >= 2)
            diffs[count++] = a->t[i] - b->t[best];
    }
    double candidates[3] = {a->t[lead_a] - b->t[lead_b], estimate, estimate};
    int ncandidates = 2;
    if (count >= 3) {
        qsort(diffs, (size_t)count, sizeof *diffs, compare_floats);
        candidates[1] = diffs[count / 2];
        ncandidates = 3;
    }
    free(diffs);
    double delta = candidates[0];
    int best_score = kb_offset_score(a, lead_a, b, delta, max_len);
    for (int c = 1; c < ncandidates; c++) {
        int score = kb_offset_score(a, lead_a, b, candidates[c], max_len);
        if (score > best_score) {
            best_score = score;
            delta = candidates[c];
        }
    }
    return delta;
}

/* gemeinsamer Anfang von a mit b (Versatz delta): Bild für Bild vergleichen bis zum ersten
 * Bild, ab dem a dauerhaft anders ist -> false ohne gemeinsames Stück, das ganz am Anfang
 * beginnt (davor nur Schwarzbild). start: erstes gleiches informatives Bild von a, end:
 * erstes abweichendes Bild (INFINITY: bis zum Ende des Gelesenen gleich) */
static bool kb_shared_start(const Kennbilder *a, const Kennbilder *b, double delta, float *start,
                            float *end)
{
    double half_b = 0.6 / kb_fps(b);
    signed char *match = xmalloc((size_t)a->n);     /* 1 gleich, -1 anders, 0 ohne Partner */
    for (int i = 0; i < a->n; i++) {
        double tb = a->t[i] - delta;
        int j = kb_nearest(b, tb);
        if (fabs(b->t[j] - tb) > half_b) {
            match[i] = 0;
            continue;
        }
        bool same = kb_same(a, i, b, j);
        /* liegt das Bild genau zwischen zwei Bildern von b (z. B. 50 gegen 25 Bilder/s), gilt
         * auch der andere Nachbar – ein Schwarzbild muss aber zu beiden passen, damit am
         * Übergang nie ein schwarzes Inhaltsbild als Intro zählt */
        int other = b->t[j] < tb ? j + 1 : j - 1;
        if (other >= 0 && other < b->n &&
            fabs(fabs(b->t[other] - tb) - fabs(b->t[j] - tb)) < 2e-3) {
            bool same_other = kb_same(a, i, b, other);
            same = kb_uniform(a, i) ? same && same_other : same || same_other;
        }
        match[i] = same ? 1 : -1;
    }
    int first = -1;
    for (int i = 0; i < a->n && first < 0; i++)
        if (match[i] == 1 && !kb_uniform(a, i)) first = i;
    bool ok = first >= 0 && a->t[first] <= AUTO_MAX_LEAD_SECS;
    for (int i = 0; ok && i < first; i++) ok = kb_uniform(a, i);    /* davor nur Schwarzbild */
    double first_b = ok ? a->t[first] - delta : 0;
    ok = ok && first_b <= AUTO_MAX_LEAD_SECS + half_b;
    for (int j = 0; ok && j < b->n && b->t[j] < first_b - half_b; j++) ok = kb_uniform(b, j);
    *start = ok ? a->t[first] : 0;
    *end = INFINITY;
    if (ok) {
        int window = (int)lround(AUTO_DIVERGE_SECS * kb_fps(a));
        for (int k = first + 1; k < a->n; k++) {
            if (match[k] != -1) continue;
            int differ = 0, counted = 0;
            for (int i = k; i < a->n && i < k + window; i++)
                if (match[i]) {
                    counted++;
                    differ += match[i] == -1;
                }
            if (counted * 2 >= window && differ >= AUTO_DIVERGE_SHARE * counted) {
                *end = a->t[k];
                break;
            }
        }
        ok = *end - *start >= IC_AUTO_MIN_INTRO;
    }
    free(match);
    return ok;
}

/* ---- Suche über alle Videos ---- */

typedef struct {
    int a, b;
    float start_a, end_a, start_b, end_b;   /* gemeinsamer Anfang je Seite (s) */
} AutoPair;

typedef struct {
    int video, frame;
} AutoEntry;

typedef struct {                    /* Index über die Hashes: 4 Bänder à 16 Bit */
    uint32_t *first[4];             /* je Band 65537 Fachgrenzen */
    AutoEntry *entries[4];
} AutoIndex;

typedef struct {                    /* Kandidat aus dem Index */
    int video;
    float delta;
} AutoVote;

typedef struct {                    /* Menge schon verglichener Paare (offene Adressierung) */
    uint64_t *slots;
    size_t count, capacity;
} PairSet;

/* Paar a<b eintragen -> false, wenn es schon drin war */
static bool pair_set_add(PairSet *s, int a, int b)
{
    if ((s->count + 1) * 2 > s->capacity) {
        size_t old_capacity = s->capacity;
        uint64_t *old = s->slots;
        s->capacity = old_capacity ? old_capacity * 2 : 4096;
        s->slots = xmalloc(s->capacity * sizeof *s->slots);
        memset(s->slots, 0, s->capacity * sizeof *s->slots);
        s->count = 0;
        for (size_t i = 0; i < old_capacity; i++)
            if (old[i]) pair_set_add(s, (int)(old[i] >> 32) - 1, (int)(old[i] & 0xffffffffu) - 1);
        free(old);
    }
    if (a > b) {
        int swap = a;
        a = b;
        b = swap;
    }
    uint64_t key = (uint64_t)(a + 1) << 32 | (uint32_t)(b + 1);
    size_t i = (size_t)((key * 11400714819323198485ULL) >> 20) & (s->capacity - 1);
    while (s->slots[i]) {
        if (s->slots[i] == key) return false;
        i = (i + 1) & (s->capacity - 1);
    }
    s->slots[i] = key;
    s->count++;
    return true;
}

/* Wurzel im Union-Find (mit Pfadhalbierung) */
static int auto_root(int *parent, int x)
{
    while (parent[x] != x) x = parent[x] = parent[parent[x]];
    return x;
}

/* Indexbilder eines Videos: ab dem ersten informativen alle AUTO_ANCHOR_STEP Sekunden das
 * nächste informative Bild bis zur Suchlänge -> Anzahl (in frames) */
static int auto_anchor_frames(const Kennbilder *k, int lead, double max_len, double step,
                              int *frames, int max)
{
    int count = 0;
    double next = k->t[lead];
    for (int i = lead; i < k->n && k->t[i] <= max_len && count < max; i++) {
        if (k->t[i] + 1e-6 < next || kb_uniform(k, i)) continue;
        frames[count++] = i;
        next = k->t[i] + step;
    }
    return count;
}

/* Index über die Indexbilder aller Videos mit verwertbarem Anfang aufbauen */
static void auto_index_build(AutoIndex *ix, const Kennbilder *kb, const int *lead, int n,
                             double max_len)
{
    int max_anchors = (int)(max_len / AUTO_ANCHOR_STEP) + 2;
    int *frames = xmalloc((size_t)max_anchors * sizeof *frames);
    for (int band = 0; band < 4; band++) {
        ix->first[band] = xmalloc(65537 * sizeof(uint32_t));
        memset(ix->first[band], 0, 65537 * sizeof(uint32_t));
    }
    uint32_t *fill[4] = {NULL, NULL, NULL, NULL};
    for (int pass = 0; pass < 2; pass++) {      /* 1. Fächer zählen, 2. füllen */
        for (int band = 0; band < 4 && pass == 1; band++) {
            for (int b = 0; b < 65536; b++) ix->first[band][b + 1] += ix->first[band][b];
            ix->entries[band] = xmalloc((size_t)(ix->first[band][65536] + 1) * sizeof(AutoEntry));
            fill[band] = xmalloc(65536 * sizeof(uint32_t));
            memcpy(fill[band], ix->first[band], 65536 * sizeof(uint32_t));
        }
        for (int v = 0; v < n; v++) {
            if (lead[v] < 0) continue;
            int count = auto_anchor_frames(&kb[v], lead[v], max_len, AUTO_ANCHOR_STEP, frames,
                                           max_anchors);
            for (int a = 0; a < count; a++)
                for (int band = 0; band < 4; band++) {
                    unsigned bucket = (unsigned)(kb[v].hash[frames[a]] >> (16 * band)) & 0xffff;
                    if (pass == 0)
                        ix->first[band][bucket + 1]++;
                    else
                        ix->entries[band][fill[band][bucket]++] = (AutoEntry){v, frames[a]};
                }
        }
        for (int band = 0; band < 4 && pass == 1; band++) free(fill[band]);
    }
    free(frames);
}

static void auto_index_free(AutoIndex *ix)
{
    for (int band = 0; band < 4; band++) {
        free(ix->first[band]);
        free(ix->entries[band]);
    }
}

static int compare_votes(const void *a, const void *b)
{
    const AutoVote *x = a, *y = b;
    if (x->video != y->video) return x->video - y->video;
    return x->delta < y->delta ? -1 : x->delta > y->delta;
}

typedef struct {
    int video, votes;
    float delta;
} AutoCandidate;

static int compare_candidates(const void *a, const void *b)
{
    const AutoCandidate *x = a, *y = b;
    return y->votes != x->votes ? y->votes - x->votes : x->video - y->video;
}

/* Kandidaten für Video v: Bilder seines Anfangs im Index nachschlagen, Treffer je Video und
 * Versatz zählen -> Anzahl (die besten AUTO_CANDIDATES in out) */
static int auto_candidates(const AutoIndex *ix, const Kennbilder *kb, const int *lead, int v,
                           double max_len, AutoCandidate *out)
{
    const Kennbilder *k = &kb[v];
    int max_queries = (int)(max_len / AUTO_QUERY_STEP) + 2;
    int *frames = xmalloc((size_t)max_queries * sizeof *frames);
    int queries = auto_anchor_frames(k, lead[v], max_len, AUTO_QUERY_STEP, frames, max_queries);
    size_t nvotes = 0, cap = 1024;
    AutoVote *votes = xmalloc(cap * sizeof *votes);
    for (int q = 0; q < queries; q++) {
        uint64_t h = k->hash[frames[q]];
        for (int band = 0; band < 4; band++) {
            unsigned bucket = (unsigned)(h >> (16 * band)) & 0xffff;
            uint32_t from = ix->first[band][bucket], to = ix->first[band][bucket + 1];
            uint32_t scan[AUTO_BUCKET_SCAN], nscan = 0;
            if (to - from <= AUTO_BUCKET_SCAN) {
                for (uint32_t e = from; e < to; e++) scan[nscan++] = e;
            } else {
                /* volles Fach (gleiche Bilder in vielen Videos): die Nachbarn in der
                 * Pfad-Reihenfolge – Folgen einer Serie liegen meist im selben Ordner –,
                 * dazu eine je Video und Abfrage wechselnde Stichprobe aus dem Rest */
                uint32_t low = from, high = to;
                while (low < high) {                /* erster Eintrag mit Video >= v */
                    uint32_t middle = low + (high - low) / 2;
                    if (ix->entries[band][middle].video < v)
                        low = middle + 1;
                    else
                        high = middle;
                }
                uint32_t near = AUTO_BUCKET_SCAN * 3 / 4, size = to - from;
                uint32_t start = low >= from + near / 2 ? low - near / 2 : from;
                if (start + near > to) start = to - near;
                for (uint32_t e = start; e < start + near; e++) scan[nscan++] = e;
                uint64_t mix = ((uint64_t)(unsigned)v * 0x9E3779B97F4A7C15ULL) ^
                               ((uint64_t)(unsigned)(q * 4 + band) * 0xC2B2AE3D27D4EB4FULL);
                mix ^= mix >> 31;
                uint32_t offset = (uint32_t)((mix * 0xBF58476D1CE4E5B9ULL) >> 32) % size;
                for (uint32_t s = 0; nscan < AUTO_BUCKET_SCAN && s < size; s++) {
                    uint32_t e = from + (offset + s) % size;
                    if (e < start || e >= start + near) scan[nscan++] = e;
                }
            }
            for (uint32_t s = 0; s < nscan; s++) {
                AutoEntry entry = ix->entries[band][scan[s]];
                if (entry.video == v) continue;
                const Kennbilder *other = &kb[entry.video];
                if (__builtin_popcountll(h ^ other->hash[entry.frame]) > AUTO_VOTE_BITS) continue;
                if (nvotes == cap) {
                    cap *= 2;
                    votes = xrealloc(votes, cap * sizeof *votes);
                }
                votes[nvotes++] = (AutoVote){entry.video, k->t[frames[q]] - other->t[entry.frame]};
            }
        }
    }
    free(frames);
    qsort(votes, nvotes, sizeof *votes, compare_votes);
    /* je Video der dichteste Versatz (±40 ms) */
    int ncand = 0, cand_cap = 64;
    AutoCandidate *cand = xmalloc((size_t)cand_cap * sizeof *cand);
    for (size_t i = 0; i < nvotes;) {
        size_t end = i;
        while (end < nvotes && votes[end].video == votes[i].video) end++;
        int best = 0;
        float best_delta = 0;
        for (size_t lo = i, hi = i; hi < end; hi++) {
            while (votes[hi].delta - votes[lo].delta > 0.08f) lo++;
            if ((int)(hi - lo + 1) > best) {
                best = (int)(hi - lo + 1);
                best_delta = votes[lo + (hi - lo) / 2].delta;
            }
        }
        if (best >= AUTO_MIN_VOTES) {
            if (ncand == cand_cap) {
                cand_cap *= 2;
                cand = xrealloc(cand, (size_t)cand_cap * sizeof *cand);
            }
            cand[ncand++] = (AutoCandidate){votes[i].video, best, best_delta};
        }
        i = end;
    }
    free(votes);
    qsort(cand, (size_t)ncand, sizeof *cand, compare_candidates);
    int count = ncand < AUTO_CANDIDATES ? ncand : AUTO_CANDIDATES;
    memcpy(out, cand, (size_t)count * sizeof *out);
    free(cand);
    return count;
}

/* zwei Videos genau vergleichen -> true und pair, wenn sie ein gemeinsames Intro haben */
static bool auto_compare(const Kennbilder *kb, const int *lead, int a, int b, double estimate,
                         double max_len, AutoPair *pair)
{
    double delta = kb_exact_offset(&kb[a], lead[a], &kb[b], lead[b], estimate, max_len);
    float start_a, end_a, start_b, end_b;
    if (!kb_shared_start(&kb[a], &kb[b], delta, &start_a, &end_a) ||
        !kb_shared_start(&kb[b], &kb[a], -delta, &start_b, &end_b))
        return false;
    *pair = (AutoPair){a, b, start_a, end_a, start_b, end_b};
    return true;
}

typedef struct {
    AutoPair *v;
    size_t n, cap;
} AutoPairs;

static void auto_pairs_add(AutoPairs *p, const AutoPair *pair)
{
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 1024;
        p->v = xrealloc(p->v, p->cap * sizeof *p->v);
    }
    p->v[p->n++] = *pair;
}

/* Kennbilder parallel lesen (Anzahl regelt der Governor) */
typedef struct {
    DoneNode node;                  /* muss erstes Element sein */
    pthread_t thr;
    bool joinable;
    int index;
    const char *path;
    double secs;
    bool single;
    Gate *gate;
    DoneQueue *dq;
    Kennbilder kb;
    bool ok;
} KbTask;

static void *kb_task_thread(void *arg)
{
    KbTask *t = arg;
    t->ok = kb_extract(t->path, t->secs, t->single, &t->kb);
    gate_release(t->gate);
    done_queue_push(t->dq, &t->node);
    return NULL;
}

/* fehlende Kennbilder lesen: todo[0..ntodo) sind Indizes in videos; Ergebnisse nach kb */
static void auto_read_kennbilder(const VideoList *videos, const int *todo, int ntodo,
                                 double secs, KbCache *cache, Kennbilder *kb, bool *readable)
{
    Governor gov;                               /* kurze Jobs: ohne Einschwingzeit regeln */
    governor_init(&gov, MAX_WORKERS, CPU_COUNT / 2 > 2 ? CPU_COUNT / 2 : 2, 0.0);
    Gate *gate = gate_new(0, &gov, "CPU");
    DoneQueue done_queue;
    done_queue_init(&done_queue);
    bool single = ntodo >= CPU_COUNT;           /* viele Dateien -> je ein Decoder-Thread */
    int next = 0, inflight = 0, done = 0;
    while ((next < ntodo || inflight) && !ic_cancel) {
        while (next < ntodo && inflight < POOL_THREADS && gate_try_acquire(gate)) {
            KbTask *t = xmalloc(sizeof *t);
            memset(t, 0, sizeof *t);
            t->index = todo[next++];
            t->path = videos->v[t->index].path;
            t->secs = secs;
            t->single = single;
            t->gate = gate;
            t->dq = &done_queue;
            inflight++;
            if (start_thread(&t->thr, kb_task_thread, t) == 0)
                t->joinable = true;
            else
                kb_task_thread(t);
        }
        if (!inflight) {
            sleep_secs(0.2);
            continue;
        }
        for (DoneNode *d = done_queue_wait(&done_queue, 0.5); d;) {
            KbTask *t = (KbTask *)d;
            d = d->next;
            inflight--;
            if (t->joinable) pthread_join(t->thr, NULL);
            if (!ic_cancel && t->ok) {
                char *key = kb_key(t->path);
                if (key) kb_cache_put(cache, key, &t->kb);
                free(key);
                kb[t->index] = t->kb;
                readable[t->index] = true;
            } else {
                kb_free(&t->kb);
            }
            done++;
            if (ic_fe.detect_progress && !ic_cancel) {
                int target;
                double cpu;
                governor_status(&gov, &target, &cpu);
                ic_fe.detect_progress(done, ntodo, target, gov.max, cpu);
            }
            free(t);
        }
    }
    while (inflight)                            /* Abbruch: laufende Leser abwarten */
        for (DoneNode *d = done_queue_wait(&done_queue, 0.5); d; inflight--) {
            KbTask *t = (KbTask *)d;
            d = d->next;
            if (t->joinable) pthread_join(t->thr, NULL);
            kb_free(&t->kb);
            free(t);
        }
    done_queue_destroy(&done_queue);
    gate_free(gate);
    governor_stop(&gov);
}

/* Ergebnis eines Videos aus seinen Paarvergleichen. Intro-Ende: die späteste Stelle, bis zu
 * der mindestens min_videos - 1 Partner denselben Anfang haben – das Stück kommt also in
 * mindestens min_videos Videos vor. Partner mit kürzerer Intro-Fassung ziehen es so nicht
 * nach vorn, einzelne Duplikate nicht nach hinten. Schnitt eine Viertel Bilddauer vor dem
 * ersten Bild danach. *duration: Länge des gemeinsamen Stücks (Vergleich in der Gruppe),
 * *chosen_end: das gewählte Intro-Ende (s ab Dateistart). */
static void auto_judge_video(const Kennbilder *k, const float *ends, const float *starts,
                             int count, int component_size, double max_len, int min_videos,
                             IcAutoVideo *info, double *duration, float *chosen_end)
{
    int needed = min_videos - 1;
    float *sorted = xmalloc((size_t)(count ? count : 1) * sizeof *sorted);
    memcpy(sorted, ends, (size_t)count * sizeof *sorted);
    qsort(sorted, (size_t)count, sizeof *sorted, compare_floats);
    /* das needed-größte Ende; bei zu wenigen Partnern vorsichtshalber das früheste */
    float end = count >= needed ? sorted[count - needed] : count ? sorted[0] : INFINITY;
    memcpy(sorted, starts, (size_t)count * sizeof *sorted);
    qsort(sorted, (size_t)count, sizeof *sorted, compare_floats);
    float start = count ? sorted[(count - 1) / 2] : 0;
    free(sorted);
    *duration = end - start;
    *chosen_end = end;
    info->cut_len = 0;
    if (isfinite(end) && end <= max_len) {
        int first = kb_nearest(k, end);         /* erstes Bild nach dem Intro */
        if (first > 0) {
            double previous = k->t[first - 1];
            info->last_intro_t = previous;
            info->first_content_t = k->t[first];
            info->cut_len = round((k->t[first] - (k->t[first] - previous) / 4) * 1000) / 1000;
        }
    }
    info->sure = false;
    if (info->cut_len <= 0 && count && (!isfinite(end) || end > max_len))
        snprintf(info->reason, sizeof info->reason,
                 "Gleicher Anfang reicht über die Suchlänge (%.0f s) hinaus – längeres Intro "
                 "oder gleicher Inhalt", max_len);
    else if (info->cut_len <= 0)
        snprintf(info->reason, sizeof info->reason, "Kein Intro-Ende gefunden");
    else if (component_size < min_videos)
        snprintf(info->reason, sizeof info->reason,
                 "Intro nur in %d Videos gefunden (mindestens %d nötig)", component_size,
                 min_videos);
    else if (count < needed)
        snprintf(info->reason, sizeof info->reason, "Zu wenige Vergleiche (%d)", count);
    else if (!k->exact)
        snprintf(info->reason, sizeof info->reason, "Zeitstempel der Bilder nicht eindeutig");
    else if (end - start < IC_AUTO_MIN_INTRO)
        snprintf(info->reason, sizeof info->reason, "Kürzer als %.1f s", IC_AUTO_MIN_INTRO);
    else if (end > AUTO_EDGE_SHARE * max_len)
        snprintf(info->reason, sizeof info->reason,
                 "Ende knapp vor der Suchgrenze – zur Sicherheit die Suchlänge erhöhen");
    else
        info->sure = true;
    if (info->sure) info->reason[0] = 0;
}

typedef struct {
    int index;                      /* Gruppe vor dem Sortieren */
    int members;
} AutoGroupOrder;

typedef struct {                    /* Mitglied einer Gruppe mit einem Wert (Länge, Schnitt) */
    int group;
    double value;
    int video;
} AutoMember;

/* qsort-Vergleich: nach Gruppe, darin nach Wert */
static int compare_members(const void *a, const void *b)
{
    const AutoMember *x = a, *y = b;
    if (x->group != y->group) return x->group - y->group;
    return x->value < y->value ? -1 : x->value > y->value;
}

static int compare_group_order(const void *a, const void *b)
{
    const AutoGroupOrder *x = a, *y = b;
    return y->members != x->members ? y->members - x->members : x->index - y->index;
}

/* reicht der gemeinsame Anfang des Paars bei Video v (Seite a bzw. b) bis zu dessen
 * Intro-Ende (auf 1,5 Bilder genau)? */
static bool auto_pair_covers(const AutoPair *pair, bool side_a, const float *chosen_end,
                             const Kennbilder *kb)
{
    int v = side_a ? pair->a : pair->b;
    float end = side_a ? pair->end_a : pair->end_b;
    return isfinite(chosen_end[v]) && end >= chosen_end[v] - 1.5 / kb_fps(&kb[v]);
}

typedef struct {                    /* kürzere Intro-Fassung (Familie from) in längerer (to) */
    int from, to;
} AutoLink;

static int compare_links(const void *a, const void *b)
{
    const AutoLink *x = a, *y = b;
    return x->from != y->from ? x->from - y->from : x->to - y->to;
}

/* Gruppe je Video mit Schnitt (Index des Wurzel-Videos). Zwei Videos haben dasselbe Intro,
 * wenn ihr gemeinsamer Anfang bei beiden bis zum Intro-Ende reicht ("Familie"). Eine kürzere
 * Fassung (das Paar reicht nur bei ihr bis zum Ende, beim Partner geht das Intro weiter) kommt
 * zu der längeren, mit der sie am häufigsten verglichen wurde – so bleibt ein gekürztes Intro
 * bei seiner Serie, aber ein gemeinsamer Sender-Vorspann verbindet keine verschiedenen
 * Serien (ihre Intros gehen danach auseinander, das Paar reicht bei keinem bis zum Ende). */
static int *auto_group_roots(const AutoPairs *pairs, const IcAutoVideo *info,
                             const float *chosen_end, const double *duration,
                             const Kennbilder *kb, int n)
{
    int *family = xmalloc((size_t)(n ? n : 1) * sizeof *family);
    for (int i = 0; i < n; i++) family[i] = i;
    for (size_t p = 0; p < pairs->n; p++) {
        const AutoPair *pair = &pairs->v[p];
        if (info[pair->a].cut_len <= 0 || info[pair->b].cut_len <= 0) continue;
        if (auto_pair_covers(pair, true, chosen_end, kb) &&
            auto_pair_covers(pair, false, chosen_end, kb))
            family[auto_root(family, pair->a)] = auto_root(family, pair->b);
    }
    /* typische Länge je Familie (Median der gemeinsamen Stücke) */
    AutoMember *by_family = xmalloc((size_t)(n ? n : 1) * sizeof *by_family);
    double *family_length = xmalloc((size_t)(n ? n : 1) * sizeof *family_length);
    int count = 0;
    for (int v = 0; v < n; v++)
        if (info[v].cut_len > 0) by_family[count++] = (AutoMember){auto_root(family, v),
                                                                   duration[v], v};
    qsort(by_family, (size_t)count, sizeof *by_family, compare_members);
    for (int i = 0; i < count;) {
        int j = i;
        while (j < count && by_family[j].group == by_family[i].group) j++;
        family_length[by_family[i].group] = by_family[i + (j - i - 1) / 2].value;
        i = j;
    }
    free(by_family);
    /* kürzere Fassung -> längere: Belege je Familienpaar */
    AutoLink *links = xmalloc((pairs->n ? pairs->n : 1) * sizeof *links);
    size_t nlinks = 0;
    for (size_t p = 0; p < pairs->n; p++) {
        const AutoPair *pair = &pairs->v[p];
        if (info[pair->a].cut_len <= 0 || info[pair->b].cut_len <= 0) continue;
        bool full_a = auto_pair_covers(pair, true, chosen_end, kb);
        bool full_b = auto_pair_covers(pair, false, chosen_end, kb);
        if (full_a == full_b) continue;
        int from = auto_root(family, full_a ? pair->a : pair->b);
        int to = auto_root(family, full_a ? pair->b : pair->a);
        if (from != to && family_length[to] > family_length[from]) /* nur zur längeren */
            links[nlinks++] = (AutoLink){from, to};
    }
    qsort(links, nlinks, sizeof *links, compare_links);
    int *up = xmalloc((size_t)(n ? n : 1) * sizeof *up);
    for (int i = 0; i < n; i++) up[i] = -1;
    for (size_t i = 0; i < nlinks;) {           /* je Familie die am besten belegte längere */
        size_t j = i, best_count = 0;
        int best = -1;
        while (j < nlinks && links[j].from == links[i].from) {
            size_t k = j;
            while (k < nlinks && links[k].from == links[j].from && links[k].to == links[j].to)
                k++;
            if (k - j > best_count) {
                best_count = k - j;
                best = links[j].to;
            }
            j = k;
        }
        up[links[i].from] = best;
        i = j;
    }
    free(links);
    /* Wurzel: entlang der längeren Fassungen (die Länge wächst, also ohne Kreis) */
    int *root = xmalloc((size_t)(n ? n : 1) * sizeof *root);
    for (int v = 0; v < n; v++) {
        int r = auto_root(family, v);
        for (int steps = 0; up[r] >= 0 && steps < n; steps++) r = up[r];
        root[v] = r;
    }
    free(up);
    free(family_length);
    free(family);
    return root;
}

/* die eigentliche Suche; ic_detect_auto sperrt drumherum den Standby */
static int detect_auto(const char *src, const char *out, double max_len, int min_videos, IcAuto *a,
                       const char **err)
{
    memset(a, 0, sizeof *a);
    *err = NULL;
    a->max_len = max_len;
    a->min_videos = min_videos;
    double started_at = monotonic_secs(), secs = max_len + AUTO_EXTRA_SECS;
    find_videos(src, out, &a->videos);
    if (ic_cancel) return 1;
    int n = a->videos.n;
    if (!n) {
        *err = "Keine Videos im Quellordner gefunden.";
        return -1;
    }
    a->info = xmalloc((size_t)n * sizeof *a->info);
    memset(a->info, 0, (size_t)n * sizeof *a->info);
    for (int i = 0; i < n; i++) a->info[i].group = -1;

    /* 1. Kennbilder: aus dem Cache, sonst lesen */
    Kennbilder *kb = xmalloc((size_t)n * sizeof *kb);
    memset(kb, 0, (size_t)n * sizeof *kb);
    bool *readable = xmalloc((size_t)n);
    memset(readable, 0, (size_t)n);
    int *todo = xmalloc((size_t)n * sizeof *todo), ntodo = 0;
    KbCache cache;
    kb_cache_open(&cache);
    for (int i = 0; i < n && !ic_cancel; i++) {
        char *key = kb_key(a->videos.v[i].path);
        Kennbilder cached = {0};
        if (key && kb_cache_get(&cache, key, &cached) && cached.read_secs >= secs - 1e-3) {
            kb[i] = cached;
            readable[i] = true;
        } else {
            kb_free(&cached);
            todo[ntodo++] = i;
        }
        free(key);
    }
    if (ic_fe.detect_start && !ic_cancel) ic_fe.detect_start(n, n - ntodo, ntodo);
    if (!ic_cancel && ntodo) auto_read_kennbilder(&a->videos, todo, ntodo, secs, &cache, kb,
                                                  readable);
    kb_cache_close(&cache);
    free(todo);
    if (ic_cancel) {
        for (int i = 0; i < n; i++) kb_free(&kb[i]);
        free(kb);
        free(readable);
        return 1;
    }
    if (ic_fe.detect_stage) ic_fe.detect_stage("Intros werden verglichen …");

    /* 2. Index über die Anfänge, 3. Kandidaten genau vergleichen */
    int *lead = xmalloc((size_t)n * sizeof *lead), *parent = xmalloc((size_t)n * sizeof *parent);
    for (int i = 0; i < n; i++) {
        lead[i] = readable[i] ? kb_lead(&kb[i]) : -1;
        parent[i] = i;
        if (!readable[i]) {
            a->unreadable++;
            snprintf(a->info[i].reason, sizeof a->info[i].reason, "Nicht lesbar");
        } else if (lead[i] < 0) {
            snprintf(a->info[i].reason, sizeof a->info[i].reason,
                     "Beginnt nicht mit einem Intro (Bild schon vor %.0f s)", AUTO_MAX_LEAD_SECS);
        }
    }
    AutoIndex index;
    auto_index_build(&index, kb, lead, n, max_len);
    AutoPairs pairs = {0};
    PairSet compared = {0};
    AutoCandidate candidates[AUTO_CANDIDATES];
    for (int v = 0; v < n && !ic_cancel; v++) {
        if (lead[v] < 0) continue;
        int count = auto_candidates(&index, kb, lead, v, max_len, candidates);
        for (int c = 0; c < count; c++) {
            int u = candidates[c].video;
            if (lead[u] < 0 || !pair_set_add(&compared, v, u)) continue;
            AutoPair pair;
            if (auto_compare(kb, lead, v, u, candidates[c].delta, max_len, &pair)) {
                auto_pairs_add(&pairs, &pair);
                parent[auto_root(parent, v)] = auto_root(parent, u);
            }
        }
    }
    auto_index_free(&index);

    /* 4. Gruppen: Mitglieder je Komponente, fehlende Partner ergänzen */
    int *component_size = xmalloc((size_t)n * sizeof *component_size);
    memset(component_size, 0, (size_t)n * sizeof *component_size);
    for (int i = 0; i < n; i++)
        if (lead[i] >= 0) component_size[auto_root(parent, i)]++;
    int *partner_count = xmalloc((size_t)n * sizeof *partner_count);
    memset(partner_count, 0, (size_t)n * sizeof *partner_count);
    for (size_t p = 0; p < pairs.n; p++) {
        partner_count[pairs.v[p].a]++;
        partner_count[pairs.v[p].b]++;
    }
    /* Mitglieder je Komponente (für zusätzliche Vergleiche) */
    int *members_start = xmalloc((size_t)(n + 1) * sizeof *members_start);
    int *members = xmalloc((size_t)(n ? n : 1) * sizeof *members);
    memset(members_start, 0, (size_t)(n + 1) * sizeof *members_start);
    for (int i = 0; i < n; i++)
        if (lead[i] >= 0 && component_size[auto_root(parent, i)] >= 2)
            members_start[auto_root(parent, i) + 1]++;
    for (int i = 0; i < n; i++) members_start[i + 1] += members_start[i];
    int *fill = xmalloc((size_t)(n ? n : 1) * sizeof *fill);
    memcpy(fill, members_start, (size_t)n * sizeof *fill);
    for (int i = 0; i < n; i++)
        if (lead[i] >= 0 && component_size[auto_root(parent, i)] >= 2)
            members[fill[auto_root(parent, i)]++] = i;
    free(fill);
    int needed = min_videos - 1;               /* Partner je Video (siehe auto_judge_video) */
    for (int v = 0; v < n && !ic_cancel; v++) {
        if (lead[v] < 0 || partner_count[v] >= needed) continue;
        int root = auto_root(parent, v);
        if (component_size[root] < 2) continue;
        /* zuerst die Mitglieder mit den meisten Vergleichen (gut verbunden) */
        int tried = 0;
        for (int round = 0; round < 2 && partner_count[v] < needed; round++)
            for (int m = members_start[root]; m < members_start[root + 1] &&
                                              partner_count[v] < needed &&
                                              tried < needed + AUTO_EXTRA_PARTNERS; m++) {
                int u = members[m];
                if (u == v || (round == 0 && partner_count[u] < needed)) continue;
                if (!pair_set_add(&compared, v, u)) continue;
                tried++;
                AutoPair pair;
                if (auto_compare(kb, lead, v, u, kb[v].t[lead[v]] - kb[u].t[lead[u]], max_len,
                                 &pair)) {
                    auto_pairs_add(&pairs, &pair);
                    partner_count[v]++;
                    partner_count[u]++;
                }
            }
    }
    free(members_start);
    free(members);

    /* 5. je Video: Enden seiner Paare -> Schnitt und Urteil */
    int *pair_start = xmalloc((size_t)(n + 1) * sizeof *pair_start);
    memset(pair_start, 0, (size_t)(n + 1) * sizeof *pair_start);
    for (size_t p = 0; p < pairs.n; p++) {
        pair_start[pairs.v[p].a + 1]++;
        pair_start[pairs.v[p].b + 1]++;
    }
    for (int i = 0; i < n; i++) pair_start[i + 1] += pair_start[i];
    size_t total_ends = (size_t)pair_start[n];
    float *ends = xmalloc((total_ends ? total_ends : 1) * sizeof *ends);
    float *starts = xmalloc((total_ends ? total_ends : 1) * sizeof *starts);
    int *slot = xmalloc((size_t)(n ? n : 1) * sizeof *slot);
    memcpy(slot, pair_start, (size_t)n * sizeof *slot);
    for (size_t p = 0; p < pairs.n; p++) {
        const AutoPair *pair = &pairs.v[p];
        ends[slot[pair->a]] = pair->end_a;
        starts[slot[pair->a]++] = pair->start_a;
        ends[slot[pair->b]] = pair->end_b;
        starts[slot[pair->b]++] = pair->start_b;
    }
    free(slot);
    double *duration = xmalloc((size_t)(n ? n : 1) * sizeof *duration);
    float *chosen_end = xmalloc((size_t)(n ? n : 1) * sizeof *chosen_end);
    for (int v = 0; v < n; v++) {
        int count = pair_start[v + 1] - pair_start[v];
        duration[v] = 0;
        chosen_end[v] = INFINITY;
        if (lead[v] < 0 || !count) {
            if (readable[v] && lead[v] >= 0)
                snprintf(a->info[v].reason, sizeof a->info[v].reason,
                         "Kein wiederkehrendes Intro gefunden");
            continue;
        }
        auto_judge_video(&kb[v], ends + pair_start[v], starts + pair_start[v], count,
                         component_size[auto_root(parent, v)], max_len, min_videos, &a->info[v],
                         &duration[v], &chosen_end[v]);
    }
    free(ends);
    free(starts);

    /* Gruppen: Videos mit demselben Intro (nur Videos mit Schnitt) */
    int *group_root = auto_group_roots(&pairs, a->info, chosen_end, duration, kb, n);
    free(chosen_end);
    int *group_of_root = xmalloc((size_t)(n ? n : 1) * sizeof *group_of_root);
    for (int i = 0; i < n; i++) group_of_root[i] = -1;
    int ngroups = 0, nmembers = 0;
    for (int v = 0; v < n; v++) {
        if (a->info[v].cut_len <= 0) continue;
        int root = group_root[v];
        if (group_of_root[root] < 0) group_of_root[root] = ngroups++;
        a->info[v].group = group_of_root[root];
        nmembers++;
    }
    free(group_root);
    AutoMember *by_group = xmalloc((size_t)(nmembers ? nmembers : 1) * sizeof *by_group);
    nmembers = 0;
    for (int v = 0; v < n; v++)
        if (a->info[v].group >= 0)
            by_group[nmembers++] = (AutoMember){a->info[v].group, duration[v], v};
    /* länger gemeinsam als die meisten der Gruppe (z. B. gleicher Rückblick in einigen Folgen
     * oder Duplikate): unsicher – lieber einen Rest Intro stehen lassen als Inhalt entfernen */
    qsort(by_group, (size_t)nmembers, sizeof *by_group, compare_members);
    for (int i = 0; i < nmembers;) {
        int j = i;
        while (j < nmembers && by_group[j].group == by_group[i].group) j++;
        double typical = by_group[i + (j - i - 1) / 2].value;
        for (int m = i; m < j; m++) {
            int v = by_group[m].video;
            if (a->info[v].sure && by_group[m].value > typical + 1.5 / kb_fps(&kb[v])) {
                a->info[v].sure = false;
                snprintf(a->info[v].reason, sizeof a->info[v].reason,
                         "Gemeinsamer Anfang länger als bei den meisten Videos dieser Gruppe");
            }
        }
        i = j;
    }
    /* je Gruppe: Mitglieder, sichere, Länge (Median der Schnitte) */
    IcAutoGroup *groups = xmalloc((size_t)(ngroups ? ngroups : 1) * sizeof *groups);
    memset(groups, 0, (size_t)(ngroups ? ngroups : 1) * sizeof *groups);
    for (int g = 0; g < ngroups; g++) groups[g].representative = -1;
    for (int m = 0; m < nmembers; m++) {
        int v = by_group[m].video;
        IcAutoGroup *group = &groups[a->info[v].group];
        group->members++;
        group->sure += a->info[v].sure;
        by_group[m].value = a->info[v].cut_len;
    }
    qsort(by_group, (size_t)nmembers, sizeof *by_group, compare_members);
    for (int i = 0; i < nmembers;) {
        int j = i;
        while (j < nmembers && by_group[j].group == by_group[i].group) j++;
        groups[by_group[i].group].length = by_group[i + (j - i - 1) / 2].value;
        i = j;
    }
    /* typisches Video: sicher, Schnitt nahe der typischen Länge, mit vielen verglichen */
    for (int m = 0; m < nmembers; m++) {
        int v = by_group[m].video;
        IcAutoGroup *group = &groups[a->info[v].group];
        int best = group->representative;
        double off = fabs(a->info[v].cut_len - group->length);
        double best_off = best < 0 ? 0 : fabs(a->info[best].cut_len - group->length);
        if (best < 0 || (a->info[v].sure && !a->info[best].sure) ||
            (a->info[v].sure == a->info[best].sure &&
             (off < best_off - 1e-3 ||
              (off < best_off + 1e-3 && partner_count[v] > partner_count[best]))))
            group->representative = v;
    }
    free(by_group);
    free(duration);
    for (int v = 0; v < n; v++) {               /* zu wenige sichere: ganze Gruppe unsicher */
        int g = a->info[v].group;
        if (g < 0 || groups[g].sure >= min_videos || !a->info[v].sure) continue;
        a->info[v].sure = false;
        snprintf(a->info[v].reason, sizeof a->info[v].reason,
                 "Nur %d Videos sicher erkannt (mindestens %d nötig)", groups[g].sure,
                 min_videos);
    }
    for (int g = 0; g < ngroups; g++)
        if (groups[g].sure < min_videos) groups[g].sure = 0;
    /* nach Größe sortieren, Gruppennummern der Videos nachziehen */
    size_t group_slots = ngroups > 0 ? (size_t)ngroups : 1;
    AutoGroupOrder *order = xmalloc(group_slots * sizeof *order);
    for (int g = 0; g < ngroups; g++) order[g] = (AutoGroupOrder){g, groups[g].members};
    qsort(order, (size_t)ngroups, sizeof *order, compare_group_order);
    int *new_index = xmalloc(group_slots * sizeof *new_index);
    a->groups = xmalloc(group_slots * sizeof *a->groups);
    for (int g = 0; g < ngroups; g++) {
        new_index[order[g].index] = g;
        a->groups[g] = groups[order[g].index];
    }
    for (int v = 0; v < n; v++)
        if (a->info[v].group >= 0) a->info[v].group = new_index[a->info[v].group];
    a->ngroups = ngroups;
    free(order);
    free(new_index);
    free(groups);
    free(group_of_root);
    free(pair_start);
    free(partner_count);
    free(component_size);
    free(pairs.v);
    free(compared.slots);
    free(lead);
    free(parent);
    for (int i = 0; i < n; i++) kb_free(&kb[i]);
    free(kb);
    free(readable);
    a->secs = monotonic_secs() - started_at;
    if (ic_cancel) return 1;

    /* Liste fürs Nachsehen: ~/.intro-cutter/last_auto_matches.txt */
    FILE *log = fopen(AUTO_LOG_FILE, "we");
    if (log) {
        for (int v = 0; v < n; v++)
            fprintf(log, "%d\t%s\t%.3f\t%s\t%s\n", a->info[v].group,
                    a->info[v].sure ? "sicher" : a->info[v].cut_len > 0 ? "unsicher" : "-",
                    a->info[v].cut_len, a->videos.v[v].path, a->info[v].reason);
        fclose(log);
    }
    return 0;
}

/* wiederkehrende Intros automatisch finden (siehe ic_core.h) */
int ic_detect_auto(const char *src, const char *out, double max_len, int min_videos, IcAuto *a,
                   const char **err)
{
    standby_lock_begin("Intro-Cutter sucht Intros");
    int rc = detect_auto(src, out, max_len, min_videos < 2 ? 2 : min_videos, a, err);
    standby_lock_end();
    return rc;
}

/* Ergebnis der automatischen Suche freigeben */
void ic_auto_free(IcAuto *a)
{
    free(a->info);
    free(a->groups);
    videos_free(&a->videos);
    memset(a, 0, sizeof *a);
}
