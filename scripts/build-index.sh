#!/bin/bash
# Baut aus pool/ die signierten APT-Indizes für GitHub Pages.
#   GPG_PRIVATE_KEY=... scripts/build-index.sh _site
set -euo pipefail

out=$1
rm -rf "$out"
mkdir -p "$out/dists/stable/main/binary-amd64"
cp -r pool "$out/pool"
cp vprepo.gpg "$out/vprepo.gpg"

export GNUPGHOME
GNUPGHOME=$(mktemp -d)
trap 'rm -rf "$GNUPGHOME"' EXIT
printf '%s\n' "$GPG_PRIVATE_KEY" | gpg --batch --quiet --import

cd "$out"
apt-ftparchive --arch amd64 packages pool > dists/stable/main/binary-amd64/Packages
gzip -9kn dists/stable/main/binary-amd64/Packages
# Release entsteht außerhalb von dists/stable, sonst listet es sich selbst mit falscher Größe
apt-ftparchive \
    -o APT::FTPArchive::Release::Origin=VPRepo \
    -o APT::FTPArchive::Release::Label=VPRepo \
    -o APT::FTPArchive::Release::Suite=stable \
    -o APT::FTPArchive::Release::Codename=stable \
    -o APT::FTPArchive::Release::Architectures=amd64 \
    -o APT::FTPArchive::Release::Components=main \
    release dists/stable > "$GNUPGHOME/Release"
mv "$GNUPGHOME/Release" dists/stable/Release
gpg --batch --yes --armor --detach-sign -o dists/stable/Release.gpg dists/stable/Release
gpg --batch --yes --clearsign -o dists/stable/InRelease dists/stable/Release
