/*
 * Mediathek Downloader – Engine
 * =============================
 * Sucht Filme und Serien in den Mediatheken von ZDF, ARD und 3sat, lädt sie herunter
 * und wandelt sie auf Wunsch nach AV1 um. Bedient wird die Engine über md_core.h – von
 * der Terminal-Version (md_cli.c) und der grafischen Oberfläche (md_gui.c).
 *
 * Quellen und Werkzeuge:
 *   * Suche: API von MediathekViewWeb (Metadaten und direkte MP4-Adressen).
 *   * Download: das Programm curl. "-C -" setzt .part-Dateien fort, "--limit-rate"
 *     begrenzt das Tempo. Pausieren beendet curl – die .part-Datei bleibt, deshalb
 *     überlebt eine Pause auch das Beenden des Programms und einen Neustart des PCs.
 *     Ändert sich das Tempolimit, wird curl mit dem neuen Wert neu gestartet.
 *   * AV1: ffmpeg mit denselben Werten wie der AV1-Converter – GPU per VAAPI
 *     (Qualität 64, 10 Bit; unter Windows NVENC, QSV oder AMF), CPU per SVT-AV1 (RF 35).
 *     Der Ton wird unverändert übernommen. Eine pausierte Umwandlung wird angehalten
 *     (SIGSTOP, unter Windows NtSuspendProcess).
 *   * Sprachen: Das ZDF liefert jede Sprache als eigene MP4-Datei (…a1a2… Deutsch,
 *     …a3a4… Englisch, …a5a6… Audiodeskription). Die Suche fasst diese Fassungen zu
 *     einem Eintrag zusammen. Das Bild kommt aus der Hauptdatei, jede weitere Sprache
 *     aus der kleinsten Datei – der Ton ist in allen Qualitäten gleich (AAC 128 kbit/s).
 *     ffmpeg legt alles ohne Neukodierung in eine MKV-Datei; Untertitel kommen als
 *     WebVTT und landen als SRT-Spur darin.
 *
 * Sicherheit:
 *   * Fertige Downloads heißen erst nach der Größenprüfung <Name>.mp4.
 *   * AV1-Ergebnisse entstehen in einer versteckten Temp-Datei und werden geprüft
 *     (AV1-Spur, Dauer). Ist das Ergebnis nicht kleiner, bleibt die MP4-Datei.
 *   * Die Warteschlange steht in ~/.mediathek-downloader/warteschlange.tsv. Nach dem
 *     Start sind offene Aufträge pausiert und warten auf "Fortsetzen".
 *
 * Linux und Windows: Was sich unterscheidet (Prozesse, Pipes, freier Platz), steht im
 * Abschnitt Prozesse hinter G_OS_WIN32; alles andere ist für beide gleich.
 *
 * Aufbau dieser Datei: Konfiguration – kleine Helfer – Protokoll – Prozesse –
 * JSON-Leser – Suche – Pfade – Warteschlange (Speichern, Laden) – Download –
 * Umwandlung – Verteiler – öffentliche Schnittstelle
 *
 * Namenskonventionen: md_* = öffentliche Schnittstelle (md_core.h), q.* = globaler
 * Zustand der Warteschlange (geschützt durch q.lock), GROSSBUCHSTABEN = Konstanten.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef G_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <sys/utime.h>
#include <wchar.h>
#include <windows.h>
#define localtime_r(t, tm) localtime_s(tm, t)
#else
#include <poll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utime.h>
#endif

#include "md_core.h"

/* ---------------------------------------------------------------------------
 * Konfiguration
 * ------------------------------------------------------------------------- */
#define API_URL "https://mediathekviewweb.de/api/query"
#define USER_AGENT "mediathek-downloader/" MD_VERSION
/* curl nur für Webadressen, auch bei Weiterleitungen. Adressen aus fremden Antworten stehen hinter
 * --url, damit keine als Option gilt. */
#define CURL_CMD "curl", "--proto", "=http,https", "--proto-redir", "=http,https"
#define COVER_PAGE_PREFIX "https://www.zdf.de/"
#define COVER_URL_PREFIX "https://www.zdf.de/assets/"
#define COVER_EPG_PREFIX "https://epg-image.zdf.de/"
#define PAGE_RECHECK_SECONDS (7 * 24 * 3600)    /* Seite ohne Bild/Kategorie neu prüfen */
#define NO_COVER_RETRY_SECONDS (24 * 3600)      /* Titelbild ohne Antwort der Seite neu suchen */
#define COVER_SIZE "384x216"
#define COVER_WIDTH_TEXT "384"
#define COVER_ARTE_PREFIX "https://api-cdn.arte.tv/img/v2/image/"
#define COVER_ARD_PAGE_PREFIX "https://www.ardmediathek.de/img?"
#define COVER_SRF_PREFIX "https://il.srgssr.ch/images/"
#define COVER_KIKA_PREFIX "https://www.kika.de/"
/* Seiten mit og:image, aber ohne Kategorien */
#define COVER_OTHER_PAGES "https://www.arte.tv/", "https://www.srf.ch/play/", \
                          COVER_KIKA_PREFIX
#define COVER_3SAT_PAGE "https://www.3sat.de/"
#define COVER_3SAT_PREFIX "https://www.3sat.de/assets/"
#define COVER_ARD_PAGE "https://www.ardmediathek.de/"
#define COVER_ARD_PREFIX "https://api.ardmediathek.de/image-service/"
#define ARD_ITEM_API "https://api.ardmediathek.de/page-gateway/pages/ard/item/"
#define GENRE_TEXT_LENGTH 120   /* so viel der ARD-Beschreibung zählt für die Kategorie */
#define IMDB_API "https://v3.sg.media-imdb.com/suggestion/x/"
#define IMDB_RATINGS_URL "https://datasets.imdbws.com/title.ratings.tsv.gz"
#define IMDB_GENRES_URL "https://datasets.imdbws.com/title.basics.tsv.gz"
#define IMDB_GENRES_SECONDS (30 * 24 * 3600)
#define IMDB_GENRES_MIN_SIZE (1024 * 1024)
#define IMDB_RATINGS_SECONDS (7 * 24 * 3600)
#define PAGE_INFO_UNKNOWN "\n\n"                 /* nichts gefunden, gilt nie als erledigt */
#define COVER_FRAME_SECONDS "180"                /* hinter dem Vorspann */
#define COVER_FRAME_WIDTH "384"
#define CURL_HTTP_ERROR 22                       /* Exit-Code von curl -f bei 4xx/5xx */
#define SEARCH_CACHE_SECONDS (6 * 3600)         /* so lange gilt die Liste der Vorschau */
#define PAGE_SIZE 500               /* Einträge je Anfrage */
#define SEARCH_MAX 3000             /* mehr Treffer lädt die Suche nicht */
#define BROWSE_MIN_SECONDS 1200     /* Stöbern ohne Suchbegriff: Mindestlänge für Serien */
#define POLL_MS 200                 /* so oft prüfen die Worker ihren Kindprozess */
#define RATE_TOLERANCE 0.05         /* kleinere Änderungen starten curl nicht neu */
#define MAX_FAILURES 5              /* Verbindungsfehler in Folge ohne Fortschritt */
#define RETRY_WAIT_MS 5000
#define HLS_AUDIO_BITRATE 128000    /* Bit/s je Tonspur, für das Tempolimit von Streams */
/* übliche Datenrate je Qualität (Video und Ton), für Schätzungen */
static const double QUALITY_KBIT_PER_SECOND[] = {6660, 3360, 808};
#define MIN_FREE_SPACE (512 * 1024 * 1024LL)    /* Reserve auf dem Ziellaufwerk */
#define NAME_MAX_BYTES 180
#define FILE_MAX_BYTES 230          /* Reserve zu 255 für .part und die versteckten Temp-Namen */
#define ENCODER_NICE 5
#define SVT_PRESET "6"
#define GOP_FRAMES "500"            /* Keyframe alle 10 s bei 50 Bildern/s */
#define CROP_SAMPLES 5              /* Stellen im Video für die Suche nach Balken */
#define CROP_FRAMES "25"            /* Bilder je Stelle */
#define CROP_MIN_PIXELS 8           /* schmalere Ränder bleiben */
#define QUEUE_HEADER "#mediathek-downloader-warteschlange 1"
#define QUEUE_HEADER_OLD "#zdf-downloader-warteschlange 1"
#define OLD_APP_DIR ".zdf-downloader"
#define LEGACY_APP_DIR ".mediathek-downloader"  /* bis 2.2: immer im Benutzerordner */
#define DATA_DIR_NAME "Mediathek Downloader"    /* Unterordner im gewählten Ordner */
#define CACHE_DIR_NAME "Zwischenspeicher"
#define OLD_CACHE_DIR_NAME "bilder"             /* bis 2.2 */
#define POINTER_DIR "mediathek-downloader"      /* in ~/.config bzw. %LOCALAPPDATA% */
#define POINTER_FILE "datenordner"              /* gewählter Ordner, fehlt = Standard */
#define HISTORY_FILE "verlauf.tsv"
/* Fehlertexte, die mehrfach vorkommen; sie sagen auch, was zu tun ist */
#define UNREACHABLE_TEXT "Der Server ist nicht erreichbar. Internetverbindung prüfen und erneut " \
                         "versuchen."
#define FFMPEG_MISSING_TEXT "Das Programm ist unvollständig installiert (ffmpeg fehlt). Bitte " \
                            "neu installieren."
#define AUDIO_BYTES_PER_SECOND 16000    /* Zusatzspur: AAC 128 kbit/s */
#define VARIANT_MAX_DIFFERENCE 30       /* Sekunden: so nah muss die Länge der Fassungen sein */

/* Sprachfassungen, die das ZDF am Titelende in Klammern nennt */
static const struct {
    const char *tag, *label, *lang;
} VARIANTS[] = {
    {"(Audiodeskription)", "Audiodeskription", "deu"}, {"(Hörfassung)", "Audiodeskription", "deu"},
    {"(AD)", "Audiodeskription", "deu"},
    {"(Englisch)", "Englisch", "eng"}, {"(Französisch)", "Französisch", "fra"},
    {"(Spanisch)", "Spanisch", "spa"}, {"(Italienisch)", "Italienisch", "ita"},
    {"(Dänisch)", "Dänisch", "dan"}, {"(Schwedisch)", "Schwedisch", "swe"},
    {"(Norwegisch)", "Norwegisch", "nor"}, {"(Originalfassung)", "Originalfassung", "und"},
    {"(Originalversion)", "Originalfassung", "und"},
    {"(Originalversion mit Untertitel)", "Originalfassung", "und"},
};

/* Mediatheken: Suchwort für den Sender und Name in den Daten (ZDF: auch "ZDF-tivi" …) */
static const struct {
    int source;
    const char *query, *channel, *label;
} SOURCES[] = {
    {MD_SOURCE_ZDF, "zdf", "ZDF", "ZDF"},
    {MD_SOURCE_ARD, "ard", "ARD", "ARD"},
    {MD_SOURCE_3SAT, "3sat", "3Sat", "3sat"},
    {MD_SOURCE_ZDFNEO, "zdfneo", "ZDFneo", "ZDFneo"},
    {MD_SOURCE_ONE, "one", "ONE", "ONE"},
    {MD_SOURCE_WDR, "wdr", "WDR", "WDR"},
    {MD_SOURCE_SWR, "swr", "SWR", "SWR"},
    {MD_SOURCE_NDR, "ndr", "NDR", "NDR"},
    {MD_SOURCE_RBB, "rbb", "RBB", "rbb"},
    {MD_SOURCE_NETZKINO, NULL, "Netzkino", "Netzkino"},
    {MD_SOURCE_ARTE, "arte.de", "ARTE.DE", "ARTE"},
    {MD_SOURCE_MDR, "mdr", "MDR", "MDR"},
    {MD_SOURCE_BR, "br", "BR", "BR"},
    {MD_SOURCE_SRF, "srf", "SRF", "SRF"},
    {MD_SOURCE_KIKA, "kika", "KiKA", "KiKA"},
    {MD_SOURCE_ZDFTIVI, "zdf-tivi", "ZDF-tivi", "ZDFtivi"},
    {MD_SOURCE_ZDFINFO, "zdfinfo", "ZDFinfo", "ZDFinfo"},
    {MD_SOURCE_PHOENIX, "phoenix", "PHOENIX", "phoenix"},
    {MD_SOURCE_HR, "hr", "HR", "hr"},
    {MD_SOURCE_SR, "sr", "SR", "SR"},           /* findet auch SRF, Doppelte fallen weg */
    {MD_SOURCE_ARDALPHA, "ard-alpha", "ARD-alpha", "ARD alpha"},
    {MD_SOURCE_TAGESSCHAU24, "tagesschau24", "tagesschau24", "tagesschau24"},
    {MD_SOURCE_FUNK, "funk.net", "Funk.net", "funk"},
    {MD_SOURCE_ARTE_INTL, "arte.en", "ARTE.EN", "ARTE intl."},
    {MD_SOURCE_ARTE_INTL, "arte.fr", "ARTE.FR", "ARTE intl."},
    {MD_SOURCE_ARTE_INTL, "arte.es", "ARTE.ES", "ARTE intl."},
    {MD_SOURCE_ARTE_INTL, "arte.it", "ARTE.IT", "ARTE intl."},
    {MD_SOURCE_ARTE_INTL, "arte.pl", "ARTE.PL", "ARTE intl."},
    {MD_SOURCE_NRK, NULL, "NRK", "NRK"},
};

/* Netzkino steht nicht in der Suche: Filmlisten der eigenen Schnittstelle, ohne NetzkinoPlus */
#define NETZKINO_API "https://api.netzkino.de.simplecache.net/capi-2.0a/categories/"
#define NETZKINO_VIDEO "https://pmd.netzkino-seite.netzkino.de/"
#define NETZKINO_PAGE "https://www.netzkino.de/details/"
/* NRK: nur weltweit freigegebene Folgen; der Stream wird erst beim Laden erfragt */
#define NRK_API "https://psapi.nrk.no"
#define NRK_CATALOG NRK_API "/tv/catalog/series/"
#define NRK_MANIFEST NRK_API "/playback/manifest/program/"
#define NRK_PAGE "https://tv.nrk.no/serie/"
#define NRK_STREAM_PREFIX "https://nrk-od-world-"  /* weltweites Netz; Norwegen-Server: nein */
#define NRK_THREADS 6
#define NRK_SEARCH_SERIES 6         /* so viele Serien je Suchwort */
static const int NETZKINO_CATEGORIES[] = {1, 3, 4, 5, 6, 10, 18, 32, 35, 51, 71, 10333};

/* Mediatheken, deren Titel gleich aufgebaut sind */
#define ZDF_STYLE (MD_SOURCE_ZDF | MD_SOURCE_ZDFNEO | MD_SOURCE_ZDFTIVI | MD_SOURCE_ZDFINFO)
#define ARD_STYLE (MD_SOURCE_ARD | MD_SOURCE_ONE)
/* Folgen als "Serie (3/6)"; ARTE und der BR nennen dazu nur eine Rubrik als Thema */
#define PART_STYLE (MD_SOURCE_ARTE | MD_SOURCE_ARTE_INTL | MD_SOURCE_BR | MD_SOURCE_SRF)
/* Mediatheken, die (teils) nur HLS-Streams anbieten; die übrigen haben Dateien */
#define HLS_SOURCES (MD_SOURCE_ARTE | MD_SOURCE_ARTE_INTL | MD_SOURCE_SRF | MD_SOURCE_NRK)

/* Rubriken, deren Einträge Filme sind (Stöbern ohne Suchbegriff) */
static const char *const FILM_TOPICS[] = {"Filme", "Spielfilme", "Spielfilm-Highlights",
                                          "Der Fernsehfilm der Woche", "Das kleine Fernsehspiel",
                                          "Filme in der ARD", "Spielfilm"};

/* zusätzliche Datei eines Auftrags: weitere Tonspur oder Untertitel */
typedef struct {
    bool subtitle;
    char *label, *lang, *url;
} Extra;

typedef struct {
    guint id;
    char *title, *url, *target;     /* target = <Name>.mp4 */
    char *main_label, *main_lang;   /* Tonspur der Hauptdatei */
    bool skip_main_audio;           /* Ton der Hauptdatei abgewählt */
    bool merging;                   /* Worker führt nur Spuren zusammen, kein AV1 */
    char *key, *topic;              /* für den Verlauf */
    int season, episode;
    GPtrArray *extras;              /* Extra* */
    bool convert, keep_original;
    MdEncoder encoder;
    int gpu_quality, cpu_rf;
    MdQuality quality;              /* für HLS: welche Stufe der Wiedergabeliste */

    MdJobState state;
    bool paused, remove_requested;
    bool worker;                    /* ein Thread arbeitet an diesem Auftrag */
    char *message;
    gint64 have, total;
    double speed, progress, remaining;
} Job;

static struct {
    GMutex lock;
    GCond wake;                     /* Verteiler aufwecken */
    GCond worker_done;
    GPtrArray *jobs;
    guint next_id;
    gint64 rate_limit;              /* Bytes/s für alle Downloads zusammen, 0 = unbegrenzt */
    int max_downloads, max_conversions;
    int workers;
    bool persist, quitting;
    bool save_due;                  /* Warteschlange geändert, der Verteiler schreibt sie */
    bool downloads_held, conversions_held;  /* alle Downloads bzw. Umwandlungen angehalten */
    GHashTable *open_keys;          /* Schlüssel offener Aufträge, NULL = neu bestimmen */
    GThread *dispatcher;
    GHashTable *history;            /* Schlüssel -> History* */
} q;

static char *g_app_dir, *g_log_path;
static GMutex g_log_lock;

/* ---------------------------------------------------------------------------
 * Kleine Helfer
 * ------------------------------------------------------------------------- */
/* Beim Umzug ändern sich die Pfade; die alten Zeichenketten bleiben gültig, andere Threads
 * können sie gerade benutzen */
const char *md_app_dir(void)
{
    return g_atomic_pointer_get(&g_app_dir);
}

const char *md_log_path(void)
{
    return g_atomic_pointer_get(&g_log_path);
}

/* Dezimalpunkt durch Komma ersetzen (Anzeige auf Deutsch) */
static char *with_decimal_comma(char *text)
{
    for (char *p = text; *p; p++)
        if (*p == '.') *p = ',';
    return text;
}

char *md_format_size(double bytes)
{
    if (bytes >= 1e9) return with_decimal_comma(g_strdup_printf("%.1f GB", bytes / 1e9));
    if (bytes >= 1e6) return g_strdup_printf("%.0f MB", bytes / 1e6);
    return g_strdup_printf("%.0f kB", bytes / 1e3);
}

char *md_format_time(double seconds)
{
    int s = (int)(seconds + 0.5);
    if (s >= 3600) return g_strdup_printf("%d h %02d min", s / 3600, s / 60 % 60);
    if (s >= 60) return g_strdup_printf("%d min", s / 60);
    return g_strdup_printf("%d s", s);
}

/* Größe einer Datei, -1 wenn es sie nicht gibt */
static gint64 file_size(const char *path)
{
    GStatBuf st;
    return g_stat(path, &st) == 0 ? (gint64)st.st_size : -1;
}

/* Sekunden seit einem festen Zeitpunkt */
static double now_seconds(void)
{
    return g_get_monotonic_time() / 1e6;
}

/* versteckte Datei neben dem Ziel: .<Name><suffix> */
static char *hidden_path(const char *target, const char *suffix)
{
    char *dir = g_path_get_dirname(target), *base = g_path_get_basename(target);
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    char *name = g_strdup_printf(".%s%s", base, suffix);
    char *path = g_build_filename(dir, name, NULL);
    g_free(dir);
    g_free(base);
    g_free(name);
    return path;
}

/* Temp-Datei der Umwandlung */
static char *temp_path(const char *target)
{
    return hidden_path(target, ".av1-tmp.mkv");
}

/* Datei der Zusatzspur Nummer index */
static char *extra_path(const char *target, guint index, bool subtitle)
{
    char *suffix = g_strdup_printf(".spur%u.%s", index, subtitle ? "vtt" : "mp4");
    char *path = hidden_path(target, suffix);
    g_free(suffix);
    return path;
}

/* <Name>.mp4 -> <Name>.mkv */
static char *result_path(const char *target)
{
    char *path = g_strdup(target);
    char *dot = strrchr(path, '.');
    if (dot) *dot = 0;
    char *result = g_strconcat(path, ".mkv", NULL);
    g_free(path);
    return result;
}

/* ---------------------------------------------------------------------------
 * Protokoll
 * ------------------------------------------------------------------------- */
void md_log(const char *format, ...)
{
    if (!g_log_path) return;
    va_list args;
    va_start(args, format);
    char *text = g_strdup_vprintf(format, args);
    va_end(args);
    char stamp[32];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
    g_mutex_lock(&g_log_lock);
    FILE *f = g_fopen(md_log_path(), "a");
    if (f) {
        fprintf(f, "%s  %s\n", stamp, text);
        fclose(f);
    }
    g_mutex_unlock(&g_log_lock);
    g_free(text);
}

/* ---------------------------------------------------------------------------
 * Prozesse
 * Kindprozesse enden mit dem Programm, damit nach einem Absturz kein curl oder ffmpeg
 * weiterläuft: unter Linux per PR_SET_PDEATHSIG, unter Windows über ein Job-Objekt.
 * ------------------------------------------------------------------------- */
#ifdef G_OS_WIN32
/* Windows ruft child_setup nicht im Kind auf; Job und Priorität setzt adopt_child */
#define child_setup NULL

/* Downloads und Umwandlungen kommen in dieses Job-Objekt: Endet das Programm, schließt Windows
 * es und beendet alles darin. Was der Nutzer öffnet (Player, Ordner), bleibt draußen. */
static HANDLE g_child_job;

/* Job anlegen; die eigenen Werkzeuge (curl, ffmpeg) liegen neben dem Programm */
static void platform_init(void)
{
    g_child_job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (g_child_job && !SetInformationJobObject(g_child_job, JobObjectExtendedLimitInformation,
                                                &limits, sizeof limits)) {
        CloseHandle(g_child_job);
        g_child_job = NULL;
    }
    char *exe_dir = g_win32_get_package_installation_directory_of_module(NULL);
    if (exe_dir) {
        char *bin = g_build_filename(exe_dir, "bin", NULL);
        const char *old = g_getenv("PATH");
        char *path = g_strjoin(";", bin, exe_dir, old ? old : "", NULL);
        g_setenv("PATH", path, TRUE);
        g_free(path);
        g_free(bin);
        g_free(exe_dir);
    }
}

/* Gestarteten Prozess an das Programm binden; encoder = mit niedriger Priorität */
static void adopt_child(GPid pid, bool encoder)
{
    if (g_child_job) AssignProcessToJobObject(g_child_job, pid);
    if (encoder) SetPriorityClass(pid, BELOW_NORMAL_PRIORITY_CLASS);
}

static int exit_code(int status)
{
    return status;
}

/* Ist der Prozess beendet? -> true und *code = Exit-Code; danach ist pid ungültig */
static bool child_ended(GPid pid, int *code)
{
    if (WaitForSingleObject(pid, 0) != WAIT_OBJECT_0) return false;
    DWORD value = 128;
    GetExitCodeProcess(pid, &value);
    if (code) *code = (int)value;
    g_spawn_close_pid(pid);
    return true;
}

/* Prozess beenden und auf ihn warten */
static void stop_child(GPid pid)
{
    TerminateProcess(pid, 1);
    WaitForSingleObject(pid, 5000);
    g_spawn_close_pid(pid);
}

/* Prozess anhalten oder fortsetzen; Windows kennt kein SIGSTOP */
static void suspend_child(GPid pid, bool pause)
{
    typedef LONG(NTAPI * SuspendFn)(HANDLE);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    SuspendFn fn = ntdll ? (SuspendFn)(void (*)(void))GetProcAddress(
                               ntdll, pause ? "NtSuspendProcess" : "NtResumeProcess")
                         : NULL;
    if (fn) fn(pid);
}

/* Freier Platz im Ordner -> Bytes, -1 = unbekannt */
static gint64 free_space(const char *dir)
{
    wchar_t *wide = g_utf8_to_utf16(dir, -1, NULL, NULL, NULL);
    ULARGE_INTEGER available;
    bool ok = wide && GetDiskFreeSpaceExW(wide, &available, NULL, NULL);
    g_free(wide);
    return ok ? (gint64)available.QuadPart : -1;
}

/* Auf Ausgabe warten; Pipes kennen unter Windows kein poll */
static void wait_pipes(int out_fd, int err_fd)
{
    (void)out_fd;
    (void)err_fd;
    g_usleep(POLL_MS * 1000);
}

/* Alles lesen, was gerade in der Pipe steht, ohne zu blockieren */
static void drain_pipe(int fd, GString *buffer)
{
    HANDLE pipe = (HANDLE)_get_osfhandle(fd);
    char chunk[4096];
    DWORD available = 0;
    while (PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL) && available > 0) {
        int n = _read(fd, chunk, MIN(available, (DWORD)sizeof chunk));
        if (n <= 0) break;
        g_string_append_len(buffer, chunk, n);
    }
}

static void prepare_pipe(int fd)
{
    (void)fd;
}
#else
/* im Kindprozess vor exec: data != NULL = Encoder, Priorität senken */
static void child_setup(gpointer data)
{
    signal(SIGPIPE, SIG_DFL);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (data) {
        setpriority(PRIO_PROCESS, 0, ENCODER_NICE);
        syscall(SYS_ioprio_set, 1 /* IOPRIO_WHO_PROCESS */, 0, (2 << 13) | 7);
    }
}

static void platform_init(void)
{
}

/* unter Linux schon in child_setup erledigt */
static void adopt_child(GPid pid, bool encoder)
{
    (void)pid;
    (void)encoder;
}

