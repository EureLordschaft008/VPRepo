/*
 * Mediathek Downloader – Schnittstelle der Engine (md_core.c)
 * ===========================================================
 * Genutzt von der grafischen Oberfläche (md_gui.c) und der Terminal-Version
 * (md_cli.c). Alle Funktionen sind thread-sicher, sofern nichts anderes dabeisteht.
 */
#ifndef MD_CORE_H
#define MD_CORE_H

#include <glib.h>
#include <stdbool.h>

#define MD_VERSION "1.9.0"
#define MD_MAX_DOWNLOADS 4      /* höchstens so viele Downloads gleichzeitig */
#define MD_MAX_CONVERSIONS 3    /* höchstens so viele Umwandlungen gleichzeitig */
#define MD_DEFAULT_GPU_QUALITY 64   /* VAAPI-AV1 (0–255), wie im AV1-Converter */
#define MD_DEFAULT_CPU_RF 35        /* SVT-AV1 (0–63), wie im AV1-Converter */

typedef enum { MD_KIND_ALL, MD_KIND_FILMS, MD_KIND_SERIES } MdKind;
/* Mediatheken als Bitmaske */
typedef enum {
    MD_SOURCE_ZDF = 1, MD_SOURCE_ARD = 2, MD_SOURCE_3SAT = 4, MD_SOURCE_ZDFNEO = 8,
    MD_SOURCE_ONE = 16, MD_SOURCE_WDR = 32, MD_SOURCE_SWR = 64, MD_SOURCE_NDR = 128,
    MD_SOURCE_RBB = 256, MD_SOURCE_NETZKINO = 512, MD_SOURCE_ARTE = 1024, MD_SOURCE_MDR = 2048,
    MD_SOURCE_BR = 4096, MD_SOURCE_SRF = 8192, MD_SOURCE_ORF = 16384, MD_SOURCE_KIKA = 32768,
    MD_SOURCE_ZDFTIVI = 1 << 16, MD_SOURCE_ZDFINFO = 1 << 17, MD_SOURCE_PHOENIX = 1 << 18,
    MD_SOURCE_HR = 1 << 19, MD_SOURCE_SR = 1 << 20, MD_SOURCE_ARDALPHA = 1 << 21,
    MD_SOURCE_TAGESSCHAU24 = 1 << 22, MD_SOURCE_DW = 1 << 23, MD_SOURCE_FUNK = 1 << 24,
    MD_SOURCE_ARTE_INTL = 1 << 25,  /* ARTE auf Englisch, Französisch, Spanisch … */
    MD_SOURCE_ARCHIVE = 1 << 26, MD_SOURCE_NRK = 1 << 27,
    MD_SOURCE_COMMONS = 1 << 28,    /* Blender-Filme und Wikimedia Commons */
    MD_SOURCE_ALL = (1 << 29) - 1
} MdSource;
typedef enum { MD_QUALITY_HIGH, MD_QUALITY_MEDIUM, MD_QUALITY_LOW } MdQuality;
typedef enum { MD_ENCODER_GPU, MD_ENCODER_CPU } MdEncoder;

/* Tonspur oder Untertitel eines Eintrags */
typedef struct {
    char *label;                /* "Deutsch", "Englisch", "Audiodeskription" … */
    char *lang;                 /* ISO 639-2, z. B. "deu" */
    char *url;                  /* Untertitel: WebVTT; Ton: kleinste Datei der Sprache */
} MdTrack;

/* ein Eintrag der Mediathek; Fassungen in anderen Sprachen sind zusammengefasst */
typedef struct {
    char *channel, *topic, *title, *description, *website;
    char *url_high, *url_medium, *url_low, *url_subtitle;
    char *name;                 /* Titel ohne "(S18/E05)" und "- Neue Folgen" */
    gint64 timestamp;           /* Sendezeit (Unix-Zeit) */
    int duration;               /* Sekunden */
    int season, episode;        /* -1 = keine Angabe */
    int source;                 /* MD_SOURCE_* */
    bool is_film, is_variant;   /* variant: Gebärdensprache (eigenes Bild) */
    const char *track_label, *track_lang;   /* Sprache laut Titel, NULL = Hauptfassung */
    GPtrArray *audio;           /* MdTrack*; [0] = Ton der Datei, aus der das Bild kommt */
    GPtrArray *subtitles;       /* MdTrack* */
} MdTitle;

/* Antworten der Suche zwischenspeichern? USE liest gespeicherte, REFRESH holt neu und speichert */
typedef enum { MD_CACHE_OFF, MD_CACHE_USE, MD_CACHE_REFRESH } MdCache;

