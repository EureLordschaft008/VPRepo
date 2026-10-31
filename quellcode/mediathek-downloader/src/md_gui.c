/*
 * Mediathek Downloader – grafische Oberfläche (GTK 3)
 * ===================================================
 * Aufbau und Optik wie beim AV1-Converter. Fünf Seiten, oben umschaltbar:
 *   Vorschau       Filme und Serien als Kacheln mit Bild; Häkchen sammeln eine Auswahl, die
 *                  Detailansicht zeigt Serie -> Staffel -> Folge mit dem Stand jeder Folge
 *   Neue Folgen    was bei schon geladenen Serien seit dem letzten Download dazugekommen ist
 *   Downloads      Warteschlange mit Pause, Fortsetzen und Tempolimit
 *   Bibliothek     alles, was schon geladen wurde, nach Sendung sortiert
 *   Einstellungen  Speicherort, Qualität, AV1
 *
 * Die Startliste läuft in einem eigenen Thread, das Ergebnis kommt über g_idle_add() in den
 * GTK-Hauptthread. Die Warteschlange arbeitet in der Engine (md_core.c); ein Timer holt alle
 * 500 ms den Stand ab. Der gesamte Zustand der Oberfläche liegt in der Struktur "ui".
 *
 * Aufbau dieser Datei: kleine Helfer – Einstellungen – Trefferbaum – Spurenauswahl –
 * Seite Vorschau – Seite Neue Folgen – Seite Downloads – Seite Bibliothek – Seite Einstellungen –
 * Fenster, Menü, Programmstart
 *
 * Aufruf:
 *     mediathek-downloader
 */
#define _GNU_SOURCE
#include <errno.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <signal.h>
#include <string.h>
#include <time.h>

#include "md_core.h"

#define APP_ID "de.mediathekdownloader.MediathekDownloader"
#define APP_NAME "Mediathek Downloader"
#define SETTINGS_GROUP "Einstellungen"
#define SETTINGS_FILE "einstellungen.ini"
#define RESULT_ROWS_BATCH 200   /* Zeilen der Trefferliste auf einmal (weitere per Knopf) */
#define TICK_MS 500
#define PREVIEW_TILES_BATCH 30  /* Kacheln je Abschnitt auf einmal (weitere per Knopf) */
#define PREVIEW_FILM_MIN_MINUTES 40
#define COVER_WIDTH 224
#define COVER_HEIGHT 126
#define COVER_THREADS 4
#define HOVER_DELAY_MS 500     /* so lange ruht die Maus auf der Kachel bis zum Popup */
#define GENRE_TICKS 4           /* neue Kategorien alle so vielen Timer-Schläge zeigen */
#define FOOTER_PREVIEW 0
#define FOOTER_NEWS 1
#define FOOTER_COUNT 2
#define NO_GENRE "Ohne Kategorie"
#define MD_SOURCE_ALL_1_4 7     /* so stand "Alle" früher in den Einstellungen */
#define MD_SOURCE_ALL_1_5 511
#define IMDB_THREADS 2
#define ALL_LANGUAGES "Alle Sprachen"
#define SOURCE_COUNT 10

static const int SOURCE_BITS[SOURCE_COUNT] = {MD_SOURCE_ZDF, MD_SOURCE_ZDFNEO, MD_SOURCE_ARD,
                                              MD_SOURCE_ONE, MD_SOURCE_WDR, MD_SOURCE_SWR,
                                              MD_SOURCE_NDR, MD_SOURCE_RBB, MD_SOURCE_3SAT,
                                              MD_SOURCE_NETZKINO};
static const char *const KIND_TEXT[3] = {"Alles", "Filme", "Serien"};
static const char *const QUALITY_TEXT[3] = {"Hoch – 1080p (ca. 4,5 GB je 90 min)",
                                            "Mittel (ca. 2,3 GB je 90 min)",
                                            "Niedrig (ca. 0,5 GB je 90 min)"};
static const char *const ENCODER_TEXT[2] = {"GPU – schnell (VAAPI)", "CPU – langsam (SVT-AV1)"};

typedef enum { ROW_GROUP, ROW_SEASON, ROW_ITEM } RowKind;

/* ein Treffer samt Stand */
typedef struct {
    MdTitle *title;
    char *path;                 /* vorhandene Datei */
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
    char *dir;
    int min_minutes;
    bool hide_variants;
    GPtrArray *items;           /* Item* */
} NewsJob;