static int exit_code(int status)
{
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

/* Ist der Prozess beendet? -> true und *code = Exit-Code */
static bool child_ended(GPid pid, int *code)
{
    int status = 0;
    if (waitpid(pid, &status, WNOHANG) != pid) return false;
    if (code) *code = exit_code(status);
    return true;
}

/* Prozess beenden (auch wenn er angehalten ist) und auf ihn warten */
static void stop_child(GPid pid)
{
    kill(pid, SIGCONT);
    kill(pid, SIGTERM);
    for (int i = 0; i < 100; i++) {
        if (waitpid(pid, NULL, WNOHANG) == pid) return;
        g_usleep(50 * 1000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static void suspend_child(GPid pid, bool pause)
{
    kill(pid, pause ? SIGSTOP : SIGCONT);
}

/* Freier Platz im Ordner -> Bytes, -1 = unbekannt */
static gint64 free_space(const char *dir)
{
    struct statvfs st;
    return statvfs(dir, &st) == 0 ? (gint64)st.f_bavail * (gint64)st.f_frsize : -1;
}

/* Auf Ausgabe von ffmpeg warten, höchstens POLL_MS */
static void wait_pipes(int out_fd, int err_fd)
{
    struct pollfd fds[2] = {{out_fd, POLLIN, 0}, {err_fd, POLLIN, 0}};
    poll(fds, 2, POLL_MS);
}

/* Alles lesen, was gerade in der Pipe steht */
static void drain_pipe(int fd, GString *buffer)
{
    char chunk[4096];
    ssize_t n;
    while ((n = read(fd, chunk, sizeof chunk)) > 0) g_string_append_len(buffer, chunk, n);
}

/* Pipe nicht blockierend lesen */
static void prepare_pipe(int fd)
{
    fcntl(fd, F_SETFL, O_NONBLOCK);
}
#endif

/* Kurzes Werkzeug starten und die Ausgabe einsammeln -> Exit-Code, -1 = nicht startbar */
static int run_capture(char **argv, char **out, char **err)
{
    int status = 0;
    /* child_setup: endet das Programm, endet auch das Werkzeug */
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, child_setup, NULL, out, err, &status,
                      NULL))
        return -1;
    return exit_code(status);
}

/* ---------------------------------------------------------------------------
 * JSON-Leser
 * Die Antworten der API sind einfach aufgebaut; gelesen wird direkt beim Durchlaufen.
 * Fehlerhafte Eingaben setzen ok = false, die Tiefe ist begrenzt.
 * ------------------------------------------------------------------------- */
#define JSON_MAX_DEPTH 64

typedef struct {
    const char *p;
    bool ok;
} Json;

static void json_spaces(Json *j)
{
    while (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t') j->p++;
}

static bool json_accept(Json *j, char c)
{
    json_spaces(j);
    if (*j->p != c) return false;
    j->p++;
    return true;
}

/* vier Hex-Ziffern von \uXXXX -> Wert, -1 bei Fehler */
static int json_hex4(const char *p)
{
    int value = 0;
    for (int i = 0; i < 4; i++) {
        int digit = g_ascii_xdigit_value(p[i]);
        if (digit < 0) return -1;
        value = value * 16 + digit;
    }
    return value;
}

/* Zeichenkette lesen -> neu angelegter Text in gültigem UTF-8 */
static char *json_string(Json *j)
{
    if (!json_accept(j, '"')) {
        j->ok = false;
        return g_strdup("");
    }
    GString *s = g_string_new(NULL);
    while (*j->p && *j->p != '"') {
        if (*j->p != '\\') {
            g_string_append_c(s, *j->p++);
            continue;
        }
        j->p++;
        char c = *j->p;
        if (!c) break;
        j->p++;
        switch (c) {
        case 'n': g_string_append_c(s, '\n'); break;
        case 't': g_string_append_c(s, '\t'); break;
        case 'r': g_string_append_c(s, '\r'); break;
        case 'b': g_string_append_c(s, '\b'); break;
        case 'f': g_string_append_c(s, '\f'); break;
        case 'u': {
            int code = json_hex4(j->p);
            if (code < 0) {
                j->ok = false;
                break;
            }
            j->p += 4;
            if (code >= 0xD800 && code < 0xDC00 && j->p[0] == '\\' && j->p[1] == 'u') {
                int low = json_hex4(j->p + 2);
                if (low >= 0xDC00 && low < 0xE000) {
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    j->p += 6;
                }
            }
            if (code >= 0xD800 && code < 0xE000) code = 0xFFFD;
            g_string_append_unichar(s, code);
            break;
        }
        default: g_string_append_c(s, c); break;
        }
    }
    if (!json_accept(j, '"')) j->ok = false;
    char *valid = g_utf8_make_valid(s->str, s->len);
    g_string_free(s, TRUE);
    return valid;
}

/* Zahl, true, false oder null überspringen -> Zahlenwert (sonst 0) */
static double json_scalar(Json *j)
{
    json_spaces(j);
    char *end = NULL;
    double value = g_ascii_strtod(j->p, &end);
    if (end != j->p) {
        j->p = end;
        return value;
    }
    for (const char *const *w = (const char *const[]){"true", "false", "null", NULL}; *w; w++) {
        size_t n = strlen(*w);
        if (!strncmp(j->p, *w, n)) {
            j->p += n;
            return 0;
        }
    }
    j->ok = false;
    return 0;
}

/* beliebigen Wert überspringen */
static void json_skip(Json *j, int depth)
{
    json_spaces(j);
    if (depth > JSON_MAX_DEPTH) j->ok = false;
    if (!j->ok) return;
    if (*j->p == '"') {
        g_free(json_string(j));
    } else if (*j->p == '{' || *j->p == '[') {
        bool object = *j->p == '{';
        char close = object ? '}' : ']';
        j->p++;
        if (json_accept(j, close)) return;
        do {
            if (object) {
                g_free(json_string(j));
                if (!json_accept(j, ':')) j->ok = false;
            }
            json_skip(j, depth + 1);
        } while (j->ok && json_accept(j, ','));
        if (!json_accept(j, close)) j->ok = false;
    } else {
        json_scalar(j);
    }
}

/* ---------------------------------------------------------------------------
 * Suche
 * ------------------------------------------------------------------------- */
void md_title_free(gpointer data)
{
    MdTitle *t = data;
    if (!t) return;
    g_free(t->channel);
    g_free(t->topic);
    g_free(t->title);
    g_free(t->description);
    g_free(t->website);
    g_free(t->url_high);
    g_free(t->url_medium);
    g_free(t->url_low);
    g_free(t->url_subtitle);
    g_free(t->name);
    if (t->audio) g_ptr_array_free(t->audio, TRUE);
    if (t->subtitles) g_ptr_array_free(t->subtitles, TRUE);
    g_free(t);
}

static void track_free(gpointer data)
{
    MdTrack *track = data;
    g_free(track->label);
    g_free(track->lang);
    g_free(track->url);
    g_free(track);
}

static MdTrack *track_new(const char *label, const char *lang, const char *url)
{
    MdTrack *track = g_new0(MdTrack, 1);
    track->label = g_strdup(label);
    track->lang = g_strdup(lang);
    track->url = g_strdup(url);
    return track;
}

/* Text für eine JSON-Zeichenkette schützen */
static void json_append_escaped(GString *out, const char *text)
{
    for (const char *p = text; *p; p++) {
        if (*p == '"' || *p == '\\') g_string_append_c(out, '\\');
        if ((unsigned char)*p < 0x20) g_string_append_printf(out, "\\u%04x", *p);
        else g_string_append_c(out, *p);
    }
}

/* Mediathek eines Senders; 0 = keine der unterstützten */
static int source_of(const char *channel)
{
    for (size_t i = 1; i < G_N_ELEMENTS(SOURCES); i++)
        if (!g_ascii_strcasecmp(channel, SOURCES[i].channel)) return SOURCES[i].source;
    if (!g_ascii_strncasecmp(channel, "ZDF", 3)) return MD_SOURCE_ZDF;
    return 0;
}

const char *md_source_label(int source)
{
    for (size_t i = 0; i < G_N_ELEMENTS(SOURCES); i++)
        if (SOURCES[i].source == source) return SOURCES[i].label;
    return "";
}

/* Nur Webadressen: Sie stammen aus fremden Antworten und gehen an curl, ffmpeg und den Browser */
static bool url_allowed(const char *url)
{
    return g_str_has_prefix(url, "https://") || g_str_has_prefix(url, "http://");
}

/* HLS-Wiedergabeliste statt Datei? */
static bool is_hls(const char *url)
{
    if (g_str_has_prefix(url, NRK_MANIFEST)) return true;   /* Stream erst beim Laden */
    const char *end = strpbrk(url, "?#");
    size_t length = end ? (size_t)(end - url) : strlen(url);
    return length >= 5 && !g_ascii_strncasecmp(url + length - 5, ".m3u8", 5);
}

/* Adressen, die aus Deutschland nicht abrufbar sind, und Streams ohne Unterstützung leeren */
static void drop_unusable_urls(MdTitle *t)
{
    char **urls[] = {&t->url_high, &t->url_medium, &t->url_low};
    for (size_t i = 0; i < G_N_ELEMENTS(urls); i++) {
        const char *url = *urls[i];
        bool usable = url_allowed(url) && (!is_hls(url) || (t->source & HLS_SOURCES));
        if (t->source == MD_SOURCE_SRF) usable &= strstr(url, "/world/") != NULL;
        if (!usable) **urls[i] = 0;
    }
}

/* "Haus aus Glas (3/6) - Das Verschwinden": Folge 3. Die Staffel steht manchmal im Thema
 * ("Soko Donau Staffel 10"), sonst gilt Staffel 1. Ist das Thema nur eine Rubrik wie
 * "Fernsehfilme und Serien - Serien", wird der Serienname zum Thema. */
static void part_analyse(MdTitle *t, GRegex *part, GRegex *topic_season)
{
    GMatchInfo *match = NULL;
    if (!g_regex_match(part, t->name, 0, &match)) {
        g_match_info_free(match);
        return;
    }
    char *number = g_match_info_fetch(match, 1);
    int start = 0;
    g_match_info_fetch_pos(match, 0, &start, NULL);
    char *series = g_strndup(t->name, (gsize)start);
    g_strstrip(series);
    t->episode = atoi(number);
    t->season = 1;
    g_free(number);
    g_clear_pointer(&match, g_match_info_free);
    if (g_regex_match(topic_season, t->topic, 0, &match)) {
        char *season = g_match_info_fetch(match, 1);
        g_match_info_fetch_pos(match, 0, &start, NULL);
        t->season = atoi(season);
        t->topic[start] = 0;
        g_free(season);
    }
    g_match_info_free(match);
    char *topic = g_utf8_strdown(t->topic, -1), *lower = g_utf8_strdown(series, -1);
    if (*series && !strstr(topic, lower)) {
        g_free(t->topic);
        t->topic = g_strdup(series);
    }
    char *rest = g_regex_replace_literal(part, t->name, -1, 0, "", 0, NULL);
    g_free(t->name);
    t->name = g_strstrip(rest);
    g_free(topic);
    g_free(lower);
    g_free(series);
}

/* Staffel, Folge und bereinigten Namen aus dem Titel ableiten */
static void title_analyse(MdTitle *t)
{
    static GRegex *episode_tag, *new_episodes, *numbered, *show_suffix, *srf_tag, *part;
    static GRegex *topic_season;
    static gsize once;
    if (g_once_init_enter(&once)) {
        episode_tag = g_regex_new(" ?\\(S(\\d+)/E(\\d+)\\)", 0, 0, NULL);
        srf_tag = g_regex_new("( –)? ?\\(Staffel (\\d+), Folge (\\d+)\\)", 0, 0, NULL);
        part = g_regex_new(" \\((\\d+)/(\\d+)\\)", 0, 0, NULL);
        topic_season = g_regex_new(" Staffel (\\d+)$", 0, 0, NULL);
        new_episodes = g_regex_new(" - Neue Folgen?\\b", 0, 0, NULL);
        numbered = g_regex_new("^Folge (\\d+)\\b|\\((\\d+)\\)$", 0, 0, NULL);
        show_suffix = g_regex_new(" · .*$", 0, 0, NULL);
        g_once_init_leave(&once, 1);
    }
    t->source = source_of(t->channel);
    drop_unusable_urls(t);
    if (!url_allowed(t->url_subtitle)) *t->url_subtitle = 0;
    if (!url_allowed(t->website)) *t->website = 0;
    t->season = t->episode = -1;
    GMatchInfo *match = NULL;
    if (g_regex_match(episode_tag, t->title, 0, &match)) {
        char *season = g_match_info_fetch(match, 1), *episode = g_match_info_fetch(match, 2);
        t->season = atoi(season);
        t->episode = atoi(episode);
        g_free(season);
        g_free(episode);
    }
    g_clear_pointer(&match, g_match_info_free);
    if (t->season < 0 && g_regex_match(srf_tag, t->title, 0, &match)) {
        char *season = g_match_info_fetch(match, 2), *episode = g_match_info_fetch(match, 3);
        t->season = atoi(season);
        t->episode = atoi(episode);
        g_free(season);
        g_free(episode);
    }
    g_clear_pointer(&match, g_match_info_free);
    char *without_srf = g_regex_replace_literal(srf_tag, t->title, -1, 0, "", 0, NULL);
    char *without_tag = g_regex_replace_literal(episode_tag, without_srf, -1, 0, "", 0, NULL);
    g_free(without_srf);
    t->name = g_regex_replace_literal(new_episodes, without_tag, -1, 0, "", 0, NULL);
    g_free(without_tag);
    char *show = g_strconcat(" | ", t->topic, NULL);     /* funk: "Titel | STRG_F" */
    if (g_str_has_suffix(t->name, show)) t->name[strlen(t->name) - strlen(show)] = 0;
    g_free(show);
    for (size_t i = 0; i < G_N_ELEMENTS(VARIANTS); i++) {
        char *found = strstr(t->name, VARIANTS[i].tag);
        if (!found) continue;
        memmove(found, found + strlen(VARIANTS[i].tag), strlen(found + strlen(VARIANTS[i].tag)) + 1);
        t->track_label = VARIANTS[i].label;
        t->track_lang = VARIANTS[i].lang;
        break;
    }
    g_strstrip(t->name);
    if ((t->source & ARD_STYLE)) {                   /* "… · Praxis mit Meerblick" */
        char *short_name = g_regex_replace_literal(show_suffix, t->name, -1, 0, "", 0, NULL);
        g_free(t->name);
        t->name = short_name;
    }

    if (t->season < 0 && (t->source & PART_STYLE)) part_analyse(t, part, topic_season);
    /* ARTE: eingeblendete Untertitel sind ein eigenes Bild wie die Gebärdensprache */
    t->is_variant = strstr(t->title, "Gebärdensprache") || strstr(t->title, "(mit Untertitel)");
    char *topic = g_utf8_strdown(t->topic, -1);
    t->is_film = t->season < 0 && (strstr(topic, "film") || strstr(topic, "fernsehspiel")
                                   || strstr(topic, "kino"));
    g_free(topic);
    /* Die ARD nennt oft nur "Folge 5": ohne Angabe gilt Staffel 1 */
    if (!(t->source & ZDF_STYLE) && t->season < 0 && !t->is_film &&
        g_regex_match(numbered, t->name, 0, &match)) {
        char *first = g_match_info_fetch(match, 1), *second = g_match_info_fetch(match, 2);
        t->season = 1;
        t->episode = atoi(first && *first ? first : second);
        g_free(first);
        g_free(second);
    }
    g_match_info_free(match);
    /* Titel wie "Der Prozess ()" */
    if (g_str_has_suffix(t->name, "()")) {
        t->name[strlen(t->name) - 2] = 0;
        g_strchomp(t->name);
    }
    /* "Der Nachtmann (1)": die Nummer steht schon in der Folge */
    char *number = !(t->source & ZDF_STYLE) ? strrchr(t->name, '(') : NULL;
    if (number && t->episode >= 0 && g_ascii_isdigit(number[1]) && g_str_has_suffix(t->name, ")")) {
        *number = 0;
        g_strchomp(t->name);
    }
}

/* Ein Objekt aus "results" lesen */
static MdTitle *json_read_title(Json *j)
{
    MdTitle *t = g_new0(MdTitle, 1);
    if (!json_accept(j, '{')) j->ok = false;
    if (j->ok && !json_accept(j, '}')) {
        do {
            char *key = json_string(j);
            if (!json_accept(j, ':')) j->ok = false;
            char **text = NULL;
            if (!strcmp(key, "channel")) text = &t->channel;
            else if (!strcmp(key, "topic")) text = &t->topic;
            else if (!strcmp(key, "title")) text = &t->title;
            else if (!strcmp(key, "description")) text = &t->description;
            else if (!strcmp(key, "url_website")) text = &t->website;
            else if (!strcmp(key, "url_video_hd")) text = &t->url_high;
            else if (!strcmp(key, "url_video")) text = &t->url_medium;
            else if (!strcmp(key, "url_video_low")) text = &t->url_low;
            else if (!strcmp(key, "url_subtitle")) text = &t->url_subtitle;
            json_spaces(j);
            if (text && *j->p == '"') {
                g_free(*text);
                *text = json_string(j);
            } else if (!strcmp(key, "timestamp")) {
                t->timestamp = (gint64)json_scalar(j);
            } else if (!strcmp(key, "duration")) {
                t->duration = (int)json_scalar(j);
            } else {
                json_skip(j, 2);
            }
            g_free(key);
        } while (j->ok && json_accept(j, ','));
        if (!json_accept(j, '}')) j->ok = false;
    }
    char **fields[] = {&t->channel, &t->topic, &t->title, &t->description, &t->website,
                       &t->url_high, &t->url_medium, &t->url_low, &t->url_subtitle};
    for (size_t i = 0; i < G_N_ELEMENTS(fields); i++)
        if (!*fields[i]) *fields[i] = g_strdup("");
    title_analyse(t);
    return t;
}

/* "results"-Liste lesen und an out anhängen */
static void json_read_results(Json *j, GPtrArray *out)
{
    if (!json_accept(j, '[')) {
        j->ok = false;
        return;
    }
    if (json_accept(j, ']')) return;
    do {
        g_ptr_array_add(out, json_read_title(j));
    } while (j->ok && json_accept(j, ','));
    if (!json_accept(j, ']')) j->ok = false;
}

/* Antwort der API auswerten -> false bei Fehler */
static bool parse_response(const char *text, GPtrArray *out, int *total, char **error)
{
    Json j = {text, true};
    *total = 0;
    if (!json_accept(&j, '{')) j.ok = false;
    while (j.ok) {
        char *key = json_string(&j);
        if (!json_accept(&j, ':')) j.ok = false;
        if (!strcmp(key, "result") && json_accept(&j, '{')) {
            while (j.ok) {
                char *inner = json_string(&j);
                if (!json_accept(&j, ':')) j.ok = false;
                if (!strcmp(inner, "results")) {
                    json_read_results(&j, out);
                } else if (!strcmp(inner, "queryInfo") && json_accept(&j, '{')) {
                    while (j.ok) {
                        char *field = json_string(&j);
                        if (!json_accept(&j, ':')) j.ok = false;
                        if (!strcmp(field, "totalResults")) *total = (int)json_scalar(&j);
                        else json_skip(&j, 3);
                        g_free(field);
                        if (!json_accept(&j, ',')) break;
                    }
                    if (!json_accept(&j, '}')) j.ok = false;
                } else {
                    json_skip(&j, 2);
                }
                g_free(inner);
                if (!json_accept(&j, ',')) break;
            }
            if (!json_accept(&j, '}')) j.ok = false;
        } else if (!strcmp(key, "err")) {
            json_spaces(&j);
            if (*j.p == 'n') {
                json_scalar(&j);
            } else if (j.ok) {
                *error = g_strdup("Die Mediathek-Suche meldet einen Fehler.");
                g_free(key);
                return false;
            }
        } else {
            json_skip(&j, 1);
        }
        g_free(key);
        if (!json_accept(&j, ',')) break;
    }
    if (!j.ok) *error = g_strdup("Die Antwort der Mediathek-Suche ist nicht lesbar.");
    return j.ok;
}

/* Aktueller Ordner des Zwischenspeichers */
static char *cache_dir(void)
{
    char *dir = g_build_filename(md_app_dir(), CACHE_DIR_NAME, NULL);
    g_mkdir_with_parents(dir, 0755);
    return dir;
}

/* Pfad im Zwischenspeicher: Prüfsumme von key plus Endung */
static char *cache_file(const char *key, const char *suffix)
{
    char *dir = cache_dir();
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, key, -1);
    char *name = g_strconcat(hash, suffix, NULL);
    char *path = g_build_filename(dir, name, NULL);
    g_free(dir);
    g_free(hash);
    g_free(name);
    return path;
}

/* ---------------------------------------------------------------------------
 * Jede Anfrage nur einmal: Will ein zweiter Thread dieselbe Datei des Zwischenspeichers füllen,
 * während der erste noch lädt, wartet er und liest danach das Ergebnis. Was gerade
 * fehlgeschlagen ist, wird FAILED_RETRY_SECONDS lang nicht noch einmal gefragt.
 * ------------------------------------------------------------------------- */
#define FAILED_RETRY_SECONDS 600

static GMutex flight_lock;
static GCond flight_done;
static GHashTable *flights;     /* Schlüssel, die gerade geladen werden */
static GHashTable *failures;    /* Schlüssel -> Zeitpunkt des letzten Fehlschlags (Sekunden) */

/* -> true, wenn key eben erst fehlgeschlagen ist (dann nicht ins Netz) */
static bool flight_begin(const char *key)
{
    g_mutex_lock(&flight_lock);
    if (!flights) {
        flights = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        failures = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    }
    while (g_hash_table_contains(flights, key)) g_cond_wait(&flight_done, &flight_lock);
    g_hash_table_add(flights, g_strdup(key));
    gint64 failed = GPOINTER_TO_SIZE(g_hash_table_lookup(failures, key));
    bool recent = failed && g_get_monotonic_time() / G_USEC_PER_SEC - failed < FAILED_RETRY_SECONDS;
    g_mutex_unlock(&flight_lock);
    return recent;
}

static void flight_end(const char *key, bool failed)
{
    g_mutex_lock(&flight_lock);
    g_hash_table_remove(flights, key);
    if (failed)
        g_hash_table_replace(failures, g_strdup(key),
                             GSIZE_TO_POINTER(MAX(g_get_monotonic_time() / G_USEC_PER_SEC, 1)));
    else
        g_hash_table_remove(failures, key);
    g_cond_broadcast(&flight_done);
    g_mutex_unlock(&flight_lock);
}

/* Datei seit since (Unix-Zeit) geschrieben? Dann hat sie ein anderer Thread gerade geholt. */
static bool written_since(const char *path, time_t since)
{
    GStatBuf st;
    return g_stat(path, &st) == 0 && st.st_mtime >= since;
}

static char *search_cache_path(const char *body)
{
    return cache_file(body, ".suche");
}

/* Gespeicherte Suchantwort lesen, solange sie frisch ist */
static bool search_cache_read(const char *path, char **reply)
{
    GStatBuf stat_buf;
    if (g_stat(path, &stat_buf) != 0 || time(NULL) - stat_buf.st_mtime >= SEARCH_CACHE_SECONDS)
        return false;
    return g_file_get_contents(path, reply, NULL, NULL);
}

/* Eine Seite der Suche abrufen; channel NULL = alle Sender */
static bool fetch_page(const char *channel, const char *text, const char *field, int min_seconds,
                       int offset, MdCache cache, GPtrArray *out, int *total, char **error)
{
    GString *body = g_string_new("{\"queries\":[");
    if (channel)
        g_string_append_printf(body, "{\"fields\":[\"channel\"],\"query\":\"%s\"}", channel);
    if (text && *text) {
        if (channel) g_string_append_c(body, ',');
        g_string_append_printf(body, "{\"fields\":[%s],\"query\":\"", field);
        json_append_escaped(body, text);
        g_string_append(body, "\"}");
    }
    g_string_append_printf(body, "],\"sortBy\":\"timestamp\",\"sortOrder\":\"desc\","
                           "\"future\":false,\"offset\":%d,\"size\":%d", offset, PAGE_SIZE);
    if (min_seconds > 0) g_string_append_printf(body, ",\"duration_min\":%d", min_seconds);
    g_string_append_c(body, '}');
    char *argv[] = {CURL_CMD, "-sS", "-f", "-L", "-m", "60", "-A", USER_AGENT, "-H",
                    "Content-Type: text/plain", "--data-binary", body->str, API_URL, NULL};
    char *reply = NULL, *messages = NULL;
    /* Antworten für die Vorschau werden aufgehoben, damit der Start ohne Netz auskommt */
    char *saved = cache != MD_CACHE_OFF ? search_cache_path(body->str) : NULL;
    int rc = 0;
    time_t asked = time(NULL);
    bool failed_before = saved && flight_begin(saved), tried = false;
    /* auch ein Neuladen muss nicht doppelt laufen, wenn ein anderer Thread gerade geholt hat */
    bool shared = saved && written_since(saved, asked) &&
                  g_file_get_contents(saved, &reply, NULL, NULL);
    if (!shared && (cache != MD_CACHE_USE || !search_cache_read(saved, &reply))) {
        if (failed_before) {
            rc = 1;
        } else {
            tried = true;
            rc = run_capture(argv, &reply, &messages);
            if (saved && rc == 0 && reply) g_file_set_contents(saved, reply, -1, NULL);
        }
    }
    if (saved) flight_end(saved, tried && rc != 0);
    g_free(saved);
    bool ok = false;
    if (rc == 0 && reply) {
        ok = parse_response(reply, out, total, error);
    } else {
        *error = g_strdup_printf("Die Mediathek-Suche ist nicht erreichbar (%s).",
                                 messages && *messages ? g_strstrip(messages) : "curl-Fehler");
    }
    g_string_free(body, TRUE);
    g_free(reply);
    g_free(messages);
    return ok;
}

/* Alle Seiten einer Anfrage abrufen, begrenzt auf SEARCH_MAX */
static bool fetch_all(const char *channel, const char *text, const char *field, int min_seconds,
                      MdCache cache, GPtrArray *out, const volatile gint *cancel, bool *truncated,
                      char **error)
{
    int total = 0;
    for (int offset = 0; offset < SEARCH_MAX; offset += PAGE_SIZE) {
        if (cancel && g_atomic_int_get(cancel)) return true;
        guint before = out->len;
        if (!fetch_page(channel, text, field, min_seconds, offset, cache, out, &total, error))
            return false;
        if (out->len == before || offset + PAGE_SIZE >= total) return true;
    }
    if (truncated) *truncated = true;
    return true;
}

/* Eintrag anzeigen? (Filter, die die API nicht kennt) */
static bool title_matches(const MdTitle *t, const MdQuery *query)
{
    if (!(t->source & (query->sources ? query->sources : MD_SOURCE_ALL))) return false;
    if (!*t->url_high && !*t->url_medium && !*t->url_low) return false;
    if (t->duration < query->min_minutes * 60 && t->source != MD_SOURCE_NETZKINO) return false;
    if (query->hide_variants && t->is_variant) return false;
    if (query->kind == MD_KIND_FILMS && !t->is_film) return false;
    if (query->kind == MD_KIND_SERIES && (t->is_film ||
                                          (t->season < 0 && (t->source & ZDF_STYLE))))
        return false;
    return true;
}

/* Schlüssel, unter dem die Fassungen eines Titels zusammenfinden */
static char *variant_key(const MdTitle *t)
{
    char *lower = g_utf8_strdown(t->name, -1);
    char *key = g_strdup_printf("%d\n%s\n%s\n%d\n%d", t->source, t->topic, lower, t->season,
                                t->episode);
    g_free(lower);
    return key;
}

/* Untertitel eines Eintrags aufnehmen (WebVTT liegt neben der XML-Datei) */
static void add_subtitle(MdTitle *main_title, const MdTitle *source)
{
    if (!*source->url_subtitle) return;
    const char *label = strstr(source->url_subtitle, "_eng_") ? "Englisch" : "Deutsch";
    const char *lang = strstr(source->url_subtitle, "_eng_") ? "eng" : "deu";
    for (guint i = 0; i < main_title->subtitles->len; i++) {
        const MdTrack *known = main_title->subtitles->pdata[i];
        if (!strcmp(known->label, label)) return;
    }
    char *url = g_strdup(source->url_subtitle);
    char *ard = strstr(url, "/ebutt/");
    if (g_str_has_suffix(url, ".xml")) {
        strcpy(url + strlen(url) - 4, ".vtt");
    } else if (ard) {                       /* ARD: dieselben Untertitel gibt es als WebVTT */
        *ard = 0;
        char *webvtt = g_strconcat(url, "/webvtt/", ard + strlen("/ebutt/"), ".vtt", NULL);
        g_free(url);
        url = webvtt;
    }
    g_ptr_array_add(main_title->subtitles, track_new(label, lang, url));
    g_free(url);
}

/* Sprachfassungen in ihrer Hauptfassung aufgehen lassen; die übernommenen Einträge
 * werden in raw zu NULL. Fehlt die Hauptfassung, bleibt die Fassung ein eigener Eintrag. */
static void merge_variants(GPtrArray *raw)
{
    GHashTable *mains = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < raw->len; i++) {
        MdTitle *t = raw->pdata[i];
        t->audio = g_ptr_array_new_with_free_func(track_free);
        t->subtitles = g_ptr_array_new_with_free_func(track_free);
        g_ptr_array_add(t->audio, track_new(t->track_label ? t->track_label : "Deutsch",
                                            t->track_lang ? t->track_lang : "deu", ""));
        add_subtitle(t, t);
        if (!t->track_label && !t->is_variant) g_hash_table_insert(mains, variant_key(t), t);
    }
    for (guint i = 0; i < raw->len; i++) {
        MdTitle *t = raw->pdata[i];
        if (!t->track_label) continue;
        char *key = variant_key(t);
        MdTitle *main_title = g_hash_table_lookup(mains, key);
        g_free(key);
        if (!main_title || ABS(main_title->duration - t->duration) > VARIANT_MAX_DIFFERENCE)
            continue;
        /* Ein HLS-Stream bringt alle Sprachen selbst mit: die Fassung nicht noch einmal laden */
        if (is_hls(md_title_url(main_title, MD_QUALITY_LOW))) {
            add_subtitle(main_title, t);
            md_title_free(t);
            raw->pdata[i] = NULL;
            continue;
        }
        bool known = false;
        for (guint n = 0; n < main_title->audio->len; n++)
            known |= !strcmp(((MdTrack *)main_title->audio->pdata[n])->label, t->track_label);
        if (!known)
            g_ptr_array_add(main_title->audio, track_new(t->track_label, t->track_lang,
                                                         md_title_url(t, MD_QUALITY_LOW)));
        add_subtitle(main_title, t);
        md_title_free(t);
        raw->pdata[i] = NULL;
    }
    g_hash_table_destroy(mains);
}

/* Eine Mediathek abfragen; channel NULL = alle Sender (nur mit Suchwort) */
static bool search_source(const char *channel, const MdQuery *query, GPtrArray *raw,
                          const volatile gint *cancel, bool *truncated, char **error)
{
    const char *text = query->text ? query->text : "";
    const char *both = "\"topic\",\"title\"";
    int min_seconds = query->min_minutes * 60;
    bool ok = true;
    if (!*text && query->kind == MD_KIND_FILMS) {
        for (size_t i = 0; ok && i < G_N_ELEMENTS(FILM_TOPICS); i++)
            ok = fetch_all(channel, FILM_TOPICS[i], "\"topic\"", min_seconds, query->cache, raw,
                           cancel, truncated, error);
    } else if (!*text && query->kind == MD_KIND_SERIES) {
        ok = fetch_all(channel, "", both, MAX(min_seconds, BROWSE_MIN_SECONDS), query->cache, raw,
                       cancel, truncated, error);
    } else {
        ok = fetch_all(channel, text, both, min_seconds, query->cache, raw, cancel, truncated,
                       error);
    }
    return ok;
}

/* Liste aus Texten lesen -> Texte durch Zeilenumbruch getrennt */
static char *json_read_list(Json *j)
{
    GString *out = g_string_new(NULL);
    json_spaces(j);
    if (*j->p != '[') {
        json_skip(j, 4);
        return g_string_free(out, FALSE);
    }
    j->p++;
    if (json_accept(j, ']')) return g_string_free(out, FALSE);
    do {
        json_spaces(j);
        if (*j->p == '"') {
            char *text = json_string(j);
            g_string_append_printf(out, "%s\n", text);
            g_free(text);
        } else {
            json_skip(j, 4);
        }
    } while (j->ok && json_accept(j, ','));
    if (!json_accept(j, ']')) j->ok = false;
    return g_string_free(out, FALSE);
}

/* Felder eines Netzkino-Films */
typedef struct {
    char *slug, *title, *content, *date, *genres, *thumbnail, *properties;
    char *stream, *fsk, *image;
    bool drm;
} NetzkinoPost;

static void netzkino_post_clear(NetzkinoPost *p)
{
    char **fields[] = {&p->slug, &p->title, &p->content, &p->date, &p->genres, &p->thumbnail,
                       &p->properties, &p->stream, &p->fsk, &p->image};
    for (size_t i = 0; i < G_N_ELEMENTS(fields); i++) g_clear_pointer(fields[i], g_free);
}

/* Bereich "custom_fields" */
static void netzkino_read_fields(Json *j, NetzkinoPost *p)
{
    if (!json_accept(j, '{')) {
        j->ok = false;
        return;
    }
    if (json_accept(j, '}')) return;
    do {
        char *key = json_string(j);
        if (!json_accept(j, ':')) j->ok = false;
        json_spaces(j);
        char **text = NULL;
        if (!strcmp(key, "Streaming")) text = &p->stream;
        else if (!strcmp(key, "FSK")) text = &p->fsk;
        else if (!strcmp(key, "featured_img_all_small")) text = &p->image;
        if (text) {
            g_free(*text);
            *text = json_read_list(j);
        } else if (!strcmp(key, "drm")) {
            p->drm = !strncmp(j->p, "true", 4);
            json_skip(j, 4);
        } else {
            json_skip(j, 4);
        }
        g_free(key);
    } while (j->ok && json_accept(j, ','));
    if (!json_accept(j, '}')) j->ok = false;
}

/* Ein Objekt aus "posts" lesen */
static void netzkino_read_post(Json *j, NetzkinoPost *p)
{
    if (!json_accept(j, '{')) {
        j->ok = false;
        return;
    }
    if (json_accept(j, '}')) return;
    do {
        char *key = json_string(j);
        if (!json_accept(j, ':')) j->ok = false;
        json_spaces(j);
        char **text = NULL, **list = NULL;
        if (!strcmp(key, "slug")) text = &p->slug;
        else if (!strcmp(key, "title")) text = &p->title;
        else if (!strcmp(key, "content")) text = &p->content;
        else if (!strcmp(key, "date")) text = &p->date;
        else if (!strcmp(key, "thumbnail")) text = &p->thumbnail;
        else if (!strcmp(key, "genres")) list = &p->genres;
        else if (!strcmp(key, "properties")) list = &p->properties;
        if (text && *j->p == '"') {
            g_free(*text);
            *text = json_string(j);
        } else if (list) {
            g_free(*list);
            *list = json_read_list(j);
        } else if (!strcmp(key, "custom_fields")) {
            netzkino_read_fields(j, p);
        } else {
            json_skip(j, 3);
        }
        g_free(key);
    } while (j->ok && json_accept(j, ','));
    if (!json_accept(j, '}')) j->ok = false;
}

/* Merkzettel wie bei den Mediatheken: Bild, FSK, je Zeile eine Kategorie */
static void netzkino_write_info(const NetzkinoPost *p, const char *website)
{
    char *path = cache_file(website, ".info");
    char *image = p->image && g_str_has_prefix(p->image, "https://") ? g_strdup(p->image) :
                  g_strdup(p->thumbnail ? p->thumbnail : "");
    char *end = strchr(image, '\n');
    if (end) *end = 0;
    char *fsk = g_strdup(p->fsk ? p->fsk : "");
    if ((end = strchr(fsk, '\n'))) *end = 0;
    bool usable = g_str_has_prefix(image, "https://") && !strpbrk(image, " \\'");
    char *info = g_strdup_printf("%s\n%s%s\n%s", usable ? image : "", *fsk ? "FSK " : "", fsk,
                                 p->genres ? p->genres : "");
    g_file_set_contents(path, info, -1, NULL);
    g_free(info);
    g_free(fsk);
    g_free(image);
    g_free(path);
}

/* Nur frei verfügbare Filme ohne Kopierschutz aufnehmen; seen verhindert Doppelte */
static void netzkino_add_post(const NetzkinoPost *p, const MdQuery *query, GPtrArray *out,
                              GHashTable *seen)
{
    if (!p->slug || !p->title || !p->stream || !*p->stream || p->drm) return;
    if (!p->properties || !strstr(p->properties, "avod\n")) return;
    if (strpbrk(p->slug, "/ ") || !g_hash_table_add(seen, g_strdup(p->slug))) return;
    if (query->text && *query->text) {
        char *a = g_utf8_strdown(p->title, -1), *b = g_utf8_strdown(query->text, -1);
        bool hit = strstr(a, b) != NULL;
        g_free(a);
        g_free(b);
        if (!hit) return;
    }
    char *stream = g_strdup(p->stream);
    *strchr(stream, '\n') = 0;
    MdTitle *t = g_new0(MdTitle, 1);
    t->channel = g_strdup("Netzkino");
    t->topic = g_strdup("Filme");
    t->title = g_strdup(p->title);
    t->description = g_strdup(p->content ? p->content : "");
    t->website = g_strconcat(NETZKINO_PAGE, p->slug, NULL);
    char *escaped = g_uri_escape_string(stream, "/", FALSE);        /* "Great Movies/…" */
    t->url_high = g_strconcat(NETZKINO_VIDEO, escaped, ".mp4", NULL);
    g_free(escaped);
    t->url_medium = g_strdup("");
    t->url_low = g_strdup("");
    t->url_subtitle = g_strdup("");
    GDateTime *date = p->date ? g_date_time_new_from_iso8601(p->date, NULL) : NULL;
    if (date) {
        t->timestamp = g_date_time_to_unix(date);
        g_date_time_unref(date);
    }
    title_analyse(t);
    netzkino_write_info(p, t->website);
    g_ptr_array_add(out, t);
    g_free(stream);
}

static void netzkino_parse(const char *reply, const MdQuery *query, GPtrArray *out,
                           GHashTable *seen)
{
    Json j = {reply, true};
    if (!json_accept(&j, '{')) return;
    while (j.ok) {
        char *key = json_string(&j);
        if (!json_accept(&j, ':')) j.ok = false;
        if (!strcmp(key, "posts") && json_accept(&j, '[')) {
            if (!json_accept(&j, ']')) {
                do {
                    NetzkinoPost post = {0};
                    netzkino_read_post(&j, &post);
                    if (j.ok) netzkino_add_post(&post, query, out, seen);
                    netzkino_post_clear(&post);
                } while (j.ok && json_accept(&j, ','));
                if (!json_accept(&j, ']')) j.ok = false;
            }
        } else {
            json_skip(&j, 2);
        }
        g_free(key);
        if (!json_accept(&j, ',')) break;
    }
}

/* Filme von Netzkino holen. Fällt der Dienst aus, bleiben die übrigen Mediatheken nutzbar. */
static void netzkino_fetch(const MdQuery *query, GPtrArray *out)
{
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (size_t i = 0; i < G_N_ELEMENTS(NETZKINO_CATEGORIES); i++) {
        char *url = g_strdup_printf(NETZKINO_API "%d.json?d=www&l=de-DE", NETZKINO_CATEGORIES[i]);
        char *saved = search_cache_path(url), *reply = NULL, *messages = NULL;
        char *argv[] = {CURL_CMD, "-sS", "-f", "-L", "-m", "60", "-A", USER_AGENT, url, NULL};
        if (query->cache == MD_CACHE_REFRESH || !search_cache_read(saved, &reply)) {
            if (run_capture(argv, &reply, &messages) == 0 && reply)
                g_file_set_contents(saved, reply, -1, NULL);
            else md_log("Netzkino nicht erreichbar: %s", url);
        }
        if (reply) netzkino_parse(reply, query, out, seen);
        g_free(url);
        g_free(saved);
        g_free(reply);
        g_free(messages);
    }
    g_hash_table_destroy(seen);
}

/* ---------------------------------------------------------------------------
 * Eigene Schnittstellen: gemeinsame Helfer
 * ------------------------------------------------------------------------- */
/* Kleiner JSON-Baum für verschachtelte Antworten (NRK) */
typedef struct JsonNode {
    char type;                  /* o Objekt, a Liste, s Text, n Zahl, b/0 wahr/falsch/null */
    char *text;                 /* s */
    double number;              /* n, b (1 = true) */
    GPtrArray *keys, *items;    /* o: keys[i] -> items[i]; a: items */
} JsonNode;

static void json_node_free(gpointer data)
{
    JsonNode *n = data;
    if (!n) return;
    g_free(n->text);
    if (n->keys) g_ptr_array_free(n->keys, TRUE);
    if (n->items) g_ptr_array_free(n->items, TRUE);
    g_free(n);
}

static JsonNode *json_node_parse(Json *j, int depth)
{
    json_spaces(j);
    if (depth > JSON_MAX_DEPTH) j->ok = false;
    if (!j->ok) return NULL;
    JsonNode *n = g_new0(JsonNode, 1);
    if (*j->p == '"') {
        n->type = 's';
        n->text = json_string(j);
    } else if (*j->p == '{' || *j->p == '[') {
        bool object = *j->p == '{';
        char close = object ? '}' : ']';
        n->type = object ? 'o' : 'a';
        n->items = g_ptr_array_new_with_free_func(json_node_free);
        if (object) n->keys = g_ptr_array_new_with_free_func(g_free);
        j->p++;
        if (!json_accept(j, close)) {
            do {
                if (object) {
                    g_ptr_array_add(n->keys, json_string(j));
                    if (!json_accept(j, ':')) j->ok = false;
                }
                JsonNode *child = json_node_parse(j, depth + 1);
                g_ptr_array_add(n->items, child);
                if (!child) j->ok = false;
            } while (j->ok && json_accept(j, ','));
            if (!json_accept(j, close)) j->ok = false;
        }
    } else {
        n->type = !strncmp(j->p, "true", 4) ? 'b' : !strncmp(j->p, "false", 5) ? 'f' : 'n';
        n->number = json_scalar(j);
        if (n->type == 'b') n->number = 1;
    }
    return n;
}

/* ganze Antwort als Baum; NULL = kein gültiges JSON */
static JsonNode *json_tree(const char *text)
{
    Json j = {text ? text : "", true};
    JsonNode *root = text ? json_node_parse(&j, 0) : NULL;
    if (!j.ok) g_clear_pointer(&root, json_node_free);
    return root;
}

/* Pfad wie "usageRights/geoBlock/isGeoBlocked"; Zahlen wählen in Listen aus */
static const JsonNode *json_at(const JsonNode *n, const char *path)
{
    char **parts = g_strsplit(path, "/", -1);
    for (int i = 0; n && parts[i]; i++) {
        const JsonNode *next = NULL;
        if (n->type == 'o') {
            for (guint k = 0; k < n->keys->len && !next; k++)
                if (!strcmp(n->keys->pdata[k], parts[i])) next = n->items->pdata[k];
        } else if (n->type == 'a' && g_ascii_isdigit(*parts[i])) {
            guint index = (guint)atoi(parts[i]);
            if (index < n->items->len) next = n->items->pdata[index];
        }
        n = next;
    }
    g_strfreev(parts);
    return n;
}

static const char *json_text_at(const JsonNode *n, const char *path)
{
    const JsonNode *found = json_at(n, path);
    return found && found->type == 's' ? found->text : NULL;
}

static double json_number_at(const JsonNode *n, const char *path, double fallback)
{
    const JsonNode *found = json_at(n, path);
    return found && (found->type == 'n' || found->type == 'b' || found->type == 'f')
           ? found->number : fallback;
}

static guint json_count(const JsonNode *n)
{
    return n && n->type == 'a' ? n->items->len : 0;
}

/* Bild in passender Breite aus einer Liste {url|uri, width} */
static char *json_pick_image(const JsonNode *list, int width)
{
    const char *best = NULL;
    double best_width = 0;
    for (guint i = 0; i < json_count(list); i++) {
        const JsonNode *image = list->items->pdata[i];
        const char *url = json_text_at(image, "url");
        if (!url) url = json_text_at(image, "uri");
        double w = json_number_at(image, "width", 0);
        if (url && url_allowed(url) && (!best || (w >= width && (best_width < width ||
                                                                  w < best_width)) ||
                                        (best_width < width && w > best_width))) {
            best = url;
            best_width = w;
        }
    }
    return best ? g_strdup(best) : NULL;
}

/* Adresse laden. max_age > 0: aus dem Zwischenspeicher, solange jünger; < 0: für immer */
static char *fetch_url(const char *url, int max_age, MdCache cache)
{
    char *saved = cache_file(url, ".api"), *reply = NULL;
    time_t asked = time(NULL);
    bool failed_before = flight_begin(saved), failed = false;
    GStatBuf st;
    bool fresh = (cache != MD_CACHE_REFRESH || written_since(saved, asked)) &&
                 g_stat(saved, &st) == 0 && (max_age < 0 || time(NULL) - st.st_mtime < max_age);
    if ((!fresh || !g_file_get_contents(saved, &reply, NULL, NULL)) && !failed_before) {
        char *messages = NULL;
        char *argv[] = {CURL_CMD, "-sS", "-f", "-L", "-m", "60", "-A", USER_AGENT, "--url",
                        (char *)url, NULL};
        if (run_capture(argv, &reply, &messages) == 0 && reply) {
            g_file_set_contents(saved, reply, -1, NULL);
        } else {
            md_log("Nicht erreichbar: %s", url);
            g_clear_pointer(&reply, g_free);
            failed = true;
        }
        g_free(messages);
    }
    flight_end(saved, failed);
    g_free(saved);
    return reply;
}

/* Suchwort nur aus Buchstaben, Ziffern und Leerzeichen: geht so in fremde Abfragen */
static char *plain_words(const char *text)
{
    GString *out = g_string_new(NULL);
    for (const char *p = text; p && *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (g_unichar_isalnum(c)) g_string_append_unichar(out, c);
        else if (out->len && out->str[out->len - 1] != ' ') g_string_append_c(out, ' ');
    }
    return g_strstrip(g_string_free(out, FALSE));
}

/* Merkzettel wie bei den Mediatheken schreiben: Bild, FSK (leer), je Zeile eine Kategorie */
static void write_saved_info(const char *website, const char *image, const char *genres)
{
    char *path = cache_file(website, ".info");
    bool usable = image && g_str_has_prefix(image, "https://") && !strpbrk(image, " \\'\n");
    char *info = g_strdup_printf("%s\n\n%s", usable ? image : "", genres ? genres : "");
    g_file_set_contents(path, info, -1, NULL);
    g_free(info);
    g_free(path);
}

/* ---------------------------------------------------------------------------
 * NRK
 * ------------------------------------------------------------------------- */
typedef struct {
    char *id;                   /* "humoretaten" */
    MdCache cache;
    GPtrArray *titles;          /* MdTitle* */
} NrkSeries;

static void nrk_series_free(gpointer data)
{
    NrkSeries *s = data;
    g_free(s->id);
    if (s->titles) g_ptr_array_free(s->titles, TRUE);
    g_free(s);
}

/* Folge zum Titel machen; NULL = nicht weltweit oder nicht verfügbar */
static MdTitle *nrk_episode(const JsonNode *e, const char *series_id, const char *series,
                            int season)
{
    const char *prf = json_text_at(e, "prfId"), *title = json_text_at(e, "titles/title");
    const char *subtitle = json_text_at(e, "titles/subtitle");
    if (!prf || !title || strpbrk(prf, "/?#& \n") || strchr(title, '\n')) return NULL;
    if (json_number_at(e, "usageRights/geoBlock/isGeoBlocked", 1) != 0) return NULL;
    if (g_strcmp0(json_text_at(e, "availability/status"), "available")) return NULL;
    int episode = (int)json_number_at(e, "sequenceNumber", -1);
    MdTitle *t = g_new0(MdTitle, 1);
    t->channel = g_strdup("NRK");
    t->topic = g_strdup(series);
    /* "1. episode" sagt nichts: dann den Untertitel nehmen */
    const char *name = subtitle && g_regex_match_simple("^\\d+\\. episode$", title, 0, 0)
                       ? subtitle : title;
    while (g_str_has_prefix(name, "– ")) name += strlen("– ");
    t->title = season > 0 && episode > 0
             ? g_strdup_printf("%s (S%02d/E%02d)", name, season, episode) : g_strdup(name);
    t->description = g_strdup(subtitle && name != subtitle ? subtitle : "");
    t->website = season > 0 ? g_strdup_printf(NRK_PAGE "%s/sesong/%d/episode/%s", series_id,
                                              season, prf)
                            : g_strdup_printf(NRK_PAGE "%s/%s", series_id, prf);
    t->url_high = g_strconcat(NRK_MANIFEST, prf, NULL);
    t->url_medium = g_strdup("");
    t->url_low = g_strdup("");
    t->url_subtitle = g_strdup("");
    t->duration = (int)json_number_at(e, "durationInSeconds", 0);
    const char *date = json_text_at(e, "releaseDateOnDemand");
    GDateTime *when = date ? g_date_time_new_from_iso8601(date, NULL) : NULL;
    if (when) {
        t->timestamp = g_date_time_to_unix(when);
        g_date_time_unref(when);
    }
    title_analyse(t);
    t->track_label = "Norwegisch";
    t->track_lang = "nor";
    char *image = json_pick_image(json_at(e, "image"), 600);
    write_saved_info(t->website, image, NULL);
    g_free(image);
    return t;
}

/* Katalog einer Serie lesen und ihre freien Folgen nach out */
static void nrk_parse_catalog(const char *reply, const char *id, GPtrArray *out)
{
    JsonNode *root = json_tree(reply);
    const char *type = json_text_at(root, "seriesType");
    char *title_path = g_strdup_printf("%s/titles/title", type ? type : "standard");
    const char *series = json_text_at(root, title_path);
    const JsonNode *seasons = json_at(root, "_embedded/seasons");
    for (guint i = 0; series && !strchr(series, '\n') && i < json_count(seasons); i++) {
        const JsonNode *season = seasons->items->pdata[i];
        int number = (int)json_number_at(season, "sequenceNumber", 0);
        const JsonNode *episodes = json_at(season, "_embedded/episodes");
        for (guint n = 0; n < json_count(episodes); n++) {
            MdTitle *t = nrk_episode(episodes->items->pdata[n], id, series, number);
            if (t) g_ptr_array_add(out, t);
        }
    }
    g_free(title_path);
    json_node_free(root);
}

/* im Pool: Katalog einer Serie laden und ihre freien Folgen sammeln */
static void nrk_series_worker(gpointer data, gpointer user_data)
{
    (void)user_data;
    NrkSeries *s = data;
    s->titles = g_ptr_array_new_with_free_func(md_title_free);
    char *url = g_strconcat(NRK_CATALOG, s->id, NULL);
    char *reply = fetch_url(url, SEARCH_CACHE_SECONDS, s->cache);
    if (reply) nrk_parse_catalog(reply, s->id, s->titles);
    g_free(reply);
    g_free(url);
}

/* Serienkennungen sammeln: Felder "seriesId" irgendwo im Baum der Startseite */
static void nrk_collect_series(const JsonNode *n, GPtrArray *ids, GHashTable *seen)
{
    for (guint i = 0; n && n->items && i < n->items->len; i++) {
        const JsonNode *child = n->items->pdata[i];
        const char *id = n->keys && child && child->type == 's' &&
                         !strcmp(n->keys->pdata[i], "seriesId") ? child->text : NULL;
        if (id && *id && !strpbrk(id, "/?#& \n") && g_hash_table_add(seen, g_strdup(id)))
            g_ptr_array_add(ids, g_strdup(id));
        nrk_collect_series(child, ids, seen);
    }
}

/* Serien der NRK: ohne Suchwort die der Startseite, sonst die besten Suchtreffer */
static void nrk_fetch(const MdQuery *query, GPtrArray *out)
{
    bool search = query->text && *query->text;
    char *words = search ? plain_words(query->text) : NULL;
    char *url;
    if (search) {
        char *escaped = g_uri_escape_string(words, NULL, FALSE);
        url = g_strdup_printf(NRK_API "/search?q=%s", escaped);
        g_free(escaped);
    } else {
        url = g_strdup(NRK_API "/tv/pages/frontpage");
    }
    char *reply = search && !*words ? NULL : fetch_url(url, SEARCH_CACHE_SECONDS, query->cache);
    JsonNode *root = json_tree(reply);
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (search) {
        const JsonNode *hits = json_at(root, "hits");
        for (guint i = 0; i < json_count(hits) && ids->len < NRK_SEARCH_SERIES; i++) {
            const JsonNode *hit = hits->items->pdata[i];
            const char *id = json_text_at(hit, "hit/id");
            if (!g_strcmp0(json_text_at(hit, "type"), "serie") && id && *id &&
                !strpbrk(id, "/?#& \n") && g_hash_table_add(seen, g_strdup(id)))
                g_ptr_array_add(ids, g_strdup(id));
        }
    } else {
        nrk_collect_series(root, ids, seen);
    }
    GPtrArray *series = g_ptr_array_new_with_free_func(nrk_series_free);
    GThreadPool *pool = g_thread_pool_new(nrk_series_worker, NULL, NRK_THREADS, FALSE, NULL);
    for (guint i = 0; i < ids->len; i++) {
        NrkSeries *s = g_new0(NrkSeries, 1);
        s->id = g_strdup(ids->pdata[i]);
        s->cache = query->cache;
        g_ptr_array_add(series, s);
        g_thread_pool_push(pool, s, NULL);
    }
    g_thread_pool_free(pool, FALSE, TRUE);
    for (guint i = 0; i < series->len; i++) {
        NrkSeries *s = series->pdata[i];
        while (s->titles->len) g_ptr_array_add(out, g_ptr_array_steal_index(s->titles, 0));
    }
    g_ptr_array_free(series, TRUE);
    g_hash_table_destroy(seen);
    g_ptr_array_free(ids, TRUE);
    json_node_free(root);
    g_free(reply);
    g_free(url);
    g_free(words);
}

/* Beim Laden: Stream einer NRK-Folge erfragen. Nur unverschlüsselt und vom weltweiten Netz. */
static char *nrk_stream(const char *manifest, char **error)
{
    char *messages = NULL, *reply = NULL;
    char *argv[] = {CURL_CMD, "-sS", "-f", "-L", "-m", "30", "-A", USER_AGENT, "--url",
                    (char *)manifest, NULL};
    if (run_capture(argv, &reply, &messages) != 0 || !reply) {
        *error = g_strdup_printf("NRK nicht erreichbar (%s).",
                                 messages && *messages ? g_strstrip(messages) : "curl-Fehler");
        g_free(messages);
        g_free(reply);
        return NULL;
    }
    g_free(messages);
    JsonNode *root = json_tree(reply);
    const char *url = json_text_at(root, "playable/assets/0/url");
    const char *scheme = json_text_at(root, "playable/assets/0/encryptionScheme");
    char *stream = NULL;
    if (g_strcmp0(json_text_at(root, "playability"), "playable"))
        *error = g_strdup("Die Folge ist bei der NRK derzeit nicht abspielbar.");
    else if (json_number_at(root, "availability/isGeoBlocked", 1) != 0)
        *error = g_strdup("Die Folge ist nur in Norwegen abrufbar.");
    else if (json_number_at(root, "playable/assets/0/encrypted", 1) != 0 ||
             g_strcmp0(scheme, "none"))
        *error = g_strdup("Die Folge ist verschlüsselt und lässt sich nicht laden.");
    else if (!url || !g_str_has_prefix(url, NRK_STREAM_PREFIX) || !is_hls(url))
        *error = g_strdup("Die NRK nennt keinen passenden Stream.");
    else stream = g_strdup(url);
    json_node_free(root);
    g_free(reply);
    return stream;
}

/* Beim Stöbern nach Serien: Einzelsendungen ohne Staffelangabe sind keine Serie */
static void drop_single_shows(GPtrArray *titles)
{
    GHashTable *counts = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < titles->len; i++) {
        const char *topic = ((MdTitle *)titles->pdata[i])->topic;
        int count = GPOINTER_TO_INT(g_hash_table_lookup(counts, topic));
        g_hash_table_insert(counts, (char *)topic, GINT_TO_POINTER(count + 1));
    }
    for (guint i = titles->len; i-- > 0;) {
        const MdTitle *t = titles->pdata[i];
        if (t->season < 0 && GPOINTER_TO_INT(g_hash_table_lookup(counts, t->topic)) < 2)
            g_ptr_array_remove_index(titles, i);
    }
    g_hash_table_destroy(counts);
}

