/*
 * Duplikat-Finder – Auswahlregeln, Dateiaktionen, Ergebnis speichern/laden,
 * Export (CSV, HTML), Datenbank-Import, kleiner JSON-Leser
 */
#define _GNU_SOURCE
#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "df_internal.h"

/* ---------------------------------------------------------------------------
 * Auswahl
 * ------------------------------------------------------------------------- */
const char *const DF_CRIT_NAME[DF_CRIT_COUNT] = {
    "Laufzeit", "Auflösung", "Bildrate", "Bitrate", "Ton-Bitrate", "Dateigröße",
    "Neueste Datei", "Älteste Datei", "Kürzester Pfad"};
const char *const DF_CRIT_ID[DF_CRIT_COUNT] = {
    "laufzeit", "aufloesung", "bildrate", "bitrate", "ton", "groesse", "neueste", "aelteste",
    "pfad"};
static const int DEFAULT_ORDER[] = {DF_CRIT_DURATION, DF_CRIT_RESOLUTION, DF_CRIT_FPS,
                                    DF_CRIT_BITRATE, DF_CRIT_AUDIO_BITRATE, DF_CRIT_SIZE};

double df_criterion_value(const DfFile *f, DfCriterion c)
{
    switch (c) {
    case DF_CRIT_DURATION: return f->duration;
    case DF_CRIT_RESOLUTION: return (double)f->width * f->height;
    case DF_CRIT_FPS: return f->fps;
    case DF_CRIT_BITRATE: return f->bitrate;
    case DF_CRIT_AUDIO_BITRATE: return f->abitrate;
    case DF_CRIT_SIZE: return (double)f->size;
    case DF_CRIT_NEWEST: return (double)f->mtime;
    case DF_CRIT_OLDEST: return -(double)f->mtime;
    case DF_CRIT_SHORT_PATH: return -(double)strlen(f->path);
    default: return 0;
    }
}

/* gleich gut? (kleine Unterschiede bei Laufzeit, Bildrate und Bitrate zählen nicht) */
static bool crit_equal(DfCriterion c, double a, double b)
{
    switch (c) {
    case DF_CRIT_DURATION: return fabs(a - b) < 1.0;
    case DF_CRIT_FPS: return fabs(a - b) <= fmax(0.5, fmax(a, b) * 0.05);   /* 24 ≈ 25 */
    case DF_CRIT_BITRATE:
    case DF_CRIT_AUDIO_BITRATE: return fabs(a - b) <= fmax(fabs(a), fabs(b)) * 0.02;
    default: return a == b;
    }
}

int df_best_member(const DfResult *r, int group, const int *order, int norder, const bool *removed)
{
    const DfGroup *g = &r->groups[group];
    if (!order || norder <= 0) {
        order = DEFAULT_ORDER;
        norder = (int)G_N_ELEMENTS(DEFAULT_ORDER);
    }
    int cand[g->n > 0 ? g->n : 1], nc = 0;
    for (int i = 0; i < g->n; i++)
        if (!removed || !removed[g->first + i]) cand[nc++] = g->first + i;
    if (!nc) return -1;
    for (int c = 0; c < norder && nc > 1; c++) {
        DfCriterion cr = (DfCriterion)order[c];
        double best = -INFINITY;
        for (int i = 0; i < nc; i++) {
            double v = df_criterion_value(&r->files[r->members[cand[i]].file], cr);
            if (v > best) best = v;
        }
        int k = 0;
        for (int i = 0; i < nc; i++) {
            double v = df_criterion_value(&r->files[r->members[cand[i]].file], cr);
            if (crit_equal(cr, v, best)) cand[k++] = cand[i];
        }
        nc = k;
    }
    return cand[0];
}