/* Kategorien eines Titels von IMDb */
typedef struct {
    char *key, *name;
    bool is_film;
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

/* alle Folgen einer Serie im Hintergrund */
typedef struct {
    char *key, *topic, *dir, *error;
    int min_minutes, source;
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

/* IMDb-Bewertung im Hintergrund */
typedef struct {
    char *key, *name;
    bool is_film, found;
    MdImdbInfo info;
} RatingJob;

/* Zeile der Warteschlange */
typedef struct {
    guint id;
    GtkWidget *row, *title, *subtitle, *bar, *pause, *retry, *remove, *open;
    char *target;
    bool seen;
} JobRow;

typedef struct {
    char *dir, *cache_dir;
    int quality, encoder, gpu_quality, cpu_rf;
    int parallel, conversions, min_minutes;
    double limit_mb;
    int sources;                /* gewählte Mediatheken */
    bool limit_on, convert, keep_original, show_signed;
    int width, height;
    bool maximized;
} Config;

typedef struct {
    GtkApplication *app;
    GtkWidget *win, *header, *stack, *infobar, *info_label;
    Config cfg;
    int layout_width;
    GtkWidget *page_columns[8];
    int page_column_count;
    bool quitting, gpu_ok;
    guint inhibit_cookie;

    /* Neue Folgen */
    ResultTree news_tree;
    GtkWidget *news_page, *news_note, *news_spinner;
    bool news_busy;

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
    int film_limit, series_limit, series_loading;
    bool preview_busy, preview_refreshed;
    ResultTree detail_tree;     /* Liste der offenen Detailansicht */
    Group *detail_group;
    GtkWidget *detail_note, *detail_rating;
    GtkWidget *hover_popover, *hover_rating;
    Group *hover_group;
    guint hover_timer;

    /* Downloads */
    GtkWidget *downloads_page, *hero, *hero_sub, *jobs_list, *jobs_empty;
    GtkWidget *limit_switch, *limit_spin;
    GHashTable *job_rows;       /* id -> JobRow* */
    char *fingerprint;          /* erkennt Änderungen, die den Stand der Titel berühren */

    /* Bibliothek */
    GtkWidget *library_hero, *library_sub, *library_list, *library_search;
    GHashTable *library_open;   /* aufgeklappte Sendungen */

    /* Einstellungen */
    GtkWidget *dir_sub, *cache_sub, *encoder_row, *encoder_sub, *gpu_row, *cpu_row, *keep_row;
} UiState;

static UiState ui;

static void rebuild_results(void);
static void rebuild_tree(ResultTree *tree);
static void rebuild_preview(void);
static void start_news_load(void);
static void update_tiles(void);
static void update_genres(void);
static void rebuild_library(void);
static void update_selection(void);

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
    if (before && !gtk_list_box_row_get_header(this_row))
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

/* Meldungsleiste oben im Fenster (NULL schließt sie) */
static void show_info_bar(GtkMessageType type, const char *text)
{
    if (!text) {
        gtk_info_bar_set_revealed(GTK_INFO_BAR(ui.infobar), FALSE);
        return;
    }
    gtk_info_bar_set_message_type(GTK_INFO_BAR(ui.infobar), type);
    set_label_text(ui.info_label, text);
    gtk_widget_show(ui.infobar);
    gtk_info_bar_set_revealed(GTK_INFO_BAR(ui.infobar), TRUE);
}

/* Datei- und Ordnerpfad, der am Knopf hängt, öffnen */
static void on_open_file_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    open_in_default_app(g_object_get_data(G_OBJECT(b), "path"));
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
    ".zd-footer { border-top: 1px solid @borders; padding-top: 10px; padding-bottom: 12px; }\n";

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
    c->cache_dir = g_key_file_get_string(file, SETTINGS_GROUP, "Cacheordner", NULL);
    char *old_dir = g_build_filename(g_get_home_dir(), ".zdf-downloader", NULL);
    if (c->cache_dir && g_str_has_prefix(c->cache_dir, old_dir)) {   /* umgezogen, siehe md_init */
        char *moved = g_strconcat(md_app_dir(), c->cache_dir + strlen(old_dir), NULL);
        g_free(c->cache_dir);
        c->cache_dir = moved;
    }
    g_free(old_dir);
    if (!c->cache_dir || !*c->cache_dir) {
        g_free(c->cache_dir);
        c->cache_dir = md_default_cover_dir();
    }
    c->quality = settings_int(file, "Qualitaet", MD_QUALITY_HIGH, 0, 2);
    c->encoder = settings_int(file, "Encoder", MD_ENCODER_GPU, 0, 1);
    c->gpu_quality = settings_int(file, "GpuQualitaet", MD_DEFAULT_GPU_QUALITY, 1, 255);
    c->cpu_rf = settings_int(file, "CpuRf", MD_DEFAULT_CPU_RF, 1, 63);
    c->parallel = settings_int(file, "Downloads", 2, 1, MD_MAX_DOWNLOADS);
    c->conversions = settings_int(file, "Umwandlungen", 1, 1, MD_MAX_CONVERSIONS);
    c->min_minutes = settings_int(file, "Mindestlaenge", 10, 0, 300);
    c->limit_mb = CLAMP(settings_int(file, "LimitMBs", 10, 1, 1000), 1, 1000);
    c->limit_on = settings_bool(file, "LimitAktiv", false);
    c->sources = settings_int(file, "Mediatheken", MD_SOURCE_ALL, 1, MD_SOURCE_ALL);
    if (c->sources == MD_SOURCE_ALL_1_4 || c->sources == MD_SOURCE_ALL_1_5)
        c->sources = MD_SOURCE_ALL;
    c->convert = settings_bool(file, "AV1", false);
    c->keep_original = settings_bool(file, "OriginalBehalten", false);
    c->show_signed = settings_bool(file, "Gebaerdensprache", false);
    c->width = settings_int(file, "Breite", 980, 0, 10000);
    c->height = settings_int(file, "Hoehe", 800, 0, 10000);
    c->maximized = settings_bool(file, "Maximiert", false);
    g_key_file_free(file);
}

/* Einstellungen speichern (nach jeder Änderung) */
static void settings_save(void)
{
    const Config *c = &ui.cfg;
    GKeyFile *file = g_key_file_new();
    g_key_file_set_string(file, SETTINGS_GROUP, "Speicherort", c->dir);
    g_key_file_set_string(file, SETTINGS_GROUP, "Cacheordner", c->cache_dir);
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
    g_key_file_set_integer(file, SETTINGS_GROUP, "Breite", c->width);
    g_key_file_set_integer(file, SETTINGS_GROUP, "Hoehe", c->height);
    g_key_file_set_boolean(file, SETTINGS_GROUP, "Maximiert", c->maximized);
    char *path = settings_path();
    if (!g_key_file_save_to_file(file, path, NULL)) md_log("Einstellungen nicht gespeichert");
    g_free(path);
    g_key_file_free(file);
}

/* Tempolimit und Parallelität an die Engine geben */
static void apply_queue_settings(void)
{
    md_set_rate_limit(ui.cfg.limit_on ? (gint64)(ui.cfg.limit_mb * 1e6) : 0);
    md_set_parallel(ui.cfg.parallel, ui.cfg.conversions);
    md_set_cover_dir(ui.cfg.cache_dir);
}

/* ---------------------------------------------------------------------------
 * Trefferbaum
 * ------------------------------------------------------------------------- */
static void item_free(gpointer data)
{
    Item *item = data;
    md_title_free(item->title);
    g_free(item->path);
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
    GtkWidget *badge = status_badge(item->status);
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

/* Stand aller Treffer neu bestimmen, z. B. nach einem fertigen Download */
static bool refresh_groups(GPtrArray *groups)
{
    bool changed = false;
    for (guint g = 0; groups && g < groups->len; g++) {
        Group *group = groups->pdata[g];
        for (guint s = 0; s < group->seasons->len; s++) {
            Season *season = group->seasons->pdata[s];
            for (guint i = 0; i < season->items->len; i++) {
                Item *item = season->items->pdata[i];
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

static void refresh_statuses(void)
{
    bool changed = refresh_groups(ui.preview);
    changed |= refresh_groups(ui.news_tree.groups);
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
    for (guint i = 0; i < selected->len; i++) {
        Item *item = selected->pdata[i];
        md_queue_add(item->title, &options);
        item->selected = false;
    }
    g_strfreev(skip_audio);
    g_strfreev(skip_subtitles);
    g_ptr_array_free(selected, TRUE);
    refresh_statuses();
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
static GtkWidget *build_source_bar(void)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_top(bar, 6);
    GtkWidget *label = new_label("Mediathek", "dim-label");
    gtk_widget_set_margin_end(label, 6);
    gtk_box_pack_start(GTK_BOX(bar), label, FALSE, FALSE, 0);
    for (int i = 0; i <= SOURCE_COUNT; i++) {
        int bit = i < SOURCE_COUNT ? SOURCE_BITS[i] : MD_SOURCE_ALL;
        GtkWidget *b = gtk_toggle_button_new_with_label(i < SOURCE_COUNT ? md_source_label(bit)
                                                                         : "Alle");
        add_css_class(b, "zd-chip");
        g_signal_connect(b, "toggled", G_CALLBACK(on_source_toggled), GINT_TO_POINTER(bit));
        gtk_box_pack_start(GTK_BOX(bar), b, FALSE, FALSE, 0);
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

/* Bild im Hintergrund laden und verkleinern */
static gboolean on_cover_done(gpointer data)
{
    CoverJob *job = data;
    Group *group = ui.preview_index ? g_hash_table_lookup(ui.preview_index, job->key) : NULL;
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

static void imdb_genres_worker(gpointer data, gpointer d)
{
    (void)d;
    GenreJob *job = data;
    job->genres = md_imdb_genres(job->name, job->is_film);
    g_idle_add(on_imdb_genres_done, job);
}

static void request_cover(Group *group)
{
    if (group->cover_requested || !group->website) return;
    group->cover_requested = true;
    CoverJob *job = g_new0(CoverJob, 1);
    job->key = g_strdup(group->key);
    job->website = g_strdup(group->website);
    job->video = g_strdup(group->video);
    g_thread_pool_push(ui.cover_pool, job, NULL);
}

/* sichtbare Kacheln zuerst */
static gint compare_cover_jobs(gconstpointer a, gconstpointer b, gpointer d)
{
    (void)d;
    return (int)((const CoverJob *)a)->background - (int)((const CoverJob *)b)->background;
}

/* Kategorien aller Titel im Hintergrund ermitteln */
static void request_all_genres(void)
{
    for (guint i = 0; i < ui.preview->len; i++) {
        const Group *group = ui.preview->pdata[i];
        GenreJob *imdb = g_new0(GenreJob, 1);
        imdb->key = g_strdup(group->key);
        imdb->name = g_strdup(group->name);
        imdb->is_film = group->is_film;
        ui.genre_pending++;
        g_thread_pool_push(ui.imdb_pool, imdb, NULL);
        if (group->genres || !group->website) continue;
        CoverJob *job = g_new0(CoverJob, 1);
        job->key = g_strdup(group->key);
        job->website = g_strdup(group->website);
        job->background = true;
        ui.genre_pending++;
        g_thread_pool_push(ui.cover_pool, job, NULL);
    }
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
        badge = status_badge(((Item *)season->items->pdata[0])->status);
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

static gpointer series_thread(gpointer data)
{
    SeriesJob *job = data;
    MdQuery query = {.text = job->topic, .kind = MD_KIND_ALL, .sources = job->source,
                     .min_minutes = job->min_minutes,
                     .hide_variants = job->hide_variants, .cache = job->cache};
    GPtrArray *titles = md_search(&query, NULL, NULL, &job->error);
    if (titles) {
        /* die Suche findet das Wort auch in fremden Sendungen */
        for (guint i = titles->len; i-- > 0;)
            if (strcmp(((MdTitle *)titles->pdata[i])->topic, job->topic))
                g_ptr_array_remove_index(titles, i);
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
    job->source = group->source;
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
    char *filter = g_utf8_strdown(gtk_entry_get_text(GTK_ENTRY(ui.preview_search)), -1);
    int films = 0, series = 0;
    for (guint i = 0; ui.preview && i < ui.preview->len; i++) {
        Group *group = ui.preview->pdata[i];
        if (!tile_matches(group, filter)) continue;
        int *count = group->is_film ? &films : &series;
        if (*count < (group->is_film ? ui.film_limit : ui.series_limit))
            add_tile(group->is_film ? ui.film_box : ui.series_box, group);
        (*count)++;
    }
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
        rebuild_library();
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
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(*box), 10);
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
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui.preview_search), "In der Vorschau filtern …");
    g_signal_connect(ui.preview_search, "search-changed", G_CALLBACK(on_preview_filter_changed),
                     NULL);
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
 * Seite Neue Folgen
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

static gboolean on_news_done(gpointer data)
{
    NewsJob *job = data;
    if (ui.news_tree.groups) g_ptr_array_free(ui.news_tree.groups, TRUE);
    ui.news_tree.groups = group_items(job->items);
    g_ptr_array_set_free_func(job->items, NULL);            /* gehören jetzt den Staffeln */
    for (guint g = 0; g < ui.news_tree.groups->len; g++) {
        ((Group *)ui.news_tree.groups->pdata[g])->expanded = true;
    }
    int series = (int)ui.news_tree.groups->len;
    char *title = series ? g_strdup_printf("Neue Folgen (%d)", series) : g_strdup("Neue Folgen");
    char *note = series ? g_strdup_printf("%d %s mit neuen Folgen seit dem letzten Download",
                                          series, plural(series, "Serie", "Serien"))
                        : g_strdup("Alle geladenen Serien sind aktuell.");
    gtk_container_child_set(GTK_CONTAINER(ui.stack), ui.news_page, "title", title, NULL);
    set_label_text(ui.news_note, note);
    g_free(title);
    g_free(note);
    ui.news_tree.empty_text = "Keine neuen Folgen.";
    ui.news_busy = false;
    gtk_spinner_stop(GTK_SPINNER(ui.news_spinner));
    gtk_widget_hide(ui.news_spinner);
    rebuild_tree(&ui.news_tree);
    g_ptr_array_free(job->items, TRUE);
    g_strfreev(job->topics);
    g_free(job->finished);
    g_free(job->dir);
    g_free(job);
    return G_SOURCE_REMOVE;
}

static gpointer news_thread(gpointer data)
{
    NewsJob *job = data;
    job->items = g_ptr_array_new_with_free_func(item_free);
    for (int i = 0; job->topics[i]; i++) {
        MdQuery query = {.text = job->topics[i], .kind = MD_KIND_ALL, .sources = MD_SOURCE_ALL,
                         .cache = MD_CACHE_USE, .min_minutes = job->min_minutes,
                         .hide_variants = job->hide_variants};
        char *error = NULL;
        GPtrArray *titles = md_search(&query, NULL, NULL, &error);
        g_free(error);
        if (!titles) continue;
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
    g_hash_table_destroy(latest);
    md_history_snapshot_free(entries, count);
    job->dir = g_strdup(ui.cfg.dir);
    job->min_minutes = ui.cfg.min_minutes;
    job->hide_variants = !ui.cfg.show_signed;
    ui.news_tree.empty_text = "Wird geprüft …";
    gtk_widget_show(ui.news_spinner);
    gtk_spinner_start(GTK_SPINNER(ui.news_spinner));
    g_thread_unref(g_thread_new("neue-folgen", news_thread, job));
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

static GtkWidget *build_news_page(void)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *top = new_column();
    gtk_widget_set_margin_top(top, 14);
    gtk_widget_set_margin_bottom(top, 8);
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    ui.news_note = new_label("Wird geprüft …", "zd-sub");
    add_css_class(ui.news_note, "dim-label");
    gtk_box_pack_start(GTK_BOX(bar), ui.news_note, TRUE, TRUE, 0);
    ui.news_spinner = gtk_spinner_new();
    gtk_box_pack_start(GTK_BOX(bar), ui.news_spinner, FALSE, FALSE, 0);
    GtkWidget *select = gtk_button_new_with_label("Alle neuen Folgen an/aus");
    g_signal_connect(select, "clicked", G_CALLBACK(on_news_select_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(bar), select, FALSE, FALSE, 0);
    GtkWidget *refresh = new_icon_button("view-refresh-symbolic", "Neue Folgen prüfen");
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_news_refresh_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(bar), refresh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), top, FALSE, FALSE, 0);

    GtkWidget *scrolled, *column = new_page_column(&scrolled);
    gtk_widget_set_margin_top(column, 4);
    ui.news_tree.list = add_group(column, NULL);
    ui.news_tree.limit = RESULT_ROWS_BATCH;
    g_signal_connect(ui.news_tree.list, "row-activated", G_CALLBACK(on_result_row_activated),
                     &ui.news_tree);
    gtk_box_pack_start(GTK_BOX(page), scrolled, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(page), build_footer(FOOTER_NEWS), FALSE, FALSE, 0);
    return page;
}

/* ---------------------------------------------------------------------------
 * Seite Downloads
 * ------------------------------------------------------------------------- */
static void job_row_free(gpointer data)
{
    JobRow *row = data;
    g_free(row->target);
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
    md_job_remove(row->id);
}

static void on_job_open_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    open_folder_of(((JobRow *)d)->target);
}

/* Zeile der Warteschlange anlegen */
static JobRow *new_job_row(const MdJobView *j)
{
    JobRow *row = g_new0(JobRow, 1);
    row->id = j->id;
    row->target = g_strdup(j->target);
    row->row = gtk_list_box_row_new();
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

/* Zeile auf den aktuellen Stand bringen */
static void job_row_update(JobRow *row, const MdJobView *j)
{
    bool done = j->state == MD_JOB_DONE, failed = j->state == MD_JOB_FAILED;
    bool open = !done && !failed;
    char *status = job_status_text(j);
    set_label_text(row->subtitle, status);
    g_free(status);
    g_object_set_data(G_OBJECT(row->row), "open", GINT_TO_POINTER(open));

    gtk_widget_set_visible(row->bar, open);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(row->bar), CLAMP(j->progress, 0, 1));
    gtk_widget_set_visible(row->pause, open);
    gtk_widget_set_visible(row->retry, failed);
    gtk_widget_set_visible(row->open, done);
    gtk_widget_set_visible(row->remove, true);
    gtk_widget_set_tooltip_text(row->remove, open ? "Abbrechen" : "Aus der Liste entfernen");
    gtk_widget_set_tooltip_text(row->pause, j->paused ? "Fortsetzen" : "Pausieren");
    gtk_button_set_image(GTK_BUTTON(row->pause),
                         gtk_image_new_from_icon_name(j->paused ? "media-playback-start-symbolic"
                                                                : "media-playback-pause-symbolic",
                                                      GTK_ICON_SIZE_BUTTON));
    g_object_set_data(G_OBJECT(row->pause), "paused", j->paused ? row : NULL);
}

/* Kopfzeile: was läuft gerade? */
static void update_downloads_hero(const MdJobView *jobs, int count)
{
    int active = 0, open = 0, paused = 0, failed = 0, done = 0;
    double speed = 0, remaining = 0;
    for (int i = 0; i < count; i++) {
        const MdJobView *j = &jobs[i];
        done += j->state == MD_JOB_DONE;
        failed += j->state == MD_JOB_FAILED;
        if (j->state == MD_JOB_DONE || j->state == MD_JOB_FAILED) continue;
        open++;
        if (j->paused) paused++;
        else if (j->state == MD_JOB_DOWNLOADING || j->state == MD_JOB_CONVERTING) active++;
        if (j->state == MD_JOB_DOWNLOADING) {
            speed += j->speed;
            remaining = MAX(remaining, j->remaining);
        }
    }
    char *title;
    if (active)
        title = g_strdup_printf("%d %s aktiv", active, plural(active, "Auftrag", "Aufträge"));
    else if (paused) title = g_strdup_printf("%d pausiert", paused);
    else if (open) title = g_strdup_printf("%d %s", open, plural(open, "wartet", "warten"));
    else title = g_strdup(count ? "Alles erledigt" : "Keine Downloads");
    set_label_text(ui.hero, title);
    g_free(title);

    GString *sub = g_string_new(NULL);
    if (speed > 0) {
        char *text = md_format_size(speed), *rest = md_format_time(remaining);
        g_string_append_printf(sub, "%s/s · noch %s · ", text, rest);
        g_free(text);
        g_free(rest);
    }
    g_string_append_printf(sub, "%d offen · %d fertig", open, done);
    if (failed) g_string_append_printf(sub, " · %d fehlgeschlagen", failed);
    set_label_text(ui.hero_sub, sub->str);
    g_string_free(sub, TRUE);

    char *page = open ? g_strdup_printf("Downloads (%d)", open) : g_strdup("Downloads");
    gtk_container_child_set(GTK_CONTAINER(ui.stack), ui.downloads_page, "title", page, NULL);
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

/* Timer: Warteschlange anzeigen; bei fertigen oder neuen Aufträgen auch den Stand der Titel */
static gboolean on_tick(gpointer d)
{
    (void)d;
    int count = 0;
    MdJobView *jobs = md_queue_snapshot(&count);
    GString *mark = g_string_new(NULL);
    for (int i = 0; i < count; i++) {
        const MdJobView *j = &jobs[i];
        JobRow *row = g_hash_table_lookup(ui.job_rows, GUINT_TO_POINTER(j->id));
        if (!row) {
            row = new_job_row(j);
            g_hash_table_insert(ui.job_rows, GUINT_TO_POINTER(j->id), row);
        }
        row->seen = true;
        job_row_update(row, j);
        g_string_append_printf(mark, "%u:%d,", j->id, j->state >= MD_JOB_DONE ? j->state : 0);
    }
    g_hash_table_foreach_remove(ui.job_rows, job_row_stale, NULL);
    gtk_widget_set_visible(ui.jobs_empty, count == 0);
    update_downloads_hero(jobs, count);
    md_queue_snapshot_free(jobs, count);
    if (g_strcmp0(mark->str, ui.fingerprint)) {
        g_free(ui.fingerprint);
        ui.fingerprint = g_strdup(mark->str);
        refresh_statuses();
        rebuild_library();
    }
    g_string_free(mark, TRUE);
    update_genres();
    return G_SOURCE_CONTINUE;
}

static void on_pause_all_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    md_queue_set_paused(true);
}

static void on_resume_all_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    md_queue_set_paused(false);
    show_info_bar(GTK_MESSAGE_INFO, NULL);
}

static void on_clear_done_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    md_queue_remove_finished();
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
    GtkWidget *pause = gtk_button_new_with_label("Alle pausieren");
    GtkWidget *resume = gtk_button_new_with_label("Alle fortsetzen");
    GtkWidget *clear = gtk_button_new_with_label("Fertige entfernen");
    g_signal_connect(pause, "clicked", G_CALLBACK(on_pause_all_clicked), NULL);
    g_signal_connect(resume, "clicked", G_CALLBACK(on_resume_all_clicked), NULL);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_done_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(buttons), pause, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons), resume, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), buttons, FALSE, FALSE, 0);

    GtkWidget *limit = add_group(page, "Tempo");
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
 * Seite Bibliothek
 * ------------------------------------------------------------------------- */
/* Verlaufseintrag entfernen, wenn die Datei weg ist */
static void on_forget_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    md_history_forget(g_object_get_data(G_OBJECT(b), "path"));
    refresh_statuses();
    rebuild_library();
}

/* Serie in der Vorschau zeigen */
static void on_find_missing_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    gtk_entry_set_text(GTK_ENTRY(ui.preview_search), g_object_get_data(G_OBJECT(b), "path"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui.preview_kind), MD_KIND_ALL);
    gtk_stack_set_visible_child_name(GTK_STACK(ui.stack), "vorschau");
}

static void on_library_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer d)
{
    (void)list;
    (void)d;
    const char *topic = g_object_get_data(G_OBJECT(row), "topic");
    if (!topic) return;
    if (!g_hash_table_remove(ui.library_open, topic))
        g_hash_table_add(ui.library_open, g_strdup(topic));
    rebuild_library();
}

/* Überschrift einer Sendung */
static void add_library_topic(const char *topic, int count, int missing, gint64 bytes)
{
    bool expanded = g_hash_table_contains(ui.library_open, topic);
    GtkWidget *arrow = gtk_image_new_from_icon_name(expanded ? "pan-down-symbolic"
                                                             : "pan-end-symbolic",
                                                    GTK_ICON_SIZE_MENU);
    char *size = md_format_size((double)bytes);
    char *subtitle = g_strdup_printf("%d %s · %s", count, plural(count, "Titel", "Titel"), size);
    GtkWidget *suffix = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    if (missing) {
        char *text = g_strdup_printf("%d %s", missing,
                                     plural(missing, "Datei fehlt", "Dateien fehlen"));
        GtkWidget *badge = new_badge(text, "err");
        gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(suffix), badge, FALSE, FALSE, 0);
        g_free(text);
    }
    char *lower = g_utf8_strdown(topic, -1);
    char *key = g_strdup_printf("S:%s", lower);
    const Group *series = ui.preview_index ? g_hash_table_lookup(ui.preview_index, key) : NULL;
    if (series && series->has_news) {
        GtkWidget *news = new_news_badge();
        gtk_widget_set_valign(news, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(suffix), news, FALSE, FALSE, 0);
    }
    g_free(lower);
    g_free(key);
    if (strcmp(topic, "Filme"))
        gtk_box_pack_start(GTK_BOX(suffix),
                           new_path_button("edit-find-symbolic", "In der Vorschau zeigen", topic,
                                           G_CALLBACK(on_find_missing_clicked)), FALSE, FALSE, 0);
    GtkWidget *row = add_row(ui.library_list, topic, subtitle, arrow, suffix, NULL);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    g_object_set_data_full(G_OBJECT(row), "topic", g_strdup(topic), g_free);
    g_free(size);
    g_free(subtitle);
}