GPtrArray *md_search(const MdQuery *query, const volatile gint *cancel, bool *truncated,
                     char **error)
{
    GPtrArray *raw = g_ptr_array_new_with_free_func(md_title_free);
    bool ok = true;
    if (truncated) *truncated = false;
    int sources = query->sources ? query->sources : MD_SOURCE_ALL;
    int channels = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(SOURCES); i++)
        channels += (sources & SOURCES[i].source) && SOURCES[i].query;
    if (query->text && *query->text && channels > 1) {
        /* Suchwort: eine Anfrage über alle Sender statt einer je Sender; title_matches
         * sortiert die nicht gewählten aus */
        ok = search_source(NULL, query, raw, cancel, truncated, error);
    } else {
        for (size_t i = 0; ok && i < G_N_ELEMENTS(SOURCES); i++)
            if ((sources & SOURCES[i].source) && SOURCES[i].query)
                ok = search_source(SOURCES[i].query, query, raw, cancel, truncated, error);
    }
    if (ok && (sources & MD_SOURCE_NETZKINO) && query->kind != MD_KIND_SERIES)
        netzkino_fetch(query, raw);
    if (ok && (sources & MD_SOURCE_NRK) && query->kind != MD_KIND_FILMS) nrk_fetch(query, raw);
    if (!ok) {
        g_ptr_array_free(raw, TRUE);
        return NULL;
    }

    merge_variants(raw);

    /* Filtern; dieselbe Videodatei kann unter mehreren Rubriken stehen */
    GPtrArray *result = g_ptr_array_new_with_free_func(md_title_free);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    GHashTable *episodes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < raw->len; i++) {
        MdTitle *t = raw->pdata[i];
        if (!t) continue;
        const char *url = md_title_url(t, MD_QUALITY_HIGH);
        if (!title_matches(t, query) || !g_hash_table_add(seen, (char *)url)) continue;
        /* Die ARD führt manche Folge doppelt: mit Staffelangabe und nur mit Nummer */
        if (!(t->source & ZDF_STYLE) && t->season >= 0 && !t->track_label &&
            !g_hash_table_add(episodes, variant_key(t)))
            continue;
        raw->pdata[i] = NULL;
        g_ptr_array_add(result, t);
    }
    g_hash_table_destroy(seen);
    g_hash_table_destroy(episodes);
    g_ptr_array_free(raw, TRUE);
    if (query->kind == MD_KIND_SERIES) drop_single_shows(result);
    return result;
}

const char *md_title_url(const MdTitle *t, MdQuality quality)
{
    const char *order[3][3] = {{t->url_high, t->url_medium, t->url_low},
                               {t->url_medium, t->url_low, t->url_high},
                               {t->url_low, t->url_medium, t->url_high}};
    for (int i = 0; i < 3; i++)
        if (*order[quality][i]) return order[quality][i];
    return "";
}

gint64 md_title_estimate(const MdTitle *t, MdQuality quality)
{
    return (gint64)(t->duration * QUALITY_KBIT_PER_SECOND[quality] * 1000 / 8);
}

/* ---------------------------------------------------------------------------
 * Pfade
 * ------------------------------------------------------------------------- */
/* Text auf höchstens max Bytes kürzen, ohne ein UTF-8-Zeichen zu zerteilen */
static void cut_name(char *name, size_t max)
{
    if (strlen(name) <= max) return;
    const char *end = g_utf8_find_prev_char(name, name + max + 1);
    name[end ? (size_t)(end - name) : max] = 0;
    g_strchomp(name);
}

/* Text als Datei- oder Ordnernamen nutzbar machen */
static char *safe_name(const char *text)
{
    GString *s = g_string_new(NULL);
    for (const char *p = text; *p; p++) {
        if (strchr("/\\:*?\"<>|", *p)) g_string_append_c(s, *p == ':' ? ' ' : '-');
        else if ((unsigned char)*p >= 0x20) g_string_append_c(s, *p);
    }
    char *name = g_string_free(s, FALSE);
    g_strstrip(name);
    while (*name == '.') memmove(name, name + 1, strlen(name));
    size_t length = strlen(name);
    while (length && (name[length - 1] == '.' || name[length - 1] == ' ')) name[--length] = 0;
    cut_name(name, NAME_MAX_BYTES);
    if (!*name) {
        g_free(name);
        return g_strdup("Unbenannt");
    }
    return name;
}

char *md_target_path(const MdTitle *t, const char *dir)
{
    char *topic = safe_name(t->topic), *name = safe_name(t->name), *file, *path;
    if (t->season >= 0) {
        char *season = g_strdup_printf("Staffel %02d", t->season);
        char *head = g_strdup_printf("%s - S%02dE%02d - ", topic, t->season, t->episode);
        /* Sendung und Folge sind einzeln begrenzt, zusammen wären sie zu lang für einen Namen */
        cut_name(name, FILE_MAX_BYTES - strlen(head) - strlen(".mp4"));
        file = g_strconcat(head, name, ".mp4", NULL);
        path = g_build_filename(dir, "Serien", topic, season, file, NULL);
        g_free(head);
        g_free(season);
    } else if (t->is_film) {
        file = g_strconcat(name, ".mp4", NULL);
        path = g_build_filename(dir, "Filme", file, NULL);
    } else {
        /* Sendungen ohne Folgenangabe heißen oft gleich – das Datum macht sie eindeutig */
        char date[16] = "";
        time_t when = (time_t)t->timestamp;
        struct tm tm;
        localtime_r(&when, &tm);
        strftime(date, sizeof date, "%Y-%m-%d", &tm);
        file = g_strdup_printf("%s - %s.mp4", date, name);
        path = g_build_filename(dir, topic, file, NULL);
    }
    g_free(topic);
    g_free(name);
    g_free(file);
    return path;
}

/* Eindeutiger Name eines Titels, unabhängig von Qualität und Speicherort. Folgen zählen nach
 * Staffel und Folge, weil das ZDF die Titel nachträglich ändert ("- Neue Folgen"). */
static char *title_key(const MdTitle *t)
{
    char *topic = g_utf8_strdown(t->topic, -1), *name = g_utf8_strdown(t->name, -1), *key;
    if (t->season >= 0) {
        key = g_strdup_printf("%s|S%dE%d", topic, t->season, t->episode);
    } else if (t->is_film) {
        key = g_strdup_printf("film|%s", name);
    } else {
        key = g_strdup_printf("%s|%s|%" G_GINT64_FORMAT, topic, name, t->timestamp / 86400);
    }
    g_free(topic);
    g_free(name);
    return key;
}

