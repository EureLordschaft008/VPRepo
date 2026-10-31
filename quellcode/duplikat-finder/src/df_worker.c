/*
 * Duplikat-Finder – Hilfsprozess (Dekodieren und Fingerabdrücke)
 * ===============================================================
 * Läuft als eigener Prozess, gestartet von der Engine (df_core.c). Alles, was
 * defekte Videodateien zum Absturz bringen kann (Demuxer, Decoder, Treiber),
 * passiert hier: stürzt der Prozess ab oder hängt er, startet die Engine einen
 * neuen und markiert nur diese eine Datei als fehlerhaft – das Programm selbst
 * läuft weiter.
 *
 * Ein Prozess bleibt für viele Dateien am Leben (kein Programmstart je Datei).
 * Er dekodiert per CPU (libavcodec, 1 Thread) oder per GPU (VAAPI, Aufruf mit
 * --gpu <Gerät>). Kann die GPU ein Video nicht dekodieren, meldet der Prozess
 * "SW" und die Engine gibt die Datei an einen CPU-Prozess.
 *
 * Protokoll: eine Anfrage je Zeile auf stdin (Felder durch Tabulator getrennt),
 * je Anfrage eine Antwort auf stdout:
 *     R\t<Status>\t<Textlänge>\t<Binärlänge>\n<Text><Binärdaten>
 * Status: OK, ERR (Text = Grund), SW (GPU kann das Video nicht dekodieren).
 * Der Pfad steht immer zuletzt; Tabulator, Zeilenumbruch und Backslash darin
 * kommen maskiert an (\t, \n, \\).
 *
 *   S\t<k>\t<Pfad>          Infos + k Vergleichsbilder (gleichmäßig über die
 *                           Laufzeit verteilt) -> Text key=value, Binär k Bilder
 *   F\t<t1,t2,...>\t<Pfad>  Vergleichsbilder an festen Zeitpunkten (Sekunden)
 *   A\t<max_s>\t<Pfad>      Audio-Fingerabdruck (Chromaprint, roh, je uint32)
 *   T\t<k>\t<Breite>\t<Höhe>\t<Pfad>  k Vorschaubilder (RGB24, ins Feld eingepasst)
 *                           an denselben Stellen wie S
 *   Q                       beenden
 *
 * Vergleichsbild: das Bild wird auf 64x64 Graustufen verkleinert, schwarze
 * (und auf Wunsch weiße) Balken werden abgeschnitten, der Rest auf 32x32
 * gebracht und daraus der pHash (DCT, 64 Bit) berechnet – auch gespiegelt.
 * Dazu Helligkeit und Kontrast, damit einfarbige Bilder (Schwarzbild,
 * Abblende) beim Vergleich nicht zählen.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <chromaprint.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/display.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "df_proto.h"

static AVBufferRef *g_hw;               /* VAAPI-Gerät (nur GPU-Prozess) */
static bool g_white_borders;            /* auch weiße Balken abschneiden */

/* ---------------------------------------------------------------------------
 * Ausgabe
 * ------------------------------------------------------------------------- */
typedef struct {
    char *p;
    size_t n, cap;
} Buf;

static void buf_add(Buf *b, const void *d, size_t n)
{
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap < b->n + n + 1) cap *= 2;
        char *p = realloc(b->p, cap);
        if (!p) abort();
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
    b->p[b->n] = 0;
}

