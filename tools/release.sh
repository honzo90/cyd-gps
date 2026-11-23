#!/bin/sh
# Publish a new firmware version: bump include/fw_version.h, commit, tag, push.
# GitHub Actions then builds it and creates the release the CYD updates from.
# Usage: sh tools/release.sh 1.2.3
set -eu
cd "$(dirname "$0")/.."
v="${1:?usage: tools/release.sh MAJOR.MINOR.PATCH}"
echo "$v" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || { echo "version must be MAJOR.MINOR.PATCH"; exit 1; }
git diff --quiet && git diff --cached --quiet || { echo "commit or stash your changes first"; exit 1; }
sed -i.bak "s/^#define FW_VERSION \".*\"/#define FW_VERSION \"$v\"/" include/fw_version.h && rm include/fw_version.h.bak
sh test/host/run.sh
pio run -e cyd2usb
git add include/fw_version.h
git commit -m "Release v$v"
git tag "v$v"
git push origin HEAD "v$v"
echo "Pushed v$v. Build: gh run watch  |  release: gh release view v$v"