int df_select(const DfResult *r, bool *checked, const bool *removed, const DfSelectSpec *s)
{
    if (!s->add && s->rule != DF_SEL_INVERT)
        for (int i = 0; i < r->nmembers; i++) checked[i] = false;
    for (int gi = 0; gi < r->ngroups; gi++) {
        const DfGroup *g = &r->groups[gi];
        int alive = 0;
        for (int i = 0; i < g->n; i++)
            if (!removed || !removed[g->first + i]) alive++;
        if (s->rule == DF_SEL_INVERT) {
            for (int i = 0; i < g->n; i++) {
                int m = g->first + i;
                if (!removed || !removed[m]) checked[m] = !checked[m];
            }
            continue;
        }
        if (s->rule == DF_SEL_NONE || alive < 2) continue;
        int best = -1;
        switch (s->rule) {
        case DF_SEL_LOWEST_QUALITY:
        case DF_SEL_SIMILARITY_RANGE:
            best = df_best_member(r, gi, s->order, s->norder, removed);
            break;
        case DF_SEL_KEEP_NEWEST: {
            int o[] = {DF_CRIT_NEWEST};
            best = df_best_member(r, gi, o, 1, removed);
            break;
        }
        case DF_SEL_KEEP_OLDEST: {
            int o[] = {DF_CRIT_OLDEST};
            best = df_best_member(r, gi, o, 1, removed);
            break;
        }
        default:
            break;
        }
        switch (s->rule) {
        case DF_SEL_LOWEST_QUALITY:
        case DF_SEL_KEEP_NEWEST:
        case DF_SEL_KEEP_OLDEST:
            for (int i = 0; i < g->n; i++) {
                int m = g->first + i;
                if ((!removed || !removed[m]) && m != best) checked[m] = true;
            }
            break;
        case DF_SEL_SIMILARITY_RANGE:
            for (int i = 0; i < g->n; i++) {
                int m = g->first + i;
                float sim = r->members[m].similarity;
                if ((!removed || !removed[m]) && m != best && sim >= s->sim_from - 1e-3 &&
                    sim <= s->sim_to + 1e-3)
                    checked[m] = true;
            }
            break;
        case DF_SEL_IDENTICAL:
        case DF_SEL_IDENTICAL_SIZE: {
            /* 100 % gleiche Mitglieder (bei _SIZE zusätzlich gleich groß) – je Klasse eins
             * behalten (das beste nach den Qualitätskriterien) */
            bool done[g->n];
            memset(done, 0, sizeof done);
            for (int i = 0; i < g->n; i++) {
                int mi = g->first + i;
                if (done[i] || (removed && removed[mi]) || r->members[mi].similarity < 99.99f ||
                    r->members[mi].partial)
                    continue;
                int cls[g->n], nc = 0;
                for (int j = i; j < g->n; j++) {
                    int mj = g->first + j;
                    if (done[j] || (removed && removed[mj]) || r->members[mj].similarity < 99.99f ||
                        r->members[mj].partial)
                        continue;
                    if (s->rule == DF_SEL_IDENTICAL_SIZE &&
                        r->files[r->members[mj].file].size != r->files[r->members[mi].file].size)
                        continue;
                    cls[nc++] = mj;
                    done[j] = true;
                }
                if (nc < 2) continue;
                int keep = cls[0];
                const int *order = s->order && s->norder ? s->order : DEFAULT_ORDER;
                int norder = s->order && s->norder ? s->norder : (int)G_N_ELEMENTS(DEFAULT_ORDER);
                for (int c = 1; c < nc; c++) {
                    /* besser als keep? erstes unterscheidendes Kriterium entscheidet */
                    for (int q = 0; q < norder; q++) {
                        double a = df_criterion_value(&r->files[r->members[cls[c]].file], order[q]);
                        double b = df_criterion_value(&r->files[r->members[keep].file], order[q]);
                        if (crit_equal(order[q], a, b)) continue;
                        if (a > b) keep = cls[c];
                        break;
                    }
                }
                for (int c = 0; c < nc; c++)
                    if (cls[c] != keep) checked[cls[c]] = true;
            }
            break;
        }
        case DF_SEL_PATH_CONTAINS:
        case DF_SEL_PATH_NOT_CONTAINS: {
            if (!s->text || !*s->text) break;
            int hit = 0;
            bool match[g->n];
            for (int i = 0; i < g->n; i++) {
                int m = g->first + i;
                bool c = df_glob_or_contains(r->files[r->members[m].file].path, s->text);
                match[i] = (!removed || !removed[m]) &&
                           (s->rule == DF_SEL_PATH_CONTAINS ? c : !c);
                hit += match[i];
            }
            if (!hit) break;
            int spare = -1;                 /* alle getroffen: die beste bleibt unmarkiert */
            if (hit == alive) spare = df_best_member(r, gi, s->order, s->norder, removed);
            for (int i = 0; i < g->n; i++)
                if (match[i] && g->first + i != spare) checked[g->first + i] = true;
            break;
        }
        case DF_SEL_PARTIAL_CLIPS:
            for (int i = 0; i < g->n; i++) {
                int m = g->first + i;
                if ((!removed || !removed[m]) && r->members[m].partial) checked[m] = true;
            }
            break;
        default:
            break;
        }
    }
    int n = 0;
    for (int i = 0; i < r->nmembers; i++) n += checked[i];
    return n;
}

/* ---------------------------------------------------------------------------
 * Dateiaktionen
 * ------------------------------------------------------------------------- */
