/*
 * Intro-Cutter 5.6.0 – Terminal-Version
 * =====================================
 * Interaktiver Ablauf wie in der Python-Version: Fragen im Terminal, Erkennung,
 * Trefferliste, Bearbeitung, Zusammenfassung. Die Arbeit erledigt die Engine
 * (ic_core.c).
 *
 * Exit-Codes: 0 = Erfolg, 1 = Fehler, 130 = Abbruch
 *
 * Aufruf:
 *     intro-cutter-cli            Intro am Anfang entfernen (wie das Python-Skript)
 *     intro-cutter-cli --outro    stattdessen das Outro am Ende entfernen
 *     intro-cutter-cli --bilder N gründlichere Erkennung mit N Vergleichsbildern
 *                                 (3, 5 oder 9; Standard 1 wie das Python-Skript)
 *     intro-cutter-cli --auto     Intros ohne Referenz-Video automatisch erkennen (auch
 *                                 mehrere verschiedene); --max S: Intros bis S Sekunden,
 *                                 --mindestens N: in mindestens N Videos. Geschnitten
 *                                 werden nur sicher erkannte Videos, je mit eigenem Schnitt
 * Pausieren: Strg+Z, fortsetzen: fg (der Watchdog zählt die Pause nicht mit)
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "ic_core.h"

static const char LINE[] = "================================================================";

enum { EXIT_OK = 0, EXIT_ERROR = 1, EXIT_ABORT = 130 };

static bool g_eof;              /* Eingabe unerwartet zu Ende (Pipe/Datei) */
static bool g_outro;            /* --outro: Outro am Ende statt Intro am Anfang */
static int g_frames = 1;        /* --bilder N: Vergleichsbilder je Video */
static bool g_auto;             /* --auto: wiederkehrende Intros ohne Referenz-Video finden */
static double g_auto_max = IC_AUTO_MAX_DEFAULT;     /* --max S: Intro höchstens so lang */
static int g_auto_min = IC_AUTO_MIN_VIDEOS;         /* --mindestens N: in so vielen Videos */

/* ---------------------------------------------------------------------------
 * Signale
 * ------------------------------------------------------------------------- */
static void on_signal(int sig)
{
    (void)sig;
    ic_cancel = 1;
}

/* Strg+C, SIGTERM und SIGHUP abfangen: geordneter Abbruch statt hartem Beenden */
static void install_signals(void)
{
    struct sigaction action;
    memset(&action, 0, sizeof action);
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    /* ohne SA_RESTART: blockierende Aufrufe (Eingabe, Warten) kehren sofort zurück */
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);
    signal(SIGPIPE, SIG_IGN);
}

/* ---------------------------------------------------------------------------
 * Eingabe-Hilfsfunktionen
 * ------------------------------------------------------------------------- */
/* Zeile von stdin lesen (ohne Zeilenende). -> NULL bei Strg+C (ic_cancel) oder
 * bei Eingabe-Ende (g_eof) */
static char *read_input_line(const char *prompt)
{
    char *shown = utf8_escape(prompt);
    fputs(shown, stdout);
    fflush(stdout);
    free(shown);
    Buf b = {0};
    while (true) {
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r == 1) {
            if (c == '\n') break;
            buf_add_char(&b, c);
            continue;
        }
        if (r < 0 && errno == EINTR && !ic_cancel) continue;
        if (r == 0 && b.n) break;                   /* letzte Zeile ohne Zeilenende */
        buf_free(&b);
        if (!ic_cancel) g_eof = true;
        return NULL;
    }
    return buf_detach(&b);
}

/* read_input_line() für Fragen mit Standardwert: Eingabe-Ende -> "" (= Standardwert) */
static char *read_input_or_default(const char *prompt)
{
    char *s = read_input_line(prompt);
    if (!s && !ic_cancel) {
        putchar('\n');
        return xstrdup("");
    }
    return s;
}

/* Leerraum am Anfang und Ende entfernen (verändert den String) */
static char *strip_spaces(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
    return s;
}

/* Leerzeichen und umschließende Anführungszeichen entfernen, ~ auflösen */
static char *clean_entered_path(char *s)
{
    s = strip_spaces(s);
    size_t n = strlen(s);
    if (n >= 2 && s[0] == s[n - 1] && (s[0] == '\'' || s[0] == '"')) {
        s[n - 1] = 0;
        s++;
    }
    return expanduser(strip_spaces(s));
}

