/*
 * AV1-Converter – grafische Oberfläche (GTK 3)
 * ============================================
 * Aufbau und Optik wie beim Intro-Cutter. Ablauf in vier Schritten:
 * Einstellungen -> Liste der Videos -> Umwandlung -> Fertig.
 * Quell- und Ausgabeordner lassen sich auch per Drag & Drop aus dem
 * Dateimanager setzen.
 * Die Engine (ac_core.c) läuft in einem eigenen Thread; Meldungen und
 * Fortschritt gelangen über g_idle_add() in den GTK-Hauptthread.
 * Der gesamte Zustand der Oberfläche liegt in der Struktur "ui" (Typ UiState).
 *
 * Aufbau dieser Datei: kleine Helfer – Engine -> Oberfläche (Callbacks) –
 * Einstellungen (Zustand, HandBrake-/GPU-Prüfung, Speichern) – Seite 1
 * Einstellungen – Drag & Drop – Seite 2 Suche – Seite 3 Videos – Seite 4
 * Umwandlung – Seite 5 Fertig – Ablauf – Aktion nach dem Ende (Herunterfahren
 * oder Beenden) – Fenster, Menü, Programmstart
 *
 * Aufruf:
 *     av1-converter
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

#include "ac_core.h"

#define APP_ID "de.av1converter.AV1Converter"
#define GPU_ROWS MAX_GPU_JOBS   /* so viele laufende Jobs werden je Gruppe einzeln angezeigt */
#define CPU_ROWS MAX_CPU_JOBS
#define RESULT_ROWS_BATCH 200   /* Zeilen der Videoliste auf einmal (weitere per Knopf) */
#define PROTOCOL_MAX_LINES 5000 /* Protokoll im Fenster; ältere Zeilen nur im Lauf-Protokoll */
#define SETTINGS_GROUP "Einstellungen"

static const char *const MODE_ID[3] = {"cpu", "gpu", "hybrid"};
static const char *const AUDIO_TEXT[3] = {"Wie im Profil: erste Tonspur → Opus 160 kbit/s Stereo",
                                          "Alle Tonspuren → Opus 160 kbit/s Stereo",
                                          "Alle Tonspuren unverändert übernehmen (verlustfrei)"};
static const char *const AUDIO_ID[3] = {"profil", "alle", "kopie"};
static const char *const SUBS_TEXT[2] = {"Wie im Profil: keine übernehmen",
                                         "Alle Untertitel übernehmen"};
static const char *const SUBS_ID[2] = {"profil", "alle"};
static const char *const HDR_TEXT[3] = {"Per GPU – HDR-Angaben in den MKV-Container",
                                        "Vollständig per HandBrake (langsam)",
                                        "Per GPU ohne Helligkeitsangaben"};
static const char *const HDR_ID[3] = {"gpu-container", "handbrake", "gpu-ohne"};
/* Was passiert, wenn die Umwandlung fertig ist */
enum { AFTER_NOTHING, AFTER_POWEROFF, AFTER_QUIT };
static const char *const AFTER_ID[3] = {"nichts", "herunterfahren", "beenden"};
static const char *const AFTER_TEXT[3] = {"Nichts tun", "PC herunterfahren",
                                          "AV1-Converter beenden"};

enum { DROP_SRC, DROP_OUT };                /* Ziele für Drag & Drop */

typedef struct {
    GtkWidget *row, *title, *subtitle, *bar;
} JobRow;

typedef struct {                /* Jobliste einer Art (GPU oder CPU) */
    GtkWidget *box, *status, *list, *empty, *empty_title, *more;
    JobRow rows[CPU_ROWS];
    int nrows;
} JobGroup;

typedef struct {
    GtkApplication *app;
    GtkWidget *win, *header, *stack, *back_btn, *infobar, *info_label;

    /* Einstellungen */
    char *src_dir, *out_dir_custom;         /* out_dir_custom NULL = <Quelle>_av1 */
    GtkWidget *src_row, *src_subtitle, *out_row, *out_subtitle, *out_button, *replace_switch;
    GtkWidget *mode_buttons[3], *mode_rows[3], *mode_subtitles[3], *detect_spinner;
    GtkWidget *cpu_jobs_spin, *gpu_jobs_spin, *rf_spin, *gpu_quality_spin;
    GtkWidget *audio_combo, *subs_combo, *min_saving_spin, *mtime_switch, *hdr_combo, *hdr_subtitle;
    GtkWidget *done_list_subtitle, *after_combo, *search_button, *setup_hint;
    guint save_timer;
    bool loading_settings, rf_from_settings;

    /* Fenster: Größe merken, Layout an die Breite anpassen */
    int window_width, window_height, layout_width;
    bool window_maximized;
    guint layout_idle_id;
    GtkWidget *page_columns[6];
    int page_column_count;

    /* HandBrake und GPU (Prüfung im Hintergrund beim Start) */
    AcHandbrake hb;
    AcGpu gpu;
    bool hb_ready, gpu_ready, ffmpeg_ok;

    /* laufender Durchlauf */
    char *run_src_dir, *run_out_dir;
    AcOptions run_opts;
    AcScan scan;
    AcStats stats;
    bool run_completed;
    guint inhibit_cookie;                   /* Standby/Abmelden gesperrt */
    GThread *worker_thread;
    bool busy, quit_when_idle;
    bool moving;                            /* Verschieben an den Platz der Originale läuft */
    char *move_return_page;                 /* Seite, zu der es danach zurückgeht */

    /* Seite "Suche" */
    GtkWidget *search_title, *search_subtitle, *search_progress, *search_cancel_button;
    guint pulse_timer;
    bool search_started;

    /* Seite "Videos" */
    GtkWidget *results_title, *results_subtitle, *skip_box, *results_list, *more_row;
    GtkWidget *selection_label, *start_button;
    int *todo;                              /* Index in ui.scan.v je umzuwandelndem Video */
    int todo_count, rows_shown;
    bool *selected;                         /* je Eintrag von todo */
    GPtrArray *checks;                      /* Häkchen der angelegten Zeilen */
    bool bulk_selecting;

    /* Seite "Umwandlung" */
    GtkWidget *process_title, *process_subtitle, *process_progress, *process_eta, *process_detail;
    GtkWidget *protocol_view, *process_cancel_button, *process_pause_button, *process_after_combo;
    JobGroup gpu_jobs, cpu_jobs;
    guint jobs_timer;
    int files_failed;
    bool processing_paused;

    /* Seite "Fertig" */
    GtkWidget *done_icon, *done_title, *done_subtitle, *done_stats, *done_hint, *done_open_button;
    GtkWidget *done_move_button;

    /* Verschieben an den Platz der Originale (Seite "move" und Zeile in den Einstellungen) */
    GtkWidget *pending_row, *pending_subtitle;
    GtkWidget *move_title, *move_subtitle, *move_progress, *move_cancel_button;
} UiState;

static UiState ui;

static void update_start_button(void);
static void show_page(const char *name);
static void update_done_list_row(void);
static void update_pending_rows(void);
static void on_move_clicked(GtkButton *b, gpointer d);

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

/* Pfad im Standard-Programm öffnen (Ordner im Dateimanager, Datei im Editor) */
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

/* umrandete Liste mit Überschrift (Einstellungsgruppe) */
static GtkWidget *add_group(GtkWidget *box, const char *title)
{
    if (title) gtk_box_pack_start(GTK_BOX(box), new_label(title, "ac-heading"), FALSE, FALSE, 0);
    GtkWidget *frame = gtk_frame_new(NULL);
    add_css_class(frame, "ac-group");
    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_list_box_set_header_func(GTK_LIST_BOX(list), list_separator_header, NULL, NULL);
    gtk_container_add(GTK_CONTAINER(frame), list);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    return list;
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
    GtkWidget *t = new_label(title, "ac-title");
    gtk_label_set_ellipsize(GTK_LABEL(t), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(t), 50);
    gtk_box_pack_start(GTK_BOX(v), t, FALSE, FALSE, 0);
    GtkWidget *s = NULL;
    if (subtitle) {
        s = new_label(subtitle, "ac-sub");
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

/* runder Hauptknopf (z. B. "Videos suchen") */
static GtkWidget *new_pill_button(const char *text, const char *cls)
{
    GtkWidget *b = gtk_button_new_with_label(text);
    add_css_class(b, "ac-pill");
    if (cls) add_css_class(b, cls);
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

static const char CSS[] =
    ".ac-heading { font-weight: bold; margin-top: 18px; margin-bottom: 4px; }\n"
    "frame.ac-group > border { border-radius: 10px; background-color: @theme_base_color; }\n"
    "frame.ac-group list { background-color: transparent; }\n"
    "frame.ac-group list > row:first-child { border-top-left-radius: 10px;"
    " border-top-right-radius: 10px; }\n"
    "frame.ac-group list > row:last-child { border-bottom-left-radius: 10px;"
    " border-bottom-right-radius: 10px; }\n"
    ".ac-sub { font-size: 90%; }\n"
    ".ac-hero { font-size: 170%; font-weight: 800; }\n"
    ".ac-lead { font-size: 105%; }\n"
    "frame.ac-group list > row:drop(active) { box-shadow: inset 0 0 0 2px"
    " @theme_selected_bg_color; background-color: alpha(@theme_selected_bg_color, 0.14); }\n"
    ".ac-status { margin-top: 18px; margin-bottom: 4px; font-size: 90%; }\n"
    ".ac-pill { border-radius: 999px; padding: 8px 30px; font-weight: bold; }\n"
    ".ac-badge { border-radius: 999px; padding: 1px 9px; font-size: 85%; font-weight: bold; }\n"
    ".ac-badge.ok { color: @success_color; background-color: alpha(@success_color, 0.16); }\n"
    ".ac-badge.warn { color: @warning_color; background-color: alpha(@warning_color, 0.16); }\n"
    ".ac-size { font-weight: bold; }\n"
    ".ac-eta { font-size: 125%; font-weight: bold; margin-top: 8px; }\n"
    ".ac-log { font-family: monospace; font-size: 90%; }\n"
    ".ac-bigicon { color: @success_color; }\n"
    ".ac-bigicon.warn { color: @warning_color; }\n"
    ".ac-bigicon.err { color: @error_color; }\n";

/* ---------------------------------------------------------------------------
 * Engine -> Oberfläche (die Callbacks laufen in Worker-Threads)
 * ------------------------------------------------------------------------- */
/* Zeile ans Protokoll anhängen (Fehler rot, Hinweise gelb, OK grün) */
static void protocol_append(const char *text)
{
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui.protocol_view));
    const char *tag = g_str_has_prefix(text, "FEHLER") ? "err"
                    : g_str_has_prefix(text, "GPU-FEHLER") || g_str_has_prefix(text, "KEIN GEWINN") ||
                              strstr(text, "Wenig Speicherplatz") ? "warn"
                    : g_str_has_prefix(text, "OK ") ? "ok" : NULL;
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buf, &end);
    if (gtk_text_buffer_get_char_count(buf)) gtk_text_buffer_insert(buf, &end, "\n", 1);
    if (tag)
        gtk_text_buffer_insert_with_tags_by_name(buf, &end, text, -1, tag, NULL);
    else
        gtk_text_buffer_insert(buf, &end, text, -1);
    /* bei sehr vielen Videos nur die letzten PROTOCOL_MAX_LINES Zeilen behalten */
    int lines = gtk_text_buffer_get_line_count(buf);
    if (lines > PROTOCOL_MAX_LINES + 500) {
        GtkTextIter start, cut;
        gtk_text_buffer_get_start_iter(buf, &start);
        gtk_text_buffer_get_iter_at_line(buf, &cut, lines - PROTOCOL_MAX_LINES);
        gtk_text_buffer_delete(buf, &start, &cut);
        gtk_text_buffer_get_start_iter(buf, &start);
        gtk_text_buffer_insert(buf, &start, "… ältere Zeilen stehen im Lauf-Protokoll (Menü)\n", -1);
    }
    GtkTextMark *mark = gtk_text_buffer_get_insert(buf);
    gtk_text_buffer_get_end_iter(buf, &end);
    gtk_text_buffer_place_cursor(buf, &end);
    gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(ui.protocol_view), mark);
}

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

typedef struct {
    int a, b;
    bool ok;
} EngineMsg;

static gboolean on_idle_scan_progress(gpointer p)
{
    EngineMsg *m = p;
    if (ui.win && m->b < 0) {                   /* Ordner werden noch durchlaufen */
        char *t = g_strdup_printf("%d Videos gefunden – Ordner werden gelesen …", m->a);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.search_progress), t);
        g_free(t);
    } else if (ui.win) {
        ui.search_started = m->a > 0;
        char *t = g_strdup_printf("%d von %d Videos geprüft", m->b, m->a);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.search_progress),
                                      m->a ? (double)m->b / m->a : 0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.search_progress), t);
        g_free(t);
    }
    g_free(m);
    return G_SOURCE_REMOVE;
}

/* Callback der Engine: Suchfortschritt (gefunden / geprüft) */
static void frontend_scan_progress(int found, int probed)
{
    EngineMsg *m = g_new0(EngineMsg, 1);
    *m = (EngineMsg){found, probed, true};
    g_idle_add(on_idle_scan_progress, m);
}

static gboolean on_idle_file_done(gpointer p)
{
    EngineMsg *m = p;
    if (ui.win && !m->ok) ui.files_failed++;
    g_free(m);
    return G_SOURCE_REMOVE;
}