/* freier Name im Ordner dir: "name.ext", sonst "name (2).ext" … */
static char *free_name_in(const char *dir, const char *base)
{
    char *p = g_build_filename(dir, base, NULL);
    if (g_access(p, F_OK) != 0) return p;
    g_free(p);
    const char *dot = strrchr(base, '.');
    size_t stem = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    for (int i = 2; i < 100000; i++) {
        char *name = g_strdup_printf("%.*s (%d)%s", (int)stem, base, i, dot && dot != base ? dot : "");
        p = g_build_filename(dir, name, NULL);
        g_free(name);
        if (g_access(p, F_OK) != 0) return p;
        g_free(p);
    }
    return NULL;
}

static void set_err(char *err, size_t errsz, GError *e, const char *fallback)
{
    snprintf(err, errsz, "%s", e ? e->message : fallback);
}

bool df_file_action(DfAction a, const char *path, const char *target, char **new_path, char *err,
                    size_t errsz)
{
    if (new_path) *new_path = NULL;
    GError *e = NULL;
    bool ok = false;
    switch (a) {
    case DF_ACT_TRASH: {
        GFile *f = g_file_new_for_path(path);
        ok = g_file_trash(f, NULL, &e);
        g_object_unref(f);
        if (!ok) set_err(err, errsz, e, "Papierkorb nicht möglich");
        if (ok) df_db_remove_path(path);
        break;
    }
    case DF_ACT_DELETE:
        ok = g_unlink(path) == 0;
        if (!ok) snprintf(err, errsz, "%s", g_strerror(errno));
        else df_db_remove_path(path);
        break;
    case DF_ACT_MOVE:
    case DF_ACT_COPY: {
        if (!target || !g_file_test(target, G_FILE_TEST_IS_DIR)) {
            snprintf(err, errsz, "Zielordner fehlt");
            return false;
        }
        char *base = g_path_get_basename(path);
        char *dest = free_name_in(target, base);
        g_free(base);
        if (!dest) {
            snprintf(err, errsz, "kein freier Name im Zielordner");
            return false;
        }
        GFile *src = g_file_new_for_path(path), *dst = g_file_new_for_path(dest);
        if (a == DF_ACT_MOVE)
            ok = g_file_move(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA, NULL,
                             NULL, NULL, &e);
        else
            ok = g_file_copy(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA, NULL,
                             NULL, NULL, &e);
        g_object_unref(src);
        g_object_unref(dst);
        if (!ok) set_err(err, errsz, e, "fehlgeschlagen");
        if (ok && a == DF_ACT_MOVE) df_db_rename_path(path, dest);
        if (ok && new_path) *new_path = dest;
        else g_free(dest);
        break;
    }
    case DF_ACT_HARDLINK:
    case DF_ACT_SYMLINK: {
        if (!target || !strcmp(target, path)) {
            snprintf(err, errsz, "keine Datei zum Verlinken");
            return false;
        }
        /* erst neben der Datei anlegen, dann atomar ersetzen: nie ohne Datei */
        char *tmp = g_strdup_printf("%s.df-link-%d", path, (int)getpid());
        int rc;
        if (a == DF_ACT_HARDLINK) {
            rc = link(target, tmp);
        } else {
            char *abs = g_canonicalize_filename(target, NULL);
            rc = symlink(abs, tmp);
            g_free(abs);
        }
        if (rc != 0) {
            snprintf(err, errsz, "%s", errno == EXDEV ? "anderes Laufwerk – Hardlinks gehen nur "
                     "innerhalb eines Dateisystems" : g_strerror(errno));
        } else if (rename(tmp, path) != 0) {
            snprintf(err, errsz, "%s", g_strerror(errno));
            g_unlink(tmp);
        } else {
            ok = true;
            df_db_remove_path(path);
        }
        g_free(tmp);
        break;
    }
    }
    g_clear_error(&e);
    return ok;
}

bool df_rename_file(const char *path, const char *new_name, char **new_path, char *err, size_t errsz)
{
    *new_path = NULL;
    if (!new_name || !*new_name || strchr(new_name, '/')) {
        snprintf(err, errsz, "ungültiger Name");
        return false;
    }
    char *dir = g_path_get_dirname(path);
    char *dest = g_build_filename(dir, new_name, NULL);
    g_free(dir);
    if (g_access(dest, F_OK) == 0) {
        snprintf(err, errsz, "eine Datei mit diesem Namen gibt es schon");
        g_free(dest);
        return false;
    }
    if (g_rename(path, dest) != 0) {
        snprintf(err, errsz, "%s", g_strerror(errno));
        g_free(dest);
        return false;
    }
    df_db_rename_path(path, dest);
    *new_path = dest;
    return true;
}

