/*
 * Duplikat-Finder – Engine (gemeinsam für Terminal- und GUI-Version)
 *
 * Die Engine sucht Videos (und auf Wunsch Bilder) in mehreren Ordnern, liest
 * je Datei einige Vergleichsbilder (pHash) über Hilfsprozesse (df_worker.c,
 * CPU und/oder GPU parallel), merkt sie sich in einer SQLite-Datenbank und
 * vergleicht alle Dateien miteinander. Ähnliche Dateien bilden Gruppen.
 * Optional werden Teilclips (gekürzte Fassungen) über einen Audio-Fingerabdruck
 * gefunden. Dazu kommen Auswahlregeln und Dateiaktionen (Papierkorb, Löschen,
 * Verschieben, Kopieren, Hard- und Symlinks).
 *
 * Front-ends (df_cli.c, df_gui.c) steuern den Ablauf über df_scan() und
 * bekommen Meldungen und Fortschritt über die Callbacks in df_fe.
 */
#ifndef DF_CORE_H
#define DF_CORE_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "df_proto.h"

#define DF_NAME "Duplikat-Finder"

/* Voreinstellungen */
#define DEFAULT_FRAMES 6            /* Vergleichsbilder je Video */
#define DEFAULT_SIMILARITY 90.0     /* % */
#define DEFAULT_DURATION_DIFF 20    /* % Laufzeitunterschied (100 = egal) */
#define MAX_CPU_WORKERS 32
#define MAX_GPU_WORKERS 6
#define DEFAULT_GPU_WORKERS 2       /* gemessen: mehr als 2 bringen kaum mehr */

/* Abbruch: Strg+C, SIGTERM oder "Abbrechen" in der Oberfläche */
extern volatile sig_atomic_t df_cancel;
extern char *DF_APP_DIR, *DF_DB_PATH, *DF_RUN_LOG, *DF_RESULT_PATH, *DF_SCHEDULED_PATH, *DF_THUMB_DIR;

/* ---------------------------------------------------------------------------
 * Optionen
 * ------------------------------------------------------------------------- */
typedef enum { DF_FOLDERS_ALL, DF_FOLDERS_SAME, DF_FOLDERS_DIFFERENT } DfFolderMode;

typedef struct {
    char **v;
    int n;
} DfStrList;

void df_strlist_add(DfStrList *l, const char *s);
void df_strlist_free(DfStrList *l);
void df_strlist_copy(DfStrList *dst, const DfStrList *src);

typedef struct {
    /* Dateien */
    DfStrList includes;         /* Ordner, die durchsucht werden */
    DfStrList excludes;         /* Ordner, die übersprungen werden */
    DfStrList path_contains;    /* nur Pfade, die einen dieser Texte enthalten (leer = alle) */
    DfStrList path_excludes;    /* Pfade mit einem dieser Texte überspringen (auch *?-Muster) */
    bool subdirs;               /* Unterordner einbeziehen */
    bool images;                /* auch Bilder vergleichen */
    bool skip_symlinks;         /* symbolische Links nicht verfolgen */
    bool skip_hardlinks;        /* mehrfach verlinkte Dateien nur einmal */
    bool skip_hidden;           /* versteckte Ordner/Dateien (.name) überspringen */
    long long min_size, max_size;   /* Byte, 0 = ohne Grenze */

    /* Vergleich */
    int frames;                 /* Vergleichsbilder je Video (1..DF_MAX_FRAMES) */
    double similarity;          /* Mindestähnlichkeit in % */
    bool flipped;               /* auch gespiegelte Videos erkennen */
    bool white_borders;         /* auch weiße Balken abschneiden */
    int duration_diff;          /* max. Laufzeitunterschied in % (100 = egal) */
    DfFolderMode folder_mode;
    bool against_db;            /* auch gegen alle Dateien der Datenbank vergleichen */
    bool retry_failed;          /* früher nicht lesbare Dateien erneut versuchen */

    /* Teilclips (gekürzte Fassungen) über den Ton */
    bool partial;
    int partial_min_ratio;      /* % – Clip mindestens so lang wie ... der Quelle */
    int partial_audio;          /* % Ton-Ähnlichkeit */
    bool partial_visual;        /* zusätzlich Bilder prüfen */
    int partial_visual_pct;     /* % Bild-Ähnlichkeit */

    /* Hardware */
    int cpu_workers;            /* 0 = kein CPU-Prozess */
    int gpu_workers;            /* 0 = keine GPU */
    bool considerate;           /* anderen Programmen Vorrang lassen (CPU, Speicher) */
} DfOptions;