char *md_title_key(const MdTitle *t)
{
    return title_key(t);
}

/* ---------------------------------------------------------------------------
 * Verlauf
 *
 * Merkt sich jede fertige Datei in ~/.mediathek-downloader/verlauf.tsv. So bleibt eine Folge
 * auch dann als geladen erkennbar, wenn der Speicherort inzwischen ein anderer ist.
 * -------------------------------------------------------------------------*/
typedef struct {
    char *key;
    MdHistoryEntry e;
} History;

static void history_free(gpointer data)
{
    History *h = data;
    g_free(h->key);
    g_free(h->e.topic);
    g_free(h->e.title);
    g_free(h->e.path);
    g_free(h);
}

static char *history_path(void)
{
    return g_build_filename(md_app_dir(), HISTORY_FILE, NULL);
}

/* Zeile der Verlaufsdatei bauen */
static void history_append_line(GString *out, const History *h)
{
    const char *fields[] = {h->key, h->e.topic, h->e.title, h->e.path};
    for (size_t i = 0; i < G_N_ELEMENTS(fields); i++) {
        char *escaped = g_strescape(fields[i], NULL);
        g_string_append_printf(out, "%s\t", escaped);
        g_free(escaped);
    }
    g_string_append_printf(out, "%d\t%d\t%" G_GINT64_FORMAT "\t%" G_GINT64_FORMAT "\n", h->e.season,
                           h->e.episode, h->e.size, h->e.finished);
}

/* Verlauf laden; spätere Zeilen ersetzen frühere mit demselben Schlüssel */
static void history_load(void)
{
    q.history = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, history_free);
    char *path = history_path(), *text = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL)) {
        char **lines = g_strsplit(text, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char **f = g_strsplit(lines[i], "\t", -1);
            if (g_strv_length(f) == 8) {
                History *h = g_new0(History, 1);
                h->key = g_strcompress(f[0]);
                h->e.topic = g_strcompress(f[1]);
                h->e.title = g_strcompress(f[2]);
                h->e.path = g_strcompress(f[3]);
                h->e.season = atoi(f[4]);
                h->e.episode = atoi(f[5]);
                h->e.size = g_ascii_strtoll(f[6], NULL, 10);
                h->e.finished = g_ascii_strtoll(f[7], NULL, 10);
                g_hash_table_replace(q.history, h->key, h);
            }
            g_strfreev(f);
        }
        g_strfreev(lines);
    }
    g_free(path);
    g_free(text);
}

/* Fertigen Auftrag eintragen; q.lock wird gehalten */
static void history_add(const Job *j)
{
    char *result = result_path(j->target);
    History *h = g_new0(History, 1);
    h->key = g_strdup(j->key);
    h->e.topic = g_strdup(j->topic);
    h->e.title = g_strdup(j->title);
    h->e.path = g_strdup(file_size(result) > 0 ? result : j->target);
    h->e.season = j->season;
    h->e.episode = j->episode;
    h->e.size = MAX(file_size(h->e.path), 0);
    /* nachträglich umgewandelt: geladen wurde die Datei schon früher */
    const History *before = *j->url ? NULL : g_hash_table_lookup(q.history, j->key);
    h->e.finished = before ? before->e.finished : g_get_real_time() / G_USEC_PER_SEC;
    g_free(result);

    GString *line = g_string_new(NULL);
    history_append_line(line, h);
    char *path = history_path();
    FILE *file = g_fopen(path, "a");
    if (file) {
        fputs(line->str, file);
        fclose(file);
    } else {
        md_log("Verlauf lässt sich nicht schreiben: %s", g_strerror(errno));
    }
    g_free(path);
    g_string_free(line, TRUE);
    g_hash_table_replace(q.history, h->key, h);
}

/* Verlauf komplett neu schreiben; q.lock wird gehalten */
static void history_save(void)
{
    GString *out = g_string_new(NULL);
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, q.history);
    while (g_hash_table_iter_next(&iter, NULL, &value)) history_append_line(out, value);
    char *path = history_path();
    g_file_set_contents(path, out->str, out->len, NULL);
    g_free(path);
    g_string_free(out, TRUE);
}

static int history_compare(gconstpointer a, gconstpointer b)
{
    const MdHistoryEntry *x = a, *y = b;
    int order = g_utf8_collate(x->topic, y->topic);
    if (!order) order = x->season - y->season;
    if (!order) order = x->episode - y->episode;
    return order ? order : g_utf8_collate(x->title, y->title);
}

MdHistoryEntry *md_history_snapshot(int *count)
{
    g_mutex_lock(&q.lock);
    *count = (int)g_hash_table_size(q.history);
    MdHistoryEntry *entries = g_new0(MdHistoryEntry, MAX(*count, 1));
    GHashTableIter iter;
    gpointer value;
    int n = 0;
    g_hash_table_iter_init(&iter, q.history);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        const History *h = value;
        entries[n] = h->e;
        entries[n].topic = g_strdup(h->e.topic);
        entries[n].title = g_strdup(h->e.title);
        entries[n].path = g_strdup(h->e.path);
        entries[n].is_film = g_str_has_prefix(h->key, "film|");
        n++;
    }
    g_mutex_unlock(&q.lock);
    /* außerhalb der Sperre: die Platte kann schlafen */
    for (int i = 0; i < n; i++) entries[i].exists = file_size(entries[i].path) >= 0;
    qsort(entries, (size_t)n, sizeof *entries, (int (*)(const void *, const void *))history_compare);
    return entries;
}

void md_history_snapshot_free(MdHistoryEntry *entries, int count)
{
    for (int i = 0; i < count; i++) {
        g_free(entries[i].topic);
        g_free(entries[i].title);
        g_free(entries[i].path);
    }
    g_free(entries);
}

static gboolean history_has_path(gpointer key, gpointer value, gpointer path)
{
    (void)key;
    return !strcmp(((History *)value)->e.path, path);
}

void md_history_forget(const char *path)
{
    g_mutex_lock(&q.lock);
    if (g_hash_table_foreach_remove(q.history, history_has_path, (gpointer)path)) history_save();
    g_mutex_unlock(&q.lock);
}

/* Pfad ohne Endung: <Name>.mp4 und das fertige <Name>.mkv gehören zusammen */
static char *path_stem(const char *path)
{
    const char *slash = strrchr(path, G_DIR_SEPARATOR), *dot = strrchr(path, '.');
    return dot && (!slash || dot > slash) ? g_strndup(path, (gsize)(dot - path)) : g_strdup(path);
}

bool md_file_in_use(const char *path)
{
    char *stem = path_stem(path);
    bool used = false;
    g_mutex_lock(&q.lock);
    for (guint i = 0; i < q.jobs->len && !used; i++) {
        const Job *j = q.jobs->pdata[i];
        if (j->state == MD_JOB_DONE || j->state == MD_JOB_FAILED) continue;
        char *target = path_stem(j->target);
        used = !strcmp(target, stem);
        g_free(target);
    }
    g_mutex_unlock(&q.lock);
    g_free(stem);
    return used;
}

/* --- Speicherort abgleichen ----------------------------------------------- */
static const char *const VIDEO_SUFFIXES[] = {".mkv", ".mp4", ".webm", ".m4v"};
#define FOLDER_MAX_DEPTH 6

static void found_file_free(gpointer data)
{
    MdFoundFile *f = data;
    g_free(f->path);
    g_free(f->topic);
    g_free(f->title);
    g_free(f->key);
    g_free(f);
}

static void moved_file_free(gpointer data)
{
    MdMovedFile *m = data;
    g_free(m->from);
    g_free(m->to);
    g_free(m);
}

void md_folder_check_free(MdFolderCheck *check)
{
    if (!check) return;
    g_ptr_array_free(check->moved, TRUE);
    g_ptr_array_free(check->found, TRUE);
    g_ptr_array_free(check->missing, TRUE);
    g_free(check);
}

/* Videodateien unter dir sammeln; versteckte (Zusatzspuren) und halbe Dateien nicht */
static void collect_videos(const char *dir, int depth, GPtrArray *out)
{
    GDir *d = depth <= FOLDER_MAX_DEPTH ? g_dir_open(dir, 0, NULL) : NULL;
    const char *name;
    while (d && (name = g_dir_read_name(d))) {
        if (name[0] == '.') continue;
        char *path = g_build_filename(dir, name, NULL);
        if (g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
            /* Verweise nicht verfolgen: keine Schleifen, nichts außerhalb */
        } else if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            collect_videos(path, depth + 1, out);
        } else {
            for (size_t i = 0; i < G_N_ELEMENTS(VIDEO_SUFFIXES); i++)
                if (g_str_has_suffix(name, VIDEO_SUFFIXES[i])) {
                    g_ptr_array_add(out, g_strdup(path));
                    break;
                }
        }
        g_free(path);
    }
    if (d) g_dir_close(d);
}

/* Name ohne Ordner und Endung */
static char *file_stem(const char *path)
{
    char *base = g_path_get_basename(path), *stem = path_stem(base);
    g_free(base);
    return stem;
}

/* Aus Ordner und Name ablesen, was die Datei ist (so legt md_target_path sie an) */
static MdFoundFile *found_file_new(const char *path, const char *dir)
{
    static GRegex *episode, *dated;
    static gsize once;
    if (g_once_init_enter(&once)) {
        episode = g_regex_new("^(.+) - S(\\d+)E(\\d+) - (.*)$", 0, 0, NULL);
        dated = g_regex_new("^(\\d{4})-(\\d\\d)-(\\d\\d) - (.+)$", 0, 0, NULL);
        g_once_init_leave(&once, 1);
    }
    MdFoundFile *f = g_new0(MdFoundFile, 1);
    f->path = g_strdup(path);
    f->season = f->episode = -1;
    GStatBuf st;
    if (g_stat(path, &st) == 0) {
        f->size = st.st_size;
        f->modified = st.st_mtime;
    }
    char *stem = file_stem(path), *parent = g_path_get_dirname(path);
    char *folder = g_path_get_basename(parent);
    GMatchInfo *match = NULL;
    bool is_episode = g_regex_match(episode, stem, 0, &match);
    if (!is_episode) g_clear_pointer(&match, g_match_info_free);
    /* "<Sendung>/<Datum> - <Name>"; direkt im Speicherort wäre der Ordner keine Sendung */
    bool is_dated = !is_episode && strcmp(parent, dir) && g_regex_match(dated, stem, 0, &match);
    if (is_episode) {
        char *season = g_match_info_fetch(match, 2), *number = g_match_info_fetch(match, 3);
        char *name = g_match_info_fetch(match, 4);
        f->topic = g_match_info_fetch(match, 1);
        f->season = atoi(season);
        f->episode = atoi(number);
        f->title = g_strdup_printf("%s – S%02dE%02d – %s", f->topic, f->season, f->episode, name);
        char *topic = g_utf8_strdown(f->topic, -1);
        f->key = g_strdup_printf("%s|S%dE%d", topic, f->season, f->episode);
        g_free(topic);
        g_free(season);
        g_free(number);
        g_free(name);
    } else if (is_dated) {
        char *y = g_match_info_fetch(match, 1), *m = g_match_info_fetch(match, 2);
        char *d = g_match_info_fetch(match, 3);
        f->topic = g_strdup(folder);
        f->title = g_match_info_fetch(match, 4);
        GDateTime *day = g_date_time_new_utc(atoi(y), atoi(m), atoi(d), 12, 0, 0);
        char *topic = g_utf8_strdown(f->topic, -1), *name = g_utf8_strdown(f->title, -1);
        f->key = g_strdup_printf("%s|%s|%" G_GINT64_FORMAT, topic, name,
                                 day ? g_date_time_to_unix(day) / 86400 : 0);
        if (day) g_date_time_unref(day);
        g_free(topic);
        g_free(name);
        g_free(y);
        g_free(m);
        g_free(d);
    } else {
        /* "Filme/<Name>" und alles, was sich nicht zuordnen lässt: als Film */
        f->is_film = true;
        f->topic = g_strdup("Filme");
        f->title = g_strdup(stem);
        char *name = g_utf8_strdown(stem, -1);
        f->key = g_strconcat("film|", name, NULL);
        g_free(name);
    }
    g_match_info_free(match);
    g_free(stem);
    g_free(parent);
    g_free(folder);
    return f;
}

MdFolderCheck *md_check_folder(const char *dir)
{
    MdFolderCheck *check = g_new0(MdFolderCheck, 1);
    check->moved = g_ptr_array_new_with_free_func(moved_file_free);
    check->found = g_ptr_array_new_with_free_func(found_file_free);
    check->missing = g_ptr_array_new_with_free_func(g_free);
    /* Verlauf kopieren: Schlüssel und Pfad */
    GHashTable *by_path = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_mutex_lock(&q.lock);
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, q.history);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        const History *h = value;
        g_hash_table_replace(by_path, g_strdup(h->e.path), g_strdup(h->key));
    }
    g_mutex_unlock(&q.lock);

    /* fehlende Dateien des Verlaufs, nach Name und nach Schlüssel */
    GHashTable *gone_by_stem = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GHashTable *gone_by_key = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GHashTable *keys_present = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gpointer key;
    g_hash_table_iter_init(&iter, by_path);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        if (file_size(key) >= 0) {
            check->present++;
            g_hash_table_add(keys_present, g_strdup(value));
        } else {
            g_hash_table_replace(gone_by_stem, file_stem(key), g_strdup(key));
            g_hash_table_replace(gone_by_key, g_strdup(value), g_strdup(key));
        }
    }

    GPtrArray *videos = g_ptr_array_new_with_free_func(g_free);
    collect_videos(dir, 0, videos);
    GHashTable *claimed = g_hash_table_new(g_str_hash, g_str_equal);   /* schon zugeordnet */
    for (guint i = 0; i < videos->len; i++) {
        const char *path = videos->pdata[i];
        if (g_hash_table_contains(by_path, path)) continue;
        char *stem = file_stem(path);
        MdFoundFile *f = found_file_new(path, dir);
        const char *old = g_hash_table_lookup(gone_by_stem, stem);
        if (!old) old = g_hash_table_lookup(gone_by_key, f->key);
        if (old && g_hash_table_add(claimed, (char *)old)) {
            MdMovedFile *m = g_new0(MdMovedFile, 1);
            m->from = g_strdup(old);
            m->to = g_strdup(path);
            g_ptr_array_add(check->moved, m);
            found_file_free(f);
        } else if (old || g_hash_table_contains(keys_present, f->key)) {
            found_file_free(f);         /* zweite Kopie von etwas, das schon zählt */
        } else {
            g_ptr_array_add(check->found, f);
        }
        g_free(stem);
    }
    g_hash_table_iter_init(&iter, gone_by_stem);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        if (!g_hash_table_contains(claimed, value))
            g_ptr_array_add(check->missing, g_strdup(value));

    g_hash_table_destroy(claimed);
    g_ptr_array_free(videos, TRUE);
    g_hash_table_destroy(keys_present);
    g_hash_table_destroy(gone_by_key);
    g_hash_table_destroy(gone_by_stem);
    g_hash_table_destroy(by_path);
    return check;
}

void md_folder_check_apply(const MdFolderCheck *check, bool relink, bool add, bool forget)
{
    g_mutex_lock(&q.lock);
    GHashTableIter iter;
    gpointer value;
    for (guint i = 0; relink && i < check->moved->len; i++) {
        const MdMovedFile *m = check->moved->pdata[i];
        g_hash_table_iter_init(&iter, q.history);
        while (g_hash_table_iter_next(&iter, NULL, &value)) {
            History *h = value;
            if (strcmp(h->e.path, m->from)) continue;
            g_free(h->e.path);
            h->e.path = g_strdup(m->to);
            h->e.size = MAX(file_size(m->to), 0);
        }
    }
    for (guint i = 0; add && i < check->found->len; i++) {
        const MdFoundFile *f = check->found->pdata[i];
        if (g_hash_table_contains(q.history, f->key)) continue;
        History *h = g_new0(History, 1);
        h->key = g_strdup(f->key);
        h->e.topic = g_strdup(f->topic);
        h->e.title = g_strdup(f->title);
        h->e.path = g_strdup(f->path);
        h->e.season = f->season;
        h->e.episode = f->episode;
        h->e.size = f->size;
        h->e.finished = f->modified;
        g_hash_table_replace(q.history, h->key, h);
    }
    for (guint i = 0; forget && i < check->missing->len; i++)
        g_hash_table_foreach_remove(q.history, history_has_path, check->missing->pdata[i]);
    history_save();
    g_mutex_unlock(&q.lock);
}

MdStatus md_title_status(const MdTitle *t, const char *dir, char **path)
{
    char *target = md_target_path(t, dir), *key = title_key(t), *known = NULL;
    bool recorded = false;
    g_mutex_lock(&q.lock);
    /* Die Vorschau fragt Tausende Titel ab: offene Aufträge einmal verzeichnen statt jedes Mal
     * die ganze Warteschlange zu durchsuchen */
    if (!q.open_keys) {
        q.open_keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        for (guint i = 0; i < q.jobs->len; i++) {
            const Job *j = q.jobs->pdata[i];
            if (j->state != MD_JOB_DONE && j->state != MD_JOB_FAILED)
                g_hash_table_add(q.open_keys, g_strdup(j->key));
        }
    }
    bool queued = g_hash_table_contains(q.open_keys, key);
    const History *h = g_hash_table_lookup(q.history, key);
    if (h) {
        known = g_strdup(h->e.path);
        recorded = true;
    }
    g_mutex_unlock(&q.lock);

    char *result = result_path(target), *part = g_strconcat(target, ".part", NULL);
    const char *found = NULL;
    if (known && file_size(known) >= 0) found = known;
    else if (file_size(result) > 0) found = result;
    else if (file_size(target) > 0) found = target;

    MdStatus status = queued ? MD_STATUS_QUEUED
                    : found ? MD_STATUS_DONE
                    : file_size(part) >= 0 ? MD_STATUS_PARTIAL
                    : recorded ? MD_STATUS_GONE : MD_STATUS_MISSING;
    if (path) *path = g_strdup(found);
    g_free(target);
    g_free(key);
    g_free(known);
    g_free(result);
    g_free(part);
    return status;
}

/* ---------------------------------------------------------------------------
 * Titelbilder und Kategorien
 * ---------------------------------------------------------------------------
 * Die Suchdaten enthalten beides nicht, die Seite des Titels bei zdf.de schon: das Bild im
 * Feld "thumbnailUrl", die Kategorien im Block "meta-data-tags". Beides landet einmal geladen
 * im einstellbaren Bilderordner. Die Bildgröße steht je nach Server in der Adresse
 * (~1280x720) oder im Parameter (layout=1280x720).
 * ------------------------------------------------------------------------- */
/* Text hinter key bis zum Anführungszeichen; NULL = nicht gefunden */
static char *text_after(const char *page, const char *key)
{
    const char *start = strstr(page, key);
    if (!start) return NULL;
    start += strlen(key);
    const char *end = strchr(start, '"');
    return end ? g_strndup(start, (gsize)(end - start)) : NULL;
}

/* og:image, auch wenn content vor property steht */
static char *og_image(const char *page)
{
    char *url = text_after(page, "property=\"og:image\" content=\"");
    const char *found = url ? NULL : strstr(page, "\" property=\"og:image\"");
    if (!found) return url;
    const char *start = found;
    while (start > page && start[-1] != '"' && start[-1] != '<' && start[-1] != '>') start--;
    if (start - page < 9 || strncmp(start - 9, "content=\"", 9)) return NULL;
    return g_strndup(start, (gsize)(found - start));
}

/* Wert von name=… in einer Adresse durch value ersetzen */
static char *replace_parameter(const char *url, const char *name, const char *value)
{
    const char *found = strstr(url, name);
    if (!found) return g_strdup(url);
    const char *end = strchr(found, '&');
    return g_strdup_printf("%.*s%s%s%s", (int)(found - url), url, name, value, end ? end : "");
}

/* Bilder von ARTE, SRF, KiKA und der ARD-Mediathek in kleiner Größe; NULL = unbekannt */
static char *other_cover_url(const char *raw)
{
    char *url = g_strdup(raw), *amp;
    while ((amp = strstr(url, "&amp;"))) memmove(amp + 1, amp + 5, strlen(amp + 5) + 1);
    char *result = NULL;
    if (!url_allowed(url)) {
        /* keine Webadresse */
    } else if (g_str_has_prefix(url, COVER_ARTE_PREFIX)) {
        char *size = strstr(url, "/1920x1080");    /* ohne "?type=TEXT": kein Schriftzug */
        if (size) result = g_strdup_printf("%.*s/" COVER_SIZE, (int)(size - url), url);
    } else if (g_str_has_prefix(url, COVER_ARD_PAGE_PREFIX)) {
        result = replace_parameter(url, "imwidth=", COVER_WIDTH_TEXT);
    } else if (g_str_has_prefix(url, COVER_SRF_PREFIX)) {
        result = replace_parameter(url, "width=", COVER_WIDTH_TEXT);
    } else if (g_str_has_prefix(url, COVER_KIKA_PREFIX)) {
        result = g_strdup(url);
    }
    g_free(url);
    return result;
}

/* Adresse des Bildes in der kleinen Größe; NULL = die Seite nennt keines */
static char *cover_url(const char *page)
{
    char *url = text_after(page, "\"thumbnailUrl\":\""), *result = NULL;
    if (url && strchr(url, '\\')) {             /* SRF: JSON-maskiert, og:image ist sauber */
        g_free(url);
        url = NULL;
    }
    if (!url) url = og_image(page);
    if (!url) return NULL;
    char *tilde = strrchr(url, '~'), *layout = strstr(url, "layout=");
    if (strpbrk(url, " \\'")) {
        /* keine gültige Adresse */
    } else if (tilde && (g_str_has_prefix(url, COVER_URL_PREFIX) ||
                         g_str_has_prefix(url, COVER_3SAT_PREFIX))) {
        const char *query = strchr(tilde, '?');
        *tilde = 0;
        result = g_strconcat(url, "~" COVER_SIZE, query ? query : "", NULL);
    } else if (layout && g_str_has_prefix(url, COVER_EPG_PREFIX)) {
        const char *rest = strchr(layout, '&');
        *layout = 0;
        result = g_strconcat(url, "layout=" COVER_SIZE, rest ? rest : "", NULL);
    } else {
        result = other_cover_url(url);
    }
    g_free(url);
    return result;
}

/* Keine Kategorie, sondern Fassung, Altersfreigabe oder Sendereihe? */
static bool genre_tag(const char *href, const char *label)
{
    static const char *const SKIP[] = {"/filme", "/serien", "/untertitel", "/audiodeskription",
                                       "/deutsche-gebaerdensprache", "/livestream",
                                       "/altersfreigabe"};
    for (size_t i = 0; i < G_N_ELEMENTS(SKIP); i++)
        if (g_str_has_prefix(href, SKIP[i])) return false;
    if (*href != '/' || strchr(href + 1, '/') || !*label || strlen(label) > 40) return false;
    /* "/herzkino-196" ist eine Reihe, "/genre-10281" eine Kategorie */
    const char *dash = strrchr(href, '-');
    if (dash && g_ascii_isdigit(dash[1]) && !g_str_has_prefix(href, "/genre-")) return false;
    /* kleingeschrieben sind die Stimmungswörter wie "amüsant" */
    return g_utf8_validate(label, -1, NULL) && g_unichar_isupper(g_utf8_get_char(label)) &&
           !strpbrk(label, "\n<&");
}

/* Altersfreigabe und Kategorien der Seite: FSK in die erste Zeile, Kategorien dahinter */
static void page_genres(const char *page, GString *out)
{
    GString *genres = g_string_new(NULL), *fsk = g_string_new(NULL);
    const char *start = strstr(page, "data-testid=\"meta-data-tags\"");
    const char *end = start ? strstr(start, "</div>") : NULL;
    for (const char *p = start; end && (p = g_strstr_len(p, end - p, "href=\"")); ) {
        p += strlen("href=\"");
        const char *quote = strchr(p, '"'), *text = strchr(p, '>');
        const char *close = text ? g_strstr_len(text, end - text, "</a>") : NULL;
        if (!quote || !close) break;
        char *href = g_strndup(p, (gsize)(quote - p));
        char *label = g_strndup(text + 1, (gsize)(close - text - 1));
        if (g_str_has_prefix(href, "/altersfreigabe") && g_str_has_prefix(label, "FSK") &&
            !strpbrk(label, "\n<")) {
            g_string_assign(fsk, label);
        } else if (genre_tag(href, label)) {
            g_string_append_printf(genres, "%s\n", label);
        }
        g_free(href);
        g_free(label);
    }
    g_string_append_printf(out, "%s\n%s", fsk->str, genres->str);
    g_string_free(genres, TRUE);
    g_string_free(fsk, TRUE);
}

/* Fehlt dem Merkzettel etwas, das die Seite inzwischen haben könnte? */
static bool page_info_stale(const char *info, const char *path)
{
    char **lines = g_strsplit(info, "\n", 3);
    bool complete = g_strv_length(lines) == 3 && *lines[0] && *lines[2];
    g_strfreev(lines);
    GStatBuf stat_buf;
    if (complete) return false;
    if (!strcmp(info, PAGE_INFO_UNKNOWN)) return true;
    return g_stat(path, &stat_buf) != 0 ||
           time(NULL) - stat_buf.st_mtime >= PAGE_RECHECK_SECONDS;
}

/* ARD und 3sat nennen keine Kategorien: aus Stichwörtern im Text ableiten */
static void genres_from_text(const char *text, GString *out)
{
    static const struct {
        const char *word, *genre;
    } WORDS[] = {
        {"tatort", "Krimi"}, {"polizeiruf", "Krimi"}, {"krimi", "Krimi"}, {"thriller", "Thriller"},
        {"komödie", "Komödie"}, {"drama", "Drama"}, {"doku", "Dokumentation"},
        {"reportage", "Reportage"}, {"tagesschau", "Nachrichten"},
        {"tagesthemen", "Nachrichten"}, {"sportschau", "Sport"}, {"kabarett", "Kabarett"},
    };
    char *lower = g_utf8_strdown(text, -1);
    for (size_t i = 0; i < G_N_ELEMENTS(WORDS); i++) {
        char *line = g_strconcat(WORDS[i].genre, "\n", NULL);
        if (strstr(lower, WORDS[i].word) && !strstr(out->str, line))
            g_string_append(out, line);
        g_free(line);
    }
    g_free(lower);
}

/* Antwort der ARD-Schnittstelle zum Merkzettel machen */
static void ard_page_info(const char *reply, GString *out)
{
    const char *player = strstr(reply, "\"geoblocked\"");
    char *image = text_after(player ? player : reply, "\"src\":\"");
    char *rating = text_after(reply, "\"maturityContentRating\":\"");
    char *show = text_after(reply, "\"page_show\":\"");
    char *synopsis = text_after(player ? player : reply, "\"synopsis\":\"");
    char *width = image ? strstr(image, "{width}") : NULL;
    if (width && g_str_has_prefix(image, COVER_ARD_PREFIX) && !strpbrk(image, " \\'")) {
        *width = 0;
        g_string_append_printf(out, "%s384%s", image, width + strlen("{width}"));
    }
    g_string_append_c(out, '\n');
    if (rating && g_str_has_prefix(rating, "FSK") && g_ascii_isdigit(rating[3]))
        g_string_append_printf(out, "FSK %s", rating + 3);
    g_string_append_c(out, '\n');
    if (synopsis && strlen(synopsis) > GENRE_TEXT_LENGTH) synopsis[GENRE_TEXT_LENGTH] = 0;
    char *text = g_strconcat(show ? show : "", " ", synopsis ? synopsis : "", NULL);
    genres_from_text(text, out);
    g_free(text);
    g_free(image);
    g_free(rating);
    g_free(show);
    g_free(synopsis);
}

/* Merkzettel aus einer Seite von zdf.de oder 3sat.de */
static void html_page_info(const char *website, const char *reply, GString *out)
{
    char *url = cover_url(reply);
    g_string_assign(out, url ? url : "");
    g_string_append_c(out, '\n');
    g_free(url);
    page_genres(reply, out);
    if (g_str_has_prefix(website, COVER_3SAT_PAGE)) genres_from_text(website, out);
}

/* Bei der ARD liefert die Schnittstelle dieselben Angaben wie die Seite, nur verlässlicher */
static char *page_address(const char *website)
{
    if (g_str_has_prefix(website, COVER_ARD_PAGE)) {
        const char *id = strrchr(website, '/');
        return g_strconcat(ARD_ITEM_API, id + 1, "?devicetype=pc&embedded=false", NULL);
    }
    return g_strdup(website);
}

/* Merkzettel einer Seite, die es nicht mehr gibt. Die Adresse nennt bei 3sat die Kategorie. */
static char *page_gone_info(const char *website)
{
    GString *out = g_string_new("\n\n");
    if (g_str_has_prefix(website, COVER_3SAT_PAGE)) genres_from_text(website, out);
    if (out->len == 2) g_string_append_c(out, '\n');
    return g_string_free(out, FALSE);
}

/* Merkzettel zur Seite website: Bildadresse, FSK, danach je Zeile eine Kategorie.
 * Einmal geholt bleibt er liegen. NULL = Seite nicht erreichbar. */
static char *page_info_fetch(const char *website, bool *failed);

static char *page_info(const char *website)
{
    char *key = cache_file(website, ".info");
    bool failed = flight_begin(key);
    char *info = NULL;
    if (!failed) {
        info = page_info_fetch(website, &failed);
    } else {
        g_file_get_contents(key, &info, NULL, NULL);    /* ältere Angaben gelten weiter */
        failed = false;                                 /* nicht verlängern */
    }
    flight_end(key, failed);
    g_free(key);
    return info;
}

