#!/bin/sh
# The standard verification sweep: every route, unioned, with the summary
# written where STATUS.md can carry a number that was measured.
#
#     sh make_snapshots.sh /tmp/popcorn-snaps
#     sh verify_routes.sh /tmp/popcorn-snaps /tmp/summary.md
#
# This is the command behind the coverage table in STATUS.md. It used to live
# only in a shell history, which meant the table could be read but not
# re-derived; a figure nobody can reproduce is a claim, not a measurement.
#
# The three base routes play, watch the demo, and play on the keyboard. The
# rest resume from a captured state, because the states that catch the
# remaining routines - the ending, a falling capsule, the extended paddle - are
# minutes to hours of play away, and two of them the bot cannot reach at all.
#
# --chase is worth about twenty routines on its own: a routine whose caller is
# being sampled is never sampled itself, so the first pass reports as unchecked
# a great many it in fact ran straight past.
set -e
D="${1:?usage: verify_routes.sh SNAPDIR [SUMMARY.md]}"
OUT="${2:-/dev/stdout}"

# The cheat, typed at the menu: it unlocks the level-select the demo uses.
CHEAT='@0206:^l,@0206:^a,@0206:^c,@0206:^r,@0206:^a,@0206:^l,@0206:space'
CHEAT="$CHEAT,@0206:s,@0206:o,@0206:f,@0206:t,@0206:w,@0206:a,@0206:r,@0206:e"
CHEAT="$CHEAT,@0206:return"

# Fail now rather than forty minutes in. A route whose file is missing runs
# anyway and reports every routine as unreached, which reads as a coverage
# result rather than as a typo.
for f in menu level level3 level10 level49 ending capsule tall particles marks \
         vlife twoplayer cleared holes lastball bonus; do
    [ -f "$D/$f.snap" ] || {
        echo "missing $D/$f.snap - run make_snapshots.sh $D first" >&2
        exit 1
    }
done

uv run verify_all.py --chase --summary "$OUT" \
    --snapshot "$D/level.snap" \
    --snapshot "$D/menu.snap=@0206:f8" \
    --snapshot "$D/menu.snap=@0206:f6" \
    --snapshot "$D/menu.snap=@0206:f2" \
    --snapshot "$D/menu.snap=$CHEAT" \
    --snapshot "$D/menu.snap=@0206:f2,@1785:p,@1785:o,@1785:p" \
    --snapshot "$D/level3.snap" \
    --snapshot "$D/bonus.snap" \
    --snapshot "$D/level10.snap=@1a62:escape,@1a62:space" \
    --snapshot "$D/level10.snap" \
    --snapshot "$D/level49.snap" \
    --snapshot "$D/capsule.snap" \
    --snapshot "$D/tall.snap" \
    --snapshot "$D/marks.snap" \
    --snapshot "$D/vlife.snap" \
    --snapshot "$D/lastball.snap" \
    --snapshot "$D/ending.snap" \
    --snapshot "$D/particles.snap" \
    --snapshot "$D/twoplayer.snap" \
    --snapshot "$D/cleared.snap" \
    --snapshot "$D/holes.snap"