/* Callback der Engine: eine Datei ist fertig */
static void frontend_file_done(int done, int total, bool ok)
{
    EngineMsg *m = g_new0(EngineMsg, 1);
    *m = (EngineMsg){done, total, ok};
    g_idle_add(on_idle_file_done, m);
}

/* ---------------------------------------------------------------------------
 * Einstellungen: Zustand
 * ------------------------------------------------------------------------- */
static int selected_mode(void)
{
    for (int i = 0; i < 3; i++)
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.mode_buttons[i]))) return i;
    return AC_MODE_HYBRID;
}

static bool replace_originals_on(void)
{
    return gtk_switch_get_active(GTK_SWITCH(ui.replace_switch));
}

static int selected_after_action(void)
{
    int a = gtk_combo_box_get_active(GTK_COMBO_BOX(ui.after_combo));
    return a < 0 ? AFTER_NOTHING : a;
}

/* Ausgabeordner: gewählt oder <Quelle>_av1 */
static char *effective_out_dir(void)
{
    if (ui.out_dir_custom) return g_strdup(ui.out_dir_custom);
    if (!ui.src_dir) return NULL;
    char *src = g_canonicalize_filename(ui.src_dir, NULL);
    char *out = g_strconcat(src, "_av1", NULL);
    g_free(src);
    return out;
}

/* Einstellungen der Oberfläche -> Optionen der Engine */
static void options_from_ui(AcOptions *o)
{
    ac_options_default(o);
    o->mode = (AcMode)selected_mode();
    o->cpu_jobs = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.cpu_jobs_spin));
    o->gpu_jobs = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.gpu_jobs_spin));
    o->rf = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui.rf_spin));
    o->gpu_quality = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.gpu_quality_spin));
    o->audio = MAX(0, gtk_combo_box_get_active(GTK_COMBO_BOX(ui.audio_combo)));
    o->subs = MAX(0, gtk_combo_box_get_active(GTK_COMBO_BOX(ui.subs_combo)));
    o->min_saving = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(ui.min_saving_spin));
    o->replace = replace_originals_on();
    o->keep_mtime = gtk_switch_get_active(GTK_SWITCH(ui.mtime_switch));
    o->hdr = MAX(0, gtk_combo_box_get_active(GTK_COMBO_BOX(ui.hdr_combo)));
}

/* Texte der Einstellungsseite an den Zustand anpassen */
static void update_setup_labels(void)
{
    char *src = ui.src_dir ? path_for_display(ui.src_dir)
                           : g_strdup("Noch nicht gewählt – oder Ordner hierher ziehen");
    set_label_text(ui.src_subtitle, src);
    g_free(src);
    bool replace = replace_originals_on();
    char *out = effective_out_dir(), *out_text;
    if (replace) out_text = g_strdup("Nicht benötigt – die Originale werden ersetzt");
    else if (!out) out_text = g_strdup("Wird nach Wahl des Quellordners vorgeschlagen");
    else {
        char *d = path_for_display(out);
        out_text = ui.out_dir_custom ? d : g_strdup_printf("%s (Vorschlag)", d);
        if (!ui.out_dir_custom) g_free(d);
    }
    set_label_text(ui.out_subtitle, out_text);
    g_free(out_text);
    g_free(out);
    gtk_widget_set_sensitive(ui.out_row, !replace);
    gtk_widget_set_sensitive(ui.out_button, !replace);
    int mode = selected_mode();
    gtk_widget_set_sensitive(ui.gpu_jobs_spin, mode != AC_MODE_CPU);
    gtk_widget_set_sensitive(ui.gpu_quality_spin, mode != AC_MODE_CPU);
    gtk_widget_set_sensitive(ui.cpu_jobs_spin, mode != AC_MODE_GPU);
    update_start_button();
}

/* Hauptknopf nur aktiv, wenn alles beisammen ist; Hinweis daneben */
static void update_start_button(void)
{
    const char *hint = !ui.src_dir ? "Quellordner wählen"
                       : !ui.hb_ready || !ui.gpu_ready ? "HandBrake und GPU werden geprüft …"
                       : "";
    if (ui.setup_hint) set_label_text(ui.setup_hint, hint);
    if (ui.search_button)
        gtk_widget_set_sensitive(ui.search_button, ui.src_dir && !ui.busy && ui.ffmpeg_ok);
}

/* ---------------------------------------------------------------------------
 * HandBrake und GPU beim Start prüfen (im Hintergrund, dauert Sekunden)
 * ------------------------------------------------------------------------- */
static void update_mode_rows(void)
{
    char *t;
    if (!ui.hb_ready) {
        t = g_strdup("HandBrake wird gesucht …");
    } else if (ui.hb.available) {
        t = g_strdup_printf("HandBrake %s%s mit dem Profil „" AC_PRESET "“ (SVT-AV1 10 Bit, "
                            "Preset %s, RF %g). Kleinste Dateien, aber sehr langsam: 1080p ~21, "
                            "4K ~8 Bilder/s.", ui.hb.version, ui.hb.flatpak ? " (Flatpak)" : "",
                            ui.hb.svt_preset, ui.hb.rf);
    } else {
        t = g_strdup("HandBrake nicht gefunden – ffmpeg mit SVT-AV1 und denselben Werten "
                     "(empfohlen: HandBrake über die Anwendungsverwaltung installieren).");
    }
    set_label_text(ui.mode_subtitles[0], t);
    g_free(t);
    if (!ui.gpu_ready) {
        t = g_strdup("GPU wird geprüft …");
    } else if (ui.gpu.available) {
        t = g_strdup_printf("%s per VAAPI – gleiche Einstellungen, 25- bis 40-mal so schnell "
                            "(1080p bis ~850, 4K ~100–180 Bilder/s); bei gleicher Bildqualität "
                            "25–40 %% größere Dateien als per CPU. HDR-Videos je nach Einstellung "
                            "„HDR-Informationen erhalten“ per HandBrake. "
                            "Scheitert ein Video, übernimmt HandBrake.", ui.gpu.name);
    } else {
        t = g_strdup("Keine Grafikkarte mit AV1-Encoder (VAAPI) gefunden.");
    }
    set_label_text(ui.mode_subtitles[1], t);
    g_free(t);
    bool gpu_ok = ui.gpu_ready && ui.gpu.available;
    gtk_widget_set_sensitive(ui.mode_rows[1], gpu_ok);
    gtk_widget_set_sensitive(ui.mode_rows[2], gpu_ok);
    if (ui.gpu_ready && !gpu_ok && selected_mode() != AC_MODE_CPU)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.mode_buttons[0]), TRUE);
    if (ui.hb_ready && ui.gpu_ready) gtk_widget_hide(ui.detect_spinner);
    update_start_button();
}

static gboolean on_hb_done(gpointer p)
{
    (void)p;
    ui.hb_ready = true;
    if (ui.win) {
        /* RF des Profils übernehmen, solange der Nutzer keine eigene gespeichert hat */
        if (ui.hb.available && !ui.rf_from_settings) {
            ui.loading_settings = true;
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.rf_spin), ui.hb.rf);
            ui.loading_settings = false;
        }
        update_mode_rows();
    }
    return G_SOURCE_REMOVE;
}

static gpointer hb_check_thread(gpointer p)
{
    (void)p;
    ac_handbrake_detect(&ui.hb);
    g_idle_add(on_hb_done, NULL);
    return NULL;
}

static gboolean on_gpu_done(gpointer p)
{
    (void)p;
    ui.gpu_ready = true;
    if (ui.win) update_mode_rows();
    return G_SOURCE_REMOVE;
}

static gpointer gpu_check_thread(gpointer p)
{
    (void)p;
    ac_gpu_detect(&ui.gpu);
    g_idle_add(on_gpu_done, NULL);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * Einstellungen speichern (~/.av1-converter/gui.ini, bei jeder Änderung)
 * ------------------------------------------------------------------------- */
static char *settings_path(void)
{
    return g_build_filename(AC_APP_DIR, "gui.ini", NULL);
}

/* Index eines Textes in einer Liste (oder fallback) */
static int index_of(const char *s, const char *const *ids, int n, int fallback)
{
    for (int i = 0; s && i < n; i++)
        if (!strcmp(s, ids[i])) return i;
    return fallback;
}

static void settings_load(void)
{
    GKeyFile *kf = g_key_file_new();
    char *path = settings_path(), *s;
    ui.loading_settings = true;
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        const char *G = SETTINGS_GROUP;
        s = g_key_file_get_string(kf, G, "modus", NULL);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.mode_buttons[index_of(s, MODE_ID, 3, 1)]),
                                     TRUE);
        g_free(s);
        if (g_key_file_has_key(kf, G, "cpu_jobs", NULL))
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.cpu_jobs_spin),
                                      g_key_file_get_integer(kf, G, "cpu_jobs", NULL));
        if (g_key_file_has_key(kf, G, "gpu_jobs", NULL))
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.gpu_jobs_spin),
                                      g_key_file_get_integer(kf, G, "gpu_jobs", NULL));
        if (g_key_file_has_key(kf, G, "rf", NULL)) {
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.rf_spin),
                                      g_key_file_get_double(kf, G, "rf", NULL));
            ui.rf_from_settings = true;
        }
        if (g_key_file_has_key(kf, G, "gpu_qualitaet", NULL))
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.gpu_quality_spin),
                                      g_key_file_get_integer(kf, G, "gpu_qualitaet", NULL));
        s = g_key_file_get_string(kf, G, "ton", NULL);
        gtk_combo_box_set_active(GTK_COMBO_BOX(ui.audio_combo), index_of(s, AUDIO_ID, 3, 0));
        g_free(s);
        s = g_key_file_get_string(kf, G, "untertitel", NULL);
        gtk_combo_box_set_active(GTK_COMBO_BOX(ui.subs_combo), index_of(s, SUBS_ID, 2, 0));
        g_free(s);
        if (g_key_file_has_key(kf, G, "mindestersparnis", NULL))
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui.min_saving_spin),
                                      g_key_file_get_integer(kf, G, "mindestersparnis", NULL));
        s = g_key_file_get_string(kf, G, "hdr", NULL);
        gtk_combo_box_set_active(GTK_COMBO_BOX(ui.hdr_combo), index_of(s, HDR_ID, 3, 0));
        g_free(s);
        if (g_key_file_has_key(kf, G, "datum_behalten", NULL))
            gtk_switch_set_active(GTK_SWITCH(ui.mtime_switch),
                                  g_key_file_get_boolean(kf, G, "datum_behalten", NULL));
        gtk_switch_set_active(GTK_SWITCH(ui.replace_switch),
                              g_key_file_get_boolean(kf, G, "ersetzen", NULL));
        s = g_key_file_get_string(kf, G, "danach", NULL);
        gtk_combo_box_set_active(GTK_COMBO_BOX(ui.after_combo), index_of(s, AFTER_ID, 3, 0));
        g_free(s);
        /* Ordner merken: ein Lauf über viele TB geht oft über mehrere Tage */
        s = g_key_file_get_string(kf, G, "quellordner", NULL);
        if (s && *s && g_file_test(s, G_FILE_TEST_IS_DIR)) ui.src_dir = g_strdup(s);
        g_free(s);
        s = g_key_file_get_string(kf, G, "ausgabeordner", NULL);
        if (s && *s) ui.out_dir_custom = g_strdup(s);
        g_free(s);
        ui.window_width = g_key_file_get_integer(kf, G, "fenster_breite", NULL);
        ui.window_height = g_key_file_get_integer(kf, G, "fenster_hoehe", NULL);
        ui.window_maximized = g_key_file_get_boolean(kf, G, "fenster_maximiert", NULL);
    }
    g_free(path);
    g_key_file_unref(kf);
    ui.loading_settings = false;
}

static void settings_save(void)
{
    GKeyFile *kf = g_key_file_new();
    const char *G = SETTINGS_GROUP;
    char *path = settings_path();
    AcOptions o;
    options_from_ui(&o);
    g_key_file_set_string(kf, G, "modus", MODE_ID[o.mode]);
    g_key_file_set_integer(kf, G, "cpu_jobs", o.cpu_jobs);
    g_key_file_set_integer(kf, G, "gpu_jobs", o.gpu_jobs);
    if (ui.rf_from_settings || !ui.hb.available || fabs(o.rf - ui.hb.rf) > 0.01)
        g_key_file_set_double(kf, G, "rf", o.rf);
    g_key_file_set_integer(kf, G, "gpu_qualitaet", o.gpu_quality);
    g_key_file_set_string(kf, G, "ton", AUDIO_ID[o.audio]);
    g_key_file_set_string(kf, G, "untertitel", SUBS_ID[o.subs]);
    g_key_file_set_integer(kf, G, "mindestersparnis", o.min_saving);
    g_key_file_set_boolean(kf, G, "datum_behalten", o.keep_mtime);
    g_key_file_set_string(kf, G, "hdr", HDR_ID[o.hdr]);
    g_key_file_set_boolean(kf, G, "ersetzen", o.replace);
    g_key_file_set_string(kf, G, "danach", AFTER_ID[selected_after_action()]);
    g_key_file_set_string(kf, G, "quellordner", ui.src_dir ? ui.src_dir : "");
    g_key_file_set_string(kf, G, "ausgabeordner", ui.out_dir_custom ? ui.out_dir_custom : "");
    if (ui.window_width > 0 && ui.window_height > 0) {
        g_key_file_set_integer(kf, G, "fenster_breite", ui.window_width);
        g_key_file_set_integer(kf, G, "fenster_hoehe", ui.window_height);
    }
    g_key_file_set_boolean(kf, G, "fenster_maximiert", ui.window_maximized);
    g_mkdir_with_parents(AC_APP_DIR, 0755);
    g_key_file_save_to_file(kf, path, NULL);
    g_free(path);
    g_key_file_unref(kf);
}

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
static char *choose_folder(const char *title, const char *current)
{
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new(
        title, GTK_WINDOW(ui.win), GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, "Auswählen", "Abbrechen");
    if (current && g_file_test(current, G_FILE_TEST_IS_DIR))
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), current);
    char *result = NULL;
    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
        result = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    g_object_unref(dialog);
    return result;
}