/* ---------------------------------------------------------------------------
 * Kleiner JSON-Leser (für Ergebnisse und Datenbank-Import)
 * ------------------------------------------------------------------------- */
typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JNode {
    JType t;
    double num;
    char *str;                  /* J_STR; bei Objekt-Mitgliedern: Schlüssel in key */
    char *key;
    struct JNode **kids;
    int nkids, cap;
} JNode;

typedef struct {
    const char *p, *end;
    bool bad;
} JParser;

static void jfree(JNode *n)
{
    if (!n) return;
    for (int i = 0; i < n->nkids; i++) jfree(n->kids[i]);
    g_free(n->kids);
    g_free(n->str);
    g_free(n->key);
    g_free(n);
}

static void jskip(JParser *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\n' || *j->p == '\r' || *j->p == '\t')) j->p++;
}

static char *jstring(JParser *j)
{
    if (j->p >= j->end || *j->p != '"') {
        j->bad = true;
        return NULL;
    }
    j->p++;
    GString *s = g_string_new(NULL);
    while (j->p < j->end && *j->p != '"') {
        char c = *j->p++;
        if (c != '\\') {
            g_string_append_c(s, c);
            continue;
        }
        if (j->p >= j->end) break;
        c = *j->p++;
        switch (c) {
        case 'n': g_string_append_c(s, '\n'); break;
        case 't': g_string_append_c(s, '\t'); break;
        case 'r': g_string_append_c(s, '\r'); break;
        case 'b': g_string_append_c(s, '\b'); break;
        case 'f': g_string_append_c(s, '\f'); break;
        case 'u': {
            if (j->end - j->p < 4) {
                j->bad = true;
                break;
            }
            char hex[5] = {j->p[0], j->p[1], j->p[2], j->p[3], 0};
            j->p += 4;
            unsigned cp = (unsigned)strtoul(hex, NULL, 16);
            if (cp >= 0xD800 && cp < 0xDC00 && j->end - j->p >= 6 && j->p[0] == '\\' &&
                j->p[1] == 'u') {
                char h2[5] = {j->p[2], j->p[3], j->p[4], j->p[5], 0};
                unsigned lo = (unsigned)strtoul(h2, NULL, 16);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    j->p += 6;
                }
            }
            if (cp < 0x20) g_string_append_c(s, (char)cp);   /* Steuerzeichen (Pfad-Bytes) */
            else g_string_append_unichar(s, cp);
            break;
        }
        default: g_string_append_c(s, c); break;
        }
    }
    if (j->p < j->end) j->p++;
    else j->bad = true;
    return g_string_free(s, FALSE);
}

static JNode *jvalue(JParser *j, int depth)
{
    jskip(j);
    if (j->p >= j->end || depth > 64) {
        j->bad = true;
        return NULL;
    }
    JNode *n = g_new0(JNode, 1);
    char c = *j->p;
    if (c == '{' || c == '[') {
        n->t = c == '{' ? J_OBJ : J_ARR;
        j->p++;
        jskip(j);
        if (j->p < j->end && *j->p == (c == '{' ? '}' : ']')) {
            j->p++;
            return n;
        }
        while (!j->bad) {
            char *key = NULL;
            if (n->t == J_OBJ) {
                jskip(j);
                key = jstring(j);
                jskip(j);
                if (j->p >= j->end || *j->p != ':') {
                    g_free(key);
                    j->bad = true;
                    break;
                }
                j->p++;
            }
            JNode *kid = jvalue(j, depth + 1);
            if (!kid) {
                g_free(key);
                break;
            }
            kid->key = key;
            if (n->nkids == n->cap) {
                n->cap = n->cap ? n->cap * 2 : 8;
                n->kids = g_realloc_n(n->kids, (gsize)n->cap, sizeof *n->kids);
            }
            n->kids[n->nkids++] = kid;
            jskip(j);
            if (j->p < j->end && *j->p == ',') {
                j->p++;
                continue;
            }
            if (j->p < j->end && *j->p == (n->t == J_OBJ ? '}' : ']')) {
                j->p++;
                break;
            }
            j->bad = true;
        }
    } else if (c == '"') {
        n->t = J_STR;
        n->str = jstring(j);
    } else if (!strncmp(j->p, "true", 4) || !strncmp(j->p, "false", 5)) {
        n->t = J_BOOL;
        n->num = *j->p == 't';
        j->p += *j->p == 't' ? 4 : 5;
    } else if (!strncmp(j->p, "null", 4)) {
        n->t = J_NULL;
        j->p += 4;
    } else {
        char *e;
        n->t = J_NUM;
        n->num = g_ascii_strtod(j->p, &e);
        if (e == j->p) j->bad = true;
        j->p = e;
    }
    if (j->bad) {
        jfree(n);
        return NULL;
    }
    return n;
}

