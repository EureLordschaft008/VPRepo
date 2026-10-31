/*
 * Intro-Cutter – Engine (gemeinsam für Terminal- und GUI-Version)
 *
 * Die Engine erkennt ein wiederkehrendes Intro per perceptual Hash und
 * schneidet es mit ffmpeg heraus. Front-ends (ic_cli.c, ic_gui.c) steuern den
 * Ablauf über ic_detect() / ic_process() und bekommen Meldungen, Fortschritt
 * und Rückfragen über die Callbacks in ic_fe.
 */
#ifndef IC_CORE_H
#define IC_CORE_H

#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Einstellungen, die auch die Oberflächen anzeigen */
#define DEFAULT_THRESHOLD 4     /* Hamming-Distanz: kleiner = strenger, größer = toleranter */
#define CPU_TARGET 85           /* harte Obergrenze in % – darüber drosselt der Governor */
#define GPU_WORKERS 3           /* parallele VAAPI-Encodes */
#define GPU_TARGET 95           /* GPU-Auslastung in %: darüber werden GPU-Jobs kurz pausiert */
#define DISK_MIN_GB 10          /* darunter pausiert die Bearbeitung und fragt nach */
#define IC_MAX_FRAMES 9         /* höchstens so viele Vergleichsbilder je Video */
#ifndef GPU_ENABLE
#define GPU_ENABLE 1            /* 0 = nie GPU (z. B. make CFLAGS="-O2 -DGPU_ENABLE=0") */
#endif

extern int CPU_COUNT, MAX_WORKERS, POOL_THREADS;
extern char *APP_DIR, *LOG_FILE, *AUTO_LOG_FILE, *RUN_LOG, *HASH_CACHE, *SELFTEST_CLIP;

/* Abbruch: Strg+C, SIGTERM, Eingabe-Ende oder "Abbrechen" in der Oberfläche.
 * Laufende ffmpeg-Prozesse werden beendet, Ergebnisse danach verworfen. */
extern volatile sig_atomic_t ic_cancel;

/* ---------------------------------------------------------------------------
 * Kleine Helfer
 * ------------------------------------------------------------------------- */
typedef struct {                /* dynamischer Byte-Puffer (immer NUL-terminiert) */
    char *p;
    size_t n, cap;
} Buf;

typedef struct {                /* Liste eigener Strings (NULL-terminiert) */
    char **v;
    int n, cap;
} StrList;

void *xmalloc(size_t n);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
__attribute__((format(printf, 1, 2))) char *xasprintf(const char *fmt, ...);
void buf_add(Buf *b, const void *d, size_t n);
void buf_add_str(Buf *b, const char *s);
void buf_add_char(Buf *b, char c);
__attribute__((format(printf, 2, 3))) void buf_printf(Buf *b, const char *fmt, ...);
const char *buf_str(Buf *b);
char *buf_detach(Buf *b);
void buf_free(Buf *b);
void strlist_add(StrList *l, const char *s);
void strlist_free(StrList *l);
bool parse_float(const char *s, double *out);
bool parse_int(const char *s, long *out);
char *format_duration(double s, char *out, size_t sz);
char *normpath(const char *path);
char *expanduser(const char *p);
char *realpath_lenient(const char *path);
char *path_join(const char *a, const char *b);
bool is_file(const char *p);
bool is_dir(const char *p);
double monotonic_secs(void);
/* Anzeige wie Pythons backslashreplace: ungültige UTF-8-Bytes als "\udcXX" */
char *utf8_escape(const char *s);

/* ---------------------------------------------------------------------------
 * Daten
 * ------------------------------------------------------------------------- */
typedef struct {
    char *path;                 /* Quellordner + relativer Pfad */
    char *rel;                  /* relativ zum Quellordner */
    double cut_len;             /* eigene Schnittlänge (s, automatische Erkennung); 0 = die
                                 * gemeinsame aus ic_process. Copy schneidet dann am letzten
                                 * Keyframe im Intro (nimmt nie Inhalt mit) */
} Video;

typedef struct {
    Video *v;
    int n, cap;
} VideoList;