static void on_choose_src(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    char *f = choose_folder("Quellordner mit den Videos", ui.src_dir);
    if (!f) return;
    g_free(ui.src_dir);
    ui.src_dir = f;
    update_setup_labels();
    settings_changed();
}

static void on_choose_out(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    char *current = effective_out_dir();
    char *f = choose_folder("Ausgabeordner", current && g_file_test(current, G_FILE_TEST_IS_DIR)
                                                 ? current : ui.src_dir);
    g_free(current);
    if (!f) return;
    g_free(ui.out_dir_custom);
    ui.out_dir_custom = f;
    update_setup_labels();
    settings_changed();
}

/* ---------------------------------------------------------------------------
 * Drag & Drop aus dem Dateimanager
 * ------------------------------------------------------------------------- */
/* Eintrag einer Drop-Liste (URI oder Pfad) -> lokaler Pfad, NULL wenn keiner */
static char *dropped_item_path(const char *item)
{
    char *t = g_strstrip(g_strdup(item)), *path = NULL;
    if (*t == '/') {
        path = g_strdup(t);
    } else if (!strncmp(t, "~/", 2)) {
        path = g_build_filename(g_get_home_dir(), t + 2, NULL);
    } else if (*t != '#' && g_uri_peek_scheme(t)) {
        GFile *f = g_file_new_for_uri(t);
        path = g_file_get_path(f);
        g_object_unref(f);
    }
    g_free(t);
    return path;
}

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

/* Datei oder Ordner wurde auf eine Zeile gezogen (eine Datei zählt als ihr Ordner) */
static void on_drop(GtkWidget *w, GdkDragContext *ctx, gint x, gint y, GtkSelectionData *selection,
                    guint info, guint time, gpointer data)
{
    (void)w;
    (void)ctx;
    (void)x;
    (void)y;
    (void)info;
    (void)time;
    char *path = first_dropped_path(selection);
    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS)) {
        show_info_bar(GTK_MESSAGE_WARNING, "Bitte einen Ordner aus dem Dateimanager ziehen – nur "
                      "lokale bzw. eingehängte Pfade sind möglich.");
        g_free(path);
        return;
    }
    char *dir = g_file_test(path, G_FILE_TEST_IS_DIR) ? g_strdup(path) : g_path_get_dirname(path);
    char **slot = GPOINTER_TO_INT(data) == DROP_SRC ? &ui.src_dir : &ui.out_dir_custom;
    g_free(*slot);
    *slot = dir;
    g_free(path);
    show_info_bar(GTK_MESSAGE_INFO, NULL);
    update_setup_labels();
    settings_changed();
}

static void make_drop_target(GtkWidget *w, int target, const char *tooltip)
{
    gtk_drag_dest_set(w, GTK_DEST_DEFAULT_ALL, NULL, 0, GDK_ACTION_COPY | GDK_ACTION_LINK);
    gtk_drag_dest_add_uri_targets(w);
    gtk_target_list_add_text_targets(gtk_drag_dest_get_target_list(w), 1);
    g_signal_connect(w, "drag-data-received", G_CALLBACK(on_drop), GINT_TO_POINTER(target));
    gtk_widget_set_tooltip_text(w, tooltip);
}

/* Einstellung mit Einfluss auf die Texte geändert */
static void on_setup_changed(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
    update_setup_labels();
    settings_changed();
}

/* Einstellung ohne Einfluss auf die Texte geändert: nur speichern */
static void on_setting_changed(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
    settings_changed();
}

static void on_rf_changed(GtkWidget *w, gpointer d)
{
    (void)w;
    (void)d;
    if (!ui.loading_settings) ui.rf_from_settings = true;
    settings_changed();
}

/* "Wenn fertig" gibt es auf der Einstellungs- und der Umwandlungsseite */
static void on_after_changed(GtkComboBox *c, gpointer d)
{
    (void)d;
    int a = gtk_combo_box_get_active(c);
    GtkWidget *other = GTK_WIDGET(c) == ui.after_combo ? ui.process_after_combo : ui.after_combo;
    if (other && gtk_combo_box_get_active(GTK_COMBO_BOX(other)) != a)
        gtk_combo_box_set_active(GTK_COMBO_BOX(other), a);
    settings_changed();
}

static GtkWidget *new_combo(const char *const *texts, int n)
{
    GtkWidget *c = gtk_combo_box_text_new();
    for (int i = 0; i < n; i++) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(c), NULL, texts[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(c), 0);
    return c;
}

static GtkWidget *new_after_action_combo(void)
{
    GtkWidget *c = new_combo(AFTER_TEXT, 3);
    g_signal_connect(c, "changed", G_CALLBACK(on_after_changed), NULL);
    return c;
}

/* Klick auf eine Zeile der Hardware-Auswahl wählt sie aus */
static void on_mode_row_activated(GtkListBox *list, GtkListBoxRow *r, gpointer d)
{
    (void)list;
    (void)d;
    for (int i = 0; i < 3; i++)
        if (GTK_WIDGET(r) == ui.mode_rows[i] && gtk_widget_get_sensitive(ui.mode_rows[i]))
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.mode_buttons[i]), TRUE);
}

/* Zeile "Bearbeitete Dateien": Anzahl der gemerkten Dateien */
static void update_done_list_row(void)
{
    int n = ac_done_count();
    char *t = n ? g_strdup_printf("%d Datei(en) gemerkt – sie werden bei der nächsten Suche "
                                  "übersprungen (auch nach einem Abbruch geht es dort weiter).", n)
                : g_strdup("Noch keine – bearbeitete Dateien werden gemerkt, damit ein neuer Lauf "
                           "dort weitermacht, wo der letzte aufgehört hat.");
    set_label_text(ui.done_list_subtitle, t);
    g_free(t);
}

/* Liste der bearbeiteten Dateien zurücksetzen (Auswahl: nur Fehler/ohne Gewinn oder alles) */
static void reset_done_list(void)
{
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(ui.win), GTK_DIALOG_MODAL,
                                          GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
                                          "Liste der bearbeiteten Dateien zurücksetzen?");
    gtk_message_dialog_format_secondary_text(
        GTK_MESSAGE_DIALOG(d), "Vergessene Dateien werden bei der nächsten Suche wieder geprüft "
        "und – falls nötig – erneut umgewandelt. Bereits nach AV1 umgewandelte Dateien werden "
        "ohnehin übersprungen.");
    gtk_dialog_add_button(GTK_DIALOG(d), "Abbrechen", GTK_RESPONSE_CANCEL);
    gtk_dialog_add_button(GTK_DIALOG(d), "Nur Fehler und „ohne Gewinn“", 1);
    GtkWidget *all = gtk_dialog_add_button(GTK_DIALOG(d), "Alles vergessen", 2);
    add_css_class(all, "destructive-action");
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    int r = gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (r == 1 || r == 2) {
        if (!ac_done_clear(r == 1)) show_info_bar(GTK_MESSAGE_ERROR, "Die Liste konnte nicht "
                                                  "geändert werden.");
        update_done_list_row();
    }
}

static void on_reset_done_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    if (!ui.busy) reset_done_list();
}

/* Erklärung zur HDR-Auswahl (hängt davon ab, ob mkvtoolnix installiert ist) */
static void update_hdr_subtitle(void)
{
    static const char *const text[3] = {
        "Schnell: Die GPU behält Farbraum und Kennlinie (BT.2020, PQ/HLG); die "
        "Helligkeitsangaben (Mastering Display, MaxCLL/MaxFALL) liest das Programm aus dem "
        "Original und schreibt sie per mkvpropedit in den MKV-Container. Videos mit Dolby Vision "
        "oder HDR10+ gehen an HandBrake – nur das überträgt deren dynamische Daten.",
        "Alle HDR-Videos wandelt HandBrake um – alles bleibt erhalten, auch Dolby Vision und "
        "HDR10+. Dauert lange: 4K ~8 Bilder/s, ein 2-Stunden-Film ~6 h.",
        "Schnell: auch HDR-Videos per GPU. Sie bleiben HDR (BT.2020, PQ/HLG), aber die "
        "Helligkeitsangaben fehlen; Fernseher und Player nutzen dann Standardwerte fürs "
        "Tone-Mapping."};
    int i = MAX(0, gtk_combo_box_get_active(GTK_COMBO_BOX(ui.hdr_combo)));
    char *t = i == AC_HDR_GPU_CONTAINER && !ac_have_mkvpropedit()
                  ? g_strdup("mkvtoolnix ist nicht installiert – HDR-Videos gehen deshalb an "
                             "HandBrake (vollständig, aber langsam). Installieren: "
                             "sudo apt install mkvtoolnix")
                  : g_strdup(text[i]);
    set_label_text(ui.hdr_subtitle, t);
    g_free(t);
}

static void on_hdr_changed(GtkComboBox *c, gpointer d)
{
    (void)c;
    (void)d;
    update_hdr_subtitle();
    settings_changed();
}

static void on_search_clicked(GtkButton *b, gpointer d);