static JNode *jget(const JNode *o, const char *key)
{
    if (!o || o->t != J_OBJ) return NULL;
    for (int i = 0; i < o->nkids; i++)
        if (o->kids[i]->key && !strcmp(o->kids[i]->key, key)) return o->kids[i];
    return NULL;
}

static double jnum(const JNode *o, const char *key, double def)
{
    JNode *n = jget(o, key);
    return n && (n->t == J_NUM || n->t == J_BOOL) ? n->num : def;
}

static const char *jstr(const JNode *o, const char *key)
{
    JNode *n = jget(o, key);
    return n && n->t == J_STR && n->str ? n->str : "";
}

static JNode *jparse_file(const char *path, char *err, size_t errsz)
{
    char *data = NULL;
    gsize len = 0;
    GError *e = NULL;
    if (!g_file_get_contents(path, &data, &len, &e)) {
        snprintf(err, errsz, "%s", e ? e->message : "nicht lesbar");
        g_clear_error(&e);
        return NULL;
    }
    JParser j = {data, data + len, false};
    JNode *root = jvalue(&j, 0);
    g_free(data);
    if (!root || j.bad) {
        jfree(root);
        snprintf(err, errsz, "keine gültige JSON-Datei");
        return NULL;
    }
    return root;
}

/* ---------------------------------------------------------------------------
 * Ergebnis speichern / laden
 * ------------------------------------------------------------------------- */
static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        if (*c == '"' || *c == '\\') fprintf(f, "\\%c", *c);
        else if (*c < 0x20) fprintf(f, "\\u%04x", *c);
        else fputc(*c, f);
    }
    fputc('"', f);
}

bool df_result_save(const DfResult *r, const bool *removed, const bool *checked, const char *path,
                    char *err, size_t errsz)
{
    char *tmp = g_strconcat(path, ".tmp", NULL);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        snprintf(err, errsz, "%s: %s", path, g_strerror(errno));
        g_free(tmp);
        return false;
    }
    fprintf(f, "{\"format\": \"duplikat-finder-ergebnis\", \"version\": 1, \"aehnlichkeit\": %.2f,"
            " \"vergleichsbilder\": %d, \"zeit\": %lld,\n\"dateien\": [\n", r->similarity, r->frames,
            r->scan_time);
    for (int i = 0; i < r->nfiles; i++) {
        const DfFile *d = &r->files[i];
        fprintf(f, "%s {\"id\": %lld, \"pfad\": ", i ? ",\n" : "", (long long)d->id);
        json_str(f, d->path);
        fprintf(f, ", \"groesse\": %lld, \"geaendert\": %lld, \"dauer\": %.3f, \"breite\": %d,"
                " \"hoehe\": %d, \"fps\": %.3f, \"bitrate\": %d, \"abitrate\": %d, \"asr\": %d,"
                " \"ach\": %d, \"vcodec\": ", d->size, d->mtime, d->duration, d->width, d->height,
                d->fps, d->bitrate, d->abitrate, d->asr, d->ach);
        json_str(f, d->vcodec);
        fputs(", \"acodec\": ", f);
        json_str(f, d->acodec);
        fprintf(f, ", \"bild\": %d, \"bilder\": \"", d->image);
        if (d->nframes) {
            char *b64 = g_base64_encode((const guchar *)d->frames, sizeof(DfFrame) * (gsize)d->nframes);
            fputs(b64, f);
            g_free(b64);
        }
        fputs("\"}", f);
    }
    fputs("\n],\n\"gruppen\": [\n", f);
    for (int g = 0; g < r->ngroups; g++) {
        const DfGroup *gr = &r->groups[g];
        fprintf(f, "%s [", g ? ",\n" : "");
        for (int i = 0; i < gr->n; i++) {
            int mi = gr->first + i;
            const DfMember *m = &r->members[mi];
            fprintf(f, "%s{\"datei\": %d, \"aehnlichkeit\": %.2f, \"gespiegelt\": %s,"
                    " \"teilclip\": %s, \"quelle\": %d, \"versatz\": %.3f, \"entfernt\": %s,"
                    " \"markiert\": %s}", i ? ", " : "", m->file, m->similarity,
                    m->flipped ? "true" : "false", m->partial ? "true" : "false", m->source,
                    m->offset, removed && removed[mi] ? "true" : "false",
                    checked && checked[mi] ? "true" : "false");
        }
        fputs("]", f);
    }
    fputs("\n]}\n", f);
    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (ok && g_rename(tmp, path) != 0) ok = false;
    if (!ok) {
        snprintf(err, errsz, "%s: Schreibfehler", path);
        g_unlink(tmp);
    }
    g_free(tmp);
    return ok;
}

