/*
 * Mediathek Downloader – Terminal-Version
 * =======================================
 * Sucht in den Mediatheken und lädt die gewählten Treffer herunter. Gut für lange
 * Läufe per SSH oder in einer tmux-Sitzung.
 *
 * Aufruf:
 *     mediathek-downloader-cli [Optionen] SUCHBEGRIFF
 *
 * Ohne --laden zeigt das Programm nur die nummerierte Trefferliste. Strg+C pausiert:
 * Die .part-Dateien bleiben, derselbe Befehl macht dort weiter.
 *
 * Exit-Codes: 0 = Erfolg, 1 = Fehler bei mindestens einem Auftrag, 2 = Aufruffehler,
 * 130 = abgebrochen.
 */
#define _GNU_SOURCE
#include <glib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "md_core.h"

static const char USAGE[] =
    "Aufruf: mediathek-downloader-cli [Optionen] SUCHBEGRIFF\n"
    "\n"
    "  --filme                nur Filme\n"
    "  --serien               nur Serien\n"
    "  --mindestlaenge MIN    kürzere Einträge ausblenden (Standard 10)\n"
    "  --varianten            Fassungen mit Gebärdensprache anzeigen\n"
    "  --mediathek LISTE      zdf, zdfneo, zdftivi, zdfinfo, phoenix, ard, one,\n"
    "                         ardalpha, tagesschau24, wdr, swr, ndr, mdr, br, hr, sr,\n"
    "                         rbb, kika, funk, 3sat, arte, arte-intl, srf, nrk,\n"
    "                         netzkino oder alle,\n"
    "                         z. B. zdf,ard (Standard alle)\n"
    "  --staffel N            nur diese Staffel\n"
    "  --nur-fehlende         schon geladene Titel ausblenden\n"
    "  --verlauf              alles anzeigen, was schon geladen wurde\n"
    "  --laden AUSWAHL        Nummern der Trefferliste, z. B. 1,3-5, oder \"alle\"\n"
    "  --ziel ORDNER          Speicherort (Standard: aktueller Ordner)\n"
    "  --qualitaet Q          hoch, mittel oder niedrig (Standard hoch)\n"
    "  --limit MB             Download-Tempo begrenzen, in MB/s (Standard unbegrenzt)\n"
    "  --parallel N           gleichzeitige Downloads, 1-4 (Standard 2)\n"
    "  --av1 gpu|cpu          nach dem Download nach AV1 umwandeln\n"
    "  --original-behalten    MP4-Datei nach der Umwandlung behalten\n"
    "  --ohne-ton LISTE       diese Tonspuren nicht laden, z. B. Englisch,Audiodeskription\n"
    "  --ohne-untertitel LISTE  diese Untertitel nicht laden (Standard: alles laden)\n";

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* Wert einer Option holen oder mit Aufruffehler beenden */
static const char *option_value(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "Bei %s fehlt der Wert.\n", argv[*i]);
        exit(2);
    }
    return argv[++*i];
}

/* Auswahl wie "1,3-5" oder "alle" -> je Treffer true */
static bool *parse_selection(const char *text, int count)
{
    bool *selected = g_new0(bool, MAX(count, 1));
    if (!strcmp(text, "alle")) {
        for (int i = 0; i < count; i++) selected[i] = true;
        return selected;
    }
    char **parts = g_strsplit(text, ",", -1);
    for (int i = 0; parts[i]; i++) {
        int first = 0, last = 0;
        int fields = sscanf(parts[i], "%d-%d", &first, &last);
        if (fields < 1) continue;
        if (fields == 1) last = first;
        for (int n = MAX(first, 1); n <= MIN(last, count); n++) selected[n - 1] = true;
    }
    g_strfreev(parts);
    return selected;
}

/* Namen für --mediathek */
static const struct {
    const char *name;
    int source;
} SOURCE_NAMES[] = {
    {"zdf", MD_SOURCE_ZDF}, {"zdfneo", MD_SOURCE_ZDFNEO}, {"zdftivi", MD_SOURCE_ZDFTIVI},
    {"zdfinfo", MD_SOURCE_ZDFINFO}, {"phoenix", MD_SOURCE_PHOENIX}, {"ard", MD_SOURCE_ARD},
    {"one", MD_SOURCE_ONE}, {"ardalpha", MD_SOURCE_ARDALPHA},
    {"tagesschau24", MD_SOURCE_TAGESSCHAU24}, {"wdr", MD_SOURCE_WDR}, {"swr", MD_SOURCE_SWR},
    {"ndr", MD_SOURCE_NDR}, {"mdr", MD_SOURCE_MDR}, {"br", MD_SOURCE_BR}, {"hr", MD_SOURCE_HR},
    {"sr", MD_SOURCE_SR}, {"rbb", MD_SOURCE_RBB}, {"kika", MD_SOURCE_KIKA},
    {"funk", MD_SOURCE_FUNK}, {"3sat", MD_SOURCE_3SAT}, {"arte", MD_SOURCE_ARTE},
    {"arte-intl", MD_SOURCE_ARTE_INTL}, {"srf", MD_SOURCE_SRF}, {"nrk", MD_SOURCE_NRK},
    {"netzkino", MD_SOURCE_NETZKINO},
};

