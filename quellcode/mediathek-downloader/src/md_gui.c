/*
 * Mediathek Downloader – grafische Oberfläche (GTK 3)
 * ===================================================
 * Aufbau und Optik wie beim AV1-Converter. Fünf Seiten, oben umschaltbar:
 *   Vorschau       Filme und Serien als Kacheln mit Bild; Häkchen sammeln eine Auswahl, die
 *                  Detailansicht zeigt Serie -> Staffel -> Folge mit dem Stand jeder Folge
 *   Meine Videos   oben die neuen Folgen schon geladener Serien seit dem letzten Download,
 *                  darunter alles Geladene als Kacheln, Serien mit Staffeln und Folgen;
 *                  abgespielt wird im eingebauten Player (md_player.c), der sich den Stand
 *                  jeder Datei merkt
 *   Warteschlange  Downloads mit Pause, Fortsetzen und Tempolimit
 *   Statistik      Sehzeit, Downloads und Platz
 *   Einstellungen  Speicherort, Qualität, AV1
 *
 * Die Startliste läuft in einem eigenen Thread, das Ergebnis kommt über g_idle_add() in den
 * GTK-Hauptthread. Die Warteschlange arbeitet in der Engine (md_core.c); ein Timer holt alle
 * 500 ms den Stand ab. Der gesamte Zustand der Oberfläche liegt in der Struktur "ui".
 *
 * Aufbau dieser Datei: kleine Helfer – Einstellungen – Trefferbaum – Spurenauswahl –
 * Seite Vorschau – Neue Folgen – Seite Warteschlange – Seite Meine Videos –
 * Seite Einstellungen –
 * Fenster, Menü, Programmstart
 *
 * Aufruf:
 *     mediathek-downloader
 */
#define _GNU_SOURCE
#include <errno.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#ifdef G_OS_UNIX
#include <glib-unix.h>
#include <unistd.h>
#endif
#include <locale.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include "md_core.h"
#include "md_player.h"
#include "md_stats.h"

#define APP_ID "de.mediathekdownloader.MediathekDownloader"
#define APP_NAME "Mediathek Downloader"
#define SETTINGS_GROUP "Einstellungen"
#define SETTINGS_FILE "einstellungen.ini"
#define RESULT_ROWS_BATCH 200   /* Zeilen der Trefferliste auf einmal (weitere per Knopf) */
#define TICK_MS 500
#define TICK_IDLE 10            /* ohne Arbeit: nur jeder so vielte Takt sieht nach */
#define TICK_HIDDEN 4            /* Fenster verborgen, Aufträge laufen */
#define INFO_LINES_MAX 3        /* so viele Meldungen zeigt die Leiste übereinander */
#define INFO_SECONDS 10         /* so lange bleibt die Meldungsleiste stehen */
#define JOB_ROWS_BATCH 100      /* Zeilen der Downloadliste auf einmal (weitere per Knopf) */
#define PREVIEW_TILES_BATCH 30  /* Kacheln je Abschnitt auf einmal (weitere per Knopf) */
#define PREVIEW_FILM_MIN_MINUTES 40
#define COVER_WIDTH 224
#define COVER_HEIGHT 126
#define COVER_THREADS 4
#define COVER_CACHE_MAX 1000    /* Titelbilder im Speicher, je etwa 85 KB */
#define UPDATE_DELAY_SECONDS 8  /* Update-Suche nach dem Start, wenn die Vorschau steht */
#define NEWS_INTERVAL_SECONDS (6 * 3600)  /* neue Folgen erneut suchen, wenn sie von selbst laden */
#define HOVER_DELAY_MS 500     /* so lange ruht die Maus auf der Kachel bis zum Popup */
#define GENRE_TICKS 4           /* neue Kategorien alle so vielen Timer-Schläge zeigen */
#define FOOTER_PREVIEW 0
#define FOOTER_NEWS 1
#define FOOTER_COUNT 2
#define NO_GENRE "Ohne Kategorie"
#define MD_SOURCE_ALL_1_4 7     /* so stand "Alle" früher in den Einstellungen */
#define MD_SOURCE_ALL_1_5 511
#define MD_SOURCE_ALL_1_6 1023
#define MD_SOURCE_ALL_1_8 131071
#define MD_SOURCE_ALL_1_9 ((1 << 29) - 1)
#define IMDB_THREADS 2
#define ALL_LANGUAGES "Alle Sprachen"
#define SOURCE_COUNT 25
#define WATCH_FILE "wiedergabe.tsv"
#define VIDEO_GENRES_FILE "kategorien.tsv"  /* Kategorien der geladenen Serien und Filme */
#define WATCHED_SHARE 0.95      /* ab diesem Anteil gilt eine Datei als gesehen (Abspann) */
#define RESUME_MIN_SECONDS 10   /* kürzer angeschaut zählt nicht als angefangen */
#define VIDEOS_ROW_TILES 12     /* Kacheln in "Weiterschauen" */
#define FRAME_THREADS 2
#define ROW_SCROLL_STEP 60.0    /* Pixel je Schritt des Mausrads über einer Reihe */
#define TILES_PER_LINE_MAX 24   /* auch sehr breite Fenster füllen sich mit Kacheln */
#define COMPACT_TABS_WIDTH 1040 /* schmaler: Reiter nur als Symbole */

/* Reihenfolge der Quellenleiste: ZDF-Familie, ARD-Familie, übrige, eigene Schnittstellen */
static const int SOURCE_BITS[SOURCE_COUNT] = {
    MD_SOURCE_ZDF, MD_SOURCE_ZDFNEO, MD_SOURCE_ZDFTIVI, MD_SOURCE_ZDFINFO, MD_SOURCE_PHOENIX,
    MD_SOURCE_ARD, MD_SOURCE_ONE, MD_SOURCE_ARDALPHA, MD_SOURCE_TAGESSCHAU24, MD_SOURCE_WDR,
    MD_SOURCE_SWR, MD_SOURCE_NDR, MD_SOURCE_MDR, MD_SOURCE_BR, MD_SOURCE_HR, MD_SOURCE_SR,
    MD_SOURCE_RBB, MD_SOURCE_KIKA, MD_SOURCE_FUNK, MD_SOURCE_3SAT, MD_SOURCE_ARTE,
    MD_SOURCE_ARTE_INTL, MD_SOURCE_SRF, MD_SOURCE_NRK, MD_SOURCE_NETZKINO};
static const char *const KIND_TEXT[3] = {"Alles", "Filme", "Serien"};
static const char *const QUALITY_TEXT[3] = {"Hoch – 1080p (ca. 4,5 GB je 90 min)",
                                            "Mittel (ca. 2,3 GB je 90 min)",
                                            "Niedrig (ca. 0,5 GB je 90 min)"};
#ifdef G_OS_WIN32
static const char *const ENCODER_TEXT[2] = {"Grafikkarte – schnell (NVENC, QSV oder AMF)",
                                            "Prozessor – langsam (SVT-AV1)"};
#else
static const char *const ENCODER_TEXT[2] = {"Grafikkarte – schnell (VAAPI)",
                                            "Prozessor – langsam (SVT-AV1)"};
#endif
/* Qualitätsstufen der Umwandlung: Werte für Grafikkarte und Prozessor (kleiner = besser);
 * passt die Einstellung zu keiner Stufe, gilt "Eigene Werte" */
static const struct {
    const char *text;
    int gpu, cpu;
} AV1_LEVELS[] = {{"Gut – kleinste Dateien", 80, 40},
                  {"Sehr gut – empfohlen", 64, 35},
                  {"Beste – größere Dateien", 48, 28}};
#define AV1_LEVEL_CUSTOM G_N_ELEMENTS(AV1_LEVELS)

typedef enum { ROW_GROUP, ROW_SEASON, ROW_ITEM } RowKind;

/* ein Treffer samt Stand */
typedef struct {
    MdTitle *title;
    char *path;                 /* vorhandene Datei */
    char *key;                  /* md_title_key, erst bei Bedarf */
    MdStatus status;
    bool selected;
} Item;

typedef struct {
    int number;                 /* -1 = ohne Staffelangabe */
    GPtrArray *items;           /* Item* */
    bool expanded;
} Season;

enum { IMDB_UNKNOWN, IMDB_LOADING, IMDB_FOUND, IMDB_NOT_FOUND };

/* eine Serie, Sendung oder ein einzelner Film */
typedef struct {
    char *name;
    bool is_film;
    GPtrArray *seasons;         /* Season* */
    bool expanded;
    int source;                 /* MD_SOURCE_* */
    /* nur Vorschau */
    char *key, *website, *video, *description, *cover_path;
    gint64 newest;
    bool complete;              /* alle Folgen da (die Startliste enthält nur neue) */
    bool has_news;              /* Serie: neue Folgen seit dem letzten Download */
    bool loading, select_after_load, cover_requested;
    GdkPixbuf *cover;
    char **genres;              /* Kategorien; NULL = noch nicht bekannt */
    char **imdb_genres;         /* von IMDb, warten auf die der Seite */
    char *fsk;
    MdImdbInfo imdb;
    int imdb_state;             /* IMDB_UNKNOWN … */
    char *found_by;             /* Suchbegriff (klein), mit dem die Suche den Titel fand */
    GtkWidget *tile_image, *tile_check, *tile_sub, *tile_badges;
} Group;

/* angelegte Zeile der Trefferliste */
typedef struct {
    RowKind kind;
    gpointer data;              /* Group*, Season* oder Item* */
    GtkWidget *check;
} RowRef;

/* eine Liste mit dem Baum Serie -> Staffel -> Folge */
typedef struct {
    GtkWidget *list;
    GPtrArray *refs;            /* RowRef* der angelegten Zeilen */
    GPtrArray *groups;          /* Group* */
    int limit;                  /* angezeigte Zeilen */
    const char *empty_text;     /* NULL = "Keine passenden Einträge." */
} ResultTree;

/* Neue Folgen der geladenen Serien im Hintergrund suchen */
typedef struct {
    char **topics;              /* Serien aus dem Verlauf */
    gint64 *finished;           /* je Serie: letzter Download */
    GHashTable *offers;         /* wie ui.season_offers, für die geprüften Serien */
    int total;
    char *dir;
    int min_minutes;
    bool hide_variants;
    GPtrArray *items;           /* Item* */
} NewsJob;

/* Kategorien eines Titels von IMDb */
typedef struct {
    char *key, *name;
    bool is_film;
    bool videos;                /* für "Meine Videos" statt für die Vorschau */
    char **genres;
} GenreJob;

/* Startliste der Vorschau im Hintergrund */
typedef struct {
    int min_minutes;
    bool hide_variants;
    MdCache cache;
    char *dir, *error;
    GPtrArray *items;           /* Item* */
} PreviewJob;

/* freie Suche in allen Mediatheken */
typedef struct {
    char *text, *dir, *error;
    int min_minutes;
    bool hide_variants, truncated;
    GPtrArray *items;           /* Item* */
} SearchJob;

/* alle Folgen einer Serie im Hintergrund */
typedef struct {
    char *key, *topic, *dir, *error;
    int min_minutes;
    bool hide_variants;
    MdCache cache;
    GPtrArray *items;           /* Item* */
} SeriesJob;

/* Bild und Kategorien eines Titels im Hintergrund */
typedef struct {
    char *key, *website, *video, *path;
    bool background;            /* nur Kategorien; sichtbare Kacheln gehen vor */
    GdkPixbuf *pixbuf;
    MdPageInfo *info;
} CoverJob;

/* fertig verkleinertes Titelbild im Speicher */
typedef struct {
    GdkPixbuf *pixbuf;
    char *path;
} CachedCover;

/* Bild einer Kachel in "Meine Videos" im Hintergrund */
typedef struct {
    char *key, *video, *website;
    GdkPixbuf *pixbuf;
} FrameJob;

/* Wie weit eine Datei geschaut ist */
typedef struct {
    double position, duration;  /* Sekunden; duration 0 = unbekannt */
    gint64 updated;             /* Unix-Zeit */
} WatchState;

/* Serie, Sendung oder Film in "Meine Videos" */
typedef struct {
    char *name;
    bool is_film;
    GPtrArray *entries;         /* MdHistoryEntry* aus ui.videos */
    gint64 newest, bytes;
} Show;

/* IMDb-Bewertung im Hintergrund */
typedef struct {
    char *key, *name;
    bool is_film, found;
    MdImdbInfo info;
} RatingJob;

/* Zeile der Warteschlange; merkt sich, was sie zeigt, damit der Timer nur Änderungen anfasst */
typedef struct {
    guint id;
    GtkWidget *row, *title, *subtitle, *bar, *pause, *retry, *remove, *open;
    char *target, *topic;
    bool seen;
    int order;                  /* Platz in der Liste */
    bool shown, paused;         /* shown: Stand unten gilt */
    MdJobState state;
    char *status;
} JobRow;

typedef struct {
    char *dir;
    int quality, encoder, gpu_quality, cpu_rf;
    int parallel, conversions, min_minutes;
    double limit_mb;
    int sources;                /* gewählte Mediatheken */
    bool limit_on, convert, keep_original, show_signed, hide_audio;
    int width, height;
    bool maximized;
    bool update_check;          /* beim Start nach Updates suchen */
    char *update_skip;          /* diese Version nicht mehr anbieten */
    bool auto_news;             /* neue Folgen geladener Serien von selbst laden */
    bool skip_intro;            /* erkannte Intros im Player von selbst überspringen */
    bool skip_recap;            /* erkannte Rückblicke ebenso */
    double volume;              /* Lautstärke des Players beim letzten Mal, 0…1 */
    bool gpu_convert;           /* Player: Farbumrechnung in der Grafikkarte */
    bool hold_downloads, hold_conversions;  /* alle Downloads bzw. Umwandlungen angehalten */
} Config;

typedef struct {
    GtkApplication *app;
    GHashTable *job_groups;     /* Sendungen mit eigener Gruppenzeile in der Warteschlange */
    GtkWidget *win, *header, *stack, *infobar, *info_label;
    Config cfg;
    int layout_width;
    GtkWidget *page_columns[16];
    int page_column_count;
    bool quitting, gpu_ok;
    guint inhibit_cookie;

    /* Updates */
    GtkWidget *update_bar, *update_label, *update_install, *update_notes, *update_skip_button;
    MdUpdate *update;
    bool update_busy;

    /* Neue Folgen */
    ResultTree news_tree;
    GtkWidget *news_heading, *news_note, *news_spinner, *news_select, *news_footer;
    GtkWidget *news_busy_box, *news_busy_detail, *news_progress, *header_spinner;
    bool news_busy;
    bool start_minimized;       /* --autostart */
    GtkWidget *videos_page, *player_return;     /* Seite, zu der der Player zurückkehrt */

    /* Fußzeile und Mediatheken */
    GtkWidget *selection_labels[FOOTER_COUNT], *target_labels[FOOTER_COUNT];
    GtkWidget *download_buttons[FOOTER_COUNT];
    GtkWidget *source_buttons[SOURCE_COUNT + 1];    /* zuletzt "Alle" */
    bool updating_checks;

    /* Vorschau */
    GtkWidget *preview_search, *preview_kind, *preview_spinner, *preview_note;
    GtkWidget *preview_language;
    GtkWidget *genre_box, *genre_all;
    GHashTable *genre_chips;    /* Kategorie -> GtkToggleButton */
    char *active_genre;         /* NULL = alle */
    int genre_pending, genre_tick;
    bool genres_dirty, updating_chips;
    GtkWidget *film_title, *film_box, *film_more, *series_title, *series_box, *series_more;
    GPtrArray *preview;         /* Group*, neueste zuerst */
    GHashTable *preview_index;  /* Group.key -> Group* */
    GThreadPool *cover_pool, *imdb_pool;
    GHashTable *covers;         /* Seite -> CachedCover*, überlebt das Neuladen der Vorschau */
    GQueue *cover_order;        /* Seiten in der Reihenfolge des Ladens, für COVER_CACHE_MAX */
    int film_limit, series_limit, series_loading;
    bool preview_busy, preview_refreshed;
    char *searching;            /* Begriff der laufenden Suche, NULL = keine */
    char *searched, *search_result;     /* letzter Suchbegriff (klein) und was sie fand */
    GtkWidget *search_hint;
    ResultTree detail_tree;     /* Liste der offenen Detailansicht */
    Group *detail_group;
    GtkWidget *detail_note, *detail_rating;
    GtkWidget *hover_popover, *hover_rating;
    Group *hover_group;
    guint hover_timer;

    /* Downloads */
    GtkWidget *downloads_page, *hero, *hero_sub, *jobs_list, *jobs_empty, *jobs_more;
    GtkWidget *run_all;         /* ein Knopf für "Alle pausieren" und "Alle fortsetzen" */
    GtkWidget *clear_failed;
    GtkWidget *hold_downloads, *hold_conversions;   /* Schalter: angehalten */
    int jobs_limit;             /* so viele Zeilen zeigt die Liste */
    int jobs_finished;          /* seit dem Start fertig gewordene (verschwinden aus der Liste) */
    guint info_timer;           /* blendet die Meldungsleiste aus */
    GtkWidget *limit_switch, *limit_spin;
    GHashTable *job_rows;       /* id -> JobRow* */
    GHashTable *job_marks;      /* Nummer -> JobMark: Stand beim letzten Timer-Schlag */


    /* Meine Videos */
    GtkWidget *videos_stack, *videos_scroller, *videos_sub, *videos_search, *videos_content;
    GtkWidget *stats_scroller, *stats_sub, *stats_content;
    GtkWidget *videos_check;    /* "Ordner prüfen" */
    GtkWidget *videos_genre, *videos_sort;
    GHashTable *video_genres;   /* "S:name" bzw. "F:name" -> Kategorien (leer = keine) */
    GHashTable *video_genres_asked;     /* Schlüssel, die gerade bei IMDb nachgefragt werden */
    bool updating_video_genres;
    GtkWidget *show_scroller, *show_title, *show_sub, *show_play, *show_seasons, *show_episodes;
    MdHistoryEntry *videos;     /* Verlauf, auf den die Kacheln zeigen */
    int video_count;
    GPtrArray *shows;           /* Show* */
    char *videos_fingerprint;   /* erkennt Änderungen am Verlauf */
    char *open_show;            /* Serie der Serienansicht, NULL = Übersicht */
    int show_season;            /* gewählte Staffel, G_MININT = alle */
    /* was die Mediatheken von den geladenen Serien anbieten (Suche nach neuen Folgen):
     * "serie" -> 0 für jede geprüfte Serie, "serie\tstaffel" -> höchste angebotene Folge;
     * Namen klein; NULL = noch nicht geprüft */
    GHashTable *season_offers;
    GHashTable *watch;          /* Pfad -> WatchState* */
    GHashTable *frames;         /* Bildschlüssel -> GdkPixbuf* */
    GHashTable *frame_waiting;  /* Bildschlüssel -> GPtrArray der GtkImage, die warten */
    GThreadPool *frame_pool;
    guint videos_rebuild_source;
    bool closed;                /* Hauptfenster abgebaut; der Player meldet sich danach noch */

    /* Einstellungen */
    GtkWidget *data_reset;
    GtkWidget *dir_sub, *cache_sub, *encoder_row, *encoder_sub, *gpu_row, *cpu_row, *keep_row;
    GtkWidget *level_row, *level_combo;     /* Qualitätsstufe der Umwandlung */
} UiState;

static UiState ui;

static void rebuild_results(void);
static void rebuild_tree(ResultTree *tree);
static void rebuild_preview(void);
static int compare_newest(gconstpointer a, gconstpointer b);
static void update_language_choices(void);
static void start_news_load(void);
static void show_news_busy(bool busy);
static void show_news_list(void);
static void update_tiles(void);
static void update_genres(void);
static void rebuild_videos(bool force);
static gboolean on_tick(gpointer d);
static void play_path(const char *path);
static void on_video_tile_activated(GtkFlowBox *box, GtkFlowBoxChild *child, gpointer d);
static void update_selection(void);
static const WatchState *watch_get(const char *path);
static bool watch_done(const WatchState *w);
static bool watch_started(const WatchState *w);
static int watch_percent(const WatchState *w);

/* ---------------------------------------------------------------------------
 * Kleine Helfer
 * ------------------------------------------------------------------------- */
static void add_css_class(GtkWidget *w, const char *cls)
{
    gtk_style_context_add_class(gtk_widget_get_style_context(w), cls);
}

/* Label mit linksbündigem Text und optionaler CSS-Klasse */
static GtkWidget *new_label(const char *text, const char *cls)
{
    GtkWidget *widget = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(widget), 0);
    if (cls) add_css_class(widget, cls);
    return widget;
}

/* umbrechender Text, der die Inhaltsspalte nicht verbreitert */
static GtkWidget *set_wrapping(GtkWidget *widget, int max_chars)
{
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(widget), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(widget), max_chars);
    return widget;
}

/* Text eines Labels setzen (ungültiges UTF-8 wird ersetzt) */
static void set_label_text(GtkWidget *widget, const char *text)
{
    char *valid = g_utf8_make_valid(text ? text : "", -1);
    /* gleicher Text: nicht setzen, sonst rechnet GTK das ganze Fenster neu aus */
    if (g_strcmp0(gtk_label_get_label(GTK_LABEL(widget)), valid))
        gtk_label_set_text(GTK_LABEL(widget), valid);
    g_free(valid);
}

/* Pfad im Standard-Programm öffnen (Ordner im Dateimanager, Video im Player) */
static void open_in_default_app(const char *path)
{
    GError *err = NULL;
    char *uri = g_filename_to_uri(path, NULL, &err);
    if (uri && gtk_show_uri_on_window(GTK_WINDOW(ui.win), uri, GDK_CURRENT_TIME, &err)) {
        g_free(uri);
        return;
    }
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(ui.win), GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                                          "Konnte nicht geöffnet werden");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s",
                                             err ? err->message : path);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    g_clear_error(&err);
    g_free(uri);
}

/* Ordner einer Datei öffnen; fehlt der Ordner, den nächsten vorhandenen darüber */
static void open_folder_of(const char *path)
{
    char *dir = g_path_get_dirname(path);
    while (!g_file_test(dir, G_FILE_TEST_IS_DIR) && strlen(dir) > 1) {
        char *parent = g_path_get_dirname(dir);
        g_free(dir);
        dir = parent;
    }
    open_in_default_app(dir);
    g_free(dir);
}

/* Trennlinie zwischen den Zeilen einer Liste */
static void list_separator_header(GtkListBoxRow *this_row, GtkListBoxRow *before, gpointer data)
{
    (void)data;
    if (!before) gtk_list_box_row_set_header(this_row, NULL);   /* nach Umsortieren */
    else if (!gtk_list_box_row_get_header(this_row))
        gtk_list_box_row_set_header(this_row, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
}

/* umrandete Liste mit Überschrift (Einstellungsgruppe) */
static GtkWidget *add_group(GtkWidget *box, const char *title)
{
    if (title) gtk_box_pack_start(GTK_BOX(box), new_label(title, "zd-heading"), FALSE, FALSE, 0);
    GtkWidget *frame = gtk_frame_new(NULL);
    add_css_class(frame, "zd-group");
    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_list_box_set_header_func(GTK_LIST_BOX(list), list_separator_header, NULL, NULL);
    gtk_container_add(GTK_CONTAINER(frame), list);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    return list;
}

/* Zeile: [prefix]  Titel / Untertitel  [suffix] */
static GtkWidget *add_row(GtkWidget *list, const char *title, const char *subtitle,
                          GtkWidget *prefix, GtkWidget *suffix, GtkWidget **sub_out)
{
    GtkWidget *r = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(r), FALSE);
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_top(h, 10);
    gtk_widget_set_margin_bottom(h, 10);
    gtk_widget_set_margin_start(h, 14);
    gtk_widget_set_margin_end(h, 12);
    if (prefix) {
        gtk_widget_set_valign(prefix, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(h), prefix, FALSE, FALSE, 0);
    }
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    gtk_widget_set_valign(v, GTK_ALIGN_CENTER);
    GtkWidget *t = new_label(title, "zd-title");
    gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(t), 50);
    gtk_box_pack_start(GTK_BOX(v), t, FALSE, FALSE, 0);
    GtkWidget *s = NULL;
    if (subtitle) {
        s = new_label(subtitle, "zd-sub");
        add_css_class(s, "dim-label");
        gtk_label_set_line_wrap(GTK_LABEL(s), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(s), PANGO_WRAP_WORD_CHAR);
        gtk_label_set_width_chars(GTK_LABEL(s), 20);
        gtk_label_set_max_width_chars(GTK_LABEL(s), 60);
        gtk_box_pack_start(GTK_BOX(v), s, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(h), v, TRUE, TRUE, 0);
    if (suffix) {
        gtk_widget_set_valign(suffix, GTK_ALIGN_CENTER);
        gtk_box_pack_end(GTK_BOX(h), suffix, FALSE, FALSE, 0);
    }
    gtk_container_add(GTK_CONTAINER(r), h);
    gtk_container_add(GTK_CONTAINER(list), r);
    if (sub_out) *sub_out = s;
    return r;
}

/* Inhaltsspalte; ihre Ränder folgen der Fensterbreite */
static GtkWidget *new_column(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(box, TRUE);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    if (ui.page_column_count < (int)G_N_ELEMENTS(ui.page_columns))
        ui.page_columns[ui.page_column_count++] = box;
    return box;
}

/* Seitenrahmen: Spalte in einem Scrollbereich */
static GtkWidget *new_page_column(GtkWidget **scroller)
{
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    GtkWidget *box = new_column();
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 24);
    gtk_container_add(GTK_CONTAINER(scrolled), box);
    *scroller = scrolled;
    return box;
}

/* runder Hauptknopf */
static GtkWidget *new_pill_button(const char *text, const char *cls)
{
    GtkWidget *b = gtk_button_new_with_label(text);
    add_css_class(b, "zd-pill");
    if (cls) add_css_class(b, cls);
    return b;
}

/* flacher Knopf mit Symbol */
static GtkWidget *new_icon_button(const char *icon, const char *tooltip)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
    gtk_button_set_relief(GTK_BUTTON(b), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(b, tooltip);
    return b;
}

/* Zahlenfeld mit Bereich */
static GtkWidget *new_spin(double lo, double hi, double step, double value)
{
    GtkWidget *s = gtk_spin_button_new_with_range(lo, hi, step);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s), value);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(s), TRUE);
    return s;
}

/* Auswahlliste aus festen Texten */
static GtkWidget *new_combo(const char *const *texts, int count, int active)
{
    GtkWidget *combo = gtk_combo_box_text_new();
    for (int i = 0; i < count; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), texts[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);
    return combo;
}

/* kleine farbige Marke; cls = "ok", "warn" oder NULL */
static GtkWidget *new_badge(const char *text, const char *cls)
{
    GtkWidget *badge = gtk_label_new(text);
    add_css_class(badge, "zd-badge");
    if (cls) add_css_class(badge, cls);
    return badge;
}

/* Ja/Nein-Rückfrage (modal) -> true bei "yes_label" */
static bool ask_confirmation(const char *title, const char *text, const char *no_label,
                             const char *yes_label, bool destructive)
{
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(ui.win), GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE, "%s", title);
    if (text) gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", text);
    gtk_dialog_add_button(GTK_DIALOG(d), no_label, GTK_RESPONSE_CANCEL);
    GtkWidget *yes = gtk_dialog_add_button(GTK_DIALOG(d), yes_label, GTK_RESPONSE_YES);
    add_css_class(yes, destructive ? "destructive-action" : "suggested-action");
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    bool r = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_YES;
    gtk_widget_destroy(d);
    return r;
}

static gboolean on_info_timeout(gpointer d)
{
    (void)d;
    ui.info_timer = 0;
    gtk_info_bar_set_revealed(GTK_INFO_BAR(ui.infobar), FALSE);
    return G_SOURCE_REMOVE;
}

/* Meldungsleiste oben im Fenster (NULL schließt sie); sie geht nach INFO_SECONDS von selbst
 * zu, jede neue Meldung startet die Zeit neu */
static void show_info_bar(GtkMessageType type, const char *text)
{
    if (ui.info_timer) g_source_remove(ui.info_timer);
    ui.info_timer = 0;
    if (!text) {
        gtk_info_bar_set_revealed(GTK_INFO_BAR(ui.infobar), FALSE);
        return;
    }
    ui.info_timer = g_timeout_add_seconds(INFO_SECONDS, on_info_timeout, NULL);
    /* steht schon eine Meldung da, bleibt sie unter der neuen stehen (höchstens INFO_LINES_MAX);
     * die Farbe richtet sich nach der ernstesten */
    static GPtrArray *lines;
    static GtkMessageType shown_type;
    if (!lines) lines = g_ptr_array_new_with_free_func(g_free);
    if (!gtk_info_bar_get_revealed(GTK_INFO_BAR(ui.infobar)) ||
        !gtk_widget_get_visible(ui.infobar)) {
        g_ptr_array_set_size(lines, 0);
        shown_type = type;
    }
    for (guint i = 0; i < lines->len; i++)
        if (!strcmp(lines->pdata[i], text)) g_ptr_array_remove_index(lines, i--);
    g_ptr_array_insert(lines, 0, g_strdup(text));
    if (lines->len > INFO_LINES_MAX) g_ptr_array_set_size(lines, INFO_LINES_MAX);
    static const GtkMessageType RANK[] = {GTK_MESSAGE_OTHER, GTK_MESSAGE_INFO,
                                          GTK_MESSAGE_QUESTION, GTK_MESSAGE_WARNING,
                                          GTK_MESSAGE_ERROR};
    int old_rank = 0, new_rank = 0;
    for (size_t i = 0; i < G_N_ELEMENTS(RANK); i++) {
        if (RANK[i] == shown_type) old_rank = (int)i;
        if (RANK[i] == type) new_rank = (int)i;
    }
    if (new_rank > old_rank) shown_type = type;
    g_ptr_array_add(lines, NULL);
    char *joined = g_strjoinv("\n", (char **)lines->pdata);
    g_ptr_array_remove_index(lines, lines->len - 1);
    gtk_info_bar_set_message_type(GTK_INFO_BAR(ui.infobar), shown_type);
    set_label_text(ui.info_label, joined);
    g_free(joined);
    gtk_widget_show(ui.infobar);
    gtk_info_bar_set_revealed(GTK_INFO_BAR(ui.infobar), TRUE);
}

/* Datei- und Ordnerpfad, der am Knopf hängt, öffnen */
static void on_open_file_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    play_path(g_object_get_data(G_OBJECT(b), "path"));
}

static void on_open_folder_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    open_folder_of(g_object_get_data(G_OBJECT(b), "path"));
}

/* Knopf, der mit dem Pfad path arbeitet */
static GtkWidget *new_path_button(const char *icon, const char *tooltip, const char *path,
                                  GCallback callback)
{
    GtkWidget *b = new_icon_button(icon, tooltip);
    g_object_set_data_full(G_OBJECT(b), "path", g_strdup(path), g_free);
    g_signal_connect(b, "clicked", callback, NULL);
    return b;
}

/* IMDb im Hintergrund nachschlagen und die Seite im Browser öffnen */
typedef struct {
    char *name, *url;
    bool is_film;
} ImdbJob;

static gboolean on_imdb_done(gpointer data)
{
    ImdbJob *job = data;
    GError *err = NULL;
    if (!gtk_show_uri_on_window(GTK_WINDOW(ui.win), job->url, GDK_CURRENT_TIME, &err)) {
        show_info_bar(GTK_MESSAGE_ERROR, err->message);
        g_clear_error(&err);
    }
    g_free(job->name);
    g_free(job->url);
    g_free(job);
    return G_SOURCE_REMOVE;
}

static gpointer imdb_thread(gpointer data)
{
    ImdbJob *job = data;
    job->url = md_imdb_url(job->name, job->is_film);
    g_idle_add(on_imdb_done, job);
    return NULL;
}

static void on_imdb_clicked(GtkButton *b, gpointer d)
{
    ImdbJob *job = g_new0(ImdbJob, 1);
    job->name = g_strdup(g_object_get_data(G_OBJECT(b), "name"));
    job->is_film = d != NULL;
    g_thread_unref(g_thread_new("imdb", imdb_thread, job));
}

/* Knopf, der die IMDb-Seite eines Films oder einer Serie öffnet */
static GtkWidget *new_imdb_button(const char *name, bool is_film)
{
    GtkWidget *b = gtk_button_new_with_label("IMDb");
    add_css_class(b, "zd-imdb");
    gtk_widget_set_tooltip_text(b, "Details bei IMDb im Browser ansehen");
    g_object_set_data_full(G_OBJECT(b), "name", g_strdup(name), g_free);
    g_signal_connect(b, "clicked", G_CALLBACK(on_imdb_clicked),
                     is_film ? GINT_TO_POINTER(1) : NULL);
    return b;
}

/* Datum für die Anzeige */
static char *format_date(gint64 unix_time)
{
    GDateTime *when = g_date_time_new_from_unix_local(unix_time);
    char *text = g_date_time_format(when, "%d.%m.%Y");
    g_date_time_unref(when);
    return text;
}

/* Mehrzahl: "1 Folge", "2 Folgen" */
static const char *plural(int count, const char *one, const char *many)
{
    return count == 1 ? one : many;
}

static const char CSS[] =
    ".zd-heading { font-weight: bold; margin-top: 18px; margin-bottom: 4px; }\n"
    "frame.zd-group > border { border-radius: 10px; background-color: @theme_base_color; }\n"
    "frame.zd-group list { background-color: transparent; }\n"
    "frame.zd-group list > row:first-child { border-top-left-radius: 10px;"
    " border-top-right-radius: 10px; }\n"
    "frame.zd-group list > row:last-child { border-bottom-left-radius: 10px;"
    " border-bottom-right-radius: 10px; }\n"
    ".zd-sub { font-size: 90%; }\n"
    ".zd-hero { font-size: 170%; font-weight: 800; }\n"
    ".zd-lead { font-size: 105%; }\n"
    ".zd-pill { border-radius: 999px; padding: 8px 30px; font-weight: bold; }\n"
    ".zd-badge { border-radius: 999px; padding: 1px 9px; font-size: 85%; font-weight: bold;"
    " color: @theme_selected_bg_color;"
    " background-color: alpha(@theme_selected_bg_color, 0.16); }\n"
    ".zd-badge.ok { color: @success_color; background-color: alpha(@success_color, 0.16); }\n"
    ".zd-badge.warn { color: @warning_color; background-color: alpha(@warning_color, 0.16); }\n"
    ".zd-badge.err { color: @error_color; background-color: alpha(@error_color, 0.16); }\n"
    "flowboxchild { padding: 6px; border-radius: 10px; }\n"
    "flowboxchild:hover { background-color: alpha(@theme_fg_color, 0.07); }\n"
    ".zd-chips flowboxchild { padding: 2px; }\n"
    ".zd-chips flowboxchild:hover { background-color: transparent; }\n"
    "button.zd-chip { border-radius: 999px; padding: 3px 12px; min-height: 0; }\n"
    ".zd-cover { border-radius: 8px; background-color: @theme_base_color; }\n"
    ".zd-tile-check { padding: 3px 4px; border-radius: 6px;"
    " background-color: rgba(0, 0, 0, 0.6); }\n"
    ".zd-tile-badges .zd-badge { background-color: rgba(0, 0, 0, 0.75); }\n"
    ".zd-badge.news, .zd-tile-badges .zd-badge.news { color: white; padding: 1px 7px;"
    " background-color: @success_color; }\n"
    "button.zd-imdb { min-height: 0; min-width: 0; padding: 0 5px; border: none;"
    " border-radius: 5px; background-image: none; background-color: #f5c518; color: #000000;"
    " font-size: 85%; font-weight: bold; box-shadow: none; }\n"
    "button.zd-imdb:hover { background-color: #ffd93d; }\n"
    ".zd-busy { padding: 12px 16px; border-radius: 10px;"
    " background-color: alpha(@theme_selected_bg_color, 0.18);"
    " border: 1px solid alpha(@theme_selected_bg_color, 0.5); }\n"
    ".zd-busy-title { font-weight: bold; font-size: 115%; }\n"
    ".zd-footer { border-top: 1px solid @borders; padding-top: 10px; padding-bottom: 12px; }\n"
    ".zd-play-hint { opacity: 0; color: white; -gtk-icon-shadow: 0 1px 4px rgba(0, 0, 0, 0.8); }\n"
    "flowboxchild:hover .zd-play-hint { opacity: 1; }\n"
    "progressbar.zd-watch trough, progressbar.zd-watch progress { min-height: 4px; }\n"
    "progressbar.zd-watch trough { background-color: rgba(0, 0, 0, 0.6); border: none; }\n"
    "progressbar.zd-watch progress { background-color: @theme_selected_bg_color; border: none; }\n"
    ".zd-stat { padding: 10px 14px; border-radius: 10px; background-color: @theme_base_color; }\n"
    ".zd-stat-value { font-size: 150%; font-weight: 800; }\n"
    ".zd-stats flowboxchild, .zd-stats flowboxchild:hover { padding: 3px;"
    " background-color: transparent; }\n";

/* ---------------------------------------------------------------------------
 * Einstellungen
 * ------------------------------------------------------------------------- */
static char *settings_path(void)
{
    return g_build_filename(md_app_dir(), SETTINGS_FILE, NULL);
}

/* Ganzzahl aus der Einstellungsdatei, begrenzt auf lo..hi */
static int settings_int(GKeyFile *file, const char *key, int fallback, int lo, int hi)
{
    GError *err = NULL;
    int value = g_key_file_get_integer(file, SETTINGS_GROUP, key, &err);
    if (err) value = fallback;
    g_clear_error(&err);
    return CLAMP(value, lo, hi);
}

static bool settings_bool(GKeyFile *file, const char *key, bool fallback)
{
    GError *err = NULL;
    bool value = g_key_file_get_boolean(file, SETTINGS_GROUP, key, &err);
    if (err) value = fallback;
    g_clear_error(&err);
    return value;
}

/* Einstellungen laden; fehlende Werte bekommen die Standardwerte */
static void settings_load(void)
{
    GKeyFile *file = g_key_file_new();
    char *path = settings_path();
    g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL);
    g_free(path);
    Config *c = &ui.cfg;
    c->dir = g_key_file_get_string(file, SETTINGS_GROUP, "Speicherort", NULL);
    if (!c->dir || !*c->dir) {
        const char *videos = g_get_user_special_dir(G_USER_DIRECTORY_VIDEOS);
        g_free(c->dir);
        c->dir = g_build_filename(videos ? videos : g_get_home_dir(), "Mediathek", NULL);
    }
    /* Bis 2.2 gab es einen eigenen Ordner für den Zwischenspeicher. War einer gewählt, wird er
     * zum Ordner für alle Programmdaten; der Zwischenspeicher zieht dort in den Unterordner. */
    char *cache_dir = g_key_file_get_string(file, SETTINGS_GROUP, "Cacheordner", NULL);
    if (cache_dir && *cache_dir && !g_str_has_suffix(cache_dir, G_DIR_SEPARATOR_S "bilder")) {
        char *error = NULL;
        if (md_set_data_parent(cache_dir, &error)) md_adopt_cache(cache_dir);
        else md_log("Datenordner nicht umgezogen: %s", error);
        g_free(error);
        g_key_file_remove_key(file, SETTINGS_GROUP, "Cacheordner", NULL);
        path = settings_path();                     /* liegt jetzt im neuen Datenordner */
        g_key_file_save_to_file(file, path, NULL);
        g_free(path);
    }
    g_free(cache_dir);
    c->quality = settings_int(file, "Qualitaet", MD_QUALITY_HIGH, 0, 2);
    c->encoder = settings_int(file, "Encoder", MD_ENCODER_GPU, 0, 1);
    c->gpu_quality = settings_int(file, "GpuQualitaet", MD_DEFAULT_GPU_QUALITY, 1, 255);
    c->cpu_rf = settings_int(file, "CpuRf", MD_DEFAULT_CPU_RF, 1, 63);
    c->parallel = settings_int(file, "Downloads", 2, 1, MD_MAX_DOWNLOADS);
    c->conversions = settings_int(file, "Umwandlungen", 1, 1, MD_MAX_CONVERSIONS);
    c->min_minutes = settings_int(file, "Mindestlaenge", 10, 0, 300);
    c->limit_mb = CLAMP(settings_int(file, "LimitMBs", 10, 1, 1000), 1, 1000);
    c->limit_on = settings_bool(file, "LimitAktiv", false);
    c->sources = settings_int(file, "Mediatheken", MD_SOURCE_ALL, 1, MD_SOURCE_ALL_1_9);
    if (c->sources == MD_SOURCE_ALL_1_4 || c->sources == MD_SOURCE_ALL_1_5 ||
        c->sources == MD_SOURCE_ALL_1_6 || c->sources == MD_SOURCE_ALL_1_8 ||
        c->sources == MD_SOURCE_ALL_1_9)
        c->sources = MD_SOURCE_ALL;
    c->sources &= MD_SOURCE_ALL;            /* entfernte Mediatheken (ORF, DW …) fallen weg */
    if (!c->sources) c->sources = MD_SOURCE_ALL;
    c->convert = settings_bool(file, "AV1", false);
    c->keep_original = settings_bool(file, "OriginalBehalten", false);
    c->show_signed = settings_bool(file, "Gebaerdensprache", false);
    c->hide_audio = settings_bool(file, "NurTonAusblenden", false);
    c->width = settings_int(file, "Breite", 980, 0, 10000);
    c->height = settings_int(file, "Hoehe", 800, 0, 10000);
    c->maximized = settings_bool(file, "Maximiert", false);
    c->update_check = settings_bool(file, "UpdateSuchen", true);
    c->auto_news = settings_bool(file, "NeueFolgenLaden", false);
    c->skip_intro = settings_bool(file, "IntroUeberspringen", false);
    md_player_set_auto_skip_intro(c->skip_intro);
    c->skip_recap = settings_bool(file, "RueckblickUeberspringen", false);
    md_player_set_auto_skip_recap(c->skip_recap);
    c->volume = g_key_file_has_key(file, SETTINGS_GROUP, "Lautstaerke", NULL)
                    ? CLAMP(g_key_file_get_double(file, SETTINGS_GROUP, "Lautstaerke", NULL), 0, 1)
                    : 1.0;
    md_player_set_volume(c->volume);
    c->gpu_convert = settings_bool(file, "PlayerGrafikkarte", true);
    md_player_set_gpu_convert(c->gpu_convert);
    /* Tasten des Players: nur abweichende stehen in der Datei, als GDK-Tastenname */
    for (int i = 0; i < md_player_key_count(); i++) {
        char *name = g_strconcat("Taste", md_player_key_id(i), NULL);
        char *value = g_key_file_get_string(file, SETTINGS_GROUP, name, NULL);
        guint key = value ? gdk_keyval_from_name(value) : GDK_KEY_VoidSymbol;
        if (key && key != GDK_KEY_VoidSymbol) md_player_key_set(i, key);
        g_free(value);
        g_free(name);
    }
    c->hold_downloads = settings_bool(file, "DownloadsAngehalten", false);
    c->hold_conversions = settings_bool(file, "UmwandlungAngehalten", false);
    c->update_skip = g_key_file_get_string(file, SETTINGS_GROUP, "UpdateUebersprungen", NULL);
    g_key_file_free(file);
}

/* Einstellungen speichern (nach jeder Änderung) */
static void settings_save(void)
{
    const Config *c = &ui.cfg;
    GKeyFile *file = g_key_file_new();
    g_key_file_set_string(file, SETTINGS_GROUP, "Speicherort", c->dir);
    g_key_file_set_integer(file, SETTINGS_GROUP, "Qualitaet", c->quality);
    g_key_file_set_integer(file, SETTINGS_GROUP, "Encoder", c->encoder);
    g_key_file_set_integer(file, SETTINGS_GROUP, "GpuQualitaet", c->gpu_quality);
    g_key_file_set_integer(file, SETTINGS_GROUP, "CpuRf", c->cpu_rf);
    g_key_file_set_integer(file, SETTINGS_GROUP, "Downloads", c->parallel);
    g_key_file_set_integer(file, SETTINGS_GROUP, "Umwandlungen", c->conversions);
    g_key_file_set_integer(file, SETTINGS_GROUP, "Mindestlaenge", c->min_minutes);
    g_key_file_set_integer(file, SETTINGS_GROUP, "LimitMBs", (int)c->limit_mb);
    g_key_file_set_boolean(file, SETTINGS_GROUP, "LimitAktiv", c->limit_on);
    g_key_file_set_integer(file, SETTINGS_GROUP, "Mediatheken", c->sources);
    g_key_file_set_boolean(file, SETTINGS_GROUP, "AV1", c->convert);
    g_key_file_set_boolean(file, SETTINGS_GROUP, "OriginalBehalten", c->keep_original);
    g_key_file_set_boolean(file, SETTINGS_GROUP, "Gebaerdensprache", c->show_signed);
    g_key_file_set_boolean(file, SETTINGS_GROUP, "NurTonAusblenden", c->hide_audio);
    g_key_file_set_integer(file, SETTINGS_GROUP, "Breite", c->width);
    g_key_file_set_integer(file, SETTINGS_GROUP, "Hoehe", c->height);
    g_key_file_set_boolean(file, SETTINGS_GROUP, "Maximiert", c->maximized);
    g_key_file_set_boolean(file, SETTINGS_GROUP, "UpdateSuchen", c->update_check);
    g_key_file_set_boolean(file, SETTINGS_GROUP, "NeueFolgenLaden", c->auto_news);
    g_key_file_set_boolean(file, SETTINGS_GROUP, "IntroUeberspringen", c->skip_intro);
    g_key_file_set_boolean(file, SETTINGS_GROUP, "RueckblickUeberspringen", c->skip_recap);
    g_key_file_set_double(file, SETTINGS_GROUP, "Lautstaerke", c->volume);
    g_key_file_set_boolean(file, SETTINGS_GROUP, "PlayerGrafikkarte", c->gpu_convert);
    for (int i = 0; i < md_player_key_count(); i++) {
        guint key = md_player_key_get(i);
        const char *key_name = gdk_keyval_name(key);
        if (key == md_player_key_default(i) || !key_name) continue;
        char *name = g_strconcat("Taste", md_player_key_id(i), NULL);
        g_key_file_set_string(file, SETTINGS_GROUP, name, key_name);
        g_free(name);
    }
    g_key_file_set_boolean(file, SETTINGS_GROUP, "DownloadsAngehalten", c->hold_downloads);
    g_key_file_set_boolean(file, SETTINGS_GROUP, "UmwandlungAngehalten", c->hold_conversions);
    if (c->update_skip)
        g_key_file_set_string(file, SETTINGS_GROUP, "UpdateUebersprungen", c->update_skip);
    char *path = settings_path();
    if (!g_key_file_save_to_file(file, path, NULL)) md_log("Einstellungen nicht gespeichert");
    g_free(path);
    g_key_file_free(file);
}

/* Lautstärke erst speichern, wenn sie eine Sekunde ruht: beim Ziehen am Regler oder Halten der
 * Pfeiltaste kommen viele Werte kurz hintereinander */
static guint volume_save_id;

static gboolean save_volume(gpointer data)
{
    (void)data;
    volume_save_id = 0;
    settings_save();
    return G_SOURCE_REMOVE;
}

static void on_player_volume(double volume)
{
    ui.cfg.volume = volume;
    if (volume_save_id) g_source_remove(volume_save_id);
    volume_save_id = g_timeout_add_seconds(1, save_volume, NULL);
}

/* Tempolimit, Parallelität und Suchfilter an die Engine geben */
static void apply_queue_settings(void)
{
    md_set_hide_audio(ui.cfg.hide_audio);
    md_set_rate_limit(ui.cfg.limit_on ? (gint64)(ui.cfg.limit_mb * 1e6) : 0);
    md_set_parallel(ui.cfg.parallel, ui.cfg.conversions);
    md_set_encoder((MdEncoder)ui.cfg.encoder, ui.cfg.gpu_quality, ui.cfg.cpu_rf);
    md_queue_hold(ui.cfg.hold_downloads, ui.cfg.hold_conversions);
}

/* ---------------------------------------------------------------------------
 * Trefferbaum
 * ------------------------------------------------------------------------- */
static void item_free(gpointer data)
{
    Item *item = data;
    md_title_free(item->title);
    g_free(item->path);
    g_free(item->key);
    g_free(item);
}

static void season_free(gpointer data)
{
    Season *season = data;
    g_ptr_array_free(season->items, TRUE);
    g_free(season);
}

static void group_free(gpointer data)
{
    Group *group = data;
    g_free(group->name);
    g_free(group->key);
    g_free(group->website);
    g_free(group->video);
    g_free(group->description);
    g_free(group->cover_path);
    g_strfreev(group->genres);
    g_strfreev(group->imdb_genres);
    g_free(group->fsk);
    g_free(group->found_by);
    g_clear_object(&group->cover);
    g_ptr_array_free(group->seasons, TRUE);
    g_free(group);
}

/* lässt sich dieser Treffer auswählen? */
static bool item_open(const Item *item)
{
    return item->status != MD_STATUS_DONE && item->status != MD_STATUS_QUEUED;
}

/* Zähler einer Staffel oder Serie */
typedef struct {
    int total, open, done, queued, selected;
} Counts;

static void counts_add_season(Counts *c, const Season *season)
{
    for (guint i = 0; i < season->items->len; i++) {
        const Item *item = season->items->pdata[i];
        c->total++;
        c->open += item_open(item);
        c->done += item->status == MD_STATUS_DONE;
        c->queued += item->status == MD_STATUS_QUEUED;
        c->selected += item->selected;
    }
}

static void counts_add_group(Counts *c, const Group *group)
{
    for (guint i = 0; i < group->seasons->len; i++) counts_add_season(c, group->seasons->pdata[i]);
}

/* Häkchen aller offenen Treffer einer Staffel setzen */
static void season_select(Season *season, bool selected)
{
    for (guint i = 0; i < season->items->len; i++) {
        Item *item = season->items->pdata[i];
        if (item_open(item)) item->selected = selected;
    }
}

static void group_select(Group *group, bool selected)
{
    for (guint i = 0; i < group->seasons->len; i++)
        season_select(group->seasons->pdata[i], selected);
}

/* Reihenfolge innerhalb einer Staffel */
static int compare_items(gconstpointer a, gconstpointer b)
{
    const MdTitle *x = (*(Item *const *)a)->title, *y = (*(Item *const *)b)->title;
    if (x->episode != y->episode) return x->episode - y->episode;
    return x->timestamp < y->timestamp ? 1 : x->timestamp > y->timestamp ? -1 : 0;
}

/* Staffeln aufsteigend, Sendungen ohne Staffel zum Schluss */
static int compare_seasons(gconstpointer a, gconstpointer b)
{
    int x = (*(Season *const *)a)->number, y = (*(Season *const *)b)->number;
    if (x < 0 || y < 0) return y - x;
    return x - y;
}

/* Staffel einer Gruppe holen oder anlegen */
static Season *group_season(Group *group, int number)
{
    for (guint i = 0; i < group->seasons->len; i++) {
        Season *season = group->seasons->pdata[i];
        if (season->number == number) return season;
    }
    Season *season = g_new0(Season, 1);
    season->number = number;
    season->items = g_ptr_array_new_with_free_func(item_free);
    g_ptr_array_add(group->seasons, season);
    return season;
}

/* Steht derselbe Film oder dieselbe Folge schon aus einer anderen Mediathek in der Gruppe? */
static bool item_duplicate(GHashTable *seen, const Group *group, const MdTitle *t)
{
    char *key = t->is_film ? g_strdup(group->key)
                : t->season >= 0 && t->episode >= 0
                    ? g_strdup_printf("%s:S%dE%d", group->key, t->season, t->episode)
                    : g_strdup_printf("%s:%s", group->key, t->name);
    gpointer source = g_hash_table_lookup(seen, key);
    if (source) {
        g_free(key);
        return GPOINTER_TO_INT(source) != t->source;
    }
    g_hash_table_insert(seen, key, GINT_TO_POINTER(t->source));
    return false;
}

/* Trefferliste zum Baum ordnen; übernimmt die Items -> Group*. Gleiche Titel aus mehreren
 * Mediatheken ergeben eine Gruppe, group->source nennt dann alle. */
static GPtrArray *group_items(GPtrArray *items)
{
    GPtrArray *groups = g_ptr_array_new_with_free_func(group_free);
    GHashTable *by_name = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < items->len; i++) {
        Item *item = items->pdata[i];
        const MdTitle *t = item->title;
        const char *name = t->is_film ? t->name : t->topic;
        char *lower = g_utf8_strdown(name, -1);
        char *key = g_strdup_printf("%c:%s", t->is_film ? 'F' : 'S', lower);
        g_free(lower);
        Group *group = g_hash_table_lookup(by_name, key);
        if (!group) {
            group = g_new0(Group, 1);
            group->name = g_strdup(name);
            group->is_film = t->is_film;
            group->seasons = g_ptr_array_new_with_free_func(season_free);
            group->key = g_strdup(key);
            g_ptr_array_add(groups, group);
            g_hash_table_insert(by_name, g_strdup(key), group);
        }
        g_free(key);
        group->source |= t->source;
        if (item_duplicate(seen, group, t)) {
            item_free(item);
            continue;
        }
        g_ptr_array_add(group_season(group, t->season)->items, item);
        if (t->timestamp >= group->newest) {
            group->newest = t->timestamp;
            if (*t->website) {
                g_free(group->website);
                group->website = g_strdup(t->website);
                g_free(group->video);
                group->video = g_strdup(md_title_url(t, MD_QUALITY_LOW));
            }
            g_free(group->description);
            group->description = g_strdup(t->description);
        }
    }
    g_hash_table_destroy(by_name);
    g_hash_table_destroy(seen);
    for (guint i = 0; i < groups->len; i++) {
        Group *group = groups->pdata[i];
        g_ptr_array_sort(group->seasons, compare_seasons);
        for (guint n = 0; n < group->seasons->len; n++) {
            Season *season = group->seasons->pdata[n];
            g_ptr_array_sort(season->items, compare_items);
            season->expanded = group->seasons->len == 1;
        }
    }
    return groups;
}

static void add_items(GPtrArray *items, GPtrArray *titles, const char *dir)
{
    for (guint i = 0; i < titles->len; i++) {
        Item *item = g_new0(Item, 1);
        item->title = titles->pdata[i];
        item->key = md_title_key(item->title);
        item->status = md_title_status(item->title, dir, &item->path);
        g_ptr_array_add(items, item);
    }
    g_ptr_array_set_free_func(titles, NULL);
    g_ptr_array_free(titles, TRUE);
}

/* Marke zum Stand eines Treffers */
static GtkWidget *status_badge(MdStatus status)
{
    switch (status) {
    case MD_STATUS_DONE: return new_badge("Geladen", "ok");
    case MD_STATUS_QUEUED: return new_badge("In Warteschlange", NULL);
    case MD_STATUS_PARTIAL: return new_badge("Angefangen", "warn");
    case MD_STATUS_GONE: return new_badge("Datei fehlt", "err");
    default: return NULL;
    }
}

/* Marke zum Sehstand einer geladenen Datei; NULL = noch nicht angefangen */
static GtkWidget *watch_badge(const char *path)
{
    const WatchState *w = ui.watch ? watch_get(path) : NULL;
    if (watch_done(w)) return new_badge("✓ Gesehen", "ok");
    if (!watch_started(w) || w->duration <= 0) return NULL;
    char *text = g_strdup_printf("%d %% gesehen", watch_percent(w));
    GtkWidget *badge = new_badge(text, "ok");
    char *rest = md_format_time(w->duration - w->position);
    char *tip = g_strdup_printf("Geladen · noch %s", rest);
    gtk_widget_set_tooltip_text(badge, tip);
    g_free(rest);
    g_free(tip);
    g_free(text);
    return badge;
}

/* Marke zum Stand einer Staffel oder Serie */
static GtkWidget *counts_badge(const Counts *c)
{
    if (c->done == c->total) return new_badge("Alles geladen", "ok");
    if (c->done + c->queued == c->total) return new_badge("In Warteschlange", NULL);
    if (!c->done) return NULL;
    char *text = g_strdup_printf("%d von %d geladen", c->done, c->total);
    GtkWidget *badge = new_badge(text, "warn");
    g_free(text);
    return badge;
}

static void ref_free(gpointer data)
{
    g_free(data);
}

/* Häkchen je nach Stand der Treffer setzen */
static void update_tree_checks(const ResultTree *tree)
{
    for (guint i = 0; tree->refs && i < tree->refs->len; i++) {
        const RowRef *ref = tree->refs->pdata[i];
        Counts c = {0};
        if (ref->kind == ROW_ITEM) {
            const Item *item = ref->data;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ref->check), item->selected);
            continue;
        }
        if (ref->kind == ROW_GROUP) counts_add_group(&c, ref->data);
        else counts_add_season(&c, ref->data);
        bool all = c.open > 0 && c.selected == c.open;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ref->check), all);
        gtk_toggle_button_set_inconsistent(GTK_TOGGLE_BUTTON(ref->check), c.selected && !all);
        gtk_widget_set_sensitive(ref->check, c.open > 0);
    }
}

static void update_checks(void)
{
    ui.updating_checks = true;
    update_tree_checks(&ui.news_tree);
    update_tree_checks(&ui.detail_tree);
    update_tiles();
    ui.updating_checks = false;
    update_selection();
}

/* Häkchen einer Zeile angeklickt. Gruppen wählen nur aus, was noch fehlt. */
static void row_toggled(const RowRef *ref)
{
    if (ref->kind == ROW_ITEM) {
        Item *item = ref->data;
        item->selected = !item->selected;
    } else {
        Counts c = {0};
        if (ref->kind == ROW_GROUP) counts_add_group(&c, ref->data);
        else counts_add_season(&c, ref->data);
        bool select = c.selected < c.open;
        if (ref->kind == ROW_GROUP) group_select(ref->data, select);
        else season_select(ref->data, select);
    }
    update_checks();
}

static void on_check_toggled(GtkToggleButton *b, gpointer d)
{
    (void)b;
    if (!ui.updating_checks) row_toggled(d);
}

/* Klick auf die Zeile: Serie/Staffel auf- und zuklappen, Folge an- oder abwählen */
static void on_result_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer d)
{
    (void)list;
    ResultTree *tree = d;
    const RowRef *ref = g_object_get_data(G_OBJECT(row), "ref");
    if (!ref) {                                     /* "Weitere anzeigen" */
        tree->limit += RESULT_ROWS_BATCH;
        rebuild_tree(tree);
    } else if (ref->kind == ROW_GROUP) {
        Group *group = ref->data;
        group->expanded = !group->expanded;
        rebuild_tree(tree);
    } else if (ref->kind == ROW_SEASON) {
        Season *season = ref->data;
        season->expanded = !season->expanded;
        rebuild_tree(tree);
    } else if (item_open(ref->data)) {
        row_toggled(ref);
    }
}

/* Vorderteil einer Zeile: Pfeil (nur Serie/Staffel) und Häkchen */
static GtkWidget *row_prefix(RowRef *ref, bool expandable, bool expanded)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    if (expandable)
        gtk_box_pack_start(GTK_BOX(box),
                           gtk_image_new_from_icon_name(expanded ? "pan-down-symbolic"
                                                                 : "pan-end-symbolic",
                                                        GTK_ICON_SIZE_MENU), FALSE, FALSE, 0);
    ref->check = gtk_check_button_new();
    gtk_widget_set_valign(ref->check, GTK_ALIGN_CENTER);
    g_signal_connect(ref->check, "toggled", G_CALLBACK(on_check_toggled), ref);
    gtk_box_pack_start(GTK_BOX(box), ref->check, FALSE, FALSE, 0);
    return box;
}

/* Zeile der Trefferliste anlegen; depth rückt sie ein */
static void add_result_row(ResultTree *tree, RowRef *ref, const char *title, const char *subtitle,
                           GtkWidget *prefix, GtkWidget *suffix, int depth, bool activatable)
{
    GtkWidget *row = add_row(tree->list, title, subtitle, prefix, suffix, NULL);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), activatable);
    gtk_widget_set_margin_start(gtk_bin_get_child(GTK_BIN(row)), 14 + depth * 26);
    g_object_set_data(G_OBJECT(row), "ref", ref);
    g_ptr_array_add(tree->refs, ref);
}

/* Beschreibung einer Folge oder eines Films */
static char *item_subtitle(const MdTitle *t)
{
    char *date = format_date(t->timestamp);
    char *size = md_format_size((double)md_title_estimate(t, ui.cfg.quality));
    GString *s = g_string_new(md_source_label(t->source));
    g_string_append(s, " · ");
    if (t->season >= 0) g_string_append_printf(s, "S%02dE%02d · ", t->season, t->episode);
    if (t->duration)
        g_string_append_printf(s, "%d min · %s · ca. %s", t->duration / 60, date, size);
    else g_string_append(s, date);
    if (t->audio->len > 1 || t->subtitles->len) {
        g_string_append(s, " · ");
        for (guint i = 0; i < t->audio->len; i++)
            g_string_append_printf(s, "%s%s", i ? ", " : "",
                                   ((MdTrack *)t->audio->pdata[i])->label);
        if (t->subtitles->len) g_string_append(s, " · Untertitel");
    }
    g_free(date);
    g_free(size);
    return g_string_free(s, FALSE);
}

/* Zeile eines Treffers */
static void add_item_row(ResultTree *tree, Item *item, int depth)
{
    RowRef *ref = g_new0(RowRef, 1);
    ref->kind = ROW_ITEM;
    ref->data = item;
    GtkWidget *suffix = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *badge = item->status == MD_STATUS_DONE && item->path ? watch_badge(item->path)
                                                                    : NULL;
    if (!badge) badge = status_badge(item->status);
    if (badge) {
        gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(suffix), badge, FALSE, FALSE, 0);
    }
    if (item->path) {
        gtk_box_pack_start(GTK_BOX(suffix),
                           new_path_button("media-playback-start-symbolic", "Abspielen", item->path,
                                           G_CALLBACK(on_open_file_clicked)), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(suffix),
                           new_path_button("folder-open-symbolic", "Ordner öffnen", item->path,
                                           G_CALLBACK(on_open_folder_clicked)), FALSE, FALSE, 0);
    }
    if (item->title->is_film && tree != &ui.detail_tree) {
        GtkWidget *imdb = new_imdb_button(item->title->name, true);
        gtk_widget_set_valign(imdb, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(suffix), imdb, FALSE, FALSE, 0);
    }
    char *subtitle = item_subtitle(item->title);
    GtkWidget *prefix = row_prefix(ref, false, false);
    gtk_widget_set_sensitive(ref->check, item_open(item));
    add_result_row(tree, ref, item->title->name, subtitle, prefix, suffix, depth, item_open(item));
    if (*item->title->description) {
        GtkWidget *row = gtk_widget_get_parent(gtk_widget_get_parent(prefix));
        gtk_widget_set_tooltip_text(row, item->title->description);
    }
    g_free(subtitle);
}

/* Überschriftszeile einer Staffel */
/* Folgen, die laut Nummerierung fehlen. Ein fehlendes Ende der Staffel lässt sich nicht erkennen. */
static int season_gaps(const Season *season)
{
    GHashTable *numbers = g_hash_table_new(g_direct_hash, g_direct_equal);
    int highest = 0;
    for (guint i = 0; i < season->items->len; i++) {
        int episode = ((const Item *)season->items->pdata[i])->title->episode;
        if (episode <= 0) continue;
        g_hash_table_add(numbers, GINT_TO_POINTER(episode));
        highest = MAX(highest, episode);
    }
    int gaps = highest - (int)g_hash_table_size(numbers);
    g_hash_table_destroy(numbers);
    return gaps;
}

/* complete: alle Folgen geladen, nur dann sind Lücken echt */
static void add_season_row(ResultTree *tree, Season *season, bool complete)
{
    Counts c = {0};
    counts_add_season(&c, season);
    RowRef *ref = g_new0(RowRef, 1);
    ref->kind = ROW_SEASON;
    ref->data = season;
    char *title = season->number >= 0 ? g_strdup_printf("Staffel %d", season->number)
                                      : g_strdup("Einzelne Sendungen");
    int gaps = complete && season->number >= 0 ? season_gaps(season) : 0;
    char *subtitle = gaps ? g_strdup_printf("⚠ %d %s, %d nicht in der Mediathek", c.total,
                                            plural(c.total, "Folge", "Folgen"), gaps)
                          : g_strdup_printf("%d %s", c.total, plural(c.total, "Folge", "Folgen"));
    add_result_row(tree, ref, title, subtitle, row_prefix(ref, true, season->expanded),
                   counts_badge(&c), 1, true);
    g_free(title);
    g_free(subtitle);
}

/* Überschriftszeile einer Serie */
static void add_group_row(ResultTree *tree, Group *group)
{
    Counts c = {0};
    counts_add_group(&c, group);
    RowRef *ref = g_new0(RowRef, 1);
    ref->kind = ROW_GROUP;
    ref->data = group;
    int seasons = 0, seconds = 0;
    for (guint i = 0; i < group->seasons->len; i++) {
        Season *season = group->seasons->pdata[i];
        seasons += season->number >= 0;
        for (guint n = 0; n < season->items->len; n++)
            seconds += ((Item *)season->items->pdata[n])->title->duration;
    }
    char *time_text = md_format_time(seconds);
    char *subtitle;
    if (seasons)
        subtitle = g_strdup_printf("%d %s · %d %s · %s", c.total,
                                   plural(c.total, "Folge", "Folgen"), seasons,
                                   plural(seasons, "Staffel", "Staffeln"), time_text);
    else
        subtitle = g_strdup_printf("%d %s · %s", c.total,
                                   plural(c.total, "Sendung", "Sendungen"), time_text);
    GtkWidget *suffix = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *badge = counts_badge(&c);
    if (badge) {
        gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(suffix), badge, FALSE, FALSE, 0);
    }
    if (tree != &ui.detail_tree) {
        GtkWidget *imdb = new_imdb_button(group->name, group->is_film);
        gtk_widget_set_valign(imdb, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(suffix), imdb, FALSE, FALSE, 0);
    }
    add_result_row(tree, ref, group->name, subtitle, row_prefix(ref, true, group->expanded), suffix,
                   0, true);
    g_free(time_text);
    g_free(subtitle);
}

/* Zeile ohne Häkchen: Hinweis oder "Weitere anzeigen" */
static void add_note_row(ResultTree *tree, const char *text, bool activatable)
{
    GtkWidget *label = new_label(text, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(label), 0.5);
    gtk_widget_set_margin_top(label, 16);
    gtk_widget_set_margin_bottom(label, 16);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), activatable);
    gtk_container_add(GTK_CONTAINER(row), label);
    gtk_container_add(GTK_CONTAINER(tree->list), row);
}

/* Liste neu aufbauen (aufgeklappte Ebenen, Filter, Stand) */
static void rebuild_tree(ResultTree *tree)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(tree->list));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(children);
    if (tree->refs) g_ptr_array_set_size(tree->refs, 0);
    else tree->refs = g_ptr_array_new_with_free_func(ref_free);

    int rows = 0;
    bool more = false;
    for (guint g = 0; tree->groups && g < tree->groups->len && !more; g++) {
        Group *group = tree->groups->pdata[g];
        Counts c = {0};
        counts_add_group(&c, group);
        if (rows >= tree->limit) {
            more = true;
            break;
        }
        rows++;
        if (group->is_film && c.total == 1) {
            add_item_row(tree, ((Season *)group->seasons->pdata[0])->items->pdata[0], 0);
            continue;
        }
        add_group_row(tree, group);
        if (!group->expanded) continue;
        bool flat = group->seasons->len == 1 && ((Season *)group->seasons->pdata[0])->number < 0;
        for (guint s = 0; s < group->seasons->len && !more; s++) {
            Season *season = group->seasons->pdata[s];
            Counts sc = {0};
            counts_add_season(&sc, season);
            if (!flat) {
                add_season_row(tree, season, group->complete);
                rows++;
            }
            if (!flat && !season->expanded) continue;
            for (guint i = 0; i < season->items->len; i++) {
                Item *item = season->items->pdata[i];
                if (rows >= tree->limit) {
                    more = true;
                    break;
                }
                add_item_row(tree, item, flat ? 1 : 2);
                rows++;
            }
        }
    }
    if (more) add_note_row(tree, "Weitere anzeigen …", true);
    else if (!rows) add_note_row(tree, tree->empty_text ? tree->empty_text
                                                        : "Keine passenden Einträge.", false);
    gtk_widget_show_all(tree->list);
    update_checks();
}

static void rebuild_results(void)
{
    if (ui.news_tree.list) rebuild_tree(&ui.news_tree);
    if (ui.detail_tree.list) rebuild_tree(&ui.detail_tree);
}

/* Stand der Treffer neu bestimmen, z. B. nach einem fertigen Download. keys: nur Titel mit
 * diesen Schlüsseln, NULL = alle. Die Vorschau hat Zehntausende Titel, und jede Prüfung schaut
 * auf die Platte. */
static bool refresh_groups(GPtrArray *groups, GHashTable *keys)
{
    bool changed = false;
    for (guint g = 0; groups && g < groups->len; g++) {
        Group *group = groups->pdata[g];
        for (guint s = 0; s < group->seasons->len; s++) {
            Season *season = group->seasons->pdata[s];
            for (guint i = 0; i < season->items->len; i++) {
                Item *item = season->items->pdata[i];
                if (keys) {
                    if (!item->key) item->key = md_title_key(item->title);
                    if (!g_hash_table_contains(keys, item->key)) continue;
                }
                char *path = NULL;
                MdStatus status = md_title_status(item->title, ui.cfg.dir, &path);
                changed |= status != item->status || g_strcmp0(path, item->path);
                item->status = status;
                g_free(item->path);
                item->path = path;
                if (!item_open(item)) item->selected = false;
            }
        }
    }
    return changed;
}

/* Serien markieren, die seit dem letzten Download neue Folgen bekommen haben. Sender stellen
 * Folgen vor ihrer Sendezeit online, darum zählt eine Folge erst ab dieser Zeit als neu. */
static void update_series_news(void)
{
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    int count = 0;
    MdHistoryEntry *entries = md_history_snapshot(&count);
    GHashTable *latest = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    for (int i = 0; i < count; i++) {
        char *key = g_utf8_strdown(entries[i].topic, -1);
        gint64 *known = g_hash_table_lookup(latest, key);
        if (!known) g_hash_table_insert(latest, g_strdup(key), g_memdup2(&entries[i].finished,
                                                                        sizeof(gint64)));
        else *known = MAX(*known, entries[i].finished);
        g_free(key);
    }
    for (guint g = 0; ui.preview && g < ui.preview->len; g++) {
        Group *group = ui.preview->pdata[g];
        char *key = g_utf8_strdown(group->name, -1);
        const gint64 *finished = group->is_film ? NULL : g_hash_table_lookup(latest, key);
        g_free(key);
        group->has_news = false;
        for (guint s = 0; finished && s < group->seasons->len; s++) {
            const Season *season = group->seasons->pdata[s];
            for (guint i = 0; i < season->items->len; i++) {
                const Item *item = season->items->pdata[i];
                gint64 aired = item->title->timestamp;
                group->has_news |= item_open(item) && aired > *finished && aired <= now;
            }
        }
    }
    g_hash_table_destroy(latest);
    md_history_snapshot_free(entries, count);
}

static void refresh_statuses(GHashTable *keys)
{
    bool changed = refresh_groups(ui.preview, keys);
    changed |= refresh_groups(ui.news_tree.groups, keys);
    update_series_news();
    if (changed) rebuild_results();
    else update_tiles();
}

/* gewählte Treffer in Baumreihenfolge */
static void collect_from(GPtrArray *out, GHashTable *seen, GPtrArray *groups)
{
    for (guint g = 0; groups && g < groups->len; g++) {
        Group *group = groups->pdata[g];
        for (guint s = 0; s < group->seasons->len; s++) {
            Season *season = group->seasons->pdata[s];
            for (guint i = 0; i < season->items->len; i++) {
                Item *item = season->items->pdata[i];
                if (item->selected &&
                    g_hash_table_add(seen, (char *)md_title_url(item->title, MD_QUALITY_HIGH)))
                    g_ptr_array_add(out, item);
            }
        }
    }
}

/* alle gewählten Treffer aus Vorschau und Neue Folgen */
static GPtrArray *collect_selected(void)
{
    GPtrArray *out = g_ptr_array_new();
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    collect_from(out, seen, ui.preview);
    collect_from(out, seen, ui.news_tree.groups);
    g_hash_table_destroy(seen);
    return out;
}

/* Fußzeile: Anzahl, geschätzte Größe, Ziel */
static void update_selection(void)
{
    GPtrArray *selected = collect_selected();
    gint64 bytes = 0;
    for (guint i = 0; i < selected->len; i++)
        bytes += md_title_estimate(((Item *)selected->pdata[i])->title, ui.cfg.quality);
    char *size = md_format_size((double)bytes);
    char *text = ui.series_loading ? g_strdup("Folgen werden geladen …")
                 : selected->len ? g_strdup_printf("%u ausgewählt · ca. %s", selected->len, size)
                                 : g_strdup("Nichts ausgewählt");
    char *display = g_filename_display_name(ui.cfg.dir);
    char *target = g_strdup_printf("Ziel: %s%s", display,
                                   ui.cfg.convert ? " · danach AV1" : "");
    for (int i = 0; i < FOOTER_COUNT; i++) {
        set_label_text(ui.selection_labels[i], text);
        set_label_text(ui.target_labels[i], target);
        gtk_widget_set_sensitive(ui.download_buttons[i], selected->len > 0 && !ui.series_loading);
    }
    g_free(size);
    g_free(text);
    g_free(display);
    g_free(target);
    g_ptr_array_free(selected, TRUE);
}

/* ---------------------------------------------------------------------------
 * Spurenauswahl
 * ------------------------------------------------------------------------- */
/* Bezeichnungen aller Spuren der Auswahl, ohne Doppelte */
static void add_labels(GPtrArray *labels, GPtrArray *tracks)
{
    for (guint i = 0; i < tracks->len; i++) {
        const char *label = ((MdTrack *)tracks->pdata[i])->label;
        bool known = false;
        for (guint n = 0; n < labels->len; n++) known |= !strcmp(labels->pdata[n], label);
        if (!known) g_ptr_array_add(labels, (char *)label);
    }
}

/* Häkchen je Bezeichnung in die Liste; alles ist vorgewählt */
static GPtrArray *add_track_checks(GtkWidget *box, const char *heading, GPtrArray *labels)
{
    GPtrArray *checks = g_ptr_array_new();
    GtkWidget *list = add_group(box, heading);
    for (guint i = 0; i < labels->len; i++) {
        GtkWidget *check = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), TRUE);
        add_row(list, labels->pdata[i], NULL, check, NULL, NULL);
        g_ptr_array_add(checks, check);
    }
    return checks;
}

/* mindestens eine Tonspur muss gewählt bleiben */
static void on_track_toggled(GtkToggleButton *b, gpointer d)
{
    (void)b;
    GPtrArray *audio = d;
    bool any = false;
    for (guint i = 0; i < audio->len; i++)
        any |= gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(audio->pdata[i]));
    gtk_widget_set_sensitive(g_object_get_data(G_OBJECT(audio->pdata[0]), "ok"), any);
}

/* abgewählte Bezeichnungen als NULL-terminierte Liste */
static char **unchecked_labels(GPtrArray *labels, GPtrArray *checks)
{
    GPtrArray *out = g_ptr_array_new();
    for (guint i = 0; i < labels->len; i++)
        if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(checks->pdata[i])))
            g_ptr_array_add(out, g_strdup(labels->pdata[i]));
    g_ptr_array_add(out, NULL);
    return (char **)g_ptr_array_free(out, FALSE);
}

/* Dialog "Sprachen und Untertitel". Standard: alles laden. -> false = abgebrochen */
static bool ask_tracks(GPtrArray *selected, char ***skip_audio, char ***skip_subtitles)
{
    GPtrArray *audio = g_ptr_array_new(), *subtitles = g_ptr_array_new();
    for (guint i = 0; i < selected->len; i++) {
        const MdTitle *t = ((Item *)selected->pdata[i])->title;
        add_labels(audio, t->audio);
        add_labels(subtitles, t->subtitles);
    }
    bool ok = true;
    if (audio->len > 1 || subtitles->len) {
        GtkWidget *d = gtk_dialog_new_with_buttons("Sprachen und Untertitel", GTK_WINDOW(ui.win),
                                                   GTK_DIALOG_MODAL | GTK_DIALOG_USE_HEADER_BAR,
                                                   "Abbrechen", GTK_RESPONSE_CANCEL, NULL);
        GtkWidget *go = gtk_dialog_add_button(GTK_DIALOG(d), "Herunterladen", GTK_RESPONSE_OK);
        add_css_class(go, "suggested-action");
        gtk_window_set_default_size(GTK_WINDOW(d), 440, -1);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_set_margin_start(box, 18);
        gtk_widget_set_margin_end(box, 18);
        gtk_widget_set_margin_bottom(box, 18);
        char *hint = g_strdup_printf("Gilt für %u %s. Eine Folge, die eine gewählte Spur nicht "
                                     "hat, wird ohne sie geladen.", selected->len,
                                     plural((int)selected->len, "Titel", "Titel"));
        gtk_box_pack_start(GTK_BOX(box), set_wrapping(new_label(hint, "zd-sub"), 50), FALSE, FALSE,
                           6);
        g_free(hint);
        GPtrArray *audio_checks = add_track_checks(box, "Sprachausgaben", audio);
        GPtrArray *subtitle_checks = subtitles->len
                                     ? add_track_checks(box, "Untertitel", subtitles)
                                     : g_ptr_array_new();
        for (guint i = 0; i < audio_checks->len; i++)
            g_signal_connect(audio_checks->pdata[i], "toggled", G_CALLBACK(on_track_toggled),
                             audio_checks);
        g_object_set_data(G_OBJECT(audio_checks->pdata[0]), "ok", go);
        gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), box, TRUE, TRUE, 0);
        gtk_widget_show_all(d);
        ok = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK;
        if (ok) {
            *skip_audio = unchecked_labels(audio, audio_checks);
            *skip_subtitles = unchecked_labels(subtitles, subtitle_checks);
        }
        gtk_widget_destroy(d);
        g_ptr_array_free(audio_checks, TRUE);
        g_ptr_array_free(subtitle_checks, TRUE);
    }
    g_ptr_array_free(audio, TRUE);
    g_ptr_array_free(subtitles, TRUE);
    return ok;
}

/* Auswahl in die Warteschlange stellen */
static void on_download_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    GPtrArray *selected = collect_selected();
    char **skip_audio = NULL, **skip_subtitles = NULL;
    if (!selected->len || !ask_tracks(selected, &skip_audio, &skip_subtitles)) {
        g_ptr_array_free(selected, TRUE);
        return;
    }
    MdJobOptions options = {.dir = ui.cfg.dir, .quality = (MdQuality)ui.cfg.quality,
                            .convert = ui.cfg.convert, .encoder = (MdEncoder)ui.cfg.encoder,
                            .gpu_quality = ui.cfg.gpu_quality, .cpu_rf = ui.cfg.cpu_rf,
                            .keep_original = ui.cfg.keep_original,
                            .skip_audio = (const char *const *)skip_audio,
                            .skip_subtitles = (const char *const *)skip_subtitles};
    GHashTable *keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < selected->len; i++) {
        Item *item = selected->pdata[i];
        md_queue_add(item->title, &options);
        item->selected = false;
        g_hash_table_add(keys, md_title_key(item->title));
    }
    g_strfreev(skip_audio);
    g_strfreev(skip_subtitles);
    g_ptr_array_free(selected, TRUE);
    refresh_statuses(keys);
    g_hash_table_destroy(keys);
    update_checks();
    gtk_stack_set_visible_child(GTK_STACK(ui.stack), ui.downloads_page);
}

/* Knöpfe zum gewählten Stand bringen */
static void sync_source_buttons(void)
{
    ui.updating_chips = true;
    for (int i = 0; i < SOURCE_COUNT; i++)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.source_buttons[i]),
                                     ui.cfg.sources != MD_SOURCE_ALL &&
                                     (ui.cfg.sources & SOURCE_BITS[i]));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.source_buttons[SOURCE_COUNT]),
                                 ui.cfg.sources == MD_SOURCE_ALL);
    ui.updating_chips = false;
}

/* Mediathek angeklickt. Aus "Alle" heraus gilt der Klick nur dieser einen, danach kommen weitere
 * dazu oder gehen wieder weg; ganz ohne Mediathek bleibt die zuletzt gewählte. */
static void on_source_toggled(GtkToggleButton *b, gpointer d)
{
    (void)b;
    if (ui.updating_chips) return;
    int bit = GPOINTER_TO_INT(d);
    if (bit == MD_SOURCE_ALL) ui.cfg.sources = MD_SOURCE_ALL;
    else if (ui.cfg.sources == MD_SOURCE_ALL) ui.cfg.sources = bit;
    else if (ui.cfg.sources != bit) ui.cfg.sources ^= bit;
    sync_source_buttons();
    settings_save();
    ui.film_limit = ui.series_limit = PREVIEW_TILES_BATCH;
    ui.genres_dirty = true;
    rebuild_preview();
}

/* Zeile "Mediathek: ZDF ARD 3sat Alle" */
/* Knöpfe der Mediatheken; brechen in schmalen Fenstern um */
static GtkWidget *build_source_bar(void)
{
    GtkWidget *bar = gtk_flow_box_new();
    add_css_class(bar, "zd-chips");
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(bar), GTK_SELECTION_NONE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(bar), 3);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(bar), SOURCE_COUNT + 2);
    gtk_widget_set_margin_top(bar, 6);
    GtkWidget *label = new_label("Mediathek", "dim-label");
    gtk_widget_set_margin_end(label, 6);
    gtk_flow_box_insert(GTK_FLOW_BOX(bar), label, -1);
    for (int i = 0; i <= SOURCE_COUNT; i++) {
        int bit = i < SOURCE_COUNT ? SOURCE_BITS[i] : MD_SOURCE_ALL;
        GtkWidget *b = gtk_toggle_button_new_with_label(i < SOURCE_COUNT ? md_source_label(bit)
                                                                         : "Alle");
        add_css_class(b, "zd-chip");
        g_signal_connect(b, "toggled", G_CALLBACK(on_source_toggled), GINT_TO_POINTER(bit));
        gtk_flow_box_insert(GTK_FLOW_BOX(bar), b, -1);
        ui.source_buttons[i] = b;
    }
    return bar;
}

/* Fußzeile mit Auswahl, Ziel und Knopf "Herunterladen" */
static GtkWidget *build_footer(int index)
{
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    add_css_class(footer, "zd-footer");
    GtkWidget *text = new_column();
    gtk_widget_set_margin_end(text, 0);
    gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
    ui.selection_labels[index] = new_label("", "zd-title");
    ui.target_labels[index] = new_label("", "zd-sub");
    add_css_class(ui.target_labels[index], "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(ui.target_labels[index]), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(text), ui.selection_labels[index], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text), ui.target_labels[index], FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(footer), text, TRUE, TRUE, 0);
    GtkWidget *button = new_pill_button("Herunterladen", "suggested-action");
    gtk_widget_set_margin_end(button, 24);
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    g_signal_connect(button, "clicked", G_CALLBACK(on_download_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(footer), button, FALSE, FALSE, 0);
    ui.download_buttons[index] = button;
    return footer;
}

/* ---------------------------------------------------------------------------
 * Seite Vorschau
 * ---------------------------------------------------------------------------
 * Die Startliste nennt nur die neuesten Folgen einer Serie. Alle Folgen werden erst geholt,
 * wenn eine Serie angehakt oder geöffnet wird (Group.complete). Die Häkchen landen in denselben
 * Items wie in der Detailansicht, die Auswahl gilt also für beide.
 * ------------------------------------------------------------------------- */
/* Bild in die Kachel setzen */
static void set_tile_cover(const Group *group)
{
    if (group->tile_image && group->cover)
        gtk_image_set_from_pixbuf(GTK_IMAGE(group->tile_image), group->cover);
}

/* Kategorien von IMDb zu denen der Mediathek legen */
static void merge_imdb_genres(Group *group)
{
    if (!group->genres || !group->imdb_genres) return;
    GStrvBuilder *all = g_strv_builder_new();
    g_strv_builder_addv(all, (const char **)group->genres);
    for (char **g = group->imdb_genres; *g; g++)
        if (!g_strv_contains((const char *const *)group->genres, *g))
            g_strv_builder_add(all, *g);
    g_strfreev(group->genres);
    group->genres = g_strv_builder_end(all);
    g_strv_builder_unref(all);
    g_clear_pointer(&group->imdb_genres, g_strfreev);
    ui.genres_dirty = true;
}

static void cached_cover_free(gpointer data)
{
    CachedCover *cover = data;
    g_object_unref(cover->pixbuf);
    g_free(cover->path);
    g_free(cover);
}

/* Titelbild merken; die ältesten fallen heraus */
static void cache_cover(const char *website, GdkPixbuf *pixbuf, const char *path)
{
    if (!ui.covers) {
        ui.covers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, cached_cover_free);
        ui.cover_order = g_queue_new();
    }
    CachedCover *cover = g_new0(CachedCover, 1);
    cover->pixbuf = g_object_ref(pixbuf);
    cover->path = g_strdup(path);
    if (!g_hash_table_contains(ui.covers, website))
        g_queue_push_tail(ui.cover_order, g_strdup(website));
    g_hash_table_replace(ui.covers, g_strdup(website), cover);
    while (g_queue_get_length(ui.cover_order) > COVER_CACHE_MAX) {
        char *old = g_queue_pop_head(ui.cover_order);
        g_hash_table_remove(ui.covers, old);
        g_free(old);
    }
}

/* Bild im Hintergrund laden und verkleinern */
static gboolean on_cover_done(gpointer data)
{
    CoverJob *job = data;
    Group *group = ui.preview_index ? g_hash_table_lookup(ui.preview_index, job->key) : NULL;
    if (job->pixbuf) cache_cover(job->website, job->pixbuf, job->path);
    if (group && job->pixbuf) {
        g_set_object(&group->cover, job->pixbuf);
        g_free(group->cover_path);
        group->cover_path = g_strdup(job->path);
        set_tile_cover(group);
    }
    if (group && job->info && !group->genres) {
        group->genres = g_steal_pointer(&job->info->genres);
        group->fsk = g_steal_pointer(&job->info->fsk);
        ui.genres_dirty = true;
    }
    if (group && !job->info && !group->genres) group->genres = g_new0(char *, 1);
    if (group) merge_imdb_genres(group);
    if (job->background && --ui.genre_pending == 0) ui.genres_dirty = true;
    md_page_info_free(job->info);
    g_clear_object(&job->pixbuf);
    g_free(job->key);
    g_free(job->website);
    g_free(job->video);
    g_free(job->path);
    g_free(job);
    return G_SOURCE_REMOVE;
}

static void cover_worker(gpointer data, gpointer d)
{
    (void)d;
    CoverJob *job = data;
    job->info = md_page_info(job->website);
    if (!job->background) job->path = md_cover_file(job->website, job->video);
    if (job->path)
        job->pixbuf = gdk_pixbuf_new_from_file_at_scale(job->path, COVER_WIDTH, COVER_HEIGHT,
                                                        FALSE, NULL);
    g_idle_add(on_cover_done, job);
}

static gboolean on_imdb_genres_done(gpointer data)
{
    GenreJob *job = data;
    Group *group = ui.preview_index ? g_hash_table_lookup(ui.preview_index, job->key) : NULL;
    if (group && !group->website && !group->genres) group->genres = g_new0(char *, 1);
    if (group && job->genres) {
        g_strfreev(group->imdb_genres);
        group->imdb_genres = g_steal_pointer(&job->genres);
        merge_imdb_genres(group);
    }
    if (--ui.genre_pending == 0) ui.genres_dirty = true;
    g_strfreev(job->genres);
    g_free(job->key);
    g_free(job->name);
    g_free(job);
    return G_SOURCE_REMOVE;
}

static gboolean on_video_genres_done(gpointer data);

static void imdb_genres_worker(gpointer data, gpointer d)
{
    (void)d;
    GenreJob *job = data;
    job->genres = md_imdb_genres(job->name, job->is_film);
    g_idle_add(job->videos ? on_video_genres_done : on_imdb_genres_done, job);
}

static void request_cover(Group *group)
{
    if (group->cover_requested || !group->website) return;
    group->cover_requested = true;
    CachedCover *cached = ui.covers ? g_hash_table_lookup(ui.covers, group->website) : NULL;
    if (cached) {
        g_set_object(&group->cover, cached->pixbuf);
        g_free(group->cover_path);
        group->cover_path = g_strdup(cached->path);
        set_tile_cover(group);
        return;
    }
    CoverJob *job = g_new0(CoverJob, 1);
    job->key = g_strdup(group->key);
    job->website = g_strdup(group->website);
    job->video = g_strdup(group->video);
    g_thread_pool_push(ui.cover_pool, job, NULL);
}

/* "Meine Videos" vor der Vorschau: das sind wenige, die Vorschau fragt Hunderte an */
static gint compare_genre_jobs(gconstpointer a, gconstpointer b, gpointer d)
{
    (void)d;
    return (int)((const GenreJob *)b)->videos - (int)((const GenreJob *)a)->videos;
}

/* sichtbare Kacheln zuerst */
static gint compare_cover_jobs(gconstpointer a, gconstpointer b, gpointer d)
{
    (void)d;
    return (int)((const CoverJob *)a)->background - (int)((const CoverJob *)b)->background;
}

/* Kategorien eines Titels im Hintergrund ermitteln */
static void request_group_genres(const Group *group)
{
    GenreJob *imdb = g_new0(GenreJob, 1);
    imdb->key = g_strdup(group->key);
    imdb->name = g_strdup(group->name);
    imdb->is_film = group->is_film;
    ui.genre_pending++;
    g_thread_pool_push(ui.imdb_pool, imdb, NULL);
    ui.genres_dirty = true;
    if (group->genres || !group->website) return;
    CoverJob *job = g_new0(CoverJob, 1);
    job->key = g_strdup(group->key);
    job->website = g_strdup(group->website);
    job->background = true;
    ui.genre_pending++;
    g_thread_pool_push(ui.cover_pool, job, NULL);
}

static void request_all_genres(void)
{
    for (guint i = 0; i < ui.preview->len; i++) request_group_genres(ui.preview->pdata[i]);
    ui.genres_dirty = true;
}

/* Eckdaten eines Titels: FSK, Länge, Datum. Serien nennen je Folge eine ungefähre Länge;
 * alle Folgen sind erst bekannt, wenn die Serie geöffnet oder angehakt wurde. */
static char *group_meta(const Group *group, bool short_form)
{
    Counts c = {0};
    counts_add_group(&c, group);
    int seconds = 0, seasons = 0;
    for (guint i = 0; i < group->seasons->len; i++) {
        const Season *season = group->seasons->pdata[i];
        seasons += season->number >= 0;
        for (guint n = 0; n < season->items->len; n++)
            seconds += ((Item *)season->items->pdata[n])->title->duration;
    }
    GString *s = g_string_new(NULL);
    if (!short_form) {
        for (int i = 0; i < SOURCE_COUNT; i++)
            if (group->source & SOURCE_BITS[i])
                g_string_append_printf(s, "%s · ", md_source_label(SOURCE_BITS[i]));
    }
    if (group->fsk) g_string_append_printf(s, "%s · ", group->fsk);
    char *date = format_date(group->newest);
    if (group->is_film) {
        if (seconds) g_string_append_printf(s, "%d min · ", seconds / MAX(c.total, 1) / 60);
        g_string_append(s, date);
    } else if (group->complete && short_form) {
        g_string_append_printf(s, "%d %s · %s", c.total, plural(c.total, "Folge", "Folgen"), date);
    } else if (group->complete) {
        char *total = md_format_time(seconds);
        g_string_append_printf(s, "%d %s · %d %s · insgesamt %s · neueste Folge %s", c.total,
                               plural(c.total, "Folge", "Folgen"), seasons,
                               plural(seasons, "Staffel", "Staffeln"), total, date);
        g_free(total);
    } else {
        g_string_append_printf(s, "ca. %d min je Folge · %s%s", seconds / MAX(c.total, 1) / 60,
                               short_form ? "" : "neueste Folge ", date);
    }
    g_free(date);
    return g_string_free(s, FALSE);
}

/* Bewertung als Text */
static char *rating_text(const Group *group)
{
    if (group->imdb_state == IMDB_LOADING) return g_strdup("IMDb: wird nachgeschlagen …");
    if (group->imdb_state != IMDB_FOUND) return g_strdup("IMDb: kein sicherer Treffer");
    GString *s = g_string_new("IMDb: ");
    if (group->imdb.rating > 0) {
        char number[16];
        g_ascii_formatd(number, sizeof number, "%.1f", group->imdb.rating);
        *strchr(number, '.') = ',';
        int votes = group->imdb.votes;
        g_string_append_printf(s, "★ %s von 10 · ", number);
        if (votes >= 1000000) g_string_append_printf(s, "%d Mio. Stimmen", votes / 1000000);
        else if (votes >= 1000) g_string_append_printf(s, "%d Tsd. Stimmen", votes / 1000);
        else g_string_append_printf(s, "%d %s", votes, plural(votes, "Stimme", "Stimmen"));
    } else {
        g_string_append(s, "noch keine Bewertung");
    }
    if (group->imdb.year) g_string_append_printf(s, " · erschienen %d", group->imdb.year);
    return g_string_free(s, FALSE);
}

/* offene Anzeigen der Bewertung auffrischen */
static void show_rating(const Group *group)
{
    char *text = rating_text(group);
    if (group == ui.hover_group && ui.hover_rating) set_label_text(ui.hover_rating, text);
    if (group == ui.detail_group && ui.detail_rating) set_label_text(ui.detail_rating, text);
    g_free(text);
}

static gboolean on_rating_done(gpointer data)
{
    RatingJob *job = data;
    Group *group = ui.preview_index ? g_hash_table_lookup(ui.preview_index, job->key) : NULL;
    if (group) {
        group->imdb = job->info;
        group->imdb_state = job->found ? IMDB_FOUND : IMDB_NOT_FOUND;
        show_rating(group);
    }
    g_free(job->key);
    g_free(job->name);
    g_free(job);
    return G_SOURCE_REMOVE;
}

static gpointer rating_thread(gpointer data)
{
    RatingJob *job = data;
    job->found = md_imdb_info(job->name, job->is_film, &job->info);
    g_idle_add(on_rating_done, job);
    return NULL;
}

/* Bewertung einmal je Titel nachschlagen */
static void request_rating(Group *group)
{
    if (group->imdb_state != IMDB_UNKNOWN) return;
    group->imdb_state = IMDB_LOADING;
    RatingJob *job = g_new0(RatingJob, 1);
    job->key = g_strdup(group->key);
    job->name = g_strdup(group->name);
    job->is_film = group->is_film;
    g_thread_unref(g_thread_new("bewertung", rating_thread, job));
}

/* grünes "!" an Serien mit neuen Folgen */
static GtkWidget *new_news_badge(void)
{
    GtkWidget *badge = new_badge("!", "news");
    gtk_widget_set_tooltip_text(badge, "Neue Folgen seit dem letzten Download");
    return badge;
}

/* Häkchen, Marke und Beschreibung einer Kachel */
static void update_tile(const Group *group)
{
    if (!group->tile_check) return;
    Counts c = {0};
    counts_add_group(&c, group);
    bool all = c.open > 0 && c.selected == c.open;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(group->tile_check), all);
    gtk_toggle_button_set_inconsistent(GTK_TOGGLE_BUTTON(group->tile_check),
                                       group->loading || (c.selected && !all));
    gtk_widget_set_sensitive(group->tile_check, c.open > 0 || !group->complete);

    GList *children = gtk_container_get_children(GTK_CONTAINER(group->tile_badges));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(children);
    GtkWidget *badge = NULL;
    if (group->is_film && c.total == 1) {
        const Season *season = group->seasons->pdata[0];
        const Item *item = season->items->pdata[0];
        if (item->status == MD_STATUS_DONE && item->path) badge = watch_badge(item->path);
        if (!badge) badge = status_badge(item->status);
    } else if (c.selected) {
        char *text = g_strdup_printf("%d gewählt", c.selected);
        badge = new_badge(text, NULL);
        g_free(text);
    } else if (group->complete) {
        badge = counts_badge(&c);
    } else if (c.done) {
        char *text = g_strdup_printf("%d geladen", c.done);
        badge = new_badge(text, "ok");
        g_free(text);
    }
    if (badge) {
        gtk_box_pack_start(GTK_BOX(group->tile_badges), badge, FALSE, FALSE, 0);
        gtk_widget_show(badge);
    }
    if (group->has_news) {
        GtkWidget *news = new_news_badge();
        gtk_box_pack_end(GTK_BOX(group->tile_badges), news, FALSE, FALSE, 3);
        gtk_widget_show(news);
    }

    char *sub = group_meta(group, true);
    set_label_text(group->tile_sub, sub);
    g_free(sub);
}

static void update_tiles(void)
{
    for (guint i = 0; ui.preview && i < ui.preview->len; i++) update_tile(ui.preview->pdata[i]);
}

/* Alle Folgen einer Serie im Hintergrund holen */
static gboolean on_series_done(gpointer data)
{
    SeriesJob *job = data;
    ui.series_loading--;
    Group *group = g_hash_table_lookup(ui.preview_index, job->key);
    if (group) {
        group->loading = false;
        if (job->items) {
            GPtrArray *found = group_items(job->items);
            g_ptr_array_set_free_func(job->items, NULL);    /* gehören jetzt den Staffeln */
            for (guint i = 0; i < found->len; i++) {
                Group *fresh = found->pdata[i];
                if (strcmp(fresh->key, group->key)) continue;
                GPtrArray *old = group->seasons;
                group->seasons = fresh->seasons;
                fresh->seasons = old;
                group->source |= fresh->source;     /* Folgen auch aus anderen Mediatheken */
                group->complete = true;
            }
            g_ptr_array_free(found, TRUE);
        }
        if (group->complete && group->select_after_load) group_select(group, true);
        group->select_after_load = false;
        if (job->error) show_info_bar(GTK_MESSAGE_ERROR, job->error);
    }
    if (group && group == ui.detail_group) {
        gtk_widget_hide(ui.detail_note);
        rebuild_tree(&ui.detail_tree);
    } else {
        update_checks();
    }
    if (job->items) g_ptr_array_free(job->items, TRUE);
    g_free(job->key);
    g_free(job->topic);
    g_free(job->dir);
    g_free(job->error);
    g_free(job);
    return G_SOURCE_REMOVE;
}

/* Alle Folgen aus allen Mediatheken, nicht nur aus denen, die die Vorschau zeigte: Bietet
 * die eine Mediathek nur einige Folgen einer Staffel, kommen die übrigen von einer anderen. */
static gpointer series_thread(gpointer data)
{
    SeriesJob *job = data;
    MdQuery query = {.text = job->topic, .kind = MD_KIND_ALL, .sources = MD_SOURCE_ALL,
                     .min_minutes = job->min_minutes,
                     .hide_variants = job->hide_variants, .cache = job->cache};
    GPtrArray *titles = md_search(&query, NULL, NULL, &job->error);
    if (titles) {
        /* die Suche findet das Wort auch in fremden Sendungen; die Schreibweise des Namens
         * unterscheidet sich zwischen den Mediatheken manchmal */
        char *topic = g_utf8_casefold(job->topic, -1);
        for (guint i = titles->len; i-- > 0;) {
            char *other = g_utf8_casefold(((MdTitle *)titles->pdata[i])->topic, -1);
            if (strcmp(other, topic)) g_ptr_array_remove_index(titles, i);
            g_free(other);
        }
        g_free(topic);
        job->items = g_ptr_array_new_with_free_func(item_free);
        add_items(job->items, titles, job->dir);
    }
    g_idle_add(on_series_done, job);
    return NULL;
}

static void load_series(Group *group)
{
    if (group->complete || group->loading) return;
    group->loading = true;
    ui.series_loading++;
    SeriesJob *job = g_new0(SeriesJob, 1);
    job->key = g_strdup(group->key);
    job->topic = g_strdup(group->name);
    job->dir = g_strdup(ui.cfg.dir);
    job->min_minutes = ui.cfg.min_minutes;
    job->hide_variants = !ui.cfg.show_signed;
    job->cache = ui.preview_refreshed ? MD_CACHE_REFRESH : MD_CACHE_USE;
    g_thread_unref(g_thread_new("serie", series_thread, job));
}

/* Häkchen auf der Kachel: Film wählen; Serie: alles Fehlende, auch was noch nicht geladen ist */
static void on_tile_toggled(GtkToggleButton *b, gpointer d)
{
    (void)b;
    if (ui.updating_checks) return;
    Group *group = d;
    if (group->complete) {
        Counts c = {0};
        counts_add_group(&c, group);
        group_select(group, c.selected < c.open);
    } else {
        group->select_after_load = !group->select_after_load;
        load_series(group);
    }
    update_checks();
}

/* Zeilen mit Eckdaten, Kategorien und Bewertung anlegen */
static void add_title_facts(GtkWidget *box, Group *group, GtkWidget **rating)
{
    char *meta = group_meta(group, false);
    GtkWidget *label = new_label(meta, "zd-title");
    set_wrapping(label, 44);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    g_free(meta);
    if (group->genres && *group->genres) {
        char *genres = g_strjoinv(" · ", group->genres);
        label = new_label(genres, "zd-sub");
        add_css_class(label, "dim-label");
        set_wrapping(label, 44);
        gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
        g_free(genres);
    }
    request_rating(group);
    char *text = rating_text(group);
    *rating = new_label(text, "zd-title");
    gtk_box_pack_start(GTK_BOX(box), *rating, FALSE, FALSE, 0);
    g_free(text);
}

/* Popup schließen und eine anstehende Anzeige verwerfen */
static void close_hover(void)
{
    if (ui.hover_timer) g_source_remove(ui.hover_timer);
    ui.hover_timer = 0;
    ui.hover_group = NULL;
    ui.hover_rating = NULL;
    if (ui.hover_popover) gtk_widget_destroy(ui.hover_popover);
}

/* Die Maus ruht auf der Kachel: Popup mit den Details zeigen */
static gboolean on_hover_timeout(gpointer anchor)
{
    Group *group = ui.hover_group;
    ui.hover_timer = 0;
    ui.hover_popover = gtk_popover_new(anchor);
    gtk_popover_set_modal(GTK_POPOVER(ui.hover_popover), FALSE);
    gtk_popover_set_position(GTK_POPOVER(ui.hover_popover), GTK_POS_RIGHT);
    g_signal_connect(ui.hover_popover, "destroy", G_CALLBACK(gtk_widget_destroyed),
                     &ui.hover_popover);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 12);
    GtkWidget *title = new_label(group->name, "zd-lead");
    add_css_class(title, "zd-title");
    set_wrapping(title, 44);
    gtk_label_set_width_chars(GTK_LABEL(title), 40);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    add_title_facts(box, group, &ui.hover_rating);
    if (group->description && *group->description) {
        GtkWidget *text = new_label(group->description, NULL);
        set_wrapping(text, 44);
        gtk_label_set_width_chars(GTK_LABEL(text), 40);
        gtk_label_set_lines(GTK_LABEL(text), 9);
        gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
        gtk_box_pack_start(GTK_BOX(box), text, FALSE, FALSE, 4);
    }
    gtk_container_add(GTK_CONTAINER(ui.hover_popover), box);
    gtk_widget_show_all(box);
    gtk_popover_popup(GTK_POPOVER(ui.hover_popover));
    return G_SOURCE_REMOVE;
}

static gboolean on_tile_enter(GtkWidget *w, GdkEventCrossing *e, gpointer d)
{
    if (e->detail == GDK_NOTIFY_INFERIOR || ui.hover_group == d) return FALSE;
    close_hover();
    ui.hover_group = d;
    ui.hover_timer = g_timeout_add(HOVER_DELAY_MS, on_hover_timeout, w);
    return FALSE;
}

static gboolean on_tile_leave(GtkWidget *w, GdkEventCrossing *e, gpointer d)
{
    (void)w;
    (void)d;
    if (e->detail != GDK_NOTIFY_INFERIOR) close_hover();
    return FALSE;
}

/* Detailansicht: Bild, Beschreibung, und die Folgen als Baum mit Häkchen */
static void open_detail(Group *group)
{
    close_hover();
    GtkWidget *d = gtk_dialog_new_with_buttons(group->name, GTK_WINDOW(ui.win),
                                               GTK_DIALOG_MODAL | GTK_DIALOG_USE_HEADER_BAR,
                                               NULL, NULL);
    add_css_class(gtk_dialog_add_button(GTK_DIALOG(d), "Fertig", GTK_RESPONSE_OK),
                  "suggested-action");
    gtk_window_set_default_size(GTK_WINDOW(d), 820, group->is_film ? 420 : 680);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 18);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    GtkWidget *image = group->cover_path ? gtk_image_new_from_file(group->cover_path)
                       : gtk_image_new_from_icon_name("video-x-generic-symbolic",
                                                      GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_size_request(image, 384, 216);
    gtk_widget_set_valign(image, GTK_ALIGN_START);
    add_css_class(image, "zd-cover");
    gtk_box_pack_start(GTK_BOX(top), image, FALSE, FALSE, 0);
    GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *heading = new_label(group->name, "zd-hero");
    set_wrapping(heading, 30);
    gtk_label_set_width_chars(GTK_LABEL(heading), 24);  /* sonst misst GTK sehr hohe Zeilen */
    gtk_box_pack_start(GTK_BOX(info), heading, FALSE, FALSE, 0);
    GtkWidget *description = new_label(group->description ? group->description : "", "zd-lead");
    set_wrapping(description, 44);
    gtk_label_set_width_chars(GTK_LABEL(description), 36);
    gtk_label_set_lines(GTK_LABEL(description), 7);
    gtk_label_set_ellipsize(GTK_LABEL(description), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(info), description, FALSE, FALSE, 0);
    add_title_facts(info, group, &ui.detail_rating);
    GtkWidget *imdb = new_imdb_button(group->name, group->is_film);
    gtk_widget_set_halign(imdb, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(info), imdb, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(top), info, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);

    ui.detail_note = new_label("Alle Folgen werden geladen …", "dim-label");
    gtk_widget_set_no_show_all(ui.detail_note, TRUE);
    gtk_box_pack_start(GTK_BOX(box), ui.detail_note, FALSE, FALSE, 0);

    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    GtkWidget *holder = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    ui.detail_tree.list = add_group(holder, NULL);
    g_signal_connect(ui.detail_tree.list, "row-activated", G_CALLBACK(on_result_row_activated),
                     &ui.detail_tree);
    gtk_container_add(GTK_CONTAINER(scrolled), holder);
    gtk_box_pack_start(GTK_BOX(box), scrolled, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(d))), box, TRUE, TRUE, 0);

    ui.detail_group = group;
    ui.detail_tree.groups = g_ptr_array_new();
    g_ptr_array_add(ui.detail_tree.groups, group);
    ui.detail_tree.limit = RESULT_ROWS_BATCH;
    group->expanded = true;
    load_series(group);
    rebuild_tree(&ui.detail_tree);
    gtk_widget_set_visible(ui.detail_note, group->loading);
    gtk_window_set_position(GTK_WINDOW(d), GTK_WIN_POS_CENTER_ON_PARENT);
    gtk_widget_show_all(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);

    ui.detail_group = NULL;
    ui.detail_note = NULL;
    ui.detail_rating = NULL;
    ui.detail_tree.list = NULL;
    g_ptr_array_free(ui.detail_tree.groups, TRUE);
    ui.detail_tree.groups = NULL;
    g_ptr_array_set_size(ui.detail_tree.refs, 0);
}

static gboolean on_open_detail(gpointer group)
{
    open_detail(group);
    return G_SOURCE_REMOVE;
}

/* Der Dialog läuft in eigener Schleife, darum erst nach dem Klick */
static void on_tile_activated(GtkFlowBox *box, GtkFlowBoxChild *child, gpointer d)
{
    (void)box;
    (void)d;
    g_idle_add(on_open_detail, g_object_get_data(G_OBJECT(child), "group"));
}

/* Kachel anlegen */
static void add_tile(GtkWidget *box, Group *group)
{
    GtkWidget *tile = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_halign(tile, GTK_ALIGN_CENTER);
    GtkWidget *overlay = gtk_overlay_new();
    group->tile_image = group->cover ? gtk_image_new_from_pixbuf(group->cover)
                        : gtk_image_new_from_icon_name("video-x-generic-symbolic",
                                                       GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_size_request(group->tile_image, COVER_WIDTH, COVER_HEIGHT);
    add_css_class(group->tile_image, "zd-cover");
    gtk_container_add(GTK_CONTAINER(overlay), group->tile_image);
    group->tile_check = gtk_check_button_new();
    add_css_class(group->tile_check, "zd-tile-check");
    gtk_widget_set_halign(group->tile_check, GTK_ALIGN_START);
    gtk_widget_set_valign(group->tile_check, GTK_ALIGN_START);
    gtk_widget_set_margin_start(group->tile_check, 6);
    gtk_widget_set_margin_top(group->tile_check, 6);
    gtk_widget_set_tooltip_text(group->tile_check, group->is_film ? "Auswählen"
                                : "Alle fehlenden Folgen auswählen");
    g_signal_connect(group->tile_check, "toggled", G_CALLBACK(on_tile_toggled), group);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), group->tile_check);
    GtkWidget *source_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    add_css_class(source_box, "zd-tile-badges");
    gtk_widget_set_halign(source_box, GTK_ALIGN_END);
    gtk_widget_set_valign(source_box, GTK_ALIGN_START);
    gtk_widget_set_margin_end(source_box, 6);
    gtk_widget_set_margin_top(source_box, 6);
    for (int i = 0; i < SOURCE_COUNT; i++)
        if (group->source & SOURCE_BITS[i])
            gtk_box_pack_start(GTK_BOX(source_box),
                               new_badge(md_source_label(SOURCE_BITS[i]), NULL), FALSE, FALSE, 0);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), source_box);
    group->tile_badges = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    add_css_class(group->tile_badges, "zd-tile-badges");
    gtk_widget_set_halign(group->tile_badges, GTK_ALIGN_END);
    gtk_widget_set_valign(group->tile_badges, GTK_ALIGN_END);
    gtk_widget_set_margin_end(group->tile_badges, 6);
    gtk_widget_set_margin_bottom(group->tile_badges, 6);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), group->tile_badges);
    gtk_box_pack_start(GTK_BOX(tile), overlay, FALSE, FALSE, 0);
    GtkWidget *title = new_label(group->name, "zd-title");
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title), 10);
    gtk_widget_set_size_request(title, COVER_WIDTH, -1);
    gtk_box_pack_start(GTK_BOX(tile), title, FALSE, FALSE, 0);
    GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_size_request(line, COVER_WIDTH, -1);
    group->tile_sub = new_label("", "zd-sub");
    add_css_class(group->tile_sub, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(group->tile_sub), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(group->tile_sub), 10);
    gtk_box_pack_start(GTK_BOX(line), group->tile_sub, TRUE, TRUE, 0);
    GtkWidget *imdb = new_imdb_button(group->name, group->is_film);
    gtk_widget_set_valign(imdb, GTK_ALIGN_CENTER);
    gtk_box_pack_end(GTK_BOX(line), imdb, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tile), line, FALSE, FALSE, 0);
    /* eigene Ereignisfläche, sonst meldet die Kachel kein Betreten und Verlassen */
    GtkWidget *hover = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(hover), FALSE);
    gtk_widget_set_halign(hover, GTK_ALIGN_CENTER);
    gtk_widget_add_events(hover, GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);
    gtk_container_add(GTK_CONTAINER(hover), tile);
    g_signal_connect(hover, "enter-notify-event", G_CALLBACK(on_tile_enter), group);
    g_signal_connect(hover, "leave-notify-event", G_CALLBACK(on_tile_leave), group);
    gtk_flow_box_insert(GTK_FLOW_BOX(box), hover, -1);
    g_object_set_data(G_OBJECT(gtk_widget_get_parent(hover)), "group", group);
    request_cover(group);
}

/* Gibt es eine Tonspur in dieser Sprache? */
static bool group_has_language(const Group *group, const char *language)
{
    for (guint s = 0; s < group->seasons->len; s++) {
        const Season *season = group->seasons->pdata[s];
        for (guint i = 0; i < season->items->len; i++) {
            const GPtrArray *audio = ((const Item *)season->items->pdata[i])->title->audio;
            for (guint a = 0; a < audio->len; a++)
                if (!strcmp(((const MdTrack *)audio->pdata[a])->label, language)) return true;
        }
    }
    return false;
}

/* Kategorie der Kachel? Noch unbekannte Kategorien zählen nirgends. */
static bool group_has_genre(const Group *group, const char *genre)
{
    if (!group->genres) return false;
    if (!strcmp(genre, NO_GENRE)) return !*group->genres;
    return g_strv_contains((const char *const *)group->genres, genre);
}

/* Passt eine Kachel zu den Filtern oben? filter ist kleingeschrieben. */
/* Zeile unter dem Filterfeld: sagt, dass Enter alle Mediatheken durchsucht, und was es fand */
static void update_search_hint(const char *filter, int shown)
{
    char *text = NULL;
    if (ui.searching)
        text = g_strdup_printf("Suche „%s“ in allen Mediatheken …", ui.searching);
    else if (*filter && !g_strcmp0(filter, ui.searched))
        text = g_strdup(ui.search_result);
    else if (*filter && shown == 0)
        text = g_strdup("Nichts in der Vorschau, sie zeigt nur die neuesten Titel. Enter sucht "
                        "in allen Mediatheken.");
    else if (*filter)
        text = g_strdup("Enter sucht auch in allen Mediatheken, nicht nur unter den neuesten "
                        "Titeln.");
    set_label_text(ui.search_hint, text ? text : "");
    gtk_widget_set_visible(ui.search_hint, text != NULL);
    g_free(text);
}

static gboolean on_search_done(gpointer data)
{
    SearchJob *job = data;
    g_clear_pointer(&ui.searching, g_free);
    if (!ui.preview_busy) {
        gtk_spinner_stop(GTK_SPINNER(ui.preview_spinner));
        gtk_widget_hide(ui.preview_spinner);
    }
    char *lower = g_utf8_strdown(job->text, -1);
    g_free(ui.searched);
    ui.searched = lower;
    g_clear_pointer(&ui.search_result, g_free);
    if (job->items && ui.preview && !ui.preview_busy) {
        close_hover();
        GPtrArray *groups = group_items(job->items);
        g_ptr_array_set_free_func(job->items, NULL);        /* gehören jetzt den Staffeln */
        g_ptr_array_free(job->items, TRUE);
        guint found = groups->len;
        for (guint i = 0; i < groups->len; i++) {
            Group *group = groups->pdata[i];
            Group *known = g_hash_table_lookup(ui.preview_index, group->key);
            if (known) {
                /* schon in der Vorschau: die dortige Gruppe gilt, sie hat schon Bild und Daten */
                if (!known->found_by) known->found_by = g_strdup(lower);
                group_free(group);
                continue;
            }
            group->complete = group->is_film;
            group->found_by = g_strdup(lower);
            g_ptr_array_add(ui.preview, group);
            g_hash_table_insert(ui.preview_index, group->key, group);
            request_group_genres(group);
        }
        g_ptr_array_set_free_func(groups, NULL);
        g_ptr_array_free(groups, TRUE);
        ui.search_result =
            found == 0 ? g_strdup_printf("„%s“ ist in keiner Mediathek zu finden.", job->text)
            : g_strdup_printf("%u Titel aus allen Mediatheken für „%s“%s.", found, job->text,
                              job->truncated ? ", nur die ersten Treffer – genauer suchen hilft"
                                             : "");
        g_ptr_array_sort(ui.preview, compare_newest);
        update_language_choices();
        ui.film_limit = ui.series_limit = PREVIEW_TILES_BATCH;
    } else {
        if (job->items) g_ptr_array_free(job->items, TRUE);
        ui.search_result = g_strdup(job->error ? job->error
                                    : "Die Suche ging nicht, bitte gleich noch einmal.");
    }
    rebuild_preview();
    g_free(job->text);
    g_free(job->dir);
    g_free(job->error);
    g_free(job);
    return G_SOURCE_REMOVE;
}

static gpointer search_thread(gpointer data)
{
    SearchJob *job = data;
    MdQuery query = {.text = job->text, .kind = MD_KIND_ALL, .sources = MD_SOURCE_ALL,
                     .min_minutes = job->min_minutes, .hide_variants = job->hide_variants,
                     .cache = MD_CACHE_USE};
    GPtrArray *titles = md_search(&query, NULL, &job->truncated, &job->error);
    if (titles) {
        job->items = g_ptr_array_new_with_free_func(item_free);
        add_items(job->items, titles, job->dir);
    }
    g_idle_add(on_search_done, job);
    return NULL;
}

/* Enter im Filterfeld: alle Mediatheken fragen und die Treffer in die Vorschau aufnehmen */
static void on_preview_search_activate(GtkEntry *entry, gpointer d)
{
    (void)d;
    char *text = g_strstrip(g_strdup(gtk_entry_get_text(entry)));
    if (!*text || ui.searching || ui.preview_busy || !ui.preview) {
        g_free(text);
        return;
    }
    ui.searching = g_strdup(text);
    SearchJob *job = g_new0(SearchJob, 1);
    job->text = text;
    job->dir = g_strdup(ui.cfg.dir);
    job->min_minutes = ui.cfg.min_minutes;
    job->hide_variants = !ui.cfg.show_signed;
    gtk_widget_show(ui.preview_spinner);
    gtk_spinner_start(GTK_SPINNER(ui.preview_spinner));
    rebuild_preview();
    g_thread_unref(g_thread_new("suche", search_thread, job));
}

static bool tile_matches(const Group *group, const char *filter)
{
    int kind = gtk_combo_box_get_active(GTK_COMBO_BOX(ui.preview_kind));
    if (!(group->source & ui.cfg.sources)) return false;
    if (kind == MD_KIND_FILMS && !group->is_film) return false;
    if (kind == MD_KIND_SERIES && group->is_film) return false;
    if (ui.active_genre && !group_has_genre(group, ui.active_genre)) return false;
    char *language = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(ui.preview_language));
    bool speaks = !language || !strcmp(language, ALL_LANGUAGES) ||
                  group_has_language(group, language);
    g_free(language);
    if (!speaks) return false;
    /* die Suche findet auch über die Beschreibung; solche Treffer bleiben bei ihrem Begriff */
    if (*filter && !g_strcmp0(group->found_by, filter)) return true;
    char *lower = g_utf8_strdown(group->name, -1);
    bool match = strstr(lower, filter) != NULL;
    g_free(lower);
    return match;
}

/* Kacheln neu aufbauen */
static void rebuild_preview(void)
{
    close_hover();
    update_series_news();
    for (guint i = 0; ui.preview && i < ui.preview->len; i++) {
        Group *group = ui.preview->pdata[i];
        group->tile_image = group->tile_check = group->tile_sub = group->tile_badges = NULL;
    }
    GtkWidget *boxes[] = {ui.film_box, ui.series_box};
    for (size_t b = 0; b < G_N_ELEMENTS(boxes); b++) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(boxes[b]));
        for (GList *l = children; l; l = l->next) gtk_widget_destroy(l->data);
        g_list_free(children);
    }
    char *filter = g_strstrip(g_utf8_strdown(gtk_entry_get_text(GTK_ENTRY(ui.preview_search)),
                                             -1));
    int films = 0, series = 0;
    for (guint i = 0; ui.preview && i < ui.preview->len; i++) {
        Group *group = ui.preview->pdata[i];
        if (!tile_matches(group, filter)) continue;
        int *count = group->is_film ? &films : &series;
        if (*count < (group->is_film ? ui.film_limit : ui.series_limit))
            add_tile(group->is_film ? ui.film_box : ui.series_box, group);
        (*count)++;
    }
    update_search_hint(filter, films + series);
    g_free(filter);
    char *text = g_strdup_printf("Filme (%d)", films);
    set_label_text(ui.film_title, text);
    g_free(text);
    text = g_strdup_printf("Serien (%d)", series);
    set_label_text(ui.series_title, text);
    g_free(text);
    gtk_widget_set_visible(ui.film_more, films > ui.film_limit);
    gtk_widget_set_visible(ui.series_more, series > ui.series_limit);
    int kind = gtk_combo_box_get_active(GTK_COMBO_BOX(ui.preview_kind));
    gtk_widget_set_visible(ui.film_title, kind != MD_KIND_SERIES);
    gtk_widget_set_visible(ui.film_box, kind != MD_KIND_SERIES);
    gtk_widget_set_visible(ui.series_title, kind != MD_KIND_FILMS);
    gtk_widget_set_visible(ui.series_box, kind != MD_KIND_FILMS);
    gtk_widget_show_all(ui.film_box);
    gtk_widget_show_all(ui.series_box);
    update_checks();
}

/* Kategorien zählen -> Name auf Anzahl */
static GHashTable *count_genres(void)
{
    GHashTable *counts = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; ui.preview && i < ui.preview->len; i++) {
        const Group *group = ui.preview->pdata[i];
        if (!(group->source & ui.cfg.sources)) continue;
        char *none[] = {NO_GENRE, NULL};
        for (char **g = group->genres && !*group->genres ? none : group->genres; g && *g; g++) {
            int count = GPOINTER_TO_INT(g_hash_table_lookup(counts, *g));
            g_hash_table_insert(counts, *g, GINT_TO_POINTER(count + 1));
        }
    }
    return counts;
}

/* häufigste Kategorie zuerst */
static gint compare_genre_names(gconstpointer a, gconstpointer b, gpointer d)
{
    int x = GPOINTER_TO_INT(g_hash_table_lookup(d, a));
    int y = GPOINTER_TO_INT(g_hash_table_lookup(d, b));
    return x != y ? y - x : g_utf8_collate(a, b);
}

/* Knöpfe zum gewählten Stand bringen */
static void sync_genre_chips(void)
{
    ui.updating_chips = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.genre_all), !ui.active_genre);
    GHashTableIter iter;
    gpointer name, chip;
    g_hash_table_iter_init(&iter, ui.genre_chips);
    while (g_hash_table_iter_next(&iter, &name, &chip))
        gtk_toggle_button_set_active(chip, !g_strcmp0(name, ui.active_genre));
    ui.updating_chips = false;
}

/* Kategorie gewählt; "Alle" und ein zweiter Klick zeigen wieder alles */
static void on_genre_toggled(GtkToggleButton *b, gpointer d)
{
    if (ui.updating_chips) return;
    g_free(ui.active_genre);
    ui.active_genre = gtk_toggle_button_get_active(b) && d ? g_strdup(d) : NULL;
    sync_genre_chips();
    ui.film_limit = ui.series_limit = PREVIEW_TILES_BATCH;
    rebuild_preview();
}

static void chip_destroy(GtkWidget *child, gpointer d)
{
    (void)d;
    if (gtk_bin_get_child(GTK_BIN(child)) != ui.genre_all) gtk_widget_destroy(child);
}

/* Knopfleiste: läuft die Ermittlung noch, kommen neue Knöpfe nur hinten dazu, sonst springt
 * der Knopf unter dem Mauszeiger weg. Am Ende wird nach Häufigkeit sortiert. */
static void update_genre_chips(void)
{
    GHashTable *counts = count_genres();
    GList *names = g_hash_table_get_keys(counts);
    names = g_list_sort_with_data(names, compare_genre_names, counts);
    if (!ui.genre_pending) {
        g_hash_table_remove_all(ui.genre_chips);
        gtk_container_foreach(GTK_CONTAINER(ui.genre_box), chip_destroy, NULL);
    }
    for (GList *l = names; l; l = l->next) {
        GtkWidget *chip = g_hash_table_lookup(ui.genre_chips, l->data);
        if (!chip) {
            chip = gtk_toggle_button_new();
            add_css_class(chip, "zd-chip");
            char *name = g_strdup(l->data);
            g_object_set_data_full(G_OBJECT(chip), "name", name, g_free);
            g_signal_connect(chip, "toggled", G_CALLBACK(on_genre_toggled), name);
            g_hash_table_insert(ui.genre_chips, g_strdup(name), chip);
            gtk_flow_box_insert(GTK_FLOW_BOX(ui.genre_box), chip, -1);
            gtk_widget_show_all(gtk_widget_get_parent(chip));
        }
        char *text = g_strdup_printf("%s  %d", (char *)l->data,
                                     GPOINTER_TO_INT(g_hash_table_lookup(counts, l->data)));
        gtk_button_set_label(GTK_BUTTON(chip), text);
        g_free(text);
    }
    g_list_free(names);
    g_hash_table_destroy(counts);
    sync_genre_chips();
    if (!ui.preview_busy) {
        char *note = ui.genre_pending
                     ? g_strdup_printf("Kategorien werden ermittelt, noch %d Abfragen …",
                                       ui.genre_pending)
                     : g_strdup("");
        set_label_text(ui.preview_note, note);
        g_free(note);
    }
}

/* Timer: neue Kategorien zeigen */
static void update_genres(void)
{
    if (!ui.genres_dirty || (ui.genre_pending && ++ui.genre_tick % GENRE_TICKS)) return;
    ui.genres_dirty = false;
    update_genre_chips();
    if (ui.active_genre) rebuild_preview();
    else update_checks();
}

static void on_preview_filter_changed(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
    ui.film_limit = ui.series_limit = PREVIEW_TILES_BATCH;
    rebuild_preview();
}

/* Sprachen der geladenen Titel zur Auswahl stellen; die gewählte bleibt, wenn es sie noch gibt */
static void update_language_choices(void)
{
    GtkComboBoxText *combo = GTK_COMBO_BOX_TEXT(ui.preview_language);
    char *active = gtk_combo_box_text_get_active_text(combo);
    GHashTable *found = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint g = 0; g < ui.preview->len; g++) {
        const Group *group = ui.preview->pdata[g];
        for (guint s = 0; s < group->seasons->len; s++) {
            const Season *season = group->seasons->pdata[s];
            for (guint i = 0; i < season->items->len; i++) {
                const GPtrArray *audio = ((const Item *)season->items->pdata[i])->title->audio;
                for (guint a = 0; a < audio->len; a++)
                    g_hash_table_add(found, ((MdTrack *)audio->pdata[a])->label);
            }
        }
    }
    GList *names = g_list_sort(g_hash_table_get_keys(found), (GCompareFunc)g_utf8_collate);
    g_signal_handlers_block_by_func(combo, on_preview_filter_changed, NULL);
    gtk_combo_box_text_remove_all(combo);
    gtk_combo_box_text_append(combo, ALL_LANGUAGES, ALL_LANGUAGES);
    for (GList *l = names; l; l = l->next) gtk_combo_box_text_append(combo, l->data, l->data);
    if (!active || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), active))
        gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    g_signal_handlers_unblock_by_func(combo, on_preview_filter_changed, NULL);
    g_list_free(names);
    g_hash_table_destroy(found);
    g_free(active);
}

static void on_more_films_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    ui.film_limit += PREVIEW_TILES_BATCH;
    rebuild_preview();
}

static void on_more_series_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    ui.series_limit += PREVIEW_TILES_BATCH;
    rebuild_preview();
}

/* neueste zuerst */
static int compare_newest(gconstpointer a, gconstpointer b)
{
    gint64 x = (*(Group *const *)a)->newest, y = (*(Group *const *)b)->newest;
    return x < y ? 1 : x > y ? -1 : 0;
}

/* Startliste übernehmen */
static gboolean on_preview_done(gpointer data)
{
    PreviewJob *job = data;
    ui.preview_busy = false;
    gtk_spinner_stop(GTK_SPINNER(ui.preview_spinner));
    gtk_widget_hide(ui.preview_spinner);
    if (job->items) {
        close_hover();
        if (ui.preview_index) g_hash_table_destroy(ui.preview_index);
        if (ui.preview) g_ptr_array_free(ui.preview, TRUE);
        ui.preview = group_items(job->items);
        g_clear_pointer(&ui.searched, g_free);      /* ihre Treffer sind mit der alten Liste weg */
        update_language_choices();
        g_ptr_array_set_free_func(job->items, NULL);        /* gehören jetzt den Staffeln */
        g_ptr_array_sort(ui.preview, compare_newest);
        ui.preview_index = g_hash_table_new(g_str_hash, g_str_equal);
        for (guint i = 0; i < ui.preview->len; i++) {
            Group *group = ui.preview->pdata[i];
            group->complete = group->is_film;
            g_hash_table_insert(ui.preview_index, group->key, group);
        }
        ui.film_limit = ui.series_limit = PREVIEW_TILES_BATCH;
        set_label_text(ui.preview_note, "");
        rebuild_preview();
        g_ptr_array_free(job->items, TRUE);
        request_all_genres();
        rebuild_videos(true);           /* Titelbilder der Mediathek */
        start_news_load();
    } else {
        set_label_text(ui.preview_note, job->error ? job->error : "Keine Vorschau verfügbar.");
    }
    g_free(job->dir);
    g_free(job->error);
    g_free(job);
    return G_SOURCE_REMOVE;
}

static gpointer preview_thread(gpointer data)
{
    PreviewJob *job = data;
    MdQuery films = {.text = "", .kind = MD_KIND_FILMS, .hide_variants = job->hide_variants,
                     .min_minutes = MAX(job->min_minutes, PREVIEW_FILM_MIN_MINUTES),
                     .sources = MD_SOURCE_ALL, .cache = job->cache};
    MdQuery series = {.text = "", .kind = MD_KIND_SERIES, .hide_variants = job->hide_variants,
                      .min_minutes = job->min_minutes, .sources = MD_SOURCE_ALL,
                      .cache = job->cache};
    GPtrArray *titles = md_search(&films, NULL, NULL, &job->error);
    GPtrArray *more = titles ? md_search(&series, NULL, NULL, &job->error) : NULL;
    if (more) {
        job->items = g_ptr_array_new_with_free_func(item_free);
        add_items(job->items, titles, job->dir);
        add_items(job->items, more, job->dir);
    } else if (titles) {
        g_ptr_array_free(titles, TRUE);
    }
    g_idle_add(on_preview_done, job);
    return NULL;
}

static void start_preview_load(MdCache cache)
{
    if (ui.preview_busy) return;
    ui.preview_busy = true;
    ui.preview_refreshed = cache == MD_CACHE_REFRESH;
    PreviewJob *job = g_new0(PreviewJob, 1);
    job->cache = cache;
    job->dir = g_strdup(ui.cfg.dir);
    job->min_minutes = ui.cfg.min_minutes;
    job->hide_variants = !ui.cfg.show_signed;
    gtk_widget_show(ui.preview_spinner);
    gtk_spinner_start(GTK_SPINNER(ui.preview_spinner));
    set_label_text(ui.preview_note, "Neue Filme und Serien werden geladen …");
    g_thread_unref(g_thread_new("vorschau", preview_thread, job));
}

/* "Aktualisieren": die Auswahl der Kacheln geht dabei verloren, also vorher fragen */
static void on_preview_refresh_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    GPtrArray *selected = g_ptr_array_new();
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    collect_from(selected, seen, ui.preview);
    bool any = selected->len > 0;
    g_hash_table_destroy(seen);
    g_ptr_array_free(selected, TRUE);
    if (!any || ask_confirmation("Vorschau neu laden?",
                                 "Die Auswahl in der Vorschau geht verloren.", "Behalten",
                                 "Neu laden", false))
        start_preview_load(MD_CACHE_REFRESH);
}

/* ein Abschnitt: Überschrift, Kacheln, "Weitere anzeigen" */
static void add_preview_section(GtkWidget *page, GtkWidget **title, GtkWidget **box,
                                GtkWidget **more, GCallback on_more)
{
    *title = new_label("", "zd-heading");
    gtk_box_pack_start(GTK_BOX(page), *title, FALSE, FALSE, 0);
    *box = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(*box), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(*box), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(*box), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(*box), TILES_PER_LINE_MAX);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(*box), 6);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(*box), 6);
    g_signal_connect(*box, "child-activated", G_CALLBACK(on_tile_activated), NULL);
    gtk_box_pack_start(GTK_BOX(page), *box, FALSE, FALSE, 0);
    *more = gtk_button_new_with_label("Weitere anzeigen");
    gtk_widget_set_halign(*more, GTK_ALIGN_CENTER);
    gtk_widget_set_no_show_all(*more, TRUE);
    g_signal_connect(*more, "clicked", on_more, NULL);
    gtk_box_pack_start(GTK_BOX(page), *more, FALSE, FALSE, 6);
}

static GtkWidget *build_preview_page(void)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *top = new_column();
    gtk_widget_set_margin_top(top, 14);
    gtk_widget_set_margin_bottom(top, 4);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    ui.preview_search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui.preview_search),
                                   "Filtern – Enter sucht in allen Mediatheken (Strg+F)");
    g_signal_connect(ui.preview_search, "search-changed", G_CALLBACK(on_preview_filter_changed),
                     NULL);
    g_signal_connect(ui.preview_search, "activate", G_CALLBACK(on_preview_search_activate), NULL);
    gtk_box_pack_start(GTK_BOX(bar), ui.preview_search, TRUE, TRUE, 0);
    ui.preview_kind = new_combo(KIND_TEXT, 3, MD_KIND_ALL);
    g_signal_connect(ui.preview_kind, "changed", G_CALLBACK(on_preview_filter_changed), NULL);
    gtk_box_pack_start(GTK_BOX(bar), ui.preview_kind, FALSE, FALSE, 0);
    ui.preview_language = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.preview_language), ALL_LANGUAGES,
                              ALL_LANGUAGES);
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui.preview_language), 0);
    gtk_widget_set_tooltip_text(ui.preview_language, "Zeigt nur Titel mit dieser Tonspur");
    g_signal_connect(ui.preview_language, "changed", G_CALLBACK(on_preview_filter_changed), NULL);
    gtk_box_pack_start(GTK_BOX(bar), ui.preview_language, FALSE, FALSE, 0);
    ui.preview_spinner = gtk_spinner_new();
    gtk_box_pack_start(GTK_BOX(bar), ui.preview_spinner, FALSE, FALSE, 0);
    GtkWidget *refresh = new_icon_button("view-refresh-symbolic", "Vorschau neu laden");
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_preview_refresh_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(bar), refresh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), build_source_bar(), FALSE, FALSE, 0);
    ui.genre_box = gtk_flow_box_new();
    add_css_class(ui.genre_box, "zd-chips");
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(ui.genre_box), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(ui.genre_box), 30);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(ui.genre_box), 3);
    gtk_widget_set_margin_top(ui.genre_box, 6);
    ui.genre_all = gtk_toggle_button_new_with_label("Alle");
    add_css_class(ui.genre_all, "zd-chip");
    g_signal_connect(ui.genre_all, "toggled", G_CALLBACK(on_genre_toggled), NULL);
    gtk_flow_box_insert(GTK_FLOW_BOX(ui.genre_box), ui.genre_all, -1);
    ui.genre_chips = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gtk_box_pack_start(GTK_BOX(top), ui.genre_box, FALSE, FALSE, 0);
    ui.preview_note = new_label("", "zd-sub");
    add_css_class(ui.preview_note, "dim-label");
    set_wrapping(ui.preview_note, 90);
    gtk_box_pack_start(GTK_BOX(top), ui.preview_note, FALSE, FALSE, 0);
    ui.search_hint = new_label("", "zd-sub");
    set_wrapping(ui.search_hint, 90);
    gtk_widget_set_no_show_all(ui.search_hint, TRUE);
    gtk_box_pack_start(GTK_BOX(top), ui.search_hint, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);

    GtkWidget *scrolled, *column = new_page_column(&scrolled);
    gtk_widget_set_margin_top(column, 0);
    add_preview_section(column, &ui.film_title, &ui.film_box, &ui.film_more,
                        G_CALLBACK(on_more_films_clicked));
    add_preview_section(column, &ui.series_title, &ui.series_box, &ui.series_more,
                        G_CALLBACK(on_more_series_clicked));
    gtk_box_pack_start(GTK_BOX(page), scrolled, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), build_footer(FOOTER_PREVIEW), FALSE, FALSE, 0);
    return page;
}

/* ---------------------------------------------------------------------------
 * Neue Folgen (oben in "Meine Videos")
 * ---------------------------------------------------------------------------
 * Fragt für jede Serie aus dem Verlauf nach, was seit dem letzten Download gesendet wurde. So
 * erscheinen auch Serien, die gar nicht in der Vorschau stehen.
 * ------------------------------------------------------------------------- */
/* Ist der Titel eine Folge dieser Serie, gesendet seit finished und noch nicht geladen? */
static bool title_is_news(const MdTitle *t, const char *topic, gint64 finished, const char *dir)
{
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    if (t->is_film || t->timestamp <= finished || t->timestamp > now) return false;
    char *a = g_utf8_strdown(t->topic, -1), *b = g_utf8_strdown(topic, -1);
    bool same = !strcmp(a, b);
    g_free(a);
    g_free(b);
    return same && md_title_status(t, dir, NULL) == MD_STATUS_MISSING;
}

/* Einstellung "Neue Folgen automatisch laden": alle gefundenen Folgen mit allen Spuren in die
 * Warteschlange */
static void queue_news(void)
{
    MdJobOptions options = {.dir = ui.cfg.dir, .quality = (MdQuality)ui.cfg.quality,
                            .convert = ui.cfg.convert, .encoder = (MdEncoder)ui.cfg.encoder,
                            .gpu_quality = ui.cfg.gpu_quality, .cpu_rf = ui.cfg.cpu_rf,
                            .keep_original = ui.cfg.keep_original};
    GHashTable *keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    int queued = 0;
    for (guint g = 0; ui.news_tree.groups && g < ui.news_tree.groups->len; g++) {
        const Group *group = ui.news_tree.groups->pdata[g];
        for (guint s = 0; s < group->seasons->len; s++) {
            const Season *season = group->seasons->pdata[s];
            for (guint i = 0; i < season->items->len; i++) {
                Item *item = season->items->pdata[i];
                if (!item_open(item)) continue;
                md_queue_add(item->title, &options);
                item->selected = false;
                g_hash_table_add(keys, md_title_key(item->title));
                queued++;
            }
        }
    }
    if (queued) {
        md_log("Neue Folgen automatisch eingereiht: %d", queued);
        char *text = g_strdup_printf("%d neue %s automatisch in die Warteschlange gestellt.",
                                     queued, plural(queued, "Folge", "Folgen"));
        show_info_bar(GTK_MESSAGE_INFO, text);
        g_free(text);
        refresh_statuses(keys);
        update_checks();
    }
    g_hash_table_destroy(keys);
}

static gboolean on_news_done(gpointer data)
{
    NewsJob *job = data;
    if (ui.news_tree.groups) g_ptr_array_free(ui.news_tree.groups, TRUE);
    ui.news_tree.groups = group_items(job->items);
    g_ptr_array_set_free_func(job->items, NULL);            /* gehören jetzt den Staffeln */
    /* zugeklappt: eine Zeile je Serie, darunter bleiben die eigenen Videos im Blick */
    int series = (int)ui.news_tree.groups->len;
    char *title = series ? g_strdup_printf("Neue Folgen deiner Serien (%d)", series)
                         : g_strdup("Neue Folgen deiner Serien");
    char *note = series ? g_strdup_printf("%d %s mit neuen Folgen seit dem letzten Download",
                                          series, plural(series, "Serie", "Serien"))
                        : g_strdup("Alle geladenen Serien sind aktuell.");
    set_label_text(ui.news_heading, title);
    gtk_container_child_set(GTK_CONTAINER(ui.stack), ui.videos_page, "needs-attention",
                            series > 0, NULL);
    set_label_text(ui.news_note, note);
    g_free(title);
    g_free(note);
    ui.news_tree.empty_text = "Keine neuen Folgen.";
    ui.news_busy = false;
    show_news_busy(false);
    rebuild_tree(&ui.news_tree);
    show_news_list();
    if (ui.cfg.auto_news) queue_news();
    if (ui.season_offers) g_hash_table_destroy(ui.season_offers);
    ui.season_offers = job->offers;
    rebuild_videos(true);           /* Staffeln und Serien als komplett markieren */
    g_ptr_array_free(job->items, TRUE);
    g_strfreev(job->topics);
    g_free(job->finished);
    g_free(job->dir);
    g_free(job);
    return G_SOURCE_REMOVE;
}

/* Stand der Suche: gerade geprüfte Serie */
typedef struct {
    int index, total;
    char *topic;
} NewsStep;

static gboolean on_news_step(gpointer data)
{
    NewsStep *step = data;
    char *text = g_strdup_printf("Serie %d von %d: %s", step->index + 1, step->total,
                                 step->topic);
    set_label_text(ui.news_busy_detail, text);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.news_progress),
                                  step->total ? (double)step->index / step->total : 0);
    g_free(text);
    g_free(step->topic);
    g_free(step);
    return G_SOURCE_REMOVE;
}

/* Liste, Auswahlknopf und Download-Leiste nur, wenn es neue Folgen gibt */
static void show_news_list(void)
{
    bool any = ui.news_tree.groups && ui.news_tree.groups->len > 0;
    GtkWidget *widgets[] = {gtk_widget_get_parent(ui.news_tree.list), ui.news_select,
                            ui.news_footer};
    for (size_t i = 0; i < G_N_ELEMENTS(widgets); i++) {
        if (!widgets[i]) continue;
        gtk_widget_set_no_show_all(widgets[i], !any);
        if (any) gtk_widget_show_all(widgets[i]);
        else gtk_widget_hide(widgets[i]);
    }
}

/* Hinweis "Suche nach neuen Folgen" in "Meine Videos" und in der Titelleiste */
static void show_news_busy(bool busy)
{
    gtk_widget_set_visible(ui.news_busy_box, busy);
    gtk_widget_set_visible(ui.header_spinner, busy);
    if (busy) {
        gtk_spinner_start(GTK_SPINNER(ui.news_spinner));
        gtk_spinner_start(GTK_SPINNER(ui.header_spinner));
        set_label_text(ui.news_busy_detail, "Liste der geladenen Serien wird gelesen …");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.news_progress), 0);
        set_label_text(ui.news_heading, "Neue Folgen deiner Serien");
        set_label_text(ui.news_note, "");
    } else {
        gtk_spinner_stop(GTK_SPINNER(ui.news_spinner));
        gtk_spinner_stop(GTK_SPINNER(ui.header_spinner));
    }
}

/* Höchste angebotene Folge je Staffel der Serie topic merken (für "komplett") */
static void record_offers(GHashTable *offers, GPtrArray *titles, const char *topic)
{
    char *lower = g_utf8_strdown(topic, -1);
    g_hash_table_replace(offers, g_strdup(lower), GINT_TO_POINTER(0));
    for (guint n = 0; n < titles->len; n++) {
        const MdTitle *t = titles->pdata[n];
        if (t->is_film || t->season < 0 || t->episode < 1) continue;
        char *a = g_utf8_strdown(t->topic, -1);
        if (!strcmp(a, lower)) {
            char *key = g_strdup_printf("%s\t%d", lower, t->season);
            int most = GPOINTER_TO_INT(g_hash_table_lookup(offers, key));
            g_hash_table_replace(offers, key, GINT_TO_POINTER(MAX(most, t->episode)));
        }
        g_free(a);
    }
    g_free(lower);
}

static gpointer news_thread(gpointer data)
{
    NewsJob *job = data;
    job->items = g_ptr_array_new_with_free_func(item_free);
    for (int i = 0; job->topics[i]; i++) {
        NewsStep *step = g_new0(NewsStep, 1);
        step->index = i;
        step->total = job->total;
        step->topic = g_strdup(job->topics[i]);
        g_idle_add(on_news_step, step);
        MdQuery query = {.text = job->topics[i], .kind = MD_KIND_ALL, .sources = MD_SOURCE_ALL,
                         .cache = MD_CACHE_USE, .min_minutes = job->min_minutes,
                         .hide_variants = job->hide_variants};
        char *error = NULL;
        GPtrArray *titles = md_search(&query, NULL, NULL, &error);
        g_free(error);
        if (!titles) continue;
        record_offers(job->offers, titles, job->topics[i]);
        GPtrArray *found = g_ptr_array_new();
        for (guint n = titles->len; n-- > 0;)
            if (title_is_news(titles->pdata[n], job->topics[i], job->finished[i], job->dir))
                g_ptr_array_add(found, g_ptr_array_steal_index(titles, n));
        g_ptr_array_free(titles, TRUE);
        add_items(job->items, found, job->dir);
    }
    g_idle_add(on_news_done, job);
    return NULL;
}

/* Serien aus dem Verlauf mit dem jeweils letzten Download prüfen */
static void start_news_load(void)
{
    if (ui.news_busy) return;
    ui.news_busy = true;
    int count = 0;
    MdHistoryEntry *entries = md_history_snapshot(&count);
    GHashTable *latest = g_hash_table_new(g_str_hash, g_str_equal);
    for (int i = 0; i < count; i++) {
        if (!strcmp(entries[i].topic, "Filme")) continue;
        gint64 finished = GPOINTER_TO_SIZE(g_hash_table_lookup(latest, entries[i].topic));
        g_hash_table_insert(latest, entries[i].topic,
                            GSIZE_TO_POINTER(MAX(finished, entries[i].finished)));
    }
    NewsJob *job = g_new0(NewsJob, 1);
    job->offers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    guint size = g_hash_table_size(latest), n = 0;
    job->topics = g_new0(char *, size + 1);
    job->finished = g_new0(gint64, size + 1);
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, latest);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        job->topics[n] = g_strdup(key);
        job->finished[n++] = GPOINTER_TO_SIZE(value);
    }
    job->total = (int)n;
    g_hash_table_destroy(latest);
    md_history_snapshot_free(entries, count);
    job->dir = g_strdup(ui.cfg.dir);
    job->min_minutes = ui.cfg.min_minutes;
    job->hide_variants = !ui.cfg.show_signed;
    ui.news_tree.empty_text = "Wird geprüft …";
    show_news_busy(true);
    g_thread_unref(g_thread_new("neue-folgen", news_thread, job));
}

/* bei laufendem Programm (z. B. aus dem Autostart) regelmäßig nachsehen */
static gboolean on_news_timer(gpointer d)
{
    (void)d;
    if (ui.cfg.auto_news) start_news_load();
    return G_SOURCE_CONTINUE;
}

static void on_news_refresh_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    start_news_load();
}

/* Alle neuen Folgen an oder wieder aus */
static void on_news_select_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    Counts c = {0};
    for (guint g = 0; ui.news_tree.groups && g < ui.news_tree.groups->len; g++)
        counts_add_group(&c, ui.news_tree.groups->pdata[g]);
    bool select = c.selected < c.open;
    for (guint g = 0; ui.news_tree.groups && g < ui.news_tree.groups->len; g++)
        group_select(ui.news_tree.groups->pdata[g], select);
    update_checks();
}

/* Abschnitt oben in "Meine Videos": Überschrift, Knöpfe, Fortschritt der Suche und Liste */
static GtkWidget *build_news_section(void)
{
    GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_top(section, 14);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    ui.news_heading = new_label("Neue Folgen deiner Serien", "zd-heading");
    gtk_box_pack_start(GTK_BOX(texts), ui.news_heading, FALSE, FALSE, 0);
    ui.news_note = new_label("Die Suche beginnt, sobald die Startliste geladen ist.", "zd-sub");
    add_css_class(ui.news_note, "dim-label");
    set_wrapping(ui.news_note, 60);
    gtk_box_pack_start(GTK_BOX(texts), ui.news_note, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), texts, TRUE, TRUE, 0);
    ui.news_select = gtk_button_new_with_label("Alle neuen Folgen an/aus");
    gtk_widget_set_valign(ui.news_select, GTK_ALIGN_CENTER);
    g_signal_connect(ui.news_select, "clicked", G_CALLBACK(on_news_select_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(bar), ui.news_select, FALSE, FALSE, 0);
    GtkWidget *refresh = new_icon_button("view-refresh-symbolic", "Neue Folgen prüfen");
    gtk_widget_set_valign(refresh, GTK_ALIGN_CENTER);
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_news_refresh_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(bar), refresh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(section), bar, FALSE, FALSE, 0);

    ui.news_busy_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    add_css_class(ui.news_busy_box, "zd-busy");
    gtk_widget_set_margin_top(ui.news_busy_box, 6);
    ui.news_spinner = gtk_spinner_new();
    gtk_widget_set_size_request(ui.news_spinner, 32, 32);
    gtk_widget_set_valign(ui.news_spinner, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(ui.news_busy_box), ui.news_spinner, FALSE, FALSE, 0);
    GtkWidget *busy_texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(busy_texts),
                       new_label("Suche nach neuen Folgen …", "zd-busy-title"), FALSE, FALSE, 0);
    ui.news_busy_detail = new_label("", "zd-sub");
    gtk_label_set_ellipsize(GTK_LABEL(ui.news_busy_detail), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(busy_texts), ui.news_busy_detail, FALSE, FALSE, 0);
    ui.news_progress = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(busy_texts), ui.news_progress, FALSE, FALSE, 2);
    gtk_box_pack_start(GTK_BOX(ui.news_busy_box), busy_texts, TRUE, TRUE, 0);
    gtk_widget_show_all(ui.news_busy_box);
    gtk_widget_hide(ui.news_busy_box);
    gtk_widget_set_no_show_all(ui.news_busy_box, TRUE);
    gtk_box_pack_start(GTK_BOX(section), ui.news_busy_box, FALSE, FALSE, 0);

    ui.news_tree.list = add_group(section, NULL);
    ui.news_tree.limit = RESULT_ROWS_BATCH;
    g_signal_connect(ui.news_tree.list, "row-activated", G_CALLBACK(on_result_row_activated),
                     &ui.news_tree);
    show_news_list();
    return section;
}

/* ---------------------------------------------------------------------------
 * Seite Downloads
 * ------------------------------------------------------------------------- */
static void job_row_free(gpointer data)
{
    JobRow *row = data;
    g_free(row->target);
    g_free(row->topic);
    g_free(row->status);
    g_free(row);
}

static void on_job_pause_clicked(GtkButton *b, gpointer d)
{
    JobRow *row = d;
    bool paused = g_object_get_data(G_OBJECT(b), "paused") != NULL;
    md_job_set_paused(row->id, !paused);
}

static void on_job_retry_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    md_job_retry(((JobRow *)d)->id);
}

static void on_job_remove_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    JobRow *row = d;
    if (g_object_get_data(G_OBJECT(row->row), "open") &&
        !ask_confirmation("Download abbrechen?", "Der bisher geladene Teil wird gelöscht.",
                          "Behalten", "Abbrechen und löschen", true))
        return;
    /* ein Fehlschlag mit Resten fragt auch, sonst sind sie ohne Warnung weg */
    gint64 bytes = g_object_get_data(G_OBJECT(row->row), "open") ? 0
                                                                  : md_job_partial_bytes(row->id);
    if (bytes > 0) {
        char *size = md_format_size(bytes);
        char *text = g_strdup_printf("Die halbfertigen Dateien (%s) werden gelöscht.", size);
        bool yes = ask_confirmation("Aus der Liste entfernen?", text, "Behalten", "Entfernen",
                                    true);
        g_free(size);
        g_free(text);
        if (!yes) return;
    }
    md_job_remove(row->id);
}

static void on_job_open_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    open_folder_of(((JobRow *)d)->target);
}

/* Zeile der Warteschlange an Platz order anlegen */
/* --- Aufträge einer Serie als Gruppe ----------------------------------------------------
 * Hat eine Sendung mindestens JOB_GROUP_MIN offene Aufträge, steht über ihrem ersten eine Zeile
 * mit dem Namen und Knöpfen für alle zusammen. Die Reihenfolge bleibt die der Abarbeitung. */
#define JOB_GROUP_MIN 3

static void on_group_pause(GtkButton *b, gpointer d)
{
    md_queue_pause_topic(g_object_get_data(G_OBJECT(b), "topic"), GPOINTER_TO_INT(d));
    on_tick(NULL);
}

static void on_group_remove(GtkButton *b, gpointer d)
{
    (void)d;
    const char *topic = g_object_get_data(G_OBJECT(b), "topic");
    int jobs = md_queue_topic_jobs(topic);
    if (!jobs) return;
    char *title = g_strdup_printf("Alle %d offenen Aufträge von „%s“ entfernen?", jobs, topic);
    bool yes = ask_confirmation(title, "Laufende Downloads und Umwandlungen werden abgebrochen, "
                                       "schon geladene Teile gelöscht. Fertige Videos bleiben.",
                                "Behalten", "Entfernen", true);
    g_free(title);
    if (!yes) return;
    md_queue_remove_topic(topic);
    on_tick(NULL);
}

static GtkWidget *new_group_header(const char *topic)
{
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    add_css_class(h, "zd-job-group");
    gtk_widget_set_margin_top(h, 8);
    gtk_widget_set_margin_bottom(h, 2);
    gtk_widget_set_margin_start(h, 14);
    gtk_widget_set_margin_end(h, 12);
    GtkWidget *label = new_label(topic, "zd-heading");
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(h), label, TRUE, TRUE, 0);
    const struct {
        const char *icon, *tip;
        GCallback callback;
        int arg;
    } buttons[] = {
        {"media-playback-pause-symbolic", "Alle Aufträge dieser Serie pausieren",
         G_CALLBACK(on_group_pause), 1},
        {"media-playback-start-symbolic", "Alle Aufträge dieser Serie fortsetzen",
         G_CALLBACK(on_group_pause), 0},
        {"window-close-symbolic", "Alle offenen Aufträge dieser Serie entfernen",
         G_CALLBACK(on_group_remove), 0},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(buttons); i++) {
        GtkWidget *b = new_icon_button(buttons[i].icon, buttons[i].tip);
        g_object_set_data_full(G_OBJECT(b), "topic", g_strdup(topic), g_free);
        g_signal_connect(b, "clicked", buttons[i].callback, GINT_TO_POINTER(buttons[i].arg));
        gtk_box_pack_start(GTK_BOX(h), b, FALSE, FALSE, 0);
    }
    g_object_set_data_full(G_OBJECT(h), "topic", g_strdup(topic), g_free);
    gtk_widget_show_all(h);
    return h;
}

static void job_row_header(GtkListBoxRow *this_row, GtkListBoxRow *before, gpointer data)
{
    (void)data;
    const JobRow *row = g_object_get_data(G_OBJECT(this_row), "job");
    const JobRow *prev = before ? g_object_get_data(G_OBJECT(before), "job") : NULL;
    GtkWidget *header = gtk_list_box_row_get_header(this_row);
    bool group = row && row->topic && ui.job_groups &&
                 g_hash_table_contains(ui.job_groups, row->topic) &&
                 (!prev || g_strcmp0(prev->topic, row->topic));
    if (group) {
        const char *shown = header ? g_object_get_data(G_OBJECT(header), "topic") : NULL;
        if (g_strcmp0(shown, row->topic))
            gtk_list_box_row_set_header(this_row, new_group_header(row->topic));
    } else if (!before) {
        gtk_list_box_row_set_header(this_row, NULL);
    } else if (!header || !GTK_IS_SEPARATOR(header)) {
        gtk_list_box_row_set_header(this_row, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    }
}

/* Welche Sendungen bilden eine Gruppe? -> hat sich das geändert? */
static bool update_job_groups(const MdJobView *jobs, int count)
{
    GHashTable *counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (int i = 0; i < count; i++) {
        if (jobs[i].state == MD_JOB_DONE || !jobs[i].topic || !*jobs[i].topic) continue;
        int n = GPOINTER_TO_INT(g_hash_table_lookup(counts, jobs[i].topic));
        g_hash_table_replace(counts, g_strdup(jobs[i].topic), GINT_TO_POINTER(n + 1));
    }
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, counts);
    while (g_hash_table_iter_next(&iter, &key, &value))
        if (GPOINTER_TO_INT(value) < JOB_GROUP_MIN) g_hash_table_iter_remove(&iter);
    bool changed = !ui.job_groups || g_hash_table_size(counts) != g_hash_table_size(ui.job_groups);
    g_hash_table_iter_init(&iter, counts);
    while (!changed && g_hash_table_iter_next(&iter, &key, NULL))
        changed = !g_hash_table_contains(ui.job_groups, key);
    if (ui.job_groups) g_hash_table_destroy(ui.job_groups);
    ui.job_groups = counts;
    return changed;
}

/* Meldung des Systems, wenn die Warteschlange leer gelaufen ist und das Fenster nicht vorn ist */
static void notify_queue_done(const MdJobView *jobs, int count)
{
    static int before = -1;
    int open = 0;
    for (int i = 0; i < count; i++) open += jobs[i].state != MD_JOB_DONE && jobs[i].state != MD_JOB_FAILED;
    if (before > 0 && open == 0 && !gtk_window_is_active(GTK_WINDOW(ui.win))) {
        GNotification *note = g_notification_new("Warteschlange fertig");
        g_notification_set_body(note, "Alle Downloads und Umwandlungen sind abgeschlossen.");
        g_application_send_notification(G_APPLICATION(ui.app), "warteschlange-fertig", note);
        g_object_unref(note);
    }
    before = open;
}

static JobRow *new_job_row(const MdJobView *j, int order)
{
    JobRow *row = g_new0(JobRow, 1);
    row->id = j->id;
    row->target = g_strdup(j->target);
    row->topic = g_strdup(j->topic);
    row->order = order;
    row->row = gtk_list_box_row_new();
    g_object_set_data(G_OBJECT(row->row), "job", row);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row->row), FALSE);
    GtkWidget *v = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_top(v, 10);
    gtk_widget_set_margin_bottom(v, 10);
    gtk_widget_set_margin_start(v, 14);
    gtk_widget_set_margin_end(v, 12);
    GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    row->title = new_label(j->title, "zd-title");
    gtk_label_set_ellipsize(GTK_LABEL(row->title), PANGO_ELLIPSIZE_END);
    row->subtitle = new_label("", "zd-sub");
    add_css_class(row->subtitle, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(row->subtitle), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(text), row->title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text), row->subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(h), text, TRUE, TRUE, 0);
    row->pause = new_icon_button("media-playback-pause-symbolic", "Pausieren");
    row->retry = new_icon_button("view-refresh-symbolic", "Erneut versuchen");
    row->open = new_icon_button("folder-open-symbolic", "Ordner öffnen");
    row->remove = new_icon_button("window-close-symbolic", "Aus der Liste entfernen");
    g_signal_connect(row->pause, "clicked", G_CALLBACK(on_job_pause_clicked), row);
    g_signal_connect(row->retry, "clicked", G_CALLBACK(on_job_retry_clicked), row);
    g_signal_connect(row->open, "clicked", G_CALLBACK(on_job_open_clicked), row);
    g_signal_connect(row->remove, "clicked", G_CALLBACK(on_job_remove_clicked), row);
    GtkWidget *buttons[] = {row->pause, row->retry, row->open, row->remove};
    for (size_t i = 0; i < G_N_ELEMENTS(buttons); i++) {
        gtk_widget_set_valign(buttons[i], GTK_ALIGN_CENTER);
        gtk_widget_set_no_show_all(buttons[i], TRUE);
        gtk_box_pack_start(GTK_BOX(h), buttons[i], FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(v), h, FALSE, FALSE, 0);
    row->bar = gtk_progress_bar_new();
    gtk_widget_set_no_show_all(row->bar, TRUE);
    gtk_box_pack_start(GTK_BOX(v), row->bar, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(row->row), v);
    gtk_container_add(GTK_CONTAINER(ui.jobs_list), row->row);
    gtk_widget_show_all(row->row);
    return row;
}

/* Statuszeile eines Auftrags */
static char *job_status_text(const MdJobView *j)
{
    char *have = md_format_size((double)j->have), *total = md_format_size((double)j->total);
    char *speed = md_format_size(j->speed);
    char *rest = j->remaining >= 0 ? md_format_time(j->remaining) : g_strdup("?");
    char *text = NULL;
    switch (j->state) {
    case MD_JOB_WAITING:
        if (j->total > 0 && j->paused)
            text = g_strdup_printf("Pausiert · %s von %s", have, total);
        else text = g_strdup(j->paused ? "Pausiert" : "Wartet");
        break;
    case MD_JOB_DOWNLOADING:
        if (j->paused) text = g_strdup("Wird pausiert …");
        else if (j->total > 0)
            text = g_strdup_printf("%s von %s · %s/s · noch %s", have, total, speed, rest);
        else text = g_strdup("Verbindet …");
        break;
    case MD_JOB_CONVERT_WAITING:
        text = g_strdup(j->paused ? "Download fertig · Weiterverarbeitung pausiert"
                                  : "Download fertig · wartet auf die Weiterverarbeitung");
        break;
    case MD_JOB_CONVERTING:
        text = g_strdup_printf("%s · %.0f %% · noch %s%s",
                               j->merging ? "Spuren zusammenführen" : "Umwandlung nach AV1",
                               j->progress * 100, rest, j->paused ? " · pausiert" : "");
        break;
    case MD_JOB_DONE:
        text = g_strdup(j->message ? j->message : "Fertig");
        break;
    case MD_JOB_FAILED:
        text = g_strdup(j->message ? j->message : "Fehlgeschlagen");
        break;
    }
    g_free(have);
    g_free(total);
    g_free(speed);
    g_free(rest);
    return text;
}

/* Zeile auf den aktuellen Stand bringen; Unverändertes bleibt unangetastet */
static void job_row_update(JobRow *row, const MdJobView *j)
{
    char *status = job_status_text(j);
    bool changed = !row->shown || row->state != j->state || row->paused != j->paused;
    if (!changed && !g_strcmp0(status, row->status)) {
        g_free(status);
        return;
    }
    set_label_text(row->subtitle, status);
    g_free(row->status);
    row->status = status;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(row->bar), CLAMP(j->progress, 0, 1));
    if (!changed) return;
    row->shown = true;
    row->state = j->state;
    row->paused = j->paused;

    bool done = j->state == MD_JOB_DONE, failed = j->state == MD_JOB_FAILED;
    bool open = !done && !failed;
    g_object_set_data(G_OBJECT(row->row), "open", GINT_TO_POINTER(open));
    gtk_widget_set_visible(row->bar, open);
    gtk_widget_set_visible(row->pause, open);
    gtk_widget_set_visible(row->retry, failed);
    gtk_widget_set_visible(row->open, done);
    gtk_widget_set_visible(row->remove, true);
    gtk_widget_set_tooltip_text(row->remove, open ? "Download abbrechen" : "Aus der Liste entfernen");
    gtk_widget_set_tooltip_text(row->pause, j->paused ? "Fortsetzen" : "Pausieren");
    gtk_button_set_image(GTK_BUTTON(row->pause),
                         gtk_image_new_from_icon_name(j->paused ? "media-playback-start-symbolic"
                                                                : "media-playback-pause-symbolic",
                                                      GTK_ICON_SIZE_BUTTON));
    g_object_set_data(G_OBJECT(row->pause), "paused", j->paused ? row : NULL);
}

/* Kopfzeile: was läuft gerade? */
static void on_hold_changed(GObject *w, GParamSpec *spec, gpointer d);
static void on_run_all_toggled(GtkToggleButton *b, gpointer d);

/* Schalter "Downloads/Umwandlung anhalten" zum Stand bringen ("Alle fortsetzen" hebt beides auf) */
static void sync_hold_switches(bool downloads, bool conversions)
{
    ui.cfg.hold_downloads = downloads;
    ui.cfg.hold_conversions = conversions;
    GtkWidget *switches[] = {ui.hold_downloads, ui.hold_conversions};
    bool states[] = {downloads, conversions};
    for (int i = 0; i < 2; i++) {
        g_signal_handlers_block_by_func(switches[i], on_hold_changed, NULL);
        gtk_switch_set_active(GTK_SWITCH(switches[i]), states[i]);
        g_signal_handlers_unblock_by_func(switches[i], on_hold_changed, NULL);
    }
}

/* Knopf zum Stand: gedrückt und farbig, solange etwas läuft oder laufen darf */
static void update_run_button(int open, int paused, bool held)
{
    bool running = open > paused && !held;
    GtkToggleButton *b = GTK_TOGGLE_BUTTON(ui.run_all);
    g_signal_handlers_block_by_func(b, on_run_all_toggled, NULL);
    gtk_toggle_button_set_active(b, running);
    g_signal_handlers_unblock_by_func(b, on_run_all_toggled, NULL);
    GtkStyleContext *style = gtk_widget_get_style_context(ui.run_all);
    if (running) gtk_style_context_add_class(style, "suggested-action");
    else gtk_style_context_remove_class(style, "suggested-action");
    gtk_button_set_label(GTK_BUTTON(ui.run_all), running ? "Läuft" : "Alle fortsetzen");
    gtk_widget_set_tooltip_text(ui.run_all, running ? "Klicken pausiert alle Aufträge"
                                : open ? "Setzt alle pausierten Aufträge fort"
                                : "Keine offenen Aufträge");
    gtk_widget_set_sensitive(ui.run_all, open > 0);
}

static void update_downloads_hero(const MdJobView *jobs, int count)
{
    bool hold_downloads = false, hold_conversions = false;
    md_queue_holds(&hold_downloads, &hold_conversions);
    if (hold_downloads != ui.cfg.hold_downloads || hold_conversions != ui.cfg.hold_conversions) {
        sync_hold_switches(hold_downloads, hold_conversions);
        settings_save();
    }
    int active = 0, open = 0, paused = 0, failed = 0, to_convert = 0, done = ui.jobs_finished;
    double speed = 0;
    for (int i = 0; i < count; i++) {
        const MdJobView *j = &jobs[i];
        done += j->state == MD_JOB_DONE;
        failed += j->state == MD_JOB_FAILED;
        to_convert += j->state == MD_JOB_CONVERT_WAITING;
        if (j->state == MD_JOB_DONE || j->state == MD_JOB_FAILED) continue;
        open++;
        if (j->paused) paused++;
        else if (j->state == MD_JOB_DOWNLOADING ||
                 (j->state == MD_JOB_CONVERTING && !hold_conversions)) active++;
        if (j->state == MD_JOB_DOWNLOADING) speed += j->speed;
    }
    update_run_button(open, paused, hold_downloads && hold_conversions);
    char *title;
    if (active)
        title = g_strdup_printf("%d %s aktiv", active, plural(active, "Auftrag", "Aufträge"));
    else if (paused) title = g_strdup_printf("%d pausiert", paused);
    else if (open) title = g_strdup_printf("%d %s", open, plural(open, "wartet", "warten"));
    else title = g_strdup(count || done ? "Alles erledigt" : "Keine Downloads");
    set_label_text(ui.hero, title);
    g_free(title);

    GString *sub = g_string_new(NULL);
    /* keine Restzeit: sie galt nur für den längsten laufenden Download, nicht für die offenen */
    if (speed > 0) {
        char *text = md_format_size(speed);
        g_string_append_printf(sub, "%s/s · ", text);
        g_free(text);
    }
    g_string_append_printf(sub, "%d offen · %d fertig", open, done);
    if (to_convert) g_string_append_printf(sub, " · %d %s auf Umwandlung", to_convert,
                                           plural(to_convert, "wartet", "warten"));
    if (failed) g_string_append_printf(sub, " · %d fehlgeschlagen", failed);
    if (hold_downloads) g_string_append(sub, " · Downloads angehalten");
    if (hold_conversions) g_string_append(sub, " · Umwandlung angehalten");
    set_label_text(ui.hero_sub, sub->str);
    g_string_free(sub, TRUE);
    gtk_widget_set_sensitive(ui.clear_failed, failed > 0);

    /* im Reitertitel: läuft gerade etwas oder steht alles? */
    const char *state = active ? "läuft"
                        : paused == open || hold_downloads || hold_conversions ? "angehalten"
                                                                                : "wartet";
    char *page = open ? g_strdup_printf("Warteschlange (%d) · %s", open, state)
                      : g_strdup("Warteschlange"), *old;
    gtk_container_child_get(GTK_CONTAINER(ui.stack), ui.downloads_page, "title", &old, NULL);
    if (g_strcmp0(old, page))
        gtk_container_child_set(GTK_CONTAINER(ui.stack), ui.downloads_page, "title", page, NULL);
    g_free(old);
    g_free(page);

    /* Standby sperren, solange gearbeitet wird */
    if (active && !ui.inhibit_cookie)
        ui.inhibit_cookie = gtk_application_inhibit(ui.app, GTK_WINDOW(ui.win),
                                                    GTK_APPLICATION_INHIBIT_SUSPEND,
                                                    "Mediathek Downloader arbeitet");
    if (!active && ui.inhibit_cookie) {
        gtk_application_uninhibit(ui.app, ui.inhibit_cookie);
        ui.inhibit_cookie = 0;
    }
}

/* Entfernte Aufträge aus der Liste nehmen */
static gboolean job_row_stale(gpointer key, gpointer value, gpointer data)
{
    (void)key;
    (void)data;
    JobRow *row = value;
    if (row->seen) {
        row->seen = false;
        return FALSE;
    }
    gtk_widget_destroy(row->row);
    return TRUE;
}

/* Reihenfolge der Liste: was läuft, dann Fehler, dann Wartendes, Fertiges zuletzt */
static int job_rank(const MdJobView *j)
{
    switch (j->state) {
    case MD_JOB_DOWNLOADING:
    case MD_JOB_CONVERTING: return 0;
    case MD_JOB_FAILED: return 1;
    case MD_JOB_DONE: return 3;
    default: return 2;
    }
}

typedef struct {
    int rank, index;
} JobSlot;

static int job_slot_compare(const void *a, const void *b)
{
    const JobSlot *x = a, *y = b;
    return x->rank != y->rank ? x->rank - y->rank : x->index - y->index;
}

static int job_row_compare(GtkListBoxRow *a, GtkListBoxRow *b, gpointer d)
{
    (void)d;
    const JobRow *x = g_object_get_data(G_OBJECT(a), "job");
    const JobRow *y = g_object_get_data(G_OBJECT(b), "job");
    return x->order - y->order;
}

/* Stand eines Auftrags beim letzten Timer-Schlag */
typedef struct {
    int state;                  /* offen = 0, sonst MD_JOB_DONE oder MD_JOB_FAILED */
    char *key;
} JobMark;

static void job_mark_free(gpointer data)
{
    JobMark *mark = data;
    g_free(mark->key);
    g_free(mark);
}

/* Schlüssel der Aufträge, die seit dem letzten Mal dazukamen, fertig wurden, fehlschlugen oder
 * verschwanden; nur deren Titel müssen neu geprüft werden */
static GHashTable *changed_job_keys(const MdJobView *jobs, int count)
{
    GHashTable *marks = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, job_mark_free);
    GHashTable *keys = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (int i = 0; i < count; i++) {
        gpointer id = GUINT_TO_POINTER(jobs[i].id);
        JobMark *mark = g_new(JobMark, 1);
        mark->state = jobs[i].state >= MD_JOB_DONE ? (int)jobs[i].state : 0;
        mark->key = g_strdup(jobs[i].key);
        const JobMark *old = g_hash_table_lookup(ui.job_marks, id);
        if (!old || old->state != mark->state) g_hash_table_add(keys, g_strdup(mark->key));
        g_hash_table_remove(ui.job_marks, id);
        g_hash_table_insert(marks, id, mark);
    }
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, ui.job_marks);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        g_hash_table_add(keys, g_strdup(((JobMark *)value)->key));
    g_hash_table_destroy(ui.job_marks);
    ui.job_marks = marks;
    return keys;
}

/* Timer: Warteschlange anzeigen; bei fertigen oder neuen Aufträgen auch den Stand der Titel.
 * Die Liste zeigt nur die ersten jobs_limit Aufträge: Tausende Zeilen machen GTK zäh. */
static gboolean on_tick(gpointer d)
{
    /* Der Takt (d gesetzt) kopiert sonst zweimal je Sekunde die ganze Warteschlange, auch wenn
     * nichts läuft. Ohne Änderung und ohne arbeitenden Auftrag reicht alle TICK_IDLE Takte ein
     * Blick, bei verborgenem Fenster auch mit Arbeit nur jeder TICK_HIDDEN-te. */
    static int last_serial = -1, skipped;
    int serial = md_queue_serial();
    if (d && serial == last_serial) {
        bool busy = md_queue_busy();
        bool shown = gtk_widget_get_mapped(ui.win) &&
                     !(gdk_window_get_state(gtk_widget_get_window(ui.win)) &
                       GDK_WINDOW_STATE_ICONIFIED);
        if (++skipped < (busy ? (shown ? 1 : TICK_HIDDEN) : TICK_IDLE)) {
            update_genres();
            return G_SOURCE_CONTINUE;
        }
    }
    last_serial = serial;
    skipped = 0;
    /* Fertiges verschwindet von selbst aus der Liste; gezählt wird es für den Kopf weiter */
    int finished = md_queue_remove_finished();
    ui.jobs_finished += finished;
    if (finished) last_serial = md_queue_serial();
    int count = 0;
    MdJobView *jobs = md_queue_snapshot(&count);
    bool groups_changed = update_job_groups(jobs, count);
    notify_queue_done(jobs, count);
    JobSlot *slots = g_new(JobSlot, MAX(count, 1));
    for (int i = 0; i < count; i++) slots[i] = (JobSlot){job_rank(&jobs[i]), i};
    qsort(slots, count, sizeof *slots, job_slot_compare);
    int shown = MIN(count, ui.jobs_limit);
    bool moved = false;
    for (int n = 0; n < shown; n++) {
        const MdJobView *j = &jobs[slots[n].index];
        JobRow *row = g_hash_table_lookup(ui.job_rows, GUINT_TO_POINTER(j->id));
        if (!row) {
            row = new_job_row(j, n);
            g_hash_table_insert(ui.job_rows, GUINT_TO_POINTER(j->id), row);
        }
        moved |= row->order != n;
        row->order = n;
        row->seen = true;
        job_row_update(row, j);
    }
    g_free(slots);
    g_hash_table_foreach_remove(ui.job_rows, job_row_stale, NULL);
    if (moved) gtk_list_box_invalidate_sort(GTK_LIST_BOX(ui.jobs_list));
    else if (groups_changed) gtk_list_box_invalidate_headers(GTK_LIST_BOX(ui.jobs_list));
    gtk_widget_set_visible(ui.jobs_empty, count == 0);
    gtk_widget_set_visible(ui.jobs_more, count > shown);
    if (count > shown) {
        char *more = g_strdup_printf("Weitere anzeigen (noch %d)", count - shown);
        if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(ui.jobs_more)), more))
            gtk_button_set_label(GTK_BUTTON(ui.jobs_more), more);
        g_free(more);
    }

    update_downloads_hero(jobs, count);
    bool first = !ui.job_marks;
    if (first)
        ui.job_marks = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, job_mark_free);
    GHashTable *keys = changed_job_keys(jobs, count);
    md_queue_snapshot_free(jobs, count);
    if (first || g_hash_table_size(keys) || md_history_take_changed()) {
        refresh_statuses(first ? NULL : keys);
        rebuild_videos(false);
    }
    g_hash_table_destroy(keys);
    update_genres();
    return G_SOURCE_CONTINUE;
}

/* Gedrückt (farbig) = Aufträge laufen; ein Klick pausiert alle bzw. setzt alle fort */
static void on_run_all_toggled(GtkToggleButton *b, gpointer d)
{
    (void)d;
    bool run = gtk_toggle_button_get_active(b);
    md_queue_set_paused(!run);
    if (run) show_info_bar(GTK_MESSAGE_INFO, NULL);
    on_tick(NULL);
}

static void on_hold_changed(GObject *w, GParamSpec *spec, gpointer d)
{
    (void)w;
    (void)spec;
    (void)d;
    ui.cfg.hold_downloads = gtk_switch_get_active(GTK_SWITCH(ui.hold_downloads));
    ui.cfg.hold_conversions = gtk_switch_get_active(GTK_SWITCH(ui.hold_conversions));
    md_queue_hold(ui.cfg.hold_downloads, ui.cfg.hold_conversions);
    settings_save();
    on_tick(NULL);
}

static void on_clear_failed_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    int count = 0;
    gint64 bytes = md_queue_failed_bytes(&count);
    if (count == 0) return;
    char *title = count == 1 ? g_strdup("Einen Fehlschlag entfernen?")
                             : g_strdup_printf("%d Fehlschläge entfernen?", count);
    char *size = md_format_size(bytes);
    char *text = bytes > 0 ? g_strdup_printf("Die halbfertigen Dateien (%s) werden gelöscht.", size)
                           : g_strdup("Es liegen keine halbfertigen Dateien vor.");
    bool yes = ask_confirmation(title, text, "Behalten", "Entfernen", bytes > 0);
    g_free(title);
    g_free(size);
    g_free(text);
    if (!yes) return;
    md_queue_remove_failed();
    on_tick(NULL);
}

static void on_jobs_more_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    ui.jobs_limit += JOB_ROWS_BATCH;
    on_tick(NULL);
}

static void on_limit_changed(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
    ui.cfg.limit_on = gtk_switch_get_active(GTK_SWITCH(ui.limit_switch));
    ui.cfg.limit_mb = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui.limit_spin));
    gtk_widget_set_sensitive(ui.limit_spin, ui.cfg.limit_on);
    apply_queue_settings();
    settings_save();
}

static GtkWidget *build_downloads_page(void)
{
    GtkWidget *scrolled, *page = new_page_column(&scrolled);
    ui.hero = new_label("", "zd-hero");
    ui.hero_sub = new_label("", "zd-lead");
    add_css_class(ui.hero_sub, "dim-label");
    gtk_box_pack_start(GTK_BOX(page), ui.hero, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), ui.hero_sub, FALSE, FALSE, 0);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(buttons, 10);
    ui.run_all = gtk_toggle_button_new_with_label("Alle fortsetzen");
    ui.clear_failed = gtk_button_new_with_label("Fehlgeschlagene entfernen");
    gtk_widget_set_tooltip_text(ui.clear_failed,
                                "Löscht nach Rückfrage auch die halbfertigen Dateien");
    g_signal_connect(ui.run_all, "toggled", G_CALLBACK(on_run_all_toggled), NULL);
    g_signal_connect(ui.clear_failed, "clicked", G_CALLBACK(on_clear_failed_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(buttons), ui.run_all, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), ui.clear_failed, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), buttons, FALSE, FALSE, 0);

    GtkWidget *limit = add_group(page, "Steuerung");
    ui.hold_downloads = gtk_switch_new();
    ui.hold_conversions = gtk_switch_new();
    sync_hold_switches(ui.cfg.hold_downloads, ui.cfg.hold_conversions);
    g_signal_connect(ui.hold_downloads, "notify::active", G_CALLBACK(on_hold_changed), NULL);
    g_signal_connect(ui.hold_conversions, "notify::active", G_CALLBACK(on_hold_changed), NULL);
    add_row(limit, "Downloads anhalten",
            "Umwandlungen laufen weiter. Ein Download setzt später an derselben Stelle fort.", NULL,
            ui.hold_downloads, NULL);
    add_row(limit, "Umwandlung anhalten",
            "AV1 und das Zusammenführen der Spuren stehen still, Downloads laufen weiter.", NULL,
            ui.hold_conversions, NULL);
    ui.limit_switch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(ui.limit_switch), ui.cfg.limit_on);
    ui.limit_spin = new_spin(1, 1000, 1, ui.cfg.limit_mb);
    gtk_widget_set_sensitive(ui.limit_spin, ui.cfg.limit_on);
    GtkWidget *suffix = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(suffix), ui.limit_spin, FALSE, FALSE, 0);
    GtkWidget *unit = new_label("MB/s", NULL);
    gtk_box_pack_start(GTK_BOX(suffix), unit, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(suffix), ui.limit_switch, FALSE, FALSE, 8);
    gtk_widget_set_valign(ui.limit_switch, GTK_ALIGN_CENTER);
    add_row(limit, "Geschwindigkeit begrenzen", "Gilt für alle Downloads zusammen und sofort.",
            NULL, suffix, NULL);
    g_signal_connect(ui.limit_switch, "notify::active", G_CALLBACK(on_limit_changed), NULL);
    g_signal_connect(ui.limit_spin, "value-changed", G_CALLBACK(on_limit_changed), NULL);

    ui.jobs_list = add_group(page, "Aufträge");
    gtk_list_box_set_sort_func(GTK_LIST_BOX(ui.jobs_list), job_row_compare, NULL, NULL);
    gtk_list_box_set_header_func(GTK_LIST_BOX(ui.jobs_list), job_row_header, NULL, NULL);
    ui.jobs_limit = JOB_ROWS_BATCH;
    ui.jobs_more = gtk_button_new_with_label("Weitere anzeigen");
    gtk_widget_set_halign(ui.jobs_more, GTK_ALIGN_CENTER);
    gtk_widget_set_no_show_all(ui.jobs_more, TRUE);
    g_signal_connect(ui.jobs_more, "clicked", G_CALLBACK(on_jobs_more_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(page), ui.jobs_more, FALSE, FALSE, 6);
    ui.jobs_empty = new_label("Noch nichts in der Warteschlange. In der Vorschau auswählen "
                              "und „Herunterladen“ klicken.", "dim-label");
    gtk_widget_set_no_show_all(ui.jobs_empty, TRUE);
    set_wrapping(ui.jobs_empty, 70);
    gtk_widget_set_margin_top(ui.jobs_empty, 12);
    gtk_box_pack_start(GTK_BOX(page), ui.jobs_empty, FALSE, FALSE, 0);
    ui.job_rows = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, job_row_free);
    return scrolled;
}

/* ---------------------------------------------------------------------------
 * Seite Meine Videos
 *
 * Alles Geladene als Kacheln wie in einer Mediathek: oben "Weiterschauen" und "Zuletzt
 * geladen", darunter Serien und Filme von A bis Z. Eine Serie öffnet sich mit ihren Staffeln
 * und Folgen. Wie weit jede Datei geschaut ist, steht in ~/.mediathek-downloader/wiedergabe.tsv.
 * ------------------------------------------------------------------------- */
/* bis zum Abspann geschaut */
static bool watch_done(const WatchState *w)
{
    return w && w->duration > 0 && w->position >= w->duration * WATCHED_SHARE;
}

/* angefangen, aber nicht zu Ende */
static bool watch_started(const WatchState *w)
{
    return w && w->position >= RESUME_MIN_SECONDS && !watch_done(w);
}

/* gesehener Anteil in Prozent, für angefangene Dateien 1 bis 99 */
static int watch_percent(const WatchState *w)
{
    if (watch_done(w)) return 100;
    if (!watch_started(w) || w->duration <= 0) return 0;
    return CLAMP((int)(w->position * 100 / w->duration), 1, 99);
}

static char *watch_path(void)
{
    return g_build_filename(md_app_dir(), WATCH_FILE, NULL);
}

static void watch_load(void)
{
    ui.watch = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    char *path = watch_path(), *text = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL)) {
        char **lines = g_strsplit(text, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char **f = g_strsplit(lines[i], "\t", -1);
            if (g_strv_length(f) == 4) {
                WatchState *w = g_new0(WatchState, 1);
                w->position = g_ascii_strtod(f[1], NULL);
                w->duration = g_ascii_strtod(f[2], NULL);
                w->updated = g_ascii_strtoll(f[3], NULL, 10);
                g_hash_table_replace(ui.watch, g_strcompress(f[0]), w);
            }
            g_strfreev(f);
        }
        g_strfreev(lines);
    }
    g_free(path);
    g_free(text);
}

static void watch_save(void)
{
    GString *out = g_string_new(NULL);
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, ui.watch);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const WatchState *w = value;
        char *escaped = g_strescape(key, NULL);
        char position[G_ASCII_DTOSTR_BUF_SIZE], duration[G_ASCII_DTOSTR_BUF_SIZE];
        g_string_append_printf(out, "%s\t%s\t%s\t%" G_GINT64_FORMAT "\n", escaped,
                               g_ascii_formatd(position, sizeof position, "%.1f", w->position),
                               g_ascii_formatd(duration, sizeof duration, "%.1f", w->duration),
                               w->updated);
        g_free(escaped);
    }
    char *path = watch_path();
    g_file_set_contents(path, out->str, out->len, NULL);
    g_free(path);
    g_string_free(out, TRUE);
}

/* Stand setzen; position < 0 vergisst ihn */
static void watch_set(const char *path, double position, double duration)
{
    if (position < 0) {
        g_hash_table_remove(ui.watch, path);
        return;
    }
    WatchState *w = g_hash_table_lookup(ui.watch, path);
    if (!w) {
        w = g_new0(WatchState, 1);
        g_hash_table_insert(ui.watch, g_strdup(path), w);
    }
    w->position = position;
    if (duration > 0) w->duration = duration;
    w->updated = g_get_real_time() / G_USEC_PER_SEC;
}

/* Stand von path; nach der Umwandlung einer MP4- in eine MKV-Datei zieht er zum neuen Namen mit */
static const WatchState *watch_get(const char *path)
{
    WatchState *w = g_hash_table_lookup(ui.watch, path);
    if (w || !g_str_has_suffix(path, ".mkv")) return w;
    char *stem = g_strndup(path, strlen(path) - strlen(".mkv"));
    char *mp4 = g_strconcat(stem, ".mp4", NULL);
    gpointer old_key = NULL;
    if (g_hash_table_steal_extended(ui.watch, mp4, &old_key, (gpointer *)&w)) {
        g_free(old_key);
        g_hash_table_insert(ui.watch, g_strdup(path), w);
    }
    g_free(stem);
    g_free(mp4);
    return w;
}

/* Bezeichnung einer Folge ohne den Namen der Sendung und "S01E03" */
static char *episode_label(const MdHistoryEntry *e)
{
    const char *text = e->title;
    size_t topic_length = strlen(e->topic);
    if (!e->is_film && !strncmp(text, e->topic, topic_length) &&
        g_str_has_prefix(text + topic_length, " – "))
        text += topic_length + strlen(" – ");
    /* den Gedankenstrich selbst prüfen: sscanf vergleicht ihn unter UTF-8 nicht */
    int season, episode, used = 0;
    if (sscanf(text, "S%dE%d%n", &season, &episode, &used) == 2 &&
        g_str_has_prefix(text + used, " – "))
        text += used + strlen(" – ");
    char *label = g_strdup(text), *tail = g_strconcat(" – ", e->topic, NULL);
    if (!e->is_film && g_str_has_suffix(label, tail) && strlen(label) > strlen(tail))
        label[strlen(label) - strlen(tail)] = 0;
    g_free(tail);
    return label;
}

/* Titel für den Player: "Sendung – S01E03 – Folge" */
static char *player_title(const MdHistoryEntry *e)
{
    if (e->is_film) return g_strdup(e->title);
    char *label = episode_label(e), *title;
    if (e->season >= 0)
        title = g_strdup_printf("%s – S%02dE%02d – %s", e->topic, e->season, e->episode, label);
    else title = g_strdup_printf("%s – %s", e->topic, label);
    g_free(label);
    return title;
}

static void show_free(gpointer data)
{
    Show *show = data;
    g_free(show->name);
    g_ptr_array_free(show->entries, TRUE);
    g_free(show);
}

static Show *find_show(const char *name)
{
    for (guint i = 0; ui.shows && i < ui.shows->len; i++) {
        Show *show = ui.shows->pdata[i];
        if (!show->is_film && !strcmp(show->name, name)) return show;
    }
    return NULL;
}

/* Eintrag des Verlaufs zu einer Datei */
static const MdHistoryEntry *find_video(const char *path, Show **show_out)
{
    for (guint i = 0; ui.shows && i < ui.shows->len; i++) {
        Show *show = ui.shows->pdata[i];
        for (guint n = 0; n < show->entries->len; n++) {
            const MdHistoryEntry *e = show->entries->pdata[n];
            if (!strcmp(e->path, path)) {
                if (show_out) *show_out = show;
                return e;
            }
        }
    }
    return NULL;
}

/* Folge, mit der es in einer Serie weitergeht: zuletzt angefangene, sonst die erste ungesehene */
static const MdHistoryEntry *show_resume_entry(const Show *show, bool *resume)
{
    const MdHistoryEntry *latest = NULL, *unseen = NULL;
    gint64 latest_time = 0;
    for (guint i = 0; i < show->entries->len; i++) {
        const MdHistoryEntry *e = show->entries->pdata[i];
        const WatchState *w = watch_get(e->path);
        if (watch_started(w) && w->updated > latest_time) {
            latest = e;
            latest_time = w->updated;
        }
        if (!unseen && !watch_done(w)) unseen = e;
    }
    *resume = latest != NULL;
    return latest ? latest : unseen ? unseen : show->entries->pdata[0];
}

/* --- Bilder ---------------------------------------------------------------------------- */
static void frame_job_free(FrameJob *job)
{
    g_free(job->key);
    g_free(job->video);
    g_free(job->website);
    g_clear_object(&job->pixbuf);
    g_free(job);
}

static gboolean on_frame_done(gpointer data)
{
    FrameJob *job = data;
    GPtrArray *images = NULL;
    g_hash_table_steal_extended(ui.frame_waiting, job->key, NULL, (gpointer *)&images);
    if (job->pixbuf) g_hash_table_replace(ui.frames, g_strdup(job->key), g_object_ref(job->pixbuf));
    for (guint i = 0; images && job->pixbuf && i < images->len; i++)
        if (gtk_widget_get_parent(images->pdata[i]))      /* Kachel noch da */
            gtk_image_set_from_pixbuf(GTK_IMAGE(images->pdata[i]), job->pixbuf);
    if (images) g_ptr_array_free(images, TRUE);
    frame_job_free(job);
    return G_SOURCE_REMOVE;
}

static void frame_worker(gpointer data, gpointer d)
{
    (void)d;
    FrameJob *job = data;
    char *path = job->website ? md_cover_file(job->website, NULL) : NULL;
    if (!path) path = md_video_frame(job->video);
    if (path)
        job->pixbuf = gdk_pixbuf_new_from_file_at_scale(path, COVER_WIDTH, COVER_HEIGHT, FALSE,
                                                        NULL);
    g_free(path);
    g_idle_add(on_frame_done, job);
}

/* Bild für image besorgen: Titelbild der Mediathek (website) oder Standbild aus video */
static void request_frame(GtkWidget *image, const char *key, const char *video,
                          const char *website)
{
    GdkPixbuf *pixbuf = g_hash_table_lookup(ui.frames, key);
    CachedCover *cached = website && ui.covers ? g_hash_table_lookup(ui.covers, website) : NULL;
    if (!pixbuf && cached) pixbuf = cached->pixbuf;     /* schon aus der Vorschau bekannt */
    if (pixbuf) {
        gtk_image_set_from_pixbuf(GTK_IMAGE(image), pixbuf);
        return;
    }
    GPtrArray *images = g_hash_table_lookup(ui.frame_waiting, key);
    bool queued = images != NULL;
    if (!images) {
        images = g_ptr_array_new_with_free_func(g_object_unref);
        g_hash_table_insert(ui.frame_waiting, g_strdup(key), images);
    }
    g_ptr_array_add(images, g_object_ref(image));
    if (queued) return;
    FrameJob *job = g_new0(FrameJob, 1);
    job->key = g_strdup(key);
    job->video = g_strdup(video);
    job->website = g_strdup(website);
    g_thread_pool_push(ui.frame_pool, job, NULL);
}

/* Gruppe der Vorschau zu einer Serie oder einem Film; NULL = nicht in der Startliste */
static const Group *preview_group(const Show *show)
{
    if (!ui.preview_index) return NULL;
    char *lower = g_utf8_strdown(show->name, -1);
    char *key = g_strdup_printf("%c:%s", show->is_film ? 'F' : 'S', lower);
    const Group *group = g_hash_table_lookup(ui.preview_index, key);
    g_free(lower);
    g_free(key);
    return group;
}

/* --- Abspielen ------------------------------------------------------------------------- */
static gboolean on_videos_idle_rebuild(gpointer d)
{
    (void)d;
    ui.videos_rebuild_source = 0;
    rebuild_videos(true);
    return G_SOURCE_REMOVE;
}

/* Wofür die Sehzeit zählt: Serie oder Film; ohne Eintrag im Verlauf der Dateiname */
static char *stats_name(const char *path, bool *film)
{
    Show *show = NULL;
    const MdHistoryEntry *e = find_video(path, &show);
    *film = e ? show->is_film : true;
    return e ? g_strdup(show->name) : g_path_get_basename(path);
}

/* Sehzeit buchen: nur, was seit der letzten Meldung wirklich lief, keine Sprünge */
static void stats_progress(const char *path, double position, double duration, bool left)
{
    static char *last_path;
    static double last_position;
    static gint64 last_time;
    gint64 now = g_get_monotonic_time();
    double seconds = 0;
    if (!g_strcmp0(path, last_path)) {
        double elapsed = (double)(now - last_time) / G_USEC_PER_SEC;
        seconds = CLAMP(position - last_position, 0, elapsed + 1);
    }
    bool was_done = watch_done(watch_get(path));
    bool done = duration > 0 && position >= duration * WATCHED_SHARE;
    if (seconds > 0 || (done && !was_done)) {
        bool film = false;
        char *name = stats_name(path, &film);
        md_stats_add(name, film, g_get_real_time() / G_USEC_PER_SEC, seconds, 0,
                     done && !was_done);
        g_free(name);
    }
    g_free(last_path);
    last_path = left ? NULL : g_strdup(path);
    last_position = position;
    last_time = now;
}

static void on_player_progress(const char *path, double position, double duration, bool left,
                               gpointer data)
{
    (void)data;
    if (duration > 0 && duration - position < 1) position = duration;
    stats_progress(path, position, duration, left);
    watch_set(path, position, duration);
    watch_save();
    md_stats_save();
    if (left && !ui.videos_rebuild_source && !ui.closed)
        ui.videos_rebuild_source = g_idle_add(on_videos_idle_rebuild, NULL);
}

static char *on_player_next(const char *path, char **title, gpointer data)
{
    (void)data;
    Show *show = NULL;
    const MdHistoryEntry *e = find_video(path, &show);
    if (!e || show->is_film) return NULL;
    for (guint i = 0; i + 1 < show->entries->len; i++)
        if (show->entries->pdata[i] == e) {
            const MdHistoryEntry *next = show->entries->pdata[i + 1];
            *title = player_title(next);
            return g_strdup(next->path);
        }
    return NULL;
}

/* Player zu: zurück zur Seite, von der aus er geöffnet wurde */
static void on_player_closed(gpointer data)
{
    (void)data;
    GtkWidget *back = ui.player_return ? ui.player_return : ui.videos_page;
    if (!g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(ui.stack)), "player"))
        gtk_stack_set_visible_child(GTK_STACK(ui.stack), back);
}

/* Datei im eingebauten Player; geht das nicht, im Standard-Programm */
static void play_file(const char *path, const char *title, double start)
{
    MdPlayerHooks hooks = {on_player_progress, on_player_next, on_player_closed, NULL};
    GtkWidget *current = gtk_stack_get_visible_child(GTK_STACK(ui.stack));
    if (g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(ui.stack)), "player"))
        ui.player_return = current;
    if (md_player_play(GTK_STACK(ui.stack), path, title, start, &hooks)) {
        bool film = false;
        char *name = stats_name(path, &film);
        md_stats_add(name, film, g_get_real_time() / G_USEC_PER_SEC, 0, 1, 0);
        md_stats_save();
        g_free(name);
        return;
    }
    char *why = NULL, *text;
    md_player_available(&why);
    text = g_strdup_printf("Der eingebaute Player geht nicht (%s). Die Datei läuft im "
                           "Standard-Programm.", why ? why : "GStreamer fehlt");
    show_info_bar(GTK_MESSAGE_WARNING, text);
    g_free(why);
    g_free(text);
    open_in_default_app(path);
}

/* Datei abspielen; wo sie aufgehört hat, geht es weiter */
static void play_path(const char *path)
{
    const MdHistoryEntry *e = find_video(path, NULL);
    const WatchState *w = watch_get(path);
    double start = watch_started(w) ? w->position : 0;
    char *title = e ? player_title(e) : g_path_get_basename(path);
    play_file(path, title, start);
    g_free(title);
}

/* --- Kacheln --------------------------------------------------------------------------- */
static void on_mark_watched(GtkMenuItem *item, gpointer d)
{
    (void)d;
    bool watched = g_object_get_data(G_OBJECT(item), "watched") != NULL;
    for (char **path = g_object_get_data(G_OBJECT(item), "paths"); *path; path++) {
        const WatchState *w = watch_get(*path);
        double end = w && w->duration > 0 ? w->duration : 1;
        watch_set(*path, watched ? end : -1, end);
    }
    watch_save();
    rebuild_videos(true);
}

static void on_menu_open_folder(GtkMenuItem *item, gpointer d)
{
    (void)d;
    open_folder_of(g_object_get_data(G_OBJECT(item), "path"));
}

static void on_menu_open_external(GtkMenuItem *item, gpointer d)
{
    (void)d;
    open_in_default_app(g_object_get_data(G_OBJECT(item), "path"));
}

static GtkWidget *add_menu_item(GtkWidget *menu, const char *label, GCallback callback,
                                const char *path)
{
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    if (path) g_object_set_data_full(G_OBJECT(item), "path", g_strdup(path), g_free);
    g_signal_connect(item, "activate", callback, NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

/* Leere Staffel- und Serienordner wegräumen, höchstens bis zum Speicherort */
static void remove_empty_folders(const char *path)
{
    char *dir = g_path_get_dirname(path);
    for (int depth = 0; depth < 2 && g_strcmp0(dir, ui.cfg.dir) &&
                        g_str_has_prefix(dir, ui.cfg.dir) && g_rmdir(dir) == 0; depth++) {
        char *parent = g_path_get_dirname(dir);
        g_free(dir);
        dir = parent;
    }
    g_free(dir);
}

/* Dateien nach Rückfrage in den Papierkorb; der Verlauf bleibt, damit die Suche nach neuen
 * Folgen sie nicht wieder als neu meldet. Bei einer ganzen Serie (topic gesetzt) verschwinden
 * auch ihre geplanten Downloads und Umwandlungen aus der Warteschlange. */
static void on_menu_trash(GtkMenuItem *item, gpointer d)
{
    (void)d;
    char **paths = g_object_get_data(G_OBJECT(item), "paths");
    const char *title = g_object_get_data(G_OBJECT(item), "title");
    const char *topic = g_object_get_data(G_OBJECT(item), "topic");
    const char *playing = md_player_current();
    int jobs = topic ? md_queue_topic_jobs(topic) : 0;
    int count = 0;
    gint64 bytes = 0;
    for (char **path = paths; *path; path++) {
        /* Aufträge der Serie werden gleich abgebrochen, ihre Dateien sind dann frei */
        if (!g_strcmp0(*path, playing) || (!jobs && md_file_in_use(*path))) {
            show_info_bar(GTK_MESSAGE_WARNING, "Eine der Dateien läuft gerade im Player oder "
                          "wird noch bearbeitet. Bitte erst schließen bzw. warten.");
            return;
        }
        GStatBuf st;
        if (g_stat(*path, &st) == 0) bytes += st.st_size;
        count++;
    }
    char *size = md_format_size((double)bytes);
    char *question = count == 1 ? g_strdup_printf("„%s“ löschen?", title)
                                : g_strdup_printf("%d Folgen von „%s“ löschen?", count, title);
    char *queued = jobs ? g_strdup_printf(" %d %s der Serie in der Warteschlange %s "
                                          "abgebrochen und entfernt.", jobs,
                                          plural(jobs, "Auftrag", "Aufträge"),
                                          jobs == 1 ? "wird" : "werden")
                        : g_strdup("");
    char *text = g_strdup_printf("%s werden in den Papierkorb verschoben und lassen sich dort "
                                 "wiederherstellen.%s", size, queued);
    g_free(queued);
    bool yes = ask_confirmation(question, text, "Abbrechen", "In den Papierkorb", true);
    g_free(size);
    g_free(question);
    g_free(text);
    if (!yes) return;
    if (topic) {
        md_queue_remove_topic(topic);
        /* laufende Umwandlungen geben ihre Dateien erst frei, wenn ffmpeg beendet ist */
        for (int wait = 0; wait < 100; wait++) {
            bool busy = false;
            for (char **path = paths; *path && !busy; path++) busy = md_file_in_use(*path);
            if (!busy) break;
            g_usleep(100 * 1000);
        }
        on_tick(NULL);
    }
    int failed = 0;
    char *reason = NULL;
    for (char **path = paths; *path; path++) {
        GFile *file = g_file_new_for_path(*path);
        GError *error = NULL;
        if (g_file_trash(file, NULL, &error) ||
            g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) {
            watch_set(*path, -1, 0);
            remove_empty_folders(*path);
        } else {
            failed++;
            if (!reason) reason = g_strdup(error->message);
        }
        g_clear_error(&error);
        g_object_unref(file);
    }
    watch_save();
    if (failed) {
        char *message = g_strdup_printf("%d %s nicht gelöscht: %s", failed,
                                        plural(failed, "Datei", "Dateien"), reason);
        show_info_bar(GTK_MESSAGE_ERROR, message);
        g_free(message);
    }
    g_free(reason);
    rebuild_videos(true);
}

/* Geladene Dateien nachträglich nach AV1 umwandeln, mit Encoder und Qualität aus den
 * Einstellungen */
static void on_menu_convert(GtkMenuItem *item, gpointer d)
{
    (void)d;
    char **paths = g_object_get_data(G_OBJECT(item), "paths");
    MdJobOptions options = {.dir = ui.cfg.dir, .quality = (MdQuality)ui.cfg.quality,
                            .convert = true, .encoder = (MdEncoder)ui.cfg.encoder,
                            .gpu_quality = ui.cfg.gpu_quality, .cpu_rf = ui.cfg.cpu_rf,
                            .keep_original = ui.cfg.keep_original};
    int added = 0, skipped = 0;
    for (char **path = paths; *path; path++) {
        if (md_queue_convert(*path, &options)) added++;
        else skipped++;
    }
    if (skipped) {
        char *text = g_strdup_printf("%d %s schon in Arbeit und %s übersprungen.", skipped,
                                     plural(skipped, "Datei ist", "Dateien sind"),
                                     plural(skipped, "wurde", "wurden"));
        show_info_bar(GTK_MESSAGE_INFO, text);
        g_free(text);
    }
    if (added) gtk_stack_set_visible_child(GTK_STACK(ui.stack), ui.downloads_page);
}

static gboolean destroy_widget(gpointer widget)
{
    gtk_widget_destroy(widget);
    return G_SOURCE_REMOVE;
}

/* GTK schließt das Menü, bevor es den gewählten Eintrag auslöst: erst danach abbauen */
static void on_menu_deactivate(GtkMenuShell *menu, gpointer d)
{
    (void)d;
    g_idle_add(destroy_widget, menu);
}

/* Menü der rechten Maustaste: gesehen/ungesehen, Ordner, anderes Programm (nicht in
 * "Weiterschauen") */
static gboolean on_tile_press(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    (void)d;
    if (e->type != GDK_BUTTON_PRESS || e->button != GDK_BUTTON_SECONDARY) return FALSE;
    const Show *show = g_object_get_data(G_OBJECT(w), "show");
    const MdHistoryEntry *entry = g_object_get_data(G_OBJECT(w), "entry");
    GStrvBuilder *builder = g_strv_builder_new();
    bool all_done = true;
    for (guint i = 0; i < (entry ? 1 : show->entries->len); i++) {
        const MdHistoryEntry *x = entry ? entry : show->entries->pdata[i];
        g_strv_builder_add(builder, x->path);
        all_done &= watch_done(watch_get(x->path));
    }
    char **paths = g_strv_builder_end(builder);
    g_strv_builder_unref(builder);
    bool single = entry || show->is_film;
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *mark = add_menu_item(menu, all_done ? "Als nicht gesehen markieren"
                                    : single ? "Als gesehen markieren"
                                    : "Alle Folgen als gesehen markieren",
                                    G_CALLBACK(on_mark_watched), NULL);
    g_object_set_data_full(G_OBJECT(mark), "paths", paths, (GDestroyNotify)g_strfreev);
    if (!all_done) g_object_set_data(G_OBJECT(mark), "watched", GINT_TO_POINTER(1));
    if (single) {
        if (!g_object_get_data(G_OBJECT(w), "resume"))
            add_menu_item(menu, "Mit anderem Programm öffnen",
                          G_CALLBACK(on_menu_open_external), paths[0]);
        add_menu_item(menu, "Ordner öffnen", G_CALLBACK(on_menu_open_folder), paths[0]);
    }
    GtkWidget *convert = add_menu_item(menu, single ? "Nach AV1 umwandeln"
                                       : "Alle Folgen nach AV1 umwandeln",
                                       G_CALLBACK(on_menu_convert), NULL);
    g_object_set_data_full(G_OBJECT(convert), "paths", g_strdupv(paths),
                           (GDestroyNotify)g_strfreev);
    gtk_widget_set_tooltip_text(convert, "Spart Platz. Dateien, die schon AV1 sind, bleiben, "
                                         "wie sie sind. Encoder und Qualität wie in den "
                                         "Einstellungen.");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
    GtkWidget *trash = add_menu_item(menu, single ? "In den Papierkorb verschieben"
                                     : "Alle Folgen in den Papierkorb verschieben",
                                     G_CALLBACK(on_menu_trash), NULL);
    g_object_set_data_full(G_OBJECT(trash), "paths", g_strdupv(paths),
                           (GDestroyNotify)g_strfreev);
    char *name = entry ? (entry->is_film ? g_strdup(entry->title) : episode_label(entry))
                       : g_strdup(show->name);
    g_object_set_data_full(G_OBJECT(trash), "title", name, g_free);
    if (!single) g_object_set_data_full(G_OBJECT(trash), "topic", g_strdup(show->name), g_free);
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "deactivate", G_CALLBACK(on_menu_deactivate), NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)e);
    return TRUE;
}

/* Kachel mit Bild, Fortschritt und Beschriftung; data hängt am Kind der Kachelfläche */
static GtkWidget *add_video_tile(GtkWidget *box, const char *title, const char *subtitle,
                                 GtkWidget **image_out, GtkWidget **badges_out, double watched)
{
    GtkWidget *tile = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_halign(tile, GTK_ALIGN_CENTER);
    GtkWidget *overlay = gtk_overlay_new();
    GtkWidget *image = gtk_image_new_from_icon_name("video-x-generic-symbolic",
                                                    GTK_ICON_SIZE_DIALOG);
    gtk_widget_set_size_request(image, COVER_WIDTH, COVER_HEIGHT);
    add_css_class(image, "zd-cover");
    gtk_container_add(GTK_CONTAINER(overlay), image);
    GtkWidget *hint = gtk_image_new_from_icon_name("media-playback-start-symbolic",
                                                   GTK_ICON_SIZE_DIALOG);
    add_css_class(hint, "zd-play-hint");
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), hint);
    gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(overlay), hint, TRUE);
    GtkWidget *badges = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 3);
    add_css_class(badges, "zd-tile-badges");
    gtk_widget_set_halign(badges, GTK_ALIGN_END);
    gtk_widget_set_valign(badges, GTK_ALIGN_START);
    gtk_widget_set_margin_end(badges, 6);
    gtk_widget_set_margin_top(badges, 6);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), badges);
    if (watched > 0 && watched < 1) {
        GtkWidget *bar = gtk_progress_bar_new();
        add_css_class(bar, "zd-watch");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), watched);
        gtk_widget_set_valign(bar, GTK_ALIGN_END);
        gtk_widget_set_margin_start(bar, 8);
        gtk_widget_set_margin_end(bar, 8);
        gtk_widget_set_margin_bottom(bar, 8);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), bar);
    }
    if (watched >= 1) gtk_box_pack_start(GTK_BOX(badges), new_badge("✓ Gesehen", NULL), FALSE,
                                         FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tile), overlay, FALSE, FALSE, 0);
    GtkWidget *name = new_label(title, "zd-title");
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(name), 10);
    gtk_widget_set_size_request(name, COVER_WIDTH, -1);
    gtk_widget_set_tooltip_text(name, title);
    gtk_box_pack_start(GTK_BOX(tile), name, FALSE, FALSE, 0);
    GtkWidget *sub = new_label(subtitle, "zd-sub");
    add_css_class(sub, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(sub), 10);
    gtk_widget_set_size_request(sub, COVER_WIDTH, -1);
    gtk_box_pack_start(GTK_BOX(tile), sub, FALSE, FALSE, 0);
    GtkWidget *events = gtk_event_box_new();
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(events), FALSE);
    gtk_container_add(GTK_CONTAINER(events), tile);
    g_signal_connect(events, "button-press-event", G_CALLBACK(on_tile_press), NULL);
    gtk_flow_box_insert(GTK_FLOW_BOX(box), events, -1);
    *image_out = image;
    if (badges_out) *badges_out = badges;
    return events;
}

/* Gesehener Anteil einer Datei, 0 bis 1 */
static double watch_fraction(const WatchState *w)
{
    if (watch_done(w)) return 1;
    return watch_started(w) && w->duration > 0 ? MIN(w->position / w->duration, 0.99) : 0;
}

/* Restzeit oder Stand einer Datei */
static char *watch_text(const WatchState *w, const MdHistoryEntry *e)
{
    if (watch_done(w)) return g_strdup("Gesehen");
    if (watch_started(w) && w->duration > 0) {
        char *rest = md_format_time(w->duration - w->position);
        char *text = g_strdup_printf("%d %% · noch %s", watch_percent(w), rest);
        g_free(rest);
        return text;
    }
    char *date = format_date(e->finished), *text = g_strdup_printf("Geladen am %s", date);
    g_free(date);
    return text;
}

/* Kachel einer einzelnen Datei (Folge oder Film) */
static void add_entry_tile(GtkWidget *box, const MdHistoryEntry *e, bool with_show)
{
    const WatchState *w = watch_get(e->path);
    char *label = episode_label(e), *title;
    if (with_show) title = g_strdup(e->is_film ? e->title : e->topic);
    else if (e->season >= 0 && !g_str_has_prefix(label, "Folge"))
        title = g_strdup_printf("%d. %s", e->episode, label);
    else title = g_strdup(label);
    char *state = watch_text(w, e), *subtitle;
    if (with_show && !e->is_film && e->season >= 0)
        subtitle = g_strdup_printf("S%02d E%02d · %s", e->season, e->episode, state);
    else if (with_show && !e->is_film) subtitle = g_strdup_printf("%s · %s", label, state);
    else subtitle = g_strdup(state);
    GtkWidget *image;
    GtkWidget *events = add_video_tile(box, title, subtitle, &image, NULL, watch_fraction(w));
    g_object_set_data(G_OBJECT(events), "entry", (gpointer)e);
    g_object_set_data(G_OBJECT(gtk_widget_get_parent(events)), "entry", (gpointer)e);
    /* Kachel in "Weiterschauen": dort geht es ums Weiterschauen im Player */
    if (with_show) g_object_set_data(G_OBJECT(events), "resume", GINT_TO_POINTER(1));
    request_frame(image, e->path, e->path, NULL);
    g_free(label);
    g_free(title);
    g_free(state);
    g_free(subtitle);
}

/* Ist eine Staffel komplett? Ja, wenn die Folgen von 1 bis zur höchsten geladenen oder
 * angebotenen ohne Lücke geladen sind. Was keine Mediathek mehr anbietet, kann am Ende einer
 * Staffel fehlen, ohne dass es auffällt. */
typedef enum { SEASON_UNKNOWN, SEASON_INCOMPLETE, SEASON_COMPLETE } SeasonState;

static SeasonState season_state(const Show *show, int season)
{
    if (show->is_film || season < 0 || !ui.season_offers) return SEASON_UNKNOWN;
    char *lower = g_utf8_strdown(show->name, -1);
    bool checked = g_hash_table_contains(ui.season_offers, lower);
    char *key = g_strdup_printf("%s\t%d", lower, season);
    int most = GPOINTER_TO_INT(g_hash_table_lookup(ui.season_offers, key));
    g_free(key);
    g_free(lower);
    if (!checked) return SEASON_UNKNOWN;
    for (guint i = 0; i < show->entries->len; i++) {
        const MdHistoryEntry *e = show->entries->pdata[i];
        if (e->season == season) most = MAX(most, e->episode);
    }
    if (most < 1) return SEASON_UNKNOWN;
    bool *have = g_new0(bool, most + 1);
    for (guint i = 0; i < show->entries->len; i++) {
        const MdHistoryEntry *e = show->entries->pdata[i];
        if (e->season == season && e->episode >= 1) have[e->episode] = true;
    }
    int missing = 0;
    for (int n = 1; n <= most; n++) missing += !have[n];
    g_free(have);
    return missing ? SEASON_INCOMPLETE : SEASON_COMPLETE;
}

/* Ist die Serie komplett: jede geladene und jede angebotene Staffel? */
static bool show_complete(const Show *show)
{
    if (show->is_film || !ui.season_offers) return false;
    bool any = false;
    for (guint i = 0; i < show->entries->len; i++) {
        const MdHistoryEntry *e = show->entries->pdata[i];
        if (e->season < 0) continue;
        if (season_state(show, e->season) != SEASON_COMPLETE) return false;
        any = true;
    }
    if (!any) return false;
    /* angebotene Staffeln, von denen nichts geladen ist */
    char *lower = g_utf8_strdown(show->name, -1), *prefix = g_strconcat(lower, "\t", NULL);
    bool complete = true;
    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init(&iter, ui.season_offers);
    while (complete && g_hash_table_iter_next(&iter, &key, NULL))
        if (g_str_has_prefix(key, prefix))
            complete = season_state(show, atoi((char *)key + strlen(prefix))) == SEASON_COMPLETE;
    g_free(prefix);
    g_free(lower);
    return complete;
}

/* Kachel einer Serie oder eines Films */
static void add_show_tile(GtkWidget *box, const Show *show)
{
    if (show->is_film) {
        const Group *group = preview_group(show);
        const MdHistoryEntry *e = show->entries->pdata[0];
        const WatchState *w = watch_get(e->path);
        char *state = watch_text(w, e);
        GtkWidget *image;
        GtkWidget *events = add_video_tile(box, show->name, state, &image, NULL,
                                           watch_fraction(w));
        g_object_set_data(G_OBJECT(events), "show", (gpointer)show);
        g_object_set_data(G_OBJECT(gtk_widget_get_parent(events)), "show", (gpointer)show);
        char *key = g_strdup_printf("film:%s|%d", e->path, group && group->website);
        request_frame(image, key, e->path, group ? group->website : NULL);
        g_free(key);
        g_free(state);
        return;
    }
    int count = (int)show->entries->len, seasons = 0, last_season = G_MININT, seen = 0;
    double watched = 0;
    for (guint i = 0; i < show->entries->len; i++) {
        const MdHistoryEntry *e = show->entries->pdata[i];
        if (e->season >= 0 && e->season != last_season) seasons++;
        last_season = e->season;
        const WatchState *w = watch_get(e->path);
        seen += watch_done(w);
        watched += watch_fraction(w);
    }
    char *subtitle;
    if (seen == count) subtitle = g_strdup_printf("%d %s · alle gesehen", count,
                                                  plural(count, "Folge", "Folgen"));
    else if (seen || watched > 0) subtitle = g_strdup_printf("%d von %d gesehen", seen, count);
    else if (seasons > 1) subtitle = g_strdup_printf("%d Staffeln · %d Folgen", seasons, count);
    else subtitle = g_strdup_printf("%d %s", count, plural(count, "Folge", "Folgen"));
    GtkWidget *image, *badges;
    GtkWidget *events = add_video_tile(box, show->name, subtitle, &image, &badges,
                                       seen == count ? 1 : MIN(watched / count, 0.99));
    g_object_set_data(G_OBJECT(events), "show", (gpointer)show);
    g_object_set_data(G_OBJECT(gtk_widget_get_parent(events)), "show", (gpointer)show);
    const Group *group = preview_group(show);
    if (group && group->has_news) gtk_box_pack_start(GTK_BOX(badges), new_news_badge(), FALSE,
                                                     FALSE, 0);
    if (show_complete(show)) {
        GtkWidget *badge = new_badge("✓ Komplett", "ok");
        gtk_widget_set_tooltip_text(badge, "Alle Folgen geladen, die die Mediatheken anbieten, "
                                           "ohne Lücke in der Nummerierung");
        gtk_box_pack_start(GTK_BOX(badges), badge, FALSE, FALSE, 0);
    }
    const MdHistoryEntry *first = show->entries->pdata[0];
    char *key = g_strdup_printf("show:%s|%d", show->name, group && group->website);
    request_frame(image, key, first->path, group ? group->website : NULL);
    g_free(key);
    g_free(subtitle);
}

/* --- Serienansicht --------------------------------------------------------------------- */
static void clear_container(GtkWidget *box)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(children);
}

static GtkWidget *new_tile_box(void)
{
    GtkWidget *box = gtk_flow_box_new();
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(box), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(box), TRUE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(box), 1);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(box), TILES_PER_LINE_MAX);
    gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(box), 6);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(box), 6);
    g_signal_connect(box, "child-activated", G_CALLBACK(on_video_tile_activated), NULL);
    return box;
}

static void rebuild_show_episodes(const Show *show)
{
    clear_container(ui.show_episodes);
    for (guint i = 0; i < show->entries->len; i++) {
        const MdHistoryEntry *e = show->entries->pdata[i];
        if (ui.show_season == G_MININT || e->season == ui.show_season)
            add_entry_tile(ui.show_episodes, e, false);
    }
    gtk_widget_show_all(ui.show_episodes);
}

static void on_season_toggled(GtkToggleButton *b, gpointer d)
{
    (void)d;
    if (!gtk_toggle_button_get_active(b)) return;
    ui.show_season = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "season"));
    const Show *show = ui.open_show ? find_show(ui.open_show) : NULL;
    if (show) rebuild_show_episodes(show);
}

static void on_show_play_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    play_path(g_object_get_data(G_OBJECT(b), "path"));
}

/* Serienansicht füllen; Staffel bleibt, wenn es sie noch gibt */
static void fill_show_page(const Show *show)
{
    int seasons = 0, last = G_MININT, unseen = 0, complete_seasons = 0, numbered_seasons = 0;
    bool season_known = false;
    gint64 bytes = 0;
    clear_container(ui.show_seasons);
    GSList *group = NULL;
    for (guint i = 0; i < show->entries->len; i++) {
        const MdHistoryEntry *e = show->entries->pdata[i];
        bytes += e->size;
        unseen += !watch_done(watch_get(e->path));
        if (e->season == last) continue;
        last = e->season;
        seasons++;
        season_known |= e->season == ui.show_season;
        bool done = season_state(show, e->season) == SEASON_COMPLETE;
        complete_seasons += done;
        numbered_seasons += e->season >= 0;
        char *text = e->season >= 0 ? g_strdup_printf(done ? "Staffel %d ✓" : "Staffel %d",
                                                      e->season)
                                    : g_strdup("Ohne Staffel");
        GtkWidget *chip = gtk_radio_button_new_with_label(group, text);
        if (done) gtk_widget_set_tooltip_text(chip, "Staffel komplett");
        group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(chip));
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(chip), FALSE);
        add_css_class(chip, "zd-chip");
        g_object_set_data(G_OBJECT(chip), "season", GINT_TO_POINTER(e->season));
        gtk_box_pack_start(GTK_BOX(ui.show_seasons), chip, FALSE, FALSE, 0);
        g_free(text);
    }
    if (!season_known) ui.show_season = seasons > 1 ? ((MdHistoryEntry *)
                                                       show->entries->pdata[0])->season
                                                    : G_MININT;
    GList *chips = gtk_container_get_children(GTK_CONTAINER(ui.show_seasons));
    for (GList *l = chips; l; l = l->next) {
        if (GPOINTER_TO_INT(g_object_get_data(l->data, "season")) == ui.show_season)
            gtk_toggle_button_set_active(l->data, TRUE);
        g_signal_connect(l->data, "toggled", G_CALLBACK(on_season_toggled), NULL);
    }
    g_list_free(chips);
    gtk_widget_set_visible(ui.show_seasons, seasons > 1);
    gtk_widget_set_no_show_all(ui.show_seasons, seasons <= 1);

    int count = (int)show->entries->len;
    char *size = md_format_size((double)bytes);
    char *complete = show_complete(show) ? g_strdup(" · komplett")
                     : complete_seasons ? g_strdup_printf(" · %d von %d %s komplett",
                                                          complete_seasons, numbered_seasons,
                                                          plural(numbered_seasons, "Staffel",
                                                                 "Staffeln"))
                     : g_strdup("");
    char *sub = g_strdup_printf("%d %s · %d von %d gesehen · %s%s", count,
                                plural(count, "Folge", "Folgen"), count - unseen, count, size,
                                complete);
    g_free(complete);
    set_label_text(ui.show_title, show->name);
    set_label_text(ui.show_sub, sub);
    bool resume = false;
    const MdHistoryEntry *next = show_resume_entry(show, &resume);
    char *label = next->season >= 0 ? g_strdup_printf("S%02d E%02d", next->season,
                                                      next->episode)
                                    : episode_label(next);
    char *text = g_strdup_printf("%s %s", resume ? "▶  Weiterschauen:" : "▶  Abspielen:", label);
    gtk_button_set_label(GTK_BUTTON(ui.show_play), text);
    g_object_set_data_full(G_OBJECT(ui.show_play), "path", g_strdup(next->path), g_free);
    g_free(size);
    g_free(sub);
    g_free(label);
    g_free(text);
    rebuild_show_episodes(show);
    gtk_widget_show_all(ui.show_seasons);
}

static void open_show(const Show *show)
{
    if (g_strcmp0(ui.open_show, show->name)) ui.show_season = G_MININT;
    g_free(ui.open_show);
    ui.open_show = g_strdup(show->name);
    fill_show_page(show);
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(
                                 GTK_SCROLLED_WINDOW(ui.show_scroller)), 0);
    gtk_stack_set_visible_child_name(GTK_STACK(ui.videos_stack), "serie");
}

static void on_show_back_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    g_clear_pointer(&ui.open_show, g_free);
    gtk_stack_set_visible_child_name(GTK_STACK(ui.videos_stack), "uebersicht");
}

/* Klick auf eine Kachel: Serie öffnen, sonst abspielen */
static void on_video_tile_activated(GtkFlowBox *box, GtkFlowBoxChild *child, gpointer d)
{
    (void)box;
    (void)d;
    const Show *show = g_object_get_data(G_OBJECT(child), "show");
    const MdHistoryEntry *e = g_object_get_data(G_OBJECT(child), "entry");
    if (show && !show->is_film) open_show(show);
    else if (show) play_path(((MdHistoryEntry *)show->entries->pdata[0])->path);
    else if (e) play_path(e->path);
}

/* --- Übersicht ------------------------------------------------------------------------- */
/* Mausrad über einer Reihe scrollt die Seite, nicht die Reihe */
static gboolean on_row_scroll(GtkWidget *w, GdkEventScroll *e, gpointer d)
{
    (void)w;
    (void)d;
    double dx = 0, dy = 0;
    if (e->direction == GDK_SCROLL_UP) dy = -1;
    else if (e->direction == GDK_SCROLL_DOWN) dy = 1;
    else if (e->direction == GDK_SCROLL_SMOOTH) gdk_event_get_scroll_deltas((GdkEvent *)e, &dx,
                                                                             &dy);
    if (dy == 0 || ABS(dx) > ABS(dy)) return FALSE;
    GtkAdjustment *page = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(ui.videos_scroller));
    gtk_adjustment_set_value(page, gtk_adjustment_get_value(page) + dy * ROW_SCROLL_STEP);
    return TRUE;
}

/* Abschnitt mit Überschrift: Reihe zum seitlichen Blättern oder Raster */
static GtkWidget *add_videos_section(const char *title, bool row)
{
    gtk_box_pack_start(GTK_BOX(ui.videos_content), new_label(title, "zd-heading"), FALSE, FALSE,
                       0);
    GtkWidget *box = new_tile_box();
    gtk_widget_set_halign(box, GTK_ALIGN_START);
    if (!row) {
        gtk_box_pack_start(GTK_BOX(ui.videos_content), box, FALSE, FALSE, 0);
        return box;
    }
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(box), FALSE);
    gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(box), VIDEOS_ROW_TILES);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(box), VIDEOS_ROW_TILES);
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_NEVER);
    gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scrolled), TRUE);
    gtk_container_add(GTK_CONTAINER(scrolled), box);
    g_signal_connect(scrolled, "scroll-event", G_CALLBACK(on_row_scroll), NULL);
    gtk_box_pack_start(GTK_BOX(ui.videos_content), scrolled, FALSE, FALSE, 0);
    return box;
}

static int compare_show_names(gconstpointer a, gconstpointer b)
{
    const Show *x = *(Show *const *)a, *y = *(Show *const *)b;
    return g_utf8_collate(x->name, y->name);
}

static int compare_show_newest(gconstpointer a, gconstpointer b)
{
    const Show *x = *(Show *const *)a, *y = *(Show *const *)b;
    return x->newest < y->newest ? 1 : x->newest > y->newest ? -1 : 0;
}

static int compare_watch_updated(gconstpointer a, gconstpointer b)
{
    const WatchState *x = watch_get((*(MdHistoryEntry *const *)a)->path);
    const WatchState *y = watch_get((*(MdHistoryEntry *const *)b)->path);
    return x->updated < y->updated ? 1 : x->updated > y->updated ? -1 : 0;
}

/* Trifft der Suchtext auf diesen Eintrag zu? */
static bool entry_matches(const MdHistoryEntry *e, const char *filter)
{
    if (!*filter) return true;
    char *text = g_strconcat(e->topic, " ", e->title, NULL), *lower = g_utf8_strdown(text, -1);
    bool match = strstr(lower, filter) != NULL;
    g_free(text);
    g_free(lower);
    return match;
}

static bool show_matches(const Show *show, const char *filter)
{
    if (!*filter) return true;
    for (guint i = 0; i < show->entries->len; i++)
        if (entry_matches(show->entries->pdata[i], filter)) return true;
    return false;
}

/* Serien und Filme aus dem Verlauf zusammenstellen; nur vorhandene Dateien */
static void collect_shows(void)
{
    if (ui.shows) g_ptr_array_free(ui.shows, TRUE);
    ui.shows = g_ptr_array_new_with_free_func(show_free);
    Show *current = NULL;
    for (int i = 0; i < ui.video_count; i++) {
        MdHistoryEntry *e = &ui.videos[i];
        if (!e->exists) continue;
        if (e->is_film || !current || current->is_film || strcmp(current->name, e->topic)) {
            current = g_new0(Show, 1);
            current->name = g_strdup(e->is_film ? e->title : e->topic);
            current->is_film = e->is_film;
            current->entries = g_ptr_array_new();
            g_ptr_array_add(ui.shows, current);
        }
        g_ptr_array_add(current->entries, e);
        current->newest = MAX(current->newest, e->finished);
        current->bytes += e->size;
    }
}

/* --- Kategorien in "Meine Videos" ----------------------------------------------------- */
static char *show_key(const Show *show)
{
    char *lower = g_utf8_strdown(show->name, -1);
    char *key = g_strdup_printf("%c:%s", show->is_film ? 'F' : 'S', lower);
    g_free(lower);
    return key;
}

static char *video_genres_path(void)
{
    return g_build_filename(md_app_dir(), VIDEO_GENRES_FILE, NULL);
}

/* Schlüssel <Tab> Kategorien mit "|" getrennt */
static void video_genres_load(void)
{
    ui.video_genres = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            (GDestroyNotify)g_strfreev);
    ui.video_genres_asked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *path = video_genres_path(), *text = NULL;
    if (g_file_get_contents(path, &text, NULL, NULL)) {
        char **lines = g_strsplit(text, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            char **f = g_strsplit(lines[i], "\t", 2);
            if (g_strv_length(f) == 2)
                g_hash_table_replace(ui.video_genres, g_strdup(f[0]),
                                     *f[1] ? g_strsplit(f[1], "|", -1) : g_new0(char *, 1));
            g_strfreev(f);
        }
        g_strfreev(lines);
    }
    g_free(path);
    g_free(text);
}

static void video_genres_save(void)
{
    GString *out = g_string_new(NULL);
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, ui.video_genres);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        /* ohne Treffer: beim nächsten Start noch einmal fragen, IMDb antwortet nicht immer */
        if (!*(char **)value) continue;
        char *joined = g_strjoinv("|", value);
        g_string_append_printf(out, "%s\t%s\n", (char *)key, joined);
        g_free(joined);
    }
    char *path = video_genres_path();
    g_file_set_contents(path, out->str, out->len, NULL);
    g_free(path);
    g_string_free(out, TRUE);
}

/* Kategorien einer Serie oder eines Films; NULL = noch unbekannt (dann wird nachgefragt).
 * Die Vorschau weiß es am genauesten, sonst IMDb. */
static char **show_genres(const Show *show)
{
    char *key = show_key(show);
    const Group *group = preview_group(show);
    char **genres = g_hash_table_lookup(ui.video_genres, key);
    if (group && group->genres && *group->genres &&
        (!genres || !g_strv_equal((const char *const *)genres,
                                  (const char *const *)group->genres))) {
        genres = g_strdupv(group->genres);
        g_hash_table_replace(ui.video_genres, g_strdup(key), genres);
        video_genres_save();
    }
    if (!genres && !g_hash_table_contains(ui.video_genres_asked, key)) {
        g_hash_table_add(ui.video_genres_asked, g_strdup(key));
        GenreJob *job = g_new0(GenreJob, 1);
        job->key = g_strdup(key);
        job->name = g_strdup(show->name);
        job->is_film = show->is_film;
        job->videos = true;
        g_thread_pool_push(ui.imdb_pool, job, NULL);
    }
    g_free(key);
    return genres;
}

static bool show_has_genre(const Show *show, const char *genre)
{
    char **genres = show_genres(show);
    if (!genres) return false;
    if (!strcmp(genre, NO_GENRE)) return !*genres;
    return g_strv_contains((const char *const *)genres, genre);
}

static void update_video_genre_choices(void);

static gboolean on_video_genres_done(gpointer data)
{
    GenreJob *job = data;
    g_hash_table_remove(ui.video_genres_asked, job->key);
    if (!g_hash_table_contains(ui.video_genres, job->key)) {
        g_hash_table_replace(ui.video_genres, g_strdup(job->key),
                             job->genres ? g_steal_pointer(&job->genres) : g_new0(char *, 1));
        video_genres_save();
    }
    /* erst wenn alle Antworten da sind, sonst springt die Auswahl bei jeder Antwort */
    if (!g_hash_table_size(ui.video_genres_asked)) {
        if (gtk_combo_box_get_active_id(GTK_COMBO_BOX(ui.videos_genre))) rebuild_videos(true);
        else update_video_genre_choices();
    }
    g_strfreev(job->genres);
    g_free(job->key);
    g_free(job->name);
    g_free(job);
    return G_SOURCE_REMOVE;
}

/* Auswahl der Kategorien mit Anzahl, häufigste zuerst; die gewählte bleibt */
static void update_video_genre_choices(void)
{
    GHashTable *counts = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; ui.shows && i < ui.shows->len; i++) {
        char **genres = show_genres(ui.shows->pdata[i]);
        char *none[] = {NO_GENRE, NULL};
        for (char **g = genres && !*genres ? none : genres; g && *g; g++) {
            int count = GPOINTER_TO_INT(g_hash_table_lookup(counts, *g));
            g_hash_table_insert(counts, *g, GINT_TO_POINTER(count + 1));
        }
    }
    GList *names = g_list_sort_with_data(g_hash_table_get_keys(counts), compare_genre_names,
                                         counts);
    GtkComboBoxText *combo = GTK_COMBO_BOX_TEXT(ui.videos_genre);
    char *active = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo)));
    ui.updating_video_genres = true;
    gtk_combo_box_text_remove_all(combo);
    gtk_combo_box_text_append(combo, NULL, g_hash_table_size(ui.video_genres_asked)
                                           ? "Alle Kategorien (werden ermittelt …)"
                                           : "Alle Kategorien");
    bool found = false;
    for (GList *l = names; l; l = l->next) {
        char *text = g_strdup_printf("%s (%d)", (char *)l->data,
                                     GPOINTER_TO_INT(g_hash_table_lookup(counts, l->data)));
        gtk_combo_box_text_append(combo, l->data, text);
        found |= !g_strcmp0(active, l->data);
        g_free(text);
    }
    if (active && found) gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), active);
    else gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    ui.updating_video_genres = false;
    g_free(active);
    g_list_free(names);
    g_hash_table_destroy(counts);
}

static void on_videos_filter_changed(GtkComboBox *combo, gpointer d)
{
    (void)combo;
    (void)d;
    if (!ui.updating_video_genres) rebuild_videos(true);
}

/* Seite neu aufbauen; ohne force nur, wenn sich der Verlauf geändert hat */
static void rebuild_videos(bool force)
{
    int count = 0;
    /* von selbst (Takt): nicht auf die Platte warten; nach einer Aktion des Nutzers genau */
    MdHistoryEntry *entries = force ? md_history_snapshot(&count)
                                    : md_history_snapshot_cached(&count);
    GString *mark = g_string_new(NULL);
    for (int i = 0; i < count; i++)
        g_string_append_printf(mark, "%s|%d|%" G_GINT64_FORMAT "\n", entries[i].path,
                               entries[i].exists, entries[i].finished);
    char *fingerprint = g_compute_checksum_for_string(G_CHECKSUM_SHA1, mark->str, -1);
    g_string_free(mark, TRUE);
    if (!force && !g_strcmp0(fingerprint, ui.videos_fingerprint)) {
        md_history_snapshot_free(entries, count);
        g_free(fingerprint);
        return;
    }
    g_free(ui.videos_fingerprint);
    ui.videos_fingerprint = fingerprint;
    /* die Kacheln zeigen auf die alten Einträge: erst abbauen, dann freigeben */
    clear_container(ui.videos_content);
    clear_container(ui.show_episodes);
    if (ui.videos) md_history_snapshot_free(ui.videos, ui.video_count);
    ui.videos = entries;
    ui.video_count = count;
    collect_shows();

    char *filter = g_utf8_strdown(gtk_entry_get_text(GTK_ENTRY(ui.videos_search)), -1);
    const char *genre = gtk_combo_box_get_active_id(GTK_COMBO_BOX(ui.videos_genre));
    int series = 0, films = 0;
    gint64 bytes = 0;
    GPtrArray *started = g_ptr_array_new(), *shown = g_ptr_array_new();
    for (guint i = 0; i < ui.shows->len; i++) {
        Show *show = ui.shows->pdata[i];
        series += !show->is_film;
        films += show->is_film;
        bytes += show->bytes;
        if (!show_matches(show, filter) || (genre && !show_has_genre(show, genre))) continue;
        g_ptr_array_add(shown, show);
        for (guint n = 0; n < show->entries->len; n++) {
            MdHistoryEntry *e = show->entries->pdata[n];
            if (watch_started(watch_get(e->path))) g_ptr_array_add(started, e);
        }
    }
    if (!*filter && !genre && started->len) {
        g_ptr_array_sort(started, compare_watch_updated);
        GtkWidget *box = add_videos_section("Weiterschauen", true);
        for (guint i = 0; i < MIN(started->len, VIDEOS_ROW_TILES); i++)
            add_entry_tile(box, started->pdata[i], true);
    }
    /* jede Serie und jeder Film nur einmal: sortiert nach Name oder nach dem letzten Download */
    bool newest = !g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(ui.videos_sort)), "neu");
    g_ptr_array_sort(shown, newest ? compare_show_newest : compare_show_names);
    for (int film = 0; film < 2; film++) {
        GtkWidget *box = NULL;
        for (guint i = 0; i < shown->len; i++) {
            const Show *show = shown->pdata[i];
            if (show->is_film != film) continue;
            if (!box) box = add_videos_section(film ? "Filme" : "Serien", false);
            add_show_tile(box, show);
        }
    }
    if (!shown->len) {
        GtkWidget *label = new_label(ui.shows->len ? "Nichts gefunden."
                                     : "Noch nichts geladen. Fertige Downloads erscheinen hier.",
                                     "dim-label");
        gtk_label_set_xalign(GTK_LABEL(label), 0.5);
        gtk_widget_set_margin_top(label, 24);
        gtk_box_pack_start(GTK_BOX(ui.videos_content), label, FALSE, FALSE, 0);
    }
    char *size = md_format_size((double)bytes);
    char *sub = g_strdup_printf("%d %s · %d %s · %s", series, plural(series, "Serie", "Serien"),
                                films, plural(films, "Film", "Filme"), size);
    set_label_text(ui.videos_sub, sub);
    g_free(size);
    g_free(sub);
    g_free(filter);
    g_ptr_array_free(started, TRUE);
    g_ptr_array_free(shown, TRUE);
    gtk_widget_show_all(ui.videos_content);
    update_video_genre_choices();

    const Show *open = ui.open_show ? find_show(ui.open_show) : NULL;
    if (open) fill_show_page(open);
    else if (ui.open_show) on_show_back_clicked(NULL, NULL);
}

static void on_videos_search_changed(GtkSearchEntry *e, gpointer d)
{
    (void)e;
    (void)d;
    rebuild_videos(true);
}

/* --- Statistik ------------------------------------------------------------------------- */
#define STATS_TOP 10            /* Einträge der Bestenlisten */
#define STATS_DAYS 30
#define STATS_MONTHS 12
#define STATS_CHART_HEIGHT 110

/* Gibt es noch kein Protokoll, zählt der bisherige Wiedergabestand als Sehzeit */
static void stats_import(void)
{
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, ui.watch);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        const WatchState *w = value;
        bool film = false, done = watch_done(w);
        char *name = stats_name(key, &film);
        md_stats_add(name, film, w->updated, done ? w->duration : w->position, 1, done);
        g_free(name);
    }
    md_stats_save();
}

/* Kachel mit großer Zahl und Beschriftung */
static void add_stat(GtkWidget *box, const char *value, const char *label, const char *tip)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    add_css_class(card, "zd-stat");
    gtk_widget_set_size_request(card, 170, -1);
    GtkWidget *number = new_label(value, "zd-stat-value");
    gtk_label_set_ellipsize(GTK_LABEL(number), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(card), number, FALSE, FALSE, 0);
    GtkWidget *text = new_label(label, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(card), text, FALSE, FALSE, 0);
    if (tip) gtk_widget_set_tooltip_text(card, tip);
    gtk_flow_box_insert(GTK_FLOW_BOX(box), card, -1);
}

static GtkWidget *add_stats_cards(void)
{
    GtkWidget *box = gtk_flow_box_new();
    add_css_class(box, "zd-stats");
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(box), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(box), TRUE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(box), 8);
    gtk_box_pack_start(GTK_BOX(ui.stats_content), box, FALSE, FALSE, 0);
    return box;
}

static void add_stats_heading(const char *title)
{
    gtk_box_pack_start(GTK_BOX(ui.stats_content), new_label(title, "zd-heading"), FALSE, FALSE,
                       0);
}

/* Diagramm mit kleiner Überschrift */
static GtkWidget *stats_chart_box(const char *title, GtkWidget *chart)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *label = new_label(title, "dim-label");
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), chart, TRUE, TRUE, 0);
    return box;
}

/* Zeile einer Bestenliste: Platz, Name, Balken, Wert */
static void add_stats_rank(GtkWidget *grid, int row, const char *name, const char *detail,
                           double share, const char *value)
{
    char *rank = g_strdup_printf("%d.", row + 1);
    GtkWidget *number = new_label(rank, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(number), 1);
    gtk_grid_attach(GTK_GRID(grid), number, 0, row, 1, 1);
    GtkWidget *names = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *title = new_label(name, NULL);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_widget_set_tooltip_text(title, name);
    gtk_box_pack_start(GTK_BOX(names), title, FALSE, FALSE, 0);
    if (detail) {
        GtkWidget *extra = new_label(detail, "dim-label");
        add_css_class(extra, "zd-sub");
        gtk_box_pack_start(GTK_BOX(names), extra, FALSE, FALSE, 0);
    }
    gtk_widget_set_hexpand(names, TRUE);
    gtk_grid_attach(GTK_GRID(grid), names, 1, row, 1, 1);
    GtkWidget *bar = gtk_level_bar_new_for_interval(0, 1);
    gtk_level_bar_set_value(GTK_LEVEL_BAR(bar), CLAMP(share, 0, 1));
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(bar), GTK_LEVEL_BAR_OFFSET_LOW);
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(bar), GTK_LEVEL_BAR_OFFSET_HIGH);
    gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(bar), GTK_LEVEL_BAR_OFFSET_FULL);
    gtk_widget_set_size_request(bar, 160, -1);
    gtk_widget_set_valign(bar, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(grid), bar, 2, row, 1, 1);
    GtkWidget *amount = new_label(value, NULL);
    gtk_label_set_xalign(GTK_LABEL(amount), 1);
    gtk_grid_attach(GTK_GRID(grid), amount, 3, row, 1, 1);
    g_free(rank);
}

static GtkWidget *add_stats_grid(void)
{
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_box_pack_start(GTK_BOX(ui.stats_content), grid, FALSE, FALSE, 0);
    return grid;
}

static char *hours_text(double seconds)
{
    return seconds < 60 ? g_strdup("0 min") : md_format_time(seconds);
}

static void fill_stats_watching(void)
{
    double days[STATS_DAYS], week = 0, month = 0;
    md_stats_days(days, STATS_DAYS);
    for (int i = 0; i < STATS_DAYS; i++) {
        month += days[i];
        if (i >= STATS_DAYS - 7) week += days[i];
    }
    int films = 0, episodes = 0, started = 0;
    GPtrArray *titles = md_stats_titles();
    for (guint i = 0; i < titles->len; i++) {
        const MdStatsTitle *t = titles->pdata[i];
        if (t->film) films += t->finished;
        else episodes += t->finished;
    }
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init(&iter, ui.watch);
    while (g_hash_table_iter_next(&iter, NULL, &value)) started += watch_started(value);
    char *best_day = NULL;
    double best = md_stats_best_day(&best_day);
    int streak = md_stats_streak();

    add_stats_heading("Angesehen");
    GtkWidget *cards = add_stats_cards();
    char *total = hours_text(md_stats_total()), *last_week = hours_text(week);
    char *average = hours_text(month / STATS_DAYS), *best_time = hours_text(best);
    char *films_text = g_strdup_printf("%d", films);
    char *episodes_text = g_strdup_printf("%d", episodes);
    char *started_text = g_strdup_printf("%d", started);
    char *streak_text = g_strdup_printf("%d %s", streak, plural(streak, "Tag", "Tage"));
    char *best_label = *best_day ? g_strdup_printf("Bester Tag (%c%c.%c%c.%.4s)", best_day[8],
                                                   best_day[9], best_day[5], best_day[6],
                                                   best_day)
                                 : g_strdup("Bester Tag");
    add_stat(cards, total, "Sehzeit insgesamt", "Nur was wirklich lief, ohne Sprünge");
    add_stat(cards, last_week, "Letzte 7 Tage", NULL);
    add_stat(cards, average, "Ø pro Tag (30 Tage)", NULL);
    add_stat(cards, films_text, "Filme zu Ende gesehen", NULL);
    add_stat(cards, episodes_text, "Folgen zu Ende gesehen", NULL);
    add_stat(cards, started_text, "Angefangen", "Laufen gerade unter „Weiterschauen“");
    add_stat(cards, streak_text, "Am Stück geschaut",
             "Tage mit mindestens einer Minute, bis heute");
    add_stat(cards, best_time, best_label, NULL);
    g_free(total);
    g_free(last_week);
    g_free(average);
    g_free(best_time);
    g_free(films_text);
    g_free(episodes_text);
    g_free(started_text);
    g_free(streak_text);
    g_free(best_label);
    g_free(best_day);

    /* letzte 30 Tage */
    char *labels[STATS_DAYS], *tips[STATS_DAYS];
    GDateTime *now = g_date_time_new_now_local();
    for (int i = 0; i < STATS_DAYS; i++) {
        GDateTime *t = g_date_time_add_days(now, i - (STATS_DAYS - 1));
        char *time = hours_text(days[i]), *date = g_date_time_format(t, "%a %d.%m.");
        labels[i] = (STATS_DAYS - 1 - i) % 5 == 0 ? g_date_time_format(t, "%d.%m.") : NULL;
        tips[i] = g_strdup_printf("%s: %s", date, time);
        g_free(time);
        g_free(date);
        g_date_time_unref(t);
    }
    g_date_time_unref(now);
    GtkWidget *days_chart = md_stats_chart(days, (const char *const *)labels,
                                           (const char *const *)tips, STATS_DAYS,
                                           STATS_CHART_HEIGHT);
    for (int i = 0; i < STATS_DAYS; i++) {
        g_free(labels[i]);
        g_free(tips[i]);
    }
    gtk_box_pack_start(GTK_BOX(ui.stats_content), stats_chart_box("Sehzeit der letzten 30 Tage",
                                                                  days_chart), FALSE, FALSE, 0);

    /* Wochentag und Tageszeit nebeneinander, in schmalen Fenstern untereinander */
    static const char *const WEEKDAYS[7] = {"Mo", "Di", "Mi", "Do", "Fr", "Sa", "So"};
    double weekdays[7], hours[24];
    md_stats_weekdays(weekdays);
    md_stats_hours(hours);
    char *week_tips[7], *hour_labels[24], *hour_tips[24];
    for (int i = 0; i < 7; i++) {
        char *time = hours_text(weekdays[i]);
        week_tips[i] = g_strdup_printf("%s: %s", WEEKDAYS[i], time);
        g_free(time);
    }
    for (int i = 0; i < 24; i++) {
        char *time = hours_text(hours[i]);
        hour_labels[i] = i % 6 == 0 ? g_strdup_printf("%d", i) : NULL;
        hour_tips[i] = g_strdup_printf("%d–%d Uhr: %s", i, i + 1, time);
        g_free(time);
    }
    GtkWidget *pair = gtk_flow_box_new();
    add_css_class(pair, "zd-stats");
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(pair), GTK_SELECTION_NONE);
    gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(pair), TRUE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(pair), 2);
    GtkWidget *week_box = stats_chart_box("Nach Wochentag",
                                          md_stats_chart(weekdays, WEEKDAYS,
                                                         (const char *const *)week_tips, 7,
                                                         STATS_CHART_HEIGHT));
    GtkWidget *hour_box = stats_chart_box("Nach Tageszeit",
                                          md_stats_chart(hours, (const char *const *)hour_labels,
                                                         (const char *const *)hour_tips, 24,
                                                         STATS_CHART_HEIGHT));
    gtk_widget_set_size_request(week_box, 300, -1);
    gtk_widget_set_size_request(hour_box, 300, -1);
    gtk_flow_box_insert(GTK_FLOW_BOX(pair), week_box, -1);
    gtk_flow_box_insert(GTK_FLOW_BOX(pair), hour_box, -1);
    gtk_box_pack_start(GTK_BOX(ui.stats_content), pair, FALSE, FALSE, 0);
    for (int i = 0; i < 7; i++) g_free(week_tips[i]);
    for (int i = 0; i < 24; i++) {
        g_free(hour_labels[i]);
        g_free(hour_tips[i]);
    }

    add_stats_heading("Am meisten gesehen");
    if (!titles->len || ((MdStatsTitle *)titles->pdata[0])->seconds < 60) {
        gtk_box_pack_start(GTK_BOX(ui.stats_content),
                           new_label("Noch nichts angesehen. Was im Player läuft, zählt hier.",
                                     "dim-label"), FALSE, FALSE, 0);
    } else {
        GtkWidget *grid = add_stats_grid();
        double top = ((MdStatsTitle *)titles->pdata[0])->seconds;
        for (guint i = 0; i < MIN(titles->len, STATS_TOP); i++) {
            const MdStatsTitle *t = titles->pdata[i];
            if (t->seconds < 60) break;
            char *time = hours_text(t->seconds), *detail;
            if (t->film) detail = g_strdup_printf("Film · %d× gestartet", t->starts);
            else detail = g_strdup_printf("Serie · %d %s zu Ende", t->finished,
                                          plural(t->finished, "Folge", "Folgen"));
            add_stats_rank(grid, (int)i, t->name, detail, t->seconds / top, time);
            g_free(time);
            g_free(detail);
        }
    }
    g_ptr_array_free(titles, TRUE);
}

/* Größe einer Serie oder eines Films für die Bestenliste der Downloads */
typedef struct {
    const char *name;
    bool film;
    int count;
    gint64 bytes;
} StatsShow;

static gint compare_stats_shows(gconstpointer a, gconstpointer b)
{
    const StatsShow *x = *(StatsShow *const *)a, *y = *(StatsShow *const *)b;
    if (x->count != y->count) return y->count - x->count;
    return (x->bytes < y->bytes) - (x->bytes > y->bytes);
}

static gint compare_entry_size(gconstpointer a, gconstpointer b)
{
    const MdHistoryEntry *x = *(MdHistoryEntry *const *)a, *y = *(MdHistoryEntry *const *)b;
    return (x->size < y->size) - (x->size > y->size);
}

static void fill_stats_downloads(void)
{
    int films = 0, episodes = 0, this_month = 0;
    gint64 bytes = 0, month_bytes = 0;
    double months[STATS_MONTHS] = {0};
    GDateTime *now = g_date_time_new_now_local();
    int now_index = g_date_time_get_year(now) * 12 + g_date_time_get_month(now) - 1;
    GHashTable *shows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GPtrArray *show_list = g_ptr_array_new();
    for (int i = 0; i < ui.video_count; i++) {
        const MdHistoryEntry *e = &ui.videos[i];
        films += e->is_film;
        episodes += !e->is_film;
        bytes += e->size;
        GDateTime *t = g_date_time_new_from_unix_local(e->finished);
        if (t) {
            int index = now_index - (g_date_time_get_year(t) * 12 + g_date_time_get_month(t) - 1);
            if (index >= 0 && index < STATS_MONTHS)
                months[STATS_MONTHS - 1 - index] += (double)e->size;
            if (index == 0) {
                this_month++;
                month_bytes += e->size;
            }
            g_date_time_unref(t);
        }
        const char *name = e->is_film ? e->title : e->topic;
        char *key = g_strdup_printf("%c%s", e->is_film ? 'F' : 'S', name);
        StatsShow *s = g_hash_table_lookup(shows, key);
        if (!s) {
            s = g_new0(StatsShow, 1);
            s->name = name;
            s->film = e->is_film;
            g_hash_table_insert(shows, key, s);
            g_ptr_array_add(show_list, s);
        } else {
            g_free(key);
        }
        s->count++;
        s->bytes += e->size;
    }
    int series = 0;
    for (guint i = 0; i < show_list->len; i++) series += !((StatsShow *)show_list->pdata[i])->film;

    add_stats_heading("Geladen");
    GtkWidget *cards = add_stats_cards();
    char *count = g_strdup_printf("%d", ui.video_count), *size = md_format_size((double)bytes);
    char *films_text = g_strdup_printf("%d", films);
    char *episodes_text = g_strdup_printf("%d", episodes);
    char *series_text = g_strdup_printf("%d", series);
    char *month_size = md_format_size((double)month_bytes);
    char *month_text = g_strdup_printf("%d (%s)", this_month, month_size);
    add_stat(cards, count, "Downloads insgesamt", "Auch Dateien, die nicht mehr da sind");
    add_stat(cards, size, "Datenmenge insgesamt", NULL);
    add_stat(cards, films_text, "Filme", NULL);
    add_stat(cards, episodes_text, "Folgen", NULL);
    add_stat(cards, series_text, "Serien und Sendungen", NULL);
    add_stat(cards, month_text, "Diesen Monat", NULL);
    g_free(count);
    g_free(size);
    g_free(films_text);
    g_free(episodes_text);
    g_free(series_text);
    g_free(month_text);
    g_free(month_size);

    char *labels[STATS_MONTHS], *tips[STATS_MONTHS];
    for (int i = 0; i < STATS_MONTHS; i++) {
        GDateTime *t = g_date_time_add_months(now, i - (STATS_MONTHS - 1));
        char *amount = md_format_size(months[i]);
        labels[i] = g_date_time_format(t, "%b");
        char *month = g_date_time_format(t, "%B %Y");
        tips[i] = g_strdup_printf("%s: %s", month, amount);
        g_free(month);
        g_free(amount);
        g_date_time_unref(t);
    }
    gtk_box_pack_start(GTK_BOX(ui.stats_content),
                       stats_chart_box("Geladen je Monat",
                                       md_stats_chart(months, (const char *const *)labels,
                                                      (const char *const *)tips, STATS_MONTHS,
                                                      STATS_CHART_HEIGHT)),
                       FALSE, FALSE, 0);
    for (int i = 0; i < STATS_MONTHS; i++) {
        g_free(labels[i]);
        g_free(tips[i]);
    }
    g_date_time_unref(now);

    g_ptr_array_sort(show_list, compare_stats_shows);
    if (show_list->len) {
        add_stats_heading("Am meisten geladen");
        GtkWidget *grid = add_stats_grid();
        const StatsShow *top = show_list->pdata[0];
        for (guint i = 0; i < MIN(show_list->len, STATS_TOP); i++) {
            const StatsShow *s = show_list->pdata[i];
            char *amount = md_format_size((double)s->bytes);
            char *detail = s->film ? g_strdup("Film")
                                   : g_strdup_printf("%d %s", s->count,
                                                     plural(s->count, "Folge", "Folgen"));
            add_stats_rank(grid, (int)i, s->name, detail, (double)s->count / top->count, amount);
            g_free(amount);
            g_free(detail);
        }
    }
    g_ptr_array_free(show_list, TRUE);
    g_hash_table_destroy(shows);
}

static void fill_stats_disk(void)
{
    int present = 0, missing = 0, seen = 0, unseen = 0;
    gint64 bytes = 0, seen_bytes = 0, unseen_bytes = 0;
    GPtrArray *files = g_ptr_array_new();
    for (int i = 0; i < ui.video_count; i++) {
        MdHistoryEntry *e = &ui.videos[i];
        if (!e->exists) {
            missing++;
            continue;
        }
        present++;
        bytes += e->size;
        g_ptr_array_add(files, e);
        if (watch_done(watch_get(e->path))) {
            seen++;
            seen_bytes += e->size;
        } else {
            unseen++;
            unseen_bytes += e->size;
        }
    }
    guint64 free_bytes = 0, total_bytes = 0;
    char *existing = g_strdup(ui.cfg.dir);          /* Speicherort entsteht erst beim Laden */
    while (!g_file_test(existing, G_FILE_TEST_IS_DIR) && strlen(existing) > 1) {
        char *parent = g_path_get_dirname(existing);
        g_free(existing);
        existing = parent;
    }
    GFile *dir = g_file_new_for_path(existing);
    g_free(existing);
    GFileInfo *info = g_file_query_filesystem_info(dir, G_FILE_ATTRIBUTE_FILESYSTEM_FREE ","
                                                   G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, NULL, NULL);
    if (info) {
        free_bytes = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
        total_bytes = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
        g_object_unref(info);
    }
    g_object_unref(dir);

    add_stats_heading("Festplatte");
    GtkWidget *cards = add_stats_cards();
    char *size = md_format_size((double)bytes), *free_text = md_format_size((double)free_bytes);
    char *seen_size = md_format_size((double)seen_bytes);
    char *unseen_size = md_format_size((double)unseen_bytes);
    char *files_text = g_strdup_printf("%d %s", present, plural(present, "Datei", "Dateien"));
    char *seen_label = g_strdup_printf("Schon gesehen (%d)", seen);
    char *unseen_label = g_strdup_printf("Noch ungesehen (%d)", unseen);
    char *missing_text = g_strdup_printf("%d", missing);
    add_stat(cards, size, "Belegt durch Videos", NULL);
    add_stat(cards, free_text, "Frei am Speicherort", ui.cfg.dir);
    add_stat(cards, files_text, "Auf der Festplatte", NULL);
    add_stat(cards, seen_size, seen_label, "Lässt sich in „Meine Videos“ per rechter Maustaste "
             "in den Papierkorb verschieben");
    add_stat(cards, unseen_size, unseen_label, NULL);
    add_stat(cards, missing_text, "Nicht mehr da", "Im Verlauf, aber ohne Datei");
    g_free(size);
    g_free(free_text);
    g_free(seen_size);
    g_free(unseen_size);
    g_free(files_text);
    g_free(seen_label);
    g_free(unseen_label);
    g_free(missing_text);

    if (total_bytes > 0) {
        /* Laufwerk: Videos, sonstiges, frei */
        double videos = (double)bytes / (double)total_bytes;
        double used = 1 - (double)free_bytes / (double)total_bytes;
        GtkWidget *bar = gtk_level_bar_new_for_interval(0, 1);
        gtk_level_bar_set_value(GTK_LEVEL_BAR(bar), used);
        gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(bar), GTK_LEVEL_BAR_OFFSET_LOW);
        gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(bar), GTK_LEVEL_BAR_OFFSET_HIGH);
        gtk_level_bar_remove_offset_value(GTK_LEVEL_BAR(bar), GTK_LEVEL_BAR_OFFSET_FULL);
        gtk_widget_set_size_request(bar, -1, 12);
        char *total = md_format_size((double)total_bytes);
        char *text = g_strdup_printf("Laufwerk %s: %.0f %% belegt, davon %.0f %% durch Videos",
                                     total, used * 100, videos * 100);
        gtk_box_pack_start(GTK_BOX(ui.stats_content), stats_chart_box(text, bar), FALSE, FALSE,
                           6);
        g_free(total);
        g_free(text);
    }

    if (files->len) {
        g_ptr_array_sort(files, compare_entry_size);
        add_stats_heading("Größte Dateien");
        GtkWidget *grid = add_stats_grid();
        double top = (double)((MdHistoryEntry *)files->pdata[0])->size;
        for (guint i = 0; i < MIN(files->len, 5); i++) {
            const MdHistoryEntry *e = files->pdata[i];
            char *amount = md_format_size((double)e->size);
            char *label = e->is_film ? g_strdup(e->title) : episode_label(e);
            add_stats_rank(grid, (int)i, label, e->is_film ? NULL : e->topic,
                           top > 0 ? (double)e->size / top : 0, amount);
            g_free(amount);
            g_free(label);
        }
    }
    g_ptr_array_free(files, TRUE);
}

/* Statistik neu aufbauen, wenn der Reiter sichtbar wird */
static void rebuild_stats(void)
{
    rebuild_videos(false);
    clear_container(ui.stats_content);
    fill_stats_watching();
    fill_stats_downloads();
    fill_stats_disk();
    gtk_widget_show_all(ui.stats_content);
}

static void on_stack_page_changed(GObject *stack, GParamSpec *spec, gpointer d)
{
    (void)spec;
    (void)d;
    const char *name = gtk_stack_get_visible_child_name(GTK_STACK(stack));
    if (!g_strcmp0(name, "statistik")) rebuild_stats();
    if (!g_strcmp0(name, "player")) return;
    /* hierhin kehrt der Player zurück, wenn er zugeht */
    ui.player_return = gtk_stack_get_visible_child(GTK_STACK(stack));
    /* anderer Reiter gewählt: ein laufendes Video spielt im kleinen Fenster weiter, sonst geht
     * der Player zu */
    if (md_player_current() && !md_player_pip()) md_player_close();
}

/* Sehzeit löschen; Downloads und Platz kommen aus Verlauf und Dateien und bleiben */
static void on_stats_reset_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    if (!ask_confirmation("Statistik zurücksetzen?",
                          "Die erfasste Sehzeit mit allen Tagen, Uhrzeiten und Titeln wird "
                          "gelöscht. Downloads und Speicherplatz zählen weiter, denn sie kommen "
                          "aus deinen Dateien. Das lässt sich nicht rückgängig machen.",
                          "Abbrechen", "Zurücksetzen", true))
        return;
    md_stats_reset();
    md_log("Statistik: Sehzeit zurückgesetzt");
    rebuild_stats();
}

static GtkWidget *build_stats_page(void)
{
    GtkWidget *page = new_page_column(&ui.stats_scroller);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(top), new_label("Statistik", "zd-hero"), TRUE, TRUE, 0);
    GtkWidget *reset = gtk_button_new_with_label("Statistik zurücksetzen");
    gtk_widget_set_tooltip_text(reset, "Erfasste Sehzeit löschen und neu anfangen");
    gtk_widget_set_valign(reset, GTK_ALIGN_CENTER);
    g_signal_connect(reset, "clicked", G_CALLBACK(on_stats_reset_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(top), reset, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);
    ui.stats_sub = new_label("Was du geschaut und geladen hast. Sehzeit zählt nur im eingebauten "
                             "Player.", "zd-lead");
    add_css_class(ui.stats_sub, "dim-label");
    set_wrapping(ui.stats_sub, 60);
    gtk_box_pack_start(GTK_BOX(page), ui.stats_sub, FALSE, FALSE, 0);
    ui.stats_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(page), ui.stats_content, FALSE, FALSE, 0);
    return ui.stats_scroller;
}

static void on_main_window_destroy(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
    ui.closed = true;
    if (ui.videos_rebuild_source) g_source_remove(ui.videos_rebuild_source);
    ui.videos_rebuild_source = 0;
}

/* --- Speicherort prüfen ---------------------------------------------------------------- */
#define CHECK_LIST_MAX 200      /* so viele Namen zeigt die Liste unter einer Auswahl */

/* Auswahl mit aufklappbarer Liste der betroffenen Dateien */
static GtkWidget *add_check_option(GtkWidget *box, const char *text, bool active,
                                   GPtrArray *names)
{
    GtkWidget *check = gtk_check_button_new_with_label(text);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), active);
    gtk_box_pack_start(GTK_BOX(box), check, FALSE, FALSE, 0);
    GString *list = g_string_new(NULL);
    for (guint i = 0; i < MIN(names->len, CHECK_LIST_MAX); i++)
        g_string_append_printf(list, "%s%s", i ? "\n" : "", (char *)names->pdata[i]);
    if (names->len > CHECK_LIST_MAX)
        g_string_append_printf(list, "\n… und %u weitere", names->len - CHECK_LIST_MAX);
    GtkWidget *expander = gtk_expander_new("Dateien anzeigen");
    gtk_widget_set_margin_start(expander, 24);
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), 120);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scrolled), 220);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scrolled), TRUE);
    GtkWidget *label = new_label(list->str, "zd-sub");
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_label_set_yalign(GTK_LABEL(label), 0);
    gtk_container_add(GTK_CONTAINER(scrolled), label);
    gtk_container_add(GTK_CONTAINER(expander), scrolled);
    gtk_box_pack_start(GTK_BOX(box), expander, FALSE, FALSE, 0);
    g_string_free(list, TRUE);
    return check;
}

/* Pfad relativ zum Speicherort, für die Liste */
static char *check_display_path(const char *path)
{
    size_t length = strlen(ui.cfg.dir);
    if (g_str_has_prefix(path, ui.cfg.dir) && G_IS_DIR_SEPARATOR(path[length]))
        return g_filename_display_name(path + length + 1);
    return g_filename_display_name(path);
}

static gboolean on_folder_checked(gpointer data)
{
    MdFolderCheck *check = data;
    if (ui.closed) {
        md_folder_check_free(check);
        return G_SOURCE_REMOVE;
    }
    gtk_widget_set_sensitive(ui.videos_check, TRUE);
    gtk_button_set_label(GTK_BUTTON(ui.videos_check), "Ordner prüfen");
    guint moved = check->moved->len, found = check->found->len, missing = check->missing->len;
    GtkWidget *d = gtk_dialog_new_with_buttons("Speicherort geprüft", GTK_WINDOW(ui.win),
                                               GTK_DIALOG_MODAL | GTK_DIALOG_USE_HEADER_BAR,
                                               NULL, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_box_set_spacing(GTK_BOX(box), 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 16);
    gtk_window_set_default_size(GTK_WINDOW(d), 560, -1);
    char *summary = g_strdup_printf("%d %s im Verlauf liegen dort, wo sie sein sollen.",
                                    check->present, plural(check->present, "Datei", "Dateien"));
    gtk_box_pack_start(GTK_BOX(box), set_wrapping(new_label(summary, NULL), 60), FALSE, FALSE, 0);
    g_free(summary);
    GtkWidget *relink = NULL, *add = NULL, *forget = NULL;
    if (moved) {
        GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; i < moved; i++)
            g_ptr_array_add(names, check_display_path(((MdMovedFile *)check->moved->pdata[i])->to));
        char *text = g_strdup_printf("Neuen Ort von %u verschobenen %s übernehmen", moved,
                                     moved == 1 ? "Datei" : "Dateien");
        relink = add_check_option(box, text, true, names);
        g_free(text);
        g_ptr_array_free(names, TRUE);
    }
    if (found) {
        GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; i < found; i++)
            g_ptr_array_add(names, check_display_path(
                ((MdFoundFile *)check->found->pdata[i])->path));
        char *text = g_strdup_printf("%u %s in „Meine Videos“ aufnehmen, die dort noch %s", found,
                                     found == 1 ? "Datei" : "Dateien",
                                     found == 1 ? "fehlt" : "fehlen");
        add = add_check_option(box, text, true, names);
        g_free(text);
        g_ptr_array_free(names, TRUE);
    }
    if (missing) {
        GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
        for (guint i = 0; i < missing; i++)
            g_ptr_array_add(names, check_display_path(check->missing->pdata[i]));
        char *text = g_strdup_printf("%u %s aus dem Verlauf entfernen, %s Datei fehlt (die "
                                     "Folgen gelten dann als nicht geladen)", missing,
                                     missing == 1 ? "Eintrag" : "Einträge",
                                     missing == 1 ? "dessen" : "deren");
        forget = add_check_option(box, text, false, names);
        g_free(text);
        g_ptr_array_free(names, TRUE);
    }
    bool anything = moved || found || missing;
    if (!anything)
        gtk_box_pack_start(GTK_BOX(box), new_label("Alles stimmt überein.", "dim-label"),
                           FALSE, FALSE, 0);
    gtk_dialog_add_button(GTK_DIALOG(d), anything ? "Abbrechen" : "Schließen",
                          GTK_RESPONSE_CANCEL);
    if (anything) {
        GtkWidget *apply = gtk_dialog_add_button(GTK_DIALOG(d), "Übernehmen", GTK_RESPONSE_OK);
        add_css_class(apply, "suggested-action");
    }
    gtk_widget_show_all(d);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        bool do_relink = relink && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(relink));
        bool do_add = add && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(add));
        bool do_forget = forget && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(forget));
        md_folder_check_apply(check, do_relink, do_add, do_forget);
        rebuild_videos(true);
    }
    gtk_widget_destroy(d);
    md_folder_check_free(check);
    return G_SOURCE_REMOVE;
}

static gpointer folder_check_thread(gpointer data)
{
    char *dir = data;
    g_idle_add(on_folder_checked, md_check_folder(dir));
    g_free(dir);
    return NULL;
}

static void on_videos_check_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
    gtk_button_set_label(b, "Prüfe …");
    g_thread_unref(g_thread_new("ordner", folder_check_thread, g_strdup(ui.cfg.dir)));
}

static GtkWidget *build_videos_page(void)
{
    watch_load();
    ui.frames = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    ui.frame_waiting = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    ui.frame_pool = g_thread_pool_new(frame_worker, NULL, FRAME_THREADS, FALSE, NULL);
    ui.show_season = G_MININT;
    ui.videos_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(ui.videos_stack),
                                  GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);

    GtkWidget *page = new_page_column(&ui.videos_scroller);
    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_box_pack_start(GTK_BOX(heading), new_label("Meine Videos", "zd-hero"), FALSE, FALSE, 0);
    ui.videos_sub = new_label("", "zd-lead");
    add_css_class(ui.videos_sub, "dim-label");
    gtk_box_pack_start(GTK_BOX(heading), ui.videos_sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), heading, TRUE, TRUE, 0);
    ui.videos_search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui.videos_search), "In meinen Videos suchen …");
    gtk_entry_set_width_chars(GTK_ENTRY(ui.videos_search), 28);
    gtk_widget_set_valign(ui.videos_search, GTK_ALIGN_CENTER);
    g_signal_connect(ui.videos_search, "search-changed", G_CALLBACK(on_videos_search_changed),
                     NULL);
    gtk_box_pack_end(GTK_BOX(top), ui.videos_search, FALSE, FALSE, 0);
    video_genres_load();
    ui.videos_genre = gtk_combo_box_text_new();
    gtk_widget_set_tooltip_text(ui.videos_genre, "Nur eine Kategorie zeigen");
    gtk_widget_set_valign(ui.videos_genre, GTK_ALIGN_CENTER);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.videos_genre), NULL, "Alle Kategorien");
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui.videos_genre), 0);
    g_signal_connect(ui.videos_genre, "changed", G_CALLBACK(on_videos_filter_changed), NULL);
    GtkWidget *filters = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(filters, 6);
    ui.videos_sort = gtk_combo_box_text_new();
    gtk_widget_set_tooltip_text(ui.videos_sort, "Reihenfolge der Serien und Filme");
    gtk_widget_set_valign(ui.videos_sort, GTK_ALIGN_CENTER);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.videos_sort), "name", "A bis Z");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.videos_sort), "neu", "Zuletzt geladen");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui.videos_sort), "name");
    g_signal_connect(ui.videos_sort, "changed", G_CALLBACK(on_videos_filter_changed), NULL);
    gtk_box_pack_start(GTK_BOX(filters), ui.videos_sort, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(filters), ui.videos_genre, FALSE, FALSE, 0);
    ui.videos_check = gtk_button_new_with_label("Ordner prüfen");
    gtk_widget_set_tooltip_text(ui.videos_check, "Speicherort mit dem Verlauf abgleichen: "
                                "verschobene, neue und fehlende Dateien finden");
    gtk_widget_set_valign(ui.videos_check, GTK_ALIGN_CENTER);
    g_signal_connect(ui.videos_check, "clicked", G_CALLBACK(on_videos_check_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(top), ui.videos_check, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), build_news_section(), FALSE, FALSE, 0);
    GtkWidget *library = new_label("Geladene Videos", "zd-heading");
    gtk_widget_set_margin_top(library, 18);
    gtk_box_pack_start(GTK_BOX(page), library, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), filters, FALSE, FALSE, 0);
    ui.videos_content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(page), ui.videos_content, FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(ui.videos_stack), ui.videos_scroller, "uebersicht");

    GtkWidget *show = new_page_column(&ui.show_scroller);
    GtkWidget *back = gtk_button_new_with_label("Alle Videos");
    gtk_button_set_image(GTK_BUTTON(back), gtk_image_new_from_icon_name("go-previous-symbolic",
                                                                        GTK_ICON_SIZE_BUTTON));
    gtk_button_set_always_show_image(GTK_BUTTON(back), TRUE);
    gtk_button_set_relief(GTK_BUTTON(back), GTK_RELIEF_NONE);
    gtk_widget_set_halign(back, GTK_ALIGN_START);
    g_signal_connect(back, "clicked", G_CALLBACK(on_show_back_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(show), back, FALSE, FALSE, 0);
    ui.show_title = set_wrapping(new_label("", "zd-hero"), 40);
    ui.show_sub = new_label("", "zd-lead");
    add_css_class(ui.show_sub, "dim-label");
    gtk_box_pack_start(GTK_BOX(show), ui.show_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(show), ui.show_sub, FALSE, FALSE, 0);
    ui.show_play = new_pill_button("", "suggested-action");
    gtk_widget_set_halign(ui.show_play, GTK_ALIGN_START);
    gtk_widget_set_margin_top(ui.show_play, 10);
    g_signal_connect(ui.show_play, "clicked", G_CALLBACK(on_show_play_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(show), ui.show_play, FALSE, FALSE, 0);
    ui.show_seasons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(ui.show_seasons, 14);
    gtk_box_pack_start(GTK_BOX(show), ui.show_seasons, FALSE, FALSE, 0);
    ui.show_episodes = new_tile_box();
    gtk_widget_set_halign(ui.show_episodes, GTK_ALIGN_START);
    gtk_widget_set_margin_top(ui.show_episodes, 8);
    gtk_box_pack_start(GTK_BOX(show), ui.show_episodes, FALSE, FALSE, 0);
    gtk_stack_add_named(GTK_STACK(ui.videos_stack), ui.show_scroller, "serie");
    /* Download-Leiste für die neuen Folgen, nur wenn es welche gibt */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.videos_stack, TRUE, TRUE, 0);
    ui.news_footer = build_footer(FOOTER_NEWS);
    gtk_box_pack_start(GTK_BOX(box), ui.news_footer, FALSE, FALSE, 0);
    show_news_list();
    return box;
}

/* ---------------------------------------------------------------------------
 * Seite Einstellungen
 * ------------------------------------------------------------------------- */
/* Zeilen je nach AV1-Schalter und Encoder ein- oder ausgrauen */
static void update_settings_rows(void)
{
    gtk_widget_set_sensitive(ui.encoder_row, ui.cfg.convert);
    gtk_widget_set_sensitive(ui.level_row, ui.cfg.convert);
    gtk_widget_set_sensitive(ui.keep_row, ui.cfg.convert);
    gtk_widget_set_sensitive(ui.gpu_row, ui.cfg.convert && ui.cfg.encoder == MD_ENCODER_GPU);
    gtk_widget_set_sensitive(ui.cpu_row, ui.cfg.convert && ui.cfg.encoder == MD_ENCODER_CPU);
    /* die Rohwerte nur bei "Eigene Werte" */
    bool custom = gtk_combo_box_get_active(GTK_COMBO_BOX(ui.level_combo)) == AV1_LEVEL_CUSTOM;
    gtk_widget_set_visible(ui.gpu_row, custom);
    gtk_widget_set_visible(ui.cpu_row, custom);
}

/* Stufe, zu der die Werte passen; sonst "Eigene Werte" */
static int av1_level(int gpu, int cpu)
{
    for (size_t i = 0; i < AV1_LEVEL_CUSTOM; i++)
        if (AV1_LEVELS[i].gpu == gpu && AV1_LEVELS[i].cpu == cpu) return (int)i;
    return AV1_LEVEL_CUSTOM;
}

static void update_dir_label(void)
{
    char *display = g_filename_display_name(ui.cfg.dir);
    set_label_text(ui.dir_sub, display);
    g_free(display);
    display = g_filename_display_name(md_app_dir());
    char *sub = g_strdup_printf("%s\nEinstellungen, Verlauf, Warteschlange und Zwischenspeicher; "
                                "beim Wechsel zieht alles mit um", display);
    set_label_text(ui.cache_sub, sub);
    g_free(sub);
    g_free(display);
    gtk_widget_set_sensitive(ui.data_reset, md_data_parent() != NULL);
}

/* Ordner wählen lassen -> neuer Pfad oder NULL */
static char *choose_folder(const char *title, const char *current)
{
    GtkWidget *chooser = gtk_file_chooser_dialog_new(title, GTK_WINDOW(ui.win),
                                                     GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                     "Abbrechen", GTK_RESPONSE_CANCEL,
                                                     "Auswählen", GTK_RESPONSE_ACCEPT, NULL);
    g_mkdir_with_parents(current, 0755);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), current);
    char *path = NULL;
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
        path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);
    return path;
}

static void on_choose_dir_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    char *path = choose_folder("Speicherort wählen", ui.cfg.dir);
    if (!path) return;
    g_free(ui.cfg.dir);
    ui.cfg.dir = path;
    update_dir_label();
    settings_save();
    refresh_statuses(NULL);
    update_selection();
}

/* Alle Programmdaten in einen anderen Ordner verschieben; parent NULL = Standardordner */
static void move_data(const char *parent)
{
    char *target = parent ? g_build_filename(parent, "Mediathek Downloader", NULL)
                          : md_default_data_dir();
    char *text = g_strdup_printf("Einstellungen, Verlauf, Warteschlange, Protokoll und "
                                 "Zwischenspeicher ziehen nach %s um. Der Zwischenspeicher folgt "
                                 "im Hintergrund.", target);
    bool yes = ask_confirmation("Programmdaten verschieben?", text, "Abbrechen", "Verschieben",
                                false);
    g_free(text);
    g_free(target);
    if (!yes) return;
    char *error = NULL;
    if (!md_set_data_parent(parent, &error)) {
        show_info_bar(GTK_MESSAGE_ERROR, error);
        g_free(error);
        update_dir_label();
        return;
    }
    settings_save();
    update_dir_label();
    char *display = g_filename_display_name(md_app_dir());
    char *done = g_strdup_printf("Die Programmdaten liegen jetzt in %s.", display);
    show_info_bar(GTK_MESSAGE_INFO, done);
    g_free(done);
    g_free(display);
}

static void on_choose_cache_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    char *start = md_data_parent() ? g_strdup(md_data_parent())
                                   : g_path_get_dirname(md_app_dir());
    char *path = choose_folder("Ordner für Programmdaten wählen", start);
    g_free(start);
    if (path) move_data(path);
    g_free(path);
}

static void on_data_reset_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    move_data(NULL);
}

/* ---------------------------------------------------------------------------
 * Tastenbelegung des Players: Klick auf die Taste einer Aktion, dann die neue Taste drücken.
 * Gehört sie schon einer anderen Aktion, tauschen die beiden.
 * ------------------------------------------------------------------------- */
#define KEY_ACTIONS_MAX 32
static GtkWidget *key_buttons[KEY_ACTIONS_MAX], *key_note;
static int key_capturing = -1;
static gulong key_capture_handler;

static void update_key_buttons(void)
{
    for (int i = 0; i < md_player_key_count() && i < KEY_ACTIONS_MAX; i++) {
        if (!key_buttons[i]) continue;
        char *label = i == key_capturing ? g_strdup("Taste drücken …")
                                         : md_player_key_label(md_player_key_get(i));
        gtk_button_set_label(GTK_BUTTON(key_buttons[i]), label);
        g_free(label);
    }
}

static void stop_key_capture(void)
{
    key_capturing = -1;
    if (key_capture_handler) g_signal_handler_disconnect(ui.win, key_capture_handler);
    key_capture_handler = 0;
    update_key_buttons();
}

static gboolean on_key_capture(GtkWidget *w, GdkEventKey *e, gpointer d)
{
    (void)w;
    (void)d;
    int action = key_capturing;
    if (action < 0) return FALSE;
    if (e->is_modifier) return TRUE;            /* auf die eigentliche Taste warten */
    guint key = gdk_keyval_to_lower(e->keyval), old = md_player_key_get(action);
    int other = md_player_key_find(key);
    char *note = NULL;
    if (other >= 0 && other != action) {
        md_player_key_set(other, old);
        char *label = md_player_key_label(old);
        note = g_strdup_printf("Getauscht: „%s“ liegt jetzt auf %s.", md_player_key_action(other),
                               label);
        g_free(label);
    }
    md_player_key_set(action, key);
    stop_key_capture();
    gtk_label_set_text(GTK_LABEL(key_note), note ? note : "");
    gtk_widget_set_visible(key_note, note != NULL);
    g_free(note);
    settings_save();
    return TRUE;
}

static void on_key_button_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    int action = GPOINTER_TO_INT(d);
    if (key_capturing == action) {              /* zweiter Klick bricht ab */
        stop_key_capture();
        return;
    }
    key_capturing = action;
    if (!key_capture_handler)
        key_capture_handler = g_signal_connect(ui.win, "key-press-event",
                                               G_CALLBACK(on_key_capture), NULL);
    update_key_buttons();
}

static void on_keys_reset(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    for (int i = 0; i < md_player_key_count(); i++) md_player_key_set(i, md_player_key_default(i));
    stop_key_capture();
    gtk_widget_hide(key_note);
    settings_save();
}

static void add_key_settings(GtkWidget *page)
{
    GtkWidget *group = add_group(page, "Tasten im Player");
    GtkWidget *reset = gtk_button_new_with_label("Standard");
    g_signal_connect(reset, "clicked", G_CALLBACK(on_keys_reset), NULL);
    GtkWidget *row = add_row(group, "Klick auf eine Taste, dann die neue Taste drücken",
                             "Gehört sie schon einer anderen Aktion, tauschen die beiden. Ein "
                             "zweiter Klick bricht ab. Zweittasten wie J, K, L und F11 gelten "
                             "zusätzlich, solange keine Aktion sie belegt.", NULL, reset, NULL);
    GtkWidget *box = gtk_bin_get_child(GTK_BIN(row));
    GList *children = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *l = children; l; l = l->next)
        if (GTK_IS_BOX(l->data)) {
            key_note = new_label("", "zd-sub");
            gtk_label_set_line_wrap(GTK_LABEL(key_note), TRUE);
            gtk_widget_set_no_show_all(key_note, TRUE);
            gtk_box_pack_end(GTK_BOX(l->data), key_note, FALSE, FALSE, 0);
            break;
        }
    g_list_free(children);
    for (int i = 0; i < md_player_key_count() && i < KEY_ACTIONS_MAX; i++) {
        key_buttons[i] = gtk_button_new_with_label("");
        gtk_widget_set_size_request(key_buttons[i], 130, -1);
        g_signal_connect(key_buttons[i], "clicked", G_CALLBACK(on_key_button_clicked),
                         GINT_TO_POINTER(i));
        add_row(group, md_player_key_action(i), NULL, NULL, key_buttons[i], NULL);
    }
    update_key_buttons();
}

/* gemeinsamer Handler aller übrigen Einstellungen: alles einlesen und speichern */
static GtkWidget *quality_combo, *encoder_combo, *gpu_spin, *cpu_spin, *parallel_spin;
static GtkWidget *convert_spin, *av1_switch, *keep_switch, *signed_switch, *audio_switch, *min_spin;
static GtkWidget *update_switch, *news_switch, *intro_switch, *recap_switch, *gpu_switch;

static void on_setting_changed(GtkWidget *w, gpointer d)
{
    (void)d;
    Config *c = &ui.cfg;
    int level = gtk_combo_box_get_active(GTK_COMBO_BOX(ui.level_combo));
    if (w == ui.level_combo && level >= 0 && level < (int)AV1_LEVEL_CUSTOM) {
        /* Stufe gewählt: die Rohwerte folgen */
        g_signal_handlers_block_by_func(gpu_spin, on_setting_changed, NULL);
        g_signal_handlers_block_by_func(cpu_spin, on_setting_changed, NULL);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(gpu_spin), AV1_LEVELS[level].gpu);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(cpu_spin), AV1_LEVELS[level].cpu);
        g_signal_handlers_unblock_by_func(gpu_spin, on_setting_changed, NULL);
        g_signal_handlers_unblock_by_func(cpu_spin, on_setting_changed, NULL);
    }
    int old_quality = c->quality;
    c->quality = gtk_combo_box_get_active(GTK_COMBO_BOX(quality_combo));
    c->encoder = gtk_combo_box_get_active(GTK_COMBO_BOX(encoder_combo));
    c->gpu_quality = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(gpu_spin));
    c->cpu_rf = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(cpu_spin));
    c->parallel = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(parallel_spin));
    c->conversions = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(convert_spin));
    c->min_minutes = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(min_spin));
    c->convert = gtk_switch_get_active(GTK_SWITCH(av1_switch));
    c->keep_original = gtk_switch_get_active(GTK_SWITCH(keep_switch));
    c->show_signed = gtk_switch_get_active(GTK_SWITCH(signed_switch));
    bool hide_audio = c->hide_audio;
    c->hide_audio = gtk_switch_get_active(GTK_SWITCH(audio_switch));
    c->update_check = gtk_switch_get_active(GTK_SWITCH(update_switch));
    c->skip_intro = gtk_switch_get_active(GTK_SWITCH(intro_switch));
    md_player_set_auto_skip_intro(c->skip_intro);
    c->skip_recap = gtk_switch_get_active(GTK_SWITCH(recap_switch));
    md_player_set_auto_skip_recap(c->skip_recap);
    if (gpu_switch) {
        c->gpu_convert = gtk_switch_get_active(GTK_SWITCH(gpu_switch));
        md_player_set_gpu_convert(c->gpu_convert);
    }
    bool auto_news = c->auto_news;
    c->auto_news = gtk_switch_get_active(GTK_SWITCH(news_switch));
    if (c->auto_news && !auto_news) start_news_load();      /* gleich nachsehen */
    apply_queue_settings();
    settings_save();
    update_settings_rows();
    if (c->quality != old_quality) rebuild_results();   /* Größen der Treffer */
    /* Filter gilt sofort: Vorschau aus dem Zwischenspeicher neu aufbauen, neue Folgen neu */
    if (c->hide_audio != hide_audio) {
        start_preview_load(MD_CACHE_USE);
        start_news_load();
    }
    update_selection();
}

/* Autostart steht im System, nicht in der Einstellungsdatei */
static void on_autostart_changed(GObject *w, GParamSpec *spec, gpointer d)
{
    (void)spec;
    (void)d;
    bool enabled = gtk_switch_get_active(GTK_SWITCH(w));
    if (enabled == md_autostart_enabled()) return;
    char *error = NULL;
    if (!md_autostart_set(enabled, &error)) {
        show_info_bar(GTK_MESSAGE_WARNING, error);
        g_free(error);
        g_signal_handlers_block_by_func(w, on_autostart_changed, NULL);
        gtk_switch_set_active(GTK_SWITCH(w), !enabled);
        g_signal_handlers_unblock_by_func(w, on_autostart_changed, NULL);
    }
}

/* Schalter anlegen und verdrahten */
static GtkWidget *new_setting_switch(bool active)
{
    GtkWidget *s = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(s), active);
    g_signal_connect(s, "notify::active", G_CALLBACK(on_setting_changed), NULL);
    return s;
}

static GtkWidget *new_setting_spin(double lo, double hi, double value)
{
    GtkWidget *s = new_spin(lo, hi, 1, value);
    g_signal_connect(s, "value-changed", G_CALLBACK(on_setting_changed), NULL);
    return s;
}

static GtkWidget *new_setting_combo(const char *const *texts, int count, int active)
{
    GtkWidget *c = new_combo(texts, count, active);
    g_signal_connect(c, "changed", G_CALLBACK(on_setting_changed), NULL);
    return c;
}

static GtkWidget *build_settings_page(void)
{
    const Config *c = &ui.cfg;
    GtkWidget *scrolled, *page = new_page_column(&scrolled);

    GtkWidget *where = add_group(page, "Speichern");
    GtkWidget *choose = gtk_button_new_with_label("Ändern …");
    g_signal_connect(choose, "clicked", G_CALLBACK(on_choose_dir_clicked), NULL);
    add_row(where, "Speicherort", "", NULL, choose, &ui.dir_sub);
    GtkWidget *data_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    /* beschriftet: der Knopf verschiebt alle Daten, ein Rückgängig-Symbol wirkt zu harmlos */
    ui.data_reset = gtk_button_new_with_label("Standardordner");
    gtk_widget_set_tooltip_text(ui.data_reset, "Alle Programmdaten zurück in den Standardordner "
                                               "verschieben");
    gtk_widget_set_valign(ui.data_reset, GTK_ALIGN_CENTER);
    g_signal_connect(ui.data_reset, "clicked", G_CALLBACK(on_data_reset_clicked), NULL);
    GtkWidget *cache = gtk_button_new_with_label("Ändern …");
    gtk_widget_set_valign(cache, GTK_ALIGN_CENTER);
    g_signal_connect(cache, "clicked", G_CALLBACK(on_choose_cache_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(data_buttons), ui.data_reset, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(data_buttons), cache, FALSE, FALSE, 0);
    GtkWidget *cache_row = add_row(where, "Ordner für Programmdaten", "", NULL, data_buttons,
                                   &ui.cache_sub);
    gtk_widget_set_tooltip_text(cache_row, "Im gewählten Ordner entsteht dafür der Unterordner "
                                           "„Mediathek Downloader“.");
    quality_combo = new_setting_combo(QUALITY_TEXT, 3, c->quality);
    add_row(where, "Qualität", NULL, NULL, quality_combo, NULL);

    GtkWidget *av1 = add_group(page, "Platz sparen (AV1-Umwandlung)");
    av1_switch = new_setting_switch(c->convert);
    add_row(av1, "Nach dem Download verkleinern",
            "Wandelt ins Format AV1 um; das spart meist die Hälfte des Platzes bei gleichem Bild. "
            "Schwarze Balken im Bild werden abgeschnitten. Das Ergebnis wird geprüft; ist es nicht "
            "kleiner, bleibt die MP4-Datei.", NULL, av1_switch, NULL);
    encoder_combo = new_setting_combo(ENCODER_TEXT, 2, c->encoder);
    ui.encoder_row = add_row(av1, "Umwandlung mit", "Grafikkarte wird geprüft …", NULL,
                             encoder_combo, &ui.encoder_sub);
    ui.level_combo = gtk_combo_box_text_new();
    for (size_t i = 0; i < AV1_LEVEL_CUSTOM; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui.level_combo), AV1_LEVELS[i].text);
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui.level_combo), "Eigene Werte");
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui.level_combo), av1_level(c->gpu_quality, c->cpu_rf));
    gtk_widget_set_valign(ui.level_combo, GTK_ALIGN_CENTER);
    g_signal_connect(ui.level_combo, "changed", G_CALLBACK(on_setting_changed), NULL);
    ui.level_row = add_row(av1, "Bildqualität", "Bessere Qualität ergibt größere Dateien.", NULL,
                           ui.level_combo, NULL);
    gpu_spin = new_setting_spin(1, 255, c->gpu_quality);
    ui.gpu_row = add_row(av1, "Grafikkarte: Qualitätswert",
                         "Kleiner = besser und größer. „Sehr gut“ ist 64.", NULL, gpu_spin, NULL);
    cpu_spin = new_setting_spin(1, 63, c->cpu_rf);
    ui.cpu_row = add_row(av1, "Prozessor: Qualitätswert (RF)",
                         "Kleiner = besser und größer. „Sehr gut“ ist 35.", NULL, cpu_spin, NULL);
    keep_switch = new_setting_switch(c->keep_original);
    ui.keep_row = add_row(av1, "Original behalten",
                          "Sonst wird die MP4-Datei nach geprüfter Umwandlung gelöscht.", NULL,
                          keep_switch, NULL);

    GtkWidget *queue = add_group(page, "Warteschlange");
    parallel_spin = new_setting_spin(1, MD_MAX_DOWNLOADS, c->parallel);
    add_row(queue, "Gleichzeitige Downloads", NULL, NULL, parallel_spin, NULL);
    convert_spin = new_setting_spin(1, MD_MAX_CONVERSIONS, c->conversions);
    add_row(queue, "Gleichzeitige Umwandlungen", NULL, NULL, convert_spin, NULL);

    GtkWidget *search = add_group(page, "Anzeige");
    min_spin = new_setting_spin(0, 300, c->min_minutes);
    add_row(search, "Mindestlänge in Minuten", "Blendet Trailer und kurze Clips aus.", NULL,
            min_spin, NULL);
    signed_switch = new_setting_switch(c->show_signed);
    add_row(search, "Fassungen mit Gebärdensprache anzeigen",
            "Die Einblendung ist fest im Bild, darum sind das eigene Einträge.", NULL,
            signed_switch, NULL);
    audio_switch = new_setting_switch(c->hide_audio);
    add_row(search, "Reine Audio-Inhalte ausblenden",
            "Hörfassungen ohne Bildfassung daneben, Podcasts und Radio-Talks. Die "
            "Audiodeskription als Tonspur einer normalen Fassung bleibt wählbar.", NULL,
            audio_switch, NULL);

    GtkWidget *player_group = add_group(page, "Player");
    intro_switch = new_setting_switch(c->skip_intro);
    add_row(player_group, "Intros automatisch überspringen",
            "Der Vorspann einer Serie wird am Ton erkannt, den mehrere Folgen gemeinsam haben. "
            "Ausgeschaltet erscheint während des Intros der Knopf „Intro überspringen“. "
            "Den Abspann erkennt der Player genauso; er wird nie von selbst übersprungen, "
            "der Knopf „Abspann überspringen“ führt zur nächsten Folge.", NULL,
            intro_switch, NULL);
    recap_switch = new_setting_switch(c->skip_recap);
    add_row(player_group, "Rückblicke automatisch überspringen",
            "„Was bisher geschah“ wird an Ton oder Bild erkannt, die aus früheren Folgen stammen; "
            "dazu muss die vorige Folge geladen sein. Ausgeschaltet erscheint der Knopf "
            "„Rückblick überspringen“; folgt gleich das Intro, springt er auch darüber.", NULL,
            recap_switch, NULL);
    if (md_player_gpu_convert_possible()) {
        gpu_switch = new_setting_switch(c->gpu_convert);
        add_row(player_group, "Bild in der Grafikkarte umrechnen",
                "Entlastet den Prozessor und verhindert Ruckeln bei hoher Auflösung. Bei Bildfehlern "
                "ausschalten; gilt ab dem nächsten Öffnen des Players.", NULL, gpu_switch, NULL);
    }
    add_key_settings(page);

    GtkWidget *start = add_group(page, "Automatisch");
    GtkWidget *autostart = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(autostart), md_autostart_enabled());
    g_signal_connect(autostart, "notify::active", G_CALLBACK(on_autostart_changed), NULL);
    add_row(start, "Mit dem Computer starten",
            "Startet nach der Anmeldung minimiert.", NULL,
            autostart, NULL);
    news_switch = new_setting_switch(c->auto_news);
    add_row(start, "Neue Folgen automatisch laden",
            "Neue Folgen von Serien, von denen schon etwas geladen ist, kommen von selbst in die "
            "Warteschlange, mit allen Sprachen und Untertiteln. Gesucht wird beim Start und danach "
            "alle sechs Stunden.", NULL, news_switch, NULL);

    GtkWidget *updates = add_group(page, "Updates");
    update_switch = new_setting_switch(c->update_check);
    char *installed = g_strdup_printf("Installiert ist Version %s. Neue Versionen kommen von "
                                      "GitHub und werden vor dem Installieren geprüft.",
                                      MD_VERSION);
    /* eine Zeile statt zwei ähnlich klingender: Schalter für den Start, Knopf für jetzt */
    GtkWidget *update_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *check = gtk_button_new_with_label("Jetzt suchen");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(check), "app.check-update");
    gtk_widget_set_valign(check, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(update_switch, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(update_box), check, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(update_box), update_switch, FALSE, FALSE, 0);
    add_row(updates, "Beim Start nach Updates suchen", installed, NULL, update_box, NULL);
    g_free(installed);
    return scrolled;
}

/* GPU-Prüfung fertig (Hauptthread) */
static gboolean on_gpu_checked(gpointer d)
{
    ui.gpu_ok = d != NULL;
    set_label_text(ui.encoder_sub, ui.gpu_ok ? "Die Grafikkarte kann AV1. Empfohlen."
                   : "Die Grafikkarte kann kein AV1 – der Prozessor übernimmt, das dauert sehr viel "
                     "länger.");
    return G_SOURCE_REMOVE;
}

static gpointer gpu_check_thread(gpointer d)
{
    (void)d;
    g_idle_add(on_gpu_checked, md_gpu_available() ? GINT_TO_POINTER(1) : NULL);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Fenster, Menü, Programmstart
 * ------------------------------------------------------------------------- */
static int page_margin(int window_w)
{
    return CLAMP(window_w * 4 / 100, 16, 120);
}

/* Reiter als Symbole (der Name steht dann im Tooltip) oder als Text */
static void set_compact_tabs(bool compact)
{
    static const char *const ICONS[][2] = {
        {"vorschau", "view-grid-symbolic"},
        {"videos", "video-x-generic-symbolic"}, {"downloads", "folder-download-symbolic"},
        {"statistik", "x-office-spreadsheet-symbolic"},
        {"einstellungen", "emblem-system-symbolic"}};
    for (size_t i = 0; i < G_N_ELEMENTS(ICONS); i++) {
        GtkWidget *page = gtk_stack_get_child_by_name(GTK_STACK(ui.stack), ICONS[i][0]);
        if (page) gtk_container_child_set(GTK_CONTAINER(ui.stack), page, "icon-name",
                                          compact ? ICONS[i][1] : NULL, NULL);
    }
}

static gboolean on_idle_layout(gpointer d)
{
    (void)d;
    int w = ui.win ? gtk_widget_get_allocated_width(ui.win) : 0;
    if (w > 1 && w != ui.layout_width) {
        ui.layout_width = w;
        int margin = page_margin(w);
        for (int i = 0; i < ui.page_column_count; i++) {
            gtk_widget_set_margin_start(ui.page_columns[i], margin);
            gtk_widget_set_margin_end(ui.page_columns[i], margin);
        }
        for (int i = 0; i < FOOTER_COUNT; i++)
            gtk_widget_set_margin_end(ui.download_buttons[i], margin);
        set_compact_tabs(w < COMPACT_TABS_WIDTH);
    }
    return G_SOURCE_REMOVE;
}

static void on_window_size_allocate(GtkWidget *w, GdkRectangle *a, gpointer d)
{
    (void)w;
    (void)d;
    if (a->width != ui.layout_width) g_idle_add(on_idle_layout, NULL);
}

/* Fenstergröße merken */
static gboolean on_window_configure(GtkWidget *w, GdkEventConfigure *e, gpointer d)
{
    (void)e;
    (void)d;
    if (!ui.cfg.maximized) gtk_window_get_size(GTK_WINDOW(w), &ui.cfg.width, &ui.cfg.height);
    return FALSE;
}

static gboolean on_window_state(GtkWidget *w, GdkEventWindowState *e, gpointer d)
{
    (void)w;
    (void)d;
    ui.cfg.maximized = (e->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
    return FALSE;
}

/* Läuft oder wartet noch etwas? */
static int open_jobs(void)
{
    int count = 0, open = 0;
    MdJobView *jobs = md_queue_snapshot(&count);
    for (int i = 0; i < count; i++)
        open += jobs[i].state != MD_JOB_DONE && jobs[i].state != MD_JOB_FAILED && !jobs[i].paused;
    md_queue_snapshot_free(jobs, count);
    return open;
}

/* Fenster schließen: bei laufenden Downloads erst nachfragen */
static gboolean on_window_delete(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w;
    (void)e;
    (void)d;
    if (open_jobs() && !ui.quitting &&
        !ask_confirmation("Downloads laufen noch",
                          "Sie werden pausiert und lassen sich beim nächsten Start an derselben "
                          "Stelle fortsetzen.", "Weiterlaufen lassen", "Pausieren und beenden",
                          false))
        return TRUE;
    settings_save();
    return FALSE;
}

static gboolean on_close_after_signal(gpointer d)
{
    (void)d;
    if (ui.win) gtk_window_close(GTK_WINDOW(ui.win));
    return G_SOURCE_REMOVE;
}

/* SIGINT/SIGTERM/SIGHUP (z. B. beim Abmelden) oder Update: geordnet beenden. Ein offener Dialog
 * oder ein offenes Menü hält das Hauptfenster gesperrt und wird darum zuerst geschlossen. */
static gboolean on_quit_request(gpointer d)
{
    (void)d;
    ui.quitting = true;
    GList *windows = gtk_window_list_toplevels();
    for (GList *l = windows; l; l = l->next) {
        GtkWidget *child = gtk_bin_get_child(GTK_BIN(l->data));
        if (GTK_IS_DIALOG(l->data)) gtk_dialog_response(l->data, GTK_RESPONSE_CANCEL);
        else if (GTK_IS_MENU(child)) gtk_menu_shell_deactivate(GTK_MENU_SHELL(child));
    }
    g_list_free(windows);
    g_timeout_add(100, on_close_after_signal, NULL);
    return G_SOURCE_CONTINUE;
}

static void on_info_bar_response(GtkInfoBar *info_bar, gint response, gpointer d)
{
    (void)response;
    (void)info_bar;
    (void)d;
    show_info_bar(GTK_MESSAGE_INFO, NULL);
}

/* --- Updates ------------------------------------------------------------- */
enum { UPDATE_INSTALL = 1, UPDATE_NOTES, UPDATE_SKIP, UPDATE_RESTART };

typedef struct {
    bool manual;                /* über Menü oder Knopf: auch "aktuell" und Fehler melden */
    MdUpdate *update;
    char *error, *path;
} UpdateJob;

static void update_job_free(UpdateJob *job)
{
    md_update_free(job->update);
    g_free(job->error);
    g_free(job->path);
    g_free(job);
}

/* Leiste mit Text und passenden Knöpfen zeigen */
static void show_update_bar(GtkMessageType type, const char *text, bool can_install,
                            bool can_skip)
{
    gtk_info_bar_set_message_type(GTK_INFO_BAR(ui.update_bar), type);
    set_label_text(ui.update_label, text);
    gtk_widget_set_visible(ui.update_install, can_install);
    gtk_widget_set_visible(ui.update_notes, ui.update != NULL);
    gtk_widget_set_visible(ui.update_skip_button, can_skip);
    gtk_widget_show(ui.update_bar);
    gtk_info_bar_set_revealed(GTK_INFO_BAR(ui.update_bar), TRUE);
}

static gboolean on_update_checked(gpointer data)
{
    UpdateJob *job = data;
    ui.update_busy = false;
    if (job->update && (job->manual || g_strcmp0(job->update->version, ui.cfg.update_skip))) {
        md_update_free(ui.update);
        ui.update = g_steal_pointer(&job->update);
        /* ohne Datei für dieses System oder ohne Prüfsumme nur der Hinweis auf die Seite */
        bool installable = ui.update->file_url && ui.update->sums_url;
        char *text = g_strdup_printf("Version %s ist da (installiert ist %s).%s",
                                     ui.update->version, MD_VERSION, installable ? ""
                                     : " Sie lässt sich nur von der Release-Seite laden.");
        gtk_button_set_label(GTK_BUTTON(ui.update_install), "Installieren");
        show_update_bar(GTK_MESSAGE_INFO, text, installable, true);
        g_free(text);
    } else if (job->manual && job->error) {
        show_update_bar(GTK_MESSAGE_WARNING, job->error, false, false);
    } else if (job->manual) {
        char *text = g_strdup_printf("Version %s ist aktuell.", MD_VERSION);
        show_update_bar(GTK_MESSAGE_INFO, text, false, false);
        g_free(text);
    } else if (job->error) {
        md_log("Update-Suche: %s", job->error);
    }
    update_job_free(job);
    return G_SOURCE_REMOVE;
}

static gpointer update_check_thread(gpointer data)
{
    UpdateJob *job = data;
    job->update = md_update_check(&job->error);
    g_idle_add(on_update_checked, job);
    return NULL;
}

static void start_update_check(bool manual)
{
    if (ui.update_busy) return;
    ui.update_busy = true;
    UpdateJob *job = g_new0(UpdateJob, 1);
    job->manual = manual;
    g_thread_unref(g_thread_new("update", update_check_thread, job));
}

/* kurz nach dem Start: Fenster und Vorschau zuerst */
static gboolean on_update_timer(gpointer d)
{
    (void)d;
    if (ui.cfg.update_check) start_update_check(false);
    return G_SOURCE_REMOVE;
}

static void on_action_check_update(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    start_update_check(true);
}

#ifndef G_OS_WIN32
/* pkexec/apt-get fertig (Linux) */
static void on_update_installed(GPid pid, gint status, gpointer d)
{
    (void)d;
    g_spawn_close_pid(pid);
    ui.update_busy = false;
    if (g_spawn_check_wait_status(status, NULL)) {
        char *text = g_strdup_printf("Version %s ist installiert und gilt nach einem Neustart.",
                                     ui.update->version);
        gtk_button_set_label(GTK_BUTTON(ui.update_install), "Neu starten");
        show_update_bar(GTK_MESSAGE_INFO, text, true, false);
        g_free(text);
        md_update_free(ui.update);
        ui.update = NULL;
    } else {
        show_update_bar(GTK_MESSAGE_WARNING, "Die Installation wurde abgebrochen oder ist "
                        "fehlgeschlagen. Alternativ im Terminal: sudo apt update && sudo apt "
                        "upgrade", true, true);
    }
}
#endif

/* Datei geladen und geprüft: installieren */
static gboolean on_update_downloaded(gpointer data)
{
    UpdateJob *job = data;
    if (!job->path) {
        ui.update_busy = false;
        show_update_bar(GTK_MESSAGE_WARNING, job->error, true, true);
        update_job_free(job);
        return G_SOURCE_REMOVE;
    }
    char *error = NULL;
#ifdef G_OS_WIN32
    /* Der Installer wartet, bis dieses Programm beendet ist, und ersetzt dann die Dateien */
    char *argv[] = {job->path, "/UPDATE", NULL};
    if (md_launch_detached(argv, &error)) {
        on_quit_request(NULL);
    } else {
        ui.update_busy = false;
        show_update_bar(GTK_MESSAGE_WARNING, error, true, true);
    }
#else
    char *argv[] = {"pkexec", "apt-get", "install", "-y", job->path, NULL};
    GPid pid = 0;
    GError *err = NULL;
    if (g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD, NULL,
                      NULL, &pid, &err)) {
        set_label_text(ui.update_label, "Bitte die Installation bestätigen …");
        g_child_watch_add(pid, on_update_installed, NULL);
    } else {
        ui.update_busy = false;
        error = g_strdup_printf("pkexec fehlt. Im Terminal installieren: sudo apt install %s",
                                job->path);
        show_update_bar(GTK_MESSAGE_WARNING, error, false, true);
        g_clear_error(&err);
    }
#endif
    g_free(error);
    update_job_free(job);
    return G_SOURCE_REMOVE;
}

static gpointer update_download_thread(gpointer data)
{
    UpdateJob *job = data;
    job->path = md_update_download(job->update, &job->error);
    g_idle_add(on_update_downloaded, job);
    return NULL;
}

#ifndef G_OS_WIN32
/* Neu starten, sobald dieses Programm beendet ist (sonst übernimmt es den neuen Start) */
static void restart_after_update(void)
{
    char *script = g_strdup_printf("while kill -0 %d 2>/dev/null; do sleep 0.3; done; "
                                   "exec mediathek-downloader", (int)getpid());
    char *argv[] = {"sh", "-c", script, NULL};
    if (md_launch_detached(argv, NULL)) on_quit_request(NULL);
    g_free(script);
}
#endif

static void on_update_bar_response(GtkInfoBar *bar, gint response, gpointer d)
{
    (void)d;
    if (response == UPDATE_INSTALL && !ui.update) {
#ifndef G_OS_WIN32
        restart_after_update();
#endif
        return;
    }
    if (response == UPDATE_INSTALL && !ui.update_busy) {
        ui.update_busy = true;
        UpdateJob *job = g_new0(UpdateJob, 1);
        job->update = g_new0(MdUpdate, 1);
        *job->update = (MdUpdate){
            .version = g_strdup(ui.update->version), .file_name = g_strdup(ui.update->file_name),
            .file_url = g_strdup(ui.update->file_url), .sums_url = g_strdup(ui.update->sums_url),
            .file_size = ui.update->file_size};
        char *size = md_format_size((double)ui.update->file_size);
        char *text = g_strdup_printf("Version %s wird geladen (%s) …", ui.update->version, size);
        show_update_bar(GTK_MESSAGE_INFO, text, false, false);
        g_free(text);
        g_free(size);
        g_thread_unref(g_thread_new("update-laden", update_download_thread, job));
    } else if (response == UPDATE_NOTES && ui.update) {
        gtk_show_uri_on_window(GTK_WINDOW(ui.win), ui.update->page, GDK_CURRENT_TIME, NULL);
    } else if (response == UPDATE_SKIP && ui.update) {
        g_free(ui.cfg.update_skip);
        ui.cfg.update_skip = g_strdup(ui.update->version);
        settings_save();
        gtk_info_bar_set_revealed(bar, FALSE);
    } else if (response == GTK_RESPONSE_CLOSE) {
        gtk_info_bar_set_revealed(bar, FALSE);
    }
}

/* Leiste für Updates, getrennt von den übrigen Meldungen */
static GtkWidget *build_update_bar(void)
{
    ui.update_bar = gtk_info_bar_new();
    gtk_info_bar_set_show_close_button(GTK_INFO_BAR(ui.update_bar), TRUE);
    ui.update_install = gtk_info_bar_add_button(GTK_INFO_BAR(ui.update_bar), "Installieren",
                                                UPDATE_INSTALL);
    ui.update_notes = gtk_info_bar_add_button(GTK_INFO_BAR(ui.update_bar), "Was ist neu?",
                                              UPDATE_NOTES);
    ui.update_skip_button = gtk_info_bar_add_button(GTK_INFO_BAR(ui.update_bar),
                                                    "Überspringen", UPDATE_SKIP);
    gtk_widget_set_no_show_all(ui.update_install, TRUE);
    gtk_widget_set_no_show_all(ui.update_notes, TRUE);
    gtk_widget_set_no_show_all(ui.update_skip_button, TRUE);
    g_signal_connect(ui.update_bar, "response", G_CALLBACK(on_update_bar_response), NULL);
    ui.update_label = new_label("", NULL);
    set_wrapping(ui.update_label, 90);
    gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(ui.update_bar))),
                      ui.update_label);
    return ui.update_bar;
}

static void on_action_open_log(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    if (g_file_test(md_log_path(), G_FILE_TEST_IS_REGULAR)) open_in_default_app(md_log_path());
    else show_info_bar(GTK_MESSAGE_INFO, "Es gibt noch kein Protokoll.");
}

static void on_action_open_data(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    open_in_default_app(md_app_dir());
}

static void on_action_about(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    char *comments = g_strdup_printf(
        "Lädt Filme und Serien aus den Mediatheken von ZDF, ARD und weiteren für den privaten "
        "Gebrauch – mit Pause, Tempolimit, allen Sprachen und Untertiteln und auf Wunsch als AV1. "
        "Suche über MediathekViewWeb.\n"
        "Information courtesy of IMDb (https://www.imdb.com). Used with permission.\n"
        "Einstellungen, Verlauf und Protokoll: %s", md_app_dir());
    gtk_show_about_dialog(GTK_WINDOW(ui.win), "program-name", APP_NAME, "version", MD_VERSION,
                          "logo-icon-name", "mediathek-downloader", "comments", comments, NULL);
    g_free(comments);
}

static void on_action_quit(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    if (ui.win) gtk_window_close(GTK_WINDOW(ui.win));
}

/* Reiter in der Reihenfolge der Kopfzeile; Strg+1 … Strg+5 */
static const char *const PAGE_NAMES[] = {"vorschau", "videos", "downloads", "statistik",
                                         "einstellungen"};

static void on_action_page(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)d;
    gtk_stack_set_visible_child_name(GTK_STACK(ui.stack), g_variant_get_string(p, NULL));
}

/* Strg+Bild↑/↓: Reiter davor oder danach; im Player gilt der Reiter, aus dem er kam */
static void step_page(int step)
{
    const char *name = gtk_stack_get_visible_child_name(GTK_STACK(ui.stack));
    if (!g_strcmp0(name, "player") && ui.player_return)
        gtk_container_child_get(GTK_CONTAINER(ui.stack), ui.player_return, "name", &name, NULL);
    int count = G_N_ELEMENTS(PAGE_NAMES), at = 0;
    for (int i = 0; i < count; i++)
        if (!g_strcmp0(name, PAGE_NAMES[i])) at = i;
    gtk_stack_set_visible_child_name(GTK_STACK(ui.stack),
                                     PAGE_NAMES[(at + step + count) % count]);
}

static void on_action_next_page(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    step_page(1);
}

static void on_action_previous_page(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    step_page(-1);
}

/* Strg+F: in "Meine Videos" deren Suchfeld, sonst das der Vorschau */
static void on_action_find(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    bool videos = !g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(ui.stack)), "videos");
    GtkWidget *entry = videos ? ui.videos_search : ui.preview_search;
    if (!videos) gtk_stack_set_visible_child_name(GTK_STACK(ui.stack), "vorschau");
    if (videos && ui.open_show) on_show_back_clicked(NULL, NULL);  /* das Suchfeld ist oben */
    gtk_widget_grab_focus(entry);
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
}

/* F5: was die Seite neu laden kann */
static void on_action_reload(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    const char *name = gtk_stack_get_visible_child_name(GTK_STACK(ui.stack));
    if (!g_strcmp0(name, "vorschau")) on_preview_refresh_clicked(NULL, NULL);
    else if (!g_strcmp0(name, "videos")) {
        rebuild_videos(true);
        start_news_load();
    }
}

/* Liste "Taste – Wirkung" in einen Abschnitt des Dialogs */
static void add_key_section(GtkWidget *box, const char *title, const char *keys)
{
    GtkWidget *heading = new_label(title, "zd-heading");
    gtk_widget_set_margin_top(heading, 6);
    gtk_box_pack_start(GTK_BOX(box), heading, FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    char **lines = g_strsplit(keys, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char **parts = g_strsplit(lines[i], "\t", 2);
        if (parts[0] && parts[1]) {
            GtkWidget *key = new_label(parts[0], "zd-title");
            gtk_grid_attach(GTK_GRID(grid), key, 0, i, 1, 1);
            gtk_grid_attach(GTK_GRID(grid), new_label(parts[1], NULL), 1, i, 1, 1);
        }
        g_strfreev(parts);
    }
    g_strfreev(lines);
    gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
}

/* Dialog mit Überschrift und Inhalt; schließt mit "Schließen" oder Esc */
static void show_text_dialog(const char *title, GtkWidget *content)
{
    GtkWidget *d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(ui.win),
                                               GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                               "Schließen", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(d), 560, -1);
    GtkWidget *area = gtk_dialog_get_content_area(GTK_DIALOG(d));
    gtk_container_set_border_width(GTK_CONTAINER(content), 18);
    gtk_box_pack_start(GTK_BOX(area), content, TRUE, TRUE, 0);
    gtk_widget_show_all(d);
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
}

static void on_action_shortcuts(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    add_key_section(box, "Hauptfenster",
                    "Strg+1 … Strg+5\tReiter Vorschau, Meine Videos, Warteschlange, "
                    "Statistik, Einstellungen\n"
                    "Strg+Bild↓ / Strg+Bild↑\tNächster / voriger Reiter\n"
                    "Strg+F\tSuchfeld (in „Meine Videos“ deren eigenes)\n"
                    "Enter im Suchfeld der Vorschau\tIn allen Mediatheken suchen\n"
                    "F5\tSeite neu laden\n"
                    "F1\tDiese Übersicht\n"
                    "Strg+Q\tBeenden");
    add_key_section(box, "Player", md_player_keys_text());
    show_text_dialog("Tastenkürzel", box);
}

static void on_action_help(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    static const char *const STEPS[][2] = {
        {"1. Finden", "Die Vorschau zeigt die neuesten Filme und Serien aller Mediatheken. Tippen "
                      "filtert sie, Enter sucht in allen Mediatheken auch nach älteren Titeln."},
        {"2. Auswählen", "Das Häkchen auf einer Kachel wählt einen Film oder alle fehlenden Folgen "
                         "einer Serie; ein Klick auf die Kachel zeigt Staffeln und Folgen."},
        {"3. Laden", "„Herunterladen“ unten rechts stellt die Auswahl in die Warteschlange. Dort "
                     "lässt sich alles anhalten und fortsetzen, auch nach einem Neustart."},
        {"4. Schauen", "Fertiges steht unter „Meine Videos“ und spielt im eingebauten Player; "
                       "„?“ zeigt dort alle Tasten."},
        {"5. Dranbleiben", "Oben in „Meine Videos“ steht, was bei deinen Serien neu gesendet wurde. In den "
                           "Einstellungen lässt sich das auch automatisch laden."},
        {"Platz sparen", "In den Einstellungen wandelt „Platz sparen“ jedes Video nach dem "
                         "Download ins Format AV1 um, meist halb so groß."}};
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    for (size_t i = 0; i < G_N_ELEMENTS(STEPS); i++) {
        GtkWidget *heading = new_label(STEPS[i][0], "zd-heading");
        gtk_widget_set_margin_top(heading, i ? 10 : 0);
        gtk_box_pack_start(GTK_BOX(box), heading, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), set_wrapping(new_label(STEPS[i][1], NULL), 60), FALSE,
                           FALSE, 0);
    }
    show_text_dialog("Erste Schritte", box);
}

static void on_startup(GApplication *app, gpointer d)
{
    (void)d;
    /* GTK stellt das Zahlenformat auf die Landessprache um – die Engine braucht Punkte */
    setlocale(LC_NUMERIC, "C");
    static const GActionEntry actions[] = {
        {.name = "open-log", .activate = on_action_open_log},
        {.name = "open-data", .activate = on_action_open_data},
        {.name = "check-update", .activate = on_action_check_update},
        {.name = "about", .activate = on_action_about},
        {.name = "quit", .activate = on_action_quit},
        {.name = "page", .activate = on_action_page, .parameter_type = "s"},
        {.name = "next-page", .activate = on_action_next_page},
        {.name = "previous-page", .activate = on_action_previous_page},
        {.name = "find", .activate = on_action_find},
        {.name = "reload", .activate = on_action_reload},
        {.name = "shortcuts", .activate = on_action_shortcuts},
        {.name = "help", .activate = on_action_help},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), actions, G_N_ELEMENTS(actions), NULL);
    static const struct {
        const char *action, *keys[3];
    } ACCELS[] = {{"app.quit", {"<Control>q"}},
                  {"app.next-page", {"<Control>Page_Down"}},
                  {"app.previous-page", {"<Control>Page_Up"}},
                  {"app.find", {"<Control>f"}},
                  {"app.reload", {"F5"}},
                  {"app.shortcuts", {"F1", "<Control>question"}}};
    for (size_t i = 0; i < G_N_ELEMENTS(ACCELS); i++)
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), ACCELS[i].action,
                                              ACCELS[i].keys);
    for (size_t i = 0; i < G_N_ELEMENTS(PAGE_NAMES); i++) {
        char *action = g_strdup_printf("app.page::%s", PAGE_NAMES[i]);
        char *key = g_strdup_printf("<Control>%zu", i + 1);
        const char *keys[] = {key, NULL};
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), action, keys);
        g_free(action);
        g_free(key);
    }
}

static GtkWidget *build_menu_button(void)
{
    GMenu *menu = g_menu_new(), *help = g_menu_new(), *about = g_menu_new();
    g_menu_append(help, "Erste Schritte", "app.help");
    g_menu_append(help, "Tastenkürzel", "app.shortcuts");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(help));
    GMenu *files = g_menu_new();
    g_menu_append(files, "Protokoll öffnen", "app.open-log");
    g_menu_append(files, "Datenordner öffnen", "app.open-data");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(files));
    g_object_unref(files);
    g_menu_append(about, "Nach Updates suchen", "app.check-update");
    g_menu_append(about, "Über Mediathek Downloader", "app.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(about));
    g_object_unref(help);
    GtkWidget *menu_button = gtk_menu_button_new();
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button), G_MENU_MODEL(menu));
    gtk_button_set_image(GTK_BUTTON(menu_button),
                         gtk_image_new_from_icon_name("open-menu-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_widget_set_tooltip_text(menu_button, "Menü");
    g_object_unref(menu);
    g_object_unref(about);
    return menu_button;
}

/* Beim Start: Hinweis auf fehlende Programme und liegengebliebene Downloads */
static void show_start_notes(void)
{
    if (md_data_problem()) show_info_bar(GTK_MESSAGE_WARNING, md_data_problem());
    char *missing = NULL;
    if (!md_have_tools(&missing)) {
        char *text = g_strdup_printf("%s ist nicht installiert – bitte über die "
                                     "Anwendungsverwaltung nachinstallieren.", missing);
        show_info_bar(GTK_MESSAGE_ERROR, text);
        g_free(text);
        g_free(missing);
        return;
    }
    int count = 0, paused = 0;
    MdJobView *jobs = md_queue_snapshot(&count);
    for (int i = 0; i < count; i++) paused += jobs[i].paused;
    md_queue_snapshot_free(jobs, count);
    if (paused) {
        char *text = g_strdup_printf("%d %s vom letzten Mal – in der Warteschlange fortsetzen.",
                                     paused, plural(paused, "Download wartet", "Downloads warten"));
        show_info_bar(GTK_MESSAGE_INFO, text);
        g_free(text);
    }
}

static void on_activate(GtkApplication *app, gpointer d)
{
    (void)d;
    if (ui.win) {                                   /* zweiter Start: vorhandenes Fenster zeigen */
        gtk_window_present(GTK_WINDOW(ui.win));
        return;
    }
    settings_load();
    md_player_on_volume_changed(on_player_volume);
    apply_queue_settings();
    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css_provider, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(css_provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css_provider);

    ui.win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(ui.win), APP_NAME);
    gtk_window_set_default_size(GTK_WINDOW(ui.win), ui.cfg.width, ui.cfg.height);
    gtk_window_set_icon_name(GTK_WINDOW(ui.win), "mediathek-downloader");
    if (ui.cfg.maximized) gtk_window_maximize(GTK_WINDOW(ui.win));
    ui.header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(ui.header), TRUE);
    gtk_window_set_titlebar(GTK_WINDOW(ui.win), ui.header);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(ui.header), build_menu_button());
    ui.header_spinner = gtk_spinner_new();
    gtk_widget_set_tooltip_text(ui.header_spinner, "Suche nach neuen Folgen läuft");
    gtk_header_bar_pack_end(GTK_HEADER_BAR(ui.header), ui.header_spinner);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    ui.infobar = gtk_info_bar_new();
    gtk_info_bar_set_show_close_button(GTK_INFO_BAR(ui.infobar), TRUE);
    g_signal_connect(ui.infobar, "response", G_CALLBACK(on_info_bar_response), NULL);
    ui.info_label = new_label("", NULL);
    set_wrapping(ui.info_label, 90);
    gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(ui.infobar))),
                      ui.info_label);
    gtk_box_pack_start(GTK_BOX(vbox), ui.infobar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), build_update_bar(), FALSE, FALSE, 0);
    ui.stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(ui.stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(ui.stack), 180);
    gtk_stack_add_titled(GTK_STACK(ui.stack), build_preview_page(), "vorschau", "Vorschau");
    ui.videos_page = build_videos_page();
    gtk_stack_add_titled(GTK_STACK(ui.stack), ui.videos_page, "videos", "Meine Videos");
    ui.downloads_page = build_downloads_page();
    gtk_stack_add_titled(GTK_STACK(ui.stack), ui.downloads_page, "downloads", "Warteschlange");
    gtk_stack_add_titled(GTK_STACK(ui.stack), build_stats_page(), "statistik", "Statistik");
    gtk_stack_add_titled(GTK_STACK(ui.stack), build_settings_page(), "einstellungen",
                         "Einstellungen");
    gtk_box_pack_start(GTK_BOX(vbox), ui.stack, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(ui.win), vbox);
    GtkWidget *switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(ui.stack));
    gtk_header_bar_set_custom_title(GTK_HEADER_BAR(ui.header), switcher);
    /* schon vor dem ersten Zeigen, sonst machen die Reiter das Fenster breiter als gespeichert */
    set_compact_tabs(!ui.cfg.maximized && ui.cfg.width < COMPACT_TABS_WIDTH);

    g_signal_connect(ui.win, "delete-event", G_CALLBACK(on_window_delete), NULL);
    g_signal_connect(ui.win, "destroy", G_CALLBACK(on_main_window_destroy), NULL);
    g_signal_connect(ui.win, "configure-event", G_CALLBACK(on_window_configure), NULL);
    g_signal_connect(ui.win, "window-state-event", G_CALLBACK(on_window_state), NULL);
    g_signal_connect(ui.win, "size-allocate", G_CALLBACK(on_window_size_allocate), NULL);
    g_signal_connect(ui.stack, "notify::visible-child", G_CALLBACK(on_stack_page_changed), NULL);
    if (ui.start_minimized) gtk_window_iconify(GTK_WINDOW(ui.win));
    gtk_widget_show_all(ui.win);
    gtk_widget_hide(ui.infobar);
    gtk_widget_hide(ui.update_bar);
    gtk_widget_hide(ui.preview_spinner);
    gtk_widget_set_no_show_all(ui.preview_spinner, TRUE);
    gtk_widget_hide(ui.header_spinner);
    gtk_widget_set_no_show_all(ui.header_spinner, TRUE);
    update_dir_label();
    update_settings_rows();
    /* vor dem ersten Aufbau: "Meine Videos" fragt gleich nach Kategorien */
    ui.cover_pool = g_thread_pool_new(cover_worker, NULL, COVER_THREADS, FALSE, NULL);
    ui.imdb_pool = g_thread_pool_new(imdb_genres_worker, NULL, IMDB_THREADS, FALSE, NULL);
    g_thread_pool_set_sort_function(ui.imdb_pool, compare_genre_jobs, NULL);
    g_thread_pool_set_sort_function(ui.cover_pool, compare_cover_jobs, NULL);
    rebuild_results();
    rebuild_videos(true);
    if (!md_stats_load()) stats_import();   /* erst jetzt kennen die Dateien ihre Serie */
    show_start_notes();
    g_timeout_add(TICK_MS, on_tick, GINT_TO_POINTER(1));   /* 1 = Takt, darf auslassen */
    ui.film_limit = ui.series_limit = PREVIEW_TILES_BATCH;
    rebuild_preview();
    sync_genre_chips();
    sync_source_buttons();
    on_tick(NULL);
    start_preview_load(MD_CACHE_USE);
    g_thread_unref(g_thread_new("gpu", gpu_check_thread, NULL));
    g_timeout_add_seconds(UPDATE_DELAY_SECONDS, on_update_timer, NULL);
    g_timeout_add_seconds(NEWS_INTERVAL_SECONDS, on_news_timer, NULL);
    gtk_widget_grab_focus(ui.preview_search);
}

/* --autostart: minimiert starten */
static gint on_local_options(GApplication *app, GVariantDict *options, gpointer d)
{
    (void)app;
    (void)d;
    ui.start_minimized = g_variant_dict_contains(options, "autostart");
    return -1;                                      /* weiter wie üblich */
}

int main(int argc, char **argv)
{
#ifdef G_OS_UNIX
    signal(SIGPIPE, SIG_IGN);
#endif
    md_init(true);
    g_set_prgname("mediathek-downloader");
    g_set_application_name(APP_NAME);
    ui.app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(ui.app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(ui.app, "activate", G_CALLBACK(on_activate), NULL);
    g_application_add_main_option(G_APPLICATION(ui.app), "autostart", 0, G_OPTION_FLAG_NONE,
                                  G_OPTION_ARG_NONE, "Minimiert starten (Autostart)", NULL);
    g_signal_connect(ui.app, "handle-local-options", G_CALLBACK(on_local_options), NULL);
#ifdef G_OS_UNIX
    g_unix_signal_add(SIGINT, on_quit_request, NULL);
    g_unix_signal_add(SIGTERM, on_quit_request, NULL);
    g_unix_signal_add(SIGHUP, on_quit_request, NULL);
#endif
    int rc = g_application_run(G_APPLICATION(ui.app), argc, argv);
    if (volume_save_id) save_volume(NULL);   /* Lautstärke kurz vor dem Beenden geändert */
    md_shutdown();
    g_object_unref(ui.app);
    return rc;
}