/* Zeile eines geladenen Titels */
static void add_library_entry(const MdHistoryEntry *e)
{
    GtkWidget *suffix = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    if (e->exists) {
        gtk_box_pack_start(GTK_BOX(suffix),
                           new_path_button("media-playback-start-symbolic", "Abspielen", e->path,
                                           G_CALLBACK(on_open_file_clicked)), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(suffix),
                           new_path_button("folder-open-symbolic", "Ordner öffnen", e->path,
                                           G_CALLBACK(on_open_folder_clicked)), FALSE, FALSE, 0);
    } else {
        GtkWidget *badge = new_badge("Datei fehlt", "err");
        gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
        gtk_box_pack_start(GTK_BOX(suffix), badge, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(suffix),
                           new_path_button("edit-clear-symbolic", "Aus dem Verlauf entfernen",
                                           e->path, G_CALLBACK(on_forget_clicked)),
                           FALSE, FALSE, 0);
    }
    char *date = format_date(e->finished), *size = md_format_size((double)e->size);
    char *subtitle = g_strdup_printf("%s · geladen am %s", size, date);
    GtkWidget *row = add_row(ui.library_list, e->title, subtitle, NULL, suffix, NULL);
    gtk_widget_set_margin_start(gtk_bin_get_child(GTK_BIN(row)), 40);
    gtk_widget_set_tooltip_text(row, e->path);
    g_free(date);
    g_free(size);
    g_free(subtitle);
}