__attribute__((format(printf, 2, 3))) static void buf_printf(Buf *b, const char *fmt, ...)
{
    char tmp[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) buf_add(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}

static void reply(const char *status, const Buf *text, const Buf *bin)
{
    size_t tn = text ? text->n : 0, bn = bin ? bin->n : 0;
    printf("R\t%s\t%zu\t%zu\n", status, tn, bn);
    if (tn) fwrite(text->p, 1, tn, stdout);
    if (bn) fwrite(bin->p, 1, bn, stdout);
    fflush(stdout);
}

static void reply_error(const char *status, const char *msg)
{
    Buf t = {0};
    buf_add(&t, msg, strlen(msg));
    reply(status, &t, NULL);
    free(t.p);
}

static void av_error_text(int err, char *out, size_t sz)
{
    av_strerror(err, out, sz);
}

/* ---------------------------------------------------------------------------
 * pHash (32x32 -> DCT -> 8x8 tiefe Frequenzen -> Bit je Koeffizient > Median)
 * ------------------------------------------------------------------------- */
static double g_dct[8][32];             /* DCT-II-Basis, nur die 8 tiefsten Frequenzen */

static void init_dct(void)
{
    for (int k = 0; k < 8; k++)
        for (int n = 0; n < 32; n++) g_dct[k][n] = cos(M_PI / 32.0 * (n + 0.5) * k);
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static uint64_t phash32(const float *px, bool flip)
{
    double rows[32][8], coef[64], sorted[64];
    for (int y = 0; y < 32; y++)                /* je Zeile: DCT über x */
        for (int k = 0; k < 8; k++) {
            double s = 0;
            for (int x = 0; x < 32; x++) s += px[y * 32 + (flip ? 31 - x : x)] * g_dct[k][x];
            rows[y][k] = s;
        }
    for (int ky = 0; ky < 8; ky++)              /* je Spalte: DCT über y */
        for (int kx = 0; kx < 8; kx++) {
            double s = 0;
            for (int y = 0; y < 32; y++) s += rows[y][kx] * g_dct[ky][y];
            coef[ky * 8 + kx] = s;
        }
    /* Gleichanteil (0,0) nicht mitzählen: er sagt nur die Helligkeit */
    memcpy(sorted, coef + 1, 63 * sizeof *coef);
    qsort(sorted, 63, sizeof *sorted, cmp_double);
    double median = sorted[31];
    uint64_t h = 0;
    for (int i = 0; i < 64; i++) h = h << 1 | (i && coef[i] > median);
    /* Bit 63 (Gleichanteil) bleibt 0 – für beide Videos gleich */
    return h;
}

/* 64x64-Graubild -> Vergleichsbild (Balken ab, 32x32, pHash, Helligkeit) */
static void frame_signature(const uint8_t *g64, DfFrame *out)
{
    enum { N = 64 };
    /* Balken: Zeilen/Spalten am Rand, die fast ganz schwarz (bzw. weiß) sind */
    int top = 0, bottom = N, left = 0, right = N;
#define DARK_LINE(sum, mx) ((sum) < 22 * N && (mx) < 60)
#define WHITE_LINE(sum, mn) (g_white_borders && (sum) > 233 * N && (mn) > 195)
    for (int pass = 0; pass < 2; pass++) {
        while (bottom - top > N / 2) {          /* oben */
            int y = top, sum = 0, mx = 0, mn = 255;
            for (int x = left; x < right; x++) {
                int v = g64[y * N + x];
                sum += v; if (v > mx) mx = v; if (v < mn) mn = v;
            }
            sum = sum * N / (right - left);
            if (!(DARK_LINE(sum, mx) || WHITE_LINE(sum, mn))) break;
            top++;
        }
        while (bottom - top > N / 2) {          /* unten */
            int y = bottom - 1, sum = 0, mx = 0, mn = 255;
            for (int x = left; x < right; x++) {
                int v = g64[y * N + x];
                sum += v; if (v > mx) mx = v; if (v < mn) mn = v;
            }
            sum = sum * N / (right - left);
            if (!(DARK_LINE(sum, mx) || WHITE_LINE(sum, mn))) break;
            bottom--;
        }
        while (right - left > N / 2) {          /* links */
            int x = left, sum = 0, mx = 0, mn = 255;
            for (int y = top; y < bottom; y++) {
                int v = g64[y * N + x];
                sum += v; if (v > mx) mx = v; if (v < mn) mn = v;
            }
            sum = sum * N / (bottom - top);
            if (!(DARK_LINE(sum, mx) || WHITE_LINE(sum, mn))) break;
            left++;
        }
        while (right - left > N / 2) {          /* rechts */
            int x = right - 1, sum = 0, mx = 0, mn = 255;
            for (int y = top; y < bottom; y++) {
                int v = g64[y * N + x];
                sum += v; if (v > mx) mx = v; if (v < mn) mn = v;
            }
            sum = sum * N / (bottom - top);
            if (!(DARK_LINE(sum, mx) || WHITE_LINE(sum, mn))) break;
            right--;
        }
    }
    /* ganz dunkles Bild: keine Balken, sondern Schwarzbild -> alles behalten */
    if (bottom - top <= N / 2 + 1 && right - left <= N / 2 + 1) {
        top = left = 0;
        bottom = right = N;
    }
    /* Ausschnitt bilinear auf 32x32 */
    float px[32 * 32];
    double sx = (double)(right - left) / 32, sy = (double)(bottom - top) / 32;
    double sum = 0, sum2 = 0;
    for (int y = 0; y < 32; y++) {
        double fy = top + (y + 0.5) * sy - 0.5;
        int y0 = (int)floor(fy);
        double wy = fy - y0;
        int y1 = y0 + 1;
        if (y0 < top) y0 = top;
        if (y1 > bottom - 1) y1 = bottom - 1;
        if (y0 > bottom - 1) y0 = bottom - 1;
        for (int x = 0; x < 32; x++) {
            double fx = left + (x + 0.5) * sx - 0.5;
            int x0 = (int)floor(fx);
            double wx = fx - x0;
            int x1 = x0 + 1;
            if (x0 < left) x0 = left;
            if (x1 > right - 1) x1 = right - 1;
            if (x0 > right - 1) x0 = right - 1;
            double v = (1 - wy) * ((1 - wx) * g64[y0 * N + x0] + wx * g64[y0 * N + x1]) +
                       wy * ((1 - wx) * g64[y1 * N + x0] + wx * g64[y1 * N + x1]);
            px[y * 32 + x] = (float)v;
            sum += v;
            sum2 += v * v;
        }
    }
    double mean = sum / 1024, var = sum2 / 1024 - mean * mean;
    double sd = var > 0 ? sqrt(var) : 0;
    memset(out, 0, sizeof *out);
    out->hash = phash32(px, false);
    out->hash_flip = phash32(px, true);
    out->mean = (uint8_t)lrint(fmin(255, mean));
    out->contrast = (uint8_t)lrint(fmin(255, sd));
    out->flags = DF_FRAME_OK | (sd < DF_FLAT_CONTRAST ? DF_FRAME_FLAT : 0);
}

/* ---------------------------------------------------------------------------
 * Video öffnen und dekodieren
 * ------------------------------------------------------------------------- */
typedef struct {
    AVFormatContext *fmt;
    AVCodecContext *dec;
    int vidx, aidx;
    AVStream *vst;
    bool hw;                    /* dekodiert per VAAPI */
    bool hw_failed;             /* GPU kann dieses Video nicht */
    bool is_image;
    AVPacket *pkt;
    AVFrame *frame, *sw;
    struct SwsContext *sws;
    int sws_w, sws_h, sws_fmt, sws_ow, sws_oh, sws_ofmt;
    int64_t last_pts;           /* zuletzt dekodiertes Bild (Stream-Zeitbasis) */
    bool eof;
} Video;

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *fmts)
{
    Video *v = ctx->opaque;
    for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_VAAPI) return *p;
    v->hw_failed = true;                        /* kein VAAPI für dieses Profil */
    return fmts[0];
}