/* Liste wie "zdf,ard" -> Bitmaske */
static int parse_sources(const char *text)
{
    int sources = 0;
    char **parts = g_strsplit(text, ",", -1);
    for (int i = 0; parts[i]; i++) {
        bool known = !g_ascii_strcasecmp(parts[i], "alle");
        if (known) sources = MD_SOURCE_ALL;
        for (size_t n = 0; !known && n < G_N_ELEMENTS(SOURCE_NAMES); n++)
            if (!g_ascii_strcasecmp(parts[i], SOURCE_NAMES[n].name)) {
                sources |= SOURCE_NAMES[n].source;
                known = true;
            }
        if (!known) {
            fprintf(stderr, "Unbekannte Mediathek %s\n", parts[i]);
            exit(2);
        }
    }
    g_strfreev(parts);
    return sources;
}

static const char *const STATUS_TEXT[] = {"", "angefangen", "in Warteschlange", "geladen",
                                          "Datei fehlt"};

/* Alles ausgeben, was schon geladen wurde -> Exit-Code */
static int print_history(void)
{
    int count = 0;
    MdHistoryEntry *entries = md_history_snapshot(&count);
    for (int i = 0; i < count; i++) {
        char *size = md_format_size((double)entries[i].size);
        printf("%-12s  %10s  %s\n", entries[i].exists ? "geladen" : "Datei fehlt", size,
               entries[i].title);
        g_free(size);
    }
    printf("%d Einträge\n", count);
    md_history_snapshot_free(entries, count);
    return 0;
}

/* Bezeichnungen der Spuren durch Komma getrennt anhängen */
static void append_labels(GString *out, GPtrArray *tracks)
{
    for (guint i = 0; i < tracks->len; i++)
        g_string_append_printf(out, "%s%s", i ? "," : "", ((MdTrack *)tracks->pdata[i])->label);
}

/* Trefferliste ausgeben */
static void print_titles(GPtrArray *titles, const char *dir)
{
    for (guint i = 0; i < titles->len; i++) {
        const MdTitle *t = titles->pdata[i];
        GString *tracks = g_string_new("");
        if (t->audio->len > 1 || t->subtitles->len) {
            g_string_append(tracks, "  [Ton: ");
            append_labels(tracks, t->audio);
            if (t->subtitles->len) g_string_append(tracks, "; UT: ");
            append_labels(tracks, t->subtitles);
            g_string_append_c(tracks, ']');
        }
        printf("%4u  %-12s  %-14s  %-24.24s  %3d min  %s%s\n", i + 1, md_source_label(t->source),
               STATUS_TEXT[md_title_status(t, dir, NULL)], t->topic, t->duration / 60, t->title,
               tracks->str);
        g_string_free(tracks, TRUE);
    }
}

/* Warten, bis alles erledigt ist, und den Stand ausgeben -> Anzahl der Fehler */
static int wait_for_queue(void)
{
    for (;;) {
        int count = 0, open = 0, failed = 0;
        MdJobView *jobs = md_queue_snapshot(&count);
        GString *line = g_string_new(NULL);
        for (int i = 0; i < count; i++) {
            const MdJobView *j = &jobs[i];
            if (j->state == MD_JOB_FAILED) failed++;
            if (j->state != MD_JOB_DONE && j->state != MD_JOB_FAILED) open++;
            if (j->state == MD_JOB_DOWNLOADING) {
                char *speed = md_format_size(j->speed);
                g_string_append_printf(line, "  %3.0f %% %s/s", j->progress * 100, speed);
                g_free(speed);
            } else if (j->state == MD_JOB_CONVERTING) {
                g_string_append_printf(line, "  %s %3.0f %%", j->merging ? "Spuren" : "AV1", j->progress * 100);
            }
        }
        printf("\r\033[K%d offen%s", open, line->str);
        fflush(stdout);
        g_string_free(line, TRUE);
        md_queue_snapshot_free(jobs, count);
        if (!open || g_stop) {
            printf("\n");
            return failed;
        }
        g_usleep(500 * 1000);
    }
}

/* Fehler der Aufträge ausgeben */
static void print_failures(void)
{
    int count = 0;
    MdJobView *jobs = md_queue_snapshot(&count);
    for (int i = 0; i < count; i++)
        if (jobs[i].state == MD_JOB_FAILED)
            fprintf(stderr, "Fehler: %s: %s\n", jobs[i].title, jobs[i].message);
    md_queue_snapshot_free(jobs, count);
}