/* Trifft der Filter der Bibliothek auf diesen Eintrag zu? */
static bool library_matches(const MdHistoryEntry *e, const char *filter)
{
    if (!*filter) return true;
    char *text = g_strconcat(e->topic, " ", e->title, NULL), *lower = g_utf8_strdown(text, -1);
    bool match = strstr(lower, filter) != NULL;
    g_free(text);
    g_free(lower);
    return match;
}

/* Bibliothek aus dem Verlauf neu aufbauen */
static void rebuild_library(void)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(ui.library_list));
    for (GList *l = children; l; l = l->next) gtk_widget_destroy(l->data);
    g_list_free(children);

    int count = 0, shown = 0, gone = 0;
    gint64 total = 0;
    MdHistoryEntry *entries = md_history_snapshot(&count);
    char *filter = g_utf8_strdown(gtk_entry_get_text(GTK_ENTRY(ui.library_search)), -1);
    for (int i = 0; i < count; ) {
        int end = i, matches = 0, missing = 0;
        gint64 bytes = 0;
        while (end < count && !strcmp(entries[end].topic, entries[i].topic)) {
            bool match = library_matches(&entries[end], filter);
            matches += match;
            missing += match && !entries[end].exists;
            bytes += match && entries[end].exists ? entries[end].size : 0;
            end++;
        }
        if (matches) add_library_topic(entries[i].topic, matches, missing, bytes);
        bool expanded = *filter || g_hash_table_contains(ui.library_open, entries[i].topic);
        for (int n = i; n < end && expanded; n++)
            if (library_matches(&entries[n], filter)) add_library_entry(&entries[n]);
        shown += matches;
        gone += missing;
        total += bytes;
        i = end;
    }
    if (!shown) {
        GtkWidget *label = new_label(count ? "Nichts gefunden." : "Noch nichts geladen.",
                                     "dim-label");
        gtk_label_set_xalign(GTK_LABEL(label), 0.5);
        gtk_widget_set_margin_top(label, 16);
        gtk_widget_set_margin_bottom(label, 16);
        gtk_container_add(GTK_CONTAINER(ui.library_list), label);
    }
    char *size = md_format_size((double)total);
    char *hero = g_strdup_printf("%d %s geladen", shown, plural(shown, "Titel", "Titel"));
    char *sub = gone ? g_strdup_printf("%s · bei %d %s fehlt die Datei", size, gone,
                                       plural(gone, "Titel", "Titeln"))
                     : g_strdup_printf("%s auf dem Laufwerk", size);
    set_label_text(ui.library_hero, hero);
    set_label_text(ui.library_sub, sub);
    g_free(size);
    g_free(hero);
    g_free(sub);
    g_free(filter);
    md_history_snapshot_free(entries, count);
    gtk_widget_show_all(ui.library_list);
}