void df_options_default(DfOptions *o);
void df_options_free(DfOptions *o);
void df_options_copy(DfOptions *dst, const DfOptions *src);

/* ---------------------------------------------------------------------------
 * Dateien und Ergebnis
 * ------------------------------------------------------------------------- */
typedef struct {
    int64_t id;                 /* Datenbank-Schlüssel */
    char *path;
    long long size, mtime;
    double duration;            /* s (Bilder: 0) */
    int width, height;
    float fps;
    int bitrate, abitrate;      /* kbit/s */
    int asr, ach;               /* Ton: Abtastrate, Kanäle (0 = kein Ton) */
    char vcodec[16], acodec[16];
    bool image;
    int nframes;
    DfFrame *frames;            /* nframes Vergleichsbilder */
} DfFile;

typedef struct {
    int file;                   /* Index in DfResult.files */
    float similarity;           /* % (beste Übereinstimmung in der Gruppe) */
    bool flipped;               /* gespiegelt */
    bool partial;               /* Teilclip einer anderen Datei der Gruppe */
    double offset;              /* Teilclip: beginnt bei ... s in der Quelle */
    int source;                 /* Teilclip: Index (in files) der Quelle, sonst -1 */
} DfMember;

typedef struct {
    int first, n;               /* Mitglieder members[first .. first+n) */
    float similarity;           /* niedrigste Ähnlichkeit der Gruppe */
    bool partial;               /* enthält Teilclips */
} DfGroup;

typedef struct {
    DfFile *files;
    int nfiles;
    DfMember *members;
    int nmembers;
    DfGroup *groups;
    int ngroups;
    double similarity;          /* Schwelle, mit der verglichen wurde */
    int frames;
    long long scan_time;        /* Unix-Zeit */
} DfResult;

void df_result_free(DfResult *r);

typedef struct {
    int found;                  /* Dateien in den Ordnern */
    int cached;                 /* aus der Datenbank */
    int scanned;                /* neu gelesen */
    int failed;                 /* nicht lesbar */
    int gpu_files, cpu_files;   /* davon per GPU / CPU */
    int audio_scanned;          /* Teilclips: neue Ton-Fingerabdrücke */
    long long pairs;            /* verglichene Paare */
    int groups, duplicates;
    double secs_walk, secs_scan, secs_audio, secs_compare;
    bool aborted;
} DfStats;

/* ---------------------------------------------------------------------------
 * Front-end-Schnittstelle (Callbacks können aus Worker-Threads kommen)
 * ------------------------------------------------------------------------- */
typedef enum {
    DF_PHASE_WALK,              /* Ordner werden gelesen */
    DF_PHASE_SCAN,              /* Vergleichsbilder werden gelesen */
    DF_PHASE_AUDIO,             /* Ton-Fingerabdrücke (Teilclips) */
    DF_PHASE_COMPARE,           /* Vergleich */
    DF_PHASE_PARTIAL,           /* Teilclips vergleichen */
} DfPhase;

typedef struct {
    void (*log)(const char *msg);                   /* sichtbare Meldung */
    void (*phase)(DfPhase phase, int total);        /* neue Phase (total: Anzahl Schritte) */
    void (*progress)(DfPhase phase, int done, int total);
} DfFrontend;

extern DfFrontend df_fe;

/* ---------------------------------------------------------------------------
 * Ablauf
 * ------------------------------------------------------------------------- */
void df_init(void);                             /* Pfade, Datenbank öffnen */
void df_shutdown(void);
const char *df_worker_path(void);               /* NULL = Hilfsprozess fehlt */
void df_log_open(void);
__attribute__((format(printf, 2, 3))) void df_log(bool show, const char *fmt, ...);

typedef struct {
    bool available;
    char device[64];            /* /dev/dri/renderD128 */
    char name[96];
} DfGpu;
bool df_gpu_detect(DfGpu *gpu);
/* Standby verhindern (systemd-logind, "sleep:idle"); verschachtelbar, aus jedem Thread */
void df_inhibit_sleep(bool on, const char *why);                 /* startet einen GPU-Hilfsprozess zum Test */
int df_cpu_count(void);
int df_default_cpu_workers(void);

/* Ordner durchsuchen, Vergleichsbilder lesen (Cache), vergleichen.
 * -> 0 = ok, 1 = abgebrochen, -1 = Fehler (err: Meldung) */