typedef struct {
    const Video *v;
    int dist;                   /* Hamming-Distanz zum Referenz-Frame (Median aller Bilder) */
    int order;                  /* für stabile Sortierung */
    int nd;                     /* Anzahl Vergleichsbilder */
    unsigned char d[IC_MAX_FRAMES];     /* Distanz je Bild, 255 = nicht lesbar */
} Result;

typedef struct {                /* VAAPI-Fähigkeiten eines Geräts */
    char device[PATH_MAX];
    bool h264, hevc, av1;       /* Encoder nutzbar */
    bool hevc10, av110;         /* 10-Bit-Encode (p010) */
    int hwdecode;               /* -1 = nicht gesetzt (gilt als ja), 0/1 */
} GpuCaps;

typedef struct {                /* Schnittmethode (z. B. "Copy", "GPU HEVC") und wie oft sie lief */
    char method[48];
    int files;
} MethodCount;

typedef struct {
    int ok, err, replaced;
    int move_err;               /* nicht an den Platz der Originale verschoben */
    bool aborted;               /* bei wenig Platz vom Nutzer beendet */
    long long saved_bytes;
    double total_cut, max_trim;
    int trimmed_files;
    /* Copy bei automatisch erkannten Intros: Keyframe im Intro, ein Rest davon blieb stehen */
    double max_rest;
    int rest_files;
    int method_count;
    MethodCount methods[32];    /* Reihenfolge des ersten Auftretens */
} Stats;

/* ---------------------------------------------------------------------------
 * Front-end-Schnittstelle (Callbacks können aus Worker-Threads kommen)
 * ------------------------------------------------------------------------- */
typedef struct {
    void (*log)(const char *msg);   /* sichtbare Meldung (steht immer auch im Lauf-Log) */
    void (*info)(const char *msg);  /* Hinweis nur für die Anzeige */
    void (*detect_start)(int videos, int from_cache, int to_read);
    void (*detect_progress)(int done, int total, int workers, int max_workers, double cpu);
    /* automatische Erkennung: neuer Abschnitt der Suche (z. B. "Intros werden verglichen …") */
    void (*detect_stage)(const char *text);
    void (*file_done)(int done, int total, bool ok);
    /* wenig Speicherplatz (Keep-Modus): fertige Dateien verschieben? weiterlaufen? */
    bool (*ask_disk_move)(double free_gb, int n_finished);
    bool (*ask_disk_continue)(double free_gb);
    /* Verschieben an den Platz der Originale: Fortschritt nach Dateien und Byte */
    void (*move_progress)(int done, int total, double bytes_done, double bytes_total);
    /* Statuszeile "… läuft: …", wenn länger keine Datei fertig wird, und alle paar Minuten
     * eine Zeile mit der Restdauer – nur für Oberflächen ohne eigene Anzeige (Terminal) */
    bool show_heartbeat;
} IcFrontend;

extern IcFrontend ic_fe;

/* ---------------------------------------------------------------------------
 * Ablauf
 * ------------------------------------------------------------------------- */
void ic_init(void);                             /* Konfiguration und Pfade */
bool ic_have_ffmpeg(void);
void ic_setup_dependencies(void);               /* installiert ffmpeg per apt (Terminal) */
void ic_log_open(void);                         /* neues Lauf-Log anlegen */
__attribute__((format(printf, 2, 3))) void ic_log(bool show, const char *fmt, ...);
int ic_cleanup_temps(void);                     /* -> Anzahl entfernter Temp-Dateien */

bool ic_gpu_detect(GpuCaps *out);
char *ic_gpu_summary(const GpuCaps *caps);      /* caps NULL = keine GPU */

/* cut_len: Länge des Intros bzw. Outros in Sekunden – das Stück, das abgeschnitten wird */

/* Zeitpunkt des Vergleichsframes (Mitte von cut_len) in Python-Darstellung, z. B. "2.5" */
void ic_ref_timestamp(double cut_len, char *ts, size_t sz);
/* frames Vergleichsbilder, gleichmäßig über das Intro verteilt (frames = 1: Mitte).
 * ic_frames_for: tatsächliche Anzahl (ungerade, bei sehr kurzen Intros weniger) */