static void on_library_search_changed(GtkSearchEntry *e, gpointer d)
{
    (void)e;
    (void)d;
    rebuild_library();
}

static GtkWidget *build_library_page(void)
{
    GtkWidget *scrolled, *page = new_page_column(&scrolled);
    ui.library_hero = new_label("", "zd-hero");
    ui.library_sub = new_label("", "zd-lead");
    add_css_class(ui.library_sub, "dim-label");
    gtk_box_pack_start(GTK_BOX(page), ui.library_hero, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), ui.library_sub, FALSE, FALSE, 0);
    ui.library_search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ui.library_search), "In der Bibliothek suchen …");
    gtk_widget_set_margin_top(ui.library_search, 10);
    g_signal_connect(ui.library_search, "search-changed", G_CALLBACK(on_library_search_changed),
                     NULL);
    gtk_box_pack_start(GTK_BOX(page), ui.library_search, FALSE, FALSE, 0);
    ui.library_list = add_group(page, "Nach Sendung");
    g_signal_connect(ui.library_list, "row-activated", G_CALLBACK(on_library_row_activated), NULL);
    ui.library_open = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    return scrolled;
}

/* ---------------------------------------------------------------------------
 * Seite Einstellungen
 * ------------------------------------------------------------------------- */
/* Zeilen je nach AV1-Schalter und Encoder ein- oder ausgrauen */
static void update_settings_rows(void)
{
    gtk_widget_set_sensitive(ui.encoder_row, ui.cfg.convert);
    gtk_widget_set_sensitive(ui.keep_row, ui.cfg.convert);
    gtk_widget_set_sensitive(ui.gpu_row, ui.cfg.convert && ui.cfg.encoder == MD_ENCODER_GPU);
    gtk_widget_set_sensitive(ui.cpu_row, ui.cfg.convert && ui.cfg.encoder == MD_ENCODER_CPU);
}