int df_scan(const DfOptions *o, const DfGpu *gpu, DfResult *res, DfStats *st, const char **err);

/* Nur neu vergleichen (andere Schwelle o. Ä.) – mit den Dateien des letzten Laufs */
int df_recompare(const DfOptions *o, DfResult *res);

/* Pausieren/Fortsetzen (aus jedem Thread) */
void df_set_paused(bool on);
bool df_paused(void);

typedef struct {                /* laufender Hilfsprozess (für Statusanzeigen) */
    char path[1024];
    bool gpu, busy, held;       /* held: wartet wegen anderer Programme */
    double secs;                /* seit Beginn der Datei */
} DfJobInfo;
int df_jobs_snapshot(DfJobInfo *out, int max);

typedef struct {
    int done, total;
    int gpu_done, cpu_done, failed;
    double files_per_sec, gpu_per_sec, cpu_per_sec;
    double eta;                 /* s, < 0 = unbekannt */
    double cpu_load;            /* CPU-Last gesamt in % */
    int active_cpu, held_cpu;   /* laufende / wegen Last angehaltene CPU-Prozesse */
} DfProgress;
bool df_progress(DfProgress *p);

/* ---------------------------------------------------------------------------
 * Auswahl (checked: je Mitglied, removed: aus der Liste entfernt)
 * ------------------------------------------------------------------------- */
typedef enum {
    DF_CRIT_DURATION, DF_CRIT_RESOLUTION, DF_CRIT_FPS, DF_CRIT_BITRATE, DF_CRIT_AUDIO_BITRATE,
    DF_CRIT_SIZE, DF_CRIT_NEWEST, DF_CRIT_OLDEST, DF_CRIT_SHORT_PATH, DF_CRIT_COUNT
} DfCriterion;
extern const char *const DF_CRIT_NAME[DF_CRIT_COUNT];
extern const char *const DF_CRIT_ID[DF_CRIT_COUNT];

/* bestes Mitglied einer Gruppe nach der Reihenfolge der Kriterien (nicht entfernte) */
int df_best_member(const DfResult *r, int group, const int *order, int norder, const bool *removed);
/* Wert des Kriteriums (größer = besser) */
double df_criterion_value(const DfFile *f, DfCriterion c);

typedef enum {
    DF_SEL_LOWEST_QUALITY,      /* alle außer der besten markieren */
    DF_SEL_KEEP_NEWEST,         /* ältere markieren */
    DF_SEL_KEEP_OLDEST,         /* neuere markieren */
    DF_SEL_IDENTICAL,           /* 100 % gleich: alle außer der besten */
    DF_SEL_IDENTICAL_SIZE,      /* 100 % gleich und gleich groß (bytegleich wahrscheinlich) */
    DF_SEL_PATH_CONTAINS,       /* Pfad enthält Text (je Gruppe bleibt eine unmarkiert) */
    DF_SEL_PATH_NOT_CONTAINS,
    DF_SEL_SIMILARITY_RANGE,    /* Ähnlichkeit von..bis (alle außer der besten) */
    DF_SEL_PARTIAL_CLIPS,       /* Teilclips markieren (Quelle bleibt) */
    DF_SEL_INVERT,
    DF_SEL_NONE,
} DfSelectRule;

typedef struct {
    DfSelectRule rule;
    const int *order;           /* Qualitätskriterien (NULL = Standard) */
    int norder;
    const char *text;           /* Pfadtext */
    double sim_from, sim_to;
    bool add;                   /* zur Auswahl hinzufügen statt ersetzen */
} DfSelectSpec;
/* -> Anzahl markierter Mitglieder danach */
int df_select(const DfResult *r, bool *checked, const bool *removed, const DfSelectSpec *s);

/* ---------------------------------------------------------------------------
 * Dateiaktionen (je Datei; Fehlertext in err)
 * ------------------------------------------------------------------------- */
typedef enum {
    DF_ACT_TRASH, DF_ACT_DELETE, DF_ACT_MOVE, DF_ACT_COPY, DF_ACT_HARDLINK, DF_ACT_SYMLINK
} DfAction;
/* Datei path (Mitglied) verarbeiten. target: Zielordner (MOVE/COPY) bzw. Datei, auf
 * die verlinkt wird (HARDLINK/SYMLINK). new_path (MOVE): neuer Ort. -> true = ok */
bool df_file_action(DfAction a, const char *path, const char *target, char **new_path, char *err,
                    size_t errsz);
