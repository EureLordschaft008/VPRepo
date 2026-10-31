/*
 * AV1-Converter – Engine (gemeinsam für Terminal- und GUI-Version)
 *
 * Die Engine sucht Videos in einem Ordner (inkl. Unterordner), prüft sie mit
 * ffprobe und wandelt sie nach AV1 um: per CPU mit HandBrake und dem Profil
 * „AV1 MKV 2160p60 4K“ oder per GPU (VAAPI) mit gleichwertigen Einstellungen.
 * Front-ends (ac_cli.c, ac_gui.c) steuern den Ablauf über ac_scan() /
 * ac_convert() und bekommen Meldungen und Fortschritt über die Callbacks in ac_fe.
 */
#ifndef AC_CORE_H
#define AC_CORE_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>

#define AC_PRESET "AV1 MKV 2160p60 4K"     /* HandBrake-Profil, dem alles entspricht */

/* Voreinstellungen (in der Oberfläche änderbar) */
#define DEFAULT_CPU_JOBS 1      /* HandBrake: 1 Job 21,5 Bilder/s (1080p), 3 Jobs nur 25,6 – bei
                                   3,2 GB (4K: 6,2 GB) Arbeitsspeicher je Job */
#define DEFAULT_GPU_JOBS 3      /* VAAPI: 1 Job ~575, 2–3 Jobs ~850 Bilder/s (1080p, CPU-Dekoder);
                                   der dritte hält die GPU ausgelastet, während einer analysiert */
#define DEFAULT_RF 35.0         /* Qualität des Profils (HandBrake 1.11: RF 35, SVT-AV1 Preset 4) */
#define DEFAULT_GPU_QUALITY 64  /* VAAPI-AV1 (0–255): 64 = gleiche Bildqualität wie RF 35 (PSNR/SSIM
                                   gemessen), Dateien dafür ~25 % größer */
#define DEFAULT_MIN_SAVING 10   /* % – kleinere Ersparnis: Original behalten */
#define DISK_MIN_GB 10          /* darunter starten keine neuen Jobs (Warten auf Platz) */
#define MAX_CPU_JOBS 8
#define MAX_GPU_JOBS 4

/* Abbruch: Strg+C, SIGTERM oder "Abbrechen" in der Oberfläche. Laufende
 * Encoder werden beendet, halbfertige Dateien gelöscht, Originale bleiben. */
extern volatile sig_atomic_t ac_cancel;
extern char *AC_APP_DIR, *AC_RUN_LOG, *AC_DONE_DB;

/* ---------------------------------------------------------------------------
 * Daten
 * ------------------------------------------------------------------------- */
typedef enum {
    AC_TODO,                    /* wird umgewandelt */
    AC_SKIP_AV1,                /* ist schon AV1 */
    AC_SKIP_DONE,               /* in einem früheren Lauf umgewandelt */
    AC_SKIP_NOGAIN,             /* früher: AV1 wäre nicht kleiner geworden (auch per HandBrake) */
    AC_SKIP_FAILED,             /* früher: Fehler */
    AC_SKIP_EXISTS,             /* Ergebnis liegt schon im Ausgabeordner */
    AC_SKIP_UNREADABLE,         /* ffprobe findet kein Video */
    AC_STATE_COUNT
} AcState;

typedef struct {
    char *path;                 /* voller Pfad */
    char *rel;                  /* relativ zum Quellordner */
    long long size, mtime;      /* Byte, Unix-Zeit */
    char vcodec[24], pix_fmt[24];
    int vindex;                 /* Index der Haupt-Videospur (kein Cover) */
    int width, height;
    double fps, duration, kbps; /* Bilder/s, Sekunden, Gesamt-Bitrate */
    bool interlaced, hdr;
    bool cpu_only;              /* gleich per HandBrake (CPU): GPU brachte keinen Gewinn bzw. HDR
                                   soll vollständig erhalten bleiben */
    char *streams;              /* Ton/Untertitel: "a1:aac:2,s3:subrip" (Index:Codec[:Kanäle]) */
    AcState state;
} AcVideo;

typedef struct {
    AcVideo *v;                 /* nach Größe absteigend sortiert */
    int n;
    int count[AC_STATE_COUNT];
    long long bytes[AC_STATE_COUNT];
    double secs;
} AcScan;