static void update_dir_label(void)
{
    char *display = g_filename_display_name(ui.cfg.dir);
    set_label_text(ui.dir_sub, display);
    g_free(display);
    display = g_filename_display_name(ui.cfg.cache_dir);
    set_label_text(ui.cache_sub, display);
    g_free(display);
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
    refresh_statuses();
    update_selection();
}

static void on_choose_cache_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    char *path = choose_folder("Ordner für Zwischenspeicher wählen", ui.cfg.cache_dir);
    if (!path) return;
    g_free(ui.cfg.cache_dir);
    ui.cfg.cache_dir = path;
    update_dir_label();
    apply_queue_settings();
    settings_save();
}

/* gemeinsamer Handler aller übrigen Einstellungen: alles einlesen und speichern */
static GtkWidget *quality_combo, *encoder_combo, *gpu_spin, *cpu_spin, *parallel_spin;
static GtkWidget *convert_spin, *av1_switch, *keep_switch, *signed_switch, *min_spin;

static void on_setting_changed(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
    Config *c = &ui.cfg;
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
    apply_queue_settings();
    settings_save();
    update_settings_rows();
    if (c->quality != old_quality) rebuild_results();   /* Größen der Treffer */
    update_selection();
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
    GtkWidget *cache = gtk_button_new_with_label("Ändern …");
    g_signal_connect(cache, "clicked", G_CALLBACK(on_choose_cache_clicked), NULL);
    GtkWidget *cache_row = add_row(where, "Ordner für Zwischenspeicher", "", NULL, cache,
                                   &ui.cache_sub);
    gtk_widget_set_tooltip_text(cache_row, "Hier liegen Titelbilder und Kategorien der Vorschau.");
    quality_combo = new_setting_combo(QUALITY_TEXT, 3, c->quality);
    add_row(where, "Qualität", NULL, NULL, quality_combo, NULL);

    GtkWidget *av1 = add_group(page, "AV1");
    av1_switch = new_setting_switch(c->convert);
    add_row(av1, "Nach dem Download nach AV1 umwandeln",
            "Spart meist die Hälfte des Platzes. Das Ergebnis wird geprüft; ist es nicht kleiner, "
            "bleibt die MP4-Datei.", NULL, av1_switch, NULL);
    encoder_combo = new_setting_combo(ENCODER_TEXT, 2, c->encoder);
    ui.encoder_row = add_row(av1, "Encoder", "GPU wird geprüft …", NULL, encoder_combo,
                             &ui.encoder_sub);
    gpu_spin = new_setting_spin(1, 255, c->gpu_quality);
    ui.gpu_row = add_row(av1, "GPU-Qualität", "Kleiner = besser und größer. Standard 64.", NULL,
                         gpu_spin, NULL);
    cpu_spin = new_setting_spin(1, 63, c->cpu_rf);
    ui.cpu_row = add_row(av1, "CPU-Qualität (RF)", "Kleiner = besser und größer. Standard 35.",
                         NULL, cpu_spin, NULL);
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
    return scrolled;
}