int ic_frames_for(double cut_len, int frames);
void ic_ref_timestamps(double cut_len, int frames, char ts[][24]);
/* Vergleichsframe als PNG (Vorschau, max. 320 px breit); outro: vom Ende gemessen */
bool ic_ref_frame_png(const char *video, double cut_len, bool outro, const char *png);

typedef struct {
    VideoList videos;           /* alle gefundenen Videos */
    Result *results;            /* lesbare Videos, nach Distanz sortiert (stabil) */
    int nres, nmatch, skipped;  /* results[0..nmatch) liegen im Schwellwert */
    double secs;
    char ts[64];                /* Intro-Mitte */
    int frames;                 /* Vergleichsbilder je Video */
    char tss[IC_MAX_FRAMES][24];
} IcDetect;

/* frames: Vergleichsbilder je Video (1 = schnell wie die Python-Version; 3, 5, 9 =
 * gründlicher: Distanz ist der Median über alle Bilder). outro: Outro statt
 * Intro suchen – die Bilder werden dann vom Ende aus gemessen.
 * -> 0 = ok, 1 = abgebrochen, -1 = Fehler (err: Meldung) */
int ic_detect(const char *ref, double cut_len, const char *src, const char *out,
              int threshold, int frames, bool outro, IcDetect *d, const char **err);
void ic_detect_free(IcDetect *d);

/* ---------------------------------------------------------------------------
 * Automatische Erkennung: wiederkehrende Intros ohne Referenz-Video finden – auch
 * mehrere verschiedene im selben Quellordner. Je Video ein eigener, bildgenauer Schnitt.
 * Lieber nicht schneiden als Inhalt entfernen: "unsicher" wird nicht vorausgewählt.
 * ------------------------------------------------------------------------- */
#define IC_AUTO_MAX_DEFAULT 20      /* Suchlänge: Intro höchstens so lang (s) */
#define IC_AUTO_MIN_VIDEOS 5        /* ein Intro muss in so vielen Videos vorkommen */
#define IC_AUTO_MIN_INTRO 1.5       /* kürzere Gemeinsamkeiten zählen nicht (s) */

typedef struct {
    int group;                  /* Intro-Gruppe (Index in IcAuto.groups), -1 = keine */
    bool sure;                  /* sicher erkannt: wird vorausgewählt */
    double cut_len;             /* Schnitt (s ab Dateistart wie -ss), 0 = keiner möglich */
    double last_intro_t;        /* letztes Bild des Intros (s ab Dateistart) */
    double first_content_t;     /* erstes Bild danach */
    char reason[160];           /* unsicher bzw. kein Intro: warum */
} IcAutoVideo;

typedef struct {
    int members;                /* Videos mit diesem Intro */
    int sure;                   /* davon sicher erkannt */
    int representative;         /* typisches Video der Gruppe (Index in IcAuto.videos) */
    double length;              /* typische Intro-Länge (Median der Schnitte, s) */
} IcAutoGroup;

typedef struct {
    VideoList videos;           /* alle gefundenen Videos, nach Pfad sortiert */
    IcAutoVideo *info;          /* je Video (gleicher Index wie videos.v) */
    IcAutoGroup *groups;        /* nach Größe absteigend */
    int ngroups;
    int unreadable;             /* Videos ohne lesbare Bilder */
    double max_len;             /* Suchlänge (s) */
    int min_videos;
    double secs;                /* Dauer der Suche */
} IcAuto;

/* alle Videos unter src (ohne out) untersuchen: Intros höchstens max_len Sekunden, in
 * mindestens min_videos Videos -> 0 = ok, 1 = abgebrochen, -1 = Fehler (err: Meldung) */
int ic_detect_auto(const char *src, const char *out, double max_len, int min_videos, IcAuto *a,
                   const char **err);
void ic_auto_free(IcAuto *a);

