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
 *     (Qualität 64, 10 Bit), CPU per SVT-AV1 (RF 35). Der Ton wird unverändert
 *     übernommen. Eine pausierte Umwandlung wird per SIGSTOP angehalten.
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
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "md_core.h"

/* ---------------------------------------------------------------------------
 * Konfiguration
 * ------------------------------------------------------------------------- */
#define API_URL "https://mediathekviewweb.de/api/query"
#define USER_AGENT "mediathek-downloader/" MD_VERSION
#define COVER_PAGE_PREFIX "https://www.zdf.de/"
#define COVER_URL_PREFIX "https://www.zdf.de/assets/"
#define COVER_EPG_PREFIX "https://epg-image.zdf.de/"
#define PAGE_RECHECK_SECONDS (7 * 24 * 3600)    /* Seite ohne Bild/Kategorie neu prüfen */
#define COVER_SIZE "384x216"
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
#define MIN_FREE_SPACE (512 * 1024 * 1024LL)    /* Reserve auf dem Ziellaufwerk */
#define NAME_MAX_BYTES 180
#define ENCODER_NICE 5
#define SVT_PRESET "6"
#define GOP_FRAMES "500"            /* Keyframe alle 10 s bei 50 Bildern/s */
#define QUEUE_HEADER "#mediathek-downloader-warteschlange 1"
#define QUEUE_HEADER_OLD "#zdf-downloader-warteschlange 1"
#define OLD_APP_DIR ".zdf-downloader"
#define HISTORY_FILE "verlauf.tsv"
#define AUDIO_BYTES_PER_SECOND 16000    /* Zusatzspur: AAC 128 kbit/s */
#define VARIANT_MAX_DIFFERENCE 30       /* Sekunden: so nah muss die Länge der Fassungen sein */

/* Sprachfassungen, die das ZDF am Titelende in Klammern nennt */
static const struct {
    const char *tag, *label, *lang;
} VARIANTS[] = {
    {"(Audiodeskription)", "Audiodeskription", "deu"}, {"(Hörfassung)", "Audiodeskription", "deu"},
    {"(Englisch)", "Englisch", "eng"}, {"(Französisch)", "Französisch", "fra"},
    {"(Spanisch)", "Spanisch", "spa"}, {"(Italienisch)", "Italienisch", "ita"},
    {"(Dänisch)", "Dänisch", "dan"}, {"(Schwedisch)", "Schwedisch", "swe"},
    {"(Norwegisch)", "Norwegisch", "nor"}, {"(Originalfassung)", "Originalfassung", "und"},
    {"(Originalversion)", "Originalfassung", "und"},
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
};

/* Netzkino steht nicht in der Suche: Filmlisten der eigenen Schnittstelle, ohne NetzkinoPlus */
#define NETZKINO_API "https://api.netzkino.de.simplecache.net/capi-2.0a/categories/"
#define NETZKINO_VIDEO "https://pmd.netzkino-seite.netzkino.de/"
#define NETZKINO_PAGE "https://www.netzkino.de/details/"
static const int NETZKINO_CATEGORIES[] = {1, 3, 4, 5, 6, 10, 18, 32, 35, 51, 71, 10333};

/* Mediatheken, deren Titel gleich aufgebaut sind */
#define ZDF_STYLE (MD_SOURCE_ZDF | MD_SOURCE_ZDFNEO)
#define ARD_STYLE (MD_SOURCE_ARD | MD_SOURCE_ONE)

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
    GThread *dispatcher;
    GHashTable *history;            /* Schlüssel -> History* */
} q;

static char *g_app_dir, *g_log_path;
static GMutex g_log_lock;

/* ---------------------------------------------------------------------------
 * Kleine Helfer
 * ------------------------------------------------------------------------- */
const char *md_app_dir(void)
{
    return g_app_dir;
}

const char *md_log_path(void)
{
    return g_log_path;
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
    struct stat st;
    return stat(path, &st) == 0 ? (gint64)st.st_size : -1;
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
    FILE *f = fopen(g_log_path, "a");
    if (f) {
        fprintf(f, "%s  %s\n", stamp, text);
        fclose(f);
    }
    g_mutex_unlock(&g_log_lock);
    g_free(text);
}

