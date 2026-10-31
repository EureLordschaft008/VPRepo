/*
 * Intro-Cutter – grafische Oberfläche (GTK 3)
 * ===========================================
 * Ablauf in vier Schritten: Einstellungen -> Treffer -> Bearbeitung -> Fertig.
 * Entfernt wahlweise das Intro am Anfang oder das Outro am Ende der Videos – oder findet
 * die Intros automatisch (ohne Referenz-Video, je Video ein eigener Schnitt).
 * Referenz-Video, Quell- und Ausgabeordner lassen sich auch per Drag & Drop
 * aus dem Dateimanager setzen.
 * Die Engine (ic_core.c) läuft in einem eigenen Thread; Meldungen, Fortschritt
 * und Rückfragen gelangen über g_idle_add() in den GTK-Hauptthread.
 * Der gesamte Zustand der Oberfläche liegt in der Struktur "ui" (Typ UiState).
 *
 * Aufbau dieser Datei: kleine Helfer – Engine -> Oberfläche (Callbacks) –
 * Einstellungen (Zustand, Vorschaubild, GPU-Prüfung, Speichern) – Seite 1
 * Einstellungen – Drag & Drop – Seite 2 Suche – Seite 3 Treffer (mit
 * Vorschau-Leisten) – Intro-Ende/Outro-Beginn am Video festlegen – Seite 3 der
 * automatischen Erkennung (Intro-Gruppen) – Ablauf der Erkennung – Seite 4
 * Bearbeitung – Seite 5 Fertig – Ablauf der Bearbeitung –
 * Aktion nach dem Ende (Herunterfahren oder Beenden) – Hash-Cache ansehen und
 * aufräumen – Fenster, Menü, Programmstart
 *
 * Aufruf:
 *     intro-cutter
 */
#define _GNU_SOURCE
#include <errno.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "ic_core.h"

#define APP_ID "de.introcutter.IntroCutter"
#define GPU_ROWS 6              /* so viele laufende Jobs werden je Gruppe einzeln angezeigt */
#define CPU_ROWS 12
#define NEAR_FACTOR 10          /* nicht erkannte Videos bis zum 10-fachen Schwellwert listen */
#define RESULT_ROWS_BATCH 200   /* Zeilen je Ergebnisliste auf einmal (weitere per Knopf) */
#define PROTOCOL_MAX_LINES 5000 /* Protokoll im Fenster; ältere Zeilen nur im Lauf-Protokoll */
#define THUMB_N 5               /* Vorschaubilder je nicht erkanntem Video */
#define THUMB_MIN_W 72          /* ihre Breite wächst mit dem Fenster */
#define THUMB_MAX_W 320
#define THUMB_GAP 6
#define THUMB_LABEL_H 17
#define THUMB_THREADS 2         /* gleichzeitige ffmpeg-Aufrufe für Vorschaubilder */
#define SETTINGS_GROUP "Einstellungen"

static const char *const MODE_ID[4] = {"copy", "cpu", "gpu", "hybrid"};
/* Genauigkeit der Erkennung: Vergleichsbilder je Video */
static const int ACC_FRAMES[4] = {1, 3, 5, 9};
static const char *const ACC_TEXT[4] = {"Schnell – 1 Vergleichsbild",
                                        "Gründlich – 3 Vergleichsbilder",
                                        "Sehr gründlich – 5 Vergleichsbilder",
                                        "Maximal – 9 Vergleichsbilder"};
/* Was passiert, wenn die Bearbeitung fertig ist */
enum { AFTER_NOTHING, AFTER_POWEROFF, AFTER_QUIT };
static const char *const AFTER_ID[3] = {"nichts", "herunterfahren", "beenden"};
static const char *const AFTER_TEXT[3] = {"Nichts tun", "PC herunterfahren",
                                          "Intro-Cutter beenden"};
/* Videoformate für Dateiauswahl und Drag & Drop */
static const char *const VIDEO_EXT[] = {"mp4", "mkv", "mov", "avi", "m4v", "webm", "flv",
                                        "wmv", "mpg", "mpeg", "ts", NULL};

enum { DROP_REF, DROP_SRC, DROP_OUT };      /* Ziele für Drag & Drop */

typedef struct {
    GtkWidget *row, *title, *subtitle, *bar;
} JobRow;

/* Eine Ergebnisliste (Treffer oder nicht erkannte Videos). Ihre Zeilen werden in
 * Portionen von RESULT_ROWS_BATCH angelegt, damit auch 100 000 Treffer die
 * Oberfläche nicht lahmlegen; ausgewählt sein können trotzdem alle. */
typedef struct {
    GtkWidget *list;            /* GtkListBox */
    GtkWidget *more_row, *more_button;  /* "Weitere anzeigen" (NULL: noch nicht nötig) */
    int first, step;            /* Ergebnis-Index der ersten Zeile, Richtung (+1/-1) */
    int total, shown;           /* Zeilen insgesamt / davon angelegt */
    bool matches;               /* Treffer (sonst nicht erkannte Videos) */
} ResultList;

typedef struct {                /* Jobliste einer Art (GPU oder CPU) */
    GtkWidget *box, *status, *list, *empty, *more;
    JobRow rows[CPU_ROWS];
    int nrows;
} JobGroup;

typedef struct {                /* eine Intro-Gruppe der automatischen Erkennung */
    GtkWidget *check;           /* Häkchen der Gruppe (NULL, solange nicht angezeigt) */
    GtkWidget *image[2];        /* letztes Bild des Intros, erstes danach */
    GtkWidget *summary;         /* Text unter dem typischen Video */
    GtkWidget *list, *more_row, *more_button;
    int *members;               /* Videos mit Schnitt (Indizes in ui.auto_result.videos) */
    int nmembers, shown;        /* davon als Zeile angelegt */
    bool shifting;              /* wird gerade an eine Korrektur angepasst */
} AutoGroupView;

typedef struct {
    GtkApplication *app;
    GtkWidget *win, *header, *stack, *back_btn, *infobar, *info_label;

    /* Einstellungen */
    char *ref_video, *src_dir, *out_dir_custom; /* out_dir_custom NULL = <Quelle>_output */
    GtkWidget *kind_buttons[3], *kind_heading; /* Intro (0), Outro (1) oder automatisch (2) */
    int kind_shown;                         /* zuletzt gewählte Art */
    double kind_length[2];                  /* Länge je Art (Wechsel merkt sich beide) */
    GtkWidget *intro_frame;                 /* Gruppe Referenz-Video und Länge */
    GtkWidget *auto_heading, *auto_frame, *auto_max_spin, *auto_min_spin;
    GtkWidget *threshold_row, *accuracy_row; /* nur bei der Suche mit Referenz-Video */
    GtkWidget *ref_row, *ref_subtitle, *ref_preview, *length_spin, *length_title, *length_subtitle;
    GtkWidget *src_row, *src_subtitle, *out_subtitle, *out_row, *out_button;
    GtkWidget *replace_switch, *mode_buttons[4], *mode_rows[4];
    GtkWidget *copy_subtitle, *gpu_subtitle, *gpu_spinner;
    GtkWidget *threshold_spin, *accuracy_combo, *after_combo, *search_button, *setup_hint;
    GtkWidget *mark_button;                 /* "Im Video festlegen …" */
    GtkWidget *cache_subtitle;              /* Größe des Hash-Caches */
    GdkPixbuf *ref_preview_pixbuf;          /* Vergleichsbild in voller Größe */
    guint ref_preview_timer, save_timer;
    int ref_preview_generation;
    bool loading_settings;                  /* Einstellungen werden gerade geladen */

    /* Fenster: Größe merken, Layout an die Breite anpassen */
    int window_width, window_height, layout_width;
    bool window_maximized;
    guint layout_idle_id;
    GtkWidget *page_columns[6], *search_box;
    int page_column_count;
    GThread *gpu_thread;
    bool gpu_ready, have_gpu, ffmpeg_ok;
    GpuCaps gpu;

    /* laufender Durchlauf */
    char *run_ref_video, *run_src_dir, *run_out_dir;
    const char *run_mode;
    double run_length;
    int run_threshold, run_frames;
    bool run_outro, run_replace, run_completed, moving;
    guint inhibit_cookie;                   /* Standby/Abmelden gesperrt */
    IcDetect detection;
    IcRun cut_run;
    GThread *worker_thread;
    bool busy, quit_when_idle;

    /* Seite "Suche" */
    GtkWidget *search_title, *search_subtitle, *search_progress, *search_detail;
    GtkWidget *search_cancel_button;
    guint pulse_timer;
    bool search_started;        /* Anzahl der zu lesenden Videos ist bekannt */

    /* Seite "Treffer" */
    GtkWidget *results_title, *results_subtitle, *results_box, *selection_label, *cut_button;
    ResultList result_lists[2];         /* [0] Treffer, [1] nicht erkannte Videos */
    bool *result_selected;              /* je Kandidat (Index in ui.detection.results) */
    int candidate_count;                /* Treffer + nicht erkannte bis NEAR_FACTOR-fach */
    GPtrArray *result_checks;           /* Häkchen der angelegten Zeilen */
    bool bulk_selecting;
    GPtrArray *preview_strips;  /* Vorschau-Leisten der nicht erkannten Videos (PreviewStrip *) */
    gint preview_generation;    /* neue Trefferliste -> alte Vorschau-Aufträge verwerfen */
    GThreadPool *preview_pool;

    /* Seite "Treffer" der automatischen Erkennung */
    bool run_auto;
    double run_auto_max;
    int run_auto_min;
    IcAuto auto_result;
    bool *auto_selected;                /* je Video (Index in auto_result.videos) */
    AutoGroupView *auto_views;          /* je Gruppe */
    int auto_view_count;
    int auto_groups_shown, auto_others_shown;
    GtkWidget *auto_title, *auto_subtitle, *auto_box, *auto_selection_label, *auto_cut_button;
    GtkWidget *auto_more_groups_row, *auto_others_list, *auto_others_more;
    GPtrArray *auto_checks;             /* Häkchen der angelegten Video-Zeilen */
    bool auto_bulk;                     /* Häkchen werden gerade gesammelt gestellt */
    GThreadPool *auto_thumb_pool;
    gint auto_generation;               /* neue Trefferliste: alte Bilder verwerfen */

    /* Seite "Bearbeitung" */
    GtkWidget *process_title, *process_subtitle, *process_progress, *protocol_view;
    GtkWidget *process_estimate;            /* Restdauer unter dem Fortschrittsbalken */
    GtkWidget *process_cancel_button, *process_pause_button, *process_after_combo;
    JobGroup gpu_jobs, cpu_jobs;
    guint jobs_timer;
    int files_done, files_total, files_failed;
    double process_fraction;                /* erledigte Arbeit 0..1 (Balken, steigt nur) */
    bool process_bar_moving;                /* Balken zeigt gerade das Verschieben */
    bool processing_paused;

    /* Seite "Fertig" */
    GtkWidget *done_icon, *done_title, *done_subtitle, *done_stats, *done_hint;
    GtkWidget *done_move_button, *done_open_button;
    GtkWidget *done_move_progress;          /* Fortschritt beim Verschieben */

    /* Rückfragen aus dem Worker-Thread */
    GMutex ask_mutex;
    GCond ask_cond;
    int ask_answer;             /* -1 = offen */
} UiState;

static UiState ui;

static void update_start_button(void);
static void show_page(const char *name);
static void update_process_progress(void);
static void update_cache_row(void);
static void open_cache_window(void);
static void on_cache_clicked(GtkButton *b, gpointer d);
static void on_mark_clicked(GtkButton *b, gpointer d);

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

/* Pfad für die Anzeige (Dateinamen in beliebiger Kodierung) */
static char *path_for_display(const char *path)
{
    return g_filename_display_name(path ? path : "");
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

/* Trennlinie zwischen den Zeilen einer Liste */
static void list_separator_header(GtkListBoxRow *this_row, GtkListBoxRow *before, gpointer data)
{
    (void)data;
    if (before && !gtk_list_box_row_get_header(this_row))
        gtk_list_box_row_set_header(this_row, gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
}

/* Überschrift + umrandete Liste (Einstellungsgruppe); heading: Überschrift-Label */
static GtkWidget *add_group_with_heading(GtkWidget *box, const char *title, GtkWidget **heading)
{
    if (title) {
        GtkWidget *h = new_label(title, "ic-heading");
        gtk_box_pack_start(GTK_BOX(box), h, FALSE, FALSE, 0);
        if (heading) *heading = h;
    }
    GtkWidget *frame = gtk_frame_new(NULL);
    add_css_class(frame, "ic-group");
    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_list_box_set_header_func(GTK_LIST_BOX(list), list_separator_header, NULL, NULL);
    gtk_container_add(GTK_CONTAINER(frame), list);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    return list;
}

/* umrandete Liste mit Überschrift (Einstellungsgruppe) */
static GtkWidget *add_group(GtkWidget *box, const char *title)
{
    return add_group_with_heading(box, title, NULL);
}

/* Zeile: [prefix]  Titel / Untertitel  [suffix] */
static GtkWidget *add_row(GtkWidget *list, const char *title, const char *subtitle,
                          GtkWidget *prefix, GtkWidget *suffix, GtkWidget **title_out,
                          GtkWidget **sub_out)
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
    GtkWidget *t = new_label(title, "ic-title");
    gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(t), 50);
    gtk_box_pack_start(GTK_BOX(v), t, FALSE, FALSE, 0);
    GtkWidget *s = NULL;
    if (subtitle) {
        s = new_label(subtitle, "ic-sub");
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
    if (title_out) *title_out = t;
    if (sub_out) *sub_out = s;
    return r;
}

/* Seitenrahmen: zentrierte Spalte in einem Scrollbereich */
static GtkWidget *new_page_column(GtkWidget **scroller)
{
    GtkWidget *scrolled = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(box, TRUE);              /* wächst mit dem Fenster */
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 24);
    gtk_widget_set_margin_start(box, 24);
    gtk_widget_set_margin_end(box, 24);
    gtk_container_add(GTK_CONTAINER(scrolled), box);
    if (ui.page_column_count < (int)G_N_ELEMENTS(ui.page_columns))
        ui.page_columns[ui.page_column_count++] = box;
    *scroller = scrolled;
    return box;
}

/* runder Hauptknopf (z. B. "Intro suchen") */
static GtkWidget *new_pill_button(const char *text, const char *cls)
{
    GtkWidget *b = gtk_button_new_with_label(text);
    add_css_class(b, "ic-pill");
    if (cls) add_css_class(b, cls);
    return b;
}

/* Ja/Nein-Rückfrage (modal) -> true bei "yes_label" */
static bool ask_confirmation(const char *title, const char *text, const char *no_label,
                             const char *yes_label, bool destructive)
{
    GtkWindow *parent = gtk_application_get_active_window(ui.app);    /* z. B. Cache-Fenster */
    GtkWidget *d = gtk_message_dialog_new(parent ? parent : GTK_WINDOW(ui.win), GTK_DIALOG_MODAL,
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

/* Meldungsleiste oben im Fenster */
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

static const char CSS[] =
    ".ic-heading { font-weight: bold; margin-top: 18px; margin-bottom: 4px; }\n"
    "frame.ic-group > border { border-radius: 10px; background-color: @theme_base_color; }\n"
    "frame.ic-group list { background-color: transparent; }\n"
    "frame.ic-group list > row:first-child { border-top-left-radius: 10px;"
    " border-top-right-radius: 10px; }\n"
    "frame.ic-group list > row:last-child { border-bottom-left-radius: 10px;"
    " border-bottom-right-radius: 10px; }\n"
    ".ic-sub { font-size: 90%; }\n"
    ".ic-hero { font-size: 170%; font-weight: 800; }\n"
    ".ic-lead { font-size: 105%; }\n"
    "frame.ic-group list > row:drop(active) { box-shadow: inset 0 0 0 2px"
    " @theme_selected_bg_color; background-color: alpha(@theme_selected_bg_color, 0.14); }\n"
    ".ic-kind button { padding: 5px 22px; }\n"
    ".ic-status { margin-top: 18px; margin-bottom: 4px; font-size: 90%; }\n"
    ".ic-kind button:checked { background-image: none; background-color:"
    " @theme_selected_bg_color; border-color: @theme_selected_bg_color; }\n"
    ".ic-kind button:checked label { color: @theme_selected_fg_color; }\n"
    ".ic-pill { border-radius: 999px; padding: 8px 30px; font-weight: bold; }\n"
    ".ic-badge { border-radius: 999px; padding: 1px 9px; font-size: 85%; font-weight: bold; }\n"
    ".ic-badge.ok { color: @success_color; background-color: alpha(@success_color, 0.16); }\n"
    ".ic-badge.near { color: @warning_color; background-color: alpha(@warning_color, 0.16); }\n"
    ".ic-thumb { border-radius: 6px; background-color: alpha(@theme_fg_color, 0.06); }\n"
    ".ic-log { font-family: monospace; font-size: 90%; }\n"
    ".ic-time { font-family: monospace; font-weight: bold; }\n"
    ".ic-bigicon { color: @success_color; }\n"
    ".ic-bigicon.warn { color: @warning_color; }\n"
    ".ic-bigicon.err { color: @error_color; }\n";

/* ---------------------------------------------------------------------------
 * Engine -> Oberfläche (die Callbacks laufen in Worker-Threads)
 * ------------------------------------------------------------------------- */
typedef struct {
    int a, b, c, d;
    double x;
    bool ok;
} EngineMsg;

/* Nachricht der Engine für den Hauptthread anlegen */
static EngineMsg *engine_msg_new(int a, int b, int c, int d, double x, bool ok)
{
    EngineMsg *m = g_new0(EngineMsg, 1);
    *m = (EngineMsg){a, b, c, d, x, ok};
    return m;
}

/* Zeile ans Protokoll der Bearbeitungsseite anhängen (Fehler rot, Hinweise gelb, OK grün) */
static void protocol_append(const char *text)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui.protocol_view));
    const char *tag = strstr(text, "FEHLER") || strstr(text, "!!!") ? "err"
                    : strstr(text, "Watchdog") || strstr(text, "verworfen") ? "warn"
                    : strstr(text, "] OK ") ? "ok" : NULL;
    const char *s = text;
    while (*s == '\n') s++;                     /* Leerzeilen der Terminal-Ausgabe */
    while (*s == ' ') s++;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);
    if (gtk_text_buffer_get_char_count(buf)) gtk_text_buffer_insert(buf, &end, "\n", 1);
    if (tag)
        gtk_text_buffer_insert_with_tags_by_name(buf, &end, s, -1, tag, NULL);
    else
        gtk_text_buffer_insert(buf, &end, s, -1);
    /* bei sehr vielen Videos nur die letzten PROTOCOL_MAX_LINES Zeilen behalten – in
     * Schüben von 500, damit nicht jede neue Zeile ein Löschen auslöst */
    int lines = gtk_text_buffer_get_line_count(buf);
    if (lines > PROTOCOL_MAX_LINES + 500) {
        GtkTextIter start, cut;
        gtk_text_buffer_get_start_iter(buf, &start);
        gtk_text_buffer_get_iter_at_line(buf, &cut, lines - PROTOCOL_MAX_LINES);
        gtk_text_buffer_delete(buf, &start, &cut);
        gtk_text_buffer_get_start_iter(buf, &start);
        gtk_text_buffer_insert(buf, &start,
                               "… ältere Zeilen stehen im Lauf-Protokoll (Menü)\n", -1);
    }
    GtkTextMark *mark = gtk_text_buffer_get_insert(buf);
    gtk_text_buffer_get_end_iter(buf, &end);
    gtk_text_buffer_place_cursor(buf, &end);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(ui.protocol_view), mark);
}

/* im Hauptthread: Protokollzeile anzeigen */
static gboolean on_idle_protocol(gpointer p)
{
    if (ui.win) protocol_append(p);
    g_free(p);
    return G_SOURCE_REMOVE;
}

/* Callback der Engine: Meldung fürs Protokoll (aus beliebigem Thread) */
static void frontend_log(const char *msg)
{
    g_idle_add(on_idle_protocol, g_utf8_make_valid(msg, -1));
}

/* im Hauptthread: Anzahl gefundener und zu lesender Videos anzeigen */
static gboolean on_idle_search_start(gpointer p)
{
    EngineMsg *m = p;
    if (!ui.win) {
        g_free(m);
        return G_SOURCE_REMOVE;
    }
    char *s = g_strdup_printf("%d Videos gefunden · %d aus dem Cache · %d werden gelesen",
                              m->a, m->b, m->c);
    set_label_text(ui.search_subtitle, s);
    g_free(s);
    ui.search_started = true;
    ui.files_total = m->c;
    if (!m->c) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.search_progress), 1.0);
    g_free(m);
    return G_SOURCE_REMOVE;
}

/* Callback der Engine: die Suche beginnt */
static void frontend_search_start(int videos, int from_cache, int to_read)
{
    g_idle_add(on_idle_search_start, engine_msg_new(videos, from_cache, to_read, 0, 0, true));
}

/* im Hauptthread: Suchfortschritt anzeigen */
static gboolean on_idle_search_progress(gpointer p)
{
    EngineMsg *m = p;
    if (!ui.win) {
        g_free(m);
        return G_SOURCE_REMOVE;
    }
    char *t = g_strdup_printf("%d von %d", m->a, m->b);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.search_progress),
                                  m->b ? (double)m->a / m->b : 1);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.search_progress), t);
    char *d = g_strdup_printf("Parallel: %d von max. %d · CPU-Last %.0f %%", m->c, m->d, m->x);
    set_label_text(ui.search_detail, d);
    g_free(t);
    g_free(d);
    g_free(m);
    return G_SOURCE_REMOVE;
}

/* Callback der Engine: Suchfortschritt */
static void frontend_search_progress(int done, int total, int workers, int max_workers, double cpu)
{
    g_idle_add(on_idle_search_progress,
               engine_msg_new(done, total, workers, max_workers, cpu, true));
}

static gboolean on_pulse_tick(gpointer d);

/* im Hauptthread: neuer Abschnitt der automatischen Suche – Text zeigen, Balken pendelt */
static gboolean on_idle_search_stage(gpointer p)
{
    if (ui.win && ui.busy) {
        set_label_text(ui.search_subtitle, p);
        set_label_text(ui.search_detail, "");
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.search_progress), NULL);
        ui.search_started = false;
        if (!ui.pulse_timer) ui.pulse_timer = g_timeout_add(120, on_pulse_tick, NULL);
    }
    g_free(p);
    return G_SOURCE_REMOVE;
}

/* Callback der Engine: neuer Abschnitt der automatischen Suche */
static void frontend_search_stage(const char *text)
{
    g_idle_add(on_idle_search_stage, g_utf8_make_valid(text, -1));
}

/* im Hauptthread: Fortschritt der Bearbeitung aktualisieren */
static gboolean on_idle_file_done(gpointer p)
{
    EngineMsg *m = p;
    if (!ui.win) {
        g_free(m);
        return G_SOURCE_REMOVE;
    }
    ui.files_done = m->a;
    ui.files_total = m->b;
    if (!m->ok) ui.files_failed++;
    update_process_progress();
    g_free(m);
    return G_SOURCE_REMOVE;
}

/* Callback der Engine: eine Datei ist fertig */
static void frontend_file_done(int done, int total, bool ok)
{
    g_idle_add(on_idle_file_done, engine_msg_new(done, total, 0, 0, 0, ok));
}

typedef struct {
    int kind;                   /* 0 = fertige verschieben?, 1 = weiterlaufen? */
    double free_gb;
    int n;
} DiskQuestion;

/* im Hauptthread: Rückfrage bei wenig Speicherplatz stellen und Antwort melden */
static gboolean on_idle_disk_question(gpointer p)
{
    DiskQuestion *a = p;
    if (!ui.win) {
        g_mutex_lock(&ui.ask_mutex);
        ui.ask_answer = 0;
        g_cond_signal(&ui.ask_cond);
        g_mutex_unlock(&ui.ask_mutex);
        g_free(a);
        return G_SOURCE_REMOVE;
    }
    char *title = g_strdup_printf("Nur noch %.1f GB frei auf dem Ausgabe-Datenträger",
                                  a->free_gb);
    char *text = a->kind == 0
        ? g_strdup_printf("Die Bearbeitung pausiert (Schwelle %d GB). Dürfen die %d fertigen "
                          "Dateien an den Platz der Originale verschoben werden? Die Originale "
                          "werden dabei gelöscht.", DISK_MIN_GB, a->n)
        : g_strdup_printf("Die Bearbeitung pausiert (Schwelle %d GB). Soll sie weiterlaufen?",
                          DISK_MIN_GB);
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(ui.win), GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s", title);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s", text);
    gtk_dialog_add_button(GTK_DIALOG(d), a->kind == 0 ? "Nicht verschieben" : "Beenden",
                          GTK_RESPONSE_NO);
    GtkWidget *yes = gtk_dialog_add_button(GTK_DIALOG(d),
                                           a->kind == 0 ? "Verschieben" : "Weiterlaufen",
                                           GTK_RESPONSE_YES);
    add_css_class(yes, a->kind == 0 ? "destructive-action" : "suggested-action");
    int resp = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    g_mutex_lock(&ui.ask_mutex);
    ui.ask_answer = resp == GTK_RESPONSE_YES;
    g_cond_signal(&ui.ask_cond);
    g_mutex_unlock(&ui.ask_mutex);
    g_free(title);
    g_free(text);
    g_free(a);
    return G_SOURCE_REMOVE;
}

/* Rückfrage im Hauptthread stellen und im Worker auf die Antwort warten */
static bool ask_in_main_thread(int kind, double free_gb, int n)
{
    DiskQuestion *a = g_new0(DiskQuestion, 1);
    *a = (DiskQuestion){kind, free_gb, n};
    g_mutex_lock(&ui.ask_mutex);
    ui.ask_answer = -1;
    g_idle_add(on_idle_disk_question, a);
    while (ui.ask_answer < 0 && !ic_cancel)
        g_cond_wait_until(&ui.ask_cond, &ui.ask_mutex,
                          g_get_monotonic_time() + G_TIME_SPAN_SECOND / 4);
    bool r = ui.ask_answer == 1;
    g_mutex_unlock(&ui.ask_mutex);
    return r;
}

/* Callback der Engine: fertige Dateien wegen Platzmangel verschieben? */
static bool frontend_ask_disk_move(double free_gb, int n_finished)
{
    return ask_in_main_thread(0, free_gb, n_finished);
}

/* Byte für die Anzeige: "12.3 GB", "456 MB" bzw. "78 KB" */
static void format_bytes(double bytes, char *out, size_t size)
{
    if (bytes >= 1e9)
        snprintf(out, size, "%.1f GB", bytes / 1e9);
    else if (bytes >= 1e6)
        snprintf(out, size, "%.0f MB", bytes / 1e6);
    else
        snprintf(out, size, "%.0f KB", bytes / 1e3);
}

/* Fortschritt beim Verschieben an den Platz der Originale */
typedef struct {
    int done, total;
    double bytes_done, bytes_total;
} MoveProgressMsg;

/* im Hauptthread: Fortschritt des Verschiebens anzeigen – auf der Fertig-Seite bzw.
 * (bei Platzmangel mitten in der Bearbeitung) im Fortschrittsbalken der Bearbeitung */
static gboolean on_idle_move_progress(gpointer p)
{
    MoveProgressMsg *m = p;
    if (ui.win) {
        char done_text[32], total_text[32];
        format_bytes(m->bytes_done, done_text, sizeof done_text);
        format_bytes(m->bytes_total, total_text, sizeof total_text);
        char *text = m->bytes_total > 0
                         ? g_strdup_printf("%d von %d Dateien verschoben · %s von %s", m->done,
                                           m->total, done_text, total_text)
                         : g_strdup_printf("%d von %d Dateien verschoben", m->done, m->total);
        double fraction = m->bytes_total > 0 ? m->bytes_done / m->bytes_total
                        : m->total ? (double)m->done / m->total : 1;
        GtkWidget *bar = ui.moving ? ui.done_move_progress : ui.process_progress;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), fmin(1, fraction));
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(bar), text);
        if (!ui.moving) ui.process_bar_moving = m->done < m->total;
        if (ui.moving)
            gtk_widget_show(bar);
        else if (m->done == m->total)
            update_process_progress();          /* wieder der Stand der Bearbeitung */
        g_free(text);
    }
    g_free(m);
    return G_SOURCE_REMOVE;
}

/* Callback der Engine: Fortschritt beim Verschieben (aus dem Arbeits-Thread) */
static void frontend_move_progress(int done, int total, double bytes_done, double bytes_total)
{
    MoveProgressMsg *m = g_new(MoveProgressMsg, 1);
    *m = (MoveProgressMsg){done, total, bytes_done, bytes_total};
    g_idle_add(on_idle_move_progress, m);
}

/* Callback der Engine: trotz Platzmangel weiterlaufen? */
static bool frontend_ask_disk_continue(double free_gb)
{
    return ask_in_main_thread(1, free_gb, 0);
}

/* ---------------------------------------------------------------------------
 * Einstellungen: Zustand, Vorschaubild, GPU-Prüfung, Speichern
 * ------------------------------------------------------------------------- */
static int selected_mode(void)
{
    for (int i = 0; i < 4; i++)
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.mode_buttons[i]))) return i;
    return 0;
}

/* true, wenn "Originale ersetzen" eingeschaltet ist */
static bool replace_originals_on(void)
{
    return gtk_switch_get_active(GTK_SWITCH(ui.replace_switch));
}

/* eingestellte Länge des Intros bzw. Outros in Sekunden */
static double selected_length(void)
{
    return gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui.length_spin));
}

/* Outro am Ende statt Intro am Anfang? */
static bool outro_selected(void)
{
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.kind_buttons[1]));
}

/* Intros automatisch erkennen (ohne Referenz-Video)? */
static bool auto_selected(void)
{
    return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.kind_buttons[2]));
}

/* Ausgabeordner: selbst gewählt oder <Quelle>_output (wie in der Terminal-Version) */
static char *effective_out_dir(void)
{
    if (ui.out_dir_custom) return g_strdup(ui.out_dir_custom);
    if (!ui.src_dir) return NULL;
    char *src_norm = normpath(ui.src_dir), *out_dir = g_strdup_printf("%s_output", src_norm);
    free(src_norm);
    return out_dir;
}

/* Vergleichsbilder je Video laut Einstellung "Genauigkeit" */
static int selected_frames(void)
{
    int i = gtk_combo_box_get_active(GTK_COMBO_BOX(ui.accuracy_combo));
    return ACC_FRAMES[i >= 0 && i < 4 ? i : 0];
}

/* gewählte Aktion nach der Bearbeitung (AFTER_NOTHING, AFTER_POWEROFF, AFTER_QUIT) */
static int selected_after_action(void)
{
    int a = gtk_combo_box_get_active(GTK_COMBO_BOX(ui.after_combo));
    return a >= 0 && a < 3 ? a : AFTER_NOTHING;
}

/* Texte der Einstellungsseite an Intro/Outro, Länge und Auswahl anpassen */
static void update_setup_labels(void)
{
    char ts[64], *s;
    bool outro = outro_selected();
    const char *kind = outro ? "Outro" : "Intro";
    ic_ref_timestamp(selected_length(), ts, sizeof ts);
    int n_frames = ic_frames_for(selected_length(), selected_frames());
    if (n_frames > 1) {                         /* mehrere Vergleichsbilder: alle Zeitpunkte */
        char times[IC_MAX_FRAMES][24];
        ic_ref_timestamps(selected_length(), n_frames, times);
        GString *joined = g_string_new(NULL);
        for (int j = 0; j < n_frames; j++)
            g_string_append_printf(joined, "%s%s", j ? ", " : "", times[j]);
        snprintf(ts, sizeof ts, "%s", joined->str);
        g_string_free(joined, TRUE);
    }
    set_label_text(ui.kind_heading, kind);
    if (ui.ref_video) {
        char *base = g_path_get_basename(ui.ref_video), *name = path_for_display(base);
        const char *what = n_frames > 1 ? "Vergleichsbilder" : "Vergleichsbild";
        s = outro ? g_strdup_printf("%s · %s %s s vor dem Ende", name, what, ts)
                  : g_strdup_printf("%s · %s bei %s s", name, what, ts);
        g_free(base);
        g_free(name);
    } else {
        s = g_strdup_printf("Video mit dem %s wählen oder hierher ziehen", kind);
    }
    set_label_text(ui.ref_subtitle, s);
    g_free(s);
    set_label_text(ui.length_title, outro ? "Länge des Outros" : "Länge des Intros");
    gtk_widget_set_sensitive(ui.mark_button, ui.ref_video != NULL);
    gtk_widget_set_tooltip_text(ui.mark_button,
                                !ui.ref_video ? "Zuerst ein Referenz-Video wählen"
                                : outro ? "Das Ende des Referenz-Videos ansehen und den Beginn des "
                                          "Outros bildgenau festlegen"
                                        : "Den Anfang des Referenz-Videos ansehen und das Ende "
                                          "des Intros bildgenau festlegen");
    set_label_text(ui.length_subtitle, outro ? "So viel wird am Ende jedes Videos abgeschnitten."
                               : "Alles bis zu dieser Stelle wird herausgeschnitten.");
    bool automatic = auto_selected();
    set_label_text(ui.copy_subtitle,
                   automatic ? "Schnitt am letzten Keyframe im jeweiligen Intro, ohne "
                               "Neukodierung – nimmt nie Inhalt weg, evtl. bleibt ein Rest des "
                               "Intros stehen. Liegt im Intro kein Keyframe, wird bildgenau neu "
                               "kodiert."
                   : outro ? "Schnitt am Ende ohne Neukodierung, bis auf wenige Frames genau. "
                             "Klappt der Schnitt nicht sauber, wird automatisch neu kodiert."
                           : "Schnitt am ersten Keyframe nach dem Intro, ohne Neukodierung. Liegt "
                             "dort kein Keyframe, geht etwas Inhalt direkt nach dem Intro mit. "
                             "Klappt der Schnitt nicht sauber, wird automatisch neu kodiert.");
    gtk_button_set_label(GTK_BUTTON(ui.search_button), automatic ? "Intros suchen"
                                                       : outro   ? "Outro suchen"
                                                                 : "Intro suchen");
    /* automatisch: ohne Referenz-Video, Länge, Schwellwert und Vergleichsbilder */
    gtk_widget_set_visible(ui.kind_heading, !automatic);
    gtk_widget_set_visible(ui.intro_frame, !automatic);
    gtk_widget_set_visible(ui.auto_heading, automatic);
    gtk_widget_set_visible(ui.auto_frame, automatic);
    gtk_widget_set_visible(ui.threshold_row, !automatic);
    gtk_widget_set_visible(ui.accuracy_row, !automatic);
    if (ui.src_dir) {
        char *p = path_for_display(ui.src_dir);
        s = g_strdup_printf("%s  (inkl. Unterordner)", p);
        g_free(p);
    } else {
        s = g_strdup("Ordner mit den Videos wählen oder hierher ziehen");
    }
    set_label_text(ui.src_subtitle, s);
    g_free(s);
    char *out = effective_out_dir();
    if (replace_originals_on())
        set_label_text(ui.out_subtitle, "Nicht benötigt – die Originale werden ersetzt");
    else if (out) {
        char *p = path_for_display(out);
        set_label_text(ui.out_subtitle, p);
        g_free(p);
    } else {
        set_label_text(ui.out_subtitle, "Standard: neben dem Quellordner (<Quelle>_output)");
    }
    g_free(out);
    gtk_widget_set_sensitive(ui.out_row, !replace_originals_on());
    update_start_button();
}

/* Suchknopf nur aktiv, wenn alles Nötige gewählt ist; sonst Hinweis daneben */
static void update_start_button(void)
{
    bool need_ref = !auto_selected();               /* automatisch: ohne Referenz-Video */
    gtk_widget_set_sensitive(ui.search_button, (ui.ref_video || !need_ref) && ui.src_dir &&
                                                   ui.ffmpeg_ok && !ui.busy);
    set_label_text(ui.setup_hint, !ui.ffmpeg_ok             ? "ffmpeg fehlt"
                            : need_ref && !ui.ref_video ? "Zuerst ein Referenz-Video wählen"
                            : !ui.src_dir               ? "Noch einen Quellordner wählen"
                                                        : "");
}

typedef struct {
    int gen;
    char *ref, *png;
    double intro;
    bool outro, ok;
} RefPreviewJob;

/* Größe des Vergleichsbilds: wächst mit dem Fenster (16:9) */
static void ref_preview_size(int *w, int *h)
{
    int window_w = ui.layout_width > 0 ? ui.layout_width : 780;
    *w = CLAMP(window_w * 16 / 100, 128, 320);
    *h = *w * 9 / 16;
}