typedef enum { AC_MODE_CPU, AC_MODE_GPU, AC_MODE_HYBRID } AcMode;
enum { AC_AUDIO_PRESET, AC_AUDIO_ALL_OPUS, AC_AUDIO_ALL_COPY };
enum { AC_SUBS_PRESET, AC_SUBS_ALL };
/* HDR-Videos: Die GPU (VAAPI) behält Farbraum und Kennlinie (BT.2020, PQ/HLG), schreibt aber keine
 * Helligkeitsangaben (Mastering Display, MaxCLL/MaxFALL). Die überträgt mkvpropedit in den
 * MKV-Container; Dolby Vision und HDR10+ (dynamisch) kann nur HandBrake übernehmen. */
enum {
    AC_HDR_GPU_CONTAINER,       /* GPU + Angaben in den MKV-Container (DV/HDR10+: HandBrake) */
    AC_HDR_HANDBRAKE,           /* alle HDR-Videos per HandBrake (vollständig, langsam) */
    AC_HDR_GPU_PLAIN            /* GPU ohne Helligkeitsangaben */
};

typedef struct {
    AcMode mode;
    int cpu_jobs, gpu_jobs;     /* parallele Jobs */
    double rf;                  /* HandBrake-Qualität (RF) */
    int gpu_quality;            /* VAAPI: 0–255, kleiner = besser */
    int audio, subs;            /* AC_AUDIO_*, AC_SUBS_* */
    int min_saving;             /* % */
    bool replace;               /* Originale ersetzen (sonst Ausgabeordner) */
    bool keep_mtime;            /* Änderungsdatum des Originals übernehmen */
    int hdr;                    /* AC_HDR_*: wie HDR-Videos umgewandelt werden */
    const char *out_dir;        /* Ausgabeordner (nur ohne replace) */
} AcOptions;

void ac_options_default(AcOptions *o);

typedef struct {
    int ok, nogain, err;
    int gpu_files, cpu_files, fallback_files;   /* umgewandelt per GPU / CPU / CPU nach GPU-Fehler */
    long long bytes_in, bytes_out;              /* nur erfolgreiche Dateien */
    double secs;
    bool aborted;               /* abgebrochen */
} AcStats;

/* ---------------------------------------------------------------------------
 * Front-end-Schnittstelle (Callbacks können aus Worker-Threads kommen)
 * ------------------------------------------------------------------------- */
typedef struct {
    void (*log)(const char *msg);                       /* sichtbare Meldung */
    void (*scan_progress)(int found, int probed);       /* Suche: gefunden / geprüft (-1: Ordner
                                                           werden noch gelesen) */
    void (*file_done)(int done, int total, bool ok);
    /* Verschieben an den Platz der Originale: Fortschritt nach Dateien und Byte */
    void (*move_progress)(int done, int total, double bytes_done, double bytes_total);
} AcFrontend;

extern AcFrontend ac_fe;

/* ---------------------------------------------------------------------------
 * Ablauf
 * ------------------------------------------------------------------------- */
void ac_init(void);                             /* Pfade, Prioritäten */
bool ac_have_ffmpeg(void);
bool ac_have_mkvpropedit(void);                 /* mkvtoolnix: HDR-Angaben in den Container */
void ac_log_open(void);                         /* neues Lauf-Log */
__attribute__((format(printf, 2, 3))) void ac_log(bool show, const char *fmt, ...);
int ac_cleanup_temps(void);                     /* -> Anzahl entfernter Temp-Dateien */

/* HandBrake suchen (Flatpak fr.handbrake.ghb bevorzugt, sonst HandBrakeCLI) und das
 * Profil auslesen. Dauert einige Sekunden. */
typedef struct {
    bool available, flatpak;
    char version[32];
    double rf;                  /* Qualität laut Profil */
    char svt_preset[8];         /* SVT-AV1-Preset laut Profil */
    char encoder[32];
} AcHandbrake;
bool ac_handbrake_detect(AcHandbrake *hb);

/* GPU suchen und mit einem Mini-Encode testen (AV1, 10 Bit). Dauert ~1 s. */
typedef struct {
    bool available;
    char device[64];            /* /dev/dri/renderD128 */
    char name[96];              /* z. B. "AMD Radeon RX 7600" */
} AcGpu;
bool ac_gpu_detect(AcGpu *gpu);

