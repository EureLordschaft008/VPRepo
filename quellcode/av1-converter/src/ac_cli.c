/*
 * AV1-Converter – Terminal-Version
 * ================================
 * Wandelt alle Videos eines Ordners (inkl. Unterordner) nach AV1 um – mit den
 * Einstellungen des HandBrake-Profils „AV1 MKV 2160p60 4K“. Gut für lange Läufe
 * per SSH oder in einer tmux-Sitzung.
 *
 * Aufruf:
 *     av1-converter-cli [Optionen] QUELLORDNER
 *
 * Exit-Codes: 0 = Erfolg, 1 = Fehler bei mindestens einer Datei, 2 = Aufruffehler,
 * 130 = abgebrochen. Strg+C bricht ab (fertige Dateien bleiben), Strg+Z pausiert
 * (weiter mit "fg").
 */
#define _GNU_SOURCE
#include <glib.h>
#include <glib-unix.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ac_core.h"

static const char USAGE[] =
    "Aufruf: av1-converter-cli [Optionen] QUELLORDNER\n"
    "        av1-converter-cli --verschieben\n"
    "\n"
    "Wandelt alle Videos im Ordner (inkl. Unterordner) nach AV1 um – Einstellungen wie das\n"
    "HandBrake-Profil „" AC_PRESET "“. Bereits bearbeitete Dateien werden übersprungen.\n"
    "\n"
    "  --modus cpu|gpu|hybrid   CPU (HandBrake), GPU (VAAPI) oder beides (Standard: gpu)\n"
    "  --ersetzen               Originale ersetzen (sonst Ausgabeordner)\n"
    "  --ausgabe ORDNER         Ausgabeordner (Standard: QUELLORDNER_av1)\n"
    "  --cpu-jobs N             parallele CPU-Jobs (Standard: " G_STRINGIFY(DEFAULT_CPU_JOBS) ")\n"
    "  --gpu-jobs N             parallele GPU-Jobs (Standard: " G_STRINGIFY(DEFAULT_GPU_JOBS) ")\n"
    "  --rf ZAHL                Qualität der CPU (HandBrake-RF, Standard: wie im Profil)\n"
    "  --gpu-qualitaet ZAHL     Qualität der GPU, 0–255, kleiner = besser (Standard: "
    G_STRINGIFY(DEFAULT_GPU_QUALITY) ")\n"
    "  --ton profil|alle|kopie  erste Spur als Opus (Profil), alle als Opus, alle unverändert\n"
    "  --untertitel profil|alle keine (Profil) oder alle übernehmen\n"
    "  --mindestersparnis P     Original behalten, wenn AV1 nicht P %% kleiner ist (Standard: "
    G_STRINGIFY(DEFAULT_MIN_SAVING) ")\n"
    "  --datum-neu              Änderungsdatum nicht vom Original übernehmen\n"
    "  --hdr container|handbrake|ohne  HDR-Videos: per GPU mit HDR-Angaben im MKV-Container\n"
    "                           (Standard, braucht mkvtoolnix; Dolby Vision/HDR10+: HandBrake),\n"
    "                           vollständig per HandBrake (langsam) oder per GPU ohne Angaben\n"
    "  --nur-anzeigen           nur suchen und auflisten, nichts umwandeln\n"
    "  --verschieben            fertige Ergebnisse aus Ausgabeordnern an den Platz ihrer\n"
    "                           Originale verschieben (Originale werden gelöscht) – geht\n"
    "                           jederzeit nach einer Umwandlung ohne --ersetzen\n"
    "  -h, --help               diese Hilfe\n";

static bool g_paused_by_tstp;

/* Strg+C / SIGTERM: geordnet abbrechen */
static void on_cancel_signal(int sig)
{
    (void)sig;
    if (ac_cancel) _exit(130);                  /* zweites Strg+C: sofort */
    ac_cancel = 1;
    static const char msg[] = "\nWird abgebrochen … (noch einmal Strg+C: sofort beenden)\n";
    if (write(2, msg, sizeof msg - 1)) {}
}