static char *page_info_fetch(const char *website, bool *failed)
{
    if (g_str_has_prefix(website, NETZKINO_PAGE) ||     /* schon mit der Filmliste gespeichert */
        g_str_has_prefix(website, NRK_PAGE)) {
        char *path = cache_file(website, ".info"), *saved = NULL;
        g_file_get_contents(path, &saved, NULL, NULL);
        g_free(path);
        return saved;
    }
    static const char *const PAGES[] = {COVER_PAGE_PREFIX, COVER_3SAT_PAGE, COVER_OTHER_PAGES};
    bool ard = g_str_has_prefix(website, COVER_ARD_PAGE), known = ard;
    for (size_t i = 0; !known && i < G_N_ELEMENTS(PAGES); i++)
        known = g_str_has_prefix(website, PAGES[i]);
    if (!known) return NULL;
    char *path = cache_file(website, ".info"), *info = NULL;
    if (g_file_get_contents(path, &info, NULL, NULL) && !page_info_stale(info, path)) {
        g_free(path);
        return info;
    }
    char *address = page_address(website);
    char *argv[] = {CURL_CMD, "-sS", "-f", "-L", "-m", "20", "--retry", "2", "-A", USER_AGENT,
                    "-w", "\n%{http_code}", "--url", address, NULL};
    char *reply = NULL, *messages = NULL;
    int rc = run_capture(argv, &reply, &messages);
    g_free(address);
    const char *code = reply ? strrchr(reply, '\n') : NULL;
    if (rc == CURL_HTTP_ERROR && code && (!strcmp(code, "\n404") || !strcmp(code, "\n410"))) {
        g_free(info);                   /* Seite ist weg: nicht bei jedem Start fragen */
        info = page_gone_info(website);
        g_file_set_contents(path, info, -1, NULL);
    } else if (rc != 0) {               /* vorübergehend: beim nächsten Start neu versuchen */
        md_log("Seite nicht geladen (curl %d%s): %s", rc, code ? code : "", website);
        *failed = true;
    } else if (reply) {
        GString *fresh = g_string_new(NULL);
        if (ard) ard_page_info(reply, fresh);
        else html_page_info(website, reply, fresh);
        if (strcmp(fresh->str, PAGE_INFO_UNKNOWN))
            g_file_set_contents(path, fresh->str, (gssize)fresh->len, NULL);
        g_free(info);
        info = g_string_free(fresh, FALSE);
    }
    g_free(reply);
    g_free(messages);
    g_free(path);
    return info;
}

/* Genres von IMDb und aus den Mediatheken auf Deutsch, jede nur einmal; NULL-Ziel = weglassen */
static void add_genre(GPtrArray *genres, const char *name)
{
    static const struct {
        const char *from, *to;
    } NAMES[] = {
        {"Spielfilm", NULL},    /* stand bis 1.3.0 im Zwischenspeicher, sagt nichts über den Film */
        {"Short", NULL}, {"Adult", NULL},
        {"Comedy", "Komödie"}, {"Romance", "Romanze"}, {"Crime", "Krimi"},
        {"Documentary", "Dokumentation"}, {"Family", "Familie"}, {"Adventure", "Abenteuer"},
        {"Sci-Fi", "Science-Fiction"}, {"Science Fiction", "Science-Fiction"},
        {"Biography", "Biografie"}, {"History", "Geschichte"}, {"War", "Krieg"},
        {"Music", "Musik"}, {"News", "Nachrichten"}, {"Talk-Show", "Talk"},
        {"Game-Show", "Show"}, {"Reality-TV", "Show"}, {"Film-Noir", "Film Noir"},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(NAMES); i++) {
        if (strcmp(name, NAMES[i].from)) continue;
        name = NAMES[i].to;
        break;
    }
    if (!name) return;
    for (guint i = 0; i < genres->len; i++)
        if (!strcmp(genres->pdata[i], name)) return;
    g_ptr_array_add(genres, g_strdup(name));
}

MdPageInfo *md_page_info(const char *website)
{
    char *info = page_info(website);
    if (!info) return NULL;
    char **lines = g_strsplit(info, "\n", -1);
    MdPageInfo *result = g_new0(MdPageInfo, 1);
    GPtrArray *genres = g_ptr_array_new();
    for (int i = 0; lines[i]; i++) {
        if (i == 1 && *lines[i]) result->fsk = g_strdup(lines[i]);
        if (i >= 2 && *lines[i]) add_genre(genres, lines[i]);
    }
    g_ptr_array_add(genres, NULL);
    result->genres = (char **)g_ptr_array_free(genres, FALSE);
    g_strfreev(lines);
    g_free(info);
    return result;
}

void md_page_info_free(MdPageInfo *info)
{
    if (!info) return;
    g_free(info->fsk);
    g_strfreev(info->genres);
    g_free(info);
}

char *md_video_frame(const char *path)
{
    GStatBuf st;
    if (g_stat(path, &st) != 0) return NULL;
    /* Größe und Änderungszeit im Schlüssel: eine ersetzte Datei bekommt ein neues Bild */
    char *key = g_strdup_printf("%s|%" G_GINT64_FORMAT "|%" G_GINT64_FORMAT, path,
                                (gint64)st.st_size, (gint64)st.st_mtime);
    char *frame = cache_file(key, ".bild.jpg");
    g_free(key);
    /* kurze Dateien haben nach drei Minuten kein Bild mehr */
    const char *const offsets[] = {COVER_FRAME_SECONDS, "20", "0"};
    for (size_t i = 0; i < G_N_ELEMENTS(offsets) && file_size(frame) <= 0; i++) {
        char *tmp = g_strconcat(frame, ".part", NULL);
        char *argv[] = {"ffmpeg", "-nostdin", "-v", "error", "-ss", (char *)offsets[i],
                        "-i", (char *)path, "-frames:v", "1", "-vf",
                        "scale=" COVER_FRAME_WIDTH ":-2", "-q:v", "4", "-f", "image2", "-y",
                        tmp, NULL};
        if (run_capture(argv, NULL, NULL) != 0 || file_size(tmp) <= 0 || g_rename(tmp, frame) != 0)
            g_unlink(tmp);
        g_free(tmp);
    }
    if (file_size(frame) > 0) return frame;
    g_free(frame);
    return NULL;
}


/* Die Seite nennt kein Bild: Standbild aus dem Video holen. Nicht bei Streams: ffmpeg lädt dort
 * jedes Teilstück bis zur Sprungmarke und belegt minutenlang einen der Bild-Threads. */
static void cover_from_video(const char *video, const char *path)
{
    if (!url_allowed(video) || is_hls(video)) return;
    char *tmp = g_strconcat(path, ".part", NULL);
    char *argv[] = {"ffmpeg", "-nostdin", "-v", "error", "-rw_timeout", "20000000",
                    "-ss", COVER_FRAME_SECONDS, "-i", (char *)video, "-frames:v", "1",
                    "-vf", "scale=" COVER_FRAME_WIDTH ":-2", "-q:v", "4", "-f", "image2", "-y",
                    tmp, NULL};
    if (run_capture(argv, NULL, NULL) != 0 || file_size(tmp) <= 0 || g_rename(tmp, path) != 0)
        g_unlink(tmp);
    g_free(tmp);
}

/* Merker "kein Bild": Ohne ihn fragte jeder Aufbau einer Übersicht erneut im Netz nach, für
 * Titel ohne Bild per ffmpeg mit bis zu 20 s Wartezeit, und hielt die Bild-Threads auf. */
static bool no_cover_marked(const char *marker)
{
    GStatBuf st;
    if (g_stat(marker, &st) != 0) return false;
    /* leerer Merker: Seite ohne Bild; sonst kam keine Antwort, das kann am Netz gelegen haben */
    return time(NULL) - st.st_mtime < (st.st_size ? NO_COVER_RETRY_SECONDS
                                                  : PAGE_RECHECK_SECONDS);
}

static char *cover_file(const char *website, const char *video);

char *md_cover_file(const char *website, const char *video)
{
    char *key = cache_file(website, ".jpg");
    if (file_size(key) > 0) return key;
    flight_begin(key);          /* Fehlschläge merkt sich der Merker .keinbild */
    char *path = cover_file(website, video);
    flight_end(key, false);
    g_free(key);
    return path;
}

static char *cover_file(const char *website, const char *video)
{
    char *path = cache_file(website, ".jpg");
    if (file_size(path) > 0) return path;
    char *marker = cache_file(website, ".keinbild");
    if (no_cover_marked(marker)) {
        g_free(marker);
        g_free(path);
        return NULL;
    }
    char *info = page_info(website);
    if (info && *info != '\n') {
        *strchr(info, '\n') = 0;
        char *tmp = g_strconcat(path, ".part", NULL), *messages = NULL;
        char *argv[] = {CURL_CMD, "-sS", "-f", "-L", "-m", "20", "-A", USER_AGENT, "-o", tmp,
                        "--url", info, NULL};
        if (run_capture(argv, NULL, &messages) != 0 || g_rename(tmp, path) != 0) g_unlink(tmp);
        g_free(messages);
        g_free(tmp);
    }
    if (file_size(path) <= 0 && video && *video) cover_from_video(video, path);
    bool found = file_size(path) > 0;
    if (found) g_unlink(marker);
    else g_file_set_contents(marker, info ? "" : "?", -1, NULL);
    g_free(info);
    g_free(marker);
    if (found) return path;
    g_free(path);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * IMDb
 * ---------------------------------------------------------------------------
 * Die Vorschlagsliste von IMDb führt direkt zur Titelseite. Sie ist nur ein Wegweiser: Passt
 * kein Treffer sicher, öffnet sich die IMDb-Suche, damit nie ein falscher Titel erscheint.
 * Bewertungen stehen in der offiziellen Datendatei von IMDb, die wöchentlich neu geladen
 * und dann lokal durchsucht wird; die Titelseiten selbst weisen Programme ab.
 * ------------------------------------------------------------------------- */
/* Wert von "key":" im Bereich start..end */
static char *json_text_between(const char *start, const char *end, const char *key)
{
    const char *value = g_strstr_len(start, end - start, key);
    if (!value) return NULL;
    value += strlen(key);
    const char *close = g_strstr_len(value, end - value, "\"");
    return close ? g_strndup(value, (gsize)(close - value)) : NULL;
}

/* Gehört der Treffer label zum Titel name? Ein Titel wie "Wind River - Tod im Schnee" passt zu
 * "Wind River", aber nur am Anfang und bis zu einem Wortende. */
static bool imdb_label_fits(const char *label, const char *name)
{
    char *a = g_utf8_strdown(label, -1), *b = g_utf8_strdown(name, -1);
    size_t length = strlen(a);
    bool fits = length >= 3 && !strncmp(a, b, length) && (!b[length] || strchr(" :-", b[length]));
    g_free(a);
    g_free(b);
    return fits;
}

/* Titelkennung aus der Vorschlagsliste; NULL = kein sicherer Treffer. year darf NULL sein. */
static char *imdb_pick(const char *reply, const char *name, bool is_film, int *year)
{
    static const char KEY[] = "\"id\":\"tt";
    char *result = NULL;
    for (const char *p = strstr(reply, KEY); p && !result;) {
        const char *next = strstr(p + 1, KEY);
        const char *end = next ? next : p + strlen(p);
        char *id = json_text_between(p, end, "\"id\":\"");
        char *label = json_text_between(p, end, "\"l\":\"");
        char *kind = json_text_between(p, end, "\"qid\":\"");
        bool type_fits = kind && (is_film ? !strcmp(kind, "movie") || !strcmp(kind, "tvMovie")
                                          : !strcmp(kind, "tvSeries") ||
                                            !strcmp(kind, "tvMiniSeries"));
        if (id && label && type_fits && imdb_label_fits(label, name) &&
            strspn(id, "t0123456789") == strlen(id)) {
            result = g_strdup(id);
            const char *y = g_strstr_len(p, end - p, "\"y\":");
            if (y && year) *year = atoi(y + strlen("\"y\":"));
        }
        g_free(id);
        g_free(label);
        g_free(kind);
        p = next;
    }
    return result;
}

/* Gespeicherte Zuordnung lesen: "tt5362988 2017", "-" = kein Treffer. true = gültig */
static bool imdb_cache_read(const char *path, char **id, int *year)
{
    GStatBuf stat_buf;
    char *text = NULL;
    if (g_stat(path, &stat_buf) != 0 || !g_file_get_contents(path, &text, NULL, NULL))
        return false;
    bool found = g_str_has_prefix(text, "tt");
    if (!found && time(NULL) - stat_buf.st_mtime >= PAGE_RECHECK_SECONDS) {
        g_free(text);
        return false;
    }
    if (found) {
        char *space = strchr(text, ' ');
        if (space && year) *year = atoi(space + 1);
        if (space) *space = 0;
        *id = g_strdup(text);
    }
    g_free(text);
    return true;
}

/* Titel bei IMDb nachschlagen -> Kennung wie "tt5362988", NULL = kein sicherer Treffer */
static char *imdb_lookup(const char *name, bool is_film, int *year)
{
    char *key = g_strconcat(is_film ? "film:" : "serie:", name, NULL);
    char *saved = cache_file(key, ".imdb");
    g_free(key);
    char *known = NULL;
    if (imdb_cache_read(saved, &known, year)) {
        g_free(saved);
        return known;
    }
    /* Vorschau, Bewertung und "Meine Videos" fragen oft gleichzeitig nach demselben Titel */
    bool failed_before = flight_begin(saved);
    if (failed_before || imdb_cache_read(saved, &known, year)) {
        flight_end(saved, failed_before);
        g_free(saved);
        return known;
    }
    /* Untertitel wie "- Tod im Schnee" kennt IMDb meist nicht */
    char *query = g_strdup(name);
    char *cut = strstr(query, " - ");
    if (cut) *cut = 0;
    if ((cut = strstr(query, ": "))) *cut = 0;
    char *lower = g_utf8_strdown(query, -1);
    char *escaped = g_uri_escape_string(lower, NULL, FALSE);
    char *api = g_strdup_printf(IMDB_API "%s.json?includeVideos=0", escaped);
    char *argv[] = {CURL_CMD, "-sS", "-f", "-L", "-m", "15", "-A", USER_AGENT, api, NULL};
    char *reply = NULL, *messages = NULL, *id = NULL;
    bool answered = run_capture(argv, &reply, &messages) == 0 && reply;
    if (answered) {
        id = imdb_pick(reply, name, is_film, year);
        char *line = id ? g_strdup_printf("%s %d", id, year ? *year : 0) : g_strdup("-");
        g_file_set_contents(saved, line, -1, NULL);
        g_free(line);
    }
    flight_end(saved, !answered);
    g_free(saved);
    g_free(query);
    g_free(lower);
    g_free(escaped);
    g_free(api);
    g_free(reply);
    g_free(messages);
    return id;
}

char *md_imdb_url(const char *name, bool is_film)
{
    char *id = imdb_lookup(name, is_film, NULL);
    char *full = g_uri_escape_string(name, NULL, FALSE);
    char *url = id ? g_strdup_printf("https://www.imdb.com/title/%s/", id)
                   : g_strdup_printf("https://www.imdb.com/find/?q=%s", full);
    md_log("IMDb: %s -> %s", name, url);
    g_free(id);
    g_free(full);
    return url;
}

/* Datendatei der Bewertungen laden, wenn sie fehlt oder alt ist -> Pfad */
static char *imdb_ratings_file(void)
{
    static GMutex lock;
    char *dir = cache_dir();
    char *path = g_build_filename(dir, "imdb-bewertungen.tsv", NULL);
    char *packed = g_strconcat(path, ".part.gz", NULL), *plain = g_strconcat(path, ".part", NULL);
    GStatBuf stat_buf;
    g_mutex_lock(&lock);
    if (g_stat(path, &stat_buf) != 0 || time(NULL) - stat_buf.st_mtime >= IMDB_RATINGS_SECONDS) {
        char *fetch[] = {CURL_CMD, "-sS", "-f", "-L", "-m", "120", "-A", USER_AGENT, "-o", packed,
                         IMDB_RATINGS_URL, NULL};
        char *unpack[] = {"gzip", "-df", packed, NULL};
        char *messages = NULL, *more = NULL;
        if (run_capture(fetch, NULL, &messages) == 0 && run_capture(unpack, NULL, &more) == 0)
            g_rename(plain, path);
        else md_log("IMDb-Bewertungen nicht geladen");
        g_unlink(packed);
        g_unlink(plain);
        g_free(messages);
        g_free(more);
    }
    g_mutex_unlock(&lock);
    g_free(dir);
    g_free(packed);
    g_free(plain);
    return path;
}

/* Datendatei der Titel laden und auf Kennung und Genres eindampfen -> Pfad. Sie ist groß und
 * ändert sich kaum, darum nur selten. Scheitert es, bleibt es für diesen Programmlauf dabei. */
static char *imdb_genres_file(void)
{
    static GMutex lock;
    static bool failed;
    static const char SCRIPT[] =
        "gzip -dc \"$1\" | grep -E '^tt[0-9]+\\s(movie|tvMovie|tvSeries|tvMiniSeries)\\s' "
        "| cut -f1,9 | grep -v '\\\\N$' > \"$2\"";
    char *dir = cache_dir();
    char *path = g_build_filename(dir, "imdb-kategorien.tsv", NULL);
    char *packed = g_strconcat(path, ".part.gz", NULL), *plain = g_strconcat(path, ".part", NULL);
    GStatBuf stat_buf;
    g_mutex_lock(&lock);
    if (!failed &&
        (g_stat(path, &stat_buf) != 0 || time(NULL) - stat_buf.st_mtime >= IMDB_GENRES_SECONDS)) {
        char *fetch[] = {CURL_CMD, "-sS", "-f", "-L", "-m", "1800", "-A", USER_AGENT, "-o", packed,
                         IMDB_GENRES_URL, NULL};
        char *unpack[] = {"sh", "-c", (char *)SCRIPT, "sh", packed, plain, NULL};
        char *messages = NULL, *more = NULL;
        failed = run_capture(fetch, NULL, &messages) != 0 ||
                 run_capture(unpack, NULL, &more) != 0 || file_size(plain) < IMDB_GENRES_MIN_SIZE ||
                 g_rename(plain, path) != 0;
        if (failed) md_log("IMDb-Kategorien nicht geladen");
        g_unlink(packed);
        g_unlink(plain);
        g_free(messages);
        g_free(more);
    }
    g_mutex_unlock(&lock);
    g_free(dir);
    g_free(packed);
    g_free(plain);
    return path;
}

char **md_imdb_genres(const char *name, bool is_film)
{
    int year = 0;
    char *id = imdb_lookup(name, is_film, &year);
    if (!id) return NULL;
    char *path = imdb_genres_file();
    GMappedFile *file = g_mapped_file_new(path, FALSE, NULL);
    g_free(path);
    GPtrArray *genres = g_ptr_array_new();
    const char *text = file ? g_mapped_file_get_contents(file) : NULL;
    gsize length = file ? g_mapped_file_get_length(file) : 0;
    char *key = g_strdup_printf("%s\t", id);       /* Zeile "tt5362988<Tab>Crime,Drama" */
    for (const char *p = text; p && p < text + length;) {
        const char *hit = g_strstr_len(p, text + length - p, key);
        if (!hit) break;
        p = hit + 1;
        if (hit != text && hit[-1] != '\n') continue;
        const char *start = hit + strlen(key);
        const char *end = memchr(start, '\n', (gsize)(text + length - start));
        char *line = g_strndup(start, end ? (gsize)(end - start) : 0);
        char **names = g_strsplit(line, ",", -1);
        for (char **n = names; *n; n++) add_genre(genres, *n);
        g_strfreev(names);
        g_free(line);
        break;
    }
    g_ptr_array_add(genres, NULL);
    g_free(key);
    g_free(id);
    if (file) g_mapped_file_unref(file);
    return (char **)g_ptr_array_free(genres, FALSE);
}

/* Zeile "tt5362988<Tab>7.7<Tab>322319" suchen */
static void imdb_read_rating(const char *id, MdImdbInfo *info)
{
    char *path = imdb_ratings_file();
    GMappedFile *file = g_mapped_file_new(path, FALSE, NULL);
    g_free(path);
    if (!file) return;
    const char *text = g_mapped_file_get_contents(file);
    gsize length = g_mapped_file_get_length(file);
    char *key = g_strdup_printf("\n%s\t", id);
    const char *line = text ? g_strstr_len(text, (gssize)length, key) : NULL;
    if (line && line + strlen(key) + 16 < text + length) {
        char *end = NULL;
        info->rating = g_ascii_strtod(line + strlen(key), &end);
        if (end && *end == '\t') info->votes = atoi(end + 1);
    }
    g_free(key);
    g_mapped_file_unref(file);
}

bool md_imdb_info(const char *name, bool is_film, MdImdbInfo *info)
{
    memset(info, 0, sizeof(*info));
    char *id = imdb_lookup(name, is_film, &info->year);
    if (!id) return false;
    imdb_read_rating(id, info);
    g_free(id);
    return true;
}

/* ---------------------------------------------------------------------------
 * Warteschlange: Speichern und Laden
 * ------------------------------------------------------------------------- */
/* Zusatzspuren für die Warteschlange in eine Zeile packen */
static char *extras_to_text(const Job *j)
{
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < j->extras->len; i++) {
        const Extra *e = j->extras->pdata[i];
        g_string_append_printf(s, "%s%d\x1f%s\x1f%s\x1f%s", i ? "\x1e" : "", e->subtitle, e->label,
                               e->lang, e->url);
    }
    return g_string_free(s, FALSE);
}

static void extra_free(gpointer data)
{
    Extra *e = data;
    g_free(e->label);
    g_free(e->lang);
    g_free(e->url);
    g_free(e);
}

/* Zusatzspuren aus der Warteschlange lesen */
static void extras_from_text(Job *j, const char *text)
{
    char **records = g_strsplit(text, "\x1e", -1);
    for (int i = 0; records[i]; i++) {
        char **f = g_strsplit(records[i], "\x1f", -1);
        if (g_strv_length(f) == 4) {
            Extra *e = g_new0(Extra, 1);
            e->subtitle = atoi(f[0]);
            e->label = g_strdup(f[1]);
            e->lang = g_strdup(f[2]);
            e->url = g_strdup(f[3]);
            g_ptr_array_add(j->extras, e);
        }
        g_strfreev(f);
    }
    g_strfreev(records);
}

static char *queue_path(void)
{
    return g_build_filename(md_app_dir(), "warteschlange.tsv", NULL);
}

/* Warteschlange als Text (q.lock gehalten) */
static GString *queue_text(void)
{
    GString *out = g_string_new(QUEUE_HEADER "\n");
    for (guint i = 0; i < q.jobs->len; i++) {
        Job *j = q.jobs->pdata[i];
        if (j->remove_requested) continue;
        /* unterbrochene Arbeit gilt beim nächsten Start als wartend */
        MdJobState state = j->state == MD_JOB_DOWNLOADING ? MD_JOB_WAITING
                         : j->state == MD_JOB_CONVERTING ? MD_JOB_CONVERT_WAITING : j->state;
        char *title = g_strescape(j->title, NULL), *url = g_strescape(j->url, NULL);
        char *target = g_strescape(j->target, NULL);
        char *message = g_strescape(j->message ? j->message : "", NULL);
        char *extras = extras_to_text(j), *extras_text = g_strescape(extras, NULL);
        char *label = g_strescape(j->main_label, NULL), *lang = g_strescape(j->main_lang, NULL);
        char *key = g_strescape(j->key, NULL), *topic = g_strescape(j->topic, NULL);
        g_string_append_printf(out, "%d\t%d\t%d\t%d\t%d\t%d\t%" G_GINT64_FORMAT
                               "\t%s\t%s\t%s\t%s\t%d\t%s\t%s\t%s\t%s\t%s\t%d\t%d\t%d\n",
                               state, j->convert, j->keep_original, j->encoder, j->gpu_quality,
                               j->cpu_rf, j->total, title, url, target, message, j->skip_main_audio,
                               label, lang, extras_text, key, topic, j->season, j->episode,
                               j->quality);
        g_free(key);
        g_free(topic);
        g_free(extras);
        g_free(extras_text);
        g_free(label);
        g_free(lang);
        g_free(title);
        g_free(url);
        g_free(target);
        g_free(message);
    }
    return out;
}

/* Text schreiben (ohne q.lock: bei Tausenden Aufträgen dauert das, samt fsync) */
static void queue_write(GString *text)
{
    char *path = queue_path();
    g_file_set_contents(path, text->str, text->len, NULL);
    g_free(path);
    g_string_free(text, TRUE);
}

/* Warteschlange sichern lassen (q.lock gehalten). Der Verteiler schreibt sie, so blockiert
 * weder die Oberfläche noch ein Worker, und viele Änderungen hintereinander ergeben eine Datei. */
static void queue_save(void)
{
    if (!q.persist) return;
    q.save_due = true;
    g_cond_signal(&q.wake);
}

static void job_free(Job *j)
{
    g_free(j->title);
    g_free(j->url);
    g_free(j->target);
    g_free(j->message);
    g_free(j->main_label);
    g_free(j->main_lang);
    g_free(j->key);
    g_free(j->topic);
    g_ptr_array_free(j->extras, TRUE);
    g_free(j);
}