/* ---------------------------------------------------------------------------
 * Prozesse
 * Kindprozesse enden mit dem Thread, der sie gestartet hat (PR_SET_PDEATHSIG),
 * damit nach einem Absturz kein curl oder ffmpeg weiterläuft.
 * ------------------------------------------------------------------------- */
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

/* Kurzes Werkzeug starten und die Ausgabe einsammeln -> Exit-Code, -1 = nicht startbar */
static int run_capture(char **argv, char **out, char **err)
{
    int status = 0;
    if (!g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, out, err, &status, NULL))
        return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

/* Prozess beenden (auch wenn er angehalten ist) und auf ihn warten */
static void stop_child(pid_t pid)
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

/* Staffel, Folge und bereinigten Namen aus dem Titel ableiten */
static void title_analyse(MdTitle *t)
{
    static GRegex *episode_tag, *new_episodes, *numbered, *show_suffix;
    static gsize once;
    if (g_once_init_enter(&once)) {
        episode_tag = g_regex_new(" ?\\(S(\\d+)/E(\\d+)\\)", 0, 0, NULL);
        new_episodes = g_regex_new(" - Neue Folgen?\\b", 0, 0, NULL);
        numbered = g_regex_new("^Folge (\\d+)\\b|\\((\\d+)\\)$", 0, 0, NULL);
        show_suffix = g_regex_new(" · .*$", 0, 0, NULL);
        g_once_init_leave(&once, 1);
    }
    t->source = source_of(t->channel);
    /* Streams lassen sich nicht wie Dateien laden */
    char **urls[] = {&t->url_high, &t->url_medium, &t->url_low};
    for (size_t i = 0; i < G_N_ELEMENTS(urls); i++)
        if (g_str_has_suffix(*urls[i], ".m3u8")) **urls[i] = 0;
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
    char *without_tag = g_regex_replace_literal(episode_tag, t->title, -1, 0, "", 0, NULL);
    t->name = g_regex_replace_literal(new_episodes, without_tag, -1, 0, "", 0, NULL);
    g_free(without_tag);
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

    t->is_variant = strstr(t->title, "Gebärdensprache") != NULL;
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

static GMutex cover_lock;
static char *cover_dir;         /* NULL = Standardordner */

/* Aktueller Ordner des Zwischenspeichers */
static char *cache_dir(void)
{
    g_mutex_lock(&cover_lock);
    char *dir = cover_dir ? g_strdup(cover_dir) : md_default_cover_dir();
    g_mutex_unlock(&cover_lock);
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

/* Eine Seite der Suche abrufen */
static bool fetch_page(const char *channel, const char *text, const char *field, int min_seconds,
                       int offset, MdCache cache, GPtrArray *out, int *total, char **error)
{
    GString *body = g_string_new(NULL);
    g_string_printf(body, "{\"queries\":[{\"fields\":[\"channel\"],\"query\":\"%s\"}", channel);
    if (text && *text) {
        g_string_append_printf(body, ",{\"fields\":[%s],\"query\":\"", field);
        json_append_escaped(body, text);
        g_string_append(body, "\"}");
    }
    g_string_append_printf(body, "],\"sortBy\":\"timestamp\",\"sortOrder\":\"desc\","
                           "\"future\":false,\"offset\":%d,\"size\":%d", offset, PAGE_SIZE);
    if (min_seconds > 0) g_string_append_printf(body, ",\"duration_min\":%d", min_seconds);
    g_string_append_c(body, '}');
    char *argv[] = {"curl", "-sS", "-f", "-L", "-m", "60", "-A", USER_AGENT, "-H",
                    "Content-Type: text/plain", "--data-binary", body->str, API_URL, NULL};
    char *reply = NULL, *messages = NULL;
    /* Antworten für die Vorschau werden aufgehoben, damit der Start ohne Netz auskommt */
    char *saved = cache != MD_CACHE_OFF ? search_cache_path(body->str) : NULL;
    int rc = 0;
    if (cache != MD_CACHE_USE || !search_cache_read(saved, &reply)) {
        rc = run_capture(argv, &reply, &messages);
        if (saved && rc == 0 && reply) g_file_set_contents(saved, reply, -1, NULL);
    }
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

/* Eine Mediathek abfragen */
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
        char *argv[] = {"curl", "-sS", "-f", "-L", "-m", "60", "-A", USER_AGENT, url, NULL};
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
    for (size_t i = 0; ok && i < G_N_ELEMENTS(SOURCES); i++)
        if ((sources & SOURCES[i].source) && SOURCES[i].query)
            ok = search_source(SOURCES[i].query, query, raw, cancel, truncated, error);
    if (ok && (sources & MD_SOURCE_NETZKINO) && query->kind != MD_KIND_SERIES)
        netzkino_fetch(query, raw);
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
    static const double KBIT_PER_SECOND[] = {6660, 3360, 808};
    return (gint64)(t->duration * KBIT_PER_SECOND[quality] * 1000 / 8);
}

/* ---------------------------------------------------------------------------
 * Pfade
 * ------------------------------------------------------------------------- */
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
    if (length > NAME_MAX_BYTES) {
        const char *end = g_utf8_find_prev_char(name, name + NAME_MAX_BYTES + 1);
        name[end ? end - name : NAME_MAX_BYTES] = 0;
        g_strchomp(name);
    }
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
        file = g_strdup_printf("%s - S%02dE%02d - %s.mp4", topic, t->season, t->episode, name);
        path = g_build_filename(dir, "Serien", topic, season, file, NULL);
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
    h->e.finished = g_get_real_time() / G_USEC_PER_SEC;
    g_free(result);

    GString *line = g_string_new(NULL);
    history_append_line(line, h);
    char *path = history_path();
    FILE *file = fopen(path, "a");
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

MdStatus md_title_status(const MdTitle *t, const char *dir, char **path)
{
    char *target = md_target_path(t, dir), *key = title_key(t), *known = NULL;
    bool queued = false, recorded = false;
    g_mutex_lock(&q.lock);
    for (guint i = 0; i < q.jobs->len && !queued; i++) {
        const Job *j = q.jobs->pdata[i];
        queued = !strcmp(j->key, key) && j->state != MD_JOB_DONE && j->state != MD_JOB_FAILED;
    }
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

/* Adresse des Bildes in der kleinen Größe; NULL = die Seite nennt keines */
static char *cover_url(const char *page)
{
    char *url = text_after(page, "\"thumbnailUrl\":\""), *result = NULL;
    if (!url) url = text_after(page, "property=\"og:image\" content=\"");
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
static char *page_info(const char *website)
{
    if (g_str_has_prefix(website, NETZKINO_PAGE)) {     /* schon mit der Filmliste gespeichert */
        char *path = cache_file(website, ".info"), *saved = NULL;
        g_file_get_contents(path, &saved, NULL, NULL);
        g_free(path);
        return saved;
    }
    bool ard = g_str_has_prefix(website, COVER_ARD_PAGE);
    if (!ard && !g_str_has_prefix(website, COVER_PAGE_PREFIX) &&
        !g_str_has_prefix(website, COVER_3SAT_PAGE))
        return NULL;
    char *path = cache_file(website, ".info"), *info = NULL;
    if (g_file_get_contents(path, &info, NULL, NULL) && !page_info_stale(info, path)) {
        g_free(path);
        return info;
    }
    char *address = page_address(website);
    char *argv[] = {"curl", "-sS", "-f", "-L", "-m", "20", "--retry", "2", "-A", USER_AGENT,
                    "-w", "\n%{http_code}", address, NULL};
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

void md_set_cover_dir(const char *dir)
{
    g_mutex_lock(&cover_lock);
    g_free(cover_dir);
    cover_dir = g_strdup(dir);
    g_mutex_unlock(&cover_lock);
}

char *md_default_cover_dir(void)
{
    return g_build_filename(md_app_dir(), "bilder", NULL);
}

/* Die Seite nennt kein Bild: Standbild aus dem Video holen */
static void cover_from_video(const char *video, const char *path)
{
    char *tmp = g_strconcat(path, ".part", NULL);
    char *argv[] = {"ffmpeg", "-nostdin", "-v", "error", "-rw_timeout", "20000000",
                    "-ss", COVER_FRAME_SECONDS, "-i", (char *)video, "-frames:v", "1",
                    "-vf", "scale=" COVER_FRAME_WIDTH ":-2", "-q:v", "4", "-f", "image2", "-y",
                    tmp, NULL};
    if (run_capture(argv, NULL, NULL) != 0 || file_size(tmp) <= 0 || g_rename(tmp, path) != 0)
        g_unlink(tmp);
    g_free(tmp);
}

char *md_cover_file(const char *website, const char *video)
{
    char *info = page_info(website);
    char *path = cache_file(website, ".jpg");
    if (file_size(path) <= 0 && info && *info != '\n') {
        *strchr(info, '\n') = 0;
        char *tmp = g_strconcat(path, ".part", NULL), *messages = NULL;
        char *argv[] = {"curl", "-sS", "-f", "-L", "-m", "20", "-A", USER_AGENT, "-o", tmp, info,
                        NULL};
        if (run_capture(argv, NULL, &messages) != 0 || g_rename(tmp, path) != 0) g_unlink(tmp);
        g_free(messages);
        g_free(tmp);
    }
    g_free(info);
    if (file_size(path) <= 0 && video && *video) cover_from_video(video, path);
    if (file_size(path) > 0) return path;
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
    /* Untertitel wie "- Tod im Schnee" kennt IMDb meist nicht */
    char *query = g_strdup(name);
    char *cut = strstr(query, " - ");
    if (cut) *cut = 0;
    if ((cut = strstr(query, ": "))) *cut = 0;
    char *lower = g_utf8_strdown(query, -1);
    char *escaped = g_uri_escape_string(lower, NULL, FALSE);
    char *api = g_strdup_printf(IMDB_API "%s.json?includeVideos=0", escaped);
    char *argv[] = {"curl", "-sS", "-f", "-L", "-m", "15", "-A", USER_AGENT, api, NULL};
    char *reply = NULL, *messages = NULL, *id = NULL;
    if (run_capture(argv, &reply, &messages) == 0 && reply) {
        id = imdb_pick(reply, name, is_film, year);
        char *line = id ? g_strdup_printf("%s %d", id, year ? *year : 0) : g_strdup("-");
        g_file_set_contents(saved, line, -1, NULL);
        g_free(line);
    }
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
        char *fetch[] = {"curl", "-sS", "-f", "-L", "-m", "120", "-A", USER_AGENT, "-o", packed,
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
        char *fetch[] = {"curl", "-sS", "-f", "-L", "-m", "1800", "-A", USER_AGENT, "-o", packed,
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
    return g_build_filename(g_app_dir, "warteschlange.tsv", NULL);
}

/* Warteschlange sichern (q.lock gehalten) */
static void queue_save(void)
{
    if (!q.persist) return;
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
                               "\t%s\t%s\t%s\t%s\t%d\t%s\t%s\t%s\t%s\t%s\t%d\t%d\n",
                               state, j->convert, j->keep_original, j->encoder, j->gpu_quality,
                               j->cpu_rf, j->total, title, url, target, message, j->skip_main_audio,
                               label, lang, extras_text, key, topic, j->season, j->episode);
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
    char *path = queue_path();
    g_file_set_contents(path, out->str, out->len, NULL);
    g_free(path);
    g_string_free(out, TRUE);
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
        if (g_strv_length(f) == 19) {
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

/* Auftrag aus der Liste nehmen und freigeben (q.lock gehalten, kein Worker aktiv) */
static void job_discard(Job *j)
{
    g_ptr_array_remove(q.jobs, j);
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
    j->state = MD_JOB_FAILED;
    g_free(j->message);
    j->message = g_strdup(message);
}

/* soll der Worker aufhören? (q.lock gehalten) */
static bool job_interrupted(const Job *j)
{
    return j->paused || j->remove_requested || q.quitting;
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
    char *argv[] = {"curl", "-sSIL", "-m", "30", "-A", USER_AGENT, (char *)url, NULL};
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
        *error = g_strdup("Der Server ist nicht erreichbar.");
    else if (status == 403)
        *error = g_strdup("Zugriff verweigert – das Video ist nur aus Deutschland abrufbar.");
    else if (status == 404 || status == 410)
        *error = g_strdup("Das Video ist nicht mehr in der Mediathek.");
    else if (status != 200 || size <= 0)
        *error = g_strdup_printf("Unerwartete Antwort des Servers (HTTP %d).", status);
    g_free(out);
    g_free(messages);
    return *error ? 0 : size;
}

/* curl für die .part-Datei starten -> Prozessnummer, 0 = Fehler */
static pid_t start_curl(const char *url, const char *part, gint64 rate)
{
    char *rate_text = g_strdup_printf("%" G_GINT64_FORMAT, rate);
    char *argv[32];
    int n = 0;
    char *fixed[] = {"curl", "-sS", "-f", "-L", "-C", "-", "--connect-timeout", "20",
                     "--speed-limit", "1024", "--speed-time", "60", "-A", USER_AGENT, NULL};
    for (int i = 0; fixed[i]; i++) argv[n++] = fixed[i];
    if (rate > 0) {
        argv[n++] = "--limit-rate";
        argv[n++] = rate_text;
    }
    argv[n++] = "-o";
    argv[n++] = (char *)part;
    argv[n++] = (char *)url;
    argv[n] = NULL;
    GPid pid = 0;
    bool ok = g_spawn_async(NULL, argv, NULL,
                            G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD
                            | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                            child_setup, NULL, &pid, NULL);
    g_free(rate_text);
    return ok ? pid : 0;
}

/* Warten, bis curl endet oder unterbrochen wird; Fortschritt nachführen. base = Bytes
 * der schon fertigen Dateien des Auftrags.
 * -> Exit-Code von curl, -1 = unterbrochen, -2 = Tempolimit geändert */
static int watch_curl(Job *j, pid_t pid, const char *part, gint64 rate, gint64 base)
{
    gint64 last_size = MAX(file_size(part), 0);
    double last_time = now_seconds();
    for (;;) {
        g_usleep(POLL_MS * 1000);
        int status = 0;
        bool ended = waitpid(pid, &status, WNOHANG) == pid;
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

        if (ended) return WIFEXITED(status) ? WEXITSTATUS(status) : -3;
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
    struct statvfs st;
    bool ok = statvfs(dir, &st) != 0
              || (gint64)st.f_bavail * (gint64)st.f_frsize >= needed + MIN_FREE_SPACE;
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

        pid_t pid = start_curl(url, part, rate);
        if (!pid) {
            *error = g_strdup("curl lässt sich nicht starten.");
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
            *error = g_strdup("Die Datei lässt sich nicht schreiben (Laufwerk voll?).");
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
    char *argv[] = {"curl", "-sS", "-f", "-L", "-m", "60", "-A", USER_AGENT, "-o", part,
                    e->url, NULL};
    if (run_capture(argv, NULL, NULL) == 0) g_rename(part, path);
    else md_log("Untertitel nicht verfügbar: %s (%s)", j->title, e->url);
    g_unlink(part);
    g_free(part);
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
            if (e && e->subtitle) continue;
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
        j->state = processing && !already_there ? MD_JOB_CONVERT_WAITING : MD_JOB_DONE;
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
        j->state = MD_JOB_WAITING;                  /* pausiert, abgebrochen oder Programmende */
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

/* Pipe lesen, ohne zu blockieren */
static void drain_pipe(int fd, GString *buffer)
{
    char chunk[4096];
    ssize_t n;
    while ((n = read(fd, chunk, sizeof chunk)) > 0) g_string_append_len(buffer, chunk, n);
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
 * copy = Bild unverändert übernehmen, sonst AV1 mit dem gewählten Encoder. */
static GPtrArray *build_ffmpeg_cmd(const Job *j, bool copy, MdEncoder encoder, const char *device,
                                   const char *tmp)
{
    GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
    args_add(a, (const char *const[]){"ffmpeg", "-hide_banner", "-nostdin", "-y", "-loglevel",
                                      "error", "-progress", "pipe:1", "-nostats", NULL});
    if (!copy && encoder == MD_ENCODER_GPU)
        args_add(a, (const char *const[]){"-vaapi_device", device, NULL});
    args_add(a, (const char *const[]){"-i", j->target, NULL});

    GPtrArray *maps = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *metadata = g_ptr_array_new_with_free_func(g_free);
    args_add(maps, (const char *const[]){"-map", "0:v:0", NULL});
    int inputs = 1, audio = 0, subtitles = 0;
    if (!j->skip_main_audio) {
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
        args_add(a, (const char *const[]){"-vf", "format=p010,hwupload", "-c:v", "av1_vaapi",
                                          "-rc_mode", "CQP", "-global_quality:v", quality,
                                          "-g:v", GOP_FRAMES, NULL});
    } else {
        snprintf(quality, sizeof quality, "%d", j->cpu_rf);
        args_add(a, (const char *const[]){"-vf", "format=yuv420p10le", "-c:v", "libsvtav1",
                                          "-preset", SVT_PRESET, "-crf", quality, "-svtav1-params",
                                          "tune=1", "-g:v", GOP_FRAMES, NULL});
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
static int run_encoder(Job *j, bool copy, MdEncoder encoder, const char *tmp, double duration,
                       char **messages)
{
    const char *device = render_device();
    if (!copy && encoder == MD_ENCODER_GPU && !device) {
        *messages = g_strdup("Kein Render-Gerät gefunden.");
        return 1;
    }
    GPtrArray *command = build_ffmpeg_cmd(j, copy, encoder, device, tmp);
    char **argv = (char **)command->pdata;

    GPid pid = 0;
    int out_fd = -1, err_fd = -1;
    if (!g_spawn_async_with_pipes(NULL, argv, NULL,
                                  G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, child_setup,
                                  GINT_TO_POINTER(1), &pid, NULL, &out_fd, &err_fd, NULL)) {
        *messages = g_strdup("ffmpeg lässt sich nicht starten.");
        g_ptr_array_free(command, TRUE);
        return 1;
    }
    g_ptr_array_free(command, TRUE);
    fcntl(out_fd, F_SETFL, O_NONBLOCK);
    fcntl(err_fd, F_SETFL, O_NONBLOCK);
    GString *progress = g_string_new(NULL), *errors = g_string_new(NULL);
    double seconds = 0, factor = 0;
    bool stopped = false, interrupted = false;
    int status = 0, rc;
    for (;;) {
        struct pollfd fds[2] = {{out_fd, POLLIN, 0}, {err_fd, POLLIN, 0}};
        poll(fds, 2, POLL_MS);
        drain_pipe(out_fd, progress);
        drain_pipe(err_fd, errors);
        parse_progress(progress, &seconds, &factor);
        if (errors->len > 2000) g_string_erase(errors, 0, errors->len - 2000);

        g_mutex_lock(&q.lock);
        bool end = j->remove_requested || q.quitting, pause = j->paused;
        j->progress = duration > 0 ? MIN(seconds / duration, 1.0) : 0;
        j->remaining = factor > 0 ? (duration - seconds) / factor : -1;
        g_mutex_unlock(&q.lock);

        if (end) {
            interrupted = true;
            stop_child(pid);
            break;
        }
        if (pause != stopped) {
            kill(pid, pause ? SIGSTOP : SIGCONT);
            stopped = pause;
        }
        if (waitpid(pid, &status, WNOHANG) == pid) {
            drain_pipe(err_fd, errors);
            break;
        }
    }
    close(out_fd);
    close(err_fd);
    g_string_free(progress, TRUE);
    if (interrupted) rc = -1;
    else rc = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
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

/* Worker: Spuren zusammenführen und auf Wunsch nach AV1 umwandeln; Ergebnis prüfen */
static gpointer convert_thread(gpointer data)
{
    Job *j = data;
    char *tmp = temp_path(j->target), *result = result_path(j->target);
    char *error = NULL, *note = NULL, *codec = NULL, *messages = NULL;
    double duration = 0;
    bool done = false, interrupted = false, copy = !j->convert, av1_useless = false;

    if (!media_info(j->target, &codec, &duration))
        error = g_strdup("Die heruntergeladene Datei ist nicht lesbar.");
    while (!error && !done && !interrupted) {
        g_mutex_lock(&q.lock);
        j->merging = copy;
        j->progress = 0;
        g_mutex_unlock(&q.lock);
        g_clear_pointer(&messages, g_free);
        int rc = run_encoder(j, copy, j->encoder, tmp, duration, &messages);
        if (rc > 0 && !copy && j->encoder == MD_ENCODER_GPU) {
            md_log("GPU-Umwandlung fehlgeschlagen, versuche CPU: %s (%s)", j->title, messages);
            g_clear_pointer(&messages, g_free);
            rc = run_encoder(j, false, MD_ENCODER_CPU, tmp, duration, &messages);
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
            note = g_strdup("AV1 wäre nicht kleiner – die MP4-Datei bleibt");
            done = true;
        } else if (g_rename(tmp, result) != 0) {
            error = g_strdup("Das Ergebnis lässt sich nicht ablegen.");
        } else {
            char *tracks = tracks_note(j), *after = md_format_size(file_size(result));
            if (copy) {
                note = g_strdup_printf("%s%s%s", after, tracks,
                                       av1_useless ? " · AV1 wäre nicht kleiner" : "");
            } else {
                char *before = md_format_size(comparable_size(j, duration));
                note = g_strdup_printf("AV1: %s statt %s%s", after, before, tracks);
                g_free(before);
            }
            g_free(tracks);
            g_free(after);
            /* ohne AV1 steckt die MP4-Datei vollständig in der MKV-Datei */
            if (!j->keep_original || !j->convert) g_unlink(j->target);
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
        j->state = MD_JOB_DONE;
        j->progress = 1;
        g_free(j->message);
        j->message = g_strdup(note);
        history_add(j);
        md_log("Verarbeitung fertig: %s (%s)", j->title, note);
    } else if (interrupted) {
        j->state = MD_JOB_CONVERT_WAITING;
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
            if (j->state == MD_JOB_WAITING && downloads < q.max_downloads) {
                j->state = MD_JOB_DOWNLOADING;
                downloads++;
                worker = download_thread;
            } else if (j->state == MD_JOB_CONVERT_WAITING && conversions < q.max_conversions) {
                j->state = MD_JOB_CONVERTING;
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
        g_cond_wait_until(&q.wake, &q.lock, g_get_monotonic_time() + G_TIME_SPAN_SECOND);
    }
    g_mutex_unlock(&q.lock);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Öffentliche Schnittstelle
 * ------------------------------------------------------------------------- */
/* Bis Version 1.2 hieß das Programm ZDF-Downloader: Verlauf, Warteschlange und Cache mitnehmen */
static void adopt_old_app_dir(void)
{
    char *old = g_build_filename(g_get_home_dir(), OLD_APP_DIR, NULL);
    if (g_file_test(old, G_FILE_TEST_IS_DIR) && !g_file_test(g_app_dir, G_FILE_TEST_EXISTS))
        g_rename(old, g_app_dir);
    g_free(old);
}

void md_init(bool persist)
{
    g_app_dir = g_build_filename(g_get_home_dir(), ".mediathek-downloader", NULL);
    adopt_old_app_dir();
    g_log_path = g_build_filename(g_app_dir, "protokoll.log", NULL);
    g_mkdir_with_parents(g_app_dir, 0755);
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
    queue_save();
    g_mutex_unlock(&q.lock);
    g_thread_join(q.dispatcher);
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
    j->gpu_quality = o->gpu_quality;
    j->cpu_rf = o->cpu_rf;
    j->state = MD_JOB_WAITING;
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
                               g_strdup(j->target), g_strdup(j->message), j->have, j->total,
                               j->speed, j->progress, j->remaining};
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
    queue_save();
    g_cond_signal(&q.wake);
    g_mutex_unlock(&q.lock);
}

void md_job_retry(guint id)
{
    g_mutex_lock(&q.lock);
    Job *j = job_by_id(id);
    if (j && j->state == MD_JOB_FAILED) {
        j->state = MD_JOB_WAITING;  /* fertige Dateien überspringt der Download von selbst */
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

void md_set_rate_limit(gint64 bytes_per_second)
{
    g_mutex_lock(&q.lock);
    q.rate_limit = MAX(bytes_per_second, 0);
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