typedef struct {
    Stats stats;
    StrList fin_dest, fin_orig; /* Keep-Modus: fertig, noch nicht verschoben */
    StrList created_dirs;       /* in diesem Lauf angelegte Ausgabeordner (Reihenfolge) */
    int dirs_removed;           /* leere Ausgabeordner, die ic_move_finished entfernt hat */
    double secs;
} IcRun;

/* mode: "copy", "cpu", "gpu" oder "hybrid" (GPU + CPU parallel).
 * outro: die letzten cut_len Sekunden abschneiden statt der ersten. -> false bei Abbruch */
bool ic_process(const Video *const *sel, int n, const char *out, double cut_len, bool outro,
                const char *mode, bool delete_orig, const GpuCaps *gpu, IcRun *r);
/* fertige Dateien an den Platz der Originale verschieben, danach die leer gewordenen
 * Ausgabeordner dieses Laufs entfernen -> Anzahl verschobener Dateien */
int ic_move_finished(IcRun *r);
void ic_run_free(IcRun *r);

typedef struct {                /* laufender Job (für Statusanzeigen) */
    char rel[1024];
    char what[48];
    double pct, secs;           /* secs: Laufzeit ohne Pausen */
    bool gpu;                   /* GPU-Job (sonst CPU bzw. Copy) */
} IcJobInfo;

int ic_jobs_snapshot(IcJobInfo *out, int max);

/* Restdauer der laufenden Bearbeitung, geschätzt aus dem bisherigen Tempo: Größe der Dateien
 * plus fester Aufwand je Datei (aus den fertigen gelernt), laufende Dateien anteilig, Pausen
 * ausgenommen, geglättet. Aus jedem Thread abfragbar. */
typedef struct {
    double fraction;            /* erledigter Anteil der Arbeit, 0..1 */
    double remaining;           /* Restdauer in s (ohne Pausen), < 0 = noch unbekannt */
    double elapsed;             /* bisherige Bearbeitungszeit ohne Pausen (s) */
    bool paused;                /* vom Nutzer pausiert – die Restdauer steht still */
} IcEstimate;

bool ic_process_estimate(IcEstimate *e);        /* -> false, wenn keine Bearbeitung läuft */
/* als Text: "ca. 25 Min. · fertig gegen 23:40", "ca. 2 Std. 10 Min. · fertig morgen gegen
 * 01:30", "weniger als 1 Min.", "ca. 25 Min. (pausiert)" bzw. "wird ermittelt …";
 * ohne with_clock fehlt die Uhrzeit ("ca. 25 Min.") */
void ic_format_estimate(const IcEstimate *e, bool with_clock, char *out, size_t size);

/* GPU-Bremse während der Bearbeitung: geglättete GPU-Last (%) und Anteil der
 * Pausen (0..1) -> false, wenn gerade nicht gemessen wird */
bool ic_gpu_load(double *load, double *pause);
/* CPU-Last (%) während der Bearbeitung -> false, wenn keine läuft */
bool ic_cpu_load(double *cpu);

/* Bearbeitung pausieren/fortsetzen (aus jedem Thread): laufende ffmpeg-Jobs
 * werden angehalten, neue nicht gestartet; Watchdog und Laufzeiten zählen die
 * Pause nicht mit. Ein neuer Lauf beginnt immer unpausiert. */
void ic_set_paused(bool on);
bool ic_paused(void);

/* Vorschaubilder: Container-Dauer (s, 0 = unbekannt) und ein Bild bei t s ab
 * Dateistart als RGB24 (w x h, Seitenverhältnis erhalten, schwarzer Rand) */
double ic_duration(const char *video);
bool ic_frame_rgb(const char *video, double t, int w, int h, unsigned char *rgb);

/* ---------------------------------------------------------------------------
 * Videovorschau: Intro-Ende bzw. Outro-Beginn direkt am Video festlegen
 * ------------------------------------------------------------------------- */
typedef struct {
    double duration;            /* s, 0 = unbekannt */
    double fps;                 /* Bilder je Sekunde, 0 = unbekannt */
    int width, height;          /* Anzeigegröße (Pixel-Seitenverhältnis eingerechnet) */
    bool has_audio;
} IcVideoProps;