typedef struct {
    const char *text;           /* Suchbegriff; leer = neueste Einträge */
    MdKind kind;
    int sources;                /* MD_SOURCE_*, 0 = alle */
    MdCache cache;
    int min_minutes;            /* kürzere Einträge (Trailer, Clips) ausblenden */
    bool hide_variants;         /* Fassungen mit Gebärdensprache ausblenden */
} MdQuery;

/* Wohin und wie ein Eintrag heruntergeladen wird */
typedef struct {
    const char *dir;
    MdQuality quality;
    bool convert;               /* nach dem Download nach AV1 umwandeln */
    MdEncoder encoder;
    int gpu_quality, cpu_rf;
    bool keep_original;         /* MP4 nach erfolgreicher Umwandlung behalten */
    /* abgewählte Bezeichnungen (MdTrack.label), NULL-terminiert; NULL = alles laden */
    const char *const *skip_audio, *const *skip_subtitles;
} MdJobOptions;

typedef enum {
    MD_JOB_WAITING,             /* Download wartet (auch: pausiert) */
    MD_JOB_DOWNLOADING,
    MD_JOB_CONVERT_WAITING,     /* Download fertig, Umwandlung/Zusammenführen wartet */
    MD_JOB_CONVERTING,          /* AV1-Umwandlung oder Zusammenführen der Spuren */
    MD_JOB_DONE,
    MD_JOB_FAILED
} MdJobState;

/* Momentaufnahme eines Auftrags (gehört dem Aufrufer) */
typedef struct {
    guint id;
    MdJobState state;
    bool paused;
    bool merging;               /* CONVERTING: nur Spuren zusammenführen, kein AV1 */
    char *title, *target, *message;
    gint64 have, total;         /* Bytes */
    double speed;               /* Download: Bytes/s */
    double progress;            /* 0..1 der aktuellen Phase */
    double remaining;           /* Sekunden der aktuellen Phase, < 0 = unbekannt */
} MdJobView;

/* Angaben von der Seite des Titels */
typedef struct {
    char *fsk;                  /* "FSK 12", NULL = keine Angabe */
    char **genres;              /* "Komödie" …, NULL-terminiert */
} MdPageInfo;

/* Angaben von IMDb */
typedef struct {
    double rating;              /* 0 = keine Bewertung */
    int votes, year;            /* year: Erscheinungsjahr, 0 = unbekannt */
} MdImdbInfo;

/* Stand eines Titels aus Sicht des Nutzers */
typedef enum {
    MD_STATUS_MISSING,          /* noch nicht geladen */
    MD_STATUS_PARTIAL,          /* angefangen, aber nicht in der Warteschlange */
    MD_STATUS_QUEUED,           /* in der Warteschlange */
    MD_STATUS_DONE,             /* Datei liegt vor */
    MD_STATUS_GONE              /* laut Verlauf geladen, Datei aber nicht mehr da */
} MdStatus;

/* ein Eintrag des Verlaufs (gehört dem Aufrufer) */
typedef struct {
    char *topic, *title, *path;
    int season, episode;        /* -1 = keine Angabe */
    gint64 size, finished;      /* Bytes, Unix-Zeit */
    bool exists;                /* liegt die Datei noch dort? */
    bool is_film;
} MdHistoryEntry;

/* --- Start und Ende ----------------------------------------------------- */
/* persist: Warteschlange in ~/.mediathek-downloader speichern und von dort laden */
void md_init(bool persist);
/* laufende Arbeit anhalten (.part-Dateien bleiben) und Warteschlange sichern */
void md_shutdown(void);
const char *md_app_dir(void);
const char *md_log_path(void);
void md_log(const char *format, ...) G_GNUC_PRINTF(1, 2);
bool md_have_tools(char **missing);     /* curl, ffmpeg, ffprobe vorhanden? */
bool md_gpu_available(void);            /* AV1-Encoder per VAAPI nutzbar? (kurzer Test) */

/* --- Suche -------------------------------------------------------------- */
/* Liefert MdTitle* (mit md_title_free freigeben); NULL bei Fehler (*error gesetzt).
 * cancel: wird zwischen den Seiten geprüft, darf NULL sein. */
GPtrArray *md_search(const MdQuery *query, const volatile gint *cancel, bool *truncated,
                     char **error);
void md_title_free(gpointer title);
const char *md_source_label(int source);    /* "ZDF", "ARD" … */
const char *md_title_url(const MdTitle *title, MdQuality quality);
gint64 md_title_estimate(const MdTitle *title, MdQuality quality);  /* Bytes, geschätzt */
char *md_target_path(const MdTitle *title, const char *dir);        /* Pfad der MP4-Datei */