/* Strg+Z: Encoder anhalten, dann selbst anhalten; nach "fg" weiter */
static void on_tstp(int sig)
{
    (void)sig;
    g_paused_by_tstp = true;
}

static void on_cont(int sig)
{
    (void)sig;
    g_paused_by_tstp = false;
}

static gpointer pause_watcher(gpointer d)
{
    (void)d;
    while (true) {
        if (g_paused_by_tstp && !ac_paused()) {
            ac_set_paused(true);
            fprintf(stderr, "\nPausiert – weiter mit \"fg\".\n");
            raise(SIGSTOP);
        } else if (!g_paused_by_tstp && ac_paused()) {
            ac_set_paused(false);
            fprintf(stderr, "Fortgesetzt.\n");
        }
        g_usleep(200 * 1000);
    }
    return NULL;
}

static int last_scan_line;

static void cli_scan_progress(int found, int probed)
{
    if (!isatty(1)) return;
    if (probed < 0) printf("\r  %d Videos gefunden – Ordner werden gelesen …   ", found);
    else printf("\r  %d Videos gefunden, %d geprüft …                ", found, probed);
    fflush(stdout);
    last_scan_line = 1;
}

static void cli_log(const char *msg)
{
    if (last_scan_line) {
        putchar('\n');
        last_scan_line = 0;
    }
    puts(msg);
    fflush(stdout);
}

/* Statuszeile alle 60 s: Fortschritt, Ersparnis, Restzeit */
static gpointer status_thread(gpointer d)
{
    volatile bool *stop = d;
    while (!*stop) {
        for (int i = 0; i < 600 && !*stop; i++) g_usleep(100 * 1000);
        AcProgress p;
        if (*stop || !ac_progress(&p)) continue;
        char done[32], total[32], saved[32], eta[32], finish[48];
        double rest = p.eta;
        double cpu = 0;
        bool have_cpu = ac_cpu_load(&cpu);
        printf("  … %d/%d Dateien · %s von %s · gespart %s · Restdauer %s (fertig %s)%s%.0f %%\n",
               p.files_done, p.files_total, ac_format_bytes(p.bytes_done, done, sizeof done),
               ac_format_bytes(p.bytes_total, total, sizeof total),
               ac_format_bytes((double)p.saved, saved, sizeof saved),
               ac_format_remaining(rest, eta, sizeof eta), ac_format_finish(rest, finish, sizeof finish),
               have_cpu ? " · CPU " : "", have_cpu ? cpu : 0.0);
        fflush(stdout);
    }
    return NULL;
}