/* Warteschlange vom letzten Mal laden; offene Aufträge starten pausiert */
static void queue_load(void)
{
    char *path = queue_path(), *text = NULL;
    bool found = g_file_get_contents(path, &text, NULL, NULL);
    g_free(path);
    if (!found) return;
    char **lines = g_strsplit(text, "\n", -1);
    g_free(text);
    if (!lines[0] ||
        (strcmp(lines[0], QUEUE_HEADER) && strcmp(lines[0], QUEUE_HEADER_OLD))) {
        g_strfreev(lines);
        return;
    }
    for (int i = 1; lines[i]; i++) {
        char **f = g_strsplit(lines[i], "\t", -1);
        guint fields = g_strv_length(f);
        if (fields == 19 || fields == 20) {    /* bis 1.7.0 ohne Qualität */
            Job *j = g_new0(Job, 1);
            j->id = ++q.next_id;
            j->state = (MdJobState)CLAMP(atoi(f[0]), MD_JOB_WAITING, MD_JOB_FAILED);
            j->convert = atoi(f[1]);
            j->keep_original = atoi(f[2]);
            j->encoder = atoi(f[3]) == MD_ENCODER_CPU ? MD_ENCODER_CPU : MD_ENCODER_GPU;
            j->gpu_quality = atoi(f[4]);
            j->cpu_rf = atoi(f[5]);
            j->total = g_ascii_strtoll(f[6], NULL, 10);
            j->title = g_strcompress(f[7]);
            j->url = g_strcompress(f[8]);
            j->target = g_strcompress(f[9]);
            j->message = *f[10] ? g_strcompress(f[10]) : NULL;
            j->skip_main_audio = atoi(f[11]);
            j->main_label = g_strcompress(f[12]);
            j->main_lang = g_strcompress(f[13]);
            j->key = g_strcompress(f[15]);
            j->topic = g_strcompress(f[16]);
            j->season = atoi(f[17]);
            j->episode = atoi(f[18]);
            j->quality = fields == 20 ? (MdQuality)CLAMP(atoi(f[19]), MD_QUALITY_HIGH,
                                                          MD_QUALITY_LOW) : MD_QUALITY_HIGH;
            j->extras = g_ptr_array_new_with_free_func(extra_free);
            char *extras = g_strcompress(f[14]);
            extras_from_text(j, extras);
            g_free(extras);
            j->paused = j->state == MD_JOB_WAITING || j->state == MD_JOB_CONVERT_WAITING;
            j->remaining = -1;
            char *part = g_strconcat(j->target, ".part", NULL);
            j->have = MAX(file_size(part), 0);
            g_free(part);
            if (j->state == MD_JOB_WAITING && j->total > 0)
                j->progress = (double)j->have / j->total;
            g_ptr_array_add(q.jobs, j);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
}

/* Auftrag nach Nummer suchen (q.lock gehalten) */
static Job *job_by_id(guint id)
{
    for (guint i = 0; i < q.jobs->len; i++) {
        Job *j = q.jobs->pdata[i];
        if (j->id == id) return j;
    }
    return NULL;
}

/* Auftrag ohne Download: eine schon geladene Datei nachträglich nach AV1 (md_queue_convert) */
static bool is_reconvert(const Job *j)
{
    return !*j->url;
}

/* Muss nach dem Download noch ffmpeg ran? */
static bool needs_processing(const Job *j)
{
    return j->convert || j->extras->len > 0 || j->skip_main_audio;
}

/* Dateien der Zusatzspuren löschen */
static void remove_extra_files(const Job *j)
{
    for (guint i = 0; i < j->extras->len; i++) {
        const Extra *e = j->extras->pdata[i];
        char *path = extra_path(j->target, i, e->subtitle);
        char *part = g_strconcat(path, ".part", NULL);
        g_unlink(path);
        g_unlink(part);
        g_free(path);
        g_free(part);
    }
}

/* halbfertige Dateien eines Auftrags löschen */
static void remove_partial_files(const Job *j)
{
    char *part = g_strconcat(j->target, ".part", NULL), *tmp = temp_path(j->target);
    g_unlink(part);
    g_unlink(tmp);
    remove_extra_files(j);
    g_free(part);
    g_free(tmp);
}

static gint64 file_bytes(const char *path)
{
    GStatBuf st;
    return g_stat(path, &st) == 0 ? (gint64)st.st_size : 0;
}

/* Größe der Dateien, die remove_partial_files löschen würde */
static gint64 partial_bytes(const Job *j)
{
    char *part = g_strconcat(j->target, ".part", NULL), *tmp = temp_path(j->target);
    gint64 bytes = file_bytes(part) + file_bytes(tmp);
    for (guint i = 0; i < j->extras->len; i++) {
        const Extra *e = j->extras->pdata[i];
        char *path = extra_path(j->target, i, e->subtitle);
        char *extra_part = g_strconcat(path, ".part", NULL);
        bytes += file_bytes(path) + file_bytes(extra_part);
        g_free(path);
        g_free(extra_part);
    }
    g_free(part);
    g_free(tmp);
    return bytes;
}

/* Zustand setzen (q.lock gehalten); das Verzeichnis offener Aufträge gilt danach nicht mehr */
static void job_set_state(Job *j, MdJobState state)
{
    j->state = state;
    g_clear_pointer(&q.open_keys, g_hash_table_destroy);
}

/* Auftrag aus der Liste nehmen und freigeben (q.lock gehalten, kein Worker aktiv) */
static void job_discard(Job *j)
{
    g_ptr_array_remove(q.jobs, j);
    g_clear_pointer(&q.open_keys, g_hash_table_destroy);
    job_free(j);
}

/* Worker meldet sich ab (q.lock gehalten) */
static void worker_finish(Job *j)
{
    j->worker = false;
    j->speed = 0;
    j->remaining = -1;
    if (j->remove_requested) {
        remove_partial_files(j);
        job_discard(j);
    }
    q.workers--;
    queue_save();
    g_cond_broadcast(&q.worker_done);
    g_cond_signal(&q.wake);
}

/* Auftrag als fehlgeschlagen markieren (q.lock gehalten) */
static void job_fail(Job *j, const char *message)
{
    md_log("FEHLER %s: %s", j->title, message);
    job_set_state(j, MD_JOB_FAILED);
    g_free(j->message);
    j->message = g_strdup(message);
}

/* soll der Worker aufhören? (q.lock gehalten). Angehaltene Downloads enden wie pausierte und
 * laden später an derselben Stelle weiter. */
static bool job_interrupted(const Job *j)
{
    return j->paused || j->remove_requested || q.quitting ||
           (q.downloads_held && j->state == MD_JOB_DOWNLOADING);
}

/* ---------------------------------------------------------------------------
 * Download
 * ------------------------------------------------------------------------- */
/* Tempolimit je Download in Bytes/s, 0 = unbegrenzt (q.lock gehalten) */
static gint64 rate_per_download(void)
{
    if (q.rate_limit <= 0) return 0;
    int active = 0;
    for (guint i = 0; i < q.jobs->len; i++) {
        Job *j = q.jobs->pdata[i];
        if (j->state == MD_JOB_DOWNLOADING && !j->paused) active++;
    }
    return MAX(q.rate_limit / MAX(active, 1), 16 * 1024);
}

/* Weicht das Limit so stark ab, dass curl neu starten muss? */
static bool rate_changed(gint64 running, gint64 wanted)
{
    if (running == wanted) return false;
    if (!running || !wanted) return true;
    return llabs(running - wanted) > running * RATE_TOLERANCE;
}

/* Größe der Datei beim Server erfragen -> Bytes, 0 = Fehler (*error gesetzt) */
static gint64 remote_size(const char *url, char **error)
{
    char *argv[] = {CURL_CMD, "-sSIL", "-m", "30", "-A", USER_AGENT, "--url", (char *)url, NULL};
    char *out = NULL, *messages = NULL;
    int rc = run_capture(argv, &out, &messages);
    int status = 0;
    gint64 size = 0;
    char **lines = g_strsplit(out ? out : "", "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (!strncmp(lines[i], "HTTP/", 5)) {
            const char *space = strchr(lines[i], ' ');
            status = space ? atoi(space + 1) : 0;
            size = 0;                               /* Weiterleitung: letzter Block zählt */
        } else if (!g_ascii_strncasecmp(lines[i], "content-length:", 15)) {
            size = g_ascii_strtoll(lines[i] + 15, NULL, 10);
        }
    }
    g_strfreev(lines);
    if (rc != 0 || status == 0)
        *error = g_strdup(UNREACHABLE_TEXT);
    else if (status == 403)
        *error = g_strdup("Zugriff verweigert – das Video ist nur aus Deutschland abrufbar.");
    else if (status == 404 || status == 410)
        *error = g_strdup("Das Video ist nicht mehr in der Mediathek. Erneut versuchen hilft "
                          "nicht, bitte aus der Liste entfernen.");
    else if (status != 200 || size <= 0)
        *error = g_strdup_printf("Unerwartete Antwort des Servers (HTTP %d).", status);
    g_free(out);
    g_free(messages);
    return *error ? 0 : size;
}

/* curl für die .part-Datei starten -> Prozessnummer, 0 = Fehler */
static GPid start_curl(const char *url, const char *part, gint64 rate)
{
    char *rate_text = g_strdup_printf("%" G_GINT64_FORMAT, rate);
    char *argv[32];
    int n = 0;
    char *fixed[] = {CURL_CMD, "-sS", "-f", "-L", "-C", "-", "--connect-timeout", "20",
                     "--speed-limit", "1024", "--speed-time", "60", "-A", USER_AGENT, NULL};
    for (int i = 0; fixed[i]; i++) argv[n++] = fixed[i];
    if (rate > 0) {
        argv[n++] = "--limit-rate";
        argv[n++] = rate_text;
    }
    argv[n++] = "-o";
    argv[n++] = (char *)part;
    argv[n++] = "--url";
    argv[n++] = (char *)url;
    argv[n] = NULL;
    GPid pid = 0;
    bool ok = g_spawn_async(NULL, argv, NULL,
                            G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD
                            | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                            child_setup, NULL, &pid, NULL);
    g_free(rate_text);
    if (ok) adopt_child(pid, false);
    return ok ? pid : 0;
}

/* Warten, bis curl endet oder unterbrochen wird; Fortschritt nachführen. base = Bytes
 * der schon fertigen Dateien des Auftrags.
 * -> Exit-Code von curl, -1 = unterbrochen, -2 = Tempolimit geändert */
static int watch_curl(Job *j, GPid pid, const char *part, gint64 rate, gint64 base)
{
    gint64 last_size = MAX(file_size(part), 0);
    double last_time = now_seconds();
    for (;;) {
        g_usleep(POLL_MS * 1000);
        int code = 0;
        bool ended = child_ended(pid, &code);
        gint64 size = MAX(file_size(part), 0);
        double now = now_seconds();

        g_mutex_lock(&q.lock);
        j->have = base + size;
        j->progress = j->total ? (double)j->have / j->total : 0;
        if (now - last_time >= 0.8) {
            double current = MAX(size - last_size, 0) / (now - last_time);
            j->speed = j->speed > 0 ? 0.7 * j->speed + 0.3 * current : current;
            j->remaining = j->speed > 1 ? (j->total - j->have) / j->speed : -1;
            last_size = size;
            last_time = now;
        }
        bool interrupted = job_interrupted(j);
        bool new_rate = rate_changed(rate, rate_per_download());
        g_mutex_unlock(&q.lock);

        if (ended) return code;
        if (interrupted || new_rate) {
            stop_child(pid);
            return interrupted ? -1 : -2;
        }
    }
}

/* Wartezeit vor dem nächsten Versuch, die eine Pause sofort beendet */
static void retry_wait(Job *j)
{
    for (int waited = 0; waited < RETRY_WAIT_MS; waited += POLL_MS) {
        g_mutex_lock(&q.lock);
        bool interrupted = job_interrupted(j);
        g_mutex_unlock(&q.lock);
        if (interrupted) return;
        g_usleep(POLL_MS * 1000);
    }
}

/* Platz für die restlichen Bytes vorhanden? */
static bool enough_space(const char *path, gint64 needed)
{
    char *dir = g_path_get_dirname(path);
    gint64 available = free_space(dir);
    bool ok = available < 0 || available >= needed + MIN_FREE_SPACE;
    g_free(dir);
    return ok;
}

/* Eine Datei fertig laden -> true = vollständig unter path; false = unterbrochen oder
 * Fehler (*error gesetzt) */
static bool download_file(Job *j, const char *url, const char *path, gint64 size, gint64 base,
                          char **error)
{
    char *part = g_strconcat(path, ".part", NULL);
    int failures = 0;
    gint64 progress_mark = -1;
    bool complete = false;
    for (;;) {
        gint64 have = file_size(part);
        if (have > size) {                          /* Datei passt nicht zur Quelle */
            g_unlink(part);
            have = 0;
        }
        if (have == size) {
            complete = true;
            break;
        }
        g_mutex_lock(&q.lock);
        bool interrupted = job_interrupted(j);
        gint64 rate = rate_per_download();
        g_mutex_unlock(&q.lock);
        if (interrupted) break;

        GPid pid = start_curl(url, part, rate);
        if (!pid) {
            *error = g_strdup("Das Programm ist unvollständig installiert (curl fehlt). Bitte "
                              "neu installieren.");
            break;
        }
        int rc = watch_curl(j, pid, part, rate, base);
        if (rc == -1) break;
        if (rc == -2) continue;
        gint64 current = MAX(file_size(part), 0);
        if (rc == 0 && current == size) continue;   /* oben als fertig erkannt */
        if (rc == 33) {                             /* Server kann nicht fortsetzen */
            g_unlink(part);
            continue;
        }
        if (rc == 23) {
            *error = g_strdup("Die Datei lässt sich nicht schreiben. Platz auf dem Laufwerk "
                              "prüfen und erneut versuchen.");
            break;
        }
        if (current > progress_mark) failures = 0;  /* es ging voran: neue Versuche */
        progress_mark = current;
        md_log("Download unterbrochen (curl %d): %s", rc, j->title);
        if (++failures >= MAX_FAILURES) {
            *error = g_strdup_printf("Download abgebrochen (curl-Fehler %d). "
                                     "Er lässt sich später fortsetzen.", rc);
            break;
        }
        retry_wait(j);
    }
    if (complete && g_rename(part, path) != 0) {
        *error = g_strdup("Die fertige Datei lässt sich nicht umbenennen.");
        complete = false;
    }
    g_free(part);
    return complete;
}

/* Untertitel laden; ein Fehler bricht den Auftrag nicht ab (die Spur fehlt dann) */
static void download_subtitle(Job *j, const Extra *e, const char *path)
{
    char *part = g_strconcat(path, ".part", NULL);
    char *argv[] = {CURL_CMD, "-sS", "-f", "-L", "-m", "60", "-A", USER_AGENT, "-o", part,
                    "--url", e->url, NULL};
    if (run_capture(argv, NULL, NULL) == 0) g_rename(part, path);
    else md_log("Untertitel nicht verfügbar: %s (%s)", j->title, e->url);
    g_unlink(part);
    g_free(part);
}

/* ffmpeg-Fortschritt aus dem Puffer lesen; verarbeitete Zeilen entfernen */
static void parse_progress(GString *buffer, double *seconds, double *factor)
{
    char *newline;
    while ((newline = strchr(buffer->str, '\n'))) {
        *newline = 0;
        if (!strncmp(buffer->str, "out_time_us=", 12) || !strncmp(buffer->str, "out_time_ms=", 12)) {
            double value = g_ascii_strtod(buffer->str + 12, NULL);
            if (value > 0) *seconds = value / 1e6;
        } else if (!strncmp(buffer->str, "speed=", 6)) {
            double value = g_ascii_strtod(buffer->str + 6, NULL);
            if (value > 0) *factor = value;
        }
        g_string_erase(buffer, 0, newline + 1 - buffer->str);
    }
}

/* Argument an die Befehlszeile anhängen */
static void arg_add(GPtrArray *argv, const char *text)
{
    g_ptr_array_add(argv, g_strdup(text));
}

/* Mehrere feste Argumente anhängen (NULL-terminiert) */
static void args_add(GPtrArray *argv, const char *const *texts)
{
    for (; *texts; texts++) arg_add(argv, *texts);
}

/* ---------------------------------------------------------------------------
 * HLS-Streams (ARTE, SRF, NRK)
 *
 * ffmpeg lädt die Wiedergabeliste und schreibt Bild und alle Tonspuren in die MP4-Datei. Eine
 * angefangene Datei lässt sich nicht fortsetzen: nach einer Pause beginnt der Stream von vorn.
 * Verschlüsselte Streams (EXT-X-KEY) und Livestreams werden nicht geladen.
 * ------------------------------------------------------------------------- */
typedef struct {
    char *uri, *name, *lang;
} HlsAudio;

/* Wert eines Attributs einer #EXT-X-Zeile; NULL = fehlt */
static char *hls_attribute(const char *line, const char *name)
{
    size_t length = strlen(name);
    for (const char *p = strchr(line, ':'); p && *p; p = strchr(p + 1, ',')) {
        const char *key = p + 1;
        if (strncmp(key, name, length) || key[length] != '=') continue;
        const char *value = key + length + 1;
        if (*value == '"') {
            const char *close = strchr(value + 1, '"');
            return close ? g_strndup(value + 1, (gsize)(close - value - 1)) : NULL;
        }
        return g_strndup(value, strcspn(value, ","));
    }
    return NULL;
}

/* relative Adresse aus der Liste base auflösen; NULL = keine Webadresse */
static char *hls_resolve(const char *base, const char *uri)
{
    char *url = g_uri_resolve_relative(base, uri, G_URI_FLAGS_NONE, NULL);
    if (url && !url_allowed(url)) g_clear_pointer(&url, g_free);
    return url;
}

static void hls_audio_free(gpointer data)
{
    HlsAudio *a = data;
    g_free(a->uri);
    g_free(a->name);
    g_free(a->lang);
    g_free(a);
}

/* Wiedergabeliste holen -> Text, NULL = Fehler (*error gesetzt) */
static char *hls_fetch(const char *url, char **error)
{
    char *argv[] = {CURL_CMD, "-sS", "-f", "-L", "-m", "30", "-A", USER_AGENT, "--url",
                    (char *)url, NULL};
    char *out = NULL;
    int rc = run_capture(argv, &out, NULL);
    if (rc == 0 && out && g_str_has_prefix(out, "#EXTM3U")) return out;
    g_free(out);
    *error = g_strdup(rc == 22 ? "Das Video ist nicht mehr abrufbar oder aus Deutschland "
                                 "gesperrt. Erneut versuchen hilft meist nicht."
                               : UNREACHABLE_TEXT);
    return NULL;
}

/* zweibuchstabige Sprache der Liste -> ISO 639-2 */
static const char *hls_language(const char *code)
{
    static const char *const CODES[][2] = {{"de", "deu"}, {"fr", "fra"}, {"en", "eng"},
                                           {"es", "spa"}, {"it", "ita"}, {"pl", "pol"}};
    for (size_t i = 0; code && i < G_N_ELEMENTS(CODES); i++)
        if (!g_ascii_strcasecmp(code, CODES[i][0])) return CODES[i][1];
    return code && strlen(code) == 3 ? code : "und";
}

/* Stufe einer Hauptliste wählen: höchste Bildhöhe bis zur Grenze der Qualität, bei gleicher
 * Höhe H.264. -> Adresse der Teilliste; *audio_group und *bandwidth dazu. */
static char *hls_pick_variant(const char *master, const char *base, MdQuality quality,
                              char **audio_group, gint64 *bandwidth)
{
    static const int MAX_HEIGHT[] = {1080, 720, 480};
    char **lines = g_strsplit(master, "\n", -1), *best = NULL;
    int best_height = -1;
    bool best_avc = false, best_fits = false;
    for (int i = 0; lines[i]; i++) {
        g_strchomp(lines[i]);
        if (!g_str_has_prefix(lines[i], "#EXT-X-STREAM-INF:") || !lines[i + 1]) continue;
        char *resolution = hls_attribute(lines[i], "RESOLUTION");
        char *codecs = hls_attribute(lines[i], "CODECS");
        char *bw = hls_attribute(lines[i], "AVERAGE-BANDWIDTH");
        if (!bw) bw = hls_attribute(lines[i], "BANDWIDTH");
        const char *x = resolution ? strchr(resolution, 'x') : NULL;
        int height = x ? atoi(x + 1) : 0;
        bool avc = !codecs || strstr(codecs, "avc1"), fits = height <= MAX_HEIGHT[quality];
        /* passende Höhe vor zu großer; darin die größte, sonst die kleinste */
        bool better = !best || (fits && !best_fits)
                      || (fits == best_fits && (fits ? height > best_height
                                                     : height < best_height))
                      || (height == best_height && avc && !best_avc);
        g_strchomp(lines[i + 1]);
        char *uri = better ? hls_resolve(base, lines[i + 1]) : NULL;
        if (uri) {
            g_free(best);
            best = uri;
            best_height = height;
            best_avc = avc;
            best_fits = fits;
            g_free(*audio_group);
            *audio_group = hls_attribute(lines[i], "AUDIO");
            *bandwidth = bw ? g_ascii_strtoll(bw, NULL, 10) : 0;
        }
        g_free(resolution);
        g_free(codecs);
        g_free(bw);
    }
    g_strfreev(lines);
    return best;
}

/* Tonspuren der Gruppe group mit eigener Liste */
static GPtrArray *hls_audio_tracks(const char *master, const char *base, const char *group)
{
    GPtrArray *tracks = g_ptr_array_new_with_free_func(hls_audio_free);
    char **lines = g_strsplit(master, "\n", -1);
    for (int i = 0; group && lines[i]; i++) {
        g_strchomp(lines[i]);
        if (!g_str_has_prefix(lines[i], "#EXT-X-MEDIA:")) continue;
        char *type = hls_attribute(lines[i], "TYPE"), *id = hls_attribute(lines[i], "GROUP-ID");
        char *uri = hls_attribute(lines[i], "URI");
        if (type && id && uri && !strcmp(type, "AUDIO") && !strcmp(id, group)) {
            HlsAudio *a = g_new0(HlsAudio, 1);
            a->uri = hls_resolve(base, uri);
            a->name = hls_attribute(lines[i], "NAME");
            a->lang = hls_attribute(lines[i], "LANGUAGE");
            if (a->uri) g_ptr_array_add(tracks, a);
            else hls_audio_free(a);
        }
        g_free(type);
        g_free(id);
        g_free(uri);
    }
    g_strfreev(lines);
    return tracks;
}

/* Teilliste prüfen -> Länge in Sekunden, 0 = Fehler (*error gesetzt) */
static double hls_duration(const char *url, char **error)
{
    char *text = hls_fetch(url, error);
    if (!text) return 0;
    double seconds = 0;
    bool encrypted = false;
    char **lines = g_strsplit(text, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (g_str_has_prefix(lines[i], "#EXTINF:")) seconds += g_ascii_strtod(lines[i] + 8, NULL);
        if (g_str_has_prefix(lines[i], "#EXT-X-KEY:") && !strstr(lines[i], "METHOD=NONE"))
            encrypted = true;
    }
    if (encrypted)
        *error = g_strdup("Der Stream ist verschlüsselt und wird nicht geladen.");
    else if (!strstr(text, "#EXT-X-ENDLIST"))
        *error = g_strdup("Das ist ein Livestream, kein abrufbares Video.");
    else if (seconds <= 0)
        *error = g_strdup("Die Wiedergabeliste ist leer.");
    g_strfreev(lines);
    g_free(text);
    return *error ? 0 : seconds;
}

/* ffmpeg-Aufruf: ein Eingang je Liste, Bild aus dem ersten, Ton aus allen übrigen */
static GPtrArray *hls_command(const char *video, GPtrArray *audio, double readrate,
                              const char *part)
{
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
    args_add(argv, (const char *const[]){"ffmpeg", "-nostdin", "-v", "error", "-nostats",
                                         "-progress", "pipe:1", "-y", NULL});
    char rate[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(rate, sizeof rate, "%.2f", readrate);
    for (guint i = 0; i <= audio->len; i++) {
        const char *url = i ? ((HlsAudio *)audio->pdata[i - 1])->uri : video;
        args_add(argv, (const char *const[]){"-protocol_whitelist", "http,https,tls,tcp",
                                             "-user_agent", USER_AGENT, NULL});
        if (readrate > 0) args_add(argv, (const char *const[]){"-readrate", rate, NULL});
        args_add(argv, (const char *const[]){"-i", url, NULL});
    }
    args_add(argv, (const char *const[]){"-map", "0:v:0", NULL});
    if (!audio->len) args_add(argv, (const char *const[]){"-map", "0:a?", NULL});
    for (guint i = 0; i < audio->len; i++) {
        const HlsAudio *a = audio->pdata[i];
        char *map = g_strdup_printf("%u:a:0", i + 1), *key = g_strdup_printf("-metadata:s:a:%u", i);
        char *lang = g_strdup_printf("language=%s", hls_language(a->lang));
        char *title = g_strdup_printf("title=%s", a->name ? a->name : "");
        /* MP4 bewahrt den Namen nur als handler_name */
        char *handler = g_strdup_printf("handler_name=%s", a->name ? a->name : "");
        args_add(argv, (const char *const[]){"-map", map, key, lang, key, title, key, handler,
                                             NULL});
        g_free(handler);
        g_free(map);
        g_free(key);
        g_free(lang);
        g_free(title);
    }
    args_add(argv, (const char *const[]){"-c", "copy", "-f", "mp4", part, NULL});
    g_ptr_array_add(argv, NULL);
    return argv;
}

/* ffmpeg begleiten -> Exit-Code, -1 = unterbrochen. Fortschritt nach der Zeit im Video */
static int watch_hls(Job *j, GPtrArray *argv, const char *part, double duration, gint64 base,
                     char **messages)
{
    GPid pid = 0;
    int out_fd = -1, err_fd = -1;
    if (!g_spawn_async_with_pipes(NULL, (char **)argv->pdata, NULL,
                                  G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, child_setup,
                                  NULL, &pid, NULL, &out_fd, &err_fd, NULL)) {
        *messages = g_strdup(FFMPEG_MISSING_TEXT);
        return 1;
    }
    adopt_child(pid, false);
    prepare_pipe(out_fd);
    prepare_pipe(err_fd);
    GString *progress = g_string_new(NULL), *errors = g_string_new(NULL);
    double seconds = 0, factor = 0, last_time = now_seconds();
    gint64 last_size = 0;
    int rc = -1;
    bool interrupted = false;
    for (;;) {
        wait_pipes(out_fd, err_fd);
        drain_pipe(out_fd, progress);
        drain_pipe(err_fd, errors);
        parse_progress(progress, &seconds, &factor);
        if (errors->len > 2000) g_string_erase(errors, 0, errors->len - 2000);
        gint64 size = MAX(file_size(part), 0);
        double now = now_seconds(), share = MIN(seconds / duration, 1.0);

        g_mutex_lock(&q.lock);
        gint64 estimate = share > 0.01 ? (gint64)(size / share) : 0;
        if (estimate) j->total = base + estimate;
        j->have = base + size;
        j->progress = share;
        if (now - last_time >= 0.8) {
            double current = MAX(size - last_size, 0) / (now - last_time);
            j->speed = j->speed > 0 ? 0.7 * j->speed + 0.3 * current : current;
            j->remaining = factor > 0 ? (duration - seconds) / factor : -1;
            last_size = size;
            last_time = now;
        }
        interrupted = job_interrupted(j);
        g_mutex_unlock(&q.lock);

        if (interrupted) {
            stop_child(pid);
            break;
        }
        if (child_ended(pid, &rc)) {
            drain_pipe(err_fd, errors);
            break;
        }
    }
    close(out_fd);
    close(err_fd);
    g_string_free(progress, TRUE);
    *messages = g_strstrip(g_string_free(errors, FALSE));
    return interrupted ? -1 : rc;
}

/* HLS-Stream nach path laden -> true = fertig; false = unterbrochen oder Fehler (*error) */
static bool download_stream(Job *j, const char *url, const char *path, gint64 base,
                            char **error)
{
    char *master = hls_fetch(url, error);
    if (!master) return false;
    char *video = NULL, *group = NULL;
    gint64 bandwidth = 0;
    if (strstr(master, "#EXT-X-STREAM-INF:"))
        video = hls_pick_variant(master, url, j->quality, &group, &bandwidth);
    else video = g_strdup(url);
    if (!bandwidth) bandwidth = (gint64)(QUALITY_KBIT_PER_SECOND[j->quality] * 1000);
    GPtrArray *audio = hls_audio_tracks(master, url, group);
    double duration = video ? hls_duration(video, error) : 0;
    if (!video && !*error) *error = g_strdup("Die Wiedergabeliste enthält kein Video.");
    /* auch die Tonspuren dürfen nicht verschlüsselt sein */
    for (guint i = 0; i < audio->len && !*error; i++)
        hls_duration(((HlsAudio *)audio->pdata[i])->uri, error);
    char *part = g_strconcat(path, ".part", NULL);
    bool complete = false;
    for (int failures = 0; !*error;) {
        g_mutex_lock(&q.lock);
        bool interrupted = job_interrupted(j);
        gint64 rate = rate_per_download();
        g_mutex_unlock(&q.lock);
        if (interrupted) break;
        /* Tempolimit über die Lesegeschwindigkeit: Vielfaches der Abspielzeit */
        double bytes_per_second = (bandwidth + HLS_AUDIO_BITRATE * (gint64)audio->len) / 8.0;
        double readrate = rate > 0 && bytes_per_second > 0 ? MAX(rate / bytes_per_second, 0.1) : 0;
        GPtrArray *argv = hls_command(video, audio, readrate, part);
        char *messages = NULL;
        int rc = watch_hls(j, argv, part, duration, base, &messages);
        g_ptr_array_free(argv, TRUE);
        if (rc == 0 && file_size(part) > 0) {
            complete = g_rename(part, path) == 0;
            if (!complete) *error = g_strdup("Die fertige Datei lässt sich nicht umbenennen.");
        } else if (rc > 0) {
            md_log("Stream unterbrochen (ffmpeg %d): %s – %s", rc, j->title, messages);
            if (++failures >= MAX_FAILURES)
                *error = g_strdup_printf("Der Stream lässt sich nicht laden: %s",
                                         *messages ? messages : "ffmpeg-Fehler");
            else retry_wait(j);
        }
        g_free(messages);
        if (rc <= 0 || complete) break;
    }
    g_unlink(part);                                 /* ein Stream beginnt immer von vorn */
    g_ptr_array_free(audio, TRUE);
    g_free(part);
    g_free(video);
    g_free(group);
    g_free(master);
    return complete;
}

/* Größe mit bis zu drei Versuchen erfragen, 0 = Fehler */
static gint64 remote_size_retry(Job *j, const char *url, char **error)
{
    gint64 size = 0;
    for (int attempt = 0; attempt < 3 && !size; attempt++) {
        g_clear_pointer(error, g_free);
        size = remote_size(url, error);
        if (size || !strstr(*error, "erreichbar")) break;
        retry_wait(j);
    }
    return size;
}

/* Stream laden; bei der NRK zuerst die Adresse erfragen (sie gilt nur kurz) */
static bool download_hls(Job *j, const char *url, const char *path, gint64 base, char **error)
{
    if (!g_str_has_prefix(url, NRK_MANIFEST)) return download_stream(j, url, path, base, error);
    char *stream = nrk_stream(url, error);
    bool complete = stream && download_stream(j, stream, path, base, error);
    g_free(stream);
    return complete;
}

/* Worker: alle Dateien eines Auftrags herunterladen. Die Angaben im Job ändern sich nicht,
 * solange worker gesetzt ist, und dürfen deshalb ohne Sperre gelesen werden. */
static gpointer download_thread(gpointer data)
{
    Job *j = data;
    char *result = result_path(j->target), *dir = g_path_get_dirname(j->target), *error = NULL;
    bool processing = needs_processing(j);
    bool already_there = file_size(result) > 0;
    bool complete = already_there;
    guint count = 1 + j->extras->len;
    gint64 *sizes = g_new0(gint64, count);
    gint64 total = 0;

    if (!complete && g_mkdir_with_parents(dir, 0755) != 0)
        error = g_strdup_printf("Ordner lässt sich nicht anlegen: %s", g_strerror(errno));

    /* Größen aller Videodateien erfragen, damit der Fortschritt von Anfang an stimmt */
    if (!complete && !error) {
        gint64 missing = 0;
        for (guint i = 0; i < count && !error; i++) {
            const Extra *e = i ? j->extras->pdata[i - 1] : NULL;
            if ((e && e->subtitle) || is_hls(e ? e->url : j->url)) continue;
            sizes[i] = remote_size_retry(j, e ? e->url : j->url, &error);
            total += sizes[i];
            char *path = e ? extra_path(j->target, i - 1, false) : g_strdup(j->target);
            char *part = g_strconcat(path, ".part", NULL);
            missing += file_size(path) == sizes[i] ? 0 : sizes[i] - MAX(file_size(part), 0);
            g_free(path);
            g_free(part);
        }
        if (!error && !enough_space(j->target, missing))
            error = g_strdup("Zu wenig freier Platz am Speicherort.");
    }
    if (!complete && !error) {
        g_mutex_lock(&q.lock);
        j->total = total;
        g_mutex_unlock(&q.lock);
        gint64 base = 0;
        complete = true;
        for (guint i = 0; i < count && complete; i++) {
            const Extra *e = i ? j->extras->pdata[i - 1] : NULL;
            char *path = e ? extra_path(j->target, i - 1, e->subtitle) : g_strdup(j->target);
            if (e && e->subtitle) {
                if (file_size(path) <= 0) download_subtitle(j, e, path);
            } else if (is_hls(e ? e->url : j->url)) {
                if (file_size(path) <= 0)
                    complete = download_hls(j, e ? e->url : j->url, path, base, &error);
                sizes[i] = MAX(file_size(path), 0);
            } else if (file_size(path) != sizes[i]) {
                complete = download_file(j, e ? e->url : j->url, path, sizes[i], base, &error);
            }
            base += sizes[i];
            g_free(path);
        }
    }

    g_mutex_lock(&q.lock);
    if (error) {
        job_fail(j, error);
    } else if (complete) {
        j->have = j->total;
        j->progress = 0;
        job_set_state(j, processing && !already_there ? MD_JOB_CONVERT_WAITING : MD_JOB_DONE);
        if (already_there) {
            g_free(j->message);
            j->message = g_strdup("War schon vorhanden");
        }
        if (j->state == MD_JOB_DONE) {
            j->progress = 1;
            history_add(j);
        }
        md_log("Download fertig: %s", j->title);
    } else {
        job_set_state(j, MD_JOB_WAITING);       /* pausiert, abgebrochen oder Programmende */
    }
    worker_finish(j);
    g_mutex_unlock(&q.lock);
    g_free(result);
    g_free(dir);
    g_free(sizes);
    g_free(error);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Umwandlung
 * ------------------------------------------------------------------------- */
#ifdef G_OS_WIN32
/* AV1-Encoder der Grafikkarte: NVIDIA, Intel oder AMD, NULL = keiner. Unter Windows gibt es
 * kein Render-Gerät; der Name des Encoders steht an seiner Stelle. */
static gpointer find_gpu_encoder(gpointer data)
{
    (void)data;
    static const char *const ENCODERS[] = {"av1_nvenc", "av1_qsv", "av1_amf"};
    for (size_t i = 0; i < G_N_ELEMENTS(ENCODERS); i++) {
        char *argv[] = {"ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-f", "lavfi",
                        "-i", "color=black:s=320x240:d=0.2", "-pix_fmt", "p010le", "-c:v",
                        (char *)ENCODERS[i], "-f", "null", "-", NULL};
        if (run_capture(argv, NULL, NULL) == 0) return (gpointer)ENCODERS[i];
    }
    return NULL;
}

static const char *render_device(void)
{
    static GOnce once = G_ONCE_INIT;
    return g_once(&once, find_gpu_encoder, NULL);
}

bool md_gpu_available(void)
{
    return render_device() != NULL;
}

/* Bild per Grafikkarte nach AV1, Qualität als konstanter Quantisierer */
static void add_gpu_encoder(GPtrArray *a, const char *encoder, const char *quality,
                            const char *crop)
{
    if (crop) args_add(a, (const char *const[]){"-vf", crop, NULL});
    args_add(a, (const char *const[]){"-pix_fmt", "p010le", "-c:v", encoder, NULL});
    if (!strcmp(encoder, "av1_nvenc"))
        args_add(a, (const char *const[]){"-rc", "constqp", "-qp", quality, NULL});
    else if (!strcmp(encoder, "av1_amf"))
        args_add(a, (const char *const[]){"-rc", "cqp", "-qp_i", quality, "-qp_p", quality,
                                          NULL});
    else args_add(a, (const char *const[]){"-global_quality:v", quality, NULL});
    args_add(a, (const char *const[]){"-g:v", GOP_FRAMES, NULL});
}
#else
/* erstes Render-Gerät, NULL = keines (Ergebnis statisch) */
static const char *render_device(void)
{
    static char device[32];
    for (int i = 128; i < 136 && !*device; i++) {
        char path[32];
        snprintf(path, sizeof path, "/dev/dri/renderD%d", i);
        if (g_access(path, R_OK | W_OK) == 0) snprintf(device, sizeof device, "%s", path);
    }
    return *device ? device : NULL;
}

bool md_gpu_available(void)
{
    const char *device = render_device();
    if (!device) return false;
    char *argv[] = {"ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "error", "-vaapi_device",
                    (char *)device, "-f", "lavfi", "-i", "color=black:s=320x240:d=0.2",
                    "-vf", "format=p010,hwupload", "-c:v", "av1_vaapi", "-f", "null", "-", NULL};
    return run_capture(argv, NULL, NULL) == 0;
}

/* Bild per VAAPI nach AV1 */
static void add_gpu_encoder(GPtrArray *a, const char *device, const char *quality,
                            const char *crop)
{
    (void)device;
    char *filter = g_strdup_printf("%s%sformat=p010,hwupload", crop ? crop : "", crop ? "," : "");
    args_add(a, (const char *const[]){"-vf", filter, "-c:v", "av1_vaapi",
                                      "-rc_mode", "CQP", "-global_quality:v", quality,
                                      "-g:v", GOP_FRAMES, NULL});
    g_free(filter);
}
#endif

/* Videocodec und Dauer lesen -> false bei Fehler */
static bool media_info(const char *path, char **codec, double *duration)
{
    char *argv[] = {"ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                    "stream=codec_name:format=duration", "-of", "default=noprint_wrappers=1",
                    (char *)path, NULL};
    char *out = NULL;
    bool ok = run_capture(argv, &out, NULL) == 0;
    *codec = NULL;
    *duration = 0;
    char **lines = g_strsplit(out ? out : "", "\n", -1);
    for (int i = 0; lines[i]; i++) {
        if (!strncmp(lines[i], "codec_name=", 11)) *codec = g_strdup(lines[i] + 11);
        if (!strncmp(lines[i], "duration=", 9)) *duration = g_ascii_strtod(lines[i] + 9, NULL);
    }
    g_strfreev(lines);
    g_free(out);
    return ok && *codec && *duration > 0;
}

/* Breite und Höhe des Bildes -> false bei Fehler */
static bool video_size(const char *path, int *width, int *height)
{
    char *argv[] = {"ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
                    "stream=width,height", "-of", "csv=p=0:s=x", (char *)path, NULL};
    char *out = NULL;
    bool ok = run_capture(argv, &out, NULL) == 0 && out &&
              sscanf(out, "%dx%d", width, height) == 2 && *width > 0 && *height > 0;
    g_free(out);
    return ok;
}

/* Fest ins Bild kodierte schwarze Balken finden: cropdetect an CROP_SAMPLES Stellen, der
 * Bereich mit Inhalt ist die Vereinigung aller Stellen. Eine dunkle Szene lässt sich so nicht
 * für einen Balken halten. -> false, wenn es nichts abzuschneiden gibt. */
static bool detect_bars(const char *path, double duration, MdCrop *crop)
{
    int width = 0, height = 0;
    if (duration < 60 || !video_size(path, &width, &height)) return false;
    int x1 = width, x2 = -1, y1 = height, y2 = -1, valid = 0;
    for (int i = 0; i < CROP_SAMPLES; i++) {
        char at[32];
        g_ascii_formatd(at, sizeof at, "%.1f", duration * (i + 1) / (CROP_SAMPLES + 1));
        char *argv[] = {"ffmpeg", "-hide_banner", "-nostdin", "-loglevel", "info", "-ss", at,
                        "-i", (char *)path, "-map", "0:v:0", "-vf",
                        "cropdetect=limit=24:round=2:reset=0", "-frames:v", CROP_FRAMES, "-an",
                        "-sn", "-f", "null", "-", NULL};
        char *err = NULL;
        if (run_capture(argv, NULL, &err) == 0 && err) {
            /* mit reset=0 steht der Bereich aller Bilder in der letzten Zeile */
            const char *last = g_strrstr(err, "x1:");
            int a1, a2, b1, b2;
            if (last && sscanf(last, "x1:%d x2:%d y1:%d y2:%d", &a1, &a2, &b1, &b2) == 4 &&
                a2 > a1 && b2 > b1) {
                x1 = MIN(x1, a1);
                x2 = MAX(x2, a2);
                y1 = MIN(y1, b1);
                y2 = MAX(y2, b2);
                valid++;
            }
        }
        g_free(err);
    }
    if (valid < CROP_SAMPLES / 2 + 1) return false;
    /* gerade Werte, im Zweifel etwas mehr Bild */
    x1 = MAX(x1, 0) & ~1;
    y1 = MAX(y1, 0) & ~1;
    int w = MIN((x2 - x1 + 2) & ~1, width - x1), h = MIN((y2 - y1 + 2) & ~1, height - y1);
    bool worth = width - w >= CROP_MIN_PIXELS || height - h >= CROP_MIN_PIXELS;
    if (!worth || w < width / 2 || h < height / 2) return false;
    *crop = (MdCrop){.x = x1, .y = y1, .width = w, .height = h, .frame_width = width,
                     .frame_height = height};
    return true;
}

bool md_detect_bars(const char *path, MdCrop *crop)
{
    char *codec = NULL;
    double duration = 0;
    bool ok = media_info(path, &codec, &duration) && detect_bars(path, duration, crop);
    g_free(codec);
    return ok;
}

/* Filter gegen die Balken für ffmpeg, NULL = keine */
static char *crop_filter(const char *path, double duration)
{
    MdCrop c;
    if (!detect_bars(path, duration, &c)) return NULL;
    return g_strdup_printf("crop=%d:%d:%d:%d", c.width, c.height, c.x, c.y);
}

/* Name und Sprache der Ausgabespur setzen; kind = 'a' oder 's' */
static void add_track_metadata(GPtrArray *argv, char kind, int index, const char *label,
                               const char *lang)
{
    char *stream = g_strdup_printf("-metadata:s:%c:%d", kind, index);
    char *title = g_strdup_printf("title=%s", label), *language = g_strdup_printf("language=%s", lang);
    args_add(argv, (const char *const[]){stream, language, stream, title, NULL});
    g_free(stream);
    g_free(title);
    g_free(language);
}

/* ffmpeg-Befehl bauen: Bild aus der Hauptdatei, dazu alle vorhandenen Spuren.
 * copy = Bild unverändert übernehmen, sonst AV1 mit dem gewählten Encoder; crop = Filter gegen
 * schwarze Balken oder NULL. */
static GPtrArray *build_ffmpeg_cmd(const Job *j, bool copy, MdEncoder encoder, const char *device,
                                   const char *crop, const char *tmp)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    args_add(a, (const char *const[]){"ffmpeg", "-hide_banner", "-nostdin", "-y", "-loglevel",
                                      "error", "-progress", "pipe:1", "-nostats", NULL});
#ifndef G_OS_WIN32
    if (!copy && encoder == MD_ENCODER_GPU)
        args_add(a, (const char *const[]){"-vaapi_device", device, NULL});
#endif
    args_add(a, (const char *const[]){"-i", j->target, NULL});

    GPtrArray *maps = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *metadata = g_ptr_array_new_with_free_func(g_free);
    args_add(maps, (const char *const[]){"-map", "0:v:0", NULL});
    int inputs = 1, audio = 0, subtitles = 0;
    if (is_reconvert(j)) {
        /* die Spuren sind schon in der Datei und behalten ihre Angaben */
        args_add(maps, (const char *const[]){"-map", "0:a?", "-map", "0:s?", NULL});
    } else if (!j->skip_main_audio) {
        args_add(maps, (const char *const[]){"-map", "0:a?", NULL});
        add_track_metadata(metadata, 'a', audio++, j->main_label, j->main_lang);
    }
    for (guint i = 0; i < j->extras->len; i++) {
        const Extra *e = j->extras->pdata[i];
        char *path = extra_path(j->target, i, e->subtitle);
        if (file_size(path) > 0) {
            args_add(a, (const char *const[]){"-i", path, NULL});
            char *map = g_strdup_printf("%d:%c:0", inputs++, e->subtitle ? 's' : 'a');
            args_add(maps, (const char *const[]){"-map", map, NULL});
            g_free(map);
            if (e->subtitle) add_track_metadata(metadata, 's', subtitles++, e->label, e->lang);
            else add_track_metadata(metadata, 'a', audio++, e->label, e->lang);
        }
        g_free(path);
    }
    for (guint i = 0; i < maps->len; i++) arg_add(a, maps->pdata[i]);
    args_add(a, (const char *const[]){"-map_metadata", "0", NULL});

    char quality[16];
    if (copy) {
        args_add(a, (const char *const[]){"-c:v", "copy", NULL});
    } else if (encoder == MD_ENCODER_GPU) {
        snprintf(quality, sizeof quality, "%d", j->gpu_quality);
        add_gpu_encoder(a, device, quality, crop);
    } else {
        snprintf(quality, sizeof quality, "%d", j->cpu_rf);
        char *filter = g_strdup_printf("%s%sformat=yuv420p10le", crop ? crop : "",
                                       crop ? "," : "");
        args_add(a, (const char *const[]){"-vf", filter, "-c:v", "libsvtav1",
                                          "-preset", SVT_PRESET, "-crf", quality, "-svtav1-params",
                                          "tune=1", "-g:v", GOP_FRAMES, NULL});
        g_free(filter);
    }
    args_add(a, (const char *const[]){"-c:a", "copy", "-c:s", "srt", NULL});
    for (guint i = 0; i < metadata->len; i++) arg_add(a, metadata->pdata[i]);
    args_add(a, (const char *const[]){"-max_muxing_queue_size", "4096", "-f", "matroska", tmp,
                                      NULL});
    g_ptr_array_add(a, NULL);
    g_ptr_array_free(maps, TRUE);
    g_ptr_array_free(metadata, TRUE);
    return a;
}

/* ffmpeg ausführen -> 0 = fertig, -1 = unterbrochen, sonst Fehler (die letzten
 * ffmpeg-Meldungen landen in *messages) */
static int run_encoder(Job *j, bool copy, MdEncoder encoder, const char *crop, const char *tmp,
                       double duration, char **messages)
{
    const char *device = render_device();
    if (!copy && encoder == MD_ENCODER_GPU && !device) {
        *messages = g_strdup("Keine Grafikkarte mit AV1-Encoder gefunden.");
        return 1;
    }
    GPtrArray *command = build_ffmpeg_cmd(j, copy, encoder, device, copy ? NULL : crop, tmp);
    char **argv = (char **)command->pdata;

    GPid pid = 0;
    int out_fd = -1, err_fd = -1;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL,
                                  G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, child_setup,
                                  GINT_TO_POINTER(1), &pid, NULL, &out_fd, &err_fd, NULL)) {
        *messages = g_strdup(FFMPEG_MISSING_TEXT);
        g_ptr_array_free(command, TRUE);
        return 1;
    }
    g_ptr_array_free(command, TRUE);
    adopt_child(pid, true);
    prepare_pipe(out_fd);
    prepare_pipe(err_fd);
    GString *progress = g_string_new(NULL), *errors = g_string_new(NULL);
    double seconds = 0, factor = 0;
    bool stopped = false, interrupted = false;
    int rc = 128;
    for (;;) {
        wait_pipes(out_fd, err_fd);
        drain_pipe(out_fd, progress);
        drain_pipe(err_fd, errors);
        parse_progress(progress, &seconds, &factor);
        if (errors->len > 2000) g_string_erase(errors, 0, errors->len - 2000);

        g_mutex_lock(&q.lock);
        bool end = j->remove_requested || q.quitting, pause = j->paused || q.conversions_held;
        j->progress = duration > 0 ? MIN(seconds / duration, 1.0) : 0;
        j->remaining = factor > 0 ? (duration - seconds) / factor : -1;
        g_mutex_unlock(&q.lock);

        if (end) {
            interrupted = true;
            stop_child(pid);
            break;
        }
        if (pause != stopped) {
            suspend_child(pid, pause);
            stopped = pause;
        }
        if (child_ended(pid, &rc)) {
            drain_pipe(err_fd, errors);
            break;
        }
    }
    close(out_fd);
    close(err_fd);
    g_string_free(progress, TRUE);
    if (interrupted) rc = -1;
    *messages = g_string_free(errors, FALSE);
    if (rc > 0) g_strstrip(*messages);
    return rc;
}