static GtkWidget *build_setup_page(void)
{
    GtkWidget *scroller, *box = new_page_column(&scroller), *list, *button, *sub;
    GtkWidget *lead = new_label("Wandelt alle Videos eines Ordners (inkl. Unterordner) nach AV1 um "
                                "– mit den Einstellungen des HandBrake-Profils „" AC_PRESET "“. "
                                "Ziel: deutlich weniger Speicherplatz bei gleicher Bildqualität.",
                                "ac-lead");
    set_wrapping(lead, 80);
    add_css_class(lead, "dim-label");
    gtk_box_pack_start(GTK_BOX(box), lead, FALSE, FALSE, 6);

    list = add_group(box, "Videos");
    button = gtk_button_new_with_label("Auswählen …");
    g_signal_connect(button, "clicked", G_CALLBACK(on_choose_src), NULL);
    ui.src_row = add_row(list, "Quellordner", "", NULL, button, NULL, &ui.src_subtitle);
    make_drop_target(ui.src_row, DROP_SRC, "Ordner auswählen oder aus dem Dateimanager hierher "
                     "ziehen (eine gezogene Datei zählt als ihr Ordner)");
    gtk_label_set_ellipsize(GTK_LABEL(ui.src_subtitle), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_line_wrap(GTK_LABEL(ui.src_subtitle), FALSE);
    ui.replace_switch = gtk_switch_new();
    g_signal_connect(ui.replace_switch, "notify::active", G_CALLBACK(on_setup_changed), NULL);
    add_row(list, "Originale ersetzen", "Die AV1-Datei (.mkv) ersetzt das Original – erst nachdem "
            "sie vollständig geprüft ist. Das spart sofort Platz. Ohne diese Option landen die "
            "Ergebnisse im Ausgabeordner.", NULL, ui.replace_switch, NULL, NULL);
    ui.out_button = gtk_button_new_with_label("Ändern …");
    g_signal_connect(ui.out_button, "clicked", G_CALLBACK(on_choose_out), NULL);
    ui.out_row = add_row(list, "Ausgabeordner", "", NULL, ui.out_button, NULL, &ui.out_subtitle);
    make_drop_target(ui.out_row, DROP_OUT, "Ordner auswählen oder aus dem Dateimanager hierher "
                     "ziehen");
    gtk_label_set_ellipsize(GTK_LABEL(ui.out_subtitle), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_line_wrap(GTK_LABEL(ui.out_subtitle), FALSE);

    list = add_group(box, "Hardware");
    static const char *const title[3] = {"CPU – HandBrake (exakt wie das Profil)",
                                         "GPU – Grafikkarte (empfohlen)",
                                         "GPU + CPU parallel"};
    ui.detect_spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(ui.detect_spinner));
    for (int i = 0; i < 3; i++) {
        GtkRadioButton *group_member = i ? GTK_RADIO_BUTTON(ui.mode_buttons[0]) : NULL;
        ui.mode_buttons[i] = gtk_radio_button_new_from_widget(group_member);
        ui.mode_rows[i] = add_row(list, title[i], "", ui.mode_buttons[i],
                                  i == 0 ? ui.detect_spinner : NULL, NULL, &ui.mode_subtitles[i]);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(ui.mode_rows[i]), TRUE);
    }
    set_label_text(ui.mode_subtitles[2], "Für möglichst kleine Dateien: Die CPU wandelt zusätzlich "
                   "die kleinsten Dateien mit HandBrake um. Bringt die GPU bei "
                   "einem Video keinen Gewinn (z. B. stark komprimierte YouTube-Downloads), versucht "
                   "es HandBrake – meist noch 13–25 % kleiner; auch früher ohne Gewinn gebliebene "
                   "Videos werden dann wieder aufgenommen. Langsamer: HandBrake bremst die GPU "
                   "(gemessen insgesamt ~310 statt ~500–850 Bilder/s).");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.mode_buttons[1]), TRUE);
    g_signal_connect(list, "row-activated", G_CALLBACK(on_mode_row_activated), NULL);
    for (int i = 0; i < 3; i++)
        g_signal_connect(ui.mode_buttons[i], "toggled", G_CALLBACK(on_setup_changed), NULL);

    list = add_group(box, "Leistung");
    ui.cpu_jobs_spin = new_spin(1, MAX_CPU_JOBS, 1, DEFAULT_CPU_JOBS);
    g_signal_connect(ui.cpu_jobs_spin, "value-changed", G_CALLBACK(on_setting_changed), NULL);
    add_row(list, "CPU-Jobs gleichzeitig", "Ein HandBrake-Job lastet die CPU schon fast aus: 2 Jobs "
            "bringen nur 16 % mehr, brauchen aber je 3–6 GB Arbeitsspeicher. Videos über 1440p "
            "belegen zwei Plätze; neue Jobs starten nur bei genug freiem Speicher.", NULL,
            ui.cpu_jobs_spin, NULL, NULL);
    ui.gpu_jobs_spin = new_spin(1, MAX_GPU_JOBS, 1, DEFAULT_GPU_JOBS);
    g_signal_connect(ui.gpu_jobs_spin, "value-changed", G_CALLBACK(on_setting_changed), NULL);
    add_row(list, "GPU-Jobs gleichzeitig", "1080p: 1 Job ~575, 2–3 Jobs ~850 Bilder/s. Drei halten "
            "die GPU ausgelastet, während einer die schwarzen Balken erkennt (~1,5 s je Datei).",
            NULL, ui.gpu_jobs_spin, NULL, NULL);

    list = add_group(box, "Qualität");
    ui.rf_spin = new_spin(15, 55, 1, DEFAULT_RF);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(ui.rf_spin), 0);
    g_signal_connect(ui.rf_spin, "value-changed", G_CALLBACK(on_rf_changed), NULL);
    add_row(list, "CPU (HandBrake-RF)", "Kleiner = bessere Qualität, größere Dateien. Standard ist "
            "der Wert aus dem Profil.", NULL, ui.rf_spin, NULL, NULL);
    ui.gpu_quality_spin = new_spin(1, 255, 1, DEFAULT_GPU_QUALITY);
    g_signal_connect(ui.gpu_quality_spin, "value-changed", G_CALLBACK(on_setting_changed), NULL);
    add_row(list, "GPU (VAAPI, 0–255)", "Kleiner = bessere Qualität, größere Dateien. "
            G_STRINGIFY(DEFAULT_GPU_QUALITY) " ergibt laut Messung (PSNR/SSIM) dieselbe "
            "Bildqualität wie RF 35 per CPU.", NULL, ui.gpu_quality_spin, NULL, NULL);
    ui.hdr_combo = new_combo(HDR_TEXT, 3);
    g_signal_connect(ui.hdr_combo, "changed", G_CALLBACK(on_hdr_changed), NULL);
    add_row(list, "HDR-Videos", "", NULL, ui.hdr_combo, NULL, &ui.hdr_subtitle);

    list = add_group(box, "Ton und Untertitel");
    ui.audio_combo = new_combo(AUDIO_TEXT, 3);
    g_signal_connect(ui.audio_combo, "changed", G_CALLBACK(on_setting_changed), NULL);
    add_row(list, "Tonspuren", "Das Profil behält nur die erste Tonspur. Wer mehrere Sprachen "
            "behalten will, wählt „alle“.", NULL, ui.audio_combo, NULL, NULL);
    ui.subs_combo = new_combo(SUBS_TEXT, 2);
    g_signal_connect(ui.subs_combo, "changed", G_CALLBACK(on_setting_changed), NULL);
    add_row(list, "Untertitel", "Das Profil übernimmt keine Untertitel (HandBrake brennt nur "
            "erzwungene fremdsprachige ein). Kapitel bleiben immer erhalten.", NULL, ui.subs_combo,
            NULL, NULL);

    list = add_group(box, "Speicherplatz");
    ui.min_saving_spin = new_spin(0, 90, 1, DEFAULT_MIN_SAVING);
    g_signal_connect(ui.min_saving_spin, "value-changed", G_CALLBACK(on_setting_changed), NULL);
    add_row(list, "Mindestersparnis (%)", "Wird die AV1-Datei nicht mindestens so viel kleiner, "
            "bleibt das Original (z. B. bei schon gut komprimiertem HEVC).", NULL,
            ui.min_saving_spin, NULL, NULL);
    ui.mtime_switch = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(ui.mtime_switch), TRUE);
    g_signal_connect(ui.mtime_switch, "notify::active", G_CALLBACK(on_setting_changed), NULL);
    add_row(list, "Änderungsdatum behalten", "Die AV1-Datei bekommt das Datum des Originals – "
            "Mediatheken sortieren dann weiter richtig.", NULL, ui.mtime_switch, NULL, NULL);
    button = gtk_button_new_with_label("Zurücksetzen …");
    g_signal_connect(button, "clicked", G_CALLBACK(on_reset_done_clicked), NULL);
    add_row(list, "Bearbeitete Dateien", "", NULL, button, NULL, &ui.done_list_subtitle);
    button = gtk_button_new_with_label("Verschieben …");
    add_css_class(button, "destructive-action");
    g_signal_connect(button, "clicked", G_CALLBACK(on_move_clicked), NULL);
    ui.pending_row = add_row(list, "Noch nicht an den Platz der Originale verschoben", "", NULL,
                             button, NULL, &ui.pending_subtitle);
    gtk_widget_show_all(ui.pending_row);
    gtk_widget_set_no_show_all(ui.pending_row, TRUE);

    list = add_group(box, "Nach der Umwandlung");
    ui.after_combo = new_after_action_combo();
    add_row(list, "Wenn alles fertig ist", "Herunterfahren bzw. Beenden erst nach einem Countdown, "
            "der sich abbrechen lässt – und nur, wenn die Umwandlung vollständig durchlief.", NULL,
            ui.after_combo, NULL, &sub);

    /* Hauptaktion immer sichtbar in der Leiste unten */
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(outer), scroller, TRUE, TRUE, 0);
    GtkWidget *action_bar = gtk_action_bar_new();
    ui.setup_hint = new_label("", "dim-label");
    gtk_widget_set_margin_start(ui.setup_hint, 12);
    gtk_action_bar_pack_start(GTK_ACTION_BAR(action_bar), ui.setup_hint);
    ui.search_button = new_pill_button("Videos suchen", "suggested-action");
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

static void on_search_cancel_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    ac_cancel = 1;
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

static GtkWidget *build_search_page(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    GtkWidget *spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_widget_set_size_request(spinner, 48, 48);
    ui.search_title = center_widget(new_label("Suche läuft …", "ac-hero"));
    ui.search_subtitle = center_widget(new_label("", "dim-label"));
    ui.search_progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(ui.search_progress), TRUE);
    gtk_widget_set_size_request(ui.search_progress, 460, -1);
    ui.search_cancel_button = gtk_button_new_with_label("Abbrechen");
    gtk_widget_set_halign(ui.search_cancel_button, GTK_ALIGN_CENTER);
    g_signal_connect(ui.search_cancel_button, "clicked", G_CALLBACK(on_search_cancel_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.search_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.search_subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.search_progress, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.search_cancel_button, FALSE, FALSE, 8);
    return box;
}

/* ---------------------------------------------------------------------------
 * Seite 3: Videos (Auswahl)
 * Zeilen werden in Portionen von RESULT_ROWS_BATCH angelegt, damit auch
 * Zehntausende Videos die Oberfläche nicht lahmlegen; ausgewählt sein
 * können trotzdem alle.
 * ------------------------------------------------------------------------- */
static void update_selection_count(void)
{
    int n = 0;
    double bytes = 0, est = 0;
    for (int i = 0; i < ui.todo_count; i++)
        if (ui.selected[i]) {
            const AcVideo *v = &ui.scan.v[ui.todo[i]];
            n++;
            bytes += (double)v->size;
            est += v->size * ac_estimated_saving(v);
        }
    AcVideo **sel = g_new(AcVideo *, MAX(ui.todo_count, 1));
    int k = 0;
    for (int i = 0; i < ui.todo_count; i++)
        if (ui.selected[i]) sel[k++] = &ui.scan.v[ui.todo[i]];
    double secs = ac_estimate_total(sel, k, &ui.run_opts, ui.gpu.available);
    g_free(sel);
    char b[32], e[32], dur[32];
    char *t = g_strdup_printf("%d ausgewählt · %s · Ersparnis ca. %s · Dauer ca. %s", n,
                              ac_format_bytes(bytes, b, sizeof b), ac_format_bytes(est, e, sizeof e),
                              ac_format_duration(secs, dur, sizeof dur));
    set_label_text(ui.selection_label, t);
    g_free(t);
    t = n == 1 ? g_strdup("1 Video umwandeln") : g_strdup_printf("%d Videos umwandeln", n);
    gtk_button_set_label(GTK_BUTTON(ui.start_button), t);
    gtk_widget_set_sensitive(ui.start_button, n > 0 && !ui.busy);
    g_free(t);
}

static void on_check_toggled(GtkToggleButton *check, gpointer d)
{
    int i = GPOINTER_TO_INT(d);
    ui.selected[i] = gtk_toggle_button_get_active(check);
    if (!ui.bulk_selecting) update_selection_count();
}

static void select_all(bool on)
{
    ui.bulk_selecting = true;
    for (int i = 0; i < ui.todo_count; i++) ui.selected[i] = on;
    for (guint i = 0; i < ui.checks->len; i++)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.checks->pdata[i]), on);
    ui.bulk_selecting = false;
    update_selection_count();
}

static void on_select_all_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    select_all(true);
}

static void on_select_none_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    select_all(false);
}

/* Beschreibung eines Videos: "Ordner · 1920×1080 · H.264 · 2,8 Mbit/s · 1:24:00 h" */
static char *video_details(const AcVideo *v)
{
    GString *s = g_string_new(NULL);
    char *dir = g_path_get_dirname(v->rel);
    if (strcmp(dir, ".")) {
        char *d = path_for_display(dir);
        g_string_append_printf(s, "%s · ", d);
        g_free(d);
    }
    g_free(dir);
    char dur[32], kbps[32];
    g_snprintf(kbps, sizeof kbps, "%.1f", v->kbps / 1000);
    for (char *p = kbps; *p; p++)
        if (*p == '.') *p = ',';
    g_string_append_printf(s, "%d×%d · %s · %s Mbit/s · %s", v->width, v->height,
                           ac_codec_name(v->vcodec), kbps,
                           ac_format_duration(v->duration, dur, sizeof dur));
    if (v->hdr) g_string_append(s, " · HDR");
    if (v->interlaced) g_string_append(s, " · interlaced");
    if (v->cpu_only)
        g_string_append(s, v->hdr && ui.run_opts.hdr != AC_HDR_GPU_PLAIN
                               ? " · per HandBrake (HDR bleibt vollständig erhalten)"
                               : " · per HandBrake (GPU brachte keinen Gewinn)");
    else if (v->hdr && ui.run_opts.mode != AC_MODE_CPU)
        g_string_append(s, ui.run_opts.hdr == AC_HDR_GPU_CONTAINER
                               ? " · per GPU, HDR-Angaben in den Container"
                               : " · per GPU (ohne HDR-Helligkeitsangaben)");
    return g_string_free(s, FALSE);
}

static void add_video_row(int i)
{
    const AcVideo *v = &ui.scan.v[ui.todo[i]];
    GtkWidget *check = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), ui.selected[i]);
    g_signal_connect(check, "toggled", G_CALLBACK(on_check_toggled), GINT_TO_POINTER(i));
    g_ptr_array_add(ui.checks, check);
    char b[32], *base = g_path_get_basename(v->rel), *name = path_for_display(base);
    char *details = video_details(v);
    GtkWidget *size = new_label(ac_format_bytes((double)v->size, b, sizeof b), "ac-size");
    GtkWidget *row = add_row(ui.results_list, name, details, check, size, NULL, NULL);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    g_object_set_data(G_OBJECT(row), "check", check);
    gtk_widget_show_all(row);
    g_free(base);
    g_free(name);
    g_free(details);
}

static void on_more_clicked(GtkButton *b, gpointer d);

/* nächste Portion Zeilen anlegen; "Weitere anzeigen" ans Ende */
static void show_more_rows(void)
{
    if (ui.more_row) {
        gtk_widget_destroy(ui.more_row);
        ui.more_row = NULL;
    }
    int end = MIN(ui.todo_count, ui.rows_shown + RESULT_ROWS_BATCH);
    for (int i = ui.rows_shown; i < end; i++) add_video_row(i);
    ui.rows_shown = end;
    if (ui.rows_shown < ui.todo_count) {
        char *t = g_strdup_printf("Weitere anzeigen (%d von %d angezeigt)", ui.rows_shown,
                                  ui.todo_count);
        GtkWidget *button = gtk_button_new_with_label(t);
        g_free(t);
        g_signal_connect(button, "clicked", G_CALLBACK(on_more_clicked), NULL);
        gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
        ui.more_row = gtk_list_box_row_new();
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(ui.more_row), FALSE);
        gtk_widget_set_margin_top(button, 8);
        gtk_widget_set_margin_bottom(button, 8);
        gtk_container_add(GTK_CONTAINER(ui.more_row), button);
        gtk_container_add(GTK_CONTAINER(ui.results_list), ui.more_row);
        gtk_widget_show_all(ui.more_row);
    }
}

