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
#     sh verify_routes.sh /tmp/popcorn-snaps summary.md
#
# Every state here is reached by a **rule**, not by a lucky frame: `--at`
# stops the first time execution reaches a routine, and `--poke` fast-forwards
# rather than fakes - clearing 0x2f10 is the brick count reaching zero, which
# is what the play loop watches for, so the game runs its own level-done path
# from there. An earlier version of this set was half hand-caught frame
# numbers, which meant the coverage figure could not be reproduced on another
# machine, or on this one after the scratchpad was cleared.
#
# It takes something like half an hour: the bot has to play, and four of these
# wait for a state that arrives when it arrives.
set -e
D="${1:?usage: make_snapshots.sh DIR}"
PY="uv run"
mkdir -p "$D"

# The menu, sitting in its own animation loop.
$PY snapshot.py "$D/menu.snap" --seconds 25

# A level, played into rather than started - the paddle is out, the ball is
# moving and entities are live. Everything below resumes from this.
$PY snapshot.py "$D/level.snap" --keys @0206:f1 --seconds 45 --bot

# Levels 3, 10 and 49 from their own first frame. Poking the level *before*
# and clearing it makes play_session load the wanted one the way it normally
# would; 0x1c3f is the level intro, so the snapshot lands at a level start.
#
# Level 3 is here for the four-hit brick: its last hit leaves the spinning 100
# at 1ac2:366f, whose counter at [bx+2] is a **byte** and whose [bx+3] holds
# whatever the recycled entity slot left behind. Reading that pair as a word
# means the counter never reaches zero and the sprite is never taken off the
# screen - which is a divergence no other route here reaches.
for lv in 3 10 49; do
    prev=$((lv - 1)); off=$((0xc + prev * 176))
    $PY snapshot.py "$D/level$lv.snap" --resume "$D/level.snap" \
        --at 0x1c3f --seconds 30 --poke 0x13cc=$prev \
        --poke 0x13ca=$((off & 255)) --poke 0x13cb=$((off >> 8)) \
        --poke 0x2f10=0
done

# The last level cleared, so the next frame is the ending.
$PY snapshot.py "$D/ending.snap" --resume "$D/level49.snap" \
    --at 0x1c3f --seconds 30 --poke 0x2f10=0

# A capsule falling. 0x3273 is entity_capsule, so this stops the first frame
# one exists rather than at a frame number someone watched for.
$PY snapshot.py "$D/capsule.snap" --resume "$D/level.snap" \
    --at 0x3273 --seconds 400 --bot

# The extended paddle: 0x538d is tall_sprite, which only the wide paddle draws.
$PY snapshot.py "$D/tall.snap" --resume "$D/capsule.snap" \
    --at 0x538d --seconds 900 --bot

# The ending's particle fountain, which is a screen of its own.
$PY snapshot.py "$D/particles.snap" --resume "$D/ending.snap" \
    --at 0x5a43 --seconds 400

# The field marks - 0x0598 draws them, and nothing else calls it.
$PY snapshot.py "$D/marks.snap" --resume "$D/level.snap" \
    --at 0x0598 --seconds 400 --bot

# The extra-life capsule, which is 7 chances in 255 and which no run had ever
# reached. 0x33b1 is the odds table - eleven cumulative weights out of 255 -
# and zeroing everything below V's entry makes bonus_kind return V every time.
# The poke is in the snapshot, so *both* sides see the rigged table and the
# comparison is as honest as any other; the rare path is just guaranteed.
#
# --poke lands when the snapshot is *written*, not while it is captured, so it
# seeds the run that resumes from here rather than steering this one. That is
# why this stops at the level intro: the next capsule the resumed run drops is
# the one that was rigged.
$PY snapshot.py "$D/vlife.snap" --resume "$D/level.snap" \
    --at 0x1c3f --seconds 30 \
    --poke 0x33b1=0 --poke 0x33b2=0 --poke 0x33b3=0 --poke 0x33b4=0 \
    --poke 0x33b5=0 --poke 0x33b6=0 --poke 0x33b7=0 --poke 0x33b8=0xff

# The end-of-level bonus. Reaching it needs a `+` capsule, which is 2 chances
# in 255 and which the bot will not wait for, so the odds table is rigged the
# same way as the extra life above - zero everything below the + and let it be
# certain. Then play on until 1ac2:4210, which is the bonus's own body, so the
# capture is *inside* it rather than on the way.
#
# Two stages because --poke seeds the written file rather than the run that
# writes it: the first rigs the table, the second plays with it rigged.
$PY snapshot.py "$D/plusrig.snap" --resume "$D/level.snap" --seconds 4 --bot \
    --poke 0x33b1=0 --poke 0x33b2=0 --poke 0x33b3=0 --poke 0x33b4=0 \
    --poke 0x33b5=0 --poke 0x33b6=0 --poke 0x33b7=0 --poke 0x33b8=0 \
    --poke 0x33b9=0xff
$PY snapshot.py "$D/bonus.snap" --resume "$D/plusrig.snap" \
    --at 0x4210 --seconds 400 --bot

# Two players, the second out of lives, so the next frame runs the results
# and the hall-of-fame walk with two entries to order rather than one. The
# player table is 0x11b bytes a player at 0x344f: copying player 1's record
# gives player 2 one the game made, and only the name, the score and the lives
# are then changed.
#
# Both counters matter and only one is obvious: 0x3f08 is how many players
# were entered, 0x3f09 how many are still in. next_player hands over while
# 0x3f09 is more than one and runs the hall of fame only when it reaches the
# last, so this needs 2 and 1. With both, score_before runs - and it has no
# other way in, since it exists to order two players' scores.
$PY snapshot.py "$D/twoplayer.snap" --resume "$D/level10.snap" --seconds 2 \
    --copy 0x356a=0x344f:0x11b \
    --poke-str "0x356a=     AL     " --poke-str 0x357a=001000 \
    --poke 0x3576=0 --poke 0x3f08=2 --poke 0x3f09=1

# A field with holes in it. Cell 0x0c is not a brick - it is what brick 11
# leaves behind - so nothing reaches the three routines that draw it unless a
# level containing one is played. Poking 0x0c into the *table* rather than the
# live copy at 0x2f18 is what makes it survive: play_session refreshes the copy
# from the table every level, so a poke into the copy is gone by the intro.
# Levels 9 to 12 are done because which one loads next depends on the seed.
#
# This reaches cell_special, cell_hole_draw, panel_reveal, panel_finish and
# field_marks_wide - five routines no route had ever run.
HOLES=""
for lv in 9 10 11 12; do
    b=$((0xc46c + lv * 176 + 8))
    for i in 0 1 2 3 12 13 24 25 36 37 48 49; do
        HOLES="$HOLES --poke $((b + i))=0x0c"
    done
done
# shellcheck disable=SC2086
$PY snapshot.py "$D/cleared.snap" --resume "$D/level10.snap" --seconds 3 \
    $HOLES --poke 0x2f10=0
# ...and then on to the between-level screen itself, 1ac2:05f8, which is the
# only thing that draws a hole. From the clear it is minutes of emulated time
# away - further than the sweep gives a route, so the union called it unreached
# while a longer run found twenty calls of it agreeing.
$PY snapshot.py "$D/holes.snap" --resume "$D/cleared.snap" --at 0x05f8 \
    --seconds 400

# A life about to be lost: no balls left in play.
$PY snapshot.py "$D/lastball.snap" --resume "$D/level.snap" \
    --seconds 2 --poke 0x2e73=0

ls -l "$D"