bool df_result_load(DfResult *r, bool **removed, bool **checked, const char *path, char *err,
                    size_t errsz)
{
    memset(r, 0, sizeof *r);
    *removed = *checked = NULL;
    JNode *root = jparse_file(path, err, errsz);
    if (!root) return false;
    if (strcmp(jstr(root, "format"), "duplikat-finder-ergebnis") != 0) {
        snprintf(err, errsz, "keine Ergebnis-Datei des Duplikat-Finders");
        jfree(root);
        return false;
    }
    JNode *files = jget(root, "dateien"), *groups = jget(root, "gruppen");
    if (!files || files->t != J_ARR || !groups || groups->t != J_ARR) {
        snprintf(err, errsz, "Ergebnis-Datei unvollständig");
        jfree(root);
        return false;
    }
    r->similarity = jnum(root, "aehnlichkeit", DEFAULT_SIMILARITY);
    r->frames = (int)jnum(root, "vergleichsbilder", DEFAULT_FRAMES);
    r->scan_time = (long long)jnum(root, "zeit", 0);
    r->nfiles = files->nkids;
    r->files = g_new0(DfFile, MAX(r->nfiles, 1));
    for (int i = 0; i < r->nfiles; i++) {
        JNode *o = files->kids[i];
        DfFile *d = &r->files[i];
        d->id = (int64_t)jnum(o, "id", 0);
        d->path = g_strdup(jstr(o, "pfad"));
        d->size = (long long)jnum(o, "groesse", 0);
        d->mtime = (long long)jnum(o, "geaendert", 0);
        d->duration = jnum(o, "dauer", 0);
        d->width = (int)jnum(o, "breite", 0);
        d->height = (int)jnum(o, "hoehe", 0);
        d->fps = (float)jnum(o, "fps", 0);
        d->bitrate = (int)jnum(o, "bitrate", 0);
        d->abitrate = (int)jnum(o, "abitrate", 0);
        d->asr = (int)jnum(o, "asr", 0);
        d->ach = (int)jnum(o, "ach", 0);
        g_strlcpy(d->vcodec, jstr(o, "vcodec"), sizeof d->vcodec);
        g_strlcpy(d->acodec, jstr(o, "acodec"), sizeof d->acodec);
        d->image = jnum(o, "bild", 0) != 0;
        const char *b64 = jstr(o, "bilder");
        if (*b64) {
            gsize n = 0;
            guchar *b = g_base64_decode(b64, &n);
            if (n && n % sizeof(DfFrame) == 0 && n / sizeof(DfFrame) <= DF_MAX_FRAMES) {
                d->frames = (DfFrame *)b;
                d->nframes = (int)(n / sizeof(DfFrame));
            } else {
                g_free(b);
            }
        }
    }
    int nm = 0;
    for (int g = 0; g < groups->nkids; g++)
        if (groups->kids[g]->t == J_ARR) nm += groups->kids[g]->nkids;
    r->members = g_new0(DfMember, MAX(nm, 1));
    r->groups = g_new0(DfGroup, MAX(groups->nkids, 1));
    *removed = g_new0(bool, MAX(nm, 1));
    *checked = g_new0(bool, MAX(nm, 1));
    for (int g = 0; g < groups->nkids; g++) {
        JNode *ga = groups->kids[g];
        if (ga->t != J_ARR || ga->nkids < 1) continue;
        DfGroup *gr = &r->groups[r->ngroups];
        gr->first = r->nmembers;
        gr->similarity = 100;
        for (int i = 0; i < ga->nkids; i++) {
            JNode *o = ga->kids[i];
            int fi = (int)jnum(o, "datei", -1);
            if (fi < 0 || fi >= r->nfiles) continue;
            DfMember *m = &r->members[r->nmembers];
            m->file = fi;
            m->similarity = (float)jnum(o, "aehnlichkeit", 0);
            m->flipped = jnum(o, "gespiegelt", 0) != 0;
            m->partial = jnum(o, "teilclip", 0) != 0;
            m->source = (int)jnum(o, "quelle", -1);
            m->offset = jnum(o, "versatz", 0);
            (*removed)[r->nmembers] = jnum(o, "entfernt", 0) != 0;
            (*checked)[r->nmembers] = jnum(o, "markiert", 0) != 0;
            if (m->similarity < gr->similarity) gr->similarity = m->similarity;
            if (m->partial) gr->partial = true;
            r->nmembers++;
            gr->n++;
        }
        if (gr->n) r->ngroups++;
    }
    jfree(root);
    return true;
}