/* --verschieben: alle offenen Ergebnisse an den Platz der Originale */
static int move_to_originals(void)
{
    struct sigaction sa = {.sa_handler = on_cancel_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    AcPending p;
    if (!ac_pending_info(&p)) {
        printf("Nichts zu verschieben – es liegen keine Ergebnisse im Ausgabeordner, deren "
               "Originale noch ersetzt werden könnten.\n");
        return 0;
    }
    ac_log_open();
    AcMoveStats ms;
    bool completed = ac_move_to_originals(&ms);
    printf("Lauf-Protokoll: %s\n", AC_RUN_LOG);
    return !completed ? 130 : ms.failed ? 1 : 0;
}

static bool parse_int_arg(const char *s, int lo, int hi, int *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (*end || v < lo || v > hi) return false;
    *out = (int)v;
    return true;
}

int main(int argc, char **argv)
{
    ac_init();
    ac_fe.log = cli_log;
    ac_fe.scan_progress = cli_scan_progress;
    AcOptions o;
    ac_options_default(&o);
    const char *src = NULL;
    char *out_dir = NULL;
    bool list_only = false, rf_set = false, move_only = false;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *next = i + 1 < argc ? argv[i + 1] : NULL;
        bool bad = false;
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(USAGE, stdout);
            return 0;
        } else if (!strcmp(a, "--modus") && next) {
            i++;
            if (!strcmp(next, "cpu")) o.mode = AC_MODE_CPU;
            else if (!strcmp(next, "gpu")) o.mode = AC_MODE_GPU;
            else if (!strcmp(next, "hybrid")) o.mode = AC_MODE_HYBRID;
            else bad = true;
        } else if (!strcmp(a, "--ersetzen")) {
            o.replace = true;
        } else if (!strcmp(a, "--ausgabe") && next) {
            out_dir = g_canonicalize_filename(argv[++i], NULL);
        } else if (!strcmp(a, "--cpu-jobs") && next) {
            bad = !parse_int_arg(argv[++i], 1, MAX_CPU_JOBS, &o.cpu_jobs);
        } else if (!strcmp(a, "--gpu-jobs") && next) {
            bad = !parse_int_arg(argv[++i], 1, MAX_GPU_JOBS, &o.gpu_jobs);
        } else if (!strcmp(a, "--rf") && next) {
            o.rf = g_ascii_strtod(argv[++i], NULL);
            bad = o.rf < 1 || o.rf > 63;
            rf_set = true;
        } else if (!strcmp(a, "--gpu-qualitaet") && next) {
            bad = !parse_int_arg(argv[++i], 1, 255, &o.gpu_quality);
        } else if (!strcmp(a, "--ton") && next) {
            i++;
            if (!strcmp(next, "profil")) o.audio = AC_AUDIO_PRESET;
            else if (!strcmp(next, "alle")) o.audio = AC_AUDIO_ALL_OPUS;
            else if (!strcmp(next, "kopie")) o.audio = AC_AUDIO_ALL_COPY;
            else bad = true;
        } else if (!strcmp(a, "--untertitel") && next) {
            i++;
            if (!strcmp(next, "profil")) o.subs = AC_SUBS_PRESET;
            else if (!strcmp(next, "alle")) o.subs = AC_SUBS_ALL;
            else bad = true;
        } else if (!strcmp(a, "--mindestersparnis") && next) {
            bad = !parse_int_arg(argv[++i], 0, 90, &o.min_saving);
        } else if (!strcmp(a, "--datum-neu")) {
            o.keep_mtime = false;
        } else if (!strcmp(a, "--hdr") && next) {
            i++;
            if (!strcmp(next, "container")) o.hdr = AC_HDR_GPU_CONTAINER;
            else if (!strcmp(next, "handbrake")) o.hdr = AC_HDR_HANDBRAKE;
            else if (!strcmp(next, "ohne")) o.hdr = AC_HDR_GPU_PLAIN;
            else bad = true;
        } else if (!strcmp(a, "--verschieben")) {
            move_only = true;
        } else if (!strcmp(a, "--nur-anzeigen")) {
            list_only = true;
        } else if (a[0] != '-' && !src) {
            src = a;
        } else {
            bad = true;
        }
        if (bad) {
            fprintf(stderr, "Ungültige Angabe: %s\n\n%s", a, USAGE);
            return 2;
        }
    }
    if (move_only) return move_to_originals();
    if (!src) {
        fputs(USAGE, stderr);
        return 2;
    }
    if (!ac_have_ffmpeg()) {
        fprintf(stderr, "ffmpeg/ffprobe fehlen – bitte installieren: sudo apt install ffmpeg\n");
        return 2;
    }
    char *src_abs = g_canonicalize_filename(src, NULL);
    while (strlen(src_abs) > 1 && g_str_has_suffix(src_abs, "/")) src_abs[strlen(src_abs) - 1] = 0;
    if (!o.replace) {
        if (!out_dir) out_dir = g_strconcat(src_abs, "_av1", NULL);
        if (!strcmp(out_dir, src_abs)) {
            fprintf(stderr, "Ausgabe- und Quellordner dürfen nicht identisch sein.\n");
            return 2;
        }
        o.out_dir = out_dir;
    }

    struct sigaction sa = {.sa_handler = on_cancel_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sa.sa_handler = on_tstp;
    sigaction(SIGTSTP, &sa, NULL);
    sa.sa_handler = on_cont;
    sigaction(SIGCONT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    g_thread_unref(g_thread_new("pause", pause_watcher, NULL));

    ac_log_open();
    printf("AV1-Converter %s – Profil „%s“\n", AC_VERSION, AC_PRESET);
    AcHandbrake hb;
    AcGpu gpu;
    if (o.mode != AC_MODE_GPU) {
        if (ac_handbrake_detect(&hb)) {
            printf("  CPU: HandBrake %s%s – SVT-AV1 Preset %s, RF %.1f\n", hb.version,
                   hb.flatpak ? " (Flatpak)" : "", hb.svt_preset, rf_set ? o.rf : hb.rf);
            if (!rf_set) o.rf = hb.rf;
        } else {
            printf("  CPU: HandBrake nicht gefunden – ffmpeg/libsvtav1 mit gleichwertigen Werten\n");
        }
    } else {
        memset(&hb, 0, sizeof hb);
        ac_handbrake_detect(&hb);               /* nur für den Zuschnitt (Scan) */
    }
    if (o.mode != AC_MODE_CPU) {
        if (ac_gpu_detect(&gpu))
            printf("  GPU: %s (%s) – VAAPI AV1, Qualität %d\n", gpu.name, gpu.device, o.gpu_quality);
        else
            printf("  GPU: keine mit AV1-Encoder gefunden\n");
    } else {
        memset(&gpu, 0, sizeof gpu);
    }
    printf("Suche in %s …\n", src_abs);

    AcScan scan;
    const char *err = NULL;
    int rc = ac_scan(src_abs, &o, &scan, &err);
    if (last_scan_line) putchar('\n');
    if (rc == -1) {
        fprintf(stderr, "%s\n", err);
        return 2;
    }
    if (rc == 1) {
        ac_scan_free(&scan);
        return 130;
    }
    static const char *const state_text[AC_STATE_COUNT] = {
        "umzuwandeln", "schon AV1", "früher umgewandelt", "früher ohne Gewinn", "früher Fehler",
        "schon im Ausgabeordner", "nicht lesbar"};
    for (int s = 0; s < AC_STATE_COUNT; s++) {
        if (!scan.count[s]) continue;
        char b[32];
        printf("  %-24s %6d  (%s)\n", state_text[s], scan.count[s],
               ac_format_bytes((double)scan.bytes[s], b, sizeof b));
    }
    AcVideo **sel = g_new(AcVideo *, MAX(scan.n, 1));
    int n = 0;
    double est = 0;
    for (int i = 0; i < scan.n; i++)
        if (scan.v[i].state == AC_TODO) {
            sel[n++] = &scan.v[i];
            est += scan.v[i].size * ac_estimated_saving(&scan.v[i]);
        }
    if (n) {
        char b[32], d[32];
        printf("  Geschätzte Ersparnis: ca. %s · geschätzte Dauer: ca. %s\n",
               ac_format_bytes(est, b, sizeof b),
               ac_format_duration(ac_estimate_total(sel, n, &o, gpu.available), d, sizeof d));
    }
    if (list_only) {
        for (int i = 0; i < n; i++) {
            char b[32];
            printf("  %10s  %-6s %5dx%-5d %s\n", ac_format_bytes((double)sel[i]->size, b, sizeof b),
                   ac_codec_name(sel[i]->vcodec), sel[i]->width, sel[i]->height, sel[i]->rel);
        }
        g_free(sel);
        ac_scan_free(&scan);
        return 0;
    }
    if (!n) {
        printf("Nichts zu tun.\n");
        g_free(sel);
        ac_scan_free(&scan);
        return 0;
    }

    volatile bool stop_status = false;
    GThread *status = g_thread_new("status", status_thread, (gpointer)&stop_status);
    AcStats st;
    bool completed = ac_convert(sel, n, &o, &hb, &gpu, &st);
    stop_status = true;
    g_thread_join(status);
    g_free(sel);
    ac_scan_free(&scan);
    printf("Lauf-Protokoll: %s\n", AC_RUN_LOG);
    AcPending pending;
    if (!o.replace && ac_pending_info(&pending)) {
        char b[32];
        printf("%d Ergebnis(se) liegen im Ausgabeordner. An den Platz der Originale (spart %s):\n"
               "    av1-converter-cli --verschieben\n", pending.files,
               ac_format_bytes((double)(pending.orig_bytes - pending.dest_bytes), b, sizeof b));
    }
    return !completed ? 130 : st.err ? 1 : 0;
}