static void video_close(Video *v)
{
    if (v->sws) sws_freeContext(v->sws);
    av_frame_free(&v->frame);
    av_frame_free(&v->sw);
    av_packet_free(&v->pkt);
    avcodec_free_context(&v->dec);
    avformat_close_input(&v->fmt);
    memset(v, 0, sizeof *v);
}

static bool is_cover_stream(const AVStream *st)
{
    return (st->disposition & AV_DISPOSITION_ATTACHED_PIC) != 0;
}

/* Datei öffnen, Haupt-Videospur suchen. want_hw: per VAAPI dekodieren.
 * -> 0, sonst Fehlertext in err */
static int video_open(Video *v, const char *path, bool want_hw, bool open_decoder, char *err,
                      size_t errsz)
{
    memset(v, 0, sizeof *v);
    v->vidx = v->aidx = -1;
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "probesize", "20000000", 0);
    av_dict_set(&opts, "analyzeduration", "20000000", 0);
    int r = avformat_open_input(&v->fmt, path, NULL, &opts);
    av_dict_free(&opts);
    if (r < 0) {
        char e[128];
        av_error_text(r, e, sizeof e);
        if (r == AVERROR_INVALIDDATA)
            snprintf(err, errsz, "Datei defekt oder unvollständig (z. B. abgebrochener Download)");
        else
            snprintf(err, errsz, "Datei nicht lesbar: %s", e);
        return -1;
    }
    v->fmt->flags |= AVFMT_FLAG_GENPTS;
    if ((r = avformat_find_stream_info(v->fmt, NULL)) < 0) {
        char e[128];
        av_error_text(r, e, sizeof e);
        snprintf(err, errsz, "Format nicht erkannt: %s", e);
        return -1;
    }
    /* Haupt-Videospur: größte Auflösung, kein Titelbild */
    long best = -1;
    for (unsigned i = 0; i < v->fmt->nb_streams; i++) {
        AVStream *st = v->fmt->streams[i];
        if (st->codecpar->codec_type != AVMEDIA_TYPE_VIDEO || is_cover_stream(st)) continue;
        long px = (long)st->codecpar->width * st->codecpar->height;
        if (px > best) {
            best = px;
            v->vidx = (int)i;
        }
    }
    const char *iname = v->fmt->iformat ? v->fmt->iformat->name : "";
    v->is_image = strstr(iname, "_pipe") || !strcmp(iname, "image2") || !strcmp(iname, "gif") ||
                  !strcmp(iname, "webp_pipe");
    if (v->vidx < 0) {                          /* nur Titelbild (z. B. Musik) -> trotzdem nicht */
        snprintf(err, errsz, "keine Videospur");
        return -1;
    }
    v->aidx = av_find_best_stream(v->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    v->vst = v->fmt->streams[v->vidx];
    if (!open_decoder) return 0;
    const AVCodec *codec = avcodec_find_decoder(v->vst->codecpar->codec_id);
    if (!codec) {
        snprintf(err, errsz, "kein Decoder für %s", avcodec_get_name(v->vst->codecpar->codec_id));
        return -1;
    }
    v->dec = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(v->dec, v->vst->codecpar);
    v->dec->opaque = v;
    v->dec->pkt_timebase = v->vst->time_base;
    v->dec->thread_count = 1;                   /* viele Prozesse parallel: je 1 Thread */
    v->dec->flags2 |= AV_CODEC_FLAG2_FAST;
    v->dec->skip_loop_filter = AVDISCARD_ALL;   /* Entblocken: für 32x32 unnötig, spart ~25 % */
    if (want_hw) {
        bool supported = false;
        for (int i = 0;; i++) {
            const AVCodecHWConfig *c = avcodec_get_hw_config(codec, i);
            if (!c) break;
            if ((c->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
                c->device_type == AV_HWDEVICE_TYPE_VAAPI)
                supported = true;
        }
        if (!supported) {
            v->hw_failed = true;
            snprintf(err, errsz, "GPU kann %s nicht", codec->name);
            return -2;
        }
        v->dec->hw_device_ctx = av_buffer_ref(g_hw);
        v->dec->get_format = get_hw_format;
        v->dec->extra_hw_frames = 4;
        v->hw = true;
    }
    if ((r = avcodec_open2(v->dec, codec, NULL)) < 0) {
        char e[128];
        av_error_text(r, e, sizeof e);
        snprintf(err, errsz, "Decoder: %s", e);
        return want_hw ? -2 : -1;
    }
    v->pkt = av_packet_alloc();
    v->frame = av_frame_alloc();
    v->sw = av_frame_alloc();
    v->last_pts = AV_NOPTS_VALUE;
    return 0;
}

static double stream_duration(const Video *v)
{
    double d = 0;
    if (v->fmt->duration > 0) d = v->fmt->duration / (double)AV_TIME_BASE;
    if (v->vst->duration > 0) {
        double sd = v->vst->duration * av_q2d(v->vst->time_base);
        if (d <= 0 || (sd > 0 && sd < d * 1.5 && sd > d * 0.5)) d = sd;
    }
    return d;
}

/* Laufzeit fehlt im Container (manche WebM/TS): letzten Zeitstempel der Videospur suchen –
 * erst per Sprung ans Ende, sonst durch Lesen der Pakete (ohne Dekodieren) */
static double scan_duration(Video *v)
{
    AVStream *st = v->vst;
    double tb = av_q2d(st->time_base);
    int64_t start = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0, last = AV_NOPTS_VALUE;
    AVPacket *pkt = av_packet_alloc();
    if (avformat_seek_file(v->fmt, v->vidx, INT64_MIN, INT64_MAX, INT64_MAX, AVSEEK_FLAG_BACKWARD) >= 0) {
        for (int i = 0; i < 2000 && av_read_frame(v->fmt, pkt) >= 0; i++) {
            if (pkt->stream_index == v->vidx && pkt->pts != AV_NOPTS_VALUE && pkt->pts > last)
                last = pkt->pts + (pkt->duration > 0 ? pkt->duration : 0);
            av_packet_unref(pkt);
        }
    }
    if (last == AV_NOPTS_VALUE || (last - start) * tb < 0.5) {
        av_seek_frame(v->fmt, v->vidx, start, AVSEEK_FLAG_BACKWARD);
        long n = 0;
        while (av_read_frame(v->fmt, pkt) >= 0 && ++n < 5000000) {
            if (pkt->stream_index == v->vidx && pkt->pts != AV_NOPTS_VALUE && pkt->pts > last)
                last = pkt->pts + (pkt->duration > 0 ? pkt->duration : 0);
            av_packet_unref(pkt);
        }
    }
    av_packet_free(&pkt);
    av_seek_frame(v->fmt, v->vidx, start, AVSEEK_FLAG_BACKWARD);
    if (v->dec) avcodec_flush_buffers(v->dec);
    v->eof = false;
    return last == AV_NOPTS_VALUE ? 0 : (last - start) * tb;
}

static double frame_time(const Video *v, const AVFrame *f)
{
    int64_t pts = f->best_effort_timestamp;
    if (pts == AV_NOPTS_VALUE) pts = f->pts;
    if (pts == AV_NOPTS_VALUE) return NAN;
    double t = pts * av_q2d(v->vst->time_base);
    if (v->vst->start_time != AV_NOPTS_VALUE) t -= v->vst->start_time * av_q2d(v->vst->time_base);
    return t;
}

/* nächstes Bild dekodieren -> 1 = Bild in v->frame, 0 = Ende, <0 Fehler */
static int decode_next(Video *v)
{
    int errors = 0;
    while (true) {
        int r = avcodec_receive_frame(v->dec, v->frame);
        if (r == 0) {
            if (v->hw && v->frame->format != AV_PIX_FMT_VAAPI) v->hw_failed = true;
            return 1;
        }
        if (r == AVERROR_EOF) return 0;
        if (r != AVERROR(EAGAIN)) return r;
        if (v->eof) return 0;
        r = av_read_frame(v->fmt, v->pkt);
        if (r < 0) {
            v->eof = true;
            avcodec_send_packet(v->dec, NULL);
            continue;
        }
        if (v->pkt->stream_index == v->vidx) {
            r = avcodec_send_packet(v->dec, v->pkt);
            if (r < 0 && r != AVERROR(EAGAIN) && r != AVERROR_INVALIDDATA && ++errors > 200) {
                av_packet_unref(v->pkt);
                return r;
            }
        }
        av_packet_unref(v->pkt);
    }
}

/* Bild an Position t (Sekunden ab Start) dekodieren -> true, wenn v->frame gültig.
 * Liegt t kurz hinter dem zuletzt dekodierten Bild, wird weiterdekodiert statt
 * gesprungen. Nicht-Referenzbilder (B-Frames) werden auf dem Weg übersprungen. */
static bool seek_decode(Video *v, double t, double prev_t)
{
    bool sequential = !isnan(prev_t) && t >= prev_t && t - prev_t < 4.0 && !v->eof;
    if (!sequential) {
        int64_t ts = (int64_t)(t / av_q2d(v->vst->time_base));
        if (v->vst->start_time != AV_NOPTS_VALUE) ts += v->vst->start_time;
        if (t <= 0.05) ts = v->vst->start_time != AV_NOPTS_VALUE ? v->vst->start_time : 0;
        if (av_seek_frame(v->fmt, v->vidx, ts, AVSEEK_FLAG_BACKWARD) < 0) {
            /* Formate ohne Index: Byte-Position schätzen */
            if (av_seek_frame(v->fmt, -1, (int64_t)(t * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD) < 0 &&
                t > 0.05)
                av_seek_frame(v->fmt, v->vidx, 0, AVSEEK_FLAG_BACKWARD);
        }
        avcodec_flush_buffers(v->dec);
        v->eof = false;
    }
    int frames = 0;
    bool have = false;
    double frame_dur = v->vst->avg_frame_rate.num > 0 ? 1.0 / av_q2d(v->vst->avg_frame_rate) : 0.04;
    while (true) {
        /* weit vor dem Ziel: B-Frames überspringen (gleiches Ergebnis bis auf ~1 Bild) */
        v->dec->skip_frame = have && !isnan(frame_time(v, v->frame)) &&
                                     t - frame_time(v, v->frame) > 6 * frame_dur
                                 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
        if (have) av_frame_unref(v->frame);
        int r = decode_next(v);
        if (r <= 0) return have ? false : false;
        if (v->hw_failed) return false;
        have = true;
        double ft = frame_time(v, v->frame);
        if (isnan(ft) || ft + frame_dur * 0.5 >= t) break;
        if (++frames > 900) break;              /* kaputter Index: lieber ein Bild daneben */
    }
    v->dec->skip_frame = AVDISCARD_DEFAULT;
    return true;
}

/* aktuelles Bild (ggf. von der GPU geholt) -> Software-Bild */
static AVFrame *current_sw_frame(Video *v)
{
    if (v->frame->format != AV_PIX_FMT_VAAPI) return v->frame;
    av_frame_unref(v->sw);
    if (av_hwframe_transfer_data(v->sw, v->frame, 0) < 0) return NULL;
    v->sw->width = v->frame->width;
    v->sw->height = v->frame->height;
    return v->sw;
}

static struct SwsContext *get_sws(Video *v, const AVFrame *f, int ow, int oh, int ofmt, int flags)
{
    if (!v->sws || v->sws_w != f->width || v->sws_h != f->height || v->sws_fmt != f->format ||
        v->sws_ow != ow || v->sws_oh != oh || v->sws_ofmt != ofmt) {
        if (v->sws) sws_freeContext(v->sws);
        v->sws = sws_getContext(f->width, f->height, f->format, ow, oh, ofmt, flags, NULL, NULL,
                                NULL);
        v->sws_w = f->width;
        v->sws_h = f->height;
        v->sws_fmt = f->format;
        v->sws_ow = ow;
        v->sws_oh = oh;
        v->sws_ofmt = ofmt;
    }
    return v->sws;
}

/* aktuelles Bild -> Vergleichsbild */
static bool signature_of_current(Video *v, DfFrame *out)
{
    AVFrame *f = current_sw_frame(v);
    if (!f || f->width <= 0 || f->height <= 0) return false;
    uint8_t g64[64 * 64];
    struct SwsContext *s = get_sws(v, f, 64, 64, AV_PIX_FMT_GRAY8, SWS_AREA);
    if (!s) return false;
    uint8_t *dst[4] = {g64};
    int stride[4] = {64};
    if (sws_scale(s, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, stride) <= 0)
        return false;
    frame_signature(g64, out);
    return true;
}

/* Zeitpunkt des i-ten von k Vergleichsbildern */
static double sample_time(double dur, int i, int k)
{
    return dur * (i + 1) / (k + 1);
}

/* ---------------------------------------------------------------------------
 * Befehle
 * ------------------------------------------------------------------------- */
static void info_text(Video *v, Buf *t, double dur)
{
    AVCodecParameters *cp = v->vst->codecpar;
    double fps = v->vst->avg_frame_rate.num > 0 ? av_q2d(v->vst->avg_frame_rate)
               : v->vst->r_frame_rate.num > 0 ? av_q2d(v->vst->r_frame_rate) : 0;
    if (fps > 1000) fps = 0;
    int w = cp->width, h = cp->height;
    /* gedrehte Videos (Handy): Anzeige-Maße */
    const AVPacketSideData *sd = av_packet_side_data_get(cp->coded_side_data, cp->nb_coded_side_data,
                                                         AV_PKT_DATA_DISPLAYMATRIX);
    if (sd && sd->size >= 9 * 4) {
        double rot = av_display_rotation_get((const int32_t *)sd->data);
        if (fabs(fmod(fabs(rot), 180) - 90) < 1) {
            int tmp = w;
            w = h;
            h = tmp;
        }
    }
    int64_t br = v->fmt->bit_rate;
    buf_printf(t, "duration=%.3f\nwidth=%d\nheight=%d\nfps=%.3f\nbitrate=%lld\n", dur, w, h, fps,
               (long long)(br > 0 ? br / 1000 : 0));
    buf_printf(t, "vcodec=%s\nformat=%s\nimage=%d\n", avcodec_get_name(cp->codec_id),
               v->fmt->iformat ? v->fmt->iformat->name : "", v->is_image ? 1 : 0);
    if (v->aidx >= 0) {
        AVCodecParameters *ap = v->fmt->streams[v->aidx]->codecpar;
        buf_printf(t, "acodec=%s\nasr=%d\nach=%d\nabitrate=%lld\n", avcodec_get_name(ap->codec_id),
                   ap->sample_rate, ap->ch_layout.nb_channels,
                   (long long)(ap->bit_rate > 0 ? ap->bit_rate / 1000 : 0));
    }
    AVDictionaryEntry *e = av_dict_get(v->fmt->metadata, "creation_time", NULL, 0);
    if (e && e->value) buf_printf(t, "created=%s\n", e->value);
}

/* S: Infos und k Vergleichsbilder */
static void cmd_scan(int k, const char *path)
{
    char err[256] = "";
    Video v;
    int r = video_open(&v, path, g_hw != NULL, true, err, sizeof err);
    if (r < 0) {
        bool sw = r == -2 || v.hw_failed;
        video_close(&v);
        reply_error(sw ? "SW" : "ERR", err);
        return;
    }
    double dur = stream_duration(&v);
    if (!v.is_image && dur <= 0.2) dur = scan_duration(&v);
    if (v.is_image || dur <= 0.2) k = 1;
    if (k < 1) k = 1;
    if (k > DF_MAX_FRAMES) k = DF_MAX_FRAMES;
    Buf t = {0}, b = {0};
    info_text(&v, &t, v.is_image ? 0 : dur);
    DfFrame fr[DF_MAX_FRAMES];
    memset(fr, 0, sizeof fr);
    int good = 0;
    double prev = NAN;
    for (int i = 0; i < k; i++) {
        double ts = (v.is_image || k == 1) && dur <= 0.2 ? 0 : sample_time(dur, i, k);
        if (seek_decode(&v, ts, prev) && signature_of_current(&v, &fr[i])) {
            good++;
            double ft = frame_time(&v, v.frame);
            prev = isnan(ft) ? ts : ft;
        } else {
            prev = NAN;
        }
        if (v.hw_failed) break;
    }
    if (v.hw && v.hw_failed) {
        video_close(&v);
        free(t.p);
        reply_error("SW", "GPU kann dieses Video nicht dekodieren");
        return;
    }
    video_close(&v);
    if (!good) {
        free(t.p);
        reply_error("ERR", "kein Bild dekodierbar");
        return;
    }
    buf_printf(&t, "frames=%d\nhw=%d\n", k, g_hw ? 1 : 0);
    buf_add(&b, fr, sizeof(DfFrame) * (size_t)k);
    reply("OK", &t, &b);
    free(t.p);
    free(b.p);
}

/* F: Vergleichsbilder an festen Zeitpunkten */
static void cmd_frames(const char *path, const char *times)
{
    char err[256] = "";
    Video v;
    int r = video_open(&v, path, g_hw != NULL, true, err, sizeof err);
    if (r < 0) {
        bool sw = r == -2 || v.hw_failed;
        video_close(&v);
        reply_error(sw ? "SW" : "ERR", err);
        return;
    }
    double ts[256];
    int n = 0;
    for (const char *p = times; *p && n < 256;) {
        char *end;
        double x = strtod(p, &end);
        if (end == p) break;
        ts[n++] = x;
        p = *end == ',' ? end + 1 : end;
    }
    Buf b = {0};
    double prev = NAN;
    for (int i = 0; i < n; i++) {
        DfFrame f;
        memset(&f, 0, sizeof f);
        if (seek_decode(&v, ts[i], prev) && signature_of_current(&v, &f)) {
            double ft = frame_time(&v, v.frame);
            prev = isnan(ft) ? ts[i] : ft;
        } else {
            prev = NAN;
        }
        if (v.hw_failed) break;
        buf_add(&b, &f, sizeof f);
    }
    bool sw = v.hw && v.hw_failed;
    video_close(&v);
    if (sw) {
        free(b.p);
        reply_error("SW", "GPU kann dieses Video nicht dekodieren");
        return;
    }
    reply("OK", NULL, &b);
    free(b.p);
}

/* T: k Vorschaubilder (RGB24) im Feld w x bh; Text: "w h" je Bild */
static void cmd_thumbs(int k, int w, int bh, const char *path)
{
    char err[256] = "";
    Video v;
    int r = video_open(&v, path, g_hw != NULL, true, err, sizeof err);
    if (r < 0) {
        bool sw = r == -2 || v.hw_failed;
        video_close(&v);
        reply_error(sw ? "SW" : "ERR", err);
        return;
    }
    double dur = stream_duration(&v);
    if (!v.is_image && dur <= 0.2) dur = scan_duration(&v);
    if (v.is_image || dur <= 0.2) k = 1;
    if (k < 1) k = 1;
    if (k > DF_MAX_FRAMES) k = DF_MAX_FRAMES;
    if (w < 16) w = 16;
    if (w > 1920) w = 1920;
    if (bh < 16) bh = w * 9 / 16;
    Buf t = {0}, b = {0};
    double prev = NAN;
    for (int i = 0; i < k; i++) {
        double ts = k == 1 && dur <= 0.2 ? 0 : sample_time(dur, i, k);
        AVFrame *f = NULL;
        if (seek_decode(&v, ts, prev)) {
            double ft = frame_time(&v, v.frame);
            prev = isnan(ft) ? ts : ft;
            f = current_sw_frame(&v);
        } else {
            prev = NAN;
        }
        if (v.hw && v.hw_failed) break;
        if (!f || f->width <= 0) {
            buf_printf(&t, "0 0\n");
            continue;
        }
        /* ins Feld w x bh einpassen (Hochkant-Videos werden schmaler statt höher) */
        double sar = f->sample_aspect_ratio.num > 0 ? av_q2d(f->sample_aspect_ratio) : 1;
        double dw = f->width * sar, dh = f->height;
        int ow = w, oh = (int)lrint(w * dh / dw);
        if (oh > bh) {
            oh = bh;
            ow = (int)lrint(oh * dw / dh);
        }
        ow = ow < 2 ? 2 : ow;
        oh = oh < 2 ? 2 : oh;
        struct SwsContext *s = get_sws(&v, f, ow, oh, AV_PIX_FMT_RGB24, SWS_AREA);
        size_t sz = (size_t)ow * oh * 3;
        uint8_t *rgb = malloc(sz);
        uint8_t *dst[4] = {rgb};
        int stride[4] = {ow * 3};
        if (s && rgb &&
            sws_scale(s, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, stride) > 0) {
            buf_printf(&t, "%d %d\n", ow, oh);
            buf_add(&b, rgb, sz);
        } else {
            buf_printf(&t, "0 0\n");
        }
        free(rgb);
    }
    bool sw = v.hw && v.hw_failed;
    video_close(&v);
    if (sw) {
        free(t.p);
        free(b.p);
        reply_error("SW", "GPU kann dieses Video nicht dekodieren");
        return;
    }
    reply("OK", &t, &b);
    free(t.p);
    free(b.p);
}

/* A: Audio-Fingerabdruck (Chromaprint) der ersten max_s Sekunden (0 = ganz) */
static void cmd_audio(const char *path, double max_s)
{
    AVFormatContext *fmt = NULL;
    if (avformat_open_input(&fmt, path, NULL, NULL) < 0 || avformat_find_stream_info(fmt, NULL) < 0) {
        avformat_close_input(&fmt);
        reply_error("ERR", "Datei nicht lesbar");
        return;
    }
    int aidx = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (aidx < 0) {
        avformat_close_input(&fmt);
        reply_error("ERR", "keine Tonspur");
        return;
    }
    for (unsigned i = 0; i < fmt->nb_streams; i++)  /* nur die Tonspur lesen */
        if ((int)i != aidx) fmt->streams[i]->discard = AVDISCARD_ALL;
    AVStream *st = fmt->streams[aidx];
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    AVCodecContext *dec = codec ? avcodec_alloc_context3(codec) : NULL;
    if (!dec || avcodec_parameters_to_context(dec, st->codecpar) < 0 ||
        avcodec_open2(dec, codec, NULL) < 0) {
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        reply_error("ERR", "kein Decoder für die Tonspur");
        return;
    }
    enum { RATE = 11025 };
    SwrContext *swr = NULL;
    AVChannelLayout mono = AV_CHANNEL_LAYOUT_MONO;
    if (swr_alloc_set_opts2(&swr, &mono, AV_SAMPLE_FMT_S16, RATE, &dec->ch_layout, dec->sample_fmt,
                            dec->sample_rate, 0, NULL) < 0 ||
        swr_init(swr) < 0) {
        swr_free(&swr);
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        reply_error("ERR", "Ton nicht umwandelbar");
        return;
    }
    ChromaprintContext *cp = chromaprint_new(CHROMAPRINT_ALGORITHM_DEFAULT);
    chromaprint_start(cp, RATE, 1);
    AVPacket *pkt = av_packet_alloc();
    AVFrame *fr = av_frame_alloc();
    int16_t *out = NULL;
    int out_cap = 0;
    double fed = 0, silent = 0;
    bool done = false;
    while (!done) {
        int r = av_read_frame(fmt, pkt);
        if (r < 0) {
            avcodec_send_packet(dec, NULL);
            done = true;
        } else if (pkt->stream_index == aidx) {
            avcodec_send_packet(dec, pkt);
        }
        av_packet_unref(pkt);
        while (avcodec_receive_frame(dec, fr) == 0) {
            int max_out = swr_get_out_samples(swr, fr->nb_samples) + 32;
            if (max_out > out_cap) {
                out_cap = max_out;
                out = realloc(out, (size_t)out_cap * sizeof *out);
            }
            uint8_t *o[1] = {(uint8_t *)out};
            int n = swr_convert(swr, o, out_cap, (const uint8_t **)fr->extended_data, fr->nb_samples);
            if (n > 0) {
                int loud = 0;
                for (int i = 0; i < n; i++)
                    if (out[i] > 300 || out[i] < -300) loud++;
                if (loud < n / 50) silent += (double)n / RATE;
                chromaprint_feed(cp, out, n);
                fed += (double)n / RATE;
            }
            av_frame_unref(fr);
            if (max_s > 0 && fed >= max_s) done = true;
        }
    }
    chromaprint_finish(cp);
    uint32_t *raw = NULL;
    int raw_n = 0;
    Buf t = {0}, b = {0};
    if (chromaprint_get_raw_fingerprint(cp, &raw, &raw_n) && raw_n > 0) {
        buf_printf(&t, "seconds=%.3f\nsilent=%.3f\nitems=%d\n", fed, silent, raw_n);
        buf_add(&b, raw, (size_t)raw_n * sizeof *raw);
        reply("OK", &t, &b);
    } else {
        reply_error("ERR", "Ton zu kurz für einen Fingerabdruck");
    }
    if (raw) chromaprint_dealloc(raw);
    free(t.p);
    free(b.p);
    free(out);
    chromaprint_free(cp);
    av_frame_free(&fr);
    av_packet_free(&pkt);
    swr_free(&swr);
    avcodec_free_context(&dec);
    avformat_close_input(&fmt);
}

/* Name der GPU aus der Meldung von libva ("VAAPI driver: ... for AMD Radeon ... (radeonsi") */
static char g_gpu_name[128];

static void log_capture(void *avcl, int level, const char *fmt, va_list vl)
{
    (void)avcl;
    if (level > AV_LOG_VERBOSE || g_gpu_name[0]) return;
    char line[1024];
    vsnprintf(line, sizeof line, fmt, vl);
    const char *d = strstr(line, "VAAPI driver: ");
    const char *f = d ? strstr(d, " for ") : NULL;
    if (!f) return;
    f += 5;
    size_t n = strcspn(f, "(\n");
    while (n && f[n - 1] == ' ') n--;
    if (n >= sizeof g_gpu_name) n = sizeof g_gpu_name - 1;
    memcpy(g_gpu_name, f, n);
    g_gpu_name[n] = 0;
}

/* maskierten Pfad zurückverwandeln (\t, \n, \\) */
static void unescape(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            *o++ = *s == 't' ? '\t' : *s == 'n' ? '\n' : *s;
        } else {
            *o++ = *s;
        }
    }
    *o = 0;
}

