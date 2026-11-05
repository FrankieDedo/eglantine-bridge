#!/usr/bin/env bash
# Builds everything. Run from Git Bash or MSYS2:  ./build.sh
# Needs MSYS2 (C:\msys64) with mingw-w64-ucrt-x86_64-{gcc,cmake,ninja} and devkitPro switch-dev.
#
# devkitPro's make breaks on paths with spaces ("Eglantine bridge"), so the
# Switch app is built in a temporary copy and the .nro is copied back.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
MSYS=/c/msys64

echo "== Host PC (C++) =="
# Windows can't overwrite a running .exe: the link would fail with "Permission denied".
for exe in ebridge_host_cli.exe eglantine_bridge.exe; do
    if tasklist 2>/dev/null | grep -qi "$exe"; then
        echo "ERROR: $exe is running. Close it (Ctrl+C in its window) and run ./build.sh again" >&2
        exit 1
    fi
done
PATH="$MSYS/ucrt64/bin:$PATH" cmake -S "$ROOT/host" -B "$ROOT/build/host" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
PATH="$MSYS/ucrt64/bin:$PATH" cmake --build "$ROOT/build/host"
# Copy the DLLs they need (FFmpeg, Opus, GCC runtime) next to the exes, so they run with a double click.
for exe in ebridge_host_cli.exe eglantine_bridge.exe; do
    EXE_WIN="$(cygpath -m "$ROOT/build/host/$exe")"
    MSYSTEM=UCRT64 "$MSYS/usr/bin/bash.exe" -lc "ldd '$EXE_WIN' | awk '/\/ucrt64\// {print \$3}'" </dev/null |
        while read -r dll; do cp -u "$MSYS$dll" "$ROOT/build/host/"; done
done

echo "== App Switch (libnx) =="
# Windows-style path (C:/...) so Git Bash and MSYS2 agree on the location.
TMP="$(cygpath -m "${LOCALAPPDATA:-C:/Temp}")/ebridge-switch-build"
case "$TMP" in *" "*) TMP="C:/Temp/ebridge-switch-build" ;; esac
mkdir -p "$TMP/switch-client"
cp -r "$ROOT/common" "$TMP/"
cp -r "$ROOT/switch-client/Makefile" "$ROOT/switch-client/source" "$TMP/switch-client/"
[ -f "$ROOT/switch-client/icon.jpg" ] && cp "$ROOT/switch-client/icon.jpg" "$TMP/switch-client/"
MSYSTEM=MSYS "$MSYS/usr/bin/bash.exe" -lc "export DEVKITPRO=/opt/devkitpro; cd '$TMP/switch-client' && make" </dev/null
cp "$TMP/switch-client/eglantine-bridge.nro" "$ROOT/switch-client/"
echo "OK: switch-client/eglantine-bridge.nro"
