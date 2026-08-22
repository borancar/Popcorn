#!/bin/sh
# Capture the standard set of states for verify_all.py.
#
# No single play route reaches everything: a game started with the mouse never
# runs the keyboard input path, the menu's border animation never runs from a
# level, and the game's ending is fifty levels away.  Each of these is a state
# the game genuinely enters, captured where reaching it by playing would take
# from minutes to never.
#
#     sh make_snapshots.sh /tmp/popcorn-snaps
#     venv/bin/python verify_all.py --dir /tmp/popcorn-snaps
#
# --poke fast-forwards rather than fakes: clearing 0x2f10 is the brick count
# reaching zero, which is what the play loop watches for, and the game runs its
# own level-done path from there.
set -e
D="${1:?usage: make_snapshots.sh DIR}"
PY=venv/bin/python
mkdir -p "$D"

# The menu, sitting in its own animation loop.
$PY snapshot.py "$D/menu.snap" --seconds 25

# A level, played into rather than started - the paddle is out, the ball is
# moving and entities are live.
$PY snapshot.py "$D/level.snap" --keys @0206:f1 --seconds 45 --bot

# The same level with the brick count at zero, so the next frame runs the
# level-done path: the tally, the between-level screen and the next intro.
$PY snapshot.py "$D/cleared.snap" --resume "$D/level.snap" \
    --seconds 2 --poke 0x2f10=0

# The last level, so the ending is one clear away.
$PY snapshot.py "$D/ending.snap" --resume "$D/level.snap" \
    --seconds 2 --poke 0x13cc=0x31 --poke 0x2f10=0

# A life about to be lost: no balls left in play.
$PY snapshot.py "$D/lastball.snap" --resume "$D/level.snap" \
    --seconds 2 --poke 0x2e73=0

ls -l "$D"
