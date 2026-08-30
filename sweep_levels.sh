#!/bin/sh
# Compare the port and the emulator on any level, without playing to it.
#
# The bot reaches the deep levels now, but playing there takes hours of
# wall clock - and most of the game had never been compared at all because of
# it. This pokes the level *before* the one wanted and clears it, so
# play_session loads the wanted one the way it normally would, then runs the
# frame-by-frame comparison from that level's own start.
#
#     sh sweep_levels.sh SEED.snap OUTDIR 15 21 27 32 37 42 47
#
# LEVEL is the **panel number**, the one the header bar shows. [0x13cc] is
# 0-based, so panel level P is level_number P-1, and to land on P we poke P-2
# and clear its brick count. Panel level 1 has no level before it and so
# cannot be reached this way.
#
# SEED.snap is any snapshot taken mid-play at a frame close - the ones
# sidebyside.py --snapshots writes will do.
set -e
SEED="${1:?usage: sweep_levels.sh SEED.snap OUTDIR LEVEL...}"
OUT="${2:?}"
shift 2
PY="uv run"
mkdir -p "$OUT"
for lv in "$@"; do
    prev=$((lv - 2))
    off=$((0xc + prev * 176))              # [0x13ca], the level's offset
    snap="$OUT/$(printf L%02d "$lv").snap"
    $PY snapshot.py "$snap" --resume "$SEED" --at 0x1c3f --seconds 20 \
        --poke 0x13cc=$prev --poke 0x13ca=$((off & 255)) \
        --poke 0x13cb=$((off >> 8)) --poke 0x2f10=0 >/dev/null 2>&1
    printf "level %-3s " "$lv"
    $PY sidebyside.py --resume "$snap" --frames "${FRAMES:-4000}" \
        --from-session --no-sound 2>&1 |
        grep -E "frames differ|identical throughout|frame [0-9]+, level" |
        head -1
done