/* Titelbild der Seite website als Datei holen (blockiert); NULL = keines. Freigeben mit g_free. */
char *md_cover_file(const char *website, const char *video);
/* Altersfreigabe und Kategorien von der Seite des Titels (blockiert); NULL = nicht erreichbar */
MdPageInfo *md_page_info(const char *website);
void md_page_info_free(MdPageInfo *info);
void md_set_cover_dir(const char *dir);     /* Ordner der Titelbilder; NULL = Standard */
char *md_default_cover_dir(void);           /* ~/.mediathek-downloader/bilder */

/* Adresse der passenden IMDb-Seite, sonst der IMDb-Suche (blockiert). Freigeben mit g_free. */
char *md_imdb_url(const char *name, bool is_film);
/* Kategorien von IMDb auf Deutsch (blockiert, lädt anfangs etwa 230 MB). NULL = kein sicherer
 * Treffer. Freigeben mit g_strfreev(). */
char **md_imdb_genres(const char *name, bool is_film);
/* Bewertung nachschlagen (blockiert, lädt beim ersten Mal die Datendatei von IMDb);
 * false = kein sicherer Treffer */
bool md_imdb_info(const char *name, bool is_film, MdImdbInfo *info);

/* --- Warteschlange ------------------------------------------------------ */
/* -> Auftragsnummer; 0 = schon in der Warteschlange */
guint md_queue_add(const MdTitle *title, const MdJobOptions *options);
MdJobView *md_queue_snapshot(int *count);
void md_queue_snapshot_free(MdJobView *views, int count);
void md_job_set_paused(guint id, bool paused);
void md_queue_set_paused(bool paused);  /* alle offenen Aufträge */
void md_job_retry(guint id);
void md_job_remove(guint id);           /* bricht ab und löscht halbfertige Dateien */
void md_queue_remove_finished(void);    /* fertige Einträge aus der Liste nehmen */
void md_set_rate_limit(gint64 bytes_per_second);    /* 0 = unbegrenzt, gilt sofort */
void md_set_parallel(int downloads, int conversions);
/* Encoder für alle offenen Aufträge; eine laufende Umwandlung bleibt, wie sie ist */
void md_set_encoder(MdEncoder encoder, int gpu_quality, int cpu_rf);

/* --- Verlauf: was schon geladen ist ------------------------------------- */
/* Prüft Warteschlange, Verlauf und den Ordner dir. path (darf NULL sein): Datei, falls da.
 * Eine Folge gilt als geladen, egal in welcher Qualität oder an welchem Speicherort. */
MdStatus md_title_status(const MdTitle *title, const char *dir, char **path);
/* sortiert nach Sendung, Staffel, Folge; prüft jede Datei */
MdHistoryEntry *md_history_snapshot(int *count);
void md_history_snapshot_free(MdHistoryEntry *entries, int count);
void md_history_forget(const char *path);   /* Eintrag löschen, die Datei bleibt */
/* Arbeitet ein offener Auftrag noch an dieser Datei (Zusammenführen, AV1)? */
bool md_file_in_use(const char *path);

/* --- Speicherort mit dem Verlauf abgleichen ----------------------------- */
typedef struct {
    char *path, *topic, *title;
    int season, episode;        /* -1 = keine Angabe */
    bool is_film;
    char *key;                  /* Schlüssel im Verlauf */
    gint64 size, modified;
} MdFoundFile;

typedef struct {
    char *from, *to;
} MdMovedFile;

typedef struct {
    int present;                /* Verlauf und Ordner stimmen überein */
    GPtrArray *moved;           /* MdMovedFile*: Datei liegt jetzt woanders im Ordner */
    GPtrArray *found;           /* MdFoundFile*: im Ordner, aber nicht im Verlauf */
    GPtrArray *missing;         /* char*: im Verlauf, Datei nirgends gefunden */
} MdFolderCheck;

/* Ordner dir durchsuchen (blockiert; kann bei großen Platten dauern) */
MdFolderCheck *md_check_folder(const char *dir);
void md_folder_check_free(MdFolderCheck *check);
/* Ergebnis übernehmen: Pfade berichtigen, Gefundenes aufnehmen, Fehlendes vergessen */
void md_folder_check_apply(const MdFolderCheck *check, bool relink, bool add, bool forget);
/* Standbild aus einer geladenen Datei (blockiert, wird zwischengespeichert); NULL = keines.
 * Freigeben mit g_free. */
char *md_video_frame(const char *path);

/* --- Anzeige ------------------------------------------------------------ */
char *md_format_size(double bytes);         /* "3,2 GB" */
char *md_format_time(double seconds);       /* "1 h 05 min" */

#endif