int main(int argc, char **argv)
{
    MdQuery query = {.min_minutes = 10, .hide_variants = true};
    MdJobOptions options = {.dir = ".", .gpu_quality = MD_DEFAULT_GPU_QUALITY,
                            .cpu_rf = MD_DEFAULT_CPU_RF};
    const char *selection = NULL;
    double limit_mb = 0;
    int parallel = 2, season = -1;
    bool only_missing = false, show_history = false;
    char **skip_audio = NULL, **skip_subtitles = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--filme")) query.kind = MD_KIND_FILMS;
        else if (!strcmp(a, "--serien")) query.kind = MD_KIND_SERIES;
        else if (!strcmp(a, "--varianten")) query.hide_variants = false;
        else if (!strcmp(a, "--mindestlaenge")) query.min_minutes = atoi(option_value(argc, argv, &i));
        else if (!strcmp(a, "--laden")) selection = option_value(argc, argv, &i);
        else if (!strcmp(a, "--ziel")) options.dir = option_value(argc, argv, &i);
        else if (!strcmp(a, "--limit")) limit_mb = atof(option_value(argc, argv, &i));
        else if (!strcmp(a, "--parallel")) parallel = atoi(option_value(argc, argv, &i));
        else if (!strcmp(a, "--original-behalten")) options.keep_original = true;
        else if (!strcmp(a, "--nur-fehlende")) only_missing = true;
        else if (!strcmp(a, "--verlauf")) show_history = true;
        else if (!strcmp(a, "--staffel")) season = atoi(option_value(argc, argv, &i));
        else if (!strcmp(a, "--mediathek"))
            query.sources = parse_sources(option_value(argc, argv, &i));
        else if (!strcmp(a, "--ohne-ton"))
            skip_audio = g_strsplit(option_value(argc, argv, &i), ",", -1);
        else if (!strcmp(a, "--ohne-untertitel"))
            skip_subtitles = g_strsplit(option_value(argc, argv, &i), ",", -1);
        else if (!strcmp(a, "--qualitaet")) {
            const char *v = option_value(argc, argv, &i);
            options.quality = !strcmp(v, "mittel") ? MD_QUALITY_MEDIUM
                            : !strcmp(v, "niedrig") ? MD_QUALITY_LOW : MD_QUALITY_HIGH;
        } else if (!strcmp(a, "--av1")) {
            options.convert = true;
            options.encoder = strcmp(option_value(argc, argv, &i), "cpu") ? MD_ENCODER_GPU
                                                                          : MD_ENCODER_CPU;
        } else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            fputs(USAGE, stdout);
            return 0;
        } else if (a[0] == '-') {
            fprintf(stderr, "Unbekannte Option %s\n\n%s", a, USAGE);
            return 2;
        } else {
            query.text = a;
        }
    }
    if (!query.text && query.kind == MD_KIND_ALL && !show_history) {
        fputs(USAGE, stderr);
        return 2;
    }
    char *missing = NULL;
    if (!md_have_tools(&missing)) {
        fprintf(stderr, "%s ist nicht installiert.\n", missing);
        return 1;
    }

    md_init(false);
    if (show_history) {
        int rc = print_history();
        md_shutdown();
        return rc;
    }
    bool truncated = false;
    char *error = NULL;
    GPtrArray *titles = md_search(&query, NULL, &truncated, &error);
    if (!titles) {
        fprintf(stderr, "%s\n", error);
        md_shutdown();
        return 1;
    }
    options.skip_audio = (const char *const *)skip_audio;
    options.skip_subtitles = (const char *const *)skip_subtitles;
    for (guint i = titles->len; season >= 0 && i-- > 0;)
        if (((MdTitle *)titles->pdata[i])->season != season) g_ptr_array_remove_index(titles, i);
    for (guint i = titles->len; only_missing && i-- > 0;) {
        MdStatus status = md_title_status(titles->pdata[i], options.dir, NULL);
        if (status == MD_STATUS_DONE || status == MD_STATUS_QUEUED)
            g_ptr_array_remove_index(titles, i);
    }
    print_titles(titles, options.dir);
    printf("%u Treffer%s\n", titles->len, truncated ? " (Liste gekürzt)" : "");
    int rc = 0;
    if (selection) {
#ifdef G_OS_WIN32
        signal(SIGINT, on_signal);
        signal(SIGTERM, on_signal);
#else
        struct sigaction action = {.sa_handler = on_signal};
        sigaction(SIGINT, &action, NULL);
        sigaction(SIGTERM, &action, NULL);
        signal(SIGPIPE, SIG_IGN);
#endif
        md_set_rate_limit((gint64)(limit_mb * 1e6));
        md_set_parallel(parallel, 1);
        bool *selected = parse_selection(selection, (int)titles->len);
        for (guint i = 0; i < titles->len; i++)
            if (selected[i]) md_queue_add(titles->pdata[i], &options);
        g_free(selected);
        int failed = wait_for_queue();
        if (failed) print_failures();
        rc = g_stop ? 130 : failed ? 1 : 0;
    }
    g_ptr_array_free(titles, TRUE);
    md_shutdown();
    return rc;
}
