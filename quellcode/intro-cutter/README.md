# Intro-Cutter 5.6.1

Erkennt ein wiederkehrendes Intro – oder alternativ ein Outro – in vielen Videos (inkl.
Unterordner) und schneidet es heraus: als Programm mit grafischer Oberfläche (`intro-cutter`) und
als Terminal-Version (`intro-cutter-cli`). C-Portierung der früheren Python-Version plus
Outro-Modus; Hash-Cache, Einstellungen und Protokolle liegen wie bisher in `~/.intro-cutter`
(Cache austauschbar mit der Python-Version).

- **Intro am Anfang:** alles bis zur angegebenen Länge wird vorne abgeschnitten (wie im
  Python-Skript).
- **Outro am Ende:** die angegebene Länge wird am Ende jedes Videos abgeschnitten. Erkannt wird
  das Outro an einem Vergleichsbild in seiner Mitte, vom Dateiende aus gemessen.
- **Automatisch (neu in 5.6):** ganz ohne Referenz-Video – Intro-Cutter findet selbst alle
  wiederkehrenden Intros im Quellordner (auch mehrere verschiedene, z. B. je Serie ein eigenes)
  und schneidet jedes Video bildgenau an seinem eigenen Intro-Ende. Siehe unten.

Vier Schnittmodi: verlustfrei per Copy, framegenau per CPU (x264; AV1-Quellen und WebM bleiben
AV1 per SVT-AV1), framegenau per GPU (VAAPI, Codec wie die Quelle) oder framegenau per GPU +
CPU parallel (die GPU nimmt die größten Dateien, die CPU parallel die kleinsten – maximales
Tempo).

In der Oberfläche lassen sich Referenz-Video, Quellordner und Ausgabeordner auch per Drag & Drop
aus dem Dateimanager setzen (eine auf einen Ordner gezogene Datei zählt als ihr Ordner).

Die Trefferliste zeigt unter den erkannten Videos alle nicht erkannten bis zum 10-fachen
Schwellwert – jeweils mit Vorschaubildern bei 1 s, 2 s und 5 s sowie bei 1/30 und 1/10 der
Laufzeit (im Outro-Modus vom Ende aus gemessen), damit sich leicht entscheiden lässt, ob dort
doch das Intro steckt. Lange Listen erscheinen in Portionen von 200 Zeilen („Weitere
anzeigen“); ausgeblendete Treffer bleiben ausgewählt und werden mitgeschnitten.

Auch sehr große Sammlungen sind kein Problem – getestet mit 100 000 Videos: die Suche aus dem
Cache dauert wenige Sekunden, die Oberfläche bleibt flüssig, der Speicherbedarf bleibt unter
1 GB (auch mit 9 Vergleichsbildern je Video), und Suche, Bearbeitung und Verschieben laufen
ohne Lecks bei Threads, Dateien und Speicher.

Die GPU wird höchstens zu 95 % ausgelastet: liegt die gemessene Auslastung der Video-Einheit
darüber, werden die GPU-Jobs regelmäßig kurz pausiert, damit nebenher z. B. Videos flüssig
laufen. Die Bearbeitungsseite zeigt GPU- und CPU-Jobs getrennt, jeweils mit Auslastung.

## Intros automatisch erkennen

Im Modus „Automatisch“ braucht es kein Referenz-Video und keine Länge. Einstellbar sind nur:

- **Intro höchstens … Sekunden** (Standard 20 s): so weit wird am Anfang jedes Videos gesucht.
  Längere Intros werden nicht geschnitten – dann die Suchlänge erhöhen (bis 120 s).
- **Kommt vor in mindestens … Videos** (Standard 5): erst dann gilt ein Intro als sicher.

So arbeitet die Erkennung:

1. Von jedem Video werden die ersten Sekunden Bild für Bild gelesen („Kennbilder“: Bild-
   Fingerabdruck, Helligkeit, Detail und der echte Zeitstempel jedes Bilds). Sie bleiben im
   Cache (`~/.intro-cutter/kennbilder.bin`, etwa 10 KB je Video) – spätere Suchen lesen nur neue
   oder geänderte Videos.
2. Ein Suchindex findet die Videos, die mit denselben Bildern beginnen; Versatz, Auflösung,
   Bildrate und Codec gleicht er aus. Folgen einer Serie im selben Ordner werden zuerst
   verglichen.
3. Je zwei Videos werden Bild für Bild verglichen bis zum ersten Bild, ab dem sie sich
   unterscheiden. Das Intro-Ende eines Videos ist die Stelle, bis zu der mindestens 4 andere
   Videos denselben Anfang haben – Duplikate verlängern den Schnitt so nicht, kürzere
   Fassungen eines Intros ziehen ihn nicht nach vorn.
4. Videos mit demselben Intro bilden eine Gruppe. Gekürzte Fassungen eines Intros bleiben bei
   ihrer Serie; ein gemeinsamer Sender-Vorspann verbindet aber keine verschiedenen Serien.

Lieber nicht schneiden als Inhalt entfernen – deshalb gilt ein Video nur als **sicher**, wenn:

