/*
 * Duplikat-Finder – Terminal-Version
 * ==================================
 * Aufruf:
 *     duplikat-finder-cli [Optionen] ORDNER [ORDNER ...]
 *     duplikat-finder-cli --zeitplan          (Einstellungen der Oberfläche, für den Zeitplan)
 *
 * Optionen:
 *     -a, --aehnlichkeit N     Mindestähnlichkeit in % (Standard 90)
 *     -b, --bilder N           Vergleichsbilder je Video (Standard 6)
 *     -l, --laufzeit N         max. Laufzeitunterschied in % (100 = egal, Standard 20)
 *     -x, --ausschliessen DIR  Ordner überspringen (mehrfach möglich)
 *         --fotos              auch Bilder vergleichen
 *         --keine-unterordner
 *         --nicht-gespiegelt   gespiegelte Videos nicht erkennen
 *         --teilclips          Teilclips über den Ton finden
 *         --ganze-datenbank    auch gegen alle Dateien der Datenbank vergleichen
 *         --cpu N              CPU-Prozesse (Standard: Kerne - 2)
 *         --gpu N              GPU-Prozesse (0 = keine GPU, Standard 2)
 *         --volle-leistung     keine Rücksicht auf andere Programme
 *         --json DATEI         Ergebnis speichern (für die Oberfläche: "Ergebnis öffnen")
 *         --csv DATEI          Ergebnis als Tabelle
 *         --html DATEI         Ergebnis als Webseite
 *     -q, --leise              nur das Ergebnis ausgeben
 *         --test A B           zwei Dateien vergleichen (Diagnose)
 *         --db-info | --db-aufraeumen | --db-verschieben ALT NEU
 */
#define _GNU_SOURCE
#include <glib.h>
#include <locale.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "df_core.h"

static bool g_quiet;
static DfPhase g_phase;
static double g_last_draw;

static void on_signal(int s)
{
    (void)s;
    df_cancel = 1;
}

static void cli_log(const char *msg)
{
    if (g_quiet) return;
    fprintf(stderr, "\r\033[K%s\n", msg);
}

static void cli_phase(DfPhase phase, int total)
{
    g_phase = phase;
    if (g_quiet) return;
    static const char *const name[] = {"Ordner lesen", "Vergleichsbilder lesen",
                                       "Ton-Fingerabdrücke", "Vergleichen", "Teilclips vergleichen"};
    if (total >= 0) fprintf(stderr, "\r\033[K== %s (%d)\n", name[phase], total);
    else fprintf(stderr, "\r\033[K== %s\n", name[phase]);
}

static void cli_progress(DfPhase phase, int done, int total)
{
    if (g_quiet || !isatty(2)) return;
    double now = (double)g_get_monotonic_time() / 1e6;
    if (now - g_last_draw < 0.5 && done != total) return;
    g_last_draw = now;
    if (phase == DF_PHASE_WALK) {
        fprintf(stderr, "\r\033[K  %d Dateien gefunden …", done);
    } else if (phase == DF_PHASE_SCAN || phase == DF_PHASE_AUDIO) {
        DfProgress p;
        df_progress(&p);
        char eta[48];
        fprintf(stderr, "\r\033[K  %d/%d  %.1f Dateien/s (GPU %.1f, CPU %.1f)  CPU-Last %.0f %%%s"
                "  noch %s", done, total, p.files_per_sec, p.gpu_per_sec, p.cpu_per_sec, p.cpu_load,
                p.held_cpu > 0 ? " (gebremst)" : "", df_format_eta(p.eta, eta, sizeof eta));
    } else {
        fprintf(stderr, "\r\033[K  %d/%d", done, total);
    }
}

static void print_result(const DfResult *r)
{
    for (int g = 0; g < r->ngroups; g++) {
        const DfGroup *gr = &r->groups[g];
        printf("\nGruppe %d (%d Dateien, ab %.1f %%)%s\n", g + 1, gr->n, gr->similarity,
               gr->partial ? " – mit Teilclips" : "");
        for (int i = 0; i < gr->n; i++) {
            const DfMember *m = &r->members[gr->first + i];
            const DfFile *f = &r->files[m->file];
            char sz[32], dur[32];
            printf("  %5.1f %%  %9s  %8s  %4dx%-4d %-6s %s%s", m->similarity,
                   df_format_bytes((double)f->size, sz, sizeof sz),
                   f->image ? "Bild" : df_format_duration(f->duration, dur, sizeof dur), f->width,
                   f->height, df_codec_name(f->vcodec), f->path, m->flipped ? "  [gespiegelt]" : "");
            if (m->partial) printf("  [Teilclip ab %s]", df_format_duration(m->offset, dur, sizeof dur));
            printf("\n");
        }
    }
}