/* Pfad abfragen (Anführungszeichen und ~ erlaubt), bis die Prüfung check ihn akzeptiert */
static char *ask_path(const char *prompt)
{
    char *q = xasprintf("%s: ", prompt), *line = read_input_line(q);
    free(q);
    if (!line) return NULL;
    char *p = clean_entered_path(line);
    free(line);
    return p;
}

/* Pfad einer vorhandenen Datei abfragen */
static char *ask_file(const char *prompt)
{
    while (true) {
        char *p = ask_path(prompt);
        if (!p) return NULL;
        if (*p && is_file(p)) return p;
        free(p);
        printf("  -> Datei nicht gefunden, bitte erneut eingeben.\n");
    }
}

/* Pfad eines vorhandenen Ordners abfragen */
static char *ask_dir(const char *prompt)
{
    while (true) {
        char *p = ask_path(prompt);
        if (!p) return NULL;
        if (!*p)
            printf("  -> Bitte einen Pfad eingeben.\n");
        else if (!is_dir(p))
            printf("  -> Ordner nicht gefunden, bitte erneut eingeben.\n");
        else
            return p;
        free(p);
    }
}

/* Zahl größer 0 abfragen (Komma oder Punkt) */
static bool ask_float(const char *prompt, double *out)
{
    char *q = xasprintf("%s: ", prompt);
    bool ok = false;
    while (!ic_cancel) {
        char *line = read_input_line(q);
        if (!line) break;
        for (char *c = line; *c; c++)
            if (*c == ',') *c = '.';
        double v;
        ok = parse_float(line, &v) && v > 0 && isfinite(v);
        free(line);
        if (ok) {
            *out = v;
            break;
        }
        printf("  -> Bitte eine positive Zahl eingeben (z. B. 2 oder 2.5).\n");
    }
    free(q);
    return ok;
}

/* ganze Zahl im Bereich lo..hi abfragen (Enter = Standardwert) */
static int ask_int(const char *prompt, int dflt)
{
    char *q = xasprintf("%s [Enter = %d]: ", prompt, dflt), *line = read_input_or_default(q);
    free(q);
    if (!line) return dflt;
    char *s = strip_spaces(line);
    long v = dflt;
    if (*s && (!parse_int(s, &v) || v < INT_MIN || v > INT_MAX)) {
        printf("  -> Ungültig, nutze %d.\n", dflt);
        v = dflt;
    }
    free(line);
    return (int)v;
}

/* Ausgabeordner abfragen (Enter = <Quelle>_output) */
static char *ask_output(const char *src)
{
    char *src_norm = normpath(src), *default_out = xasprintf("%s_output", src_norm);
    free(src_norm);
    char *q = xasprintf("Ausgabeordner [Enter = %s]: ", default_out),
         *line = read_input_or_default(q);
    char *p = line ? clean_entered_path(line) : NULL;
    free(q);
    free(line);
    if (p && *p) {
        free(default_out);
        return p;
    }
    free(p);
    if (ic_cancel) {
        free(default_out);
        return NULL;
    }
    return default_out;
}

/* Auswahl 1..n abfragen */
static int ask_choice(const char *prompt, const char *const *options, int n, int default_choice)
{
    printf("%s:\n", prompt);
    for (int i = 0; i < n; i++) printf("  %d) %s\n", i + 1, options[i]);
    char *prompt_text = xasprintf("Auswahl [Enter = %d]: ", default_choice),
         *line = read_input_or_default(prompt_text);
    free(prompt_text);
    if (!line) return default_choice;
    char *answer = strip_spaces(line);
    long number;
    int choice = default_choice;
    if (*answer) {
        if (parse_int(answer, &number) && number >= 1 && number <= n)
            choice = (int)number;
        else
            printf("  -> Ungültig, nutze %d.\n", default_choice);
    }
    free(line);
    return choice;
}

/* Ja/Nein-Frage (j oder n, Enter = Standard) */
static bool ask_yes_no(const char *prompt, bool dflt)
{
    char *q = xasprintf("%s [%s]: ", prompt, dflt ? "J/n" : "j/N"),
         *line = read_input_or_default(q);
    free(q);
    if (!line) return false;
    char *s = strip_spaces(line);
    for (char *c = s; *c; c++) *c = (char)tolower((unsigned char)*c);
    bool r = *s ? (!strcmp(s, "j") || !strcmp(s, "ja") || !strcmp(s, "y") || !strcmp(s, "yes"))
                : dflt;
    free(line);
    return r;
}