/* Vergleichsbild in der aktuellen Größe anzeigen */
static void update_ref_preview(void)
{
    int w, h;
    ref_preview_size(&w, &h);
    gtk_widget_set_size_request(ui.ref_preview, w, h);
    if (!ui.ref_preview_pixbuf) return;
    int pix_w = gdk_pixbuf_get_width(ui.ref_preview_pixbuf),
        pix_h = gdk_pixbuf_get_height(ui.ref_preview_pixbuf);
    double scale = fmin((double)w / pix_w, (double)h / pix_h);
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(ui.ref_preview_pixbuf,
                                                MAX(1, (int)lround(pix_w * scale)),
                                                MAX(1, (int)lround(pix_h * scale)),
                                                GDK_INTERP_BILINEAR);
    gtk_image_set_from_pixbuf(GTK_IMAGE(ui.ref_preview), scaled);
    g_object_unref(scaled);
}

/* im Hauptthread: neues Vergleichsbild übernehmen (veraltete Ergebnisse verwerfen) */
static gboolean on_ref_preview_done(gpointer p)
{
    RefPreviewJob *t = p;
    if (ui.win && t->gen == ui.ref_preview_generation) {
        GdkPixbuf *pixbuf = t->ok ? gdk_pixbuf_new_from_file(t->png, NULL) : NULL;
        g_clear_object(&ui.ref_preview_pixbuf);
        if (pixbuf) {
            ui.ref_preview_pixbuf = pixbuf;
            update_ref_preview();
        } else {
            gtk_image_set_from_icon_name(GTK_IMAGE(ui.ref_preview), "dialog-warning-symbolic",
                                         GTK_ICON_SIZE_DND);
            set_label_text(ui.ref_subtitle, t->outro ? "An dieser Stelle ist kein Bild lesbar – "
                                            "Outro-Länge prüfen"
                                          : "An dieser Stelle ist kein Bild lesbar – "
                                            "Intro-Länge prüfen");
        }
    }
    g_unlink(t->png);
    g_free(t->png);
    g_free(t->ref);
    g_free(t);
    return G_SOURCE_REMOVE;
}

/* Vergleichsbild im Hintergrund aus dem Referenz-Video lesen */
static gpointer ref_preview_thread(gpointer p)
{
    RefPreviewJob *t = p;
    t->ok = ic_ref_frame_png(t->ref, t->intro, t->outro, t->png);
    g_idle_add(on_ref_preview_done, t);
    return NULL;
}

/* Lesen des Vergleichsbilds starten */
static gboolean start_ref_preview(gpointer data)
{
    (void)data;
    ui.ref_preview_timer = 0;
    if (!ui.ref_video) return G_SOURCE_REMOVE;
    RefPreviewJob *t = g_new0(RefPreviewJob, 1);
    t->gen = ++ui.ref_preview_generation;
    t->ref = g_strdup(ui.ref_video);
    t->intro = selected_length();
    t->outro = outro_selected();
    t->png = g_strdup_printf("%s/intro-cutter-vorschau-%d-%d.png", g_get_tmp_dir(),
                             (int)getpid(), t->gen);
    g_thread_unref(g_thread_new("vorschau", ref_preview_thread, t));
    return G_SOURCE_REMOVE;
}

/* Vergleichsbild kurz verzögert neu lesen (z. B. während die Länge getippt wird) */
static void schedule_ref_preview(void)
{
    if (ui.ref_preview_timer) g_source_remove(ui.ref_preview_timer);
    ui.ref_preview_timer = g_timeout_add(350, start_ref_preview, NULL);
}

#define STR_(x) #x
#define STR(x) STR_(x)
static const char GPU_DESC[] = "VAAPI, Codec wie die Quelle – exakt und schnell; die CPU "
                               "springt nur bei Fehlern ein. Die GPU wird höchstens zu "
                               STR(GPU_TARGET) " % ausgelastet.";

/* im Hauptthread: Ergebnis der GPU-Prüfung anzeigen, GPU-Modi freigeben */
static gboolean on_gpu_check_done(gpointer p)
{
    (void)p;
    ui.gpu_ready = true;
    if (ui.gpu_thread) {
        g_thread_join(ui.gpu_thread);
        ui.gpu_thread = NULL;
    }
    if (!ui.win) return G_SOURCE_REMOVE;
    gtk_spinner_stop(GTK_SPINNER(ui.gpu_spinner));
    gtk_widget_hide(ui.gpu_spinner);
    char *sum = ic_gpu_summary(ui.have_gpu ? &ui.gpu : NULL);
    char *s = ui.have_gpu ? g_strdup_printf("%s\n%s", GPU_DESC, sum)
                         : g_strdup_printf("%s\nKeine geeignete GPU (VAAPI) gefunden.", GPU_DESC);
    set_label_text(ui.gpu_subtitle, s);
    gtk_widget_set_sensitive(ui.mode_rows[2], ui.have_gpu);
    gtk_widget_set_sensitive(ui.mode_rows[3], ui.have_gpu);
    if (!ui.have_gpu && selected_mode() >= 2)           /* GPU-Modi ohne GPU nicht möglich */
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.mode_buttons[0]), TRUE);
    free(sum);
    g_free(s);
    return G_SOURCE_REMOVE;
}

/* GPU (VAAPI) im Hintergrund erkennen und testen */
static gpointer gpu_check_thread(gpointer p)
{
    (void)p;
    ui.have_gpu = GPU_ENABLE && ui.ffmpeg_ok && ic_gpu_detect(&ui.gpu);
    g_idle_add(on_gpu_check_done, NULL);
    return NULL;
}

/* Pfad der Einstellungsdatei ~/.intro-cutter/gui.ini */
static char *settings_path(void)
{
    return g_build_filename(APP_DIR, "gui.ini", NULL);
}

/* Einstellungen (ohne Referenz-Video und Ordner – die werden je Lauf neu
 * gewählt) in ~/.intro-cutter/gui.ini; gespeichert wird bei jeder Änderung */
static void settings_load(void)
{
    GKeyFile *kf = g_key_file_new();
    char *path = settings_path(), *s;
    ui.loading_settings = true;
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        if ((s = g_key_file_get_string(kf, SETTINGS_GROUP, "entfernen", NULL)) &&
            (!strcmp(s, "outro") || !strcmp(s, "auto")))  /* vor den Längen (Umschalten …) */
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.kind_buttons[s[0] == 'o' ? 1 : 2]),
                                         TRUE);
        g_free(s);
        double auto_max = g_key_file_get_double(kf, SETTINGS_GROUP, "auto_hoechstens", NULL);
        if (auto_max > 0) gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.auto_max_spin), auto_max);
        int auto_min = g_key_file_get_integer(kf, SETTINGS_GROUP, "auto_mindestens", NULL);
        if (auto_min > 0) gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.auto_min_spin), auto_min);
        for (int k = 0; k < 2; k++) {
            double len = g_key_file_get_double(kf, SETTINGS_GROUP,
                                               k ? "outro_sekunden" : "intro_sekunden", NULL);
            if (len > 0) ui.kind_length[k] = len;
        }
        if (g_key_file_has_key(kf, SETTINGS_GROUP, "schwellwert", NULL))
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.threshold_spin),
                                      g_key_file_get_integer(kf, SETTINGS_GROUP, "schwellwert",
                                                             NULL));
        gtk_switch_set_active(GTK_SWITCH(ui.replace_switch),
                              g_key_file_get_boolean(kf, SETTINGS_GROUP, "ersetzen", NULL));
        int m = g_key_file_get_integer(kf, SETTINGS_GROUP, "modus", NULL);
        if (m >= 0 && m < 4)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.mode_buttons[m]), TRUE);
        int f = g_key_file_get_integer(kf, SETTINGS_GROUP, "genauigkeit", NULL);
        for (int i = 0; i < 4; i++)
            if (ACC_FRAMES[i] == f) gtk_combo_box_set_active(GTK_COMBO_BOX(ui.accuracy_combo), i);
        if ((s = g_key_file_get_string(kf, SETTINGS_GROUP, "danach", NULL)))
            for (int i = 0; i < 3; i++)
                if (!strcmp(s, AFTER_ID[i]))
                    gtk_combo_box_set_active(GTK_COMBO_BOX(ui.after_combo), i);
        g_free(s);
        ui.window_width = g_key_file_get_integer(kf, SETTINGS_GROUP, "fenster_breite", NULL);
        ui.window_height = g_key_file_get_integer(kf, SETTINGS_GROUP, "fenster_hoehe", NULL);
        ui.window_maximized = g_key_file_get_boolean(kf, SETTINGS_GROUP, "fenster_maximiert", NULL);
    }
    g_free(path);
    g_key_file_unref(kf);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.length_spin), ui.kind_length[outro_selected()]);
    ui.kind_shown = auto_selected() ? 2 : outro_selected();
    ui.loading_settings = false;
}

/* alle Einstellungen außer Pfaden in die Einstellungsdatei schreiben */
static void settings_save(void)
{
    GKeyFile *kf = g_key_file_new();
    char *path = settings_path();
    if (!auto_selected()) ui.kind_length[outro_selected()] = selected_length();
    g_key_file_set_string(kf, SETTINGS_GROUP, "entfernen",
                          auto_selected() ? "auto" : outro_selected() ? "outro" : "intro");
    g_key_file_set_double(kf, SETTINGS_GROUP, "auto_hoechstens",
                          gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui.auto_max_spin)));
    g_key_file_set_integer(kf, SETTINGS_GROUP, "auto_mindestens",
                           gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.auto_min_spin)));
    g_key_file_set_double(kf, SETTINGS_GROUP, "intro_sekunden", ui.kind_length[0]);
    g_key_file_set_double(kf, SETTINGS_GROUP, "outro_sekunden", ui.kind_length[1]);
    g_key_file_set_integer(kf, SETTINGS_GROUP, "schwellwert",
                           gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.threshold_spin)));
    g_key_file_set_boolean(kf, SETTINGS_GROUP, "ersetzen", replace_originals_on());
    g_key_file_set_integer(kf, SETTINGS_GROUP, "modus", selected_mode());
    g_key_file_set_integer(kf, SETTINGS_GROUP, "genauigkeit", selected_frames());
    g_key_file_set_string(kf, SETTINGS_GROUP, "danach", AFTER_ID[selected_after_action()]);
    if (ui.window_width > 0 && ui.window_height > 0) {
        g_key_file_set_integer(kf, SETTINGS_GROUP, "fenster_breite", ui.window_width);
        g_key_file_set_integer(kf, SETTINGS_GROUP, "fenster_hoehe", ui.window_height);
    }
    g_key_file_set_boolean(kf, SETTINGS_GROUP, "fenster_maximiert", ui.window_maximized);
    g_mkdir_with_parents(APP_DIR, 0755);
    g_key_file_save_to_file(kf, path, NULL);
    g_free(path);
    g_key_file_unref(kf);
}

/* gesammelte Änderungen speichern */
static gboolean on_save_timer(gpointer d)
{
    (void)d;
    ui.save_timer = 0;
    settings_save();
    return G_SOURCE_REMOVE;
}

/* nach jeder Änderung (gesammelt) speichern */
static void settings_changed(void)
{
    if (ui.loading_settings || !ui.win) return;
    if (ui.save_timer) g_source_remove(ui.save_timer);
    ui.save_timer = g_timeout_add(400, on_save_timer, NULL);
}

/* ---------------------------------------------------------------------------
 * Seite 1: Einstellungen
 * ------------------------------------------------------------------------- */
static char *choose_path(const char *title, GtkFileChooserAction action, const char *current,
                         bool videos_only)
{
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new(title, GTK_WINDOW(ui.win), action,
                                                               "Auswählen", "Abbrechen");
    GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
    if (current && action == GTK_FILE_CHOOSER_ACTION_OPEN && is_file(current)) {
        gtk_file_chooser_set_filename(chooser, current);
    } else if (current) {
        char *dir = is_dir(current) ? g_strdup(current) : g_path_get_dirname(current);
        if (is_dir(dir)) gtk_file_chooser_set_current_folder(chooser, dir);
        g_free(dir);
    }
    if (videos_only) {
        GtkFileFilter *f = gtk_file_filter_new();
        gtk_file_filter_set_name(f, "Videos");
        gtk_file_filter_add_mime_type(f, "video/*");
        for (int i = 0; VIDEO_EXT[i]; i++) {
            char *pattern_lower = g_strdup_printf("*.%s", VIDEO_EXT[i]),
                 *pattern_upper = g_ascii_strup(pattern_lower, -1);
            gtk_file_filter_add_pattern(f, pattern_lower);
            gtk_file_filter_add_pattern(f, pattern_upper);
            g_free(pattern_lower);
            g_free(pattern_upper);
        }
        gtk_file_chooser_add_filter(chooser, f);
        GtkFileFilter *all_files = gtk_file_filter_new();
        gtk_file_filter_set_name(all_files, "Alle Dateien");
        gtk_file_filter_add_pattern(all_files, "*");
        gtk_file_chooser_add_filter(chooser, all_files);
    }
    char *result = NULL;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
        result = gtk_file_chooser_get_filename(chooser);
    g_object_unref(dialog);
    return result;
}

/* übernimmt f (g_free) */
static void set_ref_video(char *f)
{
    g_free(ui.ref_video);
    ui.ref_video = f;
    if (!ui.src_dir) {                          /* Quellordner vorschlagen */
        ui.src_dir = g_path_get_dirname(f);
    }
    update_setup_labels();
    schedule_ref_preview();
}

/* Knopf "Auswählen …" beim Referenz-Video */
static void on_choose_ref(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    char *f = choose_path(outro_selected() ? "Referenz-Video mit dem Outro"
                                           : "Referenz-Video mit dem Intro",
                          GTK_FILE_CHOOSER_ACTION_OPEN, ui.ref_video, true);
    if (f) set_ref_video(f);
}

/* Knopf "Auswählen …" beim Quellordner */
static void on_choose_src(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    char *f = choose_path("Quellordner mit den Videos", GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                          ui.src_dir ? ui.src_dir : ui.ref_video, false);
    if (!f) return;
    g_free(ui.src_dir);
    ui.src_dir = f;
    update_setup_labels();
}

/* Knopf "Ändern …" beim Ausgabeordner */
static void on_choose_out(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    char *current_out = effective_out_dir();
    char *f = choose_path("Ausgabeordner", GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                          current_out && is_dir(current_out) ? current_out : ui.src_dir, false);
    g_free(current_out);
    if (!f) return;
    g_free(ui.out_dir_custom);
    ui.out_dir_custom = f;
    update_setup_labels();
}

/* ---------------------------------------------------------------------------
 * Drag & Drop aus dem Dateimanager
 * ------------------------------------------------------------------------- */
static bool looks_like_video(const char *path)
{
    const char *base = strrchr(path, '/'), *dot = strrchr(base ? base : path, '.');
    if (dot)
        for (int i = 0; VIDEO_EXT[i]; i++)
            if (!g_ascii_strcasecmp(dot + 1, VIDEO_EXT[i])) return true;
    char *content_type = g_content_type_guess(path, NULL, 0, NULL),
         *mime = g_content_type_get_mime_type(content_type);
    bool v = mime && g_str_has_prefix(mime, "video/");
    g_free(mime);
    g_free(content_type);
    return v;
}

/* Eintrag einer Drop-Liste (URI oder Pfad) -> lokaler Pfad, NULL wenn keiner */
static char *dropped_item_path(const char *item)
{
    char *t = g_strstrip(g_strdup(item)), *path = NULL;
    if (*t == '/') {
        path = g_strdup(t);
    } else if (!strncmp(t, "~/", 2)) {
        path = g_build_filename(g_get_home_dir(), t + 2, NULL);
    } else if (*t != '#' && g_uri_peek_scheme(t)) {    /* '#': Kommentar in text/uri-list */
        GFile *f = g_file_new_for_uri(t);
        path = g_file_get_path(f);              /* Netzwerk über gvfs: FUSE-Pfad; http: NULL */
        g_object_unref(f);
    }
    g_free(t);
    return path;
}

/* erster lokaler Pfad aus den Drop-Daten (text/uri-list, sonst Text) */
static char *first_dropped_path(GtkSelectionData *selection)
{
    char **items = gtk_selection_data_get_uris(selection), *path = NULL;
    if (!items) {
        char *text = (char *)gtk_selection_data_get_text(selection);
        items = text ? g_strsplit(text, "\n", -1) : NULL;
        g_free(text);
    }
    for (int i = 0; items && items[i] && !path; i++) path = dropped_item_path(items[i]);
    g_strfreev(items);
    return path;
}

/* Datei oder Ordner wurde auf eine Zeile gezogen */
static void on_drop(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, GtkSelectionData *selection,
                    guint info, guint time, gpointer data)
{
    (void)w;
    (void)ctx;
    (void)x;
    (void)y;
    (void)info;
    (void)time;
    int target = GPOINTER_TO_INT(data);
    char *path = first_dropped_path(selection), *msg = NULL;
    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS)) {
        show_info_bar(GTK_MESSAGE_WARNING,
                      "Bitte eine Datei oder einen Ordner aus dem Dateimanager "
                      "ziehen – nur lokale bzw. eingehängte Pfade sind möglich.");
        g_free(path);
        return;
    }
    char *name = g_filename_display_basename(path);
    if (target == DROP_REF) {
        if (is_dir(path))
            msg = g_strdup_printf("„%s“ ist ein Ordner – als Referenz bitte ein Video ziehen. "
                                  "Ordner gehören zu „Quellordner“.", name);
        else if (!looks_like_video(path))
            msg = g_strdup_printf("„%s“ ist kein Video.", name);
        if (!msg) {
            set_ref_video(path);
            path = NULL;
        }
    } else {                                    /* Datei gezogen -> ihr Ordner */
        char *dir = is_dir(path) ? g_strdup(path) : g_path_get_dirname(path);
        char **slot = target == DROP_SRC ? &ui.src_dir : &ui.out_dir_custom;
        g_free(*slot);
        *slot = dir;
        update_setup_labels();
    }
    show_info_bar(GTK_MESSAGE_WARNING, msg);    /* NULL: alte Meldung schließen */
    g_free(msg);
    g_free(name);
    g_free(path);
}

/* Zeile als Ziel für Drag & Drop aus dem Dateimanager einrichten */
static void make_drop_target(GtkWidget *w, int target, const char *tooltip)
{
    gtk_drag_dest_set(w, GTK_DEST_DEFAULT_ALL, NULL, 0, GDK_ACTION_COPY | GDK_ACTION_LINK);
    gtk_drag_dest_add_uri_targets(w);
    GtkTargetList *targets = gtk_drag_dest_get_target_list(w);
    gtk_target_list_add_text_targets(targets, 1);
    g_signal_connect(w, "drag-data-received", G_CALLBACK(on_drop), GINT_TO_POINTER(target));
    gtk_widget_set_tooltip_text(w, tooltip);
}

/* Wechsel zwischen Intro, Outro und automatisch: Intro und Outro behalten ihre Länge */
static void on_kind_toggled(GtkToggleButton *t, gpointer d)
{
    int kind = GPOINTER_TO_INT(d);
    if (!gtk_toggle_button_get_active(t) || kind == ui.kind_shown) return;
    if (ui.kind_shown < 2) ui.kind_length[ui.kind_shown] = selected_length();
    ui.kind_shown = kind;
    if (kind < 2) gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.length_spin), ui.kind_length[kind]);
    show_info_bar(GTK_MESSAGE_INFO, NULL);
    update_setup_labels();
    if (kind < 2) schedule_ref_preview();
    settings_changed();
}

/* Einstellung mit Einfluss auf die Texte geändert */
static void on_setup_changed(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
    update_setup_labels();
    settings_changed();
}

/* Länge geändert: Texte und Vergleichsbild aktualisieren */
static void on_length_changed(GtkSpinButton *spin, gpointer d)
{
    (void)spin;
    (void)d;
    update_setup_labels();
    schedule_ref_preview();
    settings_changed();
}

/* Einstellung ohne Einfluss auf die Texte geändert: nur speichern */
static void on_setting_changed(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
    settings_changed();
}

/* Genauigkeit der Erkennung geändert */
static void on_accuracy_changed(GtkComboBox *c, gpointer d)
{
    (void)c;
    (void)d;
    update_setup_labels();
    settings_changed();
}

/* "Wenn fertig" gibt es auf der Einstellungs- und der Bearbeitungsseite */
static void on_after_changed(GtkComboBox *c, gpointer d)
{
    (void)d;
    int a = gtk_combo_box_get_active(c);
    GtkWidget *other = GTK_WIDGET(c) == ui.after_combo ? ui.process_after_combo : ui.after_combo;
    if (other && gtk_combo_box_get_active(GTK_COMBO_BOX(other)) != a)
        gtk_combo_box_set_active(GTK_COMBO_BOX(other), a);
    settings_changed();
}

/* Auswahl "Wenn fertig" (gibt es auf zwei Seiten, beide bleiben synchron) */
static GtkWidget *new_after_action_combo(void)
{
    GtkWidget *c = gtk_combo_box_text_new();
    for (int i = 0; i < 3; i++)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), NULL, AFTER_TEXT[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(c), AFTER_NOTHING);
    g_signal_connect(c, "changed", G_CALLBACK(on_after_changed), NULL);
    return c;
}

/* Eingabe mit Komma oder Punkt ("2,5" wie "2.5") */
static gint on_length_input(GtkSpinButton *spin, gdouble *val, gpointer d)
{
    (void)d;
    char *s = g_strdup(gtk_entry_get_text(GTK_ENTRY(spin))), *end;
    g_strdelimit(s, ",", '.');
    g_strstrip(s);
    double v = g_ascii_strtod(s, &end);
    bool ok = end != s && (*end == 0 || !strcmp(end, "s"));
    g_free(s);
    if (!ok) return GTK_INPUT_ERROR;
    *val = v;
    return TRUE;
}

/* Klick auf eine Zeile des Schnittmodus wählt ihn aus */
static void on_mode_row_activated(GtkListBox *list, GtkListBoxRow *r, gpointer d)
{
    (void)list;
    (void)d;
    for (int i = 0; i < 4; i++)
        if (GTK_WIDGET(r) == ui.mode_rows[i] && gtk_widget_get_sensitive(ui.mode_rows[i]))
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.mode_buttons[i]), TRUE);
}

static void on_search_clicked(GtkButton *b, gpointer d);

/* Seite 1: Einstellungen */
static GtkWidget *build_setup_page(void)
{
    GtkWidget *scroller, *box = new_page_column(&scroller), *list, *button;
    GtkWidget *lead = new_label("Findet ein wiederkehrendes Intro oder Outro in vielen Videos und "
                                "schneidet es heraus – alle Tonspuren, Untertitel, Kapitel und "
                                "Cover bleiben erhalten.", "ic-lead");
    set_wrapping(lead, 80);
    add_css_class(lead, "dim-label");
    gtk_box_pack_start(GTK_BOX(box), lead, FALSE, FALSE, 6);

    /* Was soll weg: Umschalter im Stil einer Segment-Auswahl */
    GtkWidget *kind_switch = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_set_homogeneous(GTK_BOX(kind_switch), TRUE);
    add_css_class(kind_switch, "linked");
    add_css_class(kind_switch, "ic-kind");
    gtk_widget_set_halign(kind_switch, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(kind_switch, 8);
    static const char *const kind_text[3] = {"Intro am Anfang", "Outro am Ende", "Automatisch"};
    static const char *const kind_tip[3] = {"Den wiederkehrenden Vorspann am Anfang entfernen",
                                            "Den wiederkehrenden Abspann am Ende entfernen",
                                            "Wiederkehrende Intros selbst finden – ohne "
                                            "Referenz-Video, auch mehrere verschiedene"};
    for (int k = 0; k < 3; k++) {
        /* der erste Knopf eröffnet die Gruppe, die übrigen treten ihr bei */
        GtkRadioButton *group_member = k ? GTK_RADIO_BUTTON(ui.kind_buttons[0]) : NULL;
        ui.kind_buttons[k] =
            gtk_radio_button_new_with_label_from_widget(group_member, kind_text[k]);
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(ui.kind_buttons[k]), FALSE);
        gtk_widget_set_tooltip_text(ui.kind_buttons[k], kind_tip[k]);
        gtk_box_pack_start(GTK_BOX(kind_switch), ui.kind_buttons[k], TRUE, TRUE, 0);
    }
    ui.kind_length[0] = ui.kind_length[1] = 5;
    for (int k = 0; k < 3; k++)
        g_signal_connect(ui.kind_buttons[k], "toggled", G_CALLBACK(on_kind_toggled),
                         GINT_TO_POINTER(k));
    gtk_box_pack_start(GTK_BOX(box), kind_switch, FALSE, FALSE, 0);

    list = add_group_with_heading(box, "Intro", &ui.kind_heading);
    ui.ref_preview = gtk_image_new_from_icon_name("video-x-generic-symbolic", GTK_ICON_SIZE_DND);
    gtk_widget_set_size_request(ui.ref_preview, 128, 72);
    add_css_class(ui.ref_preview, "ic-thumb");
    button = gtk_button_new_with_label("Auswählen …");
    g_signal_connect(button, "clicked", G_CALLBACK(on_choose_ref), NULL);
    ui.ref_row = add_row(list, "Referenz-Video", "", ui.ref_preview, button, NULL,
                         &ui.ref_subtitle);
    make_drop_target(ui.ref_row, DROP_REF, "Video auswählen oder aus dem Dateimanager hierher "
                     "ziehen");
    ui.length_spin = gtk_spin_button_new_with_range(0.1, 3600, 0.5);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ui.length_spin), 3);   /* Millisekunden */
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.length_spin), 5);
    g_signal_connect(ui.length_spin, "input", G_CALLBACK(on_length_input), NULL);
    g_signal_connect(ui.length_spin, "value-changed", G_CALLBACK(on_length_changed), NULL);
    GtkWidget *length_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(length_box), ui.length_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(length_box), new_label("Sekunden", "dim-label"), FALSE, FALSE, 0);
    ui.mark_button = gtk_button_new_with_label("Im Video festlegen …");
    gtk_widget_set_margin_start(ui.mark_button, 6);
    g_signal_connect(ui.mark_button, "clicked", G_CALLBACK(on_mark_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(length_box), ui.mark_button, FALSE, FALSE, 0);
    add_row(list, "", "", NULL, length_box, &ui.length_title, &ui.length_subtitle);
    ui.intro_frame = gtk_widget_get_parent(list);

    /* automatische Erkennung: Suchlänge und wie oft ein Intro vorkommen muss */
    list = add_group_with_heading(box, "Automatische Erkennung", &ui.auto_heading);
    ui.auto_frame = gtk_widget_get_parent(list);
    ui.auto_max_spin = gtk_spin_button_new_with_range(3, 120, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.auto_max_spin), IC_AUTO_MAX_DEFAULT);
    g_signal_connect(ui.auto_max_spin, "value-changed", G_CALLBACK(on_setting_changed), NULL);
    GtkWidget *max_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(max_box), ui.auto_max_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(max_box), new_label("Sekunden", "dim-label"), FALSE, FALSE, 0);
    add_row(list, "Intro höchstens",
            "Gesucht wird am Anfang jedes Videos bis zu dieser Länge (Standard: "
            STR(IC_AUTO_MAX_DEFAULT) " s). Längere Intros werden nicht geschnitten.", NULL,
            max_box, NULL, NULL);
    ui.auto_min_spin = gtk_spin_button_new_with_range(2, 100, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.auto_min_spin), IC_AUTO_MIN_VIDEOS);
    g_signal_connect(ui.auto_min_spin, "value-changed", G_CALLBACK(on_setting_changed), NULL);
    GtkWidget *min_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(min_box), ui.auto_min_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(min_box), new_label("Videos", "dim-label"), FALSE, FALSE, 0);
    add_row(list, "Kommt vor in mindestens",
            "So viele Videos müssen mit demselben Intro beginnen, damit es als sicher gilt "
            "(Standard: " STR(IC_AUTO_MIN_VIDEOS) "). Seltenere schlägt Intro-Cutter nur vor, "
            "ohne sie auszuwählen. Geschnitten wird je Video bildgenau; im Zweifel lieber nicht.",
            NULL, min_box, NULL, NULL);
    /* Sichtbarkeit steuert update_setup_labels; show_all des Fensters lässt sie in Ruhe */
    GtkWidget *switchable[4] = {ui.kind_heading, ui.intro_frame, ui.auto_heading, ui.auto_frame};
    for (int i = 0; i < 4; i++) {
        gtk_widget_show_all(switchable[i]);
        gtk_widget_set_no_show_all(switchable[i], TRUE);
    }

    list = add_group(box, "Videos");
    button = gtk_button_new_with_label("Auswählen …");
    g_signal_connect(button, "clicked", G_CALLBACK(on_choose_src), NULL);
    ui.src_row = add_row(list, "Quellordner", "", NULL, button, NULL, &ui.src_subtitle);
    make_drop_target(ui.src_row, DROP_SRC, "Ordner auswählen oder aus dem Dateimanager hierher "
                     "ziehen (eine gezogene Datei zählt als ihr Ordner)");
    gtk_label_set_ellipsize(GTK_LABEL(ui.src_subtitle), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(ui.src_subtitle), 60);
    gtk_label_set_line_wrap(GTK_LABEL(ui.src_subtitle), FALSE);
    ui.replace_switch = gtk_switch_new();
    g_signal_connect(ui.replace_switch, "notify::active", G_CALLBACK(on_setup_changed), NULL);
    add_row(list, "Originale ersetzen", "Die geschnittene Datei ersetzt das Original. Ohne diese "
            "Option landen die Ergebnisse im Ausgabeordner.", NULL, ui.replace_switch, NULL, NULL);
    ui.out_button = gtk_button_new_with_label("Ändern …");
    g_signal_connect(ui.out_button, "clicked", G_CALLBACK(on_choose_out), NULL);
    ui.out_row = add_row(list, "Ausgabeordner", "", NULL, ui.out_button, NULL, &ui.out_subtitle);
    make_drop_target(ui.out_row, DROP_OUT, "Ordner auswählen oder aus dem Dateimanager hierher "
                     "ziehen");
    gtk_label_set_ellipsize(GTK_LABEL(ui.out_subtitle), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(ui.out_subtitle), 60);
    gtk_label_set_line_wrap(GTK_LABEL(ui.out_subtitle), FALSE);

    list = add_group(box, "Schnittmodus");
    static const char *const title[4] = {"Schnell & verlustfrei (Copy)", "Framegenau per CPU",
                                         "Framegenau per GPU", "Framegenau per GPU + CPU parallel"};
    static const char *const desc[4] = {
        "",                                     /* Copy: je nach Intro/Outro, siehe oben */
        "x264, AV1-Quellen und WebM: SVT-AV1 – exakter Schnitt ohne Inhaltsverlust, aber "
        "langsam.",
        NULL,
        "Maximales Tempo: die GPU nimmt die größten Dateien (Codec wie die Quelle), die CPU "
        "parallel die kleinsten (x264, mit niedriger Priorität). Ergebnis: gemischte Codecs; "
        "der PC ist dabei stark ausgelastet (GPU höchstens " STR(GPU_TARGET) " %)."};
    ui.gpu_spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(ui.gpu_spinner));
    for (int i = 0; i < 4; i++) {
        /* der erste Knopf eröffnet die Gruppe, die übrigen treten ihr bei */
        GtkRadioButton *group_member = i ? GTK_RADIO_BUTTON(ui.mode_buttons[0]) : NULL;
        ui.mode_buttons[i] = gtk_radio_button_new_from_widget(group_member);
        GtkWidget *subtitle;
        char *d = i == 2 ? g_strdup_printf("%s\nGPU wird geprüft …", GPU_DESC) : g_strdup(desc[i]);
        ui.mode_rows[i] = add_row(list, title[i], d, ui.mode_buttons[i],
                                  i == 2 ? ui.gpu_spinner : NULL, NULL, &subtitle);
        g_free(d);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(ui.mode_rows[i]), TRUE);
        if (i == 0) ui.copy_subtitle = subtitle;
        if (i == 2) ui.gpu_subtitle = subtitle;
    }
    gtk_widget_set_sensitive(ui.mode_rows[2], FALSE);   /* GPU-Modi erst nach der GPU-Prüfung */
    gtk_widget_set_sensitive(ui.mode_rows[3], FALSE);
    g_signal_connect(list, "row-activated", G_CALLBACK(on_mode_row_activated), NULL);
    for (int i = 0; i < 4; i++)
        g_signal_connect(ui.mode_buttons[i], "toggled", G_CALLBACK(on_setting_changed), NULL);

    list = add_group(box, "Erkennung");
    ui.threshold_spin = gtk_spin_button_new_with_range(0, 64, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.threshold_spin), DEFAULT_THRESHOLD);
    g_signal_connect(ui.threshold_spin, "value-changed", G_CALLBACK(on_setting_changed), NULL);
    ui.threshold_row = add_row(list, "Schwellwert",
            "Erlaubte Abweichung vom Referenz-Frame (Hamming-Distanz): kleiner = strenger, "
            "größer = toleranter. Standard: 4.", NULL, ui.threshold_spin, NULL, NULL);
    ui.accuracy_combo = gtk_combo_box_text_new();
    for (int i = 0; i < 4; i++)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui.accuracy_combo), NULL, ACC_TEXT[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(ui.accuracy_combo), 0);
    g_signal_connect(ui.accuracy_combo, "changed", G_CALLBACK(on_accuracy_changed), NULL);
    ui.accuracy_row = add_row(list, "Genauigkeit",
            "Mehr Vergleichsbilder, verteilt über das ganze Intro bzw. Outro: weniger Fehltreffer "
            "und weniger übersehene Videos (z. B. bei Einblendungen). Die Suche dauert beim ersten "
            "Mal etwa so viel länger, wie es Bilder sind – danach kommt das meiste aus dem Cache.",
            NULL, ui.accuracy_combo, NULL, NULL);
    for (int i = 0; i < 2; i++) {
        GtkWidget *row = i ? ui.accuracy_row : ui.threshold_row;
        gtk_widget_show_all(row);
        gtk_widget_set_no_show_all(row, TRUE);
    }
    button = gtk_button_new_with_label("Anzeigen …");
    gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
    g_signal_connect(button, "clicked", G_CALLBACK(on_cache_clicked), NULL);
    add_row(list, "Hash-Cache", "", NULL, button, NULL, &ui.cache_subtitle);

    list = add_group(box, "Nach der Bearbeitung");
    ui.after_combo = new_after_action_combo();
    add_row(list, "Wenn alles fertig ist", "Herunterfahren bzw. Beenden erst nach einem Countdown, "
            "der sich abbrechen lässt – und nur, wenn die Bearbeitung vollständig durchlief.", NULL,
            ui.after_combo, NULL, NULL);

    /* Hauptaktion immer sichtbar in der Leiste unten */
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(outer), scroller, TRUE, TRUE, 0);
    GtkWidget *action_bar = gtk_action_bar_new();
    ui.setup_hint = new_label("", "dim-label");
    gtk_widget_set_margin_start(ui.setup_hint, 12);
    gtk_action_bar_pack_start(GTK_ACTION_BAR(action_bar), ui.setup_hint);
    ui.search_button = new_pill_button("Intro suchen", "suggested-action");
    gtk_widget_set_margin_top(ui.search_button, 6);
    gtk_widget_set_margin_bottom(ui.search_button, 6);
    gtk_widget_set_margin_end(ui.search_button, 12);
    g_signal_connect(ui.search_button, "clicked", G_CALLBACK(on_search_clicked), NULL);
    gtk_action_bar_pack_end(GTK_ACTION_BAR(action_bar), ui.search_button);
    gtk_box_pack_end(GTK_BOX(outer), action_bar, FALSE, FALSE, 0);
    return outer;
}

/* ---------------------------------------------------------------------------
 * Seite 2: Suche läuft
 * ------------------------------------------------------------------------- */
static gboolean on_pulse_tick(gpointer d)
{
    (void)d;
    if (ui.search_started) {
        ui.pulse_timer = 0;
        return G_SOURCE_REMOVE;
    }
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(ui.search_progress));
    return G_SOURCE_CONTINUE;
}

/* Suche abbrechen */
static void on_search_cancel_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    ic_cancel = 1;
    gtk_button_set_label(b, "Wird abgebrochen …");
    gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
}

/* Widget zentrieren; Labels zentriert umbrechen */
static GtkWidget *center_widget(GtkWidget *w)
{
    if (GTK_IS_LABEL(w)) {
        gtk_label_set_xalign(GTK_LABEL(w), 0.5);
        gtk_label_set_justify(GTK_LABEL(w), GTK_JUSTIFY_CENTER);
        set_wrapping(w, 60);
    }
    gtk_widget_set_halign(w, GTK_ALIGN_CENTER);
    return w;
}