- das Intro ganz am Anfang beginnt (davor höchstens 1 s Schwarzbild),
- es innerhalb der Suchlänge endet (nicht knapp davor) und danach eindeutig Neues kommt,
- es mindestens 1,5 s lang ist und in mindestens 5 Videos vorkommt,
- sein gemeinsamer Anfang nicht länger ist als bei den meisten Videos der Gruppe (sonst könnte
  z. B. ein gleicher Rückblick in einigen Folgen Inhalt sein).

Alles andere ist **unsicher**: wird mit Grund angezeigt, aber nicht ausgewählt. Die
Trefferseite zeigt je Intro das letzte Bild des Intros und das erste Bild danach (von einem
typischen Video), Länge, Anzahl und ein Häkchen für die ganze Gruppe; aufgeklappt jedes Video
mit seinem eigenen Schnitt (unsichere zuerst). „Ansehen …“ öffnet das bildgenaue
Markierfenster: eine Korrektur am typischen Video gilt für die ganze Gruppe – je Video genau auf
dessen eigene Bilder gelegt –, eine Korrektur an einem einzelnen Video nur für dieses. Darunter
die Videos ohne erkanntes Intro, jeweils mit Grund.

Geschnitten wird je Video an seinem eigenen Intro-Ende: per CPU oder GPU bildgenau. **Copy**
schneidet im automatischen Modus am letzten Keyframe im Intro (oder genau auf dem ersten Bild
danach, wenn dort einer liegt) – es nimmt also nie Inhalt weg, lässt aber evtl. einen Rest des
Intros stehen (die Zusammenfassung nennt, wie viel). Liegt im Intro kein Keyframe, wird bildgenau
neu kodiert. Videos im AV1-Format werden erkannt und geschnitten wie alle anderen.

Getestet mit 46 Testvideos bekannter Wahrheit (alle Intros bildgenau, alle Fallen richtig
nicht geschnitten; H.264, HEVC, AV1 in MP4/MKV/WebM, TS mit großen Zeitstempeln, 25/30/50
Bilder/s, Schwarzvorlauf, Duplikate, Intro nach einer Vorab-Szene, Intro länger als die
Suchlänge) und mit 100 000 simulierten Videos (Vergleich aus dem Cache in gut 2 Minuten,
1,2 GB Speicher; kein falscher Schnitt, Rückblicke und gemeinsame Vorspänne richtig behandelt).
Das erstmalige Einlesen dauert je Full-HD-Video einige Sekunden CPU-Zeit.

## Weitere Funktionen der Oberfläche

- **Ungefähre Restdauer der Bearbeitung:** unter dem Fortschrittsbalken steht z. B.
  „Restdauer: ca. 25 Min. · fertig gegen 23:40“ (bei langen Läufen „fertig morgen gegen
  03:10“), dazu Prozent und Restdauer im Fenstertitel – so auch in der Taskleiste. Geschätzt
  wird aus dem bisherigen Tempo: Größe der Dateien plus fester Aufwand je Datei (das
  Verhältnis lernt Intro-Cutter aus den fertigen Dateien), laufende Dateien zählen anteilig,
  gegen Ende werden die letzten Dateien einzeln auf die freien Plätze verplant; Pausen zählen
  nicht. Der Balken zeigt die erledigte Arbeit statt nur die Zahl fertiger Dateien. Die
  Terminal-Version meldet die Restdauer nach 2 Minuten und dann alle 5 Minuten.
- **Intro-Länge am Video festlegen – bildgenau:** „Im Video festlegen …“ neben der Länge öffnet
  die erste Minute (im Outro-Modus die letzte) des Referenz-Videos: Videobild mit Wiedergabe
  samt Ton, darunter ein Bildstreifen mit den Nachbarbildern und eine Zeitleiste mit
  Vorschaubildern. Der Schnitt liegt immer genau zwischen zwei Bildern: zum ersten Bild nach
  dem Intro (bzw. zum ersten Bild des Outros) gehen und „Video beginnt mit diesem Bild“ bzw.
  „Outro beginnt mit diesem Bild“ wählen (Taste M). Ein Schild im Bild zeigt, ob das gezeigte
  Bild abgeschnitten wird oder bleibt; im Bildstreifen sind die wegfallenden Bilder rot und der
  Schnitt ist eine gelbe Linie – ein Klick in die Lücke zwischen zwei Bildern setzt ihn dorthin.
  ←/→ gehen ein Bild weiter (sofort, die Bilder ringsum werden vorgeladen), Umschalt+←/→ eine
  Sekunde, Strg+←/→ verschieben den Schnitt um ein Bild, Leertaste spielt ab; der gelbe Marker
  auf der Zeitleiste lässt sich auch ziehen, der Ausschnitt auf 2 oder 5 Minuten erweitern.
  „Übernehmen“ trägt die Länge auf die Millisekunde ein – die Modi per CPU und GPU schneiden
  dann genau an diesem Bild (Copy schneidet beim Intro erst am nächsten Keyframe). Die
  Zeitpunkte der Bilder liest ffprobe aus dem Video, das klappt auch bei variabler Bildrate,
  29,97 Bildern/s und Transport-Streams (.ts).