static void on_more_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    show_more_rows();
}

/* Klick auf eine Zeile schaltet ihr Häkchen um */
static void on_video_row_activated(GtkListBox *list, GtkListBoxRow *r, gpointer d)
{
    (void)list;
    (void)d;
    GtkWidget *check = g_object_get_data(G_OBJECT(r), "check");
    if (check)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check),
                                     !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check)));
}

static void clear_results(void)
{
    if (ui.results_list) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(ui.results_list));
        for (GList *c = children; c; c = c->next) gtk_widget_destroy(c->data);
        g_list_free(children);
    }
    ui.more_row = NULL;
    g_ptr_array_set_size(ui.checks, 0);
    g_free(ui.todo);
    g_free(ui.selected);
    ui.todo = NULL;
    ui.selected = NULL;
    ui.todo_count = ui.rows_shown = 0;
}

/* Seite 3 füllen: Zusammenfassung, übersprungene Videos, Liste */
static void fill_results_page(void)
{
    clear_results();
    ui.todo = g_new(int, MAX(ui.scan.n, 1));
    for (int i = 0; i < ui.scan.n; i++)
        if (ui.scan.v[i].state == AC_TODO) ui.todo[ui.todo_count++] = i;
    ui.selected = g_new(bool, MAX(ui.todo_count, 1));
    for (int i = 0; i < ui.todo_count; i++) ui.selected[i] = true;

    char b[32], d[32];
    char *t = ui.todo_count == 1 ? g_strdup("1 Video zum Umwandeln")
              : g_strdup_printf("%d Videos zum Umwandeln", ui.todo_count);
    set_label_text(ui.results_title, ui.todo_count ? t : "Nichts zu tun");
    g_free(t);
    t = g_strdup_printf("%d Videos gefunden · %s zum Umwandeln · Suche %s", ui.scan.n,
                        ac_format_bytes((double)ui.scan.bytes[AC_TODO], b, sizeof b),
                        ac_format_duration(ui.scan.secs, d, sizeof d));
    set_label_text(ui.results_subtitle, t);
    g_free(t);

    GList *old = gtk_container_get_children(GTK_CONTAINER(ui.skip_box));
    for (GList *c = old; c; c = c->next) gtk_widget_destroy(c->data);
    g_list_free(old);
    static const char *const skip_text[AC_STATE_COUNT] = {
        NULL, "Schon AV1", "Früher umgewandelt", "Früher ohne Gewinn (Original behalten)",
        "Früher fehlgeschlagen", "Schon im Ausgabeordner", "Nicht lesbar (kein Video)"};
    GtkWidget *list = NULL;
    for (int s = 1; s < AC_STATE_COUNT; s++) {
        if (!ui.scan.count[s]) continue;
        if (!list) list = add_group(ui.skip_box, "Übersprungen");
        GtkWidget *badge = new_label("", "ac-badge");
        char *n = g_strdup_printf("%d · %s", ui.scan.count[s],
                                  ac_format_bytes((double)ui.scan.bytes[s], b, sizeof b));
        set_label_text(badge, n);
        g_free(n);
        add_css_class(badge, s == AC_SKIP_FAILED || s == AC_SKIP_UNREADABLE ? "warn" : "ok");
        add_row(list, skip_text[s], NULL, NULL, badge, NULL, NULL);
    }
    if (ui.scan.count[AC_SKIP_FAILED] || ui.scan.count[AC_SKIP_NOGAIN])
        add_row(list, "Noch einmal versuchen?", "Fehlgeschlagene und gewinnlose Dateien lassen "
                "sich über „Bearbeitete Dateien → Zurücksetzen …“ in den Einstellungen wieder "
                "einreihen.", NULL, NULL, NULL, NULL);
    gtk_widget_show_all(ui.skip_box);
    show_more_rows();
    update_selection_count();
}

static void on_start_clicked(GtkButton *b, gpointer d);

static void on_back_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    if (ui.busy) return;
    show_info_bar(GTK_MESSAGE_INFO, NULL);
    show_page("setup");
}

static GtkWidget *build_results_page(void)
{
    GtkWidget *scroller, *box = new_page_column(&scroller);
    ui.results_title = new_label("", "ac-hero");
    gtk_widget_set_margin_top(ui.results_title, 8);
    ui.results_subtitle = new_label("", "dim-label");
    gtk_box_pack_start(GTK_BOX(box), ui.results_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.results_subtitle, FALSE, FALSE, 0);
    ui.skip_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_box_pack_start(GTK_BOX(box), ui.skip_box, FALSE, FALSE, 0);

    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(head), new_label("Videos (größte zuerst)", "ac-heading"), FALSE,
                       FALSE, 0);
    GtkWidget *none = gtk_button_new_with_label("Keine");
    GtkWidget *all = gtk_button_new_with_label("Alle");
    gtk_widget_set_valign(none, GTK_ALIGN_END);
    gtk_widget_set_valign(all, GTK_ALIGN_END);
    g_signal_connect(none, "clicked", G_CALLBACK(on_select_none_clicked), NULL);
    g_signal_connect(all, "clicked", G_CALLBACK(on_select_all_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(head), none, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(head), all, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), head, FALSE, FALSE, 0);
    ui.results_list = add_group(box, NULL);
    g_signal_connect(ui.results_list, "row-activated", G_CALLBACK(on_video_row_activated), NULL);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(outer), scroller, TRUE, TRUE, 0);
    GtkWidget *action_bar = gtk_action_bar_new();
    ui.selection_label = new_label("", "dim-label");
    gtk_widget_set_margin_start(ui.selection_label, 12);
    gtk_label_set_ellipsize(GTK_LABEL(ui.selection_label), PANGO_ELLIPSIZE_END);
    gtk_action_bar_pack_start(GTK_ACTION_BAR(action_bar), ui.selection_label);
    ui.start_button = new_pill_button("Umwandeln", "suggested-action");
    gtk_widget_set_margin_top(ui.start_button, 6);
    gtk_widget_set_margin_bottom(ui.start_button, 6);
    gtk_widget_set_margin_end(ui.start_button, 12);
    g_signal_connect(ui.start_button, "clicked", G_CALLBACK(on_start_clicked), NULL);
    gtk_action_bar_pack_end(GTK_ACTION_BAR(action_bar), ui.start_button);
    gtk_box_pack_end(GTK_BOX(outer), action_bar, FALSE, FALSE, 0);
    return outer;
}

/* ---------------------------------------------------------------------------
 * Seite 4: Umwandlung
 * ------------------------------------------------------------------------- */
static void update_process_title(void)
{
    AcProgress p;
    int total = ac_progress(&p) ? p.files_total : 0;
    char *t = ui.processing_paused ? g_strdup("Pausiert")
              : total == 1 ? g_strdup("Wandle 1 Video um …")
                           : g_strdup_printf("Wandle %d Videos um …", total);
    set_label_text(ui.process_title, t);
    g_free(t);
}

/* Gesamtfortschritt: Dateien, Datenmenge, Ersparnis, Restzeit */
static void update_process_progress(void)
{
    AcProgress p;
    if (!ac_progress(&p)) return;
    char done[32], total[32], saved[32], eta[32];
    GString *t = g_string_new(NULL);
    g_string_append_printf(t, "%d von %d Dateien · %s von %s", p.files_done, p.files_total,
                           ac_format_bytes(p.bytes_done, done, sizeof done),
                           ac_format_bytes(p.bytes_total, total, sizeof total));
    if (ui.files_failed) g_string_append_printf(t, " · %d Fehler", ui.files_failed);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.process_progress),
                                  p.bytes_total > 0 ? CLAMP(p.bytes_done / p.bytes_total, 0, 1) : 0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.process_progress), t->str);
    g_string_truncate(t, 0);
    if (p.in_ok > 0)
        g_string_append_printf(t, "Bisher gespart: %s (−%.0f %%)",
                               ac_format_bytes((double)p.saved, saved, sizeof saved),
                               100.0 * (double)p.saved / (double)p.in_ok);
    else
        g_string_append(t, "Bisher gespart: –");
    char elapsed[32], est[32], finish[48];
    g_string_append_printf(t, " · Laufzeit %s · anfangs geschätzt %s",
                           ac_format_duration(p.secs, elapsed, sizeof elapsed),
                           ac_format_duration(p.est_total, est, sizeof est));
    set_label_text(ui.process_detail, t->str);
    g_string_truncate(t, 0);
    double rest = p.eta;
    g_string_append_printf(t, "Restdauer: %s", ac_format_remaining(rest, eta, sizeof eta));
    if (ui.processing_paused)
        g_string_append(t, " – pausiert, die Uhr läuft erst nach „Fortsetzen“ weiter");
    else
        g_string_append_printf(t, " · fertig voraussichtlich %s%s",
                               ac_format_finish(rest, finish, sizeof finish),
                               p.eta_measured ? "" : " (erste Schätzung)");
    set_label_text(ui.process_eta, t->str);
    g_string_free(t, TRUE);
}

static void set_processing_paused(bool on)
{
    ui.processing_paused = on;
    ac_set_paused(on);
    gtk_button_set_label(GTK_BUTTON(ui.process_pause_button), on ? "Fortsetzen" : "Pausieren");
    GtkStyleContext *ctx = gtk_widget_get_style_context(ui.process_pause_button);
    if (on) gtk_style_context_add_class(ctx, "suggested-action");
    else gtk_style_context_remove_class(ctx, "suggested-action");
    update_process_title();
    update_process_progress();
}

static void on_pause_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    if (!ui.busy || ac_cancel) return;
    set_processing_paused(!ui.processing_paused);
    protocol_append(ui.processing_paused
                        ? "Umwandlung pausiert – laufende Jobs sind angehalten, neue starten erst "
                          "nach „Fortsetzen“."
                        : "Umwandlung fortgesetzt.");
}

static void on_process_cancel_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    if (!ui.busy || ac_cancel) return;
    if (!ask_confirmation("Umwandlung abbrechen?",
                          "Laufende Jobs werden beendet und ihre halbfertigen Dateien gelöscht. "
                          "Fertige Dateien bleiben, die Originale der laufenden Jobs sind "
                          "unverändert. Ein neuer Lauf macht dort weiter.",
                          "Weiterlaufen lassen", "Umwandlung abbrechen", true))
        return;
    ac_cancel = 1;
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

/* Jobliste einer Art: Überschrift mit Status, Zeilen, "keine" und "+ N weitere" */
static void job_group_init(JobGroup *group, GtkWidget *box, const char *title, int nrows)
{
    group->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_box_pack_start(GTK_BOX(head), new_label(title, "ac-heading"), FALSE, FALSE, 0);
    group->status = new_label("", "dim-label");
    add_css_class(group->status, "ac-status");
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
    group->empty_title = empty_title;
    gtk_label_set_line_wrap(GTK_LABEL(empty_title), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(empty_title), PANGO_ELLIPSIZE_NONE);
    add_css_class(empty_title, "dim-label");
    gtk_widget_show_all(group->empty);
    gtk_widget_set_no_show_all(group->empty, TRUE);
    group->more = new_label("", "dim-label");
    add_css_class(group->more, "ac-sub");
    gtk_widget_set_margin_top(group->more, 4);
    gtk_widget_set_no_show_all(group->more, TRUE);
    gtk_box_pack_start(GTK_BOX(group->box), group->more, FALSE, FALSE, 0);
    gtk_widget_set_no_show_all(group->box, TRUE);
    gtk_widget_show_all(head);
    gtk_widget_show_all(gtk_widget_get_parent(group->list));
    gtk_widget_show(group->list);
    gtk_box_pack_start(GTK_BOX(box), group->box, FALSE, FALSE, 0);
}

static GtkWidget *build_process_page(void)
{
    GtkWidget *scroller, *box = new_page_column(&scroller);
    ui.process_title = new_label("", "ac-hero");
    gtk_widget_set_margin_top(ui.process_title, 8);
    ui.process_subtitle = new_label("", "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(ui.process_subtitle), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(ui.process_subtitle), 70);
    ui.process_progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(ui.process_progress), TRUE);
    gtk_widget_set_margin_top(ui.process_progress, 10);
    ui.process_eta = new_label("", "ac-eta");
    gtk_label_set_ellipsize(GTK_LABEL(ui.process_eta), PANGO_ELLIPSIZE_END);
    gtk_widget_set_tooltip_text(ui.process_eta, "Geschätzt aus Länge, Bildrate und Auflösung der "
                                "restlichen Videos und dem bisher gemessenen Tempo (die letzten "
                                "30 Minuten zählen am stärksten). Pausen zählen nicht mit.");
    ui.process_detail = new_label("", "dim-label");
    gtk_widget_set_margin_top(ui.process_detail, 4);
    gtk_box_pack_start(GTK_BOX(box), ui.process_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.process_subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.process_eta, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.process_progress, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.process_detail, FALSE, FALSE, 0);

    job_group_init(&ui.gpu_jobs, box, "GPU-Jobs", GPU_ROWS);
    job_group_init(&ui.cpu_jobs, box, "CPU-Jobs", CPU_ROWS);

    gtk_box_pack_start(GTK_BOX(box), new_label("Protokoll", "ac-heading"), FALSE, FALSE, 0);
    GtkWidget *frame = gtk_frame_new(NULL);
    add_css_class(frame, "ac-group");
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
    add_css_class(ui.protocol_view, "ac-log");
    create_protocol_tags();
    gtk_container_add(GTK_CONTAINER(protocol_scroller), ui.protocol_view);
    gtk_container_add(GTK_CONTAINER(frame), protocol_scroller);
    gtk_widget_set_vexpand(frame, TRUE);                /* Protokoll füllt die Höhe */
    gtk_box_pack_start(GTK_BOX(box), frame, TRUE, TRUE, 0);

    /* unten: Danach-Auswahl, Pausieren, Abbrechen */
    GtkWidget *bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(bottom, 14);
    ui.process_after_combo = new_after_action_combo();
    ui.process_pause_button = gtk_button_new_with_label("Pausieren");
    gtk_widget_set_tooltip_text(ui.process_pause_button,
                                "Laufende Jobs anhalten, keine neuen starten");
    g_signal_connect(ui.process_pause_button, "clicked", G_CALLBACK(on_pause_clicked), NULL);
    ui.process_cancel_button = gtk_button_new_with_label("Abbrechen");
    add_css_class(ui.process_cancel_button, "destructive-action");
    g_signal_connect(ui.process_cancel_button, "clicked", G_CALLBACK(on_process_cancel_clicked),
                     NULL);
    gtk_box_pack_start(GTK_BOX(bottom), new_label("Wenn fertig:", "dim-label"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bottom), ui.process_after_combo, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bottom), ui.process_cancel_button, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(bottom), ui.process_pause_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), bottom, FALSE, FALSE, 0);
    return scroller;
}