/* Seite 2: Suche läuft */
static GtkWidget *build_search_page(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(box, 460, -1);      /* Breite folgt dem Fenster */
    ui.search_box = box;
    GtkWidget *spinner = gtk_spinner_new();
    gtk_widget_set_size_request(spinner, 40, 40);
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_box_pack_start(GTK_BOX(box), spinner, FALSE, FALSE, 0);
    ui.search_title = center_widget(new_label("Suche läuft …", "ic-hero"));
    ui.search_subtitle = center_widget(new_label("", "dim-label"));
    ui.search_progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(ui.search_progress), TRUE);
    ui.search_detail = center_widget(new_label("", "dim-label"));
    add_css_class(ui.search_detail, "ic-sub");
    ui.search_cancel_button = center_widget(gtk_button_new_with_label("Abbrechen"));
    gtk_widget_set_margin_top(ui.search_cancel_button, 12);
    g_signal_connect(ui.search_cancel_button, "clicked",
                     G_CALLBACK(on_search_cancel_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), ui.search_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.search_subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.search_progress, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.search_detail, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.search_cancel_button, FALSE, FALSE, 0);
    return box;
}

/* ---------------------------------------------------------------------------
 * Seite 3: Treffer
 * ------------------------------------------------------------------------- */
/* Anzahl ausgewählter Videos – auch der (noch) nicht angezeigten Zeilen */
static int selected_count(void)
{
    int n = 0;
    for (int i = 0; i < ui.candidate_count; i++) n += ui.result_selected[i];
    return n;
}

/* Ergebnis-Index, zu dem ein Häkchen gehört (-1, wenn keiner) */
static int check_result_index(GtkToggleButton *check)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(check), "index")) - 1;
}

/* Anzahl ausgewählter Videos und Beschriftung des Schneiden-Knopfs aktualisieren */
static void update_selection_count(void)
{
    int n = selected_count();
    char *count_text = g_strdup_printf("%d von %d ausgewählt", n, ui.candidate_count);
    char *button_text = n == 1 ? g_strdup("1 Video schneiden")
                               : g_strdup_printf("%d Videos schneiden", n);
    set_label_text(ui.selection_label, count_text);
    gtk_button_set_label(GTK_BUTTON(ui.cut_button), button_text);
    gtk_widget_set_sensitive(ui.cut_button, n > 0);
    g_free(count_text);
    g_free(button_text);
}

/* Häkchen eines Videos geändert: in die Auswahl übernehmen */
static void on_check_toggled(GtkToggleButton *check, gpointer d)
{
    (void)d;
    int index = check_result_index(check);
    if (index >= 0 && index < ui.candidate_count)
        ui.result_selected[index] = gtk_toggle_button_get_active(check);
    if (!ui.bulk_selecting) update_selection_count();
}

/* select_matches: alle Treffer auswählen (nicht erkannte Videos bleiben, wie sie sind);
 * false: nichts auswählen. Gilt auch für Zeilen, die (noch) nicht angezeigt werden. */
static void select_videos(bool select_matches)
{
    for (int i = 0; i < ui.candidate_count; i++)
        if (!select_matches || i < ui.detection.nmatch) ui.result_selected[i] = select_matches;
    ui.bulk_selecting = true;
    for (guint i = 0; i < ui.result_checks->len; i++) {
        GtkToggleButton *check = g_ptr_array_index(ui.result_checks, i);
        gtk_toggle_button_set_active(check, ui.result_selected[check_result_index(check)]);
    }
    ui.bulk_selecting = false;
    update_selection_count();
}

/* Knopf "Alle Treffer auswählen" */
static void on_select_matches_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    select_videos(true);
}

/* Knopf "Keine auswählen" */
static void on_select_none_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    select_videos(false);
}

/* Video im Standard-Player öffnen */
static void on_play_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    open_in_default_app(((const Video *)d)->path);
}

/* Klick auf eine Zeile setzt bzw. entfernt ihr Häkchen */
static void on_result_row_activated(GtkListBox *list, GtkListBoxRow *r, gpointer d)
{
    (void)list;
    (void)d;
    GtkToggleButton *check = g_object_get_data(G_OBJECT(r), "check");
    if (check) gtk_toggle_button_set_active(check, !gtk_toggle_button_get_active(check));
}

/* ---------------------------------------------------------------------------
 * Vorschau-Leiste der nicht erkannten Videos: Bilder bei 1 s, 2 s, 5 s sowie
 * 1/30 und 1/10 der Laufzeit (Outro: so weit vor dem Ende). Geladen wird erst,
 * wenn die Leiste sichtbar wird – im Hintergrund, mit THUMB_THREADS ffmpeg-
 * Aufrufen gleichzeitig.
 * ------------------------------------------------------------------------- */
typedef struct {
    GtkWidget *area;
    char *path;
    bool queued;
    int ew;                     /* Breite, in der zuletzt geladen wurde */
    int req_h;                  /* angeforderte Höhe (folgt der Breite) */
    int n;                      /* Bilder in der Leiste (-1: Laufzeit noch unbekannt) */
    char label[THUMB_N][24];
    GdkPixbuf *pix[THUMB_N];
    bool failed[THUMB_N];
} PreviewStrip;

typedef struct {                /* Auftrag an den Thread-Pool */
    int gen, idx;
    char *path;
    bool outro;
    int w, h;                   /* Bildgröße */
} PreviewJob;

typedef struct {                /* Ergebnis an den Hauptthread */
    int gen, idx, slot;         /* slot -1: Zeitpunkte/Beschriftungen */
    int n, w, h;
    char label[THUMB_N][24];
    guchar *rgb;                /* NULL: Bild nicht lesbar */
} PreviewImage;

/* Breite eines Vorschaubilds bei aw Pixel Leistenbreite */
static int preview_image_width(int strip_width)
{
    return CLAMP((strip_width - (THUMB_N - 1) * THUMB_GAP) / THUMB_N, THUMB_MIN_W, THUMB_MAX_W);
}

/* Vorschauleiste freigeben */
static void preview_strip_free(gpointer p)
{
    PreviewStrip *strip = p;
    for (int i = 0; i < THUMB_N; i++)
        if (strip->pix[i]) g_object_unref(strip->pix[i]);
    g_free(strip->path);
    g_free(strip);
}

/* "0,3 s", "5 s", "1:05" bzw. vor dem Ende "−5 s", "−1:05" */
static void preview_time_label(double t, bool minus, char *out, size_t out_size)
{
    t = fmin(fmax(t, 0.0), 359999.0);
    int whole_secs = (int)lround(t), tenth = (int)lround(t * 10);
    const char *sign = minus ? "\u2212" : "";
    if (t < 9.95 && tenth % 10)                 /* kurze Videos: eine Nachkommastelle */
        snprintf(out, out_size, "%s%d,%d s", sign, tenth / 10, tenth % 10);
    else if (whole_secs < 60)
        snprintf(out, out_size, "%s%d s", sign, whole_secs);
    else
        snprintf(out, out_size, "%s%d:%02d", sign, (whole_secs / 60) % 10000, whole_secs % 60);
}

/* Zeitpunkte ab Dateistart, chronologisch, ohne Doppelte -> Anzahl */
static int preview_times(double duration, bool outro, double *times, char labels[][24])
{
    double back[THUMB_N] = {1, 2, 5, duration / 30, duration / 10};
    int n_times = duration > 0 ? THUMB_N : 3;
    if (outro && duration <= 0) return 0;
    double candidates[THUMB_N];
    for (int i = 0; i < n_times; i++) candidates[i] = outro ? duration - back[i] : back[i];
    for (int i = 1; i < n_times; i++)           /* aufsteigend sortieren */
        for (int j = i; j > 0 && candidates[j] < candidates[j - 1]; j--) {
            double x = candidates[j];
            candidates[j] = candidates[j - 1];
            candidates[j - 1] = x;
        }
    int n = 0;
    for (int i = 0; i < n_times; i++) {
        double x = candidates[i];
        if (duration > 0) x = fmin(x, fmax(0.0, duration - 0.1));
        if (x < 0) x = 0;
        if (n && fabs(x - times[n - 1]) < 0.04) continue; /* gleicher Frame */
        times[n] = x;
        preview_time_label(outro ? duration - x : x, outro, labels[n], sizeof labels[n]);
        n++;
    }
    return n;
}

/* Pixeldaten eines Vorschaubilds freigeben (wenn GdkPixbuf sie nicht mehr braucht) */
static void free_pixels(guchar *pixels, gpointer data)
{
    (void)data;
    g_free(pixels);
}

/* im Hauptthread: Zeitpunkte bzw. ein fertiges Vorschaubild übernehmen */
static gboolean on_idle_preview_image(gpointer p)
{
    PreviewImage *d = p;
    if (ui.win && d->gen == g_atomic_int_get(&ui.preview_generation) &&
        d->idx < (int)ui.preview_strips->len) {
        PreviewStrip *strip = g_ptr_array_index(ui.preview_strips, d->idx);
        if (d->slot < 0) {
            strip->n = d->n;
            memcpy(strip->label, d->label, sizeof strip->label);
        } else if (d->rgb) {
            if (strip->pix[d->slot]) g_object_unref(strip->pix[d->slot]); /* schärfer nachgeladen */
            strip->pix[d->slot] = gdk_pixbuf_new_from_data(d->rgb, GDK_COLORSPACE_RGB, FALSE, 8,
                                                           d->w, d->h, d->w * 3, free_pixels, NULL);
            d->rgb = NULL;                      /* gehört jetzt dem Pixbuf */
            strip->failed[d->slot] = false;
        } else if (!strip->pix[d->slot]) {
            strip->failed[d->slot] = true;
        }
        gtk_widget_queue_draw(strip->area);
    }
    g_free(d->rgb);
    g_free(d);
    return G_SOURCE_REMOVE;
}

/* Thread-Pool: Laufzeit ermitteln und die Vorschaubilder einer Leiste lesen */
static void preview_worker(gpointer data, gpointer user)
{
    (void)user;
    PreviewJob *j = data;
    double t[THUMB_N];
    PreviewImage *d = g_new0(PreviewImage, 1);
    *d = (PreviewImage){.gen = j->gen, .idx = j->idx, .slot = -1};
    int n = 0;
    if (j->gen == g_atomic_int_get(&ui.preview_generation)) {
        n = d->n = preview_times(ic_duration(j->path), j->outro, t, d->label);
        g_idle_add(on_idle_preview_image, d);
    } else {
        g_free(d);
    }
    for (int i = 0; i < n && j->gen == g_atomic_int_get(&ui.preview_generation); i++) {
        d = g_new0(PreviewImage, 1);
        *d = (PreviewImage){.gen = j->gen, .idx = j->idx, .slot = i, .w = j->w, .h = j->h};
        d->rgb = g_malloc((size_t)j->w * j->h * 3);
        if (!ic_frame_rgb(j->path, t[i], j->w, j->h, d->rgb)) g_clear_pointer(&d->rgb, g_free);
        g_idle_add(on_idle_preview_image, d);
    }
    g_free(j->path);
    g_free(j);
}

/* abgerundetes Rechteck als Pfad */
static void path_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

/* Vorschauleiste zeichnen; beim ersten Sichtbarwerden das Laden anstoßen */
static gboolean on_preview_strip_draw(GtkWidget *w, cairo_t *cr, gpointer data)
{
    int strip_index = GPOINTER_TO_INT(data);
    if (strip_index >= (int)ui.preview_strips->len) return FALSE;
    PreviewStrip *strip = g_ptr_array_index(ui.preview_strips, strip_index);
    int thumb_w = preview_image_width(gtk_widget_get_allocated_width(w)),
        thumb_h = thumb_w * 9 / 16;
    /* erst laden, wenn sichtbar; ist das Fenster deutlich gewachsen, schärfer nachladen */
    if (!strip->queued || (strip->ew && thumb_w > strip->ew * 3 / 2)) {
        int load_w = CLAMP((thumb_w + 15) / 16 * 16, 96, THUMB_MAX_W);
        strip->queued = true;
        strip->ew = load_w;
        PreviewJob *j = g_new0(PreviewJob, 1);
        *j = (PreviewJob){g_atomic_int_get(&ui.preview_generation), strip_index,
                          g_strdup(strip->path), ui.run_outro, load_w, load_w * 9 / 16 / 2 * 2};
        g_thread_pool_push(ui.preview_pool, j, NULL);
    }
    GtkStyleContext *ctx = gtk_widget_get_style_context(w);
    GdkRGBA fg_color;
    gtk_style_context_get_color(ctx, gtk_style_context_get_state(ctx), &fg_color);
    PangoLayout *layout = gtk_widget_create_pango_layout(w, NULL);
    PangoFontDescription *font = pango_font_description_copy(
        pango_context_get_font_description(pango_layout_get_context(layout)));
    if (pango_font_description_get_size_is_absolute(font))
        pango_font_description_set_absolute_size(font, pango_font_description_get_size(font) * 0.8);
    else
        pango_font_description_set_size(font, pango_font_description_get_size(font) * 8 / 10);
    pango_layout_set_font_description(layout, font);
    int slots = strip->n < 0 ? THUMB_N : strip->n;
    for (int i = 0; i < slots; i++) {
        double x = i * (thumb_w + THUMB_GAP);
        cairo_save(cr);
        path_rounded_rect(cr, x, 0, thumb_w, thumb_h, 4);
        if (strip->n >= 0 && strip->pix[i]) {
            cairo_clip(cr);
            int pix_w = gdk_pixbuf_get_width(strip->pix[i]),
                pix_h = gdk_pixbuf_get_height(strip->pix[i]);
            cairo_translate(cr, x, 0);
            cairo_scale(cr, (double)thumb_w / pix_w, (double)thumb_h / pix_h);
            gdk_cairo_set_source_pixbuf(cr, strip->pix[i], 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
            cairo_paint(cr);
        } else {                                /* Platzhalter: lädt bzw. nicht lesbar */
            cairo_set_source_rgba(cr, fg_color.red, fg_color.green, fg_color.blue, 0.08);
            cairo_fill(cr);
            if (strip->n >= 0 && strip->failed[i]) {
                pango_layout_set_text(layout, "–", -1);
                int text_w, text_h;
                pango_layout_get_pixel_size(layout, &text_w, &text_h);
                cairo_set_source_rgba(cr, fg_color.red, fg_color.green, fg_color.blue, 0.5);
                cairo_move_to(cr, x + (thumb_w - text_w) / 2.0, (thumb_h - text_h) / 2.0);
                pango_cairo_show_layout(cr, layout);
            }
        }
        cairo_restore(cr);
        if (strip->n > 0) {
            pango_layout_set_text(layout, strip->label[i], -1);
            int text_w, text_h;
            pango_layout_get_pixel_size(layout, &text_w, &text_h);
            cairo_set_source_rgba(cr, fg_color.red, fg_color.green, fg_color.blue, 0.65);
            cairo_move_to(cr, x + (thumb_w - text_w) / 2.0, thumb_h + 2);
            pango_cairo_show_layout(cr, layout);
        }
    }
    if (strip->n == 0) {
        pango_layout_set_text(layout, "Keine Vorschau (Laufzeit unbekannt)", -1);
        cairo_set_source_rgba(cr, fg_color.red, fg_color.green, fg_color.blue, 0.6);
        cairo_move_to(cr, 0, 0);
        pango_cairo_show_layout(cr, layout);
    }
    pango_font_description_free(font);
    g_object_unref(layout);
    return FALSE;
}

/* Höhe der Leiste folgt ihrer Breite (Bilder 16:9) */
static gboolean on_idle_preview_resize(gpointer p)
{
    GtkWidget *area = p;
    int h = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(area), "want-h"));
    gtk_widget_set_size_request(area, THUMB_N * (THUMB_MIN_W + THUMB_GAP) - THUMB_GAP, h);
    g_object_unref(area);
    return G_SOURCE_REMOVE;
}

/* neue Breite der Leiste: Höhe für 16:9-Bilder anpassen */
static void on_preview_strip_allocate(GtkWidget *area, GdkRectangle *a, gpointer d)
{
    (void)d;
    int want = preview_image_width(a->width) * 9 / 16 + THUMB_LABEL_H;
    if (want != GPOINTER_TO_INT(g_object_get_data(G_OBJECT(area), "want-h"))) {
        g_object_set_data(G_OBJECT(area), "want-h", GINT_TO_POINTER(want));
        g_idle_add(on_idle_preview_resize, g_object_ref(area));
    }
}

/* seitlicher Seitenrand bei Fensterbreite window_w: 4 %, mindestens 16 und höchstens 120 px */
static int page_margin(int window_w)
{
    return CLAMP(window_w * 4 / 100, 16, 120);
}

/* Vorschauleiste für ein nicht erkanntes Video anlegen */
static GtkWidget *new_preview_strip(const Video *v)
{
    PreviewStrip *strip = g_new0(PreviewStrip, 1);
    strip->path = g_strdup(v->path);
    strip->n = -1;
    strip->area = gtk_drawing_area_new();
    /* Startgröße aus der Fensterbreite schätzen (ohne Seitenränder und etwa 80 px für Häkchen
     * und Zeilenränder), danach folgt sie der tatsächlichen Zuteilung */
    int window_w = ui.layout_width > 0 ? ui.layout_width : 780;
    int strip_w = window_w - 2 * page_margin(window_w) - 80;
    int want = preview_image_width(strip_w) * 9 / 16 + THUMB_LABEL_H;
    g_object_set_data(G_OBJECT(strip->area), "want-h", GINT_TO_POINTER(want));
    gtk_widget_set_size_request(strip->area, THUMB_N * (THUMB_MIN_W + THUMB_GAP) - THUMB_GAP, want);
    gtk_widget_set_hexpand(strip->area, TRUE);
    gtk_widget_set_tooltip_text(strip->area, ui.run_outro
                                              ? "Bilder 1 s, 2 s und 5 s sowie 1/30 und 1/10 der "
                                                "Laufzeit vor dem Ende"
                                              : "Bilder bei 1 s, 2 s und 5 s sowie bei 1/30 und "
                                                "1/10 der Laufzeit");
    g_signal_connect(strip->area, "draw", G_CALLBACK(on_preview_strip_draw),
                     GINT_TO_POINTER((int)ui.preview_strips->len));
    g_signal_connect(strip->area, "size-allocate", G_CALLBACK(on_preview_strip_allocate), NULL);
    g_ptr_array_add(ui.preview_strips, strip);
    return strip->area;
}

/* alte Vorschau verwerfen (laufende Aufträge brechen ab, Ergebnisse verfallen) */
static void clear_preview_strips(void)
{
    g_atomic_int_inc(&ui.preview_generation);
    g_ptr_array_set_size(ui.preview_strips, 0);
}

/* Zeile für Ergebnis index an Position position der Liste:
 * [Häkchen] Name / Ordner [Distanz, Abspielen]; darunter ggf. die Vorschau */
static GtkWidget *add_result_row(GtkWidget *list, int position, int index, bool match)
{
    const Result *result = &ui.detection.results[index];
    const Video *v = result->v;
    GtkWidget *check = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), ui.result_selected[index]);
    g_object_set_data(G_OBJECT(check), "index", GINT_TO_POINTER(index + 1));
    g_signal_connect(check, "toggled", G_CALLBACK(on_check_toggled), NULL);
    g_ptr_array_add(ui.result_checks, check);

    GtkWidget *suffix = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    char *badge_text = g_strdup_printf("Distanz %d", result->dist);
    GtkWidget *badge = new_label(badge_text, "ic-badge");
    add_css_class(badge, match ? "ok" : "near");
    gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
    if (result->nd > 1) {                       /* mehrere Vergleichsbilder: Einzelwerte */
        GString *tooltip =
            g_string_new("Abweichung vom Referenz-Video (0 = identisch) – Median der "
                         "Vergleichsbilder:");
        for (int j = 0; j < result->nd; j++) {
            if (result->d[j] == 255)
                g_string_append(tooltip, j ? ", –" : " –");
            else
                g_string_append_printf(tooltip, "%s%d", j ? ", " : " ", result->d[j]);
        }
        gtk_widget_set_tooltip_text(badge, tooltip->str);
        g_string_free(tooltip, TRUE);
    } else {
        gtk_widget_set_tooltip_text(badge, "Abweichung vom Referenz-Frame (0 = identisch)");
    }
    GtkWidget *play = gtk_button_new_from_icon_name("media-playback-start-symbolic",
                                                    GTK_ICON_SIZE_BUTTON);
    gtk_button_set_relief(GTK_BUTTON(play), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(play, "Im Videoplayer öffnen");
    g_signal_connect(play, "clicked", G_CALLBACK(on_play_clicked), (gpointer)v);
    gtk_box_pack_start(GTK_BOX(suffix), badge, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(suffix), play, FALSE, FALSE, 0);

    char *base = g_path_get_basename(v->rel), *dir = g_path_get_dirname(v->rel);
    char *name = path_for_display(base);
    char *sub = strcmp(dir, ".") ? path_for_display(dir) : g_strdup("Quellordner");
    GtkWidget *row_widget = gtk_list_box_row_new();
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_top(grid, 10);
    gtk_widget_set_margin_bottom(grid, 10);
    gtk_widget_set_margin_start(grid, 14);
    gtk_widget_set_margin_end(grid, 12);
    GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    GtkWidget *t = new_label(name, "ic-title");
    gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(t), 50);
    GtkWidget *sub_label = new_label(sub, "ic-sub");
    add_css_class(sub_label, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(sub_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(sub_label), 50);
    gtk_box_pack_start(GTK_BOX(text_box), t, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(text_box), sub_label, FALSE, FALSE, 0);
    gtk_widget_set_hexpand(text_box, TRUE);
    gtk_widget_set_valign(text_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(check, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(suffix, GTK_ALIGN_CENTER);
    gtk_grid_attach(GTK_GRID(grid), check, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), text_box, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), suffix, 2, 0, 1, 1);
    if (!match) gtk_grid_attach(GTK_GRID(grid), new_preview_strip(v), 1, 1, 2, 1);
    gtk_container_add(GTK_CONTAINER(row_widget), grid);
    gtk_list_box_insert(GTK_LIST_BOX(list), row_widget, position);
    g_object_set_data(G_OBJECT(row_widget), "check", check);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row_widget), TRUE);
    g_free(badge_text);
    g_free(base);
    g_free(dir);
    g_free(name);
    g_free(sub);
    return row_widget;
}

static void on_more_results_clicked(GtkButton *b, gpointer data);

/* nächste Portion Zeilen einer Ergebnisliste anlegen; solange noch Zeilen fehlen,
 * steht am Ende der Liste ein Knopf "Weitere … anzeigen" */
static void show_more_results(ResultList *rl)
{
    int end = MIN(rl->total, rl->shown + RESULT_ROWS_BATCH);
    for (; rl->shown < end; rl->shown++) {      /* jeweils vor dem Knopf einfügen */
        int index = rl->first + rl->step * rl->shown;
        gtk_widget_show_all(add_result_row(rl->list, rl->shown, index, rl->matches));
    }
    int rest = rl->total - rl->shown;
    if (rest > 0) {
        if (!rl->more_row) {
            rl->more_button = gtk_button_new();
            gtk_button_set_relief(GTK_BUTTON(rl->more_button), GTK_RELIEF_NONE);
            g_signal_connect(rl->more_button, "clicked", G_CALLBACK(on_more_results_clicked), rl);
            rl->more_row = gtk_list_box_row_new();
            gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(rl->more_row), FALSE);
            gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(rl->more_row), FALSE);
            gtk_container_add(GTK_CONTAINER(rl->more_row), rl->more_button);
            gtk_container_add(GTK_CONTAINER(rl->list), rl->more_row);
        }
        char *text = g_strdup_printf("Weitere %d anzeigen (noch %d ausgeblendet)",
                                     MIN(rest, RESULT_ROWS_BATCH), rest);
        gtk_button_set_label(GTK_BUTTON(rl->more_button), text);
        g_free(text);
        gtk_widget_show_all(rl->more_row);
    } else if (rl->more_row) {
        gtk_widget_hide(rl->more_row);          /* alle Zeilen angezeigt */
    }
}

/* Knopf "Weitere … anzeigen" einer Ergebnisliste */
static void on_more_results_clicked(GtkButton *b, gpointer data)
{
    (void)b;
    show_more_results(data);
}

/* Auswahl und Listen der Ergebnisseite zurücksetzen (die Zeilen selbst entfernt der Aufrufer) */
static void reset_result_lists(void)
{
    g_free(ui.result_selected);
    ui.result_selected = NULL;
    ui.candidate_count = 0;
    memset(ui.result_lists, 0, sizeof ui.result_lists);
    g_ptr_array_set_size(ui.result_checks, 0);
}

/* Seite 3 mit dem Suchergebnis füllen: Treffer und nicht erkannte Videos */
static void fill_results_page(void)
{
    const IcDetect *det = &ui.detection;
    clear_preview_strips();
    GList *old_children = gtk_container_get_children(GTK_CONTAINER(ui.results_box));
    for (GList *item = old_children; item; item = item->next) gtk_widget_destroy(item->data);
    g_list_free(old_children);
    reset_result_lists();
    /* nicht erkannt, aber bis zum 10-fachen Schwellwert ähnlich (Liste nach Distanz sortiert) */
    int limit = ui.run_threshold * NEAR_FACTOR, near = 0;
    while (det->nmatch + near < det->nres && det->results[det->nmatch + near].dist <= limit) near++;
    ui.candidate_count = det->nmatch + near;
    ui.result_selected = g_new0(bool, ui.candidate_count + 1);
    for (int i = 0; i < det->nmatch; i++) ui.result_selected[i] = true;    /* Treffer: ja */

    const char *kind = ui.run_outro ? "Outro" : "Intro";
    char *t = det->nmatch ? g_strdup_printf("%d von %d Videos enthalten das %s", det->nmatch,
                                            det->videos.n, kind)
                        : g_strdup_printf("Keines der %d Videos enthält das %s", det->videos.n,
                                          kind);
    set_label_text(ui.results_title, t);
    g_free(t);
    char secs[64];
    GString *summary = g_string_new(NULL);
    g_string_append_printf(summary, "Suche dauerte %s.",
                           format_duration(det->secs, secs, sizeof secs));
    if (det->frames > 1)
        g_string_append_printf(summary, " %d Vergleichsbilder je Video (Distanz = Median).",
                               det->frames);
    if (det->skipped)
        g_string_append_printf(summary, " %d Datei(en) ohne lesbaren Frame (zu kurz oder defekt) "
                               "übersprungen.", det->skipped);
    if (!det->nmatch && det->nres)
        g_string_append(summary,
                        " Schwellwert erhöhen oder nicht erkannte Videos unten auswählen.");
    set_label_text(ui.results_subtitle, summary->str);
    g_string_free(summary, TRUE);

    if (det->nmatch) {
        GtkWidget *list = add_group(ui.results_box, "Treffer – unsicherste zuerst");
        g_signal_connect(list, "row-activated", G_CALLBACK(on_result_row_activated), NULL);
        ui.result_lists[0] = (ResultList){.list = list, .first = det->nmatch - 1, .step = -1,
                                          .total = det->nmatch, .matches = true};
        show_more_results(&ui.result_lists[0]);
        if (det->nmatch > RESULT_ROWS_BATCH) {
            GtkWidget *hint_label =
                new_label("Angezeigt werden zunächst die unsichersten Treffer. Ausgeblendete "
                          "Treffer sind mit ausgewählt und werden mitgeschnitten – „Keine "
                          "auswählen“ wählt auch sie ab.", "dim-label");
            add_css_class(hint_label, "ic-sub");
            set_wrapping(hint_label, 80);
            gtk_box_pack_start(GTK_BOX(ui.results_box), hint_label, FALSE, FALSE, 0);
        }
    }
    if (near > 0) {
        gtk_box_pack_start(GTK_BOX(ui.results_box),
                           new_label(ui.run_outro ? "Nicht erkannt – prüfen, ob echte Outros fehlen"
                                             : "Nicht erkannt – prüfen, ob echte Intros fehlen",
                                     "ic-heading"),
                           FALSE, FALSE, 0);
        char *h = g_strdup_printf("%d Video%s bis Distanz %d (%d-facher Schwellwert), ähnlichste "
                                  "zuerst. %s", near, near == 1 ? "" : "s", limit, NEAR_FACTOR,
                                  ui.run_outro ? "Vorschau 1 s, 2 s und 5 s sowie 1/30 und 1/10 "
                                                "der Laufzeit vor dem Ende."
                                              : "Vorschau bei 1 s, 2 s und 5 s sowie bei 1/30 und "
                                                "1/10 der Laufzeit.");
        GtkWidget *hint_label = new_label(h, "dim-label");
        add_css_class(hint_label, "ic-sub");
        set_wrapping(hint_label, 80);
        gtk_widget_set_margin_bottom(hint_label, 4);
        gtk_box_pack_start(GTK_BOX(ui.results_box), hint_label, FALSE, FALSE, 0);
        g_free(h);
        GtkWidget *list = add_group(ui.results_box, NULL);
        g_signal_connect(list, "row-activated", G_CALLBACK(on_result_row_activated), NULL);
        ui.result_lists[1] = (ResultList){.list = list, .first = det->nmatch, .step = 1,
                                          .total = near, .matches = false};
        show_more_results(&ui.result_lists[1]);
    }
    gtk_widget_show_all(ui.results_box);
    GtkStyleContext *ctx = gtk_widget_get_style_context(ui.cut_button);
    gtk_style_context_remove_class(ctx, ui.run_replace ? "suggested-action" : "destructive-action");
    gtk_style_context_add_class(ctx, ui.run_replace ? "destructive-action" : "suggested-action");
    update_selection_count();
}

static void on_cut_clicked(GtkButton *b, gpointer d);

/* zurück zu den Einstellungen (offene Vorschau-Aufträge verwerfen) */
static void on_back_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    g_atomic_int_inc(&ui.preview_generation);  /* offene Vorschau-Aufträge verwerfen */
    g_atomic_int_inc(&ui.auto_generation);
    show_page("setup");
}

/* Seite 3: Treffer */
static GtkWidget *build_results_page(void)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *scroller, *box = new_page_column(&scroller);
    ui.results_title = new_label("", "ic-hero");
    set_wrapping(ui.results_title, 40);
    ui.results_subtitle = new_label("", "dim-label");
    set_wrapping(ui.results_subtitle, 80);
    gtk_widget_set_margin_top(ui.results_title, 8);
    gtk_box_pack_start(GTK_BOX(box), ui.results_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.results_subtitle, FALSE, FALSE, 0);
    GtkWidget *tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *select_matches_button = gtk_button_new_with_label("Alle Treffer auswählen");
    GtkWidget *none = gtk_button_new_with_label("Keine auswählen");
    gtk_button_set_relief(GTK_BUTTON(select_matches_button), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(none), GTK_RELIEF_NONE);
    g_signal_connect(select_matches_button, "clicked", G_CALLBACK(on_select_matches_clicked), NULL);
    g_signal_connect(none, "clicked", G_CALLBACK(on_select_none_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(tools), select_matches_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tools), none, FALSE, FALSE, 0);
    gtk_widget_set_margin_top(tools, 6);
    gtk_box_pack_start(GTK_BOX(box), tools, FALSE, FALSE, 0);
    ui.results_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(box), ui.results_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), scroller, TRUE, TRUE, 0);

    GtkWidget *action_bar = gtk_action_bar_new();
    ui.selection_label = new_label("", "dim-label");
    gtk_widget_set_margin_start(ui.selection_label, 12);
    gtk_action_bar_pack_start(GTK_ACTION_BAR(action_bar), ui.selection_label);
    ui.cut_button = new_pill_button("Schneiden", "suggested-action");
    gtk_widget_set_margin_top(ui.cut_button, 6);
    gtk_widget_set_margin_bottom(ui.cut_button, 6);
    gtk_widget_set_margin_end(ui.cut_button, 12);
    g_signal_connect(ui.cut_button, "clicked", G_CALLBACK(on_cut_clicked), NULL);
    gtk_action_bar_pack_end(GTK_ACTION_BAR(action_bar), ui.cut_button);
    gtk_box_pack_end(GTK_BOX(outer), action_bar, FALSE, FALSE, 0);
    return outer;
}

/* ---------------------------------------------------------------------------
 * Intro-Ende bzw. Outro-Beginn am Video festlegen – bildgenau. Oben das Videobild
 * (mit Wiedergabe samt Ton), darunter ein Bildstreifen mit den Nachbarbildern und
 * die Zeitleiste der ersten (bzw. letzten) Minuten.
 *
 * Der Schnitt liegt immer zwischen zwei Bildern (ihre Zeitstempel liest ffprobe aus dem
 * Video). ffmpeg behält beim Schneiden die Bilder ab dieser Zeit – so trifft der Schnitt
 * per CPU/GPU genau das gewählte Bild, auch wenn die Länge auf Millisekunden gerundet wird
 * (siehe mark_cut_time_of).
 *
 * Bilder liefert ffmpeg im Hintergrund, mehrere aufeinanderfolgende in einem Lauf;
 * die Bilder um die Abspielposition bleiben im Speicher, damit Schritte sofort gehen.
 * ------------------------------------------------------------------------- */
#define MARK_FRAME_W 640            /* Bildbreite für Wiedergabe und Einzelbilder */
#define MARK_THUMB_H 72             /* Höhe der Vorschaubilder der Zeitleiste */
#define MARK_THUMBS_MAX 150         /* höchstens so viele Vorschaubilder (sonst 1 je s) */
#define MARK_TIMELINE_H 104         /* Höhe der Zeitleiste in Pixel */
#define MARK_SCALE_H 20             /* davon das Band mit der Zeitskala oben */
#define MARK_STRIP_H 112            /* Höhe des Bildstreifens */
#define MARK_STRIP_TEXT_H 20        /* davon die Zeile mit den Zeiten unter den Bildern */
#define MARK_STRIP_GAP 8            /* Lücke zwischen den Bildern (dort liegt der Schnitt) */
#define MARK_STRIP_MAX 15           /* höchstens so viele Bilder im Streifen */
#define MARK_PAD 10                 /* Rand links/rechts, damit der Marker greifbar bleibt */
#define MARK_GRAB 10                /* so nah (Pixel) muss ein Klick am Marker liegen */
#define MARK_PLAY_FPS_MAX 30        /* Wiedergabe mit höchstens so vielen Bildern je s */
#define MARK_BATCH 30               /* Bilder je ffmpeg-Lauf beim schrittweisen Ansehen */
#define MARK_CACHE_MAX 120          /* so viele Bilder bleiben im Speicher (je etwa 0,7 MB) */
#define MARK_AHEAD 15               /* so viele Bilder nach der Abspielposition vorladen */
#define MARK_BEHIND 10              /* und so viele davor */

/* wählbare Ausschnitte der Zeitleiste (s) */
static const int MARK_RANGES[3] = {60, 120, 300};
static const char *const MARK_RANGE_TEXT[2][3] = {
    {"Erste Minute", "Erste 2 Minuten", "Erste 5 Minuten"},
    {"Letzte Minute", "Letzte 2 Minuten", "Letzte 5 Minuten"}};

enum { DRAG_NONE, DRAG_MARKER, DRAG_PLAYHEAD };
enum { STEP_START, STEP_SECOND_BACK, STEP_FRAME_BACK, STEP_FRAME_FORWARD, STEP_SECOND_FORWARD,
       STEP_END };

typedef struct MarkLoadJob MarkLoadJob;

/* "Übernehmen": neue Länge des Intros bzw. Outros (s, auf ms gerundet) und Zeitpunkt des
 * ersten Bilds nach dem Schnitt (Intro) bzw. des ersten abgeschnittenen (Outro) */
typedef void (*MarkApply)(double length, double first_after_cut, gpointer data);

