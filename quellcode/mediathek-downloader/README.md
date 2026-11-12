# Mediathek Downloader 2.0.1

Lädt Filme und Serien aus den Mediatheken von ZDF, ZDFneo, ZDFtivi, ZDFinfo, phoenix, ARD, ONE,
ARD alpha, tagesschau24, WDR, SWR, NDR, MDR, BR, hr, SR, rbb, KiKA, funk, 3sat, ARTE (auch auf
Englisch, Französisch, Spanisch, Italienisch und Polnisch), SRF, NRK und Netzkino – einzelne Folgen,
ganze Staffeln oder eine Serie mit allen Staffeln auf einmal. Als Programm mit grafischer Oberfläche
(`mediathek-downloader`, Aufbau und Optik wie beim AV1-Converter) und als Terminal-Version
(`mediathek-downloader-cli`, z. B. für lange Läufe in tmux). Es läuft unter Linux und Windows.

Bis Version 1.2 hieß das Programm ZDF-Downloader. Beim ersten Start zieht es Einstellungen, Verlauf,
Warteschlange und Zwischenspeicher aus `~/.zdf-downloader` nach `~/.mediathek-downloader` um.

## Installieren

### Linux

Über die Paketquelle [VPRepo](https://github.com/EureLordschaft008/VPRepo):

```bash
sudo apt install mediathek-downloader
```

Oder direkt aus der Paketdatei:

```bash
sudo apt install ./mediathek-downloader_2.0.1-1_amd64.deb
```

Das Programm steht danach im Startmenü unter **Unterhaltungsmedien**. Es braucht `curl`, `ffmpeg`
und für den eingebauten Player GStreamer; `apt` installiert alles mit.

### Windows

`mediathek-downloader-2.0.1-setup.exe` von der
[Release-Seite](https://github.com/EureLordschaft008/mediathek-downloader/releases/latest) laden
und starten. Der Installer braucht keine Administratorrechte und legt das Programm nach
`%LOCALAPPDATA%\Programs\Mediathek Downloader`, dazu einen Eintrag im Startmenü. GTK, GStreamer,
ffmpeg und curl sind dabei. Die Terminal-Version liegt als `mediathek-downloader-cli.exe` im
Unterordner `bin`. Windows SmartScreen kennt den Installer nicht und warnt beim ersten Start
(„Weitere Informationen“ → „Trotzdem ausführen“), weil er nicht signiert ist.

Einstellungen, Verlauf, Protokoll und Zwischenspeicher liegen unter Windows im Unterordner `Daten`
des Installationsordners, solange kein anderer Ordner gewählt ist (siehe „Programmdaten“).
Deinstallieren über die Windows-Einstellungen (Apps); Einstellungen und Videos bleiben dabei.

### Updates

Kurz nach dem Start fragt das Programm bei GitHub nach einer neuen Version. Gibt es eine, erscheint
oben eine Leiste mit „Installieren“, „Was ist neu?“ und „Überspringen“. „Installieren“ lädt das
Paket für das eigene System, prüft es gegen die `SHA256SUMS` des Releases und installiert es: unter
Linux per `pkexec apt-get install` (fragt nach dem Passwort), unter Windows über den Installer, der
das Programm beendet, die Dateien ersetzt und es wieder starten kann. Die Suche lässt sich in den
Einstellungen abschalten; das Menü hat „Nach Updates suchen“. Wer VPRepo eingerichtet hat, bekommt
das Update unter Linux auch über `sudo apt upgrade`.

## Was es kann

- **Mediatheken** – die Vorschau läuft wahlweise über eine, mehrere oder alle Mediatheken. Aus
  „Alle“ heraus wählt ein Klick genau eine, weitere Klicks nehmen andere dazu. Die Auswahl bleibt
  gespeichert.
- **Vorschau** – die Startseite zeigt neue Filme und Serien als Kacheln mit Titelbild. Ein Häkchen
  merkt einen Film vor, bei einer Serie alle fehlenden Folgen. Ein Klick auf die Kachel öffnet
  Bild, Beschreibung und alle Staffeln und Folgen zum einzelnen Auswählen. „Herunterladen“ startet
  alles auf einmal. Das Feld oben filtert die geladenen Titel nach Namen, das Menü daneben nach
  Sprache der Tonspur. Enter im Feld sucht in allen Mediatheken, auch nach älteren Titeln, und
  nimmt die Treffer in die Vorschau auf. Fehlt die Seite eines Titels, kommt das Bild als Standbild aus dem Video.
- **Kategorien** – Knöpfe über den Kacheln filtern nach Genres wie Komödie, Drama, Krimi oder
  Thriller, dazu die Wahl zwischen Filmen und Serien. Ein Titel kann in mehreren Kategorien stehen,
  darum ist die Summe größer als die Zahl der Titel. Die Kategorien stammen von der Mediathek und
  von IMDb. „Ohne Kategorie“ sammelt, was keine von beiden kennt. Beim ersten Start sammelt das
  Programm alles im Hintergrund, das dauert einige Minuten und lädt einmalig etwa 230 MB von IMDb.
  Danach kommen sie aus dem Zwischenspeicher, nur neue Titel werden nachgeholt.
- **Details beim Überfahren** – ruht die Maus kurz auf einer Kachel, zeigt ein Popup Altersfreigabe,
  Länge, Datum, Kategorien, IMDb-Bewertung und Beschreibung. Dieselben Angaben stehen in der
  Detailansicht.
- **IMDb** – der gelbe Knopf an Filmen und Serien öffnet die IMDb-Seite im Browser. Passt kein
  Treffer sicher, öffnet sich die IMDb-Suche zum Titel, und es erscheint keine Bewertung. So zeigt
  das Programm nie die Bewertung eines falschen Titels. Das trifft vor allem deutsche Titel, die bei
  IMDb anders heißen.
- **Staffeln und Folgen** – die Detailansicht zeigt Serie → Staffel → Folge; jede Ebene hat ein
  Häkchen. Eine Serie oder Staffel wählt nur aus, was noch fehlt. Die Folgen kommen aus allen
  Mediatheken: Bietet die eine nur einen Teil einer Staffel, kommen die übrigen von einer anderen,
  auch wenn diese in der Vorschau gerade nicht ausgewählt ist. Eine schon geladene Folge lässt
  sich nicht aus Versehen doppelt laden. Bieten mehrere Mediatheken dieselbe Folge an, erscheint sie
  nur einmal, und zwar die mit Staffelangabe: Fassungen ohne Staffel (auch als „Fernsehfilm“
  geführte) fallen weg, ebenso die ARD-Zählung, die alle Folgen in Staffel 1 durchnummeriert
  („Mutter und Sohn (S01/E13)“ neben „… (13) (S03/E01)“). Ist eine Folge schon mit Staffel geladen,
  gilt dieselbe Folge ohne Staffel als geladen. Ein ⚠ an der Staffel warnt, wenn Folgen in der Nummerierung
  fehlen. Fehlt nur das Ende einer Staffel, ist das nicht zu erkennen.
- **Was ist schon da?** – Jede Folge trägt ihren Stand: *Geladen*, *In Warteschlange*, *Angefangen*
  oder *Datei fehlt*. Serien und Staffeln zeigen „2 von 265 geladen“. Der Filter oben blendet alles
  außer den fehlenden oder den geladenen Titeln aus.
- **Neue Folgen** – oben in „Meine Videos“ stehen alle Serien, von denen du schon etwas geladen hast
  und die seit dem letzten Download neue Folgen bekommen haben, auch wenn sie nicht in der Vorschau
  stehen; ein Punkt am Reiter zeigt, dass es welche gibt. Je Serie eine Zeile, aufklappbar bis zur
  Folge. Ein Klick wählt alle aus, „Herunterladen“ unten startet sie. In der Vorschau markiert
  ein kleines grünes „!“ dieselben Serien. Während der Suche zeigt der Abschnitt, welche Serie gerade
  dran ist und wie weit es ist; ein Spinner in der Titelleiste zeigt es auf allen Seiten. Mit
  „Neue Folgen automatisch laden“ (Einstellungen) kommen sie von selbst in die Warteschlange, mit
  allen Sprachen und Untertiteln; gesucht wird beim Start und danach alle sechs Stunden.
- **Meine Videos** – alles Geladene als Kacheln wie in einer Mediathek: oben „Weiterschauen“ zum
  seitlichen Blättern, darunter jede Serie und jeder Film genau einmal, wahlweise von A bis Z oder
  zuletzt geladen zuerst. Das Menü daneben filtert nach Kategorie, mit der Anzahl je Kategorie; die
  Kategorien kommen aus der Vorschau, sonst von IMDb, und bleiben gespeichert. Eine Serie
  öffnet sich mit ihren Staffeln und Folgen; der große Knopf spielt die angefangene oder die nächste
  ungesehene Folge. Bilder kommen von der Mediathek, sonst als Standbild aus der Datei. Ein Balken
  zeigt, wie weit eine Datei oder Serie geschaut ist („3 von 10 gesehen“); jede angefangene Folge
  nennt dazu den Anteil und die Restzeit („40 % · noch 25 min“). Auch in der Detailansicht der
  Vorschau steht an geladenen Folgen „40 % gesehen“ oder „Gesehen“ statt nur „Geladen“. Die rechte Maustaste
  markiert als gesehen oder ungesehen, öffnet Ordner oder ein anderes Programm, wandelt nachträglich
  nach AV1 um (eine Datei oder alle Folgen; was schon AV1 ist, bleibt) und verschiebt Filme,
  Folgen oder ganze Serien nach Rückfrage in den Papierkorb; leere Staffelordner verschwinden mit.
  Ob die Dateien noch da sind, prüft das Programm im Hintergrund, die Seite wartet also nicht auf
  die Festplatte. Folgen ohne Staffelnummer stehen hinter den Staffeln.
  Beim Löschen einer ganzen Serie verschwinden auch ihre geplanten Downloads und Umwandlungen aus
  der Warteschlange; die Rückfrage nennt, wie viele es sind.
  Der Verlauf behält sie, damit die Suche nach neuen Folgen sie nicht wieder meldet. An „Weiterschauen“
  fehlt „Mit anderem Programm öffnen“. Nach der Suche nach neuen Folgen trägt eine Serie
  „✓ Komplett“, wenn jede Staffel ohne Lücke bis zur letzten angebotenen Folge da ist; komplette
  Staffeln tragen ein ✓ („Staffel 2 ✓“), die Serienseite nennt „komplett“ oder „2 von 3 Staffeln
  komplett“. Was keine Mediathek mehr anbietet, kann am Ende einer Staffel fehlen, ohne dass es
  auffällt.
- **Ordner prüfen** – gleicht den Speicherort mit dem Verlauf ab: Verschobene Dateien bekommen ihren
  neuen Ort, Dateien, die das Programm noch nicht kennt (etwa aus einer Sicherung), erscheinen in
  „Meine Videos“ und zählen als geladen, sofern ihr Name dem üblichen Schema folgt. Einträge ohne
  Datei lassen sich auf Wunsch aus dem Verlauf entfernen.
- **Statistik** – Sehzeit insgesamt, der letzten 7 Tage und je Tag, Wochentag und Uhrzeit, Tage am
  Stück, die meistgesehenen Titel, was wann geladen wurde, die meistgeladenen Serien, der Platz auf
  der Festplatte, wie viel davon schon gesehen ist, und die größten Dateien. Sehzeit zählt nur im
  eingebauten Player und nur, was wirklich lief; sie liegt in `sehzeit.tsv` im Datenordner und
  bleibt, auch wenn die Datei gelöscht wird. „Statistik zurücksetzen“ löscht nach einer Rückfrage
  die erfasste Sehzeit; Downloads und Platz zählen weiter, denn sie kommen aus den Dateien.
- **Player** – spielt die Dateien im Hauptfenster, auch AV1, und läuft bevorzugt, damit nichts
  ruckelt, wenn andere Programme den Rechner auslasten (Linux: über rtkit wie PipeWire, ohne
  Administratorrechte; Windows: höhere Priorität, solange ein Video läuft). Die Suche nach
  schwarzen Balken läuft dagegen mit niedriger Priorität. Die Grafikkarte rechnet
  das Bild für die Anzeige um (Linux: VA-API, `vapostproc`; Windows: Direct3D 11), wenn sie das kann: Im Prozessor
  schafft ein Kern das bei 1080p mit 50 Bildern je Sekunde und 10 Bit nicht, sobald der Rechner
  ausgelastet ist, und das Bild ruckelt. Abschalten lässt es sich unter Einstellungen → Player
  („Bild in der Grafikkarte umrechnen“); nach einem Fehler schaltet der Player von selbst zurück.
  Im Vollbild verschwindet die
  Titelleiste. Er merkt sich, wo eine Datei aufgehört hat, macht dort weiter und startet am Ende die
  nächste Folge. Tonspur und Untertitel lassen sich während der Wiedergabe wechseln bzw. an- und
  ausschalten. Der Zoom blendet schwarze Balken aus: „Schwarze Balken ausblenden“ sucht per ffmpeg
  in jedem Schlüsselbild der ganzen Datei nach Balken, die durchgehend im Bild sind, und vergrößert
  den Inhalt so weit, wie er ins Fenster passt. Vom Bild wird nichts abgeschnitten, auch wenn Fenster
  und Bild ein anderes Seitenverhältnis haben oder es sich mitten im Video ändert. Das Ergebnis
  merkt sich das Programm bis zum Beenden. Tasten: Leertaste
  Pause, ←/→ 10 Sekunden (ohne Einblendung der Leiste), ↑/↓ Lautstärke, A Tonspur, S Untertitel, Z Balken aus/ein, +/− Zoom,
  0 ganzes Bild, M stumm, F oder Doppelklick Vollbild, N nächste Folge, I Intro/Rückblick/Abspann überspringen, Esc zurück, ? oder F1
  blendet diese Übersicht im Bild ein. Springen,
  Lautstärke, Zoom und Spurwechsel blenden die Leiste nicht ein, sondern nur einen kurzen Hinweis.
  Die Lautstärke bleibt über einen Neustart erhalten (`Lautstaerke` in `einstellungen.ini`).
  Die Tooltips der Knöpfe nennen ihre Taste. Jede Taste lässt sich unter Einstellungen → „Tasten
  im Player“ ändern: Klick auf die Taste, dann die neue drücken; gehört sie schon einer anderen
  Aktion, tauschen die beiden, und „Standard“ stellt alles zurück. Übersicht und Tooltips nennen
  danach die neuen Tasten.
  Gesprungen wird ohne Wartezeit zum nächstgelegenen Schlüsselbild; in AV1-Dateien dieses Programms
  liegt alle 10 Sekunden eines, ein Sprung ist dort also 5 bis 15 Sekunden weit. Das Mausrad
  verstellt die Zeitleiste nicht. Wählt man bei laufendem Video einen anderen Reiter, spielt es in
  einem kleinen Fenster über allen anderen weiter (Bild im Bild); Doppelklick oder der Knopf in
  dessen Leiste holt es zurück. Ab 95 % gilt eine Datei als gesehen. Der Stand liegt in
  `wiedergabe.tsv` im Datenordner.
  **Intros:** Läuft der Vorspann einer Serienfolge, erscheint unten links der Knopf „Intro
  überspringen“; → (10 Sekunden vor) springt dann ebenfalls ans Ende des Intros. Mit „Intros automatisch überspringen“ (Einstellungen → Player) springt der Player
  von selbst ans Ende und zeigt kurz „Intro übersprungen“; wer danach zurück ins Intro spult, sieht
  es. Erkannt wird das Intro wie bei Jellyfin am Ton: Für die ersten zehn Minuten der Folge und
  zweier Nachbarfolgen derselben Serie entsteht ein Audio-Fingerabdruck (Chromaprint), und
  ein Abschnitt von 15 Sekunden bis drei Minuten, der in beiden Nachbarn gleich klingt, gilt als
  Intro – auch nach einer Szene vor dem Vorspann. Anfang und Ende rücken auf den nächsten Schnitt
  im Bild. Das läuft gleich nach dem Download im Hintergrund, sonst beim ersten Abspielen, und dauert
  etwa 10 bis 20 Sekunden mit niedriger Priorität; das Ergebnis steht danach in `intros.tsv` im Datenordner. Voraussetzungen: Die Folge
  hat eine Staffel- und Folgennummer, und von der Serie ist mindestens eine weitere Folge geladen
  (sonst erscheint kein Knopf). Ganz kurze Titelkarten (unter 15 Sekunden)
  gelten nicht als Intro.
  **Abspann:** Genauso erkennt der Player den Abspann in den letzten fünf Minuten der Folge (ab 10 Sekunden Länge; was danach weniger als 30 Sekunden bleibt, etwa eine Vorschau, zählt dazu)
  (Ergebnis in `abspanne.tsv`). Währenddessen zeigt derselbe Knopf „Abspann überspringen“; er,
  → und I springen zur nächsten Folge, ohne nächste Folge ans Ende des Abspanns. Von selbst
  übersprungen wird der Abspann nie.
  **Rückblicke:** „Was bisher geschah“ erkennt der Player daran, dass die ersten Minuten einer Folge
  aus Ausschnitten der vorigen Folgen bestehen: Er zerlegt die ersten fünf Minuten in Stücke von
  einer Sekunde und sucht jedes in den beiden vorigen Folgen; zählen nur Stücke, deren Nachbar an
  derselben Stelle weiterläuft. Ein Abschnitt von 20 Sekunden bis drei Minuten am Anfang oder direkt
  nach dem Intro, der aus mindestens drei verschiedenen Stellen stammt – oder aus dem Rückblick
  einer Vorgängerfolge, denn Rückblicke wiederholen oft dieselben Szenen –, ist der Rückblick;
  beginnt er in den ersten 20 Sekunden, gilt er ab dem Anfang. Zuerst über den Ton; passt der
  nicht (Musik, Sprecher), über das Bild: zwei Bilder je Sekunde, verkleinert zu einem
  64-Bit-Abdruck, unter Linux in der Grafikkarte dekodiert (VAAPI), unter Windows per D3D11 –
  eine Folge nach der anderen, und solange eine Umwandlung die Grafikkarte nutzt, in Software.
  Dann erscheint „Rückblick überspringen“ – folgt das Intro direkt, „Rückblick und Intro
  überspringen“; → und I springen ebenso. Mit „Rückblicke automatisch überspringen“
  (Einstellungen → Player) geschieht das von selbst. Die Abdrücke ganzer Folgen liegen im
  Zwischenspeicher (nach 120 Tagen ohne Gebrauch gelöscht), das Ergebnis in `rueckblicke.tsv`.
  Beim ersten Mal muss für den Bildvergleich jede Folge einmal ganz gelesen werden – mit
  Grafikkarte einige Minuten, ohne deutlich länger, mit niedriger Priorität im Hintergrund nach dem
  Download; die vorige Folge muss geladen sein.
- **Fenstergröße** – die Kacheln füllen jede Breite, Leisten brechen um, und unter etwa 1000 Pixeln
  Breite werden die Reiter oben zu Symbolen (der Name steht im Tooltip). Das Fenster lässt sich so
  bis etwa 520 Pixel schmal machen.
- **Meldungen** – die farbige Leiste oben (etwa „2 neue Folgen automatisch in die Warteschlange
  gestellt“) geht nach 10 Sekunden von selbst zu; das × schließt sie sofort.
- **Tastenkürzel** – Strg+1 bis Strg+5 wählen die Reiter, Strg+Bild↑/↓ blättert durch sie, Strg+F
  springt ins Suchfeld, F5 lädt die aktuelle Seite neu, F1 zeigt alle Kürzel. „Erste Schritte“ im
  Menü erklärt den Ablauf in wenigen Sätzen.
- **Sprachen und Untertitel** – Gibt es mehrere Tonspuren oder Untertitel, fragt das Programm vor
  dem Download. Standard: alles laden. Alles landet zusammen in einer MKV-Datei. Eine „Hörfassung:“
  gilt als Tonspur mit Audiodeskription derselben Folge, nicht als eigene Folge.
- **Pause** – einzeln oder alle. Für alle gibt es einen Knopf: Er ist farbig und heißt „Läuft“,
  solange Aufträge laufen, ein Klick pausiert alle, ein weiterer setzt sie fort. Auch nach dem Beenden oder einem Absturz geht es an derselben
  Stelle weiter; offene Downloads warten beim nächsten Start pausiert auf einen Klick. Downloads und
  Umwandlung lassen sich außerdem getrennt anhalten (Seite Warteschlange, „Steuerung“): Dann laden etwa
  die Downloads weiter, während die Umwandlung stillsteht.
- **Warteschlange** – oben steht, was gerade läuft, dann Fehlgeschlagenes und Wartendes. Fertige
  Aufträge verschwinden von selbst aus der Liste; der Kopf zählt, wie viele seit dem Start fertig
  wurden („56 fertig“), und wie viele noch auf die Umwandlung warten („3 warten auf Umwandlung“). Die Liste zeigt 100 Aufträge, „Weitere anzeigen“ holt die nächsten; so bleibt sie
  auch mit Tausenden Einträgen flüssig. „Fehlgeschlagene entfernen“ räumt alle Fehlschläge samt
  ihrer halbfertigen Dateien weg. Liegen dabei Reste auf der Platte,
  fragt das Programm vorher nach und nennt ihre Größe, auch beim Entfernen eines einzelnen
  Fehlschlags. Fehlermeldungen sagen, ob „Erneut versuchen“ hilft oder was sonst zu tun ist.
  Hat eine Serie mindestens drei offene Aufträge, stehen sie unter einer Zeile mit dem Seriennamen,
  deren Knöpfe alle pausieren, fortsetzen oder (nach Rückfrage) entfernen. Ist alles erledigt und
  das Fenster nicht im Vordergrund, meldet das System „Warteschlange fertig“. Der Reiter selbst
  zeigt die Zahl der offenen Aufträge und ob sie gerade laufen, angehalten sind oder warten.
- **Tempolimit** – gilt für alle Downloads zusammen und sofort, auch mitten im Download.
- **AV1** – auf Wunsch („Platz sparen“ in den Einstellungen) wandelt das Programm jedes Video nach
  dem Download um, mit den Einstellungen des AV1-Converters. Die Bildqualität ist in drei Stufen
  wählbar (Gut, Sehr gut, Beste); „Eigene Werte“ zeigt die Qualitätswerte für Grafikkarte und
  Prozessor. Schwarze Balken, die fest im Bild sind (etwa bei 4:3-Serien oder Kinofilmen),
  schneidet es dabei ab; ffmpeg prüft dafür jedes Schlüsselbild der ganzen Datei, ein schmaler Rand
  bleibt stehen, und im Zweifel bleibt das Bild ganz. Mit der Grafikkarte dekodiert sie auch das
  Ausgangsvideo: unter Linux per VA-API samt Zuschneiden, etwa dreimal so schnell und fast ohne
  Last für den Prozessor; unter Windows per D3D11VA, DXVA2 oder CUDA, was die Karte kann. Eine Umwandlung über die Grafikkarte bleibt dort: Scheitert sie,
  dekodiert im zweiten Versuch der Prozessor, und die Grafikkarte kodiert nur noch (bei manchen
  Dateien scheitert das Umrechnen in der Grafikkarte kurz vor dem Ende); fehlt ihr dann kurz
  Speicher, folgen nach 30 Sekunden noch zwei Versuche. Auf den Prozessor weicht sie nie aus. Das Ergebnis wird geprüft; erst dann verschwindet die MP4-Datei. Schon geladene Dateien
  lassen sich in „Meine Videos“ nachträglich umwandeln; Tonspuren und Untertitel bleiben erhalten.
- **Autostart** – auf Wunsch startet das Programm minimiert mit der Anmeldung (Linux:
  `~/.config/autostart`, Windows: Registry unter „Run“).
- **Speicherort** – frei wählbar, Standard ist `~/Videos/Mediathek` (Windows: `Videos\Mediathek`).
- **Zwischenspeicher** – Titelbilder, Kategorien, FSK und IMDb-Zuordnung bleiben dauerhaft liegen
  und werden nicht neu geladen; verkleinerte Titelbilder hält das Programm zusätzlich im Speicher,
  und hat ein Titel kein Bild, fragt es erst nach sieben Tagen wieder nach. Die Film- und
  Folgenlisten der Vorschau gelten sechs Stunden; der
  Knopf „Vorschau neu laden“ holt sie sofort frisch. Jede Anfrage an eine Mediathek oder an IMDb
  geht nur einmal raus: Fragen zwei Teile des Programms gleichzeitig dasselbe, wartet der zweite auf
  die Antwort des ersten, und was gerade fehlgeschlagen ist, wird zehn Minuten nicht wiederholt.
  Gespeicherte Trefferlisten der Suche löscht das Programm beim Start nach zwei Tagen. Das
  Protokoll wird ab 2 MB auf die neuere Hälfte gekürzt.
  Halbfertige Downloads (`.part`) bleiben beim Speicherort.
- **Programmdaten** – Einstellungen, Verlauf, Warteschlange, Protokoll und Zwischenspeicher liegen
  zusammen in einem Ordner. In den Einstellungen lässt sich ein anderer Ordner wählen; darin
  entsteht der Unterordner `Mediathek Downloader`, und alles zieht dorthin um, der Zwischenspeicher
  im Hintergrund. Der Knopf „Standardordner“ holt alles zurück. Ohne Wahl liegt alles unter Windows im Installationsordner (`Daten`), unter Linux
  in `~/.mediathek-downloader`, weil der Installationsordner `/usr` nicht beschreibbar ist. Welcher
  Ordner gewählt ist, steht in `~/.config/mediathek-downloader/datenordner` (Windows:
  `%LOCALAPPDATA%\mediathek-downloader\datenordner`). Wer bis 2.2 einen
  eigenen Ordner für den Zwischenspeicher hatte, bekommt dort beim ersten Start den Datenordner;
  aus dem alten Ordner ziehen nur die Dateien des Zwischenspeichers mit.

## Wo die Dateien landen

```
<Speicherort>/Serien/Löwenzahn mit Fritz Fuchs/Staffel 45/Löwenzahn mit Fritz Fuchs - S45E11 - Tierärzte - Hilfe für Keks.mkv
<Speicherort>/Filme/Air - Der große Wurf.mkv
```

Eine Folge gilt als geladen, sobald sie im Verlauf steht und die Datei noch da ist – egal in welcher
Qualität oder an welchem Speicherort. Wird eine Datei verschoben, zeigt das Programm *Datei fehlt*;
„Ordner prüfen“ in „Meine Videos“ findet sie wieder oder nimmt den Eintrag aus dem Verlauf,
die Folge lässt sich dann neu laden.

Einstellungen, Warteschlange, Verlauf und Protokoll liegen im Datenordner (siehe „Programmdaten“).

## Terminal-Version

```bash
mediathek-downloader-cli --serien --staffel 26 --nur-fehlende "Löwenzahn"
```

```bash
mediathek-downloader-cli --serien --staffel 26 --laden alle --ziel ~/Videos/Mediathek --limit 10 --av1 gpu "Löwenzahn"
```

Ohne `--laden` zeigt das Programm nur die Trefferliste mit dem Stand jedes Titels. Strg+C pausiert;
derselbe Befehl macht dort weiter. `--mediathek arte,srf` beschränkt die Suche auf einzelne
Mediatheken. Alle Optionen: `mediathek-downloader-cli --help`. Die Warteschlange
der Oberfläche nutzt die Terminal-Version nicht, den Verlauf schon.

## Qualität und AV1

| Stufe | Bild | Größe je 90 min |
|---|---|---|
| Hoch (Standard) | 1080p, 50 Bilder/s, H.264 | ca. 4,5 GB |
| Mittel | kleiner als 1080p | ca. 2,3 GB |
| Niedrig | für unterwegs | ca. 0,5 GB |

| | GPU (Standard) | CPU |
|---|---|---|
| Encoder („Umwandlung mit“) | `av1_vaapi`, Qualität 64 („Sehr gut“); Windows: `av1_nvenc`, `av1_qsv` oder `av1_amf` | SVT-AV1, Preset 6, RF 35 |
| Tempo | sehr schnell | sehr viel langsamer |
| Ton, Untertitel | unverändert | unverändert |

Änderungen an Encoder und Qualität gelten sofort für alle offenen Aufträge, nur eine laufende
Umwandlung läuft mit den alten Werten zu Ende. Scheitert die GPU, übernimmt die CPU. Wird das Ergebnis nicht kleiner, bleibt die MP4-Datei.

## Grenzen

- **Kein 4K.** Die Suche liefert höchstens 1080p. 4K-Fassungen gibt es nur für wenige Titel und
  nur über die Datenquelle des ZDF selbst, die das Programm nicht nutzt.
- **Geoblocking.** Manche Titel liegen auf Adressen, die nur aus Deutschland erreichbar sind.
- **Mediathek-Fristen.** Geladen werden kann nur, was gerade online steht. Die Serie zeigt also nur
  die Folgen, die es derzeit gibt.
- **Staffeln stimmen nur so weit wie die Angaben.** Ohne `(S18/E05)` im Titel steht eine Sendung
  unter „Einzelne Sendungen“.
- **Fassungen mit Gebärdensprache** sind eigene Einträge, weil die Einblendung fest im Bild ist.
  Sie sind ausgeblendet und lassen sich in den Einstellungen einschalten.
- **Reine Audio-Inhalte** blendet die Einstellung „Reine Audio-Inhalte ausblenden“ aus: Hörfassungen,
  zu denen es keine Bildfassung gibt, sowie Podcasts und Radio-Talks (erkannt am Namen;
  Videopodcasts bleiben). Die Audiodeskription als Tonspur einer normalen Fassung bleibt wählbar.
- **Titelbilder** kommen von den Seiten der Mediatheken. Eine Serie zeigt das Bild ihrer neuesten
  Folge. Viele ältere Titel haben keine Seite mehr, nur noch das Video; ihr Standbild kann
  unpassend sein.
- **Kategorien** stammen beim ZDF von dessen Seiten. Die anderen Mediatheken nennen keine; dort leitet das
  Programm sie aus Stichwörtern ab, zum Beispiel Krimi bei „Tatort“. IMDb ergänzt alle Mediatheken,
  aber nur bei sicherem Titeltreffer. Eine Serie übernimmt die Kategorien ihrer neuesten Folge.
- **Staffeln bei der ARD:** Nennt ein Titel nur „Folge 5“, zählt er zu Staffel 1. Serien ganz ohne
  Nummern wie der Tatort stehen unter „Einzelne Sendungen“.
- **ARTE und SRF** liefern Streams statt Dateien. Das Programm setzt sie mit ffmpeg zu einer
  MP4-Datei zusammen, bei ARTE mit allen Sprachfassungen als Tonspuren. Ein pausierter Stream
  beginnt beim Fortsetzen von vorn. Verschlüsselte Streams und Livesendungen lädt es nicht.
- **SRF weltweit:** Nur Titel, die auch außerhalb der Schweiz freigegeben sind, erscheinen. Bei
  ARTE und SRF stehen Staffel und Folge oft nur als „(3/6)“ im Titel.
- **NRK** liefert nur Serien, und nur Folgen, die die NRK weltweit freigibt (meist eigene
  Produktionen), auf Norwegisch. Die Liste kommt von den Serien der NRK-Startseite; über die Suche
  (Enter im Suchfeld) findet sich mehr.
- **ARTE intl.** fasst die anderen Sprachfassungen von ARTE zusammen; Titel und Untertitel sind in
  der jeweiligen Sprache.
- **Private Sender und Bezahldienste** gibt es nicht: Sie stehen nicht in der Datenquelle, ihre
  Player sind auf Kopierschutz ausgelegt, und das Programm umgeht keinen Kopierschutz.
- **Netzkino** liefert nur Filme, und nur solche ohne Kopierschutz und außerhalb von NetzkinoPlus.
  Länge und Größe nennt der Dienst nicht, die Qualitätsstufe wirkt dort nicht. Die Dateien sind oft
  sehr groß, teils über 10 GB. Netzkino finanziert sich über Werbung, die beim Download entfällt.
- **Titelbilder bei WDR, SWR, NDR und rbb** sind Standbilder aus dem Video, und FSK fehlt dort.
  Bei ARTE und SRF kommt das Bild nur von der Seite; ohne Seite bleibt der Titel ohne Bild.
- **Titellisten** kommen von [MediathekViewWeb](https://mediathekviewweb.de), nicht von den Sendern.
  Ist dieser Dienst nicht erreichbar, gibt es keine Titel.
- **Windows:** Der Papierkorb, der Player und die GPU-Umwandlung laufen über die Windows-Fassungen
  von GTK, GStreamer und ffmpeg. Ohne NVIDIA-, Intel- oder AMD-Karte mit AV1-Encoder übernimmt die
  CPU. Die Fenster sehen nach GTK aus, nicht nach Windows.

## Rechtliches

Information courtesy of IMDb (https://www.imdb.com). Used with permission. Die Bewertungen stammen
aus der [offiziellen Datendatei](https://developer.imdb.com/non-commercial-datasets/) von IMDb, die
nur für den privaten, nicht kommerziellen Gebrauch freigegeben ist. Die Bewertungen werden wöchentlich
neu geladen und belegen etwa 25 MB im Zwischenspeicher, die Kategorien monatlich mit etwa 26 MB.

Das Programm lädt frei zugängliche Dateien ohne Kopierschutz; es umgeht keine technischen
Schutzmaßnahmen. Kopien zum privaten Gebrauch sind nach § 53 UrhG grundsätzlich erlaubt. Die Dateien
bleiben urheberrechtlich geschützt: nicht weitergeben, nicht veröffentlichen. Das ist keine
Rechtsberatung.

## Neue Version veröffentlichen

`VERSION` im Makefile und `MD_VERSION` in `src/md_core.h` erhöhen, committen und den Tag `v<Version>`
pushen. GitHub Actions baut dann das .deb und den Windows-Installer, legt beide mit `SHA256SUMS`
als Release an und bringt das .deb nach VPRepo. Das Release ist zugleich die Quelle der Updates im
Programm. Von Hand gestartet (`gh workflow run release.yml`) baut der Workflow beide Pakete nur als
Artefakte, ohne Release.

## Aus dem Quelltext bauen

Gebraucht werden `gcc`, `make`, `libgtk-3-dev`, `libgstreamer1.0-dev` und `dpkg-deb`.

```bash
make deb
```

Windows: In [MSYS2](https://www.msys2.org) (Umgebung UCRT64) die Pakete aus
`.github/workflows/release.yml` installieren und `windows/build.sh` aufrufen; das Ergebnis ist
`mediathek-downloader-<Version>-setup.exe`.

`make test` führt die Regressionstests und einen kurzen Fuzz-Lauf aus. Die Tests brauchen `curl` und
legen alles in einem temporären Ordner ab. `make test ASAN=1` nimmt AddressSanitizer dazu. Das geht
nur, wenn `/etc/ld.so.preload` nichts vorlädt.

| Datei | Aufgabe |
|---|---|
| `src/md_core.c` | Engine: Suche, Warteschlange, Download, Umwandlung, Verlauf |
| `src/md_gui.c` | grafische Oberfläche (GTK 3) |
| `src/md_player.c` | eingebauter Player (GStreamer) |
| `src/md_cli.c` | Terminal-Version |
| `src/md_stats.c` | Reiter Statistik |
| `data/` | Starter, Symbol, Paketbeschreibung |
| `windows/` | Build-Skript und Installer für Windows |

## Lizenz

Copyright (C) 2026 EureLordschaft008

Dieses Programm steht unter der GNU General Public License, Version 3 oder neuer
(GPL-3.0-or-later), siehe [LICENSE](LICENSE). Es wird ohne jede Gewähr bereitgestellt.