/* Jobs einer Art in ihre Liste eintragen */
static void fill_job_group(JobGroup *group, const AcJobInfo *jobs, int n, bool gpu, bool relevant)
{
    int shown = 0, total = 0;
    for (int i = 0; i < n; i++) {
        if (jobs[i].gpu != gpu) continue;
        total++;
        if (shown >= group->nrows) continue;
        JobRow *j = &group->rows[shown++];
        char since[32], eta[32], *base = g_path_get_basename(jobs[i].rel);
        char *name = path_for_display(base);
        GString *s = g_string_new(jobs[i].fallback ? "Neuversuch per HandBrake · " : "");
        g_string_append(s, jobs[i].what);
        if (jobs[i].gpu && !strcmp(jobs[i].what, "Umwandeln"))
            g_string_append(s, jobs[i].hw_decode ? " · Dekoder: GPU" : " · Dekoder: CPU");
        if (jobs[i].fps > 0) g_string_append_printf(s, " · %.0f Bilder/s", jobs[i].fps);
        g_string_append_printf(s, " · seit %s", ac_format_duration(jobs[i].secs, since, sizeof since));
        if (jobs[i].eta >= 0)
            g_string_append_printf(s, " · noch ca. %s", ac_format_duration(jobs[i].eta, eta, sizeof eta));
        if (ui.processing_paused) g_string_append(s, " · pausiert");
        char *pct_text = g_strdup_printf("%.0f %%", jobs[i].pct);
        set_label_text(j->title, name);
        set_label_text(j->subtitle, s->str);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(j->bar), CLAMP(jobs[i].pct / 100, 0, 1));
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(j->bar), pct_text);
        gtk_widget_show(j->row);
        g_free(base);
        g_free(name);
        g_string_free(s, TRUE);
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

/* laufende Jobs und Gesamtfortschritt anzeigen (alle 0,5 s) */
static gboolean on_jobs_tick(gpointer d)
{
    (void)d;
    static AcJobInfo jobs[32];
    int n = ac_jobs_snapshot(jobs, 32);
    AcMode m = ui.run_opts.mode;
    fill_job_group(&ui.gpu_jobs, jobs, n, true, m != AC_MODE_CPU);
    fill_job_group(&ui.cpu_jobs, jobs, n, false, true);     /* immer zeigen (mit CPU-Last) */
    int gpu_running = 0, gpu_cpu_decode = 0;
    for (int i = 0; i < n; i++) {
        gpu_running += jobs[i].gpu;
        gpu_cpu_decode += jobs[i].gpu && (!jobs[i].hw_decode || strcmp(jobs[i].what, "Umwandeln"));
    }
    /* keine CPU-Jobs: erklären, warum – und wofür die CPU trotzdem arbeitet */
    const char *empty = m == AC_MODE_GPU
        ? "Keine – im Modus „GPU“ wandelt HandBrake nur um, wenn die GPU an einer Datei "
          "scheitert. Die CPU-Last kommt von den GPU-Jobs: Dekodieren (H.264 & Co.), "
          "Balkenerkennung, Ton und Kopieren der Bilder."
        : m == AC_MODE_HYBRID ? "Gerade keine – die CPU wartet auf freien Arbeitsspeicher oder "
                                "auf die nächste Datei."
                              : "Gerade keine Jobs";
    set_label_text(ui.cpu_jobs.empty_title, empty);
    char *t = g_strdup_printf("%d von %d Plätzen belegt", gpu_running, ui.run_opts.gpu_jobs);
    set_label_text(ui.gpu_jobs.status, m != AC_MODE_CPU ? t : "");
    g_free(t);
    double cpu;
    int cpu_jobs = 0;
    for (int i = 0; i < n; i++) cpu_jobs += !jobs[i].gpu;
    t = !ac_cpu_load(&cpu) ? g_strdup("")
        : cpu_jobs == 0 && gpu_cpu_decode > 0
            ? g_strdup_printf("CPU-Auslastung %.0f %% – durch %d GPU-Job(s)", cpu, gpu_cpu_decode)
            : g_strdup_printf("CPU-Auslastung %.0f %%", cpu);
    set_label_text(ui.cpu_jobs.status, t);
    g_free(t);
    update_process_title();
    update_process_progress();
    return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------------------
 * Seite 5: Fertig
 * ------------------------------------------------------------------------- */
static void add_stat_row(GtkWidget *list, const char *title, const char *value)
{
    GtkWidget *v = new_label(value, NULL);
    gtk_label_set_xalign(GTK_LABEL(v), 1);
    gtk_label_set_ellipsize(GTK_LABEL(v), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(v), 50);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    add_row(list, title, NULL, NULL, v, NULL, NULL);
}

static void fill_done_page(bool aborted)
{
    const AcStats *st = &ui.stats;
    GtkStyleContext *icon_style = gtk_widget_get_style_context(ui.done_icon);
    gtk_style_context_remove_class(icon_style, "warn");
    gtk_style_context_remove_class(icon_style, "err");
    const char *icon = "emblem-ok-symbolic", *title = "Fertig";
    if (aborted) {
        icon = "process-stop-symbolic";
        title = "Abgebrochen";
        gtk_style_context_add_class(icon_style, "err");
    } else if (st->err) {
        icon = "dialog-warning-symbolic";
        title = "Fertig – mit Fehlern";
        gtk_style_context_add_class(icon_style, "warn");
    }
    gtk_image_set_from_icon_name(GTK_IMAGE(ui.done_icon), icon, GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(ui.done_icon), 72);
    set_label_text(ui.done_title, title);
    char saved[32], in[32], out[32], dur[32];
    long long saved_bytes = st->bytes_in - st->bytes_out;
    char *subtitle = g_strdup_printf("%s gespart · %d umgewandelt · %d ohne Gewinn · %d Fehler",
                                     ac_format_bytes((double)saved_bytes, saved, sizeof saved),
                                     st->ok, st->nogain, st->err);
    set_label_text(ui.done_subtitle, subtitle);
    g_free(subtitle);

    GList *old = gtk_container_get_children(GTK_CONTAINER(ui.done_stats));
    for (GList *item = old; item; item = item->next) gtk_widget_destroy(item->data);
    g_list_free(old);
    GtkWidget *list = add_group(ui.done_stats, NULL);
    char *t = g_strdup_printf("%s → %s", ac_format_bytes((double)st->bytes_in, in, sizeof in),
                              ac_format_bytes((double)st->bytes_out, out, sizeof out));
    add_stat_row(list, "Umgewandelte Dateien vorher → nachher", t);
    g_free(t);
    t = st->bytes_in > 0 ? g_strdup_printf("%s (−%.0f %%)", saved,
                                           100.0 * (double)saved_bytes / (double)st->bytes_in)
                         : g_strdup("–");
    add_stat_row(list, "Eingespart", t);
    g_free(t);
    t = g_strdup_printf("GPU: %d · CPU: %d · HandBrake-Neuversuch nach GPU: %d", st->gpu_files, st->cpu_files,
                        st->fallback_files);
    add_stat_row(list, "Umgewandelt per", t);
    g_free(t);
    t = g_strdup_printf("%d", st->nogain);
    add_stat_row(list, "Ohne Gewinn – Original behalten", t);
    g_free(t);
    t = g_strdup_printf("%d", st->err);
    add_stat_row(list, "Fehler (Original unverändert)", t);
    g_free(t);
    add_stat_row(list, "Dauer (ohne Pausen)", ac_format_duration(st->secs, dur, sizeof dur));
    t = ui.run_opts.replace ? g_strdup("Originale ersetzt") : path_for_display(ui.run_out_dir);
    add_stat_row(list, "Ergebnisse", t);
    g_free(t);
    gtk_widget_show_all(ui.done_stats);
    if (st->err || st->nogain) {
        set_label_text(ui.done_hint, "Dateien mit Fehler oder ohne Gewinn werden bei der nächsten "
                       "Suche übersprungen. Details stehen im Lauf-Protokoll; zurücksetzen lässt "
                       "sich das in den Einstellungen unter „Bearbeitete Dateien“.");
        gtk_widget_show(ui.done_hint);
    } else {
        gtk_widget_hide(ui.done_hint);
    }
    gtk_button_set_label(GTK_BUTTON(ui.done_open_button),
                         ui.run_opts.replace || !g_file_test(ui.run_out_dir, G_FILE_TEST_IS_DIR)
                             ? "Quellordner öffnen" : "Ausgabeordner öffnen");
}

static void on_open_results_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    bool output_exists = !ui.run_opts.replace && g_file_test(ui.run_out_dir, G_FILE_TEST_IS_DIR);
    open_in_default_app(output_exists ? ui.run_out_dir : ui.run_src_dir);
}

static void on_open_run_log_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    if (g_file_test(AC_RUN_LOG, G_FILE_TEST_IS_REGULAR)) open_in_default_app(AC_RUN_LOG);
}

static void on_new_run_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    clear_results();
    ac_scan_free(&ui.scan);
    update_done_list_row();
    update_pending_rows();
    show_info_bar(GTK_MESSAGE_INFO, NULL);
    show_page("setup");
}

static GtkWidget *build_done_page(void)
{
    GtkWidget *scroller, *box = new_page_column(&scroller);
    ui.done_icon = gtk_image_new_from_icon_name("emblem-ok-symbolic", GTK_ICON_SIZE_DIALOG);
    gtk_image_set_pixel_size(GTK_IMAGE(ui.done_icon), 72);
    add_css_class(ui.done_icon, "ac-bigicon");
    gtk_widget_set_margin_top(ui.done_icon, 18);
    ui.done_title = center_widget(new_label("", "ac-hero"));
    ui.done_subtitle = center_widget(new_label("", "dim-label"));
    ui.done_stats = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(ui.done_stats, 14);
    ui.done_hint = new_label("", "dim-label");
    add_css_class(ui.done_hint, "ac-sub");
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
    GtkWidget *again = new_pill_button("Neuer Durchlauf", "suggested-action");
    gtk_widget_set_halign(again, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(again, 8);
    g_signal_connect(again, "clicked", G_CALLBACK(on_new_run_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(btns), buttons_row, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), ui.done_move_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btns), again, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), btns, FALSE, FALSE, 0);
    return scroller;
}

/* ---------------------------------------------------------------------------
 * Fertige Dateien an den Platz der Originale verschieben – wie beim Intro-Cutter,
 * aber jederzeit möglich (die Liste überdauert Programmende und Neustart)
 * ------------------------------------------------------------------------- */
static void session_inhibit(bool on, const char *reason);
static void quit_application(void);

/* Zeile in den Einstellungen und Knopf auf der Fertig-Seite an die Liste anpassen */
static void update_pending_rows(void)
{
    AcPending p;
    bool any = !ui.busy && ac_pending_info(&p);
    if (any) {
        char dest[32], orig[32], freed[32];
        char *t = g_strdup_printf("%d AV1-Datei(en) (%s) liegen im Ausgabeordner, ihre Originale "
                                  "(%s) gibt es noch. Verschieben ersetzt die Originale und gibt "
                                  "%s frei.", p.files,
                                  ac_format_bytes((double)p.dest_bytes, dest, sizeof dest),
                                  ac_format_bytes((double)p.orig_bytes, orig, sizeof orig),
                                  ac_format_bytes((double)(p.orig_bytes - p.dest_bytes), freed,
                                                  sizeof freed));
        set_label_text(ui.pending_subtitle, t);
        g_free(t);
        t = p.files == 1 ? g_strdup("1 fertige Datei an den Platz des Originals verschieben")
                         : g_strdup_printf("%d fertige Dateien an den Platz der Originale "
                                           "verschieben", p.files);
        gtk_button_set_label(GTK_BUTTON(ui.done_move_button), t);
        g_free(t);
    }
    gtk_widget_set_visible(ui.pending_row, any);
    gtk_widget_set_visible(ui.done_move_button, any);
    if (ui.run_out_dir)                     /* Ausgabeordner nach dem Verschieben evtl. entfernt */
        gtk_button_set_label(GTK_BUTTON(ui.done_open_button),
                             !ui.run_opts.replace && g_file_test(ui.run_out_dir, G_FILE_TEST_IS_DIR)
                                 ? "Ausgabeordner öffnen" : "Quellordner öffnen");
}

