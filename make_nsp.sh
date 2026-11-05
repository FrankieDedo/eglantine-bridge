#!/usr/bin/env bash
# Builds an installable NSP of the Switch app (full app, not a forwarder).
#   ./make_nsp.sh /path/to/prod.keys
# Needs ./build.sh run first (it leaves the app's .elf in the temp build dir).
# hacBrewPack (GPLv2) is not part of this repository: if tools/hacbrewpack/ has no
# hacbrewpack.exe, the script clones a pinned commit and builds it with MSYS2 UCRT64.
# The keys are only read by hacbrewpack to encrypt the NCAs; they are not copied
# into the NSP. Installing it needs a console with custom firmware and sigpatches.
set -euo pipefail

KEYS="${1:?usage: ./make_nsp.sh path/to/prod.keys}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
MSYS=/c/msys64
TOOLS="$MSYS/opt/devkitpro/tools/bin"
HBP="$ROOT/tools/hacbrewpack/hacbrewpack.exe"
# The original repository is gone; this mirror holds v3.05 unchanged.
HBP_REPO=https://github.com/rlaphoenix/hacBrewPack.git
HBP_COMMIT=745b16ecfc9ce055743067d200572204cb2aac6c
TITLE_ID=05eb0b1d00000000
VERSION=1.0.0

TMP="$(cygpath -m "${LOCALAPPDATA:-C:/Temp}")/ebridge-switch-build"
case "$TMP" in *" "*) TMP="C:/Temp/ebridge-switch-build" ;; esac
ELF="$TMP/switch-client/eglantine-bridge.elf"
[ -f "$ELF" ] || { echo "missing $ELF: run ./build.sh first" >&2; exit 1; }
[ -f "$KEYS" ] || { echo "keys file not found: $KEYS" >&2; exit 1; }
if [ ! -f "$HBP" ]; then
    echo "== Building hacBrewPack =="
    SRC="$ROOT/tools/hacbrewpack"
    rm -rf "$SRC"
    git clone -q "$HBP_REPO" "$SRC"
    git -C "$SRC" checkout -q "$HBP_COMMIT"
    cp "$SRC/config.mk.template" "$SRC/config.mk"
    PATH="$MSYS/ucrt64/bin:$MSYS/usr/bin:$PATH" make -C "$SRC" CC=gcc
fi

# hacbrewpack writes several work folders: keep them out of the project, in a path without spaces.
WORK="$TMP/nsp"
rm -rf "$WORK"
mkdir -p "$WORK/exefs" "$WORK/control" "$WORK/out"

"$TOOLS/elf2nso.exe" "$ELF" "$WORK/exefs/main"
"$TOOLS/npdmtool.exe" "$ROOT/switch-client/nsp/eglantine_bridge.json" "$WORK/exefs/main.npdm"
"$TOOLS/nacptool.exe" --create "Eglantine Bridge" "FrankieDedo" "$VERSION" "$WORK/control/control.nacp" \
    --titleid="$TITLE_ID"
ICON="$ROOT/switch-client/icon.jpg"
[ -f "$ICON" ] || ICON="$MSYS/opt/devkitpro/libnx/default_icon.jpg"
cp "$ICON" "$WORK/control/icon_AmericanEnglish.dat"

cd "$WORK"
export PATH="$MSYS/ucrt64/bin:$PATH"   # hacbrewpack needs the MSYS2 runtime DLLs
# hacbrewpack (2019) stops on key formats added to prod.keys later: give it only
# the keys it uses, in a temporary copy deleted when the script ends.
trap 'rm -f "$WORK/keys.txt"' EXIT
grep -E '^[[:space:]]*(header_key|key_area_key_application_[0-9a-fA-F]{2}|master_key_[0-9a-fA-F]{2}|aes_kek_generation_source|aes_key_generation_source|key_area_key_application_source)[[:space:]]*=' \
    "$KEYS" > keys.txt
"$HBP" -k keys.txt --titleid "$TITLE_ID" --keygeneration 1 \
    --exefsdir exefs --controldir control --noromfs --nologo \
    --nspdir out --ncadir nca --tempdir temp --backupdir backup

NSP="$(ls "$WORK"/out/*.nsp | head -1)"
cp "$NSP" "$ROOT/switch-client/eglantine-bridge.nsp"
echo "OK: switch-client/eglantine-bridge.nsp"