bool df_rename_file(const char *path, const char *new_name, char **new_path, char *err,
                    size_t errsz);

/* ---------------------------------------------------------------------------
 * Datenbank
 * ------------------------------------------------------------------------- */
typedef struct {
    int files, failed, audio;
    long long bytes;            /* Größe der Datenbankdatei */
    int notmatch, ignored;
} DfDbInfo;
bool df_db_info(DfDbInfo *i);
int df_db_prune_missing(void);                  /* -> entfernte Einträge */
bool df_db_clear(void);
int df_db_relocate(const char *old_prefix, const char *new_prefix); /* -> geänderte */
bool df_db_export_json(const char *path, char *err, size_t errsz);
int df_db_import_json(const char *path, char *err, size_t errsz);   /* -> Einträge, <0 Fehler */
int df_db_forget_failed(void);
bool df_db_remove_path(const char *path);
bool df_db_rename_path(const char *old_path, const char *new_path);
/* Suche in der Datenbank (für "Datenbank bearbeiten") -> Anzahl, Ergebnis in out (frei mit
 * df_strlist_free); je Eintrag "Pfad\tStatus\tFehler" */
int df_db_search(const char *text, int max, DfStrList *out);

/* "Kein Duplikat": Paare werden nie wieder als Treffer gemeldet */
bool df_notmatch_add(int64_t a, int64_t b);
int df_notmatch_clear(void);
/* Ignorierte Dateien: tauchen in keinem Ergebnis mehr auf */
bool df_ignore_add(const char *path);
int df_ignore_clear(void);
int df_ignore_list(DfStrList *out);
bool df_ignore_remove(const char *path);

/* ---------------------------------------------------------------------------
 * Ergebnis speichern / laden / exportieren
 * ------------------------------------------------------------------------- */
/* removed/checked dürfen NULL sein */
bool df_result_save(const DfResult *r, const bool *removed, const bool *checked, const char *path,
                    char *err, size_t errsz);
bool df_result_load(DfResult *r, bool **removed, bool **checked, const char *path, char *err,
                    size_t errsz);
bool df_result_export_csv(const DfResult *r, const bool *removed, const bool *checked,
                          const char *path, char *err, size_t errsz);
bool df_result_export_html(const DfResult *r, const bool *removed, const bool *checked,
                           const char *path, char *err, size_t errsz);

/* ---------------------------------------------------------------------------
 * Vorschaubilder und Vergleich zweier Dateien (eigene kleine Prozessgruppe)
 * ------------------------------------------------------------------------- */
typedef struct {
    int n;
    int w[DF_MAX_FRAMES], h[DF_MAX_FRAMES];
    unsigned char *rgb[DF_MAX_FRAMES];          /* RGB24, NULL = nicht lesbar */
} DfThumbs;
/* blockiert (aus einem Hintergrund-Thread aufrufen); gpu: GPU-Prozess benutzen */
bool df_thumbs_get(const char *path, int k, int width, bool gpu_ok, DfThumbs *t);
void df_thumbs_free(DfThumbs *t);
void df_thumbs_shutdown(void);
void df_thumbs_set_gpu(const DfGpu *gpu);

typedef struct {
    bool ok;
    char error[256];
    DfFile a, b;                /* Infos und Vergleichsbilder (frames: eigener Speicher) */
    int nframes;
    int dist[DF_MAX_FRAMES], dist_flip[DF_MAX_FRAMES];  /* -1 = nicht vergleichbar */
    double similarity, similarity_flip;                 /* % */
} DfPairTest;
bool df_pair_test(const char *a, const char *b, int frames, bool white_borders, DfPairTest *t);
void df_pair_test_free(DfPairTest *t);

/* Ähnlichkeit zweier Dateien wie beim Suchlauf -> %, < 0 = nicht vergleichbar */
double df_similarity(const DfFile *a, const DfFile *b, bool flip);

/* ---------------------------------------------------------------------------
 * Anzeige-Helfer
 * ------------------------------------------------------------------------- */
char *df_format_bytes(double bytes, char *out, size_t sz);     /* "1,23 GB" */
char *df_format_duration(double secs, char *out, size_t sz);   /* "1:02:03" / "4:05" */
char *df_format_eta(double secs, char *out, size_t sz);        /* "ca. 3 h 05 min" */
char *df_format_date(long long t, char *out, size_t sz);       /* "14.10.2026 13:05" */
const char *df_codec_name(const char *codec);                  /* "h264" -> "H.264" */

#endif
