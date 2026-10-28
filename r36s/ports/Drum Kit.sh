#!/bin/bash
# ours-blanc Drum Kit launcher for R36S Ports (RetroArch core, no content needed).
# Log: /roms/ports/drumkit/log.txt
DIR="/roms/ports/drumkit"
[ -d "$DIR" ] || DIR="$(cd "$(dirname "$0")" && pwd)/drumkit"
LOG="$DIR/log.txt"
RA=/usr/local/bin/retroarch
{
  echo "=== $(date) ==="; id; uname -a
  echo "RA: $(ls -l $RA 2>&1)"; file -L "$RA" "$DIR/drumkit_libretro.so" 2>&1
} > "$LOG" 2>&1
echo "--- retroarch -v -L $DIR/drumkit_libretro.so ---" >> "$LOG"
"$RA" -v -L "$DIR/drumkit_libretro.so" >> "$LOG" 2>&1
echo "--- exit code $? ---" >> "$LOG"
sync