/* ---------------------------------------------------------------------------
 * Callbacks der Engine
 * ------------------------------------------------------------------------- */
static void cli_print(const char *msg)
{
    char *shown = utf8_escape(msg);
    fputs(shown, stdout);
    fputc('\n', stdout);
    fflush(stdout);
    free(shown);
}

/* printf mit Anzeige ungültiger UTF-8-Bytes als \udcXX (wie Python 5.4) */
__attribute__((format(printf, 1, 2)))
/* printf, ungültiges UTF-8 als \udcXX (wie die Python-Version) */
static void print_escaped(const char *fmt, ...)
{
    char *msg;
    va_list ap;
    va_start(ap, fmt);
    int r = vasprintf(&msg, fmt, ap);
    va_end(ap);
    if (r < 0) return;
    char *shown = utf8_escape(msg);
    fputs(shown, stdout);
    free(shown);
    free(msg);
}

/* Callback: die Suche beginnt (Anzahl Videos, davon aus dem Cache) */
static void cli_detect_start(int videos, int from_cache, int to_read)
{
    printf("Durchsuche %d Videos (%d aus Cache, %d werden gelesen) ...\n", videos, from_cache,
           to_read);
}

/* Callback: Suchfortschritt (eine Zeile alle 25 Dateien) */
static void cli_detect_progress(int done, int total, int workers, int max_workers, double cpu)
{
    if (done % 25 == 0 || done == total)
        printf("  ... Erkennung: %d/%d  (Worker %d/%d, CPU %.0f%%)\n", done, total, workers,
               max_workers, cpu);
}

/* Rückfrage bei wenig Speicherplatz: fertige Dateien schon verschieben? */
static bool cli_ask_disk_move(double free_gb, int n_finished)
{
    (void)free_gb;
    char *q = xasprintf("Dürfen die %d fertigen Dateien an den Platz der Originale "
                        "verschoben werden (Original wird gelöscht)?", n_finished);
    bool r = ask_yes_no(q, true);
    free(q);
    return r;
}

/* Rückfrage bei wenig Speicherplatz: trotzdem weiterlaufen? */
static bool cli_ask_disk_continue(double free_gb)
{
    (void)free_gb;
    return ask_yes_no("Soll das Skript weiterlaufen?", true);
}

/* ---------------------------------------------------------------------------
 * Hauptprogramm
 * ------------------------------------------------------------------------- */
static int compare_methods(const void *a, const void *b)
{
    return strcmp(((const MethodCount *)a)->method, ((const MethodCount *)b)->method);
}

