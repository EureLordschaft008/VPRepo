# VPRepo

APT-Paketquelle für Linux Mint 22 / Ubuntu 24.04 (amd64) mit AV1-Converter, Intro-Cutter und
Duplikat-Finder.

```bash
sudo install -d -m 755 /etc/apt/keyrings
curl -fsSL https://eurelordschaft008.github.io/VPRepo/vprepo.gpg | sudo tee /etc/apt/keyrings/vprepo.gpg >/dev/null
echo 'deb [signed-by=/etc/apt/keyrings/vprepo.gpg] https://eurelordschaft008.github.io/VPRepo stable main' | sudo tee /etc/apt/sources.list.d/vprepo.list
sudo apt update
sudo apt install av1-converter intro-cutter duplikat-finder
```

Updates erscheinen danach in der Aktualisierungsverwaltung.

Die Pakete werden von den Projekten per GitHub Actions nach `pool/` geschrieben. Daraus baut
`scripts/build-index.sh` die signierten Indizes, die über GitHub Pages ausgeliefert werden.