typedef struct {
    int done, total;
    double bytes_done, bytes_total;
} MoveMsg;

static gboolean on_idle_move_progress(gpointer p)
{
    MoveMsg *m = p;
    if (ui.win && ui.moving) {
        char a[32], b[32];
        char *t = g_strdup_printf("%d von %d Dateien · %s von %s", m->done, m->total,
                                  ac_format_bytes(m->bytes_done, a, sizeof a),
                                  ac_format_bytes(m->bytes_total, b, sizeof b));
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.move_progress),
                                      m->bytes_total > 0 ? CLAMP(m->bytes_done / m->bytes_total, 0, 1)
                                                         : 1);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.move_progress), t);
        g_free(t);
    }
    g_free(m);
    return G_SOURCE_REMOVE;
}

/* Callback der Engine: Fortschritt beim Verschieben (aus dem Worker-Thread) */
static void frontend_move_progress(int done, int total, double bytes_done, double bytes_total)
{
    MoveMsg *m = g_new0(MoveMsg, 1);
    *m = (MoveMsg){done, total, bytes_done, bytes_total};
    g_idle_add(on_idle_move_progress, m);
}

typedef struct {
    AcMoveStats ms;
    bool completed;
} MoveDone;

static gboolean on_move_done(gpointer p)
{
    MoveDone *m = p;
    g_thread_join(ui.worker_thread);
    ui.worker_thread = NULL;
    ui.busy = ui.moving = false;
    ac_cancel = 0;
    session_inhibit(false, NULL);
    if (ui.quit_when_idle) {
        g_free(m);
        quit_application();
        return G_SOURCE_REMOVE;
    }
    char orig[32], freed[32];
    GString *t = g_string_new(NULL);
    g_string_append_printf(t, "%s %d Datei(en) an den Platz der Originale verschoben – %s "
                           "Originale ersetzt, %s frei geworden.",
                           m->completed ? "Fertig:" : "Angehalten:", m->ms.moved,
                           ac_format_bytes((double)m->ms.orig_bytes, orig, sizeof orig),
                           ac_format_bytes((double)(m->ms.orig_bytes - m->ms.dest_bytes), freed,
                                           sizeof freed));
    if (m->ms.skipped)
        g_string_append_printf(t, " %d übersprungen (Original geändert oder nicht mehr da – "
                               "Ergebnis bleibt im Ausgabeordner).", m->ms.skipped);
    if (m->ms.left)
        g_string_append_printf(t, " %d noch offen (%s) – lassen sich später verschieben.",
                               m->ms.left, m->ms.failed ? "Fehler, siehe Lauf-Protokoll"
                                                        : "angehalten");
    if (m->ms.dirs_removed) g_string_append(t, " Leere Ausgabeordner wurden entfernt.");
    show_page(ui.move_return_page ? ui.move_return_page : "setup");
    show_info_bar(m->ms.failed ? GTK_MESSAGE_WARNING : GTK_MESSAGE_INFO, t->str);
    g_string_free(t, TRUE);
    g_free(m);
    update_pending_rows();
    update_start_button();
    return G_SOURCE_REMOVE;
}

static gpointer move_thread(gpointer p)
{
    (void)p;
    MoveDone *m = g_new0(MoveDone, 1);
    m->completed = ac_move_to_originals(&m->ms);
    g_idle_add(on_move_done, m);
    return NULL;
}

/* "Nach der aktuellen Datei anhalten" */
static void on_move_cancel_clicked(GtkButton *b, gpointer d)
{
    (void)d;
    ac_cancel = 1;
    gtk_button_set_label(b, "Wird nach der aktuellen Datei angehalten …");
    gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
}

/* Knopf "… an den Platz der Originale verschieben" (Fertig-Seite, Einstellungen, Menü) */
static void start_move(void)
{
    if (ui.busy) {
        show_info_bar(GTK_MESSAGE_INFO, "Bitte warten, bis der laufende Vorgang beendet ist.");
        return;
    }
    AcPending p;
    if (!ac_pending_info(&p)) {
        show_info_bar(GTK_MESSAGE_INFO, "Es liegen keine fertigen Dateien im Ausgabeordner, deren "
                      "Originale noch ersetzt werden könnten.");
        update_pending_rows();
        return;
    }
    char dest[32], orig[32], freed[32];
    char *text = g_strdup_printf(
        "%d AV1-Datei(en) (%s) ersetzen ihre Originale (%s). Die Originale werden gelöscht – "
        "das gibt %s frei. Aus „Film.mp4“ wird „Film.mkv“ im selben Ordner.\n\n"
        "Übersprungen werden Originale, die sich seit der Umwandlung geändert haben oder nicht "
        "mehr da sind. Das lässt sich nicht rückgängig machen.",
        p.files, ac_format_bytes((double)p.dest_bytes, dest, sizeof dest),
        ac_format_bytes((double)p.orig_bytes, orig, sizeof orig),
        ac_format_bytes((double)(p.orig_bytes - p.dest_bytes), freed, sizeof freed));
    bool ok = ask_confirmation("An den Platz der Originale verschieben?", text, "Abbrechen",
                               "Originale ersetzen", true);
    g_free(text);
    if (!ok) return;
    g_free(ui.move_return_page);
    const char *current = gtk_stack_get_visible_child_name(GTK_STACK(ui.stack));
    ui.move_return_page = g_strdup(current ? current : "setup");
    ac_cancel = 0;
    ac_log_open();
    ui.busy = ui.moving = true;
    update_start_button();
    session_inhibit(true, "AV1-Converter verschiebt fertige Dateien");
    char *sub = g_strdup_printf("%d Datei(en) · %s", p.files,
                                ac_format_bytes((double)p.dest_bytes, dest, sizeof dest));
    set_label_text(ui.move_subtitle, sub);
    g_free(sub);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.move_progress), 0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.move_progress), "Wird vorbereitet …");
    gtk_button_set_label(GTK_BUTTON(ui.move_cancel_button), "Nach der aktuellen Datei anhalten");
    gtk_widget_set_sensitive(ui.move_cancel_button, TRUE);
    show_info_bar(GTK_MESSAGE_INFO, NULL);
    show_page("move");
    ui.worker_thread = g_thread_new("verschieben", move_thread, NULL);
}

static void on_move_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    start_move();
}

static GtkWidget *build_move_page(void)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    GtkWidget *spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_widget_set_size_request(spinner, 48, 48);
    ui.move_title = center_widget(new_label("Verschiebe an den Platz der Originale …", "ac-hero"));
    ui.move_subtitle = center_widget(new_label("", "dim-label"));
    GtkWidget *hint = center_widget(new_label("Liegen Ausgabeordner und Originale auf "
                                              "verschiedenen Laufwerken, wird jede Datei erst "
                                              "vollständig kopiert und dann ausgetauscht. "
                                              "Anhalten ist jederzeit möglich – der Rest lässt "
                                              "sich später verschieben.", "dim-label"));
    add_css_class(hint, "ac-sub");
    ui.move_progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(ui.move_progress), TRUE);
    gtk_widget_set_size_request(ui.move_progress, 460, -1);
    ui.move_cancel_button = gtk_button_new_with_label("Nach der aktuellen Datei anhalten");
    gtk_widget_set_halign(ui.move_cancel_button, GTK_ALIGN_CENTER);
    g_signal_connect(ui.move_cancel_button, "clicked", G_CALLBACK(on_move_cancel_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(box), spinner, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.move_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.move_subtitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.move_progress, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), ui.move_cancel_button, FALSE, FALSE, 8);
    return box;
}

/* ---------------------------------------------------------------------------
 * Ablauf: Suche
 * ------------------------------------------------------------------------- */
/* Während Suche und Umwandlung: kein automatischer Standby, beim Abmelden
 * fragt die Sitzung nach (die Engine hält zusätzlich eine logind-Sperre) */
static void session_inhibit(bool on, const char *reason)
{
    if (on && !ui.inhibit_cookie) {
        ui.inhibit_cookie = gtk_application_inhibit(
            ui.app, GTK_WINDOW(ui.win), GTK_APPLICATION_INHIBIT_SUSPEND | GTK_APPLICATION_INHIBIT_LOGOUT,
            reason);
    } else if (!on && ui.inhibit_cookie) {
        gtk_application_uninhibit(ui.app, ui.inhibit_cookie);
        ui.inhibit_cookie = 0;
    }
}

static void quit_application(void)
{
    settings_save();
    session_inhibit(false, NULL);
    ac_cleanup_temps();
    GtkWidget *w = ui.win;
    ui.win = NULL;                              /* spätere Idle-Callbacks ignorieren */
    gtk_widget_destroy(w);
}

typedef struct {
    int rc;
    char *err;
} SearchDone;

static gboolean on_search_done(gpointer p)
{
    SearchDone *done = p;
    g_thread_join(ui.worker_thread);
    ui.worker_thread = NULL;
    ui.busy = false;
    session_inhibit(false, NULL);
    ui.search_started = true;
    if (ui.quit_when_idle) {
        g_free(done->err);
        g_free(done);
        quit_application();
        return G_SOURCE_REMOVE;
    }
    if (done->rc == 1) {
        ac_scan_free(&ui.scan);
        show_page("setup");
        show_info_bar(GTK_MESSAGE_INFO, "Suche abgebrochen.");
    } else if (done->rc == -1) {
        show_page("setup");
        show_info_bar(GTK_MESSAGE_ERROR, done->err);
    } else {
        fill_results_page();
        show_page("results");
    }
    ac_cancel = 0;
    update_start_button();
    g_free(done->err);
    g_free(done);
    return G_SOURCE_REMOVE;
}

static gpointer search_thread(gpointer p)
{
    (void)p;
    const char *err = NULL;
    SearchDone *done = g_new0(SearchDone, 1);
    done->rc = ac_scan(ui.run_src_dir, &ui.run_opts, &ui.scan, &err);
    done->err = g_strdup(err);
    g_idle_add(on_search_done, done);
    return NULL;
}

/* Knopf "Videos suchen": Eingaben prüfen und die Suche starten */
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
    if (!ui.src_dir || !g_file_test(ui.src_dir, G_FILE_TEST_IS_DIR)) {
        show_info_bar(GTK_MESSAGE_ERROR, "Der Quellordner wurde nicht gefunden.");
        return;
    }
    char *src = g_canonicalize_filename(ui.src_dir, NULL);
    char *out = replace_originals_on() ? NULL : effective_out_dir();
    if (out) {
        char *o = g_canonicalize_filename(out, NULL);
        g_free(out);
        out = o;
        if (!strcmp(out, src)) {
            show_info_bar(GTK_MESSAGE_ERROR, "Ausgabe- und Quellordner dürfen nicht identisch sein "
                          "– zum Überschreiben „Originale ersetzen“ einschalten.");
            g_free(src);
            g_free(out);
            return;
        }
    }
    while (!ui.hb_ready || !ui.gpu_ready) gtk_main_iteration();   /* Prüfung abwarten (Sekunden) */
    settings_save();
    g_free(ui.run_src_dir);
    g_free(ui.run_out_dir);
    ui.run_src_dir = src;
    ui.run_out_dir = out;
    options_from_ui(&ui.run_opts);
    ui.run_opts.out_dir = ui.run_out_dir;
    clear_results();
    ac_scan_free(&ui.scan);

    ac_cancel = 0;
    ac_log_open();
    ac_log(false, "AV1-Converter %s · Quelle %s", AC_VERSION, src);
    set_label_text(ui.search_title, "Suche läuft …");
    char *s = path_for_display(src);
    set_label_text(ui.search_subtitle, s);
    g_free(s);
    session_inhibit(true, "AV1-Converter sucht Videos");
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.search_progress), "Ordner werden durchsucht …");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.search_progress), 0);
    gtk_button_set_label(GTK_BUTTON(ui.search_cancel_button), "Abbrechen");
    gtk_widget_set_sensitive(ui.search_cancel_button, TRUE);
    ui.search_started = false;
    if (!ui.pulse_timer) ui.pulse_timer = g_timeout_add(120, on_pulse_tick, NULL);
    ui.busy = true;
    update_start_button();
    show_page("busy");
    ui.worker_thread = g_thread_new("suche", search_thread, NULL);
}

/* ---------------------------------------------------------------------------
 * Wenn alles fertig ist: PC herunterfahren oder beenden – erst nach einem
 * Countdown, der sich abbrechen lässt
 * ------------------------------------------------------------------------- */
static GtkWidget *countdown_dialog;
static int countdown_secs_left, countdown_action;
static guint countdown_timer;

static void update_countdown_text(void)
{
    char *t = countdown_action == AFTER_POWEROFF
                  ? g_strdup_printf("Die Umwandlung ist fertig. Der PC wird in %d s "
                                    "heruntergefahren.", countdown_secs_left)
                  : g_strdup_printf("Die Umwandlung ist fertig. AV1-Converter wird in %d s "
                                    "beendet.", countdown_secs_left);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(countdown_dialog), "%s", t);
    g_free(t);
}

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