typedef struct {
    GtkWidget *dialog, *video_area, *strip, *timeline, *play_button, *time_label, *cut_label;
    GtkWidget *sound_button, *range_combo, *hint_label, *controls, *cut_row, *apply_button;
    int serial;                     /* zählt geöffnete Fenster; verwirft verspätete Antworten */
    char *video;                    /* angezeigtes Video */
    double initial_length;          /* bisherige Länge des Intros bzw. Outros (s) */
    MarkApply apply;                /* "Übernehmen" meldet die neue Länge hierhin */
    gpointer apply_data;
    bool outro;                     /* Outro-Beginn statt Intro-Ende festlegen */
    IcVideoProps props;
    bool opened;                    /* Eigenschaften gelesen, die Zeitleiste kann zeichnen */
    bool ready;                     /* Bildzeiten des Ausschnitts da, Bedienung frei */
    bool sound_possible;            /* Video hat Ton und ffmpeg kann ihn ausgeben */
    bool sound_failed;              /* Ausgabe des Tons scheiterte (kein Tonserver) */
    double fps;                     /* Bildrate (25, wenn unbekannt) */
    int frame_w, frame_h;           /* Bildgröße für Wiedergabe und Einzelbilder */
    int thumb_w;                    /* Breite der Vorschaubilder */
    double range_start, range_end;  /* Ausschnitt der Zeitleiste (s ab Dateistart) */
    /* die Bilder des Ausschnitts, Nummer 0 … frame_count - 1 */
    double *times;                  /* ihre Zeitpunkte (s ab Dateistart), aufsteigend */
    int frame_count;
    bool estimated_times;           /* Zeitpunkte nur aus der Bildrate geschätzt */
    int times_generation;           /* neuer Ausschnitt: verspätete Bildzeiten verwerfen */
    GdkPixbuf **frames;             /* geladene Bilder (NULL = nicht im Speicher) */
    unsigned char *unreadable;      /* ffmpeg hat dieses Bild nicht geliefert */
    int cached;                     /* Anzahl geladener Bilder */
    int cache_epoch;                /* Bilder verworfen: verspätete nicht mehr übernehmen */
    bool single_frames;             /* Bildliste und Video passen nicht 1:1 – einzeln lesen */
    MarkLoadJob *load;              /* laufendes Lesen, NULL = keins */
    int load_ids;                   /* zählt die Lese-Aufträge */
    int load_next;                  /* nächstes Bild, das das laufende Lesen liefert */
    /* Abspielposition = gezeigtes Bild; Schnitt = erstes Bild danach (0 … frame_count) */
    int play, cut;
    double play_time, cut_time;     /* beide als Zeit – gelten auch, bevor die Bilder da sind */
    bool cut_moved;                 /* Schnitt verschoben: "Übernehmen" setzt die neue Länge */
    GdkPixbuf *frame;               /* angezeigtes Bild */
    double frame_time;              /* sein Zeitpunkt */
    int frame_index;                /* seine Nummer, -1 = aus der Wiedergabe bzw. veraltet */
    GdkPixbuf *thumbs[MARK_THUMBS_MAX];
    int thumb_count;
    double thumb_step;              /* Abstand der Vorschaubilder (s) */
    int thumb_generation;           /* neuer Ausschnitt: alte Vorschaubilder verwerfen */
    GThread *thumb_thread;
    volatile int thumb_stop;
    bool playing;
    int player_generation;          /* neue Wiedergabe bzw. Stopp: alte Bilder verwerfen */
    GThread *player_thread;
    volatile int player_stop;
    int drag;                       /* DRAG_… während eine Maustaste gedrückt ist */
    GdkCursor *grab_cursor;
} MarkWindow;

static MarkWindow mark;             /* höchstens ein Markierfenster */

/* neuestes Bild der Wiedergabe, vom Leser-Thread an den Hauptthread übergeben */
static GMutex mark_player_lock;
static GdkPixbuf *mark_player_frame;
static double mark_player_t;
static bool mark_player_idle;       /* Anzeige im Hauptthread ist schon angestoßen */

/* Aufträge der Hintergrund-Threads und ihre Ergebnisse */
typedef struct {
    int serial;
    char *video;
    IcVideoProps props;
    bool ok, sound;
} MarkPropsJob;

typedef struct {
    int serial, generation;
    char *video;
    double start, length;
    double *times;                  /* Ergebnis (mit free freigeben) */
    int count;
} MarkTimesJob;

typedef struct {
    int generation;
    char *video;
    double start, length;
    int count, w, h;
} MarkThumbsJob;

typedef struct {
    int generation, index;
    GdkPixbuf *pixbuf;
} MarkThumbMsg;

/* die Bilder first … first + count - 1 in einem ffmpeg-Lauf lesen */
struct MarkLoadJob {
    int serial, epoch, id;
    char *video;
    int first, count;
    double from, until;             /* ihre Zeitspanne; until 0 = nur das erste Bild ab from */
    int w, h;
    volatile int stop;              /* vom Hauptthread gesetzt: abbrechen */
    int got;                        /* gelieferte Bilder */
};

typedef struct {
    int serial, epoch, load_id, index;
    GdkPixbuf *pixbuf;
} MarkFrameMsg;

typedef struct {
    int generation;
    char *video;
    double start, end, fps;
    int w, h;
    bool sound;
} MarkPlayJob;

/* RGB24-Bild (w x h) als GdkPixbuf übernehmen; rgb wird später mit g_free freigegeben */
static GdkPixbuf *pixbuf_from_rgb(unsigned char *rgb, int w, int h)
{
    return gdk_pixbuf_new_from_data(rgb, GDK_COLORSPACE_RGB, FALSE, 8, w, h, w * 3, free_pixels,
                                    NULL);
}

/* Position für die Anzeige: "0:12,400" bzw. "1:02:03,040" (Millisekunden) */
static void format_position(double t, char *out, size_t size)
{
    long ms = lround(fmax(0, t) * 1000);
    long hours = ms / 3600000, minutes = ms / 60000 % 60, seconds = ms / 1000 % 60;
    if (hours)
        snprintf(out, size, "%ld:%02ld:%02ld,%03ld", hours, minutes, seconds, ms % 1000);
    else
        snprintf(out, size, "%ld:%02ld,%03ld", minutes, seconds, ms % 1000);
}

/* ---- Bilder, Abspielposition und Schnitt ---- */

/* Grenze vor Bild i: die Mitte zwischen Bild i - 1 und i (vor dem ersten bzw. nach dem
 * letzten Bild eine halbe Bilddauer daneben) – trennt beim Lesen die Bilder sicher */
static double mark_boundary(int i)
{
    double half = 0.5 / mark.fps;
    if (i <= 0) return mark.times[0] - half;
    if (i >= mark.frame_count) return mark.times[mark.frame_count - 1] + half;
    return (mark.times[i - 1] + mark.times[i]) / 2;
}

/* Zeitpunkt eines Schnitts vor Bild cut. Outro: die Mitte zwischen den beiden Bildern, dort
 * stört das Runden der Länge am wenigsten. Intro: ein Viertel Bilddauer vor dem ersten Bild,
 * das bleibt – läge es eine halbe Bilddauer nach dem Schnitt, zeigte ffmpeg es bei Ausgaben
 * mit fester Bildrate (z. B. MP4) zu Beginn doppelt. */
static double mark_cut_time_of(int cut)
{
    if (mark.outro || cut <= 0) return mark_boundary(cut);
    double previous = mark.times[cut - 1];
    double next = cut < mark.frame_count ? mark.times[cut] : previous + 1 / mark.fps;
    return next - (next - previous) / 4;
}

/* erstes Bild bei oder nach t (frame_count, wenn keins mehr kommt) */
static int mark_first_from(double t)
{
    int low = 0, high = mark.frame_count;
    while (low < high) {
        int middle = low + (high - low) / 2;
        if (mark.times[middle] < t)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

/* Bild, das t am nächsten liegt */
static int mark_nearest(double t)
{
    int i = mark_first_from(t);
    if (i >= mark.frame_count) return mark.frame_count - 1;
    if (i > 0 && t - mark.times[i - 1] < mark.times[i] - t) return i - 1;
    return i;
}

/* Schnitt im Ausschnitt halten – mindestens ein Bild fällt weg */
static int mark_clamp_cut(int cut)
{
    int lowest = mark.outro ? 0 : 1;
    int highest = mark.outro ? mark.frame_count - 1 : mark.frame_count;
    return cut < lowest ? lowest : cut > highest ? highest : cut;
}

/* fällt das Bild bei t weg? */
static bool mark_removed(double t)
{
    return mark.outro ? t >= mark.cut_time : t < mark.cut_time;
}

/* Länge des Schnitts: Intro = Schnittzeit, Outro = Dauer - Schnittzeit */
static double mark_cut_length(void)
{
    return mark.outro ? mark.props.duration - mark.cut_time : mark.cut_time;
}

/* Zeitleiste: Zeit -> x-Position und umgekehrt (ohne Runden) */
static double mark_time_to_x(double t, int width)
{
    double length = mark.range_end - mark.range_start;
    if (length <= 0) return MARK_PAD;
    return MARK_PAD + (t - mark.range_start) / length * (width - 2 * MARK_PAD);
}

static double mark_x_to_time(double x, int width)
{
    double length = mark.range_end - mark.range_start;
    return mark.range_start + (x - MARK_PAD) / fmax(1, width - 2 * MARK_PAD) * length;
}

/* Texte unter dem Bild: Abspielposition und Schnitt */
static void mark_update_labels(void)
{
    char position[64], at[32], seconds[32];
    format_position(mark.play_time, position, sizeof position);
    if (mark.ready && !mark.outro && !mark.estimated_times) {   /* Bildnummer ab Videoanfang */
        size_t used = strlen(position);
        snprintf(position + used, sizeof position - used, " · Bild %d", mark.play + 1);
    }
    set_label_text(mark.time_label, position);
    if (!mark.ready) return;
    format_position(mark.cut_time, at, sizeof at);
    snprintf(seconds, sizeof seconds, "%.3f", mark_cut_length());
    for (char *c = seconds; *c; c++)
        if (*c == '.') *c = ',';
    int removed = mark.outro ? mark.frame_count - mark.cut : mark.cut;
    char frames[48] = "";
    if (!mark.estimated_times)
        snprintf(frames, sizeof frames, " (%d %s)", removed, removed == 1 ? "Bild" : "Bilder");
    char *text = g_strdup_printf("Schnitt bei %s – %s %s s%s fallen weg", at,
                                 mark.outro ? "die letzten" : "die ersten", seconds, frames);
    set_label_text(mark.cut_label, text);
    g_free(text);
}

/* Bild k im Videobild zeigen (wenn es geladen ist) */
static void mark_show_frame(int k)
{
    if (k < 0 || k >= mark.frame_count || !mark.frames[k]) return;
    g_clear_object(&mark.frame);
    mark.frame = g_object_ref(mark.frames[k]);
    mark.frame_time = mark.times[k];
    mark.frame_index = k;
    gtk_widget_queue_draw(mark.video_area);
}

/* ---- Bilder laden und im Speicher halten ---- */

/* fehlt Bild k noch (und lässt es sich lesen)? */
static bool mark_missing(int k)
{
    return k >= 0 && k < mark.frame_count && !mark.frames[k] && !mark.unreadable[k];
}

/* geladenes Bild k übernehmen; sind es zu viele, fallen die am weitesten entfernten heraus */
static void mark_cache_put(int k, GdkPixbuf *pixbuf)
{
    if (mark.frames[k])
        g_object_unref(mark.frames[k]);
    else
        mark.cached++;
    mark.frames[k] = pixbuf;
    while (mark.cached > MARK_CACHE_MAX) {
        int left = 0, right = mark.frame_count - 1;
        while (!mark.frames[left]) left++;
        while (!mark.frames[right]) right--;
        g_clear_object(&mark.frames[mark.play - left > right - mark.play ? left : right]);
        mark.cached--;
    }
}

/* alle geladenen Bilder verwerfen (noch unterwegs befindliche werden nicht mehr übernommen) */
static void mark_cache_clear(void)
{
    for (int i = 0; i < mark.frame_count; i++) g_clear_object(&mark.frames[i]);
    if (mark.unreadable) memset(mark.unreadable, 0, (size_t)mark.frame_count);
    mark.cached = 0;
    mark.cache_epoch++;
    mark.frame_index = -1;
}

static gboolean on_mark_frame_loaded(gpointer p);
static gboolean on_mark_load_done(gpointer p);

/* im Hintergrund: die Bilder eines Auftrags lesen und einzeln an den Hauptthread geben */
static gpointer mark_load_thread(gpointer p)
{
    MarkLoadJob *job = p;
    size_t bytes = (size_t)job->w * (size_t)job->h * 3;
    /* ein Bild mehr zulassen: kommt es, passen Bildliste und Video nicht zusammen */
    int limit = job->until > 0 ? job->count + 1 : job->count;
    IcFrameStream *stream = ic_frame_stream_open_frames(job->video, job->from, job->until, limit,
                                                        job->w, job->h);
    while (stream && job->got < limit && !job->stop) {
        unsigned char *rgb = g_malloc(bytes);
        if (!ic_frame_stream_read(stream, rgb, NULL, &job->stop)) {
            g_free(rgb);
            break;
        }
        if (job->got < job->count) {
            MarkFrameMsg *m = g_new(MarkFrameMsg, 1);
            *m = (MarkFrameMsg){job->serial, job->epoch, job->id, job->first + job->got,
                                pixbuf_from_rgb(rgb, job->w, job->h)};
            g_idle_add(on_mark_frame_loaded, m);
        } else {
            g_free(rgb);
        }
        job->got++;
    }
    ic_frame_stream_close(stream);
    g_idle_add(on_mark_load_done, job);
    return NULL;
}

/* Lesen ab Bild k starten: mehrere Bilder ab k (beim Zurückgehen: bis k), bis zu schon
 * geladenen; beim Ziehen und bei unpassender Bildliste nur Bild k */
static void mark_start_load(int k)
{
    int first = k, count = 1;
    if (!mark.single_frames && mark.drag == DRAG_NONE) {
        if (k < mark.play) {
            while (count < MARK_BATCH && mark_missing(first - 1)) {
                first--;
                count++;
            }
        } else {
            while (count < MARK_BATCH && mark_missing(first + count)) count++;
        }
    }
    MarkLoadJob *job = g_new0(MarkLoadJob, 1);
    job->serial = mark.serial;
    job->epoch = mark.cache_epoch;
    job->id = ++mark.load_ids;
    job->video = g_strdup(mark.video);
    job->first = first;
    job->count = count;
    /* von der Grenze vor dem ersten bis zur Grenze nach dem letzten Bild – einzeln: das erste
     * Bild ab der Grenze, also genau das, mit dem ein Schnitt hier beginnen würde */
    job->from = mark_boundary(first);
    job->until = mark.single_frames ? 0 : mark_boundary(first + count);
    job->w = mark.frame_w;
    job->h = mark.frame_h;
    mark.load = job;
    mark.load_next = first;
    g_thread_unref(g_thread_new("vorschau-bilder", mark_load_thread, job));
}

/* laufendes Lesen abbrechen (schon gelesene Bilder werden noch übernommen) */
static void mark_abort_load(void)
{
    if (!mark.load) return;
    mark.load->stop = 1;
    mark.load = NULL;
}

/* nächstes fehlende Bild: erst das gezeigte, dann die Nachbarn (voraus weiter als zurück)
 * -> -1, wenn alle da sind */
static int mark_next_missing(void)
{
    if (mark_missing(mark.play)) return mark.play;
    for (int d = 1; d <= MARK_AHEAD; d++) {
        if (mark_missing(mark.play + d)) return mark.play + d;
        if (d <= MARK_BEHIND && mark_missing(mark.play - d)) return mark.play - d;
    }
    return -1;
}

/* weiterlesen, solange um die Abspielposition Bilder fehlen (nicht während der Wiedergabe;
 * beim Ziehen nur das gezeigte Bild) */
static void mark_load_more(void)
{
    if (!mark.ready || mark.playing || mark.load) return;
    int k = mark_next_missing();
    if (k < 0 || (mark.drag != DRAG_NONE && k != mark.play)) return;
    mark_start_load(k);
}

/* im Hauptthread: ein Bild ist gelesen */
static gboolean on_mark_frame_loaded(gpointer p)
{
    MarkFrameMsg *m = p;
    if (mark.dialog && m->serial == mark.serial && m->epoch == mark.cache_epoch &&
        m->index < mark.frame_count) {
        if (mark.load && m->load_id == mark.load->id) mark.load_next = m->index + 1;
        mark_cache_put(m->index, m->pixbuf);
        if (m->index == mark.play && !mark.playing) mark_show_frame(m->index);
        gtk_widget_queue_draw(mark.strip);
    } else {
        g_object_unref(m->pixbuf);
    }
    g_free(m);
    return G_SOURCE_REMOVE;
}

/* im Hauptthread: ein Lesen ist fertig – prüfen und weiterlesen */
static gboolean on_mark_load_done(gpointer p)
{
    MarkLoadJob *job = p;
    if (mark.dialog && job == mark.load) {
        mark.load = NULL;
        if (job->until > 0 && job->got != job->count) {
            /* zwischen den Grenzen lagen mehr oder weniger Bilder als in der Liste (etwa
             * Halbbilder als eigene Pakete): die Zuordnung stimmt nicht sicher – ab jetzt
             * jedes Bild einzeln suchen */
            mark.single_frames = true;
            mark_cache_clear();
            gtk_widget_queue_draw(mark.video_area);
        } else {
            for (int i = job->first + job->got; i < job->first + job->count; i++)
                mark.unreadable[i] = 1;
        }
        gtk_widget_queue_draw(mark.strip);
        mark_load_more();
    }
    g_free(job->video);
    g_free(job);
    return G_SOURCE_REMOVE;
}

/* Abspielposition auf Bild k setzen und es zeigen (bzw. laden) */
static void mark_set_play(int k)
{
    if (!mark.ready) return;
    k = k < 0 ? 0 : k >= mark.frame_count ? mark.frame_count - 1 : k;
    mark.play = k;
    mark.play_time = mark.times[k];
    if (mark.frames[k]) {
        mark_show_frame(k);
    } else {
        gtk_widget_queue_draw(mark.video_area);         /* bis es da ist: abgeblendet */
        /* bringt das laufende Lesen Bild k nicht gleich mit, dieses abbrechen */
        if (mark.load && (k < mark.load_next || k >= mark.load->first + mark.load->count))
            mark_abort_load();
    }
    mark_update_labels();
    gtk_widget_queue_draw(mark.strip);
    gtk_widget_queue_draw(mark.timeline);
    mark_load_more();
}

/* Schnitt vor Bild cut legen (0 … frame_count) */
static void mark_set_cut(int cut)
{
    if (!mark.ready) return;
    cut = mark_clamp_cut(cut);
    if (cut != mark.cut) mark.cut_moved = true;
    mark.cut = cut;
    mark.cut_time = mark_cut_time_of(cut);
    mark_update_labels();
    gtk_widget_queue_draw(mark.video_area);             /* "fällt weg"/"bleibt" */
    gtk_widget_queue_draw(mark.strip);
    gtk_widget_queue_draw(mark.timeline);
}

/* ---- Vorschaubilder der Zeitleiste ---- */

/* im Hauptthread: ein Vorschaubild ist da */
static gboolean on_mark_thumb(gpointer p)
{
    MarkThumbMsg *m = p;
    if (mark.dialog && m->generation == mark.thumb_generation && m->index < mark.thumb_count) {
        g_clear_object(&mark.thumbs[m->index]);
        mark.thumbs[m->index] = m->pixbuf;
        gtk_widget_queue_draw(mark.timeline);
    } else {
        g_object_unref(m->pixbuf);
    }
    g_free(m);
    return G_SOURCE_REMOVE;
}

/* im Hintergrund: alle Vorschaubilder des Ausschnitts in einem ffmpeg-Lauf lesen */
static gpointer mark_thumbs_thread(gpointer p)
{
    MarkThumbsJob *job = p;
    IcFrameStream *stream = ic_frame_stream_open(job->video, job->start, job->length, job->w,
                                                 job->h, job->count / job->length, true, false);
    size_t bytes = (size_t)job->w * (size_t)job->h * 3;
    for (int i = 0; stream && i < job->count && !mark.thumb_stop; i++) {
        unsigned char *rgb = g_malloc(bytes);
        if (!ic_frame_stream_read(stream, rgb, NULL, &mark.thumb_stop)) {
            g_free(rgb);
            break;
        }
        MarkThumbMsg *m = g_new(MarkThumbMsg, 1);
        *m = (MarkThumbMsg){job->generation, i, pixbuf_from_rgb(rgb, job->w, job->h)};
        g_idle_add(on_mark_thumb, m);
    }
    ic_frame_stream_close(stream);
    g_free(job->video);
    g_free(job);
    return NULL;
}

/* Lesen der Vorschaubilder beenden und die vorhandenen verwerfen */
static void mark_stop_thumbs(void)
{
    if (mark.thumb_thread) {
        mark.thumb_stop = 1;
        g_thread_join(mark.thumb_thread);
        mark.thumb_thread = NULL;
    }
    mark.thumb_stop = 0;
    mark.thumb_generation++;
    for (int i = 0; i < mark.thumb_count; i++) g_clear_object(&mark.thumbs[i]);
    mark.thumb_count = 0;
}

/* Vorschaubilder für den aktuellen Ausschnitt laden (etwa eines je Sekunde) */
static void mark_start_thumbs(void)
{
    mark_stop_thumbs();
    double length = mark.range_end - mark.range_start;
    if (length <= 0) return;
    int count = (int)ceil(length);
    count = count < 2 ? 2 : count > MARK_THUMBS_MAX ? MARK_THUMBS_MAX : count;
    mark.thumb_count = count;
    mark.thumb_step = length / count;
    MarkThumbsJob *job = g_new0(MarkThumbsJob, 1);
    *job = (MarkThumbsJob){mark.thumb_generation, g_strdup(mark.video), mark.range_start, length,
                           count, mark.thumb_w, MARK_THUMB_H};
    mark.thumb_thread = g_thread_new("vorschau-leiste", mark_thumbs_thread, job);
}

/* ---- Wiedergabe ---- */

/* Knopf "Abspielen" zeigt Wiedergabe bzw. Pause */
static void mark_show_play_state(void)
{
    const char *icon = mark.playing ? "media-playback-pause-symbolic"
                                    : "media-playback-start-symbolic";
    gtk_button_set_image(GTK_BUTTON(mark.play_button),
                         gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON));
    gtk_widget_set_tooltip_text(mark.play_button, mark.playing ? "Pause (Leertaste)"
                                                               : "Abspielen (Leertaste)");
}

/* im Hauptthread: neuestes Bild der Wiedergabe zeigen */
static gboolean on_mark_player_frame(gpointer p)
{
    g_mutex_lock(&mark_player_lock);
    GdkPixbuf *pixbuf = mark_player_frame;
    double t = mark_player_t;
    mark_player_frame = NULL;
    mark_player_idle = false;
    g_mutex_unlock(&mark_player_lock);
    if (pixbuf && mark.dialog && GPOINTER_TO_INT(p) == mark.player_generation) {
        g_clear_object(&mark.frame);
        mark.frame = pixbuf;
        mark.frame_time = t;
        mark.frame_index = -1;
        mark.play_time = t;
        mark.play = mark_nearest(t);
        mark_update_labels();
        gtk_widget_queue_draw(mark.video_area);
        gtk_widget_queue_draw(mark.strip);
        gtk_widget_queue_draw(mark.timeline);
    } else if (pixbuf) {
        g_object_unref(pixbuf);
    }
    return G_SOURCE_REMOVE;
}

/* im Hauptthread: Wiedergabe hat das Ende des Ausschnitts erreicht */
static gboolean on_mark_player_end(gpointer p)
{
    if (mark.dialog && GPOINTER_TO_INT(p) == mark.player_generation && mark.player_thread) {
        g_thread_join(mark.player_thread);
        mark.player_thread = NULL;
        mark.player_generation++;
        mark.playing = false;
        mark_show_play_state();
        mark_set_play(mark.play);                       /* das Bild hier genau zeigen */
    }
    return G_SOURCE_REMOVE;
}

static void mark_update_hint(void);

/* im Hauptthread: Ton ließ sich nicht ausgeben – ab jetzt ohne Ton */
static gboolean on_mark_sound_failed(gpointer p)
{
    if (mark.dialog && GPOINTER_TO_INT(p) == mark.player_generation) {
        mark.sound_possible = false;
        mark.sound_failed = true;
        gtk_widget_hide(mark.sound_button);
        mark_update_hint();
    }
    return G_SOURCE_REMOVE;
}

/* im Hintergrund: Bilder in Echtzeit lesen und weitergeben; der Ton läuft im selben
 * ffmpeg-Prozess direkt zu PulseAudio/PipeWire */
static gpointer mark_player_thread(gpointer p)
{
    MarkPlayJob *job = p;
    size_t bytes = (size_t)job->w * (size_t)job->h * 3;
    unsigned char *rgb = g_malloc(bytes);
    double t = job->start;
    IcFrameStream *stream = ic_frame_stream_open(job->video, job->start, job->end - job->start,
                                                 job->w, job->h, job->fps, false, job->sound);
    bool have = stream && ic_frame_stream_read(stream, rgb, &t, &mark.player_stop);
    if (!have && job->sound && !mark.player_stop) {     /* ohne Tonserver: noch einmal stumm */
        ic_frame_stream_close(stream);
        g_idle_add(on_mark_sound_failed, GINT_TO_POINTER(job->generation));
        stream = ic_frame_stream_open(job->video, job->start, job->end - job->start, job->w,
                                      job->h, job->fps, false, false);
        have = stream && ic_frame_stream_read(stream, rgb, &t, &mark.player_stop);
    }
    double wall_start = g_get_monotonic_time() / 1e6, first_t = t;
    while (have && !mark.player_stop) {
        double due = wall_start + (t - first_t);        /* Bild zu seiner Zeit zeigen */
        while (!mark.player_stop) {
            double wait = due - g_get_monotonic_time() / 1e6;
            if (wait <= 0) break;
            g_usleep((gulong)(fmin(wait, 0.02) * 1e6));
        }
        if (mark.player_stop) break;
        g_mutex_lock(&mark_player_lock);
        if (mark_player_frame) g_object_unref(mark_player_frame);
        mark_player_frame = pixbuf_from_rgb(rgb, job->w, job->h);
        mark_player_t = t;
        bool schedule = !mark_player_idle;
        mark_player_idle = true;
        g_mutex_unlock(&mark_player_lock);
        if (schedule) g_idle_add(on_mark_player_frame, GINT_TO_POINTER(job->generation));
        rgb = g_malloc(bytes);
        have = ic_frame_stream_read(stream, rgb, &t, &mark.player_stop);
    }
    g_free(rgb);
    ic_frame_stream_close(stream);
    if (!mark.player_stop) g_idle_add(on_mark_player_end, GINT_TO_POINTER(job->generation));
    g_free(job->video);
    g_free(job);
    return NULL;
}

/* Wiedergabe beenden (die Abspielposition bleibt, wo sie gerade ist) */
static void mark_stop_player(void)
{
    if (mark.player_thread) {
        mark.player_stop = 1;
        g_thread_join(mark.player_thread);
        mark.player_thread = NULL;
    }
    mark.player_stop = 0;
    mark.player_generation++;
    g_mutex_lock(&mark_player_lock);
    g_clear_object(&mark_player_frame);
    g_mutex_unlock(&mark_player_lock);
    if (mark.playing) {
        mark.playing = false;
        mark_show_play_state();
    }
}

/* Wiedergabe anhalten und das Bild an dieser Stelle genau zeigen */
static void mark_pause(void)
{
    bool was_playing = mark.playing;
    mark_stop_player();
    if (was_playing) mark_set_play(mark.play);
}

/* Wiedergabe ab der Abspielposition bis zum Ende des Ausschnitts (am Ende: von vorn) */
static void mark_play(void)
{
    if (!mark.ready || mark.playing) return;
    mark_abort_load();                                  /* die Wiedergabe hat Vorrang */
    mark.playing = true;                                /* ab hier wird nichts nachgeladen */
    if (mark.play >= mark.frame_count - 1) mark_set_play(0);
    bool sound = mark.sound_possible &&
                 gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(mark.sound_button));
    MarkPlayJob *job = g_new0(MarkPlayJob, 1);
    *job = (MarkPlayJob){mark.player_generation, g_strdup(mark.video), mark.times[mark.play],
                         mark.range_end, fmin(mark.fps, MARK_PLAY_FPS_MAX), mark.frame_w,
                         mark.frame_h, sound};
    mark.player_stop = 0;
    mark.player_thread = g_thread_new("vorschau-wiedergabe", mark_player_thread, job);
    mark_show_play_state();
}

/* ---- Bedienung: Schritte und Schnitt ---- */

/* Sprung um ein Bild bzw. eine Sekunde oder an den Anfang/das Ende des Ausschnitts */
static void mark_step(int step)
{
    if (!mark.ready) return;
    mark_stop_player();
    int k = mark.play;
    switch (step) {
    case STEP_START: k = 0; break;
    case STEP_SECOND_BACK: k = mark_nearest(mark.times[k] - 1); break;
    case STEP_FRAME_BACK: k--; break;
    case STEP_FRAME_FORWARD: k++; break;
    case STEP_SECOND_FORWARD: k = mark_nearest(mark.times[k] + 1); break;
    case STEP_END: k = mark.frame_count - 1; break;
    }
    mark_set_play(k);
}

/* Schnitt um delta Bilder verschieben und das erste Bild danach zeigen */
static void mark_shift_cut(int delta)
{
    if (!mark.ready) return;
    mark_stop_player();
    mark_set_cut(mark.cut + delta);
    mark_set_play(mark.cut);
}

/* Schnitt direkt vor das gezeigte Bild legen */
static void mark_cut_here(void)
{
    if (!mark.ready) return;
    mark_pause();
    mark_set_cut(mark.play);
}

/* Hinweise unter den Knöpfen: Tasten, ggf. geschätzte Zeiten und Copy-Modus */
static void mark_update_hint(void)
{
    GString *text = g_string_new(
        "←/→: ein Bild · Umschalt+←/→: eine Sekunde · Leertaste: Abspielen/Pause · M: Schnitt "
        "vor das gezeigte Bild · Strg+←/→: Schnitt um ein Bild verschieben. Im Bildstreifen "
        "wählt ein Klick ein Bild, ein Klick in die Lücke zwischen zwei Bildern setzt dort den "
        "Schnitt; auf der Zeitleiste lässt sich der gelbe Marker ziehen.");
    if (mark.estimated_times)
        g_string_append(text, "\nDie Zeitpunkte der Bilder ließen sich nicht lesen und sind aus "
                        "der Bildrate geschätzt – der Schnitt ist darum nicht sicher bildgenau.");
    if (mark.sound_failed)
        g_string_append(text, "\nTon lässt sich nicht ausgeben (kein PulseAudio/PipeWire "
                        "erreichbar) – die Wiedergabe läuft ohne Ton.");
    if (selected_mode() == 0)
        g_string_append(text, mark.outro
                                  ? "\nIm Modus „Copy“ endet das Video nur ungefähr hier (auf "
                                    "wenige Bilder genau) – bildgenau schneiden die Modi per "
                                    "CPU oder GPU."
                                  : "\nIm Modus „Copy“ wird erst am nächsten Keyframe "
                                    "geschnitten – bildgenau schneiden die Modi per CPU oder "
                                    "GPU.");
    set_label_text(mark.hint_label, text->str);
    g_string_free(text, TRUE);
}

/* ---- Ausschnitt und Bildzeiten ---- */

/* Bildzeiten, geladene Bilder und laufendes Lesen verwerfen */
static void mark_forget_frames(void)
{
    mark_abort_load();
    mark_cache_clear();
    g_clear_pointer(&mark.frames, g_free);
    g_clear_pointer(&mark.unreadable, g_free);
    free(mark.times);
    mark.times = NULL;
    mark.frame_count = 0;
    mark.ready = false;
}

/* Bildzeiten des Ausschnitts übernehmen (ohne: aus der Bildrate schätzen), Schnitt und
 * Abspielposition auf Bilder legen und die Bedienung freigeben */
static void mark_install_times(double *times, int count)
{
    mark.estimated_times = count <= 0;
    if (mark.estimated_times) {
        free(times);
        count = (int)floor((mark.range_end - mark.range_start) * mark.fps) + 1;
        times = xmalloc((size_t)count * sizeof *times);
        for (int i = 0; i < count; i++) times[i] = mark.range_start + i / mark.fps;
    }
    mark.times = times;
    mark.frame_count = count;
    mark.frames = g_new0(GdkPixbuf *, count);
    mark.unreadable = g_new0(unsigned char, count);
    mark.cached = 0;
    bool play_at_cut = mark.play_time == mark.cut_time;    /* gerade geöffnet */
    mark.cut = mark_clamp_cut(mark_first_from(mark.cut_time));
    mark.cut_time = mark_cut_time_of(mark.cut);
    mark.ready = true;
    gtk_widget_set_sensitive(mark.controls, TRUE);
    gtk_widget_set_sensitive(mark.cut_row, TRUE);
    gtk_widget_set_sensitive(mark.apply_button, TRUE);
    mark_update_hint();
    /* gerade geöffnet: das erste Bild nach dem Schnitt zeigen, sonst das nächstgelegene */
    mark_set_play(play_at_cut ? mark.cut : mark_nearest(mark.play_time));
}

/* im Hauptthread: Bildzeiten sind gelesen */
static gboolean on_mark_times_done(gpointer p)
{
    MarkTimesJob *job = p;
    if (mark.dialog && job->serial == mark.serial && job->generation == mark.times_generation) {
        mark_install_times(job->times, job->count);
        job->times = NULL;
    }
    free(job->times);
    g_free(job->video);
    g_free(job);
    return G_SOURCE_REMOVE;
}

/* im Hintergrund: Zeitpunkte der Bilder des Ausschnitts lesen */
static gpointer mark_times_thread(gpointer p)
{
    MarkTimesJob *job = p;
    job->count = ic_frame_times(job->video, job->start, job->length, &job->times);
    g_idle_add(on_mark_times_done, job);
    return NULL;
}

/* Ausschnitt der Zeitleiste (Auswahl "Erste Minute" usw.) übernehmen: Vorschaubilder und
 * Bildzeiten neu lesen – Schnitt und Abspielposition behalten ihre Zeit */
static void mark_apply_range(void)
{
    int index = gtk_combo_box_get_active(GTK_COMBO_BOX(mark.range_combo));
    double length = MARK_RANGES[index < 0 ? 0 : index], duration = mark.props.duration;
    mark_stop_player();
    mark_forget_frames();
    if (mark.outro) {
        mark.range_end = duration;
        mark.range_start = fmax(0, duration - length);
    } else {
        mark.range_start = 0;
        mark.range_end = duration > 0 ? fmin(length, duration) : length;
    }
    mark_start_thumbs();
    gtk_widget_set_sensitive(mark.controls, FALSE);
    gtk_widget_set_sensitive(mark.cut_row, FALSE);
    gtk_widget_set_sensitive(mark.apply_button, FALSE);
    set_label_text(mark.hint_label, "Die Zeitpunkte der einzelnen Bilder werden gelesen …");
    MarkTimesJob *job = g_new0(MarkTimesJob, 1);
    job->serial = mark.serial;
    job->generation = ++mark.times_generation;
    job->video = g_strdup(mark.video);
    job->start = mark.range_start;
    job->length = mark.range_end - mark.range_start;
    g_thread_unref(g_thread_new("vorschau-zeiten", mark_times_thread, job));
    gtk_widget_queue_draw(mark.video_area);
    gtk_widget_queue_draw(mark.strip);
    gtk_widget_queue_draw(mark.timeline);
}

/* ---- Zeichnen ---- */

/* Text mit der Schrift des Widgets an (x, y) */
static void mark_draw_text(GtkWidget *w, cairo_t *cr, const char *text, double x, double y,
                           double r, double g, double b)
{
    PangoLayout *layout = gtk_widget_create_pango_layout(w, text);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_move_to(cr, x, y);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

/* Text waagerecht um x zentriert; markup: Pango-Auszeichnung */
static void mark_draw_centered(GtkWidget *w, cairo_t *cr, const char *markup, double x, double y)
{
    PangoLayout *layout = gtk_widget_create_pango_layout(w, NULL);
    pango_layout_set_markup(layout, markup, -1);
    int text_w;
    pango_layout_get_pixel_size(layout, &text_w, NULL);
    cairo_move_to(cr, x - text_w / 2.0, y);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

/* Rechteck mit runden Ecken als Pfad */
static void mark_rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr);
}

