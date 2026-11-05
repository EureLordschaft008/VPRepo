/*
 * Mediathek Downloader – eingebauter Player (md_player.c)
 * =======================================================
 * Ein Fenster mit Bild, Zeitleiste, Ton- und Untertitelwahl auf Grundlage von GStreamer.
 * Es gibt höchstens ein Player-Fenster; ein weiterer Aufruf spielt dort die neue Datei.
 * Alle Funktionen laufen im GTK-Hauptthread.
 */
#ifndef MD_PLAYER_H
#define MD_PLAYER_H

#include <gtk/gtk.h>
#include <stdbool.h>

/* Stand der Wiedergabe von path: alle paar Sekunden und beim Verlassen der Datei.
 * left: Datei wird verlassen (Fenster zu oder nächste Datei). Sekunden; duration 0 = unbekannt */
typedef void (*MdPlayerProgress)(const char *path, double position, double duration, bool left,
                                 gpointer data);
/* Datei nach path -> neu angelegter Pfad (und *title) oder NULL */
typedef char *(*MdPlayerNext)(const char *path, char **title, gpointer data);

typedef struct {
    MdPlayerProgress progress;
    MdPlayerNext next;          /* darf NULL sein */
    gpointer data;
} MdPlayerHooks;

/* Lässt sich abspielen? Sonst *why (g_free) mit dem fehlenden Teil */
bool md_player_available(char **why);
/* Datei im offenen Player-Fenster; NULL = keines offen */
const char *md_player_current(void);
/* path ab start Sekunden abspielen; false = Player nicht verfügbar */
bool md_player_play(GtkWindow *parent, const char *path, const char *title, double start,
                    const MdPlayerHooks *hooks);

#endif
