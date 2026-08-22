/*
 * Routines the port has not transcribed yet.
 *
 * Each is here so the game can run around it, and each says what it is and
 * where to read it. They are **not** substitutes: a stub does nothing, and the
 * screen or the behaviour it was responsible for is simply absent. STATUS.md
 * keeps the running count, and this file shrinks as they land.
 *
 * A stub is never silently correct - `POPCORN_TRACE_STUBS=1` in the
 * environment prints each one the first time it is reached, so "that screen is
 * blank" and "that routine is missing" are the same observation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

static int trace = -1;

static void note(const char *name, unsigned off)
{
    if (trace < 0)
        trace = getenv("POPCORN_TRACE_STUBS") != NULL;
    if (!trace)
        return;
    /* Once each: a stub inside a frame loop would otherwise flood. */
    static const char *seen[64];
    static int n;
    for (int i = 0; i < n; i++)
        if (seen[i] == name)
            return;
    if (n < 64)
        seen[n++] = name;
    fprintf(stderr, "popcorn: stub %s (1ac2:%04x)\n", name, off);
}

#define STUB(name, off, what) \
    void name(void) { note(#name, off); }

/* The same, for a handler that takes its node. */
#define STUB2(name, off) \
    void name(unsigned bx) { note(#name, off); (void)bx; }

/* --- the menu's live decoration ------------------------------------- */

/* --- the screens behind the function keys ---------------------------- */
STUB(screen_define_keys,  0x1581, "F5: read three scan codes into 0x2d4f")
STUB(screen_high_scores,  0x4e1a, "F6: the hall of fame")
STUB(employee_enter,      0x4ae0, "F10, the `touche speciale pour employes`")

/* --- getting into a game -------------------------------------------- */
STUB(level_load_file,     0x08c8, "reads a .PPC level set named on the command line")

/* --- called from the play loop, which is transcribed ------------------ */
STUB(demo_input_step,     0x1a6f, "advances the recorded-input cursor")



/* --- called from play_session, which is transcribed ------------------- */
void field_marks_wide(unsigned di) { note("field_marks_wide", 0x0a1d); (void)di; }
STUB(screen_game_over,      0x0473, "GAME OVER")
STUB(screen_end_of_game,    0x0d2e, "the hall of fame, and back to the menu")
STUB(screen_all_levels_done, 0x5940, "finishing all fifty")



/* An entity whose handler has not been transcribed. Doing nothing leaves it in
 * the list for ever, so it is unlinked - the animation is lost but the list
 * does not fill up and stall the game. */
void entity_unknown(unsigned bx)
{
    note("entity_unknown", 0x1b5e);
    (void)bx;
    g_image[ENTITY_REMOVE] = 1;
}

STUB(bonus_end_level,     0x2da0, "kind 8: throws the stack away into 0x4210")

int bonus_script(unsigned bx, unsigned *px, unsigned *py)
{
    note("bonus_script", 0x3c35);
    (void)bx; (void)px; (void)py;
    return 1;
}

/* The player-name boxes return 0xff for "abort", anything else to go on. With
 * no implementation there is nobody to abort, so it reports a start. */