/* Herunterfahren über die Sitzung, sonst direkt über systemd-logind -> false mit Grund */
static bool request_power_off(char **reason)
{
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

static void on_countdown_response(GtkDialog *dialog, gint response, gpointer d)
{
    (void)d;
    if (countdown_timer) g_source_remove(countdown_timer);
    countdown_timer = 0;
    gtk_widget_destroy(GTK_WIDGET(dialog));
    countdown_dialog = NULL;
    if (response != GTK_RESPONSE_ACCEPT) {
        show_info_bar(GTK_MESSAGE_INFO, countdown_action == AFTER_POWEROFF
                                            ? "Herunterfahren abgebrochen." : "Beenden abgebrochen.");
        return;
    }
    if (countdown_action == AFTER_QUIT) {
        quit_application();
        return;
    }
    settings_save();
    ac_log(false, "Umwandlung fertig – PC wird heruntergefahren");
    char *reason = NULL;
    if (!request_power_off(&reason)) {
        char *t = g_strdup_printf("Herunterfahren nicht möglich: %s", reason);
        show_info_bar(GTK_MESSAGE_ERROR, t);
        g_free(t);
        g_free(reason);
    }
}

static void start_after_countdown(int action)
{
    if (countdown_dialog) return;
    countdown_action = action;
    countdown_secs_left = action == AFTER_POWEROFF ? 60 : 15;
    countdown_dialog = gtk_message_dialog_new(
        GTK_WINDOW(ui.win), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s",
        action == AFTER_POWEROFF ? "PC wird heruntergefahren" : "AV1-Converter wird beendet");
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

/* ---------------------------------------------------------------------------
 * Ablauf: Umwandlung
 * ------------------------------------------------------------------------- */
typedef struct {
    AcVideo **v;
    int n;
} Selection;

static gboolean on_processing_done(gpointer p)
{
    Selection *s = p;
    g_thread_join(ui.worker_thread);
    ui.worker_thread = NULL;
    if (ui.jobs_timer) g_source_remove(ui.jobs_timer);
    ui.jobs_timer = 0;
    ui.busy = false;
    g_free(s->v);
    g_free(s);
    session_inhibit(false, NULL);
    ui.processing_paused = false;
    if (ui.quit_when_idle) {
        quit_application();
        return G_SOURCE_REMOVE;
    }
    fill_done_page(!ui.run_completed);
    ac_cancel = 0;
    update_start_button();
    update_done_list_row();
    update_pending_rows();
    show_page("done");
    if (ui.run_completed && selected_after_action() != AFTER_NOTHING)
        start_after_countdown(selected_after_action());
    return G_SOURCE_REMOVE;
}

static gpointer processing_thread(gpointer p)
{
    Selection *s = p;
    ui.run_completed = ac_convert(s->v, s->n, &ui.run_opts, &ui.hb, &ui.gpu, &ui.stats);
    g_idle_add(on_processing_done, s);
    return NULL;
}

/* Knopf "N Videos umwandeln": Umwandlung starten */
static void on_start_clicked(GtkButton *b, gpointer d)
{
    (void)b;
    (void)d;
    if (ui.busy) return;
    Selection *sel = g_new0(Selection, 1);
    sel->v = g_new(AcVideo *, MAX(ui.todo_count, 1));
    for (int i = 0; i < ui.todo_count; i++)
        if (ui.selected[i]) sel->v[sel->n++] = &ui.scan.v[ui.todo[i]];
    if (!sel->n) {
        g_free(sel->v);
        g_free(sel);
        return;
    }
    if (ui.run_opts.replace) {
        char *t = g_strdup_printf("%d Originaldatei(en) werden durch die AV1-Fassung (.mkv) ersetzt "
                                  "– jeweils erst, wenn sie vollständig und geprüft ist und "
                                  "mindestens %d %% kleiner. Das lässt sich nicht rückgängig "
                                  "machen.", sel->n, ui.run_opts.min_saving);
        bool ok = ask_confirmation("Originale ersetzen?", t, "Abbrechen", "Originale ersetzen", true);
        g_free(t);
        if (!ok) {
            g_free(sel->v);
            g_free(sel);
            return;
        }
    }
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(ui.protocol_view)), "", 0);
    ui.files_failed = 0;
    ui.processing_paused = false;
    gtk_button_set_label(GTK_BUTTON(ui.process_pause_button), "Pausieren");
    gtk_style_context_remove_class(gtk_widget_get_style_context(ui.process_pause_button),
                                   "suggested-action");
    gtk_widget_set_sensitive(ui.process_pause_button, TRUE);
    gtk_button_set_label(GTK_BUTTON(ui.process_cancel_button), "Abbrechen");
    gtk_widget_set_sensitive(ui.process_cancel_button, TRUE);
    static const char *const mode_text[3] = {"CPU (HandBrake)", "GPU (VAAPI)", "GPU + CPU parallel"};
    char *outd = path_for_display(ui.run_out_dir);
    char *subtitle = ui.run_opts.replace
        ? g_strdup_printf("%s · Originale werden ersetzt", mode_text[ui.run_opts.mode])
        : g_strdup_printf("%s · Ausgabe: %s", mode_text[ui.run_opts.mode], outd);
    set_label_text(ui.process_subtitle, subtitle);
    g_free(subtitle);
    g_free(outd);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.process_progress), 0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.process_progress), "Wird gestartet …");
    set_label_text(ui.process_detail, "");
    set_label_text(ui.process_eta, "Restdauer wird berechnet …");
    ac_cancel = 0;
    ui.busy = true;
    update_start_button();
    session_inhibit(true, "AV1-Converter wandelt Videos um");
    show_page("process");
    ui.worker_thread = g_thread_new("umwandlung", processing_thread, sel);
    g_usleep(50 * 1000);                            /* Engine meldet den Lauf an */
    update_process_title();
    on_jobs_tick(NULL);
    ui.jobs_timer = g_timeout_add(500, on_jobs_tick, NULL);
}

/* ---------------------------------------------------------------------------
 * Fenster, Menü, Programmstart
 * ------------------------------------------------------------------------- */
static void show_page(const char *name)
{
    gtk_stack_set_visible_child_name(GTK_STACK(ui.stack), name);
    gtk_widget_set_visible(ui.back_btn, !strcmp(name, "results"));
    static const char *const titles[][2] = {{"setup", "AV1-Converter"},
                                            {"busy", "Suche"},
                                            {"results", "Videos"},
                                            {"process", "Umwandlung"},
                                            {"done", "Fertig"},
                                            {"move", "Verschieben"}};
    for (size_t i = 0; i < G_N_ELEMENTS(titles); i++)
        if (!strcmp(name, titles[i][0]))
            gtk_header_bar_set_subtitle(GTK_HEADER_BAR(ui.header), i ? titles[i][1] : NULL);
}

static int page_margin(int window_w)
{
    return CLAMP(window_w * 4 / 100, 16, 120);
}

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
    gtk_widget_set_size_request(ui.search_progress, CLAMP(w * 55 / 100, 380, 1000), -1);
    int bar_w = CLAMP(w * 25 / 100, 150, 600);
    JobGroup *groups[2] = {&ui.gpu_jobs, &ui.cpu_jobs};
    for (int g = 0; g < 2; g++)
        for (int i = 0; i < groups[g]->nrows; i++)
            gtk_widget_set_size_request(groups[g]->rows[i].bar, bar_w, -1);
    return G_SOURCE_REMOVE;
}

static void on_window_size_allocate(GtkWidget *w, GdkRectangle *a, gpointer d)
{
    (void)w;
    (void)d;
    if (a->width != ui.layout_width && !ui.layout_idle_id)
        ui.layout_idle_id = g_idle_add(on_idle_layout, NULL);
}

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
        ac_cleanup_temps();
        return FALSE;
    }
    if (ui.quit_when_idle) return TRUE;
    if (ui.moving) {
        if (!ask_confirmation("Das Verschieben läuft noch",
                              "Nach der aktuellen Datei anhalten und beenden? Noch nicht "
                              "verschobene Dateien bleiben im Ausgabeordner und lassen sich "
                              "später verschieben.", "Weiterlaufen lassen", "Anhalten und beenden",
                              false))
            return TRUE;
        ui.quit_when_idle = true;
        ac_cancel = 1;
        return TRUE;
    }
    if (!ask_confirmation("Die Umwandlung läuft noch",
                          "Beim Beenden werden laufende Jobs abgebrochen und halbfertige Dateien "
                          "gelöscht. Fertige Dateien bleiben; ein neuer Lauf macht dort weiter.",
                          "Weiterlaufen lassen", "Abbrechen und beenden", true))
        return TRUE;
    ui.quit_when_idle = true;
    ac_cancel = 1;
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
        ac_cancel = 1;
    } else {
        quit_application();
    }
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
    if (g_file_test(AC_RUN_LOG, G_FILE_TEST_IS_REGULAR)) open_in_default_app(AC_RUN_LOG);
    else show_info_bar(GTK_MESSAGE_INFO, "Es gibt noch kein Lauf-Protokoll.");
}

static void on_action_open_data(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    g_mkdir_with_parents(AC_APP_DIR, 0755);
    open_in_default_app(AC_APP_DIR);
}

static void on_action_reset_done(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    if (ui.busy) show_info_bar(GTK_MESSAGE_INFO, "Bitte warten, bis der Lauf beendet ist.");
    else reset_done_list();
}

static void on_action_move(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    start_move();
}

static void on_action_about(GSimpleAction *a, GVariant *p, gpointer d)
{
    (void)a;
    (void)p;
    (void)d;
    gtk_show_about_dialog(GTK_WINDOW(ui.win), "program-name", "AV1-Converter", "version",
                          AC_VERSION, "logo-icon-name", "av1-converter", "comments",
                          "Wandelt Videos nach AV1 um, um Speicherplatz zu sparen – mit den "
                          "Einstellungen des HandBrake-Profils „" AC_PRESET "“, per CPU "
                          "(HandBrake) und GPU (VAAPI).\n"
                          "Einstellungen, Protokolle und Listen: ~/.av1-converter",
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
        {.name = "reset-done", .activate = on_action_reset_done},
        {.name = "move", .activate = on_action_move},
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
    g_menu_append(menu, "Lauf-Protokoll öffnen", "app.open-log");
    g_menu_append(menu, "Datenordner öffnen", "app.open-data");
    g_menu_append(menu, "Fertige Dateien an den Platz der Originale verschieben …", "app.move");
    g_menu_append(menu, "Bearbeitete Dateien zurücksetzen …", "app.reset-done");
    g_menu_append(about, "Über AV1-Converter", "app.about");
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
    ui.checks = g_ptr_array_new();

    ui.win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(ui.win), "AV1-Converter");
    gtk_window_set_default_size(GTK_WINDOW(ui.win), 820, 820);
    gtk_window_set_icon_name(GTK_WINDOW(ui.win), "av1-converter");
    ui.header = gtk_header_bar_new();
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(ui.header), TRUE);
    gtk_header_bar_set_title(GTK_HEADER_BAR(ui.header), "AV1-Converter");
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
    gtk_stack_add_named(GTK_STACK(ui.stack), build_process_page(), "process");
    gtk_stack_add_named(GTK_STACK(ui.stack), build_done_page(), "done");
    gtk_stack_add_named(GTK_STACK(ui.stack), build_move_page(), "move");
    gtk_box_pack_start(GTK_BOX(vbox), ui.stack, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(ui.win), vbox);
    g_signal_connect(ui.win, "delete-event", G_CALLBACK(on_window_delete), NULL);

    ui.ffmpeg_ok = ac_have_ffmpeg();
    settings_load();
    if (ui.window_width > 0 && ui.window_height > 0)   /* Fenster wie beim letzten Mal */
        gtk_window_set_default_size(GTK_WINDOW(ui.win), ui.window_width, ui.window_height);
    if (ui.window_maximized) gtk_window_maximize(GTK_WINDOW(ui.win));
    g_signal_connect(ui.win, "configure-event", G_CALLBACK(on_window_configure), NULL);
    g_signal_connect(ui.win, "window-state-event", G_CALLBACK(on_window_state), NULL);
    g_signal_connect(ui.win, "size-allocate", G_CALLBACK(on_window_size_allocate), NULL);
    gtk_widget_show_all(ui.win);
    gtk_widget_hide(ui.infobar);
    update_mode_rows();
    update_hdr_subtitle();
    update_setup_labels();
    update_done_list_row();
    update_pending_rows();
    show_page("setup");
    if (!ui.ffmpeg_ok)
        show_info_bar(GTK_MESSAGE_ERROR, "ffmpeg ist nicht installiert – bitte über die "
                      "Anwendungsverwaltung oder mit „sudo apt install ffmpeg“ nachinstallieren.");
    g_thread_unref(g_thread_new("handbrake", hb_check_thread, NULL));
    g_thread_unref(g_thread_new("gpu", gpu_check_thread, NULL));
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    ac_init();
    ac_fe.log = frontend_log;
    ac_fe.scan_progress = frontend_scan_progress;
    ac_fe.file_done = frontend_file_done;
    ac_fe.move_progress = frontend_move_progress;
    g_set_prgname("av1-converter");
    g_set_application_name("AV1-Converter");
    ui.app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(ui.app, "startup", G_CALLBACK(on_startup), NULL);
    g_signal_connect(ui.app, "activate", G_CALLBACK(on_activate), NULL);
    g_unix_signal_add(SIGINT, on_unix_signal, NULL);
    g_unix_signal_add(SIGTERM, on_unix_signal, NULL);
    g_unix_signal_add(SIGHUP, on_unix_signal, NULL);
    int rc = g_application_run(G_APPLICATION(ui.app), argc, argv);
    ac_cleanup_temps();
    g_object_unref(ui.app);
    return rc;
}
