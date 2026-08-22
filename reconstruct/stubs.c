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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"

static int32_t trace = -1;

static void note(const char *name, uint32_t off)
{
    if (trace < 0)
        trace = getenv("POPCORN_TRACE_STUBS") != NULL;
    if (!trace)
        return;
    /* Once each: a stub inside a frame loop would otherwise flood. */
    static const char *seen[64];
    static int32_t n;
    for (int32_t i = 0; i < n; i++)
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
    void name(uint32_t bx) { note(#name, off); (void)bx; }

/* --- the menu's live decoration ------------------------------------- */

/* --- the screens behind the function keys ---------------------------- */

/* --- getting into a game -------------------------------------------- */

/* --- called from the play loop, which is transcribed ------------------ */



/* --- called from play_session, which is transcribed ------------------- */



/* An entity whose handler has not been transcribed. Doing nothing leaves it in
 * the list for ever, so it is unlinked - the animation is lost but the list
 * does not fill up and stall the game. */
void entity_unknown(uint32_t bx)
{
    note("entity_unknown", 0x1b5e);
    (void)bx;
    g_image[ENTITY_REMOVE] = 1;
}



/* The player-name boxes return 0xff for "abort", anything else to go on. With
 * no implementation there is nobody to abort, so it reports a start. */