static int pair_test(const char *a, const char *b, int frames)
{
    DfPairTest t;
    if (!df_pair_test(a, b, frames, false, &t)) {
        fprintf(stderr, "Fehler: %s\n", t.error);
        return 1;
    }
    printf("A: %s\n   %.1f s, %dx%d, %s\nB: %s\n   %.1f s, %dx%d, %s\n", a, t.a.duration, t.a.width,
           t.a.height, t.a.vcodec, b, t.b.duration, t.b.width, t.b.height, t.b.vcodec);
    for (int i = 0; i < t.nframes; i++)
        printf("  Bild %d: Abstand %2d Bit, gespiegelt %2d Bit%s%s\n", i + 1, t.dist[i],
               t.dist_flip[i], t.a.frames[i].flags & DF_FRAME_FLAT ? "  (A einfarbig)" : "",
               t.b.frames[i].flags & DF_FRAME_FLAT ? "  (B einfarbig)" : "");
    printf("Ähnlichkeit: %.1f %%, gespiegelt %.1f %%\n", t.similarity, t.similarity_flip);
    df_pair_test_free(&t);
    return 0;
}

/* Einstellungen der Oberfläche lesen (für --zeitplan) */
static bool load_gui_settings(DfOptions *o)
{
    char *path = g_build_filename(DF_APP_DIR, "gui.ini", NULL);
    GKeyFile *kf = g_key_file_new();
    bool ok = g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    g_free(path);
    if (!ok) {
        g_key_file_free(kf);
        return false;
    }
    const char *G = "Einstellungen";
    gsize n = 0;
    char **v = g_key_file_get_string_list(kf, G, "ordner", &n, NULL);
    for (gsize i = 0; v && i < n; i++) df_strlist_add(&o->includes, v[i]);
    g_strfreev(v);
    v = g_key_file_get_string_list(kf, G, "ausgeschlossen", &n, NULL);
    for (gsize i = 0; v && i < n; i++) df_strlist_add(&o->excludes, v[i]);
    g_strfreev(v);
    v = g_key_file_get_string_list(kf, G, "pfad_enthaelt", &n, NULL);
    for (gsize i = 0; v && i < n; i++) df_strlist_add(&o->path_contains, v[i]);
    g_strfreev(v);
    v = g_key_file_get_string_list(kf, G, "pfad_ohne", &n, NULL);
    for (gsize i = 0; v && i < n; i++) df_strlist_add(&o->path_excludes, v[i]);
    g_strfreev(v);
#define B(key, field) if (g_key_file_has_key(kf, G, key, NULL)) o->field = g_key_file_get_boolean(kf, G, key, NULL)
#define I(key, field) if (g_key_file_has_key(kf, G, key, NULL)) o->field = g_key_file_get_integer(kf, G, key, NULL)
    B("unterordner", subdirs);
    B("fotos", images);
    B("symlinks_ueberspringen", skip_symlinks);
    B("hardlinks_einmal", skip_hardlinks);
    B("versteckte_ueberspringen", skip_hidden);
    I("vergleichsbilder", frames);
    if (g_key_file_has_key(kf, G, "aehnlichkeit", NULL))
        o->similarity = g_key_file_get_double(kf, G, "aehnlichkeit", NULL);
    B("gespiegelt", flipped);
    B("weisse_balken", white_borders);
    I("laufzeit", duration_diff);
    I("ordner_modus", folder_mode);
    B("ganze_datenbank", against_db);
    B("fehler_neu", retry_failed);
    B("teilclips", partial);
    I("teilclip_anteil", partial_min_ratio);
    I("teilclip_ton", partial_audio);
    B("teilclip_bild", partial_visual);
    I("teilclip_bild_prozent", partial_visual_pct);
    I("cpu_prozesse", cpu_workers);
    I("gpu_prozesse", gpu_workers);
    B("ruecksicht", considerate);
    if (g_key_file_has_key(kf, G, "min_mb", NULL))
        o->min_size = (long long)(g_key_file_get_double(kf, G, "min_mb", NULL) * 1e6);
    if (g_key_file_has_key(kf, G, "max_mb", NULL))
        o->max_size = (long long)(g_key_file_get_double(kf, G, "max_mb", NULL) * 1e6);
#undef B
#undef I
    g_key_file_free(kf);
    return o->includes.n > 0;
}