/* GPU-Prüfung fertig (Hauptthread) */
static gboolean on_gpu_checked(gpointer d)
{
    ui.gpu_ok = d != NULL;
    set_label_text(ui.encoder_sub, ui.gpu_ok ? "GPU-Encoder gefunden. Empfohlen."
                   : "Kein GPU-Encoder gefunden – die CPU übernimmt, das dauert sehr viel länger.");
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

/* SIGINT/SIGTERM/SIGHUP (z. B. beim Abmelden): geordnet beenden. Ein offener Dialog hält das
 * Hauptfenster gesperrt und wird darum zuerst geschlossen. */
static gboolean on_unix_signal(gpointer d)
{
    (void)d;
    ui.quitting = true;
    GList *windows = gtk_window_list_toplevels();
    for (GList *l = windows; l; l = l->next)
        if (GTK_IS_DIALOG(l->data)) gtk_dialog_response(l->data, GTK_RESPONSE_CANCEL);
    g_list_free(windows);
    g_timeout_add(100, on_close_after_signal, NULL);
    return G_SOURCE_CONTINUE;
}

static void on_info_bar_response(GtkInfoBar *info_bar, gint response, gpointer d)
{
    (void)response;
    (void)d;
    gtk_info_bar_set_revealed(info_bar, FALSE);
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
    gtk_show_about_dialog(GTK_WINDOW(ui.win), "program-name", APP_NAME, "version", MD_VERSION,
                          "logo-icon-name", "mediathek-downloader", "comments",
                          "Lädt Filme und Serien aus den Mediatheken von ZDF, ARD und 3sat für den "
                          "privaten Gebrauch "
                          "– mit Pause, Tempolimit, allen Sprachen und Untertiteln und auf Wunsch "
                          "als AV1. Suche über MediathekViewWeb.\n"
                          "Information courtesy of IMDb (https://www.imdb.com). "
                          "Used with permission.\n"
                          "Einstellungen, Verlauf und Protokoll: ~/.mediathek-downloader",
                          NULL);
}

static void on_action_quit(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    if (ui.win) gtk_window_close(GTK_WINDOW(ui.win));
}

static void on_startup(GApplication *app, gpointer d)
{
    (void)d;
    /* GTK stellt das Zahlenformat auf die Landessprache um – die Engine braucht Punkte */
    setlocale(LC_NUMERIC, "C");
    static const GActionEntry actions[] = {
        {.name = "open-log", .activate = on_action_open_log},
        {.name = "open-data", .activate = on_action_open_data},
        {.name = "about", .activate = on_action_about},
        {.name = "quit", .activate = on_action_quit},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), actions, G_N_ELEMENTS(actions), NULL);
    const char *quit_accels[] = {"<Control>q", NULL};
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", quit_accels);
}