/* Schild oben links im Bild: fällt dieses Bild weg oder bleibt es? */
static void mark_draw_role(GtkWidget *w, cairo_t *cr, double x, double y)
{
    bool removed = mark_removed(mark.frame_time);
    const char *text = removed ? "wird abgeschnitten" : "bleibt";
    if (!mark.playing && mark.frame_index == mark.cut - 1)
        text = mark.outro ? "letztes Bild, das bleibt" : "letztes Bild, das abgeschnitten wird";
    else if (!mark.playing && mark.frame_index == mark.cut)
        text = mark.outro ? "erstes Bild, das abgeschnitten wird" : "erstes Bild, das bleibt";
    PangoLayout *layout = gtk_widget_create_pango_layout(w, NULL);
    char *markup = g_markup_printf_escaped("<b>%s</b>", text);
    pango_layout_set_markup(layout, markup, -1);
    g_free(markup);
    int text_w, text_h;
    pango_layout_get_pixel_size(layout, &text_w, &text_h);
    mark_rounded_rect(cr, x, y, text_w + 20, text_h + 8, (text_h + 8) / 2.0);
    if (removed)
        cairo_set_source_rgba(cr, 0.80, 0.11, 0.14, 0.92);
    else
        cairo_set_source_rgba(cr, 0.12, 0.52, 0.24, 0.92);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_move_to(cr, x + 10, y + 4);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

/* Videobild: Bild passend eingepasst mit Schild; fehlt das gewünschte Bild noch, das
 * vorige abgeblendet */
static gboolean on_mark_video_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    int width = gtk_widget_get_allocated_width(w), height = gtk_widget_get_allocated_height(w);
    cairo_set_source_rgb(cr, 0, 0, 0);
    cairo_paint(cr);
    const char *waiting = !mark.opened  ? "Video wird geöffnet …"
                          : !mark.ready ? "Bildzeiten werden gelesen …"
                                        : "Bild wird geladen …";
    if (!mark.frame) {
        mark_draw_text(w, cr, waiting, width / 2.0 - 70, height / 2.0 - 8, 0.6, 0.6, 0.6);
        return FALSE;
    }
    double frame_w = gdk_pixbuf_get_width(mark.frame), frame_h = gdk_pixbuf_get_height(mark.frame);
    double scale = fmin(width / frame_w, height / frame_h);
    double x0 = (width - frame_w * scale) / 2, y0 = (height - frame_h * scale) / 2;
    cairo_save(cr);
    cairo_translate(cr, x0, y0);
    cairo_scale(cr, scale, scale);
    gdk_cairo_set_source_pixbuf(cr, mark.frame, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
    if (!mark.ready || (!mark.playing && mark.frame_index != mark.play)) {
        if (mark.ready && mark.unreadable[mark.play])
            waiting = "Dieses Bild lässt sich nicht lesen.";
        cairo_set_source_rgba(cr, 0, 0, 0, 0.6);
        cairo_paint(cr);
        mark_draw_text(w, cr, waiting, width / 2.0 - 70, height / 2.0 - 8, 0.85, 0.85, 0.85);
        return FALSE;
    }
    mark_draw_role(w, cr, x0 + 10, y0 + 10);
    return FALSE;
}

/* Aufteilung des Bildstreifens: ungerade viele Plätze, das gezeigte Bild in der Mitte */
typedef struct {
    int first, slots;               /* Bild auf dem ersten Platz, Anzahl der Plätze */
    double x0, tile_w, tile_h, pitch;
} MarkStripLayout;

static MarkStripLayout mark_strip_layout(GtkWidget *w)
{
    MarkStripLayout l;
    int width = gtk_widget_get_allocated_width(w), height = gtk_widget_get_allocated_height(w);
    double aspect = mark.props.width > 0 && mark.props.height > 0
                        ? (double)mark.props.width / mark.props.height : 16.0 / 9;
    l.tile_h = height - MARK_STRIP_TEXT_H - 6;
    l.tile_w = fmax(24, l.tile_h * aspect);
    l.pitch = l.tile_w + MARK_STRIP_GAP;
    l.slots = (int)((width - 2 * MARK_PAD + MARK_STRIP_GAP) / l.pitch);
    l.slots = l.slots < 3 ? 3 : l.slots > MARK_STRIP_MAX ? MARK_STRIP_MAX : l.slots;
    if (l.slots % 2 == 0) l.slots--;
    l.first = mark.play - l.slots / 2;
    l.x0 = (width - (l.slots * l.pitch - MARK_STRIP_GAP)) / 2;
    return l;
}

/* Bildstreifen: die Nachbarbilder der Abspielposition, wegfallende rot getönt, das gezeigte
 * weiß umrandet, der Schnitt als gelbe Linie in der Lücke */
static gboolean on_mark_strip_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    cairo_set_source_rgb(cr, 0.13, 0.13, 0.14);
    cairo_paint(cr);
    if (!mark.ready) return FALSE;
    MarkStripLayout l = mark_strip_layout(w);
    double top = 4;
    for (int slot = 0; slot < l.slots; slot++) {
        int k = l.first + slot;
        if (k < 0 || k >= mark.frame_count) continue;
        double x = l.x0 + slot * l.pitch;
        GdkPixbuf *pixbuf = mark.frames[k];
        cairo_save(cr);
        cairo_rectangle(cr, x, top, l.tile_w, l.tile_h);
        cairo_clip(cr);
        if (pixbuf) {
            double pw = gdk_pixbuf_get_width(pixbuf), ph = gdk_pixbuf_get_height(pixbuf);
            double scale = fmin(l.tile_w / pw, l.tile_h / ph);
            cairo_translate(cr, x + (l.tile_w - pw * scale) / 2, top + (l.tile_h - ph * scale) / 2);
            cairo_scale(cr, scale, scale);
            gdk_cairo_set_source_pixbuf(cr, pixbuf, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
        } else {
            cairo_set_source_rgb(cr, 0.22, 0.22, 0.24);
        }
        cairo_paint(cr);
        cairo_restore(cr);
        if (mark_removed(mark.times[k])) {
            cairo_set_source_rgba(cr, 0.88, 0.11, 0.14, 0.40);
            cairo_rectangle(cr, x, top, l.tile_w, l.tile_h);
            cairo_fill(cr);
        }
        if (k == mark.play) {
            cairo_set_source_rgb(cr, 1, 1, 1);
            cairo_set_line_width(cr, 2);
            cairo_rectangle(cr, x - 1, top - 1, l.tile_w + 2, l.tile_h + 2);
            cairo_stroke(cr);
        }
        char position[32];
        format_position(mark.times[k], position, sizeof position);
        char *markup = g_markup_printf_escaped(k == mark.play ? "<small><b>%s</b></small>"
                                                              : "<small>%s</small>", position);
        cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
        mark_draw_centered(w, cr, markup, x + l.tile_w / 2, top + l.tile_h + 3);
        g_free(markup);
    }
    int gap = mark.cut - l.first;                       /* Lücke vor diesem Platz */
    if (gap >= 0 && gap <= l.slots) {
        double x = l.x0 + gap * l.pitch - MARK_STRIP_GAP / 2.0;
        cairo_set_source_rgb(cr, 0.96, 0.83, 0.18);
        cairo_set_line_width(cr, 3);
        cairo_move_to(cr, x, top);
        cairo_line_to(cr, x, top + l.tile_h);
        cairo_stroke(cr);
        cairo_move_to(cr, x - 6, 0);
        cairo_line_to(cr, x + 6, 0);
        cairo_line_to(cr, x, top + 6);
        cairo_close_path(cr);
        cairo_fill(cr);
    }
    return FALSE;
}

/* Platz im Bildstreifen unter x; *edge: -1/+1, wenn x in bzw. an der Lücke links/rechts
 * davon liegt, sonst 0 */
static int mark_strip_hit(GtkWidget *w, double x, int *edge)
{
    MarkStripLayout l = mark_strip_layout(w);
    double rel = x - l.x0 + MARK_STRIP_GAP / 2.0;
    int slot = (int)floor(rel / l.pitch);
    double into = rel - slot * l.pitch, grab = MARK_STRIP_GAP / 2.0 + 5;
    *edge = into <= grab ? -1 : into >= l.pitch - grab ? 1 : 0;
    return l.first + slot;
}

/* Klick in den Bildstreifen: auf ein Bild = dorthin, in eine Lücke = Schnitt dorthin */
static gboolean on_mark_strip_press(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    (void)d;
    if (!mark.ready || e->button != 1) return FALSE;
    int edge, k = mark_strip_hit(w, e->x, &edge);
    mark_pause();
    if (edge < 0)
        mark_set_cut(k);
    else if (edge > 0)
        mark_set_cut(k + 1);
    else
        mark_set_play(k);
    return TRUE;
}

/* über einer Lücke im Bildstreifen den Zieh-Zeiger zeigen */
static gboolean on_mark_strip_motion(GtkWidget *w, GdkEventMotion *e, gpointer d)
{
    (void)d;
    if (!mark.ready) return FALSE;
    int edge;
    mark_strip_hit(w, e->x, &edge);
    gdk_window_set_cursor(gtk_widget_get_window(w), edge ? mark.grab_cursor : NULL);
    return TRUE;
}

/* Zeitleiste: Filmstreifen, abgeschnittener Teil rot, Zeitskala, Abspielposition, Marker */
static gboolean on_mark_timeline_draw(GtkWidget *w, cairo_t *cr, gpointer d)
{
    (void)d;
    int width = gtk_widget_get_allocated_width(w), height = gtk_widget_get_allocated_height(w);
    double left = MARK_PAD, right = width - MARK_PAD;
    double strip_top = MARK_SCALE_H, strip_h = height - MARK_SCALE_H - 10;
    cairo_set_source_rgb(cr, 0.13, 0.13, 0.14);
    cairo_paint(cr);
    if (!mark.opened) return FALSE;

    /* Filmstreifen: Kacheln im Seitenverhältnis des Videos, jede mit dem Vorschaubild,
     * das zu ihrer Mitte gehört */
    double aspect = mark.props.width > 0 && mark.props.height > 0
                        ? (double)mark.props.width / mark.props.height : 16.0 / 9;
    double tile_w = fmax(24, strip_h * aspect);
    cairo_save(cr);
    cairo_rectangle(cr, left, strip_top, right - left, strip_h);
    cairo_clip(cr);
    for (double x0 = left; x0 < right; x0 += tile_w) {
        double center = mark_x_to_time(x0 + tile_w / 2, width);
        int index = mark.thumb_step > 0 ? (int)((center - mark.range_start) / mark.thumb_step) : 0;
        index = index < 0 ? 0 : index >= mark.thumb_count ? mark.thumb_count - 1 : index;
        GdkPixbuf *thumb = index >= 0 ? mark.thumbs[index] : NULL;
        cairo_save(cr);
        cairo_rectangle(cr, x0 + 1, strip_top, tile_w - 2, strip_h);
        cairo_clip(cr);
        if (thumb) {
            double scale = strip_h / gdk_pixbuf_get_height(thumb);
            double drawn_w = gdk_pixbuf_get_width(thumb) * scale;
            cairo_translate(cr, x0 + 1 + (tile_w - 2 - drawn_w) / 2, strip_top);
            cairo_scale(cr, scale, scale);
            gdk_cairo_set_source_pixbuf(cr, thumb, 0, 0);
        } else {
            cairo_set_source_rgb(cr, 0.22, 0.22, 0.24);
        }
        cairo_paint(cr);
        cairo_restore(cr);
    }
    cairo_restore(cr);

    /* der Teil, der abgeschnitten wird */
    double marker_x = mark_time_to_x(mark.cut_time, width);
    double cut_from = mark.outro ? marker_x : left, cut_to = mark.outro ? right : marker_x;
    cut_from = fmax(left, cut_from);
    cut_to = fmin(right, cut_to);
    if (cut_to > cut_from) {
        cairo_set_source_rgba(cr, 0.88, 0.11, 0.14, 0.40);
        cairo_rectangle(cr, cut_from, strip_top, cut_to - cut_from, strip_h);
        cairo_fill(cr);
    }

    /* Zeitskala: Striche und Beschriftung in sinnvollen Abständen */
    double length = mark.range_end - mark.range_start;
    double tick = length <= 20 ? 1 : length <= 70 ? 5 : length <= 150 ? 10 : 30;
    double label_every = length <= 20 ? 5 : tick * 2;
    cairo_set_source_rgb(cr, 0.65, 0.65, 0.65);
    cairo_set_line_width(cr, 1);
    for (double t = ceil(mark.range_start / tick) * tick; t <= mark.range_end + 1e-6; t += tick) {
        double x = round(mark_time_to_x(t, width)) + 0.5;
        bool labelled = fmod(t + 1e-6, label_every) < 1e-3;
        cairo_move_to(cr, x, strip_top - (labelled ? 6 : 3));
        cairo_line_to(cr, x, strip_top);
        cairo_stroke(cr);
        if (labelled) {
            char text[32];
            long secs = lround(t);
            if (secs >= 3600)
                snprintf(text, sizeof text, "%ld:%02ld:%02ld", secs / 3600, secs / 60 % 60,
                         secs % 60);
            else
                snprintf(text, sizeof text, "%ld:%02ld", secs / 60, secs % 60);
            PangoLayout *layout = gtk_widget_create_pango_layout(w, text);
            int text_w;
            pango_layout_get_pixel_size(layout, &text_w, NULL);
            g_object_unref(layout);
            double text_x = x + 3 + text_w > width ? x - 3 - text_w : x + 3;   /* rechter Rand */
            mark_draw_text(w, cr, text, text_x, 0, 0.65, 0.65, 0.65);
        }
    }

    /* Abspielposition: weiße Linie mit Dreieck unten */
    double play_x = mark_time_to_x(mark.play_time, width);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_set_line_width(cr, 2);
    cairo_move_to(cr, play_x, strip_top);
    cairo_line_to(cr, play_x, strip_top + strip_h);
    cairo_stroke(cr);
    cairo_move_to(cr, play_x - 6, height);
    cairo_line_to(cr, play_x + 6, height);
    cairo_line_to(cr, play_x, strip_top + strip_h);
    cairo_close_path(cr);
    cairo_fill(cr);

    /* Marker: gelbe Linie mit Griff oben */
    cairo_set_source_rgb(cr, 0.96, 0.83, 0.18);
    cairo_set_line_width(cr, 3);
    cairo_move_to(cr, marker_x, strip_top - 2);
    cairo_line_to(cr, marker_x, strip_top + strip_h);
    cairo_stroke(cr);
    cairo_move_to(cr, marker_x - 8, 2);
    cairo_line_to(cr, marker_x + 8, 2);
    cairo_line_to(cr, marker_x + 8, strip_top - 8);
    cairo_line_to(cr, marker_x, strip_top);
    cairo_line_to(cr, marker_x - 8, strip_top - 8);
    cairo_close_path(cr);
    cairo_fill(cr);
    return FALSE;
}

/* ---- Bedienung: Maus, Knöpfe, Tasten ---- */

/* Maustaste auf der Zeitleiste: am Marker ziehen, sonst die Abspielposition setzen */
static gboolean on_mark_timeline_press(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    (void)d;
    if (!mark.ready || e->button != 1) return FALSE;
    int width = gtk_widget_get_allocated_width(w);
    mark_stop_player();
    if (fabs(e->x - mark_time_to_x(mark.cut_time, width)) <= MARK_GRAB) {
        mark.drag = DRAG_MARKER;
    } else {
        mark.drag = DRAG_PLAYHEAD;
        mark_set_play(mark_nearest(mark_x_to_time(e->x, width)));
    }
    return TRUE;
}

/* Maus bewegt: Marker bzw. Abspielposition mitziehen; über dem Marker Zieh-Zeiger */
static gboolean on_mark_timeline_motion(GtkWidget *w, GdkEventMotion *e, gpointer d)
{
    (void)d;
    if (!mark.ready) return FALSE;
    int width = gtk_widget_get_allocated_width(w);
    double t = mark_x_to_time(e->x, width);
    if (mark.drag == DRAG_MARKER) {
        mark_set_cut(mark_first_from(t));
        mark_set_play(mark.cut);                /* das erste Bild nach dem Schnitt zeigen */
    } else if (mark.drag == DRAG_PLAYHEAD) {
        mark_set_play(mark_nearest(t));
    } else {
        bool near = fabs(e->x - mark_time_to_x(mark.cut_time, width)) <= MARK_GRAB;
        gdk_window_set_cursor(gtk_widget_get_window(w), near ? mark.grab_cursor : NULL);
    }
    return TRUE;
}

/* Maustaste losgelassen: jetzt auch die Nachbarbilder laden */
static gboolean on_mark_timeline_release(GtkWidget *w, GdkEventButton *e, gpointer d)
{
    (void)w;
    (void)e;
    (void)d;
    mark.drag = DRAG_NONE;
    mark_load_more();
    return TRUE;
}

/* Knopf Abspielen/Pause */
static void on_mark_play_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    if (mark.playing)
        mark_pause();
    else
        mark_play();
}

/* Sprung-Knöpfe (Daten: STEP_…) */
static void on_mark_step_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    mark_step(GPOINTER_TO_INT(d));
}

/* Knopf "… beginnt mit diesem Bild": Schnitt direkt davor */
static void on_mark_cut_here_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    mark_cut_here();
}

/* Knöpfe "Schnitt ein Bild früher/später" (Daten: -1 bzw. +1) */
static void on_mark_shift_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    mark_shift_cut(GPOINTER_TO_INT(d));
}

/* Knopf "Zum Schnitt": das erste Bild nach dem Schnitt zeigen */
static void on_mark_goto_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    mark_stop_player();
    mark_set_play(mark.cut);
}

/* Ton an/aus (läuft die Wiedergabe, wird sie mit der neuen Einstellung fortgesetzt) */
static void on_mark_sound_toggled(GtkToggleButton *b, gpointer d)
{
    (void)d;
    bool on = gtk_toggle_button_get_active(b);
    gtk_button_set_image(GTK_BUTTON(b), gtk_image_new_from_icon_name(
                                            on ? "audio-volume-high-symbolic"
                                               : "audio-volume-muted-symbolic",
                                            GTK_ICON_SIZE_BUTTON));
    if (mark.playing) {
        mark_stop_player();
        mark_play();
    }
}

/* Ausschnitt gewechselt */
static void on_mark_range_changed(GtkComboBox *c, gpointer d)
{
    (void)c;
    (void)d;
    if (mark.opened) mark_apply_range();
}

/* Tasten: Leertaste, Pfeile (Umschalt: Sekunden, Strg: Schnitt verschieben), Pos1/Ende, M */
static gboolean on_mark_key(GtkWidget *w, GdkEventKey *e, gpointer d)
{
    (void)w;
    (void)d;
    if (!mark.ready) return FALSE;
    bool shift = e->state & GDK_SHIFT_MASK, ctrl = e->state & GDK_CONTROL_MASK;
    switch (e->keyval) {
    case GDK_KEY_space:
        on_mark_play_clicked(NULL, NULL);
        return TRUE;
    case GDK_KEY_Left:
        if (ctrl)
            mark_shift_cut(-1);
        else
            mark_step(shift ? STEP_SECOND_BACK : STEP_FRAME_BACK);
        return TRUE;
    case GDK_KEY_Right:
        if (ctrl)
            mark_shift_cut(1);
        else
            mark_step(shift ? STEP_SECOND_FORWARD : STEP_FRAME_FORWARD);
        return TRUE;
    case GDK_KEY_Home:
        mark_step(STEP_START);
        return TRUE;
    case GDK_KEY_End:
        mark_step(STEP_END);
        return TRUE;
    case GDK_KEY_m:
    case GDK_KEY_M:
        mark_cut_here();
        return TRUE;
    default:
        return FALSE;
    }
}

/* im Hauptthread: Eigenschaften des Videos sind da – Ausschnitt wählen, Bildzeiten lesen */
static gboolean on_mark_props_done(gpointer p)
{
    MarkPropsJob *job = p;
    if (mark.dialog && job->serial == mark.serial) {
        if (!job->ok || job->props.width <= 0 || (mark.outro && job->props.duration <= 0)) {
            set_label_text(mark.hint_label,
                           !job->ok || job->props.width <= 0
                               ? "Das Video lässt sich nicht lesen."
                               : "Die Länge des Videos ist unbekannt – für das Outro wird sie "
                                 "gebraucht.");
        } else {
            mark.props = job->props;
            mark.fps = job->props.fps > 0 ? job->props.fps : 25;
            double aspect = (double)job->props.width / job->props.height;
            mark.frame_w = MARK_FRAME_W;
            mark.frame_h = (int)lround(MARK_FRAME_W / aspect / 2) * 2;
            mark.frame_h = mark.frame_h < 2 ? 2 : mark.frame_h > 720 ? 720 : mark.frame_h;
            mark.thumb_w = (int)lround(MARK_THUMB_H * aspect / 2) * 2;
            mark.thumb_w = mark.thumb_w < 2 ? 2 : mark.thumb_w > 400 ? 400 : mark.thumb_w;
            mark.sound_possible = job->props.has_audio && job->sound;
            gtk_widget_set_visible(mark.sound_button, mark.sound_possible);
            double current = mark.initial_length;
            mark.cut_time = mark.outro ? job->props.duration - current : current;
            mark.play_time = mark.cut_time;
            mark.opened = true;
            /* Ausschnitt so wählen, dass der bisherige Schnitt darin liegt */
            int range = current <= MARK_RANGES[0] - 10 ? 0 : current <= MARK_RANGES[1] - 10 ? 1
                                                                                            : 2;
            g_signal_handlers_block_by_func(mark.range_combo, on_mark_range_changed, NULL);
            gtk_combo_box_set_active(GTK_COMBO_BOX(mark.range_combo), range);
            g_signal_handlers_unblock_by_func(mark.range_combo, on_mark_range_changed, NULL);
            mark_apply_range();
        }
    }
    g_free(job->video);
    g_free(job);
    return G_SOURCE_REMOVE;
}

/* im Hintergrund: Eigenschaften des Videos lesen und prüfen, ob Ton möglich ist */
static gpointer mark_props_thread(gpointer p)
{
    MarkPropsJob *job = p;
    job->ok = ic_video_props(job->video, &job->props);
    job->sound = ic_sound_supported();
    g_idle_add(on_mark_props_done, job);
    return NULL;
}

/* Fenster schließen: "Übernehmen" meldet die Länge des Intros bzw. Outros – auf
 * Millisekunden gerundet, das ändert am Schnitt nichts (er liegt mit Abstand zwischen zwei
 * Bildern). Wurde der Schnitt nicht verschoben, bleibt die bisherige Länge stehen. */
static void on_mark_response(GtkDialog *dialog, gint response, gpointer d)
{
    (void)d;
    if (response == GTK_RESPONSE_ACCEPT && mark.ready && mark.cut_moved && mark.apply) {
        double length = round(mark_cut_length() * 1000) / 1000;
        double first_after = mark.cut < mark.frame_count ? mark.times[mark.cut] : mark.cut_time;
        mark.apply(length, first_after, mark.apply_data);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

/* Fenster zerstört: Hintergrund-Lesen beenden, Bilder freigeben */
static void on_mark_destroy(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
    mark_stop_player();
    mark_stop_thumbs();
    mark_forget_frames();
    mark.serial++;                              /* verspätete Antworten verwerfen */
    g_clear_object(&mark.frame);
    g_clear_object(&mark.grab_cursor);
    g_free(mark.video);
    mark.video = NULL;
    mark.dialog = NULL;
    mark.opened = mark.ready = mark.playing = false;
}

/* kleiner Knopf mit Symbol und Hinweis */
static GtkWidget *mark_icon_button(const char *icon, const char *tooltip, GCallback handler,
                                   gpointer data)
{
    GtkWidget *b = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(b, tooltip);
    g_signal_connect(b, "clicked", handler, data);
    return b;
}

/* Knopf mit Text und Hinweis */
static GtkWidget *mark_text_button(const char *label, const char *tooltip, GCallback handler,
                                   gpointer data)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    gtk_widget_set_tooltip_text(b, tooltip);
    g_signal_connect(b, "clicked", handler, data);
    return b;
}

/* Zeichenfläche mit Maus-Ereignissen */
static GtkWidget *mark_drawing_area(int height, GCallback draw, GCallback press,
                                    GCallback motion, GCallback release)
{
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, 480, height);
    gtk_widget_add_events(area, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                GDK_POINTER_MOTION_MASK);
    g_signal_connect(area, "draw", draw, NULL);
    if (press) g_signal_connect(area, "button-press-event", press, NULL);
    if (motion) g_signal_connect(area, "motion-notify-event", motion, NULL);
    if (release) g_signal_connect(area, "button-release-event", release, NULL);
    return area;
}

/* Markierfenster für video öffnen (bzw. nach vorn holen): Schnitt bei length Sekunden (vom
 * Anfang bzw. beim Outro vom Ende); "Übernehmen" ruft apply mit der neuen Länge */
static void open_mark_window_for(const char *video, bool outro, double length, MarkApply apply,
                                 gpointer apply_data)
{
    if (mark.dialog) {
        gtk_window_present(GTK_WINDOW(mark.dialog));
        return;
    }
    if (!video) return;
    int serial = mark.serial + 1;
    memset(&mark, 0, sizeof mark);
    mark.serial = serial;
    mark.video = g_strdup(video);
    mark.outro = outro;
    mark.initial_length = length;
    mark.apply = apply;
    mark.apply_data = apply_data;
    mark.fps = 25;
    mark.frame_index = -1;
    char *base = g_path_get_basename(mark.video), *name = path_for_display(base);
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        mark.outro ? "Outro-Beginn festlegen" : "Intro-Ende festlegen", GTK_WINDOW(ui.win),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Abbrechen", GTK_RESPONSE_CANCEL,
        "Übernehmen", GTK_RESPONSE_ACCEPT, NULL);
    mark.dialog = dialog;
    gtk_window_set_default_size(GTK_WINDOW(dialog), 960, 860);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    mark.apply_button = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog),
                                                           GTK_RESPONSE_ACCEPT);
    add_css_class(mark.apply_button, "suggested-action");
    gtk_widget_set_sensitive(mark.apply_button, FALSE);
    g_signal_connect(dialog, "response", G_CALLBACK(on_mark_response), NULL);
    g_signal_connect(dialog, "destroy", G_CALLBACK(on_mark_destroy), NULL);
    g_signal_connect(dialog, "key-press-event", G_CALLBACK(on_mark_key), NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_box_set_spacing(GTK_BOX(content), 8);
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);

    char *intro_text = g_strdup_printf(mark.outro ? "%s – das Ende des Videos. Den Schnitt genau "
                                                    "vor das erste Bild des Outros legen."
                                                  : "%s – der Anfang des Videos. Den Schnitt "
                                                    "genau vor das erste Bild nach dem Intro "
                                                    "legen.",
                                       name);
    gtk_box_pack_start(GTK_BOX(content), set_wrapping(new_label(intro_text, "dim-label"), 100),
                       FALSE, FALSE, 0);
    g_free(intro_text);
    g_free(base);
    g_free(name);

    mark.video_area = gtk_drawing_area_new();
    gtk_widget_set_size_request(mark.video_area, 480, 270);
    gtk_widget_set_vexpand(mark.video_area, TRUE);
    g_signal_connect(mark.video_area, "draw", G_CALLBACK(on_mark_video_draw), NULL);
    gtk_box_pack_start(GTK_BOX(content), mark.video_area, TRUE, TRUE, 0);

    mark.strip = mark_drawing_area(MARK_STRIP_H, G_CALLBACK(on_mark_strip_draw),
                                   G_CALLBACK(on_mark_strip_press),
                                   G_CALLBACK(on_mark_strip_motion), NULL);
    gtk_box_pack_start(GTK_BOX(content), mark.strip, FALSE, FALSE, 0);
    mark.timeline = mark_drawing_area(MARK_TIMELINE_H, G_CALLBACK(on_mark_timeline_draw),
                                      G_CALLBACK(on_mark_timeline_press),
                                      G_CALLBACK(on_mark_timeline_motion),
                                      G_CALLBACK(on_mark_timeline_release));
    mark.grab_cursor = gdk_cursor_new_from_name(gtk_widget_get_display(ui.win), "ew-resize");
    gtk_box_pack_start(GTK_BOX(content), mark.timeline, FALSE, FALSE, 0);

    /* Zeile 1: Wiedergabe und Sprünge, Position, Ton, Ausschnitt */
    mark.controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(mark.controls),
                       mark_icon_button("media-skip-backward-symbolic",
                                        "An den Anfang des Ausschnitts (Pos1)",
                                        G_CALLBACK(on_mark_step_clicked),
                                        GINT_TO_POINTER(STEP_START)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mark.controls),
                       mark_icon_button("media-seek-backward-symbolic",
                                        "Eine Sekunde zurück (Umschalt+←)",
                                        G_CALLBACK(on_mark_step_clicked),
                                        GINT_TO_POINTER(STEP_SECOND_BACK)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mark.controls),
                       mark_icon_button("go-previous-symbolic", "Ein Bild zurück (←)",
                                        G_CALLBACK(on_mark_step_clicked),
                                        GINT_TO_POINTER(STEP_FRAME_BACK)), FALSE, FALSE, 0);
    mark.play_button = mark_icon_button("media-playback-start-symbolic", "Abspielen (Leertaste)",
                                        G_CALLBACK(on_mark_play_clicked), NULL);
    add_css_class(mark.play_button, "suggested-action");
    gtk_box_pack_start(GTK_BOX(mark.controls), mark.play_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mark.controls),
                       mark_icon_button("go-next-symbolic", "Ein Bild vor (→)",
                                        G_CALLBACK(on_mark_step_clicked),
                                        GINT_TO_POINTER(STEP_FRAME_FORWARD)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mark.controls),
                       mark_icon_button("media-seek-forward-symbolic",
                                        "Eine Sekunde vor (Umschalt+→)",
                                        G_CALLBACK(on_mark_step_clicked),
                                        GINT_TO_POINTER(STEP_SECOND_FORWARD)), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mark.controls),
                       mark_icon_button("media-skip-forward-symbolic",
                                        "Ans Ende des Ausschnitts (Ende)",
                                        G_CALLBACK(on_mark_step_clicked),
                                        GINT_TO_POINTER(STEP_END)), FALSE, FALSE, 0);
    mark.time_label = new_label("0:00,000", "ic-time");
    gtk_widget_set_margin_start(mark.time_label, 10);
    gtk_box_pack_start(GTK_BOX(mark.controls), mark.time_label, FALSE, FALSE, 0);
    mark.range_combo = gtk_combo_box_text_new();
    for (int i = 0; i < 3; i++)
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(mark.range_combo), NULL,
                                  MARK_RANGE_TEXT[mark.outro][i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(mark.range_combo), 0);
    gtk_widget_set_tooltip_text(mark.range_combo, "Welcher Teil des Videos auf der Zeitleiste "
                                "zu sehen ist");
    g_signal_connect(mark.range_combo, "changed", G_CALLBACK(on_mark_range_changed), NULL);
    gtk_box_pack_end(GTK_BOX(mark.controls), mark.range_combo, FALSE, FALSE, 0);
    mark.sound_button = gtk_toggle_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(mark.sound_button), TRUE);
    gtk_button_set_image(GTK_BUTTON(mark.sound_button),
                         gtk_image_new_from_icon_name("audio-volume-high-symbolic",
                                                      GTK_ICON_SIZE_BUTTON));
    gtk_widget_set_tooltip_text(mark.sound_button, "Ton bei der Wiedergabe an/aus");
    gtk_widget_set_no_show_all(mark.sound_button, TRUE);
    g_signal_connect(mark.sound_button, "toggled", G_CALLBACK(on_mark_sound_toggled), NULL);
    gtk_box_pack_end(GTK_BOX(mark.controls), mark.sound_button, FALSE, FALSE, 0);
    gtk_widget_set_sensitive(mark.controls, FALSE);
    gtk_box_pack_start(GTK_BOX(content), mark.controls, FALSE, FALSE, 0);

    /* Zeile 2: Schnitt setzen, verschieben und anspringen */
    mark.cut_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_pack_start(GTK_BOX(mark.cut_row),
                       mark_text_button(mark.outro ? "Outro beginnt mit diesem Bild"
                                                   : "Video beginnt mit diesem Bild",
                                        "Schnitt direkt vor das gezeigte Bild legen (M)",
                                        G_CALLBACK(on_mark_cut_here_clicked), NULL),
                       FALSE, FALSE, 0);
    GtkWidget *shift_label = new_label("Schnitt verschieben:", "dim-label");
    gtk_widget_set_margin_start(shift_label, 12);
    gtk_box_pack_start(GTK_BOX(mark.cut_row), shift_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mark.cut_row),
                       mark_text_button("◀ 1 Bild", "Schnitt ein Bild früher (Strg+←)",
                                        G_CALLBACK(on_mark_shift_clicked), GINT_TO_POINTER(-1)),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(mark.cut_row),
                       mark_text_button("1 Bild ▶", "Schnitt ein Bild später (Strg+→)",
                                        G_CALLBACK(on_mark_shift_clicked), GINT_TO_POINTER(1)),
                       FALSE, FALSE, 0);
    GtkWidget *goto_button = mark_text_button("Zum Schnitt", "Das erste Bild nach dem Schnitt "
                                              "zeigen", G_CALLBACK(on_mark_goto_clicked), NULL);
    gtk_widget_set_margin_start(goto_button, 12);
    gtk_box_pack_start(GTK_BOX(mark.cut_row), goto_button, FALSE, FALSE, 0);
    gtk_widget_set_sensitive(mark.cut_row, FALSE);
    gtk_box_pack_start(GTK_BOX(content), mark.cut_row, FALSE, FALSE, 0);

    mark.cut_label = new_label("", "ic-title");
    gtk_box_pack_start(GTK_BOX(content), mark.cut_label, FALSE, FALSE, 0);

    mark.hint_label = set_wrapping(new_label("Video wird geöffnet …", "dim-label"), 110);
    add_css_class(mark.hint_label, "ic-sub");
    gtk_box_pack_start(GTK_BOX(content), mark.hint_label, FALSE, FALSE, 0);

    gtk_widget_show_all(dialog);
    MarkPropsJob *job = g_new0(MarkPropsJob, 1);
    job->serial = mark.serial;
    job->video = g_strdup(mark.video);
    g_thread_unref(g_thread_new("vorschau-info", mark_props_thread, job));
}

/* "Übernehmen" bei der Suche mit Referenz-Video: Länge ins Einstellungsfeld */
static void mark_apply_length(double length, double first_after_cut, gpointer data)
{
    (void)first_after_cut;
    (void)data;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.length_spin), fmax(0.1, length));
}

/* Knopf "Im Video festlegen …" */
static void on_mark_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    open_mark_window_for(ui.ref_video, outro_selected(), selected_length(), mark_apply_length,
                         NULL);
}

/* ---------------------------------------------------------------------------
 * Seite 3 der automatischen Erkennung: die gefundenen Intros. Je Gruppe zwei Bilder eines
 * typischen Videos (letztes Bild des Intros, erstes danach), Länge und Anzahl; die Videos
 * klappen auf, jedes mit seinem eigenen Schnitt. Sichere sind ausgewählt, unsichere nicht
 * (mit Grund). "Ansehen …" öffnet das bildgenaue Markierfenster; eine Korrektur am typischen
 * Video gilt für die ganze Gruppe – je Video auf dessen eigene Bilder gelegt.
 * ------------------------------------------------------------------------- */
#define AUTO_GROUPS_BATCH 30        /* Gruppen auf einmal (weitere per Knopf) */
#define AUTO_THUMB_W 176            /* Breite der beiden Bilder einer Gruppe */
#define AUTO_THUMB_H 100

typedef struct {                    /* Bilder einer Gruppe lesen (Thread-Pool) */
    int generation, group;
    char *path;
    double t[2];                    /* letztes Bild des Intros, erstes danach */
} AutoThumbJob;

typedef struct {
    int generation, group, slot;
    guchar *rgb;                    /* NULL = nicht lesbar */
} AutoThumbImage;

typedef struct {                    /* Korrektur auf eine Gruppe übertragen (Hintergrund) */
    int generation, group, n;
    double delta;                   /* so viel später (+) bzw. früher endet das Intro (s) */
    int *videos;
    char **paths;
    double *first;                  /* bisher erstes Bild nach dem Intro */
    double *cut, *last, *new_first; /* Ergebnis; cut 0 = nicht möglich */
} AutoShiftJob;

static IcAutoVideo *auto_info(int v)
{
    return &ui.auto_result.info[v];
}

/* Sekunden für die Anzeige: "7,990 s" */
static void format_secs(double secs, int decimals, char *out, size_t size)
{
    snprintf(out, size, "%.*f s", decimals, secs);
    char *dot = strchr(out, '.');
    if (dot) *dot = ',';
}

/* Anzahl ausgewählter Videos (auch in noch nicht angezeigten Zeilen) */
static int auto_selected_count(void)
{
    int n = 0;
    for (int v = 0; v < ui.auto_result.videos.n; v++) n += ui.auto_selected[v];
    return n;
}

/* "12 von 40 ausgewählt" und der Knopf "12 Videos schneiden" */
static void update_auto_selection(void)
{
    int n = auto_selected_count(), cuttable = 0;
    for (int v = 0; v < ui.auto_result.videos.n; v++) cuttable += auto_info(v)->cut_len > 0;
    char *count_text = g_strdup_printf("%d von %d ausgewählt", n, cuttable);
    char *button_text = n == 1 ? g_strdup("1 Video schneiden")
                               : g_strdup_printf("%d Videos schneiden", n);
    set_label_text(ui.auto_selection_label, count_text);
    gtk_button_set_label(GTK_BUTTON(ui.auto_cut_button), button_text);
    gtk_widget_set_sensitive(ui.auto_cut_button, n > 0);
    g_free(count_text);
    g_free(button_text);
}

/* Häkchen einer Gruppe nach ihren Videos stellen: alle, keins oder teils (Strich – ein Klick
 * wählt dann alle aus) */
