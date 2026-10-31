#!/bin/sh
# Build PT8.exe with a mingw-w64 toolchain.
#  - Windows: MSYS2 "MINGW64" shell (pacman -S mingw-w64-x86_64-gcc), then: sh build.sh
#  - Linux/macOS: with a mingw-w64 cross compiler installed, sh build.sh
set -e
cd "$(dirname "$0")"

if [ -z "$CC" ]; then
    for c in x86_64-w64-mingw32-gcc x86_64-w64-mingw32-clang gcc; do
        if command -v "$c" >/dev/null 2>&1; then CC=$c; break; fi
    done
fi
if [ -z "$WINDRES" ]; then
    for w in x86_64-w64-mingw32-windres windres; do
        if command -v "$w" >/dev/null 2>&1; then WINDRES=$w; break; fi
    done
fi
[ -n "$CC" ] || { echo "no mingw-w64 C compiler found"; exit 1; }
[ -n "$WINDRES" ] || { echo "no windres found"; exit 1; }

OUT=${OUT:-build}
mkdir -p "$OUT"
"$WINDRES" -I res res/pt8.rc -O coff -o "$OUT/pt8_res.o"
"$CC" -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -mwindows -DUNICODE -D_UNICODE \
    src/main_win32.c src/app.c src/song.c src/replayer.c "$OUT/pt8_res.o" \
    -o "$OUT/PT8.exe" -static -lwinmm -lcomdlg32 -lshell32 -lgdi32 -luser32 -lkernel32 -lm -s
echo "built $OUT/PT8.exe"