static GtkWidget *build_menu_button(void)
{
    GMenu *menu = g_menu_new(), *about = g_menu_new();
    g_menu_append(menu, "Protokoll öffnen", "app.open-log");
    g_menu_append(menu, "Datenordner öffnen", "app.open-data");
    g_menu_append(about, "Über Mediathek Downloader", "app.about");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(about));
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
        char *text = g_strdup_printf("%d %s vom letzten Mal – auf der Seite Downloads fortsetzen.",
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

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    ui.infobar = gtk_info_bar_new();
    gtk_info_bar_set_show_close_button(GTK_INFO_BAR(ui.infobar), TRUE);
    g_signal_connect(ui.infobar, "response", G_CALLBACK(on_info_bar_response), NULL);
    ui.info_label = new_label("", NULL);
    set_wrapping(ui.info_label, 90);
    gtk_container_add(GTK_CONTAINER(gtk_info_bar_get_content_area(GTK_INFO_BAR(ui.infobar))),
                      ui.info_label);
    gtk_box_pack_start(GTK_BOX(vbox), ui.infobar, FALSE, FALSE, 0);
    ui.stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(ui.stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(ui.stack), 180);
    gtk_stack_add_titled(GTK_STACK(ui.stack), build_preview_page(), "vorschau", "Vorschau");
    ui.news_page = build_news_page();
    gtk_stack_add_titled(GTK_STACK(ui.stack), ui.news_page, "neu", "Neue Folgen");
    ui.downloads_page = build_downloads_page();
    gtk_stack_add_titled(GTK_STACK(ui.stack), ui.downloads_page, "downloads", "Downloads");
    gtk_stack_add_titled(GTK_STACK(ui.stack), build_library_page(), "bibliothek", "Bibliothek");
    gtk_stack_add_titled(GTK_STACK(ui.stack), build_settings_page(), "einstellungen",
                         "Einstellungen");
    gtk_box_pack_start(GTK_BOX(vbox), ui.stack, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(ui.win), vbox);
    GtkWidget *switcher = gtk_stack_switcher_new();
    gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(ui.stack));
    gtk_header_bar_set_custom_title(GTK_HEADER_BAR(ui.header), switcher);

    g_signal_connect(ui.win, "delete-event", G_CALLBACK(on_window_delete), NULL);
    g_signal_connect(ui.win, "configure-event", G_CALLBACK(on_window_configure), NULL);
    g_signal_connect(ui.win, "window-state-event", G_CALLBACK(on_window_state), NULL);
    g_signal_connect(ui.win, "size-allocate", G_CALLBACK(on_window_size_allocate), NULL);
    gtk_widget_show_all(ui.win);
    gtk_widget_hide(ui.infobar);
    gtk_widget_hide(ui.preview_spinner);
    gtk_widget_set_no_show_all(ui.preview_spinner, TRUE);
    gtk_widget_set_no_show_all(ui.news_spinner, TRUE);
    update_dir_label();
    update_settings_rows();
    rebuild_results();
    rebuild_library();
    show_start_notes();
    g_timeout_add(TICK_MS, on_tick, NULL);
    ui.cover_pool = g_thread_pool_new(cover_worker, NULL, COVER_THREADS, FALSE, NULL);
    ui.imdb_pool = g_thread_pool_new(imdb_genres_worker, NULL, IMDB_THREADS, FALSE, NULL);
    g_thread_pool_set_sort_function(ui.cover_pool, compare_cover_jobs, NULL);
    ui.film_limit = ui.series_limit = PREVIEW_TILES_BATCH;
    rebuild_preview();
    sync_genre_chips();
    sync_source_buttons();
    on_tick(NULL);
    start_preview_load(MD_CACHE_USE);
    g_thread_unref(g_thread_new("gpu", gpu_check_thread, NULL));
    gtk_widget_grab_focus(ui.preview_search);
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    md_init(true);
    g_set_prgname("mediathek-downloader");
    g_set_application_name(APP_NAME);
    ui.app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(ui.app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(ui.app, "activate", G_CALLBACK(on_activate), NULL);
    g_unix_signal_add(SIGINT, on_unix_signal, NULL);
    g_unix_signal_add(SIGTERM, on_unix_signal, NULL);
    g_unix_signal_add(SIGHUP, on_unix_signal, NULL);
    int rc = g_application_run(G_APPLICATION(ui.app), argc, argv);
    md_shutdown();
    g_object_unref(ui.app);
    return rc;
}
