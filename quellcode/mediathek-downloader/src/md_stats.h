/*
 * Mediathek Downloader – Sehzeit und Statistik (md_stats.c)
 * =========================================================
 * Merkt sich, wann wie lange welcher Titel lief, je Tag und Stunde in sehzeit.tsv. Das
 * Protokoll bleibt, auch wenn die Datei gelöscht wird. Dazu ein einfaches Balkendiagramm.
 * Alle Funktionen laufen im GTK-Hauptthread.
 */
#ifndef MD_STATS_H
#define MD_STATS_H

#include <gtk/gtk.h>
#include <stdbool.h>

/* Ein Titel (Serie oder Film) mit allem, was über ihn erfasst ist */
typedef struct {
    char *name;
    bool film;
    double seconds;             /* angesehen, ohne Sprünge */
    int starts;                 /* so oft abgespielt */
    int finished;               /* so oft bis zum Ende */
    gint64 last;                /* zuletzt geschaut, Unix-Zeit */
} MdStatsTitle;

/* Protokoll laden; false = gab es noch nicht */
bool md_stats_load(void);
void md_stats_save(void);
/* Sehzeit komplett löschen (auch in der Datei) */
void md_stats_reset(void);
/* Sehzeit buchen; when = Unix-Zeit, zu der geschaut wurde */
void md_stats_add(const char *name, bool film, gint64 when, double seconds, int starts,
                  int finished);

/* Titel nach Sehzeit, die meiste zuerst (MdStatsTitle*, freigeben mit g_ptr_array_free) */
GPtrArray *md_stats_titles(void);
/* Sekunden je Tag der letzten days Tage, out[days - 1] = heute */
void md_stats_days(double *out, int days);
/* Sekunden je Wochentag (0 = Montag) und je Stunde */
void md_stats_weekdays(double out[7]);
void md_stats_hours(double out[24]);
double md_stats_total(void);
/* Tag mit der meisten Sehzeit (*day: g_free, "" = keiner) und Tage am Stück bis heute */
double md_stats_best_day(char **day);
int md_stats_streak(void);

/* Balkendiagramm: labels unter den Balken (Einträge dürfen NULL sein), tips beim Zeigen */
GtkWidget *md_stats_chart(const double *values, const char *const *labels,
                          const char *const *tips, int count, int height);

#endif