/* Quellordner durchsuchen und alle Videos prüfen (parallel, mit Cache).
 * -> 0 = ok, 1 = abgebrochen, -1 = Fehler (err: Meldung) */
int ac_scan(const char *src, const AcOptions *o, AcScan *s, const char **err);
void ac_scan_free(AcScan *s);

/* sel[0..n) umwandeln -> false bei Abbruch */
bool ac_convert(AcVideo *const *sel, int n, const AcOptions *o, const AcHandbrake *hb,
                const AcGpu *gpu, AcStats *st);

typedef struct {                /* laufender Job (für Statusanzeigen) */
    char rel[1024];
    char what[64];              /* Phase, z. B. "Analyse", "Umwandeln", "Prüfen" */
    double pct, fps, secs, eta; /* eta < 0: unbekannt */
    bool gpu;
    bool fallback;              /* CPU-Neuversuch nach GPU-Fehler */
    bool hw_decode;             /* GPU-Job: die GPU dekodiert (sonst die CPU) */
} AcJobInfo;
int ac_jobs_snapshot(AcJobInfo *out, int max);

typedef struct {                /* Gesamtfortschritt (Byte der Quelldateien) */
    double bytes_done, bytes_total;
    long long saved;            /* bisher eingespart */
    long long in_ok, out_ok;    /* fertige Dateien: vorher / nachher */
    int files_done, files_total;
    double secs;                /* Laufzeit ohne Pausen */
    double eta;                 /* Restdauer (s) */
    bool eta_measured;          /* nach dem gemessenen Tempo (sonst nur Modell) */
    double est_total;           /* geschätzte Gesamtdauer zu Beginn */
} AcProgress;
bool ac_progress(AcProgress *p);                /* -> false, wenn gerade nichts läuft */

/* geschätzte Dauer (s) einer Datei bzw. einer Auswahl – nach den gemessenen Werten,
 * für die eingestellte Hardware und Jobanzahl */
double ac_estimate_secs(const AcVideo *v, const AcOptions *o, bool gpu_available);
double ac_estimate_total(AcVideo *const *sel, int n, const AcOptions *o, bool gpu_available);

/* Pausieren/Fortsetzen (aus jedem Thread): laufende Encoder werden angehalten,
 * neue nicht gestartet. Ein neuer Lauf beginnt immer unpausiert. */
void ac_set_paused(bool on);
bool ac_paused(void);
bool ac_cpu_load(double *cpu);                  /* CPU-Last (%) während der Umwandlung */

/* Liste der bearbeiteten Dateien (~/.av1-converter/erledigt.tsv) */
int ac_done_count(void);

/* Ergebnisse im Ausgabeordner, deren Original noch da ist – lassen sich jederzeit (auch nach
 * Programmende) an den Platz der Originale verschieben (~/.av1-converter/ausgabe.tsv) */
typedef struct {
    int files;
    long long orig_bytes, dest_bytes;   /* Originale / Ergebnisse */
} AcPending;
bool ac_pending_info(AcPending *p);     /* -> false, wenn nichts ansteht */

typedef struct {
    int moved, skipped, failed, left;   /* left: bleibt für später (Fehler, Abbruch) */
    int dirs_removed;                   /* leere, vom Programm angelegte Ausgabeordner */
    long long orig_bytes, dest_bytes;   /* verschobene: gelöschte Originale / Ergebnisse */
} AcMoveStats;
/* Ergebnisse an den Platz der Originale verschieben (Originale werden gelöscht; ein
 * geändertes oder fehlendes Original wird übersprungen) -> false bei Abbruch (ac_cancel) */
bool ac_move_to_originals(AcMoveStats *ms);
bool ac_done_clear(bool only_failed);           /* alle bzw. nur Fehler/kein Gewinn vergessen */

/* Anzeige-Helfer */
char *ac_format_bytes(double bytes, char *out, size_t sz);     /* "1,23 TB" */
char *ac_format_duration(double secs, char *out, size_t sz);   /* "3 Tage 4 h", "4:05 min" */
char *ac_format_finish(double secs_from_now, char *out, size_t sz); /* "morgen, 14:20" */
char *ac_format_remaining(double secs, char *out, size_t sz);  /* "ca. 3 h 05 min" */
const char *ac_codec_name(const char *codec);                  /* "h264" -> "H.264" */
double ac_estimated_saving(const AcVideo *v);                  /* grobe Schätzung 0..1 */

#endif