static void auto_sync_group_check(int g)
{
    AutoGroupView *view = &ui.auto_views[g];
    if (!view->check) return;
    int selected = 0;
    for (int m = 0; m < view->nmembers; m++) selected += ui.auto_selected[view->members[m]];
    ui.auto_bulk = true;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->check), selected == view->nmembers);
    gtk_toggle_button_set_inconsistent(GTK_TOGGLE_BUTTON(view->check),
                                       selected > 0 && selected < view->nmembers);
    ui.auto_bulk = false;
}

/* angelegte Video-Häkchen nach der Auswahl stellen */
static void auto_sync_video_checks(void)
{
    ui.auto_bulk = true;
    for (guint i = 0; i < ui.auto_checks->len; i++) {
        GtkToggleButton *check = g_ptr_array_index(ui.auto_checks, i);
        int v = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(check), "video")) - 1;
        gtk_toggle_button_set_active(check, ui.auto_selected[v]);
    }
    ui.auto_bulk = false;
}

/* Häkchen eines Videos */
static void on_auto_video_toggled(GtkToggleButton *check, gpointer d)
{
    (void)d;
    if (ui.auto_bulk) return;
    int v = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(check), "video")) - 1;
    ui.auto_selected[v] = gtk_toggle_button_get_active(check);
    if (auto_info(v)->group >= 0) auto_sync_group_check(auto_info(v)->group);
    update_auto_selection();
}

/* Häkchen einer Gruppe: alle ihre Videos aus- bzw. abwählen */
static void on_auto_group_toggled(GtkToggleButton *check, gpointer d)
{
    if (ui.auto_bulk) return;
    AutoGroupView *view = &ui.auto_views[GPOINTER_TO_INT(d)];
    bool on = gtk_toggle_button_get_active(check);
    for (int m = 0; m < view->nmembers; m++) ui.auto_selected[view->members[m]] = on;
    gtk_toggle_button_set_inconsistent(check, FALSE);
    auto_sync_video_checks();
    update_auto_selection();
}

/* "Alle sicheren auswählen" (d = 1) bzw. "Keine auswählen" (d = 0) */
static void on_auto_select_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    bool sure_ones = GPOINTER_TO_INT(d);
    for (int v = 0; v < ui.auto_result.videos.n; v++)
        ui.auto_selected[v] = sure_ones && auto_info(v)->sure;
    auto_sync_video_checks();
    for (int g = 0; g < ui.auto_result.ngroups; g++) auto_sync_group_check(g);
    update_auto_selection();
}

/* Untertitel einer Video-Zeile: Ordner, Schnitt, ggf. warum unsicher */
static char *auto_video_subtitle(int v)
{
    const Video *video = &ui.auto_result.videos.v[v];
    const IcAutoVideo *info = auto_info(v);
    char cut[32], *dir = g_path_get_dirname(video->rel);
    char *folder = strcmp(dir, ".") ? path_for_display(dir) : g_strdup("Quellordner");
    format_secs(info->cut_len, 3, cut, sizeof cut);
    char *text = info->sure || !info->reason[0]
                     ? g_strdup_printf("%s · schneidet bei %s", folder, cut)
                     : g_strdup_printf("%s · schneidet bei %s · unsicher: %s", folder, cut,
                                       info->reason);
    g_free(dir);
    g_free(folder);
    return text;
}

/* Untertitel der angelegten Zeile von Video v erneuern (nach einer Korrektur) */
static void auto_refresh_video_row(int v)
{
    for (guint i = 0; i < ui.auto_checks->len; i++) {
        GtkWidget *check = g_ptr_array_index(ui.auto_checks, i);
        if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(check), "video")) - 1 != v) continue;
        char *text = auto_video_subtitle(v);
        set_label_text(g_object_get_data(G_OBJECT(check), "subtitle"), text);
        g_free(text);
    }
}

/* "Übernehmen" im Markierfenster für ein einzelnes Video: nur dieses ändern und auswählen */
static void auto_apply_video(double length, double first_after_cut, gpointer data)
{
    int v = GPOINTER_TO_INT(data) - 1;
    if (v < 0 || v >= ui.auto_result.videos.n) return;
    IcAutoVideo *info = auto_info(v);
    info->cut_len = length;
    info->first_content_t = first_after_cut;
    info->sure = true;                          /* vom Nutzer selbst festgelegt */
    info->reason[0] = 0;
    ui.auto_selected[v] = true;
    auto_refresh_video_row(v);
    auto_sync_video_checks();
    if (info->group >= 0) auto_sync_group_check(info->group);
    update_auto_selection();
}

/* Knopf "Ansehen …" eines Videos */
static void on_auto_view_video_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    int v = GPOINTER_TO_INT(d) - 1;
    open_mark_window_for(ui.auto_result.videos.v[v].path, false, auto_info(v)->cut_len,
                         auto_apply_video, d);
}

/* Zeile eines Videos: Häkchen, Name, Ordner und Schnitt, Ansehen, Abspielen */
static void add_auto_video_row(GtkWidget *list, int v)
{
    const Video *video = &ui.auto_result.videos.v[v];
    GtkWidget *check = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), ui.auto_selected[v]);
    g_object_set_data(G_OBJECT(check), "video", GINT_TO_POINTER(v + 1));
    g_signal_connect(check, "toggled", G_CALLBACK(on_auto_video_toggled), NULL);
    g_ptr_array_add(ui.auto_checks, check);
    GtkWidget *suffix = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *view_button = gtk_button_new_with_label("Ansehen …");
    gtk_widget_set_tooltip_text(view_button, "Den Schnitt bildgenau ansehen und korrigieren");
    g_signal_connect(view_button, "clicked", G_CALLBACK(on_auto_view_video_clicked),
                     GINT_TO_POINTER(v + 1));
    GtkWidget *play = gtk_button_new_from_icon_name("media-playback-start-symbolic",
                                                    GTK_ICON_SIZE_BUTTON);
    gtk_button_set_relief(GTK_BUTTON(play), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(play, "Im Videoplayer öffnen");
    g_signal_connect(play, "clicked", G_CALLBACK(on_play_clicked), (gpointer)video);
    gtk_box_pack_start(GTK_BOX(suffix), view_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(suffix), play, FALSE, FALSE, 0);
    char *base = g_path_get_basename(video->rel), *name = path_for_display(base);
    char *subtitle_text = auto_video_subtitle(v);
    GtkWidget *subtitle = NULL;
    GtkWidget *row = add_row(list, name, subtitle_text, check, suffix, NULL, &subtitle);
    g_object_set_data(G_OBJECT(check), "subtitle", subtitle);
    gtk_widget_show_all(row);
    g_free(base);
    g_free(name);
    g_free(subtitle_text);
}

/* nächste Portion Video-Zeilen einer Gruppe anlegen (vor dem Knopf) */
static void show_more_auto_members(AutoGroupView *view)
{
    int end = MIN(view->nmembers, view->shown + RESULT_ROWS_BATCH);
    for (; view->shown < end; view->shown++)
        add_auto_video_row(view->list, view->members[view->shown]);
    /* der Knopf bleibt am Ende der Liste */
    g_object_ref(view->more_row);
    gtk_container_remove(GTK_CONTAINER(view->list), view->more_row);
    gtk_container_add(GTK_CONTAINER(view->list), view->more_row);
    g_object_unref(view->more_row);
    int rest = view->nmembers - view->shown;
    if (rest > 0) {
        char *text = g_strdup_printf("Weitere %d Videos anzeigen (noch %d)",
                                     MIN(rest, RESULT_ROWS_BATCH), rest);
        gtk_button_set_label(GTK_BUTTON(view->more_button), text);
        g_free(text);
    } else {
        gtk_widget_hide(view->more_row);
    }
}

/* Knopf "Videos anzeigen" bzw. "Weitere … anzeigen" einer Gruppe */
static void on_auto_members_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    show_more_auto_members(&ui.auto_views[GPOINTER_TO_INT(d)]);
}

/* im Hauptthread: ein Bild einer Gruppe ist da */
static gboolean on_auto_thumb(gpointer p)
{
    AutoThumbImage *image = p;
    if (ui.win && image->generation == g_atomic_int_get(&ui.auto_generation) &&
        image->group < ui.auto_result.ngroups && ui.auto_views[image->group].image[image->slot]) {
        GtkImage *widget = GTK_IMAGE(ui.auto_views[image->group].image[image->slot]);
        if (image->rgb) {
            GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(image->rgb, GDK_COLORSPACE_RGB, FALSE, 8,
                                                         AUTO_THUMB_W, AUTO_THUMB_H,
                                                         AUTO_THUMB_W * 3, free_pixels, NULL);
            image->rgb = NULL;                  /* gehört jetzt dem Pixbuf */
            gtk_image_set_from_pixbuf(widget, pixbuf);
            g_object_unref(pixbuf);
        } else {
            gtk_image_set_from_icon_name(widget, "image-missing-symbolic", GTK_ICON_SIZE_DND);
        }
    }
    g_free(image->rgb);
    g_free(image);
    return G_SOURCE_REMOVE;
}

/* Thread-Pool: die beiden Bilder einer Gruppe bildgenau lesen (erstes Bild ab der Zeit) */
static void auto_thumb_worker(gpointer data, gpointer user)
{
    (void)user;
    AutoThumbJob *job = data;
    for (int slot = 0; slot < 2 && job->generation == g_atomic_int_get(&ui.auto_generation);
         slot++) {
        AutoThumbImage *image = g_new0(AutoThumbImage, 1);
        *image = (AutoThumbImage){job->generation, job->group, slot,
                                  g_malloc((size_t)AUTO_THUMB_W * AUTO_THUMB_H * 3)};
        IcFrameStream *stream = ic_frame_stream_open_frames(job->path, job->t[slot] - 0.0005, 0,
                                                            1, AUTO_THUMB_W, AUTO_THUMB_H);
        if (!stream || !ic_frame_stream_read(stream, image->rgb, NULL, NULL))
            g_clear_pointer(&image->rgb, g_free);
        ic_frame_stream_close(stream);
        g_idle_add(on_auto_thumb, image);
    }
    g_free(job->path);
    g_free(job);
}

/* Bilder einer Gruppe (vom typischen Video) laden */
static void auto_load_thumbs(int g)
{
    int rep = ui.auto_result.groups[g].representative;
    AutoThumbJob *job = g_new0(AutoThumbJob, 1);
    *job = (AutoThumbJob){g_atomic_int_get(&ui.auto_generation), g,
                          g_strdup(ui.auto_result.videos.v[rep].path),
                          {auto_info(rep)->last_intro_t, auto_info(rep)->first_content_t}};
    g_thread_pool_push(ui.auto_thumb_pool, job, NULL);
}

/* Text unter dem typischen Video einer Gruppe */
static void auto_update_group_summary(int g)
{
    AutoGroupView *view = &ui.auto_views[g];
    if (!view->summary) return;
    int rep = ui.auto_result.groups[g].representative;
    char cut[32];
    format_secs(auto_info(rep)->cut_len, 3, cut, sizeof cut);
    char *base = g_path_get_basename(ui.auto_result.videos.v[rep].rel);
    char *name = path_for_display(base);
    char *text = view->shifting
                     ? g_strdup("Die Korrektur wird auf alle Videos der Gruppe übertragen …")
                     : g_strdup_printf("Beispiel: %s · schneidet bei %s", name, cut);
    set_label_text(view->summary, text);
    g_free(text);
    g_free(base);
    g_free(name);
}

/* im Hauptthread: Korrektur ist auf die Gruppe übertragen */
static gboolean on_auto_shift_done(gpointer p)
{
    AutoShiftJob *job = p;
    if (ui.win && job->generation == g_atomic_int_get(&ui.auto_generation)) {
        AutoGroupView *view = &ui.auto_views[job->group];
        int failed = 0;
        for (int i = 0; i < job->n; i++) {
            IcAutoVideo *info = auto_info(job->videos[i]);
            if (job->cut[i] <= 0) {             /* Bilder nicht lesbar: lieber abwählen */
                failed++;
                info->sure = false;
                ui.auto_selected[job->videos[i]] = false;
                snprintf(info->reason, sizeof info->reason, "Korrektur ließ sich nicht übertragen");
            } else {
                info->cut_len = job->cut[i];
                info->last_intro_t = job->last[i];
                info->first_content_t = job->new_first[i];
            }
            auto_refresh_video_row(job->videos[i]);
        }
        view->shifting = false;
        auto_update_group_summary(job->group);
        auto_load_thumbs(job->group);
        auto_sync_video_checks();
        auto_sync_group_check(job->group);
        update_auto_selection();
        if (failed)
            show_info_bar(GTK_MESSAGE_WARNING, "Bei einigen Videos ließ sich die Korrektur nicht "
                          "übertragen – sie sind abgewählt.");
    }
    for (int i = 0; i < job->n; i++) g_free(job->paths[i]);
    g_free(job->paths);
    g_free(job->videos);
    g_free(job->first);
    g_free(job->cut);
    g_free(job->last);
    g_free(job->new_first);
    g_free(job);
    return G_SOURCE_REMOVE;
}

/* im Hintergrund: je Video das Bild suchen, das um delta verschoben am nächsten liegt, und
 * den Schnitt eine Viertel Bilddauer davor legen (wie beim Markieren) */
static gpointer auto_shift_thread(gpointer p)
{
    AutoShiftJob *job = p;
    for (int i = 0; i < job->n; i++) {
        double target = job->first[i] + job->delta, *t = NULL;
        int n = ic_frame_times(job->paths[i], fmax(0, target - 1), 2, &t), best = -1;
        for (int k = 1; k < n; k++)             /* mit Vorgänger: der Schnitt liegt davor */
            if (best < 0 || fabs(t[k] - target) < fabs(t[best] - target)) best = k;
        job->cut[i] = 0;
        if (best > 0 && fabs(t[best] - target) <= (t[best] - t[best - 1]) / 2 + 1e-3) {
            job->cut[i] = round((t[best] - (t[best] - t[best - 1]) / 4) * 1000) / 1000;
            job->last[i] = t[best - 1];
            job->new_first[i] = t[best];
        }
        free(t);
    }
    g_idle_add(on_auto_shift_done, job);
    return NULL;
}

/* "Übernehmen" im Markierfenster für das typische Video einer Gruppe: auf alle übertragen */
static void auto_apply_group(double length, double first_after_cut, gpointer data)
{
    (void)length;
    int g = GPOINTER_TO_INT(data) - 1;
    if (g < 0 || g >= ui.auto_result.ngroups) return;
    AutoGroupView *view = &ui.auto_views[g];
    int rep = ui.auto_result.groups[g].representative;
    double delta = first_after_cut - auto_info(rep)->first_content_t;
    if (fabs(delta) < 1e-4 || view->shifting) return;
    AutoShiftJob *job = g_new0(AutoShiftJob, 1);
    job->generation = g_atomic_int_get(&ui.auto_generation);
    job->group = g;
    job->delta = delta;
    job->n = view->nmembers;
    job->videos = g_memdup2(view->members, (gsize)view->nmembers * sizeof *view->members);
    job->paths = g_new0(char *, view->nmembers + 1);
    job->first = g_new0(double, view->nmembers + 1);
    job->cut = g_new0(double, view->nmembers + 1);
    job->last = g_new0(double, view->nmembers + 1);
    job->new_first = g_new0(double, view->nmembers + 1);
    for (int i = 0; i < view->nmembers; i++) {
        job->paths[i] = g_strdup(ui.auto_result.videos.v[view->members[i]].path);
        job->first[i] = auto_info(view->members[i])->first_content_t;
    }
    view->shifting = true;
    auto_update_group_summary(g);
    g_thread_unref(g_thread_new("intro-korrektur", auto_shift_thread, job));
}

/* Knopf "Ansehen …" einer Gruppe: typisches Video im Markierfenster */
static void on_auto_view_group_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    int g = GPOINTER_TO_INT(d) - 1;
    if (ui.auto_views[g].shifting) return;
    int rep = ui.auto_result.groups[g].representative;
    open_mark_window_for(ui.auto_result.videos.v[rep].path, false, auto_info(rep)->cut_len,
                         auto_apply_group, d);
}

/* Bild einer Gruppe mit Beschriftung darunter */
static GtkWidget *auto_thumb_box(const char *caption, GtkWidget **image)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    *image = gtk_image_new_from_icon_name("image-loading-symbolic", GTK_ICON_SIZE_DND);
    gtk_widget_set_size_request(*image, AUTO_THUMB_W, AUTO_THUMB_H);
    add_css_class(*image, "ic-thumb");
    GtkWidget *label = new_label(caption, "ic-sub");
    add_css_class(label, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(label), 0.5);
    gtk_box_pack_start(GTK_BOX(box), *image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    return box;
}

/* Gruppe g anzeigen: Überschrift, Kopfzeile mit Bildern und Häkchen, Knopf für die Videos */
static void add_auto_group(int g)
{
    const IcAutoGroup *group = &ui.auto_result.groups[g];
    AutoGroupView *view = &ui.auto_views[g];
    char length[32];
    format_secs(group->length, 1, length, sizeof length);
    char *title = g_strdup_printf("Intro %d · etwa %s · %d Video%s", g + 1, length,
                                  view->nmembers, view->nmembers == 1 ? "" : "s");
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(head), new_label(title, "ic-heading"), FALSE, FALSE, 0);
    g_free(title);
    /* Kennzeichen: sicher (vorausgewählt) und/oder unsicher (nicht ausgewählt) */
    int unsure = view->nmembers - group->sure;
    for (int k = 0; k < 2; k++) {
        if (k == 0 ? group->sure <= 0 : unsure <= 0) continue;
        char *text = k == 0 ? (unsure ? g_strdup_printf("%d sicher", group->sure)
                                      : g_strdup("sicher"))
                            : (group->sure ? g_strdup_printf("%d unsicher", unsure)
                                           : g_strdup("unsicher"));
        GtkWidget *badge = new_label(text, "ic-badge");
        g_free(text);
        add_css_class(badge, k == 0 ? "ok" : "near");
        gtk_widget_set_valign(badge, GTK_ALIGN_END);
        gtk_widget_set_margin_bottom(badge, 5);
        gtk_widget_set_tooltip_text(badge, k == 0 ? "Gleiches Intro in genug Videos, Ende "
                                                    "eindeutig – diese Videos sind ausgewählt"
                                                  : "Nicht ausgewählt – der Grund steht bei "
                                                    "den Videos (sie stehen zuerst)");
        gtk_box_pack_start(GTK_BOX(head), badge, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(ui.auto_box), head, FALSE, FALSE, 0);
    view->list = add_group(ui.auto_box, NULL);

    /* Kopfzeile: Häkchen, die beiden Bilder, Text, Ansehen */
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    gtk_widget_set_margin_top(grid, 10);
    gtk_widget_set_margin_bottom(grid, 10);
    gtk_widget_set_margin_start(grid, 14);
    gtk_widget_set_margin_end(grid, 12);
    view->check = gtk_check_button_new();
    gtk_widget_set_valign(view->check, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(view->check, "Alle Videos dieser Gruppe aus- bzw. abwählen");
    g_signal_connect(view->check, "toggled", G_CALLBACK(on_auto_group_toggled),
                     GINT_TO_POINTER(g));
    GtkWidget *pictures = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(pictures), auto_thumb_box("Letztes Bild des Intros",
                                                         &view->image[0]), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(pictures), auto_thumb_box("Erstes Bild danach", &view->image[1]),
                       FALSE, FALSE, 0);
    view->summary = new_label("", "ic-sub");
    add_css_class(view->summary, "dim-label");
    set_wrapping(view->summary, 40);
    gtk_widget_set_hexpand(view->summary, TRUE);
    gtk_widget_set_valign(view->summary, GTK_ALIGN_CENTER);
    GtkWidget *view_button = gtk_button_new_with_label("Ansehen …");
    gtk_widget_set_valign(view_button, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(view_button, "Das Intro-Ende am typischen Video bildgenau "
                                "ansehen – eine Korrektur gilt für alle Videos der Gruppe");
    g_signal_connect(view_button, "clicked", G_CALLBACK(on_auto_view_group_clicked),
                     GINT_TO_POINTER(g + 1));
    gtk_grid_attach(GTK_GRID(grid), view->check, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), pictures, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), view->summary, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), view_button, 3, 0, 1, 1);
    gtk_container_add(GTK_CONTAINER(row), grid);
    gtk_container_add(GTK_CONTAINER(view->list), row);

    /* Knopf, der die Videos der Gruppe zeigt */
    view->more_button = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(view->more_button), GTK_RELIEF_NONE);
    char *more = g_strdup_printf("Videos anzeigen (%d)", view->nmembers);
    gtk_button_set_label(GTK_BUTTON(view->more_button), more);
    g_free(more);
    g_signal_connect(view->more_button, "clicked", G_CALLBACK(on_auto_members_clicked),
                     GINT_TO_POINTER(g));
    view->more_row = gtk_list_box_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(view->more_row), FALSE);
    gtk_container_add(GTK_CONTAINER(view->more_row), view->more_button);
    gtk_container_add(GTK_CONTAINER(view->list), view->more_row);
    gtk_widget_show_all(head);
    gtk_widget_show_all(gtk_widget_get_parent(view->list));
    auto_update_group_summary(g);
    auto_sync_group_check(g);
    auto_load_thumbs(g);
}

static void on_auto_more_groups_clicked(GtkButton *b, gpointer d);

/* nächste Portion Gruppen anzeigen; danach ggf. ein Knopf für weitere */
static void show_more_auto_groups(void)
{
    if (ui.auto_more_groups_row) {
        gtk_widget_destroy(ui.auto_more_groups_row);
        ui.auto_more_groups_row = NULL;
    }
    int end = MIN(ui.auto_result.ngroups, ui.auto_groups_shown + AUTO_GROUPS_BATCH);
    for (; ui.auto_groups_shown < end; ui.auto_groups_shown++) add_auto_group(ui.auto_groups_shown);
    int rest = ui.auto_result.ngroups - ui.auto_groups_shown;
    if (rest > 0) {
        char *text = g_strdup_printf("Weitere %d Intros anzeigen (noch %d)",
                                     MIN(rest, AUTO_GROUPS_BATCH), rest);
        ui.auto_more_groups_row = gtk_button_new_with_label(text);
        g_free(text);
        g_signal_connect(ui.auto_more_groups_row, "clicked",
                         G_CALLBACK(on_auto_more_groups_clicked), NULL);
        gtk_box_pack_start(GTK_BOX(ui.auto_box), ui.auto_more_groups_row, FALSE, FALSE, 6);
        gtk_widget_show(ui.auto_more_groups_row);
    }
    /* die Liste "Ohne erkanntes Intro" bleibt am Ende */
    if (ui.auto_others_list) {
        GtkWidget *parts[2] = {g_object_get_data(G_OBJECT(ui.auto_others_list), "heading"),
                               gtk_widget_get_parent(ui.auto_others_list)};
        for (int i = 0; i < 2; i++) {
            if (!parts[i]) continue;
            g_object_ref(parts[i]);
            gtk_container_remove(GTK_CONTAINER(ui.auto_box), parts[i]);
            gtk_box_pack_start(GTK_BOX(ui.auto_box), parts[i], FALSE, FALSE, 0);
            g_object_unref(parts[i]);
        }
    }
}

/* Knopf "Weitere … Intros anzeigen" */
static void on_auto_more_groups_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    show_more_auto_groups();
}

/* nächste Portion der Videos ohne erkanntes Intro (Name, Ordner, Grund) */
static void show_more_auto_others(void)
{
    int added = 0, rest = 0;
    for (int v = 0; v < ui.auto_result.videos.n; v++) {
        if (auto_info(v)->cut_len > 0) continue;
        if (v < ui.auto_others_shown) continue;
        if (added == RESULT_ROWS_BATCH) {
            rest++;
            continue;
        }
        const Video *video = &ui.auto_result.videos.v[v];
        char *base = g_path_get_basename(video->rel), *dir = g_path_get_dirname(video->rel);
        char *name = path_for_display(base);
        char *folder = strcmp(dir, ".") ? path_for_display(dir) : g_strdup("Quellordner");
        char *sub = g_strdup_printf("%s · %s", folder, auto_info(v)->reason[0]
                                                           ? auto_info(v)->reason
                                                           : "Kein wiederkehrendes Intro gefunden");
        GtkWidget *row = add_row(ui.auto_others_list, name, sub, NULL, NULL, NULL, NULL);
        gtk_widget_show_all(row);
        g_free(base);
        g_free(dir);
        g_free(name);
        g_free(folder);
        g_free(sub);
        ui.auto_others_shown = v + 1;
        added++;
    }
    GtkWidget *more_row = g_object_get_data(G_OBJECT(ui.auto_others_list), "more-row");
    g_object_ref(more_row);
    gtk_container_remove(GTK_CONTAINER(ui.auto_others_list), more_row);
    gtk_container_add(GTK_CONTAINER(ui.auto_others_list), more_row);
    g_object_unref(more_row);
    if (rest > 0) {
        char *text = g_strdup_printf("Weitere %d anzeigen (noch %d)", MIN(rest, RESULT_ROWS_BATCH),
                                     rest);
        gtk_button_set_label(GTK_BUTTON(ui.auto_others_more), text);
        g_free(text);
    } else {
        gtk_widget_hide(more_row);
    }
}

/* Knopf "Anzeigen" bzw. "Weitere …" bei den Videos ohne erkanntes Intro */
static void on_auto_others_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    show_more_auto_others();
}

/* Trefferseite der automatischen Erkennung leeren (neues Ergebnis bzw. neue Suche) */
static void clear_auto_page(void)
{
    g_atomic_int_inc(&ui.auto_generation);      /* offene Bilder und Korrekturen verwerfen */
    if (ui.auto_box) {
        GList *old = gtk_container_get_children(GTK_CONTAINER(ui.auto_box));
        for (GList *item = old; item; item = item->next) gtk_widget_destroy(item->data);
        g_list_free(old);
    }
    for (int g = 0; g < ui.auto_view_count; g++) g_free(ui.auto_views[g].members);
    g_clear_pointer(&ui.auto_views, g_free);
    g_clear_pointer(&ui.auto_selected, g_free);
    ui.auto_view_count = 0;
    if (ui.auto_checks) g_ptr_array_set_size(ui.auto_checks, 0);
    ui.auto_more_groups_row = ui.auto_others_list = ui.auto_others_more = NULL;
    ui.auto_groups_shown = ui.auto_others_shown = 0;
}

/* Seite mit dem Ergebnis der automatischen Erkennung füllen */
static void fill_auto_page(void)
{
    const IcAuto *a = &ui.auto_result;
    clear_auto_page();
    int n = a->videos.n, sure = 0, unsure = 0, others = 0;
    ui.auto_selected = g_new0(bool, n + 1);
    ui.auto_views = g_new0(AutoGroupView, a->ngroups + 1);
    ui.auto_view_count = a->ngroups;
    for (int v = 0; v < n; v++) {
        const IcAutoVideo *info = &a->info[v];
        ui.auto_selected[v] = info->sure;
        if (info->cut_len <= 0) {
            others++;
            continue;
        }
        info->sure ? sure++ : unsure++;
        if (info->group >= 0) ui.auto_views[info->group].nmembers++;
    }
    for (int g = 0; g < a->ngroups; g++) {
        ui.auto_views[g].members = g_new(int, ui.auto_views[g].nmembers + 1);
        ui.auto_views[g].nmembers = 0;
    }
    for (int pass = 0; pass < 2; pass++)        /* je Gruppe die unsicheren zuerst */
        for (int v = 0; v < n; v++)
            if (a->info[v].cut_len > 0 && a->info[v].group >= 0 && a->info[v].sure == pass) {
                AutoGroupView *view = &ui.auto_views[a->info[v].group];
                view->members[view->nmembers++] = v;
            }

    char *title = a->ngroups ? g_strdup_printf("%d Intro%s in %d Video%s gefunden", a->ngroups,
                                               a->ngroups == 1 ? "" : "s", sure + unsure,
                                               sure + unsure == 1 ? "" : "s")
                             : g_strdup_printf("In den %d Videos kein wiederkehrendes Intro "
                                               "gefunden", n);
    set_label_text(ui.auto_title, title);
    g_free(title);
    char secs[64];
    GString *summary = g_string_new(NULL);
    g_string_append_printf(summary, "Suche dauerte %s · Intros bis %.0f s, die in mindestens %d "
                           "Videos vorkommen. %d sicher erkannt (ausgewählt), %d unsicher (nicht "
                           "ausgewählt), %d ohne erkanntes Intro.",
                           format_duration(a->secs, secs, sizeof secs), a->max_len, a->min_videos,
                           sure, unsure, others);
    if (a->unreadable)
        g_string_append_printf(summary, " %d Datei(en) waren nicht lesbar.", a->unreadable);
    set_label_text(ui.auto_subtitle, summary->str);
    g_string_free(summary, TRUE);

    show_more_auto_groups();
    if (others) {                               /* Videos ohne erkanntes Intro (aufklappbar) */
        char *heading_text = g_strdup_printf("Ohne erkanntes Intro (%d)", others);
        GtkWidget *heading = new_label(heading_text, "ic-heading");
        g_free(heading_text);
        gtk_box_pack_start(GTK_BOX(ui.auto_box), heading, FALSE, FALSE, 0);
        ui.auto_others_list = add_group(ui.auto_box, NULL);
        g_object_set_data(G_OBJECT(ui.auto_others_list), "heading", heading);
        ui.auto_others_more = gtk_button_new_with_label("Anzeigen");
        gtk_button_set_relief(GTK_BUTTON(ui.auto_others_more), GTK_RELIEF_NONE);
        g_signal_connect(ui.auto_others_more, "clicked", G_CALLBACK(on_auto_others_clicked), NULL);
        GtkWidget *more_row = gtk_list_box_row_new();
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(more_row), FALSE);
        gtk_container_add(GTK_CONTAINER(more_row), ui.auto_others_more);
        gtk_container_add(GTK_CONTAINER(ui.auto_others_list), more_row);
        g_object_set_data(G_OBJECT(ui.auto_others_list), "more-row", more_row);
        gtk_widget_show_all(heading);
        gtk_widget_show_all(gtk_widget_get_parent(ui.auto_others_list));
    }
    gtk_widget_show(ui.auto_box);
    GtkStyleContext *ctx = gtk_widget_get_style_context(ui.auto_cut_button);
    gtk_style_context_remove_class(ctx, ui.run_replace ? "suggested-action" : "destructive-action");
    gtk_style_context_add_class(ctx, ui.run_replace ? "destructive-action" : "suggested-action");
    update_auto_selection();
}

/* Seite 3 der automatischen Erkennung */
static GtkWidget *build_auto_page(void)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *scroller, *box = new_page_column(&scroller);
    ui.auto_title = new_label("", "ic-hero");
    set_wrapping(ui.auto_title, 40);
    ui.auto_subtitle = new_label("", "dim-label");
    set_wrapping(ui.auto_subtitle, 80);
    gtk_widget_set_margin_top(ui.auto_title, 8);
    gtk_box_pack_start(GTK_BOX(box), ui.auto_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.auto_subtitle, FALSE, FALSE, 0);
    GtkWidget *tools = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *sure_button = gtk_button_new_with_label("Alle sicheren auswählen");
    GtkWidget *none = gtk_button_new_with_label("Keine auswählen");
    gtk_button_set_relief(GTK_BUTTON(sure_button), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(none), GTK_RELIEF_NONE);
    g_signal_connect(sure_button, "clicked", G_CALLBACK(on_auto_select_clicked),
                     GINT_TO_POINTER(1));
    g_signal_connect(none, "clicked", G_CALLBACK(on_auto_select_clicked), GINT_TO_POINTER(0));
    gtk_box_pack_start(GTK_BOX(tools), sure_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(tools), none, FALSE, FALSE, 0);
    gtk_widget_set_margin_top(tools, 6);
    gtk_box_pack_start(GTK_BOX(box), tools, FALSE, FALSE, 0);
    ui.auto_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(box), ui.auto_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), scroller, TRUE, TRUE, 0);

    GtkWidget *action_bar = gtk_action_bar_new();
    ui.auto_selection_label = new_label("", "dim-label");
    gtk_widget_set_margin_start(ui.auto_selection_label, 12);
    gtk_action_bar_pack_start(GTK_ACTION_BAR(action_bar), ui.auto_selection_label);
    ui.auto_cut_button = new_pill_button("Schneiden", "suggested-action");
    gtk_widget_set_margin_top(ui.auto_cut_button, 6);
    gtk_widget_set_margin_bottom(ui.auto_cut_button, 6);
    gtk_widget_set_margin_end(ui.auto_cut_button, 12);
    g_signal_connect(ui.auto_cut_button, "clicked", G_CALLBACK(on_cut_clicked), NULL);
    gtk_action_bar_pack_end(GTK_ACTION_BAR(action_bar), ui.auto_cut_button);
    gtk_box_pack_end(GTK_BOX(outer), action_bar, FALSE, FALSE, 0);
    ui.auto_checks = g_ptr_array_new();
    ui.auto_thumb_pool = g_thread_pool_new(auto_thumb_worker, NULL, THUMB_THREADS, FALSE, NULL);
    return outer;
}

/* ---------------------------------------------------------------------------
 * Ablauf: Erkennung
 * ------------------------------------------------------------------------- */
typedef struct {
    int rc;
    char *err;
} SearchDone;

/* Während Suche, Bearbeitung und Verschieben: kein automatischer Standby, beim
 * Abmelden/Herunterfahren fragt die Sitzung nach (die Engine hält zusätzlich
 * eine logind-Sperre gegen Standby) */
static void session_inhibit(bool on, const char *reason)
{
    if (on && !ui.inhibit_cookie) {
        GtkApplicationInhibitFlags flags = GTK_APPLICATION_INHIBIT_SUSPEND |
                                           GTK_APPLICATION_INHIBIT_LOGOUT;
        ui.inhibit_cookie = gtk_application_inhibit(ui.app, GTK_WINDOW(ui.win), flags, reason);
    } else if (!on && ui.inhibit_cookie) {
        gtk_application_uninhibit(ui.app, ui.inhibit_cookie);
        ui.inhibit_cookie = 0;
    }
}

/* Programm beenden: Einstellungen speichern, Sperren lösen, aufräumen */
static void quit_application(void)
{
    settings_save();
    session_inhibit(false, NULL);
    ic_cleanup_temps();
    GtkWidget *w = ui.win;
    ui.win = NULL;                              /* spätere Idle-Callbacks ignorieren */
    gtk_widget_destroy(w);
}

/* im Hauptthread: Suche beendet – Ergebnis oder Fehler anzeigen */
static gboolean on_search_done(gpointer p)
{
    SearchDone *done = p;
    g_thread_join(ui.worker_thread);
    ui.worker_thread = NULL;
    ui.busy = false;
    session_inhibit(false, NULL);
    ui.search_started = true;
    ic_cleanup_temps();
    update_cache_row();                         /* die Suche hat den Cache ergänzt */
    if (ui.quit_when_idle) {
        g_free(done->err);
        g_free(done);
        quit_application();
        return G_SOURCE_REMOVE;
    }
    if (done->rc == 1) {
        show_page("setup");
        show_info_bar(GTK_MESSAGE_INFO, "Suche abgebrochen.");
    } else if (done->rc == -1) {
        show_page("setup");
        show_info_bar(GTK_MESSAGE_ERROR, done->err);
    } else if (ui.run_auto) {
        fill_auto_page();
        show_page("auto");
    } else {
        fill_results_page();
        show_page("results");
    }
    ic_cancel = 0;
    update_start_button();
    g_free(done->err);
    g_free(done);
    return G_SOURCE_REMOVE;
}

/* die Suche läuft in einem eigenen Thread */
static gpointer search_thread(gpointer p)
{
    (void)p;
    const char *err = NULL;
    SearchDone *done = g_new0(SearchDone, 1);
    if (ui.run_auto)
        done->rc = ic_detect_auto(ui.run_src_dir, ui.run_out_dir, ui.run_auto_max,
                                  ui.run_auto_min, &ui.auto_result, &err);
    else
        done->rc = ic_detect(ui.run_ref_video, ui.run_length, ui.run_src_dir, ui.run_out_dir,
                             ui.run_threshold, ui.run_frames, ui.run_outro, &ui.detection, &err);
    done->err = g_strdup(err);
    g_idle_add(on_search_done, done);
    return NULL;
}