static void usage(void)
{
    fprintf(stderr,
            "Aufruf: duplikat-finder-cli [Optionen] ORDNER [ORDNER ...]\n"
            "  -a, --aehnlichkeit N    Mindestähnlichkeit in %% (Standard %.0f)\n"
            "  -b, --bilder N          Vergleichsbilder je Video (1–%d, Standard %d)\n"
            "  -l, --laufzeit N        max. Laufzeitunterschied in %% (100 = egal, Standard %d)\n"
            "  -x, --ausschliessen DIR Ordner überspringen (mehrfach möglich)\n"
            "      --fotos             auch Bilder vergleichen\n"
            "      --keine-unterordner\n"
            "      --nicht-gespiegelt  gespiegelte Videos nicht erkennen\n"
            "      --teilclips         Teilclips (gekürzte Fassungen) über den Ton finden\n"
            "      --ganze-datenbank   auch gegen alle Dateien der Datenbank vergleichen\n"
            "      --cpu N / --gpu N   Anzahl Hilfsprozesse (GPU 0 = keine GPU)\n"
            "      --volle-leistung    keine Rücksicht auf andere Programme\n"
            "      --json/--csv/--html DATEI   Ergebnis speichern\n"
            "  -q, --leise             nur das Ergebnis ausgeben\n"
            "      --zeitplan          mit den Einstellungen der Oberfläche suchen und das\n"
            "                          Ergebnis für die Oberfläche speichern\n"
            "      --test A B          zwei Dateien vergleichen (Diagnose)\n"
            "      --db-info, --db-aufraeumen, --db-verschieben ALT NEU\n",
            DEFAULT_SIMILARITY, DF_MAX_FRAMES, DEFAULT_FRAMES, DEFAULT_DURATION_DIFF);
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    setlocale(LC_NUMERIC, "C");
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = {.sa_handler = on_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    df_init();
    DfOptions o;
    df_options_default(&o);
    const char *json = NULL, *csv = NULL, *html = NULL;
    bool schedule = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
#define NEXT() (i + 1 < argc ? argv[++i] : (usage(), exit(2), ""))
        if (!strcmp(a, "-a") || !strcmp(a, "--aehnlichkeit")) o.similarity = atof(NEXT());
        else if (!strcmp(a, "-b") || !strcmp(a, "--bilder")) o.frames = atoi(NEXT());
        else if (!strcmp(a, "-l") || !strcmp(a, "--laufzeit")) o.duration_diff = atoi(NEXT());
        else if (!strcmp(a, "-x") || !strcmp(a, "--ausschliessen")) df_strlist_add(&o.excludes, NEXT());
        else if (!strcmp(a, "--fotos")) o.images = true;
        else if (!strcmp(a, "--keine-unterordner")) o.subdirs = false;
        else if (!strcmp(a, "--nicht-gespiegelt")) o.flipped = false;
        else if (!strcmp(a, "--teilclips")) o.partial = true;
        else if (!strcmp(a, "--ganze-datenbank")) o.against_db = true;
        else if (!strcmp(a, "--cpu")) o.cpu_workers = atoi(NEXT());
        else if (!strcmp(a, "--gpu")) o.gpu_workers = atoi(NEXT());
        else if (!strcmp(a, "--volle-leistung")) o.considerate = false;
        else if (!strcmp(a, "--json")) json = NEXT();
        else if (!strcmp(a, "--csv")) csv = NEXT();
        else if (!strcmp(a, "--html")) html = NEXT();
        else if (!strcmp(a, "-q") || !strcmp(a, "--leise")) g_quiet = true;
        else if (!strcmp(a, "--zeitplan")) schedule = true;
        else if (!strcmp(a, "--test") && i + 2 < argc) {
            int rc = pair_test(argv[i + 1], argv[i + 2], o.frames);
            df_shutdown();
            return rc;
        } else if (!strcmp(a, "--db-info")) {
            DfDbInfo info;
            df_db_info(&info);
            char sz[32];
            printf("%s: %d Dateien, %d nicht lesbar, %d Ton-Fingerabdrücke, %d „kein Duplikat“, "
                   "%d ignoriert, %s\n", DF_DB_PATH, info.files, info.failed, info.audio,
                   info.notmatch, info.ignored, df_format_bytes((double)info.bytes, sz, sizeof sz));
            df_shutdown();
            return 0;
        } else if (!strcmp(a, "--db-aufraeumen")) {
            printf("%d Einträge fehlender Dateien entfernt\n", df_db_prune_missing());
            df_shutdown();
            return 0;
        } else if (!strcmp(a, "--db-verschieben") && i + 2 < argc) {
            printf("%d Pfade geändert\n", df_db_relocate(argv[i + 1], argv[i + 2]));
            df_shutdown();
            return 0;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--hilfe") || !strcmp(a, "--help")) {
            usage();
            return 0;
        } else if (a[0] == '-') {
            fprintf(stderr, "Unbekannte Option: %s\n", a);
            usage();
            return 2;
        } else {
            df_strlist_add(&o.includes, a);
        }
#undef NEXT
    }
    if (schedule) {
        DfOptions s;
        df_options_default(&s);
        if (!load_gui_settings(&s)) {
            fprintf(stderr, "Keine Ordner in den Einstellungen der Oberfläche.\n");
            return 2;
        }
        df_options_free(&o);
        o = s;
        g_quiet = true;
        if (!json) json = DF_SCHEDULED_PATH;
    }
    if (!o.includes.n) {
        usage();
        return 2;
    }
    if (o.frames < 1) o.frames = 1;
    if (o.frames > DF_MAX_FRAMES) o.frames = DF_MAX_FRAMES;
    df_fe.log = cli_log;
    df_fe.phase = cli_phase;
    df_fe.progress = cli_progress;
    DfGpu gpu = {0};
    if (o.gpu_workers > 0) df_gpu_detect(&gpu);
    if (!g_quiet)
        fprintf(stderr, "GPU: %s, CPU-Prozesse: %d, GPU-Prozesse: %d\n",
                gpu.available ? gpu.name : "keine", o.cpu_workers, gpu.available ? o.gpu_workers : 0);
    DfResult r;
    DfStats st;
    const char *err;
    int rc = df_scan(&o, &gpu, &r, &st, &err);
    if (!g_quiet) fprintf(stderr, "\r\033[K");
    if (rc < 0) {
        fprintf(stderr, "Fehler: %s\n", err);
        df_shutdown();
        return 1;
    }
    if (rc == 1) {
        fprintf(stderr, "Abgebrochen.\n");
        df_shutdown();
        return 130;
    }
    if (!schedule) print_result(&r);
    long long dup_bytes = 0;
    for (int g = 0; g < r.ngroups; g++) {
        const DfGroup *gr = &r.groups[g];
        long long mx = 0, sum = 0;
        for (int i = 0; i < gr->n; i++) {
            long long s = r.files[r.members[gr->first + i].file].size;
            sum += s;
            if (s > mx) mx = s;
        }
        dup_bytes += sum - mx;
    }
    char sz[32];
    fprintf(schedule ? stdout : stderr,
            "\n%d Gruppen, %d Duplikate (bis zu %s), %d Dateien geprüft (%d neu, %d nicht lesbar), "
            "%lld Paare verglichen in %.1f s\n", st.groups, st.duplicates,
            df_format_bytes((double)dup_bytes, sz, sizeof sz), st.found, st.scanned, st.failed,
            st.pairs, st.secs_compare);
    char errbuf[512];
    if (json && !df_result_save(&r, NULL, NULL, json, errbuf, sizeof errbuf))
        fprintf(stderr, "Fehler: %s\n", errbuf);
    if (csv && !df_result_export_csv(&r, NULL, NULL, csv, errbuf, sizeof errbuf))
        fprintf(stderr, "Fehler: %s\n", errbuf);
    if (html && !df_result_export_html(&r, NULL, NULL, html, errbuf, sizeof errbuf))
        fprintf(stderr, "Fehler: %s\n", errbuf);
    if (schedule && g_find_program_in_path("notify-send")) {
        char *msg = g_strdup_printf("%d Gruppen mit %d Duplikaten gefunden", st.groups, st.duplicates);
        char *argv2[] = {"notify-send", "-a", "Duplikat-Finder", "-i", "duplikat-finder",
                         "Geplanter Suchlauf fertig", msg, NULL};
        g_spawn_sync(NULL, argv2, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                     G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, NULL, NULL, NULL, NULL);
        g_free(msg);
    }
    df_result_free(&r);
    df_options_free(&o);
    df_shutdown();
    return 0;
}