/* Zusammenfassung am Ende (Text wie in der Python-Version) */
static void print_summary(const IcRun *run, double detect_secs, double process_secs,
                          const char *out, bool delete_orig, const char *mode)
{
    const Stats *stats = &run->stats;
    char detect_text[64], process_text[64], cut_text[64];
    printf("\n%s\n", LINE);
    printf(" Fertig: %d bearbeitet, %d Fehler, %d Originale ersetzt.\n", stats->ok, stats->err,
           stats->replaced);
    if (stats->move_err)
        printf(" %d Datei(en) konnten nicht an den Platz der Originale verschoben werden "
               "(Originale unverändert, siehe Log).\n", stats->move_err);
    if (stats->aborted) printf(" Lauf wurde vorzeitig beendet.\n");
    if (stats->method_count) {
        MethodCount sorted[32];
        memcpy(sorted, stats->methods, sizeof sorted);
        qsort(sorted, (size_t)stats->method_count, sizeof *sorted, compare_methods);
        printf(" Methoden: ");
        for (int i = 0; i < stats->method_count; i++)
            printf("%s%s: %d", i ? ", " : "", sorted[i].method, sorted[i].files);
        printf("\n");
    }
    printf(" Erkennung:    %s\n", format_duration(detect_secs, detect_text, sizeof detect_text));
    printf(" Bearbeitung:  %s\n", format_duration(process_secs, process_text, sizeof process_text));
    printf(" Insgesamt abgeschnitten: %.1f s (%s)\n", stats->total_cut,
           format_duration(stats->total_cut, cut_text, sizeof cut_text));
    printf(" Eingesparte/abgeschnittene Datenmenge: %.1f MB",
           (double)stats->saved_bytes / (1024.0 * 1024.0));
    if (llabs(stats->saved_bytes) >= 1024LL * 1024 * 1024)
        printf(" (%.2f GB)", (double)stats->saved_bytes / (1024.0 * 1024.0 * 1024.0));
    printf("\n");
    if (!delete_orig) print_escaped(" Ausgabe: %s\n", out);
    print_escaped(" Ablauf-Log: %s\n", RUN_LOG);
    printf("%s\n", LINE);
    Buf methods_dict = {0};                         /* wie Pythons dict-Darstellung */
    buf_add_char(&methods_dict, '{');
    for (int i = 0; i < stats->method_count; i++)
        buf_printf(&methods_dict, "%s'%s': %d", i ? ", " : "", stats->methods[i].method,
                   stats->methods[i].files);
    buf_add_char(&methods_dict, '}');
    ic_log(false, "Fertig: %d ok, %d Fehler, Methoden %s, Bearbeitung %s", stats->ok, stats->err,
           buf_str(&methods_dict), process_text);
    buf_free(&methods_dict);
    if (!strcmp(mode, "copy") && stats->trimmed_files) {
        printf("Hinweis: Bei %d Video(s) wurde über das Intro hinaus geschnitten "
               "(max. %.1fs),\n", stats->trimmed_files, stats->max_trim);
        printf("weil kein Keyframe am Intro-Ende lag. Für framegenaue Schnitte das "
               "Skript mit Schnittmodus 2, 3 oder 4 starten.\n");
    }
    if (!strcmp(mode, "copy") && stats->rest_files) {
        printf("Hinweis: Bei %d Video(s) blieb ein Rest des Intros stehen (max. %.1fs),\n",
               stats->rest_files, stats->max_rest);
        printf("weil kein Keyframe genau am Intro-Ende lag (Copy schneidet nie in den Inhalt).\n"
               "Für bildgenaue Schnitte Schnittmodus 2, 3 oder 4 wählen.\n");
    }
}

/* Ergebnis der automatischen Erkennung ausgeben: je Intro Länge, Anzahl und ein Beispiel,
 * dazu die unsicheren Videos mit Grund -> Anzahl der sicher erkannten (nur sie werden
 * geschnitten) */
static int print_auto_result(const IcAuto *found)
{
    int sure = 0, unsure = 0;
    for (int v = 0; v < found->videos.n; v++) {
        if (found->info[v].cut_len <= 0) continue;
        found->info[v].sure ? sure++ : unsure++;
    }
    char duration_text[64];
    printf("\n%s\n", LINE);
    if (found->ngroups)
        printf(" %d Intro%s in %d Videos gefunden: %d sicher erkannt, %d unsicher, %d ohne "
               "Intro.\n", found->ngroups, found->ngroups == 1 ? "" : "s", sure + unsure, sure,
               unsure, found->videos.n - sure - unsure);
    else
        printf(" In den %d Videos kein wiederkehrendes Intro gefunden.\n", found->videos.n);
    if (found->unreadable) printf(" %d Datei(en) waren nicht lesbar.\n", found->unreadable);
    printf(" Erkennung dauerte %s.\n",
           format_duration(found->secs, duration_text, sizeof duration_text));
    printf("%s\n", LINE);
    for (int g = 0; g < found->ngroups; g++) {
        const IcAutoGroup *group = &found->groups[g];
        const IcAutoVideo *example = &found->info[group->representative];
        printf("\nIntro %d: etwa %.1f s, %d Videos (%d sicher)\n", g + 1, group->length,
               group->members, group->sure);
        print_escaped("  Beispiel: %s (Schnitt bei %.3f s)\n",
                      found->videos.v[group->representative].rel, example->cut_len);
    }
    if (unsure) {
        printf("\nUnsicher – werden NICHT geschnitten (in der grafischen Oberfläche prüfbar):\n");
        int shown = 0;
        for (int v = 0; v < found->videos.n && shown < 10; v++) {
            const IcAutoVideo *info = &found->info[v];
            if (info->cut_len <= 0 || info->sure) continue;
            print_escaped("  %s: %s\n", found->videos.v[v].rel, info->reason);
            shown++;
        }
        if (unsure > shown)
            print_escaped("  ... und %d weitere (vollständige Liste: %s)\n", unsure - shown,
                          AUTO_LOG_FILE);
    }
    return sure;
}