/* Bytes, die das Ergebnis unterbieten muss, damit AV1 Platz spart */
static gint64 comparable_size(const Job *j, double duration)
{
    gint64 size = file_size(j->target);
    for (guint i = 0; i < j->extras->len; i++)
        if (!((const Extra *)j->extras->pdata[i])->subtitle)
            size += (gint64)(duration * AUDIO_BYTES_PER_SECOND);
    return size;
}

/* Wie viele Spuren wurden wirklich zusammengeführt? */
static char *tracks_note(const Job *j)
{
    int audio = j->skip_main_audio ? 0 : 1, subtitles = 0, missing = 0;
    for (guint i = 0; i < j->extras->len; i++) {
        const Extra *e = j->extras->pdata[i];
        char *path = extra_path(j->target, i, e->subtitle);
        if (file_size(path) <= 0) missing++;
        else if (e->subtitle) subtitles++;
        else audio++;
        g_free(path);
    }
    if (audio <= 1 && !subtitles && !missing) return g_strdup("");
    return g_strdup_printf(" · %d Tonspur%s, %d Untertitel%s", audio, audio == 1 ? "" : "en",
                           subtitles, missing ? " (nicht alle verfügbar)" : "");
}

/* <Name>.mkv -> <Name> (Original).mkv; false bei Fehler */
static bool keep_as_original(const char *path)
{
    char *stem = path_stem(path), *backup = g_strconcat(stem, " (Original)",
                                                        path + strlen(stem), NULL);
    bool ok = g_rename(path, backup) == 0;
    g_free(stem);
    g_free(backup);
    return ok;
}