/* Knopf "Intro suchen": Eingaben prüfen und die Suche starten */
static void on_search_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    show_info_bar(GTK_MESSAGE_INFO, NULL);
    if (!ui.ffmpeg_ok) {
        show_info_bar(GTK_MESSAGE_ERROR, "ffmpeg ist nicht installiert – bitte über die "
                      "Anwendungsverwaltung oder mit „sudo apt install ffmpeg“ nachinstallieren.");
        return;
    }
    bool automatic = auto_selected();
    if (!automatic && (!ui.ref_video || !is_file(ui.ref_video))) {
        show_info_bar(GTK_MESSAGE_ERROR, "Das Referenz-Video wurde nicht gefunden.");
        return;
    }
    if (!ui.src_dir || !is_dir(ui.src_dir)) {
        show_info_bar(GTK_MESSAGE_ERROR, "Der Quellordner wurde nicht gefunden.");
        return;
    }
    char *out = effective_out_dir();
    if (replace_originals_on() && ui.out_dir_custom) { /* Ausgabeordner wird nicht genutzt */
        char *src_norm = normpath(ui.src_dir);
        g_free(out);
        out = g_strdup_printf("%s_output", src_norm);
        free(src_norm);
    }
    char *real_out = realpath_lenient(out), *real_src = realpath_lenient(ui.src_dir);
    bool same = !strcmp(real_out, real_src);
    free(real_out);
    free(real_src);
    if (same) {
        show_info_bar(GTK_MESSAGE_ERROR, "Ausgabe- und Quellordner dürfen nicht identisch sein.");
        g_free(out);
        return;
    }
    settings_save();

    g_free(ui.run_ref_video);
    g_free(ui.run_src_dir);
    g_free(ui.run_out_dir);
    ui.run_ref_video = automatic ? NULL : g_strdup(ui.ref_video);
    ui.run_src_dir = g_strdup(ui.src_dir);
    ui.run_out_dir = out;
    ui.run_auto = automatic;
    ui.run_auto_max = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui.auto_max_spin));
    ui.run_auto_min = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.auto_min_spin));
    ui.run_length = automatic ? 0 : selected_length();     /* automatisch: je Video eigener */
    ui.run_outro = !automatic && outro_selected();
    ui.run_frames = selected_frames();
    ui.run_threshold = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.threshold_spin));
    ui.run_mode = MODE_ID[selected_mode()];
    ui.run_replace = replace_originals_on();
    ic_detect_free(&ui.detection);
    clear_auto_page();                          /* Zeilen zeigen auf das alte Ergebnis */
    ic_auto_free(&ui.auto_result);
    ic_run_free(&ui.cut_run);
    memset(&ui.cut_run, 0, sizeof ui.cut_run);

    ic_cancel = 0;
    ic_log_open();
    if (ui.gpu_ready) {
        char *gpu_info = ic_gpu_summary(ui.have_gpu ? &ui.gpu : NULL);
        ic_log(false, "  %s", gpu_info);
        free(gpu_info);
    }
    set_label_text(ui.search_title, "Suche läuft …");
    int n_frames = automatic ? 0 : ic_frames_for(ui.run_length, ui.run_frames);
    char *busy_text =
        automatic    ? g_strdup_printf("Videos werden gesucht – Intros bis %.0f s …",
                                       ui.run_auto_max)
        : n_frames > 1 ? g_strdup_printf("%d Vergleichsbilder je Video werden gelesen …", n_frames)
                     : g_strdup("Referenz-Frame wird gelesen und Videos werden gesucht …");
    set_label_text(ui.search_subtitle, busy_text);
    g_free(busy_text);
    session_inhibit(true, "Intro-Cutter sucht Intros");
    set_label_text(ui.search_detail, "");
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.search_progress), NULL);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.search_progress), 0);
    gtk_button_set_label(GTK_BUTTON(ui.search_cancel_button), "Abbrechen");
    gtk_widget_set_sensitive(ui.search_cancel_button, TRUE);
    ui.search_started = false;
    if (!ui.pulse_timer) ui.pulse_timer = g_timeout_add(120, on_pulse_tick, NULL);
    ui.busy = true;
    update_start_button();
    show_page("busy");
    ui.worker_thread = g_thread_new("erkennung", search_thread, NULL);
}

/* ---------------------------------------------------------------------------
 * Seite 4: Bearbeitung
 * ------------------------------------------------------------------------- */
static void update_process_title(void)
{
    char *t = ui.processing_paused ? g_strdup("Pausiert")
              : ui.files_total == 1 ? g_strdup("Bearbeite 1 Video …")
                               : g_strdup_printf("Bearbeite %d Videos …", ui.files_total);
    set_label_text(ui.process_title, t);
    g_free(t);
}

/* Fortschrittsbalken der Bearbeitung aktualisieren: Text nach Dateien, Länge nach der
 * erledigten Arbeit (laufende Dateien anteilig, siehe ic_process_estimate) */
static void update_process_progress(void)
{
    GString *t = g_string_new(NULL);
    g_string_append_printf(t, "%d von %d fertig", ui.files_done, ui.files_total);
    if (ui.files_failed) g_string_append_printf(t, " · %d Fehler", ui.files_failed);
    if (ui.processing_paused) g_string_append(t, " · pausiert");
    double files = ui.files_total ? (double)ui.files_done / ui.files_total : 1;
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.process_progress),
                                  fmin(1, fmax(files, ui.process_fraction)));
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.process_progress), t->str);
    g_string_free(t, TRUE);
}

/* Restdauer unter dem Fortschrittsbalken und im Fenstertitel – so steht sie auch in der
 * Taskleiste, der Stand vorn (schmale Einträge kürzen hinten). Die Kopfleiste im Fenster
 * übernimmt den Fenstertitel sonst und zeigt weiter nur "Intro-Cutter". */
static void update_process_estimate(void)
{
    IcEstimate e;
    if (!ic_process_estimate(&e)) return;
    char text[160], short_text[64];
    ic_format_estimate(&e, true, text, sizeof text);
    ic_format_estimate(&e, false, short_text, sizeof short_text);
    char *line = g_strdup_printf("Restdauer: %s", text);
    set_label_text(ui.process_estimate, line);
    g_free(line);
    if (e.fraction > ui.process_fraction) {
        ui.process_fraction = e.fraction;
        if (!ui.process_bar_moving) update_process_progress();
    }
    char *title = e.paused ? g_strdup("Pausiert – Intro-Cutter")
                  : e.remaining >= 0
                      ? g_strdup_printf("%.0f %% · noch %s – Intro-Cutter",
                                        floor(ui.process_fraction * 100), short_text)
                      : g_strdup_printf("%.0f %% – Intro-Cutter", floor(ui.process_fraction * 100));
    gtk_window_set_title(GTK_WINDOW(ui.win), title);
    gtk_header_bar_set_title(GTK_HEADER_BAR(ui.header), "Intro-Cutter");
    g_free(title);
}

/* Bearbeitung pausieren bzw. fortsetzen (Engine und Anzeige) */
static void set_processing_paused(bool on)
{
    ui.processing_paused = on;
    ic_set_paused(on);
    gtk_button_set_label(GTK_BUTTON(ui.process_pause_button), on ? "Fortsetzen" : "Pausieren");
    GtkStyleContext *ctx = gtk_widget_get_style_context(ui.process_pause_button);
    if (on)
        gtk_style_context_add_class(ctx, "suggested-action");
    else
        gtk_style_context_remove_class(ctx, "suggested-action");
    update_process_title();
    update_process_progress();
    update_process_estimate();
}

/* Knopf "Pausieren"/"Fortsetzen" */
static void on_pause_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    if (!ui.busy || ui.moving || ic_cancel) return;
    set_processing_paused(!ui.processing_paused);
    protocol_append(ui.processing_paused
                        ? "Bearbeitung pausiert – laufende Jobs sind angehalten, neue "
                          "starten erst nach „Fortsetzen“."
                        : "Bearbeitung fortgesetzt.");
}

/* Knopf "Abbrechen" während der Bearbeitung (mit Rückfrage) */
static void on_process_cancel_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    if (!ui.busy || ic_cancel) return;
    if (!ask_confirmation("Bearbeitung abbrechen?",
                          "Laufende ffmpeg-Prozesse werden beendet und halbfertige Dateien "
                          "gelöscht. Bereits fertige Dateien bleiben erhalten.",
                          "Weiterlaufen lassen", "Bearbeitung abbrechen", true))
        return;
    ic_cancel = 1;
    gtk_button_set_label(b, "Wird abgebrochen …");
    gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
    gtk_widget_set_sensitive(ui.process_pause_button, FALSE);
}

/* Farben fürs Protokoll aus dem Theme (Fehler, Warnung, OK) */
static void create_protocol_tags(void)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui.protocol_view));
    GtkStyleContext *ctx = gtk_widget_get_style_context(ui.protocol_view);
    static const char *const tag[3][3] = {{"ok", "success_color", "#26a269"},
                                          {"warn", "warning_color", "#c88800"},
                                          {"err", "error_color", "#e01b24"}};
    for (int i = 0; i < 3; i++) {
        GdkRGBA c;
        if (!gtk_style_context_lookup_color(ctx, tag[i][1], &c)) gdk_rgba_parse(&c, tag[i][2]);
        gtk_text_buffer_create_tag(buf, tag[i][0], "foreground-rgba", &c, NULL);
    }
}

/* Jobliste einer Art: Überschrift mit Auslastung, Zeilen, "keine" und "+ N weitere" */
static void job_group_init(JobGroup *group, GtkWidget *box, const char *title, int nrows)
{
    group->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(head), new_label(title, "ic-heading"), FALSE, FALSE, 0);
    group->status = new_label("", "dim-label");
    add_css_class(group->status, "ic-status");
    gtk_label_set_xalign(GTK_LABEL(group->status), 1);
    gtk_label_set_ellipsize(GTK_LABEL(group->status), PANGO_ELLIPSIZE_END);
    gtk_box_pack_end(GTK_BOX(head), group->status, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(group->box), head, FALSE, FALSE, 0);
    group->list = add_group(group->box, NULL);
    group->nrows = nrows;
    for (int i = 0; i < nrows; i++) {
        JobRow *j = &group->rows[i];
        j->bar = gtk_progress_bar_new();
        gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(j->bar), TRUE);
        gtk_widget_set_size_request(j->bar, 150, -1);       /* Breite folgt dem Fenster */
        j->row = add_row(group->list, "", "", NULL, j->bar, &j->title, &j->subtitle);
        gtk_label_set_line_wrap(GTK_LABEL(j->subtitle), FALSE);
        gtk_label_set_ellipsize(GTK_LABEL(j->subtitle), PANGO_ELLIPSIZE_END);
        gtk_widget_show_all(j->row);
        gtk_widget_hide(j->row);
        gtk_widget_set_no_show_all(j->row, TRUE);
    }
    GtkWidget *empty_title = NULL;
    group->empty = add_row(group->list, "Gerade keine Jobs", NULL, NULL, NULL, &empty_title, NULL);
    add_css_class(empty_title, "dim-label");
    gtk_widget_set_no_show_all(group->empty, TRUE);
    group->more = new_label("", "dim-label");
    add_css_class(group->more, "ic-sub");
    gtk_widget_set_margin_top(group->more, 4);
    gtk_widget_set_no_show_all(group->more, TRUE);
    gtk_box_pack_start(GTK_BOX(group->box), group->more, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(group->box, TRUE);
    gtk_widget_show_all(head);
    gtk_widget_show_all(gtk_widget_get_parent(group->list));
    gtk_widget_show(group->list);
    gtk_box_pack_start(GTK_BOX(box), group->box, FALSE, FALSE, 0);
}

/* Seite 4: Bearbeitung */
static GtkWidget *build_process_page(void)
{
    GtkWidget *scroller, *box = new_page_column(&scroller);
    ui.process_title = new_label("", "ic-hero");
    gtk_widget_set_margin_top(ui.process_title, 8);
    ui.process_subtitle = new_label("", "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(ui.process_subtitle), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(ui.process_subtitle), 70);
    ui.process_progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(ui.process_progress), TRUE);
    gtk_widget_set_margin_top(ui.process_progress, 10);
    ui.process_estimate = new_label("", "dim-label");
    gtk_widget_set_margin_top(ui.process_estimate, 6);
    gtk_box_pack_start(GTK_BOX(box), ui.process_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.process_subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.process_progress, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.process_estimate, FALSE, FALSE, 0);

    job_group_init(&ui.gpu_jobs, box, "GPU-Jobs", GPU_ROWS);
    job_group_init(&ui.cpu_jobs, box, "CPU-Jobs", CPU_ROWS);

    gtk_box_pack_start(GTK_BOX(box), new_label("Protokoll", "ic-heading"), FALSE, FALSE, 0);
    GtkWidget *frame = gtk_frame_new(NULL);
    add_css_class(frame, "ic-group");
    GtkWidget *protocol_scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(protocol_scroller), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(protocol_scroller, -1, 180);
    ui.protocol_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(ui.protocol_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(ui.protocol_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(ui.protocol_view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(ui.protocol_view), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(ui.protocol_view), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(ui.protocol_view), 8);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(ui.protocol_view), 8);
    add_css_class(ui.protocol_view, "ic-log");
    create_protocol_tags();
    gtk_container_add(GTK_CONTAINER(protocol_scroller), ui.protocol_view);
    gtk_container_add(GTK_CONTAINER(frame), protocol_scroller);
    gtk_widget_set_vexpand(frame, TRUE);                /* Protokoll füllt die Höhe */
    gtk_box_pack_start(GTK_BOX(box), frame, TRUE, TRUE, 0);

    /* unten: Danach-Auswahl, Pausieren, Abbrechen */
    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(bottom, 14);
    GtkWidget *after_label = new_label("Wenn fertig:", "dim-label");
    ui.process_after_combo = new_after_action_combo();
    ui.process_pause_button = gtk_button_new_with_label("Pausieren");
    gtk_widget_set_tooltip_text(ui.process_pause_button,
                                "Laufende Jobs anhalten, keine neuen starten");
    g_signal_connect(ui.process_pause_button, "clicked", G_CALLBACK(on_pause_clicked), NULL);
    ui.process_cancel_button = gtk_button_new_with_label("Abbrechen");
    add_css_class(ui.process_cancel_button, "destructive-action");
    g_signal_connect(ui.process_cancel_button, "clicked",
                     G_CALLBACK(on_process_cancel_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(bottom), after_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bottom), ui.process_after_combo, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bottom), ui.process_cancel_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bottom), ui.process_pause_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), bottom, FALSE, FALSE, 0);
    return scroller;
}

/* Jobs einer Art in ihre Liste eintragen */
static void fill_job_group(JobGroup *group, const IcJobInfo *jobs, int n, bool gpu, bool relevant)
{
    int shown = 0, total = 0;
    for (int i = 0; i < n; i++) {
        if (jobs[i].gpu != gpu) continue;
        total++;
        if (shown >= group->nrows) continue;
        JobRow *j = &group->rows[shown++];
        char t[64], *base = g_path_get_basename(jobs[i].rel), *name = path_for_display(base);
        char *subtitle_text = g_strdup_printf("%s · seit %s%s", jobs[i].what,
                                              format_duration(jobs[i].secs, t, sizeof t),
                                              ui.processing_paused ? " · pausiert" : "");
        char *pct_text = g_strdup_printf("%.0f %%", jobs[i].pct);
        set_label_text(j->title, name);
        set_label_text(j->subtitle, subtitle_text);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(j->bar),
                                      fmin(1.0, fmax(0.0, jobs[i].pct / 100)));
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(j->bar), pct_text);
        gtk_widget_show(j->row);
        g_free(base);
        g_free(name);
        g_free(subtitle_text);
        g_free(pct_text);
    }
    for (int i = shown; i < group->nrows; i++) gtk_widget_hide(group->rows[i].row);
    gtk_widget_set_visible(group->empty, total == 0);
    char *more = total > group->nrows ? g_strdup_printf("+ %d weitere laufen", total - group->nrows)
                                  : g_strdup("");
    set_label_text(group->more, more);
    gtk_widget_set_visible(group->more, *more != 0);
    g_free(more);
    gtk_widget_set_visible(group->box, relevant || total > 0);
}

/* laufende Jobs anzeigen (alle 0,5 s) – GPU- und CPU-Jobs getrennt */
static gboolean on_jobs_tick(gpointer d)
{
    (void)d;
    static IcJobInfo jobs[64];
    int n = ic_jobs_snapshot(jobs, 64);
    const char *m = ui.run_mode ? ui.run_mode : "";
    bool hybrid = !strcmp(m, "hybrid");
    fill_job_group(&ui.gpu_jobs, jobs, n, true, hybrid || !strcmp(m, "gpu"));
    fill_job_group(&ui.cpu_jobs, jobs, n, false, hybrid || !strcmp(m, "cpu") || !strcmp(m, "copy"));
    double load, pause, cpu;
    char *status_text = NULL;
    if (ic_gpu_load(&load, &pause))             /* GPU-Bremse (Ziel GPU_TARGET %) */
        status_text = g_strdup_printf("Auslastung %.0f %% (Ziel %d %%)", load, GPU_TARGET);
    set_label_text(ui.gpu_jobs.status, status_text ? status_text : "");
    g_free(status_text);
    status_text = ic_cpu_load(&cpu)
                      ? g_strdup_printf("Auslastung %.0f %% (Ziel %d %%)", cpu, CPU_TARGET)
                      : NULL;
    set_label_text(ui.cpu_jobs.status, status_text ? status_text : "");
    g_free(status_text);
    update_process_estimate();
    return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------------------
 * Seite 5: Fertig
 * ------------------------------------------------------------------------- */
/* qsort-Vergleich: Schnittmethoden alphabetisch */
static int compare_methods(const void *a, const void *b)
{
    return strcmp(((const MethodCount *)a)->method, ((const MethodCount *)b)->method);
}

/* Zeile "Bezeichnung … Wert" der Zusammenfassung */
static void add_stat_row(GtkWidget *list, const char *title, const char *value)
{
    GtkWidget *v = new_label(value, NULL);
    gtk_label_set_xalign(GTK_LABEL(v), 1);
    gtk_label_set_ellipsize(GTK_LABEL(v), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(v), 44);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    add_row(list, title, NULL, NULL, v, NULL, NULL);
}

/* Seite 5 mit der Zusammenfassung füllen */
static void fill_done_page(bool aborted)
{
    const Stats *stats = &ui.cut_run.stats;
    GtkStyleContext *icon_style = gtk_widget_get_style_context(ui.done_icon);
    gtk_style_context_remove_class(icon_style, "warn");
    gtk_style_context_remove_class(icon_style, "err");
    const char *icon = "emblem-ok-symbolic", *title = "Fertig";
    if (aborted) {
        icon = "process-stop-symbolic";
        title = "Abgebrochen";
        gtk_style_context_add_class(icon_style, "err");
    } else if (stats->aborted) {                    /* bei wenig Platz beendet */
        icon = "dialog-warning-symbolic";
        title = "Vorzeitig beendet";
        gtk_style_context_add_class(icon_style, "warn");
    } else if (stats->err || stats->move_err) {
        icon = "dialog-warning-symbolic";
        title = "Fertig – mit Fehlern";
        gtk_style_context_add_class(icon_style, "warn");
    }
    gtk_image_set_from_icon_name(GTK_IMAGE(ui.done_icon), icon, GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(ui.done_icon), 72);
    set_label_text(ui.done_title, title);
    char *subtitle = g_strdup_printf("%d bearbeitet · %d Fehler · %d Originale ersetzt", stats->ok,
                                     stats->err, stats->replaced);
    set_label_text(ui.done_subtitle, subtitle);
    g_free(subtitle);

    GList *old_children = gtk_container_get_children(GTK_CONTAINER(ui.done_stats));
    for (GList *item = old_children; item; item = item->next) gtk_widget_destroy(item->data);
    g_list_free(old_children);
    GtkWidget *list = add_group(ui.done_stats, NULL);
    GString *methods_text = g_string_new(NULL);
    MethodCount sorted[32];
    memcpy(sorted, stats->methods, sizeof sorted);
    qsort(sorted, (size_t)stats->method_count, sizeof *sorted, compare_methods);
    for (int i = 0; i < stats->method_count; i++)
        g_string_append_printf(methods_text, "%s%s: %d", i ? ", " : "", sorted[i].method,
                               sorted[i].files);
    add_stat_row(list, "Methoden", methods_text->len ? methods_text->str : "–");
    g_string_free(methods_text, TRUE);
    char duration_text[64], *text;
    add_stat_row(list, "Suche", format_duration(ui.run_auto ? ui.auto_result.secs
                                                            : ui.detection.secs,
                                                duration_text, sizeof duration_text));
    add_stat_row(list, "Bearbeitung",
                 format_duration(ui.cut_run.secs, duration_text, sizeof duration_text));
    format_duration(stats->total_cut, duration_text, sizeof duration_text);
    text = g_strdup_printf("%.1f s (%s)", stats->total_cut, duration_text);
    add_stat_row(list, "Insgesamt abgeschnitten", text);
    g_free(text);
    text = llabs(stats->saved_bytes) >= 1024LL * 1024 * 1024
        ? g_strdup_printf("%.1f MB (%.2f GB)", stats->saved_bytes / 1048576.0,
                          stats->saved_bytes / 1073741824.0)
        : g_strdup_printf("%.1f MB", stats->saved_bytes / 1048576.0);
    add_stat_row(list, "Eingesparte Datenmenge", text);
    g_free(text);
    bool all_moved = !ui.run_replace && stats->replaced > 0 && !ui.cut_run.fin_dest.n;
    text = ui.run_replace ? g_strdup("Originale ersetzt")
           : all_moved    ? g_strdup("An den Platz der Originale verschoben")
                          : path_for_display(ui.run_out_dir);
    add_stat_row(list, "Ergebnisse", text);
    g_free(text);
    if (stats->move_err) {
        text = g_strdup_printf("%d – Originale unverändert, siehe Lauf-Protokoll", stats->move_err);
        add_stat_row(list, "Nicht an Originalplatz verschoben", text);
        g_free(text);
    }
    gtk_widget_show_all(ui.done_stats);

    if (!strcmp(ui.run_mode, "copy") && stats->trimmed_files) {
        text = g_strdup_printf("Hinweis: Bei %d Video(s) wurde über das Intro hinaus geschnitten "
                               "(max. %.1f s), weil kein Keyframe am Intro-Ende lag. Für "
                               "framegenaue Schnitte einen der Modi „Framegenau“ wählen.",
                               stats->trimmed_files, stats->max_trim);
        set_label_text(ui.done_hint, text);
        g_free(text);
        gtk_widget_show(ui.done_hint);
    } else if (!strcmp(ui.run_mode, "copy") && stats->rest_files) {
        text = g_strdup_printf("Hinweis: Bei %d Video(s) blieb ein Rest des Intros stehen "
                               "(max. %.1f s), weil kein Keyframe genau am Intro-Ende lag – "
                               "Copy schneidet nie in den Inhalt. Für bildgenaue Schnitte einen "
                               "der Modi „Framegenau“ wählen.",
                               stats->rest_files, stats->max_rest);
        set_label_text(ui.done_hint, text);
        g_free(text);
        gtk_widget_show(ui.done_hint);
    } else {
        gtk_widget_hide(ui.done_hint);
    }
    gtk_button_set_label(GTK_BUTTON(ui.done_open_button),
                         ui.run_replace || !g_file_test(ui.run_out_dir, G_FILE_TEST_IS_DIR)
                             ? "Quellordner öffnen" : "Ausgabeordner öffnen");
    int finished_count = ui.cut_run.fin_dest.n;
    text = finished_count == 1
               ? g_strdup("1 fertige Datei an den Platz des Originals verschieben")
               : g_strdup_printf("%d fertige Dateien an den Platz der Originale verschieben",
                                 finished_count);
    gtk_button_set_label(GTK_BUTTON(ui.done_move_button), text);
    g_free(text);
    gtk_widget_set_sensitive(ui.done_move_button, TRUE);
    gtk_widget_set_visible(ui.done_move_button, !ui.run_replace && finished_count > 0);

    if (!aborted) {                                 /* wie die Terminal-Version */
        Buf methods_dict = {0};
        buf_add_char(&methods_dict, '{');
        for (int i = 0; i < stats->method_count; i++)
            buf_printf(&methods_dict, "%s'%s': %d", i ? ", " : "", stats->methods[i].method,
                       stats->methods[i].files);
        buf_add_char(&methods_dict, '}');
        ic_log(false, "Fertig: %d ok, %d Fehler, Methoden %s, Bearbeitung %s", stats->ok,
               stats->err, buf_str(&methods_dict),
               format_duration(ui.cut_run.secs, duration_text, sizeof duration_text));
        buf_free(&methods_dict);
    }
}

/* Ausgabe- bzw. Quellordner öffnen */
static void on_open_results_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    /* nach dem Verschieben an den Platz der Originale ist der Ausgabeordner evtl. weg */
    bool output_exists = !ui.run_replace && g_file_test(ui.run_out_dir, G_FILE_TEST_IS_DIR);
    open_in_default_app(output_exists ? ui.run_out_dir : ui.run_src_dir);
}

/* Lauf-Protokoll öffnen */
static void on_open_run_log_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    if (is_file(RUN_LOG)) open_in_default_app(RUN_LOG);
}

/* Trefferliste leeren */
static void clear_results(void)
{
    clear_preview_strips();
    GList *old_children = gtk_container_get_children(GTK_CONTAINER(ui.results_box));
    for (GList *item = old_children; item; item = item->next) gtk_widget_destroy(item->data);
    g_list_free(old_children);
    reset_result_lists();
}

/* Knopf "Neuer Durchlauf": zurück zu den Einstellungen */
static void on_new_run_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    clear_results();
    ic_detect_free(&ui.detection);
    clear_auto_page();
    ic_auto_free(&ui.auto_result);
    ic_run_free(&ui.cut_run);
    memset(&ui.cut_run, 0, sizeof ui.cut_run);
    show_info_bar(GTK_MESSAGE_INFO, NULL);
    show_page("setup");
}

/* im Hauptthread: Verschieben beendet – Ergebnis anzeigen */
static gboolean on_move_done(gpointer p)
{
    int moved = GPOINTER_TO_INT(p);
    g_thread_join(ui.worker_thread);
    ui.worker_thread = NULL;
    ui.busy = ui.moving = false;
    session_inhibit(false, NULL);
    if (ui.quit_when_idle) {
        quit_application();
        return G_SOURCE_REMOVE;
    }
    int left = ui.cut_run.fin_dest.n;
    gtk_widget_hide(ui.done_move_progress);
    ic_log(false, "  %d Datei(en) verschoben.", moved);
    if (ui.cut_run.dirs_removed)
        ic_log(false, "  %d leere Ausgabeordner entfernt.", ui.cut_run.dirs_removed);
    fill_done_page(false);
    const char *dirs_note = ui.cut_run.dirs_removed ? " Leere Ausgabeordner wurden entfernt." : "";
    char *t = left ? g_strdup_printf("%d Datei(en) verschoben, %d konnten nicht verschoben "
                                     "werden (Details im Lauf-Protokoll).%s", moved, left,
                                     dirs_note)
                   : g_strdup_printf("%d Datei(en) an den Platz der Originale verschoben.%s",
                                     moved, dirs_note);
    show_info_bar(left ? GTK_MESSAGE_WARNING : GTK_MESSAGE_INFO, t);
    g_free(t);
    update_start_button();
    return G_SOURCE_REMOVE;
}

/* fertige Dateien im Hintergrund an den Platz der Originale verschieben */
static gpointer move_thread(gpointer p)
{
    (void)p;
    int moved = ic_move_finished(&ui.cut_run);
    g_idle_add(on_move_done, GINT_TO_POINTER(moved));
    return NULL;
}

/* Knopf "… an den Platz der Originale verschieben" (mit Rückfrage) */
static void on_move_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    char *t = g_strdup_printf("%d fertige Dateien ersetzen ihre Originale im Quellordner. Die "
                              "Originale werden dabei gelöscht.", ui.cut_run.fin_dest.n);
    bool ok = ask_confirmation("An den Platz der Originale verschieben?", t, "Abbrechen",
                               "Verschieben", true);
    g_free(t);
    if (!ok || ui.busy) return;
    ui.busy = ui.moving = true;
    session_inhibit(true, "Intro-Cutter verschiebt fertige Dateien");
    gtk_button_set_label(b, "Wird verschoben …");
    gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.done_move_progress), 0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.done_move_progress), "Wird vorbereitet …");
    gtk_widget_show(ui.done_move_progress);
    ui.worker_thread = g_thread_new("verschieben", move_thread, NULL);
}

/* Seite 5: Fertig */
static GtkWidget *build_done_page(void)
{
    GtkWidget *scroller, *box = new_page_column(&scroller);
    ui.done_icon = gtk_image_new_from_icon_name("emblem-ok-symbolic", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(ui.done_icon), 72);
    add_css_class(ui.done_icon, "ic-bigicon");
    gtk_widget_set_margin_top(ui.done_icon, 18);
    ui.done_title = center_widget(new_label("", "ic-hero"));
    ui.done_subtitle = center_widget(new_label("", "dim-label"));
    ui.done_stats = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(ui.done_stats, 14);
    ui.done_hint = new_label("", "dim-label");
    add_css_class(ui.done_hint, "ic-sub");
    set_wrapping(ui.done_hint, 80);
    gtk_widget_set_margin_top(ui.done_hint, 8);
    gtk_widget_set_no_show_all(ui.done_hint, TRUE);
    gtk_box_pack_start(GTK_BOX(box), ui.done_icon, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.done_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.done_subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.done_stats, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.done_hint, FALSE, FALSE, 0);

    GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_halign(btns, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(btns, 22);
    GtkWidget *buttons_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(buttons_row, GTK_ALIGN_CENTER);
    ui.done_open_button = gtk_button_new_with_label("Ausgabeordner öffnen");
    g_signal_connect(ui.done_open_button, "clicked", G_CALLBACK(on_open_results_clicked), NULL);
    GtkWidget *logb = gtk_button_new_with_label("Lauf-Protokoll öffnen");
    g_signal_connect(logb, "clicked", G_CALLBACK(on_open_run_log_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(buttons_row), ui.done_open_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(buttons_row), logb, FALSE, FALSE, 0);
    ui.done_move_button = gtk_button_new_with_label("");
    add_css_class(ui.done_move_button, "destructive-action");
    gtk_widget_set_no_show_all(ui.done_move_button, TRUE);
    g_signal_connect(ui.done_move_button, "clicked", G_CALLBACK(on_move_clicked), NULL);
    ui.done_move_progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(ui.done_move_progress), TRUE);
    gtk_widget_set_size_request(ui.done_move_progress, 420, -1);
    gtk_widget_set_no_show_all(ui.done_move_progress, TRUE);
    GtkWidget *again = new_pill_button("Neuer Durchlauf", "suggested-action");
    gtk_widget_set_halign(again, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(again, 8);
    g_signal_connect(again, "clicked", G_CALLBACK(on_new_run_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(btns), buttons_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), ui.done_move_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), ui.done_move_progress, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), again, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), btns, FALSE, FALSE, 0);
    return scroller;
}

/* ---------------------------------------------------------------------------
 * Ablauf: Bearbeitung
 * ------------------------------------------------------------------------- */
typedef struct {
    const Video **v;
    int n;
} Selection;

/* ---------------------------------------------------------------------------
 * Wenn alles fertig ist: PC herunterfahren oder Intro-Cutter beenden – erst
 * nach einem Countdown, der sich abbrechen lässt
 * ------------------------------------------------------------------------- */
static GtkWidget *countdown_dialog;
static int countdown_secs_left, countdown_action;
static guint countdown_timer;

/* Text des Countdown-Dialogs aktualisieren */
static void update_countdown_text(void)
{
    char *t = countdown_action == AFTER_POWEROFF
                  ? g_strdup_printf("Die Bearbeitung ist fertig. Der PC wird in %d s "
                                    "heruntergefahren.", countdown_secs_left)
                  : g_strdup_printf("Die Bearbeitung ist fertig. Intro-Cutter wird in %d s "
                                    "beendet.", countdown_secs_left);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(countdown_dialog), "%s", t);
    g_free(t);
}

/* Countdown jede Sekunde herunterzählen; bei 0 ausführen */
static gboolean on_countdown_tick(gpointer d)
{
    (void)d;
    if (--countdown_secs_left <= 0) {
        countdown_timer = 0;
        gtk_dialog_response(GTK_DIALOG(countdown_dialog), GTK_RESPONSE_ACCEPT);
        return G_SOURCE_REMOVE;
    }
    update_countdown_text();
    return G_SOURCE_CONTINUE;
}

/* Herunterfahren über die Sitzung (Programme werden ordentlich beendet), sonst
 * direkt über systemd-logind -> false mit Grund */
static bool request_power_off(char **reason)
{
#ifdef IC_TEST_POWEROFF
    const char *mark = getenv("IC_POWEROFF_MARK");          /* nur Test-Builds */
    if (mark) return g_file_set_contents(mark, "poweroff\n", -1, NULL);
#endif
    GError *err = NULL;
    GVariant *reply = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
    if (bus) {
        reply = g_dbus_connection_call_sync(bus, "org.gnome.SessionManager",
                                            "/org/gnome/SessionManager", "org.gnome.SessionManager",
                                            "RequestShutdown", NULL, NULL, G_DBUS_CALL_FLAGS_NONE,
                                            15000, NULL, &err);
        g_object_unref(bus);
    }
    if (!reply) {
        g_clear_error(&err);
        bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &err);
        if (bus) {
            reply = g_dbus_connection_call_sync(bus, "org.freedesktop.login1",
                                                "/org/freedesktop/login1",
                                                "org.freedesktop.login1.Manager", "PowerOff",
                                                g_variant_new("(b)", TRUE), NULL,
                                                G_DBUS_CALL_FLAGS_NONE, 15000, NULL, &err);
            g_object_unref(bus);
        }
    }
    if (reply) {
        g_variant_unref(reply);
        return true;
    }
    *reason = g_strdup(err ? err->message : "unbekannter Fehler");
    g_clear_error(&err);
    return false;
}

/* Antwort im Countdown-Dialog: abbrechen oder sofort ausführen */
static void on_countdown_response(GtkDialog *dialog, gint response, gpointer d)
{
    (void)d;
    if (countdown_timer) g_source_remove(countdown_timer);
    countdown_timer = 0;
    gtk_widget_destroy(GTK_WIDGET(dialog));
    countdown_dialog = NULL;
    if (response != GTK_RESPONSE_ACCEPT) {
        show_info_bar(GTK_MESSAGE_INFO,
                      countdown_action == AFTER_POWEROFF ? "Herunterfahren abgebrochen."
                                                         : "Beenden abgebrochen.");
        return;
    }
    if (countdown_action == AFTER_QUIT) {
        quit_application();
        return;
    }
    settings_save();
    ic_log(false, "Bearbeitung fertig – PC wird heruntergefahren");
    char *reason = NULL;
    if (!request_power_off(&reason)) {
        char *t = g_strdup_printf("Herunterfahren nicht möglich: %s", reason);
        show_info_bar(GTK_MESSAGE_ERROR, t);
        g_free(t);
        g_free(reason);
    }
}