bool ic_video_props(const char *video, IcVideoProps *p);   /* -> false, wenn nicht lesbar */

/* Bilder eines Videos fortlaufend aus einem ffmpeg-Prozess: ab start für length Sekunden,
 * fps Bilder je Sekunde als RGB24 (w x h, proportional mit schwarzem Rand). thumbs:
 * schnell und grob dekodieren (Bilder der Zeitleiste), sonst volle Qualität (Wiedergabe);
 * sound: den Ton zugleich über PulseAudio/PipeWire abspielen. Gelesen wird in einem
 * eigenen Thread; *stop (aus jedem Thread setzbar) beendet ein wartendes Lesen sofort. */
typedef struct IcFrameStream IcFrameStream;
IcFrameStream *ic_frame_stream_open(const char *video, double start, double length, int w, int h,
                                    double fps, bool thumbs, bool sound);
/* nächstes Bild und sein Zeitpunkt ab Dateistart -> false am Ende, bei Fehler oder Stopp */
bool ic_frame_stream_read(IcFrameStream *s, unsigned char *rgb, double *t,
                          const volatile int *stop);
void ic_frame_stream_close(IcFrameStream *s);           /* beendet ffmpeg und gibt frei */
/* höchstens count aufeinanderfolgende Bilder ab dem ersten Bild, das bei oder nach from liegt,
 * und vor until (until <= from: ohne Ende) – jedes Bild genau einmal, so wie es im Video
 * steht (für Bild-für-Bild-Schritte); lesen mit ic_frame_stream_read (der gelieferte
 * Zeitpunkt ist dabei ohne Bedeutung) */
IcFrameStream *ic_frame_stream_open_frames(const char *video, double from, double until,
                                           int count, int w, int h);
/* Zeitpunkte aller Bilder des Hauptvideos zwischen start und start + length (s ab Dateistart,
 * so wie -ss sie versteht), aufsteigend -> Anzahl (0 = nicht ermittelbar); *times mit free()
 * freigeben */
int ic_frame_times(const char *video, double start, double length, double **times);
bool ic_sound_supported(void);                          /* ffmpeg kann Ton ausgeben */

/* ---------------------------------------------------------------------------
 * Hash-Cache ansehen und aufräumen (nicht während einer Suche aufrufen)
 * ------------------------------------------------------------------------- */
typedef struct {
    char *path;                 /* Video, wie im Cache gespeichert */
    long long size, mtime;      /* Dateigröße (Byte) und Änderungszeit (s) laut Cache */
    int frames;                 /* gespeicherte Vergleichsbilder (Intro und Outro) */
    int outro_frames;           /* davon vom Ende gemessen (Outro) */
} IcCacheVideo;

typedef struct {
    IcCacheVideo *videos;       /* nach Pfad sortiert */
    int n;
    long entries, other;        /* Einträge insgesamt, davon in fremdem Format */
    bool hash_file;             /* hash_cache.json vorhanden */
    long long file_bytes;       /* Größe von hash_cache.json */
    double modified;            /* letzte Änderung (Unix-Zeit) */
    long long kb_bytes;         /* Kennbilder der automatischen Erkennung (kennbilder.bin) */
    int kb_videos;              /* ... für so viele Videos */
} IcCacheInfo;

/* -> false, wenn es keine Cache-Datei gibt (weder Hash- noch Kennbilder-Cache) */
bool ic_cache_read(IcCacheInfo *info);
void ic_cache_info_free(IcCacheInfo *info);
/* die Einträge der Videos paths[0..n) entfernen (beide Caches) -> Anzahl entfernter
 * Einträge, -1 = Fehler */
long ic_cache_remove_videos(const char *const *paths, int n);
/* Einträge für Videos entfernen, die es nicht mehr gibt oder die sich geändert haben
 * (andere Größe/Änderungszeit; beide Caches) -> Anzahl entfernter Einträge, -1 = Fehler */
long ic_cache_remove_stale(void);
bool ic_cache_clear(void);                      /* beide Cache-Dateien löschen -> false = Fehler */

#endif