/* der interaktive Ablauf: fragen, suchen, bestätigen, schneiden -> Exit-Code */
static int run_interactive(void)
{
    ic_log_open();
    printf("%s\n Intro-Cutter %s\n%s\n", LINE, IC_VERSION, LINE);

    char *ref = NULL, *src = NULL, *out = NULL;
    double cut_len = 0, process_secs = 0;
    const char *exit_msg = NULL, *mode = "copy";
    int exit_rc = EXIT_ERROR;                       /* Exit-Code zu exit_msg */
    bool processed = false, delete_orig = false;
    IcDetect det;
    IcAuto found;                                   /* --auto: Ergebnis der Erkennung */
    IcRun run;
    memset(&det, 0, sizeof det);
    memset(&found, 0, sizeof found);
    memset(&run, 0, sizeof run);
    const Video **selected = NULL;                  /* zu schneidende Videos */
    int nselected = 0;

    if (g_auto) {                                   /* ohne Referenz-Video und Länge */
        if (!(src = ask_dir("Quellordner (wird inkl. Unterordner durchsucht)")) ||
            !(out = ask_output(src)))
            goto finish;
    } else if (!(ref = ask_file(g_outro ? "Referenz-Video mit dem Outro"
                                        : "Referenz-Video mit dem Intro")) ||
               !ask_float(g_outro ? "Länge des Outros in Sekunden"
                                  : "Länge des Intros in Sekunden", &cut_len) ||
               !(src = ask_dir("Quellordner (wird inkl. Unterordner durchsucht)")) ||
               !(out = ask_output(src))) {
        goto finish;
    }
    char *real_out = realpath_lenient(out), *real_src = realpath_lenient(src);
    bool same_dir = !strcmp(real_out, real_src);
    free(real_out);
    free(real_src);
    if (same_dir) {
        exit_msg = "Ausgabe- und Quellordner dürfen nicht identisch sein.";
        goto finish;
    }
    int threshold = g_auto ? DEFAULT_THRESHOLD
                           : ask_int("Erkennungs-Schwellwert (Hamming-Distanz, "
                                     "kleiner=strenger)", DEFAULT_THRESHOLD);
    if (ic_cancel) goto finish;

    GpuCaps gpu_caps;
    const GpuCaps *gpu = NULL;
    if (GPU_ENABLE) {
        printf("\nPrüfe GPU (VAAPI) ...\n");
        if (ic_gpu_detect(&gpu_caps)) gpu = &gpu_caps;
        char *gpu_info = ic_gpu_summary(gpu);
        ic_log(true, "  %s\n", gpu_info);
        free(gpu_info);
    }
    if (ic_cancel) goto finish;

    char *copy_option = g_auto
        ? xasprintf("Schnell & verlustfrei (Copy) – Schnitt am letzten Keyframe im Intro: "
                    "nimmt nie Inhalt weg, evtl. bleibt ein Rest des Intros. Liegt im Intro "
                    "kein Keyframe oder klappt der Schnitt nicht sauber: Neukodierung (%s).",
                    gpu ? "GPU, sonst CPU" : "CPU")
        : g_outro
        ? xasprintf("Schnell & verlustfrei (Copy) – Schnitt am Ende ohne Neukodierung, bis auf "
                    "wenige Frames genau. Klappt der Schnitt nicht sauber: Neukodierung (%s).",
                    gpu ? "GPU, sonst CPU" : "CPU")
        : xasprintf("Schnell & verlustfrei (Copy) – Schnitt am ersten Keyframe nach dem "
                    "Intro. Liegt dort kein Keyframe, wird etwas Inhalt direkt nach dem "
                    "Intro mitentfernt. Klappt der Schnitt nicht sauber: Neukodierung (%s).",
                    gpu ? "GPU, sonst CPU" : "CPU");
    const char *options[4] = {
        copy_option,
        "Framegenau per CPU (x264) – exakter Schnitt, kein Inhaltsverlust, langsam.",
        "Framegenau per GPU (VAAPI, Codec wie die Quelle) – exakt, schnell, CPU bleibt frei; "
        "CPU springt nur bei Fehlern ein.",
        "Framegenau per GPU + CPU parallel – maximales Tempo: größte Dateien auf die GPU "
        "(Codec wie Quelle), kleinste parallel auf die CPU (x264). Ergebnis: gemischte Codecs; "
        "der PC ist dabei voll ausgelastet.",
    };
    int choice = ask_choice("Schnittmodus", options, gpu ? 4 : 2, 1);
    free(copy_option);
    if (ic_cancel) goto finish;
    static const char *const MODES[4] = {"copy", "cpu", "gpu", "hybrid"};
    mode = MODES[choice - 1];

    char ts[64];
    ic_ref_timestamp(cut_len, ts, sizeof ts);
    int n_frames = ic_frames_for(cut_len, g_frames);
    if (g_auto) {
        printf("\nGesucht werden Intros bis %.0f s, die in mindestens %d Videos vorkommen.\n",
               g_auto_max, g_auto_min);
    } else if (n_frames > 1) {                      /* --bilder: Liste der Zeitpunkte */
        char frame_times[IC_MAX_FRAMES][24];
        ic_ref_timestamps(cut_len, n_frames, frame_times);
        Buf times_text = {0};
        for (int j = 0; j < n_frames; j++)
            buf_printf(&times_text, "%s%s", j ? ", " : "", frame_times[j]);
        printf(g_outro ? "\n%d Vergleichsbilder je Video, %s s vor dem Ende (Median der "
                         "Distanzen).\n"
                       : "\n%d Vergleichsbilder je Video bei Sekunde %s (Median der "
                         "Distanzen).\n", n_frames, buf_str(&times_text));
        buf_free(&times_text);
    } else if (g_outro) {
        printf("\nReferenz-Frame wird %s s vor dem Ende verglichen.\n", ts);
    } else {
        printf("\nReferenz-Frame wird bei Sekunde %s verglichen.\n", ts);
    }
    printf("CPU-Ziel %d %% (dynamisch), GPU-Jobs parallel: %d.\n\n", CPU_TARGET,
           gpu ? GPU_WORKERS : 0);

    /* ----------------------------- Erkennung ----------------------------- */
    const char *err = NULL;
    if (g_auto) {
        if (ic_detect_auto(src, out, g_auto_max, g_auto_min, &found, &err) != 0) {
            exit_msg = err;                         /* NULL bei Abbruch */
            goto finish;
        }
        int sure = print_auto_result(&found);
        if (!sure) {
            exit_msg = "\nNichts sicher erkannt – nichts zu tun. Ggf. die Suchlänge (--max) "
                       "erhöhen oder die unsicheren Videos in der grafischen Oberfläche prüfen.";
            exit_rc = EXIT_OK;
            goto finish;
        }
        selected = xmalloc((size_t)sure * sizeof *selected);
        for (int v = 0; v < found.videos.n; v++)    /* nur sichere, je Video eigener Schnitt */
            if (found.info[v].cut_len > 0 && found.info[v].sure) {
                found.videos.v[v].cut_len = found.info[v].cut_len;
                selected[nselected++] = &found.videos.v[v];
            }
        goto confirm;
    }
    int detect_rc = ic_detect(ref, cut_len, src, out, threshold, g_frames, g_outro, &det, &err);
    if (detect_rc != 0) {
        exit_msg = err;                             /* NULL bei Abbruch */
        goto finish;
    }
    char duration_text[64];
    printf("\n%s\n", LINE);
    printf(" %d von %d Videos enthalten das %s.\n", det.nmatch, det.videos.n,
           g_outro ? "Outro" : "Intro");
    if (det.skipped)
        printf(" %d Datei(en) ohne lesbaren Frame (zu kurz/defekt) übersprungen.\n", det.skipped);
    printf(" Erkennung dauerte %s.\n",
           format_duration(det.secs, duration_text, sizeof duration_text));
    printf("%s\n", LINE);
    if (det.nmatch) {
        printf("\nTreffer (Distanz : Datei) – unsicherste zuerst:\n");
        for (int i = det.nmatch - 1; i >= 0 && i >= det.nmatch - 15; i--)
            print_escaped("  %3d : %s\n", det.results[i].dist, det.results[i].v->rel);
        if (det.nmatch > 15)
            print_escaped("  ... und %d weitere (vollständige Liste: %s)\n",
                          det.nmatch - 15, LOG_FILE);
    }
    if (det.nmatch < det.nres) {
        printf("\nKnapp NICHT erkannt (prüfen, ob echte %s fehlen):\n",
               g_outro ? "Outros" : "Intros");
        for (int i = det.nmatch; i < det.nres && i < det.nmatch + 10; i++)
            print_escaped("  %3d : %s\n", det.results[i].dist, det.results[i].v->rel);
    }
    if (!det.nmatch) {
        exit_msg = "\nNichts zu tun. Ggf. Schwellwert erhöhen und erneut starten.";
        exit_rc = EXIT_OK;
        goto finish;
    }
    selected = xmalloc((size_t)det.nmatch * sizeof *selected);
    for (int i = 0; i < det.nmatch; i++) selected[nselected++] = det.results[i].v;
confirm:
    printf("\n");
    char *question = g_auto ? xasprintf("Diese %d sicher erkannten Videos jetzt bearbeiten?",
                                        nselected)
                            : xasprintf("Diese %d Videos jetzt bearbeiten?", nselected);
    bool proceed = ask_yes_no(question, false);
    free(question);
    if (ic_cancel) goto finish;
    if (!proceed) {
        exit_msg = "Abgebrochen.";
        exit_rc = EXIT_ABORT;
        goto finish;
    }
    delete_orig = ask_yes_no("Originale ERSETZEN? (Schnitt wird an den Platz der "
                             "Originaldatei verschoben, Original gelöscht; Ausgabeordner "
                             "wird dann nicht genutzt)", false);
    if (ic_cancel) goto finish;

    /* ---------------------------- Verarbeitung ---------------------------- */
    printf("\nVerarbeite ...\n\n");
    double process_start = monotonic_secs();
    /* --auto: cut_len 0 -> je Video der eigene Schnitt */
    processed = ic_process(selected, nselected, out, cut_len, g_outro, mode, delete_orig, gpu,
                           &run);
    /* --- Keep-Modus: am Ende restliche fertige Dateien anbieten --- */
    if (processed && !delete_orig && run.fin_dest.n) {
        printf("\n");
        char *move_question = xasprintf("Sollen die %d fertigen Dateien an den Platz der Originale "
                                        "verschoben werden (Originale werden gelöscht)?",
                                        run.fin_dest.n);
        if (ask_yes_no(move_question, false)) {
            int moved = ic_move_finished(&run);
            ic_log(true, "  %d Datei(en) verschoben.", moved);
            if (run.dirs_removed)
                ic_log(true, "  %d leere Ausgabeordner entfernt.", run.dirs_removed);
            if (run.fin_dest.n)
                ic_log(true, "  %d Datei(en) konnten nicht verschoben werden – sie liegen weiter "
                       "im Ausgabeordner, die Originale sind unverändert.", run.fin_dest.n);
        }
        free(move_question);
        processed = !ic_cancel;
    }
    process_secs = monotonic_secs() - process_start;

finish:
    {
        int removed = ic_cleanup_temps();
        if (removed) printf("\n%d temporäre Datei(en) aufgeräumt.\n", removed);
    }
    int rc;
    const Stats *stats = &run.stats;
    if (ic_cancel) {
        rc = EXIT_ABORT;
        printf("\nAbgebrochen.\n");
    } else if (g_eof && !exit_msg) {                /* Pflichtangabe fehlt */
        rc = EXIT_ERROR;
        printf("\nEingabe unerwartet beendet – Abbruch.\n");
    } else if (exit_msg) {
        rc = exit_rc;
        printf("%s\n", exit_msg);
    } else if (!processed) {                        /* z. B. Ausgabeordner nicht anlegbar */
        rc = EXIT_ERROR;
    } else {
        print_summary(&run, g_auto ? found.secs : det.secs, process_secs, out, delete_orig,
                      mode);
        rc = stats->aborted ? EXIT_ABORT : (stats->err || stats->move_err) ? EXIT_ERROR : EXIT_OK;
    }
    ic_run_free(&run);
    ic_detect_free(&det);
    ic_auto_free(&found);
    free(selected);
    free(ref);
    free(src);
    free(out);
    return rc;
}