/* Countdown für die Aktion nach der Bearbeitung anzeigen */
static void start_after_countdown(int action)
{
    if (countdown_dialog) return;
    countdown_action = action;
    countdown_secs_left = action == AFTER_POWEROFF ? 60 : 15;
    const char *title = action == AFTER_POWEROFF ? "PC wird heruntergefahren"
                                                 : "Intro-Cutter wird beendet";
    countdown_dialog = gtk_message_dialog_new(GTK_WINDOW(ui.win), GTK_DIALOG_MODAL,
                                              GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s", title);
    gtk_dialog_add_button(GTK_DIALOG(countdown_dialog), "Abbrechen", GTK_RESPONSE_CANCEL);
    GtkWidget *now = gtk_dialog_add_button(GTK_DIALOG(countdown_dialog),
                                           action == AFTER_POWEROFF ? "Jetzt herunterfahren"
                                                                    : "Jetzt beenden",
                                           GTK_RESPONSE_ACCEPT);
    add_css_class(now, "destructive-action");
    gtk_dialog_set_default_response(GTK_DIALOG(countdown_dialog), GTK_RESPONSE_CANCEL);
    update_countdown_text();
    g_signal_connect(countdown_dialog, "response", G_CALLBACK(on_countdown_response), NULL);
    gtk_widget_show(countdown_dialog);
    gtk_window_present(GTK_WINDOW(ui.win));
    countdown_timer = g_timeout_add(1000, on_countdown_tick, NULL);
}

/* im Hauptthread: Bearbeitung beendet – Zusammenfassung, danach ggf. Countdown */
static gboolean on_processing_done(gpointer p)
{
    Selection *s = p;
    g_thread_join(ui.worker_thread);
    ui.worker_thread = NULL;
    if (ui.jobs_timer) g_source_remove(ui.jobs_timer);
    ui.jobs_timer = 0;
    ic_cleanup_temps();
    ui.busy = false;
    g_free(s->v);
    g_free(s);
    session_inhibit(false, NULL);
    ui.processing_paused = false;
    gtk_window_set_title(GTK_WINDOW(ui.win), "Intro-Cutter");
    if (ui.quit_when_idle) {
        quit_application();
        return G_SOURCE_REMOVE;
    }
    fill_done_page(!ui.run_completed);
    ic_cancel = 0;
    update_start_button();
    show_page("done");
    /* nur nach vollständigem Durchlauf (nicht nach Abbruch oder Stopp bei wenig Platz) */
    if (ui.run_completed && !ui.cut_run.stats.aborted && selected_after_action() != AFTER_NOTHING)
        start_after_countdown(selected_after_action());
    return G_SOURCE_REMOVE;
}

/* die Bearbeitung läuft in einem eigenen Thread */
static gpointer processing_thread(gpointer p)
{
    Selection *s = p;
    ui.run_completed = ic_process(s->v, s->n, ui.run_out_dir, ui.run_length, ui.run_outro,
                                  ui.run_mode, ui.run_replace, ui.have_gpu ? &ui.gpu : NULL,
                                  &ui.cut_run);
    g_idle_add(on_processing_done, s);
    return NULL;
}

/* Knopf "N Videos schneiden": Bearbeitung starten */
static void on_cut_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    int n = ui.run_auto ? auto_selected_count() : selected_count();
    if (!n || ui.busy) return;
    if (ui.run_replace) {
        char *t = g_strdup_printf("%d Originaldatei(en) werden durch die geschnittene Fassung "
                                  "ersetzt. Das lässt sich nicht rückgängig machen.", n);
        bool ok = ask_confirmation("Originale ersetzen?", t, "Abbrechen",
                                   "Originale ersetzen", true);
        g_free(t);
        if (!ok) return;
    }
    while (!ui.gpu_ready) gtk_main_iteration();     /* GPU-Prüfung abwarten (Sekunden) */
    g_atomic_int_inc(&ui.preview_generation);       /* Vorschaubilder nicht mehr laden */
    Selection *selection = g_new0(Selection, 1);
    selection->v = g_new(const Video *, n);
    if (ui.run_auto) {                              /* je Video der eigene Schnitt */
        for (int v = 0; v < ui.auto_result.videos.n; v++)
            if (ui.auto_selected[v]) {
                Video *video = &ui.auto_result.videos.v[v];
                video->cut_len = auto_info(v)->cut_len;
                selection->v[selection->n++] = video;
            }
    } else {
        for (int i = 0; i < ui.candidate_count; i++)    /* auch nicht angezeigte Zeilen */
            if (ui.result_selected[i]) selection->v[selection->n++] = ui.detection.results[i].v;
    }
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui.protocol_view)), "", 0);
    ui.files_done = ui.files_failed = 0;
    ui.files_total = selection->n;
    ui.process_fraction = 0;
    ui.process_bar_moving = false;
    set_label_text(ui.process_estimate, "Restdauer: wird ermittelt …");
    ui.processing_paused = false;
    gtk_button_set_label(GTK_BUTTON(ui.process_pause_button), "Pausieren");
    gtk_style_context_remove_class(gtk_widget_get_style_context(ui.process_pause_button),
                                   "suggested-action");
    gtk_widget_set_sensitive(ui.process_pause_button, TRUE);
    update_process_title();
    int mode_i = 0;
    while (mode_i < 3 && strcmp(ui.run_mode, MODE_ID[mode_i])) mode_i++;
    static const char *const mode_text[4] = {"Copy (verlustfrei)", "Framegenau per CPU",
                                             "Framegenau per GPU", "GPU + CPU parallel"};
    char *outd = path_for_display(ui.run_out_dir);
    char *what = ui.run_auto  ? g_strdup("Intros automatisch erkannt (je Video eigener Schnitt)")
               : ui.run_outro ? g_strdup_printf("Outro (letzte %g s)", ui.run_length)
                              : g_strdup_printf("Intro (erste %g s)", ui.run_length);
    const char *mode_name = mode_text[mode_i];
    char *subtitle_text =
        ui.run_replace ? g_strdup_printf("%s · %s · Originale werden ersetzt", what, mode_name)
                       : g_strdup_printf("%s · %s · Ausgabe: %s", what, mode_name, outd);
    g_free(what);
    set_label_text(ui.process_subtitle, subtitle_text);
    g_free(outd);
    g_free(subtitle_text);
    update_process_progress();
    gtk_button_set_label(GTK_BUTTON(ui.process_cancel_button), "Abbrechen");
    gtk_widget_set_sensitive(ui.process_cancel_button, TRUE);
    ic_cancel = 0;
    ui.busy = true;
    session_inhibit(true, "Intro-Cutter bearbeitet Videos");
    update_start_button();
    show_page("process");
    on_jobs_tick(NULL);
    ui.jobs_timer = g_timeout_add(500, on_jobs_tick, NULL);
    ui.worker_thread = g_thread_new("bearbeitung", processing_thread, selection);
}

/* ---------------------------------------------------------------------------
 * Hash-Cache ansehen und aufräumen: Liste der Videos mit gespeicherten
 * Vergleichsbildern, Suche, Einträge entfernen. Laden und Aufräumen laufen im
 * Hintergrund (ein großer Cache hat schnell Hunderte MB).
 * ------------------------------------------------------------------------- */
enum { CACHE_COL_NAME, CACHE_COL_FOLDER, CACHE_COL_SIZE_TEXT, CACHE_COL_FRAMES, CACHE_COL_KIND,
       CACHE_COL_PATH, CACHE_COL_SIZE, CACHE_COL_SEARCH, CACHE_COLS };

enum { CACHE_LOAD, CACHE_REMOVE_SELECTED, CACHE_REMOVE_STALE, CACHE_CLEAR };

typedef struct {
    GtkWidget *dialog, *summary, *search, *view, *spinner;
    GtkWidget *remove_button, *stale_button, *clear_button;
    GtkListStore *store;
    GtkTreeModel *filter;
    char *search_text;              /* kleingeschrieben; NULL = alle anzeigen */
    bool working;                   /* Laden bzw. Aufräumen läuft im Hintergrund */
} CacheWindow;

static CacheWindow cache_window;    /* höchstens ein Cache-Fenster */

/* Auftrag für den Hintergrund-Thread und sein Ergebnis */
typedef struct {
    int kind;                       /* CACHE_LOAD, CACHE_REMOVE_… oder CACHE_CLEAR */
    char **paths;                   /* CACHE_REMOVE_SELECTED: Pfade (NULL-terminiert) */
    long removed;                   /* entfernte Einträge, -1 = Fehler */
    bool cleared;
    IcCacheInfo info;               /* Stand nach dem Auftrag */
    bool have_info;
} CacheJob;

/* Zeile "Hash-Cache" der Einstellungsseite: Größe der Cache-Dateien (Vergleichsbilder der
 * Suche mit Referenz-Video, Kennbilder der automatischen Erkennung) */
static void update_cache_row(void)
{
    if (!ui.cache_subtitle || !APP_DIR) return;
    struct stat st;
    char hash_text[32] = "", kb_text[32] = "", *text;
    if (HASH_CACHE && stat(HASH_CACHE, &st) == 0)
        format_bytes((double)st.st_size, hash_text, sizeof hash_text);
    char *kb_path = g_build_filename(APP_DIR, "kennbilder.bin", NULL);
    if (stat(kb_path, &st) == 0 && st.st_size > 0)
        format_bytes((double)st.st_size, kb_text, sizeof kb_text);
    g_free(kb_path);
    if (*hash_text && *kb_text)
        text = g_strdup_printf("Gespeicherte Vergleichsbilder (%s) und Kennbilder der "
                               "automatischen Erkennung (%s) früherer Suchen – spart das erneute "
                               "Einlesen. Hier ansehen oder aufräumen.", hash_text, kb_text);
    else if (*hash_text || *kb_text)
        text = g_strdup_printf("Gespeicherte %s früherer Suchen (%s) – spart das erneute "
                               "Einlesen. Hier ansehen oder aufräumen.",
                               *hash_text ? "Vergleichsbilder"
                                          : "Kennbilder der automatischen Erkennung",
                               *hash_text ? hash_text : kb_text);
    else
        text = g_strdup("Noch leer – die erste Suche liest alle Videos ein und füllt ihn.");
    set_label_text(ui.cache_subtitle, text);
    g_free(text);
}

/* Knöpfe des Cache-Fensters: nur ohne laufenden Auftrag und ohne laufende Suche bzw.
 * Bearbeitung (die Engine schreibt den Cache sonst mit ihrem Stand zurück) */
static void cache_window_update_buttons(void)
{
    bool idle = !cache_window.working && !ui.busy;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(cache_window.view));
    gtk_widget_set_sensitive(cache_window.remove_button,
                             idle && gtk_tree_selection_count_selected_rows(selection) > 0);
    gtk_widget_set_sensitive(cache_window.stale_button, idle);
    gtk_widget_set_sensitive(cache_window.clear_button, idle);
    if (cache_window.working)
        gtk_spinner_start(GTK_SPINNER(cache_window.spinner));
    else
        gtk_spinner_stop(GTK_SPINNER(cache_window.spinner));
}

/* Liste und Zusammenfassung mit dem Stand info füllen */
static void cache_window_fill(const IcCacheInfo *info, bool exists, const char *note)
{
    GtkWidget *view = cache_window.view;
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(view));
    g_object_ref(model);
    gtk_tree_view_set_model(GTK_TREE_VIEW(view), NULL);    /* schneller füllen */
    gtk_list_store_clear(cache_window.store);
    long frames = 0;
    for (int i = 0; i < info->n; i++) {
        const IcCacheVideo *v = &info->videos[i];
        char *base = g_path_get_basename(v->path), *dir = g_path_get_dirname(v->path);
        char *name = path_for_display(base), *folder = path_for_display(dir);
        char *search = g_utf8_casefold(folder, -1), *search_name = g_utf8_casefold(name, -1);
        char *haystack = g_strconcat(search, "/", search_name, NULL);
        char size_text[32];
        format_bytes((double)v->size, size_text, sizeof size_text);
        int intro_frames = v->frames - v->outro_frames;
        const char *kind = !v->outro_frames ? "Intro" : !intro_frames ? "Outro" : "Intro + Outro";
        gtk_list_store_insert_with_values(cache_window.store, NULL, -1, CACHE_COL_NAME, name,
                                          CACHE_COL_FOLDER, folder, CACHE_COL_SIZE_TEXT, size_text,
                                          CACHE_COL_FRAMES, v->frames, CACHE_COL_KIND, kind,
                                          CACHE_COL_PATH, v->path, CACHE_COL_SIZE, v->size,
                                          CACHE_COL_SEARCH, haystack, -1);
        frames += v->frames;
        g_free(base);
        g_free(dir);
        g_free(name);
        g_free(folder);
        g_free(search);
        g_free(search_name);
        g_free(haystack);
    }
    gtk_tree_view_set_model(GTK_TREE_VIEW(view), model);
    g_object_unref(model);
    GString *text = g_string_new(NULL);
    if (!exists) {
        g_string_append(text, "Der Cache ist leer – die nächste Suche liest alle Videos ein.");
    } else if (!info->hash_file) {
        g_string_append(text, "Noch keine Vergleichsbilder der Suche mit Referenz-Video.");
    } else {
        char size_text[32], date_text[64] = "";
        format_bytes((double)info->file_bytes, size_text, sizeof size_text);
        GDateTime *when = g_date_time_new_from_unix_local((gint64)info->modified);
        if (when) {
            char *formatted = g_date_time_format(when, "%d.%m.%Y %H:%M");
            snprintf(date_text, sizeof date_text, "%s", formatted);
            g_free(formatted);
            g_date_time_unref(when);
        }
        g_string_append_printf(text, "%d Videos · %ld gespeicherte Vergleichsbilder · %s · "
                               "zuletzt geändert %s", info->n, frames, size_text, date_text);
        if (info->other)
            g_string_append_printf(text, " · %ld Einträge in anderem Format", info->other);
    }
    if (exists && info->kb_bytes > 0) {
        char kb_text[32];
        format_bytes((double)info->kb_bytes, kb_text, sizeof kb_text);
        g_string_append_printf(text, "\nAutomatische Erkennung: Kennbilder von %d Videos · %s "
                               "(werden mit entfernt bzw. gelöscht)", info->kb_videos, kb_text);
    }
    if (exists)
        g_string_append(text, "\nDer Cache spart bei weiteren Suchen das erneute Einlesen der "
                        "Videos; die Python-Version nutzt die Vergleichsbilder mit. Einträge zu "
                        "entfernen ist ungefährlich – sie werden bei Bedarf neu berechnet.");
    if (ui.busy)
        g_string_append(text, "\nWährend einer Suche oder Bearbeitung lässt er sich nur "
                        "ansehen.");
    if (note) g_string_append_printf(text, "\n%s", note);
    set_label_text(cache_window.summary, text->str);
    g_string_free(text, TRUE);
}

static gboolean on_cache_job_done(gpointer p);

/* im Hintergrund: Auftrag ausführen, den neuen Stand lesen und an den Hauptthread geben */
static gpointer cache_job_thread(gpointer p)
{
    CacheJob *job = p;
    if (job->kind == CACHE_REMOVE_SELECTED)
        job->removed = ic_cache_remove_videos((const char *const *)job->paths,
                                              (int)g_strv_length(job->paths));
    else if (job->kind == CACHE_REMOVE_STALE)
        job->removed = ic_cache_remove_stale();
    else if (job->kind == CACHE_CLEAR)
        job->cleared = ic_cache_clear();
    job->have_info = ic_cache_read(&job->info);
    g_idle_add(on_cache_job_done, job);
    return NULL;
}

/* im Hauptthread: Auftrag fertig – Liste und Meldung aktualisieren */
static gboolean on_cache_job_done(gpointer p)
{
    CacheJob *job = p;
    update_cache_row();
    if (cache_window.dialog) {
        char *note = NULL;
        if (job->kind == CACHE_CLEAR)
            note = g_strdup(job->cleared ? "Der Cache wurde gelöscht."
                                         : "Der Cache ließ sich nicht löschen.");
        else if (job->kind != CACHE_LOAD && job->removed < 0)
            note = g_strdup("Der Cache ließ sich nicht speichern.");
        else if (job->kind != CACHE_LOAD)
            note = job->removed == 1 ? g_strdup("1 Eintrag entfernt.")
                                     : g_strdup_printf("%ld Einträge entfernt.", job->removed);
        cache_window_fill(&job->info, job->have_info, note);
        g_free(note);
        cache_window.working = false;
        cache_window_update_buttons();
    }
    ic_cache_info_free(&job->info);
    g_strfreev(job->paths);
    g_free(job);
    return G_SOURCE_REMOVE;
}

/* Auftrag im Hintergrund starten (übernimmt paths) */
static void cache_start_job(int kind, char **paths)
{
    CacheJob *job = g_new0(CacheJob, 1);
    job->kind = kind;
    job->paths = paths;
    cache_window.working = true;
    cache_window_update_buttons();
    g_thread_unref(g_thread_new("cache", cache_job_thread, job));
}

/* Knopf "Ausgewählte entfernen" */
static void on_cache_remove_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    GtkTreeModel *model;
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(cache_window.view));
    GList *rows = gtk_tree_selection_get_selected_rows(selection, &model);
    GPtrArray *paths = g_ptr_array_new();
    for (GList *r = rows; r; r = r->next) {
        GtkTreeIter iter;
        char *path = NULL;
        if (gtk_tree_model_get_iter(model, &iter, r->data))
            gtk_tree_model_get(model, &iter, CACHE_COL_PATH, &path, -1);
        if (path) g_ptr_array_add(paths, path);
    }
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    g_ptr_array_add(paths, NULL);
    cache_start_job(CACHE_REMOVE_SELECTED, (char **)g_ptr_array_free(paths, FALSE));
}

/* Knopf "Veraltete entfernen": Einträge für gelöschte oder geänderte Videos */
static void on_cache_stale_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    cache_start_job(CACHE_REMOVE_STALE, NULL);
}

/* Knopf "Alles löschen" (mit Rückfrage) */
static void on_cache_clear_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    if (!ask_confirmation("Hash-Cache löschen?",
                          "Alle gespeicherten Vergleichsbilder und die Kennbilder der "
                          "automatischen Erkennung werden gelöscht. Die nächste Suche liest dann "
                          "alle Videos neu ein und dauert entsprechend länger.",
                          "Abbrechen", "Cache löschen", true))
        return;
    cache_start_job(CACHE_CLEAR, NULL);
}

/* Knopf "Datenordner öffnen" */
static void on_cache_folder_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    open_in_default_app(APP_DIR);
}

/* Auswahl in der Liste geändert */
static void on_cache_selection_changed(GtkTreeSelection *selection, gpointer d)
{
    (void)selection;
    (void)d;
    cache_window_update_buttons();
}

/* Suchfeld: Liste auf passende Videos (Name oder Ordner) einschränken */
static void on_cache_search_changed(GtkSearchEntry *entry, gpointer d)
{
    (void)d;
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
    g_free(cache_window.search_text);
    cache_window.search_text = *text ? g_utf8_casefold(text, -1) : NULL;
    gtk_tree_model_filter_refilter(GTK_TREE_MODEL_FILTER(cache_window.filter));
}

/* Filter der Liste: Zeile passt zum Suchtext? */
static gboolean cache_row_visible(GtkTreeModel *model, GtkTreeIter *iter, gpointer d)
{
    (void)d;
    if (!cache_window.search_text) return TRUE;
    char *haystack = NULL;
    gtk_tree_model_get(model, iter, CACHE_COL_SEARCH, &haystack, -1);
    gboolean visible = haystack && strstr(haystack, cache_window.search_text);
    g_free(haystack);
    return visible;
}

/* Fenster geschlossen: Zustand zurücksetzen (laufende Aufträge melden sich ins Leere) */
static void on_cache_window_destroy(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
    g_free(cache_window.search_text);
    g_object_unref(cache_window.store);
    memset(&cache_window, 0, sizeof cache_window);
}

/* Spalte der Liste anlegen (sortierbar) */
static void cache_add_column(GtkWidget *view, const char *title, int column, int sort_column,
                             bool expand, float xalign)
{
    GtkCellRenderer *cell = gtk_cell_renderer_text_new();
    g_object_set(cell, "xalign", xalign, NULL);
    if (expand) g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);
    GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(title, cell, "text", column,
                                                                      NULL);
    gtk_tree_view_column_set_sort_column_id(col, sort_column);
    gtk_tree_view_column_set_resizable(col, TRUE);
    gtk_tree_view_column_set_expand(col, expand);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
}

/* Cache-Fenster öffnen (bzw. nach vorn holen) und den Cache im Hintergrund lesen */
static void open_cache_window(void)
{
    if (cache_window.dialog) {
        gtk_window_present(GTK_WINDOW(cache_window.dialog));
        return;
    }
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Hash-Cache", GTK_WINDOW(ui.win),
                                                    GTK_DIALOG_DESTROY_WITH_PARENT,
                                                    "Schließen", GTK_RESPONSE_CLOSE, NULL);
    cache_window.dialog = dialog;
    gtk_window_set_default_size(GTK_WINDOW(dialog), 860, 560);
    g_signal_connect(dialog, "response", G_CALLBACK(gtk_widget_destroy), NULL);
    g_signal_connect(dialog, "destroy", G_CALLBACK(on_cache_window_destroy), NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_box_set_spacing(GTK_BOX(content), 10);
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    cache_window.summary = set_wrapping(new_label("Cache wird gelesen …", "dim-label"), 90);
    gtk_widget_set_hexpand(cache_window.summary, TRUE);
    cache_window.spinner = gtk_spinner_new();
    gtk_box_pack_start(GTK_BOX(top), cache_window.summary, TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(top), cache_window.spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), top, FALSE, FALSE, 0);

    cache_window.search = gtk_search_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(cache_window.search), "Video oder Ordner suchen");
    g_signal_connect(cache_window.search, "search-changed", G_CALLBACK(on_cache_search_changed),
                     NULL);
    gtk_box_pack_start(GTK_BOX(content), cache_window.search, FALSE, FALSE, 0);

    cache_window.store = gtk_list_store_new(CACHE_COLS, G_TYPE_STRING, G_TYPE_STRING,
                                            G_TYPE_STRING, G_TYPE_INT, G_TYPE_STRING,
                                            G_TYPE_STRING, G_TYPE_INT64, G_TYPE_STRING);
    cache_window.filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(cache_window.store), NULL);
    gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(cache_window.filter),
                                           cache_row_visible, NULL, NULL);
    GtkTreeModel *sorted = gtk_tree_model_sort_new_with_model(cache_window.filter);
    g_object_unref(cache_window.filter);       /* gehört jetzt dem sortierten Modell */
    cache_window.view = gtk_tree_view_new_with_model(sorted);
    g_object_unref(sorted);
    cache_add_column(cache_window.view, "Video", CACHE_COL_NAME, CACHE_COL_NAME, true, 0);
    cache_add_column(cache_window.view, "Ordner", CACHE_COL_FOLDER, CACHE_COL_FOLDER, true, 0);
    cache_add_column(cache_window.view, "Größe", CACHE_COL_SIZE_TEXT, CACHE_COL_SIZE, false, 1);
    cache_add_column(cache_window.view, "Bilder", CACHE_COL_FRAMES, CACHE_COL_FRAMES, false, 1);
    cache_add_column(cache_window.view, "Art", CACHE_COL_KIND, CACHE_COL_KIND, false, 0);
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(cache_window.view));
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
    g_signal_connect(selection, "changed", G_CALLBACK(on_cache_selection_changed), NULL);
    GtkWidget *scroller = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroller), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(scroller), cache_window.view);
    gtk_box_pack_start(GTK_BOX(content), scroller, TRUE, TRUE, 0);

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *folder_button = gtk_button_new_with_label("Datenordner öffnen");
    g_signal_connect(folder_button, "clicked", G_CALLBACK(on_cache_folder_clicked), NULL);
    cache_window.remove_button = gtk_button_new_with_label("Ausgewählte entfernen");
    gtk_widget_set_tooltip_text(cache_window.remove_button,
                                "Gespeicherte Bilder der markierten Videos entfernen "
                                "(Mehrfachauswahl mit Strg bzw. Umschalt)");
    g_signal_connect(cache_window.remove_button, "clicked", G_CALLBACK(on_cache_remove_clicked),
                     NULL);
    cache_window.stale_button = gtk_button_new_with_label("Veraltete entfernen");
    gtk_widget_set_tooltip_text(cache_window.stale_button,
                                "Einträge für Videos entfernen, die es nicht mehr gibt oder die "
                                "sich seitdem geändert haben (z. B. geschnittene Originale)");
    g_signal_connect(cache_window.stale_button, "clicked", G_CALLBACK(on_cache_stale_clicked),
                     NULL);
    cache_window.clear_button = gtk_button_new_with_label("Alles löschen");
    add_css_class(cache_window.clear_button, "destructive-action");
    g_signal_connect(cache_window.clear_button, "clicked", G_CALLBACK(on_cache_clear_clicked),
                     NULL);
    gtk_box_pack_start(GTK_BOX(buttons), folder_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), cache_window.clear_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), cache_window.stale_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(buttons), cache_window.remove_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), buttons, FALSE, FALSE, 0);

    gtk_widget_show_all(dialog);
    cache_start_job(CACHE_LOAD, NULL);
}

/* Knopf "Anzeigen …" der Einstellungsseite bzw. Menü "Hash-Cache …" */
static void on_cache_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    open_cache_window();
}

/* Menü "Hash-Cache …" */
static void on_action_cache(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    open_cache_window();
}

/* ---------------------------------------------------------------------------
 * Fenster, Menü, Programmstart
 * ------------------------------------------------------------------------- */
/* Widget samt allen Kindern neu vermessen lassen */
static void queue_resize_tree(GtkWidget *widget, gpointer data)
{
    gtk_widget_queue_resize(widget);
    if (GTK_IS_CONTAINER(widget))
        gtk_container_forall(GTK_CONTAINER(widget), queue_resize_tree, data);
}

/* Seite zeigen. Vorher wird sie samt Fenster vollständig neu vermessen: GTK 3 verteilt beim
 * Seitenwechsel sonst nur teilweise neu und verliert dabei Größenänderungen, die währenddessen
 * entstehen (z. B. von der Bildlaufleiste einer Seite, die sich im Hintergrund geändert hat) –
 * danach bliebe das Layout stehen, eingeblendete Bereiche erschienen nicht mehr. */
static void show_page(const char *name)
{
    static const char *const page_subtitles[][2] = {{"setup", "Einstellungen"}, {"busy", "Suche"},
                                         {"results", "Treffer"}, {"auto", "Treffer"},
                                         {"process", "Bearbeitung"}, {"done", "Fertig"}};
    for (size_t i = 0; i < G_N_ELEMENTS(page_subtitles); i++)
        if (!strcmp(name, page_subtitles[i][0]))
            gtk_header_bar_set_subtitle(GTK_HEADER_BAR(ui.header), page_subtitles[i][1]);
    GtkWidget *page = gtk_stack_get_child_by_name(GTK_STACK(ui.stack), name);
    if (page) queue_resize_tree(page, NULL);
    gtk_widget_queue_resize(ui.win);
    gtk_stack_set_visible_child_name(GTK_STACK(ui.stack), name);
    gtk_widget_set_visible(ui.back_btn, !strcmp(name, "results") || !strcmp(name, "auto"));
}

/* Hinweise der Engine (z. B. I/O-Limit, Verschiebe-Fehler): Protokollfenster + Lauf-Log */
static void frontend_info_gui(const char *msg)
{
    ic_log(false, "%s", msg);
    frontend_log(msg);
}

/* Layout an die Fensterbreite anpassen: Ränder, Vorschaubild, Fortschrittsbalken */
static gboolean on_idle_layout(gpointer d)
{
    (void)d;
    ui.layout_idle_id = 0;
    if (!ui.win) return G_SOURCE_REMOVE;
    int w = gtk_widget_get_allocated_width(ui.win);
    if (w <= 1 || w == ui.layout_width) return G_SOURCE_REMOVE;
    ui.layout_width = w;
    int margin = page_margin(w);
    for (int i = 0; i < ui.page_column_count; i++) {
        gtk_widget_set_margin_start(ui.page_columns[i], margin);
        gtk_widget_set_margin_end(ui.page_columns[i], margin);
    }
    update_ref_preview();
    gtk_widget_set_size_request(ui.search_box, CLAMP(w * 55 / 100, 380, 1000), -1);
    int bar_w = CLAMP(w * 25 / 100, 150, 600);
    JobGroup *groups[2] = {&ui.gpu_jobs, &ui.cpu_jobs};
    for (int group_i = 0; group_i < 2; group_i++)
        for (int i = 0; i < groups[group_i]->nrows; i++)
            gtk_widget_set_size_request(groups[group_i]->rows[i].bar, bar_w, -1);
    return G_SOURCE_REMOVE;
}

/* Fenstergröße geändert: Layout anpassen (gesammelt im Leerlauf) */
static void on_window_size_allocate(GtkWidget *w, GdkRectangle *a, gpointer d)
{
    (void)w;
    (void)d;
    if (a->width != ui.layout_width && !ui.layout_idle_id)
        ui.layout_idle_id = g_idle_add(on_idle_layout, NULL);
}

/* Fenstergröße und -zustand merken (beim nächsten Start wieder so) */
static gboolean on_window_configure(GtkWidget *w, GdkEventConfigure *e, gpointer d)
{
    (void)e;
    (void)d;
    if (!ui.window_maximized) {
        int width, height;
        gtk_window_get_size(GTK_WINDOW(w), &width, &height);
        if (width != ui.window_width || height != ui.window_height) {
            ui.window_width = width;
            ui.window_height = height;
            settings_changed();
        }
    }
    return FALSE;
}

/* Fenster maximiert bzw. wiederhergestellt: merken */
static gboolean on_window_state(GtkWidget *w, GdkEventWindowState *e, gpointer d)
{
    (void)w;
    (void)d;
    bool max = (e->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
    if (max != ui.window_maximized) {
        ui.window_maximized = max;
        settings_changed();
    }
    return FALSE;
}

/* Fenster schließen: während der Arbeit erst nachfragen */
static gboolean on_window_delete(GtkWidget *w, GdkEvent *e, gpointer d)
{
    (void)w;
    (void)e;
    (void)d;
    if (!ui.busy) {
        settings_save();
        ic_cleanup_temps();
        return FALSE;
    }
    if (ui.quit_when_idle) return TRUE;
    if (ui.moving) {
        show_info_bar(GTK_MESSAGE_INFO, "Bitte warten, bis das Verschieben abgeschlossen ist.");
        return TRUE;
    }
    if (!ask_confirmation("Die Bearbeitung läuft noch",
                          "Beim Beenden werden laufende ffmpeg-Prozesse abgebrochen und "
                          "halbfertige Dateien gelöscht. Bereits fertige Dateien bleiben erhalten.",
                          "Weiterlaufen lassen", "Abbrechen und beenden", true))
        return TRUE;
    ui.quit_when_idle = true;
    ic_cancel = 1;
    show_info_bar(GTK_MESSAGE_INFO, "Wird abgebrochen …");
    return TRUE;
}

/* SIGINT/SIGTERM/SIGHUP (z. B. beim Abmelden): geordnet aufräumen */
static gboolean on_unix_signal(gpointer d)
{
    (void)d;
    if (!ui.win) return G_SOURCE_CONTINUE;
    if (ui.busy) {
        ui.quit_when_idle = true;
        ic_cancel = 1;
    } else {
        quit_application();
    }
    return G_SOURCE_CONTINUE;
}

/* Meldungsleiste schließen */
static void on_info_bar_response(GtkInfoBar *info_bar, gint response, gpointer d)
{
    (void)response;
    (void)d;
    gtk_info_bar_set_revealed(info_bar, FALSE);
}

/* Menü "Lauf-Protokoll öffnen" */
static void on_action_open_log(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    if (is_file(RUN_LOG))
        open_in_default_app(RUN_LOG);
    else
        show_info_bar(GTK_MESSAGE_INFO, "Es gibt noch kein Lauf-Protokoll.");
}

/* Menü "Datenordner öffnen" */
static void on_action_open_data(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    g_mkdir_with_parents(APP_DIR, 0755);
    open_in_default_app(APP_DIR);
}

/* Menü "Über Intro-Cutter" */
static void on_action_about(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    gtk_show_about_dialog(GTK_WINDOW(ui.win), "program-name", "Intro-Cutter", "version",
                          IC_VERSION, "logo-icon-name", "intro-cutter", "comments",
                          "Erkennt ein wiederkehrendes Intro oder Outro in vielen Videos und "
                          "schneidet es heraus.\nEngine: C-Portierung der Python-Version "
                          "(+ Outro-Modus).\n"
                          "Hash-Cache und Einstellungen: ~/.intro-cutter",
                          NULL);
}

/* Menü bzw. Strg+Q: Programm beenden */
static void on_action_quit(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    if (ui.win) gtk_window_close(GTK_WINDOW(ui.win));
}

/* Programmstart: Zahlenformat, Menüaktionen, Tastenkürzel */
static void on_startup(GApplication *app, gpointer d)
{
    (void)d;
    /* GTK stellt das Zahlenformat auf die Landessprache um – die Engine braucht
     * aber Punkte (ffmpeg-Argumente, Cache-Schlüssel, Python-kompatible Werte) */
    setlocale(LC_NUMERIC, "C");
    static const GActionEntry actions[] = {
        {.name = "open-log", .activate = on_action_open_log},
        {.name = "open-data", .activate = on_action_open_data},
        {.name = "cache", .activate = on_action_cache},
        {.name = "about", .activate = on_action_about},
        {.name = "quit", .activate = on_action_quit},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), actions, G_N_ELEMENTS(actions), NULL);
    const char *quit_accels[] = {"<Control>q", NULL};
    gtk_application_set_accels_for_action(GTK_APPLICATION(app), "app.quit", quit_accels);
}

/* Menüknopf in der Titelleiste */
static GtkWidget *build_menu_button(void)
{
    GMenu *menu = g_menu_new(), *about = g_menu_new();
    g_menu_append(menu, "Lauf-Protokoll öffnen", "app.open-log");
    g_menu_append(menu, "Datenordner öffnen", "app.open-data");
    g_menu_append(menu, "Hash-Cache …", "app.cache");
    g_menu_append(about, "Über Intro-Cutter", "app.about");
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

/* Hauptfenster aufbauen (bei einem zweiten Start nur nach vorn holen) */
static void on_activate(GtkApplication *app, gpointer d)
{
    (void)d;
    if (ui.win) {                                   /* zweiter Start: vorhandenes Fenster zeigen */
        gtk_window_present(GTK_WINDOW(ui.win));
        return;
    }
    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css_provider, CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(css_provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css_provider);
    ui.result_checks = g_ptr_array_new();
    ui.preview_strips = g_ptr_array_new_with_free_func(preview_strip_free);
    ui.preview_pool = g_thread_pool_new(preview_worker, NULL, THUMB_THREADS, FALSE, NULL);

    ui.win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(ui.win), "Intro-Cutter");
    gtk_window_set_default_size(GTK_WINDOW(ui.win), 780, 760);
    gtk_window_set_icon_name(GTK_WINDOW(ui.win), "intro-cutter");
    ui.header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(ui.header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(ui.header), "Intro-Cutter");
    gtk_window_set_titlebar(GTK_WINDOW(ui.win), ui.header);
    ui.back_btn = gtk_button_new_from_icon_name("go-previous-symbolic", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(ui.back_btn, "Zurück zu den Einstellungen");
    g_signal_connect(ui.back_btn, "clicked", G_CALLBACK(on_back_clicked), NULL);
    gtk_header_bar_pack_start(GTK_HEADER_BAR(ui.header), ui.back_btn);
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
    gtk_stack_add_named(GTK_STACK(ui.stack), build_setup_page(), "setup");
    gtk_stack_add_named(GTK_STACK(ui.stack), build_search_page(), "busy");
    gtk_stack_add_named(GTK_STACK(ui.stack), build_results_page(), "results");
    gtk_stack_add_named(GTK_STACK(ui.stack), build_auto_page(), "auto");
    gtk_stack_add_named(GTK_STACK(ui.stack), build_process_page(), "process");
    gtk_stack_add_named(GTK_STACK(ui.stack), build_done_page(), "done");
    gtk_box_pack_start(GTK_BOX(vbox), ui.stack, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(ui.win), vbox);
    g_signal_connect(ui.win, "delete-event", G_CALLBACK(on_window_delete), NULL);

    ui.ffmpeg_ok = ic_have_ffmpeg();
    settings_load();
    if (ui.window_width > 0 && ui.window_height > 0) /* Fenster wie beim letzten Mal */
        gtk_window_set_default_size(GTK_WINDOW(ui.win), ui.window_width, ui.window_height);
    if (ui.window_maximized) gtk_window_maximize(GTK_WINDOW(ui.win));
    g_signal_connect(ui.win, "configure-event", G_CALLBACK(on_window_configure), NULL);
    g_signal_connect(ui.win, "window-state-event", G_CALLBACK(on_window_state), NULL);
    g_signal_connect(ui.win, "size-allocate", G_CALLBACK(on_window_size_allocate), NULL);
    update_setup_labels();
    update_cache_row();
    gtk_widget_show_all(ui.win);
    gtk_widget_hide(ui.infobar);
    show_page("setup");
    if (!ui.ffmpeg_ok)
        show_info_bar(GTK_MESSAGE_ERROR, "ffmpeg ist nicht installiert – bitte über die "
                      "Anwendungsverwaltung oder mit „sudo apt install ffmpeg“ nachinstallieren.");
    if (ui.ref_video) schedule_ref_preview();
    ui.gpu_thread = g_thread_new("gpu", gpu_check_thread, NULL);
}

/* Einstieg: Engine einrichten, Callbacks setzen, GTK-Anwendung starten */
int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    ic_init();
    ic_fe.log = frontend_log;
    ic_fe.info = frontend_info_gui;
    ic_fe.detect_start = frontend_search_start;
    ic_fe.detect_progress = frontend_search_progress;
    ic_fe.detect_stage = frontend_search_stage;
    ic_fe.file_done = frontend_file_done;
    ic_fe.ask_disk_move = frontend_ask_disk_move;
    ic_fe.ask_disk_continue = frontend_ask_disk_continue;
    ic_fe.move_progress = frontend_move_progress;
    g_mutex_init(&ui.ask_mutex);
    g_cond_init(&ui.ask_cond);
    g_set_prgname("intro-cutter");
    g_set_application_name("Intro-Cutter");
    ui.app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(ui.app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(ui.app, "activate", G_CALLBACK(on_activate), NULL);
    g_unix_signal_add(SIGINT, on_unix_signal, NULL);
    g_unix_signal_add(SIGTERM, on_unix_signal, NULL);
    g_unix_signal_add(SIGHUP, on_unix_signal, NULL);
    int rc = g_application_run(G_APPLICATION(ui.app), argc, argv);
    ic_cleanup_temps();
    g_object_unref(ui.app);
    return rc;
}