- **Hash-Cache ansehen und aufräumen** (Zeile „Hash-Cache“ in den Einstellungen oder Menü):
  Liste aller Videos mit gespeicherten Vergleichsbildern, Suche, ausgewählte Einträge entfernen,
  veraltete Einträge (gelöschte oder geänderte Videos) entfernen oder den Cache ganz löschen.
  Die Kennbilder der automatischen Erkennung werden dabei mit aufgeräumt; ihre Größe steht
  ebenfalls dort.
- **An den Platz der Originale verschieben** zeigt einen Fortschrittsbalken (Dateien und GB);
  danach werden die leer gewordenen Ausgabeordner entfernt – nur die, die der Lauf selbst
  angelegt hat, und nur, wenn sie wirklich leer sind.
- **Genauigkeit der Erkennung:** statt eines Vergleichsbilds 3, 5 oder 9 über das ganze
  Intro bzw. Outro verteilt (Distanz = Median) – weniger Fehltreffer und übersehene Videos,
  dafür beim ersten Durchlauf entsprechend mehr Rechenzeit (danach Cache).
- **Pausieren:** hält alle laufenden Jobs an und startet keine neuen; „Fortsetzen“ macht
  weiter. In der Terminal-Version: Strg+Z, weiter mit `fg`.
- **Wenn alles fertig ist:** nichts tun, PC herunterfahren oder Intro-Cutter beenden – jeweils
  nach einem Countdown, der sich abbrechen lässt.
- Während Suche und Bearbeitung geht der PC nicht in den Standby; beim Abmelden oder
  Herunterfahren fragt die Sitzung nach.
- Alle Einstellungen (außer Referenz-Video und Ordnern) werden sofort gespeichert und beim
  nächsten Start wiederhergestellt, ebenso Fenstergröße und -zustand. Die Oberfläche passt
  sich der Fenstergröße an.

## Installieren

Einmalig die Paketquelle VPRepo einbinden und den Intro-Cutter installieren:

```bash
sudo install -d -m 755 /etc/apt/keyrings
curl -fsSL https://eurelordschaft008.github.io/VPRepo/vprepo.gpg | sudo tee /etc/apt/keyrings/vprepo.gpg >/dev/null
echo 'deb [signed-by=/etc/apt/keyrings/vprepo.gpg] https://eurelordschaft008.github.io/VPRepo stable main' | sudo tee /etc/apt/sources.list.d/vprepo.list
sudo apt update
sudo apt install intro-cutter
```

Neue Versionen erscheinen danach automatisch in der Aktualisierungsverwaltung von Linux Mint.

Danach im Menü unter „Sound & Video“ →
„Intro-Cutter“. Die Terminal-Version heißt `intro-cutter-cli`; Outros entfernt sie mit
`--outro`, gründlicher erkennt sie mit `--bilder 3` (bzw. 5 oder 9):

```bash
intro-cutter-cli --outro --bilder 3
```

Intros automatisch erkennen (geschnitten werden nur sicher erkannte Videos; unsichere lassen sich
in der grafischen Oberfläche prüfen):

```bash
intro-cutter-cli --auto --max 20 --mindestens 5
```

Deinstallieren:

```bash
sudo apt remove intro-cutter
```

## Neue Version veröffentlichen

`VERSION` (bei reinen Paketänderungen `DEBREV`) im `Makefile` erhöhen, committen und einen
passenden Tag pushen, z. B. `v5.6.1` bzw. `v5.6.1-2`. GitHub Actions baut dann das Paket,
legt es als Release an und stellt es über VPRepo bereit. Eine Versionsnummer lässt sich nur
einmal veröffentlichen.

## Selbst bauen

Benötigt `build-essential`, `libgtk-3-dev` und `ffmpeg`.

```bash
make        # baut intro-cutter und intro-cutter-cli
make deb    # baut das .deb-Paket
```

## Aufbau

- `src/ic_core.c`, `src/ic_core.h` – Engine: Erkennung (pHash), Schnitt, Watchdog, Cache
- `src/ic_gui.c` – grafische Oberfläche (GTK 3)
- `src/ic_cli.c` – Terminal-Version (Exit-Codes: 0 = Erfolg, 1 = Fehler, 130 = Abbruch)
- `data/` – Menüeintrag, Icon, Paketbeschreibung

Der Quelltext ist auf Lesbarkeit ausgelegt: sprechende Funktions- und Variablennamen,
ein Kommentar über jeder Funktion und eine Übersicht der Abschnitte am Anfang jeder
Datei. Namenskonventionen: `ic_*` = öffentliche Schnittstelle der Engine
(`ic_core.h`), `g_*` = globaler Zustand der Engine, `ui` = Zustand der Oberfläche,
GROSSBUCHSTABEN = Konstanten.

## Lizenz

Copyright (C) 2026 EureLordschaft008

Dieses Programm steht unter der GNU General Public License, Version 3 oder neuer
(GPL-3.0-or-later), siehe [LICENSE](LICENSE). Es wird ohne jede Gewähr bereitgestellt.