/* Aufrufhilfe ausgeben */
static void print_usage(FILE *f)
{
    fprintf(f, "Aufruf: intro-cutter-cli [--outro] [--bilder N]\n"
               "       intro-cutter-cli --auto [--max S] [--mindestens N]\n"
               "  (ohne Option)  wiederkehrendes Intro am Anfang der Videos entfernen\n"
               "  --outro        stattdessen das Outro am Ende entfernen: die angegebene\n"
               "                 Länge wird am Ende der erkannten Videos abgeschnitten\n"
               "  --bilder N     gründlichere Erkennung: N Vergleichsbilder (3, 5 oder 9)\n"
               "                 über das ganze Intro statt eines – weniger Fehltreffer und\n"
               "                 übersehene Intros, dafür ca. N-fache Rechenzeit\n"
               "  --auto         Intros ohne Referenz-Video automatisch erkennen – auch\n"
               "                 mehrere verschiedene; je Video ein eigener, bildgenauer\n"
               "                 Schnitt. Geschnitten werden nur sicher erkannte Videos\n"
               "  --max S        dabei Intros bis S Sekunden suchen (3–120, Standard %d)\n"
               "  --mindestens N ein Intro muss in mindestens N Videos vorkommen\n"
               "                 (2–100, Standard %d)\n"
               "  Pausieren mit Strg+Z, fortsetzen mit fg.\n",
            IC_AUTO_MAX_DEFAULT, IC_AUTO_MIN_VIDEOS);
}