/* ---------------------------------------------------------------------------
 * Export
 * ------------------------------------------------------------------------- */
static void csv_field(FILE *f, const char *s)
{
    fputc('"', f);
    for (const char *c = s; *c; c++) {
        if (*c == '"') fputc('"', f);
        fputc(*c, f);
    }
    fputc('"', f);
}

bool df_result_export_csv(const DfResult *r, const bool *removed, const bool *checked,
                          const char *path, char *err, size_t errsz)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(err, errsz, "%s: %s", path, g_strerror(errno));
        return false;
    }
    fputs("\xEF\xBB\xBF" "Gruppe;Pfad;Größe (Byte);Laufzeit (s);Breite;Höhe;Bilder/s;Bitrate (kbit/s);"
          "Video;Ton;Ton-Bitrate (kbit/s);Ähnlichkeit (%);Gespiegelt;Teilclip ab (s);Geändert;Bild;"
          "Markiert\n", f);
    int gno = 0;
    for (int g = 0; g < r->ngroups; g++) {
        const DfGroup *gr = &r->groups[g];
        int alive = 0;
        for (int i = 0; i < gr->n; i++) alive += !(removed && removed[gr->first + i]);
        if (alive < 1) continue;
        gno++;
        for (int i = 0; i < gr->n; i++) {
            int mi = gr->first + i;
            if (removed && removed[mi]) continue;
            const DfMember *m = &r->members[mi];
            const DfFile *d = &r->files[m->file];
            char date[32], num[64];
            fprintf(f, "%d;", gno);
            csv_field(f, d->path);
            snprintf(num, sizeof num, "%.3f", d->duration);
            for (char *c = num; *c; c++) if (*c == '.') *c = ',';
            fprintf(f, ";%lld;%s;%d;%d;", d->size, num, d->width, d->height);
            snprintf(num, sizeof num, "%.2f", d->fps);
            for (char *c = num; *c; c++) if (*c == '.') *c = ',';
            fprintf(f, "%s;%d;%s;%s;%d;", num, d->bitrate, df_codec_name(d->vcodec),
                    df_codec_name(d->acodec), d->abitrate);
            snprintf(num, sizeof num, "%.1f", m->similarity);
            for (char *c = num; *c; c++) if (*c == '.') *c = ',';
            fprintf(f, "%s;%s;", num, m->flipped ? "ja" : "nein");
            if (m->partial) fprintf(f, "%.1f", m->offset);
            fprintf(f, ";%s;%s;%s\n", df_format_date(d->mtime, date, sizeof date),
                    d->image ? "ja" : "nein", checked && checked[mi] ? "ja" : "nein");
        }
    }
    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) snprintf(err, errsz, "%s: Schreibfehler", path);
    return ok;
}

static void html_text(FILE *f, const char *s)
{
    char *valid = g_utf8_make_valid(s, -1);
    for (const char *c = valid; *c; c++) {
        if (*c == '<') fputs("&lt;", f);
        else if (*c == '>') fputs("&gt;", f);
        else if (*c == '&') fputs("&amp;", f);
        else if (*c == '"') fputs("&quot;", f);
        else fputc(*c, f);
    }
    g_free(valid);
}