/* ---------------------------------------------------------------------------
 * Hauptschleife
 * ------------------------------------------------------------------------- */
int main(int argc, char **argv)
{
    const char *gpu = NULL;
    int nice_level = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--gpu") && i + 1 < argc) gpu = argv[++i];
        else if (!strcmp(argv[i], "--nice") && i + 1 < argc) nice_level = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--white-borders")) g_white_borders = true;
    }
    if (nice_level > 0) {
        setpriority(PRIO_PROCESS, 0, nice_level);
        /* Platten-Priorität: "best effort", niedrigste Stufe (nicht "idle" – das kann bei
         * dauernd laufenden anderen Programmen ewig warten) */
        syscall(SYS_ioprio_set, 1, 0, (2 << 13) | 7);
    }
    prctl(PR_SET_PDEATHSIG, SIGKILL);           /* Engine weg -> Prozess auch */
    init_dct();
    if (gpu) {
        av_log_set_level(AV_LOG_VERBOSE);
        av_log_set_callback(log_capture);
        int r = av_hwdevice_ctx_create(&g_hw, AV_HWDEVICE_TYPE_VAAPI, gpu, NULL, 0);
        av_log_set_callback(av_log_default_callback);
        if (r < 0) {
            printf("H\tERR\n");
            fflush(stdout);
            return 3;
        }
    }
    av_log_set_level(AV_LOG_QUIET);
    printf("H\tOK\t%d\t%s\n", DF_PROTO_VERSION, g_gpu_name);   /* bereit */
    fflush(stdout);
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, stdin)) > 0) {
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        char *f[6] = {0};
        int nf = 0;
        for (char *p = line; nf < 6;) {
            f[nf++] = p;
            char *tab = strchr(p, '\t');
            if (!tab) break;
            *tab = 0;
            p = tab + 1;
        }
        if (!strcmp(f[0], "Q")) break;
        if (nf >= 2) unescape(f[nf - 1]);
        if (!strcmp(f[0], "S") && nf >= 3) cmd_scan(atoi(f[1]), f[2]);
        else if (!strcmp(f[0], "F") && nf >= 3) cmd_frames(f[2], f[1]);
        else if (!strcmp(f[0], "T") && nf >= 5) cmd_thumbs(atoi(f[1]), atoi(f[2]), atoi(f[3]), f[4]);
        else if (!strcmp(f[0], "A") && nf >= 3) cmd_audio(f[2], atof(f[1]));
        else if (!strcmp(f[0], "P")) reply("OK", NULL, NULL);       /* Lebenszeichen */
        else reply_error("ERR", "unbekannter Befehl");
    }
    free(line);
    av_buffer_unref(&g_hw);
    return 0;
}