/* Einstieg: Optionen lesen, Engine einrichten, interaktiven Ablauf starten */
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--outro")) {
            g_outro = true;
        } else if (!strcmp(argv[i], "--bilder") && i + 1 < argc) {
            long n;
            if (!parse_int(argv[++i], &n) || (n != 1 && n != 3 && n != 5 && n != 9)) {
                fprintf(stderr, "--bilder erwartet 1, 3, 5 oder 9\n");
                return EXIT_ERROR;
            }
            g_frames = (int)n;
        } else if (!strcmp(argv[i], "--auto")) {
            g_auto = true;
        } else if (!strcmp(argv[i], "--max") && i + 1 < argc) {
            double v;
            char *text = argv[++i];
            for (char *c = text; *c; c++)
                if (*c == ',') *c = '.';
            if (!parse_float(text, &v) || !(v >= 3 && v <= 120)) {
                fprintf(stderr, "--max erwartet 3 bis 120 (Sekunden)\n");
                return EXIT_ERROR;
            }
            g_auto_max = v;
        } else if (!strcmp(argv[i], "--mindestens") && i + 1 < argc) {
            long n;
            if (!parse_int(argv[++i], &n) || n < 2 || n > 100) {
                fprintf(stderr, "--mindestens erwartet 2 bis 100\n");
                return EXIT_ERROR;
            }
            g_auto_min = (int)n;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_usage(stdout);
            return EXIT_OK;
        } else {
            fprintf(stderr, "Unbekannte Option: %s\n", argv[i]);
            print_usage(stderr);
            return EXIT_ERROR;
        }
    }
    bool auto_options = g_auto_max != IC_AUTO_MAX_DEFAULT || g_auto_min != IC_AUTO_MIN_VIDEOS;
    if (g_auto && (g_outro || g_frames != 1)) {
        fprintf(stderr, "--auto lässt sich nicht mit --outro oder --bilder kombinieren\n");
        return EXIT_ERROR;
    }
    if (!g_auto && auto_options) {
        fprintf(stderr, "--max und --mindestens gelten nur zusammen mit --auto\n");
        return EXIT_ERROR;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    tzset();
    install_signals();
    ic_init();
    ic_fe.log = cli_print;
    ic_fe.info = cli_print;
    ic_fe.show_heartbeat = true;
    ic_fe.detect_start = cli_detect_start;
    ic_fe.detect_progress = cli_detect_progress;
    ic_fe.ask_disk_move = cli_ask_disk_move;
    ic_fe.ask_disk_continue = cli_ask_disk_continue;
    /* Bootstrap: fehlt ffmpeg, wird es installiert */
    ic_setup_dependencies();
    int rc = run_interactive();
    ic_cleanup_temps();
    if (isatty(STDIN_FILENO)) free(read_input_line("\nEnter drücken zum Beenden ..."));
    return rc;
}
