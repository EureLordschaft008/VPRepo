/*
 * Duplikat-Finder – gemeinsame Teile der Oberfläche (df_gui.c, df_gui_results.c)
 */
#ifndef DF_GUI_H
#define DF_GUI_H

#include <gtk/gtk.h>

#include "df_core.h"

#define APP_ID "de.duplikatfinder.DuplikatFinder"
#define SETTINGS_GROUP "Einstellungen"
#define GROUPS_BATCH 40         /* Gruppen der Trefferliste auf einmal (weitere per Knopf) */
#define PROTOCOL_MAX_LINES 5000

typedef struct MemberRow MemberRow;
typedef struct GroupBox GroupBox;

typedef struct {
    GtkApplication *app;
    GtkWidget *win, *header, *stack, *back_btn, *infobar, *info_label, *info_action;
    char *info_action_kind;                 /* was der Knopf der Meldungsleiste tut */

    /* Einstellungen */
    GtkWidget *include_list, *exclude_list, *include_add_row, *exclude_add_row;
    DfStrList includes, excludes;
    GtkWidget *subdirs_sw, *images_sw, *hidden_sw, *symlinks_sw, *hardlinks_sw;
    GtkWidget *min_mb_spin, *max_mb_spin, *contains_entry, *not_contains_entry;
    GtkWidget *similarity_spin, *frames_spin, *frames_sub, *flipped_sw, *duration_spin;
    GtkWidget *folder_combo, *againstdb_sw, *white_sw, *retry_sw;
    GtkWidget *partial_sw, *partial_ratio_spin, *partial_audio_spin, *partial_visual_sw;
    GtkWidget *partial_visual_spin, *partial_rows[4];
    GtkWidget *gpu_row_sub, *gpu_spinner, *cpu_spin, *gpu_spin, *considerate_sw;
    GtkWidget *thumbs_spin, *thumb_size_combo;
    GtkWidget *db_sub, *search_button, *open_last_button, *setup_hint;
    guint save_timer;
    bool loading_settings;
    int window_width, window_height;
    bool window_maximized;
    int criteria[DF_CRIT_COUNT];            /* Reihenfolge der Qualitätskriterien */
    int ncriteria;

    /* GPU */
    DfGpu gpu;
    bool gpu_ready;

    /* Suchlauf */
    DfOptions run_opts;
    GThread *worker;
    bool busy, quit_when_idle, paused;
    guint progress_timer;
    guint inhibit_cookie;                   /* Standby/Abmelden gesperrt (Sitzung) */
    int inhibit_count;
    DfPhase phase;
    GtkWidget *scan_title, *scan_sub, *scan_progress, *scan_eta, *scan_rates, *scan_jobs_list;
    GtkWidget *scan_jobs_rows[MAX_CPU_WORKERS + MAX_GPU_WORKERS];
    GtkWidget *scan_jobs_titles[MAX_CPU_WORKERS + MAX_GPU_WORKERS];
    GtkWidget *scan_jobs_subs[MAX_CPU_WORKERS + MAX_GPU_WORKERS];
    GtkWidget *scan_jobs_heading, *scan_jobs_frame;
    GtkWidget *protocol_view, *pause_button, *cancel_button;
    int phase_total;

    /* Ergebnis */
    DfResult res;
    bool *removed, *checked;
    bool have_result;
    guint result_save_timer;
    int *view;                              /* gefilterte/sortierte Gruppen */
    int nview, shown;
    MemberRow **mrows;                      /* je Mitglied (NULL = nicht angezeigt) */
    GroupBox **gboxes;                      /* je Gruppe */
    GtkWidget *res_summary, *res_list_box, *res_more_button, *res_search, *res_filter_combo;
    GtkWidget *res_sort_combo, *res_sel_label, *res_empty, *res_scroller;
    guint filter_timer;
    GThreadPool *thumb_pool;
    GHashTable *thumb_cache;                /* Schlüssel -> GdkPixbuf (Streifen) */
    int thumb_generation;
} UiState;

extern UiState ui;

/* Helfer (df_gui.c) */
void add_css_class(GtkWidget *w, const char *cls);
GtkWidget *new_label(const char *text, const char *cls);
GtkWidget *set_wrapping(GtkWidget *widget, int max_chars);
void set_label_text(GtkWidget *widget, const char *text);
char *path_for_display(const char *path);
void open_in_default_app(const char *path);
void show_in_file_manager(const char *path);
GtkWidget *add_group(GtkWidget *box, const char *title);
GtkWidget *add_row(GtkWidget *list, const char *title, const char *subtitle, GtkWidget *prefix,
                   GtkWidget *suffix, GtkWidget **title_out, GtkWidget **sub_out);
GtkWidget *new_page_column(GtkWidget **scroller, int max_width);
GtkWidget *new_pill_button(const char *text, const char *cls);
GtkWidget *new_spin(double lo, double hi, double step, double value);
bool ask_confirmation(const char *title, const char *text, const char *no_label,
                      const char *yes_label, bool destructive);
void show_info_bar(GtkMessageType type, const char *text);
void show_info_bar_action(GtkMessageType type, const char *text, const char *button,
                          const char *kind);
void show_page(const char *name);
char *choose_folder(const char *title, const char *current);
char *choose_file(const char *title, bool save, const char *suggest, const char *filter_name,
                  const char *pattern);
void options_from_ui(DfOptions *o);
void settings_changed(void);
void session_inhibit(bool on, const char *reason);
void update_db_row(void);
int thumb_count_setting(void);
int thumb_width_setting(void);

/* Trefferliste (df_gui_results.c) */
GtkWidget *build_results_page(void);
void results_set(DfResult *r, bool *removed, bool *checked);   /* übernimmt den Speicher */
void results_clear(void);
void results_rebuild_view(void);
void results_save_now(void);
bool results_load_file(const char *path, bool quiet);
void results_shutdown(void);
void compare_window_open(int group);
void pair_test_dialog(void);

#endif