bool df_result_export_html(const DfResult *r, const bool *removed, const bool *checked,
                           const char *path, char *err, size_t errsz)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(err, errsz, "%s: %s", path, g_strerror(errno));
        return false;
    }
    fputs("<!DOCTYPE html>\n<html lang=\"de\"><head><meta charset=\"utf-8\">"
          "<title>Duplikate</title><style>"
          "body{font-family:sans-serif;margin:24px;background:#fafafa;color:#222}"
          "table{border-collapse:collapse;width:100%;margin-bottom:18px;background:#fff}"
          "td,th{border-bottom:1px solid #ddd;padding:4px 8px;text-align:left;font-size:13px}"
          "th{background:#eee}.m{color:#b00;font-weight:bold}h2{font-size:15px;margin:18px 0 4px}"
          "@media(prefers-color-scheme:dark){body{background:#1e1e1e;color:#ddd}"
          "table{background:#262626}th{background:#333}td,th{border-color:#444}}"
          "</style></head><body>\n<h1>Duplikate</h1>\n", f);
    char date[32];
    fprintf(f, "<p>Suchlauf vom %s, Ähnlichkeit ab %.0f %%, %d Vergleichsbilder je Video.</p>\n",
            df_format_date(r->scan_time, date, sizeof date), r->similarity, r->frames);
    int gno = 0;
    for (int g = 0; g < r->ngroups; g++) {
        const DfGroup *gr = &r->groups[g];
        int alive = 0;
        for (int i = 0; i < gr->n; i++) alive += !(removed && removed[gr->first + i]);
        if (alive < 1) continue;
        fprintf(f, "<h2>Gruppe %d – %d Dateien</h2><table><tr><th>Datei</th><th>Größe</th>"
                "<th>Laufzeit</th><th>Auflösung</th><th>Bitrate</th><th>Codec</th>"
                "<th>Ähnlichkeit</th><th>Geändert</th></tr>\n", ++gno, alive);
        for (int i = 0; i < gr->n; i++) {
            int mi = gr->first + i;
            if (removed && removed[mi]) continue;
            const DfMember *m = &r->members[mi];
            const DfFile *d = &r->files[m->file];
            char sz[32], dur[32];
            fprintf(f, "<tr%s><td>", checked && checked[mi] ? " class=\"m\"" : "");
            html_text(f, d->path);
            fprintf(f, "</td><td>%s</td><td>%s</td><td>%d×%d</td><td>%d kbit/s</td><td>%s</td>"
                    "<td>%.1f %%%s%s</td><td>%s</td></tr>\n",
                    df_format_bytes((double)d->size, sz, sizeof sz),
                    d->image ? "–" : df_format_duration(d->duration, dur, sizeof dur), d->width,
                    d->height, d->bitrate, df_codec_name(d->vcodec), m->similarity,
                    m->flipped ? " (gespiegelt)" : "", m->partial ? " (Teilclip)" : "",
                    df_format_date(d->mtime, date, sizeof date));
        }
        fputs("</table>\n", f);
    }
    fputs("<p>Rot: markierte Dateien.</p></body></html>\n", f);
    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) snprintf(err, errsz, "%s: Schreibfehler", path);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Datenbank-Import (Export-Format von df_db_export_json)
 * ------------------------------------------------------------------------- */
int df_db_import_json(const char *path, char *err, size_t errsz)
{
    JNode *root = jparse_file(path, err, errsz);
    if (!root) return -1;
    JNode *files = jget(root, "dateien");
    if (strcmp(jstr(root, "format"), "duplikat-finder-datenbank") != 0 || !files ||
        files->t != J_ARR) {
        snprintf(err, errsz, "keine Datenbank-Exportdatei des Duplikat-Finders");
        jfree(root);
        return -1;
    }
    int n = 0;
    df_db_lock();
    df_db_begin();
    for (int i = 0; i < files->nkids; i++) {
        JNode *o = files->kids[i];
        DfDbEntry e;
        memset(&e, 0, sizeof e);
        e.f.path = (char *)jstr(o, "pfad");
        if (!*e.f.path) continue;
        e.f.size = (long long)jnum(o, "groesse", 0);
        e.f.mtime = (long long)jnum(o, "geaendert", 0);
        e.inode = (long long)jnum(o, "inode", 0);
        e.k = (int)jnum(o, "k", 0);
        e.white_borders = jnum(o, "weiss", 0) != 0;
        e.status = (int)jnum(o, "status", 0);
        const char *er = jstr(o, "fehler");
        e.error = *er ? (char *)er : NULL;
        e.f.duration = jnum(o, "dauer", 0);
        e.f.width = (int)jnum(o, "breite", 0);
        e.f.height = (int)jnum(o, "hoehe", 0);
        e.f.fps = (float)jnum(o, "fps", 0);
        e.f.bitrate = (int)jnum(o, "bitrate", 0);
        g_strlcpy(e.f.vcodec, jstr(o, "vcodec"), sizeof e.f.vcodec);
        g_strlcpy(e.f.acodec, jstr(o, "acodec"), sizeof e.f.acodec);
        e.f.asr = (int)jnum(o, "asr", 0);
        e.f.ach = (int)jnum(o, "ach", 0);
        e.f.abitrate = (int)jnum(o, "abitrate", 0);
        e.f.image = jnum(o, "bild", 0) != 0;
        gsize len = 0;
        guchar *b = NULL;
        const char *b64 = jstr(o, "bilder");
        if (*b64) b = g_base64_decode(b64, &len);
        if (b && len % sizeof(DfFrame) == 0 && len / sizeof(DfFrame) <= DF_MAX_FRAMES) {
            e.f.frames = (DfFrame *)b;
            e.f.nframes = (int)(len / sizeof(DfFrame));
        }
        if (df_db_put(&e) > 0) n++;
        g_free(b);
    }
    df_db_commit();
    df_db_unlock();
    jfree(root);
    return n;
}