/* Worker: Spuren zusammenführen und auf Wunsch nach AV1 umwandeln; Ergebnis prüfen */
static gpointer convert_thread(gpointer data)
{
    Job *j = data;
    char *tmp = temp_path(j->target), *result = result_path(j->target);
    char *error = NULL, *note = NULL, *codec = NULL, *messages = NULL, *crop = NULL;
    double duration = 0;
    bool done = false, interrupted = false, copy = !j->convert, av1_useless = false;

    if (!media_info(j->target, &codec, &duration)) {
        error = g_strdup("Die heruntergeladene Datei ist nicht lesbar.");
    } else if (is_reconvert(j) && !strcmp(codec, "av1")) {
        note = g_strdup("Ist schon AV1 – nichts zu tun");
        done = true;
    } else if (!copy && (crop = crop_filter(j->target, duration))) {
        md_log("Schwarze Balken werden abgeschnitten (%s): %s", crop, j->title);
    }
    /* eine MKV-Datei heißt nach der Umwandlung wie vorher; das Original bekommt dann einen
     * anderen Namen oder geht */
    bool same_name = !strcmp(result, j->target);
    while (!error && !done && !interrupted) {
        g_mutex_lock(&q.lock);
        j->merging = copy;
        j->progress = 0;
        g_mutex_unlock(&q.lock);
        g_clear_pointer(&messages, g_free);
        int rc = run_encoder(j, copy, j->encoder, crop, tmp, duration, &messages);
        if (rc > 0 && !copy && j->encoder == MD_ENCODER_GPU) {
            md_log("GPU-Umwandlung fehlgeschlagen, versuche CPU: %s (%s)", j->title, messages);
            g_clear_pointer(&messages, g_free);
            rc = run_encoder(j, false, MD_ENCODER_CPU, crop, tmp, duration, &messages);
        }
        if (rc < 0) {
            interrupted = true;
            break;
        }
        if (rc > 0) {
            error = g_strdup_printf("ffmpeg ist fehlgeschlagen: %s",
                                    messages && *messages ? messages : "unbekannter Fehler");
            break;
        }
        char *out_codec = NULL;
        double out_duration = 0;
        bool readable = media_info(tmp, &out_codec, &out_duration);
        bool right_codec = readable && (copy || !strcmp(out_codec, "av1"));
        double tolerance = MAX(2.0, duration * 0.005);
        g_free(out_codec);
        if (!right_codec || ABS(out_duration - duration) > tolerance) {
            error = g_strdup("Das Ergebnis ist unvollständig – die MP4-Datei bleibt.");
        } else if (!copy && file_size(tmp) >= comparable_size(j, duration)) {
            if (j->extras->len || j->skip_main_audio) {
                copy = true;                /* AV1 bringt nichts: Spuren trotzdem vereinen */
                av1_useless = true;
                continue;
            }
            note = g_strdup(is_reconvert(j) ? "AV1 wäre nicht kleiner – die Datei bleibt"
                                             : "AV1 wäre nicht kleiner – die MP4-Datei bleibt");
            done = true;
        } else if (same_name && j->keep_original && !keep_as_original(j->target)) {
            error = g_strdup("Das Original lässt sich nicht umbenennen.");
        } else if (g_rename(tmp, result) != 0) {
            error = g_strdup("Das Ergebnis lässt sich nicht ablegen.");
        } else {
            char *tracks = tracks_note(j), *after = md_format_size(file_size(result));
            if (copy) {
                note = g_strdup_printf("%s%s%s", after, tracks,
                                       av1_useless ? " · AV1 wäre nicht kleiner" : "");
            } else {
                char *before = md_format_size(comparable_size(j, duration));
                note = g_strdup_printf("AV1: %s statt %s%s%s", after, before,
                                       crop ? " · Balken abgeschnitten" : "", tracks);
                g_free(before);
            }
            g_free(tracks);
            g_free(after);
            /* ohne AV1 steckt die MP4-Datei vollständig in der MKV-Datei */
            if (!same_name && (!j->keep_original || !j->convert)) g_unlink(j->target);
            remove_extra_files(j);
            done = true;
        }
    }
    g_unlink(tmp);

    g_mutex_lock(&q.lock);
    j->merging = false;
    if (error) {
        job_fail(j, error);
    } else if (done) {
        job_set_state(j, MD_JOB_DONE);
        j->progress = 1;
        g_free(j->message);
        j->message = g_strdup(note);
        history_add(j);
        md_log("Verarbeitung fertig: %s (%s)", j->title, note);
    } else if (interrupted) {
        job_set_state(j, MD_JOB_CONVERT_WAITING);
        j->progress = 0;
        if (q.quitting) j->paused = true;
    }
    worker_finish(j);
    g_mutex_unlock(&q.lock);
    g_free(tmp);
    g_free(result);
    g_free(error);
    g_free(note);
    g_free(codec);
    g_free(messages);
    g_free(crop);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Verteiler: startet Worker, solange Plätze frei sind
 * ------------------------------------------------------------------------- */
static gpointer dispatcher_thread(gpointer data)
{
    (void)data;
    g_mutex_lock(&q.lock);
    while (!q.quitting) {
        int downloads = 0, conversions = 0;
        for (guint i = 0; i < q.jobs->len; i++) {
            Job *j = q.jobs->pdata[i];
            if (j->worker && j->state == MD_JOB_DOWNLOADING) downloads++;
            if (j->worker && j->state == MD_JOB_CONVERTING) conversions++;
        }
        bool started = false;
        for (guint i = 0; i < q.jobs->len; i++) {
            Job *j = q.jobs->pdata[i];
            if (j->paused || j->worker) continue;
            GThreadFunc worker = NULL;
            if (j->state == MD_JOB_WAITING && downloads < q.max_downloads && !q.downloads_held) {
                job_set_state(j, MD_JOB_DOWNLOADING);
                downloads++;
                worker = download_thread;
            } else if (j->state == MD_JOB_CONVERT_WAITING && conversions < q.max_conversions &&
                       !q.conversions_held) {
                job_set_state(j, MD_JOB_CONVERTING);
                conversions++;
                worker = convert_thread;
            }
            if (!worker) continue;
            j->worker = true;
            q.workers++;
            started = true;
            g_thread_unref(g_thread_new("auftrag", worker, j));
        }
        if (started) queue_save();
        if (q.save_due) {
            q.save_due = false;
            GString *text = queue_text();
            g_mutex_unlock(&q.lock);
            queue_write(text);
            g_mutex_lock(&q.lock);
            continue;
        }
        g_cond_wait_until(&q.wake, &q.lock, g_get_monotonic_time() + G_TIME_SPAN_SECOND);
    }
    g_mutex_unlock(&q.lock);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Öffentliche Schnittstelle
 * ------------------------------------------------------------------------- */
/* --- Datenordner -----------------------------------------------------------
 * Einstellungen, Verlauf, Warteschlange, Protokoll und Zwischenspeicher liegen zusammen in einem
 * Ordner: "<gewählter Ordner>/Mediathek Downloader". Ohne Wahl unter Windows im
 * Installationsordner ("…\Mediathek Downloader\Daten"); unter Linux ist der Installationsordner
 * /usr nicht beschreibbar, dort bleibt es ~/.mediathek-downloader. Welcher Ordner gewählt ist,
 * steht als einziges außerhalb: in ~/.config/mediathek-downloader/datenordner.
 * ------------------------------------------------------------------------- */
static char *g_data_parent;     /* gewählter Ordner, NULL = Standard */
static char *g_data_problem;    /* gewählter Ordner war beim Start nicht erreichbar */

static char *pointer_path(void)
{
    return g_build_filename(g_get_user_config_dir(), POINTER_DIR, POINTER_FILE, NULL);
}

static char *legacy_app_dir(void)
{
    return g_build_filename(g_get_home_dir(), LEGACY_APP_DIR, NULL);
}

char *md_default_data_dir(void)
{
#ifdef G_OS_WIN32
    char *install = g_win32_get_package_installation_directory_of_module(NULL);
    if (install) {
        char *dir = g_build_filename(install, "Daten", NULL);
        g_free(install);
        return dir;
    }
#endif
    return legacy_app_dir();
}

const char *md_data_parent(void)
{
    return g_atomic_pointer_get(&g_data_parent);
}

const char *md_data_problem(void)
{
    return g_data_problem;
}

static char *data_dir_for(const char *parent)
{
    return parent && *parent ? g_build_filename(parent, DATA_DIR_NAME, NULL)
                             : md_default_data_dir();
}

/* Datei kopieren, samt Änderungszeit (der Zwischenspeicher prüft daran, wie alt etwas ist) */
static bool copy_file(const char *from, const char *to)
{
    GStatBuf st;
    FILE *in = g_fopen(from, "rb");
    char *tmp = g_strconcat(to, ".part", NULL);
    FILE *out = in ? g_fopen(tmp, "wb") : NULL;
    bool ok = in && out && g_stat(from, &st) == 0;
    char buffer[65536];
    size_t n;
    while (ok && (n = fread(buffer, 1, sizeof buffer, in)) > 0) ok = fwrite(buffer, 1, n, out) == n;
    ok &= in && !ferror(in);
    if (in) fclose(in);
    if (out) ok &= fclose(out) == 0;
    if (ok) {
        struct utimbuf times = {st.st_atime, st.st_mtime};
        g_utime(tmp, &times);
        ok = g_rename(tmp, to) == 0;
    }
    if (!ok) g_unlink(tmp);
    g_free(tmp);
    return ok;
}

/* Datei oder Ordner verschieben, auch auf ein anderes Laufwerk. Was im Ziel schon liegt, bleibt
 * (es ist neuer); die Quelle wird dann gelöscht. -> false, wenn etwas nicht ging */
static bool move_tree(const char *from, const char *to)
{
    if (!g_file_test(from, G_FILE_TEST_EXISTS)) return true;
    if (g_file_test(from, G_FILE_TEST_IS_DIR)) {
        g_mkdir_with_parents(to, 0755);
        GDir *dir = g_dir_open(from, 0, NULL);
        bool ok = dir != NULL;
        const char *name;
        while (dir && (name = g_dir_read_name(dir))) {
            char *a = g_build_filename(from, name, NULL), *b = g_build_filename(to, name, NULL);
            ok &= move_tree(a, b);
            g_free(a);
            g_free(b);
        }
        if (dir) g_dir_close(dir);
        g_rmdir(from);
        return ok;
    }
    if (g_file_test(to, G_FILE_TEST_EXISTS)) return g_unlink(from) == 0;
    if (g_rename(from, to) == 0) return true;
    return copy_file(from, to) && g_unlink(from) == 0;
}

/* Liegt in dir etwas vom Programm (Einstellungen, Verlauf …)? */
static bool has_app_files(const char *dir)
{
    static const char *const FILES[] = {"einstellungen.ini", "verlauf.tsv", "warteschlange.tsv"};
    bool found = false;
    for (size_t i = 0; i < G_N_ELEMENTS(FILES) && !found; i++) {
        char *path = g_build_filename(dir, FILES[i], NULL);
        found = g_file_test(path, G_FILE_TEST_EXISTS);
        g_free(path);
    }
    return found;
}

/* Name einer Datei des Zwischenspeichers (Prüfsumme plus Endung, IMDb-Daten)? Nur solche nimmt
 * md_adopt_cache aus einem fremden Ordner mit. */
static bool is_cache_name(const char *name)
{
    static GRegex *pattern;
    if (g_once_init_enter(&pattern)) {
        GRegex *re = g_regex_new("^([0-9a-f]{40}\\.(info|api|suche|jpg|bild\\.jpg|keinbild|imdb)"
                                 "(\\.part)?|imdb-(bewertungen|kategorien)\\.tsv)$", 0, 0, NULL);
        g_once_init_leave(&pattern, re);
    }
    return g_regex_match(pattern, name, 0, NULL);
}

static gpointer adopt_cache_thread(gpointer data)
{
    char **dirs = data;         /* alter Ordner, neuer Ordner */
    g_mkdir_with_parents(dirs[1], 0755);
    GDir *dir = g_dir_open(dirs[0], 0, NULL);
    const char *name;
    int moved = 0;
    while (dir && (name = g_dir_read_name(dir))) {
        if (!is_cache_name(name)) continue;
        char *a = g_build_filename(dirs[0], name, NULL), *b = g_build_filename(dirs[1], name, NULL);
        moved += move_tree(a, b);
        g_free(a);
        g_free(b);
    }
    if (dir) g_dir_close(dir);
    if (moved) md_log("Zwischenspeicher umgezogen: %d Dateien aus %s", moved, dirs[0]);
    g_rmdir(dirs[0]);           /* nur, wenn er jetzt leer ist */
    g_strfreev(dirs);
    return NULL;
}

void md_adopt_cache(const char *old_dir)
{
    char *now = cache_dir();
    if (old_dir && *old_dir && strcmp(old_dir, now) && g_file_test(old_dir, G_FILE_TEST_IS_DIR)) {
        char **dirs = g_new0(char *, 3);
        dirs[0] = g_strdup(old_dir);
        dirs[1] = g_strdup(now);
        g_thread_unref(g_thread_new("zwischenspeicher", adopt_cache_thread, dirs));
    }
    g_free(now);
}

static gpointer move_cache_thread(gpointer data)
{
    char **dirs = data;
    if (!move_tree(dirs[0], dirs[1])) md_log("Zwischenspeicher nicht ganz umgezogen: %s", dirs[0]);
    char *old_data = g_path_get_dirname(dirs[0]);
    g_rmdir(old_data);          /* der alte Datenordner, nur wenn er jetzt leer ist */
    g_free(old_data);
    g_strfreev(dirs);
    return NULL;
}

/* Pfade umstellen (die alten Zeichenketten bleiben gültig, siehe md_app_dir; die Liste hält
 * sie erreichbar, damit sie nicht als Leck gelten) */
static GPtrArray *g_old_paths;

static void replace_path(char **slot, char *value)
{
    char *old = g_atomic_pointer_exchange(slot, value);
    if (!old) return;
    if (!g_old_paths) g_old_paths = g_ptr_array_new();
    g_ptr_array_add(g_old_paths, old);
}

static void use_data_dir(const char *parent, const char *dir)
{
    replace_path(&g_data_parent, parent && *parent ? g_strdup(parent) : NULL);
    replace_path(&g_app_dir, g_strdup(dir));
    replace_path(&g_log_path, g_build_filename(dir, "protokoll.log", NULL));
}

bool md_set_data_parent(const char *parent, char **error)
{
    if (parent && !*parent) parent = NULL;
    char *from = g_strdup(md_app_dir()), *to = data_dir_for(parent);
    if (!strcmp(from, to)) {
        g_free(from);
        g_free(to);
        return true;
    }
    if (g_str_has_prefix(to, from) && to[strlen(from)] == G_DIR_SEPARATOR) {
        *error = g_strdup("Der neue Ordner liegt im bisherigen Datenordner.");
        g_free(from);
        g_free(to);
        return false;
    }
    if (g_mkdir_with_parents(to, 0755) != 0) {
        *error = g_strdup_printf("%s lässt sich nicht anlegen: %s", to, g_strerror(errno));
        g_free(from);
        g_free(to);
        return false;
    }
    /* erst alles außer dem Zwischenspeicher, damit Verlauf und Warteschlange nicht in zwei
     * Ordnern weiterwachsen; der Zwischenspeicher kann groß sein und folgt im Hintergrund */
    g_mutex_lock(&q.lock);
    g_mutex_lock(&g_log_lock);
    bool ok = true;
    GDir *dir = g_dir_open(from, 0, NULL);
    const char *name;
    while (dir && (name = g_dir_read_name(dir))) {
        if (!strcmp(name, CACHE_DIR_NAME)) continue;
        char *a = g_build_filename(from, name, NULL), *b = g_build_filename(to, name, NULL);
        ok &= move_tree(a, b);
        g_free(a);
        g_free(b);
    }
    if (dir) g_dir_close(dir);
    use_data_dir(parent, to);
    g_mutex_unlock(&g_log_lock);
    g_mutex_unlock(&q.lock);

    char **dirs = g_new0(char *, 3);
    dirs[0] = g_build_filename(from, CACHE_DIR_NAME, NULL);
    dirs[1] = g_build_filename(to, CACHE_DIR_NAME, NULL);
    g_thread_unref(g_thread_new("zwischenspeicher", move_cache_thread, dirs));

    char *pointer = pointer_path();
    if (parent) {
        char *pointer_dir = g_path_get_dirname(pointer);
        g_mkdir_with_parents(pointer_dir, 0755);
        g_free(pointer_dir);
        ok &= g_file_set_contents(pointer, parent, -1, NULL);
    } else {
        g_unlink(pointer);
    }
    g_free(pointer);
    g_clear_pointer(&g_data_problem, g_free);
    md_log("Datenordner umgezogen: %s -> %s", from, to);
    if (!ok) *error = g_strdup("Nicht alle Dateien ließen sich verschieben.");
    g_free(from);
    g_free(to);
    return ok;
}

/* Datenordner beim Start festlegen; Daten älterer Versionen ziehen mit */
static void choose_data_dir(void)
{
    char *pointer = pointer_path(), *parent = NULL;
    if (g_file_get_contents(pointer, &parent, NULL, NULL)) g_strstrip(parent);
    g_free(pointer);
    if (parent && !*parent) g_clear_pointer(&parent, g_free);
    char *dir = data_dir_for(parent);
    if (parent && g_mkdir_with_parents(dir, 0755) != 0) {
        /* etwa ein Netzlaufwerk, das noch nicht verbunden ist: nichts überschreiben */
        g_data_problem = g_strdup_printf("Der Datenordner %s ist nicht erreichbar. Bis er wieder "
                                         "da ist, gilt der Standardordner.", dir);
        g_free(dir);
        g_clear_pointer(&parent, g_free);
        dir = md_default_data_dir();
    }
    /* Bis Version 1.2 hieß das Programm ZDF-Downloader */
    char *legacy = legacy_app_dir(), *oldest = g_build_filename(g_get_home_dir(), OLD_APP_DIR, NULL);
    if (g_file_test(oldest, G_FILE_TEST_IS_DIR) && !g_file_test(legacy, G_FILE_TEST_EXISTS))
        g_rename(oldest, legacy);
    g_free(oldest);
    /* bis 2.2 lag alles in ~/.mediathek-downloader */
    if (strcmp(legacy, dir) && has_app_files(legacy) && !has_app_files(dir) &&
        !g_data_problem) {
        g_mkdir_with_parents(dir, 0755);
        move_tree(legacy, dir);
    }
    g_free(legacy);
    g_mkdir_with_parents(dir, 0755);
    char *old_cache = g_build_filename(dir, OLD_CACHE_DIR_NAME, NULL);
    char *cache = g_build_filename(dir, CACHE_DIR_NAME, NULL);
    if (g_file_test(old_cache, G_FILE_TEST_IS_DIR) && !g_file_test(cache, G_FILE_TEST_EXISTS))
        g_rename(old_cache, cache);
    g_free(old_cache);
    g_free(cache);
    use_data_dir(parent, dir);
    g_free(parent);
    g_free(dir);
}

void md_init(bool persist)
{
    platform_init();
    choose_data_dir();
    q.jobs = g_ptr_array_new();
    q.persist = persist;
    q.max_downloads = 2;
    q.max_conversions = 1;
    history_load();
    if (persist) queue_load();
    q.dispatcher = g_thread_new("verteiler", dispatcher_thread, NULL);
}

void md_shutdown(void)
{
    g_mutex_lock(&q.lock);
    q.quitting = true;
    /* Downloads gelten danach als pausiert, damit sie nicht von selbst weiterlaufen */
    for (guint i = 0; i < q.jobs->len; i++) {
        Job *j = q.jobs->pdata[i];
        if (j->state == MD_JOB_DOWNLOADING) j->paused = true;
    }
    g_cond_broadcast(&q.wake);
    while (q.workers > 0) g_cond_wait(&q.worker_done, &q.lock);
    g_mutex_unlock(&q.lock);
    g_thread_join(q.dispatcher);
    if (q.persist) {
        g_mutex_lock(&q.lock);
        GString *text = queue_text();
        g_mutex_unlock(&q.lock);
        queue_write(text);
    }
}

bool md_have_tools(char **missing)
{
    const char *tools[] = {"curl", "ffmpeg", "ffprobe"};
    for (size_t i = 0; i < G_N_ELEMENTS(tools); i++) {
        char *path = g_find_program_in_path(tools[i]);
        bool found = path != NULL;
        g_free(path);
        if (!found) {
            if (missing) *missing = g_strdup(tools[i]);
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------
 * Updates
 * Neue Versionen kommen aus den Releases auf GitHub: unter Linux das .deb, unter Windows der
 * Installer. SHA256SUMS am selben Release schützt vor kaputten Downloads.
 * ------------------------------------------------------------------------- */
#define UPDATE_API "https://api.github.com/repos/EureLordschaft008/mediathek-downloader/" \
                   "releases/latest"
#define UPDATE_DOWNLOADS "https://github.com/EureLordschaft008/mediathek-downloader/" \
                         "releases/download/"
#define UPDATE_PAGES "https://github.com/EureLordschaft008/mediathek-downloader/releases/"
#ifdef G_OS_WIN32
#define UPDATE_ASSET_SUFFIX "-setup.exe"
#else
#define UPDATE_ASSET_SUFFIX "_amd64.deb"
#endif

/* "v1.10.2" -> Zahlen; fehlende Stellen sind 0 */
static void parse_version(const char *text, int parts[3])
{
    parts[0] = parts[1] = parts[2] = 0;
    if (text && (*text == 'v' || *text == 'V')) text++;
    for (int i = 0; i < 3 && text && g_ascii_isdigit(*text); i++) {
        parts[i] = (int)g_ascii_strtoll(text, (char **)&text, 10);
        if (*text != '.') break;
        text++;
    }
}

bool md_version_newer(const char *candidate, const char *current)
{
    int a[3], b[3];
    parse_version(candidate, a);
    parse_version(current, b);
    for (int i = 0; i < 3; i++)
        if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

void md_update_free(MdUpdate *u)
{
    if (!u) return;
    g_free(u->version);
    g_free(u->notes);
    g_free(u->page);
    g_free(u->file_name);
    g_free(u->file_url);
    g_free(u->sums_url);
    g_free(u);
}

MdUpdate *md_update_check(char **error)
{
    *error = NULL;
    char *argv[] = {CURL_CMD, "-sS", "-f", "-L", "-m", "30", "-A", USER_AGENT, "-H",
                    "Accept: application/vnd.github+json", "--url", UPDATE_API, NULL};
    char *reply = NULL, *messages = NULL;
    int rc = run_capture(argv, &reply, &messages);
    JsonNode *root = rc == 0 ? json_tree(reply) : NULL;
    g_free(reply);
    if (!root) {
        *error = g_strdup_printf("GitHub ist nicht erreichbar (%s).",
                                 rc < 0 ? "curl fehlt" : messages && *messages
                                 ? g_strstrip(messages) : "keine gültige Antwort");
        g_free(messages);
        return NULL;
    }
    g_free(messages);
    MdUpdate *u = NULL;
    const char *tag = json_text_at(root, "tag_name");
    if (tag && md_version_newer(tag, MD_VERSION)) {
        u = g_new0(MdUpdate, 1);
        u->version = g_strdup(*tag == 'v' ? tag + 1 : tag);
        u->notes = g_strdup(json_text_at(root, "body") ? json_text_at(root, "body") : "");
        const char *page = json_text_at(root, "html_url");
        u->page = g_strdup(page && g_str_has_prefix(page, UPDATE_PAGES) ? page : UPDATE_PAGES);
        const JsonNode *assets = json_at(root, "assets");
        for (guint i = 0; i < json_count(assets); i++) {
            const JsonNode *a = assets->items->pdata[i];
            const char *name = json_text_at(a, "name");
            const char *url = json_text_at(a, "browser_download_url");
            /* nur Dateien aus diesem Repo, und keine Namen mit Pfadteilen */
            if (!name || !url || !g_str_has_prefix(url, UPDATE_DOWNLOADS) || strchr(name, '/')
                || strchr(name, '\\'))
                continue;
            if (!strcmp(name, "SHA256SUMS")) {
                g_free(u->sums_url);
                u->sums_url = g_strdup(url);
            } else if (g_str_has_suffix(name, UPDATE_ASSET_SUFFIX) && !u->file_url) {
                u->file_name = g_strdup(name);
                u->file_url = g_strdup(url);
                u->file_size = (gint64)json_number_at(a, "size", 0);
            }
        }
    }
    json_node_free(root);
    return u;
}

/* SHA256 einer Datei als Hex-Text, NULL = nicht lesbar */
static char *file_sha256(const char *path)
{
    FILE *f = g_fopen(path, "rb");
    if (!f) return NULL;
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);
    guchar chunk[65536];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) g_checksum_update(sum, chunk, n);
    bool ok = !ferror(f);
    fclose(f);
    char *hex = ok ? g_strdup(g_checksum_get_string(sum)) : NULL;
    g_checksum_free(sum);
    return hex;
}

/* Prüfsumme zu name aus dem Inhalt von SHA256SUMS ("<hex>  <name>"), NULL = fehlt */
static char *listed_sha256(const char *sums, const char *name)
{
    char *found = NULL;
    char **lines = g_strsplit(sums, "\n", -1);
    for (int i = 0; lines[i] && !found; i++) {
        char *line = g_strstrip(lines[i]);
        char *space = strchr(line, ' ');
        if (!space || space - line != 64) continue;
        const char *file = space;
        while (*file == ' ' || *file == '*') file++;
        if (!strcmp(file, name)) found = g_ascii_strdown(line, 64);
    }
    g_strfreev(lines);
    return found;
}

char *md_update_download(const MdUpdate *u, char **error)
{
    *error = NULL;
    if (!u->file_url || !u->sums_url) {
        *error = g_strdup(!u->file_url
                          ? "Für dieses System liegt der neuen Version keine Datei bei."
                          : "Der neuen Version liegt keine Prüfsumme bei.");
        return NULL;
    }
    char *dir = g_build_filename(md_app_dir(), "updates", NULL);
    g_mkdir_with_parents(dir, 0700);
    char *path = g_build_filename(dir, u->file_name, NULL);
    char *part = g_strconcat(path, ".part", NULL);
    g_free(dir);
    g_unlink(part);
    char *fetch[] = {CURL_CMD, "-sS", "-f", "-L", "-m", "1800", "-A", USER_AGENT, "-o", part,
                     "--url", u->file_url, NULL};
    char *sums_args[] = {CURL_CMD, "-sS", "-f", "-L", "-m", "30", "-A", USER_AGENT, "--url",
                         u->sums_url, NULL};
    char *sums = NULL, *expected = NULL, *actual = NULL;
    if (run_capture(fetch, NULL, NULL) != 0)
        *error = g_strdup("Das Update ließ sich nicht laden.");
    else if (run_capture(sums_args, &sums, NULL) != 0 || !sums
             || !(expected = listed_sha256(sums, u->file_name)))
        *error = g_strdup("Die Prüfsumme des Updates fehlt.");
    else if (!(actual = file_sha256(part)) || strcmp(actual, expected))
        *error = g_strdup("Das geladene Update ist beschädigt (Prüfsumme stimmt nicht).");
    else if (g_rename(part, path) != 0 && (g_unlink(path), g_rename(part, path) != 0))
        *error = g_strdup("Das Update ließ sich nicht speichern.");
    g_free(sums);
    g_free(expected);
    g_free(actual);
    if (*error) {
        g_unlink(part);
        g_free(part);
        g_free(path);
        md_log("Update: %s", *error);
        return NULL;
    }
    g_free(part);
    md_log("Update %s geladen und geprüft: %s", u->version, path);
    return path;
}

bool md_launch_detached(char **argv, char **error)
{
#ifdef G_OS_WIN32
    /* eigene Befehlszeile: das Programm wird gleich beendet, das Kind muss weiterlaufen */
    GString *line = g_string_new(NULL);
    for (int i = 0; argv[i]; i++) {
        if (i) g_string_append_c(line, ' ');
        g_string_append_printf(line, "\"%s\"", argv[i]);
    }
    wchar_t *wide = g_utf8_to_utf16(line->str, -1, NULL, NULL, NULL);
    g_string_free(line, TRUE);
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    bool ok = wide && CreateProcessW(NULL, wide, NULL, NULL, FALSE, DETACHED_PROCESS, NULL, NULL,
                                     &si, &pi);
    g_free(wide);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else if (error) {
        char *message = g_win32_error_message((int)GetLastError());
        *error = g_strdup_printf("Konnte nicht gestartet werden: %s", message);
        g_free(message);
    }
    return ok;
#else
    GError *err = NULL;
    bool ok = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err);
    if (!ok && error) *error = g_strdup(err->message);
    g_clear_error(&err);
    return ok;
#endif
}

/* ---------------------------------------------------------------------------
 * Autostart
 * Mit --autostart startet das Programm minimiert. Ob es eingetragen ist, steht nur im System,
 * nicht in den Einstellungen; so gilt auch, was der Nutzer dort selbst ändert.
 * ------------------------------------------------------------------------- */
#ifdef G_OS_WIN32
#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE L"Mediathek Downloader"

bool md_autostart_enabled(void)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    bool found = RegQueryValueExW(key, RUN_VALUE, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
    RegCloseKey(key);
    return found;
}

bool md_autostart_set(bool enabled, char **error)
{
    HKEY key;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &key,
                              NULL);
    if (rc == ERROR_SUCCESS) {
        if (enabled) {
            wchar_t exe[MAX_PATH], line[MAX_PATH + 32];
            DWORD length = GetModuleFileNameW(NULL, exe, MAX_PATH);
            if (length == 0 || length >= MAX_PATH) rc = ERROR_FILENAME_EXCED_RANGE;
            else {
                _snwprintf(line, G_N_ELEMENTS(line), L"\"%ls\" --autostart", exe);
                line[G_N_ELEMENTS(line) - 1] = 0;
                rc = RegSetValueExW(key, RUN_VALUE, 0, REG_SZ, (const BYTE *)line,
                                    (DWORD)((wcslen(line) + 1) * sizeof(wchar_t)));
            }
        } else {
            rc = RegDeleteValueW(key, RUN_VALUE);
            if (rc == ERROR_FILE_NOT_FOUND) rc = ERROR_SUCCESS;
        }
        RegCloseKey(key);
    }
    if (rc != ERROR_SUCCESS && error) {
        char *message = g_win32_error_message((int)rc);
        *error = g_strdup_printf("Autostart lässt sich nicht ändern: %s", message);
        g_free(message);
    }
    return rc == ERROR_SUCCESS;
}
#else
static char *autostart_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "autostart", "mediathek-downloader.desktop",
                            NULL);
}

bool md_autostart_enabled(void)
{
    char *path = autostart_path();
    bool found = g_file_test(path, G_FILE_TEST_IS_REGULAR);
    g_free(path);
    return found;
}

bool md_autostart_set(bool enabled, char **error)
{
    static const char ENTRY[] = "[Desktop Entry]\n"
                                "Type=Application\n"
                                "Name=Mediathek Downloader\n"
                                "Exec=mediathek-downloader --autostart\n"
                                "Icon=mediathek-downloader\n"
                                "Terminal=false\n"
                                "X-GNOME-Autostart-enabled=true\n";
    char *path = autostart_path(), *dir = g_path_get_dirname(path);
    GError *err = NULL;
    bool ok;
    if (enabled) {
        ok = g_mkdir_with_parents(dir, 0755) == 0 &&
             g_file_set_contents(path, ENTRY, -1, &err);
    } else {
        ok = g_unlink(path) == 0 || errno == ENOENT;
    }
    if (!ok && error)
        *error = g_strdup_printf("Autostart lässt sich nicht ändern: %s",
                                 err ? err->message : g_strerror(errno));
    g_clear_error(&err);
    g_free(path);
    g_free(dir);
    return ok;
}
#endif

/* Steht label in der NULL-terminierten Liste? */
static bool label_listed(const char *const *list, const char *label)
{
    return list && g_strv_contains(list, label);
}

static Extra *extra_new(bool subtitle, const MdTrack *track)
{
    Extra *e = g_new0(Extra, 1);
    e->subtitle = subtitle;
    e->label = g_strdup(track->label);
    e->lang = g_strdup(track->lang);
    e->url = g_strdup(track->url);
    return e;
}

guint md_queue_add(const MdTitle *t, const MdJobOptions *o)
{
    char *target = md_target_path(t, o->dir);
    g_mutex_lock(&q.lock);
    for (guint i = 0; i < q.jobs->len; i++) {
        Job *other = q.jobs->pdata[i];
        if (!strcmp(other->target, target) && other->state != MD_JOB_DONE) {
            g_mutex_unlock(&q.lock);
            g_free(target);
            return 0;
        }
    }
    Job *j = g_new0(Job, 1);
    j->id = ++q.next_id;
    j->title = t->season >= 0 ? g_strdup_printf("%s – S%02dE%02d – %s", t->topic, t->season,
                                                t->episode, t->name)
                              : g_strdup(t->name);
    j->url = g_strdup(md_title_url(t, o->quality));
    j->target = target;
    j->key = title_key(t);
    j->topic = g_strdup(t->is_film ? "Filme" : t->topic);
    j->season = t->season;
    j->episode = t->episode;
    const MdTrack *main_audio = t->audio->pdata[0];
    j->main_label = g_strdup(main_audio->label);
    j->main_lang = g_strdup(main_audio->lang);
    /* Ein Titel, der keine der gewählten Sprachen hat, behält seinen Originalton */
    bool other_audio = false;
    for (guint i = 1; i < t->audio->len; i++)
        other_audio |= !label_listed(o->skip_audio, ((MdTrack *)t->audio->pdata[i])->label);
    j->skip_main_audio = other_audio && label_listed(o->skip_audio, main_audio->label);
    j->extras = g_ptr_array_new_with_free_func(extra_free);
    for (guint i = 1; i < t->audio->len; i++) {
        const MdTrack *track = t->audio->pdata[i];
        if (!label_listed(o->skip_audio, track->label))
            g_ptr_array_add(j->extras, extra_new(false, track));
    }
    for (guint i = 0; i < t->subtitles->len; i++) {
        const MdTrack *track = t->subtitles->pdata[i];
        if (!label_listed(o->skip_subtitles, track->label))
            g_ptr_array_add(j->extras, extra_new(true, track));
    }
    j->convert = o->convert;
    j->keep_original = o->keep_original;
    j->encoder = o->encoder;
    j->quality = o->quality;
    j->gpu_quality = o->gpu_quality;
    j->cpu_rf = o->cpu_rf;
    job_set_state(j, MD_JOB_WAITING);
    j->remaining = -1;
    g_ptr_array_add(q.jobs, j);
    guint id = j->id;
    queue_save();
    g_cond_signal(&q.wake);
    g_mutex_unlock(&q.lock);
    return id;
}

guint md_queue_convert(const char *path, const MdJobOptions *o)
{
    g_mutex_lock(&q.lock);
    const History *h = g_hash_table_find(q.history, history_has_path, (gpointer)path);
    char *stem = path_stem(path);
    bool busy = !h;
    for (guint i = 0; i < q.jobs->len && !busy; i++) {
        const Job *other = q.jobs->pdata[i];
        char *other_stem = path_stem(other->target);
        busy = other->state != MD_JOB_DONE && other->state != MD_JOB_FAILED &&
               !strcmp(other_stem, stem);
        g_free(other_stem);
    }
    g_free(stem);
    if (busy) {
        g_mutex_unlock(&q.lock);
        return 0;
    }
    Job *j = g_new0(Job, 1);
    j->id = ++q.next_id;
    j->title = g_strdup(h->e.title);
    j->url = g_strdup("");
    j->target = g_strdup(path);
    j->key = g_strdup(h->key);
    j->topic = g_strdup(h->e.topic);
    j->season = h->e.season;
    j->episode = h->e.episode;
    j->main_label = g_strdup("");
    j->main_lang = g_strdup("");
    j->extras = g_ptr_array_new_with_free_func(extra_free);
    j->convert = true;
    j->keep_original = o->keep_original;
    j->encoder = o->encoder;
    j->quality = o->quality;
    j->gpu_quality = o->gpu_quality;
    j->cpu_rf = o->cpu_rf;
    j->total = j->have = MAX(file_size(path), 0);
    job_set_state(j, MD_JOB_CONVERT_WAITING);
    j->remaining = -1;
    g_ptr_array_add(q.jobs, j);
    guint id = j->id;
    queue_save();
    g_cond_signal(&q.wake);
    g_mutex_unlock(&q.lock);
    return id;
}

MdJobView *md_queue_snapshot(int *count)
{
    g_mutex_lock(&q.lock);
    MdJobView *views = g_new0(MdJobView, MAX(q.jobs->len, 1));
    for (guint i = 0; i < q.jobs->len; i++) {
        Job *j = q.jobs->pdata[i];
        views[i] = (MdJobView){j->id, j->state, j->paused, j->merging, g_strdup(j->title),
                               g_strdup(j->target), g_strdup(j->message), g_strdup(j->key),
                               j->have, j->total, j->speed, j->progress, j->remaining};
    }
    *count = (int)q.jobs->len;
    g_mutex_unlock(&q.lock);
    return views;
}

void md_queue_snapshot_free(MdJobView *views, int count)
{
    for (int i = 0; i < count; i++) {
        g_free(views[i].title);
        g_free(views[i].target);
        g_free(views[i].message);
        g_free(views[i].key);
    }
    g_free(views);
}

/* Pausenzustand eines Auftrags setzen (q.lock gehalten) */
static void job_set_paused(Job *j, bool paused)
{
    if (j->state == MD_JOB_DONE || j->state == MD_JOB_FAILED) return;
    j->paused = paused;
    if (paused && j->state == MD_JOB_DOWNLOADING) j->speed = 0;
}

void md_job_set_paused(guint id, bool paused)
{
    g_mutex_lock(&q.lock);
    Job *j = job_by_id(id);
    if (j) job_set_paused(j, paused);
    queue_save();
    g_cond_signal(&q.wake);
    g_mutex_unlock(&q.lock);
}

void md_queue_set_paused(bool paused)
{
    g_mutex_lock(&q.lock);
    for (guint i = 0; i < q.jobs->len; i++) job_set_paused(q.jobs->pdata[i], paused);
    if (!paused) q.downloads_held = q.conversions_held = false;
    queue_save();
    g_cond_signal(&q.wake);
    g_mutex_unlock(&q.lock);
}

void md_queue_hold(bool downloads, bool conversions)
{
    g_mutex_lock(&q.lock);
    q.downloads_held = downloads;
    q.conversions_held = conversions;
    g_cond_signal(&q.wake);
    g_mutex_unlock(&q.lock);
}

void md_queue_holds(bool *downloads, bool *conversions)
{
    g_mutex_lock(&q.lock);
    *downloads = q.downloads_held;
    *conversions = q.conversions_held;
    g_mutex_unlock(&q.lock);
}

void md_job_retry(guint id)
{
    g_mutex_lock(&q.lock);
    Job *j = job_by_id(id);
    if (j && j->state == MD_JOB_FAILED) {
        /* fertige Dateien überspringt der Download von selbst */
        job_set_state(j, is_reconvert(j) ? MD_JOB_CONVERT_WAITING : MD_JOB_WAITING);
        j->paused = false;
        g_clear_pointer(&j->message, g_free);
        queue_save();
        g_cond_signal(&q.wake);
    }
    g_mutex_unlock(&q.lock);
}

void md_job_remove(guint id)
{
    g_mutex_lock(&q.lock);
    Job *j = job_by_id(id);
    if (j) {
        j->remove_requested = true;
        if (!j->worker) {
            remove_partial_files(j);
            job_discard(j);
        }
        queue_save();
    }
    g_mutex_unlock(&q.lock);
}

void md_queue_remove_finished(void)
{
    g_mutex_lock(&q.lock);
    for (guint i = q.jobs->len; i-- > 0;) {
        Job *j = q.jobs->pdata[i];
        if (j->state == MD_JOB_DONE && !j->worker) job_discard(j);
    }
    queue_save();
    g_mutex_unlock(&q.lock);
}

gint64 md_job_partial_bytes(guint id)
{
    g_mutex_lock(&q.lock);
    Job *j = job_by_id(id);
    gint64 bytes = j ? partial_bytes(j) : 0;
    g_mutex_unlock(&q.lock);
    return bytes;
}

gint64 md_queue_failed_bytes(int *count)
{
    gint64 bytes = 0;
    int n = 0;
    g_mutex_lock(&q.lock);
    for (guint i = 0; i < q.jobs->len; i++) {
        const Job *j = q.jobs->pdata[i];
        if (j->state != MD_JOB_FAILED || j->worker) continue;
        bytes += partial_bytes(j);
        n++;
    }
    g_mutex_unlock(&q.lock);
    if (count) *count = n;
    return bytes;
}

void md_queue_remove_failed(void)
{
    bool removed = false;
    g_mutex_lock(&q.lock);
    for (guint i = q.jobs->len; i-- > 0;) {
        Job *j = q.jobs->pdata[i];
        if (j->state != MD_JOB_FAILED || j->worker) continue;
        remove_partial_files(j);
        job_discard(j);
        removed = true;
    }
    if (removed) queue_save();
    g_mutex_unlock(&q.lock);
}

void md_set_rate_limit(gint64 bytes_per_second)
{
    g_mutex_lock(&q.lock);
    q.rate_limit = MAX(bytes_per_second, 0);
    g_mutex_unlock(&q.lock);
}

void md_set_encoder(MdEncoder encoder, int gpu_quality, int cpu_rf)
{
    g_mutex_lock(&q.lock);
    bool changed = false;
    for (guint i = 0; i < q.jobs->len; i++) {
        Job *j = q.jobs->pdata[i];
        if (j->state == MD_JOB_CONVERTING || j->state == MD_JOB_DONE) continue;
        changed |= j->encoder != encoder || j->gpu_quality != gpu_quality || j->cpu_rf != cpu_rf;
        j->encoder = encoder;
        j->gpu_quality = gpu_quality;
        j->cpu_rf = cpu_rf;
    }
    if (changed) queue_save();
    g_mutex_unlock(&q.lock);
}

void md_set_parallel(int downloads, int conversions)
{
    g_mutex_lock(&q.lock);
    q.max_downloads = CLAMP(downloads, 1, MD_MAX_DOWNLOADS);
    q.max_conversions = CLAMP(conversions, 1, MD_MAX_CONVERSIONS);
    g_cond_signal(&q.wake);
    g_mutex_unlock(&q.lock);
}
